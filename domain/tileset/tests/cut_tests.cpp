// Channels cut into the ground (cuts.h; docs/subsystems/tileset.md, "Channels cut into the
// ground"; roadmap W89): the section against its formula, the finest level cut and a level whose
// spacing is not finer than the width keeping its heights, the tiles no cut reaches unmoved, a set
// without cuts keyed as it was, and the provider standing on the cut ground.
#include <domain/scene_gen/scene_gen.h>
#include <domain/tileset/cuts.h>
#include <domain/tileset/tileset.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <schemas/scene.h>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::tileset;

namespace {

scene::TerrainCut cut_of(std::string name, WorldPos a, WorldPos b, f32 width, f32 depth,
                         f32 slope = 1.0f) {
  scene::TerrainCut c;
  c.name = std::move(name);
  c.points = {a, b};
  c.width_m = {width, width};
  c.depth_m = {depth, depth};
  c.bank_slope = slope;
  return c;
}

// The distance from (x, z) to the segment, computed here apart from the build's own.
f64 segment_distance(f64 x, f64 z, f64 ax, f64 az, f64 bx, f64 bz) {
  const f64 ex = bx - ax, ez = bz - az;
  const f64 t = std::clamp(((x - ax) * ex + (z - az) * ez) / (ex * ex + ez * ez), 0.0, 1.0);
  const f64 px = ax + t * ex - x, pz = az + t * ez - z;
  return std::sqrt(px * px + pz * pz);
}

// A flat 40 m square at 25 cm, 16-cell tiles (4 m), its chain 1, 2, 4 and 8 m.
struct Flat {
  test::TempDir tmp{"engine_tileset_cuts"};
  Heightfield field = synth_heightfield(SynthKind::flat, 161, 250);
  BuildOptions options;
  Flat() {
    options.cells = 16;
    options.ring_spacing_mm = 1000;
    options.far_ratio = 1;
    options.far_levels = 4;
  }
  BuildResult build(const BuildOptions& o, const std::string& name) {
    BuildResult out;
    std::string error;
    REQUIRE_MESSAGE(build_tileset(field, 0x1234, "flat", o, tmp.file(name), out, error), error);
    return out;
  }
};

}  // namespace

TEST_CASE("tileset: a cut's section is the trapezoid its formula gives") {
  f64 y = 0;
  // 3 m wide, 0.6 m deep, banks at 45 degrees: a flat bed 1.8 m wide, the top at 1.5 m.
  REQUIRE(cut_section(10.0, 3.0, 0.6, 1.0, 0.0, y));
  CHECK(y == doctest::Approx(9.4).epsilon(1e-12));
  REQUIRE(cut_section(10.0, 3.0, 0.6, 1.0, 0.9, y));
  CHECK(y == doctest::Approx(9.4).epsilon(1e-12));
  REQUIRE(cut_section(10.0, 3.0, 0.6, 1.0, -1.2, y));
  CHECK(y == doctest::Approx(9.7).epsilon(1e-12));
  REQUIRE(cut_section(10.0, 3.0, 0.6, 1.0, 1.5, y));
  CHECK(y == doctest::Approx(10.0).epsilon(1e-12));
  // Past the top the face goes on up for at most a depth: to 2.1 m, then nothing.
  REQUIRE(cut_section(10.0, 3.0, 0.6, 1.0, 2.0, y));
  CHECK(y == doctest::Approx(10.5).epsilon(1e-12));
  CHECK_FALSE(cut_section(10.0, 3.0, 0.6, 1.0, 2.2, y));
  // A bed narrower than the banks allow is a V: 1 m wide and 1 m deep steepens the banks to 2.
  REQUIRE(cut_section(10.0, 1.0, 1.0, 1.0, 0.0, y));
  CHECK(y == doctest::Approx(9.0).epsilon(1e-12));
  REQUIRE(cut_section(10.0, 1.0, 1.0, 1.0, 0.25, y));
  CHECK(y == doctest::Approx(9.5).epsilon(1e-12));
}

