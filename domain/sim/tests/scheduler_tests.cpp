// The tick scheduler (docs/plan/05-simulation.md §5.2), the materialization contract
// (03 §3.4) and tile reconciliation (§5.5), against a fake store and fake systems.

#include <core/containers/vector.h>
#include <core/jobs/job_system.h>
#include <domain/sim/scheduler.h>

#include <doctest/doctest.h>

#include <cstring>
#include <mutex>
#include <string>

using namespace engine;
using namespace engine::sim;

namespace {

// Component ids. In a real build these come from the schema registry; here they are just
// numbers, which is the point of the mask being opaque to the scheduler.
constexpr u32 k_position = 0;
constexpr u32 k_velocity = 1;
constexpr u32 k_health = 2;

ComponentMask mask_of(u32 a) {
  ComponentMask mask;
  mask.set(a);
  return mask;
}

// Every system appends its name, so the assertions are about order and not about arithmetic.
struct Trace {
  std::mutex mutex;
  Vector<u16> order;
  Vector<u8> phases;
  Vector<u64> ticks;
  u32 begins = 0;
  u32 ends = 0;
};

struct SystemStub {
  Trace* trace = nullptr;
  u32 batches_seen = 0;

  // A wave's invocations genuinely run in parallel, so the bookkeeping takes the trace's lock.
  // The lock is the test harness's, not the scheduler's: what is being checked is that the
  // *result* does not depend on the threading, not that unsynchronized writes happen to work.
  static void tick(SystemContext& context, Batch batch) {
    auto* self = static_cast<SystemStub*>(context.context);
    (void)batch;
    std::lock_guard lock(self->trace->mutex);
    self->trace->order.push_back(context.system);
    self->trace->phases.push_back(static_cast<u8>(context.phase));
    self->trace->ticks.push_back(context.tick.value);
    ++self->batches_seen;
  }
  static void begin(SystemContext& context) {
    auto* self = static_cast<SystemStub*>(context.context);
    std::lock_guard lock(self->trace->mutex);
    ++self->trace->begins;
  }
  static void end(SystemContext& context) {
    auto* self = static_cast<SystemStub*>(context.context);
    std::lock_guard lock(self->trace->mutex);
    ++self->trace->ends;
  }
};

SystemDesc describe(const char* name, TickPhase phase, ComponentMask reads, ComponentMask writes,
                    SystemStub& stub) {
  SystemDesc desc;
  desc.name = name;
  desc.phase = phase;
  desc.reads = reads;
  desc.writes = writes;
  desc.context = &stub;
  desc.tick = &SystemStub::tick;
  return desc;
}

// ---- the fake store ---------------------------------------------------------------------------

struct FakeStore {
  GameTime last_active;
  u64 seed = 0;
  bool known = true;
  Vector<EntityRecord> records;
  u32 state_calls = 0;
  u32 load_calls = 0;

  static bool tile_state(void* context, u64 tile, TileState& out) {
    auto* self = static_cast<FakeStore*>(context);
    ++self->state_calls;
    (void)tile;
    if (!self->known) return false;
    out.last_active = self->last_active;
    out.seed = self->seed;
    return true;
  }
  static void load_records(void* context, u64 tile, Vector<EntityRecord>& out) {
    auto* self = static_cast<FakeStore*>(context);
    ++self->load_calls;
    (void)tile;
    for (u32 i = 0; i < self->records.size(); ++i)
      out.push_back(self->records[i]);
  }

  TileStore interface_for() {
    TileStore store;
    store.context = this;
    store.tile_state = &FakeStore::tile_state;
    store.load_records = &FakeStore::load_records;
    return store;
  }
};

// ---- the fake system that implements the whole contract ---------------------------------------

struct FakeSystem {
  Vector<u64> materialized;
  Vector<u64> promoted;
  Vector<u64> demoted;
  Vector<u64> dropped;
  u64 summarized_from = 0;
  u64 summarized_to = 0;
  u64 summarized_seed = 0;
  u64 summarized_tile = 0;
  u32 summarize_calls = 0;
  // The summary is a deterministic function of the seed and the gap, and of nothing else.
  u64 drift = 0;

