// Channels of flowing water on the CPU (docs/subsystems/water.md, "Channels"; roadmap S96, S106):
// a solved description read, bounded and refused; the sheet over the flow fixture's three channels
// (level across the line, between the wet edges and a margin under the bank, the phase the travel
// time at the solved speed, the hydraulic depth the solved Froude number's, the fall a parabola of
// negative UV.y, the jump a ramp at its head's state); no two triangles overlapping in plan, on
// the fixture and round a hairpin; nothing drawn between two stations that stand over a bank; the
// depth map the level less the ground; no boundary of the sheet above the ground; a channel's
// water at a point; the files the same bytes twice and pinned; the generator through the
// registry. No device.
#include <core/hash/hash.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/assets/gltf.h>
#include <domain/gfx/visibility_resolve.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/scene_gen/water_channel.h>
#include <foundation/io/vfs.h>
#include <systems/water/channel.h>
#include <systems/water/water.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <string>

#include "channel_fixture.h"

using namespace engine;

namespace {

struct Built {
  water::Channel channel;
  water::ChannelMesh sheet;
};

Built build(const scene::WaterChannel& block) {
  Built b;
  std::string error;
  REQUIRE_MESSAGE(water::read_channel(block, b.channel, &error), error);
  water::build_channel_mesh(b.channel, b.sheet);
  return b;
}

// How many triangles hold each of a grid of points in plan, `step` apart (off the lattice's own
// lines): the most any point is held by, and how many are held at all.
struct Cover {
  u32 most = 0;
  u32 covered = 0;
  f32 step = 0.0f;
  f64 area() const { return static_cast<f64>(covered) * static_cast<f64>(step) * step; }
};

Cover plan_cover(const water::Mesh& m, f32 step) {
  Cover cover;
  cover.step = step;
  f32 lo_x = 1e30f, lo_z = 1e30f, hi_x = -1e30f, hi_z = -1e30f;
  for (const Vec3& p : m.positions) {
    lo_x = std::min(lo_x, p.x);
    hi_x = std::max(hi_x, p.x);
    lo_z = std::min(lo_z, p.z);
    hi_z = std::max(hi_z, p.z);
  }
  if (m.positions.empty()) return cover;
  const f32 ox = lo_x - 0.5f + 0.037f * step, oz = lo_z - 0.5f + 0.053f * step;
  const u32 nx = static_cast<u32>((hi_x - lo_x + 1.0f) / step) + 2;
  const u32 nz = static_cast<u32>((hi_z - lo_z + 1.0f) / step) + 2;
  Vector<u8> count(nx * nz);
  std::fill(count.begin(), count.end(), u8{0});
  for (u32 t = 0; t + 2 < m.indices.size(); t += 3) {
    const Vec3 a = m.positions[m.indices[t]];
    const Vec3 b = m.positions[m.indices[t + 1]];
    const Vec3 c = m.positions[m.indices[t + 2]];
    const f32 x0 = std::min(a.x, std::min(b.x, c.x)), x1 = std::max(a.x, std::max(b.x, c.x));
    const f32 z0 = std::min(a.z, std::min(b.z, c.z)), z1 = std::max(a.z, std::max(b.z, c.z));
    const u32 i0 = static_cast<u32>(std::max(0.0f, std::floor((x0 - ox) / step)));
    const u32 i1 = std::min(nx - 1, static_cast<u32>(std::max(0.0f, std::ceil((x1 - ox) / step))));
    const u32 j0 = static_cast<u32>(std::max(0.0f, std::floor((z0 - oz) / step)));
    const u32 j1 = std::min(nz - 1, static_cast<u32>(std::max(0.0f, std::ceil((z1 - oz) / step))));
    const f64 area = static_cast<f64>(b.x - a.x) * (c.z - a.z) - static_cast<f64>(c.x - a.x) * (b.z - a.z);
    if (std::abs(area) < 1e-12) continue;
    for (u32 j = j0; j <= j1; ++j) {
      for (u32 i = i0; i <= i1; ++i) {
        const f64 x = static_cast<f64>(ox) + static_cast<f64>(i) * step;
        const f64 z = static_cast<f64>(oz) + static_cast<f64>(j) * step;
        const f64 w0 = ((b.x - x) * (c.z - z) - (c.x - x) * (b.z - z)) / area;
        const f64 w1 = ((c.x - x) * (a.z - z) - (a.x - x) * (c.z - z)) / area;
        const f64 w2 = 1.0 - w0 - w1;
        if (w0 > 1e-9 && w1 > 1e-9 && w2 > 1e-9) ++count[j * nx + i];
      }
    }
  }
  for (const u8 n : count) {
    cover.most = std::max<u32>(cover.most, n);
    cover.covered += n > 0 ? 1u : 0u;
  }
  return cover;
}

// The map's depth at (x, z), metres from the channel's origin, read bilinearly as the resolve does.
f32 map_depth(const water::DepthMap& map, f32 x, f32 z) {
  const f32 fi = (x - map.origin.x) / map.texel_m;
  const f32 fj = (z - map.origin.y) / map.texel_m;
  const u32 i = static_cast<u32>(std::floor(fi));
  const u32 j = static_cast<u32>(std::floor(fj));
  REQUIRE(i + 1 < map.size);
  REQUIRE(j + 1 < map.size);
  const f32 u = fi - static_cast<f32>(i), w = fj - static_cast<f32>(j);
  const f32 a = water::depth_map_depth(map, i, j), b = water::depth_map_depth(map, i + 1, j);
  const f32 c = water::depth_map_depth(map, i, j + 1), d = water::depth_map_depth(map, i + 1, j + 1);
  return (a * (1.0f - u) + b * u) * (1.0f - w) + (c * (1.0f - u) + d * u) * w;
}

scene::WaterChannel flagged(scene::WaterChannel c, std::initializer_list<u32> stations, u16 flag) {
  for (const u32 i : stations)
    c.flags[i] = static_cast<u16>(c.flags[i] | flag);
  return c;
}

}  // namespace

