// The state hash a tick over scheduled residents (domain/ecs/tick_hash.h; roadmap T53, plan 09
// §9.2): a world of residents is recorded for 10^4 ticks — its hash log and the record of the run —
// and replayed from the record alone, with an observer walking through it so residents are
// promoted, walked and demoted along the way; the bisector reports no divergence. What it would
// report, if a resident's routine depended on anything but the seed and the tick, is shown on a
// small world in domain/ecs's tick_hash_tests.cpp.
//
// The second case is roadmap S18's at scale (domain/sim/door.h): the observer is a player steered
// by a stick through the door, the run is recorded under irregular frames and calls that end with a
// write-back flush off the cadence, and the door's record alone replays it two ways.
#include <core/json/json.h>
#include <domain/doc/document.h>
#include <domain/ecs/materialize.h>
#include <domain/ecs/scheduled_tick.h>
#include <domain/ecs/sim_world.h>
#include <domain/ecs/tick_hash.h>
#include <domain/sim/door.h>
#include <domain/sim/hash_log.h>
#include <domain/sim/materialize.h>
#include <domain/sim/tiers.h>
#include <foundation/input/input.h>
#include <foundation/io/vfs.h>
#include <systems/npc/generator.h>
#include <systems/npc/npc.h>
#include <systems/npc/routine.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <memory>
#include <string>

using namespace engine;
using namespace engine::npc;

namespace {

constexpr u32 k_hz = 1;
constexpr u32 k_residents = 200;
constexpr u64 k_ticks = 10'000;
// Monday 06:00: the morning's commutes fall inside the run.
constexpr i64 k_morning = 6 * 60 * k_us_per_minute;
// The door's run writes back every 97 ticks: a resident's transitions fall on the game minute, and
// a cadence the minute divides would catch every one itself, so a call's flush would never write
// anything first. At 97 a call that ends off the cadence writes transitions the cadence has not.
constexpr u32 k_cadence = 97;

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

  // The door's run (roadmap S18), when asked for: the observer is the player, steered by a stick
  // through the door and moved by a system in the Input phase; the residents' state is written
  // back to the document every k_cadence ticks and at the end of every call, the call's flush
  // recorded through the door; the hash log carries the door and the write-backs as two more
  // tables.
  bool with_door = false;
  sim::Door door;
  input::ActionMap map;
  input::ActionId move_x = input::k_invalid_action;
  input::ActionId move_z = input::k_invalid_action;
  input::InputState state;
  f32 sent[2] = {0.0f, 0.0f};
  u64 writeback_hash = 0;
  u64 commits = 0;

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

  Run(const sim::RunRecord& record, sim::HashLog& log, bool door_run = false)
      : sim(world_config(record.hz)),
        scheduler(scheduler_config(record)),
        records(sim.world()),
        npc(npc_config(record.seed)),
        driver(scheduler, driver_config()),
        with_door(door_run) {
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

    if (with_door) {
      // First, so its Input hook feeds the stick before the player's system reads it.
      door.install(scheduler);
      move_x = map.add_action("move_x", input::ActionKind::Axis);
      move_z = map.add_action("move_z", input::ActionKind::Axis);
      map.bind(move_x, input::Binding{input::Source::GamepadAxis, 0u});
      map.bind(move_z, input::Binding{input::Source::GamepadAxis, 1u});
      state.set_map(map);
      door.set_input_state(&state);
      door.on(sim::door_flush_kind(), sim::DoorHandler{this, &Run::replay_flush});
      sim::SystemDesc player;
      player.name = "test.player";
      player.phase = sim::TickPhase::Input;
      player.context = this;
      player.begin_tick = &Run::steer;
      scheduler.add_system(player);
    }
    npc.install(sim, scheduler);
    scheduler.add_hooks(records.hooks());
    scheduler.add_hooks(npc.hooks());
    driver.set_target(records.target());
    tick = std::make_unique<ecs::ScheduledTick>(sim, scheduler);
    observers.add(WorldPos{-96.0, 0.0, 0.0}, 1.0f);
    npc.set_observers(&observers);
    npc.places().refresh(document);
    driver.materialize(document);
    if (with_door) {
      driver.set_writeback_sink(sim::WriteBackSink{this, &Run::commit});
      driver.set_writeback_every(k_cadence);
      driver.install_writeback();
    }
    // Last, after every system it hashes the work of.
    ecs::TickHashConfig config;
    config.reserve_ticks = record.ticks;
    hash = std::make_unique<ecs::TickHash>(sim, scheduler, log, config);
    if (with_door) {
      hash->add_source(ecs::TickHashSource{"door", this, [](void* c, sim::TickPhase) {
                                             return static_cast<Run*>(c)->door.delivered_hash();
                                           }});
      hash->add_source(ecs::TickHashSource{"writeback", this, [](void* c, sim::TickPhase) {
                                             return static_cast<Run*>(c)->writeback_hash;
                                           }});
    }
  }

