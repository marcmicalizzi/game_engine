#include <domain/geometry/cluster_pages.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace engine::geometry {

namespace {

constexpr u32 k_no_index = ~u32{0};

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

// A group's identity, as the DAG stores it: the level of its clusters and the bits of the bound
// its simplification produced. Every member of group g carries the bound as `parent`/
// `parent_error`, and every cluster produced by simplifying g carries the same bits as `own`/
// `own_error` — which is the only way back from a cluster to the group it refines into, because
// clusterlod's `refined` index is not in the format. Bits, not values: the two are copies of one
// float, so they compare exactly.
//
// The level is in the key and not just along for the ride. When a simplification step costs no
// error at all — a flat panel, a piece of glass — clusterlod carries the group's bound and error
// through to the next level unchanged, so the bound alone is ambiguous between a group and the
// one above it. A cluster at level L is always produced from a group at level L - 1 (each round
// of the builder partitions only the clusters the round before it made), so the level settles it.
struct GroupKey {
  u32 bits[6] = {0, 0, 0, 0, 0, 0};
};

GroupKey bound_key(u32 level, Vec4 sphere, f32 error) noexcept {
  GroupKey key;
  key.bits[0] = level;
  const f32 values[5] = {sphere.x, sphere.y, sphere.z, sphere.w, error};
  for (u32 i = 0; i < 5; ++i)
    std::memcpy(&key.bits[i + 1], &values[i], sizeof(f32));
  return key;
}

int compare_keys(const GroupKey& a, const GroupKey& b) noexcept {
  for (u32 i = 0; i < 6; ++i) {
    if (a.bits[i] != b.bits[i]) return a.bits[i] < b.bits[i] ? -1 : 1;
  }
  return 0;
}

// One group in the lookup table above, with the group index as the tie-breaker so the sort is a
// total order and the layout does not depend on the sort's internals.
struct GroupEntry {
  GroupKey key;
  u32 group = 0;
};

bool entry_less(const GroupEntry& a, const GroupEntry& b) noexcept {
  const int order = compare_keys(a.key, b.key);
  return order != 0 ? order < 0 : a.group < b.group;
}

bool key_less(const GroupEntry& a, const GroupEntry& b) noexcept {
  return compare_keys(a.key, b.key) < 0;
}

// Where each group's clusters are, given that a paged mesh keeps a group contiguous. Fails when
// a group is split or spans more than one level: that means the mesh was not laid out in pages
// (or was reordered afterwards), and every rule below would be reading the wrong clusters.
bool group_runs(std::span<const ClusterLodDesc> lod, u32 group_count, Vector<ClusterChildren>& out,
                std::string* error) {
  out.assign(group_count, ClusterChildren{});
  for (u32 i = 0; i < lod.size(); ++i) {
    if (lod[i].group >= group_count) {
      return fail(error, "cluster " + std::to_string(i) + " names group " +
                             std::to_string(lod[i].group) + ", outside the " +
                             std::to_string(group_count) + " groups");
    }
    ClusterChildren& run = out[lod[i].group];
    if (run.cluster_count == 0) {
      run.first_cluster = i;
      run.cluster_count = 1;
      continue;
    }
    if (run.first_cluster + run.cluster_count != i) {
      return fail(error, "group " + std::to_string(lod[i].group) +
                             " is not one run of clusters: the mesh is not laid out in pages");
    }
    if (lod[run.first_cluster].level != lod[i].level)
      return fail(error,
                  "group " + std::to_string(lod[i].group) + " spans more than one DAG level");
    ++run.cluster_count;
  }
  return true;
}

// The clusters each cluster refines into: the members of the group whose simplified bound is this
// cluster's `own` bound. Level-0 clusters are original geometry and have none. If two groups ever
// carried the same bound bits, the range would cover both, which asks for more pages than needed
// and never for fewer — the safe direction.
bool match_children(std::span<const ClusterLodDesc> lod, std::span<const ClusterChildren> runs,
                    Vector<ClusterChildren>& out, std::string* error) {
  Vector<GroupEntry> table;
  table.reserve(static_cast<u32>(runs.size()));
  for (u32 g = 0; g < runs.size(); ++g) {
    if (runs[g].cluster_count == 0) continue;
    const ClusterLodDesc& any = lod[runs[g].first_cluster];
    GroupEntry entry;
    entry.key = bound_key(any.level, any.parent, any.parent_error);
    entry.group = g;
    table.push_back(entry);
  }
  std::sort(table.begin(), table.end(), entry_less);

  out.assign(static_cast<u32>(lod.size()), ClusterChildren{});
  for (u32 i = 0; i < lod.size(); ++i) {
    if (lod[i].level == 0) continue;  // original geometry: nothing finer exists
    GroupEntry wanted;
    wanted.key = bound_key(lod[i].level - 1, lod[i].own, lod[i].own_error);
    u32 low = k_no_index;
    u32 high = 0;
    for (const GroupEntry* it = std::lower_bound(table.begin(), table.end(), wanted, key_less);
         it != table.end() && compare_keys(it->key, wanted.key) == 0; ++it) {
      const ClusterChildren& run = runs[it->group];
      if (run.first_cluster < low) low = run.first_cluster;
      const u32 end = run.first_cluster + run.cluster_count;
      high = end > high ? end : high;
    }
    if (low == k_no_index) {
      return fail(error, "cluster " + std::to_string(i) + " at level " +
                             std::to_string(lod[i].level) +
                             " refines a group that is not in the mesh");
    }
    out[i] = ClusterChildren{low, high - low};
  }
  return true;
}

// The pages holding a run of clusters, which is a range of pages because the builder keeps a
// group in consecutive pages. Returns false when any of them is missing, and appends the missing
// ones to `missing`.
bool run_resident(const ClusterPages& pages, const PageResidency& residency, u32 first, u32 count,
                  Vector<u32>* missing) {
  if (count == 0) return true;
  const u32 from = pages.page_of_cluster[first];
  const u32 to = pages.page_of_cluster[first + count - 1];
  bool complete = true;
  for (u32 page = from; page <= to; ++page) {
    if (residency.is_resident(page)) continue;
    complete = false;
    if (missing != nullptr) missing->push_back(page);
  }
  return complete;
}

// Sorts and removes the repeats from the run of `values` appended since `from`.
void sort_unique_tail(Vector<u32>& values, u32 from) {
  if (values.size() <= from) return;
  std::sort(values.begin() + from, values.end());
  u32* last = std::unique(values.begin() + from, values.end());
  values.resize(static_cast<u32>(last - values.begin()));
}

}  // namespace

