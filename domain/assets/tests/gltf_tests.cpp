#include <domain/assets/gltf.h>
#include <domain/geometry/cluster.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::assets;

namespace {

// The fixtures are written here rather than committed: a binary in the tree is something no one
// can review, and the numbers below are the test's expectations as much as its input.

struct TempDir {
  std::string path;
  explicit TempDir(const char* name) {
    const auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    path = io::normalize_path(p.string());
  }
  ~TempDir() { std::filesystem::remove_all(std::filesystem::path(path)); }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

// A unit cube: six faces of four vertices, 36 indices, per-face normals, per-face UVs in [0, 1].
struct Cube {
  std::vector<Vec3> positions;
  std::vector<Vec3> normals;
  std::vector<Vec2> uvs;
  std::vector<std::uint16_t> indices;
};

Cube make_cube() {
  const Vec3 face_normals[6] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
  const Vec3 across[6] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
  const Vec3 up[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, 1, 0}};

  Cube cube;
  for (std::uint16_t face = 0; face < 6; ++face) {
    const std::uint16_t base = static_cast<std::uint16_t>(face * 4);
    for (int corner = 0; corner < 4; ++corner) {
      const f32 u = (corner == 1 || corner == 2) ? 1.0f : 0.0f;
      const f32 v = (corner >= 2) ? 1.0f : 0.0f;
      cube.positions.push_back(face_normals[face] * 0.5f + across[face] * (u - 0.5f) +
                               up[face] * (v - 0.5f));
      cube.normals.push_back(face_normals[face]);
      cube.uvs.push_back(Vec2{u, v});
    }
    const std::uint16_t order[6] = {0, 1, 2, 0, 2, 3};
    for (std::uint16_t k : order)
      cube.indices.push_back(static_cast<std::uint16_t>(base + k));
  }
  return cube;
}

// Byte layout of the cube's buffer; every offset is a multiple of four.
constexpr std::size_t k_position_offset = 0;
constexpr std::size_t k_normal_offset = k_position_offset + 24 * 3 * 4;  // 288
constexpr std::size_t k_uv_offset = k_normal_offset + 24 * 3 * 4;        // 576
constexpr std::size_t k_index_offset = k_uv_offset + 24 * 2 * 4;         // 768
constexpr std::size_t k_buffer_bytes = k_index_offset + 36 * 2;          // 840

template <class T>
void append_raw(std::vector<std::uint8_t>& out, const std::vector<T>& values) {
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(values.data());
  out.insert(out.end(), bytes, bytes + values.size() * sizeof(T));
}

std::vector<std::uint8_t> cube_buffer(const Cube& cube) {
  std::vector<std::uint8_t> out;
  out.reserve(k_buffer_bytes);
  append_raw(out, cube.positions);
  append_raw(out, cube.normals);
  append_raw(out, cube.uvs);
  append_raw(out, cube.indices);
  REQUIRE(out.size() == k_buffer_bytes);
  return out;
}

std::string base64(const std::vector<std::uint8_t>& bytes) {
  static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    const std::uint32_t b0 = bytes[i];
    const std::uint32_t b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0u;
    const std::uint32_t b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0u;
    const std::uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
    out += table[(triple >> 18) & 63u];
    out += table[(triple >> 12) & 63u];
    out += i + 1 < bytes.size() ? table[(triple >> 6) & 63u] : '=';
    out += i + 2 < bytes.size() ? table[triple & 63u] : '=';
  }
  return out;
}

// Eight bytes that stand in for a PNG file: this module hands image bytes on undecoded.
const std::vector<std::uint8_t> k_png_bytes = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};

std::string n(std::size_t value) { return std::to_string(value); }