TEST_CASE("tileset: a cut lowers the finest level to its section and leaves the rest as it was") {
  Flat s;
  const BuildResult plain = s.build(s.options, "plain");
  BuildOptions o = s.options;
  // A 3 m channel along z = 20 m from x = 5 to 35 m, 0.6 m deep, its top on the flat ground.
  const scene::TerrainCut cut =
      cut_of("run", WorldPos{5.0, 0.0, 20.0}, WorldPos{35.0, 0.0, 20.0}, 3.0f, 0.6f);
  std::string error;
  REQUIRE_MESSAGE(o.cuts.build({&cut, 1}, &error), error);
  const BuildResult cut_set = s.build(o, "cut");
  CHECK(cut_set.key != plain.key);
  CHECK(cut_set.cut_points > 0);

  TileSet a, b;
  REQUIRE_MESSAGE(a.open(plain.manifest, error), error);
  REQUIRE_MESSAGE(b.open(cut_set.manifest, error), error);
  // Every finest point: the section where the cut reaches, rounded once, else the ground; and a
  // finest tile no cut reaches holds the same millimetres it held without the cut.
  const i64 sp = 250;
  const f64 reach = 1.5 + 0.6;  // half the width, and the face's run past it
  u32 cut_points = 0, untouched_tiles = 0, touched_tiles = 0;
  for (i64 tz = 0; tz < 10; ++tz) {
    for (i64 tx = 0; tx < 10; ++tx) {
      bool tile_touched = false;
      for (i64 j = tz * 16; j <= tz * 16 + 16; ++j) {
        for (i64 i = tx * 16; i <= tx * 16 + 16; ++i) {
          const f64 x = static_cast<f64>(i * sp), z = static_cast<f64>(j * sp);
          const f64 ground = a.point_mm(i * sp, j * sp);
          // Millimetres, as the build computes them.
          const f64 d_mm = segment_distance(x, z, 5000.0, 20000.0, 35000.0, 20000.0);
          const f64 d = d_mm / 1000.0;
          f64 expected = ground;
          f64 section = 0;
          if (cut_section(0.0, 3000.0, 600.0, 1.0, d_mm, section))
            expected = std::min(ground, std::floor(section + 0.5));
          CHECK(b.point_mm(i * sp, j * sp) == expected);
          if (expected != ground) {
            ++cut_points;
            tile_touched = true;
          }
          if (d > reach + 0.25) CHECK(b.point_mm(i * sp, j * sp) == ground);
        }
      }
      ++(tile_touched ? touched_tiles : untouched_tiles);
    }
  }
  CHECK(cut_points > 1000);
  CHECK(touched_tiles >= 8);
  CHECK(untouched_tiles >= 60);
  // The bed under the line and the top at the bank's edge.
  CHECK(b.point_mm(20000, 20000) == -600.0);
  CHECK(b.point_mm(20000, 21500) == 0.0);

  // The chain: a level finer than the width (1 and 2 m) holds the cut; one not finer (4, 8 m)
  // keeps its heights to the byte, its hash the plain set's.
  const TileSetManifest& pm = a.manifest();
  const TileSetManifest& cm = b.manifest();
  REQUIRE(pm.levels.size() == 5);
  REQUIRE(cm.levels.size() == 5);
  CHECK(cm.levels[0].hash != pm.levels[0].hash);
  CHECK(cm.levels[1].spacing_mm == 1000);
  CHECK(cm.levels[1].hash != pm.levels[1].hash);
  CHECK(cm.levels[2].spacing_mm == 2000);
  CHECK(cm.levels[2].hash != pm.levels[2].hash);
  CHECK(cm.levels[3].spacing_mm == 4000);
  CHECK(cm.levels[3].hash == pm.levels[3].hash);
  CHECK(cm.levels[4].hash == pm.levels[4].hash);
}

TEST_CASE("tileset: a cut no wider than a level's spacing closes on it") {
  Flat s;
  const BuildResult plain = s.build(s.options, "plain");
  BuildOptions o = s.options;
  // 0.2 m wide: not wider than the finest's 25 cm, so no level holds it.
  const scene::TerrainCut rill =
      cut_of("rill", WorldPos{5.0, 0.0, 20.0}, WorldPos{35.0, 0.0, 20.0}, 0.2f, 0.1f);
  std::string error;
  REQUIRE_MESSAGE(o.cuts.build({&rill, 1}, &error), error);
  const BuildResult closed = s.build(o, "rill");
  CHECK(closed.cut_points == 0);
  CHECK(closed.key != plain.key);
  TileSet a, b;
  REQUIRE_MESSAGE(a.open(plain.manifest, error), error);
  REQUIRE_MESSAGE(b.open(closed.manifest, error), error);
  for (u32 l = 0; l < a.manifest().levels.size(); ++l)
    CHECK(b.manifest().levels[l].hash == a.manifest().levels[l].hash);
}

TEST_CASE("tileset: cuts key a set only when there are some, and their order is not part of it") {
  Flat s;
  const BuildOptions none = s.options;
  BuildOptions empty = s.options;
  std::string error;
  REQUIRE(empty.cuts.build({}, &error));
  CHECK(empty.cuts.empty());
  CHECK(tileset_key(7, none) == tileset_key(7, empty));
  // Two crossing channels, built in either order: the same heights to the byte (the lowest of the
  // sections, whichever came first), though the key, which names the order, differs.
  const scene::TerrainCut a =
      cut_of("a", WorldPos{5.0, 0.0, 20.0}, WorldPos{35.0, 0.0, 20.0}, 3.0f, 0.6f);
  const scene::TerrainCut b =
      cut_of("b", WorldPos{20.0, 0.2, 5.0}, WorldPos{20.0, -0.1, 35.0}, 2.0f, 0.9f, 2.0f);
  BuildOptions ab = s.options, ba = s.options;
  const scene::TerrainCut first[2] = {a, b};
  const scene::TerrainCut second[2] = {b, a};
  REQUIRE(ab.cuts.build(first, &error));
  REQUIRE(ba.cuts.build(second, &error));
  const BuildResult x = s.build(ab, "ab");
  const BuildResult y = s.build(ba, "ba");
  TileSet tx, ty;
  REQUIRE_MESSAGE(tx.open(x.manifest, error), error);
  REQUIRE_MESSAGE(ty.open(y.manifest, error), error);
  for (u32 l = 0; l < tx.manifest().levels.size(); ++l)
    CHECK(tx.manifest().levels[l].hash == ty.manifest().levels[l].hash);
  // Building again is the same bytes.
  const BuildResult again = s.build(ab, "ab-again");
  CHECK(again.hash == x.hash);
}

