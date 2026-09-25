# E37: a streamed world's tile changes cost the tile, not the tail

- **Question ([10 §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing), following [E35](e35-world-streaming.md)):** E35 found that the renderer taking a streamed world's tail was the whole cost of streaming — 3.2 ms a change for a thousand buildings of the kit of boxes and 15–17 ms for E33's ashlar kit, on six frames of seven along the desert flythrough, each of them waiting for the device — because the tail was one prefix sum and every change rewrote every tile's instances and pair entries. With each tile in a block of its own, the pair table expanded on the GPU from the instance table and the tables in one set per frame in flight ([renderer](../subsystems/renderer.md#instances-that-come-and-go)), what does a change cost the CPU, what does the expansion cost the GPU, does any frame still wait for the device, and what do the holes a block layout leaves cost the cull?
- **Date:** 2026-09-25. **Machine:** Intel Core i9-10980XE (18 cores, 36 logical CPUs), 64 GB, Windows 11 Pro 26200; **GPU:** RTX 5090, driver 610.88, Vulkan 1.4. **Build:** `msvc-release`, twice, in one session: **before** is main at `d952a5e` — E35's code — exported with `git archive` into `build/e37-baseline` and built there; **after** is this change on the same commit.
- **Machine state:** shared, and never quiet: every run waited 60 s for a quiet machine (`--wait-quiet 60`) and measured without one. Each group held the GPU lock on its own (`ENGINE_GPU_LOCK_OWNER=agent-tail`), between other agents' holds. The rows below are the second group, in which other processes took 17–82% of the CPU at a run's start and 10–50% at its end (the before run of the ashlar kit with mid blocks started at 82%, its after run at 35%); the GPU was ours at every start (7–12% busy) with 7,874–8,434 MiB of its 32,607 MiB held by other processes. The first group, with four size-class steps an octave (below), ran at 7–14% of the CPU at its starts and 13–16% at its ends, and its before rows agree with the second's to within 3%, a change's cost included. The CPU numbers are upper bounds on a busy machine; the GPU's are the timers'.
- **Decision:** none of its own. The renderer's [Instances that come and go](../subsystems/renderer.md#instances-that-come-and-go) says what was built and why; [world](../subsystems/world.md), [ADR-0040](../adr/0040-the-tile-ring.md)'s consequences and [04 §4.9](../plan/04-renderer.md#49-streaming-and-residency)'s status note carry the result. The one default it sets is the compaction share, `renderer.tiles.compact_pct` = 25, with the 12% row below as the other side of the trade.

## Setup

**What was built** ([renderer](../subsystems/renderer.md#instances-that-come-and-go)): engine-view hands the renderer the tail tile by tile, each tile's run a `DynamicBlock` keyed by the tile; the renderer gives each tile a block of instance slots and a reserved run of pairs (sixteen-pair and four-slot granules, eight size classes an octave past eight granules), keeps a block whose tile comes back unchanged, rewrites one in place when its new instances fit, frees the rest and places the new tiles in the smallest free block that holds them or at the end, and compacts when the holes pass 25% of the cull's dispatch (or when the world starts over, at each repeat). A change writes its slots into the CPU's instance table; the next frame flips to the next of two table sets (two frames in flight), copies that set's changed slots in from its own staging buffer and expands their pair-table entries on the GPU (`pair_expand.slang`), and nothing waits for the device.

**The runs** are E35's, on E35's own scene files under `game_engine_local\world\e35\` (never committed): the desert overlook's terrain (187,660 pairs) with E34's thousand buildings over it, the default rings with the **kit of boxes** and low blocks near (`scene-boxes-world.json`), and rings of 1.5, 6 and 10 tiles with **E33's ashlar kit** and mid blocks near (`scene-ashlar-world-mid.json`) or low blocks near (`scene-ashlar-world-low.json`). engine-view offscreen at 1920 × 1080, the desert overlook's path resampled to 600 frames, three repeats, occlusion on, shadows off, the hardware rasterizer, a world log; each scene before and then after, back to back. The scripts are in the session's scratchpad, not the tree:

```powershell
$env:ENGINE_GPU_LOCK_OWNER = 'agent-tail'
pwsh tools/gpu-lock.ps1 run -Purpose "E37 ..." -Exec "pwsh -NoProfile -File run-e37.ps1 -Scenes boxes-world,ashlar-world-mid,ashlar-world-low"
#   each: engine-view --scene <e35>\scene-<row>.json --camera-path content\test-scenes\desert-overlook\camera-path.json
#     --offscreen --benchmark out\<build>-<row>.jsonl --frames 600 --repeat 3 --width 1920 --height 1080
#     --shadows off --ddc D:\workspace\game_engine_local\ruins\blocks\ddc --log info --wait-quiet 60
#     --world-log out\<build>-<row>.world.jsonl  [--tunable renderer.tiles.compact_pct=12]
pwsh analyze-e37.ps1 -Names <runs>
```

The per-change numbers are the world's: the ruins consumer's `insert_ms` (the renderer taking the tail, summed over every hand-over of the run, warm-ups and restarts included) and `insert_ms_max`, and each recorded frame's commit (deciding the budget, concatenating the tail, and the renderer taking it). The GPU's are the flythrough's records, which since this change carry `table_slots`, `table_pairs`, `hole_pairs` and `gpu_ms.tables`; the renderer's `tile changes` log line at the end of a run says how the blocks were used.

## Results

### What a change costs the CPU

| scene | build | the renderer taking the tail, mean a hand-over (worst) | commit on a frame with events, ms, median (p95; p99; max) | frame ms, median (p95; max) |
|---|---|---|---|---|
| boxes, low blocks near | before | 3.33 ms (10.0) | 3.80 (5.34; 8.77; 11.4) | 4.08 (4.78; 9.49) |
| | **after** | **0.356 ms** (2.93) | **0.85** (1.10; 1.73; 3.39) | **1.20** (1.56; 2.04) |
| ashlar, mid blocks near | before | 15.9 ms (31.9) | 16.3 (19.5; 22.5; 32.2) | 16.1 (18.7; 22.8) |
| | **after** | **0.177 ms** (3.74) | **0.35** (0.52; 1.02; 2.60) | **0.59** (1.00; 2.07) |
| ashlar, low blocks near | before | 15.6 ms (23.1) | 16.0 (19.1; 21.3; 23.6) | 15.9 (18.4; 20.1) |
| | **after** | **0.172 ms** (3.97) | **0.34** (0.48; 0.62; 2.75) | **0.58** (0.91; 2.43) |

**Ninety times less for the ashlar kit and nine times less for the boxes, and no frame waits for the device**: nothing in a change calls a wait, and the worst frame of the after runs is 2.0–2.4 ms of wall time where the before runs' was 9.5–23 ms. What is left is CPU work that is not the tile's: the renderer compares every block the world hands over against what it holds, because the world hands the whole tail over on every change — about 9,300 instances for the ashlar kit and 21,000 for the boxes, which is why the boxes cost twice what the heavier kit does — and the world itself concatenates the whole tail at its commit, the rest of the commit column. The worst hand-overs, 2.9–4.0 ms, are the ones that write many tiles at once: a repeat's first fill of several hundred, a compaction, a growth of the per-pair buffers.

### What the tables cost the GPU

On 1,311–1,335 of the 1,800 recorded frames the frame flipped to a table set behind a change and brought it up to date:

| scene | slots copied, median (p95; max) | pairs expanded, median (p95; max) | copy + expansion, ms, median (p95; p99; max) | expansion alone, mean |
|---|---|---|---|---|
| boxes | 544 (2,128; 56,793) | 148 (1,240; 210,702) | 0.017 (0.035; 0.19; 0.51) | 0.0063 ms |
| ashlar, mid | 264 (2,332; 19,521) | 149,435 (384,394; 8,568,881) | 0.016 (0.040; 0.14; 0.27) | 0.0079 ms |
| ashlar, low | 280 (2,580; 13,841) | 130,971 (346,540; 7,853,329) | 0.016 (0.044; 0.18; 0.44) | 0.0081 ms |

A frame's table update is 16–17 µs of GPU at the median, most of it the copy's fixed cost. The copy and the expansion write a whole ashlar tail — 8.6 million entries, 69 MB of `uint2`s — in 0.27 ms, about 250 GB/s, which is what a compaction or a growth costs each of the two table sets once. Before, the same entries were built on the CPU and uploaded: 66 MB a change.

### What the holes cost the cull

| scene | holes / dispatch, median (p95; max) | pairs dispatched, median: before → after | cull ms, median (p95): before → after | all passes ms, median (p95): before → after |
|---|---|---|---|---|
| boxes | 13.7% (19.6; 20.4) | 208,937 → 243,020 | 0.032 (0.038) → 0.030 (0.034) | 0.133 (0.151) → 0.145 (0.168) |
| ashlar, mid | 17.5% (23.2; 24.9) | 8,472,877 → 10,109,196 | 0.204 (0.219) → 0.241 (0.268) | 0.333 (0.357) → 0.380 (0.431) |
| ashlar, low | 14.5% (23.3; 24.4) | 8,113,835 → 9,522,540 | 0.196 (0.211) → 0.232 (0.259) | 0.323 (0.347) → 0.374 (0.413) |
| ashlar, mid, compacting at 12% | 7.6% (11.5; 12.0) | 8,472,877 → 9,084,684 | 0.204 (0.219) → 0.223 (0.241) | 0.333 (0.357) → 0.365 (0.473) |

**A hole costs the cull about what a live pair it culls does.** The dispatch grows by the holes and the ashlar cull by the same share — 18% at the median, 22% at p95 — because a thread's cost is mostly the binary search over `first_pair`, which a hole pays in full before the mesh it lands on says it has no clusters; the cheaper rejection a per-pair mark could give needs a field `gfx::CullParams` does not have. On the boxes, whose dispatch is mostly the terrain's, it does not show. The compaction share bounds it: at 25% the holes cost the cull at most a third, and measured 0.037 ms at the median; at 12% they stayed under 12% and cost 0.019 ms, for four times the compactions (63 against 15 over the run) and a p95 frame whose passes took 0.47 ms instead of 0.43, because a compaction rewrites both table sets whole. 25% is the default; the tunable is the trade.

**Where the holes come from.** Free blocks more than slack: eight size classes an octave instead of four (at most an eighth of a block slack instead of a quarter, the first group's layout) moved the ashlar kit's median holes from 16.2% to 17.5% and its cull not at all (0.241 ms in both), because a block of the ashlar kit is tens of thousands of pairs and its slack is small beside the blocks the ring's departures free. The layout kept 318,000 blocks unchanged over the ashlar run and 1.3 million over the boxes', rewrote 700–1,035 in place, reused a freed block for 1,341–1,665 tiles, appended 1,119–2,814, and compacted 3 times (boxes) and 9–15 times (ashlar).

### Repeats

The boxes' three repeats drew the same visible pairs at every frame. The ashlar runs' did not at 36–41 frames, and every one of them is the first repeat's: the frames after its per-pair buffers grew (frame 43 with mid blocks, 54 with low), which starts the occlusion history over, and whose pass 1 then tests a larger set for a few dozen frames; the second and third repeats agree frame for frame, because a restart lays the tiles out from scratch as the first fill did. The pictures do not depend on it (the renderer's tests hold the ids, depths and colours to the loaded scene's through every change). With compaction at 12% two frames differed, both the first repeat's, as E35's own rows did.

## What surprised me

- **The comparison is now the cost.** Once nothing is uploaded that did not change, what a change costs the CPU is reading the tail the world hands over — every tile, to find the ones that changed — and the boxes' 21,000 small instances cost twice the ashlar kit's 9,300 large ones. A world that said which tiles changed would pay for those alone.
- **Holes are not cheap threads.** The plan was that a hole is rejected "in its first instruction"; without a field in `CullParams` to mark one, it is rejected after the search that finds its instance, which is most of what any culled pair costs. The compaction share is the whole of the bound.
- **Finer size classes did nothing measurable.** The holes are freed blocks, not slack.
- **Double-buffering costs a copy of every change per set.** With two sets each change is copied and expanded twice, once when each set comes round; at 16 µs a frame it does not matter, and a compaction's full rewrite is 0.3–0.5 ms twice.

## What it decides

- **The insertion is incremental**, and a streamed world's changes cost the renderer a fraction of a millisecond on the CPU and 16 µs on the GPU; E35's "a hitch every second or so at a runner's speed" is gone.
- **The holes cost the cull what they take of the dispatch**, bounded by the compaction share (25% by default).

## What it does not decide

- **A change that names only the tiles that changed.** The world hands the tail over whole; an interface that said which tiles moved would take the comparison off the CPU.
- **A hole rejected before the search.** A per-pair live mark (or the pair table read first, as the resolve reads it) would make a hole nearly free and take the compaction share out of the cull's cost, and needs a `CullParams` field.
- **A heavier world.** The ashlar kit under the default rings (16.7 million pairs at the visibility id's cap) was not run: at the cap the classes' slack packs tight and compacts more often, which the tests cover and this run does not measure.
- **Ray tracing and deformation of a tail**, which E35 left out and this does too.

## Caveats

- **A busy machine**, as the header says; the before and after of a scene ran back to back, and the first group agrees with the second on the before rows.
- **One path, one seed, one tile size**, and a camera that moves seventeen times as far between updates as a runner does between ticks (E35's): more changes than a game makes, and so more holes and more compactions.
- **Two frames in flight, two table sets.** Three frames in flight keep three sets and copy each change three times; the tests run both, the flythrough only two.
