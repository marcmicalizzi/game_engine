#pragma once

// Parameters of the GPU cluster culling and LOD selection pass (shaders/cluster_cull.slang).
// The RHI stays ignorant of the geometry module: the pass reads cluster and LOD descriptors
// through device addresses whose layouts geometry pins with size tables, and this struct only
// carries the camera, the frustum, and those addresses.
//
//     CullParams params{};
//     set_frustum(params, frustum_from_view_proj(view_proj));
//     params.camera = Vec4(eye, znear);
//     params.lod = Vec4(proj_scale, threshold_px, 1.0f, 1.0f);
//     ...write to a host-visible buffer, push its address, dispatch ceil(cluster_count / 64)...
//     vkCmdDrawMeshTasksIndirectEXT(commands, draw_args.buffer, 0, 1, sizeof(u32) * 3);
//
// Before the dispatch, `draw_args` must hold {0, 1, 1}: the pass counts survivors into x.

#include <core/base/types.h>
#include <core/math/math.h>

namespace engine::gfx {

// Mirrors CullParams in cluster_cull.slang. 176 bytes.
struct CullParams {
  Vec4 planes[6];  // inward-facing, normalized
  Vec4 camera;     // xyz position, w = znear
  Vec4 lod;        // x = proj_scale (cot(fov_y/2) * viewport_height / 2), y = threshold_px,
                   // z = LOD selection enabled, w = frustum culling enabled
  u32 cluster_count = 0;
  u32 plane_count = 0;
  u32 pad0 = 0;
  u32 pad1 = 0;
  u64 clusters = 0;   // geometry::ClusterDesc[]
  u64 lods = 0;       // geometry::ClusterLodDesc[]
  u64 visible = 0;    // u32[cluster_count]
  u64 draw_args = 0;  // u32[3] = {survivors, 1, 1}
};
static_assert(sizeof(CullParams) == 176);

inline constexpr u32 k_cull_workgroup_size = 64;  // numthreads in cluster_cull.slang

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

}  // namespace engine::gfx