TEST_CASE("water channel: a solved description is read and bounded, and the rest refused") {
  water::Channel ch;
  std::string error;
  REQUIRE_MESSAGE(water::read_channel(channel_fixture::step(), ch, &error), error);
  CHECK(ch.points.size() == 12);
  // The origin: the bounding box's centre in x and z taken down to a whole metre, the first
  // station's level.
  CHECK(ch.origin.x == 33.0);
  CHECK(ch.origin.y == 12.499);
  CHECK(ch.origin.z == 0.0);
  CHECK(ch.points[0].x == -25.0f);
  CHECK(ch.manning_n == 0.04f);
  // What a stretch is comes from the flags, never from its slope.
  CHECK(water::stretch_falls(ch, 6));
  CHECK_FALSE(water::stretch_falls(ch, 5));
  CHECK_FALSE(water::stretch_falls(ch, 7));
  CHECK(water::stretch_jumps(ch, 8));
  CHECK_FALSE(water::stretch_jumps(ch, 9));
  for (u32 i = 0; i + 1 < 12; ++i)
    CHECK(water::stretch_drawn(ch, i));
  // The hydraulic depth is the solve's own A / T: at the uniform reach's normal depth of 0.5 m in
  // the fixture's trapezoid, 1.25 m2 over 3 m.
  water::Channel uniform;
  REQUIRE(water::read_channel(channel_fixture::uniform(), uniform, nullptr));
  CHECK(water::station_hydraulic_depth(uniform, 0) == doctest::Approx(1.25 / 3.0).epsilon(2e-4));

  // **The bounds**: a thin film's Froude number of 109 reaches the draw as 4, a speed of 60 m/s
  // as 20 and still water as 5 cm/s, and the hydraulic depth is never under a millimetre.
  scene::WaterChannel film = channel_fixture::uniform();
  film.froude[1] = 109.0f;
  film.speed_mps[2] = 60.0f;
  film.speed_mps[3] = 0.0f;
  film.froude[3] = 0.0f;
  film.speed_mps[4] = 0.06f;
  film.froude[4] = 3.9f;
  REQUIRE_MESSAGE(water::read_channel(film, ch, &error), error);
  CHECK(ch.froude[1] == water::k_channel_froude_max);
  CHECK(ch.speed_mps[2] == water::k_channel_max_speed);
  CHECK(ch.speed_mps[3] == water::k_channel_min_speed);
  CHECK(water::station_hydraulic_depth(ch, 1) ==
        doctest::Approx(1.2794 * 1.2794 / (9.81 * 16.0)).epsilon(1e-5));
  CHECK(water::station_hydraulic_depth(ch, 3) == 0.5f);  // still: the station's greatest depth
  CHECK(water::station_hydraulic_depth(ch, 4) == water::k_channel_depth_min_m);

  const auto refused = [&](scene::WaterChannel c, const char* says) {
    std::string why;
    water::Channel out;
    CHECK_FALSE(water::read_channel(c, out, &why));
    CHECK_MESSAGE(why.find(says) != std::string::npos, why);
    CHECK(why.find("\"step\"") != std::string::npos);
  };
  // A description laid before W120 — a level, a width, a depth and a slope — is refused by name;
  // so is one that names a discharge and no solve.
  scene::WaterChannel laid = channel_fixture::step();
  laid.discharge_m3s.clear();
  laid.speed_mps.clear();
  laid.froude.clear();
  laid.wet_left_m.clear();
  laid.wet_right_m.clear();
  laid.flags.clear();
  refused(laid, "cooked before W120: cook again");
  scene::WaterChannel unsolved = laid;
  unsolved.discharge_m3s = channel_fixture::step().discharge_m3s;
  refused(unsolved, "names a discharge and is not solved");
  scene::WaterChannel one = channel_fixture::step();
  one.points.resize(1);
  refused(one, "fewer than two points");
  scene::WaterChannel unequal = channel_fixture::step();
  unequal.wet_left_m.pop_back();
  refused(unequal, "its wet_left_m has 11 entries for 12 points");
  scene::WaterChannel lipless = channel_fixture::step();
  lipless.flags[7] = 0;
  refused(lipless, "its fall lip at point 6 is not followed by the fall's foot");
  scene::WaterChannel unknown = channel_fixture::step();
  unknown.flags[2] = 0x0100;
  refused(unknown, "hold a bit this build does not know");
  // This capability's own: a station is a place along the line.
  scene::WaterChannel stacked = channel_fixture::step();
  stacked.points[7].x = 38.0;
  refused(stacked, "its points 6 and 7 stand at one place in plan");
  scene::WaterChannel rough = channel_fixture::step();
  rough.manning_n = 0.0f;
  refused(rough, "its manning_n must be positive");
}

TEST_CASE("water channel: Manning's speed, the Froude number and the cube root") {
  // v = (1/n) d^(2/3) S^(1/2): n 0.04, 0.6 m deep, 1 in 100 — 1.7786 m/s, Froude 0.733.
  const f32 v = water::manning_speed(0.04f, 0.6f, 0.01f);
  CHECK(v == doctest::Approx(25.0 * std::pow(0.6, 2.0 / 3.0) * 0.1).epsilon(1e-6));
  CHECK(water::froude(v, 0.6f) ==
        doctest::Approx(static_cast<f64>(v) / std::sqrt(9.81 * 0.6)).epsilon(1e-6));
  CHECK(water::froude(v, 0.6f) < 1.0f);
  CHECK(water::froude(water::manning_speed(0.04f, 0.4f, 0.04f), 0.4f) > 1.0f);
  CHECK(water::manning_speed(0.04f, 1.0f, 0.0f) == 0.0f);
  for (const f64 a : {0.0, 1e-6, 0.36, 1.0, 8.0, 2.25, 1e6})
    CHECK(water::cube_root(a) == doctest::Approx(std::cbrt(a)).epsilon(1e-14));
}

