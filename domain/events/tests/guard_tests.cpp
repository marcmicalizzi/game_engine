// The world event bus's runaway guards (docs/plan/05-simulation.md §5.7; docs/subsystems/events.md,
// "The runaway guards"): each trips where its rule says, is counted in the tick's report, loses
// nothing it says it carries, and a steady tick allocates nothing.

#include <core/containers/vector.h>
#include <core/jobs/job_system.h>
#include <core/memory/memory.h>
#include <core/time/time.h>
#include <domain/events/events.h>
#include <domain/sim/scheduler.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <schemas/events_test.h>
#include <span>

using namespace engine;
using namespace engine::events;
namespace test = engine::events_test;

namespace {

struct Counter {
  u32 heard = 0;
  u32 overflowed = 0;
  u32 overflow_calls = 0;
  Vector<u8> depths;
  static void handle(void* context, const Event& event, Emitter&) {
    auto* self = static_cast<Counter*>(context);
    ++self->heard;
    self->depths.push_back(event.record->depth);
  }
  static void overflow(void* context, u32 skipped) {
    auto* self = static_cast<Counter*>(context);
    self->overflowed += skipped;
    ++self->overflow_calls;
  }
};

// Raises `fan` events of `out` for every event it hears.
struct Fan {
  u16 out = k_invalid_type;
  u32 fan = 1;
  static void handle(void* context, const Event& event, Emitter& emit) {
    auto* self = static_cast<Fan*>(context);
    for (u32 i = 0; i < self->fan; ++i) {
      test::Pong p;
      p.n = event.record->seq;
      emit.raise_bytes(self->out, &p, sizeof(p), alignof(test::Pong), {});
    }
  }
};

u16 subscribe(EventBus& bus, const char* name, void* context, std::span<const u16> types,
              void (*handle)(void*, const Event&, Emitter&), u32 budget = 0,
              void (*overflow)(void*, u32) = nullptr) {
  SubscriberDesc desc;
  desc.name = name;
  desc.context = context;
  desc.types = types;
  desc.handle = handle;
  desc.budget = budget;
  desc.overflow = overflow;
  return bus.subscribe(desc);
}

void raise_pings(EventBus& bus, u32 source, EventType<test::Ping> ping, u32 count, u64 key = 0) {
  Emitter emit = bus.emitter(source, key);
  for (u32 i = 0; i < count; ++i) {
    test::Ping p;
    p.n = i;
    emit.raise(ping, p);
  }
}

}  // namespace

TEST_CASE("guards: depth — a chain past max_depth is dropped at the raise, and counted") {
  EventBusConfig config;
  config.max_depth = 3;
  EventBus bus(config);
  const EventType<test::Ping> ping = bus.register_type<test::Ping>();
  const EventType<test::Pong> pong = bus.register_type<test::Pong>();
  // Two subscribers bouncing a chain between the two types: neither hears what it raises, so no
  // self-loop slows it and only the depth cap ends it.
  struct Bounce {
    u16 out = 0;
    Vector<u8> depths;
    static void handle(void* context, const Event& event, Emitter& emit) {
      auto* self = static_cast<Bounce*>(context);
      self->depths.push_back(event.record->depth);
      test::Ping p;
      emit.raise_bytes(self->out, &p, sizeof(p), alignof(test::Ping), {});
    }
  };
  Bounce a{pong.index, {}};
  Bounce b{ping.index, {}};
  const u16 hears_ping[] = {ping.index};
  const u16 hears_pong[] = {pong.index};
  subscribe(bus, "a", &a, hears_ping, &Bounce::handle);
  subscribe(bus, "b", &b, hears_pong, &Bounce::handle);
  raise_pings(bus, bus.add_source("host"), ping, 2);
  bus.deliver(sim::TickPhase::EventsIn, 1);

  // Depths 0..3 delivered for each of two roots; the raise at depth 4 refused twice.
  const TickReport& report = bus.current_report();
  CHECK(report.trips.depth_dropped == 2u);
  CHECK(report.max_depth == 3u);
  CHECK(report.depth[0] == 2u);
  CHECK(report.depth[3] == 2u);
  CHECK(report.delivered == 8u);
  CHECK(a.depths.size() == 4u);  // depths 0 and 2, twice
  CHECK(bus.pending() == 0u);
  // Each root's chain is three consequences, and both chains are in the top list, the earlier
  // root first on the tie: the list is made when the tick closes.
  bus.deliver(sim::TickPhase::EventsIn, 2);
  const TickReport& closed = bus.last_report();
  CHECK(closed.tick == 1u);
  CHECK(closed.top_chains[0].events == 3u);
  CHECK(closed.top_chains[1].events == 3u);
  CHECK(closed.top_chains[0].root < closed.top_chains[1].root);
  CHECK(closed.top_chains[2].root.is_null());
}

