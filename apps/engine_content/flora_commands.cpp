// engine-content flora (content_commands.h; docs/subsystems/flora.md, "The build"): a flora file —
// a species and its parameters — solved and built into a `.clusters` container through the
// derived-data cache, keyed by the canonical parameters, the species and the solver's version, so
// a palm is built once per parameter set and the renderer's scene reader finds what this wrote.
// Present when this build has the flora capability; refused with a sentence otherwise.
#include "content_commands.h"

#include <core/base/types.h>
#include <core/json/json.h>

#include <cstdio>
#include <string>
#include <string_view>

#if ENGINE_CONTENT_FLORA
#include <core/platform/process.h>
#include <core/time/time.h>
#include <domain/content_build/content_build.h>
#include <domain/content_build/generator_hosts.h>
#include <domain/flora/flora.h>
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

const char* k_flora_usage =
    "usage: engine-content flora <params.flora.json> <out.clusters> [options]\n"
    "       engine-content flora <params.flora.json> --cache [options]\n"
    "  solve a flora file (engine.flora.FloraFile: a species and its parameters) and build the\n"
    "  mesh into a cluster container, keyed by the canonical parameters, the species and the\n"
    "  solver's version; an output that already records that key is not built again\n"
    "      --cache        write <ddc>/clusters/<key>.clusters instead of a named output: the\n"
    "                     entry engine-view --mesh and a scene's mesh table read\n"
    "      --ddc <dir>    the cache root (default: <repo>/ddc, found beside AGENTS.md); implies\n"
    "                     --cache when no output is named\n"
    "      --force        build even when the output is already the answer\n"
    "      --materials <dir>  the material pack a part with no set of its own takes its role's\n"
    "                     default from (default: <repo>/content/materials, found beside "
    "AGENTS.md)\n"
    "      --no-materials no pack: every part with no set of its own draws its flat colour\n"
    "  with a derived-data root (--cache or --ddc), the images the parts sample are built into\n"
    "  <ddc>/textures beside the container, as `build --cache` builds a glTF's\n";

[[maybe_unused]] int usage(const char* message) {
  if (message != nullptr) std::fprintf(stderr, "engine-content flora: %s\n", message);
  std::fputs(k_flora_usage, stderr);
  return k_exit_usage;
}

[[maybe_unused]] int failed(const std::string& message) {
  std::fprintf(stderr, "engine-content flora: %s\n", message.c_str());
  return k_exit_error;
}

#if ENGINE_CONTENT_FLORA
void print_json(const JsonValue& value) {
  std::string text = write_json(value, JsonWriteOptions{.pretty = false});
  text.push_back('\n');
  std::fwrite(text.data(), 1, text.size(), stdout);
}

