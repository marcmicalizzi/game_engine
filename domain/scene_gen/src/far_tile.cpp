// A far tile's recipe (far_tile.h; scene_gen.md, "A far tile"; G198).
#include <domain/scene_gen/far_tile.h>

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <foundation/io/vfs.h>

#include <cstdio>

namespace engine::scene_gen {
namespace {

bool refuse(std::string* error, std::string message) {
  if (error != nullptr) *error = "far tile: " + std::move(message);
  return false;
}

JsonValue floats(std::span<const f32> values) {
  JsonValue out = JsonValue::array();
  for (const f32 v : values)
    out.push_back(JsonValue(v));
  return out;
}

bool read_floats(const JsonValue* value, f32* out, u32 count) {
  if (value == nullptr || !value->is_array() || value->size() != count) return false;
  for (u32 k = 0; k < count; ++k) {
    f64 v = 0;
    if (!(*value)[k].get_f64(v)) return false;
    out[k] = static_cast<f32>(v);
  }
  return true;
}

}  // namespace

bool far_tile_mesh(const FarTile& tile, std::string_view dir, f32 reach, PlacementMesh& out,
                   std::string* error) {
  if (tile.placements.empty()) return refuse(error, "it places nothing");
  JsonValue root = JsonValue::object();
  root.set("far_tile", JsonValue(k_far_tile_version));
  root.set("level", JsonValue(tile.level));
  JsonValue meshes = JsonValue::array();
  for (const PlacementMesh& m : tile.meshes) {
    JsonValue entry = JsonValue::object();
    entry.set("path", JsonValue(m.path));
    entry.set("name", JsonValue(m.name));
    entry.set("hash", JsonValue(m.hash));
    entry.set("recipe", JsonValue(m.recipe));
    entry.set("reach", JsonValue(m.reach));
    meshes.push_back(std::move(entry));
  }
  root.set("meshes", std::move(meshes));
  JsonValue placements = JsonValue::array();
  for (const FarTilePlacement& p : tile.placements) {
    if (p.mesh >= tile.meshes.size()) return refuse(error, "a placement names a mesh it lacks");
    JsonValue entry = JsonValue::object();
    entry.set("mesh", JsonValue(p.mesh));
    JsonValue mm = JsonValue::array();
    mm.push_back(JsonValue(p.x_mm));
    mm.push_back(JsonValue(p.y_mm));
    mm.push_back(JsonValue(p.z_mm));
    entry.set("mm", std::move(mm));
    const f32 turn[4] = {p.rotation.x, p.rotation.y, p.rotation.z, p.rotation.w};
    entry.set("rotation", floats(turn));
    const f32 scale[3] = {p.scale.x, p.scale.y, p.scale.z};
    entry.set("scale", floats(scale));
    placements.push_back(std::move(entry));
  }
  root.set("placements", std::move(placements));
  JsonWriteOptions compact;
  compact.pretty = false;
  std::string recipe = write_json(root, compact);
  const u64 h = hash_bytes(recipe.data(), recipe.size());
  char name[48];
  std::snprintf(name, sizeof(name), "far-%016llx", static_cast<unsigned long long>(h));
  out = PlacementMesh(io::join_path(dir, std::string(name) + ".far.json"),
                      std::string("far/") + name, 0, std::move(recipe), reach);
  return true;
}

bool read_far_tile(std::string_view recipe, FarTile& out, std::string* error) {
  out = FarTile{};
  JsonValue root;
  if (!parse_json(recipe, root).ok || !root.is_object()) return refuse(error, "not JSON");
  u64 version = 0, level = 0;
  const JsonValue* v = root.find("far_tile");
  if (v == nullptr || !v->get_u64(version) || version != k_far_tile_version)
    return refuse(error, "not a version " + std::to_string(k_far_tile_version) + " recipe");
  const JsonValue* l = root.find("level");
  if (l == nullptr || !l->get_u64(level) || level > 64) return refuse(error, "no level");
  out.level = static_cast<u32>(level);
  const JsonValue* meshes = root.find("meshes");
  const JsonValue* placements = root.find("placements");
  if (meshes == nullptr || !meshes->is_array() || placements == nullptr ||
      !placements->is_array())
    return refuse(error, "no meshes or placements");
  for (const JsonValue& m : meshes->as_array()) {
    std::string_view path, name, text;
    u64 hash = 0;
    f64 reach = 0;
    const JsonValue* p = m.find("path");
    const JsonValue* n = m.find("name");
    const JsonValue* h = m.find("hash");
    const JsonValue* r = m.find("recipe");
    const JsonValue* re = m.find("reach");
    if (p == nullptr || !p->get_string(path) || n == nullptr || !n->get_string(name) ||
        h == nullptr || !h->get_u64(hash) || r == nullptr || !r->get_string(text) ||
        re == nullptr || !re->get_f64(reach))
      return refuse(error, "a mesh is not path, name, hash, recipe and reach");
    out.meshes.push_back(PlacementMesh(std::string(path), std::string(name), hash,
                                       std::string(text), static_cast<f32>(reach)));
  }
  for (const JsonValue& e : placements->as_array()) {
    FarTilePlacement p;
    u64 mesh = 0;
    const JsonValue* m = e.find("mesh");
    const JsonValue* mm = e.find("mm");
    if (m == nullptr || !m->get_u64(mesh) || mesh >= out.meshes.size() || mm == nullptr ||
        !mm->is_array() || mm->size() != 3 || !(*mm)[0].get_i64(p.x_mm) ||
        !(*mm)[1].get_i64(p.y_mm) || !(*mm)[2].get_i64(p.z_mm))
      return refuse(error, "a placement is not a mesh and three millimetres");
    p.mesh = static_cast<u32>(mesh);
    f32 turn[4], scale[3];
    if (!read_floats(e.find("rotation"), turn, 4) || !read_floats(e.find("scale"), scale, 3))
      return refuse(error, "a placement has no rotation or scale");
    p.rotation = Quat{turn[0], turn[1], turn[2], turn[3]};
    p.scale = Vec3{scale[0], scale[1], scale[2]};
    out.placements.push_back(p);
  }
  if (out.placements.empty()) return refuse(error, "it places nothing");
  return true;
}

bool is_far_tile(const PlacementMesh& mesh) noexcept {
  constexpr std::string_view suffix = ".far.json";
  const std::string_view path = mesh.path;
  return path.size() > suffix.size() && path.substr(path.size() - suffix.size()) == suffix;
}

}  // namespace engine::scene_gen
