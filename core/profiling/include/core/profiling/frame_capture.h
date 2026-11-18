#pragma once

// The frame capture (roadmap T44; docs/subsystems/profiling.md, "The frame capture"): what a
// running process did over a stretch of frames, recorded only while a host has turned it on, in
// every build — Tracy or not. Three kinds of event go into one buffer `capture_start` allocates:
//
//   - **zones**: every `ENGINE_PROFILE_ZONE`/`ENGINE_PROFILE_ZONE_NAMED` scope (profile.h) that
//     began and ended while the capture was on, with its thread and its two times;
//   - **frame marks**: `ENGINE_PROFILE_FRAME()` or `capture_frame(index)`, the boundary a frame
//     loop draws at the start of each frame, which is what a zone is attributed to a frame by;
//   - **values**: a time somebody else measured for a frame — a GPU pass from the GPU timer's
//     queries, a CPU span of the frame loop — named, and with the frame it measured, which for a
//     GPU time is frames behind the mark of the frame it arrives in.
//
// **Off it costs one relaxed atomic load per zone and allocates nothing.** On, an event claims a
// slot with one atomic add and writes 48 bytes; a full buffer drops and counts, so a frame loop
// with the capture on still allocates nothing (AGENTS.md, "No allocations in the frame loop").
// `foundation/profiler` reads a stopped capture into a frame report and a trace a person opens.
//
// Names are pointers, never copied: a string literal, `__func__`, or a table that outlives the
// capture. A thread's name (`ENGINE_PROFILE_THREAD`, `capture_name_thread`) is copied, because the
// job system names its workers from a stack buffer.

#include <core/base/types.h>

#include <atomic>
#include <span>

namespace engine::profiling {

enum class CaptureKind : u8 { zone, frame, frame_end, value };
// Which clock a value was measured on: the CPU's (a span of the frame loop) or the GPU's (a
// pass's timestamp queries). A zone is always the CPU's.
enum class CaptureTrack : u8 { cpu, gpu };

inline constexpr u32 k_capture_default_capacity = 1u << 18;  // 14 MiB with the slots' words
inline constexpr u32 k_capture_max_threads = 256;
inline constexpr u32 k_capture_thread_name_bytes = 32;

// One event as `capture_snapshot` returns it. Times are nanoseconds from the capture's origin
// (`capture_start`), on `trace_now_ns`'s clock (startup_trace.h).
struct CaptureEvent {
  const char* name = "";
  i64 begin_ns = 0;  // a zone's begin, a frame mark's time, the moment a value was recorded
  i64 end_ns = 0;    // a zone's end; a frame mark's and a value's are their begin
  u64 frame = 0;     // a frame mark's index, the frame a value measured; 0 for a zone
  f64 ms = 0.0;      // a value's milliseconds; a zone's and a mark's are their duration (0)
  u16 thread = 0;    // the capture's own thread index, in order of first event (0 = first)
  CaptureKind kind = CaptureKind::zone;
  CaptureTrack track = CaptureTrack::cpu;
  u32 reserved = 0;

  i64 duration_ns() const noexcept { return end_ns >= begin_ns ? end_ns - begin_ns : 0; }
};

struct CaptureOptions {
  u32 capacity = k_capture_default_capacity;
};

// A stopped capture, as `capture_snapshot` copies it out.
struct CaptureInfo {
  i64 origin_ns = 0;     // `trace_now_ns` at `capture_start`
  i64 stopped_ns = 0;    // from the origin; the capture's length
  u64 dropped = 0;       // events a full buffer refused
  u32 threads = 0;       // thread indices used
  bool running = false;  // true: the snapshot was taken while recording (events may be missing)
};

namespace detail {
extern std::atomic<bool> g_capture_on;
u32 capture_zone_begin_check() noexcept;  // the capture's generation, or 0 when off
void capture_zone_end(const char* name, i64 begin_ns, u32 generation) noexcept;
i64 capture_clock_ns() noexcept;
}  // namespace detail

inline bool capture_enabled() noexcept {
  return detail::g_capture_on.load(std::memory_order_relaxed);
}

// Starts recording: allocates the buffer (or keeps one large enough from the last capture) and
// clears it. False when already on.
bool capture_start(const CaptureOptions& options = {});
// Stops recording, and returns only once no thread is still writing an event: a zone that began
// while it was on and ends after it is dropped, not half written.
void capture_stop() noexcept;
// Frees the buffer. Recording must be stopped.
void capture_reset() noexcept;

// A frame begins: the boundary every zone is attributed to a frame by. `index` is the caller's own
// (a frame loop's frame index), which a value measured for that frame names too.
void capture_frame(u64 index) noexcept;
// Frame `index`'s work is done (optional). A frame with no end lasts until the next mark, or the
// capture's stop: a host that draws frames in bursts between requests (engine-host) ends each one,
// so the wait for the next request is not counted as the last frame's time.
void capture_frame_end(u64 index) noexcept;
// A time measured elsewhere for frame `index` — a GPU pass, a frame loop's span. Several values of
// one name for one frame add up.
void capture_value(CaptureTrack track, const char* name, u64 index, f64 ms) noexcept;
// Names the calling thread in captures (copied, `k_capture_thread_name_bytes` - 1 at most). Costs
// nothing a frame: the name is kept on the thread and read when its first event is recorded.
void capture_name_thread(const char* name) noexcept;

// Copies the events out, in the order they were claimed, and the capture's state. Safe while
// recording (a slot still being written is left out), exact once stopped.
u32 capture_snapshot(std::span<CaptureEvent> out, CaptureInfo& info) noexcept;
// Events recorded so far (claimed and not dropped).
u32 capture_count() noexcept;
// The name the capture has for thread index `thread`: what it named itself, or "" when it did not.
const char* capture_thread_name(u32 thread) noexcept;

// A zone scope: records itself when the capture was on at both its ends.
class CaptureZone {
 public:
  explicit CaptureZone(const char* name) noexcept : name_(name) {
    if (capture_enabled()) {
      generation_ = detail::capture_zone_begin_check();
      if (generation_ != 0) begin_ns_ = detail::capture_clock_ns();
    }
  }
  ~CaptureZone() {
    if (generation_ != 0) detail::capture_zone_end(name_, begin_ns_, generation_);
  }
  CaptureZone(const CaptureZone&) = delete;
  CaptureZone& operator=(const CaptureZone&) = delete;

 private:
  const char* name_;
  i64 begin_ns_ = 0;
  u32 generation_ = 0;
};

}  // namespace engine::profiling
