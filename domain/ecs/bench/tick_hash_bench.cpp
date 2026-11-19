// The state hash a tick's cost when on (domain/ecs/tick_hash.h; ecs.md, "The state hash a tick";
// roadmap T53): one phase's hash of a world of 10^4 and 10^5 entities, each with a flat component
// (`Standing`: a vec3, an i64, an f32 and a u32) and one holding a string (`Label`), and a whole
// scheduled tick of that world with the hash off, at the tick's end only, and at all eight phases.
// One item is one entity.
#include <core/jobs/job_system.h>
#include <domain/ecs/identity.h>
#include <domain/ecs/scheduled_tick.h>
#include <domain/ecs/sim_world.h>
#include <domain/ecs/tick_hash.h>
#include <domain/sim/hash_log.h>
#include <domain/sim/scheduler.h>
#include <foundation/bench/bench.h>

#include <memory>
#include <schemas/ecs_demo.h>
#include <schemas/ecs_demo_ecs.h>
#include <string>

using namespace engine;
using namespace engine::ecs;

namespace {

u32 entity_count(bench::State& state) {
  return bench::smoke_mode() ? 1000u : static_cast<u32>(state.arg());
}

struct HashWorld {
  SimWorld sim;
  sim::SimScheduler scheduler;
  std::unique_ptr<ScheduledTick> tick;
  sim::HashLog log;
  std::unique_ptr<TickHash> hash;

  HashWorld(u32 entities, u8 phases, TickHashConfig config = {}) {
    demo::register_ecs_demo_components(sim.world());
    for (u32 i = 0; i < entities; ++i) {
      flecs::entity e = create_entity(sim.world(), Id128{1u, i + 1u});
      e.set<demo::Standing>({Vec3{static_cast<f32>(i), 0.0f, 1.0f}, i64{i}, 1.0f, i % 7u, 0u});
      e.set<demo::Label>({"stall " + std::to_string(i), static_cast<u8>(i)});
    }
    sim.world()
        .system<demo::Standing>("bench_drift")
        .kind(sim.phase(TickPhase::Systems))
        .each([](demo::Standing& s) { s.position.y += 0.25f; });
    tick = std::make_unique<ScheduledTick>(sim, scheduler);
    if (phases != 0) {
      config.phases = phases;
      config.reserve_ticks = 1u << 16;
      hash = std::make_unique<TickHash>(sim, scheduler, log, config);
    }
  }
};

// The hash's own job system for the `.jobs` rows: eight performance workers, unpinned, apart from
// the world's (the scheduled tick here runs on one thread).
jobs::JobSystem& hash_jobs() {
  static std::unique_ptr<jobs::JobSystem> made;
  if (made == nullptr) {
    jobs::JobSystemConfig config;
    config.performance_workers = 8;
    config.efficiency_workers = 0;
    config.pin_threads = false;
    made = std::make_unique<jobs::JobSystem>(config);
  }
  return *made;
}

TickHashConfig with_jobs() {
  TickHashConfig config;
  config.jobs = &hash_jobs();
  return config;
}

TickHashConfig reflected() {
  TickHashConfig config;
  config.reflected = true;
  return config;
}

void run_phase(bench::State& state, const TickHashConfig& config) {
  const u32 n = entity_count(state);
  HashWorld world(n, 0x80u, config);
  Vector<u64> out;
  world.hash->hash_now(sim::TickPhase::Persist, out);  // fixes the table set
  while (state.keep_running()) {
    world.hash->hash_now(sim::TickPhase::Persist, out);
    bench::keep(out[0]);
  }
  state.set_items(n);
}

void run_tick(bench::State& state, u8 phases, const TickHashConfig& config = {}) {
  const u32 n = entity_count(state);
  HashWorld world(n, phases, config);
  u64 steps = 0;
  while (state.keep_running()) {
    world.tick->step();
    // The log is reserved for 2^16 ticks; past that it is cleared, outside nothing measured
    // differently: a clear keeps its capacity.
    if (++steps % (1u << 16) == 0 && world.hash != nullptr) world.log.clear();
  }
  bench::keep(world.log.rows().size());
  state.set_items(n);
}

}  // namespace

ENGINE_BENCH_ARGS(ecs_tick_hash_phase, "ecs.tick_hash.phase", 10'000, 100'000) {
  run_phase(state, {});
}

// The same phase with every table hashed by reflection (the path before the flat programs), and
// on the hash's job system.
ENGINE_BENCH_ARGS(ecs_tick_hash_phase_reflected, "ecs.tick_hash.phase.reflected", 10'000, 100'000) {
  run_phase(state, reflected());
}

ENGINE_BENCH_ARGS(ecs_tick_hash_phase_jobs, "ecs.tick_hash.phase.jobs", 10'000, 100'000) {
  run_phase(state, with_jobs());
}

ENGINE_BENCH_ARGS(ecs_tick_hash_tick_off, "ecs.tick_hash.tick.off", 10'000, 100'000) {
  run_tick(state, 0u);
}

ENGINE_BENCH_ARGS(ecs_tick_hash_tick_persist, "ecs.tick_hash.tick.persist", 10'000, 100'000) {
  run_tick(state, 0x80u);
}

ENGINE_BENCH_ARGS(ecs_tick_hash_tick_all, "ecs.tick_hash.tick.all_phases", 10'000, 100'000) {
  run_tick(state, 0xFFu);
}

ENGINE_BENCH_ARGS(ecs_tick_hash_tick_all_jobs, "ecs.tick_hash.tick.all_phases.jobs", 10'000,
                  100'000) {
  run_tick(state, 0xFFu, with_jobs());
}
