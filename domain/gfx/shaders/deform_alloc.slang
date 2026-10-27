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
// **The placement rule.** The budget is the frame's, not the run's, so the three runs and every
// view share one cursor, and the cursor has to advance by *what was placed* rather than by what
// was asked for — otherwise a block that refused one big cluster would charge the budget for it
// and refuse every small one behind it. The entries are taken in **tiles** of 256 (per view,
// views in order; the runs in the order the render graph gives their dispatches): a tile's entry
// is placed when the cursor plus its exclusive prefix within the tile plus its own count fits the
// budget, and the cursor then advances by the largest inclusive prefix that fitted. Until
// 2026-10-26 one workgroup walked the tiles in that order, which was right at E25's 1,300 clusters
// a frame and 15 ms at the forest's grass clearing, whose sward puts hundreds of thousands of
// entries in the pool (roadmap R104). `deform_alloc_reference.slang` keeps that walk as the
// reference this file is tested against, word for word.
//
// **The same answer as three dispatches** (reduce, then scan, then place):
//
//   1. `deform_alloc_tiles_main`, one workgroup per tile: every entry's vertex count, the tile's
//      inclusive scan written into `slots` (scratch until pass 3), and per tile two words in
//      `blocks`: the tile's total and its first non-zero inclusive prefix (the smallest count
//      the tile could place at all).
//   2. `deform_alloc_scan_main`, one workgroup: the tiles' cursors. Below the budget a tile
//      places everything, so its cursor is the unconditional prefix of the totals — a parallel
//      scan, 256 tiles a step. At the first tile whose total no longer fits (the **crossing**) the
//      placed and the unconditional prefixes part, and from there the cursor is walked tile by
//      tile, by one thread over the totals already in shared memory: a tile that fits whole
//      advances it by its total, one whose first count does not fit leaves it, and only a tile
//      that places part of itself costs the group a search over its 256 prefixes. After the
//      crossing the room left is less than the cluster that crossed it (at most 255 vertices,
//      geometry's cluster bound), and every partial tile places at least one cluster, so the
//      searches are bounded by that room over the smallest cluster, whatever the cut's size.
//   3. `deform_alloc_place_main`, one workgroup per tile: each entry's block from its tile's
//      cursor and its prefix, the counters reduced in shared memory and added once per group.
//
// Every number this computes is an integer sum or a comparison, so the result is a function of the
// visible list alone, whatever the GPU's scheduling: no atomic decides an order, no group waits
// on another (a single-pass decoupled look-back would, and Vulkan promises no forward progress
// between workgroups, which the Maxwell and Pascal runners are the cards to find out on). The
// three passes are separate render-graph passes so the graph puts the barriers between them.
//
// **What it does not buy is a reproducible overflow.** The order the tiles cover is the order the
// cull pass's atomics appended entries in, which is not the same twice. While everything fits
// that is invisible — every entry gets a block and reads its own, so the picture is identical
// whatever order the blocks were handed out in, which is what the byte-identical captures rest
// on. Once the budget is exceeded, *which* pairs fall back to the rest pose follows that order and
// two runs can differ. That is one more reason the budget is sized so it does not happen.
//
// An entry that does not fit is given `k_no_pool_slot`, which every position reader turns into
// the **rest pose**, and is counted in `gfx::DeformAlloc`. Never garbage, and never a crash.

#include "scene.slang"

// Mirrors gfx::DeformAllocParams (96 bytes).
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
  // The cascades' rest pose (R95; gfx::DeformAllocParams): negative on a camera run's dispatch. On
  // the cascades', an entry of a `k_instance_shadow_rest` instance takes no block, nor with a
  // positive distance one whose instance is farther than it from the eye.
  float rest_beyond_m;
  uint64_t eye;     // FrameEye*, read only for a positive `rest_beyond_m`
  uint64_t blocks;  // uint2 per tile of this dispatch: {total, first} then {cursor, first}
  uint groups;      // workgroups in passes 1 and 3, which stride over the tiles by it
  uint pad;
};

[[vk::push_constant]] ConstantBuffer<AllocParams> g_params;

static const uint k_group = 256;           // gfx::k_deform_alloc_workgroup
static const uint k_draw_args_bytes = 16;  // gfx::k_draw_args_bytes

groupshared uint s_scan[k_group];
groupshared uint s_total[k_group];  // pass 2: the chunk's tile totals
groupshared uint s_first[k_group];  // pass 2: the chunk's first counts
groupshared uint s_cursor;          // pass 2: the cursor
groupshared uint s_at;              // pass 2: the next tile of the chunk to walk
groupshared uint s_min;             // pass 1: the tile's first non-zero inclusive prefix
groupshared uint s_counts[3];       // pass 3: placed entries, refused entries, refused vertices

