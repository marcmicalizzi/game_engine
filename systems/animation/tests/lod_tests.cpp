// The capability's LOD policy: the tier bands, what each tier costs, and the transitions in and
// out of LOD3 — where the pool slot is released and re-acquired and the playhead is advanced
// analytically. The interesting assertions are the ones that say the policy cannot drift from the
// engine's reference (`sim::TierAssignment`) and that a promotion lands where ticking would have.
#include "animation_glb.h"

#include <domain/ecs/identity.h>
#include <domain/sim/scheduler.h>
#include <domain/sim/tiers.h>
#include <systems/animation/animation.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::animation;

namespace {

constexpr u32 k_hz = 64;

struct Fixture {
  engine::test::TempDir tmp{"engine_animation_lod"};
  Library library;
  Id128 skeleton;
  Id128 walk;

  Fixture() {
    const std::string path = tmp.file("rig.glb");
    REQUIRE(test_fixture::write_animated_glb(path));
    std::string error;
    REQUIRE_MESSAGE(library.load_gltf(path, "rig", nullptr, &error), error);
    skeleton = Library::skeleton_id("rig/bar_skin");
    walk = Library::clip_id("rig/walk");
  }
};

ecs::SimWorldConfig world_config() {
  ecs::SimWorldConfig config;
  config.hz = k_hz;
  return config;
}

}  // namespace

TEST_CASE("animation LOD: each tier costs what the policy says it costs") {
  CHECK(lod_plan(0).divisor == 1);
  CHECK(lod_plan(0).interpolate);
  CHECK(lod_plan(0).skin);
  CHECK(lod_plan(1).divisor == 2);
  CHECK_FALSE(lod_plan(1).interpolate);
  CHECK(lod_plan(1).skin);
  CHECK(lod_plan(2).divisor == 4);
  CHECK_FALSE(lod_plan(2).skin);
  CHECK(lod_plan(2).animated);
  CHECK_FALSE(lod_plan(3).animated);
  CHECK(lod_plan(3).divisor == 0);
  // A tier beyond the policy's own count is the coarsest one, not undefined behaviour.
  CHECK_FALSE(lod_plan(200).animated);

  // Work per tick per instance is monotonic in the tier, which is the property that makes the
  // policy worth having at all. A divisor of 0 is "never", so it reads as infinity here rather
  // than as the smallest number.
  const auto period = [](const LodPlan& plan) {
    return plan.divisor == 0 ? 0xFFFFu : static_cast<u32>(plan.divisor);
  };
  for (u8 tier = 1; tier < k_tier_count; ++tier) {
    const LodPlan finer = lod_plan(static_cast<u8>(tier - 1));
    const LodPlan coarser = lod_plan(tier);
    CHECK(period(coarser) >= period(finer));
    CHECK(static_cast<int>(coarser.skin) <= static_cast<int>(finer.skin));
    CHECK(static_cast<int>(coarser.interpolate) <= static_cast<int>(finer.interpolate));
  }
}

TEST_CASE("animation LOD: the policy agrees with sim::TierAssignment about every band") {
  // A capability owns its bands, not its band *arithmetic*: the engine's reference implementation
  // is what the plan says every capability's policy has to agree with (ADR-0027, plan 05 §5.4).
  const sim::TierParams params = tier_params();
  CHECK(params.tier_count == k_tier_count);
  CHECK(params.boundaries[0] < params.boundaries[1]);
  CHECK(params.boundaries[1] < params.boundaries[2]);

  for (f32 score = 0.0f; score < 400.0f; score += 0.37f) {
    const u8 tight = sim::TierAssignment::tier_of(score, params);
    // Promotion tests the tight boundary: from the coarsest tier, the answer is the tight tier.
    CHECK(lod_tier(score, static_cast<u8>(k_tier_count - 1), params) == tight);
    // Demotion tests the widened one, so a score inside the hysteresis band does not move.
    const u8 from_tight = lod_tier(score, tight, params);
    CHECK(from_tight == tight);
  }

  // And the band actually holds an entity through a boundary walk that would otherwise oscillate.
  const f32 boundary = params.boundaries[0];
  u8 tier = 0;
  for (u32 i = 0; i < 8; ++i) {
    tier = lod_tier(boundary * 1.02f, tier, params);
    CHECK(tier == 0);
    tier = lod_tier(boundary * 0.98f, tier, params);
    CHECK(tier == 0);
  }
  // Past the widened boundary it does move, once.
  tier = lod_tier(boundary * (1.0f + params.hysteresis) * 1.01f, tier, params);
  CHECK(tier == 1);
}

