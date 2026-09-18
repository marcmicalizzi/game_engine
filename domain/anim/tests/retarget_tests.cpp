// The standard skeleton's first cut (docs/plan/05-simulation.md §5.11): mapping a rig's joints
// onto the canonical roles by name, and moving a pose from one rig to another through per-joint
// offsets. The round trip — retargeting a skeleton onto itself — is the property that pins the
// arithmetic, because it is the one case where the answer is known exactly.
#include <domain/anim/clip.h>
#include <domain/anim/standard_skeleton.h>

#include <doctest/doctest.h>

#include <cmath>
#include <string>

using namespace engine;
using namespace engine::anim;

namespace {

struct JointSpec {
  const char* name;
  i32 parent;
  Vec3 offset;
};

// A humanoid chain, deliberately small: hips, a spine, a head, and one leg. `prefix` and `scale`
// are what make the second rig a different rig — a different exporter's naming and a different
// set of proportions.
Skeleton humanoid(const char* prefix, f32 scale, bool short_names) {
  const JointSpec k_long[] = {
      {"Hips", k_no_joint, Vec3{0.0f, 1.0f, 0.0f}}, {"Spine", 0, Vec3{0.0f, 0.2f, 0.0f}},
      {"Chest", 1, Vec3{0.0f, 0.2f, 0.0f}},         {"Neck", 2, Vec3{0.0f, 0.2f, 0.0f}},
      {"Head", 3, Vec3{0.0f, 0.1f, 0.0f}},          {"LeftUpLeg", 0, Vec3{0.1f, -0.1f, 0.0f}},
      {"LeftLeg", 5, Vec3{0.0f, -0.4f, 0.0f}},      {"LeftFoot", 6, Vec3{0.0f, -0.4f, 0.0f}}};
  const JointSpec k_short[] = {{"pelvis", k_no_joint, Vec3{0.0f, 1.0f, 0.0f}},
                               {"spine_01", 0, Vec3{0.0f, 0.2f, 0.0f}},
                               {"spine_02", 1, Vec3{0.0f, 0.2f, 0.0f}},
                               {"neck_01", 2, Vec3{0.0f, 0.2f, 0.0f}},
                               {"head", 3, Vec3{0.0f, 0.1f, 0.0f}},
                               {"thigh_l", 0, Vec3{0.1f, -0.1f, 0.0f}},
                               {"calf_l", 5, Vec3{0.0f, -0.4f, 0.0f}},
                               {"foot_l", 6, Vec3{0.0f, -0.4f, 0.0f}}};
  const JointSpec* spec = short_names ? k_short : k_long;

  Skeleton skeleton;
  skeleton.resize(8);
  for (u32 i = 0; i < 8; ++i) {
    skeleton.names[i] = std::string(prefix) + spec[i].name;
    skeleton.parents[i] = spec[i].parent;
    skeleton.local_bind[i].position = spec[i].offset * scale;
  }
  compute_inverse_bind(skeleton);
  return skeleton;
}

bool near(Vec3 a, Vec3 b, f32 eps = 1.0e-5f) {
  return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}

bool same_rotation(Quat a, Quat b, f32 eps = 1.0e-5f) {
  return std::fabs(std::fabs(dot(a, b)) - 1.0f) < eps;  // q and -q are the same rotation
}

}  // namespace

TEST_CASE("standard skeleton: the canonical names, and mapping a rig onto them") {
  CHECK(std::string(standard_joint_name(StandardJoint::LeftUpperArm)) == "LeftUpperArm");
  CHECK(standard_joint_from_name("Hips") == StandardJoint::Hips);
  CHECK(standard_joint_from_name("hips") == StandardJoint::Count);  // the file spelling is exact

  // A Mixamo-shaped rig: the prefix, the colon, and the exporter's own spellings all normalize
  // away, so the roles are found without anything being renamed on the way in.
  const Skeleton mixamo = humanoid("mixamorig:", 1.0f, /*short_names=*/false);
  const JointMapping mapped = map_joints(mixamo);
  CHECK(mapped[StandardJoint::Hips] == 0);
  CHECK(mapped[StandardJoint::Spine] == 1);
  CHECK(mapped[StandardJoint::Chest] == 2);
  CHECK(mapped[StandardJoint::Neck] == 3);
  CHECK(mapped[StandardJoint::Head] == 4);
  CHECK(mapped[StandardJoint::LeftUpperLeg] == 5);
  CHECK(mapped[StandardJoint::LeftLowerLeg] == 6);
  CHECK(mapped[StandardJoint::LeftFoot] == 7);
  CHECK(mapped[StandardJoint::RightFoot] == k_no_joint);  // the rig has no right leg
  CHECK(mapped.mapped == 8);

  // A completely different naming convention reaches the same roles.
  const Skeleton other = humanoid("", 1.0f, /*short_names=*/true);
  const JointMapping other_mapped = map_joints(other);
  CHECK(other_mapped[StandardJoint::Hips] == 0);
  CHECK(other_mapped[StandardJoint::LeftUpperLeg] == 5);
  CHECK(other_mapped[StandardJoint::LeftLowerLeg] == 6);
  CHECK(other_mapped[StandardJoint::LeftFoot] == 7);
  CHECK(other_mapped[StandardJoint::Head] == 4);

  // One skeleton joint plays at most one role: a finger named after the hand cannot take the
  // hand's place, which is what the exact-before-substring passes are for.
  Skeleton hands;
  hands.resize(2);
  hands.names[0] = "LeftHandIndex1";
  hands.names[1] = "LeftHand";
  hands.parents[1] = k_no_joint;
  const JointMapping hand_map = map_joints(hands);
  CHECK(hand_map[StandardJoint::LeftHand] == 1);
}

