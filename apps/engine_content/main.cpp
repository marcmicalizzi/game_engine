// engine-content: the content build on the command line (docs/plan/07-content-pipeline.md §7.3).
// `build` imports a glTF 2.0 or GLB file (domain/assets), welds its vertices, builds one cluster
// LOD DAG per primitive so that every cluster has a single material (domain/geometry), merges
// them, and writes the result as a `.clusters` container together with the material table, the
// image paths the materials name, and the source path they are relative to. `--cache` writes
// that container into the derived-data cache instead, under the hash of the source and the
// build options, which is the path `engine-view --mesh` looks in before it builds anything.
// `info` prints what a container holds. Both print one JSON line on stdout, so scripts and
// agents read the numbers without parsing prose; everything else goes through the log to
// stderr.
//
// Exit codes: 0 ok; 1 the file could not be loaded, built, or written; 2 usage.
#include <core/json/json.h>
#include <core/log/log.h>
#include <core/platform/process.h>
#include <core/time/time.h>
#include <domain/assets/gltf.h>
#include <domain/geometry/cluster_file.h>
#include <foundation/io/vfs.h>

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
    "      --no-weld             keep the file's duplicate vertices\n"
    "      --cache               write into the derived-data cache instead of a named output,\n"
    "                            addressed by the source and the options above\n"
    "      --ddc <dir>           the cache root (default: <repo>/ddc, found beside AGENTS.md)\n"
    "      --log <spec>          log levels, e.g. \"info\" or \"warn,content=debug\"\n"
    "  info <file.clusters>                    print the header, sections, and counts\n"
    "\n"
    "examples:\n"
    "  engine-content build content/samples/Suzanne/Suzanne.gltf ddc/suzanne.clusters\n"
    "  engine-content build content/samples/Suzanne/Suzanne.gltf --cache\n"
    "  engine-content info ddc/suzanne.clusters\n";

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

struct BuildOptions {
  std::string input;
  std::string output;
  std::string log_spec;
  std::string ddc;
  u32 max_triangles = geometry::ClusterLodOptions{}.max_triangles;
  u32 max_vertices = geometry::ClusterLodOptions{}.max_vertices;
  bool weld = true;
  bool cache = false;
};

// The lod options the cache key is taken over, so that the key and the build agree by
// construction.
geometry::ClusterLodOptions lod_options_of(const BuildOptions& options) {
  geometry::ClusterLodOptions lod_options;
  lod_options.max_triangles = options.max_triangles;
  lod_options.max_vertices = options.max_vertices;
  return lod_options;
}

int build(const BuildOptions& options) {
  const i64 start_ns = time::monotonic_ns();
  // --cache: the output is where the derived-data cache wants this source with these options,
  // which is the same path engine-view looks in before it builds anything itself.
  std::string output = options.output;
  u64 source_hash = 0;
  if (options.cache) {
    std::string hash_error;
    if (!assets::source_mesh_hash(options.input, source_hash, &hash_error))
      return failed(hash_error);
    const u64 key = geometry::cluster_cache_key(source_hash, lod_options_of(options), options.weld);
    output = geometry::cluster_cache_path(options.ddc, key);
    const io::Status status = io::make_directories(io::parent_path(output));
    if (status != io::Status::Ok) {
      return failed("cannot create the cache directory '" + std::string(io::parent_path(output)) +
                    "': " + io::status_name(status));
    }
  }

  assets::MeshData mesh;
  std::string error;
  if (!assets::load_gltf(options.input, mesh, &error)) return failed(error);

  const u32 loaded_vertices = mesh.positions.size();
  u32 vertices = loaded_vertices;
  if (options.weld) {
    vertices = geometry::weld_vertices(mesh.positions, mesh.normals, mesh.uvs,
                                       std::span<u32>(mesh.indices.data(), mesh.indices.size()));
  }
  ENGINE_LOG_INFO(log_content, "mesh loaded", log::field("path", options.input),
                  log::field("vertices", loaded_vertices), log::field("welded", vertices),
                  log::field("triangles", mesh.indices.size() / 3),
                  log::field("primitives", mesh.primitives.size()),
                  log::field("materials", mesh.materials.size()),
                  log::field("images", mesh.images.size()));

  // One DAG per primitive, so every cluster belongs to exactly one material; merging keeps the
  // parts' clusters and remembers which part each came from.
  const geometry::ClusterLodOptions lod_options = lod_options_of(options);
  const geometry::AttributeSource attributes = assets::attribute_source(mesh);
  Vector<geometry::ClusterLodMesh> parts;
  Vector<i32> part_material;
  for (const assets::Primitive& primitive : mesh.primitives) {
    if (primitive.index_count < 3) continue;
    const std::span<const u32> range(mesh.indices.data() + primitive.first_index,
                                     primitive.index_count);
    geometry::ClusterLodMesh part;
    if (!geometry::build_cluster_lod(mesh.positions, range, lod_options, part, &error, attributes))
      return failed(error);
    parts.push_back(std::move(part));
    part_material.push_back(primitive.material);
  }
  if (parts.empty()) return failed("'" + options.input + "' has no triangles to cluster");

  geometry::ClusterFileData data;
  Vector<u32> part_of_cluster;
  if (!geometry::merge_cluster_lod(parts, data.mesh, &part_of_cluster, &error))
    return failed(error);

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
  // Image paths as the glTF gives them, relative to the glTF file; an image the file embedded
  // has no path of its own and keeps its slot with an empty one.
  u32 embedded_images = 0;
  data.image_paths.reserve(mesh.images.size());
  for (const assets::ImageRef& image : mesh.images) {
    if (image.uri.empty()) ++embedded_images;
    data.image_paths.push_back(image.uri);
  }

  // The source as it was given, so the renderer resolves the image paths above against its
  // directory however it came by the container.
  data.source_path = options.input;

  if (!geometry::write_cluster_file(output, data, &error)) return failed(error);
  const u64 content_hash = geometry::cluster_file_hash(data);
  const f64 build_ms = static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e6;
  io::FileInfo info;
  const u64 bytes = io::stat_file(output, info) == io::Status::Ok ? info.size : 0;

  const geometry::ClusterLodMesh& lod = data.mesh;
  ENGINE_LOG_INFO(log_content, "cluster file written", log::field("path", output),
                  log::field("clusters", lod.mesh.clusters.size()), log::field("bytes", bytes),
                  log::field("cached", options.cache), log::field("build_ms", build_ms));

  JsonValue summary = JsonValue::object();
  summary.set("path", JsonValue(output));
  summary.set("cached", JsonValue(options.cache));
  if (options.cache) summary.set("source_hash", JsonValue(source_hash));
  summary.set("clusters", JsonValue(lod.mesh.clusters.size()));
  summary.set("leaf_clusters",
              JsonValue(lod.level_cluster_counts.empty() ? 0u : lod.level_cluster_counts[0]));
  summary.set("triangles", JsonValue(lod.leaf_triangle_count));
  summary.set("lod_levels", JsonValue(lod.level_cluster_counts.size()));
  summary.set("vertices", JsonValue(lod.mesh.vertices.size()));
  summary.set("materials", JsonValue(data.materials.size()));
  summary.set("images", JsonValue(data.image_paths.size()));
  summary.set("embedded_images", JsonValue(embedded_images));
  summary.set("bytes", JsonValue(bytes));
  summary.set("build_ms", JsonValue(build_ms));
  summary.set("hash", JsonValue(content_hash));
  print_json(summary);
  return k_exit_ok;
}

