// The scatter leaves a scene's holes, channels and water empty (roadmap G135; docs/subsystems/
// scene_gen.md, "Exclusion"; scatter.md): a forest placed across a lake puts no tree below its
// level, a rock scatter across a stream none in its bed, and a scene with none of the three places
// every point it did. The placer is run on a synthetic ground, with no renderer.
#include <domain/scene_gen/scene_gen.h>
#include <systems/scatter/scatter.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

using namespace engine;
using namespace engine::scatter;

namespace {

// A basin: 0 m at the rim, 5 m down at the middle, 10 m of lake radius at the level 0 -- the ground
// of (x, z) is `((x - 100)^2 + (z - 100)^2) / 20 - 5`, which is below 0 within 10 m of (100, 100).
f64 basin(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 x = static_cast<f64>(x_mm) / 1000.0 - 100.0;
  const f64 z = static_cast<f64>(z_mm) / 1000.0 - 100.0;
  return (x * x + z * z) / 20.0 - 5.0;
}

scene_gen::Ground basin_ground() {
  scene_gen::Ground g;
  g.surface_fn = &basin;
  return g;
}

Layer pebbles() {
  Layer layer;
  layer.seed = 11;
  layer.density.per_m2 = 0.3;
  Kind kind;
  kind.name = "pebble";
  kind.rock = Rock{};
  kind.spacing = 1.5f;
  layer.kinds.push_back(kind);
  return layer;
}

Placer open_layer(const Layer& layer) {
  Placer placer;
  std::string error;
  REQUIRE_MESSAGE(placer.open(layer, std::string(), 7, &error), error);
  return placer;
}

Vector<Point> place(const Placer& placer, const scene_gen::Exclusion* ex) {
  Site site;
  site.ground = basin_ground();
  site.exclusion = ex;
  Vector<Point> out;
  placer.place_rect(RectMm{60000, 60000, 140000, 140000}, site, out);
  return out;
}

}  // namespace

TEST_CASE("scatter exclusion: no tree below a lake's level, and the same trees round it") {
  const Placer placer = open_layer(pebbles());
  const auto base = place(placer, nullptr);
  REQUIRE(base.size() > 500);

  scene::WaterBody lake;
  lake.level_m = 0.0;
  scene::WaterCircle circle;
  circle.center = WorldPos{100.0, 0.0, 100.0};
  circle.radius_m = 30.0f;  // wider than the lake: the ground above the level is dry
  lake.shape.circle = circle;
  scene_gen::Exclusion ex;
  REQUIRE(ex.build({}, {&lake, 1}, {}));
  const auto out = place(placer, &ex);

  u32 under = 0;
  for (const Point& p : base) {
    const f64 below = basin(nullptr, p.x_mm, p.z_mm);
    under += below < 0.0 ? 1u : 0u;
  }
  REQUIRE(under > 50);  // pi 10^2 * 0.3
  CHECK(out.size() == base.size() - under);
  for (const Point& p : out)
    CHECK(basin(nullptr, p.x_mm, p.z_mm) >= 0.0);
  // Every point that stood on dry ground is still there, byte for byte.
  u32 at = 0;
  for (const Point& p : base) {
    if (basin(nullptr, p.x_mm, p.z_mm) < 0.0) continue;
    REQUIRE(at < out.size());
    CHECK(std::memcmp(&p, &out[at], sizeof(Point)) == 0);
    ++at;
  }
}

TEST_CASE("scatter exclusion: a 3 m stream's bed and banks and a hole are bare") {
  const Placer placer = open_layer(pebbles());
  const auto base = place(placer, nullptr);

  scene::WaterChannel channel;
  channel.points.push_back(WorldPos{50.0, 0.0, 80.0});
  channel.points.push_back(WorldPos{150.0, 0.0, 80.0});
  channel.width_m.push_back(3.0f);
  channel.width_m.push_back(3.0f);
  scene::TerrainHole hole;
  hole.center = WorldPos{120.0, 0.0, 120.0};
  hole.radius_m = 6.0f;
  scene_gen::Exclusion ex;
  REQUIRE(ex.build({&hole, 1}, {}, {&channel, 1}));
  const auto out = place(placer, &ex);

  const auto in_bed = [](const Point& p) {
    return std::fabs(static_cast<f64>(p.z_mm) / 1000.0 - 80.0) < 2.65 - 0.01;
  };
  const auto in_hole = [](const Point& p) {
    const f64 dx = static_cast<f64>(p.x_mm) / 1000.0 - 120.0;
    const f64 dz = static_cast<f64>(p.z_mm) / 1000.0 - 120.0;
    return dx * dx + dz * dz < 36.0 - 0.1;
  };
  u32 gone = 0;
  for (const Point& p : base)
    gone += (in_bed(p) || in_hole(p)) ? 1u : 0u;
  REQUIRE(gone > 40);
  for (const Point& p : out) {
    CHECK_FALSE(in_bed(p));
    CHECK_FALSE(in_hole(p));
  }
  // Not a point more is lost than the ones standing in them, less the ones on the rim.
  CHECK(out.size() <= base.size() - gone);
  CHECK(out.size() + gone + 40 >= base.size());
}

TEST_CASE("scatter exclusion: a scene with none places every point it did, byte for byte") {
  const Placer placer = open_layer(pebbles());
  const auto base = place(placer, nullptr);

  scene_gen::Exclusion empty;
  REQUIRE(empty.build({}, {}, {}));
  scene::TerrainHole away;
  away.center = WorldPos{5000.0, 0.0, 5000.0};
  away.radius_m = 4.0f;
  scene_gen::Exclusion far;
  REQUIRE(far.build({&away, 1}, {}, {}));
  for (const scene_gen::Exclusion* ex : {&empty, &far}) {
    const auto out = place(placer, ex);
    REQUIRE(out.size() == base.size());
    for (u32 i = 0; i < base.size(); ++i)
      CHECK(std::memcmp(&base[i], &out[i], sizeof(Point)) == 0);
  }
}
