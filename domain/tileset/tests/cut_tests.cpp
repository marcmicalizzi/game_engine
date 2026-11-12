// Channels cut into the ground (cuts.h; docs/subsystems/tileset.md, "Channels cut into the
// ground"; roadmap W89): the section against its formula, the finest level cut and a level whose
// spacing is not finer than the width keeping its heights, the tiles no cut reaches unmoved, a set
// without cuts keyed as it was, and the provider standing on the cut ground.
#include <core/hash/hash.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/tileset/cuts.h>
#include <domain/tileset/tileset.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <schemas/scene.h>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::tileset;

namespace {

scene::TerrainEdit cut_of(std::string name, WorldPos a, WorldPos b, f32 width, f32 depth,
                          f32 slope = 1.0f) {
  scene::TerrainEdit c;
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

TEST_CASE("tileset: a cut's section is the trapezoid its formula gives, and its faces run out") {
  f64 y = 0;
  // 3 m wide, 0.6 m deep, banks at 45 degrees: a flat bed 1.8 m wide, the top at 1.5 m.
  const CutProfile p{10.0, 3.0, 0.0, 0.6, 1.0, 1.0, 0.6};
  REQUIRE(cut_profile(p, 0.0, y));
  CHECK(y == doctest::Approx(9.4).epsilon(1e-12));
  REQUIRE(cut_profile(p, 0.9, y));
  CHECK(y == doctest::Approx(9.4).epsilon(1e-12));
  REQUIRE(cut_profile(p, -1.2, y));
  CHECK(y == doctest::Approx(9.7).epsilon(1e-12));
  REQUIRE(cut_profile(p, 1.5, y));
  CHECK(y == doctest::Approx(10.0).epsilon(1e-12));
  // Past the top the face goes on up for a depth, to 2.1 m, and then runs out over 0.6 m more,
  // steepening: 0.6 / (1 - 1/6) over the top a sixth of that run on, and nothing past 0.95 of it.
  REQUIRE(cut_profile(p, 2.0, y));
  CHECK(y == doctest::Approx(10.5).epsilon(1e-12));
  REQUIRE(cut_profile(p, 2.2, y));
  CHECK(y == doctest::Approx(10.72).epsilon(1e-9));
  CHECK_FALSE(cut_profile(p, 2.7, y));
  // A bed narrower than the banks allow is a V: 1 m wide and 1 m deep steepens the banks to 2.
  const CutProfile v{10.0, 1.0, 0.0, 1.0, 1.0, 1.0, 1.0};
  REQUIRE(cut_profile(v, 0.0, y));
  CHECK(y == doctest::Approx(9.0).epsilon(1e-12));
  REQUIRE(cut_profile(v, 0.25, y));
  CHECK(y == doctest::Approx(9.5).epsilon(1e-12));
}

TEST_CASE("tileset: a cut lowers the finest level to its section and leaves the rest as it was") {
  Flat s;
  const BuildResult plain = s.build(s.options, "plain");
  BuildOptions o = s.options;
  // A 3 m channel along z = 20 m from x = 5 to 35 m, 0.6 m deep, its top on the flat ground.
  const scene::TerrainEdit cut =
      cut_of("run", WorldPos{5.0, 0.0, 20.0}, WorldPos{35.0, 0.0, 20.0}, 3.0f, 0.6f);
  std::string error;
  REQUIRE_MESSAGE(o.edits.build({&cut, 1}, &error), error);
  const BuildResult cut_set = s.build(o, "cut");
  CHECK(cut_set.key != plain.key);
  CHECK(cut_set.edited_points > 0);

  TileSet a, b;
  REQUIRE_MESSAGE(a.open(plain.manifest, error), error);
  REQUIRE_MESSAGE(b.open(cut_set.manifest, error), error);
  // Every finest point: the section where the cut reaches, joined to the ground by the rounded
  // minimum over 0.75 of the depth (a cut that names no rounding), rounded once to the millimetre,
  // else the ground; and a finest tile no cut reaches holds the same millimetres it held without
  // the cut.
  const i64 sp = 250;
  const f64 depth_mm =
      static_cast<f64>(0.6f) * 1000.0;  // the depth the build reads: the f32 widened
  const f64 reach = 1.5 + 2 * 0.6;      // half the width, and the face's two runs past it
  u32 edited_points = 0, untouched_tiles = 0, touched_tiles = 0;
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
          if (cut_profile(CutProfile{0.0, 3000.0, 0.0, depth_mm, 1.0, 1.0, depth_mm}, d_mm,
                          section))
            expected =
                std::min(ground, std::floor(cut_round(ground, section, 0.75 * depth_mm) + 0.5));
          CHECK(b.point_mm(i * sp, j * sp) == expected);
          if (expected != ground) {
            ++edited_points;
            tile_touched = true;
          }
          if (d > reach + 0.25) CHECK(b.point_mm(i * sp, j * sp) == ground);
        }
      }
      ++(tile_touched ? touched_tiles : untouched_tiles);
    }
  }
  CHECK(edited_points > 1000);
  CHECK(touched_tiles >= 8);
  CHECK(untouched_tiles >= 60);
  // The bed under the line, and the top at the bank's edge rounded down by a quarter of the band:
  // 450 / 4 = 112.5 mm and the f32's excess over 0.6, so 113 to the millimetre.
  CHECK(b.point_mm(20000, 20000) == -600.0);
  CHECK(b.point_mm(20000, 21500) == -113.0);

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
  const scene::TerrainEdit rill =
      cut_of("rill", WorldPos{5.0, 0.0, 20.0}, WorldPos{35.0, 0.0, 20.0}, 0.2f, 0.1f);
  std::string error;
  REQUIRE_MESSAGE(o.edits.build({&rill, 1}, &error), error);
  const BuildResult closed = s.build(o, "rill");
  CHECK(closed.edited_points == 0);
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
  REQUIRE(empty.edits.build({}, &error));
  CHECK(empty.edits.empty());
  CHECK(tileset_key(7, none) == tileset_key(7, empty));
  // Two crossing channels, built in either order: the same heights to the byte (the lowest of the
  // sections, whichever came first), though the key, which names the order, differs.
  const scene::TerrainEdit a =
      cut_of("a", WorldPos{5.0, 0.0, 20.0}, WorldPos{35.0, 0.0, 20.0}, 3.0f, 0.6f);
  const scene::TerrainEdit b =
      cut_of("b", WorldPos{20.0, 0.2, 5.0}, WorldPos{20.0, -0.1, 35.0}, 2.0f, 0.9f, 2.0f);
  BuildOptions ab = s.options, ba = s.options;
  const scene::TerrainEdit first[2] = {a, b};
  const scene::TerrainEdit second[2] = {b, a};
  REQUIRE(ab.edits.build(first, &error));
  REQUIRE(ba.edits.build(second, &error));
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
  TerrainEdits cuts;
  scene::TerrainEdit one = cut_of("lone", WorldPos{0, 0, 0}, WorldPos{1, 0, 0}, 1.0f, 0.2f);
  one.points.resize(1);
  one.width_m.resize(1);
  one.depth_m.resize(1);
  CHECK_FALSE(cuts.build({&one, 1}, &error));
  CHECK(error.find("\"lone\" has fewer than two points") != std::string::npos);
  scene::TerrainEdit uneven = cut_of("uneven", WorldPos{0, 0, 0}, WorldPos{1, 0, 0}, 1.0f, 0.2f);
  uneven.depth_m.push_back(0.3f);
  CHECK_FALSE(cuts.build({&uneven, 1}, &error));
  CHECK(error.find("length") != std::string::npos);
  scene::TerrainEdit flat = cut_of("flat", WorldPos{0, 0, 0}, WorldPos{1, 0, 0}, 1.0f, 0.0f);
  CHECK_FALSE(cuts.build({&flat, 1}, &error));
  CHECK(error.find("positive") != std::string::npos);
  scene::TerrainEdit sheer = cut_of("sheer", WorldPos{0, 0, 0}, WorldPos{1, 0, 0}, 1.0f, 0.2f, 9.f);
  CHECK_FALSE(cuts.build({&sheer, 1}, &error));
  CHECK(error.find("bank slope") != std::string::npos);
  CHECK(cuts.empty());
}

