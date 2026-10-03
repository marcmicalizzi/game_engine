#pragma once

// The GPU-resident scene (docs/plan/04-renderer.md §4.2) and the passes that consume it: the
// cluster culling and LOD selection pass (shaders/cluster_cull.slang), the two rasterizers'
// shared push block, the visibility resolve, and the Hi-Z pyramid used by two-pass occlusion
// culling (shaders/hiz_build.slang). The RHI stays ignorant of the geometry module: the passes
// read cluster and LOD descriptors through device addresses whose layouts geometry pins with
// size tables, and these structs only carry cameras, frustums, transforms, and addresses.
//
// **The scene is instances of meshes.** Every mesh of a scene lives in one set of global buffers
// (clusters, LOD, triangles, attributes, quantized positions) with the offsets baked into each
// `geometry::ClusterDesc`; a `MeshDesc` says which range of clusters a mesh owns and which
// 16-bit grid its positions are on, and an `InstanceDesc` places one mesh in the world. The unit
// of culling is the **pair** (instance, cluster): instance i owns `pair_count` of them starting
// at `InstanceDesc::first_pair`, a prefix sum over the instances in order, so the cull dispatch
// covers `CullParams::pair_count` threads and thread t finds its instance by binary search.
// Survivors append a `uint2 {instance, cluster}` to the visible list, and the rasterizers read
// `visible[i]` to get the instance and the cluster. The visibility buffer's id is not the entry but
// the scene's pair, `pair << 8 | triangle` (docs/subsystems/gfx.md, "The tie rule"), and the
// resolve gets from a pair back to its entry through `CullParams::pair_entries`.
//
//     CullParams params{};
//     set_frustum(params, frustum_from_view_proj(view_proj));
//     params.view_proj = view_proj;
//     params.camera = Vec4(eye, znear);
//     params.lod = Vec4(proj_scale, threshold_px, 1.0f, 1.0f);
//     params.cone_cull = 1;  // unless the mesh is two-sided
//     params.instances = instances.address;  // InstanceDesc[instance_count]
//     params.meshes = meshes.address;        // MeshDesc[]
//     params.instance_count = n; params.pair_count = total;
//     ...write to a host-visible buffer, push its address, dispatch cull_group_count(pairs)...
//     commands.draw_mesh_tasks_indirect(draw_args.buffer, 0, 1, sizeof(u32) * 3);
//
// Before the dispatch, `draw_args` (and `sw_args`) must hold {0, 1, 1}: the pass counts
// survivors into x. For occlusion culling, `hiz` points at a pyramid laid out by hiz_layout(),
// `pass` is 1 then 2 within a frame, and `flags`/`prev_flags` ping-pong between frames; both are
// indexed by pair, so both are `pair_count` words long.
//
// **With more than one view** (a `renderer::ViewSet`, docs/plan/04-renderer.md §4.6) the pass runs
// once per view with that view's frustum, Hi-Z and LOD threshold, and every one of these buffers
// carries a slice per view: the visible list is laid out **run-major** — run r of view v starts at
// entry `(r * views + v) * pair_count` — the argument blocks are `k_draw_args_bytes` apart, and
// `flags`/`prev_flags` point at the view's own `pair_count` words. Run-major is what makes the
// first run of every view one contiguous range, which is the range the ray tracing chain builds
// its union from; for a single view the layout is the same three runs it has always been.

#include <core/base/types.h>
#include <core/math/math.h>

#include <cmath>

