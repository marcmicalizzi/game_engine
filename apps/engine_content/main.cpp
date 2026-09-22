// engine-content: the content build on the command line (docs/plan/07-content-pipeline.md §7.3).
// `build` imports a glTF 2.0 or GLB file (domain/assets), validates it against the rules of
// §7.4, welds its vertices, builds one cluster LOD DAG per primitive so that every cluster has a
// single material (domain/geometry), merges them, and writes the result as a `.clusters`
// container together with the material table, the image paths the materials name, the encoded
// bytes of the images the source embedded rather than named, the source path those paths are
// relative to, and the source's identity (its content hash and the build key over it and the
// options), which is what makes a second build able to skip the work. The
// per-primitive DAG builds run as jobs on the job system's performance pool, and the merge is
// over the primitives in index order, never in completion order, so the bytes do not depend on
// how many threads ran them.
//
// `build-all` reads a manifest of meshes and builds every entry that is not already up to date,
// one job per mesh. `--cache` puts a build into the derived-data cache, under the hash of the
// source and the build options, which is the path `engine-view --mesh` looks in before it builds
// anything. `info` prints what a container holds; `stats` prints the content-build metrics of a
// container. Every command prints one JSON line per result on stdout, so scripts and agents read
// the numbers without parsing prose; everything else goes through the log to stderr.
//
// Exit codes: 0 ok; 1 a file could not be loaded, validated, built, or written; 2 usage.
#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <core/log/log.h>
#include <core/platform/cpu_baseline.h>
#include <core/platform/process.h>
#include <core/time/time.h>
#include <domain/assets/gltf.h>
#include <domain/geometry/cluster_file.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>

using namespace engine;

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_content, "content");

constexpr int k_exit_ok = 0;
constexpr int k_exit_error = 1;
constexpr int k_exit_usage = 2;

const char* k_usage =
    "usage: engine-content <command> [options]\n"
    "\n"
    "  build <in.gltf|in.glb> <out.clusters>   import a mesh and write a cluster container\n"
    "      --max-triangles <n>   triangles per cluster (4..256, default 124)\n"
    "      --max-vertices <n>    vertices per cluster (1..255, default 64)\n"
    "      --page-bytes <n>      streaming page target in bytes (default 131072); 0 writes no\n"
    "                            page table and leaves the clusters in builder order\n"
    "      --no-weld             keep the file's duplicate vertices\n"
    "      --no-morph            drop the source's morph targets (what does the stream cost?)\n"
    "      --uv-seams <rule>     what a LOD collapse may do at a UV atlas island edge:\n"
    "                            none, protect (default), or lock\n"
    "      --normal-seams <rule> the same at a hard shading edge (default none)\n"
    "      --uv-weight <n>       UV weight in the simplifier's error metric, in thousandths\n"
    "                            (default 500 = 0.5); 0 removes the term\n"
    "      --normal-weight <n>   the same for normals (default 0; 500 costs 2-4x the triangles)\n"
    "      --cache               write into the derived-data cache instead of a named output,\n"
    "                            addressed by the source and the options above\n"
    "      --ddc <dir>           the cache root (default: <repo>/ddc, found beside AGENTS.md)\n"
    "      --jobs <n>            performance-pool workers: one per primitive here, one per\n"
    "                            mesh in build-all (default: the machine's performance CPUs)\n"
    "      --strict              treat validation warnings as errors\n"
    "      --log <spec>          log levels, e.g. \"info\" or \"warn,content=debug\"\n"
    "  build-all <manifest.json>               build every mesh a manifest names\n"
    "      --cache               entries with no \"output\" go to the derived-data cache\n"
    "      --page-bytes <n>      the default for entries whose \"options\" do not say\n"
    "      --ddc <dir>, --jobs <n>, --strict, --log <spec>   as above\n"
    "  info <file.clusters>                    print the header, sections, and counts\n"
    "  stats <file.clusters>                   print the content-build metrics of a container\n"
    "\n"
    "the build-all manifest:\n"
    "  {\"meshes\":[{\"source\":\"a.gltf\",\"output\":\"a.clusters\",\n"
    "               \"options\":{\"max_triangles\":124,\"max_vertices\":64,\"weld\":true,\n"
    "                           \"page_bytes\":131072}}]}\n"
    "  paths are relative to the manifest file; \"output\" and \"options\" are optional.\n"
    "\n"
    "examples:\n"
    "  engine-content build content/samples/Suzanne/Suzanne.gltf ddc/suzanne.clusters\n"
    "  engine-content build content/samples/Suzanne/Suzanne.gltf --cache\n"
    "  engine-content build-all content/meshes.json --cache --jobs 8\n"
    "  engine-content info ddc/suzanne.clusters\n"
    "  engine-content stats ddc/suzanne.clusters\n";

int usage(const char* message) {
  if (message != nullptr) std::fprintf(stderr, "engine-content: %s\n", message);
  std::fputs(k_usage, stderr);
  return k_exit_usage;
}

int failed(const std::string& message) {
  std::fprintf(stderr, "engine-content: %s\n", message.c_str());
  return k_exit_error;
}

void print_json(const JsonValue& value) {
  std::string text = write_json(value, JsonWriteOptions{.pretty = false});
  text.push_back('\n');
  std::fwrite(text.data(), 1, text.size(), stdout);
}

// Routes the log to stderr: warnings and above by default, whatever `--log` asks for otherwise.
void start_logging(log::StreamSink& sink, const std::string& spec) {
  sink.set_min_level(log::Level::Warn);
  log::add_sink(&sink);
  if (spec.empty()) return;
  sink.set_min_level(log::Level::Trace);
  log::apply_level_spec("warn");
  log::apply_level_spec(spec);
}

bool next_value(int argc, char** argv, int& i, std::string& out) {
  if (i + 1 >= argc) {
    std::fprintf(stderr, "engine-content: %s needs a value\n", argv[i]);
    return false;
  }
  out = argv[++i];
  return true;
}

bool next_u32(int argc, char** argv, int& i, u32& out) {
  std::string text;
  if (!next_value(argc, argv, i, text)) return false;
  u64 value = 0;
  bool digits = false;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      digits = false;
      break;
    }
    digits = true;
    value = value * 10 + static_cast<u64>(c - '0');
    if (value > 0xffffffffull) value = 0xffffffffull;
  }
  if (!digits) {
    std::fprintf(stderr, "engine-content: '%s' is not a number\n", text.c_str());
    return false;
  }
  out = static_cast<u32>(value);
  return true;
}

// A non-negative weight, in thousandths of a unit, so the parser stays the integer one above and
// a weight on the command line is still readable: `--uv-weight 500` is 0.5.
bool next_weight(int argc, char** argv, int& i, f32& out) {
  u32 thousandths = 0;
  if (!next_u32(argc, argv, i, thousandths)) return false;
  out = static_cast<f32>(thousandths) / 1000.0f;
  return true;
}

// ---- what a build is asked for ---------------------------------------------------------------

// The per-mesh knobs, which are also what the cache key is taken over, so that the key and the
// build agree by construction.
struct MeshOptions {
  u32 max_triangles = geometry::ClusterLodOptions{}.max_triangles;
  u32 max_vertices = geometry::ClusterLodOptions{}.max_vertices;
  // The streaming page target (docs/plan/04-renderer.md §4.3 step 3); 0 writes a container with
  // no page table. It is in the cache key because laying a mesh out in pages renumbers its
  // clusters, so two targets are two different containers.
  u32 page_bytes = geometry::ClusterPagesOptions{}.page_bytes;
  bool weld = true;
  // What the LOD simplifier is given beyond the positions (docs/subsystems/geometry.md, "What
  // the simplifier is given, and why"). They are on the command line rather than fixed because
  // the seam rule is the difference between a mesh whose coarse levels paint the right part of
  // the atlas and one whose coarse levels do not, and the only honest way to say what it costs
  // is to build both containers and compare them — which is a flag, not a rebuild.
  f32 normal_weight = geometry::ClusterLodOptions{}.normal_weight;
  f32 uv_weight = geometry::ClusterLodOptions{}.uv_weight;
  geometry::SeamRule uv_seams = geometry::ClusterLodOptions{}.uv_seams;
  geometry::SeamRule normal_seams = geometry::ClusterLodOptions{}.normal_seams;
  // `build --no-morph`: drop the source's morph targets before anything else sees them, so the
  // container is the one the same file would give with no channels — welded without the deltas in
  // the key, simplified without `morph_seams`, and with the seven morph sections empty. It exists
  // to answer "what does the morph stream cost this asset?" as a pair of command lines, the way
  // the seam rules do; the recorded build key says it (see `morph_key`), so a container built
  // without channels is never mistaken for the answer to a build that wanted them.
  bool morph = true;
};

