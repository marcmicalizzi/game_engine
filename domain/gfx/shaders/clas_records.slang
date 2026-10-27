// The GPU-driven half of the cluster acceleration structure path (docs/plan/04-renderer.md
// §4.4, ADR-0025), for a scene of instances: the frame's ray tracing geometry is exactly the
// frame's raster cut, with no CPU in between.
//
// A scene builds one cluster bottom-level structure per instance, so an instance's CLAS records
// have to be contiguous, while the cull pass's visible list is in whatever order its atomics
// produced. Three passes sort that out (gfx::ClusterRecordParams documents the buffers). The
// order they leave an instance's records in is still arrival order, so the structure a frame
// builds is not a function of the frame; nothing the picture shows depends on that, because the
// ray path settles a tied distance by the pair (ray_visibility.slang), not by traversal order.
//
//   records_main  one thread per visible entry, bucketing it into its instance's slice of
//                 `slots` — a drawn entry from the bottom of the slice up, a shadow caster from
//                 the top down — and counting it. The slices are as long as the instances'
//                 meshes times the view count, so the result is sparse.
//   ranges_main   one workgroup: what the set's capacity keeps (below), the prefix sum of it, the
//                 total record count the CLAS build reads from the device, and one 16-byte
//                 bottom-level record per instance over that instance's run of CLAS addresses.
//   emit_main     one thread per pair slot, moving the kept entries down into the dense
//                 `records` array — an instance's drawn clusters, then its casters — and writing
//                 the 64-byte CLAS build record there. A record's cluster id and base geometry
//                 index are the entry's **visible index**, which leads to the same {instance,
//                 cluster} the rasterizer drew, so the ray-traced picture writes the same pair
//                 (ray_visibility.slang).
//
// **The capacity is the frame's, not the scene's** (docs/subsystems/renderer.md, "The ray
// tracing chain's memory"). The set holds `capacity` structures, a step above what recent frames
// built; what a frame wants beyond it is dropped by `ranges_main` whole instances at a time,
// every instance's drawn clusters before any caster, so which instances lose their structures is
// a function of the per-instance counts and not of the order the atomics ran in.
//
// A deformed instance's record points at the frame's deformed-vertex pool instead of the mesh's
// rest-pose positions, so the ray geometry deforms with the raster geometry (§4.3). With the
// instantiate mode, emit_main writes 32-byte instantiate records naming each cluster's prebuilt
// template (MeshDesc::templates) and this frame's positions for it, which is what cluster
// acceleration structure templates exist for.
//
// With more than one view (§4.6) the three passes build the **union** of the views' cuts under
// one top-level structure. The visible list is laid out run-major — run r of view v starts at
// `(r * views + v) * pair_count` — so the *first* run of every view is one contiguous range at
// the front of the list, which is exactly the range the ray tracing chain consumes: one
// records_main dispatch covers `views * pair_count` entries, reads each view's own count word out
// of its indirect argument block, and the view count widens every instance's slice of `slots` so
// that a cluster drawn by every view has a slot in each. A record's base geometry index stays the
// entry's visible index, unique across the frame because the runs share one list.
//
// **Shadow casters** (docs/subsystems/geometry.md, "Normal cones"): with `caster_count` set the
// cull pass also left the pairs its cone test alone rejected in run `k_caster_run` of every view,
// and records_main covers that run too. A caster lands in its instance's slice from the top (in
// one view a pair is drawn or a caster, never both, so the two ends never meet) and is written
// **without the opaque flag**: the shadow rays force opacity and stop on it, the primary rays cull
// non-opaque geometry and pass through, so the traced picture stays the rasterized one and only
// the shadows gain the clusters that face away from the camera. A **deformed** caster (one outside
// the frustum: a deformed instance is never cone-tested) reads its own pool block like a drawn
// entry, and one the frame's pool budget had no room for is left out by records_main rather than
// built at its rest pose.

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

