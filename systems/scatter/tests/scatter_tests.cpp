// The scatter layer on the CPU (docs/subsystems/scatter.md, "Testing"): over a synthetic ground of
// planes, with no renderer — the same layer and seed give the same bytes twice and in any tile
// order; a tile's placements are the same asked alone or beside its neighbours, and the tiles of a
// block are the block; no two placements of a kind are closer than its spacing, across tile
// borders; slope and height limits hold; the density, constant, a noise or a `.tex` mask, is met
// within a declared tolerance over a large region; circles, a mask, another layer and an earlier
// kind's clearance exclude; the budget caps a tile and a far ring keeps a subset; the generator
// through the registry agrees tile by tile with its whole read and refuses what it cannot place;
// and the determinism hash pinned below, which GCC and Clang must reproduce.
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/texture/texture_build.h>
#include <domain/texture/texture_file.h>
#include <systems/scatter/scatter.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::scatter;

namespace {

// The ground: level west of x = 0 but for a gentle rise to the north (-z), a 1-in-2 plane east of
// it (26.7°). Plain arithmetic of the millimetres, so every toolchain answers the same heights.
f64 planes(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 x = static_cast<f64>(x_mm) / 1000.0;
  const f64 z = static_cast<f64>(z_mm) / 1000.0;
  return (x > 0.0 ? 0.5 * x : 0.0) - 0.05 * z;
}
f64 floor_of_planes(const void*, i64, i64) noexcept { return -3.0; }

scene_gen::Ground ground() {
  scene_gen::Ground g;
  g.surface_fn = &planes;
  g.floor_fn = &floor_of_planes;
  return g;
}

Kind rock_kind(const char* name, f32 spacing) {
  Kind kind;
  kind.name = name;
  kind.rock = Rock{};
  kind.spacing = spacing;
  return kind;
}

// The layer the determinism hash is pinned on: two kinds, a noise density, a slope rule, a
// clearance, a random and an uphill yaw, alignment, a scale range, a sink and a budget.
Layer pinned_layer() {
  Layer layer;
  layer.name = "pinned";
  layer.seed = 20261011;
  layer.region.kind = RegionKind::Tiles;
  layer.region.tile_min = {-2, -2};
  layer.region.tile_max = {1, 1};
  layer.density.kind = DensityKind::Noise;
  layer.density.per_m2 = 0.08;
  layer.density.octaves = 3;
  layer.density.wavelength = 40.0;
  layer.density.low = 0.25;
  layer.density.high = 0.75;
  Kind boulder = rock_kind("boulder", 4.0f);
  boulder.weight = 1.0;
  boulder.slope_max_deg = 30.0f;
  boulder.scale_min = 0.5f;
  boulder.scale_max = 2.0f;
  boulder.align = true;
  boulder.sink = 0.2f;
  Kind scrub = rock_kind("scrub", 1.5f);
  scrub.weight = 3.0;
  scrub.clearance = 2.0f;
  scrub.yaw = YawRule::Uphill;
  scrub.yaw_deg = 10.0f;
  scrub.stand = Stand::Floor;
  layer.kinds.push_back(boulder);
  layer.kinds.push_back(scrub);
  layer.budget.per_tile = 40;
  return layer;
}

Placer open_layer(const Layer& layer, const std::string& dir = std::string()) {
  Placer placer;
  std::string error;
  const bool ok = placer.open(layer, dir, 7, &error);
  REQUIRE_MESSAGE(ok, error);
  return placer;
}

bool same_points(const Vector<Point>& a, const Vector<Point>& b) {
  if (a.size() != b.size()) return false;
  for (u32 i = 0; i < a.size(); ++i) {
    if (std::memcmp(&a[i], &b[i], sizeof(Point)) != 0) return false;
  }
  return true;
}

// The closest two placements of one kind, squared millimetres: a sweep over z.
i64 closest_of_kind(Vector<Point> points, u32 kind) {
  std::vector<Point> of;
  for (const Point& p : points) {
    if (p.kind == kind) of.push_back(p);
  }
  std::sort(of.begin(), of.end(), [](const Point& a, const Point& b) { return a.z_mm < b.z_mm; });
  i64 best = INT64_MAX;
  for (size_t i = 0; i < of.size(); ++i) {
    for (size_t j = i + 1; j < of.size(); ++j) {
      const i64 dz = of[j].z_mm - of[i].z_mm;
      if (dz * dz >= best) break;
      const i64 dx = of[j].x_mm - of[i].x_mm;
      best = std::min(best, dx * dx + dz * dz);
    }
  }
  return best;
}

RectMm tile_rect(i32 x, i32 z, i64 tile_mm) {
  return RectMm{x * tile_mm, z * tile_mm, (x + 1) * tile_mm, (z + 1) * tile_mm};
}

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  return f.good();
}

