// Skeletons, poses, clips, and the CPU skinning reference (docs/plan/05-simulation.md §5.11).
// The sampler cases check each of the three glTF interpolation modes against values worked out
// by hand rather than against another implementation, because the sampler *is* the definition.
#include <domain/anim/clip.h>
#include <domain/anim/skeleton.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace engine;
using namespace engine::anim;

namespace {

// Two joints: a root at the origin and a tip one unit up it, which is the smallest skeleton on
// which every piece here says something.
Skeleton two_bone() {
  Skeleton skeleton;
  skeleton.resize(2);
  skeleton.names[0] = "joint_root";
  skeleton.names[1] = "joint_tip";
  skeleton.parents[0] = k_no_joint;
  skeleton.parents[1] = 0;
  skeleton.local_bind[1].position = Vec3{0.0f, 1.0f, 0.0f};
  compute_inverse_bind(skeleton);
  return skeleton;
}

bool near(Vec3 a, Vec3 b, f32 eps = 1.0e-5f) {
  return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}

// Whether this build evaluates a float expression the way it is written. MSVC's /fp:precise never
// fuses `a*b + c` into one rounding, and an instruction set without FMA cannot; GCC fuses by
// default (-ffp-contract=fast, which it keeps for C++ even at -std=c++20) and clang within one
// expression, and at x86-64-v3 both have the instruction. A fused and an unfused sum of the same
// products differ in the last bits, and the two sides compared below — the kernels' SSE
// intrinsics and core/math's scalar operator* — are spelled differently enough that GCC fuses
// different multiplies in each (docs/ci/local-linux.md, "What the first v3 runs found").
#if (defined(_MSC_VER) && !defined(__clang__)) || !defined(__FMA__)
constexpr bool k_evaluates_as_written = true;
#else
constexpr bool k_evaluates_as_written = false;
#endif

// How far apart a contracting build may leave them: 8 ulp of max(1, |x|). Every entry is a
// four-term dot product of unit-scale rotations and translations under 2, fusing moves each of its
// three additions by at most half an ulp of a partial sum, and the chain below is five joints deep;
// GCC 13 at x86-64-v3 measured 2 ulp for local_to_model and 3 for skinning_matrices. A transposed
// lane, a wrong parent or a dropped term is wrong by a hundredth or more, which is five orders of
// magnitude past this. Where the build evaluates as written, the bound is zero and every entry
// must be the same float.
constexpr f64 k_contraction_ulps = k_evaluates_as_written ? 0.0 : 8.0;

struct FloatAgreement {
  u32 count = 0;
  u32 differ = 0;
  f64 worst_ulps = 0.0;

  void add(f32 got, f32 want) {
    ++count;
    if (got == want) return;
    ++differ;
    const f64 ulp = 1.1920928955078125e-7 * std::max(1.0, std::fabs(static_cast<f64>(want)));
    const f64 apart = std::fabs(static_cast<f64>(got) - static_cast<f64>(want));
    worst_ulps = std::max(worst_ulps, apart / ulp);
  }
  u32 allowed_differ() const { return k_evaluates_as_written ? 0u : count; }
};

}  // namespace

