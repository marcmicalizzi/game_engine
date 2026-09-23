// The UV-degenerate repair (docs/subsystems/geometry.md, "UV-degenerate triangles: the repair").
// Every case is a synthetic mesh small enough to say exactly what the right answer is: a flat
// grid whose UVs are its positions, so "the neighbouring island's UV at this position" is the
// position itself, with one defect cut into it the way a generator leaves one.

#include <domain/geometry/cluster.h>
#include <domain/geometry/uv_repair.h>

#include <doctest/doctest.h>

#include <cmath>
#include <string>

using namespace engine;
using namespace engine::geometry;

namespace {

// n x n vertices over the unit square in z = 0, UV = (x, y), normals +z, 2 (n-1)^2 triangles
// wound counter-clockwise seen from +z. One island, every vertex shared.
struct Mesh {
  Vector<Vec3> positions;
  Vector<Vec3> normals;
  Vector<Vec2> uvs;
  Vector<u32> indices;
};

constexpr u32 k_n = 4;
constexpr f32 k_step = 1.0f / static_cast<f32>(k_n - 1);

u32 grid_vertex(u32 x, u32 y) { return y * k_n + x; }

Vec3 grid_position(u32 x, u32 y) {
  return Vec3{static_cast<f32>(x) * k_step, static_cast<f32>(y) * k_step, 0.0f};
}

Mesh make_grid(u32 skip_quad_x = ~0u, u32 skip_quad_y = ~0u) {
  Mesh m;
  for (u32 y = 0; y < k_n; ++y) {
    for (u32 x = 0; x < k_n; ++x) {
      const Vec3 p = grid_position(x, y);
      m.positions.push_back(p);
      m.normals.push_back(Vec3{0.0f, 0.0f, 1.0f});
      m.uvs.push_back(Vec2{p.x, p.y});
    }
  }
  for (u32 y = 0; y + 1 < k_n; ++y) {
    for (u32 x = 0; x + 1 < k_n; ++x) {
      if (x == skip_quad_x && y == skip_quad_y) continue;
      const u32 a = grid_vertex(x, y);
      const u32 b = grid_vertex(x + 1, y);
      const u32 c = grid_vertex(x, y + 1);
      const u32 d = grid_vertex(x + 1, y + 1);
      for (const u32 i : {a, b, c, b, d, c})
        m.indices.push_back(i);
    }
  }
  return m;
}

// Appends a vertex of its own (a UV-seam duplicate, as an exporter writes one) and returns it.
u32 add_vertex(Mesh& m, Vec3 p, Vec2 uv, Vec3 normal = Vec3{0.0f, 0.0f, 1.0f}) {
  m.positions.push_back(p);
  m.normals.push_back(normal);
  m.uvs.push_back(uv);
  return m.positions.size() - 1;
}

void add_triangle(Mesh& m, u32 a, u32 b, u32 c) {
  m.indices.push_back(a);
  m.indices.push_back(b);
  m.indices.push_back(c);
}

bool repair(Mesh& m, UvRepairReport& report, Vector<UvRepairRange>* ranges_out = nullptr,
            std::string* error = nullptr) {
  Vector<UvRepairRange> ranges;
  ranges.push_back(UvRepairRange{0, m.indices.size(), true});
  if (ranges_out != nullptr && !ranges_out->empty()) ranges = *ranges_out;
  const bool ok = repair_uv_degenerate_triangles(
      std::span<const Vec3>(m.positions.data(), m.positions.size()), m.normals, m.uvs, m.indices,
      std::span<UvRepairRange>(ranges.data(), ranges.size()), UvRepairOptions{}, report, error);
  if (ranges_out != nullptr) *ranges_out = ranges;
  return ok;
}

bool same_uv(Vec2 a, Vec2 b, f32 tolerance = 0.0f) {
  return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance;
}

}  // namespace

