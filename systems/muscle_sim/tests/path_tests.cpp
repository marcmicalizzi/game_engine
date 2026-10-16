// The path from the skeleton (path.h): the routed length against the closed form on a synthetic
// two-bone skeleton, the tick's Hermite from the boundaries' lengths, the kernel on that Hermite
// against the kernel on the analytic path, and animation's posed skeleton through the pose source.
#include "two_bone.h"

#include <domain/anim/clip.h>
#include <domain/assets/gltf.h>
#include <domain/ecs/identity.h>
#include <domain/ecs/sim_world.h>
#include <systems/animation/animation.h>
#include <systems/animation/library.h>
#include <systems/muscle/supervisor.h>
#include <systems/muscle_sim/muscle_sim.h>
#include <systems/muscle_sim/path.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <schemas/animation.h>
#include <span>
#include <string>

using namespace engine;
using namespace engine::muscle_sim;

namespace {

// The analytic path as the kernel reads one: L(theta(t)) and its exact rate, in absolute time.
struct JointAnglePath {
  test::Motion motion;
  void sample(double t, double, double& length, double& rate) const noexcept {
    length = test::analytic_length(motion.angle(t));
    rate = test::analytic_rate(motion, t);
  }
};

muscle::ActuatorSpec r008_spec(u8 tag) {
  muscle::ActuatorSpec s{};
  s.actuator_id = Id128{0x7061, tag};
  s.path_id = s.actuator_id;
  REQUIRE(muscle::parse_sha256("9665d9f3c53dfae5861a5318536d6625b973eaed432fd67c533d4627cd5c1ed6",
                               s.parameter_sha256));
  return s;
}

}  // namespace

TEST_CASE("muscle_sim path: the routed length is the closed form's to f32 rounding") {
  const anim::Skeleton s = test::two_bone_skeleton();
  const test::Motion m;
  Mat4 model[2];
  const RoutePoint direct[2] = {{0, test::k_origin}, {1, test::k_insertion}};
  const RoutePoint via[3] = {{0, test::k_origin}, {0, test::k_via}, {1, test::k_insertion}};
  double worst = 0.0;
  for (u32 k = 0; k <= 120; ++k) {
    const double theta = m.angle(static_cast<double>(k) / 60.0);
    test::pose_two_bone(s, theta, 0.0f, model);
    worst = std::max(worst, std::fabs(route_length(model, direct) - test::analytic_length(theta)));
    worst =
        std::max(worst, std::fabs(route_length(model, via) - test::analytic_length(theta, true)));
  }
  std::printf("muscle_sim path: routed length against the closed form, worst %.3g m\n", worst);
  // The joint transforms are f32 (Mat4), so a point 0.3 m out carries up to ~3e-8 m of rounding
  // (an ulp of 0.3f); measured 8.5e-10 m on MSVC, and the f32 sine and cosine inside the joint's
  // quaternion are the C library's, so the bound is the f32 one.
  CHECK(worst <= 1e-7);
  CHECK(route_length(model, std::span<const RoutePoint>(direct, 1)) == 0.0);
}

TEST_CASE("muscle_sim path: the tick's Hermite runs from the last boundary's length and rate") {
  PathHistory h;
  muscle::HermitePath p = next_path(h, 0.15);
  CHECK(p.l0 == 0.15);
  CHECK(p.l1 == 0.15);
  CHECK(p.v0 == 0.0);
  CHECK(p.v1 == 0.0);
  CHECK(h.valid);
  p = next_path(h, 0.151);
  CHECK(p.l0 == 0.15);
  CHECK(p.v0 == 0.0);
  CHECK(p.l1 == 0.151);
  CHECK(p.v1 == doctest::Approx(0.06));
  p = next_path(h, 0.153);
  CHECK(p.v0 == doctest::Approx(0.06));
  CHECK(p.v1 == doctest::Approx(0.12));
  // A boundary that is not a number ends the history; the next valid one starts it again.
  p = next_path(h, std::numeric_limits<double>::quiet_NaN());
  CHECK(std::isnan(p.l1));
  CHECK_FALSE(h.valid);
  p = next_path(h, 0.149);
  CHECK(p.l0 == 0.149);
  CHECK(p.v0 == 0.0);
}

