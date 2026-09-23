// The imported-mesh side of the UV repair (`assets::repair_uv_degenerate_triangles`): which
// primitives it looks at, and that their ranges follow a dropped triangle. The rule itself is
// domain/geometry's and tested there (tests/uv_repair_tests.cpp).

#include <domain/assets/gltf.h>

#include <doctest/doctest.h>

using namespace engine;
using namespace engine::assets;

namespace {

// Appends a unit quad in z = 0 at x offset `x0` as two triangles over four shared vertices, with
// UVs over a quarter of the atlas: one sound island.
void add_quad(MeshData& m, f32 x0) {
  const u32 base = m.positions.size();
  const Vec3 p[4] = {Vec3{x0, 0, 0}, Vec3{x0 + 1, 0, 0}, Vec3{x0 + 1, 1, 0}, Vec3{x0, 1, 0}};
  const Vec2 t[4] = {Vec2{0, 0}, Vec2{0.5f, 0}, Vec2{0.5f, 0.5f}, Vec2{0, 0.5f}};
  for (u32 i = 0; i < 4; ++i) {
    m.positions.push_back(p[i]);
    m.normals.push_back(Vec3{0, 0, 1});
    m.uvs.push_back(t[i]);
  }
  for (const u32 i : {0u, 1u, 2u, 0u, 2u, 3u})
    m.indices.push_back(base + i);
}

// A sliver along the quad's bottom edge with three vertices of its own on one UV point: an island
// of no area that is also thinner than the mesh's grid step, so the repair drops it.
void add_sliver(MeshData& m, f32 x0) {
  const u32 base = m.positions.size();
  const Vec3 p[3] = {Vec3{x0, 0, 0}, Vec3{x0 + 1, 0, 0}, Vec3{x0 + 0.5f, 1.0e-9f, 0}};
  for (const Vec3& q : p) {
    m.positions.push_back(q);
    m.normals.push_back(Vec3{0, 0, 1});
    m.uvs.push_back(Vec2{0.75f, 0.75f});
  }
  for (u32 i = 0; i < 3; ++i)
    m.indices.push_back(base + i);
}

}  // namespace

TEST_CASE("assets: the UV repair runs per primitive, only where a material samples a texture") {
  MeshData m;
  Material textured;
  textured.base_color_image = 0;
  Material plain;
  m.materials.push_back(textured);
  m.materials.push_back(plain);
  m.images.push_back(ImageRef{});

  // Primitive 0 (textured): a quad and a sliver. Primitive 1 (untextured): the same again.
  add_quad(m, 0.0f);
  add_sliver(m, 0.0f);
  m.primitives.push_back(Primitive{0, 9, 0, -1});
  add_quad(m, 2.0f);
  add_sliver(m, 2.0f);
  m.primitives.push_back(Primitive{9, 9, 1, -1});

  geometry::UvRepairReport report;
  std::string error;
  REQUIRE_MESSAGE(repair_uv_degenerate_triangles(m, report, &error), error);
  // Only the textured primitive's sliver is repaired (dropped); the untextured one keeps its own.
  CHECK(report.islands == 1);
  CHECK(report.triangles == 1);
  CHECK(report.dropped == 1);
  CHECK(m.indices.size() == 15);
  CHECK(m.primitives[0].first_index == 0);
  CHECK(m.primitives[0].index_count == 6);
  CHECK(m.primitives[1].first_index == 6);
  CHECK(m.primitives[1].index_count == 9);
  CHECK(m.indices[6] == 7);  // the second quad's first vertex, now where the second range starts
}

TEST_CASE("assets: primitives listed out of index order get their own ranges back") {
  MeshData m;
  Material textured;
  textured.base_color_image = 0;
  m.materials.push_back(textured);
  m.images.push_back(ImageRef{});
  add_quad(m, 0.0f);
  add_sliver(m, 0.0f);
  add_quad(m, 2.0f);
  // The second range is listed first.
  m.primitives.push_back(Primitive{9, 6, 0, -1});
  m.primitives.push_back(Primitive{0, 9, 0, -1});
  geometry::UvRepairReport report;
  REQUIRE(repair_uv_degenerate_triangles(m, report));
  CHECK(report.dropped == 1);
  CHECK(m.primitives[0].first_index == 6);
  CHECK(m.primitives[0].index_count == 6);
  CHECK(m.primitives[1].first_index == 0);
  CHECK(m.primitives[1].index_count == 6);
}
