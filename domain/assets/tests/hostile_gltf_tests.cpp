// Hostile glTF input (T187, from T13's reading, docs/experiments/fuzzing-design.md): each case is
// a file a client can make the engine read through `render.load`, built here as text by a function
// that returns the bytes, so that the fuzz harness can lift each one as a seed. Every case failed
// before its mend (a read past a buffer or a stack overflow); now each is a refusal with a message.

#include <domain/assets/gltf.h>

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <string>

using namespace engine;
using namespace engine::assets;

namespace hostile_gltf {

std::string base64(const std::uint8_t* bytes, std::size_t size) {
  static const char k_alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t i = 0; i < size; i += 3) {
    const std::uint32_t a = bytes[i];
    const std::uint32_t b = i + 1 < size ? bytes[i + 1] : 0u;
    const std::uint32_t c = i + 2 < size ? bytes[i + 2] : 0u;
    const std::uint32_t triple = (a << 16) | (b << 8) | c;
    out += k_alphabet[(triple >> 18) & 63];
    out += k_alphabet[(triple >> 12) & 63];
    out += i + 1 < size ? k_alphabet[(triple >> 6) & 63] : '=';
    out += i + 2 < size ? k_alphabet[triple & 63] : '=';
  }
  return out;
}

// One triangle: three float positions (36 bytes) then three u32 indices (12 bytes), as a `data:`
// URI so that a seed is one text file.
std::string triangle_buffer_uri() {
  const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
  const std::uint32_t indices[3] = {0u, 1u, 2u};
  std::uint8_t bytes[48];
  std::memcpy(bytes, positions, sizeof(positions));
  std::memcpy(bytes + 36, indices, sizeof(indices));
  return "data:application/octet-stream;base64," + base64(bytes, sizeof(bytes));
}

// The triangle with its index accessor's `count` replaced: the file's text otherwise valid.
std::string triangle_with_index_count(const char* count) {
  return std::string(R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":48,"uri":")") +
         triangle_buffer_uri() +
         R"("}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)"
         R"({"buffer":0,"byteOffset":36,"byteLength":12}],)"
         R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
         R"("min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5125,"count":)" +
         count +
         R"(,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},)"
         R"("indices":1}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
}

// An index accessor of 2^62 + 1 u32s over a 12-byte view: cgltf_validate's extent
// `offset + stride * (count - 1) + element_size` is 4 * 2^62 + 4, which wraps to 4 and passes,
// and its index-bound loop then walks the whole count past the buffer.
std::string index_count_wraps() { return triangle_with_index_count("4611686018427387905"); }

// A skin whose `inverseBindMatrices` accessor (no buffer view, so zeros by the format) counts
// 2^60 matrices: `count * 16` floats wrapped to 0, the unpacked array came back empty, and the
// skin then read one matrix per joint from it.
std::string inverse_bind_count_wraps() {
  return std::string(R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":48,"uri":")") +
         triangle_buffer_uri() +
         R"("}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)"
         R"({"buffer":0,"byteOffset":36,"byteLength":12}],)"
         R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
         R"("min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5125,"count":3,)"
         R"("type":"SCALAR"},{"componentType":5126,"count":1152921504606846976,"type":"MAT4"}],)"
         R"("skins":[{"joints":[1],"inverseBindMatrices":2}],)"
         R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],)"
         R"("nodes":[{"mesh":0,"skin":0},{"translation":[0,1,0]}],"scenes":[{"nodes":[0,1]}],)"
         R"("scene":0})";
}

// The triangle under a chain of `nodes` nodes, each the only child of the one before, the mesh on
// the last: its depth below the scene's root is `nodes - 1`.
std::string node_chain(std::uint32_t nodes) {
  std::string text =
      std::string(R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":48,"uri":")") +
      triangle_buffer_uri() +
      R"("}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)"
      R"({"buffer":0,"byteOffset":36,"byteLength":12}],)"
      R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
      R"("min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5125,"count":3,)"
      R"("type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},)"
      R"("indices":1}]}],"nodes":[)";
  for (std::uint32_t i = 0; i + 1 < nodes; ++i)
    text += R"({"children":[)" + std::to_string(i + 1) + "]},";
  text += R"({"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
  return text;
}

// Twenty thousand levels: the recursive walk overflowed its thread's stack (a debug frame is
// several hundred bytes and a thread's stack 1 MiB) before the depth was bounded.
std::string node_chain_too_deep() { return node_chain(20000); }

}  // namespace hostile_gltf

namespace {

bool load_text(const std::string& text, MeshData& mesh, std::string& error) {
  const std::span<const u8> bytes(reinterpret_cast<const u8*>(text.data()), text.size());
  return load_gltf_memory(bytes, "", mesh, &error);
}

}  // namespace

TEST_CASE("gltf hostile: an accessor whose extent wraps is refused before cgltf reads it") {
  MeshData mesh;
  std::string error;
  // The same file with an honest count loads, so the refusal below is the count's alone.
  REQUIRE(load_text(hostile_gltf::triangle_with_index_count("3"), mesh, error));
  CHECK(mesh.indices.size() == 3u);

  CHECK_FALSE(load_text(hostile_gltf::index_count_wraps(), mesh, error));
  CHECK(error.find("accessor 1") != std::string::npos);
  CHECK(mesh.positions.empty());
  CHECK(mesh.indices.empty());
}

TEST_CASE("gltf hostile: an inverseBindMatrices count whose float count wraps is refused") {
  MeshData mesh;
  std::string error;
  CHECK_FALSE(load_text(hostile_gltf::inverse_bind_count_wraps(), mesh, error));
  CHECK(error.find("inverseBindMatrices") != std::string::npos);
  CHECK(mesh.positions.empty());
}

TEST_CASE("gltf hostile: a node hierarchy deeper than the importer walks is refused by name") {
  MeshData mesh;
  std::string error;
  // At the bound the triangle still imports: 256 levels below the root, 257 nodes.
  REQUIRE(load_text(hostile_gltf::node_chain(257), mesh, error));
  CHECK(mesh.indices.size() == 3u);

  CHECK_FALSE(load_text(hostile_gltf::node_chain(258), mesh, error));
  CHECK(error.find("256 levels") != std::string::npos);
  CHECK(mesh.positions.empty());

  CHECK_FALSE(load_text(hostile_gltf::node_chain_too_deep(), mesh, error));
  CHECK(error.find("256 levels") != std::string::npos);
  CHECK(mesh.positions.empty());
}