TEST_CASE("animation LOD: demotion releases the slot and promotion re-acquires it in time") {
  Fixture fixture;
  ecs::SimWorld sim(world_config());
  AnimationSystem animation(fixture.library);
  animation.install(sim);

  const flecs::entity entity = sim.world().entity("dancer");
  REQUIRE(animation.attach(entity, fixture.skeleton, fixture.walk));
  const u32 first_slot = entity.try_get<SkeletonInstance>()->pose_slot;
  REQUIRE(first_slot != k_no_slot);
  CHECK(animation.poses().live_count() == 1);

  for (u32 i = 0; i < 8; ++i)
    sim.step();
  const f32 frozen_time = entity.try_get<AnimationPlayer>()->time;
  const GameTime frozen_at = sim.game_time();

  animation.set_tier(entity, 3);
  CHECK(entity.try_get<SkeletonInstance>()->pose_slot == k_no_slot);
  CHECK(entity.try_get<AnimationLod>()->tier == 3);
  CHECK(entity.try_get<AnimationLod>()->divisor == 0);
  CHECK(animation.poses().live_count() == 0);
  // The arena keeps the run: a released slot is reused, not returned, so steady state allocates
  // nothing.
  CHECK(animation.poses().joint_capacity() == 2);

  // Nothing happens to a frozen instance, however long the world runs.
  constexpr u32 k_frozen_steps = 100;
  for (u32 i = 0; i < k_frozen_steps; ++i)
    sim.step();
  CHECK(entity.try_get<AnimationPlayer>()->time == frozen_time);
  CHECK(animation.stats().sampled == 0);
  CHECK(animation.stats().skinned == 0);

  const GameTime resumed_at = sim.game_time();
  animation.set_tier(entity, 0);
  const SkeletonInstance& instance = *entity.try_get<SkeletonInstance>();
  CHECK(instance.pose_slot == first_slot);  // LIFO reuse: the same run comes back
  CHECK(animation.poses().live_count() == 1);
  CHECK(animation.poses().joint_capacity() == 2);

  // **Analytically, not by rewinding.** The playhead is where it would have been had the instance
  // ticked through the gap: the same closed form the tick uses, with the whole gap as its delta.
  const f32 gap = static_cast<f32>(GameTime{resumed_at.us - frozen_at.us}.seconds());
  const f32 duration = fixture.library.clip_data(instance.clip_index).duration;
  f32 expected = std::fmod(frozen_time + gap, duration);
  if (expected < 0.0f) expected += duration;
  CHECK(entity.try_get<AnimationPlayer>()->time == doctest::Approx(expected).epsilon(1e-5));
  // It is not simply where it was, which is the bug this replaces.
  CHECK(std::fabs(entity.try_get<AnimationPlayer>()->time - frozen_time) > 1.0e-3f);

  // And the slot it came back into is a pose, not whatever the previous occupant left: the
  // promotion fills it with the rest pose before the first sample.
  sim.step();
  CHECK(animation.stats().sampled == 1);
}

