#pragma once

// Fixed-size cluster pages (docs/plan/04-renderer.md §4.3 step 3 and §4.9): the unit the
// renderer streams clusters in. A page is a contiguous run of clusters, of a target size
// (~128 KB) counted over exactly the streams the GPU reads for a cluster — its `ClusterDesc`,
// its `ClusterLodDesc`, its quantized positions, its attributes, and its packed triangles —
// and pages are laid out **coarse to fine**, so that a viewer who has the first n pages has a
// complete, if coarse, picture of the whole mesh rather than a detailed picture of part of it.
//
// "Coarse first" means the *minimum resident set* first, not the top level first: a DAG has one
// root per part that simplified all the way down plus one per group the simplifier got stuck on,
// and none of them can be stood in for by anything coarser. Those groups fill the first pages,
// which are the ones the residency manager pins; everything after them is refinement.
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
// flagged oversized; the `k_page_root` pages are a prefix and page 0 carries the coarsest level;
// levels never rise from one page to the next inside that prefix or inside the refinement that
// follows it; every cluster's children are finer, inside the mesh, and later in the array; and
// the child page runs are ascending, unique, and inside the page array.
bool validate_cluster_pages(const ClusterLodMesh& mesh, const ClusterPages& pages,
                            std::string* error = nullptr);

// The same over a **scene** of meshes merged by `merge_paged_cluster_meshes`. The invariants that
// are about the whole array — the pages tiling the clusters, the vertices and the triangles, the
// byte target, children finer and later, child page runs ascending — hold across the scene
// exactly as they do in one mesh. The two that are about a *mesh* are checked per mesh instead:
// the pinned `k_page_root` pages are a prefix of **each mesh's** run and its first page is one of
// them, and the coarse-to-fine order restarts at each mesh, because mesh 1's coarsest page
// necessarily follows mesh 0's finest. `parts` must tile the page array and the cluster array.
bool validate_cluster_pages(const ClusterLodMesh& mesh, const ClusterPages& pages,
                            std::span<const ClusterMeshPart> parts, std::string* error = nullptr);

