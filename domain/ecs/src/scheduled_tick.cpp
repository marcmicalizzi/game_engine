#include <core/base/assert.h>
#include <core/log/log.h>
#include <domain/ecs/scheduled_tick.h>

namespace engine::ecs {

ENGINE_LOG_CATEGORY_DECLARE(log_ecs);

ScheduledTick::ScheduledTick(SimWorld& sim_world, sim::SimScheduler& sim_scheduler)
    : world_(&sim_world), scheduler_(&sim_scheduler), step_seconds_(sim_world.step_seconds()) {
  // One clock: the scheduler's step is the step, and a world configured for another rate would
  // hand its systems a delta that disagrees with the game time the scheduler publishes.
  ENGINE_VERIFY(sim_scheduler.step_size().us == sim_world.game_clock().us_per_tick(),
                "ecs::ScheduledTick: the world and the scheduler must run the same fixed step");
  ENGINE_ASSERT(!sim_world.scheduled(), "ecs::ScheduledTick: this world already has a scheduler");
  sim_world.set_scheduled(true);
  sim_world.sync_clock(sim_scheduler.tick(), sim_scheduler.game_time());

  flecs::world& w = sim_world.world();
  for (u32 phase = 0; phase < k_phase_count; ++phase) {
    const flecs::entity_t phase_entity = sim_world.phase(static_cast<TickPhase>(phase)).id();
    // One pipeline per phase: every system that names the phase, in creation order (flecs' own
    // default ordering for a pipeline), which is the order `SimWorld::step()` runs them in within a
    // phase — so the two executors run the same systems in the same order.
    ecs_pipeline_desc_t pipeline = {};
    pipeline.query.terms[0].id = EcsSystem;
    pipeline.query.terms[1].id = phase_entity;
    pipelines_[phase] = ecs_pipeline_init(w.c_ptr(), &pipeline);
    // The cheap question asked before running one: does the phase have a system at all.
    ecs_query_desc_t any = {};
    any.terms[0].id = EcsSystem;
    any.terms[1].id = phase_entity;
    any.cache_kind = EcsQueryCacheAuto;
    has_systems_[phase] = ecs_query_init(w.c_ptr(), &any);
  }

  sim::TickExecutor executor;
  executor.context = this;
  executor.begin_tick = &ScheduledTick::begin_tick;
  executor.run_phase = &ScheduledTick::run_phase;
  executor.end_tick = &ScheduledTick::end_tick;
  sim_scheduler.set_executor(executor);
}

ScheduledTick::~ScheduledTick() {
  scheduler_->set_executor(sim::TickExecutor{});
  flecs::world& w = world_->world();
  for (u32 phase = 0; phase < k_phase_count; ++phase) {
    if (has_systems_[phase] != nullptr) ecs_query_fini(has_systems_[phase]);
    if (pipelines_[phase] != 0) ecs_delete(w.c_ptr(), pipelines_[phase]);
  }
  world_->set_scheduled(false);
}

flecs::entity ScheduledTick::pipeline(TickPhase phase) const noexcept {
  const u32 index = static_cast<u32>(phase);
  return flecs::entity(world_->world().c_ptr(), index < k_phase_count ? pipelines_[index] : 0);
}

void ScheduledTick::begin_tick(void* context, SimTick tick, GameTime time, GameTime) {
  auto* self = static_cast<ScheduledTick*>(context);
  // Published before any phase runs, exactly as `SimWorld::step()` publishes before its pipeline:
  // every phase of this tick sees this tick's numbers.
  self->world_->sync_clock(tick, time);
  ecs_frame_begin(self->world_->world().c_ptr(), self->step_seconds_);
}

void ScheduledTick::run_phase(void* context, TickPhase phase, SimTick, GameTime, GameTime) {
  auto* self = static_cast<ScheduledTick*>(context);
  const u32 index = static_cast<u32>(phase);
  if (!ecs_query_is_true(self->has_systems_[index])) {
    ++self->phases_skipped_;
    return;
  }
  ++self->phases_run_;
  ecs_run_pipeline(self->world_->world().c_ptr(), self->pipelines_[index], self->step_seconds_);
}

void ScheduledTick::end_tick(void* context, SimTick, GameTime) {
  auto* self = static_cast<ScheduledTick*>(context);
  ecs_frame_end(self->world_->world().c_ptr());
  self->world_->watch_tables();
}

}  // namespace engine::ecs
