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