  static void materialize(void* context, const EntityRecord& record, u8 tier) {
    auto* self = static_cast<FakeSystem*>(context);
    (void)tier;
    self->materialized.push_back(record.entity);
  }
  static void promote(void* context, u64 entity, u8 from, u8 to) {
    auto* self = static_cast<FakeSystem*>(context);
    (void)from;
    (void)to;
    self->promoted.push_back(entity);
  }
  static void demote(void* context, u64 entity, u8 from, u8 to) {
    auto* self = static_cast<FakeSystem*>(context);
    (void)from;
    (void)to;
    self->demoted.push_back(entity);
  }
  static void dematerialize(void* context, u64 entity) {
    static_cast<FakeSystem*>(context)->dropped.push_back(entity);
  }
  static void summarize(void* context, const SummarizeInterval& interval) {
    auto* self = static_cast<FakeSystem*>(context);
    ++self->summarize_calls;
    self->summarized_from = static_cast<u64>(interval.from.us);
    self->summarized_to = static_cast<u64>(interval.to.us);
    self->summarized_seed = interval.seed;
    self->summarized_tile = interval.tile;
    const u64 days =
        static_cast<u64>((interval.to.us - interval.from.us) / GameTime::from_days(1).us);
    self->drift += days * (interval.seed % 97u + 1u);
  }

  MaterializationHooks hooks_for() {
    MaterializationHooks hooks;
    hooks.name = "fake";
    hooks.context = this;
    hooks.materialize = &FakeSystem::materialize;
    hooks.promote = &FakeSystem::promote;
    hooks.demote = &FakeSystem::demote;
    hooks.dematerialize = &FakeSystem::dematerialize;
    return hooks;
  }
  Summarizer summarizer_for(u16 system) {
    Summarizer summarizer;
    summarizer.name = "fake";
    summarizer.fn = &FakeSystem::summarize;
    summarizer.context = this;
    summarizer.system = system;
    summarizer.tiers = 0x08u;  // LOD3 only
    return summarizer;
  }
};

}  // namespace

TEST_CASE("scheduler: the eight phases run in the order of plan 05 section 5.2") {
  SimScheduler scheduler;
  Trace trace;
  SystemStub stubs[k_phase_count];
  // Registered back to front, so the order that comes out is the phases' and not the
  // registration's.
  for (u32 i = k_phase_count; i > 0; --i) {
    const auto phase = static_cast<TickPhase>(i - 1);
    stubs[i - 1].trace = &trace;
    scheduler.add_system(
        describe(phase_name(phase), phase, ComponentMask{}, ComponentMask{}, stubs[i - 1]));
  }
  scheduler.step();
  REQUIRE(trace.phases.size() == k_phase_count);
  for (u32 i = 0; i < k_phase_count; ++i)
    CHECK(static_cast<u32>(trace.phases[i]) == i);
  for (u32 i = 0; i < k_phase_count; ++i)
    CHECK(trace.ticks[i] == 1u);

  CHECK(std::string(phase_name(TickPhase::Input)) == "input");
  CHECK(std::string(phase_name(TickPhase::Persist)) == "persist");
  CHECK(std::string(phase_name(TickPhase::Lod)) == "lod");
}

TEST_CASE("scheduler: conflicting systems keep declaration order, the rest share a wave") {
  SimScheduler scheduler;
  Trace trace;
  SystemStub a;
  SystemStub b;
  SystemStub c;
  SystemStub d;
  a.trace = &trace;
  b.trace = &trace;
  c.trace = &trace;
  d.trace = &trace;

  // a writes position; b reads position (conflict with a); c writes health (no conflict with
  // either); d writes position (conflict with a and b).
  const u16 ia = scheduler.add_system(
      describe("a", TickPhase::Systems, ComponentMask{}, mask_of(k_position), a));
  const u16 ib = scheduler.add_system(
      describe("b", TickPhase::Systems, mask_of(k_position), ComponentMask{}, b));
  const u16 ic = scheduler.add_system(
      describe("c", TickPhase::Systems, mask_of(k_velocity), mask_of(k_health), c));
  const u16 id = scheduler.add_system(
      describe("d", TickPhase::Systems, ComponentMask{}, mask_of(k_position), d));

  CHECK(scheduler.wave_of(ia) == 0);
  CHECK(scheduler.wave_of(ib) == 1);
  CHECK(scheduler.wave_of(ic) == 0);  // conflicts with nobody, so it goes in the first wave
  CHECK(scheduler.wave_of(id) == 2);
  CHECK(scheduler.wave_count(TickPhase::Systems) == 3);
  CHECK(scheduler.wave_count(TickPhase::Physics) == 0);

  const std::span<const ScheduleEntry> plan = scheduler.schedule(TickPhase::Systems);
  REQUIRE(plan.size() == 4);
  CHECK(plan[0].wave == 0);
  CHECK(plan[1].wave == 0);
  CHECK(plan[2].wave == 1);
  CHECK(plan[3].wave == 2);

  scheduler.step();
  REQUIRE(trace.order.size() == 4);
  // a and c may come out in either order relative to each other on a threaded run; what the
  // plan promises is that a precedes b and b precedes d.
  u32 position_of[4] = {0, 0, 0, 0};
  for (u32 i = 0; i < trace.order.size(); ++i)
    position_of[trace.order[i]] = i;
  CHECK(position_of[ia] < position_of[ib]);
  CHECK(position_of[ib] < position_of[id]);
}

