// Canonical vertex ids through the geometry half of the content build (docs/subsystems/
// geometry.md, "Canonical vertex identity"): the derivation rule for a mesh nobody named, the weld
// that must not merge two points of the base the author named apart, the builders that carry an id
// to every cluster copy of its vertex, the LOD builder that must not merge them back, and the two
// merges and the page layout that move the stream with its vertices. The glTF attribute and the
// Khronos samples are domain/assets' (tests/vertex_id_tests.cpp there); the container is
// cluster_file_tests.cpp.

#include <domain/geometry/cluster.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/cluster_pages.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace engine;
using namespace engine::geometry;

namespace {

// An n x n grid on the unit square in the z = 0 plane, facing +z, with UVs that are its positions.
void make_grid(u32 n, Vector<Vec3>& positions, Vector<u32>& indices) {
  positions.clear();
  indices.clear();
  for (u32 y = 0; y < n; ++y) {
    for (u32 x = 0; x < n; ++x)
      positions.push_back(Vec3{static_cast<f32>(x) / (n - 1), static_cast<f32>(y) / (n - 1), 0.0f});
  }
  for (u32 y = 0; y + 1 < n; ++y) {
    for (u32 x = 0; x + 1 < n; ++x) {
      const u32 a = y * n + x;
      indices.push_back(a);
      indices.push_back(a + 1);
      indices.push_back(a + n);
      indices.push_back(a + 1);
      indices.push_back(a + n + 1);
      indices.push_back(a + n);
    }
  }
}

// The same grid with its middle column **split** over rows [first_row, last_row]: every vertex of
// the column there exists twice, once for the triangles on its left and once for those on its
// right, at bit-identical positions with the same normal and the same UV. Nothing a renderer can
// see tells the two apart — only the ids do, which is the case authored ids exist for: two points
// of the base that happen to touch. Split over every row it is a clean seam from border to border
// (a module's boundary loop against the body's); split over the middle rows it is a **slit** that
// ends inside the surface at both ends, which is what closed lips or eyelids are. Every vertex
// takes its grid index as its id, except the right copies, which take `k_right_base` + it, so a
// triangle that uses one side's copy with a corner on the other side is one that crossed. The
// surface has a gentle height field so the simplifier has something to trade.
constexpr u32 k_right_base = 1u << 20;

struct SplitGrid {
  Vector<Vec3> positions;
  Vector<Vec3> normals;
  Vector<Vec2> uvs;
  Vector<u32> ids;
  Vector<u32> indices;
  u32 n = 0;
  u32 mid = 0;
  u32 first_row = 0;
  u32 last_row = 0;
  u32 seam_pairs = 0;
};

SplitGrid make_split_grid(u32 n, u32 first_row, u32 last_row, bool flat = true) {
  SplitGrid g;
  g.n = n;
  g.mid = (n - 1) / 2;
  g.first_row = first_row;
  g.last_row = last_row;
  Vector<u32> main_copy(n * n, ~0u);
  Vector<u32> right_copy(n * n, ~0u);
  auto add = [&](u32 x, u32 y, u32 id) {
    const f32 fx = static_cast<f32>(x) / (n - 1);
    const f32 fy = static_cast<f32>(y) / (n - 1);
    const f32 h = flat ? 0.0f : 0.1f * std::sin(fx * 7.0f) * std::cos(fy * 5.0f);
    g.positions.push_back(Vec3{fx, fy, h});
    g.normals.push_back(Vec3{0.0f, 0.0f, 1.0f});
    g.uvs.push_back(Vec2{fx, fy});
    g.ids.push_back(id);
    return g.positions.size() - 1;
  };
  for (u32 y = 0; y < n; ++y) {
    for (u32 x = 0; x < n; ++x) {
      main_copy[y * n + x] = add(x, y, y * n + x);
      if (x == g.mid && y >= first_row && y <= last_row) {
        right_copy[y * n + x] = add(x, y, k_right_base + y * n + x);
        ++g.seam_pairs;
      }
    }
  }
  for (u32 y = 0; y + 1 < n; ++y) {
    for (u32 x = 0; x + 1 < n; ++x) {
      // A quad right of the column takes the column's right copies where there are any.
      auto at = [&](u32 xx, u32 yy) {
        const u32 i = yy * n + xx;
        return x >= g.mid && right_copy[i] != ~0u ? right_copy[i] : main_copy[i];
      };
      const u32 tri[6] = {at(x, y),     at(x + 1, y),     at(x, y + 1),
                          at(x + 1, y), at(x + 1, y + 1), at(x, y + 1)};
      for (const u32 v : tri)
        g.indices.push_back(v);
    }
  }
  return g;
}

AttributeSource source_of(const SplitGrid& g, VertexIdSource source) {
  AttributeSource out;
  out.normals = std::span<const Vec3>(g.normals.data(), g.normals.size());
  out.uvs = std::span<const Vec2>(g.uvs.data(), g.uvs.size());
  out.vertex_ids = std::span<const u32>(g.ids.data(), g.ids.size());
  out.vertex_id_source = source;
  return out;
}

// Triangles, over every level of a DAG, that use one side's copy of a split vertex together with a
// corner on the other side of the column: the two points the ids named apart were merged by a
// collapse, so that side's surface now hangs off the other's vertex.
u32 crossing_triangles(const ClusterLodMesh& lod, const SplitGrid& g) {
  const ClusterMesh& mesh = lod.mesh;
  const f32 column = static_cast<f32>(g.mid) / (g.n - 1);
  u32 crossing = 0;
  for (const ClusterDesc& cluster : mesh.clusters) {
    for (u32 t = 0; t < cluster.triangle_count; ++t) {
      const u32 packed = mesh.triangles[cluster.triangle_offset + t];
      bool right_copy = false;
      bool left_copy = false;
      bool left_of = false;
      bool right_of = false;
      for (u32 k = 0; k < 3; ++k) {
        const u32 v = cluster.vertex_offset + ClusterMesh::unpack(packed, k);
        const u32 id = mesh.vertex_ids[v];
        const f32 x = mesh.vertices[v].x;
        const u32 row = (id % k_right_base) / g.n;
        const bool split = x == column && row >= g.first_row && row <= g.last_row;
        right_copy = right_copy || id >= k_right_base;
        left_copy = left_copy || (split && id < k_right_base);
        left_of = left_of || x < column;
        right_of = right_of || x > column;
      }
      crossing += ((right_copy && left_of) || (left_copy && right_of)) ? 1u : 0u;
    }
  }
  return crossing;
}

ClusterLodMesh build_with(const SplitGrid& g, SeamRule id_seams) {
  ClusterLodOptions options;
  options.id_seams = id_seams;
  ClusterLodMesh lod;
  std::string error;
  REQUIRE_MESSAGE(build_cluster_lod(g.positions, g.indices, options, lod, &error,
                                    source_of(g, VertexIdSource::authored)),
                  error);
  return lod;
}

// Every cluster vertex carries its own source vertex's id: the identity contract at this layer.
u32 misplaced_ids(const ClusterMesh& mesh, std::span<const u32> source_ids) {
  u32 wrong = 0;
  for (u32 v = 0; v < mesh.vertices.size(); ++v) {
    const u32 source = mesh.vertex_source[v];
    if (source >= source_ids.size() || mesh.vertex_ids[v] != source_ids[source]) ++wrong;
  }
  return wrong;
}

}  // namespace

