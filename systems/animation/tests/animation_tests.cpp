// The animation capability end to end (ADR-0027, ADR-0028). Every case here is an invariant from
// docs/subsystems/animation.md, and the ones that matter most are the ones that check this
// capability against `domain/anim` rather than against itself: a pose in the pool has to be the
// clip sampled directly, and the matrices have to be `anim::skinning_matrices` of that pose. A
// system layer that quietly reimplements its data layer is the failure this guards against.
#include "animation_glb.h"

#include <core/ids/id128.h>
#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <domain/assets/gltf.h>
#include <domain/ecs/identity.h>
#include <domain/ecs/os_api.h>
#include <domain/ecs/scheduled_tick.h>
#include <domain/ecs/world_commands.h>
#include <domain/sim/scheduler.h>
#include <systems/animation/animation.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::animation;

namespace {

// 64 Hz, so a step is exactly 0.015625 s and a one-second clip is exactly 64 steps. The looping
// case asserts an *exact* wrap, and it can only do that if the numbers are exact binary fractions.
constexpr u32 k_hz = 64;
constexpr u32 k_steps_per_clip = 64;

struct Fixture {
  engine::test::TempDir tmp{"engine_animation"};
  Library library;
  Id128 skeleton;
  Id128 walk;
  Id128 idle;

  Fixture() {
    const std::string path = tmp.file("rig.glb");
    REQUIRE(test_fixture::write_animated_glb(path));
    std::string error;
    LoadStats stats;
    REQUIRE_MESSAGE(library.load_gltf(path, "rig", &stats, &error), error);
    CHECK(stats.skeletons == 1);
    CHECK(stats.clips == 2);
    skeleton = Library::skeleton_id("rig/bar_skin");
    walk = Library::clip_id("rig/walk");
    idle = Library::clip_id("rig/idle");
  }
};

bool near(Vec3 a, Vec3 b, f32 eps = 1.0e-5f) {
  return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}

bool near(Quat a, Quat b, f32 eps = 1.0e-5f) {
  return std::fabs(std::fabs(dot(a, b)) - 1.0f) < eps;
}

JsonValue parse(const std::string& text) {
  JsonValue value;
  const JsonParseResult result = parse_json(text, value);
  REQUIRE_MESSAGE(result.ok, result.message);
  return value;
}

std::string hex(const Id128& id) {
  char buffer[Id128::k_hex_length + 1];
  id.to_hex(buffer);
  return std::string(buffer);
}

ecs::SimWorldConfig world_config() {
  ecs::SimWorldConfig config;
  config.hz = k_hz;
  return config;
}

// The pose the two `anim::` calls produce on their own, for comparison with what the capability
// put in the pool.
void sample_directly(const Library& library, u32 skeleton_index, u32 clip_index, f32 time,
                     bool looping, anim::Pose& out) {
  anim::rest_pose(library.skeleton_data(skeleton_index), out);
  if (clip_index != Library::k_not_found) library.clip_data(clip_index).sample(time, out, looping);
}

}  // namespace

TEST_CASE("animation library: a glTF skin becomes a skeleton and its animations become clips") {
  Fixture fixture;
  REQUIRE(fixture.library.skeleton_count() == 1);
  REQUIRE(fixture.library.clip_count() == 2);

  const u32 index = fixture.library.skeleton_index(fixture.skeleton);
  REQUIRE(index != Library::k_not_found);
  const SkeletonAsset& asset = fixture.library.skeleton(index);
  CHECK(asset.name == "rig/bar_skin");
  const anim::Skeleton& skeleton = asset.skeleton;
  REQUIRE(skeleton.joint_count() == 2);
  // The palette order is the skeleton's order, and the parent comes first.
  CHECK(skeleton.names[0] == "joint_root");
  CHECK(skeleton.names[1] == "joint_tip");
  CHECK(skeleton.parents[0] == anim::k_no_joint);
  CHECK(skeleton.parents[1] == 0);
  CHECK(near(skeleton.local_bind[1].position, Vec3{0.0f, 1.0f, 0.0f}));
  // The file's own inverse bind matrices, not recomputed ones: the tip's undoes its height.
  CHECK(near(skeleton.inverse_bind[1].c[3].xyz(), Vec3{0.0f, -1.0f, 0.0f}));
  // The standard-skeleton mapping is computed once, at load. This rig fills no humanoid role, and
  // the honest answer to that is a complete table of `k_no_joint` rather than an absent one.
  CHECK(asset.mapping.to_skeleton.size() == anim::k_standard_joint_count);

  const u32 walk = fixture.library.clip_index(fixture.walk);
  const u32 idle = fixture.library.clip_index(fixture.idle);
  REQUIRE(walk != Library::k_not_found);
  REQUIRE(idle != Library::k_not_found);
  CHECK(fixture.library.clip(walk).name == "rig/walk");
  CHECK(fixture.library.clip_data(walk).duration == doctest::Approx(test_fixture::k_clip_seconds));
  CHECK(fixture.library.clip_data(walk).tracks.size() == 1);
  CHECK(fixture.library.clip_data(walk).tracks[0].joint == 1);
  CHECK(fixture.library.clip_data(idle).tracks[0].joint == 0);
  CHECK(fixture.library.clip(walk).skeleton == index);

  // The ids are a function of the name alone, so a second library built from the same content
  // resolves what the first one's save wrote. That is the whole reason `AnimationPlayer::clip` is
  // an `Id128` and not an index.
  Library second;
  std::string error;
  REQUIRE_MESSAGE(second.load_gltf(fixture.tmp.file("rig.glb"), "rig", nullptr, &error), error);
  CHECK(second.skeleton(second.skeleton_index(fixture.skeleton)).id == fixture.skeleton);
  CHECK(second.clip_index(fixture.walk) != Library::k_not_found);
}

