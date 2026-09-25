// engine-content: the content build on the command line (docs/plan/07-content-pipeline.md §7.3).
// `build` imports a glTF 2.0 or GLB file (domain/assets), validates it against the rules of
// §7.4, welds its vertices, builds one cluster LOD DAG per primitive so that every cluster has a
// single material (domain/geometry), merges them, and writes the result as a `.clusters`
// container together with the material table, the image paths the materials name, the encoded
// bytes of the images the source embedded rather than named, the source path those paths are
// relative to, and the source's identity (its content hash and the build key over it and the
// options), which is what makes a second build able to skip the work. `--atlas repack` puts one
// more step between the UV repair and the weld: the mesh is re-charted and every texture its
// materials sample is rebaked into the new atlas (domain/atlas, docs/subsystems/atlas.md). The
// per-primitive DAG builds run as jobs on the job system's performance pool, and the merge is
// over the primitives in index order, never in completion order, so the bytes do not depend on
// how many threads ran them.
//
// `build-all` reads a manifest of meshes and builds every entry that is not already up to date,
// one job per mesh. `--cache` puts a build into the derived-data cache, under the hash of the
// source and the build options, which is the path `engine-view --mesh` looks in before it builds
// anything. Every image a mesh's materials sample is also a derived node of its own
// (docs/subsystems/texture.md): a build that has a derived-data root builds each into
// `<ddc>/textures/<key>.tex` — block-compressed, mipmapped, keyed by the image's own bytes and the
// options its material slots ask for — and records the key in the container, so the renderer
// finds it from the container alone. `texture` builds one image the same way. `info` prints what a
// container or a texture holds; `stats` prints the content-build metrics of a container. Every
// command prints one JSON line per result on stdout, so scripts and agents read the numbers without
// parsing prose; everything else goes through the log to stderr.
//
// `tissue`, `limit-dump` and `ruins` live beside this file (content_commands.h): the tissue
// definition's import, info, validators and report, the limit-surface conformance exchange, and
// the ruin assembler's scene fragment.
//
// The build itself — the rules, the per-primitive jobs, the manifest and its identity skip, the
// texture step, and the metrics `stats` prints — is `domain/content_build`, because engine-host's
// `content.build` runs the same build in-process (docs/subsystems/content_build.md). What stays
// here is the command line: the flags, the usage, the stderr lines and the JSON lines' shape.
//
// Exit codes: 0 ok; 1 a file could not be loaded, validated, built, or written; 2 usage.
#include "content_commands.h"

#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <core/log/log.h>
#include <core/platform/cpu_baseline.h>
#include <core/platform/process.h>
#include <core/time/time.h>
#include <domain/assets/gltf.h>
#include <domain/atlas/repack.h>
#include <domain/content_build/container_stats.h>
#include <domain/content_build/content_build.h>
#include <domain/geometry/cluster_file.h>
#include <domain/texture/material_textures.h>
#include <domain/texture/texture_build.h>
#include <domain/texture/texture_file.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <engine_build_stamp.h>
#include <span>
#include <string>
#include <string_view>
#include <utility>

using namespace engine;
using namespace engine::content_build;

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_content, "content");

constexpr int k_exit_ok = 0;
constexpr int k_exit_error = 1;
constexpr int k_exit_usage = 2;