// A `.tex` mask: red 255 where `inside(x, y)` holds, else 0; `size` texels a side.
template <typename F>
std::string write_mask(const test::TempDir& tmp, const char* name, u32 size, F inside) {
  std::vector<u8> rgba(size * size * 4u, u8{0});
  for (u32 y = 0; y < size; ++y) {
    for (u32 x = 0; x < size; ++x) {
      const u32 at = (y * size + x) * 4u;
      rgba[at] = inside(x, y) ? u8{255} : u8{0};
      rgba[at + 3] = 255;
    }
  }
  texture::TextureBuildOptions options;
  options.format = texture::FormatChoice::bc4;
  options.color_space = texture::ColorSpace::linear;
  options.mips = false;
  texture::TextureData data;
  std::string error;
  REQUIRE_MESSAGE(texture::build_texture(std::span<const u8>(rgba.data(), rgba.size()), size, size,
                                         1, options, data, nullptr, &error),
                  error);
  const std::string path = tmp.file(name);
  REQUIRE_MESSAGE(texture::write_texture_file(path, data, &error), error);
  return path;
}

}  // namespace

TEST_CASE("scatter: sin and cos without the C library are within 2e-15 of it") {
  f64 worst = 0.0;
  for (i32 i = -4000; i <= 4000; ++i) {
    const f64 a = static_cast<f64>(i) * 0.00731;
    f64 s = 0.0;
    f64 c = 0.0;
    sin_cos(a, s, c);
    worst = std::max(worst, std::max(std::fabs(s - std::sin(a)), std::fabs(c - std::cos(a))));
  }
  CHECK(worst < 2e-15);
}

TEST_CASE("scatter: the same layer and seed give the same bytes twice and in any tile order") {
  const Layer layer = pinned_layer();
  const Placer a = open_layer(layer);
  const Placer b = open_layer(layer);
  const scene_gen::Ground g = ground();
  std::vector<std::pair<i32, i32>> tiles;
  for (i32 z = -2; z <= 1; ++z) {
    for (i32 x = -2; x <= 1; ++x)
      tiles.push_back({x, z});
  }
  std::vector<Vector<Point>> forward(tiles.size());
  for (size_t i = 0; i < tiles.size(); ++i)
    a.place_tile(scene_gen::TileCoord{tiles[i].first, tiles[i].second}, g, ~0u, forward[i]);
  u32 total = 0;
  for (size_t i = tiles.size(); i-- > 0;) {
    Vector<Point> again;
    b.place_tile(scene_gen::TileCoord{tiles[i].first, tiles[i].second}, g, ~0u, again);
    CHECK(same_points(forward[i], again));
    total += again.size();
  }
  CHECK(total > 100);  // the case has something to hold
}

TEST_CASE("scatter: a tile is the same alone or beside its neighbours, and tiles make the block") {
  Layer layer = pinned_layer();
  layer.budget.per_tile = 0;  // the budget is per tile; the block's union is before it
  layer.region.kind = RegionKind::Scene;
  const Placer placer = open_layer(layer);
  const scene_gen::Ground g = ground();
  // The block of 4 x 4 tiles at once, then each tile alone: the union is the block, point for
  // point, so no point at a border is lost or doubled.
  Vector<Point> block;
  placer.place_rect(RectMm{-2 * placer.tile_mm(), -2 * placer.tile_mm(), 2 * placer.tile_mm(),
                           2 * placer.tile_mm()},
                    g, block);
  Vector<Point> tiles;
  for (i32 z = -2; z <= 1; ++z) {
    for (i32 x = -2; x <= 1; ++x) {
      Vector<Point> alone;
      placer.place_rect(tile_rect(x, z, placer.tile_mm()), g, alone);
      // The tile with its neighbours in the same request: its own part is the same.
      Vector<Point> beside;
      placer.place_rect(tile_rect(x, z, placer.tile_mm()).grown(placer.tile_mm()), g, beside);
      Vector<Point> own;
      for (const Point& p : beside) {
        if (tile_rect(x, z, placer.tile_mm()).contains(p.x_mm, p.z_mm)) own.push_back(p);
      }
      CHECK(same_points(alone, own));
      for (const Point& p : alone)
        tiles.push_back(p);
    }
  }
  std::sort(tiles.begin(), tiles.end(), [](const Point& a, const Point& b) {
    if (a.kind != b.kind) return a.kind < b.kind;
    if (a.z_mm != b.z_mm) return a.z_mm < b.z_mm;
    return a.x_mm < b.x_mm;
  });
  CHECK(same_points(block, tiles));
  // Across every border, no two of a kind closer than its spacing, nor a scrub nearer a boulder
  // than its clearance.
  CHECK(closest_of_kind(block, 0) >= 4000 * 4000);
  CHECK(closest_of_kind(block, 1) >= 1500 * 1500);
  for (const Point& s : block) {
    if (s.kind != 1) continue;
    for (const Point& r : block) {
      if (r.kind != 0) continue;
      const i64 dx = s.x_mm - r.x_mm;
      const i64 dz = s.z_mm - r.z_mm;
      CHECK(dx * dx + dz * dz >= 2000 * 2000);
    }
  }
}