// The cube as a glTF document. `buffer_uri` is empty for the GLB form, where the buffer is the
// BIN chunk; `mode` is the primitive topology (4 = triangles).
std::string cube_json(const std::string& buffer_uri, int mode = 4) {
  const std::string buffer =
      buffer_uri.empty()
          ? "{\"byteLength\": " + n(k_buffer_bytes) + "}"
          : "{\"uri\": \"" + buffer_uri + "\", \"byteLength\": " + n(k_buffer_bytes) + "}";
  const std::string mode_field = mode == 4 ? std::string() : ", \"mode\": " + std::to_string(mode);
  const std::string image_uri = "data:image/png;base64," + base64(k_png_bytes);

  std::string json;
  json += "{\n";
  json += "  \"asset\": {\"version\": \"2.0\", \"generator\": \"engine assets tests\"},\n";
  json += "  \"scene\": 0,\n";
  json += "  \"scenes\": [{\"nodes\": [0]}],\n";
  json += "  \"nodes\": [\n";
  json += "    {\"name\": \"root\", \"children\": [1]},\n";
  // A quarter turn about +Y, then two metres along +X: (x, y, z) -> (z + 2, y, -x).
  json += "    {\"name\": \"cube\", \"mesh\": 0, \"translation\": [2, 0, 0],\n";
  json += "     \"rotation\": [0, 0.70710678118654752, 0, 0.70710678118654752]}\n";
  json += "  ],\n";
  json += "  \"meshes\": [{\"name\": \"cube\", \"primitives\": [\n";
  json +=
      "    {\"attributes\": {\"POSITION\": 0, \"NORMAL\": 1, \"TEXCOORD_0\": 2},"
      " \"indices\": 3, \"material\": 0" +
      mode_field + "},\n";
  json +=
      "    {\"attributes\": {\"POSITION\": 0, \"NORMAL\": 1, \"TEXCOORD_0\": 2},"
      " \"indices\": 4, \"material\": 1}\n";
  json += "  ]}],\n";
  json += "  \"materials\": [\n";
  json +=
      "    {\"name\": \"painted\", \"pbrMetallicRoughness\": {"
      "\"baseColorFactor\": [0.25, 0.5, 0.75, 1.0], \"metallicFactor\": 0.25,"
      " \"roughnessFactor\": 0.75, \"baseColorTexture\": {\"index\": 0}}},\n";
  json +=
      "    {\"name\": \"bumped\", \"pbrMetallicRoughness\": {"
      "\"baseColorFactor\": [1.0, 0.5, 0.25, 1.0], \"metallicFactor\": 0.0,"
      " \"roughnessFactor\": 1.0}, \"normalTexture\": {\"index\": 1}}\n";
  json += "  ],\n";
  json += "  \"textures\": [{\"source\": 0}, {\"source\": 1}],\n";
  json += "  \"images\": [\n";
  json += "    {\"name\": \"albedo\", \"uri\": \"" + image_uri + "\"},\n";
  json += "    {\"name\": \"bumps\", \"uri\": \"textures/normal%20map.png\"}\n";
  json += "  ],\n";
  json += "  \"accessors\": [\n";
  json +=
      "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": 24, \"type\": \"VEC3\","
      " \"min\": [-0.5, -0.5, -0.5], \"max\": [0.5, 0.5, 0.5]},\n";
  json += "    {\"bufferView\": 1, \"componentType\": 5126, \"count\": 24, \"type\": \"VEC3\"},\n";
  json += "    {\"bufferView\": 2, \"componentType\": 5126, \"count\": 24, \"type\": \"VEC2\"},\n";
  json +=
      "    {\"bufferView\": 3, \"componentType\": 5123, \"count\": 18, \"type\": \"SCALAR\"},\n";
  json +=
      "    {\"bufferView\": 3, \"byteOffset\": 36, \"componentType\": 5123, \"count\": 18,"
      " \"type\": \"SCALAR\"}\n";
  json += "  ],\n";
  json += "  \"bufferViews\": [\n";
  json += "    {\"buffer\": 0, \"byteOffset\": " + n(k_position_offset) +
          ", \"byteLength\": 288, \"target\": 34962},\n";
  json += "    {\"buffer\": 0, \"byteOffset\": " + n(k_normal_offset) +
          ", \"byteLength\": 288, \"target\": 34962},\n";
  json += "    {\"buffer\": 0, \"byteOffset\": " + n(k_uv_offset) +
          ", \"byteLength\": 192, \"target\": 34962},\n";
  json += "    {\"buffer\": 0, \"byteOffset\": " + n(k_index_offset) +
          ", \"byteLength\": 72, \"target\": 34963}\n";
  json += "  ],\n";
  json += "  \"buffers\": [" + buffer + "]\n";
  json += "}\n";
  return json;
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xffu));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
  out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xffu));
  out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xffu));
}