TEST_CASE("tileset: a cut that is not one is refused with a sentence") {
  std::string error;
  TerrainCuts cuts;
  scene::TerrainCut one = cut_of("lone", WorldPos{0, 0, 0}, WorldPos{1, 0, 0}, 1.0f, 0.2f);
  one.points.resize(1);
  one.width_m.resize(1);
  one.depth_m.resize(1);
  CHECK_FALSE(cuts.build({&one, 1}, &error));
  CHECK(error.find("\"lone\" has fewer than two points") != std::string::npos);
  scene::TerrainCut uneven = cut_of("uneven", WorldPos{0, 0, 0}, WorldPos{1, 0, 0}, 1.0f, 0.2f);
  uneven.depth_m.push_back(0.3f);
  CHECK_FALSE(cuts.build({&uneven, 1}, &error));
  CHECK(error.find("length") != std::string::npos);
  scene::TerrainCut flat = cut_of("flat", WorldPos{0, 0, 0}, WorldPos{1, 0, 0}, 1.0f, 0.0f);
  CHECK_FALSE(cuts.build({&flat, 1}, &error));
  CHECK(error.find("positive") != std::string::npos);
  scene::TerrainCut sheer = cut_of("sheer", WorldPos{0, 0, 0}, WorldPos{1, 0, 0}, 1.0f, 0.2f, 9.f);
  CHECK_FALSE(cuts.build({&sheer, 1}, &error));
  CHECK(error.find("bank slope") != std::string::npos);
  CHECK(cuts.empty());
}

// ---- W93: the profile across a line, left and right, rounded, with a flood bed ------------------

namespace {

// A cut of `n` points along a line from `a` to `b`, with every W93 field per point.
struct Shaped {
  scene::TerrainCut cut;
  Shaped(std::string name, WorldPos a, WorldPos b, u32 n) {
    cut.name = std::move(name);
    for (u32 k = 0; k < n; ++k) {
      const f64 t = static_cast<f64>(k) / static_cast<f64>(n - 1);
      cut.points.push_back(
          WorldPos{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z)});
    }
  }
  // Width, depth and the two slopes at each point: `f(k, w, d, l, r)`.
  template <class F>
  Shaped& section(F f) {
    for (u32 k = 0; k < cut.points.size(); ++k) {
      f32 w = 0, d = 0, l = 0, r = 0;
      f(k, w, d, l, r);
      cut.width_m.push_back(w);
      cut.depth_m.push_back(d);
      cut.bank_slope_left.push_back(l);
      cut.bank_slope_right.push_back(r);
    }
    return *this;
  }
};

// A ground that is not flat: a slope with a swell, whole millimetres.
i32 swell_mm(i64 i, i64 j) {
  const f64 x = static_cast<f64>(i) * 0.25, z = static_cast<f64>(j) * 0.25;
  return static_cast<i32>(
      std::lround(1000.0 * (0.02 * x + 0.3 * std::sin(z / 5.0) * std::cos(x / 7.0))));
}

// The test's own reading of one straight segment's cut at a point, apart from the build's: the
// projection, the side from the cross product, the profile at the interpolated numbers, and
// beyond an end the two sides blended by the direction. Millimetres. False past the reach.
struct Station {
  f64 top, width, depth, left, right;
};
bool straight_surface(f64 x, f64 z, f64 ax, f64 az, f64 bx, f64 bz, const Station& a,
                      const Station& b, f64 round, f64& surface, f64& k) {
  const f64 ex = bx - ax, ez = bz - az, len = std::sqrt(ex * ex + ez * ez);
  const f64 raw = ((x - ax) * ex + (z - az) * ez) / (len * len);
  const f64 t = std::clamp(raw, 0.0, 1.0);
  const f64 qx = ax + t * ex - x, qz = az + t * ez - z;
  const f64 d = std::sqrt(qx * qx + qz * qz);
  const f64 lateral = (ex * (z - az) - ez * (x - ax)) / len;
  CutProfile p;
  p.top = a.top + t * (b.top - a.top);
  p.width = a.width + t * (b.width - a.width);
  p.depth = a.depth + t * (b.depth - a.depth);
  p.slope_left = a.left + t * (b.left - a.left);
  p.slope_right = a.right + t * (b.right - a.right);
  p.rise = p.depth;
  p.run_out = round > 0.0;
  k = round * p.depth;
  if (raw >= 0.0 && raw <= 1.0) return cut_profile(p, lateral >= 0.0 ? d : -d, surface);
  f64 l = 0, r = 0;
  const bool has_l = cut_profile(p, d, l), has_r = cut_profile(p, -d, r);
  if (!has_l && !has_r) return false;
  // A face that runs out ends twenty rises over the top (0.95 of its second run).
  const f64 end = p.run_out ? 20.0 * p.rise : p.rise;
  if (!has_l) l = p.top + end;
  if (!has_r) r = p.top + end;
  const f64 share = d > 0.0 ? std::clamp(0.5 + 0.5 * lateral / d, 0.0, 1.0) : 0.5;
  surface = r + share * (l - r);
  return true;
}

}  // namespace

