// The load's embedded image bytes, let go once the built textures stand in for them, and read again
// from the container by the few readers that come late (W51-3; renderer.md, "The embedded image
// bytes go once the textures are built").
#include "source_images.h"

#include <core/containers/hash_map.h>
#include <core/hash/hash.h>
#include <core/log/log.h>
#include <domain/geometry/cluster_file.h>
#include <domain/texture/texture_build.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/scene.h>

#include <renderer_log.h>
#include <string>

namespace engine::renderer {

namespace {

// Whether every image a material of `mesh` samples has its built `.tex` under the mesh's root: what
// `find_built_textures` will find, so the upload decodes none of them. `exists` remembers each key's
// answer for the load, since a forest's meshes share a few hundred textures.
bool every_sampled_image_built(const SourceMesh& mesh, HashMap<u64, u8>& exists) {
  const assets::MeshData& data = mesh.data;
  for (const assets::Material& material : data.materials) {
    for (const i32 image :
         {material.base_color_image, material.metallic_roughness_image, material.normal_image,
          material.occlusion_image, material.emissive_image}) {
      if (image < 0 || static_cast<u32>(image) >= data.images.size()) continue;
      const u64 key = mesh.textures[static_cast<u32>(image)].key;
      if (key == 0) return false;
      const u8* known = exists.find_value(key);
      if (known == nullptr) {
        const bool there = io::exists(texture::texture_cache_path(mesh.texture_ddc, key));
        exists.insert(key, there ? u8{1} : u8{0});
        if (!there) return false;
      } else if (*known == 0) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

ImageBytesReleased release_built_image_bytes(Vector<SourceMesh>& sources) {
  ImageBytesReleased out;
  HashMap<u64, u8> exists;
  for (SourceMesh& mesh : sources) {
    // The bytes go only where they can come back (the container this mesh was read from or written
    // to holds them) and where nothing the load knows of needs them (every sampled image built).
    if (mesh.image_bytes_released || mesh.container.empty() || mesh.texture_ddc.empty() ||
        mesh.data.images.empty() || mesh.textures.size() != mesh.data.images.size()) {
      continue;
    }
    u64 bytes = 0;
    for (const assets::ImageRef& image : mesh.data.images)
      bytes += image.bytes.capacity();
    if (bytes == 0 || !every_sampled_image_built(mesh, exists)) continue;
    for (assets::ImageRef& image : mesh.data.images) {
      if (!image.bytes.empty()) ++out.images;
      image.bytes = Vector<u8>();  // the capacity too: a cleared Vector keeps it
    }
    mesh.image_bytes_released = true;
    ++out.meshes;
    out.bytes += bytes;
  }
  ENGINE_LOG_INFO(log_renderer, "embedded image bytes released", log::field("meshes", out.meshes),
                  log::field("images", out.images), log::field("bytes", out.bytes),
                  log::field("textures_probed", static_cast<u32>(exists.size())));
  return out;
}

std::span<const u8> source_image_bytes(const SourceMesh& mesh, u32 image, Vector<u8>& storage,
                                       std::string* error) {
  if (image >= mesh.data.images.size()) {
    if (error != nullptr) *error = "image " + std::to_string(image) + " is not the mesh's";
    return {};
  }
  const assets::ImageRef& ref = mesh.data.images[image];
  if (!mesh.image_bytes_released || !ref.bytes.empty())
    return std::span<const u8>(ref.bytes.data(), ref.bytes.size());
  // The container's resident sections: the descriptors, the materials and the images, none of the
  // paged streams (`ClusterFileReader::open`).
  geometry::ClusterFileReader reader;
  geometry::ClusterFileData resident;
  std::string why;
  if (!reader.open(mesh.container, &resident, &why)) {
    if (error != nullptr) *error = "'" + mesh.container + "' cannot be read again: " + why;
    return {};
  }
  if (image >= resident.images.size() || resident.images[image].bytes.empty()) {
    // Named by path in the container too: no bytes, and no error.
    storage.clear();
    return {};
  }
  storage = std::move(resident.images[image].bytes);
  const u64 expected = image < mesh.textures.size() ? mesh.textures[image].source_hash : 0;
  if (expected != 0 && hash_bytes(storage.data(), storage.size()) != expected) {
    if (error != nullptr)
      *error = "'" + mesh.container + "' image " + std::to_string(image) +
               " is not the bytes the load built its texture from";
    storage.clear();
    return {};
  }
  return std::span<const u8>(storage.data(), storage.size());
}

}  // namespace engine::renderer
