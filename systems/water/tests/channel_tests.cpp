// Channels of flowing water on the CPU (docs/subsystems/water.md, "Channels"; roadmap S96): the
// description checked and refused, Manning's speed and the Froude number, the ribbon over a
// synthetic channel of six points with a 10 m fall (its width, its heights, its phase monotone and
// scaled by the speed, its fall steeper than 45 degrees and the rest facing up, the wrap), the
// depth map from the description, the GLB's bytes the same twice and pinned, and the generator
// through the registry. No device.
#include <core/hash/hash.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/assets/gltf.h>
#include <domain/gfx/visibility_resolve.h>
#include <domain/scene_gen/scene_gen.h>
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

using namespace engine;

namespace {

// Six points down a slope, far from the world's origin: a gentle reach, a steep reach (rapids), a
// lip, a 10 m fall over a metre of run into a plunge pool, and a gentle run-out.
scene::WaterChannel test_channel() {
  scene::WaterChannel c;
  c.name = "brook";
  const f64 x0 = 5000.0, z0 = -3000.0;
  c.points = {WorldPos{x0 + 0.0, 20.0, z0 + 0.0},  WorldPos{x0 + 20.0, 19.8, z0 + 0.0},
              WorldPos{x0 + 40.0, 19.0, z0 + 5.0}, WorldPos{x0 + 55.0, 18.4, z0 + 10.0},
              WorldPos{x0 + 56.0, 8.4, z0 + 10.0}, WorldPos{x0 + 80.0, 8.2, z0 + 12.0}};
  c.width_m = {4.0f, 4.0f, 3.0f, 3.0f, 3.0f, 5.0f};
  c.depth_m = {0.6f, 0.6f, 0.4f, 0.4f, 1.5f, 0.8f};
  c.slope = {0.01f, 0.01f, 0.04f, 0.04f, 0.01f, 0.008f};
  c.seed = 7;
  return c;
}

}  // namespace

TEST_CASE("water channel: the description is read, and refused with a sentence") {
  water::Channel ch;
  std::string error;
  REQUIRE_MESSAGE(water::read_channel(test_channel(), ch, &error), error);
  CHECK(ch.points.size() == 6);
  // The origin: the points' bounding box centre in x and z, the first point's surface.
  CHECK(ch.origin.x == 5040.0);
  CHECK(ch.origin.y == 20.0);
  CHECK(ch.origin.z == -2994.0);
  CHECK(ch.points[0].x == -40.0f);
  CHECK(ch.manning_n == 0.04f);

  const auto refused = [&](scene::WaterChannel c, const char* says) {
    std::string why;
    water::Channel out;
    CHECK_FALSE(water::read_channel(c, out, &why));
    CHECK_MESSAGE(why.find(says) != std::string::npos, why);
    CHECK(why.find("\"brook\"") != std::string::npos);
  };
  scene::WaterChannel one = test_channel();
  one.points.resize(1);
  one.width_m.resize(1);
  one.depth_m.resize(1);
  one.slope.resize(1);
  refused(one, "fewer than two points");
  scene::WaterChannel unequal = test_channel();
  unequal.depth_m.pop_back();
  refused(unequal, "one entry each per point");
  scene::WaterChannel narrow = test_channel();
  narrow.width_m[2] = 0.0f;
  refused(narrow, "width_m at point 2 must be positive");
  scene::WaterChannel dry = test_channel();
  dry.depth_m[0] = -0.1f;
  refused(dry, "depth_m at point 0 must be positive");
  scene::WaterChannel uphill = test_channel();
  uphill.slope[3] = -0.02f;
  refused(uphill, "slope at point 3 is negative where the line descends");
  // A negative slope where the line does not descend (a level pool) is the description's to say.
  scene::WaterChannel level = test_channel();
  for (WorldPos& p : level.points)
    p.y = 5.0;
  level.slope[1] = -0.01f;
  CHECK(water::read_channel(level, ch, &error));
}

