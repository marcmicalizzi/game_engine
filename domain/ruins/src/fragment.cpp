#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/ruins/fragment.h>
#include <foundation/io/vfs.h>

#include <filesystem>
#include <system_error>

namespace engine::ruins {

namespace {

std::string hex16(u64 value) {
  static constexpr char k_digits[] = "0123456789abcdef";
  std::string text(16, '0');
  for (u32 i = 0; i < 16; ++i)
    text[15 - i] = k_digits[(value >> (4 * i)) & 0xfu];
  return text;
}

// A mesh path as the fragment should name it: relative to the fragment's directory when there is
// such a path, absolute otherwise, with forward slashes either way.
std::string mesh_path(const std::string& mesh, std::string_view fragment_dir) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path absolute = fs::absolute(fs::path(mesh), ec);
  if (ec) return mesh;
  const fs::path dir =
      fs::absolute(fs::path(std::string(fragment_dir.empty() ? "." : fragment_dir)), ec);
  if (ec) return absolute.generic_string();
  const fs::path relative = fs::relative(absolute, dir, ec);
  if (ec || relative.empty()) return absolute.generic_string();
  return relative.generic_string();
}

}  // namespace

void make_fragment(const Kit& kit, const Output& out, std::string_view fragment_dir,
                   std::string_view name, scene::Scene& scene) {
  scene = scene::Scene{};
  scene.format = "engine.scene.v1";
  scene.name = std::string(name);
  scene.description = "Ruins assembled from the kit '" + kit.name + "' (" +
                      std::to_string(out.sites.size()) +
                      " buildings), written by engine-content ruins";
  scene.meshes.reserve(kit.meshes.size());
  for (u32 i = 0; i < kit.meshes.size(); ++i) {
    scene::Mesh mesh;
    mesh.path = mesh_path(kit.meshes[i], fragment_dir);
    mesh.name = std::string(io::file_name(kit.meshes[i]));
    if (kit.mesh_hashes[i] != 0) mesh.hash = hex16(kit.mesh_hashes[i]);
    scene.meshes.push_back(std::move(mesh));
  }
  scene.instances.reserve(out.instances.size());
  for (const Instance& piece : out.instances) {
    const Member& member = kit.members[piece.member];
    scene::Instance instance;
    instance.mesh = member.mesh_index;
    instance.name = member.name;
    instance.translation = instance_translation(kit, piece);
    instance.yaw_deg = static_cast<f32>(instance_yaw_step(kit, piece)) * 22.5f;
    scene::RuinTag tag;
    tag.building = piece.building;
    tag.wall = piece.wall;
    tag.slot = piece.slot;
    tag.kind = static_cast<scene::RuinPieceKind>(piece.kind);
    tag.height = static_cast<f32>(piece.height_q) / static_cast<f32>(k_q_one);
    instance.ruin = tag;
    scene.instances.push_back(std::move(instance));
  }
  for (const Site& site : out.sites) {
    scene::RuinSite record;
    record.seed = hex16(site.seed);
    record.tile = {site.tile.x, site.tile.z};
    record.shape = shape_name(static_cast<Shape>(site.shape));
    record.origin = site.origin;
    record.yaw_deg = static_cast<f32>(site.yaw) * 22.5f;
    record.walls = site.walls;
    record.instances = site.instance_count;
    scene.ruin_sites.push_back(std::move(record));
  }
  for (const Drift& drift : out.drifts) {
    scene::SandDrift record;
    record.building = drift.building;
    record.wall = drift.wall;
    record.from = drift.from;
    record.to = drift.to;
    record.normal = drift.normal;
    record.height = drift.height;
    record.reach = drift.reach;
    record.windward = drift.windward != 0;
    scene.sand_drifts.push_back(record);
  }
}

bool write_fragment(const std::string& path, const Kit& kit, const Output& out,
                    std::string_view name, std::string* error) {
  scene::Scene scene;
  make_fragment(kit, out, io::parent_path(path), name, scene);
  const std::string text = write_json(schema::to_json(scene)) + "\n";
  const std::string_view dir = io::parent_path(path);
  if (!dir.empty() && io::make_directories(dir) != io::Status::Ok) {
    if (error != nullptr) *error = "cannot create " + std::string(dir);
    return false;
  }
  const io::Status status = io::write_file(path, text);
  if (status != io::Status::Ok) {
    if (error != nullptr) *error = "cannot write " + path + ": " + io::status_name(status);
    return false;
  }
  return true;
}

}  // namespace engine::ruins
