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
// Survivors append a `uint2 {instance, cluster}` to the visible list, and the visibility buffer's
// id is `visible_index << 8 | triangle`: the rasterizers and the resolve read `visible[i]` to get
// back to the instance and the cluster.
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
//     vkCmdDrawMeshTasksIndirectEXT(commands, draw_args.buffer, 0, 1, sizeof(u32) * 3);
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

namespace engine::gfx {

inline constexpr u32 k_hiz_max_mips = 16;  // enough for 32768 x 32768

// One indirect argument block, and the stride between two views' blocks in one buffer. Four u32
// covers both shapes the cull pass fills — `{groups, 1, 1}` for `vkCmdDrawMeshTasksIndirectEXT`
// and `{vertexCount, instanceCount, 0, 0}` for `vkCmdDrawIndirect` — and keeps every view's block
// 16-byte aligned. `clas_records.slang` mirrors the number, because it reads each view's count
// word out of one buffer (docs/plan/04-renderer.md §4.6).
inline constexpr u32 k_draw_args_bytes = 16;

// Mirrors CullParams in cluster_cull.slang. 376 bytes.
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
                        // groups {count, 1, 1}, 1 for vkCmdDrawIndirect {verts, count, 0, 0}
  u32 cone_cull = 0;    // 1: backface-cull clusters by their normal cone (ClusterDesc::cone)
  u64 clusters = 0;     // geometry::ClusterDesc[]
  u64 lods = 0;         // geometry::ClusterLodDesc[]
  u64 visible = 0;      // u32x2[]: {instance, cluster} of the hardware survivors. Every draw of a
                        // frame appends to its own run of one list, so this is the run's address
                        // and the draw's ClusterDrawParams::visible_offset is the run's index.
  u64 draw_args = 0;    // u32[3] = {survivors, 1, 1} for vkCmdDrawMeshTasksIndirectEXT
  u64 sw_visible = 0;   // u32x2[]: the software-rasterized survivors, in their own run
  u64 sw_args = 0;      // u32[3] = {survivors, 1, 1} for vkCmdDispatchIndirect
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
};
static_assert(sizeof(CullParams) == 400);
static_assert(sizeof(CullParams) % 16 == 0, "the block is read as float4 rows on the GPU");

inline constexpr f32 k_raster_hardware = 0.0f;  // CullParams::raster.y
inline constexpr f32 k_raster_software = 1.0f;
inline constexpr f32 k_raster_split = 2.0f;

// Per-instance deformation (docs/plan/04-renderer.md §4.3, ADR-0026 decision 7). An instance
// whose `InstanceDesc::deform` is not `k_invalid_deform` is **deformed**: every position read for
// it — by the three rasterizers, by the resolve's reconstruction, and by the cluster
// acceleration structure records — comes out of the frame's deformed-vertex pool instead of its
// mesh's 16-bit grid. `pool_offset` is added to the **scene-wide** vertex index, so it is the
// instance's block base minus its mesh's first vertex (u32 arithmetic, and the bias wraps); the
// block is as long as the whole mesh's cluster-ordered vertex range, so a cluster's vertices are
// contiguous in the pool and a CLAS record can point straight at them.
//
// **24 bytes, not the original 16.** A skinned instance also needs the address of its own bone
// matrices, and that is per *instance*, not per mesh: two characters share one mesh, one skin
// binding stream and one skeleton, and have entirely different poses. The word that was `pad`
// became `joint_count`, which the pass clamps a binding's joint index against, and the address
// was appended. GPU-mirrored; keep in step with `DeformDesc` in the shaders.
struct DeformDesc {
  u32 pool_offset = 0;   // added to the scene-wide vertex index to reach this instance's slot
  u32 vertex_count = 0;  // the block's length: the mesh's cluster-ordered vertex count
  u32 flags = 0;         // which deformer: k_deform_*
  u32 joint_count = 0;   // the bone-matrix array's length; 0 leaves a skinned instance at rest
  u64 joints = 0;        // anim::JointMatrix[joint_count]: three float4 rows each, or 0
};
static_assert(sizeof(DeformDesc) == 24);