u32 cluster_page_bytes(const ClusterMesh& mesh, u32 cluster) noexcept {
  if (cluster >= mesh.clusters.size()) return 0;
  const ClusterDesc& desc = mesh.clusters[cluster];
  // Six bytes of quantized position and eight of attributes a vertex, four bytes a triangle, and
  // the two descriptors. The float positions are not counted: only the acceleration-structure
  // builders read those, and they are not what a page streams.
  const u32 per_vertex = 6u + (mesh.attributes.size() == mesh.vertices.size() ? 8u : 0u);
  const u32 fixed = static_cast<u32>(sizeof(ClusterDesc) + sizeof(ClusterLodDesc));
  return fixed + desc.vertex_count * per_vertex + desc.triangle_count * 4u;
}

void permute_cluster_array(std::span<const u32> source_of_cluster, Vector<u32>& values) {
  if (values.size() != source_of_cluster.size()) return;
  Vector<u32> moved(values.size());
  for (u32 i = 0; i < values.size(); ++i)
    moved[i] = values[source_of_cluster[i]];
  values = std::move(moved);
}

bool build_cluster_pages(ClusterLodMesh& mesh, const ClusterPagesOptions& options,
                         ClusterPages& out, std::string* error, Vector<u32>* source_of_cluster) {
  out = ClusterPages{};
  if (source_of_cluster != nullptr) source_of_cluster->clear();
  ClusterMesh& geo = mesh.mesh;
  const u32 count = geo.clusters.size();
  if (count == 0 || mesh.lod.size() != count)
    return fail(error, "build_cluster_pages: the LOD table does not match the clusters");
  if (options.page_bytes == 0)
    return fail(error, "build_cluster_pages: page_bytes must be at least one byte");
  if (mesh.level_cluster_counts.empty())
    return fail(error, "build_cluster_pages: the mesh has no DAG levels");
  const u32 level_count = mesh.level_cluster_counts.size();
  const u32 group_count = mesh.group_count;
  if (group_count == 0) return fail(error, "build_cluster_pages: the mesh has no groups");
  for (u32 i = 0; i < count; ++i) {
    if (mesh.lod[i].group >= group_count) {
      return fail(error, "build_cluster_pages: cluster " + std::to_string(i) +
                             " names a group outside the mesh");
    }
    if (mesh.lod[i].level >= level_count) {
      return fail(error, "build_cluster_pages: cluster " + std::to_string(i) +
                             " names a level outside the mesh");
    }
  }
  if (geo.vertex_source.size() != geo.vertices.size())
    return fail(error, "build_cluster_pages: the vertex source stream is the wrong length");
  const bool has_attributes = geo.attributes.size() == geo.vertices.size();
  const bool has_quantized = geo.quantized.size() >= u64{geo.vertices.size()} * 3;
  if (!geo.quantized.empty() && !has_quantized)
    return fail(error, "build_cluster_pages: the quantized stream is the wrong length");

  // Reordering the clusters reorders their runs of the vertex and triangle streams, which only
  // means anything when those runs tile the streams: one run per cluster, in order, no gaps and
  // nothing shared. Every builder here produces exactly that; a mesh that does not is refused
  // rather than quietly rewritten into one that has lost vertices.
  Vector<u32> order(count);
  for (u32 i = 0; i < count; ++i)
    order[i] = i;
  auto tiles = [&](bool over_vertices) {
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
      return over_vertices ? geo.clusters[a].vertex_offset < geo.clusters[b].vertex_offset
                           : geo.clusters[a].triangle_offset < geo.clusters[b].triangle_offset;
    });
    u64 at = 0;
    for (const u32 c : order) {
      const ClusterDesc& desc = geo.clusters[c];
      const u64 offset = over_vertices ? desc.vertex_offset : desc.triangle_offset;
      if (offset != at) return false;
      at += over_vertices ? desc.vertex_count : desc.triangle_count;
    }
    return at == (over_vertices ? geo.vertices.size() : geo.triangles.size());
  };
  if (!tiles(true))
    return fail(error, "build_cluster_pages: the clusters do not tile the vertex stream");
  if (!tiles(false))
    return fail(error, "build_cluster_pages: the clusters do not tile the triangle stream");

  // The clusters of each group, by a counting sort, so the order inside a group is the mesh's own
  // and the whole layout is a function of the input alone.
  Vector<u32> group_start(group_count + 1, 0u);
  for (u32 i = 0; i < count; ++i)
    ++group_start[mesh.lod[i].group + 1];
  for (u32 g = 0; g < group_count; ++g)
    group_start[g + 1] += group_start[g];
  Vector<u32> fill_at(group_count, 0u);
  for (u32 g = 0; g < group_count; ++g)
    fill_at[g] = group_start[g];
  Vector<u32> group_clusters(count);
  for (u32 i = 0; i < count; ++i)
    group_clusters[fill_at[mesh.lod[i].group]++] = i;

  Vector<u32> group_level(group_count, 0u);
  Vector<u8> group_terminal(group_count, u8{0});
  for (u32 g = 0; g < group_count; ++g) {
    if (group_start[g + 1] == group_start[g]) continue;
    const ClusterLodDesc& first_member = mesh.lod[group_clusters[group_start[g]]];
    const u32 level = first_member.level;
    for (u32 k = group_start[g]; k < group_start[g + 1]; ++k) {
      if (mesh.lod[group_clusters[k]].level != level) {
        return fail(error, "build_cluster_pages: group " + std::to_string(g) +
                               " spans more than one DAG level");
      }
    }
    group_level[g] = level;
    // A group whose simplification never happened — the root, or one the simplifier got stuck on.
    // Every member carries the group's simplified error, so one of them settles it.
    group_terminal[g] = first_member.parent_error >= k_lod_terminal_error ? u8{1} : u8{0};
  }

  // The order the pages are filled in, and so the order a viewer streams them in:
  //
  //   1. every group with no coarser version — the root of the DAG and every group the
  //      simplifier got stuck on — coarsest level first;
  //   2. everything else, coarsest level first;
  //   3. by group index inside a level, which is only there to make the result deterministic.
  //
  // The first block is the mesh's *minimum resident set*: nothing else can stand in for it, so a
  // viewer that has it can draw the whole mesh, coarsely, and everything after it is refinement.
  // Putting it first is what makes "a prefix of the pages is a complete picture" true for a mesh
  // whose DAG has more than one root, which is every mesh built from several primitives or with
  // a patch the simplifier could not merge. It also keeps the pages the residency manager must
  // pin at the front, so pinning one never strands the pages above it.
  //
  // It does not disturb the property everything else rests on — a cluster's children are always
  // later in the array — because a group that produced parents was simplified, so a cluster's
  // children are never a terminal group and always land in block 2 at a finer level.
  const u32 bucket_count = 2 * level_count;
  Vector<u32> bucket_start(bucket_count + 1, 0u);
  auto bucket_of = [&](u32 g) {
    return (group_terminal[g] != 0 ? 0u : level_count) + (level_count - 1 - group_level[g]);
  };
  for (u32 g = 0; g < group_count; ++g) {
    if (group_start[g + 1] == group_start[g]) continue;
    ++bucket_start[bucket_of(g) + 1];
  }
  for (u32 b = 0; b < bucket_count; ++b)
    bucket_start[b + 1] += bucket_start[b];
  Vector<u32> group_order(bucket_start[bucket_count]);
  Vector<u32> bucket_fill(bucket_count, 0u);
  for (u32 b = 0; b < bucket_count; ++b)
    bucket_fill[b] = bucket_start[b];
  for (u32 g = 0; g < group_count; ++g) {
    if (group_start[g + 1] == group_start[g]) continue;
    group_order[bucket_fill[bucket_of(g)]++] = g;
  }

  // Fill pages group by group. A group only ever straddles a page boundary when it does not fit
  // in a page at all, because a cut refines a whole group at once: splitting one would make a
  // camera move ask for two pages where it could have asked for one.
  const u32 target = options.page_bytes;
  Vector<u32> new_to_old;
  new_to_old.reserve(count);
  Vector<u32> group_new(group_count, k_no_index);
  Vector<ClusterChildren> runs(group_order.size());
  ClusterPageDesc page;
  bool page_open = false;
  u32 page_bytes = 0;
  auto close_page = [&]() {
    page.cluster_count = new_to_old.size() - page.first_cluster;
    page.bytes = page_bytes;
    out.pages.push_back(page);
    page_bytes = 0;
    page_open = false;
  };
  for (u32 rank = 0; rank < group_order.size(); ++rank) {
    const u32 g = group_order[rank];
    const u32 first = group_start[g];
    const u32 members = group_start[g + 1] - first;
    u32 group_bytes = 0;
    for (u32 k = 0; k < members; ++k)
      group_bytes += cluster_page_bytes(geo, group_clusters[first + k]);
    if (page_open && page_bytes + group_bytes > target) close_page();
    if (!page_open) {
      page = ClusterPageDesc{};
      page.first_cluster = new_to_old.size();
      page.level_min = group_level[g];
      page.level_max = group_level[g];
      page_open = true;
    }
    page.level_min = group_level[g] < page.level_min ? group_level[g] : page.level_min;
    page.level_max = group_level[g] > page.level_max ? group_level[g] : page.level_max;
    group_new[g] = rank;
    runs[rank] = ClusterChildren{new_to_old.size(), members};
    for (u32 k = 0; k < members; ++k)
      new_to_old.push_back(group_clusters[first + k]);
    page_bytes += group_bytes;
    if (group_bytes > target) {
      // A group that does not fit gets a page of its own, over the target and flagged as such.
      page.flags |= k_page_oversized;
      close_page();
    }
  }
  if (page_open) close_page();
  // A group with no clusters cannot come out of a builder, but keeping `group_count` meaningful
  // costs two lines and keeps every index inside the table it names.
  u32 next_group = group_order.size();
  for (u32 g = 0; g < group_count; ++g) {
    if (group_new[g] == k_no_index) group_new[g] = next_group++;
  }
  runs.resize(group_count, ClusterChildren{});

  // The permutation, applied to the descriptors and to the streams so that a page's clusters, its
  // vertices, and its triangles are each one run. Nothing is written back to `mesh` until the
  // child table below has been matched, so a failure leaves the caller's mesh as it was.
  Vector<ClusterDesc> clusters(count);
  Vector<ClusterLodDesc> lods(count);
  Vector<Vec3> vertices;
  Vector<u32> vertex_source;
  Vector<VertexAttributes> attributes;
  Vector<u16> quantized;
  Vector<u32> triangles;
  vertices.reserve(geo.vertices.size());
  vertex_source.reserve(geo.vertices.size());
  if (has_attributes) attributes.reserve(geo.vertices.size());
  if (has_quantized) quantized.reserve(geo.quantized.size());
  triangles.reserve(geo.triangles.size());
  for (u32 n = 0; n < count; ++n) {
    const u32 from = new_to_old[n];
    ClusterDesc desc = geo.clusters[from];
    const u32 first_vertex = desc.vertex_offset;
    const u32 first_triangle = desc.triangle_offset;
    desc.vertex_offset = vertices.size();
    desc.triangle_offset = triangles.size();
    for (u32 v = 0; v < desc.vertex_count; ++v) {
      const u32 source = first_vertex + v;
      vertices.push_back(geo.vertices[source]);
      vertex_source.push_back(geo.vertex_source[source]);
      if (has_attributes) attributes.push_back(geo.attributes[source]);
      if (has_quantized) {
        quantized.push_back(geo.quantized[source * 3 + 0]);
        quantized.push_back(geo.quantized[source * 3 + 1]);
        quantized.push_back(geo.quantized[source * 3 + 2]);
      }
    }
    for (u32 t = 0; t < desc.triangle_count; ++t)
      triangles.push_back(geo.triangles[first_triangle + t]);
    clusters[n] = desc;
    ClusterLodDesc lod = mesh.lod[from];
    lod.group = group_new[lod.group];
    lods[n] = lod;
  }
  // The shaders read the last position triple as two whole 32-bit words.
  if (has_quantized && quantized.size() % 2 != 0) quantized.push_back(0);

  // The stream ranges, the levels, and the per-cluster page each page ended up with.
  out.page_bytes_target = target;
  out.page_of_cluster.assign(count, 0u);
  for (u32 p = 0; p < out.pages.size(); ++p) {
    ClusterPageDesc& desc = out.pages[p];
    desc.first_vertex = clusters[desc.first_cluster].vertex_offset;
    desc.first_triangle = clusters[desc.first_cluster].triangle_offset;
    desc.vertex_count = 0;
    desc.triangle_count = 0;
    for (u32 k = 0; k < desc.cluster_count; ++k) {
      const u32 c = desc.first_cluster + k;
      out.page_of_cluster[c] = p;
      desc.vertex_count += clusters[c].vertex_count;
      desc.triangle_count += clusters[c].triangle_count;
      // Nothing coarser can stand in for a cluster whose group could not be simplified — the
      // root, or a group the simplifier got stuck on — so the page it is in never goes away.
      if (lods[c].parent_error >= k_lod_terminal_error) desc.flags |= k_page_root;
    }
  }

  // Every cluster's children, and then every page's child pages: the union of its clusters'
  // ranges, ascending and without repeats, which is what a prefetch of "one level finer than this
  // page" reads.
  const std::span<const ClusterLodDesc> lod_view(lods.data(), lods.size());
  if (!match_children(lod_view, std::span<const ClusterChildren>(runs.data(), runs.size()),
                      out.children, error)) {
    out = ClusterPages{};
    return false;
  }
  for (u32 p = 0; p < out.pages.size(); ++p) {
    ClusterPageDesc& desc = out.pages[p];
    const u32 at = out.child_pages.size();
    desc.first_child_page = at;
    for (u32 k = 0; k < desc.cluster_count; ++k) {
      const ClusterChildren& kids = out.children[desc.first_cluster + k];
      if (kids.cluster_count == 0) continue;
      const u32 from = out.page_of_cluster[kids.first_cluster];
      const u32 to = out.page_of_cluster[kids.first_cluster + kids.cluster_count - 1];
      for (u32 child = from; child <= to; ++child) {
        // A page that holds two levels is its own child; saying so would only make it look like
        // something depends on it from outside, and eviction reads this list for exactly that.
        if (child != p) out.child_pages.push_back(child);
      }
    }
    sort_unique_tail(out.child_pages, at);
    desc.child_page_count = out.child_pages.size() - at;
  }

  geo.clusters = std::move(clusters);
  geo.vertices = std::move(vertices);
  geo.vertex_source = std::move(vertex_source);
  if (has_attributes) geo.attributes = std::move(attributes);
  if (has_quantized) geo.quantized = std::move(quantized);
  geo.triangles = std::move(triangles);
  mesh.lod = std::move(lods);
  if (source_of_cluster != nullptr) *source_of_cluster = std::move(new_to_old);
  return true;
}

