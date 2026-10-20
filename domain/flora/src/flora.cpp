// The flora file, its key, and the container the solver's mesh is built into (flora.h).
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/assets/mesh_generator.h>
#include <domain/flora/flora.h>
#include <domain/flora/palm.h>
#include <domain/flora/trees.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/geometry/material_sampling.h>
#include <domain/materials/materials.h>
#include <domain/texture/material_textures.h>

#include <optional>
#include <span>
#include <string>
#include <utility>

namespace engine::flora {

namespace {
bool materials_in_range(const FloraFile& file, std::string& error);
}  // namespace

bool read_flora_file(std::string_view text, FloraFile& out, std::string& error) {
  out = FloraFile{};
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
    error = text_errors.empty() ? std::string("not an engine.flora.FloraFile") : text_errors;
    return false;
  }
  bool in_range = false;
  if (out.species == k_species_date_palm) {
    in_range = palm_in_range(out.palm, error);
  } else if (out.species == k_species_broadleaf) {
    in_range = broadleaf_in_range(out.broadleaf, error);
  } else if (out.species == k_species_conifer) {
    in_range = conifer_in_range(out.conifer, error);
  } else if (out.species == k_species_birch) {
    in_range = birch_in_range(out.birch, error);
  } else if (out.species == k_species_willow) {
    in_range = willow_in_range(out.willow, error);
  } else if (out.species == k_species_orchard) {
    in_range = orchard_in_range(out.orchard, error);
  } else if (out.species == k_species_ancient) {
    in_range = ancient_in_range(out.ancient, error);
  } else {
    error = "species \"" + out.species + "\" is not one the solver has (it has \"" +
            k_species_date_palm + "\", \"" + k_species_broadleaf + "\", \"" + k_species_conifer +
            "\", \"" + k_species_birch + "\", \"" + k_species_willow + "\", \"" + k_species_orchard +
            "\" and \"" + k_species_ancient + "\")";
    return false;
  }
  return in_range && materials_in_range(out, error);
}

namespace {

// A recipe's set in the shape domain/materials takes.
materials::MaterialSet set_of(const MaterialSet& s) {
  materials::MaterialSet out;
  out.base_color = s.base_color;
  out.normal = s.normal;
  out.roughness = s.roughness;
  out.metallic = s.metallic;
  out.height = s.height;
  out.tile_m = s.tile_m;
  out.normal_scale = s.normal_scale;
  return out;
}

// Which of the recipe's sets a part takes.
enum class Block : u8 { bark, leaf, skirt };

// One material of a species, in its material order: the part's word in its role, how its UVs are
// laid out, and the recipe's set it reads (flora.md, "Materials").
struct PartRole {
  const char* part;
  materials::UvLayout layout;
  Block block;
};

constexpr PartRole k_palm_parts[] = {{"bark", materials::UvLayout::metres, Block::bark},
                                     {"frond", materials::UvLayout::metres, Block::leaf},
                                     {"skirt", materials::UvLayout::metres, Block::skirt}};
constexpr PartRole k_conifer_parts[] = {{"bark", materials::UvLayout::metres, Block::bark},
                                        {"needles", materials::UvLayout::card, Block::leaf}};
// The broadleaf's, and any species' that says nothing of its own: a barked tube and leaf cards.
constexpr PartRole k_tree_parts[] = {{"bark", materials::UvLayout::metres, Block::bark},
                                     {"leaf", materials::UvLayout::card, Block::leaf}};

std::span<const PartRole> parts_of(const std::string& species) {
  if (species == k_species_date_palm) return k_palm_parts;
  if (species == k_species_conifer) return k_conifer_parts;
  return k_tree_parts;
}

const std::optional<MaterialSet>& block_set(const FloraMaterials& block, Block which) {
  switch (which) {
    case Block::leaf: return block.leaf;
    case Block::skirt: return block.skirt;
    case Block::bark: break;
  }
  return block.bark;
}

bool materials_in_range(const FloraFile& file, std::string& error) {
  if (!file.materials.has_value()) return true;
  const FloraMaterials& m = *file.materials;
  const std::pair<const char*, const std::optional<MaterialSet>*> sets[] = {
      {"bark", &m.bark}, {"leaf", &m.leaf}, {"skirt", &m.skirt}, {"wood", &m.wood}};
  for (const auto& [name, set] : sets) {
    if (!set->has_value()) continue;
    const std::string field = std::string("engine.flora.FloraFile.materials.") + name;
    if (!materials::set_in_range(set_of(**set), field, error)) return false;
  }
  return true;
}

}  // namespace

