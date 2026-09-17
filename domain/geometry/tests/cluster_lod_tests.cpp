#include <domain/geometry/cluster_lod.h>

#include <doctest/doctest.h>

#include <cmath>
#include <string>

using namespace engine;
using namespace engine::geometry;

namespace {

// A heightfield over [-extent, extent]^2 so simplification has real error to measure.
void make_terrain(u32 n, f32 extent, Vector<Vec3>& positions, Vector<u32>& indices) {
  positions.clear();
  indices.clear();
  for (u32 z = 0; z < n; ++z) {
    for (u32 x = 0; x < n; ++x) {
      const f32 fx = -extent + 2.0f * extent * static_cast<f32>(x) / (n - 1);
      const f32 fz = -extent + 2.0f * extent * static_cast<f32>(z) / (n - 1);
      const f32 h = 0.9f * std::sin(fx * 0.55f) * std::cos(fz * 0.4f) +
                    0.35f * std::sin(fx * 1.7f + fz * 1.1f);
      positions.push_back(Vec3{fx, h, fz});
    }
  }
  for (u32 z = 0; z + 1 < n; ++z) {
    for (u32 x = 0; x + 1 < n; ++x) {
      const u32 a = z * n + x;
      indices.push_back(a);
      indices.push_back(a + n);
      indices.push_back(a + 1);
      indices.push_back(a + 1);
      indices.push_back(a + n);
      indices.push_back(a + n + 1);
    }
  }
}

u32 triangles_of(const ClusterLodMesh& mesh, const Vector<u32>& clusters) {
  u32 total = 0;
  for (const u32 c : clusters)
    total += mesh.mesh.clusters[c].triangle_count;
  return total;
}

}  // namespace

TEST_CASE("cluster lod: a terrain builds a multi-level DAG that validates") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(129, 10.0f, positions, indices);  // 32,768 triangles
  ClusterLodMesh lod;
  std::string error;
  REQUIRE_MESSAGE(build_cluster_lod(positions, indices, ClusterLodOptions{}, lod, &error), error);
  CHECK(lod.leaf_triangle_count == 32768);
  CHECK(lod.level_cluster_counts.size() >= 4);
  CHECK(lod.level_cluster_counts[0] >= 32768 / 124);
  CHECK(lod.group_count > lod.level_cluster_counts.size());
  CHECK(lod.mesh.clusters.size() == lod.lod.size());
  // Each level is coarser than the one before it.
  for (u32 level = 1; level < lod.level_cluster_counts.size(); ++level) {
    CHECK(lod.level_cluster_counts[level] < lod.level_cluster_counts[level - 1]);
  }
  CHECK_MESSAGE(validate_cluster_lod(lod, indices, &error), error);
  std::string summary = "levels:";
  for (const u32 count : lod.level_cluster_counts)
    summary += " " + std::to_string(count);
  MESSAGE(summary << " (" << lod.mesh.clusters.size() << " clusters, " << lod.group_count
                  << " groups)");

  // Raw cuts: threshold 0 is the leaves (no group simplified with zero error on this terrain),
  // a huge threshold is the coarsest level.
  Vector<u32> cut;
  REQUIRE(select_lod_raw(lod, 0.0f, cut) == lod.level_cluster_counts[0]);
  CHECK(triangles_of(lod, cut) == 32768);
  cut.clear();
  const u32 coarsest = select_lod_raw(lod, 1e30f, cut);
  CHECK(coarsest >= 1);
  CHECK(coarsest <= lod.level_cluster_counts.back() +
                        lod.level_cluster_counts[lod.level_cluster_counts.size() - 2]);
  CHECK(triangles_of(lod, cut) < 32768 / 8);
}

TEST_CASE("cluster lod: view-dependent selection coarsens with distance") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(65, 10.0f, positions, indices);  // 8,192 triangles
  ClusterLodMesh lod;
  std::string error;
  REQUIRE(build_cluster_lod(positions, indices, ClusterLodOptions{}, lod, &error));
  REQUIRE(validate_cluster_lod(lod, indices, &error));

  LodView view;
  view.znear = 0.1f;
  view.proj_scale = 1.0f / std::tan(radians(60.0f) * 0.5f) * 1080.0f * 0.5f;  // 1080p, 60 degrees
  view.threshold_px = 1.0f;
  u32 previous = ~u32{0};
  u32 previous_triangles = ~u32{0};
  for (const f32 distance : {5.0f, 15.0f, 40.0f, 120.0f, 400.0f}) {
    view.camera = Vec3{0.0f, distance, 0.0f};
    Vector<u32> cut;
    const u32 count = select_lod(lod, view, cut);
    REQUIRE(count > 0);
    const u32 triangles = triangles_of(lod, cut);
    CHECK(count <= previous);
    CHECK(triangles <= previous_triangles);
    // Every selected cluster passes the reference test and none of its DAG neighbours' both.
    for (const u32 c : cut)
      CHECK(lod_selects(lod.lod[c], view));
    MESSAGE("distance " << distance << ": " << count << " clusters, " << triangles << " triangles");
    previous = count;
    previous_triangles = triangles;
  }
  // Very close: full detail. Very far: far fewer triangles than the source.
  view.camera = Vec3{0.0f, 5.0f, 0.0f};
  Vector<u32> close_cut;
  select_lod(lod, view, close_cut);
  CHECK(triangles_of(lod, close_cut) == 8192);
  view.camera = Vec3{0.0f, 400.0f, 0.0f};
  Vector<u32> far_cut;
  select_lod(lod, view, far_cut);
  CHECK(triangles_of(lod, far_cut) < 8192 / 4);

  // The projected error formula: a terminal error is infinite; error shrinks with distance.
  const Vec4 sphere{0.0f, 0.0f, 0.0f, 1.0f};
  CHECK(projected_error(sphere, k_lod_terminal_error, view) == k_lod_terminal_error);
  view.camera = Vec3{0.0f, 11.0f, 0.0f};
  const f32 near_error = projected_error(sphere, 0.01f, view);
  view.camera = Vec3{0.0f, 101.0f, 0.0f};
  const f32 far_error = projected_error(sphere, 0.01f, view);
  CHECK(near_error > far_error * 9.0f);
  CHECK(near_error < far_error * 11.0f);
}

TEST_CASE("cluster lod: options and bad input") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(17, 4.0f, positions, indices);
  ClusterLodMesh lod;
  std::string error;
  ClusterLodOptions rt;
  rt.ray_tracing = true;
  rt.max_triangles = 64;
  rt.max_vertices = 128;
  REQUIRE_MESSAGE(build_cluster_lod(positions, indices, rt, lod, &error), error);
  CHECK(validate_cluster_lod(lod, indices, &error));
  for (const ClusterDesc& c : lod.mesh.clusters) {
    CHECK(c.triangle_count <= 64);
    CHECK(c.vertex_count <= 128);
  }
  CHECK_FALSE(build_cluster_lod({}, indices, ClusterLodOptions{}, lod, &error));
  ClusterLodOptions bad;
  bad.max_triangles = 300;
  CHECK_FALSE(build_cluster_lod(positions, indices, bad, lod, &error));
  const u32 out_of_range[3] = {0, 1, 100000};
  CHECK_FALSE(build_cluster_lod(positions, out_of_range, ClusterLodOptions{}, lod, &error));
  CHECK(error.find("out of range") != std::string::npos);
}
