#include <core/containers/vector.h>
#include <core/jobs/job_system.h>
#include <domain/ecs/os_api.h>
#include <domain/ecs/sim_world.h>

#include <doctest/doctest.h>

#include <functional>
#include <mutex>
#include <string>
#include <thread>

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

// Every system writes into one of these, so the assertions are about order and threading rather
// than about what the systems compute.
struct Log {
  Vector<u32> order;
  Vector<u64> tick_seen;
  Vector<i64> time_seen;
  std::string trail;
};

struct ThreadWitness {
  std::mutex mutex;
  Vector<u64> threads;  // distinct hashes of the threads a system body ran on
  Vector<u8> on_pool;   // parallel to `threads`: was it a worker of the right job system
  u32 invocations = 0;

  void note(bool pool_worker) {
    const u64 id = static_cast<u64>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    std::lock_guard lock(mutex);
    ++invocations;
    for (u32 i = 0; i < threads.size(); ++i) {
      if (threads[i] == id) {
        if (!pool_worker) on_pool[i] = 0;
        return;
      }
    }
    threads.push_back(id);
    on_pool.push_back(pool_worker ? u8{1} : u8{0});
  }
};

}  // namespace

TEST_CASE("ecs: the tick phases run in the order of plan 05 section 5.2") {
  SimWorld sim;
  Log log;

  // Registered back to front, so the order that comes out is the phases' and not the order the
  // systems were created in.
  Vector<std::string> names;
  names.reserve(static_cast<usize>(TickPhase::Count));
  for (usize i = static_cast<usize>(TickPhase::Count); i > 0; --i) {
    const auto which = static_cast<TickPhase>(i - 1);
    // Not the phase's own name: flecs entities are named, so `world.system("sim_lod")` would
    // find the phase entity and turn *it* into a system that depends on itself.
    names.push_back(std::string("system_in_") + phase_entity_name(which));
    sim.world()
        .system(names.back().c_str())
        .kind(sim.phase(which))
        .run([&log, which](flecs::iter& it) {
          while (it.next()) {
          }
          log.order.push_back(static_cast<u32>(which));
        });
  }

  sim.step();
  REQUIRE(log.order.size() == static_cast<usize>(TickPhase::Count));
  for (u32 i = 0; i < log.order.size(); ++i)
    CHECK(log.order[i] == i);

  // Every phase is a distinct, named entity carrying flecs' Phase tag, which is what makes the
  // default pipeline pick systems up without the pipeline knowing they exist (ADR-0027).
  for (usize i = 0; i < static_cast<usize>(TickPhase::Count); ++i) {
    const flecs::entity phase = sim.phases().by_phase[i];
    CHECK(phase.is_valid());
    CHECK(phase.has(flecs::Phase));
    CHECK(std::string(phase.name().c_str()) == phase_entity_name(static_cast<TickPhase>(i)));
  }

  log.order.clear();
  sim.step();
  CHECK(log.order.size() == static_cast<usize>(TickPhase::Count));
}

