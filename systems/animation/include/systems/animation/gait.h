#pragma once

// **A procedural gait** (docs/subsystems/animation.md, "The procedural gait"; roadmap C60): a walk
// and a run generated for any skeleton the standard-skeleton mapping can read, from normative gait
// data rather than from a capture. It is the fallback an authored clip later replaces: every rig
// in the 23 roles gets a walk and a run at load, and nothing about it is stored.
//
// **What it is.** One looping cycle (heel strike to heel strike of the right foot), sampled at
// `k_gait_key_hz` over the cycle, **in place**: no forward root translation, only the pelvis's
// vertical and lateral excursion. Each driven joint's rotation is a short Fourier series of the
// gait cycle's phase, fitted to the published mean curve (the table in gait.cpp, a citation per
// row; the fits' residuals are on docs/experiments/gait-grounding-2026-11-04.md), built about the
// **body's** axes — the lateral axis (the body's right) for flexion, the vertical for rotation, the
// forward for obliquity — and brought into the joint's rest frame through its rest orientation in
// the skeleton's model space, so the clip is a deviation from the rest pose: an A-pose rig swings
// its arms from the A. The body's axes are read from the rig itself (the legs' joints: up from the
// feet to the hips, right from the left hip to the right one), so a rig that stands up only through
// a transformed ancestor still swings about its own axes. The left side is the right half a cycle
// later; the arms swing against the legs of their own side.
//
// **What it is not.** Amplitudes are fixed per band (walk, run) at the band's reference speed; the
// speed sets only the stride and the cycle's duration. No foot IK (the feet do not slide only
// because a caller drives the playhead by distance: phase = distance over `stride_m`), no
// stochastic variation, no turning adjustment, no jump or crouch. Foot IK is plan 05 §5.15's
// "Procedural animation and IK" row.
//
// **Deterministic, not bit-portable.** One fixed-step sampling and one operation order, so two runs
// on one machine give the same bytes. The curves are evaluated in double through the C library's
// `std::sin` and `std::cos`, which are not correctly rounded and may differ in the last bit of a
// double between C libraries; after the conversion to the clip's floats that very rarely moves a
// value, and when it does, by one ulp. So no test pins a literal hash of a generated clip: poses
// are held to a double-precision reference with a tolerance, and determinism to two runs.

#include <core/base/types.h>
#include <domain/anim/standard_skeleton.h>
#include <systems/animation/library.h>

#include <string>