TEST_CASE("water channel: Manning's speed, the Froude number and the falls") {
  // v = (1/n) d^(2/3) S^(1/2): n 0.04, 0.6 m deep, 1 in 100 — 1.7786 m/s, Froude 0.733.
  const f32 v = water::manning_speed(0.04f, 0.6f, 0.01f);
  CHECK(v == doctest::Approx(25.0 * std::pow(0.6, 2.0 / 3.0) * 0.1).epsilon(1e-6));
  CHECK(water::froude(v, 0.6f) ==
        doctest::Approx(static_cast<f64>(v) / std::sqrt(9.81 * 0.6)).epsilon(1e-6));
  CHECK(water::froude(v, 0.6f) < 1.0f);
  // The steep reach is supercritical: 0.4 m at 1 in 25, 2.71 m/s, Froude 1.37.
  const f32 rapid = water::manning_speed(0.04f, 0.4f, 0.04f);
  CHECK(water::froude(rapid, 0.4f) > 1.0f);
  // No slope is still water, which still moves a little.
  CHECK(water::manning_speed(0.04f, 1.0f, 0.0f) == water::k_channel_min_speed);
  for (const f64 a : {0.0, 1e-6, 0.36, 1.0, 8.0, 2.25, 1e6})
    CHECK(water::cube_root(a) == doctest::Approx(std::cbrt(a)).epsilon(1e-14));
  water::Channel ch;
  REQUIRE(water::read_channel(test_channel(), ch, nullptr));
  CHECK_FALSE(water::stretch_falls(ch, 0));
  CHECK_FALSE(water::stretch_falls(ch, 2));
  CHECK(water::stretch_falls(ch, 3));
  CHECK_FALSE(water::stretch_falls(ch, 4));
}

TEST_CASE("water channel: the ribbon's width, heights, phase and fall") {
  water::Channel ch;
  REQUIRE(water::read_channel(test_channel(), ch, nullptr));
  water::ChannelMesh cm;
  water::build_channel_mesh(ch, cm);
  const water::Mesh& m = cm.mesh;
  const u32 per_row = cm.columns + 1;
  REQUIRE(per_row == m.edge);
  REQUIRE(m.positions.size() == m.rings * per_row);
  REQUIRE(m.positions.size() == m.uvs.size());
  REQUIRE(m.positions.size() == m.normals.size());
  REQUIRE(cm.section_s.size() >= 6);
  MESSAGE("channel ribbon: spacing ", m.spacing, " m, ", cm.columns, " columns, ",
          cm.section_s.size(), " sections, ", m.rings, " rows, ", m.positions.size(), " vertices, ",
          m.indices.size() / 3, " triangles, travel ", cm.section_tau.back(), " s");
  CHECK(water::channel_spacing(ch) == doctest::Approx(0.6666667f));  // the shore's third
  // Every row level across, as wide as its section's width with the extension, depth zero past it.
  const f32 span = 1.0f + 2.0f * water::k_channel_edge_extend;
  for (u32 r = 0; r < m.rings; ++r) {
    const Vec3 left = m.positions[r * per_row];
    const Vec3 right = m.positions[r * per_row + cm.columns];
    const f32 w = length(right - left) / span;
    CHECK(w >= 3.0f - 1e-3f);
    CHECK(w <= 5.0f + 1e-3f);
    for (u32 j = 0; j < per_row; ++j) {
      CHECK(m.positions[r * per_row + j].y == doctest::Approx(left.y).epsilon(1e-6));
      CHECK(m.uvs[r * per_row + j].x == m.uvs[r * per_row].x);
    }
    CHECK(m.uvs[r * per_row].y == 0.0f);
    CHECK(m.uvs[r * per_row + cm.columns].y == 0.0f);
  }
  // The surface's height along the line: the points' heights, interpolated.
  CHECK(m.positions[cm.columns / 2].y == doctest::Approx(0.0f));
  CHECK(m.positions[(m.rings - 1) * per_row + cm.columns / 2].y ==
        doctest::Approx(8.2f - 20.0f).epsilon(1e-5));
  // The phase: the travel time, strictly increasing, each step the station's run over the mean
  // slowness of its two ends.
  for (u32 k = 1; k < cm.section_tau.size(); ++k) {
    CHECK(cm.section_tau[k] > cm.section_tau[k - 1]);
    const f64 ds = static_cast<f64>(cm.section_s[k] - cm.section_s[k - 1]);
    const f64 dt = 0.5 * ds *
                   (1.0 / static_cast<f64>(cm.section_speed[k - 1]) +
                    1.0 / static_cast<f64>(cm.section_speed[k]));
    CHECK(cm.section_tau[k] - cm.section_tau[k - 1] == doctest::Approx(dt).epsilon(1e-9));
  }
  // The first reach runs at Manning's 1.78 m/s; the fall's foot at the free fall's speed from the
  // lip, sqrt(v_lip^2 + 2 g 10).
  CHECK(cm.section_speed[0] == doctest::Approx(water::manning_speed(0.04f, 0.6f, 0.01f)));
  const f32 v_lip = water::manning_speed(0.04f, 0.4f, 0.04f);
  const f32 v_foot = std::sqrt(v_lip * v_lip + 2.0f * 9.81f * 10.0f);
  CHECK(*std::max_element(cm.section_speed.begin(), cm.section_speed.end()) ==
        doctest::Approx(v_foot).epsilon(1e-4));
  // The total travel time passes a period, so the phase wraps: UV.x stays within a period and a
  // strip, and some sections are written twice.
  CHECK(cm.section_tau.back() > static_cast<f64>(water::k_channel_phase_period_s));
  CHECK(m.rings > cm.section_s.size());
  for (const Vec2& uv : m.uvs) {
    CHECK(uv.x >= 0.0f);
    CHECK(uv.x < water::k_channel_phase_period_s + 1.0f);
  }
  // Every triangle: the phase rises downstream across it (UV.x of the bottom row past the top's),
  // and it faces out of the water — up on the bed's reaches, steeper than 45 degrees on the fall.
  u32 fall_triangles = 0;
  u32 bed_triangles = 0;
  for (u32 t = 0; t < m.indices.size(); t += 3) {
    const Vec3 a = m.positions[m.indices[t]];
    const Vec3 b = m.positions[m.indices[t + 1]];
    const Vec3 c = m.positions[m.indices[t + 2]];
    const Vec3 nrm = cross(b - a, c - a);
    const f32 ny = nrm.y / length(nrm);
    const f32 ua = m.uvs[m.indices[t]].x;
    const f32 uc = m.uvs[m.indices[t + 2]].x;
    CHECK(uc > ua);  // c is on the row below a in both triangles of a quad
    if (ny < 0.70710678f) {
      ++fall_triangles;
      // The fall faces downstream (+x here).
      CHECK(nrm.x > 0.0f);
    } else {
      ++bed_triangles;
    }
  }
  // The fall is one stretch of 10.05 m at the spacing: 16 sections' strips, two triangles a quad.
  const u32 fall_strips =
      static_cast<u32>(std::ceil(std::sqrt(101.0) / static_cast<f64>(m.spacing)));
  CHECK(fall_triangles == fall_strips * cm.columns * 2);
  CHECK(bed_triangles > fall_triangles);
  u32 falls = 0;
  for (const u8 f : cm.section_falls)
    falls += f;
  CHECK(falls == fall_strips);
}

