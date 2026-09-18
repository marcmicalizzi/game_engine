// Cluster pages: the layout (contiguous, coarse first, group by group, within the byte target),
// the renumbering (the same geometry and the same DAG, and the same cut modulo the permutation),
// and the streaming rule (equal to select_lod with everything resident; a fallback to the coarser
// clusters that still covers the surface, and a request for exactly the missing pages, when a
// page is gone), plus the residency manager's eviction order.
#include <domain/geometry/cluster_pages.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

using namespace engine;
using namespace engine::geometry;

namespace {

// The same heightfield the LOD tests use, so the DAG has several levels and real error bounds.
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

LodView terrain_view(f32 distance, f32 threshold) {
  LodView view;
  view.camera = Vec3{0.0f, distance, 0.0f};
  view.znear = 0.1f;
  view.proj_scale = 1.0f / std::tan(radians(60.0f) * 0.5f) * 1080.0f * 0.5f;
  view.threshold_px = threshold;
  return view;
}

// One triangle as the three positions it spans, with the corners in a canonical order so that a
// triangle compares equal however the cluster indexed it.
struct Corner {
  f32 v[3] = {};
};

struct TrianglePositions {
  f32 p[9] = {};
};

bool corner_less(const Corner& a, const Corner& b) noexcept {
  for (u32 i = 0; i < 3; ++i) {
    if (a.v[i] != b.v[i]) return a.v[i] < b.v[i];
  }
  return false;
}

bool triangle_less(const TrianglePositions& a, const TrianglePositions& b) noexcept {
  for (u32 i = 0; i < 9; ++i) {
    if (a.p[i] != b.p[i]) return a.p[i] < b.p[i];
  }
  return false;
}

bool triangle_equal(const TrianglePositions& a, const TrianglePositions& b) noexcept {
  return std::memcmp(a.p, b.p, sizeof(a.p)) == 0;
}

// Every triangle of every cluster as a position triple, sorted: the mesh's geometry with the
// numbering thrown away, which is exactly what the permutation must not change.
Vector<TrianglePositions> triangle_multiset(const ClusterMesh& mesh) {
  Vector<TrianglePositions> out;
  out.reserve(mesh.triangles.size());
  for (const ClusterDesc& cluster : mesh.clusters) {
    for (u32 t = 0; t < cluster.triangle_count; ++t) {
      const u32 packed = mesh.triangles[cluster.triangle_offset + t];
      const Vec3 corner[3] = {
          mesh.vertices[cluster.vertex_offset + ClusterMesh::unpack(packed, 0)],
          mesh.vertices[cluster.vertex_offset + ClusterMesh::unpack(packed, 1)],
          mesh.vertices[cluster.vertex_offset + ClusterMesh::unpack(packed, 2)]};
      Corner values[3];
      for (u32 c = 0; c < 3; ++c)
        values[c] = Corner{{corner[c].x, corner[c].y, corner[c].z}};
      std::sort(&values[0], &values[0] + 3, corner_less);
      TrianglePositions entry;
      std::memcpy(entry.p, values, sizeof(entry.p));
      out.push_back(entry);
    }
  }
  std::sort(out.begin(), out.end(), triangle_less);
  return out;
}

u32 triangles_of(const ClusterLodMesh& mesh, const Vector<u32>& clusters) {
  u32 total = 0;
  for (const u32 c : clusters)
    total += mesh.mesh.clusters[c].triangle_count;
  return total;
}

// The coverage check: walk the DAG from its roots down, the way the cut is supposed to have been
// taken, and see whether the cut is exactly what the walk finds. A cluster is *live* when
// everything above it chose to refine rather than draw; a live cluster is either drawn, and the
// walk stops, or it refines into its children, which become live in turn. So a cut covers the
// surface exactly once when every live cluster is either in the cut or has children, every
// cluster in the cut is live, and no live leaf is left undrawn. Anything else is a hole or
// geometry drawn twice.
//
// One forward pass does it, because the page layout puts every cluster before its children.
bool cut_is_crack_free(const ClusterLodMesh& mesh, const ClusterPages& pages,
                       const Vector<u32>& cut, std::string& why) {
  const u32 count = mesh.lod.size();
  Vector<u8> in_cut(count, u8{0});
  for (const u32 c : cut)
    in_cut[c] = 1;
  Vector<u8> live(count, u8{0});
  // The roots: a cluster whose group has no coarser version is live in every cut.
  for (u32 c = 0; c < count; ++c) {
    if (mesh.lod[c].parent_error >= k_lod_terminal_error) live[c] = 1;
  }
  for (u32 c = 0; c < count; ++c) {
    if (live[c] == 0) {
      if (in_cut[c] == 0) continue;
      why = "cluster " + std::to_string(c) + " at level " + std::to_string(mesh.lod[c].level) +
            " is drawn although something coarser is drawn over it";
      return false;
    }
    if (in_cut[c] != 0) continue;  // drawn: nothing below it is
    const ClusterChildren& kids = pages.children[c];
    if (kids.cluster_count == 0) {
      why = "level 0 cluster " + std::to_string(c) +
            " is neither drawn nor replaced by anything coarser: a hole";
      return false;
    }
    for (u32 k = 0; k < kids.cluster_count; ++k)
      live[kids.first_cluster + k] = 1;
  }
  return true;
}

}  // namespace

