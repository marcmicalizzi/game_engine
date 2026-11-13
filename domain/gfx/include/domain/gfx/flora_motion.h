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
#include <domain/gfx/cluster_cull.h>

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

// ---- the field kind (renderer.md, "The field kind"; R95)
// -----------------------------------------
//
// **Grass is posed on the GPU, from the wind at the blade's root, with nothing of the CPU's but
// this one block a frame.** A field mesh's record is its mesh's (`k_deform_flora_field`), its part
// table is the scene's, static (`FloraFieldPart`: the blade's root and length, mesh space), and the
// frame's wind, gust field, flutter clock and walker are this block, written at the head of the
// frame's part-pose region, `k_flora_field_frame_bytes` before the address `DeformParams::flora`
// carries (the trees' poses follow it, so nothing about a tree moved). A blade bends about its
// root by the angle
//
//     a = compliance * s * ((0.6 + 0.8 g) + (0.25 + 0.35 g) sin(phi_f + phi_b)) + push
//
// about the horizontal axis across the wind, the trees' lean and flutter with the gust `g` taken
// from a field over the world instead of a clock per tree: four plane waves at the root,
// `g = clamp(0.5 + 0.5 sum_j w_j sin(phases_j + k_j . root), 0, 1)`, whose phases at the eye the
// CPU reduces in double so the GPU adds only the root's offset from the eye (ADR-0053). `phi_f` is
// the frame's flutter clock, `phi_b` the blade's own phase, hash(part, the instance's placement).
// The vertex's arc fraction scales the angle, so a straight blade's displacement rises as the
// square of the fraction and the root stays: the cantilever's shape. A blade at rest — no wind, no
// walker — writes its rest pose, bit for bit.

// One blade of a field mesh: its root (the part's pivot) and its length, mesh space. 16 bytes.
struct FloraFieldPart {
  Vec3 root{};
  f32 length = 0.0f;
};
static_assert(sizeof(FloraFieldPart) == 16);

// **The far field at rest** (renderer.md, "The far field at rest"; R152). Past `rest.x` metres from
// the eye a blade's root takes no wind — the angle fades to zero over the band `1 / rest.y` wide
// that ends `k_field_rest_margin_m` short of it, so the last posed frame of a blade is its rest
// pose — and an instance whose every root is that far (`field_rests`, below) is given no pool
// block (deform_alloc.slang), which every position reader draws from the grid, and references its
// mesh's resident structures in the ray tracing chain instead of having the frame build its own
// (clas_records.slang, `instance_cached`). The margin is what lets the two passes disagree by a
// float's rounding and the picture not know: inside it a posed blade is at rest to the bit.
inline constexpr f32 k_field_rest_margin_m = 0.25f;

// The frame's field, GPU-mirrored (field_rest.slang's `FloraFieldFrame`). 240 bytes, three poses'
// worth, at the head of every frame slot's part-pose region. 240, not 160 (R152): the rule above
// needs five words no field of the block could give up, and the block is where the frame's eye and
// walker already are, which the rule reads too.
struct FloraFieldFrame {
  FrameEye eye{};  // the frame's origin: the root's offset from it is what the waves read
  // Where the wind blows to in the ground plane (x, z; unit), its strength over a record's mean
  // held to k_flora_max_wind, and the flutter's phase this frame, radians in [0, 2 pi).
  Vec4 wind{1.0f, 0.0f, 0.0f, 0.0f};
  Vec4 waves[2] = {};  // the four waves' (x, z) wave vectors, radians a metre: two per Vec4
  Vec4 phases{};       // each wave's phase at the eye, radians in [0, 2 pi)
  Vec4 weights{};      // each wave's share of the gust; they sum to 1
  // The walker: its feet from the eye (xyz) and its radius (w; 0 or less: no walker).
  Vec4 walker{};
  // The walker's height, the push's most radians, its margin metres, and in w the bits of the
  // hand-off's clock (a u32, `GpuScene::handoff_tick`; G136), which a load-in age is counted from.
  Vec4 push{};
  // The far field at rest (above). x: the distance, metres (0: no rule, a blade sways at every
  // distance); y: one over the fade band's width; z: the largest |root| of any field mesh of the
  // scene, mesh space, so an instance's roots lie within z times its `scale_max` of its origin;
  // w: how near the walker an instance's origin must be for a blade of it to be pushed, less that
  // reach — the capsule's radius, the push's margin and the scene's longest blade (0: no walker).
  Vec4 rest{};
  // The scene's field records: the deform entries [field_first, field_first + field_count), one a
  // field mesh, made together at load. How a pass that reads no DeformDesc knows a field instance.
  u32 field_first = 0;
  u32 field_count = 0;
  u32 reserved[14] = {};
};
static_assert(sizeof(FloraFieldFrame) == 240);
inline constexpr u64 k_flora_field_frame_bytes = sizeof(FloraFieldFrame);
static_assert(k_flora_field_frame_bytes == 3 * sizeof(FloraPartPose),
              "the block is three poses' room at the head of a region");

// The share of the wind a blade whose root is `d` metres from the eye takes: 1 short of the band,
// 0 from `k_field_rest_margin_m` short of the rest distance on. field_rest.slang's `field_fade`,
// operation for operation.
inline f32 field_fade(const FloraFieldFrame& field, f32 d) noexcept {
  if (!(field.rest.x > 0.0f)) return 1.0f;
  const f32 t = (field.rest.x - k_field_rest_margin_m - d) * field.rest.y;
  return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

// **Whether an instance stands at rest this frame**: a field instance every one of whose roots is
// at least the rest distance from the eye, so every vertex the pool pass would write for it is its
// rest position. `from_eye` is the instance's translation from the frame's eye
// (`engine::relative`). Never a near form of a hand-off (past its band it draws nothing, which is
// not its rest pose), a far form only past its band's end (inside it the pass thins it by part),
// and never an instance the walker can reach. field_rest.slang's `field_rests`, operation for
// operation; the two passes that ask (the pool's allocator and the chain's records) run the same
// SPIR-V over the same words.
inline bool field_rests(const InstanceDesc& instance, Vec3 from_eye,
                        const FloraFieldFrame& field) noexcept {
  if (!(field.rest.x > 0.0f)) return false;
  if (instance.deform - field.field_first >= field.field_count) return false;
  const u32 role = instance.flags & k_instance_handoff_mask;
  if (role == k_instance_handoff_near) return false;
  f32 bound = field.rest.x;
  if (role == k_instance_handoff_far) {
    const f32 to = static_cast<f32>(instance.band >> 16) * 0.01f;
    if (to > bound) bound = to;
  }
  const f32 reach = field.rest.z * instance.scale_max;
  const f32 least = bound + reach;
  if (!(dot(from_eye, from_eye) >= least * least)) return false;
  if (field.rest.w > 0.0f) {
    const f32 dx = from_eye.x - field.walker.x;
    const f32 dz = from_eye.z - field.walker.z;
    const f32 near = field.rest.w + reach;
    if (dx * dx + dz * dz < near * near) return false;
  }
  return true;
}

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
