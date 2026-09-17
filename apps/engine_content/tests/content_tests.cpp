// End to end: engine-content turns a glTF file into a .clusters container and reads it back,
// builds a manifest of them incrementally and in parallel, refuses a mesh that breaks a
// validation rule, and reports a container's content-build metrics.
//
// The fixture is a unit cube written as a GLB at test time (its twelve triangles split into any
// number of primitives that divides them, one material each, the first textured with a 4x4
// checker PNG in the BIN chunk), so nothing binary lives in the tree and the test does not
// depend on the sample models tools/fetch-samples.ps1 downloads.
#include <core/json/json.h>
#include <core/math/math.h>
#include <core/platform/process.h>
#include <domain/assets/gltf.h>
#include <domain/geometry/cluster_file.h>
#include <foundation/image/png.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace engine;

namespace {

struct Run {
  i32 exit_code = -1;
  std::string output;
  JsonValue result;              // parsed stdout when it was one JSON document
  std::vector<JsonValue> lines;  // parsed stdout line by line, for the commands that stream
};

Run content(std::vector<std::string> args, bool merge_stderr = false) {
  std::vector<std::string_view> argv;
  argv.push_back(ENGINE_APP_PATH);
  for (const std::string& a : args)
    argv.push_back(a);
  platform::Process p;
  std::string error;
  Run run;
  if (!p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error, merge_stderr)) {
    FAIL("cannot spawn engine-content: " << error);
    return run;
  }
  p.close_stdin();
  p.read_all(run.output);
  run.exit_code = p.wait();
  if (!run.output.empty()) (void)parse_json(run.output, run.result);
  // One JSON object per line is the contract for build-all, so the lines are parsed separately.
  usize at = 0;
  while (at < run.output.size()) {
    usize end = run.output.find('\n', at);
    if (end == std::string::npos) end = run.output.size();
    std::string_view line(run.output.data() + at, end - at);
    while (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    if (!line.empty()) {
      JsonValue value;
      if (parse_json(line, value).ok && value.is_object()) run.lines.push_back(std::move(value));
    }
    at = end + 1;
  }
  return run;
}

u64 number(const JsonValue& object, const char* key) {
  const JsonValue* value = object.find(key);
  u64 out = 0;
  return value != nullptr && value->get_u64(out) ? out : ~u64{0};
}

std::string text_of(const JsonValue& object, const char* key) {
  const JsonValue* value = object.find(key);
  return value != nullptr && value->is_string() ? std::string(value->as_string()) : std::string();
}

void put_u32(std::vector<u8>& out, u32 v) {
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>((v >> (8 * i)) & 0xffu));
}

void put_u16(std::vector<u8>& out, u16 v) {
  out.push_back(static_cast<u8>(v & 0xffu));
  out.push_back(static_cast<u8>(v >> 8));
}

void put_f32(std::vector<u8>& out, f32 v) {
  u32 bits = 0;
  std::memcpy(&bits, &v, 4);
  put_u32(out, bits);
}

void pad4(std::vector<u8>& out, u8 fill) {
  while (out.size() % 4 != 0)
    out.push_back(fill);
}

std::string n(u32 v) { return std::to_string(v); }

// How the cube is written: how many primitives its twelve triangles are split into (any divisor
// of twelve), a scale that changes the bytes without changing anything else, whether the first
// vertex is at a position that is not finite, and whether the texture is an external file that
// is not there instead of the PNG in the BIN chunk.
struct CubeOptions {
  u32 primitives = 2;
  f32 scale = 1.0f;
  bool nan_position = false;
  bool external_image = false;
};

// The six base colors the generated materials cycle through; the first is the textured one.
const char* k_base_colors[6] = {"[1,1,1,1]",         "[0.9,0.15,0.1,1]", "[0.15,0.8,0.25,1]",
                                "[0.2,0.35,0.95,1]", "[0.95,0.8,0.1,1]", "[0.6,0.2,0.8,1]"};

