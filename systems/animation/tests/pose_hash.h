#pragma once

// One number for "how a clip poses this skeleton": the local pose's three channels and the
// skinning matrices built from them, at each of `times`, through `core/hash`'s `hash_bytes`
// (deterministic across platforms). Two loads that pose identically give the same number; a pinned
// number says a fixture still poses exactly as it did. The bytes are the floats' own, so `-0` and
// `+0` differ — which is the point: "identically" means to the bit.

#include <core/hash/hash.h>
#include <domain/anim/clip.h>
#include <domain/anim/skeleton.h>
#include <systems/animation/library.h>

#include <initializer_list>
#include <span>

namespace engine::animation::test_fixture {

inline u64 pose_hash(const Library& library, u32 skeleton_index, u32 clip_index,
                     std::initializer_list<f32> times) {
  const anim::Skeleton& skeleton = library.skeleton_data(skeleton_index);
  const u32 joints = skeleton.joint_count();
  anim::Pose pose;
  Vector<Mat4> model(joints, Mat4::identity());
  Vector<anim::JointMatrix> matrices(joints, anim::JointMatrix{});
  u64 hash = hash_bytes(&joints, sizeof(joints));
  for (const f32 time : times) {
    anim::rest_pose(skeleton, pose);
    if (clip_index != Library::k_not_found) library.clip_data(clip_index).sample(time, pose, true);
    anim::local_to_model(skeleton, pose, std::span<Mat4>(model.data(), model.size()));
    anim::skinning_matrices(
        std::span<const Mat4>(model.data(), model.size()),
        std::span<const Mat4>(skeleton.inverse_bind.data(), skeleton.inverse_bind.size()),
        std::span<anim::JointMatrix>(matrices.data(), matrices.size()));
    hash = hash_bytes(pose.translation.data(), sizeof(Vec3) * joints, hash);
    hash = hash_bytes(pose.rotation.data(), sizeof(Quat) * joints, hash);
    hash = hash_bytes(pose.scale.data(), sizeof(Vec3) * joints, hash);
    hash = hash_bytes(matrices.data(), sizeof(anim::JointMatrix) * joints, hash);
  }
  return hash;
}

}  // namespace engine::animation::test_fixture
