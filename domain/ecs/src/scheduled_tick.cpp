#include <core/base/assert.h>
#include <core/log/log.h>
#include <domain/ecs/scheduled_tick.h>
#include <domain/ecs/systems.h>

#include <span>

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
  // A world handed back to its own pipeline runs every system it had before it was scheduled.
  for (const flecs::entity_t system : tier_disabled_) {
    if (ecs_is_alive(w.c_ptr(), system)) ecs_enable(w.c_ptr(), system, true);
  }
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

void ScheduledTick::apply_tiers(u8 tiers) {
  flecs::world& w = world_->world();
  ecs_world_t* c = w.c_ptr();
  const SystemRegistry* registry = systems_if_present(w);
  const std::span<const RegisteredSystem> all =
      registry != nullptr ? registry->all() : std::span<const RegisteredSystem>{};
  for (const RegisteredSystem& row : all) {
    if (row.system == 0 || !ecs_is_alive(c, row.system)) continue;
    const bool wanted = (row.desc.tiers & tiers) != 0;
    u32 found = tier_disabled_.size();
    for (u32 i = 0; i < tier_disabled_.size(); ++i) {
      if (tier_disabled_[i] == row.system) found = i;
    }
    const bool ours = found < tier_disabled_.size();
    if (!wanted && !ours && !ecs_has_id(c, row.system, EcsDisabled)) {
      // Only a system that is running is ours to stop, so one its owner disabled stays theirs.
      ecs_enable(c, row.system, false);
      tier_disabled_.push_back(row.system);
    } else if (wanted && ours) {
      ecs_enable(c, row.system, true);
      tier_disabled_[found] = tier_disabled_.back();
      tier_disabled_.pop_back();
    }
  }
  applied_tiers_ = tiers;
  applied_systems_ = static_cast<u32>(all.size());
}

void ScheduledTick::begin_tick(void* context, SimTick tick, GameTime time, GameTime) {
  auto* self = static_cast<ScheduledTick*>(context);
  // Tier filtering for flecs' systems, the executor's half of `SimScheduler::set_active_tiers`:
  // applied before any phase runs, so the whole tick runs under one mask, and only when the mask
  // or the registry changed, so a steady tick costs one compare.
  const u8 tiers = self->scheduler_->active_tiers();
  const SystemRegistry* registry = systems_if_present(self->world_->world());
  const u32 registered = registry != nullptr ? registry->size() : 0u;
  if (tiers != self->applied_tiers_ || registered != self->applied_systems_) {
    self->apply_tiers(tiers);
  }
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