TEST_CASE("tileset: a profile differs left from right, and W89's section is the symmetric one") {
  // 4 m wide, 1 m deep, the left bank a point bar at 0.5, the right a cut bank at 4: the flat bed
  // runs from the bar's toe on the line (2 / 0.5 = 2 m of run) to 0.25 m short of the right top.
  const CutProfile p{10.0, 4.0, 0.0, 1.0, 0.5, 4.0, 1.0, false};
  f64 y = 0;
  REQUIRE(cut_profile(p, 0.0, y));
  CHECK(y == doctest::Approx(9.0).epsilon(1e-12));
  REQUIRE(cut_profile(p, 1.0, y));  // half way up the bar
  CHECK(y == doctest::Approx(9.5).epsilon(1e-12));
  REQUIRE(cut_profile(p, -1.0, y));  // still the bed on the right
  CHECK(y == doctest::Approx(9.0).epsilon(1e-12));
  REQUIRE(cut_profile(p, -1.9, y));  // on the cut bank
  CHECK(y == doctest::Approx(9.6).epsilon(1e-12));
  REQUIRE(cut_profile(p, 2.0, y));
  CHECK(y == doctest::Approx(10.0).epsilon(1e-12));
  // Each face goes on a rise past its top at its own slope: 2 m more on the left, 0.25 m on the
  // right, then nothing.
  REQUIRE(cut_profile(p, 3.9, y));
  CHECK(y == doctest::Approx(10.95).epsilon(1e-12));
  CHECK_FALSE(cut_profile(p, 4.1, y));
  REQUIRE(cut_profile(p, -2.2, y));
  CHECK(y == doctest::Approx(10.8).epsilon(1e-12));
  CHECK_FALSE(cut_profile(p, -2.3, y));
  // Banks too gentle to reach the bed inside the width are steepened together to a V: 0.5 and 1
  // would need 3 m of run for 1 m of depth in a 2 m width, so both are 1.5 times as steep and
  // meet 1 m down, two thirds of the width from the gentler bank's top.
  const CutProfile v{10.0, 2.0, 0.0, 1.0, 0.5, 1.0, 1.0, false};
  REQUIRE(cut_profile(v, 1.0 - 4.0 / 3.0, y));
  CHECK(y == doctest::Approx(9.0).epsilon(1e-9));
  REQUIRE(cut_profile(v, 0.5, y));
  CHECK(y == doctest::Approx(10.0 - 0.75 * 0.5).epsilon(1e-9));
  // An offset moves the whole section off the line.
  const CutProfile o{10.0, 4.0, 1.0, 1.0, 1.0, 1.0, 1.0, false};
  REQUIRE(cut_profile(o, 3.0, y));
  CHECK(y == doctest::Approx(10.0).epsilon(1e-12));
  REQUIRE(cut_profile(o, -1.0, y));
  CHECK(y == doctest::Approx(10.0).epsilon(1e-12));
  REQUIRE(cut_profile(o, 1.0, y));
  CHECK(y == doctest::Approx(9.0).epsilon(1e-12));
  // W89's section is this profile, bit for bit.
  for (f64 d = -2.4; d <= 2.4; d += 0.07) {
    f64 a = 0, b = 0;
    const bool has_a = cut_section(10.0, 3.0, 0.6, 1.0, d, a);
    const bool has_b = cut_profile(CutProfile{10.0, 3.0, 0.0, 0.6, 1.0, 1.0, 0.6, false}, d, b);
    REQUIRE(has_a == has_b);
    if (has_a) CHECK(a == b);
  }
  // A face that runs out: where W89's ends (10.6 at 2.1 m) it goes on with the same slope, twice
  // the rise over the top half a run on, twenty rises at 0.95 of the run, and nothing past that.
  const CutProfile r{10.0, 3.0, 0.0, 0.6, 1.0, 1.0, 0.6, true};
  REQUIRE(cut_profile(r, 2.1, y));
  CHECK(y == doctest::Approx(10.6).epsilon(1e-12));
  REQUIRE(cut_profile(r, 2.1 + 1e-6, y));
  CHECK(y == doctest::Approx(10.6 + 1e-6).epsilon(1e-9));
  REQUIRE(cut_profile(r, 2.4, y));
  CHECK(y == doctest::Approx(10.0 + 2 * 0.6).epsilon(1e-9));
  REQUIRE(cut_profile(r, -2.1 - 0.56, y));
  CHECK(y > 10.0 + 8 * 0.6);
  CHECK_FALSE(cut_profile(r, 2.1 + 0.58, y));
  // The rounded minimum: the minimum outside the band, a quarter of the band under the crease,
  // and tangent to both at the band's edges.
  CHECK(cut_round(5.0, 9.0, 1.0) == 5.0);
  CHECK(cut_round(9.0, 5.0, 1.0) == 5.0);
  CHECK(cut_round(5.0, 5.0, 1.0) == doctest::Approx(4.75).epsilon(1e-12));
  CHECK(cut_round(5.0, 5.5, 1.0) == doctest::Approx(5.0 - 0.0625).epsilon(1e-12));
  CHECK(cut_round(5.0, 5.5, 0.0) == 5.0);
  CHECK(cut_round(5.0, 6.0, 1.0) == 5.0);
}