namespace engine::gfx {

inline constexpr u32 k_hiz_max_mips = 16;  // enough for 32768 x 32768

// **Geometry streaming** (docs/plan/04-renderer.md §4.3 step 3 and §4.9, geometry/cluster_pages.h).
// The addresses the cull pass needs to run `geometry::select_lod_streaming`'s rule on the GPU and
// to tell the CPU what it could not draw. Everything here is scene-sized or page-sized; none of it
// is screen-sized, so one block serves every view of a frame.
//
// `CullParams::streaming` is null for a scene that is uploaded whole, and the pass then runs
// exactly the instructions it ran before streaming existed — which is what keeps every picture of
// a non-streamed scene byte-identical.
//
// The three feedback arrays are the GPU half of §4.9's request queue. `requests` is a bounded
// array of `geometry::PageRequest` (12 bytes: page, screen pixels, distance) appended under the
// atomic in `request_count`; `request_mask` deduplicates, one word per page, so a page a thousand
// clusters want costs one entry and one atomic exchange rather than a thousand; and `used` is the
// "the cut read this page this frame" bit that makes the CPU manager's LRU mean something. All
// three are cleared by the frame's reset pass and copied back to the host one frame slot later,
// so nothing in the frame path ever waits on the device.
struct StreamParams {
  u64 pages = 0;            // geometry::ClusterPageDesc[page_count]
  u64 page_of_cluster = 0;  // u32 per cluster of the scene
  u64 children = 0;         // geometry::ClusterChildren (two u32) per cluster
  u64 residency = 0;        // u32 per page: non-zero while the page's payload is in a pool slot
  u64 used = 0;             // u32 per page, written by the cut: the LRU's "touched this frame"
  u64 requests = 0;         // geometry::PageRequest[CullParams::max_requests]
  u64 request_count = 0;    // u32: the atomic the requests are appended under
  u64 request_mask = 0;     // u32 per page: one request per page per frame
};
static_assert(sizeof(StreamParams) == 64);

// One indirect argument block, and the stride between two views' blocks in one buffer. Four u32
// covers both shapes the cull pass fills — `{groups, 1, 1}` for `draw_mesh_tasks_indirect` and
// `{vertex_count, instance_count, 0, 0}` for `draw_indirect` — and keeps every view's block
// 16-byte aligned. `clas_records.slang` mirrors the number, because it reads each view's count
// word out of one buffer (docs/plan/04-renderer.md §4.6).
inline constexpr u32 k_draw_args_bytes = 16;

// The run of the visible list the shadow casters go in (`CullParams::casters`): the software
// rasterizer's, because a frame that builds ray tracing geometry never fills it — the chain needs
// `hw`, `vertex` or `rt`, and none of them splits clusters off to the software rasterizer. Run r of
// view v starts at entry `(r * views + v) * pair_count`, so the casters of view v start at
// `(k_caster_run * views + v) * pair_count` and their visible indices never collide with a drawn
// entry's. `clas_records.slang` mirrors the number, because it builds that run beside run 0.
inline constexpr u32 k_caster_run = 2;

// Mirrors CullParams in cluster_cull.slang. 464 bytes.
struct CullParams {
  Vec4 planes[6];         // inward-facing, normalized
  Vec4 camera;            // xyz position, w = znear
  Vec4 lod;               // x = proj_scale (cot(fov_y/2) * viewport_height / 2), y = threshold_px,
                          // z = LOD selection enabled, w = frustum culling enabled
  Vec4 raster;            // x = projected cluster diameter (px) below which a cluster is software
                          // rasterized, y = mode (k_raster_*), z and w unused
  u32 cluster_count = 0;  // the global cluster array; the dispatch covers pair_count instead
  u32 plane_count = 0;
  u32 count_index = 0;  // which u32 of draw_args counts hardware survivors: 0 for mesh-task
                        // groups {count, 1, 1}, 1 for draw_indirect {verts, count, 0, 0}
  u32 cone_cull = 0;    // 1: backface-cull clusters by their normal cone (ClusterDesc::cone), on
                        // rigid instances of uniform scale only (InstanceDesc)
  u64 clusters = 0;     // geometry::ClusterDesc[]
  u64 lods = 0;         // geometry::ClusterLodDesc[]
  u64 visible = 0;      // u32x2[]: {instance, cluster} of the hardware survivors. Every draw of a
                        // frame appends to its own run of one list, so this is the run's address
                        // and the draw's ClusterDrawParams::visible_offset is the run's index.
  u64 draw_args = 0;    // u32[3] = {survivors, 1, 1} for draw_mesh_tasks_indirect
  u64 sw_visible = 0;   // u32x2[]: the software-rasterized survivors, in their own run
  u64 sw_args = 0;      // u32[3] = {survivors, 1, 1} for dispatch_indirect
  // Occlusion culling; hiz == 0 disables it.
  Mat4 view_proj;
  u64 hiz = 0;         // f32[] pyramid from hiz_layout()
  u64 prev_flags = 0;  // u32[pair_count]: drawn last frame
  u64 flags = 0;       // u32[pair_count]: drawn this frame (cleared before pass 1)
  u32 hiz_width = 0;
  u32 hiz_height = 0;
  u32 hiz_mips = 0;
  u32 pass = 0;  // 0 single pass, 1 last frame's visible set, 2 the rest
  u32 hiz_offsets[k_hiz_max_mips] = {};
  // The scene.
  u64 instances = 0;  // InstanceDesc[instance_count], in order of first_pair
  u64 meshes = 0;     // MeshDesc[], indexed by InstanceDesc::mesh
  u32 instance_count = 0;
  u32 pair_count = 0;  // the prefix sum: one thread per (instance, cluster) pair
  // Streaming; 0 runs the pass exactly as it ran before pages existed.
  u64 streaming = 0;     // StreamParams*
  u32 page_count = 0;    // the scene's page table, which bounds every page index below
  u32 max_requests = 0;  // the request buffer's capacity; the atomic is clamped to it
  // **Shadow casters** (docs/subsystems/geometry.md, "Normal cones", and renderer.md, "Shadows").
  // A pair the cone test alone rejected faces away from the camera, not from the light, so with
  // `casters` set it is not dropped: it goes on through the frustum's, the cut's and the page
  // table's tests like any other pair, and a survivor is appended here instead of to `visible`.
  // Nothing draws this run; the ray tracing chain builds it into the frame's acceleration
  // structures beside the drawn one, so a shadow is cast by the geometry at the picture's own LOD
  // whichever way it faces. 0 drops such a pair, which is what the pass always did.
  u64 casters = 0;       // u32x2[]: {instance, cluster}, a run of its own
  u64 caster_count = 0;  // u32: the atomic the run is appended under
  // **The vertex path's indexed draw** (`VertexDrawHeader`, below): this run's header, which the
  // `cull_vertex_main` entry point allocates each hardware survivor's triangles under, and its
  // records, where it writes the survivor's `VertexDrawRecord`. Only that entry point reads them;
  // `cull_main` never does.
  u64 vertex_draw = 0;     // VertexDrawHeader
  u64 vertex_records = 0;  // VertexDrawRecord[pair_count]
  // **The way back from a pixel to the entry** (docs/subsystems/gfx.md, "The tie rule"). The
  // visibility id carries the scene's pair, not the visible entry, so that a depth tie is settled
  // by the scene's order rather than by which thread reached the append first; the resolve then
  // needs the entry the pair was drawn as this frame, for its {instance, cluster} and a deformed
  // instance's pool block. The cull pass writes it here, one word per pair of this view, for every
  // pair it draws: `visible_base + slot` for the hardware run and `sw_visible_base + slot` for the
  // software one, the entry's index in the whole list. 0 writes nothing.
  u64 pair_entries = 0;     // u32[pair_count], this view's
  u32 visible_base = 0;     // the whole list's index of `visible[0]`
  u32 sw_visible_base = 0;  // and of `sw_visible[0]`
  // **Terrain levels** (`TerrainLevelDesc`): the frame's table, which an instance with
  // `InstanceDesc::terrain` names. A cluster of a level whose sphere lies wholly inside the level's
  // `hole` is dropped — the next finer level draws that ground. 0 tests nothing.
  u64 terrain = 0;
  u64 pad_terrain = 0;
};
static_assert(sizeof(CullParams) == 480);
static_assert(sizeof(CullParams) % 16 == 0, "the block is read as float4 rows on the GPU");

inline constexpr f32 k_raster_hardware = 0.0f;  // CullParams::raster.y
inline constexpr f32 k_raster_software = 1.0f;
inline constexpr f32 k_raster_split = 2.0f;

// ---- the vertex path's indexed draw (cluster_vertex_indexed.slang, docs/subsystems/gfx.md) ----
//
// The baseline tier draws one hardware run of the visible list with **one indexed draw of exactly
// the triangles the run's clusters hold**, and an index is `slot << 8 | local vertex`, so the
// post-transform vertex cache shares a cluster's vertices between its triangles. Measured on the
// TITAN Xp (docs/experiments/e1-pascal-rerun.md, "After"): the pass is bound by the vertex stage's
// invocations, and the draw it replaced ran three per triangle corner of every cluster's capacity
// — the ones past a cluster's count turned out nearly free, the three per corner did not.
//
// Each (view, hardware run) owns one slot of three buffers, run-major like the visible list: a
// `VertexDrawHeader`, one `VertexDrawRecord` per pair (a survivor's slot indexes it), and an index
// array of three u32 per triangle of `index_capacity`. Three buffers and not one region because
// the draw reads each a different way — arguments, storage, indices — and the render graph orders
// a pass against a buffer by the one access it declares. The frame's reset writes the header;
// `cull_vertex_main` allocates a survivor's triangles under `cursor` and writes its record;
// `expand_main` (vertex_expand.slang) writes the indices, one workgroup per survivor, and finishes
// `index_count`; the draw reads the header's first twenty bytes as a `DrawIndexedIndirectArgs`
// (rhi.h) and the array as its index buffer.
//
// **The index array is a budget, and running out of it draws the same picture more slowly.** A
// survivor whose triangles do not fit is flagged in its record and drawn by the fallback — the
// capacity draw, 3 x `triangles_per_cluster` vertices per cluster with the ones past its count
// collapsed, instanced over the slots up to the highest one that overflowed — so nothing is ever
// missing. Allocation is in atomic order, so the survivors that fit are exactly those allocated
// before the first that did not, and `fit_end` (that one's start) is where the indexed draw stops.
struct VertexDrawHeader {
  // DrawIndexedIndirectArgs, finished by `expand_main` from the two words after it.
  u32 index_count = 0;  // 3 x min(cursor, fit_end)
  u32 instance_count = 1;
  u32 first_index = 0;
  i32 vertex_offset = 0;
  u32 first_instance = 0;
  u32 cursor = 0;          // triangles the cull pass allocated, whether or not they fit
  u32 fit_end = 0;         // atomic min over the overflowing survivors' starts; reset to capacity
  u32 index_capacity = 0;  // triangles the run's index array holds
  // DrawIndirectArgs of the fallback: {3 x triangles_per_cluster, 1 + the highest slot that
  // overflowed (0 when none did), 0, 0}.
  u32 fallback_vertex_count = 0;
  u32 fallback_instance_count = 0;
  u32 fallback_first_vertex = 0;
  u32 fallback_first_instance = 0;
  // DispatchIndirectArgs of `expand_main`: one workgroup per survivor.
  u32 expand_groups = 0;
  u32 expand_y = 1;
  u32 expand_z = 1;
  u32 overflow = 0;  // survivors drawn by the fallback
};
static_assert(sizeof(VertexDrawHeader) == 64);

// One per pair slot of a run, written by `cull_vertex_main` at the survivor's slot. Everything the
// draw needs of an entry is here, so its vertex stage never reads the visible list.
struct VertexDrawRecord {
  u32 instance = 0;
  u32 cluster = 0;
  u32 first_triangle = 0;  // where its triangles start in the run's index array
  u32 overflow = 0;        // 1: it did not fit, and the fallback draws it
};
static_assert(sizeof(VertexDrawRecord) == 16);

inline constexpr u64 k_vertex_draw_fallback_offset = 32;  // the fallback's DrawIndirectArgs
inline constexpr u64 k_vertex_draw_expand_offset = 48;    // expand_main's DispatchIndirectArgs
inline constexpr u32 k_vertex_draw_index_bytes = 3 * sizeof(u32);  // one triangle of the array

// The header a frame starts a run from: nothing allocated, the whole array free.
inline VertexDrawHeader vertex_draw_reset(u32 index_capacity, u32 triangles_per_cluster) noexcept {
  VertexDrawHeader h;
  h.fit_end = index_capacity;
  h.index_capacity = index_capacity;
  h.fallback_vertex_count = triangles_per_cluster * 3;
  return h;
}

// Mirrors ExpandParams in vertex_expand.slang: the push constants of `expand_main`, dispatched
// indirectly from the header's own `expand_groups`, so workgroup g expands slot g. 40 bytes.
struct VertexExpandParams {
  u64 header = 0;     // VertexDrawHeader of the run
  u64 records = 0;    // VertexDrawRecord[pair_count]
  u64 indices = 0;    // u32[3 * index_capacity], the index buffer
  u64 clusters = 0;   // geometry::ClusterDesc[]
  u64 triangles = 0;  // the packed triangles: three local indices in a u32
};
static_assert(sizeof(VertexExpandParams) == 40);

inline constexpr u32 k_expand_workgroup_size = 64;  // numthreads of expand_main

// Per-instance deformation (docs/plan/04-renderer.md §4.3, ADR-0026 decision 7). An instance
// whose `InstanceDesc::deform` is not `k_invalid_deform` is **deformed**: every position read for
// it — by the three rasterizers, by the resolve's reconstruction, and by the cluster
// acceleration structure records — comes out of the frame's deformed-vertex pool instead of its
// mesh's 16-bit grid.
//
// **Where in the pool is no longer a property of the instance.** E25 gave each deformed instance
// a block as long as its mesh's whole cluster-ordered vertex range, so `pool_offset` plus the
// scene-wide vertex index was the slot — and the pool was then sized by the *population* rather
// than by the frame (1.7 GB for 1,024 FlightHelmets). The pool is now suballocated per frame from
// a budget, one block per **visible entry**, so the address depends on which entry is being drawn
// and lives in `MeshDesc::deform_slots` rather than here. What is left on the instance is what is
// genuinely per instance: which deformer, and a skinned one's own bone matrices, which are per
// *instance* and not per mesh because two characters share one mesh, one binding stream and one
// skeleton and have entirely different poses.
//
// **A chain of stages, not one exclusive mode** (docs/plan/04-renderer.md §4.3, gfx.md "The
// deform chain"). `stages` is a mask, and the pass runs the ones it names **in a fixed order**:
//
//   1 static shape   a set of morph channel weights that changes rarely — a character's body,
//                    baked once from a parametric rig. Its result is cached in a persistent
//                    per-instance buffer (`cache`), so the per-frame stages start from there and
//                    it costs nothing until the weights change.
//   2 pose morphs    per-frame morph weights: expressions, correctives.
//   3 skinning       linear blend skinning, `joints` x the mesh's `geometry::SkinBinding` stream.
//   4 procedural     the E25 stand-ins for a cage solver: `wave` and `lattice`, whose kind is in
//                    `k_deform_kind_mask` of this same word.
//
// The order is not a preference; it is what the data means. Morph deltas are authored **in bind
// space**, against the rest mesh, so they have to be applied before the skin moves that mesh —
// applying a bind-space delta to a posed vertex would displace it along an axis that has already
// rotated. A cage acts on the **posed surface**, because that is what a soft-tissue solver sees.
// Static before pose is not forced by the data but by the cache: the static stage's result is the
// rest mesh every later frame starts from, and a stage that ran after it could not be cached with
// it. gfx.md carries the argument and the crack rule stage by stage.
//
// **48 bytes, up from 16.** What is here is what is per *instance*: which stages, where its bone
// matrices are, where its weights are, and where its static cache is. What is per *mesh* — the
// morph channel records, the per-cluster slice directory, the delta streams — is **not** here and
// not in `MeshDesc` either (which is full at 64 bytes): those are scene-wide arrays like the
// clusters and the triangles, so they ride in `DeformParams`, which only the pool pass reads.
struct DeformDesc {
  u32 stages = 0;         // k_deform_stage_* | the procedural kind in k_deform_kind_mask
  u32 joint_count = 0;    // the bone-matrix array's length; 0 leaves a skinned instance at rest
  u32 first_channel = 0;  // this instance's mesh's first entry of the scene's morph channel array
  u32 channel_count = 0;  // how many channels it owns, which is `weights`' width
  // This mesh's first vertex in the scene-wide vertex arrays. The static cache is one block per
  // *mesh* vertex, and a cluster's `vertex_offset` is scene-wide, so this is what turns the one
  // into the other. It is here rather than in `MeshDesc` because that record is full at 64 bytes
  // and this is read by one pass, not by every position read in the renderer.
  u32 first_vertex = 0;
  u32 pad = 0;
  u64 joints = 0;  // anim::JointMatrix[joint_count]: three float4 rows each, or 0
  // f32[2 * channel_count]: the **static** weights first, then the **pose** weights. One array
  // rather than two addresses because the two are the same shape, are written by the same host
  // code, and a second pointer would cost eight bytes on every deformed instance to save nothing.
  u64 weights = 0;
  // float3 per vertex of this instance's mesh — the static stage's result, kept between frames.
  // Zero means the instance has no cache slot (its mesh is bigger than the budget had room for,
  // or nothing static is playing), and the static stage then runs per frame over the cut, which
  // is the same answer for more work.
  u64 cache = 0;
};
static_assert(sizeof(DeformDesc) == 48);

// One vertex of a static-shape cache: the displaced position and its octahedral normal, in
// `geometry::encode_normal_oct`'s packing. The normal is here because a cache of positions alone
// would leave the static stage's morph scan running every frame for the normals, which is the
// cost the cache exists to remove.
struct DeformCacheVertex {
  Vec3 position{};
  u32 normal_oct = 0;
};
static_assert(sizeof(DeformCacheVertex) == 16);

// `MeshDesc::deform_slots[entry]`: the entry's block did not fit the frame's pool budget, so
// every reader falls back to the **rest pose** for it. Never garbage and never a crash — a
// character momentarily drawn at rest, counted in `DeformAlloc::overflow_entries`.
inline constexpr u32 k_no_pool_slot = ~u32{0};

// The frame's pool allocator, one record in a device buffer (`deform_alloc.slang`). The cursor is
// in **vertices**, not bytes, because that is the unit `deform_slots` holds and the unit the
// pool's twelve-byte stride multiplies. A frame resets it in its first allocation dispatch and
// the run's dispatches carry it forward, so the whole frame's cut shares one budget.
struct DeformAlloc {
  u32 cursor = 0;             // vertices handed out so far this frame
  u32 entries = 0;            // visible entries that got a block
  u32 overflow_entries = 0;   // entries that did not fit and draw their rest pose
  u32 overflow_vertices = 0;  // the vertices those entries would have needed
};
static_assert(sizeof(DeformAlloc) == 16);

inline constexpr u32 k_invalid_deform = ~u32{0};  // InstanceDesc::deform: the instance is rigid

// The **procedural** deformer of the fourth stage, in the low two bits of `DeformDesc::stages`.
// They kept their values from when they were the whole of the mode word, so a `--deform lattice`
// run means what it always did.
inline constexpr u32 k_deform_identity = 0;
inline constexpr u32 k_deform_wave = 1;     // sinusoidal displacement along the vertex normal
inline constexpr u32 k_deform_lattice = 2;  // 3x3x3 trilinear cage over the mesh's grid box
// 3 was `k_deform_skin` while the modes were exclusive; skinning is a **stage** now, so the value
// is retired rather than reused — a container or a saved scene that still holds it would name a
// procedural deformer that no longer exists, and naming nothing is better than naming the wrong
// one.
inline constexpr u32 k_deform_kind_mask = 3;

// The stages, in the order the pass runs them. The mask lives in the same `u32` as the kind
// above, from bit 8 up, so an instance that skins *and* runs a cage is one record and one pass —
// which is the whole point of the change (E25 left "skinning plus a cage deformer on one instance
// needs a second pass or a wider kind field" as the open question, and this is the wider field).
inline constexpr u32 k_deform_stage_static = 1u << 8;       // morph channels that change rarely
inline constexpr u32 k_deform_stage_pose = 1u << 9;         // morph channels that change per frame
inline constexpr u32 k_deform_stage_skin = 1u << 10;        // linear blend skinning
inline constexpr u32 k_deform_stage_procedural = 1u << 11;  // wave / lattice / identity
// Set for the one frame in which the static stage's cache has to be filled. The pass then writes
// `DeformDesc::cache` as well as the pool; every other frame reads the cache and skips the stage.
inline constexpr u32 k_deform_stage_rebuild_cache = 1u << 12;
// **A terrain level's heights** (docs/subsystems/renderer.md, "The dunes in time-lapse"): the
// vertex's height is read out of two evaluated height fields and blended, `a (1 - t) + b t`, and
// its normal is the central difference of the blended field. It is a stage of its own and never
// runs beside the others: a terrain level is a heightfield whose every vertex sits on one lattice,
// which is what lets its position be a function of the lattice point alone (`TerrainLevelDesc`).
inline constexpr u32 k_deform_stage_terrain = 1u << 13;
inline constexpr u32 k_deform_stage_mask = k_deform_stage_static | k_deform_stage_pose |
                                           k_deform_stage_skin | k_deform_stage_procedural |
                                           k_deform_stage_terrain;

// ---- terrain levels (docs/subsystems/renderer.md, "The dunes in time-lapse")
// ----------------------
//
// **One evaluated height field**: the dune field sampled at one game time on a window of a level's
// lattice — `nx * nz` heights in metres, rows of x in order of z, sample (i, j) at lattice point
// (i0 + i, j0 + j). A level's two fields may sit on different windows of the same lattice (a ring
// that re-centred between their evaluations), which is why each carries its own.
struct TerrainField {
  u64 heights = 0;  // f32[nx * nz], or 0: none
  i32 i0 = 0;       // the lattice point of sample (0, 0), along x
  i32 j0 = 0;       // and along z
  u32 nx = 0;
  u32 nz = 0;
};

// **A terrain level as a frame draws it**: its lattice, its two fields and the blend between them,
// how far its vertices may have left their rest heights, and the square the next finer level
// covers. One per level, written per frame into a host-visible table (`CullParams::terrain`,
// `DeformParams::terrain`) and named by `InstanceDesc::terrain`.
//
// The pool pass writes a level vertex at lattice point (i, j) — found from its rest position, which
// every copy of the vertex shares, so the stage keeps the crack rule of gfx.md's deform chain — at
// `origin + (i, j) * spacing` with the height `a (1 - blend) + b blend` and the normal of the
// blended field's central differences. A vertex strictly inside `hole` is moved to the hole's
// nearest edge, at the field's height there: the finer level draws that ground, and a cluster of
// this one that straddles the edge keeps only its part outside it. The cull pass drops a cluster
// whose sphere is wholly inside `hole`, so a level does not pay for the ground a finer one draws,
// and pads every sphere of the level by `padding` — the largest distance any of its vertices is
// from its rest height in either field — in place of `InstanceDesc::bounds_padding`, because it
// changes as the fields do and a per-frame table is where it can change without an upload.
//
// Everything is in the level's **mesh space**, which is the world: a terrain level's instance has
// the identity transform (renderer::GpuScene refuses anything else).
struct TerrainLevelDesc {
  TerrainField a;
  TerrainField b;
  Vec2 origin{};       // x, z of lattice point (0, 0), metres
  f32 spacing = 1.0f;  // metres between lattice points
  f32 blend = 0.0f;    // 0 draws `a`, 1 draws `b`
  f32 padding = 0.0f;  // metres: the cull's sphere padding for every cluster of the level
  // Metres a ring's skirt hangs below its border: a vertex whose rest normal points straight down
  // is a skirt's (renderer::GpuScene marks them so), and is written that far under the field.
  f32 skirt = 0.0f;
  // `k_terrain_level_named`: a vertex of this level whose rest normal is horizontal is **drawn
  // from the level it names** — the level at `round(atan2(n.z, n.x) / (pi / 8)) mod 16`, its
  // lattice point, height and normal — rather than from this one: a world tile's border along a
  // coarser tile, which both tiles then draw from one description (renderer.md, "The ground from
  // the world's tiles"; `terrain_level_normal`). A heightfield vertex's rest normal is free for it,
  // since the pool writes the normals the resolve reads, as a skirt's already is.
  u32 flags = 0;
  u32 pad = 0;
  Vec4 hole{};  // x0, z0, x1, z1, metres; none when x1 <= x0
};
inline constexpr u32 k_terrain_level_named = 1u << 0;
// The rest normal that names terrain level `level` (0..15) for `k_terrain_level_named`:
// horizontal, at `level` sixteenths of a turn from +x towards +z — eighths until the far levels
// (renderer.md, "Ground to the horizon") took the tile levels past eight; 22.5 degrees apart is
// still a hundred times the octahedral encoding's step. Far from any heightfield normal, whose y is
// above cos 60 degrees on any sand, and from a skirt's, which points down.
inline Vec3 terrain_level_normal(u32 level) noexcept {
  constexpr f32 k_sixteenth = 0.39269908169872415481f;
  const f32 a = static_cast<f32>(level & 15u) * k_sixteenth;
  return Vec3{std::cos(a), 0.0f, std::sin(a)};
}

// GPU-mirrored; keep in step with the MeshDesc struct in the shaders. 64 bytes, read through a
// device address. One per mesh of the scene: the 16-bit position grid this mesh's positions are
// on (`geometry::ClusterMeshPart`), the stream of three u16 per vertex that every mesh shares
// (`geometry::ClusterMesh::quantized`, indexed by the scene-wide vertex index and padded to an
// even count so the shaders' load_position may read the last triple as two 32-bit words), and
// the range of the global cluster array this mesh owns. Six bytes of position per vertex instead
// of twelve; the acceleration structure builders still read the float positions.
//
// The last four addresses are where a **deformed** instance's positions come from instead. Two
// of them (`deform_pool`, `deform_slots`) are the scene's, the same in every MeshDesc, and they
// ride here rather than in `ClusterDrawParams` because that push block is full at its 128-byte
// limit and every position read already holds the MeshDesc. `templates` and `skin` are the
// mesh's own.
//
// `deform_slots` replaced the `DeformDesc[]` address that used to sit here, at the same size:
// with the pool suballocated per frame, a position reader needs the **visible entry's** block
// base and not the instance's deformer, and only the pool pass reads a `DeformDesc` at all (it
// takes the table through its own push constant). One indirection fewer per position read, and
// the table it replaces was being read for one word that never changed.
//
// `skin` took the struct from 56 to 64 bytes, and it belongs on the mesh rather than on the
// `DeformDesc` because a skin binding is per *vertex* and the vertex streams are the mesh's: a
// crowd of a hundred characters built from one mesh shares one binding stream and has a hundred
// bone-matrix arrays, which is exactly the split between this field and `DeformDesc::joints`.
struct MeshDesc {
  Vec4 quant{};           // xyz grid origin, w grid step: this mesh's own grid
  u64 quantized = 0;      // u16[3 * vertex_count] of the whole scene, rounded up to an even count
  u32 first_cluster = 0;  // in the global cluster array
  u32 cluster_count = 0;
  u64 deform_pool = 0;   // f32[3 * pool_vertices]: the frame's pool; 0 when nothing deforms
  u64 deform_slots = 0;  // u32[] per entry of the visible list: its pool vertex base, or
                         // k_no_pool_slot; 0 when nothing deforms
  u64 templates = 0;     // u64[]: one cluster template address per global cluster index, or 0
  u64 skin = 0;          // geometry::SkinBinding[]: eight bytes per scene-wide vertex, or 0
};
static_assert(sizeof(MeshDesc) == 64);

// InstanceDesc::flags, bit 0: the world transform scales every axis alike, so a normal cone may
// be tested (rotating its axis keeps it a cone) and a normal only needs the rotation.
inline constexpr u32 k_instance_uniform_scale = 1u;

// GPU-mirrored; keep in step with the InstanceDesc struct in the shaders. 96 bytes, read through
// a device address. One per instance of the scene, in order of `first_pair`.
//
// `bounds_padding` took the first of the two padding words and is **zero for every instance that
// existed before skinning**, so the cull arithmetic reduces to exactly what it was (a float plus
// zero is exact) and the pictures are byte-identical. It is how a *deformed* instance stays
// conservatively culled: a skinned vertex leaves its rest position, so the cluster sphere the
// cull pass tests no longer contains it. The caller supplies a bound, in the instance's own mesh
// space, on how far any vertex of the mesh can move under the deformation it will play, and the
// cull pass adds `bounds_padding * scale_max` to every cluster sphere radius *and* to both LOD
// spheres. Adding it to all three keeps the DAG cut crack-free: a cluster's parent sphere is its
// children's own sphere, so one constant added to every sphere of an instance leaves exactly one
// cluster per DAG path passing the test, which is the property the cut rests on
// (docs/subsystems/renderer.md, "Skinned instances").
//
// A padded sphere still bounds a deformed cluster; its **normal cone** does not bound anything,
// because the cone was fit to the rest triangles and a turned joint turns them by more than any
// margin. So an instance whose `deform` is not `k_invalid_deform` is never cone-tested — the cone
// test needs a rigid instance as well as a uniform scale (docs/subsystems/geometry.md, "Normal
// cones", has the measurement behind choosing that over a per-bone cone).
struct InstanceDesc {
  Mat4 world;             // mesh space to world; the top three rows are used
  u32 mesh = 0;           // index into the MeshDesc array
  u32 material_base = 0;  // added to the cluster's material index in the resolve
  u32 first_pair = 0;     // prefix sum of the instances' mesh cluster counts, in order
  f32 scale_max = 1.0f;   // largest axis scale: radii and LOD errors multiply by it
  u32 flags = k_instance_uniform_scale;
  u32 deform = k_invalid_deform;  // entry of the DeformDesc table; k_invalid_deform: rigid
  f32 bounds_padding = 0.0f;  // mesh-space slack added to every sphere this instance is culled by
  // 1 + the terrain level this instance draws, in the frame's `TerrainLevelDesc` table; 0 for
  // everything that is not a terrain level. It took the record's last pad word.
  u32 terrain = 0;
};
static_assert(sizeof(InstanceDesc) == 96);

// Fills `world`, `scale_max`, and the uniform-scale flag from an affine transform. The scales are
// the lengths of the upper-left 3x3's columns; "uniform" means they agree to a part in 10^4,
// which is what lets the cone test and the cheap normal transform run. A scale that far from
// uniform turns a normal against the cone's axis by up to 1e-4 radians, which is what
// `geometry::k_cone_margin` (1e-3) is sized to absorb: loosen this and widen that.
inline void set_instance_transform(InstanceDesc& instance, const Mat4& world) noexcept {
  instance.world = world;
  const f32 sx = length(world.c[0].xyz());
  const f32 sy = length(world.c[1].xyz());
  const f32 sz = length(world.c[2].xyz());
  const f32 hi = sx > sy ? (sx > sz ? sx : sz) : (sy > sz ? sy : sz);
  const f32 lo = sx < sy ? (sx < sz ? sx : sz) : (sy < sz ? sy : sz);
  instance.scale_max = hi;
  instance.flags = hi - lo <= 1.0e-4f * hi ? k_instance_uniform_scale : 0u;
}

// Mirrors MeshParams in cluster_mesh.slang and RasterParams in cluster_sw_raster.slang: the push
// constants of the mesh-shader and software rasterization paths. 128 bytes, the largest push
// block the renderer allows.
struct ClusterDrawParams {
  Mat4 view_proj;
  u64 clusters = 0;
  u64 mesh = 0;  // MeshDesc[]: the quantized positions and each mesh's grid
  u64 triangles = 0;
  // Index of this draw's first entry in the whole scene's visible list, which is what everything
  // per entry is indexed by (a deformed instance's pool block). Occlusion pass 2 and the software
  // rasterizer append to their own runs of one list, so `visible` points at the run and this
  // shifts a slot back onto the whole list. The visibility id is the scene's pair, not the entry.
  u32 visible_offset = 0;
  u32 triangles_per_cluster = 0;  // vertex path only: the draw's vertex count / 3
  u64 visible = 0;     // u32x2[]: {instance, cluster} per entry; 0 draws {0, i} in index order
  u64 visibility = 0;  // u64[width * height] visibility buffer (fs_visibility, software raster)
  u32 width = 0;
  u32 height = 0;
  u64 instances = 0;  // InstanceDesc[]
};
static_assert(sizeof(ClusterDrawParams) == 128);

// Mirrors DeformParams in deform.slang: the push constants of the deformed-vertex pool pass.
// 88 bytes. One workgroup per entry of one run of the cull pass's visible list, dispatched
// indirectly from that run's count, so the pass costs the LOD cut and not the source mesh. A
// thread writes the entry's own block of the pool, which `deform_alloc.slang` handed out before
// this pass ran; an entry that did not fit writes nothing and draws its rest pose.
//
// The morph stream's scene-wide arrays, one block in a device buffer that `DeformParams::morph`
// points at. They are scene-wide because `geometry::merge_cluster_meshes` concatenates every
// mesh's channels, directory and deltas into one set with the slices keyed by the global cluster
// index — so one block serves every mesh and every view of a frame, exactly as `StreamParams`
// does for the page table. Null for a scene with no channels.
struct MorphParams {
  u64 channels = 0;       // geometry::MorphChannel[]
  u64 directory = 0;      // u32[cluster_count + 1]: the CSR into `slices`
  u64 slices = 0;         // geometry::MorphSlice[]
  u64 indices = 0;        // u8 per delta, cluster-local, padded to a multiple of four
  u64 deltas = 0;         // i16[3 * delta_count]
  u64 normal_deltas = 0;  // i16[3 * delta_count], or 0 when no channel of the scene moves normals
};
static_assert(sizeof(MorphParams) == 48);

// **104, not the 88 it was.** Two addresses were appended: the morph block above, and the
// deformed normal pool. Both are null for a scene with no morph channels.
struct DeformParams {
  u64 clusters = 0;       // geometry::ClusterDesc[]
  u64 instances = 0;      // InstanceDesc[]
  u64 meshes = 0;         // MeshDesc[]
  u64 attributes = 0;     // geometry::VertexAttributes[]; 0: displace radially instead
  u64 visible = 0;        // u32x2[]: one run of the visible list, {instance, cluster} per entry
  u64 visible_count = 0;  // u32: that run's count word, the cull pass's atomic
  u64 pool = 0;           // f32[3 * pool_vertices] out
  u64 deform = 0;         // DeformDesc[]
  u64 slots = 0;          // u32[]: the whole list's per-entry pool bases, from deform_alloc.slang
  // The morph stream's six addresses, as **one block behind one address** the way
  // `CullParams::streaming` carries the page table's: the push block is 128 bytes and six more
  // addresses do not fit beside what is already here, and unlike those they are read once per
  // workgroup rather than once per vertex. Null for a scene with no morph channels, and the pass
  // then runs exactly the instructions it ran before morphs existed — which is what keeps every
  // picture of such a scene byte-identical.
  u64 morph = 0;  // MorphParams*
  // The **deformed normal pool**, one octahedral u32 per pool vertex, parallel to `pool`. Zero
  // when the frame does not carry one, and the resolve then reads the rest normal off the
  // attribute stream exactly as it always has (gfx.md, "What happens to the shading normal").
  u64 normal_pool = 0;
  f32 time = 0.0f;         // animation phase in seconds
  f32 amplitude = 1.0f;    // displacement scale as a fraction of the mesh's grid box
  u32 max_entries = 0;     // the run's capacity; the count read from the device is clamped to it
  u32 visible_offset = 0;  // this run's first entry in the whole visible list
  // The frame's `TerrainLevelDesc` table, which a `k_deform_stage_terrain` instance reads through
  // `InstanceDesc::terrain`; 0 for a scene with no terrain levels.
  u64 terrain = 0;
};
static_assert(sizeof(DeformParams) == 112);

// Mirrors AllocParams in deform_alloc.slang: the push constants of the pool's allocator. 72
// bytes. **One workgroup**, once per run of the visible list, looping over the views inside it:
// the allocation is a prefix sum over the visible entries' vertex counts, and doing it in one
// workgroup in a fixed order (entry, then view, then run) is what makes *which* entries overflow
// a function of the cut rather than of how the GPU happened to schedule the dispatch.
struct DeformAllocParams {
  u64 clusters = 0;        // geometry::ClusterDesc[]
  u64 instances = 0;       // InstanceDesc[]
  u64 visible = 0;         // u32x2[]: the whole visible list
  u64 visible_counts = 0;  // u32: view 0's count word for this run; views k_draw_args_bytes apart
  u64 slots = 0;           // u32[] out: one per entry of the whole visible list
  u64 alloc = 0;           // DeformAlloc: the cursor and the counters
  u32 pool_vertices = 0;   // the budget, in vertices
  u32 pair_count = 0;      // one view's run is at most this long
  u32 views = 0;
  // The whole list's entry where view 0's run of this dispatch starts; view v's is v * pair_count
  // further on. A camera run is run-major, `run * views * pair_count`; the shadow cascades' runs
  // follow every view's three, and their dispatch passes the cascade count as `views`. It was the
  // run index until the cascades needed a run the run-major formula cannot name.
  u32 first_entry = 0;
  u32 reset = 0;  // 1: the frame's first allocation dispatch, which zeroes the record
  u32 pad = 0;
};
static_assert(sizeof(DeformAllocParams) == 72);

inline constexpr u32 k_deform_workgroup_size = 128;   // numthreads in deform.slang
inline constexpr u32 k_deform_alloc_workgroup = 256;  // numthreads in deform_alloc.slang

// Mirrors HizParams in hiz_build.slang: the push constants of **one dispatch**, which folds a
// 32 x 32 tile of mip `src_mip` down through up to `k_hiz_levels_per_dispatch` further mips in
// shared memory. The destination mips are contiguous behind the source, so the shader walks
// their offsets and extents the way hiz_layout() does rather than being handed an array of them.
//
// **48 bytes, not the 40 it was**: `coverage` was appended when the resolve started skipping
// empty tiles. The mask is this pass's by-product — the from_visibility dispatch already reads
// every word of the visibility buffer and its workgroup is exactly one tile — so the address had
// to reach it, and there was no spare word: every other field is in use and an address needs
// eight bytes. The block is push constants, well under the 128-byte limit.
struct HizParams {
  u64 src = 0;       // u64[] visibility buffer (from_visibility) or f32[] of mip src_mip
  u64 pyramid = 0;   // f32[] from hiz_layout(): this view's mips back to back
  u64 coverage = 0;  // u32[] one per 32 x 32 tile; 0 writes none. Only from_visibility writes it.
  u32 width = 0;     // mip 0's extent, which every mip's extent is derived from
  u32 height = 0;
  u32 src_mip = 0;     // which mip `src` holds
  u32 src_offset = 0;  // element offset of mip `src_mip` in `pyramid` (hiz_layout's offsets[m])
  u32 levels = 0;      // reductions written: mips src_mip + 1 .. src_mip + levels
  u32 from_visibility = 0;  // 1: src is the u64 visibility buffer and mip src_mip is written too
};
static_assert(sizeof(HizParams) == 48);

// Tiles in one row of a `hiz_build.slang` coverage mask over a region `extent` pixels wide: one
// per workgroup of the from_visibility dispatch, which is what makes the mask free.
inline u32 hiz_coverage_pitch(u32 extent) noexcept { return (extent + 31) / 32; }

inline constexpr u32 k_cull_workgroup_size = 64;  // numthreads in cluster_cull.slang
inline constexpr u32 k_hiz_workgroup_size = 16;   // numthreads in hiz_build.slang (16 x 16)
inline constexpr u32 k_hiz_tile = 32;             // source texels one workgroup folds, per side
// 32 -> 16 -> 8 -> 4 -> 2 -> 1: the halvings a 16 x 16 workgroup can do in shared memory.
inline constexpr u32 k_hiz_levels_per_dispatch = 5;

inline void set_frustum(CullParams& params, const Frustum& frustum) noexcept {
  params.plane_count = frustum.plane_count;
  for (u32 i = 0; i < 6; ++i) {
    params.planes[i] = i < frustum.plane_count
                           ? Vec4{frustum.planes[i].normal.x, frustum.planes[i].normal.y,
                                  frustum.planes[i].normal.z, frustum.planes[i].d}
                           : Vec4{};
  }
}

// One thread per (instance, cluster) pair.
inline u32 cull_group_count(u32 pair_count) noexcept {
  return (pair_count + k_cull_workgroup_size - 1) / k_cull_workgroup_size;
}

inline u32 hiz_mip_extent(u32 extent, u32 mip) noexcept {
  return (extent + (1u << mip) - 1) >> mip;
}

// Number of mips down to 1 x 1.
inline u32 hiz_mip_count(u32 width, u32 height) noexcept {
  u32 mips = 1;
  while (hiz_mip_extent(width, mips - 1) > 1 || hiz_mip_extent(height, mips - 1) > 1)
    ++mips;
  return mips < k_hiz_max_mips ? mips : k_hiz_max_mips;
}

// Fills `offsets` (element index of each mip, back to back) and returns the total element
// count of the pyramid for a `width` x `height` mip 0.
inline u32 hiz_layout(u32 width, u32 height, u32 offsets[k_hiz_max_mips]) noexcept {
  const u32 mips = hiz_mip_count(width, height);
  u32 total = 0;
  for (u32 m = 0; m < k_hiz_max_mips; ++m) {
    offsets[m] = total;
    if (m < mips) total += hiz_mip_extent(width, m) * hiz_mip_extent(height, m);
  }
  return total;
}

// Workgroups along one axis of a dispatch whose **source** mip is `extent` texels wide, each
// folding `k_hiz_tile` of them.
inline u32 hiz_group_count(u32 extent) noexcept { return (extent + k_hiz_tile - 1) / k_hiz_tile; }

// How many dispatches a pyramid of `mips` mips takes: the first writes mip 0 and up to five
// more, each further one reads the last mip written and writes up to five. A single-mip pyramid
// still takes the one dispatch that copies the depth word.
inline u32 hiz_dispatch_count(u32 mips) noexcept {
  if (mips <= 1) return 1;
  return (mips - 1 + k_hiz_levels_per_dispatch - 1) / k_hiz_levels_per_dispatch;
}

// The source mip of dispatch `d` of that chain, and how many reductions it writes.
inline u32 hiz_dispatch_src_mip(u32 d) noexcept { return d * k_hiz_levels_per_dispatch; }

inline u32 hiz_dispatch_levels(u32 mips, u32 d) noexcept {
  const u32 done = hiz_dispatch_src_mip(d);
  const u32 left = mips > done + 1 ? mips - 1 - done : 0;
  return left < k_hiz_levels_per_dispatch ? left : k_hiz_levels_per_dispatch;
}

}  // namespace engine::gfx
