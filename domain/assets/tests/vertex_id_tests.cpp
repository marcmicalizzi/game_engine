// Canonical vertex ids through the import and the weld (docs/subsystems/assets.md, "Canonical
// vertex ids"; docs/plan/07-content-pipeline.md §7.11, "identity has to survive the weld"): the
// `_CANONICAL_ID` attribute in every form the importer accepts and each form it refuses, the rule
// for a mesh without it, and the property the stream exists for — every vertex of the built mesh
// carries the id of every source vertex it stands for — on synthetic files and on the Khronos
// skinned and morph samples. The rule itself (what may merge) is domain/geometry's and tested
// there (tests/vertex_id_tests.cpp).

#include <domain/assets/gltf.h>
#include <domain/geometry/cluster.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/uv_repair.h>

#include <doctest/doctest.h>
#include <test_paths.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::assets;

namespace {

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
void append_raw(std::vector<std::uint8_t>& out, const T* values, std::size_t count) {
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(values);
  out.insert(out.end(), bytes, bytes + count * sizeof(T));
  while (out.size() % 4 != 0)
    out.push_back(0);
}

// Three unit quads in a row along x, each written with its own four vertices — the way an
// exporter that splits at every face writes them — facing +z, one primitive:
//
//   quad A  x 0..1    a0 (0,0)  a1 (1,0)  a2 (1,1)  a3 (0,1)
//   quad B  x 1..2    b0 (1,0)  b1 (2,0)  b2 (2,1)  b3 (1,1)
//   quad C  x 2..3    c0 (2,0)  c1 (3,0)  c2 (3,1)  c3 (2,1)
//
// Every coincident pair agrees on normal and UV except b3, whose UV is off a2's: a UV seam. The ids
// name the grid point, (x, y) -> 4y + x, so each pair shares one — except c0, which the author
// named 100: two points of the base at one position, like closed lips. What the weld must do:
//
//   a1 = b0    same id, same attributes      merge
//   a2 / b3    same id, a UV seam            stay two vertices, both carrying id 5
//   b1 / c0    different ids, same attributes stay two vertices (merged without the stream)
//   b2 = c3    same id, same attributes      merge
struct Strip {
  std::vector<Vec3> positions;
  std::vector<Vec3> normals;
  std::vector<Vec2> uvs;
  std::vector<std::uint32_t> ids;
  std::vector<std::uint16_t> indices;
};

Strip make_strip() {
  Strip s;
  for (u32 q = 0; q < 3; ++q) {
    const f32 x0 = static_cast<f32>(q);
    const Vec3 corners[4] = {Vec3{x0, 0, 0}, Vec3{x0 + 1, 0, 0}, Vec3{x0 + 1, 1, 0},
                             Vec3{x0, 1, 0}};
    for (u32 k = 0; k < 4; ++k) {
      const Vec3 p = corners[k];
      s.positions.push_back(p);
      s.normals.push_back(Vec3{0.0f, 0.0f, 1.0f});
      Vec2 uv{p.x / 3.0f, p.y};
      if (q == 1 && k == 3) uv.x = 0.5f;  // b3: the UV seam against a2
      s.uvs.push_back(uv);
      std::uint32_t id = static_cast<std::uint32_t>(p.y) * 4u + static_cast<std::uint32_t>(p.x);
      if (q == 2 && k == 0) id = 100;  // c0: named apart from b1
      s.ids.push_back(id);
    }
    const std::uint16_t base = static_cast<std::uint16_t>(q * 4);
    const std::uint16_t order[6] = {0, 1, 2, 0, 2, 3};
    for (const std::uint16_t k : order)
      s.indices.push_back(static_cast<std::uint16_t>(base + k));
  }
  return s;
}

// How the `_CANONICAL_ID` accessor is written. The first four are the forms the importer takes;
// the rest are the ones it refuses.
enum class IdForm {
  u32,
  u16,
  u8,
  f32,
  none,
  vec2,
  signed16,
  normalized8,
  fraction,
  negative,
  reserved
};

// The strip as a .gltf with an embedded buffer. `second_primitive` adds a second primitive over the
// same accessors **without** the id attribute, which is the mixed file.
std::string strip_gltf(const Strip& s, IdForm form, bool second_primitive = false) {
  std::vector<std::uint8_t> buffer;
  struct View {
    std::size_t offset = 0;
    std::size_t length = 0;
  };
  std::vector<View> views;
  auto view = [&](auto append) {
    View v;
    v.offset = buffer.size();
    append();
    v.length = buffer.size() - v.offset;
    views.push_back(v);
  };
  view([&] { append_raw(buffer, s.positions.data(), s.positions.size()); });
  view([&] { append_raw(buffer, s.normals.data(), s.normals.size()); });
  view([&] { append_raw(buffer, s.uvs.data(), s.uvs.size()); });
  const std::size_t n = s.ids.size();
  std::string id_type = "SCALAR";
  int component = 5125;
  bool normalized = false;
  view([&] {
    switch (form) {
      case IdForm::u32:
      case IdForm::none: append_raw(buffer, s.ids.data(), n); break;
      case IdForm::reserved: {
        std::vector<std::uint32_t> ids = s.ids;
        ids[7] = 0xffffffffu;
        append_raw(buffer, ids.data(), n);
        break;
      }
      case IdForm::u16:
      case IdForm::signed16: {
        std::vector<std::uint16_t> ids;
        for (const std::uint32_t id : s.ids)
          ids.push_back(static_cast<std::uint16_t>(id));
        append_raw(buffer, ids.data(), n);
        component = form == IdForm::u16 ? 5123 : 5122;
        break;
      }
      case IdForm::u8:
      case IdForm::normalized8: {
        std::vector<std::uint8_t> ids;
        for (const std::uint32_t id : s.ids)
          ids.push_back(static_cast<std::uint8_t>(id));
        append_raw(buffer, ids.data(), n);
        component = 5121;
        normalized = form == IdForm::normalized8;
        break;
      }
      case IdForm::f32:
      case IdForm::fraction:
      case IdForm::negative: {
        std::vector<float> ids;
        for (const std::uint32_t id : s.ids)
          ids.push_back(static_cast<float>(id));
        if (form == IdForm::fraction) ids[4] = 1.5f;
        if (form == IdForm::negative) ids[4] = -1.0f;
        append_raw(buffer, ids.data(), n);
        component = 5126;
        break;
      }
      case IdForm::vec2: {
        std::vector<std::uint32_t> ids;
        for (const std::uint32_t id : s.ids) {
          ids.push_back(id);
          ids.push_back(0);
        }
        append_raw(buffer, ids.data(), ids.size());
        id_type = "VEC2";
        break;
      }
    }
  });
  view([&] { append_raw(buffer, s.indices.data(), s.indices.size()); });

  const std::string count = std::to_string(n);
  std::string accessors =
      "{\"bufferView\":0,\"componentType\":5126,\"count\":" + count + ",\"type\":\"VEC3\"}," +
      "{\"bufferView\":1,\"componentType\":5126,\"count\":" + count + ",\"type\":\"VEC3\"}," +
      "{\"bufferView\":2,\"componentType\":5126,\"count\":" + count + ",\"type\":\"VEC2\"}," +
      "{\"bufferView\":3,\"componentType\":" + std::to_string(component) + ",\"count\":" + count +
      ",\"type\":\"" + id_type + "\"" + (normalized ? ",\"normalized\":true" : "") + "}," +
      "{\"bufferView\":4,\"componentType\":5123,\"count\":" + std::to_string(s.indices.size()) +
      ",\"type\":\"SCALAR\"}";
  std::string buffer_views;
  for (std::size_t i = 0; i < views.size(); ++i) {
    if (i != 0) buffer_views += ",";
    buffer_views += "{\"buffer\":0,\"byteOffset\":" + std::to_string(views[i].offset) +
                    ",\"byteLength\":" + std::to_string(views[i].length) + "}";
  }
  const std::string with_ids =
      form == IdForm::none ? std::string() : std::string(",\"") + k_canonical_id_attribute + "\":3";
  std::string primitives = "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2" +
                           with_ids + "},\"indices\":4}";
  if (second_primitive)
    primitives += ",{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":4}";
  return "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
         "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[" +
         primitives + "]}],\"accessors\":[" + accessors + "],\"bufferViews\":[" + buffer_views +
         "],\"buffers\":[{\"byteLength\":" + std::to_string(buffer.size()) +
         ",\"uri\":\"data:application/octet-stream;base64," + base64(buffer) + "\"}]}";
}

