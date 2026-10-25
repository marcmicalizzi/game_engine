#include <core/hash/hash.h>
#include <core/schema/json_reflect.h>
#include <domain/scene_gen/scene_gen.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <schemas/flora.h>

namespace engine::flora {
namespace {
struct Entry {
  SwardPlacements params;
  Vector<scene_gen::PlacementMesh> meshes;
  Vector<u32> family_base;
  u32 cells = 0;
};
bool fail(std::string* error, std::string text) {
  if (error) *error = "sward: " + text;
  return false;
}
bool open(const JsonValue& params, const scene_gen::Context& context, void** state,
          std::string* error) {
  *state = nullptr;
  auto entry = std::make_unique<Entry>();
  schema::ReadContext read;
  if (!schema::from_json(entry->params, params, read) || !read.ok())
    return fail(error, "invalid engine.flora.SwardPlacements");
  const auto& p = entry->params;
  if ((p.cell_m != 1 && p.cell_m != 2) || !(context.tile_size >= p.cell_m) ||
      context.tile_size > 1024 || std::fmod(context.tile_size, p.cell_m) != 0)
    return fail(error, "cell_m must be 1 or 2 and divide the tile size (at most 1024 m)");
  entry->cells = static_cast<u32>(context.tile_size / p.cell_m);
  if (p.min_x > p.max_x || p.min_z > p.max_z || p.layer >= scene_gen::k_mask_max_palette)
    return fail(error, "invalid tile window or palette layer");
  const u64 width = static_cast<u64>(static_cast<i64>(p.max_x) - p.min_x + 1);
  const u64 depth = static_cast<u64>(static_cast<i64>(p.max_z) - p.min_z + 1);
  const u64 tile_cells = static_cast<u64>(entry->cells) * entry->cells;
  if (width > 0xffffffffull / depth || width * depth > 0xffffffffull / tile_cells)
    return fail(error, "tile window exceeds the nonzero 32-bit stable cell ID space");
  if (p.families.empty() || p.families.size() > 16)
    return fail(error, "need 1..16 ascending density families");
  f32 previous = 0;
  for (const auto& family : p.families) {
    if (!(family.density > previous && family.density <= 1) || family.near.empty() ||
        family.near.size() > 16 || family.cards.size() != family.near.size())
      return fail(error, "families need ascending density and equal near/cards lists (1..16)");
    previous = family.density;
    entry->family_base.push_back(entry->meshes.size());
    for (const auto* paths : {&family.near, &family.cards}) {
      for (const auto& path : *paths) {
        if (path.empty()) return fail(error, "empty family mesh path");
        const auto resolved = (std::filesystem::path(context.dir) / path).lexically_normal();
        entry->meshes.push_back({resolved.generic_string(), "sward", 0});
      }
    }
  }
  if (previous != 1) return fail(error, "last density family must cover weight 1");
  scene_gen::GroundMasks masks;
  if (!context.ground.masks(masks, error)) return false;
  if (!masks.valid()) return fail(error, "requires the cooked terrain mask level");
  *state = entry.release();
  return true;
}
void close(void* state) noexcept { delete static_cast<Entry*>(state); }
bool occupies(const void* state, scene_gen::TileCoord tile) noexcept {
  const auto& p = static_cast<const Entry*>(state)->params;
  return tile.x >= p.min_x && tile.x <= p.max_x && tile.z >= p.min_z && tile.z <= p.max_z;
}
u8 representation(const void*, const scene_gen::Context& context) noexcept {
  return context.ring == scene_gen::k_no_ring ? 0 : std::min<u8>(context.ring, 2);
}
bool meshes(void* state, const scene_gen::Context&, Vector<scene_gen::PlacementMesh>& out,
            std::string*) {
  out = static_cast<const Entry*>(state)->meshes;
  return true;
}
bool tile(void* state, scene_gen::TileCoord at, const scene_gen::Context& context,
          scene_gen::Placements& out, std::string* error) {
  out.clear();
  out.kind = representation(state, context);
  if (out.kind >= 2 || !occupies(state, at)) return true;
  const auto& entry = *static_cast<const Entry*>(state);
  const auto& p = entry.params;
  scene_gen::GroundMasks masks;
  if (!context.ground.masks(masks, error)) return false;
  scene_gen::GroundMaskInfo info;
  if (!masks.valid() || masks.ops->info == nullptr || masks.ops->tile == nullptr ||
      !masks.ops->info(masks.state, info) || info.spacing_mm <= 0 || info.cells < 1 ||
      (info.cells & (info.cells - 1)) != 0 || info.cells > 1024 || p.layer >= info.palette)
    return fail(error, "invalid or missing mask level/layer");
  Vector<u32> words(scene_gen::mask_tile_words(info.cells));
  i32 last_x = 0, last_z = 0;
  bool loaded = false, found = false;
  const u64 width = static_cast<u64>(static_cast<i64>(p.max_x) - p.min_x + 1);
  const u64 tile_id = static_cast<u64>(static_cast<i64>(at.z) - p.min_z) * width +
                      static_cast<u64>(static_cast<i64>(at.x) - p.min_x);
  for (u32 z = 0; z < entry.cells; ++z) {
    for (u32 x = 0; x < entry.cells; ++x) {
      const f64 wx = static_cast<f64>(at.x) * static_cast<f64>(context.tile_size) +
                     (x + .5) * static_cast<f64>(p.cell_m);
      const f64 wz = static_cast<f64>(at.z) * static_cast<f64>(context.tile_size) +
                     (z + .5) * static_cast<f64>(p.cell_m);
      const i64 x_mm = scene_gen::nearest_mm(wx), z_mm = scene_gen::nearest_mm(wz);
      // Check in i64 before mask_tile_of narrows its tile coordinates to i32. An unrelated
      // distant placement window must not wrap into the authored mask level.
      const i64 mask_x =
          scene_gen::mask_floor_div(scene_gen::mask_floor_div(x_mm, info.spacing_mm), info.cells);
      const i64 mask_z =
          scene_gen::mask_floor_div(scene_gen::mask_floor_div(z_mm, info.spacing_mm), info.cells);
      if (mask_x < info.tile_min_x || mask_x > info.tile_max_x || mask_z < info.tile_min_z ||
          mask_z > info.tile_max_z)
        continue;
      const auto ref = scene_gen::mask_tile_of(info, x_mm, 0, z_mm, 0);
      if (!loaded || last_x != ref.tx || last_z != ref.tz) {
        last_x = ref.tx;
        last_z = ref.tz;
        loaded = true;
        found = masks.ops->tile(masks.state, ref.tx, ref.tz, words);
      }
      if (!found) continue;
      const auto sample = scene_gen::mask_sample(info, words, ref, 0);
      f32 density = 0;
      for (u32 ch = 0; ch < scene_gen::k_mask_channels; ++ch)
        if (((words[0] >> (ch * 8)) & 255u) == p.layer) density += sample.weight[ch];
      if (!(density > 0)) continue;
      bool excluded = false;
      for (const auto& earlier : context.earlier) {
        if (!(earlier.radius > 0)) continue;
        const f64 dx = wx - static_cast<f64>(earlier.x_mm) * .001;
        const f64 dz = wz - static_cast<f64>(earlier.z_mm) * .001;
        // The full patch clears earlier structures, not merely its centre.
        const f64 radius = earlier.radius + .7071067811865476 * static_cast<f64>(p.cell_m);
        if (dx * dx + dz * dz < radius * radius) {
          excluded = true;
          break;
        }
      }
      if (excluded) continue;
      const u32 id =
          static_cast<u32>(tile_id * entry.cells * entry.cells + z * entry.cells + x + 1);
      const u64 random = hash_combine(hash_combine(context.world_seed, p.seed), id);
      // Unbiased stochastic interpolation between adjacent authored density classes. The field
      // is identical in all rings; ring and request order never participate in the hash.
      u32 upper = 0;
      while (upper + 1 < p.families.size() && p.families[upper].density < density)
        ++upper;
      const f32 low = upper == 0 ? 0 : p.families[upper - 1].density;
      const f32 t = (density - low) / (p.families[upper].density - low);
      const f64 unit = static_cast<f64>(random >> 11) / 9007199254740992.0;
      if (unit >= static_cast<f64>(t)) {
        if (upper == 0) continue;
        --upper;
      }
      const auto& family = p.families[upper];
      scene_gen::Placement placed;
      placed.id = id;
      placed.mesh = entry.family_base[upper] + static_cast<u32>(random % family.near.size()) +
                    (out.kind == 1 ? family.near.size() : 0);
      placed.position = {wx, context.ground.surface_mm(x_mm, z_mm), wz};
      // A height field is sampled per patch. Slope conformity and wetness-specific morphology
      // require separately cooked families; silently scaling real leaf dimensions is forbidden.
      out.instances.push_back(placed);
    }
  }
  return true;
}
bool expand(void* state, const scene_gen::Context& context, scene_gen::Placements& out,
            std::string* error) {
  out.clear();
  const auto& entry = *static_cast<const Entry*>(state);
  out.meshes = entry.meshes;
  scene_gen::Placements one;
  // i64 loop counters avoid overflow at an inclusive INT_MAX tile bound.
  for (i64 z = entry.params.min_z; z <= entry.params.max_z; ++z)
    for (i64 x = entry.params.min_x; x <= entry.params.max_x; ++x) {
      if (!tile(state, {static_cast<i32>(x), static_cast<i32>(z)}, context, one, error))
        return false;
      for (const auto& p : one.instances)
        out.instances.push_back(p);
    }
  return true;
}
const scene_gen::PlacementGeneratorDesc k_generator{.name = "sward",
                                                    .open = &open,
                                                    .close = &close,
                                                    .expand = &expand,
                                                    .meshes = &meshes,
                                                    .tile = &tile,
                                                    .occupies = &occupies,
                                                    .representation = &representation};
const scene_gen::Registrar k_registrar{k_generator};
}  // namespace
}  // namespace engine::flora
