// Where nothing is placed (exclusion.h; docs/subsystems/scene_gen.md, "Exclusion"; roadmap G135):
// the query over a hole, a channel and a body -- inside, at the edge, outside, and the distance --
// and the answer of a scene with none.
#include <domain/scene_gen/scene_gen.h>

#include <doctest/doctest.h>

#include <cmath>
#include <string>

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

// W93 (which folds W92 in): the corridor is the cut's. A cut's bank tops stand at half the width,
// inside the dressed bank's edge, so a channel without a flood bed is excluded as it was; a flood
// bed reaches further, on the side it reaches, and the description hands its edge over.
TEST_CASE("exclusion: a channel's flood bed is left bare on the side it reaches") {
  scene::WaterChannel channel = straight_channel(0.0, 40.0, 3.0f);
  // The line runs along +x, so its left ((-dz, dx)) is +z: 6 m of bed on the left, 1 m on the
  // right, which is inside the bank's 2.65 m and changes nothing there.
  for (u32 i = 0; i < 2; ++i) {
    channel.flood_left_m.push_back(6.0f);
    channel.flood_right_m.push_back(1.0f);
  }
  Exclusion ex;
  REQUIRE(ex.build({}, {}, {&channel, 1}));
  const f64 h = 0.0;
  const Ground ground = ground_at(h);
  CHECK(ex.excluded_mm(ground, mm(20.0), mm(5.9)));         // on the bed
  CHECK_FALSE(ex.excluded_mm(ground, mm(20.0), mm(6.1)));   // past its edge
  CHECK(ex.excluded_mm(ground, mm(20.0), mm(-2.6)));        // the right bank, as before
  CHECK_FALSE(ex.excluded_mm(ground, mm(20.0), mm(-2.7)));  // and not 5.9 m out on that side
  CHECK_FALSE(ex.excluded_mm(ground, mm(20.0), mm(-5.9)));
  // The riparian distance is measured from the bed's edge.
  CHECK(ex.channel_distance_mm(mm(20.0), mm(7.0)) == doctest::Approx(1.0).epsilon(1e-9));
  CHECK(ex.riparian_factor_mm(mm(20.0), mm(6.0)) == doctest::Approx(k_riparian_floor));
  CHECK(ex.riparian_factor_mm(mm(20.0), mm(6.0 + k_riparian_reach_m)) == 1.0);
  // The reach follows the line: 6 m at one end and none at the other is 3 m half way, which the
  // bank's 2.65 m is inside of.
  scene::WaterChannel closing = straight_channel(0.0, 40.0, 3.0f);
  closing.flood_left_m.push_back(6.0f);
  closing.flood_left_m.push_back(0.0f);
  closing.flood_right_m.push_back(0.0f);
  closing.flood_right_m.push_back(0.0f);
  Exclusion taper;
  REQUIRE(taper.build({}, {}, {&closing, 1}));
  CHECK(taper.excluded_mm(ground, mm(20.0), mm(2.9)));
  CHECK_FALSE(taper.excluded_mm(ground, mm(20.0), mm(3.1)));
  CHECK_FALSE(taper.excluded_mm(ground, mm(36.0), mm(2.7)));
  // Arrays that are not the points' long, and a number that is not one, are no flood bed: the
  // channel is excluded exactly as one that names none.
  const scene::WaterChannel plain = straight_channel(0.0, 40.0, 3.0f);
  scene::WaterChannel odd = plain;
  odd.flood_left_m.push_back(6.0f);
  odd.flood_right_m.push_back(6.0f);
  scene::WaterChannel bad = plain;
  for (u32 i = 0; i < 2; ++i) {
    bad.flood_left_m.push_back(-4.0f);
    bad.flood_right_m.push_back(std::numeric_limits<f32>::quiet_NaN());
  }
  Exclusion a, b, c;
  REQUIRE(a.build({}, {}, {&plain, 1}));
  REQUIRE(b.build({}, {}, {&odd, 1}));
  REQUIRE(c.build({}, {}, {&bad, 1}));
  for (i64 z = -9000; z <= 9000; z += 50) {
    const bool e = a.excluded_mm(ground, mm(20.0), z);
    CHECK(b.excluded_mm(ground, mm(20.0), z) == e);
    CHECK(c.excluded_mm(ground, mm(20.0), z) == e);
    CHECK(b.riparian_factor_mm(mm(20.0), z) == a.riparian_factor_mm(mm(20.0), z));
    CHECK(c.riparian_factor_mm(mm(20.0), z) == a.riparian_factor_mm(mm(20.0), z));
  }
}

