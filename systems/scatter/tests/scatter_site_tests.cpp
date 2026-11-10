// Scatter's kinds by ecotope rule (roadmap G144; docs/subsystems/scatter.md, "Kinds by site"; the
// grounding in docs/experiments/site-and-ecotope.md): over a valley of two 31° planes, one facing
// the sun and one away, a ruled conifer places nothing above its limit, its limit stands higher on
// the sunny side by the amount the aspect shift says, its density falls across the soft edge and
// its stature through the krummholz band; a heat-load rule leaves the sunny side bare; a distance
// rule keeps a kind by the water; a kind with no rule places what it did; and the tiles of a block
// are the block. The placer is run on a synthetic ground, with no renderer.
#include <domain/scene_gen/scene_gen.h>
#include <systems/scatter/scatter.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace engine;
using namespace engine::scatter;

namespace {

// A valley along x: the ground rises 0.6 m a metre either side of z = 0 (31°). North of the floor
// (z < 0) the slope faces south, towards the sun; south of it (z > 0) it faces north, away.
constexpr f64 k_rise = 0.6;
f64 valley(const void*, i64, i64 z_mm) noexcept {
  const f64 z = static_cast<f64>(z_mm) / 1000.0;
  return k_rise * (z < 0.0 ? -z : z);
}
scene_gen::Ground valley_ground() {
  scene_gen::Ground g;
  g.surface_fn = &valley;
  return g;
}

Kind conifer() {
  Kind kind;
  kind.name = "conifer";
  kind.rock = Rock{};
  kind.spacing = 1.0f;
  return kind;
}
Layer layer_of(const Kind& kind, f64 per_m2 = 0.3) {
  Layer layer;
  layer.seed = 23;
  layer.density.per_m2 = per_m2;
  layer.kinds.push_back(kind);
  return layer;
}
Placer open_layer(const Layer& layer) {
  Placer placer;
  std::string error;
  REQUIRE_MESSAGE(placer.open(layer, std::string(), 7, &error), error);
  return placer;
}
std::string refusal(const Layer& layer) {
  Placer placer;
  std::string error;
  CHECK_FALSE(placer.open(layer, std::string(), 7, &error));
  return error;
}

// The valley's slopes from 6 m off the floor (past the site's 5 m step, so the derivatives are the
// plane's) out to 700 m either side, along 128 m of x.
constexpr RectMm k_north{0, -700000, 128000, -6000};
constexpr RectMm k_south{0, 6000, 128000, 700000};

Vector<Point> place(const Placer& placer, const RectMm& rect, const scene_gen::Site* site) {
  Site s;
  s.ground = valley_ground();
  s.site = site;
  Vector<Point> out;
  placer.place_rect(rect, s, out);
  return out;
}

f64 elevation(const Point& p) { return valley(nullptr, p.x_mm, p.z_mm); }

// The sunward tilt of the valley's sides at 46.5° N: sin(slope) times cos 45° (the sides face
// due south and north, the sun's bearing is south-west).
f64 sunward_of_side() { return k_rise / std::sqrt(1.0 + k_rise * k_rise) * 0.70710678118654752; }

}  // namespace

