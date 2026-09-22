#include <core/base/macros.h>
#include <domain/anim/skeleton.h>

#include <cmath>

// --- the affine matrix product, in 128-bit registers ---------------------------------------------
//
// SSE2 is guaranteed by x86-64 itself, so this is not a dispatch and there is no scalar path to
// choose between at run time: the `#else` below exists for a non-x86 build, which
// `cmake/EngineCpuBaseline.cmake` already contemplates and nothing here has ever been compiled
// for. The two produce the same bits (see `affine_column`).
#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__) || defined(__SSE2__)
#define ENGINE_ANIM_SSE 1
#include <emmintrin.h>
#else
#define ENGINE_ANIM_SSE 0
#endif

namespace engine::anim {

namespace {

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

f32 clamp01(f32 t) noexcept { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); }

#if ENGINE_ANIM_SSE

// One column of `a * b`: `a.c0 * b[j].x + a.c1 * b[j].y + a.c2 * b[j].z + a.c3 * b[j].w`, summed
// **left to right**, which is the association `core/math`'s `operator*(const Mat4&, Vec4)` has —
// `((c0*x + c1*y) + c2*z) + c3*w`. Every lane therefore sees the same three additions in the same
// order as the scalar code it replaces, so this is not an approximation of the old result, it is
// the same float — wherever the compiler evaluates what is written. Two tests pin that:
// `local_to_model` and `skinning_matrices` are checked against `Mat4 operator*` in
// `anim_tests.cpp`, exactly on MSVC and on every build without FMA. GCC and clang at x86-64-v3
// fuse multiply-adds on their own (GCC across these intrinsics too, since its `_mm_add_ps` is a
// plain vector `+`), and fuse different ones on the two sides, so there the test allows 8 ulp.
ENGINE_FORCE_INLINE __m128 affine_column(__m128 a0, __m128 a1, __m128 a2, __m128 a3,
                                         __m128 b) noexcept {
  __m128 r = _mm_mul_ps(a0, _mm_shuffle_ps(b, b, _MM_SHUFFLE(0, 0, 0, 0)));
  r = _mm_add_ps(r, _mm_mul_ps(a1, _mm_shuffle_ps(b, b, _MM_SHUFFLE(1, 1, 1, 1))));
  r = _mm_add_ps(r, _mm_mul_ps(a2, _mm_shuffle_ps(b, b, _MM_SHUFFLE(2, 2, 2, 2))));
  r = _mm_add_ps(r, _mm_mul_ps(a3, _mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 3, 3, 3))));
  return r;
}

// `Mat4` is four `Vec4` columns of four `f32`, so a column is sixteen contiguous bytes and
// `Mat4::data()` already promises the tree that. Unaligned loads and stores because nothing
// aligns a `Mat4` to 16 — `Vec4`'s alignment is 4 — and on every part this engine targets
// `movups` on an address that happens to be aligned costs what `movaps` costs.
ENGINE_FORCE_INLINE void load_columns(const Mat4& m, __m128& c0, __m128& c1, __m128& c2,
                                      __m128& c3) noexcept {
  const f32* p = m.data();
  c0 = _mm_loadu_ps(p);
  c1 = _mm_loadu_ps(p + 4);
  c2 = _mm_loadu_ps(p + 8);
  c3 = _mm_loadu_ps(p + 12);
}

#endif  // ENGINE_ANIM_SSE

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