// The build key of a container built with `--no-morph`: the ordinary key, moved by a constant
// through one SplitMix64 round. `geometry::cluster_cache_key` has no morph term because nothing
// else builds without the channels; this keeps such a container from recording the key a full
// build would record, which is what `build-all` skips on.
u64 morph_key(u64 key, const MeshOptions& options) noexcept {
  if (options.morph) return key;
  u64 z = key ^ 0x6e6f2d6d6f727068ull;  // "no-morph"
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
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

// ---- validation (docs/plan/07-content-pipeline.md §7.4) ---------------------------------------
//
// Rules carry a stable id so that a diagnostic is machine-readable, which is what §7.3 asks of a
// build failure: an asset, a rule, a severity, and a sentence. An error stops the build at the
// first one; a warning is a log record, and `--strict` turns it into an error.

constexpr const char* k_rule_empty_primitive = "geometry.empty_primitive";
constexpr const char* k_rule_index_range = "geometry.index_range";
constexpr const char* k_rule_nan_position = "geometry.nan_position";
constexpr const char* k_rule_degenerate = "geometry.degenerate_triangle";
constexpr const char* k_rule_cluster_budget = "geometry.cluster_budget";
constexpr const char* k_rule_uv_range = "geometry.uv_range";
constexpr const char* k_rule_missing_image = "material.missing_image";

// A UV that far outside the unit square is a broken unwrap or a unit mix-up, not tiling: 16
// wraps is already a texel density no texture pipeline can serve. A warning, not an error,
// because tiling trim sheets legitimately run to a few wraps.
constexpr f32 k_uv_limit = 16.0f;

// The visibility buffer packs `cluster << 8 | triangle` into 32 bits (AGENTS.md, ADR-0005), so a
// container with more clusters than this cannot be drawn whatever else is right about it.
constexpr u64 k_max_clusters = 1ull << 24;

struct Diagnostic {
  const char* rule = "";
  std::string message;
};

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

// ---- building one mesh ------------------------------------------------------------------------

struct BuildResult {
  std::string path;
  bool cached = false;
  u64 source_hash = 0;
  u64 build_key = 0;
  u32 clusters = 0;
  u32 leaf_clusters = 0;
  u32 triangles = 0;
  u32 lod_levels = 0;
  u32 vertices = 0;
  u32 pages = 0;
  u32 page_bytes = 0;   // the target the layout was given, 0 when there is no page table
  f64 page_fill = 0.0;  // mean page bytes over the target
  u32 materials = 0;
  u32 images = 0;
  u32 embedded_images = 0;      // of those, how many the container carries the bytes of
  u32 deduplicated_images = 0;  // slots served by an earlier image's identical bytes
  u64 image_bytes = 0;          // what the distinct embedded images take in the container
  u64 bytes = 0;
  u64 hash = 0;
  f64 build_ms = 0.0;
  Vector<Diagnostic> warnings;
};

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

  const u32 loaded_vertices = mesh.positions.size();
  u32 vertices = loaded_vertices;
  if (options.weld) {
    // The skin bindings are part of the weld key when the source has them, so two coincident
    // vertices with different weights stay two vertices — merging them would hand one surface the
    // other's deformation, before clustering and therefore beyond noticing
    // ([geometry](geometry.md), "Skinned meshes"). `engine-view --mesh` welds identically, because
    // the two apps share cache entries and what one writes the other has to find.
    vertices = geometry::weld_vertices(mesh.positions, mesh.normals, mesh.uvs,
                                       std::span<u32>(mesh.indices.data(), mesh.indices.size()),
                                       mesh.skin_bindings.empty() ? nullptr : &mesh.skin_bindings);
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
  out.bytes = io::stat_file(output, info) == io::Status::Ok ? info.size : 0;
  out.hash = geometry::cluster_file_hash(data);
  out.build_ms = static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e6;

  ENGINE_LOG_INFO(log_content, "cluster file written", log::field("path", output),
                  log::field("clusters", out.clusters), log::field("pages", out.pages),
                  log::field("bytes", out.bytes), log::field("build_ms", out.build_ms));
  return true;
}

void log_warnings(const std::string& source, const Vector<Diagnostic>& warnings) {
  for (const Diagnostic& warning : warnings) {
    ENGINE_LOG_WARN(log_content, "validation warning", log::field("source", source),
                    log::field("rule", warning.rule), log::field("detail", warning.message));
  }
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

// ---- build ------------------------------------------------------------------------------------

struct BuildCommandOptions {
  std::string input;
  std::string output;
  std::string log_spec;
  std::string ddc;
  MeshOptions mesh;
  u32 jobs = 0;
  bool cache = false;
  bool strict = false;
};

int build(const BuildCommandOptions& options) {
  // The source's identity comes first whatever the destination is: it addresses the cache entry,
  // and it is written into the container so that a later build can tell it is still the answer.
  u64 source_hash = 0;
  std::string message;
  if (!assets::source_mesh_hash(options.input, source_hash, &message)) return failed(message);
  const u64 key = morph_key(geometry::cluster_cache_key(source_hash, lod_options_of(options.mesh),
                                                        options.mesh.weld, options.mesh.page_bytes),
                            options.mesh);

  std::string output = options.output;
  if (options.cache) {
    output = geometry::cluster_cache_path(options.ddc, key);
    const io::Status status = io::make_directories(io::parent_path(output));
    if (status != io::Status::Ok) {
      return failed("cannot create the cache directory '" + std::string(io::parent_path(output)) +
                    "': " + io::status_name(status));
    }
  }

  jobs::JobSystem pool(job_config(options.jobs));
  BuildResult result;
  Diagnostic error;
  const bool ok = build_one(options.input, output, options.mesh, options.strict, &pool, source_hash,
                            key, result, error);
  log_warnings(options.input, result.warnings);
  if (!ok) return failed(std::string(error.rule) + ": " + error.message);

  JsonValue summary = JsonValue::object();
  summary.set("path", JsonValue(result.path));
  summary.set("cached", JsonValue(options.cache));
  summary.set("source_hash", JsonValue(result.source_hash));
  summary.set("build_key", JsonValue(result.build_key));
  summary.set("clusters", JsonValue(result.clusters));
  summary.set("leaf_clusters", JsonValue(result.leaf_clusters));
  summary.set("triangles", JsonValue(result.triangles));
  summary.set("lod_levels", JsonValue(result.lod_levels));
  summary.set("vertices", JsonValue(result.vertices));
  summary.set("pages", JsonValue(result.pages));
  summary.set("page_bytes", JsonValue(result.page_bytes));
  summary.set("page_fill", JsonValue(result.page_fill));
  summary.set("materials", JsonValue(result.materials));
  summary.set("images", JsonValue(result.images));
  summary.set("embedded_images", JsonValue(result.embedded_images));
  summary.set("deduplicated_images", JsonValue(result.deduplicated_images));
  summary.set("image_bytes", JsonValue(result.image_bytes));
  summary.set("warnings", JsonValue(result.warnings.size()));
  summary.set("bytes", JsonValue(result.bytes));
  summary.set("build_ms", JsonValue(result.build_ms));
  summary.set("hash", JsonValue(result.hash));
  print_json(summary);
  return k_exit_ok;
}

int build_command(int argc, char** argv) {
  BuildCommandOptions options;
  Vector<std::string> positional;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--max-triangles") {
      if (!next_u32(argc, argv, i, options.mesh.max_triangles)) return k_exit_usage;
    } else if (a == "--max-vertices") {
      if (!next_u32(argc, argv, i, options.mesh.max_vertices)) return k_exit_usage;
    } else if (a == "--page-bytes") {
      if (!next_u32(argc, argv, i, options.mesh.page_bytes)) return k_exit_usage;
    } else if (a == "--normal-weight") {
      if (!next_weight(argc, argv, i, options.mesh.normal_weight)) return k_exit_usage;
    } else if (a == "--uv-weight") {
      if (!next_weight(argc, argv, i, options.mesh.uv_weight)) return k_exit_usage;
    } else if (a == "--uv-seams" || a == "--normal-seams") {
      std::string rule;
      if (!next_value(argc, argv, i, rule)) return k_exit_usage;
      geometry::SeamRule& target =
          a == "--uv-seams" ? options.mesh.uv_seams : options.mesh.normal_seams;
      if (!read_seam_rule(rule, target)) return usage("a seam rule is none, protect, or lock");
    } else if (a == "--jobs") {
      if (!next_u32(argc, argv, i, options.jobs)) return k_exit_usage;
    } else if (a == "--log") {
      if (!next_value(argc, argv, i, options.log_spec)) return k_exit_usage;
    } else if (a == "--ddc") {
      if (!next_value(argc, argv, i, options.ddc)) return k_exit_usage;
    } else if (a == "--no-weld") {
      options.mesh.weld = false;
    } else if (a == "--no-morph") {
      options.mesh.morph = false;
    } else if (a == "--cache") {
      options.cache = true;
    } else if (a == "--strict") {
      options.strict = true;
    } else if (!a.empty() && a[0] == '-') {
      return usage("unknown option for build");
    } else {
      positional.push_back(std::string(a));
    }
  }
  // The output is a path or the cache, never both: two destinations for one build would leave
  // a caller guessing which one the summary names.
  if (positional.size() != (options.cache ? 1u : 2u)) {
    return usage(options.cache ? "build --cache takes an input mesh and no output file"
                               : "build takes an input mesh and an output file");
  }
  options.input = positional[0];
  if (!options.cache) options.output = positional[1];
  if (options.cache && options.ddc.empty()) {
    options.ddc = geometry::find_ddc_root(platform::executable_directory());
    if (options.ddc.empty())
      return usage("--cache found no repository root above the executable; pass --ddc <dir>");
  }
  // The cluster format's own limits; the builder rejects the rest with its own message.
  if (!options_in_range(options.mesh))
    return usage("--max-triangles is 4..256 and --max-vertices is 1..255");
  if (options.jobs > 4096) return usage("--jobs is 1..4096, or absent for one per CPU");

  log::StreamSink stderr_sink(stderr, log::StreamSink::Format::Text);
  start_logging(stderr_sink, options.log_spec);
  const int code = build(options);
  log::remove_sink(&stderr_sink);
  return code;
}