// A binary glTF container: the 12-byte header, then the JSON chunk, then the BIN chunk, each
// padded to four bytes (JSON with spaces, BIN with zeros).
std::vector<std::uint8_t> make_glb(const std::string& json,
                                   const std::vector<std::uint8_t>& binary) {
  std::string json_chunk = json;
  while (json_chunk.size() % 4 != 0)
    json_chunk += ' ';
  std::vector<std::uint8_t> bin_chunk = binary;
  while (bin_chunk.size() % 4 != 0)
    bin_chunk.push_back(0);

  std::vector<std::uint8_t> out;
  const std::size_t total = 12 + 8 + json_chunk.size() + 8 + bin_chunk.size();
  append_u32(out, 0x46546c67u);  // "glTF"
  append_u32(out, 2u);
  append_u32(out, static_cast<std::uint32_t>(total));
  append_u32(out, static_cast<std::uint32_t>(json_chunk.size()));
  append_u32(out, 0x4e4f534au);  // "JSON"
  out.insert(out.end(), json_chunk.begin(), json_chunk.end());
  append_u32(out, static_cast<std::uint32_t>(bin_chunk.size()));
  append_u32(out, 0x004e4942u);  // "BIN\0"
  out.insert(out.end(), bin_chunk.begin(), bin_chunk.end());
  REQUIRE(out.size() == total);
  return out;
}

std::string_view view_of(const std::vector<std::uint8_t>& bytes) {
  return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::span<const u8> span_of(const std::vector<std::uint8_t>& bytes) {
  return std::span<const u8>(bytes.data(), bytes.size());
}

// The node transform the fixture bakes in: a quarter turn about +Y, then +2 along x.
Vec3 expected_position(Vec3 source) { return Vec3{source.z + 2.0f, source.y, -source.x}; }
Vec3 expected_normal(Vec3 source) { return Vec3{source.z, source.y, -source.x}; }

// Everything both the .gltf and the .glb fixture must produce.
void check_cube_mesh(const MeshData& mesh, const Cube& cube) {
  // Two primitives share one POSITION accessor, but the merged space gives each its own copy.
  REQUIRE(mesh.positions.size() == 48u);
  REQUIRE(mesh.normals.size() == 48u);
  REQUIRE(mesh.uvs.size() == 48u);
  REQUIRE(mesh.indices.size() == 36u);
  REQUIRE(mesh.primitives.size() == 2u);

  CHECK(mesh.primitives[0].first_index == 0u);
  CHECK(mesh.primitives[0].index_count == 18u);
  CHECK(mesh.primitives[0].material == 0);
  CHECK(mesh.primitives[1].first_index == 18u);
  CHECK(mesh.primitives[1].index_count == 18u);
  CHECK(mesh.primitives[1].material == 1);

  for (u32 i = 0; i < mesh.positions.size(); ++i) {
    const u32 source = i % 24u;
    CHECK(length(mesh.positions[i] - expected_position(cube.positions[source])) < 1e-5f);
    CHECK(std::fabs(length(mesh.normals[i]) - 1.0f) < 1e-5f);
    CHECK(length(mesh.normals[i] - expected_normal(cube.normals[source])) < 1e-5f);
    CHECK(mesh.uvs[i].x >= 0.0f);
    CHECK(mesh.uvs[i].x <= 1.0f);
    CHECK(mesh.uvs[i].y >= 0.0f);
    CHECK(mesh.uvs[i].y <= 1.0f);
    CHECK(mesh.uvs[i] == cube.uvs[source]);
  }

  // Each primitive's indices stay inside its own block of the merged vertex space, widened from
  // the file's unsigned shorts.
  for (u32 i = 0; i < 18u; ++i) {
    CHECK(mesh.indices[i] < 24u);
    CHECK(mesh.indices[i] == static_cast<u32>(cube.indices[i]));
    CHECK(mesh.indices[18u + i] >= 24u);
    CHECK(mesh.indices[18u + i] == 24u + static_cast<u32>(cube.indices[18u + i]));
  }
}

void check_cube_materials(const MeshData& mesh) {
  REQUIRE(mesh.materials.size() == 2u);
  CHECK(mesh.materials[0].name == "painted");
  CHECK(length(mesh.materials[0].base_color - Vec4(0.25f, 0.5f, 0.75f, 1.0f)) < 1e-6f);
  CHECK(std::fabs(mesh.materials[0].metallic - 0.25f) < 1e-6f);
  CHECK(std::fabs(mesh.materials[0].roughness - 0.75f) < 1e-6f);
  CHECK(mesh.materials[0].base_color_image == 0);
  CHECK(mesh.materials[0].normal_image == -1);

  CHECK(mesh.materials[1].name == "bumped");
  CHECK(length(mesh.materials[1].base_color - Vec4(1.0f, 0.5f, 0.25f, 1.0f)) < 1e-6f);
  CHECK(std::fabs(mesh.materials[1].metallic) < 1e-6f);
  CHECK(std::fabs(mesh.materials[1].roughness - 1.0f) < 1e-6f);
  CHECK(mesh.materials[1].base_color_image == -1);
  CHECK(mesh.materials[1].normal_image == 1);

  // The embedded image arrives undecoded, with its media type; the external one as a URI with
  // its percent escapes resolved.
  REQUIRE(mesh.images.size() == 2u);
  CHECK(mesh.images[0].name == "albedo");
  CHECK(mesh.images[0].mime_type == "image/png");
  CHECK(mesh.images[0].uri.empty());
  REQUIRE(mesh.images[0].bytes.size() == k_png_bytes.size());
  CHECK(std::memcmp(mesh.images[0].bytes.data(), k_png_bytes.data(), k_png_bytes.size()) == 0);
  CHECK(mesh.images[1].name == "bumps");
  CHECK(mesh.images[1].uri == "textures/normal map.png");
  CHECK(mesh.images[1].bytes.empty());
}

}  // namespace