bool load(const std::string& text, MeshData& out, std::string& error) {
  return load_gltf_memory(
      std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()), "", out, &error);
}

// Where the weld sent each source vertex, read off the index buffer.
Vector<u32> welded_of(const Vector<u32>& before, const Vector<u32>& after, u32 source_vertices) {
  Vector<u32> out(source_vertices, ~0u);
  for (u32 i = 0; i < before.size(); ++i)
    out[before[i]] = after[i];
  return out;
}

// One DAG per primitive over the welded mesh, exactly as both builders build it.
bool build_parts(const MeshData& welded, Vector<geometry::ClusterLodMesh>& parts,
                 std::string& error) {
  const geometry::AttributeSource attributes = attribute_source(welded);
  for (const Primitive& primitive : welded.primitives) {
    if (primitive.index_count < 3) continue;
    const std::span<const u32> range(welded.indices.data() + primitive.first_index,
                                     primitive.index_count);
    geometry::ClusterLodMesh part;
    if (!geometry::build_cluster_lod(welded.positions, range, geometry::ClusterLodOptions{}, part,
                                     &error, attributes)) {
      return false;
    }
    parts.push_back(std::move(part));
  }
  return true;
}

// **The identity contract, end to end**: every cluster vertex of the built mesh carries the id of
// every source vertex it stands for, compared against the mesh *before* the weld through the map
// the weld's own index rewrite gives, so nothing of the weld's bookkeeping sits between the sides.
struct Identity {
  u32 copies = 0;  // (cluster vertex, source vertex it stands for) pairs looked at
  u32 wrong = 0;
};