const char* k_usage =
    "usage: engine-content <command> [options]\n"
    "\n"
    "  build <in.gltf|in.glb> <out.clusters>   import a mesh and write a cluster container\n"
    "  build <in.wav|flac|mp3> <out.clip>      decode a clip once into the mix format (48 kHz\n"
    "                                          f32), as the clip store loads it; --cache writes\n"
    "                                          <ddc>/clips/<key>.clip. The mesh flags below do\n"
    "                                          not apply. Present with the audio capability\n"
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
    "      --atlas <mode>        keep (default): the source's UV atlas as it is; repack: re-chart\n"
    "                            the mesh, pack one atlas per textured material, and rebake its\n"
    "                            textures into it (the source's UV layout is not kept)\n"
    "      --atlas-normal-maps <m>  convert (default): through object space into the new tangent\n"
    "                            frame; resample: as stored, which is wrong where a chart turns\n"
    "      --atlas-proxy <n>     cut the charts on a copy simplified to about n triangles and\n"
    "                            carry them onto the mesh (default 2000); 0 charts the mesh\n"
    "                            itself\n"
    "      --atlas-chart-cost <n>  how far xatlas lets a chart grow, on the proxy or the mesh, in\n"
    "                            thousandths of xatlas's maxCost (default 2000)\n"
    "      --atlas-supersample <n>  1 (default), 2 or 4: the atlas and every rebaked image at n\n"
    "                            times the source texture's side\n"
    "      --cache               write into the derived-data cache instead of a named output,\n"
    "                            addressed by the source and the options above\n"
    "      --ddc <dir>           the cache root (default: <repo>/ddc, found beside AGENTS.md).\n"
    "                            The images the materials sample are built into\n"
    "                            <ddc>/textures/ whenever the build has a root: with --cache,\n"
    "                            or with --ddc named outright. A named output with neither\n"
    "                            writes the container and touches nothing else\n"
    "      --no-textures         build no textures (the container still records their keys)\n"
    "      --jobs <n>            performance-pool workers: one per primitive here, one per\n"
    "                            mesh in build-all, and a texture's block rows in both\n"
    "                            (default: the machine's performance CPUs)\n"
    "      --strict              treat validation warnings as errors\n"
    "      --log <spec>          log levels, e.g. \"info\" or \"warn,content=debug\"\n"
    "  build-all <manifest.json>               build every mesh (and clip) a manifest names\n"
    "      --cache               entries with no \"output\" go to the derived-data cache\n"
    "      --page-bytes <n>      the default for entries whose \"options\" do not say\n"
    "      --ddc <dir>, --no-textures, --jobs <n>, --strict, --log <spec>   as above; a mesh\n"
    "                            that is up to date still has its textures checked\n"
    "  texture <in.png|jpg|tga|bmp> <out.tex>   build one image into a block-compressed texture\n"
    "      --format <f>          auto (default), bc1, bc3, bc4, bc5, bc7 or rgba8. auto is bc5\n"
    "                            for a normal map, bc4 for a one-channel --linear image, bc7\n"
    "                            otherwise\n"
    "      --srgb | --linear     what the image's bytes are: colour (default) or data\n"
    "      --normal              a tangent-space normal map: renormalized, filtered as vectors\n"
    "      --no-mips             level 0 alone (default: the full chain to 1x1)\n"
    "      --wrap <mode>[,<mode>]  what the mip filter reads past the edges, across and down:\n"
    "                            clamp (default), repeat or mirror — the sampler's wrap, so a\n"
    "                            tiling texture's levels tile too\n"
    "      --cache               write <ddc>/textures/<key>.tex instead of a named output\n"
    "      --ddc <dir>, --jobs <n>, --log <spec>   as above\n"
    "  info <file.clusters|file.tex|file.clip>  print the header, sections, and counts\n"
    "  stats <file.clusters>                   print the content-build metrics of a container\n"
    "  tissue import|info|validate|report|example ...   tissue definitions (engine-content tissue\n"
    "                                          help); present with the tissue capability\n"
    "  limit-dump <control.json> --level <n> --out <dump.json> [--mode <m>]\n"
    "                                          the limit-surface conformance exchange\n"
    "  normal-cases <cases.json> --out <results.json>\n"
    "                                          the footpoint-normal rule's edge cases, one by one\n"
    "  ruins <kit.json> <seed> <x,z> --out <scene.json>   a tile's ruined building, assembled\n"
    "                                          from a kit, as a scene fragment (ruins --help);\n"
    "                                          present with the ruins capability\n"
    "  ruins-kit <directory>                   the synthetic kit of boxes, for tests and\n"
    "                                          measurements\n"
    "  ruins-block-kit <directory> [--fidelity low|mid|high]\n"
    "                                          the synthetic block kit, for ruins --blocks\n"
    "  terrain <scene.json> --tile <x,z> [--time <s>]   one tile of the scene's dune field at a\n"
    "                                          game time as one JSON line (terrain --help);\n"
    "                                          present with the terrain capability\n"
    "  --version                              the commit this binary was built from, as JSON\n"
    "\n"
    "the build-all manifest:\n"
    "  {\"meshes\":[{\"source\":\"a.gltf\",\"output\":\"a.clusters\",\n"
    "               \"options\":{\"max_triangles\":124,\"max_vertices\":64,\"weld\":true,\n"
    "                           \"page_bytes\":131072,\"atlas\":\"keep\"}}]}\n"
    "  paths are relative to the manifest file; \"output\" and \"options\" are optional.\n"
    "  with the audio capability, "
    "\"clips\":[{\"source\":\"theme.wav\",\"output\":\"theme.clip\"}]\n"
    "  beside (or instead of) \"meshes\": each built once, skipped while its output records the\n"
    "  same source and build key.\n"
    "\n"
    "examples:\n"
    "  engine-content build content/samples/Suzanne/Suzanne.gltf ddc/suzanne.clusters\n"
    "  engine-content build content/samples/Suzanne/Suzanne.gltf --cache\n"
    "  engine-content build prop.glb prop.clusters --atlas repack\n"
    "  engine-content build-all content/meshes.json --cache --jobs 8\n"
    "  engine-content texture albedo.png albedo.tex --format bc7 --srgb\n"
    "  engine-content texture normal.png normal.tex --normal\n"
    "  engine-content build music/theme.wav --cache\n"
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

// ---- what a build says ------------------------------------------------------------------------

void log_warnings(const std::string& source, const Vector<Diagnostic>& warnings) {
  for (const Diagnostic& warning : warnings) {
    ENGINE_LOG_WARN(log_content, "validation warning", log::field("source", source),
                    log::field("rule", warning.rule), log::field("detail", warning.message));
  }
}