// A unit cube as a GLB: 24 vertices with normals and UVs, twelve triangles split into
// `primitives` index accessors with one material each, and a 4x4 checker PNG in the BIN chunk.
// The same fixture apps/engine_view/tests/mesh_view_tests.cpp renders.
bool write_cube_glb(const std::string& path, const CubeOptions& options = {}) {
  if (options.primitives == 0 || options.primitives > 6 || 12 % options.primitives != 0)
    return false;
  const Vec3 normals[6] = {Vec3{1, 0, 0},  Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                           Vec3{0, -1, 0}, Vec3{0, 0, 1},  Vec3{0, 0, -1}};
  const Vec3 tangents[6] = {Vec3{0, 1, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1},
                            Vec3{0, 0, 1}, Vec3{1, 0, 0}, Vec3{1, 0, 0}};
  std::vector<u8> bin;
  const f32 h = 0.5f * options.scale;
  for (u32 f = 0; f < 6; ++f) {  // positions: corners wound counter-clockwise from outside
    const Vec3 nrm = normals[f];
    const Vec3 t = tangents[f];
    const Vec3 b = cross(nrm, t);
    const Vec3 corners[4] = {nrm * h - t * h - b * h, nrm * h + t * h - b * h,
                             nrm * h + t * h + b * h, nrm * h - t * h + b * h};
    for (const Vec3& c : corners) {
      put_f32(bin, c.x);
      put_f32(bin, c.y);
      put_f32(bin, c.z);
    }
  }
  if (options.nan_position) {
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    u32 bits = 0;
    std::memcpy(&bits, &nan, 4);
    for (u32 i = 0; i < 4; ++i)
      bin[i] = static_cast<u8>((bits >> (8 * i)) & 0xffu);
  }
  const u32 normal_offset = static_cast<u32>(bin.size());
  for (u32 f = 0; f < 6; ++f) {
    for (u32 c = 0; c < 4; ++c) {
      put_f32(bin, normals[f].x);
      put_f32(bin, normals[f].y);
      put_f32(bin, normals[f].z);
    }
  }
  const u32 uv_offset = static_cast<u32>(bin.size());
  for (u32 f = 0; f < 6; ++f) {
    const f32 uvs[8] = {0, 0, 1, 0, 1, 1, 0, 1};
    for (const f32 v : uvs)
      put_f32(bin, v);
  }
  const u32 index_offset = static_cast<u32>(bin.size());
  for (u32 f = 0; f < 6; ++f) {
    const u16 base = static_cast<u16>(f * 4);
    const u16 tris[6] = {base, static_cast<u16>(base + 1), static_cast<u16>(base + 2),
                         base, static_cast<u16>(base + 2), static_cast<u16>(base + 3)};
    for (const u16 i : tris)
      put_u16(bin, i);
  }
  const u32 index_bytes = static_cast<u32>(bin.size()) - index_offset;
  pad4(bin, 0);
  const u32 png_offset = static_cast<u32>(bin.size());
  Vector<u8> pixels(4 * 4 * 4);
  for (u32 y = 0; y < 4; ++y) {
    for (u32 x = 0; x < 4; ++x) {
      const bool a = ((x + y) & 1) == 0;
      u8* p = &pixels[(y * 4 + x) * 4];
      p[0] = a ? 40 : 240;
      p[1] = a ? 90 : 220;
      p[2] = a ? 230 : 40;
      p[3] = 255;
    }
  }
  Vector<u8> png;
  if (!image::encode_png(4, 4, 4, std::span<const u8>(pixels.data(), pixels.size()), png))
    return false;
  bin.insert(bin.end(), png.begin(), png.end());
  const u32 png_bytes = png.size();
  pad4(bin, 0);

  // One index accessor and one material per primitive, all over the same index buffer view.
  const u32 group_indices = 36 / options.primitives;
  std::string primitives;
  std::string materials;
  std::string index_accessors;
  for (u32 g = 0; g < options.primitives; ++g) {
    if (g != 0) {
      primitives += ',';
      materials += ',';
      index_accessors += ',';
    }
    primitives +=
        "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":" + n(3 + g) +
        ",\"material\":" + n(g) + "}";
    materials += "{\"pbrMetallicRoughness\":{\"baseColorFactor\":";
    materials += k_base_colors[g];
    if (g == 0) materials += ",\"baseColorTexture\":{\"index\":0}";
    materials += ",\"metallicFactor\":0,\"roughnessFactor\":";
    materials += g == 0 ? "0.8" : "0.5";
    materials += "}}";
    index_accessors += "{\"bufferView\":3,\"byteOffset\":" + n(g * group_indices * 2) +
                       ",\"componentType\":5123,\"count\":" + n(group_indices) +
                       ",\"type\":\"SCALAR\"}";
  }

  const std::string image = options.external_image
                                ? "{\"uri\":\"textures/not_there.png\"}"
                                : "{\"bufferView\":4,\"mimeType\":\"image/png\"}";
  std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":[" +
      primitives +
      "]}],"
      "\"materials\":[" +
      materials +
      "],"
      "\"textures\":[{\"source\":0}],"
      "\"images\":[" +
      image +
      "],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\","
      "\"min\":[-0.5,-0.5,-0.5],\"max\":[0.5,0.5,0.5]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":24,\"type\":\"VEC2\"}," +
      index_accessors +
      "],"
      "\"bufferViews\":["
      "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
      n(normal_offset) + "},{\"buffer\":0,\"byteOffset\":" + n(normal_offset) +
      ",\"byteLength\":" + n(uv_offset - normal_offset) +
      "},{\"buffer\":0,\"byteOffset\":" + n(uv_offset) +
      ",\"byteLength\":" + n(index_offset - uv_offset) +
      "},{\"buffer\":0,\"byteOffset\":" + n(index_offset) + ",\"byteLength\":" + n(index_bytes) +
      "},{\"buffer\":0,\"byteOffset\":" + n(png_offset) + ",\"byteLength\":" + n(png_bytes) +
      "}],\"buffers\":[{\"byteLength\":" + n(static_cast<u32>(bin.size())) + "}]}";
  while (json.size() % 4 != 0)
    json += ' ';

  std::vector<u8> glb;
  put_u32(glb, 0x46546c67u);  // "glTF"
  put_u32(glb, 2u);
  put_u32(glb, static_cast<u32>(12 + 8 + json.size() + 8 + bin.size()));
  put_u32(glb, static_cast<u32>(json.size()));
  put_u32(glb, 0x4e4f534au);  // "JSON"
  glb.insert(glb.end(), json.begin(), json.end());
  put_u32(glb, static_cast<u32>(bin.size()));
  put_u32(glb, 0x004e4942u);  // "BIN\0"
  glb.insert(glb.end(), bin.begin(), bin.end());
  std::ofstream f(path, std::ios::binary);
  if (!f.is_open()) return false;
  f.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
  return f.good();
}

