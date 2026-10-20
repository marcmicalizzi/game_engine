#pragma once

// Parameters of the ray-query primary visibility pass (shaders/ray_visibility.slang): one ray
// per pixel center against a top-level structure in the bindless set, writing the visibility
// buffer's 64-bit word (reversed-Z depth bits << 32 | pair << 8 | triangle) so the RT picture
// compares against the rasterizer's word for word. Read through a device address.
//
// A hit's GeometryIndex is the hit cluster's **visible entry**. On the cluster path each CLAS
// record carries its base geometry index, which is set to exactly that, so nothing has to be
// remapped. On the KHR path a bottom-level structure is built per instance with one geometry per
// visible cluster of that instance, so `instance_base` holds the visible index of each instance's
// first geometry and InstanceID() picks the entry. The id the pass writes is not the entry but the
// scene's pair the entry names (docs/subsystems/gfx.md, "The tie rule"), which is why the visible
// list, the instances and the meshes are here: `visible[entry]` is the {instance, cluster}, and
// the instance's `first_pair` and its mesh's `first_cluster` turn that into the pair.
//
// **Ties.** A ray query commits the first of two hits at the same distance that the traversal
// happens to meet, and the traversal's order is the acceleration structure's, not the scene's. So
// after the closest hit the pass looks again over exactly that distance, with every candidate
// reported to it, and keeps the one the rasterizers' tie rule would: the larger (pair, triangle).
// `caster_base` is where the shadow casters' entries start, so that second look can leave them out
// the way the first does (RAY_FLAG_CULL_NON_OPAQUE cannot, once every candidate is forced
// non-opaque to be reported).

#include <core/base/types.h>
#include <core/math/math.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/view_ray.h>

namespace engine::gfx {

// **Alpha-tested materials** (renderer.md, "Alpha-tested geometry"): with `masks.materials` set the
// pass traces without culling non-opaque geometry and its loop decides each non-opaque candidate
// itself — a shadow caster's entry (at or past `caster_base`) is left out as the cull flag left it
// out, an alpha-tested pair is kept where its mask keeps the hit (`clusters` and `triangles` give
// the hit's UV), and anything else non-opaque is kept. All zero: the pass is the one it always was.
//
// Mirrors RayVisibilityParams in ray_visibility.slang. 256 bytes.
struct RayVisibilityParams {
  Mat4 view_proj;
  // Clip space to the world direction of the eye's ray, `clip_to_ray(projection, view)`: the
  // rotation and projection without the eye's position, so a ray's direction does not depend on
  // where the camera stands (view_ray.h).
  Mat4 clip_to_ray;
  Vec4 camera;            // xyz position
  u64 output = 0;         // u64[width * height]
  u64 instance_base = 0;  // u32[instance_count]: the visible index of each instance's first
                          // geometry; 0 when GeometryIndex is already the visible index
  u32 width = 0;
  u32 height = 0;
  u32 scene = 0;                  // bindless slot of the top-level structure
  u32 caster_base = 0xFFFFFFFFu;  // the first shadow caster's visible entry; ~0 when there are none
  u64 visible = 0;    // u32x2[]: the visible list GeometryIndex (+ instance_base) is an entry of
  u64 instances = 0;  // InstanceDesc[] (cluster_cull.h)
  u64 meshes = 0;     // MeshDesc[] (cluster_cull.h)
  u64 triangles = 0;  // u32[]: packed local indices, for a masked candidate's UV
  u64 clusters = 0;   // geometry::ClusterDesc[]
  u64 pad = 0;        // keeps the block a whole number of float4 rows
  MaskTables masks;   // 0: no alpha-tested material in the frame
};
static_assert(sizeof(RayVisibilityParams) == 256);

inline constexpr u32 k_ray_visibility_workgroup = 8;  // numthreads(8, 8, 1)

inline u32 ray_visibility_group_count(u32 extent) noexcept {
  return (extent + k_ray_visibility_workgroup - 1) / k_ray_visibility_workgroup;
}

}  // namespace engine::gfx
