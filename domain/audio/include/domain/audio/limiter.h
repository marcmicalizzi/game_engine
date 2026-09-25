#pragma once

// The master's look-ahead peak limiter (docs/subsystems/audio.md, "The master").
//
// A brickwall limiter: no sample leaves it above its ceiling, and material that never reaches the
// ceiling leaves it unchanged — bit for bit, only later by the look-ahead. It sits after the
// decode stage and before the master's counted hard clip, which it keeps from ever firing, and it
// is **off by default** (`audio.limiter`): the page's policy is that a mix too hot for its headroom
// is said so by a counter rather than hidden, and a limiter also delays every sound by its
// look-ahead. A game whose content cannot leave headroom turns it on; `MixerStats::limited_frames`
// then says how often it had to work.
//
// ---- how it works, per frame --------------------------------------------------------------------
//
//   1. required gain   r = ceiling / max(peak across channels, ceiling)       (1 when under)
//   2. hold            h = the smallest r of the last `k_window` input frames
//   3. attack          s = the mean of the last `k_window` h's                 (a box filter)
//   4. release         the reduction 1 - s is taken at once when it deepens, and let go of by
//                      a one-pole of `release` frames when it lessens
//   5. output          the input `k_lookahead_frames` ago, times the gain, clamped to the ceiling
//
// A peak entering at frame p makes every h in [p, p + window) no larger than its r, so their mean
// — the gain on frame p + window - 1, which is when the delayed peak comes out — is no larger than
// r either: the gain is fully down when the peak arrives. **The attack is instantaneous in the
// sense a brickwall's is** — there is no attack time to set and nothing overshoots — and the box
// filter shapes it as a linear ramp across the look-ahead rather than a step at its start, because
// a step in gain is the click the fixed-time ramps (decoder.h) exist to avoid. The final clamp
// only ever moves a sample by the rounding of the gain's arithmetic.
//
// ---- determinism --------------------------------------------------------------------------------
//
// The box filter sums its deficits as integers (1 - h in units of 2^-24, rounded up so the gain
// errs low), so the running sum never drifts however long it runs; the rest is IEEE-754 f32 and
// f64 arithmetic with contraction off and `ceil`, which is exact everywhere; the release
// coefficient is 1 / release_frames, not an exponential, so nothing calls the C library's
// transcendentals. Everything is per frame and carried across calls, so the output does not
// depend on how frames are divided into blocks. The ceiling is an input: the mixer turns the dB
// tunable into it with the C library's `pow`, so a test that pins limited output hands the
// ceiling over as a linear value.

#include <core/base/types.h>
#include <core/containers/vector.h>

namespace engine::audio {

class Limiter {
 public:
  // 5 ms: the shortest ramp that is not heard as a click (the same threshold `audio.ramp_ms` sits
  // at the top of), which is what the attack needs; and every frame of look-ahead is a frame of
  // latency on every sound, so it is no longer than that. The hold and the box filter span one
  // frame more than the delay, which is what puts the whole attack in front of the peak.
  static constexpr u32 k_lookahead_frames = 240;
  static constexpr u32 k_window = k_lookahead_frames + 1;

  Limiter() = default;

  // Sizes the delay line for `channels` and sets the ceiling (linear, (0, 1]) and the release (in
  // frames, >= 1). Allocates: the controlling thread, before the first `process`. A limiter never
  // configured, or configured with 0 channels, is off and `process` returns at once.
  void configure(u32 channels, f32 ceiling, u32 release_frames);

  bool enabled() const noexcept { return channels_ != 0; }
  f32 ceiling() const noexcept { return ceiling_; }
  u32 release_frames() const noexcept { return release_frames_; }
  // Frames every sound is delayed by while the limiter is on; 0 when it is off.
  u32 latency_frames() const noexcept { return enabled() ? k_lookahead_frames : 0u; }

  // Limits `frames` interleaved frames of `channels` in place. `block_peak` is the largest |sample|
  // among them, which the caller has already measured: a limiter at rest given a block under its
  // ceiling only delays it. The audio thread: no allocation, no lock, no log.
  void process(f32* io, u32 frames, f32 block_peak) noexcept;

  // Frames whose gain was below 1, since configuration.
  u64 limited_frames() const noexcept { return limited_frames_; }
  // The lowest gain of the last `process` call: 1 when it did nothing.
  f32 last_gain() const noexcept { return last_gain_; }

 private:
  bool at_rest() const noexcept { return held_ == 0 && deficit_sum_ == 0 && reduction_ == 0.0f; }
  void delay_only(f32* io, u32 frames) noexcept;

  // The delay line: `k_lookahead_frames` frames of `channels_`, a ring at `delay_at_`.
  Vector<f32> delay_;
  // The hold's sliding minimum: a monotonic queue of (required gain, input frame), increasing
  // from `head_`, `held_` long, in a ring of `k_window`. Only gains below 1 are entered, so an
  // empty queue is a hold of 1.
  Vector<f32> held_gain_;
  Vector<u32> held_frame_;
  // The box filter's window of deficits, in units of 2^-24, a ring at `deficit_at_`.
  Vector<u32> deficit_;
  u64 deficit_sum_ = 0;
  u64 limited_frames_ = 0;
  f64 deficit_scale_ = 0.0;  // 1 / (k_window * 2^24)
  f32 ceiling_ = 1.0f;
  f32 release_ = 1.0f;    // the one-pole's decay per frame, 1 - 1 / release_frames
  f32 reduction_ = 0.0f;  // 1 - the gain
  f32 last_gain_ = 1.0f;
  u32 channels_ = 0;
  u32 release_frames_ = 0;
  u32 frame_ = 0;  // input frames seen, wrapping; the hold compares with unsigned subtraction
  u32 delay_at_ = 0;
  u32 head_ = 0;
  u32 held_ = 0;
  u32 deficit_at_ = 0;
};

}  // namespace engine::audio
