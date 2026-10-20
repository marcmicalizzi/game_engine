#pragma once

// **The flora stage of the deformed-vertex pool** (docs/subsystems/renderer.md, "The flora stage";
// docs/subsystems/flora.md, "Motion"; roadmap S66): what `deform.slang` reads to sway a generated
// tree, and the CPU mirror of what it does with it.
//
// A tree's mesh carries the motion contract (geometry::ClusterFileMotionPart per part,
// geometry::ClusterFileMotionVertex per vertex: its part and its bend weight, 0 at the part's root
// to 1 at its tip). A frame gives every part of every swaying instance **one pose**: a bend — a
// rotation of `angle` radians about `axis` through `pivot`, scaled per vertex by the vertex's bend
// weight, so the root stays and the tip travels — and the rigid `frame` its parent chain's bends
// carried the whole part to. The CPU computes the frames parent-first (renderer's flora_motion.h),
// so the shader never walks a chain: a vertex is one table read, one sine and cosine, and two small
// matrix products, whatever the depth of its part.
//
//     q  = pivot + rotate(p - pivot, axis, angle * weight)
//     p' = frame * q                       (frame: a 3x4, rotation and translation)
//     n' = frame.linear * rotate(n, axis, angle * weight)
//
// **A part at rest is passed through, not computed**: a pose whose `axis.w` is 0 writes the rest
// position off the grid and the rest normal's packed word, so a tree with no wind and no push
// draws its rest pose to the bit (the renderer's test pins it).
//
// Everything is in the instance's **mesh space** (ADR-0053): the pivots are the container's, and no
// field holds a world position.

#include <core/base/types.h>
#include <core/math/math.h>

#include <cmath>

namespace engine::gfx {

// One part's pose for one frame, GPU-mirrored (deform.slang's `FloraPartPose`). 80 bytes: the
// frame's three rows, then the bend.
//
// `rows` is the rigid transform the part's ancestors' bends moved it by — rows[r] = (R[r][0],
// R[r][1], R[r][2], t[r]), the 3x4 a point goes through as three dot products with float4(p, 1),
// like `InstanceDesc::rows` — the identity for the trunk. `pivot_angle` is the part's pivot (xyz)
// and its own bend at the tip (w, radians); `axis` the bend's unit axis (xyz), and in w 1 when the
// part moves at all and 0 when it is at rest.
struct FloraPartPose {
  Vec4 rows[3] = {{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}};
  Vec4 pivot_angle{};
  Vec4 axis{0.0f, 0.0f, 1.0f, 0.0f};
};
static_assert(sizeof(FloraPartPose) == 80);

// Rodrigues' rotation of `v` about the unit `k` by the angle whose cosine and sine are `c`, `s`:
// v c + (k x v) s + k (k . v)(1 - c). The shader's `flora_rotate` is the same expression in the
// same order, which is what lets the reference hold the two to a few ulps.
inline Vec3 flora_rotate(Vec3 v, Vec3 k, f32 c, f32 s) noexcept {
  const Vec3 kxv = cross(k, v);
  const f32 kv = dot(k, v) * (1.0f - c);
  return Vec3{v.x * c + kxv.x * s + k.x * kv, v.y * c + kxv.y * s + k.y * kv,
              v.z * c + kxv.z * s + k.z * kv};
}

inline Vec3 flora_frame_point(const FloraPartPose& pose, Vec3 q) noexcept {
  return Vec3{dot(pose.rows[0].xyz(), q) + pose.rows[0].w,
              dot(pose.rows[1].xyz(), q) + pose.rows[1].w,
              dot(pose.rows[2].xyz(), q) + pose.rows[2].w};
}
inline Vec3 flora_frame_direction(const FloraPartPose& pose, Vec3 d) noexcept {
  return Vec3{dot(pose.rows[0].xyz(), d), dot(pose.rows[1].xyz(), d), dot(pose.rows[2].xyz(), d)};
}

// **The CPU mirror of the stage**: the position and normal a vertex of bend weight `weight` (0..1)
// at rest position `p` with rest normal `n` takes under `pose`. A pose at rest returns them as they
// are.
inline void flora_bend(const FloraPartPose& pose, f32 weight, Vec3 p, Vec3 n, Vec3& out_p,
                       Vec3& out_n) noexcept {
  if (pose.axis.w == 0.0f) {
    out_p = p;
    out_n = n;
    return;
  }
  const f32 a = pose.pivot_angle.w * weight;
  const f32 c = std::cos(a);
  const f32 s = std::sin(a);
  const Vec3 k = pose.axis.xyz();
  const Vec3 pivot = pose.pivot_angle.xyz();
  const Vec3 q = flora_rotate(p - pivot, k, c, s) + pivot;
  out_p = flora_frame_point(pose, q);
  const Vec3 m = flora_frame_direction(pose, flora_rotate(n, k, c, s));
  const f32 len = length(m);
  out_n = len > 0.0f ? m * (1.0f / len) : n;
}

}  // namespace engine::gfx