Identity check_identity(const MeshData& source, const MeshData& welded,
                        std::span<const geometry::ClusterLodMesh> parts) {
  Identity out;
  const u32 source_vertices = source.positions.size();
  const Vector<u32> map = welded_of(source.indices, welded.indices, source_vertices);
  for (const geometry::ClusterLodMesh& part : parts) {
    const geometry::ClusterMesh& mesh = part.mesh;
    for (u32 v = 0; v < mesh.vertices.size(); ++v) {
      const u32 w = mesh.vertex_source[v];
      for (u32 o = 0; o < source_vertices; ++o) {
        if (map[o] != w) continue;
        ++out.copies;
        if (mesh.vertex_ids.size() != mesh.vertices.size() ||
            mesh.vertex_ids[v] != source.vertex_ids[o]) {
          ++out.wrong;
        }
      }
    }
  }
  return out;
}

// Source vertices, distinct ids among them, and how many share their id with another.
struct IdCounts {
  u32 vertices = 0;
  u32 distinct = 0;
  u32 sharing = 0;
};

IdCounts count_ids(const Vector<u32>& ids) {
  IdCounts out;
  Vector<u32> sorted = ids;
  std::sort(sorted.begin(), sorted.end());
  out.vertices = sorted.size();
  for (u32 i = 0; i < sorted.size();) {
    u32 j = i;
    while (j < sorted.size() && sorted[j] == sorted[i])
      ++j;
    ++out.distinct;
    if (j - i > 1) out.sharing += j - i;
    i = j;
  }
  return out;
}

}  // namespace

