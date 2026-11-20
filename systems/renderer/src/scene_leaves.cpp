// The leaves a mesh hangs on hinges, drawn where their doors are
// (docs/subsystems/scene_collision.md, "What is drawn swings with what collides"; roadmap S91). A
// building leaves its leaves out of its own mesh because they turn; the scene hangs each as an
// instance of the leaf's own mesh at the angle its record declares, so a house nobody touched draws
// what it drew with the leaf in its mesh, and a host moves that one instance when its door turns.

#include <core/math/deterministic.h>
#include <domain/assets/mesh_generator.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/scene.h>

#include <algorithm>
#include <cmath>
#include <span>
#include <string>

namespace engine::renderer {

Quat hinge_turn(i32 mdeg) noexcept {
  i64 m = static_cast<i64>(mdeg) % 360000;
  if (m > 180000) m -= 360000;
  if (m <= -180000) m += 360000;
  const f64 half = static_cast<f64>(m) * (3.14159265358979323846 / 360000.0);
  return Quat{0.0f, static_cast<f32>(det::sin(half)), 0.0f, static_cast<f32>(det::cos(half))};
}

Quat leaf_heading(const geometry::ClusterFileDoor& record) noexcept {
  const f64 c = std::clamp(static_cast<f64>(record.shut_x), -1.0, 1.0);
  const f64 half_cos = std::sqrt(std::max(0.0, (1.0 + c) * 0.5));
  f64 half_sin = std::sqrt(std::max(0.0, (1.0 - c) * 0.5));
  if (record.shut_z > 0.0f) half_sin = -half_sin;  // the heading's sine is -shut_z
  return Quat{0.0f, static_cast<f32>(half_sin), 0.0f, static_cast<f32>(half_cos)};
}

i32 leaf_side(const geometry::ClusterFileDoor& record) noexcept {
  const f32 cross_y = record.shut_z * record.swing_x - record.shut_x * record.swing_z;
  return cross_y >= 0.0f ? 1 : -1;
}

i32 leaf_declared_mdeg(const geometry::ClusterFileDoor& record) noexcept {
  return leaf_side(record) * std::clamp(static_cast<i32>(record.open_deg), 0, 90) * 1000;
}

namespace {

// The generator a mesh's path names, when it answers `leaves`.
const assets::MeshGeneratorDesc* leaf_generator(std::string_view path) noexcept {
  const std::string_view name = assets::mesh_generator_of(path);
  if (name.empty()) return nullptr;
  const assets::MeshGeneratorDesc* g = assets::MeshGeneratorRegistry::global().find(name);
  return g != nullptr && g->leaves != nullptr ? g : nullptr;
}

Vec3 scaled(Vec3 v, Vec3 s) noexcept { return Vec3{v.x * s.x, v.y * s.y, v.z * s.z}; }

std::span<const u8> bytes_of(const std::string& text) noexcept {
  return {reinterpret_cast<const u8*>(text.data()), text.size()};
}

// Where the leaf stands in its house's frame at its declared angle: the pivot through the house's
// scale, and the house's turn times the leaf's heading and declared angle — the pose the
// collision's door takes when it is made (`declared_door`, `door_leaf_pose`), by the same
// functions.
Quat rest_turn(const geometry::ClusterFileDoor& record) noexcept {
  return leaf_heading(record) * hinge_turn(leaf_declared_mdeg(record));
}

}  // namespace

bool placement_mesh_hangs_leaves(const scene_gen::PlacementMesh& mesh) noexcept {
  return !mesh.recipe.empty() && leaf_generator(mesh.path) != nullptr;
}

bool hang_placement_leaves(scene_gen::Placements& placed, u32 resident, Vector<SceneLeaf>* hung,
                           std::string& error) {
  const u32 meshes = placed.meshes.size();
  const u32 placements = placed.instances.size();
  Vector<assets::GeneratedLeaf> leaves;
  for (u32 k = 0; k < meshes; ++k) {
    if (!placement_mesh_hangs_leaves(placed.meshes[k])) continue;
    const assets::MeshGeneratorDesc* g = leaf_generator(placed.meshes[k].path);
    // Copied: the appends below may move `placed.meshes`.
    const std::string house_path = placed.meshes[k].path;
    const std::string house_name = placed.meshes[k].name;
    if (!g->leaves(bytes_of(placed.meshes[k].recipe), house_path, leaves, error)) return false;
    if (leaves.empty()) continue;
    const u32 first_leaf = placed.meshes.size();
    for (u32 l = 0; l < leaves.size(); ++l) {
      // Its reach about the hinge, where it is placed: the far corner of the leaf's box, at any
      // angle of its swing (its width, height and thickness, not the larger of the first two).
      const geometry::ClusterFileDoor& r = leaves[l].record;
      const f32 reach = std::sqrt(r.width_m * r.width_m + r.height_m * r.height_m +
                                  r.thickness_m * r.thickness_m);
      placed.meshes.push_back(scene_gen::PlacementMesh(leaves[l].path,
                                                       house_name + ".leaf" + std::to_string(l), 0,
                                                       std::move(leaves[l].recipe), reach));
    }
    for (u32 i = 0; i < placements; ++i) {
      const scene_gen::Placement house = placed.instances[i];
      if (house.mesh != resident + k) continue;
      for (u32 l = 0; l < leaves.size(); ++l) {
        const geometry::ClusterFileDoor& record = leaves[l].record;
        scene_gen::Placement leaf = house;
        leaf.position =
            house.position + DVec3{rotate(house.rotation, scaled(record.pivot, house.scale))};
        leaf.rotation = house.rotation * rest_turn(record);
        leaf.mesh = resident + first_leaf + l;
        leaf.id = 0;  // a leaf is named by its house and its index, never by an id of its own
        // `mesh` the placement's own index (resident meshes counted), as `leaf.mesh` names it.
        if (hung != nullptr)
          hung->push_back(SceneLeaf{placed.instances.size(), i, l, leaf.mesh, house.scale.x,
                                    leaf_declared_mdeg(record)});
        placed.instances.push_back(leaf);
      }
    }
  }
  return true;
}

bool hang_scene_leaves(SceneDesc& desc, std::string& error) {
  desc.leaves.clear();
  const u32 meshes = desc.meshes.size();
  const u32 instances = desc.instances.size();
  const bool streams = desc.world.enabled;
  Vector<assets::GeneratedLeaf> leaves;
  std::string text;
  for (u32 m = 0; m < meshes; ++m) {
    const std::string house_path = desc.meshes[m];
    const assets::MeshGeneratorDesc* g = leaf_generator(house_path);
    if (g == nullptr) continue;
    if (m < desc.mesh_info.size() && desc.mesh_info[m].fit.active()) continue;
    // The bytes: a recipe handed under this path (matched by its end, as the load matches it), or
    // the file.
    const MeshRecipe* handed = nullptr;
    for (const MeshRecipe& recipe : desc.recipes) {
      const std::string& p = recipe.path;
      const bool tail = house_path.size() > p.size() &&
                        house_path.compare(house_path.size() - p.size(), p.size(), p) == 0 &&
                        (house_path[house_path.size() - p.size() - 1] == '/' ||
                         house_path[house_path.size() - p.size() - 1] == '\\');
      if (house_path == p || tail) {
        handed = &recipe;
        break;
      }
    }
    // Is any instance of it one that hangs? Read nothing for a mesh nobody places.
    bool placed = false;
    for (u32 i = 0; i < instances && !placed; ++i)
      placed = desc.instances[i].mesh == m;
    if (!placed) continue;
    if (handed != nullptr) {
      text = handed->bytes;
    } else {
      const io::Status status = io::read_file(house_path, text);
      if (status != io::Status::Ok) {
        error = "cannot read " + house_path + " for its leaves: " + io::status_name(status);
        return false;
      }
    }
    if (!g->leaves(bytes_of(text), house_path, leaves, error)) {
      error = house_path + ": " + error;
      return false;
    }
    if (leaves.empty()) continue;
    // Each leaf's mesh: one for every house that shares it (its path names its recipe).
    Vector<u32> leaf_mesh;
    leaf_mesh.reserve(leaves.size());
    for (assets::GeneratedLeaf& leaf : leaves) {
      u32 found = ~0u;
      for (u32 j = 0; j < desc.meshes.size(); ++j)
        if (desc.meshes[j] == leaf.path) found = j;
      if (found == ~0u) {
        found = desc.meshes.size();
        desc.meshes.push_back(leaf.path);
        desc.recipes.push_back(MeshRecipe{leaf.path, std::move(leaf.recipe)});
        if (!desc.mesh_info.empty()) {
          SceneMeshInfo info;
          const std::string_view stem = house_path;
          info.name = std::string(stem.substr(stem.find_last_of("/\\") + 1)) + ".leaf" +
                      std::to_string(leaf_mesh.size());
          desc.mesh_info.push_back(std::move(info));
        }
      }
      leaf_mesh.push_back(found);
    }
    for (u32 i = 0; i < instances; ++i) {
      const SceneInstance house = desc.instances[i];
      if (house.mesh != m || house.joints != 0) continue;
      if (streams && i >= desc.authored_first && i - desc.authored_first < desc.authored_count)
        continue;
      for (u32 l = 0; l < leaves.size(); ++l) {
        const geometry::ClusterFileDoor& record = leaves[l].record;
        SceneInstance leaf = house;
        leaf.mesh = leaf_mesh[l];
        // The pivot in the house's frame through its turn and scale, from where the house stands,
        // in f64 (ADR-0053): the leaf's transform is a turn and a scale with no offset.
        const Vec3 pivot =
            rotate(house.transform.rotation, scaled(record.pivot, house.transform.scale));
        leaf.origin = house.origin + DVec3{house.transform.position} + DVec3{pivot};
        leaf.transform.position = Vec3{};
        leaf.transform.rotation = house.transform.rotation * rest_turn(record);
        leaf.animation = SceneAnimation{};
        leaf.motion_key = k_motion_key_load;
        desc.leaves.push_back(SceneLeaf{desc.instances.size(), i, l, leaf_mesh[l],
                                        house.transform.scale.x, leaf_declared_mdeg(record)});
        desc.instances.push_back(leaf);
      }
    }
  }
  return true;
}

}  // namespace engine::renderer
