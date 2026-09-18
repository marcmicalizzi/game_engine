#pragma once

// Skeletons and the poses that ride on them (docs/plan/05-simulation.md §5.11): "own system with
// SoA poses, blend trees and state machines as data, retargeting via a standard skeleton with
// per-character offsets, IK for feet and hands, motion matching later". This header is the first
// two of those: the skeleton, the pose, the blends over a pose, and the matrices a skinned mesh
// is deformed by.
//
// **Everything here is structure of arrays.** A pose is three parallel arrays — translations,
// rotations, scales — rather than an array of `Transform3`, because that is what the work over a
// pose looks like: a blend tree's weighted average touches one channel at a time, a rotation-only
// additive layer never loads a translation, and a clip whose tracks are all rotations (most of
// them are) writes one contiguous array. The array-of-structs form makes every one of those
// read forty bytes to use twelve. Nothing here allocates per joint: a `Pose` is three `Vector`s
// sized once, and every function below writes into storage the caller already owns.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/geometry/cluster.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::anim {

// A joint that has no parent.
inline constexpr i32 k_no_joint = -1;

// A skeleton: the joint hierarchy, its bind pose, and the inverse bind matrices that turn an
// animated joint's model transform into a skinning matrix. SoA for the same reason a pose is,
// and because the only whole-skeleton operation — composing local transforms into model ones —
// walks `parents` and `local_bind` together and touches nothing else.
//
// **A joint's parent always comes before it**, so `local_to_model` is one forward pass with no
// stack and no recursion. `assets::load_gltf` already sorts its node array that way;
// `validate` refuses a skeleton that is not.
struct Skeleton {
  Vector<i32> parents;            // k_no_joint for a root
  Vector<Transform3> local_bind;  // the bind pose, in the parent's frame
  Vector<Mat4> inverse_bind;      // model space -> the joint's space at bind
  Vector<std::string> names;      // parallel; a joint may be unnamed

  u32 joint_count() const noexcept { return parents.size(); }
  // Linear: a skeleton is a hundred joints and a name lookup happens at load, not per frame.
  i32 find(std::string_view name) const noexcept;
  // Sizes every array and leaves the joints at the identity with no parent.
  void resize(u32 joints);
  // Parallel arrays, parents in range and before their joint, no cycles. Returns false with a
  // sentence in `error`.
  bool validate(std::string* error = nullptr) const;
};

// Fills `inverse_bind` from `parents` and `local_bind`: the inverse of each joint's model-space
// bind transform. A source that carries its own inverse bind matrices (glTF usually does) keeps
// those instead — they are authored data and may disagree with the node transforms, and where
// they do, the file is right about how the mesh was bound.
void compute_inverse_bind(Skeleton& skeleton);

// A pose: one local transform per joint, in the parent's frame, as three parallel arrays. The
// arrays are always the same length; `resize` is the only way to change it.
struct Pose {
  Vector<Vec3> translation;
  Vector<Quat> rotation;
  Vector<Vec3> scale;

  u32 joint_count() const noexcept { return rotation.size(); }
  void resize(u32 joints);
  bool matches(const Skeleton& skeleton) const noexcept {
    return joint_count() == skeleton.joint_count();
  }
};

// The skeleton's bind pose as a `Pose`. A clip writes only the joints its tracks name, so this
// is what a caller fills a pose with before sampling: everything the clip does not animate then
// holds the bind value rather than whatever was in the buffer.
void rest_pose(const Skeleton& skeleton, Pose& out);

// Linear interpolation from `a` to `b` at `t`, clamped to 0..1: lerp for translation and scale,
// slerp on the short arc for rotation. `out` may alias `a` or `b`. Mismatched lengths leave
// `out` untouched, which is the one case a blend tree can hit by wiring two rigs together.
void blend(const Pose& a, const Pose& b, f32 t, Pose& out);

// An additive layer: `additive` is a **difference** from its own reference pose, applied on top
// of `base` with `weight`. Translation and scale add (scaled by the weight), rotation composes
// on the right — `base.rotation * slerp(identity, additive.rotation, weight)` — which is the
// order that makes an additive aim or lean rotate the joint in its own frame rather than in its
// parent's. `make_additive` is how the difference is built in the first place.
void blend_additive(const Pose& base, const Pose& additive, f32 weight, Pose& out);

