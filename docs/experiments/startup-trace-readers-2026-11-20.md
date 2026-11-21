# The startup trace's readers at exit, before and after T210 (2026-11-20)

**Why.** The owner's five-minute walk of the forest at 4K on 2026-11-20 ended with `engine-view`
busy on one core for over twenty minutes inside `profiling::startup_summary_json`, under
`StartupTrace::finish`, and the session's report was never written; on 2026-11-17 the same step
took 43 s. This page is the "before" by number, never by waiting, and the "after"
([profiling](../subsystems/profiling.md#the-startup-trace), roadmap T210).

## What the trace holds after a long session

- **It records past ready.** Nothing stops the trace at `nearby_stable`: every `frame` stage (one a
  frame, top-level on the main thread), the world's updates and tile changes inside it, the
  workers' terrain fields, collision builds and cluster reads, the waits between them, and the
  sampler's one sample every 20 ms go on filling the buffer for the whole walk. That is deliberate
  in use: T167 and T172 attributed walk frames with those stages.
- **Its memory is bounded and never grows**: `trace_start` allocates engine-view's 2^18 slots once
  (about 70 MB). A five-minute walk at 60 frames a second is 18,000 frames and 15,000 samples
  before anything inside a frame is counted, so a walk with a handful of stages a frame fills the
  buffer; every event after that is refused.
- **`dropped_events` counts every refusal from the start to the stop**, the walk's included, so a
  summary that says it dropped events had not necessarily lost any of its load. engine-view's
  block now also says `dropped_before_ready`: what had been refused when `nearby_stable` was
  reached (null when it never was).
- **What grew with the walk was the readers' time, not the trace.** The summary visited every
  event once for each top-level stage of the main thread up to `until` and climbed each one's
  parents through a lookup by id; a session that reaches `nearby_stable` early has few such stages
  (the old summary is then linear in the events with that count as its constant), and one that
  never does — a walk whose ring never settles for thirty frames — makes every `frame` one
  (quadratic). `trace_acyclic`, called beside it, scanned every event per edge.

## The measurements

`engine_profiling_bench` in `msvc-release` (`bench/startup_trace_bench.cpp`), on the synthetic
streamed session of `tests/synthetic_session.h`: a load of 64 meshes, then frames of seven events
each (a `frame` holding a world update, its tile change and a wait for the previous frame's terrain
field; a terrain field and its cluster read on one of four workers; a sample), `nearby_stable`
reached at frame 100 (`ready`) or never (`never`). An i9-10980XE, 2026-11-20 about 16:00, **with
other processes using 99 % of the processors** (the harness's `machine_state`; `--wait-quiet=60`
found no quiet minute), so every time is an upper bound; the growth laws are what the page is for.

| reader, shape | 4,000 | 8,000 | 10,000 | 16,000 | 32,000 | 100,000 | 262,144 | 1,000,000 |
|---|---|---|---|---|---|---|---|---|
| summary before T210, never | 29.2 ms | 121.8 ms | | 667 ms | 3.16 s | | (~6 min) | |
| summary before T210, ready | | | 22.7 ms | | | 583 ms | 1.81 s | |
| summary after, never | | | 19.8 ms | | | 203 ms | 619 ms | |
| summary after, ready | | | 1.7 ms | | | 22.3 ms | 66.0 ms | 177 ms |
| Chrome trace after | | | 26.6 ms | | | 194 ms | 513 ms | |
| acyclic check after | | | 0.17 ms | | | 5.9 ms | 17.2 ms | |

- **The old summary, never ready, is quadratic**: each doubling of the events multiplied its time
  by 4.2, 5.5 and 4.7, a power of 2.25 from 4,000 to 32,000 events. Extrapolated to the 2^18
  events a five-minute walk fills, 3.16 s x (262,144 / 32,000)^2.25 is about 360 s on this shape,
  whose frames are seven events with parent chains two deep; the forest's frames hold more stages
  and deeper chains, each another lookup per event per top-level stage, which is the same law at
  the twenty minutes the owner met.
- **The old summary, ready at frame 100, is linear with a large constant**: 74 top-level stages
  before ready made 1.8 s at 2^18 events. A window's ready comes later than an offscreen run's (the
  layout lags; apps.md, "--startup-trace"): with about 2,000 frames before it, the same law gives
  about 50 s, the 43 s of 2026-11-17.
- **After T210 all three readers are linear** (ten times the events, ten times the time, within the
  noise), and a full trace's exit costs about 0.6 s for the summary of a session never ready, 0.5 s
  for the Chrome trace and 17 ms for the acyclic check, on a machine whose processors were taken.
  The never-ready summary is dearer than the ready one because its document lists every frame:
  37,000 stage objects at 2^18 events, which is what that summary says, not a cost of the walk.
- **The bytes are unchanged**: the tests compare the new readers with the old ones, kept as
  `tests/startup_summary_reference.h`, on a recorded trace and on this shape, and hold each
  reader's visits under the bound its passes give at 10^5 and 10^6 events (21 to 28 visits an
  event for the summary, against a bound of about 61).

## The rest of engine-view's exit path

Read for anything else super-linear in the session's length (2026-11-20, at 4a51ab021): the frame
records' summary sorts each series once (`renderer::summarize_frames`, O(F log F)); the report's
frame times are a fixed histogram (`report.cpp`); the world log's summary copies and sorts its
update times once; the input recording, the benchmark file and the world log are each one string
appended to; the walk, the game's state file, the HUD and the time-lapse are per-item or constant.
None is worse than n log n in the session. One is quadratic in something else: the memory report's
grouping by allocation site (`memory_report.cpp`, a linear search and an insertion sort per live
large allocation), bounded by its 16,384 and 8,192 slots and by what is live at exit, not by the
session's length; and two constant factors are wasted (`summarize_slow` sorts the wall times
twice, an offscreen run summarizes the world log twice). Left as they are.
