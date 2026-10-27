// **Test only**: the one-workgroup allocator `deform_alloc.slang` was until 2026-10-26 (roadmap
// R104), kept as the reference the device-wide scan is held to. `deform_alloc_tests.cpp` runs
// both on the same visible list and requires the same slot table and the same record, so this
// file is the placement rule in executable form: entries are taken in blocks of 256 (per view,
// views in order, runs in the order the dispatches come), a block's entry is placed when the
// cursor plus its exclusive prefix within the block plus its own count fits the budget, and the
// cursor advances by the largest inclusive prefix that fitted — *what was placed*, never what was
// asked for. It ships in no renderer; `deform_alloc.slang` says why the rule is what it is.
//
// The body is the old shader's, unchanged but for the entry point's name; the push constants are
// the new struct, whose two trailing fields (`blocks`, `groups`) this one never reads.

#include "scene.slang"

// Mirrors gfx::DeformAllocParams (96 bytes).
struct AllocParams {
  uint64_t clusters;
  uint64_t instances;
  uint64_t visible;
  uint64_t visible_counts;
  uint64_t slots;
  uint64_t alloc;
  uint pool_vertices;
  uint pair_count;
  uint views;
  uint first_entry;
  uint reset;
  float rest_beyond_m;
  uint64_t eye;
  uint64_t blocks;  // unused here
  uint groups;      // unused here
  uint pad;
};

[[vk::push_constant]] ConstantBuffer<AllocParams> g_params;

static const uint k_group = 256;
static const uint k_draw_args_bytes = 16;

groupshared uint s_scan[k_group];
groupshared uint s_cursor;

[shader("compute")]
[numthreads(k_group, 1, 1)]
void deform_alloc_reference_main(uint3 group_thread : SV_GroupThreadID) {
  const uint lane = group_thread.x;
  uint* record = (uint*)g_params.alloc;
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
    const uint count = min(*count_word, g_params.pair_count);
    const uint base_entry = g_params.first_entry + view * g_params.pair_count;
    for (uint block = 0; block < count; block += k_group) {
      const uint index = block + lane;
      uint need = 0;
      if (index < count) {
        const uint2 pair = visible[base_entry + index];
        const InstanceDesc instance = instances[pair.x];
        if (instance.deform != k_invalid_deform) need = clusters[pair.y].vertex_count;
        if (need != 0 && g_params.rest_beyond_m >= 0.0) {
          if ((instance.flags & k_instance_shadow_rest) != 0) {
            need = 0;
          } else if (g_params.rest_beyond_m > 0.0) {
            const float3 from_eye = instance_from_eye(instance, load_eye(g_params.eye));
            if (dot(from_eye, from_eye) > g_params.rest_beyond_m * g_params.rest_beyond_m) need = 0;
          }
        }
      }
      s_scan[lane] = need;
      GroupMemoryBarrierWithGroupSync();
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