TEST_CASE("vertex ids: a position weld names each distinct position by its rank") {
  // Two positions twice over, a -0 against a +0, and three more: the ids are the ranks of the
  // distinct positions in x-then-y-then-z order, numerically, with the two zeros one position.
  const Vec3 positions[] = {
      Vec3{1.0f, 0.0f, 0.0f},   Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f},
      Vec3{-0.0f, 0.0f, -0.0f}, Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f},
      Vec3{-2.0f, 5.0f, 5.0f},
  };
  Vector<u32> ids;
  CHECK(position_weld_ids(positions, ids) == 5);
  REQUIRE(ids.size() == 7);
  CHECK(ids[6] == 0);  // (-2, 5, 5): the smallest x
  CHECK(ids[5] == 1);  // (0, 0, -1)
  CHECK(ids[1] == 2);  // (0, 0, 0)
  CHECK(ids[3] == 2);  // (-0, 0, -0): the same position
  CHECK(ids[4] == 3);  // (0, 1, 0)
  CHECK(ids[0] == 4);  // (1, 0, 0)
  CHECK(ids[2] == 4);

  // The ids are a function of the **set** of positions: the same grid with its vertices in
  // another order names every position the same way, which is what lets a sidecar written against
  // one export of a base survive a re-export that only reordered it.
  Vector<Vec3> grid;
  Vector<u32> indices;
  make_grid(17, grid, indices);
  Vector<u32> forward;
  CHECK(position_weld_ids(grid, forward) == 17 * 17);
  Vector<Vec3> shuffled;
  Vector<u32> order;
  for (u32 i = 0; i < grid.size(); ++i)
    order.push_back((i * 97u + 13u) % grid.size());  // 97 is coprime with 289: a permutation
  for (const u32 i : order)
    shuffled.push_back(grid[i]);
  Vector<u32> backward;
  CHECK(position_weld_ids(shuffled, backward) == 17 * 17);
  u32 moved = 0;
  for (u32 k = 0; k < order.size(); ++k)
    moved += backward[k] != forward[order[k]] ? 1u : 0u;
  CHECK(moved == 0);
  // And it is dense: every id from 0 to the count minus one is used exactly once on the grid.
  Vector<u32> sorted = forward;
  std::sort(sorted.begin(), sorted.end());
  for (u32 i = 0; i < sorted.size(); ++i)
    CHECK(sorted[i] == i);

  Vector<u32> none;
  CHECK(position_weld_ids(std::span<const Vec3>(), none) == 0);
  CHECK(none.empty());
  CHECK(std::string(vertex_id_source_name(VertexIdSource::authored)) == "authored");
  CHECK(std::string(vertex_id_source_name(VertexIdSource::position_weld)) == "position_weld");
  CHECK(std::string(vertex_id_source_name(static_cast<VertexIdSource>(77))) == "unknown");
}

