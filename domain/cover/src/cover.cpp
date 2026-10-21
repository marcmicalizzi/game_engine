// The cover capability's recipes, their containers and the two mesh generators (cover.h).
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/cover/cover.h>
#include <domain/flora/motion.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/geometry/material_sampling.h>
#include <domain/materials/materials.h>
#include <domain/texture/material_textures.h>

#include <cmath>
#include <span>

namespace engine::cover {

namespace {

template <typename T>
bool read_schema_file(std::string_view text, T& out, const char* type, std::string& error) {
  out = T{};
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
    error = text_errors.empty() ? std::string("not an ") + type : text_errors;
    return false;
  }
  return true;
}

bool site_in_range(const HostSite& s, std::string& error) {
  const auto bad = [&](const char* field) {
    error = std::string("site.") + field + " is out of range (cover.md, \"The exposure terms\")";
    return false;
  };
  const auto in = [](f32 v, f32 lo, f32 hi) { return v >= lo && v <= hi; };
  if (!in(s.yaw_deg, -360.0f, 360.0f)) return bad("yaw_deg");
  if (!in(s.light_azimuth_deg, 0.0f, 360.0f)) return bad("light_azimuth_deg");
  if (!in(s.light_elevation_deg, 1.0f, 90.0f)) return bad("light_elevation_deg");
  if (!in(s.ground_y_m, -1000.0f, 1000.0f)) return bad("ground_y_m");
  if (!in(s.water_distance_m, 0.0f, 100000.0f)) return bad("water_distance_m");
  if (!in(s.canopy_shade, 0.0f, 1.0f)) return bad("canopy_shade");
  if (!in(s.wetness, 0.0f, 1.0f)) return bad("wetness");
  return true;
}

std::string compact(const JsonValue& value) {
  JsonWriteOptions options;
  options.pretty = false;
  return write_json(value, options);
}

u64 source_hash_of(const std::string& canonical, const char* generator, u64 host_identity) {
  u64 h = hash_bytes(canonical.data(), canonical.size());
  h = hash_combine(h, hash_bytes(generator, std::string_view(generator).size()));
  h = hash_combine(h, k_cover_version);
  return hash_combine(h, host_identity);
}

}  // namespace

bool read_vine_file(std::string_view text, VineFile& out, std::string& error) {
  if (!read_schema_file(text, out, "engine.cover.VineFile", error)) return false;
  if (out.species != k_species_ivy) {
    error = "species \"" + out.species + "\" is not a climbing strategy the solver has (\"ivy\")";
    return false;
  }
  if (out.host.empty()) {
    error = "host names no mesh: a vine grows over one";
    return false;
  }
  return site_in_range(out.site, error) && vine_in_range(out.vine, error);
}

bool read_cover_file(std::string_view text, CoverFile& out, std::string& error) {
  if (!read_schema_file(text, out, "engine.cover.CoverFile", error)) return false;
  if (out.host.empty()) {
    error = "host names no mesh: a cover grows over one";
    return false;
  }
  return site_in_range(out.site, error) && cover_in_range(out.cover, error);
}

std::string canonical_vine_json(const VineFile& file) { return compact(schema::to_json(file)); }
std::string canonical_cover_json(const CoverFile& file) { return compact(schema::to_json(file)); }

u64 vine_source_hash(const VineFile& file, u64 host_identity) {
  return source_hash_of(canonical_vine_json(file), k_vine_generator, host_identity);
}
u64 cover_source_hash(const CoverFile& file, u64 host_identity) {
  return source_hash_of(canonical_cover_json(file), k_cover_generator, host_identity);
}

bool prepare_host(const assets::GeneratorHostMesh& mesh, Host& out, std::string& error) {
  if (!make_host_surface(mesh, out.surface, error)) return false;
  out.bvh.build(out.surface);
  return true;
}

