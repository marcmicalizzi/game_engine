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

// ---- a scene of paged meshes -----------------------------------------------------------------

namespace {

// Two terrains of different sizes, each its own DAG laid out in its own pages: the smallest scene
// that has everything a merged page table has to get right — two root pages, two group spaces,
// and two runs of the cluster, vertex, triangle and child-page arrays.
struct PagedScene {
  Vector<ClusterLodMesh> meshes;
  Vector<ClusterPages> pages;
  ClusterLodMesh scene;
  Vector<ClusterMeshPart> parts;
  ClusterPages table;
};

bool build_paged_scene(u32 page_bytes, PagedScene& out, std::string& error) {
  const u32 sizes[2] = {33u, 65u};
  const f32 extents[2] = {4.0f, 10.0f};
  out.meshes.resize(2);
  out.pages.resize(2);
  for (u32 m = 0; m < 2; ++m) {
    Vector<Vec3> positions;
    Vector<u32> indices;
    make_terrain(sizes[m], extents[m], positions, indices);
    if (!build_cluster_lod(positions, indices, ClusterLodOptions{}, out.meshes[m], &error))
      return false;
    ClusterPagesOptions options;
    options.page_bytes = page_bytes;
    if (!build_cluster_pages(out.meshes[m], options, out.pages[m], &error)) return false;
  }
  return merge_paged_cluster_meshes(
      std::span<const ClusterLodMesh>(out.meshes.data(), out.meshes.size()),
      std::span<const ClusterPages>(out.pages.data(), out.pages.size()), out.scene, out.parts,
      out.table, &error);
}

}  // namespace

TEST_CASE("cluster pages: two paged meshes merge into one scene's page table") {
  PagedScene scene;
  std::string error;
  REQUIRE_MESSAGE(build_paged_scene(16 * 1024, scene, error), error);
  REQUIRE(scene.parts.size() == 2);

  // The page invariants hold over the scene, per mesh where they are about a mesh.
  CHECK_MESSAGE(
      validate_cluster_pages(scene.scene, scene.table,
                             std::span<const ClusterMeshPart>(scene.parts.data(), 2), &error),
      error);
  CHECK(scene.table.page_bytes_target == 16 * 1024);

  // Each mesh's pages stay together, in its own order, with its pinned pages at the front of its
  // own run: that is what lets a scene pin one page per mesh rather than one page in all.
  u32 page_at = 0;
  u32 cluster_at = 0;
  for (u32 m = 0; m < 2; ++m) {
    const ClusterMeshPart& part = scene.parts[m];
    CHECK(part.first_page == page_at);
    CHECK(part.page_count == scene.pages[m].pages.size());
    CHECK(part.first_cluster == cluster_at);
    CHECK(part.cluster_count == scene.meshes[m].mesh.clusters.size());
    CHECK((scene.table.pages[part.first_page].flags & k_page_root) != 0);
    // A paged mesh is coarse first, so its leaves are its last clusters, and the part says so.
    CHECK(part.leaf_cluster_count == scene.meshes[m].level_cluster_counts[0]);
    CHECK(part.first_leaf_cluster == part.cluster_count - part.leaf_cluster_count);
    for (u32 i = 0; i < part.cluster_count; ++i) {
      CHECK((scene.scene.lod[part.first_cluster + i].level == 0) == (i >= part.first_leaf_cluster));
      const u32 page = scene.table.page_of_cluster[part.first_cluster + i];
      CHECK(page >= part.first_page);
      CHECK(page < part.first_page + part.page_count);
    }
    // Every page descriptor is the mesh's own, shifted: the same clusters, bytes, levels, flags.
    for (u32 p = 0; p < part.page_count; ++p) {
      const ClusterPageDesc& before = scene.pages[m].pages[p];
      const ClusterPageDesc& after = scene.table.pages[part.first_page + p];
      CHECK(after.cluster_count == before.cluster_count);
      CHECK(after.vertex_count == before.vertex_count);
      CHECK(after.triangle_count == before.triangle_count);
      CHECK(after.bytes == before.bytes);
      CHECK(after.flags == before.flags);
      CHECK(after.level_min == before.level_min);
      CHECK(after.level_max == before.level_max);
      CHECK(after.child_page_count == before.child_page_count);
      CHECK(after.first_cluster == before.first_cluster + part.first_cluster);
      // A child page is in the same mesh: nothing in one mesh's DAG names another mesh's page.
      for (u32 k = 0; k < after.child_page_count; ++k) {
        const u32 child = scene.table.child_pages[after.first_child_page + k];
        CHECK(child == scene.pages[m].child_pages[before.first_child_page + k] + part.first_page);
        CHECK(child >= part.first_page);
        CHECK(child < part.first_page + part.page_count);
      }
    }
    page_at += part.page_count;
    cluster_at += part.cluster_count;
  }
  CHECK(page_at == scene.table.pages.size());
  CHECK(cluster_at == scene.scene.mesh.clusters.size());
  MESSAGE("scene: " << cluster_at << " clusters in " << page_at << " pages, "
                    << scene.parts[0].page_count << " + " << scene.parts[1].page_count);
}

