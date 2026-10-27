#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/profiling/startup_trace.h>
#include <core/schema/json_reflect.h>
#include <foundation/image/decode.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/terrain_materials.h>

#include <cmath>

namespace engine::renderer {

bool validate_terrain_materials(const scene::TerrainMaterials& m, std::string* error, bool masked) {
  const auto fail = [&](const std::string& reason) {
    if (error != nullptr) *error = "terrain.materials: " + reason;
    return false;
  };
  if (m.layers.empty()) return fail("layers must not be empty");
  if (masked) {
    // With masks the layers are a palette and the budget is per tile (renderer.md, "Ground
    // materials by place"): a texel carries four weights.
    if (m.layers.size() > 254) return fail("a palette holds at most 254 layers");
    if (m.layer_budget < 1 || m.layer_budget > 4)
      return fail(
          "layer_budget is the per-tile budget with masks and must be 1 to 4 (a mask texel "
          "carries four weights); set it");
  } else if (m.layer_budget == 0 || m.layers.size() > m.layer_budget) {
    return fail("layers exceed layer_budget");
  }
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
    if (!(l.porosity >= 0 && l.porosity <= 1)) return fail(l.name + ": porosity must be in [0,1]");
    if (l.sampling != scene::TerrainSampling::Tiled &&
        l.sampling != scene::TerrainSampling::Stochastic)
      return fail(l.name + ": unknown sampling");
    if (l.sampling == scene::TerrainSampling::Stochastic &&
        l.projection != scene::TerrainProjection::Triplanar)
      return fail(l.name + ": stochastic sampling is for a triplanar layer (a Map is a painting)");
    if (!(l.rotation_deg >= 0 && l.rotation_deg <= 180))
      return fail(l.name + ": rotation_deg must be in [0,180]");
    if (l.rotation_deg > 0 && l.sampling != scene::TerrainSampling::Stochastic)
      return fail(l.name + ": rotation_deg turns a stochastic layer's tiles; this layer is tiled");
    for (const auto* path : {&l.base_color, &l.normal, &l.roughness, &l.mask, &l.height}) {
      if (!path->empty() && !io::exists(*path)) return fail(l.name + ": missing image " + *path);
    }
  }
  return true;
}

