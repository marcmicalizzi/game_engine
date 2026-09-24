// The GPU-driven half of the cluster acceleration structure path (docs/plan/04-renderer.md
// §4.4, ADR-0025), for a scene of instances: the frame's ray tracing geometry is exactly the
// frame's raster cut, with no CPU in between.
//
// A scene builds one cluster bottom-level structure per instance, so an instance's CLAS records
// have to be contiguous, while the cull pass's visible list is in whatever order its atomics
// produced. Three passes sort that out (gfx::ClusterRecordParams documents the buffers):
//
//   records_main  one thread per visible entry, bucketing it into its instance's slice of
//                 `slots` (slot = instance.first_pair * views + a per-instance atomic) and
//                 counting the instance's survivors. The slices are as long as the instances'
//                 meshes times the view count, so the result is sparse.
//   ranges_main   one thread: the prefix sum of the per-instance counts, the total record count
//                 the CLAS build reads from the device, and one 16-byte bottom-level record per
//                 instance over that instance's run of CLAS addresses.
//   emit_main     one thread per pair slot, moving the bucketed entries down into the dense
//                 `records` array and writing the 64-byte CLAS build record there. A record's
//                 cluster id and base geometry index are the entry's **visible index**, the same
//                 word the rasterizer puts in the visibility buffer, so the ray-traced and
//                 rasterized pictures name the same pair.
//
// A deformed instance's record points at the frame's deformed-vertex pool instead of the mesh's
// rest-pose positions, so the ray geometry deforms with the raster geometry (§4.3). With
// `instantiate` set, emit_main writes 32-byte instantiate records naming each cluster's prebuilt
// template (MeshDesc::templates) and this frame's positions for it, which is what cluster
// acceleration structure templates exist for.
//
// With more than one view (§4.6) the three passes build the **union** of the views' cuts under
// one top-level structure. The visible list is laid out run-major — run r of view v starts at
// `(r * views + v) * pair_count` — so the *first* run of every view is one contiguous range at
// the front of the list, which is exactly the range the ray tracing chain consumes: one
// records_main dispatch covers `views * pair_count` entries, reads each view's own count word out
// of its indirect argument block, and `views` widens every instance's slice of `slots` so that a
// cluster drawn by every view has a slot in each. A record's base geometry index stays the entry's
// visible index, unique across the frame because the runs share one list.
//
// **Shadow casters** (docs/subsystems/geometry.md, "Normal cones"): with `caster_count` set the
// cull pass also left the pairs its cone test alone rejected in run `k_caster_run` of every view,
// and records_main covers that run too. A caster lands in its instance's slice like a drawn pair
// (in one view a pair is one or the other, never both, so the slices are still long enough) and
// is written **without the opaque flag**: the shadow rays force opacity and stop on it, the
// primary rays cull non-opaque geometry and pass through, so the traced picture stays the
// rasterized one and only the shadows gain the clusters that face away from the camera.

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

// Mirrors VkClusterAccelerationStructureInstantiateClusterInfoNV (32 bytes): what the templates
// variant writes instead. The offsets are added to the template's own cluster id and base
// geometry index, both built as zero, so they are the visible entry outright.
struct InstantiateRecord {
  uint cluster_id_offset;
  uint geometry_index_offset;  // 24 bits, then 8 reserved
  uint64_t template_address;
  uint64_t vertex_address;
  uint64_t vertex_stride;
};

// Mirrors VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV (16 bytes).
struct BlasRecord {
  uint count;
  uint stride;
  uint64_t references;
};

