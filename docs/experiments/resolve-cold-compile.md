# The resolve's cold compile (R205, 2026-11-23)

**What this settles.** Why the material resolve's traced water modules took minutes of a driver's time on a cold cache — batch 72's W156 measured **341 s of a 350 s gap** in a cold load of the forest staging spent compiling one pipeline, `visibility_resolve_rt_water_terrain_stochastic`'s `vs_fullscreen`/`fs_resolve` (5.1 MB of SPIR-V then), with commit **+6 GiB** while it ran — what that time is made of, what two mends of it bought (a quarter of the time, 4 GB less of the driver's memory) and why both were refused (more than 3 ms of the forest's resolve at 3840 × 2160), and the next leg. It is paid once per driver or binary change: by every player at their first launch, and by the owner after every install. The method, so it is not rediscovered: [gfx](../subsystems/gfx.md#pipelines-created-in-parallel) says what "cold" means; this page adds a second way to be cold that needs no environment at all.

## How a cold compile was reproduced small

- **A byte-different module is a cold one.** The driver's own disk cache is keyed by the SPIR-V, so one `OpSourceExtension` naming a nonce, put in the module's debug section, misses it and changes nothing the driver compiles; the engine's pipeline cache misses too (its key covers the bytes). Each trial was a fresh nonce. Nothing of the driver's cache was touched.
- **The module the scene asks for, from a file.** `engine-view --shaders <manifest.json>` loads every module the manifest names from its `spirv` path in place of the embedded copy (a `source` that does not exist is never recompiled), so a small scene's load compiles whichever SPIR-V the manifest gives the name its resolve asks for. `content/test-scenes/beach` (an open sea over dunes; its resolve is `visibility_resolve_rt_water` under `--raster rt --shadows rt`) at 1280 × 720 for two frames, with the copied-out `engine-view` of main 5801d60f and a scratch `--ddc`: a load of about 35 s, and the summary's `load.pipelines.slowest` names the resolve's own milliseconds.
- **The machine.** The RTX 5090 (driver 621.24) and the i9-10980XE, shared all night with other agents' builds and GPU work: free commit 42 to 70 GB at a trial's start, processors 50 to 80 % busy. Every time below is the pipeline's own (`pipeline created`), a wall clock of the driver's call, so it moves with the machine's load; a pair is two trials back to back in one lease.

The baseline, reproduced: **the forest's variant cold on the beach, 353.1 s, the process's peak private bytes 7.13 GB** against 1.69 GB for the same load warm — the driver's compile holds about 5.4 GB while it runs, as W156 saw on the forest.

## What the time is made of

**slangc inlines everything.** At `-O2` every resolve module is two functions, `vs_fullscreen` and one `fs_resolve`, so the driver is handed one function of 22,267 instructions (the plain module) to 303,658 (the forest's). The instructions of `fs_resolve` in each of the sixteen modules at main cce82e0c4:

| module | bytes | instructions | | module | bytes | instructions |
|---|---|---|---|---|---|---|
| plain | 423,952 | 22,267 | | water | 1,438,408 | 76,381 |
| stochastic | 448,864 | 23,617 | | water_stochastic | 1,463,272 | 77,731 |
| rt | 471,152 | 25,105 | | water_terrain | 2,316,496 | 126,029 |
| rt_stochastic | 496,032 | 26,455 | | water_terrain_stochastic | 2,592,344 | 141,070 |
| terrain | 613,284 | 32,606 | | rt_water | 3,123,548 | 173,016 |
| rt_terrain | 660,420 | 35,444 | | rt_water_stochastic | 3,296,980 | 182,136 |
| terrain_stochastic | 697,788 | 37,183 | | rt_water_terrain | 4,704,268 | 262,347 |
| rt_terrain_stochastic | 744,924 | 40,021 | | rt_water_terrain_stochastic | 5,470,684 | 303,658 |

The rays add 3,000 instructions to a plain module and the layered ground 10,000, but the water adds 54,000 without rays and 148,000 with them, and the ground adds 89,000 to a traced water module: something in the water path is written out many times.

**Where the copies come from.** A build of the same source with `[noinline]` on the water's functions and on what they call keeps them as functions, so the module says how often each is reached; a compiler that inlines everything writes each one out that many times (instructions are each function's own):

| function | own | copies | what makes the copies |
|---|---|---|---|
| `sample_material_grad` | 902 | 90 | every material read: each hit's, the refraction's, each layer of the ground |
| `cascade_visibility` | 341 | 42 | inside every shadow query |
| `shadow_ray_blocked` | 752 | 31 | 21 shadow queries and 10 hits' sun rays |
| `shadow_visibility` | 154 | 21 | the two tiers' surfaces, the bottom, the refraction, the white water, the ground |
| `scene_ambient` | 2,668 | 11 | the ground and the ten reflection hits |
| `water_ground_wetness_of` | 6,505 | 11 | the ground and every reflection's hit (the shore's wet band where the reflection meets a bank) |
| `shade_hit` | 2,518 | 10 | `water_reflection`, ten copies |
| `terrain_hit_material` | 1,539 | 10 | inside every `shade_hit` |
| `water_reflection` | 571 | 10 | both tiers × four places that shade water, and the wet ground's sheet and film |
| `sample_terrain_material` | 12,416 | 5 | the ground, and the bottom under the water in each of four volume tiers |
| `water_pixel_volume`, `water_pixel_low` | 1,476, 607 | 4 each | `water_pixel` once, `open_water_pixel` three times |
| `water_refraction` | 1,653 | 4 | each volume tier |
| `open_water_pixel` | 2,012 | 3 | the sea in front of a surface, and `empty_pixel`, which two returns of `fs_resolve` call |

