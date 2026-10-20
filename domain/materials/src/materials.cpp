// Material sets and packs (materials.h; docs/subsystems/materials.md).
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/log/log.h>
#include <domain/geometry/material_sampling.h>
#include <domain/materials/materials.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <mutex>
#include <utility>

namespace engine::materials {

ENGINE_LOG_CATEGORY_DEFINE(log_materials, "materials");

bool names_images(const MaterialSet& set) noexcept {
  return !set.base_color.empty() || !set.normal.empty() || !set.roughness.empty() ||
         !set.metallic.empty() || !set.height.empty();
}

bool set_in_range(const MaterialSet& set, std::string_view field, std::string& error) {
  // Written so that a NaN fails both.
  if (!(set.tile_m >= k_tile_min_m && set.tile_m <= k_tile_max_m)) {
    error = std::string(field) + ".tile_m is out of range (0.05 to 20 m)";
    return false;
  }
  if (!(set.normal_scale >= 0.0f && set.normal_scale <= k_normal_scale_max)) {
    error = std::string(field) + ".normal_scale is out of range (0 to 4)";
    return false;
  }
  return true;
}

namespace {

// The image's index in `data.image_paths`, appended when it is not there yet.
i32 image_index(geometry::ClusterFileData& data, const std::string& path) {
  for (u32 i = 0; i < data.image_paths.size(); ++i) {
    if (data.image_paths[i] == path) return static_cast<i32>(i);
  }
  data.image_paths.push_back(path);
  // Named by path: the container carries no bytes for it (cluster_file.h, ClusterFileImage).
  while (data.images.size() < data.image_paths.size())
    data.images.push_back(geometry::ClusterImage{});
  return static_cast<i32>(data.image_paths.size() - 1);
}

void scale_slot(geometry::ClusterFileMaterialSampling& record, u32 slot, f32 scale) {
  record.slots[slot].scale[0] = scale;
  record.slots[slot].scale[1] = scale;
}

// glTF's MASK as the alpha word stores the mode (assets::k_alpha_mask), and its default cutoff.
constexpr u8 k_alpha_mask_mode = 1;
constexpr f32 k_card_cutoff = 0.5f;

}  // namespace

void apply_set(const MaterialSet& set, UvLayout layout, geometry::ClusterFileData& data,
               u32 material) {
  if (!names_images(set)) return;
  geometry::ClusterFileMaterial& m = data.materials[material];
  geometry::ClusterFileMaterialSampling& sampling = data.material_sampling[material];
  const f32 scale = layout == UvLayout::metres ? 1.0f / set.tile_m : 1.0f;
  if (!set.base_color.empty()) {
    m.base_color_image = image_index(data, set.base_color);
    // The image is the colour; the flat colour it replaces would tint it.
    m.base_color = Vec4{1.0f, 1.0f, 1.0f, m.base_color.w};
    scale_slot(sampling, geometry::k_slot_base_color, scale);
    // **A card is a cutout** (renderer.md, "Alpha-tested geometry"): a leaf or a needle spray is
    // the image whole on a parallelogram, and the image's alpha is the leaf's outline, so a card
    // set is alpha-tested at glTF's default cutoff. An image without alpha passes everywhere and
    // draws what an opaque card drew.
    if (layout == UvLayout::card) {
      m.alpha = geometry::encode_alpha_word(k_alpha_mask_mode, geometry::alpha_word_double_sided(m.alpha),
                                            k_card_cutoff);
    }
  }
  if (!set.roughness.empty()) {
    // Roughness alone in glTF's metallic-roughness slot: G is roughness, and the B a greyscale
    // image also fills is multiplied by a metallic factor of zero (materials.h).
    m.metallic_roughness_image = geometry::encode_optional_image(image_index(data, set.roughness));
    m.roughness = 1.0f;
    m.metallic = 0.0f;
    scale_slot(sampling, geometry::k_slot_metallic_roughness, scale);
  }
  if (!set.normal.empty()) {
    m.normal_image = image_index(data, set.normal);
    m.normal_scale = set.normal_scale;
    scale_slot(sampling, geometry::k_slot_normal, scale);
  }
}

// ---- packs
// ---------------------------------------------------------------------------------------

namespace {

bool number_of(const JsonValue* value, f64& out) {
  if (value == nullptr || !value->is_number()) return false;
  if (value->is_float()) {
    out = value->as_float();
  } else if (value->is_int()) {
    out = static_cast<f64>(value->as_int());
  } else {
    out = static_cast<f64>(value->as_uint());
  }
  return true;
}

bool string_of(const JsonValue* value, std::string& out) {
  if (value == nullptr || !value->is_string()) return false;
  out = std::string(value->as_string());
  return true;
}

}  // namespace

bool parse_pack(std::string_view text, std::string_view dir, Pack& out, std::string& error) {
  out = Pack{};
  JsonValue root;
  const JsonParseResult parsed = parse_json(text, root);
  if (!parsed.ok) {
    error = "pack.json line " + std::to_string(parsed.line) + ": " + parsed.message;
    return false;
  }
  std::string format;
  if (!root.is_object() || !string_of(root.find("format"), format) || format != k_pack_format) {
    error = std::string("pack.json is not an ") + k_pack_format;
    return false;
  }
  out.dir = std::string(dir);
  string_of(root.find("name"), out.name);
  string_of(root.find("version"), out.version);
  const JsonValue* roles = root.find("roles");
  if (roles == nullptr || !roles->is_object()) {
    error = "pack.json has no roles object";
    return false;
  }
  for (const auto& [role, entry] : roles->as_object()) {
    const std::string where = "pack.json roles." + role;
    // `<species or rock>/<part>`: two words and one slash, which is what a recipe's part looks up.
    const usize slash = role.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 == role.size() ||
        role.find('/', slash + 1) != std::string::npos) {
      error = where + " is not a role (<species>/<part>)";
      return false;
    }
    if (!entry.is_object()) {
      error = where + " is not an object";
      return false;
    }
    PackRole held;
    held.role = role;
    f64 tile = 0.0;
    if (!number_of(entry.find("tile_m"), tile)) {
      error = where + " has no tile_m";
      return false;
    }
    held.set.tile_m = static_cast<f32>(tile);
    f64 normal_scale = 1.0;
    if (entry.find("normal_scale") != nullptr &&
        !number_of(entry.find("normal_scale"), normal_scale)) {
      error = where + ".normal_scale is not a number";
      return false;
    }
    held.set.normal_scale = static_cast<f32>(normal_scale);
    std::string convention;
    if (!string_of(entry.find("normal_convention"), convention) || convention != "opengl") {
      // The renderer reads glTF's tangent space, +Y up; a DirectX map would light every bump
      // from below, and flipping it is the packer's job, not a guess here.
      error = where + ".normal_convention is not \"opengl\"";
      return false;
    }
    const JsonValue* maps = entry.find("maps");
    if (maps == nullptr || !maps->is_object()) {
      error = where + " has no maps";
      return false;
    }
    struct Slot {
      const char* name;
      std::string* path;
    };
    const Slot slots[] = {{"base_color", &held.set.base_color},
                          {"normal", &held.set.normal},
                          {"roughness", &held.set.roughness},
                          {"metallic", &held.set.metallic},
                          {"height", &held.set.height}};
    for (const Slot& slot : slots) {
      const JsonValue* map = maps->find(slot.name);
      std::string file;
      if (map == nullptr || !map->is_object() || !string_of(map->find("file"), file) ||
          file.empty() || io::is_absolute_path(file)) {
        error = where + ".maps." + slot.name + " names no file in the pack";
        return false;
      }
      *slot.path = io::join_path(dir, file);
    }
    if (!set_in_range(held.set, where, error)) return false;
    out.roles.push_back(std::move(held));
  }
  std::sort(out.roles.begin(), out.roles.end(),
            [](const PackRole& a, const PackRole& b) { return a.role < b.role; });
  out.hash = hash_combine(hash_bytes(text.data(), text.size()), hash_bytes(dir.data(), dir.size()));
  return true;
}