TEST_CASE("cluster pages: a terrain lays out in pages, coarse first, group by group") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(129, 10.0f, positions, indices);  // 32,768 triangles
  ClusterLodMesh lod;
  std::string error;
  REQUIRE_MESSAGE(build_cluster_lod(positions, indices, ClusterLodOptions{}, lod, &error), error);
  const u32 count = lod.mesh.clusters.size();
  const u32 level_count = lod.level_cluster_counts.size();

  ClusterPages pages;
  Vector<u32> source_of_cluster;
  const ClusterPagesOptions options;  // the 128 KB default: a terrain this size fills several
  REQUIRE_MESSAGE(build_cluster_pages(lod, options, pages, &error, &source_of_cluster), error);
  CHECK_MESSAGE(validate_cluster_pages(lod, pages, &error), error);
  CHECK(pages.page_bytes_target == options.page_bytes);
  CHECK(source_of_cluster.size() == count);
  CHECK(pages.page_of_cluster.size() == count);
  CHECK(pages.children.size() == count);
  REQUIRE(pages.pages.size() > 1);

  // The pages tile the clusters, the vertices, and the triangles, and every cluster is in the one
  // page that names it.
  u32 cluster_at = 0;
  u32 vertex_at = 0;
  u32 triangle_at = 0;
  u32 oversized = 0;
  u64 page_bytes_total = 0;
  for (u32 p = 0; p < pages.pages.size(); ++p) {
    const ClusterPageDesc& page = pages.pages[p];
    CHECK(page.first_cluster == cluster_at);
    CHECK(page.first_vertex == vertex_at);
    CHECK(page.first_triangle == triangle_at);
    CHECK(page.cluster_count > 0);
    for (u32 k = 0; k < page.cluster_count; ++k)
      CHECK(pages.page_of_cluster[page.first_cluster + k] == p);
    if ((page.flags & k_page_oversized) != 0) {
      ++oversized;
    } else {
      CHECK(page.bytes <= options.page_bytes);
    }
    cluster_at += page.cluster_count;
    vertex_at += page.vertex_count;
    triangle_at += page.triangle_count;
    page_bytes_total += page.bytes;
  }
  CHECK(cluster_at == count);
  CHECK(vertex_at == lod.mesh.vertices.size());
  CHECK(triangle_at == lod.mesh.triangles.size());
  CHECK(oversized == 0);  // a group of a terrain is well under 128 KB

  // Coarse to fine, with the whole coarsest level in page 0 and level 0 last.
  CHECK((pages.pages[0].flags & k_page_root) != 0);
  CHECK(pages.pages[0].level_max == level_count - 1);
  u32 coarsest_in_page_0 = 0;
  for (u32 k = 0; k < pages.pages[0].cluster_count; ++k) {
    if (lod.lod[pages.pages[0].first_cluster + k].level == level_count - 1) ++coarsest_in_page_0;
  }
  CHECK(coarsest_in_page_0 == lod.level_cluster_counts[level_count - 1]);
  CHECK(lod.lod[count - 1].level == 0);
  // The pages that hold a group nothing coarser can replace are a prefix — the minimum resident
  // set — and after them the refinement runs coarse to fine.
  u32 root_pages = 0;
  while (root_pages < pages.pages.size() && (pages.pages[root_pages].flags & k_page_root) != 0)
    ++root_pages;
  CHECK(root_pages >= 1);
  for (u32 p = root_pages; p < pages.pages.size(); ++p) {
    CHECK((pages.pages[p].flags & k_page_root) == 0);
    if (p > root_pages) CHECK(pages.pages[p].level_max <= pages.pages[p - 1].level_max);
  }
  // Every cluster's children come after it, which is what makes a prefix of the pages something
  // a viewer can draw and what lets eviction work from the fine end inward.
  for (u32 c = 0; c < count; ++c) {
    if (pages.children[c].cluster_count != 0) CHECK(pages.children[c].first_cluster > c);
  }

  // A group is one run of clusters inside one page, unless it was too big for a page of its own.
  for (u32 c = 1; c < count; ++c) {
    if (lod.lod[c].group == lod.lod[c - 1].group)
      CHECK(pages.page_of_cluster[c] == pages.page_of_cluster[c - 1]);
  }
  Vector<u32> seen_group(lod.group_count, 0u);
  for (u32 c = 0; c < count; ++c) {
    if (c == 0 || lod.lod[c].group != lod.lod[c - 1].group) {
      CHECK(seen_group[lod.lod[c].group] == 0);
      seen_group[lod.lod[c].group] = 1;
    }
  }

  const f64 fill = static_cast<f64>(page_bytes_total) /
                   (static_cast<f64>(pages.pages.size()) * static_cast<f64>(options.page_bytes));
  MESSAGE("terrain: " << count << " clusters in " << pages.pages.size() << " pages of "
                      << options.page_bytes << " bytes, " << page_bytes_total
                      << " bytes total, fill " << fill << ", " << pages.child_pages.size()
                      << " child page entries");
  CHECK(fill > 0.5);  // a page is not half empty when the groups are this much smaller than one
}