TEST_CASE("cluster pages: a merged scene's cut is the union of its meshes' cuts") {
  PagedScene scene;
  std::string error;
  REQUIRE_MESSAGE(build_paged_scene(16 * 1024, scene, error), error);
  const u32 page_count = scene.table.pages.size();

  // The page of the second mesh the last test's fallback case takes away: one with nothing under
  // it, so that residency stays ancestor-closed, which is what eviction is allowed to take.
  u32 dropped = ~u32{0};
  for (u32 p = scene.parts[1].page_count; p-- > 0;) {
    if (scene.pages[1].pages[p].child_page_count == 0 &&
        (scene.pages[1].pages[p].flags & k_page_root) == 0) {
      dropped = p;
      break;
    }
  }
  REQUIRE(dropped != ~u32{0});

  // Three residencies, each built the same way for the scene and for the meshes on their own:
  // everything, the pinned pages alone, and everything but that one page.
  for (u32 mode = 0; mode < 3; ++mode) {
    PageResidency scene_residency;
    scene_residency.resident.assign(page_count, u8{0});
    Vector<PageResidency> mesh_residency(2);
    for (u32 m = 0; m < 2; ++m) {
      const ClusterMeshPart& part = scene.parts[m];
      mesh_residency[m].resident.assign(part.page_count, u8{0});
      for (u32 p = 0; p < part.page_count; ++p) {
        const bool root = (scene.pages[m].pages[p].flags & k_page_root) != 0;
        bool here = mode == 0 || root;
        if (mode == 2) here = !(m == 1 && p == dropped);
        mesh_residency[m].resident[p] = here ? u8{1} : u8{0};
        scene_residency.resident[part.first_page + p] = here ? u8{1} : u8{0};
      }
    }

    for (const f32 distance : {5.0f, 30.0f, 150.0f}) {
      const LodView view = terrain_view(distance, 1.0f);
      Vector<u32> scene_cut;
      Vector<u32> scene_requests;
      const u32 drawn = select_lod_streaming(scene.scene, view, scene.table, scene_residency,
                                             scene_cut, scene_requests);
      Vector<u32> union_cut;
      Vector<u32> union_requests;
      u32 union_drawn = 0;
      for (u32 m = 0; m < 2; ++m) {
        Vector<u32> cut;
        Vector<u32> requests;
        union_drawn += select_lod_streaming(scene.meshes[m], view, scene.pages[m],
                                            mesh_residency[m], cut, requests);
        for (const u32 c : cut)
          union_cut.push_back(c + scene.parts[m].first_cluster);
        for (const u32 p : requests)
          union_requests.push_back(p + scene.parts[m].first_page);
      }
      REQUIRE(drawn == union_drawn);
      REQUIRE(scene_cut.size() == union_cut.size());
      bool same = true;
      for (u32 i = 0; i < scene_cut.size(); ++i)
        same = same && scene_cut[i] == union_cut[i];
      CHECK(same);
      REQUIRE(scene_requests.size() == union_requests.size());
      bool same_requests = true;
      for (u32 i = 0; i < scene_requests.size(); ++i)
        same_requests = same_requests && scene_requests[i] == union_requests[i];
      CHECK(same_requests);
      std::string why;
      CHECK_MESSAGE(cut_is_crack_free(scene.scene, scene.table, scene_cut, why), why);
      // Close up, a cut that is missing pages has something to ask for. Far away it does not:
      // the coarse levels are already fine enough, which is the point of streaming.
      if (mode != 0 && distance < 10.0f) CHECK_FALSE(scene_requests.empty());
    }
  }
}

