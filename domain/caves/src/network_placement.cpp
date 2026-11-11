// The placement generator "caves" (docs/subsystems/caves.md, "The generator"): `open` reads the
// entry, plans the network once against the ground it is handed, and writes one recipe a cell (and
// one for its dressing) under the derived-data root; `meshes` names them; `tile` places the cells
// whose bounds' middle is on the tile — every one near, only the ones that hold a mouth in a far
// ring; `expand` is every cell.
#include <core/json/json.h>
#include <core/log/log.h>
#include <core/schema/json_reflect.h>
#include <domain/caves/network.h>
#include <domain/geometry/cluster_file.h>
#include <foundation/io/vfs.h>

#include <cstdio>
#include <filesystem>
#include <memory>

namespace engine::caves {
namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_caves, "caves");

struct Piece {
  scene_gen::TileCoord tile;
  WorldPos origin;
  u32 mesh = 0;
  u32 id = 0;
  bool mouth = false;
};

struct Entry {
  Plan plan;
  Vector<scene_gen::PlacementMesh> meshes;
  Vector<Piece> pieces;
  std::string where;
};

bool fail(std::string* error, const std::string& where, std::string_view why) {
  if (error != nullptr) *error = where + ": caves: " + std::string(why);
  return false;
}

scene_gen::Placement place(const Piece& piece) {
  scene_gen::Placement p;
  p.position = piece.origin;
  p.mesh = piece.mesh;
  p.id = piece.id;
  return p;
}

bool open(const JsonValue& params, const scene_gen::Context& context, void** state,
          std::string* error) {
  *state = nullptr;
  auto entry = std::make_unique<Entry>();
  entry->where = context.where.empty() ? std::string("caves") : std::string(context.where);
  Network network;
  schema::ReadContext read;
  if (!schema::from_json(network, params, read) || !read.ok())
    return fail(error, entry->where,
                read.diagnostics.empty()
                    ? std::string("invalid engine.caves.Network")
                    : read.diagnostics[0].path + ": " + read.diagnostics[0].message);
  std::string why;
  const i64 tile_mm = scene_gen::nearest_mm(static_cast<f64>(context.tile_size));
  if (!plan_network(network, context.ground, tile_mm, entry->plan, why))
    return fail(error, entry->where, why);
  // Where the recipes go: where the entry says; else the host's derived-data root; else the
  // repository's, found from the scene's directory; else `caves` beside the scene.
  std::string dir;
  const auto resolve = [&](std::string_view path) {
    return (std::filesystem::path(std::string(context.dir)) / std::string(path))
        .lexically_normal()
        .generic_string();
  };
  if (!network.recipes.empty()) {
    dir = resolve(network.recipes);
  } else {
    std::string ddc(context.ddc);
    if (ddc.empty()) {
      std::error_code ec;
      const std::filesystem::path from = std::filesystem::absolute(
          std::filesystem::path(context.dir.empty() ? "." : std::string(context.dir)), ec);
      if (!ec) ddc = geometry::find_ddc_root(from.generic_string());
    }
    dir = !ddc.empty() ? io::join_path(ddc, "caves") : resolve("caves");
  }
  if (io::make_directories(dir) != io::Status::Ok)
    return fail(error, entry->where, "cannot create " + dir);
  const Plan& plan = entry->plan;
  const u32 parts = network.speleothems.has_value() ? 2 : 1;
  for (u32 cell = 0; cell < plan.cells.size(); ++cell) {
    if (plan.cells[cell].flooded) continue;
    for (u32 part = 0; part < parts; ++part) {
      CellRecipe recipe;
      cell_recipe(plan, cell, part, recipe);
      const std::string text = cell_recipe_json(recipe);
      char name[64];
      std::snprintf(name, sizeof(name), "%s-%016llx.cavecell.json", part == 0 ? "cell" : "dress",
                    static_cast<unsigned long long>(cell_source_hash(recipe)));
      scene_gen::PlacementMesh mesh;
      mesh.path = io::join_path(dir, name);
      mesh.name = std::string("caves/") + name;
      if (!io::exists(mesh.path)) {
        const io::Status status = io::write_file_atomic(mesh.path, text);
        if (status != io::Status::Ok)
          return fail(error, entry->where,
                      "cannot write " + mesh.path + ": " + io::status_name(status));
      }
      Piece piece;
      piece.tile = plan.cells[cell].tile;
      piece.origin = cell_origin(plan, cell);
      piece.mesh = entry->meshes.size();
      piece.id = cell * 2 + part + 1;
      piece.mouth = plan.cells[cell].mouth;
      entry->meshes.push_back(std::move(mesh));
      entry->pieces.push_back(piece);
    }
  }
  ENGINE_LOG_INFO(log_caves, "cave network planned", log::field("entry", entry->where),
                  log::field("passage_m", static_cast<f64>(plan.report.passage_m)),
                  log::field("cells", static_cast<u64>(plan.cells.size())),
                  log::field("mouths", static_cast<u64>(plan.mouths.size())),
                  log::field("loops", static_cast<u64>(plan.report.loops)),
                  log::field("notes", plan.report.notes));
  *state = entry.release();
  return true;
}

void close(void* state) noexcept { delete static_cast<Entry*>(state); }

bool occupies(const void* state, scene_gen::TileCoord tile) noexcept {
  for (const Piece& piece : static_cast<const Entry*>(state)->pieces)
    if (piece.tile == tile) return true;
  return false;
}

u8 representation(const void* state, const scene_gen::Context& context) noexcept {
  const Entry& e = *static_cast<const Entry*>(state);
  return context.world != nullptr && context.ring != scene_gen::k_no_ring &&
                 context.ring >= e.plan.network.far_ring
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
  for (const Piece& piece : e.pieces) {
    if (!(piece.tile == t)) continue;
    // Far away only what daylight reaches is drawn: a cell with no mouth cannot be seen.
    if (out.kind == 1 && !piece.mouth) continue;
    out.instances.push_back(place(piece));
  }
  out.things = out.instances.size();
  return true;
}

bool expand(void* state, const scene_gen::Context&, scene_gen::Placements& out, std::string*) {
  out.clear();
  const Entry& e = *static_cast<Entry*>(state);
  out.meshes = e.meshes;
  for (const Piece& piece : e.pieces)
    out.instances.push_back(place(piece));
  out.things = out.instances.size();
  return true;
}

constexpr scene_gen::PlacementGeneratorDesc k_generator{.name = "caves",
                                                        .open = open,
                                                        .close = close,
                                                        .expand = expand,
                                                        .meshes = meshes,
                                                        .tile = tile,
                                                        .occupies = occupies,
                                                        .representation = representation};
const scene_gen::Registrar k_registrar{k_generator};

}  // namespace
}  // namespace engine::caves