TEST_CASE("anim skeleton: the bind pose, its inverses, and the model transforms it composes to") {
  const Skeleton skeleton = two_bone();
  std::string error;
  CHECK_MESSAGE(skeleton.validate(&error), error);
  CHECK(skeleton.joint_count() == 2);
  CHECK(skeleton.find("joint_tip") == 1);
  CHECK(skeleton.find("nothing") == k_no_joint);

  // The inverse bind takes a point from model space into the joint's own space at bind, so the
  // tip's undoes its one unit of height.
  CHECK(near(skeleton.inverse_bind[0].c[3].xyz(), Vec3{}));
  CHECK(near(skeleton.inverse_bind[1].c[3].xyz(), Vec3{0.0f, -1.0f, 0.0f}));

  Pose pose;
  rest_pose(skeleton, pose);
  REQUIRE(pose.matches(skeleton));
  CHECK(near(pose.translation[1], Vec3{0.0f, 1.0f, 0.0f}));

  Vector<Mat4> model(2, Mat4::identity());
  local_to_model(skeleton, pose, std::span<Mat4>(model.data(), model.size()));
  CHECK(near(model[1].c[3].xyz(), Vec3{0.0f, 1.0f, 0.0f}));

  // At bind, every skinning matrix is the identity: that is what "bind pose" means, and it is
  // the invariant a wrong inverse bind breaks first.
  Vector<JointMatrix> matrices(2, JointMatrix{});
  skinning_matrices(std::span<const Mat4>(model.data(), model.size()),
                    std::span<const Mat4>(skeleton.inverse_bind.data(), 2),
                    std::span<JointMatrix>(matrices.data(), 2));
  for (u32 j = 0; j < 2; ++j) {
    const Vec3 probe{0.3f, 1.7f, -0.4f};
    CHECK(near(transform_point(matrices[j], probe), probe));
  }

  // Rotating the tip a quarter turn about +z: its model transform turns with it, and the
  // skinning matrix is the one that maps a bound vertex to where the bone took it.
  pose.rotation[1] = quat_from_axis_angle(Vec3::unit_z(), radians(90.0f));
  local_to_model(skeleton, pose, std::span<Mat4>(model.data(), model.size()));
  skinning_matrices(std::span<const Mat4>(model.data(), model.size()),
                    std::span<const Mat4>(skeleton.inverse_bind.data(), 2),
                    std::span<JointMatrix>(matrices.data(), 2));
  // The bar's top-left corner, bound entirely to the tip: (-0.5, 2, 0) is one unit above the
  // joint and half a unit to its left, so a quarter turn about +z puts it at (-1, 0.5, 0).
  CHECK(near(transform_point(matrices[1], Vec3{-0.5f, 2.0f, 0.0f}), Vec3{-1.0f, 0.5f, 0.0f}));
  CHECK(near(transform_point(matrices[0], Vec3{-0.5f, 0.0f, 0.0f}), Vec3{-0.5f, 0.0f, 0.0f}));

  // The 3x4 drops a row that is always (0, 0, 0, 1), and nothing else.
  const Mat4 rebuilt = mat4_from_joint(matrices[1]);
  const Mat4 full = model[1] * skeleton.inverse_bind[1];
  for (u32 row = 0; row < 3; ++row) {
    for (u32 col = 0; col < 4; ++col)
      CHECK(rebuilt.at(row, col) == doctest::Approx(full.at(row, col)));
  }
  CHECK(rebuilt.at(3, 3) == 1.0f);
}