bool rebuild_cluster_page_index(const ClusterLodMesh& mesh, ClusterPages& pages,
                                std::string* error) {
  pages.page_of_cluster.clear();
  pages.children.clear();
  if (pages.pages.empty()) return true;
  const u32 count = mesh.mesh.clusters.size();
  if (mesh.lod.size() != count)
    return fail(error, "cluster pages: the LOD table does not match the clusters");
  pages.page_of_cluster.assign(count, 0u);
  u32 at = 0;
  for (u32 p = 0; p < pages.pages.size(); ++p) {
    const ClusterPageDesc& desc = pages.pages[p];
    if (desc.first_cluster != at || u64{desc.first_cluster} + desc.cluster_count > count) {
      return fail(error, "cluster page " + std::to_string(p) +
                             " does not continue the cluster array where the page before it ended");
    }
    for (u32 k = 0; k < desc.cluster_count; ++k)
      pages.page_of_cluster[at + k] = p;
    at += desc.cluster_count;
  }
  if (at != count) {
    return fail(error, "cluster pages cover " + std::to_string(at) + " of " +
                           std::to_string(count) + " clusters");
  }
  const std::span<const ClusterLodDesc> lod_view(mesh.lod.data(), mesh.lod.size());
  Vector<ClusterChildren> runs;
  if (!group_runs(lod_view, mesh.group_count, runs, error)) return false;
  return match_children(lod_view, std::span<const ClusterChildren>(runs.data(), runs.size()),
                        pages.children, error);
}

