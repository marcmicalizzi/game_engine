#pragma once

// **The crouch layer's pose** (docs/subsystems/apps.md, "The crouch layer"; roadmap P54): how a
// body bends to put its eye where a crouching walker's is. The player's body (player.h) measures a
// `CrouchRig` from its joints at load and asks `crouch_pose` every tick for the walker's eye; the
// animated world turns the joints by the answer on its copy of the skinning matrices
// (animation.h, `CrouchTurn`).
//
// **Why it is shaped this way.** The trunk leans forward a share of `trunk` as the crouch deepens —
// stooped walking under a low roof flexes the trunk as the room drops (Friedrich et al. 2000) — the
// hips take the rest of the eye's drop and move back by half of what the lean carried the eye
// forward, so the head stays over the feet rather than past the capsule, and each leg is a two-bone
// chain solved in the body's sagittal plane by the law of cosines with the knee forward and the sole
// kept level. The whole body then moves by the shift that returns the ankles to where they stood:
// the feet stay planted whatever the depth. Every number is `+ - * / sqrt` and `fly_sin_cos` /
// `fly_atan2` (fly_camera.h), so the pose is the same bits on every compiler.
//
// Header-only and free of the animation module, so the walk's tests check the arithmetic with no
// device and no rig file.

#include "fly_camera.h"

#include <core/base/types.h>
#include <core/math/math.h>

#include <algorithm>
#include <cmath>