TEST_CASE("guards: cascade — past max_cascade a tick, consequences wait, and none is lost") {
  EventBusConfig config;
  config.max_cascade = 10;
  EventBus bus(config);
  const EventType<test::Ping> ping = bus.register_type<test::Ping>();
  const EventType<test::Pong> pong = bus.register_type<test::Pong>();
  Fan fan{pong.index, 2};
  Counter sink;
  const u16 hears_ping[] = {ping.index};
  const u16 hears_pong[] = {pong.index};
  subscribe(bus, "fan", &fan, hears_ping, &Fan::handle);
  subscribe(bus, "sink", &sink, hears_pong, &Counter::handle);
  raise_pings(bus, bus.add_source("host"), ping, 20);

  bus.deliver(sim::TickPhase::EventsIn, 1);
  CHECK(sink.heard == 10u);
  CHECK(bus.current_report().trips.cascade_carried == 30u);
  CHECK(bus.carried() == 30u);
  // The carry waits for the next tick's EventsIn, not this tick's EventsOut.
  bus.deliver(sim::TickPhase::EventsOut, 1);
  CHECK(sink.heard == 10u);
  u64 tick = 2;
  for (; tick < 10u && bus.carried() != 0u; ++tick)
    bus.deliver(sim::TickPhase::EventsIn, tick);
  CHECK(sink.heard == 40u);
  CHECK(tick == 5u);  // 10 a tick: ticks 1, 2, 3 and 4
  CHECK(bus.current_report().carried_in == 10u);
  CHECK(bus.last_report().carried_in == 20u);
  CHECK(bus.total_trips().cascade_carried == 30u + 20u + 10u);
  CHECK(bus.total_trips().full_dropped == 0u);
}

TEST_CASE("guards: storm — one source past max_per_source waits; another source does not") {
  EventBusConfig config;
  config.max_per_source = 100;
  EventBus bus(config);
  const EventType<test::Ping> ping = bus.register_type<test::Ping>();
  Counter counter;
  const u16 hears_ping[] = {ping.index};
  subscribe(bus, "counter", &counter, hears_ping, &Counter::handle);
  const u32 storm = bus.add_source("storm");
  const u32 calm = bus.add_source("calm");
  raise_pings(bus, storm, ping, 250);
  raise_pings(bus, calm, ping, 5);

  bus.deliver(sim::TickPhase::EventsIn, 1);
  CHECK(counter.heard == 105u);
  CHECK(bus.current_report().trips.storm_carried == 150u);
  // The source's count is per tick: a second point in the same tick admits none of it.
  raise_pings(bus, storm, ping, 1, 1);
  bus.deliver(sim::TickPhase::EventsOut, 1);
  CHECK(counter.heard == 105u);
  CHECK(bus.current_report().trips.storm_carried == 151u);
  bus.deliver(sim::TickPhase::EventsIn, 2);
  CHECK(counter.heard == 205u);
  bus.deliver(sim::TickPhase::EventsIn, 3);
  CHECK(counter.heard == 256u);
  CHECK(bus.carried() == 0u);
}

TEST_CASE("guards: self-loop — a subscriber's raise of a type it hears waits a tick") {
  EventBus bus;
  const EventType<test::Ping> ping = bus.register_type<test::Ping>();
  struct Loop {
    EventType<test::Ping> ping;
    u32 heard = 0;
    static void handle(void* context, const Event&, Emitter& emit) {
      auto* self = static_cast<Loop*>(context);
      ++self->heard;
      emit.raise(self->ping, test::Ping{});
    }
  };
  Loop loop{ping, 0};
  const u16 hears_ping[] = {ping.index};
  subscribe(bus, "loop", &loop, hears_ping, &Loop::handle);
  raise_pings(bus, bus.add_source("host"), ping, 1);
  for (u64 tick = 1; tick <= 5u; ++tick) {
    bus.deliver(sim::TickPhase::EventsIn, tick);
    bus.deliver(sim::TickPhase::EventsOut, tick);
    CHECK(loop.heard == tick);  // one a tick, never a spin inside one
    CHECK(bus.current_report().trips.self_loops == 1u);
  }
  CHECK(bus.current_report().depth[4] == 1u);
}

