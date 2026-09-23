// The canonical vertex ids as the renderer's loaded scene exposes them (`mesh_vertex_ids`,
// docs/subsystems/renderer.md): read-only, per mesh, the same whether the mesh was built from its
// glTF or read back out of the derived-data cache, and empty for a mesh with none. Nothing here
// needs a GPU — the ids are host-side only and nothing uploads them.

#include <domain/geometry/cluster.h>
#include <systems/renderer/scene.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::renderer;

namespace {

std::string slashes(const std::filesystem::path& p) {
  std::string s = p.string();
  for (char& c : s) {
    if (c == '\\') c = '/';
  }
  return s;
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

template <class T>
void append(std::vector<std::uint8_t>& out, const T* values, std::size_t count) {
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(values);
  out.insert(out.end(), bytes, bytes + count * sizeof(T));
}

// Two unit quads side by side, each with its own four vertices, facing +z. The two vertices of the
// shared edge are coincident and agree on normal and UV, so a weld would merge them — unless the
// file names them apart, which `with_ids` does: quad A's corners are 10..13 and quad B's 20..23, so
// the authored mesh keeps all eight vertices and a mesh without the attribute welds to six.
struct Quads {
  Vec3 positions[8] = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0}, Vec3{0, 1, 0},
                       Vec3{1, 0, 0}, Vec3{2, 0, 0}, Vec3{2, 1, 0}, Vec3{1, 1, 0}};
  std::uint32_t ids[8] = {10, 11, 12, 13, 20, 21, 22, 23};
};

bool write_quads(const std::string& path, bool with_ids) {
  const Quads q;
  Vec3 normals[8];
  Vec2 uvs[8];
  for (u32 v = 0; v < 8; ++v) {
    normals[v] = Vec3{0.0f, 0.0f, 1.0f};
    uvs[v] = Vec2{q.positions[v].x * 0.5f, q.positions[v].y};
  }
  const std::uint16_t indices[12] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
  std::vector<std::uint8_t> buffer;
  append(buffer, q.positions, 8);  // 96
  append(buffer, normals, 8);      // 96
  append(buffer, uvs, 8);          // 64
  append(buffer, q.ids, 8);        // 32
  append(buffer, indices, 12);     // 24
  const std::string ids_attribute =
      with_ids ? std::string(",\"") + assets::k_canonical_id_attribute + "\":3" : std::string();
  const std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,"
      "\"NORMAL\":1,\"TEXCOORD_0\":2" +
      ids_attribute +
      "},\"indices\":4}]}],\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":8,\"type\":\"VEC3\","
      "\"min\":[0,0,0],\"max\":[2,1,0]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":8,\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":8,\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"componentType\":5125,\"count\":8,\"type\":\"SCALAR\"},"
      "{\"bufferView\":4,\"componentType\":5123,\"count\":12,\"type\":\"SCALAR\"}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":96},"
      "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":96},"
      "{\"buffer\":0,\"byteOffset\":192,\"byteLength\":64},"
      "{\"buffer\":0,\"byteOffset\":256,\"byteLength\":32},"
      "{\"buffer\":0,\"byteOffset\":288,\"byteLength\":24}],"
      "\"buffers\":[{\"byteLength\":312,\"uri\":\"data:application/octet-stream;base64," +
      base64(buffer) + "\"}]}";
  std::ofstream f(path, std::ios::binary);
  if (!f.is_open()) return false;
  f << json;
  return f.good();
}

// Every id the span holds is one the file authored, at the position the file put it.
u32 misplaced(const SceneData& data, u32 mesh) {
  const Quads q;
  const std::span<const u32> ids = mesh_vertex_ids(data, mesh);
  const u32 first = data.parts[mesh].first_vertex;
  u32 wrong = 0;
  for (u32 i = 0; i < ids.size(); ++i) {
    const Vec3 p = data.lod.mesh.vertices[first + i];
    bool found = false;
    for (u32 v = 0; v < 8; ++v) {
      if (q.ids[v] == ids[i]) found = q.positions[v].x == p.x && q.positions[v].y == p.y;
    }
    wrong += found ? 0u : 1u;
  }
  return wrong;
}

}  // namespace

TEST_CASE("renderer: a loaded mesh exposes its canonical vertex ids, from the glTF and the cache") {
  const test::TempDir tmp("engine_renderer_vertex_ids");
  const std::filesystem::path dir = tmp.native();
  const std::string named = slashes(dir / "named.gltf");
  const std::string unnamed = slashes(dir / "unnamed.gltf");
  REQUIRE(write_quads(named, true));
  REQUIRE(write_quads(unnamed, false));

  SceneDesc desc;
  desc.meshes.push_back(named);
  desc.ddc = slashes(dir / "ddc");
  std::string error;

  // Built from the glTF (a cache miss), then read back out of the entry that load wrote (a hit):
  // the same ids at the same vertices either way.
  for (const char* expected_cache : {"miss", "hit"}) {
    SceneData data;
    REQUIRE_MESSAGE(load_scene(desc, data, error), error);
    CHECK(std::string(data.mesh_cache) == expected_cache);
    REQUIRE(data.parts.size() == 1);
    CHECK(data.parts[0].vertex_id_source == geometry::VertexIdSource::authored);
    const std::span<const u32> ids = mesh_vertex_ids(data, 0);
    CHECK(ids.size() == data.lod.mesh.vertices.size());
    CHECK(misplaced(data, 0) == 0);
    // The two edges the file named apart stayed apart: all eight authored ids are there.
    for (const u32 id : Quads{}.ids)
      CHECK(std::find(ids.begin(), ids.end(), id) != ids.end());
    CHECK(mesh_vertex_ids(data, 1).empty());  // past the scene's meshes
  }

  // Two meshes in one scene: each keeps its own id space and says which it is. The second has no
  // attribute, so it is named by the position weld, whose ids are ranks of its six positions.
  SceneDesc pair;
  pair.meshes.push_back(named);
  pair.meshes.push_back(unnamed);
  pair.cache = false;
  SceneData data;
  REQUIRE_MESSAGE(load_scene(pair, data, error), error);
  REQUIRE(data.parts.size() == 2);
  CHECK(data.parts[0].vertex_id_source == geometry::VertexIdSource::authored);
  CHECK(data.parts[1].vertex_id_source == geometry::VertexIdSource::position_weld);
  const std::span<const u32> first = mesh_vertex_ids(data, 0);
  const std::span<const u32> second = mesh_vertex_ids(data, 1);
  CHECK(first.size() == data.parts[1].first_vertex - data.parts[0].first_vertex);
  CHECK(first.size() + second.size() == data.lod.mesh.vertices.size());
  CHECK(misplaced(data, 0) == 0);
  for (const u32 id : second)
    CHECK(id < 6);

  // A mesh with no ids — the procedural terrain — has none to expose.
  SceneDesc terrain;
  terrain.meshes.push_back("");
  terrain.heightfield_grid = 17;
  terrain.cache = false;
  SceneData ground;
  REQUIRE_MESSAGE(load_scene(terrain, ground, error), error);
  CHECK(ground.parts[0].vertex_id_source == geometry::VertexIdSource::none);
  CHECK(mesh_vertex_ids(ground, 0).empty());
}
