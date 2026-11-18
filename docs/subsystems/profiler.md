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

**A capture that filled** (2026-11-18, from review). Once the buffer is full every later event is refused, so the frame in progress loses its end and every mark after it, and the first version stretched that frame to the stop: a 20 s capture at the default capacity, which a heavy loop fills in about 4 s, reported a 16 s frame in its max, p99, mean and window, with only `dropped_events` to show for it. Two mends were possible: end the cut frame where the buffer filled, or leave it out. Ending it there would report a frame shorter than it was, so **the report leaves it out**, and every number it gives is over whole frames whose events were all kept:
- `core/profiling` stamps the moment the buffer filled (`CaptureInfo::full_ns`, written once, by the claim that first finds it full, after that claim; every recorded event was stamped before its own, earlier claim, so none is later).
- When events were dropped, the report is `truncated`, at `truncated_ms`. A last frame with no end event is left out and named (`truncated_in_frame`, `truncated_frame`); the frame before it ends at that frame's mark if it had no end of its own. A frame whose end was recorded is whole.
- A zone that began in the cut frame, or after the buffer filled when it filled between frames, belongs to no whole frame and is counted nowhere: not in a frame, not outside one.
- A value track's last measured frame is left out of that track. A frame's GPU passes and CPU spans arrive in one burst when its slot comes back, frames after the frame was drawn, so the burst the buffer filled in may have lost its last values; counting the frame would give a missing pass a zero there.
- What it cannot see: a zone that began in a whole frame and ended after the buffer filled was refused at its end, so that frame's zone totals are short by it. A cross-frame zone (a long job) near the fill can go missing this way. The frames and the values are not affected.
- The trace follows the same frames: the cut frame has no frame event. Its kept zones and values are still drawn, as recorded, and `otherData` carries `truncated`, `truncated_ms` and `truncated_frame`.

**A report costs its events once, not the frames squared** (found by batch 68's review of the capture, mended in batch 69). Matching each frame's end to its mark walked back from the newest frame, which with a renderer's increasing indices is frames² / 2 compares (80,000 frames, about 3 × 10⁹, under the host's dispatch lock, and again for the trace), and every event found its name's series by `strcmp` down the list. Now the marks are sorted once by (index, begin) and an end is a binary search — the latest mark of its index at or before it, as the walk found, a repeated index and equal times included — and a name is a hash lookup by its pointer, compared by its characters only the first time a pointer is met, so two copies of one name are still one series (`frame_report_tests.cpp`, "ends find their marks and names their series, as the walks did"). `engine_profiler_bench`'s `profiler.report.build/<frames>` (each frame a mark, an end, ten zones of ten names and three values; `msvc-release`, 2026-11-18, others at 58 to 88 % of the CPU, upper bounds), before and after: 2,000 frames 3.8 to 4.2 ms before and 1.5 ms after; 20,000 frames 207 to 239 ms before and 22.4 to 22.9 ms after.

**A value that is not a time is left out and counted** (`invalid_values`): NaN, an infinity, or a magnitude past `k_max_value_ms` (10^7 ms, about 2.8 hours). A broken timer's answer would otherwise be averaged into a series, and in the trace it would reach a cast to `i64` that is undefined for it and print as `nan`. The trace skips the same values.

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

**Testing.** `tests/frame_report_tests.cpp` (CPU, about a second, 0.6 s of it one case's sleep): the percentiles' definition on five values; a hand-made capture of four frames — one ended early, a zone around them that is outside every frame, a zone on two threads in one frame, GPU values for three of the four frames, a value for a frame the capture does not have — whose report is checked number for number, then over a window of two frames with one series a list; and a live capture of three frames written as a trace, read back through `core/json` and counted (zones, frames, GPU values laid end to end), with the thread's name and the format.
- **A live capture of 64 events that fills.** It fills in frame 6 of 12, and the test then sleeps 0.6 s before the stop. The report is checked to be truncated in frame 6 at the stamp, with five frames, a frame max, p99 and window far under the sleep, the CPU values over the five frames, and frame 6's kept zone counted nowhere. The trace is checked to have no frame 6 and to say where it was truncated.
- **A hand-made capture that filled between two whole frames.** Its GPU track's last frame lost a pass, so that frame is left out of the track. The case also carries a NaN, an infinite and a 1e300 ms value, which are counted as invalid and are absent from the report and from a trace that still parses.

**Depends on.** `base`, `containers`, `json` (the tests read the trace back), `profiling`.
