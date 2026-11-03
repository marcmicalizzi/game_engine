# Where the forest's resolve goes at the owner's width (R115, 2026-11-01)

Roadmap R115, first stage. The owner plays the v24 forest proof of concept at 11520×2160 with `--raster rt --shadows rt` and sees 55–80 ms a frame in the forest, about 30 on the east shore looking back at it and 12 facing the sea; the recorded 3840×2160 walk of 2026-10-31 (frame 4200) put 16.3 of the forest frame's 27.2 GPU ms in the resolve, the one pass that scales with the pixels. No upscaler and no lower-resolution image: the owner has ruled those out. This page splits the resolve by stage at three poses and two widths, ranks what each stage would give back, and measures the first lever, which returned **12.4–15.4 ms of the resolve at 11520×2160** (37–46%), and the picture moved by **one code step in at most 0.01% of the pixels**, never more than one.

## How it was measured

- **The split** is `renderer.resolve.stop_after` (renderer.md, ["Where the resolve's time goes"](../subsystems/renderer.md#where-the-resolves-time-goes)): a word in `gfx::ResolveParams` that ends each pixel's work after a named stage. Under `cycle` frame f stops after stage f mod 8, so one flight measures all eight. The stages, in the order the shader does them: `decode` (the visibility word and the pair table), `reconstruct` (the triangle's corners and attributes at the pixel), `material` (UV transform and derivatives, textures, normal map, ground detail), `lighting` (the BSDF with the sun, the lights and the ambient, all unshadowed), `sun_shadow`, `light_shadows`, `sky` (the air, the exposure, the physical sky behind empty pixels), and the picture, which adds the water. The participating medium is not in the resolve; it is a frame pass, timed as `frame_passes`.
- **The word costs nothing measurable.** The forest pose at 11520×2160 resolved in 33.04 ms (mean of 64 frames, range 32.55–33.67) on a build without it and 33.14 ms (8 frames, 32.73–33.75) with it present and 0; the shore went from 41.45 to 41.43 and the sea from 33.64 to 33.66. That is within the frame-to-frame range, so the word stays.
- **The scene and flags** are the owner's: the v24 world (`reports/lab/20261031-125927-663/scene.json`, 416 animals, the campfire at the origin, the eastern sea open), its materials and derived-data cache, `--terrain-tiles --terrain-far 6 --time-rate 0 --world-rate 1 --time-of-day 15 --no-lights --lod 1 --deform-pool-mib 64 --rt-budget-mib 0 --raster rt --shadows rt --tunable renderer.flora.motion=true --tunable renderer.flora.wind_strength=0.7 --present-format hdr10`, offscreen, without the game module. The cache was not written to.
- **The poses** are one camera path that holds each pose for 96 frames, at fov 55°: the forest (4.97, 1.68, −9.87) at yaw −174.25°, pitch 4.32° (the walk's frame 4200); the east shore (47.56, 6.52, 19.18) looking back at the forest, yaw −59.3°; the same place facing the sea, yaw 120.7°. 240 warmup frames at the forest, one repeat. Each number is the **median of 8 frames** of that stage, from frames 32–95 of each hold. At 11520×2160 a single view spans 141° horizontally, so the "sea" pose sees the shore and the forest at its sides; at 3840×2160 it sees the sea alone. The 3840 forest frame reproduces the owner's walk: 26.6 GPU ms, of which the resolve is 15.9 (the walk: 27.2 and 16.3); the sea, 11.1 and 2.2 (the walk: 12.2 and 2.6).
- **The machine**: RTX 5090 (32,607 MiB), `msvc-release` at 4e442d76 plus this change. Each run was under the GPU lock with `--wait-quiet 120`. Other processes held 18.6–19.3 GB of the card's memory when each run began, with the GPU 0% busy, and the 11520×2160 runs fitted offscreen beside them. Other processes' CPU use was 8–31% at the start of every run and 12–26% at the end. The one exception is the lever's 11520 run, which began beside this branch's own debug build (100%) and ended at 22%. The GPU numbers are the pass timers', and nothing else ran on the GPU during a run.

## The split: cumulative resolve milliseconds, `--shadows rt`

At **11520×2160**:

| stage | forest | shore, facing the forest | shore, facing the sea |
|---|---|---|---|
| decode | 0.37 | 0.37 | 0.37 |
| reconstruct | 1.02 | 1.08 | 1.03 |
| material | 7.20 | 6.04 | 5.95 |
| lighting | 8.08 | 6.86 | 6.78 |
| sun shadow | 20.09 | 25.88 | 17.66 |
| light shadows | 32.18 | 40.88 | 32.95 |
| sky | 32.58 | 41.26 | 33.42 |
| **the picture (+ water)** | **32.99** | **41.44** | **33.76** |
| GPU frame | 52.92 | 67.21 | 53.74 |
| visible clusters / shadow casters | 1,313,755 / 6,992 | 2,074,035 / 325,432 | 1,870,598 / 2,097 |

At **3840×2160**:

| stage | forest | shore, facing the forest | shore, facing the sea |
|---|---|---|---|
| decode | 0.13 | 0.13 | 0.09 |
| reconstruct | 0.37 | 0.40 | 0.29 |
| material | 2.20 | 2.06 | 1.21 |
| lighting | 2.50 | 2.32 | 1.47 |
| sun shadow | 8.08 | 9.07 | 1.65 |
| light shadows | 15.62 | 14.70 | 2.08 |
| sky | 15.78 | 14.76 | 2.18 |
| **the picture (+ water)** | **15.87** | **14.87** | **2.21** |
| GPU frame | 26.62 | 32.96 | 11.08 |
| visible clusters / shadow casters | 709,340 / 89,937 | 1,497,663 / 744,526 | 1,222,915 / 5,634 |

**What each stage costs, forest against the sea view** (11520×2160, the stage's own milliseconds; 3840 in brackets):

| stage | forest | shore → forest | sea |
|---|---|---|---|
| decode | 0.37 (0.13) | 0.37 (0.13) | 0.37 (0.09) |
| reconstruct | 0.65 (0.25) | 0.71 (0.27) | 0.66 (0.19) |
| material | 6.18 (1.83) | 4.96 (1.67) | 4.92 (0.92) |
| lighting | 0.88 (0.29) | 0.82 (0.25) | 0.83 (0.26) |
| sun shadow | 12.01 (5.58) | 19.02 (6.75) | 10.88 (0.18) |
| light shadows | 12.09 (7.54) | 15.00 (5.63) | 15.30 (0.43) |
| sky | 0.40 (0.17) | 0.39 (0.07) | 0.47 (0.10) |
| water | 0.41 (0.09) | 0.18 (0.11) | 0.34 (0.03) |

**The shadows are three quarters of the resolve.** At 11520×2160 the two shadow stages are 24.1 of the forest's 33.0 ms, 34.0 of the shore's 41.4 and 26.2 of the sea view's 33.8; at 3840 they are 13.1 of the forest's 15.9 and 0.6 of the sea's 2.2. Everything else is what any material resolve costs and scales with the pixels: 8–9 ms at 11520, 2.2–2.7 at 3840, of which the material's textures are two thirds.

**The lights' shadows cost as much as the sun's, and carry nothing.** With `--no-lights` the frame's light array holds two lights: the sky's moon (`frame_lighting` adds it whenever the scene's moon is on, day or night) and the campfire (`add_fire_lights`: the four nearest fires, each a point light whose reach is where it falls to 0.05 lux, 82 m for this fire at its flicker's peak). Every pixel that faces either traces a ray to it: the moon's to infinity, the fire's horizontally through the understory. At 15:00 the moon's illuminance is of order a millionth of the sun's (domain/sky's `moon_illuminance`: the moon's albedo times its angular radius squared times its phase). The fire's 250 cd is 0.002 lux in the sun's units of 128,000 at a metre and 2×10⁻⁵ at ten. Neither moves a code of either output. The rays cost 12.1–15.3 ms at 11520.

**The sun's ray is the alpha masks.** The same flight with `renderer.shadow.opaque_rays=true` (a measurement switch that makes every caster solid, so a leaf casts its whole card), with the lever below on, leaves the sun stage at **1.95 ms in the forest, 2.72 on the shore and 1.51 facing the sea**, against 11.99, 19.06 and 10.89 with the masks. About 85% of the sun ray's cost is the shader deciding non-opaque candidates: the material, the UVs and a mask sample per candidate, in `shadow_ray_blocked`'s loop. R109 found the same thing on the expanded forest ([shadow-proxy](shadow-proxy-2026-10-28.md): 34.3 ms against 11.6).

**`--shadows csm` is not a third choice on this path.** On `--raster rt` the cascaded maps are not drawn ("the direct, sw, auto, and rt paths do not draw them", renderer.md's table), so `csm` is `off` with a warning. Both drop every shadow:

| GPU frame / resolve, ms | `rt` | `csm` | `off` |
|---|---|---|---|
| 11520 forest | 52.92 / 32.99 | 30.54 / 8.51 | 30.53 / 8.57 |
| 11520 shore → forest | 67.21 / 41.44 | 34.35 / 7.38 | 34.45 / 7.34 |
| 11520 sea | 53.74 / 33.76 | 28.31 / 7.37 | 28.58 / 7.48 |
| 3840 forest | 26.62 / 15.87 | 12.99 / 2.76 | 12.78 / 2.70 |
| 3840 shore → forest | 32.96 / 14.87 | 15.14 / 2.51 | 15.04 / 2.51 |
| 3840 sea | 11.08 / 2.21 | 9.83 / 1.63 | 9.82 / 1.63 |

The rest of the frame does not depend on the shadows: the visibility rays (`rt`) are 4.9, 8.6 and 5.0 ms at 11520 with the casters and 4.6, 6.9 and 4.8 without them.

**The medium is not in the resolve, and the fire's march is not its cost.** The participating medium (R112, `systems/volumetrics`) is a frame pass after the resolve. The same flight with `--no-medium` drew no frame pass. Its GPU frames were 50.69, 64.98 and 51.47 ms, so the medium, the encode and the linear resolve cost **2.23, 2.23 and 2.27 ms** at 11520. That is the same at all three poses. The forest's `frame_passes` (2.15 ms) is 0.23 ms over the sea view's (1.92), so the near fire's march is at most that.

## The levers, ranked by what they return at 11520×2160

Forest / shore facing the forest / shore facing the sea, from the tables above:

1. **The lights' shadow rays: 12.1 / 15.0 / 15.3 ms. Built, below.** A light whose unshadowed contribution is a negligible share of the pixel's other light does not need its shadow tested. That is Ward's adaptive shadow testing ([Ward 1991, "Adaptive Shadow Testing for Ray Tracing", Second Eurographics Workshop on Rendering](https://floyd.lbl.gov/radiance/papers/erw91/erw91.html)). Sources are tested in decreasing order of potential contribution, testing stops when what is left is under a tolerance, and an untested source is added in proportion to the visibility the tested ones found. Ward reports run time roughly proportional to the share of sources tested, and an 80% cut in shadow testing for under 5% mean pixel error at a 20% tolerance. Here the tolerance is 2^-12 and the picture holds to a code.
2. **The sun ray's alpha masks: 10.0 / 16.3 / 9.4 ms** (the sun stage with masks less without them). The literature's answer is hardware opacity for alpha-tested geometry: **opacity micromaps** (`VK_EXT_opacity_micromap`), which mark each micro-triangle opaque, transparent or unknown so that the traversal skips the shader for the first two. NVIDIA measured Indiana Jones and the Great Circle's vegetation tracing at [7.90 ms against 3.58 with them](https://developer.nvidia.com/blog/path-tracing-optimizations-in-indiana-jones-opacity-micromaps-and-compaction-of-dynamic-blass) (RTX 5080). The [HPG 2023 paper on micromap baking](https://diglib7.eg.org/bitstream/handle/10.2312/hpg20231133/023-031.pdf) cites a 19–86% reduction in any-hit invocations. The first question is whether the cluster acceleration structures this path builds every frame (ADR-0025) accept a micromap; nothing here has checked. A cheaper hit-any query is already in place: the opaque path forces opacity and ends on the first hit, and the masked path commits the first kept candidate. R109's coverage proxy is the in-engine alternative and its page says why it stays off: no threshold buys the cost without moving the dappled light. The cost to the picture is none for micromaps at the mask's own resolution.
3. **The material: 6.2 / 5.0 / 4.9 ms.** Texture reads at a leaf's footprint and the terrain's layered sampling. The known technique against divergence in a visibility-buffer resolve is to shade by material: classify tiles or sort pixels by material and dispatch one shader per class ([Burns and Hunt 2013, "The Visibility Buffer: A Cache-Friendly Approach to Deferred Shading", JCGT 2(2)](https://jcgt.org/published/0002/02/04/paper.pdf), and Nanite's material passes over screen tiles). That costs a classification pass and a draw per material class, and nothing to the picture. What it returns is bounded by the stage's 6 ms and not measured. The next measurement is the split inside the stage, terrain pixels against flora, before any design.
4. **The medium (a frame pass, R113's to change): 2.2 ms** at every pose. The fire's march is at most 0.23 of it. Anything here is a request to `systems/volumetrics`, not a resolve change.
5. **The lighting: 0.8–0.9 ms.** The BSDF, the sky's transmittance per light and the nine-harmonic ambient. Nothing to take.
6. **The reconstruction: 0.65–0.71 ms** for 1.3–2.1 million visible clusters. The divergence across clusters costs little; the reconstruction is not a lever.
7. **The sky, 0.4–0.5 ms; the water, 0.2–0.4 ms; the decode, 0.37 ms.** The decode reads 24.9 million 8-byte words, 199 MB, and the pair table. That is the bandwidth floor of a visibility buffer at this width.

The night is not measured here. With the sun below the horizon its ray is still traced wherever a normal faces its direction, for no light. Ward's rule extended to the sun, with the ambient as the rest, would skip it, which this stage leaves for a night measurement.

## The first lever: a light too faint to test

**The rule** (renderer.md, ["A light too faint to test"](../subsystems/renderer.md#a-light-too-faint-to-test)): with the lights' shadows traced, a light whose unshadowed contribution is at most `renderer.shadow.light_tolerance` (2^-12) of the pixel's luminance from the sun, the ambient and the emission is not tested. It is added at the visibility the pixel's own sun query found, which is Ward's estimate from the sources that were tested. The sun is always tested, and the sum keeps its order. 2^-12 is 0.024%. The smallest code step of the 8-bit sRGB encode is 0.87% (at white), and of the 10-bit PQ encode 0.90% (at 1,000 nits; 0.94% at 203, 1.25% at 10). Six lights skipped at the bound therefore move a pixel by a sixth of a step at most. In daylight the rule skips the moon everywhere and the fire past a few metres. At night they are the light and are traced.

**What it returned** (`lever-*` runs, same flags, same path):

| | 11520 forest | 11520 shore → forest | 11520 sea | 3840 forest | 3840 shore → forest | 3840 sea |
|---|---|---|---|---|---|---|
| resolve before | 32.99 | 41.44 | 33.76 | 15.87 | 14.87 | 2.21 |
| resolve after | **20.62** | **26.48** | **18.35** | **8.34** | **9.26** | **1.79** |
| light shadows stage after | 0.02 | −0.06 | 0.04 | 0.04 | 0.01 | 0.00 |
| GPU frame before | 52.92 | 67.21 | 53.74 | 26.62 | 32.96 | 11.08 |
| GPU frame after | **41.61** | **52.86** | **38.85** | **18.75** | **25.93** | **10.43** |

**What it did to the picture.** Three 11520×2160 captures of each pose, all from one build at the marker frames 88, 184 and 280 (8-bit PNG, the offscreen default; `--marker-captures`): `before` and `before2` with the tolerance at 0, and `after` at its default. Pixels counted where any channel differs, by the largest difference:

| pose | before vs before2 (the same run twice) | before vs after (the lever) |
|---|---|---|
| forest | 5,117 pixels (0.021%): 32 by one code, the rest by 2–115 | 7,198 (0.029%): **2,556 by one code**, the rest by 2–112 |
| shore → forest | **identical** | **86 pixels (0.0003%), all by one code** |
| sea | 18 pixels: 1 by one code, the rest by 3–64 | 630: **608 by one code**, the rest by 3–64 |

The shore pose draws the same bytes run after run, and there the lever moved 86 pixels by one code. In the forest and the sea view the same flags do not draw the same picture twice. The forest moved 5,117 pixels by up to 115 codes between two runs with the tolerance at 0, which is a whole surface present in one run and not in the other, not shading. R114's cluster cache overflow is the likely candidate (a cluster the cache could not place draws nothing that frame), but this page did not trace it. Above that noise the lever's own change is about 2,520 pixels in the forest (0.010%) and 607 in the sea view (0.002%), all by one code. That is what Ward's bound predicts at a tolerance of 2^-12: a skipped light moves a pixel by far less than a step, and the 8-bit dither turns that into a one-code flip for the few pixels standing at a rounding edge. The split itself is likewise inside the run-to-run noise: the stop word at 0 against the build without it gave byte-identical shore captures, and forest and sea histograms within that noise.

## What this does not cover

- One frame clock, at 15:00, with the moon up and one fire. A night, a dusk with the fire as the key light, or a scene with the stand-in lights on (`--no-lights` off: two orbiting point lights the size of the scene) are other cases. There the rule traces what is bright, by construction, but nothing here measures it.
- The owner's interactive frame also carries the game module, the animals' animation and the present. The GPU frames here are offscreen and without the game. The owner's 55–80 ms in the forest against 52.9 here, and this page's sea view against the owner's 12 ms, are different views: a single 11520 view spans 141°.
- The split's stages are cumulative and measured on separate frames. A stage's cost is the difference of two medians of 8, each within about ±1.5% frame to frame. The increments under 0.1 ms are at the noise.

## Stage 2: the walk through the woods, and the sun ray's split (2026-11-01)

The owner walked v25 through denser woods (the lab run `reports/lab/20261101-115716-655`, markers 1 → 2, 19 s) and recorded 62 ms a frame — the resolve 38, the primary trace 12.8, the cluster build 5.3 — over 1.44 million visible clusters and 7.8 thousand shadow casters, and plays it at 15–20 fps at 11520×2160. This stage flies that walk at both widths with every resolve stage and every sun-ray rule interleaved in one flight, counts what the sun's ray does per pixel, and weighs the levers against the masks' cost ([renderer](../subsystems/renderer.md#what-the-sun-rays-masks-cost-and-what-does-not-buy-it-back) has the rules that follow).

### How it was measured

- **The walk** is a camera path of the run's records 6693–6993 (marker 1 at pose time 197.7 s to marker 2 at 216.6 s), one key a record at the records' `pose_position`, `pose_yaw` and `pose_pitch`, fov 55°, four path frames a record (1,201 frames: the walk flown four times slower than walked, so the cycles see all of it), after W82's `make-path.ps1`.
- **One flight measures everything**: `renderer.resolve.stop_after=cycle` (frame f stops after stage f mod 8) with `renderer.shadow.sweep=true` (frame f takes the sun ray's rule (f / 8) mod 8 in the build measured, f0f4ade7: as set, every caster solid, the proxy at 0.5, 0.3 and 0.2, the mask at the ray's footprint with the pixel's cone and with the sun's disc, as set with the counting bit; the two footprint rules were taken out after this measurement and the sweep now has six). `--benchmark` writes each path frame three times (its repeats), so every cell — a stage of a rule — is the **median of 57 frames** spread along the whole walk. 120 warm-up frames at marker 1.
- **The flags** are the owner's without the game module: the v25 `scene.json`, the v22 materials, the coordinator's copy of the derived-data cache (not written to), `--terrain-tiles --terrain-far 6 --time-rate 0 --world-rate 1 --time-of-day 15 --no-lights --lod 1 --deform-pool-mib 64 --rt-budget-mib 0 --raster rt --shadows rt`, flora motion at wind 0.7, offscreen, 8-bit; and the same with `--raster hw`.
- **The machine**: RTX 5090, `msvc-release` at f0f4ade7, each run under the GPU lock with `--wait-quiet 120`. At the start of every run the GPU was idle (0%) with 19.7–20.3 GB of its memory held by other processes, and other processes used 7.8–20.1% of the CPU; at the end 7.5–27.1%, and the GPU 0–100% (the run's own last frames). The GPU numbers are the pass timers'.
- **The pictures and the counts**: one more process at 3840×2160 held marker 1's pose and marker 2's (`--world-rate 0 --no-medium`, so every capture is the same instant and the codes are the resolve's), `--marker-captures` at frames whose rule is each rule once and the as-set rule twice. The as-set captures of each marker are **identical run to run** (0 pixels differ), so every difference below is the rule's.

**What this replay does not reproduce.** At 11520×2160 the flight draws the owner's cut — 1.47 million visible clusters at the median, the cluster build 5.3 ms — but resolves in 22.5 ms where the owner's record says 38–43, and traces its primary rays in 9.6 where it says 12.8–16; at 3840×2160 it draws 0.86 million clusters and resolves in 7.2. The owner's run carried the game module (416 animals, their decisions, the fire), an HDR10 window and a live session, none of which an agent may run. The game adds no lights (no light call in its sources), so the gap is something else it brings or the session's own load; the owner's walk with `--tunable renderer.resolve.stop_after=cycle` would split it the same way.

### The walk's curves

Cumulative resolve milliseconds by stage, the frame as set (`--shadows rt`):

| stage | 3840, `rt` | 11520, `rt` | 3840, `hw` | 11520, `hw` |
|---|---:|---:|---:|---:|
| decode | 0.12 | 0.36 | 0.12 | 0.35 |
| reconstruct | 0.38 | 1.04 | 0.37 | 1.01 |
| material | 2.21 | 6.66 | 2.20 | 6.59 |
| lighting | 2.49 | 7.58 | 2.48 | 7.48 |
| sun shadow | 7.06 | 22.21 | 6.93 | 22.50 |
| light shadows | 7.14 | 21.88 | 6.96 | 22.37 |
| sky | 7.44 | 22.44 | — | — |
| **the picture (+ water)** | **7.19** | **22.52** | **7.06** | **22.99** |

The frame's passes (medians of the picture's frames):

| ms | 3840, `rt` | 11520, `rt` | 3840, `hw` | 11520, `hw` |
|---|---:|---:|---:|---:|
| primary visibility: the trace (`trace`) or the mesh rasterizer (`hw`) | 3.87 | 9.58 | 3.00 | 7.51 |
| the chain (`rt`: records, cluster build, bottom and top level) | 5.56 | 7.14 | 5.53 | 6.78 |
| of it the cluster build (`clas`) | 3.93 | 5.31 | 3.74 | 5.01 |
| the deformed-vertex pool (`deform`) | 1.30 | 1.85 | 1.67 | 2.19 |
| cull | 0.38 | 0.39 | 0.38 | 0.39 |
| frame passes (the medium) | 0.82 | 1.87 | 0.81 | 2.04 |
| resolve | 7.19 | 22.52 | 7.06 | 22.99 |
| **GPU frame** | **19.00** | **42.91** | **18.69** | **42.71** |
| visible clusters / shadow casters | 855,220 / 4,247 | 1,467,152 / 10,926 | the same | the same |

- **The sun's ray is two thirds of the resolve** at both widths: 14.6 of 22.5 ms at 11520, 4.6 of 7.2 at 3840. The lights' stage is nothing (stage 1's lever), the sky and the water 0.3–0.6, and the material 5.6 at 11520 and 1.8 at 3840, still the second item.
- **The mesh rasterizer against the trace**: 7.5 against 9.6 ms at 11520 and 3.0 against 3.9 at 3840 over the same 1.47 million clusters, and the resolve does not change with it (23.0 against 22.5, inside the noise). The frame does not either (42.7 against 42.9): the shadows keep the chain, and the hw path's pool pass is 0.3 ms dearer, which takes most of the 2 ms back. Under traced shadows the trace is not the lever.

### The sun ray's split

The sun stage's own milliseconds (the sun-shadow stage less the lighting stage, each a median of 57 frames) by rule, and the picture's change at marker 1 (3840×2160, against the as-set capture; pixels whose largest channel moved, and how many by more than 16 codes):

| rule | 3840 `rt` | 11520 `rt` | 3840 `hw` | 11520 `hw` | marker 1: pixels changed (> 16 codes) |
|---|---:|---:|---:|---:|---|
| as set: every mask at level 0 | **4.57** | **14.63** | 4.45 | 15.02 | — (0 run to run) |
| every caster solid (`opaque_rays`) | 0.69 | 2.43 | 0.67 | 2.60 | 1,019,878, 12.3% (910,338) |
| the proxy at 0.5 | 4.12 | 14.41 | 4.01 | 14.89 | 362,738, 4.4% (353,281) |
| the proxy at 0.3 | 3.69 | 11.90 | 3.58 | 12.41 | 408,761, 4.9% (394,234) |
| the proxy at 0.2 | 2.92 | 8.98 | 2.78 | 9.29 | 776,305, 9.4% (698,570) |
| the mask at the footprint, the pixel's cone | 5.44 | 16.37 | 5.27 | 17.13 | 363,804, 4.4% (328,051) |
| the mask at the footprint, the sun's disc | 7.60 | 25.11 | 7.34 | 25.80 | 1,506,631, 18.2% (1,443,597) |

At marker 2 the footprint moved 177,345 pixels (2.1%) with the pixel's cone and 2,171,646 (26.2%) with the disc's. The largest channel difference was 108–109 codes at marker 1 and 201 at marker 2 for every rule: whole patches of sun turned to shade or back.

**What the ray does**, counted (the stop word's counting bit, marker 1 and marker 2 at 3840×2160, the rule as set):

| | marker 1 | marker 2 |
|---|---:|---:|
| pixels asking the sun / reaching it / blocked | 6,291,256 / 2,806,539 / 3,484,717 | 4,648,999 / 1,084,872 / 3,564,127 |
| non-opaque candidates per ray: mean (reaching / blocked) | 1.77 (1.11 / 2.31) | 1.87 (1.33 / 2.04) |
| candidates per ray: median, 90th, 99th percentile | 1, 5, 13 | 1, 4, 13 |
| masks sampled per ray: mean (reaching / blocked) | 1.61 (1.04 / 2.07) | 1.76 (1.29 / 1.90) |
| masks sampled per ray: median, 90th, 99th percentile | 1, 4, 12 | 1, 4, 12 |

**A pixel buys one or two mask decisions in dense canopy, and the tail a dozen.** The masks' 3.9 ms at 3840 is about ten million decisions. The cost is the shape of each: a dozen dependent loads (the hit's entry, its pair, the instance, the cluster's material, the material, the cluster, the triangle, three UVs, the texel) behind the traversal's hand-back to the shader, and a warp of 32 rays waits for its longest, which the 90th and 99th percentiles put at 5 to 13. That is why the footprint lost: the texel fetch is one link of the chain, and the lever added three (the texel table, the mesh's grid step, the receiver's derivatives for every pixel that asks), while a coarse level, which keeps a card's coverage but not its holes, changed which rays get through. With the disc's cone (a 20 m crown's cone is 19 cm across) the decisions were coarse enough to move a fifth of the picture, and its added time suggests the rays that went through met more candidates (not counted: the counting bit was flown with the rule as set only).

### The levers, against the brief's rule

The rule: a change under a few codes in the penumbra's texture is the trade; a changed shadow edge is not.

1. **(a) The mask at the ray's footprint**: slower at both widths and both cones (+1.7 and +10.5 ms at 11520), and 4.4% to 26% of the pixels moved by more than 16 codes. Removed; it is in the history at f0f4ade7, behind `renderer.shadow.mask_footprint`.
2. **(b) A stricter proxy for the sun alone**: the time comes only with the shadow's shape. At 0.5 it saves 0.2 ms at 11520 and moves 4.4% of the pixels (R109's v20 forest moved nothing at 0.5; v25's woods have dense clusters — far crowns and impostors — that pass it); 0.3 saves 2.7 ms for 4.9%; 0.2 saves 5.6 ms for 9.4%. Every one closes sunflecks and moves shadow edges, so none ships. The eye reads a canopy's shadow for its density (the sunflecks are pinhole images of the sun's disc, a leaf's umbra ending about 107 of its widths behind it, which is the geometry of the sun's 9.3 mrad disc and not a measurement; the paper on canopy penumbrae, Anderson and Miller 1975, "Forest cover as a solar camera: penumbral effects in plant canopies", was found by title and has not been read here), and the proxy changes the density.
3. **(c) Opacity micromaps**: the cluster structures take them. `VkClusterAccelerationStructureBuildTriangleClusterInfoNV` (VK_NV_cluster_acceleration_structure) carries `opacityMicromapArray` (0 disables micromaps for that cluster), `opacityMicromapIndexBuffer` (0: the triangle's own index), `opacityMicromapIndexType` and its stride, and VUID-10881 asks for `VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_CLUSTER_OPACITY_MICROMAPS_BIT_NV` in the build's input flags ([the reference page](https://docs.vulkan.org/refpages/latest/refpages/source/VkClusterAccelerationStructureBuildTriangleClusterInfoNV.html)). A micromap array is built once per mesh from its mask and UVs (R109's page has the bake through meshoptimizer and the memory: kilobytes to megabytes per species after deduplication), so what the frame pays is its cluster build with the arrays attached, not a build of micromaps for the cut. A two-state array would let the traversal decide a card's interior and corners and hand the shader only its edge — at the 99th percentile's 12 masks, most of them. Whether the template instantiation path carries the same fields was not read. **This is the lever, and it is the chain's** (`clas_records.slang`, `cluster_acceleration.h`): a request, not built here. Its proof is the counting bit: the masks a ray samples should fall from 1.6 to the edges'.

What it would return is bounded by the opaque rule: the sun stage at 2.4 ms against 14.6 at 11520, 12.2 ms of the frame's 42.9; the material stage (5.6 ms at 11520) is next after it, and its terrain-against-flora split (this stage's item 4) was not taken.

## Stage 3: the owner's own path, with the module (2026-11-02; the proof of concept's session measured it)

Roadmap R127. Stage 2's replay carried no game module and resolved the v25 woods in 22.5 ms at 11520×2160 where the owner's record said 38–43; the split of what the live run adds could only come from the live run. The proof of concept's own session took it, through its launcher, on the owner's latest body-free recording (`reports/lab/20261102-152929-957/input.jsonl`, 29,011 ticks at 240 Hz, 120.9 s): the v26 scene, 416 animated animals and their loops, the HDR10 swapchain, the visible 11520×2160 window, `renderer.resolve.stop_after=cycle` with `renderer.shadow.sweep=true`. Its report is `D:\workspace\forest_wildlife_poc\reports\r127\` (`HANDOFF.md`, `STAGES.md`, `analysis.json`, the two runs' folders), outside this repository and read here as text. **Upper bounds**: a shared machine, other CPU 9.1% at the start and 13.5% at the end of the sweep, the GPU lock held, the harness's `quiet` false; the normal run quiet, other CPU 3.5% to 8.4%. The numbers are medians of interleaved samples over the woods interval (80–120 s of the recording, chosen before the sweep was read), 10–32 samples a cell, and a stage's increment is a difference of medians, not a paired subtraction. One caveat of method the engine should remove (roadmap T70): in a windowed session the stage a frame drew is not `record.frame mod 8` but `round(time × 240) / 4 mod 8`, which the analysis derived from the tick count and checked against the session's length.

Cumulative resolve milliseconds by stage, the frame as set and with every caster solid (`rt` primary visibility):

| stage | as set | every caster solid |
|---|---:|---:|
| decode | 0.37 | 0.37 |
| reconstruct | 1.09 | 1.10 |
| material | 6.64 | 6.68 |
| lighting | 7.61 | 7.41 |
| sun shadow | 28.25 | 10.61 |
| light shadows | 28.99 | 11.02 |
| sky | 29.50 | 11.09 |
| **the picture** | **29.72** | **11.39** |

The sun's ray is **20.6 ms of the 29.7**, 69% of the resolve, and **3.2 ms** with every caster solid: the alpha masks are about **17.4 ms** of the owner's frame at full width, against 12.2 on stage 2's replay. The proxies behave as on the replay — 0.5 saves 0.7 ms, 0.3 saves 3.6, 0.2 saves 9.4 (the picture at 29.0, 26.1 and 20.3) — and none ships, for stage 2's reason. The material stage is 5.5 ms after the reconstruction, the replay's number.

The frame's passes, the sweep (`rt` primary, quiet false) and the normal run (`hw` primary, the same recording, no tunables, quiet true), medians over the woods:

| ms | `rt`, the sweep's as-set picture frames (20) | `hw`, the normal run (792) |
|---|---:|---:|
| primary visibility: the trace or the mesh rasterizer | 15.40 | 5.25 |
| the chain | 6.26 | 5.93 |
| the deformed-vertex pool | 1.62 | 1.42 |
| resolve | 29.72 | 31.69 |
| **GPU frame** | **56.91** | **50.10** |

**What the live run adds, against stage 2's replay at the same width**: the frame 56.9 against 42.9. The sun's stage is 20.6 against 14.6, the primary trace 15.4 against 9.6, the chain 6.3 against 7.1, the pool 1.6 against 1.9, the material the same. Two thirds of the 14 ms is in the two traversals, and nothing else moved. What the module brings a traversal is 416 deforming instances — cluster templates instantiated every frame, each its own bottom-level structure — and the v26 woods are not the v25 woods (G120's conifer forms, a different cut), so **whether the animals are the cause is not established**: a matched ablation, the same recording at population 0, is the next measurement, and the launcher's population setting takes it. The mesh rasterizer's 5.3 against the trace's 15.4 is the one clean lever here, and the launcher's default is the mesh path with traced shadows since this run; 56.9 against 50.1 is not a clean A/B (sample counts, poses, the sweep's own rules and the background load differ).

**Two findings for the chain's capacity** (roadmap R128). One frame of the sweep (record 3,233, at 72.85 s, before the woods interval) built 172,032 structures against a wanted 178,141 and dropped 4,933 instances, while the log's current capacity already read 338,115: the frame was built against the capacity before a growth, and the report names the one the GPU's counts returned to, so raising the budget would not have helped. The normal run had the same once (162 instances, 13 casters), and its swapped growth took 254.75 ms, 248.6 of it allocating the bottom-level set — on the frame thread, swapped or not.

**Where that leaves full width.** At 11520×2160 on the owner's path the frame is 50 ms with the mesh rasterizer: the resolve 31.7 (the sun's ray about 21, the material 5.5), the rasterizer 5.3, the chain 5.9, the pool 1.4, the rest about 6. Opacity micromaps (R120) are bounded by the solid row at about 17 ms; a shading rate by region (R126) at about 10 of the resolve's remainder; the hidden forest (R122) is where the rasterizer's and the chain's milliseconds are. At 3840×2160 the v26 handoff's 35 s walk presented 14.6 ms median and 28.1 p95 on the lake's shore, and the woods are the measurement still to take there.

## Stage 4: a shading rate by region (2026-11-02)

Roadmap R126 ([renderer](../subsystems/renderer.md#a-shading-rate-by-region) has the rule and why it is shaped so). The sun's ray asked once per 2×2 block of one pair in the frame's sides by a quarter-size pass, `resolve_half_sun.slang`, and taken by the resolve for the block's other three pixels; the default is a third at each side of a frame wider than 2.5:1 and nothing otherwise. A shading rule that changes the picture in the regions it names, measured here as what it returns and what it moves.

### How it was measured

- **The walk** is stage 2's path (records 6693–6993, four path frames a record, 1,201 frames, `walk.json` of stage 2's scratch, copied), flown with stage 2's flags but without `renderer.shadow.sweep`: the v25 `scene.json`, the v22 materials, the coordinator's derived-data cache (nothing cooked into it), `--terrain-tiles --terrain-far 6 --time-rate 0 --world-rate 1 --time-of-day 15 --no-lights --lod 1 --deform-pool-mib 64 --rt-budget-mib 0 --raster rt --shadows rt`, flora motion at wind 0.7, offscreen, 8-bit, `--warmup 120 --wait-quiet 120`, three repeats.
- **One flight a width measures every cell**: `renderer.resolve.stop_after=cycle` (frame f stops after stage f mod 8) with `renderer.resolve.half_rate_sweep=true` (frame f takes the sides' share (f / 8) mod 3: none, as set, a half — the half is the whole frame), so every cell — a stage at a share — is the **median of 150 frames** along the whole walk.
- **The pictures**: one more process at 11520×2160 held markers 1, 2 and 3 (marker 3 at (6.63, 1.33, −24.95), yaw 170.2°, pitch −0.7°) 48 frames each with `--world-rate 0 --no-medium`, captured by `--marker-captures` at each pose's frames 0 (full rate), 8 (a third), 16 (a half) and 24 (full rate again). A pixel's change is its largest channel difference in codes.
- **The machine**: RTX 5090, `msvc-release` at the branch's head (R126's two commits on 643e0854), each run under the GPU lock. The 11520 flight was **not quiet** (`quiet` false): other processes held 9.2% of the CPU at its start and 28.8% at its end, the GPU 66% busy at the end (the run's own frames), 22.3 GB of its memory in use; the 3840 flight was quiet (3.2% and 2.5%). The GPU numbers are the pass timers'.

### What it returns

Cumulative resolve milliseconds by stage, and the frame's passes, at each share of the sides:

| stage | 11520, none | 11520, a third (default) | 11520, a half (all) | 3840, none | 3840, as set (default: none) | 3840, a half (all) |
|---|---:|---:|---:|---:|---:|---:|
| decode | 0.36 | 0.35 | 0.35 | 0.12 | 0.12 | 0.12 |
| reconstruct | 1.08 | 1.07 | 1.08 | 0.40 | 0.40 | 0.39 |
| material | 6.93 | 6.87 | 6.85 | 2.32 | 2.32 | 2.33 |
| lighting | 7.94 | 7.82 | 7.79 | 2.61 | 2.63 | 2.67 |
| sun shadow | 23.27 | 18.92 | 17.67 | 7.43 | 7.58 | 6.04 |
| light shadows | 23.21 | 18.95 | 17.56 | 7.47 | 7.55 | 6.07 |
| sky | 23.48 | 19.21 | 17.77 | 7.65 | 7.67 | 6.17 |
| **the picture (+ water)** | **23.43** | **19.25** | **18.04** | **7.62** | **7.84** | **6.20** |
| the half pass (`half_sun`) | 0.00 | 1.20 | 1.72 | 0.00 | 0.00 | 0.67 |
| **resolve + half pass** | **23.43** | **20.45** | **19.76** | **7.62** | **7.84** | **6.87** |
| the sun's stage (sun shadow − lighting) | 15.34 | 11.10 | 9.88 | 4.82 | 4.95 | 3.37 |
| **GPU frame** | **44.18** | **40.90** | **39.65** | **19.03** | **19.28** | **18.32** |

- **At the owner's width the default returns 3.0 ms of the resolve net of its pass, 3.3 of the GPU frame** (44.2 → 40.9); the whole frame at half rate 3.7 and 4.5. The replay's full-rate numbers are stage 2's (23.4 against 22.5 for the resolve, 44.2 against 42.9 for the frame, on a busier machine).
- **It is not the bound the row set** (about 10 ms): the sun's stage falls from 15.3 to 11.1 ms where a quarter rate over two thirds of the frame would leave 7.7. The rule takes only blocks of one pair, and the rays that cost — a leaf's edge, the ground behind a card, a thin branch, the masks' candidates — are in the blocks it cannot take. The stage's drop is three quarters of the taken blocks' cost, so the taken blocks carried about 5.6 ms of the sides' 10.2 (2/3 of 15.3): about 45% of the sides' sun cost is in blocks of two pairs or more, and stays per pixel by design.
- **At 3840×2160 the default changes nothing**: the as-set share records no pass (0.00 ms in every frame) and its resolve is the full rate's within the noise (7.84 against 7.62, the same work). The whole frame at half rate there would save 0.75 ms of 7.6.

### What it moves

At 11520×2160, against the full-rate capture of the same frame:

| marker | region | pixels changed | share of the region | mean change (codes) | 99th percentile | by more than 16 codes | largest |
|---|---|---:|---:|---:|---:|---:|---:|
| 1 | left third | 183,621 | 2.21% | 40.2 | 72 | 169,295 | 109 |
| 1 | centre third | 0 | 0 | — | — | 0 | 0 |
| 1 | right third | 300,616 | 3.62% | 40.7 | 70 | 283,562 | 106 |
| 2 | left third | 126,043 | 1.52% | 40.3 | 75 | 111,332 | 101 |
| 2 | centre third | 0 | 0 | — | — | 0 | 0 |
| 2 | right third | 339,226 | 4.09% | 59.3 | 97 | 335,435 | 173 |
| 3 | left third | 257,527 | 3.11% | 42.1 | 72 | 248,776 | 107 |
| 3 | centre third | 0 | 0 | — | — | 0 | 0 |
| 3 | right third | 254,984 | 3.07% | 84.2 | 129 | 253,752 | 187 |

With the whole frame at half rate the left and right halves change 2.5% and 3.5% (marker 1), 1.5% and 3.6% (marker 2), 2.6% and 2.7% (marker 3), by the same codes. **Run to run the captures are identical** at markers 2 and 3 (0 pixels between frames 48 and 72, 96 and 120); at marker 1 the path's first frame differs from its 24th in 258 pixels of the left third, the flight settling on its first frame, so marker 1's left count carries up to 258 pixels that are not the rule's.

**What the change is.** A changed pixel is one of a block's three that took its top-left pixel's answer where the sun's edge passes between them: a sunfleck or a shadow's edge moved by up to one pixel, so it is lit where it was shaded or the reverse — which is why nearly every change is a large one (the 16-code line is the whole of it) and there are no small ones. On the outer monitors of the owner's surround a shadow's edge is drawn on a 2×2 staircase; everything else — the material, the lighting, the silhouettes, the centre monitor — is the full-rate picture to the byte. Whether that is a trade the eye accepts at the periphery is the owner's to read in the captures; the lever is a tunable (`renderer.resolve.half_rate_sides`, 0 turns it off).

### What is left

The material stage at half rate (R126's second step) is not built: its two side thirds at a quarter of the rate are at most 2.8 ms of its 6.9 at 11520, less the same share of mixed blocks this stage found (perhaps half of that), with a quarter-size G-buffer of the block's material and a second pass. Opacity micromaps (R120) attack the cost this rule cannot take — the masks' candidates in mixed blocks — and the two compose: a micromap makes a taken block's one ray cheaper and a mixed block's four.
