// engine-content vine / cover (content_commands.h; docs/subsystems/cover.md, "The build"): a vine's
// or a cover's recipe grown over the host it names and built into a `.clusters` container — the
// host's companion — through the derived-data cache, keyed by the canonical recipe, the solvers'
// version and the host's identity, so the renderer's scene reader finds what this wrote. The host
// is read through content_build's `GeneratorHosts`, the seam the scene reader uses, under `build`'s
// default options. Present when this build has the cover capability; refused with a sentence
// otherwise.
#include "content_commands.h"

#include <core/base/types.h>
#include <core/json/json.h>

#include <cstdio>
#include <string>
#include <string_view>

#if ENGINE_CONTENT_COVER
#include <core/platform/process.h>
#include <core/time/time.h>
#include <domain/content_build/content_build.h>
#include <domain/content_build/generator_hosts.h>
#include <domain/cover/cover.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/materials/materials.h>
#include <foundation/io/vfs.h>
#endif

namespace engine::content {

namespace {

constexpr int k_exit_ok = 0;
constexpr int k_exit_error = 1;
constexpr int k_exit_usage = 2;

const char* k_cover_usage =
    "usage: engine-content vine <recipe.vine.json> <out.clusters> [options]\n"
    "       engine-content vine <recipe.vine.json> --cache [options]\n"
    "       engine-content cover <recipe.cover.json> <out.clusters>|--cache [options]\n"
    "  grow a vine (engine.cover.VineFile) or organic cover (engine.cover.CoverFile) over the "
    "host\n"
    "  mesh its recipe names, and build it into the host's companion container, keyed by the\n"
    "  canonical recipe, the solvers' version and the host's identity\n"
    "      --cache        write <ddc>/clusters/<key>.clusters: the entry a scene's mesh table "
    "reads\n"
    "      --ddc <dir>    the cache root (default: <repo>/ddc); implies --cache without an output\n"
    "      --force        build even when the output is already the answer\n"
    "      --materials <dir>  the material pack the parts take vine/stem, vine/leaf, moss/mat,\n"
    "                     lichen/crust, algae/film and fungus/cap from\n"
    "      --no-materials no pack: the parts draw their flat colours\n";

[[maybe_unused]] int usage(const char* command, const char* message) {
  if (message != nullptr) std::fprintf(stderr, "engine-content %s: %s\n", command, message);
  std::fputs(k_cover_usage, stderr);
  return k_exit_usage;
}

[[maybe_unused]] int failed(const char* command, const std::string& message) {
  std::fprintf(stderr, "engine-content %s: %s\n", command, message.c_str());
  return k_exit_error;
}

#if ENGINE_CONTENT_COVER
void print_json(const JsonValue& value) {
  std::string text = write_json(value, JsonWriteOptions{.pretty = false});
  text.push_back('\n');
  std::fwrite(text.data(), 1, text.size(), stdout);
}

JsonValue vine_json(const cover::VineReport& r) {
  JsonValue out = JsonValue::object();
  out.set("stems", JsonValue(r.stems));
  out.set("seeds", JsonValue(r.seeds));
  out.set("steps", JsonValue(r.steps));
  out.set("held_steps", JsonValue(r.held_steps));
  out.set("leaning_steps", JsonValue(r.leaning_steps));
  out.set("free_steps", JsonValue(r.free_steps));
  out.set("leaves", JsonValue(r.leaves));
  out.set("triangles", JsonValue(r.triangles));
  out.set("row_stride", JsonValue(r.row_stride));
  out.set("leaf_stride", JsonValue(r.leaf_stride));
  out.set("length_m", JsonValue(r.length_m));
  out.set("seed_height_m", JsonValue(r.seed_height));
  out.set("tip_height_m", JsonValue(r.tip_height));
  return out;
}

JsonValue cover_json(const cover::CoverReport& r) {
  JsonValue out = JsonValue::object();
  JsonValue mean = JsonValue::object();
  mean.set("moss", JsonValue(r.mean[cover::k_moss]));
  mean.set("lichen", JsonValue(r.mean[cover::k_lichen]));
  mean.set("algae", JsonValue(r.mean[cover::k_algae]));
  mean.set("fungus", JsonValue(r.mean[cover::k_fungus]));
  out.set("mean", mean);
  out.set("holding_area_m2", JsonValue(r.holding_area_m2));
  out.set("film_triangles", JsonValue(r.film_triangles));
  out.set("cushions", JsonValue(r.cushions));
  out.set("tufts", JsonValue(r.tufts));
  out.set("brackets", JsonValue(r.brackets));
  out.set("mushrooms", JsonValue(r.mushrooms));
  out.set("triangles", JsonValue(r.triangles));
  out.set("film_threshold", JsonValue(static_cast<f64>(r.film_threshold)));
  out.set("density_scale", JsonValue(r.density_scale));
  return out;
}

int build(const char* command, bool is_vine, int argc, char** argv) {
  std::string input;
  std::string output;
  std::string ddc;
  std::string materials_dir;
  bool cache = false;
  bool force = false;
  bool no_materials = false;
  bool ddc_named = false;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--cache") {
      cache = true;
    } else if (a == "--force") {
      force = true;
    } else if (a == "--no-materials") {
      no_materials = true;
    } else if (a == "--materials") {
      if (i + 1 >= argc) return usage(command, "--materials needs a directory");
      materials_dir = argv[++i];
    } else if (a == "--ddc") {
      ddc_named = true;
      if (i + 1 >= argc) return usage(command, "--ddc needs a directory");
      ddc = argv[++i];
    } else if (!a.empty() && a[0] == '-') {
      return usage(command, "unknown option");
    } else if (input.empty()) {
      input = std::string(a);
    } else if (output.empty()) {
      output = std::string(a);
    } else {
      return usage(command, "one recipe and at most one output");
    }
  }
  if (input.empty()) return usage(command, "a recipe is needed");
  if (output.empty() && !ddc.empty()) cache = true;
  if (cache == !output.empty())
    return usage(command,
                 cache ? "--cache takes no output file" : "an output or --cache is needed");
  if (cache && ddc.empty()) {
    ddc = geometry::find_ddc_root(platform::executable_directory());
    if (ddc.empty()) return usage(command, "--cache found no repository root; pass --ddc");
  }
  const i64 start_ns = time::monotonic_ns();
  std::string text;
  const io::Status status = io::read_file(input, text);
  if (status != io::Status::Ok)
    return failed(command, "cannot read " + input + ": " + io::status_name(status));
  std::string error;
  cover::VineFile vine_file;
  cover::CoverFile cover_file;
  if (is_vine ? !cover::read_vine_file(text, vine_file, error)
              : !cover::read_cover_file(text, cover_file, error))
    return failed(command, input + ": " + error);
  const std::string& host_path = is_vine ? vine_file.host : cover_file.host;

