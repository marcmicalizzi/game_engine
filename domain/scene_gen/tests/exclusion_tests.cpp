// Where nothing is placed (exclusion.h; docs/subsystems/scene_gen.md, "Exclusion"; roadmap G135):
// the query over a hole, a channel and a body -- inside, at the edge, outside, and the distance --
// and the answer of a scene with none.
#include <domain/scene_gen/scene_gen.h>

#include <doctest/doctest.h>

#include <cmath>

using namespace engine;
using namespace engine::scene_gen;

namespace {

scene::TerrainHole circle_hole(f64 x, f64 z, f32 radius) {
  scene::TerrainHole h;
  h.center = WorldPos{x, 0.0, z};
  h.radius_m = radius;
  return h;
}

// A straight channel along +x from (x0, 0) to (x1, 0), `width` metres wide throughout.
scene::WaterChannel straight_channel(f64 x0, f64 x1, f32 width) {
  scene::WaterChannel c;
  c.points.push_back(WorldPos{x0, 0.0, 0.0});
  c.points.push_back(WorldPos{x1, 0.0, 0.0});
  c.width_m.push_back(width);
  c.width_m.push_back(width);
  c.depth_m.push_back(0.5f);
  c.depth_m.push_back(0.5f);
  c.slope.push_back(0.01f);
  c.slope.push_back(0.01f);
  return c;
}

scene::WaterBody circle_body(f64 x, f64 z, f32 radius, f64 level) {
  scene::WaterBody b;
  b.level_m = level;
  scene::WaterCircle c;
  c.center = WorldPos{x, level, z};
  c.radius_m = radius;
  b.shape.circle = c;
  return b;
}

// A ground at a constant height, in metres.
f64 flat_ground(const void* context, i64, i64) noexcept {
  return *static_cast<const f64*>(context);
}
Ground ground_at(const f64& height) {
  Ground g;
  g.surface_fn = &flat_ground;
  g.context = &height;
  return g;
}

i64 mm(f64 metres) { return static_cast<i64>(std::llround(metres * 1000.0)); }

}  // namespace

TEST_CASE("exclusion: a scene with none excludes nothing and leaves every density alone") {
  Exclusion none;
  REQUIRE(none.build({}, {}, {}));
  CHECK(none.empty());
  const f64 h = -5.0;
  const Ground ground = ground_at(h);
  CHECK_FALSE(none.excluded_mm(ground, 0, 0));
  CHECK(none.riparian_factor_mm(0, 0) == 1.0);
  // A context with no exclusion answers the same, with no object behind it.
  Context context;
  context.ground = ground;
  CHECK_FALSE(context.excluded_mm(0, 0));
  CHECK(context.riparian_factor_mm(0, 0) == 1.0);
}

TEST_CASE("exclusion: a 6 m hole excludes its inside and not its rim") {
  Exclusion ex;
  const scene::TerrainHole hole = circle_hole(10.0, -4.0, 3.0f);
  REQUIRE(ex.build({&hole, 1}, {}, {}));
  CHECK_FALSE(ex.empty());
  CHECK(ex.hole_count() == 1u);
  const f64 h = 0.0;
  const Ground ground = ground_at(h);
  CHECK(ex.excluded_mm(ground, mm(10.0), mm(-4.0)));
  CHECK(ex.excluded_mm(ground, mm(12.9), mm(-4.0)));
  CHECK_FALSE(ex.excluded_mm(ground, mm(13.0), mm(-4.0)));  // the rim is ground
  CHECK_FALSE(ex.excluded_mm(ground, mm(13.5), mm(-4.0)));
  CHECK_FALSE(ex.excluded_mm(ground, mm(0.0), mm(0.0)));
  // A caller that asks only for the other kinds is not told about the hole.
  CHECK_FALSE(ex.excluded_mm(ground, mm(10.0), mm(-4.0), k_exclude_channels | k_exclude_water));
  // A hole is not a bank: the density a bank leaves is untouched.
  CHECK(ex.riparian_factor_mm(mm(10.0), mm(-4.0)) == 1.0);
}

