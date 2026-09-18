#include <domain/anim/skeleton.h>

#include <cmath>

namespace engine::anim {

namespace {

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

f32 clamp01(f32 t) noexcept { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); }

}  // namespace

i32 Skeleton::find(std::string_view name) const noexcept {
  for (u32 i = 0; i < names.size(); ++i) {
    if (names[i] == name) return static_cast<i32>(i);
  }
  return k_no_joint;
}

void Skeleton::resize(u32 joints) {
  parents.assign(joints, k_no_joint);
  local_bind.assign(joints, Transform3::identity());
  inverse_bind.assign(joints, Mat4::identity());
  names.clear();
  names.resize(joints);
}

bool Skeleton::validate(std::string* error) const {
  const u32 joints = parents.size();
  if (local_bind.size() != joints || inverse_bind.size() != joints || names.size() != joints) {
    return fail(error, "skeleton: the joint arrays are not the same length (" +
                           std::to_string(joints) + " parents, " +
                           std::to_string(local_bind.size()) + " bind transforms, " +
                           std::to_string(inverse_bind.size()) + " inverse binds, " +
                           std::to_string(names.size()) + " names)");
  }
  for (u32 i = 0; i < joints; ++i) {
    const i32 parent = parents[i];
    if (parent == k_no_joint) continue;
    if (parent < 0 || static_cast<u32>(parent) >= joints) {
      return fail(error, "skeleton: joint " + std::to_string(i) + " names parent " +
                             std::to_string(parent) + ", which is not a joint");
    }
    // Before, not merely different: `local_to_model` is one forward pass, and a parent that came
    // later would be composed against a stale matrix rather than failing visibly.
    if (static_cast<u32>(parent) >= i) {
      return fail(error, "skeleton: joint " + std::to_string(i) + "'s parent " +
                             std::to_string(parent) + " does not come before it");
    }
  }
  return true;
}

void compute_inverse_bind(Skeleton& skeleton) {
  const u32 joints = skeleton.joint_count();
  skeleton.inverse_bind.assign(joints, Mat4::identity());
  Vector<Mat4> model(joints, Mat4::identity());
  for (u32 i = 0; i < joints; ++i) {
    const Mat4 local = mat4_from_transform(skeleton.local_bind[i]);
    const i32 parent = skeleton.parents[i];
    model[i] = parent == k_no_joint ? local : model[static_cast<u32>(parent)] * local;
    skeleton.inverse_bind[i] = inverse(model[i]);
  }
}

void Pose::resize(u32 joints) {
  translation.assign(joints, Vec3{});
  rotation.assign(joints, Quat::identity());
  scale.assign(joints, Vec3::one());
}

// The `Pose&` forms size the output and then do the work on views, so there is exactly one copy
// of each loop and the owning form cannot drift from the pooled one.

void rest_pose(const Skeleton& skeleton, Pose& out) {
  out.resize(skeleton.joint_count());
  rest_pose(skeleton, PoseView{out});
}

void rest_pose(const Skeleton& skeleton, PoseView out) {
  const u32 joints = skeleton.joint_count();
  if (!out.consistent() || out.joint_count() != joints) return;
  for (u32 i = 0; i < joints; ++i)
    out.translation[i] = skeleton.local_bind[i].position;
  for (u32 i = 0; i < joints; ++i)
    out.rotation[i] = skeleton.local_bind[i].rotation;
  for (u32 i = 0; i < joints; ++i)
    out.scale[i] = skeleton.local_bind[i].scale;
}

void blend(const Pose& a, const Pose& b, f32 t, Pose& out) {
  const u32 joints = a.joint_count();
  if (b.joint_count() != joints) return;  // two rigs wired together: leave `out` alone
  if (out.joint_count() != joints) out.resize(joints);
  blend(ConstPoseView{a}, ConstPoseView{b}, t, PoseView{out});
}

void blend(ConstPoseView a, ConstPoseView b, f32 t, PoseView out) {
  const u32 joints = a.joint_count();
  if (b.joint_count() != joints || out.joint_count() != joints) return;
  if (!a.consistent() || !b.consistent() || !out.consistent()) return;
  const f32 k = clamp01(t);
  // One channel at a time, which is the whole reason a pose is three arrays: this loop is three
  // streaming passes with no strided reads and nothing loaded that is not used.
  for (u32 i = 0; i < joints; ++i)
    out.translation[i] = a.translation[i] + (b.translation[i] - a.translation[i]) * k;
  for (u32 i = 0; i < joints; ++i)
    out.rotation[i] = slerp(a.rotation[i], b.rotation[i], k);
  for (u32 i = 0; i < joints; ++i)
    out.scale[i] = a.scale[i] + (b.scale[i] - a.scale[i]) * k;
}

void make_additive(const Pose& pose, const Pose& reference, Pose& out) {
  const u32 joints = pose.joint_count();
  if (reference.joint_count() != joints) return;
  if (out.joint_count() != joints) out.resize(joints);
  make_additive(ConstPoseView{pose}, ConstPoseView{reference}, PoseView{out});
}