TEST_CASE(
    "guards: budget — past its budget a subscriber skips, is told how many, others hear all") {
  EventBus bus;
  const EventType<test::Ping> ping = bus.register_type<test::Ping>();
  Counter small;
  Counter large;
  const u16 hears_ping[] = {ping.index};
  subscribe(bus, "small", &small, hears_ping, &Counter::handle, 5, &Counter::overflow);
  subscribe(bus, "large", &large, hears_ping, &Counter::handle);
  const u32 host = bus.add_source("host");
  raise_pings(bus, host, ping, 12);
  bus.deliver(sim::TickPhase::EventsIn, 1);
  CHECK(small.heard == 5u);
  CHECK(small.overflowed == 7u);
  CHECK(small.overflow_calls == 1u);
  CHECK(large.heard == 12u);
  CHECK(bus.current_report().trips.budget_skipped == 7u);
  // A new tick, a new budget.
  raise_pings(bus, host, ping, 3);
  bus.deliver(sim::TickPhase::EventsIn, 2);
  CHECK(small.heard == 8u);
  CHECK(small.overflow_calls == 1u);
}

TEST_CASE("guards: full — a raise past the bank's capacity is refused, returned and counted") {
  EventBusConfig config;
  config.capacity = 16;
  EventBus bus(config);
  const EventType<test::Ping> ping = bus.register_type<test::Ping>();
  Counter counter;
  const u16 hears_ping[] = {ping.index};
  subscribe(bus, "counter", &counter, hears_ping, &Counter::handle);
  Emitter emit = bus.emitter(bus.add_source("host"));
  u32 accepted = 0;
  for (u32 i = 0; i < 20u; ++i)
    accepted += emit.raise(ping, test::Ping{}) ? 1u : 0u;
  CHECK(accepted == 16u);
  bus.deliver(sim::TickPhase::EventsIn, 1);
  CHECK(counter.heard == 16u);
  CHECK(bus.current_report().trips.full_dropped == 4u);
  // The payload arena is a bound too.
  EventBusConfig tight;
  tight.payload_bytes = 64;
  EventBus small(tight);
  const EventType<test::Blast> blast = small.register_type<test::Blast>();
  Emitter e = small.emitter(small.add_source("host"));
  u32 fit = 0;
  for (u32 i = 0; i < 4u; ++i)
    fit += e.raise(blast, test::Blast{}) ? 1u : 0u;
  CHECK(fit == 64u / ((sizeof(test::Blast) + 7u) & ~usize{7}));
}