TEST_CASE("exclusion: a 3 m channel excludes its bed and banks, and thins the grass beyond them") {
  Exclusion ex;
  const scene::WaterChannel channel = straight_channel(0.0, 40.0, 3.0f);
  REQUIRE(ex.build({}, {}, {&channel, 1}));
  CHECK(ex.segment_count() == 1u);
  const f64 h = 0.0;
  const Ground ground = ground_at(h);
  // The bank's outer edge: half the width + 0.25 w + 0.4 m = 1.5 + 0.75 + 0.4 = 2.65 m.
  CHECK(ex.excluded_mm(ground, mm(20.0), 0));
  CHECK(ex.excluded_mm(ground, mm(20.0), mm(1.5)));   // the water's edge
  CHECK(ex.excluded_mm(ground, mm(20.0), mm(-2.6)));  // on the bank
  CHECK_FALSE(ex.excluded_mm(ground, mm(20.0), mm(2.7)));
  CHECK_FALSE(ex.excluded_mm(ground, mm(20.0), mm(-30.0)));
  // Past the ends, the cap is round: 2.65 m from the end point.
  CHECK(ex.excluded_mm(ground, mm(-2.6), 0));
  CHECK_FALSE(ex.excluded_mm(ground, mm(-2.7), 0));
  CHECK_FALSE(ex.excluded_mm(ground, mm(20.0), 0, k_exclude_holes | k_exclude_water));
  // The distance to the bank's edge, and the density factor along it: the floor at the edge,
  // rising monotonically to 1 at the riparian reach and staying there.
  CHECK(ex.channel_distance_mm(mm(20.0), 0) == 0.0);
  CHECK(ex.channel_distance_mm(mm(20.0), mm(2.65 + 1.0)) == doctest::Approx(1.0).epsilon(1e-9));
  CHECK(ex.channel_distance_mm(mm(20.0), mm(40.0)) > 1.0e20);
  CHECK(ex.riparian_factor_mm(mm(20.0), mm(2.65)) == doctest::Approx(k_riparian_floor));
  f64 last = 0.0;
  for (i64 z = 2650; z <= 2650 + 3200; z += 100) {
    const f64 f = ex.riparian_factor_mm(mm(20.0), z);
    CHECK(f >= last);
    CHECK(f <= 1.0);
    last = f;
  }
  CHECK(ex.riparian_factor_mm(mm(20.0), mm(2.65 + k_riparian_reach_m)) == 1.0);
  CHECK(ex.riparian_factor_mm(mm(20.0), mm(50.0)) == 1.0);
  // The width follows the line: a channel narrowing from 4 m to 1 m is 3.1 m out at the wide end
  // (2 + 1 + 0.4 = 3.4 m) and 1.3 m at the narrow one (0.5 + 0.25 + 0.4 = 1.15 m).
  scene::WaterChannel taper = straight_channel(100.0, 140.0, 4.0f);
  taper.width_m[1] = 1.0f;
  Exclusion t;
  REQUIRE(t.build({}, {}, {&taper, 1}));
  CHECK(t.excluded_mm(ground, mm(100.0), mm(3.3)));
  CHECK_FALSE(t.excluded_mm(ground, mm(100.0), mm(3.5)));
  CHECK(t.excluded_mm(ground, mm(140.0), mm(1.1)));
  CHECK_FALSE(t.excluded_mm(ground, mm(140.0), mm(1.2)));
}

