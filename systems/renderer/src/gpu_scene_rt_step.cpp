// The ray tracing chain's step (R152; renderer.md, "The frame's set stands in chunks" and "A step
// is made on the worker"): what a resize of the per-frame chain asks of the scene, makes, takes
// and drops, and the chunks it frees. Moved here from gpu_scene.cpp whole and unchanged; the types
// are rt_step.h's.

#include <core/time/time.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/rt_capacity.h>

#include <algorithm>
#include <string>

namespace engine::renderer {

namespace {

constexpr gfx::BufferUsage k_address =
    gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress;

}  // namespace

// Every chunk of the frame's cluster set (R152), and a step asked for before this is another
// scene's: `rt_step_current` says so.
void GpuScene::destroy_rt_chunks() noexcept {
  for (u32 k = 0; k < k_rt_chunks_max; ++k) {
    if (device_ != nullptr) gfx::destroy_cluster_set(*device_, clas_chunks_[k]);
    clas_chunks_[k] = gfx::ClusterSet{};
  }
  rt_chunk_count_ = 0;
  ++rt_generation_;
}

// **A step, asked for** (R152; rt_step.h, "A step in two halves"): what `make_rt_step` needs of
// the scene, by value, so the making can be another thread's while the frame's thread goes on.
bool GpuScene::rt_step_inputs(u32 capacity, u32 references, bool beyond_budget, bool swap,
                              RtStepInputs& out, std::string* error) const {
  out = RtStepInputs{};
  const u32 ceiling =
      beyond_budget || rt_capacity_limit_ == 0 ? rt_union_clusters_ : rt_capacity_limit_;
  const u64 index_room = rt_cache() ? u64{gfx::k_cluster_cache_geometry}
                                    : u64{device_->features().cluster_max_geometry_index} + 1;
  capacity =
      static_cast<u32>(std::max<u64>(1, std::min({u64{capacity}, u64{ceiling}, index_room})));
  // The cache's references (renderer.md, "The resident cluster structure cache"): 0 keeps them.
  if (rt_cache()) {
    if (references == 0) references = rt_reference_capacity_;
    const u32 reference_ceiling =
        beyond_budget || rt_reference_limit_ == 0 ? rt_union_clusters_ : rt_reference_limit_;
    references = static_cast<u32>(std::max<u64>(
        1, std::min({u64{references}, u64{reference_ceiling}, u64{rt_union_clusters_}})));
  } else {
    references = 0;
  }
  out.capacity = capacity;
  out.references = references;
  // **The swap** (R116): the old buffers go to `retired`. The cache's references swap with them
  // (R130): each frame slot reads its own copy of the cache's table, written when its frame is
  // recorded (`rt_cache_tables_address`), so a frame in flight keeps the copy naming the old ones.
  // `renderer.rt.swap_references` false is the old rule, a references step that waits.
  out.swap = swap;
  out.swap_references = swap && references != rt_reference_capacity_;
  if (out.swap_references && !rt_swap_references_tunable()) {
    if (error != nullptr) *error = "the cache's references change: the resize must wait";
    return false;
  }
  // **A step of the references alone keeps the frame's own set** (R130): the records, the
  // record-to-entry table and the cluster set are sized by the capacity, so a swapped step that
  // leaves it alone makes only the references, the bottom-level set and, when it must grow, the
  // scratch. **And a step of the capacity keeps the chunks both capacities hold** (R152): whole
  // chunks are the same buffers before and after, so only the last, partial one and the chunks
  // past it are made or handed back.
  out.keep_frame_set = swap && capacity == rt_capacity_ && rt_capacity_ != 0;
  out.chunk_clusters =
      out.keep_frame_set
          ? rt_chunk_clusters_
          : renderer::rt_chunk_clusters(capacity, rt_chunk_clusters_tunable(), k_rt_chunks_max);
  out.kept_chunks =
      out.keep_frame_set ? rt_chunk_count_
      : swap ? rt_chunks_kept(rt_capacity_, rt_chunk_clusters_, capacity, out.chunk_clusters)
             : 0u;
  for (u32 k = 0; k < out.kept_chunks; ++k) {
    out.kept_scratch_bytes = std::max(out.kept_scratch_bytes, clas_chunks_[k].build_scratch_bytes);
  }
  out.limits = rt_limits_;
  out.limits.max_clusters = capacity;
  // With a cache the frame's set also builds what the cache could not place (R114), named as the
  // cache names it — the cluster within its mesh under bit 23 — so its indices reach 24 bits.
  out.limits.max_geometry_index = rt_cache() ? gfx::k_cluster_cache_geometry * 2 - 1 : capacity - 1;
  // One bottom-level set over both: an instance references the frame's own structures or the
  // cache's, never both, so its most is the larger of the two capacities.
  // And at most one structure per reference: the build is over the instances that reference
  // anything (R143, `blas_compact_main`), never over every slot.
  out.blas_structures = static_cast<u32>(
      std::max<u64>(1, std::min<u64>(rt_instance_slots_, u64{capacity} + references)));
  out.blas_per_instance = std::min(rt_max_per_instance_, std::max(capacity, references));
  // The cache's misses build in the same chain, one pass before the frame's own (the graph orders
  // the two), so they share the scratch with the top-level build.
  out.fixed_scratch_bytes = std::max(tlas.build_scratch_bytes, rt_cache_scratch_bytes_);
  out.scratch_bytes = swap ? rt_scratch.size : 0u;
  out.generation = rt_generation_;
  return true;
}

// **A step, made** (R152): every buffer the step needs and the scene does not hold, from the
// inputs alone. Allocations and the driver's size queries: any thread's.
bool GpuScene::make_rt_step(const gfx::Device& device, const RtStepInputs& in, RtStep& out,
                            std::string* error) {
  out = RtStep{};
  out.inputs = in;
  // Where the time goes, part by part: a slow resize says which (the slow resize's line).
  i64 mark = time::monotonic_ns();
  const auto lap = [&mark](i64& into) {
    const i64 now = time::monotonic_ns();
    into += now - mark;
    mark = now;
  };
  // The frame's records are a transfer source too, so a test can read the drop rule's result back
  // and hold the scan to the one-thread rule's bytes (R128).
  constexpr gfx::BufferUsage k_record_usage =
      k_address | gfx::BufferUsage::AccelerationBuildInput | gfx::BufferUsage::TransferSrc;
  const u64 record_bytes = in.limits.instantiate ? gfx::k_cluster_instantiate_record_bytes
                                                 : gfx::k_cluster_build_record_bytes;
  bool ok = true;
  u64 chunk_scratch = in.kept_scratch_bytes;
  if (!in.keep_frame_set) {
    ok = gfx::create_buffer(device, record_bytes * in.capacity, k_record_usage, false, out.records,
                            error) &&
         gfx::create_buffer(device, sizeof(u32) * u64{in.capacity}, k_address, false,
                            out.hit_entries, error);
    lap(out.cost.records_ns);
    // The arrays of the whole capacity, and the chunks the scene does not already hold: each its
    // own buffer, reserved at the driver's most for its clusters, built by its own command.
    ok = ok && gfx::create_cluster_outputs(device, in.capacity, out.outputs, error);
    out.chunk_count = renderer::rt_chunk_count(in.capacity, in.chunk_clusters);
    for (u32 k = in.kept_chunks; ok && k < out.chunk_count; ++k) {
      gfx::ClusterSetLimits limits = in.limits;
      limits.max_clusters = rt_chunk_size(in.capacity, in.chunk_clusters, k);
      ok = gfx::create_cluster_storage(device, limits, out.chunks[k], error);
      chunk_scratch = std::max(chunk_scratch, out.chunks[k].build_scratch_bytes);
    }
    lap(out.cost.clas_ns);
  }
  // The cache's references: swapped, beside the old ones, which `take_rt_step` hands back; waited
  // for, the old ones are gone already. A miss is a pending reference's key, claimed once: never
  // more of them than references.
  if (in.swap_references || (!in.swap && in.references != 0)) {
    ok = ok &&
         gfx::create_buffer(device, u64{in.references} * sizeof(u64), k_record_usage, false,
                            out.references, error) &&
         gfx::create_buffer(device, u64{in.references} * sizeof(u32), k_address, false,
                            out.reference_keys, error) &&
         gfx::create_buffer(device, u64{in.references} * gfx::k_cluster_cache_miss_bytes, k_address,
                            false, out.misses, error);
    lap(out.cost.references_ns);
  }
  ok = ok && gfx::create_cluster_blas_set(device, in.blas_structures, in.capacity + in.references,
                                          in.blas_per_instance, out.blas, error,
                                          in.limits.opacity_micromaps);
  lap(out.cost.blas_ns);
  if (ok) {
    // The largest of the three builds': a chunk's, the bottom-level set's, and the top-level
    // build's and the cache's misses', which share it (the graph orders them).
    const u64 scratch_bytes =
        std::max({chunk_scratch, out.blas.build_scratch_bytes, in.fixed_scratch_bytes});
    // Made again when the builds need more than the scene holds, or less than half of it: a step
    // that keeps most of its chunks keeps the scratch too.
    if (scratch_bytes > in.scratch_bytes || scratch_bytes * 2 < in.scratch_bytes)
      ok = gfx::create_scratch(device, scratch_bytes, out.scratch, error);
  }
  lap(out.cost.scratch_ns);
  if (!ok) drop_rt_step(device, out);
  return ok;
}

void GpuScene::drop_rt_step(const gfx::Device& device, RtStep& step) noexcept {
  gfx::destroy_buffer(device, step.records);
  gfx::destroy_buffer(device, step.hit_entries);
  gfx::destroy_cluster_set(device, step.outputs);
  for (u32 k = 0; k < k_rt_chunks_max; ++k)
    gfx::destroy_cluster_set(device, step.chunks[k]);
  gfx::destroy_cluster_blas_set(device, step.blas);
  gfx::destroy_buffer(device, step.scratch);
  gfx::destroy_buffer(device, step.references);
  gfx::destroy_buffer(device, step.reference_keys);
  gfx::destroy_buffer(device, step.misses);
  step = RtStep{};
}

void GpuScene::take_rt_step(RtStep& step, RtRetired* retired) {
  const RtStepInputs in = step.inputs;
  const gfx::Device& device = *device_;
  // The frames in flight imported the old buffers by handle: swapped, they are the caller's to free
  // once those frames have completed; waited for, the caller destroyed them before the step was
  // made and these are empty.
  const auto retire = [&](gfx::BufferResource& old) {
    if (old.buffer.valid()) {
      if (retired != nullptr && retired->count < RtRetired::k_buffers) {
        retired->buffers[retired->count++] = old;
      } else {
        gfx::destroy_buffer(device, old);
      }
    }
    old = gfx::BufferResource{};
  };
  if (!in.keep_frame_set) {
    retire(records);
    retire(hit_entries);
    retire(clas_set.addresses);
    retire(clas_set.sizes);
    for (u32 k = in.kept_chunks; k < k_rt_chunks_max; ++k) {
      retire(clas_chunks_[k].data);
      clas_chunks_[k] = gfx::ClusterSet{};
    }
    records = step.records;
    hit_entries = step.hit_entries;
    clas_set = step.outputs;
    clas_set.limits = in.limits;
    for (u32 k = in.kept_chunks; k < step.chunk_count; ++k)
      clas_chunks_[k] = step.chunks[k];
    rt_chunk_count_ = step.chunk_count;
    rt_chunk_clusters_ = in.chunk_clusters;
    // What one build of the largest chunk needs, and where a chunk's storage may start.
    clas_set.build_scratch_bytes = 0;
    for (u32 k = 0; k < rt_chunk_count_; ++k) {
      clas_set.build_scratch_bytes =
          std::max(clas_set.build_scratch_bytes, clas_chunks_[k].build_scratch_bytes);
    }
    clas_set.alignment = rt_chunk_count_ != 0 ? clas_chunks_[0].alignment : 0u;
  }
  retire(blas_set.data);
  retire(blas_set.addresses);
  blas_set = step.blas;
  if (step.scratch.buffer.valid()) {
    retire(rt_scratch);
    rt_scratch = step.scratch;
  }
  if (in.swap_references || !in.swap) {
    retire(rt_cache_buffers.references);
    retire(rt_cache_buffers.reference_keys);
    retire(rt_cache_buffers.misses);
    rt_cache_buffers.references = step.references;
    rt_cache_buffers.reference_keys = step.reference_keys;
    rt_cache_buffers.misses = step.misses;
  }
  rt_capacity_ = in.capacity;
  rt_reference_capacity_ = in.references;
  // Swapped, the table's copies are written when each slot's frame is recorded, never here: the
  // frames in flight read theirs.
  if (!in.swap) write_rt_cache_tables();
  u64 chunk_bytes = 0;
  for (u32 k = 0; k < rt_chunk_count_; ++k)
    chunk_bytes += clas_chunks_[k].data.size;
  rt_bytes_ = rt_scene_bytes_ + records.size + hit_entries.size + chunk_bytes +
              clas_set.addresses.size + clas_set.sizes.size + blas_set.data.size +
              blas_set.addresses.size + rt_scratch.size + rt_cache_buffers.references.size +
              rt_cache_buffers.reference_keys.size + rt_cache_buffers.misses.size;
  rt_resize_cost_ = step.cost;
  ++rt_generation_;
  // Everything the step made is the scene's now: nothing of it is left to drop.
  step = RtStep{};
}

u64 GpuScene::rt_step_bytes(u32 capacity, u32 references) const noexcept {
  const u32 chunk =
      renderer::rt_chunk_clusters(capacity, rt_chunk_clusters_tunable(), k_rt_chunks_max);
  const u32 kept = capacity == rt_capacity_
                       ? rt_chunk_count_
                       : rt_chunks_kept(rt_capacity_, rt_chunk_clusters_, capacity, chunk);
  u64 made = 0;
  for (u32 k = kept; k < renderer::rt_chunk_count(capacity, chunk); ++k)
    made += rt_chunk_size(capacity, chunk, k);
  return made * rt_bytes_per_cluster_ + (references != 0 && references != rt_reference_capacity_
                                             ? u64{references} * rt_bytes_per_reference_
                                             : 0u);
}

bool GpuScene::resize_ray_tracing(u32 capacity, std::string* error, bool beyond_budget,
                                  u32 references, RtRetired* retired) {
  rt_resize_cost_ = RtResizeCost{};
  if (retired != nullptr) retired->count = 0;
  if (!ray_tracing_ || device_ == nullptr) return true;
  const gfx::Device& device = *device_;
  const bool swap = retired != nullptr;
  RtStepInputs inputs;
  if (!rt_step_inputs(capacity, references, beyond_budget, swap, inputs, error)) return false;
  // Waited for, the old buffers go first, so the device never holds both sets.
  i64 free_ns = 0;
  if (!swap) {
    const i64 started = time::monotonic_ns();
    gfx::destroy_buffer(device, rt_cache_buffers.references);
    gfx::destroy_buffer(device, rt_cache_buffers.reference_keys);
    gfx::destroy_buffer(device, rt_cache_buffers.misses);
    rt_cache_buffers.references = rt_cache_buffers.reference_keys = rt_cache_buffers.misses =
        gfx::BufferResource{};
    rt_reference_capacity_ = 0;
    gfx::destroy_buffer(device, records);
    gfx::destroy_buffer(device, hit_entries);
    records = hit_entries = gfx::BufferResource{};
    gfx::destroy_cluster_set(device, clas_set);
    clas_set = gfx::ClusterSet{};
    destroy_rt_chunks();
    gfx::destroy_cluster_blas_set(device, blas_set);
    blas_set = gfx::ClusterBlasSet{};
    gfx::destroy_buffer(device, rt_scratch);
    rt_scratch = gfx::BufferResource{};
    rt_capacity_ = 0;
    free_ns = time::monotonic_ns() - started;
  }
  RtStep step;
  if (!make_rt_step(device, inputs, step, error)) {
    // Swapped, the old set is untouched and still the scene's: the caller waits and tries again.
    if (swap) return false;
    write_rt_cache_tables();
    rt_bytes_ = rt_scene_bytes_;
    return false;
  }
  take_rt_step(step, retired);
  rt_resize_cost_.free_ns = free_ns;
  return true;
}

}  // namespace engine::renderer