TEST_CASE("uv repair: areas are measured as stored, and a small triangle is not a small island") {
  CHECK(uv_area_texels(Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f}) == 0.0);
  // A right triangle one texel on a side is half a texel.
  const f32 texel = 1.0f / 4096.0f;
  CHECK(uv_area_texels(Vec2{0.0f, 0.0f}, Vec2{texel, 0.0f}, Vec2{0.0f, texel}) ==
        doctest::Approx(0.5));
  CHECK(k_uv_degenerate_texels == 1.0);
  // The E10 finding in one line: between u = 0.5 and 1 a half float's step is two texels of a
  // 4096 atlas, so a triangle of a third of a texel in the file is a point in the container...
  const Vec2 p{0.875f, 0.875f};
  const Vec2 q{0.875f + 0.8f * texel, 0.875f};
  const Vec2 r{0.875f, 0.875f + 0.8f * texel};
  CHECK(uv_area_texels(p, q, r) == doctest::Approx(0.32).epsilon(0.01));
  CHECK(stored_uv_area_texels(p, q, r) == 0.0);
  // ...and rounding can as easily inflate one: the same triangle off the half grid stores as two
  // texels. Either way the stored number, not the file's, is what the renderer samples.
  const Vec2 p1{0.9f, 0.9f};
  CHECK(stored_uv_area_texels(p1, Vec2{p1.x + 0.8f * texel, p1.y},
                              Vec2{p1.x, p1.y + 0.8f * texel}) == doctest::Approx(2.0));

  // A sound grid with a tiny triangle *inside* its island (it shares a vertex with the grid): a
  // thousandth of a texel, and nothing moves, byte for byte, because its island is large.
  Mesh m = make_grid();
  const f32 tiny = 0.05f * texel;
  const u32 e1 = add_vertex(m, Vec3{tiny, 0.0f, 0.0f}, Vec2{tiny, 0.0f});
  const u32 e2 = add_vertex(m, Vec3{0.0f, tiny, 0.0f}, Vec2{0.0f, tiny});
  add_triangle(m, grid_vertex(0, 0), e1, e2);
  CHECK(stored_uv_area_texels(m.uvs[grid_vertex(0, 0)], m.uvs[e1], m.uvs[e2]) < 0.01);
  const Mesh before = m;
  UvRepairReport report;
  REQUIRE(repair(m, report));
  CHECK(report.triangles == 0);
  CHECK(report.islands == 0);
  CHECK_FALSE(report.changed());
  CHECK(report.threshold_texels == k_uv_degenerate_texels);
  REQUIRE(m.indices.size() == before.indices.size());
  for (u32 i = 0; i < m.indices.size(); ++i)
    CHECK(m.indices[i] == before.indices[i]);
  for (u32 v = 0; v < m.uvs.size(); ++v)
    CHECK(same_uv(m.uvs[v], before.uvs[v]));
}

TEST_CASE("uv repair: a one-triangle island of no area takes its neighbours' UVs and welds in") {
  // One triangle of the grid replaced by a copy with vertices of its own whose three UVs
  // coincide: the E10 defect exactly, an island of zero texels in a sound atlas.
  Mesh m = make_grid(1, 1);
  const u32 a = grid_vertex(1, 1);
  const u32 b = grid_vertex(2, 1);
  const u32 c = grid_vertex(1, 2);
  const u32 d = grid_vertex(2, 2);
  add_triangle(m, a, b, c);  // the quad's first half stays sound
  // Its normal is not the grid's either, the way a remesher writes a vertex on a chart border.
  const Vec2 point{0.9f, 0.1f};
  const Vec3 tilted = normalize(Vec3{0.0f, 0.2f, 1.0f});
  const u32 b2 = add_vertex(m, m.positions[b], point, tilted);
  const u32 d2 = add_vertex(m, m.positions[d], point, tilted);
  const u32 c2 = add_vertex(m, m.positions[c], point, tilted);
  add_triangle(m, b2, d2, c2);
  const u32 grid_vertices = k_n * k_n;

  UvRepairReport report;
  REQUIRE(repair(m, report));
  CHECK(report.islands == 1);
  CHECK(report.triangles == 1);
  CHECK(report.refolded == 1);
  CHECK(report.dropped == 0);
  CHECK(report.unrepaired == 0);
  CHECK(report.corners_from_neighbours == 3);
  CHECK(report.corners_on_edge == 0);
  CHECK(report.changed());
  // Each corner now has the UV the island has at its position, so the triangle's UV area is its
  // share of the atlas again.
  CHECK(same_uv(m.uvs[b2], m.uvs[b]));
  CHECK(same_uv(m.uvs[d2], m.uvs[d]));
  CHECK(same_uv(m.uvs[c2], m.uvs[c]));
  CHECK(uv_area_texels(m.uvs[b2], m.uvs[d2], m.uvs[c2]) > 1.0);
  // And the island's normal there, so that the corner can become the island's vertex.
  for (const u32 v : {b2, d2, c2}) {
    CHECK(m.normals[v].x == 0.0f);
    CHECK(m.normals[v].y == 0.0f);
    CHECK(m.normals[v].z == 1.0f);
  }

  // And the weld that follows in the build merges the copies into the grid's own vertices: the
  // island is one island again, by index.
  const u32 welded = weld_vertices(m.positions, m.normals, m.uvs,
                                   std::span<u32>(m.indices.data(), m.indices.size()));
  CHECK(welded == grid_vertices);
}