TEST_CASE("cluster pages: a scene pins a root page per mesh, and eviction never takes one") {
  PagedScene scene;
  std::string error;
  REQUIRE_MESSAGE(build_paged_scene(16 * 1024, scene, error), error);
  const u32 page_count = scene.table.pages.size();

  u64 total = 0;
  for (const ClusterPageDesc& page : scene.table.pages)
    total += page.bytes;
  PageResidencyManager manager;
  REQUIRE_MESSAGE(manager.reset(scene.table, total, &error), error);

  // Both meshes' first pages are pinned from the start: a scene that pinned page 0 alone could
  // not draw the second mesh at all.
  CHECK(manager.page_residency().is_resident(scene.parts[0].first_page));
  CHECK(manager.page_residency().is_resident(scene.parts[1].first_page));
  CHECK(manager.resident_pages() >= 2);

  Vector<u32> everything;
  for (u32 p = 0; p < page_count; ++p)
    everything.push_back(p);
  manager.request(std::span<const u32>(everything.data(), everything.size()));
  manager.admit(~u32{0});
  CHECK(manager.resident_pages() == page_count);

  // A budget of nothing peels both meshes from the fine end inward and stops at the pages nothing
  // coarser can replace — in both meshes, not only in the first.
  manager.set_budget(0);
  manager.evict_to_budget();
  u32 pinned[2] = {0, 0};
  for (u32 m = 0; m < 2; ++m) {
    const ClusterMeshPart& part = scene.parts[m];
    for (u32 p = part.first_page; p < part.first_page + part.page_count; ++p) {
      const bool root = (scene.table.pages[p].flags & k_page_root) != 0;
      CHECK(manager.page_residency().is_resident(p) == root);
      if (root) ++pinned[m];
    }
    CHECK(pinned[m] >= 1);
  }
  CHECK(manager.resident_pages() == pinned[0] + pinned[1]);

  // And what is left still draws both meshes: the roots are a picture, coarse but whole.
  const LodView view = terrain_view(5.0f, 1.0f);
  Vector<u32> cut;
  Vector<u32> requests;
  REQUIRE(select_lod_streaming(scene.scene, view, scene.table, manager.page_residency(), cut,
                               requests) > 0);
  u32 per_mesh[2] = {0, 0};
  for (const u32 c : cut)
    ++per_mesh[c < scene.parts[1].first_cluster ? 0 : 1];
  CHECK(per_mesh[0] > 0);
  CHECK(per_mesh[1] > 0);
  std::string why;
  CHECK_MESSAGE(cut_is_crack_free(scene.scene, scene.table, cut, why), why);

  // Bad input is refused rather than half-merged.
  ClusterLodMesh out;
  Vector<ClusterMeshPart> parts;
  ClusterPages table;
  CHECK_FALSE(merge_paged_cluster_meshes({}, {}, out, parts, table, &error));
  Vector<ClusterPages> mismatched(2);
  mismatched[0] = scene.pages[0];
  mismatched[1] = scene.pages[1];
  mismatched[1].page_bytes_target = 4096;
  CHECK_FALSE(merge_paged_cluster_meshes(std::span<const ClusterLodMesh>(scene.meshes.data(), 2),
                                         std::span<const ClusterPages>(mismatched.data(), 2), out,
                                         parts, table, &error));
  CHECK(error.find("page byte target") != std::string::npos);
  CHECK(table.pages.empty());
}

// ---- request priority ------------------------------------------------------------------------