// ---- build-all ----------------------------------------------------------------------------------

struct ManifestEntry {
  std::string source;  // resolved against the manifest's directory
  std::string output;  // likewise; empty means "the derived-data cache"
  MeshOptions options;
};

// Reads the manifest and resolves every path against the manifest's own directory, so a manifest
// is movable as a unit and says the same thing from any working directory.
bool read_manifest(const std::string& path, const MeshOptions& defaults, Vector<ManifestEntry>& out,
                   std::string& error) {
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
      if (!options_in_range(built.options)) {
        error = at + ": max_triangles is 4..256 and max_vertices is 1..255";
        return false;
      }
    }
    out.push_back(std::move(built));
  }
  return true;
}

enum class TaskState : u8 { Built, Skipped, Failed };

// One manifest entry in flight. Everything a job writes lives here, and the printing walks the
// tasks in manifest order afterwards, so neither the output files nor the output lines depend on
// which job finished first.
struct MeshTask {
  const ManifestEntry* entry = nullptr;
  const std::string* ddc = nullptr;
  bool strict = false;
  TaskState state = TaskState::Failed;
  BuildResult result;
  Diagnostic error;
};

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
  const u64 key = geometry::cluster_cache_key(source_hash, lod_options_of(entry.options),
                                              entry.options.weld, entry.options.page_bytes);
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
  if (io::exists(output) && geometry::read_cluster_file_identity(output, had_hash, had_key) &&
      had_key != 0 && had_key == key && had_hash == source_hash) {
    io::FileInfo info;
    task.result.bytes = io::stat_file(output, info) == io::Status::Ok ? info.size : 0;
    task.state = TaskState::Skipped;
    return;
  }
  // The per-primitive jobs are not nested inside this one: the manifest is the coarser level and
  // already fills the pool, and one wait per mesh keeps the helper stack shallow.
  task.state = build_one(entry.source, output, entry.options, task.strict, nullptr, source_hash,
                         key, task.result, task.error)
                   ? TaskState::Built
                   : TaskState::Failed;
}

struct BuildAllCommandOptions {
  std::string manifest;
  std::string log_spec;
  std::string ddc;
  MeshOptions defaults;  // what an entry's "options" object does not override
  u32 jobs = 0;
  bool cache = false;
  bool strict = false;
};

int build_all(const BuildAllCommandOptions& options) {
  const i64 start_ns = time::monotonic_ns();
  Vector<ManifestEntry> entries;
  std::string message;
  if (!read_manifest(options.manifest, options.defaults, entries, message)) return failed(message);
  for (const ManifestEntry& entry : entries) {
    if (entry.output.empty() && !options.cache) {
      return failed("manifest '" + options.manifest + "' entry '" + entry.source +
                    "' has no \"output\" and --cache was not given");
    }
  }
  if (options.cache) {
    const std::string dir = io::join_path(options.ddc, "clusters");
    const io::Status status = io::make_directories(dir);
    if (status != io::Status::Ok)
      return failed("cannot create the cache directory '" + dir + "': " + io::status_name(status));
  }

  Vector<MeshTask> tasks;
  tasks.reserve(entries.size());
  for (const ManifestEntry& entry : entries) {
    MeshTask task;
    task.entry = &entry;
    task.ddc = &options.ddc;
    task.strict = options.strict;
    tasks.push_back(std::move(task));
  }

  {
    jobs::JobSystem pool(job_config(options.jobs));
    Vector<jobs::Job> job_list;
    job_list.reserve(tasks.size());
    jobs::Counter counter;
    for (MeshTask& task : tasks)
      job_list.push_back(jobs::Job{run_mesh_task, &task, &counter});
    if (!job_list.empty()) {
      pool.schedule(jobs::Pool::Performance,
                    std::span<const jobs::Job>(job_list.data(), job_list.size()), counter);
      pool.wait(counter);
    }
  }

  u32 built = 0;
  u32 skipped = 0;
  u32 failures = 0;
  for (const MeshTask& task : tasks) {
    log_warnings(task.entry->source, task.result.warnings);
    JsonValue line = JsonValue::object();
    line.set("source", JsonValue(task.entry->source));
    line.set("path", JsonValue(task.result.path));
    switch (task.state) {
      case TaskState::Built: {
        ++built;
        line.set("status", JsonValue("built"));
        line.set("cached", JsonValue(task.result.cached));
        line.set("source_hash", JsonValue(task.result.source_hash));
        line.set("build_key", JsonValue(task.result.build_key));
        line.set("clusters", JsonValue(task.result.clusters));
        line.set("leaf_clusters", JsonValue(task.result.leaf_clusters));
        line.set("triangles", JsonValue(task.result.triangles));
        line.set("lod_levels", JsonValue(task.result.lod_levels));
        line.set("vertices", JsonValue(task.result.vertices));
        line.set("pages", JsonValue(task.result.pages));
        line.set("page_bytes", JsonValue(task.result.page_bytes));
        line.set("page_fill", JsonValue(task.result.page_fill));
        line.set("materials", JsonValue(task.result.materials));
        line.set("images", JsonValue(task.result.images));
        line.set("embedded_images", JsonValue(task.result.embedded_images));
        line.set("deduplicated_images", JsonValue(task.result.deduplicated_images));
        line.set("image_bytes", JsonValue(task.result.image_bytes));
        line.set("warnings", JsonValue(task.result.warnings.size()));
        line.set("bytes", JsonValue(task.result.bytes));
        line.set("build_ms", JsonValue(task.result.build_ms));
        line.set("hash", JsonValue(task.result.hash));
        break;
      }
      case TaskState::Skipped: {
        ++skipped;
        line.set("status", JsonValue("skipped"));
        line.set("cached", JsonValue(task.result.cached));
        line.set("source_hash", JsonValue(task.result.source_hash));
        line.set("build_key", JsonValue(task.result.build_key));
        line.set("bytes", JsonValue(task.result.bytes));
        break;
      }
      case TaskState::Failed: {
        ++failures;
        line.set("status", JsonValue("failed"));
        line.set("rule", JsonValue(task.error.rule));
        line.set("error", JsonValue(task.error.message));
        std::fprintf(stderr, "engine-content: %s: %s\n", task.error.rule,
                     task.error.message.c_str());
        break;
      }
    }
    print_json(line);
  }

  JsonValue summary = JsonValue::object();
  summary.set("built", JsonValue(built));
  summary.set("skipped", JsonValue(skipped));
  summary.set("failed", JsonValue(failures));
  summary.set("seconds", JsonValue(static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e9));
  print_json(summary);
  return failures == 0 ? k_exit_ok : k_exit_error;
}