  // `build`'s defaults, which are the renderer's, so the two apps share entries.
  const geometry::ClusterLodOptions lod =
      content_build::lod_options_of(content_build::MeshOptions{});
  const u32 page_bytes = geometry::ClusterPagesOptions{}.page_bytes;
  if (!no_materials && materials_dir.empty())
    materials_dir = materials::default_pack_dir(platform::executable_directory());
  const materials::Pack* pack = no_materials ? nullptr : materials::pack_at(materials_dir);
  const content_build::GeneratorHosts hosts(input, lod, page_bytes, pack);
  u64 identity = 0;
  if (!hosts.identify(host_path, identity, error)) return failed(command, input + ": " + error);
  const u64 source_hash = is_vine ? cover::vine_source_hash(vine_file, identity)
                                  : cover::cover_source_hash(cover_file, identity);
  const u64 build_key = assets::generated_mesh_build_key(
      source_hash, assets::MeshGeneratorInputs{pack}, lod, page_bytes);
  const std::string path = cache ? geometry::cluster_cache_path(ddc, build_key) : output;

  bool cached = false;
  geometry::ClusterFileData data;
  if (!force && io::exists(path)) {
    std::string read_error;
    cached = geometry::read_cluster_file(path, data, &read_error) &&
             data.source_hash == source_hash && data.build_key == build_key;
  }
  cover::VineReport vine_report;
  cover::CoverReport cover_report;
  u32 host_triangles = 0;
  f64 host_ms = 0.0;
  if (!cached) {
    const i64 host_start = time::monotonic_ns();
    assets::GeneratorHostMesh mesh;
    if (!hosts.load(host_path, mesh, error)) return failed(command, input + ": " + error);
    cover::Host host;
    if (!cover::prepare_host(mesh, host, error)) return failed(command, input + ": " + error);
    host_triangles = host.surface.triangle_count();
    host_ms = static_cast<f64>(time::monotonic_ns() - host_start) / 1.0e6;
    const bool built =
        is_vine ? cover::build_vine_container(vine_file, host, identity, lod, page_bytes, pack,
                                              data, &vine_report, error)
                : cover::build_cover_container(cover_file, host, identity, lod, page_bytes, pack,
                                               data, &cover_report, error);
    if (!built) return failed(command, input + ": " + error);
    data.source_path = input;
    if (cache) {
      const io::Status made = io::make_directories(io::parent_path(path));
      if (made != io::Status::Ok)
        return failed(command, "cannot create the cache directory for " + path + ": " +
                                   io::status_name(made));
    }
    if (!geometry::write_cluster_file(path, data, &error)) return failed(command, error);
  }
  JsonValue summary = JsonValue::object();
  summary.set("path", JsonValue(path));
  summary.set("cached", JsonValue(cached));
  summary.set("host", JsonValue(hosts.resolve(host_path)));
  summary.set("host_identity", JsonValue(identity));
  summary.set("source_hash", JsonValue(source_hash));
  summary.set("build_key", JsonValue(build_key));
  summary.set("cover_version", JsonValue(cover::k_cover_version));
  summary.set("triangles", JsonValue(data.mesh.leaf_triangle_count));
  summary.set("clusters", JsonValue(data.mesh.mesh.clusters.size()));
  summary.set("lod_levels", JsonValue(data.mesh.level_cluster_counts.size()));
  summary.set("cover_mask", JsonValue(data.cover_mask.size()));
  u32 texture_failures = 0;
  summary.set("textures", generated_textures(data, input, cache || ddc_named ? ddc : std::string(),
                                             texture_failures));
  if (!cached) {
    summary.set("host_triangles", JsonValue(host_triangles));
    summary.set("host_ms", JsonValue(host_ms));
    summary.set(is_vine ? "vine" : "cover",
                is_vine ? vine_json(vine_report) : cover_json(cover_report));
  }
  summary.set("build_ms", JsonValue(static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e6));
  print_json(summary);
  return texture_failures > 0 ? k_exit_error : k_exit_ok;
}
#else
int refuse(const char* command) {
  std::fprintf(stderr,
               "engine-content %s: this build has no cover capability (ENGINE_WITH_COVER is off, "
               "or the minimal build); docs/subsystems/cover.md\n",
               command);
  return k_exit_error;
}
#endif

int command_of(const char* command, bool is_vine, int argc, char** argv) {
  if (argc >= 3 && (std::string_view(argv[2]) == "--help" || std::string_view(argv[2]) == "-h")) {
    std::fputs(k_cover_usage, stdout);
    return k_exit_ok;
  }
#if ENGINE_CONTENT_COVER
  return build(command, is_vine, argc, argv);
#else
  (void)is_vine;
  (void)argv;
  return refuse(command);
#endif
}

}  // namespace

int vine_command(int argc, char** argv) { return command_of("vine", true, argc, argv); }
int cover_command(int argc, char** argv) { return command_of("cover", false, argc, argv); }

}  // namespace engine::content