TEST_CASE("standard skeleton: retargeting a rig onto itself reproduces the pose exactly") {
  const Skeleton rig = humanoid("mixamorig:", 1.0f, /*short_names=*/false);
  Retarget retarget;
  std::string error;
  REQUIRE_MESSAGE(build_retarget(rig, rig, retarget, &error), error);
  CHECK(retarget.mapped == 8);
  CHECK(retarget.translation_scale == doctest::Approx(1.0f));

  // A pose with something to say on every channel.
  Pose source;
  rest_pose(rig, source);
  source.translation[0] = Vec3{0.3f, 1.1f, -0.2f};
  source.rotation[1] = quat_from_axis_angle(Vec3::unit_x(), radians(20.0f));
  source.rotation[5] = quat_from_axis_angle(Vec3::unit_z(), radians(-35.0f));
  source.scale[4] = Vec3{1.2f, 1.2f, 1.2f};

  Pose target;
  rest_pose(rig, target);
  retarget_pose(retarget, source, target);
  // Identity in, identity out: the offsets cancel, which is the statement that the bind-difference
  // form is the right one and that nothing is being applied twice.
  for (u32 j = 0; j < rig.joint_count(); ++j) {
    CHECK(near(target.translation[j], source.translation[j]));
    CHECK(same_rotation(target.rotation[j], source.rotation[j]));
    CHECK(near(target.scale[j], source.scale[j]));
  }

  // And through a clip, which is the shape the plan describes: sample on the source, write on
  // the target.
  Clip clip;
  const f32 times[2] = {0.0f, 1.0f};
  const f32 hips[6] = {0, 1, 0, 0.5f, 1, 0};
  REQUIRE(clip.add_track(0, k_channel_translation, k_interp_linear, 3, times, hips));
  Pose sampled;
  rest_pose(rig, sampled);
  clip.sample(0.5f, sampled);
  Pose written;
  rest_pose(rig, written);
  retarget_pose(retarget, sampled, written);
  CHECK(near(written.translation[0], Vec3{0.25f, 1.0f, 0.0f}));
}

TEST_CASE("standard skeleton: a different rig takes the pose through per-joint offsets") {
  const Skeleton source_rig = humanoid("mixamorig:", 1.0f, /*short_names=*/false);
  // Twice as tall, differently named, and with a different bind orientation on the spine — the
  // three ways a second rig actually differs.
  Skeleton target_rig = humanoid("", 2.0f, /*short_names=*/true);
  target_rig.local_bind[1].rotation = quat_from_axis_angle(Vec3::unit_x(), radians(15.0f));
  compute_inverse_bind(target_rig);

  Retarget retarget;
  std::string error;
  REQUIRE_MESSAGE(build_retarget(source_rig, target_rig, retarget, &error), error);
  CHECK(retarget.mapped == 8);
  // Hips at 1.0 against hips at 2.0: root motion authored for the short rig moves the tall one
  // twice as far, so it covers the same fraction of a stride.
  CHECK(retarget.translation_scale == doctest::Approx(2.0f));

  Pose source;
  rest_pose(source_rig, source);
  source.translation[0] = source.translation[0] + Vec3{0.5f, 0.0f, 0.0f};
  const Quat lean = quat_from_axis_angle(Vec3::unit_x(), radians(20.0f));
  source.rotation[1] = lean;

  Pose target;
  rest_pose(target_rig, target);
  retarget_pose(retarget, source, target);
  // The hips' deviation from bind (half a unit along x) is scaled by the proportion and applied
  // to the target's own bind position.
  CHECK(near(target.translation[0], target_rig.local_bind[0].position + Vec3{1.0f, 0.0f, 0.0f}));
  // The spine took the source's *deviation from its own bind*, composed onto the target's bind
  // orientation: a 20 degree lean on top of the target's 15, not 20 degrees in absolute terms.
  const Quat expected = normalize(target_rig.local_bind[1].rotation * lean);
  CHECK(same_rotation(target.rotation[1], expected));
  CHECK_FALSE(same_rotation(target.rotation[1], lean));

  // A joint nothing maps keeps its bind pose, so the result is always a complete pose.
  Skeleton extra = target_rig;
  extra.resize(9);
  extra = humanoid("", 2.0f, /*short_names=*/true);
  extra.names[3] = "some_helper_bone";  // no longer a neck: nothing maps to it
  compute_inverse_bind(extra);
  Retarget partial;
  REQUIRE(build_retarget(source_rig, extra, partial, &error));
  CHECK(partial.mapped == 7);
  Pose partial_pose;
  rest_pose(extra, partial_pose);
  Pose out;
  rest_pose(extra, out);
  retarget_pose(partial, source, out);
  CHECK(near(out.translation[3], extra.local_bind[3].position));
  CHECK(same_rotation(out.rotation[3], extra.local_bind[3].rotation));

  // Two skeletons that share no canonical joint are a content error, not a silent bind pose.
  Skeleton nameless;
  nameless.resize(3);
  nameless.parents[1] = 0;
  nameless.parents[2] = 1;
  Retarget refused;
  CHECK_FALSE(build_retarget(source_rig, nameless, refused, &error));
  CHECK(error.find("canonical") != std::string::npos);
}