// `local_to_model` and `skinning_matrices` are hand-vectorized (skeleton.cpp explains why: the
// plain form compiled to a store-forwarding stall per joint under /arch:AVX2, which is ADR-0031's
// one recorded regression). A kernel rewritten for speed has to prove it did not also change the
// answer, and "close enough" is the wrong standard where the build can do better: the GPU skinning
// test asserts the shader reproduces this to a tolerance, so any drift here spends that budget. So
// this compares against `core/math`'s `Mat4 operator*` — the expression the kernel used to be —
// with `==` wherever the compiler evaluates both as written (MSVC, and every build without FMA),
// and within `k_contraction_ulps` where it fuses multiply-adds of its own accord (GCC and clang at
// x86-64-v3), which is a property of the compiler's choices and not of the kernel.
//
// A chain, a branch and a lone root in one skeleton, none of the transforms axis-aligned, so
// every lane of every column carries a different value and a transposed or swapped one shows.
TEST_CASE("anim skeleton: the vectorized kernels compute the scalar product") {
  constexpr u32 k_joints = 12;
  Skeleton skeleton;
  skeleton.resize(k_joints);
  const i32 parents[k_joints] = {k_no_joint, 0, 1, 2, 3, 1, 5, 0, 7, k_no_joint, 9, 10};
  for (u32 j = 0; j < k_joints; ++j)
    skeleton.parents[j] = parents[j];
  for (u32 j = 0; j < k_joints; ++j) {
    const f32 t = static_cast<f32>(j) * 0.41f + 0.13f;
    skeleton.local_bind[j].position = Vec3{0.31f * t, -0.17f + t, 0.07f * t};
    skeleton.local_bind[j].rotation = normalize(Quat{0.21f * t, 0.13f, -0.37f * t, 0.83f});
    skeleton.local_bind[j].scale = Vec3{1.0f + 0.03f * t, 0.97f, 1.11f - 0.02f * t};
  }
  compute_inverse_bind(skeleton);

  Pose pose;
  rest_pose(skeleton, pose);
  for (u32 j = 0; j < k_joints; ++j) {
    const f32 t = static_cast<f32>(j) * 0.29f;
    pose.rotation[j] = normalize(Quat{-0.11f + t, 0.23f * t, 0.19f, 0.71f - 0.05f * t});
    pose.translation[j] = pose.translation[j] + Vec3{0.02f * t, 0.05f, -0.03f * t};
    pose.scale[j] = Vec3{1.0f + 0.01f * t, 1.0f, 0.99f};
  }

  Vector<Mat4> model(k_joints, Mat4::identity());
  local_to_model(skeleton, pose, std::span<Mat4>(model.data(), model.size()));

  // The reference: the same forward pass written the way the header describes it.
  Vector<Mat4> reference(k_joints, Mat4::identity());
  for (u32 j = 0; j < k_joints; ++j) {
    const Mat4 local =
        mat4_from_transform(Transform3{pose.translation[j], pose.rotation[j], pose.scale[j]});
    reference[j] =
        parents[j] == k_no_joint ? local : reference[static_cast<u32>(parents[j])] * local;
  }
  FloatAgreement composed;
  for (u32 j = 0; j < k_joints; ++j) {
    for (u32 row = 0; row < 4; ++row) {
      for (u32 col = 0; col < 4; ++col)
        composed.add(model[j].at(row, col), reference[j].at(row, col));
    }
  }
  MESSAGE("local_to_model: " << composed.differ << " of " << composed.count
                             << " entries differ from the scalar product, the worst by "
                             << composed.worst_ulps << " ulp");
  CHECK(composed.differ <= composed.allowed_differ());
  CHECK(composed.worst_ulps <= k_contraction_ulps);

  Vector<JointMatrix> matrices(k_joints, JointMatrix{});
  skinning_matrices(std::span<const Mat4>(model.data(), model.size()),
                    std::span<const Mat4>(skeleton.inverse_bind.data(), k_joints),
                    std::span<JointMatrix>(matrices.data(), k_joints));
  FloatAgreement skinning;
  for (u32 j = 0; j < k_joints; ++j) {
    const JointMatrix expected = joint_matrix(reference[j] * skeleton.inverse_bind[j]);
    for (u32 row = 0; row < 3; ++row) {
      for (u32 col = 0; col < 4; ++col)
        skinning.add(matrices[j].rows[row][col], expected.rows[row][col]);
    }
  }
  MESSAGE("skinning_matrices: " << skinning.differ << " of " << skinning.count
                                << " entries differ from the scalar product, the worst by "
                                << skinning.worst_ulps << " ulp");
  CHECK(skinning.differ <= skinning.allowed_differ());
  CHECK(skinning.worst_ulps <= k_contraction_ulps);

  // The fourth row is never written by `skinning_matrices` — it builds the 3x4 directly — so the
  // row it skips is worth looking at once.
  //
  // **It is (0, 0, 0, 1) to floating-point accuracy and not exactly**, and the difference is
  // instructive: the model matrix's fourth row *is* exact, because it is a product of matrices
  // built by `mat4_from_transform`, whose fourth row is the literal (0, 0, 0, 1). The inverse
  // bind's is not, because `compute_inverse_bind` calls `core/math`'s **general** cofactor
  // `inverse()` rather than an affine one, so its bottom row comes out of the same division every
  // other entry does. Row 3 of `A * B` is row 3 of `B` when A is exactly affine, so the product
  // inherits the inverse's error: three of these twelve joints land a few ulp off 1.
  //
  // That is exactly why `joint_matrix` documents the row as "dropped, not checked", and why this
  // kernel is allowed to skip computing it. If an affine inverse ever replaces the general one,
  // this becomes an equality — but it is not one today and a test that claimed it would be wrong.
  for (u32 j = 0; j < k_joints; ++j) {
    const Mat4 full = reference[j] * skeleton.inverse_bind[j];
    CHECK(full.at(3, 0) == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(full.at(3, 1) == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(full.at(3, 2) == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(full.at(3, 3) == doctest::Approx(1.0f).epsilon(1e-5));
  }

  // Running it twice into the same storage gives the same bits: the kernel carries no state and
  // nothing in it depends on where the arrays landed.
  Vector<Mat4> again(k_joints, Mat4::identity());
  local_to_model(skeleton, pose, std::span<Mat4>(again.data(), again.size()));
  for (u32 j = 0; j < k_joints; ++j)
    CHECK(again[j] == model[j]);
}

TEST_CASE("anim skeleton: a parent that does not come before its child is refused") {
  Skeleton skeleton;
  skeleton.resize(2);
  skeleton.parents[0] = 1;  // the root's parent is the joint after it
  skeleton.parents[1] = k_no_joint;
  std::string error;
  CHECK_FALSE(skeleton.validate(&error));
  CHECK(error.find("before") != std::string::npos);

  skeleton.parents[0] = 7;
  CHECK_FALSE(skeleton.validate(&error));
  CHECK(error.find("not a joint") != std::string::npos);

  skeleton.parents[0] = k_no_joint;
  skeleton.names.resize(1);
  CHECK_FALSE(skeleton.validate(&error));
  CHECK(error.find("same length") != std::string::npos);
}

TEST_CASE("anim clip: each sampler mode against values worked out by hand") {
  const Skeleton skeleton = two_bone();
  Clip clip;
  clip.name = "modes";

  // STEP translation on the root: (0,0,0) until 0.5, then (0,2,0).
  const f32 step_times[2] = {0.0f, 0.5f};
  const f32 step_values[6] = {0, 0, 0, 0, 2, 0};
  REQUIRE(clip.add_track(0, k_channel_translation, k_interp_step, 3, step_times, step_values));
  // LINEAR rotation on the tip: the identity, then a quarter turn about +z at t = 1.
  const f32 rotation_times[2] = {0.0f, 1.0f};
  const f32 quarter = 0.70710678f;
  const f32 rotation_values[8] = {0, 0, 0, 1, 0, 0, quarter, quarter};
  REQUIRE(
      clip.add_track(1, k_channel_rotation, k_interp_linear, 4, rotation_times, rotation_values));
  // CUBICSPLINE scale on the tip: 1 -> 2 with zero tangents, which is a smoothstep.
  const f32 scale_times[2] = {0.0f, 1.0f};
  const f32 scale_values[18] = {0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 2, 2, 2, 0, 0, 0};
  REQUIRE(clip.add_track(1, k_channel_scale, k_interp_cubic, 3, scale_times, scale_values));
  std::string error;
  CHECK_MESSAGE(clip.validate(&error), error);
  CHECK(clip.duration == doctest::Approx(1.0f));
  CHECK(clip.joint_count == 2);

  Pose pose;
  rest_pose(skeleton, pose);

  // STEP holds the previous key: the value jumps at 0.5 and never interpolates.
  clip.sample(0.25f, pose);
  CHECK(near(pose.translation[0], Vec3{}));
  clip.sample(0.49f, pose);
  CHECK(near(pose.translation[0], Vec3{}));
  clip.sample(0.5f, pose);
  CHECK(near(pose.translation[0], Vec3{0.0f, 2.0f, 0.0f}));

  // LINEAR on a rotation is a slerp on the short arc, so half of a quarter turn is an eighth of
  // a turn — 22.5 degrees of half-angle, cos(22.5) = 0.9238795 as the quaternion's w.
  clip.sample(0.5f, pose);
  CHECK(pose.rotation[1].w == doctest::Approx(0.92387953f).epsilon(1e-5));
  CHECK(pose.rotation[1].z == doctest::Approx(0.38268343f).epsilon(1e-5));
  const Vec3 turned = rotate(pose.rotation[1], Vec3{1.0f, 0.0f, 0.0f});
  CHECK(turned.x == doctest::Approx(std::cos(radians(45.0f))).epsilon(1e-5));
  CHECK(turned.y == doctest::Approx(std::sin(radians(45.0f))).epsilon(1e-5));

  // CUBICSPLINE with zero tangents is the Hermite basis with m0 = m1 = 0, which at t = 0.5 is
  // 0.5 p0 + 0.5 p1: the scale is 1.5 halfway from 1 to 2, and 1.15625 a quarter of the way
  // (2 t^3 - 3 t^2 + 1 = 0.84375 of p0 plus 0.15625 of p1).
  clip.sample(0.5f, pose);
  CHECK(pose.scale[1].x == doctest::Approx(1.5f).epsilon(1e-5));
  clip.sample(0.25f, pose);
  CHECK(pose.scale[1].y == doctest::Approx(1.15625f).epsilon(1e-5));

  // Looping wraps, holding does not, and a clip held past its end holds its last pose.
  Pose looped;
  rest_pose(skeleton, looped);
  clip.sample(1.25f, looped, /*loop=*/true);
  Pose direct;
  rest_pose(skeleton, direct);
  clip.sample(0.25f, direct, /*loop=*/true);
  CHECK(near(looped.scale[1], direct.scale[1]));
  CHECK(looped.rotation[1].w == doctest::Approx(direct.rotation[1].w));
  clip.sample(-0.25f, looped, /*loop=*/true);  // wraps to 0.75, where the smoothstep is 1.84375
  CHECK(looped.scale[1].x == doctest::Approx(1.84375f).epsilon(1e-5));
  Pose held;
  rest_pose(skeleton, held);
  clip.sample(5.0f, held, /*loop=*/false);
  CHECK(held.scale[1].x == doctest::Approx(2.0f).epsilon(1e-5));
  CHECK(held.rotation[1].w == doctest::Approx(quarter).epsilon(1e-5));

  // The game clock reaches the same sample as the float does: a scheduler-driven layer never has
  // to invent a float of its own.
  Pose by_clock;
  rest_pose(skeleton, by_clock);
  clip.sample(GameTime::from_us(250000), by_clock);
  CHECK(by_clock.scale[1].x == doctest::Approx(direct.scale[1].x).epsilon(1e-5));

  // A clip writes only what it animates: the root's rotation and scale are still the bind pose's,
  // which is why a caller fills the pose with `rest_pose` first.
  CHECK(pose.rotation[0].w == doctest::Approx(1.0f));
  CHECK(near(pose.scale[0], Vec3::one()));
}

TEST_CASE("anim clip: a track that does not add up is refused") {
  Clip clip;
  const f32 times[2] = {0.0f, 1.0f};
  const f32 three[3] = {0, 0, 0};
  CHECK_FALSE(clip.add_track(0, k_channel_translation, k_interp_linear, 3, times, three));
  const f32 six[6] = {0, 0, 0, 1, 1, 1};
  CHECK(clip.add_track(0, k_channel_translation, k_interp_linear, 3, times, six));
  const f32 backwards[2] = {1.0f, 0.0f};
  CHECK_FALSE(clip.add_track(0, k_channel_translation, k_interp_linear, 3, backwards, six));
  CHECK(clip.tracks.size() == 1);

  // A rotation track with three components would be read as a quaternion with a garbage w.
  const Track copy = clip.tracks[0];
  clip.tracks.push_back(copy);
  clip.tracks[1].channel = k_channel_rotation;
  std::string error;
  CHECK_FALSE(clip.validate(&error));
  CHECK(error.find("rotation") != std::string::npos);
}

TEST_CASE("anim clip: morph weight tracks sample beside the pose and write only what they drive") {
  const Skeleton skeleton = two_bone();
  Clip clip;
  clip.joint_count = skeleton.joint_count();
  // One track over two channels — which is what glTF writes, one SCALAR sampler for a whole
  // mesh's targets — starting at channel 1, so channel 0 is one nothing animates.
  const f32 times[3] = {0.0f, 1.0f, 2.0f};
  const f32 weights[6] = {0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.5f};
  REQUIRE(clip.add_weight_track(1, 2, k_interp_linear, times, weights));
  CHECK(clip.morph_count == 3);
  CHECK(clip.duration == doctest::Approx(2.0f));
  // A joint track beside it, so the one walk really does both.
  const f32 translations[6] = {0, 0, 0, 0, 4, 0};
  const f32 two_times[2] = {0.0f, 2.0f};
  REQUIRE(clip.add_track(1, k_channel_translation, k_interp_linear, 3, two_times, translations));
  std::string error;
  REQUIRE_MESSAGE(clip.validate(&error), error);

  Pose pose;
  pose.resize(skeleton.joint_count());
  rest_pose(skeleton, pose);
  f32 played[3] = {0.25f, -1.0f, -1.0f};  // channel 0 is the caller's default and must survive
  clip.sample(0.0f, pose, std::span<f32>(played, 3), false);
  CHECK(played[0] == doctest::Approx(0.25f));
  CHECK(played[1] == doctest::Approx(0.0f));
  CHECK(played[2] == doctest::Approx(1.0f));
  CHECK(near(pose.translation[1], Vec3{0, 0, 0}));

  clip.sample(0.5f, pose, std::span<f32>(played, 3), false);
  CHECK(played[1] == doctest::Approx(0.5f));
  CHECK(played[2] == doctest::Approx(0.5f));
  CHECK(near(pose.translation[1], Vec3{0, 1, 0}));

  clip.sample(2.0f, pose, std::span<f32>(played, 3), false);
  CHECK(played[1] == doctest::Approx(0.0f));
  CHECK(played[2] == doctest::Approx(0.5f));

  // A span shorter than the track takes the part that fits rather than writing past the end.
  f32 narrow[2] = {0.0f, 0.0f};
  clip.sample_weights(1.0f, std::span<f32>(narrow, 2), false);
  CHECK(narrow[0] == doctest::Approx(0.0f));
  CHECK(narrow[1] == doctest::Approx(1.0f));

  // And what the validator refuses.
  Clip bad = clip;
  bad.morph_count = 1;
  CHECK_FALSE(bad.validate(&error));
  CHECK(error.find("morph channels outside") != std::string::npos);
  const f32 short_values[3] = {0, 0, 0};
  CHECK_FALSE(clip.add_weight_track(0, 2, k_interp_linear, times, short_values));
}

TEST_CASE("anim blending: the two ends are exact, and an additive layer round-trips") {
  const Skeleton skeleton = two_bone();
  Pose a;
  Pose b;
  rest_pose(skeleton, a);
  rest_pose(skeleton, b);
  b.translation[0] = Vec3{2.0f, 0.0f, 0.0f};
  b.rotation[1] = quat_from_axis_angle(Vec3::unit_z(), radians(90.0f));
  b.scale[1] = Vec3{2.0f, 2.0f, 2.0f};

  Pose mid;
  blend(a, b, 0.5f, mid);
  CHECK(near(mid.translation[0], Vec3{1.0f, 0.0f, 0.0f}));
  CHECK(near(mid.scale[1], Vec3{1.5f, 1.5f, 1.5f}));
  CHECK(mid.rotation[1].w == doctest::Approx(0.92387953f).epsilon(1e-5));

  // The ends are exact, and a weight outside 0..1 is clamped rather than extrapolated.
  Pose end;
  blend(a, b, 0.0f, end);
  CHECK(near(end.translation[0], a.translation[0]));
  blend(a, b, 1.0f, end);
  CHECK(near(end.translation[0], b.translation[0]));
  blend(a, b, 4.0f, end);
  CHECK(near(end.translation[0], b.translation[0]));

  // An additive layer is a difference from a reference pose, so building the difference and
  // applying it at full weight gives the pose back.
  Pose difference;
  make_additive(b, a, difference);
  Pose applied;
  blend_additive(a, difference, 1.0f, applied);
  CHECK(near(applied.translation[0], b.translation[0]));
  CHECK(near(applied.scale[1], b.scale[1]));
  CHECK(std::fabs(std::fabs(dot(applied.rotation[1], b.rotation[1])) - 1.0f) < 1.0e-5f);
  // At zero weight it is the base pose, untouched.
  blend_additive(a, difference, 0.0f, applied);
  CHECK(near(applied.translation[0], a.translation[0]));
  CHECK(std::fabs(std::fabs(dot(applied.rotation[1], a.rotation[1])) - 1.0f) < 1.0e-5f);

  // Two rigs wired together leave the output alone rather than writing half a pose.
  Pose other;
  other.resize(5);
  Pose untouched;
  untouched.resize(2);
  untouched.translation[0] = Vec3{9.0f, 9.0f, 9.0f};
  blend(a, other, 0.5f, untouched);
  CHECK(near(untouched.translation[0], Vec3{9.0f, 9.0f, 9.0f}));
}

TEST_CASE("anim pose views: an arena is storage a clip samples into, with the same answer") {
  // What `systems/animation`'s pose pool is: three arrays with several instances' joints laid end
  // to end, and a view per instance. The point of the test is that a slot of an arena and a `Pose`
  // of its own are the same bytes, so the pooled form is not a second implementation.
  const Skeleton skeleton = two_bone();
  const u32 joints = skeleton.joint_count();
  constexpr u32 k_slots = 3;

  Vector<Vec3> translation(k_slots * 2, Vec3{});
  Vector<Quat> rotation(k_slots * 2, Quat::identity());
  Vector<Vec3> scale(k_slots * 2, Vec3::one());
  auto slot = [&](u32 index) {
    return PoseView{std::span<Vec3>(translation.data() + index * joints, joints),
                    std::span<Quat>(rotation.data() + index * joints, joints),
                    std::span<Vec3>(scale.data() + index * joints, joints)};
  };

  Clip clip;
  clip.joint_count = joints;
  const f32 times[2] = {0.0f, 1.0f};
  const f32 quarter = 0.70710678f;
  const f32 turns[8] = {0, 0, 0, 1, 0, 0, quarter, quarter};
  REQUIRE(clip.add_track(1, k_channel_rotation, k_interp_linear, 4, times, turns));

  // The middle slot, sampled through the pool.
  rest_pose(skeleton, slot(1));
  clip.sample(0.5f, slot(1));

  // The same clip at the same time into a `Pose` of its own.
  Pose owned;
  rest_pose(skeleton, owned);
  clip.sample(0.5f, owned);
  for (u32 j = 0; j < joints; ++j) {
    CHECK(near(translation[joints + j], owned.translation[j]));
    CHECK(near(scale[joints + j], owned.scale[j]));
    CHECK(std::fabs(std::fabs(dot(rotation[joints + j], owned.rotation[j])) - 1.0f) < 1.0e-6f);
  }

  // The neighbours are untouched: a slot is exactly its own joints and nothing either side.
  for (u32 j = 0; j < joints; ++j) {
    CHECK(rotation[j] == Quat::identity());
    CHECK(rotation[2 * joints + j] == Quat::identity());
  }

  // A blend reads and writes views, and `local_to_model` takes one, so a pooled pose never needs
  // to be copied into a `Pose` to be composed.
  rest_pose(skeleton, slot(0));
  blend(slot(0), slot(1), 1.0f, slot(2));
  Vector<Mat4> model(joints, Mat4::identity());
  local_to_model(skeleton, ConstPoseView{slot(2)}, std::span<Mat4>(model.data(), model.size()));
  Vector<Mat4> reference(joints, Mat4::identity());
  local_to_model(skeleton, owned, std::span<Mat4>(reference.data(), reference.size()));
  for (u32 j = 0; j < joints; ++j) {
    for (u32 c = 0; c < 4; ++c)
      CHECK(near(model[j].c[c].xyz(), reference[j].c[c].xyz()));
  }

  // Three channels of different lengths write nothing rather than half a pose, which is the
  // answer two mismatched `Pose`s already got.
  PoseView ragged = slot(0);
  ragged.scale = std::span<Vec3>(scale.data(), 1);
  CHECK_FALSE(ragged.consistent());
  translation[0] = Vec3{7.0f, 7.0f, 7.0f};
  rest_pose(skeleton, ragged);
  CHECK(near(translation[0], Vec3{7.0f, 7.0f, 7.0f}));
}

TEST_CASE("anim skinning: the CPU reference is linear blend skinning out of 255") {
  const Skeleton skeleton = two_bone();
  // The bar of the assets fixture: three rows, bound to the root, half and half, and the tip.
  const Vec3 positions[6] = {{-0.5f, 0.0f, 0.0f}, {0.5f, 0.0f, 0.0f},  {-0.5f, 1.0f, 0.0f},
                             {0.5f, 1.0f, 0.0f},  {-0.5f, 2.0f, 0.0f}, {0.5f, 2.0f, 0.0f}};
  const f32 influence[3][2] = {{1.0f, 0.0f}, {0.5f, 0.5f}, {0.0f, 1.0f}};
  Vector<geometry::SkinBinding> bindings;
  for (u32 v = 0; v < 6; ++v) {
    const u32 joints[4] = {0, 1, 0, 0};
    const f32 weights[4] = {influence[v / 2][0], influence[v / 2][1], 0.0f, 0.0f};
    bindings.push_back(geometry::make_skin_binding(joints, weights));
  }

  Pose pose;
  rest_pose(skeleton, pose);
  Vector<Mat4> model(2, Mat4::identity());
  Vector<JointMatrix> matrices(2, JointMatrix{});
  auto build = [&]() {
    local_to_model(skeleton, pose, std::span<Mat4>(model.data(), model.size()));
    skinning_matrices(std::span<const Mat4>(model.data(), model.size()),
                      std::span<const Mat4>(skeleton.inverse_bind.data(), 2),
                      std::span<JointMatrix>(matrices.data(), 2));
  };
  build();

  Vector<Vec3> skinned(6, Vec3{});
  skin_positions(std::span<const Vec3>(positions, 6),
                 std::span<const geometry::SkinBinding>(bindings.data(), bindings.size()),
                 std::span<const JointMatrix>(matrices.data(), 2),
                 std::span<Vec3>(skinned.data(), skinned.size()));
  // At bind the rest pose comes back exactly, whatever the weights are — the one property that
  // makes a skinned instance and a rigid one draw the same picture.
  for (u32 v = 0; v < 6; ++v)
    CHECK(near(skinned[v], positions[v]));

  pose.rotation[1] = quat_from_axis_angle(Vec3::unit_z(), radians(90.0f));
  build();
  skin_positions(std::span<const Vec3>(positions, 6),
                 std::span<const geometry::SkinBinding>(bindings.data(), bindings.size()),
                 std::span<const JointMatrix>(matrices.data(), 2),
                 std::span<Vec3>(skinned.data(), skinned.size()));
  // The bottom row is the root's alone and does not move; the top row is the tip's alone and
  // swings a quarter turn about the joint at (0, 1, 0).
  CHECK(near(skinned[0], Vec3{-0.5f, 0.0f, 0.0f}));
  CHECK(near(skinned[4], Vec3{-1.0f, 0.5f, 0.0f}));
  CHECK(near(skinned[5], Vec3{-1.0f, 1.5f, 0.0f}));
  // The middle row is the weighted average of what the two matrices do to it, weights out of 255.
  const f32 w0 = static_cast<f32>(bindings[2].weights[0]) / 255.0f;
  const f32 w1 = static_cast<f32>(bindings[2].weights[1]) / 255.0f;
  const Vec3 expected = transform_point(matrices[0], positions[2]) * w0 +
                        transform_point(matrices[1], positions[2]) * w1;
  CHECK(near(skinned[2], expected));

  // Bad data deforms a vertex short rather than reading past the end of the palette.
  geometry::SkinBinding rogue;
  rogue.joints[0] = 200;
  rogue.weights[0] = 255;
  Vector<geometry::SkinBinding> broken = bindings;
  broken[0] = rogue;
  skin_positions(std::span<const Vec3>(positions, 6),
                 std::span<const geometry::SkinBinding>(broken.data(), broken.size()),
                 std::span<const JointMatrix>(matrices.data(), 2),
                 std::span<Vec3>(skinned.data(), skinned.size()));
  CHECK(near(skinned[0], Vec3{}));
}
