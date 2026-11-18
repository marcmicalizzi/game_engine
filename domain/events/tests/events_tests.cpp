// The world event bus (docs/subsystems/events.md): the delivery order, the scheduler's phases, the
// document's change feed and the replay of external raises.

#include <core/containers/vector.h>
#include <core/jobs/job_system.h>
#include <core/schema/materialize.h>
#include <domain/doc/document.h>
#include <domain/events/events.h>
#include <domain/sim/scheduler.h>

#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <string>
#include <schemas/events.h>
#include <schemas/events_test.h>
#include <utility>

using namespace engine;
using namespace engine::events;
namespace test = engine::events_test;

namespace {

struct Heard {
  u64 tick = 0;
  u64 id = 0;
  u32 n = 0;
  u32 source = 0;
  u8 phase = 0;
  u8 depth = 0;
  u16 pad = 0;
  u32 pad2 = 0;
  bool operator==(const Heard&) const = default;
};

struct Listener {
  EventType<test::Blast> blast;
  Vector<Heard> heard;
  static void handle(void* context, const Event& event, Emitter&) {
    auto* self = static_cast<Listener*>(context);
    Heard h;
    h.tick = event.record->tick;
    h.id = event.record->id.value;
    h.n = event.as(self->blast).n;
    h.source = event.record->source;
    h.phase = event.record->phase;
    h.depth = event.record->depth;
    self->heard.push_back(h);
  }
};

// A table system that raises from each of its batches: batch b raises `per_batch` blasts whose
// count depends on the tick, so the batches finish at different times on different runs.
struct Raiser {
  Raiser(EventBus* b, EventType<test::Blast> t, u32 n) : bus(b), blast(t), per_batch(n) {}
  EventBus* bus = nullptr;
  EventType<test::Blast> blast;
  u32 per_batch = 0;
  std::atomic<u32> refused{0};
  static void tick(sim::SystemContext& context, sim::Batch batch) {
    auto* self = static_cast<Raiser*>(context.context);
    Emitter emit = self->bus->emitter(context, batch);
    const u32 count =
        self->per_batch + ((static_cast<u32>(context.tick.value) * 7u + batch.begin) % 5u);
    for (u32 i = 0; i < count; ++i) {
      test::Blast blast;
      blast.n = batch.begin * 1000u + i;
      blast.strength = static_cast<f32>(i);
      // Not a doctest CHECK: the batches run on the job system's threads.
      if (!emit.raise(self->blast, blast)) ++self->refused;
    }
  }
};

struct WaveRun {
  Vector<Heard> heard;
  u64 hash = 0;
  u32 sorted = 0;
};

WaveRun run_wave(jobs::JobSystem* jobs, u32 ticks) {
  sim::SimSchedulerConfig config;
  config.job_system = jobs;
  sim::SimScheduler scheduler(config);
  EventBus bus;
  Listener listener;
  listener.blast = bus.register_type<test::Blast>();
  const u16 types[] = {listener.blast.index};
  SubscriberDesc desc;
  desc.name = "listener";
  desc.context = &listener;
  desc.types = types;
  desc.handle = &Listener::handle;
  bus.subscribe(desc);

  Raiser raiser{&bus, listener.blast, 40};
  sim::SystemDesc system;
  system.name = "raiser";
  system.phase = sim::TickPhase::Systems;
  system.batches = 8;
  system.context = &raiser;
  system.tick = &Raiser::tick;
  scheduler.add_system(system);
  bus.install(scheduler);

  WaveRun run;
  for (u32 t = 0; t < ticks; ++t) {
    scheduler.step();
    run.sorted += bus.current_report().sorted;
  }
  CHECK(raiser.refused.load() == 0u);
  run.heard = static_cast<Vector<Heard>&&>(listener.heard);
  run.hash = bus.delivery_hash();
  return run;
}

}  // namespace