TEST_CASE(
    "uv repair: an island of a third of a texel, a point once stored, folds in the same way") {
  // What E10's meshes actually had: not coincident UVs in the file, but an island so small that
  // the half-float UVs of the container store it as a point.
  Mesh m = make_grid(1, 1);
  const u32 b = grid_vertex(2, 1);
  const u32 c = grid_vertex(1, 2);
  const u32 d = grid_vertex(2, 2);
  add_triangle(m, grid_vertex(1, 1), b, c);
  const f32 texel = 1.0f / 4096.0f;
  const u32 b2 = add_vertex(m, m.positions[b], Vec2{0.875f, 0.875f});
  const u32 d2 = add_vertex(m, m.positions[d], Vec2{0.875f + 0.8f * texel, 0.875f});
  const u32 c2 = add_vertex(m, m.positions[c], Vec2{0.875f, 0.875f + 0.8f * texel});
  add_triangle(m, b2, d2, c2);
  CHECK(uv_area_texels(m.uvs[b2], m.uvs[d2], m.uvs[c2]) > 0.1);

  UvRepairReport report;
  REQUIRE(repair(m, report));
  CHECK(report.islands == 1);
  CHECK(report.refolded == 1);
  CHECK(same_uv(m.uvs[b2], m.uvs[b]));
  CHECK(same_uv(m.uvs[d2], m.uvs[d]));
  CHECK(same_uv(m.uvs[c2], m.uvs[c]));

  // An island just over the line is left alone: the same triangle at five texels.
  Mesh n = make_grid(1, 1);
  add_triangle(n, grid_vertex(1, 1), b, c);
  const u32 b3 = add_vertex(n, n.positions[b], Vec2{0.25f, 0.25f});
  const u32 d3 = add_vertex(n, n.positions[d], Vec2{0.25f + 4.0f * texel, 0.25f});
  const u32 c3 = add_vertex(n, n.positions[c], Vec2{0.25f, 0.25f + 4.0f * texel});
  add_triangle(n, b3, d3, c3);
  CHECK(stored_uv_area_texels(n.uvs[b3], n.uvs[d3], n.uvs[c3]) > 1.0);
  UvRepairReport untouched;
  REQUIRE(repair(n, untouched));
  CHECK(untouched.triangles == 0);
  CHECK(same_uv(n.uvs[b3], Vec2{0.25f, 0.25f}));
}

TEST_CASE("uv repair: a corner the island does not reach is put on the shared edge") {
  // A triangle hanging off the grid's right border: two corners on the border edge from (1, 0)
  // to (1, 1/3), the third out at (1.2, 0.1) where the island has no vertex.
  Mesh m = make_grid();
  const u32 e0 = grid_vertex(k_n - 1, 0);
  const u32 e1 = grid_vertex(k_n - 1, 1);
  const Vec2 point{0.2f, 0.7f};
  const u32 a = add_vertex(m, m.positions[e0], point);
  const u32 b = add_vertex(m, Vec3{1.2f, 0.1f, 0.0f}, point);
  const u32 c = add_vertex(m, m.positions[e1], point);
  add_triangle(m, a, b, c);

  UvRepairReport report;
  REQUIRE(repair(m, report));
  CHECK(report.refolded == 1);
  CHECK(report.corners_from_neighbours == 2);
  CHECK(report.corners_on_edge == 1);
  CHECK(same_uv(m.uvs[a], m.uvs[e0]));
  CHECK(same_uv(m.uvs[c], m.uvs[e1]));
  // (1.2, 0.1) is nearest the edge at y = 0.1, which is 0.3 of the way from e0 to e1: the UV
  // there, which on this grid is the position itself.
  CHECK(same_uv(m.uvs[b], Vec2{1.0f, 0.1f}, 1.0e-6f));
  // The triangle is joined to the island through the edge, and its UV is a segment inside it.
  CHECK(uv_area_texels(m.uvs[a], m.uvs[b], m.uvs[c]) < 1.0e-3);
}

