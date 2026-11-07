// The placement generator "channels" (docs/subsystems/channels.md, "In a scene"): the network over
// the entry's region decided once, when the entry is opened, its pieces written as recipes into
// the derived-data root, and each tile handed the pieces whose origin it holds — so a tile's
// placements are the same whenever and in whatever order it comes in.
#include "detail.h"

#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/geometry/cluster_file.h>
#include <foundation/io/vfs.h>

#include <cstdio>
#include <filesystem>
#include <memory>

namespace engine::channels {
namespace {
using namespace detail;

struct Entry {
  ChannelLayer layer;
  Vector<scene_gen::PlacementMesh> meshes;
  Vector<scene_gen::Placement> placements;
  Vector<scene_gen::TileCoord> tiles;  // per placement
};

bool refused(std::string* error, std::string message) {
  if (error) *error = std::move(message);
  return false;
}
constexpr const char* k_moving =
    "channels require static ground; a moving ground has no drainage to dress";

bool open(const JsonValue& params, const scene_gen::Context& context, void** state,
          std::string* error) {
  *state = nullptr;
  auto e = std::make_unique<Entry>();
  schema::ReadContext reader;
  if (!schema::from_json(e->layer, params, reader) || !reader.ok()) {
    std::string message = "engine.channels.ChannelLayer";
    for (const auto& d : reader.diagnostics)
      message += "; " + d.path + ": " + d.message;
    return refused(error, message);
  }
  if (context.stand.present()) return refused(error, k_moving);
  std::string why;
  ChannelNetwork network;
  if (!plan_channels(e->layer, context.ground, context.world_seed, context.jobs(), network, why))
    return refused(error, why);
  Vector<PlacedPiece> pieces;
  if (!place_pieces(e->layer, network, context.ground, context.tile_size, pieces, why))
    return refused(error, why);
  std::string root(context.ddc);
  if (root.empty()) {
    std::error_code ec;
    const auto from = std::filesystem::absolute(
        context.dir.empty() ? std::filesystem::path(".") : std::filesystem::path(context.dir), ec);
    if (!ec) root = geometry::find_ddc_root(from.generic_string());
    if (root.empty()) root = io::join_path(context.dir, "ddc");
  }
  std::error_code ec;
  root = std::filesystem::absolute(io::join_path(root, "channels"), ec).generic_string();
  if (ec || io::make_directories(root) != io::Status::Ok)
    return refused(error, "channels: cannot make the derived-data directory " + root);
  for (const PlacedPiece& piece : pieces) {
    const std::string text = piece_json(piece.recipe);
    char name[64];
    std::snprintf(name, sizeof(name), "channel-%016llx.channel.json",
                  static_cast<unsigned long long>(piece_source_hash(piece.recipe)));
    scene_gen::PlacementMesh mesh;
    mesh.path = io::join_path(root, name);
    mesh.name = std::string("channels/") + name;
    if (!io::exists(mesh.path) && io::write_file_atomic(mesh.path, text) != io::Status::Ok)
      return refused(error, "channels: cannot write " + mesh.path);
    scene_gen::Placement at;
    at.position = piece.origin;
    at.mesh = e->meshes.size();
    at.id = piece.id;
    e->placements.push_back(at);
    e->tiles.push_back(piece.tile);
    e->meshes.push_back(std::move(mesh));
  }
  *state = e.release();
  return true;
}
void close(void* state) noexcept { delete static_cast<Entry*>(state); }
bool expand(void* state, const scene_gen::Context& c, scene_gen::Placements& out,
            std::string* error) {
  if (c.stand.present()) return refused(error, k_moving);
  const auto& e = *static_cast<Entry*>(state);
  const u32 offset = out.meshes.size();
  for (const auto& m : e.meshes)
    out.meshes.push_back(m);
  for (const auto& p : e.placements) {
    auto at = p;
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
  if (c.stand.present()) return refused(error, k_moving);
  const auto& e = *static_cast<Entry*>(state);
  for (u32 i = 0; i < e.placements.size(); ++i)
    if (e.tiles[i] == tile) {
      out.instances.push_back(e.placements[i]);
      ++out.things;
    }
  return true;
}
bool occupies(const void* state, scene_gen::TileCoord tile) noexcept {
  for (const auto& t : static_cast<const Entry*>(state)->tiles)
    if (t == tile) return true;
  return false;
}
u8 representation(const void*, const scene_gen::Context&) noexcept { return 0; }
constexpr scene_gen::PlacementGeneratorDesc k_channels{.name = "channels",
                                                       .open = &open,
                                                       .close = &close,
                                                       .expand = &expand,
                                                       .meshes = &meshes,
                                                       .tile = &tile,
                                                       .occupies = &occupies,
                                                       .representation = &representation};
const scene_gen::Registrar k_registrar{k_channels};
}  // namespace
}  // namespace engine::channels
