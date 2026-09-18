#pragma once

// animation capability (ADR-0027; docs/plan/02-architecture.md §2.8,
// docs/plan/05-simulation.md §5.11 and §5.15, docs/plan/11-performance-principles.md §11.10).
//
// The system layer that plays clips. `domain/anim` owns the data — skeletons, poses, clips, the
// blends over a pose, the 3x4 skinning matrices — and nothing ticks it; this capability is the
// tick. It owns a playhead per entity, a pose pool, and the bone-matrix buffer a skinned instance
// uploads, and it attaches to the engine through the registration points and through nothing else.
//
// What it does not do: draw. The renderer contract is a span and a pair of offsets (see "The
// renderer contract" below and docs/subsystems/animation.md); wiring it to `gfx::DeformDesc` is
// the renderer's change, not this module's. Also absent, and named rather than implied: blend
// trees and state machines as data, IK, and motion matching (docs/plan/05-simulation.md §5.11).
//
// ---- ADR-0027 decision 2, registration point by registration point ----------------------------
//
//   [x] capability graph    engine_capability_requires(animation ecs) — anything that ticks does
//   [x] schema types        systems/animation/schemas/animation.schema, engine_schema_library
//   [x] scheduler entry     three sim::SystemDesc, registered with ecs::register_system below
//   [ ] render passes       none: this capability produces a buffer, it does not draw
//   [ ] derived data        none: clips are imported at load, not precomputed by the content build
//   [ ] protocol methods    none yet; the components are already reachable through world.apply
//   [x] tunables            animation.* in src/animation.cpp, read once per tick, never per entity
//   [x] LOD policy          lod_plan()/lod_tier() over sim::TierParams, driven by
//                           sim::MaterializationHooks (AnimationSystem::hooks())
//   [x] determinism         k_determinism = "hashed"; see below
//   [x] zero cost unused    no linked code (ENGINE_WITH_ANIMATION=OFF) and no instances: a world
//                           with no AnimationPlayer matches no query and the pool stays empty
//   [x] docs, tests, size table, bench
//
// **Determinism: hashed.** Everything this capability does inside a tick is integer- and
// float-arithmetic over per-entity state, with no wall-clock read, no worker-count-dependent
// ordering and no shared accumulator: `advance_players` touches one entity's playhead,
// `sample_poses` and `build_skinning_matrices` write one instance's own run of the pool, and pool
// slots are handed out from a LIFO free list in the single-threaded LOD phase. So one worker and
// eight produce identical bytes, and so do two runs — which the tests assert rather than assume.
// The playhead is gameplay state (a foot plant, a hit frame) and enters a save; the matrices are
// `derived` output nothing reads back, and they are recomputed from the playhead on load.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/ids/id128.h>
#include <core/time/time.h>
#include <domain/ecs/sim_world.h>
#include <domain/ecs/systems.h>
#include <domain/sim/scheduler.h>
#include <domain/sim/tiers.h>
#include <systems/animation/library.h>
#include <systems/animation/pose_pool.h>

#include <flecs.h>
#include <schemas/animation.h>
#include <span>

