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
// **What is not here, and why.** The page payloads come out of the `SceneData` the load produced,
// not out of the `.clusters` container by range: the container reader has no API for a section's
// file offset and `io::AsyncRead` reads whole files, so a file-backed source is two additive API
// changes and a source of its own rather than a branch in this one. `PageSource` is the seam it
// slots into. See docs/subsystems/renderer.md for what that costs today (host memory, not device).

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/gfx/vulkan.h>
#include <systems/renderer/gpu_scene.h>

#include <string>

namespace engine::renderer {

// What a run of streaming did, as engine-view's summary and `render.benchmark` report it. The
// counters are cumulative over the run; `pages_resident` and `pending` are the latest frame's.
struct StreamStats {
  u32 pages_total = 0;
  u32 pages_resident = 0;
  u32 pages_pinned = 0;   // `geometry::k_page_root`: resident from the start, never evicted
  u32 page_slots = 0;     // the pool's fixed-size slots
  u32 pending = 0;        // requests queued in the manager and not yet admitted
  u64 requests = 0;       // request entries the cull passes wrote, summed over the run
  u64 uploads_bytes = 0;  // payload copied into the pool
  u64 uploads = 0;        // pages copied
  u64 evictions = 0;
  u64 stale = 0;      // requests dropped because the page that asked has been evicted since
  u64 overflows = 0;  // frames whose request buffer was full: the pages come back next frame
  // Frames from the first frame with work outstanding to the first frame with none. A camera jump
  // starts a new interval, so this is "how long the last convergence took" and not a total.
  u32 frames_to_converge = 0;
  u64 resident_bytes = 0;
  u64 page_bytes = 0;    // every page of the scene: what streaming is a fraction of
  u64 budget_bytes = 0;  // the residency manager's budget
};

class GeometryStreamer {
 public:
  GeometryStreamer() noexcept = default;
  ~GeometryStreamer();
  ENGINE_NON_COPYABLE(GeometryStreamer);

  // `scene` and `device` must outlive the streamer. Does nothing and stays inactive for a scene
  // that is not streamed, so a caller may create one unconditionally.
  bool create(const gfx::Device& device, GpuScene& scene, u32 frames_in_flight,
              std::string* error = nullptr);
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
  u64 staged_bytes(u32 page) const noexcept;
  void apply_evictions();
  bool take_slot(u32 page, u32& slot);
  // One page's bytes into the staging ring, in the order `record_uploads` copies them out: the
  // patched cluster descriptors first, then the quantized positions, the attributes, the packed
  // triangles, and — when the frame builds acceleration structures — the float positions and their
  // 8-bit indices. **This is the seam a file-backed source replaces**: a page is a contiguous run
  // of each of those streams, so reading one from a `.clusters` container is a handful of range
  // reads rather than a different shape of code.
  void copy_page(u32 page, u32 slot, u8* dst);

  const gfx::Device* device_ = nullptr;
  GpuScene* scene_ = nullptr;
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
