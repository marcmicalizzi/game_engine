// The `tiles` ground provider (docs/subsystems/tileset.md, "The provider"; scene_gen.md, "The
// registrar pattern"): a scene's terrain naming it opens an authored tile set — the one its
// `tiles` block's `manifest` names, or the one its `heightfield` builds into the derived-data
// cache on a miss — and answers every height from it. It has no time: the set does not move, so
// it has no `evaluate`, `travel_m`, `wind` or `transport`, and a time-lapse moves nothing of it.
#include <domain/geometry/cluster_file.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/tileset/masks.h>
#include <domain/tileset/tileset.h>
#include <foundation/io/vfs.h>
#include <foundation/tunables/tunables.h>

#include <memory>
#include <mutex>
#include <schemas/scene.h>

namespace engine::tileset {

namespace {

tunables::Int cache_mib{"tileset.cache_mib", 64, 1, 16384,
                        "Resident authored terrain tile payload per provider, MiB"};

struct TilesGround {
  TilesGround() : set(static_cast<u64>(cache_mib.get()) * 1024 * 1024) {}
  TileSet set;
  // The terrain's `masks` block, built beside the set on first ask (`GroundOps::masks`), not at
  // make: a sampler is made for every height a scene read asks, and the build's key hashes every
  // image the block names.
  bool has_masks = false;
  MaskInputs mask_inputs;
  std::string ddc;
  std::mutex mask_mutex;
  bool mask_tried = false;
  std::string mask_error;
  MaskLevel masks;
};

const TileSet& set_of(const void* state) noexcept {
  return static_cast<const TilesGround*>(state)->set;
}

void tiles_destroy(void* state) noexcept { delete static_cast<TilesGround*>(state); }

f32 tiles_height(const void* state, f32 x, f32 z) noexcept {
  return set_of(state).height_mm(scene_gen::nearest_mm(static_cast<f64>(x)),
                                 scene_gen::nearest_mm(static_cast<f64>(z)));
}

f32 tiles_height_mm(const void* state, f64, i64 x_mm, i64 z_mm) noexcept {
  return set_of(state).height_mm(x_mm, z_mm);
}

f32 tiles_surface_mm(const void* state, i64 x_mm, i64 z_mm) noexcept {
  return set_of(state).height_mm(x_mm, z_mm);
}

scene_gen::TileSource tiles_source(const void* state) noexcept { return set_of(state).source(); }

bool tiles_masks(const void* state, scene_gen::GroundMasks& out, std::string* error) {
  out = scene_gen::GroundMasks{};
  auto& g = *const_cast<TilesGround*>(static_cast<const TilesGround*>(state));
  if (!g.has_masks) return true;
  const std::lock_guard<std::mutex> lock(g.mask_mutex);
  if (!g.mask_tried) {
    g.mask_tried = true;
    MaskBuildResult built;
    std::string why;
    if (!ensure_mask_level(g.set, g.mask_inputs, std::string(), g.ddc, false, built, why) ||
        !g.masks.open(built.manifest, why))
      g.mask_error = "the ground provider \"tiles\" could not build its masks: " + why;
  }
  if (!g.mask_error.empty()) {
    if (error != nullptr) *error = g.mask_error;
    return false;
  }
  out = g.masks.masks();
  return true;
}

// A grid: the scene's own (its float coordinates' nearest millimetres) or a window of the world's
// lattice. **It cannot refuse** (`GroundOps::grid` returns nothing), so a lattice filtered to a
// spacing the chain lacks is answered with the ground at its points. Lattice consumers instead
// get the set's native source through `GroundProvider::tiles()`, which retains refusals.
void tiles_grid(const void* state, const scene_gen::Lattice& lattice, i32 i0, i32 j0, u32 nx,
                u32 nz, std::span<f32> heights) noexcept {
  const TileSet& set = set_of(state);
  if (!lattice.scene_grid && lattice.spacing_mm > 0 &&
      set.heights(lattice.spacing_mm, lattice.filter_mm, i0, j0, nx, nz, 0,
                  scene_gen::window_blocks(nx, nz), heights))
    return;
  for (u32 z = 0; z < nz; ++z) {
    for (u32 x = 0; x < nx; ++x) {
      heights[static_cast<usize>(z) * nx + x] =
          set.height_mm(lattice.x_mm(i64{i0} + x), lattice.z_mm(i64{j0} + z));
    }
  }
}

constexpr scene_gen::GroundOps k_tiles_ops{
    .destroy = &tiles_destroy,
    .height = &tiles_height,
    .floor = nullptr,
    .grid = &tiles_grid,
    .evaluate = nullptr,
    .travel_m = nullptr,
    .wind = nullptr,
    .transport = nullptr,
    .make_rings = nullptr,
    .open_tiles = nullptr,
    .record = nullptr,
    .height_mm = &tiles_height_mm,
    .surface_mm = &tiles_surface_mm,
    .floor_mm = &tiles_surface_mm,
    .tile_source = &tiles_source,
    .masks = &tiles_masks,
};

std::string resolve(std::string_view dir, const std::string& path) {
  return io::is_absolute_path(path) || dir.empty() ? path : io::join_path(dir, path);
}

bool tiles_make(const scene::Terrain& entry, const scene_gen::Context& context,
                scene_gen::GroundProvider& out, std::string* error) {
  const auto fail = [&](std::string sentence) {
    if (error != nullptr) *error = std::move(sentence);
    return false;
  };
  if (!entry.tiles.has_value()) {
    return fail(
        "the ground provider \"tiles\" is given no tile set: the terrain has no \"tiles\" block "
        "naming a manifest or a heightfield");
  }
  if (!entry.ridges.empty() || !entry.basins.empty()) {
    return fail(
        "the ground provider \"tiles\" draws an authored terrain, which carries its own ridges and "
        "basins; the terrain names some besides");
  }
  const scene::TerrainTileSet& t = *entry.tiles;
  std::string manifest;
  std::string ddc = t.ddc.empty() ? std::string() : resolve(context.dir, t.ddc);
  if (!t.manifest.empty()) {
    manifest = resolve(context.dir, t.manifest);
    if (ddc.empty()) ddc = geometry::find_ddc_root(io::parent_path(manifest));
  } else if (!t.heightfield.empty()) {
    // **The content step as a library call**, as a scene mesh's clusters are built on a miss: the
    // set of this heightfield and these options, from the derived-data cache, built there first
    // when it is not.
    const std::string source = resolve(context.dir, t.heightfield);
    BuildOptions options;
    options.cells = t.cells;
    options.ring_spacing_mm = t.ring_spacing_mm;
    options.far_ratio = t.far_ratio;
    options.far_levels = t.far_levels;
    options.vertical_scale = static_cast<f64>(t.vertical_scale);
    options.offset_x_mm = scene_gen::nearest_mm(t.origin.x);
    options.offset_y_mm = scene_gen::nearest_mm(t.origin.y);
    options.offset_z_mm = scene_gen::nearest_mm(t.origin.z);
    if (ddc.empty()) ddc = geometry::find_ddc_root(io::parent_path(source));
    BuildResult built;
    std::string why;
    if (!ensure_tileset(source, options, std::string(), ddc, false, built, why))
      return fail("the ground provider \"tiles\" could not build its set: " + why);
    manifest = built.manifest;
  } else {
    return fail("the ground provider \"tiles\" names neither a manifest nor a heightfield");
  }
  auto ground = std::make_unique<TilesGround>();
  std::string why;
  if (!ground->set.open(manifest, why))
    return fail("the ground provider \"tiles\" could not open its set: " + why);
  if (entry.masks.has_value()) {
    // The mask level, built beside the set on a miss as the set itself is (masks.h), on first ask.
    if (!mask_inputs(entry, context.dir, ground->mask_inputs, why) ||
        !validate_mask_inputs(ground->mask_inputs, why))
      return fail("the ground provider \"tiles\" cannot build its masks: " + why);
    ground->has_masks = true;
    ground->ddc = ddc;
  }
  out = scene_gen::GroundProvider(&k_tiles_ops, ground.release());
  return true;
}

constexpr scene_gen::GroundProviderDesc k_tiles{
    .name = "tiles",
    .make = &tiles_make,
    .flags = scene_gen::k_ground_millimetres,
};
const scene_gen::Registrar k_tiles_registrar{k_tiles};

}  // namespace

const TileSet* tile_set_of(const scene_gen::GroundProvider& provider) noexcept {
  if (provider.ops() != &k_tiles_ops || provider.state() == nullptr) return nullptr;
  return &set_of(provider.state());
}

}  // namespace engine::tileset