TEST_CASE(
    "vertex ids: the weld keeps two points the author named apart, and merges one named once") {
  // The split grid written as its own triangle soup, so every corner is a vertex and the weld has
  // everything to do. Each soup corner keeps its split-grid vertex's attributes and id.
  const SplitGrid g = make_split_grid(9, 0, 8);
  Vector<Vec3> positions;
  Vector<Vec3> normals;
  Vector<Vec2> uvs;
  Vector<u32> ids;
  Vector<u32> indices;
  for (const u32 v : g.indices) {
    indices.push_back(positions.size());
    positions.push_back(g.positions[v]);
    normals.push_back(g.normals[v]);
    uvs.push_back(g.uvs[v]);
    ids.push_back(g.ids[v]);
  }

  // Without the ids nothing tells the two columns at x = 0.5 apart, so they weld into one: 81
  // vertices, the grid's own count. This is the defect the stream exists to prevent.
  {
    Vector<Vec3> p = positions;
    Vector<Vec3> n = normals;
    Vector<Vec2> t = uvs;
    Vector<u32> i = indices;
    CHECK(weld_vertices(p, n, t, i) == 81);
  }
  // With them the split survives — 81 + 9 — every soup corner of one grid point with one id still
  // merges (equal ids permit the merge the other streams agree on), and the ids were renumbered
  // with the vertices, so every welded vertex still carries the id its corners had.
  Vector<u32> welded_ids = ids;
  Vector<Vec3> p = positions;
  Vector<Vec3> n = normals;
  Vector<Vec2> t = uvs;
  Vector<u32> i = indices;
  const u32 welded = weld_vertices(p, n, t, i, nullptr, nullptr, &welded_ids);
  CHECK(welded == 81 + g.seam_pairs);
  REQUIRE(welded_ids.size() == welded);
  u32 wrong = 0;
  for (u32 k = 0; k < indices.size(); ++k)
    wrong += welded_ids[i[k]] != ids[indices[k]] ? 1u : 0u;
  CHECK(wrong == 0);
  Vector<u32> distinct = welded_ids;
  std::sort(distinct.begin(), distinct.end());
  CHECK(std::unique(distinct.begin(), distinct.end()) == distinct.end());  // one vertex an id

  // **A derived id never splits anything**: it is a function of the position, so a weld keyed on it
  // gives the same vertices and the same indices as a weld without it. That is what makes the id
  // stream free for every mesh with no authored ids.
  Vector<u32> derived;
  position_weld_ids(positions, derived);
  Vector<Vec3> p2 = positions;
  Vector<Vec3> n2 = normals;
  Vector<Vec2> t2 = uvs;
  Vector<u32> i2 = indices;
  Vector<Vec3> p3 = positions;
  Vector<Vec3> n3 = normals;
  Vector<Vec2> t3 = uvs;
  Vector<u32> i3 = indices;
  CHECK(weld_vertices(p2, n2, t2, i2, nullptr, nullptr, &derived) == weld_vertices(p3, n3, t3, i3));
  CHECK(i2.size() == i3.size());
  CHECK(std::equal(i2.begin(), i2.end(), i3.begin()));
}

