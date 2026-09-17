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

TEST_CASE("cluster lod: DAGs built per part merge into one mesh with the leaves first") {
  // Two terrains side by side, as two primitives of one mesh would be.
  Vector<Vec3> positions_a;
  Vector<u32> indices_a;
  make_terrain(33, 4.0f, positions_a, indices_a);
  Vector<Vec3> positions_b;
  Vector<u32> indices_b;
  make_terrain(17, 2.0f, positions_b, indices_b);
  for (Vec3& p : positions_b)
    p.x += 8.0f;
  ClusterLodMesh parts[2];
  std::string error;
  REQUIRE_MESSAGE(build_cluster_lod(positions_a, indices_a, ClusterLodOptions{}, parts[0], &error),
                  error);
  REQUIRE_MESSAGE(build_cluster_lod(positions_b, indices_b, ClusterLodOptions{}, parts[1], &error),
                  error);

  ClusterLodMesh merged;
  Vector<u32> part_of_cluster;
  REQUIRE_MESSAGE(merge_cluster_lod(parts, merged, &part_of_cluster, &error), error);
  const u32 total_clusters = parts[0].mesh.clusters.size() + parts[1].mesh.clusters.size();
  CHECK(merged.mesh.clusters.size() == total_clusters);
  CHECK(merged.lod.size() == total_clusters);
  CHECK(part_of_cluster.size() == total_clusters);
  CHECK(merged.mesh.vertices.size() ==
        parts[0].mesh.vertices.size() + parts[1].mesh.vertices.size());
  CHECK(merged.mesh.attributes.size() == merged.mesh.vertices.size());
  CHECK(merged.mesh.triangles.size() ==
        parts[0].mesh.triangles.size() + parts[1].mesh.triangles.size());
  CHECK(merged.leaf_triangle_count == 2 * 32 * 32 + 2 * 16 * 16);
  CHECK(merged.group_count == parts[0].group_count + parts[1].group_count);
  CHECK(merged.level_cluster_counts[0] ==
        parts[0].level_cluster_counts[0] + parts[1].level_cluster_counts[0]);

  // Leaves first, then the coarser levels; every cluster names its part and a valid group.
  for (u32 i = 0; i < total_clusters; ++i) {
    CHECK((merged.lod[i].level == 0) == (i < merged.level_cluster_counts[0]));
    CHECK(merged.lod[i].group < merged.group_count);
    CHECK(part_of_cluster[i] < 2);
    const ClusterDesc& c = merged.mesh.clusters[i];
    // Part 1 lives at x >= 6; part 0 within |x| <= 4.
    CHECK((c.center.x > 5.0f) == (part_of_cluster[i] == 1));
  }

  // The merged mesh validates against the concatenated, shifted source indices.
  Vector<u32> source_indices;
  source_indices.append(std::span<const u32>(indices_a.data(), indices_a.size()));
  for (const u32 index : indices_b)
    source_indices.push_back(index + static_cast<u32>(positions_a.size()));
  CHECK_MESSAGE(validate_cluster_lod(merged, source_indices, &error), error);

  // One grid over both parts, not one grid per part: the merged step spans the wider mesh, and
  // every merged vertex is still within half a step of its float position (validate checks the
  // whole stream, so this pins the shape of it).
  CHECK(merged.mesh.quantized.size() >= merged.mesh.vertices.size() * 3);
  CHECK(merged.mesh.quantized.size() % 2 == 0);
  CHECK(merged.mesh.quant_scale > parts[0].mesh.quant_scale);
  CHECK(merged.mesh.quant_scale > parts[1].mesh.quant_scale);
  CHECK(merged.mesh.quant_origin.x <= parts[0].mesh.quant_origin.x);
  f32 worst = 0.0f;
  for (u32 v = 0; v < merged.mesh.vertices.size(); ++v) {
    const Vec3 p = merged.mesh.vertices[v];
    const Vec3 q = dequantize_position(merged.mesh, v);
    const f32 axis[3] = {std::fabs(q.x - p.x), std::fabs(q.y - p.y), std::fabs(q.z - p.z)};
    for (const f32 e : axis)
      worst = e > worst ? e : worst;
  }
  CHECK(worst <= merged.mesh.quant_scale * 0.5f + 1e-6f);
  MESSAGE("merged grid: step " << merged.mesh.quant_scale << " over parts "
                               << parts[0].mesh.quant_scale << " and " << parts[1].mesh.quant_scale
                               << ", worst error " << worst);

  // A view-dependent cut of the merged mesh is the union of the parts' cuts.
  LodView view;
  view.camera = Vec3{4.0f, 6.0f, 12.0f};
  view.proj_scale = 1000.0f;
  view.threshold_px = 1.5f;
  Vector<u32> cut;
  select_lod(merged, view, cut);
  Vector<u32> cut_a;
  Vector<u32> cut_b;
  select_lod(parts[0], view, cut_a);
  select_lod(parts[1], view, cut_b);
  CHECK(cut.size() == cut_a.size() + cut_b.size());
  CHECK(triangles_of(merged, cut) == triangles_of(parts[0], cut_a) + triangles_of(parts[1], cut_b));

  // Empty input and an empty part are rejected.
  CHECK_FALSE(merge_cluster_lod({}, merged, nullptr, &error));
  ClusterLodMesh empty_parts[2] = {parts[0], ClusterLodMesh{}};
  CHECK_FALSE(merge_cluster_lod(empty_parts, merged, nullptr, &error));
  CHECK_FALSE(error.empty());
}
