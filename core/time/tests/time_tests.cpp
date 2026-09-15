#include <core/time/time.h>

#include <doctest/doctest.h>

#include <thread>

using namespace engine;

TEST_CASE("time: monotonic clock never goes backwards and measures sleeps") {
  const i64 a = time::monotonic_ns();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  const i64 b = time::monotonic_ns();
  CHECK(b > a);
  CHECK(b - a >= 4'000'000);
  time::Stopwatch sw;
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  CHECK(sw.elapsed_ms() >= 1.5);
  CHECK(time::wall_unix_ms() > 1'600'000'000'000ll);  // after 2020
  CHECK(time::wall_unix_us() / 1000 >= time::wall_unix_ms() - 1);
}

TEST_CASE("time: GameTime arithmetic is integer and exact") {
  constexpr GameTime day = GameTime::from_days(1);
  static_assert(day.us == 86'400'000'000ll);
  static_assert(GameTime::from_minutes(90).whole_seconds() == 5400);
  static_assert((GameTime::from_seconds(1) + GameTime::from_ms(500)).seconds() == 1.5);
  static_assert(GameTime::from_hours(2) > GameTime::from_minutes(119));
  GameTime t = GameTime::from_seconds(10);
  t += GameTime::from_ms(250);
  t -= GameTime::from_us(250'000);
  CHECK(t == GameTime::from_seconds(10));
  CHECK((t * 3).whole_seconds() == 30);
  CHECK((t / 4).milliseconds() == 2500);
  CHECK(GameTime::from_days(3).whole_days() == 3);
}

TEST_CASE("time: SimTick ordering and arithmetic") {
  SimTick t;
  CHECK(t.value == 0);
  ++t;
  CHECK(t.value == 1);
  CHECK((t + 5).value == 6);
  CHECK((t + 5) - t == 5);
  CHECK(t < t + 1);
}

TEST_CASE("time: FixedStepClock produces exactly one step per step duration") {
  FixedStepClock clock(60);
  CHECK(clock.step_ns() == 16'666'666);
  CHECK_FALSE(clock.step());
  clock.advance(clock.step_ns() * 3 + 1000);
  int steps = 0;
  while (clock.step()) ++steps;
  CHECK(steps == 3);
  CHECK(clock.tick().value == 3);
  CHECK(clock.interpolation_alpha() >= 0.0f);
  CHECK(clock.interpolation_alpha() < 0.001f);
  // Sub-step advances accumulate.
  clock.advance(clock.step_ns() / 2);
  CHECK_FALSE(clock.step());
  CHECK(clock.interpolation_alpha() > 0.49f);
  CHECK(clock.interpolation_alpha() < 0.51f);
  clock.advance(clock.step_ns() / 2 + 2);
  CHECK(clock.step());
  CHECK(clock.tick().value == 4);
}

TEST_CASE("time: FixedStepClock caps the backlog and counts dropped steps") {
  FixedStepClock clock(100, 4);  // 10 ms steps, at most 4 per advance
  clock.advance(1'000'000'000);  // a one-second stall
  int steps = 0;
  while (clock.step()) ++steps;
  CHECK(steps == 4);
  CHECK(clock.dropped_steps() == 96);
  clock.reset();
  CHECK(clock.tick().value == 0);
  CHECK(clock.dropped_steps() == 0);
}

TEST_CASE("time: time scale slows or pauses the sim without changing the step") {
  FixedStepClock clock(50);
  clock.set_time_scale(0.5);
  clock.advance(clock.step_ns());  // half a step's worth after scaling
  CHECK_FALSE(clock.step());
  clock.advance(clock.step_ns());
  CHECK(clock.step());
  clock.set_time_scale(0.0);
  clock.advance(clock.step_ns() * 10);
  CHECK_FALSE(clock.step());
  clock.set_time_scale(-3.0);
  CHECK(clock.time_scale() == 0.0);
  CHECK(clock.step_ns() == 20'000'000);
}

TEST_CASE("time: GameClock advances by integer microseconds per tick") {
  FixedStepClock sim(60);
  GameClock game(sim, 60.0);  // one game-minute per real second
  CHECK(game.us_per_tick() == 1'000'000);  // 16.667 ms * 60 = 1 s of game time per tick
  for (int i = 0; i < 60; ++i) game.advance_tick();
  CHECK(game.now() == GameTime::from_minutes(1));
  game.jump_by(GameTime::from_hours(8));
  CHECK(game.now() == GameTime::from_minutes(1) + GameTime::from_hours(8));
  game.jump_to(GameTime{});
  game.set_rate(1.0);
  game.advance_ticks(120);
  CHECK(game.now().us == 120 * 16'667);
  // Same tick sequence, same result, regardless of how real time was delivered.
  FixedStepClock sim2(60);
  GameClock game2(sim2, 1.0);
  game2.advance_ticks(120);
  CHECK(game2.now() == game.now());
}