TEST_CASE("water channel: the uniform reach's sheet is level across, between its wet edges") {
  const Built b = build(channel_fixture::uniform());
  const water::Mesh& m = b.sheet.mesh;
  REQUIRE(m.positions.size() == m.uvs.size());
  REQUIRE(m.positions.size() == m.normals.size());
  REQUIRE(m.indices.size() == 3 * b.sheet.triangles);
  REQUIRE(b.sheet.triangles > 0);
  MESSAGE("uniform sheet: lattice ", b.sheet.lattice_m, " m, ", m.positions.size(), " vertices, ",
          b.sheet.triangles, " triangles, ", b.sheet.wrap_vertices, " wrap copies");
  // The lattice: a quarter of the narrowest sheet, 3 m of wet reach and half a metre under each
  // bank.
  CHECK(water::channel_lattice(b.channel) == 1.0f);
  CHECK(b.sheet.fall_triangles == 0);
  const f64 hydraulic = 1.2794 * 1.2794 / (9.81 * 0.6328 * 0.6328);
  const f64 period = static_cast<f64>(water::k_channel_phase_period_s);
  for (u32 k = 0; k < m.positions.size(); ++k) {
    const f64 x = b.channel.origin.x + static_cast<f64>(m.positions[k].x);
    const f64 z = b.channel.origin.z + static_cast<f64>(m.positions[k].z);
    // Between the head and the last station, within the wet reach and the margin under the bank.
    CHECK(x >= 8.0 - 1e-4);
    CHECK(x <= 48.0 + 1e-4);
    CHECK(std::abs(z + 20.0) <= 1.5 + static_cast<f64>(water::k_channel_under_bank_m) + 1e-4);
    // **The level across a row**: the level of the vertex's own place along the line, whatever
    // its distance from it.
    const f64 level = 10.5 - 0.01 * (x - 8.0);
    CHECK(b.channel.origin.y + static_cast<f64>(m.positions[k].y) ==
          doctest::Approx(level).epsilon(1e-6));
    // The phase: the travel time at the solved speed, less whole periods.
    const f64 tau = (x - 8.0) / 1.2794;
    const f64 wraps = (tau - static_cast<f64>(m.uvs[k].x)) / period;
    CHECK(std::abs(wraps - std::round(wraps)) < 1e-4);
    CHECK(m.uvs[k].x >= -1e-4f);
    CHECK(m.uvs[k].x < water::k_channel_phase_period_s + 1.0f / 1.2794f + 1e-3f);
    // The hydraulic depth: the solved A / T.
    CHECK(static_cast<f64>(m.uvs[k].y) == doctest::Approx(hydraulic).epsilon(1e-5));
    CHECK(m.normals[k].y > 0.9999f);
  }
  // 31 s of travel pass three periods: vertices on a wrap are written once a side.
  CHECK(b.sheet.wrap_vertices > 0);
  // Every triangle faces up, and its three phases lie within its own travel time of each other.
  for (u32 t = 0; t < m.indices.size(); t += 3) {
    const Vec3 a = m.positions[m.indices[t]];
    const Vec3 bb = m.positions[m.indices[t + 1]];
    const Vec3 c = m.positions[m.indices[t + 2]];
    CHECK(cross(bb - a, c - a).y > 0.0f);
    const f32 lo = std::min(m.uvs[m.indices[t]].x,
                            std::min(m.uvs[m.indices[t + 1]].x, m.uvs[m.indices[t + 2]].x));
    const f32 hi = std::max(m.uvs[m.indices[t]].x,
                            std::max(m.uvs[m.indices[t + 1]].x, m.uvs[m.indices[t + 2]].x));
    CHECK(hi - lo <= 1.0f / 1.2794f + 1e-3f);
  }
  // **Plan overlap 0**, and the sheet is the region it should be: 40 m by 4 m, less the four
  // corners the clip takes off inside one cell each (at most half a cell).
  const Cover cover = plan_cover(m, 0.05f);
  CHECK(cover.most == 1);
  CHECK(cover.area() <= 160.0 + 0.5);
  CHECK(cover.area() >= 160.0 - 4.0 * 0.5 - 0.5);
}