// Regression, found by the Khronos morph samples (AnimatedMorphCube, MorphStressTest, SimpleMorph):
// every one of them animates morph weights and has no skin, and the library refused the file
// outright — "the mesh has no skin" — so `engine-view --morph-animate` had no curve to play and
// drew the default weights at every frame. An animation none of whose channels lands in a skin's
// palette was also skipped whole, weight tracks and all. Both now load as a skeleton-less clip.
TEST_CASE("animation library: morph-weight curves load with no skin, as a skeleton-less clip") {
  assets::MeshData mesh;
  mesh.nodes.resize(1);
  mesh.morph.resize(2);  // the channel array the curve's `morph_channel` indexes

  assets::Animation square;
  square.name = "Square";
  square.duration = 1.0f;
  assets::AnimationSampler weights;
  weights.times = {0.0f, 1.0f};
  weights.values = {0.0f, 0.0f, 1.0f, 0.5f};  // two targets per key, as glTF writes them
  weights.interpolation = assets::k_interp_linear;
  weights.components = 2;
  square.samplers.push_back(weights);
  assets::AnimationSampler slide;  // a translation of a node that is no joint: skipped, and counted
  slide.times = {0.0f, 1.0f};
  slide.values = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
  slide.components = 3;
  square.samplers.push_back(slide);
  square.channels.push_back(assets::AnimationChannel{
      .node = 0, .sampler = 0, .path = assets::k_path_weights, .morph_channel = 0});
  square.channels.push_back(assets::AnimationChannel{
      .node = 0, .sampler = 1, .path = assets::k_path_translation, .morph_channel = 0});
  mesh.animations.push_back(square);

  Library library;
  LoadStats stats;
  std::string error;
  REQUIRE_MESSAGE(library.add(mesh, "cube", &stats, &error), error);
  CHECK(library.skeleton_count() == 0);
  REQUIRE(library.clip_count() == 1);
  CHECK(stats.clips == 1);
  CHECK(stats.skipped_channels == 1);
  const ClipAsset& clip = library.clip(0);
  CHECK(clip.name == "cube/Square");
  CHECK(clip.skeleton == Library::k_not_found);
  CHECK(clip.clip.joint_count == 0);
  CHECK(clip.clip.tracks.empty());
  REQUIRE(clip.clip.weight_tracks.size() == 1);
  CHECK(clip.clip.morph_count == 2);

  // The curve plays: halfway through, both targets are halfway to their second key.
  f32 played[2] = {-1.0f, -1.0f};
  clip.clip.sample_weights(0.5f, std::span<f32>(played, 2), false);
  CHECK(played[0] == doctest::Approx(0.5f));
  CHECK(played[1] == doctest::Approx(0.25f));

  // A file with neither a skin nor a weight curve still has nothing to give, and still says so.
  assets::MeshData rigid;
  rigid.nodes.resize(1);
  assets::Animation spin;
  spin.samplers.push_back(slide);
  spin.channels.push_back(assets::AnimationChannel{.node = 0, .sampler = 0});
  rigid.animations.push_back(spin);
  Library refuses;
  CHECK_FALSE(refuses.add(rigid, "prop", nullptr, &error));
  CHECK(error.find("no skin") != std::string::npos);
}

