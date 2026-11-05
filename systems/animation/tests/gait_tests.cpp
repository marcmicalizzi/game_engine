// The procedural gait (gait.h; docs/subsystems/animation.md, "The procedural gait";
// docs/experiments/gait-grounding-2026-11-04.md). The curves against the published keypoints, the
// stride against its relation, and a walk and a run generated for the Khronos RiggedFigure
// (tools/fetch-samples.ps1; the cases that need it say so and pass when it has not been fetched):
// the clip held to the curves in double precision, the sides half a cycle apart, the knee bending
// backward and the hip forward, the loop closed, the run's flight, and two runs the same bytes.
//
// No literal hash of a generated clip is pinned: the curves go through the C library's sin and
// cos, which may differ in a double's last bit between toolchains (gait.h).

#include <systems/animation/gait.h>
#include <systems/animation/library.h>

#include <core/math/math.h>
#include <domain/anim/clip.h>
#include <domain/assets/gltf.h>
#include <domain/anim/skeleton.h>
#include <domain/anim/standard_skeleton.h>

#include <doctest/doctest.h>
#include <test_paths.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <string>

using namespace engine;
using namespace engine::animation;
using SJ = anim::StandardJoint;

namespace {

struct Keypoint {
  f64 phase;
  f64 degrees;
};

// The published peaks at their phases (the sources are gait.cpp's rows and the grounding page).
constexpr Keypoint k_walk_hip[] = {{0.00, 25}, {0.52, -10}, {0.87, 28}};
constexpr Keypoint k_walk_knee[] = {{0.00, 5}, {0.15, 18}, {0.40, 4}, {0.73, 60}};
constexpr Keypoint k_walk_ankle[] = {{0.00, 0}, {0.07, -6}, {0.48, 10}, {0.62, -18}};
constexpr Keypoint k_walk_shoulder[] = {{0.00, -24}, {0.50, 8}};
constexpr Keypoint k_run_hip[] = {{0.00, 40}, {0.39, -8}, {0.85, 55}};
constexpr Keypoint k_run_knee[] = {{0.00, 20}, {0.15, 42}, {0.39, 18}, {0.70, 92}};
constexpr Keypoint k_run_ankle[] = {{0.15, 20}, {0.45, -22}};

void check_peaks(GaitBand band, GaitCurve curve, std::span<const Keypoint> points,
                 const char* label) {
  for (const Keypoint& p : points) {
    const f64 got = gait_curve(band, curve, p.phase);
    CHECK_MESSAGE(std::abs(got - p.degrees) <= 2.0,
                  label << " at " << p.phase * 100.0 << "%: " << got << " against " << p.degrees);
  }
}

std::string rigged_figure() {
  return test::data_path(ENGINE_SOURCE_DIR "/content/samples/RiggedFigure/RiggedFigure.glb",
                         "content/samples/RiggedFigure/RiggedFigure.glb");
}

// The rest pose's model matrices and composed rotations, as gait.cpp reads them.
struct RestFrames {
  Vector<Mat4> model;
  Vector<Quat> world;
};

void rest_frames(const anim::Skeleton& s, RestFrames& out) {
  anim::Pose rest;
  anim::rest_pose(s, rest);
  out.model.resize(s.joint_count());
  anim::local_to_model(s, rest, std::span<Mat4>(out.model.data(), out.model.size()));
  out.world.resize(s.joint_count());
  for (u32 j = 0; j < s.joint_count(); ++j) {
    const i32 p = s.parents[j];
    out.world[j] = normalize(p >= 0 ? out.world[static_cast<u32>(p)] * s.local_bind[j].rotation
                                    : s.local_bind[j].rotation);
  }
}

Vec3 position(const Mat4& m) { return Vec3{m.c[3].x, m.c[3].y, m.c[3].z}; }

struct Body {
  Vec3 up, right, forward;
};

Body body_axes(const RestFrames& r, const anim::JointMapping& m) {
  const auto at = [&](SJ role) { return position(r.model[static_cast<u32>(m[role])]); };
  const Vec3 hips = (at(SJ::LeftUpperLeg) + at(SJ::RightUpperLeg)) * 0.5f;
  const Vec3 feet = (at(SJ::LeftFoot) + at(SJ::RightFoot)) * 0.5f;
  Body b;
  b.up = normalize(hips - feet);
  Vec3 across = at(SJ::RightUpperLeg) - at(SJ::LeftUpperLeg);
  b.right = normalize(across - b.up * dot(across, b.up));
  b.forward = cross(b.up, b.right);
  return b;
}

// The key `k` of the rotation track that drives `joint`.
Quat key_rotation(const anim::Clip& clip, u32 joint, u32 k) {
  for (const anim::Track& t : clip.tracks) {
    if (t.joint != joint || t.channel != anim::k_channel_rotation) continue;
    const f32* v = clip.values.data() + t.first_value + 4 * k;
    return Quat{v[0], v[1], v[2], v[3]};
  }
  return Quat::identity();
}

u32 key_count(const anim::Clip& clip) { return clip.tracks.empty() ? 0 : clip.tracks[0].key_count; }

// The body-frame turn about `axis` (degrees) the key's rotation of `joint` makes from its rest:
// body = W (rest^-1 q) W^-1, whose angle about a unit axis is 2 atan2(v . axis, w) for a rotation
// about that axis alone.
f64 body_angle(const anim::Skeleton& s, const RestFrames& r, u32 joint, Quat q, Vec3 axis) {
  const Quat w = r.world[joint];
  const Quat body = w * (conjugate(s.local_bind[joint].rotation) * q) * conjugate(w);
  const f64 along = static_cast<f64>(dot(body.vector_part(), axis));
  return 2.0 * std::atan2(along, static_cast<f64>(body.w)) * 57.29577951308232;
}

bool load_figure(Library& library) {
  const std::string path = rigged_figure();
  if (!test::path_exists(path)) {
    MESSAGE("not fetched (tools/fetch-samples.ps1): " << path);
    return false;
  }
  std::string error;
  REQUIRE_MESSAGE(library.load_gltf(path, {}, nullptr, &error), error);
  REQUIRE(library.skeleton_count() > 0);
  return true;
}

bool generate(const Library& library, GaitBand band, ClipAsset& out, GaitCycle& cycle) {
  const SkeletonAsset& asset = library.skeleton(0);
  GaitParams params;
  params.band = band;
  out.name = band == GaitBand::Run ? "gait-run" : "gait-walk";
  out.id = Library::clip_id(out.name);
  out.skeleton = 0;
  std::string error;
  const bool ok =
      generate_gait_clip(asset, gait_mapping(asset.skeleton, asset.mapping), params, out, cycle, error);
  CHECK_MESSAGE(ok, error);
  return ok;
}

}  // namespace

