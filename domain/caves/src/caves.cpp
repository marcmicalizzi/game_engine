// The passage recipe, its key, the container the solve is built into (caves.h), and the mesh
// generator "cave" that makes a scene's `.cave.json` a passage.
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/assets/mesh_generator.h>
#include <domain/caves/caves.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/geometry/material_sampling.h>
#include <domain/materials/materials.h>
#include <domain/texture/material_textures.h>

#include <string>

namespace engine::caves {

namespace {

// A sentence naming the field and its range; written so that a NaN fails every test.
struct Check {
  std::string& error;
  const char* type;
  bool bad(const char* field, const char* range) {
    error = std::string("engine.caves.") + type + "." + field + " is out of range (" + range + ")";
    return false;
  }
  bool f(f32 v, f32 lo, f32 hi, const char* field, const char* range) {
    return (v >= lo && v <= hi) ? true : bad(field, range);
  }
  bool u(u32 v, u32 lo, u32 hi, const char* field, const char* range) {
    return (v >= lo && v <= hi) ? true : bad(field, range);
  }
};

}  // namespace

bool passage_in_range(const Passage& p, std::string& error) {
  Check c{error, "Passage"};
  if (p.format != k_passage_format) {
    error = "engine.caves.Passage.format is \"" + p.format + "\", not \"" +
            std::string(k_passage_format) + "\"";
    return false;
  }
  if (p.version != 1) return c.bad("version", "1");
  if (!c.f(p.entrance_radius_m, 0.5f, 20.0f, "entrance_radius_m", "0.5 to 20 m") ||
      !c.f(p.shaft_m, 0.0f, 50.0f, "shaft_m", "0 to 50 m") ||
      !c.f(p.roughness, 0.0f, 0.15f, "roughness", "0 to 0.15") ||
      !c.u(p.ring_vertices, 8, 64, "ring_vertices", "8 to 64") ||
      !c.f(p.ring_spacing_m, 0.1f, 2.0f, "ring_spacing_m", "0.1 to 2 m") ||
      !c.f(p.roughness_factor, 0.0f, 1.0f, "roughness_factor", "0 to 1"))
    return false;
  if (p.points.empty() || p.points.size() > 256) return c.bad("points", "1 to 256 points");
  for (const Vec3& q : p.points) {
    if (!c.f(q.x, -10000.0f, 10000.0f, "points", "within 10 km of the entrance") ||
        !c.f(q.y, -10000.0f, 10000.0f, "points", "within 10 km of the entrance") ||
        !c.f(q.z, -10000.0f, 10000.0f, "points", "within 10 km of the entrance"))
      return false;
  }
  if (!p.collar_heights_m.empty() &&
      (p.collar_heights_m.size() < 8 || p.collar_heights_m.size() > 256))
    return c.bad("collar_heights_m", "empty, or 8 to 256 samples round the rim");
  for (const f32 h : p.collar_heights_m) {
    if (!c.f(h, -100.0f, 100.0f, "collar_heights_m", "-100 to 100 m")) return false;
  }
  for (const f32 v : {p.base_color.x, p.base_color.y, p.base_color.z}) {
    if (!c.f(v, 0.0f, 1.0f, "base_color", "0 to 1 a channel")) return false;
  }
  Check s{error, "PassageSegment"};
  for (const PassageSegment& seg : p.segments) {
    if (static_cast<u8>(seg.profile) > static_cast<u8>(PassageProfile::Keyhole))
      return s.bad("profile", "Phreatic, Vadose or Keyhole");
    if (!s.f(seg.width_m, 0.8f, 12.0f, "width_m", "0.8 to 12 m")) return false;
    if (seg.height_m != 0.0f) {
      // The profile's own ratio range (docs/experiments/caves.md).
      const f32 ratio = seg.width_m / seg.height_m;
      if (seg.profile == PassageProfile::Phreatic &&
          !s.f(ratio, 1.2f, 3.5f, "height_m", "a tube 1.2 to 3.5 times as wide as high"))
        return false;
      if (seg.profile == PassageProfile::Vadose &&
          !s.f(1.0f / ratio, 1.5f, 8.0f, "height_m", "a canyon 1.5 to 8 times as tall as wide"))
        return false;
      if (seg.profile == PassageProfile::Keyhole &&
          !s.f(1.0f / ratio, 0.4f, 4.0f, "height_m", "a keyhole 0.4 to 4 times as tall as wide"))
        return false;
    }
  }
  if (p.speleothems.has_value()) {
    const Speleothems& sp = *p.speleothems;
    Check k{error, "Speleothems"};
    if (!k.f(sp.drip_per_m2, 0.0f, 5.0f, "drip_per_m2", "0 to 5") ||
        !k.f(sp.stalactite_min_m, 0.05f, 3.0f, "stalactite_min_m", "0.05 to 3 m") ||
        !k.f(sp.stalactite_max_m, sp.stalactite_min_m, 3.0f, "stalactite_max_m",
             "stalactite_min_m to 3 m") ||
        !k.f(sp.slenderness, 0.06f, 0.25f, "slenderness", "0.06 to 0.25") ||
        !k.f(sp.straw_radius_m, 0.002f, 0.0035f, "straw_radius_m", "2 to 3.5 mm") ||
        !k.f(sp.straw_share, 0.0f, 1.0f, "straw_share", "0 to 1") ||
        !k.f(sp.straw_max_m, 0.02f, 1.0f, "straw_max_m", "0.02 to 1 m") ||
        !k.f(sp.stalagmite_radius_m, 0.015f, 0.15f, "stalagmite_radius_m", "1.5 to 15 cm") ||
        !k.f(sp.stalagmite_max_m, 0.0f, 3.0f, "stalagmite_max_m", "0 to 3 m") ||
        !k.f(sp.column_gap_m, 0.0f, 0.5f, "column_gap_m", "0 to 0.5 m") ||
        !k.u(sp.sides, 4, 16, "sides", "4 to 16") ||
        !k.u(sp.triangle_budget, 0, 1000000, "triangle_budget", "0 to 1,000,000"))
      return false;
  }
  return true;
}

bool read_passage(std::string_view text, Passage& out, std::string& error) {
  out = Passage{};
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
    error = text_errors.empty() ? std::string("not an engine.caves.Passage") : text_errors;
    return false;
  }
  return passage_in_range(out, error);
}