// The one definition of a bank's edge, asked out to a reach the caller gives (W93, for G144's
// site): inside the riparian reach it is `channel_distance_mm` to the bit, past it the same
// distance goes on, and past the caller's reach there is no channel.
TEST_CASE("exclusion: the distance to a bank's edge out to any reach is the riparian one's") {
  // Two channels: a 3 m one along +x with 6 m of flood bed on its left, and a narrowing one 60 m
  // off that turns across it.
  scene::WaterChannel a = straight_channel(0.0, 40.0, 3.0f);
  for (u32 i = 0; i < 2; ++i) {
    a.flood_left_m.push_back(6.0f);
    a.flood_right_m.push_back(0.0f);
  }
  scene::WaterChannel b;
  b.points = {WorldPos{-30.0, 0.0, 60.0}, WorldPos{10.0, 0.0, 70.0}, WorldPos{50.0, 0.0, 55.0}};
  b.width_m = {4.0f, 2.0f, 1.0f};
  b.depth_m = {0.5f, 0.5f, 0.5f};
  b.slope = {0.01f, 0.01f, 0.01f};
  const scene::WaterChannel both[2] = {a, b};
  Exclusion ex;
  REQUIRE(ex.build({}, {}, both));
  u32 inside = 0, near = 0, far = 0;
  for (i64 z = -20000; z <= 90000; z += 370) {
    for (i64 x = -50000; x <= 70000; x += 410) {
      const f64 riparian = ex.channel_distance_mm(x, z);
      CHECK(ex.channel_edge_distance_mm(x, z, k_riparian_reach_m) == riparian);
      const f64 wide = ex.channel_edge_distance_mm(x, z, 128.0);
      // Every point of this box is within 128 m of an edge.
      REQUIRE(wide < 128.0);
      if (riparian < 1e20) {
        CHECK(wide == riparian);
        ++(riparian == 0.0 ? inside : near);
      } else {
        CHECK(wide >= k_riparian_reach_m);
        ++far;
        // A shorter reach than the distance finds nothing; a longer one the same distance.
        CHECK(ex.channel_edge_distance_mm(x, z, wide * 0.99) > 1e20);
        CHECK(ex.channel_edge_distance_mm(x, z, wide + 1.0) == wide);
      }
    }
  }
  CHECK(inside > 100);
  CHECK(near > 100);
  CHECK(far > 10000);
  // By hand: 20 m to the left of the first channel's line is 14 m from its flood bed's edge, and
  // 20 m to its right 17.35 m from its bank's (1.5 + 0.75 + 0.4 = 2.65 m out).
  CHECK(ex.channel_edge_distance_mm(mm(20.0), mm(20.0), 128.0) == doctest::Approx(14.0));
  CHECK(ex.channel_edge_distance_mm(mm(20.0), mm(-20.0), 128.0) == doctest::Approx(17.35));
  CHECK(ex.channel_edge_distance_mm(mm(20.0), mm(-20.0), 0.0) > 1e20);
  Exclusion none;
  REQUIRE(none.build({}, {}, {}));
  CHECK(none.channel_edge_distance_mm(0, 0, 128.0) > 1e20);
}

// ---- W98: footprints as shapes ------------------------------------------------------------------

TEST_CASE("exclusion: a band footprint excludes its band and nothing else") {
  // A wall's foot: 12 m along +x from (2, 5) m, 0.4 m either side of its line, its ends round.
  const Footprint wall = Footprint::band(mm(2.0), mm(5.0), mm(14.0), mm(5.0), mm(0.4));
  // Its edge, metres: inside negative, outside positive, round past the ends.
  CHECK(wall.edge_m(mm(8.0), mm(5.0)) == doctest::Approx(-0.4).epsilon(1e-12));
  CHECK(wall.edge_m(mm(8.0), mm(5.399)) == doctest::Approx(-0.001).epsilon(1e-9));
  CHECK(wall.edge_m(mm(8.0), mm(6.4)) == doctest::Approx(1.0).epsilon(1e-12));
  CHECK(wall.edge_m(mm(14.6), mm(5.0)) == doctest::Approx(0.2).epsilon(1e-12));
  CHECK(wall.covers_mm(mm(8.0), mm(4.6)));
  CHECK_FALSE(wall.covers_mm(mm(8.0), mm(4.599)));
  // Its disc holds the whole band, a millimetre over: a reader that knows only discs keeps clear.
  CHECK(wall.x_mm == mm(8.0));
  CHECK(wall.z_mm == mm(5.0));
  CHECK(wall.radius == doctest::Approx(6.401).epsilon(1e-12));
  for (const f64 x : {2.0, 8.0, 14.0, 14.4})
    for (const f64 z : {4.6, 5.0, 5.4}) {
      if (!wall.covers_mm(mm(x), mm(z))) continue;
      const f64 dx = x - 8.0, dz = z - 5.0;
      CHECK(dx * dx + dz * dz < wall.radius * wall.radius);
    }

  Exclusion ex;
  REQUIRE(ex.build({}, {}, {}));
  std::string error;
  REQUIRE_MESSAGE(ex.add_footprints({&wall, 1}, &error), error);
  CHECK(ex.footprint_count() == 1);
  CHECK_FALSE(ex.empty());
  const f64 h = 0.0;
  const Ground ground = ground_at(h);
  // **The sward grows to the wall's foot**: a point a millimetre past the band is free, where the
  // wall's disc of 6.4 m held everything back by its radius (the sward adds half a patch's
  // diagonal to an earlier footprint's radius, so 1.6 m and more for a 4 m wall's disc).
  for (const f64 x : {2.5, 8.0, 13.5}) {
    CHECK(ex.excluded_mm(ground, mm(x), mm(5.4)));
    CHECK(ex.excluded_mm(ground, mm(x), mm(4.6)));
    CHECK_FALSE(ex.excluded_mm(ground, mm(x), mm(5.401)));
    CHECK_FALSE(ex.excluded_mm(ground, mm(x), mm(4.599)));
  }
  // Every generator asks with every kind by default; one that asks without the footprints'
  // kind is not refused by them.
  CHECK_FALSE(ex.excluded_mm(ground, mm(8.0), mm(5.0), k_exclude_all & ~k_exclude_footprints));
  // How far to the edge, within a reach, and nothing past it.
  CHECK(ex.footprint_distance_mm(mm(8.0), mm(6.4), 3.0) == doctest::Approx(1.0).epsilon(1e-12));
  CHECK(ex.footprint_distance_mm(mm(8.0), mm(5.0), 3.0) == 0.0);
  CHECK(ex.footprint_distance_mm(mm(8.0), mm(9.0), 3.0) > 1e20);
  CHECK(ex.footprint_distance_mm(mm(8.0), mm(9.0), 4.0) == doctest::Approx(3.6).epsilon(1e-12));
  // The channels, holes and water still answer as they did.
  CHECK(ex.channel_distance_mm(mm(8.0), mm(5.0)) > 1e20);
  // `build` clears the footprints.
  REQUIRE(ex.build({}, {}, {}));
  CHECK(ex.empty());
  CHECK_FALSE(ex.excluded_mm(ground, mm(8.0), mm(5.0)));
}