// The difference `pose` − `reference`, in the form `blend_additive` consumes.
void make_additive(const Pose& pose, const Pose& reference, Pose& out);

// Composes a pose's local transforms into model space, one forward pass: out[j] = out[parent] *
// local[j], with a root's own transform. `out` must hold `skeleton.joint_count()` matrices.
void local_to_model(const Skeleton& skeleton, const Pose& pose, std::span<Mat4> out);

// A skinning matrix: the affine 3x4 of `model * inverse_bind`, stored as three `float4` **rows**.
// 48 bytes, GPU-mirrored (deform.slang's `JointMatrix`), pinned by the size table.
//
// **Why 3x4 and not 4x4.** A skinning matrix is always affine: the fourth row of the 4x4 is
// (0, 0, 0, 1) for every joint of every pose, because it is a product of rigid transforms and
// scales. Storing it costs a quarter of the array — 16 bytes a joint, 1.6 KB for a hundred-joint
// character, and this array is uploaded *per instance per frame*, which is exactly the kind of
// bandwidth docs/plan/11-performance-principles.md §11.2 says to count. The shader
// pays nothing for the choice: skinning a position is three dot products with `float4(p, 1)`
// either way, and the row-major layout is what makes those three dots three contiguous loads.
// The cost is that a `JointMatrix` cannot be multiplied by another one without reconstructing
// the fourth row, which nothing downstream of the skinning does — the composition happens in
// `local_to_model`, in `Mat4`, before the conversion.
struct JointMatrix {
  Vec4 rows[3] = {Vec4{1.0f, 0.0f, 0.0f, 0.0f}, Vec4{0.0f, 1.0f, 0.0f, 0.0f},
                  Vec4{0.0f, 0.0f, 1.0f, 0.0f}};
};
static_assert(sizeof(JointMatrix) == 48, "JointMatrix is a 48-byte GPU-mirrored record");

// The 3x4 form of an affine matrix; the fourth row is dropped, not checked.
JointMatrix joint_matrix(const Mat4& m) noexcept;
// The 4x4 it came from, with (0, 0, 0, 1) as the fourth row. For tests and CPU reference code.
Mat4 mat4_from_joint(const JointMatrix& m) noexcept;
// Transforms a point by a 3x4: three dot products, exactly as the shader does it.
Vec3 transform_point(const JointMatrix& m, Vec3 p) noexcept;

// out[j] = model[j] * inverse_bind[j], as 3x4. The three spans must be the same length; a
// mismatch writes nothing.
void skinning_matrices(std::span<const Mat4> model, std::span<const Mat4> inverse_bind,
                       std::span<JointMatrix> out);

// The CPU reference for the skinning mode of `domain/gfx/shaders/deform.slang`: linear blend
// skinning of one vertex stream, four influences a vertex, weights out of 255.
//
//     out[v] = sum over k of (weights[k] / 255) * (matrices[joints[k]] * positions[v])
//
// It is here, in the module that owns the pose, rather than beside the shader, because it is the
// definition of what the shader has to reproduce and because the two GPU tests that check it are
// not the only callers a CPU skinning path will ever have (a collision proxy, a footstep query,
// a headless server that never builds a device). An influence whose weight is zero is skipped
// outright, which is also what makes a joint index out of range on a zero-weight influence
// harmless; a non-zero influence naming a joint past the end of `matrices` is skipped too, so
// bad data deforms a vertex short rather than reading out of bounds.
//
// This is where the crack rule lands for skinning: the result is a function of the rest position
// and the binding alone, both of which every copy of a surface point shares byte for byte (see
// geometry.md, "Skinned meshes"), so skinning is position-only and cannot tear.
void skin_positions(std::span<const Vec3> positions,
                    std::span<const geometry::SkinBinding> bindings,
                    std::span<const JointMatrix> matrices, std::span<Vec3> out);

}  // namespace engine::anim