bool validate_cluster_pages(const ClusterLodMesh& mesh, const ClusterPages& pages,
                            std::string* error) {
  const u32 count = mesh.mesh.clusters.size();
  if (pages.pages.empty()) return fail(error, "no pages");
  if (pages.page_of_cluster.size() != count || pages.children.size() != count)
    return fail(error, "the per-cluster page and child tables are the wrong length");
  // The DAG first: children are finer, inside the mesh, and later in the array, which is what
  // lets one forward pass walk a cut and lets eviction decide a page is free by looking only at
  // the pages that name it as a child.
  for (u32 c = 0; c < count; ++c) {
    const ClusterChildren& kids = pages.children[c];
    if (kids.cluster_count == 0) {
      if (mesh.lod[c].level != 0)
        return fail(error, "cluster " + std::to_string(c) + " above level 0 has no children");
      continue;
    }
    if (u64{kids.first_cluster} + kids.cluster_count > count)
      return fail(error, "cluster " + std::to_string(c) + "'s children are outside the mesh");
    if (kids.first_cluster <= c) {
      return fail(error, "cluster " + std::to_string(c) + " at level " +
                             std::to_string(mesh.lod[c].level) + " has children at " +
                             std::to_string(kids.first_cluster) + ".." +
                             std::to_string(kids.first_cluster + kids.cluster_count) +
                             ", which is not after it");
    }
    for (u32 k = 0; k < kids.cluster_count; ++k) {
      if (mesh.lod[kids.first_cluster + k].level >= mesh.lod[c].level)
        return fail(error, "cluster " + std::to_string(c) + " has a child that is not finer");
    }
  }
  u32 cluster_at = 0;
  u32 vertex_at = 0;
  u32 triangle_at = 0;
  u32 previous_level = k_no_index;
  bool past_the_roots = false;
  for (u32 p = 0; p < pages.pages.size(); ++p) {
    const ClusterPageDesc& desc = pages.pages[p];
    const std::string where = "page " + std::to_string(p);
    if (desc.cluster_count == 0) return fail(error, where + " is empty");
    if (desc.first_cluster != cluster_at || desc.first_vertex != vertex_at ||
        desc.first_triangle != triangle_at) {
      return fail(error, where + " does not continue the ranges of the page before it");
    }
    u32 vertices = 0;
    u32 triangles = 0;
    u32 bytes = 0;
    u32 level_min = k_no_index;
    u32 level_max = 0;
    for (u32 k = 0; k < desc.cluster_count; ++k) {
      const u32 c = desc.first_cluster + k;
      if (c >= count) return fail(error, where + " runs past the last cluster");
      if (pages.page_of_cluster[c] != p)
        return fail(error, "cluster " + std::to_string(c) + " is not in " + where);
      const ClusterDesc& cluster = mesh.mesh.clusters[c];
      if (cluster.vertex_offset != vertex_at + vertices ||
          cluster.triangle_offset != triangle_at + triangles) {
        return fail(error,
                    "cluster " + std::to_string(c) + " does not continue " + where + "'s streams");
      }
      vertices += cluster.vertex_count;
      triangles += cluster.triangle_count;
      bytes += cluster_page_bytes(mesh.mesh, c);
      const u32 level = mesh.lod[c].level;
      level_min = level < level_min ? level : level_min;
      level_max = level > level_max ? level : level_max;
    }
    if (desc.vertex_count != vertices || desc.triangle_count != triangles || desc.bytes != bytes)
      return fail(error, where + " does not add up to its clusters");
    if (desc.level_min != level_min || desc.level_max != level_max)
      return fail(error, where + " does not name the levels of its clusters");
    if (desc.bytes > pages.page_bytes_target && (desc.flags & k_page_oversized) == 0)
      return fail(error, where + " is over the byte target and is not flagged oversized");
    if ((desc.flags & k_page_oversized) != 0 && desc.bytes <= pages.page_bytes_target)
      return fail(error, where + " is flagged oversized and is within the byte target");
    // The root pages come first — they are the minimum resident set — and within that block and
    // within the refinement that follows it, a page never holds anything coarser than the page
    // before it.
    const bool root = (desc.flags & k_page_root) != 0;
    if (root && past_the_roots)
      return fail(error, where +
                             " holds a group with no coarser version after a page that does "
                             "not, so the pinned pages are not a prefix");
    if (!root && !past_the_roots) {
      past_the_roots = true;
      previous_level = k_no_index;  // the refinement block starts its own coarse-to-fine run
    }
    if (previous_level != k_no_index && desc.level_max > previous_level)
      return fail(error, where + " is coarser than the page before it");
    previous_level = desc.level_max;
    if (u64{desc.first_child_page} + desc.child_page_count > pages.child_pages.size())
      return fail(error, where + "'s child page run is outside the child page list");
    for (u32 k = 0; k < desc.child_page_count; ++k) {
      const u32 child = pages.child_pages[desc.first_child_page + k];
      if (child >= pages.pages.size())
        return fail(error, where + " names a child page that is not there");
      // Children are finer, so they are later in the array; a page is never its own child.
      if (child <= p) {
        return fail(error, where + " names page " + std::to_string(child) +
                               " as a child, which is not after it");
      }
      if (k != 0 && child <= pages.child_pages[desc.first_child_page + k - 1])
        return fail(error, where + "'s child pages are not ascending and unique");
    }
    cluster_at += desc.cluster_count;
    vertex_at += vertices;
    triangle_at += triangles;
  }
  if (cluster_at != count || vertex_at != mesh.mesh.vertices.size() ||
      triangle_at != mesh.mesh.triangles.size()) {
    return fail(error, "the pages do not cover the clusters, the vertices, and the triangles");
  }
  if ((pages.pages[0].flags & k_page_root) == 0)
    return fail(error, "page 0 does not hold the coarsest level");
  return true;
}