namespace {

struct PackCache {
  std::mutex mutex;
  // A handful of entries, searched linearly, never in a frame.
  Vector<std::pair<std::string, std::unique_ptr<Pack>>> packs;  // a null pack: looked, none there
  Vector<std::string> roles_said;
};

PackCache& cache() {
  static PackCache held;
  return held;
}

std::string absolute_dir(std::string_view dir) {
  std::error_code ec;
  const std::filesystem::path absolute = std::filesystem::absolute(std::filesystem::path(dir), ec);
  return io::normalize_path(ec ? std::string(dir) : absolute.generic_string());
}

}  // namespace

const Pack* pack_at(std::string_view dir_in) {
  if (dir_in.empty()) return nullptr;
  const std::string dir = absolute_dir(dir_in);
  PackCache& held = cache();
  const std::lock_guard<std::mutex> lock(held.mutex);
  for (const auto& [where, pack] : held.packs) {
    if (where == dir) return pack.get();
  }
  std::unique_ptr<Pack> pack;
  const std::string manifest = io::join_path(dir, "pack.json");
  if (!io::exists(manifest)) {
    ENGINE_LOG_INFO(log_materials, "no material pack; generated meshes draw their flat colours",
                    log::field("dir", dir));
  } else {
    std::string text;
    std::string error;
    const io::Status status = io::read_file(manifest, text);
    pack = std::make_unique<Pack>();
    if (status != io::Status::Ok) {
      error = std::string("cannot read pack.json: ") + io::status_name(status);
    } else {
      parse_pack(text, dir, *pack, error);  // `error` says why when it refuses
    }
    if (!error.empty()) {
      ENGINE_LOG_WARN(log_materials, "material pack refused", log::field("dir", dir),
                      log::field("error", error));
      pack.reset();
    } else {
      ENGINE_LOG_INFO(log_materials, "material pack", log::field("dir", dir),
                      log::field("name", pack->name), log::field("version", pack->version),
                      log::field("roles", pack->roles.size()));
    }
  }
  held.packs.push_back({dir, std::move(pack)});
  return held.packs.back().second.get();
}