TEST_CASE(
    "tileset: a shaped cut's heights are its profile and its rounded top, to the millimetre") {
  // One straight segment across a ground with a slope and a swell, its width, depth and both bank
  // slopes changing along it, the top rounded over 0.75 of the depth.
  const Station a{600.0, 3000.0, 500.0, 0.4, 3.0}, b{900.0, 4200.0, 800.0, 2.5, 0.6};
  Shaped s("bend", WorldPos{6.0, 0.6, 14.0}, WorldPos{31.0, 0.9, 26.0}, 2);
  s.section([&](u32 k, f32& w, f32& d, f32& l, f32& r) {
    const Station& e = k == 0 ? a : b;
    w = static_cast<f32>(e.width / 1000.0);
    d = static_cast<f32>(e.depth / 1000.0);
    l = static_cast<f32>(e.left);
    r = static_cast<f32>(e.right);
  });
  s.cut.bank_round = 0.75f;
  // The numbers the build reads: the f32s widened, in millimetres.
  const auto station = [&](u32 k) {
    return Station{s.cut.points[k].y * 1000.0, static_cast<f64>(s.cut.width_m[k]) * 1000.0,
                   static_cast<f64>(s.cut.depth_m[k]) * 1000.0,
                   static_cast<f64>(s.cut.bank_slope_left[k]),
                   static_cast<f64>(s.cut.bank_slope_right[k])};
  };
  TerrainCuts cuts;
  std::string error;
  REQUIRE_MESSAGE(cuts.build({&s.cut, 1}, &error), error);
  const i64 n = 161;
  std::vector<i32> ground(static_cast<usize>(n * n)), cut(static_cast<usize>(n * n));
  for (i64 j = 0; j < n; ++j)
    for (i64 i = 0; i < n; ++i)
      ground[static_cast<usize>(j * n + i)] = swell_mm(i, j);
  cut = ground;
  const u64 lowered = cuts.apply(250, 0, 0, n, n, cut);
  u64 expected_lowered = 0, rounded = 0, capped = 0;
  for (i64 j = 0; j < n; ++j) {
    for (i64 i = 0; i < n; ++i) {
      const f64 x = static_cast<f64>(i * 250), z = static_cast<f64>(j * 250);
      const i32 g = ground[static_cast<usize>(j * n + i)];
      f64 surface = 0, k = 0;
      i32 expected = g;
      if (straight_surface(x, z, 6000.0, 14000.0, 31000.0, 26000.0, station(0), station(1), 0.75,
                           surface, k)) {
        const f64 v = cut_round(static_cast<f64>(g), surface, k);
        const f64 mm = std::floor(v + 0.5);
        if (mm < static_cast<f64>(g)) expected = static_cast<i32>(mm);
        if (v < std::min(static_cast<f64>(g), surface) - 1.0) ++rounded;
        const f64 raw = ((x - 6000.0) * 25000.0 + (z - 14000.0) * 12000.0) /
                        (25000.0 * 25000.0 + 12000.0 * 12000.0);
        if ((raw < 0.0 || raw > 1.0) && expected != g) ++capped;
      }
      // The build computes the projection by its own operations: a point within a millimetre of
      // the test's reading is the same reading.
      CHECK(std::abs(cut[static_cast<usize>(j * n + i)] - expected) <= 1);
      expected_lowered += expected != g;
    }
  }
  CHECK(lowered > 1500);
  CHECK(std::llabs(static_cast<i64>(lowered) - static_cast<i64>(expected_lowered)) <= 8);
  CHECK(rounded > 100);  // the rounded top lowered points the plain minimum would not have
  CHECK(capped > 10);    // and the ends are round caps
  // The same cut without the rounding has a crease: every point is the plain minimum.
  s.cut.bank_round = 0.f;
  TerrainCuts plain;
  REQUIRE(plain.build({&s.cut, 1}, &error));
  std::vector<i32> creased = ground;
  plain.apply(250, 0, 0, n, n, creased);
  u64 lower = 0;
  for (usize q = 0; q < creased.size(); ++q) {
    CHECK(cut[q] <= creased[q]);
    lower += cut[q] < creased[q];
  }
  CHECK(lower > 100);
  CHECK(cuts.hash() != plain.hash());
}