TEST_CASE("animation: a player advances by the step and loops exactly") {
  Fixture fixture;
  ecs::SimWorld sim(world_config());
  AnimationSystem animation(fixture.library);
  animation.install(sim);

  const flecs::entity entity = sim.world().entity("dancer");
  REQUIRE(animation.attach(entity, fixture.skeleton, fixture.walk));

  sim.step();
  CHECK(entity.try_get<AnimationPlayer>()->time == doctest::Approx(1.0f / k_hz));
  CHECK(animation.stats().players == 1);
  CHECK(animation.stats().sampled == 1);
  CHECK(animation.stats().skinned == 1);

  for (u32 i = 1; i < k_steps_per_clip; ++i)
    sim.step();
  // Exactly one clip's worth of steps wraps to exactly zero, not to an epsilon: the step and the
  // duration are both exact binary fractions and the wrap is one `fmod`.
  CHECK(entity.try_get<AnimationPlayer>()->time == 0.0f);

  for (u32 i = 0; i < k_steps_per_clip / 2; ++i)
    sim.step();
  CHECK(entity.try_get<AnimationPlayer>()->time == doctest::Approx(0.5f));

  // Speed multiplies the step, and the wrap is still exact.
  entity.try_get_mut<AnimationPlayer>()->speed = 2.0f;
  entity.try_get_mut<AnimationPlayer>()->time = 0.0f;
  for (u32 i = 0; i < k_steps_per_clip / 2; ++i)
    sim.step();
  CHECK(entity.try_get<AnimationPlayer>()->time == 0.0f);

  // Without looping the playhead holds the clip's last pose instead of wrapping.
  entity.try_get_mut<AnimationPlayer>()->looping = false;
  entity.try_get_mut<AnimationPlayer>()->speed = 1.0f;
  for (u32 i = 0; i < k_steps_per_clip * 2; ++i)
    sim.step();
  CHECK(entity.try_get<AnimationPlayer>()->time == doctest::Approx(test_fixture::k_clip_seconds));
}

TEST_CASE("animation: the pooled pose is the clip sampled directly, and the matrices are anim's") {
  Fixture fixture;
  ecs::SimWorld sim(world_config());
  AnimationSystem animation(fixture.library);
  animation.install(sim);

  const flecs::entity entity = sim.world().entity("dancer");
  REQUIRE(animation.attach(entity, fixture.skeleton, fixture.walk));
  for (u32 i = 0; i < 13; ++i)
    sim.step();

  const SkeletonInstance& instance = *entity.try_get<SkeletonInstance>();
  const AnimationPlayer& player = *entity.try_get<AnimationPlayer>();
  REQUIRE(instance.pose_slot != k_no_slot);
  CHECK(instance.joints == 2);

  anim::Pose expected;
  sample_directly(fixture.library, instance.skeleton_index, instance.clip_index, player.time,
                  player.looping, expected);
  const anim::ConstPoseView pooled = animation.poses().pose(instance.pose_slot);
  REQUIRE(pooled.joint_count() == expected.joint_count());
  for (u32 j = 0; j < expected.joint_count(); ++j) {
    CHECK(near(pooled.translation[j], expected.translation[j]));
    CHECK(near(pooled.rotation[j], expected.rotation[j]));
    CHECK(near(pooled.scale[j], expected.scale[j]));
  }

  // And the matrices are what `anim::skinning_matrices` makes of that pose — the capability adds
  // the pool and the tick, not a second implementation of the arithmetic.
  const anim::Skeleton& skeleton = fixture.library.skeleton_data(instance.skeleton_index);
  Vector<Mat4> model(skeleton.joint_count(), Mat4::identity());
  Vector<anim::JointMatrix> reference(skeleton.joint_count(), anim::JointMatrix{});
  anim::local_to_model(skeleton, expected, std::span<Mat4>(model.data(), model.size()));
  anim::skinning_matrices(
      std::span<const Mat4>(model.data(), model.size()),
      std::span<const Mat4>(skeleton.inverse_bind.data(), skeleton.inverse_bind.size()),
      std::span<anim::JointMatrix>(reference.data(), reference.size()));

  const std::span<const anim::JointMatrix> built = animation.poses().matrices(instance.pose_slot);
  REQUIRE(built.size() == reference.size());
  for (u32 j = 0; j < reference.size(); ++j) {
    for (u32 r = 0; r < 3; ++r)
      CHECK(near(built[j].rows[r].xyz(), reference[j].rows[r].xyz()));
  }

  // The renderer contract: the instance's run is inside the one span, at the offsets the pool
  // reports, and that span is what `gfx::DeformDesc::joints` would be based on.
  const std::span<const anim::JointMatrix> all = animation.joint_matrices();
  CHECK(all.size() == animation.poses().joint_capacity());
  CHECK(animation.first_joint(instance.pose_slot) + animation.joint_count(instance.pose_slot) <=
        all.size());
  CHECK(built.data() == all.data() + animation.first_joint(instance.pose_slot));
}

