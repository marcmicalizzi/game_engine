// The recipe file and the mesh generator `wall` (docs/subsystems/walls.md, "The module"): a
// module's container is its finest form decimated as every mesh is, cut where the error reaches
// the mid form's, then the mid form (the courses as bands, the cope as blocks) and the outline
// as authored levels over the same positions (geometry.md, "Authored levels").

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/texture/material_textures.h>
#include <domain/walls/walls.h>

namespace engine::walls {

bool read_file(std::string_view text, Module& out, std::string& error) {
  JsonValue value;
  const auto parsed = parse_json(text, value);
  if (!parsed.ok) {
    error = "wall recipe: " + std::string(parsed.message);
    return false;
  }
  schema::ReadContext ctx;
  if (!schema::from_json(out, value, ctx) || !ctx.ok()) {
    error = ctx.diagnostics.empty()
                ? std::string("wall recipe: invalid engine.walls.Module")
                : "wall recipe: " + ctx.diagnostics[0].path + ": " + ctx.diagnostics[0].message;
    return false;
  }
  std::string why;
  if (!module_in_range(out, why)) {
    error = "wall recipe: " + why;
    return false;
  }
  return true;
}

std::string canonical_json(const Module& module) {
  return write_json(schema::to_json(module), {.pretty = false});
}

u64 source_hash(const Module& module) {
  const std::string text = canonical_json(module);
  return hash_combine(hash_bytes(text.data(), text.size()), k_wall_version);
}

bool build_container(const Module& module, const assets::MeshGeneratorInputs& inputs,
                     const geometry::ClusterLodOptions& lod, u32 page_bytes,
                     geometry::ClusterFileData& out, ModuleReport& report, std::string& error) {
  out = {};
  assets::MeshData mesh;
  if (!solve_module(module, mesh, report, error)) return false;
  geometry::ClusterFileData result;
  geometry::ClusterLodOptions options = lod;
  options.normal_cones = true;  // every stone, the core and the forms are closed and one-sided
  const geometry::ClusterLodLevel levels[2] = {{report.mid_indices, k_mid_error_m},
                                               {report.far_indices, k_far_error_m}};
  const std::span<const geometry::ClusterLodLevel> authored =
      module.kind == ModuleKind::strip ? std::span<const geometry::ClusterLodLevel>{}
                                       : std::span<const geometry::ClusterLodLevel>{levels, 2};
  std::string build_error;
  if (!geometry::build_cluster_lod_levels(mesh.positions, mesh.indices, authored, options,
                                          result.mesh, &build_error,
                                          assets::attribute_source(mesh))) {
    error = "the cluster build refused the wall module: " + build_error;
    return false;
  }
  if (page_bytes > 0) {
    geometry::ClusterPagesOptions paging;
    paging.page_bytes = page_bytes;
    if (!geometry::build_cluster_pages(result.mesh, paging, result.pages, &build_error)) {
      error = "the wall module's pages were not laid out: " + build_error;
      return false;
    }
  }
  result.cluster_material.assign(result.mesh.mesh.clusters.size(), 0u);
  const assets::Material& m = mesh.materials[0];
  geometry::ClusterFileMaterial material;
  material.base_color = m.base_color;
  material.metallic = m.metallic;
  material.roughness = m.roughness;
  material.base_color_image = 0;
  material.alpha = geometry::encode_alpha_word(assets::k_alpha_opaque, false, 0.5f);
  result.materials.push_back(material);
  result.material_sampling.push_back(geometry::encode_material_sampling(m.sampling, 1.0f));
  for (assets::ImageRef& image : mesh.images) {
    result.image_paths.push_back(image.uri);
    result.images.push_back(geometry::ClusterImage{image.mime_type, std::move(image.bytes)});
  }
  texture::fill_cluster_texture_records(result);
  result.source_hash = source_hash(module);
  result.build_key = assets::generated_mesh_build_key(result.source_hash, inputs, lod, page_bytes);
  result.generator = "wall";
  result.generator_kind = "wall";
  result.generator_params = canonical_json(module);
  out = std::move(result);
  return true;
}

namespace {
std::string_view text_of(std::span<const u8> bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
bool hash_recipe(std::span<const u8> bytes, u64& out, std::string& error) {
  Module module;
  if (!read_file(text_of(bytes), module, error)) return false;
  out = source_hash(module);
  return true;
}
bool build_recipe(std::span<const u8> bytes, const assets::MeshGeneratorInputs& inputs,
                  const geometry::ClusterLodOptions& lod, u32 page_bytes,
                  geometry::ClusterFileData& out, std::string& error) {
  Module module;
  ModuleReport report;
  return read_file(text_of(bytes), module, error) &&
         build_container(module, inputs, lod, page_bytes, out, report, error);
}
constexpr assets::MeshGeneratorDesc k_generator{
    .name = "wall", .source_hash = hash_recipe, .build = build_recipe};
const assets::MeshGeneratorRegistrar k_registrar{k_generator};
}  // namespace

}  // namespace engine::walls
