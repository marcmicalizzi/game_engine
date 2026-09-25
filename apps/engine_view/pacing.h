#pragma once

// A window's frame pacing (docs/subsystems/apps.md, "Pacing"), in the pieces that need no GPU and
// no window and so are tested with made-up numbers (tests/fly_tests.cpp): `--present`'s names,
// the display pacer's depth, the display times a measured run keeps, and what the window title
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

// **How far the display pacer waits back** (`--pace display`, docs/subsystems/apps.md, "Pacing").
// Before a frame samples its input the loop waits until an earlier present has been shown: the
// last one (depth 1: one frame queued, the least latency) or the one before it (depth 2: two
// queued, a refresh more latency and a refresh more room for the frame's own work). At depth 1 a
// frame has about one refresh from sampling its input to being ready — on the RTX 5090 at
// 11520×2160 frames with 8.3 ms of CPU and GPU work in them made every 12.2 ms refresh — and one
// with more work than that misses its refresh; a depth-1 loop that keeps missing shows every frame
// for two refreshes, half the rate the unpaced loop would have kept.
//
// So this watches the waits it is fed for **misses**: a wait that came round a refresh and a half
// or more after the one before, which the next one did not make up for. The second condition is
// for a window the compositor composes, where a wait that returns late is followed by one that
// returns early and the two together still span two refreshes (measured: waits from about 5 to
// 20 ms apart round a 12.2 ms refresh in a composed 1280×720 window). Two misses
// within `k_recent` waits mean the work no longer fits, and the pacer goes to depth 2; after
// `hold()` frames there it tries depth 1 again, and a try that fails within two windows doubles the
// hold (up to 16 times `k_hold`), so a loop whose work does not fit pays two or three missed
// refreshes a minute and one whose work shrank has its latency back within seconds. The first wait
// after a change of depth is the queue refilling or draining, and is not judged. The refresh is
// the swapchain's where it reports one, else the median interval of the last `k_window` waits.
// Fixed storage, no allocation, and a function of what it is fed, so tests/fly_tests.cpp drives it
// with made-up waits.
class DisplayPacer {
 public:
  static constexpr u32 k_window = 64;  // waits the refresh median is taken over
  static constexpr u32 k_recent = 32;  // two misses closer than this mean the work does not fit
  static constexpr u32 k_hold = 240;   // frames at depth 2 before depth 1 is tried (about 3 s)
  static constexpr u32 k_max_hold = 16 * k_hold;
  static constexpr u32 k_blind = 120;  // frames not waited for after two waits timed out

  // `refresh_ns`: the display's period where the swapchain reports it, else 0.
  explicit DisplayPacer(i64 refresh_ns = 0) noexcept : refresh_ns_(refresh_ns) {}

  u32 depth() const noexcept { return depth_; }
  u32 hold() const noexcept { return hold_; }
  u32 switches() const noexcept { return switches_; }
  i64 refresh_ns() const noexcept { return refresh_ns_ > 0 ? refresh_ns_ : estimate_ns_; }
  // Refreshes that went by with no new frame, all told: frames that missed their refresh and
  // stalls. Each wait counts the refreshes it spanned less one, so an early wait after a late one
  // counts back, jitter nets out, and a small error in an estimated refresh does not accumulate.
  u64 misses() const noexcept { return missed_ > 0 ? static_cast<u64>(missed_) : 0; }

  // Whether to wait before this frame at all. Two waits running that timed out (a minimized or
  // covered window, whose presents are never shown) turn waiting off for `k_blind` frames, so such
  // a window costs two timeouts every `k_blind` frames rather than one every frame.
  bool should_wait() noexcept {
    if (blind_ == 0) return true;
    --blind_;
    return false;
  }

  // A wait returned at `now_ns`; `shown` is false when it timed out (a window nobody can see),
  // which is neither a miss nor an interval.
  void waited(i64 now_ns, bool shown) noexcept {
    ++frames_;
    const i64 interval = last_return_ns_ != 0 ? now_ns - last_return_ns_ : 0;
    last_return_ns_ = shown ? now_ns : 0;
    timeouts_ = shown ? 0 : timeouts_ + 1;
    if (timeouts_ >= 2) {
      blind_ = k_blind;
      timeouts_ = 0;
    }
    if (!shown || interval <= 0) return;
    intervals_[count_ % k_window] = interval;
    ++count_;
    if (refresh_ns_ == 0 && count_ % k_window == 0) {
      for (u32 i = 0; i < k_window; ++i)
        scratch_[i] = intervals_[i];
      std::nth_element(scratch_, scratch_ + k_window / 2, scratch_ + k_window);
      estimate_ns_ = scratch_[k_window / 2];
    }
    const i64 period = refresh_ns();
    if (period <= 0) return;
    missed_ += (interval + period / 2) / period - 1;
    if (skip_next_) {
      skip_next_ = false;
    } else {
      const bool is_long = interval * 2 >= period * 3;
      const bool is_early = interval * 2 < period;
      if (pending_long_ && !is_early) {
        miss_at_[0] = miss_at_[1];
        miss_at_[1] = frames_;
      }
      pending_long_ = is_long;
    }
    if (depth_ == 1) {
      if (miss_at_[0] == 0 || miss_at_[1] - miss_at_[0] >= k_recent) return;
      // The work no longer fits in a refresh. A try at depth 1 that failed this soon doubles the
      // hold before the next; one that lasted resets it.
      hold_ = tried_at_ != 0 && frames_ - tried_at_ < 2 * k_recent
                  ? (hold_ * 2 < k_max_hold ? hold_ * 2 : k_max_hold)
                  : k_hold;
      depth_ = 2;
      ++switches_;
      switched_at_ = frames_;
      clear_recent();
    } else if (frames_ - switched_at_ >= hold_) {
      depth_ = 1;
      ++switches_;
      tried_at_ = frames_;
      clear_recent();
    }
  }

 private:
  void clear_recent() noexcept {
    miss_at_[0] = 0;
    miss_at_[1] = 0;
    pending_long_ = false;
    skip_next_ = true;
  }

  i64 refresh_ns_ = 0;
  i64 estimate_ns_ = 0;
  i64 last_return_ns_ = 0;
  i64 intervals_[k_window] = {};
  i64 scratch_[k_window] = {};
  u64 count_ = 0;
  i64 missed_ = 0;       // refreshes spanned beyond one a wait, early waits counting back
  u64 miss_at_[2] = {};  // the frames of the last two misses, oldest first; 0: none
  bool pending_long_ = false;
  bool skip_next_ = false;
  u64 frames_ = 0;
  u64 switched_at_ = 0;
  u64 tried_at_ = 0;
  u32 depth_ = 1;
  u32 hold_ = k_hold;
  u32 switches_ = 0;
  u32 timeouts_ = 0;  // waits running that timed out
  u32 blind_ = 0;     // frames left not to wait for
};

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