TEST_CASE("uv repair: a patch of UV-degenerate triangles folds in from its border") {
  // One quad of the grid replaced by a fan of four triangles around a new centre vertex, all five
  // vertices its own and all with one UV: the centre has no twin in the island, so it is only
  // reachable once a first triangle of the fan has folded in and made it an island vertex.
  Mesh m = make_grid(1, 1);
  const Vec2 point{0.05f, 0.95f};
  const u32 a = add_vertex(m, grid_position(1, 1), point);
  const u32 b = add_vertex(m, grid_position(2, 1), point);
  const u32 d = add_vertex(m, grid_position(2, 2), point);
  const u32 c = add_vertex(m, grid_position(1, 2), point);
  const Vec3 centre = (grid_position(1, 1) + grid_position(2, 2)) * 0.5f;
  const u32 o = add_vertex(m, centre, point);
  add_triangle(m, a, b, o);
  add_triangle(m, b, d, o);
  add_triangle(m, d, c, o);
  add_triangle(m, c, a, o);

  UvRepairReport report;
  REQUIRE(repair(m, report));
  CHECK(report.islands == 1);
  CHECK(report.triangles == 4);
  CHECK(report.refolded == 4);
  CHECK(report.unrepaired == 0);
  CHECK(report.corners_from_neighbours == 4);  // the four fan corners, each once
  CHECK(report.corners_on_edge == 1);          // the centre, once, by the first triangle
  CHECK(same_uv(m.uvs[a], Vec2{centre.x - 0.5f * k_step, centre.y - 0.5f * k_step}, 1e-6f));
  CHECK(same_uv(m.uvs[d], Vec2{centre.x + 0.5f * k_step, centre.y + 0.5f * k_step}, 1e-6f));
  // The first triangle (a, b, o) put the centre on its edge a-b, at its foot: the midpoint.
  CHECK(same_uv(m.uvs[o], Vec2{centre.x, centre.y - 0.5f * k_step}, 1e-6f));
  // Every fan triangle now lies inside the island's part of the atlas, not at one far point.
  for (const u32 v : {a, b, c, d, o}) {
    CHECK(m.uvs[v].x >= grid_position(1, 1).x - 1e-6f);
    CHECK(m.uvs[v].x <= grid_position(2, 2).x + 1e-6f);
  }
}

TEST_CASE("uv repair: a UV-degenerate sliver thinner than the grid is dropped, runs move down") {
  // Two runs. The first holds the grid plus a sliver along an interior edge that is both
  // UV-degenerate and thinner than a step of the 16-bit grid (1/65535 of the unit extent); the
  // second is one sound triangle, whose run has to move down by the three indices dropped.
  Mesh m = make_grid();
  const Vec2 point{0.3f, 0.3f};
  const u32 s0 = add_vertex(m, Vec3{0.0f, 0.0f, 0.0f}, point);
  const u32 s1 = add_vertex(m, Vec3{k_step, 0.0f, 0.0f}, point);
  const u32 s2 = add_vertex(m, Vec3{0.5f * k_step, 1.0e-9f, 0.0f}, point);
  add_triangle(m, s0, s1, s2);
  const u32 first_run = m.indices.size();
  add_triangle(m, grid_vertex(0, 0), grid_vertex(1, 0), grid_vertex(0, 1));

  Vector<UvRepairRange> ranges;
  ranges.push_back(UvRepairRange{0, first_run, true});
  ranges.push_back(UvRepairRange{first_run, 3, true});
  UvRepairReport report;
  REQUIRE(repair(m, report, &ranges));
  CHECK(report.triangles == 1);
  CHECK(report.dropped == 1);
  CHECK(report.refolded == 0);
  CHECK(m.indices.size() == first_run);
  CHECK(ranges[0].first == 0);
  CHECK(ranges[0].count == first_run - 3);
  CHECK(ranges[1].first == first_run - 3);
  CHECK(ranges[1].count == 3);
  CHECK(m.indices[ranges[1].first] == grid_vertex(0, 0));
  for (u32 i = 0; i < m.indices.size(); ++i)
    CHECK((m.indices[i] != s0 && m.indices[i] != s1 && m.indices[i] != s2));
}

