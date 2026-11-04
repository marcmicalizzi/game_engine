# The hitches of the owner's v27 walk (R130, 2026-11-03)

Roadmap R130, with R128 and T71 part 3. The owner walked the forest proof of concept's v27 at 3840×2160 HDR10, `--raster rt --shadows rt`, 416 animals and the creator body, windowed and uncapped (`D:/workspace/forest_wildlife_poc/reports/lab/20261103-095454-764/`, 59,826 frames), and saw "occasional jumps to 160 ms for a frame or few". Twenty records have a `frame_ms` over 100 ms. This page is what those records say once read against the frames before them, what the record could not say and now does, and what was changed.

## Reading the records

A record's `frame_ms` is the interval *ending* at that frame's start, so the frame a hitch belongs to is the one before the record that shows it; and a windowed record's `wait_ms` is the whole `begin_frame` call — the slot's wait (`cpu.wait`) **and** the rest (`cpu.begin`), which holds a step of the ray tracing chain. The coordinator's first extraction read the long `wait_ms` as the GPU's in every case; splitting it by `cpu.wait` and `cpu.begin`, and reading `gpu_ms` on the frames one to three before, the twenty fall into four classes. (Extraction: `Select-String` over `frames.jsonl` for the records named, read as text; the frames listed are the record numbers.)

| class | where the time is | frames (record whose interval it is) | the named numbers |
|---|---|---|---|
| **A. the drop rule on the GPU** | `gpu_ms.rt` 118–168 ms, of which `clas` + `blas` 2–12; the chain's other zones had no bucket | 21125, 38378, 42013–14, 55129, 55953–54, 56131–32, 56412–13, 58976–77 — two frames a step, then a `cpu.wait` of 130–168 ms on the frame that reuses their slot | every one is a frame past its capacity (`rt_wanted > rt_built`), or one whose cached references sat at their capacity (42013–14 at 24,576, 21125 and 58976 at 53,24x): the frames that run the drop rule — which one, the record could not say; the replay below found the one-thread rule inside `ranges_main` |
| **B. a step of the cache's references, waited for** | `cpu.begin` 64–205 ms | 21126 (204.8), 42015 (181.5), 55956 (63.8), 55957 (94.8), 57115 (66.3), 58978 (191.4) | `rt_references` steps on each (59,677 from 53,242; 40,262 from 24,576; 84,464 from 49,152; 68,097 from 53,245); the chain's own capacity does not move. `resize_ray_tracing` refused to swap a references step (one host-visible table named them) and waited for the device |
| **C. a first build at a new capacity** | `gpu_ms.blas` 103 ms in a frame of 1,193 structures | 7390 | the capacity had just stepped (172,032 ⇄ 338,115 early in the run) |
| **D. the world's own work and time no field named** | `world.total_ms` 20–30, `tiles_ms` 14–15, and 130 ms more that no field holds | 1318, 1326 | no wait, no resize, GPU 8–9 ms: the record could not say where the rest went |

Class A is the largest (eleven of the twenty intervals carry it, 118–168 ms each) and class B the next (six, 64–205 ms); the two often follow one another, because a turn into the woods both overflows the chain and steps the references. R128's measured 254.75 ms swapped growth (248.6 of it allocating the bottom-level set) does not appear on v27: the chain's own growths there were swapped and cost 3–6 ms of `cpu.begin`.

## What the record now says

- **`sections`** (`FrameRecord` version 15, `engine.scene.FrameSections`): the frame's own interval partitioned into the loop's spans — host, slot wait, begin (with the resize inside it), fold, acquire, ticks, submit, after, present, tail — and `other_ms`, the remainder. Every host fills it (flights in both hosts through `renderer::frame_sections`, the window, the offscreen replay). Class D's 130 ms, in a record written now, lands in one named span.
- **`gpu_ms`** (`PassMs` version 8): the chain's zones `rt_records`, `rt_ranges`, `rt_drop` (the walk, a timer zone of its own, "ranges drop"), `rt_emit`, `rt_cache`, `clas_cache`, `tlas`; `terrain_upload`; and `unbucketed`, the total less every bucket.
- **`cpu.resize`** (`CpuMs` version 2): the step's share of `cpu.begin`.

## What was changed

