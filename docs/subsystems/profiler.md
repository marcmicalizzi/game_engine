# profiler (foundation)

**Purpose.** The profiling service of [09 §9.3](../plan/09-testing-profiling.md#93-profiling-infrastructure-before-optimizing-anything) (roadmap T44): what [core/profiling](profiling.md)'s **frame capture** recorded, read two ways — a **frame report**, per pass and per zone, the GPU's and the CPU's milliseconds as a median and a tail over a window of frames, bounded in size; and a **trace** a person opens in [Perfetto](https://ui.perfetto.dev) or `chrome://tracing`, every zone on its thread and every frame's measured times beside it. It is what [plan 06 §6.6](../plan/06-agent-tooling.md#66-introspection)'s `profile` returns, once a host puts it on the protocol (roadmap A10).

**Why two modules.** The recording has to be in `core`, because every module that carries a zone is at or above it and the zone macros are `core/profiling`'s; reading it needs no more than `core` either, but the plan places the service in the foundation layer and it will grow what a foundation module may (budgets read from files, a Tracy capture beside the trace), so the reading lives here and the recording there. Nothing here names a renderer type: whoever measures a time — the renderer's GPU timer, a frame loop's spans — records it into the capture as a named value for a frame, and this module reads names and numbers.

**Public API.** `foundation/profiler/frame_report.h`:

- `copy_capture(CaptureCopy&)`: the capture's events, its state (`CaptureInfo`: origin, length, events dropped, threads, running or not) and every thread's name, copied out. It allocates, so a host calls it between frames or after `profiling::capture_stop()`.
- `build_report(events, info, ReportOptions) -> FrameReport`: the frame report. `ReportOptions::window_frames` (0: every frame) and `max_series` (32 a list) bound it.
- `chrome_trace_json(const CaptureCopy&)`, `write_chrome_trace(copy, path, &error)`: the trace.
- `percentiles(span<f64>)`: median, p95, p99, max and mean, linear between the closest ranks — the definition `renderer::percentiles` uses for a flythrough, so a pass's median here and in a benchmark summary are the same statistic.

## The frame report

**A frame** is the interval from its mark (`profiling::capture_frame(index)`) to its end (`capture_frame_end(index)`), or to the next mark, or to the capture's stop. The end is optional; a host that draws frames in bursts between requests (engine-host) ends each one so that the wait for the next request is not read as its last frame's time. `FrameReport::frame` is the frames' own intervals.

**A series** is one name's per-frame sum over the window, kept in one of three lists:

- `zones`: every `ENGINE_PROFILE_ZONE` scope, attributed to the frame its begin falls in. **The time is inclusive and summed over threads**: a nested zone is inside its parent's time as well as its own, and eight job workers busy for a whole frame read as eight frames' worth — a zone's sum is CPU time, not wall time. A zone's calls outside every frame of the window (a request's dispatch around its frames, a load between them) are counted separately, as `outside_count` and `outside_ms`, and never in the percentiles.
- `gpu` and `cpu`: **values**, a time somebody else measured, named, for a frame index (`profiling::capture_value(track, name, frame, ms)`). The index is how a GPU pass whose queries come back two frames later lands on the frame that drew it. Several values of one name for one frame add up.

**Zeros count, unmeasured frames do not.** A frame of the window the series did not appear in counts as zero — a pass that runs every other frame has a median that says so, and `frames_seen` says how often it ran. But a value's series is summarized over the frames *its track* measured (`gpu_frames`, `cpu_frames`): the last frames of a capture whose GPU times have not come back are not frames of no GPU time. Zones are summarized over every frame of the window.

**Bounded.** Each list is sorted by its total over the window, largest first (then by its outside total, then by name, so the order is a function of the numbers), and cut to `max_series`; `series_dropped` counts the names cut, all three lists together. So a report's size is fixed by its options and never by how much the capture held.

**What it says about itself.** `events` read, `dropped_events` the capture's full buffer refused (a report whose count is not zero is missing events, and says how many), `threads`, `capture_ms`, and `capture_running` when it was read from a capture still recording.

## The trace

Chrome's JSON Object Format (`"traceEvents"`, microseconds, `displayTimeUnit` ms), which Perfetto and `chrome://tracing` open as they are:

- a **complete event per zone** (`"ph":"X"`, `cat` `zone`) on its thread, named by the thread's own name (`ENGINE_PROFILE_THREAD`: the job system's `perf-3`, `eff-0`), at its own times;
- a **frames** track, one event a frame from its mark to its end, `args.frame` its index;
- a **GPU** track and a **CPU** track for the values, **laid end to end from their frame's mark** in the order they were recorded. A value is a duration somebody measured, not a time stamp, so the track says how long each pass took and in what order, not when it ran on the GPU — and its name says so ("GPU passes (durations laid end to end from the frame's mark)"). A value whose frame has no mark in the capture starts at the moment it was recorded.
- `otherData`: `format` `engine.frame_capture.v1`, `capture_ms`, `dropped_events`, `running`.

The GPU's own time stamps per pass, placed on the GPU's clock, are roadmap T45's (GPU zones), as is a Tracy capture beside the trace.

## What a host does

```cpp
profiling::capture_start();               // on: zones, marks and values record
// per frame, in the frame loop:
profiling::capture_frame(frame_index);
...                                        // zones record themselves
profiling::capture_value(profiling::CaptureTrack::gpu, "visibility", measured_frame, ms);
profiling::capture_frame_end(frame_index);
// later:
profiling::capture_stop();
profiler::CaptureCopy copy;
profiler::copy_capture(copy);
const profiler::FrameReport report = profiler::build_report(copy.events, copy.info, {.window_frames = 240});
profiler::write_chrome_trace(copy, "frames.json");
```

Which hosts do this, and over which methods, is A10's ([protocol](protocol.md) once it lands).

**General purpose.** Nothing here knows a scene, a pass or a generator: a series is a name and numbers. A host with no GPU timer records no values and gets a report of zones and frames; a process with no zones gets frames and values; a scene of any content costs a capture what its frame loop records, about 48 bytes an event.

**Testing.** `tests/frame_report_tests.cpp` (CPU, under a second): the percentiles' definition on five values; a hand-made capture of four frames — one ended early, a zone around them that is outside every frame, a zone on two threads in one frame, GPU values for three of the four frames, a value for a frame the capture does not have — whose report is checked number for number, then over a window of two frames with one series a list; and a live capture of three frames written as a trace, read back through `core/json` and counted (zones, frames, GPU values laid end to end), with the thread's name and the format.

**Depends on.** `base`, `containers`, `json` (the tests read the trace back), `profiling`.
