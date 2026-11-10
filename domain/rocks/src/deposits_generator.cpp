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

namespace engine::rocks {
namespace {
ENGINE_LOG_CATEGORY_DEFINE(log_deposits, "deposits");
struct PatchPlace {
  scene_gen::Placement placement;
  scene_gen::TileCoord tile;
  f64 width = 0;
  f64 depth = 0;
};
struct Entry {
  DepositLayer layer;
  Vector<scene_gen::PlacementMesh> meshes;
  Vector<PatchPlace> patches;
};
i64 floor_div(i64 a, i64 b) {
  const i64 q = a / b;
  return q - (a % b < 0 ? 1 : 0);
}
bool refused(std::string* error, std::string message) {
  if (error) *error = std::move(message);
  return false;
}
bool open(const JsonValue& params, const scene_gen::Context& context, void** state,
          std::string* error) {
  *state = nullptr;
  auto e = std::make_unique<Entry>();
  schema::ReadContext reader;
  if (!schema::from_json(e->layer, params, reader) || !reader.ok()) {
    std::string message = "engine.rocks.DepositLayer";
    for (const auto& d : reader.diagnostics)
      message += "; " + d.path + ": " + d.message;
    return refused(error, message);
  }
  if (context.stand.present())
    return refused(error, "deposits require static ground; rebake for changed terrain");
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
  DepositPlan plan;
  std::string why;
  if (!plan_deposits(e->layer, context.ground, plan, why)) return refused(error, why);
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
  std::string root(context.ddc);
  if (root.empty()) {
    std::error_code ec;
    const auto from = std::filesystem::absolute(
        context.dir.empty() ? std::filesystem::path(".") : std::filesystem::path(context.dir), ec);
    if (!ec) root = geometry::find_ddc_root(from.generic_string());
    if (root.empty()) root = io::join_path(context.dir, "ddc");
  }
  std::error_code ec;
  root = std::filesystem::absolute(io::join_path(root, "deposits"), ec).generic_string();
  if (ec) return refused(error, "deposits: cannot resolve derived-data directory");
  const i64 end_x = plan.x_mm + static_cast<i64>(plan.width - 1) * plan.spacing_mm;
  const i64 end_z = plan.z_mm + static_cast<i64>(plan.depth - 1) * plan.spacing_mm;
  // G143: each block goes to the patch its centre stands in (the last row and column of samples
  // belong to the patch before them, as the bed's do), bucketed once in plan order.
  const i64 bx0 = floor_div(plan.x_mm, patch_mm), bz0 = floor_div(plan.z_mm, patch_mm);
  const i64 columns = floor_div(end_x - 1, patch_mm) - bx0 + 1;
  const i64 rows = floor_div(end_z - 1, patch_mm) - bz0 + 1;
  auto patch_of = [&](i64 x_mm, i64 z_mm) {
    const i64 cx = std::min(floor_div(x_mm, patch_mm), bx0 + columns - 1) - bx0;
    const i64 cz = std::min(floor_div(z_mm, patch_mm), bz0 + rows - 1) - bz0;
    return static_cast<u32>(cz * columns + cx);
  };
  Vector<u32> first_block(static_cast<u32>(columns * rows) + 1, 0), block_order;
  for (const auto& b : plan.blocks)
    ++first_block[patch_of(b.x_mm, b.z_mm) + 1];
  for (u32 k = 1; k < first_block.size(); ++k)
    first_block[k] += first_block[k - 1];
  block_order.resize(plan.blocks.size());
  {
    Vector<u32> fill(first_block);
    for (u32 b = 0; b < plan.blocks.size(); ++b)
      block_order[fill[patch_of(plan.blocks[b].x_mm, plan.blocks[b].z_mm)]++] = b;
  }
  u32 id = 0;
  for (i64 bz = floor_div(plan.z_mm, patch_mm); bz <= floor_div(end_z - 1, patch_mm); ++bz)
    for (i64 bx = floor_div(plan.x_mm, patch_mm); bx <= floor_div(end_x - 1, patch_mm); ++bx) {
      ++id;
      const i64 x0 = std::max(plan.x_mm, bx * patch_mm), z0 = std::max(plan.z_mm, bz * patch_mm);
      const i64 x1 = std::min(end_x, (bx + 1) * patch_mm),
                z1 = std::min(end_z, (bz + 1) * patch_mm);
      const u32 ix = static_cast<u32>((x0 - plan.x_mm) / plan.spacing_mm);
      const u32 iz = static_cast<u32>((z0 - plan.z_mm) / plan.spacing_mm);
      DepositPatch p = e->layer.patch;
      p.seed =
          hash_combine(p.seed, hash_combine(e->layer.seed, hash_combine(context.world_seed, id)));
      p.width = static_cast<u32>((x1 - x0) / plan.spacing_mm) + 1;
      p.depth = static_cast<u32>((z1 - z0) / plan.spacing_mm) + 1;
      p.spacing_m = e->layer.spacing_m;
      p.ground.clear();
      p.thickness.clear();
      p.coarse.clear();
      p.blocks.clear();
      const f64 y0 = plan.ground[iz * plan.width + ix];
      bool any = false;
      for (u32 z = 0; z < p.depth; ++z)
        for (u32 x = 0; x < p.width; ++x) {
          const u32 i = (iz + z) * plan.width + ix + x;
          p.ground.push_back(static_cast<f32>(plan.ground[i] - y0));
          p.thickness.push_back(plan.thickness[i]);
          p.coarse.push_back(plan.coarse[i]);
          any = any || plan.thickness[i] > .001f;
        }
      // G143: the patch's blocks, local to its corner, and its rock: the bed most of its deposit
      // and its blocks were shed from (a patch is one material; the lowest type wins a tie).
      const u32 slot = static_cast<u32>((bz - bz0) * columns + (bx - bx0));
      u32 votes[static_cast<u32>(RockType::Dolostone) + 1] = {};
      for (u32 k = first_block[slot]; k < first_block[slot + 1]; ++k) {
        const DepositPlanBlock& b = plan.blocks[block_order[k]];
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
        ++votes[static_cast<u32>(b.type)];
      }
      any = any || !p.blocks.empty();
      if (!any) continue;
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
      if (!deposit_patch_in_range(p, why)) return refused(error, why);
      // Validate capacity before returning a scene that would fail later in its mesh build.
      assets::MeshData checked;
      if (!solve_deposit(p, checked, why)) return refused(error, why);
      JsonWriteOptions compact;
      compact.pretty = false;
      const std::string text = write_json(schema::to_json(p), compact);
      char name[64];
      std::snprintf(name, sizeof(name), "deposit-%016llx.deposit.json",
                    static_cast<unsigned long long>(deposit_source_hash(p)));
      scene_gen::PlacementMesh mesh;
      mesh.path = io::join_path(root, name);
      mesh.name = std::string("deposits/") + name;
      if (io::make_directories(root) != io::Status::Ok ||
          (!io::exists(mesh.path) && io::write_file_atomic(mesh.path, text) != io::Status::Ok))
        return refused(error, "deposits: cannot write " + mesh.path);
      PatchPlace at;
      at.placement.position = {static_cast<f64>(x0) / 1000.0, y0, static_cast<f64>(z0) / 1000.0};
      at.placement.mesh = e->meshes.size();
      at.placement.id = id;
      const i64 tx = floor_div(x0, tile_mm), tz = floor_div(z0, tile_mm);
      if (tx < std::numeric_limits<i32>::min() || tx > std::numeric_limits<i32>::max() ||
          tz < std::numeric_limits<i32>::min() || tz > std::numeric_limits<i32>::max())
        return refused(error, "deposits: tile coordinate out of range");
      at.tile = {static_cast<i32>(tx), static_cast<i32>(tz)};
      at.width = static_cast<f64>(x1 - x0) / 1000.0;
      at.depth = static_cast<f64>(z1 - z0) / 1000.0;
      e->patches.push_back(at);
      e->meshes.push_back(std::move(mesh));
    }
  // G143: what the plan found, once an entry: a scene's cones and blocks, by count.
  ENGINE_LOG_INFO(log_deposits, "deposits planned",
                  log::field("cones", static_cast<u64>(plan.cones.size())),
                  log::field("blocks", static_cast<u64>(plan.blocks.size())),
                  log::field("patches", static_cast<u64>(e->patches.size())));
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
bool expand(void* state, const scene_gen::Context& c, scene_gen::Placements& out,
            std::string* error) {
  if (c.stand.present())
    return refused(error, "deposits require static ground; rebake for changed terrain");
  const auto& e = *static_cast<Entry*>(state);
  const u32 offset = out.meshes.size();
  for (const auto& m : e.meshes)
    out.meshes.push_back(m);
  for (const auto& p : e.patches)
    if (allowed(e, p, c)) {
      auto at = p.placement;
      at.mesh += offset;
      out.instances.push_back(at);
      ++out.things;
    }
  return true;
}
bool meshes(void* state, const scene_gen::Context&, Vector<scene_gen::PlacementMesh>& out,
            std::string*) {
  for (const auto& m : static_cast<Entry*>(state)->meshes)
    out.push_back(m);
  return true;
}
bool tile(void* state, scene_gen::TileCoord tile, const scene_gen::Context& c,
          scene_gen::Placements& out, std::string* error) {
  if (c.stand.present())
    return refused(error, "deposits require static ground; rebake for changed terrain");
  const auto& e = *static_cast<Entry*>(state);
  for (const auto& p : e.patches)
    if (p.tile == tile && allowed(e, p, c)) {
      out.instances.push_back(p.placement);
      ++out.things;
    }
  return true;
}
bool occupies(const void* state, scene_gen::TileCoord tile) noexcept {
  for (const auto& p : static_cast<const Entry*>(state)->patches)
    if (p.tile == tile) return true;
  return false;
}
u8 representation(const void*, const scene_gen::Context&) noexcept { return 0; }
constexpr scene_gen::PlacementGeneratorDesc k_deposits{.name = "deposits",
                                                       .open = &open,
                                                       .close = &close,
                                                       .expand = &expand,
                                                       .meshes = &meshes,
                                                       .tile = &tile,
                                                       .occupies = &occupies,
                                                       .representation = &representation};
const scene_gen::Registrar k_registrar{k_deposits};
}  // namespace
}  // namespace engine::rocks
