#pragma once

// The content build as a library (docs/plan/07-content-pipeline.md §7.3,
// docs/subsystems/content_build.md): import a glTF or GLB file, validate it against §7.4's rules,
// repair its UV-degenerate islands, optionally repack its atlas, weld it, build one cluster LOD DAG
// per primitive on the job system's performance pool, merge them in primitive order, lay them out
// in streaming pages, and write the `.clusters` container with its materials, its images and the
// source's identity; and the same over a manifest of meshes, one job per mesh, skipping an entry
// whose output already records this source and these options.
//
// **Why it is a module and not two copies.** It used to be `apps/engine_content/main.cpp`, which
// was fine while one program ran it. engine-host's `content.build` (docs/subsystems/protocol.md)
// runs the same build in-process, and a build that two hosts run from two copies would not stay
// one build: the bytes a container holds, its cache key and its skip rule are all things the two
// must agree on byte for byte, which is exactly what `domain/geometry`'s cache rules exist to
// guarantee between engine-content and engine-view. It cannot live in `domain/geometry` or
// `domain/assets` themselves: the build calls `domain/atlas`, which depends on both, so the
// orchestration sits one step above all three, which is here. Everything a command line prints
// — the usage, the stderr lines, the order of a JSON line's keys — stays in engine-content; what
// both hosts need, the build and what it reports, is here.
//
// **The bytes do not depend on the thread count or the toolchain** (docs/subsystems/geometry.md,
// "The same bytes from every toolchain"): the per-primitive DAGs merge in primitive index order,
// never completion order, and a manifest's results are kept per entry and read in manifest order.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/jobs/job_system.h>
#include <core/json/json_value.h>
#include <domain/atlas/repack.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/geometry/uv_repair.h>
#include <domain/texture/texture_build.h>
#include <domain/texture/texture_file.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::content_build {

// ---- what a build is asked for -----------------------------------------------------------------

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
  // the simplifier is given, and why"). They are options rather than fixed because the seam rule
  // is the difference between a mesh whose coarse levels paint the right part of the atlas and
  // one whose coarse levels do not, and the only honest way to say what it costs is to build both
  // containers and compare them — which is an option, not a rebuild.
  f32 normal_weight = geometry::ClusterLodOptions{}.normal_weight;
  f32 uv_weight = geometry::ClusterLodOptions{}.uv_weight;
  geometry::SeamRule uv_seams = geometry::ClusterLodOptions{}.uv_seams;
  geometry::SeamRule normal_seams = geometry::ClusterLodOptions{}.normal_seams;
  // `build --no-morph`: drop the source's morph targets before anything else sees them, so the
  // container is the one the same file would give with no channels — welded without the deltas in
  // the key, simplified without `morph_seams`, and with the seven morph sections empty. It exists
  // to answer "what does the morph stream cost this asset?" as a pair of builds, the way the seam
  // rules do; the recorded build key says it (see `build_key_of`), so a container built without
  // channels is never mistaken for the answer to a build that wanted them.
  bool morph = true;
  // `--atlas repack`: re-chart the mesh and rebake its textures before the weld
  // (docs/subsystems/atlas.md). `keep` is the source's atlas as it is, and the default until E10's
  // numbers say otherwise (docs/experiments/e10-generated-props.md, "Repack").
  bool repack = false;
  atlas::RepackOptions atlas;
};

// Every key a build records: `geometry::cluster_cache_key` over the source hash and the options,
// then moved by the two steps outside the geometry — `--no-morph` and `--atlas repack` — each only
// when it is asked for, so that a container built before either existed is still the answer to
// the question it answered.
u64 build_key_of(u64 source_hash, const MeshOptions& options) noexcept;
geometry::ClusterLodOptions lod_options_of(const MeshOptions& options);
// The cluster format's own limits: 4..256 triangles and 1..255 vertices a cluster.
bool options_in_range(const MeshOptions& options);

// The spellings the command line and the manifest share: "none", "protect", "lock"; "keep",
// "repack"; "convert", "resample". False, with `out` untouched, for anything else.
bool read_seam_rule(std::string_view text, geometry::SeamRule& out);
bool read_atlas_mode(std::string_view text, bool& repack);
bool read_normal_map_mode(std::string_view text, atlas::NormalMapMode& out);

// ---- what a build can say about its input (docs/plan/07-content-pipeline.md §7.4) --------------
//
// Rules carry a stable id so that a diagnostic is machine-readable, which is what §7.3 asks of a
// build failure: an asset, a rule, a severity, and a sentence. An error stops the build at the
// first one; a warning is reported, and `strict` turns it into an error.

