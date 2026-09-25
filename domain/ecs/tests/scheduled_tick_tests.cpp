// The engine's scheduler as the executor (domain/ecs/scheduled_tick.h; ADR-0038, proposed): one
// clock, the plan's phase order, flecs' systems run inside each phase, and the same bytes as flecs'
// own pipeline.
#include <core/containers/vector.h>
#include <core/jobs/job_system.h>
#include <domain/ecs/os_api.h>
#include <domain/ecs/scheduled_tick.h>
#include <domain/ecs/sim_world.h>
#include <domain/ecs/world_commands.h>
#include <domain/sim/scheduler.h>

#include <doctest/doctest.h>

#include <schemas/ecs_demo.h>
#include <schemas/ecs_demo_ecs.h>
#include <string>

using namespace engine;
using namespace engine::ecs;

namespace {

struct Position {
  f32 x = 0;
  f32 y = 0;
};
struct Velocity {
  f32 x = 0;
  f32 y = 0;
};
struct Trail {
  u32 written = 0;
};

struct Trace {
  Vector<std::string> events;
  Vector<u64> ticks;
  Vector<i64> times;
  Vector<f32> deltas;
};

// A table system that notes its phase, so a test can see where it ran relative to flecs'.
void note_table(sim::SystemContext& context, sim::Batch) {
  auto* trace = static_cast<Trace*>(context.context);
  trace->events.push_back(std::string("table:") + sim::phase_name(context.phase));
}

}  // namespace

TEST_CASE("ecs: under the scheduler, the phases run in plan order with flecs' systems inside") {
  SimWorld world;
  sim::SimScheduler scheduler;
  Trace trace;
  // Back to front, so the order that comes out is the phases' and not creation order.
  Vector<std::string> names;
  names.reserve(sim::k_phase_count);
  for (u32 i = sim::k_phase_count; i > 0; --i) {
    const auto phase = static_cast<TickPhase>(i - 1u);
    names.push_back(std::string("flecs_in_") + phase_entity_name(phase));
    world.world()
        .system(names.back().c_str())
        .kind(world.phase(phase))
        .run([&trace, phase](flecs::iter& it) {
          while (it.next()) {
          }
          trace.events.push_back(std::string("flecs:") + sim::phase_name(phase));
          trace.deltas.push_back(it.delta_time());
        });
  }
  // A table system in one phase, to show where the table's waves run within it.
  sim::SystemDesc table;
  table.name = "table.lod";
  table.phase = TickPhase::Lod;
  table.context = &trace;
  table.tick = &note_table;
  scheduler.add_system(table);

  ScheduledTick tick(world, scheduler);
  CHECK(world.scheduled());
  tick.step();

  REQUIRE(trace.events.size() == sim::k_phase_count + 1u);
  u32 at = 0;
  for (u32 phase = 0; phase < sim::k_phase_count; ++phase) {
    const char* name = sim::phase_name(static_cast<TickPhase>(phase));
    CHECK(trace.events[at++] == std::string("flecs:") + name);
    // Within a phase, flecs' systems first and then the table's waves (ADR-0038).
    if (static_cast<TickPhase>(phase) == TickPhase::Lod) CHECK(trace.events[at++] == "table:lod");
  }
  // The step is the scheduler's, and flecs' systems are handed it as their delta.
  for (const f32 delta : trace.deltas)
    CHECK(delta == doctest::Approx(1.0f / 60.0f));
  CHECK(tick.phases_run() == sim::k_phase_count);
}

TEST_CASE("ecs: under the scheduler there is one clock, and the world reads it") {
  SimWorld world;
  sim::SimSchedulerConfig config;
  config.epoch = GameTime::from_hours(6);
  sim::SimScheduler scheduler(config);
  Trace trace;
  world.world()
      .system("read_clock")
      .kind(world.phase(TickPhase::Systems))
      .run([&trace](flecs::iter& it) {
        while (it.next()) {
        }
        trace.ticks.push_back(it.world().get<SimTick>().value);
        trace.times.push_back(it.world().get<GameTime>().us);
      });
  {
    ScheduledTick tick(world, scheduler);
    // The world's clock is set to the scheduler's the moment it is attached.
    CHECK(world.game_time().us == GameTime::from_hours(6).us);
    for (int i = 0; i < 3; ++i)
      tick.step();
    CHECK(tick.phases_run() == 3);
    // Seven empty phases a tick, skipped without asking flecs to run a pipeline.
    CHECK(tick.phases_skipped() == 3u * (sim::k_phase_count - 1u));
  }
  REQUIRE(trace.ticks.size() == 3);
  const i64 step = scheduler.step_size().us;
  for (u64 i = 0; i < 3; ++i) {
    CHECK(trace.ticks[static_cast<u32>(i)] == i + 1);
    CHECK(trace.times[static_cast<u32>(i)] ==
          GameTime::from_hours(6).us + static_cast<i64>(i + 1) * step);
  }
  CHECK(world.tick().value == scheduler.tick().value);
  CHECK(world.game_time().us == scheduler.game_time().us);
  // Detached, the world may step itself again.
  CHECK_FALSE(world.scheduled());
  world.step();
  CHECK(world.tick().value == 4);
}

