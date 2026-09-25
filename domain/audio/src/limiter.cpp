#include <domain/audio/limiter.h>

#include <algorithm>
#include <cmath>

namespace engine::audio {

namespace {

// The box filter's unit: a deficit 1 - h is counted in steps of 2^-24, the resolution of an f32
// just below 1, so the integer sum loses nothing a gain could have said.
constexpr f32 k_deficit_unit = 16777216.0f;  // 2^24

// A reduction smaller than this is none: 2^-16 of gain is 0.00013 dB, and the step of letting it go
// is 96 dB under the signal, below a 16-bit output's least significant bit. Without it the one-pole
// would creep towards zero for ever — stalling a few thousand ulps under 1, where each step rounds
// to nothing — the limiter would never be at rest, and material under the ceiling would never again
// leave it bit for bit. From a full reduction it is reached in about eleven release times.
constexpr f32 k_no_reduction = 1.0f / 65536.0f;

}  // namespace

void Limiter::configure(u32 channels, f32 ceiling, u32 release_frames) {
  channels_ = channels;
  ceiling_ = ceiling > 0.0f && ceiling <= 1.0f ? ceiling : 1.0f;
  release_frames_ = release_frames != 0 ? release_frames : 1u;
  release_ = 1.0f - 1.0f / static_cast<f32>(release_frames_);
  deficit_scale_ = 1.0 / (static_cast<f64>(k_window) * static_cast<f64>(k_deficit_unit));
  delay_.resize_exact(k_lookahead_frames * channels, 0.0f);
  held_gain_.resize_exact(k_window, 1.0f);
  held_frame_.resize_exact(k_window, 0u);
  deficit_.resize_exact(k_window, 0u);
  deficit_sum_ = 0;
  limited_frames_ = 0;
  reduction_ = 0.0f;
  last_gain_ = 1.0f;
  frame_ = 0;
  delay_at_ = 0;
  head_ = 0;
  held_ = 0;
  deficit_at_ = 0;
}

// At rest and under the ceiling the gain is 1 on every frame, so the output is the input
// `k_lookahead_frames` later: swap the block through the delay line, a contiguous run at a time.
// Bit-identical to the per-frame path in the same state (which multiplies by 1 and clamps nothing).
void Limiter::delay_only(f32* io, u32 frames) noexcept {
  const u32 channels = channels_;
  f32* delay = delay_.data();
  for (u32 i = 0; i < frames;) {
    const u32 room = k_lookahead_frames - delay_at_;
    const u32 run = frames - i < room ? frames - i : room;
    std::swap_ranges(io + static_cast<usize>(i) * channels,
                     io + static_cast<usize>(i + run) * channels,
                     delay + static_cast<usize>(delay_at_) * channels);
    i += run;
    delay_at_ += run;
    if (delay_at_ == k_lookahead_frames) delay_at_ = 0;
  }
}

void Limiter::process(f32* io, u32 frames, f32 block_peak) noexcept {
  if (channels_ == 0 || frames == 0) return;
  if (at_rest() && block_peak <= ceiling_) {
    delay_only(io, frames);
    frame_ += frames;
    last_gain_ = 1.0f;
    return;
  }

  const u32 channels = channels_;
  const f32 ceiling = ceiling_;
  const f32 release = release_;
  f32* delay = delay_.data();
  f32* held_gain = held_gain_.data();
  u32* held_frame = held_frame_.data();
  u32* deficit = deficit_.data();
  f32 lowest = 1.0f;
  u64 limited = 0;

  for (u32 i = 0; i < frames; ++i) {
    f32* x = io + static_cast<usize>(i) * channels;

    // 1. The gain this frame needs: 1 at or under the ceiling, exactly (ceiling / ceiling).
    f32 peak = 0.0f;
    for (u32 c = 0; c < channels; ++c) {
      const f32 m = std::fabs(x[c]);
      peak = m > peak ? m : peak;
    }
    const f32 required = ceiling / (peak > ceiling ? peak : ceiling);

    // 2. The hold: the smallest required gain of the last k_window frames, as a monotonic queue.
    //    The oldest entry leaves when it falls out of the window (at most one a frame, since every
    //    entry is a different frame); a new one first removes every entry it is no larger than,
    //    which can never be the minimum again while it is in the window.
    const u32 now = frame_++;
    if (held_ != 0 && now - held_frame[head_] >= k_window) {
      head_ = head_ + 1u == k_window ? 0u : head_ + 1u;
      --held_;
    }
    if (required < 1.0f) {
      while (held_ != 0) {
        const u32 back = head_ + held_ - 1u;
        const u32 at = back >= k_window ? back - k_window : back;
        if (held_gain[at] < required) break;
        --held_;
      }
      const u32 slot = head_ + held_;
      const u32 at = slot >= k_window ? slot - k_window : slot;
      held_gain[at] = required;
      held_frame[at] = now;
      ++held_;
    }
    const f32 hold = held_ != 0 ? held_gain[head_] : 1.0f;

    // 3. The attack: the mean of the window's holds, as a sum of integer deficits (rounded up, so
    //    the gain errs low) that cannot drift however long it runs.
    const u32 d = static_cast<u32>(std::ceil((1.0f - hold) * k_deficit_unit));
    deficit_sum_ += d;
    deficit_sum_ -= deficit[deficit_at_];
    deficit[deficit_at_] = d;
    deficit_at_ = deficit_at_ + 1u == k_window ? 0u : deficit_at_ + 1u;
    const f32 target = static_cast<f32>(static_cast<f64>(deficit_sum_) * deficit_scale_);

    // 4. The release: a deeper reduction is taken at once (the box filter has already shaped it);
    //    a shallower one is approached by a one-pole of `release_frames`, and one too small to move
    //    the gain off 1 is none.
    const f32 released = target + (reduction_ - target) * release;
    f32 reduction = target >= reduction_ ? target : released;
    reduction = reduction < k_no_reduction ? 0.0f : reduction;
    reduction_ = reduction;
    const f32 gain = 1.0f - reduction;
    lowest = gain < lowest ? gain : lowest;
    limited += reduction > 0.0f ? 1u : 0u;

    // 5. The frame from k_lookahead_frames ago, at this gain, held to the ceiling against the
    //    rounding of the gain's arithmetic.
    f32* line = delay + static_cast<usize>(delay_at_) * channels;
    for (u32 c = 0; c < channels; ++c) {
      const f32 y = line[c] * gain;
      line[c] = x[c];
      x[c] = y < -ceiling ? -ceiling : (y > ceiling ? ceiling : y);
    }
    delay_at_ = delay_at_ + 1u == k_lookahead_frames ? 0u : delay_at_ + 1u;
  }
  limited_frames_ += limited;
  last_gain_ = lowest;
}

}  // namespace engine::audio