TEST_CASE("gait: the curves pass within 2 degrees of the published peaks at their phases") {
  check_peaks(GaitBand::Walk, GaitCurve::Hip, k_walk_hip, "walk hip");
  check_peaks(GaitBand::Walk, GaitCurve::Knee, k_walk_knee, "walk knee");
  check_peaks(GaitBand::Walk, GaitCurve::Ankle, k_walk_ankle, "walk ankle");
  check_peaks(GaitBand::Walk, GaitCurve::Shoulder, k_walk_shoulder, "walk shoulder");
  check_peaks(GaitBand::Run, GaitCurve::Hip, k_run_hip, "run hip");
  check_peaks(GaitBand::Run, GaitCurve::Knee, k_run_knee, "run knee");
  check_peaks(GaitBand::Run, GaitCurve::Ankle, k_run_ankle, "run ankle");

  // Over the whole cycle: the knee never extends past the published stance minimum (3-5 degrees,
  // less the 2-degree tolerance), the hip's extremes are where the sources put them, and every
  // curve is periodic.
  f64 knee_min = 1e9;
  f64 hip_min = 1e9;
  f64 hip_min_at = 0.0;
  f64 run_knee_min = 1e9;
  for (u32 i = 0; i < 1000; ++i) {
    const f64 p = static_cast<f64>(i) / 1000.0;
    knee_min = std::min(knee_min, gait_curve(GaitBand::Walk, GaitCurve::Knee, p));
    run_knee_min = std::min(run_knee_min, gait_curve(GaitBand::Run, GaitCurve::Knee, p));
    const f64 hip = gait_curve(GaitBand::Walk, GaitCurve::Hip, p);
    if (hip < hip_min) {
      hip_min = hip;
      hip_min_at = p;
    }
  }
  CHECK(knee_min >= 1.0);
  CHECK(run_knee_min >= 15.0);  // a runner's knee stays bent (Novacheck: 18-20 at its least)
  CHECK(hip_min_at > 0.45);
  CHECK(hip_min_at < 0.60);
  for (u32 c = 0; c < static_cast<u32>(GaitCurve::Count); ++c) {
    for (const GaitBand band : {GaitBand::Walk, GaitBand::Run}) {
      const f64 a = gait_curve(band, static_cast<GaitCurve>(c), 0.0);
      const f64 b = gait_curve(band, static_cast<GaitCurve>(c), 1.0);
      CHECK(std::abs(a - b) < 1e-9);
    }
  }
}

