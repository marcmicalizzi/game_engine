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
// - `descheduled` instead of `world`, `module`, `recording` or `other` (never instead of a slot
//   wait or the present path, which are the thread's own waits) when the frame thread was off the
//   processor for at least half of that part's time beyond its slot and present waits and one tick
//   of the thread clock (`sections.off_cpu_ms` less those, less `k_thread_cpu_tick_ms`). On Windows
//   the thread's CPU is charged in 15.625 ms clock ticks and can read a tick short, so the tick is
//   allowed for, and a frame shorter than two ticks whose thread CPU reads zero is unmeasured.
//   Off the processor is not proof of preemption: a wait on a job's result inside the world's
//   update is off it too, and no record names that wait, so the word means "off the processor",
//   the system's doing or the thread's own waiting.
//
// The ten longest are kept with their parts, by frame index. A loop that keeps no records
// classifies straight from what it holds (`Input`; frame_records.h's `classify_folded`), so
// `--report` alone allocates nothing a frame.

#include <core/base/types.h>
#include <core/json/json_value.h>

#include <schemas/scene.h>
#include <span>
#include <string_view>

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
  // The thread clock's resolution: Windows charges a thread's CPU time in clock ticks
  // (`GetThreadTimes`); Linux's `CLOCK_THREAD_CPUTIME_ID` is exact.
#if defined(_WIN32)
  static constexpr f64 k_thread_cpu_tick_ms = 15.625;
#else
  static constexpr f64 k_thread_cpu_tick_ms = 0.0;
#endif

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
  // What a classification reads of a frame: a record's fields, or a loop's own numbers for a frame
  // it keeps no record of (a window with `--report` alone; building a `FrameRecord` there copies
  // strings and terrain levels, which allocate). Pointers into the caller's.
  struct Input {
    u32 frame = 0;
    f64 frame_ms = 0.0;
    const scene::FrameSections* sections = nullptr;  // null: unmeasured
    const scene::FrameWorld* world = nullptr;
    bool wait_split = false;  // `gpu_frame.wait_split` and its three parts
    f64 wait_gpu_ms = 0.0;
    f64 wait_before_ms = 0.0;
    f64 wait_late_ms = 0.0;
    std::string_view slowest_pass;
    f64 slowest_pass_ms = 0.0;
  };
  static Input input_of(const scene::FrameRecord& record) noexcept;
  // A frame's parts and its cause, whatever its length. No allocation.
  static Frame classify(const Input& frame) noexcept;
  static Frame classify(const scene::FrameRecord& record) noexcept {
    return classify(input_of(record));
  }

  void enable() noexcept { enabled_ = true; }
  bool enabled() const noexcept { return enabled_; }
  void add(const Input& frame) noexcept;  // a no-op unless enabled
  void add(const scene::FrameRecord& record) noexcept { add(input_of(record)); }
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