int flora(int argc, char** argv) {
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
      if (i + 1 >= argc) return usage("--materials needs a directory");
      materials_dir = argv[++i];
    } else if (a == "--ddc") {
      ddc_named = true;
      if (i + 1 >= argc) return usage("--ddc needs a directory");
      ddc = argv[++i];
    } else if (!a.empty() && a[0] == '-') {
      return usage("unknown option");
    } else if (input.empty()) {
      input = std::string(a);
    } else if (output.empty()) {
      output = std::string(a);
    } else {
      return usage("one flora file and at most one output");
    }
  }
  if (input.empty()) return usage("a flora file is needed");
  if (output.empty() && !ddc.empty()) cache = true;
  // The output is a path or the cache, never both, as `build`'s is.
  if (cache == !output.empty()) {
    return usage(cache ? "--cache takes no output file" : "an output file or --cache is needed");
  }
  if (cache && ddc.empty()) {
    ddc = geometry::find_ddc_root(platform::executable_directory());
    if (ddc.empty())
      return usage("--cache found no repository root above the executable; pass --ddc");
  }

  const i64 start_ns = time::monotonic_ns();
  std::string text;
  const io::Status status = io::read_file(input, text);
  if (status != io::Status::Ok)
    return failed("cannot read " + input + ": " + io::status_name(status));
  flora::FloraFile file;
  std::string error;
  if (!flora::read_flora_file(text, file, error)) return failed(input + ": " + error);

  // `build`'s defaults, which are the renderer's: the two apps share an entry only when they agree
  // on the options the key is taken over.
  const geometry::ClusterLodOptions lod =
      content_build::lod_options_of(content_build::MeshOptions{});
  const u32 page_bytes = geometry::ClusterPagesOptions{}.page_bytes;
  // The material pack a part with no set of its own falls back to (materials.md).
  if (!no_materials && materials_dir.empty())
    materials_dir = materials::default_pack_dir(platform::executable_directory());
  const materials::Pack* pack = no_materials ? nullptr : materials::pack_at(materials_dir);
  const content_build::GeneratorHosts hosts(input, lod, page_bytes, pack);
  u64 source_hash = 0;
  if (!flora::flora_source_hash_with(file, hosts.inputs(), source_hash, error))
    return failed(input + ": " + error);
  const u64 build_key = flora::flora_build_key(source_hash, lod, page_bytes, pack);
  const std::string path = cache ? geometry::cluster_cache_path(ddc, build_key) : output;

  // The identity skip: an output that records this source and this key is already the answer.
  bool cached = false;
  geometry::ClusterFileData data;
  if (!force && io::exists(path)) {
    std::string read_error;
    cached = geometry::read_cluster_file(path, data, &read_error) &&
             data.source_hash == source_hash && data.build_key == build_key;
  }
  flora::PalmReport report;
  flora::TreeReport tree;
  bool have_report = false;
  if (!cached) {
    if (!flora::build_flora_container(file, lod, page_bytes, data, &report, error, pack, &tree,
                                      hosts.hooks()))
      return failed(input + ": " + error);
    have_report = true;
    data.source_path = input;
    if (cache) {
      const io::Status made = io::make_directories(io::parent_path(path));
      if (made != io::Status::Ok)
        return failed("cannot create the cache directory for " + path + ": " +
                      io::status_name(made));
    }
    if (!geometry::write_cluster_file(path, data, &error)) return failed(error);
  }

  JsonValue summary = JsonValue::object();
  summary.set("path", JsonValue(path));
  summary.set("cached", JsonValue(cached));
  summary.set("species", JsonValue(file.species));
  summary.set("source_hash", JsonValue(source_hash));
  summary.set("build_key", JsonValue(build_key));
  summary.set("flora_version", JsonValue(flora::k_flora_version));
  summary.set("triangles", JsonValue(data.mesh.leaf_triangle_count));
  summary.set("clusters", JsonValue(data.mesh.mesh.clusters.size()));
  summary.set("lod_levels", JsonValue(data.mesh.level_cluster_counts.size()));
  summary.set("vertices", JsonValue(data.mesh.mesh.vertices.size()));
  summary.set("materials", JsonValue(data.materials.size()));
  summary.set("images", JsonValue(data.image_paths.size()));
  summary.set("pack", pack != nullptr ? JsonValue(pack->name + " " + pack->version) : JsonValue());
  summary.set("pages", JsonValue(data.pages.pages.size()));
  // The texture step beside the container, when there is a derived-data root to put textures in.
  u32 texture_failures = 0;
  summary.set("textures", generated_textures(data, input, cache || ddc_named ? ddc : std::string(),
                                             texture_failures));
  if (have_report && file.species != flora::k_species_date_palm) {
    // A branching species' budget, and for the allometric crowns what the structure is (flora.md,
    // "Allometry"): the twigs and orders, the cards, the merge level, and the realised leaf area
    // index over the footprint.
    JsonValue budget = JsonValue::object();
    budget.set("triangles", JsonValue(tree.triangles));
    budget.set("wood_triangles", JsonValue(tree.wood_triangles));
    budget.set("leaf_triangles", JsonValue(tree.leaf_triangles));
    budget.set("parts", JsonValue(tree.parts));
    budget.set("steps", JsonValue(tree.steps));
    summary.set("budget", std::move(budget));
    if (tree.twigs > 0) {
      JsonValue crown = JsonValue::object();
      crown.set("twigs", JsonValue(tree.twigs));
      crown.set("orders", JsonValue(tree.orders));
      crown.set("leaf_cards", JsonValue(tree.leaf_cards));
      crown.set("merge_level", JsonValue(tree.merge_level));
      crown.set("card_length_m", JsonValue(static_cast<f64>(tree.card_length)));
      crown.set("wood_class", JsonValue(tree.wood_class));
      crown.set("part_class", JsonValue(tree.part_class));
      crown.set("footprint_m2", JsonValue(static_cast<f64>(tree.footprint_m2)));
      crown.set("leaf_area_m2", JsonValue(static_cast<f64>(tree.leaf_area_m2)));
      crown.set("leaf_area_index", JsonValue(static_cast<f64>(tree.leaf_area_index)));
      summary.set("crown", std::move(crown));
    }
  } else if (have_report) {
    // What the budget bought (flora.md, "The budget"); a cache hit did not run the solver.
    JsonValue budget = JsonValue::object();
    budget.set("triangle_budget", JsonValue(file.palm.triangle_budget));
    budget.set("trunk_triangles", JsonValue(report.trunk_triangles));
    budget.set("crown_triangles", JsonValue(report.crown_triangles));
    budget.set("skirt_triangles", JsonValue(report.skirt_triangles));
    budget.set("rings", JsonValue(report.rings));
    budget.set("rows_per_ring", JsonValue(report.rows_per_ring));
    budget.set("trunk_segments", JsonValue(report.trunk_segments));
    budget.set("rachis_sides", JsonValue(report.rachis_sides));
    budget.set("leaflet_segments", JsonValue(report.leaflet_segments));
    budget.set("leaflets_per_side", JsonValue(report.leaflets_per_side));
    budget.set("steps", JsonValue(report.steps));
    summary.set("budget", std::move(budget));
  }
  summary.set("build_ms", JsonValue(static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e6));
  print_json(summary);
  return texture_failures > 0 ? k_exit_error : k_exit_ok;
}
#else
int refuse() {
  std::fputs(
      "engine-content flora: this build has no flora capability (ENGINE_WITH_FLORA is off, or the "
      "minimal build), so it cannot solve a flora file; docs/subsystems/flora.md\n",
      stderr);
  return k_exit_error;
}
#endif

}  // namespace

int flora_command(int argc, char** argv) {
  if (argc >= 3 && (std::string_view(argv[2]) == "--help" || std::string_view(argv[2]) == "-h")) {
    std::fputs(k_flora_usage, stdout);
    return k_exit_ok;
  }
#if ENGINE_CONTENT_FLORA
  return flora(argc, argv);
#else
  (void)argv;
  return refuse();
#endif
}

}  // namespace engine::content