TEST_CASE("cluster pages: the permutation keeps the geometry, the DAG, and the cut") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(65, 10.0f, positions, indices);  // 8,192 triangles
  ClusterLodMesh before;
  std::string error;
  REQUIRE_MESSAGE(build_cluster_lod(positions, indices, ClusterLodOptions{}, before, &error),
                  error);
  ClusterLodMesh after = before;
  const Vector<TrianglePositions> geometry_before = triangle_multiset(before.mesh);

  ClusterPages pages;
  Vector<u32> source_of_cluster;
  ClusterPagesOptions options;
  options.page_bytes = 64 * 1024;
  REQUIRE_MESSAGE(build_cluster_pages(after, options, pages, &error, &source_of_cluster), error);
  const u32 count = after.mesh.clusters.size();
  REQUIRE(count == before.mesh.clusters.size());

  // The same triangles, as positions, in some other order.
  const Vector<TrianglePositions> geometry_after = triangle_multiset(after.mesh);
  REQUIRE(geometry_after.size() == geometry_before.size());
  bool same_geometry = true;
  for (u32 i = 0; i < geometry_after.size(); ++i)
    same_geometry = same_geometry && triangle_equal(geometry_after[i], geometry_before[i]);
  CHECK(same_geometry);
  // And the whole mesh still validates against the source it was built from, which covers the
  // streams, the spheres, the cones, and the quantized positions in one go.
  CHECK_MESSAGE(validate_cluster_lod(after, indices, &error), error);
  CHECK(after.mesh.quant_scale == before.mesh.quant_scale);
  CHECK(after.leaf_triangle_count == before.leaf_triangle_count);
  CHECK(after.group_count == before.group_count);
  REQUIRE(after.level_cluster_counts.size() == before.level_cluster_counts.size());
  for (u32 l = 0; l < after.level_cluster_counts.size(); ++l)
    CHECK(after.level_cluster_counts[l] == before.level_cluster_counts[l]);

  // Every cluster is the same cluster, with the same bounds and the same errors; only its index
  // and its group number moved.
  Vector<u32> new_of_old(count, ~u32{0});
  for (u32 n = 0; n < count; ++n) {
    const u32 from = source_of_cluster[n];
    REQUIRE(from < count);
    CHECK(new_of_old[from] == ~u32{0});  // a permutation: every old cluster exactly once
    new_of_old[from] = n;
    const ClusterDesc& a = before.mesh.clusters[from];
    const ClusterDesc& b = after.mesh.clusters[n];
    CHECK(b.vertex_count == a.vertex_count);
    CHECK(b.triangle_count == a.triangle_count);
    CHECK(b.center == a.center);
    CHECK(b.radius == a.radius);
    CHECK(b.cone == a.cone);
    const ClusterLodDesc& x = before.lod[from];
    const ClusterLodDesc& y = after.lod[n];
    CHECK(y.own == x.own);
    CHECK(y.parent == x.parent);
    CHECK(y.own_error == x.own_error);
    CHECK(y.parent_error == x.parent_error);
    CHECK(y.level == x.level);
  }
  // Group membership is the same partition, renumbered: two clusters share a group afterwards
  // exactly when they shared one before.
  Vector<u32> group_map(before.group_count, ~u32{0});
  for (u32 from = 0; from < count; ++from) {
    const u32 old_group = before.lod[from].group;
    const u32 new_group = after.lod[new_of_old[from]].group;
    if (group_map[old_group] == ~u32{0}) group_map[old_group] = new_group;
    CHECK(group_map[old_group] == new_group);
  }

  // The same cut, modulo the renumbering, at three distances.
  for (const f32 distance : {6.0f, 40.0f, 300.0f}) {
    const LodView view = terrain_view(distance, 1.0f);
    Vector<u32> cut_before;
    Vector<u32> cut_after;
    const u32 n_before = select_lod(before, view, cut_before);
    const u32 n_after = select_lod(after, view, cut_after);
    REQUIRE(n_before == n_after);
    REQUIRE(n_before > 0);
    Vector<u32> mapped;
    mapped.reserve(n_before);
    for (const u32 c : cut_before)
      mapped.push_back(new_of_old[c]);
    std::sort(mapped.begin(), mapped.end());
    bool same_cut = true;
    for (u32 i = 0; i < mapped.size(); ++i)
      same_cut = same_cut && mapped[i] == cut_after[i];
    CHECK(same_cut);
    for (const u32 c : cut_after)
      CHECK(lod_selects(after.lod[c], view));
  }

  // Deterministic: the same DAG laid out twice gives the same pages and the same permutation.
  ClusterLodMesh again = before;
  ClusterPages pages_again;
  Vector<u32> source_again;
  REQUIRE(build_cluster_pages(again, options, pages_again, &error, &source_again));
  REQUIRE(pages_again.pages.size() == pages.pages.size());
  CHECK(std::memcmp(pages_again.pages.data(), pages.pages.data(),
                    pages.pages.size() * sizeof(ClusterPageDesc)) == 0);
  REQUIRE(source_again.size() == source_of_cluster.size());
  CHECK(std::memcmp(source_again.data(), source_of_cluster.data(),
                    source_of_cluster.size() * sizeof(u32)) == 0);
}

