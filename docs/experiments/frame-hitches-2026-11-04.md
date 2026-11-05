# The ray tracing chain's remaining hitch causes (R128, 2026-11-04)

Roadmap R128, after R130 ([the measurement it continues](frame-hitches-2026-11-03.md)). R130 found four causes for the twenty hitch frames of the owner's v27 walk (`D:/workspace/forest_wildlife_poc/reports/lab/20261103-095454-764/`, 3840×2160, `--raster rt --shadows rt`) and fixed two. This page is the other two and what R128 changed for them: the one-thread drop rule (class A), the growth step while the demand rises, the counts naming the capacity they were built against, and the first build at a new capacity (class C) — which did not reproduce, and what did instead.

## The runs

Two paths from the owner's records' `pose_*` fields: **R130's** (one key every three recorded frames over 55,000–56,500, 58,900–59,100 and 41,900–42,100; 635 frames, two cuts) and **one at the owner's own pace** (a key per recorded frame over 55,000–56,500; 1,501 frames), so the turn into the woods arrives as fast as it did for the owner. Both flown offscreen at 3840×2160 with the run's flags (`--raster rt --shadows rt --terrain-tiles --terrain-far 6 --lod 1 --deform-pool-mib 64 --rt-budget-mib 0 --no-lights --time-of-day 15 --world-rate 1 --time-rate 0`, the flora's motion and wind, micromaps off), `--benchmark --repeat 1 --warmup 30 --wait-quiet 120`, from the run's `scene.json` copied out and the shared v24 derived-data copy (nothing was cooked into it: every mesh and texture hit), `msvc-release`, RTX 5090, one process each, all six in one hold of the GPU lock. **Before** is R128's switches off in the same binary — `renderer.rt.scan_every_frame=false renderer.rt.slope_frames=0`, R130's rules — and **after** the defaults.

Both paths start at the budget (338,115 clusters, the device's sixteenth) because the scene's estimate is past it, and the default shrink rules keep most of it, so the growth rule has little to do on them: the owner's capacity stepped because his walk was long enough for the ten-second windows to shrink it. The third pair, **S**, flies the owner-pace path with the capacity following the demand closely — `renderer.rt.shrink_seconds=0 shrink_frames=30 peak_seconds=0 peak_frames=0 floor_pct=0` — so it steps 14 to 15 times in the run: that is where the growth rule and the first build at a new capacity are tested.

**The machine was shared.** The harness's `machine_state` at the runs' starts and ends: other processes on 6–63% of the CPU (A-before 63% at its start, 44% at its end; the rest 6–23%), the GPU 0–3% busy except at S-before's end (97%, another agent's tests), and 19.6–22.0 GB of the card in use, this process included. Only S-after was quiet by the harness's rule, so every number is an upper bound.

## What they say

| | R130's path, before | after | owner's pace, before | after | S (capacity follows), before | after |
|---|---:|---:|---:|---:|---:|---:|
| frames whose GPU passed 100 ms | **1** (471: `rt_ranges` 123.5 ms) | **0** | 0 | 0 | 0 | 0 |
| frames whose GPU passed 50 ms | 1 | 0 | 0 | 0 | 0 | 0 |
| largest `rt_ranges` | 123.46 ms | 0.49 ms | 0.25 | 0.17 | 0.19 | 0.20 |
| largest GPU frame | 155.8 ms | 36.7 ms | 29.5 | 29.4 | 30.1 | 29.8 |
| frames past the capacity they were built against | 15 (all past the budget) | 15 (the same) | 37 (all past the budget) | 37 | 45 | 40 |
| of them under the budget (a growth that came late) | 0 | 0 | 0 | 0 | **10** | **4** |
| capacity steps | 0 | 0 | 2 (211 ↓, 392 ↑) | 2 (211 ↓, **225** ↑) | 14 | 15 |
| frames whose wall time passed 100 ms / 50 ms | 13 / 44 | 11 / 16 | 10 / 23 | 10 / 23 | 10 / 39 | 11 / 33 |
| of the 100 ms ones, the host's turn (the offscreen world completing its tiles) | 12 (and 473, the slot wait behind 471) | 11 | 10 | 10 | 10 | 10 (and 162) |
| median frame (wall) | 22.4 ms | 22.5 ms | 20.6 ms | 20.9 ms | 20.7 ms | 20.6 ms |
| on frames that fit: `rt_drop` median / p95 | 0.000 / 0.009 ms | 0.007 / 0.011 ms | 0.000 / 0.009 | 0.007 / 0.011 | 0.007 / 0.011 | 0.007 / 0.011 |

