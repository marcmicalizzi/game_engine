#include <domain/geometry/cluster.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace engine;
using namespace engine::geometry;

namespace {

// A regular grid of (n x n) vertices in the unit square, 2 (n-1)^2 triangles.
void make_grid(u32 n, Vector<Vec3>& positions, Vector<u32>& indices) {
  positions.clear();
  indices.clear();
  for (u32 y = 0; y < n; ++y) {
    for (u32 x = 0; x < n; ++x) {
      positions.push_back(Vec3{static_cast<f32>(x) / (n - 1), static_cast<f32>(y) / (n - 1), 0.0f});
    }
  }
  for (u32 y = 0; y + 1 < n; ++y) {
    for (u32 x = 0; x + 1 < n; ++x) {
      const u32 a = y * n + x;
      const u32 b = a + 1;
      const u32 c = a + n;
      const u32 d = c + 1;
      indices.push_back(a);
      indices.push_back(c);
      indices.push_back(b);
      indices.push_back(b);
      indices.push_back(c);
      indices.push_back(d);
    }
  }
}

}  // namespace

TEST_CASE("clusters: a grid splits into valid clusters within the limits") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_grid(33, positions, indices);  // 2048 triangles
  ClusterMesh mesh;
  std::string error;
  const ClusterBuildOptions options{};
  REQUIRE_MESSAGE(build_clusters(positions, indices, options, mesh, &error), error);
  CHECK(mesh.source_vertex_count == 33 * 33);
  CHECK(mesh.source_triangle_count == 2048);
  CHECK(mesh.clusters.size() >= 2048 / 124 + 1);
  CHECK(mesh.clusters.size() <= 64);  // meshoptimizer packs well on a grid
  CHECK(mesh.triangles.size() == 2048);
  CHECK(mesh.vertices.size() >=
        positions.size());  // shared vertices are duplicated across clusters
  CHECK(mesh.vertices.size() < 2048 * 3);
  CHECK_MESSAGE(validate_clusters(mesh, indices, options, &error), error);
  u32 max_vertices = 0;
  u32 max_triangles = 0;
  for (const ClusterDesc& c : mesh.clusters) {
    max_vertices = c.vertex_count > max_vertices ? c.vertex_count : max_vertices;
    max_triangles = c.triangle_count > max_triangles ? c.triangle_count : max_triangles;
    CHECK(c.radius > 0.0f);
    CHECK(c.radius < 1.0f);
  }
  CHECK(max_vertices <= 64);
  CHECK(max_triangles <= 124);
  MESSAGE("clusters: " << mesh.clusters.size() << ", max vertices " << max_vertices
                       << ", max triangles " << max_triangles);
}

TEST_CASE("clusters: tighter limits produce more clusters and still validate") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_grid(9, positions, indices);  // 128 triangles
  ClusterBuildOptions small;
  small.max_vertices = 16;
  small.max_triangles = 16;
  ClusterMesh mesh;
  std::string error;
  REQUIRE(build_clusters(positions, indices, small, mesh, &error));
  CHECK(mesh.clusters.size() >= 8);
  CHECK(validate_clusters(mesh, indices, small, &error));
  // A single triangle is one cluster.
  const Vec3 tri[3] = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}};
  const u32 tri_indices[3] = {0, 1, 2};
  REQUIRE(build_clusters(tri, tri_indices, ClusterBuildOptions{}, mesh, &error));
  REQUIRE(mesh.clusters.size() == 1);
  CHECK(mesh.clusters[0].vertex_count == 3);
  CHECK(mesh.clusters[0].triangle_count == 1);
  CHECK(ClusterMesh::unpack(mesh.triangles[0], 0) < 3);
  CHECK(validate_clusters(mesh, tri_indices, ClusterBuildOptions{}, &error));
}

TEST_CASE("clusters: bad input is rejected") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_grid(3, positions, indices);
  ClusterMesh mesh;
  std::string error;
  CHECK_FALSE(build_clusters({}, indices, ClusterBuildOptions{}, mesh, &error));
  CHECK_FALSE(error.empty());
  const u32 not_triangles[4] = {0, 1, 2, 3};
  CHECK_FALSE(build_clusters(positions, not_triangles, ClusterBuildOptions{}, mesh, &error));
  const u32 out_of_range[3] = {0, 1, 99};
  CHECK_FALSE(build_clusters(positions, out_of_range, ClusterBuildOptions{}, mesh, &error));
  CHECK(error.find("out of range") != std::string::npos);
  ClusterBuildOptions bad;
  bad.max_triangles = 126;  // not a multiple of four
  CHECK_FALSE(build_clusters(positions, indices, bad, mesh, &error));
  bad = ClusterBuildOptions{};
  bad.max_vertices = 256;
  CHECK_FALSE(build_clusters(positions, indices, bad, mesh, &error));
  CHECK(ClusterMesh::pack(1, 2, 3) == (1u | (2u << 8) | (3u << 16)));
  CHECK(ClusterMesh::unpack(ClusterMesh::pack(7, 8, 9), 2) == 9);
}

