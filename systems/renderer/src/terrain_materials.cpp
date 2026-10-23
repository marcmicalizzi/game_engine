#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <foundation/image/decode.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/terrain_materials.h>

#include <cmath>

namespace engine::renderer {

bool validate_terrain_materials(const scene::TerrainMaterials& m, std::string* error) {
  const auto fail = [&](const std::string& reason) {
    if (error != nullptr) *error = "terrain.materials: " + reason;
    return false;
  };
  if (m.layers.empty()) return fail("layers must not be empty");
  if (m.layer_budget == 0 || m.layers.size() > m.layer_budget)
    return fail("layers exceed layer_budget");
  if (!(m.detail_near_m >= 0 && std::isfinite(m.detail_near_m) &&
        m.detail_far_m > m.detail_near_m && std::isfinite(m.detail_far_m)))
    return fail("detail distances must be finite, nonnegative and increasing");
  for (const auto& l : m.layers) {
    const f32 pitch_mm = std::round(l.tile_m * 1000.0f);
    if (!(l.tile_m >= 0.001f && std::isfinite(pitch_mm) &&
          static_cast<f64>(pitch_mm) < 9223372036854775808.0))
      return fail(l.name + ": tile_m must form a positive signed-64-bit millimetre pitch");
    if (l.projection != scene::TerrainProjection::Map &&
        l.projection != scene::TerrainProjection::Triplanar)
      return fail(l.name + ": unknown projection");
    if (!(l.weight >= 0 && l.weight <= 1 && l.roughness_factor >= 0 && l.roughness_factor <= 1 &&
          l.normal_scale >= 0 && l.normal_scale <= 4))
      return fail(l.name + ": weight/roughness_factor must be in [0,1], normal_scale in [0,4]");
    const Vec3 tint = l.tint.value_or(Vec3{1, 1, 1});
    for (const f32 c : {tint.x, tint.y, tint.z})
      if (!(c >= 0 && c <= 1)) return fail(l.name + ": tint must be finite in [0,1]");
    for (const auto* path : {&l.base_color, &l.normal, &l.roughness, &l.mask}) {
      if (!path->empty() && !io::exists(*path)) return fail(l.name + ": missing image " + *path);
    }
  }
  return true;
}

bool append_terrain_materials(const TerrainDesc& terrain, assets::MeshData& mesh,
                              std::string* error) {
  if (!terrain.materials.has_value()) return true;
  if (!validate_terrain_materials(*terrain.materials, error)) return false;
  // Embed sources so cached terrain containers retain their exact image bytes. Roughness R
  // becomes glTF G with B=0; normals keep the existing OpenGL convention and BC5 build path.
  const auto add_image = [&](const std::string& path, bool roughness, i32& index, Vec3* mean) {
    index = -1;
    if (path.empty()) return true;
    image::Image decoded;
    if (image::read_image(path, decoded, 4, error) != io::Status::Ok) return false;
    if (mean != nullptr) {
      f64 sums[3]{};
      for (u32 p = 0; p < decoded.pixels.size(); p += 4) {
        for (u32 c = 0; c < 3; ++c) {
          const f64 v = decoded.pixels[p + (roughness ? 0 : c)] / 255.0;
          sums[c] +=
              roughness ? v : (v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4));
        }
      }
      const f64 count = static_cast<f64>(decoded.width) * decoded.height;
      *mean = Vec3{static_cast<f32>(sums[0] / count), static_cast<f32>(sums[1] / count),
                   static_cast<f32>(sums[2] / count)};
    }
    if (roughness) {
      for (u32 p = 0; p < decoded.pixels.size(); p += 4) {
        decoded.pixels[p + 1] = decoded.pixels[p];
        decoded.pixels[p + 2] = 0;
        decoded.pixels[p + 3] = 255;
      }
    }
    assets::ImageRef ref;
    ref.name = path;
    ref.mime_type = "image/png";
    if (!image::encode_png(decoded.width, decoded.height, 4,
                           std::span<const u8>(decoded.pixels.data(), decoded.pixels.size()),
                           ref.bytes)) {
      if (error != nullptr) *error = "terrain.materials: cannot encode " + path;
      return false;
    }
    index = static_cast<i32>(mesh.images.size());
    mesh.images.push_back(std::move(ref));
    return true;
  };
  for (const auto& layer : terrain.materials->layers) {
    assets::Material m;
    m.name = layer.name;
    const Vec3 tint = layer.tint.value_or(Vec3{1, 1, 1});
    m.base_color = Vec4{tint, 1.0f};
    m.metallic = 0;
    m.roughness = layer.roughness_factor;
    m.normal_scale = layer.normal_scale;
    Vec3 colour_mean{1, 1, 1};
    Vec3 roughness_mean{1, 1, 1};
    if (!add_image(layer.base_color, false, m.base_color_image, &colour_mean) ||
        !add_image(layer.normal, false, m.normal_image, nullptr) ||
        !add_image(layer.roughness, true, m.metallic_roughness_image, &roughness_mean) ||
        !add_image(layer.mask, false, m.occlusion_image, nullptr))
      return false;
    // Cold container transport for the two means. GPU upload moves them into uv_transform and
    // clears emissive/metallic before shading; no cluster directly names a layer entry.
    m.emissive = Vec3{colour_mean.x * tint.x, colour_mean.y * tint.y, colour_mean.z * tint.z};
    m.metallic = roughness_mean.x * layer.roughness_factor;
    for (u32 slot = 0; slot < geometry::k_material_slots; ++slot) {
      if (slot == geometry::k_slot_occlusion || layer.projection == scene::TerrainProjection::Map) {
        m.sampling[slot].sampler.wrap_s = geometry::TextureWrap::clamp_to_edge;
        m.sampling[slot].sampler.wrap_t = geometry::TextureWrap::clamp_to_edge;
      }
    }
    mesh.materials.push_back(std::move(m));
  }
  return true;
}

u64 terrain_materials_hash(const scene::TerrainMaterials& materials) {
  scene::TerrainMaterials identity = materials;
  for (auto& layer : identity.layers)
    for (auto* path : {&layer.base_color, &layer.normal, &layer.roughness, &layer.mask})
      path->clear();
  const std::string text = write_json(schema::to_json(identity));
  u64 h = hash_bytes(text.data(), text.size());
  for (const auto& layer : materials.layers) {
    for (const auto* path : {&layer.base_color, &layer.normal, &layer.roughness, &layer.mask}) {
      std::string bytes;
      h = hash_combine(h, !path->empty() && io::read_file(*path, bytes) == io::Status::Ok
                              ? hash_bytes(bytes.data(), bytes.size())
                              : 0u);
    }
  }
  return h;
}
}  // namespace engine::renderer
