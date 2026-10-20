// Authored coarser levels above the decimated chain (geometry.md, "Authored levels"): the cut, the
// switch distance, the hand-over between two parts, and the plain build untouched.

#include <domain/geometry/cluster_lod.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>

using namespace engine;
using namespace engine::geometry;

namespace {

// A heightfield over [-extent, extent]^2, appended to `positions` (the cluster LOD test's).
void append_terrain(u32 n, f32 extent, Vector<Vec3>& positions, Vector<u32>& indices) {
  const u32 base = positions.size();
  for (u32 z = 0; z < n; ++z) {
    for (u32 x = 0; x < n; ++x) {
      const f32 fx = -extent + 2.0f * extent * static_cast<f32>(x) / static_cast<f32>(n - 1);
      const f32 fz = -extent + 2.0f * extent * static_cast<f32>(z) / static_cast<f32>(n - 1);
      const f32 h = 0.9f * std::sin(fx * 0.55f) * std::cos(fz * 0.4f) +
                    0.35f * std::sin(fx * 1.7f + fz * 1.1f);
      positions.push_back(Vec3{fx, h, fz});
    }
  }
  for (u32 z = 0; z + 1 < n; ++z) {
    for (u32 x = 0; x + 1 < n; ++x) {
      const u32 a = base + z * n + x;
      for (const u32 i : {a, a + n, a + 1, a + 1, a + n, a + n + 1})
        indices.push_back(i);
    }
  }
}

u32 triangles_of(const ClusterLodMesh& mesh, const Vector<u32>& clusters) {
  u32 total = 0;
  for (const u32 c : clusters)
    total += mesh.mesh.clusters[c].triangle_count;
  return total;
}

// Where `error` projects to the view's threshold, measured from the sphere's centre.
f32 switch_distance(f32 error, Vec4 sphere, const LodView& view) {
  return error * view.proj_scale / view.threshold_px + sphere.w;
}

LodView view_1080p() {
  LodView view;
  view.znear = 0.1f;
  view.proj_scale = 1.0f / std::tan(radians(60.0f) * 0.5f) * 1080.0f * 0.5f;
  view.threshold_px = 1.0f;
  return view;
}

}  // namespace

TEST_CASE("authored levels: they sit above the decimated chain and switch whole") {
  Vector<Vec3> positions;
  Vector<u32> fine;
  append_terrain(65, 10.0f, positions, fine);  // 8,192 triangles
  Vector<u32> coarse;
  append_terrain(9, 10.0f, positions, coarse);  // 128 triangles
  Vector<u32> quad;
  append_terrain(2, 10.0f, positions, quad);  // 2 triangles
  const ClusterLodLevel levels[] = {{coarse, 0.5f}, {quad, 2.0f}};
  ClusterLodMesh lod;
  std::string error;
  REQUIRE_MESSAGE(
      build_cluster_lod_levels(positions, fine, levels, ClusterLodOptions{}, lod, &error), error);
  CHECK_MESSAGE(validate_cluster_lod(lod, fine, &error), error);
  ClusterLodMesh plain;
  REQUIRE(build_cluster_lod(positions, fine, ClusterLodOptions{}, plain, &error));
  CHECK(lod.leaf_triangle_count == 8192);

  // The decimated chain was cut where its error reached the first level's, and nothing that
  // survived draws past it.
  const u32 depth = lod.level_cluster_counts.size();
  REQUIRE(depth >= 3);
  CHECK(lod.level_cluster_counts[depth - 1] == 1);  // the quad
  u32 authored = 0;
  for (const ClusterLodDesc& d : lod.lod) {
    if (d.level + 2 >= depth) {
      ++authored;
      continue;
    }
    CHECK(d.own_error < 0.5f);
    CHECK(d.parent_error <= 0.5f);
  }
  CHECK(authored == lod.level_cluster_counts[depth - 2] + 1);
  const Vec4 sphere = authored_lod_sphere(lod, 0.5f);
  CHECK(sphere.w >= 10.0f * std::sqrt(2.0f));

  LodView view = view_1080p();
  const Vec3 centre{sphere.x, sphere.y, sphere.z};
  const auto cut_at = [&](const ClusterLodMesh& mesh, f32 distance, Vector<u32>& cut) {
    view.camera = centre + Vec3{0.0f, distance, 0.0f};
    cut.clear();
    return select_lod(mesh, view, cut);
  };
  // Nearer than the first level's switch the cut is the plain build's; one per cent past it, the
  // level whole; past the second's, the quad — and one per cent inside each, the finer form.
  const f32 first = switch_distance(0.5f, sphere, view);
  const f32 second = switch_distance(2.0f, sphere, view);
  MESSAGE("switches at " << first << " m and " << second << " m from the sphere's centre");
  Vector<u32> cut;
  Vector<u32> plain_cut;
  for (const f32 d : {20.0f, 80.0f, first * 0.99f}) {
    cut_at(lod, d, cut);
    cut_at(plain, d, plain_cut);
    CHECK(triangles_of(lod, cut) == triangles_of(plain, plain_cut));
    for (const u32 c : cut)
      CHECK(lod.lod[c].level + 2 < depth);
  }
  cut_at(lod, first * 1.01f, cut);
  CHECK(cut.size() == lod.level_cluster_counts[depth - 2]);
  CHECK(triangles_of(lod, cut) == 128);
  cut_at(lod, second * 0.99f, cut);
  CHECK(triangles_of(lod, cut) == 128);
  cut_at(lod, second * 1.01f, cut);
  CHECK(cut.size() == 1);
  CHECK(triangles_of(lod, cut) == 2);

  // No levels is the plain build, byte for byte.
  ClusterLodMesh none;
  REQUIRE(build_cluster_lod_levels(positions, fine, {}, ClusterLodOptions{}, none, &error));
  REQUIRE(none.lod.size() == plain.lod.size());
  CHECK(std::memcmp(none.lod.data(), plain.lod.data(),
                    plain.lod.size() * sizeof(ClusterLodDesc)) == 0);
  REQUIRE(none.mesh.quantized.size() == plain.mesh.quantized.size());
  CHECK(std::memcmp(none.mesh.quantized.data(), plain.mesh.quantized.data(),
                    plain.mesh.quantized.size() * sizeof(u16)) == 0);
  CHECK(none.mesh.triangles.size() == plain.mesh.triangles.size());
}