// Mirrors gfx::ClusterRecordParams (128 bytes).
struct RecordParams {
  uint64_t clusters;         // ClusterDesc[]
  uint64_t vertices;         // float3[]: cluster-ordered rest positions
  uint64_t indices8;         // uint8[3 * triangles]: packed cluster indices
  uint64_t instances;        // InstanceDesc[instance_count]
  uint64_t meshes;           // MeshDesc[]: the deformed-vertex pool and the cluster templates
  uint64_t visible;        // uint2[]: the cull pass's visible list, from its first run
  uint64_t visible_count;  // uint: view 0's count word; view v's is k_draw_args_bytes further on
  uint64_t slots;            // uint[views * pair_count]: visible index per pair slot
  // uint[2 * instance_count]: survivors per instance (zeroed first), then the dense record bases
  uint64_t instance_counts;
  uint64_t caster_count;     // uint: view 0's caster count word, or 0 for no caster run
  uint64_t records;          // ClusterRecord[] or InstantiateRecord[] out, views * pair_count long
  uint64_t record_count;     // uint out
  uint64_t blas_records;     // BlasRecord[instance_count] out
  uint64_t clas_addresses;   // uint64_t[]: where the CLAS build writes each address
  uint instance_count;
  uint pair_count;   // the scene's; one run's count cannot exceed it
  uint views;        // how many views' runs accumulate into this set
  uint instantiate;  // 1: write InstantiateRecord from MeshDesc::templates instead
};

[[vk::push_constant]] ConstantBuffer<RecordParams> g_params;

static const uint k_index_type_8bit = 1;
static const uint k_geometry_opaque = 4;
static const uint k_draw_args_bytes = 16;  // gfx::k_draw_args_bytes: one block per view
static const uint k_caster_run = 2;        // gfx::k_caster_run: the casters' run of the list

// The first visible index of the caster run: nothing below it is a caster.
uint caster_base() { return k_caster_run * g_params.views * g_params.pair_count; }

// Each instance's dense record base: the second half of `instance_counts`.
uint* instance_first() {
  return (uint*)(g_params.instance_counts + uint64_t(g_params.instance_count) * 4);
}

// Bucket each visible entry into its instance's slice of the pair-indexed scratch array. The
// dispatch covers every view's first run at once, because those runs are contiguous, and then —
// when there is a caster run — every view's caster run, which is contiguous too.
[shader("compute")]
[numthreads(64, 1, 1)]
void records_main(uint3 thread_id : SV_DispatchThreadID) {
  const uint run_entries = g_params.views * g_params.pair_count;
  const uint runs = g_params.caster_count != 0 ? 2u : 1u;
  if (thread_id.x >= runs * run_entries) return;
  const bool caster = thread_id.x >= run_entries;
  const uint within = caster ? thread_id.x - run_entries : thread_id.x;
  const uint view = within / g_params.pair_count;
  const uint local_index = within - view * g_params.pair_count;
  const uint64_t count_word = caster ? g_params.caster_count : g_params.visible_count;
  uint* visible_count = (uint*)(count_word + uint64_t(view) * k_draw_args_bytes);
  // The run is filled by one atomic over a dispatch of pair_count threads, so that is its bound.
  if (local_index >= min(*visible_count, g_params.pair_count)) return;
  const uint index = caster ? caster_base() + within : within;
  uint2* visible = (uint2*)g_params.visible;
  const uint instance_index = visible[index].x;
  InstanceDesc* instances = (InstanceDesc*)g_params.instances;
  Atomic<uint>* counts = (Atomic<uint>*)g_params.instance_counts;
  const uint local = counts[instance_index].add(1);
  uint* slots = (uint*)g_params.slots;
  slots[instances[instance_index].first_pair * g_params.views + local] = index;
}