std::string slashes(const std::filesystem::path& p) {
  std::string s = p.string();
  for (char& c : s) {
    if (c == '\\') c = '/';
  }
  return s;
}

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  if (!f.is_open()) return false;
  f << text;
  return f.good();
}

std::string file_bytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << f.rdbuf();
  return buffer.str();
}

// The source index list validate_cluster_lod wants for a merged mesh: each primitive's indices
// shifted by the source vertex counts of the primitives before it, which is how
// engine-content's per-primitive DAGs were merged.
bool merged_source_indices(const std::string& mesh_path, Vector<u32>& out) {
  assets::MeshData mesh;
  std::string error;
  if (!assets::load_gltf(mesh_path, mesh, &error)) return false;
  geometry::weld_vertices(mesh.positions, mesh.normals, mesh.uvs,
                          std::span<u32>(mesh.indices.data(), mesh.indices.size()));
  const u32 vertices = mesh.positions.size();
  u32 base = 0;
  for (const assets::Primitive& primitive : mesh.primitives) {
    if (primitive.index_count < 3) continue;
    for (u32 i = 0; i < primitive.index_count; ++i)
      out.push_back(mesh.indices[primitive.first_index + i] + base);
    base += vertices;
  }
  return true;
}

}  // namespace

TEST_CASE("engine-content: build writes a cluster file that reads back and validates") {
  const auto dir = std::filesystem::temp_directory_path() / "engine_content_tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string mesh = slashes(dir / "cube.glb");
  const std::string out = slashes(dir / "cube.clusters");
  REQUIRE(write_cube_glb(mesh));

  const Run built = content({"build", mesh, out});
  REQUIRE_MESSAGE(built.exit_code == 0, built.output);
  REQUIRE_MESSAGE(built.result.is_object(), built.output);
  CHECK(number(built.result, "triangles") == 12);
  CHECK(number(built.result, "leaf_clusters") == 2);  // six triangles per material, one each
  CHECK(number(built.result, "clusters") >= 2);
  CHECK(number(built.result, "lod_levels") >= 1);
  CHECK(number(built.result, "vertices") == 24);
  CHECK(number(built.result, "materials") == 2);
  CHECK(number(built.result, "images") == 1);
  CHECK(number(built.result, "embedded_images") == 1);  // the checker PNG is in the BIN chunk
  CHECK(number(built.result, "warnings") == 0);
  CHECK(number(built.result, "bytes") > 0);
  CHECK(number(built.result, "bytes") == std::filesystem::file_size(out));
  CHECK(built.result.find("build_ms") != nullptr);
  CHECK(number(built.result, "hash") != 0);

  // The container records what it was built from, whatever the destination was.
  u64 source_hash = 0;
  std::string error;
  REQUIRE_MESSAGE(assets::source_mesh_hash(mesh, source_hash, &error), error);
  const u64 key =
      geometry::cluster_cache_key(source_hash, geometry::ClusterLodOptions{}, /*weld=*/true);
  CHECK(number(built.result, "source_hash") == source_hash);
  CHECK(number(built.result, "build_key") == key);

  geometry::ClusterFileData data;
  REQUIRE_MESSAGE(geometry::read_cluster_file(out, data, &error), error);
  CHECK(data.mesh.mesh.clusters.size() == number(built.result, "clusters"));
  CHECK(data.mesh.leaf_triangle_count == 12);
  CHECK(data.materials.size() == 2);
  CHECK(data.cluster_material.size() == data.mesh.mesh.clusters.size());
  CHECK(data.image_paths.size() == 1);
  CHECK(data.image_paths[0].empty());  // an embedded image keeps its slot with an empty path
  CHECK(data.source_path == mesh);     // the glTF those paths would have been relative to
  CHECK(data.source_hash == source_hash);
  CHECK(data.build_key == key);
  CHECK(data.materials[0].base_color_image == 0);
  CHECK(data.materials[1].base_color_image == -1);
  CHECK(data.materials[1].base_color.x == doctest::Approx(0.9f));
  CHECK(geometry::cluster_file_hash(data) == number(built.result, "hash"));
  // Both materials are used, one per primitive.
  bool used[2] = {false, false};
  for (const u32 material : data.cluster_material) {
    REQUIRE(material < 2);
    used[material] = true;
  }
  CHECK(used[0]);
  CHECK(used[1]);

  Vector<u32> source_indices;
  REQUIRE(merged_source_indices(mesh, source_indices));
  CHECK_MESSAGE(geometry::validate_cluster_lod(data.mesh, source_indices, &error), error);

  // info names every section of the file it just wrote.
  const Run described = content({"info", out});
  REQUIRE_MESSAGE(described.exit_code == 0, described.output);
  REQUIRE_MESSAGE(described.result.is_object(), described.output);
  CHECK(number(described.result, "version") == geometry::k_cluster_file_version);
  CHECK(number(described.result, "total_bytes") == number(built.result, "bytes"));
  CHECK(number(described.result, "hash") == number(built.result, "hash"));
  CHECK(number(described.result, "clusters") == number(built.result, "clusters"));
  CHECK(number(described.result, "materials") == 2);
  CHECK(number(described.result, "source_hash") == source_hash);
  CHECK(number(described.result, "build_key") == key);
  REQUIRE(described.result.find("source_path") != nullptr);
  CHECK(described.result.find("source_path")->as_string() == mesh);
  const JsonValue* sections = described.result.find("sections");
  REQUIRE(sections != nullptr);
  REQUIRE(sections->is_array());
  CHECK(sections->size() == 15);
  for (const char* name :
       {"clusters", "lod", "vertices", "attributes", "triangles", "vertex_source",
        "level_cluster_counts", "cluster_material", "materials", "image_paths", "strings",
        "scalars", "quantized", "source_path", "source_hash"}) {
    bool found = false;
    for (usize i = 0; i < sections->size(); ++i) {
      const JsonValue* section_name = (*sections)[i].find("name");
      if (section_name != nullptr && section_name->as_string() == name) found = true;
    }
    CHECK_MESSAGE(found, "info does not name the " << name << " section");
  }

  // --no-weld keeps the file's duplicate vertices, --max-triangles makes smaller clusters.
  const Run unwelded = content({"build", mesh, slashes(dir / "unwelded.clusters"), "--no-weld"});
  REQUIRE_MESSAGE(unwelded.exit_code == 0, unwelded.output);
  CHECK(number(unwelded.result, "triangles") == 12);
  const Run tight =
      content({"build", mesh, slashes(dir / "tight.clusters"), "--max-triangles", "4"});
  REQUIRE_MESSAGE(tight.exit_code == 0, tight.output);
  CHECK(number(tight.result, "leaf_clusters") >= 4);  // twelve triangles, four per cluster

  std::filesystem::copy_file(
      dir / "cube.clusters",
      std::filesystem::temp_directory_path() / "engine_content_cube.clusters",
      std::filesystem::copy_options::overwrite_existing);
  std::filesystem::remove_all(dir);
}

