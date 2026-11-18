// The state hash a tick over scheduled residents (domain/ecs/tick_hash.h; roadmap T53, plan 09
// §9.2): a world of residents is recorded for 10^4 ticks — its hash log and the record of the run —
// and replayed from the record alone, with an observer walking through it so residents are promoted,
// walked and demoted along the way; the bisector reports no divergence. What it would report, if a
// resident's routine depended on anything but the seed and the tick, is shown on a small world in
// domain/ecs's tick_hash_tests.cpp.
#include <domain/doc/document.h>
#include <domain/ecs/materialize.h>
#include <domain/ecs/scheduled_tick.h>
#include <domain/ecs/sim_world.h>
#include <domain/ecs/tick_hash.h>
#include <domain/sim/hash_log.h>
#include <domain/sim/materialize.h>
#include <domain/sim/tiers.h>
#include <foundation/io/vfs.h>
#include <systems/npc/generator.h>
#include <systems/npc/npc.h>
#include <systems/npc/routine.h>

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <test_temp_dir.h>

using namespace engine;
using namespace engine::npc;

namespace {

constexpr u32 k_hz = 1;
constexpr u32 k_residents = 200;
constexpr u64 k_ticks = 10'000;
// Monday 06:00: the morning's commutes fall inside the run.
constexpr i64 k_morning = 6 * 60 * k_us_per_minute;

// Everything a replay builds from the record: the generator's world from the seed, the residents,
// the entity store and the scheduler, an observer walking east at 0.02 m a tick, and the hash.
struct Run {
  doc::Document document;
  ecs::SimWorld sim;
  sim::SimScheduler scheduler;
  ecs::RecordMaterializer records;
  NpcSystem npc;
  sim::Materializer driver;
  sim::ObserverSet observers;
  std::unique_ptr<ecs::ScheduledTick> tick;
  std::unique_ptr<ecs::TickHash> hash;

  static ecs::SimWorldConfig world_config(u32 hz) {
    ecs::SimWorldConfig c;
    c.hz = hz;
    return c;
  }
  static sim::SimSchedulerConfig scheduler_config(const sim::RunRecord& record) {
    sim::SimSchedulerConfig c;
    c.hz = record.hz;
    c.epoch = GameTime{record.start_time_us};
    c.start_tick = SimTick{record.start_tick};
    return c;
  }
  static NpcConfig npc_config(u64 seed) {
    NpcConfig c;
    c.world_seed = seed;
    c.lod_every = 1;
    return c;
  }
  static sim::MaterializeConfig driver_config() {
    sim::MaterializeConfig c;
    c.tier = 2;
    return c;
  }

  Run(const sim::RunRecord& record, sim::HashLog& log)
      : sim(world_config(record.hz)),
        scheduler(scheduler_config(record)),
        records(sim.world()),
        npc(npc_config(record.seed)),
        driver(scheduler, driver_config()) {
    GeneratorParams p;
    p.seed = record.seed;
    p.residents = k_residents;
    p.min_x = -96.0;
    p.min_z = -64.0;
    p.max_x = 160.0;
    p.max_z = 96.0;
    p.time_us = record.start_time_us;
    document.add_layer("base", doc::LayerRole::Base);
    const u32 layer = document.add_layer(generate_layer(p));
    document.set_edit_layer(layer);
    document.rebuild_index();

    npc.install(sim, scheduler);
    scheduler.add_hooks(records.hooks());
    scheduler.add_hooks(npc.hooks());
    driver.set_target(records.target());
    tick = std::make_unique<ecs::ScheduledTick>(sim, scheduler);
    observers.add(WorldPos{-96.0, 0.0, 0.0}, 1.0f);
    npc.set_observers(&observers);
    npc.places().refresh(document);
    driver.materialize(document);
    // Last, after every system it hashes the work of.
    ecs::TickHashConfig config;
    config.reserve_ticks = record.ticks;
    hash = std::make_unique<ecs::TickHash>(sim, scheduler, log, config);
  }
  ~Run() {
    driver.dematerialize_all();
    tick.reset();
  }

  void run(u64 ticks) {
    for (u64 i = 0; i < ticks; ++i) {
      // The observer walks between ticks, the way a host moves its observers (world.md).
      const WorldPos at = observers.position(0);
      observers.clear();
      observers.add(WorldPos{at.x + 0.02, at.y, at.z}, 1.0f);
      tick->step();
    }
  }
};

}  // namespace

TEST_CASE("npc: 10^4 ticks of residents recorded and replayed from the record hash the same") {
  test::TempDir dir;
  sim::RunRecord record;
  record.seed = 42;
  record.hz = k_hz;
  record.start_time_us = k_morning;
  record.ticks = k_ticks;
  record.hash_log = "residents.hashlog";

  sim::HashLog recorded;
  u64 transitions = 0;
  {
    Run run(record, recorded);
    run.run(k_ticks);
    transitions = run.npc.stats().transitions;
    MESSAGE("tables " << run.hash->tables().size() << ", entity components hashed "
                      << run.hash->stats().entities << ", log " << recorded.memory_bytes()
                      << " bytes, per tick " << run.hash->memory().per_tick);
    CHECK(run.hash->stats().refused == 0);
    CHECK(run.hash->memory().per_entity == 0);
  }
  // The morning's commutes happened: a run in which nothing moved would agree with anything.
  CHECK(transitions > k_residents);
  REQUIRE(recorded.rows().size() == k_ticks * sim::k_phase_count);
  REQUIRE(recorded.save(sim::run_file(dir.path(), record.hash_log)));
  REQUIRE(sim::write_run_record(dir.path(), record));

  // The replay knows the record and nothing else.
  sim::RunRecord read;
  std::string error;
  REQUIRE_MESSAGE(sim::read_run_record(dir.path(), read, &error), error);
  sim::HashLog expected;
  REQUIRE_MESSAGE(expected.load(sim::run_file(dir.path(), read.hash_log), &error), error);
  sim::HashLog replayed;
  {
    Run run(read, replayed);
    run.run(read.ticks);
  }
  const sim::Divergence d = sim::first_divergence(expected, replayed);
  CHECK_FALSE(d.found);
  CHECK_FALSE(d.rows_differ);
  CHECK(d.rows_compared == k_ticks * sim::k_phase_count);
  // The residents' own tables are among those hashed.
  CHECK(expected.table_index("engine.npc.NpcState") != ~0u);
}