TEST_CASE("animation: a cross-fade blends the two clips and then becomes the clip") {
  Fixture fixture;
  ecs::SimWorld sim(world_config());
  AnimationSystem animation(fixture.library);
  animation.install(sim);

  const flecs::entity entity = sim.world().entity("dancer");
  REQUIRE(animation.attach(entity, fixture.skeleton, fixture.walk));
  for (u32 i = 0; i < 8; ++i)
    sim.step();

  // Half a clip's worth of fade, so the midpoint lands on a step boundary.
  const f32 fade_seconds = 0.5f;
  REQUIRE(animation.play(entity, fixture.idle, fade_seconds));
  CHECK(entity.try_get<AnimationPlayer>()->fade_clip == fixture.idle);

  for (u32 i = 0; i < 8; ++i)
    sim.step();
  {
    const SkeletonInstance& instance = *entity.try_get<SkeletonInstance>();
    const AnimationPlayer& player = *entity.try_get<AnimationPlayer>();
    REQUIRE_FALSE(player.fade_clip.is_null());
    const f32 t = player.fade_elapsed / player.fade_seconds;
    CHECK(t > 0.0f);
    CHECK(t < 1.0f);

    anim::Pose from;
    anim::Pose to;
    sample_directly(fixture.library, instance.skeleton_index, instance.clip_index, player.time,
                    player.looping, from);
    sample_directly(fixture.library, instance.skeleton_index, instance.fade_index, player.fade_time,
                    player.fade_looping, to);
    anim::Pose expected;
    anim::blend(from, to, t, expected);

    const anim::ConstPoseView pooled = animation.poses().pose(instance.pose_slot);
    for (u32 j = 0; j < expected.joint_count(); ++j) {
      CHECK(near(pooled.translation[j], expected.translation[j]));
      CHECK(near(pooled.rotation[j], expected.rotation[j]));
    }
    // The blend is real: the root is somewhere between the two clips' translations, and not at
    // either end.
    CHECK(pooled.translation[0].y > 0.0f);
  }

  // Run past the end of the fade: the incoming clip becomes the clip, and nothing is left behind.
  for (u32 i = 0; i < k_steps_per_clip; ++i)
    sim.step();
  const AnimationPlayer& player = *entity.try_get<AnimationPlayer>();
  CHECK(player.clip == fixture.idle);
  CHECK(player.fade_clip.is_null());
  CHECK(player.fade_seconds == 0.0f);
  CHECK(player.fade_elapsed == 0.0f);
  // And the cached index followed the clip, without a second lookup having been needed.
  CHECK(entity.try_get<SkeletonInstance>()->clip_index == fixture.library.clip_index(fixture.idle));
  CHECK(entity.try_get<SkeletonInstance>()->fade_index == Library::k_not_found);
}

TEST_CASE("animation: a weight below one fades the player toward the bind pose") {
  Fixture fixture;
  ecs::SimWorld sim(world_config());
  AnimationSystem animation(fixture.library);
  animation.install(sim);

  const flecs::entity entity = sim.world().entity("dancer");
  REQUIRE(animation.attach(entity, fixture.skeleton, fixture.idle));
  entity.try_get_mut<AnimationPlayer>()->weight = 0.0f;
  for (u32 i = 0; i < 16; ++i)
    sim.step();

  const SkeletonInstance& instance = *entity.try_get<SkeletonInstance>();
  const anim::Skeleton& skeleton = fixture.library.skeleton_data(instance.skeleton_index);
  anim::Pose rest;
  anim::rest_pose(skeleton, rest);
  const anim::ConstPoseView pooled = animation.poses().pose(instance.pose_slot);
  for (u32 j = 0; j < rest.joint_count(); ++j)
    CHECK(near(pooled.translation[j], rest.translation[j]));

  // At full weight the clip reaches the pose, which is what says the zero case was the weight and
  // not a clip that does nothing.
  entity.try_get_mut<AnimationPlayer>()->weight = 1.0f;
  sim.step();
  CHECK(pooled.translation[0].y > 0.0f);
}