u64 passage_source_hash(const Passage& passage) {
  JsonWriteOptions compact;
  compact.pretty = false;
  const std::string canonical = write_json(schema::to_json(passage), compact);
  return hash_combine(hash_bytes(canonical.data(), canonical.size()), k_caves_version);
}

bool build_passage_container(const Passage& passage, const geometry::ClusterLodOptions& lod,
                             u32 page_bytes, geometry::ClusterFileData& out, PassageReport* report,
                             std::string& error, const materials::Pack* pack) {
  out = geometry::ClusterFileData{};
  assets::MeshData mesh;
  if (!solve_passage(passage, mesh, report, &error)) return false;
  // One DAG, cones on: the shell is wound to face its axis and every speleothem outward, so the
  // front of every triangle is the side a camera in the passage sees (geometry.md, "Normal
  // cones"); a camera outside sees the shell's back and the cone test rightly drops it. The open
  // collar is locked, so every cut keeps the ring that meets the hole's rim.
  geometry::ClusterLodOptions options = lod;
  options.normal_cones = true;
  geometry::AttributeSource attributes = assets::attribute_source(mesh);
  Vector<u8> locked(mesh.positions.size(), u8{0});
  if (report != nullptr) {
    for (u32 v = 0; v < report->collar_count; ++v)
      locked[report->collar_first + v] = 1;
  } else {
    for (u32 v = 0; v <= passage.ring_vertices; ++v)
      locked[v] = 1;
  }
  attributes.locked = std::span<const u8>(locked.data(), locked.size());
  std::string build_error;
  if (!geometry::build_cluster_lod(mesh.positions,
                                   std::span<const u32>(mesh.indices.data(), mesh.indices.size()),
                                   options, out.mesh, &build_error, attributes)) {
    error = "the cluster build refused the passage: " + build_error;
    return false;
  }
  if (page_bytes > 0) {
    geometry::ClusterPagesOptions page_options;
    page_options.page_bytes = page_bytes;
    std::string page_error;
    if (!geometry::build_cluster_pages(out.mesh, page_options, out.pages, &page_error)) {
      error = "the passage's pages were not laid out: " + page_error;
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
  // The limestone from a pack's `rock/limestone` set when it has one: the UVs are metres round and
  // along the passage, so the set's repeat is its transform.
  const materials::MaterialSet* chosen =
      materials::resolve_set(nullptr, pack, k_limestone_role, nullptr);
  if (chosen != nullptr && !out.materials.empty())
    materials::apply_set(*chosen, materials::UvLayout::metres, out, 0);
  if (!out.image_paths.empty()) texture::fill_cluster_texture_records(out);
  out.source_hash = passage_source_hash(passage);
  out.build_key = assets::generated_mesh_build_key(
      out.source_hash, assets::MeshGeneratorInputs{pack}, lod, page_bytes);
  out.generator = "cave";
  out.generator_kind = "passage";
  JsonWriteOptions compact;
  compact.pretty = false;
  out.generator_params = write_json(schema::to_json(passage), compact);
  return true;
}

namespace {

// ---- the mesh generator "cave" (assets/mesh_generator.h): `<name>.cave.json`
// ---------------------

std::string_view text_of(std::span<const u8> bytes) {
  return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool generator_source_hash(std::span<const u8> bytes, u64& out, std::string& error) {
  Passage passage;
  if (!read_passage(text_of(bytes), passage, error)) return false;
  out = passage_source_hash(passage);
  return true;
}

bool generator_build(std::span<const u8> bytes, const assets::MeshGeneratorInputs& inputs,
                     const geometry::ClusterLodOptions& lod, u32 page_bytes,
                     geometry::ClusterFileData& out, std::string& error) {
  Passage passage;
  if (!read_passage(text_of(bytes), passage, error)) return false;
  PassageReport report;
  return build_passage_container(passage, lod, page_bytes, out, &report, error, inputs.materials);
}

constexpr assets::MeshGeneratorDesc k_cave_generator{
    .name = "cave",
    .source_hash = generator_source_hash,
    .build = generator_build,
};
const assets::MeshGeneratorRegistrar k_cave_registrar{k_cave_generator};

}  // namespace

}  // namespace engine::caves
