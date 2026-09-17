// The GPU-driven half of the cluster acceleration structure path (docs/plan/04-renderer.md
// §4.4, ADR-0025): one thread per entry of the cull pass's visible list writes the 64-byte
// build record of that cluster's CLAS (gfx::cluster_acceleration.h documents the layout), and
// thread 0 writes the record count the CLAS build reads from the device and the 16-byte record
// of the cluster bottom-level build (count, stride, address of the CLAS address array). The
// frame's ray tracing geometry is therefore exactly the frame's raster cut, with no CPU in
// between.

// Mirrors geometry::ClusterDesc (48 bytes).
struct ClusterDesc {
  uint vertex_offset;
  uint triangle_offset;
  uint vertex_count;
  uint triangle_count;
  float3 center;
  float radius;
  float3 cone_apex;
  uint cone;
};

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

struct RecordParams {
  uint64_t clusters;        // ClusterDesc[]
  uint64_t vertices;        // float3[]: cluster-ordered positions
  uint64_t indices8;        // uint8[3 * triangles]: packed cluster indices, triangle_offset * 3 in
  uint64_t visible;         // uint[]: the cull pass's visible list
  uint64_t visible_count;   // uint: how many (the cull pass's count word)
  uint64_t records;         // ClusterRecord[max_clusters] out
  uint64_t record_count;    // uint out
  uint64_t blas_record;     // BlasRecord out
  uint64_t clas_addresses;  // uint64_t[]: where the CLAS build writes each address
  uint max_clusters;
  uint pad;
};

[[vk::push_constant]] ConstantBuffer<RecordParams> g_params;

static const uint k_index_type_8bit = 1;
static const uint k_geometry_opaque = 4;

[shader("compute")]
[numthreads(64, 1, 1)]
void records_main(uint3 thread_id : SV_DispatchThreadID) {
  const uint index = thread_id.x;
  uint* visible_count = (uint*)g_params.visible_count;
  const uint count = min(*visible_count, g_params.max_clusters);
  if (index == 0) {
    uint* record_count = (uint*)g_params.record_count;
    *record_count = count;
    BlasRecord* blas = (BlasRecord*)g_params.blas_record;
    blas->count = count;
    blas->stride = 8;
    blas->references = g_params.clas_addresses;
  }
  if (index >= count) return;
  uint* visible = (uint*)g_params.visible;
  const uint cluster_id = visible[index];
  ClusterDesc* clusters = (ClusterDesc*)g_params.clusters;
  const ClusterDesc cluster = clusters[cluster_id];
  ClusterRecord record;
  record.cluster_id = cluster_id;
  record.cluster_flags = 0;
  record.counts = (cluster.triangle_count & 0x1ff) | ((cluster.vertex_count & 0x1ff) << 9) |
                  (k_index_type_8bit << 24);
  record.geometry = (cluster_id & 0xffffff) | (k_geometry_opaque << 29);
  record.strides0 = 1 | (12 << 16);
  record.strides1 = 0;
  record.index_buffer = g_params.indices8 + uint64_t(cluster.triangle_offset) * 3;
  record.vertex_buffer = g_params.vertices + uint64_t(cluster.vertex_offset) * 12;
  record.geometry_buffer = 0;
  record.omm_array = 0;
  record.omm_indices = 0;
  ClusterRecord* records = (ClusterRecord*)g_params.records;
  records[index] = record;
}