TEST_CASE("scatter site: a treeline with an aspect shift, its soft edge and its krummholz band") {
  scene_gen::Site site;
  REQUIRE(site.build(46.5, {}, {}));
  Kind kind = conifer();
  Response limit;
  limit.max = 300.0;
  limit.soft = 50.0;
  kind.elevation = limit;
  kind.aspect_shift_m = 80.0f;
  kind.krummholz_m = 120.0f;
  kind.krummholz_scale = 0.15f;
  const Placer placer = open_layer(layer_of(kind));
  const auto sunny = place(placer, k_north, &site);
  const auto shaded = place(placer, k_south, &site);
  REQUIRE(sunny.size() > 1000);
  REQUIRE(shaded.size() > 1000);

  // The shift: 80 m times twice the sunward tilt, up on the sunny side and down on the shaded.
  const f64 shift = 80.0 * 2.0 * sunward_of_side();
  CHECK(std::fabs(shift - 58.2) < 0.1);
  const f64 sunny_top = 300.0 + shift;
  const f64 shaded_top = 300.0 - shift;
  f64 sunny_max = 0.0;
  f64 shaded_max = 0.0;
  for (const Point& p : sunny)
    sunny_max = std::max(sunny_max, elevation(p));
  for (const Point& p : shaded)
    shaded_max = std::max(shaded_max, elevation(p));
  // No tree above its limit and its soft edge, on either side.
  CHECK(sunny_max <= sunny_top + 50.0);
  CHECK(shaded_max <= shaded_top + 50.0);
  // And the sunny side's trees reach past where the shaded side's stop.
  CHECK(sunny_max > shaded_top + 50.0 + 0.5 * shift);

  // The density across the shaded side's soft edge: five 10 m bins, each no denser than the one
  // below it, and the whole edge within 10% of its expected count (the smoothstep's mean is a
  // half).
  u32 plateau = 0;
  u32 bins[5] = {};
  for (const Point& p : shaded) {
    const f64 e = elevation(p);
    if (e >= shaded_top - 50.0 && e < shaded_top) ++plateau;
    if (e >= shaded_top && e < shaded_top + 50.0) ++bins[static_cast<u32>((e - shaded_top) / 10.0)];
  }
  REQUIRE(plateau > 1000);
  u32 edge = 0;
  for (u32 b = 0; b < 5; ++b) {
    edge += bins[b];
    if (b > 0) CHECK(bins[b] < bins[b - 1]);
  }
  CHECK(bins[0] > bins[4] * 5);
  CHECK(std::fabs(static_cast<f64>(edge) / static_cast<f64>(plateau) - 0.5) < 0.05);

  // The stature through the band: the scale (1, the kind's own) falls with the elevation, to 0.15
  // at the limit and no further.
  Vector<Point> by_height(shaded);
  std::sort(by_height.begin(), by_height.end(),
            [](const Point& a, const Point& b) { return elevation(a) < elevation(b); });
  for (u32 i = 1; i < by_height.size(); ++i)
    CHECK(by_height[i].scale <= by_height[i - 1].scale);
  for (const Point& p : shaded) {
    const f64 e = elevation(p);
    const f64 t = std::clamp((e - (shaded_top - 120.0)) / 120.0, 0.0, 1.0);
    const f64 want = 1.0 - 0.85 * (t * t * (3.0 - 2.0 * t));
    CHECK(std::fabs(static_cast<f64>(p.scale) - want) < 1e-6);
    if (e < shaded_top - 120.0) CHECK(p.scale == 1.0f);
    if (e >= shaded_top) CHECK(p.scale == doctest::Approx(0.15f));
  }
}

TEST_CASE("scatter site: a heat-load rule keeps the forest on the shaded side (r06)") {
  scene_gen::Site site;
  REQUIRE(site.build(46.5, {}, {}));
  Kind kind = conifer();
  Response cool;
  cool.max = 1.0;
  cool.soft = 0.1;
  kind.heat_load = cool;
  const Placer placer = open_layer(layer_of(kind));
  const auto sunny = place(placer, k_north, &site);
  const auto shaded = place(placer, k_south, &site);
  CHECK(sunny.empty());
  CHECK(shaded.size() > 10000);
  // The shaded side keeps every point a kind with no rule places there, byte for byte.
  const auto free = place(open_layer(layer_of(conifer())), k_south, &site);
  REQUIRE(free.size() == shaded.size());
  for (u32 i = 0; i < free.size(); ++i)
    CHECK(std::memcmp(&free[i], &shaded[i], sizeof(Point)) == 0);
}