bool build_companion_container(const assets::MeshData& mesh, flora::Motion& motion,
                               std::span<const char* const> roles,
                               const geometry::ClusterLodOptions& lod, u32 page_bytes,
                               const materials::Pack* pack, geometry::ClusterFileData& out,
                               std::string& error) {
  out = geometry::ClusterFileData{};
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
      error = "the cluster build refused the " +
              mesh.materials[static_cast<u32>(primitive.material)].name + ": " + part_error;
      return false;
    }
    parts.push_back(std::move(part));
    part_material.push_back(static_cast<u32>(primitive.material));
  }
  Vector<u32> part_of_cluster;
  std::string merge_error;
  if (!geometry::merge_cluster_lod(parts, out.mesh, &part_of_cluster, &merge_error)) {
    error = "the companion's parts did not merge: " + merge_error;
    return false;
  }
  if (page_bytes > 0) {
    Vector<u32> source_of_cluster;
    geometry::ClusterPagesOptions page_options;
    page_options.page_bytes = page_bytes;
    std::string page_error;
    if (!geometry::build_cluster_pages(out.mesh, page_options, out.pages, &page_error,
                                       &source_of_cluster)) {
      error = "the companion's pages were not laid out: " + page_error;
      return false;
    }
    geometry::permute_cluster_array(source_of_cluster, part_of_cluster);
  }
  if (!flora::motion_records(motion, out.mesh, out.motion_vertices)) {
    error = "the companion's motion attributes do not cover its vertices";
    return false;
  }
  out.motion_parts = std::move(motion.parts);
  out.structural_parts = std::move(motion.structure);
  out.cluster_material.reserve(part_of_cluster.size());
  for (const u32 part : part_of_cluster)
    out.cluster_material.push_back(part_material[part]);
  for (u32 i = 0; i < mesh.materials.size(); ++i) {
    const assets::Material& m = mesh.materials[i];
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
  // The pack's sets by role: the stems and the films in metres, a leaf or a cap's card whole.
  for (u32 i = 0; i < out.materials.size() && i < roles.size(); ++i) {
    const materials::MaterialSet none;
    if (const materials::MaterialSet* chosen = materials::resolve_set(&none, pack, roles[i])) {
      const std::string_view role = roles[i];
      const bool card = role == "vine/leaf";
      materials::apply_set(*chosen, card ? materials::UvLayout::card : materials::UvLayout::metres,
                           out, i);
    }
  }
  if (!out.image_paths.empty()) texture::fill_cluster_texture_records(out);
  return true;
}

bool build_vine_container(const VineFile& file, const Host& host, u64 host_identity,
                          const geometry::ClusterLodOptions& lod, u32 page_bytes,
                          const materials::Pack* pack, geometry::ClusterFileData& out,
                          VineReport* report, std::string& error) {
  assets::MeshData mesh;
  flora::Motion motion;
  if (!solve_vine(host.surface, host.bvh, file.site, file.vine, mesh, motion, report, &error))
    return false;
  if (!build_companion_container(mesh, motion, std::span<const char* const>(k_vine_roles, 2), lod,
                                 page_bytes, pack, out, error))
    return false;
  out.source_hash = vine_source_hash(file, host_identity);
  out.build_key = assets::generated_mesh_build_key(
      out.source_hash, assets::MeshGeneratorInputs{pack}, lod, page_bytes);
  out.generator = k_vine_generator;
  out.generator_kind = file.species;
  out.generator_params = canonical_vine_json(file);
  return true;
}

bool build_cover_container(const CoverFile& file, const Host& host, u64 host_identity,
                           const geometry::ClusterLodOptions& lod, u32 page_bytes,
                           const materials::Pack* pack, geometry::ClusterFileData& out,
                           CoverReport* report, std::string& error) {
  assets::MeshData mesh;
  flora::Motion motion;
  Vector<u32> mask;
  if (!solve_cover(host.surface, host.bvh, file.site, file.cover, mesh, motion, &mask, report,
                   &error))
    return false;
  // The roles of the materials the cover emitted, in its order (moss, lichen, algae, fungus).
  if (!build_companion_container(mesh, motion,
                                 std::span<const char* const>(k_cover_roles, k_cover_kinds), lod,
                                 page_bytes, pack, out, error))
    return false;
  out.cover_mask = std::move(mask);
  out.source_hash = cover_source_hash(file, host_identity);
  out.build_key = assets::generated_mesh_build_key(
      out.source_hash, assets::MeshGeneratorInputs{pack}, lod, page_bytes);
  out.generator = k_cover_generator;
  out.generator_kind = "cover";
  out.generator_params = canonical_cover_json(file);
  return true;
}

