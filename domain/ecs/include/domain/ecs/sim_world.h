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
// What this module *does* add is the part that is the engine's and not flecs' — the five seams
// of ADR-0028, of which this file holds the tick:
//
//   1. The tick phases of plan 05 §5.2 as flecs phase entities in order, driven at a fixed step
//      by core/time, with the current tick and game time published as singletons. The phase enum
//      is `sim::TickPhase` itself (seam 2).
//   2. A job-system adapter so flecs' workers run on the engine's performance-core pool rather
//      than on threads of their own (domain/ecs/os_api.h).
//   3. flecs' log routed into core/log (domain/ecs/os_api.h).
//   4. A debug watchdog on the table count, because relationships fragment archetypes as a cross
//      product and E6 measured what that costs (`TableWatchConfig` below).
//
// The other four seams live beside this file: components.h (the schema IDL is the one type
// system), systems.h (a system declares itself to the engine and its body to flecs), identity.h
// (persistent identity is `Id128`), world_commands.h (mutation from outside a system).
//
// Determinism (ADR-0010): `hashed`. The step is fixed, the tick and game time are integers,
// nothing here reads a wall clock, and flecs orders systems within a phase deterministically —
// by the order they were created, whatever the worker count — so the same world and the same
// inputs give the same tick sequence on the same binary.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/time/time.h>
#include <domain/sim/scheduler.h>

#include <flecs.h>

namespace engine::ecs {

// **One phase enum, and it is the scheduler's** (ADR-0028 seam 2). `sim::TickPhase` is the list
// of plan 05 §5.2 — Input, EventsIn, Lod, Systems, Physics, PostPhysics, EventsOut, Persist —
// and this is an alias of it, not a copy: a capability that declares `SystemDesc::phase` and the
// flecs phase entity its body attaches to are now the same value and not two that have to be
// kept in agreement. The dependency goes this way round on purpose: `domain/sim` must stay free
// of flecs so the executor can change without touching the declaration.
using TickPhase = sim::TickPhase;
using sim::k_phase_count;

// The flecs entity name of a phase ("sim_lod"). Distinct from `sim::phase_name`, which is the
// phase's own short name ("lod"): this one is what the explorer, the pipeline dump and any log
// of the tick show, and it is prefixed so a phase entity can never collide with a system named
// after the phase it runs in.
const char* phase_entity_name(TickPhase phase) noexcept;

// The phase entities of one world, indexed by TickPhase. A system joins the tick with
// `.kind(sim.phases()[TickPhase::Lod])` and the pipeline needs no edit to know about it, which
// is ADR-0027's registration point for a ticking capability.
struct Phases {
  flecs::entity by_phase[static_cast<usize>(TickPhase::Count)];

  flecs::entity operator[](TickPhase phase) const noexcept {
    return by_phase[static_cast<usize>(phase)];
  }
};

// The archetype-fragmentation watchdog (E6's first trap, ADR-0028's guard rails).
//
// A relationship multiplies archetypes as a *cross product*. E6 gave 10,000 children an
// independent faction as well as a parent and the same data went from 1,330 tables to 9,096, the
// tick from 2.79 ms to 4.71 ms (1.7×) and memory from 162 to 773 bytes an entity. Nothing about
// the world's contents changed and nothing failed; it just got slower, which is the kind of
// regression that reaches a release. So a world says how many archetypes its component design
// expects, and a debug build says so in the log when it is badly wrong.
//
// It is a warning and not an assert because the number is a design estimate, not an invariant: a
// world legitimately grows tables while it loads, and a game that wants 20,000 of them is allowed
// to want that. What it is not allowed to do is get there without noticing.
struct TableWatchConfig {
  u32 expected_archetypes = 0;  // 0 switches the watchdog off, which is the default
  f32 warn_multiple = 4.0f;     // warn once the table count passes this multiple of the estimate
  u32 check_every_ticks = 64;   // the count is a world-info read, but not one worth doing a tick
};

struct SimWorldConfig {
  u32 hz = 60;                    // fixed step rate; 60 by default, a game may choose 30
  u32 max_steps_per_advance = 8;  // the cap that stops the spiral of death after a stall
  f64 game_seconds_per_real_second = 1.0;
  GameTime epoch;  // the game time tick 0 sits at
  TableWatchConfig table_watch;
};

// The watchdog on its own, so a test or a tool can run it against a world it did not step.
// Returns true when this call warned. Compiled in every configuration; `SimWorld` only calls it
// in debug builds.
class TableWatch {
 public:
  TableWatch() = default;
  explicit TableWatch(const TableWatchConfig& config) noexcept : config_(config) {}

  bool check(const flecs::world& world) noexcept;

  u32 tables() const noexcept { return tables_; }
  u32 warnings() const noexcept { return warnings_; }
  const TableWatchConfig& config() const noexcept { return config_; }

 private:
  TableWatchConfig config_;
  u32 tables_ = 0;
  u32 warnings_ = 0;
  u32 warned_at_ = 0;  // the table count the last warning was about; re-warn only past it
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

  // The archetype watchdog. Read it to find out what the world's table count actually is; in a
  // debug build `step()` has been checking it against `SimWorldConfig::table_watch`.
  const TableWatch& table_watch() const noexcept { return table_watch_; }

 private:
  void build_phases();
  void publish_singletons();
  void run_tick();

  flecs::world world_;
  Phases phases_;
  FixedStepClock clock_;
  GameClock game_clock_;
  SimTick tick_;
  TableWatch table_watch_;
  f32 step_seconds_ = 1.0f / 60.0f;
};

}  // namespace engine::ecs