TEST_CASE("water channel: the step's fall is a parabola and its jump a ramp at the head's state") {
  const Built b = build(channel_fixture::step());
  const water::Mesh& m = b.sheet.mesh;
  MESSAGE("step sheet: lattice ", b.sheet.lattice_m, " m, ", m.positions.size(), " vertices, ",
          b.sheet.triangles, " triangles of which ", b.sheet.fall_triangles, " on the fall");
  CHECK(b.sheet.fall_triangles > 0);
  CHECK(b.sheet.fall_triangles < b.sheet.triangles / 4);
  const f64 drop = 12.077 - 9.837;
  const f64 fall_time = std::sqrt(2.0 * drop / 9.81);
  const f64 head_depth = 4.0748 * 4.0748 / (9.81 * 3.1902 * 3.1902);
  u32 on_fall = 0, on_jump = 0, rising = 0;
  for (u32 k = 0; k < m.positions.size(); ++k) {
    const f64 x = b.channel.origin.x + static_cast<f64>(m.positions[k].x);
    const f64 y = b.channel.origin.y + static_cast<f64>(m.positions[k].y);
    if (m.uvs[k].y < 0.0f) {
      // **Between the lip and the foot**: the nappe, a projectile's parabola from the lip's level
      // to the foot's, its thickness the lip's unit discharge over its own speed.
      ++on_fall;
      CHECK(x > 38.0 - 1e-4);
      CHECK(x < 40.0 + 1e-4);
      const f64 t = (x - 38.0) / 2.0;
      CHECK(y == doctest::Approx(12.077 - drop * t * t).epsilon(1e-5));
      const f64 across = 2.0 / fall_time, down = 9.81 * fall_time * t;
      const f64 thickness =
          std::min(1.5993 / (2.0 * 1.377) / std::sqrt(across * across + down * down),
                   1.7861 * 1.7861 / 9.81);
      CHECK(static_cast<f64>(-m.uvs[k].y) == doctest::Approx(thickness).epsilon(1e-3));
      // Facing downstream as it steepens.
      CHECK(m.normals[k].x >= 0.0f);
    } else if (x > 41.246 + 1e-3 && x < 44.201 - 1e-3) {
      // **The jump**: the level a ramp from the head's to the foot's, the head's hydraulic depth
      // (so the head's Froude number, 3.19: white water) held to the foot.
      ++on_jump;
      const f64 t = (x - 41.246) / (44.201 - 41.246);
      CHECK(y == doctest::Approx(9.88 + (10.372 - 9.88) * t).epsilon(1e-5));
      CHECK(static_cast<f64>(m.uvs[k].y) == doctest::Approx(head_depth).epsilon(1e-4));
    } else if (x > 40.0 + 1e-3 && x < 41.246 - 1e-3) {
      // **The level rises along shooting water**, as the solve has it: drawn as solved.
      ++rising;
      const f64 t = (x - 40.0) / 1.246;
      CHECK(y == doctest::Approx(9.837 + (9.88 - 9.837) * t).epsilon(1e-5));
      CHECK(y > 9.837 - 1e-6);
    }
  }
  CHECK(on_fall > 0);
  CHECK(on_jump > 0);
  CHECK(rising > 0);
  CHECK(plan_cover(m, 0.05f).most == 1);
  // The fall's travel time is the free fall's, and the jump's its length at the head's speed.
  const water::ChannelLine line = water::channel_line(b.channel);
  CHECK(line.tau[7] - line.tau[6] == doctest::Approx(fall_time).epsilon(1e-6));
  CHECK(line.tau[9] - line.tau[8] == doctest::Approx((44.201 - 41.246) / 4.0748).epsilon(1e-5));
  // A reach's is its length over the mean slowness of its two stations.
  CHECK(line.tau[1] - line.tau[0] ==
        doctest::Approx(0.5 * 8.0 * (1.0 / 1.2837 + 1.0 / 1.2905)).epsilon(1e-6));
}

TEST_CASE("water channel: nothing is drawn between two stations that stand over a bank") {
  // Stations 3, 4 and 5 of the mouth's eleven over their banks: stretches 3 and 4 (x 32 to 48)
  // are not drawn, and the sheet either side ends square at x 32 and x 48.
  const Built b = build(flagged(channel_fixture::mouth(), {3, 4, 5}, scene_gen::k_channel_over_bank));
  CHECK_FALSE(water::stretch_drawn(b.channel, 3));
  CHECK_FALSE(water::stretch_drawn(b.channel, 4));
  CHECK(water::stretch_drawn(b.channel, 2));
  CHECK(water::stretch_drawn(b.channel, 5));
  const water::Mesh& m = b.sheet.mesh;
  u32 above = 0, below = 0;
  for (u32 t = 0; t < m.indices.size(); t += 3) {
    f64 x = 0.0;
    for (u32 k = 0; k < 3; ++k) {
      const f64 vx = b.channel.origin.x + static_cast<f64>(m.positions[m.indices[t + k]].x);
      CHECK((vx <= 32.0 + 1e-4 || vx >= 48.0 - 1e-4));
      x += vx / 3.0;
    }
    above += x < 32.0 ? 1u : 0u;
    below += x > 48.0 ? 1u : 0u;
  }
  CHECK(above > 0);
  CHECK(below > 0);
  // At a station over its bank the sheet has no margin: the wet reach ends at the bank's crest.
  for (u32 k = 0; k < m.positions.size(); ++k) {
    const f64 x = b.channel.origin.x + static_cast<f64>(m.positions[k].x);
    const f64 z = b.channel.origin.z + static_cast<f64>(m.positions[k].z);
    if (std::abs(x - 32.0) < 1e-3) CHECK(std::abs(z - 20.0) <= 1.686 + 1e-3);
  }
  // A line that stands over its bank at every station (W120's film along a line that climbs out
  // of a hollow) draws nothing at all.
  const Built film = build(flagged(channel_fixture::mouth(), {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10},
                                   scene_gen::k_channel_over_bank));
  CHECK(film.sheet.triangles == 0);
  CHECK(film.sheet.mesh.positions.empty());
}