TEST_CASE("exclusion: a polygon footprint excludes its inside, a notch included, by its edge") {
  // An L: a 10 m square at the origin with its north-east quarter missing, a yard round a house.
  const FootprintPoint l[6] = {{0, 0},       {10000, 0},    {10000, 5000},
                               {5000, 5000}, {5000, 10000}, {0, 10000}};
  const Footprint yard = Footprint::polygon(l);
  CHECK(yard.covers_mm(2000, 2000));
  CHECK(yard.covers_mm(8000, 2000));
  CHECK(yard.covers_mm(2000, 8000));
  CHECK_FALSE(yard.covers_mm(8000, 8000));  // the notch
  CHECK(yard.edge_m(8000, 8000) == doctest::Approx(3.0).epsilon(1e-12));
  CHECK(yard.edge_m(2000, 2000) == doctest::Approx(-2.0).epsilon(1e-12));
  CHECK(yard.edge_m(-1000, 5000) == doctest::Approx(1.0).epsilon(1e-12));
  // Either turn is the same polygon (to the last bits of a projection taken from the other end).
  const FootprintPoint reversed[6] = {l[5], l[4], l[3], l[2], l[1], l[0]};
  const Footprint other = Footprint::polygon(reversed);
  for (i64 z = -2000; z <= 12000; z += 700)
    for (i64 x = -2000; x <= 12000; x += 700)
      CHECK(std::fabs(other.edge_m(x, z) - yard.edge_m(x, z)) < 1e-9);
  Exclusion ex;
  REQUIRE(ex.build({}, {}, {}));
  std::string error;
  {
    // The exclusion copies the points: the footprint may go once it is added.
    const FootprintPoint copy[6] = {l[0], l[1], l[2], l[3], l[4], l[5]};
    const Footprint held = Footprint::polygon(copy);
    REQUIRE_MESSAGE(ex.add_footprints({&held, 1}, &error), error);
  }
  const f64 h = 0.0;
  const Ground ground = ground_at(h);
  u64 agree = 0;
  for (i64 z = -2000; z <= 12000; z += 250)
    for (i64 x = -2000; x <= 12000; x += 250) {
      CHECK(ex.excluded_mm(ground, x, z) == yard.covers_mm(x, z));
      ++agree;
    }
  CHECK(agree == 57 * 57);
  // A disc and a band beside it, in one call; refusals leave nothing added.
  const Footprint discs[2] = {Footprint{30000, 30000, 2.0},
                              Footprint::band(0, 20000, 0, 20000, 500)};
  REQUIRE(ex.add_footprints(discs, &error));
  CHECK(ex.footprint_count() == 3);
  CHECK(ex.excluded_mm(ground, 31500, 30000));
  CHECK(ex.excluded_mm(ground, 0, 20400));  // a band of no length is a disc of its half-width
  const Footprint bad[2] = {Footprint{0, 0, 1.0}, Footprint::band(0, 0, 1000, 0, 0)};
  CHECK_FALSE(ex.add_footprints(bad, &error));
  CHECK(error.find("footprint 1 is a band with a half-width under a millimetre") !=
        std::string::npos);
  CHECK(ex.footprint_count() == 3);
  const FootprintPoint flat[3] = {{0, 0}, {1000, 0}, {2000, 0}};
  const Footprint line = Footprint::polygon(flat);
  CHECK_FALSE(ex.add_footprints({&line, 1}, &error));
  CHECK(error.find("no area") != std::string::npos);
  const Footprint nan_disc{0, 0, std::nan("")};
  CHECK_FALSE(ex.add_footprints({&nan_disc, 1}, &error));
}