TEST_CASE("ecs: the scheduler as executor computes what flecs' pipeline computes, bit for bit") {
  struct Sample {
    f32 x = 0;
    f32 y = 0;
    u32 written = 0;
  };
  const auto run = [](bool scheduled, u32 workers) {
    jobs::JobSystemConfig config;
    config.performance_workers = 4;
    config.efficiency_workers = 1;
    jobs::JobSystem js(config);
    JobOsApi adapter(js);
    SimWorld world;
    for (int i = 0; i < 1024; ++i) {
      world.world()
          .entity()
          .set<Position>(Position{static_cast<f32>(i), static_cast<f32>(i) * 0.5f})
          .set<Velocity>(Velocity{1.0f / static_cast<f32>(i + 1), 0.25f})
          .set<Trail>(Trail{});
    }
    world.world()
        .system<Position, const Velocity>("move")
        .kind(world.phase(TickPhase::Systems))
        .multi_threaded()
        .each([](flecs::iter& it, size_t, Position& p, const Velocity& v) {
          p.x += v.x * it.delta_time() * 60.0f;
          p.y += v.y;
        });
    world.world()
        .system<Trail, const Position>("stamp")
        .kind(world.phase(TickPhase::Lod))
        .multi_threaded()
        .each([](Trail& t, const Position& p) { t.written += static_cast<u32>(p.x) + 1u; });
    if (workers > 1) REQUIRE(set_workers(world.world(), workers));
    sim::SimScheduler scheduler;
    if (scheduled) {
      ScheduledTick tick(world, scheduler);
      for (int t = 0; t < 16; ++t)
        tick.step();
    } else {
      for (int t = 0; t < 16; ++t)
        world.step();
    }
    Vector<Sample> out;
    world.world().query_builder<const Position, const Trail>().build().each(
        [&out](const Position& p, const Trail& t) { out.push_back(Sample{p.x, p.y, t.written}); });
    return out;
  };

  const Vector<Sample> pipeline = run(false, 1);
  REQUIRE(pipeline.size() == 1024);
  for (const u32 workers : {1u, 3u}) {
    const Vector<Sample> scheduled = run(true, workers);
    REQUIRE(scheduled.size() == pipeline.size());
    bool identical = true;
    for (u32 i = 0; i < pipeline.size(); ++i) {
      identical = identical && scheduled[i].x == pipeline[i].x && scheduled[i].y == pipeline[i].y &&
                  scheduled[i].written == pipeline[i].written;
    }
    CHECK(identical);
  }
}

TEST_CASE("ecs: world commands drain at the top of EventsIn under the scheduler") {
  SimWorld world;
  demo::register_ecs_demo_components(world.world());
  sim::SimScheduler scheduler;
  WorldCommands commands(world.world());
  commands.install(world, TickPhase::EventsIn);
  u32 seen = 0;
  world.world()
      .system<const demo::Standing>("count")
      .kind(world.phase(TickPhase::Systems))
      .each([&seen](const demo::Standing&) { ++seen; });
  ScheduledTick tick(world, scheduler);

  const Id128 id = Id128::from_parts(9, 9);
  commands.create(id);
  JsonValue standing = JsonValue::object();
  standing.set("wealth", JsonValue(i64{12}));
  REQUIRE(commands.set_json(id, "engine.ecs.demo.Standing", standing));
  CHECK(commands.pending() == 2);
  tick.step();
  CHECK(commands.pending() == 0);
  // Applied in EventsIn, merged at the phase's end, and seen by a system in a later phase.
  CHECK(seen == 1);
}
