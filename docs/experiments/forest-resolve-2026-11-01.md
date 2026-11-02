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

The owner walked v25 through denser woods (the lab run `reports/lab/20261101-115716-655`, markers 1 → 2, 19 s): 62 ms a frame at 3840×2160 — the resolve 38, the primary trace 12.8, the cluster build 5.3 — over 1.44 million visible clusters and 7.8 thousand shadow casters, and 15–20 fps at 11520×2160. This stage flies that walk as a camera path at both widths with every resolve stage and every sun-ray rule interleaved in one flight (`renderer.resolve.stop_after=cycle` with `renderer.shadow.sweep`, [renderer](../subsystems/renderer.md#the-mask-at-the-rays-footprint)), counts what the sun's ray does per pixel, and measures the lever it built, the mask at the ray's footprint.