TEST_CASE("attributes: octahedral normals and half UVs round-trip, builders carry them") {
  const Vec3 samples[] = {
      Vec3{0, 1, 0},          Vec3{0, -1, 0},           Vec3{1, 0, 0},
      Vec3{0, 0, -1},         Vec3{0.3f, 0.4f, 0.866f}, Vec3{-0.5f, 0.2f, -0.84f},
      Vec3{0.7f, -0.7f, 0.1f}};
  for (const Vec3& s : samples) {
    const Vec3 n = normalize(s);
    const Vec3 back = decode_normal_oct(encode_normal_oct(n));
    CHECK(length(back - n) < 2e-4f);
  }
  const Vec2 uvs[] = {Vec2{0, 0}, Vec2{1, 1}, Vec2{0.25f, 0.75f}, Vec2{-3.5f, 1000.0f},
                      Vec2{0.001f, 65504.0f}};
  for (const Vec2& uv : uvs) {
    const Vec2 back = decode_half2(encode_half2(uv));
    CHECK(std::fabs(back.x - uv.x) <= std::fabs(uv.x) * 1e-3f + 1e-6f);
    CHECK(std::fabs(back.y - uv.y) <= std::fabs(uv.y) * 1e-3f + 1e-6f);
  }
  CHECK(encode_half2(Vec2{0, 0}) == 0);

  // A flat grid: computed normals all point up; a builder without a source computes them.
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_grid(9, positions, indices);
  Vector<Vec3> normals;
  compute_vertex_normals(positions, indices, normals);
  REQUIRE(normals.size() == positions.size());
  // The grid lies in z = 0 and winds clockwise seen from +z, so its normals face -z.
  for (const Vec3& n : normals)
    CHECK(length(n - Vec3{0, 0, -1}) < 1e-5f);
  ClusterMesh mesh;
  std::string error;
  REQUIRE(build_clusters(positions, indices, ClusterBuildOptions{}, mesh, &error));
  REQUIRE(mesh.attributes.size() == mesh.vertices.size());
  for (const VertexAttributes& a : mesh.attributes) {
    CHECK(length(decode_normal_oct(a.normal_oct) - Vec3{0, 0, -1}) < 2e-4f);
    CHECK(a.uv_half2 == 0);
  }

  // With a source: attributes follow vertex_source.
  Vector<Vec2> source_uvs;
  Vector<Vec3> source_normals;
  for (const Vec3& p : positions) {
    source_uvs.push_back(Vec2{p.x, p.y});
    source_normals.push_back(normalize(Vec3{p.x - 0.5f, p.y - 0.5f, 1.0f}));
  }
  AttributeSource source;
  source.normals = std::span<const Vec3>(source_normals.data(), source_normals.size());
  source.uvs = std::span<const Vec2>(source_uvs.data(), source_uvs.size());
  REQUIRE(build_clusters(positions, indices, ClusterBuildOptions{}, mesh, &error, source));
  REQUIRE(mesh.attributes.size() == mesh.vertex_source.size());
  for (u32 i = 0; i < mesh.attributes.size(); ++i) {
    const u32 s = mesh.vertex_source[i];
    const Vec2 uv = decode_half2(mesh.attributes[i].uv_half2);
    CHECK(std::fabs(uv.x - source_uvs[s].x) < 1e-3f);
    CHECK(std::fabs(uv.y - source_uvs[s].y) < 1e-3f);
    CHECK(length(decode_normal_oct(mesh.attributes[i].normal_oct) - source_normals[s]) < 2e-4f);
  }
}