int build_command(int argc, char** argv) {
  BuildOptions options;
  Vector<std::string> positional;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--max-triangles") {
      if (!next_u32(argc, argv, i, options.max_triangles)) return k_exit_usage;
    } else if (a == "--max-vertices") {
      if (!next_u32(argc, argv, i, options.max_vertices)) return k_exit_usage;
    } else if (a == "--log") {
      if (!next_value(argc, argv, i, options.log_spec)) return k_exit_usage;
    } else if (a == "--ddc") {
      if (!next_value(argc, argv, i, options.ddc)) return k_exit_usage;
    } else if (a == "--no-weld") {
      options.weld = false;
    } else if (a == "--cache") {
      options.cache = true;
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
  if (options.max_triangles < 4 || options.max_triangles > 256 || options.max_vertices < 1 ||
      options.max_vertices > 255) {
    return usage("--max-triangles is 4..256 and --max-vertices is 1..255");
  }

  log::StreamSink stderr_sink(stderr, log::StreamSink::Format::Text);
  start_logging(stderr_sink, options.log_spec);
  const int code = build(options);
  log::remove_sink(&stderr_sink);
  return code;
}

int info(const std::string& path) {
  std::string file;
  const io::Status status = io::read_file(path, file);
  if (status != io::Status::Ok)
    return failed("cannot read '" + path + "': " + io::status_name(status));
  geometry::ClusterFileData data;
  std::string error;
  const std::span<const u8> bytes(reinterpret_cast<const u8*>(file.data()), file.size());
  if (!geometry::read_cluster_file_memory(bytes, data, &error)) return failed(error);

  // The table is read straight from the bytes, so `info` reports the sections a newer build
  // wrote as well as the ones this build understands.
  geometry::ClusterFileHeader header;
  std::memcpy(&header, file.data(), sizeof(header));
  JsonValue sections = JsonValue::array();
  for (u32 i = 0; i < header.section_count; ++i) {
    geometry::ClusterFileSection section;
    std::memcpy(&section, file.data() + sizeof(header) + sizeof(section) * i, sizeof(section));
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
  summary.set("images", JsonValue(data.image_paths.size()));
  summary.set("source_path", JsonValue(data.source_path));
  print_json(summary);
  return k_exit_ok;
}

int info_command(int argc, char** argv) {
  std::string log_spec;
  Vector<std::string> positional;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--log") {
      if (!next_value(argc, argv, i, log_spec)) return k_exit_usage;
    } else if (!a.empty() && a[0] == '-') {
      return usage("unknown option for info");
    } else {
      positional.push_back(std::string(a));
    }
  }
  if (positional.size() != 1) return usage("info takes one cluster file");

  log::StreamSink stderr_sink(stderr, log::StreamSink::Format::Text);
  start_logging(stderr_sink, log_spec);
  const int code = info(positional[0]);
  log::remove_sink(&stderr_sink);
  return code;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage(nullptr);
  const std::string_view command = argv[1];
  if (command == "--help" || command == "-h" || command == "help") {
    std::fputs(k_usage, stdout);
    return k_exit_ok;
  }
  if (command == "build") return build_command(argc, argv);
  if (command == "info") return info_command(argc, argv);
  return usage("unknown command");
}