// Joins meshes that have each been laid out in pages into the one set of buffers and the one page
// table a scene streams from: `merge_cluster_meshes` over the geometry with `ClusterOrder::keep`,
// so no mesh's page-ordered clusters are shuffled, and the page tables concatenated with every
// index shifted — cluster, vertex and triangle ranges by the merged bases, child-page entries and
// `page_of_cluster` by the pages before this mesh, `children` by the clusters before it.
//
// A mesh's pages stay together and in their own order, so `parts_out[m].first_page` and
// `page_count` name them and mesh m's root pages — the groups nothing coarser can replace, which
// `build_cluster_pages` keeps at the front — are the front of *its* run. That is what makes the
// pinning rule right for a scene: `PageResidencyManager` pins every `k_page_root` page, which is
// one per mesh at least, not page 0 alone; a scene that pinned only page 0 could not draw mesh 1
// at all. Nothing links the meshes, so a page's children are always in the same mesh.
//
// Every mesh must carry a page table laid out with the same `page_bytes_target` — the merged
// table has one target, and two targets in one scene would make every fill ratio and every
// oversized flag mean something different from page to page. Fails on an empty list, a mesh with
// no page table, a page table that does not match its mesh, or targets that disagree.
bool merge_paged_cluster_meshes(std::span<const ClusterLodMesh> meshes,
                                std::span<const ClusterPages> pages, ClusterLodMesh& out,
                                Vector<ClusterMeshPart>& parts_out, ClusterPages& pages_out,
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

// ---- request priority --------------------------------------------------------------------

// What one missing page is worth, as the cluster that could not refine into it sees it. Two of
// the three terms of docs/plan/04-renderer.md §4.9's priority — screen contribution and distance
// to the observer — travel with the request; the third, time since request, is the manager's,
// because only it knows how long a request has been waiting (`set_starvation_frames`).
//
// The defaults say *nothing*: a request made without a view (`request(u32)`) scores zero and
// loses every distance tie, so a queue of them is served in page order, which is coarse first,
// which is what the manager did before priorities existed.
inline constexpr f32 k_unknown_distance = 3.402823466e+38f;  // FLT_MAX: behind everything known

struct PageRequest {
  u32 page = 0;
  // The projected diameter of the requesting cluster's bounding sphere, in pixels: what this page
  // would add to the picture, and the term that decides. `screen_pixels` computes it.
  f32 screen_px = 0.0f;
  // Observer to the near side of that sphere, in mesh units. It settles the one thing screen size
  // cannot: a small near cluster and a large far one project to the same size, and the near one
  // is the one an observer moving at all will still want next frame.
  f32 distance = k_unknown_distance;
};

// The projected diameter of a bounding sphere in pixels — 2 * radius / max(distance - radius,
// znear) * proj_scale — which is `projected_error`'s projection applied to the sphere's own size
// rather than to an error, so the two are in the same units and the same view. And the distance
// the request records: observer to the near side of the sphere, never below `znear`.
f32 screen_pixels(Vec4 sphere, const LodView& view) noexcept;
f32 sphere_distance(Vec4 sphere, const LodView& view) noexcept;

// `select_lod_streaming` with the priority of every request filled in from the cluster that made
// it: the same cut and the same pages, each carrying the screen size and the distance of its
// requester. A page several clusters ask for keeps the best of them — the largest screen size and
// the smallest distance — because the page is worth what the cluster that would show most of it
// says it is. The appended requests are sorted by page, free of repeats, and a function of the
// inputs alone.
u32 select_lod_streaming(const ClusterLodMesh& mesh, const LodView& view, const ClusterPages& pages,
                         const PageResidency& residency, Vector<u32>& cut,
                         Vector<PageRequest>& page_requests);

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
// The queue is served by **priority**, not in arrival order (plan 04 §4.9): a page that would
// cover 400 pixels of the picture loads before one that would cover 4, whichever was asked for
// first. The order over queued pages, most urgent first, is
//
//   1. a request that has waited more than `starvation_frames()` frames, oldest first;
//   2. the largest screen contribution (`PageRequest::screen_px`);
//   3. the smallest distance to the observer;
//   4. the smallest page index.
//
// Rule 4 makes the order total, so a run is reproducible and a queue of requests that carry no
// view (`request(u32)`) is served in page order — coarse first, which is what the manager did
// before priorities existed. Rules 2 and 3 are the view: what the page is worth now.
//
// Rule 1 is the starvation guard, and it is a **step rather than a slope** on purpose. A smooth
// "time since request" term would change every queued page's key every frame, which is exactly
// what a heap cannot survive: the ordering it was built under would be stale the moment the frame
// advanced, and re-keying the whole queue every frame would cost more than the queue is worth.
// A step costs one drain of a FIFO that is already in request order, and it is also the honest
// statement of the requirement — not "old requests count for a little more" but "nothing waits
// longer than N frames", which is what a viewer parked in front of a small distant detail needs.
// The requests themselves do not age: a page's score is what the clusters that asked for it said,
// kept at the best of them, so a re-request may only ever move a page up the queue.
class PageResidencyManager {
 public:
  // About a quarter of a second at 60 Hz: long enough that the guard does not fight the screen
  // order under any normal load, short enough that nothing a viewer is looking at stays missing
  // for longer than a glance.
  static constexpr u32 k_default_starvation_frames = 16;

  // Points the manager at a page table and a budget. Every `k_page_root` page becomes resident,
  // whatever the budget says — a budget below them is not honoured, because a mesh missing one
  // of them cannot be drawn at all. In a scene merged by `merge_paged_cluster_meshes` that is one
  // page per mesh at least, not page 0 alone, and the rule needs no special case: the flag is on
  // the page. Fails on an empty page table. The sizes, flags, and child lists are copied, so
  // `table` does not have to outlive the manager.
  bool reset(const ClusterPages& table, u64 budget, std::string* error = nullptr);

  void set_budget(u64 budget) noexcept { budget_ = budget; }
  // Advances the frame and applies the starvation guard to whatever has waited too long.
  void begin_frame() noexcept;
  u64 frame_index() const noexcept { return frame_; }

  // How long a queued page may wait before rule 1 takes it. Zero promotes everything at once,
  // which is the old arrival order and is what a test that wants no view-dependence asks for.
  void set_starvation_frames(u32 frames) noexcept { starvation_frames_ = frames; }
  u32 starvation_frames() const noexcept { return starvation_frames_; }

  // Marks a resident page as used this frame, which is what keeps it out of the eviction order.
  void touch(u32 page) noexcept;
  // Queues a page to be loaded. A page that is already resident is touched instead; a page
  // already queued keeps the better of the two priorities rather than being queued twice.
  void request(u32 page);
  void request(const PageRequest& entry);
  void request(std::span<const u32> list);
  void request(std::span<const PageRequest> list);

  // The page the next `admit` would take, without taking it: false when nothing is queued.
  bool next_request(u32& page) const noexcept;
  // Drops a queued page **without** admitting it, and returns whether it was queued at all.
  //
  // This is what a request that has gone **stale** needs, and it is not a convenience. A request
  // only ever comes from a cluster that is being drawn, so it arrives with a resident parent — but
  // a queue is not served in the frame it was filled in, and by the time a page reaches the front
  // the parent that asked for it may have been evicted. Admitting it then would put the finer
  // geometry back with nothing above it, which is exactly the hole the ancestor-closed invariant
  // exists to prevent: the parents draw the coarse surface and the children draw the fine one on
  // top of it. The caller is the one that can tell — it knows which pages hold a page's parents —
  // so the manager offers the removal rather than deciding.
  bool drop(u32 page);

  // The loader: makes up to `max_pages` queued pages resident, most urgent first, and returns how
  // many. The budget is not consulted here — `evict_to_budget` is what enforces it, after the
  // frame has said which pages it is using.
  u32 admit(u32 max_pages);
  // Evicts least-recently-used pages, ties broken by page index, until the resident bytes are
  // within the budget or nothing may go. Only a page with no resident child page and no
  // `k_page_root` flag is a candidate, so residency stays ancestor-closed and the cut stays
  // crack-free. Returns how many were evicted.
  u32 evict_to_budget();

  const PageResidency& page_residency() const noexcept { return residency_; }
  u64 resident_bytes() const noexcept { return resident_bytes_; }
  u32 resident_pages() const noexcept { return resident_pages_; }
  u32 pending() const noexcept { return heap_.size(); }
  u64 budget_bytes() const noexcept { return budget_; }
  u64 total_bytes() const noexcept { return total_bytes_; }

 private:
  void admit_one(u32 page);
  bool may_evict(u32 page) const noexcept;
  // The queue is a binary max-heap of page indices under the order above, with `heap_at_` giving
  // every queued page its slot so that a re-request or the starvation guard can move one entry
  // instead of rebuilding the heap. Every array below is sized once in `reset` and the heap keeps
  // its capacity across a drain, so a steady state of requesting and admitting allocates nothing.
  bool more_urgent(u32 a, u32 b) const noexcept;
  void sift_up(u32 at) noexcept;
  void sift_down(u32 at) noexcept;
  u32 pop_most_urgent() noexcept;
  void remove_at(u32 at) noexcept;
  void promote_starved() noexcept;

  Vector<u32> bytes_;  // per page
  Vector<u64> used_;   // the frame each page was last touched
  Vector<u8> root_;    // k_page_root: resident from the start, never evicted
  // The page table's child lists, copied so that eviction can check the ancestor-closure
  // invariant without the caller keeping the table alive.
  Vector<u32> child_first_;
  Vector<u32> child_count_;
  Vector<u32> child_pages_;
  Vector<f32> screen_;     // per page, the best screen contribution asked with
  Vector<f32> distance_;   // per page, the smallest distance asked with
  Vector<u64> requested_;  // per page, the frame it was queued in
  Vector<u8> starved_;     // per page, whether the guard has promoted it
  Vector<u32> heap_at_;    // per page, its slot in heap_, or ~0 when it is not queued
  Vector<u32> heap_;
  // The queued pages in the order they were first asked for, so the guard drains a head rather
  // than scanning the queue. An entry carries the frame it was made in, which is what tells a
  // stale entry (its page was admitted and asked for again) from a live one.
  Vector<u32> age_page_;
  Vector<u64> age_frame_;
  PageResidency residency_;
  u32 age_head_ = 0;
  u32 starvation_frames_ = k_default_starvation_frames;
  u32 resident_pages_ = 0;
  u64 resident_bytes_ = 0;
  u64 total_bytes_ = 0;
  u64 budget_ = 0;
  u64 frame_ = 0;
};

}  // namespace engine::geometry