// The prefix sum over the instances, the total, and one bottom-level record per instance.
[shader("compute")]
[numthreads(1, 1, 1)]
void ranges_main() {
  uint* counts = (uint*)g_params.instance_counts;
  uint* first = instance_first();
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

// Where a cluster's twelve-byte positions live this frame: the mesh's rest-pose float array for
// a rigid instance, and this **visible entry's** block of the deformed-vertex pool for a deformed
// one. The block is exactly the cluster's vertex range and is contiguous, which is what lets a
// build or an instantiate record address it with no gather (E25 decision 2). An entry the frame's
// pool budget had no room for falls back to the rest-pose float positions — the same fallback the
// rasterizers take, so the ray geometry and the picture stay the same surface.
uint64_t cluster_vertex_address(MeshDesc mesh, InstanceDesc instance, ClusterDesc cluster,
                                uint visible_index) {
  const uint base = deform_base(mesh, instance, visible_index);
  if (base != k_no_pool_slot) return mesh.deform_pool + uint64_t(base) * 12;
  return g_params.vertices + uint64_t(cluster.vertex_offset) * 12;
}

// Move the bucketed entries down to the dense array and write their CLAS build records, or, in
// the templates variant, the instantiate records that name the cluster's template and this
// frame's positions for it.
[shader("compute")]
[numthreads(64, 1, 1)]
void emit_main(uint3 thread_id : SV_DispatchThreadID) {
  const uint slot = thread_id.x;
  if (slot >= g_params.views * g_params.pair_count) return;
  InstanceDesc* instances = (InstanceDesc*)g_params.instances;
  // An instance's slice is `views` times its mesh's cluster count, so dividing the slot by the
  // view count lands back in the pair space the prefix sum `first_pair` is in.
  const uint instance_index =
      instance_of_pair(instances, g_params.instance_count, slot / g_params.views);
  const InstanceDesc instance = instances[instance_index];
  const uint local = slot - instance.first_pair * g_params.views;
  uint* counts = (uint*)g_params.instance_counts;
  if (local >= counts[instance_index]) return;  // this instance drew fewer clusters than it has
  uint* slots = (uint*)g_params.slots;
  const uint visible_index = slots[slot];
  uint2* visible = (uint2*)g_params.visible;
  const uint cluster_id = visible[visible_index].y;
  ClusterDesc* clusters = (ClusterDesc*)g_params.clusters;
  const ClusterDesc cluster = clusters[cluster_id];
  MeshDesc* meshes = (MeshDesc*)g_params.meshes;
  const MeshDesc mesh = meshes[instance.mesh];
  uint* first = instance_first();
  const uint dense = first[instance_index] + local;
  const uint64_t vertex_address = cluster_vertex_address(mesh, instance, cluster, visible_index);

  if (g_params.instantiate != 0) {
    uint64_t* templates = (uint64_t*)mesh.templates;
    InstantiateRecord record;
    record.cluster_id_offset = visible_index;
    record.geometry_index_offset = visible_index & 0xffffff;
    record.template_address = templates[cluster_id];
    record.vertex_address = vertex_address;
    record.vertex_stride = 12;
    InstantiateRecord* records = (InstantiateRecord*)g_params.records;
    records[dense] = record;
    return;
  }

  ClusterRecord record;
  record.cluster_id = visible_index;
  record.cluster_flags = 0;
  record.counts = (cluster.triangle_count & 0x1ff) | ((cluster.vertex_count & 0x1ff) << 9) |
                  (k_index_type_8bit << 24);
  // Everything the picture draws is opaque; a shadow caster is not, which is the whole of how the
  // primary rays leave it out (RAY_FLAG_CULL_NON_OPAQUE) while the shadow rays keep it.
  const bool opaque = g_params.caster_count == 0 || visible_index < caster_base();
  record.geometry = (visible_index & 0xffffff) | ((opaque ? k_geometry_opaque : 0u) << 29);
  record.strides0 = 1 | (12 << 16);
  record.strides1 = 0;
  record.index_buffer = g_params.indices8 + uint64_t(cluster.triangle_offset) * 3;
  record.vertex_buffer = vertex_address;
  record.geometry_buffer = 0;
  record.omm_array = 0;
  record.omm_indices = 0;
  ClusterRecord* records = (ClusterRecord*)g_params.records;
  records[dense] = record;
}
