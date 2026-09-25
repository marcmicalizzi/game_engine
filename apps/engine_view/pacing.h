#pragma once

// A window's frame pacing (docs/subsystems/apps.md, "Pacing"), in the pieces that need no GPU and
// no window and so are tested with made-up numbers (tests/fly_tests.cpp): `--present`'s names,
// the display times a measured run keeps, and what the window title
// says while a session is flown — the last frame's milliseconds and the 99th percentile over the
// last second. There is no UI module yet, and the title is the one piece of text a window has that
// nobody has to build a renderer pass for.
//
// The per-frame pieces are fixed storage and allocate nothing. The title's percentile is the
// nearest-rank one over the frames that ended inside the window, found with `nth_element` on a
// scratch copy, so a frame costs a copy of at most `k_capacity` floats and only when the title is
// actually rewritten.

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <algorithm>
#include <string_view>

namespace engine::view {

// The present modes `--present` names, in engine-view's own words so this header (and its tests)
// need no Vulkan; main.cpp maps them onto VkPresentModeKHR.
enum class PresentMode : u8 { Fifo, FifoRelaxed, Mailbox, Immediate, FifoLatestReady };

// `--present`'s spelling, with hyphens; the summary writes the same names with underscores.
inline bool parse_present_mode(std::string_view text, PresentMode* out) noexcept {
  struct Name {
    std::string_view text;
    PresentMode mode;
  };
  static constexpr Name k_names[] = {{"fifo", PresentMode::Fifo},
                                     {"fifo-relaxed", PresentMode::FifoRelaxed},
                                     {"mailbox", PresentMode::Mailbox},
                                     {"immediate", PresentMode::Immediate},
                                     {"fifo-latest-ready", PresentMode::FifoLatestReady}};
  for (const Name& name : k_names) {
    if (name.text == text) {
      if (out != nullptr) *out = name.mode;
      return true;
    }
  }
  return false;
}

// When each presented frame reached the display, by present id (the swapchain's first-pixel-out
// time on time::monotonic_ns()'s clock), and when it sampled its input. A windowed `--benchmark`
// keeps both for the whole run and writes each record's `shown_ms` and `latency_ms` at its end:
// display times are read once, then, because reading them blocks for a refresh on this project's
// driver (gfx::Swapchain::poll_timings). Grows with the run, like the records it serves; reserve
// what a session is expected to present.
class DisplayTimes {
 public:
  void reserve(u32 n) {
    shown_.reserve(n);
    sampled_.reserve(n);
  }
  void sampled(u64 id, i64 ns) {
    grow(id);
    sampled_[static_cast<u32>(id)] = ns;
  }
  void shown(u64 id, i64 ns) {
    grow(id);
    if (shown_[static_cast<u32>(id)] == 0) ++reported_;
    shown_[static_cast<u32>(id)] = ns;
  }
  u32 reported() const noexcept { return reported_; }
  // Milliseconds from the previous present reaching the display to this one's: the cadence the
  // viewer sees. 0 when either time is unknown.
  f64 shown_ms(u64 id) const noexcept {
    if (id < 2 || id >= shown_.size()) return 0.0;
    const i64 now = shown_[static_cast<u32>(id)];
    const i64 before = shown_[static_cast<u32>(id - 1)];
    return now != 0 && before != 0 ? static_cast<f64>(now - before) / 1.0e6 : 0.0;
  }
  // Milliseconds from the frame sampling its input to reaching the display. 0 when unknown.
  f64 latency_ms(u64 id) const noexcept {
    if (id == 0 || id >= shown_.size()) return 0.0;
    const i64 now = shown_[static_cast<u32>(id)];
    const i64 sampled = sampled_[static_cast<u32>(id)];
    return now != 0 && sampled != 0 ? static_cast<f64>(now - sampled) / 1.0e6 : 0.0;
  }

 private:
  void grow(u64 id) {
    while (shown_.size() <= id) {
      shown_.push_back(0);
      sampled_.push_back(0);
    }
  }
  Vector<i64> shown_;    // by present id; 0: not reported
  Vector<i64> sampled_;  // by present id; 0: not a sampled frame
  u32 reported_ = 0;
};

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