TEST_CASE("vertex ids: an authored _CANONICAL_ID keeps two points apart through the weld") {
  const Strip strip = make_strip();
  MeshData source;
  std::string error;
  REQUIRE_MESSAGE(load(strip_gltf(strip, IdForm::u32), source, error), error);
  REQUIRE(source.positions.size() == 12);
  CHECK(source.vertex_id_source == geometry::VertexIdSource::authored);
  REQUIRE(source.vertex_ids.size() == 12);
  for (u32 v = 0; v < 12; ++v)
    CHECK(source.vertex_ids[v] == strip.ids[v]);

  // The weld: a1 = b0 and b2 = c3 merge, a2 / b3 stay two (their UVs differ) and so do b1 / c0
  // (their ids differ), so 12 become 10.
  MeshData welded = source;
  CHECK(weld_vertices(welded) == 10);
  REQUIRE(welded.vertex_ids.size() == 10);
  u32 at_b1 = 0;       // vertices at (2, 0)
  u32 at_a2 = 0;       // vertices at (1, 1)
  bool b1_c0 = false;  // (2, 0) carries both 2 and 100
  u32 id2 = 0;
  u32 id100 = 0;
  for (u32 v = 0; v < welded.positions.size(); ++v) {
    const Vec3 p = welded.positions[v];
    if (p.x == 2.0f && p.y == 0.0f) {
      ++at_b1;
      id2 += welded.vertex_ids[v] == 2 ? 1u : 0u;
      id100 += welded.vertex_ids[v] == 100 ? 1u : 0u;
    }
    if (p.x == 1.0f && p.y == 1.0f) {
      ++at_a2;
      CHECK(welded.vertex_ids[v] == 5);  // both sides of the UV seam carry the point's one id
    }
  }
  b1_c0 = id2 == 1 && id100 == 1;
  CHECK(at_b1 == 2);
  CHECK(b1_c0);
  CHECK(at_a2 == 2);

  // **Without the stream the defect is back**: the geometry weld not handed the ids merges b1 and
  // c0 into one vertex, and one of the two names is gone. This is what the assets weld did before
  // the stream existed, and what the test would see if it stopped handing the ids over.
  {
    MeshData old = source;
    CHECK(geometry::weld_vertices(old.positions, old.normals, old.uvs,
                                  std::span<u32>(old.indices.data(), old.indices.size()),
                                  &old.skin_bindings, &old.morph) == 9);
  }

  // And through the builder, every cluster copy of every vertex carries its source's id.
  Vector<geometry::ClusterLodMesh> parts;
  REQUIRE_MESSAGE(build_parts(welded, parts, error), error);
  const Identity identity = check_identity(
      source, welded, std::span<const geometry::ClusterLodMesh>(parts.data(), parts.size()));
  CHECK(identity.copies == 12);
  CHECK(identity.wrong == 0);
  REQUIRE(parts.size() == 1);
  CHECK(parts[0].mesh.vertex_id_source == geometry::VertexIdSource::authored);
}

TEST_CASE("vertex ids: _CANONICAL_ID reads the same from every accepted component type") {
  const Strip strip = make_strip();
  for (const IdForm form : {IdForm::u32, IdForm::u16, IdForm::u8, IdForm::f32}) {
    MeshData mesh;
    std::string error;
    REQUIRE_MESSAGE(load(strip_gltf(strip, form), mesh, error), error);
    CHECK(mesh.vertex_id_source == geometry::VertexIdSource::authored);
    REQUIRE(mesh.vertex_ids.size() == strip.ids.size());
    for (u32 v = 0; v < strip.ids.size(); ++v)
      CHECK(mesh.vertex_ids[v] == strip.ids[v]);
  }
}