TEST_CASE("scatter: the packing is what the density divides by") {
  // Saturated: every lattice point the two rounds keep is placed. Measured over 4 km² at a 4 m
  // spacing (~80,000 points), the count a square metre times r² is the constant the keep test
  // divides by, to within 1%.
  Layer layer;
  layer.seed = 99;
  layer.density.per_m2 = 1000.0;
  layer.kinds.push_back(rock_kind("dense", 4.0f));
  const Placer placer = open_layer(layer);
  Vector<Point> points;
  placer.place_rect(RectMm{0, 0, 2000000, 2000000}, scene_gen::Ground{}, points);
  const f64 packing = static_cast<f64>(points.size()) / 4.0e6 * 16.0;
  MESSAGE("measured packing " << packing << " against k_packing " << k_packing);
  CHECK(std::fabs(packing - k_packing) < 0.01 * k_packing);
  CHECK(closest_of_kind(points, 0) >= 4000 * 4000);
}

TEST_CASE("scatter: slope and height limits hold on the planes") {
  Layer layer;
  layer.seed = 5;
  layer.density.per_m2 = 0.2;
  Kind flat = rock_kind("flat", 1.0f);
  flat.slope_max_deg = 15.0f;
  flat.height_min = -1.0;
  flat.height_max = 1.0;
  Kind steep = rock_kind("steep", 1.0f);
  steep.slope_min_deg = 20.0f;
  layer.kinds.push_back(flat);
  layer.kinds.push_back(steep);
  const Placer placer = open_layer(layer);
  Vector<Point> points;
  const scene_gen::Ground g = ground();
  placer.place_rect(RectMm{-60000, -60000, 60000, 60000}, g, points);
  u32 flats = 0;
  u32 steeps = 0;
  for (const Point& p : points) {
    const f64 h = planes(nullptr, p.x_mm, p.z_mm);
    if (p.kind == 0) {
      ++flats;
      CHECK(p.x_mm <= 500);  // the slope is measured half a metre either side
      CHECK(h >= -1.0);
      CHECK(h <= 1.0);
      CHECK(p.y == h);
    } else {
      ++steeps;
      CHECK(p.x_mm >= -500);
    }
  }
  CHECK(flats > 50);
  CHECK(steeps > 500);
}