TEST_CASE("water channel: a hairpin tighter than the wet reach does not fold") {
  // A line that turns back on itself 2 m away with 3 m of wet reach either side: rows across the
  // line would cross on the inside of the bend. The lattice's cells cannot.
  scene::WaterChannel c = channel_fixture::solved("hairpin", 0.0,
                                                  {{0.0, 5.0, 0.4f, 1.0f, 0.5f, 3.0f, 0},
                                                   {20.0, 4.9, 0.4f, 1.0f, 0.5f, 3.0f, 0},
                                                   {21.0, 4.89, 0.4f, 1.0f, 0.5f, 3.0f, 0},
                                                   {20.0, 4.88, 0.4f, 1.0f, 0.5f, 3.0f, 0},
                                                   {0.0, 4.78, 0.4f, 1.0f, 0.5f, 3.0f, 0}});
  c.points[2].z = 1.0;
  c.points[3].z = 2.0;
  c.points[4].z = 2.0;
  const Built b = build(c);
  REQUIRE(b.sheet.triangles > 0);
  const Cover cover = plan_cover(b.sheet.mesh, 0.05f);
  CHECK(cover.most == 1);
  // The triangles' areas in plan add up to the area they cover: nothing is covered twice.
  f64 sum = 0.0;
  const water::Mesh& m = b.sheet.mesh;
  for (u32 t = 0; t < m.indices.size(); t += 3) {
    const Vec3 a = m.positions[m.indices[t]];
    const Vec3 bb = m.positions[m.indices[t + 1]];
    const Vec3 cc = m.positions[m.indices[t + 2]];
    sum += 0.5 * std::abs(static_cast<f64>(bb.x - a.x) * (cc.z - a.z) -
                          static_cast<f64>(cc.x - a.x) * (bb.z - a.z));
  }
  CHECK(cover.area() == doctest::Approx(sum).epsilon(0.02));
}

TEST_CASE("water channel: the depth map is the level less the ground") {
  const scene_gen::Ground ground = channel_fixture::ground();
  {
    const Built b = build(channel_fixture::uniform());
    water::DepthMap map;
    water::build_channel_depth_map(b.channel, b.sheet, ground, map);
    REQUIRE(map.size >= water::k_depth_map_min);
    REQUIRE(map.size <= water::k_depth_map_max);
    CHECK(map.texel_m == water::k_channel_map_texel_m);
    CHECK(map.bank_max == water::k_depth_map_bank_min);
    CHECK(map.depth_max == doctest::Approx(0.5f).epsilon(1e-4));
    // A texel is a multiple of its size from the world's origin.
    const f64 world_x = (b.channel.origin.x + static_cast<f64>(map.origin.x)) / 0.25;
    CHECK(world_x == std::round(world_x));
    MESSAGE("uniform depth map: ", map.size, " texels a side at ", map.texel_m, " m");
    // Under the wet reach and on the bank inside the margin, at places off the texels: the level
    // less the ground to a millimetre (the ground is linear between the texels it is sampled at).
    for (const f64 x : {10.3, 20.7, 33.1, 45.9}) {
      for (const f64 dz : {0.0, 0.4, -0.9, 1.3, -1.45, 1.8, -1.9}) {
        const f64 want = (10.5 - 0.01 * (x - 8.0)) - channel_fixture::ground_m(x, -20.0 + dz);
        const f32 got = map_depth(map, static_cast<f32>(x - b.channel.origin.x),
                                  static_cast<f32>(-20.0 + dz - b.channel.origin.z));
        CHECK(static_cast<f64>(got) == doctest::Approx(want).scale(1.0).epsilon(1e-3));
        CHECK((dz > -1.5 && dz < 1.5) == (got > 0.0f));
      }
      // Past the margin and a texel: bank, as high as the map stores.
      CHECK(map_depth(map, static_cast<f32>(x - b.channel.origin.x),
                      static_cast<f32>(-20.0 + 4.0 - b.channel.origin.z)) == -map.bank_max);
    }
  }
  {
    // On the fall the surface is the nappe: at its middle, x = 39 (a texel), a quarter of the
    // drop below the lip over the apron at 9.7.
    const Built b = build(channel_fixture::step());
    water::DepthMap map;
    water::build_channel_depth_map(b.channel, b.sheet, ground, map);
    const f64 want = 12.077 - 0.25 * (12.077 - 9.837) - 9.7;
    CHECK(static_cast<f64>(map_depth(map, static_cast<f32>(39.0 - b.channel.origin.x), 0.0f)) ==
          doctest::Approx(want).epsilon(1e-3));
    // Below the jump, the backwater's 0.636 m at x = 52.
    CHECK(static_cast<f64>(map_depth(map, static_cast<f32>(52.0 - b.channel.origin.x), 0.0f)) ==
          doctest::Approx(10.336 - 9.7).epsilon(1e-3));
  }
  {
    // A stretch that is not drawn holds no water in the map either.
    const Built b =
        build(flagged(channel_fixture::mouth(), {3, 4, 5}, scene_gen::k_channel_over_bank));
    water::DepthMap map;
    water::build_channel_depth_map(b.channel, b.sheet, ground, map);
    // (Zero, to the half step of 16 bits over the map's 3.2 m.)
    CHECK(map_depth(map, static_cast<f32>(40.0 - b.channel.origin.x), 0.0f) <= 2.5e-5f);
    CHECK(map_depth(map, static_cast<f32>(60.0 - b.channel.origin.x), 0.0f) > 0.8f);
  }
  {
    // No ground at all (a scene with no terrain): the bed is level at the solved depth across the
    // wet reach.
    const Built b = build(channel_fixture::uniform());
    water::DepthMap map;
    water::build_channel_depth_map(b.channel, b.sheet, scene_gen::Ground{}, map);
    CHECK(map_depth(map, -8.0f, 0.5f) == doctest::Approx(0.5f).epsilon(1e-3));
    CHECK(map_depth(map, -8.0f, 3.0f) == -map.bank_max);
  }
}