namespace engine::animation {

enum class GaitBand : u8 { Walk = 0, Run = 1 };

// Keys a second over the cycle (the last key is the first again, so the clip loops exactly).
inline constexpr f32 k_gait_key_hz = 60.0f;

// The bands' reference speeds, metres a second: a natural walk (Perry & Burnfield 2010, free
// walking velocity 82 m/min) and an easy run (Novacheck 1998's running condition, 3.2 m/s).
inline constexpr f32 k_gait_walk_reference_mps = 1.37f;
inline constexpr f32 k_gait_run_reference_mps = 3.2f;
// The walk-to-run transition (Thorstensson & Roberthson 1987: 1.88-2.2 m/s with leg length; a
// mean near 2.0). A caller choosing a band by speed uses it; engine-view's player uses the sprint
// flag instead, whose speed (5 m/s by default) is well past it.
inline constexpr f32 k_gait_transition_mps = 2.0f;

struct GaitParams {
  GaitBand band = GaitBand::Walk;
  // The speed the cycle is timed for; 0 is the band's reference. It sets the stride and the clip's
  // duration (`gait_stride_m`), never the angles.
  f32 speed_mps = 0.0f;
  // Metres per unit of the skeleton's model space (the scale a host stands the rig at).
  f32 metres_per_unit = 1.0f;
  // The leg length (greater trochanter height), metres; 0 measures it from the joints: the upper
  // legs' height above the feet along the body's up, over (0.530 - 0.039) / 0.530 — Winter's
  // anthropometric proportions (Biomechanics and Motor Control of Human Movement, 4th ed., Fig.
  // 4.1), hip 0.530 and ankle 0.039 of the stature.
  f32 leg_length_m = 0.0f;
};

// What the generated cycle is: everything a caller needs to drive its playhead by distance.
struct GaitCycle {
  GaitBand band = GaitBand::Walk;
  f32 speed_mps = 0.0f;
  f32 leg_length_m = 0.0f;
  f32 stride_m = 0.0f;  // one cycle: two steps
  f32 cycle_s = 0.0f;   // the clip's duration, stride over speed
  f32 cadence_spm = 0.0f;  // steps a minute
  f32 stance = 0.0f;       // the fraction of the cycle a foot is on the ground (toe-off's phase)
  u32 keys = 0;            // keys per track (the last repeats the first)
  u32 joints_driven = 0;
};

// **The stride as a function of speed and leg length**, metres a cycle.
//  * Walk: Grieve's power law (Grieve 1968, step length proportional to speed^0.42), made
//    dimensionless over the leg (stride/L = A (v / sqrt(g L))^0.42, the Froude form of dynamic
//    similarity) and calibrated at Perry & Burnfield's free walk: 1.41 m at 1.37 m/s for a
//    0.93 m leg (0.530 of a 1.75 m stature).
//  * Run: a stride frequency of 1.40 Hz for a 0.93 m leg, nearly independent of speed over the
//    distance-running range (Cavanagh & Kram 1989), scaled by sqrt(0.93 / L) (the pendulum's
//    frequency); stride = speed over it.
// `speed_mps` is clamped to the band's range (walk 0.4-2.6, run 1.8-10).
f32 gait_stride_m(GaitBand band, f32 speed_mps, f32 leg_length_m) noexcept;

// The roles the gait needs (both legs: upper, lower, foot, and the hips); a rig without one of
// them is refused with a sentence naming the ones missing. Arms, spine and chest are driven when
// mapped.
bool gait_roles_present(const anim::JointMapping& mapping, std::string& missing);

// **The mapping the gait plays through**: `by_name` (the library's `anim::map_joints`) where it
// holds the hips and both legs, else `by_name` completed from the rig's rest-pose geometry — a rig
// whose joints are named for their chain and not their role (RiggedFigure's `torso_joint_1`,
// `leg_joint_R_2`) still stands on two legs. The hips are the first joint (parents first) with two
// children whose chains of three or more joints run parallel and away from a third (the spine), or
// downward in model space when there is no third; the leg on the model's -x side is the right
// (glTF's front faces +z, so the body's right is -x); each chain is upper leg, lower leg, foot,
// toes. The spine and chest follow the third child's single-child chain, and where that chain
// branches into two side chains with a joint each side of it they are the arms (upper, lower,
// hand). Only roles `by_name` left empty are filled; `inferred` counts them.
anim::JointMapping gait_mapping(const anim::Skeleton& skeleton, const anim::JointMapping& by_name,
                                u32* inferred = nullptr);

// The leg length in metres measured from the rig's rest pose (see `GaitParams::leg_length_m`);
// 0 when the legs are not mapped.
f32 gait_leg_length_m(const anim::Skeleton& skeleton, const anim::JointMapping& mapping,
                      f32 metres_per_unit);

// **Generates one cycle** of `params.band` for `skeleton` through `mapping` into `out.clip`
// (`out.name`, `out.id`, `out.skeleton` are the caller's: a library name, its id, the skeleton's
// index). False, with a sentence, for a rig missing a leg role or with a degenerate leg (feet at
// the hips' height, both hips at one point).
bool generate_gait_clip(const SkeletonAsset& skeleton, const anim::JointMapping& mapping,
                        const GaitParams& params, ClipAsset& out, GaitCycle& cycle,
                        std::string& error);

// **The published curves, evaluated** (degrees, at `phase` in [0, 1) from the right heel strike):
// what the clip's rotations are built from, exposed so a test can hold the clip to them and a
// reader can plot them. Sagittal angles are flexion positive (the ankle: dorsiflexion positive),
// the knee's flexion bends the shank backward; the pelvis's are the right side's (rotation: the
// right side forward; obliquity: the right side up; tilt: anterior), the trunk's its rotation
// relative to the pelvis. Translations are the pelvis's excursion in metres (up; the body's right).
enum class GaitCurve : u32 {
  Hip = 0,
  Knee,
  Ankle,
  Shoulder,
  Elbow,
  PelvisRotation,
  PelvisObliquity,
  PelvisTilt,
  TrunkRotation,
  PelvisRise,     // metres
  PelvisLateral,  // metres
  Count,
};
f64 gait_curve(GaitBand band, GaitCurve curve, f64 phase) noexcept;

}  // namespace engine::animation
