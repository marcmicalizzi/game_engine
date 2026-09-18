#pragma once

// Fixed-size cluster pages (docs/plan/04-renderer.md §4.3 step 3 and §4.9): the unit the
// renderer streams clusters in. A page is a contiguous run of clusters, of a target size
// (~128 KB) counted over exactly the streams the GPU reads for a cluster — its `ClusterDesc`,
// its `ClusterLodDesc`, its quantized positions, its attributes, and its packed triangles —
// and pages are laid out **coarse to fine**, the root in page 0, so that a viewer who has the
// first n pages has a complete, if coarse, picture of the whole mesh rather than a detailed
// picture of part of it.
//
// Within a level the clusters are packed **group by group**. A group is the set of siblings a
// LOD cut refines into together (`ClusterLodDesc::group`): they share one parent error, so they
// enter and leave a cut as one. Keeping a group in one page means a cut that moves touches as
// few pages as it can, and it is also what makes the drawing rule below exact rather than
// conservative. A page exceeds the byte target only when a single group (or a single cluster)
// does; it is flagged when that happens.
//
// `build_cluster_pages` therefore **renumbers** the mesh's clusters, and rewrites everything
// that names one: the cluster and LOD descriptors, the vertex, attribute, quantized, and
// triangle streams (so a page's ranges are contiguous there too), and the group indices. A
// caller with per-cluster arrays of its own (a material per cluster, `part_of_cluster` from
// `merge_cluster_lod`) asks for the permutation and runs `permute_cluster_array` over them.
//
// Drawing with pages missing, the rule `select_lod_streaming` implements:
//
//   draw c  <=>  resident(group of c)                      // never draw out of a missing page
//                && projected parent error > threshold     // the coarser version is too coarse
//                && (projected own error <= threshold      // c itself is fine enough
//                    || !resident(children of c))          // ...or we cannot refine any further
//
// Both residency tests are over a whole *group*, which is half of what keeps the cut crack-free:
// the decision to refine a group is taken once for the group, identically by the parent that
// would be drawn instead and by the children that would replace it, so the two never disagree.
// A cluster that wanted to refine but could not appends the pages it is missing to the request
// list, which is the CPU model of the feedback buffer the cull pass will write (§4.9).
//
// The other half is an invariant on residency itself: **a page is resident only while the pages
// holding its clusters' parents are**. Without it the rule is not crack-free — evict a middle
// page and its clusters stop drawing, their parents draw instead, and the clusters two levels
// down go on drawing the same surface underneath them, because nothing in a per-cluster test can
// see that a *grandparent* is missing. Requests keep one end of the invariant (they only ever
// come from a cluster that is being drawn, so a page is only ever asked for by a resident
// parent) and `PageResidencyManager::evict_to_budget` keeps the other (it only evicts a page
// none of whose child pages are resident, so eviction peels the DAG from the fine end inward).
// The coarse-to-fine page order is what makes that cheap: a cluster's children are always in a
// later page than the cluster, so the invariant is "no resident page names this one as a child".

#include <domain/geometry/cluster_lod.h>

