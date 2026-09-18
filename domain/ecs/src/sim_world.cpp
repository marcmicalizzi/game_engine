#include <core/log/log.h>
#include <domain/ecs/identity.h>
#include <domain/ecs/os_api.h>
#include <domain/ecs/sim_world.h>

namespace engine::ecs {

ENGINE_LOG_CATEGORY_DECLARE(log_ecs);

namespace {

// The phase entities' names. They are what the flecs explorer, the pipeline dump, and any log
// of the tick show, so they read as the plan's phase list and not as C++ identifiers.
constexpr const char* k_phase_names[static_cast<usize>(TickPhase::Count)] = {
    "sim_input",   "sim_events_in",    "sim_lod",        "sim_systems",
    "sim_physics", "sim_post_physics", "sim_events_out", "sim_persist",
};

}  // namespace

const char* phase_entity_name(TickPhase phase) noexcept {
  const usize index = static_cast<usize>(phase);
  if (index >= static_cast<usize>(TickPhase::Count)) return "unknown";
  return k_phase_names[index];
}

bool TableWatch::check(const flecs::world& world) noexcept {
  const ecs_world_info_t* info = ecs_get_world_info(world.c_ptr());
  if (info == nullptr) return false;
  tables_ = static_cast<u32>(info->table_count);
  if (config_.expected_archetypes == 0) return false;

  const f32 limit = static_cast<f32>(config_.expected_archetypes) * config_.warn_multiple;
  if (static_cast<f32>(tables_) <= limit) return false;
  // Warn once per *step past* the last warning, not once per check: a world that settles at
  // 9,000 tables should say so once, and one that keeps climbing should keep saying so.
  if (tables_ <= warned_at_) return false;

  warned_at_ = tables_;
  ++warnings_;
  ENGINE_LOG_WARN(log_ecs, "archetype count far past the world's estimate",
                  log::field("tables", tables_),
                  log::field("expected_archetypes", config_.expected_archetypes),
                  log::field("warn_multiple", config_.warn_multiple));
  return true;
}

SimWorld::SimWorld() : SimWorld(SimWorldConfig{}) {}

SimWorld::SimWorld(const SimWorldConfig& config)
    : clock_(config.hz, config.max_steps_per_advance),
      game_clock_(clock_, config.game_seconds_per_real_second),
      table_watch_(config.table_watch) {
  install_log_sink();
  step_seconds_ = static_cast<f32>(clock_.step_seconds());
  game_clock_.jump_to(config.epoch);
  build_phases();
  publish_singletons();
  // The identity seam is installed here rather than on first use, because "on first use" can be
  // from inside a system body or a deferred batch, and an observer created there does not see
  // the commands that are already queued in front of it. A world that has phases has identity.
  identity_map(world_);
}

SimWorld::~SimWorld() {
  // A world with task workers must stop using them before it is torn down, and doing it here
  // rather than leaving it to flecs means the adapter's task slots are certainly free by the
  // time a JobOsApi that outlives this world is destroyed.
  ecs_set_task_threads(world_.c_ptr(), 1);
}

void SimWorld::build_phases() {
  // Every phase hangs off flecs' last built-in phase. The engine does not use the built-in
  // phases at all, but a system that forgets to name one lands in OnUpdate by default, and
  // anchoring here means such a system runs *before* the engine's tick rather than somewhere in
  // the middle of it: a visible mistake instead of an invisible one.
  flecs::entity previous = world_.entity(flecs::PostFrame);
  for (usize i = 0; i < static_cast<usize>(TickPhase::Count); ++i) {
    const flecs::entity phase =
        world_.entity(k_phase_names[i]).add(flecs::Phase).depends_on(previous);
    phases_.by_phase[i] = phase;
    previous = phase;
  }
}

void SimWorld::publish_singletons() {
  world_.set<SimTick>(tick_);
  world_.set<GameTime>(game_clock_.now());
}

void SimWorld::run_tick() {
  ++tick_;
  game_clock_.advance_tick();
  // Published before the pipeline runs, so every phase of this tick sees this tick's numbers.
  publish_singletons();
  world_.progress(step_seconds_);
#if ENGINE_DEBUG
  // Debug only: a release build pays nothing for a diagnostic whose whole job is to be read
  // during development. The period keeps even the debug cost to a world-info read every 64th
  // tick (plan 11 §11.10).
  const u32 period = table_watch_.config().check_every_ticks;
  if (period != 0 && tick_.value % period == 0) table_watch_.check(world_);
#endif
}

void SimWorld::step() { run_tick(); }

u32 SimWorld::advance(i64 real_ns) {
  clock_.advance(real_ns);
  u32 steps = 0;
  while (clock_.step()) {
    run_tick();
    ++steps;
  }
  return steps;
}

}  // namespace engine::ecs