// The atlas step for a person, on stderr, like the repair line below: what was repacked into
// what, and what was left and why.
void print_atlas(const std::string& source, const BuildResult& result) {
  if (!result.repacked) return;
  const atlas::RepackReport& r = result.atlas;
  u32 resolution = 0;
  for (const atlas::RepackedMaterial& m : r.materials)
    resolution = std::max(resolution, m.resolution);
  std::fprintf(stderr,
               "atlas: repack: %s: %u material%s re-charted into %u charts (atlas up to %u), %u "
               "image%s rebaked (%.1f MB of PNG), %u vertices before and %u after, %.0f ms\n",
               source.c_str(), r.materials_repacked, r.materials_repacked == 1 ? "" : "s", r.charts,
               resolution, static_cast<u32>(r.images.size()), r.images.size() == 1 ? "" : "s",
               static_cast<double>(r.image_bytes) / 1.0e6, r.vertices_before, r.vertices_after,
               r.total_ms);
  for (const std::string& note : r.notes)
    std::fprintf(stderr, "atlas: repack: %s: %s\n", source.c_str(), note.c_str());
}

// The same, for a person, on stderr: a `repair` line, which is tool output like the stats table
// and not a log record — a warning is something to act on, and this is something that was done.
void print_repairs(const std::string& source, const BuildResult& result) {
  const geometry::UvRepairReport& r = result.uv_repair;
  if (r.triangles == 0) return;
  std::fprintf(stderr,
               "repair: %s: %s: %u atlas island%s under %g texel%s of a 4096 atlas as stored, "
               "%u triangle%s: %u refolded into the neighbouring island (%u corners from it, %u "
               "onto the shared edge), %u dropped, %u unrepaired\n",
               k_rule_uv_degenerate, source.c_str(), r.islands, r.islands == 1 ? "" : "s",
               r.threshold_texels, r.threshold_texels == 1.0 ? "" : "s", r.triangles,
               r.triangles == 1 ? "" : "s", r.refolded, r.corners_from_neighbours,
               r.corners_on_edge, r.dropped, r.unrepaired);
}

// What went wrong in the step, on stderr, for a person: a failure is an error line, a missing file
// the validation warning it already was.
u32 report_texture_problems(const Vector<TextureResult>& results) {
  u32 failures = 0;
  for (const TextureResult& r : results) {
    if (r.state == TextureState::Failed) {
      ++failures;
      std::fprintf(stderr, "engine-content: %s: image %u: %s\n", k_rule_texture_build, r.image,
                   r.error.c_str());
    } else if (r.state == TextureState::Missing) {
      ENGINE_LOG_WARN(log_content, "texture not built", log::field("rule", k_rule_texture_missing),
                      log::field("image", r.image), log::field("detail", r.error));
    } else if (r.state == TextureState::Unreadable) {
      ENGINE_LOG_WARN(log_content, "texture not built",
                      log::field("rule", k_rule_texture_unreadable), log::field("image", r.image),
                      log::field("detail", r.error));
    }
  }
  return failures;
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
  // The texture step runs when the build has a derived-data root to put textures in: `--cache`, or
  // `--ddc` named outright. A named output with neither touches nothing but that output, which is
  // what every build did before textures were derived and what every test that builds into its own
  // scratch directory relies on.
  bool textures = true;
  bool ddc_named = false;
};

int build(const BuildCommandOptions& options) {
  // The source's identity comes first whatever the destination is: it addresses the cache entry,
  // and it is written into the container so that a later build can tell it is still the answer.
  u64 source_hash = 0;
  std::string message;
  if (!assets::source_mesh_hash(options.input, source_hash, &message)) return failed(message);
  const u64 key = build_key_of(source_hash, options.mesh);

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
  print_repairs(options.input, result);
  print_atlas(options.input, result);

  // The texture step, after the container: every image a material samples, reported in the
  // container's image order whatever order the jobs finished in (`run_texture_step`).
  const bool textures = options.textures && (options.cache || options.ddc_named);
  Vector<TextureResult> texture_results;
  if (textures) {
    Vector<const TextureSource*> sources;
    for (const TextureSource& source : result.texture_sources)
      sources.push_back(&source);
    const Vector<u32> mesh_of(sources.size(), 0u);
    run_texture_step(std::span<const TextureSource* const>(sources.data(), sources.size()),
                     std::span<const u32>(mesh_of.data(), mesh_of.size()), options.ddc, pool,
                     texture_results);
  }
  const u32 texture_failures = report_texture_problems(texture_results);

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
  summary.set("vertex_id_source", JsonValue(result.vertex_id_source));
  summary.set("warnings", JsonValue(result.warnings.size()));
  summary.set("repairs", repairs_json(result));
  summary.set("atlas", atlas_json(result, options.mesh));
  summary.set("textures", textures_json(texture_results, ~u32{0}, textures, true));
  summary.set("bytes", JsonValue(result.bytes));
  summary.set("build_ms", JsonValue(result.build_ms));
  summary.set("hash", JsonValue(result.hash));
  print_json(summary);
  return texture_failures == 0 ? k_exit_ok : k_exit_error;
}