inline constexpr const char* k_rule_empty_primitive = "geometry.empty_primitive";
inline constexpr const char* k_rule_index_range = "geometry.index_range";
inline constexpr const char* k_rule_nan_position = "geometry.nan_position";
inline constexpr const char* k_rule_degenerate = "geometry.degenerate_triangle";
inline constexpr const char* k_rule_cluster_budget = "geometry.cluster_budget";
inline constexpr const char* k_rule_uv_range = "geometry.uv_range";
inline constexpr const char* k_rule_missing_image = "material.missing_image";
// Not a warning: a **repair**. The build fixes the fault itself, mechanically and the same way
// every time, and says so with the counts (docs/subsystems/geometry.md, "UV-degenerate triangles:
// the repair"). An asset that needed it is still one the pipeline can take unattended, which is
// the difference between this and a warning.
inline constexpr const char* k_rule_uv_degenerate = "geometry.uv_degenerate";
// The atlas step's failure (xatlas refused the mesh, or a rebaked image could not be encoded). A
// material it merely could not repack is not one: it keeps its own atlas and the report says why.
inline constexpr const char* k_rule_atlas_repack = "atlas.repack";

// The visibility buffer packs `cluster << 8 | triangle` into 32 bits (AGENTS.md, ADR-0005), so a
// container with more clusters than this cannot be drawn whatever else is right about it.
inline constexpr u64 k_max_clusters = 1ull << 24;

struct Diagnostic {
  const char* rule = "";
  std::string message;
};

// ---- building one mesh -------------------------------------------------------------------------

// One image a mesh's materials sample, as the texture step needs it: which image, the options its
// slots ask for (`texture::options_for_roles`, packed), and where its bytes are — carried in the
// container, or in a file beside the source.
struct TextureSource {
  u32 image = 0;
  u32 options = 0;
  std::string file;  // the image file, resolved against the source's directory; empty if embedded
  Vector<u8> bytes;  // the embedded bytes; empty when `file` names them
};

// The images of a container's records that have a texture to build, in image order. `dir` is what
// a path in the container is relative to: the source mesh's directory.
void collect_texture_sources(const geometry::ClusterFileData& data, std::string_view dir,
                             Vector<TextureSource>& out);

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
  // Where the canonical vertex ids came from: "authored" when the source carried `_CANONICAL_ID`,
  // "position_weld" when the build derived them, "none" for a container with none.
  const char* vertex_id_source = "none";
  u64 bytes = 0;
  u64 hash = 0;
  f64 build_ms = 0.0;
  Vector<Diagnostic> warnings;
  geometry::UvRepairReport uv_repair;  // what the UV repair did; all zero when it had nothing to do
  bool repacked = false;               // `--atlas repack` ran (whether or not it changed anything)
  atlas::RepackReport atlas;
  // The images the texture step builds for this mesh, from the records the container carries.
  Vector<TextureSource> texture_sources;
};

// Imports, validates, clusters, and writes one mesh, always (the identity skip is the manifest's,
// `run_mesh_task` below). `pool` is where the per-primitive DAG builds run; null builds them one
// after another on the calling thread, which is what a manifest does because it already has one
// job per mesh in flight and nesting the two levels would only deepen the wait stack. Either way
// the merge walks the primitives in index order. `output`'s directory must exist.
bool build_one(const std::string& input, const std::string& output, const MeshOptions& options,
               bool strict, jobs::JobSystem* pool, u64 source_hash, u64 build_key, BuildResult& out,
               Diagnostic& error);

// The job system a build runs on. `worker_count` is `--jobs`; 0 leaves the system its own
// default, one worker per performance CPU. The efficiency pool is asked for one worker because
// nothing here uses it.
jobs::JobSystemConfig job_config(u32 worker_count);

// What a build did beyond its counts, as JSON: the `repairs` array (one row per rule that fired,
// empty when none did, so a reader tells "nothing to repair" from "an older build" by the key) and
// the `atlas` object (`{"mode":"keep"}`, or for a repack its options, the charts, the atlas per
// material and every rebaked image with how far it is from its source on the surface).
JsonValue repairs_json(const BuildResult& result);
JsonValue atlas_json(const BuildResult& result, const MeshOptions& options);

// ---- the texture step (docs/subsystems/texture.md) ---------------------------------------------
//
// Every image a mesh's materials sample is a derived node of its own: built into
// `<ddc>/textures/<key>.tex`, block-compressed and mipmapped, keyed by the image's own bytes and
// the options its slots ask for. A host runs the step after the containers are settled, and only
// when it has a derived-data root to put textures in.

// A missing image file is the validation warning `material.missing_image` again, not a new
// failure: the mesh built, and that texture draws as it always did without its file.
inline constexpr const char* k_rule_texture_missing = "material.missing_image";
inline constexpr const char* k_rule_texture_build = "texture.build";
// An image the decoder cannot read (a WebP or KTX2 source, a truncated PNG) or the builder cannot
// take (a side past 16,384) is a warning of its own: the mesh still builds and the image draws as
// it did before textures were built, which for an unreadable one is not at all. A write that
// fails is `texture.build` and fails the command.
inline constexpr const char* k_rule_texture_unreadable = "texture.unreadable";