TEST_CASE("exclusion: a body excludes the points of its outline whose ground is below its level") {
  const scene::WaterBody lake = circle_body(100.0, 100.0, 20.0f, 2.0);
  Exclusion ex;
  REQUIRE(ex.build({}, {&lake, 1}, {}));
  CHECK(ex.body_count() == 1u);
  const f64 low = 0.5;   // the ground under the water
  const f64 high = 3.0;  // the ground above its level: a dry basin
  CHECK(ex.excluded_mm(ground_at(low), mm(100.0), mm(100.0)));
  CHECK(ex.excluded_mm(ground_at(low), mm(119.0), mm(100.0)));
  CHECK_FALSE(ex.excluded_mm(ground_at(low), mm(121.0), mm(100.0)));  // outside the outline
  CHECK_FALSE(ex.excluded_mm(ground_at(high), mm(100.0), mm(100.0)));
  CHECK_FALSE(
      ex.excluded_mm(ground_at(low), mm(100.0), mm(100.0), k_exclude_holes | k_exclude_channels));

  // A polygon, in either winding: the same square, so the same answer.
  scene::WaterBody square;
  square.level_m = 2.0;
  for (const auto& p :
       {WorldPos{0, 2, 0}, WorldPos{10, 2, 0}, WorldPos{10, 2, 10}, WorldPos{0, 2, 10}})
    square.shape.polygon.push_back(p);
  scene::WaterBody reversed = square;
  for (u32 i = 0; i < 4; ++i)
    reversed.shape.polygon[i] = square.shape.polygon[3 - i];
  for (const scene::WaterBody* body : {&square, &reversed}) {
    Exclusion p;
    REQUIRE(p.build({}, {body, 1}, {}));
    CHECK(p.excluded_mm(ground_at(low), mm(5.0), mm(5.0)));
    CHECK_FALSE(p.excluded_mm(ground_at(low), mm(11.0), mm(5.0)));
    CHECK_FALSE(p.excluded_mm(ground_at(low), mm(5.0), mm(-1.0)));
  }

  // A body that fills: the box its flood may reach, where the ground is below the level.
  scene::WaterBody filled;
  filled.level_m = 2.0;
  scene::WaterFill fill;
  fill.seed = WorldPos{50.0, 2.0, 50.0};
  fill.max_extent_m = 30.0f;
  filled.shape.fill = fill;
  Exclusion f;
  REQUIRE(f.build({}, {&filled, 1}, {}));
  CHECK(f.excluded_mm(ground_at(low), mm(70.0), mm(50.0)));
  CHECK_FALSE(f.excluded_mm(ground_at(low), mm(90.0), mm(50.0)));
  CHECK_FALSE(f.excluded_mm(ground_at(high), mm(70.0), mm(50.0)));

  // Open water goes on wherever the ground is below the level.
  scene::WaterBody sea = lake;
  sea.open = true;
  Exclusion o;
  REQUIRE(o.build({}, {&sea, 1}, {}));
  CHECK(o.excluded_mm(ground_at(low), mm(5000.0), mm(-5000.0)));
  CHECK_FALSE(o.excluded_mm(ground_at(high), mm(5000.0), mm(-5000.0)));
}

TEST_CASE("exclusion: the same scene answers the same, whichever way it was built") {
  const scene::TerrainHole hole = circle_hole(30.0, 30.0, 5.0f);
  const scene::WaterChannel a = straight_channel(0.0, 80.0, 3.0f);
  const scene::WaterChannel b = straight_channel(10.0, 20.0, 1.0f);
  const scene::WaterChannel both[2] = {a, b};
  const scene::WaterChannel swapped[2] = {b, a};
  Exclusion one, two;
  REQUIRE(one.build({&hole, 1}, {}, {both, 2}));
  REQUIRE(two.build({&hole, 1}, {}, {swapped, 2}));
  const f64 h = 0.0;
  const Ground ground = ground_at(h);
  u32 excluded = 0;
  for (i64 x = -10000; x <= 90000; x += 700)
    for (i64 z = -12000; z <= 40000; z += 650) {
      const bool e = one.excluded_mm(ground, x, z);
      CHECK(e == two.excluded_mm(ground, x, z));
      CHECK(one.riparian_factor_mm(x, z) == two.riparian_factor_mm(x, z));
      excluded += e ? 1u : 0u;
    }
  CHECK(excluded > 100u);
}
