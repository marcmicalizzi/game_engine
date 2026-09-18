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

TEST_CASE("cluster lod: separate meshes merge into a scene, each keeping its own grid") {
  // A big terrain and a small one a long way off: one grid over both would spend the 16 bits on
  // the distance between them, which is exactly what merge_cluster_meshes must not do.
  Vector<Vec3> positions_a;
  Vector<u32> indices_a;
  make_terrain(33, 20.0f, positions_a, indices_a);
  Vector<Vec3> positions_b;
  Vector<u32> indices_b;
  make_terrain(17, 0.02f, positions_b, indices_b);
  ClusterLodMesh meshes[2];
  std::string error;
  REQUIRE_MESSAGE(build_cluster_lod(positions_a, indices_a, ClusterLodOptions{}, meshes[0], &error),
                  error);
  REQUIRE_MESSAGE(build_cluster_lod(positions_b, indices_b, ClusterLodOptions{}, meshes[1], &error),
                  error);

  ClusterLodMesh scene;
  Vector<ClusterMeshPart> parts;
  REQUIRE_MESSAGE(merge_cluster_meshes(meshes, scene, parts, &error), error);
  REQUIRE(parts.size() == 2);
  const u32 total_clusters = meshes[0].mesh.clusters.size() + meshes[1].mesh.clusters.size();
  CHECK(scene.mesh.clusters.size() == total_clusters);
  CHECK(scene.lod.size() == total_clusters);
  CHECK(scene.mesh.vertices.size() ==
        meshes[0].mesh.vertices.size() + meshes[1].mesh.vertices.size());
  CHECK(scene.mesh.attributes.size() == scene.mesh.vertices.size());
  CHECK(scene.leaf_triangle_count == 2 * 32 * 32 + 2 * 16 * 16);
  CHECK(scene.group_count == meshes[0].group_count + meshes[1].group_count);

  // Each mesh keeps its own grid, so the small one is not quantized on the big one's step.
  for (u32 m = 0; m < 2; ++m) {
    CHECK(parts[m].quant_scale == meshes[m].mesh.quant_scale);
    CHECK(parts[m].quant_origin == meshes[m].mesh.quant_origin);
    CHECK(parts[m].cluster_count == meshes[m].mesh.clusters.size());
    CHECK(parts[m].leaf_cluster_count == meshes[m].level_cluster_counts[0]);
    CHECK(parts[m].first_vertex == (m == 0 ? 0u : meshes[0].mesh.vertices.size()));
  }
  CHECK(parts[0].first_cluster == 0);
  CHECK(parts[1].first_cluster == parts[0].cluster_count);
  CHECK(parts[1].quant_scale * 100.0f < parts[0].quant_scale);
  MESSAGE("grids: " << parts[0].quant_scale << " and " << parts[1].quant_scale);

  // Every mesh's clusters are contiguous, its own leaves first, and its offsets shifted.
  for (u32 m = 0; m < 2; ++m) {
    const ClusterMeshPart& part = parts[m];
    for (u32 i = 0; i < part.cluster_count; ++i) {
      const u32 index = part.first_cluster + i;
      CHECK((scene.lod[index].level == 0) == (i < part.leaf_cluster_count));
      const ClusterDesc& c = scene.mesh.clusters[index];
      CHECK(c.vertex_offset >= part.first_vertex);
      CHECK(c.vertex_offset + c.vertex_count <= part.first_vertex + meshes[m].mesh.vertices.size());
    }
  }

  // Three u16 a vertex at the scene-wide vertex index, padded to an even count, and every vertex
  // within half a step of its own mesh's grid.
  CHECK(scene.mesh.quantized.size() >= scene.mesh.vertices.size() * 3);
  CHECK(scene.mesh.quantized.size() % 2 == 0);
  f32 worst_steps = 0.0f;
  for (u32 m = 0; m < 2; ++m) {
    const ClusterMeshPart& part = parts[m];
    for (u32 v = 0; v < meshes[m].mesh.vertices.size(); ++v) {
      const u32 index = part.first_vertex + v;
      const Vec3 p = scene.mesh.vertices[index];
      const Vec3 q =
          part.quant_origin + Vec3{static_cast<f32>(scene.mesh.quantized[index * 3 + 0]),
                                   static_cast<f32>(scene.mesh.quantized[index * 3 + 1]),
                                   static_cast<f32>(scene.mesh.quantized[index * 3 + 2])} *
                                  part.quant_scale;
      const f32 axis[3] = {std::fabs(q.x - p.x), std::fabs(q.y - p.y), std::fabs(q.z - p.z)};
      for (const f32 e : axis)
        worst_steps = std::fmax(worst_steps, e / part.quant_scale);
    }
  }
  CHECK(worst_steps <= 0.5f + 1e-3f);
  MESSAGE("worst quantization error over both meshes: " << worst_steps << " of a grid step");

  // The first mesh's grid is the merged mesh's, so a one-mesh merge behaves as before.
  CHECK(scene.mesh.quant_scale == meshes[0].mesh.quant_scale);
  ClusterLodMesh single;
  Vector<ClusterMeshPart> single_parts;
  REQUIRE(merge_cluster_meshes(std::span<const ClusterLodMesh>(&meshes[1], 1), single, single_parts,
                               &error));
  CHECK(single.mesh.quant_scale == meshes[1].mesh.quant_scale);
  CHECK(single_parts.size() == 1);

  // Empty input and an empty mesh are rejected.
  CHECK_FALSE(merge_cluster_meshes({}, scene, parts, &error));
  ClusterLodMesh empty_meshes[2] = {meshes[0], ClusterLodMesh{}};
  CHECK_FALSE(merge_cluster_meshes(empty_meshes, scene, parts, &error));
  CHECK_FALSE(error.empty());
}