TEST_CASE("animation LOD: the divisor staggers the work and keeps the average rate") {
  Fixture fixture;
  ecs::SimWorld sim(world_config());
  AnimationSystem animation(fixture.library);
  animation.install(sim);

  constexpr u32 k_instances = 16;
  std::vector<flecs::entity> entities;
  for (u32 i = 0; i < k_instances; ++i) {
    const flecs::entity entity = sim.world().entity();
    REQUIRE(animation.attach(entity, fixture.skeleton, fixture.walk));
    animation.set_tier(entity, 2);  // every fourth tick, sampled but not skinned
    entities.push_back(entity);
  }

  u32 sampled_total = 0;
  u32 skinned_total = 0;
  u32 busiest_tick = 0;
  for (u32 step = 0; step < 4; ++step) {
    sim.step();
    sampled_total += animation.stats().sampled;
    skinned_total += animation.stats().skinned;
    if (animation.stats().sampled > busiest_tick) busiest_tick = animation.stats().sampled;
  }
  // Every instance is sampled exactly once over the divisor's cycle...
  CHECK(sampled_total == k_instances);
  // ...and the cycle is spread, rather than all of it landing on one tick.
  CHECK(busiest_tick <= k_instances / 4 + 1);
  // LOD2 does not skin: the pose exists for gameplay and nothing on screen deforms from it.
  CHECK(skinned_total == 0);

  // The playhead still advances at the clip's own rate: an instance visited every fourth tick owes
  // four ticks of clip time when it is visited.
  const f32 expected = 4.0f / static_cast<f32>(k_hz);
  for (const flecs::entity& entity : entities)
    CHECK(entity.try_get<AnimationPlayer>()->time == doctest::Approx(expected));
}

TEST_CASE("animation LOD: sim::MaterializationHooks drive the transitions") {
  // The registration point ADR-0027 names for the materialization contract, driven by the
  // scheduler that owns it rather than by this capability calling itself.
  Fixture fixture;
  ecs::SimWorld sim(world_config());
  AnimationSystem animation(fixture.library);
  animation.install(sim);

  sim::SimScheduler scheduler;
  const u16 row = scheduler.add_hooks(animation.hooks());
  CHECK(row == 0);
  CHECK(scheduler.hooks_count() == 1);

  constexpr u32 k_instances = 8;
  std::vector<flecs::entity> entities;
  Vector<sim::EntityHandle> ids;
  for (u32 i = 0; i < k_instances; ++i) {
    const flecs::entity entity = sim.world().entity();
    REQUIRE(animation.attach(entity, fixture.skeleton, fixture.walk));
    entities.push_back(entity);
    // `promote`/`demote`/`dematerialize` name a live entity with a `sim::EntityHandle`, and
    // `domain/ecs` is the only module that knows what is inside one (ADR-0028 seam 3).
    ids.push_back(ecs::handle_of(entity));
  }
  CHECK(animation.poses().live_count() == k_instances);

  // Demote half of them to LOD3 through the hooks table.
  Vector<sim::TierChange> changes;
  for (u32 i = 0; i < k_instances; i += 2)
    changes.push_back(sim::TierChange{i, 0, 3});
  scheduler.apply_tier_changes({changes.data(), changes.size()}, {ids.data(), ids.size()});
  CHECK(animation.poses().live_count() == k_instances / 2);
  for (u32 i = 0; i < k_instances; i += 2)
    CHECK(entities[i].try_get<SkeletonInstance>()->pose_slot == k_no_slot);

  // And promote them back.
  changes.clear();
  for (u32 i = 0; i < k_instances; i += 2)
    changes.push_back(sim::TierChange{i, 3, 0});
  scheduler.apply_tier_changes({changes.data(), changes.size()}, {ids.data(), ids.size()});
  CHECK(animation.poses().live_count() == k_instances);
  for (u32 i = 0; i < k_instances; i += 2) {
    CHECK(entities[i].try_get<SkeletonInstance>()->pose_slot != k_no_slot);
    CHECK(entities[i].try_get<AnimationLod>()->tier == 0);
  }

  // `dematerialize` is the teardown half: the slot goes back and the transient components go with
  // it, while the player — the entity's own state — stays for the save.
  Vector<sim::EntityHandle> going;
  going.push_back(ids[1]);
  scheduler.dematerialize({going.data(), going.size()});
  CHECK(entities[1].try_get<SkeletonInstance>() == nullptr);
  CHECK(entities[1].try_get<AnimationLod>() == nullptr);
  CHECK(entities[1].try_get<AnimationPlayer>() != nullptr);
  CHECK(animation.poses().live_count() == k_instances - 1);
}