TEST_CASE("scheduler: the schedule is a function of the registration and nothing else") {
  auto build = [](SimScheduler& scheduler, SystemStub* stubs) {
    scheduler.add_system(
        describe("a", TickPhase::Systems, ComponentMask{}, mask_of(k_position), stubs[0]));
    scheduler.add_system(
        describe("b", TickPhase::Lod, mask_of(k_position), mask_of(k_health), stubs[1]));
    scheduler.add_system(
        describe("c", TickPhase::Systems, mask_of(k_position), ComponentMask{}, stubs[2]));
  };

  SystemStub first_stubs[3];
  SystemStub second_stubs[3];
  SimScheduler first;
  SimScheduler second;
  build(first, first_stubs);
  build(second, second_stubs);
  CHECK(first.schedule_hash() == second.schedule_hash());

  // A different registration order is a different schedule, and says so.
  SystemStub third_stubs[3];
  SimScheduler third;
  third.add_system(
      describe("c", TickPhase::Systems, mask_of(k_position), ComponentMask{}, third_stubs[2]));
  third.add_system(
      describe("a", TickPhase::Systems, ComponentMask{}, mask_of(k_position), third_stubs[0]));
  third.add_system(
      describe("b", TickPhase::Lod, mask_of(k_position), mask_of(k_health), third_stubs[1]));
  CHECK(first.schedule_hash() != third.schedule_hash());
}

TEST_CASE("scheduler: eight workers produce the same tick as one and as none") {
  auto run = [](jobs::JobSystem* system, Vector<u64>& out) {
    SimSchedulerConfig config;
    config.job_system = system;
    SimScheduler scheduler(config);
    Trace trace;
    // Sixteen systems that do not conflict, each split into four batches: the widest wave the
    // scheduler will ever build in this test is 64 parallel invocations.
    Vector<SystemStub> stubs;
    stubs.resize(16);
    Vector<std::string> names;
    names.resize(16);
    for (u32 i = 0; i < 16; ++i) {
      stubs[i].trace = &trace;
      names[i] = std::string("system_") + std::to_string(i);
      SystemDesc desc = describe(names[i].c_str(), TickPhase::Systems, ComponentMask{},
                                 ComponentMask{}, stubs[i]);
      desc.batches = 4;
      scheduler.add_system(desc);
    }
    for (u32 tick = 0; tick < 8; ++tick)
      scheduler.step();
    out.clear();
    for (u32 i = 0; i < 16; ++i)
      out.push_back(stubs[i].batches_seen);
    out.push_back(static_cast<u64>(trace.order.size()));
    out.push_back(scheduler.tick().value);
    out.push_back(static_cast<u64>(scheduler.game_time().us));
    out.push_back(scheduler.schedule_hash());
  };

  Vector<u64> serial;
  run(nullptr, serial);

  jobs::JobSystemConfig config;
  config.performance_workers = 1;
  config.efficiency_workers = 1;
  jobs::JobSystem one(config);
  Vector<u64> with_one;
  run(&one, with_one);

  config.performance_workers = 8;
  jobs::JobSystem eight(config);
  Vector<u64> with_eight;
  run(&eight, with_eight);

  REQUIRE(serial.size() == with_one.size());
  REQUIRE(serial.size() == with_eight.size());
  const usize bytes = static_cast<usize>(serial.size()) * sizeof(u64);
  CHECK(std::memcmp(serial.data(), with_one.data(), bytes) == 0);
  CHECK(std::memcmp(serial.data(), with_eight.data(), bytes) == 0);
  CHECK(serial[0] == 32);    // 8 ticks of 4 batches
  CHECK(serial[16] == 512);  // 16 systems x 4 batches x 8 ticks
}

