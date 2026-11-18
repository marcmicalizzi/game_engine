// The world event bus's delivery (docs/subsystems/events.md, "Performance"): what one event costs
// to raise, order and deliver, at 10^3 and 10^5 events a tick.
//
//   events.deliver.ordered   N raises that arrive in key order (one source, one thread) and one
//                            subscriber: the steady case, no sort.
//   events.deliver.shuffled  the same N from N emitters whose keys arrive backwards, as a parallel
//                            wave's raises can: the sort is paid.
//   events.deliver.fanout    in order, heard by four subscribers: the per-delivery cost.
//   events.deliver.unhashed  `ordered` with the delivery hash off: what the replay check costs.
//
// One iteration is one tick: N raises and the delivery point that delivers them. Items are events.

#include <core/base/macros.h>
#include <core/containers/vector.h>
#include <domain/events/events.h>
#include <foundation/bench/bench.h>

#include <schemas/events_test.h>

using namespace engine;
using namespace engine::events;
namespace test = engine::events_test;

namespace {

struct Count {
  u64 heard = 0;
  static void handle(void* context, const Event& event, Emitter&) {
    static_cast<Count*>(context)->heard += event.record->seq;
  }
};

EventBusConfig bench_config() {
  EventBusConfig config;
  config.capacity = 1u << 17;
  config.payload_bytes = 8u << 20;
  config.max_per_source = 1u << 24;
  return config;
}

void run(bench::State& state, u32 subscribers, bool backwards, bool hashed = true) {
  const u32 n = static_cast<u32>(state.arg());
  EventBusConfig config = bench_config();
  config.hash_deliveries = hashed;
  EventBus bus(config);
  const EventType<test::Ping> ping = bus.register_type<test::Ping>();
  const u16 types[] = {ping.index};
  Count counts[4];
  for (u32 s = 0; s < subscribers; ++s) {
    SubscriberDesc desc;
    desc.name = "count";
    desc.context = &counts[s];
    desc.types = types;
    desc.handle = &Count::handle;
    bus.subscribe(desc);
  }
  const u32 source = bus.add_source("bench");
  u64 tick = 1;
  while (state.keep_running()) {
    if (backwards) {
      for (u32 i = 0; i < n; ++i) {
        Emitter emit = bus.emitter(source, static_cast<u64>(n - i));
        test::Ping p;
        p.n = i;
        emit.raise(ping, p);
      }
    } else {
      Emitter emit = bus.emitter(source);
      for (u32 i = 0; i < n; ++i) {
        test::Ping p;
        p.n = i;
        emit.raise(ping, p);
      }
    }
    bus.deliver(sim::TickPhase::EventsIn, tick++);
    bench::keep(counts[0].heard);
  }
  state.set_items(n);
}

}  // namespace

ENGINE_BENCH_ARGS(deliver_ordered, "events.deliver.ordered", 1000, 100000) { run(state, 1, false); }
ENGINE_BENCH_ARGS(deliver_shuffled, "events.deliver.shuffled", 1000, 100000) {
  run(state, 1, true);
}
ENGINE_BENCH_ARGS(deliver_fanout, "events.deliver.fanout", 1000, 100000) { run(state, 4, false); }
ENGINE_BENCH_ARGS(deliver_unhashed, "events.deliver.unhashed", 1000, 100000) {
  run(state, 1, false, false);
}