namespace engine::animation {

// ADR-0010's stance, in the header as the ADR requires. See the paragraph above for why.
inline constexpr const char* k_determinism = "hashed";

// All three run in `Systems`, and the ordering between all three is declared rather than implied.
// `advance_players` before `sample_poses` is a component conflict; `sample_poses` before
// `build_skinning_matrices` is a **resource** conflict on the pose pool (`k_resource_pose_pool`),
// which `sim::SystemDesc::writes_resources` can now name. Until it could, the third system sat in
// `PostPhysics` to borrow an ordering from the phase — honest, but it moved a system for a reason
// that had nothing to do with which phase it belongs in, and a capability whose two pool-ordered
// systems genuinely belong in one phase had no answer at all. See docs/subsystems/animation.md.
inline constexpr sim::TickPhase k_phase_advance = sim::TickPhase::Systems;
inline constexpr sim::TickPhase k_phase_sample = sim::TickPhase::Systems;
inline constexpr sim::TickPhase k_phase_skin = sim::TickPhase::Systems;

// The two things this capability owns that are not components, as `sim` resource names. They are
// separate because they are written by different systems and read by different consumers: the pose
// channels are `sample_poses`' output and `build_skinning_matrices`' input, and the matrices are
// the renderer's (`joint_matrices()`), which is a reader outside the tick entirely.
inline constexpr const char* k_resource_pose_pool = "animation.pose_pool";
inline constexpr const char* k_resource_joint_matrices = "animation.joint_matrices";

// ---- the LOD policy --------------------------------------------------------------------------
//
// The tier itself comes from `sim::TierAssignment` — the minimum over observers of
// f(distance, importance, weight), with the hysteresis band and the rate limits the engine's
// reference implementation already gets right, and which a capability has no business
// reimplementing. What belongs to *this* capability is two things: the band boundaries (a
// character stops being worth animating much nearer than a generic entity stops being worth
// simulating) and what each tier actually costs, which is `LodPlan`.

inline constexpr u8 k_tier_count = 4;

// What a tier costs one instance. Stored on `AnimationLod` so the hot loops read numbers instead
// of switching on a tier per entity.
struct LodPlan {
  u8 divisor = 1;           // update every `divisor` ticks; 0 means never
  bool interpolate = true;  // blend the cross-fade rather than snapping to the dominant clip
  bool skin = true;         // build skinning matrices
  bool animated = true;     // hold a pose-pool slot at all
};

// LOD0 every tick, fully blended, skinned.
// LOD1 every second tick with cross-fade interpolation off — a character at that distance is a
//      few pixels across and the fade is the first thing that stops being visible, while the
//      halved sampling rate is the larger saving.
// LOD2 every fourth tick, sampled but not skinned: the pose still exists for gameplay (a foot
//      plant, a grab point, a footstep query) and nothing on screen deforms from it.
// LOD3 not animated at all. The slot is released, the playhead freezes, and the promotion back
//      advances it analytically so the character is where it would have been.
LodPlan lod_plan(u8 tier) noexcept;

// This capability's band boundaries, as `sim::TierParams` so they go straight into
// `sim::TierAssignment::assign_tiers`. Backed by tunables (`animation.lod.*`).
sim::TierParams tier_params() noexcept;

// The tier one instance belongs at, given its observer score and the tier it is at now. It is
// exactly `sim::TierAssignment`'s rule for a single entity — tight boundary to promote, widened
// boundary to demote — and the test pins it against `sim::TierAssignment::tier_of` so the two can
// never disagree about a band.
u8 lod_tier(f32 observer_score, u8 current, const sim::TierParams& params) noexcept;

// ---- counters ---------------------------------------------------------------------------------

// What the last tick did. Cheap by construction: each counter is written **once per system per
// tick, by stage 0 only**, so nothing here is a per-entity atomic and nothing in the hot loop
// contends. The price is that with more than one worker these are stage 0's share of the work and
// not the world's total — a diagnostic, not a metric. `promotions` and `demotions` are exact,
// because a tier transition never happens on a worker.
struct AnimationStats {
  u32 players = 0;       // players advanced
  u32 sampled = 0;       // poses written
  u32 skinned = 0;       // instances whose bone matrices were rebuilt
  u32 promotions = 0;    // slots acquired since install
  u32 demotions = 0;     // slots released since install
  u32 skipped_tier = 0;  // instances whose divisor said "not this tick"
};

struct AnimationConfig {
  // Joints to reserve the pool for. A world that knows its population allocates once; one that
  // does not still stops allocating as soon as the population stops growing, because a released
  // slot is reused rather than returned.
  u32 reserve_joints = 0;
};

// The capability.
//
// One per world. It is constructed with the library it plays from, `install()`ed into a
// `SimWorld` once outside a tick, and thereafter owns the pose pool, the bone-matrix buffer and
// the tier transitions.
class AnimationSystem {
 public:
  explicit AnimationSystem(Library& clips, const AnimationConfig& config = {});
  ENGINE_NON_COPYABLE(AnimationSystem);

  // --- registration ----------------------------------------------------------------------------

  // Registers this capability's components with the world (ADR-0028 seam 1, through the header
  // schemac generated) and its three systems with the engine's descriptor (seam 2). Call once per
  // world, outside a tick — `ecs::register_system` creates flecs systems and a world flecs has
  // made readonly for the pipeline cannot have one created in it.
  void install(ecs::SimWorld& sim);

  // The materialization contract of [03 §3.4] as a row for `sim::SimScheduler::add_hooks`: this
  // is how tier changes computed by `sim::TierAssignment` reach the capability.
  //
  // **The hooks' `u64 entity` is a flecs entity id**, and the capability uses it inside the call
  // and never stores it (ADR-0028 seam 3). `domain/sim` speaks `u64` and `domain/ecs` speaks
  // `Id128`, and the two registration points do not share a vocabulary; this is the choice this
  // capability made and it is stated rather than implied. `set_tier(Id128, u8)` is the form for a
  // caller that came from a save.
  sim::MaterializationHooks hooks() noexcept;