namespace {

// The pages a request may name: everything the manager does not pin, in page order.
Vector<u32> loadable_pages(const ClusterPages& pages) {
  Vector<u32> out;
  for (u32 p = 0; p < pages.pages.size(); ++p) {
    if ((pages.pages[p].flags & k_page_root) == 0) out.push_back(p);
  }
  return out;
}

// Admits one page at a time and records the order, which is what the priority is about.
Vector<u32> drain(PageResidencyManager& manager) {
  Vector<u32> order;
  u32 next = 0;
  while (manager.next_request(next)) {
    order.push_back(next);
    REQUIRE(manager.admit(1) == 1);
    CHECK(manager.page_residency().is_resident(next));
  }
  return order;
}

}  // namespace

TEST_CASE("cluster pages: requests are served by screen contribution, then distance, then page") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(65, 10.0f, positions, indices);
  ClusterLodMesh lod;
  std::string error;
  REQUIRE(build_cluster_lod(positions, indices, ClusterLodOptions{}, lod, &error));
  ClusterPages pages;
  ClusterPagesOptions options;
  options.page_bytes = 16 * 1024;
  REQUIRE_MESSAGE(build_cluster_pages(lod, options, pages, &error), error);
  const Vector<u32> free_pages = loadable_pages(pages);
  REQUIRE(free_pages.size() >= 5);

  u64 total = 0;
  for (const ClusterPageDesc& page : pages.pages)
    total += page.bytes;

  // Four requests: a big near one, a big far one that projects to the same size, a small far one,
  // and one with no view behind it at all. They are queued in the reverse of the order they
  // should be served in, so arrival order cannot be what produces the answer.
  const PageRequest small_far{free_pages[0], 4.0f, 250.0f};
  const PageRequest unknown{free_pages[1], 0.0f, k_unknown_distance};
  const PageRequest big_far{free_pages[2], 400.0f, 60.0f};
  const PageRequest big_near{free_pages[3], 400.0f, 5.0f};

  PageResidencyManager manager;
  REQUIRE(manager.reset(pages, total, &error));
  manager.request(unknown);
  manager.request(small_far);
  manager.request(big_far);
  manager.request(big_near);
  CHECK(manager.pending() == 4);
  const Vector<u32> order = drain(manager);
  REQUIRE(order.size() == 4);
  CHECK(order[0] == big_near.page);  // the same screen size, nearer: it is wanted for longer
  CHECK(order[1] == big_far.page);
  CHECK(order[2] == small_far.page);
  CHECK(order[3] == unknown.page);  // no view behind it: behind everything that has one
  CHECK(manager.pending() == 0);

  // Deterministic: the same four requests in any order give the same answer, and so does a repeat
  // of the whole run. A request for a page already queued keeps the better of the two.
  PageResidencyManager again;
  REQUIRE(again.reset(pages, total, &error));
  again.request(big_near);
  again.request(big_far);
  again.request(small_far);
  again.request(unknown);
  again.request(PageRequest{small_far.page, 1.0f, 900.0f});  // worse: it must not move
  again.request(PageRequest{unknown.page, 0.0f, k_unknown_distance});
  CHECK(again.pending() == 4);
  const Vector<u32> order_again = drain(again);
  REQUIRE(order_again.size() == order.size());
  bool same = true;
  for (u32 i = 0; i < order.size(); ++i)
    same = same && order[i] == order_again[i];
  CHECK(same);

  // A better priority for a queued page moves it up; a plain request carries none and so leaves
  // page order, which is coarse first and is what the manager did before priorities existed.
  PageResidencyManager plain;
  REQUIRE(plain.reset(pages, total, &error));
  plain.request(free_pages[3]);
  plain.request(free_pages[1]);
  plain.request(free_pages[2]);
  plain.request(free_pages[0]);
  const Vector<u32> plain_order = drain(plain);
  REQUIRE(plain_order.size() == 4);
  for (u32 i = 1; i < plain_order.size(); ++i)
    CHECK(plain_order[i] > plain_order[i - 1]);

  PageResidencyManager promoted;
  REQUIRE(promoted.reset(pages, total, &error));
  promoted.request(free_pages[0]);
  promoted.request(free_pages[1]);
  promoted.request(PageRequest{free_pages[1], 900.0f, 1.0f});  // better: to the front
  u32 next = 0;
  REQUIRE(promoted.next_request(next));
  CHECK(next == free_pages[1]);
}