inline constexpr u32 k_invalid_deform = ~u32{0};  // InstanceDesc::deform: the instance is rigid
// The deformers. Identity writes the rest pose into the pool, which is what proves a deformed
// instance and a rigid one draw the same picture; wave and lattice are the E25 spike's procedural
// stand-ins; **skin** is the first real one (docs/plan/05-simulation.md §5.11), reading the
// mesh's `geometry::SkinBinding` stream and the instance's `anim::JointMatrix` array.
inline constexpr u32 k_deform_identity = 0;
inline constexpr u32 k_deform_wave = 1;     // sinusoidal displacement along the vertex normal
inline constexpr u32 k_deform_lattice = 2;  // 3x3x3 trilinear cage over the mesh's grid box
// Linear blend skinning: MeshDesc::skin (per vertex) x DeformDesc::joints (per instance).
inline constexpr u32 k_deform_skin = 3;
inline constexpr u32 k_deform_kind_mask = 3;  // exactly four kinds fit; a fifth needs another bit

// GPU-mirrored; keep in step with the MeshDesc struct in the shaders. 64 bytes, read through a
// device address. One per mesh of the scene: the 16-bit position grid this mesh's positions are
// on (`geometry::ClusterMeshPart`), the stream of three u16 per vertex that every mesh shares
// (`geometry::ClusterMesh::quantized`, indexed by the scene-wide vertex index and padded to an
// even count so the shaders' load_position may read the last triple as two 32-bit words), and
// the range of the global cluster array this mesh owns. Six bytes of position per vertex instead
// of twelve; the acceleration structure builders still read the float positions.
//
// The last four addresses are where a **deformed** instance's positions come from instead. Two
// of them (`deform_pool`, `deform`) are the scene's, the same in every MeshDesc, and they ride
// here rather than in `ClusterDrawParams` because that push block is full at its 128-byte limit
// and every position read already holds the MeshDesc. `templates` and `skin` are the mesh's own.
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
  u64 deform_pool = 0;  // f32[3 * pool_vertices]: the frame's pool; 0 when nothing deforms
  u64 deform = 0;       // DeformDesc[], indexed by InstanceDesc::deform; 0 when nothing deforms
  u64 templates = 0;    // u64[]: one cluster template address per global cluster index, or 0
  u64 skin = 0;         // geometry::SkinBinding[]: eight bytes per scene-wide vertex, or 0
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
struct InstanceDesc {
  Mat4 world;             // mesh space to world; the top three rows are used
  u32 mesh = 0;           // index into the MeshDesc array
  u32 material_base = 0;  // added to the cluster's material index in the resolve
  u32 first_pair = 0;     // prefix sum of the instances' mesh cluster counts, in order
  f32 scale_max = 1.0f;   // largest axis scale: radii and LOD errors multiply by it
  u32 flags = k_instance_uniform_scale;
  u32 deform = k_invalid_deform;  // entry of the DeformDesc table; k_invalid_deform: rigid
  f32 bounds_padding = 0.0f;  // mesh-space slack added to every sphere this instance is culled by
  u32 pad = 0;
};
static_assert(sizeof(InstanceDesc) == 96);

// Fills `world`, `scale_max`, and the uniform-scale flag from an affine transform. The scales are
// the lengths of the upper-left 3x3's columns; "uniform" means they agree to a part in 10^4,
// which is what lets the cone test and the cheap normal transform run.
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
  // Index of this draw's first entry in the whole scene's visible list, which the id carries and
  // the resolve indexes. Occlusion pass 2 and the software rasterizer append to their own runs of
  // one list, so `visible` points at the run and this shifts the ids back onto the whole list.
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
// 80 bytes. One workgroup per entry of one run of the cull pass's visible list, dispatched
// indirectly from that run's count, so the pass costs the LOD cut and not the source mesh. A
// thread writes the pool slots of the cluster's vertices only; every other slot is left alone.
struct DeformParams {
  u64 clusters = 0;       // geometry::ClusterDesc[]
  u64 instances = 0;      // InstanceDesc[]
  u64 meshes = 0;         // MeshDesc[]
  u64 attributes = 0;     // geometry::VertexAttributes[]; 0: displace radially instead
  u64 visible = 0;        // u32x2[]: one run of the visible list, {instance, cluster} per entry
  u64 visible_count = 0;  // u32: that run's count word, the cull pass's atomic
  u64 pool = 0;           // f32[3 * pool_vertices] out
  u64 deform = 0;         // DeformDesc[]
  f32 time = 0.0f;        // animation phase in seconds
  f32 amplitude = 1.0f;   // displacement scale as a fraction of the mesh's grid box
  u32 max_entries = 0;    // the run's capacity; the count read from the device is clamped to it
  u32 pad = 0;
};
static_assert(sizeof(DeformParams) == 80);

inline constexpr u32 k_deform_workgroup_size = 128;  // numthreads in deform.slang

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