Class B: **a step of the cache's references is swapped in, not waited for.** The cache's table has a copy a frame slot, written when the slot's frame is recorded, so the frames in flight keep the references they were recorded with; the three reference buffers go to the slot's deferred frees with the rest ([renderer](../subsystems/renderer.md#a-resize-is-swapped-in-not-waited-for)). `renderer.rt.swap_references=false` is the old rule. The test `rt capacity: a step of the cache's references swapped in flight draws the same` steps the references three times with two frames in flight: swapped, no stop for the device and the same ids and colour as a chain that never steps; waited, one stop a step.

Class A, once the replay named it (below): **the frames that first pass the capacity record the drop order's scan.** The record's new buckets put the 120 ms in `rt_ranges` and none in `rt_drop`: the scan's passes were recorded only within 64 frames of one that dropped, the demand reaches the frame thread two frames late, so the first two frames of every overflow after a quiet stretch took the one-thread rule inside `ranges_main` (R106's fallback). A frame now records the scan also while the last read-back demand, of either kind, is at least half the capacity; on a frame that fits, its three dispatches have no groups and cost 7 µs (the median over 212 fitting frames with the scan recorded; p95 11 µs). `renderer.rt.scan_near_capacity=false` is the old rule.

And a references step that leaves the capacity alone **keeps the frame's own set** (records, record table, CLAS set, the scratch when it is large enough): the first replay with the swap but without that made the 338,115-cluster CLAS set again beside the old one, and the driver took 937 ms over it on a card other processes held 21 GB of.

## The replay

The owner's poses, one key every three recorded frames over owner frames 55,000–56,500, 58,900–59,100 and 41,900–42,100 (635 path frames, two cuts between the ranges), flown offscreen at 3840×2160 with the run's flags (`--raster rt --shadows rt --terrain-tiles --terrain-far 6 --lod 1 --deform-pool-mib 64 --rt-budget-mib 0 --no-lights --time-of-day 15 --world-rate 1 --time-rate 0`, the flora's motion and wind, micromaps off), `--benchmark --repeat 1 --warmup 30 --wait-quiet 120`, from the run's `scene.json` copied out and the v24 derived-data copy, `msvc-release`, RTX 5090, one process each, under the GPU lock. The 416 animals stand in the scene without the module, static. Before is `renderer.rt.swap_references=false renderer.rt.scan_near_capacity=false` (the rules at 85eda802), after both true, in the same binary. **The machine was shared**: the harness measured other processes at 10–100% of the CPU and the GPU 5–99% busy at the runs' ends, with 21 GB of the card held by others, so every number is an upper bound.

| | before | after |
|---|---:|---:|
| frames whose GPU time passed 100 ms (all `rt_ranges`, 119–122 ms) | 4 (377, 378, 471, 472) | 1 (471) |
| frames whose slot wait passed 100 ms (the GPU of the two before) | 4 | 1 |
| the references' step (frame 211) | 59.8 ms, waited | 1.5 ms, swapped |
| frames whose wall time passed 100 ms / 50 ms | 15 / 24 | 13 / 30 |
| of them, the host's turn (the offscreen world waiting for its tiles) | 11 | 12 |
| largest frame not the host's | 155.8 ms | 153.6 ms (473, behind 471) |
| median frame | 23.0 ms | 24.0 ms |
| capacity steps of the chain | 0 | 0 |
| `sections.other_ms`, largest; `gpu_ms.unbucketed`, largest | 0.000; 0.000 | 0.000; 0.000 |

The path reproduces class A at both overflows (owner frames 56,131–32 and 56,412–13 land on path frames 377–378 and 471–472), and class B once (frame 211, a references step at a held capacity). It does not reproduce the chain's own capacity steps — the replay holds 338,115 from its first frame — nor class C. The one frame left (471) is a demand that went from under half the capacity to past it within two frames: at a key every three recorded frames the replay turns three times faster than the owner did; at the owner's own pace that jump was 38% of the capacity at most. The rest of the frames over 100 ms are the host's turn, the offscreen replay's world completing its tiles synchronously (565–907 ms on the path's cuts and its fastest stretches), which a window budgets across frames instead — the owner's 1318 and 1326 are that class at the window's scale (20–30 ms of `world.total_ms`), and what else those two frames held is what `sections` says from the next recording on. The medians are inside the shared machine's spread.

## What is next

- **The one-thread rule itself** (`clas_records.slang`, `domain/gfx`): a frame past its capacity that is recorded without the scan still costs 120 ms at 225,000 instances; recording the scan on every frame would remove it at 7 µs a fitting frame, or the rule could leave `ranges_main`. Either is R128's.
- **A larger step while the demand's slope is positive** (R128), so a turn into the woods overflows once.
- **The counts naming their own capacity** (R128's second finding), not touched.
