#pragma once

// Parameters of the ray-query primary visibility pass (shaders/ray_visibility.slang): one ray
// per pixel center against a top-level structure in the bindless set, writing the visibility
// buffer's 64-bit word (reversed-Z depth bits << 32 | visible_index << 8 | triangle) so the RT
// picture compares against the rasterizer's word for word. Read through a device address.
//
// A hit's GeometryIndex must be the visible index the rasterizer would have written. On the
// cluster path each CLAS record carries its base geometry index, which is set to exactly that,
// so nothing has to be remapped. On the KHR path a bottom-level structure is built per instance
// with one geometry per visible cluster of that instance, so `instance_base` holds the visible
// index of each instance's first geometry and InstanceID() picks the entry.

#include <core/base/types.h>
#include <core/math/math.h>

namespace engine::gfx {

// Mirrors RayVisibilityParams in ray_visibility.slang. 176 bytes.
struct RayVisibilityParams {
  Mat4 view_proj;
  Mat4 inv_view_proj;
  Vec4 camera;            // xyz position
  u64 output = 0;         // u64[width * height]
  u64 instance_base = 0;  // u32[instance_count]: the visible index of each instance's first
                          // geometry; 0 when GeometryIndex is already the visible index
  u32 width = 0;
  u32 height = 0;
  u32 scene = 0;  // bindless slot of the top-level structure
  u32 pad = 0;
};
static_assert(sizeof(RayVisibilityParams) == 176);

inline constexpr u32 k_ray_visibility_workgroup = 8;  // numthreads(8, 8, 1)

inline u32 ray_visibility_group_count(u32 extent) noexcept {
  return (extent + k_ray_visibility_workgroup - 1) / k_ray_visibility_workgroup;
}

}  // namespace engine::gfx
