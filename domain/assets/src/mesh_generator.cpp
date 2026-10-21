// The mesh-generator registry (mesh_generator.h).
#include <core/base/assert.h>
#include <core/hash/hash.h>
#include <domain/assets/mesh_generator.h>
#include <domain/materials/materials.h>

#include <algorithm>

namespace engine::assets {

std::string_view mesh_generator_of(std::string_view path) noexcept {
  constexpr std::string_view k_json = ".json";
  if (path.size() <= k_json.size() || path.substr(path.size() - k_json.size()) != k_json) return {};
  const std::string_view stem = path.substr(0, path.size() - k_json.size());
  // The second extension: after the last '.', within the file's own name.
  const usize dot = stem.find_last_of('.');
  const usize slash = stem.find_last_of("/\\");
  if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
  // A dot that begins the file's name is a hidden file's, not an extension.
  if (dot == 0 || (slash != std::string_view::npos && dot == slash + 1)) return {};
  const std::string_view name = stem.substr(dot + 1);
  return name.empty() ? std::string_view{} : name;
}

u64 generated_mesh_build_key(u64 source_hash, const MeshGeneratorInputs& inputs,
                             const geometry::ClusterLodOptions& lod, u32 page_bytes) noexcept {
  const u64 key = geometry::cluster_cache_key(source_hash, lod, false, page_bytes);
  const u64 pack = materials::pack_key(inputs.materials);
  return pack == 0 ? key : hash_combine(key, pack);
}

bool generated_source_hash(const MeshGeneratorDesc& desc, std::span<const u8> bytes,
                           const MeshGeneratorInputs& inputs, u64& out, std::string& error) {
  if (desc.source_hash_with != nullptr) return desc.source_hash_with(bytes, inputs, out, error);
  return desc.source_hash(bytes, out, error);
}

void host_mesh_of(const geometry::ClusterFileData& container, GeneratorHostMesh& out) {
  out = GeneratorHostMesh{};
  const geometry::ClusterLodMesh& lod = container.mesh;
  out.positions = lod.mesh.vertices;
  out.identity = container.source_hash != 0 ? container.source_hash : container.build_key;
  // Which parts hold a climber: part 0 (a trunk, or a rigid mesh's only part), and a part the
  // structural table flags load-bearing or a collider — the parts the walker's collision keeps
  // (scene_collision.md, "What gets no body"), for the same reason: they are the wood.
  const bool moving = container.motion_vertices.size() == lod.mesh.vertices.size() &&
                      !container.motion_vertices.empty();
  Vector<u8> part_holds;
  if (moving) {
    part_holds.resize(container.motion_parts.size() + 1u, 0u);
    part_holds[0] = 1u;
    for (const geometry::ClusterFileStructuralPart& record : container.structural_parts) {
      if (record.part < part_holds.size() &&
          (record.flags &
           (geometry::k_structural_load_bearing | geometry::k_structural_collider)) != 0)
        part_holds[record.part] = 1u;
    }
  }
  const auto vertex_holds = [&](u32 v) {
    if (!moving) return true;
    const u32 part = container.motion_vertices[v].part;
    return part < part_holds.size() && part_holds[part] != 0;
  };
  const bool has_lod = lod.lod.size() == lod.mesh.clusters.size();
  for (u32 c = 0; c < lod.mesh.clusters.size(); ++c) {
    if (has_lod && lod.lod[c].level != 0) continue;
    const geometry::ClusterDesc& cluster = lod.mesh.clusters[c];
    const u32 material = c < container.cluster_material.size() ? container.cluster_material[c] : 0u;
    const f32 roughness =
        material < container.materials.size() ? container.materials[material].roughness : 1.0f;
    for (u32 t = 0; t < cluster.triangle_count; ++t) {
      const u32 packed = lod.mesh.triangles[cluster.triangle_offset + t];
      const u32 a = cluster.vertex_offset + (packed & 0xFFu);
      const u32 b = cluster.vertex_offset + ((packed >> 8) & 0xFFu);
      const u32 d = cluster.vertex_offset + ((packed >> 16) & 0xFFu);
      out.indices.push_back(a);
      out.indices.push_back(b);
      out.indices.push_back(d);
      out.roughness.push_back(roughness < 0.0f ? 0.0f : (roughness > 1.0f ? 1.0f : roughness));
      out.holds.push_back(vertex_holds(a) && vertex_holds(b) && vertex_holds(d) ? 1u : 0u);
    }
  }
}

MeshGeneratorRegistry& MeshGeneratorRegistry::global() {
  // A function-local static, so a registrar in another translation unit's static initialization
  // finds it constructed whatever the order the linker put them in.
  static MeshGeneratorRegistry registry;
  return registry;
}

bool MeshGeneratorRegistry::add(const MeshGeneratorDesc& desc) noexcept {
  if (desc.name == nullptr || desc.name[0] == '\0' || desc.source_hash == nullptr ||
      desc.build == nullptr) {
    return false;
  }
  for (const MeshGeneratorDesc* held : generators_) {
    if (held == &desc) return true;
    if (std::string_view(held->name) == desc.name) return false;
  }
  generators_.push_back(&desc);
  return true;
}

const MeshGeneratorDesc* MeshGeneratorRegistry::find(std::string_view name) const noexcept {
  for (const MeshGeneratorDesc* held : generators_)
    if (std::string_view(held->name) == name) return held;
  return nullptr;
}

Vector<std::string_view> MeshGeneratorRegistry::names() const {
  Vector<std::string_view> out;
  for (const MeshGeneratorDesc* held : generators_)
    out.push_back(held->name);
  std::sort(out.begin(), out.end());
  return out;
}

std::string MeshGeneratorRegistry::unknown(std::string_view path, std::string_view name) const {
  std::string upper(name);
  for (char& c : upper)
    c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  std::string text = std::string(path) + " is a mesh made by the generator \"" + std::string(name) +
                     "\", which this build does not carry: it comes with the " + std::string(name) +
                     " capability (ENGINE_WITH_" + upper +
                     "), switched off here or not linked into this program; the generators "
                     "it has: ";
  const Vector<std::string_view> have = names();
  if (have.empty()) text += "none";
  for (u32 i = 0; i < have.size(); ++i) {
    if (i != 0) text += ", ";
    text += have[i];
  }
  return text;
}

MeshGeneratorRegistrar::MeshGeneratorRegistrar(const MeshGeneratorDesc& desc) noexcept {
  ENGINE_VERIFY(MeshGeneratorRegistry::global().add(desc),
                "assets: a mesh generator with no name, source_hash or build, or a second one "
                "under a name another holds");
}

}  // namespace engine::assets
