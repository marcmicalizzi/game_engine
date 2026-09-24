#pragma once

// Geometry pages on the GPU (docs/plan/04-renderer.md §4.3 step 3 and §4.9,
// docs/subsystems/renderer.md "Geometry streaming"). `domain/geometry/cluster_pages.h` is the CPU
// model — the layout, the drawing rule, and `PageResidencyManager`'s priority heap and
// ancestor-closed eviction. This is the half that runs beside a frame: it reads the cull pass's
// feedback back one frame slot late, drives that manager with it, copies the pages the manager
// admits into the `GpuScene`'s page pool through a staging ring with a per-frame byte budget, and
// keeps the residency words the next cull pass will read.
//
// **Nothing here blocks.** The feedback of frame N is read when slot N comes around again, which
// is `frames_in_flight` frames later and is the same deferred-readback pattern the visible-pair
// counts already use (`SceneRenderer::collect_slot`). The cost of that latency is that a page a
// camera jump asks for arrives two or three frames after it was wanted, which is exactly what the
// drawing rule's fallback clause exists to cover: the picture is coarser in the meantime and never
// has a hole in it.
//
// **Where a page's bytes come from.** Two answers, and `copy_page` is the seam between them
// (`systems/renderer/page_source.h`). Out of the `SceneData` the load produced, which is what the
// procedural heightfield and a glTF loaded with `--no-cache` have to use because there is no file
// behind them — and which means streaming saves device memory and not host memory. Or out of the
// `.clusters` containers the meshes came from, by ranged reads on the Efficiency pool, which is
// what lets the merged host streams be released once the scene is on the GPU. The second is a
// `FilePageSource` handed to `create`; without one the first is what happens.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/gfx/vulkan.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/page_source.h>

#include <string>

namespace engine::renderer {

// What a run of streaming did, as engine-view's summary and `render.benchmark` report it. The
// counters are cumulative over the run; `pages_resident` and `pending` are the latest frame's.
struct StreamStats {
  u32 pages_total = 0;
  u32 pages_resident = 0;  // the manager's: admitted, whether or not the bytes have landed
  u32 pool_pages = 0;      // the pool's: what the cull pass can draw out of this frame
  u32 pages_pinned = 0;    // `geometry::k_page_root`: resident from the start, never evicted
  u32 page_slots = 0;      // the pool's fixed-size slots
  u32 pending = 0;         // requests queued in the manager and not yet admitted
  u64 requests = 0;        // request entries the cull passes wrote, summed over the run
  u64 uploads_bytes = 0;   // payload copied into the pool
  u64 uploads = 0;         // pages copied
  u64 evictions = 0;
  u64 stale = 0;      // requests dropped because the page that asked has been evicted since
  u64 overflows = 0;  // frames whose request buffer was full: the pages come back next frame
  // Frames from the first frame with work outstanding to the first frame with none. A camera jump
  // starts a new interval, so this is "how long the last convergence took" and not a total.
  u32 frames_to_converge = 0;
  u64 resident_bytes = 0;
  u64 page_bytes = 0;    // every page of the scene: what streaming is a fraction of
  u64 budget_bytes = 0;  // the residency manager's budget
  // Where the payloads came from. `from_file` is the container-backed source; with it, `reads`
  // and `read_bytes` are what came off disk and `host_bytes_freed` is what releasing the merged
  // streams saved. Without it the three are zero and the bytes came out of host memory.
  bool from_file = false;
  u64 file_reads = 0;
  u64 file_bytes = 0;
  u64 host_bytes_freed = 0;
  // Frames in which a page was wanted and its reads had not landed yet. It is the read latency
  // made visible: a run with a high count against few uploads is waiting on the disk, and one
  // with none never had to.
  u64 load_waits = 0;
  u32 loads_in_flight = 0;
  // Loads given up so that the page at the head of the walk could start its own: every load slot
  // held a page the walk cannot stage before the head, which is the circular wait that stalled
  // the desert flythrough (docs/subsystems/renderer.md, "Admission never waits on a read it cannot
  // start"). Each one is a read done twice; a run that counts many has too few load slots for its
  // read latency.
  u64 steals = 0;
};

class GeometryStreamer {
 public:
  GeometryStreamer() noexcept = default;
  ~GeometryStreamer();
  ENGINE_NON_COPYABLE(GeometryStreamer);

  // `scene`, `device` and `source` must outlive the streamer. Does nothing and stays inactive for
  // a scene that is not streamed, so a caller may create one unconditionally. A null `source` is
  // the in-memory path: pages are copied out of `scene.data()`, which has to still hold its
  // streams.
  bool create(const gfx::Device& device, GpuScene& scene, u32 frames_in_flight,
              PageSource* source = nullptr, std::string* error = nullptr);
  void destroy() noexcept;
  bool active() const noexcept { return scene_ != nullptr && scene_->streamed(); }

  // The feedback the frame that last used `slot` wrote: requests into the manager's heap, the
  // "used this frame" bits into its LRU. Called when the slot comes around, never before.
  void consume(u32 slot);
  // Evict to the budget, admit what this frame's upload budget allows, stage the payloads, and
  // write `slot`'s residency words. Returns the address the frame's `gfx::CullParams::streaming`
  // must carry, which is `slot`'s `gfx::StreamParams` block.
  u64 prepare(u32 slot);
  // The copies `prepare` staged, recorded into the frame's command buffer.
  void record_uploads(VkCommandBuffer commands);
  bool has_uploads() const noexcept { return !uploads_.empty(); }
  // Where the frame's feedback is copied to, one buffer per frame slot: `{u32 count,
  // geometry::PageRequest[max_requests], u32 used[page_count]}`.
  const gfx::BufferResource& feedback(u32 slot) const noexcept { return feedback_[slot]; }