namespace engine::geometry {

// The page holds a group that nothing coarser can replace: the root of the DAG, which is page 0,
// or a group the simplifier got stuck on (`parent_error` is `k_lod_terminal_error`, and the
// clusters of such a group are drawn at every distance because there is no coarser version of
// that patch of surface). Evicting one of these would leave a hole no fallback can fill, so the
// residency manager holds them from the start and never evicts them.
inline constexpr u32 k_page_root = 1u;
// The page is over the byte target because one group did not fit in a page of its own. Splitting
// the group instead would split a cut, so the page grows.
inline constexpr u32 k_page_oversized = 2u;

// One page, GPU-mirrorable: 48 bytes, twelve words, no padding. The three `first_*` fields are
// what a streaming upload copies, the two levels are what a coarse-first prefetch sorts by, and
// `first_child_page`/`child_page_count` name a run of `ClusterPages::child_pages`.
struct ClusterPageDesc {
  u32 first_cluster = 0;  // in ClusterMesh::clusters
  u32 cluster_count = 0;
  u32 first_vertex = 0;  // in the vertex, attribute, and quantized streams
  u32 vertex_count = 0;
  u32 first_triangle = 0;  // in ClusterMesh::triangles
  u32 triangle_count = 0;
  u32 level_min = 0;  // the DAG levels this page's clusters come from
  u32 level_max = 0;
  u32 first_child_page = 0;  // into ClusterPages::child_pages
  u32 child_page_count = 0;
  u32 bytes = 0;  // what the streams above cost, the number pages are sized by
  u32 flags = 0;  // k_page_root, k_page_oversized
};

static_assert(sizeof(ClusterPageDesc) == 48, "ClusterPageDesc is a 48-byte GPU record");

// The clusters one cluster refines into: the group it was simplified from, which the page
// builder keeps contiguous, so they are a range rather than a list. A level-0 cluster is
// original geometry and has none.
struct ClusterChildren {
  u32 first_cluster = 0;
  u32 cluster_count = 0;
};

// The page table of one mesh. `pages` and `child_pages` are what the `.clusters` container
// stores; `page_of_cluster` and `children` are derived from them and from the DAG, and
// `rebuild_cluster_page_index` recomputes them after a read.
struct ClusterPages {
  Vector<ClusterPageDesc> pages;
  // Flat list, one run per page: the *other* pages holding the children (the finer clusters) of
  // that page's clusters, ascending and without repeats. A page's own run is the union of its
  // clusters' child ranges, minus itself, which is what a prefetch of "one level finer than this
  // page" reads and what eviction checks before it may take the page away.
  Vector<u32> child_pages;
  Vector<u32> page_of_cluster;       // one per cluster
  Vector<ClusterChildren> children;  // one per cluster
  u32 page_bytes_target = 0;         // the target `build_cluster_pages` was given
};

// At namespace scope, not nested in ClusterPages: a default argument of a type declared inside
// another class trips a GCC bug, and this is a default argument everywhere.
struct ClusterPagesOptions {
  u32 page_bytes = 128 * 1024;
};

// What one cluster costs a page: its two descriptors, six bytes of quantized position and eight
// bytes of attributes per vertex (attributes only when the mesh carries them), and four bytes
// per packed triangle. The float `vertices` are not counted: only the acceleration-structure
// builders read those, and they are not streamed.
u32 cluster_page_bytes(const ClusterMesh& mesh, u32 cluster) noexcept;

// Lays `mesh` out in pages, renumbering its clusters as described above. `out` is replaced.
// When `source_of_cluster` is given it receives the permutation — the old index of every new
// cluster — which `permute_cluster_array` applies to a caller's own per-cluster arrays.
//
// Takes one mesh's DAG, before `merge_cluster_meshes` joins several into a scene: the renumbering
// is over the whole cluster array, so running it on an already-merged scene would break the
// per-mesh contiguity `ClusterMeshPart` promises.
//
// Fails, leaving `out` empty and `mesh` untouched, when the DAG is inconsistent (a LOD table of
// the wrong length, a group whose clusters are not all on one level, a cluster whose children
// cannot be found), when the vertex or triangle streams are not partitioned by the clusters, or
// when `page_bytes` is zero.
bool build_cluster_pages(ClusterLodMesh& mesh, const ClusterPagesOptions& options,
                         ClusterPages& out, std::string* error = nullptr,
                         Vector<u32>* source_of_cluster = nullptr);

// `values[new] = values[source_of_cluster[new]]`, in place. The sizes must match; a shorter or
// empty array is left alone, so a caller may pass one it did not fill.
void permute_cluster_array(std::span<const u32> source_of_cluster, Vector<u32>& values);

// Fills `page_of_cluster` and `children` from `pages` and the DAG. The container stores the two
// arrays that cannot be recomputed cheaply and leaves these to the reader. Fails on a page table
// that does not tile the clusters, or on a DAG whose children cannot be matched.
bool rebuild_cluster_page_index(const ClusterLodMesh& mesh, ClusterPages& pages,
                                std::string* error = nullptr);

// The invariants the renderer and the tests rely on: the pages tile the clusters, the vertices,
// and the triangles exactly and in order; every page is within the byte target unless it is
// flagged oversized; levels never rise from one page to the next (pages are coarse to fine) and
// page 0 carries the coarsest level; every child range is inside the mesh and one level below;
// and the child page runs are ascending, unique, and inside the page array.
bool validate_cluster_pages(const ClusterLodMesh& mesh, const ClusterPages& pages,
                            std::string* error = nullptr);

// ---- streaming ---------------------------------------------------------------------------

// Which pages a viewer has. One byte per page rather than a bit: the cull pass will read this
// per cluster, and a byte is one load instead of a load and a shift.
struct PageResidency {
  Vector<u8> resident;
  bool is_resident(u32 page) const noexcept {
    return page < resident.size() && resident[page] != 0;
  }
};

// `select_lod` with pages that may be missing: appends the cut to `cut` and, for every cluster
// that wanted to refine and could not, the pages it is missing to `page_requests`. The appended
// requests are sorted and free of repeats, so they are in coarse-first order — which is also
// priority order, because a coarser page completes more of the picture.
//
// With every page resident this is `select_lod` exactly, cluster for cluster and in the same
// order. With pages missing the cut falls back to the coarsest clusters whose children are not
// all there; it never draws a cluster out of a page that is not resident, and — as long as
// `residency` is ancestor-closed, which is what `PageResidencyManager` maintains — it draws
// every part of the surface exactly once. Returns how many clusters were selected.
//
// A mesh with no page table (an empty `pages`) is selected with `select_lod`: nothing to stream.
u32 select_lod_streaming(const ClusterLodMesh& mesh, const LodView& view, const ClusterPages& pages,
                         const PageResidency& residency, Vector<u32>& cut,
                         Vector<u32>& page_requests);

// The CPU model of the residency manager of docs/plan/04-renderer.md §4.9, for one mesh: a fixed
// budget in bytes, a queue of requested pages, and eviction of the least recently used page
// first — restricted to the pages that may go at all, which is what keeps the drawing rule above
// honest. A page may be evicted only when none of its child pages is resident (so residency
// stays ancestor-closed) and only when it is not flagged `k_page_root` (so nothing that has no
// coarser version is ever taken away). Between them those two rules mean eviction peels the DAG
// from the fine end inward, which is also the right thing to do: the fine pages are the bulk of
// the bytes and the first thing a camera pulling back stops needing.
//
// The GPU version — feedback from the cull pass, asynchronous I/O, several meshes and three page
// types under one budget — comes later; this is the reference its numbers and its tests are
// taken against, and what `engine-content stats` sweeps a camera over.
class PageResidencyManager {
 public:
  // Points the manager at a page table and a budget. Every `k_page_root` page becomes resident,
  // whatever the budget says. Fails on an empty page table. The table must outlive nothing: its
  // sizes and child lists are copied.
  bool reset(const ClusterPages& table, u64 budget, std::string* error = nullptr);

