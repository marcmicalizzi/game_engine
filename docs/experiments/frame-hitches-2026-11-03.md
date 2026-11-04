# The hitches of the owner's v27 walk (R130, 2026-11-03)

Roadmap R130, with R128 and T71 part 3. The owner walked the forest proof of concept's v27 at 3840×2160 HDR10, `--raster rt --shadows rt`, 416 animals and the creator body, windowed and uncapped (`D:/workspace/forest_wildlife_poc/reports/lab/20261103-095454-764/`, 59,826 frames), and saw "occasional jumps to 160 ms for a frame or few". Twenty records have a `frame_ms` over 100 ms. This page is what those records say once read against the frames before them, what the record could not say and now does, and what was changed.

## Reading the records

A record's `frame_ms` is the interval *ending* at that frame's start, so the frame a hitch belongs to is the one before the record that shows it; and a windowed record's `wait_ms` is the whole `begin_frame` call — the slot's wait (`cpu.wait`) **and** the rest (`cpu.begin`), which holds a step of the ray tracing chain. The coordinator's first extraction read the long `wait_ms` as the GPU's in every case; splitting it by `cpu.wait` and `cpu.begin`, and reading `gpu_ms` on the frames one to three before, the twenty fall into four classes. (Extraction: `Select-String` over `frames.jsonl` for the records named, read as text; the frames listed are the record numbers.)

| class | where the time is | frames (record whose interval it is) | the named numbers |
|---|---|---|---|
| **A. the drop walk on the GPU** | `gpu_ms.rt` 118–168 ms, of which `clas` + `blas` 2–12; the chain's other zones had no bucket | 21125, 38378, 42013–14, 55129, 55953–54, 56131–32, 56412–13, 58976–77 — two frames a step, then a `cpu.wait` of 130–168 ms on the frame that reuses their slot | every one is a frame past its capacity (`rt_wanted > rt_built`), or one whose cached references sat at their capacity (42013–14 at 24,576, 21125 and 58976 at 53,24x): the frames that run the drop walk (`ranges_drop_main`, one workgroup) |
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

Class A is not changed here: the walk is `domain/gfx`'s (`clas_records.slang`), which this row could not edit. It is the next item of R128, with the request in the batch report.

## The replay

See below (to be filled by the reproduction).