TEST_CASE("water channel: the depth map is the description's bed") {
  water::Channel ch;
  REQUIRE(water::read_channel(test_channel(), ch, nullptr));
  water::ChannelMesh cm;
  water::build_channel_mesh(ch, cm);
  water::DepthMap map;
  water::build_channel_depth_map(ch, cm, map);
  REQUIRE(map.size >= water::k_depth_map_min);
  REQUIRE(map.size <= water::k_depth_map_max);
  const auto at = [&](f32 x, f32 z) {
    const f32 fi = (x - map.origin.x) / map.texel_m;
    const f32 fj = (z - map.origin.y) / map.texel_m;
    const u32 i = static_cast<u32>(std::lround(fi));
    const u32 j = static_cast<u32>(std::lround(fj));
    return static_cast<f32>(map.texels[j * map.size + i]) / 65535.0f * map.depth_max;
  };
  // The deepest is the plunge pool's 1.5 m at the fall's foot.
  CHECK(map.depth_max == doctest::Approx(1.5f).epsilon(0.02));
  // On the line at the head, 0.6 m (within a texel's slope of the parabola); 1.9 m off it — past
  // the 2 m half width less a texel — nearly dry; 3 m off it, dry.
  CHECK(at(ch.points[0].x + 0.01f, ch.points[0].z) == doctest::Approx(0.6f).epsilon(0.03));
  CHECK(at(ch.points[0].x + 5.0f, ch.points[0].z + 3.0f) == 0.0f);
  CHECK(at(ch.points[0].x + 5.0f, ch.points[0].z + 1.0f) ==
        doctest::Approx(0.6f * (1.0f - 0.25f)).epsilon(0.05));
}

