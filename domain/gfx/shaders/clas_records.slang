// The GPU-driven half of the cluster acceleration structure path (docs/plan/04-renderer.md
// §4.4, ADR-0025), for a scene of instances: the frame's ray tracing geometry is exactly the
// frame's raster cut, with no CPU in between.
//
// A scene builds one cluster bottom-level structure per instance, so an instance's CLAS records
// have to be contiguous, while the cull pass's visible list is in whatever order its atomics
// produced. Four passes sort that out (gfx::ClusterRecordParams documents the buffers). The
// order they leave an instance's records in is still arrival order, so the structure a frame
// builds is not a function of the frame; nothing the picture shows depends on that, because the
// ray path settles a tied distance by the pair (ray_visibility.slang), not by traversal order.
//
//   records_main  one thread per visible entry, bucketing it into its instance's slice of
//                 `slots` — a drawn entry from the bottom of the slice up, a shadow caster from
//                 the top down — and counting it. The slices are as long as the instances'
//                 meshes times the view count, so the result is sparse.
//   ranges_count_main, then ranges_main   a device-wide scan over the instances, a workgroup a
//                 block of 1,024: each block's sums, then what the set's capacity keeps (below),
//                 the prefix sum of it, the total record count the CLAS build reads from the
//                 device, and one 16-byte bottom-level record per instance over that instance's run
//                 of CLAS addresses — and past the capacity three more passes, dispatched only
//                 then: the walk (ranges_drop_main) and the kept instances' scan
//                 (ranges_kept_count_main, ranges_kept_main), under "past the capacity" below.
//   emit_main     one thread per pair slot, moving the kept entries down into the dense
//                 `records` array — an instance's drawn clusters, then its casters — and writing
//                 the 64-byte CLAS build record there. A record's cluster id and base geometry
//                 index are the entry's **visible index**, which leads to the same {instance,
//                 cluster} the rasterizer drew, so the ray-traced picture writes the same pair
//                 (ray_visibility.slang).
//
// All but the first come in two variants (R106): the plain entry points for a frame with no cache,
// which never read an instance record to ask whether it is cached, and their `_cached_` twins for
// a frame with one (the resident cluster structure cache, at the end of this file).
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

// Mirrors gfx::ClusterCacheTables (120 bytes).
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
  uint64_t reference_keys;     // uint per cached reference: the pending ones' dense indices
  // uint[4 + k_cache_ages + 1]: free count, threshold, quota, taken, ages, then the slots occupied
  // (kept from frame to frame)
  uint64_t scratch;
  uint64_t slot_base;
  uint64_t args;  // uint[k_cache_args_words]: the cache passes' dispatches, then the pending count
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
static const uint k_cache_workgroup = 64;          // gfx::k_cluster_cache_workgroup
// The scratch's words: what `ranges_count_main` clears every frame, and the occupied count after
// them, which only a frame with misses changes (gfx::k_cluster_cache_scratch_occupied).
static const uint k_scratch_cleared = 4 + k_cache_ages;
static const uint k_scratch_occupied = k_scratch_cleared;
// The args block (gfx::k_cluster_cache_args_*): four dispatches of three words — the slot scan, the
// eviction, the misses' slots, the pending references — then the pending references' count.
static const uint k_args_scan = 0;
static const uint k_args_evict = 3;
static const uint k_args_assign = 6;
static const uint k_args_references = 9;
static const uint k_args_pending = 12;
static const uint k_cache_args_words = 13;
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
static const uint k_drop_scan = 0x20000;    // gfx::k_cluster_records_drop_scan

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
// record per instance: **a device-wide scan in two dispatches** (R106). `ranges_count_main` sums
// each block of `k_ranges_block` instances, a workgroup a block, into the block sums past
// `instance_counts`' three thirds; `ranges_main`, the same groups, has every group sum the block
// sums itself — the totals, and its own block's offset as the sum of the blocks before it — so the
// scan needs no third dispatch and no group waits on another (no look-back), and then writes its
// block's instances from that offset. That is the whole of it whenever everything the frame wants
// fits: every instance kept, the records the prefix sum.
//
// History: one thread over the instances ("at 90 instances it is nothing") until the world's tiles
// made it 40,000, every tile slot one — the RT chain took 7.50 ms a frame at the median over
// desert-endless's first minute at 1080p on the RTX 5090 with one thread, and 0.67 ms over its
// whole flight with one workgroup (gfx.md); then one workgroup of 256 threads, each walking a
// contiguous run of `n / 256` instances twice, which R101's second prefix sum and the endless
// desert's tens of thousands of ring slots made a measurable share of its `rt` pass (renderer.md,
// "The resident cluster structure cache", R106).
//
// A frame that wants more than the set holds takes the rule further below ("past the capacity"),
// which was one thread over every instance until R106 and is now a scan of its own; it writes what
// it always wrote.
//
// **Two variants of each pass** (the generic `kCache`): `ranges_count_main`/`ranges_main` for a
// frame with no cache, which never read an instance record, keep one sum and never mark a count —
// the chain as it cost before R101 — and `ranges_count_cached_main`/`ranges_cached_main` for a
// frame whose scene has cached instances. The renderer picks the pair by the frame's
// `ClusterRecordParams::cache`; a branch per instance on it is what R101 had.
static const uint k_ranges_threads = 256;  // numthreads of both passes
static const uint k_ranges_each = 4;       // contiguous instances per thread
static const uint k_ranges_block = k_ranges_threads * k_ranges_each;  // gfx::k_cluster_ranges_block
groupshared uint4 g_ranges_sum[k_ranges_threads];
groupshared uint2 g_ranges_scan[k_ranges_threads];
// While the ranges run, bit 30 of an instance's two counts says it is cached: read from its 96-byte
// record once, by `ranges_count_cached_main`, and carried in words the rule below loads anyway — a
// load of the record, or of one more word, per instance on that rule's one thread made the
// forest's over-capacity frames' chain slower by 4 to 29 ms. Cleared before `ranges_main` ends.
static const uint k_count_cached = 0x40000000u;
static const uint k_count_mask = 0x3fffffffu;