TEST_CASE(
    "vertex ids: every cluster copy of a vertex carries its id, and the validator checks it") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_grid(33, positions, indices);
  Vector<u32> ids;
  position_weld_ids(positions, ids);
  AttributeSource attributes;
  attributes.vertex_ids = std::span<const u32>(ids.data(), ids.size());
  attributes.vertex_id_source = VertexIdSource::position_weld;

  ClusterMesh mesh;
  std::string error;
  const ClusterBuildOptions options{};
  REQUIRE_MESSAGE(build_clusters(positions, indices, options, mesh, &error, attributes), error);
  REQUIRE(mesh.vertex_ids.size() == mesh.vertices.size());
  CHECK(mesh.vertex_id_source == VertexIdSource::position_weld);
  CHECK(mesh.vertices.size() > positions.size());  // shared vertices are duplicated across clusters
  CHECK(misplaced_ids(mesh, ids) == 0);
  CHECK_MESSAGE(validate_clusters(mesh, indices, options, &error), error);

  // A mesh built without ids has no stream and says so.
  ClusterMesh plain;
  REQUIRE(build_clusters(positions, indices, options, plain, &error));
  CHECK(plain.vertex_ids.empty());
  CHECK(plain.vertex_id_source == VertexIdSource::none);

  // The validator: two cluster copies of one source vertex disagreeing, a short stream, ids with no
  // source, and a source with no ids each fail with their own sentence.
  u32 copy_a = ~0u;
  u32 copy_b = ~0u;
  for (u32 v = 0; v < mesh.vertices.size() && copy_b == ~0u; ++v) {
    for (u32 w = v + 1; w < mesh.vertices.size(); ++w) {
      if (mesh.vertex_source[w] == mesh.vertex_source[v]) {
        copy_a = v;
        copy_b = w;
        break;
      }
    }
  }
  REQUIRE(copy_b != ~0u);
  ClusterMesh broken = mesh;
  broken.vertex_ids[copy_b] = broken.vertex_ids[copy_a] + 1;
  CHECK_FALSE(validate_clusters(broken, indices, options, &error));
  CHECK(error.find("different vertex ids") != std::string::npos);
  broken = mesh;
  broken.vertex_ids.pop_back();
  CHECK_FALSE(validate_clusters(broken, indices, options, &error));
  CHECK(error.find("not parallel") != std::string::npos);
  broken = mesh;
  broken.vertex_id_source = VertexIdSource::none;
  CHECK_FALSE(validate_clusters(broken, indices, options, &error));
  CHECK(error.find("does not say where") != std::string::npos);
  broken = plain;
  broken.vertex_id_source = VertexIdSource::authored;
  CHECK_FALSE(validate_clusters(broken, indices, options, &error));
  CHECK(error.find("carries no ids") != std::string::npos);
}