bool append_terrain_materials(const TerrainDesc& terrain, assets::MeshData& mesh,
                              std::string* error) {
  if (!terrain.materials.has_value()) return true;
  if (!validate_terrain_materials(*terrain.materials, error, terrain.masks.has_value()))
    return false;
  // Embed sources so cached terrain containers retain their exact image bytes. Roughness R
  // becomes glTF G with B=0; normals keep the existing OpenGL convention and BC5 build path.
  const auto add_image = [&](const std::string& path, bool roughness, i32& index, Vec3* mean,
                             const std::string& height = std::string(),
                             f32* height_mean = nullptr) {
    index = -1;
    if (path.empty() && height.empty()) return true;
    image::Image decoded;
    if (!path.empty() && image::read_image(path, decoded, 4, error) != io::Status::Ok) return false;
    if (path.empty()) {
      roughness = false;
      mean = nullptr;
    }
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
    if (!height.empty()) {
      // The palette's height detail rides in the map's B, which a terrain layer never reads as
      // metallic (renderer.md, "Ground materials by place"); without a roughness map G is 1.
      image::Image h;
      if (image::read_image(height, h, 4, error) != io::Status::Ok) return false;
      if (roughness && (h.width != decoded.width || h.height != decoded.height)) {
        if (error != nullptr)
          *error = "terrain.materials: " + height + " must be the size of its layer's roughness";
        return false;
      }
      if (!roughness) {
        decoded = h;
        for (u32 p = 0; p < decoded.pixels.size(); p += 4) {
          decoded.pixels[p] = 255;
          decoded.pixels[p + 1] = 255;
        }
      }
      f64 sum = 0;
      for (u32 p = 0; p < decoded.pixels.size(); p += 4) {
        decoded.pixels[p + 2] = h.pixels[p];
        decoded.pixels[p + 3] = 255;
        sum += h.pixels[p] / 255.0;
      }
      if (height_mean != nullptr)
        *height_mean = static_cast<f32>(sum / (static_cast<f64>(h.width) * h.height));
    }
    assets::ImageRef ref;
    ref.name = height.empty() ? path : path + "+" + height;
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
    f32 height_mean = 0.5f;
    if (!add_image(layer.base_color, false, m.base_color_image, &colour_mean) ||
        !add_image(layer.normal, false, m.normal_image, nullptr) ||
        !add_image(layer.roughness, true, m.metallic_roughness_image, &roughness_mean, layer.height,
                   &height_mean) ||
        !add_image(layer.mask, false, m.occlusion_image, nullptr))
      return false;
    // Cold container transport for the two means. GPU upload moves them into uv_transform and
    // clears emissive/metallic before shading; no cluster directly names a layer entry.
    m.emissive = Vec3{colour_mean.x * tint.x, colour_mean.y * tint.y, colour_mean.z * tint.z};
    m.metallic = roughness_mean.x * layer.roughness_factor;
    // A stochastic layer's height mean (R96), which its variance-preserving blend is taken about,
    // in the alpha cutoff a terrain layer never reads; a tiled layer's material is as it was.
    if (layer.sampling == scene::TerrainSampling::Stochastic) m.alpha_cutoff = height_mean;
    for (u32 slot = 0; slot < geometry::k_material_slots; ++slot) {
      if (slot == geometry::k_slot_occlusion || layer.projection == scene::TerrainProjection::Map) {
        m.sampling[slot].sampler.wrap_s = geometry::TextureWrap::clamp_to_edge;
        m.sampling[slot].sampler.wrap_t = geometry::TextureWrap::clamp_to_edge;
      }
    }
    mesh.materials.push_back(std::move(m));
  }
  // The terrain's record (`gfx::TerrainRecord`), one entry after the layers that no cluster names:
  // upload writes the layers' range, the detail distances, the map frame and the mask level into
  // it.
  assets::Material record;
  record.name = "terrain record";
  mesh.materials.push_back(std::move(record));
  return true;
}

u64 terrain_materials_hash(const scene::TerrainMaterials& materials) {
  scene::TerrainMaterials identity = materials;
  bool stochastic = false;
  for (auto& layer : identity.layers) {
    for (auto* path :
         {&layer.base_color, &layer.normal, &layer.roughness, &layer.mask, &layer.height})
      path->clear();
    stochastic = stochastic || layer.sampling == scene::TerrainSampling::Stochastic;
  }
  // The sampling fields (R96) are shading, read at upload, except that a stochastic layer's
  // container carries its height mean: they leave the identity text, so every scene before them
  // keeps its key, and a scene with a stochastic layer is keyed apart below.
  JsonValue json = schema::to_json(identity);
  if (JsonValue* layers = json.find("layers"); layers != nullptr && layers->is_array())
    for (JsonValue& layer : layers->as_array())
      if (layer.is_object()) {
        layer.as_object().erase(std::string("sampling"));
        layer.as_object().erase(std::string("rotation_deg"));
      }
  const std::string text = write_json(json);
  // 2: the terrain record entry after the layers (2026-10-24), so a container built before it,
  // which has none, is never read for a scene that needs one.
  u64 h = hash_combine(hash_bytes(text.data(), text.size()), 2u);
  if (stochastic) {
    h = hash_combine(h, 0x53544F4348415354ull);  // "STOCHAST"
    for (const auto& layer : materials.layers)
      h = hash_combine(h, layer.sampling == scene::TerrainSampling::Stochastic ? 1u : 0u);
  }
  for (const auto& layer : materials.layers) {
    for (const auto* path :
         {&layer.base_color, &layer.normal, &layer.roughness, &layer.mask, &layer.height}) {
      std::string bytes;
      h = hash_combine(h, !path->empty() && io::read_file(*path, bytes) == io::Status::Ok
                              ? hash_bytes(bytes.data(), bytes.size())
                              : 0u);
    }
  }
  return h;
}
}  // namespace engine::renderer