int build_all_command(int argc, char** argv) {
  BuildAllCommandOptions options;
  Vector<std::string> positional;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--jobs") {
      if (!next_u32(argc, argv, i, options.jobs)) return k_exit_usage;
    } else if (a == "--page-bytes") {
      if (!next_u32(argc, argv, i, options.defaults.page_bytes)) return k_exit_usage;
    } else if (a == "--log") {
      if (!next_value(argc, argv, i, options.log_spec)) return k_exit_usage;
    } else if (a == "--ddc") {
      if (!next_value(argc, argv, i, options.ddc)) return k_exit_usage;
    } else if (a == "--cache") {
      options.cache = true;
    } else if (a == "--strict") {
      options.strict = true;
    } else if (!a.empty() && a[0] == '-') {
      return usage("unknown option for build-all");
    } else {
      positional.push_back(std::string(a));
    }
  }
  if (positional.size() != 1) return usage("build-all takes one manifest file");
  options.manifest = positional[0];
  if (options.cache && options.ddc.empty()) {
    options.ddc = geometry::find_ddc_root(platform::executable_directory());
    if (options.ddc.empty())
      return usage("--cache found no repository root above the executable; pass --ddc <dir>");
  }
  if (options.jobs > 4096) return usage("--jobs is 1..4096, or absent for one per CPU");

  log::StreamSink stderr_sink(stderr, log::StreamSink::Format::Text);
  start_logging(stderr_sink, options.log_spec);
  const int code = build_all(options);
  log::remove_sink(&stderr_sink);
  return code;
}

// ---- info and stats -----------------------------------------------------------------------------

// Reads a container and hands back its bytes as well, so a caller can walk the section table
// straight from the file rather than from what this build understands of it.
bool load_container(const std::string& path, std::string& file, geometry::ClusterFileHeader& header,
                    Vector<geometry::ClusterFileSection>& sections, geometry::ClusterFileData& data,
                    std::string& error) {
  const io::Status status = io::read_file(path, file);
  if (status != io::Status::Ok) {
    error = "cannot read '" + path + "': " + io::status_name(status);
    return false;
  }
  const std::span<const u8> bytes(reinterpret_cast<const u8*>(file.data()), file.size());
  if (!geometry::read_cluster_file_memory(bytes, data, &error)) return false;
  std::memcpy(&header, file.data(), sizeof(header));
  sections.resize(header.section_count);
  for (u32 i = 0; i < header.section_count; ++i) {
    std::memcpy(&sections[i], file.data() + sizeof(header) + sizeof(sections[i]) * i,
                sizeof(sections[i]));
  }
  return true;
}

// The page table at a glance, for `info` and as the head of `stats`'s page section: how many
// pages, how full they are, how much of the mesh is pinned (the pages holding a group with no
// coarser version, which the residency manager never evicts), and how connected they are.
JsonValue page_summary(const geometry::ClusterPages& pages) {
  JsonValue out = JsonValue::object();
  out.set("count", JsonValue(pages.pages.size()));
  out.set("bytes_target", JsonValue(pages.page_bytes_target));
  u64 total = 0;
  u32 largest = 0;
  u32 oversized = 0;
  u32 root = 0;
  for (const geometry::ClusterPageDesc& page : pages.pages) {
    total += page.bytes;
    largest = page.bytes > largest ? page.bytes : largest;
    if ((page.flags & geometry::k_page_oversized) != 0) ++oversized;
    if ((page.flags & geometry::k_page_root) != 0) ++root;
  }
  out.set("bytes", JsonValue(total));
  out.set("largest_bytes", JsonValue(largest));
  const f64 capacity =
      static_cast<f64>(pages.pages.size()) * static_cast<f64>(pages.page_bytes_target);
  out.set("mean_fill", JsonValue(capacity > 0.0 ? static_cast<f64>(total) / capacity : 0.0));
  out.set("oversized", JsonValue(oversized));
  out.set("root_pages", JsonValue(root));
  out.set("child_page_entries", JsonValue(pages.child_pages.size()));
  return out;
}

int info(const std::string& path) {
  std::string file;
  geometry::ClusterFileHeader header;
  Vector<geometry::ClusterFileSection> records;
  geometry::ClusterFileData data;
  std::string error;
  if (!load_container(path, file, header, records, data, error)) return failed(error);

  // The table is read straight from the bytes, so `info` reports the sections a newer build
  // wrote as well as the ones this build understands.
  JsonValue sections = JsonValue::array();
  for (const geometry::ClusterFileSection& section : records) {
    JsonValue entry = JsonValue::object();
    entry.set("kind", JsonValue(section.kind));
    entry.set("name", JsonValue(geometry::cluster_section_name(section.kind)));
    entry.set("element_size", JsonValue(section.element_size));
    entry.set("element_count", JsonValue(section.element_count));
    entry.set("offset", JsonValue(section.offset));
    sections.push_back(std::move(entry));
  }

  const geometry::ClusterLodMesh& lod = data.mesh;
  JsonValue summary = JsonValue::object();
  summary.set("path", JsonValue(path));
  summary.set("pages", page_summary(data.pages));
  summary.set("version", JsonValue(header.version));
  summary.set("flags", JsonValue(header.flags));
  summary.set("total_bytes", JsonValue(header.total_bytes));
  summary.set("hash", JsonValue(header.content_hash));
  summary.set("sections", std::move(sections));
  summary.set("clusters", JsonValue(lod.mesh.clusters.size()));
  summary.set("leaf_clusters",
              JsonValue(lod.level_cluster_counts.empty() ? 0u : lod.level_cluster_counts[0]));
  summary.set("triangles", JsonValue(lod.leaf_triangle_count));
  summary.set("lod_levels", JsonValue(lod.level_cluster_counts.size()));
  summary.set("vertices", JsonValue(lod.mesh.vertices.size()));
  summary.set("groups", JsonValue(lod.group_count));
  summary.set("materials", JsonValue(data.materials.size()));
  const geometry::ClusterImageSummary images = geometry::summarize_cluster_images(data);
  summary.set("images", JsonValue(images.count));
  summary.set("embedded_images", JsonValue(images.embedded));
  summary.set("deduplicated_images", JsonValue(images.deduplicated));
  summary.set("image_bytes", JsonValue(images.bytes));
  summary.set("morph_channels", JsonValue(lod.mesh.morph_channels.size()));
  summary.set("morph_deltas", JsonValue(lod.mesh.morph_delta_count));
  summary.set("source_path", JsonValue(data.source_path));
  summary.set("source_hash", JsonValue(data.source_hash));
  summary.set("build_key", JsonValue(data.build_key));
  print_json(summary);
  return k_exit_ok;
}

// ---- the streaming sweep ----------------------------------------------------------------------
//
// What the page layout costs a viewer that has to fetch it: a camera flies in from 50 mesh radii
// to half a radius, and at every step the sweep reports what the ideal cut needs and what a
// budgeted residency manager (docs/plan/04-renderer.md §4.9) actually asks for. It is a content
// metric, not a benchmark: it answers "does the way this mesh is paged make a fly-in cheap?"
// without a GPU, a window, or a frame loop.

constexpr u32 k_sweep_steps = 32;
constexpr f32 k_sweep_far = 50.0f;    // in mesh radii
constexpr f32 k_sweep_near = 0.5f;    // inside the mesh's own sphere: full detail
constexpr f64 k_sweep_budget = 0.25;  // of the mesh's total page bytes

struct SweepStep {
  f32 distance = 0.0f;  // in mesh radii
  u32 clusters = 0;     // the cut with everything resident
  u32 triangles = 0;
  u32 pages_needed = 0;  // distinct pages that cut draws from
  u32 requested = 0;     // pages this step asked for, under the budget
  u32 drawn = 0;         // clusters the budgeted viewer actually drew
  u32 evicted = 0;       // pages the budget took back this step
  u32 resident = 0;      // pages held at the end of the step
  u64 resident_bytes = 0;
};