TEST_CASE("vertex ids: the LOD builder keeps an authored id seam, on every level") {
  // A seam from border to border, built straight into a DAG with authored ids. Every level's
  // cluster copies carry their source vertex's id, and no triangle on any level uses one side's
  // copy with a corner on the other side. A clean seam is the easy case: meshoptimizer routes a
  // collapse's wedges along the seam's edge loops whether or not the vertices are tagged, so it
  // does not cross with the rule off either — the slit below is where the rule earns its keep.
  const SplitGrid seam = make_split_grid(65, 0, 64);
  std::string error;
  const ClusterLodMesh lod = build_with(seam, SeamRule::protect);
  CHECK_MESSAGE(validate_cluster_lod(lod, seam.indices, &error), error);
  REQUIRE(lod.mesh.vertex_ids.size() == lod.mesh.vertices.size());
  CHECK(misplaced_ids(lod.mesh, seam.ids) == 0);
  REQUIRE(lod.level_cluster_counts.size() >= 3);  // deep enough that coarse levels exist to check
  CHECK(crossing_triangles(lod, seam) == 0);
  const ClusterLodMesh seam_off = build_with(seam, SeamRule::none);
  MESSAGE("seam, " << seam.indices.size() / 3 << " triangles: " << lod.mesh.clusters.size()
                   << " clusters in " << lod.level_cluster_counts.size() << " levels with "
                   << "id_seams = protect, " << crossing_triangles(lod, seam) << " crossing; "
                   << seam_off.mesh.clusters.size() << " clusters with none, "
                   << crossing_triangles(seam_off, seam) << " crossing");

  // **A slit** — the column split over the middle half of the rows, on a surface with some relief,
  // closed at both ends: closed lips. Here the seam's two copies are `Kind_Complex` to permissive
  // simplification unless tagged, and a collapse at or near the slit hands one lip's triangles the
  // other lip's vertex. `protect` keeps the copies seam vertices, which is the difference: at
  // 65 x 65 the rule-off build crosses the slit and the default does not; at 129 x 129 the default
  // is not absolute — a manifold vertex beside the slit's end may still collapse onto one lip's
  // copy, which no tag on the copies prevents — but it crosses far less. The numbers are this
  // test's MESSAGE output (geometry.md, "Canonical vertex identity").
  for (const u32 n : {65u, 129u}) {
    const SplitGrid slit = make_split_grid(n, n / 4 + 1, 3 * n / 4 - 1, /*flat=*/false);
    const ClusterLodMesh kept = build_with(slit, SeamRule::protect);
    const ClusterLodMesh merged = build_with(slit, SeamRule::none);
    CHECK_MESSAGE(validate_cluster_lod(kept, slit.indices, &error), error);
    CHECK(misplaced_ids(kept.mesh, slit.ids) == 0);
    const u32 with_rule = crossing_triangles(kept, slit);
    const u32 without_rule = crossing_triangles(merged, slit);
    MESSAGE("slit " << n << "x" << n << " (" << slit.seam_pairs
                    << " split vertices): " << kept.mesh.clusters.size() << " clusters in "
                    << kept.level_cluster_counts.size() << " levels with id_seams = protect, "
                    << with_rule << " triangles crossing; " << merged.mesh.clusters.size()
                    << " clusters with none, " << without_rule << " crossing");
    CHECK(without_rule > 0);          // the case is real
    CHECK(with_rule < without_rule);  // and the rule is what closes it
    if (n == 65) CHECK(with_rule == 0);
  }

  // And a derived id never tags anything: the DAG is the one the same build gives with no ids.
  Vector<u32> derived;
  position_weld_ids(seam.positions, derived);
  AttributeSource with_derived = source_of(seam, VertexIdSource::position_weld);
  with_derived.vertex_ids = std::span<const u32>(derived.data(), derived.size());
  AttributeSource without = source_of(seam, VertexIdSource::none);
  without.vertex_ids = {};
  ClusterLodMesh a;
  ClusterLodMesh b;
  REQUIRE(build_cluster_lod(seam.positions, seam.indices, ClusterLodOptions{}, a, &error,
                            with_derived));
  REQUIRE(build_cluster_lod(seam.positions, seam.indices, ClusterLodOptions{}, b, &error, without));
  REQUIRE(a.mesh.clusters.size() == b.mesh.clusters.size());
  REQUIRE(a.mesh.triangles.size() == b.mesh.triangles.size());
  CHECK(std::equal(a.mesh.triangles.begin(), a.mesh.triangles.end(), b.mesh.triangles.begin()));
  CHECK(std::equal(a.mesh.vertex_source.begin(), a.mesh.vertex_source.end(),
                   b.mesh.vertex_source.begin()));
  CHECK(b.mesh.vertex_ids.empty());
}