namespace {

// A unit sphere wound counter-clockwise seen from outside: one pole vertex at each end, `rings`
// latitude bands of `segments` vertices, fans at the poles and two triangles per quad between.
void make_sphere(u32 rings, u32 segments, Vector<Vec3>& positions, Vector<u32>& indices) {
  positions.clear();
  indices.clear();
  positions.push_back(Vec3{0.0f, 1.0f, 0.0f});
  for (u32 r = 1; r < rings; ++r) {
    const f32 theta = k_pi * static_cast<f32>(r) / static_cast<f32>(rings);
    for (u32 s = 0; s < segments; ++s) {
      const f32 phi = k_two_pi * static_cast<f32>(s) / static_cast<f32>(segments);
      positions.push_back(
          Vec3{std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)});
    }
  }
  positions.push_back(Vec3{0.0f, -1.0f, 0.0f});
  const u32 bottom = positions.size() - 1;
  auto ring = [segments](u32 r, u32 s) { return 1 + (r - 1) * segments + (s % segments); };
  for (u32 s = 0; s < segments; ++s) {
    indices.push_back(0);
    indices.push_back(ring(1, s + 1));
    indices.push_back(ring(1, s));
  }
  for (u32 r = 1; r + 1 < rings; ++r) {
    for (u32 s = 0; s < segments; ++s) {
      const u32 a = ring(r, s);
      const u32 b = ring(r, s + 1);
      const u32 c = ring(r + 1, s);
      const u32 d = ring(r + 1, s + 1);
      indices.push_back(a);
      indices.push_back(b);
      indices.push_back(c);
      indices.push_back(b);
      indices.push_back(d);
      indices.push_back(c);
    }
  }
  for (u32 s = 0; s < segments; ++s) {
    indices.push_back(ring(rings - 1, s));
    indices.push_back(ring(rings - 1, s + 1));
    indices.push_back(bottom);
  }
}

// The largest per-axis distance between a float position and its point on the 16-bit grid.
f32 worst_quantization_error(const ClusterMesh& mesh) {
  f32 worst = 0.0f;
  for (u32 v = 0; v < mesh.vertices.size(); ++v) {
    const Vec3 p = mesh.vertices[v];
    const Vec3 q = dequantize_position(mesh, v);
    const f32 axis[3] = {std::fabs(q.x - p.x), std::fabs(q.y - p.y), std::fabs(q.z - p.z)};
    for (const f32 e : axis)
      worst = e > worst ? e : worst;
  }
  return worst;
}

}  // namespace

TEST_CASE(
    "normal cones: packing round-trips, a sphere's far side is backfacing, cones can be off") {
  // Packing: the axis within a snorm8 step, the cutoff rounded up, never down.
  const NormalCone tilted = decode_cone(encode_cone(normalize(Vec3{0.3f, -0.5f, 0.8f}), 0.5f));
  CHECK(length(tilted.axis - normalize(Vec3{0.3f, -0.5f, 0.8f})) < 1.5f / 127.0f);
  CHECK(tilted.cutoff >= 0.5f);
  CHECK(tilted.cutoff <= 0.5f + 1.0f / 127.0f);
  CHECK(encode_cone(Vec3{}, 1.0f) == k_cone_none);
  CHECK(decode_cone(k_cone_none).cutoff == 1.0f);
  CHECK(decode_cone(encode_cone(Vec3{0, 0, 1}, 0.999f)).cutoff == 1.0f);  // rounds up to none

  Vector<Vec3> positions;
  Vector<u32> indices;
  make_sphere(24, 48, positions, indices);  // 2,208 triangles
  ClusterMesh mesh;
  std::string error;
  REQUIRE_MESSAGE(build_clusters(positions, indices, ClusterBuildOptions{}, mesh, &error), error);
  CHECK_MESSAGE(validate_clusters(mesh, indices, ClusterBuildOptions{}, &error), error);
  u32 with_cone = 0;
  u32 backfacing = 0;
  const Vec3 camera{0.0f, 0.0f, 4.0f};
  for (const ClusterDesc& c : mesh.clusters) {
    const NormalCone cone = decode_cone(c.cone);
    if (cone.cutoff < 1.0f) {
      ++with_cone;
      CHECK(cone.cutoff >= 0.0f);
      // The axis of a cluster on a sphere points roughly outward at its center.
      CHECK(dot(normalize(cone.axis), normalize(c.center)) > 0.8f);
      CHECK(length(c.cone_apex) < 1.05f);  // apex inside or near the unit sphere
    }
    const bool back = cluster_backfacing(c, camera);
    backfacing += back;
    if (c.center.z > 0.25f) CHECK_FALSE(back);  // facing the camera: never culled
    if (c.center.z < -0.6f && cone.cutoff < 1.0f) CHECK(back);
  }
  MESSAGE("clusters " << mesh.clusters.size() << ", with cones " << with_cone << ", backfacing "
                      << backfacing);
  CHECK(with_cone * 10 >= mesh.clusters.size() * 8);  // a sphere's clusters are compact
  CHECK(backfacing * 100 >= mesh.clusters.size() * 20);
  CHECK(backfacing * 100 <= mesh.clusters.size() * 60);

  // From the center every outward-wound cluster shows its back; the apex form of the test
  // catches nearly all of them from there.
  u32 from_inside = 0;
  for (const ClusterDesc& c : mesh.clusters)
    from_inside += cluster_backfacing(c, Vec3{});
  CHECK(from_inside * 10 >= with_cone * 9);

  // Cones off: nothing is ever backfacing, and the mesh still validates.
  ClusterBuildOptions two_sided;
  two_sided.normal_cones = false;
  REQUIRE(build_clusters(positions, indices, two_sided, mesh, &error));
  CHECK(validate_clusters(mesh, indices, two_sided, &error));
  for (const ClusterDesc& c : mesh.clusters) {
    CHECK(c.cone == k_cone_none);
    CHECK_FALSE(cluster_backfacing(c, camera));
  }

  // A cone that does not contain a face fails validation.
  REQUIRE(build_clusters(positions, indices, ClusterBuildOptions{}, mesh, &error));
  mesh.clusters[0].cone = encode_cone(normalize(Vec3{0, 0, 1}) * -1.0f, 0.1f);
  mesh.clusters[0].cone_apex = mesh.clusters[0].center;
  bool any_failed = false;
  for (ClusterDesc& c : mesh.clusters) {
    const u32 saved = c.cone;
    c.cone = encode_cone(normalize(c.center) * -1.0f, 0.1f);  // points inward
    if (!validate_clusters(mesh, indices, ClusterBuildOptions{}, &error)) {
      any_failed = true;
      CHECK(error.find("cone") != std::string::npos);
    }
    c.cone = saved;
  }
  CHECK(any_failed);
}