std::string flora_role(const std::string& species, u32 material) {
  const std::span<const PartRole> parts = parts_of(species);
  if (material >= parts.size()) return std::string();
  const std::string prefix = species == k_species_date_palm ? std::string("palm") : species;
  return prefix + "/" + parts[material].part;
}

void apply_flora_materials(const FloraFile& file, const materials::Pack* pack,
                           geometry::ClusterFileData& out) {
  const std::span<const PartRole> parts = parts_of(file.species);
  for (u32 i = 0; i < out.materials.size() && i < parts.size(); ++i) {
    materials::MaterialSet recipe;
    if (file.materials.has_value()) {
      const std::optional<MaterialSet>& set = block_set(*file.materials, parts[i].block);
      if (set.has_value()) recipe = set_of(*set);
    }
    if (const materials::MaterialSet* chosen =
            materials::resolve_set(&recipe, pack, flora_role(file.species, i)))
      materials::apply_set(*chosen, parts[i].layout, out, i);
  }
  // Where each image's built texture will be — its roles and options, and a zero key, since an
  // image named by path is not the container's bytes (texture/material_textures.h) — so a host's
  // texture step finds them from the container alone.
  if (!out.image_paths.empty()) texture::fill_cluster_texture_records(out);
}

namespace {
// The member of `engine.flora.FloraFile` a species' parameters are in.
const char* species_member(const std::string& species) {
  if (species == k_species_broadleaf) return "broadleaf";
  if (species == k_species_conifer) return "conifer";
  if (species == k_species_birch) return "birch";
  if (species == k_species_willow) return "willow";
  if (species == k_species_orchard) return "orchard";
  if (species == k_species_ancient) return "ancient";
  return "palm";
}
}  // namespace

std::string canonical_flora_json(const FloraFile& file) {
  // The species, its own parameters and the materials — nothing of the species not chosen, so a
  // palm's key does not move when the broadleaf's defaults do. The writer sorts the keys.
  const JsonValue whole = schema::to_json(file);
  JsonValue canonical = JsonValue::object();
  for (const char* key : {"species", species_member(file.species), "materials"}) {
    const JsonValue* value = whole.find(key);
    if (value != nullptr && !value->is_null()) canonical.set(key, *value);
  }
  JsonWriteOptions compact;
  compact.pretty = false;
  return write_json(canonical, compact);
}

u64 flora_source_hash(const FloraFile& file) {
  const std::string canonical = canonical_flora_json(file);
  u64 h = hash_bytes(canonical.data(), canonical.size());
  h = hash_combine(h, hash_bytes(file.species.data(), file.species.size()));
  return hash_combine(h, k_flora_version);
}

u64 flora_build_key(u64 source_hash, const geometry::ClusterLodOptions& lod, u32 page_bytes,
                    const materials::Pack* pack) {
  // No weld: the solver's vertices are shared where they are meant to be and nowhere else.
  return assets::generated_mesh_build_key(source_hash, assets::MeshGeneratorInputs{pack}, lod,
                                          page_bytes);
}

bool solve_flora(const FloraFile& file, assets::MeshData& mesh, Motion& motion,
                 PalmReport* palm_report, TreeReport* tree_report, std::string& error) {
  if (file.species == k_species_broadleaf)
    return solve_broadleaf(file.broadleaf, mesh, tree_report, &error, &motion);
  if (file.species == k_species_conifer)
    return solve_conifer(file.conifer, mesh, tree_report, &error, &motion);
  if (file.species == k_species_birch)
    return solve_birch(file.birch, mesh, tree_report, &error, &motion);
  if (file.species == k_species_willow)
    return solve_willow(file.willow, mesh, tree_report, &error, &motion);
  if (file.species == k_species_orchard)
    return solve_orchard(file.orchard, mesh, tree_report, &error, &motion);
  if (file.species == k_species_ancient)
    return solve_ancient(file.ancient, mesh, tree_report, &error, &motion);
  return solve_palm(file.palm, mesh, palm_report, &error, &motion);
}

