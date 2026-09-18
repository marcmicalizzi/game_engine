#include <domain/ecs/os_api.h>
#include <domain/ecs/sim_world.h>

namespace engine::ecs {

namespace {

// The phase entities' names. They are what the flecs explorer, the pipeline dump, and any log
// of the tick show, so they read as the plan's phase list and not as C++ identifiers.
constexpr const char* k_phase_names[static_cast<usize>(TickPhase::Count)] = {
    "sim_input",   "sim_events_in",    "sim_lod",        "sim_systems",
    "sim_physics", "sim_post_physics", "sim_events_out", "sim_persist",
};

}  // namespace

const char* phase_name(TickPhase phase) noexcept {
  const usize index = static_cast<usize>(phase);
  if (index >= static_cast<usize>(TickPhase::Count)) return "unknown";
  return k_phase_names[index];
}

SimWorld::SimWorld() : SimWorld(SimWorldConfig{}) {}

SimWorld::SimWorld(const SimWorldConfig& config)
    : clock_(config.hz, config.max_steps_per_advance),
      game_clock_(clock_, config.game_seconds_per_real_second) {
  install_log_sink();
  step_seconds_ = static_cast<f32>(clock_.step_seconds());
  game_clock_.jump_to(config.epoch);
  build_phases();
  publish_singletons();
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