// **Why this loop is written with intrinsics** (ADR-0031's one open regression; anim.md has the
// numbers). The plain `out[parent] * matrix` above compiled, under MSVC with `/arch:AVX2`, to the
// sixteen scalar products written one `vmovss` at a time into a stack temporary, which was then
// read back with two **32-byte** `vmovups` and stored to `out[i]` as two more. That is a
// store-forwarding stall twice per joint: a load wider than the stores feeding it cannot forward
// and has to wait for them to reach L1. Worse, the next joint reads `out[parent]` back out of
// that array, and a 32-byte store cannot forward to the narrower load that reads it either — and
// in a skeleton every joint but the root reads a matrix written a few instructions earlier, so
// the loop's whole critical path runs through memory. The x86-64-v2 build of the same source
// escaped it by accident, using 16-byte moves throughout, which is why the regression looked like
// "AVX2 made the kernel slower" when the arithmetic never widened at all: both builds were
// scalar, and v3's was 25% *fewer* instructions.
//
// Doing the product in 128-bit registers and storing four 16-byte columns fixes both halves and
// takes the codegen out of the compiler's hands, so the next compiler cannot rediscover it.
void local_to_model(const Skeleton& skeleton, ConstPoseView pose, std::span<Mat4> out) {
  const u32 joints = skeleton.joint_count();
  if (pose.joint_count() != joints || out.size() != joints || !pose.consistent()) return;
  for (u32 i = 0; i < joints; ++i) {
    const Transform3 local{pose.translation[i], pose.rotation[i], pose.scale[i]};
    const Mat4 matrix = mat4_from_transform(local);
    const i32 parent = skeleton.parents[i];
    // One forward pass: `validate` guarantees the parent is already done.
    //
    // A root's copy is left as an assignment on purpose. MSVC compiles it to one 256-bit `vmovups`
    // pair at `/arch:AVX2`, which looks like the very thing this kernel exists to avoid — so it
    // was **tried** as four explicit 16-byte stores, and measured: no change either way (453 ns
    // against 444 ns for the whole 23-joint loop, inside the session-to-session spread), because
    // one joint in twenty-three is a root and a store that nothing reads back in the same
    // iteration does not stall. The complexity is not worth a number that is not there; recorded
    // so the next reader does not retry it. anim.md has the table.
    if (parent == k_no_joint) {
      out[i] = matrix;
      continue;
    }
#if ENGINE_ANIM_SSE
    __m128 a0, a1, a2, a3;
    load_columns(out[static_cast<u32>(parent)], a0, a1, a2, a3);
    const f32* b = matrix.data();
    f32* dst = &out[i].c[0].x;
    _mm_storeu_ps(dst, affine_column(a0, a1, a2, a3, _mm_loadu_ps(b)));
    _mm_storeu_ps(dst + 4, affine_column(a0, a1, a2, a3, _mm_loadu_ps(b + 4)));
    _mm_storeu_ps(dst + 8, affine_column(a0, a1, a2, a3, _mm_loadu_ps(b + 8)));
    _mm_storeu_ps(dst + 12, affine_column(a0, a1, a2, a3, _mm_loadu_ps(b + 12)));
#else
    out[i] = out[static_cast<u32>(parent)] * matrix;
#endif
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

// The 3x4 is built **directly**, rather than as a 4x4 whose fourth row is then dropped. The
// product's four columns are transposed in registers and only three of the four resulting rows
// are stored, so the row the header explains is always (0, 0, 0, 1) is never written anywhere —
// which is the difference between three 16-byte stores a joint and four plus a 48-byte copy out
// of a stack temporary. `joint_matrix` is pure data movement, so this is the same bits by
// construction, and the same association in the arithmetic as `affine_column` promises.
void skinning_matrices(std::span<const Mat4> model, std::span<const Mat4> inverse_bind,
                       std::span<JointMatrix> out) {
  if (model.size() != inverse_bind.size() || out.size() != model.size()) return;
  for (usize i = 0; i < model.size(); ++i) {
#if ENGINE_ANIM_SSE
    __m128 a0, a1, a2, a3;
    load_columns(model[i], a0, a1, a2, a3);
    const f32* b = inverse_bind[i].data();
    const __m128 c0 = affine_column(a0, a1, a2, a3, _mm_loadu_ps(b));
    const __m128 c1 = affine_column(a0, a1, a2, a3, _mm_loadu_ps(b + 4));
    const __m128 c2 = affine_column(a0, a1, a2, a3, _mm_loadu_ps(b + 8));
    const __m128 c3 = affine_column(a0, a1, a2, a3, _mm_loadu_ps(b + 12));
    // Columns to rows, written out rather than `_MM_TRANSPOSE4_PS` so that the fourth row's two
    // instructions are not emitted at all and the warning policy never has to meet a macro's
    // insides. t0/t2 interleave columns 0 and 1, t1/t3 columns 2 and 3; the halves then combine.
    const __m128 t0 = _mm_unpacklo_ps(c0, c1);  // c0.x c1.x c0.y c1.y
    const __m128 t1 = _mm_unpacklo_ps(c2, c3);  // c2.x c3.x c2.y c3.y
    const __m128 t2 = _mm_unpackhi_ps(c0, c1);  // c0.z c1.z c0.w c1.w
    const __m128 t3 = _mm_unpackhi_ps(c2, c3);  // c2.z c3.z c2.w c3.w
    f32* dst = &out[i].rows[0].x;
    _mm_storeu_ps(dst, _mm_movelh_ps(t0, t1));      // row 0
    _mm_storeu_ps(dst + 4, _mm_movehl_ps(t1, t0));  // row 1
    _mm_storeu_ps(dst + 8, _mm_movelh_ps(t2, t3));  // row 2
#else
    out[i] = joint_matrix(model[i] * inverse_bind[i]);
#endif
  }
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