uint ranges_blocks() { return (g_params.instance_count + k_ranges_block - 1) / k_ranges_block; }
// Two words a block, past the three thirds of `instance_counts`: what its instances want built, and
// what they want referenced from the cache.
uint* block_sums() {
  return (uint*)(g_params.instance_counts + uint64_t(g_params.instance_count) * 12);
}

// The group's sum of four words, every thread's.
uint4 group_sum(uint t, uint4 value) {
  GroupMemoryBarrierWithGroupSync();  // whoever read the array last is done with it
  g_ranges_sum[t] = value;
  GroupMemoryBarrierWithGroupSync();
  for (uint step = k_ranges_threads / 2; step > 0; step >>= 1) {
    if (t < step) g_ranges_sum[t] += g_ranges_sum[t + step];
    GroupMemoryBarrierWithGroupSync();
  }
  return g_ranges_sum[0];
}

// The group's exclusive prefix sum of two words, in thread order.
uint2 group_exclusive(uint t, uint2 value) {
  GroupMemoryBarrierWithGroupSync();
  g_ranges_scan[t] = value;
  GroupMemoryBarrierWithGroupSync();
  for (uint step = 1; step < k_ranges_threads; step <<= 1) {
    const uint2 add = t >= step ? g_ranges_scan[t - step] : uint2(0u, 0u);
    GroupMemoryBarrierWithGroupSync();
    g_ranges_scan[t] += add;
    GroupMemoryBarrierWithGroupSync();
  }
  return g_ranges_scan[t] - value;
}

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

// The first dispatch: each block's sums, and — group 0 — the per-frame words the passes after it
// count into, cleared (they and this pass are ordered by the graph).
void ranges_count<let kCache : int>(uint group, uint t) {
  uint* drawn = drawn_counts();
  uint* casters = caster_counts();
  uint* record_count = (uint*)g_params.record_count;
  const uint n = g_params.instance_count;
  if (group == 0) {
    if (kCache != 0) {
      CacheTables* tables = cache_tables();
      uint* scratch = (uint*)tables->scratch;
      // Word 10 is the slots occupied after the frame: the count the last frame with misses left,
      // unless this one has misses too (`cache_assign_main` writes it again then).
      if (t < 6) record_count[k_count_misses + t] = t == 4 ? scratch[k_scratch_occupied] : 0u;
      if (t < k_scratch_cleared) scratch[t] = 0;
      // Every dispatch of the cache's passes empty, `x` 0 and `y`, `z` 1, until a miss or a pending
      // reference of this frame's emit pass grows it.
      uint* args = (uint*)tables->args;
      if (t < k_args_pending) args[t] = t % 3 == 0 ? 0u : 1u;
      if (t == k_args_pending) args[t] = 0;
    } else if (t < 6) {
      record_count[k_count_misses + t] = 0;
    }
    // The drop words: no frame is past its capacity until `ranges_main` says so, and the passes
    // after it dispatch nothing (`x` 0, `y` and `z` 1).
    if (t < 8) drop_words()[t] = t == 1 || t == 2 || t == 4 || t == 5 ? 1u : 0u;
  }
  // The sequence's block sums, which `ranges_main` adds into past the capacity: four words a group
  // (the sequence has at most twice the instances).
  if (t < 4) sequence_sums()[4 * group + t] = 0;
  const uint begin = group * k_ranges_block + t * k_ranges_each;
  uint own = 0;
  uint own_cached = 0;
  for (uint k = 0; k < k_ranges_each; ++k) {
    const uint i = begin + k;
    if (i >= n) break;
    const uint count = drawn[i] + casters[i];
    if (kCache != 0 && cached_at(i)) {
      own_cached += count;
      drawn[i] |= k_count_cached;
      casters[i] |= k_count_cached;
    } else {
      own += count;
    }
  }
  const uint4 sums = group_sum(t, uint4(own, own_cached, 0u, 0u));
  if (t == 0 && group < ranges_blocks()) {
    uint* blocks = block_sums();
    blocks[2 * group] = sums.x;
    blocks[2 * group + 1] = sums.y;
  }
}