namespace {

// A triangle's normal on the 16-bit grid, as the rasterizers draw it, in double; false for one
// thinner than a grid step, which the cone deliberately leaves out (geometry.md, "Normal cones").
bool grid_normal(const ClusterMesh& mesh, u32 v0, u32 v1, u32 v2, f64 n[3], f64 p0[3]) {
  f64 p[3][3];
  const u32 v[3] = {v0, v1, v2};
  for (u32 k = 0; k < 3; ++k) {
    const Vec3 q = dequantize_position(mesh, v[k]);
    p[k][0] = static_cast<f64>(q.x);
    p[k][1] = static_cast<f64>(q.y);
    p[k][2] = static_cast<f64>(q.z);
  }
  const f64 e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
  const f64 e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
  const f64 e3[3] = {p[2][0] - p[1][0], p[2][1] - p[1][1], p[2][2] - p[1][2]};
  n[0] = e1[1] * e2[2] - e1[2] * e2[1];
  n[1] = e1[2] * e2[0] - e1[0] * e2[2];
  n[2] = e1[0] * e2[1] - e1[1] * e2[0];
  auto len = [](const f64 a[3]) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); };
  const f64 longest = std::max(std::max(len(e1), len(e2)), len(e3));
  const f64 l = len(n);
  if (!(l > static_cast<f64>(mesh.quant_scale) * longest)) return false;
  for (u32 k = 0; k < 3; ++k) {
    n[k] /= l;
    p0[k] = p[0][k];
  }
  return true;
}

}  // namespace

