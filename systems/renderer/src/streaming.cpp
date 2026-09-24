#include <core/base/assert.h>
#include <systems/renderer/streaming.h>

#include <algorithm>
#include <cstring>
#include <renderer_log.h>

namespace engine::renderer {

namespace {

// One spelling of "this page has no pool slot", shared with the accessor a reader outside the
// streamer asks with (`GeometryStreamer::slot_of_page`).
constexpr u32 k_no_slot = GeometryStreamer::k_no_page_slot;

}  // namespace

GeometryStreamer::~GeometryStreamer() { destroy(); }

bool GeometryStreamer::create(const gfx::Device& device, GpuScene& scene, u32 frames_in_flight,
                              FilePageSource* source, std::string* error) {
  destroy();
  if (!scene.streamed()) return true;
  device_ = &device;
  scene_ = &scene;
  source_ = source != nullptr && source->valid() ? source : nullptr;
  frames_in_flight_ = frames_in_flight > 0 ? frames_in_flight : 1;
  if (frames_in_flight_ > k_stream_slots) {
    if (error != nullptr) {
      *error = "a streamed scene supports at most " + std::to_string(k_stream_slots) +
               " frames in flight; the residency word array has one region per frame slot";
    }
    destroy();
    return false;
  }
  const u32 pages = scene.page_count();
  if (!manager_.reset(scene.data().pages, scene.page_budget_bytes(), error)) {
    destroy();
    return false;
  }
  slot_of_page_.assign(pages, k_no_slot);
  page_of_slot_.assign(scene.page_slots(), k_no_slot);
  resident_.assign(pages, u8{0});
  load_of_page_.assign(pages, FilePageSource::k_no_load);
  free_slots_.reserve(scene.page_slots());
  for (u32 s = scene.page_slots(); s > 0; --s)
    free_slots_.push_back(s - 1);
  uploads_.reserve(scene.page_slots());
  // The inverse of the page table's child lists: which pages hold a page's parents. The table
  // stores the forward direction because that is what eviction asks ("is anything under me still
  // here"); admission asks the other one ("is anything above me still here"), and inverting once
  // at load is a counting sort over a list of a few thousand entries.
  {
    const geometry::ClusterPages& table = scene.data().pages;
    parent_first_.assign(pages + 1, 0u);
    for (const u32 child : table.child_pages)
      ++parent_first_[child + 1];
    for (u32 p = 0; p < pages; ++p)
      parent_first_[p + 1] += parent_first_[p];
    parent_count_.assign(pages, 0u);
    parent_pages_.assign(table.child_pages.size(), 0u);
    for (u32 p = 0; p < pages; ++p) {
      const geometry::ClusterPageDesc& desc = table.pages[p];
      for (u32 k = 0; k < desc.child_page_count; ++k) {
        const u32 child = table.child_pages[desc.first_child_page + k];
        parent_pages_[parent_first_[child] + parent_count_[child]++] = p;
      }
    }
  }
  // The largest page's cluster descriptors, staged patched. Sized once, so a frame allocates
  // nothing (AGENTS.md, "no allocations in the frame loop in steady state").
  u32 max_clusters = 0;
  for (u32 p = 0; p < pages; ++p) {
    max_clusters = std::max(max_clusters, scene.data().pages.pages[p].cluster_count);
  }
  scratch_.resize(max_clusters * static_cast<u32>(sizeof(geometry::ClusterDesc)));
  indices8_.reserve(scene.slot_triangles() * 3);

  // One feedback buffer per frame slot: the count, the requests, and the used bits, copied out of
  // the device-local arrays by the frame's own transfer pass and read when the slot comes around.
  const u64 bytes = sizeof(u32) + u64{scene.max_requests()} * sizeof(geometry::PageRequest) +
                    u64{pages} * sizeof(u32);
  feedback_.resize(frames_in_flight_);
  for (u32 slot = 0; slot < frames_in_flight_; ++slot) {
    if (!gfx::create_buffer(device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, feedback_[slot],
                            error)) {
      destroy();
      return false;
    }
    std::memset(feedback_[slot].mapped, 0, bytes);
  }
  reset_stats();
  return true;
}

void GeometryStreamer::destroy() noexcept {
  // Every outstanding read is given back first: a job still writing into a load buffer must not
  // outlive the source that owns it, and `release` waits.
  if (source_ != nullptr) {
    for (u32 p = 0; p < load_of_page_.size(); ++p) {
      if (load_of_page_[p] != FilePageSource::k_no_load) source_->release(load_of_page_[p]);
    }
  }
  source_ = nullptr;
  load_of_page_.clear();
  if (device_ != nullptr) {
    for (gfx::BufferResource& buffer : feedback_)
      gfx::destroy_buffer(*device_, buffer);
  }
  feedback_.clear();
  slot_of_page_.clear();
  page_of_slot_.clear();
  parent_first_.clear();
  parent_count_.clear();
  parent_pages_.clear();
  free_slots_.clear();
  retiring_slot_.clear();
  retiring_frame_.clear();
  resident_.clear();
  uploads_.clear();
  scratch_.clear();
  indices8_.clear();
  device_ = nullptr;
  scene_ = nullptr;
  frame_ = 0;
  converge_frames_ = 0;
  converging_ = false;
  stats_ = StreamStats{};
}

void GeometryStreamer::reset_stats() noexcept {
  stats_ = StreamStats{};
  converge_frames_ = 0;
  converging_ = false;
  if (!active()) return;
  stats_.pages_total = scene_->page_count();
  stats_.page_slots = scene_->page_slots();
  stats_.page_bytes = manager_.total_bytes();
  stats_.budget_bytes = manager_.budget_bytes();
  for (const geometry::ClusterPageDesc& page : scene_->data().pages.pages)
    stats_.pages_pinned += (page.flags & geometry::k_page_root) != 0 ? 1u : 0u;
  stats_.pages_resident = manager_.resident_pages();
  stats_.resident_bytes = manager_.resident_bytes();
  stats_.pending = manager_.pending();
  stats_.from_file = source_ != nullptr;
  if (source_ != nullptr) stats_.host_bytes_freed = source_->released_bytes();
}

bool GeometryStreamer::reset_residency(std::string* error) {
  if (!active()) return true;
  if (!manager_.reset(scene_->data().pages, scene_->page_budget_bytes(), error)) return false;
  for (u32 p = 0; p < load_of_page_.size(); ++p)
    cancel_load(p);
  slot_of_page_.assign(scene_->page_count(), k_no_slot);
  page_of_slot_.assign(scene_->page_slots(), k_no_slot);
  resident_.assign(scene_->page_count(), u8{0});
  free_slots_.clear();
  retiring_slot_.clear();
  retiring_frame_.clear();
  for (u32 s = scene_->page_slots(); s > 0; --s)
    free_slots_.push_back(s - 1);
  uploads_.clear();
  return true;
}

// One formula for what a page costs, in `GpuScene`: the staging ring's layout, the upload budget's
// floor, and the byte accounting a summary reports all read it, and two of those being a byte
// apart is a page that never loads.
u64 GeometryStreamer::staged_bytes(u32 page) const noexcept {
  return scene_->page_payload_bytes(page);
}

// The descriptors' offsets are made **slot-relative here, on the host**, which is the decision the
// whole design turns on: a cluster of a page in slot s reads its vertices at
// `s * slot_vertices + (its offset - the page's first vertex)`, and every shader that touches
// geometry goes on reading `ClusterDesc::vertex_offset` exactly as it did before pages existed.
// The alternative — leaving the offsets page-relative and having the shader add the slot base —
// needs the page table's address and `page_of_cluster`'s in the rasterizers, the resolve and the
// CLAS records, and `gfx::ClusterDrawParams` is full at its 128-byte push limit while
// `gfx::MeshDesc` is full at 64. See docs/subsystems/gfx.md for what the indirection costs where
// it *can* be measured.
void GeometryStreamer::patch_clusters(u32 page, u32 slot, u8* dst) {
  const SceneData& data = scene_->data();
  const geometry::ClusterPageDesc& desc = data.pages.pages[page];
  const geometry::ClusterMesh& mesh = data.lod.mesh;
  const u32 vertex_base = slot * scene_->slot_vertices();
  const u32 triangle_base = slot * scene_->slot_triangles();
  auto* patched = reinterpret_cast<geometry::ClusterDesc*>(scratch_.data());
  for (u32 k = 0; k < desc.cluster_count; ++k) {
    geometry::ClusterDesc cluster = mesh.clusters[desc.first_cluster + k];
    cluster.vertex_offset = vertex_base + (cluster.vertex_offset - desc.first_vertex);
    cluster.triangle_offset = triangle_base + (cluster.triangle_offset - desc.first_triangle);
    patched[k] = cluster;
  }
  const u64 bytes = u64{desc.cluster_count} * sizeof(geometry::ClusterDesc);
  if (bytes > 0) std::memcpy(dst, patched, static_cast<usize>(bytes));
}

// The 8-bit form the cluster acceleration structure builds read: three bytes a triangle, packed
// from the **same words** the rasterizers read — out of the staged page rather than out of a
// second source — so the two can never disagree about a triangle whichever source the page came
// from.
//
// **`indices8_` is a scratch buffer and `pack_cluster_indices` appends**, which is what the whole
// mesh's pack in `GpuScene::create_ray_tracing` wants and is the opposite of what one page wants.
// Clearing it is therefore not tidiness: without the clear, page k staged the concatenation of
// every page staged before it, starting at this page's block, so page 1 onwards went into the
// pool carrying page 0's triangles and the write ran `k` pages past the end of this page's region
// of the staging ring — over the region the *other* frame slot's page was staged in and, once it
// was long enough, off the end of the mapped ring entirely. See docs/subsystems/renderer.md,
// "A page's 8-bit indices are its own". `clear()` keeps the capacity `create` reserved, so the
// frame still allocates nothing.
//
// The copy is sized from the **page**, not from the scratch buffer, so the block a page writes is
// a function of the page's own descriptor and of nothing that happened before it. The two asserts
// say the same thing twice on purpose: the first catches a scratch buffer that was not empty, the
// second catches a layout that disagrees with the pack about what a page costs.
void GeometryStreamer::pack_indices(u32 page, u8* dst) {
  const geometry::ClusterPageDesc& desc = scene_->data().pages.pages[page];
  const GpuScene::PageStage stage = scene_->page_stage_layout(page);
  if (!stage.ray_tracing || desc.triangle_count == 0) return;
  const auto* triangles = reinterpret_cast<const u32*>(dst + stage.triangles);
  const u64 bytes = u64{desc.triangle_count} * 3;
  indices8_.clear();
  gfx::pack_cluster_indices(std::span<const u32>(triangles, desc.triangle_count), indices8_);
  ENGINE_ASSERT(indices8_.size() == bytes, "a page's 8-bit indices are three bytes a triangle");
  ENGINE_ASSERT(stage.indices8 + bytes <= stage.total,
                "a page's staged bytes stay inside the page's own region of the staging ring");
  std::memcpy(dst + stage.indices8, indices8_.data(), static_cast<usize>(bytes));
}

void GeometryStreamer::copy_page(u32 page, u32 slot, u8* dst) {
  const SceneData& data = scene_->data();
  const geometry::ClusterPageDesc& desc = data.pages.pages[page];
  const geometry::ClusterMesh& mesh = data.lod.mesh;
  const GpuScene::PageStage stage = scene_->page_stage_layout(page);
  auto put = [&](const void* source, u64 at, u64 bytes) {
    if (bytes > 0) std::memcpy(dst + at, source, static_cast<usize>(bytes));
  };
  patch_clusters(page, slot, dst);
  put(mesh.quantized.data() + u64{desc.first_vertex} * 3, stage.quantized,
      u64{desc.vertex_count} * 3 * sizeof(u16));
  put(mesh.attributes.data() + desc.first_vertex, stage.attributes,
      u64{desc.vertex_count} * sizeof(geometry::VertexAttributes));
  put(mesh.triangles.data() + desc.first_triangle, stage.triangles,
      u64{desc.triangle_count} * sizeof(u32));
  if (!stage.ray_tracing) return;
  put(mesh.vertices.data() + desc.first_vertex, stage.vertices,
      u64{desc.vertex_count} * sizeof(Vec3));
  pack_indices(page, dst);
}

void GeometryStreamer::give_back_slot(u32 page) noexcept {
  const u32 slot = slot_of_page_[page];
  if (slot == k_no_slot) return;
  // No frame has ever been told this page is resident, so the slot goes straight back rather than
  // through the retirement list: nothing on the queue can be reading it.
  slot_of_page_[page] = k_no_slot;
  page_of_slot_[slot] = k_no_slot;
  free_slots_.push_back(slot);
}

void GeometryStreamer::cancel_load(u32 page) {
  if (source_ == nullptr || load_of_page_[page] == FilePageSource::k_no_load) return;
  source_->release(load_of_page_[page]);
  load_of_page_[page] = FilePageSource::k_no_load;
  if (resident_[page] == 0) give_back_slot(page);
}

// A page whose reads are in flight can still be evicted by the manager, because the manager's
// residency and the pool's are two different things and only the pool's knows about the read.
// `apply_evictions` cannot see it — it walks the pages the *pool* holds — so the cancellation is
// its own pass, and it is what keeps a load slot and a pool slot from leaking one per eviction.
void GeometryStreamer::cancel_stale_loads() {
  if (source_ == nullptr) return;
  const geometry::PageResidency& held = manager_.page_residency();
  for (u32 p = 0; p < scene_->page_count(); ++p) {
    if (load_of_page_[p] == FilePageSource::k_no_load || held.is_resident(p)) continue;
    cancel_load(p);
    ++stats_.stale;
  }
}

void GeometryStreamer::prefetch(u32 page) {
  if (source_ == nullptr || load_of_page_[page] != FilePageSource::k_no_load) return;
  if (source_->in_flight() >= source_->capacity() || free_slots_.empty()) return;
  u32 pool_slot = 0;
  if (!take_slot(page, pool_slot)) return;
  u32 handle = FilePageSource::k_no_load;
  if (!source_->begin(page, handle)) {
    give_back_slot(page);
    return;
  }
  load_of_page_[page] = handle;
}

// One attempt to put a page in the pool. `remaining` is the frame's upload budget over what is
// actually copied; `requested` is the same budget over what is newly asked for, so a frame can
// neither copy nor start more than one budget's worth.
GeometryStreamer::Stage GeometryStreamer::stage_page(u32 page, u8* ring, u64 ring_base, u64& at,
                                                     u64& remaining, u64& requested) {
  const u64 bytes = staged_bytes(page);
  if (source_ == nullptr) {
    if (bytes > remaining || !free_one_slot()) return Stage::blocked;
    u32 pool_slot = 0;
    if (!take_slot(page, pool_slot)) return Stage::blocked;
    copy_page(page, pool_slot, ring + at);
    uploads_.push_back(Upload{page, pool_slot, ring_base + at});
    at += bytes;
    remaining -= bytes;
    resident_[page] = 1;
    stats_.uploads_bytes += bytes;
    ++stats_.uploads;
    return Stage::done;
  }

  u32 handle = load_of_page_[page];
  if (handle == FilePageSource::k_no_load) {
    if (bytes > requested) return Stage::blocked;
    if (source_->in_flight() >= source_->capacity()) return Stage::blocked;
    if (!free_one_slot()) return Stage::blocked;
    u32 pool_slot = 0;
    if (!take_slot(page, pool_slot)) return Stage::blocked;
    if (!source_->begin(page, handle)) {
      give_back_slot(page);
      return Stage::blocked;
    }
    load_of_page_[page] = handle;
    requested -= bytes;
    ++stats_.load_waits;
    return Stage::pending;
  }
  if (!source_->done(handle)) {
    ++stats_.load_waits;
    return Stage::pending;
  }
  if (!source_->complete(handle)) {
    // A read that failed or came back short. The page is dropped rather than staged: the pool
    // would otherwise hold this page's cluster descriptors over whatever was in the buffer.
    ENGINE_LOG_WARN(log_renderer, "geometry page read failed", log::field("page", page));
    cancel_load(page);
    return Stage::blocked;
  }
  if (bytes > remaining) return Stage::pending;  // the bytes are here; the budget is not
  const u32 pool_slot = slot_of_page_[page];
  u8* dst = ring + at;
  std::memcpy(dst, source_->bytes(handle), static_cast<usize>(bytes));
  // The two blocks the host owns, written over whatever the read left: the descriptors have to
  // carry *this* slot's offsets, and the 8-bit indices are packed from the triangles that just
  // landed rather than read a second time.
  patch_clusters(page, pool_slot, dst);
  pack_indices(page, dst);
  source_->release(handle);
  load_of_page_[page] = FilePageSource::k_no_load;
  uploads_.push_back(Upload{page, pool_slot, ring_base + at});
  at += bytes;
  remaining -= bytes;
  resident_[page] = 1;
  stats_.uploads_bytes += bytes;
  ++stats_.uploads;
  return Stage::done;
}

void GeometryStreamer::consume(u32 slot) {
  if (!active() || slot >= feedback_.size()) return;
  const auto* words = static_cast<const u32*>(feedback_[slot].mapped);
  const u32 written = words[0];
  const u32 count = written < scene_->max_requests() ? written : scene_->max_requests();
  if (written > scene_->max_requests()) ++stats_.overflows;
  stats_.requests += count;
  const auto* requests = reinterpret_cast<const geometry::PageRequest*>(words + 1);
  const u32* used = words + 1 + scene_->max_requests() * 3;
  // A frame of the manager, then what the frame said: the pages the cut read (so the LRU has
  // something to order by) and the pages it could not refine into.
  manager_.begin_frame();
  for (u32 p = 0; p < scene_->page_count(); ++p) {
    if (used[p] != 0) manager_.touch(p);
  }
  manager_.request(std::span<const geometry::PageRequest>(requests, count));
}

u64 GeometryStreamer::prepare(u32 slot) {
  if (!active()) return 0;
  uploads_.clear();
  ++frame_;
  retire_slots();
  // Evict once, at the top of the frame. The pool's slot count is the largest number of pages the
  // byte budget can ever hold (`GpuScene::create_streaming`), so the budget may be briefly
  // exceeded between an admission and the next frame's eviction; what it can never do is run out
  // of slots, which is the property that keeps an eviction a slot handed back rather than a
  // reshuffle of the pool.
  stats_.evictions += manager_.evict_to_budget();
  apply_evictions();
  cancel_stale_loads();

  const u64 budget = scene_->upload_budget_bytes();
  u64 remaining = budget;
  u64 requested = budget;
  const u64 ring_base = u64{slot} * budget;
  u8* ring = static_cast<u8*>(scene_->page_stage.mapped) + ring_base;
  u64 at = 0;

  // **The manager's residency and the pool's are two different things**, and the gap between them
  // is the upload budget. A page the manager holds but the pool does not is invisible to the cull
  // pass and must not be drawn out of, so `resident_` — which is what the residency words say —
  // only turns on when the bytes are actually on their way. Two kinds of page are in that gap: the
  // pinned ones, which `PageResidencyManager::reset` makes resident before anything has asked for
  // them, and anything admitted in a frame whose budget ran out. Both are served here, in page
  // order, which is coarse first.
  const geometry::PageResidency& held = manager_.page_residency();
  bool gap = false;      // this frame stages no page past here
  bool blocked = false;  // and admits nothing new either
  // In **page order**, which is coarse first and — because the layout puts a cluster's children
  // after it — is also parent before child. That is what keeps the pool ancestor-closed the way the
  // manager keeps its own residency closed: within a frame a parent page is always filled before
  // any page it refines into, and across frames the budget runs out at a page whose children are
  // later still and therefore also unfilled.
  //
  // Once there is a gap the walk goes on, but only to **start the reads** of the pages behind it.
  // That is what keeps a file-backed source from converging at one page per read latency: the
  // disk works on the queue while the frame waits for the page at the head of it. Nothing behind
  // the gap is staged, so the page order — and with it the pool's ancestor-closure — holds.
  for (u32 p = 0; p < scene_->page_count(); ++p) {
    if (resident_[p] != 0 || !held.is_resident(p)) continue;
    if (gap) {
      prefetch(p);
      continue;
    }
    const Stage staged = stage_page(p, ring, ring_base, at, remaining, requested);
    if (staged == Stage::done) continue;
    gap = true;
    // **Pending is not blocked.** A page whose reads are on their way stops *this* page being
    // staged and nothing else: the manager may go on admitting, because what keeps the pool
    // ancestor-closed is the order pages are staged in, which the loop above enforces whatever
    // the manager holds. Blocked — no budget, no pool slot, no load capacity — does stop it.
    blocked = blocked || staged == Stage::blocked;
  }
  // **Nothing new while the pool is behind the manager and cannot catch up.** The manager's
  // ancestor-closure keeps a parent resident while a child is, but the *pool* is what the cull
  // pass reads, and a parent still waiting for its bytes with a child already in a slot is exactly
  // the hole the invariant exists to prevent — the child draws over nothing. The queue has not
  // lost anything: it is served next frame, one upload budget later.
  u32 page = 0;
  while (!blocked && remaining > 0 && requested > 0 && manager_.next_request(page)) {
    // A request the queue has outlived: the page that asked for it was evicted while it waited, so
    // loading it now would put fine geometry back under a hole. It is dropped rather than served;
    // a view that still wants it is still drawing its parent and will ask again next frame.
    if (!may_admit(page)) {
      manager_.drop(page);
      ++stats_.stale;
      continue;
    }
    if (staged_bytes(page) > remaining) break;  // next frame; the request stays at the heap's head
    if (!free_one_slot()) break;                // the pool is full and nothing may go
    manager_.admit(1);
    if (stage_page(page, ring, ring_base, at, remaining, requested) == Stage::blocked) break;
  }

  // This frame's residency words. Writing the whole array is `page_count` words — 6 KB on the
  // heightfield's 1,500 pages — against the branch a dirty list would need in the middle of a
  // mapped write, and it is the only thing in the block that changes from frame to frame.
  u32* words = scene_->residency_slot(slot);
  u32 pool_pages = 0;
  for (u32 p = 0; p < scene_->page_count(); ++p) {
    words[p] = resident_[p];
    pool_pages += resident_[p];
  }

  stats_.pages_resident = manager_.resident_pages();
  stats_.pool_pages = pool_pages;
  stats_.resident_bytes = manager_.resident_bytes();
  stats_.pending = manager_.pending();
  if (source_ != nullptr) {
    stats_.file_reads = source_->reads();
    stats_.file_bytes = source_->bytes_read();
    stats_.loads_in_flight = source_->in_flight();
    stats_.host_bytes_freed = source_->released_bytes();
  }
  // Convergence: an interval starts the first frame anything is outstanding and ends the first
  // frame nothing is, so the number a summary reports is how long the last one took rather than a
  // total over the run. A read still in flight counts as outstanding, or a run measured against a
  // file-backed source would report itself converged while a page was still on its way.
  const bool busy = stats_.pending > 0 || !uploads_.empty() || stats_.loads_in_flight > 0;
  if (busy) {
    converging_ = true;
    ++converge_frames_;
  } else if (converging_) {
    stats_.frames_to_converge = converge_frames_;
    converging_ = false;
    converge_frames_ = 0;
  }
  return scene_->stream_params_address(slot);
}

// A page the manager dropped gives its slot back. The manager does not report *which* pages it
// evicted, and it does not need to: the streamer keeps its own copy of residency and the
// difference is the eviction list. It is one pass over the pages, the same pass the residency
// words are written in.
void GeometryStreamer::apply_evictions() {
  const geometry::PageResidency& now = manager_.page_residency();
  for (u32 p = 0; p < scene_->page_count(); ++p) {
    if (resident_[p] == 0 || now.is_resident(p)) continue;
    resident_[p] = 0;
    const u32 slot = slot_of_page_[p];
    if (slot == k_no_slot) continue;
    slot_of_page_[p] = k_no_slot;
    page_of_slot_[slot] = k_no_slot;
    retiring_slot_.push_back(slot);
    retiring_frame_.push_back(frame_);
  }
}

// A slot comes back into use once every frame that could still be reading it has finished, which
// is `frames_in_flight` frames after it was given up. The list is in frame order, so this is a
// drain of its head rather than a scan.
void GeometryStreamer::retire_slots() {
  u32 released = 0;
  while (released < retiring_slot_.size() &&
         frame_ - retiring_frame_[released] >= frames_in_flight_) {
    free_slots_.push_back(retiring_slot_[released]);
    ++released;
  }
  if (released == 0) return;
  const u32 left = retiring_slot_.size() - released;
  for (u32 i = 0; i < left; ++i) {
    retiring_slot_[i] = retiring_slot_[i + released];
    retiring_frame_[i] = retiring_frame_[i + released];
  }
  retiring_slot_.resize(left);
  retiring_frame_.resize(left);
}

// **The pool can run out of slots while the byte budget still has room.** A slot is sized for the
// largest page and a page is not, so a set of small pages fills the pool at a fraction of the
// budget's bytes — and then nothing more can be admitted and nothing is over budget, which is a
// scene that stops converging with requests still queued. When that happens the *slot* is the
// budget, so one eviction is forced by lowering the manager's own budget for the length of the
// call: it takes the least recently used page with nothing resident under it, exactly as a
// byte-driven eviction would, and the ancestor-closure invariant is the manager's either way.
bool GeometryStreamer::free_one_slot() {
  if (!free_slots_.empty()) return true;
  if (manager_.resident_bytes() == 0) return false;
  const u64 budget = manager_.budget_bytes();
  manager_.set_budget(manager_.resident_bytes() - 1);
  const u32 evicted = manager_.evict_to_budget();
  manager_.set_budget(budget);
  stats_.evictions += evicted;
  apply_evictions();
  retire_slots();
  return !free_slots_.empty();
}

bool GeometryStreamer::may_admit(u32 page) const noexcept {
  const geometry::ClusterPageDesc& desc = scene_->data().pages.pages[page];
  if ((desc.flags & geometry::k_page_root) != 0) return true;
  // A page that spans two DAG levels holds its own parents, and `build_cluster_pages` leaves it
  // out of its own child run, so the inverse list cannot say so.
  if (desc.level_min != desc.level_max) return true;
  const geometry::PageResidency& held = manager_.page_residency();
  for (u32 k = 0; k < parent_count_[page]; ++k) {
    if (held.is_resident(parent_pages_[parent_first_[page] + k])) return true;
  }
  return false;
}

bool GeometryStreamer::take_slot(u32 page, u32& slot) {
  if (slot_of_page_[page] != k_no_slot) {  // already in the pool: re-admitted before it was reused
    slot = slot_of_page_[page];
    return true;
  }
  if (free_slots_.empty()) return false;
  slot = free_slots_.back();
  free_slots_.pop_back();
  slot_of_page_[page] = slot;
  page_of_slot_[slot] = page;
  return true;
}

void GeometryStreamer::record_uploads(VkCommandBuffer commands) {
  if (!active() || uploads_.empty()) return;
  const SceneData& data = scene_->data();
  const bool rt = scene_->vertices.buffer != VK_NULL_HANDLE;
  for (const Upload& upload : uploads_) {
    const geometry::ClusterPageDesc& desc = data.pages.pages[upload.page];
    // The same layout the page was staged into, from the one function that knows it.
    const GpuScene::PageStage stage = scene_->page_stage_layout(upload.page);
    auto copy = [&](VkBuffer dst, u64 src_offset, u64 dst_offset, u64 bytes) {
      if (bytes == 0) return;
      const VkBufferCopy region{upload.stage + src_offset, dst_offset, bytes};
      vkCmdCopyBuffer(commands, scene_->page_stage.buffer, dst, 1, &region);
    };
    const u64 vertex_base = u64{upload.slot} * scene_->slot_vertices();
    const u64 triangle_base = u64{upload.slot} * scene_->slot_triangles();
    copy(scene_->clusters.buffer, stage.clusters,
         u64{desc.first_cluster} * sizeof(geometry::ClusterDesc),
         u64{desc.cluster_count} * sizeof(geometry::ClusterDesc));
    copy(scene_->quantized.buffer, stage.quantized, vertex_base * 3 * sizeof(u16),
         u64{desc.vertex_count} * 3 * sizeof(u16));
    copy(scene_->attributes.buffer, stage.attributes,
         vertex_base * sizeof(geometry::VertexAttributes),
         u64{desc.vertex_count} * sizeof(geometry::VertexAttributes));
    copy(scene_->triangles.buffer, stage.triangles, triangle_base * sizeof(u32),
         u64{desc.triangle_count} * sizeof(u32));
    if (!rt) continue;
    copy(scene_->vertices.buffer, stage.vertices, vertex_base * sizeof(Vec3),
         u64{desc.vertex_count} * sizeof(Vec3));
    copy(scene_->indices8.buffer, stage.indices8, triangle_base * 3, u64{desc.triangle_count} * 3);
  }
}

}  // namespace engine::renderer
