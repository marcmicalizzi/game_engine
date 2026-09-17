// The GPU-driven half of the cluster acceleration structure path (docs/plan/04-renderer.md
// §4.4, ADR-0025), for a scene of instances: the frame's ray tracing geometry is exactly the
// frame's raster cut, with no CPU in between.
//
// A scene builds one cluster bottom-level structure per instance, so an instance's CLAS records
// have to be contiguous, while the cull pass's visible list is in whatever order its atomics
// produced. Three passes sort that out (gfx::ClusterRecordParams documents the buffers):
//
//   records_main  one thread per visible entry, bucketing it into its instance's slice of
//                 `slots` (slot = instance.first_pair + a per-instance atomic) and counting the
//                 instance's survivors. The slices are as long as the instances' meshes, so the
//                 result is sparse.
//   ranges_main   one thread: the prefix sum of the per-instance counts, the total record count
//                 the CLAS build reads from the device, and one 16-byte bottom-level record per
//                 instance over that instance's run of CLAS addresses.
//   emit_main     one thread per pair slot, moving the bucketed entries down into the dense
//                 `records` array and writing the 64-byte CLAS build record there. A record's
//                 cluster id and base geometry index are the entry's **visible index**, the same
//                 word the rasterizer puts in the visibility buffer, so the ray-traced and
//                 rasterized pictures name the same pair.

#include "scene.slang"

// Mirrors VkClusterAccelerationStructureBuildTriangleClusterInfoNV (64 bytes).
struct ClusterRecord {
  uint cluster_id;
  uint cluster_flags;
  uint counts;    // triangle_count | vertex_count << 9 | truncate bits << 18 | index type << 24
  uint geometry;  // base geometry index | flags << 29
  uint strides0;  // index buffer stride | vertex buffer stride << 16
  uint strides1;  // geometry index stride | opacity micromap index stride << 16
  uint64_t index_buffer;
  uint64_t vertex_buffer;
  uint64_t geometry_buffer;
  uint64_t omm_array;
  uint64_t omm_indices;
};

// Mirrors VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV (16 bytes).
struct BlasRecord {
  uint count;
  uint stride;
  uint64_t references;
};

// Mirrors gfx::ClusterRecordParams (120 bytes).
struct RecordParams {
  uint64_t clusters;         // ClusterDesc[]
  uint64_t vertices;         // float3[]: cluster-ordered positions
  uint64_t indices8;         // uint8[3 * triangles]: packed cluster indices
  uint64_t instances;        // InstanceDesc[instance_count]
  uint64_t visible;          // uint2[]: the cull pass's visible list
  uint64_t visible_count;    // uint: how many (the cull pass's count word)
  uint64_t slots;            // uint[pair_count]: visible index per pair slot
  uint64_t instance_counts;  // uint[instance_count]: survivors per instance (zeroed first)
  uint64_t instance_first;   // uint[instance_count]: dense record base
  uint64_t records;          // ClusterRecord[max_clusters] out
  uint64_t record_count;     // uint out
  uint64_t blas_records;     // BlasRecord[instance_count] out
  uint64_t clas_addresses;   // uint64_t[]: where the CLAS build writes each address
  uint instance_count;
  uint pair_count;
  uint max_clusters;
  uint pad;
};

[[vk::push_constant]] ConstantBuffer<RecordParams> g_params;

static const uint k_index_type_8bit = 1;
static const uint k_geometry_opaque = 4;

// Bucket each visible entry into its instance's slice of the pair-indexed scratch array.
[shader("compute")]
[numthreads(64, 1, 1)]
void records_main(uint3 thread_id : SV_DispatchThreadID) {
  const uint index = thread_id.x;
  uint* visible_count = (uint*)g_params.visible_count;
  const uint count = min(*visible_count, g_params.max_clusters);
  if (index >= count) return;
  uint2* visible = (uint2*)g_params.visible;
  const uint instance_index = visible[index].x;
  InstanceDesc* instances = (InstanceDesc*)g_params.instances;
  Atomic<uint>* counts = (Atomic<uint>*)g_params.instance_counts;
  const uint local = counts[instance_index].add(1);
  uint* slots = (uint*)g_params.slots;
  slots[instances[instance_index].first_pair + local] = index;
}

// The prefix sum over the instances, the total, and one bottom-level record per instance.
[shader("compute")]
[numthreads(1, 1, 1)]
void ranges_main() {
  uint* counts = (uint*)g_params.instance_counts;
  uint* first = (uint*)g_params.instance_first;
  BlasRecord* blas = (BlasRecord*)g_params.blas_records;
  uint total = 0;
  for (uint i = 0; i < g_params.instance_count; ++i) {
    first[i] = total;
    BlasRecord record;
    record.count = counts[i];
    record.stride = 8;
    record.references = g_params.clas_addresses + uint64_t(total) * 8;
    blas[i] = record;
    total += counts[i];
  }
  uint* record_count = (uint*)g_params.record_count;
  *record_count = total;
}

// Move the bucketed entries down to the dense array and write their CLAS build records.
[shader("compute")]
[numthreads(64, 1, 1)]
void emit_main(uint3 thread_id : SV_DispatchThreadID) {
  const uint slot = thread_id.x;
  if (slot >= g_params.pair_count) return;
  InstanceDesc* instances = (InstanceDesc*)g_params.instances;
  const uint instance_index = instance_of_pair(instances, g_params.instance_count, slot);
  const uint local = slot - instances[instance_index].first_pair;
  uint* counts = (uint*)g_params.instance_counts;
  if (local >= counts[instance_index]) return;  // this instance drew fewer clusters than it has
  uint* slots = (uint*)g_params.slots;
  const uint visible_index = slots[slot];
  uint2* visible = (uint2*)g_params.visible;
  const uint cluster_id = visible[visible_index].y;
  ClusterDesc* clusters = (ClusterDesc*)g_params.clusters;
  const ClusterDesc cluster = clusters[cluster_id];
  uint* first = (uint*)g_params.instance_first;
  const uint dense = first[instance_index] + local;

  ClusterRecord record;
  record.cluster_id = visible_index;
  record.cluster_flags = 0;
  record.counts = (cluster.triangle_count & 0x1ff) | ((cluster.vertex_count & 0x1ff) << 9) |
                  (k_index_type_8bit << 24);
  record.geometry = (visible_index & 0xffffff) | (k_geometry_opaque << 29);
  record.strides0 = 1 | (12 << 16);
  record.strides1 = 0;
  record.index_buffer = g_params.indices8 + uint64_t(cluster.triangle_offset) * 3;
  record.vertex_buffer = g_params.vertices + uint64_t(cluster.vertex_offset) * 12;
  record.geometry_buffer = 0;
  record.omm_array = 0;
  record.omm_indices = 0;
  ClusterRecord* records = (ClusterRecord*)g_params.records;
  records[dense] = record;
}