// The cone is fit to what the rasterizers draw, which is the 16-bit grid and not the floats. A unit
// sphere beside one triangle a thousand units away quantizes on a 0.0153 step, eight or nine steps
// to a triangle edge, so the grid turns normals by a tenth of a radian and more — and a cone tight
// around the float normals, which is what meshoptimizer builds, is not a cone around the grid. The
// brute-force half: from cameras all round the sphere at three distances, no cluster the
// cone test culls holds a grid triangle that faces the camera. Before the refit this failed.
TEST_CASE("normal cones: fit to the grid the rasterizers draw, with a margin") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_sphere(24, 48, positions, indices);
  const u32 far = positions.size();
  positions.push_back(Vec3{1000.0f, 0.0f, 0.0f});
  positions.push_back(Vec3{1000.0f, 1.0f, 0.0f});
  positions.push_back(Vec3{1000.0f, 0.0f, 1.0f});
  indices.push_back(far);
  indices.push_back(far + 1);
  indices.push_back(far + 2);
  ClusterMesh mesh;
  std::string error;
  REQUIRE_MESSAGE(build_clusters(positions, indices, ClusterBuildOptions{}, mesh, &error), error);
  CHECK_MESSAGE(validate_clusters(mesh, indices, ClusterBuildOptions{}, &error), error);
  CHECK(mesh.quant_scale > 0.015f);

  // How far the grid turns a triangle's normal from its float one: the reason this test exists.
  f64 widest_turn = 0.0;
  for (const ClusterDesc& c : mesh.clusters) {
    for (u32 t = 0; t < c.triangle_count; ++t) {
      const u32 packed = mesh.triangles[c.triangle_offset + t];
      const u32 v0 = c.vertex_offset + ClusterMesh::unpack(packed, 0);
      const u32 v1 = c.vertex_offset + ClusterMesh::unpack(packed, 1);
      const u32 v2 = c.vertex_offset + ClusterMesh::unpack(packed, 2);
      f64 n[3];
      f64 p[3];
      if (!grid_normal(mesh, v0, v1, v2, n, p)) continue;
      const Vec3 f = normalize(
          cross(mesh.vertices[v1] - mesh.vertices[v0], mesh.vertices[v2] - mesh.vertices[v0]));
      const f64 d = n[0] * static_cast<f64>(f.x) + n[1] * static_cast<f64>(f.y) +
                    n[2] * static_cast<f64>(f.z);
      widest_turn = std::max(widest_turn, std::acos(std::min(1.0, d)));
    }
  }
  CHECK(widest_turn > 0.05);

  u32 culled = 0;
  u32 checked = 0;
  u32 facing = 0;
  u32 with_cone = 0;
  for (const ClusterDesc& c : mesh.clusters)
    with_cone += decode_cone(c.cone).cutoff < 1.0f ? 1u : 0u;
  constexpr u32 k_cameras = 96;
  for (u32 i = 0; i < k_cameras; ++i) {
    // A Fibonacci sphere of directions, at three distances: close, near, and far.
    const f32 y = 1.0f - 2.0f * (static_cast<f32>(i) + 0.5f) / static_cast<f32>(k_cameras);
    const f32 r = std::sqrt(std::max(0.0f, 1.0f - y * y));
    const f32 phi = 2.39996323f * static_cast<f32>(i);
    const f32 distance = i % 3 == 0 ? 1.2f : (i % 3 == 1 ? 2.5f : 20.0f);
    const Vec3 camera = Vec3{r * std::cos(phi), y, r * std::sin(phi)} * distance;
    for (const ClusterDesc& c : mesh.clusters) {
      if (!cluster_backfacing(c, camera)) continue;
      ++culled;
      for (u32 t = 0; t < c.triangle_count; ++t) {
        const u32 packed = mesh.triangles[c.triangle_offset + t];
        f64 n[3];
        f64 p[3];
        if (!grid_normal(mesh, c.vertex_offset + ClusterMesh::unpack(packed, 0),
                         c.vertex_offset + ClusterMesh::unpack(packed, 1),
                         c.vertex_offset + ClusterMesh::unpack(packed, 2), n, p))
          continue;
        ++checked;
        const f64 to_triangle = (p[0] - static_cast<f64>(camera.x)) * n[0] +
                                (p[1] - static_cast<f64>(camera.y)) * n[1] +
                                (p[2] - static_cast<f64>(camera.z)) * n[2];
        if (to_triangle < 0.0) ++facing;  // the camera is in front of this triangle
      }
    }
  }
  MESSAGE("grid step " << mesh.quant_scale << ", widest turn " << widest_turn << " rad, "
                       << with_cone << " of " << mesh.clusters.size() << " clusters with a cone, "
                       << culled << " culls over " << k_cameras << " cameras, " << checked
                       << " grid triangles checked, " << facing << " facing the camera");
  CHECK(culled > 0);
  CHECK(with_cone * 2 >= mesh.clusters.size());  // the margin costs cones, not most of them
  CHECK(facing == 0);
}

// Two triangles with no normal, which the cone must neither widen for nor fail on: two coincident
// corners — a UV sphere's pole, and the triangle whose FMA residue made every x86-64-v3 GCC and
// clang build fail the morph test (docs/ci/local-linux.md) — and a sliver along an edge through a
// point placed on it in float, collinear up to rounding. The plane is tilted so no coordinate is
// round and the sliver's float cross product is rounding residue with a direction of its own.
TEST_CASE("normal cones: a triangle with no normal neither widens nor breaks its cone") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_grid(9, positions, indices);
  for (Vec3& p : positions)
    p.z = 0.37f * p.x + 0.21f * p.y;
  const u32 a = 4 * 9 + 4;  // an interior vertex and its neighbour along x
  const u32 b = a + 1;
  const u32 duplicate = positions.size();
  positions.push_back(positions[a]);
  const u32 along = positions.size();
  positions.push_back(positions[a] + (positions[b] - positions[a]) * 0.3f);
  for (const u32 i : {a, duplicate, b, a, along, b})
    indices.push_back(i);

  ClusterMesh mesh;
  std::string error;
  REQUIRE_MESSAGE(build_clusters(positions, indices, ClusterBuildOptions{}, mesh, &error), error);
  CHECK_MESSAGE(validate_clusters(mesh, indices, ClusterBuildOptions{}, &error), error);
  // Every face of a plane has the same normal, so every cluster keeps a cone as narrow as the
  // snorm8 axis and the margin allow: a few 127ths.
  for (const ClusterDesc& c : mesh.clusters) {
    const NormalCone cone = decode_cone(c.cone);
    CHECK(cone.cutoff <= 3.0f / 127.0f);
  }
}