// Mirrors gfx::ClusterRecordParams (160 bytes: past the 128 every device guarantees, which this
// pass may take because it runs only where VK_NV_cluster_acceleration_structure does, and every
// such device has 256).
struct RecordParams {
  uint64_t clusters;         // ClusterDesc[]
  uint64_t vertices;         // float3[]: cluster-ordered rest positions
  uint64_t indices8;         // uint8[3 * triangles]: packed cluster indices
  uint64_t instances;        // InstanceDesc[instance_count]
  uint64_t meshes;           // MeshDesc[]: cluster counts, the deformed-vertex pool, templates
  uint64_t visible;          // uint2[]: the cull pass's visible list, from its first run
  uint64_t visible_count;    // uint: view 0's count word; view v's is k_draw_args_bytes further on
  uint64_t slots;            // uint[views * pair_count]: visible index per pair slot
  // uint[3 * instance_count]: drawn and caster counts per instance (zeroed first), then the dense
  // record bases
  uint64_t instance_counts;
  uint64_t caster_count;     // uint: view 0's caster count word, or 0 for no caster run
  uint64_t records;          // ClusterRecord[] or InstantiateRecord[] out, `capacity` long
  uint64_t record_count;     // uint[4] out: built, wanted, instances dropped, casters dropped
  uint64_t blas_records;     // BlasRecord[instance_count] out
  uint64_t clas_addresses;   // uint64_t[]: where the CLAS build writes each address
  uint instance_count;
  uint pair_count;  // the scene's; one run's count cannot exceed it
  uint mode;        // low 16 bits: views whose runs accumulate here; bit 16: instantiate
  uint capacity;    // the structures the set holds; ranges_main keeps no more
  // Alpha-tested materials (renderer.md, "Alpha-tested geometry"): the scene's material table and
  // material per cluster, or 0 for a frame with none. A masked pair is built non-opaque.
  uint64_t mask_materials;
  uint64_t mask_cluster_materials;
  // **The drop order** (renderer.md, "The ray tracing chain's memory"): uint count, uint pad, then
  // `count` {instance, rank} pairs, nearest rank first — the prefix, then a streamed world's rings
  // inside out, each by tile key and slot — or 0 for instance order in one rank.
  uint64_t drop_order;
  uint64_t hit_entries;
  // **The resident cluster structure cache** (renderer.md, "The resident cluster structure cache";
  // gfx::ClusterCacheTables): its tables, or 0 for none; and the frame's stamp.
  uint64_t cache;
  uint frame;
  uint cache_pad;
};

// Mirrors gfx::ClusterCacheTables (112 bytes).
struct CacheTables {
  uint64_t bases;              // uint per mesh: its first cluster's key / 3, or ~0
  uint64_t keys;               // uint per key: its slot, or a mark
  uint64_t owners;             // uint per slot: its key, or k_cache_empty
  uint64_t stamps;             // uint per slot: the last frame that drew it
  uint64_t free_list;          // uint per slot
  uint64_t misses;             // uint4 per slot: key, cluster, geometry word, 0
  uint64_t miss_records;       // ClusterRecord per slot
  uint64_t miss_destinations;  // uint64_t per slot
  uint64_t references;         // uint64_t per cached reference
  uint64_t reference_keys;     // uint per cached reference
  uint64_t scratch;            // uint[4 + k_cache_ages]: free count, threshold, quota, taken, ages
  uint64_t slot_base;
  uint slot_stride;
  uint slot_count;
  uint mesh_count;
  uint reference_capacity;
};

[[vk::push_constant]] ConstantBuffer<RecordParams> g_params;

static const uint k_cache_empty = 0xffffffffu;     // gfx::k_cluster_cache_empty
static const uint k_cache_claim = 0xfffffffeu;     // gfx::k_cluster_cache_claim
static const uint k_cache_overflow = 0xfffffffdu;  // gfx::k_cluster_cache_overflow
static const uint k_cache_forms = 3;               // gfx::k_cluster_cache_forms
static const uint k_cache_geometry = 1u << 23;     // gfx::k_cluster_cache_geometry
static const uint k_cache_caster = 1u << 22;       // gfx::k_cluster_cache_caster
static const uint k_cache_ages = 64;               // gfx::k_cluster_cache_ages
// record_count's words (gfx::k_cluster_record_count_words)
static const uint k_count_refs_kept = 4;
static const uint k_count_refs_wanted = 5;
static const uint k_count_misses = 6;
static const uint k_count_miss_built = 7;
static const uint k_count_miss_overflow = 8;
static const uint k_count_evicted = 9;
static const uint k_count_occupied = 10;

CacheTables* cache_tables() { return (CacheTables*)g_params.cache; }

// **A cached instance** (renderer.md, "The resident cluster structure cache"): one whose clusters'
// structures are the cache's, built once, and not this frame's. Rigid — nothing in the pool moves
// its vertices, so every instance of its mesh traces the same clusters — and of a mesh the cache
// keeps. A static form and an untraced instance never reach a records pass that asks.
bool instance_cached(InstanceDesc instance) {
  if (g_params.cache == 0 || instance.deform != k_invalid_deform) return false;
  CacheTables* tables = cache_tables();
  if (instance.mesh >= tables->mesh_count) return false;
  uint* bases = (uint*)tables->bases;
  return bases[instance.mesh] != k_cache_empty;
}

