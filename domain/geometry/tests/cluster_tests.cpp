#include <domain/geometry/cluster.h>

#include <doctest/doctest.h>

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
