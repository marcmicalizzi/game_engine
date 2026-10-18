// The flora file, its key, and the container the solver's mesh is built into (flora.h).
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/assets/mesh_generator.h>
#include <domain/flora/flora.h>
#include <domain/flora/palm.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/geometry/material_sampling.h>

#include <string>

namespace engine::flora {

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
  if (out.species != k_species_date_palm) {
    error = "species \"" + out.species + "\" is not one the solver has (it has \"" +
            k_species_date_palm + "\")";
    return false;
  }
  return palm_in_range(out.palm, error);
}

std::string canonical_flora_json(const FloraFile& file) {
  JsonWriteOptions compact;
  compact.pretty = false;
  return write_json(schema::to_json(file), compact);
}

u64 flora_source_hash(const FloraFile& file) {
  const std::string canonical = canonical_flora_json(file);
  u64 h = hash_bytes(canonical.data(), canonical.size());
  h = hash_combine(h, hash_bytes(file.species.data(), file.species.size()));
  return hash_combine(h, k_flora_version);
}

u64 flora_build_key(u64 source_hash, const geometry::ClusterLodOptions& lod, u32 page_bytes) {
  // No weld: the solver's vertices are shared where they are meant to be and nowhere else.
  return geometry::cluster_cache_key(source_hash, lod, false, page_bytes);
}

bool build_flora_container(const FloraFile& file, const geometry::ClusterLodOptions& lod,
                           u32 page_bytes, geometry::ClusterFileData& out, PalmReport* report,
                           std::string& error) {
  out = geometry::ClusterFileData{};
  assets::MeshData mesh;
  if (!solve_palm(file.palm, mesh, report, &error)) return false;

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
      error = "the cluster build refused the palm's " +
              mesh.materials[static_cast<u32>(primitive.material)].name + ": " + part_error;
      return false;
    }
    parts.push_back(std::move(part));
    part_material.push_back(static_cast<u32>(primitive.material));
  }
  Vector<u32> part_of_cluster;
  std::string merge_error;
  if (!geometry::merge_cluster_lod(parts, out.mesh, &part_of_cluster, &merge_error)) {
    error = "the palm's parts did not merge: " + merge_error;
    return false;
  }
  if (page_bytes > 0) {
    Vector<u32> source_of_cluster;
    geometry::ClusterPagesOptions page_options;
    page_options.page_bytes = page_bytes;
    std::string page_error;
    if (!geometry::build_cluster_pages(out.mesh, page_options, out.pages, &page_error,
                                       &source_of_cluster)) {
      error = "the palm's pages were not laid out: " + page_error;
      return false;
    }
    geometry::permute_cluster_array(source_of_cluster, part_of_cluster);
  }
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
  out.source_hash = flora_source_hash(file);
  out.build_key = flora_build_key(out.source_hash, lod, page_bytes);
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

bool generator_build(std::span<const u8> bytes, const geometry::ClusterLodOptions& lod,
                     u32 page_bytes, geometry::ClusterFileData& out, std::string& error) {
  FloraFile file;
  if (!read_flora_file(text_of(bytes), file, error)) return false;
  return build_flora_container(file, lod, page_bytes, out, nullptr, error);
}

constexpr assets::MeshGeneratorDesc k_flora_generator{
    .name = "flora",
    .source_hash = generator_source_hash,
    .build = generator_build,
};
const assets::MeshGeneratorRegistrar k_flora_registrar{k_flora_generator};

}  // namespace

}  // namespace engine::flora
