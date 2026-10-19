// engine-content rocks (content_commands.h; docs/subsystems/rocks.md, "The build"): a rock file —
// a shape and its parameters — solved and built into a `.clusters` container through the
// derived-data cache, keyed by the canonical parameters and the solver's version, so a rock is
// built once per parameter set and the renderer's scene reader finds what this wrote; and, with
// `--glb`, the solved mesh as a GLB, which is what E10's harness (tools/e10-harness.ps1) takes.
// Present when this build has the rocks capability; refused with a sentence otherwise.
#include "content_commands.h"

#include <core/base/types.h>
#include <core/json/json.h>

#include <cstdio>
#include <string>
#include <string_view>

#if ENGINE_CONTENT_ROCKS
#include <core/containers/vector.h>
#include <core/platform/process.h>
#include <core/time/time.h>
#include <domain/content_build/content_build.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/rocks/rocks.h>
#include <foundation/io/vfs.h>

#include <cstring>
#endif

namespace engine::content {

namespace {

constexpr int k_exit_ok = 0;
constexpr int k_exit_error = 1;
constexpr int k_exit_usage = 2;

const char* k_rocks_usage =
    "usage: engine-content rocks <params.rock.json> <out.clusters> [options]\n"
    "       engine-content rocks <params.rock.json> --cache [options]\n"
    "       engine-content rocks <params.rock.json> --glb <out.glb>\n"
    "  solve a rock file (engine.rocks.RockFile: a boulder, an outcrop or scree and its\n"
    "  parameters) and build the mesh into a cluster container, keyed by the canonical parameters\n"
    "  and the solver's version; an output that already records that key is not built again\n"
    "      --cache        write <ddc>/clusters/<key>.clusters instead of a named output: the\n"
    "                     entry engine-view --mesh and a scene's mesh table read\n"
    "      --ddc <dir>    the cache root (default: <repo>/ddc, found beside AGENTS.md); implies\n"
    "                     --cache when no output is named\n"
    "      --force        build even when the output is already the answer\n"
    "      --glb <file>   write the solved mesh as a GLB instead (positions, normals, UVs, the\n"
    "                     material's colour): what tools/e10-harness.ps1 judges\n";

[[maybe_unused]] int usage(const char* message) {
  if (message != nullptr) std::fprintf(stderr, "engine-content rocks: %s\n", message);
  std::fputs(k_rocks_usage, stderr);
  return k_exit_usage;
}

[[maybe_unused]] int failed(const std::string& message) {
  std::fprintf(stderr, "engine-content rocks: %s\n", message.c_str());
  return k_exit_error;
}

#if ENGINE_CONTENT_ROCKS
void print_json(const JsonValue& value) {
  std::string text = write_json(value, JsonWriteOptions{.pretty = false});
  text.push_back('\n');
  std::fwrite(text.data(), 1, text.size(), stdout);
}

void put_u32(Vector<u8>& out, u32 v) {
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>(v >> (8 * i)));
}

template <typename T>
void put_array(Vector<u8>& out, const Vector<T>& values) {
  const u32 at = out.size();
  out.resize(at + values.size() * static_cast<u32>(sizeof(T)));
  if (!values.empty()) std::memcpy(out.data() + at, values.data(), values.size() * sizeof(T));
}

std::string number(f64 v) {
  JsonWriteOptions compact;
  compact.pretty = false;
  return write_json(JsonValue(v), compact);
}

// The mesh as a GLB: one node, one mesh, one primitive, its material's colour and roughness.
Vector<u8> mesh_glb(const assets::MeshData& mesh) {
  Vector<u8> bin;
  put_array(bin, mesh.positions);
  const u32 normal_offset = bin.size();
  put_array(bin, mesh.normals);
  const u32 uv_offset = bin.size();
  put_array(bin, mesh.uvs);
  const u32 index_offset = bin.size();
  put_array(bin, mesh.indices);
  Vec3 lo{1e30f, 1e30f, 1e30f};
  Vec3 hi{-1e30f, -1e30f, -1e30f};
  for (const Vec3& p : mesh.positions) {
    lo = Vec3{p.x < lo.x ? p.x : lo.x, p.y < lo.y ? p.y : lo.y, p.z < lo.z ? p.z : lo.z};
    hi = Vec3{p.x > hi.x ? p.x : hi.x, p.y > hi.y ? p.y : hi.y, p.z > hi.z ? p.z : hi.z};
  }
  const assets::Material& m = mesh.materials[0];
  const auto n = [](u64 v) { return std::to_string(v); };
  const auto f = [](f32 v) { return number(static_cast<f64>(v)); };
  const std::string vertices = n(mesh.positions.size());
  std::string json =
      "{\"asset\":{\"version\":\"2.0\",\"generator\":\"engine-content rocks\"},\"scene\":0,"
      "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
      "\"materials\":[{\"name\":\"" +
      m.name + "\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[" + f(m.base_color.x) + "," +
      f(m.base_color.y) + "," + f(m.base_color.z) + "," + f(m.base_color.w) +
      "],\"metallicFactor\":" + f(m.metallic) + ",\"roughnessFactor\":" + f(m.roughness) +
      "}}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},"
      "\"indices\":3,\"material\":0}]}],"
      "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":" +
      vertices + ",\"type\":\"VEC3\",\"min\":[" + f(lo.x) + "," + f(lo.y) + "," + f(lo.z) +
      "],\"max\":[" + f(hi.x) + "," + f(hi.y) + "," + f(hi.z) +
      "]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":" +
      vertices +
      ",\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":" +
      vertices +
      ",\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"componentType\":5125,\"count\":" +
      n(mesh.indices.size()) +
      ",\"type\":\"SCALAR\"}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
      n(normal_offset) + "},{\"buffer\":0,\"byteOffset\":" + n(normal_offset) +
      ",\"byteLength\":" + n(uv_offset - normal_offset) +
      "},{\"buffer\":0,\"byteOffset\":" + n(uv_offset) +
      ",\"byteLength\":" + n(index_offset - uv_offset) +
      "},{\"buffer\":0,\"byteOffset\":" + n(index_offset) +
      ",\"byteLength\":" + n(bin.size() - index_offset) +
      "}],\"buffers\":[{\"byteLength\":" + n(bin.size()) + "}]}";
  while (json.size() % 4 != 0)
    json += ' ';
  Vector<u8> glb;
  put_u32(glb, 0x46546c67u);  // "glTF"
  put_u32(glb, 2u);
  put_u32(glb, static_cast<u32>(12 + 8 + json.size() + 8 + bin.size()));
  put_u32(glb, static_cast<u32>(json.size()));
  put_u32(glb, 0x4e4f534au);  // "JSON"
  for (const char c : json)
    glb.push_back(static_cast<u8>(c));
  put_u32(glb, bin.size());
  put_u32(glb, 0x004e4942u);  // "BIN\0"
  for (const u8 byte : bin)
    glb.push_back(byte);
  return glb;
}

JsonValue report_json(const rocks::RockReport& report, u32 budget) {
  JsonValue budget_json = JsonValue::object();
  budget_json.set("triangle_budget", JsonValue(budget));
  budget_json.set("triangles", JsonValue(report.triangles));
  budget_json.set("parts", JsonValue(report.parts));
  budget_json.set("finest_grid", JsonValue(report.finest_grid));
  budget_json.set("coarsest_grid", JsonValue(report.coarsest_grid));
  budget_json.set("steps", JsonValue(report.steps));
  budget_json.set("cuts", JsonValue(report.cuts));
  budget_json.set("layers", JsonValue(report.layers));
  budget_json.set("rounding", JsonValue(static_cast<f64>(report.rounding)));
  return budget_json;
}

u32 budget_of(const rocks::RockFile& file) {
  switch (file.shape) {
    case rocks::RockShape::Boulder: return file.boulder->triangle_budget;
    case rocks::RockShape::Outcrop: return file.outcrop->triangle_budget;
    case rocks::RockShape::Scree: return file.scree->triangle_budget;
  }
  return 0;
}

int rocks_build(int argc, char** argv) {
  std::string input;
  std::string output;
  std::string ddc;
  std::string glb;
  bool cache = false;
  bool force = false;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--cache") {
      cache = true;
    } else if (a == "--force") {
      force = true;
    } else if (a == "--ddc") {
      if (i + 1 >= argc) return usage("--ddc needs a directory");
      ddc = argv[++i];
    } else if (a == "--glb") {
      if (i + 1 >= argc) return usage("--glb needs a file");
      glb = argv[++i];
    } else if (!a.empty() && a[0] == '-') {
      return usage("unknown option");
    } else if (input.empty()) {
      input = std::string(a);
    } else if (output.empty()) {
      output = std::string(a);
    } else {
      return usage("one rock file and at most one output");
    }
  }
  if (input.empty()) return usage("a rock file is needed");
  if (!glb.empty() && (cache || !output.empty() || !ddc.empty()))
    return usage("--glb takes no container output");
  if (glb.empty()) {
    if (output.empty() && !ddc.empty()) cache = true;
    if (cache == !output.empty()) {
      return usage(cache ? "--cache takes no output file" : "an output file or --cache is needed");
    }
    if (cache && ddc.empty()) {
      ddc = geometry::find_ddc_root(platform::executable_directory());
      if (ddc.empty())
        return usage("--cache found no repository root above the executable; pass --ddc");
    }
  }

  const i64 start_ns = time::monotonic_ns();
  std::string text;
  const io::Status status = io::read_file(input, text);
  if (status != io::Status::Ok)
    return failed("cannot read " + input + ": " + io::status_name(status));
  rocks::RockFile file;
  std::string error;
  if (!rocks::read_rock_file(text, file, error)) return failed(input + ": " + error);

  if (!glb.empty()) {
    assets::MeshData mesh;
    rocks::RockReport report;
    if (!rocks::solve_rock(file, mesh, &report, &error)) return failed(input + ": " + error);
    const Vector<u8> bytes = mesh_glb(mesh);
    const io::Status wrote = io::write_file_atomic(
        glb, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (wrote != io::Status::Ok)
      return failed("cannot write " + glb + ": " + io::status_name(wrote));
    JsonValue summary = JsonValue::object();
    summary.set("path", JsonValue(glb));
    summary.set("shape", JsonValue(rocks::rock_shape_name(file.shape)));
    summary.set("triangles", JsonValue(report.triangles));
    summary.set("vertices", JsonValue(report.vertices));
    summary.set("budget", report_json(report, budget_of(file)));
    print_json(summary);
    return k_exit_ok;
  }

  // `build`'s defaults, which are the renderer's: the two apps share an entry only when they agree
  // on the options the key is taken over.
  const geometry::ClusterLodOptions lod =
      content_build::lod_options_of(content_build::MeshOptions{});
  const u32 page_bytes = geometry::ClusterPagesOptions{}.page_bytes;
  const u64 source_hash = rocks::rock_source_hash(file);
  const u64 build_key = rocks::rock_build_key(source_hash, lod, page_bytes);
  const std::string path = cache ? geometry::cluster_cache_path(ddc, build_key) : output;

  bool cached = false;
  geometry::ClusterFileData data;
  if (!force && io::exists(path)) {
    std::string read_error;
    cached = geometry::read_cluster_file(path, data, &read_error) &&
             data.source_hash == source_hash && data.build_key == build_key;
  }
  rocks::RockReport report;
  bool have_report = false;
  if (!cached) {
    if (!rocks::build_rock_container(file, lod, page_bytes, data, &report, error))
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
  summary.set("shape", JsonValue(rocks::rock_shape_name(file.shape)));
  summary.set("source_hash", JsonValue(source_hash));
  summary.set("build_key", JsonValue(build_key));
  summary.set("rocks_version", JsonValue(rocks::k_rocks_version));
  summary.set("triangles", JsonValue(data.mesh.leaf_triangle_count));
  summary.set("clusters", JsonValue(data.mesh.mesh.clusters.size()));
  summary.set("lod_levels", JsonValue(data.mesh.level_cluster_counts.size()));
  summary.set("vertices", JsonValue(data.mesh.mesh.vertices.size()));
  summary.set("materials", JsonValue(data.materials.size()));
  summary.set("pages", JsonValue(data.pages.pages.size()));
  // What the budget bought (rocks.md, "The budget ladder"); a cache hit did not run the solver.
  if (have_report) summary.set("budget", report_json(report, budget_of(file)));
  summary.set("build_ms", JsonValue(static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e6));
  print_json(summary);
  return k_exit_ok;
}
#else
int refuse() {
  std::fputs(
      "engine-content rocks: this build has no rocks capability (ENGINE_WITH_ROCKS is off, or the "
      "minimal build), so it cannot solve a rock file; docs/subsystems/rocks.md\n",
      stderr);
  return k_exit_error;
}
#endif

}  // namespace

int rocks_command(int argc, char** argv) {
  if (argc >= 3 && (std::string_view(argv[2]) == "--help" || std::string_view(argv[2]) == "-h")) {
    std::fputs(k_rocks_usage, stdout);
    return k_exit_ok;
  }
#if ENGINE_CONTENT_ROCKS
  return rocks_build(argc, argv);
#else
  (void)argv;
  return refuse();
#endif
}

}  // namespace engine::content