TEST_CASE("authored levels: an empty last level hands a part over to another's") {
  Vector<Vec3> positions;
  Vector<u32> first_part;
  append_terrain(33, 4.0f, positions, first_part);
  Vector<u32> second_part;
  const u32 second_base = positions.size();
  append_terrain(33, 4.0f, positions, second_part);
  for (u32 v = second_base; v < positions.size(); ++v)
    positions[v].x += 30.0f;
  Vector<u32> stand_in;
  append_terrain(2, 20.0f, positions, stand_in);

  // Part one draws nothing past 1.5; part two stands there for both.
  const ClusterLodLevel handover[] = {{{}, 1.5f}};
  const ClusterLodLevel standing[] = {{stand_in, 1.5f}};
  ClusterLodMesh a;
  ClusterLodMesh b;
  std::string error;
  REQUIRE_MESSAGE(
      build_cluster_lod_levels(positions, first_part, handover, ClusterLodOptions{}, a, &error),
      error);
  REQUIRE_MESSAGE(
      build_cluster_lod_levels(positions, second_part, standing, ClusterLodOptions{}, b, &error),
      error);
  for (const ClusterLodDesc& d : a.lod)
    CHECK(d.parent_error <= 1.5f);
  const Vec4 shared = authored_lod_sphere(b, 1.5f, authored_lod_sphere(a, 1.5f));
  CHECK_FALSE(widen_lod_spheres(a, 1.5f, Vec4{shared.x, shared.y, shared.z, 1.0f}, &error));
  REQUIRE_MESSAGE(widen_lod_spheres(a, 1.5f, shared, &error), error);
  REQUIRE_MESSAGE(widen_lod_spheres(b, 1.5f, shared, &error), error);

  // Merged, every view draws part one's chain and part two's, or the stand-in alone.
  const ClusterLodMesh parts[] = {a, b};
  ClusterLodMesh merged;
  Vector<u32> part_of;
  REQUIRE(merge_cluster_lod(parts, merged, &part_of, &error));
  LodView view = view_1080p();
  u32 far_views = 0;
  for (f32 d = 5.0f; d < 6000.0f; d *= 1.37f) {
    view.camera = Vec3{shared.x + d, shared.y, shared.z};
    Vector<u32> cut;
    select_lod(merged, view, cut);
    u32 stand = 0;
    u32 of_a = 0;
    for (const u32 c : cut) {
      if (merged.lod[c].own_error >= 1.5f)
        ++stand;
      else if (part_of[c] == 0)
        ++of_a;
    }
    const bool far = projected_error(shared, 1.5f, view) <= view.threshold_px;
    far_views += far ? 1u : 0u;
    CHECK(stand == (far ? 1u : 0u));
    CHECK((of_a == 0u) == far);
  }
  CHECK(far_views > 0);
}

TEST_CASE("authored levels: errors that do not rise are refused") {
  Vector<Vec3> positions;
  Vector<u32> fine;
  append_terrain(9, 1.0f, positions, fine);
  ClusterLodMesh lod;
  const ClusterLodLevel falling[] = {{fine, 0.5f}, {fine, 0.25f}};
  CHECK_FALSE(build_cluster_lod_levels(positions, fine, falling, ClusterLodOptions{}, lod));
  const ClusterLodLevel empty_first[] = {{{}, 0.5f}, {fine, 1.0f}};
  CHECK_FALSE(build_cluster_lod_levels(positions, fine, empty_first, ClusterLodOptions{}, lod));
}