TEST_CASE("gait: stride and cycle time follow speed and leg length") {
  // Calibration: Perry & Burnfield's free walk, 1.41 m at 1.37 m/s on a 0.93 m leg.
  CHECK(gait_stride_m(GaitBand::Walk, 1.37f, 0.93f) == doctest::Approx(1.41).epsilon(1e-4));
  // Grieve's exponent: stride grows as speed^0.42 at one leg length.
  const f32 s10 = gait_stride_m(GaitBand::Walk, 1.0f, 0.93f);
  const f32 s14 = gait_stride_m(GaitBand::Walk, 1.4f, 0.93f);
  const f32 s18 = gait_stride_m(GaitBand::Walk, 1.8f, 0.93f);
  CHECK(static_cast<f64>(s14 / s10) == doctest::Approx(std::pow(1.4, 0.42)).epsilon(1e-4));
  CHECK(static_cast<f64>(s18 / s10) == doctest::Approx(std::pow(1.8, 0.42)).epsilon(1e-4));
  // Cadences (steps a minute) inside normative adult ranges at each speed: Perry's free walk is
  // 113 +- 9 at 1.37 m/s; slow walking near 1 m/s runs 90-110, brisk walking near 1.8 m/s 120-145
  // (Grieve & Gear 1966's adults span these).
  const auto cadence = [](f32 speed, f32 stride) { return 120.0 * speed / stride; };
  CHECK(cadence(1.0f, s10) > 88.0);
  CHECK(cadence(1.0f, s10) < 110.0);
  CHECK(cadence(1.4f, s14) > 104.0);
  CHECK(cadence(1.4f, s14) < 122.0);
  CHECK(cadence(1.8f, s18) > 120.0);
  CHECK(cadence(1.8f, s18) < 145.0);
  // A longer leg strides further and steps more slowly at the same speed (a 1.6 m and a 1.9 m
  // body: legs of 0.85 and 1.01 m).
  const f32 short_leg = gait_stride_m(GaitBand::Walk, 1.4f, 0.85f);
  const f32 long_leg = gait_stride_m(GaitBand::Walk, 1.4f, 1.01f);
  CHECK(long_leg > short_leg);
  CHECK(cadence(1.4f, long_leg) < cadence(1.4f, short_leg));
  // The run: 1.40 Hz on the reference leg (Cavanagh & Kram 1989), so stride = speed / 1.4.
  CHECK(gait_stride_m(GaitBand::Run, 5.0f, 0.93f) == doctest::Approx(5.0 / 1.4).epsilon(1e-4));
  CHECK(gait_stride_m(GaitBand::Run, 5.0f, 1.01f) > gait_stride_m(GaitBand::Run, 5.0f, 0.85f));
  // The bands' reference speeds fall either side of the walk-to-run transition.
  CHECK(k_gait_walk_reference_mps < k_gait_transition_mps);
  CHECK(k_gait_run_reference_mps > k_gait_transition_mps);
}