  // The player's system: the observer walks east and the stick steers it, at the tick.
  static void steer(sim::SystemContext& context) {
    auto* self = static_cast<Run*>(context.context);
    const WorldPos at = self->observers.position(0);
    const f64 dx = 0.02 + 0.5 * static_cast<f64>(self->state.axis(self->move_x));
    const f64 dz = 0.5 * static_cast<f64>(self->state.axis(self->move_z));
    self->observers.clear();
    self->observers.add(WorldPos{at.x + dx, at.y, at.z + dz}, 1.0f);
  }

  // A write-back committed the way a session would, and hashed: the tick and every change.
  static bool commit(void* context, const sim::WriteBackBatch& batch) {
    auto* self = static_cast<Run*>(context);
    u64 h = self->writeback_hash ^ (batch.tick.value * 0x9E3779B97F4A7C15ull);
    for (const sim::WriteBackChange& change : batch.changes) {
      const std::string value = write_json(change.value);
      h = (h ^ change.record.hi) * 0x100000001B3ull;
      h = (h ^ change.record.lo) * 0x100000001B3ull;
      h = (h ^ change.row) * 0x100000001B3ull;
      for (const char c : value)
        h = (h ^ static_cast<u8>(c)) * 0x100000001B3ull;
    }
    self->writeback_hash = h;
    ++self->commits;
    doc::Transaction transaction = self->document.begin(batch.attribution);
    for (const doc::Command& command : batch.commands) {
      if (!transaction.apply(command)) return false;
    }
    return transaction.commit();
  }

  // A call's end, live: the write-back's flush and its record. A replay does neither (the door
  // flushes where the record says).
  void end_call() {
    if (door.replaying()) return;
    driver.flush_writeback(scheduler.tick(), scheduler.game_time());
    door.record_between(sim::DoorOrigin::Agent, sim::door_flush_kind());
  }

  static void replay_flush(void* context, const sim::DoorEntry& entry, std::span<const u8>) {
    auto* self = static_cast<Run*>(context);
    self->driver.flush_writeback(SimTick{entry.tick - 1}, GameTime{entry.game_time_us});
  }

