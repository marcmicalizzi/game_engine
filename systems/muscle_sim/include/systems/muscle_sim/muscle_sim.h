#pragma once

// muscle_sim capability (ADR-0027, ADR-0055; docs/subsystems/muscle_sim.md,
// docs/plan/05-simulation.md §5.16).
//
// The line tier's engine wiring. `systems/muscle` is the kernel and its supervisor, a batch API
// over its own store with no ECS; this capability is everything that needs the entity store: it
// registers the line actuator's components, takes `engine.muscle.LineActuator` records through
// materialization (refusing the ones the validator refuses), routes each actuator's path through
// its character's posed skeleton every tick, and ticks every actuator of the world through the
// supervisor once, from one scheduler entry, writing each one's force, tier and stale flag into its
// `LineOutput` and the tick's events into a buffer the rest of the engine reads.
//
// What it does not do: apply the force to anything (no consumer exists yet: physics reads it when a
// ragdoll does), drive the excitation (a property, `LineExcitation`, until a controller writes it),
// or save the committed line state through the document (it is runtime integration state, saved
// with `save`/`restore` below, not an authored property).
//
// ---- ADR-0027 decision 2, registration point by registration point ----------------------------
//
//   [x] capability graph  engine_capability_requires(muscle_sim muscle / ecs / animation)
//   [x] schema types      systems/muscle/schemas/muscle.schema (LineActuator, LineActuatorSpec,
//                         LineExcitation, LineOutput, the mapping): declared beside the parameter
//                         structs they carry, registered here
//   [x] scheduler entry   one sim::SystemDesc, `muscle_sim.tick`, registered with
//                         ecs::register_system, in TickPhase::Physics
//   [x] materialization   a hooks row: the validator's gate, the store slot, the dematerialization
//   [ ] render passes     none: a muscle draws nothing
//   [ ] derived data      none
//   [ ] protocol methods  none; records reach the world through materialization
//   [ ] tunables          none: the schedule is fixed by D20-D23
//   [x] LOD policy        k_lod_tiers: entities at LOD0 and LOD1 only (the mapping's @tiers)
//   [x] determinism       k_determinism = "hashed"
//   [x] zero cost unused  no linked code (ENGINE_WITH_MUSCLE_SIM=OFF) and no instances: a world
//                         with no LineOutput matches nothing and the tick returns at once
//   [x] docs, tests, size table

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <core/math/math.h>
#include <domain/ecs/sim_world.h>
#include <domain/sim/scheduler.h>
#include <systems/muscle/store.h>
#include <systems/muscle/validator.h>
#include <systems/muscle_sim/path.h>

#include <schemas/muscle.h>
#include <span>
#include <string_view>

namespace engine::animation {
class AnimationSystem;
}

namespace engine::muscle_sim {

/// Determinism stance (ADR-0010): "hashed". The committed line state is integration state the next
/// tick reads back, and `systems/muscle` makes it the same bytes on every toolchain; this system
/// adds a gather into per-slot arrays and one single-threaded pass in slot order, so the bytes do
/// not depend on the worker count or the entity store's table order.
inline constexpr const char* k_determinism = "hashed";

/// The phase: Physics, the one after the phase where kinematics and animation run. A path is
/// routed through the pose `animation.sample_poses` writes in `Systems`, and a phase boundary is
/// the ordering the scheduler guarantees (each phase is its own pipeline run under
/// `ScheduledTick`), where two capabilities' systems inside one phase would be ordered by their
/// registration. The force is a physics input, so it is computed in the phase physics runs in,
/// before `PostPhysics` reads it.
inline constexpr sim::TickPhase k_phase = sim::TickPhase::Physics;

/// LOD tiers the system runs at: LOD0 and LOD1, the tiers `LineActuator` materializes at. A farther
/// character's muscles are not entities.
inline constexpr u32 k_lod_tiers = 0b0011u;

/// Where the actuators' skeletons are posed. An actuator's character is its `ChildOf` parent; the
/// source names its joints and poses them. Two function pointers, called between and inside ticks
/// on the system's own thread: `find_joint` once per attachment when a route is resolved,
/// `model_transforms` once per character per tick.
struct PoseSource {
  void* context = nullptr;
  /// The joint of `character`'s skeleton called `bone`; false when the character has no skeleton
  /// yet or none of that name (the route is tried again next tick).
  bool (*find_joint)(void* context, flecs::entity character, std::string_view bone,
                     u32& joint) = nullptr;
  /// Every joint's model-space transform of `character`'s skeleton as posed this tick, into `out`
  /// (`joints` says how many); false when it has no pose this tick or more joints than `out`.
  bool (*model_transforms)(void* context, flecs::entity character, std::span<Mat4> out,
                           u32& joints) = nullptr;
  /// The named resources the source reads, for the system's descriptor.
  sim::ResourceMask reads_resources{};
};

/// The posed skeleton of `systems/animation`: a character's `SkeletonInstance`, its skeleton's
/// joint names from the clip library, and the pose pool's local transforms composed into model
/// space (`anim::local_to_model`) — what `AnimationSystem::joint_model_transform` composes, once
/// per character a tick instead of once per joint. Declares the read of `animation.pose_pool`.
PoseSource animation_pose_source(animation::AnimationSystem& animation) noexcept;

struct MuscleSimConfig {
  /// Actuators one world holds: every per-actuator array is sized to it once, at construction. A
  /// record past it is refused, with that said.
  u32 capacity = 1024;
  /// The largest skeleton a route is posed on: the model-transform scratch, sized once.
  u32 max_joints = 256;
};

/// What the last tick did, and what materialization refused.
struct MuscleSimStats {
  u32 actuators = 0;  // live
  u32 ticked = 0;     // supervised last tick
  u32 frozen = 0;     // of those, failed (tier 0 or 1)
  u32 rigid = 0;      // in tier 1 after the tick
  u32 inert = 0;      // in tier 2 after the tick
  u32 waiting = 0;    // with no route or no static start yet: not ticked
  u32 events = 0;     // the last tick's
  u64 refused = 0;    // records refused, cumulative
};

/// A `LineActuator` record that did not become an actuator: a document-level validation failure.
/// The entity the entity store's hook made for it keeps its authored components and never ticks.
struct RecordRefusal {
  Id128 record;
  /// The validator's refusal (code, field, value, unit, rule), or None for a refusal of this
  /// capability's own (`reason`).
  muscle::ValidationResult result;
  const char* reason = "";
};

class MuscleSimSystem {
 public:
  explicit MuscleSimSystem(const PoseSource& poses, const MuscleSimConfig& config = {});
  ENGINE_NON_COPYABLE(MuscleSimSystem);

