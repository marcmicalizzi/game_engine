// The imported-mesh side of the zero-area repair (`assets::repair_zero_area_triangles`): every
// primitive is looked at whatever its material, each keeps its material and gets its own range
// back, and no per-vertex stream moves. The rule itself is domain/geometry's and tested there
// (tests/zero_area_repair_tests.cpp).

#include <domain/assets/gltf.h>

#include <doctest/doctest.h>

using namespace engine;
using namespace engine::assets;

namespace {

void add_vertex(MeshData& m, Vec3 p, Vec2 uv) {
  m.positions.push_back(p);
  m.normals.push_back(Vec3{0, 0, 1});
  m.uvs.push_back(uv);
  m.vertex_ids.push_back(m.positions.size() - 1);
}

void add_triangle(MeshData& m, u32 a, u32 b, u32 c) {
  m.indices.push_back(a);
  m.indices.push_back(b);
  m.indices.push_back(c);
}

// Primitive 0 (material 0, textured): a quad and a collinear triangle between them (three distinct
// vertices on the quad's bottom edge line, the middle one halfway). Primitive 1 (material 1,
// untextured): a quad and a triangle two of whose distinct vertices sit at one position.
MeshData make_mesh() {
  MeshData m;
  Material textured;
  textured.base_color_image = 0;
  m.materials.push_back(textured);
  m.materials.push_back(Material{});
  m.images.push_back(ImageRef{});
  for (const Vec3 p : {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0}, Vec3{0, 1, 0}})
    add_vertex(m, p, Vec2{p.x * 0.5f, p.y * 0.5f});
  add_vertex(m, Vec3{0, 0, 0}, Vec2{0.75f, 0.75f});  // 4..6: on the line y = 0
  add_vertex(m, Vec3{0.5f, 0, 0}, Vec2{0.8f, 0.75f});
  add_vertex(m, Vec3{1, 0, 0}, Vec2{0.75f, 0.8f});
  add_triangle(m, 0, 1, 2);
  add_triangle(m, 4, 5, 6);  // collinear
  add_triangle(m, 0, 2, 3);
  m.primitives.push_back(Primitive{0, 9, 0, -1});
  for (const Vec3 p : {Vec3{2, 0, 0}, Vec3{3, 0, 0}, Vec3{3, 1, 0}, Vec3{2, 1, 0}})
    add_vertex(m, p, Vec2{0, 0});
  add_vertex(m, Vec3{3, 1, 0}, Vec2{0.25f, 0.25f});  // 11: vertex 9's position
  add_triangle(m, 7, 8, 9);
  add_triangle(m, 7, 9, 10);
  add_triangle(m, 8, 9, 11);  // coincident: 9 and 11
  m.primitives.push_back(Primitive{9, 9, 1, -1});
  return m;
}

}  // namespace

TEST_CASE("assets: the zero-area repair runs over every primitive and keeps each one's material") {
  MeshData m = make_mesh();
  const MeshData before = m;
  geometry::ZeroAreaRepairReport report;
  std::string error;
  REQUIRE_MESSAGE(repair_zero_area_triangles(m, report, &error), error);
  CHECK(report.triangles == 2);
  CHECK(report.collinear == 1);
  CHECK(report.coincident == 1);  // in the untextured primitive: geometry is everyone's
  CHECK(report.dropped == 2);
  CHECK(report.unrepaired == 0);

  // The survivors, in order, and each primitive's range around its own.
  const u32 expected[] = {0, 1, 2, 0, 2, 3, 7, 8, 9, 7, 9, 10};
  REQUIRE(m.indices.size() == 12);
  for (u32 i = 0; i < 12; ++i)
    CHECK(m.indices[i] == expected[i]);
  REQUIRE(m.primitives.size() == 2);
  CHECK(m.primitives[0].first_index == 0);
  CHECK(m.primitives[0].index_count == 6);
  CHECK(m.primitives[0].material == 0);
  CHECK(m.primitives[1].first_index == 6);
  CHECK(m.primitives[1].index_count == 6);
  CHECK(m.primitives[1].material == 1);

  // Every per-vertex stream is as the file had it: same length, same values, the dropped
  // triangles' own vertices included (the weld drops those once nothing references them).
  REQUIRE(m.positions.size() == before.positions.size());
  REQUIRE(m.normals.size() == before.normals.size());
  REQUIRE(m.uvs.size() == before.uvs.size());
  REQUIRE(m.vertex_ids.size() == before.vertex_ids.size());
  for (u32 v = 0; v < m.positions.size(); ++v) {
    CHECK(m.positions[v].x == before.positions[v].x);
    CHECK(m.positions[v].y == before.positions[v].y);
    CHECK(m.uvs[v].x == before.uvs[v].x);
    CHECK(m.uvs[v].y == before.uvs[v].y);
    CHECK(m.vertex_ids[v] == before.vertex_ids[v]);
  }
  CHECK(m.materials.size() == 2);

  // What is left welds to the two quads' eight vertices: the dropped triangles' four are gone.
  CHECK(weld_vertices(m) == 8);
}

TEST_CASE("assets: zero-area repair: primitives listed out of index order get their own ranges") {
  MeshData m = make_mesh();
  // The second range listed first.
  const Primitive first = m.primitives[0];
  m.primitives[0] = m.primitives[1];
  m.primitives[1] = first;
  geometry::ZeroAreaRepairReport report;
  REQUIRE(repair_zero_area_triangles(m, report));
  CHECK(report.dropped == 2);
  CHECK(m.primitives[0].first_index == 6);
  CHECK(m.primitives[0].index_count == 6);
  CHECK(m.primitives[0].material == 1);
  CHECK(m.primitives[1].first_index == 0);
  CHECK(m.primitives[1].index_count == 6);
  CHECK(m.primitives[1].material == 0);
}