[shader("compute")]
[numthreads(256, 1, 1)]
void ranges_count_main(uint3 group : SV_GroupID, uint3 group_thread : SV_GroupThreadID) {
  ranges_count<0>(group.x, group_thread.x);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void ranges_count_cached_main(uint3 group : SV_GroupID, uint3 group_thread : SV_GroupThreadID) {
  ranges_count<1>(group.x, group_thread.x);
}


// The second dispatch, over the same groups.
void ranges<let kCache : int>(uint group, uint t) {
  uint* drawn = drawn_counts();
  uint* casters = caster_counts();
  uint* first = instance_first();
  BlasRecord* blas = (BlasRecord*)g_params.blas_records;
  uint* record_count = (uint*)g_params.record_count;
  const uint n = g_params.instance_count;
  // The totals and this block's offset, from the block sums: at most a few hundred words (a block
  // is 1,024 instances), each group summing them alike.
  const uint blocks = ranges_blocks();
  uint* sums = block_sums();
  uint4 mine = uint4(0u, 0u, 0u, 0u);  // built, cached; then the same of the blocks before this one
  for (uint j = t; j < blocks; j += k_ranges_threads) {
    const uint built = sums[2 * j];
    const uint cached = kCache != 0 ? sums[2 * j + 1] : 0u;
    mine += uint4(built, cached, j < group ? built : 0u, j < group ? cached : 0u);
  }
  const uint4 all = group_sum(t, mine);
  const uint wanted = all.x;
  const uint cached_wanted = all.y;
  const uint64_t cached_references = kCache != 0 ? cache_tables()->references : 0;
  if (wanted <= g_params.capacity && (kCache == 0 || cached_wanted <= reference_capacity())) {
    // Everything fits: nothing is dropped, and each thread writes its instances from its prefix.
    const uint begin = group * k_ranges_block + t * k_ranges_each;
    uint own = 0;
    uint own_cached = 0;
    for (uint k = 0; k < k_ranges_each; ++k) {
      const uint i = begin + k;
      if (i >= n) break;
      const uint word = drawn[i];
      const uint count = (word & k_count_mask) + (casters[i] & k_count_mask);
      if (kCache != 0 && (word & k_count_cached) != 0) {
        own_cached += count;
      } else {
        own += count;
      }
    }
    const uint2 before = group_exclusive(t, uint2(own, own_cached)) + all.zw;
    uint total = before.x;
    uint cached_total = before.y;
    for (uint k = 0; k < k_ranges_each; ++k) {
      const uint i = begin + k;
      if (i >= n) break;
      uint d = drawn[i];
      uint c = casters[i];
      const bool cached = kCache != 0 && (d & k_count_cached) != 0;
      if (kCache != 0) {
        d &= k_count_mask;
        c &= k_count_mask;
        drawn[i] = d;
        casters[i] = c;
      }
      BlasRecord record;
      record.count = d + c;
      record.stride = 8;
      if (cached) {
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
    if (group == 0 && t == 0) {
      record_count[0] = wanted;
      record_count[1] = wanted;
      record_count[2] = 0;
      record_count[3] = 0;
      record_count[k_count_refs_kept] = cached_wanted;
      record_count[k_count_refs_wanted] = cached_wanted;
    }
    return;
  }
  // **Past the capacity** (R106): the rule below as a scan, the one-thread walk only past the
  // crossing. This dispatch lays the order out as the walk spends it — every order entry's drawn
  // item and caster item at their places in one sequence (`sequence_items`), each rank's drawn
  // items and then its casters — and sums each 1,024 items of it; `ranges_drop_main` walks the
  // sequence; `ranges_kept_count_main` and `ranges_kept_main` write what was kept. **Only when
  // the renderer recorded those three** (`k_drop_scan` in `mode`): it does on the frames after one
  // that dropped anything, and a frame past its capacity with no such frame before it takes the
  // one-thread rule here, so a scene that always fits records no pass for a walk it never takes.
  if ((g_params.mode & k_drop_scan) == 0) {
    if (group == 0 && t == 0) drop_serial<kCache>(wanted, cached_wanted);
    return;
  }
  over_capacity<kCache>(group, t);
}

// ---- past the capacity (R106) --------------------------------------------------------------------
//
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
// structure cache"), by the same rule; an instance dropped loses its counts and with them its
// mark, and keeps nothing of either kind.
//
// That rule is `drop_serial` below, one thread over every instance, and was the whole of it until
// R106: 34 to 44 ms of the forest proof of concept's chain at 11520×2160, whose ground alone
// outgrows the budget every frame. It is still what a frame past its capacity computes, word for
// word, by four passes:
//
//   1. `ranges_main` (above), every group: each order entry's **drawn item** and **caster item**
//      at their places in the sequence the rule spends — a rank `[b, e)` of the order holds places
//      `[2b, 2e)`, its drawn items first (`b + k`), then its casters (`e + k`), the rank's bounds
//      found by a binary search over the order's ranks, which are sorted — and each 1,024 places'
//      sums per kind. An instance the order names is marked visited.
//   2. `ranges_drop_main`, one workgroup: the walk. Until the first item that does not fit nothing
//      is dropped, so whole blocks of the sequence are taken from their sums alone, 256 blocks a
//      look; from there a block at a time: a kind whose block fits is taken whole, one whose
//      smallest item does not fit loses every item of it, and only a block that keeps part of
//      itself is walked item by item, by one thread, over shared memory. After the crossing the
//      room left is less than the item that crossed it, so those walks are few.
//   3. `ranges_kept_count_main`: an instance the order did not name is dropped (and counted), and
//      each block of instances sums what it kept.
//   4. `ranges_kept_main`: the prefix sums of what was kept, the dense bases and the bottom-level
//      records, as the fitting frame's `ranges_main` writes them.
//
// The last three are dispatched indirectly from the drop words, which `ranges_count_main` empties
// and `ranges_main` fills only past the capacity, so a frame that fits dispatches none of them. An
// order whose ranks are not sorted, or that names more entries than there are instances — neither
// of which the renderer writes — falls back to `drop_serial`, on `ranges_drop_main`'s one thread.
// The order names an instance at most once (`rt_drop_order` writes each slot once).

static const uint k_count_visited = 0x80000000u;
static const uint k_drop_over = 1;    // the frame is past its capacity
static const uint k_drop_serial = 2;  // and its order is one the scan does not take
static const uint k_item_caster = 1;  // a sequence item's flags
static const uint k_item_cached = 2;

// The words past the block sums (gfx::cluster_instance_count_words): the kept block sums, two a
// block; the sequence's block sums, two a block of 1,024 items (at most twice the instance
// blocks); the drop words — the drop pass's dispatch, the kept passes' dispatch, the state; then
// the sequence, four words an item, two items an order entry.
uint* kept_sums() { return block_sums() + 2 * max(ranges_blocks(), 1u); }
uint* sequence_sums() { return kept_sums() + 2 * max(ranges_blocks(), 1u); }
uint* drop_words() { return sequence_sums() + 4 * max(ranges_blocks(), 1u); }
uint* sequence_items() { return drop_words() + 8; }
static const uint k_drop_args = 0;       // three words: the drop pass's dispatch
static const uint k_drop_kept_args = 3;  // three words: the kept passes' dispatch
static const uint k_drop_state = 6;

uint order_count() {
  uint* order = (uint*)g_params.drop_order;
  return g_params.drop_order != 0 ? order[0] : g_params.instance_count;
}
uint order_instance(uint k) {
  uint* order = (uint*)g_params.drop_order;
  return g_params.drop_order != 0 ? order[2 + 2 * k] : k;
}
uint order_rank(uint k) {
  uint* order = (uint*)g_params.drop_order;
  return g_params.drop_order != 0 ? order[2 + 2 * k + 1] : 0u;
}

// The first order entry whose rank is at least `rank` (`above` false), or more than it (true).
uint rank_bound(uint rank, uint m, bool above) {
  uint lo = 0;
  uint hi = m;
  while (lo < hi) {
    const uint mid = (lo + hi) / 2;
    const uint r = order_rank(mid);
    if (above ? r <= rank : r < rank) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

void over_capacity<let kCache : int>(uint group, uint t) {
  uint* drawn = drawn_counts();
  uint* casters = caster_counts();
  uint* words = drop_words();
  const uint n = g_params.instance_count;
  const uint m = order_count();
  const uint groups = max(ranges_blocks(), 1u);
  if (group == 0 && t == 0) {
    Atomic<uint>* state = (Atomic<uint>*)(words + k_drop_state);
    state.or(m > n ? k_drop_over | k_drop_serial : k_drop_over);
    words[k_drop_args] = 1;
    words[k_drop_kept_args] = groups;
  }
  if (m > n) return;  // more entries than the sequence has room for: the serial rule takes it
  uint* items = sequence_items();
  Atomic<uint>* seq_sums = (Atomic<uint>*)sequence_sums();
  const uint stride = groups * k_ranges_threads;
  for (uint k = group * k_ranges_threads + t; k < m; k += stride) {
    const uint i = order_instance(k);
    const uint rank = order_rank(k);
    if (k > 0 && order_rank(k - 1) > rank) {
      ((Atomic<uint>*)(words + k_drop_state)).or(k_drop_serial);
    }
    const uint b = g_params.drop_order != 0 ? rank_bound(rank, m, false) : 0u;
    const uint e = g_params.drop_order != 0 ? rank_bound(rank, m, true) : m;
    uint d = 0;
    uint c = 0;
    uint flags = 0;
    if (i < n) {
      d = drawn[i];
      c = casters[i];
      if (kCache != 0 && (d & k_count_cached) != 0) flags = k_item_cached;
      drawn[i] = d | k_count_visited;
    }
    d &= k_count_mask;
    c &= k_count_mask;
    const uint drawn_at = min(b + k, 2 * m - 1);
    const uint caster_at = min(e + k, 2 * m - 1);
    const uint instance = i < n ? i : 0xffffffffu;
    uint* item = items + 4 * drawn_at;
    item[0] = d;
    item[1] = instance;
    item[2] = flags;
    item[3] = caster_at;
    item = items + 4 * caster_at;
    item[0] = c;
    item[1] = instance;
    item[2] = flags | k_item_caster;
    item[3] = 0xffffffffu;
    const uint kind = (flags & k_item_cached) != 0 ? 1u : 0u;
    if (d != 0) seq_sums[2 * (drawn_at / k_ranges_block) + kind].add(d);
    if (c != 0) seq_sums[2 * (caster_at / k_ranges_block) + kind].add(c);
  }
}

// The rule on one thread, over the order and then every instance, as it was before R106: what an
// order the scan does not take falls back to.
void drop_serial<let kCache : int>(uint wanted, uint cached_wanted) {
  uint* drawn = drawn_counts();
  uint* casters = caster_counts();
  uint* first = instance_first();
  BlasRecord* blas = (BlasRecord*)g_params.blas_records;
  uint* record_count = (uint*)g_params.record_count;
  const uint n = g_params.instance_count;
  const uint64_t cached_references = kCache != 0 ? cache_tables()->references : 0;
  uint left = g_params.capacity;
  uint left_cached = reference_capacity();
  uint dropped_drawn = 0;
  uint dropped_casters = 0;
  // `over_capacity` marked the instances it reached; this walk marks its own.
  for (uint i = 0; i < n; ++i)
    drawn[i] &= ~k_count_visited;
  const uint m = order_count();
  uint begin_rank = 0;
  while (begin_rank < m) {
    const uint rank = order_rank(begin_rank);
    uint end_rank = begin_rank;
    while (end_rank < m && (g_params.drop_order == 0 || order_rank(end_rank) == rank)) {
      const uint i = order_instance(end_rank);
      if (i < n) {
        const uint word = drawn[i];
        const bool cached = kCache != 0 && (word & k_count_cached) != 0;
        if (!take(left, left_cached, cached, word & k_count_mask)) {
          drawn[i] = 0;
          casters[i] = 0;
          ++dropped_drawn;
        }
        drawn[i] |= k_count_visited;
      }
      ++end_rank;
    }
    for (uint k = begin_rank; k < end_rank; ++k) {
      const uint i = order_instance(k);
      if (i >= n) continue;
      const uint word = casters[i];
      const bool cached = kCache != 0 && (word & k_count_cached) != 0;
      if (!take(left, left_cached, cached, word & k_count_mask)) {
        casters[i] = 0;
        ++dropped_casters;
      }
    }
    begin_rank = end_rank;
  }
  // An instance the order did not name builds nothing (and is counted), so the build never holds
  // more than the capacity whatever the order says.
  for (uint i = 0; i < n; ++i) {
    if ((drawn[i] & k_count_visited) != 0) {
      drawn[i] &= ~k_count_visited;
    } else if (((drawn[i] | casters[i]) & k_count_mask) != 0) {
      drawn[i] = 0;
      casters[i] = 0;
      ++dropped_drawn;
    }
  }
  uint total = 0;
  uint cached_total = 0;
  for (uint i = 0; i < n; ++i) {
    const bool cached = kCache != 0 && ((drawn[i] | casters[i]) & k_count_cached) != 0;
    drawn[i] &= k_count_mask;
    casters[i] &= k_count_mask;
    BlasRecord record;
    record.count = drawn[i] + casters[i];
    record.stride = 8;
    if (cached) {
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

// The walk's block of the sequence, in shared memory, and what the group says about it.
groupshared uint4 g_block_items[k_ranges_block];
groupshared uint g_walk[4];  // the room left of each kind after a serial walk, then a flag

// An item that does not fit: a drawn item takes its instance's casters with it — zeroes the
// instance's counts, keeping it visited, and its caster item, which comes later in the sequence
// (in this block's shared copy, or the sequence itself) — and a caster item only the casters.
void drop_item(uint4 item, uint block_base, inout uint dropped_drawn, inout uint dropped_casters) {
  const uint i = item.y;
  if ((item.z & k_item_caster) != 0) {
    caster_counts()[i] = 0;
    ++dropped_casters;
    return;
  }
  drawn_counts()[i] = k_count_visited;
  caster_counts()[i] = 0;
  ++dropped_drawn;
  const uint partner = item.w;
  if (partner >= block_base && partner < block_base + k_ranges_block) {
    g_block_items[partner - block_base].x = 0;
  } else {
    // **Atomic, not a plain store** (R106, the Titan Xp): the caster item is in a later block,
    // which other invocations of this same dispatch load after the group's barrier. The module
    // is on the GLSL450 memory model, under which a plain store to a storage buffer is not
    // guaranteed visible to another invocation's plain load in the same dispatch, barrier or no
    // barrier — Pascal's L1 can hold the line from the previous block's load (the sequence is
    // only 4-byte aligned, so a line straddles two blocks) and serve the item's old count, and
    // the walk then drops a caster whose drawn item it already dropped. Atomics go to L2; so
    // does the load of `.x` in `ranges_drop` that reads it.
    ((Atomic<uint>*)sequence_items())[4 * partner].store(0u);
  }
}

void ranges_drop<let kCache : int>(uint t) {
  uint* words = drop_words();
  const uint state = words[k_drop_state];
  if ((state & k_drop_over) == 0) return;
  // The totals again, from the instance blocks' sums, for the words.
  const uint blocks = ranges_blocks();
  uint* sums = block_sums();
  uint4 mine = uint4(0u, 0u, 0u, 0u);
  for (uint j = t; j < blocks; j += k_ranges_threads) {
    mine.x += sums[2 * j];
    mine.y += kCache != 0 ? sums[2 * j + 1] : 0u;
  }
  const uint4 all = group_sum(t, mine);
  if ((state & k_drop_serial) != 0) {
    if (t == 0) drop_serial<kCache>(all.x, all.y);
    return;
  }
  const uint item_count = 2 * order_count();
  const uint sequence_blocks = (item_count + k_ranges_block - 1) / k_ranges_block;
  uint* seq_sums = sequence_sums();
  uint* items = sequence_items();
  uint left = g_params.capacity;
  uint left_cached = kCache != 0 ? reference_capacity() : 0u;
  uint dropped_drawn = 0;  // this thread's
  uint dropped_casters = 0;
  bool failed = false;  // nothing dropped yet: a block's sums are exactly what it holds
  uint at = 0;          // the next block of the sequence
  while (at < sequence_blocks) {
    if (!failed) {
      // Before the crossing: as many whole blocks as fit, 256 at a look, from their sums alone.
      const uint j = at + t;
      const uint2 own = j < sequence_blocks
                            ? uint2(seq_sums[2 * j], kCache != 0 ? seq_sums[2 * j + 1] : 0u)
                            : uint2(0u, 0u);
      const uint2 through = group_exclusive(t, own) + own;
      const bool fits = j < sequence_blocks && through.x <= left && through.y <= left_cached;
      const uint4 counted = group_sum(t, uint4(fits ? 1u : 0u, 0u, 0u, 0u));
      const uint whole = counted.x;  // the fitting blocks are a prefix: the sums only grow
      if (whole > 0) {
        GroupMemoryBarrierWithGroupSync();
        if (t == whole - 1) {
          g_walk[0] = through.x;
          g_walk[1] = through.y;
        }
        GroupMemoryBarrierWithGroupSync();
        left -= g_walk[0];
        left_cached -= g_walk[1];
        at += whole;
        continue;
      }
    }
    // A block that does not fit whole: its items, to shared memory.
    const uint base = at * k_ranges_block;
    uint4 totals = uint4(0u, 0u, 0xffffffffu, 0xffffffffu);  // per kind: sum, smallest
    for (uint r = 0; r < k_ranges_each; ++r) {
      const uint local = t + r * k_ranges_threads;
      uint4 item = uint4(0u, 0xffffffffu, 0u, 0xffffffffu);
      if (base + local < item_count) {
        uint* src = items + 4 * (base + local);
        // `.x` may have been zeroed by an earlier block of this dispatch (`drop_item`): an atomic
        // load, so it is never a stale line in this invocation's L1. The other three words are
        // written only by `ranges_main`, a dispatch before.
        item = uint4(((Atomic<uint>*)src)[0].load(), src[1], src[2], src[3]);
      }
      g_block_items[local] = item;
      if (item.x != 0) {
        if ((item.z & k_item_cached) != 0) {
          totals.y += item.x;
          totals.w = min(totals.w, item.x);
        } else {
          totals.x += item.x;
          totals.z = min(totals.z, item.x);
        }
      }
    }
    const uint4 block = group_sum_min(t, totals);
    const bool whole_built = block.x <= left;
    const bool whole_cached = block.y <= left_cached;
    const bool none_built = !whole_built && block.z > left;
    const bool none_cached = !whole_cached && block.w > left_cached;
    if ((!whole_built && !none_built) || (!whole_cached && !none_cached)) {
      // Part of the block: walked item by item, in order, by one thread over shared memory.
      if (t == 0) {
        for (uint k = 0; k < k_ranges_block; ++k) {
          const uint4 item = g_block_items[k];
          if (item.x == 0) continue;
          const bool cached = (item.z & k_item_cached) != 0;
          if (!take(left, left_cached, cached, item.x)) {
            drop_item(item, base, dropped_drawn, dropped_casters);
          }
        }
        g_walk[0] = left;
        g_walk[1] = left_cached;
      }
      AllMemoryBarrierWithGroupSync();
      left = g_walk[0];
      left_cached = g_walk[1];
    } else {
      // Each kind whole or not at all: a kind kept whole spends its sum; one that does not fit at
      // all loses its drawn items — and with them the casters behind them — and then whatever
      // casters are left.
      if (whole_built) left -= block.x;
      if (whole_cached) left_cached -= block.y;
      for (uint pass = 0; pass < 2; ++pass) {
        for (uint r = 0; r < k_ranges_each; ++r) {
          const uint4 item = g_block_items[t + r * k_ranges_threads];
          const bool caster = (item.z & k_item_caster) != 0;
          if (item.x == 0 || caster != (pass == 1)) continue;
          const bool cached = (item.z & k_item_cached) != 0;
          if (cached ? none_cached : none_built) {
            drop_item(item, base, dropped_drawn, dropped_casters);
          }
        }
        AllMemoryBarrierWithGroupSync();
      }
    }
    failed = failed || !whole_built || !whole_cached;
    ++at;
  }
  const uint4 dropped = group_sum(t, uint4(dropped_drawn, dropped_casters, 0u, 0u));
  if (t == 0) {
    uint* record_count = (uint*)g_params.record_count;
    record_count[1] = all.x;
    record_count[2] = dropped.x;  // `ranges_kept_count_main` adds the instances the order missed
    record_count[3] = dropped.y;
    record_count[k_count_refs_wanted] = all.y;
  }
}

// The group's sums of the first two words and smallest of the last two.
uint4 group_sum_min(uint t, uint4 value) {
  GroupMemoryBarrierWithGroupSync();
  g_ranges_sum[t] = value;
  GroupMemoryBarrierWithGroupSync();
  for (uint step = k_ranges_threads / 2; step > 0; step >>= 1) {
    if (t < step) {
      const uint4 a = g_ranges_sum[t];
      const uint4 b = g_ranges_sum[t + step];
      g_ranges_sum[t] = uint4(a.x + b.x, a.y + b.y, min(a.z, b.z), min(a.w, b.w));
    }
    GroupMemoryBarrierWithGroupSync();
  }
  return g_ranges_sum[0];
}

// What each block of instances kept, after the walk; an instance the order did not name keeps
// nothing and is counted with the instances that lost their drawn clusters.
void ranges_kept_count<let kCache : int>(uint group, uint t) {
  const uint state = drop_words()[k_drop_state];
  if ((state & k_drop_over) == 0 || (state & k_drop_serial) != 0) return;
  uint* drawn = drawn_counts();
  uint* casters = caster_counts();
  const uint n = g_params.instance_count;
  const uint begin = group * k_ranges_block + t * k_ranges_each;
  uint own = 0;
  uint own_cached = 0;
  uint unnamed = 0;
  for (uint k = 0; k < k_ranges_each; ++k) {
    const uint i = begin + k;
    if (i >= n) break;
    uint d = drawn[i];
    uint c = casters[i];
    if ((d & k_count_visited) != 0) {
      d &= ~k_count_visited;
      drawn[i] = d;
    } else if (((d | c) & k_count_mask) != 0) {
      d = 0;
      c = 0;
      drawn[i] = 0;
      casters[i] = 0;
      ++unnamed;
    }
    const uint count = (d & k_count_mask) + (c & k_count_mask);
    if (kCache != 0 && ((d | c) & k_count_cached) != 0) {
      own_cached += count;
    } else {
      own += count;
    }
  }
  const uint4 sums = group_sum(t, uint4(own, own_cached, unnamed, 0u));
  if (t == 0 && group < ranges_blocks()) {
    uint* kept = kept_sums();
    kept[2 * group] = sums.x;
    kept[2 * group + 1] = sums.y;
    if (sums.z != 0) ((Atomic<uint>*)g_params.record_count)[2].add(sums.z);
  }
}

// The prefix sums of what was kept, written as the fitting frame's are.
void ranges_kept<let kCache : int>(uint group, uint t) {
  const uint state = drop_words()[k_drop_state];
  if ((state & k_drop_over) == 0 || (state & k_drop_serial) != 0) return;
  uint* drawn = drawn_counts();
  uint* casters = caster_counts();
  uint* first = instance_first();
  BlasRecord* blas = (BlasRecord*)g_params.blas_records;
  const uint n = g_params.instance_count;
  const uint blocks = ranges_blocks();
  uint* kept = kept_sums();
  uint4 mine = uint4(0u, 0u, 0u, 0u);
  for (uint j = t; j < blocks; j += k_ranges_threads) {
    const uint built = kept[2 * j];
    const uint cached = kCache != 0 ? kept[2 * j + 1] : 0u;
    mine += uint4(built, cached, j < group ? built : 0u, j < group ? cached : 0u);
  }
  const uint4 all = group_sum(t, mine);
  const uint64_t cached_references = kCache != 0 ? cache_tables()->references : 0;
  const uint begin = group * k_ranges_block + t * k_ranges_each;
  uint own = 0;
  uint own_cached = 0;
  for (uint k = 0; k < k_ranges_each; ++k) {
    const uint i = begin + k;
    if (i >= n) break;
    const uint d = drawn[i];
    const uint c = casters[i];
    const uint count = (d & k_count_mask) + (c & k_count_mask);
    if (kCache != 0 && ((d | c) & k_count_cached) != 0) {
      own_cached += count;
    } else {
      own += count;
    }
  }
  const uint2 before = group_exclusive(t, uint2(own, own_cached)) + all.zw;
  uint total = before.x;
  uint cached_total = before.y;
  for (uint k = 0; k < k_ranges_each; ++k) {
    const uint i = begin + k;
    if (i >= n) break;
    uint d = drawn[i];
    uint c = casters[i];
    const bool cached = kCache != 0 && ((d | c) & k_count_cached) != 0;
    d &= k_count_mask;
    c &= k_count_mask;
    drawn[i] = d;
    casters[i] = c;
    BlasRecord record;
    record.count = d + c;
    record.stride = 8;
    if (cached) {
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
  if (group == 0 && t == 0) {
    uint* record_count = (uint*)g_params.record_count;
    record_count[0] = all.x;  // what the build reads: never more than the capacity
    record_count[k_count_refs_kept] = all.y;
  }
}
[shader("compute")]
[numthreads(256, 1, 1)]
void ranges_main(uint3 group : SV_GroupID, uint3 group_thread : SV_GroupThreadID) {
  ranges<0>(group.x, group_thread.x);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void ranges_cached_main(uint3 group : SV_GroupID, uint3 group_thread : SV_GroupThreadID) {
  ranges<1>(group.x, group_thread.x);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void ranges_drop_main(uint3 group_thread : SV_GroupThreadID) {
  ranges_drop<0>(group_thread.x);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void ranges_drop_cached_main(uint3 group_thread : SV_GroupThreadID) {
  ranges_drop<1>(group_thread.x);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void ranges_kept_count_main(uint3 group : SV_GroupID, uint3 group_thread : SV_GroupThreadID) {
  ranges_kept_count<0>(group.x, group_thread.x);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void ranges_kept_count_cached_main(uint3 group : SV_GroupID,
                                   uint3 group_thread : SV_GroupThreadID) {
  ranges_kept_count<1>(group.x, group_thread.x);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void ranges_kept_main(uint3 group : SV_GroupID, uint3 group_thread : SV_GroupThreadID) {
  ranges_kept<0>(group.x, group_thread.x);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void ranges_kept_cached_main(uint3 group : SV_GroupID, uint3 group_thread : SV_GroupThreadID) {
  ranges_kept<1>(group.x, group_thread.x);
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
// its own, but a reference — slot `dense` of the cache's `references` — and a look-up of its key.
// A key whose structure is resident has its slot stamped with this frame, which is what keeps the
// eviction off it, and the reference is its slot's address, written here. One that is not resident
// is **pending**: its key is parked in the reference itself and its dense index listed in
// `reference_keys`, for `cache_references_main` to address once the misses have slots; and the key
// is claimed by exactly one thread (the compare-exchange) and listed as a miss, which
// `cache_assign_main` gives a slot and a build record.
//
// **An idle cache does no walk** (R106): the cache's four passes after this one are dispatched
// indirectly from the args block, every dispatch empty until this pass grows it — the slot scan
// and the eviction by the frame's first miss, the misses' slots by every 64th, the pending
// references by every 64th of those — so a frame that misses nothing dispatches no thread past
// this pass, and one whose references are all resident none either.
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
  Atomic<uint>* keys = (Atomic<uint>*)tables->keys;
  const uint value = keys[key].load();
  uint64_t* references = (uint64_t*)tables->references;
  if (value < k_cache_overflow) {
    uint* stamps = (uint*)tables->stamps;
    stamps[value] = g_params.frame;
    references[dense] = tables->slot_base + uint64_t(value) * tables->slot_stride;
    return;
  }
  references[dense] = key;
  Atomic<uint>* args = (Atomic<uint>*)tables->args;
  const uint pending = args[k_args_pending].add(1);
  uint* reference_keys = (uint*)tables->reference_keys;
  reference_keys[pending] = dense;
  if (pending % k_cache_workgroup == 0) args[k_args_references].add(1);
  if (value == k_cache_claim) return;  // another entry of this frame claimed it
  if (keys[key].compareExchange(value, k_cache_claim) != value) return;
  Atomic<uint>* counts = (Atomic<uint>*)g_params.record_count;
  const uint index = counts[k_count_misses].add(1);
  if (index == 0) {
    // The frame's first miss: the slot scan and the eviction walk the slots.
    const uint slot_groups = (tables->slot_count + k_cache_workgroup - 1) / k_cache_workgroup;
    args[k_args_scan].store(slot_groups);
    args[k_args_evict].store(slot_groups);
  }
  if (index < tables->slot_count && index % k_cache_workgroup == 0) {
    args[k_args_assign].add(1);
  }
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
// positions for it. Two variants like the ranges': `emit_main` for a frame with no cache, which
// never asks whether an instance is cached, and `emit_cached_main`.
void emit<let kCache : int>(uint slot) {
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
  if (kCache != 0 && instance_cached(instance)) {
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

[shader("compute")]
[numthreads(64, 1, 1)]
void emit_main(uint3 thread_id : SV_DispatchThreadID) {
  emit<0>(thread_id.x);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void emit_cached_main(uint3 thread_id : SV_DispatchThreadID) {
  emit<1>(thread_id.x);
}

// ---- the resident cluster structure cache (renderer.md, "The resident cluster structure cache") --
//
// After the emit pass every cached entry has looked its key up: a resident key's slot carries this
// frame's stamp and its reference that slot's address, and a missing one is claimed and listed in
// `misses`, its references listed as pending. Then four passes, each dispatched indirectly from the
// args block the emit pass grew (`k_args_*`), so each is empty on a frame with nothing for it: over
// the slots, the slot scan and the eviction (a frame with a miss); over the misses, their slots;
// over the pending references, their addresses.

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

// How many slots the misses need beyond the free ones, and the oldest ages that hold them — every
// slot older than `threshold`, and `quota` of those exactly that old. **Least recently drawn
// first**, to the frame within the histogram's 63, everything older as one. Each group of the
// eviction works it out for itself from the scan's histogram (64 words), which is what the one-
// thread pass that did it before R106 cost a dispatch and a barrier a frame for.
groupshared uint g_threshold;
groupshared uint g_quota;

// Evicts what the threshold chose: the key loses its slot (its next draw misses and builds it
// again) and the slot joins the free list behind the ones the scan found.
[shader("compute")]
[numthreads(64, 1, 1)]
void cache_evict_main(uint3 thread_id : SV_DispatchThreadID,
                      uint3 group_thread : SV_GroupThreadID) {
  CacheTables* tables = cache_tables();
  uint* words = (uint*)tables->scratch;
  if (group_thread.x == 0) {
    uint* record_count = (uint*)g_params.record_count;
    const uint misses = min(record_count[k_count_misses], tables->slot_count);
    const uint free = words[0];
    const uint need = misses > free ? misses - free : 0u;
    uint threshold = k_cache_ages;  // none
    uint quota = 0;
    if (need > 0) {
      uint held = 0;
      threshold = 1;
      quota = words[4 + 1];  // every slot not drawn this frame, if that is what it takes
      for (int age = int(k_cache_ages) - 1; age >= 1; --age) {
        const uint count = words[4 + age];
        if (held + count >= need) {
          threshold = uint(age);
          quota = need - held;
          break;
        }
        held += count;
      }
    }
    g_threshold = threshold;
    g_quota = quota;
  }
  GroupMemoryBarrierWithGroupSync();
  const uint threshold = g_threshold;
  const uint quota = g_quota;
  const uint slot = thread_id.x;
  if (threshold >= k_cache_ages || slot == 0 || slot >= tables->slot_count) return;
  uint* owners = (uint*)tables->owners;
  const uint owner = owners[slot];
  if (owner == k_cache_empty) return;
  uint* stamps = (uint*)tables->stamps;
  const uint age = g_params.frame - stamps[slot];
  if (age == 0) return;
  Atomic<uint>* scratch = (Atomic<uint>*)tables->scratch;
  const uint bucket = min(age, k_cache_ages - 1);
  if (bucket < threshold) return;
  if (bucket == threshold && scratch[3].add(1) >= quota) return;
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
// explicit build reads and how many slots are occupied after it, which the scratch keeps for the
// frames with no miss (`ranges_count_cached_main` copies it into theirs).
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
    const uint occupied = tables->slot_count - 1 - free + assigned;
    record_count[k_count_miss_built] = assigned;
    record_count[k_count_occupied] = occupied;
    scratch[k_scratch_occupied] = occupied;
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

// Every pending reference's address — its key, parked in the reference by the emit pass, now has
// its slot, or the inactive structure for a key the cache had no room for this frame. A resident
// key's references were addressed by the emit pass itself.
[shader("compute")]
[numthreads(64, 1, 1)]
void cache_references_main(uint3 thread_id : SV_DispatchThreadID) {
  CacheTables* tables = cache_tables();
  uint* args = (uint*)tables->args;
  const uint p = thread_id.x;
  if (p >= args[k_args_pending]) return;
  uint* reference_keys = (uint*)tables->reference_keys;
  const uint dense = reference_keys[p];
  uint64_t* references = (uint64_t*)tables->references;
  uint* keys = (uint*)tables->keys;
  const uint slot = keys[uint(references[dense])];
  references[dense] =
      tables->slot_base + (slot < k_cache_overflow ? uint64_t(slot) * tables->slot_stride : 0u);
}