TEST_CASE("engine-content: --cache writes the container the source's hash addresses") {
  const auto dir = std::filesystem::temp_directory_path() / "engine_content_cache_tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string mesh = slashes(dir / "cube.glb");
  const std::string ddc = slashes(dir / "ddc");
  REQUIRE(write_cube_glb(mesh));

  // The path the app must choose, computed here from the same two helpers the renderer uses.
  u64 source_hash = 0;
  std::string error;
  REQUIRE_MESSAGE(assets::source_mesh_hash(mesh, source_hash, &error), error);
  const std::string expected = geometry::cluster_cache_path(
      ddc, geometry::cluster_cache_key(source_hash, geometry::ClusterLodOptions{}, true));

  const Run cached = content({"build", mesh, "--cache", "--ddc", ddc});
  REQUIRE_MESSAGE(cached.exit_code == 0, cached.output);
  REQUIRE_MESSAGE(cached.result.is_object(), cached.output);
  const JsonValue* path = cached.result.find("path");
  REQUIRE_MESSAGE(path != nullptr, cached.output);
  CHECK(path->as_string() == expected);  // the app prints where it put it
  REQUIRE_MESSAGE(std::filesystem::exists(expected), expected);
  CHECK(std::filesystem::file_size(expected) == number(cached.result, "bytes"));
  CHECK(number(cached.result, "source_hash") == source_hash);
  const JsonValue* flag = cached.result.find("cached");
  REQUIRE(flag != nullptr);
  CHECK(flag->as_bool());

  // The container names the source, so a renderer that finds it in the cache still resolves
  // the image paths relative to the glTF rather than to the cache directory.
  geometry::ClusterFileData data;
  REQUIRE_MESSAGE(geometry::read_cluster_file(expected, data, &error), error);
  CHECK(data.source_path == mesh);
  CHECK(data.mesh.leaf_triangle_count == 12);

  // The same source and options land on the same entry; a different limit does not.
  const Run again = content({"build", mesh, "--cache", "--ddc", ddc});
  REQUIRE_MESSAGE(again.exit_code == 0, again.output);
  REQUIRE(again.result.find("path") != nullptr);
  CHECK(again.result.find("path")->as_string() == expected);
  const Run tighter = content({"build", mesh, "--cache", "--ddc", ddc, "--max-triangles", "8"});
  REQUIRE_MESSAGE(tighter.exit_code == 0, tighter.output);
  REQUIRE(tighter.result.find("path") != nullptr);
  CHECK(tighter.result.find("path")->as_string() != expected);
  CHECK(std::filesystem::exists(std::string(tighter.result.find("path")->as_string())));

  // Two destinations for one build is a usage error, in either direction.
  CHECK(content({"build", mesh, slashes(dir / "out.clusters"), "--cache", "--ddc", ddc}, true)
            .exit_code == 2);
  CHECK(content({"build", mesh, "--ddc", ddc}, true).exit_code == 2);

  std::filesystem::remove_all(dir);
}