// One view's run of this dispatch: bounded by the dispatch the cull pass appended it with.
uint view_count(uint view) {
  uint* count_word = (uint*)(g_params.visible_counts + uint64_t(view) * k_draw_args_bytes);
  return min(*count_word, g_params.pair_count);
}

uint tile_count() {
  uint tiles = 0;
  for (uint view = 0; view < g_params.views; ++view) tiles += (view_count(view) + k_group - 1) / k_group;
  return tiles;
}

// Tile `tile` of the dispatch (views in order, each view's tiles in entry order): the whole list's
// entry its first lane covers, and how many of its 256 lanes hold an entry.
void locate(uint tile, out uint first_entry, out uint entries) {
  uint left = tile;
  first_entry = 0;
  entries = 0;
  for (uint view = 0; view < g_params.views; ++view) {
    const uint count = view_count(view);
    const uint tiles = (count + k_group - 1) / k_group;
    if (left < tiles) {
      first_entry = g_params.first_entry + view * g_params.pair_count + left * k_group;
      entries = min(k_group, count - left * k_group);
      return;
    }
    left -= tiles;
  }
}

// The vertices one entry asks for: its cluster's count when its instance is deformed, nothing
// when it is rigid — and, on the cascades' dispatch, nothing when its species casts its rest pose
// or it stands past the distance the trees' shadows stop swaying at (renderer.md, "The caster
// policy"); such an entry gets `k_no_pool_slot`, which every reader draws from the grid, and is
// counted as no overflow because it asked for nothing.
uint entry_need(uint entry) {
  ClusterDesc* clusters = (ClusterDesc*)g_params.clusters;
  InstanceDesc* instances = (InstanceDesc*)g_params.instances;
  uint2* visible = (uint2*)g_params.visible;
  const uint2 pair = visible[entry];
  const InstanceDesc instance = instances[pair.x];
  uint need = 0;
  if (instance.deform != k_invalid_deform) need = clusters[pair.y].vertex_count;
  if (need != 0 && g_params.rest_beyond_m >= 0.0) {
    if ((instance.flags & k_instance_shadow_rest) != 0) {
      need = 0;
    } else if (g_params.rest_beyond_m > 0.0) {
      const float3 from_eye = instance_from_eye(instance, load_eye(g_params.eye));
      if (dot(from_eye, from_eye) > g_params.rest_beyond_m * g_params.rest_beyond_m) need = 0;
    }
  }
  return need;
}

// Hillis-Steele inclusive scan of `s_scan`, eight rounds for 256 lanes. The simplest correct
// scan is the right one here: a tile is 256 entries, and the passes are bound by their gathers.
void scan_shared(uint lane) {
  for (uint offset = 1; offset < k_group; offset <<= 1) {
    const uint add = lane >= offset ? s_scan[lane - offset] : 0u;
    GroupMemoryBarrierWithGroupSync();
    s_scan[lane] += add;
    GroupMemoryBarrierWithGroupSync();
  }
}

// ---- pass 1: the tiles' scans ------------------------------------------------------------------

[shader("compute")]
[numthreads(k_group, 1, 1)]
void deform_alloc_tiles_main(uint3 group : SV_GroupID, uint3 group_thread : SV_GroupThreadID) {
  const uint lane = group_thread.x;
  uint* slots = (uint*)g_params.slots;
  uint2* blocks = (uint2*)g_params.blocks;
  const uint tiles = tile_count();
  for (uint tile = group.x; tile < tiles; tile += g_params.groups) {
    uint first_entry;
    uint entries;
    locate(tile, first_entry, entries);
    const uint need = lane < entries ? entry_need(first_entry + lane) : 0u;
    s_scan[lane] = need;
    if (lane == 0) s_min = ~0u;
    GroupMemoryBarrierWithGroupSync();
    scan_shared(lane);
    const uint inclusive = s_scan[lane];
    // The scan is scratch in the entry's own slot word until pass 3 turns it into the block.
    if (lane < entries) slots[first_entry + lane] = inclusive;
    if (need != 0) InterlockedMin(s_min, inclusive);
    GroupMemoryBarrierWithGroupSync();
    if (lane == k_group - 1) blocks[tile] = uint2(inclusive, s_min);
    GroupMemoryBarrierWithGroupSync();
  }
}

// ---- pass 2: the tiles' cursors ----------------------------------------------------------------