TEST_CASE("scatter site: a distance rule keeps a kind by the water") {
  scene::WaterChannel channel;
  channel.points.push_back(WorldPos{0.0, 0.0, 500.0});
  channel.points.push_back(WorldPos{128.0, 0.0, 500.0});
  channel.width_m.push_back(3.0f);
  channel.width_m.push_back(3.0f);
  scene_gen::Site site;
  REQUIRE(site.build(46.5, {}, {&channel, 1}));
  Kind kind = conifer();
  Response near;
  near.max = 30.0;
  near.soft = 10.0;
  kind.water_m = near;
  const auto out = place(open_layer(layer_of(kind)), k_south, &site);
  REQUIRE(out.size() > 1000);
  for (const Point& p : out) {
    const f64 off = std::fabs(static_cast<f64>(p.z_mm) / 1000.0 - 500.0) - 2.65;
    CHECK(off < 40.0);
  }
  // With no site handed, the fallback has no water: every distance is the reach, and nothing
  // stands.
  CHECK(place(open_layer(layer_of(kind)), k_south, nullptr).empty());
}

TEST_CASE(
    "scatter site: a kind with no rule, or a rule that holds everywhere, places what it did") {
  scene_gen::Site site;
  REQUIRE(site.build(46.5, {}, {}));
  const Placer plain = open_layer(layer_of(conifer()));
  const auto base = place(plain, k_north, nullptr);
  const auto with_site = place(plain, k_north, &site);
  REQUIRE(base.size() == with_site.size());
  CHECK(hash_points({base.data(), base.size()}) ==
        hash_points({with_site.data(), with_site.size()}));
  Kind open_kind = conifer();
  open_kind.elevation = Response{};
  open_kind.slope = Response{};
  open_kind.heat_load = Response{};
  open_kind.curvature = Response{};
  const auto everywhere = place(open_layer(layer_of(open_kind)), k_north, &site);
  REQUIRE(everywhere.size() == base.size());
  for (u32 i = 0; i < base.size(); ++i)
    CHECK(std::memcmp(&base[i], &everywhere[i], sizeof(Point)) == 0);
}

TEST_CASE("scatter site: a ruled kind's tiles are its block") {
  scene_gen::Site site;
  REQUIRE(site.build(46.5, {}, {}));
  Kind kind = conifer();
  Response limit;
  limit.max = 30.0;
  limit.soft = 20.0;
  kind.elevation = limit;
  kind.aspect_shift_m = 40.0f;
  kind.krummholz_m = 15.0f;
  kind.krummholz_scale = 0.3f;
  Layer layer = layer_of(kind);
  layer.region.kind = RegionKind::Tiles;
  layer.region.tile_min = {-2, -3};
  layer.region.tile_max = {1, 2};
  const Placer placer = open_layer(layer);
  Site s;
  s.ground = valley_ground();
  s.site = &site;
  Vector<Point> block;
  placer.place_rect(RectMm{-64000, -96000, 64000, 96000}, s, block);
  REQUIRE(block.size() > 1000);
  Vector<Point> tiles;
  for (i32 tz = -3; tz <= 2; ++tz)
    for (i32 tx = -2; tx <= 1; ++tx)
      placer.place_tile(scene_gen::TileCoord{tx, tz}, s, ~0u, tiles);
  std::sort(tiles.begin(), tiles.end(), [](const Point& a, const Point& b) {
    if (a.kind != b.kind) return a.kind < b.kind;
    if (a.z_mm != b.z_mm) return a.z_mm < b.z_mm;
    return a.x_mm < b.x_mm;
  });
  REQUIRE(tiles.size() == block.size());
  for (u32 i = 0; i < block.size(); ++i)
    CHECK(std::memcmp(&block[i], &tiles[i], sizeof(Point)) == 0);
}

TEST_CASE("scatter site: a rule that cannot be read is refused") {
  Kind kind = conifer();
  Response bad;
  bad.min = 10.0;
  bad.max = 5.0;
  kind.slope = bad;
  CHECK(refusal(layer_of(kind)).find("slope response") != std::string::npos);
  kind = conifer();
  Response far;
  far.max = 120.0;
  far.soft = 20.0;
  kind.water_m = far;
  CHECK(refusal(layer_of(kind)).find("128 m") != std::string::npos);
  kind = conifer();
  kind.krummholz_m = 50.0f;
  CHECK(refusal(layer_of(kind)).find("needs an elevation response") != std::string::npos);
  kind = conifer();
  kind.elevation = Response{};
  kind.krummholz_scale = 0.0f;
  CHECK(refusal(layer_of(kind)).find("krummholz_scale") != std::string::npos);
}