TEST_CASE("water channel: the GLB is the same bytes twice, pinned, and the generator makes it") {
  water::Channel ch;
  REQUIRE(water::read_channel(test_channel(), ch, nullptr));
  water::ChannelMesh a;
  water::ChannelMesh b;
  water::build_channel_mesh(ch, a);
  water::build_channel_mesh(ch, b);
  const Vector<u8> glb_a = water::mesh_glb(a.mesh, Vec3{0.30f, 0.55f, 0.50f}, 0.08f);
  const Vector<u8> glb_b = water::mesh_glb(b.mesh, Vec3{0.30f, 0.55f, 0.50f}, 0.08f);
  REQUIRE(glb_a.size() == glb_b.size());
  CHECK(std::equal(glb_a.begin(), glb_a.end(), glb_b.begin()));
  water::DepthMap map;
  water::build_channel_depth_map(ch, a, map);
  const Vector<u8> map_bytes = water::depth_map_bytes(map);
  const u64 glb_hash = hash_bytes(glb_a.data(), glb_a.size());
  const u64 map_hash = hash_bytes(map_bytes.data(), map_bytes.size());
  MESSAGE("channel glb ", glb_a.size(), " bytes, hash ", glb_hash, "; map ", map_bytes.size(),
          " bytes, hash ", map_hash);
  // Taken on MSVC (2026-11-07): the toolchains reproduce them, since every number is + - * / and
  // sqrt (the cube root is Newton's, not the C library's).
  CHECK(glb_hash == 17028419247352518564ull);  // 100,928 bytes
  // Re-pinned by S94: the depth map's header says version 2 (a channel's map has no bank, so only
  // the version word moved). Re-pinned by S100 stage B: version 3's header holds the mesh's three
  // spacings, twelve bytes more (the texels are the bytes they were).
  CHECK(map_hash == 13241460754695211659ull);  // 466,622 bytes
  // The importer reads it as the renderer will: positions, normals and the phase and depth UVs.
  assets::MeshData data;
  std::string error;
  REQUIRE_MESSAGE(
      assets::load_gltf_memory(std::span<const u8>(glb_a.data(), glb_a.size()), "", data, &error),
      error);
  CHECK(data.positions.size() == a.mesh.positions.size());

  const test::TempDir tmp("water_channel_generator");
  const scene_gen::PlacementGeneratorDesc* generator =
      scene_gen::GeneratorRegistry::global().find_placement(water::k_channel_generator);
  REQUIRE(generator != nullptr);
  const scene::WaterChannel blocks[1] = {test_channel()};
  scene_gen::Context context;
  const std::string dir = tmp.path();
  context.dir = dir;
  context.where = "test";
  void* state = nullptr;
  REQUIRE_MESSAGE(generator->open(water::channel_generator_params(blocks), context, &state, &error),
                  error);
  scene_gen::Placements placed;
  REQUIRE_MESSAGE(generator->expand(state, context, placed, &error), error);
  generator->close(state);
  REQUIRE(placed.meshes.size() == 1);
  CHECK(placed.meshes[0].name == "water/brook");
  CHECK(io::exists(placed.meshes[0].path));
  CHECK(io::exists(placed.meshes[0].path.substr(0, placed.meshes[0].path.size() - 4) + ".depth"));
  CHECK(placed.instances[0].position == ch.origin);
  scene::WaterChannel bad = test_channel();
  bad.width_m.pop_back();
  CHECK_FALSE(generator->open(
      water::channel_generator_params(std::span<const scene::WaterChannel>(&bad, 1)), context,
      &state, &error));
  CHECK(error.find("one entry each per point") != std::string::npos);
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

// ---- S98: the regimes, the brink, the plunge stretch, the profile and the plunge records ------

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
  // The surface's deformation q^2 / 2g with q the shear velocity: the test's gentle reach (0.6 m
  // at 1 in 100, 1.78 m/s) is u* = sqrt(g d S) = 0.243 m/s and a few millimetres of surface; its
  // steep reach (0.4 m at 1 in 25) 0.396 m/s and about eight millimetres.
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

TEST_CASE("water channel: the lateral profile carries the section's discharge") {
  // C = (4/3) / integral (1 - x^2)^(7/6) = (4/3) Gamma(8/3) / (sqrt(pi) Gamma(13/6)).
  const f64 closed = (4.0 / 3.0) * std::tgamma(8.0 / 3.0) /
                     (std::sqrt(3.14159265358979323846) * std::tgamma(13.0 / 6.0));
  CHECK(water::k_channel_profile_mean == doctest::Approx(closed).epsilon(1e-6));
  // The discharge across a parabolic bed of width w and centre depth d at a mean speed v is v
  // times its area, 2/3 w d: integrate the profile's speed times the depth across the width.
  const f64 w = 4.0, d = 0.6, v = 1.78;
  const u32 n = 200000;
  f64 discharge = 0.0, area = 0.0;
  for (u32 i = 0; i < n; ++i) {
    const f64 a = -0.5 * w + (static_cast<f64>(i) + 0.5) * (w / static_cast<f64>(n));
    const f64 x = 2.0 * a / w;
    const f64 r = 1.0 - x * x;
    discharge += v * water::lateral_speed_share(r) * d * r * (w / static_cast<f64>(n));
    area += d * r * (w / static_cast<f64>(n));
  }
  CHECK(area == doctest::Approx(2.0 / 3.0 * w * d).epsilon(1e-6));
  CHECK(discharge == doctest::Approx(v * area).epsilon(1e-5));
  // Fastest mid-channel, at C times the mean, and still at the banks.
  CHECK(water::lateral_speed_share(1.0) == doctest::Approx(water::k_channel_profile_mean));
  CHECK(water::lateral_speed_share(0.0) == 0.0);
  CHECK(water::lateral_speed_share(0.25) < 1.0);
}

TEST_CASE(
    "water channel: a subcritical reach speeds up into the brink, and a fall's foot plunges") {
  // A gentle reach (Froude 0.73) to a 6 m fall at its end, nothing below it.
  scene::WaterChannel block;
  block.name = "spout";
  block.points = {WorldPos{0.0, 10.0, 0.0}, WorldPos{30.0, 9.7, 0.0}, WorldPos{31.0, 3.7, 0.0}};
  block.width_m = {4.0f, 4.0f, 4.0f};
  block.depth_m = {0.6f, 0.6f, 0.6f};
  block.slope = {0.01f, 0.01f, 0.01f};
  water::Channel ch;
  REQUIRE(water::read_channel(block, ch, nullptr));
  // The plunge stretch: one more point, 0.4 of the 6 m on along the fall's heading, level.
  const water::Channel plunged = water::with_plunge_stretch(ch);
  REQUIRE(plunged.points.size() == 4);
  CHECK(plunged.points[3].x == doctest::Approx(ch.points[2].x + 2.4f));
  CHECK(plunged.points[3].y == ch.points[2].y);
  CHECK(plunged.slope[3] == 0.0f);
  water::Channel pooled = ch;
  pooled.pooled = true;
  CHECK(water::with_plunge_stretch(pooled).points.size() == 3);
  // A channel that continues past its fall is its own.
  water::Channel test;
  REQUIRE(water::read_channel(test_channel(), test, nullptr));
  CHECK(water::with_plunge_stretch(test).points.size() == test.points.size());

  water::ChannelMesh cm;
  water::build_channel_mesh(ch, cm);
  // Rouse's brink: q = v d, d_c = (q^2 / g)^(1/3), the lip's speed q / (0.715 d_c).
  const f64 v = static_cast<f64>(water::manning_speed(0.04f, 0.6f, 0.01f));
  const f64 q = v * 0.6;
  const f64 dc = std::cbrt(q * q / 9.81);
  const f64 brink = q / (0.715 * dc);
  CHECK(brink > v);
  u32 lip = 0;
  for (u32 k = 0; k < cm.section_s.size(); ++k)
    if (cm.section_falls[k] != 0) {
      lip = k;
      break;
    }
  REQUIRE(lip > 0);
  CHECK(static_cast<f64>(cm.section_speed[lip]) == doctest::Approx(brink).epsilon(1e-5));
  // The reach is Manning's until four d_c from the lip, and speeds up through them.
  CHECK(static_cast<f64>(cm.section_speed[0]) == doctest::Approx(v).epsilon(1e-6));
  for (u32 k = 1; k <= lip; ++k)
    CHECK(cm.section_speed[k] >= cm.section_speed[k - 1]);
  // The fall leaves at the brink's speed and lands at sqrt(v_b^2 + 2 g 6); the plunge stretch
  // then slows it back to the still pool's drift.
  const f32 top = *std::max_element(cm.section_speed.begin(), cm.section_speed.end());
  CHECK(static_cast<f64>(top) ==
        doctest::Approx(std::sqrt(brink * brink + 2.0 * 9.81 * 6.0)).epsilon(1e-4));
  CHECK(cm.section_speed.back() == doctest::Approx(water::k_channel_min_speed));
  CHECK(cm.section_s.back() == doctest::Approx(cm.section_s[lip] + 6.0833f + 2.4f).epsilon(1e-3));
  // A pooled channel stops at the foot.
  water::ChannelMesh pm;
  water::build_channel_mesh(pooled, pm);
  CHECK(pm.section_s.back() < cm.section_s.back());
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