  /// Registers the line actuator's components with the world (ADR-0028 seam 1) and the tick with
  /// the engine's descriptor (seam 2). Once per world, outside a tick.
  void install(ecs::SimWorld& sim);

  /// The materialization hooks row for `sim::SimScheduler::add_hooks`, registered **after** the
  /// entity store's (`ecs::RecordMaterializer`), which makes the entity and writes the authored
  /// components this row reads: the validator's gate, a store slot, and the `LineOutput` that makes
  /// the entity an actuator.
  sim::MaterializationHooks hooks() noexcept;

  /// The last tick's events, in slot order: every actuator's `tick_events`, copied out of the store
  /// after the tick into a buffer sized once (capacity x `muscle::k_event_slots`, so nothing is
  /// ever dropped). Nothing in the engine delivers simulation events yet; this is where a consumer
  /// reads them, between ticks or from a later phase.
  std::span<const muscle::ValidationEvent> events() const noexcept {
    return {events_.data(), event_count_};
  }
  /// Every record refused so far, in the order materialization met them.
  std::span<const RecordRefusal> refusals() const noexcept {
    return {refusals_.data(), refusals_.size()};
  }

  const muscle::LineStore& store() const noexcept { return store_; }
  const MuscleSimStats& stats() const noexcept { return stats_; }
  const sim::SystemDesc& descriptor() const noexcept { return desc_; }

  /// The store slot of the actuator record `record` made, or `k_no_slot`.
  u32 slot_of(const Id128& record) const noexcept;
  static constexpr u32 k_no_slot = 0xFFFF'FFFFu;

  /// The committed record of `record`'s actuator (`muscle::save`), and back (`muscle::restore`,
  /// which refuses another law or parameter set), with the path's history at the last boundary so
  /// that the next tick routes the same Hermite. False for a record with no actuator here.
  bool save(const Id128& record, muscle::LineState& out, PathHistory* history = nullptr) const;
  bool restore(const Id128& record, const muscle::LineState& in,
               const PathHistory* history = nullptr);

 private:
  sim::EntityHandle materialize(const sim::EntityRecord& record);
  void dematerialize(sim::EntityHandle handle);
  void refuse(const Id128& record, const muscle::ValidationResult& result, const char* reason);
  void release(u32 slot);
  void run(flecs::iter& it);
  bool resolve_route(u32 slot, flecs::entity character, const muscle::LineActuatorSpec& spec);

  PoseSource poses_;
  MuscleSimConfig config_;
  muscle::LineStore store_;
  flecs::world* world_ = nullptr;
  sim::SystemDesc desc_;
  MuscleSimStats stats_;
  HashMap<Id128, u32> slots_;  // by record id; read at materialization and by save/restore
  Vector<RecordRefusal> refusals_;
  // Per slot, sized once to the capacity.
  Vector<u8> live_;
  Vector<u8> resolved_;
  Vector<Id128> record_;
  Vector<flecs::entity_t> character_;
  Vector<RoutePoint> route_;  // k_max_route_points a slot
  Vector<u8> route_points_;
  Vector<PathHistory> history_;
  Vector<double> excitation_;
  Vector<double> length_;
  Vector<muscle::HermitePath> path_;
  Vector<muscle::LineOutput*> output_;
  Vector<u32> free_;
  Vector<u32> order_;  // this tick's live, initialised slots, ascending
  // Per tick.
  Vector<muscle::ValidationEvent> events_;
  u32 event_count_ = 0;
  Vector<Mat4> model_;
};

}  // namespace engine::muscle_sim
