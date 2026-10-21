// Authored coarser levels above the decimated chain (geometry.md, "Authored levels"): the cut, the
// switch distance, the hand-over between two parts, and the plain build untouched.

#include <domain/geometry/cluster_lod.h>

#include <doctest/doctest.h>

#include <algorithm>
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
  CHECK(std::memcmp(none.lod.data(), plain.lod.data(), plain.lod.size() * sizeof(ClusterLodDesc)) ==
        0);
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

namespace {

// A nested fixture (`build_cluster_lod_hierarchy`): a 16 x 16 field of separate small "leaf" quads,
// each in the card of its 4 x 4 block (16 first-level cards), each card in the card of its 2 x 2
// block of cards (4 second-level cards), and those in one top card. Every vertex knows what it
// stands for, so a cut can be counted leaf by leaf.
struct Nest {
  Vector<Vec3> positions;
  Vector<u32> finest;
  Vector<u32> finest_element;
  Vector<u32> indices[3];
  Vector<u32> element_of_triangle[3];
  Vector<u32> parent_of_element[3];
  Vector<i32> leaf_of_vertex;   // -1 for a card's vertex
  Vector<i32> level_of_vertex;  // 0 a leaf, k a level-k card
  Vector<u32> element_of_vertex;
  u32 elements[3] = {16, 4, 1};

  void quad(f32 x0, f32 z0, f32 size, f32 y, Vector<u32>& out, i32 leaf, i32 level, u32 element) {
    const u32 base = positions.size();
    positions.push_back(Vec3{x0, y, z0});
    positions.push_back(Vec3{x0 + size, y, z0});
    positions.push_back(Vec3{x0 + size, y, z0 + size});
    positions.push_back(Vec3{x0, y, z0 + size});
    for (u32 v = 0; v < 4; ++v) {
      leaf_of_vertex.push_back(leaf);
      level_of_vertex.push_back(level);
      element_of_vertex.push_back(element);
    }
    for (const u32 i : {base, base + 2, base + 1, base, base + 3, base + 2})
      out.push_back(i);
  }

  Nest() {
    for (u32 j = 0; j < 16; ++j)
      for (u32 i = 0; i < 16; ++i) {
        quad(static_cast<f32>(i) + 0.1f, static_cast<f32>(j) + 0.1f, 0.8f, 0.0f, finest,
             static_cast<i32>(j * 16 + i), 0, 0);
        const u32 card = (j / 4) * 4 + i / 4;
        finest_element.push_back(card);
        finest_element.push_back(card);
      }
    for (u32 b = 0; b < 16; ++b) {
      quad(static_cast<f32>((b % 4) * 4), static_cast<f32>((b / 4) * 4), 4.0f, 0.05f, indices[0],
           -1, 1, b);
      element_of_triangle[0].push_back(b);
      element_of_triangle[0].push_back(b);
      parent_of_element[0].push_back(((b / 4) / 2) * 2 + (b % 4) / 2);
    }
    for (u32 b = 0; b < 4; ++b) {
      quad(static_cast<f32>((b % 2) * 8), static_cast<f32>((b / 2) * 8), 8.0f, 0.1f, indices[1], -1,
           2, b);
      element_of_triangle[1].push_back(b);
      element_of_triangle[1].push_back(b);
      parent_of_element[1].push_back(0u);
    }
    quad(0.0f, 0.0f, 16.0f, 0.15f, indices[2], -1, 3, 0);
    element_of_triangle[2].push_back(0u);
    element_of_triangle[2].push_back(0u);
  }