bool build_flora_container(const FloraFile& file, const geometry::ClusterLodOptions& lod,
                           u32 page_bytes, geometry::ClusterFileData& out, PalmReport* report,
                           std::string& error, const materials::Pack* pack) {
  out = geometry::ClusterFileData{};
  assets::MeshData mesh;
  Motion motion;
  if (!solve_flora(file, mesh, motion, report, nullptr, error)) return false;
  // One DAG per primitive, each with its material's cone setting: the trunk is one-sided and keeps
  // its cones, a frond is seen from both faces and has none (flora.md, "Two-sided crowns").
  const geometry::AttributeSource attributes = assets::attribute_source(mesh);
  Vector<geometry::ClusterLodMesh> parts;
  Vector<u32> part_material;
  for (const assets::Primitive& primitive : mesh.primitives) {
    geometry::ClusterLodOptions options = lod;
    options.normal_cones = !mesh.materials[static_cast<u32>(primitive.material)].double_sided;
    const std::span<const u32> range(mesh.indices.data() + primitive.first_index,
                                     primitive.index_count);
    geometry::ClusterLodMesh part;
    std::string part_error;
    if (!geometry::build_cluster_lod(mesh.positions, range, options, part, &part_error,
                                     attributes)) {
      error = "the cluster build refused the tree's " +
              mesh.materials[static_cast<u32>(primitive.material)].name + ": " + part_error;
      return false;
    }
    parts.push_back(std::move(part));
    part_material.push_back(static_cast<u32>(primitive.material));
  }
  Vector<u32> part_of_cluster;
  std::string merge_error;
  if (!geometry::merge_cluster_lod(parts, out.mesh, &part_of_cluster, &merge_error)) {
    error = "the tree's parts did not merge: " + merge_error;
    return false;
  }
  if (page_bytes > 0) {
    Vector<u32> source_of_cluster;
    geometry::ClusterPagesOptions page_options;
    page_options.page_bytes = page_bytes;
    std::string page_error;
    if (!geometry::build_cluster_pages(out.mesh, page_options, out.pages, &page_error,
                                       &source_of_cluster)) {
      error = "the tree's pages were not laid out: " + page_error;
      return false;
    }
    geometry::permute_cluster_array(source_of_cluster, part_of_cluster);
  }
  // The motion contract, per cluster vertex through the vertex each was built from — after the
  // page layout, which reorders the vertices with their clusters.
  if (!motion_records(motion, out.mesh, out.motion_vertices)) {
    error = "the tree's motion attributes do not cover its vertices";
    return false;
  }
  out.motion_parts = std::move(motion.parts);
  out.structural_parts = std::move(motion.structure);  // section 37, where the species emits it
  out.cluster_material.reserve(part_of_cluster.size());
  for (const u32 part : part_of_cluster)
    out.cluster_material.push_back(part_material[part]);
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
  apply_flora_materials(file, pack, out);
  out.source_hash = flora_source_hash(file);
  out.build_key = flora_build_key(out.source_hash, lod, page_bytes, pack);
  out.generator = "flora";
  out.generator_kind = file.species;
  out.generator_params = canonical_flora_json(file);
  return true;
}

namespace {

// ---- the mesh generator "flora" (assets/mesh_generator.h)
// ------------------------------------------

std::string_view text_of(std::span<const u8> bytes) {
  return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool generator_source_hash(std::span<const u8> bytes, u64& out, std::string& error) {
  FloraFile file;
  if (!read_flora_file(text_of(bytes), file, error)) return false;
  out = flora_source_hash(file);
  return true;
}

bool generator_build(std::span<const u8> bytes, const assets::MeshGeneratorInputs& inputs,
                     const geometry::ClusterLodOptions& lod, u32 page_bytes,
                     geometry::ClusterFileData& out, std::string& error) {
  FloraFile file;
  if (!read_flora_file(text_of(bytes), file, error)) return false;
  return build_flora_container(file, lod, page_bytes, out, nullptr, error, inputs.materials);
}

constexpr assets::MeshGeneratorDesc k_flora_generator{
    .name = "flora",
    .source_hash = generator_source_hash,
    .build = generator_build,
};
const assets::MeshGeneratorRegistrar k_flora_registrar{k_flora_generator};

}  // namespace

}  // namespace engine::flora
