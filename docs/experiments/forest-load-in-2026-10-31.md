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
| the rest of the sink | 1.5 s | the props' fires over the tail, the chain's first sizing |
| a relayout's wait | 0.2 s | the stride grew twice |

A commit's share that reads every held tile — the tail written, compared, and the bookkeeping after it — was **10.8 ms at the median** (p90 16.2, worst 23.0), on every commit whatever it changed: 127,000–177,000 instances of the tail at 15.0–15.6 million pairs. **The tail is at the visibility id's ceiling**: the tail's pairs reach 15.6 of the 16.8 million, 34 outer tiles are withheld at 1600×900 (242 at the owner's 11520×2160), and the layout's holes then take its end past the ceiling, so every compaction is the tight one — every slot written again, the table set rewritten whole on the GPU — about once every 18 commits, 25–45 ms each. Those are the worst frames in a window's terms.

So the dominant cost is the hand-over reading the whole tail on every commit (about 14 of the placements' 19 s), as world.md's "Not yet" said; next the tight compactions of a pair space at its ceiling; next the tiles' own building.

## The fix

**The hand-over names each tile with its own instances and a version** (W81). `PlacementTiles::set_run_sink` hands a sink each drawn tile's run — key, place, count, ring, the tile's own instances and a version every build of the tile changes — and writes no tail (`tail()` writes it when asked). `renderer::DynamicBlock` carries the two (`instances`, `version`), and `GpuScene::set_dynamic_instances` keeps a block whose key, count and version it holds without reading one instance; a block with no version is compared as before. engine-view's world hands its tiles over that way, a game's props as one more block from their own list with no version. Nothing about where a block lands, when it is kept, rewritten or placed, or what the frame draws changed: a block kept by its version is one the comparison would have kept.

**What follows a change costs its slots.** The policy counts that every change recounted over the whole instance table (twice when a tree swayed in) are kept slot by slot as the slots are written and trimmed (`GpuScene::policy_count`); a debug build recounts after every change and asserts they agree.

**What holds it** (each fails without its change): `world ruins: a run sink is handed each drawn tile's own instances, and a new version only for a tile the update built again` (`placement_tiles_tests.cpp`, no device: the runs are the consumer's own instances in tile order, versions distinct, and a step east keeps 46 tiles' versions and gives the 14 it built again new ones); `engine-view: a streamed world's hand-over reads only the tiles it changed` (`ruins_view_tests.cpp`: a flight east until the four buildings' tiles leave the rings one column before the other, and back — every commit's `compared` is 0 while blocks were kept beside tiles placed and freed, and the last frame is the one a camera that never left draws). The renderer's own tail tests (`dynamic_instances_tests.cpp`, `streamed_tail_tests.cpp`) hold the pictures, and run with the debug build's recount after every change.
