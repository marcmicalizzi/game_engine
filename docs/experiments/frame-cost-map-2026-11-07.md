# Where the frame goes on today's world, and the levers ranked (R135, 2026-11-07)

Roadmap R135. The owner, 2026-11-06: "We still need to find some major performance levers; maybe we need to start with better profiling of where the frame time is going." Every frame record already names its milliseconds (`gpu_ms`'s buckets, `cpu`, `sections`; [apps](../subsystems/apps.md), "The frame record's sections: every millisecond named") and the resolve's stages come from `renderer.resolve.stop_after=cycle` ([renderer](../subsystems/renderer.md#where-the-resolves-time-goes)). This page puts them together for the current world — the forest proof of concept's v34 — at the owner's two widths, and ranks the levers by a bound read from the numbers. It **supersedes the stage tables of [forest-resolve-2026-11-01.md](forest-resolve-2026-11-01.md) (R115) as the current attribution**; that page stays the record of how the resolve's split was found and what R120 and R126 bought.

**The answer in four lines.** At 11520×2160 on three monitors the dense forest's GPU frame is 95 ms, and three things are 90% of it: **the sun's shadow ray, 45 ms, of which 37 is the alpha masks it samples** (every caster solid leaves 7.5), mostly in the two side views; **the primary visibility rays, 21–35 ms**, which the mesh rasterizer draws in 9–10 with the same visibility buffer; and **the ray-tracing chain, 11 ms**, 6.3 of it the cluster structures' build. The material, the lighting, the pool, the medium, the cull and the sky together are under 15 ms. The CPU is not the limit at these frame times, but its largest section is the world's tiles built on the frame thread (10 ms a frame while a pose holds) and the passes' recording (4–7 ms).

## 1. The owner's v34 walk, read from its records

**The run** is `D:\workspace\forest_wildlife_poc\reports\lab\20261106-165421-150\` (outside this repository, read as text): the v34 scene (416 animated animals, the camp fire, the female player), 3840×2160 in one view, HDR10, `--raster rt --shadows rt`, `--terrain-tiles --terrain-far 6 --lod 1 --deform-pool-mib 64 --rt-budget-mib 0`, micromaps on, the proof of concept's game module, `--present immediate` with no ceiling; 7,515 frames over 341 s of walking and flying. The extraction streams `frames.jsonl` into one row a frame (`extract.ps1` and `stats.ps1` in the agent's scratch, not committed); the first 30 frames (the load) are left out, so every number below is over **7,486 frames**.

**The frame**: median 46.4 ms, p95 76.2; **88.6% of frames over 16.7 ms, 68.2% over 33.3, 44.7% over 50**. It is GPU-bound throughout: the GPU's own total is 44.1 median (p95 72.1), and the CPU waits for the frame slot 27.4 ms of the median frame.

| GPU bucket, the walk | median ms | share of 44.1 | p95 ms |
|---|---:|---:|---:|
| `resolve` | 18.70 | 42% | 31.41 |
| `trace` (the primary visibility rays) | 13.81 | 31% | 33.04 |
| `rt` (the chain) | 6.60 | 15% | 10.68 |
| — of which `clas` / `blas` / `tlas` | 3.09 / 2.22 / 0.31 | | 6.79 / 3.38 / 0.42 |
| — of which `rt_records` / `rt_emit` / the rest | 0.26 / 0.16 / < 0.1 | | |
| `frame_passes` (the participating medium) | 2.34 | 5% | 3.43 |
| `deform` (the deformed-vertex pool) | 1.30 | 3% | 3.00 |
| `half_sun` | 0.60 | 1% | 1.41 |
| `cull` | 0.42 | 1% | 0.77 |
| `sky`, `tables`, `terrain_upload` | 0.03, 0.02, 0.00 | — | 0.05, 0.42, 0.97 |

`half_sun` ran in the walk although R126's default rule turns it off below 2.5:1 and the 3840 flight below recorded no such pass; what turned it on in the owner's session (the module, the window) is not established here.

**The heaviest ten-second stretches** (non-overlapping windows by their median GPU total; the pose is the window's middle record, yaw and pitch in radians):

| from (s) | frame ms | GPU ms | resolve | CPU ms | record | position | yaw | pitch | visible pairs |
|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| 101.2 | 71.9 | 71.7 | 23.3 | 17.4 | 2232 | (359.0, 5.56, −169.6) | 2.773 | 0.193 | 1,247,833 |
| 151.4 | 71.3 | 71.3 | 21.8 | 18.7 | 3034 | (369.1, 5.25, −163.6) | 2.685 | −0.337 | 1,008,257 |
| 20.9 | 68.1 | 70.1 | 29.1 | 31.6 | 910 | (423.4, −42.77, −300.9) | 0.377 | 0.454 | 1,231,979 |
| 201.6 | 66.9 | 67.4 | 29.6 | 23.3 | 3892 | (323.2, 7.03, −71.5) | 2.760 | 0.180 | 1,042,606 |
| 111.2 | 68.8 | 67.3 | 23.2 | 16.6 | 2374 | (359.0, 5.56, −169.6) | −2.802 | 0.325 | 1,241,113 |

The forest floor between 20 and 210 s is 51–72 ms throughout; the flight out over the valley (210–270 s) 33–49; the far views and the sea 13–32. The two heaviest stretches are the owner standing at one place turning, and their primary trace is 32–34 ms where the forest walk's is 13–17: the visibility rays, not the resolve, made the walk's worst frames.

**The CPU side.** `cpu_ms` median 17.5 ms (p95 39.9). By section (medians): `submit_ms` 7.6, nearly all the renderer recording its passes (`cpu.passes` 6.9, `cpu.record` 0.3); `ticks_ms` 5.0 — the session's ticks and the game's turn: `world.game_ms` 4.0 is the proof of concept's module, and inside the tick the `walk` block's ground queries (4,321 a frame in record 1999) take 3.2 ms; `host_ms` 0.55 median but 22.3 at p95, the world's update (`world.update_ms` p95 21.7, `world.tiles_ms` p95 17.0: tiles built on the frame thread); `slot_wait_ms` 27.4, the GPU. So the engine's own CPU in the median frame is about 9 ms (the recording, the begin and present, the host's turn and the ticks less the module's 4), and its spikes are the world's tiles.

## 2. Three poses flown at both widths

### How it was measured

- **The poses** are three records of the walk, each held 240 path frames in one camera path (`engine.camera-path.v1`, fov 55°, after W82's `make-path.ps1`): **forest**, record 1756, (321.32, 7.01, −295.93), yaw −2.947, pitch 0.066 — a dense interior; **valley**, record 4086, (−60.32, 112.31, 1150.62), yaw −1.506, pitch 0.033 — 112 m up, across the valley to the far forest, 253,000 instances; **heaviest**, record 2232, (359.00, 5.56, −169.65), yaw 2.773, pitch 0.194 — the heaviest stretch's middle.
- **The flags** are the owner's from `run.json` without what an agent may not run (the module, the player, the window, HDR10, `--interactive`): the v34 `scene.json`, the v30 materials, the coordinator's copy of the derived-data cache with `ENGINE_PIPELINE_CACHE=0`, `--shadows rt --no-lights --time-of-day 15 --world-rate 1 --time-rate 0 --lod 1 --deform-pool-mib 64 --rt-budget-mib 0 --terrain-tiles --terrain-far 6`, flora motion at wind 0.7, micromaps at their default (on), offscreen 8-bit, `--repeat 3 --warmup 120 --wait-quiet 120`, `msvc-release` at e5df81cb (main's 8be9c6d4 with only the roadmap row). 11520×2160 is `--views surround3`, the owner's layout; 3840×2160 one view.
- **One flight measures every cell**: `renderer.resolve.stop_after=cycle` with `renderer.shadow.sweep=true` (path frame f draws stage f mod 8 under the sun ray's rule (f / 8) mod 6). The first 48 frames of each hold are left out; a "picture" number is the median of the 12 frames that drew the whole picture under the rule as set, a stage of a rule the median of 12, a bucket the resolve does not change the median of all 576.
- **Four processes under one hold of the GPU lock** (01:18–01:42): rt at 11520, rt at 3840, hw (`--raster hw`) at 11520, and rt at 11520 with `--peripheral-lod 2`. Nothing in the cache was cooked.
- **The machine**: RTX 5090 (32,607 MiB), its memory 18.9–19.8 GB held by others at each start and the GPU idle; other processes on the CPU at 5.2% (start) to 20.5% (end) for the rt flight at 11520, 50–51% for the 3840 flight and 43–44% for the hw flight (another agent's build), 5.6–7.7% for the last. The harness's `quiet` was false for every flight, so the CPU numbers are upper bounds; the GPU numbers are the pass timers'.

### The buckets, ms (share of the GPU total)

| bucket | 11520 forest | 11520 valley | 11520 heaviest | 3840 forest | 3840 valley | 3840 heaviest |
|---|---:|---:|---:|---:|---:|---:|
| **GPU total** | **94.8** | **59.7** | **97.8** | **26.0** | **21.5** | **23.0** |
| `resolve` | 54.2 (57%) | 29.2 (49%) | 43.7 (45%) | 11.8 (46%) | 10.0 (47%) | 6.8 (30%) |
| `trace` | 21.0 (22%) | 14.9 (25%) | 34.8 (36%) | 4.8 (19%) | 3.3 (16%) | 6.9 (30%) |
| `rt` | 11.0 (12%) | 8.2 (14%) | 11.2 (11%) | 6.6 (25%) | 5.4 (25%) | 6.6 (29%) |
| — `clas` / `blas` | 6.3 / 2.5 | 4.0 / 2.4 | 6.6 / 2.5 | 3.6 / 2.3 | 2.2 / 2.2 | 3.5 / 2.3 |
| — `tlas` / `rt_records` / `rt_emit` | 0.33 / 0.76 / 0.45 | 0.48 / 0.76 / 0.47 | 0.33 / 0.76 / 0.44 | 0.32 / 0.26 / 0.16 | 0.45 / 0.25 / 0.17 | 0.32 / 0.26 / 0.16 |
| `deform` | 2.9 | 2.4 | 3.1 | 1.4 | 1.4 | 1.3 |
| `half_sun` | 2.3 | 1.0 | 1.4 | — | — | — |
| `frame_passes` | 2.2 | 2.2 | 2.5 | 0.8 | 0.8 | 0.9 |
| `cull` | 1.1 | 1.2 | 1.1 | 0.4 | 0.5 | 0.4 |
| `sky`, `tables` | 0.04, 0.0 | 0.01, 0.07 | 0.01, 0.0 | 0.01, 0.0 | 0.04, 0.07 | 0.01, 0.0 |
| visible pairs | 1,632,601 | 1,925,427 | 1,336,998 | 933,245 | 1,078,054 | 593,022 |
| chain wanted / built (capacity 338,072) | 366,538 / 338,072 | 188,095 / 188,095 | 373,838 / 338,072 | 175,739 / 175,739 | 104,416 / 104,416 | 169,781 / 169,781 |
| shadow casters | 601,692 | 611,214 | 218,332 | 111,679 | 366,758 | 6,072 |

**The rt flight at 11520 runs past the chain's capacity at two of the three poses**: 1,428 of its 2,160 records wanted more than the 338,072 clusters the device's budget holds and dropped instances (15.2 million instance-drops over the flight; drawn clusters are kept before casters). The hw flight at the same width wanted 282,000–327,000 and never overflowed.

### The resolve's stages at the rule as set (each stage's increment, ms)

| stage | 11520 forest | valley | heaviest | 3840 forest | valley | heaviest |
|---|---:|---:|---:|---:|---:|---:|
| decode | 0.50 | 0.49 | 0.53 | 0.18 | 0.17 | 0.18 |
| reconstruct | 1.09 | 0.92 | 1.25 | 0.41 | 0.36 | 0.42 |
| material | 5.96 | 8.31 | 3.97 | 2.14 | 3.06 | 1.62 |
| lighting | 1.24 | 1.37 | 1.04 | 0.40 | 0.45 | 0.30 |
| **sun_shadow** | **44.59** | **17.23** | **36.19** | **8.55** | **5.79** | **4.10** |
| light_shadows, sky, picture | 0.81 | 0.85 | 0.73 | 0.18 | 0.21 | 0.23 |
| **the picture** | **54.20** | **29.17** | **43.71** | **11.84** | **10.04** | **6.84** |

**The sun ray's rules** (the picture's resolve, ms; R115's sweep, [renderer](../subsystems/renderer.md#what-the-sun-rays-masks-cost-and-what-does-not-buy-it-back)):

| rule | 11520 forest | valley | heaviest | 3840 forest | valley | heaviest |
|---|---:|---:|---:|---:|---:|---:|
| as set (the default) | 54.20 | 29.17 | 43.71 | 11.84 | 10.04 | 6.84 |
| every caster solid | 16.94 | 15.25 | 13.01 | 5.96 | 5.79 | 4.32 |
| proxy at 0.5 / 0.3 / 0.2 | 54.26 / 47.05 / 34.86 | 29.54 / 29.29 / 24.70 | 44.18 / 37.79 / 32.06 | 11.84 / 11.28 / 9.58 | 10.25 / 10.17 / 9.25 | 6.90 / 6.73 / 6.40 |
| **the masks' share of the sun stage** (as set less solid) | **37.1** | **13.9** | **30.6** | **5.9** | **4.3** | **2.5** |
| `half_sun` as set / solid | 2.26 / 0.66 | 0.96 / 0.53 | 1.44 / 0.36 | — | — | — |

### The views

The offscreen flight's records carry no per-view milliseconds (the renderer keeps them in `Stats::views`; only the window's summary prints them, and the frame record has no field), so the views are read by difference. **The centre view of a flat `surround3` is bit for bit the single 3840×2160 view** ([apps](../subsystems/apps.md), `--views`), so the 11520 flight less the 3840 flight is the two side views' cost — an upper bound, since the 11520 frame's chain also holds the sides' casters, which the centre's own rays meet:

| the two side views, ms (11520 less 3840) | forest | valley | heaviest |
|---|---:|---:|---:|
| resolve | 42.4 | 19.1 | 36.9 |
| — the sun stage / its masks | 36.0 / 31.2 | 11.4 / 9.6 | 32.1 / 28.1 |
| — the material | 3.8 | 5.3 | 2.4 |
| trace | 16.1 | 11.5 | 27.9 |
| visible pairs | 699,356 | 847,373 | 743,976 |

**The two side views' resolve is 1.9 to 5.4 times the centre's** where they hold twice its pixels: they look along the forest's floor at a slant, and their sun rays cross more crowns. That is where the frame is.

### `--raster hw` at 11520 (the primary visibility rasterized)

| ms | forest rt → hw | valley rt → hw | heaviest rt → hw |
|---|---:|---:|---:|
| `trace` → `hw` | 21.0 → 8.8 | 14.9 → 6.0 | 34.8 → 9.7 |
| `rt` (the chain) | 11.0 → 9.9 | 8.2 → 6.2 | 11.2 → 9.0 |
| `resolve` | 54.2 → 55.4 | 29.2 → 30.4 | 43.7 → 44.8 |
| **GPU total** | **94.8 → 83.2** | **59.7 → 49.7** | **97.8 → 72.3** |

The resolve moves by about a millisecond the other way (the sun's rays against a chain that holds every instance instead of a dropped few), and the chain is smaller: it wants 119,558–326,836 clusters where the rt frames want 188,095–373,838, and it never overflows.

### `--peripheral-lod 2`

**It changed nothing**: every one of the 2,160 records drew the same visible pairs as the flight without it. That is by design, not a fault: a frame that builds acceleration structures takes the finest view's LOD cut in every view ([renderer](../subsystems/renderer.md), "One cut for every view", `scene_renderer.cpp`'s `cut_view`), because two views' different levels in one bottom-level structure made shadow rays meet the far ground from below. Under `--shadows rt` — the owner's path, with either raster — a coarser periphery returns nothing until the chain can hold per-view cuts.

## 3. The levers, ranked

Each bound is read from the tables above at 11520×2160 (forest / valley / heaviest) unless it says otherwise; "picture: none" means the change cannot move a pixel by the tests' contract.

| # | Lever | Returns (bound) | Picture | Size | Row |
|---:|---|---|---|---|---|
| 1 | **The sun ray's masks in the side views and the far forest**: a coarser rule where the eye cannot resolve a leaf's shadow — the casters past a distance or in the side views as opaque forms or the coverage proxy, R115's rules chosen by region | all casters solid: **37.1 / 13.9 / 30.6 ms** (5.9 / 4.3 / 2.5 at 3840), of which the sides ≤ 31.2 / 9.6 / 28.1; the proxy at 0.2 everywhere already returns 19.3 / 4.5 / 11.6, and `half_sun` another 1.6 / 0.4 / 1.1 | changes: leaf-shadow texture becomes blob or solid shadow in the regions named (R115 stage 2 counted the codes per rule) | M | proposed R138 |
| 2 | **Primary visibility rasterized** (`--raster hw`, the engine's default; the owner's v34 run asked for `rt`) | **11.6 / 10.0 / 25.5 ms** of GPU total (trace less hw 12.2 / 8.9 / 25.1, the chain 1.1 / 2.0 / 2.2, the resolve −1.2); at 3840 R127 measured the mesh path 5.3 against the trace's 15.4 in the live session | none (the visibility buffer is word for word the rays', the vertex and ray tests assert it); and it ends the rt flight's overflow past the chain's capacity | S (a flag, the proof of concept's launcher) | R127's finding; no engine row |
| 3 | **The chain's cluster build** (`clas`), which scales with the clusters built | clas 6.3 / 4.0 / 6.6 of `rt` 11.0 / 8.2 / 11.2; at most R122's hidden share of what is built for the picture — but R122(b) keeps a hidden pair as a caster, so its structure stays; the build returns only where a coarser caster replaces it | none if casters keep their LOD; a stated LOD rule otherwise | L | R122 |
| 4 | **The hidden forest's pairs in the rasterizer** (R122(a)) | on the hw path ≤ 81% (R118's hidden share) of `hw` 8.8 / 6.0 / 9.7 ≈ **7.1 / 4.9 / 7.9 ms**; nothing on the rt path's trace, which pays per ray and not per hidden cluster | none (occlusion never changes the picture) | L | R122 |
| 5 | **The material at half rate in the side thirds** (R126 step 2) | ¾ of the sides' material stage: **≤ 2.9 / 3.9 / 1.8 ms**; nothing at 3840 | changes: the sides' texture detail per 2×2 block where a block holds one pair | M | R126 |
| 6 | **The terrain's resolve reads** (R136, being built tonight) | ≤ the material stage, which holds every material: 5.96 / **8.31** / 3.97 (the valley, most ground in view, is the largest); the terrain's own share is not separable without a timer per material class | R136's to state | M | R136 |
| 7 | **The deformed-vertex pool** (the flora's wind; no animals in the flight) | 2.9 / 2.4 / 3.1 (1.3–1.4 at 3840), all of it only with motion off | changes (no wind) unless the pool's blocks are cut, e.g. a still crown in the side views | M | proposed with #1's region rule |
| 8 | **The medium** (`frame_passes`) | 2.2 / 2.2 / 2.5 (0.8–0.9 at 3840) | changes | S–M | — |
| 9 | **The half-rate sun pass itself** | 2.3 / 1.0 / 1.4 is what R126's pass costs; it already returns more (R126 stage 4) | — | — | R126 |
| 10 | The cull, the sky, the tables, the light shadows | 1.1–1.2, ≤ 0.04, ≤ 0.07, ≤ 0.18: nothing to win | — | — | — |
| 11 | A coarser periphery (`--peripheral-lod`) | **0** under traced shadows (one cut for every view, measured to the pair) | — | L (per-view cuts in the chain) | — |
| 12 | The shadow cascades (`--shadows csm`) | not measured: the owner's path never used them | — | — | — |

**The CPU**, which does not limit these frames but would a 60 Hz one (the walk's 17.5 ms median is over 16.7):

| # | Lever | Returns (bound) | Picture | Size | Row |
|---:|---|---|---|---|---|
| C1 | **The world's tiles built on the frame thread** | 10.4 ms a frame (four tiles a frame, 508 pending) through the whole valley and heaviest holds, and the walk's `host_ms` p95 22.3 (`world.tiles_ms` p95 17.0) — the walk's hitches' CPU | none | M | proposed W89 |
| C2 | **The passes' recording** (`cpu.passes`) | 4.3–5.2 ms offscreen, 6.9 in the walk, on the frame thread | none | M | — |
| C3 | **The ground queries** of the walk's tick | 3.2 ms a frame (4,321 queries) | none | S–M | — |

**What the live session adds is the largest unknown.** At the same three poses at 3840×2160 the owner's walk measured 60.0, 47.7 and 71.2 GPU ms where the replay measured 26.0, 21.5 and 23.0, with 1.52, 1.64 and 1.25 million visible pairs against 0.93, 1.08 and 0.59, and a trace of 17.0, 12.0 and 32.3 against 4.8, 3.3 and 6.9. The replay has no module (416 animals), no player body and no third-person boom (`view.camera.boom_m`, 2.5 m behind the eye the records name) — any of which changes what the camera sees — and R127 found the same gap at 11520 on v26. The matched ablation (the owner's recording at population 0, then without the player) is the proof of concept's session's to run; until then the levers above are ranked on the replay, and the live frame is at least as heavy.

## Next

Nothing was built: the top lever with no picture change (#2) is a flag that exists, and the top lever overall (#1) changes the picture and is not small. The briefs:

- **#2, now, no engine work**: the proof of concept's launcher draws the primary visibility with `--raster hw` (the engine's default) and keeps `--shadows rt`. On these poses that is 10–25 ms of the 11520 frame for the same visibility buffer, and the chain stops overflowing. The owner's v34 walk asked for `rt`.
- **#1, proposed R138 — the sun's masks by region.** The sweep's rules today apply to the whole frame; the lever is the same rules chosen per pixel by a region the owner already accepts as peripheral: R126's side thirds (`gfx::half_rate_sides`) and a distance past which a leaf's shadow is below a pixel at the sun's angle. Measure first which of the two carries the 31 ms the sides add (a `stop_after=cycle` flight with the rule set per region by a tunable), then state the pixels it moves with R115 stage 2's codes. The bound: 37 ms at the forest pose, 31 of it in the sides.
- **T73 (proposed) — per-view milliseconds in the frame record**: `Stats::views` already holds each view's cull, raster, trace, resolve and deform; a `views` array in `FrameRecord` (a schema version) would make this page's side-view table a measurement instead of a difference.
- **W89 (proposed) — the tiles off the frame thread**: four tiles a frame at 2.6 ms each held the frame thread through a whole held pose; build them on a worker and hand them over.

## What this does not cover

- **The live session**: the module, the player, the boom camera, HDR10 and the window (above). Every replay number is a lower bound on the owner's frame.
- **Per-view timing**, read by difference (above).
- **The chain past its capacity**: the rt flight at 11520 dropped instances at two poses; what the dropped casters would have cost the sun's rays is not in its numbers.
- **The CPU's numbers** are upper bounds: no flight was quiet by the harness's rule, and two ran beside a build at 43–51% of the CPU.
- **Picture changes** of #1, #5 and #7 are named, not counted; R115 stage 2 and R126 stage 4 have the method.