// A capability's derived output (content_commands.h): one source, always built, into the named
// output or the cache — the same task a manifest runs, forced, as `build` always builds a mesh.
int derived_build(const BuildCommandOptions& options, const content_build::DerivedStep& step) {
  content_build::DerivedEntry entry;
  entry.source = options.input;
  if (!options.cache) entry.output = options.output;
  content_build::DerivedTask task;
  task.step = &step;
  task.entry = &entry;
  task.ddc = &options.ddc;
  task.force = true;
  content_build::run_derived_task(&task);
  if (task.state == TaskState::Failed)
    return failed(std::string(task.error.rule) + ": " + task.error.message);
  JsonValue summary = JsonValue::object();
  summary.set("path", JsonValue(task.path));
  summary.set("cached", JsonValue(options.cache));
  summary.set("step", JsonValue(step.name));
  summary.set("source_hash", JsonValue(task.source_hash));
  summary.set("build_key", JsonValue(task.build_key));
  summary.set("source_bytes", JsonValue(task.source_bytes));
  for (auto [key, value] : task.report.as_object())
    summary.set(key, value);
  summary.set("bytes", JsonValue(task.bytes));
  summary.set("build_ms", JsonValue(task.build_ms));
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
    } else if (a == "--atlas") {
      std::string mode;
      if (!next_value(argc, argv, i, mode)) return k_exit_usage;
      if (!read_atlas_mode(mode, options.mesh.repack)) return usage("--atlas is keep or repack");
    } else if (a == "--atlas-normal-maps") {
      std::string mode;
      if (!next_value(argc, argv, i, mode)) return k_exit_usage;
      if (!read_normal_map_mode(mode, options.mesh.atlas.normal_maps))
        return usage("--atlas-normal-maps is convert or resample");
    } else if (a == "--atlas-proxy") {
      if (!next_u32(argc, argv, i, options.mesh.atlas.proxy_triangles)) return k_exit_usage;
    } else if (a == "--atlas-supersample") {
      if (!next_u32(argc, argv, i, options.mesh.atlas.supersample)) return k_exit_usage;
      const u32 s = options.mesh.atlas.supersample;
      if (s != 1 && s != 2 && s != 4) return usage("--atlas-supersample is 1, 2 or 4");
    } else if (a == "--atlas-chart-cost") {
      if (!next_u32(argc, argv, i, options.mesh.atlas.max_chart_cost_milli)) return k_exit_usage;
      if (options.mesh.atlas.max_chart_cost_milli == 0)
        return usage("--atlas-chart-cost is a positive number of thousandths");
    } else if (a == "--jobs") {
      if (!next_u32(argc, argv, i, options.jobs)) return k_exit_usage;
    } else if (a == "--log") {
      if (!next_value(argc, argv, i, options.log_spec)) return k_exit_usage;
    } else if (a == "--ddc") {
      if (!next_value(argc, argv, i, options.ddc)) return k_exit_usage;
      options.ddc_named = true;
    } else if (a == "--no-textures") {
      options.textures = false;
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
  // A source a capability's derived step takes (an audio clip) is that step's to build; the mesh
  // flags do not apply to it.
  const content_build::DerivedStep* step = nullptr;
  for (const content_build::DerivedStep* candidate : content::derived_steps()) {
    if (candidate->accepts(options.input)) {
      step = candidate;
      break;
    }
  }
  const int code = step != nullptr ? derived_build(options, *step) : build(options);
  log::remove_sink(&stderr_sink);
  return code;
}

// ---- build-all ----------------------------------------------------------------------------------

struct BuildAllCommandOptions {
  std::string manifest;
  std::string log_spec;
  std::string ddc;
  MeshOptions defaults;  // what an entry's "options" object does not override
  u32 jobs = 0;
  bool cache = false;
  bool strict = false;
  bool textures = true;    // as `build`: when there is a derived-data root
  bool ddc_named = false;  // --ddc was given
};

