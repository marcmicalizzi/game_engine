#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/ruins/fragment.h>
#include <foundation/io/vfs.h>

#include <algorithm>
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

void add_meshes(const Vector<std::string>& meshes, const Vector<u64>& hashes,
                std::string_view fragment_dir, scene::Scene& scene) {
  scene.meshes.reserve(meshes.size());
  for (u32 i = 0; i < meshes.size(); ++i) {
    scene::Mesh mesh;
    mesh.path = mesh_path(meshes[i], fragment_dir);
    mesh.name = std::string(io::file_name(meshes[i]));
    if (hashes[i] != 0) mesh.hash = hex16(hashes[i]);
    scene.meshes.push_back(std::move(mesh));
  }
}

// The buildings and the sand their walls ask for, the same for either representation.
void add_sites_and_drifts(const Vector<Site>& sites, const Vector<Drift>& drifts,
                          scene::Scene& scene) {
  for (const Site& site : sites) {
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
  for (const Drift& drift : drifts) {
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

bool write_scene(const std::string& path, const scene::Scene& scene, std::string* error) {
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

}  // namespace

void make_fragment(const Kit& kit, const Output& out, std::string_view fragment_dir,
                   std::string_view name, scene::Scene& scene) {
  scene = scene::Scene{};
  scene.format = "engine.scene.v1";
  scene.name = std::string(name);
  scene.description = "Ruins assembled from the kit '" + kit.name + "' (" +
                      std::to_string(out.sites.size()) +
                      " buildings), written by engine-content ruins";
  add_meshes(kit.meshes, kit.mesh_hashes, fragment_dir, scene);
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
  add_sites_and_drifts(out.sites, out.drifts, scene);
}

bool write_fragment(const std::string& path, const Kit& kit, const Output& out,
                    std::string_view name, std::string* error) {
  scene::Scene scene;
  make_fragment(kit, out, io::parent_path(path), name, scene);
  return write_scene(path, scene, error);
}

void make_block_fragment(const Kit& kit, const BlockKit& blocks, const BlockOutput& out,
                         std::string_view fragment_dir, std::string_view name,
                         scene::Scene& scene) {
  scene = scene::Scene{};
  scene.format = "engine.scene.v1";
  scene.name = std::string(name);
  scene.description = "Ruins assembled from the kit '" + kit.name +
                      "' and laid block by block from '" + blocks.name + "' (" +
                      std::to_string(out.sites.size()) +
                      " buildings), written by engine-content ruins --blocks";
  add_meshes(blocks.meshes, blocks.mesh_hashes, fragment_dir, scene);
  // The intact wall in whole courses, as the layer counts them: what a block's course is a
  // fraction of.
  const i32 course = std::max(blocks.course_cm, 1);
  const i32 courses = std::clamp((kit.wall_height_cm + course / 2) / course, 1, 255);
  scene.instances.reserve(out.blocks.size());
  for (const Block& block : out.blocks) {
    const KitBlock& kb = blocks.blocks[block.block];
    const bool fallen = (block.flags & k_block_fallen) != 0;
    scene::Instance instance;
    instance.mesh = kb.mesh_index;
    instance.name = kb.name;
    instance.translation = block_translation(blocks, block);
    instance.yaw_deg = static_cast<f32>(block_yaw_step(blocks, block)) * 22.5f;
    scene::RuinTag tag;
    tag.building = block.building;
    tag.wall = block.wall;
    tag.slot = block.index;
    tag.kind = fallen ? scene::RuinPieceKind::Debris : scene::RuinPieceKind::Section;
    tag.height = fallen ? 0.0f : static_cast<f32>(block.course + 1) / static_cast<f32>(courses);
    scene::RuinBlockTag block_tag;
    block_tag.role = static_cast<scene::RuinBlockRole>(block.role);
    block_tag.course = block.course;
    block_tag.fallen = fallen;
    block_tag.eroded = (block.flags & k_block_eroded) != 0;
    tag.block = block_tag;
    instance.ruin = tag;
    scene.instances.push_back(std::move(instance));
  }
  add_sites_and_drifts(out.sites, out.drifts, scene);
}

bool write_block_fragment(const std::string& path, const Kit& kit, const BlockKit& blocks,
                          const BlockOutput& out, std::string_view name, std::string* error) {
  scene::Scene scene;
  make_block_fragment(kit, blocks, out, io::parent_path(path), name, scene);
  return write_scene(path, scene, error);
}

}  // namespace engine::ruins
