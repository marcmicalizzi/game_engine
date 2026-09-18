// The timing wheel itself (docs/plan/05-simulation.md §5.3): ordering, cancellation, cascading
// between levels, periodics, the far list, and bit-for-bit repeatability.

#include <core/containers/vector.h>
#include <domain/sim/timing_wheel.h>

#include <doctest/doctest.h>

#include <cstring>

using namespace engine;
using namespace engine::sim;

namespace {

// What a run of the wheel produced, in a form two runs can be compared with memcmp.
struct Record {
  i64 at = 0;
  u64 subject = 0;
  u64 sequence = 0;
  u32 kind = 0;
  u32 pad = 0;
};

struct Recorder {
  Vector<Record> events;

  void operator()(const TimerEvent& event) {
    events.push_back(
        Record{event.at.us, event.payload.subject, event.sequence, event.payload.kind, 0u});
  }
};

TimerPayload payload_of(u64 subject, u32 kind) {
  TimerPayload payload;
  payload.subject = subject;
  payload.kind = kind;
  return payload;
}

bool same_bytes(const Vector<Record>& a, const Vector<Record>& b) {
  if (a.size() != b.size()) return false;
  if (a.empty()) return true;
  return std::memcmp(a.data(), b.data(), static_cast<usize>(a.size()) * sizeof(Record)) == 0;
}

}  // namespace

