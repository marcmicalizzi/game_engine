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
GPU lock.

(after-run numbers below)