TEST_CASE("weld: unindexed copies merge back, differing attributes stay apart") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_grid(9, positions, indices);  // 81 vertices, 128 triangles
  Vector<Vec3> normals;
  compute_vertex_normals(positions, indices, normals);
  // Unindex: one vertex per corner, 384 of them, like an exporter that writes no index buffer.
  Vector<Vec3> flat_positions;
  Vector<Vec3> flat_normals;
  Vector<Vec2> flat_uvs;
  Vector<u32> flat_indices;
  for (const u32 i : indices) {
    flat_indices.push_back(flat_positions.size());
    flat_positions.push_back(positions[i]);
    flat_normals.push_back(normals[i]);
    flat_uvs.push_back(Vec2{positions[i].x, positions[i].y});
  }
  REQUIRE(flat_positions.size() == 384);
  const Vector<Vec3> expected_positions = flat_positions;
  const Vector<u32> corner_order = flat_indices;
  const u32 unique = weld_vertices(flat_positions, flat_normals, flat_uvs, flat_indices);
  CHECK(unique == 81);
  CHECK(flat_positions.size() == 81);
  CHECK(flat_normals.size() == 81);
  CHECK(flat_uvs.size() == 81);
  for (u32 c = 0; c < flat_indices.size(); ++c) {  // every corner still lands on its position
    const Vec3 p = flat_positions[flat_indices[c]];
    const Vec3 e = expected_positions[corner_order[c]];
    CHECK(p.x == e.x);
    CHECK(p.y == e.y);
    CHECK(p.z == e.z);
    CHECK(flat_uvs[flat_indices[c]].x == e.x);
  }
  ClusterMesh mesh;
  std::string error;
  REQUIRE(build_clusters(flat_positions, flat_indices, ClusterBuildOptions{}, mesh, &error));
  CHECK(mesh.clusters.size() <= 3);  // 128 triangles over shared vertices: a few full clusters
  CHECK(validate_clusters(mesh, flat_indices, ClusterBuildOptions{}, &error));

  // A corner with a different normal keeps its own vertex; an unreferenced vertex disappears.
  Vector<Vec3> split_positions = expected_positions;
  Vector<Vec3> split_normals;
  Vector<Vec2> no_uvs;
  for (const u32 i : indices)
    split_normals.push_back(normals[i]);
  split_normals[5] = Vec3{0.0f, 0.0f, 1.0f};          // flipped against its neighbours
  split_positions.push_back(Vec3{9.0f, 9.0f, 9.0f});  // never indexed
  split_normals.push_back(Vec3{0.0f, 1.0f, 0.0f});
  Vector<u32> split_indices = corner_order;
  CHECK(weld_vertices(split_positions, split_normals, no_uvs, split_indices) == 82);
  CHECK(split_positions.size() == 82);
  CHECK(no_uvs.empty());
  bool found_far = false;
  for (const Vec3& p : split_positions)
    found_far = found_far || p.x == 9.0f;
  CHECK_FALSE(found_far);
}