namespace {

// B5: a wave that overfills the live bank. Eight batches of one source raise Blasts and Pings in
// turn under their own keys; the bank holds about a third of them.
constexpr u32 k_overfill_batches = 8;
constexpr u32 k_overfill_raises = 2000;

struct OverfillHeard {
  u64 key = 0;
  u32 seq = 0;
  u32 n = 0;
  u32 type = 0;
  u32 pad = 0;
  bool operator==(const OverfillHeard&) const = default;
};

struct OverfillListener {
  EventType<test::Blast> blast;
  EventType<test::Ping> ping;
  Vector<OverfillHeard> heard;
  static void handle(void* context, const Event& event, Emitter&) {
    auto* self = static_cast<OverfillListener*>(context);
    OverfillHeard h;
    h.key = event.record->key;
    h.seq = event.record->seq;
    h.type = event.type();
    h.n = event.type() == self->blast.index ? event.as(self->blast).n : event.as(self->ping).n;
    self->heard.push_back(h);
  }
};

struct OverfillBatch {
  EventBus* bus = nullptr;
  EventType<test::Blast> blast;
  EventType<test::Ping> ping;
  u32 source = 0;
  u32 batch = 0;
  static void run(void* data) {
    auto* self = static_cast<OverfillBatch*>(data);
    Emitter emit = self->bus->emitter(self->source, u64{self->batch} + 1u);
    for (u32 i = 0; i < k_overfill_raises; ++i) {
      if (i % 2u == 0u) {
        test::Blast b;
        b.n = self->batch * 10000u + i;
        b.strength = static_cast<f32>(i);
        emit.raise(self->blast, b);
      } else {
        test::Ping p;
        p.n = self->batch * 10000u + i;
        emit.raise(self->ping, p);
      }
    }
  }
};

struct OverfillRun {
  Vector<OverfillHeard> heard;
  u64 hash = 0;
  u32 refused = 0;
};

OverfillRun run_overfill(const EventBusConfig& config, jobs::JobSystem* jobs,
                         std::span<const u32> order) {
  EventBus bus(config);
  OverfillListener listener;
  listener.blast = bus.register_type<test::Blast>();
  listener.ping = bus.register_type<test::Ping>();
  const u16 types[] = {listener.blast.index, listener.ping.index};
  subscribe(bus, "listener", &listener, types, &OverfillListener::handle);
  const u32 source = bus.add_source("wave");
  OverfillBatch batches[k_overfill_batches];
  for (u32 b = 0; b < k_overfill_batches; ++b) {
    batches[b].bus = &bus;
    batches[b].blast = listener.blast;
    batches[b].ping = listener.ping;
    batches[b].source = source;
    batches[b].batch = b;
  }
  if (jobs == nullptr) {
    for (const u32 b : order)
      OverfillBatch::run(&batches[b]);
  } else {
    jobs::Counter counter;
    jobs::Job list[k_overfill_batches];
    for (u32 k = 0; k < k_overfill_batches; ++k) {
      list[k].fn = &OverfillBatch::run;
      list[k].data = &batches[order[k]];
    }
    jobs->schedule(jobs::Pool::Performance, list, counter);
    jobs->wait(counter);
  }
  bus.deliver(sim::TickPhase::EventsIn, 1);
  OverfillRun run;
  run.heard = static_cast<Vector<OverfillHeard>&&>(listener.heard);
  run.hash = bus.delivery_hash();
  run.refused = bus.current_report().trips.full_dropped;
  return run;
}

}  // namespace

TEST_CASE("guards: full — a wave that overfills the bank keeps the lowest keys at any arrival") {
  // Two banks: one whose arena runs out first (Blasts and Pings of different sizes, so evictions
  // leave holes and the selection compacts), one whose records do.
  EventBusConfig by_bytes;
  by_bytes.capacity = 4096;
  by_bytes.payload_bytes = 48u << 10;
  EventBusConfig by_records;
  by_records.capacity = 2300;
  by_records.payload_bytes = 1u << 20;
  const u32 orders[][k_overfill_batches] = {
      {0, 1, 2, 3, 4, 5, 6, 7},
      {7, 6, 5, 4, 3, 2, 1, 0},
      {3, 6, 0, 5, 1, 7, 2, 4},
      {7, 1, 5, 3, 0, 6, 4, 2},
  };
  jobs::JobSystemConfig jobs_config;
  jobs_config.performance_workers = 1;
  jobs_config.efficiency_workers = 1;
  jobs::JobSystem one(jobs_config);
  jobs_config.performance_workers = 8;
  jobs::JobSystem eight(jobs_config);
  jobs::JobSystem* const systems[] = {nullptr, &one, &eight};

  for (const EventBusConfig& config : {by_bytes, by_records}) {
    // The run that fits, in key order: batch by batch, each in its own sequence, until the first
    // raise that does not fit.
    const u32 blast_words = static_cast<u32>((sizeof(test::Blast) + 7u) / 8u);
    const u32 ping_words = static_cast<u32>((sizeof(test::Ping) + 7u) / 8u);
    u32 kept = 0;
    u32 words = 0;
    for (; kept < k_overfill_batches * k_overfill_raises; ++kept) {
      const u32 w = (kept % k_overfill_raises) % 2u == 0u ? blast_words : ping_words;
      if (kept + 1u > config.capacity || words + w > config.payload_bytes / 8u) break;
      words += w;
    }
    REQUIRE(kept < k_overfill_batches * k_overfill_raises / 2u);

    const OverfillRun reference = run_overfill(config, nullptr, orders[0]);
    REQUIRE(reference.heard.size() == kept);
    bool prefix = true;
    for (u32 j = 0; j < kept; ++j) {
      const OverfillHeard& h = reference.heard[j];
      prefix = prefix && h.key == j / k_overfill_raises + 1u && h.seq == j % k_overfill_raises;
    }
    CHECK(prefix);
    CHECK(reference.refused == k_overfill_batches * k_overfill_raises - kept);

    for (jobs::JobSystem* const system : systems) {
      for (const auto& order : orders) {
        const OverfillRun run = run_overfill(config, system, order);
        CAPTURE(order[0]);
        CAPTURE(system == nullptr ? 0 : system == &one ? 1 : 8);
        CHECK(run.heard.size() == reference.heard.size());
        CHECK((run.heard.size() == reference.heard.size() &&
               std::equal(run.heard.begin(), run.heard.end(), reference.heard.begin())));
        CHECK(run.refused == reference.refused);
        CHECK(run.hash == reference.hash);
      }
    }
  }
}

