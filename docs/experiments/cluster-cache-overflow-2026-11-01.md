# The cluster cache's overflow on the v24 forest (2026-11-01)

Roadmap R114. **Question:** the owner's v24 walk through the forest proof of concept
(`fly-slow-and-rt-miss-run`, markers 3–6, ticks 28265–30153) saw polygons vanish and come back in
front of the player while the chain's budget dropped nothing (`rt_wanted == rt_built` on every
frame of the window). Is the resident cluster structure cache the cause, and what does it take for
an overflowing cache never to change the picture? **Answer:** yes — the forest references more
distinct cached clusters a frame than the old fixed ceiling's 43,690 slots, and a frame pointed
the ones it could not place at the inactive slot 0, so they drew and cast nothing. A miss the cache
cannot place is now built in the frame's own set, and the ceiling follows the device (a
sixty-fourth, 506 MiB and 86,357 slots on the RTX 5090), which holds v24's wanted set whole
([renderer](../subsystems/renderer.md#an-overflow-is-built-in-the-frames-own-set)).

## The test

`rt cache: a frame wanting more than the cache holds builds the rest itself`
(`systems/renderer/tests/rt_capacity_tests.cpp`): the nine-instance heightfield grid at a
quarter-pixel cut from 0.8 radii — 551 distinct cached clusters a frame — against a cache of 200
slots (`renderer.rt.cache_slots`), a frame drawn and then two captured, each against the per-frame
build of the same cut (`renderer.rt.cache=false`). `msvc-debug`, RTX 5090:

| | ray path: id words / colour bytes differing, two frames | hardware raster, traced shadows: colour bytes differing | the frame's own builds |
|---|---:|---:|---:|
| before (4e442d76) | 74,602 / 75,240 | 3,804 | 0 of 0 — the 352 misses a frame pointed at slot 0 |
| after | **0 / 0** | **0** | 352 of 352 |

Both: 352 misses a frame the cache had no slot for (199 slots occupied of 200, slot 0 the
inactive one), counted in `cache_overflow`. Before, the five checks that fail are `built > 0` and
the picture's, twice; after, the case and the other four cache and ceiling cases pass.

## The v24 walk

The run: `msvc-release` engine-view, offscreen, 1920×1080, the v24 scene
(`reports/lab/20261031-125927-663/scene.json`, the v22 materials, the derived-data cache copied
once for the batch, nothing cooked into it), the owner's flags less `--game`, `--interactive`, the
window and its present format: `--raster rt --shadows rt --no-lights --time-of-day 15
--world-rate 1 --time-rate 0 --lod 1 --deform-pool-mib 64 --rt-budget-mib 0 --terrain-tiles
--terrain-far 6` and the flora's motion at wind 0.7. `--start` is a live session's, so the pose is
a camera path instead: marker 3's pose (4.97, 1.68, −9.87) at yaw −174.25°, pitch 4.32°, walking
a quarter of a metre straight ahead at 1 m/s over 16 frames at 60 fps, the gaze held. A benchmark
of those 16 frames after a warm-up flight of the same 16 (`--warmup 16 --repeat 1 --wait-quiet
120`), then the ids at frame 15 twice: the marker capture (after eight frames at its pose) and the
final capture, the next frame at the same pose. One process before, one after, each under the
GPU lock. A third run, after, with `--tunable renderer.rt.cache_mib=256` — the old ceiling — runs
the new overflow path on the real scene: what it costs, and whether its picture is the after run's.

| | before (4e442d76) | after | after, at the old ceiling |
|---|---:|---:|---:|
| cache ceiling, slots | 256 MiB, 43,690 | **506 MiB (the device's), 86,357** | 256 MiB, 43,690 |
| cache bytes | 289.5 MB | 554.5 MB | 288.8 MB |
| distinct cached clusters the run drew (slots occupied at the end) | 43,689 — full | 46,323 | 43,689 — full |
| cached references a frame (`rt_references`) | 46,498–46,523 | 46,498–46,523 | 46,498–46,523 |
| misses the cache had no slot for (`cache_overflow`, the run) | 112,506 — 2,550 a frame, drawn as nothing | **0** | 112,506 — 2,550 a frame, built in the frame's set |
| the frame's own builds (`rt_built` = `rt_wanted`), timed frames | 64,553–64,740 | 64,553–64,740 | 67,117–67,290 |
| slots evicted (`rt_cache_evicted`), timed frames | 56 (0–19 a frame) | 0 | 50 (0–17 a frame) |
| structures built into the cache, timed frames | 0–19 a frame | 0–34 a frame | 0–17 a frame |
| `clas` (ms, median / p95) | 1.339 / 1.350 | 1.359 / 1.376 | 1.494 / 1.537 |
| `rt`, the whole chain with the cache's passes and its build (ms) | 3.612 / 3.795 | 3.631 / 3.648 | 3.774 / 3.890 |
| `blas` (ms) | 1.458 / 1.646 | 1.456 / 1.459 | 1.454 / 1.458 |
| frame GPU total (ms) | 11.114 / 11.614 | 10.907 / 11.121 | 11.469 / 11.584 |
| the chain's bytes (`rt_bytes`) | 4,409 MB | 4,681 MB | 4,415 MB |
| bytes a reference (`rt_bytes_per_reference`) | 220 | 236 | 236 |
| machine | quiet: others 3.3–5.5% of the CPU, GPU idle at both ends | others 16.5–17.5% of the CPU, the GPU busy at the end: upper bounds | others 30.8% of the CPU (two container builds): upper bounds |

The `clas cache` zone, the build of the misses into their slots, is inside `rt` and not in the
frame record of its own; `rt` less `clas` and `blas` is the records, the ranges, the emit, the
cache's four passes, the cache's build and the top level together — 0.815 ms before, 0.816 after.

**The ids.** At frame 15, before against after: **21,045 pixels differ** (1.0% of the frame's
2,073,600), and no pixel is empty in one and not the other — the 2,550 clusters a frame the old
cache drew as nothing were in front of other geometry, and the rays met what stood behind them.
After against after at the old ceiling: **0 pixels differ**, both captures — the misses the cache
had no slot for are built in the frame's own set, and the picture is the one the larger cache
draws. In each run the two consecutive captures at frame 15's pose (the marker's, after eight
frames at it, and the next frame's) are identical: at a standing pose the overflow's holes stand
still. They move only as clusters enter the cut — the walk's 1 to 19 a frame — because a slot
this frame drew is never evicted: a new cluster takes the slot of one that left the cut, and which
of the frame's misses gets it follows the atomics, so a hole appears where a cluster lost that race
and closes when it wins one. The owner's flight, faster than this walk, brought many more each
frame, which is the vanishing and returning the walk showed. The captures are kept in the batch's
scratch (`r114/run-*/markers/00015-end.ids.bin` and `final.ids.bin`, 12 bytes a pixel), not here.

**What the overflow path costs**, from the third run against the second: the frame builds the
2,550 misses itself (64,579 → 67,148 structures), `clas` +0.135 ms at the median and `rt`
+0.143 ms; the frame total moved more (+0.56 ms) under twice the CPU load of the after run, so
that row is an upper bound. That is the price of a cache smaller than the wanted set — the
per-frame builds the cache exists to save — and no longer a hole in the picture.

**The device's ceiling** holds the walk's whole set: 46,323 distinct clusters in 86,357 slots, no
overflow, no eviction, 265 MB more of slots than the old ceiling. The owner's 3840×2160 run
references 53,000 to 55,000 a frame, which the 5090's ceiling holds too.

## What was not built

**An admission rule** — a cluster cached only once it had been drawn some frames in a row, so that
a wanted set larger than the slots would settle on the clusters that stay. The table says the
cache already settles: at the old ceiling the walk evicted 0 to 17 slots a frame while the frame
built 2,550 misses itself, because a slot drawn this frame is never evicted, so the resident set
stays and only clusters entering the cut compete. A counter a key would add a pass and a word a
key for a churn of under twenty a frame.

**A transient overflow region** in the cache's buffer, built with explicit destinations: it would
have kept `cache_references_main` before the frame's build, but its size would have been a second
budget with slot 0 past it. The frame's own set is already sized by the frames, under the budget
that follows the device.