TEST_CASE("cluster lod: skin bindings reach every level and survive both merges") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(33, 10.0f, positions, indices);
  // Three joints along +x, so a binding is a function of position alone and two vertices at one
  // place always agree — the property the crack rule asks of a deformer's inputs.
  Vector<SkinBinding> skin;
  for (const Vec3& p : positions) {
    const f32 t = (p.x + 10.0f) / 20.0f * 2.0f;  // 0 .. 2 across the terrain
    const u32 lower = t < 1.0f ? 0u : 1u;
    const u32 joints[4] = {lower, lower + 1, 0, 0};
    const f32 fraction = t - static_cast<f32>(lower);
    const f32 weights[4] = {1.0f - fraction, fraction, 0.0f, 0.0f};
    skin.push_back(make_skin_binding(joints, weights));
  }
  AttributeSource attributes;
  attributes.skin = std::span<const SkinBinding>(skin.data(), skin.size());
  attributes.joint_count = 3;

  ClusterLodMesh mesh;
  std::string error;
  REQUIRE_MESSAGE(
      build_cluster_lod(positions, indices, ClusterLodOptions{}, mesh, &error, attributes), error);
  REQUIRE(mesh.mesh.skin.size() == mesh.mesh.vertices.size());
  CHECK(mesh.mesh.skin_joint_count == 3);
  CHECK_MESSAGE(validate_cluster_lod(mesh, indices, &error), error);
  // Every level's vertices are original vertices (clusterlod keeps them), so a coarse cluster's
  // binding is its source vertex's binding and not something a simplifier invented. That is why
  // skinning does not tear at a LOD-group boundary the way displacement along a normal does.
  u32 coarse_checked = 0;
  for (u32 c = 0; c < mesh.mesh.clusters.size(); ++c) {
    if (mesh.lod[c].level == 0) continue;
    const ClusterDesc& desc = mesh.mesh.clusters[c];
    for (u32 v = 0; v < desc.vertex_count; ++v) {
      const u32 at = desc.vertex_offset + v;
      const SkinBinding& want = skin[mesh.mesh.vertex_source[at]];
      for (u32 k = 0; k < 4; ++k) {
        CHECK(mesh.mesh.skin[at].joints[k] == want.joints[k]);
        CHECK(mesh.mesh.skin[at].weights[k] == want.weights[k]);
      }
      ++coarse_checked;
    }
  }
  CHECK(coarse_checked > 0);

  // Both merges carry the stream: the parts of one mesh (merge_cluster_lod) and the meshes of
  // one scene (merge_cluster_meshes), whose palette width is the widest of them.
  const ClusterLodMesh skinned_parts[2] = {mesh, mesh};
  ClusterLodMesh merged_parts;
  REQUIRE(merge_cluster_lod(std::span<const ClusterLodMesh>(skinned_parts, 2), merged_parts,
                            nullptr, &error));
  CHECK(merged_parts.mesh.skin.size() == merged_parts.mesh.vertices.size());
  CHECK(merged_parts.mesh.skin_joint_count == 3);

  ClusterLodMesh skinned_scene;
  Vector<ClusterMeshPart> skinned_scene_parts;
  REQUIRE(merge_cluster_meshes(std::span<const ClusterLodMesh>(skinned_parts, 2), skinned_scene,
                               skinned_scene_parts, &error));
  REQUIRE(skinned_scene.mesh.skin.size() == skinned_scene.mesh.vertices.size());
  CHECK(skinned_scene.mesh.skin_joint_count == 3);
  for (u32 v = 0; v < mesh.mesh.skin.size(); ++v) {
    CHECK(skinned_scene.mesh.skin[v].joints[0] == mesh.mesh.skin[v].joints[0]);
    CHECK(skinned_scene.mesh.skin[skinned_scene_parts[1].first_vertex + v].weights[0] ==
          mesh.mesh.skin[v].weights[0]);
  }

  // An unskinned build of the same terrain carries no stream at all: the eight bytes a vertex are
  // paid by skinned meshes only.
  ClusterLodMesh plain;
  REQUIRE(build_cluster_lod(positions, indices, ClusterLodOptions{}, plain, &error));
  CHECK(plain.mesh.skin.empty());
  CHECK(plain.mesh.skin_joint_count == 0);
}