// The bounding sphere of the mesh's original geometry, which is what "radii" measures.
void leaf_bounds(const geometry::ClusterLodMesh& lod, Vec3& center, f32& radius) {
  Vec3 lo{1e30f, 1e30f, 1e30f};
  Vec3 hi{-1e30f, -1e30f, -1e30f};
  for (u32 i = 0; i < lod.mesh.clusters.size(); ++i) {
    if (lod.lod[i].level != 0) continue;
    const geometry::ClusterDesc& c = lod.mesh.clusters[i];
    lo = Vec3{std::min(lo.x, c.center.x - c.radius), std::min(lo.y, c.center.y - c.radius),
              std::min(lo.z, c.center.z - c.radius)};
    hi = Vec3{std::max(hi.x, c.center.x + c.radius), std::max(hi.y, c.center.y + c.radius),
              std::max(hi.z, c.center.z + c.radius)};
  }
  center = (lo + hi) * 0.5f;
  radius = 1e-6f;
  for (u32 i = 0; i < lod.mesh.clusters.size(); ++i) {
    if (lod.lod[i].level != 0) continue;
    const geometry::ClusterDesc& c = lod.mesh.clusters[i];
    radius = std::max(radius, length(c.center - center) + c.radius);
  }
}

bool sweep_streaming(const geometry::ClusterFileData& data, Vector<SweepStep>& out, u64& budget,
                     std::string& error) {
  out.clear();
  budget = 0;
  const geometry::ClusterPages& pages = data.pages;
  if (pages.pages.empty()) return true;
  u64 total = 0;
  for (const geometry::ClusterPageDesc& page : pages.pages)
    total += page.bytes;
  budget = static_cast<u64>(static_cast<f64>(total) * k_sweep_budget);

  Vec3 center{};
  f32 radius = 1.0f;
  leaf_bounds(data.mesh, center, radius);

  geometry::PageResidency everything;
  everything.resident.assign(pages.pages.size(), u8{1});
  geometry::PageResidencyManager manager;
  if (!manager.reset(pages, budget, &error)) return false;

  geometry::LodView view;
  view.znear = 0.1f;
  // 1080p at 60 degrees, one pixel of error: the same view the LOD tests use.
  view.proj_scale = 1.0f / std::tan(radians(60.0f) * 0.5f) * 1080.0f * 0.5f;
  view.threshold_px = 1.0f;

  Vector<u32> cut;
  Vector<u32> budgeted;
  Vector<u32> requests;
  Vector<u8> seen(pages.pages.size(), u8{0});
  for (u32 step = 0; step < k_sweep_steps; ++step) {
    const f32 t = static_cast<f32>(step) / static_cast<f32>(k_sweep_steps - 1);
    SweepStep entry;
    entry.distance = k_sweep_far + (k_sweep_near - k_sweep_far) * t;
    view.camera = center + Vec3{0.0f, 0.0f, entry.distance * radius};

    // What the mesh would draw with every page in memory, and how many pages that touches.
    cut.clear();
    requests.clear();
    entry.clusters =
        geometry::select_lod_streaming(data.mesh, view, pages, everything, cut, requests);
    for (u8& flag : seen)
      flag = 0;
    for (const u32 c : cut) {
      entry.triangles += data.mesh.mesh.clusters[c].triangle_count;
      u8& flag = seen[pages.page_of_cluster[c]];
      if (flag == 0) ++entry.pages_needed;
      flag = 1;
    }

    // And what a viewer under a quarter of the bytes asks for, one frame per step.
    manager.begin_frame();
    budgeted.clear();
    requests.clear();
    entry.drawn = geometry::select_lod_streaming(data.mesh, view, pages, manager.page_residency(),
                                                 budgeted, requests);
    for (const u32 c : budgeted)
      manager.touch(pages.page_of_cluster[c]);
    entry.requested = requests.size();
    manager.request(std::span<const u32>(requests.data(), requests.size()));
    manager.admit(~u32{0});
    entry.evicted = manager.evict_to_budget();
    entry.resident = manager.resident_pages();
    entry.resident_bytes = manager.resident_bytes();
    out.push_back(entry);
  }
  return true;
}

// The sweep as a table on stderr, so a human reads it and stdout stays one JSON line.
void print_sweep(const Vector<SweepStep>& sweep, u64 budget) {
  if (sweep.empty()) return;
  std::fprintf(stderr, "  streaming sweep, 1080p at 1 px, budget %llu bytes (25%%)\n",
               static_cast<unsigned long long>(budget));
  std::fprintf(stderr, "  %6s %9s %10s %7s %10s %8s %8s %9s %9s\n", "radii", "clusters",
               "triangles", "pages", "requested", "drawn", "evicted", "resident", "KB");
  for (const SweepStep& step : sweep) {
    std::fprintf(stderr, "  %6.2f %9u %10u %7u %10u %8u %8u %9u %9llu\n",
                 static_cast<double>(step.distance), step.clusters, step.triangles,
                 step.pages_needed, step.requested, step.drawn, step.evicted, step.resident,
                 static_cast<unsigned long long>(step.resident_bytes / 1024));
  }
}

// ---- the atlas, and what LOD can do with it ----------------------------------------------------
//
// How fragmented a UV atlas is decides how far a mesh can be simplified at all, because the LOD
// builder is forbidden to collapse across an island edge (docs/subsystems/geometry.md, "What the
// simplifier is given, and why"). Before that rule existed the question did not arise and the
// builder simply produced the wrong picture; now it is a **cost** the asset controls, which makes
// it something a content build has to report. These are the numbers §7.4's atlas-fragmentation
// validator will threshold.
//
// An island is a connected component of the level-0 triangles over *source vertex indices*, which
// is exactly right because the weld keys on the UV: the two sides of an island edge are different
// source vertices, and only they are. Areas are in UV space, reported in texels of a 4096 atlas
// because that is the unit an author reasons in and the unit "smaller than a triangle" is decided
// in.
struct AtlasStats {
  u32 islands = 0;
  u32 source_vertices = 0;    // distinct source vertices the clusters reference
  u32 seam_vertices = 0;      // of those, ones sharing a position with a different-UV vertex
  f64 uv_area = 0.0;          // total, in UV units (1.0 is the whole atlas once)
  f64 smallest_texels = 0.0;  // the smallest island's area, in texels of a 4096 atlas
  f64 median_texels = 0.0;
  f64 largest_texels = 0.0;
  u32 smallest_triangles = 0;
  bool valid = false;
};

u32 find_root(Vector<u32>& parent, u32 v) {
  while (parent[v] != v) {
    parent[v] = parent[parent[v]];  // path halving; the union-find is over source vertices
    v = parent[v];
  }
  return v;
}