TEST_CASE("quantized positions: one grid per mesh, half a step of error, six bytes a vertex") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_grid(33, positions, indices);  // the unit square: 1089 vertices, 2048 triangles
  ClusterMesh mesh;
  std::string error;
  REQUIRE_MESSAGE(build_clusters(positions, indices, ClusterBuildOptions{}, mesh, &error), error);

  // Three u16 a vertex, padded with at most one entry to an even count so a shader may read the
  // last triple as two whole 32-bit words, and still half of what the floats cost.
  CHECK(mesh.quantized.size() >= mesh.vertices.size() * 3);
  CHECK(mesh.quantized.size() <= mesh.vertices.size() * 3 + 1);
  CHECK(mesh.quantized.size() % 2 == 0);
  CHECK(mesh.quantized.size() * sizeof(u16) <= mesh.vertices.size() * sizeof(Vec3) / 2 + 2);
  // A mesh one unit across gets a step of 1 / 65535.
  CHECK(mesh.quant_scale > 1.52e-5f);
  CHECK(mesh.quant_scale < 1.53e-5f);
  CHECK(mesh.quant_origin.x <= 0.0f);
  CHECK(worst_quantization_error(mesh) <= mesh.quant_scale * 0.5f + 1e-6f);
  CHECK_MESSAGE(validate_clusters(mesh, indices, ClusterBuildOptions{}, &error), error);
  MESSAGE("grid: step " << mesh.quant_scale << ", worst error " << worst_quantization_error(mesh));

  // Every cluster's copy of a shared source vertex lands on the same integer triple, which is
  // what keeps a welded mesh crack-free through quantization.
  u32 shared = 0;
  for (u32 a = 0; a < mesh.vertices.size(); ++a) {
    for (u32 b = a + 1; b < mesh.vertices.size(); ++b) {
      if (mesh.vertex_source[a] != mesh.vertex_source[b]) continue;
      ++shared;
      for (u32 k = 0; k < 3; ++k)
        CHECK(mesh.quantized[a * 3 + k] == mesh.quantized[b * 3 + k]);
    }
  }
  CHECK(shared > 0);  // the grid's clusters do share vertices

  // The same grid a thousand units across: the step scales with the mesh, the error with it.
  Vector<Vec3> large = positions;
  for (Vec3& p : large)
    p = p * 1000.0f;
  REQUIRE(build_clusters(large, indices, ClusterBuildOptions{}, mesh, &error));
  CHECK(mesh.quant_scale > 0.0152f);
  CHECK(mesh.quant_scale < 0.0153f);
  CHECK(worst_quantization_error(mesh) <= mesh.quant_scale * 0.5f + 1e-6f);
  CHECK(validate_clusters(mesh, indices, ClusterBuildOptions{}, &error));

  // A sphere: the grid spans the largest extent of the whole mesh, two units here.
  Vector<Vec3> sphere_positions;
  Vector<u32> sphere_indices;
  make_sphere(24, 48, sphere_positions, sphere_indices);
  REQUIRE(build_clusters(sphere_positions, sphere_indices, ClusterBuildOptions{}, mesh, &error));
  CHECK(mesh.quant_scale > 3.04e-5f);
  CHECK(mesh.quant_scale < 3.06e-5f);
  CHECK(mesh.quant_origin.y < -0.999f);
  CHECK(worst_quantization_error(mesh) <= mesh.quant_scale * 0.5f + 1e-6f);
  CHECK_MESSAGE(validate_clusters(mesh, sphere_indices, ClusterBuildOptions{}, &error), error);
  MESSAGE("sphere: step " << mesh.quant_scale << ", worst error "
                          << worst_quantization_error(mesh));

  // A degenerate mesh (one point) keeps the identity step rather than dividing by zero.
  const Vec3 point[3] = {Vec3{2.0f, 3.0f, 4.0f}, Vec3{2.0f, 3.0f, 4.0f}, Vec3{2.0f, 3.0f, 4.0f}};
  const u32 point_indices[3] = {0, 1, 2};
  REQUIRE(build_clusters(point, point_indices, ClusterBuildOptions{}, mesh, &error));
  CHECK(mesh.quant_scale == 1.0f);
  CHECK(mesh.quant_origin.x == 2.0f);
  CHECK(dequantize_position(mesh, 0).z == 4.0f);
  CHECK(dequantize_position(mesh, 10000).x == 2.0f);  // out of range reads as the origin
}