TEST_CASE("engine-content: the same mesh builds the same bytes whatever --jobs says") {
  const auto dir = std::filesystem::temp_directory_path() / "engine_content_jobs_tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  // Six primitives, so six per-primitive DAG builds race and the merge has an order to get
  // wrong; it must merge by primitive index, never by which job finished first.
  const std::string mesh = slashes(dir / "cube6.glb");
  REQUIRE(write_cube_glb(mesh, CubeOptions{.primitives = 6}));

  const std::string serial = slashes(dir / "serial.clusters");
  const std::string parallel = slashes(dir / "parallel.clusters");
  const Run one = content({"build", mesh, serial, "--jobs", "1"});
  REQUIRE_MESSAGE(one.exit_code == 0, one.output);
  const Run eight = content({"build", mesh, parallel, "--jobs", "8"});
  REQUIRE_MESSAGE(eight.exit_code == 0, eight.output);
  CHECK(number(one.result, "leaf_clusters") == 6);  // one cluster per two-triangle primitive
  CHECK(number(one.result, "materials") == 6);
  CHECK(number(one.result, "hash") == number(eight.result, "hash"));
  const std::string serial_bytes = file_bytes(serial);
  CHECK(serial_bytes.size() > 0);
  CHECK(serial_bytes == file_bytes(parallel));

  // And the same through build-all, where the parallelism is one job per mesh instead: the two
  // entries name the same source with the same options, so their containers are the same bytes
  // as each other and the same bytes at either thread count.
  const std::string manifest = slashes(dir / "meshes.json");
  REQUIRE(write_text(manifest,
                     "{\"meshes\":[{\"source\":\"cube6.glb\",\"output\":\"a.clusters\"},"
                     "{\"source\":\"cube6.glb\",\"output\":\"b.clusters\"}]}"));
  const Run all_one = content({"build-all", manifest, "--jobs", "1"});
  REQUIRE_MESSAGE(all_one.exit_code == 0, all_one.output);
  const std::string a = file_bytes(slashes(dir / "a.clusters"));
  CHECK(a.size() > 0);
  CHECK(a == file_bytes(slashes(dir / "b.clusters")));
  std::filesystem::remove(dir / "a.clusters");
  std::filesystem::remove(dir / "b.clusters");
  const Run all_eight = content({"build-all", manifest, "--jobs", "8"});
  REQUIRE_MESSAGE(all_eight.exit_code == 0, all_eight.output);
  CHECK(file_bytes(slashes(dir / "a.clusters")) == a);
  CHECK(file_bytes(slashes(dir / "b.clusters")) == a);

  std::filesystem::remove_all(dir);
}

