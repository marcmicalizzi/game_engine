#include "animation_log.h"

#include <core/base/assert.h>
#include <domain/ecs/components.h>
#include <domain/ecs/identity.h>
#include <foundation/tunables/tunables.h>
#include <systems/animation/animation.h>

#include <cmath>
#include <schemas/animation_ecs.h>

namespace engine::animation {

ENGINE_LOG_CATEGORY_DEFINE(log_animation, "animation");

namespace {

// ---- tunables (ADR-0011) ----------------------------------------------------------------------
//
// The LOD band boundaries, in metres of observer score. They are much tighter than
// `sim::TierParams`' defaults (32 / 128 / 1024) because the thing being decided is different: a
// settlement's economy is worth simulating a kilometre away, and a character's fingers are not
// worth sampling at forty metres. These are the one knob a game is most likely to move, which is
// exactly ADR-0011's case for a tunable rather than a constant.
// `tunables::Float` takes f64: a float literal here is a widening conversion and
// -Wdouble-promotion is an error on both Linux compilers.
tunables::Float lod_near{"animation.lod.near", 12.0, 0.0, 4096.0,
                         "Observer score below which an instance is animated every tick"};
tunables::Float lod_mid{"animation.lod.mid", 40.0, 0.0, 4096.0,
                        "Observer score below which an instance is animated every second tick"};
tunables::Float lod_far{"animation.lod.far", 120.0, 0.0, 8192.0,
                        "Observer score below which an instance is sampled every fourth tick"};
tunables::Float lod_hysteresis{"animation.lod.hysteresis", 0.15, 0.0, 4.0,
                               "Demotion tests the band boundary widened by this fraction"};
tunables::Int lod_max_promotions{"animation.lod.max_promotions", 64, 1, 1 << 20,
                                 "Instances that may re-acquire a pose slot in one tick"};
tunables::Int lod_max_demotions{"animation.lod.max_demotions", 256, 1, 1 << 20,
                                "Instances that may release a pose slot in one tick"};

f32 clamp01(f32 v) noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// The playhead, advanced by `delta` seconds of clip time. Looping wraps into [0, duration) and is
// exact for a delta that is an exact binary fraction of the duration, which is what makes the
// analytic catch-up below equal to having ticked: both go through this one function, one with a
// tick's worth of delta and one with a hundred ticks' worth.
f32 advance_time(f32 time, f32 delta, bool looping, f32 duration) noexcept {
  f32 at = time + delta;
  if (looping && duration > 0.0f) {
    at = std::fmod(at, duration);
    if (at < 0.0f) at += duration;
    return at;
  }
  if (at < 0.0f) return 0.0f;
  return at > duration ? duration : at;
}

// Per-thread scratch. A cross-fade needs somewhere to put the incoming clip's pose before it is
// mixed into the slot, and composing model transforms needs a matrix per joint; both are
// per-instance working sets that must not be shared between flecs' worker stages. One thread_local
// per worker grows to the widest skeleton it has seen and then never allocates again, which is
// what "no allocations in the frame loop in steady state" means for a system whose instances are
// not all the same size.
struct Scratch {
  Vector<Vec3> translation;
  Vector<Quat> rotation;
  Vector<Vec3> scale;
  Vector<Mat4> model;