TEST_CASE("cluster pages: a starved request eventually goes first") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(65, 10.0f, positions, indices);
  ClusterLodMesh lod;
  std::string error;
  REQUIRE(build_cluster_lod(positions, indices, ClusterLodOptions{}, lod, &error));
  ClusterPages pages;
  ClusterPagesOptions options;
  options.page_bytes = 16 * 1024;
  REQUIRE_MESSAGE(build_cluster_pages(lod, options, pages, &error), error);
  const Vector<u32> free_pages = loadable_pages(pages);
  REQUIRE(free_pages.size() >= 8);
  u64 total = 0;
  for (const ClusterPageDesc& page : pages.pages)
    total += page.bytes;

  // One small far page asked for once, and a big near page asked for every frame afterwards while
  // the loader only manages one page a frame. On screen size alone the small one never loads.
  auto run = [&](u32 starvation_frames, u32& served_at) {
    PageResidencyManager manager;
    REQUIRE(manager.reset(pages, total, &error));
    manager.set_starvation_frames(starvation_frames);
    const u32 starved = free_pages[0];
    manager.request(PageRequest{starved, 2.0f, 400.0f});
    served_at = ~u32{0};
    for (u32 frame = 1; frame <= 6; ++frame) {
      manager.begin_frame();
      CHECK(manager.frame_index() == frame);
      manager.request(PageRequest{free_pages[frame], 900.0f, 3.0f});
      u32 next = 0;
      REQUIRE(manager.next_request(next));
      if (next == starved && served_at == ~u32{0}) served_at = frame;
      REQUIRE(manager.admit(1) == 1);
    }
  };

  u32 guarded = 0;
  run(2, guarded);
  CHECK(guarded == 2);  // requested in frame 0, promoted the frame its age reaches the bound

  u32 never = 0;
  run(1000, never);
  CHECK(never == ~u32{0});  // without the guard the small far page is starved for good

  // The whole run is a function of its inputs: the same sequence twice gives the same frame.
  u32 repeat = 0;
  run(2, repeat);
  CHECK(repeat == guarded);
}

TEST_CASE("cluster pages: a request carries the priority of the cluster that made it") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(65, 10.0f, positions, indices);
  ClusterLodMesh lod;
  std::string error;
  REQUIRE(build_cluster_lod(positions, indices, ClusterLodOptions{}, lod, &error));
  ClusterPages pages;
  ClusterPagesOptions options;
  options.page_bytes = 16 * 1024;
  REQUIRE_MESSAGE(build_cluster_pages(lod, options, pages, &error), error);

  // Nothing but the pinned pages: every cluster that wants to refine has to ask.
  PageResidency roots;
  roots.resident.assign(pages.pages.size(), u8{0});
  for (u32 c = 0; c < lod.lod.size(); ++c) {
    if (lod.lod[c].parent_error >= k_lod_terminal_error)
      roots.resident[pages.page_of_cluster[c]] = 1;
  }
  const LodView view = terrain_view(6.0f, 1.0f);
  Vector<u32> cut;
  Vector<u32> plain;
  Vector<u32> detailed_cut;
  Vector<PageRequest> detailed;
  const u32 a = select_lod_streaming(lod, view, pages, roots, cut, plain);
  const u32 b = select_lod_streaming(lod, view, pages, roots, detailed_cut, detailed);
  REQUIRE(a == b);
  REQUIRE(cut.size() == detailed_cut.size());
  bool same_cut = true;
  for (u32 i = 0; i < cut.size(); ++i)
    same_cut = same_cut && cut[i] == detailed_cut[i];
  CHECK(same_cut);
  // The same pages, in the same order, each with a priority a cluster of this view produced.
  REQUIRE(detailed.size() == plain.size());
  REQUIRE_FALSE(detailed.empty());
  for (u32 i = 0; i < detailed.size(); ++i) {
    CHECK(detailed[i].page == plain[i]);
    CHECK(detailed[i].screen_px > 0.0f);
    CHECK(detailed[i].distance >= view.znear);
    CHECK(detailed[i].distance < k_unknown_distance);
    if (i != 0) CHECK(detailed[i].page > detailed[i - 1].page);
  }
  // Pulling the camera back shrinks every request: the priority is the view's, not the page's.
  const LodView far_view = terrain_view(60.0f, 1.0f);
  Vector<u32> far_cut;
  Vector<PageRequest> far_requests;
  select_lod_streaming(lod, far_view, pages, roots, far_cut, far_requests);
  for (const PageRequest& near_request : detailed) {
    for (const PageRequest& far_request : far_requests) {
      if (far_request.page != near_request.page) continue;
      CHECK(far_request.screen_px < near_request.screen_px);
      CHECK(far_request.distance > near_request.distance);
    }
  }
  // And `screen_pixels` is the projection the cut uses, applied to the sphere's own size.
  const Vec4 sphere{0.0f, 0.0f, 0.0f, 2.0f};
  CHECK(screen_pixels(sphere, view) ==
        doctest::Approx(projected_error(sphere, 4.0f, view)).epsilon(1e-6));
  CHECK(sphere_distance(sphere, view) == doctest::Approx(view.camera.y - 2.0f).epsilon(1e-5));