[shader("compute")]
[numthreads(k_group, 1, 1)]
void deform_alloc_scan_main(uint3 group_thread : SV_GroupThreadID) {
  const uint lane = group_thread.x;
  uint* record = (uint*)g_params.alloc;  // cursor, entries, overflow_entries, overflow_vertices
  uint* slots = (uint*)g_params.slots;
  uint2* blocks = (uint2*)g_params.blocks;
  const uint budget = g_params.pool_vertices;
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

  const uint tiles = tile_count();
  for (uint chunk = 0; chunk < tiles; chunk += k_group) {
    const uint n = min(k_group, tiles - chunk);
    const uint2 b = lane < n ? blocks[chunk + lane] : uint2(0u, ~0u);
    s_total[lane] = b.x;
    s_first[lane] = b.y;
    s_scan[lane] = b.x;
    GroupMemoryBarrierWithGroupSync();
    scan_shared(lane);
    const uint cursor = s_cursor;
    const uint chunk_total = s_scan[k_group - 1];
    if (chunk_total <= budget - cursor) {
      // The whole chunk fits: every tile's cursor is the unconditional prefix.
      if (lane < n) blocks[chunk + lane].x = cursor + s_scan[lane] - b.x;
      GroupMemoryBarrierWithGroupSync();
      if (lane == 0) s_cursor = cursor + chunk_total;
      GroupMemoryBarrierWithGroupSync();
      continue;
    }
    // The crossing is in this chunk: walk it, one thread over shared memory, stopping only for a
    // tile that places part of itself.
    if (lane == 0) s_at = 0;
    GroupMemoryBarrierWithGroupSync();
    for (;;) {
      if (lane == 0) {
        uint at = s_at;
        uint c = s_cursor;
        for (; at < n; ++at) {
          blocks[chunk + at].x = c;
          const uint room = budget - c;
          if (s_total[at] <= room) {
            c += s_total[at];
          } else if (s_first[at] <= room) {
            break;  // a partial tile: the group searches it below
          }
          // else: not even its first cluster fits, and the cursor stays where it is.
        }
        s_at = at;
        s_cursor = c;
      }
      GroupMemoryBarrierWithGroupSync();
      const uint at = s_at;
      if (at >= n) break;
      // What the partial tile places: the largest inclusive prefix within the room, which pass 1
      // left in the slot words.
      uint first_entry;
      uint entries;
      locate(chunk + at, first_entry, entries);
      const uint room = budget - s_cursor;
      const uint inclusive = lane < entries ? slots[first_entry + lane] : 0u;
      s_scan[lane] = inclusive <= room ? inclusive : 0u;
      GroupMemoryBarrierWithGroupSync();
      for (uint half = k_group / 2; half > 0; half >>= 1) {
        if (lane < half) s_scan[lane] = max(s_scan[lane], s_scan[lane + half]);
        GroupMemoryBarrierWithGroupSync();
      }
      if (lane == 0) {
        s_cursor += s_scan[0];
        s_at = at + 1;
      }
      GroupMemoryBarrierWithGroupSync();
    }
  }
  if (lane == 0) record[0] = s_cursor;
}

// ---- pass 3: the blocks ------------------------------------------------------------------------

[shader("compute")]
[numthreads(k_group, 1, 1)]
void deform_alloc_place_main(uint3 group : SV_GroupID, uint3 group_thread : SV_GroupThreadID) {
  const uint lane = group_thread.x;
  uint* slots = (uint*)g_params.slots;
  uint2* blocks = (uint2*)g_params.blocks;
  const uint budget = g_params.pool_vertices;
  if (lane < 3) s_counts[lane] = 0;
  GroupMemoryBarrierWithGroupSync();
  const uint tiles = tile_count();
  for (uint tile = group.x; tile < tiles; tile += g_params.groups) {
    uint first_entry;
    uint entries;
    locate(tile, first_entry, entries);
    const uint cursor = blocks[tile].x;
    const uint inclusive = lane < entries ? slots[first_entry + lane] : 0u;
    s_scan[lane] = inclusive;
    GroupMemoryBarrierWithGroupSync();
    const uint exclusive = lane > 0 ? s_scan[lane - 1] : 0u;
    const uint need = inclusive - exclusive;
    const uint start = cursor + exclusive;
    // The reference's test, term for term: the cursor never passes the budget, so `room` holds.
    const bool fits = need != 0 && exclusive + need <= budget - cursor;
    if (lane < entries) {
      slots[first_entry + lane] = fits ? start : k_no_pool_slot;
      if (need != 0) {
        if (fits) {
          InterlockedAdd(s_counts[0], 1u);
        } else {
          InterlockedAdd(s_counts[1], 1u);
          InterlockedAdd(s_counts[2], need);
        }
      }
    }
    GroupMemoryBarrierWithGroupSync();
  }
  if (lane < 3 && s_counts[lane] != 0) {
    Atomic<uint>* counters = (Atomic<uint>*)g_params.alloc;
    counters[1 + lane].add(s_counts[lane]);
  }
}