Fully inlined, that is 403,000 instructions before slangc's own folding (303,658 after). The root is two multiplications: **four places shade water** (a body's pixel, and the open sea's plane asked from three places) and **each place carries both tiers, and each tier asks its own reflection**, so ten reflection rays are written out, each with the shading of what it meets — the shore's wet band, the hit's ground material, the cache's ambient, a sun ray.

**How the driver's time moves** (each a trial as above; "beside a build" is an upper bound):

| module | instructions | cold | process peak | note |
|---|---|---|---|---|
| rt (base) | 25,105 | 11.2 s | 1.76 GB | beside a build |
| water_terrain (base) | 126,029 | 131.3 s | 3.62 GB | beside a build |
| rt_water_terrain_stochastic (base) | 303,658 | 353.1 s | 7.13 GB | |
| the same, every function above `[noinline]` | 98,408 own, 403,000 inlined | 166.9 s | 5.33 GB | beside a build |
| the same, first step of the mend (four places to one) | 138,686 | 85.9 s | 3.07 GB | beside a build |

So the driver honours a call partly — all those functions kept as calls halved the time and took 1.8 GB off the peak — but the copies are the cost, and fewer copies buy more than calls do. Time is not instruction count alone: the base `water_terrain` (126k, four copies of the tier with the ground's layers under the water in each) took longer than the mend's first step at 139k.

## Two mends, both refused for the frame

Both left the picture alone and cut the cold compile; both cost the forest's resolve more than 3 ms at 3840 × 2160, so neither is in the tree (branch `b73-R205` holds the first as commit c901e6620 and its revert; the second is described here). The frame was measured on the forest staging at the owner's settings (3840 × 2160, traced primaries and shadows, the bounce with its guide and memory, accumulation, micromaps; W149's standing place, 1,500 frames; main's copied-out binary then the branch's in one lease, the session locked; a ddc root of the row's own whose pipeline cache was a copy of the staging's).

**First: every water shaded at one place.** A body's pixel and the open sea's plane said what they would shade in the invocation's globals (a `WaterTier` of about 80 words) and returned; the entry point, after the rest of the pixel's resolve, shaded it once — the surface's light asked once for either tier, the tier, the white water, the darkening, the air and the encode — and the wet ground's sheet and film shared one reflection ray. The forest's module went from 303,658 instructions to 125,190 (2.3 MB):

- **cold compile**: 268.2 s and a process peak of 7.26 GB before, **67.8 s and 2.95 GB** after, back to back in one lease; `ground-by-place`'s real load, whose `rt_water_terrain` no cache held, 261.1 s before and 65.3 s after; on the forest itself the resolve's pipeline was **71.2 s cold** against W156's 341 s;
- **picture**: colour and ids at the markers of `beach`, `oasis-pool` and `ground-by-place`, 1280 × 720, `--raster rt --shadows rt`: every file byte-equal; the gfx `material resolve:*` cases (14) and the water suite passed on the device;
- **frame**: the forest's resolve **8.904 ms before, 12.545 ms after** (p95 8.988 and 12.735), every other pass the same to 0.003 ms; on the small scenes at 720p, +0.015 ms on the two that are mostly water.

| module (first mend) | before | after | cold after (one lease of eight, others' processors at about 70 %) |
|---|---|---|---|
| water | 76,381 | 61,681 | 19.3 s |
| water_stochastic | 77,731 | 63,031 | 21.6 s |
| water_terrain | 126,029 | 81,947 | 53.3 s (131.3 s before) |
| water_terrain_stochastic | 141,070 | 89,140 | 55.0 s |
| rt_water | 173,016 | 84,418 | 30.8 s |
| rt_water_stochastic | 182,136 | 87,433 | 29.3 s |
| rt_water_terrain | 262,347 | 112,632 | 98.0 s |
| rt_water_terrain_stochastic | 303,658 | 125,190 | 109.6 s (353.1 s before) |

**Second: the same questions asked once at each place, no globals.** Suspecting the deferral (every return of the resolve made a branch to one tail, the water's inputs live across the code between), the second form kept the tiers where they were called and kept only the local pieces: the surface's light asked once for either tier at each place (`water_surface_light`), an empty tile and an empty pixel as one return (so the open sea is asked from two places, not three), the wet ground's one ray. The forest's module: 191,898 instructions (3.5 MB), its pipeline **124.4 s cold on the forest**; and the forest's resolve **8.901 ms before, 12.267 ms after** (p95 8.995 and 12.451), the same hour, the same lease.

**What the two share, and the likely cause** (reasoned, not measured apart): both ask the surface's light — the sun, its shadow query, the ambient, the reflection ray and the shading of what it meets — *before* the volume tier reads the ground under the water (`sample_terrain_material`, the heaviest user of registers in the module), so the light's ten results are live across it; main asks them after. A fragment shader's registers are allocated for its largest path whether a pixel takes it or not ([renderer](../subsystems/renderer.md), "The resolve draws water with modules of its own", measured that once already: a branch compiled in took 0.48 ms with no water on screen), and the forest's pixels are almost all ground and leaves, so a few more registers on the water's path cost every pixel its occupancy. The small scenes' 0.015 ms at 720p was the same thing seen small; **a resolve change is judged on the forest at the owner's settings, not on a small scene**.

## The next leg

The cause of the cold compile is found and measured; a mend that keeps the frame is a day's design, not a night's:

1. **The first mend's sharing with main's order.** Ask the volume tier's bottom first (the path through the depth map and the ground's layered material, as main does), then the surface's light once, then the rest of either tier: `water_pixel_volume` split into its bottom and its light-and-volume halves, the low tier taking the light alone, so nothing the light returns is live across the ground's material. Fly it on the forest before anything else; if it holds, add the empty return and the wet ground's one ray one at a time, each flown.
2. **Measure registers rather than guess them.** The driver's per-pipeline statistics (`VK_KHR_pipeline_executable_properties`: register count and spills per stage) would say in seconds what a forest pair says in twenty minutes; the RHI does not ask for them yet. With them, each step of 1 is judged on the card's own numbers before it is flown.
3. **The water apart from the main path.** Under `renderer.resolve.classes` (off by default) the resolve draws a pixel class at a time; a water class drawn by a module that holds only the water's pixel and its tier, and a ground class by one that holds only the ground, would each be about half today's module, compiled side by side, and the ground's module would not carry the water's registers at all — which is also the frame's gain. It costs the classify pass and a draw per class ([renderer](../subsystems/renderer.md#the-resolve-by-pixel-class) measured it).
4. **What bounds a single module.** Even with every copy gone, one module holds the main path (the plain traced module with the ground, 40,000 instructions, about 15 s alone), the water's own pixel (`water_pixel` with the swash, the shore's foam and the channels, 25,500 inlined) and the tier (45,000: the reflection and its hit's shading 18,500, the ground under the water 16,000, the refraction): 68 to 110 s on this machine for the four modules with the layered ground. The brief's 20 s per pipeline needs 3 (or the reflection's hit shaded in a pass of its own), not fewer copies alone.

## W161: the water's ambient

W161's second rule (every ambient term of the water reads the world-space cache at the surface point, [water](../subsystems/water.md#translucent-by-volume)) added one call, `water_scene_ambient` round `scene_ambient`, inside each tier, in the white water and in a lifted body's sheet. The copies multiplied as above: **ten more copies of `scene_ambient`** (2,668 instructions of its own: the walk up the cache's ladder, four levels, each a probe of the cells' keys and a face chosen by the normal) in every water module, so `visibility_resolve_water` and `_water_stochastic` went over the line of 1,536 MiB that makes a shader heavy and `build.shader_pool` failed ([heavy shaders share a pool](../ci/what-to-run.md#heavy-shaders-share-a-pool)). Counted as above: a build with `[noinline]` on `scene_ambient` alone, calls counted. `water`: 1 copy without W161, 11 with it, 7 asked once; `rt_water`: 11, 21, 17.

**Asked once a place, and refused for the frame.** At each place that shades water (`water_pixel`, and `open_water_pixel`, which is written out three times) both tiers ask the same thing — the hemisphere at the surface's interpolated normal through the cache at its point, from the same surface — so a form was tried in which `water_surface_ambient` asked it there, before the tier is chosen, and handed it to both: four copies fewer in every module, the white water (its own plane) and the sheet (the level surface, at a ground pixel) keeping their own. Nothing was computed differently, so the picture was the same bytes: the beach (`--raster rt --shadows rt`, `renderer.gi.mode=sky`, 640 × 360, 48 frames) drawn by one `engine-view` with the water modules as W161 has them and asked once, through `--shaders`, wrote the same PNG, and without W161 another. **What it kept live**: one colour (three floats) from before the tier to where the tier used it, which W161 asks after the tier's other work: across the volume tier's march through the depth map, its ground material under the water (`sample_terrain_material` in the four modules with the layered ground) and its two sun shadow queries, and across the low tier's one shadow query. That is the kind of value the two mends above paid for at 4K, and the forest said it costs here too: its resolve **9.258 ms asked once against 9.180 ms as W161 has it**, 0.078 ms over a floor of 0.011 ms between two flights of one form (the table below). So the tree holds W161 as written: `water` and `water_stochastic` are heavy, at the 1,854 and 1,886 MiB `build.shader_pool` printed for them, in `cmake/shader-costs.txt`.

`fs_resolve`'s instructions (counted as above; the "without" column is the same bytes as the table above), slangc's peak and its seconds, each a real compile of `msvc-debug`'s arguments with the shader cache off, the eight one after another, other agents building beside them (the seconds an upper bound; the peaks do not move with load):

| module | without W161 | W161 | asked once | peak MiB (without, W161, once) | seconds (without, W161, once) |
|---|---|---|---|---|---|
| water | 76,381 | 99,060 | 89,788 | 1,285, **1,854**, 1,308 | 10.0, 15.8, 10.7 |
| water_stochastic | 77,731 | 100,410 | 91,138 | 1,309, **1,886**, 1,310 | 9.1, 16.8, 11.1 |
| water_terrain | 126,029 | 148,708 | 139,436 | 3,081, 3,017, 3,081 | 27.3, 30.4, 38.5 |
| water_terrain_stochastic | 141,070 | 163,749 | 154,477 | 4,267, 6,447, 4,299 | 31.4, 47.2, 73.0 |
| rt_water | 173,016 | 195,663 | 186,359 | 3,174, 4,842, 4,938 | 30.3, 42.3, 51.3 |
| rt_water_stochastic | 182,136 | 204,783 | 195,479 | 5,355, 5,261, 5,356 | 38.0, 47.4, 55.3 |
| rt_water_terrain | 262,347 | 284,994 | 275,690 | 11,738, 11,686, 11,866 | 83.2, 92.9, 82.1 |
| rt_water_terrain_stochastic | 303,658 | 326,305 | 317,001 | 16,515, 16,483, 16,611 | 111.8, 136.1, 131.5 |

So W161 added 22,650 to 22,680 instructions to every module and asking once took back 9,270 to 9,300; asked once, the two light modules were under the line again at their old peaks, and the six heavy ones stayed heavy. slangc's peak follows the copies only loosely: `rt_water` kept W161's peak with four copies fewer, and the three largest moved by under 2 % either way. `cmake/shader-costs.txt` takes the W161 column for the two modules it made heavy; its rows for the other six are still their first measurement, which the modules had outgrown before W161 (`cmake/shader-costs.txt` says why they were left).

**On the forest** (R205's rule: a resolve change is judged there). The forest staging at the owner's settings — 3840 × 2160 offscreen, `--raster rt --shadows rt`, the world's tiles with six far levels built behind the frames, `--time-rate 0 --world-rate 1 --no-lights --time-of-day 15 --lod 1`, flora motion at wind 0.7, the bounce with `renderer.gi.guide` and `renderer.gi.memory` (both off by default), `renderer.reconstruction.accumulation=on` (off by default), micromaps (on by default), `--rt-budget-mib 0` — standing at the staging's own camera (its lake-range path held at its first key: at (80, 14, 130) looking towards (−180, 580, 2850)) for 240 warm-up frames and 1,500 recorded; the window the last 1,000. The binary is the installed release binaries of main (W161 changes no C++), each form's modules named in a `--shaders` manifest; that the manifest is read was checked first on the beach, whose picture differs without W161 and is the same bytes with it and asked once. Each form's forest module (`visibility_resolve_rt_water_terrain_stochastic`, the one the load asks for) was a nonce copy, so its first load compiled cold and the later ones were warm in the driver's own cache. 2026-11-23, 13:55 to 14:29, the RTX 5090 (driver 621.24) and the i9-10980XE, the session unlocked in the daytime, other agents free to build: at each lease's start free commit 40 to 53 GB, the processors 2 to 30 % busy, nothing else on the card. Three cold loads, one a lease; then the warm four in one lease, `before` first and last:

| form | cold compile | resolve median (warm) | p95 | p99 |
|---|---|---|---|---|
| without W161 | 265.1 s | 9.034 and 9.024 ms | 9.144 and 10.161 | 9.191 and 10.537 |
| W161 | 291.3 s | 9.180 ms | 9.280 | 9.971 |
| W161 asked once | 257.9 s | 9.258 ms | 9.384 | 10.227 |

The two flights of one form differ by 0.011 ms at the median, every other pass by 0.023 ms at most (`gi_fold`), and the cold loads' own frames agree (9.038, 9.191, 9.261 ms). So **W161 costs the forest's resolve 0.151 ms** where almost no water is on screen — the registers the water's path holds, paid by every pixel — and **asking once a further 0.078 ms**, seven times the floor: the tree keeps W161 as written. The cold compiles are each one load and move with the machine (the same module took 269.1 s on the beach an hour before); asking once took about a tenth off W161's, which is not worth the frame. A load's peak commit was 19.7 to 20.4 GB cold and 18.2 GB warm.

## The second leg: the compile starts with the load, and what the water costs (2026-11-24)

**What it settles.** The resolve's pipeline now starts compiling when the scene file has been read, on a worker, and the renderer's creation takes it ([renderer](../subsystems/renderer.md), "The resolve compiles from the scene file"); the driver's own register counts are a switch away ([gfx](../subsystems/gfx.md#what-the-driver-says-a-pipeline-costs)); and what the water costs the forest's frame is parted by class: the frame drawn by the module without water takes **4.58 ms of resolve against 9.00**, but that is the sea's quarter of the frame drawn as plastic — the five classes that cannot be water cost **3.11 ms through the water's module and 3.50 through the one without** ([below](#what-the-water-costs-the-forests-frame)). So the water does not make every pixel dearer on this frame; its own pixels are the cost, the register counts say the same, and a water class with a module of its own is worth building for the cold compile and only possibly for the frame ([the design](#the-third-leg-the-water-as-a-pixel-class-with-a-module-of-its-own-the-design)).

The machine for everything here: the RTX 5090 (driver 621.24) and the i9-10980XE, 2026-11-23 22:05 to 2026-11-24 01:00, shared with two other coordinators' agents building and flying (other processes' processors 5 to 100 % at a run's start, free commit 36 to 65 GB at a start and as low as 0.6 GB during one run); each run's state is in its row. The forest staging at the owner's settings (3840 × 2160 offscreen, `--raster rt --shadows rt`, the bounce with `renderer.gi.guide` and `renderer.gi.memory`, `renderer.reconstruction.accumulation=on`, micromaps), W149's standing path, `--time-rate 0 --world-rate 1 --no-lights --time-of-day 15 --lod 1`, flora motion at wind 0.7, `--terrain-tiles --terrain-far 6 --terrain-async`, `--rt-budget-mib 0`, `--audio off`; the "before" binary is this leg's base (cc95d783a, main with W161) built `msvc-release` and copied out, the tip this branch's. A cold module is the page's nonce method: the forest's `visibility_resolve_rt_water_terrain_stochastic` SPIR-V copied with one `OpSourceExtension`, a fresh nonce per cold run, named in a `--shaders` manifest, so both caches miss and nothing compiled differs. **The staging's captures are not a byte check**: two cold loads of the base binary drew different last frames, so the picture is held by the renderer's case instead (`early_pipelines_tests.cpp`: the same colour and ids with the early pipeline and without).

### The compile starts when the scene file is read

The base asks for the resolve inside `create_pipelines`' batch, after the upload; the tip starts it on a worker the moment `read_scene_file` returns and takes it after the batch. Two cold pairs, each run one declared 20-minute lease, the base first; seconds from launch (the startup trace's stages; the commit is the sampler's private bytes every 2 s, GiB):

| run | machine at its start | scene file read | `gpu_scene` | the resolve's compile | `renderer` ends | the commit's maximum, and when | after it |
|---|---|---|---|---|---|---|---|
| base, first pair (22:05) | others' processors 100 %, free commit 39.8 GB, falling to **0.6 GB** during the run | 275.1 | 404.7 | 406.0 to 860.2: **454.1 s** | 862.4 | 20.54 at 844 s | 17.4 at 500 s after the fall |
| tip, first pair (22:24) | 16 %, 36.3 GB, falling to 5.6 GB | 64.1 | 140.5 | **64.1** to 495.9 on the worker: **431.8 s** | 497.8 | 19.73 at 441 s | 15.5 at 500 s |
| base, second pair (22:57) | 37 %, 65.3 GB | 41.1 | 81.5 | 82.6 to 379.6: **297.0 s** | 381.1 | **20.52 at 355 s** | 16.3 at 380 s |
| tip, second pair (23:35) | 11 %, 54.3 GB; **this row's own no-driver test run (three jobs) beside it** | 55.1 | 110.0 | **55.1** to 363.5 on the worker: **308.4 s** | **365.2** | **19.79 at 349 s** | 15.7 at 370 s |

So on the second pair the renderer had its resolve **16 s earlier from launch** although the tip's own load reached `gpu_scene` 28.5 s later (its scene file and load shared the processors with the compile and with the test run), and measured from the end of the upload the renderer waited **255.2 s instead of 299.6**; the commit's maximum was **0.73 GiB lower** and fell about 10 s earlier. The first pair says the same more loudly and less fairly (the base ran with every processor taken and the scene file took 275 s): the compile had finished 366 s before the base's started. **What it does not do**: the compile is about 300 s and the forest's load to `gpu_scene` 80 to 140 s on this machine, so the compile still spans the upload and the driver's +6 GiB still lands on the uploaded scene's ~14 GiB; the peak moves by what was not yet uploaded when the compile peaked, not under the standing figure. W156's first proposal — compile before the load and wait for it — would put it there, at the price of the load's start; a player's first launch, whose load also builds content, gains more from the overlap than this warm-content staging does. The early compile itself ran 4 % longer than the base's (308.4 against 297.0 s) for sharing the processors with the load. Warm, both binaries in one lease (23:10 to 23:18, 1,500 frames recorded after 240): the early pipeline was made on the worker in 62.6 ms; the loads took 228 and 204 s whole (`gpu_scene` 102.4 and 81.4 s: the machine's), the commit's maximum 18,413 and 18,424 MiB, and the frame's resolve 9.003 and 9.019 ms median (every other pass within 0.012 ms) — a warm load costs what it cost.

### What the water costs the forest's frame

The brief's prize for a water class of its own (item 3 of [the next leg](#the-next-leg)): the forest's module as it is, and the same binary with the module without water, `visibility_resolve_rt_terrain_stochastic`, named in a `--shaders` manifest under the forest's name, so the frame is the same frame drawn by the module that carries no water (the water's pixels draw as the untextured dielectric the plain module draws a water material as, and the wet ground dry). The standing place, 240 warm-up frames and 1,500 recorded, the last 1,000; each a load of its own in one lease (lease A 23:10 to 23:21, others' processors 5 to 52 % at the starts, free commit 52 to 60 GB; lease B 23:42 to 23:53, 4 to 29 %, 43 to 54 GB). The resolve's milliseconds:

| form | binary | median | p95 | p99 | `total` median |
|---|---|---|---|---|---|
| the forest's module as it is | base | 9.003 | 9.069 | 9.109 | 16.487 |
| the same | tip | 9.019 / 9.020 | 9.086 / 9.088 | 9.122 / 9.114 | 16.496 / 16.506 |
| **the module without water in its place** | base / tip | **4.579 / 4.585** | 4.670 / 4.675 | 4.734 / 4.741 | **12.045 / 12.046** |
| `renderer.resolve.classes=modules` (classify 0.257 / 0.271 / 0.280 besides) | tip | 8.532 | 8.605 | 8.676 | 16.278 |

Every other pass within 0.013 ms across the forms. The frame's pixels by class (the classify pass's census, `renderer.resolve.class_census`, at the standing place): sky 3,683,560 (44.4 %), masked 1,460,748 (17.6 %), ground 892,549 (10.8 %), rigid 187,572, deformed 87,628, **water 1,909,923 (23.0 %)** and wet 72,420. The standing place looks out over the staging's `eastern-sea`, an open body whose plane runs to the horizon, so a quarter of the frame is water — and because the sea is open, `modules` drew no class by the plain module (the census's `plain=0`: an open body's plane may be any pixel's, so the split falls back to the scene's module for every class, [renderer](../subsystems/renderer.md#the-resolve-by-pixel-class)); what it bought, 0.21 ms net of its classify pass, is the class draws' own. So the 4.42 ms the module without water saves is the water's pixels drawn as an untextured dielectric **and** whatever the water module costs every other pixel; the classes part them (lease C, 00:13 to 00:19, others' processors 10 to 24 %, free commit 66 GB; `renderer.resolve.classes=draws` with `renderer.resolve.class_mask=31`, so the water and wet classes keep the clear):

| the five classes that cannot be water (sky, rigid, ground, masked, deformed) | binary | median | p95 | p99 | classify |
|---|---|---|---|---|---|
| drawn by the forest's module | base | **3.112** | 3.138 | 3.152 | 0.257 |
| drawn by the module without water | base | **3.497** | 3.552 | 3.583 | 0.256 |

**The water does not make the other pixels dearer; its own pixels are the cost.** The five classes that cannot be water cost 3.11 ms through the water module and 0.39 ms *more* through the module without water; with all seven drawn (`modules` above, every class by the water module) the resolve is 8.53 ms, so the water and wet classes — 1,982,343 pixels, 23.9 % of the frame — take about 5.42 ms, 2.7 ns a pixel against 0.49 ns for the other 6.3 million (the sky among them). The one draw's 9.00 ms is those plus its quads that mix classes (9.00 against the split's 8.53 here). This rereads the first leg too: its two mends were flown at this place (W149's standing path, the same file), where a quarter of the frame is the sea, so the 3.4 to 3.6 ms they cost may be the water's own pixels made dearer rather than every pixel's; W161's 0.151 ms was flown here as well. A change to the water is judged by the classes: the water class's time and the five others' apart, not the resolve's whole.

### What the driver says the modules are

`ENGINE_PIPELINE_STATISTICS=1` on the tip in lease B ([gfx](../subsystems/gfx.md#what-the-driver-says-a-pipeline-costs)); the fragment stage of each pipeline as the driver compiled it for the forest (the foliage specialization on):

| module | `fs_resolve` instructions (above) | Register Count | Binary Size | Local Memory (spilled) a thread |
|---|---|---|---|---|
| `visibility_resolve_rt_water_terrain_stochastic` (the forest's) | 326,305 | **168** | **5,640,320** | **736 bytes** |
| `visibility_resolve_rt_terrain_stochastic` (no water) | 40,021 at cce82e0c4 | **255** | 741,120 | 64 bytes |
| `resolve_classify_water` `fs_classify` | — | 39 | 10,240 | 0 |
| the beach's `visibility_resolve_rt_water` (640 × 360, no foliage) | 195,663 | 168 | 2,737,152 | 512 bytes |

**Neither number is the cost the pages reasoned with.** The module without water holds *more* registers a thread — 255, the most a thread can have, so fewer warps on a multiprocessor — and the water's module is held at 168 and spills 736 bytes a thread to local memory from a binary 7.6 times the size; yet the five classes that cannot be water are 0.39 ms *cheaper* through the water's module, which is what the occupancy at 168 registers against 255 would predict, and the spills and the size did not show on them. So "a fragment shader's registers are allocated for its largest path" holds for the allocation (the water module's count is its largest path's, capped), but on this frame the price of the water module is the water's own pixels, and a register count is not a cost until a class's time says so. The statistics give the counts in seconds for every variant; the classes give the times ([the design below](#the-third-leg-the-water-as-a-pixel-class-with-a-module-of-its-own-the-design)).

### The third leg: the water as a pixel class with a module of its own (the design)

**The modules it makes**, for each combination of the rays, the layered ground and the stochastic materials (the eight water modules today become three each):

| module | what its `fs_resolve` holds | predicted instructions | predicted cold, alone | draws |
|---|---|---|---|---|
| ground (today's module of the combination without water; exists) | the main path | 40,021 (the forest's) | about 15 s (the first leg's plain traced module with the ground) | sky, rigid, ground, masked, deformed |
| **water** (new, `RESOLVE_WATER_CLASS`) | the pixel's decode and encode, `water_pixel` and its tier, asked from **one** place: the reflection's ray and its hit's shading written out twice (one copy per tier) instead of ten | 110,000 to 125,000 (the first mend, one place for the whole module, was 125,190 with the main path in it) | 80 to 110 s (that mend's 109.6 s) | water, and the open sea's plane |
| **wet** (new, `RESOLVE_WET_CLASS`) | the main path plus the shore's and banks' wetness (`water_ground_wetness_of`, 6,505) and the wet sheet's one reflection with its hit (about 20,000) | about 70,000 | 30 to 50 s | wet |

Compiled side by side in `create_pipelines`' batch (eight threads), the cold start's long pole becomes the water module's 80 to 110 s instead of 297 to 454 s, and the early start above takes that off the load as well. A water tier chosen by a specialization constant (the scene's `water_quality` is one setting) would leave one tier in the water module — about 45,000 instructions fewer — and is the next step if the water module's compile is still the long pole.

**The frame, and why the table refuses that half of the prize.** The table above says the pixels that cannot be water gain nothing from leaving the water's module — they are 0.39 ms dearer in the module without water at its 255 registers — so a water class with a module of its own does **not** return the 4.4 ms the module without water drew the frame in: that was the sea drawn as plastic. What it can return on the frame is the water's own pixels drawn by a smaller module (one place, two reflection copies, less spilled), which no measurement here gives: the next leg builds the water module and flies `classes=draws` with the mask on the water class alone (32) against today's module before anything else, and keeps it only if the water class is no dearer and the other five are not, the forest's resolve within 0.05 ms or better. **The half of the prize that stands is the cold compile**: three modules of 40,000, 70,000 and about 115,000 instructions compiled side by side instead of one of 326,305, and the frame's lever for water is the water's pixels themselves (5.4 ms for a quarter of this frame: the two tiers' reflection and its hit's shading first).

**What selects a pixel's class, and what that costs.** The classify pass as it is (`resolve_classify_water`: 39 registers, a 10 KB binary; **0.257 ms** at 3840 × 2160 on the forest) and the class exit at the top of each module (0.01 to 0.05 ms measured by R151). Two things change. (1) **The open sea becomes a class.** Today an open body sends every class to the scene's module, because any pixel may be its plane — the staging's `eastern-sea` does that, and it is why `modules` returned only its draws' 0.21 ms above. The classify pass asks the plane the water module asks (`open_water_pixel`'s hit: the view ray against the body's level inside its region) and writes the pixel's class as water when the plane is in front of the surface or there is none; the water module **trusts the class** (the depth-equal test is exact, and R151's rule that a class exit asks only integers holds: it never asks the plane again), so the two shaders never disagree about a pixel. (2) **The wet class must stay a claim of "may need the water's wetness"**, which it is today: over-claimed ground pixels (75 to 98 % of the class by R151's counts) cost the wet module's path, which is small, never a wrong picture.

**What the reflection's hit shading does.** It stays inline in the water module: only the water's pixels pay its registers and its size there, which is the point. A reflection pass of its own (the water pixels' rays traced and their hits shaded in a compute pass, the radiance read by the water module) would take `shade_hit` — the hit's ground material, the cache's ambient and its sun ray, 18,500 instructions a copy — out of every fragment shader, and is the step after this one if the water module's compile or its pixels' cost is still too high; it costs a buffer of the water pixels' radiance (at most 8 bytes a pixel, 66 MB at 3840 × 2160) and a dispatch.

**What it does not change.** Every pixel computes what it computes today: the water module and the wet module are the same source's functions compiled into smaller entry points, so the picture is held as `modules` is held today (R151's cases, the water suite's `resolve_class_water_tests.cpp`), including R151's open finding that two modules may round one sum differently (2 pixels of 8.3 million by one code on the mountain).