TEST_CASE("water channel: no edge of the sheet stands above the ground") {
  const scene_gen::Ground ground = channel_fixture::ground();
  for (const scene::WaterChannel& block :
       {channel_fixture::uniform(), channel_fixture::step(), channel_fixture::mouth()}) {
    const Built b = build(block);
    const water::ChannelEdge edge = water::channel_edge_exposed(b.channel, b.sheet, ground);
    MESSAGE(block.name, ": ", edge.samples, " boundary samples at 5 cm, ", edge.cuts,
            " on a cut or a nappe, ", edge.exposed, " exposed");
    CHECK(edge.samples > 1000);
    CHECK(edge.cuts > 0);
    CHECK(edge.cuts < edge.samples / 4);
    // **Visible mesh-edge samples: none.** The margin carries the sheet under ground that stands
    // above the level, so the edge a pixel shows is where the ground meets the water.
    CHECK(edge.exposed == 0);
  }
  // The count is live: the same sheet over ground that lies below it shows every bank sample.
  scene_gen::Ground flat;
  flat.surface_fn = [](const void*, i64, i64) noexcept { return 9.0; };
  const Built b = build(channel_fixture::mouth());
  const water::ChannelEdge bare = water::channel_edge_exposed(b.channel, b.sheet, flat);
  CHECK(bare.exposed == bare.samples - bare.cuts);
  CHECK(bare.worst_m > 1.3f);
}

TEST_CASE("water channel: a channel's water at a point") {
  const scene_gen::Ground ground = channel_fixture::ground();
  water::Channel ch;
  REQUIRE(water::read_channel(channel_fixture::step(), ch, nullptr));
  const water::ChannelLine line = water::channel_line(ch);
  water::ChannelSample s;
  // On the line 12 m below the head: the level between the first two stations, the bed 0.5 m
  // under it, the surface at 7/6 of the vertical's share of the mean, downstream along +x.
  REQUIRE(water::channel_sample(ch, line, ground, WorldPos{20.0, 0.0, 0.0}, s));
  const f64 level = 12.416 + (12.331 - 12.416) * 0.5;
  CHECK(s.level_m == doctest::Approx(level).epsilon(1e-6));
  CHECK(static_cast<f64>(s.depth_m) == doctest::Approx(level - 11.88).epsilon(1e-5));
  CHECK_FALSE(s.fall);
  CHECK(s.froude == doctest::Approx(0.6465f).epsilon(2e-3));
  const f64 mean = 1.0 / (0.5 * (1.0 / 1.2905 + 1.0 / 1.3090));
  const f64 hydraulic = mean * mean / (9.81 * static_cast<f64>(s.froude) * s.froude);
  const f64 surface = mean * water::k_channel_surface_factor *
                      water::lateral_speed_share(static_cast<f64>(s.depth_m), hydraulic);
  CHECK(static_cast<f64>(s.velocity.x) == doctest::Approx(surface).epsilon(1e-3));
  CHECK(s.velocity.x > 1.3f);
  CHECK(s.velocity.z == 0.0f);
  CHECK(s.velocity.y < 0.0f);
  // Slower in the shallows by the bank, dry past the wet edge, and nothing past the mouth.
  water::ChannelSample bank;
  REQUIRE(water::channel_sample(ch, line, ground, WorldPos{20.0, 0.0, 1.3}, bank));
  CHECK(bank.depth_m == doctest::Approx(s.depth_m - 0.3f).epsilon(1e-4));
  CHECK(bank.velocity.x < s.velocity.x);
  CHECK_FALSE(water::channel_sample(ch, line, ground, WorldPos{20.0, 0.0, 1.6}, bank));
  CHECK_FALSE(water::channel_sample(ch, line, ground, WorldPos{59.0, 0.0, 0.0}, bank));
  // On the nappe: falling, at the projectile's speed.
  water::ChannelSample fall;
  REQUIRE(water::channel_sample(ch, line, ground, WorldPos{39.0, 0.0, 0.0}, fall));
  CHECK(fall.fall);
  CHECK(fall.velocity.y < -2.0f);
  CHECK(fall.level_m == doctest::Approx(12.077 - 0.25 * 2.24).epsilon(1e-5));
}

TEST_CASE("water channel: the files are the same bytes twice, pinned, and the generator's") {
  const scene_gen::Ground ground = channel_fixture::ground();
  const Built a = build(channel_fixture::step());
  const Built b = build(channel_fixture::step());
  const Vector<u8> glb_a = water::mesh_glb(a.sheet.mesh, Vec3{0.30f, 0.55f, 0.50f}, 0.08f);
  const Vector<u8> glb_b = water::mesh_glb(b.sheet.mesh, Vec3{0.30f, 0.55f, 0.50f}, 0.08f);
  REQUIRE(glb_a.size() == glb_b.size());
  CHECK(std::equal(glb_a.begin(), glb_a.end(), glb_b.begin()));
  water::DepthMap map;
  water::build_channel_depth_map(a.channel, a.sheet, ground, map);
  const Vector<u8> map_bytes = water::depth_map_bytes(map);
  const u64 glb_hash = hash_bytes(glb_a.data(), glb_a.size());
  const u64 map_hash = hash_bytes(map_bytes.data(), map_bytes.size());
  MESSAGE("step glb ", glb_a.size(), " bytes, hash ", glb_hash, "; map ", map_bytes.size(),
          " bytes, hash ", map_hash);
  // Taken on MSVC (2026-11-14, S106): the toolchains reproduce them, since every number is
  // + - * / and sqrt in a fixed order.
  CHECK(glb_hash == 17413192459257434341ull);  // 24,752 bytes
  CHECK(map_hash == 4474027355344884547ull);   // 82,462 bytes
  // The importer reads it as the renderer will: positions, normals and the phase and depth UVs.
  assets::MeshData data;
  std::string error;
  REQUIRE_MESSAGE(
      assets::load_gltf_memory(std::span<const u8>(glb_a.data(), glb_a.size()), "", data, &error),
      error);
  CHECK(data.positions.size() == a.sheet.mesh.positions.size());

  const test::TempDir tmp("water_channel_generator");
  const scene_gen::PlacementGeneratorDesc* generator =
      scene_gen::GeneratorRegistry::global().find_placement(water::k_channel_generator);
  REQUIRE(generator != nullptr);
  // A channel, and one that stands over its bank from end to end: mesh k is channel k either
  // way, and the second's has no path, which is how a reader knows it draws nothing.
  const scene::WaterChannel blocks[2] = {
      channel_fixture::step(), flagged(channel_fixture::mouth(), {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10},
                                       scene_gen::k_channel_over_bank)};
  scene_gen::Context context;
  const std::string dir = tmp.path();
  context.dir = dir;
  context.where = "test";
  context.ground = ground;
  void* state = nullptr;
  REQUIRE_MESSAGE(generator->open(water::channel_generator_params(blocks), context, &state, &error),
                  error);
  scene_gen::Placements placed;
  REQUIRE_MESSAGE(generator->expand(state, context, placed, &error), error);
  generator->close(state);
  REQUIRE(placed.meshes.size() == 2);
  REQUIRE(placed.instances.size() == 2);
  CHECK(placed.meshes[0].name == "water/step");
  CHECK(io::exists(placed.meshes[0].path));
  CHECK(io::exists(placed.meshes[0].path.substr(0, placed.meshes[0].path.size() - 4) + ".depth"));
  CHECK(placed.instances[0].position == a.channel.origin);
  CHECK(placed.meshes[1].path.empty());
  // The generator refuses what `read_channel` refuses, by name.
  scene::WaterChannel laid = channel_fixture::step();
  laid.discharge_m3s.clear();
  laid.speed_mps.clear();
  laid.froude.clear();
  laid.wet_left_m.clear();
  laid.wet_right_m.clear();
  laid.flags.clear();
  CHECK_FALSE(generator->open(
      water::channel_generator_params(std::span<const scene::WaterChannel>(&laid, 1)), context,
      &state, &error));
  CHECK(error.find("cooked before W120: cook again") != std::string::npos);
}