TEST_CASE("engine-content: build-all builds a manifest once and skips what is up to date") {
  const auto dir = std::filesystem::temp_directory_path() / "engine_content_manifest_tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string first = slashes(dir / "first.glb");
  const std::string second = slashes(dir / "second.glb");
  REQUIRE(write_cube_glb(first));
  REQUIRE(write_cube_glb(second, CubeOptions{.primitives = 3, .scale = 2.0f}));
  const std::string manifest = slashes(dir / "meshes.json");
  REQUIRE(write_text(manifest,
                     "{\"meshes\":[{\"source\":\"first.glb\",\"output\":\"first.clusters\"},"
                     "{\"source\":\"second.glb\",\"output\":\"second.clusters\","
                     "\"options\":{\"max_triangles\":8,\"max_vertices\":32,\"weld\":true}}]}"));

  // Everything is built the first time, one JSON line per mesh in manifest order and a summary.
  const Run built = content({"build-all", manifest});
  REQUIRE_MESSAGE(built.exit_code == 0, built.output);
  REQUIRE_MESSAGE(built.lines.size() == 3, built.output);
  CHECK(text_of(built.lines[0], "source") == first);
  CHECK(text_of(built.lines[0], "status") == "built");
  CHECK(text_of(built.lines[0], "path") == slashes(dir / "first.clusters"));
  CHECK(number(built.lines[0], "triangles") == 12);
  CHECK(text_of(built.lines[1], "source") == second);
  CHECK(text_of(built.lines[1], "status") == "built");
  CHECK(number(built.lines[1], "leaf_clusters") >= 3);  // three primitives, eight triangles max
  CHECK(number(built.lines[2], "built") == 2);
  CHECK(number(built.lines[2], "skipped") == 0);
  CHECK(number(built.lines[2], "failed") == 0);
  CHECK(built.lines[2].find("seconds") != nullptr);
  REQUIRE(std::filesystem::exists(dir / "first.clusters"));
  REQUIRE(std::filesystem::exists(dir / "second.clusters"));
  const std::string first_bytes = file_bytes(slashes(dir / "first.clusters"));
  const std::string second_bytes = file_bytes(slashes(dir / "second.clusters"));

  // Nothing changed, so nothing is built and nothing is rewritten.
  const Run skipped = content({"build-all", manifest});
  REQUIRE_MESSAGE(skipped.exit_code == 0, skipped.output);
  REQUIRE_MESSAGE(skipped.lines.size() == 3, skipped.output);
  CHECK(text_of(skipped.lines[0], "status") == "skipped");
  CHECK(text_of(skipped.lines[1], "status") == "skipped");
  CHECK(number(skipped.lines[2], "built") == 0);
  CHECK(number(skipped.lines[2], "skipped") == 2);
  CHECK(number(skipped.lines[2], "failed") == 0);
  CHECK(file_bytes(slashes(dir / "first.clusters")) == first_bytes);
  CHECK(file_bytes(slashes(dir / "second.clusters")) == second_bytes);

  // One source's bytes change: that one is rebuilt and the other is still skipped.
  REQUIRE(write_cube_glb(first, CubeOptions{.scale = 3.0f}));
  const Run again = content({"build-all", manifest});
  REQUIRE_MESSAGE(again.exit_code == 0, again.output);
  REQUIRE_MESSAGE(again.lines.size() == 3, again.output);
  CHECK(text_of(again.lines[0], "status") == "built");
  CHECK(text_of(again.lines[1], "status") == "skipped");
  CHECK(number(again.lines[2], "built") == 1);
  CHECK(number(again.lines[2], "skipped") == 1);
  CHECK(file_bytes(slashes(dir / "first.clusters")) != first_bytes);
  CHECK(file_bytes(slashes(dir / "second.clusters")) == second_bytes);

  // A manifest entry that cannot be built fails on its own without stopping the others.
  const std::string broken = slashes(dir / "broken.json");
  REQUIRE(write_text(broken,
                     "{\"meshes\":[{\"source\":\"missing.glb\",\"output\":\"missing.clusters\"},"
                     "{\"source\":\"second.glb\",\"output\":\"second.clusters\"}]}"));
  const Run partial = content({"build-all", broken});
  CHECK(partial.exit_code == 1);
  REQUIRE_MESSAGE(partial.lines.size() == 3, partial.output);
  CHECK(text_of(partial.lines[0], "status") == "failed");
  CHECK_FALSE(text_of(partial.lines[0], "error").empty());
  CHECK(number(partial.lines[2], "failed") == 1);
  CHECK(number(partial.lines[2], "built") == 1);  // the options differ, so it is rebuilt

  // --cache puts the entries with no output where the cache addresses them, and finds them.
  const std::string ddc = slashes(dir / "ddc");
  const std::string cached_manifest = slashes(dir / "cached.json");
  REQUIRE(write_text(cached_manifest,
                     "{\"meshes\":[{\"source\":\"first.glb\"},{\"source\":\"second.glb\"}]}"));
  CHECK(content({"build-all", cached_manifest, "--ddc", ddc}, true).exit_code ==
        1);  // no --cache, no output
  const Run to_cache = content({"build-all", cached_manifest, "--cache", "--ddc", ddc});
  REQUIRE_MESSAGE(to_cache.exit_code == 0, to_cache.output);
  CHECK(number(to_cache.lines[2], "built") == 2);
  u64 source_hash = 0;
  std::string error;
  REQUIRE_MESSAGE(assets::source_mesh_hash(first, source_hash, &error), error);
  const std::string entry = geometry::cluster_cache_path(
      ddc, geometry::cluster_cache_key(source_hash, geometry::ClusterLodOptions{}, true));
  CHECK(text_of(to_cache.lines[0], "path") == entry);
  CHECK(std::filesystem::exists(entry));
  const Run cache_hit = content({"build-all", cached_manifest, "--cache", "--ddc", ddc});
  REQUIRE_MESSAGE(cache_hit.exit_code == 0, cache_hit.output);
  CHECK(number(cache_hit.lines[2], "skipped") == 2);

  std::filesystem::remove_all(dir);
}