**Class A is gone.** R130's path keeps its one frame (471) that went from under half the capacity to past it within two frames — past the budget itself, 351,195 against 338,115 — and on it the one-thread rule cost 123.5 ms of GPU and the next frame's slot wait 157 ms. With the scan recorded on every frame the same frame's `rt_ranges` is 0.49 ms and its walk (`rt_drop`) 0.89 ms, and no frame of any run passes 50 ms of GPU. What it costs a frame that fits: the scan's three dispatches with no groups, 7 µs at the median and 11 at p95 (the same as R130's 7 and 11), against nothing without them; the chain's `rt` median moved by less than the runs' spread (6.42 → 6.27 ms on R130's path, 5.90 → 5.93 on the owner's pace). The alternative the brief named — keeping the one-thread rule out of `ranges_main` only on frames that fit — would save those 7 µs and still take 120 ms on any frame the CPU did not see coming; there is no cheaper rule that sees the future.

**The growth while the demand rises.** On the default rules the owner-pace path's turn into the woods is met by the regrowth to the last minute's peak either way (R116), and the slope only brings it forward: the capacity that shrank at frame 211 grows back at 225 instead of 392. Where the capacity follows the demand (S), frames built against a capacity under the budget that their demand had already passed went from 10 to 4: the first two frames of the turn out of the clearing (128–129, which no rule can see — the demand reaches the policy two frames late) and two one-frame jumps of 39% and 31% of the capacity (954, 1412). Every frame short of its demand on the two default runs was past the budget, where no step helps.

**The counts.** In S-before, frame 955 was recorded against 266,240 and read back after the capacity had stepped to 338,115 — `rt_built_capacity` 266,240, `rt_capacity` 338,115 — the case R127 found in the owner's log; the analysis above reads every frame against its own.

## Class C did not reproduce

The owner's frame 7390 spent 103 ms in `blas`, the first bottom-level build after the capacity stepped. Across the 33 capacity steps of the four runs that stepped, the first build at the new capacity took 2.07–3.26 ms against a median of 2.15 ms over all frames (the largest, 3.26 ms at S-after's 1412, is the most structures any frame built: 315,392). So the build itself does not cost more at a new capacity — no build over the whole set, no first touch of the new storage or scratch that the frame pays — on this card with 19.6–22 GB of it in use. What remains plausible is the driver paging the new set in on a card other processes held more of (the owner's run had the creator body and 416 animals beside it, and R130's notes 21 GB held by others); a replay cannot make that happen on demand. `rt capacity: a first build at a new capacity costs what a build at an unchanged one does` holds the bound on the device's own timings, and the frame record's `rt_built_capacity` now says which frame was the first at a new capacity, so the next recording that shows it names it.

## What a step still cost: the free, on the frame's thread

Reading the steps' `sections` found the cost a growth had left on the CPU. `cpu.resize` — making the new set beside the old — was 1–28 ms (the largest, 27.5 ms, a growth of 102,400 clusters to the budget); but two frames after a large one, when the slot came round, `begin_ms` held 30–65 ms besides its resize (S-before 957: 45.3 ms with a 1.3 ms resize; S-after 1412–1415: 43.6, 70.9, 66.3 and 31.4 ms with 6.4, 7.1, 1.5 and 1.4 of resize). That was the slot's deferred frees: the retired 1.6–2 GB CLAS set released on the frame's thread, which R116's note had said ("the free, now deferred, still lands on the frame thread when the slot comes round").

**It is now freed on a worker** (`renderer.rt.free_on_worker`, true): the retired buffers wait in the slot's list for the same moment and a job releases them while the frame records ([renderer](../subsystems/renderer.md#a-resize-is-swapped-in-not-waited-for)). Flown on the S configuration, worker off and on in the same binary:

