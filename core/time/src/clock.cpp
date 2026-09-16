#include <core/time/time.h>

#include <chrono>
#include <cmath>

namespace engine {

namespace time {

i64 monotonic_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

i64 wall_unix_ms() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

i64 wall_unix_us() noexcept {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace time

FixedStepClock::FixedStepClock(u32 hz, u32 max_steps_per_advance) noexcept
    : hz_(hz == 0 ? 60 : hz),
      max_steps_(max_steps_per_advance == 0 ? 1 : max_steps_per_advance),
      step_ns_(1'000'000'000ll / hz_) {}

void FixedStepClock::advance(i64 real_ns) noexcept {
  if (real_ns <= 0) return;
  const i64 scaled = static_cast<i64>(static_cast<f64>(real_ns) * time_scale_);
  accumulator_ns_ += scaled;
  // Clamp the backlog to the step cap; anything beyond is dropped, not simulated later.
  const i64 cap = step_ns_ * static_cast<i64>(max_steps_);
  if (accumulator_ns_ > cap) {
    dropped_steps_ += static_cast<u64>((accumulator_ns_ - cap) / step_ns_);
    accumulator_ns_ = cap;
  }
}

bool FixedStepClock::step() noexcept {
  if (accumulator_ns_ < step_ns_) return false;
  accumulator_ns_ -= step_ns_;
  ++tick_;
  return true;
}

void FixedStepClock::reset() noexcept {
  accumulator_ns_ = 0;
  dropped_steps_ = 0;
  tick_ = SimTick{};
}

f32 FixedStepClock::interpolation_alpha() const noexcept {
  return static_cast<f32>(static_cast<f64>(accumulator_ns_) / static_cast<f64>(step_ns_));
}

GameClock::GameClock(const FixedStepClock& sim, f64 game_seconds_per_real_second) noexcept
    : step_ns_(sim.step_ns()) {
  set_rate(game_seconds_per_real_second);
}

void GameClock::set_rate(f64 game_seconds_per_real_second) noexcept {
  const f64 rate = game_seconds_per_real_second < 0.0 ? 0.0 : game_seconds_per_real_second;
  // Integer microseconds per tick: the only place floating point enters, rounded once.
  us_per_tick_ = static_cast<i64>(std::llround(static_cast<f64>(step_ns_) / 1000.0 * rate));
}

}  // namespace engine
