// The hosts a generated mesh grows over (generator_hosts.h).
#include <domain/assets/gltf.h>
#include <domain/content_build/generator_hosts.h>
#include <domain/geometry/cluster_file.h>
#include <foundation/io/vfs.h>

#include <span>
#include <utility>

namespace engine::content_build {

namespace {

bool identify_hook(void* context, std::string_view path, u64& out, std::string& error) {
  return static_cast<const GeneratorHosts*>(context)->identify(path, out, error);
}

bool load_hook(void* context, std::string_view path, assets::GeneratorHostMesh& out,
               std::string& error) {
  return static_cast<const GeneratorHosts*>(context)->load(path, out, error);
}

std::span<const u8> bytes_of(const std::string& text) {
  return std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size());
}

}  // namespace

GeneratorHosts::GeneratorHosts(std::string dependant, const geometry::ClusterLodOptions& lod,
                               u32 page_bytes, const materials::Pack* pack, u32 depth)
    : dependant_(std::move(dependant)),
      lod_(lod),
      page_bytes_(page_bytes),
      pack_(pack),
      depth_(depth) {
  hooks_.context = this;
  hooks_.identify = identify_hook;
  hooks_.load = load_hook;
}

std::string GeneratorHosts::resolve(std::string_view path) const {
  if (io::is_absolute_path(path)) return std::string(path);
  const std::string_view dir = io::parent_path(dependant_);
  return dir.empty() ? std::string(path) : io::join_path(dir, path);
}

bool GeneratorHosts::identify(std::string_view path, u64& out, std::string& error) const {
  if (depth_ >= k_host_depth) {
    error = "a host's host is more than " + std::to_string(k_host_depth) +
            " levels deep: a loop of recipes?";
    return false;
  }
  const std::string native = resolve(path);
  const std::string_view name = assets::mesh_generator_of(native);
  if (!name.empty()) {
    const assets::MeshGeneratorRegistry& registry = assets::MeshGeneratorRegistry::global();
    const assets::MeshGeneratorDesc* generator = registry.find(name);
    if (generator == nullptr) {
      error = registry.unknown(native, name);
      return false;
    }
    std::string text;
    const io::Status status = io::read_file(native, text);
    if (status != io::Status::Ok) {
      error = "cannot read the host " + native + ": " + io::status_name(status);
      return false;
    }
    const GeneratorHosts inner(native, lod_, page_bytes_, pack_, depth_ + 1);
    if (!assets::generated_source_hash(*generator, bytes_of(text), inner.inputs(), out, error)) {
      error = native + ": " + error;
      return false;
    }
    return true;
  }
  if (io::extension(native) == ".clusters") {
    geometry::ClusterFileData data;
    std::string read_error;
    if (!geometry::read_cluster_file(native, data, &read_error)) {
      error = "the host " + native + ": " + read_error;
      return false;
    }
    out = data.source_hash != 0 ? data.source_hash : data.build_key;
    if (out == 0) out = geometry::cluster_file_hash(data);
    return true;
  }
  std::string hash_error;
  if (!assets::source_mesh_hash(native, out, &hash_error)) {
    error = "the host " + native + ": " + hash_error;
    return false;
  }
  return true;
}

bool GeneratorHosts::load(std::string_view path, assets::GeneratorHostMesh& out,
                          std::string& error) const {
  out = assets::GeneratorHostMesh{};
  u64 identity = 0;
  if (!identify(path, identity, error)) return false;
  const std::string native = resolve(path);
  const std::string_view name = assets::mesh_generator_of(native);
  if (!name.empty() || io::extension(native) == ".clusters") {
    geometry::ClusterFileData data;
    if (!name.empty()) {
      const assets::MeshGeneratorDesc* generator =
          assets::MeshGeneratorRegistry::global().find(name);
      std::string text;
      if (generator == nullptr || io::read_file(native, text) != io::Status::Ok) {
        error = "cannot read the host " + native;
        return false;
      }
      const GeneratorHosts inner(native, lod_, page_bytes_, pack_, depth_ + 1);
      if (!generator->build(bytes_of(text), inner.inputs(), lod_, page_bytes_, data, error)) {
        error = "the host " + native + ": " + error;
        return false;
      }
    } else {
      std::string read_error;
      if (!geometry::read_cluster_file(native, data, &read_error)) {
        error = "the host " + native + ": " + read_error;
        return false;
      }
    }
    assets::host_mesh_of(data, out);
    out.identity = identity;
    return true;
  }
  assets::MeshData mesh;
  std::string load_error;
  if (!assets::load_gltf(native, mesh, &load_error)) {
    error = "the host " + native + ": " + load_error;
    return false;
  }
  out.positions = std::move(mesh.positions);
  for (const assets::Primitive& primitive : mesh.primitives) {
    f32 roughness = 1.0f;
    if (primitive.material >= 0 && static_cast<u32>(primitive.material) < mesh.materials.size())
      roughness = mesh.materials[static_cast<u32>(primitive.material)].roughness;
    roughness = roughness < 0.0f ? 0.0f : (roughness > 1.0f ? 1.0f : roughness);
    for (u32 i = 0; i + 2 < primitive.index_count; i += 3) {
      for (u32 k = 0; k < 3; ++k)
        out.indices.push_back(mesh.indices[primitive.first_index + i + k]);
      out.roughness.push_back(roughness);
      out.holds.push_back(1u);
    }
  }
  out.identity = identity;
  return true;
}

}  // namespace engine::content_build
