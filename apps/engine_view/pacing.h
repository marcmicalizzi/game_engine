#pragma once

// What the window title says about frame pacing while a session is flown: the last frame's
// milliseconds and the 99th percentile over the last second (docs/subsystems/apps.md,
// "`--interactive`"). There is no UI module yet, and the title is the one piece of text a window
// has that nobody has to build a renderer pass for.
//
// Fixed storage and no allocation: it runs every frame. The percentile is the nearest-rank one
// over the frames that ended inside the window, found with `nth_element` on a scratch copy, so a
// frame costs a copy of at most `k_capacity` floats and only when the title is actually rewritten.

#include <core/base/types.h>

#include <algorithm>

namespace engine::view {

class FramePacing {
 public:
  // More frames than a second holds at any rate a person will see; past it, the window is the
  // most recent `k_capacity` frames, which at that rate is under a second anyway.
  static constexpr u32 k_capacity = 2048;

  // A frame that ended at `now_ns` and took `ms`.
  void add(i64 now_ns, f32 ms) noexcept {
    const u32 slot = (head_ + count_) % k_capacity;
    if (count_ < k_capacity) {
      ++count_;
    } else {
      head_ = (head_ + 1) % k_capacity;
    }
    at_[slot] = now_ns;
    ms_[slot] = ms;
    last_ = ms;
  }
  f32 last_ms() const noexcept { return last_; }
  // How many frames ended within `window_ns` of `now_ns`.
  u32 in_window(i64 now_ns, i64 window_ns) const noexcept {
    u32 n = 0;
    for (u32 i = 0; i < count_; ++i) {
      if (now_ns - at_[(head_ + i) % k_capacity] <= window_ns) ++n;
    }
    return n;
  }
  // The nearest-rank `p`-th percentile (0 < p <= 1) of the frames that ended within `window_ns`
  // of `now_ns`; 0 when there are none.
  f32 percentile(i64 now_ns, i64 window_ns, f64 p) const noexcept {
    u32 n = 0;
    for (u32 i = 0; i < count_; ++i) {
      const u32 slot = (head_ + i) % k_capacity;
      if (now_ns - at_[slot] <= window_ns) scratch_[n++] = ms_[slot];
    }
    if (n == 0) return 0.0f;
    // Nearest rank: the smallest value with at least p of the frames at or below it.
    u32 rank = static_cast<u32>(p * static_cast<f64>(n) + 0.999999);
    rank = rank < 1 ? 1 : (rank > n ? n : rank);
    std::nth_element(scratch_, scratch_ + (rank - 1), scratch_ + n);
    return scratch_[rank - 1];
  }

 private:
  i64 at_[k_capacity] = {};
  f32 ms_[k_capacity] = {};
  mutable f32 scratch_[k_capacity] = {};
  u32 head_ = 0;
  u32 count_ = 0;
  f32 last_ = 0.0f;
};

}  // namespace engine::view