TEST_CASE("cluster pages: a skinned mesh pages its bindings with its vertices, and pays for them") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(65, 10.0f, positions, indices);
  Vector<SkinBinding> skin;
  for (const Vec3& p : positions) {
    const u32 joints[4] = {0, 1, 0, 0};
    const f32 t = (p.x + 10.0f) / 20.0f;
    const f32 weights[4] = {1.0f - t, t, 0.0f, 0.0f};
    skin.push_back(make_skin_binding(joints, weights));
  }
  AttributeSource attributes;
  attributes.skin = std::span<const SkinBinding>(skin.data(), skin.size());
  attributes.joint_count = 2;

  ClusterLodMesh mesh;
  std::string error;
  REQUIRE_MESSAGE(
      build_cluster_lod(positions, indices, ClusterLodOptions{}, mesh, &error, attributes), error);
  ClusterLodMesh plain;
  REQUIRE(build_cluster_lod(positions, indices, ClusterLodOptions{}, plain, &error));

  // Eight more bytes a vertex: a skinned cluster streams its bindings or it cannot be deformed.
  const u32 skinned_bytes = cluster_page_bytes(mesh.mesh, 0);
  const u32 plain_bytes = cluster_page_bytes(plain.mesh, 0);
  CHECK(skinned_bytes == plain_bytes + mesh.mesh.clusters[0].vertex_count * 8u);

  // The bindings are permuted with the vertices they belong to, so a page's bindings are one run
  // of the stream exactly as its positions are. `vertex_source` is permuted alongside, so a
  // vertex's binding is still its source vertex's.
  const Vector<SkinBinding> before = mesh.mesh.skin;
  const Vector<u32> before_source = mesh.mesh.vertex_source;
  ClusterPages pages;
  Vector<u32> source_of_cluster;
  ClusterPagesOptions options;
  options.page_bytes = 64 * 1024;
  REQUIRE_MESSAGE(build_cluster_pages(mesh, options, pages, &error, &source_of_cluster), error);
  REQUIRE(mesh.mesh.skin.size() == mesh.mesh.vertices.size());
  CHECK_MESSAGE(validate_cluster_lod(mesh, indices, &error), error);
  for (u32 v = 0; v < mesh.mesh.skin.size(); ++v) {
    const SkinBinding& want = skin[mesh.mesh.vertex_source[v]];
    for (u32 k = 0; k < 4; ++k) {
      CHECK(mesh.mesh.skin[v].joints[k] == want.joints[k]);
      CHECK(mesh.mesh.skin[v].weights[k] == want.weights[k]);
    }
  }
  // The permutation moved something: a stream identical to the one before would prove nothing.
  bool moved = false;
  for (u32 v = 0; v < before_source.size() && !moved; ++v)
    moved = before_source[v] != mesh.mesh.vertex_source[v];
  CHECK(moved);
  CHECK(before.size() == mesh.mesh.skin.size());
  MESSAGE("skinned cluster 0: " << skinned_bytes << " bytes against " << plain_bytes
                                << " rigid, over " << pages.pages.size() << " pages");
}
