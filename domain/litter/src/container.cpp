#include <domain/geometry/cluster_pages.h>
#include <domain/litter/litter.h>
#include <domain/materials/materials.h>
#include <domain/texture/material_textures.h>

namespace engine::litter {
bool build_container(const Recipe& p, const assets::MeshGeneratorInputs& inputs,
                     const geometry::ClusterLodOptions& lod, u32 page_bytes,
                     geometry::ClusterFileData& out, Report& report, std::string& error) {
  out = {};
  assets::MeshData mesh;
  if (!solve(p, mesh, report, error)) return false;
  geometry::ClusterFileData result;
  auto options = lod;
  options.normal_cones = !mesh.materials[0].double_sided;
  // Decimation can discard subpixel needles individually and erase the bed's coverage.
  // Its terminal form retains every needle as one chord quad (maximum bend error .5 mm).
  Vector<u32> coarse;
  if (p.kind == Kind::needles) {
    const u32 stride = report.segments * 6;
    for (u32 i = 0; i < report.particles; ++i) {
      const u32 first = i * stride, last = first + stride - 6;
      for (const u32 v : {first, last + 1, last + 2, first, last + 2, first + 5})
        coarse.push_back(v);
    }
  }
  const geometry::ClusterLodLevel level{coarse, .0005f};
  const std::span<const geometry::ClusterLodLevel> levels =
      coarse.empty() ? std::span<const geometry::ClusterLodLevel>{}
                     : std::span<const geometry::ClusterLodLevel>{&level, 1};
  if (!geometry::build_cluster_lod_levels(mesh.positions, mesh.indices, levels, options,
                                          result.mesh, &error, assets::attribute_source(mesh)))
    return false;
  if (page_bytes) {
    geometry::ClusterPagesOptions paging;
    paging.page_bytes = page_bytes;
    if (!geometry::build_cluster_pages(result.mesh, paging, result.pages, &error)) return false;
  }
  result.cluster_material.resize(result.mesh.mesh.clusters.size(), 0);
  geometry::ClusterFileMaterial m;
  m.base_color = mesh.materials[0].base_color;
  m.metallic = 0;
  m.roughness = .9f;
  m.alpha =
      geometry::encode_alpha_word(assets::k_alpha_opaque, mesh.materials[0].double_sided, .5f);
  result.materials.push_back(m);
  constexpr const char* roles[] = {"litter/leaf", "litter/needles", "litter/cone",
                                   "litter/bark", "litter/wood",    "litter/wood"};
  if (const auto* set =
          materials::resolve_set(nullptr, inputs.materials, roles[static_cast<u32>(p.kind)]))
    materials::apply_set(*set, materials::UvLayout::repeats, result, 0);
  if (!result.image_paths.empty()) texture::fill_cluster_texture_records(result);
  // One rigid part, explicit structural eligibility even for part zero. The shared catalogue
  // suppresses runtime pose work and bounds shadow work; all stored bend weights are zero.
  result.motion_parts.push_back({{}, Vec3::unit_y(), 0, geometry::k_motion_no_parent});
  result.structural_parts.push_back({0, 0, 0, p.collider ? geometry::k_structural_collider : 0u});
  result.motion_vertices.resize(result.mesh.mesh.vertices.size(), {});
  result.source_hash = source_hash(p);
  result.build_key = assets::generated_mesh_build_key(result.source_hash, inputs, lod, page_bytes);
  result.generator = "litter";
  result.generator_kind = "litter";
  result.generator_params = canonical_json(p);
  out = std::move(result);
  return true;
}
namespace {
std::string_view text_of(std::span<const u8> b) {
  return {reinterpret_cast<const char*>(b.data()), b.size()};
}
bool hash_recipe(std::span<const u8> b, u64& out, std::string& error) {
  Recipe p;
  if (!read_file(text_of(b), p, error)) return false;
  out = source_hash(p);
  return true;
}
bool build_recipe(std::span<const u8> b, const assets::MeshGeneratorInputs& inputs,
                  const geometry::ClusterLodOptions& lod, u32 pages, geometry::ClusterFileData& out,
                  std::string& error) {
  Recipe p;
  Report report;
  return read_file(text_of(b), p, error) &&
         build_container(p, inputs, lod, pages, out, report, error);
}
constexpr assets::MeshGeneratorDesc k_generator{
    .name = "litter", .source_hash = hash_recipe, .build = build_recipe};
const assets::MeshGeneratorRegistrar k_registrar{k_generator};
}  // namespace
}  // namespace engine::litter