u32 select_lod_streaming(const ClusterLodMesh& mesh, const LodView& view, const ClusterPages& pages,
                         const PageResidency& residency, Vector<u32>& cut,
                         Vector<u32>& page_requests) {
  const u32 count = mesh.lod.size();
  if (pages.pages.empty() || pages.page_of_cluster.size() != count ||
      pages.children.size() != count) {
    return select_lod(mesh, view, cut);  // no page table: nothing can be missing
  }
  const u32 requests_at = page_requests.size();
  u32 selected = 0;
  u32 i = 0;
  while (i < count) {
    u32 j = i + 1;
    while (j < count && mesh.lod[j].group == mesh.lod[i].group)
      ++j;
    // The residency test is over the whole group, not over the cluster: the parent that would be
    // drawn instead takes the same decision over the same group, so the two never disagree and
    // the cut stays crack-free even where a group had to be split over two pages.
    if (!run_resident(pages, residency, i, j - i, nullptr)) {
      // A terminal group has no parent to ask for it, so it asks for itself: without the coarsest
      // level there is nothing to draw at all.
      if (mesh.lod[i].parent_error >= k_lod_terminal_error)
        run_resident(pages, residency, i, j - i, &page_requests);
      i = j;
      continue;
    }
    for (u32 c = i; c < j; ++c) {
      const ClusterLodDesc& lod = mesh.lod[c];
      if (projected_error(lod.parent, lod.parent_error, view) <= view.threshold_px) continue;
      if (projected_error(lod.own, lod.own_error, view) <= view.threshold_px) {
        cut.push_back(c);
        ++selected;
        continue;
      }
      // Too coarse for this view: refine into the children, unless their pages are not all here,
      // in which case this cluster is the best there is and the missing pages are asked for.
      const ClusterChildren& kids = pages.children[c];
      const u32 before = page_requests.size();
      if (!run_resident(pages, residency, kids.first_cluster, kids.cluster_count, &page_requests)) {
        cut.push_back(c);
        ++selected;
      } else {
        page_requests.resize(before);
      }
    }
    i = j;
  }
  sort_unique_tail(page_requests, requests_at);
  return selected;
}