TEST_CASE("animation: WorldCommands creates an animated entity from JSON by schema type name") {
  Fixture fixture;
  ecs::SimWorld sim(world_config());
  AnimationSystem animation(fixture.library);
  animation.install(sim);
  flecs::world& world = sim.world();

  ecs::WorldCommands commands(world);
  const Id128 id = Id128::from_seed(0xa11ce, 1);
  commands.create(id);
  // The component is named exactly as the IDL spells it, and the payload goes through the
  // reflection schemac generated — this capability wrote no per-component protocol code.
  Vector<schema::Diagnostic> diagnostics;
  const std::string payload = R"({"clip":")" + hex(fixture.walk) +
                              R"(","time":0.25,"speed":1.0,"weight":1.0,"looping":true})";
  REQUIRE(commands.set_json(id, "engine.animation.AnimationPlayer", parse(payload), &diagnostics));
  const ecs::CommandStats stats = commands.apply();
  CHECK(stats.created == 1);
  CHECK(stats.set == 1);
  CHECK(stats.failed == 0);

  const flecs::entity entity = ecs::entity_for(world, id);
  REQUIRE(entity.is_valid());
  REQUIRE(entity.try_get<AnimationPlayer>() != nullptr);
  CHECK(entity.try_get<AnimationPlayer>()->clip == fixture.walk);
  CHECK(entity.try_get<AnimationPlayer>()->time == doctest::Approx(0.25f));

  // The player arrived from outside with no instance and no cached clip index; attaching gives it
  // a skeleton and a slot, and the tick resolves the handle it was given.
  REQUIRE(animation.attach(entity, fixture.skeleton, entity.try_get<AnimationPlayer>()->clip));
  CHECK(entity.try_get<AnimationPlayer>()->time == doctest::Approx(0.25f));
  sim.step();

  const SkeletonInstance& instance = *entity.try_get<SkeletonInstance>();
  CHECK(instance.clip_index == fixture.library.clip_index(fixture.walk));
  anim::Pose expected;
  sample_directly(fixture.library, instance.skeleton_index, instance.clip_index,
                  entity.try_get<AnimationPlayer>()->time, true, expected);
  const anim::ConstPoseView pooled = animation.poses().pose(instance.pose_slot);
  CHECK(near(pooled.rotation[1], expected.rotation[1]));

  // A type the schema does not declare cannot be named at all, which is seam 1 doing its job.
  CHECK_FALSE(commands.set_json(id, "engine.animation.NoSuchThing", parse("{}")));
}

TEST_CASE("animation: with no instances the capability is registered and costs nothing") {
  Fixture fixture;
  ecs::SimWorld sim(world_config());
  AnimationSystem animation(fixture.library);
  animation.install(sim);

  // The three descriptors are in the world's registry, declaring what they touch, and the tick
  // runs them over nothing. `zero cost when unused` at the level below the build switch: no
  // instances means no component, no query match, no pool.
  const ecs::SystemRegistry& registry = ecs::systems(sim.world());
  CHECK(registry.size() == AnimationSystem::k_system_count);
  CHECK(registry.count_in(sim::TickPhase::Systems) == AnimationSystem::k_system_count);
  CHECK(registry.count_in(sim::TickPhase::PostPhysics) == 0);

  for (u32 i = 0; i < 4; ++i)
    sim.step();
  CHECK(animation.stats().players == 0);
  CHECK(animation.stats().sampled == 0);
  CHECK(animation.poses().live_count() == 0);
  CHECK(animation.poses().bytes() == 0);
  CHECK(animation.joint_matrices().empty());
}

TEST_CASE("animation: the descriptors declare what the queries touch") {
  Fixture fixture;
  ecs::SimWorld sim(world_config());
  AnimationSystem animation(fixture.library);
  animation.install(sim);
  flecs::world& world = sim.world();

  const std::span<const sim::SystemDesc> descs = animation.descriptors();
  REQUIRE(descs.size() == AnimationSystem::k_system_count);
  const ecs::SystemRegistry& registry = ecs::systems(world);
  for (const ecs::RegisteredSystem& row : registry.all()) {
    const ecs::QueryAccess access = ecs::query_access(world, row.system);
    CHECK(access.unregistered_terms == 0);
    // Every write the query does is declared, which is the half of the seam-2 check that costs
    // correctness when it is wrong.
    for (u32 word = 0; word < sim::k_component_mask_words; ++word)
      CHECK((access.writes.words[word] & ~row.desc.writes.words[word]) == 0);
  }
  // The stance is the system's, not the phase's: the matrices are `Derived` because nothing reads
  // them back into gameplay, and they now say so from inside the phase they belong in.
  CHECK(descs[0].determinism == sim::Determinism::Hashed);
  CHECK(descs[1].determinism == sim::Determinism::Hashed);
  CHECK(descs[2].determinism == sim::Determinism::Derived);

  // Every ordering this capability needs is declared. `advance_players` writes the playhead and
  // `sample_poses` reads it — a component conflict. `sample_poses` writes the pose pool and
  // `build_skinning_matrices` reads it — a *resource* conflict, which is the whole point of
  // `SystemDesc::reads_resources`/`writes_resources`: the pool is not a component and must not be.
  for (const sim::SystemDesc& desc : descs)
    CHECK(desc.phase == sim::TickPhase::Systems);
  CHECK(descs[0].writes.intersects(descs[1].reads));
  CHECK(descs[1].writes_resources.intersects(descs[2].reads_resources));

  // And the scheduler that owns the rule agrees: three systems, three waves, in declaration order.
  sim::SimScheduler scheduler;
  for (const sim::SystemDesc& desc : descs)
    scheduler.add_system(desc);
  CHECK(scheduler.wave_count(sim::TickPhase::Systems) == 3);
  CHECK(scheduler.wave_of(0) == 0);
  CHECK(scheduler.wave_of(1) == 1);
  CHECK(scheduler.wave_of(2) == 2);
}