void make_additive(ConstPoseView pose, ConstPoseView reference, PoseView out) {
  const u32 joints = pose.joint_count();
  if (reference.joint_count() != joints || out.joint_count() != joints) return;
  if (!pose.consistent() || !reference.consistent() || !out.consistent()) return;
  for (u32 i = 0; i < joints; ++i)
    out.translation[i] = pose.translation[i] - reference.translation[i];
  for (u32 i = 0; i < joints; ++i)
    out.rotation[i] = normalize(conjugate(reference.rotation[i]) * pose.rotation[i]);
  for (u32 i = 0; i < joints; ++i)
    out.scale[i] = pose.scale[i] - reference.scale[i];
}

void blend_additive(const Pose& base, const Pose& additive, f32 weight, Pose& out) {
  const u32 joints = base.joint_count();
  if (additive.joint_count() != joints) return;
  if (out.joint_count() != joints) out.resize(joints);
  blend_additive(ConstPoseView{base}, ConstPoseView{additive}, weight, PoseView{out});
}

void blend_additive(ConstPoseView base, ConstPoseView additive, f32 weight, PoseView out) {
  const u32 joints = base.joint_count();
  if (additive.joint_count() != joints || out.joint_count() != joints) return;
  if (!base.consistent() || !additive.consistent() || !out.consistent()) return;
  const f32 k = clamp01(weight);
  for (u32 i = 0; i < joints; ++i)
    out.translation[i] = base.translation[i] + additive.translation[i] * k;
  for (u32 i = 0; i < joints; ++i) {
    // On the right: an additive layer is expressed in the joint's own frame, so an aim or a lean
    // turns the bone the way the animator saw it whatever the base pose did to the parent.
    out.rotation[i] =
        normalize(base.rotation[i] * slerp(Quat::identity(), additive.rotation[i], k));
  }
  for (u32 i = 0; i < joints; ++i)
    out.scale[i] = base.scale[i] + additive.scale[i] * k;
}

void local_to_model(const Skeleton& skeleton, ConstPoseView pose, std::span<Mat4> out) {
  const u32 joints = skeleton.joint_count();
  if (pose.joint_count() != joints || out.size() != joints || !pose.consistent()) return;
  for (u32 i = 0; i < joints; ++i) {
    const Transform3 local{pose.translation[i], pose.rotation[i], pose.scale[i]};
    const Mat4 matrix = mat4_from_transform(local);
    const i32 parent = skeleton.parents[i];
    // One forward pass: `validate` guarantees the parent is already done.
    out[i] = parent == k_no_joint ? matrix : out[static_cast<u32>(parent)] * matrix;
  }
}

JointMatrix joint_matrix(const Mat4& m) noexcept {
  JointMatrix out;
  for (u32 row = 0; row < 3; ++row)
    out.rows[row] = Vec4{m.at(row, 0), m.at(row, 1), m.at(row, 2), m.at(row, 3)};
  return out;
}

Mat4 mat4_from_joint(const JointMatrix& m) noexcept {
  Mat4 out;
  for (u32 row = 0; row < 3; ++row) {
    for (u32 col = 0; col < 4; ++col)
      out.at(row, col) = m.rows[row][col];
  }
  out.c[0].w = 0.0f;
  out.c[1].w = 0.0f;
  out.c[2].w = 0.0f;
  out.c[3].w = 1.0f;
  return out;
}

Vec3 transform_point(const JointMatrix& m, Vec3 p) noexcept {
  const Vec4 point{p, 1.0f};
  return Vec3{dot(m.rows[0], point), dot(m.rows[1], point), dot(m.rows[2], point)};
}

void skinning_matrices(std::span<const Mat4> model, std::span<const Mat4> inverse_bind,
                       std::span<JointMatrix> out) {
  if (model.size() != inverse_bind.size() || out.size() != model.size()) return;
  for (usize i = 0; i < model.size(); ++i)
    out[i] = joint_matrix(model[i] * inverse_bind[i]);
}

void skin_positions(std::span<const Vec3> positions,
                    std::span<const geometry::SkinBinding> bindings,
                    std::span<const JointMatrix> matrices, std::span<Vec3> out) {
  if (bindings.size() != positions.size() || out.size() != positions.size()) return;
  const u32 joints = static_cast<u32>(matrices.size());
  for (usize v = 0; v < positions.size(); ++v) {
    const Vec3 rest = positions[v];
    const geometry::SkinBinding& binding = bindings[v];
    Vec3 moved{};
    for (u32 k = 0; k < 4; ++k) {
      const u32 weight = binding.weights[k];
      if (weight == 0) continue;  // also what makes a stale joint index on a dead influence safe
      const u32 joint = binding.joints[k];
      if (joint >= joints) continue;  // bad data deforms short rather than reading out of bounds
      moved = moved + transform_point(matrices[joint], rest) * (static_cast<f32>(weight) / 255.0f);
    }
    out[v] = moved;
  }
}

}  // namespace engine::anim
