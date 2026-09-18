// Skeletons, poses, clips, and the CPU skinning reference (docs/plan/05-simulation.md §5.11).
// The sampler cases check each of the three glTF interpolation modes against values worked out
// by hand rather than against another implementation, because the sampler *is* the definition.
#include <domain/anim/clip.h>
#include <domain/anim/skeleton.h>

#include <doctest/doctest.h>

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