TEST_CASE("animation: two runs and eight workers give bit-identical matrices") {
  // The property the whole design rests on: an instance writes only its own run of the pool, slots
  // come off a LIFO free list in a single-threaded phase, and nothing accumulates across
  // instances — so the worker count cannot reach the output.
  Fixture fixture;
  constexpr u32 k_instances = 64;

  const auto run = [&fixture](u32 workers, std::vector<anim::JointMatrix>& out) {
    jobs::JobSystemConfig config;
    config.performance_workers = workers;
    config.efficiency_workers = 1;
    jobs::JobSystem js(config);
    ecs::JobOsApi adapter(js);

    ecs::SimWorld sim(world_config());
    AnimationSystem animation(fixture.library);
    animation.install(sim);
    if (workers > 1) {
      const u32 requested = workers <= adapter.max_workers() ? workers : adapter.max_workers();
      REQUIRE(ecs::set_workers(sim.world(), requested));
    }

    for (u32 i = 0; i < k_instances; ++i) {
      const flecs::entity entity = sim.world().entity();
      REQUIRE(
          animation.attach(entity, fixture.skeleton, (i % 2) == 0 ? fixture.walk : fixture.idle));
      // Different playheads, so two instances are never accidentally identical.
      entity.try_get_mut<AnimationPlayer>()->time = static_cast<f32>(i) / 128.0f;
      entity.try_get_mut<AnimationPlayer>()->speed = 1.0f + static_cast<f32>(i % 3) * 0.25f;
    }
    for (u32 step = 0; step < 16; ++step)
      sim.step();

    const std::span<const anim::JointMatrix> matrices = animation.joint_matrices();
    out.assign(matrices.begin(), matrices.end());
  };

  std::vector<anim::JointMatrix> one;
  std::vector<anim::JointMatrix> one_again;
  std::vector<anim::JointMatrix> eight;
  run(1, one);
  run(1, one_again);
  run(8, eight);

  REQUIRE(one.size() == k_instances * 2);
  REQUIRE(one_again.size() == one.size());
  REQUIRE(eight.size() == one.size());
  CHECK(std::memcmp(one.data(), one_again.data(), one.size() * sizeof(anim::JointMatrix)) == 0);
  CHECK(std::memcmp(one.data(), eight.data(), one.size() * sizeof(anim::JointMatrix)) == 0);
}

