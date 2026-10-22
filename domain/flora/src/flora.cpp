// The flora file, its key, and the container the solver's mesh is built into (flora.h).
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/assets/mesh_generator.h>
#include <domain/flora/flora.h>
#include <domain/flora/palm.h>
#include <domain/flora/trees.h>
#include <domain/flora/understory.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/geometry/material_sampling.h>
#include <domain/materials/materials.h>
#include <domain/texture/material_textures.h>
#include <foundation/image/png.h>

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
  } else if (out.species == k_species_shrub) {
    in_range = shrub_in_range(out.shrub, error);
  } else if (out.species == k_species_fern) {
    in_range = fern_in_range(out.fern, error);
  } else if (out.species == k_species_tuft) {
    in_range = tuft_in_range(out.tuft, error);
  } else if (out.species == k_species_herb) {
    in_range = herb_in_range(out.herb, error);
  } else {
    error = "species \"" + out.species + "\" is not one the solver has (it has \"" +
            k_species_date_palm + "\", \"" + k_species_broadleaf + "\", \"" + k_species_conifer +
            "\", \"" + k_species_birch + "\", \"" + k_species_willow + "\", \"" +
            k_species_orchard + "\", \"" + k_species_ancient + "\", \"" + k_species_shrub +
            "\", \"" + k_species_fern + "\", \"" + k_species_tuft + "\" and \"" + k_species_herb +
            "\")";
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
// `none`: a part no recipe block names (a herb's flowers: the pack's role or the flat colour).
enum class Block : u8 { bark, leaf, skirt, none };

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

// The understory's (flora.md, "The understory"): a fern's fronds, a tuft's blades, a herb's leaves
// and flowers, every one a card that takes its image whole.
constexpr PartRole k_fern_parts[] = {{"frond", materials::UvLayout::card, Block::leaf}};
constexpr PartRole k_tuft_parts[] = {{"blade", materials::UvLayout::card, Block::leaf}};
constexpr PartRole k_herb_parts[] = {{"leaf", materials::UvLayout::card, Block::leaf},
                                     {"flower", materials::UvLayout::card, Block::none}};

std::span<const PartRole> parts_of(const std::string& species) {
  if (species == k_species_date_palm) return k_palm_parts;
  if (species == k_species_conifer) return k_conifer_parts;
  if (species == k_species_fern) return k_fern_parts;
  if (species == k_species_tuft) return k_tuft_parts;
  if (species == k_species_herb) return k_herb_parts;
  return k_tree_parts;
}

const std::optional<MaterialSet>& block_set(const FloraMaterials& block, Block which) {
  static const std::optional<MaterialSet> k_nothing;
  switch (which) {
    case Block::leaf: return block.leaf;
    case Block::skirt: return block.skirt;
    case Block::none: return k_nothing;
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
  // A palm's roles are "palm/..." and a tuft's "grass/...": the material lab's words, not the
  // solver's.
  const std::string prefix = species == k_species_date_palm ? std::string("palm")
                             : species == k_species_tuft    ? std::string("grass")
                                                            : species;
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
  if (species == k_species_shrub) return "shrub";
  if (species == k_species_fern) return "fern";
  if (species == k_species_tuft) return "tuft";
  if (species == k_species_herb) return "herb";
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
                 PalmReport* palm_report, TreeReport* tree_report, std::string& error,
                 TreeLevels* levels) {
  if (levels != nullptr) *levels = TreeLevels{};
  if (file.species == k_species_broadleaf)
    return solve_broadleaf_chain(file.broadleaf, mesh, tree_report, &error, &motion, levels);
  if (file.species == k_species_conifer)
    return solve_conifer(file.conifer, mesh, tree_report, &error, &motion);
  if (file.species == k_species_birch)
    return solve_birch_chain(file.birch, mesh, tree_report, &error, &motion, levels);
  if (file.species == k_species_willow)
    return solve_willow(file.willow, mesh, tree_report, &error, &motion);
  if (file.species == k_species_orchard)
    return solve_orchard(file.orchard, mesh, tree_report, &error, &motion);
  if (file.species == k_species_ancient)
    return solve_ancient_chain(file.ancient, mesh, tree_report, &error, &motion, levels);
  if (file.species == k_species_shrub)
    return solve_shrub(file.shrub, mesh, tree_report, &error, &motion);
  if (file.species == k_species_fern)
    return solve_fern(file.fern, mesh, tree_report, &error, &motion);
  if (file.species == k_species_tuft)
    return solve_tuft(file.tuft, mesh, tree_report, &error, &motion);
  if (file.species == k_species_herb)
    return solve_herb(file.herb, mesh, tree_report, &error, &motion);
  return solve_palm(file.palm, mesh, palm_report, &error, &motion);
}

namespace {

// Bakes every level of the chain and appends what draws it: per level a colour and a normal atlas
// embedded in the container as PNG (sections 21 and 22; the texture step builds them like any
// embedded image) and a material — white, masked at one half, two-sided, the atlas's colour and
// tangent-space normal — at `mesh.materials.size() + k`, the index the clusters were given.
bool bake_chain(const assets::MeshData& mesh, const TreeLevels& chain,
                geometry::ClusterFileData& out, Vector<f32>& coverage, std::string& error) {
  BakeLook look;
  look.bark = mesh.materials[0].base_color;
  look.leaf = mesh.materials.size() > 1 ? mesh.materials[1].base_color : look.leaf;
  // A pack's leaf card is cut to the leaf (its image's alpha), so the far card is too.
  look.leaf_outline =
      out.materials.size() > 1 && geometry::alpha_word_mode(out.materials[1].alpha) != 0u;
  if (out.images.size() < out.image_paths.size()) out.images.resize(out.image_paths.size());
  for (const TreeLevel& level : chain.levels) {
    BakedAtlas atlas;
    bake_level(mesh, chain, level, look, atlas);
    // The coverage error ("The coverage bake"): the feature the level stands in for, times what it
    // changes at the pixel, kept rising so the cut's foliage term is monotonic along every path.
    const f32 difference = measure_level(mesh, chain, level, look, atlas);
    f32 level_coverage = level.feature_m * difference;
    const f32 below = coverage.empty() ? 0.0f : coverage.back();
    if (!(level_coverage > below))
      level_coverage = below > 0.0f ? below * (1.0f + 1.0f / 512.0f) : 1e-4f;
    coverage.push_back(level_coverage);
    const i32 colour_image = static_cast<i32>(out.image_paths.size());
    for (const Vector<u8>* pixels : {&atlas.colour, &atlas.normal}) {
      geometry::ClusterImage image;
      image.mime_type = "image/png";
      if (!image::encode_png(atlas.width, atlas.height, 4, *pixels, image.bytes)) {
        error = "the tree's impostor atlas did not encode";
        return false;
      }
      out.image_paths.push_back(std::string());
      out.images.push_back(std::move(image));
    }
    assets::Material m;
    m.name = level.kind == ImpostorKind::tree ? "impostor" : "branch_cards";
    m.base_color = Vec4{1.0f, 1.0f, 1.0f, 1.0f};
    m.roughness = mesh.materials.size() > 1 ? mesh.materials[1].roughness : 0.6f;
    m.metallic = 0.0f;
    m.double_sided = true;
    m.alpha_mode = assets::k_alpha_mask;
    m.alpha_cutoff = 0.5f;
    geometry::ClusterFileMaterial material;
    material.base_color = m.base_color;
    material.metallic = m.metallic;
    material.roughness = m.roughness;
    material.base_color_image = colour_image;
    material.normal_image = colour_image + 1;
    material.metallic_roughness_image = geometry::encode_optional_image(-1);
    material.occlusion_image = geometry::encode_optional_image(-1);
    material.emissive_image = geometry::encode_optional_image(-1);
    material.emissive = m.emissive;
    material.normal_scale = 1.0f;
    material.alpha = geometry::encode_alpha_word(m.alpha_mode, m.double_sided, m.alpha_cutoff);
    out.materials.push_back(material);
    out.material_sampling.push_back(
        geometry::encode_material_sampling(m.sampling, m.occlusion_strength));
    out.impostors.push_back(geometry::ClusterFileImpostor{
        static_cast<u32>(level.kind), level.merge, out.materials.size() - 1, level.groups,
        level.tile_px, level.columns, level.rows, level.error, level.parallax_m, level.texel_m,
        level_coverage, difference});
  }
  texture::fill_cluster_texture_records(out);
  return true;
}

}  // namespace

DetailTier flora_detail_tier(const FloraFile& file) {
  // The trees carry no field (it would move every tree's canonical parameters, and with them its
  // pinned source hash and cache key, for a value that cannot differ): a tree collides by its
  // trunk, so it is essential. The understory's species say theirs.
  if (file.species == k_species_shrub) return file.shrub.detail_tier;
  if (file.species == k_species_fern) return file.fern.detail_tier;
  if (file.species == k_species_tuft) return file.tuft.detail_tier;
  if (file.species == k_species_herb) return file.herb.detail_tier;
  return DetailTier::essential;
}

bool flora_collides(const FloraFile& file) {
  if (file.species == k_species_shrub) return shrub_collides(file.shrub);
  return !(file.species == k_species_fern || file.species == k_species_tuft ||
           file.species == k_species_herb);
}

bool build_flora_container(const FloraFile& file, const geometry::ClusterLodOptions& lod,
                           u32 page_bytes, geometry::ClusterFileData& out, PalmReport* report,
                           std::string& error, const materials::Pack* pack,
                           TreeReport* tree_report) {
  out = geometry::ClusterFileData{};
  assets::MeshData mesh;
  Motion motion;
  TreeLevels chain;
  if (!solve_flora(file, mesh, motion, report, tree_report, error, &chain)) return false;
  // The impostor chain (impostor.h, flora.md "The impostor chain"): the leaves' primitive takes the
  // levels above its finest, every other primitive hands over to the whole-tree impostor where it
  // stands, and every level's view is baked from the finest geometry.
  const bool has_chain = !chain.levels.empty();
  const bool has_tree = has_chain && chain.levels.back().kind == ImpostorKind::tree;
  const f32 tree_error = has_tree ? chain.levels.back().error : 0.0f;
  Vector<geometry::ClusterLodLevel> leaf_levels;
  for (const TreeLevel& level : chain.levels)
    leaf_levels.push_back(geometry::ClusterLodLevel{level.indices, level.error});
  const geometry::ClusterLodLevel handover[] = {{{}, tree_error}};
  // One DAG per primitive, each with its material's cone setting: the trunk is one-sided and keeps
  // its cones, a frond is seen from both faces and has none (flora.md, "Two-sided crowns").
  const geometry::AttributeSource attributes = assets::attribute_source(mesh);
  Vector<geometry::ClusterLodMesh> parts;
  Vector<u32> part_material;
  Vector<u32> part_first_authored;  // the part's first DAG level that is an authored one
  u32 leaf_part = ~0u;              // the part the chain's leaves are
  for (const assets::Primitive& primitive : mesh.primitives) {
    geometry::ClusterLodOptions options = lod;
    options.normal_cones = !mesh.materials[static_cast<u32>(primitive.material)].double_sided;
    const std::span<const u32> range(mesh.indices.data() + primitive.first_index,
                                     primitive.index_count);
    const bool leaves = has_chain && primitive.first_index == chain.leaf_first_index;
    std::span<const geometry::ClusterLodLevel> levels;
    if (leaves) {
      levels = std::span<const geometry::ClusterLodLevel>(leaf_levels.data(), leaf_levels.size());
    } else if (has_tree) {
      levels = handover;
    }
    geometry::ClusterLodMesh part;
    std::string part_error;
    // The leaves with the chain nest (R91, "The impostor chain"): every leaf in its sub-tree's
    // card, every card in the next level's, the last in the star, so each card switches on its own
    // sub-tree's sphere and the far side of a crown is cards while the near side is leaves.
    const bool nested = leaves && chain.leaf_element.size() * 3u == primitive.index_count;
    if (leaves) leaf_part = parts.size();
    bool built = false;
    if (nested) {
      Vector<geometry::ClusterLodHierarchyLevel> nest;
      for (const TreeLevel& level : chain.levels)
        nest.push_back(geometry::ClusterLodHierarchyLevel{level.indices, level.element_of_triangle,
                                                          level.parent_of_element, level.elements,
                                                          level.error});
      built = geometry::build_cluster_lod_hierarchy(mesh.positions, range, chain.leaf_element, nest,
                                                    options, part, &part_error, attributes);
    } else {
      built = geometry::build_cluster_lod_levels(mesh.positions, range, levels, options, part,
                                                 &part_error, attributes);
    }
    if (!built) {
      error = "the cluster build refused the tree's " +
              mesh.materials[static_cast<u32>(primitive.material)].name + ": " + part_error;
      return false;
    }
    part_first_authored.push_back(
        leaves ? part.level_cluster_counts.size() - static_cast<u32>(leaf_levels.size()) : ~0u);
    parts.push_back(std::move(part));
    part_material.push_back(static_cast<u32>(primitive.material));
  }
  if (has_tree) {
    // The one transition the parts share is the star's: the bark's hand-over and the impostor's own
    // switch carry one sphere, so they are the same test (every card below it keeps its own).
    const f32 from = tree_error;
    Vec4 shared{};
    for (u32 p = 0; p < parts.size(); ++p)
      shared = geometry::authored_lod_sphere(parts[p], from, shared);
    for (u32 p = 0; p < parts.size(); ++p) {
      std::string widen_error;
      if (!geometry::widen_lod_spheres(parts[p], from, shared, &widen_error)) {
        error = "the tree's impostor chain: " + widen_error;
        return false;
      }
    }
  }
  Vector<u32> part_of_cluster;
  std::string merge_error;
  if (!geometry::merge_cluster_lod(parts, out.mesh, &part_of_cluster, &merge_error)) {
    error = "the tree's parts did not merge: " + merge_error;
    return false;
  }
  // An authored level's clusters draw with that level's material, which follows the species' own.
  if (has_chain) {
    for (u32 c = 0; c < part_of_cluster.size(); ++c) {
      const u32 part = part_of_cluster[c];
      const u32 level = out.mesh.lod[c].level;
      if (part_first_authored[part] != ~0u && level >= part_first_authored[part])
        part_of_cluster[c] = parts.size() + level - part_first_authored[part];
    }
    for (u32 k = 0; k < chain.levels.size(); ++k)
      part_material.push_back(mesh.materials.size() + k);
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
  Vector<f32> coverage;
  if (has_chain && !bake_chain(mesh, chain, out, coverage, error)) return false;
  // Section 39 (geometry.md, "Two errors"): each cluster's own and parent coverage error. An
  // authored level's are its own and the next level's; the leaves' parent is the first level's; the
  // bark's transitions all take the star's, which its hand-over meets, so both errors rise along
  // every path of every part.
  if (has_chain && coverage.size() == chain.levels.size()) {
    const f32 none = 1.0e30f;
    out.lod_coverage.reserve(part_of_cluster.size());
    for (u32 c = 0; c < part_of_cluster.size(); ++c) {
      const u32 q = part_of_cluster[c];
      const geometry::ClusterLodDesc& record = out.mesh.lod[c];
      f32 own = none, parent = none;
      if (q >= parts.size()) {
        const u32 k = q - parts.size();
        own = coverage[k];
        parent = k + 1 < coverage.size() ? coverage[k + 1] : none;
      } else if (q == leaf_part) {
        own = record.own_error == 0.0f ? 0.0f : coverage.front();
        parent = coverage.front();
      } else if (has_tree) {
        own = coverage.back();
        parent = coverage.back();
      }
      out.lod_coverage.push_back(geometry::pack_lod_coverage(own, parent));
    }
  }
  // After the bake, which cuts a leaf to its outline only where a pack's image does. With the
  // chain, the finest leaves are alpha-tested even with no pack's image to cut them (a mask
  // of the factor alone keeps every pixel): the cross-fade into the first cards is a screen door
  // through the masked variants' discard on both sides (renderer.md, "The cross-fade"), and an
  // opaque side could only switch whole.
  if (has_chain && out.materials.size() > 1 &&
      geometry::alpha_word_mode(out.materials[1].alpha) == assets::k_alpha_opaque) {
    out.materials[1].alpha = geometry::encode_alpha_word(
        assets::k_alpha_mask, geometry::alpha_word_double_sided(out.materials[1].alpha), 0.5f);
  }
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
