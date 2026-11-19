#pragma once

// The profiling service (roadmap T44; docs/subsystems/profiler.md): what `core/profiling`'s frame
// capture recorded, read two ways — **a frame report**, per pass and per zone, the GPU's and the
// CPU's milliseconds as a median and a tail over a window of frames, bounded in size; and **a
// trace** a person opens in Perfetto or `chrome://tracing`, every zone on its thread and every
// frame's measured times beside them.
//
// **What a frame is.** A frame is the interval from its mark (`profiling::capture_frame`) to its
// end (`capture_frame_end`), or to the next mark, or to the capture's stop — except in a capture
// whose buffer filled, where a last frame with no end has no known end and is left out
// (`FrameReport::truncated`). A zone belongs to the
// frame its begin falls in; a value names its frame by index, which is how a GPU pass measured two
// frames later lands on the frame that drew it.
//
// **What a series costs a frame.** For each name, the per-frame sum over the window: a zone's
// calls summed (inclusive time, and summed over threads, so eight workers busy for a frame read as
// eight frames' worth), a value's samples summed. A frame the series did not appear in counts as
// zero, so a pass that runs every other frame has a median that says so. Values are summarized
// over the frames their track measured (a frame whose GPU times have not come back is not a frame
// of zero GPU time), zones over every frame of the window. Percentiles interpolate linearly
// between the closest ranks, the same definition as the flythrough's (`renderer::percentiles`).
//
// Reading costs the caller, never a frame: the capture is copied out, then summarized.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/profiling/frame_capture.h>

#include <span>
#include <string>

namespace engine::profiler {

using profiling::CaptureEvent;
using profiling::CaptureInfo;

// One name's cost over the window.
struct SeriesStats {
  const char* name = "";  // the capture's pointer: a literal, `__func__`, a table
  u32 frames = 0;         // frames of the window it was summarized over
  u32 frames_seen = 0;    // of those, the frames it appeared in
  u64 count = 0;          // zone calls or value samples in those frames
  f64 median_ms = 0.0;
  f64 p95_ms = 0.0;
  f64 p99_ms = 0.0;
  f64 max_ms = 0.0;
  f64 mean_ms = 0.0;
  f64 total_ms = 0.0;  // over the window
  // A zone's calls outside every frame of the window (a request's dispatch around its frames,
  // a load between them): their count and time. Zero for a value.
  u64 outside_count = 0;
  f64 outside_ms = 0.0;
};

// The largest magnitude a value may have and still be read as a time: about 2.8 hours.
inline constexpr f64 k_max_value_ms = 1.0e7;
// How far before a fill a frame's end must be to be sure of its zones (profiler.md, "A capture that
// filled"): an event's stamp and its claim are tens of nanoseconds apart unless its thread is
// preempted between them, and 50 ms is longer than one scheduler quantum on either system (Windows
// gives a client thread two 15.6 ms clock ticks, Linux's scheduler aims at 6 to 24 ms). A thread
// held off the processor for longer still escapes it.
inline constexpr i64 k_partial_stretch_ns = 50'000'000;

struct ReportOptions {
  u32 window_frames = 0;  // the last this many frames; 0: every frame of the capture
  u32 max_series = 32;    // per list; the rest are counted in `FrameReport::series_dropped`
};

struct FrameReport {
  u32 frames = 0;       // frames in the window
  u64 first_frame = 0;  // their indices
  u64 last_frame = 0;
  f64 window_ms = 0.0;        // from the window's first mark to its last frame's end
  SeriesStats frame;          // the frames' own intervals (name "frame")
  Vector<SeriesStats> gpu;    // values on the GPU track, by total over the window, largest first
  Vector<SeriesStats> cpu;    // values on the CPU track, the same
  Vector<SeriesStats> zones;  // zones, the same (largest in-window total, then outside total)
  u32 gpu_frames = 0;         // frames of the window the GPU track measured
  u32 cpu_frames = 0;         // and the CPU track
  u32 series_dropped = 0;     // names past `max_series`, all three lists together
  u64 events = 0;             // events read
  u64 dropped_events = 0;     // events the capture's full buffer refused
  // **A capture that filled** (profiler.md): the report ends where the buffer filled. `truncated`
  // when any event was refused; `truncated_ms` is when, from the capture's start. The frame in
  // progress then (`truncated_in_frame`, its mark's index `truncated_frame`) is left out with its
  // zones and values, as is each value track's last measured frame, since its burst of values may
  // have been cut; so every number above is over whole frames whose events were all kept.
  bool truncated = false;
  bool truncated_in_frame = false;
  u64 truncated_frame = 0;
  f64 truncated_ms = 0.0;
  // Frames whose end falls within k_partial_stretch_ns before the fill, counted above but possibly
  // short of a zone: an event is stamped before it claims a slot, so a thread that stamped a zone's
  // end before a frame's end can still lose the last slot to it. How many, and the first's index.
  u32 partial_frames = 0;
  u64 partial_first_frame = 0;
  // Values left out because their milliseconds were not a time (NaN, infinite, or past
  // `k_max_value_ms` either way): a broken timer's answer is counted, never averaged in.
  u64 invalid_values = 0;
  u32 threads = 0;
  f64 capture_ms = 0.0;  // the capture's length
  bool capture_running = false;
};

// A copy of the capture as it stands: its events and state, and every thread's name. Allocates;
// call it between frames or after the stop.
struct CaptureCopy {
  Vector<CaptureEvent> events;
  CaptureInfo info;
  Vector<std::string> thread_names;  // by the capture's thread index; "" when unnamed
};
void copy_capture(CaptureCopy& out);

// The report over `events` (`capture_snapshot`'s, in any order).
FrameReport build_report(std::span<const CaptureEvent> events, const CaptureInfo& info,
                         const ReportOptions& options = {});

// Median, p95, p99, max and mean of `values`, linear between the closest ranks; zeros when empty.
// `values` is sorted in place.
struct Percentiles {
  f64 median = 0.0, p95 = 0.0, p99 = 0.0, max = 0.0, mean = 0.0;
};
Percentiles percentiles(std::span<f64> values);

// **The trace** (Chrome's JSON Object Format, which Perfetto opens): a complete event per zone on
// its thread; a track of frames, one event a frame from its mark to its end; and a track each for
// the GPU's and the CPU's values, **laid end to end from their frame's mark** in the order they
// were recorded — a value is a duration someone measured, not a time stamp, so the track says how
// long and in what order, and the name of the track says so. `otherData` carries the format, the
// capture's length and the events dropped.
std::string chrome_trace_json(const CaptureCopy& capture);
// Writes the trace to `path`. False with `error` when the file cannot be written.
bool write_chrome_trace(const CaptureCopy& capture, const std::string& path,
                        std::string* error = nullptr);

}  // namespace engine::profiler
