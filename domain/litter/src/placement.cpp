#include <core/hash/hash.h>
#include <core/math/deterministic.h>
#include <core/schema/json_reflect.h>
#include <domain/litter/litter.h>
#include <domain/scene_gen/scene_gen.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>

namespace engine::litter {
namespace {
struct Entry {
  Layer p;
  Vector<scene_gen::PlacementMesh> meshes;
  u32 cells = 0;
  f32 tile_size = 0;
};
bool fail(std::string* error, std::string_view why) {
  if (error) *error = "litter placement: " + std::string(why);
  return false;
}
bool unit(f32 v) { return std::isfinite(v) && v >= 0 && v <= 1; }
f64 draw(u64 seed, u64 key) {
  return static_cast<f64>(hash_combine(seed, key) >> 11) / 9007199254740992.0;
}
bool open(const JsonValue& params, const scene_gen::Context& c, void** state, std::string* error) {
  *state = nullptr;
  auto e = std::make_unique<Entry>();
  schema::ReadContext read;
  if (!schema::from_json(e->p, params, read) || !read.ok())
    return fail(error, "invalid engine.litter.Layer");
  const auto& p = e->p;
  if (p.cell_m < 1 || p.cell_m > 16 || !std::isfinite(c.tile_size) || c.tile_size < p.cell_m ||
      c.tile_size > 1024 || std::fmod(c.tile_size, static_cast<f32>(p.cell_m)) != 0)
    return fail(error, "cell_m must divide a tile of at most 1024 m");
  e->cells = static_cast<u32>(c.tile_size) / p.cell_m;
  e->tile_size = c.tile_size;
  if (p.max_cells < 1 || p.max_cells > 65536 || e->cells * e->cells > p.max_cells)
    return fail(error, "max_cells cannot hold candidate lattice");
  if (p.min_x > p.max_x || p.min_z > p.max_z) return fail(error, "inverted tile window");
  const u64 w = static_cast<u64>(static_cast<i64>(p.max_x) - p.min_x + 1);
  const u64 d = static_cast<u64>(static_cast<i64>(p.max_z) - p.min_z + 1);
  if (w > 0xffffffffull / d || w * d > 0xffffffffull / (e->cells * e->cells))
    return fail(error, "tile window exceeds stable ID space");
  if (p.max_total_cells == 0 || w * d * e->cells * e->cells > p.max_total_cells)
    return fail(error, "max_total_cells cannot hold the whole tile window");
  if (!(p.extent_m >= .05f && p.extent_m <= p.cell_m * .35f) ||
      !(p.edge_m >= .1f && p.edge_m <= 4) || !(p.max_slope >= 0 && p.max_slope <= 2) ||
      !unit(p.background) || !unit(p.hollow_gain) || !unit(p.obstacle_gain) || p.substrate < -1 ||
      p.substrate >= static_cast<i32>(scene_gen::k_mask_max_palette))
    return fail(error, "invalid extent, edge, slope, probability or substrate");
  if (p.meshes.empty() || p.meshes.size() > 64)
    return fail(error, "meshes must contain 1..64 paths");
  for (const auto& path : p.meshes) {
    if (path.empty()) return fail(error, "empty mesh path");
    e->meshes.push_back(
        {(std::filesystem::path(c.dir) / path).lexically_normal().generic_string(), "litter", 0});
  }
  if (p.substrate >= 0) {
    scene_gen::GroundMasks masks;
    if (!c.ground.masks(masks, error)) return false;
    if (!masks.valid()) return fail(error, "substrate requires cooked ground masks");
  }
  *state = e.release();
  return true;
}
void close(void* state) noexcept { delete static_cast<Entry*>(state); }
bool occupies(const void* state, scene_gen::TileCoord t) noexcept {
  const auto& p = static_cast<const Entry*>(state)->p;
  return t.x >= p.min_x && t.x <= p.max_x && t.z >= p.min_z && t.z <= p.max_z;
}
u8 representation(const void*, const scene_gen::Context&) noexcept { return 0; }
bool meshes(void* state, const scene_gen::Context&, Vector<scene_gen::PlacementMesh>& out,
            std::string*) {
  out = static_cast<Entry*>(state)->meshes;
  return true;
}
bool tile(void* state, scene_gen::TileCoord t, const scene_gen::Context& c,
          scene_gen::Placements& out, std::string* error) {
  out.clear();
  const auto& e = *static_cast<Entry*>(state);
  const auto& p = e.p;
  if (c.tile_size != e.tile_size) return fail(error, "tile size changed after open");
  if (!occupies(state, t)) return true;
  scene_gen::Placements result;
  scene_gen::GroundMasks masks;
  scene_gen::GroundMaskInfo info;
  Vector<u32> words;
  if (p.substrate >= 0) {
    if (!c.ground.masks(masks, error)) return false;
    if (!masks.valid() || !masks.ops->info || !masks.ops->tile ||
        !masks.ops->info(masks.state, info) || info.spacing_mm <= 0 || info.cells < 1 ||
        info.cells > 1024 || (info.cells & (info.cells - 1)) ||
        static_cast<u32>(p.substrate) >= info.palette)
      return fail(error, "invalid ground mask level");
    words.resize(scene_gen::mask_tile_words(info.cells));
  }
  const f64 corner_x = static_cast<f64>(t.x) * static_cast<f64>(c.tile_size),
            corner_z = static_cast<f64>(t.z) * static_cast<f64>(c.tile_size);
  const u64 w = static_cast<u64>(static_cast<i64>(p.max_x) - p.min_x + 1);
  const u64 tile_id = static_cast<u64>(static_cast<i64>(t.z) - p.min_z) * w +
                      static_cast<u64>(static_cast<i64>(t.x) - p.min_x);
  const auto& stand = c.stand.present() ? c.stand : c.ground;
  for (u32 z = 0; z < e.cells; ++z)
    for (u32 x = 0; x < e.cells; ++x) {
      const u32 id = static_cast<u32>(tile_id * e.cells * e.cells + z * e.cells + x + 1);
      const u64 seed = hash_combine(hash_combine(c.world_seed, p.seed), id);
      const f64 wx = corner_x + (x + .4 + .2 * draw(seed, 1)) * p.cell_m;
      const f64 wz = corner_z + (z + .4 + .2 * draw(seed, 2)) * p.cell_m;
      const i64 mx = scene_gen::nearest_mm(wx), mz = scene_gen::nearest_mm(wz);
      f64 density = static_cast<f64>(p.background), edge = 0;
      bool blocked = false;
      for (const auto& f : c.earlier) {
        const f64 fx = static_cast<f64>(f.x_mm) * .001, fz = static_cast<f64>(f.z_mm) * .001;
        // Restrict whole reads to the same contract a streamed tile receives.
        if (fx < corner_x || fx >= corner_x + static_cast<f64>(c.tile_size) || fz < corner_z ||
            fz >= corner_z + static_cast<f64>(c.tile_size))
          continue;
        if (!std::isfinite(f.radius) || f.radius < 0)
          return fail(error, "invalid earlier footprint");
        const f64 dx = wx - fx, dz = wz - fz;
        const f64 gap = std::sqrt(dx * dx + dz * dz) - f.radius - static_cast<f64>(p.extent_m);
        if (gap < 0) {
          blocked = true;
          break;
        }
        edge = std::max(edge, std::max(0.0, 1 - gap / static_cast<f64>(p.edge_m)));
      }
      if (blocked) continue;
      const i64 step = scene_gen::nearest_mm(static_cast<f64>(p.extent_m));
      const f64 h = c.ground.surface_mm(mx, mz);
      const f64 average =
          (c.ground.surface_mm(mx - step, mz) + c.ground.surface_mm(mx + step, mz) +
           c.ground.surface_mm(mx, mz - step) + c.ground.surface_mm(mx, mz + step)) *
          .25;
      if (!std::isfinite(h) || !std::isfinite(average)) return fail(error, "nonfinite ground");
      density += static_cast<f64>(p.hollow_gain) * std::clamp((average - h) / .1, 0.0, 1.0) +
                 static_cast<f64>(p.obstacle_gain) * edge;
      if (p.substrate >= 0) {
        const i64 tx =
            scene_gen::mask_floor_div(scene_gen::mask_floor_div(mx, info.spacing_mm), info.cells);
        const i64 tz =
            scene_gen::mask_floor_div(scene_gen::mask_floor_div(mz, info.spacing_mm), info.cells);
        if (tx < info.tile_min_x || tx > info.tile_max_x || tz < info.tile_min_z ||
            tz > info.tile_max_z)
          continue;
        const auto ref = scene_gen::mask_tile_of(info, mx, 0, mz, 0);
        if (!masks.ops->tile(masks.state, ref.tx, ref.tz, words))
          return fail(error, "mask tile read failed");
        const auto sample = scene_gen::mask_sample(info, words, ref, 0);
        f64 weight = 0;
        for (u32 ch = 0; ch < scene_gen::k_mask_channels; ++ch)
          if (((words[0] >> (ch * 8)) & 255u) == static_cast<u32>(p.substrate))
            weight += static_cast<f64>(sample.weight[ch]);
        density = std::min(1.0, density) * weight;
      }
      if (draw(seed, 3) >= std::min(1.0, density)) continue;
      const f64 sy = stand.surface_mm(mx, mz);
      const f64 sx =
          (stand.surface_mm(mx + step, mz) - stand.surface_mm(mx - step, mz)) / (.002 * step);
      const f64 sz =
          (stand.surface_mm(mx, mz + step) - stand.surface_mm(mx, mz - step)) / (.002 * step);
      if (!std::isfinite(sy) || !std::isfinite(sx) || !std::isfinite(sz))
        return fail(error, "nonfinite standing ground");
      if (sx * sx + sz * sz > static_cast<f64>(p.max_slope) * static_cast<f64>(p.max_slope))
        continue;
      const Vec3 n = normalize(Vec3{static_cast<f32>(-sx), 1, static_cast<f32>(-sz)});
      const Quat tilt = normalize(Quat{n.z, 0, -n.x, 1 + n.y});
      const f64 a = draw(seed, 4) * 3.141592653589793;
      const Quat yaw{0, static_cast<f32>(det::sin(a)), 0, static_cast<f32>(det::cos(a))};
      scene_gen::Placement instance;
      instance.id = id;
      instance.mesh = static_cast<u32>(hash_combine(seed, 5) % e.meshes.size());
      instance.position = {wx, sy, wz};
      instance.rotation = tilt * yaw;
      result.instances.push_back(instance);
    }
  result.things = result.instances.size();
  out = std::move(result);
  return true;
}
bool expand(void* state, const scene_gen::Context& c, scene_gen::Placements& out,
            std::string* error) {
  out.clear();
  const auto& e = *static_cast<Entry*>(state);
  scene_gen::Placements result, one;
  result.meshes = e.meshes;
  for (i64 z = e.p.min_z; z <= e.p.max_z; ++z)
    for (i64 x = e.p.min_x; x <= e.p.max_x; ++x) {
      if (!tile(state, {static_cast<i32>(x), static_cast<i32>(z)}, c, one, error)) return false;
      for (const auto& p : one.instances)
        result.instances.push_back(p);
    }
  result.things = result.instances.size();
  out = std::move(result);
  return true;
}
constexpr scene_gen::PlacementGeneratorDesc k_generator{.name = "litter",
                                                        .open = open,
                                                        .close = close,
                                                        .expand = expand,
                                                        .meshes = meshes,
                                                        .tile = tile,
                                                        .occupies = occupies,
                                                        .representation = representation};
const scene_gen::Registrar k_registrar{k_generator};
}  // namespace
}  // namespace engine::litter