TEST_CASE("cluster pages: a target smaller than a group makes oversized pages, not split groups") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(33, 4.0f, positions, indices);
  ClusterLodMesh lod;
  std::string error;
  REQUIRE_MESSAGE(build_cluster_lod(positions, indices, ClusterLodOptions{}, lod, &error), error);

  ClusterPagesOptions options;
  options.page_bytes = 2048;  // under two clusters' worth, so most groups do not fit
  ClusterPages pages;
  REQUIRE_MESSAGE(build_cluster_pages(lod, options, pages, &error), error);
  CHECK_MESSAGE(validate_cluster_pages(lod, pages, &error), error);
  u32 oversized = 0;
  for (const ClusterPageDesc& page : pages.pages) {
    if (page.bytes > options.page_bytes) {
      CHECK((page.flags & k_page_oversized) != 0);
      ++oversized;
    }
  }
  CHECK(oversized > 0);
  // Even then a group is one run of clusters: the page grew rather than the group splitting.
  for (u32 c = 1; c < lod.lod.size(); ++c) {
    if (lod.lod[c].group != lod.lod[c - 1].group) continue;
    CHECK(pages.page_of_cluster[c] == pages.page_of_cluster[c - 1]);
  }
  MESSAGE(pages.pages.size() << " pages of 2 KB, " << oversized << " of them oversized");

  // Bad input: no bytes at all, and a LOD table that does not match.
  ClusterPagesOptions none;
  none.page_bytes = 0;
  ClusterPages empty;
  CHECK_FALSE(build_cluster_pages(lod, none, empty, &error));
  CHECK(error.find("page_bytes") != std::string::npos);
  ClusterLodMesh broken = lod;
  broken.lod.pop_back();
  CHECK_FALSE(build_cluster_pages(broken, ClusterPagesOptions{}, empty, &error));
  CHECK(empty.pages.empty());
}

