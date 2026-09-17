// End to end: engine-content turns a glTF file into a .clusters container and reads it back.
// The fixture is a unit cube written as a GLB at test time (two primitives with two materials,
// one of them textured with a 4x4 checker PNG in the BIN chunk), so nothing binary lives in the
// tree and the test does not depend on the sample models tools/fetch-samples.ps1 downloads.
#include <core/json/json.h>
#include <core/math/math.h>
#include <core/platform/process.h>
#include <domain/assets/gltf.h>
#include <domain/geometry/cluster_file.h>
#include <foundation/image/png.h>

#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;

namespace {

struct Run {
  i32 exit_code = -1;
  std::string output;
  JsonValue result;  // parsed stdout when it was JSON
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
  return run;
}

u64 number(const JsonValue& object, const char* key) {
  const JsonValue* value = object.find(key);
  u64 out = 0;
  return value != nullptr && value->get_u64(out) ? out : ~u64{0};
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

// A unit cube as a GLB: 24 vertices with normals and UVs, two primitives of six triangles (a
// textured white material and a plain red one), and a 4x4 checker PNG in the BIN chunk. The
// same fixture apps/engine_view/tests/mesh_view_tests.cpp renders.
bool write_cube_glb(const std::string& path) {
  const Vec3 normals[6] = {Vec3{1, 0, 0},  Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                           Vec3{0, -1, 0}, Vec3{0, 0, 1},  Vec3{0, 0, -1}};
  const Vec3 tangents[6] = {Vec3{0, 1, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1},
                            Vec3{0, 0, 1}, Vec3{1, 0, 0}, Vec3{1, 0, 0}};
  std::vector<u8> bin;
  for (u32 f = 0; f < 6; ++f) {  // positions: corners wound counter-clockwise from outside
    const Vec3 nrm = normals[f];
    const Vec3 t = tangents[f];
    const Vec3 b = cross(nrm, t);
    const Vec3 corners[4] = {nrm * 0.5f - t * 0.5f - b * 0.5f, nrm * 0.5f + t * 0.5f - b * 0.5f,
                             nrm * 0.5f + t * 0.5f + b * 0.5f, nrm * 0.5f - t * 0.5f + b * 0.5f};
    for (const Vec3& c : corners) {
      put_f32(bin, c.x);
      put_f32(bin, c.y);
      put_f32(bin, c.z);
    }
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

  std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":["
      "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,"
      "\"material\":0},"
      "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":4,"
      "\"material\":1}]}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1],"
      "\"baseColorTexture\":{\"index\":0},\"metallicFactor\":0,\"roughnessFactor\":0.8}},"
      "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.9,0.15,0.1,1],\"metallicFactor\":0,"
      "\"roughnessFactor\":0.5}}],"
      "\"textures\":[{\"source\":0}],"
      "\"images\":[{\"bufferView\":4,\"mimeType\":\"image/png\"}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\","
      "\"min\":[-0.5,-0.5,-0.5],\"max\":[0.5,0.5,0.5]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":24,\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"componentType\":5123,\"count\":18,\"type\":\"SCALAR\"},"
      "{\"bufferView\":3,\"byteOffset\":36,\"componentType\":5123,\"count\":18,"
      "\"type\":\"SCALAR\"}],"
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
  CHECK(number(built.result, "bytes") > 0);
  CHECK(number(built.result, "bytes") == std::filesystem::file_size(out));
  CHECK(built.result.find("build_ms") != nullptr);
  CHECK(number(built.result, "hash") != 0);

  geometry::ClusterFileData data;
  std::string error;
  REQUIRE_MESSAGE(geometry::read_cluster_file(out, data, &error), error);
  CHECK(data.mesh.mesh.clusters.size() == number(built.result, "clusters"));
  CHECK(data.mesh.leaf_triangle_count == 12);
  CHECK(data.materials.size() == 2);
  CHECK(data.cluster_material.size() == data.mesh.mesh.clusters.size());
  CHECK(data.image_paths.size() == 1);
  CHECK(data.image_paths[0].empty());  // an embedded image keeps its slot with an empty path
  CHECK(data.source_path == mesh);     // the glTF those paths would have been relative to
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
  REQUIRE(described.result.find("source_path") != nullptr);
  CHECK(described.result.find("source_path")->as_string() == mesh);
  const JsonValue* sections = described.result.find("sections");
  REQUIRE(sections != nullptr);
  REQUIRE(sections->is_array());
  CHECK(sections->size() == 14);
  for (const char* name : {"clusters", "lod", "vertices", "attributes", "triangles",
                           "vertex_source", "level_cluster_counts", "cluster_material", "materials",
                           "image_paths", "strings", "scalars", "quantized", "source_path"}) {
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
  CHECK(content({"info"}, true).exit_code == 2);
  CHECK(content({"info", out, "--extra"}, true).exit_code == 2);
  CHECK(content({"--help"}).exit_code == 0);

  std::filesystem::remove_all(dir);
}