TEST_CASE("water channel: the channel bit is a material flag of its own") {
  gfx::WaterShading shading;
  gfx::ResolveMaterial body;
  gfx::set_water_material(body, shading);
  CHECK(body.flags == gfx::k_material_water);
  CHECK(gfx::k_material_water_channel == (1u << 21));
  // No other material flag shares the bit.
  CHECK((gfx::k_material_water_channel &
         (gfx::k_material_terrain_formation | gfx::k_material_stochastic |
          gfx::k_material_terrain_beach)) == 0u);
}

// ---- the regimes, the profile, the nappe and the plunge records --------------------------------

TEST_CASE("water channel: the surface's regimes are their sources' numbers") {
  // Brocchini and Peregrine 2001: gravity's line q^2 = 2 g L and surface tension's
  // q^2 = 2 sigma / (rho L) cross at the capillary length, 2.7 mm, and q 0.23 m/s.
  const f64 lc = water::capillary_length_m();
  CHECK(lc == doctest::Approx(0.00273).epsilon(0.01));
  const f64 q = std::sqrt(2.0 * water::k_gravity * lc);
  CHECK(q == doctest::Approx(0.2315).epsilon(0.01));
  CHECK(q * q ==
        doctest::Approx(2.0 * water::k_water_surface_tension / (water::k_water_density * lc))
            .epsilon(1e-9));
  // The surface's deformation q^2 / 2g with q the shear velocity: a gentle reach (0.6 m at 1 in
  // 100, 1.78 m/s) is u* = sqrt(g d S) = 0.243 m/s and a few millimetres of surface; a steep one
  // (0.4 m at 1 in 25) 0.396 m/s and about eight millimetres.
  const f64 gentle =
      water::shear_velocity(static_cast<f64>(water::manning_speed(0.04f, 0.6f, 0.01f)), 0.6, 0.04);
  CHECK(gentle == doctest::Approx(std::sqrt(9.81 * 0.6 * 0.01)).epsilon(1e-5));
  const f64 steep =
      water::shear_velocity(static_cast<f64>(water::manning_speed(0.04f, 0.4f, 0.04f)), 0.4, 0.04);
  CHECK(steep == doctest::Approx(std::sqrt(9.81 * 0.4 * 0.04)).epsilon(1e-5));
  CHECK(water::surface_disturbance_m(gentle) == doctest::Approx(0.0030).epsilon(0.02));
  CHECK(water::surface_disturbance_m(steep) == doctest::Approx(0.0080).epsilon(0.02));
  // The bed's turbulence alone never breaks a stream's surface: q^2 > 2 g L with L the depth needs
  // u*^2 = g d S > 2 g d, a slope of 2. What whitens a stream is the Froude rule's (below).
  CHECK(steep * steep < 2.0 * water::k_gravity * 0.4);
  // Self-aeration from about a metre a second: inside Cummings and Chanson's 0.74 to 1.58 m/s.
  CHECK(water::k_channel_aeration_speed >= 0.74);
  CHECK(water::k_channel_aeration_speed <= 1.58);
}

TEST_CASE("water channel: standing waves stand at Kennedy's wavelength") {
  // lambda = 2 pi v^2 / g: the wave whose phase speed sqrt(g / k) is the flow's.
  for (const f64 v : {1.0, 2.0, 2.71, 4.0}) {
    const f64 lambda = water::standing_wavelength_m(v);
    const f64 k = 2.0 * 3.14159265358979323846 / lambda;
    CHECK(std::sqrt(water::k_gravity / k) == doctest::Approx(v).epsilon(1e-12));
  }
  CHECK(water::standing_wavelength_m(2.0) == doctest::Approx(2.5620).epsilon(1e-4));
  CHECK(water::k_channel_wave_froude_lo < 1.0);
  CHECK(water::k_channel_wave_froude_hi > 1.0);
}