  // The leaves a cluster stands for, by its first vertex.
  void leaves_of(const ClusterLodMesh& lod, u32 cluster, Vector<u32>& out) const {
    out.clear();
    const ClusterDesc& c = lod.mesh.clusters[cluster];
    for (u32 v = c.vertex_offset; v < c.vertex_offset + c.vertex_count; ++v) {
      const u32 source = lod.mesh.vertex_source[v];
      const i32 level = level_of_vertex[source];
      const u32 e = element_of_vertex[source];
      for (u32 leaf = 0; leaf < 256; ++leaf) {
        const u32 l1 = finest_element[leaf * 2];
        const u32 l2 = parent_of_element[0][l1];
        const bool under = level == 0   ? leaf_of_vertex[source] == static_cast<i32>(leaf)
                           : level == 1 ? l1 == e
                           : level == 2 ? l2 == e
                                        : true;
        if (under) out.push_back(leaf);
      }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
  }
};

}  // namespace

TEST_CASE(
    "authored hierarchy: one sphere a sub-tree, one cluster on every path, the fade tiles it") {
  const Nest nest;
  const f32 errors[3] = {0.4f, 1.2f, 3.0f};
  ClusterLodHierarchyLevel levels[3];
  for (u32 k = 0; k < 3; ++k) {
    levels[k].indices = nest.indices[k];
    levels[k].element_of_triangle = nest.element_of_triangle[k];
    levels[k].parent_of_element = nest.parent_of_element[k];
    levels[k].elements = nest.elements[k];
    levels[k].error = errors[k];
  }
  ClusterLodOptions options;
  options.normal_cones = false;
  options.max_vertices = 16;  // a group is four cards, so the field has groups of its own
  ClusterLodMesh lod;
  std::string error;
  REQUIRE_MESSAGE(build_cluster_lod_hierarchy(nest.positions, nest.finest, nest.finest_element,
                                              levels, options, lod, &error),
                  error);
  REQUIRE_MESSAGE(validate_cluster_lod(lod, nest.finest, &error), error);
  REQUIRE(lod.level_cluster_counts.size() == 4u);
  // The first level's groups are their own sub-trees' size, not the field's: per-card spheres.
  f32 largest_first = 0.0f;
  for (const ClusterLodDesc& d : lod.lod)
    if (d.level == 1) largest_first = d.own.w > largest_first ? d.own.w : largest_first;
  CHECK(largest_first < 8.0f);

  // Coverage words: the leaves' parent the first level's, each level its own and the next's.
  const f32 cover[3] = {0.05f, 0.2f, 0.6f};
  Vector<u32> coverage;
  for (const ClusterLodDesc& d : lod.lod) {
    const u32 k = d.level;
    coverage.push_back(k == 0 ? pack_lod_coverage(0.0f, cover[0])
                              : pack_lod_coverage(cover[k - 1], k < 3 ? cover[k] : 1.0e30f));
  }
  CHECK(lod_coverage_parent(k_lod_no_coverage) > 1.0e30f);

  // Every leaf is drawn exactly once by every cut, with and without coverage, and with the fade the
  // drawn clusters' splits tile [0, 1) for every leaf.
  LodView view = view_1080p();
  Vector<u32> leaves;
  u32 views = 0;
  bool mixed = false;
  for (const f32 foliage : {0.0f, 2.0f, 6.0f}) {
    for (const f32 fade : {0.0f, 0.1f}) {
      for (u32 step = 0; step < 24; ++step) {
        view.foliage_px = foliage;
        view.fade = fade;
        view.camera = Vec3{-2.0f - 30.0f * static_cast<f32>(step), 3.0f, 1.0f};
        f32 kept[256] = {};
        u32 drawn[256] = {};
        bool any_leaf = false, any_card = false;
        for (u32 c = 0; c < lod.lod.size(); ++c) {
          const ClusterLodDesc& d = lod.lod[c];
          const f32 low = lod_fade_split(d.own, d.own_error, lod_coverage_own(coverage[c]), view);
          const f32 high =
              lod_fade_split(d.parent, d.parent_error, lod_coverage_parent(coverage[c]), view);
          const bool selected = lod_selects(d, view, coverage[c]);
          if (fade == 0.0f) CHECK(selected == (low < high));
          if (!(low < high)) continue;
          nest.leaves_of(lod, c, leaves);
          for (const u32 leaf : leaves) {
            kept[leaf] += high - low;
            ++drawn[leaf];
          }
          if (d.level == 0) any_leaf = true;
          if (d.level == 1) any_card = true;
        }
        for (u32 leaf = 0; leaf < 256; ++leaf) {
          if (fade == 0.0f) CHECK(drawn[leaf] == 1u);
          CHECK(std::fabs(kept[leaf] - 1.0f) < 1e-5f);
        }
        mixed = mixed || (any_leaf && any_card);
        ++views;
      }
    }
  }
  CHECK(views == 144u);
  CHECK(mixed);  // some view draws leaves near and cards far
}
