# The frame at 11520×2160, and the walk's long frames (R195, 2026-11-21)

Roadmap R195. The owner, 2026-11-20: "4K is smooth, 11520x2160 still choppy"; and a five-minute walk of the forest at 4K (traced primaries and shadows, the bounce with its guide and memory on) measured a median frame of 16.8 ms (GPU 16.0), p95 29.8, p99 56, the worst 248, eleven frames over 100 ms. This page is the table by pass at the two widths on the engine's staging copy of the forest, the long frames of the staging's walk attributed by their records, the one thing removed tonight and the ranked rest. It follows [R135's cost map](frame-cost-map-2026-11-07.md), which did the same on the proof of concept's v34 with every resolve stage; the numbers here are today's main on the staging, with the bounce's guide and memory on as the owner walks.

**In four lines.** At 11520×2160 the frame is GPU-bound at 53 ms (19 Hz; 58.6 ms with the session unlocked) where 4K is 18: the **primary rays are 5.4 times** the 4K ones for 3.0 times the pixels and the **resolve 3.2 times**, together 39 of the 53 ms, because the two side monitors look along the floor through the crowns; nothing runs out of device memory (17.8 GB of the 31.6 the driver budgets, at the peak) and the traced chain never overflows (its demand peaks at 175,214 clusters of 337,597). **The one cut tried tonight returned nothing**: the renderer's page said a surround's passes, timed one zone a view, run their views one after another; timed as one zone a pass the wide frame's GPU is 52.19 ms against 52.77 and its wall 54.08 against 53.31 — under 1 %, inside the run-to-run spread — so the views were not being held apart in any way that costs, and nothing safe that is the frame's was found to remove (the ranked list is what would). The walk's long frames at 4K on this shared machine are mostly the frame thread waiting for a GPU whose own timed frame was a fifth of the wait, then the recording and the beginning of the frame preempted by other processes; the world's own stalls (a hand-over's write with a relayout, a commit, a far ring, a tile on the frame thread) are a handful a walk.

## How it was measured

- **The scene and the flags**: the forest staging (`<scratch>\v41\scene-staging.json`, the derived-data cache through a root of the row's own: junctions to the cache's read-only directories and hard links of its clusters, far forest and textures, so nothing was written into the staging; the materials of `materials-b69`), offscreen, `--raster rt --shadows rt --no-lights --time-of-day 15 --world-rate 1 --lod 1 --deform-pool-mib 64 --rt-budget-mib 0 --time-rate 0 --terrain-tiles --terrain-far 6 --terrain-async`, flora motion at wind 0.7, micromaps on, `renderer.gi.mode=bounce` **with `renderer.gi.guide=true` and `renderer.gi.memory=true`**, `--memory-report`, `--wait-quiet 60` to `150`. 3840×2160 is one view; 11520×2160 is `--views surround3`, the owner's layout.
- **The path**: the staging's grounded walk (`walk-grounded.json`: x 180 to 540 at z 1509, 4.5 m/s, the eye 1.7 m over the ground; 2,400 frames at 30 fps, the path batches 65 to 67 used for the stall rows), flown once. The first 30 records are left out of every percentile.
- **The binaries**: main's release build at 2de3230c (the code of main 4a51ab021), copied out of the tree, and this row's tip, `msvc-release`, the release flags' default.
- **The machine**: RTX 5090 (32,607 MiB), 36 threads, shared tonight with five other rows' builds and GPU cases. **No flight was quiet by the harness's rule**: the CPU's other processes stood at 30 to 100 %, so every CPU number is an upper bound; the GPU numbers are the pass timers'. Each flight's state is in its row below. **A locked session moves the GPU's numbers**: the same 4K walk read 20.2 ms of GPU at the median unlocked (beside a build) and 18.1 locked, so this page pairs only flights of one state.
- **What a record's frame time is**: a record's `frame_ms` is start to start, the frame before it ([apps](../subsystems/apps.md)); the long frames below are each record's own `sections.wall_ms`, with its own sections beside it.

| flight | binary | size | session | others' CPU (start / end) |
|---|---|---|---|---|
| A-4k | main | 3840×2160 | unlocked | 100 % / 100 % (beside this row's own release build: not used for causes) |
| A-wide | main | 11520×2160 | unlocked | 66 % / 41 % |
| B-4k | main | 3840×2160 | locked | 79 % / 100 % |
| C-wide-off | tip, `renderer.timing.view_zones=false` | 11520×2160 | locked | 80 % / 30 % |
| C-wide-on | tip, `view_zones=true` (main's code path) | 11520×2160 | locked | 100 % / 32 % |

## 1. The table by pass at the two widths

Main's code path, the same walk: 4K is B-4k (main's binary), 11520 is C-wide-on (the tip with the switch at its default, which records exactly main's commands); both with the session locked, the same night. A-wide (main, unlocked, 16:00 the day before) read 58.57 ms of GPU at the median, `resolve` 25.17, `trace` 18.02: the unlocked desktop costs the wide frame about a tenth.

| GPU ms | 3840×2160, median / p95 / p99 | 11520×2160 surround3, median / p95 / p99 | ratio of the medians |
|---|---:|---:|---:|
| `total` | 18.08 / 21.85 / 22.63 | 52.77 / 64.28 / 65.79 | 2.92 |
| `resolve` | 7.26 / 8.77 / 9.72 | 23.07 / 28.88 / 31.39 | 3.18 |
| `trace` | 3.00 / 4.80 / 5.05 | 16.29 / 22.42 / 24.80 | 5.44 |
| `rt` | 3.26 / 3.76 / 3.85 | 5.15 / 5.72 / 5.89 | 1.58 |
| `clas` | 2.01 / 2.30 / 2.37 | 3.13 / 3.60 / 3.75 | 1.56 |
| `blas` | 0.41 / 0.60 / 0.61 | 0.71 / 0.83 / 0.86 | 1.71 |
| `tlas` | 0.49 / 0.55 / 0.56 | 0.52 / 0.57 / 0.58 | 1.06 |
| `rt_records` | 0.13 / 0.14 / 0.14 | 0.37 / 0.38 / 0.40 | 2.76 |
| `rt_emit` | 0.09 / 0.10 / 0.10 | 0.26 / 0.27 / 0.27 | 2.74 |
| `gi_trace` | 1.34 / 1.69 / 1.79 | 2.20 / 2.70 / 2.96 | 1.64 |
| `gi_fold` | 0.38 / 0.48 / 0.57 | 0.60 / 1.16 / 1.51 | 1.60 |
| `deform` | 0.79 / 2.19 / 2.31 | 1.52 / 2.98 / 3.13 | 1.93 |
| `frame_passes` | 0.75 / 1.00 / 1.03 | 1.66 / 2.65 / 2.71 | 2.20 |
| `half_sun` | 0.00 / 0.00 / 0.00 | 0.58 / 1.08 / 1.46 | — |
| `cull` | 0.19 / 0.85 / 0.96 | 0.43 / 1.11 / 1.17 | 2.29 |
| frame (wall) | 18.26 / 38.24 / 60.33 | 53.31 / 64.88 / 66.44 | 2.92 |
| CPU (cpu_ms) | 2.25 / 8.47 / 22.46 | 1.93 / 3.00 / 4.98 | 0.86 |
| recording (submit_ms) | 2.24 / 8.46 / 22.45 | 1.93 / 3.00 / 4.97 | 0.86 |
| host (host_ms) | 1.48 / 5.78 / 26.56 | 1.22 / 3.20 / 6.34 | 0.82 |
| slot wait | 13.17 / 23.84 / 42.53 | 49.31 / 61.67 / 63.23 | 3.74 |
| visible pairs | 423,356 / 495,006 / 507,448 | 985,252 / 1,102,043 / 1,111,592 | 2.33 |
| shadow casters | 589,276 / 2,302,903 / 2,318,012 | 750,266 / 2,875,783 / 2,955,050 | 1.27 |
| chain wanted | 92,242 / 105,114 / 107,919 | 148,714 / 168,436 / 175,214 | 1.61 |
| chain capacity | 135,168 / 337,597 / 337,597 | 337,597 / 337,597 / 337,597 | 2.50 |

**What scales how.**

- **Faster than the pixels: the primary rays** (`trace`, 5.4× for 3.0× the pixels). The centre monitor of a flat surround is the single 4K view to the pixel ([renderer](../subsystems/renderer.md#multi-view-a-viewset-over-one-scene)), so the two side monitors cost about 4.4 times what the centre does, more than twice its rays' time each: they look along the forest's floor at a slant, through more crowns, so each ray walks further through more boxes before it commits. R135 found the same on v34 (the side views' trace 16.1 ms against the centre's 4.8).
- **About as the pixels: the resolve** (3.2×; the sun's shadow ray through the side views' crowns, R135's 31 ms of masks in the sides on v34), the chain's records and emit (2.8×: they run over the visible list, which is 2.3× as long), the cull (2.3×), the medium (`frame_passes`, 2.2×).
- **Slower than the pixels**: the deform pool (1.9×: the swaying plants in view), the bounce (`gi_trace` 1.6×, `gi_fold` 1.6×: the cache is in world space; its samples follow the pixels, its fold the cells it touched), the chain's build (`clas` 1.6×, `blas` 1.7×: the frame's union of cuts is 1.6× the 4K one — the views see nearly disjoint thirds of the world, but the centre third holds most of what is near).
- **Not at all**: the top-level build (`tlas`, one structure over every slot), the tables, the sky.
- **Only at the wide size**: `half_sun`, 0.58 ms, R126's half-rate sun pass, on by default on a frame wider than 2.5:1.
- **The CPU's side does not grow**: the recording and the host's turn are the same at both widths (1.2 to 2.2 ms at the median); the frame waits for the GPU, 49 ms of its 53.

**Memory and the chain.** Device memory at the peak (the exit snapshot, `--memory-report`): 14.5 GB at 4K and **17.8 GB at 11520** of the 31.6 GB the driver budgets — nothing runs short; with tonight's memory rows (5.4 GB off the first frame's peak at 4K, not on this base) the wide frame would stand at about 12.4 GB, inside a 16 GB card. Of the 3.3 GB the wide frame adds: the frame's own buffers 1.67 → 2.50 GB (the visible list's runs and tables, three a view — W51-5, joining tonight, sizes them by the frame), the traced chain 2.72 → 4.90 GB. **The chain is the one thing sized past what the wide frame needs**: its demand peaks at 175,294 clusters, so it never overflows and drops nothing, but it holds the budget's 337,597 for the whole walk (2.84 GB of frame sets), because the shrink rule waits for a window that peaks under half of what it holds and the wide demand's p95 (167,298) sits just under that line and its peak just over; at 4K it shrank to 135,168 at frame 448. The chain's first capacity is W51-6's ground (joining tonight).

## 2. The walk's long frames at 4K

B-4k, main, every record over 50 ms by its own wall time, each given the section that held most of it (the slot wait is called the GPU's when the GPU's own timed frame is at least four fifths of it):

The flight: 2,370 frames measured, median 18.26 ms, p95 38.24, p99 60.33, the worst 177.2; **55 over 50 ms and 2 over 100** in 80 s of walking — 1.5 a minute over 100 ms, against the owner's 2.2 a minute (11 in five minutes) on a live session.

| cause (the section that held most of the frame) | frames | their wall ms | the four longest (frame, wall, the section's ms) |
|---|---:|---:|---|
| slot wait past the GPU's timed frame | 26 | 1,619.5 | 493 (97.4, 66.1), 287 (92.0, 82.1), 282 (79.0, 75.7), 311 (78.2, 74.7) |
| recording: the passes | 8 | 510.7 | 317 (89.3, 78.0), 299 (75.7, 63.7), 1904 (70.9, 46.2), 113 (63.3, 51.3) |
| the frame's beginning, outside its slot wait | 5 | 346.2 | 201 (108.0, 93.8), 208 (73.4, 58.3), 137 (60.5, 41.0), 903 (53.3, 32.8) |
| recording: the submit | 4 | 260.9 | 1996 (81.9, 63.9), 526 (61.9, 31.9), 544 (61.7, 48.9), 463 (55.5, 41.1) |
| the world's update, its named parts under 40 % of it | 4 | 239.8 | 499 (72.3, 48.1), 289 (62.0, 58.8), 234 (53.2, 50.2), 506 (52.2, 50.0) |
| the world: a hand-over's slot write (with a relayout) | 1 | 177.2 | 1579 (177.2, 137.8: write 77.6, relayout 57.8) |
| recording: the tables | 2 | 137.5 | 1 (80.6, 37.6), 455 (57.0, 44.6) |
| recording: the record | 2 | 132.1 | 265 (81.6, 68.1), 1757 (50.5, 32.2) |
| the world: the placements' tail committed | 1 | 78.2 | 197 (78.2, 74.6) |
| the world: a far ring's hand-over | 1 | 54.4 | 2244 (54.4, 36.0) |
| the world: a tile built on the frame thread | 1 | 53.8 | 726 (53.8, 51.4) |

The two over 100 ms are the hand-over at 1579 and a beginning at 201 (93.8 ms in `begin_frame` outside its wait).

- **The largest class is the frame thread waiting for a GPU whose timed frame was short**: 26 frames that waited 42 to 82 ms for a slot while the frame's zones summed 13.5 to 18 ms. They come in pairs (a wait of two frames' worth and then a frame with none), and the wait is two to five times what the two frames' zones add to. The record has no first-to-last timestamp of the GPU's frame, so what filled the rest — work outside any zone, another process on the GPU (the owner's own sessions take no lock), or a waking thread delayed by a CPU at 79 to 100 % — cannot be split from the records; a GPU frame span in the record is the first item of the ranked list.
- **Then the recording and the beginning of the frame, preempted**: 21 frames whose `submit` or `begin` held 32 to 94 ms while the same work takes 2 ms at the median; their slowest pass is a different pass each time (in A-4k, beside a build, `ranges count` 45 ms, `tlas` 48, `trace` 116), which is the thread descheduled, not a pass.
- **The world's own stalls are few and large**: a hand-over's slot write (frame 1579: 3,904 slots written, 77.6 ms, and a relayout of 57.8 ms in the same hand-over: 177 ms), a commit of the placements' tail (72.9 ms), a far ring's hand-over (36 ms), a tile built on the frame thread (51 ms), and four frames whose world update named under a tenth of its time (48 to 59 ms unnamed). A-4k's flight (not used for causes) also met one table growth's relayout of 447.9 ms inside a 572.7 ms frame — T152's "a device too full for the new half still waits".

## The views timed as one zone

**The claim tested.** Every `GpuTimer` zone writes its two timestamps at ALL_COMMANDS, and a multi-view pass — the cull, the deform pool, the index expansion, the two rasterizers, the primary rays, the resolve — opened one zone a view inside its view loop, so the renderer's page said "timing the views separately orders them on the queue" and called the frame's total an upper bound. If that were so, a surround's three dispatches of rays and three draws of the resolve would each wait for the last to drain, a cost that grows with the views and with how unevenly a dispatch's rays finish — the side views' are the long ones. `renderer.timing.view_zones` (true, the commands main records) false times each such pass as one zone, the first view's, and leaves the views free to overlap; the picture cannot tell (`renderer: a surround's views timed as one zone a pass draw the same bytes`, colour and ids to the byte).

**The pair** (the tip's one binary, the walk at 11520×2160, `surround3`, locked, 00:46 and 01:41; the CPU's others 80 → 30 % and 100 → 32 %):

| | zones a view (main's commands) | one zone a pass | |
|---|---:|---:|---:|
| GPU total, median / p95 / p99 | 52.77 / 64.28 / 65.79 | 52.19 / 63.33 / 64.92 | −1.1 % |
| `resolve` | 23.07 / 28.88 / 31.39 | 22.69 / 28.00 / 30.47 | −1.6 % |
| `trace` | 16.29 / 22.42 / 24.80 | 16.23 / 22.36 / 24.76 | −0.4 % |
| the frame's wall, median / p95 | 53.31 / 64.88 | 54.08 / 66.52 | +1.4 % |
| slot wait, median | 49.31 | 49.49 | +0.4 % |
| visible pairs, chain wanted (median) | 985,252 · 148,714 | 985,114 · 148,714 | |

**It returns nothing.** The GPU's own zones say 0.6 ms (1 %) and the frame's wall, which is what a player sees and which no zone choice can bias, moved the other way by as much. The timestamps do not hold the views apart in any way this card shows at these sizes: each view's dispatch already fills the device, and what overlapping their ends would save is under the flights' spread. The switch stays, default true, as the comparison it is (a weaker card or another driver can be asked the same question with one flag), and the renderer's page now says what was measured instead of what was assumed.

## 3. What was removed, and the ranked rest

**Nothing was removed.** The one candidate that was the frame's and could not change a pixel was the zones above, and it returned nothing; every larger lever either changes the picture, is the owner's setting, or is another row's ground tonight. The ranked rest, by what it would buy at 11520×2160 on this walk:

| # | Lever | Buys at the wide size (bound) | Picture | Risk, size | Whose |
|---:|---|---|---|---|---|
| 1 | **The primary visibility rasterized** (`--raster hw`, the engine's default) | most of `trace`'s 16.3 ms: R135 measured 21.0 → 8.8 ms on v34's forest pose at this width, and the chain smaller | none: the visibility buffer is the rays' word for word (the vertex and ray tests) | none in code; the owner judges performance under traced primaries, so it is the owner's flag | the owner |
| 2 | **The sun's masks by region** (R139's `renderer.shadow.solid_sides`, off) | of `resolve`'s 23.1 ms, the side views' sun stage: R139 returned 19.3 ms on v35's forest pose with the sides solid outright | changes: crown-sized blocks of shade in the side views and a seam at the monitor's edge (R139's counts) | a stated picture change; proposed, not made | the owner's choice |
| 3 | **Casters out of the primary rays' way** (R140: the shadow casters in an instance or a structure the primary rays' mask leaves out) | part of `trace` and the masks' cost to the sun ray; the wide frame carries 2.9 million casters at p95 beside 1.1 million drawn pairs | none if done to the tests' contract | L; the chain's layout | R140 |
| 4 | **A GPU frame span in the record** (first to last timestamp, beside the zones' sum) | nothing by itself; it splits the 4K walk's largest class of long frames (26 of 55: a slot wait 2–5× the timed frame) between untimed GPU work and a late wake | none | S | proposed |
| 5 | **The chain's capacity at the wide size**: it holds the budget's 337,597 clusters (2.84 GB of frame sets) the whole walk for a demand that peaks at 175,214, because the wide demand's p95 sits just under the shrink line (half of what it holds) and its peak just over | memory, not time: about 1.4 GB on the device | none | M; the first capacity and the shrink rule | W51-6 (joining tonight), R152's rule |
| 6 | **The world's frame-thread stalls at 4K**: a hand-over's slot write with a relayout (177 ms), the tail's commit (78 ms), a tile built on the frame thread (54 ms), a far ring's hand-over (54 ms), and four updates whose parts name under 40 % of them (48–59 ms) | the walk's 4K frames over 100 ms (2 in 80 s here, 11 in the owner's five minutes); nothing at the wide size, which is GPU-bound | none | S to name the unnamed parts; M each to move a stall off the frame thread | T152 (the relayout), proposed |
| 7 | **Per-view cuts in the chain** (`--peripheral-lod` is inert under traced shadows: one cut for every view) | the side views' clusters at a coarser LOD: part of `clas`, `deform`, `trace` | changes by design (periphery) | L | proposed in R135 |
| 8 | **`half_sun`** at the wide size | 0.58 ms, but it is the pass that already saves the sides' resolve (R126) | — | — | — |

**What tonight's memory rows change here** (not on this base): they take 5.4 GB off the first frame's device peak at 4K; the wide frame's 17.8 GB would be about 12.4 GB, inside a 16 GB card, before item 5.

## W155: the walk's own long frames

Roadmap W155, the same night. Section 2's table left two classes it could not split: the 26 slot waits two to five times the GPU's timed frame, and four world updates whose parts named under 40 % of them. This section is the record made able to tell, the walk flown again with it, and what was removed.

**What the record says now** (`FrameRecord` version 25; [renderer](../subsystems/renderer.md#what-a-slot-wait-was), [apps](../subsystems/apps.md#flythroughs), "The frame record's GPU clock"):

- `gpu_frame`: the frame's first command to its last on the GPU's clock (`span_ms`), what of it no zone names (`untimed_ms`), how long after its submission the GPU began it (`queued_ms`) and, of that, how long after the GPU had finished our frame before it (`elsewhere_ms`); and the frame's own slot wait split by the frame it waited for — the GPU inside that frame (`wait_gpu_ms`), not yet started on it (`wait_before_ms`), after it until the thread woke (`wait_late_ms`). The marks are two timestamps a frame; the host's clock comes from `VK_KHR_calibrated_timestamps`, one call every half second.
- `world` version 2: `update_ms` by who spent it — the ring, the drawn ground's calls, the placements' calls (events and commits) and the re-standing — and the placements' calls by part (events, due tiles, meshes, hand-off cells, admission, tail, hand-over).
- `frame_ms` is the frame's own interval, `sections.wall_ms`; until version 24 it was the one before.
- `sections` version 2: the frame thread's own CPU time over the frame (`thread_cpu_ms`) and the wall time beyond it (`off_cpu_ms`), so a long recording or update that held little of the CPU says it was the thread descheduled. On Windows the figure is the system's thread times, good to a 15.6 ms clock tick; the startup trace's stages carry the same per stage (`cpu_ns`), which is how R195's flights are read again below.

**R195's "world's own" stalls, read again by their thread CPU.** R195's flights kept their startup traces, whose stages carry the frame thread's CPU. Of B-4k's walk (from 311 s into its trace), the stages on the frame thread over 25 ms that are not inside another:

| stage | thread on the CPU | stages | their ms | thread CPU ms |
|---|---|---:|---:|---:|
| placements commit | under half the stage | 14 | 653.7 | 93.8 |
| world update | under half | 9 | 332.4 | 0.0 |
| table set | under half | 1 | 40.8 | 0.0 |
| world update | half or more | 1 | 43.0 | 46.9 |
| placements commit | half or more | 1 | 25.2 | 15.6 |

**Two of 26 were the world's work; 24 were the frame thread off the processor** on a machine whose other processes held 79 to 100 % of it — the four "updates their parts do not name" and the 73 ms tail commit (frame 197) among them, each with no CPU time to its name. What is the world's: the hand-over near frame 1,579, whose slot write held 46.9 ms of CPU for 3,904 slots (where the first fill wrote 297,268 in 114 ms) and whose relayout's chain resize was 47.7 ms in the driver with none, and one update of 43 ms.

**The walk again with the new record** (T1, this row's tip before the mend below; 04:00, session locked, the machine shared with five rows' builds and GPU cases and this row's own driverless test run beside the load and the walk; 2,370 frames):

| | median | p95 | p99 | worst | over 33 | over 50 | over 100 |
|---|---:|---:|---:|---:|---:|---:|---:|
| T1, frame ms (own interval) | 18.15 | 22.24 | 28.39 | 94.8 | 16 | 6 | 0 |

Every frame over 33 ms by its own record:

| cause (the record's own words) | frames | wall ms | the longest (frame: wall, the part) |
|---|---:|---:|---|
| slot wait, late: the GPU had finished the awaited frame and the thread woke 19–76 ms after | 5 | 273.0 | 2163: 94.8 (late 76.0), 2176: 54.0 (34.0), 2178: 43.5 (24.3), 1868: 42.5 (19.4) |
| slot wait, the GPU inside the awaited frame (spans 18–22 ms, nothing untimed) | 4 | 151.7 | 2197: 47.3, 2124: 35.8, 1872: 35.0, 936: 33.6 |
| the frame's beginning outside its wait | 3 | 157.7 | 2122: 56.3, 2174: 51.6, 2120: 49.8 |
| a placements commit, the thread off the CPU (trace: 51.8 ms, CPU 0) | 1 | 65.2 | 2161 |
| the graph's compile, with 13.5 ms of the GPU elsewhere before the frame | 1 | 62.6 | 1267 |
| a pass's recording | 1 | 49.0 | 932 |
| **the world's own: the hand-over's slot write** (3,744 slots, write 30.8 ms, relayout 5.0; trace CPU 46.9 of 37.4 ms) | 1 | 39.8 | 1577 |

- **The slot waits are three things and the record now says which**: five late wakes (the GPU's frame was done; the thread was not running), four the GPU's own frame (whose span carries 0.2 ms no zone names — the zones cover the frame), and no frame whose GPU was held elsewhere for long; `elsewhere_ms` reached 13.5–14.6 ms on two frames only.
- **The world's update is named**: over the 25 updates longer than 5 ms, 91.6 % of their time is in the named parts at the top (the ring, the drawn ground, the placements, the re-standing), and inside the placements their named parts cover the rest to within a millisecond.
- **The world's own stalls are one frame here**, the growth's slot write at frame 1577; the rest of R195's list did not recur or was the thread off the CPU.

**What was removed: the CPU table's growth on the growth's frame** ([renderer](../subsystems/renderer.md#the-tails-cpu-table-grows-into-room-made-ahead)). Frame 1577 is where the tail passes 468,000 slots, half again of the first fill's 312,000: the GPU's tables grow (T152's swapped relayout, 5 ms here) and so does the CPU's own instance table, by the container's half again — 67 MB made, 45 MB copied, every page faulted in, on the frame thread. `renderer::TableRoom` makes that block ahead, faulted in 1 MiB a frame from three quarters full, and the growth copies into it; the old block is freed a frame later. Nothing drawn can differ: the table's bytes and order are the container's own growth's. The relayout's chain resize, which R195's busier B-4k flight spent 47.7 ms of in the driver, is still made on the frame (T152's row).

**The pair** (the walk, 4K, the owner's settings, `--wait-quiet 150` (which never found the machine quiet), the session locked throughout, alternating base and tip with ten minutes clear between; the base is this row's base, 29dfa76ea, built once and copied out; P2's tip is the room with the old block freed on the growth's frame, P4's the final tip):

| flight | binary | others' CPU (start → end) | median | p95 | p99 | worst | > 33 | > 50 | > 100 | GPU median / p99 | growth frame: update / slot write ms |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---|---|
| P1 | base | 30 → 26 % | 18.36 | 21.95 | 22.87 | 32.3 | 0 | 0 | 0 | 18.13 / 22.35 | 30.8 / 24.5 |
| P2 | tip (room) | 50 → 44 % | 18.19 | 21.93 | 22.86 | 29.4 | 0 | 0 | 0 | 17.96 / 22.31 | 27.4 / 20.3 |
| P3 | base | 100 → 96 % | 18.37 | 23.92 | 48.09 | 93.8 | 58 | 21 | 0 | 17.94 / 22.40 | 35.7 / 29.7 |
| P4 | tip (final) | 32 → 38 % | 18.12 | 21.87 | 22.80 | 33.7 | 1 | 0 | 0 | 17.81 / 22.23 | 20.2 / 15.2 |

(05:36 to 06:03 for P3 and P4, P1 and P2 from 04:36; each flight 2,370 frames after the first 30.)

- **The world's own frame: about a third off.** The growth frame's world update is 30.8 and 35.7 ms on the base, 27.4 with the room whose old block was freed on the frame, and 20.2 with the final tip; its slot write 24.5 and 29.7 against 20.3 and 15.2. The final tip's trace says where the rest is: the room was taken twice (at 312,464 slots, the first fill's growth, and at 471,128), the table's move into it is 11.4–11.7 ms — the copy of 45 MB into resident pages, element by element — and the resize stage 12.2 ms where P2's, which also freed the old block there, was 16.3.
- **The walk's distribution is the machine's, not the change's.** Three of the four flights had no frame over 50 ms and their medians, p95s and p99s agree within 0.2 ms; P3, the base flown while the other processes held 96–100 % of the CPU, had 58 frames over 33 ms and 21 over 50, and its record (the base's, version 23) cannot say which were late wakes. The GPU's own medians agree to 0.3 ms on all four. So the pair shows what the change removes — a third of the one frame of the world's own — and nothing it adds; it cannot show a change in the count of long frames, which on this machine tonight is set by who else is running.
- **What is left of the world's own**, largest first: the table's copy into the room (11.5 ms at 470,000 slots; done ahead it would need the table's changes since the copy tracked, or the table kept as pages a growth can extend in place), the relayout's chain resize (3.5 ms here, 47.7 ms in R195's B-4k with no thread CPU: the driver's allocation, T152's half still on the frame).

## What this does not cover

- **A quiet machine.** Every flight ran beside other rows' builds and GPU cases; the CPU numbers and the frame thread's waits are upper bounds, and the slot waits past the GPU's frame are not split.
- **The owner's own walk** (a player, the game module's animals, the window and HDR10): the replay is a camera path; R135 found the live session heavier at the same poses.
- **Per-view milliseconds** are not in the frame record (R135's T73); the side views' share is read by difference.