TEST_CASE("vertex ids: a malformed _CANONICAL_ID fails the load with a sentence") {
  const Strip strip = make_strip();
  struct Case {
    IdForm form;
    const char* says;
  };
  const Case cases[] = {
      {IdForm::vec2, "not a scalar"},           {IdForm::signed16, "signed"},
      {IdForm::normalized8, "normalized"},      {IdForm::fraction, "not a whole number"},
      {IdForm::negative, "not a whole number"}, {IdForm::reserved, "reserved"},
  };
  for (const Case& c : cases) {
    MeshData mesh;
    std::string error;
    CHECK_FALSE(load(strip_gltf(strip, c.form), mesh, error));
    CHECK_MESSAGE(error.find(k_canonical_id_attribute) != std::string::npos, error);
    CHECK_MESSAGE(error.find(c.says) != std::string::npos, error);
    CHECK(mesh.positions.empty());
    CHECK(mesh.vertex_ids.empty());
  }
}

TEST_CASE("vertex ids: a primitive without the attribute beside one with it has no id") {
  const Strip strip = make_strip();
  MeshData source;
  std::string error;
  REQUIRE_MESSAGE(load(strip_gltf(strip, IdForm::u32, /*second_primitive=*/true), source, error),
                  error);
  REQUIRE(source.positions.size() == 24);
  CHECK(source.vertex_id_source == geometry::VertexIdSource::authored);
  for (u32 v = 0; v < 12; ++v)
    CHECK(source.vertex_ids[v] == strip.ids[v]);
  for (u32 v = 12; v < 24; ++v)
    CHECK(source.vertex_ids[v] == geometry::k_no_vertex_id);
  // The id-less primitive welds as it always did — among its own vertices every coincident pair
  // agrees, b1 / c0 included, so 12 become 9 — and never into the named one: "no id" is not an id
  // the named vertices share. 10 + 9.
  MeshData welded = source;
  CHECK(weld_vertices(welded) == 19);
}

TEST_CASE("vertex ids: a mesh without the attribute is named by a position weld of the source") {
  const Strip strip = make_strip();
  MeshData source;
  std::string error;
  REQUIRE_MESSAGE(load(strip_gltf(strip, IdForm::none), source, error), error);
  CHECK(source.vertex_id_source == geometry::VertexIdSource::position_weld);
  REQUIRE(source.vertex_ids.size() == 12);
  // The rule, computed independently: the distinct positions sorted by x, then y, then z, and each
  // vertex named by its position's rank. The strip's eight grid points, x-major.
  Vector<Vec3> distinct;
  for (const Vec3& p : source.positions) {
    bool seen = false;
    for (const Vec3& q : distinct)
      seen = seen || (q.x == p.x && q.y == p.y && q.z == p.z);
    if (!seen) distinct.push_back(p);
  }
  std::sort(distinct.begin(), distinct.end(), [](const Vec3& a, const Vec3& b) {
    return a.x != b.x ? a.x < b.x : (a.y != b.y ? a.y < b.y : a.z < b.z);
  });
  REQUIRE(distinct.size() == 8);
  for (u32 v = 0; v < 12; ++v) {
    u32 rank = ~0u;
    for (u32 r = 0; r < distinct.size(); ++r) {
      const Vec3& q = distinct[r];
      if (q.x == source.positions[v].x && q.y == source.positions[v].y &&
          q.z == source.positions[v].z) {
        rank = r;
      }
    }
    CHECK(source.vertex_ids[v] == rank);
  }
  // So the two sides of the UV seam share their point's id — the control-level meaning — and so do
  // b1 and c0, which nothing in this file names apart.
  CHECK(source.vertex_ids[2] == source.vertex_ids[7]);  // a2, b3
  CHECK(source.vertex_ids[5] == source.vertex_ids[8]);  // b1, c0

  // A derived id never splits the weld: 12 become 9 exactly as without ids, and every cluster copy
  // carries its source's id.
  MeshData welded = source;
  CHECK(weld_vertices(welded) == 9);
  Vector<geometry::ClusterLodMesh> parts;
  REQUIRE_MESSAGE(build_parts(welded, parts, error), error);
  const Identity identity = check_identity(
      source, welded, std::span<const geometry::ClusterLodMesh>(parts.data(), parts.size()));
  CHECK(identity.copies == 12);
  CHECK(identity.wrong == 0);
}