bool PageResidencyManager::reset(const ClusterPages& table, u64 budget, std::string* error) {
  bytes_.clear();
  used_.clear();
  root_.clear();
  child_first_.clear();
  child_count_.clear();
  child_pages_.clear();
  queued_.clear();
  queue_.clear();
  residency_.resident.clear();
  queue_head_ = 0;
  resident_pages_ = 0;
  resident_bytes_ = 0;
  total_bytes_ = 0;
  budget_ = budget;
  frame_ = 0;
  if (table.pages.empty()) return fail(error, "page residency: the mesh has no pages");
  const u32 count = table.pages.size();
  bytes_.resize(count);
  used_.assign(count, 0ull);
  root_.assign(count, u8{0});
  child_first_.resize(count);
  child_count_.resize(count);
  child_pages_ = table.child_pages;
  queued_.assign(count, u8{0});
  residency_.resident.assign(count, u8{0});
  for (u32 p = 0; p < count; ++p) {
    bytes_[p] = table.pages[p].bytes;
    root_[p] = (table.pages[p].flags & k_page_root) != 0 ? u8{1} : u8{0};
    child_first_[p] = table.pages[p].first_child_page;
    child_count_[p] = table.pages[p].child_page_count;
    total_bytes_ += table.pages[p].bytes;
  }
  // The root pages are resident from the start and stay, whatever the budget says: nothing
  // coarser can draw what they hold, and nothing above them would ever ask for them.
  for (u32 p = 0; p < count; ++p) {
    if (root_[p] != 0) admit_one(p);
  }
  return true;
}