uint reference_capacity() { return g_params.cache != 0 ? cache_tables()->reference_capacity : 0u; }

// Whether instance `i` is cached, reading the instance table only when there is a cache: a caller
// of the ranges pass alone (the records tests) hands it no instances.
bool cached_at(uint i) {
  if (g_params.cache == 0) return false;
  InstanceDesc* instances = (InstanceDesc*)g_params.instances;
  return instance_cached(instances[i]);
}

static const uint k_index_type_8bit = 1;
static const uint k_geometry_opaque = 4;
static const uint k_draw_args_bytes = 16;  // gfx::k_draw_args_bytes: one block per view
static const uint k_caster_run = 2;        // gfx::k_caster_run: the casters' run of the list
static const uint k_instantiate = 0x10000;  // gfx::k_cluster_records_instantiate

uint view_count() { return g_params.mode & 0xffff; }
bool instantiating() { return (g_params.mode & k_instantiate) != 0; }

// The first visible index of the caster run: nothing below it is a caster.
uint caster_base() { return k_caster_run * view_count() * g_params.pair_count; }

// The three thirds of `instance_counts`: each instance's drawn entries, its casters, and its dense
// record base.
uint* drawn_counts() { return (uint*)g_params.instance_counts; }
uint* caster_counts() {
  return (uint*)(g_params.instance_counts + uint64_t(g_params.instance_count) * 4);
}
uint* instance_first() {
  return (uint*)(g_params.instance_counts + uint64_t(g_params.instance_count) * 8);
}

// An instance's slice of `slots`: `views` slots per cluster of its mesh, starting at its first
// pair times the view count.
uint slice_base(InstanceDesc instance) { return instance.first_pair * view_count(); }
uint slice_length(InstanceDesc instance) {
  MeshDesc* meshes = (MeshDesc*)g_params.meshes;
  return meshes[instance.mesh].cluster_count * view_count();
}

// Bucket each visible entry into its instance's slice of the pair-indexed scratch array. The
// dispatch covers every view's first run at once, because those runs are contiguous, and then —
// when there is a caster run — every view's caster run, which is contiguous too.
[shader("compute")]
[numthreads(64, 1, 1)]
void records_main(uint3 thread_id : SV_DispatchThreadID) {
  const uint run_entries = view_count() * g_params.pair_count;
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
  const InstanceDesc instance = instances[instance_index];
  // **A rigid form's structure is built once** (renderer.md, "Static structures for rigid forms"):
  // its cut is fixed, so the frame builds nothing for it and the top-level record takes its mesh's
  // static structure (tlas_references.slang). Left out before it is counted, so it is neither
  // built, wanted nor dropped.
  if ((instance.flags & k_instance_static_rt) != 0) return;
  // **The caster policy's `traced = false`** (renderer.md, "The caster policy"; R95): grass is never
  // in the frame's structures — no record, no slot, no count — so it costs the chain nothing.
  if ((instance.flags & k_instance_untraced) != 0) return;
  uint* slots = (uint*)g_params.slots;
  if (caster) {
    // **A deformed caster the pool had no room for is not built** (renderer.md, "Shadows"). A
    // drawn entry the budget refuses draws its rest pose, which beats a hole in the picture; a
    // caster's rest pose would cast a shadow where its pose has none — a terrain level's rest
    // heights can be metres from the field it draws — and a wrong shadow is worse than a missing
    // one. It is left out here, before it is counted, so the ranges and the emit never see it.
    MeshDesc* meshes = (MeshDesc*)g_params.meshes;
    const MeshDesc mesh = meshes[instance.mesh];
    if (is_deformed(mesh, instance) && deform_base(mesh, instance, index) == k_no_pool_slot) {
      return;
    }
    Atomic<uint>* counts = (Atomic<uint>*)caster_counts();
    const uint local = counts[instance_index].add(1);
    slots[slice_base(instance) + slice_length(instance) - 1 - local] = index;
  } else {
    Atomic<uint>* counts = (Atomic<uint>*)drawn_counts();
    const uint local = counts[instance_index].add(1);
    slots[slice_base(instance) + local] = index;
  }
}