  void set_budget(u64 budget) noexcept { budget_ = budget; }
  void begin_frame() noexcept { ++frame_; }
  u64 frame_index() const noexcept { return frame_; }

  // Marks a resident page as used this frame, which is what keeps it out of the eviction order.
  void touch(u32 page) noexcept;
  // Queues a page to be loaded. A page that is already resident is touched instead, and a page
  // already in the queue is not queued twice.
  void request(u32 page);
  void request(std::span<const u32> list);

  // The loader: makes up to `max_pages` queued pages resident, in the order they were requested,
  // and returns how many. The budget is not consulted here — `evict_to_budget` is what enforces
  // it, after the frame has said which pages it is using.
  u32 admit(u32 max_pages);
  // Evicts least-recently-used pages, ties broken by page index, until the resident bytes are
  // within the budget or nothing may go. Only a page with no resident child page and no
  // `k_page_root` flag is a candidate, so residency stays ancestor-closed and the cut stays
  // crack-free. Returns how many were evicted.
  u32 evict_to_budget();

  const PageResidency& page_residency() const noexcept { return residency_; }
  u64 resident_bytes() const noexcept { return resident_bytes_; }
  u32 resident_pages() const noexcept { return resident_pages_; }
  u32 pending() const noexcept { return queue_.size() - queue_head_; }
  u64 budget_bytes() const noexcept { return budget_; }
  u64 total_bytes() const noexcept { return total_bytes_; }

 private:
  void admit_one(u32 page);
  bool may_evict(u32 page) const noexcept;

  Vector<u32> bytes_;  // per page
  Vector<u64> used_;   // the frame each page was last touched
  Vector<u8> root_;    // k_page_root: resident from the start, never evicted
  // The page table's child lists, copied so that eviction can check the ancestor-closure
  // invariant without the caller keeping the table alive.
  Vector<u32> child_first_;
  Vector<u32> child_count_;
  Vector<u32> child_pages_;
  Vector<u8> queued_;
  Vector<u32> queue_;
  PageResidency residency_;
  u32 queue_head_ = 0;
  u32 resident_pages_ = 0;
  u64 resident_bytes_ = 0;
  u64 total_bytes_ = 0;
  u64 budget_ = 0;
  u64 frame_ = 0;
};

}  // namespace engine::geometry