bool PageResidencyManager::may_evict(u32 page) const noexcept {
  if (residency_.resident[page] == 0 || root_[page] != 0) return false;
  // Ancestor-closure: while anything finer that this page's clusters refine into is still here,
  // taking this page away would leave that finer geometry drawing under a hole.
  for (u32 k = 0; k < child_count_[page]; ++k) {
    if (residency_.resident[child_pages_[child_first_[page] + k]] != 0) return false;
  }
  return true;
}

void PageResidencyManager::admit_one(u32 page) {
  if (page >= residency_.resident.size() || residency_.resident[page] != 0) return;
  residency_.resident[page] = 1;
  used_[page] = frame_;
  resident_bytes_ += bytes_[page];
  ++resident_pages_;
  queued_[page] = 0;
}

void PageResidencyManager::touch(u32 page) noexcept {
  if (page < residency_.resident.size() && residency_.resident[page] != 0) used_[page] = frame_;
}

void PageResidencyManager::request(u32 page) {
  if (page >= residency_.resident.size()) return;
  if (residency_.resident[page] != 0) {
    used_[page] = frame_;
    return;
  }
  if (queued_[page] != 0) return;
  queued_[page] = 1;
  queue_.push_back(page);
}

void PageResidencyManager::request(std::span<const u32> list) {
  for (const u32 page : list)
    request(page);
}

