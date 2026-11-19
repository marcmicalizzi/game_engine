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
//                 of CLAS addresses — and past the capacity five more passes, dispatched only
//                 then: the cut (keep_cut_main, keep_ties_main, keep_fine_main) and the kept
//                 instances' scan (ranges_kept_count_main, ranges_kept_main), under "past the
//                 capacity" below.
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
// built; what a frame wants beyond it is dropped whole instances at a time, the smallest on screen
// first and on the ray path every drawn cluster before any caster, so which instances lose their
// structures is a function of the placements, the eye and the capacity, and not of the order the
// layout or the atomics put them in (docs/subsystems/renderer.md, "Past the capacity, by size on
// screen").
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
//
// **An untraced instance** (the caster policy's `traced = false`; R110) is left out by
// records_main in a frame whose picture is rasterized, and built from its drawn entries alone in a
// traced-primary frame (`k_untraced_drawn` in `mode`); no light's ray meets it in either, by its
// top-level record's mask (tlas_references.slang, `instance_ray_mask` in scene.slang).

#include "dispatch.slang"
#include "scene.slang"
#include "field_rest.slang"

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

// Mirrors gfx::ClusterRecordParams (200 bytes: past the 128 every device guarantees, which this
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
  // **The keep block** (renderer.md, "Past the capacity, by size on screen"; gfx::ClusterKeepTables):
  // what a frame past its capacity cuts through. Read only past the capacity.
  uint64_t keep;
  uint64_t hit_entries;
  // **The resident cluster structure cache** (renderer.md, "The resident cluster structure cache";
  // gfx::ClusterCacheTables): its tables, or 0 for none; and the frame's stamp.
  uint64_t cache;
  uint frame;
  uint fade_stride;  // the fade table's stride between views, in pairs
  // **Opacity micromaps** (gfx::MicromapTables, opacity_micromap.h; R120): the micromap, the
  // scene's index buffer and each cluster's first index, or 0 for none. A drawn alpha-tested
  // pair's record names them; a caster's never does, because a primary ray would commit its opaque
  // micro-triangles where the caster must be passed through.
  uint64_t omm;
  uint64_t omm_fade;  // the cross-fade table, view 0's block, or 0
  // **The far field at rest** (field_rest.slang; R152): the frame's FloraFieldFrame, or 0. A field
  // instance wholly past its rest distance is a cached instance this frame (`instance_cached`).
  uint64_t field;
};