AtlasStats measure_atlas(const geometry::ClusterLodMesh& lod) {
  AtlasStats out;
  const geometry::ClusterMesh& mesh = lod.mesh;
  if (mesh.attributes.size() != mesh.vertices.size() || mesh.vertex_source.empty()) return out;
  u32 source_count = 0;
  for (const u32 source : mesh.vertex_source)
    source_count = source >= source_count ? source + 1 : source_count;
  if (source_count == 0) return out;

  // One position and one UV per source vertex, taken from any cluster that carries it: every
  // cluster's copy of a source vertex holds the same bytes, which is the invariant the weld and
  // `fill_cluster_attributes` maintain and `validate_clusters` checks.
  Vector<Vec3> position(source_count, Vec3{});
  Vector<Vec2> uv(source_count, Vec2{});
  Vector<u8> present(source_count, u8{0});
  for (u32 v = 0; v < mesh.vertices.size(); ++v) {
    const u32 source = mesh.vertex_source[v];
    position[source] = mesh.vertices[v];
    uv[source] = geometry::decode_half2(mesh.attributes[v].uv_half2);
    present[source] = 1u;
  }
  for (const u8 seen : present)
    out.source_vertices += seen;

  // Islands: union the three corners of every level-0 triangle.
  Vector<u32> parent(source_count);
  for (u32 i = 0; i < source_count; ++i)
    parent[i] = i;
  Vector<f64> area(source_count, 0.0);  // accumulated on the component's root at the end
  Vector<u32> triangles(source_count, 0u);
  Vector<u32> corners;
  corners.reserve(mesh.triangles.size() * 3);
  for (u32 c = 0; c < mesh.clusters.size(); ++c) {
    if (lod.lod[c].level != 0) continue;
    const geometry::ClusterDesc& desc = mesh.clusters[c];
    for (u32 t = 0; t < desc.triangle_count; ++t) {
      const u32 packed = mesh.triangles[desc.triangle_offset + t];
      for (u32 k = 0; k < 3; ++k)
        corners.push_back(
            mesh.vertex_source[desc.vertex_offset + geometry::ClusterMesh::unpack(packed, k)]);
    }
  }
  for (u32 i = 0; i + 2 < corners.size(); i += 3) {
    const u32 a = find_root(parent, corners[i]);
    const u32 b = find_root(parent, corners[i + 1]);
    const u32 c = find_root(parent, corners[i + 2]);
    if (a != b) parent[b] = a;
    if (a != c) parent[find_root(parent, c)] = a;
  }
  for (u32 i = 0; i + 2 < corners.size(); i += 3) {
    const u32 root = find_root(parent, corners[i]);
    const Vec2 e0 = uv[corners[i + 1]] - uv[corners[i]];
    const Vec2 e1 = uv[corners[i + 2]] - uv[corners[i]];
    const f64 triangle_area = std::fabs(static_cast<f64>(e0.x * e1.y - e0.y * e1.x)) * 0.5;
    area[root] += triangle_area;
    ++triangles[root];
    out.uv_area += triangle_area;
  }

  // Seam vertices: a source vertex that shares a position with another source vertex that
  // disagrees about the UV. Sorted by a position key rather than hashed, so the answer is a
  // function of the container alone.
  struct PositionKey {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    u32 vertex = 0;
  };
  Vector<PositionKey> keys;
  keys.reserve(out.source_vertices);
  for (u32 v = 0; v < source_count; ++v) {
    if (present[v] == 0) continue;
    keys.push_back(PositionKey{position[v].x, position[v].y, position[v].z, v});
  }
  std::sort(keys.begin(), keys.end(), [](const PositionKey& a, const PositionKey& b) {
    if (a.x != b.x) return a.x < b.x;
    if (a.y != b.y) return a.y < b.y;
    if (a.z != b.z) return a.z < b.z;
    return a.vertex < b.vertex;
  });
  for (u32 i = 0; i < keys.size();) {
    u32 j = i;
    while (j < keys.size() && keys[j].x == keys[i].x && keys[j].y == keys[i].y &&
           keys[j].z == keys[i].z)
      ++j;
    if (j - i > 1) {
      for (u32 k = i; k < j; ++k) {
        bool differs = false;
        for (u32 m = i; m < j; ++m) {
          if (m == k) continue;
          if (uv[keys[m].vertex].x != uv[keys[k].vertex].x ||
              uv[keys[m].vertex].y != uv[keys[k].vertex].y) {
            differs = true;
          }
        }
        if (differs) ++out.seam_vertices;
      }
    }
    i = j;
  }

  Vector<f64> island_areas;
  f64 smallest_area = 0.0;
  for (u32 v = 0; v < source_count; ++v) {
    if (triangles[v] == 0) continue;
    if (out.islands == 0 || area[v] < smallest_area) {
      smallest_area = area[v];
      out.smallest_triangles = triangles[v];
    }
    ++out.islands;
    island_areas.push_back(area[v]);
    const f64 texels = area[v] * 4096.0 * 4096.0;
    if (texels > out.largest_texels) out.largest_texels = texels;
  }
  out.smallest_texels = smallest_area * 4096.0 * 4096.0;
  if (!island_areas.empty()) {
    std::sort(island_areas.begin(), island_areas.end());
    out.median_texels = island_areas[island_areas.size() / 2] * 4096.0 * 4096.0;
  }
  out.valid = out.islands > 0;
  return out;
}

// What a coarse cut does to the texture, on the CPU (`geometry::measure_lod_attribute_error`).
// Two budgets, because one number cannot say whether the damage grows with the coarseness.
struct AttributeErrorRow {
  f64 fraction = 0.0;  // of the leaf triangles
  u32 clusters = 0;
  u32 triangles = 0;
  geometry::AttributeError error;
};

Vector<AttributeErrorRow> measure_attribute_error(const geometry::ClusterLodMesh& lod) {
  Vector<AttributeErrorRow> rows;
  if (lod.mesh.attributes.size() != lod.mesh.vertices.size() || lod.leaf_triangle_count == 0) {
    return rows;
  }
  for (const f64 fraction : {0.25, 0.10}) {
    const u32 target = static_cast<u32>(static_cast<f64>(lod.leaf_triangle_count) * fraction);
    // The cut nearest the budget: the raw cut's triangle count is monotone in the threshold, so a
    // geometric sweep is enough and needs no bracketing.
    f32 best_threshold = 0.0f;
    u32 best_distance = ~u32{0};
    for (u32 step = 0; step <= 64; ++step) {
      const f32 threshold = std::pow(10.0f, -6.0f + 8.0f * static_cast<f32>(step) / 64.0f);
      Vector<u32> cut;
      geometry::select_lod_raw(lod, threshold, cut);
      u32 triangles = 0;
      for (const u32 c : cut)
        triangles += lod.mesh.clusters[c].triangle_count;
      const u32 distance = triangles > target ? triangles - target : target - triangles;
      if (distance < best_distance) {
        best_distance = distance;
        best_threshold = threshold;
      }
    }
    Vector<u32> cut;
    geometry::select_lod_raw(lod, best_threshold, cut);
    if (cut.empty()) continue;
    AttributeErrorRow row;
    row.fraction = fraction;
    row.clusters = cut.size();
    for (const u32 c : cut)
      row.triangles += lod.mesh.clusters[c].triangle_count;
    std::string error;
    if (!geometry::measure_lod_attribute_error(lod, cut, geometry::AttributeErrorOptions{},
                                               row.error, &error)) {
      continue;
    }
    rows.push_back(row);
  }
  return rows;
}