u32 PageResidencyManager::admit(u32 max_pages) {
  u32 loaded = 0;
  while (loaded < max_pages && queue_head_ < queue_.size()) {
    const u32 page = queue_[queue_head_++];
    if (queued_[page] == 0) continue;  // it became resident some other way
    admit_one(page);
    ++loaded;
  }
  if (queue_head_ == queue_.size()) {
    queue_.clear();
    queue_head_ = 0;
  }
  return loaded;
}

u32 PageResidencyManager::evict_to_budget() {
  // One page at a time, because taking one away is what makes the page above it evictable: the
  // set peels from the fine end inward. Least recently used first, ties by page index, so two
  // pages last touched in the same frame always go in the same order and a run is reproducible.
  // A full scan per eviction is fine for the CPU model — the GPU manager will keep an
  // eviction heap instead.
  u32 evicted = 0;
  while (resident_bytes_ > budget_) {
    u32 victim = k_no_index;
    for (u32 p = 0; p < residency_.resident.size(); ++p) {
      if (!may_evict(p)) continue;
      if (victim == k_no_index || used_[p] < used_[victim]) victim = p;
    }
    if (victim == k_no_index) break;  // everything left is a root or has something under it
    residency_.resident[victim] = 0;
    resident_bytes_ -= bytes_[victim];
    --resident_pages_;
    ++evicted;
  }
  return evicted;
}

}  // namespace engine::geometry
