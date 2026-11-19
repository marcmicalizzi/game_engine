#pragma once

// Inertialization: roadmap S35 stage 4 (docs/experiments/animation-graphs.md, "Stages, in build
// order"; what is built: animation.md, "Graphs: inertialization"). A transition that cuts to its
// target at once and hides the cut under an offset that decays to zero, instead of evaluating the
// source and the target together for the length of a cross-fade (Bollo, GDC 2018: "Inertialization:
// High-Performance Animation Transitions in Gears of War"; the sources as read, and what was not
// read, are on the experiment page, "Stage 4's curve, read and derived").
//
// At the transition's tick the offset between the source and the target is recorded per joint —
// the translation's and the rotation's (as an axis times an angle), and the velocity of each from
// the two poses one tick back (`inertial_capture`). From then on only the target is evaluated, and
// each joint's offset is added to it along a quintic in time that starts at the offset with its
// velocity and reaches zero with zero velocity and acceleration at the duration
// (`inertial_curve`, `inertial_apply`). Scale is not inertialized: it is the target's at once.
//
// **The curve** (`inertial_curve`), per channel, on the offset's magnitude `x0 >= 0` along its own
// direction (a vector's direction, a rotation's axis, fixed for the transition) and the velocity
// `v0` along it:
//
//   x(t) = A t^5 + B t^4 + C t^3 + a0/2 t^2 + v0 t + x0,   x, x', x'' = 0 at t1,
//
// with three rules, which are the guard against overshoot: a velocity away from zero is dropped
// (`v0 > 0` becomes 0); `t1` is shortened to `-5 x0 / v0` when that is sooner, which is exactly
// the duration at which the curve below would first touch zero before its end; and `a0`, chosen so
// the jerk is zero at `t1` too — `a0 = (-8 v0 t1 - 20 x0) / t1^2` — is taken as 0 when negative.
// Under them `x` never crosses zero (it is `(1 - s)^4 (x0 (1 + 4 s) + v0 t1 s)` in `s = t / t1`
// with the zero-jerk `a0`, and `(1 - s)^3 (x0 (1 + 3 s + 6 s^2) + v0 t1 s (1 + 3 s))` with
// `a0 = 0`; the experiment page derives both), and at and past `t1` it is exactly 0.
//
// **Determinism**: portable to the bit. The curve is `+ - * /` in f64; a rotation's angle comes
// from `portable_atan2` and goes back through `portable_sin_cos` (core/math/portable_trig.h), never
// the C library's; a length is `std::sqrt`, correctly rounded.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/anim/skeleton.h>

#include <memory>
#include <span>

namespace engine::animation {

// The offset of a channel at `t` seconds into a transition of `t1` seconds that starts at `x0`
// moving at `v0` (units a second), under the three rules above. 0 at and past `t1`, and for a
// duration that is not positive or an argument that is not finite.
f32 inertial_curve(f32 x0, f32 v0, f32 t, f32 t1) noexcept;

// One joint's offset while a transition is in flight: the translation's offset and its velocity,
// and the rotation's offset as an axis times an angle (radians, in [0, pi]) and its angular
// velocity, all source minus target. 48 bytes.
struct InertialJoint {
  Vec3 dx;
  Vec3 dv;
  Vec3 dr;
  Vec3 dw;
};

// The offsets of a transition at its tick: `source` and `target` are the two states' poses this
// tick, `source_prev` and `target_prev` the same one tick (`dt` seconds) back. `out` holds one
// entry a joint. A source that is itself an inertialized pose (a transition interrupting one in
// flight) is passed as it was shown, offset included, so the new offset carries the old one on.
void inertial_capture(anim::ConstPoseView source, anim::ConstPoseView source_prev,
                      anim::ConstPoseView target, anim::ConstPoseView target_prev, f32 dt,
                      std::span<InertialJoint> out) noexcept;

// Adds the offsets at `t` seconds of `duration` to `pose` (the target's): translation added along
// its direction, rotation turned about its axis on the left (`offset * target`). At and past the
// duration nothing is written, so the pose is the target's to the bit.
void inertial_apply(std::span<const InertialJoint> offsets, f32 t, f32 duration,
                    anim::PoseView pose) noexcept;

// The slots transitions in flight hold, one arena a skeleton: `joints` entries a slot, handed out
// last-in first-out like the pose pool's, and held only while a transition is in flight — a crowd
// holds a slot per instance changing state, not per instance. Slots live in blocks of 16 that
// never move, so a slot's entries stay where they are while other slots are handed out (the
// graph's systems run on several workers; `acquire` and `release` are the caller's to serialize,
// and happen only when a transition starts and ends). Nothing is allocated until the first
// acquire; after that a block when the arena grows past its most, and never in steady state.
//
// **Stated limit** (ADR-0017): 65,536 slots an arena, a skeleton's transitions in flight at once;
// `acquire` answers `k_none` past it and the transition is a cut.
class InertialArena {
 public:
  static constexpr u32 k_none = 0xFFFF'FFFFu;
  static constexpr u32 k_block_slots = 16;
  static constexpr u32 k_max_slots = 65536;

  explicit InertialArena(u32 joints = 0) noexcept : joints_(joints) {}
  u32 joints() const noexcept { return joints_; }
  // A slot of `joints()` entries, its contents unspecified until a capture writes them; `k_none`
  // past the limit.
  u32 acquire();
  void release(u32 slot) noexcept;
  std::span<InertialJoint> slot(u32 slot) noexcept {
    return {blocks_[slot / k_block_slots].get() + usize{slot % k_block_slots} * joints_, joints_};
  }
  u32 held() const noexcept { return slots_ - free_.size(); }
  usize bytes() const noexcept;

 private:
  u32 joints_ = 0;
  u32 slots_ = 0;
  // `k_max_slots / k_block_slots` block pointers, allocated at the first acquire.
  std::unique_ptr<std::unique_ptr<InertialJoint[]>[]> blocks_;
  Vector<u32> free_;  // released slots, the last released handed out first
};

}  // namespace engine::animation