TEST_CASE("events: a parallel wave's raises are delivered in key order at every thread count") {
  const WaveRun serial = run_wave(nullptr, 6);
  jobs::JobSystemConfig config;
  config.performance_workers = 1;
  config.efficiency_workers = 1;
  jobs::JobSystem one(config);
  const WaveRun with_one = run_wave(&one, 6);
  config.performance_workers = 8;
  jobs::JobSystem eight(config);
  const WaveRun with_eight = run_wave(&eight, 6);

  REQUIRE(serial.heard.size() > 6u * 8u * 40u);
  REQUIRE(serial.heard.size() == with_one.heard.size());
  REQUIRE(serial.heard.size() == with_eight.heard.size());
  bool same = true;
  for (u32 i = 0; i < serial.heard.size(); ++i) {
    same = same && serial.heard[i] == with_one.heard[i] && serial.heard[i] == with_eight.heard[i];
  }
  CHECK(same);
  CHECK(serial.hash == with_one.hash);
  CHECK(serial.hash == with_eight.hash);
  // The serial run raised in order and needed no sort, whatever the threaded runs needed.
  CHECK(serial.sorted == 0u);

  // The order itself: by tick, then by batch (the system's key), then by each batch's own sequence.
  for (u32 i = 1; i < serial.heard.size(); ++i) {
    const Heard& a = serial.heard[i - 1u];
    const Heard& b = serial.heard[i];
    CHECK(a.tick <= b.tick);
    if (a.tick == b.tick) {
      CHECK(a.n < b.n);
      CHECK(a.id < b.id);
    }
  }
  // Delivered at the same tick's EventsOut: raised in Systems, heard before the tick ended.
  CHECK(serial.heard.front().tick == 1u);
  CHECK(serial.heard.front().phase == static_cast<u8>(sim::TickPhase::Systems));
}