int build_all(const BuildAllCommandOptions& options) {
  const i64 start_ns = time::monotonic_ns();
  Vector<ManifestEntry> entries;
  std::string message;
  // The derived steps this configuration has read their own arrays of the same manifest; a
  // manifest of nothing but clips has no "meshes".
  const std::span<const DerivedStep* const> steps = content::derived_steps();
  Vector<const char*> step_arrays;
  for (const DerivedStep* step : steps)
    step_arrays.push_back(step->manifest_array);
  if (!read_manifest(options.manifest, options.defaults, entries, message,
                     std::span<const char* const>(step_arrays.data(), step_arrays.size()))) {
    return failed(message);
  }
  Vector<Vector<DerivedEntry>> derived_entries(static_cast<u32>(steps.size()));
  for (u32 s = 0; s < steps.size(); ++s) {
    if (!read_derived_entries(options.manifest, *steps[s], derived_entries[s], message))
      return failed(message);
    for (const DerivedEntry& entry : derived_entries[s]) {
      if (entry.output.empty() && !options.cache) {
        return failed("manifest '" + options.manifest + "' entry '" + entry.source +
                      "' has no \"output\" and --cache was not given");
      }
    }
  }
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

  jobs::JobSystem pool(job_config(options.jobs));
  run_mesh_tasks(std::span<MeshTask>(tasks.data(), tasks.size()), pool);

  // The derived outputs, one job each, after the meshes: nothing a mesh builds depends on them.
  Vector<DerivedTask> derived;
  for (u32 s = 0; s < steps.size(); ++s) {
    for (const DerivedEntry& entry : derived_entries[s]) {
      DerivedTask task;
      task.step = steps[s];
      task.entry = &entry;
      task.ddc = &options.ddc;
      derived.push_back(std::move(task));
    }
  }
  run_derived_tasks(std::span<DerivedTask>(derived.data(), derived.size()), pool);

  // The texture step, once every container is settled, in manifest order and then image order —
  // never completion order — with a texture two meshes share built once, for the first of them.
  // A mesh that was up to date still has its textures checked, because a texture's entry can be
  // missing (a cache that lost it, a container engine-view wrote) while its container is current.
  const bool textures = options.textures && (options.cache || options.ddc_named);
  Vector<TextureResult> texture_results;
  if (textures) {
    // The up-to-date meshes' sources come out of their containers; each list lives until the step
    // is done, since the step holds pointers into them.
    Vector<Vector<TextureSource>> from_containers(tasks.size());
    Vector<const TextureSource*> sources;
    Vector<u32> mesh_of;
    for (u32 m = 0; m < tasks.size(); ++m) {
      const MeshTask& task = tasks[m];
      const Vector<TextureSource>* list = &task.result.texture_sources;
      if (task.state == TaskState::Skipped) {
        texture_sources_of_container(task.result.path, task.entry->source, from_containers[m]);
        list = &from_containers[m];
      } else if (task.state != TaskState::Built) {
        continue;
      }
      for (const TextureSource& source : *list) {
        sources.push_back(&source);
        mesh_of.push_back(m);
      }
    }
    run_texture_step(std::span<const TextureSource* const>(sources.data(), sources.size()),
                     std::span<const u32>(mesh_of.data(), mesh_of.size()), options.ddc, pool,
                     texture_results);
  }
  const u32 texture_failures = report_texture_problems(texture_results);

  u32 built = 0;
  u32 skipped = 0;
  u32 failures = 0;
  for (u32 m = 0; m < tasks.size(); ++m) {
    const MeshTask& task = tasks[m];
    log_warnings(task.entry->source, task.result.warnings);
    JsonValue line = JsonValue::object();
    line.set("source", JsonValue(task.entry->source));
    line.set("path", JsonValue(task.result.path));
    line.set("textures", textures_json(texture_results, m, textures, false));
    switch (task.state) {
      case TaskState::Built: {
        ++built;
        print_repairs(task.entry->source, task.result);
        print_atlas(task.entry->source, task.result);
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
        line.set("repairs", repairs_json(task.result));
        line.set("atlas", atlas_json(task.result, task.entry->options));
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

  // A line per derived output, in manifest order, and a count per step. Its failures fail the
  // command as a mesh's do.
  JsonValue step_counts = JsonValue::object();
  for (const DerivedStep* step : steps) {
    JsonValue counts = JsonValue::object();
    counts.set("built", JsonValue(0u));
    counts.set("skipped", JsonValue(0u));
    counts.set("failed", JsonValue(0u));
    step_counts.set(step->name, std::move(counts));
  }
  u32 derived_failures = 0;
  for (const DerivedTask& task : derived) {
    JsonValue line = JsonValue::object();
    line.set("source", JsonValue(task.entry->source));
    line.set("path", JsonValue(task.path));
    line.set("step", JsonValue(task.step->name));
    const char* status = task.state == TaskState::Built     ? "built"
                         : task.state == TaskState::Skipped ? "skipped"
                                                            : "failed";
    line.set("status", JsonValue(status));
    JsonValue& count = step_counts[task.step->name][status];
    count = JsonValue(count.as_uint() + 1u);
    if (task.state == TaskState::Failed) {
      ++derived_failures;
      line.set("rule", JsonValue(task.error.rule));
      line.set("error", JsonValue(task.error.message));
      std::fprintf(stderr, "engine-content: %s: %s\n", task.error.rule, task.error.message.c_str());
    } else {
      line.set("source_hash", JsonValue(task.source_hash));
      line.set("build_key", JsonValue(task.build_key));
      line.set("bytes", JsonValue(task.bytes));
      if (task.state == TaskState::Built) {
        for (auto [key, value] : task.report.as_object())
          line.set(key, value);
        line.set("build_ms", JsonValue(task.build_ms));
      }
    }
    print_json(line);
  }
  failures += derived_failures;

  JsonValue summary = JsonValue::object();
  summary.set("built", JsonValue(built));
  summary.set("skipped", JsonValue(skipped));
  summary.set("failed", JsonValue(failures - derived_failures));
  if (!steps.empty()) summary.set("steps", std::move(step_counts));
  summary.set("textures", textures_json(texture_results, ~u32{0}, textures, true));
  summary.set("seconds", JsonValue(static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e9));
  print_json(summary);
  return failures == 0 && texture_failures == 0 ? k_exit_ok : k_exit_error;
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
      options.ddc_named = true;
    } else if (a == "--no-textures") {
      options.textures = false;
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

// ---- texture ------------------------------------------------------------------------------------

f64 ms_since(i64 start_ns) noexcept {
  return static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e6;
}

// A number JSON can hold: PSNR is infinite for an exact texture, and JSON has no infinity.
JsonValue finite_or_null(f64 value) {
  return std::isfinite(value) ? JsonValue(value) : JsonValue();
}

struct TextureCommandOptions {
  std::string input;
  std::string output;
  std::string log_spec;
  std::string ddc;
  texture::TextureBuildOptions build;
  u32 jobs = 0;
  bool cache = false;
};

// One image into a `.tex`: the same function the mesh build's texture step calls, with the options
// given on the command line instead of taken from a material slot, so a texture built here for the
// same bytes and options is the same bytes and the same cache entry.
int texture_command_run(const TextureCommandOptions& options) {
  const i64 start_ns = time::monotonic_ns();
  std::string bytes;
  const io::Status status = io::read_file(options.input, bytes);
  if (status != io::Status::Ok) {
    return failed("cannot read '" + options.input + "': " + io::status_name(status));
  }
  const std::span<const u8> encoded(reinterpret_cast<const u8*>(bytes.data()), bytes.size());
  const u64 source_hash = hash_bytes(encoded.data(), encoded.size());
  const u64 key = texture::texture_cache_key(source_hash, options.build);
  std::string output = options.output;
  if (options.cache) {
    output = texture::texture_cache_path(options.ddc, key);
    const io::Status made = io::make_directories(io::parent_path(output));
    if (made != io::Status::Ok) {
      return failed("cannot create the cache directory '" + std::string(io::parent_path(output)) +
                    "': " + io::status_name(made));
    }
  }
  jobs::JobSystem pool(job_config(options.jobs));
  texture::TextureData data;
  texture::TextureBuildReport report;
  std::string error;
  if (!texture::build_texture_from_encoded(encoded, options.build, data, &pool, &error, &report))
    return failed(std::string(k_rule_texture_build) + ": '" + options.input + "': " + error);
  if (!texture::write_texture_file(output, data, &error))
    return failed("output.unwritable: " + error);
  io::FileInfo info;
  const u64 written = io::stat_file(output, info) == io::Status::Ok ? info.size : 0;

  JsonValue summary = JsonValue::object();
  summary.set("path", JsonValue(output));
  summary.set("cached", JsonValue(options.cache));
  summary.set("source", JsonValue(options.input));
  summary.set("source_bytes", JsonValue(static_cast<u64>(bytes.size())));
  summary.set("source_channels", JsonValue(data.source_channels));
  summary.set("source_hash", JsonValue(data.source_hash));
  summary.set("build_key", JsonValue(data.build_key));
  summary.set("format", JsonValue(texture::texture_format_name(data.format)));
  summary.set("color_space", JsonValue(texture::color_space_name(data.color_space)));
  summary.set("normal_map", JsonValue((data.flags & texture::k_texture_normal_map) != 0));
  summary.set("has_alpha", JsonValue((data.flags & texture::k_texture_has_alpha) != 0));
  summary.set("width", JsonValue(data.width));
  summary.set("height", JsonValue(data.height));
  summary.set("levels", JsonValue(data.levels.size()));
  summary.set("block_bytes", JsonValue(data.data.size()));
  summary.set("bytes", JsonValue(written));
  summary.set("psnr", finite_or_null(report.psnr));
  summary.set("max_error", JsonValue(report.max_error));
  JsonValue ms = JsonValue::object();
  ms.set("decode", JsonValue(report.decode_ms));
  ms.set("mips", JsonValue(report.mip_ms));
  ms.set("encode", JsonValue(report.encode_ms));
  ms.set("total", JsonValue(ms_since(start_ns)));
  summary.set("ms", std::move(ms));
  summary.set("jobs", JsonValue(pool.worker_count(jobs::Pool::Performance)));
  summary.set("hash", JsonValue(texture::texture_file_hash(data)));
  print_json(summary);
  return k_exit_ok;
}

int texture_command(int argc, char** argv) {
  TextureCommandOptions options;
  Vector<std::string> positional;
  bool srgb_given = false;
  bool linear_given = false;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--format") {
      std::string value;
      if (!next_value(argc, argv, i, value)) return k_exit_usage;
      if (!texture::parse_format_choice(value, options.build.format))
        return usage("--format is auto, bc1, bc3, bc4, bc5, bc7 or rgba8");
    } else if (a == "--srgb") {
      srgb_given = true;
      options.build.color_space = texture::ColorSpace::srgb;
    } else if (a == "--linear") {
      linear_given = true;
      options.build.color_space = texture::ColorSpace::linear;
    } else if (a == "--normal") {
      options.build.normal_map = true;
    } else if (a == "--no-mips") {
      options.build.mips = false;
    } else if (a == "--wrap") {
      std::string value;
      if (!next_value(argc, argv, i, value)) return k_exit_usage;
      const usize comma = value.find(',');
      const std::string across = value.substr(0, comma);
      const std::string down = comma == std::string::npos ? across : value.substr(comma + 1);
      if (!texture::parse_edge_mode(across, options.build.edge_x) ||
          !texture::parse_edge_mode(down, options.build.edge_y)) {
        return usage("--wrap is clamp, repeat or mirror, or two of them: <across>,<down>");
      }
    } else if (a == "--jobs") {
      if (!next_u32(argc, argv, i, options.jobs)) return k_exit_usage;
    } else if (a == "--log") {
      if (!next_value(argc, argv, i, options.log_spec)) return k_exit_usage;
    } else if (a == "--ddc") {
      if (!next_value(argc, argv, i, options.ddc)) return k_exit_usage;
    } else if (a == "--cache") {
      options.cache = true;
    } else if (!a.empty() && a[0] == '-') {
      return usage("unknown option for texture");
    } else {
      positional.push_back(std::string(a));
    }
  }
  if (srgb_given && linear_given) return usage("--srgb and --linear are each other's opposite");
  // A normal map is data whatever else was said, and saying --srgb of one is a mistake worth
  // naming rather than quietly overriding.
  if (options.build.normal_map && srgb_given) return usage("a --normal map is --linear data");
  if (options.build.normal_map) options.build.color_space = texture::ColorSpace::linear;
  if (positional.size() != (options.cache ? 1u : 2u)) {
    return usage(options.cache ? "texture --cache takes an input image and no output file"
                               : "texture takes an input image and an output file");
  }
  options.input = positional[0];
  if (!options.cache) options.output = positional[1];
  if (options.cache && options.ddc.empty()) {
    options.ddc = geometry::find_ddc_root(platform::executable_directory());
    if (options.ddc.empty())
      return usage("--cache found no repository root above the executable; pass --ddc <dir>");
  }
  if (options.jobs > 4096) return usage("--jobs is 1..4096, or absent for one per CPU");
  texture::TextureFormat resolved = texture::TextureFormat::bc7;
  std::string why;
  // The source's channel count is not known until it is read; this catches the combinations that
  // are wrong whatever it is (BC4 or BC5 asked for as sRGB colour).
  if (options.build.format != texture::FormatChoice::automatic &&
      !texture::resolve_texture_format(options.build, 4, resolved, &why)) {
    return usage(why.c_str());
  }

  log::StreamSink stderr_sink(stderr, log::StreamSink::Format::Text);
  start_logging(stderr_sink, options.log_spec);
  const int code = texture_command_run(options);
  log::remove_sink(&stderr_sink);
  return code;
}

// ---- info ------------------------------------------------------------------------------------

// `info` on a `.tex`: the header, the section table with each payload's hash (so two textures can
// be compared section by section, as two containers can), and what the texture is.
int texture_info(const std::string& path) {
  std::string file;
  const io::Status status = io::read_file(path, file);
  if (status != io::Status::Ok) {
    return failed("cannot read '" + path + "': " + io::status_name(status));
  }
  const std::span<const u8> bytes(reinterpret_cast<const u8*>(file.data()), file.size());
  texture::TextureData data;
  std::string error;
  if (!texture::read_texture_file_memory(bytes, data, &error)) return failed(error);
  texture::TextureFileHeader header;
  Vector<texture::TextureFileSection> records;
  if (!texture::read_texture_file_table(bytes, header, records, &error)) return failed(error);

  JsonValue sections = JsonValue::array();
  for (const texture::TextureFileSection& section : records) {
    JsonValue entry = JsonValue::object();
    entry.set("kind", JsonValue(section.kind));
    entry.set("name", JsonValue(texture::texture_section_name(section.kind)));
    entry.set("element_size", JsonValue(section.element_size));
    entry.set("element_count", JsonValue(section.element_count));
    entry.set("offset", JsonValue(section.offset));
    const u64 payload = u64{section.element_size} * section.element_count;
    if (section.offset <= file.size() && payload <= file.size() - section.offset)
      entry.set("hash", JsonValue(hash_bytes(file.data() + section.offset, payload)));
    sections.push_back(std::move(entry));
  }
  JsonValue levels = JsonValue::array();
  for (const texture::TextureFileLevel& level : data.levels) {
    JsonValue entry = JsonValue::object();
    entry.set("width", JsonValue(level.width));
    entry.set("height", JsonValue(level.height));
    entry.set("bytes", JsonValue(level.bytes));
    levels.push_back(std::move(entry));
  }
  JsonValue summary = JsonValue::object();
  summary.set("path", JsonValue(path));
  summary.set("kind", JsonValue("texture"));
  summary.set("version", JsonValue(header.version));
  summary.set("flags", JsonValue(header.flags));
  summary.set("total_bytes", JsonValue(header.total_bytes));
  summary.set("hash", JsonValue(header.content_hash));
  summary.set("sections", std::move(sections));
  summary.set("format", JsonValue(texture::texture_format_name(data.format)));
  summary.set("color_space", JsonValue(texture::color_space_name(data.color_space)));
  summary.set("normal_map", JsonValue((data.flags & texture::k_texture_normal_map) != 0));
  summary.set("has_alpha", JsonValue((data.flags & texture::k_texture_has_alpha) != 0));
  summary.set("width", JsonValue(data.width));
  summary.set("height", JsonValue(data.height));
  summary.set("source_channels", JsonValue(data.source_channels));
  summary.set("block_bytes", JsonValue(data.data.size()));
  summary.set("levels", std::move(levels));
  summary.set("source_hash", JsonValue(data.source_hash));
  summary.set("build_key", JsonValue(data.build_key));
  print_json(summary);
  return k_exit_ok;
}

// A container's texture records, the ones that name a texture: which image, which slots, what the
// texture is built as, and — for an image the container carries — the key it is found under.
JsonValue texture_records_json(const geometry::ClusterFileData& data) {
  JsonValue rows = JsonValue::array();
  for (u32 i = 0; i < data.textures.size(); ++i) {
    const geometry::ClusterFileTexture& record = data.textures[i];
    if (record.options == 0) continue;
    JsonValue row = JsonValue::object();
    row.set("image", JsonValue(i));
    row.set("roles", JsonValue(record.roles));
    texture::TextureBuildOptions options;
    if (texture::unpack_texture_options(record.options, options)) {
      row.set("format", JsonValue(texture::format_choice_name(options.format)));
      row.set("color_space", JsonValue(texture::color_space_name(options.color_space)));
      row.set("normal_map", JsonValue(options.normal_map));
      row.set("mips", JsonValue(options.mips));
      row.set("edge_x", JsonValue(texture::edge_mode_name(options.edge_x)));
      row.set("edge_y", JsonValue(texture::edge_mode_name(options.edge_y)));
    } else {
      row.set("options", JsonValue(record.options));  // a newer build's word, shown as it is
    }
    row.set("key", JsonValue(record.key));
    row.set("source_hash", JsonValue(record.source_hash));
    rows.push_back(std::move(row));
  }
  return rows;
}

int info(const std::string& path) {
  // A clip, a texture or a container, told apart by the magic rather than by the file's name.
  int clip_code = k_exit_ok;
  if (content::clip_info(path, clip_code)) return clip_code;
  u8 magic[4] = {0, 0, 0, 0};
  u64 got = 0;
  if (io::read_file_range(path, 0, magic, sizeof(magic), got) == io::Status::Ok &&
      got == sizeof(magic) && texture::is_texture_file(std::span<const u8>(magic, sizeof(magic)))) {
    return texture_info(path);
  }
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
    // The payload's own hash, so two containers can be compared section by section: the header's
    // content hash covers every byte, the source path included, and that differs between two
    // machines that built the same mesh from two directories.
    const u64 payload = u64{section.element_size} * section.element_count;
    if (section.offset <= file.size() && payload <= file.size() - section.offset)
      entry.set("hash", JsonValue(hash_bytes(file.data() + section.offset, payload)));
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
  summary.set("textures", texture_records_json(data));
  summary.set("morph_channels", JsonValue(lod.mesh.morph_channels.size()));
  summary.set("morph_deltas", JsonValue(lod.mesh.morph_delta_count));
  // The canonical vertex ids: how many (one per cluster vertex, or none) and what id space they
  // are in, which is what a file written beside this mesh checks before it names a vertex.
  summary.set("vertex_ids", JsonValue(lod.mesh.vertex_ids.size()));
  summary.set("vertex_id_source",
              JsonValue(geometry::vertex_id_source_name(lod.mesh.vertex_id_source)));
  summary.set("source_path", JsonValue(data.source_path));
  summary.set("source_hash", JsonValue(data.source_hash));
  summary.set("build_key", JsonValue(data.build_key));
  print_json(summary);
  return k_exit_ok;
}

// ---- stats ------------------------------------------------------------------------------------

// The metrics docs/plan/07-content-pipeline.md §7.3 wants a content build to report about what it
// produced (`content_build::container_stats`, which the protocol's `content.build` returns too):
// one JSON line on stdout, so a script can watch them move between builds, and the tables a person
// reads on stderr.
int stats(const std::string& path) {
  JsonValue summary;
  std::string human;
  std::string error;
  if (!content_build::container_stats(path, summary, human, error)) return failed(error);
  std::fwrite(human.data(), 1, human.size(), stderr);
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
    const std::string message = std::string(name) + " takes one file";
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
  if (command == "--version") {
    // The build stamp (cmake/EngineBuildStamp.cmake): tools/e10-harness.ps1 refuses to measure
    // with a binary built from an older commit than the source it reports.
    std::printf("{\"tool\":\"engine-content\",\"commit\":\"%s\",\"dirty\":%s}\n",
                build_stamp::commit(), build_stamp::dirty() ? "true" : "false");
    return k_exit_ok;
  }
  if (command == "build") return build_command(argc, argv);
  if (command == "build-all") return build_all_command(argc, argv);
  if (command == "texture") return texture_command(argc, argv);
  if (command == "info") return read_command(argc, argv, "info", info);
  if (command == "stats") return read_command(argc, argv, "stats", stats);
  if (command == "tissue") return content::tissue_command(argc, argv);
  if (command == "limit-dump") return content::limit_dump_command(argc, argv);
  if (command == "normal-cases") return content::normal_cases_command(argc, argv);
  if (command == "ruins") return content::ruins_command(argc, argv);
  if (command == "ruins-kit") return content::ruins_kit_command(argc, argv);
  if (command == "ruins-block-kit") return content::ruins_block_kit_command(argc, argv);
  if (command == "terrain") return content::terrain_command(argc, argv);
  return usage("unknown command");
}