TEST_CASE("gltf: a .gltf with an external buffer merges its primitives in world space") {
  const TempDir tmp("engine_assets_gltf");
  const Cube cube = make_cube();
  const std::vector<std::uint8_t> binary = cube_buffer(cube);
  REQUIRE(io::write_file(io::join_path(tmp.path, "cube.bin"), view_of(binary)) == io::Status::Ok);
  REQUIRE(io::write_file(io::join_path(tmp.path, "cube.gltf"), cube_json("cube.bin")) ==
          io::Status::Ok);

  MeshData mesh;
  std::string error;
  REQUIRE_MESSAGE(load_gltf(io::join_path(tmp.path, "cube.gltf"), mesh, &error), error);
  CHECK(error.empty());
  check_cube_mesh(mesh, cube);
  check_cube_materials(mesh);
}

TEST_CASE("gltf: a .glb built in memory loads like the .gltf") {
  const Cube cube = make_cube();
  const std::vector<std::uint8_t> glb = make_glb(cube_json(""), cube_buffer(cube));

  MeshData mesh;
  std::string error;
  REQUIRE_MESSAGE(load_gltf_memory(span_of(glb), "", mesh, &error), error);
  check_cube_mesh(mesh, cube);
  check_cube_materials(mesh);
}

TEST_CASE("gltf: an embedded base64 buffer needs no companion file") {
  const Cube cube = make_cube();
  const std::string uri = "data:application/octet-stream;base64," + base64(cube_buffer(cube));
  const std::string json = cube_json(uri);

  MeshData mesh;
  std::string error;
  const std::span<const u8> bytes(reinterpret_cast<const u8*>(json.data()), json.size());
  REQUIRE_MESSAGE(load_gltf_memory(bytes, "", mesh, &error), error);
  check_cube_mesh(mesh, cube);
}

TEST_CASE("gltf: the loaded mesh feeds the cluster builder") {
  const Cube cube = make_cube();
  const std::vector<std::uint8_t> glb = make_glb(cube_json(""), cube_buffer(cube));
  MeshData mesh;
  std::string error;
  REQUIRE_MESSAGE(load_gltf_memory(span_of(glb), "", mesh, &error), error);

  const geometry::AttributeSource attributes = attribute_source(mesh);
  CHECK(attributes.normals.size() == mesh.positions.size());
  CHECK(attributes.uvs.size() == mesh.positions.size());

  geometry::ClusterMesh clusters;
  const geometry::ClusterBuildOptions options;
  REQUIRE_MESSAGE(
      geometry::build_clusters(std::span<const Vec3>(mesh.positions.data(), mesh.positions.size()),
                               std::span<const u32>(mesh.indices.data(), mesh.indices.size()),
                               options, clusters, &error, attributes),
      error);
  CHECK(clusters.source_triangle_count == 12u);
  CHECK(clusters.clusters.size() >= 1u);
}