TEST_CASE("events: order is tick, then phase, then source, never registration or arrival") {
  sim::SimScheduler scheduler;
  EventBus bus;
  Listener listener;
  listener.blast = bus.register_type<test::Blast>();
  const u16 types[] = {listener.blast.index};
  SubscriberDesc desc;
  desc.name = "listener";
  desc.context = &listener;
  desc.types = types;
  desc.handle = &Listener::handle;
  bus.subscribe(desc);

  // Registered Systems first, Lod second: the Lod system's events still come first.
  Raiser late{&bus, listener.blast, 1};
  Raiser early{&bus, listener.blast, 1};
  sim::SystemDesc systems;
  systems.name = "in_systems";
  systems.phase = sim::TickPhase::Systems;
  systems.context = &late;
  systems.tick = &Raiser::tick;
  scheduler.add_system(systems);
  sim::SystemDesc lod = systems;
  lod.name = "in_lod";
  lod.phase = sim::TickPhase::Lod;
  lod.context = &early;
  scheduler.add_system(lod);
  bus.install(scheduler);

  // A host source raising between ticks: delivered at the next tick's EventsIn, before anything
  // that tick's systems raise.
  const u32 host = bus.add_source("host");
  {
    Emitter emit = bus.emitter(host, 7);
    test::Blast blast;
    blast.n = 999'999u;
    CHECK(emit.raise(listener.blast, blast));
  }
  CHECK(bus.pending() == 1u);
  scheduler.step();
  REQUIRE(listener.heard.size() >= 3u);
  CHECK(listener.heard[0].n == 999'999u);
  CHECK(listener.heard[0].source == host);
  CHECK(listener.heard[1].phase == static_cast<u8>(sim::TickPhase::Lod));
  CHECK(listener.heard.back().phase == static_cast<u8>(sim::TickPhase::Systems));
  CHECK(bus.pending() == 0u);
  CHECK(std::strcmp(bus.source_name(host), "host") == 0);
  CHECK(bus.last_report().tick == 0u);
  CHECK(bus.current_report().delivered == listener.heard.size());
}

TEST_CASE("events: a handler's raises are the next generation, with cause, root and depth") {
  EventBus bus;
  const EventType<test::Ping> ping = bus.register_type<test::Ping>();
  const EventType<test::Pong> pong = bus.register_type<test::Pong>();

  struct Echo {
    EventType<test::Pong> pong;
    static void handle(void* context, const Event& event, Emitter& emit) {
      auto* self = static_cast<Echo*>(context);
      test::Pong out;
      out.n = event.as(EventType<test::Ping>{event.type()}).n + 100u;
      emit.raise(self->pong, out);
    }
  };
  struct Sink {
    Vector<EventRecord> heard;
    static void handle(void* context, const Event& event, Emitter&) {
      static_cast<Sink*>(context)->heard.push_back(*event.record);
    }
  };
  Echo echo{pong};
  Sink sink;
  const u16 ping_types[] = {ping.index};
  const u16 pong_types[] = {pong.index};
  SubscriberDesc a;
  a.name = "echo";
  a.context = &echo;
  a.types = ping_types;
  a.handle = &Echo::handle;
  bus.subscribe(a);
  SubscriberDesc b;
  b.name = "sink";
  b.context = &sink;
  b.types = pong_types;
  b.handle = &Sink::handle;
  bus.subscribe(b);

  const u32 host = bus.add_source("host");
  Emitter emit = bus.emitter(host);
  for (u32 i = 0; i < 3u; ++i) {
    test::Ping p;
    p.n = i;
    emit.raise(ping, p);
  }
  bus.deliver(sim::TickPhase::EventsIn, 1);
  REQUIRE(sink.heard.size() == 3u);
  for (u32 i = 0; i < 3u; ++i) {
    const EventRecord& r = sink.heard[i];
    CHECK(r.depth == 1u);
    CHECK_FALSE(r.cause.is_null());
    CHECK(r.root == r.cause);  // a root's root is itself, and a consequence inherits it
    CHECK(r.source == make_source(Origin::Subscriber, 0));
  }
  // Consequences come after the roots in the tick's ids, in the order their causes were heard.
  CHECK(sink.heard[0].cause < sink.heard[1].cause);
  CHECK(sink.heard[0].id > sink.heard[2].cause);
  CHECK(bus.current_report().depth[0] == 3u);
  CHECK(bus.current_report().depth[1] == 3u);
  CHECK(bus.current_report().max_depth == 1u);
  CHECK(bus.current_report().deliveries == 6u);
  CHECK(bus.type_counters()[ping.index].delivered == 3u);
  CHECK(bus.type_counters()[pong.index].raised == 3u);
}

TEST_CASE("events: a subscriber hears only its types and tiles, in order across its types") {
  EventBus bus;
  const EventType<test::Ping> ping = bus.register_type<test::Ping>();
  const EventType<test::Pong> pong = bus.register_type<test::Pong>();
  const EventType<test::Quiet> quiet = bus.register_type<test::Quiet>();
  struct Both {
    Vector<u32> order;
    static void handle(void* context, const Event& event, Emitter&) {
      static_cast<Both*>(context)->order.push_back(static_cast<u32>(event.record->seq));
    }
    static bool even_tiles(void*, u64 tile) { return tile % 2u == 0u; }
  };
  Both both;
  const u16 types[] = {pong.index, ping.index, ping.index};  // any order, duplicates allowed
  SubscriberDesc desc;
  desc.name = "both";
  desc.context = &both;
  desc.types = types;
  desc.wants_tile = &Both::even_tiles;
  desc.handle = &Both::handle;
  bus.subscribe(desc);

  Emitter emit = bus.emitter(bus.add_source("host"));
  emit.raise(ping, test::Ping{});                   // 0
  emit.raise(pong, test::Pong{});                   // 1
  emit.raise(quiet, test::Quiet{});                 // 2: not heard
  emit.raise(ping, test::Ping{}, RaiseOptions{3});  // 3: an odd tile, not heard
  emit.raise(pong, test::Pong{}, RaiseOptions{4});  // 4
  emit.raise(ping, test::Ping{});                   // 5
  bus.deliver(sim::TickPhase::EventsIn, 1);
  const u32 expected[] = {0, 1, 4, 5};
  REQUIRE(both.order.size() == 4u);
  for (u32 i = 0; i < 4u; ++i)
    CHECK(both.order[i] == expected[i]);
  CHECK(bus.current_report().delivered == 6u);
}

TEST_CASE("events: the document's change feed raises RecordChanged at EventsIn, in id order") {
  doc::Document d;
  d.add_layer("base", doc::LayerRole::Base);
  const char* type = "engine.events_test.Marker";
  const auto id = [](u64 n) { return Id128::from_parts(0xE7, n); };
  REQUIRE(d.apply(doc::cmd_create(id(3), type), nullptr, nullptr));

  sim::SimScheduler scheduler;
  EventBus bus;
  bus.attach_document(&d);
  const EventType<RecordChanged> changed{0};
  struct Log {
    Vector<RecordChanged> heard;
    Vector<u8> phases;
    static void handle(void* context, const Event& event, Emitter&) {
      auto* self = static_cast<Log*>(context);
      self->heard.push_back(event.as(EventType<RecordChanged>{event.type()}));
      self->phases.push_back(event.record->phase);
    }
  };
  Log log;
  const u16 types[] = {changed.index};
  SubscriberDesc desc;
  desc.name = "log";
  desc.context = &log;
  desc.types = types;
  desc.handle = &Log::handle;
  bus.subscribe(desc);
  bus.install(scheduler);

  // What existed when the document was attached is not news.
  scheduler.step();
  CHECK(log.heard.empty());

  REQUIRE(d.apply(doc::cmd_create(id(9), type), nullptr, nullptr));
  REQUIRE(d.apply(doc::cmd_create(id(2), type), nullptr, nullptr));
  REQUIRE(d.apply(doc::cmd_delete(id(3)), nullptr, nullptr));
  scheduler.step();
  REQUIRE(log.heard.size() == 3u);
  CHECK(log.heard[0].record == id(2));
  CHECK(log.heard[1].record == id(3));
  CHECK(log.heard[2].record == id(9));
  CHECK(log.heard[0].live);
  CHECK_FALSE(log.heard[1].live);
  CHECK(log.heard[1].record_type == 0u);
  CHECK(log.heard[0].record_type == schema::stable_type_id(type));
  CHECK(log.phases[0] == static_cast<u8>(sim::TickPhase::EventsIn));
  scheduler.step();
  CHECK(log.heard.size() == 3u);
}

namespace {

// A world in miniature for the replay: a player's raises come from an external source, a system
// raises from its own state, and a subscriber reacts with consequences that change that state.
struct World {
  sim::SimScheduler scheduler;
  EventBus bus;
  EventType<test::Blast> blast;
  EventType<test::Ping> ping;
  u32 player = 0;
  u64 state = 1;

  static void step_state(sim::SystemContext& context, sim::Batch batch) {
    auto* self = static_cast<World*>(context.context);
    self->state = self->state * 6364136223846793005ull + 1442695040888963407ull;
    if ((self->state >> 60) < 6u) {
      Emitter emit = self->bus.emitter(context, batch);
      test::Ping p;
      p.n = static_cast<u32>(self->state >> 32);
      emit.raise(self->ping, p);
    }
  }
  static void react(void* context, const Event& event, Emitter& emit) {
    auto* self = static_cast<World*>(context);
    if (event.type() == self->blast.index) {
      self->state ^= event.as(self->blast).n;
      test::Ping p;
      p.n = event.as(self->blast).n + 1u;
      emit.raise(self->ping, p);
    } else {
      self->state += event.as(self->ping).n;
    }
  }

  World() {
    blast = bus.register_type<test::Blast>();
    ping = bus.register_type<test::Ping>();
    player = bus.add_source("player", true);
    sim::SystemDesc system;
    system.name = "world.state";
    system.context = this;
    system.tick = &step_state;
    scheduler.add_system(system);
    bus.install(scheduler);
  }
};

struct Recorded {
  ExternalRaise raise;
  Vector<u8> bytes;
};

void record_external(void* context, const ExternalRaise& entry) {
  auto* log = static_cast<Vector<Recorded>*>(context);
  Recorded r;
  r.raise = entry;
  for (const u8 b : entry.payload)
    r.bytes.push_back(b);
  log->push_back(static_cast<Recorded&&>(r));
}

void subscribe_world(World& w, Vector<u16>& types) {
  types.push_back(w.blast.index);
  types.push_back(w.ping.index);
  // Not a self-loop by the bus's rule only if the reaction's type is one it does not hear; this
  // subscriber hears pings and raises them, so its pings wait a tick each: part of the replay.
  SubscriberDesc desc;
  desc.name = "world.react";
  desc.context = &w;
  desc.types = std::span<const u16>(types.data(), types.size());
  desc.handle = &World::react;
  w.bus.subscribe(desc);
}

}  // namespace

TEST_CASE("events: a run replays from its external raises alone, delivery for delivery") {
  Vector<Recorded> log;
  u64 recorded_hash = 0;
  u64 recorded_state = 0;
  {
    World w;
    Vector<u16> types;
    subscribe_world(w, types);
    w.bus.set_external_log(ExternalLog{&log, &record_external});
    for (u32 t = 0; t < 40u; ++t) {
      if (t % 3u == 0u) {
        Emitter emit = w.bus.emitter(w.player, t);
        test::Blast blast;
        blast.n = t * 17u;
        blast.at = WorldPos{static_cast<f64>(t), 0.0, 0.0};
        emit.raise(w.blast, blast);
        emit.raise(w.blast, blast);
      }
      w.scheduler.step();
    }
    recorded_hash = w.bus.delivery_hash();
    recorded_state = w.state;
    CHECK(w.bus.total_trips().self_loops > 0u);
  }
  REQUIRE(log.size() == 28u);

  const auto replay = [&log](u32 skip) {
    World w;
    Vector<u16> types;
    subscribe_world(w, types);
    u32 next = 0;
    for (u32 t = 0; t < 40u; ++t) {
      while (next < log.size() && log[next].raise.tick == w.scheduler.tick().value) {
        if (next != skip) {
          ExternalRaise entry = log[next].raise;
          entry.payload = std::span<const u8>(log[next].bytes.data(), log[next].bytes.size());
          CHECK(w.bus.replay_external(entry));
        }
        ++next;
      }
      w.scheduler.step();
    }
    return std::pair<u64, u64>(w.bus.delivery_hash(), w.state);
  };
  const auto same = replay(0xFFFF'FFFFu);
  CHECK(same.first == recorded_hash);
  CHECK(same.second == recorded_state);
  // One input left out is a different run, and the hash says so.
  const auto missing = replay(5);
  CHECK(missing.first != recorded_hash);
}

// The same run through `ExternalRecording` (roadmap T53): recorded through its hook, written to its
// file form, read back and raised again tick by tick — the replay a `sim::RunRecord` names. (The
// file's bytes, not a file: `save` and `load` are those bytes and one write or read.)
TEST_CASE("events: external raises written to a file replay to the same deliveries") {
  std::string bytes;
  u64 recorded_hash = 0;
  u64 recorded_state = 0;
  {
    World w;
    Vector<u16> types;
    subscribe_world(w, types);
    ExternalRecording recording;
    w.bus.set_external_log(recording.hook());
    for (u32 t = 0; t < 40u; ++t) {
      if (t % 3u == 0u) {
        Emitter emit = w.bus.emitter(w.player, t);
        test::Blast blast;
        blast.n = t * 17u;
        blast.at = WorldPos{static_cast<f64>(t), 0.0, 0.0};
        emit.raise(w.blast, blast);
      }
      w.scheduler.step();
    }
    recorded_hash = w.bus.delivery_hash();
    recorded_state = w.state;
    REQUIRE(recording.size() == 14u);
    recording.serialize(bytes);
  }
  ExternalRecording read;
  std::string error;
  REQUIRE_MESSAGE(read.deserialize(bytes, &error), error);
  REQUIRE(read.size() == 14u);
  World w;
  Vector<u16> types;
  subscribe_world(w, types);
  u32 taken = 0;
  for (u32 t = 0; t < 40u; ++t) {
    u32 refused = 0;
    taken += read.replay_tick(w.bus, w.scheduler.tick().value, &refused);
    CHECK(refused == 0u);
    w.scheduler.step();
  }
  CHECK(taken == 14u);
  CHECK(w.bus.delivery_hash() == recorded_hash);
  CHECK(w.state == recorded_state);
  // A file cut short is refused, and leaves nothing behind.
  CHECK_FALSE(read.deserialize(std::string_view(bytes).substr(0, bytes.size() - 1), &error));
  CHECK(read.size() == 0u);
}