// The metrics docs/plan/07-content-pipeline.md §7.3 wants a content build to report about what it
// produced: how the clusters are spread over the levels, how full they are, how much the cluster
// layout duplicates the source vertices, where the bytes went, how coarse the position grid is,
// how fragmented the UV atlas is, and how much of the texture a coarse LOD cut moves. One JSON
// line, so a script can watch them move between builds.
int stats(const std::string& path) {
  std::string file;
  geometry::ClusterFileHeader header;
  Vector<geometry::ClusterFileSection> records;
  geometry::ClusterFileData data;
  std::string error;
  if (!load_container(path, file, header, records, data, error)) return failed(error);

  const geometry::ClusterMesh& mesh = data.mesh.mesh;
  const u32 cluster_count = mesh.clusters.size();

  // Triangles per cluster: the three order statistics and the whole distribution, one entry per
  // distinct count, whose cluster counts add up to the clusters in the file.
  Vector<u32> per_cluster;
  per_cluster.reserve(cluster_count);
  for (const geometry::ClusterDesc& cluster : mesh.clusters)
    per_cluster.push_back(cluster.triangle_count);
  std::sort(per_cluster.begin(), per_cluster.end());
  JsonValue histogram = JsonValue::array();
  for (u32 i = 0; i < per_cluster.size();) {
    u32 j = i;
    while (j < per_cluster.size() && per_cluster[j] == per_cluster[i])
      ++j;
    JsonValue bucket = JsonValue::object();
    bucket.set("triangles", JsonValue(per_cluster[i]));
    bucket.set("clusters", JsonValue(j - i));
    histogram.push_back(std::move(bucket));
    i = j;
  }
  JsonValue triangles_per_cluster = JsonValue::object();
  triangles_per_cluster.set("min", JsonValue(per_cluster.empty() ? 0u : per_cluster[0]));
  // The upper middle for an even count, so the median is always a count some cluster has.
  triangles_per_cluster.set(
      "median", JsonValue(per_cluster.empty() ? 0u : per_cluster[per_cluster.size() / 2]));
  triangles_per_cluster.set(
      "max", JsonValue(per_cluster.empty() ? 0u : per_cluster[per_cluster.size() - 1]));
  triangles_per_cluster.set("total", JsonValue(mesh.triangles.size()));
  triangles_per_cluster.set("histogram", std::move(histogram));

  JsonValue levels = JsonValue::array();
  for (const u32 count : data.mesh.level_cluster_counts)
    levels.push_back(JsonValue(count));

  // Where the bytes went. The table is walked from the file, so a section this build does not
  // know is still accounted for, and the header and the table itself are named too.
  JsonValue sections = JsonValue::array();
  u64 payload_bytes = 0;
  for (const geometry::ClusterFileSection& section : records) {
    const u64 bytes = u64{section.element_size} * section.element_count;
    payload_bytes += bytes;
    JsonValue entry = JsonValue::object();
    entry.set("kind", JsonValue(section.kind));
    entry.set("name", JsonValue(geometry::cluster_section_name(section.kind)));
    entry.set("bytes", JsonValue(bytes));
    sections.push_back(std::move(entry));
  }
  const u64 header_bytes = static_cast<u64>(sizeof(header));
  const u64 table_bytes =
      static_cast<u64>(sizeof(geometry::ClusterFileSection)) * header.section_count;
  JsonValue bytes = JsonValue::object();
  bytes.set("total", JsonValue(header.total_bytes));
  bytes.set("header", JsonValue(header_bytes));
  bytes.set("section_table", JsonValue(table_bytes));
  bytes.set("payloads", JsonValue(payload_bytes));
  // What alignment cost: every payload starts on a 16-byte boundary.
  bytes.set("padding", JsonValue(header.total_bytes - header_bytes - table_bytes - payload_bytes));
  bytes.set("sections", std::move(sections));

  JsonValue origin = JsonValue::array();
  origin.push_back(JsonValue(mesh.quant_origin.x));
  origin.push_back(JsonValue(mesh.quant_origin.y));
  origin.push_back(JsonValue(mesh.quant_origin.z));
  JsonValue quantization = JsonValue::object();
  quantization.set("step", JsonValue(mesh.quant_scale));
  quantization.set("origin", std::move(origin));

  // Vertex duplication: the cluster-ordered vertices against the source vertices they came from.
  // The denominator is the number of distinct `vertex_source` entries, not the container's
  // `source_vertex_count`, because for a mesh merged from several primitives the latter counts the
  // whole source vertex space once per primitive and no ratio against it means anything.
  Vector<u32> referenced;
  referenced.reserve(mesh.vertex_source.size());
  for (const u32 source : mesh.vertex_source)
    referenced.push_back(source);
  std::sort(referenced.begin(), referenced.end());
  const u32 distinct =
      static_cast<u32>(std::unique(referenced.begin(), referenced.end()) - referenced.begin());
  const f64 duplication =
      distinct == 0 ? 0.0 : static_cast<f64>(mesh.vertices.size()) / static_cast<f64>(distinct);

  // The page table, the spread of child pages per page — which is how much of the DAG a page
  // depends on and therefore how wide a prefetch is — and the fly-in sweep.
  JsonValue pages = page_summary(data.pages);
  Vector<u32> child_counts;
  child_counts.reserve(data.pages.pages.size());
  for (const geometry::ClusterPageDesc& page : data.pages.pages)
    child_counts.push_back(page.child_page_count);
  std::sort(child_counts.begin(), child_counts.end());
  JsonValue child_histogram = JsonValue::array();
  for (u32 i = 0; i < child_counts.size();) {
    u32 j = i;
    while (j < child_counts.size() && child_counts[j] == child_counts[i])
      ++j;
    JsonValue bucket = JsonValue::object();
    bucket.set("child_pages", JsonValue(child_counts[i]));
    bucket.set("pages", JsonValue(j - i));
    child_histogram.push_back(std::move(bucket));
    i = j;
  }
  pages.set("child_pages_histogram", std::move(child_histogram));
  // The page table's own invariants, checked against the mesh it came with: a container is read
  // by the renderer, and this is the one place that says out loud whether its table is sound.
  if (!data.pages.pages.empty()) {
    std::string page_error;
    const bool valid = geometry::validate_cluster_pages(data.mesh, data.pages, &page_error);
    pages.set("valid", JsonValue(valid));
    if (!valid) pages.set("invalid_reason", JsonValue(page_error));
  }

  Vector<SweepStep> sweep;
  u64 budget = 0;
  std::string sweep_error;
  if (!sweep_streaming(data, sweep, budget, sweep_error)) return failed(sweep_error);
  JsonValue streaming = JsonValue::object();
  streaming.set("budget_bytes", JsonValue(budget));
  streaming.set("budget_fraction", JsonValue(k_sweep_budget));
  streaming.set("steps", JsonValue(k_sweep_steps));
  streaming.set("from_radii", JsonValue(k_sweep_far));
  streaming.set("to_radii", JsonValue(k_sweep_near));
  JsonValue steps = JsonValue::array();
  u64 requested_total = 0;
  u32 most_needed = 0;
  for (const SweepStep& step : sweep) {
    requested_total += step.requested;
    most_needed = step.pages_needed > most_needed ? step.pages_needed : most_needed;
    JsonValue entry = JsonValue::object();
    entry.set("radii", JsonValue(step.distance));
    entry.set("clusters", JsonValue(step.clusters));
    entry.set("triangles", JsonValue(step.triangles));
    entry.set("pages_needed", JsonValue(step.pages_needed));
    entry.set("requested", JsonValue(step.requested));
    entry.set("drawn", JsonValue(step.drawn));
    entry.set("evicted", JsonValue(step.evicted));
    entry.set("resident", JsonValue(step.resident));
    entry.set("resident_bytes", JsonValue(step.resident_bytes));
    steps.push_back(std::move(entry));
  }
  streaming.set("requested_total", JsonValue(requested_total));
  streaming.set("pages_needed_max", JsonValue(most_needed));
  streaming.set("steps_detail", std::move(steps));
  pages.set("streaming", std::move(streaming));
  print_sweep(sweep, budget);

  JsonValue summary = JsonValue::object();
  summary.set("path", JsonValue(path));
  summary.set("clusters", JsonValue(cluster_count));
  summary.set("pages", std::move(pages));
  summary.set("lod_levels", JsonValue(data.mesh.level_cluster_counts.size()));
  summary.set("level_clusters", std::move(levels));
  summary.set("groups", JsonValue(data.mesh.group_count));
  summary.set("leaf_triangles", JsonValue(data.mesh.leaf_triangle_count));
  summary.set("triangles_per_cluster", std::move(triangles_per_cluster));
  summary.set("source_vertices", JsonValue(mesh.source_vertex_count));
  summary.set("referenced_source_vertices", JsonValue(distinct));
  summary.set("cluster_vertices", JsonValue(mesh.vertices.size()));
  summary.set("vertex_duplication", JsonValue(duplication));
  summary.set("bytes", std::move(bytes));
  summary.set("quantization", std::move(quantization));
  summary.set("materials", JsonValue(data.materials.size()));
  // The images, slot by slot: a container is only drawable on its own when every image either
  // names a file or carries its bytes, so the per-image line says which it is and what it costs.
  const geometry::ClusterImageSummary image_summary = geometry::summarize_cluster_images(data);
  JsonValue image_detail = JsonValue::array();
  for (u32 i = 0; i < image_summary.count; ++i) {
    JsonValue entry = JsonValue::object();
    entry.set("path", JsonValue(i < data.image_paths.size() ? data.image_paths[i] : std::string()));
    const u64 carried = i < data.images.size() ? data.images[i].bytes.size() : 0;
    entry.set("bytes", JsonValue(carried));
    entry.set("mime", JsonValue(i < data.images.size() ? data.images[i].mime_type : std::string()));
    image_detail.push_back(std::move(entry));
  }
  JsonValue images = JsonValue::object();
  images.set("count", JsonValue(image_summary.count));
  images.set("embedded", JsonValue(image_summary.embedded));
  images.set("deduplicated", JsonValue(image_summary.deduplicated));
  images.set("bytes", JsonValue(image_summary.bytes));
  images.set("detail", std::move(image_detail));
  summary.set("images", JsonValue(image_summary.count));
  summary.set("image_detail", std::move(images));

  // The morph stream, channel by channel. The two numbers that decide whether the sparse layout
  // was worth building are `touched_fraction` — the share of the mesh's cluster vertices a channel
  // moves — and `bytes`, which is what a page has to carry for it (geometry.md, "Morph channels").
  if (!mesh.morph_channels.empty()) {
    Vector<u32> touched(mesh.morph_channels.size(), 0u);
    for (const geometry::MorphSlice& slice : mesh.morph_slices)
      touched[slice.channel] += slice.delta_count;
    const bool normals = !mesh.morph_normal_deltas.empty();
    const u32 per_delta = 1u + 6u + (normals ? 6u : 0u);
    JsonValue channels = JsonValue::array();
    u64 total_bytes = u64{mesh.morph_channels.size()} * sizeof(geometry::MorphChannel) +
                      u64{mesh.morph_cluster_slices.size()} * sizeof(u32) +
                      u64{mesh.morph_slices.size()} * sizeof(geometry::MorphSlice) +
                      u64{mesh.morph_delta_count} * per_delta;
    for (u32 c = 0; c < mesh.morph_channels.size(); ++c) {
      JsonValue entry = JsonValue::object();
      entry.set("name",
                JsonValue(c < mesh.morph_names.size() ? mesh.morph_names[c] : std::string()));
      entry.set("deltas", JsonValue(touched[c]));
      entry.set("touched_fraction",
                JsonValue(mesh.vertices.empty() ? 0.0
                                                : static_cast<f64>(touched[c]) /
                                                      static_cast<f64>(mesh.vertices.size())));
      entry.set("max_displacement",
                JsonValue(static_cast<f64>(mesh.morph_channels[c].max_displacement)));
      entry.set("position_scale",
                JsonValue(static_cast<f64>(mesh.morph_channels[c].position_scale)));
      entry.set("default_weight",
                JsonValue(static_cast<f64>(mesh.morph_channels[c].default_weight)));
      entry.set("bytes", JsonValue(u64{touched[c]} * per_delta));
      channels.push_back(std::move(entry));
    }
    JsonValue morph = JsonValue::object();
    morph.set("channels", JsonValue(mesh.morph_channels.size()));
    morph.set("deltas", JsonValue(mesh.morph_delta_count));
    morph.set("slices", JsonValue(mesh.morph_slices.size()));
    morph.set("normal_deltas", JsonValue(normals));
    morph.set("bytes", JsonValue(total_bytes));
    morph.set("bytes_per_cluster_vertex",
              JsonValue(mesh.vertices.empty() ? 0.0
                                              : static_cast<f64>(total_bytes) /
                                                    static_cast<f64>(mesh.vertices.size())));
    morph.set("detail", std::move(channels));
    summary.set("morph", std::move(morph));
    std::fprintf(stderr,
                 "  morph: %u channels, %u deltas over %u slices, %llu bytes (%.2f a cluster "
                 "vertex)%s\n",
                 mesh.morph_channels.size(), mesh.morph_delta_count, mesh.morph_slices.size(),
                 static_cast<unsigned long long>(total_bytes),
                 mesh.vertices.empty()
                     ? 0.0
                     : static_cast<double>(total_bytes) / static_cast<double>(mesh.vertices.size()),
                 normals ? "" : ", positions only");
  }

  // The atlas, and how much of the texture the LOD cut moves. Both are about the *picture* rather
  // than the bytes, and both are here because the seam rule of geometry.md made atlas
  // fragmentation a cost the asset controls rather than a defect the builder hides.
  const AtlasStats atlas_stats = measure_atlas(data.mesh);
  if (atlas_stats.valid) {
    JsonValue atlas = JsonValue::object();
    atlas.set("islands", JsonValue(atlas_stats.islands));
    atlas.set("source_vertices", JsonValue(atlas_stats.source_vertices));
    atlas.set("seam_vertices", JsonValue(atlas_stats.seam_vertices));
    atlas.set("seam_fraction", JsonValue(atlas_stats.source_vertices == 0
                                             ? 0.0
                                             : static_cast<f64>(atlas_stats.seam_vertices) /
                                                   static_cast<f64>(atlas_stats.source_vertices)));
    atlas.set("uv_area", JsonValue(atlas_stats.uv_area));
    atlas.set("smallest_island_texels_4096", JsonValue(atlas_stats.smallest_texels));
    atlas.set("smallest_island_triangles", JsonValue(atlas_stats.smallest_triangles));
    atlas.set("median_island_texels_4096", JsonValue(atlas_stats.median_texels));
    atlas.set("largest_island_texels_4096", JsonValue(atlas_stats.largest_texels));
    atlas.set("triangles_per_island",
              JsonValue(atlas_stats.islands == 0 ? 0.0
                                                 : static_cast<f64>(data.mesh.leaf_triangle_count) /
                                                       static_cast<f64>(atlas_stats.islands)));
    summary.set("atlas", std::move(atlas));
    std::fprintf(stderr,
                 "  atlas: %u islands, %u of %u vertices on a seam (%.1f%%), smallest island "
                 "%.0f texels of 4096 over %u triangles\n",
                 atlas_stats.islands, atlas_stats.seam_vertices, atlas_stats.source_vertices,
                 atlas_stats.source_vertices == 0
                     ? 0.0
                     : 100.0 * static_cast<double>(atlas_stats.seam_vertices) /
                           static_cast<double>(atlas_stats.source_vertices),
                 static_cast<double>(atlas_stats.smallest_texels), atlas_stats.smallest_triangles);
  }
  const Vector<AttributeErrorRow> attribute_rows = measure_attribute_error(data.mesh);
  if (!attribute_rows.empty()) {
    JsonValue rows = JsonValue::array();
    std::fprintf(stderr, "  LOD attribute error (closest point, texels of a 4096 atlas)\n");
    std::fprintf(stderr, "  %8s %9s %10s %9s %9s %9s %9s %9s\n", "budget", "clusters", "triangles",
                 "uv mean", "uv p99", "uv max", "over 8px", "normal");
    for (const AttributeErrorRow& row : attribute_rows) {
      JsonValue entry = JsonValue::object();
      entry.set("triangle_fraction", JsonValue(row.fraction));
      entry.set("clusters", JsonValue(row.clusters));
      entry.set("triangles", JsonValue(row.triangles));
      entry.set("samples", JsonValue(row.error.samples));
      entry.set("uv_mean_texels", JsonValue(row.error.uv_mean_texels));
      entry.set("uv_p99_texels", JsonValue(row.error.uv_p99_texels));
      entry.set("uv_max_texels", JsonValue(row.error.uv_max_texels));
      entry.set("uv_outlier_fraction", JsonValue(row.error.uv_outlier_fraction()));
      entry.set("normal_mean_deg", JsonValue(row.error.normal_mean_deg));
      entry.set("normal_max_deg", JsonValue(row.error.normal_max_deg));
      rows.push_back(std::move(entry));
      std::fprintf(stderr, "  %7.0f%% %9u %10u %9.1f %9.1f %9.1f %8.2f%% %8.2f\n",
                   row.fraction * 100.0, row.clusters, row.triangles,
                   static_cast<double>(row.error.uv_mean_texels),
                   static_cast<double>(row.error.uv_p99_texels),
                   static_cast<double>(row.error.uv_max_texels),
                   static_cast<double>(row.error.uv_outlier_fraction()) * 100.0,
                   static_cast<double>(row.error.normal_mean_deg));
    }
    summary.set("lod_attribute_error", std::move(rows));
  }

  summary.set("source_path", JsonValue(data.source_path));
  summary.set("source_hash", JsonValue(data.source_hash));
  summary.set("build_key", JsonValue(data.build_key));
  print_json(summary);
  return k_exit_ok;
}

