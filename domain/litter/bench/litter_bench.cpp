// What a cold litter entry costs, and what of it scales with the region (T125; litter.md, "Where a
// cold load's minutes went"): on a "tiles" ground at 1 m whose frame is 1 km by 1 km (argument 1)
// or 3 km by 1 km (argument 3), the mask level's cook over the whole frame, and the entry's whole
// read over the same window of 16 x 16 tiles of 32 m in 4 m cells (16,384 cells) on the cooked
// level, on one thread and on the job system's performance pool. The cook's items are mask tiles;
// the reads' are cells. A smoke run builds a 128 m frame and a 2 x 2 window.
#include <core/jobs/job_system.h>
#include <core/schema/json_reflect.h>
#include <domain/litter/litter.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/tileset/masks.h>
#include <domain/tileset/tileset.h>
#include <foundation/bench/bench.h>

#include <test_temp_dir.h>

#include <memory>
#include <string>

using namespace engine;

namespace {

struct Fixture {
  test::TempDir tmp{"engine_litter_bench"};
  tileset::TileSet set;
  tileset::MaskInputs in;
  tileset::MaskLevel level;
  u32 mask_tiles = 0;
  i32 window = 16;  // tiles a side
  bool ok = false;
};

// Rolling ground: products and sums of whole metres, slopes to about 25 degrees, so the priors lay
// soil and scree in patches.
tileset::Heightfield rolling(u32 width, u32 depth) {
  tileset::Heightfield f;
  f.width = width;
  f.height = depth;
  f.spacing_mm = 1000;
  f.origin_x_mm = -static_cast<i64>(width / 2) * 1000;
  f.origin_z_mm = -static_cast<i64>(depth / 2) * 1000;
  f.data.resize(width * depth);
  for (u32 z = 0; z < depth; ++z)
    for (u32 x = 0; x < width; ++x) {
      const f64 u = static_cast<f64>(x % 160) / 80.0 - 1.0,
                v = static_cast<f64>(z % 120) / 60.0 - 1.0;
      f.data[z * width + x] = static_cast<f32>(18.0 * (1.0 - u * u) * (1.0 - v * v));
    }
  return f;
}

Fixture& fixture(u32 wide) {
  static std::unique_ptr<Fixture> made[4];
  std::unique_ptr<Fixture>& f = made[wide < 4 ? wide : 3];
  if (f != nullptr) return *f;
  f = std::make_unique<Fixture>();
  const bool smoke = bench::smoke_mode();
  const u32 depth = smoke ? 129 : 1025;
  const u32 width = smoke ? 129 : 1024 * wide + 1;
  f->window = smoke ? 2 : 16;
  std::string error;
  const std::string source = f->tmp.file("ground.json");
  if (!tileset::write_heightfield(source, rolling(width, depth), error)) return *f;
  tileset::BuildOptions o;
  o.cells = 64;
  o.ring_spacing_mm = 2000;
  o.far_ratio = 2;
  o.far_levels = 1;
  tileset::BuildResult built;
  if (!tileset::ensure_tileset(source, o, f->tmp.file("set"), std::string(), false, built, error) ||
      !f->set.open(built.manifest, error))
    return *f;
  for (const char* name : {"soil", "scree", "rock", "snow"})
    f->in.palette.push_back(name);
  f->in.budget = 4;
  f->in.map_x0_mm = -static_cast<i64>(width / 2) * 1000;
  f->in.map_z0_mm = -static_cast<i64>(width / 2) * 1000;
  f->in.map_size_mm = static_cast<i64>(width - 1) * 1000;
  f->in.masks.spacing_mm = 1000;
  f->in.masks.tile_cells = 32;
  scene::TerrainMaskPriors p;
  p.soil = "soil";
  p.scree = "scree";
  p.bedrock = "rock";
  p.snow = "snow";
  p.scree_deg = 12;
  p.bedrock_deg = 20;
  p.snowline_m = 1000;
  f->in.masks.priors = p;
  tileset::MaskBuildResult cooked;
  if (!tileset::ensure_mask_level(f->set, f->in, f->tmp.file("masks"), std::string(), false, cooked,
                                  error) ||
      !f->level.open(cooked.manifest, error))
    return *f;
  f->mask_tiles = cooked.tiles;
  f->ok = true;
  return *f;
}

f64 surface(const void* s, i64 x, i64 z) noexcept {
  return static_cast<f64>(static_cast<const Fixture*>(s)->set.height_mm(x, z));
}
bool ground_masks(const void* s, scene_gen::GroundMasks& out, std::string*) {
  out = static_cast<const Fixture*>(s)->level.masks();
  return true;
}

void cook(bench::State& state) {
  Fixture& f = fixture(static_cast<u32>(state.arg()));
  if (!f.ok) return;
  state.set_items(f.mask_tiles);
  std::string error;
  while (state.keep_running()) {
    tileset::MaskBuildResult again;
    bench::keep(tileset::ensure_mask_level(f.set, f.in, f.tmp.file("again"), std::string(), true,
                                           again, error));
  }
}

struct Pool {
  jobs::JobSystem system;
  static jobs::JobSystem* get(void* self) { return &static_cast<Pool*>(self)->system; }
};

void expand(bench::State& state, bool pooled) {
  Fixture& f = fixture(static_cast<u32>(state.arg()));
  if (!f.ok) return;
  scene_gen::Context c;
  c.tile_size = 32;
  c.ground.surface_fn = surface;
  c.ground.masks_fn = ground_masks;
  c.ground.context = &f;
  std::unique_ptr<Pool> pool;
  if (pooled) {
    pool = std::make_unique<Pool>();
    c.jobs_fn = &Pool::get;
    c.jobs_context = pool.get();
  }
  litter::Layer p;
  p.meshes.push_back("leaf.litter.json");
  p.min_x = p.min_z = -f.window / 2;
  p.max_x = p.max_z = f.window / 2 - 1;
  p.cell_m = 4;
  p.extent_m = 1.2f;
  p.background = .4f;
  p.substrate = 0;
  const auto* g = scene_gen::GeneratorRegistry::global().find_placement("litter");
  void* entry = nullptr;
  std::string error;
  if (g == nullptr || !g->open(schema::to_json(p), c, &entry, &error)) return;
  state.set_items(static_cast<u64>(f.window) * static_cast<u64>(f.window) * 64u);
  while (state.keep_running()) {
    scene_gen::Placements out;
    bench::keep(g->expand(entry, c, out, &error));
    bench::keep(out.instances.size());
  }
  g->close(entry);
}

}  // namespace

ENGINE_BENCH_ARGS(mask_cook, "litter.mask_cook", 1, 3) { cook(state); }
ENGINE_BENCH_ARGS(expand_serial, "litter.expand.serial", 1, 3) { expand(state, false); }
ENGINE_BENCH_ARGS(expand_pool, "litter.expand.pool", 1, 3) { expand(state, true); }
