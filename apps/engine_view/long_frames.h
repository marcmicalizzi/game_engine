#pragma once

// **A session's long frames by cause** (roadmap T212; apps.md, "The report's long frames"): every
// frame record a session folds, classified by its own parts the moment it folds and dropped, so an
// hour's window keeps ten frames and a table of counts and never a list. A frame over 33, 50 or
// 100 ms of its own interval (`sections.wall_ms`) is given the part that held most of it:
//
// - its slot wait, split by what the GPU was doing for the frame it waited on (`gpu_frame`):
//   `gpu` (inside that frame: a slow GPU), `gpu_elsewhere` (not yet started on it: the card on
//   other work), `late_wake` (finished, the thread not yet woken); `slot_wait` without the split;
// - `world`, the host's turn less the game module's tick, with its largest named part
//   (`FrameWorld`, descending into the update and the placements while one part leads);
// - `module`, the game module's tick (`world.game_ms`);
// - `present`, the present path: the acquire, the present and the pacing in the tail before the
//   next frame (`sections.pace_ms`);
// - `recording` (`begin_frame` outside its wait, the submission and what followed it; the part is
//   the slowest pass when that holds most of it), and `other` (the rest of the tail and what no
//   section names);
// - `descheduled` instead of any of the last four when the frame thread was off the processor for
//   at least half of that part's time beyond its own waits (`sections.off_cpu_ms` less the slot
//   wait and the present path's). On Windows the thread's CPU is good to a 15.6 ms clock tick, so
//   this is a frame of 33 ms or more's word, not a small one's.
//
// The ten longest are kept with their parts, by frame index.

#include <core/base/types.h>
#include <core/json/json_value.h>

#include <schemas/scene.h>
#include <span>

namespace engine::view {

class LongFrames {
 public:
  enum Cause : u8 {
    gpu,
    gpu_elsewhere,
    late_wake,
    slot_wait,
    world,
    module,
    present,
    recording,
    other,
    descheduled,
    k_causes,
  };
  static const char* cause_name(Cause cause) noexcept;
  static constexpr u32 k_limits = 3;
  static constexpr f64 k_limit_ms[k_limits] = {33.0, 50.0, 100.0};
  static constexpr u32 k_listed = 10;

  struct Frame {
    u32 frame = 0;
    f64 wall_ms = 0.0;
    Cause cause = other;
    char part[40] = {};  // the named part, a field's name ("placements.commit", "slowest:trace")
    f64 part_ms = 0.0;
    f64 slot_wait_ms = 0.0;
    f64 wait_gpu_ms = 0.0;
    f64 wait_before_ms = 0.0;
    f64 wait_late_ms = 0.0;
    f64 world_ms = 0.0;
    f64 module_ms = 0.0;
    f64 present_ms = 0.0;
    f64 recording_ms = 0.0;
    f64 other_ms = 0.0;
    f64 thread_cpu_ms = -1.0;  // negative: the record did not measure it
    f64 off_cpu_ms = 0.0;
  };
  // A record's parts and its cause, whatever its length. No allocation.
  static Frame classify(const scene::FrameRecord& record) noexcept;

  void enable() noexcept { enabled_ = true; }
  bool enabled() const noexcept { return enabled_; }
  void add(const scene::FrameRecord& record) noexcept;  // a no-op unless enabled
  u64 frames() const noexcept { return frames_; }
  u64 over(u32 limit) const noexcept;  // frames over k_limit_ms[limit]
  u64 over(u32 limit, Cause cause) const noexcept { return counts_[limit][cause]; }
  std::span<const Frame> longest() const noexcept { return {longest_, listed_}; }
  JsonValue json() const;  // null when not enabled

 private:
  bool enabled_ = false;
  u64 frames_ = 0;
  u64 counts_[k_limits][k_causes] = {};
  Frame longest_[k_listed];  // the longest first
  u32 listed_ = 0;
};

}  // namespace engine::view