| the S flight (31 resizes) | worker off | worker on, no wait | **worker on, the step waits for it (as shipped)** | worker off, again |
|---|---:|---:|---:|---:|
| order flown | 1st | 2nd | 3rd | 4th |
| frames whose `begin_ms` less its resize passed 10 ms (the free on the frame's thread) | 18, the largest 73.2 ms | 0 (largest 1.4) | **0** (largest 1.7) | 16, the largest 91.9 ms |
| the resizes' own time, summed / the largest | 251 / 99.3 ms | 597 / 271.9 ms | **433 / 76.4 ms** | 1,133 / 348.9 ms |
| of the largest, waiting for the worker's frees | — | — | 70.4–72.7 ms (the burst at 1412–1415) | — |
| `begin_ms` summed over the flight | 1,042 ms | 743 ms | **597 ms** | 1,918 ms |
| frames whose wall time passed 100 ms (all but one or two the host's turn) | 11 | 14 | 10 | 12 |
| machine state (start → end): others' CPU, GPU busy, card memory in use | 7 → 8%, 0 → 0%, 19.6 → 21.5 GB | 9 → 7%, 0 → 52%, 19.6 → 21.5 GB | 8 → 14%, 0 → 100%, 19.6 → 21.4 GB | 33 → 33%, 0 → 0%, 19.5 → 21.2 GB |

**What it says, and what it does not.** Freed on the worker, no frame's `begin_ms` holds a free any more: the isolated steps' 30–50 ms two frames later (162, 131, 1257) are gone. Freed while the next step allocates, the driver's allocation was slower in the second flight (the burst at 1412–1415: 87, 272, 62 and 36 ms, the CLAS set's allocation 244 ms of the 272) — so a step now waits for any free still running before it allocates, and in the shipped flight those allocations were 1–13 ms behind 56–73 ms of waiting: in a burst of steps two frames apart the free comes back to the frame's thread as a wait, which is what it cost before. **The driver's allocation time is the larger variable**: the fourth flight, with no worker and no overlap at all, spent 340–349 ms allocating each of three steps of the same burst (the CLAS set's and once the scratch's), where the first spent 7–19. That is the same order as the owner's frame 7390 and as R127's 248.6 ms bottom-level allocation, on a card 19.5–21.5 GB full, and nothing in the chain decides it; allocating the next step ahead of the frame that needs it, on a worker — R128's original ask — would move it off the frame, and is the next thing to try (below).

## What was changed

- `renderer.rt.scan_every_frame` (true): every frame records the drop order's scan, so no frame past its capacity takes the one-thread rule in `ranges_main`. False is R130's rule. The one-thread rule stays in `clas_records.slang` for that comparison and for an order whose ranks fall, which the renderer never writes. `rt capacity: the drop order's scan on every frame writes the one-thread rule's bytes` pins a frame past its capacity recorded on the one thread after 72 that fitted and holds the scan's record count words and bottom-level records to its bytes, and its records to the same set (their order within an instance is the bucketing atomics').
- `renderer.rt.slope_frames` (4; `RtCapacityConfig::slope_frames`): while the demand rises, a growth's trigger and target are the demand plus its rise since the last observed frame times four — the read-back's two frames and the frames in flight — never past the budget. `rt capacity: a rising demand is met without a short frame`: a turn of 15% of the capacity a frame, observed two frames late, leaves 4 frames short at a fixed headroom and none following the slope.
- `FrameRecord.rt_built_capacity` (version 16): the capacity the frame was recorded against. `rt capacity: a frame's record names the capacity it was built against`.
- `renderer.rt.free_on_worker` (true): a swapped resize's retired buffers are freed on a worker.

## What is next

- **Allocate the next step ahead, on a worker** (roadmap R133). The steps' remaining cost is the driver's allocation of the new set, 1 to 349 ms for the same step from one flight to the next on this shared card: a policy that knows the next step (the demand's slope says so two frames early) could make it on a worker and swap it in when it is asked for. The worker's own pool is there now.
- **Class C on the owner's machine.** The record's `rt_built_capacity` names the first frame at a new capacity; a recording that shows a slow first `blas` again says which, with the card's memory beside it.