TEST_CASE("water channel: from the section's mean to a vertical's and to the surface") {
  // Down a vertical: the mean of (z / d)^(1/6) over the depth is 6/7, so the surface runs at 7/6
  // of the vertical's mean.
  const u32 n = 200000;
  f64 mean = 0.0;
  for (u32 i = 0; i < n; ++i)
    mean += std::pow((static_cast<f64>(i) + 0.5) / n, 1.0 / 6.0) / n;
  CHECK(mean == doctest::Approx(6.0 / 7.0).epsilon(1e-5));
  CHECK(water::k_channel_surface_factor == doctest::Approx(1.0 / mean).epsilon(1e-5));
  // Across the section: kappa = D^(5/3) / mean(d^(5/3)) makes the verticals' flows add up to the
  // section's. A rectangle's is 1, a parabola's 0.895, a triangle's 0.84; the draw's 0.9 is a
  // parabola's to 1 %, 10 % under a rectangle's and 7 % over a triangle's, and that is the error
  // in the discharge its speeds carry.
  const auto kappa = [&](f64 (*depth)(f64)) {
    f64 area = 0.0, conveyance = 0.0;
    for (u32 i = 0; i < n; ++i) {
      const f64 d = depth(-1.0 + 2.0 * (static_cast<f64>(i) + 0.5) / n);
      area += d / n;
      conveyance += std::pow(d, 5.0 / 3.0) / n;
    }
    return std::pow(area, 5.0 / 3.0) / conveyance;
  };
  const f64 rectangle = kappa([](f64) { return 1.0; });
  const f64 parabola = kappa([](f64 x) { return 1.0 - x * x; });
  const f64 triangle = kappa([](f64 x) { return 1.0 - std::abs(x); });
  CHECK(rectangle == doctest::Approx(1.0).epsilon(1e-9));
  CHECK(parabola == doctest::Approx(0.8947).epsilon(1e-3));
  CHECK(triangle == doctest::Approx(0.84).epsilon(1e-3));
  CHECK(std::abs(water::k_channel_conveyance / parabola - 1.0) < 0.01);
  CHECK(water::k_channel_conveyance / rectangle - 1.0 == doctest::Approx(-0.10).epsilon(1e-6));
  CHECK(water::k_channel_conveyance / triangle - 1.0 == doctest::Approx(0.0714).epsilon(1e-2));
  // The share itself: kappa (d / D)^(2/3), nothing where there is no depth, never past the bound.
  CHECK(water::lateral_speed_share(0.5, 0.5) == doctest::Approx(0.9).epsilon(1e-12));
  CHECK(water::lateral_speed_share(1.0, 0.5) == doctest::Approx(0.9 * std::cbrt(4.0)).epsilon(1e-12));
  CHECK(water::lateral_speed_share(0.0, 0.5) == 0.0);
  CHECK(water::lateral_speed_share(5.0, 0.5) == water::k_channel_profile_max);
}

TEST_CASE("water channel: a fall's sheet holds for Horeni's length") {
  // 5.89 q^0.319: a litre a second a metre breaks up after 0.65 m, so a 25 cm step is glass at
  // any discharge; the fixture's 1.6 m3/s over a 2.75 m lip holds for 4.95 m, past its 2.24 m.
  CHECK(water::nappe_breakup_m(0.001) == doctest::Approx(0.650).epsilon(2e-3));
  CHECK(water::nappe_breakup_m(1.5993 / 2.754) == doctest::Approx(4.95).epsilon(2e-3));
  CHECK(water::nappe_breakup_m(1e-5) > 0.14);
  CHECK(water::nappe_breakup_m(0.0) == 0.0);
  // A 2 m fall is white at its foot under 34 litres a second a metre.
  CHECK(water::nappe_breakup_m(0.034) == doctest::Approx(2.0).epsilon(0.01));
}

TEST_CASE("water channel: plunge records pack into one entry, seven at most") {
  gfx::ResolveMaterial water;
  gfx::ResolveMaterial entry;
  gfx::WaterPlunge list[9];
  for (u32 i = 0; i < 9; ++i)
    list[i] = gfx::WaterPlunge{Vec2{static_cast<f32>(i), -static_cast<f32>(i)},
                               1.0f + static_cast<f32>(i), 9.0f};
  list[2].radius_m = 0.0f;  // no radius: dropped
  CHECK(gfx::set_water_plunges(water, entry, list, 9) == gfx::k_water_plunges);
  CHECK((water.flags & gfx::k_material_water_plunge) != 0);
  gfx::WaterPlunge rows[gfx::k_water_plunges];
  std::memcpy(static_cast<void*>(rows), static_cast<const void*>(&entry), sizeof(rows));
  CHECK(rows[0].at.x == 0.0f);
  CHECK(rows[1].radius_m == 2.0f);
  CHECK(rows[2].radius_m == 4.0f);  // the third had none, the fourth moved up
  CHECK(rows[6].radius_m == 8.0f);
  // Fewer than seven: the rest zero, so the list ends.
  gfx::ResolveMaterial water2;
  gfx::ResolveMaterial entry2;
  CHECK(gfx::set_water_plunges(water2, entry2, list, 2) == 2);
  std::memcpy(static_cast<void*>(rows), static_cast<const void*>(&entry2), sizeof(rows));
  CHECK(rows[2].radius_m == 0.0f);
  // None: no flag, and the bits are the channel's and the shore's neighbours.
  gfx::ResolveMaterial water3;
  gfx::ResolveMaterial entry3;
  CHECK(gfx::set_water_plunges(water3, entry3, list + 2, 1) == 0);
  CHECK((water3.flags & gfx::k_material_water_plunge) == 0);
  CHECK(gfx::k_material_water_plunge == (gfx::k_material_water_channel << 1));
}