TEST_CASE("gait: a rig missing a leg role is refused with the roles named") {
  Library library;
  if (!load_figure(library)) return;
  const SkeletonAsset& asset = library.skeleton(0);
  anim::JointMapping mapping = gait_mapping(asset.skeleton, asset.mapping);
  std::string missing;
  CHECK_MESSAGE(gait_roles_present(mapping, missing), missing);
  mapping.to_skeleton[static_cast<u32>(SJ::RightFoot)] = anim::k_no_joint;
  mapping.to_skeleton[static_cast<u32>(SJ::LeftLowerLeg)] = anim::k_no_joint;
  ClipAsset out;
  GaitCycle cycle;
  std::string error;
  CHECK_FALSE(generate_gait_clip(asset, mapping, GaitParams{}, out, cycle, error));
  CHECK_MESSAGE(error.find("LeftLowerLeg, RightFoot") != std::string::npos, error);
}

TEST_CASE("gait: RiggedFigure's roles are read from its rest pose where its names say nothing") {
  Library library;
  if (!load_figure(library)) return;
  const SkeletonAsset& asset = library.skeleton(0);
  const anim::Skeleton& s = asset.skeleton;
  u32 inferred = 0;
  const anim::JointMapping m = gait_mapping(s, asset.mapping, &inferred);
  MESSAGE("by name " << asset.mapping.mapped << " roles; from the rest pose " << inferred);
  const auto name = [&](SJ role) {
    const i32 j = m[role];
    return j >= 0 ? s.names[static_cast<u32>(j)] : std::string("-");
  };
  CHECK(name(SJ::Hips) == "torso_joint_1");
  CHECK(name(SJ::Spine) == "torso_joint_2");
  CHECK(name(SJ::Chest) == "torso_joint_3");
  CHECK(name(SJ::RightUpperLeg) == "leg_joint_R_1");
  CHECK(name(SJ::RightLowerLeg) == "leg_joint_R_2");
  CHECK(name(SJ::RightFoot) == "leg_joint_R_3");
  CHECK(name(SJ::RightToes) == "leg_joint_R_5");
  CHECK(name(SJ::LeftUpperLeg) == "leg_joint_L_1");
  CHECK(name(SJ::LeftFoot) == "leg_joint_L_3");
  CHECK(name(SJ::RightUpperArm) == "arm_joint_R_1");
  CHECK(name(SJ::RightLowerArm) == "arm_joint_R_2");
  CHECK(name(SJ::LeftUpperArm) == "arm_joint_L_1");
  CHECK(name(SJ::LeftHand) == "arm_joint_L_3");
  // A rig whose names already hold the legs keeps its mapping untouched.
  u32 again = 0;
  const anim::JointMapping same = gait_mapping(s, m, &again);
  CHECK(again == 0);
  CHECK(same.mapped == m.mapped);
}

