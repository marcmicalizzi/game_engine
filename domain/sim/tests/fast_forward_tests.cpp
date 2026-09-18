// Fast-forward (docs/plan/05-simulation.md §5.3, §13.5): when the due-event count exceeds the
// budget, systems coarsen through `SummarizeInterval` instead of executing every event.
//
// The economy in these tests is the plan's own example — an hourly periodic — and its summarizer
// is *exact*: it adds the number of whole hours in the interval. An exact summarizer is the one
// case where the contract can be checked rather than merely exercised, because the state after
// the fast-forward must equal the state after the long way round, bit for bit.

#include <core/containers/vector.h>
#include <domain/sim/timing_wheel.h>

#include <doctest/doctest.h>

using namespace engine;
using namespace engine::sim;

namespace {

constexpr u16 k_economy = 1;
constexpr u16 k_ecology = 2;
constexpr u16 k_courier = 3;  // a periodic with no summarizer: not coarsenable

// A system whose state is a count of elapsed game hours, reachable two ways.
struct HourCounter {
  u64 hours = 0;
  u32 summarize_calls = 0;
  u32 events = 0;

  static void summarize(void* context, const SummarizeInterval& interval) {
    auto* self = static_cast<HourCounter*>(context);
    ++self->summarize_calls;
    self->hours +=
        static_cast<u64>((interval.to.us - interval.from.us) / GameTime::from_hours(1).us);
  }
};

Summarizer summarizer_for(u16 system, HourCounter& counter, const char* name) {
  Summarizer summarizer;
  summarizer.name = name;
  summarizer.fn = &HourCounter::summarize;
  summarizer.context = &counter;
  summarizer.system = system;
  summarizer.tiers = 0x0Fu;
  return summarizer;
}

}  // namespace