// What the capacity keeps, the prefix sum over the instances, the total, and one bottom-level
// record per instance. **One workgroup**: each thread sums a run of the instances and the group
// scans the runs, which is the whole pass whenever everything the frame wants fits — every
// instance kept, the records the prefix sum. It was one thread over the instances ("at 90
// instances it is nothing"), and the world's tiles made it 40,000 instances, every tile slot one:
// the RT chain took 7.50 ms a frame at the median over desert-endless's first minute at 1080p on
// the RTX 5090 with one thread, and 0.67 ms over its whole flight with the group (gfx.md). Only a frame that wants more than the set holds takes the rule below, on
// one thread, because it is the one place that sees every instance's count in order, which is
// what "whole instances, in order" needs; it writes what it always wrote.
static const uint k_ranges_threads = 256;  // numthreads below; the pass dispatches one group
groupshared uint g_ranges_sum[k_ranges_threads];
// The cached instances' references (renderer.md, "The resident cluster structure cache"): a second
// prefix sum beside the built records', because the two are two arrays. A cached instance's
// bottom-level record points into the cache's `references`, any other's into the frame's own CLAS
// addresses, and each counts against its own capacity.
groupshared uint g_cached_sum[k_ranges_threads];

// Takes `n` from the left of the kind the instance is: what the capacity rule spends.
bool take(inout uint left_built, inout uint left_cached, bool cached, uint n) {
  if (cached) {
    if (n > left_cached) return false;
    left_cached -= n;
    return true;
  }
  if (n > left_built) return false;
  left_built -= n;
  return true;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void ranges_main(uint3 group_thread : SV_GroupThreadID) {
  uint* drawn = drawn_counts();
  uint* casters = caster_counts();
  uint* first = instance_first();
  BlasRecord* blas = (BlasRecord*)g_params.blas_records;
  uint* record_count = (uint*)g_params.record_count;
  const uint n = g_params.instance_count;
  const uint t = group_thread.x;
  // The cache's words and counters start every frame at zero; the passes after this one count
  // into them (this pass and they are ordered by the graph).
  if (t < 6) record_count[k_count_misses + t] = 0;
  if (g_params.cache != 0 && t < 4 + k_cache_ages) ((uint*)cache_tables()->scratch)[t] = 0;
  uint64_t cached_references = g_params.cache != 0 ? cache_tables()->references : 0;
  const uint per = (n + k_ranges_threads - 1) / k_ranges_threads;
  const uint begin = min(t * per, n);
  const uint end = min(begin + per, n);
  uint own = 0;
  uint own_cached = 0;
  for (uint i = begin; i < end; ++i) {
    if (cached_at(i)) {
      own_cached += drawn[i] + casters[i];
    } else {
      own += drawn[i] + casters[i];
    }
  }
  g_ranges_sum[t] = own;
  g_cached_sum[t] = own_cached;
  GroupMemoryBarrierWithGroupSync();
  for (uint step = 1; step < k_ranges_threads; step <<= 1) {
    const uint add = t >= step ? g_ranges_sum[t - step] : 0u;
    const uint add_cached = t >= step ? g_cached_sum[t - step] : 0u;
    GroupMemoryBarrierWithGroupSync();
    g_ranges_sum[t] += add;
    g_cached_sum[t] += add_cached;
    GroupMemoryBarrierWithGroupSync();
  }
  const uint wanted = g_ranges_sum[k_ranges_threads - 1];
  const uint cached_wanted = g_cached_sum[k_ranges_threads - 1];
  if (wanted <= g_params.capacity && cached_wanted <= reference_capacity()) {
    // Everything fits: nothing is dropped, and each run writes its instances from its prefix.
    uint total = g_ranges_sum[t] - own;
    uint cached_total = g_cached_sum[t] - own_cached;
    for (uint i = begin; i < end; ++i) {
      BlasRecord record;
      record.count = drawn[i] + casters[i];
      record.stride = 8;
      if (cached_at(i)) {
        first[i] = cached_total;
        record.references = cached_references + uint64_t(cached_total) * 8;
        cached_total += record.count;
      } else {
        first[i] = total;
        record.references = g_params.clas_addresses + uint64_t(total) * 8;
        total += record.count;
      }
      blas[i] = record;
    }
    if (t == 0) {
      record_count[0] = wanted;
      record_count[1] = wanted;
      record_count[2] = 0;
      record_count[3] = 0;
      record_count[k_count_refs_kept] = cached_wanted;
      record_count[k_count_refs_wanted] = cached_wanted;
    }
    return;
  }
  if (t != 0) return;
  // **Nearest first, and within one distance drawn clusters before casters** (renderer.md, "The
  // ray tracing chain's memory"). The order is a rank at a time — the prefix, then a streamed
  // world's rings inside out, each by tile key and slot, which the CPU lays out with the tables —
  // and within a rank the drawn clusters instance by instance, then the casters: an instance that
  // does not fit keeps nothing — its casters go with its drawn clusters, since they would cast a
  // part of its shadow and not the rest — and the next one is tried, so a large instance cannot
  // starve every small one behind it. So what a frame past its capacity loses is the farthest
  // ring's casters, then its instances, then the next ring's, never a near tree before a far one,
  // and the same instances whatever order the atomics ran in. Without an order (a scene read
  // whole) the instances are one rank in instance order, as before. A cached instance spends the
  // references' capacity and any other the frame's own (renderer.md, "The resident cluster
  // structure cache"), by the same rule.
  uint left = g_params.capacity;
  uint left_cached = reference_capacity();
  uint dropped_drawn = 0;
  uint dropped_casters = 0;
  const uint k_visited = 0x80000000u;
  uint* order = (uint*)g_params.drop_order;
  const uint order_count = g_params.drop_order != 0 ? order[0] : g_params.instance_count;
  uint begin_rank = 0;
  while (begin_rank < order_count) {
    const uint rank = g_params.drop_order != 0 ? order[2 + 2 * begin_rank + 1] : 0u;
    uint end_rank = begin_rank;
    while (end_rank < order_count &&
           (g_params.drop_order == 0 || order[2 + 2 * end_rank + 1] == rank)) {
      const uint i = g_params.drop_order != 0 ? order[2 + 2 * end_rank] : end_rank;
      if (i < n) {
        if (!take(left, left_cached, cached_at(i), drawn[i])) {
          drawn[i] = 0;
          casters[i] = 0;
          ++dropped_drawn;
        }
        drawn[i] |= k_visited;
      }
      ++end_rank;
    }
    for (uint k = begin_rank; k < end_rank; ++k) {
      const uint i = g_params.drop_order != 0 ? order[2 + 2 * k] : k;
      if (i >= n) continue;
      if (!take(left, left_cached, cached_at(i), casters[i])) {
        casters[i] = 0;
        ++dropped_casters;
      }
    }
    begin_rank = end_rank;
  }
  // An instance the order did not name builds nothing (and is counted), so the build never holds
  // more than the capacity whatever the order says.
  for (uint i = 0; i < n; ++i) {
    if ((drawn[i] & k_visited) != 0) {
      drawn[i] &= ~k_visited;
    } else if (drawn[i] + casters[i] != 0) {
      drawn[i] = 0;
      casters[i] = 0;
      ++dropped_drawn;
    }
  }
  uint total = 0;
  uint cached_total = 0;
  for (uint i = 0; i < g_params.instance_count; ++i) {
    BlasRecord record;
    record.count = drawn[i] + casters[i];
    record.stride = 8;
    if (cached_at(i)) {
      first[i] = cached_total;
      record.references = cached_references + uint64_t(cached_total) * 8;
      cached_total += record.count;
    } else {
      first[i] = total;
      record.references = g_params.clas_addresses + uint64_t(total) * 8;
      total += record.count;
    }
    blas[i] = record;
  }
  record_count[0] = total;  // what the build reads: never more than the capacity
  record_count[1] = wanted;
  record_count[2] = dropped_drawn;
  record_count[3] = dropped_casters;
  record_count[k_count_refs_kept] = cached_total;
  record_count[k_count_refs_wanted] = cached_wanted;
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

// **A cached instance's entry** (renderer.md, "The resident cluster structure cache"): no record of
// its own, but a reference — slot `dense` of the cache's `references`, whose address
// `cache_references_main` writes once every key has its slot — and a look-up of its key. A key
// whose structure is resident has its slot stamped with this frame, which is what keeps the
// eviction off it; one that is not is claimed by exactly one thread (the compare-exchange) and
// listed as a miss, which `cache_assign_main` gives a slot and a build record.
void emit_cached(uint dense, uint cluster_id, MeshDesc mesh, InstanceDesc instance,
                 uint visible_index) {
  CacheTables* tables = cache_tables();
  MaskTables masks = no_mask_tables();
  masks.materials = g_params.mask_materials;
  masks.cluster_materials = g_params.mask_cluster_materials;
  const bool caster = g_params.caster_count != 0 && visible_index >= caster_base();
  const bool masked = pair_masked(masks, instance, cluster_id);
  // The three forms a cluster's structure can take differ in their flags and their name, and the
  // structure carries both: the drawn opaque one, the drawn alpha-tested one (non-opaque, so the
  // ray loops test its mask), and the caster (non-opaque, and named as one).
  const uint form = caster ? 2u : (masked ? 1u : 0u);
  const uint local = cluster_id - mesh.first_cluster;
  uint* bases = (uint*)tables->bases;
  const uint key = (bases[instance.mesh] + local) * k_cache_forms + form;
  uint* reference_keys = (uint*)tables->reference_keys;
  reference_keys[dense] = key;
  Atomic<uint>* keys = (Atomic<uint>*)tables->keys;
  const uint value = keys[key].load();
  if (value < k_cache_overflow) {
    uint* stamps = (uint*)tables->stamps;
    stamps[value] = g_params.frame;
    return;
  }
  if (value == k_cache_claim) return;  // another entry of this frame claimed it
  if (keys[key].compareExchange(value, k_cache_claim) != value) return;
  Atomic<uint>* counts = (Atomic<uint>*)g_params.record_count;
  const uint index = counts[k_count_misses].add(1);
  if (index >= tables->slot_count) {
    // More misses than the cache has slots: no room whatever is evicted.
    keys[key].store(k_cache_overflow);
    counts[k_count_miss_overflow].add(1);
    return;
  }
  const bool opaque = !caster && !masked;
  const uint geometry = local | k_cache_geometry | (caster ? k_cache_caster : 0u);
  uint4* misses = (uint4*)tables->misses;
  misses[index] = uint4(key, cluster_id, geometry | ((opaque ? k_geometry_opaque : 0u) << 29), 0u);
}

// Move the kept entries down to the dense array and write their CLAS build records, or, in the
// templates variant, the instantiate records that name the cluster's template and this frame's
// positions for it.
[shader("compute")]
[numthreads(64, 1, 1)]
void emit_main(uint3 thread_id : SV_DispatchThreadID) {
  const uint slot = thread_id.x;
  const uint views = view_count();
  if (slot >= views * g_params.pair_count) return;
  InstanceDesc* instances = (InstanceDesc*)g_params.instances;
  // An instance's slice is `views` times its mesh's cluster count, so dividing the slot by the
  // view count lands back in the pair space the prefix sum `first_pair` is in.
  const uint instance_index =
      instance_of_pair(instances, g_params.instance_count, slot / views);
  const InstanceDesc instance = instances[instance_index];
  const uint local = slot - slice_base(instance);
  // Drawn entries fill the slice from the bottom, casters from the top; what ranges_main left in
  // the two counts is what was kept (all of an instance's, or none).
  const uint drawn = drawn_counts()[instance_index];
  const uint from_top = slice_length(instance) - 1 - local;
  uint dense_local = 0;
  if (local < drawn) {
    dense_local = local;
  } else if (from_top < caster_counts()[instance_index]) {
    dense_local = drawn + from_top;
  } else {
    return;  // this instance drew fewer clusters than it has, or lost them to the capacity
  }
  uint* slots = (uint*)g_params.slots;
  const uint visible_index = slots[slot];
  uint2* visible = (uint2*)g_params.visible;
  const uint cluster_id = visible[visible_index].y;
  ClusterDesc* clusters = (ClusterDesc*)g_params.clusters;
  const ClusterDesc cluster = clusters[cluster_id];
  MeshDesc* meshes = (MeshDesc*)g_params.meshes;
  const MeshDesc mesh = meshes[instance.mesh];
  const uint dense = instance_first()[instance_index] + dense_local;
  if (instance_cached(instance)) {
    emit_cached(dense, cluster_id, mesh, instance, visible_index);
    return;
  }
  const uint geometry_index = g_params.hit_entries != 0 ? dense : visible_index;
  if (g_params.hit_entries != 0) {
    uint* entries = (uint*)g_params.hit_entries;
    entries[dense] = visible_index;
  }
  const uint64_t vertex_address = cluster_vertex_address(mesh, instance, cluster, visible_index);

  if (instantiating()) {
    uint64_t* templates = (uint64_t*)mesh.templates;
    InstantiateRecord record;
    record.cluster_id_offset = geometry_index;
    record.geometry_index_offset = geometry_index & 0xffffff;
    record.template_address = templates[cluster_id];
    record.vertex_address = vertex_address;
    record.vertex_stride = 12;
    InstantiateRecord* records = (InstantiateRecord*)g_params.records;
    records[dense] = record;
    return;
  }

  ClusterRecord record;
  record.cluster_id = geometry_index;
  record.cluster_flags = 0;
  record.counts = (cluster.triangle_count & 0x1ff) | ((cluster.vertex_count & 0x1ff) << 9) |
                  (k_index_type_8bit << 24);
  // Everything the picture draws is opaque; a shadow caster is not, which is the whole of how the
  // primary rays leave it out (RAY_FLAG_CULL_NON_OPAQUE) while the shadow rays keep it. Nor is an
  // alpha-tested pair, drawn or caster, whose hits the ray loops test against its mask
  // (alpha_mask.slang, `candidate_keeps`).
  MaskTables masks = no_mask_tables();
  masks.materials = g_params.mask_materials;
  masks.cluster_materials = g_params.mask_cluster_materials;
  const bool opaque = (g_params.caster_count == 0 || visible_index < caster_base()) &&
                      !pair_masked(masks, instance, cluster_id);
  record.geometry = (geometry_index & 0xffffff) | ((opaque ? k_geometry_opaque : 0u) << 29);
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

// ---- the resident cluster structure cache (renderer.md, "The resident cluster structure cache") --
//
// After the emit pass every cached entry has looked its key up: a resident key's slot carries this
// frame's stamp, and a missing one is claimed and listed in `misses`. Then, a pass each, over the
// slots or the references:

// The free slots, listed, and the occupied ones this frame did not draw counted by how many frames
// ago they were drawn (the age histogram, the last bucket everything older). Slot 0 is the
// inactive structure a reference without a slot points at, and is never free or evicted.
[shader("compute")]
[numthreads(64, 1, 1)]
void cache_scan_main(uint3 thread_id : SV_DispatchThreadID) {
  CacheTables* tables = cache_tables();
  const uint slot = thread_id.x;
  if (slot == 0 || slot >= tables->slot_count) return;
  uint* owners = (uint*)tables->owners;
  Atomic<uint>* scratch = (Atomic<uint>*)tables->scratch;
  if (owners[slot] == k_cache_empty) {
    uint* free_list = (uint*)tables->free_list;
    free_list[scratch[0].add(1)] = slot;
    return;
  }
  uint* stamps = (uint*)tables->stamps;
  const uint age = g_params.frame - stamps[slot];
  if (age == 0) return;  // drawn this frame: never evicted
  scratch[4 + min(age, k_cache_ages - 1)].add(1);
}

// One thread: how many slots the misses need beyond the free ones, and the oldest ages that hold
// them — every slot older than `threshold`, and `quota` of those exactly that old. **Least recently
// drawn first**, to the frame within the histogram's 63, everything older as one.
[shader("compute")]
[numthreads(1, 1, 1)]
void cache_threshold_main(uint3 thread_id : SV_DispatchThreadID) {
  CacheTables* tables = cache_tables();
  uint* scratch = (uint*)tables->scratch;
  uint* record_count = (uint*)g_params.record_count;
  const uint misses = min(record_count[k_count_misses], tables->slot_count);
  const uint free = scratch[0];
  const uint need = misses > free ? misses - free : 0u;
  uint threshold = k_cache_ages;  // none
  uint quota = 0;
  if (need > 0) {
    uint held = 0;
    threshold = 1;
    quota = scratch[4 + 1];  // every slot not drawn this frame, if that is what it takes
    for (int age = int(k_cache_ages) - 1; age >= 1; --age) {
      const uint count = scratch[4 + age];
      if (held + count >= need) {
        threshold = uint(age);
        quota = need - held;
        break;
      }
      held += count;
    }
  }
  scratch[1] = threshold;
  scratch[2] = quota;
  scratch[3] = 0;
}

// Evicts what the threshold chose: the key loses its slot (its next draw misses and builds it
// again) and the slot joins the free list behind the ones the scan found.
[shader("compute")]
[numthreads(64, 1, 1)]
void cache_evict_main(uint3 thread_id : SV_DispatchThreadID) {
  CacheTables* tables = cache_tables();
  const uint slot = thread_id.x;
  if (slot == 0 || slot >= tables->slot_count) return;
  uint* owners = (uint*)tables->owners;
  const uint owner = owners[slot];
  if (owner == k_cache_empty) return;
  uint* stamps = (uint*)tables->stamps;
  const uint age = g_params.frame - stamps[slot];
  if (age == 0) return;
  Atomic<uint>* scratch = (Atomic<uint>*)tables->scratch;
  uint* words = (uint*)tables->scratch;
  const uint bucket = min(age, k_cache_ages - 1);
  if (bucket < words[1]) return;
  if (bucket == words[1] && scratch[3].add(1) >= words[2]) return;
  Atomic<uint>* keys = (Atomic<uint>*)tables->keys;
  keys[owner].store(k_cache_empty);
  owners[slot] = k_cache_empty;
  uint* free_list = (uint*)tables->free_list;
  free_list[scratch[0].add(1)] = slot;
  Atomic<uint>* counts = (Atomic<uint>*)g_params.record_count;
  counts[k_count_evicted].add(1);
}

// Every miss its slot, in miss order, and its build record and destination; a miss past the free
// slots — only when this frame's own new clusters outnumber every slot it did not draw — is marked
// so its references take the inactive structure, and counted. Thread 0 writes the count the
// explicit build reads and how many slots are occupied after it.
[shader("compute")]
[numthreads(64, 1, 1)]
void cache_assign_main(uint3 thread_id : SV_DispatchThreadID) {
  CacheTables* tables = cache_tables();
  uint* record_count = (uint*)g_params.record_count;
  uint* scratch = (uint*)tables->scratch;
  const uint misses = min(record_count[k_count_misses], tables->slot_count);
  const uint free = scratch[0];
  const uint assigned = min(misses, free);
  const uint i = thread_id.x;
  if (i == 0) {
    record_count[k_count_miss_built] = assigned;
    record_count[k_count_occupied] = tables->slot_count - 1 - free + assigned;
  }
  if (i >= misses) return;
  uint4* list = (uint4*)tables->misses;
  const uint4 miss = list[i];
  Atomic<uint>* keys = (Atomic<uint>*)tables->keys;
  if (i >= free) {
    keys[miss.x].store(k_cache_overflow);
    Atomic<uint>* counts = (Atomic<uint>*)g_params.record_count;
    counts[k_count_miss_overflow].add(1);
    return;
  }
  uint* free_list = (uint*)tables->free_list;
  const uint slot = free_list[i];
  uint* owners = (uint*)tables->owners;
  uint* stamps = (uint*)tables->stamps;
  owners[slot] = miss.x;
  stamps[slot] = g_params.frame;
  keys[miss.x].store(slot);
  // The record as the emit pass writes a rigid entry's, from the rest-pose float positions, but
  // named by the cluster within its mesh (`miss.z`) rather than by a visible entry.
  ClusterDesc* clusters = (ClusterDesc*)g_params.clusters;
  const ClusterDesc cluster = clusters[miss.y];
  ClusterRecord record;
  record.cluster_id = miss.z & 0xffffff;
  record.cluster_flags = 0;
  record.counts = (cluster.triangle_count & 0x1ff) | ((cluster.vertex_count & 0x1ff) << 9) |
                  (k_index_type_8bit << 24);
  record.geometry = miss.z;
  record.strides0 = 1 | (12 << 16);
  record.strides1 = 0;
  record.index_buffer = g_params.indices8 + uint64_t(cluster.triangle_offset) * 3;
  record.vertex_buffer = g_params.vertices + uint64_t(cluster.vertex_offset) * 12;
  record.geometry_buffer = 0;
  record.omm_array = 0;
  record.omm_indices = 0;
  ClusterRecord* records = (ClusterRecord*)tables->miss_records;
  records[i] = record;
  uint64_t* destinations = (uint64_t*)tables->miss_destinations;
  destinations[i] = tables->slot_base + uint64_t(slot) * tables->slot_stride;
}

// Every cached reference's address: its key's slot, or the inactive structure for a key the cache
// had no room for this frame.
[shader("compute")]
[numthreads(64, 1, 1)]
void cache_references_main(uint3 thread_id : SV_DispatchThreadID) {
  CacheTables* tables = cache_tables();
  uint* record_count = (uint*)g_params.record_count;
  const uint d = thread_id.x;
  if (d >= record_count[k_count_refs_kept]) return;
  uint* reference_keys = (uint*)tables->reference_keys;
  uint* keys = (uint*)tables->keys;
  const uint slot = keys[reference_keys[d]];
  uint64_t* references = (uint64_t*)tables->references;
  references[d] =
      tables->slot_base + (slot < k_cache_overflow ? uint64_t(slot) * tables->slot_stride : 0u);
}