  // A frame at real time `now`: the host reads its stick and hands what changed to the door.
  void frame_input(i64 now_ns) {
    const i64 s = now_ns / 1'000'000'000;
    const f32 v[2] = {0.25f * static_cast<f32>((s / 37) % 5) - 0.5f,
                      0.25f * static_cast<f32>((s / 53) % 5) - 0.5f};
    for (u32 a = 0; a < 2; ++a) {
      if (v[a] == sent[a]) continue;
      input::RawEvent e;
      e.source = input::Source::GamepadAxis;
      e.code = a;
      e.value = v[a];
      if (door.submit_input(e)) sent[a] = v[a];
    }
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

namespace {

u64 lcg(u64& s) {
  s = s * 6364136223846793005ull + 1442695040888963407ull;
  return s >> 33;
}

struct Cadence {
  u32 stalls = 0;
  u32 calls = 0;
  u32 off_cadence = 0;  // calls that ended on a tick the write-back's cadence does not
};

// The recorded host: frames of 0.3 s (three or four to a game second), 1 s, 2.5 s (two or three
// ticks) and 20 s stalls of which the fixed step runs eight ticks and drops the rest, at 1 Hz;
// calls of 1 to 40 frames, each ending with a flush.
Cadence record_frames(Run& run, u64 ticks, u64 seed) {
  Cadence c;
  i64 now = 0;
  u64 s = seed;
  const u64 end = run.scheduler.tick().value + ticks;
  while (run.scheduler.tick().value < end) {
    const u64 frames = 1 + lcg(s) % 40;
    for (u64 f = 0; f < frames && run.scheduler.tick().value < end; ++f) {
      const u64 r = lcg(s) % 100;
      const i64 dt = r < 30   ? 300'000'000
                     : r < 70 ? 1'000'000'000
                     : r < 95 ? 2'500'000'000
                              : 20'000'000'000;
      c.stalls += dt == 20'000'000'000 ? 1u : 0u;
      now += dt;
      run.frame_input(now);
      if (end - run.scheduler.tick().value < 8) {
        run.tick->step();
      } else {
        run.tick->advance(dt);
      }
    }
    run.end_call();
    ++c.calls;
    c.off_cadence += run.scheduler.tick().value % k_cadence != 0 ? 1u : 0u;
  }
  return c;
}

}  // namespace

// Roadmap S18 at scale: the same residents and walking observer, with the observer now the player,
// steered by a stick through the door, and the residents written back to the document at a game
// minute's cadence and at the end of every call — calls that end off the cadence almost always.
// The record is the door alone (with the seed and the clock); it replays headless at the fixed step
// in one call, and under other frames and calls that flush nothing of their own, both to no
// divergence in any table, the door's and the write-backs' included.
TEST_CASE("npc: 10^4 ticks with a player's input and calls off the cadence replay from the door") {
  test::TempDir dir;
  sim::RunRecord record;
  record.seed = 42;
  record.hz = k_hz;
  record.start_time_us = k_morning;
  record.ticks = k_ticks;
  record.hash_log = "residents.hashlog";
  record.door_log = "inputs.door";

  sim::HashLog recorded;
  u64 recorded_writeback = 0;
  {
    Run run(record, recorded, true);
    const Cadence c = record_frames(run, k_ticks, 2026);
    REQUIRE(run.scheduler.tick().value == k_ticks);
    recorded_writeback = run.writeback_hash;
    MESSAGE("calls " << c.calls << " (" << c.off_cadence << " off the cadence), stalls " << c.stalls
                     << ", door entries " << run.door.entries().size() << " ("
                     << run.door.memory_bytes() << " bytes), commits " << run.commits
                     << ", transitions " << run.npc.stats().transitions);
    CHECK(c.stalls > 0);
    CHECK(c.off_cadence > c.calls / 2);
    CHECK(run.door.stats().delivered > 100);
    CHECK(run.commits > 0);
    CHECK(run.npc.stats().transitions > k_residents);
    REQUIRE(run.door.save(sim::run_file(dir.path(), record.door_log)));
  }
  REQUIRE(recorded.save(sim::run_file(dir.path(), record.hash_log)));
  REQUIRE(sim::write_run_record(dir.path(), record));

  sim::RunRecord read;
  std::string error;
  REQUIRE_MESSAGE(sim::read_run_record(dir.path(), read, &error), error);
  REQUIRE(read.door_log == record.door_log);
  sim::HashLog expected;
  REQUIRE_MESSAGE(expected.load(sim::run_file(dir.path(), read.hash_log), &error), error);
  REQUIRE(expected.table_index("door") != ~0u);
  REQUIRE(expected.table_index("writeback") != ~0u);

  // Without the calls' flushes — the door's handler for them taken away, so the replay writes back
  // on the cadence alone, which is the unsplit run — the residents' state is the same and the
  // commits are not: the split cadence, found at the first call that ended off it.
  {
    sim::HashLog replayed;
    Run run(read, replayed, true);
    REQUIRE(run.door.load(sim::run_file(dir.path(), read.door_log), &error));
    run.door.on(sim::door_flush_kind(), sim::DoorHandler{});
    run.door.begin_replay();
    for (u64 i = 0; i < read.ticks; ++i)
      run.tick->step();
    const sim::Divergence d = sim::first_divergence(expected, replayed);
    REQUIRE(d.found);
    MESSAGE("without the calls' flushes: parts at tick " << d.tick << ", phase "
                                                         << static_cast<u32>(d.phase));
    REQUIRE(d.tables.size() == 1);
    CHECK(d.tables[0].table == "writeback");
  }

  for (int how = 0; how < 2; ++how) {
    sim::HashLog replayed;
    Run run(read, replayed, true);
    REQUIRE_MESSAGE(run.door.load(sim::run_file(dir.path(), read.door_log), &error), error);
    run.door.begin_replay();
    if (how == 0) {
      for (u64 i = 0; i < read.ticks; ++i)
        run.tick->step();  // headless, one call: the player's system moves the observer
    } else {
      record_frames(run, read.ticks, 7);  // other frames and calls; their input is refused
      CHECK(run.door.stats().refused_live > 0);
    }
    run.door.replay_pending();
    const sim::Divergence d = sim::first_divergence(expected, replayed);
    CHECK_FALSE(d.found);
    CHECK_FALSE(d.rows_differ);
    CHECK(d.rows_compared == k_ticks * sim::k_phase_count);
    // The flush after the recorded run's last tick is in no row; the door applied it here.
    CHECK(run.writeback_hash == recorded_writeback);
  }
}
