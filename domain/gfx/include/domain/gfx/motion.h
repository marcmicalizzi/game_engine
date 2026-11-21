#pragma once

// The motion pass (docs/subsystems/gfx.md, "Motion from the visibility buffer"; ADR-0064; roadmap
// R154): where each pixel's surface point was in the last frame's picture, computed by one compute
// pass over the visibility buffer (`shaders/motion.slang`), because no rasterizer writes a velocity
// and the ray path has no fragment stage to add one to. This header is the pass's parameter block,
// the table of last frame's transforms for the instances that moved, and the packing of what it
// writes; the shader mirrors each once.
//
// **What a pixel's word holds**: two half floats, x in the low half and y in the high, the pixels
// the pixel's surface point moved across the picture since the last frame — x to the right, y down
// — measured with **neither frame's sub-pixel offset in it** (both frames' un-offset projections).
// The point was at `pixel - motion` in the last frame's picture. `k_motion_invalid` (both halves
// +infinity) is a point that was behind last frame's camera: there is no place to look it up.
//
// **By what the pixel holds** (the page's table, in the shader's order):
//   - nothing (the sky), or a rigid instance that did not move: `reproject` times the pixel's own
//     clip-space point — its NDC and the visibility word's depth — and nothing else is read. A
//     depth of zero is a point at infinity, so the sky needs no case of its own.
//   - a rigid instance in the moved table: the point taken back through this frame's instance
//     transform and forward through last frame's (`MovedInstance`), then `prev_view_proj`.
//   - a deformed instance: the same, with the mesh-space point replaced by the triangle's three
//     corners as last frame's inputs deformed them (`prev_pool`, the pool pass's second write) at
//     the point's barycentrics among this frame's corners.

#include <core/base/types.h>
#include <core/math/math.h>
#include <domain/gfx/cluster_cull.h>

#include <cstddef>
#include <cstring>

namespace engine::gfx {

// The pass's workgroup: 8 x 8 threads, one a pixel.
inline constexpr u32 k_motion_workgroup = 8;
// Both halves +infinity: the point was behind last frame's camera.
inline constexpr u32 k_motion_invalid = 0x7c007c00u;

// **Last frame's transform of an instance that has moved** (`SceneRenderer::move_instances`): its
// cell and its 3x4, as `InstanceDesc` holds them. The table is sorted by `instance` and searched
// only in a frame whose table is not empty. An instance stays in the table once it has moved, its
// entry last frame's transform whether or not it moved again, so the table never names an instance
// whose transform it does not know.
struct MovedInstance {
  u32 instance = 0;
  Vec3i cell;
  Vec4 rows[3] = {{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}};
};
static_assert(sizeof(MovedInstance) == 64);

// One view's block, behind one address (the push block is that address). Every matrix is made on
// the CPU in double and rounded once (`renderer::MotionPass`), so the eyes' difference between two
// frames never passes through a float sum of world positions (ADR-0053).
struct MotionParams {
  // This frame's clip space — a pixel's NDC as it was drawn, offset included, and the word's depth
  // — to last frame's un-offset clip space: last frame's view-projection, times the translation
  // from this frame's eye to last frame's, times the inverse of this frame's offset
  // view-projection.
  Mat4 reproject;
  // The same clip space to this frame's space (the eye at the origin): the inverse alone. Read only
  // for a moved or a deformed instance, whose point has to go through its transform.
  Mat4 unproject;
  // Last frame's un-offset view-projection, from last frame's space (its eye at the origin).
  Mat4 prev_view_proj;
  FrameEye eye;             // this frame's
  FrameEye prev_eye;        // last frame's: where an instance stood from it
  u64 visibility = 0;       // u64[width * height]: this view's region of this frame's buffer
  u64 prev_visibility = 0;  // the same region of last frame's, kept; nothing in this pass reads it
  u64 pairs = 0;         // u32[2 * pair_count]: the scene's {instance, cluster} per pair; 0: none
  u64 pair_entries = 0;  // u32[pair_count]: this view's pair -> visible entry; 0: the pair is it
  u64 visible = 0;       // u32[2 * entries]: the visible list, read only without `pairs`
  u64 instances = 0;     // InstanceDesc[]
  u64 meshes = 0;        // MeshDesc[]
  u64 clusters = 0;      // ClusterDesc[]
  u64 triangles = 0;     // u32[]: packed local indices
  u64 output = 0;        // u32[width * height]: this view's region of the motion buffer
  u64 moved = 0;         // MovedInstance[moved_count], sorted by instance; 0: none
  u64 prev_pool = 0;     // f32[3 * pool_vertices]: the pool as last frame's inputs wrote it; 0
  u32 width = 0;
  u32 height = 0;
  u32 instance_count = 0;
  u32 moved_count = 0;
  // Added to a pixel's NDC to give its point's un-offset NDC: (2 jx / width, -2 jy / height) for a
  // sample `(jx, jy)` pixels from the pixel's centre, x right and y down. Zero with no offset.
  f32 offset_ndc_x = 0.0f;
  f32 offset_ndc_y = 0.0f;
  // **Where the point's depth was** (R194): f32[width * height], this view's region of a buffer the
  // pass fills beside the vectors with the depth the pixel's point had in last frame's clip space
  // (reversed-Z, as the visibility buffer holds it; 0 for the sky, -1 where the point was behind
  // last frame's camera), so a reader can ask whether what last frame drew at the place the vector
  // names was that point or something in front of or behind it. 0: not written (only the
  // accumulation asks, `MotionPass::keep_depth`).
  u64 prev_depth = 0;
  // **The instance slots this frame's tables wrote since the last frame** (R194 leg 2):
  // `fresh_count` runs of `uint2 {begin, end}`, sorted, at `fresh`. An instance in one is new to
  // its slot, so its vector is its own transform's and the camera's, never last frame's deform
  // inputs' (which held another tree's pose at its record, or none). 0 / 0: none.
  u64 fresh = 0;
  u32 fresh_count = 0;
  u32 fresh_pad = 0;
};
static_assert(sizeof(MotionParams) == 432);
static_assert(offsetof(MotionParams, prev_depth) == 408);
static_assert(offsetof(MotionParams, fresh) == 416);

// A half float's value: what a reader of the motion buffer on the CPU decodes a word with.
inline f32 half_to_float(u16 half) noexcept {
  const u32 sign = static_cast<u32>(half & 0x8000u) << 16;
  const u32 exponent = (half >> 10) & 0x1fu;
  const u32 mantissa = half & 0x3ffu;
  u32 bits = 0;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      // A subnormal half: normalize it into a float's exponent.
      u32 m = mantissa;
      u32 e = 113;
      while ((m & 0x400u) == 0) {
        m <<= 1;
        --e;
      }
      bits = sign | (e << 23) | ((m & 0x3ffu) << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7f800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  }
  f32 value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// A motion word's two components, pixels; false for `k_motion_invalid`.
inline bool unpack_motion(u32 word, f32& x, f32& y) noexcept {
  if (word == k_motion_invalid) return false;
  x = half_to_float(static_cast<u16>(word & 0xffffu));
  y = half_to_float(static_cast<u16>(word >> 16));
  return true;
}

}  // namespace engine::gfx