namespace {

// ---- the mesh generators "vine" and "cover" (assets/mesh_generator.h) --------------------------

std::string_view text_of(std::span<const u8> bytes) {
  return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool needs_host(std::span<const u8>, u64&, std::string& error) {
  error =
      "a companion's key covers its host, and this host offers no host reader "
      "(assets::MeshGeneratorInputs::hosts)";
  return false;
}

template <typename File>
bool host_identity(const File& file, const assets::MeshGeneratorInputs& inputs, u64& out,
                   std::string& error) {
  if (inputs.hosts == nullptr || inputs.hosts->identify == nullptr) {
    error = "this host offers no host reader for " + file.host;
    return false;
  }
  return inputs.hosts->identify(inputs.hosts->context, file.host, out, error);
}

template <typename File>
bool load_host(const File& file, const assets::MeshGeneratorInputs& inputs, Host& host,
               u64& identity, std::string& error) {
  if (inputs.hosts == nullptr || inputs.hosts->load == nullptr) {
    error = "this host offers no host reader for " + file.host;
    return false;
  }
  assets::GeneratorHostMesh mesh;
  if (!inputs.hosts->load(inputs.hosts->context, file.host, mesh, error)) return false;
  identity = mesh.identity;
  return prepare_host(mesh, host, error);
}

bool vine_hash(std::span<const u8> bytes, const assets::MeshGeneratorInputs& inputs, u64& out,
               std::string& error) {
  VineFile file;
  if (!read_vine_file(text_of(bytes), file, error)) return false;
  u64 identity = 0;
  if (!host_identity(file, inputs, identity, error)) return false;
  out = vine_source_hash(file, identity);
  return true;
}

bool vine_build(std::span<const u8> bytes, const assets::MeshGeneratorInputs& inputs,
                const geometry::ClusterLodOptions& lod, u32 page_bytes,
                geometry::ClusterFileData& out, std::string& error) {
  VineFile file;
  if (!read_vine_file(text_of(bytes), file, error)) return false;
  Host host;
  u64 identity = 0;
  if (!load_host(file, inputs, host, identity, error)) return false;
  return build_vine_container(file, host, identity, lod, page_bytes, inputs.materials, out, nullptr,
                              error);
}

bool cover_hash(std::span<const u8> bytes, const assets::MeshGeneratorInputs& inputs, u64& out,
                std::string& error) {
  CoverFile file;
  if (!read_cover_file(text_of(bytes), file, error)) return false;
  u64 identity = 0;
  if (!host_identity(file, inputs, identity, error)) return false;
  out = cover_source_hash(file, identity);
  return true;
}

bool cover_build(std::span<const u8> bytes, const assets::MeshGeneratorInputs& inputs,
                 const geometry::ClusterLodOptions& lod, u32 page_bytes,
                 geometry::ClusterFileData& out, std::string& error) {
  CoverFile file;
  if (!read_cover_file(text_of(bytes), file, error)) return false;
  Host host;
  u64 identity = 0;
  if (!load_host(file, inputs, host, identity, error)) return false;
  return build_cover_container(file, host, identity, lod, page_bytes, inputs.materials, out,
                               nullptr, error);
}

constexpr assets::MeshGeneratorDesc k_vine_desc{
    .name = k_vine_generator,
    .source_hash = needs_host,
    .build = vine_build,
    .source_hash_with = vine_hash,
};
constexpr assets::MeshGeneratorDesc k_cover_desc{
    .name = k_cover_generator,
    .source_hash = needs_host,
    .build = cover_build,
    .source_hash_with = cover_hash,
};
const assets::MeshGeneratorRegistrar k_vine_registrar{k_vine_desc};
const assets::MeshGeneratorRegistrar k_cover_registrar{k_cover_desc};

}  // namespace

}  // namespace engine::cover
