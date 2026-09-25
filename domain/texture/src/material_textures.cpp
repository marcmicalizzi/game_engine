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

namespace {

EdgeMode edge_of(geometry::TextureWrap wrap) noexcept {
  switch (wrap) {
    case geometry::TextureWrap::repeat: return EdgeMode::repeat;
    case geometry::TextureWrap::mirrored_repeat: return EdgeMode::mirror;
    case geometry::TextureWrap::clamp_to_edge: break;
  }
  return EdgeMode::clamp;
}

}  // namespace

void image_edges(std::span<const geometry::ClusterFileMaterial> materials,
                 std::span<const geometry::ClusterFileMaterialSampling> sampling, u32 image_count,
                 Vector<EdgeMode>& edge_x, Vector<EdgeMode>& edge_y) {
  edge_x.assign(image_count, EdgeMode::clamp);
  edge_y.assign(image_count, EdgeMode::clamp);
  Vector<bool> seen(image_count, false);
  for (u32 m = 0; m < materials.size(); ++m) {
    const geometry::ClusterFileMaterial& material = materials[m];
    const geometry::ClusterFileMaterialSampling record =
        m < sampling.size() ? sampling[m] : geometry::ClusterFileMaterialSampling{};
    geometry::TextureSlotSampling slots[geometry::k_material_slots];
    f32 strength = 1.0f;
    geometry::decode_material_sampling(record, slots, strength);
    const i32 images[geometry::k_material_slots] = {
        material.base_color_image,
        geometry::decode_optional_image(material.metallic_roughness_image), material.normal_image,
        geometry::decode_optional_image(material.occlusion_image),
        geometry::decode_optional_image(material.emissive_image)};
    for (u32 s = 0; s < geometry::k_material_slots; ++s) {
      const i32 image = images[s];
      if (image < 0 || static_cast<u32>(image) >= image_count) continue;
      const u32 i = static_cast<u32>(image);
      const EdgeMode x = edge_of(slots[s].sampler.wrap_s);
      const EdgeMode y = edge_of(slots[s].sampler.wrap_t);
      if (!seen[i]) {
        seen[i] = true;
        edge_x[i] = x;
        edge_y[i] = y;
        continue;
      }
      if (edge_x[i] != x) edge_x[i] = EdgeMode::clamp;
      if (edge_y[i] != y) edge_y[i] = EdgeMode::clamp;
    }
  }
}

void cluster_texture_records(std::span<const geometry::ClusterFileMaterial> materials,
                             std::span<const geometry::ClusterFileMaterialSampling> sampling,
                             std::span<const std::span<const u8>> embedded,
                             Vector<geometry::ClusterFileTexture>& out) {
  const u32 count = static_cast<u32>(embedded.size());
  Vector<u32> roles;
  image_roles(materials, count, roles);
  Vector<EdgeMode> edge_x;
  Vector<EdgeMode> edge_y;
  image_edges(materials, sampling, count, edge_x, edge_y);
  out.assign(count, geometry::ClusterFileTexture{});
  for (u32 i = 0; i < count; ++i) {
    geometry::ClusterFileTexture& record = out[i];
    record.roles = roles[i];
    if (roles[i] == 0) continue;
    TextureBuildOptions options = options_for_roles(roles[i]);
    options.edge_x = edge_x[i];
    options.edge_y = edge_y[i];
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
      std::span<const geometry::ClusterFileMaterialSampling>(data.material_sampling.data(),
                                                             data.material_sampling.size()),
      std::span<const std::span<const u8>>(embedded.data(), embedded.size()), data.textures);
}

}  // namespace engine::texture