TEST_CASE("vertex ids: the merges and the page layout move each id with its vertex") {
  // Two parts of one mesh (the split grid's two sides, as the content build builds one DAG per
  // primitive over one welded source), merged, laid out in pages, and then merged as two separate
  // meshes of a scene beside a mesh with no ids.
  const SplitGrid g = make_split_grid(33, 0, 32);
  const u32 half = static_cast<u32>(g.indices.size() / 2 / 3 * 3);
  const std::span<const u32> first(g.indices.data(), half);
  const std::span<const u32> second(g.indices.data() + half, g.indices.size() - half);
  const AttributeSource attributes = source_of(g, VertexIdSource::authored);
  std::string error;
  Vector<ClusterLodMesh> parts(2);
  REQUIRE_MESSAGE(
      build_cluster_lod(g.positions, first, ClusterLodOptions{}, parts[0], &error, attributes),
      error);
  REQUIRE_MESSAGE(
      build_cluster_lod(g.positions, second, ClusterLodOptions{}, parts[1], &error, attributes),
      error);
  ClusterLodMesh merged;
  REQUIRE_MESSAGE(merge_cluster_lod(parts, merged, nullptr, &error), error);
  REQUIRE(merged.mesh.vertex_ids.size() == merged.mesh.vertices.size());
  CHECK(merged.mesh.vertex_id_source == VertexIdSource::authored);
  // The merge shifts each part's `vertex_source` into its own range of a doubled source, so the id
  // a cluster vertex should carry is the source id at its source index modulo the part's size.
  const u32 source_count = g.positions.size();
  auto wrong_in = [&](const ClusterMesh& mesh) {
    u32 wrong = 0;
    for (u32 v = 0; v < mesh.vertices.size(); ++v)
      wrong += mesh.vertex_ids[v] != g.ids[mesh.vertex_source[v] % source_count] ? 1u : 0u;
    return wrong;
  };
  CHECK(wrong_in(merged.mesh) == 0);

  // Paging renumbers the clusters and reorders every per-vertex stream; the ids come along, and
  // they cost a page nothing: the layout is the same with the stream as without it.
  ClusterLodMesh paged = merged;
  ClusterPages pages;
  ClusterPagesOptions page_options;
  page_options.page_bytes = 16 * 1024;
  REQUIRE_MESSAGE(build_cluster_pages(paged, page_options, pages, &error), error);
  CHECK(wrong_in(paged.mesh) == 0);
  ClusterLodMesh bare = merged;
  bare.mesh.vertex_ids.clear();
  bare.mesh.vertex_id_source = VertexIdSource::none;
  ClusterPages bare_pages;
  REQUIRE(build_cluster_pages(bare, page_options, bare_pages, &error));
  REQUIRE(bare_pages.pages.size() == pages.pages.size());
  for (u32 p = 0; p < pages.pages.size(); ++p)
    CHECK(bare_pages.pages[p].bytes == pages.pages[p].bytes);

  // Two separate meshes: the first with no ids, the second with them. Each keeps its own id space
  // and says so on its own part; the one without is filled with k_no_vertex_id, which keeps the
  // merged stream parallel.
  Vector<Vec3> grid;
  Vector<u32> grid_indices;
  make_grid(9, grid, grid_indices);
  Vector<ClusterLodMesh> meshes(2);
  REQUIRE(build_cluster_lod(grid, grid_indices, ClusterLodOptions{}, meshes[0], &error));
  meshes[1] = merged;
  ClusterLodMesh scene;
  Vector<ClusterMeshPart> scene_parts;
  REQUIRE_MESSAGE(merge_cluster_meshes(meshes, scene, scene_parts, &error), error);
  REQUIRE(scene_parts.size() == 2);
  CHECK(scene_parts[0].vertex_id_source == VertexIdSource::none);
  CHECK(scene_parts[1].vertex_id_source == VertexIdSource::authored);
  REQUIRE(scene.mesh.vertex_ids.size() == scene.mesh.vertices.size());
  for (u32 v = 0; v < scene_parts[1].first_vertex; ++v)
    CHECK(scene.mesh.vertex_ids[v] == k_no_vertex_id);
  u32 wrong = 0;
  for (u32 v = scene_parts[1].first_vertex; v < scene.mesh.vertices.size(); ++v) {
    const u32 local = v - scene_parts[1].first_vertex;
    wrong += scene.mesh.vertex_ids[v] != merged.mesh.vertex_ids[local] ? 1u : 0u;
  }
  CHECK(wrong == 0);

  // A merge of two meshes with no ids stays a merge with no ids.
  Vector<ClusterLodMesh> plain(2);
  plain[0] = meshes[0];
  plain[1] = meshes[0];
  ClusterLodMesh plain_scene;
  REQUIRE(merge_cluster_meshes(plain, plain_scene, scene_parts, &error));
  CHECK(plain_scene.mesh.vertex_ids.empty());
  CHECK(plain_scene.mesh.vertex_id_source == VertexIdSource::none);

  // Two parts of one mesh must agree about where their ids came from.
  Vector<ClusterLodMesh> disagreeing(2);
  disagreeing[0] = parts[0];
  disagreeing[1] = parts[1];
  disagreeing[1].mesh.vertex_id_source = VertexIdSource::position_weld;
  ClusterLodMesh refused;
  CHECK_FALSE(merge_cluster_lod(disagreeing, refused, nullptr, &error));
  CHECK(error.find("disagree about where their vertex ids came from") != std::string::npos);
}
