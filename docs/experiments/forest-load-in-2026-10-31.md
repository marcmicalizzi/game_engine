# The streamed forest's load-in under rapid movement (2026-10-31, W81)

**The question.** The owner walks and flies the forest proof of concept's v23 world as a stress test — rapid swings of the view, walking one way and then another, rapid flying — and reports that **flying rapidly drops the frame rate to 1–2 fps while trees and understory load in**. The walking stall of 2026-10-24 was the tile reader's lock and is fixed ([walk stalls](walk-stalls-2026-10-24.md)); this is the streamed placements arriving: the authored instances per tile ([world](../subsystems/world.md#the-consumers), W73) and the renderer taking them ([renderer](../subsystems/renderer.md#instances-that-come-and-go)). Which cost is it, and by how much?

## The flight

- **The scene**: `world/v23/base.json` of the proof of concept, read as data (243,809 authored instances, 64 m tiles in rings of 0.75, 3, 10 and 20 tiles, the ground drawn from the world's tiles), its cache copied to the agent's scratch; the copy of the scene names that copy as the tile set's cache, so the proof of concept's own is never written.
- **The path** (`engine.camera-path.v1`, 40 s at 60 frames a second, written for this from the heightfield): 20 s of flight at 80, 90, 99 and 80 m/s along four legs with three sharp turns, 35 m over the highest ground of each leg, the view swinging with each turn; a drop to eye height; then 20 s of walking at 1.5 m/s — east, north, a stop, three quarter-turns of the view in half a second each, a diagonal.
- **The run**: `msvc-release` engine-view, `--offscreen` at 1600×900, the owner's flags (`--terrain-tiles --terrain-far 6 --time-rate 0 --world-rate 1 --time-of-day 15 --no-lights --lod 1 --rt-budget-mib 0`), `--raster hw --shadows csm` and `--raster rt --shadows rt`, `--benchmark --world-log --startup-trace --wait-quiet 120`, each under the GPU lock, 240 warm-up frames at the first camera and then the path (three repeats before, one after).
- **The machine**: i9-10980XE, RTX 5090, shared with other agents' builds and GPU tests all night. Before, hw: quiet at the start, others 13.4% of the CPU and the GPU 1% busy at the end. Before, rt: still busy after the 120 s wait (others 22.6% of the CPU, the GPU idle), others 22.8% and the GPU 78% busy at the end. So every time below is an upper bound; the counts are not.

**How a slow frame is split.** A frame record's `frame_ms` is the wall time from the previous submission's start to this one's, so it is the *previous* frame's turn: its world update, its ground, its CPU and its waits. Each frame's wall time is therefore attributed to the world log's line and the record of the frame before it. The world log could not split the placements' commit (the renderer taking the tail was one number, `insert_ms`), so this work added `placements` to `engine.world.TileFrame` (version 2): the tiles built, the commit's ranking and tail, and the renderer's change split into the blocks compared, the layout, the slots written, what follows the tables and a relayout's wait ([world](../subsystems/world.md#the-world-log), [apps](../subsystems/apps.md)).

## Before

**Offscreen, the ground dominates — and a window does not pay it.** An offscreen run waits for the drawn ground's rebuild in the frame that asks for it ([renderer](../subsystems/renderer.md#what-a-frame-waits-for)); a window draws the last layout and lets the rebuild run behind. Along the flight that wait is most of every slow frame:

| run (repeat 0) | median | p99 | worst | > 33 ms | > 100 ms | of the slow frames' wall: ground wait · placements (built · commit) · renderer CPU |
|---|---:|---:|---:|---:|---:|---|
| hw, csm | 14.3 ms | 678 ms | 830 ms | 551 | 113 | 70.5 s of 83.2 s · 3.6 s · 6.1 s · 2.8 s |
| rt, rt | 15.2 ms | 910 ms | 1,066 ms | 627 | 118 | 80.9 s of 99.9 s · 4.2 s · 10.4 s · 3.3 s |

A frame of 800 ms to 1 s is the drawn ground building 2 to 27 tiles of the inner rings (the finest at 256 cells a 64 m tile) on the frame's wait: the ground's own cost, and not the trees'. It is what an offscreen measurement of this world shows first, and not what the owner's window shows.

**In a window's terms, the placements.** Taking the ground's wait out of each frame (what a window pays on the frame's thread, ignoring the rebuild's competition for the CPU):

| run (repeat 0) | median | p99 | worst | > 33 ms | > 100 ms |
|---|---:|---:|---:|---:|---:|
| hw, csm | 13.3 ms | 47.4 ms | 75.8 ms | 78 | 0 |
| rt, rt | 13.7 ms | 61.7 ms | 165.0 ms | 141 | 3 |

**Where the placements' time went** (rt, repeat 0, the 2,401 updates of one flight, 1,103 of them commits):

| part | total | what it is |
|---|---:|---|
| tiles built by their generators | 3.6 s | the authored generator laying a tile's instances, a few ms a tile |
| the commit writing the tail | 6.3 s | every drawn tile's instances copied into one span, every commit |
| the renderer comparing the tail | 2.7 s | every held tile compared instance by instance with what its block holds |
| what follows the tables | 3.4 s | the chain's drop order over every slot, the policy counts over the instance table (twice when a tree swayed in), the poser's chunks |
| slots written and buffers fitted | 1.5 s | including 60 compactions a flight, **every one of them tight**: 130,000–177,000 slots each |
| the rest of the sink | 1.5 s | the chain's statistics after a change (`note_rt_bytes`, whose estimate summed the cached pairs over the instance table), the props' fires |
| a relayout's wait | 0.2 s | the stride grew twice |

A commit's share that reads every held tile — the tail written, compared, and the bookkeeping after it — was **10.8 ms at the median** (p90 16.2, worst 23.0), on every commit whatever it changed: 127,000–177,000 instances of the tail at 15.0–15.6 million pairs. **The tail is at the visibility id's ceiling**: the tail's pairs reach 15.6 of the 16.8 million, 34 outer tiles are withheld at 1600×900 (242 at the owner's 11520×2160), and the layout's holes then take its end past the ceiling, so every compaction is the tight one — every slot written again, the table set rewritten whole on the GPU — about once every 18 commits, 25–45 ms each. Those are the worst frames in a window's terms.

So the dominant cost is the hand-over reading the whole tail on every commit (about 14 of the placements' 19 s), as world.md's "Not yet" said; next the tight compactions of a pair space at its ceiling; next the tiles' own building.

## The fix

**The hand-over names each tile with its own instances and a version** (W81). `PlacementTiles::set_run_sink` hands a sink each drawn tile's run — key, place, count, ring, the tile's own instances and a version every build of the tile changes — and writes no tail (`tail()` writes it when asked). `renderer::DynamicBlock` carries the two (`instances`, `version`), and `GpuScene::set_dynamic_instances` keeps a block whose key, count and version it holds without reading one instance; a block with no version is compared as before. engine-view's world hands its tiles over that way, a game's props as one more block from their own list with no version. Nothing about where a block lands, when it is kept, rewritten or placed, or what the frame draws changed: a block kept by its version is one the comparison would have kept.

**What follows a change costs its slots.** The policy counts that every change recounted over the whole instance table (twice when a tree swayed in), and the cached instances' pairs the chain's estimate summed over it for the statistics after every change, are kept slot by slot as the slots are written and trimmed (`GpuScene::policy_count`); a debug build recounts after every change and asserts they agree.

## After

The same path and flags, one flight each, on a busier machine than before (rt: others 40% of the CPU at the start, 59% and the GPU 15% busy at the end; hw: others 35% at both ends, the GPU idle) — so the times are upper bounds, and the counts are exact.

| run | median | p99 | worst | > 33 ms | > 100 ms | window-equivalent: median · p99 · worst · > 33 ms · > 100 ms |
|---|---:|---:|---:|---:|---:|---|
| hw, csm, before | 14.3 ms | 678 ms | 830 ms | 551 | 113 | 13.3 · 47.4 · 75.8 ms · 78 · 0 |
| hw, csm, after | 15.0 ms | 710 ms | 1,134 ms | 459 | 108 | 13.9 · 48.9 · 92.2 ms · 65 · 0 |
| rt, rt, before | 15.2 ms | 910 ms | 1,066 ms | 627 | 118 | 13.7 · 61.7 · 165.0 ms · 141 · 3 |
| rt, rt, after | 14.1 ms | 761 ms | 1,134 ms | 512 | 122 | 11.1 · 62.6 · 208.1 ms · 89 · 4 |

| rt, one flight of 2,401 updates and 1,103 commits | before | after |
|---|---:|---:|
| the commit writing the tail | 6.34 s | 0.01 s |
| the renderer reading the tiles (`compare`) | 2.72 s, every held instance every commit | 0.07 s, 159,234 instances in all — tiles built again with the count they had |
| what follows the tables (`after`) | 3.43 s | 0.25 s |
| the layout, and the rest of the sink (the chain's statistics' estimate over the table) | 1.54 s | 0.06 s |
| slots written, buffers fitted (60 tight compactions both) | 1.48 s | 1.95 s |
| a relayout's wait (the stride grew twice) | 0.16 s | 0.23 s |
| **the renderer taking the tiles, in all** | **9.33 s** | **2.56 s** |
| tiles built by their generators (3,437) | 3.60 s | 4.58 s |
| **the placements on the frame's thread, in all** | **19.4 s** | **7.3 s** |
| a commit's part that read every held tile, median · p90 · worst | 10.8 · 16.2 · 23.0 ms | 0.42 · 0.66 · 2.55 ms |

In hw the renderer's part went to 2.07 s and the placements' to 7.0 s; the tiles' building is 4.8 s there. The frames over 33 ms in a window's terms fell from 141 to 89 (rt) and 78 to 65 (hw); the worst ones did not move, because they are what is left.

**What is left, in the order it costs.** (1) **The tiles' own building**, 4.6 s a flight, 1.3 ms a tile on the frame's thread, several tiles on a frame of the fast legs: the brief's second candidate, laying a tile on the job system ahead of its activation and committing it when ready. (2) **The tight compactions of a tail at the ceiling**: 60 a flight, every slot written again, 30–56 ms each on the frame's thread and the table set rewritten on the GPU; these are most of the frames over 50 ms. Laying the tail out at its classes first, which always fits a tail admitted at its classes, was tried and was worse — the classes leave less than a tile of room at the ceiling, so the next append compacted again: **467** whole rewrites along the same flight, 15.5 s of slots written. Room the admission leaves the layout, or a compaction that moves only the blocks past the cap, would stop it; neither is built. (3) **The stride's growth**, twice a flight, 80–145 ms each, a wait for the device while the pool's and the chain's per-pair tables are made again. (4) Offscreen only, **the drawn ground's rebuild**, which the offscreen run waits for and a window does not; at the inner ring's 256 cells a 64 m tile it is up to a second of the worker for a few tiles.

The owner's own run is at 11520×2160, where the forms keep more clusters, the tail is as near the ceiling (242 outer tiles withheld), and every frame is slower; the hand-over's per-commit cost is the same instances and the same arithmetic, so what this bought there is the same seconds, and what is left is the same list.
**What holds it** (each fails without its change): `world ruins: a run sink is handed each drawn tile's own instances, and a new version only for a tile the update built again` (`placement_tiles_tests.cpp`, no device: the runs are the consumer's own instances in tile order, versions distinct, and a step east keeps 46 tiles' versions and gives the 14 it built again new ones); `engine-view: a streamed world's hand-over reads only the tiles it changed` (`ruins_view_tests.cpp`: a flight east until the four buildings' tiles leave the rings one column before the other, and back — every commit's `compared` is 0 while blocks were kept beside tiles placed and freed, and the last frame is the one a camera that never left draws). The renderer's own tail tests (`dynamic_instances_tests.cpp`, `streamed_tail_tests.cpp`) hold the pictures, and run with the debug build's recount after every change.