  // --- instances -------------------------------------------------------------------------------

  // Makes `entity` an animated instance: resolves the skeleton and the clip, gives it a pose-pool
  // slot at tier 0, and fills it with the rest pose so it is never a frame of garbage. False when
  // the library does not hold the skeleton. A null clip is legal and means "hold the rest pose".
  bool attach(flecs::entity entity, const Id128& skeleton, const Id128& clip);
  // Releases the slot and removes the transient components; the `AnimationPlayer` stays, because
  // it is the entity's state and not this capability's.
  void detach(flecs::entity entity);

  // Starts `clip` on `entity`, cross-fading from whatever is playing over `seconds`. Zero seconds
  // is a cut. A fade started while one is in flight collapses the one in flight first, so a state
  // machine that changes its mind twice in three ticks cannot accumulate layers.
  bool play(flecs::entity entity, const Id128& clip, f32 seconds = 0.0f, bool looping = true);

  // --- LOD -------------------------------------------------------------------------------------

  // Puts one instance at `tier`, acquiring or releasing its pool slot and advancing its playhead
  // analytically across the frozen interval. Idempotent: setting the tier it is already at does
  // nothing at all.
  void set_tier(flecs::entity entity, u8 tier);
  void set_tier(const Id128& entity, u8 tier);

  // --- the renderer contract -------------------------------------------------------------------
  //
  // `joint_matrices()` is every animated instance's bone matrices in one contiguous span, in slot
  // order, `anim::JointMatrix` — three `Vec4` rows, 48 bytes, the exact record `deform.slang`
  // mirrors. An instance's own run is `[first_joint(slot), first_joint(slot) + joint_count(slot))`.
  //
  // What a renderer does with it, and what this capability deliberately does not do:
  //
  //     gfx::DeformDesc desc;
  //     desc.flags       = gfx::k_deform_skin;
  //     desc.joint_count = animation.joint_count(instance.pose_slot);
  //     desc.joints      = upload_address + animation.first_joint(instance.pose_slot) * 48;
  //
  // The span is one upload per frame for the whole population rather than one per instance, which
  // is why the pool's slots are a shared arena; `DeformDesc::joints` is per instance because two
  // characters share one mesh, one `geometry::SkinBinding` stream and one skeleton and have
  // entirely different poses (domain/gfx/cluster_cull.h says the same thing from the other side).
  // A slot whose `skin` plan is false is not written this tick, and an instance at LOD3 has no
  // slot at all: such an instance's `InstanceDesc::deform` must be `gfx::k_invalid_deform`, which
  // is the renderer's call to make and the reason this module exposes the slot rather than
  // reaching across the boundary to make it.
  std::span<const anim::JointMatrix> joint_matrices() const noexcept {
    return poses_.upload_buffer();
  }
  u32 first_joint(u32 slot) const noexcept { return poses_.first_joint(slot); }
  u32 joint_count(u32 slot) const noexcept { return poses_.joint_count(slot); }

  // --- inspection ------------------------------------------------------------------------------

  const PosePool& poses() const noexcept { return poses_; }
  PosePool& poses() noexcept { return poses_; }
  const Library& library() const noexcept { return *library_; }
  const AnimationStats& stats() const noexcept { return stats_; }
  // The three descriptors, in registration order, for a test or a tool that wants to read what
  // this capability declared without going through the world's `SystemRegistry`.
  std::span<const sim::SystemDesc> descriptors() const noexcept { return {descs_, k_system_count}; }
  static constexpr u32 k_system_count = 3;

  // The model-space transform of one joint of one instance, for a caller that needs a foot or a
  // hand where the renderer would draw it (a footstep query, a grab point). Identity when the
  // instance has no slot.
  Mat4 joint_model_transform(const SkeletonInstance& instance, u32 joint) const;

 private:
  void advance_players(flecs::iter& it);
  void sample_poses(flecs::iter& it);
  void build_skinning_matrices(flecs::iter& it);
  void apply_tier(flecs::entity entity, u8 tier);

  Library* library_ = nullptr;
  flecs::world* world_ = nullptr;
  PosePool poses_;
  AnimationStats stats_;
  sim::SystemDesc descs_[k_system_count];
};

}  // namespace engine::animation