// ---- W93: the profile across a line, left and right, rounded, with a flood bed ------------------

namespace {

// A cut of `n` points along a line from `a` to `b`, with every W93 field per point.
struct Shaped {
  scene::TerrainEdit cut;
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
  k = round * p.depth;
  if (raw >= 0.0 && raw <= 1.0) return cut_profile(p, lateral >= 0.0 ? d : -d, surface);
  f64 l = 0, r = 0;
  const bool has_l = cut_profile(p, d, l), has_r = cut_profile(p, -d, r);
  if (!has_l && !has_r) return false;
  // A face that runs out ends twenty rises over the top (0.95 of its second run).
  const f64 end = 20.0 * p.rise;
  if (!has_l) l = p.top + end;
  if (!has_r) r = p.top + end;
  const f64 share = d > 0.0 ? std::clamp(0.5 + 0.5 * lateral / d, 0.0, 1.0) : 0.5;
  surface = r + share * (l - r);
  return true;
}

}  // namespace

TEST_CASE("tileset: a profile differs left from right, steepens to a V and takes an offset") {
  // 4 m wide, 1 m deep, the left bank a point bar at 0.5, the right a cut bank at 4: the flat bed
  // runs from the bar's toe on the line (2 / 0.5 = 2 m of run) to 0.25 m short of the right top.
  const CutProfile p{10.0, 4.0, 0.0, 1.0, 0.5, 4.0, 1.0};
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
  // Each face goes on a rise past its top at its own slope, 2 m more on the left and 0.25 m on
  // the right, and then runs out over as much again (0.95 of it), steepening.
  REQUIRE(cut_profile(p, 3.9, y));
  CHECK(y == doctest::Approx(10.95).epsilon(1e-12));
  REQUIRE(cut_profile(p, 4.1, y));
  CHECK(y == doctest::Approx(10.0 + 1.0 / 0.95).epsilon(1e-9));
  CHECK_FALSE(cut_profile(p, 6.0, y));
  REQUIRE(cut_profile(p, -2.2, y));
  CHECK(y == doctest::Approx(10.8).epsilon(1e-12));
  REQUIRE(cut_profile(p, -2.3, y));
  CHECK(y == doctest::Approx(11.25).epsilon(1e-9));
  CHECK_FALSE(cut_profile(p, -2.5, y));
  // Banks too gentle to reach the bed inside the width are steepened together to a V: 0.5 and 1
  // would need 3 m of run for 1 m of depth in a 2 m width, so both are 1.5 times as steep and
  // meet 1 m down, two thirds of the width from the gentler bank's top.
  const CutProfile v{10.0, 2.0, 0.0, 1.0, 0.5, 1.0, 1.0};
  REQUIRE(cut_profile(v, 1.0 - 4.0 / 3.0, y));
  CHECK(y == doctest::Approx(9.0).epsilon(1e-9));
  REQUIRE(cut_profile(v, 0.5, y));
  CHECK(y == doctest::Approx(10.0 - 0.75 * 0.5).epsilon(1e-9));
  // An offset moves the whole section off the line.
  const CutProfile o{10.0, 4.0, 1.0, 1.0, 1.0, 1.0, 1.0};
  REQUIRE(cut_profile(o, 3.0, y));
  CHECK(y == doctest::Approx(10.0).epsilon(1e-12));
  REQUIRE(cut_profile(o, -1.0, y));
  CHECK(y == doctest::Approx(10.0).epsilon(1e-12));
  REQUIRE(cut_profile(o, 1.0, y));
  CHECK(y == doctest::Approx(9.0).epsilon(1e-12));
  // A face that runs out: where W89's ends (10.6 at 2.1 m) it goes on with the same slope, twice
  // the rise over the top half a run on, twenty rises at 0.95 of the run, and nothing past that.
  const CutProfile r{10.0, 3.0, 0.0, 0.6, 1.0, 1.0, 0.6};
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
  TerrainEdits cuts;
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
  const scene::TerrainEdit both[2] = {trunk.cut, trib.cut};
  TerrainEdits cuts;
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
      const bool cut = cuts.edited_mm(static_cast<f64>(2000 + static_cast<i64>(c) * step),
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
  const scene::TerrainEdit swapped[2] = {trib.cut, trunk.cut};
  TerrainEdits other;
  REQUIRE(other.build(swapped, &error));
  for (i64 z = 20000; z <= 27000; z += 130)
    for (i64 x = 16000; x <= 23000; x += 130) {
      f64 a = 0, b = 0, la = 0, lb = 0;
      const bool has_a = cuts.surface_mm(static_cast<f64>(x), static_cast<f64>(z), 1, a);
      const bool has_b = other.surface_mm(static_cast<f64>(x), static_cast<f64>(z), 1, b);
      REQUIRE(has_a == has_b);
      if (!has_a) continue;
      CHECK(a == b);
      REQUIRE(cuts.edited_mm(static_cast<f64>(x), static_cast<f64>(z), 1, flat, la));
      REQUIRE(other.edited_mm(static_cast<f64>(x), static_cast<f64>(z), 1, flat, lb));
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
  const scene::TerrainEdit ab[2] = {a.cut, b.cut}, ba[2] = {b.cut, a.cut};
  TerrainEdits first, second;
  std::string error;
  REQUIRE(first.build(ab, &error));
  REQUIRE(second.build(ba, &error));
  const auto window = [&](const TerrainEdits& cuts, i64 i0, i64 j0, i64 nx, i64 nz) {
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
  // **The same bytes from every toolchain**: the lattice's heights after the two cuts hash to a
  // value taken on MSVC (2026-11-10) that GCC and Clang reproduce in the containers. The ground
  // is `swell_mm`'s, whose sine and cosine are the C library's, so it is pinned too: were a
  // library to round one of them otherwise, the first pin says so before the second is blamed.
  std::vector<i32> ground(static_cast<usize>(161 * 161));
  for (i64 j = 0; j < 161; ++j)
    for (i64 i = 0; i < 161; ++i)
      ground[static_cast<usize>(j * 161 + i)] = swell_mm(i, j);
  const u64 ground_hash = hash_bytes(ground.data(), ground.size() * sizeof(i32));
  const u64 cut_hash = hash_bytes(whole.data(), whole.size() * sizeof(i32));
  CHECK(ground_hash == 3593206589486382013ull);
  CHECK(cut_hash == 13997414741782788461ull);
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
  scene::TerrainEdit c =
      cut_of("flood", WorldPos{5.0, 0.0, 20.0}, WorldPos{35.0, 0.0, 20.0}, 2.f, 0.4f);
  c.flood_left_m = {5.f, 5.f};
  c.flood_right_m = {2.f, 2.f};
  c.flood_depth_m = {0.1f, 0.1f};
  c.scarp_m = {0.5f, 0.5f};
  c.scarp_slope = 2.f;
  TerrainEdits cuts;
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
  // The scarp's face goes on a scarp's height past its top (0.5 m of run in all, to 5.5 m on the
  // left and 2.5 m on the right), runs out over 0.95 of a quarter metre more, and then nothing.
  REQUIRE(at(5.45, 250, v));
  CHECK(v == doctest::Approx(bed + 900.0).epsilon(1e-6));
  CHECK(at(5.6, 250, v));
  CHECK(at(-2.6, 250, v));
  CHECK_FALSE(at(5.8, 250, v));
  CHECK_FALSE(at(-2.8, 250, v));
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
  REQUIRE_MESSAGE(o.edits.build({&c, 1}, &error), error);
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
  TerrainEdits cuts;
  scene::TerrainEdit c = cut_of("side", WorldPos{0, 0, 0}, WorldPos{1, 0, 0}, 1.0f, 0.2f);
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
  // No rounding is not a shape a cut can ask for: the crease, and the face that ends in a wall.
  c.bank_round = 0.f;
  CHECK_FALSE(cuts.build({&c, 1}, &error));
  CHECK(error.find("rounding outside [0.1, 1]") != std::string::npos);
  c.bank_round = 0.75f;
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
  CHECK(cuts.edit_count() == 1);
  // **Every field enters the hash** (W98: an edit's canonical bytes), so a scarp's slope with no
  // flood bed is another key though it moves no height; until W98 the hash named the fields one by
  // one and a field it did not name was the same key (W93's test held that, for this case).
  const scene::TerrainEdit plain = cut_of("side", WorldPos{0, 0, 0}, WorldPos{1, 0, 0}, 1.0f, 0.2f);
  scene::TerrainEdit defaults = plain;
  defaults.scarp_slope = 3.f;
  TerrainEdits x, y;
  REQUIRE(x.build({&plain, 1}, &error));
  REQUIRE(y.build({&defaults, 1}, &error));
  CHECK(x.hash() != y.hash());
  CHECK(x.hash() != cuts.hash());
  TerrainEdits again;
  REQUIRE(again.build({&plain, 1}, &error));
  CHECK(again.hash() == x.hash());
}

// ---- W98: edits that raise as well as lower, lines and patches ----------------------------------

namespace {

scene::TerrainEdit line_of(scene::TerrainEditMode mode, std::string name, WorldPos a, WorldPos b,
                           f32 width) {
  scene::TerrainEdit e;
  e.name = std::move(name);
  e.mode = mode;
  e.points = {a, b};
  e.width_m = {width, width};
  return e;
}

// A square patch of side `side` metres centred at (x, z), its outline at height `y`.
scene::TerrainEdit square_of(scene::TerrainEditMode mode, std::string name, f64 x, f64 z, f64 side,
                             f64 y) {
  scene::TerrainEdit e;
  e.name = std::move(name);
  e.mode = mode;
  const f64 h = 0.5 * side;
  e.outline = {WorldPos{x - h, y, z - h}, WorldPos{x + h, y, z - h}, WorldPos{x + h, y, z + h},
               WorldPos{x - h, y, z + h}};
  return e;
}

// A hollow: the ground falls to 1.5 m below its rim at (20, 20) m over a 12 m radius, a cosine
// bowl in whole millimetres, from the deterministic series (no C library).
i32 hollow_mm(i64 i, i64 j) {
  const f64 dx = static_cast<f64>(i) * 0.25 - 20.0, dz = static_cast<f64>(j) * 0.25 - 20.0;
  const f64 r2 = dx * dx + dz * dz;
  if (r2 >= 144.0) return 0;
  // 1.5 (1 - r^2 / 144)^2: a smooth bowl, zero slope at the rim and the centre.
  const f64 u = 1.0 - r2 / 144.0;
  return static_cast<i32>(std::floor(-1500.0 * u * u + 0.5));
}

std::vector<i32> lattice(i32 (*ground)(i64, i64), i64 i0, i64 j0, i64 nx, i64 nz) {
  std::vector<i32> v(static_cast<usize>(nx * nz));
  for (i64 j = 0; j < nz; ++j)
    for (i64 i = 0; i < nx; ++i)
      v[static_cast<usize>(j * nx + i)] = ground(i0 + i, j0 + j);
  return v;
}

}  // namespace

TEST_CASE("tileset: a fill is flat across its width, its faces fall at their slope and run out") {
  // A 4 m crest at 1 m along +x, faces at one on two falling 2 m and then running out.
  const scene::TerrainEdit e = line_of(scene::TerrainEditMode::Fill, "berm",
                                       WorldPos{0.0, 1.0, 0.0}, WorldPos{40.0, 1.0, 0.0}, 4.0f);
  TerrainEdits edits;
  std::string error;
  REQUIRE_MESSAGE(edits.build({&e, 1}, &error), error);
  CHECK(edits.raises());
  f64 v = 0;
  const auto at = [&](f64 lateral_m) { return edits.fill_mm(20000.0, lateral_m * 1000.0, 250, v); };
  REQUIRE(at(0.0));
  CHECK(v == 1000.0);
  REQUIRE(at(-1.9));
  CHECK(v == 1000.0);
  REQUIRE(at(3.0));  // a metre past the crest at 0.5: half a metre down
  CHECK(v == doctest::Approx(500.0).epsilon(1e-12));
  REQUIRE(at(-6.0));  // 4 m past it: the fill's whole 2 m
  CHECK(v == doctest::Approx(-1000.0).epsilon(1e-12));
  REQUIRE(at(8.0));  // half way through the run-out: twice the drop
  CHECK(v == doctest::Approx(1000.0 - 4000.0).epsilon(1e-9));
  CHECK_FALSE(at(10.0));  // past 0.95 of the run-out
  // No cut: the upper envelope is nowhere.
  CHECK_FALSE(edits.surface_mm(20000.0, 0.0, 250, v));
  // What it leaves of a ground: raised where below, untouched where above; on the crest exact, a
  // fill's rounding stopping at its top, and where a face meets the ground rounded over the least
  // of 0.75 of the drop and the face's own drop there (half a metre, 1 m past the crest).
  REQUIRE(edits.edited_mm(20000.0, 0.0, 250, -3000.0, v));
  CHECK(v == 1000.0);
  REQUIRE(edits.edited_mm(20000.0, 0.0, 250, 2500.0, v));
  CHECK(v == 2500.0);
  REQUIRE(edits.edited_mm(20000.0, 0.0, 250, 1000.0, v));
  CHECK(v == 1000.0);
  REQUIRE(edits.edited_mm(20000.0, 3000.0, 250, 500.0, v));
  CHECK(v == doctest::Approx(500.0 + 500.0 / 4).epsilon(1e-12));
  CHECK(fill_round(5.0, 5.0, 1.0) == doctest::Approx(5.25).epsilon(1e-12));
  CHECK(fill_round(5.0, 9.0, 1.0) == 9.0);
  CHECK(fill_round(9.0, 5.0, 0.0) == 9.0);
  // The face against its formula.
  f64 h = 0;
  REQUIRE(face_height(1.0, 0.5, 2.0, h));
  CHECK(h == 0.5);
  REQUIRE(face_height(6.0, 0.5, 2.0, h));
  CHECK(h == doctest::Approx(4.0).epsilon(1e-12));
  CHECK_FALSE(face_height(7.9, 0.5, 2.0, h));
}

TEST_CASE("tileset: a bench is flat across its tread, cut on the uphill side and filled below") {
  // A cross-slope of one in four rising to +z, and a tread 3 m wide along +x at its height at
  // z = 20 m, its cut face at 1 and its fill face at 0.5.
  const auto slope_mm = [](i64, i64 j) { return static_cast<i32>(j * 250 / 4) - 5000; };
  scene::TerrainEdit tread = line_of(scene::TerrainEditMode::Bench, "tread",
                                     WorldPos{2.0, 0.0, 20.0}, WorldPos{38.0, 0.0, 20.0}, 3.0f);
  TerrainEdits edits;
  std::string error;
  REQUIRE_MESSAGE(edits.build({&tread, 1}, &error), error);
  const i64 n = 161;
  std::vector<i32> v(static_cast<usize>(n * n));
  for (i64 j = 0; j < n; ++j)
    for (i64 i = 0; i < n; ++i)
      v[static_cast<usize>(j * n + i)] = slope_mm(i, j);
  const std::vector<i32> ground = v;
  CHECK(edits.apply(250, 0, 0, n, n, v) > 1000);
  u64 tread_points = 0, cut = 0, filled = 0;
  for (i64 j = 0; j < n; ++j) {
    for (i64 i = 24; i <= 136; ++i) {  // x 6 to 34 m, clear of the caps
      const i32 g = ground[static_cast<usize>(j * n + i)];
      const i32 e = v[static_cast<usize>(j * n + i)];
      const f64 lateral = static_cast<f64>(j) * 0.25 - 20.0;
      if (std::fabs(lateral) < 1.5) {
        // **The tread is flat to the millimetre** where it crosses the old ground's height too:
        // a bench's rounding stops at its tread.
        CHECK(e == 0);
        ++tread_points;
      } else if (lateral > 0.0) {
        CHECK(e <= g);  // uphill: cut, never raised
        CHECK(e >= 0);
        cut += e < g;
      } else {
        CHECK(e >= g);  // downhill: filled, never cut
        CHECK(e <= 0);
        filled += e > g;
      }
    }
  }
  CHECK(tread_points == 11 * 113);
  CHECK(cut > 500);
  CHECK(filled > 500);
  // Past the faces the slope is the slope: 9 m uphill (the cut face reaches 2 m over the tread in
  // 2 m of run and runs out within 2 m more) and 12 m down (the fill's 2 m drop in 4 m, then 4).
  CHECK(v[static_cast<usize>((80 + 36) * n + 80)] ==
        ground[static_cast<usize>((80 + 36) * n + 80)]);
  CHECK(v[static_cast<usize>((80 - 48) * n + 80)] ==
        ground[static_cast<usize>((80 - 48) * n + 80)]);
}

TEST_CASE("tileset: a patch is its surface, flat or a grid, feathered into the ground") {
  std::string error;
  // A pad 6 m square at 0.5 m over a ground at 0, its fill faces at 0.5, rounded over a tenth of
  // their 2 m drop: inside, the pad; 1 m past its edge, 0.5 m down the face: the ground.
  scene::TerrainEdit pad = square_of(scene::TerrainEditMode::Fill, "pad", 20.0, 20.0, 6.0, 0.5);
  pad.bank_round = 0.1f;
  // A deposit: a grid of heights over a 4 m square, rising 1 mm a millimetre... a ramp of 250 mm a
  // point along x, laid 2 m up over a ground at 0.
  scene::TerrainEdit deposit =
      square_of(scene::TerrainEditMode::Fill, "deposit", 10.0, 30.0, 4.0, 0.0);
  deposit.bank_round = 0.1f;
  deposit.grid_origin = WorldPos{8.0, 2.0, 28.0};
  deposit.grid_spacing_mm = 1000;
  deposit.grid_nx = 5;
  deposit.grid_nz = 5;
  for (i32 j = 0; j < 5; ++j)
    for (i32 i = 0; i < 5; ++i)
      deposit.grid_mm.push_back(250 * i + 10 * j);
  const scene::TerrainEdit both[2] = {pad, deposit};
  TerrainEdits edits;
  REQUIRE_MESSAGE(edits.build(both, &error), error);
  CHECK(edits.patch_count() == 2);
  f64 v = 0;
  REQUIRE(edits.edited_mm(20000.0, 20000.0, 250, 0.0, v));
  CHECK(v == 500.0);
  REQUIRE(edits.edited_mm(22900.0, 17100.0, 250, 0.0, v));
  CHECK(v == 500.0);
  REQUIRE(edits.edited_mm(24000.0, 20000.0, 250, -2000.0, v));  // 1 m out: 0.5 m down the face
  CHECK(v == doctest::Approx(0.0).epsilon(1e-12));
  REQUIRE(edits.edited_mm(20000.0, 20000.0, 250, 900.0, v));  // ground over the pad: kept
  CHECK(v == 900.0);
  // The deposit's grid: bilinear inside, from 2 m up.
  REQUIRE(edits.edited_mm(9500.0, 29250.0, 250, 0.0, v));
  CHECK(v == doctest::Approx(2000.0 + 250.0 * 1.5 + 10.0 * 1.25).epsilon(1e-12));
  // Past the grid's edge the surface is held at the edge: 0.5 m outside the square at x = 12.5,
  // a quarter metre down a face at 0.5 from the edge's height.
  REQUIRE(edits.edited_mm(12500.0, 30000.0, 250, 0.0, v));
  CHECK(v == doctest::Approx(2000.0 + 1000.0 + 20.0 - 250.0).epsilon(1e-12));
  // A bench pad on a slope: flat across the pad wherever the old ground crossed its height.
  scene::TerrainEdit bench =
      square_of(scene::TerrainEditMode::Bench, "level", 20.0, 20.0, 6.0, 0.0);
  TerrainEdits level;
  REQUIRE_MESSAGE(level.build({&bench, 1}, &error), error);
  for (const f64 g : {-1500.0, -10.0, 0.0, 10.0, 1500.0}) {
    REQUIRE(level.edited_mm(19000.0, 21000.0, 250, g, v));
    CHECK(v == 0.0);
  }
  // The patch's width the cell rule reads: four times its area over its perimeter, its side.
  CHECK(level.edited_mm(20000.0, 20000.0, 4000, 300.0, v));
  CHECK_FALSE(level.edited_mm(20000.0, 20000.0, 8000, 300.0, v));
}

TEST_CASE("tileset: raises and lowers compose the same in any order and from any window") {
  // Over the hollow: a causeway benched level across it at the rim's height, a channel cut across
  // the causeway (a cut wins: it is a gap in the causeway), a deposit lying in the hollow's side
  // and a pad cut and filled level beside it.
  scene::TerrainEdit causeway = line_of(scene::TerrainEditMode::Bench, "causeway",
                                        WorldPos{4.0, 0.0, 20.0}, WorldPos{36.0, 0.0, 20.0}, 3.0f);
  causeway.fill_slope = 0.6f;
  scene::TerrainEdit channel = line_of(scene::TerrainEditMode::Cut, "channel",
                                       WorldPos{20.0, -0.4, 6.0}, WorldPos{21.0, -0.6, 34.0}, 2.0f);
  channel.depth_m = {0.5f, 0.5f};
  scene::TerrainEdit deposit =
      square_of(scene::TerrainEditMode::Fill, "deposit", 14.0, 27.0, 5.0, 0.0);
  deposit.grid_origin = WorldPos{11.5, -1.2, 24.5};
  deposit.grid_spacing_mm = 1250;
  deposit.grid_nx = 5;
  deposit.grid_nz = 5;
  for (i32 j = 0; j < 5; ++j)
    for (i32 i = 0; i < 5; ++i)
      deposit.grid_mm.push_back(300 * ((i + 2 * j) % 3) + 100 * i);
  scene::TerrainEdit pad = square_of(scene::TerrainEditMode::Bench, "pad", 27.0, 12.0, 4.0, -0.7);
  pad.fill_m = 1.0f;
  const scene::TerrainEdit forward[4] = {causeway, channel, deposit, pad};
  const scene::TerrainEdit backward[4] = {pad, deposit, channel, causeway};
  TerrainEdits first, second;
  std::string error;
  REQUIRE_MESSAGE(first.build(forward, &error), error);
  REQUIRE_MESSAGE(second.build(backward, &error), error);
  std::vector<i32> whole = lattice(hollow_mm, 0, 0, 161, 161);
  std::vector<i32> other = whole;
  const std::vector<i32> ground = whole;
  const u64 changed = first.apply(250, 0, 0, 161, 161, whole);
  CHECK(second.apply(250, 0, 0, 161, 161, other) == changed);
  CHECK(whole == other);
  u64 raised = 0, lowered = 0;
  for (usize k = 0; k < whole.size(); ++k) {
    raised += whole[k] > ground[k];
    lowered += whole[k] < ground[k];
  }
  CHECK(raised > 2000);
  CHECK(lowered > 500);
  // The causeway's crest across the hollow, at the rim's height, where the hollow was 1 m deep.
  const auto at = [&](f64 x_m, f64 z_m) {
    return whole[static_cast<usize>(std::lround(z_m * 4) * 161 + std::lround(x_m * 4))];
  };
  CHECK(at(14.0, 20.0) == 0);
  CHECK(at(26.0, 20.0) == 0);
  // The channel cuts through it: its bed half a metre under its own top there.
  CHECK(at(20.5, 20.0) < -800);
  // **Tile-stable**: windows that start off the blocks, one a single 16-cell tile with its apron,
  // hold the same millimetres wherever they overlap the whole.
  const i64 windows[3][4] = {{37, 50, 97, 111}, {63, 47, 19, 19}, {0, 79, 161, 3}};
  for (const auto& w : windows) {
    std::vector<i32> part = lattice(hollow_mm, w[0], w[1], w[2], w[3]);
    second.apply(250, w[0], w[1], w[2], w[3], part);
    u64 differ = 0;
    for (i64 j = 0; j < w[3]; ++j)
      for (i64 i = 0; i < w[2]; ++i)
        differ += part[static_cast<usize>(j * w[2] + i)] !=
                  whole[static_cast<usize>((w[1] + j) * 161 + (w[0] + i))];
    CHECK(differ == 0);
  }
  // **Across a tile's border** the edited ground is continuous: no two neighbours 25 cm apart
  // differ by more than the steepest face over the step, plus a millimetre: the faces are at 1 or
  // gentler, and the steepest run-out is the channel's where its face meets ground 0.6 m over a top
  // whose face holds 0.5 m, a slope of 1 / (1 - 1/6)^2 = 1.44, so 1.5 x 250 + 1 = 376 mm. A seam
  // step would be the edit's own height, a metre and more (measured 250 mm on MSVC, 2026-11-10).
  i32 worst = 0;
  for (i64 j = 0; j < 161; ++j)
    for (i64 i = 1; i < 161; ++i)
      worst = std::max(worst, std::abs(whole[static_cast<usize>(j * 161 + i)] -
                                       whole[static_cast<usize>(j * 161 + i - 1)]));
  MESSAGE("the largest step between neighbours 25 cm apart: " << worst << " mm");
  CHECK(worst <= 376);
  // **The same bytes from every toolchain**: the hollow (integer arithmetic and one division) and
  // the lattice after the four edits, taken on MSVC and reproduced by Clang and GCC.
  const u64 ground_hash = hash_bytes(ground.data(), ground.size() * sizeof(i32));
  const u64 edit_hash = hash_bytes(whole.data(), whole.size() * sizeof(i32));
  MESSAGE("hollow " << ground_hash << ", edited " << edit_hash);
  CHECK(ground_hash == 17149333333107072769ull);
  CHECK(edit_hash == 16557965784987386551ull);
}

TEST_CASE("tileset: a fill closes on a level as a cut does, and the far levels keep their bytes") {
  Flat s;
  const BuildResult plain = s.build(s.options, "plain");
  BuildOptions o = s.options;
  // A berm 3 m wide 0.8 m up along z = 20 m, and a 6 m pad 0.4 m up at (10, 32).
  scene::TerrainEdit berm = line_of(scene::TerrainEditMode::Fill, "berm", WorldPos{5.0, 0.8, 20.0},
                                    WorldPos{35.0, 0.8, 20.0}, 3.0f);
  scene::TerrainEdit pad = square_of(scene::TerrainEditMode::Fill, "pad", 10.0, 32.0, 6.0, 0.4);
  const scene::TerrainEdit both[2] = {berm, pad};
  std::string error;
  REQUIRE_MESSAGE(o.edits.build(both, &error), error);
  const BuildResult built = s.build(o, "fill");
  CHECK(built.edited_points > 1000);
  TileSet a, b;
  REQUIRE_MESSAGE(a.open(plain.manifest, error), error);
  REQUIRE_MESSAGE(b.open(built.manifest, error), error);
  CHECK(b.point_mm(20000, 20000) == 800.0);
  CHECK(b.point_mm(10000, 32000) == 400.0);
  CHECK(b.point_mm(20000, 27000) == 0.0);
  const TileSetManifest& pm = a.manifest();
  const TileSetManifest& bm = b.manifest();
  REQUIRE(bm.levels.size() == 5);
  CHECK(bm.levels[1].hash != pm.levels[1].hash);  // 1 m: both
  CHECK(bm.levels[2].hash != pm.levels[2].hash);  // 2 m: both
  CHECK(bm.levels[3].hash != pm.levels[3].hash);  // 4 m: the pad, not the berm
  CHECK(bm.levels[4].hash == pm.levels[4].hash);  // 8 m: neither
  // The 4 m level holds the pad and not the berm.
  CHECK(b.manifest().levels[3].spacing_mm == 4000);
}

TEST_CASE("tileset: an edit that is not one is refused with a sentence") {
  std::string error;
  TerrainEdits edits;
  const auto refused = [&](const scene::TerrainEdit& e, const char* words) {
    CHECK_FALSE(edits.build({&e, 1}, &error));
    CHECK_MESSAGE(error.find(words) != std::string::npos, error);
    CHECK(edits.empty());
  };
  scene::TerrainEdit none;
  none.name = "none";
  refused(none, "\"none\" names neither");
  scene::TerrainEdit both = square_of(scene::TerrainEditMode::Fill, "both", 0, 0, 2, 0);
  both.points = {WorldPos{0, 0, 0}, WorldPos{1, 0, 0}};
  refused(both, "names both");
  scene::TerrainEdit deep =
      line_of(scene::TerrainEditMode::Bench, "deep", WorldPos{0, 0, 0}, WorldPos{1, 0, 0}, 1.0f);
  deep.depth_m = {0.2f, 0.2f};
  refused(deep, "names a depth");
  deep.depth_m = {0.0f, 0.0f};
  CHECK(edits.build({&deep, 1}, &error));
  scene::TerrainEdit steep = deep;
  steep.fill_slope = 9.0f;
  refused(steep, "fill slope outside [0.1, 8]");
  steep.fill_slope = 0.5f;
  steep.fill_m = 0.0f;
  refused(steep, "fill height");
  steep.fill_m = 2.0f;
  steep.fill_slope_left = {0.5f, 0.5f};
  refused(steep, "fill slope array");
  steep.fill_slope_right = {0.5f, 0.01f};
  refused(steep, "fill slope outside [0.02, 16]");
  scene::TerrainEdit flood = deep;
  flood.flood_left_m = {1.f, 1.f};
  flood.flood_right_m = {1.f, 1.f};
  flood.flood_depth_m = {0.f, 0.f};
  flood.scarp_m = {0.2f, 0.2f};
  refused(flood, "flood bed and is not a cut");
  scene::TerrainEdit thin = square_of(scene::TerrainEditMode::Fill, "thin", 0, 0, 2, 0);
  thin.outline.resize(2);
  refused(thin, "fewer than three points");
  scene::TerrainEdit line_ish = square_of(scene::TerrainEditMode::Fill, "line", 0, 0, 2, 0);
  for (WorldPos& p : line_ish.outline)
    p.z = 0.0;
  refused(line_ish, "no area");
  scene::TerrainEdit tilted = square_of(scene::TerrainEditMode::Fill, "tilted", 0, 0, 2, 0);
  tilted.outline[2].y = 0.5;
  refused(tilted, "not all at one height");
  scene::TerrainEdit gridded = square_of(scene::TerrainEditMode::Fill, "grid", 0, 0, 2, 0);
  gridded.grid_nx = 2;
  gridded.grid_nz = 2;
  gridded.grid_spacing_mm = 1000;
  gridded.grid_mm = {0, 0, 0};
  refused(gridded, "heights are not its points'");
  gridded.grid_mm.push_back(0);
  gridded.grid_spacing_mm = 0;
  refused(gridded, "spacing is not positive");
  gridded.grid_spacing_mm = 1000;
  CHECK(edits.build({&gridded, 1}, &error));
  scene::TerrainEdit arrays = square_of(scene::TerrainEditMode::Fill, "arrays", 0, 0, 2, 0);
  arrays.width_m = {1.f};
  refused(arrays, "is a patch and names a line's arrays");
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
  entry.tiles->edits.push_back(
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
  CHECK(error.find("edits besides") != std::string::npos);
  // A cut that is not one is refused before anything is built.
  entry.tiles->manifest.clear();
  entry.tiles->heightfield = "flat.json";
  entry.tiles->edits[0].bank_slope = 0.f;
  CHECK_FALSE(desc->make(entry, context, ground, &error));
  CHECK(error.find("cannot edit its ground") != std::string::npos);
}

// **What edits cost a tile** (W98; tileset.md, "Performance notes"): 32 edits — 16 benched treads
// and 16 gridded deposits — over a 1,025-point lattice at 25 cm (256 m, 256 tiles of 64 cells),
// against the same lattice with 16 cuts alone and with none. Prints the milliseconds; asserts only
// that the edits did their work. A number from this case is the build's own, in the preset it was
// built in, and states the machine's state beside it in the report that quotes it.
TEST_CASE("tileset: what edits cost a tile") {
  Vector<scene::TerrainEdit> edits, cuts_only;
  for (i32 k = 0; k < 16; ++k) {
    const f64 z = 8.0 + 15.0 * static_cast<f64>(k);
    scene::TerrainEdit tread = line_of(scene::TerrainEditMode::Bench, "tread",
                                       WorldPos{4.0, 0.0, z}, WorldPos{250.0, 0.0, z + 6.0}, 2.0f);
    edits.push_back(tread);
    scene::TerrainEdit cut = tread;
    cut.mode = scene::TerrainEditMode::Cut;
    cut.depth_m = {0.5f, 0.5f};
    cuts_only.push_back(cut);
    scene::TerrainEdit deposit = square_of(scene::TerrainEditMode::Fill, "deposit",
                                           20.0 + 14.0 * static_cast<f64>(k), z + 4.0, 6.0, 0.0);
    deposit.grid_origin = WorldPos{17.0 + 14.0 * static_cast<f64>(k), 0.3, z + 1.0};
    deposit.grid_spacing_mm = 1000;
    deposit.grid_nx = 7;
    deposit.grid_nz = 7;
    for (i32 q = 0; q < 49; ++q)
      deposit.grid_mm.push_back(100 * (q % 5));
    edits.push_back(deposit);
  }
  TerrainEdits all, cut;
  std::string error;
  REQUIRE_MESSAGE(all.build({edits.data(), edits.size()}, &error), error);
  REQUIRE_MESSAGE(cut.build({cuts_only.data(), cuts_only.size()}, &error), error);
  const i64 n = 1025;
  const auto ground = [](i64 i, i64 j) {
    return static_cast<i32>((i * 37 + j * 91) % 400) - 200;  // a rough plane of integers
  };
  const auto time = [&](const TerrainEdits& e, u64& changed) {
    std::vector<i32> v(static_cast<usize>(n * n));
    for (i64 j = 0; j < n; ++j)
      for (i64 i = 0; i < n; ++i)
        v[static_cast<usize>(j * n + i)] = ground(i, j);
    const auto t0 = std::chrono::steady_clock::now();
    changed = e.apply(250, 0, 0, n, n, v);
    return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
  };
  u64 changed_all = 0, changed_cut = 0, changed_none = 0;
  const f64 ms_all = time(all, changed_all);
  const f64 ms_cut = time(cut, changed_cut);
  const f64 ms_none = time(TerrainEdits{}, changed_none);
  MESSAGE("32 edits over 256 tiles: " << ms_all << " ms (" << ms_all / 256.0 << " a tile, "
                                      << changed_all << " points); 16 cuts alone: " << ms_cut
                                      << " ms; none: " << ms_none << " ms");
  CHECK(changed_all > 100000);
  CHECK(changed_cut > 50000);
  CHECK(changed_none == 0);
}
