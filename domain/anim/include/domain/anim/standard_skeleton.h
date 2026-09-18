#pragma once

// The standard skeleton (docs/plan/05-simulation.md §5.11): "retargeting via a **standard
// skeleton** with per-character offsets — this is what makes generated characters tractable".
//
// The idea the plan is buying. A game whose characters are generated cannot have one rig per
// character and one clip per rig: the clip library has to be authored once, against a skeleton
// that exists only as a naming convention, and every character has to be able to play it. So a
// character declares which of *its* joints plays each canonical role, and the difference between
// its proportions and the clip's is absorbed by a per-joint offset rather than by re-authoring.
//
// **This is a first cut, and the header says exactly how far it goes.** What is here: a fixed,
// named humanoid joint set; `map_joints`, which finds a skeleton's joint for each canonical role
// by name; and `build_retarget`/`retarget_pose`, which apply per-joint offset transforms so that
// a clip sampled on one skeleton can be written onto another. What is *not* here is listed under
// "What this does not do yet" below, and none of it is hidden behind a plausible-looking API —
// a retarget that silently does not do foot locking is worse than one that says so.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/anim/skeleton.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::anim {

// The canonical roles. A humanoid core: the spine chain, the head, two arms, two legs. Fingers,
// toes beyond the base, twist joints, and a face are deliberately absent from the *first* cut —
// adding a role is appending to this enum and its name table, and nothing stored on disk names a
// role by number, so the set can grow without a migration.
enum class StandardJoint : u32 {
  Root = 0,
  Hips,
  Spine,
  Chest,
  UpperChest,
  Neck,
  Head,
  LeftShoulder,
  LeftUpperArm,
  LeftLowerArm,
  LeftHand,
  RightShoulder,
  RightUpperArm,
  RightLowerArm,
  RightHand,
  LeftUpperLeg,
  LeftLowerLeg,
  LeftFoot,
  LeftToes,
  RightUpperLeg,
  RightLowerLeg,
  RightFoot,
  RightToes,
  Count,
};

inline constexpr u32 k_standard_joint_count = static_cast<u32>(StandardJoint::Count);

// "Hips", "LeftUpperArm", ... — the canonical spelling, for diagnostics and for an authored
// mapping file. Out-of-range values give "".
const char* standard_joint_name(StandardJoint joint) noexcept;
// The canonical name, or Count when nothing matches. Exact, case-sensitive: this is the spelling
// a mapping file uses, not a guess at an exporter's.
StandardJoint standard_joint_from_name(std::string_view name) noexcept;

// Which of a skeleton's joints plays each canonical role. `to_skeleton[role]` is the joint index
// or `k_no_joint`; `mapped` counts the roles that were found.
struct JointMapping {
  Vector<i32> to_skeleton;  // k_standard_joint_count entries
  u32 mapped = 0;

  i32 operator[](StandardJoint joint) const noexcept {
    const u32 index = static_cast<u32>(joint);
    return index < to_skeleton.size() ? to_skeleton[index] : k_no_joint;
  }
};

// Maps a skeleton's joints onto the canonical roles **by name**, which is the only signal every
// exporter agrees on. Names are normalized (lower-cased, everything but letters and digits
// dropped), so `mixamorig:LeftForeArm`, `Left_Fore_Arm`, and `leftforearm` are one name, and each
// role carries a table of the spellings the common rigs use. Matching runs in three passes —
// exact, then suffix, then substring — and a skeleton joint is claimed by at most one role, so
// `LeftHandIndex1` cannot take `LeftHand`'s place when the real hand is in the skeleton.
//
// It is a heuristic and it is meant to be replaceable: a character whose rig this cannot read
// will eventually carry an authored mapping, which is why `JointMapping` is a plain array a file
// can fill instead of calling this at all.
JointMapping map_joints(const Skeleton& skeleton);

// One target joint's share of a retarget: which source joint drives it and the offsets that turn
// the source's local pose into this skeleton's.
struct RetargetJoint {
  i32 source = k_no_joint;  // the source skeleton's joint, or k_no_joint: hold the bind pose
  // The inverse of the source joint's bind rotation. `source_bind_inverse * source_local` is the
  // clip's **deviation from its own bind pose**, which is the quantity that transfers.
  Quat source_bind_inverse = Quat::identity();
  Vec3 source_bind_translation{};  // likewise for translation
  Transform3 bind;                 // this joint's own bind local transform: the deviation's base
};

// The per-joint offsets of [05 §5.11], built once per (source, target) pair.
struct Retarget {
  Vector<RetargetJoint> joints;  // one per **target** joint
  // Target bind hip height / source bind hip height. Root motion authored for a 1.8 m character
  // would make a 1.2 m one skate; scaling the translation deviation by the proportion is the
  // cheapest correction that is right in the common case.
  f32 translation_scale = 1.0f;
  u32 mapped = 0;  // target joints actually driven by the source
};

// Pairs the two skeletons through the canonical roles and fills the offsets. Fails, with a
// sentence, when either skeleton does not validate or when no role maps on both sides — a
// retarget that would produce the target's bind pose for every input is a content error worth
// reporting, not a silent no-op.
bool build_retarget(const Skeleton& source, const Skeleton& target, Retarget& out,
                    std::string* error = nullptr);

// Writes the source pose onto the target through the offsets. `out` must already hold the
// target's joint count; joints with no source keep their bind transform, so the result is a
// complete pose whatever the mapping covered.
//
//   rotation:    bind.rotation * (source_bind_inverse * source.rotation)
//   translation: bind.position + (source.translation - source_bind_translation) * scale
//   scale:       the source's, unchanged
//
// The rotation form is the classic bind-difference retarget: what transfers between two rigs is
// how far a joint has turned **from its own rest**, not where it points in its parent's frame,
// because the two rests differ by exactly the modelling decisions a retarget is meant to absorb.
// It is exact when the two skeletons are the same one, which is what the round-trip test pins.
void retarget_pose(const Retarget& retarget, const Pose& source_pose, Pose& out);

// ---- What this does not do yet ---------------------------------------------------------------
//
// Every one of these is a deliberate omission rather than an oversight, and the next person to
// work on the standard skeleton should start here.
//
//  * **No IK, so no foot or hand locking.** A retargeted walk on a character with different leg
//    proportions slides, because scaling the hip translation corrects the stride but not the
//    contact. Foot IK is the fix ([05 §5.11], [05 §5.15]'s "Procedural animation and IK" row),
//    and it needs a contact channel on the clip that nothing authors yet.
//  * **No bone-length compensation on the limbs.** Only the root's translation deviation is
//    scaled; a joint that is not the root takes the source's translation deviation as-is, which
//    is right for a rig whose limbs translate only at the root (nearly all of them) and wrong for
//    a stretchy one.
//  * **No twist distribution**, so a forearm-twist joint on the target that the source does not
//    have keeps its bind rotation and the wrist shears.
//  * **No joint limits and no pose validation.** A mapping that pairs a hinge with a ball joint
//    produces whatever the arithmetic gives.
//  * **Name matching only**, with no authored mapping file and no pose-based inference; the
//    canonical set has no fingers, no toe joints past the base, and no face.
//  * **The offsets are rotations and translations, not a full basis change.** Two rigs whose
//    rest orientations differ by more than their bind poses account for — a T-pose against an
//    A-pose is fine; a bone axis convention that differs by 90° is not — need a per-joint
//    correction this does not compute.

}  // namespace engine::anim
