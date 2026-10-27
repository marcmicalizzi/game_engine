# The pool's allocator as a device-wide scan (R104, 2026-10-26)

**Question.** The deformed-vertex pool's allocator (`domain/gfx/shaders/deform_alloc.slang`) was one workgroup walking every visible entry of a run, 256 at a step. The forest proof of concept's report on c1ee971d put it at **15.0 ms** of the grass clearing's frame and **10.4 ms** of the expanded forest's at 11520×2160 — more than the pool pass it feeds. Can the same answer, word for word, come out of a parallel scan, and what does it cost?

**Answer.** Yes: three passes (the tiles' scans, the tiles' cursors in one workgroup, the blocks), described on the [renderer page](../subsystems/renderer.md#the-deformed-vertex-pool). The allocator falls from 14.8 ms to **0.070 ms** at the clearing over three 3840×2160 views, from 18.7 ms (10.4 in the POC's quieter run) to **0.053 ms** in the expanded forest, and from 0.020–0.028 ms to **0.011 ms** on the grove's small cut: the new allocator is cheaper at every cut size measured, the smallest included.

## Why this shape

The rule is the old allocator's and is not a plain prefix sum: entries are taken in tiles of 256 per view, an entry is placed when the cursor plus its exclusive prefix within its tile plus its own count fits the budget, and the cursor advances by the largest inclusive prefix that fitted. While the frame's ask fits the budget — every frame measured here (`deform_overflow_entries` 0 in the POC's reports) — that is an unconditional scan. Past the first tile whose total no longer fits, the placed prefix and the unconditional one part, so the tile cursors from there on are walked serially; the walk reads two words per tile out of shared memory and stops only for a tile that places part of itself, of which there are at most 85 after any crossing (the room the crossing leaves is under 255 vertices, and a partial tile places at least a three-vertex cluster).

Two alternatives were set aside:

- **A single-pass decoupled look-back.** One dispatch, but a tile waits for its predecessor's published prefix, and past the crossing it would wait for the predecessor's *placed* prefix, serially, by spinning. Vulkan promises no forward progress between workgroups; the Maxwell and Pascal runners are the cards a spin would hang on, and a three-pass scan with the render graph's barriers between the passes costs microseconds.
- **A plain prefix sum with a clamp.** Cheaper again, but it charges a refused cluster to the budget and so refuses the small ones behind it — exactly what the allocator's comment says it must not do — and it would change which entries overflow, which the tests hold the scan to.

Every number in the three passes is an integer sum or comparison, so the result depends on the visible list alone; it is the same bytes ten runs in a row (`deform_alloc_tests.cpp`).

## Measurements

`msvc-release`, RTX 5090, engine-view offscreen, the forest POC's frozen v14 scene and camera paths read in place with a scratch copy of its `world/v14/ddc` (`--ddc`), the arguments of its `Reproduce.cmd` otherwise (`--raster hw --shadows rt --lod 1 --deform-pool-mib 64 --rt-budget-mib 1024`, flora motion on, wind 0.7, 600 frames), and `--wait-quiet 300`, which ran out every time: **the machine was never quiet**. The "before" binary is 5552a2b3, the "after" c8acadf8, each built and run from its own checkout so that each loaded its own shaders. Medians over 600 frames from `frames.jsonl`; `machine_state` as each report recorded it at the start and the end.

| run | visible pairs | `deform_alloc` before | after | pool pass before / after | total before / after | machine (start → end) |
|---|---:|---:|---:|---:|---:|---|
| grass clearing, 11520×2160 | 3,520,068 | 14.788 ms (p95 15.41) | **0.070** (p95 0.071) | 2.50 / 2.56 | 68.7 / 52.9 | before: GPU 100% busy (another session's captures), CPU others 25 → 21%; after: GPU 2%, CPU others 27 → 20% |
| expanded forest, 11520×2160 | 2,559,504 | 18.677 (p95 76.0) | **0.053** (p95 0.053) | 4.52 / 2.37 | 200.1 / 84.4 | before: GPU 100%, CPU others 16 → 14%; after: GPU 3% → 0%, CPU others 41 → 100% |
| grass clearing, 1600×900 | 834,251 | 5.749 (p95 68.1) | **0.027** (p95 0.028) | 0.75 / 0.73 | 39.1 / 6.9 | before: GPU 100%; after: GPU 6% → 1%, CPU others 97 → 39% |
| expanded forest, 1600×900 | 826,298 | 3.015 (p95 3.22) | **0.027** (p95 0.029) | 0.82 / 0.80 | 12.3 / 9.7 | before: GPU 2% → 100%; after: GPU 1% → 100% |
| `content/test-scenes/grove`, 1600×900 | 3,617 | 0.020 (p95 0.028) | **0.011** (p95 0.014) | 0.039 / 0.039 | 0.87 / 0.85 | both GPU 100% at one end |
| `content/test-scenes/grove`, 11520×2160 | 5,860 | 0.028 (p95 0.029) | **0.011** (p95 0.012) | 0.040 / 0.039 | 2.60 / 2.59 | both GPU 99–100% at one end |

How to read it:

- **Every "before" forest row ran beside another session's GPU work** (the GPU 100% busy at both ends), so its totals and percentiles are upper bounds; the POC's own quieter runs on c1ee971d are the better "before" for the two wide rows: 15.0 ms at the clearing (the 14.8 here agrees) and 10.4 in the expanded forest (18.7 here, inflated by the load). The "after" rows ran on a GPU that was mostly idle at both ends. The allocator's after-numbers are three small passes and are not what the load inflates; the totals are.
- **The forest frame's entries** per frame, from the POC's reports of the same paths: 229,848 at the clearing and 224,591 in the expanded forest, every one placed (no overflow), 166.7 MB and 158.1 MB of pool. The allocator's cost now scales with that count over the GPU rather than over one workgroup: about 0.3 µs per 1,000 entries at the clearing against 64 µs before.
- **The grove** is E25's scale of cut (its pool is the trees' parts): the three passes and their two extra barriers cost less than the one workgroup did even there, so there is no small-cut regression to trade against.
- The pool pass, the culling and the rasterizers are unchanged by this work; their differences between the columns are the load.

## What the tests hold

`domain/gfx/tests/deform_alloc_tests.cpp` runs the new passes and `deform_alloc_reference.slang` (the old shader, test-only) on the same inputs and requires the same slot table and record, and both against the rule written out on the CPU: a frame that fits; one crossed at a tenth of its ask (948 placed, 8,421 refused) and at nineteen twentieths (8,909 and 460); a zero budget; a 255-vertex cluster at a tile's last lane with three-vertex clusters after it (the next tile places 33 of them in the 100 vertices it left: 288 placed, 712 refused); three views of a camera run with a count past the dispatch's bound and an empty view, three camera runs and four cascades with the rest pose by flag and by distance; a 1.2-million-entry frame past the 2,048-group grid (779,406 placed fitting, 389,817 at half the ask); and ten runs of one input to the same bytes. The renderer's pool tests and `engine_view.frame_loop` (no allocation and no new buffer in the frame loop) pass unchanged.

## What is open

- Which entries overflow still follows the cull pass's append order, which is not reproducible; the scan reproduces the old allocator exactly, including that ([gfx](../subsystems/gfx.md), "Not yet on the GPU side").
- A frame crossed at its first tile walks every tile serially in one thread: as many steps as the old loop took, each a few shared-memory reads. It was not measured on a real frame, because none of the scenes here overflows; the tests exercise it crossed at a tenth of a 9,369-entry ask and at half of a 1.2-million-entry one.