  anim::PoseView pose(u32 joints) {
    if (rotation.size() < joints) {
      translation.resize(joints);
      rotation.resize(joints);
      scale.resize(joints);
    }
    return anim::PoseView{std::span<Vec3>(translation.data(), joints),
                          std::span<Quat>(rotation.data(), joints),
                          std::span<Vec3>(scale.data(), joints)};
  }
  std::span<Mat4> matrices(u32 joints) {
    if (model.size() < joints) model.resize(joints);
    return {model.data(), joints};
  }
};

Scratch& scratch() {
  thread_local Scratch instance;
  return instance;
}

// Does this instance update on this tick? A divisor of 0 is LOD3 and never does.
bool updates_now(const AnimationLod& lod, u64 tick) noexcept {
  if (lod.divisor == 0) return false;
  if (lod.divisor == 1) return true;
  return (tick + lod.phase) % lod.divisor == 0;
}

}  // namespace

// ---- the LOD policy ----------------------------------------------------------------------------

LodPlan lod_plan(u8 tier) noexcept {
  switch (tier) {
    case 0: return LodPlan{1, true, true, true};
    case 1: return LodPlan{2, false, true, true};
    case 2: return LodPlan{4, false, false, true};
    default: return LodPlan{0, false, false, false};
  }
}

sim::TierParams tier_params() noexcept {
  sim::TierParams params;
  params.tier_count = k_tier_count;
  params.boundaries[0] = static_cast<f32>(lod_near.get());
  params.boundaries[1] = static_cast<f32>(lod_mid.get());
  params.boundaries[2] = static_cast<f32>(lod_far.get());
  params.hysteresis = static_cast<f32>(lod_hysteresis.get());
  params.max_promotions = static_cast<u32>(lod_max_promotions.get());
  params.max_demotions = static_cast<u32>(lod_max_demotions.get());
  return params;
}

u8 lod_tier(f32 observer_score, u8 current, const sim::TierParams& params) noexcept {
  // Exactly `sim::TierAssignment`'s rule for one entity: the tight boundary promotes, the widened
  // one demotes, and a score between the two leaves the entity where it is. Written here rather
  // than called through `assign_tiers` because that one takes SoA arrays of a population; the
  // test pins the two against each other on the same params so they cannot drift.
  const u8 tight = sim::TierAssignment::tier_of(observer_score, params);
  if (tight < current) return tight;
  const f32 widen = 1.0f + (params.hysteresis > 0.0f ? params.hysteresis : 0.0f);
  const u8 wide = sim::TierAssignment::tier_of(observer_score / widen, params);
  return wide > current ? wide : current;
}

// ---- the capability ------------------------------------------------------------------------

AnimationSystem::AnimationSystem(Library& clips, const AnimationConfig& config) : library_(&clips) {
  if (config.reserve_joints != 0) poses_.reserve(config.reserve_joints);
}

void AnimationSystem::install(ecs::SimWorld& sim) {
  flecs::world& world = sim.world();
  world_ = &world;

  // Seam 1: the components come from the schema IDL, through the header schemac generated. This
  // is the only place this capability's types become known to the world, and it is one call.
  register_animation_components(world);

  const sim::ComponentMask lod = ecs::mask_of<AnimationLod>(world);
  const sim::ComponentMask player_and_instance =
      ecs::mask_of<AnimationPlayer, SkeletonInstance>(world);
  const sim::ComponentMask all =
      ecs::mask_of<AnimationPlayer, SkeletonInstance, AnimationLod>(world);
  const sim::ComponentMask instance_and_lod = ecs::mask_of<SkeletonInstance, AnimationLod>(world);

  // The capability's own storage, named so the schedule can order two systems by it. Registered
  // here rather than at namespace scope because the registry is the process's and a static
  // initializer ordering question is not worth having for two ids read once per world.
  const sim::ResourceMask pose_pool = sim::resource_mask(k_resource_pose_pool);
  const sim::ResourceMask joint_matrices = sim::resource_mask(k_resource_joint_matrices);

  // Seam 2: each system declares itself to the engine and its body to flecs, in one call.
  //
  // **What orders the three.** `advance_players` writes `AnimationPlayer` and `SkeletonInstance`
  // and `sample_poses` reads both, so that ordering is a component conflict and always was.
  // `build_skinning_matrices` must run after `sample_poses`, and what one writes and the other
  // reads is the *pose pool* — not a component, and for good reasons (see pose_pool.h). That is
  // now declarable: `sample_poses` writes the `animation.pose_pool` resource and
  // `build_skinning_matrices` reads it, so the schedule puts them in successive waves for exactly
  // the reason the code requires it, and all three systems sit in the phase they belong in.

  descs_[0] = sim::SystemDesc{};
  descs_[0].name = "animation.advance_players";
  descs_[0].phase = k_phase_advance;
  descs_[0].reads = lod;
  descs_[0].writes = player_and_instance;
  descs_[0].tiers = 0b0111u;  // LOD0..LOD2; a frozen instance is not advanced at all
  descs_[0].determinism = sim::Determinism::Hashed;
  descs_[0].context = this;
  ecs::register_system(sim, descs_[0], [this](flecs::world& w, flecs::entity phase) {
    return w
        .system<AnimationPlayer, SkeletonInstance, const AnimationLod>("animation.advance_players")
        .kind(phase)
        .multi_threaded()
        .run([this](flecs::iter& it) { advance_players(it); });
  });

  descs_[1] = sim::SystemDesc{};
  descs_[1].name = "animation.sample_poses";
  descs_[1].phase = k_phase_sample;
  descs_[1].reads = all;
  descs_[1].writes_resources = pose_pool;
  descs_[1].tiers = 0b0111u;
  descs_[1].determinism = sim::Determinism::Hashed;
  descs_[1].context = this;
  ecs::register_system(sim, descs_[1], [this](flecs::world& w, flecs::entity phase) {
    return w
        .system<const AnimationPlayer, const SkeletonInstance, const AnimationLod>(
            "animation.sample_poses")
        .kind(phase)
        .multi_threaded()
        .run([this](flecs::iter& it) { sample_poses(it); });
  });

  descs_[2] = sim::SystemDesc{};
  descs_[2].name = "animation.build_skinning_matrices";
  descs_[2].phase = k_phase_skin;
  descs_[2].reads = instance_and_lod;
  descs_[2].reads_resources = pose_pool;
  descs_[2].writes_resources = joint_matrices;
  descs_[2].tiers = 0b0011u;  // only the tiers whose plan says `skin`
  // ADR-0010's other stance: the matrices are output nothing reads back into gameplay, and they
  // are rebuilt from the playhead after a load, so they are not in the sim hash.
  descs_[2].determinism = sim::Determinism::Derived;
  descs_[2].context = this;
  ecs::register_system(sim, descs_[2], [this](flecs::world& w, flecs::entity phase) {
    return w.system<const SkeletonInstance, const AnimationLod>("animation.build_skinning_matrices")
        .kind(phase)
        .multi_threaded()
        .run([this](flecs::iter& it) { build_skinning_matrices(it); });
  });

  ENGINE_LOG_INFO(log_animation, "capability installed",
                  log::field("skeletons", library_->skeleton_count()),
                  log::field("clips", library_->clip_count()));
}

// ---- the tick ----------------------------------------------------------------------------------

void AnimationSystem::advance_players(flecs::iter& it) {
  // The tick and the step are read once per invocation, not once per entity. Bound to a named
  // world first: GCC 13+ reads a reference obtained through a temporary `flecs::world` wrapper as
  // dangling, although the component lives in the world and not in the wrapper.
  const flecs::world world_ref = it.world();
  const u64 tick = world_ref.get<SimTick>().value;
  const f32 step = it.delta_time();
  const Library& clips = *library_;
  u32 advanced = 0;

  while (it.next()) {
    auto players = it.field<AnimationPlayer>(0);
    auto instances = it.field<SkeletonInstance>(1);
    auto lods = it.field<const AnimationLod>(2);
    for (auto row : it) {
      const AnimationLod& lod = lods[row];
      if (!updates_now(lod, tick)) continue;
      AnimationPlayer& player = players[row];
      SkeletonInstance& instance = instances[row];

      // The clip handles are `Id128` because that is what a save holds; the *lookup* is a hash and
      // has no business in a per-instance loop, so the resolved index is cached on the instance
      // and checked by a 16-byte compare against the library's own record. A clip set from JSON
      // through `WorldCommands`, or by a game writing the component directly, arrives with no
      // cached index and is resolved here — once, on the tick it changed.
      instance.clip_index = clips.resolve_clip(player.clip, instance.clip_index);
      instance.fade_index = clips.resolve_clip(player.fade_clip, instance.fade_index);

      // A divisor of n means this instance is visited every n ticks, so it owes n ticks of clip
      // time when it is. The average rate is therefore the same at every tier and a promoted
      // instance never has to catch up for having been at LOD1.
      const f32 delta = step * static_cast<f32>(lod.divisor) * player.speed;

      if (instance.clip_index != Library::k_not_found) {
        player.time = advance_time(player.time, delta, player.looping,
                                   clips.clip_data(instance.clip_index).duration);
      }
      if (!player.fade_clip.is_null()) {
        if (instance.fade_index != Library::k_not_found) {
          player.fade_time = advance_time(player.fade_time, delta, player.fade_looping,
                                          clips.clip_data(instance.fade_index).duration);
        }
        player.fade_elapsed += step * static_cast<f32>(lod.divisor);
        if (player.fade_elapsed >= player.fade_seconds) {
          // The fade is over: the incoming clip *is* the clip. Collapsing here rather than in the
          // sampler is what keeps the sampler free of state changes and therefore parallel.
          player.clip = player.fade_clip;
          player.time = player.fade_time;
          player.looping = player.fade_looping;
          player.fade_clip = Id128{};
          player.fade_time = 0.0f;
          player.fade_elapsed = 0.0f;
          player.fade_seconds = 0.0f;
          instance.clip_index = instance.fade_index;
          instance.fade_index = Library::k_not_found;
        }
      }
      ++advanced;
    }
  }
  if (world_ref.get_stage_id() == 0) stats_.players = advanced;
}

void AnimationSystem::sample_poses(flecs::iter& it) {
  const flecs::world world_ref = it.world();
  const u64 tick = world_ref.get<SimTick>().value;
  const Library& clips = *library_;
  u32 sampled = 0;
  u32 skipped = 0;

  while (it.next()) {
    auto players = it.field<const AnimationPlayer>(0);
    auto instances = it.field<const SkeletonInstance>(1);
    auto lods = it.field<const AnimationLod>(2);
    for (auto row : it) {
      const AnimationLod& lod = lods[row];
      const SkeletonInstance& instance = instances[row];
      if (instance.pose_slot == k_no_slot) continue;
      if (!updates_now(lod, tick)) {
        ++skipped;
        continue;
      }
      const AnimationPlayer& player = players[row];
      const anim::Skeleton& skeleton = clips.skeleton_data(instance.skeleton_index);
      const anim::PoseView out = poses_.pose(instance.pose_slot);
      // A clip writes only the joints its tracks name, so the slot starts at the bind pose and a
      // joint nothing animates holds the bind value rather than the previous instance's.
      anim::rest_pose(skeleton, out);
      if (instance.clip_index != Library::k_not_found)
        clips.clip_data(instance.clip_index).sample(player.time, out, player.looping);

      const bool fading = instance.fade_index != Library::k_not_found && player.fade_seconds > 0.0f;
      if (fading) {
        const f32 t = clamp01(player.fade_elapsed / player.fade_seconds);
        if (lod.interpolate) {
          const anim::PoseView incoming = scratch().pose(instance.joints);
          anim::rest_pose(skeleton, incoming);
          clips.clip_data(instance.fade_index)
              .sample(player.fade_time, incoming, player.fade_looping);
          anim::blend(out, incoming, t, out);
        } else if (t >= 0.5f) {
          // Interpolation off: the fade is a cut at its midpoint. At LOD1 the character is a few
          // pixels across and the blend is the first thing that stops being visible, while
          // sampling a second clip is the larger of the two costs.
          clips.clip_data(instance.fade_index).sample(player.fade_time, out, player.fade_looping);
        }
      }

      if (player.weight < 0.999f) {
        // A weight below 1 fades the player toward the skeleton's own bind pose. The guard is a
        // per-instance branch and not a per-joint one, and the common case skips a whole blend.
        const anim::PoseView rest = scratch().pose(instance.joints);
        anim::rest_pose(skeleton, rest);
        anim::blend(rest, out, player.weight < 0.0f ? 0.0f : player.weight, out);
      }
      ++sampled;
    }
  }
  if (world_ref.get_stage_id() == 0) {
    stats_.sampled = sampled;
    stats_.skipped_tier = skipped;
  }
}

void AnimationSystem::build_skinning_matrices(flecs::iter& it) {
  const flecs::world world_ref = it.world();
  const u64 tick = world_ref.get<SimTick>().value;
  const Library& clips = *library_;
  u32 skinned = 0;

  while (it.next()) {
    auto instances = it.field<const SkeletonInstance>(0);
    auto lods = it.field<const AnimationLod>(1);
    for (auto row : it) {
      const AnimationLod& lod = lods[row];
      if (!lod.skin) continue;
      const SkeletonInstance& instance = instances[row];
      if (instance.pose_slot == k_no_slot) continue;
      if (!updates_now(lod, tick)) continue;

      const anim::Skeleton& skeleton = clips.skeleton_data(instance.skeleton_index);
      const std::span<Mat4> model = scratch().matrices(instance.joints);
      anim::local_to_model(skeleton, poses_.pose(instance.pose_slot), model);
      anim::skinning_matrices(
          std::span<const Mat4>(model.data(), model.size()),
          std::span<const Mat4>(skeleton.inverse_bind.data(), skeleton.inverse_bind.size()),
          poses_.matrices(instance.pose_slot));
      ++skinned;
    }
  }
  if (world_ref.get_stage_id() == 0) stats_.skinned = skinned;
}

// ---- instances ----------------------------------------------------------------------------------

bool AnimationSystem::attach(flecs::entity entity, const Id128& skeleton, const Id128& clip) {
  const u32 skeleton_index = library_->skeleton_index(skeleton);
  if (skeleton_index == Library::k_not_found) {
    ENGINE_LOG_WARN(log_animation, "attach: the library does not hold this skeleton",
                    log::field("hi", skeleton.hi), log::field("lo", skeleton.lo));
    return false;
  }
  const anim::Skeleton& data = library_->skeleton_data(skeleton_index);

  SkeletonInstance instance;
  instance.skeleton = skeleton;
  instance.skeleton_index = skeleton_index;
  instance.joints = data.joint_count();
  instance.pose_slot = poses_.acquire(skeleton_index, instance.joints);
  instance.clip_index = library_->clip_index(clip);
  instance.fade_index = Library::k_not_found;
  // The rest pose, so a slot is never a frame of whatever the previous occupant left. A skeleton
  // with no joints gets no slot, and is not counted as a promotion.
  if (instance.pose_slot != k_no_slot) {
    anim::rest_pose(data, poses_.pose(instance.pose_slot));
    ++stats_.promotions;
  }

  AnimationPlayer player;
  if (const AnimationPlayer* existing = entity.try_get<AnimationPlayer>()) player = *existing;
  player.clip = clip;

  AnimationLod lod;
  const LodPlan plan = lod_plan(0);
  lod.tier = 0;
  lod.divisor = plan.divisor;
  lod.interpolate = plan.interpolate;
  lod.skin = plan.skin;
  lod.phase = instance.pose_slot == k_no_slot
                  ? u8{0}
                  : static_cast<u8>(instance.pose_slot % (plan.divisor != 0 ? plan.divisor : 1u));

  entity.set<AnimationPlayer>(player);
  entity.set<SkeletonInstance>(instance);
  entity.set<AnimationLod>(lod);
  return true;
}

void AnimationSystem::detach(flecs::entity entity) {
  if (SkeletonInstance* instance = entity.try_get_mut<SkeletonInstance>()) {
    if (instance->pose_slot != k_no_slot) {
      poses_.release(instance->pose_slot);
      ++stats_.demotions;
    }
  }
  entity.remove<SkeletonInstance>();
  entity.remove<AnimationLod>();
}

bool AnimationSystem::play(flecs::entity entity, const Id128& clip, f32 seconds, bool looping) {
  AnimationPlayer* player = entity.try_get_mut<AnimationPlayer>();
  if (player == nullptr) return false;
  if (library_->clip_index(clip) == Library::k_not_found) return false;

  if (!player->fade_clip.is_null()) {
    // A fade started while one is in flight collapses the one in flight to its target first, so a
    // state machine that changes its mind twice in three ticks cannot accumulate layers. What is
    // lost is the half-finished first fade, which is what the caller asked for by starting a
    // second one.
    player->clip = player->fade_clip;
    player->time = player->fade_time;
    player->looping = player->fade_looping;
  }
  if (seconds <= 0.0f || player->clip.is_null()) {
    player->clip = clip;
    player->time = 0.0f;
    player->looping = looping;
    player->fade_clip = Id128{};
    player->fade_seconds = 0.0f;
    player->fade_elapsed = 0.0f;
    player->fade_time = 0.0f;
  } else {
    player->fade_clip = clip;
    player->fade_time = 0.0f;
    player->fade_looping = looping;
    player->fade_elapsed = 0.0f;
    player->fade_seconds = seconds;
  }
  if (SkeletonInstance* instance = entity.try_get_mut<SkeletonInstance>()) {
    instance->clip_index = library_->clip_index(player->clip);
    instance->fade_index = library_->clip_index(player->fade_clip);
  }
  return true;
}

// ---- LOD ------------------------------------------------------------------------------------

void AnimationSystem::apply_tier(flecs::entity entity, u8 tier) {
  SkeletonInstance* instance = entity.try_get_mut<SkeletonInstance>();
  AnimationLod* lod = entity.try_get_mut<AnimationLod>();
  if (instance == nullptr || lod == nullptr) return;
  const u8 clamped = tier < k_tier_count ? tier : static_cast<u8>(k_tier_count - 1);
  if (lod->tier == clamped && instance->pose_slot != k_no_slot) return;

  const LodPlan plan = lod_plan(clamped);
  const bool was_animated = instance->pose_slot != k_no_slot;
  const GameTime now = world_ != nullptr ? world_->get<GameTime>() : GameTime{};

  if (was_animated && !plan.animated) {
    // The playhead stops here and the slot goes back. `frozen_at_us` is what makes the promotion
    // below a jump rather than a rewind.
    lod->frozen_at_us = now.us;
    poses_.release(instance->pose_slot);
    instance->pose_slot = k_no_slot;
    ++stats_.demotions;
  } else if (!was_animated && plan.animated) {
    if (AnimationPlayer* player = entity.try_get_mut<AnimationPlayer>()) {
      // **Analytically, not by ticking.** A clip's playhead is affine in elapsed time and its wrap
      // is a modulo, so "where would it be after t seconds" is closed form — the same
      // `advance_time` the tick calls, with the whole gap as its delta. That is `domain/sim`'s
      // summarizer contract (equivalence, determinism from stated inputs, idempotence over a
      // partition) met exactly rather than approximately, which is why LOD3 can be *not animated*
      // instead of animated cheaply: nothing pops in time on the way back.
      const f32 gap = static_cast<f32>(GameTime{now.us - lod->frozen_at_us}.seconds());
      const u32 clip_index = library_->clip_index(player->clip);
      if (clip_index != Library::k_not_found) {
        player->time = advance_time(player->time, gap * player->speed, player->looping,
                                    library_->clip_data(clip_index).duration);
      }
      if (!player->fade_clip.is_null()) {
        const u32 fade_index = library_->clip_index(player->fade_clip);
        if (fade_index != Library::k_not_found) {
          player->fade_time =
              advance_time(player->fade_time, gap * player->speed, player->fade_looping,
                           library_->clip_data(fade_index).duration);
        }
        player->fade_elapsed += gap;
        if (player->fade_elapsed >= player->fade_seconds) {
          player->clip = player->fade_clip;
          player->time = player->fade_time;
          player->looping = player->fade_looping;
          player->fade_clip = Id128{};
          player->fade_time = 0.0f;
          player->fade_elapsed = 0.0f;
          player->fade_seconds = 0.0f;
        }
      }
    }
    instance->pose_slot = poses_.acquire(instance->skeleton_index, instance->joints);
    if (instance->pose_slot != k_no_slot) {
      anim::rest_pose(library_->skeleton_data(instance->skeleton_index),
                      poses_.pose(instance->pose_slot));
    }
    ++stats_.promotions;
  }

  lod->tier = clamped;
  lod->divisor = plan.divisor;
  lod->interpolate = plan.interpolate;
  lod->skin = plan.skin;
  lod->phase = instance->pose_slot == k_no_slot
                   ? u8{0}
                   : static_cast<u8>(instance->pose_slot % (plan.divisor != 0 ? plan.divisor : 1u));
}

// ---- the Id128-addressed surface (ADR-0028 seam 3) -------------------------------------------
//
// One resolution each, through the world's identity map, and then the flecs form above. They
// exist so that a host which may not see flecs — an app, ADR-0028 seam 5 — can still own an
// animated world; see the header for the argument.

bool AnimationSystem::attach(const Id128& entity, const Id128& skeleton, const Id128& clip) {
  if (world_ == nullptr) return false;
  const flecs::entity found = ecs::entity_for(*world_, entity);
  return found != 0 && attach(found, skeleton, clip);
}

bool AnimationSystem::play(const Id128& entity, const Id128& clip, f32 seconds, bool looping) {
  if (world_ == nullptr) return false;
  const flecs::entity found = ecs::entity_for(*world_, entity);
  return found != 0 && play(found, clip, seconds, looping);
}

bool AnimationSystem::set_playhead(const Id128& entity, f32 time, f32 speed) {
  if (world_ == nullptr) return false;
  const flecs::entity found = ecs::entity_for(*world_, entity);
  if (found == 0) return false;
  AnimationPlayer* player = found.try_get_mut<AnimationPlayer>();
  if (player == nullptr) return false;
  player->time = time;
  player->speed = speed;
  return true;
}

bool AnimationSystem::joint_run(const Id128& entity, u32& first, u32& count) const {
  first = 0;
  count = 0;
  if (world_ == nullptr) return false;
  const flecs::entity found = ecs::entity_for(*world_, entity);
  if (found == 0) return false;
  const SkeletonInstance* instance = found.try_get<SkeletonInstance>();
  if (instance == nullptr || instance->pose_slot == k_no_slot) return false;
  first = poses_.first_joint(instance->pose_slot);
  count = poses_.joint_count(instance->pose_slot);
  return true;
}

void AnimationSystem::set_tier(flecs::entity entity, u8 tier) { apply_tier(entity, tier); }

void AnimationSystem::set_tier(const Id128& entity, u8 tier) {
  if (world_ == nullptr) return;
  const flecs::entity found = ecs::entity_for(*world_, entity);
  if (found != 0) apply_tier(found, tier);
}

sim::MaterializationHooks AnimationSystem::hooks() noexcept {
  sim::MaterializationHooks row;
  row.name = "animation";
  row.context = this;
  row.tiers = 0x0Fu;  // every tier: the transition into and out of LOD3 is the interesting one
  // The record came off disk, so it names its entity with the `Id128` that survived the trip, and
  // the identity map is what turns that into something to act on. This capability does not *bring*
  // an entity into the world — it attaches a pose to one that is already there — so what it
  // returns is the handle for the entity it found, and a null handle when there is none.
  row.materialize = [](void* context, const sim::EntityRecord& record, u8 tier) {
    AnimationSystem* self = static_cast<AnimationSystem*>(context);
    if (self->world_ == nullptr) return sim::EntityHandle{};
    const flecs::entity entity = ecs::entity_for(*self->world_, record.entity);
    if (!entity.is_valid()) return sim::EntityHandle{};
    self->apply_tier(entity, tier);
    return ecs::handle_of(entity);
  };
  // The other three act on something already live this tick, so they take the runtime handle and
  // keep nothing (ADR-0028 seam 3); `ecs::entity_of` is the one place that knows what is in one.
  row.promote = [](void* context, sim::EntityHandle entity, u8, u8 to) {
    AnimationSystem* self = static_cast<AnimationSystem*>(context);
    if (self->world_ == nullptr) return;
    const flecs::entity found = ecs::entity_of(*self->world_, entity);
    if (found.is_valid()) self->apply_tier(found, to);
  };
  row.demote = [](void* context, sim::EntityHandle entity, u8, u8 to) {
    AnimationSystem* self = static_cast<AnimationSystem*>(context);
    if (self->world_ == nullptr) return;
    const flecs::entity found = ecs::entity_of(*self->world_, entity);
    if (found.is_valid()) self->apply_tier(found, to);
  };
  row.dematerialize = [](void* context, sim::EntityHandle entity) {
    AnimationSystem* self = static_cast<AnimationSystem*>(context);
    if (self->world_ == nullptr) return;
    const flecs::entity found = ecs::entity_of(*self->world_, entity);
    if (found.is_valid()) self->detach(found);
  };
  return row;
}

Mat4 AnimationSystem::joint_model_transform(const SkeletonInstance& instance, u32 joint) const {
  if (instance.pose_slot == k_no_slot || joint >= instance.joints) return Mat4::identity();
  const anim::Skeleton& skeleton = library_->skeleton_data(instance.skeleton_index);
  const std::span<Mat4> model = scratch().matrices(instance.joints);
  anim::local_to_model(skeleton, poses_.pose(instance.pose_slot), model);
  return model[joint];
}

}  // namespace engine::animation
