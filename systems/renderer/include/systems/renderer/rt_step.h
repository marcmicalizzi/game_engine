#pragma once

// The ray tracing chain's step (R152): the types a resize of the per-frame chain is made of. The
// functions that make and take one are `GpuScene`'s (gpu_scene.h, `resize_ray_tracing` and the
// step in two halves), defined in `gpu_scene_rt_step.cpp`; `GpuScene` names these types as its own
// (`GpuScene::RtStep`), so a caller writes them as it always did.

#include <core/base/types.h>
#include <domain/gfx/cluster_acceleration.h>
#include <domain/gfx/resources.h>

namespace engine::renderer {

// **The frame's set stands in chunks** (R152; renderer.md, "The frame's set stands in chunks"):
// the cluster structures' storage is `rt_chunk_count()` buffers of `rt_chunk_clusters()` clusters
// each (the last what is left), every one built by a command of its own into its slice of the
// set's one array of addresses, so a step makes the chunks the new capacity lacks and hands back
// the ones it gives up, and the chunks both hold stay where they are. Until then the storage was
// one buffer, 3.1 GB at the RTX 5090's default budget, made again whole at every step and held
// beside the old one for two frames — which a card the forest fills had no room for, so the step
// waited for the device and freed first: 66 to 254 ms on the owner's v41 walk. At most
// `k_rt_chunks_max` chunks (a larger capacity is cut into larger ones); `renderer.rt.
// chunk_clusters` 0 is one chunk whatever the capacity, the comparison.
inline constexpr u32 k_rt_chunks_max = gfx::k_cluster_chunks_workgroup;

// What a swapped resize hands back for the caller to free once the frames in flight are done.
struct RtRetired {
  // Records, hit entries, the set's addresses and sizes, the BLAS set's two, the scratch, the
  // cache's three references, and every chunk.
  static constexpr u32 k_buffers = 10 + k_rt_chunks_max;
  gfx::BufferResource buffers[k_buffers];
  u32 count = 0;
};

// What the last `resize_ray_tracing` spent making each part, in nanoseconds: what a slow resize
// was slow at (R116).
struct RtResizeCost {
  i64 free_ns = 0, records_ns = 0, clas_ns = 0, references_ns = 0, blas_ns = 0, scratch_ns = 0;
};

// **A step in two halves** (R152; renderer.md, "A step is made on the worker"): what a step
// makes is made from a copy of what it needs of the scene (`rt_step_inputs`), by any thread
// (`make_rt_step`: allocations and driver size queries, nothing of the scene read or written),
// and becomes the scene's on the frame's thread (`take_rt_step`), which hands the buffers it
// replaces to `retired` — or, with none, destroys them: the caller waited for the device. A step
// made for a scene that has changed since (`rt_step_current` false: another step was taken, or
// the chain was laid out again) is dropped with `drop_rt_step`. `resize_ray_tracing` is the three
// in a row on one thread.
struct RtStepInputs {
  u32 capacity = 0;    // clamped: what the step makes
  u32 references = 0;  // likewise; 0 without a cache
  bool swap = false;   // made beside the scene's set: the chunks both hold are kept
  bool keep_frame_set = false;   // the capacity stays: only the references' side is made
  bool swap_references = false;  // the cache's three reference buffers are made
  u32 chunk_clusters = 0;        // the new layout's chunk
  u32 kept_chunks = 0;           // the scene's chunks [0, kept) stay
  u64 kept_scratch_bytes = 0;    // what a build of the kept chunks' largest needs
  gfx::ClusterSetLimits limits;  // `rt_limits_` with the capacity and its geometry index
  u32 blas_structures = 0;
  u32 blas_per_instance = 0;
  u64 fixed_scratch_bytes = 0;  // the top-level build's and the cache's: the scratch is no less
  u64 scratch_bytes = 0;        // the scratch the scene holds
  u64 generation = 0;           // the scene's `rt_generation_` when the step was asked for
};

struct RtStep {
  RtStepInputs inputs;
  gfx::BufferResource records, hit_entries;
  gfx::ClusterSet outputs;                  // the addresses and sizes of the whole capacity
  gfx::ClusterSet chunks[k_rt_chunks_max];  // made: [inputs.kept_chunks, chunk_count)
  u32 chunk_count = 0;
  gfx::ClusterBlasSet blas;
  gfx::BufferResource scratch;  // made only when the builds need more than the scene's
  gfx::BufferResource references, reference_keys, misses;
  RtResizeCost cost;
};

}  // namespace engine::renderer