TEST_CASE("tileset: a cut is continuous across a joint, a junction and its two sides") {
  // A trunk of five points turning 25 degrees at each, every point with its own width, depth and
  // slopes (a bar on alternating sides), and a tributary joining its third point from the side.
  Shaped trunk("trunk", WorldPos{0, 0, 0}, WorldPos{1, 0, 0}, 5);
  trunk.cut.points = {WorldPos{4.0, 1.0, 20.0}, WorldPos{12.0, 0.96, 20.0},
                      WorldPos{19.25, 0.92, 23.38}, WorldPos{27.25, 0.88, 23.38},
                      WorldPos{34.5, 0.84, 20.0}};
  trunk.section([](u32 k, f32& w, f32& d, f32& l, f32& r) {
    w = 3.0f + 0.4f * static_cast<f32>(k % 2);
    d = 0.5f + 0.1f * static_cast<f32>(k % 3);
    l = k % 2 == 0 ? 0.3f : 3.0f;
    r = k % 2 == 0 ? 3.0f : 0.3f;
  });
  trunk.cut.bank_round = 0.75f;
  Shaped trib("trib", WorldPos{19.25, 1.1, 34.0}, WorldPos{19.25, 0.92, 23.38}, 4);
  trib.section([](u32 k, f32& w, f32& d, f32& l, f32& r) {
    w = 1.6f;
    d = 0.3f + 0.05f * static_cast<f32>(k);
    l = 1.0f + 0.5f * static_cast<f32>(k);
    r = 2.0f;
  });
  const scene::TerrainCut both[2] = {trunk.cut, trib.cut};
  TerrainCuts cuts;
  std::string error;
  REQUIRE_MESSAGE(cuts.build(both, &error), error);
  // What the cuts leave of a flat ground 1.15 m up — over every top, and under every face's own
  // top, as the ground a channel is cut into is — walked on a 20 mm lattice: no two neighbours,
  // along x or along z, differ by more than the steepest slope the surface has over the step,
  // plus a millimetre: 121 mm. The steepest face here is 3, steepened at most to 4; round a
  // line's end and on the outside of a joint the two sides' surfaces are blended by direction,
  // which is as steep as their difference over the distance to the end, and with faces that run
  // out that is 5 here. A step at a joint, a junction or the line would be a share of the cut's
  // depth, 300 mm and more.
  const f64 flat = 1150.0;
  const i64 step = 20;
  const f64 bound = 6.0 * static_cast<f64>(step) + 1.0;
  f64 worst = 0;
  i64 worst_x = 0, worst_z = 0;
  u64 pairs = 0, lowered = 0;
  const f64 none = std::numeric_limits<f64>::quiet_NaN();
  const usize columns = static_cast<usize>((37000 - 2000) / step + 1);
  std::vector<f64> north(columns, none), row(columns, none);
  for (i64 z = 14000; z <= 36000; z += step) {
    for (usize c = 0; c < columns; ++c) {
      f64 here = 0;
      const bool cut = cuts.lowered_mm(static_cast<f64>(2000 + static_cast<i64>(c) * step),
                                       static_cast<f64>(z), 1, flat, here);
      row[c] = cut ? here : flat;
      lowered += cut && here < flat - 1.0;
      const auto pair = [&](f64 other) {
        ++pairs;
        if (std::fabs(row[c] - other) <= worst) return;
        worst = std::fabs(row[c] - other);
        worst_x = 2000 + static_cast<i64>(c) * step;
        worst_z = z;
      };
      if (c > 0 && !std::isnan(row[c - 1])) pair(row[c - 1]);
      if (!std::isnan(north[c])) pair(north[c]);
    }
    north.swap(row);
  }
  CHECK(pairs > 3000000);
  CHECK(lowered > 300000);
  CHECK_MESSAGE(worst <= bound, "the largest step between neighbours 20 mm apart: "
                                    << worst << " mm, at (" << worst_x << ", " << worst_z << ")");
  // At the junction the surface is the lower of the two, whichever comes first.
  const scene::TerrainCut swapped[2] = {trib.cut, trunk.cut};
  TerrainCuts other;
  REQUIRE(other.build(swapped, &error));
  for (i64 z = 20000; z <= 27000; z += 130)
    for (i64 x = 16000; x <= 23000; x += 130) {
      f64 a = 0, b = 0, la = 0, lb = 0;
      const bool has_a = cuts.surface_mm(static_cast<f64>(x), static_cast<f64>(z), 1, a);
      const bool has_b = other.surface_mm(static_cast<f64>(x), static_cast<f64>(z), 1, b);
      REQUIRE(has_a == has_b);
      if (!has_a) continue;
      CHECK(a == b);
      REQUIRE(cuts.lowered_mm(static_cast<f64>(x), static_cast<f64>(z), 1, flat, la));
      REQUIRE(other.lowered_mm(static_cast<f64>(x), static_cast<f64>(z), 1, flat, lb));
      CHECK(la == lb);
      CHECK(la <= std::min(a, flat));
    }
}

