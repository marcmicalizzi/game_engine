// The content build as a library (docs/subsystems/content_build.md). The code moved here from
// apps/engine_content/main.cpp unchanged, so that engine-host's `content.build` runs the build
// engine-content runs rather than a second one: the rules, the job layout and the skip are the
// command line's, and its tests (apps/engine_content/tests) are this module's end-to-end tests.
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/log/log.h>
#include <core/time/time.h>
#include <domain/assets/gltf.h>
#include <domain/content_build/content_build.h>
#include <domain/geometry/cluster_file.h>
#include <domain/texture/material_textures.h>
#include <domain/texture/texture_build.h>
#include <domain/texture/texture_file.h>
#include <foundation/io/vfs.h>

#include <cmath>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace engine::content_build {

namespace {

// The same category engine-content's own records use, so `--log content=debug` reaches both.
ENGINE_LOG_CATEGORY_DEFINE(log_content, "content");

u64 split_mix(u64 z) noexcept {
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

// The build key of a container built with `--no-morph`: the ordinary key, moved by a constant
// through one SplitMix64 round. `geometry::cluster_cache_key` has no morph term because nothing
// else builds without the channels; this keeps such a container from recording the key a full
// build would record, which is what `build-all` skips on.
u64 morph_key(u64 key, const MeshOptions& options) noexcept {
  if (options.morph) return key;
  return split_mix(key ^ 0x6e6f2d6d6f727068ull);  // "no-morph"
}

// And of one built with `--atlas repack`: moved by the atlas step's own key, which covers every
// option that changes its bytes and the module's version (`atlas::repack_options_key`). `keep`
// leaves the key alone, so every container built before the flag existed is still the answer to
// the question it answered — which is why this is a mix here and not a new term in
// `geometry::cluster_cache_key`, whose every existing key would have moved with it.
u64 atlas_key(u64 key, const MeshOptions& options) noexcept {
  if (!options.repack) return key;
  return split_mix(key ^ 0x6174726570616b21ull ^ atlas::repack_options_key(options.atlas));
}

}  // namespace

bool read_atlas_mode(std::string_view text, bool& repack) {
  if (text == "keep")
    repack = false;
  else if (text == "repack")
    repack = true;
  else
    return false;
  return true;
}

bool read_normal_map_mode(std::string_view text, atlas::NormalMapMode& out) {
  if (text == "convert")
    out = atlas::NormalMapMode::convert;
  else if (text == "resample")
    out = atlas::NormalMapMode::resample;
  else
    return false;
  return true;
}

geometry::ClusterLodOptions lod_options_of(const MeshOptions& options) {
  geometry::ClusterLodOptions lod_options;
  lod_options.max_triangles = options.max_triangles;
  lod_options.max_vertices = options.max_vertices;
  lod_options.normal_weight = options.normal_weight;
  lod_options.uv_weight = options.uv_weight;
  lod_options.uv_seams = options.uv_seams;
  lod_options.normal_seams = options.normal_seams;
  return lod_options;
}

// Every key a build records: the geometry's, then the two steps outside it.
u64 build_key_of(u64 source_hash, const MeshOptions& options) noexcept {
  return atlas_key(morph_key(geometry::cluster_cache_key(source_hash, lod_options_of(options),
                                                         options.weld, options.page_bytes),
                             options),
                   options);
}

bool read_seam_rule(std::string_view text, geometry::SeamRule& out) {
  if (text == "none")
    out = geometry::SeamRule::none;
  else if (text == "protect")
    out = geometry::SeamRule::protect;
  else if (text == "lock")
    out = geometry::SeamRule::lock;
  else
    return false;
  return true;
}

bool options_in_range(const MeshOptions& options) {
  return options.max_triangles >= 4 && options.max_triangles <= 256 && options.max_vertices >= 1 &&
         options.max_vertices <= 255;
}

namespace {

// A UV that far outside the unit square is a broken unwrap or a unit mix-up, not tiling: 16
// wraps is already a texel density no texture pipeline can serve. A warning, not an error,
// because tiling trim sheets legitimately run to a few wraps.
constexpr f32 k_uv_limit = 16.0f;

struct Diagnostics {
  Vector<Diagnostic> warnings;
  Diagnostic error;  // the first one; `rule` is empty while there is none
};

bool is_finite(Vec3 p) noexcept {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

bool fail_rule(Diagnostics& out, const char* rule, std::string message) {
  out.error.rule = rule;
  out.error.message = std::move(message);
  return false;
}

// Checks the mesh as it was loaded, before welding, so that a primitive and a triangle in a
// diagnostic are the ones in the source file. Returns false on the first error.
bool validate_mesh(const assets::MeshData& mesh, const std::string& source, Diagnostics& out) {
  const u32 vertex_count = mesh.positions.size();
  const bool has_uvs = mesh.uvs.size() == vertex_count && vertex_count != 0;
  for (u32 p = 0; p < mesh.primitives.size(); ++p) {
    const assets::Primitive& primitive = mesh.primitives[p];
    const std::string where = "primitive " + std::to_string(p) + " of '" + source + "'";
    if (primitive.index_count == 0)
      return fail_rule(out, k_rule_empty_primitive, where + " has no triangles");
    if (primitive.index_count % 3 != 0) {
      return fail_rule(out, k_rule_empty_primitive,
                       where + " has " + std::to_string(primitive.index_count) +
                           " indices, which is not a whole number of triangles");
    }
    f32 uv_extreme = 0.0f;
    for (u32 t = 0; t * 3 < primitive.index_count; ++t) {
      const u32 corner[3] = {mesh.indices[primitive.first_index + t * 3 + 0],
                             mesh.indices[primitive.first_index + t * 3 + 1],
                             mesh.indices[primitive.first_index + t * 3 + 2]};
      for (const u32 index : corner) {
        if (index >= vertex_count) {
          return fail_rule(out, k_rule_index_range,
                           where + ", triangle " + std::to_string(t) + ": index " +
                               std::to_string(index) + " is outside the " +
                               std::to_string(vertex_count) + " vertices");
        }
      }
      for (const u32 index : corner) {
        if (!is_finite(mesh.positions[index])) {
          return fail_rule(out, k_rule_nan_position,
                           where + ", triangle " + std::to_string(t) + ": vertex " +
                               std::to_string(index) + " has a position that is not finite");
        }
      }
      // Degenerate either topologically (a repeated corner) or geometrically (two edges exactly
      // parallel in float arithmetic, so the triangle has no area and covers no pixel).
      const Vec3 normal = cross(mesh.positions[corner[1]] - mesh.positions[corner[0]],
                                mesh.positions[corner[2]] - mesh.positions[corner[0]]);
      const bool repeated =
          corner[0] == corner[1] || corner[1] == corner[2] || corner[0] == corner[2];
      if (repeated || (normal.x == 0.0f && normal.y == 0.0f && normal.z == 0.0f)) {
        return fail_rule(out, k_rule_degenerate,
                         where + ", triangle " + std::to_string(t) + " is degenerate: indices " +
                             std::to_string(corner[0]) + " " + std::to_string(corner[1]) + " " +
                             std::to_string(corner[2]) +
                             (repeated ? " repeat a vertex" : " span no area"));
      }
      if (has_uvs) {
        for (const u32 index : corner) {
          const Vec2 uv = mesh.uvs[index];
          if (std::isfinite(uv.x) && std::abs(uv.x) > std::abs(uv_extreme)) uv_extreme = uv.x;
          if (std::isfinite(uv.y) && std::abs(uv.y) > std::abs(uv_extreme)) uv_extreme = uv.y;
        }
      }
    }
    if (std::abs(uv_extreme) > k_uv_limit) {
      Diagnostic warning;
      warning.rule = k_rule_uv_range;
      warning.message = where + " has a UV coordinate of " + std::to_string(uv_extreme) +
                        ", far outside [-16, 16]";
      out.warnings.push_back(std::move(warning));
    }
  }

  // A material that names an image the build cannot get bytes for draws untextured, which is
  // worth saying out loud but is not a reason to refuse the mesh.
  const std::string dir(io::parent_path(source));
  for (u32 m = 0; m < mesh.materials.size(); ++m) {
    const assets::Material& material = mesh.materials[m];
    const i32 slots[5] = {material.base_color_image, material.metallic_roughness_image,
                          material.normal_image, material.occlusion_image, material.emissive_image};
    for (const i32 slot : slots) {
      if (slot < 0) continue;
      const std::string named = "material " + std::to_string(m) + " of '" + source + "' names ";
      if (static_cast<u32>(slot) >= mesh.images.size()) {
        return fail_rule(out, k_rule_missing_image,
                         named + "image " + std::to_string(slot) + ", outside the " +
                             std::to_string(mesh.images.size()) + " images the file has");
      }
      const assets::ImageRef& image = mesh.images[static_cast<u32>(slot)];
      Diagnostic warning;
      warning.rule = k_rule_missing_image;
      if (image.uri.empty() && image.bytes.empty()) {
        warning.message = named + "image " + std::to_string(slot) +
                          ", which has neither a path "
                          "nor any bytes";
      } else if (!image.uri.empty() && !io::exists(io::join_path(dir, image.uri))) {
        warning.message = named + "image '" + image.uri + "', which is not there";
      } else {
        continue;
      }
      out.warnings.push_back(std::move(warning));
    }
  }
  return true;
}

// One primitive's DAG, as a job sees it. The result lives here rather than in a shared list, so
// nothing the jobs write is ordered by when they finish.
struct PartBuild {
  const assets::MeshData* mesh = nullptr;
  const geometry::ClusterLodOptions* options = nullptr;
  const geometry::AttributeSource* attributes = nullptr;
  u32 first_index = 0;
  u32 index_count = 0;
  i32 material = -1;
  geometry::ClusterLodMesh out;
  std::string error;
  bool ok = false;
};

void build_part(void* data) {
  PartBuild& part = *static_cast<PartBuild*>(data);
  const std::span<const u32> range(part.mesh->indices.data() + part.first_index, part.index_count);
  part.ok = geometry::build_cluster_lod(part.mesh->positions, range, *part.options, part.out,
                                        &part.error, *part.attributes);
}

}  // namespace

void collect_texture_sources(const geometry::ClusterFileData& data, std::string_view dir,
                             Vector<TextureSource>& out) {
  out.clear();
  for (u32 i = 0; i < data.textures.size(); ++i) {
    const geometry::ClusterFileTexture& record = data.textures[i];
    if (record.options == 0) continue;
    TextureSource source;
    source.image = i;
    source.options = record.options;
    if (i < data.images.size() && !data.images[i].bytes.empty()) {
      source.bytes = data.images[i].bytes;
    } else if (i < data.image_paths.size() && !data.image_paths[i].empty()) {
      source.file = dir.empty() ? data.image_paths[i] : io::join_path(dir, data.image_paths[i]);
    } else {
      continue;  // an image with neither bytes nor a path: validation has already said so
    }
    out.push_back(std::move(source));
  }
}

// Imports, validates, clusters, and writes one mesh. `pool` is where the per-primitive DAG
// builds run; null builds them one after another on the calling thread, which is what `build-all`
// does because it already has one job per mesh in flight and nesting the two levels would only
// deepen the wait stack. Either way the merge walks the primitives in index order.
bool build_one(const std::string& input, const std::string& output, const MeshOptions& options,
               bool strict, jobs::JobSystem* pool, u64 source_hash, u64 build_key, BuildResult& out,
               Diagnostic& error) {
  const i64 start_ns = time::monotonic_ns();
  out.path = output;
  out.source_hash = source_hash;
  out.build_key = build_key;

  assets::MeshData mesh;
  std::string message;
  if (!assets::load_gltf(input, mesh, &message)) {
    error.rule = "source.unreadable";
    error.message = message;
    return false;
  }
  if (!options.morph) mesh.morph.clear();

  Diagnostics diagnostics;
  if (!validate_mesh(mesh, input, diagnostics)) {
    error = std::move(diagnostics.error);
    return false;
  }
  out.warnings = std::move(diagnostics.warnings);
  if (strict && !out.warnings.empty()) {
    error.rule = out.warnings[0].rule;
    error.message = out.warnings[0].message + " (--strict)";
    return false;
  }

  // UV-degenerate triangles are repaired before the weld, so a corner that took its neighbouring
  // island's UV is merged into that island's vertex by the weld below. After validation, so a
  // diagnostic still names the triangle as the source numbers it. `engine-view --mesh` calls the
  // same function at the same point, because the two share cache entries.
  if (!assets::repair_uv_degenerate_triangles(mesh, out.uv_repair, &message)) {
    error.rule = k_rule_uv_degenerate;
    error.message = message;
    return false;
  }

  // `--atlas repack` (docs/subsystems/atlas.md): after the repair, so the rebake samples the
  // source through the UVs the repair left, and before the weld, which merges what the new atlas
  // made one point and drops the vertices no repacked triangle references any more. Its images
  // replace the source's in the container below; nothing of the source's UV layout survives.
  if (options.repack) {
    out.repacked = true;
    if (!atlas::repack_atlas(mesh, io::parent_path(input), options.atlas, pool, out.atlas,
                             &message)) {
      error.rule = k_rule_atlas_repack;
      error.message = "'" + input + "': " + message;
      return false;
    }
  }

  const u32 loaded_vertices = mesh.positions.size();
  u32 vertices = loaded_vertices;
  if (options.weld) {
    // `assets::weld_vertices` hands the weld every per-vertex stream the mesh owns — the skin
    // bindings and the morph channels as well as the positions, normals and UVs — so two
    // coincident vertices that disagree about any of them stay two vertices, and the channels are
    // renumbered with the vertices instead of being left in the old numbering
    // ([geometry](geometry.md), "Morph channels"). `engine-view --mesh` calls the same function,
    // because the two apps share cache entries and what one writes the other has to find.
    vertices = assets::weld_vertices(mesh);
  }
  ENGINE_LOG_INFO(log_content, "mesh loaded", log::field("path", input),
                  log::field("vertices", loaded_vertices), log::field("welded", vertices),
                  log::field("triangles", mesh.indices.size() / 3),
                  log::field("primitives", mesh.primitives.size()),
                  log::field("materials", mesh.materials.size()),
                  log::field("skins", mesh.skins.size()), log::field("images", mesh.images.size()));

  // One DAG per primitive, so every cluster belongs to exactly one material; merging keeps the
  // parts' clusters and remembers which part each came from.
  const geometry::ClusterLodOptions lod_options = lod_options_of(options);
  const geometry::AttributeSource attributes = assets::attribute_source(mesh);
  Vector<PartBuild> parts;
  parts.reserve(mesh.primitives.size());
  for (const assets::Primitive& primitive : mesh.primitives) {
    PartBuild part;
    part.mesh = &mesh;
    part.options = &lod_options;
    part.attributes = &attributes;
    part.first_index = primitive.first_index;
    part.index_count = primitive.index_count;
    part.material = primitive.material;
    parts.push_back(std::move(part));
  }
  if (parts.empty()) {
    error.rule = k_rule_empty_primitive;
    error.message = "'" + input + "' has no triangles to cluster";
    return false;
  }

  if (pool != nullptr && parts.size() > 1) {
    Vector<jobs::Job> job_list;
    job_list.reserve(parts.size());
    jobs::Counter counter;
    for (PartBuild& part : parts)
      job_list.push_back(jobs::Job{build_part, &part, &counter});
    pool->schedule(jobs::Pool::Performance,
                   std::span<const jobs::Job>(job_list.data(), job_list.size()), counter);
    pool->wait(counter);
  } else {
    for (PartBuild& part : parts)
      build_part(&part);
  }
  // Failures are reported in primitive order too: the same input fails with the same sentence
  // whatever --jobs said.
  for (const PartBuild& part : parts) {
    if (!part.ok) {
      error.rule = "geometry.cluster_build";
      error.message = part.error;
      return false;
    }
  }

  Vector<geometry::ClusterLodMesh> lods;
  Vector<i32> part_material;
  lods.reserve(parts.size());
  part_material.reserve(parts.size());
  for (PartBuild& part : parts) {
    lods.push_back(std::move(part.out));
    part_material.push_back(part.material);
  }

  geometry::ClusterFileData data;
  Vector<u32> part_of_cluster;
  if (!geometry::merge_cluster_lod(lods, data.mesh, &part_of_cluster, &message)) {
    error.rule = "geometry.cluster_merge";
    error.message = message;
    return false;
  }
  if (data.mesh.mesh.clusters.size() > k_max_clusters) {
    error.rule = k_rule_cluster_budget;
    error.message = "'" + input + "' builds " + std::to_string(data.mesh.mesh.clusters.size()) +
                    " clusters, more than the " + std::to_string(k_max_clusters) +
                    " the visibility buffer can name";
    return false;
  }

  // Into streaming pages, coarse to fine and group by group (docs/plan/04-renderer.md §4.3 step 3
  // and §4.9). This renumbers the clusters, so it happens before anything downstream indexes
  // them and the map from cluster to primitive is carried through the same permutation.
  if (options.page_bytes != 0) {
    geometry::ClusterPagesOptions page_options;
    page_options.page_bytes = options.page_bytes;
    Vector<u32> source_of_cluster;
    if (!geometry::build_cluster_pages(data.mesh, page_options, data.pages, &message,
                                       &source_of_cluster)) {
      error.rule = "geometry.cluster_pages";
      error.message = message;
      return false;
    }
    geometry::permute_cluster_array(source_of_cluster, part_of_cluster);
  }

  data.materials.reserve(mesh.materials.size() + 1);
  for (const assets::Material& source : mesh.materials) {
    geometry::ClusterFileMaterial material;
    material.base_color = source.base_color;
    material.metallic = source.metallic;
    material.roughness = source.roughness;
    material.base_color_image = source.base_color_image;
    material.normal_image = source.normal_image;
    material.metallic_roughness_image =
        geometry::encode_optional_image(source.metallic_roughness_image);
    material.occlusion_image = geometry::encode_optional_image(source.occlusion_image);
    material.emissive_image = geometry::encode_optional_image(source.emissive_image);
    material.emissive = source.emissive;
    material.normal_scale = source.normal_scale;
    material.alpha =
        geometry::encode_alpha_word(source.alpha_mode, source.double_sided, source.alpha_cutoff);
    data.materials.push_back(material);
    // How its slots sample their images (section 33), which the texture records below read too.
    data.material_sampling.push_back(
        geometry::encode_material_sampling(source.sampling, source.occlusion_strength));
  }
  // A primitive that names no material gets one appended default, shared by all of them.
  constexpr u32 k_no_default = ~u32{0};
  u32 default_material = k_no_default;
  data.cluster_material.reserve(part_of_cluster.size());
  for (const u32 part : part_of_cluster) {
    const i32 index = part_material[part];
    if (index >= 0 && static_cast<u32>(index) < mesh.materials.size()) {
      data.cluster_material.push_back(static_cast<u32>(index));
      continue;
    }
    if (default_material == k_no_default) {
      default_material = data.materials.size();
      data.materials.push_back(geometry::ClusterFileMaterial{});
      data.material_sampling.push_back(geometry::ClusterFileMaterialSampling{});
    }
    data.cluster_material.push_back(default_material);
  }
  // Image paths as the glTF gives them, relative to the glTF file; an image the file embedded has
  // no path of its own, so its **encoded bytes** travel in the container instead and the slot
  // keeps an empty path. That is what makes the container drawable wherever it ends up: a GLB
  // holds its textures inside itself, and a container that recorded only their empty paths drew
  // the mesh untextured while the GLB beside it drew it textured ([geometry](geometry.md)).
  data.image_paths.reserve(mesh.images.size());
  data.images.reserve(mesh.images.size());
  for (const assets::ImageRef& image : mesh.images) {
    data.image_paths.push_back(image.uri);
    geometry::ClusterImage carried;
    carried.mime_type = image.mime_type;
    carried.bytes = image.bytes;
    data.images.push_back(std::move(carried));
  }
  const geometry::ClusterImageSummary images = geometry::summarize_cluster_images(data);
  // Where each image's built texture is: the options its material slots ask for and, for the
  // images this container carries, the key (docs/subsystems/texture.md). engine-view's own cache
  // writer calls the same function, because the two share cache entries byte for byte.
  texture::fill_cluster_texture_records(data);
  collect_texture_sources(data, io::parent_path(input), out.texture_sources);

  // The source as it was given, so the renderer resolves the image paths above against its
  // directory however it came by the container, and the source's identity, so that a later build
  // recognizes this container as the answer to the same question.
  data.source_path = input;
  data.source_hash = source_hash;
  data.build_key = build_key;

  if (!geometry::write_cluster_file(output, data, &message)) {
    error.rule = "output.unwritable";
    error.message = message;
    return false;
  }
  io::FileInfo info;
  const geometry::ClusterLodMesh& lod = data.mesh;
  out.clusters = lod.mesh.clusters.size();
  out.leaf_clusters = lod.level_cluster_counts.empty() ? 0u : lod.level_cluster_counts[0];
  out.triangles = lod.leaf_triangle_count;
  out.lod_levels = lod.level_cluster_counts.size();
  out.vertices = lod.mesh.vertices.size();
  out.pages = data.pages.pages.size();
  out.page_bytes = options.page_bytes;
  if (out.pages != 0) {
    u64 page_total = 0;
    for (const geometry::ClusterPageDesc& page : data.pages.pages)
      page_total += page.bytes;
    out.page_fill = static_cast<f64>(page_total) /
                    (static_cast<f64>(out.pages) * static_cast<f64>(options.page_bytes));
  }
  out.materials = data.materials.size();
  out.images = data.image_paths.size();
  out.embedded_images = images.embedded;
  out.deduplicated_images = images.deduplicated;
  out.image_bytes = images.bytes;
  out.vertex_id_source = geometry::vertex_id_source_name(lod.mesh.vertex_id_source);
  out.bytes = io::stat_file(output, info) == io::Status::Ok ? info.size : 0;
  out.hash = geometry::cluster_file_hash(data);
  out.build_ms = static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e6;

  ENGINE_LOG_INFO(log_content, "cluster file written", log::field("path", output),
                  log::field("clusters", out.clusters), log::field("pages", out.pages),
                  log::field("bytes", out.bytes), log::field("build_ms", out.build_ms));
  return true;
}

// The repairs a build made, as the `repairs` array of its JSON line: one row per rule that fired,
// empty when none did, so a reader tells "nothing to repair" from "an older build" by the key.
JsonValue repairs_json(const BuildResult& result) {
  JsonValue rows = JsonValue::array();
  const geometry::UvRepairReport& r = result.uv_repair;
  if (r.triangles != 0) {
    JsonValue row = JsonValue::object();
    row.set("rule", JsonValue(k_rule_uv_degenerate));
    row.set("islands", JsonValue(r.islands));
    row.set("triangles", JsonValue(r.triangles));
    row.set("refolded", JsonValue(r.refolded));
    row.set("dropped", JsonValue(r.dropped));
    row.set("unrepaired", JsonValue(r.unrepaired));
    row.set("corners_from_neighbours", JsonValue(r.corners_from_neighbours));
    row.set("corners_on_edge", JsonValue(r.corners_on_edge));
    row.set("threshold_texels_4096", JsonValue(r.threshold_texels));
    rows.push_back(std::move(row));
  }
  return rows;
}

namespace {

const char* space_label(atlas::TextureSpace space) {
  switch (space) {
    case atlas::TextureSpace::srgb: return "srgb";
    case atlas::TextureSpace::linear: return "linear";
    case atlas::TextureSpace::tangent_normal: return "normal";
  }
  return "?";
}

}  // namespace

// What the atlas step did, as the `atlas` object of a build's JSON line: `{"mode":"keep"}` when it
// was not asked for, and otherwise the charts, the atlas per material, every rebaked image with
// how far it is from its source on the surface, and the time each stage took.
JsonValue atlas_json(const BuildResult& result, const MeshOptions& options) {
  JsonValue out = JsonValue::object();
  out.set("mode", JsonValue(result.repacked ? "repack" : "keep"));
  if (!result.repacked) return out;
  const atlas::RepackReport& r = result.atlas;
  out.set("normal_maps",
          JsonValue(options.atlas.normal_maps == atlas::NormalMapMode::convert ? "convert"
                                                                               : "resample"));
  out.set("proxy_triangles", JsonValue(options.atlas.proxy_triangles));
  out.set("supersample", JsonValue(options.atlas.supersample));
  out.set("chart_cost_milli", JsonValue(options.atlas.max_chart_cost_milli));
  out.set("padding", JsonValue(options.atlas.padding));
  out.set("materials_repacked", JsonValue(r.materials_repacked));
  out.set("materials_untextured", JsonValue(r.materials_untextured));
  out.set("materials_unreadable", JsonValue(r.materials_unreadable));
  out.set("materials_declined", JsonValue(r.materials_declined));
  out.set("triangles", JsonValue(r.triangles));
  out.set("charts", JsonValue(r.charts));
  out.set("vertices_before", JsonValue(r.vertices_before));
  out.set("vertices_after", JsonValue(r.vertices_after));
  out.set("images_before", JsonValue(r.images_before));
  out.set("images_after", JsonValue(r.images_after));
  out.set("image_bytes", JsonValue(r.image_bytes));
  JsonValue ms = JsonValue::object();
  ms.set("decode", JsonValue(r.decode_ms));
  ms.set("chart", JsonValue(r.chart_ms));
  ms.set("rebake", JsonValue(r.rebake_ms));
  ms.set("encode", JsonValue(r.encode_ms));
  ms.set("total", JsonValue(r.total_ms));
  out.set("ms", std::move(ms));
  JsonValue materials = JsonValue::array();
  for (const atlas::RepackedMaterial& m : r.materials) {
    JsonValue row = JsonValue::object();
    row.set("material", JsonValue(m.material));
    row.set("triangles", JsonValue(m.triangles));
    row.set("charts", JsonValue(m.charts));
    row.set("resolution", JsonValue(m.resolution));
    row.set("utilization", JsonValue(static_cast<f64>(m.utilization)));
    row.set("pack_attempts", JsonValue(m.pack_attempts));
    row.set("proxy_triangles", JsonValue(m.proxy_triangles));
    row.set("proxy_charts", JsonValue(m.proxy_charts));
    row.set("folded_triangles", JsonValue(m.folded_triangles));
    row.set("chart_ms", JsonValue(m.chart_ms));
    row.set("pack_ms", JsonValue(m.pack_ms));
    row.set("unatlased_triangles", JsonValue(m.unatlased_triangles));
    row.set("crumb_charts", JsonValue(m.crumb_charts));
    row.set("crumb_triangles", JsonValue(m.crumb_triangles));
    row.set("source_vertices", JsonValue(m.source_vertices));
    row.set("vertices", JsonValue(m.vertices));
    materials.push_back(std::move(row));
  }
  out.set("detail", std::move(materials));
  JsonValue images = JsonValue::array();
  for (const atlas::RebakedImage& im : r.images) {
    JsonValue row = JsonValue::object();
    row.set("material", JsonValue(im.material));
    row.set("source_image", JsonValue(im.source_image));
    row.set("image", JsonValue(im.image));
    row.set("space", JsonValue(space_label(im.space)));
    row.set("size", JsonValue(im.width));
    row.set("source_width", JsonValue(im.source_width));
    row.set("source_height", JsonValue(im.source_height));
    row.set("png_bytes", JsonValue(im.png_bytes));
    JsonValue e = JsonValue::object();
    e.set("unit", JsonValue(im.space == atlas::TextureSpace::tangent_normal ? "degrees" : "8-bit"));
    e.set("samples", JsonValue(im.error.samples));
    e.set("mean", JsonValue(im.error.mean));
    e.set("p99", JsonValue(im.error.p99));
    e.set("max", JsonValue(im.error.max));
    row.set("rebake_error", std::move(e));
    images.push_back(std::move(row));
  }
  out.set("images", std::move(images));
  JsonValue notes = JsonValue::array();
  for (const std::string& note : r.notes)
    notes.push_back(JsonValue(note));
  out.set("notes", std::move(notes));
  return out;
}

// The job system the per-primitive and per-mesh builds run on. `worker_count` is `--jobs`; 0
// leaves the system its own default, one worker per performance CPU. The efficiency pool is
// asked for one worker because nothing here uses it.
jobs::JobSystemConfig job_config(u32 worker_count) {
  jobs::JobSystemConfig config;
  config.performance_workers = worker_count;
  config.efficiency_workers = 1;
  return config;
}

// Reads the manifest and resolves every path against the manifest's own directory, so a manifest
// is movable as a unit and says the same thing from any working directory.
bool read_manifest(const std::string& path, const MeshOptions& defaults, Vector<ManifestEntry>& out,
                   std::string& error, std::span<const char* const> other_arrays) {
  std::string text;
  const io::Status status = io::read_file(path, text);
  if (status != io::Status::Ok) {
    error = "cannot read manifest '" + path + "': " + io::status_name(status);
    return false;
  }
  JsonValue document;
  const JsonParseResult parsed = parse_json(text, document);
  if (!parsed.ok) {
    error = "manifest '" + path + "' line " + std::to_string(parsed.line) + " column " +
            std::to_string(parsed.column) + ": " + parsed.message;
    return false;
  }
  const JsonValue* meshes = document.is_object() ? document.find("meshes") : nullptr;
  if (meshes == nullptr && document.is_object()) {
    // A manifest of a derived step's entries alone (a list of clips) has no meshes to build.
    for (const char* other : other_arrays) {
      const JsonValue* array = document.find(other);
      if (array != nullptr && array->is_array()) return true;
    }
  }
  if (meshes == nullptr || !meshes->is_array()) {
    error = "manifest '" + path + "' has no \"meshes\" array";
    return false;
  }
  const std::string dir(io::parent_path(path));
  auto resolve = [&dir](std::string_view relative) {
    return io::is_absolute_path(relative) ? io::normalize_path(relative)
                                          : io::join_path(dir, relative);
  };
  for (usize i = 0; i < meshes->size(); ++i) {
    const JsonValue& entry = (*meshes)[i];
    const std::string at = "manifest '" + path + "' mesh " + std::to_string(i);
    if (!entry.is_object()) {
      error = at + " is not an object";
      return false;
    }
    const JsonValue* source = entry.find("source");
    if (source == nullptr || !source->is_string()) {
      error = at + " has no \"source\" string";
      return false;
    }
    ManifestEntry built;
    built.options = defaults;  // the command line sets what an entry does not say
    built.source = resolve(source->as_string());
    if (const JsonValue* output = entry.find("output"); output != nullptr) {
      if (!output->is_string()) {
        error = at + " has an \"output\" that is not a string";
        return false;
      }
      built.output = resolve(output->as_string());
    }
    if (const JsonValue* options = entry.find("options"); options != nullptr) {
      if (!options->is_object()) {
        error = at + " has an \"options\" that is not an object";
        return false;
      }
      u64 value = 0;
      if (const JsonValue* v = options->find("max_triangles"); v != nullptr) {
        if (!v->get_u64(value)) {
          error = at + ": \"max_triangles\" is not a number";
          return false;
        }
        built.options.max_triangles = static_cast<u32>(value);
      }
      if (const JsonValue* v = options->find("max_vertices"); v != nullptr) {
        if (!v->get_u64(value)) {
          error = at + ": \"max_vertices\" is not a number";
          return false;
        }
        built.options.max_vertices = static_cast<u32>(value);
      }
      if (const JsonValue* v = options->find("page_bytes"); v != nullptr) {
        if (!v->get_u64(value)) {
          error = at + ": \"page_bytes\" is not a number";
          return false;
        }
        built.options.page_bytes = static_cast<u32>(value);
      }
      if (const JsonValue* v = options->find("weld"); v != nullptr) {
        bool weld = true;
        if (!v->get_bool(weld)) {
          error = at + ": \"weld\" is not a boolean";
          return false;
        }
        built.options.weld = weld;
      }
      if (const JsonValue* v = options->find("atlas"); v != nullptr) {
        if (!v->is_string() || !read_atlas_mode(v->as_string(), built.options.repack)) {
          error = at + ": \"atlas\" is \"keep\" or \"repack\"";
          return false;
        }
      }
      if (!options_in_range(built.options)) {
        error = at + ": max_triangles is 4..256 and max_vertices is 1..255";
        return false;
      }
    }
    out.push_back(std::move(built));
  }
  return true;
}

void run_mesh_task(void* data) {
  MeshTask& task = *static_cast<MeshTask*>(data);
  const ManifestEntry& entry = *task.entry;
  u64 source_hash = 0;
  std::string message;
  if (!assets::source_mesh_hash(entry.source, source_hash, &message)) {
    task.error.rule = "source.unreadable";
    task.error.message = message;
    task.state = TaskState::Failed;
    return;
  }
  const u64 key = build_key_of(source_hash, entry.options);
  const bool to_cache = entry.output.empty();
  const std::string output = to_cache ? geometry::cluster_cache_path(*task.ddc, key) : entry.output;
  task.result.path = output;
  task.result.cached = to_cache;
  task.result.source_hash = source_hash;
  task.result.build_key = key;

  // Up to date when the container that is already there was built from these bytes with these
  // options. A container that records no identity — an older build, or one engine-view wrote —
  // has a zero key and is rebuilt rather than trusted.
  u64 had_hash = 0;
  u64 had_key = 0;
  if (!task.force && io::exists(output) &&
      geometry::read_cluster_file_identity(output, had_hash, had_key) && had_key != 0 &&
      had_key == key && had_hash == source_hash) {
    io::FileInfo info;
    task.result.bytes = io::stat_file(output, info) == io::Status::Ok ? info.size : 0;
    task.state = TaskState::Skipped;
    return;
  }
  // In a manifest the per-primitive jobs are not nested inside this one (`pool` is null): the
  // manifest is the coarser level and already fills the pool, and one wait per mesh keeps the
  // helper stack shallow. A single mesh run on its own hands its pool down instead.
  task.state = build_one(entry.source, output, entry.options, task.strict, task.pool, source_hash,
                         key, task.result, task.error)
                   ? TaskState::Built
                   : TaskState::Failed;
}

// ---- the texture step (docs/subsystems/texture.md) ---------------------------------------------

const char* texture_state_name(TextureState state) noexcept {
  switch (state) {
    case TextureState::Built: return "built";
    case TextureState::Skipped: return "skipped";
    case TextureState::Shared: return "shared";
    case TextureState::Missing: return "missing";
    case TextureState::Failed: return "failed";
    case TextureState::Unreadable: return "unreadable";
  }
  return "?";
}

namespace {

f64 ms_since(i64 start_ns) noexcept {
  return static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e6;
}

// A number JSON can hold: PSNR is infinite for an exact texture, and JSON has no infinity.
JsonValue finite_or_null(f64 value) {
  return std::isfinite(value) ? JsonValue(value) : JsonValue();
}

// One texture of the step on its way through: the source's bytes, owned when they came from a
// file, and what building them needs. A job writes its own task and nothing else.
struct TextureTask {
  TextureResult result;
  std::string file_bytes;
  std::span<const u8> bytes;
  texture::TextureBuildOptions options;
  jobs::JobSystem* pool = nullptr;
  i64 start_ns = 0;
};

void build_texture_task(void* data) {
  TextureTask& task = *static_cast<TextureTask*>(data);
  TextureResult& out = task.result;
  const i64 start_ns = time::monotonic_ns();
  texture::TextureData built;
  std::string error;
  if (!texture::build_texture_from_encoded(task.bytes, task.options, built, task.pool, &error,
                                           &out.report)) {
    out.state = TextureState::Unreadable;
    out.error = error;
    return;
  }
  const io::Status made = io::make_directories(io::parent_path(out.path));
  if (made != io::Status::Ok || !texture::write_texture_file(out.path, built, &error)) {
    out.state = TextureState::Failed;
    out.error =
        made != io::Status::Ok
            ? std::string("cannot create the texture cache directory: ") + io::status_name(made)
            : error;
    return;
  }
  io::FileInfo info;
  out.bytes = io::stat_file(out.path, info) == io::Status::Ok ? info.size : 0;
  out.format = built.format;
  out.color_space = built.color_space;
  out.width = built.width;
  out.height = built.height;
  out.levels = built.levels.size();
  out.build_ms = ms_since(start_ns);
  out.state = TextureState::Built;
}

}  // namespace

void run_texture_step(std::span<const TextureSource* const> sources, std::span<const u32> mesh_of,
                      const std::string& ddc, jobs::JobSystem& pool, Vector<TextureResult>& out) {
  out.clear();
  Vector<TextureTask> tasks(static_cast<u32>(sources.size()));
  Vector<u64> seen;
  Vector<jobs::Job> job_list;
  jobs::Counter counter;
  for (u32 i = 0; i < sources.size(); ++i) {
    const TextureSource& source = *sources[i];
    TextureTask& task = tasks[i];
    TextureResult& r = task.result;
    task.start_ns = time::monotonic_ns();
    r.mesh = mesh_of[i];
    r.image = source.image;
    task.bytes = std::span<const u8>(source.bytes.data(), source.bytes.size());
    if (!source.file.empty()) {
      const io::Status status = io::read_file(source.file, task.file_bytes);
      if (status != io::Status::Ok) {
        r.state = TextureState::Missing;
        r.path = source.file;
        r.error = "cannot read '" + source.file + "': " + io::status_name(status);
        continue;
      }
      task.bytes = std::span<const u8>(reinterpret_cast<const u8*>(task.file_bytes.data()),
                                       task.file_bytes.size());
    }
    if (!texture::unpack_texture_options(source.options, task.options)) {
      r.state = TextureState::Failed;
      r.error = "the container's texture options word " + std::to_string(source.options) +
                " is not one this build reads";
      continue;
    }
    r.source_bytes = task.bytes.size();
    r.source_hash = hash_bytes(task.bytes.data(), task.bytes.size());
    r.key = texture::texture_cache_key(r.source_hash, task.options);
    r.path = texture::texture_cache_path(ddc, r.key);
    bool shared = false;
    for (const u64 key : seen)
      shared = shared || key == r.key;
    if (shared) {
      r.state = TextureState::Shared;
      continue;
    }
    seen.push_back(r.key);
    // Up to date when the entry that is there was built from these bytes with these options; the
    // identity is checked rather than trusted from the file name, the way a container's is.
    u64 had_hash = 0;
    u64 had_key = 0;
    if (io::exists(r.path) && texture::read_texture_file_identity(r.path, had_hash, had_key) &&
        had_hash == r.source_hash && had_key == r.key) {
      io::FileInfo info;
      r.bytes = io::stat_file(r.path, info) == io::Status::Ok ? info.size : 0;
      r.state = TextureState::Skipped;
      continue;
    }
    task.pool = &pool;
    r.state = TextureState::Failed;  // until the job says otherwise
    job_list.push_back(jobs::Job{build_texture_task, &task, &counter});
  }
  if (!job_list.empty()) {
    pool.schedule(jobs::Pool::Performance,
                  std::span<const jobs::Job>(job_list.data(), job_list.size()), counter);
    pool.wait(counter);
  }
  out.reserve(tasks.size());
  for (TextureTask& task : tasks)
    out.push_back(std::move(task.result));
}

JsonValue textures_json(const Vector<TextureResult>& results, u32 mesh, bool enabled, bool detail) {
  JsonValue out = JsonValue::object();
  out.set("enabled", JsonValue(enabled));
  u32 counts[6] = {0, 0, 0, 0, 0, 0};
  u64 bytes = 0;
  u64 source_bytes = 0;
  f64 build_ms = 0.0;
  JsonValue rows = JsonValue::array();
  for (const TextureResult& r : results) {
    if (mesh != ~u32{0} && r.mesh != mesh) continue;
    ++counts[static_cast<u32>(r.state)];
    if (r.state == TextureState::Built || r.state == TextureState::Skipped) {
      bytes += r.bytes;
      source_bytes += r.source_bytes;
    }
    build_ms += r.build_ms;
    if (!detail) continue;
    JsonValue row = JsonValue::object();
    row.set("image", JsonValue(r.image));
    row.set("status", JsonValue(texture_state_name(r.state)));
    row.set("path", JsonValue(r.path));
    row.set("key", JsonValue(r.key));
    row.set("source_bytes", JsonValue(r.source_bytes));
    if (r.state == TextureState::Built) {
      row.set("format", JsonValue(texture::texture_format_name(r.format)));
      row.set("color_space", JsonValue(texture::color_space_name(r.color_space)));
      row.set("width", JsonValue(r.width));
      row.set("height", JsonValue(r.height));
      row.set("levels", JsonValue(r.levels));
      row.set("bytes", JsonValue(r.bytes));
      row.set("psnr", finite_or_null(r.report.psnr));
      row.set("max_error", JsonValue(r.report.max_error));
      JsonValue ms = JsonValue::object();
      ms.set("decode", JsonValue(r.report.decode_ms));
      ms.set("mips", JsonValue(r.report.mip_ms));
      ms.set("encode", JsonValue(r.report.encode_ms));
      ms.set("total", JsonValue(r.build_ms));
      row.set("ms", std::move(ms));
    } else if (r.state == TextureState::Skipped) {
      row.set("bytes", JsonValue(r.bytes));
    }
    if (!r.error.empty()) row.set("error", JsonValue(r.error));
    rows.push_back(std::move(row));
  }
  out.set("built", JsonValue(counts[static_cast<u32>(TextureState::Built)]));
  out.set("skipped", JsonValue(counts[static_cast<u32>(TextureState::Skipped)]));
  out.set("shared", JsonValue(counts[static_cast<u32>(TextureState::Shared)]));
  out.set("missing", JsonValue(counts[static_cast<u32>(TextureState::Missing)]));
  out.set("failed", JsonValue(counts[static_cast<u32>(TextureState::Failed)]));
  out.set("unreadable", JsonValue(counts[static_cast<u32>(TextureState::Unreadable)]));
  out.set("bytes", JsonValue(bytes));
  out.set("source_bytes", JsonValue(source_bytes));
  out.set("build_ms", JsonValue(build_ms));
  if (detail) out.set("detail", std::move(rows));
  return out;
}

void texture_sources_of_container(const std::string& path, const std::string& source,
                                  Vector<TextureSource>& out) {
  out.clear();
  geometry::ClusterFileReader reader;
  geometry::ClusterFileData resident;
  std::string error;
  if (!reader.open(path, &resident, &error)) {
    ENGINE_LOG_WARN(log_content, "textures not checked", log::field("path", path),
                    log::field("error", error));
    return;
  }
  collect_texture_sources(resident, io::parent_path(source), out);
}

// ---- many meshes -------------------------------------------------------------------------------

void run_mesh_tasks(std::span<MeshTask> tasks, jobs::JobSystem& pool) {
  Vector<jobs::Job> job_list;
  job_list.reserve(static_cast<u32>(tasks.size()));
  jobs::Counter counter;
  for (MeshTask& task : tasks)
    job_list.push_back(jobs::Job{run_mesh_task, &task, &counter});
  if (job_list.empty()) return;
  pool.schedule(jobs::Pool::Performance,
                std::span<const jobs::Job>(job_list.data(), job_list.size()), counter);
  pool.wait(counter);
}

// ---- derived steps ------------------------------------------------------------------------------

bool read_derived_entries(const std::string& manifest, const DerivedStep& step,
                          Vector<DerivedEntry>& out, std::string& error) {
  out.clear();
  std::string text;
  const io::Status status = io::read_file(manifest, text);
  if (status != io::Status::Ok) {
    error = "cannot read manifest '" + manifest + "': " + io::status_name(status);
    return false;
  }
  JsonValue document;
  const JsonParseResult parsed = parse_json(text, document);
  if (!parsed.ok) {
    error = "manifest '" + manifest + "' line " + std::to_string(parsed.line) + " column " +
            std::to_string(parsed.column) + ": " + parsed.message;
    return false;
  }
  const JsonValue* array = document.is_object() ? document.find(step.manifest_array) : nullptr;
  if (array == nullptr) return true;
  if (!array->is_array()) {
    error =
        "manifest '" + manifest + "' has a \"" + step.manifest_array + "\" that is not an array";
    return false;
  }
  const std::string dir(io::parent_path(manifest));
  auto resolve = [&dir](std::string_view relative) {
    return io::is_absolute_path(relative) ? io::normalize_path(relative)
                                          : io::join_path(dir, relative);
  };
  for (usize i = 0; i < array->size(); ++i) {
    const JsonValue& entry = (*array)[i];
    const std::string at =
        "manifest '" + manifest + "' " + step.manifest_array + " entry " + std::to_string(i);
    if (!entry.is_object()) {
      error = at + " is not an object";
      return false;
    }
    const JsonValue* source = entry.find("source");
    if (source == nullptr || !source->is_string()) {
      error = at + " has no \"source\" string";
      return false;
    }
    DerivedEntry built;
    built.source = resolve(source->as_string());
    if (const JsonValue* output = entry.find("output"); output != nullptr) {
      if (!output->is_string()) {
        error = at + " has an \"output\" that is not a string";
        return false;
      }
      built.output = resolve(output->as_string());
    }
    out.push_back(std::move(built));
  }
  return true;
}

void run_derived_task(void* data) {
  DerivedTask& task = *static_cast<DerivedTask*>(data);
  const DerivedStep& step = *task.step;
  const DerivedEntry& entry = *task.entry;
  std::string bytes;
  const io::Status status = io::read_file(entry.source, bytes);
  if (status != io::Status::Ok) {
    task.error.rule = k_rule_source_unreadable;
    task.error.message = "cannot read '" + entry.source + "': " + io::status_name(status);
    task.state = TaskState::Failed;
    return;
  }
  // The identity first, whatever the destination: it addresses the cache entry, and the output
  // records it so that a later build can tell it is still the answer.
  const std::span<const u8> source(reinterpret_cast<const u8*>(bytes.data()), bytes.size());
  task.source_bytes = bytes.size();
  task.source_hash = hash_bytes(source.data(), source.size());
  task.build_key = step.build_key(task.source_hash);
  task.path = entry.output.empty() ? step.cache_path(*task.ddc, task.build_key) : entry.output;

  io::FileInfo info;
  if (!task.force) {
    u64 recorded_source = 0;
    u64 recorded_key = 0;
    if (step.identity(task.path, recorded_source, recorded_key) && recorded_key != 0 &&
        recorded_source == task.source_hash && recorded_key == task.build_key) {
      if (io::stat_file(task.path, info) == io::Status::Ok) task.bytes = info.size;
      task.state = TaskState::Skipped;
      return;
    }
  }
  if (entry.output.empty()) {
    const io::Status made = io::make_directories(io::parent_path(task.path));
    if (made != io::Status::Ok) {
      task.error.rule = k_rule_derived_build;
      task.error.message = "cannot create '" + std::string(io::parent_path(task.path)) +
                           "': " + io::status_name(made);
      task.state = TaskState::Failed;
      return;
    }
  }
  const i64 start_ns = time::monotonic_ns();
  std::string message;
  task.report = JsonValue::object();
  if (!step.build(source, task.source_hash, task.build_key, task.path, task.report, message)) {
    task.error.rule = k_rule_derived_build;
    task.error.message = entry.source + ": " + message;
    task.state = TaskState::Failed;
    return;
  }
  task.build_ms = static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e6;
  if (io::stat_file(task.path, info) == io::Status::Ok) task.bytes = info.size;
  task.state = TaskState::Built;
  ENGINE_LOG_DEBUG(log_content, "derived output built", log::field("step", step.name),
                   log::field("source", entry.source), log::field("path", task.path),
                   log::field("bytes", task.bytes));
}

void run_derived_tasks(std::span<DerivedTask> tasks, jobs::JobSystem& pool) {
  Vector<jobs::Job> job_list;
  job_list.reserve(static_cast<u32>(tasks.size()));
  jobs::Counter counter;
  for (DerivedTask& task : tasks)
    job_list.push_back(jobs::Job{run_derived_task, &task, &counter});
  if (job_list.empty()) return;
  pool.schedule(jobs::Pool::Performance,
                std::span<const jobs::Job>(job_list.data(), job_list.size()), counter);
  pool.wait(counter);
}

}  // namespace engine::content_build
