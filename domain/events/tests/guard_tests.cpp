// The world event bus's runaway guards (docs/plan/05-simulation.md §5.7; docs/subsystems/events.md,
// "The runaway guards"): each trips where its rule says, is counted in the tick's report, loses
// nothing it says it carries, and a steady tick allocates nothing.

#include <core/containers/vector.h>
#include <core/memory/memory.h>
#include <core/time/time.h>
#include <domain/events/events.h>
#include <domain/sim/scheduler.h>

#include <doctest/doctest.h>

#include <schemas/events_test.h>

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