namespace engine::view {

// The pivots, in the order they turn: the trunk, then the left leg's hip, knee and ankle, then the
// right's.
inline constexpr u32 k_crouch_pivots = 7;

// A body's crouch, measured once: each pivot's joint (-1 where the rig has none) and where it
// stands at rest in the mesh's drawn frame (mesh units; y up, the body facing +z), the eye there,
// the metres a mesh unit is, and the trunk's lean at a full crouch (radians). `layer` false: a rig
// missing a leg's three joints, which stands upright whatever the walker does.
struct CrouchRig {
  bool layer = false;
  i32 joints[k_crouch_pivots] = {-1, -1, -1, -1, -1, -1, -1};
  Vec3 rest[k_crouch_pivots] = {};
  Vec3 eye{};
  f32 scale = 1.0f;
  f32 trunk = 0.0f;
};

// Each pivot's turn about the body's +x and the shift after them (mesh units), and what it
// reached: the hips' drop and the eye's, metres, and the knee's flexion past its rest (the left's).
struct CrouchPose {
  Quat turns[k_crouch_pivots] = {};
  Vec3 shift{};
  bool on = false;
  f32 pelvis_drop_m = 0.0f;
  f32 eye_drop_m = 0.0f;
  f32 knee_rad = 0.0f;
};

// A turn about +x from the cosine and sine of its angle, by the half angle.
inline Quat crouch_turn_x(f32 c, f32 s) noexcept {
  const f32 ch = std::sqrt(std::max(0.0f, 0.5f * (1.0f + c)));
  const f32 sh = std::sqrt(std::max(0.0f, 0.5f * (1.0f - c)));
  return Quat{s < 0.0f ? -sh : sh, 0.0f, 0.0f, ch};
}

// The turn about +x taking the direction of `from` to that of `to`, both read in the sagittal
// plane (z ahead, y up; x is the axis and drops out). A turn about +x by `a` takes (z, y) to
// (z cos a + y sin a, y cos a - z sin a): the plane's counter-clockwise angle is -a.
inline Quat crouch_turn_between(Vec3 from, Vec3 to) noexcept {
  const f32 lf = std::sqrt(from.z * from.z + from.y * from.y);
  const f32 lt = std::sqrt(to.z * to.z + to.y * to.y);
  if (!(lf > 0.0f) || !(lt > 0.0f)) return Quat{};
  const f32 fz = from.z / lf;
  const f32 fy = from.y / lf;
  const f32 tz = to.z / lt;
  const f32 ty = to.y / lt;
  return crouch_turn_x(fz * tz + fy * ty, -(fz * ty - fy * tz));
}

// For an eye lowered `eye_drop_m` and `amount` of the way to a full crouch (walk.h,
// `Walker::crouch_amount`). Off (`out.on` false) at no drop, or for a rig without the layer.
inline void crouch_pose(const CrouchRig& rig, f32 eye_drop_m, f32 amount,
                        CrouchPose& out) noexcept {
  out = CrouchPose{};
  if (!rig.layer || !(eye_drop_m > 0.0f) || !(rig.scale > 0.0f)) return;
  const f32 a = amount < 0.0f ? 0.0f : amount > 1.0f ? 1.0f : amount;
  const f32 drop = eye_drop_m / rig.scale;  // mesh units
  // The trunk forward about its pivot, and what that does to the eye.
  f32 s = 0.0f;
  f32 c = 1.0f;
  fly_sin_cos(a * rig.trunk, s, c);
  f32 lean_drop = 0.0f;
  f32 lean_forward = 0.0f;
  if (rig.joints[0] >= 0) {
    out.turns[0] = crouch_turn_x(c, s);
    const Vec3 e = rig.eye - rig.rest[0];
    lean_drop = e.y - (e.y * c - e.z * s);
    lean_forward = (e.y * s + e.z * c) - e.z;
  }
  // The hips take the rest of the eye's drop and move back half of the lean's lead.
  const f32 d = std::max(0.0f, drop - lean_drop);
  const f32 back = 0.5f * lean_forward;
  Vec3 shift{};
  for (u32 leg = 0; leg < 2; ++leg) {
    const Vec3 h = rig.rest[1 + 3 * leg];
    const Vec3 k = rig.rest[2 + 3 * leg];
    const Vec3 an = rig.rest[3 + 3 * leg];
    const Vec3 thigh = k - h;
    const Vec3 shin = an - k;
    const f32 l1 = std::sqrt(thigh.y * thigh.y + thigh.z * thigh.z);
    const f32 l2 = std::sqrt(shin.y * shin.y + shin.z * shin.z);
    // Where the ankle goes from the hip, in the plane: as far ahead as it was plus the hips' move
    // back, and `d` nearer; held inside what the two bones reach.
    f32 tz = (an.z - h.z) + back;
    f32 ty = (an.y - h.y) + d;
    f32 r = std::sqrt(tz * tz + ty * ty);
    if (!(r > 0.0f) || !(l1 > 0.0f) || !(l2 > 0.0f)) return;
    const f32 most = 0.999f * (l1 + l2);
    const f32 least = 1.001f * std::fabs(l1 - l2) + 1.0e-6f;
    const f32 reach = r > most ? most : r < least ? least : r;
    tz = tz * reach / r;
    ty = ty * reach / r;
    r = reach;
    // The knee off the hip-to-ankle line by the law of cosines, ahead of it: the line's direction
    // turned counter-clockwise in the plane (from pointing down, toward +z).
    const f32 cphi = (l1 * l1 + r * r - l2 * l2) / (2.0f * l1 * r);
    const f32 sphi = std::sqrt(std::max(0.0f, 1.0f - cphi * cphi));
    const f32 uz = tz / r;
    const f32 uy = ty / r;
    const Vec3 knee_to{0.0f, l1 * (uz * sphi + uy * cphi), l1 * (uz * cphi - uy * sphi)};
    const Vec3 ankle_to{0.0f, ty, tz};
    const Quat hip = crouch_turn_between(thigh, knee_to);
    const Quat knee = crouch_turn_between(rotate(hip, shin), ankle_to - knee_to);
    // The sole as it was: the foot undoes the two turns above it (all three about +x, so they
    // commute and the inverse is the conjugate).
    const Quat both = knee * hip;
    out.turns[1 + 3 * leg] = hip;
    out.turns[2 + 3 * leg] = knee;
    out.turns[3 + 3 * leg] = Quat{-both.x, 0.0f, 0.0f, both.w};
    // What returns this ankle to where it stood, half from each leg.
    shift = shift + Vec3{0.0f, (an.y - h.y) - ty, (an.z - h.z) - tz} * 0.5f;
    if (leg == 0) {
      // The knee's flexion past its rest, for the summary: each angle between the bones by
      // `fly_atan2` from its cosine.
      const Vec3 bent = ankle_to - knee_to;
      const f32 cr = (thigh.z * shin.z + thigh.y * shin.y) / (l1 * l2);
      const f32 cb = (knee_to.z * bent.z + knee_to.y * bent.y) / (l1 * l2);
      out.knee_rad = fly_atan2(std::sqrt(std::max(0.0f, 1.0f - cb * cb)), cb) -
                     fly_atan2(std::sqrt(std::max(0.0f, 1.0f - cr * cr)), cr);
    }
  }
  out.shift = shift;
  out.on = true;
  out.pelvis_drop_m = -shift.y * rig.scale;
  out.eye_drop_m = (lean_drop - shift.y) * rig.scale;
}

}  // namespace engine::view
