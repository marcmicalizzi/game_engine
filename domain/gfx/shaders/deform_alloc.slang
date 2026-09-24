// The deformed-vertex pool's allocator (docs/plan/04-renderer.md §4.3, experiment E25's open
// item): a prefix sum over the visible entries' vertex counts, on the GPU, from the cull pass's
// own output, with no CPU round trip and nothing read back inside a frame.
//
// **Why there is an allocator at all.** E25 gave every deformed instance a block as long as its
// mesh's whole cluster-ordered vertex range, whether the frame's cut touched it or not: 12.2 MB
// for 1,024 foxes and 1.7 GB if they had been FlightHelmets, to write the ~1,300 clusters a frame
// actually deforms. The pool now holds **what the frame deforms**: one block per entry of the
// visible list, as long as that cluster's own vertex count, taken from a budget in vertices.
//
// **Why the block is per (instance, cluster) pair and not per instance.** A cluster
// acceleration-structure record — built or instantiated — points straight at its cluster's
// twelve-byte positions with no gather (E25 decision 2), so a cluster's vertices have to be
// contiguous in the pool. One instance's cut is a scattered set of clusters in its mesh's vertex
// stream, so an instance-sized block would have to be the whole mesh again. A per-entry block is
// the smallest unit that keeps the contiguity the ray side needs, and it is also the unit the
// pool pass, the rasterizers and the resolve all already have in hand: the visible entry.
//
// **Why it runs in one workgroup.** The budget is the frame's, not the run's, so the three runs
// and every view share one cursor, and the cursor has to advance by *what was placed* rather than
// by what was asked for — otherwise a block that refused one big cluster would charge the budget
// for it and refuse every small one behind it. One workgroup, looping the views inside a dispatch
// and the runs across the dispatches the render graph already orders, makes that a groupshared
// running total with no global atomic and no second pass. It costs one workgroup for the whole
// frame's allocation: about 5 µs per 1,000 visible entries, which is the same order as the pool
// pass it feeds at ordinary cut sizes and is the largest thing here worth improving (see
// docs/subsystems/renderer.md, "The deformed-vertex pool").
//
// **What it does not buy is a reproducible overflow.** The order this walks is the order the cull
// pass's atomics appended entries in, which is not the same twice. While everything fits that is
// invisible — every entry gets a block and reads its own, so the picture is identical whatever
// order the blocks were handed out in, which is what the byte-identical captures rest on. Once
// the budget is exceeded, *which* pairs fall back to the rest pose follows that order and two
// runs can differ. That is one more reason the budget is sized so it does not happen.
//
// An entry that does not fit is given `k_no_pool_slot`, which every position reader turns into
// the **rest pose**, and is counted in `gfx::DeformAlloc`. Never garbage, and never a crash.

#include "scene.slang"

// Mirrors gfx::DeformAllocParams (72 bytes).
struct AllocParams {
  uint64_t clusters;        // ClusterDesc[]
  uint64_t instances;       // InstanceDesc[]
  uint64_t visible;         // uint2[]: the whole visible list
  uint64_t visible_counts;  // uint: view 0's count word for this run; views 16 bytes apart
  uint64_t slots;           // uint[] out: one per entry of the whole visible list
  uint64_t alloc;           // DeformAlloc: cursor, entries, overflow_entries, overflow_vertices
  uint pool_vertices;       // the budget
  uint pair_count;          // one view's run is at most this long
  uint views;
  // Where view 0's run starts in the whole list; view v's is `pair_count` entries per view on. For
  // a run of the camera's views that is `(run * views) * pair_count`, run-major; for the shadow
  // cascades' runs, which follow every view's three, it is the first cascade's entry.
  uint first_entry;
  uint reset;  // 1: the frame's first allocation dispatch, which zeroes the record
  uint pad;
};

[[vk::push_constant]] ConstantBuffer<AllocParams> g_params;