TEST_CASE("scatter: the density is met within 5% over a large region, constant, noise or mask") {
  const test::TempDir tmp("scatter_density");
  const RectMm region{-256000, -256000, 256000, 256000};
  const f64 area = 512.0 * 512.0;

  SUBCASE("constant") {
    Layer layer;
    layer.seed = 17;
    layer.density.per_m2 = 0.02;
    layer.kinds.push_back(rock_kind("a", 2.0f));
    const Placer placer = open_layer(layer);
    Vector<Point> points;
    placer.place_rect(region, scene_gen::Ground{}, points);
    const f64 want = 0.02 * area;
    MESSAGE("constant: " << points.size() << " placed, " << want << " expected");
    CHECK(std::fabs(static_cast<f64>(points.size()) - want) < 0.05 * want);
  }
  SUBCASE("two kinds share it by weight") {
    Layer layer;
    layer.seed = 18;
    layer.density.per_m2 = 0.02;
    Kind a = rock_kind("a", 2.0f);
    a.weight = 1.0;
    Kind b = rock_kind("b", 2.0f);
    b.weight = 3.0;
    layer.kinds.push_back(a);
    layer.kinds.push_back(b);
    const Placer placer = open_layer(layer);
    Vector<Point> points;
    placer.place_rect(region, scene_gen::Ground{}, points);
    u32 count[2] = {0, 0};
    for (const Point& p : points)
      ++count[p.kind];
    MESSAGE("weights 1:3: " << count[0] << " and " << count[1]);
    CHECK(std::fabs(static_cast<f64>(count[0]) - 0.005 * area) < 0.05 * 0.005 * area * 2.0);
    CHECK(std::fabs(static_cast<f64>(count[1]) - 0.015 * area) < 0.05 * 0.015 * area);
  }
  SUBCASE("a mask: its lit half placed, its dark half empty") {
    // The left half of the mask lit (x < 0 of the region), over the region.
    const std::string mask = write_mask(tmp, "half.tex", 64, [](u32 x, u32) { return x < 32; });
    Layer layer;
    layer.seed = 19;
    layer.density.kind = DensityKind::Mask;
    layer.density.per_m2 = 0.02;
    layer.density.mask = mask;
    layer.density.mask_min = {-256.0, -256.0};
    layer.density.mask_max = {256.0, 256.0};
    layer.kinds.push_back(rock_kind("a", 2.0f));
    const Placer placer = open_layer(layer);
    Vector<Point> points;
    placer.place_rect(region, scene_gen::Ground{}, points);
    u32 lit = 0;
    u32 dark = 0;
    for (const Point& p : points) {
      // A texel is 8 m; the bilinear ramp between the halves is one texel wide.
      if (p.x_mm < -8000) ++lit;
      if (p.x_mm >= 8000) ++dark;
    }
    const f64 want = 0.02 * 248.0 * 512.0;
    MESSAGE("mask: " << lit << " in the lit half, " << want << " expected; " << dark << " dark");
    CHECK(dark == 0);
    CHECK(std::fabs(static_cast<f64>(lit) - want) < 0.05 * want);
  }
  SUBCASE("a noise") {
    Layer layer;
    layer.seed = 20;
    layer.density.kind = DensityKind::Noise;
    layer.density.per_m2 = 0.04;
    layer.density.octaves = 3;
    layer.density.wavelength = 64.0;
    layer.kinds.push_back(rock_kind("a", 2.0f));
    const Placer placer = open_layer(layer);
    Vector<Point> points;
    placer.place_rect(region, scene_gen::Ground{}, points);
    // What the field integrates to over the region, on a 1 m grid, against what was placed; and
    // the same in the region's two halves, which the noise makes unequal.
    f64 want = 0.0;
    f64 want_west = 0.0;
    for (i64 z = region.z0 + 500; z < region.z1; z += 1000) {
      for (i64 x = region.x0 + 500; x < region.x1; x += 1000) {
        const f64 d = placer.density_at(x, z);
        want += d;
        want_west += x < 0 ? d : 0.0;
      }
    }
    u32 west = 0;
    for (const Point& p : points)
      west += p.x_mm < 0 ? 1u : 0u;
    MESSAGE("noise: " << points.size() << " placed, " << want << " expected; west " << west
                      << " against " << want_west);
    CHECK(std::fabs(static_cast<f64>(points.size()) - want) < 0.05 * want);
    CHECK(std::fabs(static_cast<f64>(west) - want_west) < 0.07 * want_west);
  }
}