TEST_CASE("gait: RiggedFigure's walk follows the curves, the sides half a cycle apart") {
  Library library;
  if (!load_figure(library)) return;
  const SkeletonAsset& asset = library.skeleton(0);
  const anim::Skeleton& s = asset.skeleton;
  const anim::JointMapping m = gait_mapping(s, asset.mapping);
  MESSAGE("RiggedFigure: " << s.joint_count() << " joints, " << m.mapped << " roles");
  ClipAsset walk;
  GaitCycle cycle;
  REQUIRE(generate(library, GaitBand::Walk, walk, cycle));
  MESSAGE("walk: leg " << cycle.leg_length_m << " m, stride " << cycle.stride_m << " m, cycle "
                       << cycle.cycle_s << " s, cadence " << cycle.cadence_spm << ", "
                       << cycle.joints_driven << " joints, " << cycle.keys << " keys");
  CHECK(cycle.leg_length_m > 0.0f);
  CHECK(walk.clip.duration == doctest::Approx(cycle.cycle_s));
  const u32 keys = key_count(walk.clip);
  REQUIRE(keys == cycle.keys);
  const u32 n = keys - 1;

  RestFrames r;
  rest_frames(s, r);
  const Body b = body_axes(r, m);
  // Every key against the double-precision curve, both sides: the right at phase k/n, the left
  // half a cycle later. 0.05 degrees is the floats' rounding and the rest frame's, many times over.
  struct Check {
    SJ role;
    GaitCurve curve;
    f64 sign;
    f64 offset;
  };
  const Check checks[] = {
      {SJ::RightUpperLeg, GaitCurve::Hip, 1.0, 0.0},   {SJ::LeftUpperLeg, GaitCurve::Hip, 1.0, 0.5},
      {SJ::RightLowerLeg, GaitCurve::Knee, -1.0, 0.0}, {SJ::LeftLowerLeg, GaitCurve::Knee, -1.0, 0.5},
      {SJ::RightFoot, GaitCurve::Ankle, 1.0, 0.0},     {SJ::LeftFoot, GaitCurve::Ankle, 1.0, 0.5},
  };
  f64 worst = 0.0;
  for (const Check& c : checks) {
    const u32 joint = static_cast<u32>(m[c.role]);
    for (u32 k = 0; k < n; ++k) {
      f64 phase = static_cast<f64>(k) / static_cast<f64>(n) + c.offset;
      if (phase >= 1.0) phase -= 1.0;
      const f64 want = c.sign * gait_curve(GaitBand::Walk, c.curve, phase);
      const f64 got = body_angle(s, r, joint, key_rotation(walk.clip, joint, k), b.right);
      worst = std::max(worst, std::abs(got - want));
    }
  }
  MESSAGE("worst key against the double curve: " << worst << " degrees");
  CHECK(worst < 0.05);

  // The loop closes: every track's last key is its first.
  for (const anim::Track& t : walk.clip.tracks) {
    const f32* first = walk.clip.values.data() + t.first_value;
    const f32* last = first + static_cast<usize>(t.components) * (t.key_count - 1);
    CHECK(std::memcmp(first, last, sizeof(f32) * t.components) == 0);
  }

  // The flexion signs, in the posed rig: at the knee's swing peak (73%) the right ankle is behind
  // the line of the thigh (the knee bends backward), and at the hip's flexion peak (87%) the right
  // knee is ahead of the hip (the hip bends forward).
  anim::Pose pose;
  anim::rest_pose(s, pose);
  Vector<Mat4> model(s.joint_count());
  const auto pose_at = [&](f64 phase) {
    anim::rest_pose(s, pose);
    walk.clip.sample(static_cast<f32>(phase * static_cast<f64>(walk.clip.duration)), pose, true);
    anim::local_to_model(s, pose, std::span<Mat4>(model.data(), model.size()));
  };
  const auto at = [&](SJ role) { return position(model[static_cast<u32>(m[role])]); };
  // Each against the rig's own rest, whose thigh and knee need not be straight.
  const auto bend = [&]() {
    const Vec3 thigh = normalize(at(SJ::RightLowerLeg) - at(SJ::RightUpperLeg));
    const Vec3 shank = normalize(at(SJ::RightFoot) - at(SJ::RightLowerLeg));
    // Turning the down-pointing thigh toward the shank about +right moves it forward, so a shank
    // turned backward of the thigh's line is a negative turn about the right axis.
    return dot(cross(thigh, shank), b.right);
  };
  const auto thigh_ahead = [&]() {
    return dot(normalize(at(SJ::RightLowerLeg) - at(SJ::RightUpperLeg)), b.forward);
  };
  anim::local_to_model(s, pose, std::span<Mat4>(model.data(), model.size()));
  const f32 rest_bend = bend();
  const f32 rest_ahead = thigh_ahead();
  pose_at(0.73);
  CHECK(bend() < rest_bend - 0.5f);  // 60 degrees of flexion: sin(60) is 0.87
  pose_at(0.87);
  CHECK(thigh_ahead() > rest_ahead + 0.3f);  // the hip flexed past its rest
  pose_at(0.52);
  CHECK(thigh_ahead() < rest_ahead - 0.1f);  // and extended behind it

  // Deterministic: a second generation is the same bytes.
  ClipAsset again;
  GaitCycle again_cycle;
  REQUIRE(generate(library, GaitBand::Walk, again, again_cycle));
  REQUIRE(again.clip.values.size() == walk.clip.values.size());
  CHECK(std::memcmp(again.clip.values.data(), walk.clip.values.data(),
                    sizeof(f32) * walk.clip.values.size()) == 0);
  CHECK(std::memcmp(again.clip.times.data(), walk.clip.times.data(),
                    sizeof(f32) * walk.clip.times.size()) == 0);
}