TEST_CASE("ecs: the tick and game time are singletons that advance once per step") {
  SimWorldConfig config;
  config.hz = 50;
  config.epoch = GameTime::from_hours(6);
  SimWorld sim(config);
  Log log;

  sim.world().system("read_clock").kind(sim.phase(TickPhase::Systems)).run([&log](flecs::iter& it) {
    while (it.next()) {
    }
    log.tick_seen.push_back(it.world().get<SimTick>().value);
    log.time_seen.push_back(it.world().get<GameTime>().us);
  });

  // Before the first step the singletons already exist, so a system that runs on tick 0 (or a
  // tool that inspects the world) does not read a missing component.
  CHECK(sim.tick().value == 0);
  CHECK(sim.world().get<SimTick>().value == 0);
  CHECK(sim.world().get<GameTime>().us == GameTime::from_hours(6).us);
  CHECK(sim.step_seconds() == doctest::Approx(1.0f / 50.0f));

  sim.step();
  CHECK(sim.tick().value == 1);
  REQUIRE(log.tick_seen.size() == 1);
  CHECK(log.tick_seen[0] == 1);
  const i64 per_tick = sim.game_clock().us_per_tick();
  CHECK(per_tick == 20'000);  // 50 Hz at real time
  CHECK(log.time_seen[0] == GameTime::from_hours(6).us + per_tick);

  sim.step();
  CHECK(sim.tick().value == 2);
  CHECK(log.tick_seen[1] == 2);
  CHECK(log.time_seen[1] - log.time_seen[0] == per_tick);

  // advance() runs whole steps only, and keeps the remainder for next time.
  const i64 step_ns = 20'000'000;
  CHECK(sim.advance(step_ns * 3 + step_ns / 2) == 3);
  CHECK(sim.tick().value == 5);
  CHECK(sim.advance(step_ns / 2) == 1);
  CHECK(sim.tick().value == 6);

  // The cap stops the spiral of death after a stall; the rest of the time is dropped.
  CHECK(sim.advance(step_ns * 100) == config.max_steps_per_advance);
  CHECK(sim.clock().dropped_steps() > 0);

  // A doubled time scale consumes real time twice as fast without changing the step.
  const u64 before = sim.tick().value;
  sim.clock().reset();
  sim.clock().set_time_scale(2.0);
  CHECK(sim.advance(step_ns) == 2);
  CHECK(sim.tick().value == before + 2);
  CHECK(sim.step_seconds() == doctest::Approx(1.0f / 50.0f));
}

TEST_CASE("ecs: two systems with conflicting write sets run in the order they were created") {
  // Both systems write Trail, so nothing about the data decides which goes first; flecs orders
  // them by creation, and that has to hold at every worker count or the sim is not replayable.
  const auto build = [](SimWorld& sim, Log& log, bool a_first) {
    for (int i = 0; i < 64; ++i)
      sim.world().entity().set<Trail>(Trail{});
    const auto make = [&sim, &log](char label) {
      sim.world()
          .system<Trail>(label == 'A' ? "writer_a" : "writer_b")
          .kind(sim.phase(TickPhase::Systems))
          .multi_threaded()
          .run([&log, label](flecs::iter& it) {
            while (it.next()) {
              auto trail = it.field<Trail>(0);
              for (auto row : it)
                trail[row].written += 1;
            }
            // Every stage runs the body; only stage 0 records, so the string is the order of the
            // systems and not of the workers.
            if (it.world().get_stage_id() == 0) log.trail.push_back(label);
          });
    };
    if (a_first) {
      make('A');
      make('B');
    } else {
      make('B');
      make('A');
    }
  };

  for (const u32 workers : {1u, 2u, 4u}) {
    jobs::JobSystemConfig config;
    config.performance_workers = workers;
    config.efficiency_workers = 1;
    jobs::JobSystem js(config);
    JobOsApi adapter(js);
    const u32 requested = workers <= adapter.max_workers() ? workers : adapter.max_workers();

    // One world at a time: a world's workers hold pool workers for the world's lifetime, so two
    // live worlds share one budget and the second would be refused what the first is holding.
    {
      SimWorld forward;
      Log forward_log;
      build(forward, forward_log, true);
      REQUIRE(set_workers(forward.world(), requested));
      for (int t = 0; t < 4; ++t)
        forward.step();
      CHECK(forward_log.trail == "ABABABAB");
    }
    {
      SimWorld backward;
      Log backward_log;
      build(backward, backward_log, false);
      REQUIRE(set_workers(backward.world(), requested));
      for (int t = 0; t < 4; ++t)
        backward.step();
      CHECK(backward_log.trail == "BABABABA");
    }
  }
}

TEST_CASE("ecs: a multithreaded system runs on the engine's performance pool") {
  jobs::JobSystemConfig config;
  config.performance_workers = 4;
  config.efficiency_workers = 1;
  jobs::JobSystem js(config);
  JobOsApi adapter(js);

  // The default budget lends every performance worker but one, so a world may have as many
  // workers as the pool has threads and the pool still keeps one for everything else.
  CHECK(adapter.hosted_worker_budget() == js.worker_count(jobs::Pool::Performance) - 1);
  CHECK(adapter.max_workers() == js.worker_count(jobs::Pool::Performance));
  CHECK(adapter.hosted_workers() == 0);
  CHECK(&adapter.job_system() == &js);
  CHECK(JobOsApi::current() == &adapter);

  const u32 workers = adapter.max_workers() < 4u ? adapter.max_workers() : 4u;
  REQUIRE(workers >= 2);

  SimWorld sim;
  ThreadWitness witness;
  for (int i = 0; i < 4096; ++i) {
    sim.world().entity().set<Position>(Position{}).set<Velocity>(Velocity{1.0f, 0.5f});
  }
  sim.world()
      .system<Position, const Velocity>("move")
      .kind(sim.phase(TickPhase::Systems))
      .multi_threaded()
      .each([&witness, &js](Position& p, const Velocity& v) {
        p.x += v.x;
        p.y += v.y;
        if (p.x == 1.0f) {  // once per entity per tick is plenty; note only on the first tick
          const jobs::WorkerInfo* info = jobs::JobSystem::current_worker();
          witness.note(info != nullptr && jobs::JobSystem::current() == &js);
        }
      });

  REQUIRE(set_workers(sim.world(), workers));
  // The adapter, not flecs, created the world's workers, once: n-1 long-running jobs for n
  // stages, and they are the pool workers the budget has lent out.
  CHECK(adapter.workers_started() == workers - 1);
  CHECK(adapter.hosted_workers() == workers - 1);
  CHECK(adapter.pool_workers_kept() == js.worker_count(jobs::Pool::Performance) - (workers - 1));

  sim.step();
  sim.step();
  // Still one dispatch, not one a tick: that is the whole point of the hosting.
  CHECK(adapter.workers_started() == workers - 1);
  CHECK(adapter.tasks_started() == 0);
  CHECK(witness.invocations == 4096);  // p.x is 1 only on the first tick
  // One thread per stage: the caller runs stage 0 and the pool runs the rest.
  CHECK(witness.threads.size() == workers);
  u32 pool_threads = 0;
  for (const u8 flag : witness.on_pool)
    pool_threads += flag;
  CHECK(pool_threads == workers - 1);

  // Asking for more workers than the budget can lend is refused rather than hung, and the world
  // is left single-threaded rather than half-configured. The count is measured against the
  // budget and not against `max_workers()` at the call, because a world gives back what it
  // already holds before the request is judged.
  CHECK(!set_workers(sim.world(), adapter.hosted_worker_budget() + 2));
  CHECK(adapter.hosted_workers() == 0);
  sim.step();  // a refusal leaves a world that still ticks

  // The per-tick hosting is still selectable, because the measurement that rejected it is a
  // comparison: it creates and joins its workers inside every step.
  REQUIRE(set_workers(sim.world(), workers, WorkerHosting::Tasks));
  CHECK(adapter.hosted_workers() == 0);  // a task holds nothing between ticks
  sim.step();
  CHECK(adapter.tasks_started() == workers - 1);
  sim.step();
  CHECK(adapter.tasks_started() == 2 * (workers - 1));

  // And a single-threaded world needs no adapter at all.
  CHECK(set_workers(sim.world(), 1));
  CHECK(adapter.hosted_workers() == 0);
}

TEST_CASE("ecs: worlds give their pool workers back") {
  jobs::JobSystemConfig config;
  config.performance_workers = 4;
  config.efficiency_workers = 1;
  jobs::JobSystem js(config);
  JobOsApi adapter(js);
  const u32 workers = adapter.max_workers() < 3u ? adapter.max_workers() : 3u;
  REQUIRE(workers >= 2);

  // Worlds created and destroyed over and over in one process leave nothing occupied. This is
  // the failure the long-running hosting could plausibly have: a worker that holds a pool
  // thread for a world's lifetime holds it forever if the world's teardown forgets it.
  for (int round = 0; round < 8; ++round) {
    SimWorld sim;
    for (int i = 0; i < 256; ++i)
      sim.world().entity().set<Position>(Position{}).set<Velocity>(Velocity{1.0f, 0.5f});
    sim.world()
        .system<Position, const Velocity>("move")
        .kind(sim.phase(TickPhase::Systems))
        .multi_threaded()
        .each([](Position& p, const Velocity& v) {
          p.x += v.x;
          p.y += v.y;
        });
    REQUIRE(set_workers(sim.world(), workers));
    CHECK(adapter.hosted_workers() == workers - 1);
    sim.step();
  }
  CHECK(adapter.hosted_workers() == 0);
  CHECK(adapter.pool_workers_kept() == js.worker_count(jobs::Pool::Performance));

  // Two worlds at once share one budget, and the second is refused what the first is holding
  // rather than being given a worker that has no thread to run on.
  SimWorld a;
  SimWorld b;
  REQUIRE(set_workers(a.world(), adapter.max_workers()));
  CHECK(adapter.hosted_workers() == adapter.hosted_worker_budget());
  CHECK(adapter.max_workers() == 1);
  CHECK(!set_workers(b.world(), 2));
  b.step();  // refused, so single-threaded, and still a world that ticks
  // Reconfiguring a world is not counted twice: it gives back what it holds first.
  REQUIRE(set_workers(a.world(), 2));
  CHECK(adapter.hosted_workers() == 1);
  REQUIRE(set_workers(b.world(), 2));
  CHECK(adapter.hosted_workers() == 2);
  a.step();
  b.step();
}

TEST_CASE("ecs: an adapter that goes away first takes its workers back") {
  // The world outliving the job system is the dangerous order, because a pool thread parked
  // inside flecs cannot be joined: the job system's own destructor would hang on it. The
  // adapter's destructor therefore drops every world it still hosts back to one worker.
  SimWorld sim;
  for (int i = 0; i < 256; ++i)
    sim.world().entity().set<Position>(Position{}).set<Velocity>(Velocity{1.0f, 0.5f});
  sim.world()
      .system<Position, const Velocity>("move")
      .kind(sim.phase(TickPhase::Systems))
      .multi_threaded()
      .each([](Position& p, const Velocity& v) {
        p.x += v.x;
        p.y += v.y;
      });

  {
    jobs::JobSystemConfig config;
    config.performance_workers = 4;
    config.efficiency_workers = 1;
    jobs::JobSystem js(config);
    JobOsApi adapter(js);
    REQUIRE(set_workers(sim.world(), 3));
    sim.step();
    CHECK(adapter.hosted_workers() == 2);
  }

  // Both are gone and the world is still usable, single-threaded, and says so when asked for
  // workers it can no longer have.
  CHECK(JobOsApi::current() == nullptr);
  sim.step();
  CHECK(!set_workers(sim.world(), 4));
  sim.step();
}

TEST_CASE("ecs: a tick computes the same thing at every worker count") {
  // Determinism is the reason the engine may choose a worker count at all (ADR-0010). The world
  // is small on purpose: what is being checked is that the result does not depend on how the
  // rows were split, not how fast the split is.
  struct Sample {
    f32 x = 0;
    f32 y = 0;
    u32 written = 0;
  };
  const auto run = [](u32 workers) {
    jobs::JobSystemConfig config;
    config.performance_workers = 8;
    config.efficiency_workers = 1;
    jobs::JobSystem js(config);
    JobOsApi adapter(js);
    SimWorld sim;
    for (int i = 0; i < 1024; ++i) {
      sim.world()
          .entity()
          .set<Position>(Position{static_cast<f32>(i), static_cast<f32>(i) * 0.5f})
          .set<Velocity>(Velocity{1.0f / static_cast<f32>(i + 1), 0.25f})
          .set<Trail>(Trail{});
    }
    sim.world()
        .system<Position, const Velocity>("move")
        .kind(sim.phase(TickPhase::Systems))
        .multi_threaded()
        .each([](Position& p, const Velocity& v) {
          p.x += v.x;
          p.y += v.y;
        });
    sim.world()
        .system<Trail, const Position>("stamp")
        .kind(sim.phase(TickPhase::Lod))
        .multi_threaded()
        .each([](Trail& t, const Position& p) { t.written += static_cast<u32>(p.x) + 1u; });
    if (workers > 1) REQUIRE(set_workers(sim.world(), workers));
    for (int t = 0; t < 16; ++t)
      sim.step();

    Vector<Sample> out;
    sim.world().query_builder<const Position, const Trail>().build().each(
        [&out](const Position& p, const Trail& t) { out.push_back(Sample{p.x, p.y, t.written}); });
    return out;
  };

  const Vector<Sample> one = run(1);
  REQUIRE(one.size() == 1024);
  for (const u32 workers : {4u, 8u}) {
    const Vector<Sample> many = run(workers);
    REQUIRE(many.size() == one.size());
    bool identical = true;
    for (u32 i = 0; i < one.size(); ++i) {
      // Bit for bit, not approximately: the same rows in the same order doing the same
      // arithmetic. A tolerance here would hide exactly the bug worth finding.
      identical = identical && many[i].x == one[i].x && many[i].y == one[i].y &&
                  many[i].written == one[i].written;
    }
    CHECK(identical);
  }
}

TEST_CASE("ecs: relationships and queries are flecs' own, unwrapped") {
  // The module is deliberately thin: this is the evidence that a consumer writes plain flecs.
  SimWorld sim;
  flecs::world& world = sim.world();

  struct MemberOf {};
  const flecs::entity faction = world.entity("ironbound");
  for (int i = 0; i < 8; ++i) {
    world.entity().set<Position>(Position{static_cast<f32>(i), 0.0f}).add<MemberOf>(faction);
  }
  world.entity().set<Position>(Position{99.0f, 0.0f});

  u32 matched = 0;
  world.query_builder<const Position>().with<MemberOf>(faction).build().each(
      [&matched](const Position&) { ++matched; });
  CHECK(matched == 8);

  // The singletons are core/time's types, so nothing has to translate between two clocks.
  sim.step();
  CHECK(world.get<SimTick>().value == sim.tick().value);
}

TEST_CASE("ecs: phase entity names cover the enum") {
  CHECK(std::string(phase_entity_name(TickPhase::Input)) == "sim_input");
  CHECK(std::string(phase_entity_name(TickPhase::Persist)) == "sim_persist");
  CHECK(std::string(phase_entity_name(TickPhase::Count)) == "unknown");
}