TEST_CASE(
    "muscle_sim path: the kernel on the boundaries' Hermite follows the kernel on the "
    "analytic path within the stated bound") {
  const anim::Skeleton s = test::two_bone_skeleton();
  const test::Motion motion;  // 0.4 rad at 1 Hz
  const RoutePoint route[2] = {{0, test::k_origin}, {1, test::k_insertion}};
  const JointAnglePath analytic{motion};
  muscle::LineStore store;
  store.reserve(2);
  u32 tabulated = 0;
  u32 exact = 0;
  REQUIRE(store.add(r008_spec(0), tabulated));
  REQUIRE(store.add(r008_spec(1), exact));
  const double l0 = test::analytic_length(0.0);
  muscle::Failure f{};
  REQUIRE(muscle::initialize_static(store, tabulated, 0, 0.0, l0, f));
  REQUIRE(muscle::initialize_static(store, exact, 0, 0.0, l0, f));
  Mat4 model[2];
  PathHistory history;
  test::pose_two_bone(s, motion.angle(0.0), 0.0f, model);
  next_path(history, route_length(model, route));  // boundary 0, as the system's first tick
  double force = 0.0;
  double fiber = 0.0;
  double mid = 0.0;
  double first = 0.0;
  for (u64 k = 1; k <= 120; ++k) {
    CAPTURE(k);
    test::pose_two_bone(s, motion.angle(static_cast<double>(k) / 60.0), 0.0f, model);
    const muscle::HermitePath path = next_path(history, route_length(model, route));
    const double u = k < 30 ? 0.0 : 0.6;
    REQUIRE(muscle::supervise_actuator(store, tabulated, k, u, path));
    REQUIRE(muscle::supervise_actuator(store, exact, k, u, analytic));
    force = std::max(
        force, std::fabs(store.cache[tabulated].tendon_force - store.cache[exact].tendon_force));
    fiber = std::max(fiber, std::fabs(store.fiber_length[tabulated] - store.fiber_length[exact]));
    const double t0 = muscle::parent_start(k, 0);
    const double tm = t0 + 0.5 / 60.0;
    double lh = 0.0;
    double rh = 0.0;
    path.sample(tm, t0, lh, rh);
    const double error = std::fabs(lh - test::analytic_length(motion.angle(tm)));
    if (k == 1)
      first = error;
    else
      mid = std::max(mid, error);
  }
  std::printf(
      "muscle_sim path: Hermite against analytic over 2 s at 1 Hz: path mid-tick %.3g m "
      "(the first tick %.3g m), fiber %.3g m, tendon force %.3g N\n",
      mid, first, fiber, force);
  // The first tick has no history, so it starts from rate zero: the true rate there is
  // 0.4 (2 pi) dL/dtheta = 0.021 m/s, and h10(1/2) dt 0.021 = 4.3e-5 m at mid-tick. After it the
  // rate lags by (dt/2) L'' (about 1e-3 m/s for this motion), much of which the two ends' lags
  // cancel at mid-tick. Through force-velocity's slope at zero velocity (about 2.4 per unit of
  // v/vmax) a 1e-3 m/s lag is 0.2% of the active force: about 1 N of 600 at a = 0.6.
  CHECK(first <= 5e-5);
  CHECK(mid <= 2e-6);
  CHECK(fiber <= 1e-5);
  CHECK(force <= 1.5);
  CHECK(store.cache[tabulated].tendon_force > 0.0);
}

TEST_CASE("muscle_sim path: animation's posed skeleton, through the pose source") {
  // The two-bone arm as a glTF import would hand it over, with a clip bending the lower bone 0.5
  // rad about z over a second.
  assets::MeshData mesh;
  mesh.nodes.resize(2);
  mesh.nodes[0].name = "upper";
  mesh.nodes[1].name = "lower";
  mesh.nodes[1].parent = 0;
  mesh.nodes[1].local.position = Vec3{0.0f, test::k_bone, 0.0f};
  assets::Skin skin;
  skin.name = "arm";
  skin.joints = {0, 1};
  skin.inverse_bind = {Mat4::identity(), Mat4::identity()};
  mesh.skins.push_back(skin);
  assets::Animation bend;
  bend.name = "bend";
  bend.duration = 1.0f;
  assets::AnimationSampler sampler;
  sampler.times = {0.0f, 1.0f};
  const f32 half = 0.25f;
  sampler.values = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, std::sin(half), std::cos(half)};
  sampler.interpolation = assets::k_interp_linear;
  sampler.components = 4;
  bend.samplers.push_back(sampler);
  bend.channels.push_back(
      assets::AnimationChannel{.node = 1, .sampler = 0, .path = assets::k_path_rotation});
  mesh.animations.push_back(bend);
  animation::Library library;
  std::string error;
  REQUIRE_MESSAGE(library.add(mesh, "arm", nullptr, &error), error);
  REQUIRE(library.skeleton_count() == 1);
  REQUIRE(library.clip_count() == 1);

  ecs::SimWorld sim;
  animation::AnimationSystem animation(library);
  animation.install(sim);
  const flecs::entity character = ecs::create_entity(sim.world(), Id128{0x6172, 1});
  REQUIRE(animation.attach(character, library.skeleton(0).id, library.clip(0).id));
  const PoseSource source = animation_pose_source(animation);
  u32 upper = 9;
  u32 lower = 9;
  REQUIRE(source.find_joint(source.context, character, "upper", upper));
  REQUIRE(source.find_joint(source.context, character, "lower", lower));
  CHECK(upper == 0);
  CHECK(lower == 1);
  u32 none = 0;
  CHECK_FALSE(source.find_joint(source.context, character, "elbow", none));
  const RoutePoint route[2] = {{upper, test::k_origin}, {lower, test::k_insertion}};
  const anim::Skeleton& skeleton = library.skeleton_data(0);
  for (int k = 1; k <= 30; ++k) {
    CAPTURE(k);
    sim.step();
    Mat4 model[4];
    u32 joints = 0;
    REQUIRE(source.model_transforms(source.context, character, model, joints));
    REQUIRE(joints == 2);
    const double routed = route_length(std::span<const Mat4>(model, joints), route);
    // The pose pool's pose composed into model space, by the library's own clip at the playhead:
    // the same bits, whoever composes them.
    const f32 time = character.get<animation::AnimationPlayer>().time;
    anim::Pose pose;
    pose.resize(2);
    anim::rest_pose(skeleton, pose);
    library.clip_data(0).sample(time, pose, true);
    Mat4 reference[2];
    anim::local_to_model(skeleton, pose, reference);
    CHECK(routed == route_length(std::span<const Mat4>(reference, 2), route));
    // And near the closed form at the clip's angle (a two-key rotation interpolates its angle
    // close to linearly; the bound covers the interpolation's own departure).
    CHECK(std::fabs(routed - test::analytic_length(0.5 * static_cast<double>(time))) <= 1e-5);
  }
  CHECK(source.reads_resources == sim::resource_mask(animation::k_resource_pose_pool));
}