TEST_CASE("guards: slow — a subscriber past slow_subscriber_us is reported and nothing else") {
  EventBusConfig config;
  config.slow_subscriber_us = 1000;
  EventBus bus(config);
  const EventType<test::Ping> ping = bus.register_type<test::Ping>();
  struct Slow {
    u32 heard = 0;
    static void handle(void* context, const Event&, Emitter&) {
      ++static_cast<Slow*>(context)->heard;
      const i64 until = time::monotonic_ns() + 3'000'000;
      while (time::monotonic_ns() < until) {
      }
    }
  };
  Counter fast;
  Slow slow;
  const u16 hears_ping[] = {ping.index};
  subscribe(bus, "fast", &fast, hears_ping, &Counter::handle);
  const u16 slow_index = subscribe(bus, "slow", &slow, hears_ping, &Slow::handle);
  raise_pings(bus, bus.add_source("host"), ping, 1);
  bus.deliver(sim::TickPhase::EventsIn, 1);
  bus.deliver(sim::TickPhase::EventsIn, 2);  // closes tick 1's report
  const TickReport& report = bus.last_report();
  CHECK(report.trips.slow_subscribers == 1u);
  CHECK(report.slowest_subscriber == slow_index);
  CHECK(report.slowest_us >= 3000u);
  // Reported, never acted on: both heard the event.
  CHECK(slow.heard == 1u);
  CHECK(fast.heard == 1u);
}

TEST_CASE("guards: two raises with one whole key are counted as a tie") {
  EventBus bus;
  const EventType<test::Ping> ping = bus.register_type<test::Ping>();
  const u32 host = bus.add_source("host");
  Emitter a = bus.emitter(host, 3);
  Emitter b = bus.emitter(host, 3);
  a.raise(ping, test::Ping{});
  b.raise(ping, test::Ping{});
  bus.deliver(sim::TickPhase::EventsIn, 1);
  CHECK(bus.current_report().trips.key_ties == 1u);
  CHECK(bus.current_report().delivered == 2u);
}

TEST_CASE("guards: a steady tick allocates nothing") {
  sim::SimScheduler scheduler;
  EventBus bus;
  const EventType<test::Ping> ping = bus.register_type<test::Ping>();
  const EventType<test::Pong> pong = bus.register_type<test::Pong>();
  struct Raise {
    EventBus* bus = nullptr;
    EventType<test::Ping> ping;
    static void tick(sim::SystemContext& context, sim::Batch batch) {
      auto* self = static_cast<Raise*>(context.context);
      Emitter emit = self->bus->emitter(context, batch);
      for (u32 i = 0; i < 1000u; ++i)
        emit.raise(self->ping, test::Ping{});
    }
  };
  Raise raiser{&bus, ping};
  sim::SystemDesc system;
  system.name = "raise";
  system.batches = 4;
  system.context = &raiser;
  system.tick = &Raise::tick;
  scheduler.add_system(system);
  Fan fan{pong.index, 1};
  Counter sink;
  sink.depths.reserve(1u << 20);
  const u16 hears_ping[] = {ping.index};
  const u16 hears_pong[] = {pong.index};
  subscribe(bus, "fan", &fan, hears_ping, &Fan::handle);
  subscribe(bus, "sink", &sink, hears_pong, &Counter::handle);
  bus.install(scheduler);

  for (u32 t = 0; t < 3u; ++t)
    scheduler.step();  // the first ticks size what grows once
  const u64 before = mem::allocation_counter();
  for (u32 t = 0; t < 20u; ++t)
    scheduler.step();
  CHECK(mem::allocation_counter() == before);
  CHECK(sink.heard == 23u * 4000u);
  // No guard of the simulation's tripped. The slow report is the machine's (a debug build's
  // handlers can pass 2 ms at 8,000 deliveries a tick), so it is left out of the sum.
  const GuardTrips& trips = bus.total_trips();
  CHECK(trips.total() - trips.slow_subscribers == 0u);
}