TEST_CASE("tileset: the same cut from two windows is the same millimetres, in either order") {
  // Two crossing shaped cuts with rounded tops over the swell: the whole lattice at once, and two
  // windows of it that start elsewhere, hold the same heights wherever they overlap.
  Shaped a("a", WorldPos{3.0, 0.5, 11.0}, WorldPos{37.0, 0.7, 29.0}, 6);
  a.section([](u32 k, f32& w, f32& d, f32& l, f32& r) {
    w = 2.5f + 0.3f * static_cast<f32>(k);
    d = 0.6f;
    l = k % 2 == 0 ? 0.4f : 2.0f;
    r = k % 2 == 0 ? 2.0f : 0.4f;
  });
  a.cut.bank_round = 1.0f;
  Shaped b("b", WorldPos{22.0, 0.9, 2.0}, WorldPos{15.0, 0.2, 38.0}, 5);
  b.section([](u32, f32& w, f32& d, f32& l, f32& r) {
    w = 3.0f;
    d = 0.8f;
    l = 1.0f;
    r = 1.5f;
  });
  b.cut.bank_round = 0.5f;
  const scene::TerrainCut ab[2] = {a.cut, b.cut}, ba[2] = {b.cut, a.cut};
  TerrainCuts first, second;
  std::string error;
  REQUIRE(first.build(ab, &error));
  REQUIRE(second.build(ba, &error));
  const auto window = [&](const TerrainCuts& cuts, i64 i0, i64 j0, i64 nx, i64 nz) {
    std::vector<i32> v(static_cast<usize>(nx * nz));
    for (i64 j = 0; j < nz; ++j)
      for (i64 i = 0; i < nx; ++i)
        v[static_cast<usize>(j * nx + i)] = swell_mm(i0 + i, j0 + j);
    cuts.apply(250, i0, j0, nx, nz, v);
    return v;
  };
  const std::vector<i32> whole = window(first, 0, 0, 161, 161);
  const std::vector<i32> other_order = window(second, 0, 0, 161, 161);
  CHECK(whole == other_order);
  // Windows whose corners are not on a block of the whole's, one of them a single tile with its
  // apron.
  const i64 windows[2][4] = {{37, 50, 97, 111}, {63, 47, 18, 18}};
  for (const auto& w : windows) {
    const std::vector<i32> part = window(first, w[0], w[1], w[2], w[3]);
    u64 differ = 0, lowered = 0;
    for (i64 j = 0; j < w[3]; ++j)
      for (i64 i = 0; i < w[2]; ++i) {
        const i32 v = part[static_cast<usize>(j * w[2] + i)];
        differ += v != whole[static_cast<usize>((w[1] + j) * 161 + (w[0] + i))];
        lowered += v != swell_mm(w[0] + i, w[1] + j);
      }
    CHECK(differ == 0);
    CHECK(lowered > 50);
  }
}

TEST_CASE("tileset: a flood bed is a second profile, bounded by its scarps") {
  // A 2 m channel 0.4 m deep in a flood bed 0.1 m under the top, reaching 5 m to the left and 2 m
  // to the right, its scarps at 2 rising 0.5 m; the ground flat, 0.3 m over the top.
  scene::TerrainCut c =
      cut_of("flood", WorldPos{5.0, 0.0, 20.0}, WorldPos{35.0, 0.0, 20.0}, 2.f, 0.4f);
  c.flood_left_m = {5.f, 5.f};
  c.flood_right_m = {2.f, 2.f};
  c.flood_depth_m = {0.1f, 0.1f};
  c.scarp_m = {0.5f, 0.5f};
  c.scarp_slope = 2.f;
  TerrainCuts cuts;
  std::string error;
  REQUIRE_MESSAGE(cuts.build({&c, 1}, &error), error);
  const auto at = [&](f64 lateral_m, i64 cell, f64& v) {
    // The line runs along +x, so its left ((-dz, dx)) is +z.
    return cuts.surface_mm(20000.0, 20000.0 + lateral_m * 1000.0, cell, v);
  };
  const f64 bed = static_cast<f64>(-0.1f) * 1000.0;
  f64 v = 0;
  REQUIRE(at(0.0, 250, v));
  CHECK(v == doctest::Approx(-400.0).epsilon(1e-6));  // the channel's bed
  REQUIRE(at(3.0, 250, v));
  CHECK(v == doctest::Approx(bed).epsilon(1e-6));  // the flood bed, left
  REQUIRE(at(-1.8, 250, v));
  CHECK(v == doctest::Approx(bed).epsilon(1e-6));  // and right
  REQUIRE(at(5.1, 250, v));
  CHECK(v == doctest::Approx(bed + 200.0).epsilon(1e-6));  // 0.1 m up the left scarp at 2
  REQUIRE(at(-2.2, 250, v));
  CHECK(v == doctest::Approx(bed + 400.0).epsilon(1e-6));  // the right scarp
  // The scarp's face goes on a scarp's height past its top, then nothing: 0.5 m of run in all.
  REQUIRE(at(5.45, 250, v));
  CHECK(v == doctest::Approx(bed + 900.0).epsilon(1e-6));
  CHECK_FALSE(at(5.6, 250, v));
  CHECK_FALSE(at(-2.6, 250, v));
  // The cell rule reads each by its own width: a 4 m level holds the 7 m flood bed and not the
  // 2 m channel in it; an 8 m level neither.
  REQUIRE(at(0.0, 4000, v));
  CHECK(v == doctest::Approx(bed).epsilon(1e-6));
  CHECK_FALSE(at(0.0, 8000, v));
  // Built: the bed lowered to its level between the scarps, the ground past them untouched.
  Flat s;
  BuildOptions o = s.options;
  for (WorldPos& p : c.points)
    p.y = -0.3;  // the flat ground stands 0.3 m over the top
  REQUIRE_MESSAGE(o.cuts.build({&c, 1}, &error), error);
  const BuildResult built = s.build(o, "flood");
  TileSet set;
  REQUIRE_MESSAGE(set.open(built.manifest, error), error);
  const f64 level = -300.0 - static_cast<f64>(0.1f) * 1000.0;
  CHECK(set.point_mm(20000, 23000) == std::floor(level + 0.5));
  CHECK(set.point_mm(20000, 18250) == std::floor(level + 0.5));
  CHECK(set.point_mm(20000, 20000) == -700.0);
  CHECK(set.point_mm(20000, 26000) == 0.0);
  CHECK(set.point_mm(20000, 17000) == 0.0);
}