TEST_CASE("animation: the engine's scheduler ticks it in its phase, at the same step, to the bit") {
  // ADR-0038 (proposed): `sim::SimScheduler` owns the clock and the phase order and runs each
  // phase's flecs systems through `ecs::ScheduledTick`. The capability declared its three systems
  // in `Systems` and did not change for it, so the output has to be the same bytes as flecs' own
  // pipeline gives, with the playheads advanced by the scheduler's step.
  Fixture fixture;
  static constexpr u32 k_instances = 16;
  const auto run = [&fixture](bool scheduled, std::vector<anim::JointMatrix>& matrices,
                              std::vector<f32>& playheads) {
    ecs::SimWorld sim(world_config());
    AnimationSystem animation(fixture.library);
    animation.install(sim);
    std::vector<Id128> ids;
    for (u32 i = 0; i < k_instances; ++i) {
      const Id128 id = Id128::from_parts(0xA1, i + 1u);
      ids.push_back(id);
      const flecs::entity entity = ecs::create_entity(sim.world(), id);
      REQUIRE(
          animation.attach(entity, fixture.skeleton, (i % 2) == 0 ? fixture.walk : fixture.idle));
      entity.try_get_mut<AnimationPlayer>()->time = static_cast<f32>(i) / 64.0f;
    }
    sim::SimSchedulerConfig config;
    config.hz = k_hz;
    sim::SimScheduler scheduler(config);
    if (scheduled) {
      ecs::ScheduledTick tick(sim, scheduler);
      for (u32 step = 0; step < 20; ++step)
        tick.step();
      CHECK(tick.phases_run() == 20);  // one phase a tick holds flecs systems: Systems
      CHECK(sim.tick().value == scheduler.tick().value);
    } else {
      for (u32 step = 0; step < 20; ++step)
        sim.step();
    }
    CHECK(animation.stats().players == k_instances);
    const std::span<const anim::JointMatrix> span = animation.joint_matrices();
    matrices.assign(span.begin(), span.end());
    for (const Id128& id : ids)
      playheads.push_back(ecs::entity_for(sim.world(), id).get<AnimationPlayer>().time);
  };

  std::vector<anim::JointMatrix> pipeline_matrices, scheduled_matrices;
  std::vector<f32> pipeline_playheads, scheduled_playheads;
  run(false, pipeline_matrices, pipeline_playheads);
  run(true, scheduled_matrices, scheduled_playheads);
  REQUIRE(pipeline_matrices.size() == k_instances * 2);
  REQUIRE(scheduled_matrices.size() == pipeline_matrices.size());
  CHECK(std::memcmp(pipeline_matrices.data(), scheduled_matrices.data(),
                    pipeline_matrices.size() * sizeof(anim::JointMatrix)) == 0);
  CHECK(pipeline_playheads == scheduled_playheads);
  // The step is unchanged: 20 steps of a 64th of a second from each instance's start, wrapped.
  CHECK(scheduled_playheads[0] == doctest::Approx(20.0f / 64.0f));
}

TEST_CASE("animation: a host with no flecs drives the capability through Id128 alone") {
  // The surface `engine-view --animate` uses, and the one a game outside `systems/` copies:
  // create the entity with `ecs::WorldCommands`, attach, set the playhead, read the run back, and
  // hand the renderer the span. Nothing here names a flecs type, because `apps/` may not see one
  // (ADR-0028 seam 5) — that is the whole point of these four overloads.
  Fixture fixture;
  ecs::SimWorld sim(world_config());
  AnimationSystem animation(fixture.library);
  animation.install(sim);

  ecs::WorldCommands commands(sim.world());
  const Id128 a = Id128::from_seed(0xc0ffee, 1);
  const Id128 b = Id128::from_seed(0xc0ffee, 2);
  commands.create(a);
  commands.create(b);
  commands.apply();

  REQUIRE(animation.attach(a, fixture.skeleton, fixture.walk));
  REQUIRE(animation.attach(b, fixture.skeleton, fixture.walk));
  // An id this world does not hold is refused rather than materialized.
  CHECK_FALSE(animation.attach(Id128::from_seed(0xc0ffee, 99), fixture.skeleton, fixture.walk));

  // Two instances of one clip, one phase-shifted: the crowd trick, and the reason `set_playhead`
  // exists at all.
  REQUIRE(animation.set_playhead(a, 0.0f, 1.0f));
  REQUIRE(animation.set_playhead(b, 0.5f * test_fixture::k_clip_seconds, 1.0f));

  u32 first_a = 0;
  u32 count_a = 0;
  u32 first_b = 0;
  u32 count_b = 0;
  REQUIRE(animation.joint_run(a, first_a, count_a));
  REQUIRE(animation.joint_run(b, first_b, count_b));
  CHECK(count_a == 2);
  CHECK(count_b == 2);
  CHECK(first_a != first_b);  // one arena, a run each

  sim.step();
  const std::span<const anim::JointMatrix> span = animation.joint_matrices();
  REQUIRE(first_a + count_a <= span.size());
  REQUIRE(first_b + count_b <= span.size());
  // The runs are the pool's, and the pool is the span: this is exactly the arithmetic a renderer
  // does to point one instance's `gfx::DeformDesc::joints` at its own matrices.
  CHECK(span.data() + first_a == animation.poses().matrices(0).data());
  // Out of phase, so the two instances are genuinely different characters and not two draws of
  // one. A one-joint compare is enough: the walk clip turns joint 1.
  bool differ = false;
  for (u32 r = 0; r < 3 && !differ; ++r) {
    const Vec4 x = span[first_a + 1].rows[r];
    const Vec4 y = span[first_b + 1].rows[r];
    differ = std::fabs(x.x - y.x) > 1.0e-4f || std::fabs(x.y - y.y) > 1.0e-4f ||
             std::fabs(x.z - y.z) > 1.0e-4f || std::fabs(x.w - y.w) > 1.0e-4f;
  }
  CHECK(differ);

  // An entity with no slot answers false rather than a run of zeros a caller could mistake for
  // one, which is what a renderer reads as "this instance draws its rest pose".
  animation.set_tier(a, 3);
  u32 first = 0;
  u32 count = 0;
  CHECK_FALSE(animation.joint_run(a, first, count));
  CHECK(count == 0);
}