TEST_CASE("skin bindings: weights sum to 255, travel through the builder, and split a weld") {
  // Quantization: four influences normalize to exactly 255, and the rounding error lands on the
  // biggest one rather than on whichever happens to come last.
  const u32 four_joints[4] = {3, 1, 7, 0};
  const f32 thirds[4] = {1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f, 0.0f};
  const SkinBinding even = make_skin_binding(four_joints, thirds);
  CHECK(u32{even.weights[0]} + even.weights[1] + even.weights[2] + even.weights[3] == 255);
  CHECK(even.joints[0] == 3);
  CHECK(even.joints[2] == 7);
  CHECK(even.weights[3] == 0);
  // Unnormalized input is normalized; a single influence takes the whole 255.
  const f32 lopsided[4] = {6.0f, 2.0f, 0.0f, 0.0f};
  const SkinBinding scaled = make_skin_binding(four_joints, lopsided);
  CHECK(u32{scaled.weights[0]} + scaled.weights[1] + scaled.weights[2] + scaled.weights[3] == 255);
  CHECK(scaled.weights[0] == 191);
  CHECK(scaled.weights[1] == 64);
  const f32 single[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  CHECK(make_skin_binding(four_joints, single).weights[0] == 255);
  // No weight at all binds the vertex rigidly to its first joint instead of leaving it weightless
  // at the palette's origin, which is what a mesh with JOINTS_0 and no WEIGHTS_0 means.
  const f32 nothing[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  const SkinBinding rigid = make_skin_binding(four_joints, nothing);
  CHECK(rigid.joints[0] == 3);
  CHECK(rigid.weights[0] == 255);
  CHECK(SkinBinding{}.weights[0] == 255);  // a default binding is already valid

  // Two joints across the grid: joint 0 at x = 0, joint 1 at x = 1.
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_grid(9, positions, indices);
  Vector<SkinBinding> source_skin;
  for (const Vec3& p : positions) {
    const u32 joints[4] = {0, 1, 0, 0};
    const f32 weights[4] = {1.0f - p.x, p.x, 0.0f, 0.0f};
    source_skin.push_back(make_skin_binding(joints, weights));
  }
  AttributeSource attributes;
  attributes.skin = std::span<const SkinBinding>(source_skin.data(), source_skin.size());
  attributes.joint_count = 2;

  ClusterMesh mesh;
  std::string error;
  REQUIRE_MESSAGE(
      build_clusters(positions, indices, ClusterBuildOptions{}, mesh, &error, attributes), error);
  REQUIRE(mesh.skin.size() == mesh.vertices.size());
  CHECK(mesh.skin_joint_count == 2);
  // The stream follows vertex_source exactly, which is what makes every copy of a surface point
  // — in another cluster, or on a coarser level — deform the same way.
  for (u32 v = 0; v < mesh.vertices.size(); ++v) {
    const SkinBinding& got = mesh.skin[v];
    const SkinBinding& want = source_skin[mesh.vertex_source[v]];
    for (u32 k = 0; k < 4; ++k) {
      CHECK(got.joints[k] == want.joints[k]);
      CHECK(got.weights[k] == want.weights[k]);
    }
  }
  CHECK_MESSAGE(validate_clusters(mesh, indices, ClusterBuildOptions{}, &error), error);

  // Validation refuses a stream that has lost the invariants the shader relies on.
  ClusterMesh broken = mesh;
  broken.skin[3].weights[0] = static_cast<u8>(broken.skin[3].weights[0] + 1);
  CHECK_FALSE(validate_clusters(broken, indices, ClusterBuildOptions{}, &error));
  CHECK(error.find("255") != std::string::npos);
  broken = mesh;
  broken.skin[3].joints[0] = 9;  // outside the two-joint palette
  broken.skin[3].weights[0] = 255;
  broken.skin[3].weights[1] = 0;
  broken.skin[3].weights[2] = 0;
  broken.skin[3].weights[3] = 0;
  CHECK_FALSE(validate_clusters(broken, indices, ClusterBuildOptions{}, &error));
  CHECK(error.find("palette") != std::string::npos);
  broken = mesh;
  broken.skin.resize(broken.skin.size() - 1);
  CHECK_FALSE(validate_clusters(broken, indices, ClusterBuildOptions{}, &error));

  // Unskinned stays unskinned: no stream, no palette, and nothing to validate.
  ClusterMesh plain;
  REQUIRE(build_clusters(positions, indices, ClusterBuildOptions{}, plain, &error));
  CHECK(plain.skin.empty());
  CHECK(plain.skin_joint_count == 0);
  CHECK(validate_clusters(plain, indices, ClusterBuildOptions{}, &error));

  // The weld key includes the binding. Unindex the grid, give every corner its source vertex's
  // binding, and it folds back to 81 vertices as it does without one; give one corner different
  // weights and that corner keeps a vertex of its own, because merging the two would hand one
  // surface the other's deformation.
  Vector<Vec3> normals;
  compute_vertex_normals(positions, indices, normals);
  Vector<Vec3> flat_positions;
  Vector<Vec3> flat_normals;
  Vector<Vec2> flat_uvs;
  Vector<SkinBinding> flat_skin;
  Vector<u32> flat_indices;
  for (const u32 i : indices) {
    flat_indices.push_back(flat_positions.size());
    flat_positions.push_back(positions[i]);
    flat_normals.push_back(normals[i]);
    flat_uvs.push_back(Vec2{positions[i].x, positions[i].y});
    flat_skin.push_back(source_skin[i]);
  }
  Vector<Vec3> welded_positions = flat_positions;
  Vector<Vec3> welded_normals = flat_normals;
  Vector<Vec2> welded_uvs = flat_uvs;
  Vector<SkinBinding> welded_skin = flat_skin;
  Vector<u32> welded_indices = flat_indices;
  CHECK(weld_vertices(welded_positions, welded_normals, welded_uvs, welded_indices, &welded_skin) ==
        81);
  CHECK(welded_skin.size() == 81);

  Vector<Vec3> split_positions = flat_positions;
  Vector<Vec3> split_normals = flat_normals;
  Vector<Vec2> split_uvs = flat_uvs;
  Vector<SkinBinding> split_skin = flat_skin;
  Vector<u32> split_indices = flat_indices;
  const u32 joints_other[4] = {1, 0, 0, 0};
  const f32 weights_other[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  split_skin[7] = make_skin_binding(joints_other, weights_other);
  CHECK(weld_vertices(split_positions, split_normals, split_uvs, split_indices, &split_skin) == 82);
  CHECK(split_skin.size() == 82);
}