TEST_CASE("engine-content: the validation rules refuse a broken mesh and warn about a soft one") {
  const auto dir = std::filesystem::temp_directory_path() / "engine_content_validation_tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string out = slashes(dir / "out.clusters");

  // A position that is not finite stops the build, and the diagnostic names the primitive.
  const std::string nan_mesh = slashes(dir / "nan.glb");
  REQUIRE(write_cube_glb(nan_mesh, CubeOptions{.nan_position = true}));
  const Run refused = content({"build", nan_mesh, out}, true);
  CHECK(refused.exit_code == 1);
  CHECK_MESSAGE(refused.output.find("primitive 0") != std::string::npos, refused.output);
  CHECK_MESSAGE(refused.output.find("geometry.nan_position") != std::string::npos, refused.output);
  CHECK_MESSAGE(refused.output.find("not finite") != std::string::npos, refused.output);
  CHECK_FALSE(std::filesystem::exists(out));

  // A material naming an image that is not there is a warning: the build succeeds, says how
  // many warnings it had, and --strict turns the same warning into a refusal.
  const std::string missing_image = slashes(dir / "missing_image.glb");
  REQUIRE(write_cube_glb(missing_image, CubeOptions{.external_image = true}));
  const Run warned = content({"build", missing_image, out});
  REQUIRE_MESSAGE(warned.exit_code == 0, warned.output);
  CHECK(number(warned.result, "warnings") == 1);
  CHECK(std::filesystem::exists(out));
  std::filesystem::remove(out);
  const Run strict = content({"build", missing_image, out, "--strict"}, true);
  CHECK(strict.exit_code == 1);
  CHECK_MESSAGE(strict.output.find("material.missing_image") != std::string::npos, strict.output);
  CHECK_MESSAGE(strict.output.find("--strict") != std::string::npos, strict.output);
  CHECK_FALSE(std::filesystem::exists(out));

  // The same rule stops a manifest entry, and the failure is that entry's, not the run's.
  const std::string manifest = slashes(dir / "meshes.json");
  REQUIRE(
      write_text(manifest, "{\"meshes\":[{\"source\":\"nan.glb\",\"output\":\"nan.clusters\"}]}"));
  const Run in_manifest = content({"build-all", manifest});
  CHECK(in_manifest.exit_code == 1);
  REQUIRE_MESSAGE(in_manifest.lines.size() == 2, in_manifest.output);
  CHECK(text_of(in_manifest.lines[0], "status") == "failed");
  CHECK(text_of(in_manifest.lines[0], "rule") == "geometry.nan_position");
  CHECK(number(in_manifest.lines[1], "failed") == 1);

  // A manifest that is not one is a usage error before any mesh is touched.
  const std::string not_json = slashes(dir / "not.json");
  REQUIRE(write_text(not_json, "{\"nope\":1}"));
  CHECK(content({"build-all", not_json}, true).exit_code == 1);
  CHECK(content({"build-all", slashes(dir / "absent.json")}, true).exit_code == 1);

  std::filesystem::remove_all(dir);
}

TEST_CASE("engine-content: stats reports the metrics of a container") {
  const auto dir = std::filesystem::temp_directory_path() / "engine_content_stats_tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string mesh = slashes(dir / "cube.glb");
  const std::string out = slashes(dir / "cube.clusters");
  REQUIRE(write_cube_glb(mesh));
  const Run built = content({"build", mesh, out, "--max-triangles", "4"});
  REQUIRE_MESSAGE(built.exit_code == 0, built.output);

  const Run stats = content({"stats", out});
  REQUIRE_MESSAGE(stats.exit_code == 0, stats.output);
  REQUIRE_MESSAGE(stats.result.is_object(), stats.output);
  for (const char* key :
       {"path", "clusters", "lod_levels", "level_clusters", "groups", "leaf_triangles",
        "triangles_per_cluster", "source_vertices", "referenced_source_vertices",
        "cluster_vertices", "vertex_duplication", "bytes", "quantization", "materials", "images",
        "source_path", "source_hash", "build_key"}) {
    CHECK_MESSAGE(stats.result.find(key) != nullptr, "stats has no \"" << key << "\"");
  }
  const u64 clusters = number(stats.result, "clusters");
  CHECK(clusters == number(built.result, "clusters"));

  // The per-level counts and the histogram both account for every cluster exactly once.
  const JsonValue* levels = stats.result.find("level_clusters");
  REQUIRE(levels != nullptr);
  REQUIRE(levels->is_array());
  CHECK(levels->size() == number(stats.result, "lod_levels"));
  u64 level_total = 0;
  for (usize i = 0; i < levels->size(); ++i) {
    u64 count = 0;
    REQUIRE((*levels)[i].get_u64(count));
    level_total += count;
  }
  CHECK(level_total == clusters);

  const JsonValue* per_cluster = stats.result.find("triangles_per_cluster");
  REQUIRE(per_cluster != nullptr);
  REQUIRE(per_cluster->is_object());
  const u64 min = number(*per_cluster, "min");
  const u64 median = number(*per_cluster, "median");
  const u64 max = number(*per_cluster, "max");
  CHECK(min >= 1);
  CHECK(min <= median);
  CHECK(median <= max);
  CHECK(max <= 4);  // --max-triangles 4
  const JsonValue* histogram = per_cluster->find("histogram");
  REQUIRE(histogram != nullptr);
  REQUIRE(histogram->is_array());
  u64 histogram_total = 0;
  u64 triangle_total = 0;
  u64 previous = 0;
  for (usize i = 0; i < histogram->size(); ++i) {
    const JsonValue& bucket = (*histogram)[i];
    const u64 triangles = number(bucket, "triangles");
    const u64 count = number(bucket, "clusters");
    CHECK(triangles >= previous);  // one entry per distinct count, ascending
    previous = triangles;
    CHECK(count >= 1);
    histogram_total += count;
    triangle_total += triangles * count;
  }
  CHECK(histogram_total == clusters);
  CHECK(triangle_total == number(*per_cluster, "total"));

  // The duplication ratio and the grid are the numbers the container carries.
  geometry::ClusterFileData data;
  std::string error;
  REQUIRE_MESSAGE(geometry::read_cluster_file(out, data, &error), error);
  CHECK(number(stats.result, "source_vertices") == data.mesh.mesh.source_vertex_count);
  CHECK(number(stats.result, "cluster_vertices") == data.mesh.mesh.vertices.size());
  // The duplication ratio counts the source vertices the clusters actually name, not the
  // container's per-primitive sum, so a merged mesh's ratio still reads as a ratio.
  std::vector<u32> referenced(data.mesh.mesh.vertex_source.begin(),
                              data.mesh.mesh.vertex_source.end());
  std::sort(referenced.begin(), referenced.end());
  referenced.erase(std::unique(referenced.begin(), referenced.end()), referenced.end());
  CHECK(number(stats.result, "referenced_source_vertices") == referenced.size());
  CHECK(number(stats.result, "referenced_source_vertices") <=
        number(stats.result, "source_vertices"));
  const JsonValue* ratio = stats.result.find("vertex_duplication");
  REQUIRE(ratio != nullptr);
  f64 duplication = 0.0;
  REQUIRE(ratio->get_f64(duplication));
  CHECK(duplication == doctest::Approx(static_cast<f64>(data.mesh.mesh.vertices.size()) /
                                       static_cast<f64>(referenced.size())));
  CHECK(duplication >= 1.0);  // every cluster vertex is a copy of one source vertex
  const JsonValue* quantization = stats.result.find("quantization");
  REQUIRE(quantization != nullptr);
  const JsonValue* grid_step = quantization->find("step");
  REQUIRE(grid_step != nullptr);
  f64 step = 0.0;
  REQUIRE(grid_step->get_f64(step));
  CHECK(step == doctest::Approx(static_cast<f64>(data.mesh.mesh.quant_scale)));

  // The bytes add up to the file, section by section, with the header and the alignment named.
  const JsonValue* bytes = stats.result.find("bytes");
  REQUIRE(bytes != nullptr);
  CHECK(number(*bytes, "total") == std::filesystem::file_size(out));
  CHECK(number(*bytes, "total") == number(*bytes, "header") + number(*bytes, "section_table") +
                                       number(*bytes, "payloads") + number(*bytes, "padding"));
  const JsonValue* sections = bytes->find("sections");
  REQUIRE(sections != nullptr);
  REQUIRE(sections->is_array());
  CHECK(sections->size() == 15);
  u64 section_total = 0;
  for (usize i = 0; i < sections->size(); ++i)
    section_total += number((*sections)[i], "bytes");
  CHECK(section_total == number(*bytes, "payloads"));

  CHECK(content({"stats", mesh}, true).exit_code == 1);  // a GLB is not a container
  CHECK(content({"stats"}, true).exit_code == 2);
  CHECK(content({"stats", out, "--extra"}, true).exit_code == 2);

  std::filesystem::remove_all(dir);
}