static const uint k_group = 256;           // gfx::k_deform_alloc_workgroup
static const uint k_draw_args_bytes = 16;  // gfx::k_draw_args_bytes

groupshared uint s_scan[k_group];  // the block's vertex counts, scanned and then reduced in place
groupshared uint s_cursor;         // vertices handed out so far this frame

[shader("compute")]
[numthreads(k_group, 1, 1)]
void deform_alloc_main(uint3 group_thread : SV_GroupThreadID) {
  const uint lane = group_thread.x;
  uint* record = (uint*)g_params.alloc;  // cursor, entries, overflow_entries, overflow_vertices
  if (lane == 0) {
    if (g_params.reset != 0) {
      record[0] = 0;
      record[1] = 0;
      record[2] = 0;
      record[3] = 0;
    }
    s_cursor = g_params.reset != 0 ? 0u : record[0];
  }
  GroupMemoryBarrierWithGroupSync();

  ClusterDesc* clusters = (ClusterDesc*)g_params.clusters;
  InstanceDesc* instances = (InstanceDesc*)g_params.instances;
  uint2* visible = (uint2*)g_params.visible;
  uint* slots = (uint*)g_params.slots;
  Atomic<uint>* counters = (Atomic<uint>*)g_params.alloc;

  for (uint view = 0; view < g_params.views; ++view) {
    uint* count_word = (uint*)(g_params.visible_counts + uint64_t(view) * k_draw_args_bytes);
    // The run is filled by one atomic over a dispatch of pair_count threads, so that is its bound.
    const uint count = min(*count_word, g_params.pair_count);
    const uint base_entry = g_params.first_entry + view * g_params.pair_count;
    for (uint block = 0; block < count; block += k_group) {
      const uint index = block + lane;
      // A rigid instance's entry takes no pool vertices, and neither does a lane past the count.
      uint need = 0;
      if (index < count) {
        const uint2 pair = visible[base_entry + index];
        if (instances[pair.x].deform != k_invalid_deform) need = clusters[pair.y].vertex_count;
      }
      s_scan[lane] = need;
      GroupMemoryBarrierWithGroupSync();
      // Hillis-Steele inclusive scan over the block: eight rounds for 256 lanes. The simplest
      // correct scan is the right one at this scale — this runs once per 256 entries of the cut.
      for (uint offset = 1; offset < k_group; offset <<= 1) {
        const uint add = lane >= offset ? s_scan[lane - offset] : 0u;
        GroupMemoryBarrierWithGroupSync();
        s_scan[lane] += add;
        GroupMemoryBarrierWithGroupSync();
      }
      const uint inclusive = s_scan[lane];
      const uint exclusive = inclusive - need;
      const uint cursor = s_cursor;
      const uint start = cursor + exclusive;
      const bool fits = need != 0 && start + need <= g_params.pool_vertices;
      if (index < count) slots[base_entry + index] = fits ? start : k_no_pool_slot;
      if (index < count && need != 0) {
        if (fits) {
          counters[1].add(1);
        } else {
          counters[2].add(1);
          counters[3].add(need);
        }
      }
      // What the block actually placed: the largest inclusive prefix that fitted. Reducing it
      // rather than taking the block's total is what keeps the cursor exact when the budget runs
      // out in the middle of a block — the entries after the first refusal are refused too, but
      // their vertices were never handed out and must not be charged.
      GroupMemoryBarrierWithGroupSync();
      s_scan[lane] = fits ? inclusive : 0u;
      GroupMemoryBarrierWithGroupSync();
      for (uint half = k_group / 2; half > 0; half >>= 1) {
        if (lane < half) s_scan[lane] = max(s_scan[lane], s_scan[lane + half]);
        GroupMemoryBarrierWithGroupSync();
      }
      if (lane == 0) s_cursor = cursor + s_scan[0];
      GroupMemoryBarrierWithGroupSync();
    }
  }
  if (lane == 0) record[0] = s_cursor;
}
