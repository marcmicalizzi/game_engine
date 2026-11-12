// The scene generator "deposits" (rocks.md, "Terrain-aware deposits (G128)"): a placement
// generator through the scene-generator registry that plans a region's rockfall and sediment once
// and hands each tile its patches' recipes when the tile activates (G191; scene_gen.md, "A tile's
// meshes"; ADR-0061), so nothing of a deposit exists on a host before its tile does.
//
// **What it holds.** The plan (its per-sample arrays and its blocks) for the entry's life, kept
// once a process by its inputs' key (T120, `PlanCache` below), and each candidate patch's place. A
// patch's recipe is made from the plan when a tile asks for it, and is the same bytes whenever and
// in whatever order its tile comes in.
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/log/log.h>
#include <core/schema/json_reflect.h>
#include <domain/rocks/deposits.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>

namespace engine::rocks {
namespace {
ENGINE_LOG_CATEGORY_DEFINE(log_deposits, "deposits");
// G191: a streamed host admits a tile's mesh of at most 4,096 clusters
// (`renderer.meshes.max_clusters`; the stream bed's row met it). At 124 triangles a cluster and the
// cluster DAG's levels above its leaves, a patch of up to 240,000 triangles stays under it.
constexpr u64 k_patch_triangles_max = 240000;
struct PatchPlace {
  scene_gen::Placement placement;
  scene_gen::TileCoord tile;
  f64 width = 0;
  f64 depth = 0;
  i64 bx = 0;  // the patch's cell on the layer's patch lattice
  i64 bz = 0;
};
struct Entry {
  DepositLayer layer;
  const DepositPlan* plan = nullptr;  // held through `PlanCache`
  u64 key = 0;
  u64 world_seed = 0;
  std::string root;  // where a recipe's file would be: nothing is written there
  i64 patch_mm = 0;
  i64 bx0 = 0, bz0 = 0, columns = 0, rows = 0;
  Vector<u32> first_block, block_order;
  Vector<PatchPlace> patches;
  ~Entry();
};
i64 floor_div(i64 a, i64 b) {
  const i64 q = a / b;
  return q - (a % b < 0 ? 1 : 0);
}
bool refused(std::string* error, std::string message) {
  if (error) *error = std::move(message);
  return false;
}
// DepositLayer 1 -> 2 (G143): version 1's capped apron, `talus_depth_m`, is gone; the supply rule
// replaces it. A layer that set it to zero asked for no rockfall deposit, which is `supply_m` 0.
bool migrate_layer_1(JsonValue& object, schema::ReadContext&) {
  if (!object.is_object()) return true;
  const JsonValue* depth = object.find("talus_depth_m");
  f64 value = 1.0;
  const bool none = depth != nullptr && depth->get_f64(value) && value <= 0.0;
  object.as_object().erase(std::string("talus_depth_m"));
  if (none) object["supply_m"] = JsonValue(0.0);
  return true;
}
// DepositLayer 2 -> 3 (G191): `far_rings` is new and takes its default; nothing else moved.
bool migrate_layer_2(JsonValue&, schema::ReadContext&) { return true; }
const bool k_migration_registered = [] {
  schema::MigrationRegistry::global().add({"engine.rocks.DepositLayer", 1, &migrate_layer_1});
  schema::MigrationRegistry::global().add({"engine.rocks.DepositLayer", 2, &migrate_layer_2});
  return true;
}();

// ---- T120: the plan once a load (rocks.md, "A tile solves") ----
// The scene reader opens an entry to read it and closes it, and the world's placements consumer
// opens it again: each open planned the whole region (the forest's two "deposits planned" lines).
// A plan is a function of the layer, the world seed and the ground's heights over the region, so
// it is kept by a key over those for the process: an open that finds its key holds the plan it
// finds, and the last plan let go is kept until another is made, so a close and the next open of
// the same entry plan once. The key reads the ground (sampling is the plan's first step and is
// repeated), never a file, so two roots or a consumer that hands none still share it.
struct CachedPlan {
  u64 key = 0;
  u32 holders = 0;
  std::unique_ptr<DepositPlan> plan;
};
struct PlanCache {
  std::mutex mutex;
  Vector<CachedPlan> plans;  // a few: one a live entry, and the last let go
  u64 plans_made = 0;
};
PlanCache& plan_cache() {
  static PlanCache cache;
  return cache;
}
const DepositPlan* hold_plan(u64 key) {
  PlanCache& c = plan_cache();
  std::lock_guard<std::mutex> lock(c.mutex);
  for (auto& p : c.plans)
    if (p.key == key) {
      ++p.holders;
      return p.plan.get();
    }
  return nullptr;
}
const DepositPlan* keep_plan(u64 key, DepositPlan&& plan) {
  PlanCache& c = plan_cache();
  std::lock_guard<std::mutex> lock(c.mutex);
  for (auto& p : c.plans)
    if (p.key == key) {  // another open made the same plan meanwhile: hold that one
      ++p.holders;
      return p.plan.get();
    }
  // Let go every plan nobody holds: the newest unheld one is the one this replaces.
  for (u32 k = c.plans.size(); k-- > 0;)
    if (c.plans[k].holders == 0) c.plans.erase_at(k);
  CachedPlan entry;
  entry.key = key;
  entry.holders = 1;
  entry.plan = std::make_unique<DepositPlan>(std::move(plan));
  ++c.plans_made;
  c.plans.push_back(std::move(entry));
  return c.plans.back().plan.get();
}
void release_plan(u64 key) {
  PlanCache& c = plan_cache();
  std::lock_guard<std::mutex> lock(c.mutex);
  for (auto& p : c.plans)
    if (p.key == key && p.holders > 0) {
      --p.holders;
      return;
    }
}
Entry::~Entry() {
  if (plan != nullptr) release_plan(key);
}
// The plan's key: the layer's canonical bytes (its version and the solver's), the world seed, and
// every height the plan samples, in whole micrometres, in its own order.
bool plan_key(const DepositLayer& layer, const scene_gen::Ground& ground,
              const scene_gen::Ground* uncut, u64 world_seed, u64& out, std::string& error) {
  if (!ground.present()) {
    error = "engine.rocks deposits: invalid ground (required)";
    return false;
  }
  JsonWriteOptions compact;
  compact.pretty = false;
  const std::string text = write_json(schema::to_json(layer), compact);
  u64 h = hash_combine(hash_bytes(text.data(), text.size()),
                       hash_combine(world_seed, k_deposit_version));
  const i64 spacing = scene_gen::nearest_mm(static_cast<f64>(layer.spacing_m));
  if (spacing <= 0) {
    error = "engine.rocks deposits: invalid spacing_m";
    return false;
  }
  const i64 x0 = scene_gen::nearest_mm(layer.region_min[0]),
            z0 = scene_gen::nearest_mm(layer.region_min[1]);
  const i64 x1 = scene_gen::nearest_mm(layer.region_max[0]),
            z1 = scene_gen::nearest_mm(layer.region_max[1]);
  if (!(x1 > x0 && z1 > z0) || (x1 - x0) / spacing + 1 > layer.max_samples ||
      (z1 - z0) / spacing + 1 > layer.max_samples ||
      ((x1 - x0) / spacing + 1) * ((z1 - z0) / spacing + 1) > static_cast<i64>(layer.max_samples)) {
    out = h;  // the plan refuses it with its own sentence
    return true;
  }
  // The ground, and the ground before its edits where it differs (G190: what a face sheds).
  for (const scene_gen::Ground* g : {&ground, uncut}) {
    if (g == nullptr) continue;
    h = hash_combine(h, g == uncut ? 0x75ull : 0x67ull);
    for (i64 z = z0; z <= z1; z += spacing)
      for (i64 x = x0; x <= x1; x += spacing) {
        const f64 y = g->surface_mm(x, z);
        const i64 um = std::isfinite(y) && std::fabs(y) < 1e9 ? std::llround(y * 1e6) : i64{0x7ff0};
        h = hash_combine(h, static_cast<u64>(um));
      }
  }
  out = h;
  return true;
}

// A patch of the plan, its corner `x0`, `z0` and its height `y0`, and whether it holds anything:
// what `open` checks and `tile` hands, made the same way by both.
bool make_patch(const Entry& e, i64 bx, i64 bz, u32 id, DepositPatch& p, i64& x0, i64& z0, f64& y0,
                i64& x1, i64& z1, bool& any, f64& overhang) {
  const DepositPlan& plan = *e.plan;
  const i64 end_x = plan.x_mm + static_cast<i64>(plan.width - 1) * plan.spacing_mm;
  const i64 end_z = plan.z_mm + static_cast<i64>(plan.depth - 1) * plan.spacing_mm;
  x0 = std::max(plan.x_mm, bx * e.patch_mm);
  z0 = std::max(plan.z_mm, bz * e.patch_mm);
  x1 = std::min(end_x, (bx + 1) * e.patch_mm);
  z1 = std::min(end_z, (bz + 1) * e.patch_mm);
  const u32 ix = static_cast<u32>((x0 - plan.x_mm) / plan.spacing_mm);
  const u32 iz = static_cast<u32>((z0 - plan.z_mm) / plan.spacing_mm);
  p = e.layer.patch;
  p.seed = hash_combine(p.seed, hash_combine(e.layer.seed, hash_combine(e.world_seed, id)));
  p.width = static_cast<u32>((x1 - x0) / plan.spacing_mm) + 1;
  p.depth = static_cast<u32>((z1 - z0) / plan.spacing_mm) + 1;
  p.spacing_m = e.layer.spacing_m;
  // G143's third pass: the corner the bed's clasts are anchored to (rocks.md, "The bed is rubble").
  p.origin_mm = {x0, z0};
  p.ground.clear();
  p.thickness.clear();
  p.coarse.clear();
  p.sink.clear();
  p.slope.clear();
  p.blocks.clear();
  y0 = plan.ground[iz * plan.width + ix];
  any = false;
  for (u32 z = 0; z < p.depth; ++z)
    for (u32 x = 0; x < p.width; ++x) {
      const u32 i = (iz + z) * plan.width + ix + x;
      p.ground.push_back(static_cast<f32>(plan.ground[i] - y0));
      p.thickness.push_back(plan.thickness[i]);
      p.coarse.push_back(plan.coarse[i]);
      // G143: the edge field and the top's gradient, from the plan (rocks.md, "The edge and the
      // seams").
      p.sink.push_back(plan.sink[i]);
      p.slope.push_back(plan.slope[2 * i]);
      p.slope.push_back(plan.slope[2 * i + 1]);
      any = any || plan.thickness[i] > .001f;
    }
  // G143: the patch's blocks, local to its corner, and its rock: the bed most of its deposit and
  // its blocks were shed from (a patch is one material; the lowest type wins a tie).
  const u32 slot = static_cast<u32>((bz - e.bz0) * e.columns + (bx - e.bx0));
  u32 votes[static_cast<u32>(RockType::Dolostone) + 1] = {};
  overhang = 0;
  for (u32 k = e.first_block[slot]; k < e.first_block[slot + 1]; ++k) {
    const DepositPlanBlock& b = plan.blocks[e.block_order[k]];
    DepositBlock local;
    local.x_m = static_cast<f32>(static_cast<f64>(b.x_mm - x0) / 1000.0);
    local.z_m = static_cast<f32>(static_cast<f64>(b.z_mm - z0) / 1000.0);
    local.y_m = static_cast<f32>(b.y_m - y0);
    for (u32 a = 0; a < 3; ++a)
      local.half[a] = static_cast<f32>(b.half[a]);
    local.slope[0] = static_cast<f32>(b.slope[0]);
    local.slope[1] = static_cast<f32>(b.slope[1]);
    local.yaw = static_cast<f32>(b.yaw);
    local.buried = static_cast<f32>(b.buried);
    local.seed = b.seed;
    p.blocks.push_back(local);
    overhang = std::max(overhang, std::max(b.half[0], b.half[2]));
    ++votes[static_cast<u32>(b.type)];
  }
  any = any || !p.blocks.empty();
  if (!plan.rock.empty()) {
    for (u32 z = 0; z < p.depth; ++z)
      for (u32 x = 0; x < p.width; ++x) {
        const u32 i = (iz + z) * plan.width + ix + x;
        if (plan.thickness[i] > .001f) ++votes[static_cast<u32>(plan.rock[i])];
      }
    u32 best = 0;
    for (u32 t = 1; t <= static_cast<u32>(RockType::Dolostone); ++t)
      if (votes[t] > votes[best]) best = t;
    if (votes[best] > 0) p.type = static_cast<RockType>(best);
  }
  return true;
}

bool open(const JsonValue& params, const scene_gen::Context& context, void** state,
          std::string* error) {
  *state = nullptr;
  auto e = std::make_unique<Entry>();
  schema::ReadContext reader;
  // A placement entry carries no version; only a version-1 layer names `talus_depth_m`.
  const bool version_1 = params.is_object() && params.find("talus_depth_m") != nullptr;
  if (!(version_1 ? schema::from_json_versioned(e->layer, params, 1, reader)
                  : schema::from_json(e->layer, params, reader)) ||
      !reader.ok()) {
    std::string message = "engine.rocks.DepositLayer";
    for (const auto& d : reader.diagnostics)
      message += "; " + d.path + ": " + d.message;
    return refused(error, message);
  }
  if (context.stand.present())
    return refused(error, "deposits require static ground; rebake for changed terrain");
  if (!(e->layer.far_rings >= 1 && e->layer.far_rings <= 8))
    return refused(error, "engine.rocks.DepositLayer: far_rings 1..8");
  DepositPatch validation = e->layer.patch;
  validation.width = 2;
  validation.depth = 2;
  validation.spacing_m = e->layer.spacing_m;
  validation.ground.assign(4, 0.0f);
  validation.thickness.assign(4, 0.1f);
  validation.coarse.assign(4, 0.0f);
  std::string validation_error;
  if (!deposit_patch_in_range(validation, validation_error))
    return refused(error, validation_error);
  auto absolute_image = [&](std::optional<std::string>& path) {
    if (path && !path->empty()) {
      std::error_code path_error;
      const auto input = std::filesystem::path(*path);
      const auto resolved = std::filesystem::absolute(
          input.is_absolute() ? input : std::filesystem::path(context.dir) / input, path_error);
      if (path_error) return false;
      *path = resolved.generic_string();
    }
    return true;
  };
  auto& material = e->layer.patch.material;
  if (!absolute_image(material.base_color) || !absolute_image(material.normal) ||
      !absolute_image(material.roughness) || !absolute_image(material.metallic) ||
      !absolute_image(material.height))
    return refused(error, "deposits: cannot resolve material image path");
  std::string why;
  e->world_seed = context.world_seed;
  // G190: the ground before its edits, where the host has edits, so a face an edit cut sheds
  // nothing (the scene reader's `Context::uncut()`; a host that offers none hands the ground).
  const scene_gen::Ground before = context.uncut();
  const bool edited = before.present() && (before.surface_fn != context.ground.surface_fn ||
                                           before.context != context.ground.context);
  if (!plan_key(e->layer, context.ground, edited ? &before : nullptr, context.world_seed, e->key,
                why))
    return refused(error, why);
  e->plan = hold_plan(e->key);
  const bool found = e->plan != nullptr;
  if (!found) {
    DepositPlan plan;
    if (!plan_deposits(e->layer, context.ground, plan, why, edited ? &before : nullptr))
      return refused(error, why);
    // The drainage is the plan's own working; a patch never reads it.
    plan.drainage_m2 = Vector<f64>();
    e->plan = keep_plan(e->key, std::move(plan));
  }
  const DepositPlan& plan = *e->plan;
  if (!(context.tile_size >= .25f && context.tile_size <= 100000.0f))
    return refused(error, "deposits: invalid tile_size");
  const i64 tile_mm = scene_gen::nearest_mm(static_cast<f64>(context.tile_size));
  if (tile_mm % plan.spacing_mm != 0)
    return refused(error, "deposits: tile_size must be a whole number of spacings");
  const f64 patch_edge = static_cast<f64>(e->layer.patch_m);
  if (!(patch_edge >= 1 && patch_edge <= 32 && e->layer.patch_m >= e->layer.spacing_m))
    return refused(error, "deposits: invalid patch_m");
  const i64 patch_mm = scene_gen::nearest_mm(patch_edge);
  if (tile_mm % patch_mm != 0 || patch_mm % plan.spacing_mm != 0 || patch_mm / plan.spacing_mm > 64)
    return refused(error,
                   "deposits: patch_m must divide tile_size and contain at most 64 whole spacings");
  e->patch_mm = patch_mm;
  // Where a recipe's file would be: the host's derived-data root's `deposits/`, else the root found
  // from the scene's directory. Nothing is written there (G191): the recipe travels with the tile.
  std::string root(context.ddc);
  if (root.empty()) {
    std::error_code ec;
    const auto from = std::filesystem::absolute(
        context.dir.empty() ? std::filesystem::path(".") : std::filesystem::path(context.dir), ec);
    if (!ec) root = geometry::find_ddc_root(from.generic_string());
    if (root.empty()) root = io::join_path(context.dir, "ddc");
  }
  std::error_code ec;
  e->root = std::filesystem::absolute(io::join_path(root, "deposits"), ec).generic_string();
  if (ec) return refused(error, "deposits: cannot resolve derived-data directory");
  const i64 end_x = plan.x_mm + static_cast<i64>(plan.width - 1) * plan.spacing_mm;
  const i64 end_z = plan.z_mm + static_cast<i64>(plan.depth - 1) * plan.spacing_mm;
  // G143: each block goes to the patch its centre stands in (the last row and column of samples
  // belong to the patch before them, as the bed's do), bucketed once in plan order.
  e->bx0 = floor_div(plan.x_mm, patch_mm);
  e->bz0 = floor_div(plan.z_mm, patch_mm);
  e->columns = floor_div(end_x - 1, patch_mm) - e->bx0 + 1;
  e->rows = floor_div(end_z - 1, patch_mm) - e->bz0 + 1;
  const Entry& c = *e;
  auto patch_of = [&](i64 x_mm, i64 z_mm) {
    const i64 cx = std::min(floor_div(x_mm, patch_mm), c.bx0 + c.columns - 1) - c.bx0;
    const i64 cz = std::min(floor_div(z_mm, patch_mm), c.bz0 + c.rows - 1) - c.bz0;
    return static_cast<u32>(cz * c.columns + cx);
  };
  e->first_block.assign(static_cast<u32>(e->columns * e->rows) + 1, 0);
  for (const auto& b : plan.blocks)
    ++e->first_block[patch_of(b.x_mm, b.z_mm) + 1];
  for (u32 k = 1; k < e->first_block.size(); ++k)
    e->first_block[k] += e->first_block[k - 1];
  e->block_order.resize(plan.blocks.size());
  {
    Vector<u32> fill(e->first_block);
    for (u32 b = 0; b < plan.blocks.size(); ++b)
      e->block_order[fill[patch_of(plan.blocks[b].x_mm, plan.blocks[b].z_mm)]++] = b;
  }
  u32 id = 0;
  for (i64 bz = e->bz0; bz < e->bz0 + e->rows; ++bz)
    for (i64 bx = e->bx0; bx < e->bx0 + e->columns; ++bx) {
      ++id;
      DepositPatch p;
      i64 x0 = 0, z0 = 0, x1 = 0, z1 = 0;
      f64 y0 = 0, overhang = 0;
      bool any = false;
      make_patch(*e, bx, bz, id, p, x0, z0, y0, x1, z1, any, overhang);
      if (!any) continue;
      if (!deposit_patch_in_range(p, why)) return refused(error, why);
      // Validate capacity before returning a scene that would fail later in its mesh build, without
      // building it (G143): the patch is solved when its tile's mesh is built through the mesh
      // generator registry and the derived-data cache.
      if (!deposit_patch_fits(p, why)) return refused(error, why);
      if (static_cast<u64>(p.triangle_budget) +
              static_cast<u64>(p.blocks.size()) * static_cast<u64>(p.block_triangles) >
          k_patch_triangles_max)
        return refused(
            error,
            "deposits: a patch's triangle_budget and its blocks exceed 240,000 triangles "
            "(a streamed host admits a tile's mesh of at most 4,096 clusters)");
      PatchPlace at;
      at.placement.position = {static_cast<f64>(x0) / 1000.0, y0, static_cast<f64>(z0) / 1000.0};
      at.placement.id = id;
      const i64 tx = floor_div(x0, tile_mm), tz = floor_div(z0, tile_mm);
      if (tx < std::numeric_limits<i32>::min() || tx > std::numeric_limits<i32>::max() ||
          tz < std::numeric_limits<i32>::min() || tz > std::numeric_limits<i32>::max())
        return refused(error, "deposits: tile coordinate out of range");
      at.tile = {static_cast<i32>(tx), static_cast<i32>(tz)};
      at.width = static_cast<f64>(x1 - x0) / 1000.0;
      at.depth = static_cast<f64>(z1 - z0) / 1000.0;
      at.bx = bx;
      at.bz = bz;
      e->patches.push_back(at);
    }
  // G143, G190: what the plan found, once an entry: a scene's piles and blocks by count, what it
  // moved on and refused, and whether this open made the plan or found it (T120).
  f64 refused_m3 = 0;
  for (const auto& r : plan.refused)
    refused_m3 += r.volume_m3;
  ENGINE_LOG_INFO(log_deposits, found ? "deposits plan reused" : "deposits planned",
                  log::field("piles", static_cast<u64>(plan.piles.size())),
                  log::field("blocks", static_cast<u64>(plan.blocks.size())),
                  log::field("patches", static_cast<u64>(e->patches.size())),
                  log::field("moved", static_cast<u64>(plan.moved)),
                  log::field("refused", static_cast<u64>(plan.refused.size())),
                  log::field("refused_m3", refused_m3));
  *state = e.release();
  return true;
}
void close(void* state) noexcept { delete static_cast<Entry*>(state); }
bool allowed(const Entry& e, const PatchPlace& p, const scene_gen::Context& c) {
  // G135: a patch whose every corner and middle is over a hole has no ground to lie on (only holes:
  // gravel and rubble in a stream's bed are what a stream is). A scene with none asks nothing.
  if (c.exclusion != nullptr) {
    const f64 x0 = p.placement.position.x, z0 = p.placement.position.z;
    bool all = true;
    for (u32 k = 0; k < 5 && all; ++k) {
      const f64 fx = k == 4 ? 0.5 : static_cast<f64>(k & 1u);
      const f64 fz = k == 4 ? 0.5 : static_cast<f64>(k >> 1);
      all = c.excluded_mm(scene_gen::nearest_mm(x0 + fx * p.width),
                          scene_gen::nearest_mm(z0 + fz * p.depth), scene_gen::k_exclude_holes);
    }
    if (all) return false;
  }
  if (!e.layer.avoid_earlier) return true;
  for (const auto& f : c.earlier) {
    const f64 x = static_cast<f64>(f.x_mm) / 1000.0 - p.placement.position.x;
    const f64 z = static_cast<f64>(f.z_mm) / 1000.0 - p.placement.position.z;
    const f64 dx = x - std::clamp(x, 0.0, p.width), dz = z - std::clamp(z, 0.0, p.depth);
    const f64 r = f.radius + static_cast<f64>(e.layer.clear_m);
    if (dx * dx + dz * dz <= r * r) return false;
  }
  return true;
}
// G191: a patch's mesh, handed as its recipe with the placement that names it.
void hand(const Entry& e, const PatchPlace& at, scene_gen::Placements& out) {
  DepositPatch p;
  i64 x0 = 0, z0 = 0, x1 = 0, z1 = 0;
  f64 y0 = 0, overhang = 0;
  bool any = false;
  make_patch(e, at.bx, at.bz, at.placement.id, p, x0, z0, y0, x1, z1, any, overhang);
  JsonWriteOptions compact;
  compact.pretty = false;
  std::string text = write_json(schema::to_json(p), compact);
  char name[64];
  std::snprintf(name, sizeof(name), "deposit-%016llx.deposit.json",
                static_cast<unsigned long long>(deposit_source_hash(p)));
  // Its reach from its corner: the patch's diagonal and the largest block's overhang past it.
  const f64 reach = std::sqrt(at.width * at.width + at.depth * at.depth) + overhang;
  scene_gen::Placement placed = at.placement;
  placed.mesh = out.meshes.size();  // no resident kit: a tile's meshes are all its own
  out.meshes.push_back(scene_gen::PlacementMesh(io::join_path(e.root, name),
                                                std::string("deposits/") + name, 0, std::move(text),
                                                static_cast<f32>(reach)));
  out.instances.push_back(placed);
  ++out.things;
}
// The form a ring draws (G191): 0 the patches, 1 nothing past `far_rings`. A whole read is near.
u8 form_of(const Entry& e, const scene_gen::Context& c) {
  if (c.ring == scene_gen::k_no_ring) return 0;
  return c.ring < e.layer.far_rings ? u8{0} : u8{1};
}
bool expand(void* state, const scene_gen::Context& c, scene_gen::Placements& out,
            std::string* error) {
  if (c.stand.present())
    return refused(error, "deposits require static ground; rebake for changed terrain");
  // A whole read appends after what `out` already lists: each placement names the mesh it handed.
  const auto& e = *static_cast<Entry*>(state);
  for (const auto& p : e.patches)
    if (allowed(e, p, c)) hand(e, p, out);
  return true;
}
bool tile(void* state, scene_gen::TileCoord tile, const scene_gen::Context& c,
          scene_gen::Placements& out, std::string* error) {
  if (c.stand.present())
    return refused(error, "deposits require static ground; rebake for changed terrain");
  out.clear();
  const auto& e = *static_cast<Entry*>(state);
  out.kind = form_of(e, c);
  if (out.kind != 0) return true;
  for (const auto& p : e.patches)
    if (p.tile == tile && allowed(e, p, c)) hand(e, p, out);
  return true;
}
bool occupies(const void* state, scene_gen::TileCoord tile) noexcept {
  for (const auto& p : static_cast<const Entry*>(state)->patches)
    if (p.tile == tile) return true;
  return false;
}
u8 representation(const void* state, const scene_gen::Context& c) noexcept {
  return form_of(*static_cast<const Entry*>(state), c);
}
constexpr scene_gen::PlacementGeneratorDesc k_deposits{.name = "deposits",
                                                       .open = &open,
                                                       .close = &close,
                                                       .expand = &expand,
                                                       .meshes = nullptr,
                                                       .tile = &tile,
                                                       .occupies = &occupies,
                                                       .representation = &representation,
                                                       .tile_meshes = true};
const scene_gen::Registrar k_registrar{k_deposits};
}  // namespace

u64 deposit_plans_made() {
  PlanCache& c = plan_cache();
  std::lock_guard<std::mutex> lock(c.mutex);
  return c.plans_made;
}
}  // namespace engine::rocks
