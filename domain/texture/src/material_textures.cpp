// Per-slot texture defaults and the container's texture records (material_textures.h).
#include <core/hash/hash.h>
#include <domain/texture/material_textures.h>

namespace engine::texture {

void image_roles(std::span<const geometry::ClusterFileMaterial> materials, u32 image_count,
                 Vector<u32>& out) {
  out.assign(image_count, 0u);
  auto mark = [&](i32 image, u32 role) {
    if (image >= 0 && static_cast<u32>(image) < image_count) out[static_cast<u32>(image)] |= role;
  };
  for (const geometry::ClusterFileMaterial& m : materials) {
    mark(m.base_color_image, k_role_base_color);
    mark(m.normal_image, k_role_normal);
    mark(geometry::decode_optional_image(m.metallic_roughness_image), k_role_metallic_roughness);
    mark(geometry::decode_optional_image(m.occlusion_image), k_role_occlusion);
    mark(geometry::decode_optional_image(m.emissive_image), k_role_emissive);
  }
}

TextureBuildOptions options_for_roles(u32 roles) noexcept {
  TextureBuildOptions options;
  options.mips = true;
  if ((roles & (k_role_base_color | k_role_emissive)) != 0) {
    options.format = FormatChoice::bc7;
    options.color_space = ColorSpace::srgb;
  } else if ((roles & k_role_normal) != 0) {
    options.format = FormatChoice::bc5;
    options.color_space = ColorSpace::linear;
    options.normal_map = true;
  } else if ((roles & k_role_metallic_roughness) != 0) {
    options.format = FormatChoice::bc7;
    options.color_space = ColorSpace::linear;
  } else {
    options.format = FormatChoice::bc4;
    options.color_space = ColorSpace::linear;
  }
  return options;
}

void cluster_texture_records(std::span<const geometry::ClusterFileMaterial> materials,
                             std::span<const std::span<const u8>> embedded,
                             Vector<geometry::ClusterFileTexture>& out) {
  const u32 count = static_cast<u32>(embedded.size());
  Vector<u32> roles;
  image_roles(materials, count, roles);
  out.assign(count, geometry::ClusterFileTexture{});
  for (u32 i = 0; i < count; ++i) {
    geometry::ClusterFileTexture& record = out[i];
    record.roles = roles[i];
    if (roles[i] == 0) continue;
    const TextureBuildOptions options = options_for_roles(roles[i]);
    record.options = pack_texture_options(options);
    const std::span<const u8> bytes = embedded[i];
    if (bytes.empty()) continue;  // named by path: resolved from the file when it is needed
    record.source_hash = hash_bytes(bytes.data(), bytes.size());
    record.key = texture_cache_key(record.source_hash, options);
  }
}

void fill_cluster_texture_records(geometry::ClusterFileData& data) {
  const u32 count = data.image_paths.size();
  Vector<std::span<const u8>> embedded;
  embedded.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    if (i < data.images.size() && !data.images[i].bytes.empty()) {
      embedded.push_back(
          std::span<const u8>(data.images[i].bytes.data(), data.images[i].bytes.size()));
    } else {
      embedded.push_back(std::span<const u8>());
    }
  }
  cluster_texture_records(
      std::span<const geometry::ClusterFileMaterial>(data.materials.data(), data.materials.size()),
      std::span<const std::span<const u8>>(embedded.data(), embedded.size()), data.textures);
}

}  // namespace engine::texture
