#include "detail.h"

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/assets/gltf.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/geometry/material_sampling.h>
#include <domain/materials/materials.h>
#include <domain/soil/soil.h>
#include <domain/texture/material_textures.h>

#include <cmath>

namespace engine::soil {
u32 triangle_budget(const SoilFile& f) noexcept {
  if (f.triangle_budget) return f.triangle_budget;
  return f.kind == Kind::TreeThrow ? 24000u : f.kind == Kind::Scarp ? 12000u : 16000u;
}
bool in_range(const SoilFile& f, std::string& error) {
  const auto check = [&](bool ok, const char* field) {
    if (!ok)
      error = std::string("engine.soil.SoilFile.") + field + " is out of range (soil.schema)";
    return ok;
  };
  const auto within = [](f32 v, f32 lo, f32 hi) { return v >= lo && v <= hi; };
  if (!check(f.kind >= Kind::Scarp && f.kind <= Kind::TreeThrow, "kind") ||
      !check(f.triangle_budget == 0 || (f.triangle_budget >= 2000 && f.triangle_budget <= 200000),
             "triangle_budget") ||
      !check(within(f.width_m, 1, 32), "width_m") ||
      !check(within(f.length_m, 1, 16), "length_m") ||
      !check(within(f.depth_m, 0.2f, 3), "depth_m") ||
      !check(within(f.undercut_m, 0, 0.6f) &&
                 (f.kind != Kind::Undercut || f.undercut_m <= f.depth_m * 0.5f),
             "undercut_m") ||
      !check(within(f.plate_height_m, 0.5f, 6), "plate_height_m") ||
      !check(within(f.mound_height_m, 0, 1.5f), "mound_height_m") ||
      !check(within(f.litter_m, 0.01f, 0.2f), "litter_m") ||
      !check(within(f.topsoil_m, 0.05f, 0.6f) && f.topsoil_m + f.litter_m < f.depth_m,
             "topsoil_m") ||
      !check(f.roots <= 32, "roots") ||
      !check(within(f.root_radius_m, 0.005f, 0.15f), "root_radius_m") ||
      !check(f.stones <= 32, "stones") ||
      !check(within(f.stone_radius_m, 0.0625f, 0.25f), "stone_radius_m"))
    return false;
  const auto& g = f.ground;
  if (!check((g.side == 0 && g.heights.empty()) ||
                 (g.side >= 2 && g.side <= 513 &&
                  g.heights.size() == static_cast<usize>(g.side) * g.side),
             "ground.side/heights") ||
      !check(within(g.spacing_m, 0.025f, 1), "ground.spacing_m") ||
      !check(std::isfinite(g.min_x_m) && std::isfinite(g.min_z_m), "ground.origin"))
    return false;
  for (f32 h : g.heights)
    if (!check(within(h, -10000, 10000), "ground.heights")) return false;
  if (g.side) {
    const f32 r = detail::radius(f), end = static_cast<f32>(g.side - 1) * g.spacing_m;
    if (!check(g.min_x_m <= -r && g.min_z_m <= -r && g.min_x_m + end >= r && g.min_z_m + end >= r,
               "ground.coverage"))
      return false;
  }
  return true;
}
bool read_file(std::string_view text, SoilFile& out, std::string& error) {
  JsonValue json;
  const auto parsed = parse_json(text, json);
  if (!parsed.ok) {
    error = parsed.message;
    return false;
  }
  SoilFile next;
  schema::ReadContext ctx;
  if (!schema::from_json(next, json, ctx) || !ctx.ok()) {
    for (const auto& d : ctx.diagnostics)
      error += d.path + ": " + d.message + "; ";
    return false;
  }
  if (!in_range(next, error)) return false;
  out = std::move(next);
  return true;
}
std::string canonical_json(const SoilFile& f) {
  JsonWriteOptions opts;
  opts.pretty = false;
  return write_json(schema::to_json(f), opts);
}
u64 source_hash(const SoilFile& f) {
  const auto json = canonical_json(f);
  return hash_combine(hash_bytes(json.data(), json.size()), k_soil_version);
}
bool build_container(const SoilFile& f, const assets::MeshGeneratorInputs& inputs,
                     const geometry::ClusterLodOptions& lod, u32 page_bytes,
                     geometry::ClusterFileData& out, Report& report, std::string& error) {
  out = {};
  assets::MeshData mesh;
  if (!solve(f, mesh, report, error)) return false;
  Vector<geometry::ClusterLodMesh> parts;
  Vector<u32> part_material;
  const auto attributes = assets::attribute_source(mesh);
  for (const auto& primitive : mesh.primitives) {
    geometry::ClusterLodMesh part;
    auto opts = lod;
    opts.normal_cones = true;
    if (!geometry::build_cluster_lod(
            mesh.positions,
            std::span<const u32>(mesh.indices.data() + primitive.first_index,
                                 primitive.index_count),
            opts, part, &error, attributes))
      return false;
    parts.push_back(std::move(part));
    part_material.push_back(static_cast<u32>(primitive.material));
  }
  Vector<u32> owners;
  if (!geometry::merge_cluster_lod(parts, out.mesh, &owners, &error)) return false;
  if (page_bytes) {
    Vector<u32> permutation;
    geometry::ClusterPagesOptions opts;
    opts.page_bytes = page_bytes;
    if (!geometry::build_cluster_pages(out.mesh, opts, out.pages, &error, &permutation))
      return false;
    geometry::permute_cluster_array(permutation, owners);
  }
  for (u32 owner : owners)
    out.cluster_material.push_back(part_material[owner]);
  for (const auto& m : mesh.materials) {
    geometry::ClusterFileMaterial material;
    material.base_color = m.base_color;
    material.metallic = 0;
    material.roughness = m.roughness;
    out.materials.push_back(material);
    out.material_sampling.push_back(
        geometry::encode_material_sampling(m.sampling, m.occlusion_strength));
  }
  constexpr const char* k_roles[] = {"soil/litter", "soil/topsoil", "soil/subsoil",
                                     "soil/parent", "wood/root",    "rock/granite"};
  for (u32 i = 0; i < 6; ++i) {
    materials::MaterialSet none;
    if (const auto* set = materials::resolve_set(&none, inputs.materials, k_roles[i]))
      materials::apply_set(*set, materials::UvLayout::metres, out, i);
  }
  if (!out.image_paths.empty()) texture::fill_cluster_texture_records(out);
  out.source_hash = source_hash(f);
  out.build_key = assets::generated_mesh_build_key(out.source_hash, inputs, lod, page_bytes);
  out.generator = "soil";
  out.generator_kind = "static landform";
  out.generator_params = canonical_json(f);
  return true;
}
namespace {
std::string_view text_of(std::span<const u8> b) {
  return {reinterpret_cast<const char*>(b.data()), b.size()};
}
bool gen_hash(std::span<const u8> b, u64& out, std::string& error) {
  SoilFile f;
  if (!read_file(text_of(b), f, error)) return false;
  out = source_hash(f);
  return true;
}
bool gen_build(std::span<const u8> b, const assets::MeshGeneratorInputs& inputs,
               const geometry::ClusterLodOptions& lod, u32 page_bytes,
               geometry::ClusterFileData& out, std::string& error) {
  SoilFile f;
  Report report;
  if (!read_file(text_of(b), f, error)) return false;
  return build_container(f, inputs, lod, page_bytes, out, report, error);
}
constexpr assets::MeshGeneratorDesc k_generator{
    .name = "soil", .source_hash = gen_hash, .build = gen_build};
const assets::MeshGeneratorRegistrar k_registrar{k_generator};
}  // namespace
}  // namespace engine::soil