// The Khronos skinned and morph samples `tools/fetch-samples.ps1` fetches, through the same steps
// both builders take — the load, the UV repair, the weld, one DAG per primitive — when they are
// present (CI does not fetch them; a bundle made with -WithSamples carries them). None carries an
// authored id, so each is named by the position weld; what is checked is that the name survives
// every step, that the derived ids split nothing the weld would have merged, and what the
// distinct-id and shared-id counts are (the MESSAGE lines are the table in assets.md).
TEST_CASE("vertex ids: the Khronos skinned and morph samples keep every vertex's name") {
  const char* k_samples[] = {"Fox/Fox.glb",
                             "RiggedFigure/RiggedFigure.glb",
                             "AnimatedMorphCube/AnimatedMorphCube.glb",
                             "MorphPrimitivesTest/MorphPrimitivesTest.glb",
                             "MorphStressTest/MorphStressTest.glb",
                             "SimpleMorph/SimpleMorph.gltf"};
  for (const char* relative : k_samples) {
    const std::string path =
        test::data_path(std::string(ENGINE_SOURCE_DIR "/content/samples/") + relative,
                        std::string("content/samples/") + relative);
    if (!test::path_exists(path)) {
      MESSAGE("not fetched (tools/fetch-samples.ps1): " << path);
      continue;
    }
    MeshData source;
    std::string error;
    REQUIRE_MESSAGE(load_gltf(path, source, &error), error);
    CHECK(source.vertex_id_source == geometry::VertexIdSource::position_weld);
    REQUIRE(source.vertex_ids.size() == source.positions.size());
    geometry::UvRepairReport repair;
    REQUIRE_MESSAGE(repair_uv_degenerate_triangles(source, repair, &error), error);

    // The weld with the ids and the weld without them give the same mesh.
    MeshData welded = source;
    const u32 vertices = weld_vertices(welded);
    MeshData unnamed = source;
    const u32 unnamed_vertices =
        geometry::weld_vertices(unnamed.positions, unnamed.normals, unnamed.uvs,
                                std::span<u32>(unnamed.indices.data(), unnamed.indices.size()),
                                &unnamed.skin_bindings, &unnamed.morph);
    CHECK(vertices == unnamed_vertices);
    CHECK(std::equal(welded.indices.begin(), welded.indices.end(), unnamed.indices.begin()));

    Vector<geometry::ClusterLodMesh> parts;
    REQUIRE_MESSAGE(build_parts(welded, parts, error), error);
    const Identity identity = check_identity(
        source, welded, std::span<const geometry::ClusterLodMesh>(parts.data(), parts.size()));
    const IdCounts imported = count_ids(source.vertex_ids);
    const IdCounts kept = count_ids(welded.vertex_ids);
    MESSAGE(std::string(relative) << ": " << imported.vertices << " imported vertices name "
                                  << imported.distinct << " positions, " << imported.sharing
                                  << " sharing an id; welded to " << kept.vertices
                                  << " vertices with " << kept.distinct << " distinct ids, "
                                  << kept.sharing << " sharing an id ("
                                  << kept.vertices - kept.distinct << " seam duplicates); "
                                  << identity.copies << " cluster copies checked, "
                                  << identity.wrong << " wrong");
    CHECK(identity.copies > 0);
    CHECK(identity.wrong == 0);
    // Every position the weld kept is still named, and by the name it had.
    CHECK(kept.distinct <= imported.distinct);
  }
}