TEST_CASE("animation: the displacement bound covers every vertex at every phase of a clip") {
  // The number a renderer inflates a skinned instance's cluster spheres by
  // (`gfx::InstanceDesc::bounds_padding`). It has one job — never be smaller than the truth — so
  // the case measures the truth by brute force and checks the bound against it.
  Fixture fixture;
  const u32 skeleton_index = fixture.library.skeleton_index(fixture.skeleton);
  REQUIRE(skeleton_index != Library::k_not_found);
  const anim::Skeleton& skeleton = fixture.library.skeleton_data(skeleton_index);
  const u32 clip_index = fixture.library.clip_index(fixture.walk);
  REQUIRE(clip_index != Library::k_not_found);
  const anim::Clip& clip = fixture.library.clip_data(clip_index);

  // A bar of vertices up the two-bone rig, bound root-to-tip by height, and its influence spheres.
  Vector<Vec3> positions;
  Vector<geometry::SkinBinding> bindings;
  for (u32 i = 0; i <= 16; ++i) {
    const f32 t = static_cast<f32>(i) / 16.0f;
    const u32 joints[4] = {0, 1, 0, 0};
    const f32 weights[4] = {1.0f - t, t, 0.0f, 0.0f};
    const geometry::SkinBinding binding = geometry::make_skin_binding(joints, weights);
    for (const f32 x : {-0.25f, 0.25f}) {
      positions.push_back(Vec3{x, t * 2.0f, 0.0f});
      bindings.push_back(binding);
    }
  }
  JointBounds bounds;
  joint_influence_bounds(std::span<const Vec3>(positions.data(), positions.size()),
                         std::span<const geometry::SkinBinding>(bindings.data(), bindings.size()),
                         skeleton.joint_count(), bounds);
  REQUIRE(bounds.sphere.size() == skeleton.joint_count());
  for (const Vec4& sphere : bounds.sphere)
    CHECK(sphere.w >= 0.0f);  // both joints of this rig carry weight somewhere

  const f32 bound = clip_displacement_bound(skeleton, clip, bounds);
  CHECK(bound > 0.0f);

  // The truth: the largest distance any vertex actually travels, sampled far more finely than the
  // bound's own grid so that a bound tuned to its own sample points would fail here.
  anim::Pose pose;
  Vector<Mat4> model(skeleton.joint_count(), Mat4::identity());
  Vector<anim::JointMatrix> matrices(skeleton.joint_count(), anim::JointMatrix{});
  Vector<Vec3> moved(positions.size());
  f32 worst = 0.0f;
  constexpr u32 k_fine = 997;  // prime, so it shares no sample with the bound's uniform grid
  for (u32 i = 0; i < k_fine; ++i) {
    const f32 time = clip.duration * static_cast<f32>(i) / static_cast<f32>(k_fine);
    anim::rest_pose(skeleton, pose);
    clip.sample(time, pose, true);
    anim::local_to_model(skeleton, pose, std::span<Mat4>(model.data(), model.size()));
    anim::skinning_matrices(
        std::span<const Mat4>(model.data(), model.size()),
        std::span<const Mat4>(skeleton.inverse_bind.data(), skeleton.inverse_bind.size()),
        std::span<anim::JointMatrix>(matrices.data(), matrices.size()));
    anim::skin_positions(std::span<const Vec3>(positions.data(), positions.size()),
                         std::span<const geometry::SkinBinding>(bindings.data(), bindings.size()),
                         std::span<const anim::JointMatrix>(matrices.data(), matrices.size()),
                         std::span<Vec3>(moved.data(), moved.size()));
    for (u32 v = 0; v < moved.size(); ++v)
      worst = std::max(worst, length(moved[v] - positions[v]));
  }
  MESSAGE("displacement: bound " << bound << ", measured worst " << worst << " ("
                                 << (bound / std::max(worst, 1.0e-6f)) << "x)");
  CHECK(bound >= worst);         // conservative, which is the only thing it must be
  CHECK(bound <= 4.0f * worst);  // and not so loose that culling stops meaning anything

  // A skeleton with no clip moves nothing, and a clip over a rig nothing binds to moves nothing.
  JointBounds empty;
  CHECK(clip_displacement_bound(skeleton, clip, empty) == 0.0f);
}