TEST_CASE("animation LOD: materialize is named by Id128 and answers with a runtime handle") {
  // The other half of ADR-0028 seam 3, which the hooks table used to leave to each capability: a
  // record out of the store carries the persistent name, and what comes back is the handle the
  // tier changes then act on. The two are different numbers and different types, so the mistake
  // this replaced — reading a `u64` as whichever of the two you assumed — cannot be written.
  Fixture fixture;
  ecs::SimWorld sim(world_config());
  AnimationSystem animation(fixture.library);
  animation.install(sim);

  sim::SimScheduler scheduler;
  scheduler.add_hooks(animation.hooks());

  const Id128 persistent = Id128::from_seed(99, 1);
  const flecs::entity entity = ecs::create_entity(sim.world(), persistent);
  REQUIRE(animation.attach(entity, fixture.skeleton, fixture.walk));
  animation.set_tier(entity, 3);
  REQUIRE(entity.try_get<SkeletonInstance>()->pose_slot == k_no_slot);

  sim::EntityRecord record;
  record.entity = persistent;
  record.tier = 3;
  const sim::EntityHandle handle = scheduler.materialize(record, 0);
  CHECK(handle == ecs::handle_of(entity));
  CHECK(entity.try_get<SkeletonInstance>()->pose_slot != k_no_slot);
  CHECK(entity.try_get<AnimationLod>()->tier == 0);

  // A record naming nothing this world holds gets a null handle rather than a guess, and the
  // scheduler skips it instead of promoting into nothing.
  sim::EntityRecord stranger;
  stranger.entity = Id128::from_seed(99, 2);
  CHECK(scheduler.materialize(stranger, 0).is_null());
}

TEST_CASE("animation LOD: the same transitions give the same slots in two runs") {
  // Slot assignment is a pure function of the acquire/release sequence, which is what the
  // bit-identical matrix test rests on and what would break first if the free list stopped being
  // LIFO or started depending on iteration order.
  Fixture fixture;
  const auto run = [&fixture](std::vector<u32>& out) {
    ecs::SimWorld sim(world_config());
    AnimationSystem animation(fixture.library);
    animation.install(sim);
    std::vector<flecs::entity> entities;
    for (u32 i = 0; i < 12; ++i) {
      const flecs::entity entity = sim.world().entity();
      REQUIRE(animation.attach(entity, fixture.skeleton, fixture.walk));
      entities.push_back(entity);
    }
    // A churn of demotions and promotions in an order that is not the entity order.
    const u32 order[12] = {5, 0, 9, 2, 11, 4, 7, 1, 8, 3, 10, 6};
    for (const u32 i : order)
      animation.set_tier(entities[i], 3);
    for (u32 step = 0; step < 3; ++step)
      sim.step();
    for (u32 i = 0; i < 12; ++i)
      animation.set_tier(entities[order[11 - i]], 0);
    for (const flecs::entity& entity : entities)
      out.push_back(entity.try_get<SkeletonInstance>()->pose_slot);
  };

  std::vector<u32> first;
  std::vector<u32> second;
  run(first);
  run(second);
  REQUIRE(first.size() == 12);
  CHECK(first == second);
  // Every slot is distinct and inside the arena: a free list that handed one slot to two live
  // instances would be silent corruption.
  std::vector<u32> sorted = first;
  std::sort(sorted.begin(), sorted.end());
  CHECK(std::unique(sorted.begin(), sorted.end()) == sorted.end());
}
