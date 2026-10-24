// The tile set (docs/subsystems/tileset.md): the heightfield source and its synthetic shapes, the
// set's writer and reader with its far mip chain, the box filter against a direct computation, the
// build through the derived-data cache, and the `tiles` ground provider through the registry.
#include <core/hash/hash.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/tileset/tileset.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <schemas/scene.h>
#include <string>
#include <thread>
#include <vector>

using namespace engine;
using namespace engine::tileset;

namespace {

i64 floor_div(i64 a, i64 b) {
  const i64 q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
i64 clamp(i64 v, i64 lo, i64 hi) { return std::max(lo, std::min(hi, v)); }

// The finest's millimetres at a world lattice index, the edge held.
struct Grid {
  Vector<i32> mm;
  i64 w = 0, h = 0, ox = 0, oz = 0, s = 0;
  i32 at_world(i64 i, i64 j) const {
    return mm[static_cast<u32>(clamp(j - oz, 0, h - 1) * w + clamp(i - ox, 0, w - 1))];
  }
};

Grid grid_of(const Heightfield& f, const BuildOptions& o) {
  Grid g;
  std::string error;
  REQUIRE_MESSAGE(quantize_heights(f, o, g.mm, error), error);
  g.w = f.width;
  g.h = f.height;
  g.s = f.spacing_mm;
  g.ox = (f.origin_x_mm + o.offset_x_mm) / g.s;
  g.oz = (f.origin_z_mm + o.offset_z_mm) / g.s;
  return g;
}

// **The box filter, directly**: the trapezoid rule over the (r + 1)^2 finest points the square one
// level spacing wide round the point spans, weights 1 at the square's edges and 2 inside a side,
// summed in one loop rather than separably, and rounded half up.
i32 direct_filter(const Grid& g, i64 r, i64 I, i64 J) {
  i64 sum = 0;
  const i64 half = r / 2;
  for (i64 b = -half; b <= half; ++b) {
    for (i64 a = -half; a <= half; ++a) {
      const i64 w = (a == -half || a == half ? 1 : 2) * (b == -half || b == half ? 1 : 2);
      sum += w * g.at_world(I * r + a, J * r + b);
    }
  }
  const i64 den = 4 * r * r;
  return static_cast<i32>(floor_div(2 * sum + den, 2 * den));
}

f32 metres(f64 mm) { return static_cast<f32>(mm / 1000.0); }

// A small set: a seeded ridge 97 points a side at 50 cm, its corner 24 m west and 16 m north of
// the origin, in tiles of 16 cells, and a chain of three far levels at 1, 2 and 4 m.
struct SmallSet {
  test::TempDir tmp{"engine_tileset_small"};
  Heightfield field = synth_heightfield(SynthKind::ridge, 97, 500, 7, -24000, -16000);
  BuildOptions options;
  std::string source;
  BuildResult built;
  std::string error;
  SmallSet() {
    options.cells = 16;
    options.ring_spacing_mm = 500;
    options.far_ratio = 2;
    options.far_levels = 3;
    source = tmp.file("ridge.json");
    REQUIRE_MESSAGE(write_heightfield(source, field, error), error);
    REQUIRE_MESSAGE(ensure_tileset(source, options, tmp.file("set"), "", false, built, error),
                    error);
  }
};

}  // namespace

TEST_CASE("tileset: the synthetic shapes are what their names say") {
  const Heightfield flat = synth_heightfield(SynthKind::flat, 33, 1000);
  CHECK(std::all_of(flat.data.begin(), flat.data.end(), [](f32 h) { return h == 0.0f; }));
  const Heightfield ridge = synth_heightfield(SynthKind::ridge, 65, 1000);
  CHECK(*std::max_element(ridge.data.begin(), ridge.data.end()) > 40.0f);
  CHECK(ridge.data[0] == 0.0f);  // a corner far from the ridge's line, at the low edge
  const Heightfield basin = synth_heightfield(SynthKind::basin, 65, 1000);
  CHECK(basin.data[32 * 65 + 32] == -15.0f);  // the bowl's floor at the centre
  CHECK(basin.data[0] == 5.0f);               // outside the rim
  const Heightfield steps = synth_heightfield(SynthKind::steps, 65, 1000);
  for (const f32 h : steps.data) {
    CHECK(h >= 0.0f);
    CHECK(h <= 16.0f);
    CHECK(std::fmod(h, 2.0f) == 0.0f);
  }
  // A seed roughens the shape by at most an eighth of a metre a point, and is the same grid twice.
  const Heightfield rough = synth_heightfield(SynthKind::flat, 33, 1000, 9);
  const Heightfield again = synth_heightfield(SynthKind::flat, 33, 1000, 9);
  bool any = false;
  for (u32 p = 0; p < rough.data.size(); ++p) {
    CHECK(std::fabs(rough.data[p]) <= 0.125f);
    CHECK(rough.data[p] == again.data[p]);
    any = any || rough.data[p] != 0.0f;
  }
  CHECK(any);
  SynthKind kind{};
  CHECK(parse_synth_kind("basin", kind));
  CHECK(kind == SynthKind::basin);
  CHECK_FALSE(parse_synth_kind("mountain", kind));
}

TEST_CASE("tileset: a heightfield round-trips, and a grid that is not its header's is refused") {
  test::TempDir tmp{"engine_tileset_source"};
  const Heightfield field = synth_heightfield(SynthKind::basin, 17, 250, 3, 1000, -2000);
  std::string error;
  REQUIRE_MESSAGE(write_heightfield(tmp.file("bowl.json"), field, error), error);
  CHECK(io::exists(tmp.file("bowl.f32")));
  Heightfield back;
  u64 hash = 0;
  REQUIRE_MESSAGE(read_heightfield(tmp.file("bowl.json"), back, hash, error), error);
  CHECK(back.width == 17);
  CHECK(back.spacing_mm == 250);
  CHECK(back.origin_x_mm == 1000);
  CHECK(back.origin_z_mm == -2000);
  CHECK(back.data.size() == field.data.size());
  CHECK(std::equal(back.data.begin(), back.data.end(), field.data.begin()));
  std::string header;
  REQUIRE(io::read_file(tmp.file("bowl.json"), header) == io::Status::Ok);
  CHECK(hash == hash_bytes(header.data(), header.size()));

  // One float changed and the header left as it was: refused, naming the hash.
  Heightfield changed = field;
  changed.data[5] += 1.0f;
  const std::string_view grid(reinterpret_cast<const char*>(changed.data.data()),
                              changed.data.size() * sizeof(f32));
  REQUIRE(io::write_file(tmp.file("bowl.f32"), grid) == io::Status::Ok);
  CHECK_FALSE(read_heightfield(tmp.file("bowl.json"), back, hash, error));
  CHECK(error.find("data_hash") != std::string::npos);

  // An unknown field is refused with its path, and an origin off the lattice with a sentence.
  REQUIRE(io::write_file(tmp.file("odd.json"),
                         R"({"format":"engine.terrain.heightfield","version":1,"width":2,)"
                         R"("height":2,"spacing_mm":1000,"origin_mm":[0,0],"data":"x.f32",)"
                         R"("data_hash":"0000000000000000","colour":1})") == io::Status::Ok);
  CHECK_FALSE(read_heightfield(tmp.file("odd.json"), back, hash, error));
  CHECK(error.find("colour") != std::string::npos);
  Heightfield off = field;
  off.origin_x_mm = 100;
  CHECK_FALSE(write_heightfield(tmp.file("off.json"), off, error));
  CHECK(error.find("whole number of its spacings") != std::string::npos);
}

TEST_CASE("tileset: every tile of every level reads back as the build computed it") {
  SmallSet s;
  REQUIRE_FALSE(s.built.hit);
  TileSet set;
  REQUIRE_MESSAGE(set.open(s.built.manifest, s.error), s.error);
  const TileSetManifest& m = set.manifest();
  CHECK(m.format == "engine.tileset");
  CHECK(m.spacing_mm == 500);
  CHECK(m.origin_mm[0] == -24000);
  CHECK(m.origin_mm[1] == -16000);
  REQUIRE(m.chain.size() == 3);
  CHECK(m.chain[0] == 1000);
  CHECK(m.chain[1] == 2000);
  CHECK(m.chain[2] == 4000);
  REQUIRE(m.levels.size() == 4);
  u32 tiles = 0;
  for (const TileSetLevel& level : m.levels) {
    tiles += level.tiles;
    for (i32 tz = level.tile_min[1]; tz <= level.tile_max[1]; ++tz)
      for (i32 tx = level.tile_min[0]; tx <= level.tile_max[0]; ++tx)
        CHECK(io::exists(io::join_path(s.tmp.file("set"), tile_file(level.spacing_mm, tx, tz))));
  }
  CHECK(tiles == s.built.tiles);
  CHECK(s.built.levels == 4);

  const Grid g = grid_of(s.field, s.options);
  // Every stored point of every level, and three points past the stored window a side, where the
  // edge is held: the finest is the quantized source; a chain level the direct box filter.
  u32 compared = 0;
  u32 differ = 0;
  for (const TileSetLevel& level : m.levels) {
    const i64 i0 = level.index_min[0] - 3;
    const i64 j0 = level.index_min[1] - 3;
    const u32 nx = static_cast<u32>(level.index_max[0] - level.index_min[0] + 7);
    const u32 nz = static_cast<u32>(level.index_max[1] - level.index_min[1] + 7);
    Vector<f32> h(nx * nz);
    REQUIRE(set.source().filtered(0.0, level.spacing_mm, level.filter_mm, static_cast<i32>(i0),
                                  static_cast<i32>(j0), nx, nz,
                                  std::span<f32>(h.data(), h.size())));
    const i64 r = level.spacing_mm / m.spacing_mm;
    for (u32 z = 0; z < nz; ++z) {
      for (u32 x = 0; x < nx; ++x) {
        const i64 I = clamp(i0 + x, level.index_min[0], level.index_max[0]);
        const i64 J = clamp(j0 + z, level.index_min[1], level.index_max[1]);
        const i32 want = level.filter_mm == 0 ? g.at_world(I, J) : direct_filter(g, r, I, J);
        ++compared;
        differ += h[static_cast<u32>(z) * nx + x] == metres(want) ? 0u : 1u;
      }
    }
  }
  MESSAGE(compared << " points of " << m.levels.size() << " levels compared, " << differ
                   << " differ; " << set.reads() << " tiles read");
  CHECK(differ == 0);
  CHECK(set.failures() == 0);
  CHECK(set.reads() == s.built.tiles);
}

TEST_CASE("tileset: the box filter is the surface's mean over each point's own cell") {
  // A plane's mean over any square is the plane at the square's centre: every chain level of a
  // tilted plane is the plane at its points, exactly (the heights are whole millimetres apart).
  test::TempDir tmp{"engine_tileset_plane"};
  Heightfield plane = synth_heightfield(SynthKind::flat, 65, 1000);
  for (u32 j = 0; j < 65; ++j)
    for (u32 i = 0; i < 65; ++i)
      plane.data[j * 65 + i] = 0.003f * static_cast<f32>(i) + 0.005f * static_cast<f32>(j);
  std::string error;
  REQUIRE_MESSAGE(write_heightfield(tmp.file("plane.json"), plane, error), error);
  BuildOptions o;
  o.cells = 8;
  o.ring_spacing_mm = 2000;
  o.far_ratio = 2;
  o.far_levels = 2;
  BuildResult built;
  REQUIRE_MESSAGE(
      ensure_tileset(tmp.file("plane.json"), o, tmp.file("set"), "", false, built, error), error);
  TileSet set;
  REQUIRE_MESSAGE(set.open(built.manifest, error), error);
  const Grid g = grid_of(plane, o);
  for (const i64 spacing : {i64{4000}, i64{8000}}) {
    CAPTURE(spacing);
    const i64 r = spacing / 1000;
    // Inside the data, where the square does not reach past its edge.
    for (i64 J = 1; J < 64 / r; ++J) {
      for (i64 I = 1; I < 64 / r; ++I) {
        f32 h = 0.0f;
        REQUIRE(set.source().filtered(0.0, spacing, spacing, static_cast<i32>(I),
                                      static_cast<i32>(J), 1, 1, std::span<f32>(&h, 1)));
        CHECK(h == metres(3.0 * static_cast<f64>(I * r) + 5.0 * static_cast<f64>(J * r)));
        CHECK(h == metres(direct_filter(g, r, I, J)));
      }
    }
  }
  // And a cliff is smeared over the cell: the steps' terraces at 4 m are between their heights.
  Heightfield steps = synth_heightfield(SynthKind::steps, 65, 1000);
  REQUIRE_MESSAGE(write_heightfield(tmp.file("steps.json"), steps, error), error);
  REQUIRE_MESSAGE(
      ensure_tileset(tmp.file("steps.json"), o, tmp.file("steps"), "", false, built, error), error);
  TileSet terraces;
  REQUIRE_MESSAGE(terraces.open(built.manifest, error), error);
  const Grid gs = grid_of(steps, o);
  u32 between = 0;
  for (i64 I = 0; I <= 16; ++I) {
    f32 h = 0.0f;
    REQUIRE(terraces.source().filtered(0.0, 4000, 4000, static_cast<i32>(I), 8, 1, 1,
                                       std::span<f32>(&h, 1)));
    CHECK(h == metres(direct_filter(gs, 4, I, 8)));
    between += std::fmod(h, 2.0f) != 0.0f ? 1u : 0u;
  }
  CHECK(between > 0);
}

TEST_CASE(
    "tileset: a filter the set was not built with is refused, and a point between is its "
    "surface") {
  SmallSet s;
  TileSet set;
  REQUIRE_MESSAGE(set.open(s.built.manifest, s.error), s.error);
  const scene_gen::TileSource source = set.source();
  CHECK_FALSE(source.moves());
  f32 h[4] = {};
  CHECK_FALSE(source.filtered(0.0, 3000, 3000, 0, 0, 2, 2, std::span<f32>(h, 4)));
  CHECK_FALSE(source.filtered(0.0, 8000, 8000, 0, 0, 2, 2, std::span<f32>(h, 4)));
  CHECK_FALSE(source.filtered(0.0, 2000, 1000, 0, 0, 2, 2, std::span<f32>(h, 4)));
  CHECK(source.filtered(0.0, 2000, 2000, 0, 0, 2, 2, std::span<f32>(h, 4)));
  CHECK(set.has_filter(4000));
  CHECK_FALSE(set.has_filter(500));

  // At 25 cm, finer than the set: its bilinear surface, which is the finest's own points at every
  // other one and their mean between two.
  const Grid g = grid_of(s.field, s.options);
  Vector<f32> fine(9 * 9);
  REQUIRE(source.heights(0.0, 250, 0, 0, 9, 9, std::span<f32>(fine.data(), fine.size())));
  for (u32 z = 0; z < 9; z += 2)
    for (u32 x = 0; x < 9; x += 2)
      CHECK(fine[z * 9 + x] == metres(g.at_world(x / 2, z / 2)));
  CHECK(fine[1] == metres((static_cast<f64>(g.at_world(0, 0)) + g.at_world(1, 0)) * 0.5));
  // At 1.5 m, a multiple of the finest: every third of its points.
  Vector<f32> coarse(4 * 4);
  REQUIRE(source.heights(0.0, 1500, -2, -2, 4, 4, std::span<f32>(coarse.data(), coarse.size())));
  for (u32 z = 0; z < 4; ++z)
    for (u32 x = 0; x < 4; ++x)
      CHECK(coarse[z * 4 + x] == metres(g.at_world((i64{x} - 2) * 3, (i64{z} - 2) * 3)));

  // `height(x, z)`: at a tile's corner the point, at a cell's centre the four's mean, past the
  // data the edge's.
  const i64 x0 = -24000;
  const i64 z0 = -16000;
  for (const i64 k : {i64{0}, i64{16}, i64{48}, i64{96}}) {
    CAPTURE(k);
    CHECK(set.height_mm(x0 + k * 500, z0 + k * 500) == metres(g.at_world(-48 + k, -32 + k)));
  }
  for (const i64 k : {i64{3}, i64{40}, i64{95}}) {
    const f64 mean =
        (static_cast<f64>(g.at_world(-48 + k, -32 + k)) + g.at_world(-48 + k + 1, -32 + k) +
         g.at_world(-48 + k, -32 + k + 1) + g.at_world(-48 + k + 1, -32 + k + 1)) /
        4.0;
    CHECK(set.height_mm(x0 + k * 500 + 250, z0 + k * 500 + 250) == metres(mean));
  }
  CHECK(set.height_mm(x0 - 100000, z0 + 3000) == metres(g.at_world(-48, -32 + 6)));
  CHECK(set.height_mm(x0 + 900000, z0 + 900000) == metres(g.at_world(48, 64)));
}

TEST_CASE("tileset: a second build is a hit, and another source or option is not") {
  test::TempDir tmp{"engine_tileset_cache"};
  const Heightfield field = synth_heightfield(SynthKind::ridge, 33, 1000, 1);
  std::string error;
  REQUIRE_MESSAGE(write_heightfield(tmp.file("ridge.json"), field, error), error);
  BuildOptions o;
  o.cells = 8;
  o.far_levels = 2;
  const std::string ddc = tmp.file("ddc");
  BuildResult first;
  REQUIRE_MESSAGE(ensure_tileset(tmp.file("ridge.json"), o, "", ddc, false, first, error), error);
  CHECK_FALSE(first.hit);
  CHECK(first.manifest == io::join_path(tileset_cache_dir(ddc, first.key), "manifest.json"));
  BuildResult second;
  REQUIRE_MESSAGE(ensure_tileset(tmp.file("ridge.json"), o, "", ddc, false, second, error), error);
  CHECK(second.hit);
  CHECK(second.key == first.key);
  CHECK(second.tiles == first.tiles);
  CHECK(second.hash == first.hash);
  BuildResult forced;
  REQUIRE_MESSAGE(ensure_tileset(tmp.file("ridge.json"), o, "", ddc, true, forced, error), error);
  CHECK_FALSE(forced.hit);
  CHECK(forced.hash == first.hash);
  // Another option is another key; another grid another source hash.
  BuildOptions scaled = o;
  scaled.vertical_scale = 2.0;
  BuildResult other;
  REQUIRE_MESSAGE(ensure_tileset(tmp.file("ridge.json"), scaled, "", ddc, false, other, error),
                  error);
  CHECK_FALSE(other.hit);
  CHECK(other.key != first.key);
  Heightfield changed = field;
  changed.data[100] += 0.5f;
  REQUIRE_MESSAGE(write_heightfield(tmp.file("ridge.json"), changed, error), error);
  BuildResult moved;
  REQUIRE_MESSAGE(ensure_tileset(tmp.file("ridge.json"), o, "", ddc, false, moved, error), error);
  CHECK_FALSE(moved.hit);
  CHECK(moved.key != first.key);
  // A far level the heightfield's points cannot box-filter onto is refused at the build.
  BuildOptions odd = o;
  odd.ring_spacing_mm = 1500;
  odd.far_ratio = 1;
  CHECK_FALSE(ensure_tileset(tmp.file("ridge.json"), odd, "", ddc, false, other, error));
  CHECK(error.find("twice the heightfield's spacing") != std::string::npos);
}

TEST_CASE("tileset: the tiles ground provider, through the registry") {
  SmallSet s;
  const scene_gen::GeneratorRegistry& registry = scene_gen::GeneratorRegistry::global();
  const scene_gen::GroundProviderDesc* desc = registry.find_ground("tiles");
  REQUIRE(desc != nullptr);
  CHECK(desc->flags == scene_gen::k_ground_millimetres);
  scene::Terrain entry;
  entry.provider = "tiles";
  entry.tiles = scene::TerrainTileSet{};
  entry.tiles->manifest = "set/manifest.json";
  scene_gen::Context context;
  const std::string dir = s.tmp.path();
  context.dir = dir;
  scene_gen::GroundProvider ground;
  std::string error;
  REQUIRE_MESSAGE(desc->make(entry, context, ground, &error), error);
  CHECK_FALSE(ground.moves());
  CHECK_FALSE(ground.has_rings());
  CHECK_FALSE(ground.has_tiles());
  f32 wx = 0.0f;
  f32 wz = 0.0f;
  CHECK_FALSE(ground.wind(0.0, wx, wz));
  const TileSet* set = tile_set_of(ground);
  REQUIRE(set != nullptr);
  const Grid g = grid_of(s.field, s.options);
  // The provider's every point entry is the set's surface, at any time.
  CHECK(ground.height(-24.0f, -16.0f) == metres(g.at_world(-48, -32)));
  CHECK(ground.height_mm(1e9, -24000, -16000) == metres(g.at_world(-48, -32)));
  CHECK(ground.surface_mm(-23750, -15750) == set->height_mm(-23750, -15750));
  CHECK(ground.floor_mm(-23750, -15750) == set->height_mm(-23750, -15750));
  CHECK(ground.view().surface_mm(-20000, -10000) ==
        static_cast<f64>(set->height_mm(-20000, -10000)));
  // Its native tile source retains the set's filtering and refusal contract.
  const scene_gen::TileSource through = ground.tiles();
  Vector<f32> a(24 * 24);
  Vector<f32> b(24 * 24);
  REQUIRE(through.filtered(5.0, 2000, 2000, -15, -11, 24, 24, std::span<f32>(a.data(), a.size())));
  REQUIRE(set->source().filtered(0.0, 2000, 2000, -15, -11, 24, 24,
                                 std::span<f32>(b.data(), b.size())));
  CHECK(std::equal(a.begin(), a.end(), b.begin()));
  REQUIRE(through.heights(5.0, 500, -50, -40, 24, 24, std::span<f32>(a.data(), a.size())));
  for (u32 z = 0; z < 24; ++z)
    for (u32 x = 0; x < 24; ++x)
      CHECK(a[z * 24 + x] == metres(g.at_world(-50 + i64{x}, -40 + i64{z})));
  CHECK_FALSE(through.filtered(0.0, 3000, 3000, 0, 0, 2, 2, std::span<f32>(a.data(), 4)));
  CHECK_FALSE(set->source().filtered(0.0, 3000, 3000, 0, 0, 2, 2, std::span<f32>(a.data(), 4)));

  // The refusals, each a sentence.
  scene::Terrain none;
  none.provider = "tiles";
  CHECK_FALSE(desc->make(none, context, ground, &error));
  CHECK(error.find("no \"tiles\" block") != std::string::npos);
  scene::Terrain ridged = entry;
  ridged.ridges.push_back(scene::Ridge{});
  CHECK_FALSE(desc->make(ridged, context, ground, &error));
  CHECK(error.find("ridges") != std::string::npos);
  scene::Terrain missing = entry;
  missing.tiles->manifest = "nowhere/manifest.json";
  CHECK_FALSE(desc->make(missing, context, ground, &error));
  CHECK(error.find("could not open its set") != std::string::npos);
}

TEST_CASE("tileset: residency is bounded across travel, eviction, return and concurrent reads") {
  SmallSet s;
  const u64 tile_bytes = u64{s.options.cells + 3} * (s.options.cells + 3) * sizeof(i32);
  TileSet set(tile_bytes * 2);
  REQUIRE_MESSAGE(set.open(s.built.manifest, s.error), s.error);
  const Grid expected = grid_of(s.field, s.options);
  const auto ask = [&](i32 x, i32 z) {
    f32 h = 0.0f;
    REQUIRE(set.source().heights(1e9, 500, x, z, 1, 1, std::span<f32>(&h, 1)));
    CHECK(h == metres(expected.at_world(x, z)));
    CHECK(set.cache_stats().resident_bytes <= tile_bytes * 2);
    CHECK(set.cache_stats().tiles <= 2);
  };
  for (i32 x = -48; x <= 48; x += 8)
    ask(x, 0);
  for (i32 x = 48; x >= -48; x -= 8)
    ask(x, 0);
  ask(-100000, 100000);  // finite edge holding on a distant teleport
  ask(0, 0);
  CHECK(set.cache_stats().evictions > 0);
  CHECK(set.failures() == 0);
  // Several jobs use the same source while its two slots are continually replaced.
  std::thread a([&] {
    for (i32 i = 0; i < 100; ++i)
      ask(-48 + i % 96, -16);
  });
  std::thread b([&] {
    for (i32 i = 0; i < 100; ++i)
      ask(48 - i % 96, 32);
  });
  a.join();
  b.join();
}

TEST_CASE("tileset: missing or corrupt tile data is refused, never zero ground") {
  SmallSet s;
  // The source covers (0,0); use a fresh reader so an earlier cached copy cannot mask damage.
  const std::string file = io::join_path(io::parent_path(s.built.manifest), tile_file(500, 0, 0));
  std::string original;
  REQUIRE(io::read_file(file, original) == io::Status::Ok);
  for (const bool corrupt : {false, true}) {
    if (corrupt)
      REQUIRE(io::write_file(file, "broken") == io::Status::Ok);
    else {
      std::error_code ec;
      REQUIRE(std::filesystem::remove(file, ec));
    }
    TileSet set;
    REQUIRE_MESSAGE(set.open(s.built.manifest, s.error), s.error);
    f32 h = 123.0f;
    CHECK_FALSE(set.source().heights(0, 500, 0, 0, 1, 1, std::span<f32>(&h, 1)));
    CHECK(set.failures() > 0);
    CHECK(std::isnan(set.height_mm(0, 0)));
    REQUIRE(io::write_file(file, original) == io::Status::Ok);
    CHECK(set.source().heights(0, 500, 0, 0, 1, 1, std::span<f32>(&h, 1)));
  }
}

TEST_CASE("tileset: a fixed footprint reads the same bounded data as source extent grows") {
  for (const u32 side : {129u, 257u, 513u}) {
    const test::TempDir tmp("tileset_extent");
    const auto field = synth_heightfield(SynthKind::ridge, side, 500, 9);
    BuildOptions options;
    options.cells = 16;
    options.far_levels = 0;
    std::string error;
    REQUIRE(write_heightfield(tmp.file("field.json"), field, error));
    BuildResult built;
    REQUIRE(
        ensure_tileset(tmp.file("field.json"), options, tmp.file("set"), "", false, built, error));
    const u64 payload = 19u * 19u * sizeof(i32);
    TileSet set(payload * 4);
    REQUIRE(set.open(built.manifest, error));
    CHECK(set.reads() == 0);
    CHECK(set.cache_stats().resident_bytes == 0);
    Vector<f32> heights(17 * 17);
    REQUIRE(set.source().heights(0, 500, 24, 24, 17, 17,
                                 std::span<f32>(heights.data(), heights.size())));
    const Grid grid = grid_of(field, options);
    for (u32 z = 0; z < 17; ++z)
      for (u32 x = 0; x < 17; ++x)
        CHECK(heights[z * 17 + x] == metres(grid.at_world(24 + x, 24 + z)));
    CHECK(set.reads() == 4);
    CHECK(set.cache_stats().resident_bytes == payload * 4);
    CHECK(set.cache_stats().evictions == 0);
    MESSAGE(side << " source points a side: " << set.reads() << " reads, "
                 << set.cache_stats().resident_bytes << " resident payload bytes for 289 samples");
  }
}

TEST_CASE("tileset: a terrain naming a heightfield builds its set through the cache on a miss") {
  test::TempDir tmp{"engine_tileset_provider_build"};
  const Heightfield field = synth_heightfield(SynthKind::basin, 49, 1000);
  std::string error;
  REQUIRE_MESSAGE(write_heightfield(tmp.file("bowl.json"), field, error), error);
  scene::Terrain entry;
  entry.provider = "tiles";
  entry.tiles = scene::TerrainTileSet{};
  entry.tiles->heightfield = "bowl.json";
  entry.tiles->ddc = "ddc";
  entry.tiles->cells = 16;
  entry.tiles->far_levels = 2;
  entry.tiles->origin = WorldPos{100.0, 2.5, -48.0};
  entry.tiles->vertical_scale = 0.5f;
  scene_gen::Context context;
  const std::string dir = tmp.path();
  context.dir = dir;
  const scene_gen::GroundProviderDesc* desc =
      scene_gen::GeneratorRegistry::global().find_ground("tiles");
  REQUIRE(desc != nullptr);
  scene_gen::GroundProvider ground;
  REQUIRE_MESSAGE(desc->make(entry, context, ground, &error), error);
  // The build it made is the one the content step makes, keyed the same, and now a hit.
  BuildOptions o;
  o.cells = 16;
  o.far_levels = 2;
  o.vertical_scale = 0.5;
  o.offset_x_mm = 100000;
  o.offset_y_mm = 2500;
  o.offset_z_mm = -48000;
  BuildResult again;
  REQUIRE_MESSAGE(
      ensure_tileset(tmp.file("bowl.json"), o, "", tmp.file("ddc"), false, again, error), error);
  CHECK(again.hit);
  // The bowl's centre moved with the origin, half as deep, and lifted by its y.
  CHECK(ground.height(124.0f, -24.0f) == metres(-15000.0 * 0.5 + 2500.0));
  // An origin between the source's points is refused.
  entry.tiles->origin = WorldPos{100.5, 0.0, 0.0};
  CHECK_FALSE(desc->make(entry, context, ground, &error));
  CHECK(error.find("whole number of its spacings") != std::string::npos);
}

// **The reader never holds its lock across work** (tileset.md, "The reader's lock"): a point asked
// on one thread while another evaluates a long window is answered in about its own cost, not
// after the window. Before 2026-10-24 the window held the reader's one lock for its whole
// evaluation, and the point waited for all of it — which is what stalled a walker's tick behind
// the terrain's rebuild after a flight (docs/experiments/walk-stalls-2026-10-24.md).
TEST_CASE("tileset: a point is not held behind another thread's long window") {
  SmallSet s;
  TileSet set;
  REQUIRE_MESSAGE(set.open(s.built.manifest, s.error), s.error);
  const Grid expected = grid_of(s.field, s.options);
  // A window between the finest points (7 mm, the bilinear surface): every point four lookups.
  constexpr u32 n = 1400;
  std::vector<f32> window(static_cast<usize>(n) * n);
  std::atomic<bool> started{false};
  std::atomic<i64> window_ns{0};
  std::thread worker([&] {
    started.store(true);
    const auto from = std::chrono::steady_clock::now();
    CHECK(set.source().heights(0.0, 7, -3000, -2000, n, n, std::span<f32>(window)));
    window_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - from)
                        .count());
  });
  while (!started.load())
    std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  i64 slowest = 0;
  u32 asked = 0;
  while (window_ns.load() == 0 && asked < 200) {
    const i32 x = -40 + static_cast<i32>(asked % 80);
    const auto from = std::chrono::steady_clock::now();
    f32 h = 0.0f;
    REQUIRE(set.source().heights(0.0, 500, x, 8, 1, 1, std::span<f32>(&h, 1)));
    slowest = std::max<i64>(slowest, std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - from)
                                         .count());
    CHECK(h == metres(expected.at_world(x, 8)));
    ++asked;
  }
  worker.join();
  const f64 window_ms = static_cast<f64>(window_ns.load()) / 1.0e6;
  const f64 slowest_ms = static_cast<f64>(slowest) / 1.0e6;
  MESSAGE("a " << n << " x " << n << " window took " << window_ms << " ms; " << asked
               << " points asked beside it, the slowest " << slowest_ms << " ms");
  // The bound is generous for a shared machine and a debug build; held behind the window, the
  // slowest point took the window's own time.
  CHECK(slowest_ms < 50.0);
  if (window_ms > 200.0) CHECK(slowest_ms < window_ms / 4.0);
  CHECK(set.failures() == 0);
}