enum class TextureState : u8 { Built, Skipped, Shared, Missing, Failed, Unreadable };

const char* texture_state_name(TextureState state) noexcept;

// One texture of the step, in the order the step met it.
struct TextureResult {
  u32 mesh = 0;  // the manifest entry (0 for `build`) that first asked for it
  u32 image = 0;
  TextureState state = TextureState::Failed;
  std::string path;
  std::string error;
  u64 source_hash = 0;
  u64 key = 0;
  u64 source_bytes = 0;
  u64 bytes = 0;  // of the .tex, built or found
  texture::TextureFormat format = texture::TextureFormat::rgba8;
  texture::ColorSpace color_space = texture::ColorSpace::linear;
  u32 width = 0;
  u32 height = 0;
  u32 levels = 0;
  f64 build_ms = 0.0;
  texture::TextureBuildReport report;
};

// The texture step over `sources` (with `mesh_of`, the manifest entry each came from), into
// `ddc`. Two passes:
//
//   1. In order, on the calling thread: read each source's bytes, hash them, and take the key —
//      a function of the bytes and the options, never of a path or a timestamp, so a repainted
//      file is a new key and an entry already built under a key is a hit however it got there.
//      A key met earlier in the step is `shared` (two slots or two meshes naming identical bytes
//      with identical options cost one build); an entry whose recorded identity matches is
//      `skipped`.
//   2. Every texture left, one job each on the performance pool, and each of those spreads its
//      mip rows and block rows over the same pool — a nested wait helps rather than blocks — so a
//      mesh of fifteen textures keeps every worker busy through the serial parts of each (the
//      decode above all), and one large texture still uses them all.
//
// Every job writes its own slot and its own file, so neither the files nor `out`'s order depends
// on which finished first.
void run_texture_step(std::span<const TextureSource* const> sources, std::span<const u32> mesh_of,
                      const std::string& ddc, jobs::JobSystem& pool, Vector<TextureResult>& out);

// The step's totals and rows as a build's `textures` object, over the results of `mesh` or of every
// mesh (`~0`). `enabled` false says why nothing was built, so an empty object is never mistaken
// for a mesh with no textures.
JsonValue textures_json(const Vector<TextureResult>& results, u32 mesh, bool enabled, bool detail);

// The texture sources of a container already on disk and up to date, which a manifest skipped:
// its resident sections hold the records, the carried bytes and the paths, so the mesh is not
// rebuilt to find out which images it samples. A container that cannot be opened contributes
// nothing and says so in the log.
void texture_sources_of_container(const std::string& path, const std::string& source,
                                  Vector<TextureSource>& out);

// ---- many meshes, once each --------------------------------------------------------------------

struct ManifestEntry {
  std::string source;  // resolved against the manifest's directory
  std::string output;  // likewise; empty means "the derived-data cache"
  MeshOptions options;
};

// Reads `{"meshes":[{"source","output","options"}]}` and resolves every path against the
// manifest's own directory, so a manifest is movable as a unit and says the same thing from any
// working directory. `defaults` is what an entry's `options` does not say. False, with a sentence
// naming the manifest and the entry, on the first problem.
bool read_manifest(const std::string& path, const MeshOptions& defaults, Vector<ManifestEntry>& out,
                   std::string& error);

enum class TaskState : u8 { Built, Skipped, Failed };

// One mesh in flight. Everything a job writes lives here, and a caller walks its tasks in order
// afterwards, so neither the output files nor anything reported about them depends on which job
// finished first.
struct MeshTask {
  const ManifestEntry* entry = nullptr;
  const std::string* ddc = nullptr;  // the cache root, for an entry with no output
  bool strict = false;
  // Rebuild even when the container already at the output records this source and these options.
  // engine-content's `build-all` never sets it; the protocol's `content.build` does when asked.
  bool force = false;
  // The per-primitive builds' pool; null builds them in sequence (see `build_one`).
  jobs::JobSystem* pool = nullptr;
  TaskState state = TaskState::Failed;
  BuildResult result;
  Diagnostic error;
};

// One task, as a job function (`data` is the `MeshTask`). It hashes the source, computes the build
// key, and **skips the entry when the container already at the output records that same source
// hash and that same key** — which is what the container's `source_hash` section is for, and why a
// container that records none (an older build, or one engine-view wrote) is rebuilt rather than
// trusted. An incremental build never decides from a timestamp.
void run_mesh_task(void* data);

// Every task as one job on the performance pool, waited for. Leave each task's own `pool` null, so
// the per-primitive builds inside a task run in sequence: the manifest is the coarser level and
// already fills the pool.
void run_mesh_tasks(std::span<MeshTask> tasks, jobs::JobSystem& pool);

}  // namespace engine::content_build