TEST_CASE("gltf: a mesh with no attributes at all still loads") {
  // Positions only, no indices, no materials, no scene transform: the smallest valid document.
  const std::vector<Vec3> triangle = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  std::vector<std::uint8_t> binary;
  append_raw(binary, triangle);
  REQUIRE(binary.size() == 36u);

  const std::string json =
      "{\"asset\": {\"version\": \"2.0\"}, \"scene\": 0, \"scenes\": [{\"nodes\": [0]}],"
      " \"nodes\": [{\"mesh\": 0}],"
      " \"meshes\": [{\"primitives\": [{\"attributes\": {\"POSITION\": 0}}]}],"
      " \"accessors\": [{\"bufferView\": 0, \"componentType\": 5126, \"count\": 3,"
      " \"type\": \"VEC3\"}],"
      " \"bufferViews\": [{\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 36}],"
      " \"buffers\": [{\"byteLength\": 36, \"uri\": \"data:application/octet-stream;base64," +
      base64(binary) + "\"}]}";

  MeshData mesh;
  std::string error;
  const std::span<const u8> bytes(reinterpret_cast<const u8*>(json.data()), json.size());
  REQUIRE_MESSAGE(load_gltf_memory(bytes, "", mesh, &error), error);
  CHECK(mesh.positions.size() == 3u);
  CHECK(mesh.indices.size() == 3u);  // generated for a primitive without an index accessor
  CHECK(mesh.indices[0] == 0u);
  CHECK(mesh.indices[2] == 2u);
  CHECK(mesh.normals.empty());
  CHECK(mesh.uvs.empty());
  CHECK(mesh.materials.empty());
  CHECK(mesh.primitives.size() == 1u);
  CHECK(mesh.primitives[0].material == -1);

  const geometry::AttributeSource attributes = attribute_source(mesh);
  CHECK(attributes.normals.empty());
  CHECK(attributes.uvs.empty());
}

TEST_CASE("gltf: bad input fails with an error instead of a partial mesh") {
  const TempDir tmp("engine_assets_gltf_bad");
  MeshData mesh;
  std::string error;

  SUBCASE("a file that is not there") {
    CHECK_FALSE(load_gltf(io::join_path(tmp.path, "missing.gltf"), mesh, &error));
    CHECK_FALSE(error.empty());
  }

  SUBCASE("corrupt JSON") {
    const std::string path = io::join_path(tmp.path, "broken.gltf");
    REQUIRE(io::write_file(path, "{\"asset\": {\"version\": \"2.0\"} \"scenes\": [") ==
            io::Status::Ok);
    CHECK_FALSE(load_gltf(path, mesh, &error));
    CHECK_FALSE(error.empty());
  }

  SUBCASE("a primitive that is not triangles") {
    const Cube cube = make_cube();
    const std::vector<std::uint8_t> glb = make_glb(cube_json("", /*mode=*/5), cube_buffer(cube));
    CHECK_FALSE(load_gltf_memory(span_of(glb), "", mesh, &error));
    CHECK_FALSE(error.empty());
    CHECK(error.find("triangle strip") != std::string::npos);
  }

  SUBCASE("an external buffer that is missing") {
    const std::string path = io::join_path(tmp.path, "orphan.gltf");
    REQUIRE(io::write_file(path, cube_json("nowhere.bin")) == io::Status::Ok);
    CHECK_FALSE(load_gltf(path, mesh, &error));
    CHECK_FALSE(error.empty());
  }

  SUBCASE("empty input") {
    CHECK_FALSE(load_gltf_memory(std::span<const u8>(), "", mesh, &error));
    CHECK_FALSE(error.empty());
  }

  CHECK(mesh.positions.empty());
  CHECK(mesh.indices.empty());
  CHECK(mesh.primitives.empty());
}