// **A tile is read once however many ask for it at once**: the first caller claims it and reads it
// outside the lock, and the others wait for that read rather than reading it again.
TEST_CASE("tileset: concurrent callers missing the same tiles read each once") {
  SmallSet s;
  TileSet set;
  REQUIRE_MESSAGE(set.open(s.built.manifest, s.error), s.error);
  const Grid expected = grid_of(s.field, s.options);
  constexpr u32 n = 64;
  std::vector<std::thread> threads;
  std::atomic<u32> mismatches{0};
  for (u32 t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      std::vector<f32> out(static_cast<usize>(n) * n);
      if (!set.source().heights(0.0, 500, -32, -24, n, n, std::span<f32>(out))) {
        mismatches.fetch_add(1);
        return;
      }
      for (u32 z = 0; z < n; ++z)
        for (u32 x = 0; x < n; ++x)
          if (out[z * n + x] !=
              metres(expected.at_world(-32 + static_cast<i32>(x), -24 + static_cast<i32>(z))))
            mismatches.fetch_add(1);
    });
  }
  for (std::thread& t : threads)
    t.join();
  CHECK(mismatches.load() == 0);
  // 64 points from -32 on 16-cell tiles: five tiles a side reached, read once each.
  const TileSet::ReaderStats stats = set.reader_stats();
  CHECK(set.reads() == stats.misses);
  CHECK(set.reads() == set.cache_stats().tiles);
  CHECK(set.cache_stats().evictions == 0);
  MESSAGE(set.reads() << " tiles read for 8 callers; " << stats.load_waits
                      << " waits for another's read, " << stats.hits << " hits");
}
