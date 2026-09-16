#pragma once

// Time model (docs/plan/02-architecture.md §2.4, ADR-0010, ADR-0017).
//
//   wall clock  -> frame pacing -> render frame (variable dt, never affects the sim)
//   sim clock   -> fixed step   -> SimTick (deterministic, replayable)
//   game clock  -> GameTime     -> scheduler events (integer microseconds, scaled per tick)
//
// Tick counters and game time are 64-bit integers so precision does not degrade over long
// sessions; floating point appears only in per-frame deltas and interpolation factors.

#include <core/base/types.h>

#include <compare>

namespace engine {

namespace time {

// Steady, monotonic nanoseconds since an arbitrary origin. Never goes backwards.
i64 monotonic_ns() noexcept;
// Unix epoch time from the system clock; may jump when the user adjusts the clock.
i64 wall_unix_ms() noexcept;
i64 wall_unix_us() noexcept;

class Stopwatch {
 public:
  Stopwatch() noexcept : start_(monotonic_ns()) {}
  void reset() noexcept { start_ = monotonic_ns(); }
  i64 elapsed_ns() const noexcept { return monotonic_ns() - start_; }
  f64 elapsed_ms() const noexcept { return static_cast<f64>(elapsed_ns()) / 1.0e6; }
  f64 elapsed_s() const noexcept { return static_cast<f64>(elapsed_ns()) / 1.0e9; }

 private:
  i64 start_;
};

}  // namespace time

// One fixed simulation step. Monotonic within a session; replay compares tick for tick.
struct SimTick {
  u64 value = 0;

  constexpr auto operator<=>(const SimTick&) const noexcept = default;
  constexpr SimTick& operator++() noexcept {
    ++value;
    return *this;
  }
  constexpr SimTick operator+(u64 n) const noexcept { return SimTick{value + n}; }
  constexpr u64 operator-(SimTick other) const noexcept { return value - other.value; }
};

// Game time as integer microseconds. Serves as both a point (since the world's epoch) and a
// duration; ±292,000 years of range.
struct GameTime {
  i64 us = 0;

  static constexpr GameTime from_us(i64 v) noexcept { return GameTime{v}; }
  static constexpr GameTime from_ms(i64 v) noexcept { return GameTime{v * 1000}; }
  static constexpr GameTime from_seconds(i64 v) noexcept { return GameTime{v * 1'000'000}; }
  static constexpr GameTime from_minutes(i64 v) noexcept { return from_seconds(v * 60); }
  static constexpr GameTime from_hours(i64 v) noexcept { return from_minutes(v * 60); }
  static constexpr GameTime from_days(i64 v) noexcept { return from_hours(v * 24); }
  static constexpr GameTime from_seconds_f(f64 v) noexcept {
    return GameTime{static_cast<i64>(v * 1.0e6)};
  }

  constexpr i64 milliseconds() const noexcept { return us / 1000; }
  constexpr i64 whole_seconds() const noexcept { return us / 1'000'000; }
  constexpr f64 seconds() const noexcept { return static_cast<f64>(us) / 1.0e6; }
  constexpr i64 whole_days() const noexcept { return us / (86'400ll * 1'000'000ll); }

  constexpr auto operator<=>(const GameTime&) const noexcept = default;
  constexpr GameTime operator+(GameTime o) const noexcept { return GameTime{us + o.us}; }
  constexpr GameTime operator-(GameTime o) const noexcept { return GameTime{us - o.us}; }
  constexpr GameTime& operator+=(GameTime o) noexcept {
    us += o.us;
    return *this;
  }
  constexpr GameTime& operator-=(GameTime o) noexcept {
    us -= o.us;
    return *this;
  }
  constexpr GameTime operator*(i64 k) const noexcept { return GameTime{us * k}; }
  constexpr GameTime operator/(i64 k) const noexcept { return GameTime{us / k}; }
};

// Accumulates real time and hands out fixed steps. The classic fixed-timestep loop:
//
//   clock.advance(frame_dt_ns);
//   while (clock.step()) simulate(clock.tick());
//   render(clock.interpolation_alpha());
//
// A cap on steps per advance prevents the spiral of death after a stall; dropped steps are
// counted so telemetry can see them.
class FixedStepClock {
 public:
  explicit FixedStepClock(u32 hz = 60, u32 max_steps_per_advance = 8) noexcept;

  void advance(i64 real_ns) noexcept;  // scaled by time_scale before accumulating
  bool step() noexcept;                // consumes one step if available; increments the tick
  void reset() noexcept;

  SimTick tick() const noexcept { return tick_; }
  u32 hz() const noexcept { return hz_; }
  i64 step_ns() const noexcept { return step_ns_; }
  f64 step_seconds() const noexcept { return static_cast<f64>(step_ns_) / 1.0e9; }
  // Fraction of the next step already accumulated, in [0, 1): use to interpolate render state.
  f32 interpolation_alpha() const noexcept;
  u64 dropped_steps() const noexcept { return dropped_steps_; }

  // 1.0 is real time; 0.0 pauses; 2.0 runs the sim at double speed. Does not change step size.
  void set_time_scale(f64 scale) noexcept { time_scale_ = scale < 0.0 ? 0.0 : scale; }
  f64 time_scale() const noexcept { return time_scale_; }

 private:
  u32 hz_;
  u32 max_steps_;
  i64 step_ns_;
  i64 accumulator_ns_ = 0;
  u64 dropped_steps_ = 0;
  f64 time_scale_ = 1.0;
  SimTick tick_;
};

// Game time advanced by whole ticks at an integer rate, so every machine computes the same
// GameTime for the same tick sequence. Fast-forward is an explicit jump.
class GameClock {
 public:
  explicit GameClock(const FixedStepClock& sim, f64 game_seconds_per_real_second = 1.0) noexcept;

  void set_rate(f64 game_seconds_per_real_second) noexcept;
  i64 us_per_tick() const noexcept { return us_per_tick_; }
  GameTime now() const noexcept { return now_; }

  void advance_tick() noexcept { now_ += GameTime{us_per_tick_}; }
  void advance_ticks(u64 n) noexcept { now_ += GameTime{us_per_tick_ * static_cast<i64>(n)}; }
  void jump_to(GameTime t) noexcept { now_ = t; }
  void jump_by(GameTime dt) noexcept { now_ += dt; }

 private:
  i64 step_ns_;
  i64 us_per_tick_;
  GameTime now_;
};

}  // namespace engine
