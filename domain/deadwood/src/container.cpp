#include <domain/deadwood/deadwood.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/geometry/material_sampling.h>
#include <domain/materials/materials.h>
#include <domain/texture/material_textures.h>

namespace engine::deadwood {
bool build_container(const DeadwoodFile& file, const assets::MeshGeneratorInputs& inputs,
                     const geometry::ClusterLodOptions& lod, u32 page_bytes,
                     geometry::ClusterFileData& out, Report& report, std::string& error) {
  out = {};
  assets::MeshData mesh;
  flora::Motion motion;
  if (!solve(file, mesh, motion, report, error)) return false;
  geometry::ClusterFileData result;
  const auto attributes = assets::attribute_source(mesh);
  Vector<geometry::ClusterLodMesh> parts;
  Vector<u32> materials;
  for (const auto& primitive : mesh.primitives) {
    auto options = lod;
    options.normal_cones = !mesh.materials[static_cast<u32>(primitive.material)].double_sided;
    geometry::ClusterLodMesh part;
    if (!geometry::build_cluster_lod(
            mesh.positions, {mesh.indices.data() + primitive.first_index, primitive.index_count},
            options, part, &error, attributes))
      return false;
    parts.push_back(std::move(part));
    materials.push_back(static_cast<u32>(primitive.material));
  }
  Vector<u32> material_of_cluster;
  if (!geometry::merge_cluster_lod(parts, result.mesh, &material_of_cluster, &error)) return false;
  if (page_bytes > 0) {
    geometry::ClusterPagesOptions options;
    options.page_bytes = page_bytes;
    Vector<u32> order;
    if (!geometry::build_cluster_pages(result.mesh, options, result.pages, &error, &order))
      return false;
    geometry::permute_cluster_array(order, material_of_cluster);
  }
  for (const u32 part : material_of_cluster)
    result.cluster_material.push_back(materials[part]);
  if (!flora::motion_records(motion, result.mesh, result.motion_vertices)) {
    error = "deadwood motion records do not cover cluster vertices";
    return false;
  }
  result.motion_parts = std::move(motion.parts);
  result.structural_parts = std::move(motion.structure);
  for (const auto& m : mesh.materials) {
    geometry::ClusterFileMaterial material;
    material.base_color = m.base_color;
    material.metallic = m.metallic;
    material.roughness = m.roughness;
    material.normal_scale = m.normal_scale;
    material.alpha = geometry::encode_alpha_word(m.alpha_mode, m.double_sided, m.alpha_cutoff);
    result.materials.push_back(material);
    result.material_sampling.push_back(
        geometry::encode_material_sampling(m.sampling, m.occlusion_strength));
  }
  // Source roles and explicit sets are reused verbatim. The final material is the previously
  // unused wood slot: fractures, exposed bark patches and the hollow's inward-facing wall.
  flora::apply_flora_materials(file.tree, inputs.materials, result);
  materials::MaterialSet wood;
  if (file.tree.materials && file.tree.materials->wood) {
    const auto& w = *file.tree.materials->wood;
    wood.base_color = w.base_color;
    wood.normal = w.normal;
    wood.roughness = w.roughness;
    wood.metallic = w.metallic;
    wood.height = w.height;
    wood.tile_m = w.tile_m;
    wood.normal_scale = w.normal_scale;
  }
  const std::string role = file.tree.species + "/wood";
  if (const auto* set = materials::resolve_set(&wood, inputs.materials, role))
    materials::apply_set(*set, materials::UvLayout::metres, result, result.materials.size() - 1);
  if (!result.image_paths.empty()) texture::fill_cluster_texture_records(result);
  result.source_hash = source_hash(file);
  result.build_key = assets::generated_mesh_build_key(result.source_hash, inputs, lod, page_bytes);
  result.generator = "deadwood";
  // Catalogue species selects the existing collision policy. Both parts are parentless and
  // all weights zero; runtime poses cannot move any retained wood or foliage.
  result.generator_kind = file.tree.species;
  result.generator_params = canonical_json(file);
  out = std::move(result);
  return true;
}
namespace {
std::string_view text_of(std::span<const u8> bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
bool hash_recipe(std::span<const u8> bytes, u64& out, std::string& error) {
  DeadwoodFile file;
  if (!read_file(text_of(bytes), file, error)) return false;
  out = source_hash(file);
  return true;
}
bool build_recipe(std::span<const u8> bytes, const assets::MeshGeneratorInputs& inputs,
                  const geometry::ClusterLodOptions& lod, u32 page_bytes,
                  geometry::ClusterFileData& out, std::string& error) {
  DeadwoodFile file;
  Report report;
  return read_file(text_of(bytes), file, error) &&
         build_container(file, inputs, lod, page_bytes, out, report, error);
}
constexpr assets::MeshGeneratorDesc k_generator{
    .name = "deadwood", .source_hash = hash_recipe, .build = build_recipe};
const assets::MeshGeneratorRegistrar k_registrar{k_generator};
}  // namespace
}  // namespace engine::deadwood