TEST_CASE("cluster pages: streaming selection is select_lod when every page is resident") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(65, 10.0f, positions, indices);
  ClusterLodMesh lod;
  std::string error;
  REQUIRE(build_cluster_lod(positions, indices, ClusterLodOptions{}, lod, &error));
  ClusterPages pages;
  ClusterPagesOptions options;
  options.page_bytes = 64 * 1024;
  REQUIRE_MESSAGE(build_cluster_pages(lod, options, pages, &error), error);

  PageResidency all;
  all.resident.assign(pages.pages.size(), u8{1});
  for (const f32 distance : {6.0f, 20.0f, 60.0f, 200.0f}) {
    const LodView view = terrain_view(distance, 1.0f);
    Vector<u32> plain;
    Vector<u32> streamed;
    Vector<u32> requests;
    const u32 a = select_lod(lod, view, plain);
    const u32 b = select_lod_streaming(lod, view, pages, all, streamed, requests);
    REQUIRE(a == b);
    REQUIRE(streamed.size() == plain.size());
    bool same = true;
    for (u32 i = 0; i < plain.size(); ++i)
      same = same && plain[i] == streamed[i];
    CHECK(same);
    CHECK(requests.empty());
    std::string why;
    CHECK_MESSAGE(cut_is_crack_free(lod, pages, streamed, why), why);
  }
}

TEST_CASE("cluster pages: a missing page falls back to the coarser clusters and asks for it") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(65, 10.0f, positions, indices);
  ClusterLodMesh lod;
  std::string error;
  REQUIRE(build_cluster_lod(positions, indices, ClusterLodOptions{}, lod, &error));
  ClusterPages pages;
  ClusterPagesOptions options;
  options.page_bytes = 64 * 1024;
  REQUIRE_MESSAGE(build_cluster_pages(lod, options, pages, &error), error);
  REQUIRE(pages.pages.size() > 2);

  const LodView view = terrain_view(6.0f, 1.0f);  // close: the cut wants the fine levels
  PageResidency all;
  all.resident.assign(pages.pages.size(), u8{1});
  Vector<u32> full;
  Vector<u32> no_requests;
  REQUIRE(select_lod_streaming(lod, view, pages, all, full, no_requests) > 0);
  CHECK(no_requests.empty());

  // Drop a page the full cut is drawing out of, one with nothing under it so that residency
  // stays ancestor-closed — which is exactly what eviction is allowed to take. Its clusters
  // cannot be drawn, their parents are drawn instead, the surface is still covered exactly once,
  // and the one page that is gone is the one that is asked for.
  u32 missing = ~u32{0};
  for (const u32 c : full) {
    const u32 page = pages.page_of_cluster[c];
    if (page == 0 || pages.pages[page].child_page_count != 0) continue;
    if ((pages.pages[page].flags & k_page_root) != 0) continue;
    missing = page;
    break;
  }
  REQUIRE(missing != ~u32{0});
  PageResidency partial = all;
  partial.resident[missing] = 0;
  Vector<u32> fallback;
  Vector<u32> requests;
  const u32 drawn = select_lod_streaming(lod, view, pages, partial, fallback, requests);
  REQUIRE(drawn > 0);
  REQUIRE(requests.size() == 1);
  CHECK(requests[0] == missing);
  for (const u32 c : fallback)
    CHECK(pages.page_of_cluster[c] != missing);
  std::string why;
  CHECK_MESSAGE(cut_is_crack_free(lod, pages, fallback, why), why);
  CHECK(drawn < full.size());  // coarser clusters cover the same surface with fewer of them
  CHECK(triangles_of(lod, fallback) < triangles_of(lod, full));
  MESSAGE("page " << missing << " missing: " << drawn << " clusters and "
                  << triangles_of(lod, fallback) << " triangles instead of " << full.size()
                  << " and " << triangles_of(lod, full));

  // With nothing but the pages holding the DAG's roots — page 0, plus whatever page holds a
  // group that could not be simplified any further, which nothing above would ever ask for —
  // there is nothing finer to fall back to, so the cut is those roots and the requests are what
  // would refine them.
  PageResidency roots;
  roots.resident.assign(pages.pages.size(), u8{0});
  for (u32 c = 0; c < lod.lod.size(); ++c) {
    if (lod.lod[c].parent_error >= k_lod_terminal_error)
      roots.resident[pages.page_of_cluster[c]] = 1;
  }
  CHECK(roots.is_resident(0));
  Vector<u32> coarse;
  Vector<u32> coarse_requests;
  const u32 coarse_count = select_lod_streaming(lod, view, pages, roots, coarse, coarse_requests);
  REQUIRE(coarse_count > 0);
  CHECK_FALSE(coarse_requests.empty());
  for (const u32 c : coarse)
    CHECK(roots.is_resident(pages.page_of_cluster[c]));
  CHECK_MESSAGE(cut_is_crack_free(lod, pages, coarse, why), why);
  CHECK(triangles_of(lod, coarse) < triangles_of(lod, fallback));
  // The requests are sorted, free of repeats, and every one of them is a page that is not there.
  for (u32 i = 0; i < coarse_requests.size(); ++i) {
    CHECK_FALSE(roots.is_resident(coarse_requests[i]));
    if (i != 0) CHECK(coarse_requests[i] > coarse_requests[i - 1]);
  }
}