std::string default_pack_dir(std::string_view start) {
  const std::string ddc = geometry::find_ddc_root(start);
  if (ddc.empty()) return std::string();
  return io::join_path(io::join_path(io::parent_path(ddc), "content"), "materials");
}

const MaterialSet* find_role(const Pack* pack, std::string_view role) noexcept {
  if (pack == nullptr) return nullptr;
  for (const PackRole& held : pack->roles) {
    if (held.role == role) return &held.set;
  }
  return nullptr;
}

u64 pack_key(const Pack* pack) noexcept {
  return pack == nullptr || pack->roles.empty() ? 0u : pack->hash;
}

const MaterialSet* resolve_set(const MaterialSet* recipe, const Pack* pack, std::string_view role,
                               bool* from_pack) {
  if (from_pack != nullptr) *from_pack = false;
  if (recipe != nullptr && names_images(*recipe)) return recipe;
  if (const MaterialSet* found = find_role(pack, role)) {
    if (from_pack != nullptr) *from_pack = true;
    return found;
  }
  PackCache& held = cache();
  const std::lock_guard<std::mutex> lock(held.mutex);
  for (const std::string& said : held.roles_said) {
    if (said == role) return nullptr;
  }
  held.roles_said.push_back(std::string(role));
  ENGINE_LOG_INFO(log_materials, "no material set for the role; it draws its flat colour",
                  log::field("role", role), log::field("pack", pack != nullptr ? pack->dir : ""));
  return nullptr;
}

}  // namespace engine::materials