  const StreamStats& stats() const noexcept { return stats_; }
  // Which pool slot holds a page's payload, or `k_no_page_slot`. It is the one piece of the
  // streamer's bookkeeping a reader outside it needs: a page's bytes are at
  // `slot * GpuScene::slot_vertices()` and `slot * GpuScene::slot_triangles()` in the pool
  // streams, so this is how a test (or a diagnostic) says "what is actually in the pool for this
  // page" without re-deriving the mapping and getting it wrong in the same way twice.
  static constexpr u32 k_no_page_slot = ~u32{0};
  u32 slot_of_page(u32 page) const noexcept {
    return page < slot_of_page_.size() ? slot_of_page_[page] : k_no_page_slot;
  }
  void reset_stats() noexcept;
  // Puts every page back to the pinned set, which is what a measurement of "frames to converge
  // from cold" starts from. The GPU side follows on the next `prepare`.
  bool reset_residency(std::string* error = nullptr);

 private:
  struct Upload {
    u32 page = 0;
    u32 slot = 0;   // the pool slot it lands in
    u64 stage = 0;  // byte offset in the staging ring
  };
  // What one attempt to put a page in the pool did. `pending` exists only for the file-backed
  // source: the page's reads have been started and the bytes will be there in a frame or two,
  // which stops *this* page being staged but must not stop the manager admitting more — otherwise
  // a scene would converge at one page per read latency.
  enum class Stage : u8 { done, pending, blocked };

  u64 staged_bytes(u32 page) const noexcept;
  void apply_evictions();
  bool take_slot(u32 page, u32& slot);
  // Undoes `take_slot` for a page that never became resident, so no frame can be holding it.
  void give_back_slot(u32 page) noexcept;
  // One page's bytes into the staging ring, in the order `record_uploads` copies them out and at
  // the offsets `GpuScene::page_stage_layout` gives: the patched cluster descriptors first, then
  // the quantized positions, the attributes, the packed triangles, and — when the frame builds
  // acceleration structures — the float positions and their 8-bit indices.
  //
  // **This is the seam the file-backed source slots into.** The four read streams come either
  // from `scene_->data()` or out of a completed `FilePageSource` load; the two the host computes,
  // the patched descriptors and the 8-bit indices, are written here either way.
  void copy_page(u32 page, u32 slot, u8* dst);
  void patch_clusters(u32 page, u32 slot, u8* dst);
  void pack_indices(u32 page, u8* dst);
  Stage stage_page(u32 page, u8* ring, u64 ring_base, u64& at, u64& remaining, u64& requested);
  // Starts a page's reads without staging it, so that the reads of the pages behind a gap overlap
  // the one the frame is waiting on. A no-op without a file source.
  void prefetch(u32 page);
  void cancel_load(u32 page);
  void cancel_stale_loads();
  // Gives up the load of a page behind `head` so that `head` can start its own; see `prepare`.
  bool steal_load_for(u32 head);

  const gfx::Device* device_ = nullptr;
  GpuScene* scene_ = nullptr;
  PageSource* source_ = nullptr;  // null: the payloads come out of `scene_->data()`
  Vector<u32> load_of_page_;      // page -> the source's load handle, or k_no_load
  geometry::PageResidencyManager manager_;
  Vector<gfx::BufferResource> feedback_;
  Vector<u32> slot_of_page_;  // page -> pool slot, or ~0
  Vector<u32> page_of_slot_;  // and back, so an eviction frees exactly one slot
  Vector<u32> free_slots_;
  // A slot an eviction gave back may not be filled again for `frames_in_flight` frames: the frames
  // still on the queue carry residency words that say the page *is* resident, and their draws read
  // that slot. Handing it to another page in the same frame would put one mesh's triangles under
  // another's descriptors for as long as those frames take. So a freed slot retires first.
  Vector<u32> retiring_slot_;
  Vector<u64> retiring_frame_;
  Vector<u8> resident_;  // the streamer's copy, so an eviction can be told from the manager's view
  Vector<Upload> uploads_;
  Vector<u8> scratch_;   // one page's patched cluster descriptors, reused
  Vector<u8> indices8_;  // and its 8-bit indices, so a frame's staging allocates nothing
  void retire_slots();
  bool free_one_slot();
  // Whether a queued page may still be loaded: it is pinned, it holds its own parents (a page that
  // spans two DAG levels is its own parent and the child list leaves it out of its own run), or
  // some page holding its clusters' parents is resident. A request that fails this has gone stale
  // — the cluster that asked for it has been evicted since — and loading it would put the finer
  // geometry back under a hole.
  bool may_admit(u32 page) const noexcept;

  Vector<u32> parent_first_;  // the inverse of the page table's child lists, built once
  Vector<u32> parent_count_;
  Vector<u32> parent_pages_;

  u32 frames_in_flight_ = 1;
  u64 frame_ = 0;
  u32 converge_frames_ = 0;
  bool converging_ = false;
  StreamStats stats_;
};

}  // namespace engine::renderer
