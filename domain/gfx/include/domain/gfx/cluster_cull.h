#pragma once

// Parameters of the GPU cluster culling and LOD selection pass (shaders/cluster_cull.slang),
// the two rasterizers' shared push block, the visibility resolve, and the Hi-Z pyramid used by
// two-pass occlusion culling (shaders/hiz_build.slang). The RHI stays ignorant of the geometry
// module: the passes read cluster and LOD descriptors through device addresses whose layouts
// geometry pins with size tables, and these structs only carry cameras, frustums, and addresses.
//
//     CullParams params{};
//     set_frustum(params, frustum_from_view_proj(view_proj));
//     params.view_proj = view_proj;
//     params.camera = Vec4(eye, znear);
//     params.lod = Vec4(proj_scale, threshold_px, 1.0f, 1.0f);
//     ...write to a host-visible buffer, push its address, dispatch cull_group_count(n)...
//     vkCmdDrawMeshTasksIndirectEXT(commands, draw_args.buffer, 0, 1, sizeof(u32) * 3);
//
// Before the dispatch, `draw_args` (and `sw_args`) must hold {0, 1, 1}: the pass counts
// survivors into x. For occlusion culling, `hiz` points at a pyramid laid out by hiz_layout(),
// `pass` is 1 then 2 within a frame, and `flags`/`prev_flags` ping-pong between frames.

#include <core/base/types.h>
#include <core/math/math.h>

namespace engine::gfx {

inline constexpr u32 k_hiz_max_mips = 16;  // enough for 32768 x 32768

// Mirrors CullParams in cluster_cull.slang. 376 bytes.
struct CullParams {
  Vec4 planes[6];  // inward-facing, normalized
  Vec4 camera;     // xyz position, w = znear
  Vec4 lod;        // x = proj_scale (cot(fov_y/2) * viewport_height / 2), y = threshold_px,
                   // z = LOD selection enabled, w = frustum culling enabled
  Vec4 raster;     // x = projected cluster diameter (px) below which a cluster is software
                   // rasterized, y = mode (k_raster_*), z and w unused
  u32 cluster_count = 0;
  u32 plane_count = 0;
  u32 count_index = 0;  // which u32 of draw_args counts hardware survivors: 0 for mesh-task
                        // groups {count, 1, 1}, 1 for vkCmdDrawIndirect {verts, count, 0, 0}
  u32 pad1 = 0;
  u64 clusters = 0;    // geometry::ClusterDesc[]
  u64 lods = 0;        // geometry::ClusterLodDesc[]
  u64 visible = 0;     // u32[cluster_count]: hardware-rasterized survivors
  u64 draw_args = 0;   // u32[3] = {survivors, 1, 1} for vkCmdDrawMeshTasksIndirectEXT
  u64 sw_visible = 0;  // u32[cluster_count]: software-rasterized survivors
  u64 sw_args = 0;     // u32[3] = {survivors, 1, 1} for vkCmdDispatchIndirect
  // Occlusion culling; hiz == 0 disables it.
  Mat4 view_proj;
  u64 hiz = 0;         // f32[] pyramid from hiz_layout()
  u64 prev_flags = 0;  // u32[cluster_count]: drawn last frame
  u64 flags = 0;       // u32[cluster_count]: drawn this frame (cleared before pass 1)
  u32 hiz_width = 0;
  u32 hiz_height = 0;
  u32 hiz_mips = 0;
  u32 pass = 0;  // 0 single pass, 1 last frame's visible set, 2 the rest
  u32 hiz_offsets[k_hiz_max_mips] = {};
};
static_assert(sizeof(CullParams) == 376);

inline constexpr f32 k_raster_hardware = 0.0f;  // CullParams::raster.y
inline constexpr f32 k_raster_software = 1.0f;
inline constexpr f32 k_raster_split = 2.0f;

// Mirrors MeshParams in cluster_mesh.slang and RasterParams in cluster_sw_raster.slang: the push
// constants of the mesh-shader and software rasterization paths. 120 bytes.
struct ClusterDrawParams {
  Mat4 view_proj;
  u64 clusters = 0;
  u64 vertices = 0;
  u64 triangles = 0;
  u32 cluster_count = 0;
  u32 triangles_per_cluster = 0;  // vertex path only: the draw's vertex count / 3
  u64 visible = 0;                // cull output; 0 draws clusters in index order
  u64 visibility = 0;  // u64[width * height] visibility buffer (fs_visibility, software raster)
  u32 width = 0;
  u32 height = 0;
};
static_assert(sizeof(ClusterDrawParams) == 120);

// Mirrors HizParams in hiz_build.slang: the push constants of one pyramid level. 40 bytes.
struct HizParams {
  u64 src = 0;
  u64 dst = 0;
  u32 src_width = 0;
  u32 src_height = 0;
  u32 dst_width = 0;
  u32 dst_height = 0;
  u32 from_visibility = 0;  // 1: src is the u64 visibility buffer, else a f32 mip
  u32 pad = 0;
};
static_assert(sizeof(HizParams) == 40);

inline constexpr u32 k_cull_workgroup_size = 64;  // numthreads in cluster_cull.slang
inline constexpr u32 k_hiz_workgroup_size = 8;    // numthreads in hiz_build.slang (8 x 8)

inline void set_frustum(CullParams& params, const Frustum& frustum) noexcept {
  params.plane_count = frustum.plane_count;
  for (u32 i = 0; i < 6; ++i) {
    params.planes[i] = i < frustum.plane_count
                           ? Vec4{frustum.planes[i].normal.x, frustum.planes[i].normal.y,
                                  frustum.planes[i].normal.z, frustum.planes[i].d}
                           : Vec4{};
  }
}

inline u32 cull_group_count(u32 cluster_count) noexcept {
  return (cluster_count + k_cull_workgroup_size - 1) / k_cull_workgroup_size;
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

inline u32 hiz_group_count(u32 extent) noexcept {
  return (extent + k_hiz_workgroup_size - 1) / k_hiz_workgroup_size;
}

}  // namespace engine::gfx