namespace engine::renderer {
bool prepare_terrain_masks(const TerrainDesc& terrain, TerrainMaskUpload& out, std::string* error) {
  ENGINE_STARTUP_STAGE("terrain masks");
  out = TerrainMaskUpload{};
  const auto fail = [&](const std::string& why) {
    if (error != nullptr) *error = "terrain.masks: " + why;
    return false;
  };
  const TerrainSampler sampler(terrain);
  if (!sampler.ok()) return fail(sampler.error());
  scene_gen::GroundMasks masks;
  std::string why;
  if (!sampler.provider().masks(masks, &why)) return fail(why);
  if (!masks.valid()) return true;
  scene_gen::GroundMaskInfo info;
  if (!masks.ops->info(masks.state, info) || info.cells == 0)
    return fail("the ground provider's mask level has no description");
  const u64 tiles_x = static_cast<u64>(info.tile_max_x - info.tile_min_x + 1);
  const u64 tiles_z = static_cast<u64>(info.tile_max_z - info.tile_min_z + 1);
  const u32 words = scene_gen::mask_tile_words(info.cells);
  const u64 pages = tiles_x * tiles_z;
  // Count the manifest's presence, never payloads, before holding the resident level (W76).
  if (masks.ops->present == nullptr) return fail("the ground provider has no mask presence query");
  u64 present = 0;
  Vector<u8> has(static_cast<u32>(pages), u8{0});
  for (u64 z = 0; z < tiles_z; ++z)
    for (u64 x = 0; x < tiles_x; ++x)
      if (masks.ops->present(masks.state, info.tile_min_x + static_cast<i32>(x),
                             info.tile_min_z + static_cast<i32>(z))) {
        has[static_cast<u32>(z * tiles_x + x)] = 1;
        ++present;
      }
  const u64 bytes = (pages + present * words) * sizeof(u32);
  if (bytes > u64{info.resident_mib} * 1024 * 1024) {
    return fail("the mask level is " + std::to_string(bytes / (1024 * 1024)) +
                " MiB resident, over the block's resident_mib of " +
                std::to_string(info.resident_mib) +
                "; raise it, or coarsen spacing_mm (streaming the level is W25's)");
  }
  out.words.assign(static_cast<u32>(pages + present * words), 0u);
  u32 slot = 0;
  for (u64 p = 0; p < pages; ++p) {
    if (has[static_cast<u32>(p)] == 0) {
      out.words[static_cast<u32>(p)] = gfx::k_terrain_no_mask_tile;
      continue;
    }
    const i32 tx = info.tile_min_x + static_cast<i32>(p % tiles_x);
    const i32 tz = info.tile_min_z + static_cast<i32>(p / tiles_x);
    std::span<u32> dst(out.words.data() + pages + static_cast<u64>(slot) * words, words);
    if (!masks.ops->tile(masks.state, tx, tz, dst))
      return fail("the mask tile (" + std::to_string(tx) + ", " + std::to_string(tz) +
                  ") did not read");
    out.words[static_cast<u32>(p)] = slot++;
  }
  out.tiles = slot;
  out.record.flags = gfx::k_terrain_masked;
  out.record.mask_tile_x0 = info.tile_min_x;
  out.record.mask_tile_z0 = info.tile_min_z;
  out.record.mask_tiles_x = static_cast<u32>(tiles_x);
  out.record.mask_tiles_z = static_cast<u32>(tiles_z);
  out.record.mask_spacing_mm = static_cast<u32>(info.spacing_mm);
  out.record.mask_cells = info.cells;
  out.record.mask_words = words;
  out.record.mask_mips = scene_gen::mask_mips(info.cells);
  out.record.wetness = info.wetness;
  out.record.height_blend = info.height_blend;
  return true;
}
}  // namespace engine::renderer
