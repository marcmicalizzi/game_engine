// The placement generator `walls` (docs/subsystems/walls.md, "The generator"): `open` reads the
// layer, plans its lines once (stations.cpp) and writes the kit's recipes under the derived-data
// root; `meshes` names the kit; `tile` stands the modules whose middle falls in the tile, or in a
// far ring the strips; `expand` is every tile's modules, in slot order.

#include <core/json/json.h>
#include <core/log/log.h>
#include <core/schema/json_reflect.h>
#include <domain/geometry/cluster_file.h>
#include <domain/walls/walls.h>
#include <foundation/io/vfs.h>

#include <cstdio>
#include <filesystem>
#include <memory>

namespace engine::walls {
namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_walls, "walls");

struct Entry {
  Layer layer;
  Plan plan;
  Vector<scene_gen::PlacementMesh> meshes;
  std::string where;
};

bool fail(std::string* error, const std::string& where, std::string_view why) {
  if (error != nullptr) *error = where + ": walls: " + std::string(why);
  return false;
}

std::string resolve(std::string_view dir, std::string_view path) {
  return (std::filesystem::path(std::string(dir)) / std::string(path))
      .lexically_normal()
      .generic_string();
}

bool open(const JsonValue& params, const scene_gen::Context& context, void** state,
          std::string* error) {
  *state = nullptr;
  auto entry = std::make_unique<Entry>();
  entry->where = context.where.empty() ? std::string("walls") : std::string(context.where);
  schema::ReadContext read;
  if (!schema::from_json(entry->layer, params, read) || !read.ok())
    return fail(error, entry->where,
                read.diagnostics.empty()
                    ? std::string("invalid engine.walls.Layer")
                    : read.diagnostics[0].path + ": " + read.diagnostics[0].message);
  const i64 tile_mm = scene_gen::nearest_mm(static_cast<f64>(context.tile_size));
  std::string why;
  if (!make_plan(entry->layer, tile_mm > 0 ? tile_mm : 32000, entry->plan, why))
    return fail(error, entry->where, why);
  // Where the recipes go: where the layer says; else the host's derived-data root; else the
  // repository's, found from the scene's directory; else `walls` beside the scene (as the cliffs').
  std::string dir;
  if (!entry->layer.recipes.empty()) {
    dir = resolve(context.dir, entry->layer.recipes);
  } else {
    std::string ddc(context.ddc);
    if (ddc.empty()) {
      std::error_code ec;
      const std::filesystem::path from = std::filesystem::absolute(
          std::filesystem::path(context.dir.empty() ? "." : std::string(context.dir)), ec);
      if (!ec) ddc = geometry::find_ddc_root(from.generic_string());
    }
    dir = !ddc.empty() ? io::join_path(ddc, "walls") : resolve(context.dir, "walls");
  }
  if (io::make_directories(dir) != io::Status::Ok)
    return fail(error, entry->where, "cannot create " + dir);
  Vector<Module> kit;
  kit_recipes(entry->layer, kit);
  for (const Module& m : kit) {
    const std::string text = canonical_json(m);
    char name[48];
    std::snprintf(name, sizeof(name), "wall-%016llx.wall.json",
                  static_cast<unsigned long long>(source_hash(m)));
    scene_gen::PlacementMesh mesh;
    mesh.path = io::join_path(dir, name);
    mesh.name = std::string("walls/") + name;
    if (!io::exists(mesh.path)) {
      const io::Status status = io::write_file_atomic(mesh.path, text);
      if (status != io::Status::Ok)
        return fail(error, entry->where,
                    "cannot write " + mesh.path + ": " + io::status_name(status));
    }
    entry->meshes.push_back(std::move(mesh));
  }
  ENGINE_LOG_INFO(log_walls, "walls planned", log::field("entry", entry->where),
                  log::field("lines", static_cast<u64>(entry->plan.lines.size())),
                  log::field("modules", static_cast<u64>(entry->plan.slots.size())),
                  log::field("pieces", static_cast<u64>(entry->plan.pieces.size())));
  *state = entry.release();
  return true;
}

void close(void* state) noexcept { delete static_cast<Entry*>(state); }

bool occupies(const void* state, scene_gen::TileCoord tile) noexcept {
  const Entry& e = *static_cast<const Entry*>(state);
  return !refs_on(e.plan.slot_tiles, tile).empty() || !refs_on(e.plan.piece_tiles, tile).empty();
}

u8 representation(const void* state, const scene_gen::Context& context) noexcept {
  const Entry& e = *static_cast<const Entry*>(state);
  return context.world != nullptr && context.ring != scene_gen::k_no_ring &&
                 context.ring >= e.layer.far_ring
             ? 1
             : 0;
}

bool meshes(void* state, const scene_gen::Context&, Vector<scene_gen::PlacementMesh>& out,
            std::string*) {
  out = static_cast<Entry*>(state)->meshes;
  return true;
}

bool tile(void* state, scene_gen::TileCoord t, const scene_gen::Context& context,
          scene_gen::Placements& out, std::string* error) {
  out.clear();
  const Entry& e = *static_cast<Entry*>(state);
  if (scene_gen::nearest_mm(static_cast<f64>(context.tile_size)) != e.plan.tile_mm)
    return fail(error, e.where, "the tile size changed after open");
  out.kind = representation(state, context);
  if (out.kind == 0) {
    for (const TileRef& r : refs_on(e.plan.slot_tiles, t))
      out.instances.push_back(
          stand_slot(e.plan, e.layer, e.plan.slots[r.index], context.ground).placement);
  } else {
    for (const TileRef& r : refs_on(e.plan.piece_tiles, t))
      out.instances.push_back(stand_piece(e.plan, e.layer, e.plan.pieces[r.index], context.ground));
  }
  out.things = static_cast<u32>(out.instances.size());
  return true;
}

bool expand(void* state, const scene_gen::Context& context, scene_gen::Placements& out,
            std::string* error) {
  (void)error;
  out.clear();
  const Entry& e = *static_cast<Entry*>(state);
  out.meshes = e.meshes;
  for (const Slot& slot : e.plan.slots)
    out.instances.push_back(stand_slot(e.plan, e.layer, slot, context.ground).placement);
  out.things = static_cast<u32>(out.instances.size());
  return true;
}

constexpr scene_gen::PlacementGeneratorDesc k_generator{.name = "walls",
                                                        .open = open,
                                                        .close = close,
                                                        .expand = expand,
                                                        .meshes = meshes,
                                                        .tile = tile,
                                                        .occupies = occupies,
                                                        .representation = representation};
const scene_gen::Registrar k_registrar{k_generator};

}  // namespace
}  // namespace engine::walls