TEST_CASE("engine-content: failures and usage errors have their own exit codes") {
  const auto dir = std::filesystem::temp_directory_path() / "engine_content_error_tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string out = slashes(dir / "out.clusters");

  const Run missing = content({"build", slashes(dir / "missing.glb"), out}, true);
  CHECK(missing.exit_code == 1);
  CHECK(missing.output.find("engine-content:") != std::string::npos);
  CHECK_FALSE(std::filesystem::exists(out));

  const std::string mesh = slashes(dir / "cube.glb");
  REQUIRE(write_cube_glb(mesh));
  const Run unwritable = content({"build", mesh, slashes(dir / "no/such/dir/out.clusters")}, true);
  CHECK(unwritable.exit_code == 1);
  CHECK(unwritable.output.find("cannot write") != std::string::npos);

  const Run not_a_cluster_file = content({"info", mesh}, true);
  CHECK(not_a_cluster_file.exit_code == 1);
  CHECK(not_a_cluster_file.output.find("magic") != std::string::npos);
  const Run info_missing = content({"info", slashes(dir / "missing.clusters")}, true);
  CHECK(info_missing.exit_code == 1);

  CHECK(content({}, true).exit_code == 2);
  CHECK(content({"frobnicate"}, true).exit_code == 2);
  CHECK(content({"build"}, true).exit_code == 2);
  CHECK(content({"build", mesh}, true).exit_code == 2);
  CHECK(content({"build", mesh, out, "--nope"}, true).exit_code == 2);
  CHECK(content({"build", mesh, out, "--max-triangles"}, true).exit_code == 2);
  CHECK(content({"build", mesh, out, "--max-triangles", "many"}, true).exit_code == 2);
  CHECK(content({"build", mesh, out, "--max-triangles", "3"}, true).exit_code == 2);
  CHECK(content({"build", mesh, out, "--max-vertices", "999"}, true).exit_code == 2);
  CHECK(content({"build", mesh, out, "--jobs"}, true).exit_code == 2);
  CHECK(content({"build", mesh, out, "--jobs", "lots"}, true).exit_code == 2);
  CHECK(content({"build", mesh, out, "--jobs", "99999"}, true).exit_code == 2);
  CHECK(content({"build-all"}, true).exit_code == 2);
  CHECK(content({"build-all", "a.json", "b.json"}, true).exit_code == 2);
  CHECK(content({"build-all", "a.json", "--nope"}, true).exit_code == 2);
  CHECK(content({"info"}, true).exit_code == 2);
  CHECK(content({"info", out, "--extra"}, true).exit_code == 2);
  CHECK(content({"--help"}).exit_code == 0);

  std::filesystem::remove_all(dir);
}