TEST_CASE("timing wheel: the level ladder is ticks, seconds, minutes, hours and days") {
  TimingWheel wheel;
  CHECK(TimingWheel::level_count() == 5);
  CHECK(wheel.step().us == 16'667);
  CHECK(wheel.slots(0) == 60);  // 16,667 us rounds to 60 ticks a second, not 61
  // Each level's resolution is an integer multiple of the level below, which is what makes
  // cascading exact; the names are approximate because 60 Hz is not a divisor of a second.
  for (u32 level = 1; level < TimingWheel::level_count(); ++level) {
    CHECK(wheel.resolution(level).us ==
          wheel.resolution(level - 1).us * static_cast<i64>(wheel.slots(level - 1)));
  }
  CHECK(wheel.resolution(1).us == 1'000'020);       // about a second, 20 us over
  CHECK(wheel.resolution(4).us == 86'401'728'000);  // about a day, 0.002% over one
  CHECK(wheel.horizon().whole_days() >= 1000);      // the top level reaches years ahead
  CHECK(wheel.validate());
}

TEST_CASE("timing wheel: events come out in due order with ties in insertion order") {
  TimingWheel wheel;
  Recorder recorder;
  EventSink sink = make_sink(recorder);

  // Scheduled out of order and at the same instant, so both halves of the rule are exercised.
  wheel.schedule(GameTime::from_seconds(5), payload_of(50, 1));
  wheel.schedule(GameTime::from_seconds(1), payload_of(10, 1));
  wheel.schedule(GameTime::from_seconds(1), payload_of(11, 2));  // same time, later insert
  wheel.schedule(GameTime::from_seconds(3), payload_of(30, 1));
  wheel.schedule(GameTime::from_seconds(1), payload_of(12, 3));  // same time, later still

  wheel.advance(GameTime::from_seconds(10), sink);
  REQUIRE(recorder.events.size() == 5);
  CHECK(recorder.events[0].subject == 10);
  CHECK(recorder.events[1].subject == 11);
  CHECK(recorder.events[2].subject == 12);
  CHECK(recorder.events[3].subject == 30);
  CHECK(recorder.events[4].subject == 50);
  CHECK(wheel.live_count() == 0);
  CHECK(wheel.now().us == GameTime::from_seconds(10).us);
  CHECK(wheel.validate());
}

TEST_CASE("timing wheel: cancel is O(1) and a stale handle is refused") {
  TimingWheel wheel;
  Recorder recorder;
  EventSink sink = make_sink(recorder);

  const TimerHandle keep = wheel.schedule(GameTime::from_seconds(1), payload_of(1, 0));
  const TimerHandle drop = wheel.schedule(GameTime::from_seconds(2), payload_of(2, 0));
  CHECK(wheel.live_count() == 2);
  CHECK(wheel.is_live(drop));
  CHECK(wheel.cancel(drop));
  CHECK_FALSE(wheel.is_live(drop));
  CHECK_FALSE(wheel.cancel(drop));  // cancelling twice is not an error, it is a no-op
  CHECK(wheel.live_count() == 1);

  // A fresh timer reuses the freed slot; the old handle must not reach it.
  const TimerHandle reused = wheel.schedule(GameTime::from_seconds(3), payload_of(3, 0));
  CHECK(reused.index == drop.index);
  CHECK(reused.generation != drop.generation);
  CHECK_FALSE(wheel.cancel(drop));
  CHECK(wheel.is_live(reused));

  CHECK_FALSE(wheel.cancel(TimerHandle{}));  // the null handle

  wheel.advance(GameTime::from_seconds(5), sink);
  REQUIRE(recorder.events.size() == 2);
  CHECK(recorder.events[0].subject == 1);
  CHECK(recorder.events[1].subject == 3);
  CHECK_FALSE(wheel.is_live(keep));
}

TEST_CASE("timing wheel: timers cascade down the levels and still arrive in order") {
  TimingWheel wheel;
  Recorder recorder;
  EventSink sink = make_sink(recorder);

  // One per level: inside the current tick block, a second out, a minute, an hour, a day.
  wheel.schedule(wheel.step(), payload_of(0, 0));
  wheel.schedule(GameTime::from_seconds(2), payload_of(1, 0));
  wheel.schedule(GameTime::from_minutes(2), payload_of(2, 0));
  wheel.schedule(GameTime::from_hours(2), payload_of(3, 0));
  wheel.schedule(GameTime::from_days(2), payload_of(4, 0));
  CHECK(wheel.live_count() == 5);
  CHECK(wheel.validate());

  // Advance in small steps so every level has to hand its contents down rather than being
  // jumped over in one move.
  for (i64 minute = 1; minute <= 60 * 49; ++minute) {
    wheel.advance(GameTime::from_minutes(minute), sink);
    REQUIRE(wheel.validate());
  }
  REQUIRE(recorder.events.size() == 5);
  for (u32 i = 0; i < recorder.events.size(); ++i)
    CHECK(recorder.events[i].subject == i);
  CHECK(wheel.live_count() == 0);
}

TEST_CASE("timing wheel: a jump over a game month costs the same as a jump over a tick") {
  TimingWheel wheel;
  Recorder recorder;
  EventSink sink = make_sink(recorder);
  wheel.schedule(GameTime::from_days(29), payload_of(99, 7));
  // No intermediate advances at all: the skip search has to find one timer 29 days out.
  wheel.advance(GameTime::from_days(30), sink);
  REQUIRE(recorder.events.size() == 1);
  CHECK(recorder.events[0].at == GameTime::from_days(29).us);
  CHECK(recorder.events[0].kind == 7);
  CHECK(wheel.validate());
}

TEST_CASE("timing wheel: a periodic fires on its phase and keeps its handle") {
  TimingWheel wheel;
  Recorder recorder;
  EventSink sink = make_sink(recorder);

  // Registered at 09:13, phase 0: the first firing is 10:00 and not 10:13.
  wheel.reset(GameTime::from_minutes(9 * 60 + 13));
  const TimerHandle hourly = wheel.schedule_periodic(4, GameTime::from_hours(1), GameTime{0});
  wheel.advance(GameTime::from_minutes(12 * 60 + 59), sink);
  REQUIRE(recorder.events.size() == 3);
  CHECK(recorder.events[0].at == GameTime::from_hours(10).us);
  CHECK(recorder.events[1].at == GameTime::from_hours(11).us);
  CHECK(recorder.events[2].at == GameTime::from_hours(12).us);
  CHECK(wheel.is_live(hourly));  // the handle survives every firing

  GameTime next;
  REQUIRE(wheel.due_time(hourly, next));
  CHECK(next.us == GameTime::from_hours(13).us);

  CHECK(wheel.cancel(hourly));
  recorder.events.clear();
  wheel.advance(GameTime::from_hours(24), sink);
  CHECK(recorder.events.empty());
  CHECK(wheel.live_count() == 0);
}

TEST_CASE("timing wheel: a timer past the top level's horizon is kept, not dropped") {
  TimingWheel wheel;
  Recorder recorder;
  EventSink sink = make_sink(recorder);

  const GameTime far = GameTime::from_days(2000);  // the top level reaches about 1024 days
  CHECK(wheel.horizon() < far);
  wheel.schedule(far, payload_of(1234, 5));
  CHECK(wheel.far_count() == 1);
  CHECK(wheel.validate());

  wheel.advance(GameTime::from_days(1500), sink);  // past the horizon, before the timer
  CHECK(recorder.events.empty());
  CHECK(wheel.live_count() == 1);

  wheel.advance(GameTime::from_days(2001), sink);
  REQUIRE(recorder.events.size() == 1);
  CHECK(recorder.events[0].subject == 1234);
  CHECK(wheel.far_count() == 0);
}

TEST_CASE("timing wheel: a timer scheduled in the past is due immediately") {
  TimingWheel wheel;
  Recorder recorder;
  EventSink sink = make_sink(recorder);
  wheel.reset(GameTime::from_hours(5));
  wheel.schedule(GameTime::from_hours(1), payload_of(7, 0));
  wheel.advance(GameTime::from_hours(5) + wheel.step(), sink);
  REQUIRE(recorder.events.size() == 1);
  CHECK(recorder.events[0].at == GameTime::from_hours(5).us);
}

TEST_CASE("timing wheel: a sink may schedule and cancel while it is being delivered to") {
  TimingWheel wheel;
  Vector<i64> seen;
  TimerHandle doomed = wheel.schedule(GameTime::from_seconds(2), payload_of(2, 0));
  bool armed = false;

  auto body = [&](const TimerEvent& event) {
    seen.push_back(event.at.us);
    if (!armed) {
      armed = true;
      wheel.cancel(doomed);                                         // a later event in this batch
      wheel.schedule(GameTime::from_seconds(3), payload_of(3, 0));  // still inside the window
    }
  };
  EventSink sink = make_sink(body);
  wheel.schedule(GameTime::from_seconds(1), payload_of(1, 0));
  wheel.advance(GameTime::from_seconds(5), sink);

  REQUIRE(seen.size() == 2);
  CHECK(seen[0] == GameTime::from_seconds(1).us);
  CHECK(seen[1] == GameTime::from_seconds(3).us);
  CHECK(wheel.validate());
}

TEST_CASE("timing wheel: two runs of the same schedule produce byte-identical output") {
  auto run = [](Vector<Record>& out, u32 steps) {
    TimingWheel wheel;
    Recorder recorder;
    EventSink sink = make_sink(recorder);
    // A deterministic spread over five levels plus a few periodics and a cancellation.
    u64 state = 0x9E3779B97F4A7C15ull;
    Vector<TimerHandle> handles;
    for (u32 i = 0; i < 4000; ++i) {
      state = state * 6364136223846793005ull + 1442695040888963407ull;
      const i64 at = static_cast<i64>((state >> 11) % static_cast<u64>(GameTime::from_days(3).us));
      handles.push_back(wheel.schedule(GameTime{at}, payload_of(i, i % 7u)));
    }
    for (u16 system = 0; system < 4; ++system) {
      wheel.schedule_periodic(system, GameTime::from_hours(6), GameTime::from_minutes(system * 7));
    }
    for (u32 i = 0; i < handles.size(); i += 3)
      wheel.cancel(handles[i]);
    // The same total interval reached in a different number of advances: the wheel's output
    // must depend on the times, not on how the caller walked to them.
    const i64 total = GameTime::from_days(3).us;
    for (u32 s = 1; s <= steps; ++s) {
      wheel.advance(GameTime{total * static_cast<i64>(s) / static_cast<i64>(steps)}, sink);
    }
    out = recorder.events;
  };

  Vector<Record> a;
  Vector<Record> b;
  Vector<Record> c;
  run(a, 1);
  run(b, 1);
  run(c, 997);
  CHECK(a.size() > 2000);
  CHECK(same_bytes(a, b));
  CHECK(same_bytes(a, c));
}