TEST_CASE("scatter: circles, a mask, another layer and an earlier kind exclude") {
  const test::TempDir tmp("scatter_exclusions");
  // The other layer: big boulders at a 10 m spacing, everywhere.
  Layer other;
  other.name = "boulders";
  other.seed = 3;
  other.density.per_m2 = 1.0;
  other.kinds.push_back(rock_kind("boulder", 10.0f));
  REQUIRE(write_text(tmp.file("boulders.json"),
                     write_json(schema::to_json(other), JsonWriteOptions{})));
  const std::string mask =
      write_mask(tmp, "corner.tex", 16, [](u32 x, u32 y) { return x < 8 && y < 8; });
  Layer layer;
  layer.seed = 4;
  layer.density.per_m2 = 1.0;
  layer.kinds.push_back(rock_kind("pebble", 1.0f));
  ExcludeCircle circle;
  circle.at = WorldPos{20.0, 0.0, 20.0};
  circle.radius = 12.0f;
  layer.exclude.circles.push_back(circle);
  layer.exclude.mask = "corner.tex";
  layer.exclude.mask_min = {-64.0, -64.0};
  layer.exclude.mask_max = {64.0, 64.0};
  ExcludeLayer by;
  by.layer = "boulders.json";
  by.clearance = 3.0f;
  layer.exclude.layers.push_back(by);
  const Placer placer = open_layer(layer, tmp.path());
  (void)mask;
  const RectMm region{-64000, -64000, 64000, 64000};
  Vector<Point> points;
  placer.place_rect(region, scene_gen::Ground{}, points);
  const Placer boulders = open_layer(other);
  Vector<Point> rocks;
  boulders.place_rect(region.grown(3000), scene_gen::Ground{}, rocks);
  REQUIRE(rocks.size() > 50);
  REQUIRE(points.size() > 1000);
  for (const Point& p : points) {
    const i64 cx = p.x_mm - 20000;
    const i64 cz = p.z_mm - 20000;
    CHECK(cx * cx + cz * cz >= 12000 * 12000);
    // The lit corner is texels 0..7 of 16 in x and z: x and z below 0 (blurred a texel past).
    CHECK_FALSE((p.x_mm < -8000 && p.z_mm < -8000));
    for (const Point& r : rocks) {
      const i64 dx = p.x_mm - r.x_mm;
      const i64 dz = p.z_mm - r.z_mm;
      CHECK(dx * dx + dz * dz >= 3000 * 3000);
    }
  }
  // A layer an exclusion names may not exclude by layers itself.
  Layer nested = other;
  nested.exclude.layers.push_back(by);
  REQUIRE(
      write_text(tmp.file("nested.json"), write_json(schema::to_json(nested), JsonWriteOptions{})));
  Layer outer = layer;
  outer.exclude.layers[0].layer = "nested.json";
  Placer refused;
  std::string error;
  CHECK_FALSE(refused.open(outer, tmp.path(), 7, &error));
  CHECK(error.find("may not") != std::string::npos);
}

TEST_CASE("scatter: the budget caps a tile, and a far ring keeps a subset of a near one") {
  Layer layer = pinned_layer();
  layer.region.kind = RegionKind::Scene;
  layer.kinds[1].far = 100.0f;  // the scrub only within 100 m
  layer.budget.per_tile = 0;
  const Placer uncapped = open_layer(layer);
  layer.budget.per_tile = 12;
  const Placer capped = open_layer(layer);
  const scene_gen::Ground g = ground();
  for (i32 z = -2; z <= 1; ++z) {
    for (i32 x = -2; x <= 1; ++x) {
      Vector<Point> all;
      Vector<Point> kept;
      uncapped.place_tile(scene_gen::TileCoord{x, z}, g, ~0u, all);
      capped.place_tile(scene_gen::TileCoord{x, z}, g, ~0u, kept);
      CHECK(kept.size() == std::min<u32>(12u, all.size()));
      for (const Point& p : kept) {
        CHECK(std::find_if(all.begin(), all.end(), [&](const Point& q) {
                return std::memcmp(&p, &q, sizeof(Point)) == 0;
              }) != all.end());
      }
    }
  }
  // Rings at 1.5, 8 and 24 tiles of 32 m: the second's nearest tile is 48 m out, the third's 256.
  scene::WorldRings world;
  world.tile_size = 32.0f;
  for (f32 radius : {1.5f, 8.0f, 24.0f}) {
    scene::WorldRing ring;
    ring.radius = radius;
    world.rings.push_back(ring);
  }
  scene_gen::Context context;
  context.world = &world;
  context.tile_size = 32.0f;
  context.ring = 1;
  CHECK(capped.ring_kind_mask(context) == 3u);
  context.ring = 2;
  CHECK(capped.ring_kind_mask(context) == 1u);
  Vector<Point> near_ring;
  Vector<Point> far_ring;
  capped.place_tile(scene_gen::TileCoord{0, 0}, g, 3u, near_ring);
  capped.place_tile(scene_gen::TileCoord{0, 0}, g, 1u, far_ring);
  u32 boulders = 0;
  for (const Point& p : near_ring)
    boulders += p.kind == 0 ? 1u : 0u;
  CHECK(far_ring.size() == boulders);
  for (const Point& p : far_ring)
    CHECK(p.kind == 0);
}