int read_command(int argc, char** argv, const char* name, int (*run)(const std::string&)) {
  std::string log_spec;
  Vector<std::string> positional;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--log") {
      if (!next_value(argc, argv, i, log_spec)) return k_exit_usage;
    } else if (!a.empty() && a[0] == '-') {
      return usage("unknown option");
    } else {
      positional.push_back(std::string(a));
    }
  }
  if (positional.size() != 1) {
    const std::string message = std::string(name) + " takes one cluster file";
    return usage(message.c_str());
  }

  log::StreamSink stderr_sink(stderr, log::StreamSink::Format::Text);
  start_logging(stderr_sink, log_spec);
  const int code = run(positional[0]);
  log::remove_sink(&stderr_sink);
  return code;
}

}  // namespace

int main(int argc, char** argv) {
  engine::platform::require_cpu_baseline();  // ADR-0031, first statement
  if (argc < 2) return usage(nullptr);
  const std::string_view command = argv[1];
  if (command == "--help" || command == "-h" || command == "help") {
    std::fputs(k_usage, stdout);
    return k_exit_ok;
  }
  if (command == "build") return build_command(argc, argv);
  if (command == "build-all") return build_all_command(argc, argv);
  if (command == "info") return read_command(argc, argv, "info", info);
  if (command == "stats") return read_command(argc, argv, "stats", stats);
  return usage("unknown command");
}