TEST_CASE("cluster pages: the residency manager evicts the least recently used, never the root") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(65, 10.0f, positions, indices);
  ClusterLodMesh lod;
  std::string error;
  REQUIRE(build_cluster_lod(positions, indices, ClusterLodOptions{}, lod, &error));
  ClusterPages pages;
  ClusterPagesOptions options;
  options.page_bytes = 16 * 1024;  // about one group a page, so there are plenty to evict
  REQUIRE_MESSAGE(build_cluster_pages(lod, options, pages, &error), error);
  const u32 page_count = pages.pages.size();
  REQUIRE(page_count > 5);

  u64 total = 0;
  for (const ClusterPageDesc& page : pages.pages)
    total += page.bytes;

  PageResidencyManager manager;
  REQUIRE_MESSAGE(manager.reset(pages, total, &error), error);
  CHECK(manager.total_bytes() == total);
  CHECK(manager.resident_pages() >= 1);
  CHECK(manager.page_residency().is_resident(0));  // the root is there before anything is asked

  // Everything requested is admitted in request order, and a request for a resident page is not
  // queued again.
  Vector<u32> wanted;
  for (u32 p = 0; p < page_count; ++p)
    wanted.push_back(p);
  manager.request(std::span<const u32>(wanted.data(), wanted.size()));
  CHECK(manager.pending() == page_count - manager.resident_pages());
  CHECK(manager.admit(2) == 2);
  const u32 rest = manager.pending();
  CHECK(manager.admit(~u32{0}) == rest);
  CHECK(manager.pending() == 0);
  CHECK(manager.resident_pages() == page_count);
  CHECK(manager.resident_bytes() == total);

  // Only a page with nothing resident under it may go, so the candidates are the pages at the
  // fine end of the DAG. Touch every one of them but the first this frame: it is the least
  // recently used candidate, so a budget one byte short takes it and nothing else.
  Vector<u32> evictable;
  for (u32 p = 1; p < page_count; ++p) {
    if (pages.pages[p].child_page_count == 0 && (pages.pages[p].flags & k_page_root) == 0)
      evictable.push_back(p);
  }
  REQUIRE(evictable.size() >= 2);
  manager.begin_frame();
  for (u32 p = 0; p < page_count; ++p) {
    if (p != evictable[0]) manager.touch(p);
  }
  manager.request(0);  // resident already: a no-op that does not queue
  CHECK(manager.pending() == 0);
  manager.set_budget(total - 1);
  CHECK(manager.evict_to_budget() == 1);
  CHECK_FALSE(manager.page_residency().is_resident(evictable[0]));
  CHECK(manager.resident_pages() == page_count - 1);
  CHECK(manager.page_residency().is_resident(evictable[1]));

  // A budget of nothing peels the whole DAG from the fine end inward and stops at the pages
  // nothing coarser can replace, because a mesh without those draws a hole.
  manager.set_budget(0);
  const u32 evicted = manager.evict_to_budget();
  u32 pinned = 0;
  for (u32 p = 0; p < page_count; ++p) {
    const bool root = (pages.pages[p].flags & k_page_root) != 0;
    CHECK(manager.page_residency().is_resident(p) == root);
    if (root) ++pinned;
  }
  CHECK(evicted == page_count - 1 - pinned);  // the first eviction already took one
  CHECK(manager.page_residency().is_resident(0));
  CHECK(manager.resident_pages() == pinned);
  MESSAGE("evicted every page but the " << pinned << " of " << page_count
                                        << " that hold a group with nothing coarser above it");

  // An empty page table is refused rather than silently doing nothing.
  PageResidencyManager nothing;
  CHECK_FALSE(nothing.reset(ClusterPages{}, 1024, &error));
  CHECK_FALSE(error.empty());
}
