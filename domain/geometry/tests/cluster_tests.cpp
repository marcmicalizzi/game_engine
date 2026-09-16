#include <domain/geometry/cluster.h>

#include <doctest/doctest.h>

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