TEST_CASE("uv repair: what it leaves alone") {
  SUBCASE("a triangle with no neighbouring island is counted, not guessed at") {
    Mesh m = make_grid();
    const Vec2 point{0.6f, 0.6f};
    const u32 a = add_vertex(m, Vec3{5.0f, 5.0f, 0.0f}, point);
    const u32 b = add_vertex(m, Vec3{5.5f, 5.0f, 0.0f}, point);
    const u32 c = add_vertex(m, Vec3{5.0f, 5.5f, 0.0f}, point);
    add_triangle(m, a, b, c);
    UvRepairReport report;
    REQUIRE(repair(m, report));
    CHECK(report.triangles == 1);
    CHECK(report.unrepaired == 1);
    CHECK_FALSE(report.changed());
    CHECK(same_uv(m.uvs[a], point));
  }
  SUBCASE("a run whose material samples no texture") {
    Mesh m = make_grid(1, 1);
    const Vec2 point{0.9f, 0.1f};
    const u32 b2 = add_vertex(m, grid_position(2, 1), point);
    const u32 d2 = add_vertex(m, grid_position(2, 2), point);
    const u32 c2 = add_vertex(m, grid_position(1, 2), point);
    add_triangle(m, b2, d2, c2);
    Vector<UvRepairRange> ranges;
    ranges.push_back(UvRepairRange{0, m.indices.size(), false});
    UvRepairReport report;
    REQUIRE(repair(m, report, &ranges));
    CHECK(report.triangles == 0);
    CHECK(same_uv(m.uvs[b2], point));
  }
  SUBCASE("a UV fold inside a sound island is part of that island, not an island of its own") {
    // Vertices 0..4; triangle (1, 3, 2) has collinear UVs, but it shares all three corners with
    // sound triangles, so it belongs to their island and the island is large.
    Mesh m;
    add_vertex(m, Vec3{0.0f, 0.0f, 0.0f}, Vec2{0.0f, 0.0f});
    add_vertex(m, Vec3{1.0f, 0.0f, 0.0f}, Vec2{1.0f, 0.0f});
    add_vertex(m, Vec3{0.0f, 1.0f, 0.0f}, Vec2{0.0f, 1.0f});
    add_vertex(m, Vec3{1.0f, 1.0f, 0.0f}, Vec2{0.5f, 0.5f});
    add_vertex(m, Vec3{2.0f, 0.5f, 0.0f}, Vec2{1.0f, 0.5f});
    add_triangle(m, 0, 1, 2);
    add_triangle(m, 1, 3, 2);
    add_triangle(m, 1, 4, 3);
    const Mesh before = m;
    UvRepairReport report;
    REQUIRE(repair(m, report));
    CHECK(report.triangles == 0);
    CHECK_FALSE(report.changed());
    for (u32 v = 0; v < m.uvs.size(); ++v)
      CHECK(same_uv(m.uvs[v], before.uvs[v]));
  }
  SUBCASE("a mesh with no UVs") {
    Mesh m = make_grid();
    m.uvs.clear();
    UvRepairReport report;
    REQUIRE(repair(m, report));
    CHECK(report.triangles == 0);
  }
}

TEST_CASE("uv repair: malformed input is refused and the mesh is untouched") {
  Mesh m = make_grid();
  const Mesh before = m;
  UvRepairReport report;
  std::string error;
  SUBCASE("a run that is not whole triangles") {
    Vector<UvRepairRange> ranges;
    ranges.push_back(UvRepairRange{0, 4, true});
    CHECK_FALSE(repair(m, report, &ranges, &error));
    CHECK(error.find("whole number of triangles") != std::string::npos);
  }
  SUBCASE("an index outside the vertices") {
    m.indices[4] = 999;
    CHECK_FALSE(repair(m, report, nullptr, &error));
    CHECK(error.find("outside the") != std::string::npos);
    m.indices[4] = before.indices[4];
  }
  SUBCASE("UVs not parallel to the positions") {
    m.uvs.pop_back();
    CHECK_FALSE(repair(m, report, nullptr, &error));
    CHECK(error.find("UVs for") != std::string::npos);
    m.uvs = before.uvs;
  }
  SUBCASE("normals neither parallel nor absent") {
    m.normals.pop_back();
    CHECK_FALSE(repair(m, report, nullptr, &error));
    CHECK(error.find("normals for") != std::string::npos);
    m.normals = before.normals;
  }
  REQUIRE(m.indices.size() == before.indices.size());
  for (u32 i = 0; i < m.indices.size(); ++i)
    CHECK(m.indices[i] == before.indices[i]);
}