TEST_CASE("scatter: the determinism hash is pinned") {
  // Taken on MSVC; GCC and Clang at v3 and GCC at v2 must give the same
  // (docs/subsystems/scatter.md, "The same bytes from every toolchain"). A change that moves it
  // moves every scattered scene, and says so in its commit.
  const Placer placer = open_layer(pinned_layer());
  const scene_gen::Ground g = ground();
  Vector<Point> points;
  for (i32 z = -2; z <= 1; ++z) {
    for (i32 x = -2; x <= 1; ++x)
      placer.place_tile(scene_gen::TileCoord{x, z}, g, ~0u, points);
  }
  const u64 h = hash_points(std::span<const Point>(points.data(), points.size()));
  const Vector<u8> glb = rock_glb(Rock{});
  const u64 rock = hash_bytes(glb.data(), glb.size());
  char text[64];
  std::snprintf(text, sizeof(text), "%016llx %016llx", static_cast<unsigned long long>(h),
                static_cast<unsigned long long>(rock));
  MESSAGE("points " << points.size() << ", hashes " << text);
  CHECK(points.size() == 516u);
  CHECK(std::string(text) == "1ab4c8d051dbb2cb 7a6687b85300637a");
}

TEST_CASE("scatter: the generator through the registry: tiles are the whole read, refusals") {
  const test::TempDir tmp("scatter_registry");
  const scene_gen::PlacementGeneratorDesc* generator =
      scene_gen::GeneratorRegistry::global().find_placement(k_placement_generator);
  REQUIRE(generator != nullptr);
  Layer layer = pinned_layer();
  layer.budget.per_tile = 0;
  const JsonValue params = schema::to_json(layer);
  const std::string dir = tmp.path();
  scene_gen::Context context;
  context.world_seed = 7;
  context.dir = dir;
  context.where = "scene.json: placements 0";
  context.ground = ground();
  void* state = nullptr;
  std::string error;
  REQUIRE_MESSAGE(generator->open(params, context, &state, &error), error);
  scene_gen::Placements whole;
  REQUIRE_MESSAGE(generator->expand(state, context, whole, &error), error);
  // One mesh a kind: the procedural rock, written once into `scatter` beside the scene.
  REQUIRE(whole.meshes.size() == 2);
  CHECK(whole.meshes[0].path == whole.meshes[1].path);  // the same rock, one file
  CHECK(std::ifstream(whole.meshes[0].path).good());
  CHECK(whole.meshes[0].path.find(rock_file_name(Rock{})) != std::string::npos);
  // Tile by tile, in the whole read's order, outside a world: the same instances.
  scene_gen::Placements tiled;
  for (i32 z = -2; z <= 1; ++z) {
    for (i32 x = -2; x <= 1; ++x) {
      CHECK(generator->occupies(state, scene_gen::TileCoord{x, z}));
      REQUIRE(generator->tile(state, scene_gen::TileCoord{x, z}, context, tiled, &error));
    }
  }
  CHECK_FALSE(generator->occupies(state, scene_gen::TileCoord{2, 0}));
  REQUIRE(tiled.instances.size() == whole.instances.size());
  for (u32 i = 0; i < whole.instances.size(); ++i) {
    CHECK(tiled.instances[i].position == whole.instances[i].position);
    CHECK(tiled.instances[i].rotation == whole.instances[i].rotation);
    CHECK(tiled.instances[i].mesh == whole.instances[i].mesh);
  }
  generator->close(state);

  // An unknown field is refused with its path; a whole read of the whole scene is refused.
  JsonValue bad = params;
  bad.set("colour", JsonValue(1.0));
  state = nullptr;
  CHECK_FALSE(generator->open(bad, context, &state, &error));
  CHECK(error.find("scene.json: placements 0") == 0);
  CHECK(error.find("colour") != std::string::npos);
  Layer endless = layer;
  endless.region.kind = RegionKind::Scene;
  REQUIRE(generator->open(schema::to_json(endless), context, &state, &error));
  scene_gen::Placements none;
  CHECK_FALSE(generator->expand(state, context, none, &error));
  CHECK(error.find("whole scene") != std::string::npos);
  generator->close(state);
  // A streamed layer's grid must be the world's.
  scene::WorldRings world;
  context.world = &world;
  context.tile_size = 64.0f;
  CHECK_FALSE(generator->open(params, context, &state, &error));
  CHECK(error.find("one grid") != std::string::npos);
}
