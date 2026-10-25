// The rock file, its key, and the container the solver's mesh is built into (rocks.h), and the
// mesh generator "rocks" that makes a scene's `.rock.json` a rock.
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/assets/mesh_generator.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/geometry/material_sampling.h>
#include <domain/materials/materials.h>
#include <domain/rocks/rocks.h>
#include <domain/texture/material_textures.h>

#include <string>

namespace engine::rocks {

namespace {

// A sentence naming the field and its range; written so that a NaN fails every test.
struct Check {
  std::string& error;
  const char* type;
  bool bad(const char* field, const char* range) {
    error = std::string("engine.rocks.") + type + "." + field + " is out of range (" + range + ")";
    return false;
  }
  bool f(f32 v, f32 lo, f32 hi, const char* field, const char* range) {
    return (v >= lo && v <= hi) ? true : bad(field, range);
  }
  bool u(u32 v, u32 lo, u32 hi, const char* field, const char* range) {
    return (v >= lo && v <= hi) ? true : bad(field, range);
  }
};

bool material_in_range(const RockMaterial& m, Check& c) {
  if (!c.f(m.tile_m, 0.05f, 20.0f, "material.tile_m", "0.05 to 20 m")) return false;
  return c.f(m.normal_scale, 0.0f, 4.0f, "material.normal_scale", "0 to 4");
}

bool surface_in_range(const RockSurface& s, Check& c) {
  return c.f(s.strength, 0.0f, 1.0f, "surface.strength", "0 to 1") &&
         c.f(s.joint_spacing_m, 0.05f, 20.0f, "surface.joint_spacing_m", "0.05 to 20 m") &&
         c.f(s.joint_width_m, 0.005f, s.joint_spacing_m * 0.5f, "surface.joint_width_m",
             "0.005 m to half joint_spacing_m") &&
         c.f(s.joint_depth_m, 0.0f, 2.0f, "surface.joint_depth_m", "0 to 2 m") &&
         c.f(s.wavelength_m, 0.02f, 10.0f, "surface.wavelength_m", "0.02 to 10 m") &&
         c.f(s.relief_m, 0.0f, s.wavelength_m * 0.25f < 0.5f ? s.wavelength_m * 0.25f : 0.5f,
             "surface.relief_m", "0 to min(0.5 m, wavelength_m / 4)");
}

// The block's set in the shape domain/materials takes.
materials::MaterialSet set_of(const RockMaterial& m) {
  materials::MaterialSet out;
  out.base_color = m.base_color.value_or(std::string());
  out.normal = m.normal.value_or(std::string());
  out.roughness = m.roughness.value_or(std::string());
  out.metallic = m.metallic.value_or(std::string());
  out.height = m.height.value_or(std::string());
  out.tile_m = m.tile_m;
  out.normal_scale = m.normal_scale;
  return out;
}

// The shape's block's material and type (the file holds the block, as `read_rock_file` leaves it).
RockMaterial* material_of(RockFile& file) {
  switch (file.shape) {
    case RockShape::Boulder: return file.boulder.has_value() ? &file.boulder->material : nullptr;
    case RockShape::Outcrop: return file.outcrop.has_value() ? &file.outcrop->material : nullptr;
    case RockShape::Scree: return file.scree.has_value() ? &file.scree->material : nullptr;
    case RockShape::Cliff: return file.cliff.has_value() ? &file.cliff->material : nullptr;
  }
  return nullptr;
}

RockType type_of(const RockFile& file) {
  switch (file.shape) {
    case RockShape::Boulder: return file.boulder.value_or(Boulder{}).type;
    case RockShape::Outcrop: return file.outcrop.value_or(Outcrop{}).type;
    case RockShape::Scree: return file.scree.value_or(Scree{}).type;
    case RockShape::Cliff: return file.cliff.has_value() ? file.cliff->type : Cliff{}.type;
  }
  return RockType::Granite;
}

bool weather_in_range(const std::optional<f32>& rounding, const std::optional<u32>& layers,
                      Check& c) {
  if (rounding.has_value() && !c.f(*rounding, 0.0f, 1.0f, "rounding", "0 to 1")) return false;
  if (layers.has_value() && !c.u(*layers, 0u, 64u, "layers", "0 to 64")) return false;
  return true;
}

}  // namespace

bool rock_file_in_range(const RockFile& file, std::string& error) {
  const auto other = [&](const char* block) {
    error = std::string("engine.rocks.RockFile.") + block + " is given, and the shape is " +
            rock_shape_name(file.shape) + ": only the shape's own block may be";
    return false;
  };
  switch (file.shape) {
    case RockShape::Boulder: {
      if (file.outcrop.has_value()) return other("outcrop");
      if (file.scree.has_value()) return other("scree");
      if (file.cliff.has_value()) return other("cliff");
      const Boulder p = file.boulder.value_or(Boulder{});
      Check c{error, "Boulder"};
      return c.f(p.size_x, 0.05f, 20.0f, "size_x", "0.05 to 20 m") &&
             c.f(p.size_y, 0.05f, 20.0f, "size_y", "0.05 to 20 m") &&
             c.f(p.size_z, 0.05f, 20.0f, "size_z", "0.05 to 20 m") &&
             c.u(p.joint_sets, 0u, 4u, "joint_sets", "0 to 4") &&
             weather_in_range(p.rounding, p.layers, c) &&
             c.f(p.buried, 0.0f, 0.8f, "buried", "0 to 0.8") &&
             c.u(p.triangle_budget, 500u, 500000u, "triangle_budget", "500 to 500,000") &&
             surface_in_range(p.surface, c) && material_in_range(p.material, c);
    }
    case RockShape::Outcrop: {
      if (file.boulder.has_value()) return other("boulder");
      if (file.scree.has_value()) return other("scree");
      if (file.cliff.has_value()) return other("cliff");
      const Outcrop p = file.outcrop.value_or(Outcrop{});
      Check c{error, "Outcrop"};
      return c.f(p.size_x, 0.2f, 50.0f, "size_x", "0.2 to 50 m") &&
             c.f(p.size_y, 0.2f, 50.0f, "size_y", "0.2 to 50 m") &&
             c.f(p.size_z, 0.2f, 50.0f, "size_z", "0.2 to 50 m") &&
             c.u(p.count, 2u, 16u, "count", "2 to 16") &&
             c.u(p.joint_sets, 0u, 4u, "joint_sets", "0 to 4") &&
             weather_in_range(p.rounding, p.layers, c) &&
             c.f(p.buried, 0.0f, 0.8f, "buried", "0 to 0.8") &&
             c.u(p.triangle_budget, 1000u, 1000000u, "triangle_budget", "1,000 to 1,000,000") &&
             surface_in_range(p.surface, c) && material_in_range(p.material, c);
    }
    case RockShape::Scree: {
      if (file.boulder.has_value()) return other("boulder");
      if (file.outcrop.has_value()) return other("outcrop");
      if (file.cliff.has_value()) return other("cliff");
      const Scree p = file.scree.value_or(Scree{});
      Check c{error, "Scree"};
      return c.f(p.size_x, 0.1f, 20.0f, "size_x", "0.1 to 20 m") &&
             c.f(p.size_z, 0.1f, 20.0f, "size_z", "0.1 to 20 m") &&
             c.u(p.count, 2u, 64u, "count", "2 to 64") &&
             c.f(p.fragment_m, 0.02f, 2.0f, "fragment_m", "0.02 to 2 m") &&
             weather_in_range(p.rounding, std::nullopt, c) &&
             c.f(p.buried, 0.0f, 0.8f, "buried", "0 to 0.8") &&
             c.u(p.triangle_budget, 500u, 500000u, "triangle_budget", "500 to 500,000") &&
             surface_in_range(p.surface, c) && material_in_range(p.material, c);
    }
    case RockShape::Cliff: {
      if (file.boulder.has_value()) return other("boulder");
      if (file.outcrop.has_value()) return other("outcrop");
      if (file.scree.has_value()) return other("scree");
      // A cliff's block cannot be left to its defaults: its line is what it is.
      if (!file.cliff.has_value()) {
        error = "engine.rocks.RockFile.cliff is absent: a cliff needs its line along the ground";
        return false;
      }
      return cliff_in_range(*file.cliff, error);
    }
  }
  error = "engine.rocks.RockFile.shape is not one the solver has";
  return false;
}

bool read_rock_file(std::string_view text, RockFile& out, std::string& error) {
  out = RockFile{};
  JsonValue root;
  const JsonParseResult parsed = parse_json(text, root);
  if (!parsed.ok) {
    error = "line " + std::to_string(parsed.line) + ": " + parsed.message;
    return false;
  }
  schema::ReadContext ctx;
  if (!schema::from_json(out, root, ctx) || !ctx.ok()) {
    std::string text_errors;
    for (const schema::Diagnostic& d : ctx.diagnostics) {
      if (!text_errors.empty()) text_errors += "; ";
      text_errors += d.path.empty() ? d.message : d.path + ": " + d.message;
    }
    error = text_errors.empty() ? std::string("not an engine.rocks.RockFile") : text_errors;
    return false;
  }
  if (!rock_file_in_range(out, error)) return false;
  // The shape's block, present: what the build keys and what the solver reads.
  switch (out.shape) {
    case RockShape::Boulder:
      if (!out.boulder.has_value()) out.boulder = Boulder{};
      break;
    case RockShape::Outcrop:
      if (!out.outcrop.has_value()) out.outcrop = Outcrop{};
      break;
    case RockShape::Scree:
      if (!out.scree.has_value()) out.scree = Scree{};
      break;
    case RockShape::Cliff: break;  // in range, so it is there
  }
  return true;
}

std::string canonical_rock_json(const RockFile& file) {
  RockFile filled = file;
  if (filled.shape == RockShape::Boulder && !filled.boulder.has_value()) filled.boulder = Boulder{};
  if (filled.shape == RockShape::Outcrop && !filled.outcrop.has_value()) filled.outcrop = Outcrop{};
  if (filled.shape == RockShape::Scree && !filled.scree.has_value()) filled.scree = Scree{};
  JsonValue json = schema::to_json(filled);
  // The shapes' blocks added after version 1 are left out when absent, so a boulder's, an
  // outcrop's or a scree's parameters — and the key over them — are the bytes they were before
  // the cliff was (rocks.md, "The build").
  if (json.is_object() && !filled.cliff.has_value()) json.as_object().erase(std::string("cliff"));
  JsonWriteOptions compact;
  compact.pretty = false;
  return write_json(json, compact);
}

u64 rock_source_hash(const RockFile& file) {
  const std::string canonical = canonical_rock_json(file);
  return hash_combine(hash_bytes(canonical.data(), canonical.size()), k_rocks_version);
}

u64 rock_build_key(u64 source_hash, const geometry::ClusterLodOptions& lod, u32 page_bytes,
                   const materials::Pack* pack) {
  return assets::generated_mesh_build_key(source_hash, assets::MeshGeneratorInputs{pack}, lod,
                                          page_bytes);
}

std::string rock_role(RockType type) {
  std::string role = std::string("rock/") + rock_type_name(type);
  for (char& c : role)
    c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  return role;
}

bool build_rock_container(const RockFile& file_in, const geometry::ClusterLodOptions& lod,
                          u32 page_bytes, geometry::ClusterFileData& out, RockReport* report,
                          std::string& error, const materials::Pack* pack) {
  out = geometry::ClusterFileData{};
  // The set the rock samples, chosen before the solve: the charts are laid out in whole repeats of
  // its `tile_m` (rocks.md, "UVs and materials"), so a pack's set, whose repeat is its own, is
  // solved at that repeat. The recipe's own images keep the recipe's.
  RockFile file = file_in;
  if (file.shape == RockShape::Boulder && !file.boulder.has_value()) file.boulder = Boulder{};
  if (file.shape == RockShape::Outcrop && !file.outcrop.has_value()) file.outcrop = Outcrop{};
  if (file.shape == RockShape::Scree && !file.scree.has_value()) file.scree = Scree{};
  if (material_of(file) == nullptr) {
    error = "engine.rocks.RockFile.shape is not one the solver has";
    return false;
  }
  const materials::MaterialSet recipe = set_of(*material_of(file));
  bool from_pack = false;
  const materials::MaterialSet* chosen =
      materials::resolve_set(&recipe, pack, rock_role(type_of(file)), &from_pack);
  if (from_pack) material_of(file)->tile_m = chosen->tile_m;
  assets::MeshData mesh;
  if (!solve_rock(file, mesh, report, &error)) return false;

  // One DAG, cones on: every body is closed and one-sided (geometry.md, "Normal cones").
  geometry::ClusterLodOptions options = lod;
  options.normal_cones = true;
  const geometry::AttributeSource attributes = assets::attribute_source(mesh);
  std::string build_error;
  if (!geometry::build_cluster_lod(mesh.positions,
                                   std::span<const u32>(mesh.indices.data(), mesh.indices.size()),
                                   options, out.mesh, &build_error, attributes)) {
    error = "the cluster build refused the rock: " + build_error;
    return false;
  }
  if (page_bytes > 0) {
    geometry::ClusterPagesOptions page_options;
    page_options.page_bytes = page_bytes;
    std::string page_error;
    if (!geometry::build_cluster_pages(out.mesh, page_options, out.pages, &page_error)) {
      error = "the rock's pages were not laid out: " + page_error;
      return false;
    }
  }
  out.cluster_material.assign(out.mesh.mesh.clusters.size(), 0u);
  for (const assets::Material& m : mesh.materials) {
    geometry::ClusterFileMaterial material;
    material.base_color = m.base_color;
    material.metallic = m.metallic;
    material.roughness = m.roughness;
    material.base_color_image = m.base_color_image;
    material.normal_image = m.normal_image;
    material.metallic_roughness_image = geometry::encode_optional_image(m.metallic_roughness_image);
    material.occlusion_image = geometry::encode_optional_image(m.occlusion_image);
    material.emissive_image = geometry::encode_optional_image(m.emissive_image);
    material.emissive = m.emissive;
    material.normal_scale = m.normal_scale;
    material.alpha = geometry::encode_alpha_word(m.alpha_mode, m.double_sided, m.alpha_cutoff);
    out.materials.push_back(material);
    out.material_sampling.push_back(
        geometry::encode_material_sampling(m.sampling, m.occlusion_strength));
  }
  if (chosen != nullptr && !out.materials.empty()) {
    // The charts are in repeats already: no transform.
    materials::apply_set(*chosen, materials::UvLayout::repeats, out, 0);
    texture::fill_cluster_texture_records(out);
  }
  // The identity is the file's as it was given: the pack's repeat is in the key through the pack.
  out.source_hash = rock_source_hash(file_in);
  out.build_key = rock_build_key(out.source_hash, lod, page_bytes, pack);
  out.generator = "rock";
  out.generator_kind = rock_shape_name(file_in.shape);
  out.generator_params = canonical_rock_json(file_in);
  return true;
}

namespace {

// ---- the mesh generator "rocks" (assets/mesh_generator.h) --------------------------------------
//
// A path's generator is the second extension before `.json`, so `granite.rock.json` names "rock":
// the registry's name is the file's word, singular, as the flora's is its file's.

std::string_view text_of(std::span<const u8> bytes) {
  return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool generator_source_hash(std::span<const u8> bytes, u64& out, std::string& error) {
  RockFile file;
  if (!read_rock_file(text_of(bytes), file, error)) return false;
  out = rock_source_hash(file);
  return true;
}

bool generator_build(std::span<const u8> bytes, const assets::MeshGeneratorInputs& inputs,
                     const geometry::ClusterLodOptions& lod, u32 page_bytes,
                     geometry::ClusterFileData& out, std::string& error) {
  RockFile file;
  if (!read_rock_file(text_of(bytes), file, error)) return false;
  return build_rock_container(file, lod, page_bytes, out, nullptr, error, inputs.materials);
}

constexpr assets::MeshGeneratorDesc k_rock_generator{
    .name = "rock",
    .source_hash = generator_source_hash,
    .build = generator_build,
};
const assets::MeshGeneratorRegistrar k_rock_registrar{k_rock_generator};

}  // namespace

}  // namespace engine::rocks
