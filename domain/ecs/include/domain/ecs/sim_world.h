#pragma once

// flecs, plus the three things the engine has to add to it (docs/plan/03-data-model.md §3.4,
// docs/plan/05-simulation.md §5.2, experiment E6).
//
// **There is no engine-wide ECS abstraction here, and there will not be one.** `world()` hands
// out the `flecs::world` and every consumer writes flecs: its components, its queries, its
// relationships. A wrapper would have to re-expose relationships, wildcards, change detection,
// staging, the query DSL, reflection and the explorer to be useful, at which point it is flecs
// with worse documentation; and the cost of the wrapper is paid on every query in the engine,
// forever, to buy an option (swapping the ECS) that the wrapper would not actually deliver,
// because a real swap is a rewrite of every system's data access whatever sits in between.
// docs/plan/03-data-model.md §3.4 already says what the insurance is instead: hot systems own
// their own data (physics in Jolt, poses in the animation pools, instances in GPU buffers) and
// the ECS holds identity, relationships and gameplay components. That is an architectural
// hedge, not an interface one.
//
// What this module *does* add is the part that is the engine's and not flecs':
//
//   1. The tick phases of plan 05 §5.2 as flecs phase entities in order, driven at a fixed step
//      by core/time, with the current tick and game time published as singletons.
//   2. A job-system adapter so flecs' workers run on the engine's performance-core pool rather
//      than on threads of their own (domain/ecs/os_api.h).
//   3. flecs' log routed into core/log (domain/ecs/os_api.h).
//
// Determinism (ADR-0010): `hashed`. The step is fixed, the tick and game time are integers,
// nothing here reads a wall clock, and flecs orders systems within a phase deterministically —
// by the order they were created, whatever the worker count — so the same world and the same
// inputs give the same tick sequence on the same binary.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/time/time.h>

#include <flecs.h>

namespace engine::ecs {

// The tick phases of plan 05 §5.2, in the order they run. The enumerators and their order are
// the same list as ADR-0027's `SystemDesc::phase`, so a capability's declared phase and the
// entity it attaches to are the same thing spelled twice.
enum class TickPhase : u8 {
  Input = 0,        // raw input becomes actions for this tick
  EventsIn = 1,     // events queued since the last tick are delivered
  Lod = 2,          // observer set -> tier per entity, with hysteresis (plan 05 §5.4)
  Systems = 3,      // gameplay systems, parallel within the phase
  Physics = 4,      // the solver step (and its sub-steps)
  PostPhysics = 5,  // anything that reads the solver's result
  EventsOut = 6,    // events raised this tick are published
  Persist = 7,      // the persistence flush (foundation/store)
  Count = 8,
};
const char* phase_name(TickPhase phase) noexcept;

// The phase entities of one world, indexed by TickPhase. A system joins the tick with
// `.kind(sim.phases()[TickPhase::Lod])` and the pipeline needs no edit to know about it, which
// is ADR-0027's registration point for a ticking capability.
struct Phases {
  flecs::entity by_phase[static_cast<usize>(TickPhase::Count)];

  flecs::entity operator[](TickPhase phase) const noexcept {
    return by_phase[static_cast<usize>(phase)];
  }
};

struct SimWorldConfig {
  u32 hz = 60;                    // fixed step rate; 60 by default, a game may choose 30
  u32 max_steps_per_advance = 8;  // the cap that stops the spiral of death after a stall
  f64 game_seconds_per_real_second = 1.0;
  GameTime epoch;  // the game time tick 0 sits at
};

// One flecs world wired to the engine's tick.
//
// The singletons are core/time's own types rather than new ones, so a system reads the tick it
// is running as `it.world().get<SimTick>()` and the game time as `get<GameTime>()`, and nothing
// has to keep two representations of the same number in agreement.
class SimWorld {
 public:
  SimWorld();
  explicit SimWorld(const SimWorldConfig& config);
  ~SimWorld();
  ENGINE_NON_COPYABLE(SimWorld);

  flecs::world& world() noexcept { return world_; }
  const flecs::world& world() const noexcept { return world_; }
  const Phases& phases() const noexcept { return phases_; }
  flecs::entity phase(TickPhase which) const noexcept { return phases_[which]; }

  // Runs exactly one fixed step: publishes the new tick and game time, then runs the pipeline
  // with the fixed delta. Reads no clock at all, which is what makes a replay and a headless
  // fast-forward take the same path as a live session (plan 05 §5.10).
  void step();
  // Feeds elapsed real time to the accumulator and runs whole steps, at most
  // `max_steps_per_advance` of them. Returns how many ran.
  u32 advance(i64 real_ns);

  SimTick tick() const noexcept { return tick_; }
  GameTime game_time() const noexcept { return game_clock_.now(); }
  f32 step_seconds() const noexcept { return step_seconds_; }

  // The accumulator. `set_time_scale` changes how fast real time is consumed; it never changes
  // the step size. The clock's own tick counter is *not* the world's — `step()` advances the
  // world without touching the accumulator — so read `SimWorld::tick()`, never `clock().tick()`.
  FixedStepClock& clock() noexcept { return clock_; }
  GameClock& game_clock() noexcept { return game_clock_; }

 private:
  void build_phases();
  void publish_singletons();
  void run_tick();

  flecs::world world_;
  Phases phases_;
  FixedStepClock clock_;
  GameClock game_clock_;
  SimTick tick_;
  f32 step_seconds_ = 1.0f / 60.0f;
};

}  // namespace engine::ecs
