#include <core/hash/hash.h>
#include <core/math/deterministic.h>
#include <core/schema/json_reflect.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/wetland/wetland.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>

namespace engine::wetland {
namespace {
namespace sg = scene_gen;
struct Entry {
  Patch p;
  Vector<sg::PlacementMesh> meshes;
  u32 nx = 0, nz = 0;
  f64 tile_size = 32;
  i32 min_x = 0, max_x = 0, min_z = 0, max_z = 0;
};
bool fail(std::string* e, std::string text) {
  if (e) *e = "wetland: " + text;
  return false;
}
bool bounded(f64 v, f64 lo, f64 hi) { return v >= lo && v <= hi; }
f64 unit(u64 seed, u64 n) {
  return static_cast<f64>(hash_combine(seed, n) >> 11) / 9007199254740992.0;
}
bool open(const JsonValue& json, const sg::Context& c, void** state, std::string* error) {
  *state = nullptr;
  auto e = std::make_unique<Entry>();
  schema::ReadContext read;
  if (!schema::from_json(e->p, json, read) || !read.ok())
    return fail(error, "invalid engine.wetland.Patch");
  const auto& p = e->p;
  if (!profile(p.species) || p.meshes.empty() || p.meshes.size() > 16 ||
      (!p.far_meshes.empty() && p.far_meshes.size() != p.meshes.size()))
    return fail(error,
                "need a wetland species and 1..16 meshes, with matching optional far_meshes");
  if (!bounded(p.x_m, -1e8, 1e8) || !bounded(p.z_m, -1e8, 1e8) || !bounded(p.radius_x_m, .5, 512) ||
      !bounded(p.radius_z_m, .5, 512) || !bounded(p.spacing_m, .125, 4) ||
      !bounded(p.density, 0, 1) || !bounded(p.irregularity, 0, .75) ||
      !bounded(p.water_y_m, -10000, 10000) || !bounded(p.min_depth_m, -2, 2) ||
      !bounded(p.max_depth_m, p.min_depth_m, 2) || !bounded(p.floating_depth_m, .05, 2) ||
      !bounded(p.clearance_m, 0, 4) || !bounded(p.max_slope_deg, 0, 60) || p.mask_layer < -1 ||
      p.mask_layer >= static_cast<i32>(sg::k_mask_max_palette) || p.candidate_budget < 1 ||
      p.candidate_budget > 1000000 || !bounded(c.tile_size, 1, 1024))
    return fail(error, "Patch field out of range (wetland.schema)");
  e->nx = static_cast<u32>(std::ceil(2.0 * p.radius_x_m / p.spacing_m));
  e->nz = static_cast<u32>(std::ceil(2.0 * p.radius_z_m / p.spacing_m));
  if (static_cast<u64>(e->nx) * e->nz > p.candidate_budget)
    return fail(error, "candidate window exceeds candidate_budget");
  e->tile_size = c.tile_size;
  e->min_x = static_cast<i32>(std::floor((p.x_m - p.radius_x_m) / e->tile_size));
  e->max_x = static_cast<i32>(std::floor((p.x_m + p.radius_x_m) / e->tile_size));
  e->min_z = static_cast<i32>(std::floor((p.z_m - p.radius_z_m) / e->tile_size));
  e->max_z = static_cast<i32>(std::floor((p.z_m + p.radius_z_m) / e->tile_size));
  for (const auto* paths : {&p.meshes, &p.far_meshes})
    for (const auto& path : *paths) {
      if (path.empty()) return fail(error, "empty mesh path");
      e->meshes.push_back(
          {(std::filesystem::path(c.dir) / path).lexically_normal().generic_string(), p.species,
           0});
    }
  if (p.mask_layer >= 0) {
    sg::GroundMasks masks;
    if (!c.ground.masks(masks, error)) return false;
    sg::GroundMaskInfo info;
    if (!masks.valid() || !masks.ops->info || !masks.ops->tile ||
        !masks.ops->info(masks.state, info) || info.spacing_mm <= 0 || info.cells < 1 ||
        info.cells > 1024 || (info.cells & (info.cells - 1)) ||
        static_cast<u32>(p.mask_layer) >= info.palette)
      return fail(error, "mask_layer requires a valid cooked mask palette");
  }
  *state = e.release();
  return true;
}
void close(void* state) noexcept { delete static_cast<Entry*>(state); }
bool occupies(const void* state, sg::TileCoord t) noexcept {
  const auto& e = *static_cast<const Entry*>(state);
  return t.x >= e.min_x && t.x <= e.max_x && t.z >= e.min_z && t.z <= e.max_z;
}
u8 representation(const void* state, const sg::Context& c) noexcept {
  return !static_cast<const Entry*>(state)->p.far_meshes.empty() && c.ring != sg::k_no_ring &&
                 c.ring >= 1
             ? 1
             : 0;
}
bool meshes(void* state, const sg::Context&, Vector<sg::PlacementMesh>& out, std::string*) {
  out = static_cast<Entry*>(state)->meshes;
  return true;
}
// Continuous seeded value noise in patch-local metres: the same field on either side of a tile.
f64 field(u64 seed, f64 x, f64 z) {
  const i64 ix = static_cast<i64>(std::floor(x)), iz = static_cast<i64>(std::floor(z));
  f64 tx = x - static_cast<f64>(ix), tz = z - static_cast<f64>(iz);
  tx = tx * tx * (3 - 2 * tx);
  tz = tz * tz * (3 - 2 * tz);
  auto at = [&](i64 a, i64 b) {
    return unit(hash_combine(seed, static_cast<u64>(a)), static_cast<u64>(b));
  };
  const f64 a = at(ix, iz), b = at(ix + 1, iz), d = at(ix, iz + 1), f = at(ix + 1, iz + 1);
  return (a + (b - a) * tx) * (1 - tz) + (d + (f - d) * tx) * tz;
}
bool generate(const Entry& e, const sg::Context& c, const sg::TileCoord* tile, sg::Placements& out,
              std::string* error) {
  out.clear();
  out.kind = representation(&e, c);
  if (c.tile_size != e.tile_size) return fail(error, "tile size changed after open");
  if (tile && !occupies(&e, *tile)) return true;
  const auto& p = e.p;
  const u64 seed = hash_combine(c.world_seed, p.seed);
  const bool floating = geometry::flora_species_info(profile(p.species)->species).motion ==
                        geometry::FloraMotionKind::none;
  const f64 slope_sin = det::sin(static_cast<f64>(p.max_slope_deg) * 3.14159265358979323846 / 180);
  const f64 max_slope2 = slope_sin * slope_sin / (1 - slope_sin * slope_sin);
  sg::GroundMasks masks;
  sg::GroundMaskInfo info;
  Vector<u32> words;
  if (p.mask_layer >= 0) {
    if (!c.ground.masks(masks, error)) return false;
    if (!masks.valid() || !masks.ops->info || !masks.ops->tile ||
        !masks.ops->info(masks.state, info) || info.spacing_mm <= 0 || info.cells < 1 ||
        info.cells > 1024 || (info.cells & (info.cells - 1)) ||
        static_cast<u32>(p.mask_layer) >= info.palette)
      return fail(error, "mask level became unavailable");
    words.resize(sg::mask_tile_words(info.cells));
  }
  // Bound iteration to this tile's candidate rows, retaining one-cell jitter margin.
  auto first = [&](f64 low, f64 centre, f64 radius, u32 count) {
    return static_cast<u32>(std::clamp(std::floor((low - centre + radius) / p.spacing_m) - 1, 0.0,
                                       static_cast<f64>(count)));
  };
  auto last = [&](f64 high, f64 centre, f64 radius, u32 count) {
    return static_cast<u32>(std::clamp(std::ceil((high - centre + radius) / p.spacing_m) + 1, 0.0,
                                       static_cast<f64>(count)));
  };
  u32 x0 = 0, z0 = 0, x1 = e.nx, z1 = e.nz;
  if (tile) {
    x0 = first(tile->x * e.tile_size, p.x_m, p.radius_x_m, e.nx);
    x1 = last((static_cast<f64>(tile->x) + 1) * e.tile_size, p.x_m, p.radius_x_m, e.nx);
    z0 = first(tile->z * e.tile_size, p.z_m, p.radius_z_m, e.nz);
    z1 = last((static_cast<f64>(tile->z) + 1) * e.tile_size, p.z_m, p.radius_z_m, e.nz);
  }
  for (u32 z = z0; z < z1; ++z)
    for (u32 x = x0; x < x1; ++x) {
      const u32 id = z * e.nx + x + 1;
      const u64 random = hash_combine(seed, id);
      const f64 lx = -p.radius_x_m + (x + .15 + .7 * unit(random, 0)) * p.spacing_m;
      const f64 lz = -p.radius_z_m + (z + .15 + .7 * unit(random, 1)) * p.spacing_m;
      const f64 wx = p.x_m + lx, wz = p.z_m + lz;
      if (tile &&
          (std::floor(wx / e.tile_size) != tile->x || std::floor(wz / e.tile_size) != tile->z))
        continue;
      const f64 nx = lx / p.radius_x_m, nz = lz / p.radius_z_m;
      const f64 noise = field(seed, lx / 2, lz / 2);
      const f64 edge = 1 - p.irregularity * (.2 + .8 * noise);
      if (nx * nx + nz * nz > edge * edge) continue;
      f64 probability = p.density * (1 - p.irregularity * field(hash_combine(seed, 91), lx, lz));
      const i64 xm = sg::nearest_mm(wx), zm = sg::nearest_mm(wz);
      if (p.mask_layer >= 0) {
        const i64 mx = sg::mask_floor_div(sg::mask_floor_div(xm, info.spacing_mm), info.cells);
        const i64 mz = sg::mask_floor_div(sg::mask_floor_div(zm, info.spacing_mm), info.cells);
        if (mx < info.tile_min_x || mx > info.tile_max_x || mz < info.tile_min_z ||
            mz > info.tile_max_z)
          continue;
        const auto ref = sg::mask_tile_of(info, xm, 0, zm, 0);
        if (!masks.ops->tile(masks.state, ref.tx, ref.tz, words))
          return fail(error, "mask tile unavailable inside its declared extent");
        const auto sample = sg::mask_sample(info, words, ref, 0);
        f64 weight = 0;
        for (u32 ch = 0; ch < sg::k_mask_channels; ++ch)
          if (((words[0] >> (8 * ch)) & 255u) == static_cast<u32>(p.mask_layer))
            weight += sample.weight[ch];
        probability *= weight;
      }
      if (unit(random, 2) >= probability) continue;
      const f64 ground = c.ground.surface_mm(xm, zm), depth = p.water_y_m - ground;
      if (!std::isfinite(ground)) return fail(error, "ground returned non-finite height");
      if (depth < p.min_depth_m || depth > p.max_depth_m) continue;
      const f64 dx = c.ground.surface_mm(xm + 250, zm) - c.ground.surface_mm(xm - 250, zm);
      const f64 dz = c.ground.surface_mm(xm, zm + 250) - c.ground.surface_mm(xm, zm - 250);
      if (!std::isfinite(dx) || !std::isfinite(dz))
        return fail(error, "ground returned non-finite slope");
      if (4 * (dx * dx + dz * dz) > max_slope2) continue;
      bool excluded = false;
      for (const auto& prior : c.earlier) {
        if (!(prior.radius > 0)) continue;
        const f64 ax = wx - static_cast<f64>(prior.x_mm) * .001,
                  az = wz - static_cast<f64>(prior.z_mm) * .001;
        const f64 radius = prior.radius + p.clearance_m;
        if (ax * ax + az * az < radius * radius) {
          excluded = true;
          break;
        }
      }
      if (excluded) continue;
      sg::Placement placed;
      placed.id = id;
      placed.mesh = static_cast<u32>(hash_combine(random, 3) % p.meshes.size()) +
                    (out.kind ? p.meshes.size() : 0);
      placed.position = {wx, floating ? p.water_y_m - p.floating_depth_m : ground, wz};
      const f64 angle = unit(random, 4) * 3.14159265358979323846;
      placed.rotation = {0, static_cast<f32>(det::sin(angle)), 0,
                         static_cast<f32>(det::cos(angle))};
      out.instances.push_back(placed);
    }
  out.things = out.instances.size();
  return true;
}
bool tile(void* state, sg::TileCoord t, const sg::Context& c, sg::Placements& out,
          std::string* error) {
  return generate(*static_cast<Entry*>(state), c, &t, out, error);
}
bool expand(void* state, const sg::Context& c, sg::Placements& out, std::string* error) {
  const auto& e = *static_cast<Entry*>(state);
  if (!generate(e, c, nullptr, out, error)) return false;
  out.meshes = e.meshes;
  return true;
}
const sg::PlacementGeneratorDesc k_generator{.name = "wetland",
                                             .open = open,
                                             .close = close,
                                             .expand = expand,
                                             .meshes = meshes,
                                             .tile = tile,
                                             .occupies = occupies,
                                             .representation = representation};
const sg::Registrar k_registrar{k_generator};
}  // namespace
}  // namespace engine::wetland