// Mirrors gfx::ClusterCacheTables (120 bytes).
struct CacheTables {
  uint64_t bases;              // uint per mesh: its first cluster's key / 3, or ~0
  uint64_t keys;               // uint per key: its slot, or a mark
  uint64_t owners;             // uint per slot: its key, or k_cache_empty
  uint64_t stamps;             // uint per slot: the last frame that drew it
  uint64_t free_list;          // uint per slot
  uint64_t misses;             // uint4 per reference: key, cluster, geometry word, 0
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
// gfx::k_cluster_cache_frame: a key's value at or above it is no slot. Under the three marks, it
// names the frame's own record that built the key's structure this frame (R114).
static const uint k_cache_frame = 0x80000000u;
static const uint k_cache_forms = 4;               // gfx::k_cluster_cache_forms
static const uint k_cache_geometry = 1u << 23;     // gfx::k_cluster_cache_geometry
static const uint k_cache_caster = 1u << 22;       // gfx::k_cluster_cache_caster
static const uint k_cache_ages = 64;               // gfx::k_cluster_cache_ages
static const uint k_cache_workgroup = 64;          // gfx::k_cluster_cache_workgroup
// The scratch's words: what `ranges_count_main` clears every frame, and the occupied count after
// them, which only a frame with misses changes (gfx::k_cluster_cache_scratch_occupied).
static const uint k_scratch_cleared = 4 + k_cache_ages;
static const uint k_scratch_occupied = k_scratch_cleared;
// The frame's own records before its misses (gfx::k_cluster_cache_scratch_built): word 0 of the
// record count as the ranges left it, copied by the slot scan, which `cache_assign_main` raises.
static const uint k_scratch_built = k_scratch_occupied + 1;
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
static const uint k_count_miss_unbuilt = 11;
static const uint k_count_blas = 12;  // gfx::k_cluster_record_count_blas

CacheTables* cache_tables() { return (CacheTables*)g_params.cache; }

// Mirrors gfx::MicromapTables (24 bytes).
struct MicromapTables {
  uint64_t array;          // the micromap's address, or 0: every triangle a special index
  uint64_t indices;        // int per masked triangle
  uint64_t cluster_first;  // uint per cluster: its first index, or k_micromap_none
};
static const uint k_micromap_none = 0xffffffffu;  // gfx::k_micromap_none
static const uint k_index_type_32bit = 4;         // gfx::k_cluster_index_type_32bit

// Whether pair (instance, cluster) is inside a cross-fade band in any view this frame (R120): its
// hits then keep only the pixels its band gives them, which the ray loops decide by the pixel, so
// its record names no micromap (an opaque micro-triangle would commit without asking).
bool pair_fading(InstanceDesc instance, MeshDesc mesh, uint cluster_id) {
  if (g_params.omm_fade == 0) return false;
  uint2* table = (uint2*)g_params.omm_fade;
  const uint pair = pair_of(instance, mesh, cluster_id);
  for (uint v = 0; v < (g_params.mode & 0xffffu); ++v) {
    const uint split = table[uint64_t(v) * g_params.fade_stride + pair].x;
    if ((split & 0xffffu) != 0u || (split >> 16) != 0xffffu) return true;
  }
  return false;
}

// Attaches the cluster's micromap to a record, where the scene baked one for it (R120). Only an
// alpha-tested drawn record asks: its states are conservative, so the traversal decides only what
// the mask would have decided the same way and hands the loops every unknown micro-triangle.
void attach_micromap(inout ClusterRecord record, uint cluster_id) {
  if (g_params.omm == 0) return;
  MicromapTables* tables = (MicromapTables*)g_params.omm;
  if (tables->array == 0) return;
  uint* first = (uint*)tables->cluster_first;
  const uint index = first[cluster_id];
  if (index == k_micromap_none) return;
  record.counts |= k_index_type_32bit << 28;
  record.strides1 |= 4u << 16;
  record.omm_array = tables->array;
  record.omm_indices = tables->indices + uint64_t(index) * 4u;
}

// **A cached instance** (renderer.md, "The resident cluster structure cache"): one whose clusters'
// structures are the cache's, built once, and not this frame's. Rigid — nothing in the pool moves
// its vertices, so every instance of its mesh traces the same clusters — and of a mesh the cache
// keeps. A static form never reaches a records pass that asks. An untraced instance does in a
// traced-primary frame (R110), and since R152 is cached there like any rigid instance: the cache's
// drawn structure is every instance's of its mesh, and what keeps a light's ray off an untraced
// one is its own top-level record's mask, not the structure. (Until R152 it was left out and the
// frame built its drawn entries itself: the forest's litter, rigid and untraced, was most of what
// the owner's worst view built.) Its casters never reach here, in any frame.
//
// **A field instance at rest is cached too** (field_rest.slang; renderer.md, "The far field at
// rest"; R152): wholly past the frame's rest distance the pool gave it no block, so this frame it
// is the rigid instance the cache's structures are built for — the rest pose, from the same floats
// an own record of it would name — and it references them, untraced or not: a traced-primary
// frame's far sward is then a few hundred resident structures and not one per pair. It is asked
// with the frame's eye, so an instance stops being cached the frame it comes inside the distance.
bool instance_cached(InstanceDesc instance) {
  if (g_params.cache == 0) return false;
  if (instance.deform != k_invalid_deform) {
    if (!field_rests_at(g_params.field, instance)) return false;
  } else if ((instance.flags & k_instance_untraced) != 0 && !untraced_drawn()) {
    return false;  // a rasterized picture: none of its entries reach the records at all
  }
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
static const uint k_untraced_drawn = 0x40000;  // gfx::k_cluster_records_untraced_drawn
static const uint k_drawn_first = 0x80000;     // gfx::k_cluster_records_drawn_first

uint view_count() { return g_params.mode & 0xffff; }
// **Past the capacity, every drawn structure before any caster** (R143; renderer.md, "What a frame
// past its capacity keeps"): the kind is the keep key's top bit, so every instance's drawn clusters
// are spent before any caster, where the structures are the picture.
bool drawn_first() { return (g_params.mode & k_drawn_first) != 0; }
bool instantiating() { return (g_params.mode & k_instantiate) != 0; }
// A traced-primary frame: an untraced instance's drawn entries are built (R110).
bool untraced_drawn() { return (g_params.mode & k_untraced_drawn) != 0; }

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
  // A folded dispatch (gfx::fold_dispatch, T160): two runs of a forest's pairs are half a million
  // workgroups.
  const uint t = folded_thread(thread_id, 64);
  const uint run_entries = view_count() * g_params.pair_count;
  const uint runs = g_params.caster_count != 0 ? 2u : 1u;
  if (t >= runs * run_entries) return;
  const bool caster = t >= run_entries;
  const uint within = caster ? t - run_entries : t;
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
  // **The caster policy's `traced = false`** (renderer.md, "The caster policy"; R95, R110): grass
  // is never a caster. In a frame whose picture is rasterized it is in none of the frame's
  // structures — no record, no slot, no count — so it costs the chain nothing; in a traced-primary
  // frame its drawn entries are built as any instance's, because there the structures *are* the
  // picture, and its top-level mask keeps every light's ray off it (tlas_references.slang).
  if ((instance.flags & k_instance_untraced) != 0 && (caster || !untraced_drawn())) return;
  uint* slots = (uint*)g_params.slots;
  if (caster) {
    // **A deformed caster the pool had no room for is not built** (renderer.md, "Shadows"). A
    // drawn entry the budget refuses draws its rest pose, which beats a hole in the picture; a
    // caster's rest pose would cast a shadow where its pose has none — a terrain level's rest
    // heights can be metres from the field it draws — and a wrong shadow is worse than a missing
    // one. It is left out here, before it is counted, so the ranges and the emit never see it.
    MeshDesc* meshes = (MeshDesc*)g_params.meshes;
    const MeshDesc mesh = meshes[instance.mesh];
    // A field instance at rest has no block because its rest pose *is* its pose (field_rest.slang;
    // R152), so its caster stays: the cache's caster form, like a rigid instance's.
    if (is_deformed(mesh, instance) && deform_base(mesh, instance, index) == k_no_pool_slot &&
        !field_rests_at(g_params.field, instance)) {
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
    // The bottom-level structures the frame builds, which `blas_compact_main` counts
    // (tlas_references.slang; R143): none yet.
    if (t == 6) record_count[k_count_blas] = 0;
    // The drop words: no frame is past its capacity until `ranges_main` says so, and the passes
    // after it dispatch nothing (`x` 0, `y` and `z` 1).
    if (t < 8) drop_words()[t] = t == 1 || t == 2 || t == 4 || t == 5 ? 1u : 0u;
  }
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
  // **Past the capacity** (R174; renderer.md, "Past the capacity, by size on screen"): each
  // instance keyed and its items added to their capacity's level-1 histogram; the cut passes and
  // the kept passes after this one, dispatched from the drop words, find and keep the prefix.
  keep_items<kCache>(group, t, wanted, cached_wanted);
}

// ---- past the capacity (R174) --------------------------------------------------------------------
//
// **A prefix of the keep keys** (renderer.md, "Past the capacity, by size on screen"; ADR-0074;
// `gfx::rt_keep` is the same rule on the CPU). Every instance that wants a structure is keyed by
// its size on screen, `q`, and a hash of its placement, `h`; its drawn item and its caster item
// take that key with their kind placed in it (`keep_item_key`), and each capacity — the frame's own
// structures, the cache's references — is spent along the keys as a prefix: the least key at which
// the structures keyed at or below it pass the capacity is the cut `K`, every item keyed below it
// is kept, everything else is dropped, and equal keys go together. An instance dropped loses its
// counts, its casters with its drawn clusters. Nothing reads a place in a list, so the kept set is
// a function of the placements, the eye and the two capacities, never of the order the layout or
// the atomics put the instances in. Five passes past the ranges:
//
//   1. `ranges_main` (above), every group: each instance's key into the keep keys, and each item's
//      structures added at its key's top 16 bits to its capacity's level-1 histogram.
//   2. `keep_cut_main`, one group: each capacity's level-1 histogram scanned to the bin that
//      crosses it, and the structures keyed below that bin.
//   3. `keep_ties_main`, the instance blocks: an item in its capacity's crossing bin adds its
//      structures at its key's low 16 bits (`h`) to the level-2 histogram.
//   4. `keep_fine_main`, one group: the crossing `h`, so `K`, and the room the cut leaves.
//   5. `ranges_kept_count_main` and `ranges_kept_main`: keep what is keyed below `K`, count what
//      was dropped, and the prefix sums and bottom-level records as the fitting frame writes them.
//
// They are dispatched indirectly from the drop words, which `ranges_count_main` empties and
// `ranges_main` fills only past the capacity, so a frame that fits dispatches none of them. The
// histograms and the cut words are cleared at the start of every frame's chain by the renderer's
// "keep clear" pass (renderer.md, "The cut's tables are cleared every frame"), which also writes
// the block's eye and addresses: nothing here is left by one frame for the next.

static const uint k_drop_over = 1;  // the frame is past its capacity

// The words past the block sums (gfx::cluster_instance_count_words): the kept block sums, two a
// block; the drop words — the cut passes' dispatch, the ties' and kept passes' dispatch, the state;
// then the keep keys, two words an instance (gfx::cluster_ranges_keys_offset).
uint* kept_sums() { return block_sums() + 2 * max(ranges_blocks(), 1u); }
uint* drop_words() { return kept_sums() + 2 * max(ranges_blocks(), 1u); }
static const uint k_drop_args = 0;       // three words: the cut passes' dispatch
static const uint k_drop_kept_args = 3;  // three words: the ties' and the kept passes' dispatch
static const uint k_drop_state = 6;

// Mirrors gfx::ClusterKeepTables (88 bytes).
struct KeepTables {
  FrameEye eye;          // view 0's: a set's views share one
  uint64_t mesh_bounds;  // float4 per mesh (CullParams::mesh_bounds), or 0
  uint64_t keys;         // uint[2] per instance: q, h
  uint64_t histograms;   // uint[k_keep_bins * 4]: level 1 built, level 1 cached, level 2 of each
  uint64_t cut;          // uint[16]: five words a capacity (below)
  uint slot_count;
  uint pad;
};
static const uint k_keep_bins = 65536;  // gfx::k_rt_keep_bins
// A capacity's cut words, five from `5 * kind`: whether anything passes it, the crossing level-1
// bin, the structures keyed below that bin, `K`, and the room the cut leaves.
static const uint k_cut_crosses = 0;
static const uint k_cut_bin = 1;
static const uint k_cut_before = 2;
static const uint k_cut_key = 3;
static const uint k_cut_room = 4;

KeepTables* keep_tables() { return (KeepTables*)g_params.keep; }
uint* keep_cut_words(uint kind) { return (uint*)keep_tables()->cut + 5 * kind; }
uint keep_capacity(uint kind) { return kind == 0 ? g_params.capacity : reference_capacity(); }

// One row of the 3x4 applied to the sphere's centre, then the instance's translation from the eye,
// one rounding a line: `gfx::rt_keep_key` mirrors exactly this. Not `instance_point`, whose dots
// are not `precise` and which a driver may fuse.
float keep_axis(float4 row, float4 c, float from_eye) {
  precise const float a = row.x * c.x;
  precise const float b = row.y * c.y;
  precise const float ab = a + b;
  precise const float e = row.z * c.z;
  precise const float abe = ab + e;
  precise const float result = abe + from_eye;
  return result;
}

// `q` (gfx::rt_keep_q): (asuint(d²) − asuint(r²) + 8·2²³) >> 13, clamped first so nothing wraps.
// A zero radius and a NaN distance fall at the top; the radius is tested first, since at the eye d²
// is zero too and the difference would key a sphere-less instance at d = r.
uint keep_q(float d2, float r2) {
  if (!(r2 > 0.0)) return 32767u;
  const int u = int(asuint(d2)) - int(asuint(r2));
  const int offset = 8 << 23;
  const int top = int(32768u << 13) - offset;
  if (u < -offset) return 0u;
  if (u >= top) return 32767u;
  return uint(u + offset) >> 13;
}

// `h` (gfx::rt_keep_hash): lowbias32 over the cell relative to the eye's and the twelve row words,
// seeded as `fade_seed`.
uint keep_hash(InstanceDesc instance, FrameEye eye) {
  uint h = fade_mix(uint(instance.cell.x - eye.cell.x) ^ 0x9e3779b9u);
  h = fade_mix(h ^ uint(instance.cell.y - eye.cell.y));
  h = fade_mix(h ^ uint(instance.cell.z - eye.cell.z));
  for (uint r = 0; r < 3; ++r) {
    h = fade_mix(h ^ asuint(instance.rows[r].x));
    h = fade_mix(h ^ asuint(instance.rows[r].y));
    h = fade_mix(h ^ asuint(instance.rows[r].z));
    h = fade_mix(h ^ asuint(instance.rows[r].w));
  }
  return h >> 16;
}

// The key (gfx::rt_keep_key). **The sphere is the GPU's** (`mesh_bounds_main`), which the CPU's
// mirror is handed back rather than recomputing; without the spheres (the instance pass off) it is
// zero, every instance takes the top `q`, and the hash alone orders them.
uint2 keep_key(InstanceDesc instance, FrameEye eye, float4 sphere) {
  const float3 from_eye = instance_from_eye(instance, eye);
  const float x = keep_axis(instance.rows[0], sphere, from_eye.x);
  const float y = keep_axis(instance.rows[1], sphere, from_eye.y);
  const float z = keep_axis(instance.rows[2], sphere, from_eye.z);
  precise const float xx = x * x;
  precise const float yy = y * y;
  precise const float zz = z * z;
  precise const float xy = xx + yy;
  precise const float d2 = xy + zz;
  precise const float r = sphere.w * instance.scale_max;
  precise const float r2 = r * r;
  return uint2(keep_q(d2, r2), keep_hash(instance, eye));
}

// An item's key (gfx::rt_keep_item_key): drawn first (the ray path) puts every drawn structure
// before any caster, otherwise the drawn go first at one size.
uint keep_item_key(uint2 key, bool caster) {
  const uint kind = caster ? 1u : 0u;
  return drawn_first() ? (kind << 31) | (key.x << 16) | key.y
                       : (key.x << 17) | (kind << 16) | key.y;
}

bool keep_kept(uint kind, uint item_key) {
  uint* cut = keep_cut_words(kind);
  return cut[k_cut_crosses] == 0 || item_key < cut[k_cut_key];
}

// `ranges_main` past the capacity: the drop words, the counts the kept passes add to, and each of
// this thread's instances keyed into the level-1 histogram of its capacity.
void keep_items<let kCache : int>(uint group, uint t, uint wanted, uint cached_wanted) {
  uint* drawn = drawn_counts();
  uint* casters = caster_counts();
  uint* words = drop_words();
  const uint n = g_params.instance_count;
  if (group == 0 && t == 0) {
    words[k_drop_state] = k_drop_over;
    words[k_drop_args] = 1;
    words[k_drop_kept_args] = max(ranges_blocks(), 1u);
    uint* record_count = (uint*)g_params.record_count;
    record_count[1] = wanted;
    record_count[2] = 0;  // the kept count pass adds the instances dropped
    record_count[3] = 0;
    record_count[k_count_refs_wanted] = cached_wanted;
  }
  KeepTables* keep = keep_tables();
  const FrameEye eye = keep->eye;
  const uint64_t bounds = keep->mesh_bounds;
  uint* keys = (uint*)keep->keys;
  Atomic<uint>* histograms = (Atomic<uint>*)keep->histograms;
  InstanceDesc* instances = (InstanceDesc*)g_params.instances;
  const uint begin = group * k_ranges_block + t * k_ranges_each;
  for (uint k = 0; k < k_ranges_each; ++k) {
    const uint i = begin + k;
    if (i >= n) break;
    const uint d = drawn[i];
    const uint c = casters[i];
    const uint dm = d & k_count_mask;
    const uint cm = c & k_count_mask;
    if ((dm | cm) == 0) continue;
    const uint kind = kCache != 0 && (d & k_count_cached) != 0 ? 1u : 0u;
    const InstanceDesc instance = instances[i];
    const float4 sphere =
        bounds != 0 ? ((float4*)bounds)[instance.mesh] : float4(0.0, 0.0, 0.0, 0.0);
    const uint2 key = keep_key(instance, eye, sphere);
    keys[2 * i] = key.x;
    keys[2 * i + 1] = key.y;
    if (dm != 0) histograms[kind * k_keep_bins + (keep_item_key(key, false) >> 16)].add(dm);
    if (cm != 0) histograms[kind * k_keep_bins + (keep_item_key(key, true) >> 16)].add(cm);
  }
}

// `keep_cut_main` (level 1) and `keep_fine_main` (level 2), one group of 256 threads, 256 bins a
// thread: each capacity's crossing found from the bins' sums, which no thread order changes.
void keep_cut(uint t, uint level) {
  if ((drop_words()[k_drop_state] & k_drop_over) == 0) return;
  KeepTables* keep = keep_tables();
  uint* histograms = (uint*)keep->histograms;
  const uint per = k_keep_bins / k_ranges_threads;
  for (uint kind = 0; kind < 2; ++kind) {
    uint* cut = keep_cut_words(kind);
    // The same for every thread of the group, so the barriers below are met by all of them.
    if (level == 2 && cut[k_cut_crosses] == 0) continue;
    const uint capacity = level == 1 ? keep_capacity(kind) : keep_capacity(kind) - cut[k_cut_before];
    uint* bins = histograms + ((level == 1 ? 0u : 2u) + kind) * k_keep_bins;
    uint own = 0;
    for (uint j = 0; j < per; ++j) own += bins[t * per + j];
    const uint before = group_exclusive(t, uint2(own, 0u)).x;
    const uint total = group_sum(t, uint4(own, 0u, 0u, 0u)).x;
    if (level == 1 && t == 0 && total <= capacity) {
      cut[k_cut_crosses] = 0;
      cut[k_cut_room] = capacity - total;
    }
    if (before <= capacity && capacity < before + own) {  // the one thread whose bins cross
      uint at = before;
      for (uint j = 0; j < per; ++j) {
        const uint b = t * per + j;
        const uint v = bins[b];
        if (at + v > capacity) {
          if (level == 1) {
            cut[k_cut_crosses] = 1;
            cut[k_cut_bin] = b;
            cut[k_cut_before] = at;
          } else {
            cut[k_cut_key] = (cut[k_cut_bin] << 16) | b;
            cut[k_cut_room] = keep_capacity(kind) - (cut[k_cut_before] + at);
          }
          break;
        }
        at += v;
      }
    }
  }
}

// `keep_ties_main`, the instance blocks: the items in a crossing level-1 bin, added at `h` to their
// capacity's level-2 histogram. A frame's plain variant never marks a count cached, so one entry
// point serves both.
void keep_ties(uint group, uint t) {
  if ((drop_words()[k_drop_state] & k_drop_over) == 0) return;
  uint* cut0 = keep_cut_words(0);
  uint* cut1 = keep_cut_words(1);
  const bool crosses[2] = {cut0[k_cut_crosses] != 0, cut1[k_cut_crosses] != 0};
  if (!crosses[0] && !crosses[1]) return;
  const uint bin[2] = {cut0[k_cut_bin], cut1[k_cut_bin]};
  KeepTables* keep = keep_tables();
  uint* keys = (uint*)keep->keys;
  Atomic<uint>* histograms = (Atomic<uint>*)keep->histograms;
  uint* drawn = drawn_counts();
  uint* casters = caster_counts();
  const uint n = g_params.instance_count;
  const uint begin = group * k_ranges_block + t * k_ranges_each;
  for (uint k = 0; k < k_ranges_each; ++k) {
    const uint i = begin + k;
    if (i >= n) break;
    const uint d = drawn[i];
    const uint dm = d & k_count_mask;
    const uint cm = casters[i] & k_count_mask;
    if ((dm | cm) == 0) continue;
    const uint kind = (d & k_count_cached) != 0 ? 1u : 0u;
    if (!crosses[kind]) continue;
    const uint2 key = uint2(keys[2 * i], keys[2 * i + 1]);
    const uint fine = (2 + kind) * k_keep_bins;
    const uint dk = keep_item_key(key, false);
    const uint ck = keep_item_key(key, true);
    if (dm != 0 && (dk >> 16) == bin[kind]) histograms[fine + (dk & 0xffffu)].add(dm);
    if (cm != 0 && (ck >> 16) == bin[kind]) histograms[fine + (ck & 0xffffu)].add(cm);
  }
}

// What each block of instances kept: an instance whose drawn item is keyed at or past its
// capacity's `K` loses everything (counted in word 2), one whose casters alone are loses them
// (word 3).
void ranges_kept_count<let kCache : int>(uint group, uint t) {
  const uint state = drop_words()[k_drop_state];
  if ((state & k_drop_over) == 0) return;
  uint* drawn = drawn_counts();
  uint* casters = caster_counts();
  uint* keys = (uint*)keep_tables()->keys;
  const uint n = g_params.instance_count;
  const uint begin = group * k_ranges_block + t * k_ranges_each;
  uint own = 0;
  uint own_cached = 0;
  uint dropped_drawn = 0;
  uint dropped_casters = 0;
  for (uint k = 0; k < k_ranges_each; ++k) {
    const uint i = begin + k;
    if (i >= n) break;
    const uint d = drawn[i];
    uint c = casters[i];
    const uint dm = d & k_count_mask;
    const uint cm = c & k_count_mask;
    if ((dm | cm) == 0) continue;
    const bool cached = kCache != 0 && ((d | c) & k_count_cached) != 0;
    const uint kind = cached ? 1u : 0u;
    const uint2 key = uint2(keys[2 * i], keys[2 * i + 1]);
    const bool keep_drawn = dm == 0 || keep_kept(kind, keep_item_key(key, false));
    const bool keep_casters = cm == 0 || keep_kept(kind, keep_item_key(key, true));
    if (!keep_drawn) {
      drawn[i] = 0;
      casters[i] = 0;
      ++dropped_drawn;
      continue;
    }
    if (!keep_casters) {
      c &= k_count_cached;
      casters[i] = c;
      ++dropped_casters;
    }
    const uint count = dm + (c & k_count_mask);
    if (cached) {
      own_cached += count;
    } else {
      own += count;
    }
  }
  const uint4 sums = group_sum(t, uint4(own, own_cached, dropped_drawn, dropped_casters));
  if (t == 0 && group < ranges_blocks()) {
    uint* kept = kept_sums();
    kept[2 * group] = sums.x;
    kept[2 * group + 1] = sums.y;
    Atomic<uint>* record_count = (Atomic<uint>*)g_params.record_count;
    if (sums.z != 0) record_count[2].add(sums.z);
    if (sums.w != 0) record_count[3].add(sums.w);
  }
}

// The prefix sums of what was kept, written as the fitting frame's are.
void ranges_kept<let kCache : int>(uint group, uint t) {
  const uint state = drop_words()[k_drop_state];
  if ((state & k_drop_over) == 0) return;
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
void keep_cut_main(uint3 group_thread : SV_GroupThreadID) { keep_cut(group_thread.x, 1u); }

[shader("compute")]
[numthreads(256, 1, 1)]
void keep_ties_main(uint3 group : SV_GroupID, uint3 group_thread : SV_GroupThreadID) {
  keep_ties(group.x, group_thread.x);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void keep_fine_main(uint3 group_thread : SV_GroupThreadID) { keep_cut(group_thread.x, 2u); }

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
  // A fourth, since R120: drawn alpha-tested inside a fade band, the second form without its
  // micromap (`pair_fading`).
  const uint form =
      caster ? 2u : (masked ? (pair_fading(instance, mesh, cluster_id) ? 3u : 1u) : 0u);
  const uint local = cluster_id - mesh.first_cluster;
  uint* bases = (uint*)tables->bases;
  const uint key = (bases[instance.mesh] + local) * k_cache_forms + form;
  Atomic<uint>* keys = (Atomic<uint>*)tables->keys;
  const uint value = keys[key].load();
  uint64_t* references = (uint64_t*)tables->references;
  // A slot. A key the frame built in its own set last frame (`k_cache_frame`, R114), or that
  // nothing had room for, is missing again, like an empty one.
  if (value < k_cache_frame) {
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
  // The dispatches grow folded (T165; dispatch.slang): a frame past 65,535 workgroups of them
  // takes the next row, never more on x.
  if (pending % k_cache_workgroup == 0) {
    folded_append(args + k_args_references, pending / k_cache_workgroup);
  }
  if (value == k_cache_claim) return;  // another entry of this frame claimed it
  if (keys[key].compareExchange(value, k_cache_claim) != value) return;
  Atomic<uint>* counts = (Atomic<uint>*)g_params.record_count;
  const uint index = counts[k_count_misses].add(1);
  if (index == 0) {
    // The frame's first miss: the slot scan and the eviction walk the slots.
    const uint slot_groups = (tables->slot_count + k_cache_workgroup - 1) / k_cache_workgroup;
    const uint3 folded = folded_args(slot_groups);
    args[k_args_scan].store(folded.x);
    args[k_args_scan + 1].store(folded.y);
    args[k_args_evict].store(folded.x);
    args[k_args_evict + 1].store(folded.y);
  }
  // Every miss is listed, past the slots too (R114): `cache_assign_main` gives the ones the cache
  // has no slot for a record in the frame's own set. A miss is a pending reference's key claimed
  // once, so the list, as long as the references, always has room; the guard is for its bound.
  if (index >= tables->reference_capacity) {
    keys[key].store(k_cache_overflow);  // counted by `cache_assign_main`, from word 6
    return;
  }
  if (index % k_cache_workgroup == 0) folded_append(args + k_args_assign, index / k_cache_workgroup);
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
  const bool not_caster = g_params.caster_count == 0 || visible_index < caster_base();
  const bool masked = pair_masked(masks, instance, cluster_id);
  const bool opaque = not_caster && !masked;
  record.geometry = (geometry_index & 0xffffff) | ((opaque ? k_geometry_opaque : 0u) << 29);
  record.strides0 = 1 | (12 << 16);
  record.strides1 = 0;
  record.index_buffer = g_params.indices8 + uint64_t(cluster.triangle_offset) * 3;
  record.vertex_buffer = vertex_address;
  record.geometry_buffer = 0;
  record.omm_array = 0;
  record.omm_indices = 0;
  if (not_caster && masked && !pair_fading(instance, mesh, cluster_id)) {
    attach_micromap(record, cluster_id);
  }
  ClusterRecord* records = (ClusterRecord*)g_params.records;
  records[dense] = record;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void emit_main(uint3 thread_id : SV_DispatchThreadID) {
  emit<0>(folded_thread(thread_id, 64));  // a folded dispatch (T160), as records_main's
}

[shader("compute")]
[numthreads(64, 1, 1)]
void emit_cached_main(uint3 thread_id : SV_DispatchThreadID) {
  emit<1>(folded_thread(thread_id, 64));
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
  const uint slot = folded_thread(thread_id, 64);  // grown folded by the emit pass (T165)
  if (slot == 0) {
    // The frame's own records as the ranges kept them, for `cache_assign_main` to append the misses
    // without a slot after (R114) while its thread 0 raises the count the build reads.
    uint* words = (uint*)tables->scratch;
    words[k_scratch_built] = ((uint*)g_params.record_count)[0];
    return;
  }
  if (slot >= tables->slot_count) return;
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
  const uint slot = folded_thread(thread_id, 64);  // grown folded by the emit pass (T165)
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

// A miss's build record, as the emit pass writes a rigid entry's — from the rest-pose float
// positions — but named by the cluster within its mesh (`miss.z`) rather than by a visible entry:
// the same record whether it is built into a slot or into the frame's own set.
ClusterRecord cache_record(uint4 miss) {
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
  // The drawn alpha-tested form (key % 3 == 1) carries the cluster's micromap; the opaque and the
  // caster forms never do (R120).
  if (miss.x % k_cache_forms == 1u) attach_micromap(record, miss.y);
  return record;
}

// Every miss its slot, in miss order, and its build record and destination. **A miss past the free
// slots** — only when this frame's own new clusters outnumber every slot it did not draw — **is
// built in the frame's own set** (R114): its record appended to the frame's records after the
// ones the ranges kept, while the set has room (the frame's capacity), and its key marked with that
// record's index for `cache_references_main`, which runs after the frame's build. Thread 0 raises
// the frame's count, which that build reads, and its wanted, which the capacity policy reads, by
// those misses, so a frame that keeps overflowing grows its own set to hold them; only a miss the
// set has no room for either takes the inactive structure, and draws nothing that frame. Thread 0
// also writes the count the explicit build reads and how many slots are occupied after it, which
// the scratch keeps for the frames with no miss (`ranges_count_cached_main` copies it into theirs).
//
// Which misses take the slots follows the atomics, as before; it changes which structures are
// built where and never what they are, so the picture is the same whichever it is.
[shader("compute")]
[numthreads(64, 1, 1)]
void cache_assign_main(uint3 thread_id : SV_DispatchThreadID) {
  CacheTables* tables = cache_tables();
  uint* record_count = (uint*)g_params.record_count;
  uint* scratch = (uint*)tables->scratch;
  const uint claimed = record_count[k_count_misses];
  const uint misses = min(claimed, tables->reference_capacity);
  const uint free = scratch[0];
  const uint assigned = min(misses, free);
  // The frame's own records as the ranges kept them (the slot scan's copy: word 0 is this pass's
  // thread 0's to raise), and what its set holds past them.
  const uint built = scratch[k_scratch_built];
  const uint room = g_params.capacity > built ? g_params.capacity - built : 0u;
  const uint i = folded_thread(thread_id, 64);  // grown folded by the emit pass (T165)
  if (i == 0) {
    const uint occupied = tables->slot_count - 1 - free + assigned;
    record_count[k_count_miss_built] = assigned;
    record_count[k_count_occupied] = occupied;
    scratch[k_scratch_occupied] = occupied;
    const uint placed = min(misses - assigned, room);
    record_count[k_count_miss_overflow] = claimed - assigned;
    record_count[k_count_miss_unbuilt] = claimed - assigned - placed;
    record_count[0] = built + placed;
    record_count[1] += claimed - assigned;
  }
  if (i >= misses) return;
  uint4* list = (uint4*)tables->misses;
  const uint4 miss = list[i];
  Atomic<uint>* keys = (Atomic<uint>*)tables->keys;
  if (i >= free) {
    const uint own = i - free;
    if (own >= room) {
      keys[miss.x].store(k_cache_overflow);  // neither had room: the inactive structure
      return;
    }
    ClusterRecord* records = (ClusterRecord*)g_params.records;
    records[built + own] = cache_record(miss);
    keys[miss.x].store(k_cache_frame | (built + own));
    return;
  }
  uint* free_list = (uint*)tables->free_list;
  const uint slot = free_list[i];
  uint* owners = (uint*)tables->owners;
  uint* stamps = (uint*)tables->stamps;
  owners[slot] = miss.x;
  stamps[slot] = g_params.frame;
  keys[miss.x].store(slot);
  ClusterRecord* records = (ClusterRecord*)tables->miss_records;
  records[i] = cache_record(miss);
  uint64_t* destinations = (uint64_t*)tables->miss_destinations;
  destinations[i] = tables->slot_base + uint64_t(slot) * tables->slot_stride;
}

// Every pending reference's address — its key, parked in the reference by the emit pass, now has
// its slot; or, for a miss the frame built in its own set (R114), the address that set's build
// wrote, which is why this pass runs after it; or the inactive structure for a key nothing had
// room for. A resident key's references were addressed by the emit pass itself.
[shader("compute")]
[numthreads(64, 1, 1)]
void cache_references_main(uint3 thread_id : SV_DispatchThreadID) {
  CacheTables* tables = cache_tables();
  uint* args = (uint*)tables->args;
  const uint p = folded_thread(thread_id, 64);  // grown folded by the emit pass (T165)
  if (p >= args[k_args_pending]) return;
  uint* reference_keys = (uint*)tables->reference_keys;
  const uint dense = reference_keys[p];
  uint64_t* references = (uint64_t*)tables->references;
  uint* keys = (uint*)tables->keys;
  const uint value = keys[uint(references[dense])];
  uint64_t address = tables->slot_base;  // slot 0
  if (value < k_cache_frame) {
    address = tables->slot_base + uint64_t(value) * tables->slot_stride;
  } else if (value < k_cache_overflow) {
    address = ((uint64_t*)g_params.clas_addresses)[value - k_cache_frame];
  }
  references[dense] = address;
}