TEST_CASE("gait: RiggedFigure's run has a flight phase and rises in it") {
  Library library;
  if (!load_figure(library)) return;
  const SkeletonAsset& asset = library.skeleton(0);
  ClipAsset run;
  GaitCycle cycle;
  REQUIRE(generate(library, GaitBand::Run, run, cycle));
  MESSAGE("run: stride " << cycle.stride_m << " m, cycle " << cycle.cycle_s << " s, cadence "
                         << cycle.cadence_spm);
  // Toe-off before the other foot's contact at half a cycle: both feet off the ground between.
  CHECK(cycle.stance < 0.5f);
  CHECK(cycle.stance == doctest::Approx(0.39f));
  // The pelvis is higher in flight (45%) than in mid stance (20%), in the posed rig.
  const anim::Skeleton& s = asset.skeleton;
  RestFrames r;
  rest_frames(s, r);
  const anim::JointMapping mapping = gait_mapping(s, asset.mapping);
  const Body b = body_axes(r, mapping);
  anim::Pose pose;
  Vector<Mat4> model(s.joint_count());
  const u32 hips = static_cast<u32>(mapping[SJ::Hips]);
  const auto height = [&](f64 phase) {
    anim::rest_pose(s, pose);
    run.clip.sample(static_cast<f32>(phase * static_cast<f64>(run.clip.duration)), pose, true);
    anim::local_to_model(s, pose, std::span<Mat4>(model.data(), model.size()));
    return dot(position(model[hips]), b.up);
  };
  CHECK(height(0.45) > height(0.20));
  CHECK(height(0.95) > height(0.70));
  // The run's knee in swing bends further than the walk's.
  CHECK(gait_curve(GaitBand::Run, GaitCurve::Knee, 0.70) >
        gait_curve(GaitBand::Walk, GaitCurve::Knee, 0.73) + 20.0);

  // **The run's displacement bound covers the walk's** on the rig's own vertices: engine-view
  // attaches the player on the run so its cull padding covers both (apps.md, "The procedural
  // gait").
  assets::MeshData mesh;
  std::string why;
  REQUIRE_MESSAGE(assets::load_gltf(rigged_figure(), mesh, &why), why);
  JointBounds bounds;
  joint_influence_bounds(
      std::span<const Vec3>(mesh.positions.data(), mesh.positions.size()),
      std::span<const geometry::SkinBinding>(mesh.skin_bindings.data(), mesh.skin_bindings.size()),
      s.joint_count(), bounds);
  ClipAsset walk;
  GaitCycle walk_cycle;
  REQUIRE(generate(library, GaitBand::Walk, walk, walk_cycle));
  const f32 walk_bound = clip_displacement_bound(s, walk.clip, bounds);
  const f32 run_bound = clip_displacement_bound(s, run.clip, bounds);
  MESSAGE("displacement bound: walk " << walk_bound << ", run " << run_bound);
  CHECK(run_bound >= walk_bound);
}