TEST_CASE("scheduler: begin_tick and end_tick run once a tick, around the parallel waves") {
  SimScheduler scheduler;
  Trace trace;
  SystemStub stub;
  stub.trace = &trace;
  SystemDesc desc =
      describe("bracketed", TickPhase::Systems, ComponentMask{}, ComponentMask{}, stub);
  desc.batches = 8;
  desc.begin_tick = &SystemStub::begin;
  desc.end_tick = &SystemStub::end;
  scheduler.add_system(desc);

  scheduler.step();
  scheduler.step();
  CHECK(trace.begins == 2);
  CHECK(trace.ends == 2);
  CHECK(stub.batches_seen == 16);
}

TEST_CASE("scheduler: the fixed step drives game time and the timing wheel") {
  SimSchedulerConfig config;
  config.hz = 60;
  config.game_seconds_per_real_second = 60.0;  // one game minute a real second
  SimScheduler scheduler(config);

  Vector<i64> fired;
  auto body = [&](const TimerEvent& event) { fired.push_back(event.at.us); };
  EventSink sink = make_sink(body);
  scheduler.set_event_sink(sink);
  scheduler.wheel().schedule_periodic(0, GameTime::from_seconds(10), GameTime{0});

  // One game minute: six firings of a ten-game-second periodic.
  for (u32 i = 0; i < 60; ++i)
    scheduler.step();
  CHECK(scheduler.tick().value == 60);
  CHECK(scheduler.game_time().whole_seconds() == 60);  // 60 ticks of one game second
  CHECK(fired.size() == 6);

  // advance() is the same machinery fed by real time instead of by a caller counting ticks.
  const u32 steps = scheduler.advance(1'000'000'000);  // one real second
  CHECK(steps == 8);                                   // capped by max_steps_per_advance
  CHECK(scheduler.tick().value == 68);
}

TEST_CASE("scheduler: the persistence hook closes the tick") {
  struct Flush {
    u32 calls = 0;
    u64 last_tick = 0;
    static void run(void* context, SimTick at_tick, GameTime at_time) {
      auto* self = static_cast<Flush*>(context);
      (void)at_time;
      ++self->calls;
      self->last_tick = at_tick.value;
    }
  };
  Flush flush;
  SimScheduler scheduler;
  scheduler.set_persist_hook(&Flush::run, &flush);
  scheduler.step();
  scheduler.step();
  CHECK(flush.calls == 2);
  CHECK(flush.last_tick == 2);
}

TEST_CASE("scheduler: tier changes drive the materialization hooks") {
  SimScheduler scheduler;
  FakeSystem system;
  scheduler.add_hooks(system.hooks_for());

  Vector<u64> entities;
  entities.push_back(100);
  entities.push_back(200);
  entities.push_back(300);
  Vector<TierChange> changes;
  changes.push_back(TierChange{0, 2, 1});  // promotion
  changes.push_back(TierChange{1, 1, 3});  // demotion
  changes.push_back(TierChange{2, 2, 2});  // no transition at all

  scheduler.apply_tier_changes(std::span<const TierChange>(changes.data(), changes.size()),
                               std::span<const u64>(entities.data(), entities.size()));
  REQUIRE(system.promoted.size() == 1);
  CHECK(system.promoted[0] == 100);
  REQUIRE(system.demoted.size() == 1);
  CHECK(system.demoted[0] == 200);

  scheduler.dematerialize(std::span<const u64>(entities.data(), entities.size()));
  CHECK(system.dropped.size() == 3);
}

TEST_CASE("scheduler: a hook only sees the tiers it declared") {
  SimScheduler scheduler;
  FakeSystem near_only;
  MaterializationHooks hooks = near_only.hooks_for();
  hooks.tiers = 0x03u;  // LOD0 and LOD1 only
  scheduler.add_hooks(hooks);

  Vector<u64> entities;
  entities.push_back(7);
  entities.push_back(8);
  Vector<TierChange> changes;
  changes.push_back(TierChange{0, 3, 2});  // entirely outside the hook's tiers
  changes.push_back(TierChange{1, 2, 1});  // ends inside them
  scheduler.apply_tier_changes(std::span<const TierChange>(changes.data(), changes.size()),
                               std::span<const u64>(entities.data(), entities.size()));
  CHECK(near_only.demoted.empty());
  REQUIRE(near_only.promoted.size() == 1);
  CHECK(near_only.promoted[0] == 8);
}

TEST_CASE("scheduler: reconcile_tile runs the five steps of plan 05 section 5.5") {
  SimScheduler scheduler;
  FakeSystem system;
  scheduler.add_hooks(system.hooks_for());
  scheduler.wheel().add_summarizer(system.summarizer_for(0));

  FakeStore store;
  store.seed = 0x5EED;
  store.last_active = GameTime::from_days(10);
  for (u32 i = 0; i < 8; ++i) {
    EntityRecord record;
    record.entity = 1000 + i;
    record.seed = store.seed ^ i;
    // The first two are inside the observer's LOD0 radius; the rest are far away.
    record.position = Vec3{i < 2 ? 5.0f : 4000.0f, 0.0f, 0.0f};
    record.importance = 1.0f;
    record.tier = 3;
    store.records.push_back(record);
  }

  ObserverSet observers;
  observers.add(Vec3{0.0f, 0.0f, 0.0f}, 1.0f);
  TierParams tier_params;
  tier_params.max_promotions = 1000;
  tier_params.max_demotions = 1000;

  ReconcileParams params;
  params.tile = 77;
  params.now = GameTime::from_days(40);

  const ReconcileResult result =
      scheduler.reconcile_tile(params, store.interface_for(), observers, tier_params);

  CHECK(result.known);
  CHECK(result.gap.us == GameTime::from_days(30).us);  // step 2
  CHECK(result.summarized == 1);                       // step 3
  CHECK(system.summarize_calls == 1);
  CHECK(system.summarized_seed == 0x5EED);  // from the tile's seed, as the plan says
  CHECK(system.summarized_tile == 77);
  CHECK(system.summarized_from == static_cast<u64>(GameTime::from_days(10).us));
  CHECK(system.summarized_to == static_cast<u64>(GameTime::from_days(40).us));
  CHECK(result.records == 8);
  CHECK(result.materialized == 8);  // step 4, all at LOD2
  CHECK(system.materialized.size() == 8);
  CHECK(result.promoted == 2);  // step 5, by observer distance
  REQUIRE(system.promoted.size() == 2);
  CHECK(system.promoted[0] == 1000);
  CHECK(system.promoted[1] == 1001);
  CHECK(system.demoted.size() == 6);  // the rest fall to LOD3
}

TEST_CASE("scheduler: reconciling the same tile twice is the same summary") {
  auto run = [](u64 seed, i64 gap_days) {
    SimScheduler scheduler;
    FakeSystem system;
    scheduler.wheel().add_summarizer(system.summarizer_for(0));
    FakeStore store;
    store.seed = seed;
    store.last_active = GameTime{0};
    ObserverSet observers;
    observers.add(Vec3{0.0f, 0.0f, 0.0f}, 1.0f);
    ReconcileParams params;
    params.tile = 3;
    params.now = GameTime::from_days(gap_days);
    scheduler.reconcile_tile(params, store.interface_for(), observers, TierParams{});
    return system.drift;
  };
  CHECK(run(1234, 30) == run(1234, 30));
  CHECK(run(1234, 30) != run(1235, 30));
}

TEST_CASE("scheduler: an unknown tile is materialized without a summary") {
  SimScheduler scheduler;
  FakeSystem system;
  scheduler.add_hooks(system.hooks_for());
  scheduler.wheel().add_summarizer(system.summarizer_for(0));

  FakeStore store;
  store.known = false;
  EntityRecord record;
  record.entity = 5;
  record.position = Vec3{1.0f, 0.0f, 0.0f};
  store.records.push_back(record);

  ObserverSet observers;
  observers.add(Vec3{0.0f, 0.0f, 0.0f}, 1.0f);
  ReconcileParams params;
  params.now = GameTime::from_days(3);
  const ReconcileResult result =
      scheduler.reconcile_tile(params, store.interface_for(), observers, TierParams{});
  CHECK_FALSE(result.known);
  CHECK(result.gap.us == 0);
  CHECK(result.summarized == 0);
  CHECK(system.summarize_calls == 0);
  CHECK(result.materialized == 1);
}

TEST_CASE("scheduler: the component mask is 256 wide and says so") {
  CHECK(k_max_components == 256);
  ComponentMask mask;
  CHECK_FALSE(mask.any());
  mask.set(0);
  mask.set(255);
  CHECK(mask.test(0));
  CHECK(mask.test(255));
  CHECK_FALSE(mask.test(1));
  CHECK(mask.any());

  ComponentMask other;
  other.set(128);
  CHECK_FALSE(mask.intersects(other));
  other.set(255);
  CHECK(mask.intersects(other));
  CHECK(mask == mask);
  CHECK_FALSE(mask == other);
}
