#pragma once

// The engine's scheduler owns the tick, and flecs runs each phase's systems inside it
// ([ADR-0038](../../../../docs/adr/0038-the-scheduler-owns-the-tick.md), proposed; ADR-0028
// decision 7).
//
// `sim::SimScheduler` already owns the fixed step, the phase order, the timing wheel, tier
// assignment, the materialization hooks and a table of systems of its own; `ecs::SimWorld::step()`
// had flecs' pipeline own a second clock and the whole phase chain. A `ScheduledTick` makes the
// scheduler the one executor: it installs itself as the scheduler's `TickExecutor`, and on every
// tick the scheduler
//
//   1. advances its clock and tells this object the tick and game time, which it publishes to the
//      world (`SimWorld::sync_clock`) and opens a flecs frame (`ecs_frame_begin`);
//   2. for each phase, in plan order, runs that phase's flecs systems — a per-phase flecs pipeline,
//      `ecs_run_pipeline`, so flecs' own multi-threading, sync points and merges are unchanged —
//      then the table's own waves;
//   3. closes the frame (`ecs_frame_end`) and runs the debug table watchdog.
//
// What that buys, and what it does not, is ADR-0038's to argue: one clock, the timing wheel and the
// LOD tiers in the same loop as the systems they feed, and one executor for a replay to be
// deterministic against. What it does not do yet is move flecs' systems into the scheduler's waves:
// inside a phase flecs still splits a system's rows evenly across its workers, and hierarchy
// propagation still cannot be multi-threaded. `SystemRegistry` holds every system's declaration so
// that step is a change here and not in any capability.
//
// A phase with no flecs system is skipped without touching flecs. flecs' `OnStart` systems are not
// run: nothing in the engine declares one, and the engine's phases hang off `PostFrame`.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/time/time.h>
#include <domain/ecs/sim_world.h>
#include <domain/sim/scheduler.h>

#include <flecs.h>

namespace engine::ecs {

class ScheduledTick {
 public:
  // Installs itself as `scheduler`'s executor and marks `world` as scheduled, so the world's own
  // `step()` refuses. The two must agree on the step (the same `hz`), which is asserted: one clock
  // cannot have two step sizes. The world's clock is set to the scheduler's.
  ScheduledTick(SimWorld& world, sim::SimScheduler& scheduler);
  // Detaches: the scheduler gets no executor and the world may step itself again.
  ~ScheduledTick();
  ENGINE_NON_COPYABLE(ScheduledTick);

  // The scheduler's step and advance, spelled here so a host that holds both reads naturally.
  void step() { scheduler_->step(); }
  u32 advance(i64 real_ns) { return scheduler_->advance(real_ns); }

  SimWorld& world() noexcept { return *world_; }
  sim::SimScheduler& scheduler() noexcept { return *scheduler_; }
  // Phases that ran flecs systems, and phases that had none and were skipped, since construction.
  u64 phases_run() const noexcept { return phases_run_; }
  u64 phases_skipped() const noexcept { return phases_skipped_; }
  // The flecs pipeline that runs one phase's systems.
  flecs::entity pipeline(TickPhase phase) const noexcept;

 private:
  static void begin_tick(void* context, SimTick tick, GameTime time, GameTime step);
  static void run_phase(void* context, TickPhase phase, SimTick tick, GameTime time, GameTime step);
  static void end_tick(void* context, SimTick tick, GameTime time);

  SimWorld* world_ = nullptr;
  sim::SimScheduler* scheduler_ = nullptr;
  flecs::entity_t pipelines_[k_phase_count] = {};
  ecs_query_t* has_systems_[k_phase_count] = {};
  f32 step_seconds_ = 0.0f;
  u64 phases_run_ = 0;
  u64 phases_skipped_ = 0;
};

}  // namespace engine::ecs
