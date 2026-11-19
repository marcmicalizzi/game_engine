// What many points' heights cost through a "tiles" ground's bounded cache, asked in the caller's
// order and by place (W150; scene_gen.md, "Many heights at once"; tileset.md, "Reading a level").
// The forest staging's scene read asks the ground under each of its 470,641 instances, in the
// file's order, through the provider's 64 MiB of tiles: 17.8 s of its warm load, 126,302 tile reads
// for the 53,086 tiles the instances stand on. Here: a 2 km by 2 km ground at 1 m (1,024 tiles of
// 64 m) behind a cache of 58 tiles, and 200,000 points over it in a seeded order; the rows say
// how many tiles each order read. A smoke run is a 256 m ground, a 4-tile cache and 2,000 points.
//
// `ENGINE_BENCH_TILESET` (a tile set's manifest) and `ENGINE_BENCH_SCENE` (a scene file) measure a
// real set behind the provider's own 64 MiB with the scene's instances' points in the file's order,
// read-only: what W150's numbers on the staging were taken with.
#include <core/json/json.h>
#include <core/schema/json_text.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/tileset/tileset.h>
#include <foundation/bench/bench.h>
#include <foundation/io/vfs.h>

#include <test_paths.h>
#include <test_temp_dir.h>

#include <cmath>
#include <memory>
#include <schemas/scene.h>
#include <string>

using namespace engine;

namespace {

struct Fixture {
  std::unique_ptr<test::TempDir> tmp;
  std::unique_ptr<tileset::TileSet> set;
  Vector<i64> x_mm;
  Vector<i64> z_mm;
  bool ok = false;
};

f32 set_height_mm(const void* state, f64, i64 x_mm, i64 z_mm) noexcept {
  return static_cast<const tileset::TileSet*>(state)->height_mm(x_mm, z_mm);
}
f32 set_height(const void* state, f32 x, f32 z) noexcept {
  return static_cast<const tileset::TileSet*>(state)->height_mm(scene_gen::nearest_mm(x),
                                                                scene_gen::nearest_mm(z));
}
const scene_gen::GroundOps k_ops = [] {
  scene_gen::GroundOps ops;
  ops.height = &set_height;
  ops.height_mm = &set_height_mm;
  return ops;
}();

tileset::Heightfield rolling(u32 side) {
  tileset::Heightfield f;
  f.width = side;
  f.height = side;
  f.spacing_mm = 1000;
  f.origin_x_mm = -static_cast<i64>(side / 2) * 1000;
  f.origin_z_mm = -static_cast<i64>(side / 2) * 1000;
  f.data.resize(side * side);
  for (u32 z = 0; z < side; ++z)
    for (u32 x = 0; x < side; ++x)
      f.data[z * side + x] =
          static_cast<f32>(std::sin(x * 0.013) * 9.0 + std::cos(z * 0.011) * 7.0);
  return f;
}

bool real_points(const std::string& scene_path, Fixture& f) {
  std::string text;
  if (io::read_file(scene_path, text) != io::Status::Ok) return false;
  scene::Scene file;
  schema::ReadContext ctx;
  JsonParseResult parsed;
  if (!schema::read_json_text(file, text, ctx, parsed) || !parsed.ok) return false;
  for (const scene::Instance& i : file.instances) {
    f.x_mm.push_back(scene_gen::nearest_mm(i.translation.x));
    f.z_mm.push_back(scene_gen::nearest_mm(i.translation.z));
  }
  return true;
}

Fixture& fixture() {
  static Fixture f;
  if (f.set != nullptr) return f;
  std::string error;
  const std::string manifest = engine::test::detail::environment("ENGINE_BENCH_TILESET");
  const std::string scene = engine::test::detail::environment("ENGINE_BENCH_SCENE");
  if (!manifest.empty() && !scene.empty()) {
    f.set = std::make_unique<tileset::TileSet>(64ull * 1024 * 1024);
    f.ok = f.set->open(manifest, error) && real_points(scene, f);
    return f;
  }
  const bool smoke = bench::smoke_mode();
  const u32 side = smoke ? 257 : 2049;
  const u64 payload = 67ull * 67ull * sizeof(i32);  // a 64-cell tile with its apron
  f.tmp = std::make_unique<test::TempDir>("engine_tileset_heights_bench");
  f.set = std::make_unique<tileset::TileSet>(payload * (smoke ? 4u : 58u));
  const std::string source = f.tmp->file("ground.json");
  tileset::BuildOptions o;
  o.cells = 64;
  o.ring_spacing_mm = 2000;
  o.far_ratio = 2;
  o.far_levels = 1;
  tileset::BuildResult built;
  if (!tileset::write_heightfield(source, rolling(side), error) ||
      !tileset::ensure_tileset(source, o, f.tmp->file("set"), std::string(), false, built, error) ||
      !f.set->open(built.manifest, error))
    return f;
  // Points over the ground in a seeded order, as a file's instances are: kinds one after another,
  // each spread over the whole region.
  const u32 points = smoke ? 2'000 : 200'000;
  const i64 span_mm = static_cast<i64>(side - 1) * 1000;
  u64 state = 0x9E3779B97F4A7C15ull;
  for (u32 k = 0; k < points; ++k) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    f.x_mm.push_back(static_cast<i64>(state % static_cast<u64>(span_mm)) - span_mm / 2);
    f.z_mm.push_back(static_cast<i64>((state >> 32) % static_cast<u64>(span_mm)) - span_mm / 2);
  }
  f.ok = true;
  return f;
}

// Items are the points; bytes the tile records the last pass read (a tile is 17,956 bytes).
void report(bench::State& state, const Fixture& f, u64 io_before) {
  state.set_items(f.x_mm.size());
  state.set_bytes(f.set->reader_stats().io_bytes - io_before);
}

}  // namespace

ENGINE_BENCH(tileset_heights_file_order, "tileset.heights.file_order") {
  Fixture& f = fixture();
  ENGINE_VERIFY(f.ok, "heights bench: no ground");
  const scene_gen::GroundProvider ground(&k_ops, f.set.get());
  Vector<f32> out(f.x_mm.size());
  u64 before = 0;
  while (state.keep_running()) {
    before = f.set->reader_stats().io_bytes;
    for (u32 k = 0; k < f.x_mm.size(); ++k)
      out[k] = ground.height_mm(0.0, f.x_mm[k], f.z_mm[k]);
    bench::keep(out[0]);
  }
  report(state, f, before);
}

ENGINE_BENCH(tileset_heights_by_place, "tileset.heights.by_place") {
  Fixture& f = fixture();
  ENGINE_VERIFY(f.ok, "heights bench: no ground");
  const scene_gen::GroundProvider ground(&k_ops, f.set.get());
  Vector<f32> out(f.x_mm.size());
  u64 before = 0;
  while (state.keep_running()) {
    before = f.set->reader_stats().io_bytes;
    scene_gen::heights_by_place(ground, 0.0, std::span<const i64>(f.x_mm.data(), f.x_mm.size()),
                                std::span<const i64>(f.z_mm.data(), f.z_mm.size()),
                                std::span<f32>(out.data(), out.size()));
    bench::keep(out[0]);
  }
  report(state, f, before);
}