TEST_CASE("fast-forward: a large budget delivers every hourly event over thirty game days") {
  TimingWheel wheel;
  HourCounter economy;
  wheel.add_summarizer(summarizer_for(k_economy, economy, "economy"));
  wheel.schedule_periodic(k_economy, GameTime::from_hours(1), GameTime{0});

  auto body = [&](const TimerEvent& event) {
    if (event.system == k_economy) {
      ++economy.events;
      ++economy.hours;
    }
  };
  EventSink sink = make_sink(body);

  const GameTime target = GameTime::from_days(30);
  const FastForwardResult result = wheel.advance(GameTime{0}, target, 100'000, sink);

  CHECK_FALSE(result.over_budget);
  CHECK(result.delivered == 720);
  CHECK(result.summarized == 0);
  CHECK(result.coarsened == 0);
  CHECK(economy.events == 720);
  CHECK(economy.hours == 720);
  CHECK(economy.summarize_calls == 0);
  CHECK(wheel.now().us == target.us);
}

TEST_CASE("fast-forward: a small budget summarizes once per summarizer and fires nothing") {
  TimingWheel wheel;
  HourCounter economy;
  HourCounter ecology;
  wheel.add_summarizer(summarizer_for(k_economy, economy, "economy"));
  wheel.add_summarizer(summarizer_for(k_ecology, ecology, "ecology"));
  wheel.schedule_periodic(k_economy, GameTime::from_hours(1), GameTime{0});
  wheel.schedule_periodic(k_ecology, GameTime::from_days(1), GameTime{0});

  auto body = [&](const TimerEvent& event) {
    if (event.system == k_economy) ++economy.events;
    if (event.system == k_ecology) ++ecology.events;
  };
  EventSink sink = make_sink(body);

  const GameTime target = GameTime::from_days(30);
  const FastForwardResult result = wheel.advance(GameTime{0}, target, 64, sink);

  CHECK(result.over_budget);
  CHECK(result.due_estimate > 64);
  CHECK(result.summarized == 2);  // exactly one call per registered summarizer
  CHECK(result.coarsened == 2);   // both periodics were re-armed past the target
  CHECK(result.delivered == 0);
  CHECK(economy.summarize_calls == 1);
  CHECK(ecology.summarize_calls == 1);
  CHECK(economy.events == 0);
  CHECK(ecology.events == 0);
  CHECK(wheel.now().us == target.us);

  // The periodics are still armed, on phase, for the first firing after the gap.
  CHECK(wheel.live_count() == 2);
  auto after = [&](const TimerEvent& event) {
    if (event.system == k_economy) ++economy.events;
  };
  EventSink second = make_sink(after);
  wheel.advance(target + GameTime::from_hours(1), second);
  CHECK(economy.events == 1);
}

TEST_CASE("fast-forward: an exact summarizer ends in the same state either way") {
  const GameTime target = GameTime::from_days(30);

  HourCounter played;
  {
    TimingWheel wheel;
    wheel.add_summarizer(summarizer_for(k_economy, played, "economy"));
    wheel.schedule_periodic(k_economy, GameTime::from_hours(1), GameTime{0});
    auto body = [&](const TimerEvent& event) {
      if (event.system == k_economy) ++played.hours;
    };
    EventSink sink = make_sink(body);
    wheel.advance(GameTime{0}, target, 100'000, sink);
  }

  HourCounter skipped;
  {
    TimingWheel wheel;
    wheel.add_summarizer(summarizer_for(k_economy, skipped, "economy"));
    wheel.schedule_periodic(k_economy, GameTime::from_hours(1), GameTime{0});
    auto body = [&](const TimerEvent& event) {
      if (event.system == k_economy) ++skipped.hours;
    };
    EventSink sink = make_sink(body);
    wheel.advance(GameTime{0}, target, 8, sink);
  }

  CHECK(played.hours == 720);
  CHECK(skipped.hours == 720);
  CHECK(played.hours == skipped.hours);
  CHECK(played.summarize_calls == 0);
  CHECK(skipped.summarize_calls == 1);
}

TEST_CASE("fast-forward: what cannot be coarsened is still delivered") {
  TimingWheel wheel;
  HourCounter economy;
  wheel.add_summarizer(summarizer_for(k_economy, economy, "economy"));
  wheel.schedule_periodic(k_economy, GameTime::from_hours(1), GameTime{0});
  // A periodic whose system registered no summarizer, and a one-shot timer. Neither has a
  // coarse form, so the budget cannot make them go away: it is a trigger, not a cap.
  wheel.schedule_periodic(k_courier, GameTime::from_days(7), GameTime{0});
  TimerPayload payload;
  payload.subject = 42;
  wheel.schedule(GameTime::from_days(3), payload);

  u32 courier = 0;
  u32 oneshot = 0;
  auto body = [&](const TimerEvent& event) {
    if (event.system == k_courier) ++courier;
    if (!event.periodic) ++oneshot;
  };
  EventSink sink = make_sink(body);

  const FastForwardResult result = wheel.advance(GameTime{0}, GameTime::from_days(30), 32, sink);
  CHECK(result.over_budget);
  CHECK(result.coarsened == 1);  // only the economy had a summarizer
  CHECK(courier == 4);           // weeks 1, 2, 3 and 4 inside thirty days
  CHECK(oneshot == 1);
  CHECK(economy.summarize_calls == 1);
}

TEST_CASE("fast-forward: a gap under the budget is exactly an ordinary advance") {
  TimingWheel wheel;
  HourCounter economy;
  wheel.add_summarizer(summarizer_for(k_economy, economy, "economy"));
  wheel.schedule_periodic(k_economy, GameTime::from_hours(1), GameTime{0});

  u32 fired = 0;
  auto body = [&](const TimerEvent&) { ++fired; };
  EventSink sink = make_sink(body);

  const FastForwardResult result = wheel.advance(GameTime{0}, GameTime::from_hours(6), 6, sink);
  CHECK_FALSE(result.over_budget);
  CHECK(result.due_estimate == 6);
  CHECK(result.delivered == 6);
  CHECK(fired == 6);
  CHECK(economy.summarize_calls == 0);
}

TEST_CASE("fast-forward: counting stops as soon as the budget is exceeded") {
  TimingWheel wheel;
  wheel.schedule_periodic(k_economy, GameTime::from_hours(1), GameTime{0});
  for (u32 i = 0; i < 1000; ++i) {
    TimerPayload payload;
    payload.subject = i;
    wheel.schedule(GameTime::from_minutes(static_cast<i64>(i) + 1), payload);
  }
  // Counting stops at the first entry that takes it past the limit, so rejecting a 10^6-event
  // gap costs no more than rejecting a 17-event one. A periodic contributes its whole firing
  // count in one step, which is why the stop point is "past the limit" and not "limit + 1".
  CHECK(wheel.count_due(GameTime::from_days(30), 16) > 16);
  CHECK(wheel.count_due(GameTime::from_days(30), 16) < 4000);
  // 60 one-shot timers in the first hour, plus the economy's firing at the hour itself.
  CHECK(wheel.count_due(GameTime::from_hours(1), 10'000) == 61);
  CHECK(wheel.count_due(GameTime{0}, 10'000) == 0);
}