TEST_CASE("tileset: a shaped cut that is not one is refused with a sentence") {
  std::string error;
  TerrainCuts cuts;
  scene::TerrainCut c = cut_of("side", WorldPos{0, 0, 0}, WorldPos{1, 0, 0}, 1.0f, 0.2f);
  c.bank_slope_left = {1.f, 1.f};
  CHECK_FALSE(cuts.build({&c, 1}, &error));
  CHECK(error.find("\"side\" has a left and a right bank slope array") != std::string::npos);
  c.bank_slope_right = {1.f, 0.01f};
  CHECK_FALSE(cuts.build({&c, 1}, &error));
  CHECK(error.find("outside [0.02, 16]") != std::string::npos);
  c.bank_slope_right = {1.f, 16.f};
  CHECK(cuts.build({&c, 1}, &error));
  c.bank_round = 5.f;
  CHECK_FALSE(cuts.build({&c, 1}, &error));
  CHECK(error.find("rounding") != std::string::npos);
  c.bank_round = 0.f;
  c.flood_left_m = {2.f, 2.f};
  CHECK_FALSE(cuts.build({&c, 1}, &error));
  CHECK(error.find("flood bed arrays") != std::string::npos);
  c.flood_right_m = {2.f, 2.f};
  c.flood_depth_m = {0.f, 0.f};
  c.scarp_m = {0.3f, 0.f};
  CHECK_FALSE(cuts.build({&c, 1}, &error));
  CHECK(error.find("scarp is not positive") != std::string::npos);
  c.scarp_m = {0.3f, 0.3f};
  c.flood_left_m = {2.f, -1.f};
  CHECK_FALSE(cuts.build({&c, 1}, &error));
  CHECK(error.find("negative") != std::string::npos);
  c.flood_left_m = {2.f, 2.f};
  c.scarp_slope = 0.1f;
  CHECK_FALSE(cuts.build({&c, 1}, &error));
  CHECK(error.find("scarp slope") != std::string::npos);
  c.scarp_slope = 1.5f;
  CHECK(cuts.build({&c, 1}, &error));
  CHECK(cuts.cut_count() == 1);
  // A cut that names none of W93's fields hashes as one that never had them: the fields add to
  // the hash only where they are named.
  const scene::TerrainCut plain = cut_of("side", WorldPos{0, 0, 0}, WorldPos{1, 0, 0}, 1.0f, 0.2f);
  scene::TerrainCut defaults = plain;
  defaults.scarp_slope = 3.f;  // read only with a flood bed
  TerrainCuts x, y;
  REQUIRE(x.build({&plain, 1}, &error));
  REQUIRE(y.build({&defaults, 1}, &error));
  CHECK(x.hash() == y.hash());
  CHECK(x.hash() != cuts.hash());
}

TEST_CASE("tileset: the provider stands on the cut ground, and refuses cuts beside a built set") {
  test::TempDir tmp{"engine_tileset_cut_provider"};
  const Heightfield field = synth_heightfield(SynthKind::flat, 161, 250);
  std::string error;
  REQUIRE_MESSAGE(write_heightfield(tmp.file("flat.json"), field, error), error);
  scene::Terrain entry;
  entry.provider = "tiles";
  entry.tiles = scene::TerrainTileSet{};
  entry.tiles->heightfield = "flat.json";
  entry.tiles->ddc = "ddc";
  entry.tiles->cells = 16;
  entry.tiles->ring_spacing_mm = 1000;
  entry.tiles->far_ratio = 1;
  entry.tiles->far_levels = 2;
  entry.tiles->cuts.push_back(
      cut_of("run", WorldPos{5.0, 0.0, 20.0}, WorldPos{35.0, 0.0, 20.0}, 3.0f, 0.6f));
  scene_gen::Context context;
  const std::string dir = tmp.path();
  context.dir = dir;
  const scene_gen::GroundProviderDesc* desc =
      scene_gen::GeneratorRegistry::global().find_ground("tiles");
  REQUIRE(desc != nullptr);
  scene_gen::GroundProvider ground;
  REQUIRE_MESSAGE(desc->make(entry, context, ground, &error), error);
  CHECK(ground.view().surface_mm(20000, 20000) == static_cast<f64>(-0.6f));  // metres, as f32
  CHECK(ground.view().surface_mm(20000, 25000) == 0.0);
  // A set built ahead is not built here, so it cannot be cut.
  entry.tiles->heightfield.clear();
  entry.tiles->manifest = "set/manifest.json";
  CHECK_FALSE(desc->make(entry, context, ground, &error));
  CHECK(error.find("cuts besides") != std::string::npos);
  // A cut that is not one is refused before anything is built.
  entry.tiles->manifest.clear();
  entry.tiles->heightfield = "flat.json";
  entry.tiles->cuts[0].bank_slope = 0.f;
  CHECK_FALSE(desc->make(entry, context, ground, &error));
  CHECK(error.find("cannot cut its ground") != std::string::npos);
}
