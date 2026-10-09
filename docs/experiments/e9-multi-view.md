# E9: three-view surround against a Panini projection at 11520×2160

- **Question ([10 §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing)):** what a three-view surround costs against one wide view and against a Panini projection at the owner's 11520×2160, and what TAA, denoisers and per-view VRS do to that. Asked by [04 §4.6](../plan/04-renderer.md#46-extreme-displays), which makes multi-view a first-class concept and foveation by attention region the way to pay for it.
- **Date:** 2026-09-18. **Machine:** Intel Core i9-10980XE (18 cores), 63.7 GB, Windows 11 Pro 10.0.26200; **GPU:** NVIDIA GeForce RTX 5090, driver 610.88, Vulkan 1.4.341, 32,404 MB device-local. **Build:** `msvc-release`.
- **Machine state:** **not quiet, WARNING raised on the CPU threshold throughout.** Over the 72 samples the two runs took (`machine_state.start` and `.end` of every benchmark), other processes used **5.8% to 36.3% of the CPU, median 18.3%**, against the harness's 10% threshold. The GPU was **quiet before every single run** — all 36 `start` samples are under 20% busy, median 6%, and the card held 4.1–5.7 GiB of 31.8 GiB including this process — and the 13 samples over 20% are all `end` samples, which is the benchmark's own last frames still in flight when the sampler ran. Every number here is a GPU timestamp rather than a CPU clock ([renderer](../subsystems/renderer.md#statistics)), and every comparison below is between configurations measured in the same process minutes apart, so the ratios hold; absolute milliseconds are upper bounds and do not compare against another day's.
- **Decision:** the multi-view design, recorded as a status note in [04 §4.6](../plan/04-renderer.md#46-extreme-displays) and built as `renderer::ViewSet` ([renderer](../subsystems/renderer.md#multi-view-a-viewset-over-one-scene)). The row stays **Measured**, not Done: three of §4.6's asks — per-view VRS, per-view internal resolution and ray budgets, and screen-space passes per view — cannot be measured at all yet, because the RHI has no shading-rate path and there is no TAA, denoiser or upscaler to run per view.
- **Sequel, the same day:** ["After the fixed-cost work"](#after-the-fixed-cost-work-2026-09-18-later-the-same-day) re-measures every table below against the three changes the "Hi-Z is entirely fixed" and "the resolve is about 45% fixed" findings provoked. The frame at 11520×2160 fell 16–19% loaded and 29–31% empty, byte-identical picture and identical visible set. The original numbers here are left exactly as they were taken.

## Setup

`renderer::ViewSet` renders N views over one `GpuScene`, each with its own camera orientation, its own (possibly off-axis) projection, its own rectangle of the target and its own quality tier. Five layouts were measured at **11520×2160**:

| Name | What it is |
|---|---|
| `single` | one rectilinear view over the whole target: 55° vertical, **140.4° horizontal**. The distorted baseline, and what the renderer did before this change |
| `surround3` flat | three views, one per 3840×2160 monitor, with per-monitor off-axis frusta and the monitors coplanar (`--side-yaw 0`). Geometrically the three thirds of the same frustum as `single` |
| `surround3` 30° | the same three monitors physically turned 30° inward, so each view's camera is turned and its frustum reshaped |
| `panini` d = 0.5 and d = 1 | one view showing **the same 140.4° field of view**, rendered rectilinear into an oversampled source and resampled with a Panini projection in the resolve |
| `surround3` + `--peripheral-lod` 2 and 4 | the flat surround with the side monitors' LOD pixel threshold multiplied, which is §4.6's foveation by attention region as far as it is built |

Two corpora, both through `engine-host`'s offscreen path (engine-view sizes its swapchain from a real window, so a surround-sized frame is captured through the protocol):

- **FlightHelmet grid**, `grid_instances: 5` — 25 instances of a 94,722-triangle mesh, 2,348 clusters, **58,700 (instance, cluster) pairs**.
- **Heightfield**, `grid: 1025` — one instance, **2,097,152 triangles**, 46,626 clusters and pairs.

The cameras are explicit and **inside** the content, which is the one thing a surround measurement must get right: an orbit that frames a scene at 55° vertically leaves the outer thirds of a 48:9 frustum empty, and a first attempt at these tables did exactly that and reported side monitors with zero visible pairs. The helmet camera stands inside the 5×5 grid at a helmet's own height; the heightfield camera stands 2 m above the terrain looking across it.

```powershell
# One engine-host session per corpus: load once, benchmark each layout for 60 frames.
'{"jsonrpc":"2.0","id":1,"method":"render.load","params":{"mesh":"content/samples/FlightHelmet/FlightHelmet.gltf","grid_instances":5,"settings":{"raster":"hw"}}}'
'{"jsonrpc":"2.0","id":2,"method":"render.benchmark","params":{"scene":"scene1","width":11520,"height":2160,"frames":60,
  "camera":{"position":[0,0.5,2.6],"target":[0,0.4,-1],"fov_deg":55,"znear":0.02},
  "settings":{"raster":"hw","shadows":"off","views":"surround3","side_yaw_deg":30}}}'
```

Every figure was taken twice, in two separate processes; both are reported as `run 1 / run 2`. `--raster hw`, shadows off except where a row says otherwise.

## Results

### What the pictures look like

Captured at 2880×540 from the same camera, which is the same layout at a fifth of the width.

- **`single`.** The helmets in the middle third read normally. Towards each edge they are enormous and sheared: the outermost helmet occupies a quarter of the width and its stand is stretched into a wedge, because a rectilinear projection magnifies by 1/cos²θ and θ reaches 70°. This is the picture a 48:9 game renders today, and the reason §4.6 exists.
- **`surround3` flat.** Indistinguishable from `single` — it *is* the same projection, split into three off-axis frusta that tile the same frustum. The centre third is byte-identical (the test that says so is in [renderer](../subsystems/renderer.md#testing)).
- **`surround3` 30°.** The centre third is unchanged; the outer thirds show **more of the world, undistorted**. Where the flat picture had one giant sheared helmet at each end, the turned one shows two or three helmets at natural proportions. This is what three physically angled monitors actually see, and it is the picture the owner's arrangement should be producing.
- **`panini` d = 1.** The centre third is unchanged; the periphery is compressed rather than stretched. The outermost helmets are about a third the width they are in `single` and their stands are no longer wedges. Straight vertical lines stay vertical, which is the property Panini is chosen for; horizontal lines off the centre curve slightly, which is the price.
- **`panini` d = 0.5.** Halfway between the two, as the parameter says.
- **`--peripheral-lod 4`.** No visible difference at this scene size at 2880×540: the side monitors' cut is coarser but the helmets there are already small.

### FlightHelmet grid, 11520×2160 — GPU ms a frame, run 1 / run 2

| Layout | cull | hw raster | Hi-Z | resolve | **total** | visible pairs | VRAM (MiB) |
|---|---|---|---|---|---|---|---|
| `single` | 0.032 / 0.038 | 0.185 / 0.199 | 0.594 / 0.574 | 0.395 / 0.391 | **1.205 / 1.203** | 4,564 | 821 |
| `surround3` flat | 0.093 / 0.089 | 0.190 / 0.196 | 0.672 / 0.643 | 0.402 / 0.394 | **1.358 / 1.323** | 4,564 | 821 |
| `surround3` 30° | 0.088 / 0.096 | 0.138 / 0.132 | 0.647 / 0.648 | 0.336 / 0.339 | **1.210 / 1.216** | 5,376 | 821 |
| `panini` d = 0.5 | 0.041 / 0.034 | 0.297 / 0.294 | 1.007 / 1.041 | 0.403 / 0.389 | **1.747 / 1.758** | 4,587 | 1,027 |
| `panini` d = 1 | 0.040 / 0.034 | 0.431 / 0.395 | 1.186 / 1.172 | 0.463 / 0.413 | **2.120 / 2.014** | 4,603 | 1,130 |
| `surround3` flat, peripheral LOD ×2 | 0.093 / 0.091 | 0.186 / 0.182 | 0.640 / 0.642 | 0.388 / 0.406 | **1.307 / 1.322** | 3,752 | 821 |
| `surround3` flat, peripheral LOD ×4 | 0.083 / 0.087 | 0.179 / 0.180 | 0.661 / 0.647 | 0.394 / 0.394 | **1.317 / 1.307** | 3,269 | 821 |

### Heightfield 1025 (2.1 M triangles), 11520×2160 — GPU ms a frame, run 1 / run 2

| Layout | cull | hw raster | Hi-Z | resolve | **total** | visible pairs | VRAM (MiB) |
|---|---|---|---|---|---|---|---|
| `single` | 0.032 / 0.028 | 0.141 / 0.145 | 0.603 / 0.591 | 0.413 / 0.389 | **1.190 / 1.154** | 1,432 | 693 |
| `surround3` flat | 0.086 / 0.085 | 0.184 / 0.191 | 0.657 / 0.638 | 0.407 / 0.402 | **1.335 / 1.316** | 1,538 | 693 |
| `surround3` 30° | 0.091 / 0.081 | 0.176 / 0.178 | 0.650 / 0.635 | 0.394 / 0.388 | **1.311 / 1.282** | 1,807 | 693 |
| `panini` d = 0.5 | 0.035 / 0.032 | 0.330 / 0.336 | 1.033 / 1.001 | 0.403 / 0.418 | **1.802 / 1.786** | 1,438 | 899 |
| `panini` d = 1 | 0.033 / 0.030 | 0.424 / 0.405 | 1.300 / 1.215 | 0.450 / 0.437 | **2.207 / 2.087** | 1,438 | 1,002 |
| `surround3` flat, peripheral LOD ×2 | 0.095 / 0.079 | 0.193 / 0.198 | 0.650 / 0.651 | 0.397 / 0.378 | **1.335 / 1.306** | 1,421 | 693 |
| `surround3` flat, peripheral LOD ×4 | 0.084 / 0.077 | 0.197 / 0.183 | 0.634 / 0.652 | 0.385 / 0.395 | **1.300 / 1.306** | 1,382 | 693 |

### Per view, one frame of the flat surround (run 1)

| Corpus | view | pairs | cull | hw | Hi-Z | resolve | total |
|---|---|---|---|---|---|---|---|
| helmets | left 3840×2160 | 701 | 0.035 | 0.084 | 0.218 | 0.148 | 0.485 |
| helmets | centre | 2,976 | 0.030 | 0.055 | 0.219 | 0.125 | 0.429 |
| helmets | right | 887 | 0.028 | 0.051 | 0.236 | 0.129 | 0.443 |
| terrain | left | 184 | 0.027 | 0.078 | 0.223 | 0.149 | 0.476 |
| terrain | centre | 1,133 | 0.029 | 0.083 | 0.227 | 0.165 | 0.504 |
| terrain | right | 221 | 0.031 | 0.023 | 0.207 | 0.094 | 0.354 |

### Fixed against scene-dependent: the same frame with nothing in it

The camera turned to face the empty sky, same corpus, same resolution, 60 frames.

| Layout | cull | hw | Hi-Z | resolve | total |
|---|---|---|---|---|---|
| `single` 11520×2160 | 0.025 | 0.011 | **0.595** | **0.186** | 0.817 |
| `surround3` flat | 0.058 | 0.014 | **0.618** (3 × 0.206) | **0.184** (3 × 0.062) | 0.874 |
| `panini` d = 1 (source 22,758 px) | 0.022 | 0.012 | **1.216** | **0.241** | 1.491 |

Against the loaded frames above: **Hi-Z is entirely fixed** — 0.595 ms empty, 0.574–0.594 ms full, the same number — and **the resolve is about 45% fixed**, 0.186 ms of sky against 0.39–0.41 ms when the scene covers the frame. The whole of a 3840×2160 view costs **0.28–0.30 ms with nothing in it at all**, of which 0.206 is its Hi-Z pyramid.

### Ray-traced shadows, which is where multi-view costs memory

| Corpus | Layout | `gpu_ms.rt` | resolve | total | pairs | VRAM (MiB) |
|---|---|---|---|---|---|---|
| helmets | `single` + `--shadows rt` | 1.787 / 1.759 | 0.770 / 0.775 | 2.772 / 2.737 | 4,870 | 1,587 |
| helmets | `surround3` flat + `--shadows rt` | 1.792 / 1.792 | 0.807 / 0.765 | 2.821 / 2.793 | 4,870 | **2,275** |
| terrain | `single` + `--shadows rt` | 0.393 / 0.348 | 1.496 / 1.454 | 2.051 / 1.947 | 1,453 | 1,163 |
| terrain | `surround3` flat + `--shadows rt` | 0.266 / 0.249 | 1.984 / 1.982 | 2.476 / 2.449 | 1,572 | **1,837** |

## What surprised me

**Turning the side monitors made the frame *cheaper*, not dearer.** The flat surround costs 1.358 ms on the helmet grid and the 30° one 1.210 ms — 11% less — while drawing **18% more pairs** (5,376 against 4,564). The reason is the thing §4.6 is about: a flat 140° projection magnifies its edges by 1/cos²θ, so the outermost helmet covers a quarter of the width as a sheared wedge, and the fragments it costs are all waste. Turning the monitors to where they physically are removes the magnification: the side views' raster time halves (0.084 → 0.039 ms) and their resolve drops by a fifth, and what they show instead is *more world*. The correct picture is the faster one. I expected the yaw to be a fidelity option with a cost; it pays for itself.

**The split itself is nearly free, and the part that scales with the view count is the cull.** Three views over the same total pixels cost 1.10–1.15× one wide view of the same content, and almost all of the difference is the Hi-Z build — three pyramids of 3840×2160 are 36 dispatches against 14 for one of 11520×2160, and each needs a barrier. The resolve does not notice the split at all (0.184 against 0.186 empty). What *does* multiply is the cull: 0.032 → 0.09 ms, because each view dispatches one thread per scene pair whether or not the view can see any of them. At 58,700 pairs that is 5% of the frame; at a million pairs it would be the first thing to fix, and the fix is §4.3's per-instance culling BVH rather than anything multi-view.

**Panini costs about twice the baseline it replaces, and the resample is not why.** Showing the same 140.4° needs a rectilinear source `(d + cos t)/((d + 1) cos t)` times as wide — 1.65× at d = 0.5 and 1.975× at d = 1 — and the measured Hi-Z scales with it almost exactly (0.595 → 1.216 ms empty, a factor of 2.04 against an oversampling of 1.975). **The resample itself costs 0.055 ms at 11520×2160**, measured as the difference between the two empty resolves, and 0.02–0.07 ms on a loaded frame. So Panini is a perceptual choice and not a performance one: it buys an undistorted periphery for roughly the price of rendering it twice. A surround at 30° buys a *better* periphery for less than the baseline. If the target is one physically flat ultrawide, Panini is the only one of the two available, and that is when its price is worth paying.

**The union of per-view cuts is bigger than one view's cut, by the seams.** A cluster whose bounding sphere straddles the boundary between two monitors survives both views' frustum tests and appears in both cuts. On the continuous heightfield that is +7.4% (1,538 against 1,432); on the helmet grid it is **exactly zero** at this camera, because the discrete helmets happen to fall clear of both seams — the three views' counts sum to 4,564, which is the single view's count to the pair. The zero is luck, not a law, and it is a good illustration of why the identity that *is* a law (a flat surround's centre view equalling a single view of one monitor, pixel for pixel) is the one the test pins.

**The peripheral LOD multiplier works on the geometry and is invisible in the frame time.** At 2× the side views drop from 701 and 887 pairs to 326 and 450; at 4× to 104 and 189 — **51% and 82% off the periphery taken together, 18% and 28% off the whole frame's visible pairs**. The frame time barely moves (1.358 → 1.307 → 1.317 ms), because at 11520×2160 these two corpora are **not geometry-bound**: 80% of the frame is Hi-Z and resolve, both functions of pixels. That is not a reason to drop the lever — it is a statement that the corpus is too light to show it. A scene with ten or a hundred times the visible pairs is where §4.6's foveation earns its keep, and the corpus to prove it on does not exist yet ([04 §4.8](../plan/04-renderer.md#48-reference-renderer-and-objective-optimization)'s pathological scenes).

**Multi-view ray tracing costs memory, not time.** The union of three views' cuts builds the same structures the single view did — the trace and build times are within noise of each other (1.792 against 1.787 ms on the helmets) — but the CLAS set and the per-instance bottom-level structures have to be *sized* for the worst case, `views * pair_count`, because in principle every view can draw every cluster. That is **+688 MiB on the helmet grid and +674 MiB on the terrain**, against an actual record count that barely moves. Capacity, not traffic, and the obvious follow-up is to deduplicate the union or to build the RT geometry from one view-agnostic cut instead.

## What it decides

**The multi-view design, as built:**

1. **N views over one scene, with the per-frame working set as slices of the scene's buffers**, not a working set per renderer. A visibility id is an index into the visible list, so one list is what lets any view's resolve decode any id; the RT union is then a contiguous range of that list rather than a gather over N; and one allocation that scales with the view count is the single residency budget §4.6 asks for. The list is run-major so the views' first runs are that contiguous range.
2. **Surround is three off-axis frusta, not three cameras.** Flat, they are the three thirds of one wide frustum and the centre one is bit-for-bit a single view of one monitor — which is the invariant the tests hold and the reason a view's cut does not depend on how many views there are beside it. Angled, they are what the monitors physically see, and that is both the correct picture and the cheaper one.
3. **Panini resamples inside the resolve, in the visibility domain.** No intermediate color image, no second pass, sub-pixel-accurate reconstruction at the mapped source position, and 0.055 ms.
4. **Foveation is the LOD threshold for now.** `--peripheral-lod` is built and measured; it moves the geometry and not, on this corpus, the clock.

**What it does not decide, and what would be needed:**

- **TAA.** There is none. §4.6 says screen-space passes run per view; the seam between two views is where a temporal filter's history, jitter sequence and motion vectors either work or do not, and nothing can be said about it until there is a filter. What is needed: a TAA pass, and per-view jitter and history buffers in the `View`. The `ViewSet` already carries a per-view source extent, which is the seam that per-view jitter and internal resolution would use.
- **Denoisers.** Same answer, plus the harder half: a denoiser's spatial kernel reaches across pixels, and at a monitor seam the neighbouring pixels belong to another view with another projection. Nothing is built, so nothing is measured.
- **Per-view VRS.** `ViewQuality::shading_rate` is set by the layout (2 on a peripheral view), carried into the statistics and reported by both hosts, and **consumed by nothing**. `VK_KHR_fragment_shading_rate` is advertised by this GPU but is not in the RHI; what is needed is a shading-rate path in `domain/gfx` and either a per-view rate attachment or `vkCmdSetFragmentShadingRateKHR` per draw. Until then the periphery shades at full rate and the VRS half of E9's question is untouched.
- **Per-view internal resolution and ray budget**, the other two levers of §4.6's foveation gradient.
- **Multi-window presentation and per-output HDR metadata.** Everything here is one swapchain with the views tiled.
- **One RT cut instead of a union.** Two views at different LOD thresholds put two levels of the same surface into one top-level structure, so a ray from either may hit the other's level. With `--peripheral-lod 1` the cuts agree and nothing overlaps, which is why the tables above are clean; at 2 and 4 they do not. §4.6's "RT is view-agnostic" is the fix and it means *one cut at a view-agnostic threshold*, not the union of the views'. A related consequence: with more than one view a ray may name a pair through another view's entry, which resolves to the same (instance, cluster, triangle) and so is invisible to a capture, but it makes E2's word-for-word raster/RT invariant a single-view statement. *(2026-10-09: the cuts do not agree at `--peripheral-lod 1` when the views' projection scales differ — a turned surround's always do — and the overlap drew shadow shards on the erg; a frame that builds structures now culls every view at the finest view's LOD. [Shadow shards](shadow-shards-2026-10-09.md).)*

## After the fixed-cost work (2026-09-18, later the same day)

The numbers above stand as measured and are not edited. What follows is the same corpora, the same
resolutions and the same two layouts re-measured after the three changes E9's "Hi-Z is entirely
fixed" and "the resolve is about 45% fixed" findings provoked. Each one keeps the picture
byte-identical and the visible set exactly equal; the commits carry the individual before/after
numbers and the safety arguments, and [gfx](../subsystems/gfx.md) and
[renderer](../subsystems/renderer.md) carry the mechanisms.

1. **The Hi-Z pyramid folds six mips per workgroup** instead of one dispatch per mip. A dispatch
   per mip re-reads the whole of each mip to produce the next; a 16×16 workgroup folding a 32×32
   tile pays that read once and does the rest in shared memory. 11520×2160's fifteen mips are
   three dispatches and two barriers instead of fifteen and fourteen. The values are bit-identical
   — `min` is exact, and a power-of-two tile at a power-of-two offset covers exactly the 2×2
   subtrees the per-mip pass reduced — so the cull pass reads the same texels and the cut does not
   move.
2. **The resolve discards an empty pixel** rather than writing the sky, because the pass already
   cleared the target to it (`ResolveParams::sky_is_clear`).
3. **The resolve skips a 32×32 tile the Hi-Z build says holds nothing.** The build's first
   dispatch already reads every word of the visibility buffer and its workgroup is exactly a tile,
   so an OR across it is free; the resolve reads that word before the visibility word. Valid
   because the last Hi-Z build of a frame follows the last write to the visibility buffer whenever
   two-pass occlusion culling is on, and `ResolveParams::coverage` is zero when it is not.

**Machine state.** Same machine, same driver, same `msvc-release` build. The GPU was **quiet before
every run** — 1–16% busy at the start sample of all 48 configurations, median 2%, with 3.8–5.8 GiB
of the 31.8 GiB card held including this process — and `nvidia-smi` agreed at 3.4–4.4 GiB and 2–6%
between runs. The **CPU was not**: other processes used 3–35% for most of the run and 55–100% for
nine of the 48 configurations, against the harness's 10% threshold, because other agents were
building on the box. Every figure here is a GPU timestamp and every before/after pair was taken in
the same conditions minutes apart, so the ratios hold; **the absolute milliseconds are upper
bounds**, as they were in the tables above. Every figure was taken twice in two separate processes
and both are reported as `run 1 / run 2`; 120 frames each, `--raster hw`, shadows off.

### FlightHelmet grid ×25, GPU ms a frame — before → after

| Layout, resolution | cull | hw | Hi-Z | resolve | **total** |
|---|---|---|---|---|---|
| `single` 11520×2160 | 0.034/0.038 → 0.034/0.036 | 0.183/0.210 → 0.184/0.188 | 0.563/0.567 → **0.431/0.429** | 0.375/0.388 → **0.316/0.325** | 1.155/1.203 → **0.966/0.978** |
| `surround3` 11520×2160 | 0.090/0.089 → 0.087/0.085 | 0.184/0.185 → 0.183/0.188 | 0.623/0.620 → **0.429/0.433** | 0.374/0.386 → **0.324/0.320** | 1.271/1.280 → **1.024/1.026** |
| `single` 3840×2160 | 0.035/0.032 → 0.031/0.034 | 0.061/0.057 → 0.053/0.056 | 0.200/0.211 → **0.098/0.099** | 0.138/0.142 → **0.111/0.109** | 0.434/0.441 → **0.293/0.298** |
| `surround3` 3840×2160 | 0.083/0.082 → 0.085/0.083 | 0.056/0.059 → 0.057/0.053 | 0.271/0.266 → **0.109/0.111** | 0.134/0.140 → **0.115/0.117** | 0.543/0.547 → **0.366/0.364** |
| `single` 1920×1080 | 0.026/0.027 → 0.026/0.026 | 0.018/0.020 → 0.018/0.018 | 0.070/0.069 → **0.021/0.021** | 0.035/0.035 → **0.034/0.035** | 0.150/0.150 → **0.100/0.100** |
| `surround3` 1920×1080 | 0.075/0.077 → 0.070/0.071 | 0.018/0.018 → 0.018/0.018 | 0.129/0.123 → **0.038/0.038** | 0.038/0.038 → **0.037/0.037** | 0.259/0.256 → **0.163/0.163** |

Visible pairs unchanged in every row: 4,564 at 11520×2160 `single` and `surround3`, 2,976 and
3,042 at 3840×2160, 1,355 and 1,394 at 1920×1080.

### Heightfield 1025 (2.1 M triangles), GPU ms a frame — before → after

| Layout, resolution | cull | hw | Hi-Z | resolve | **total** |
|---|---|---|---|---|---|
| `single` 11520×2160 | 0.030/0.028 → 0.028/0.032 | 0.113/0.112 → 0.119/0.110 | 0.557/0.556 → **0.435/0.460** | 0.345/0.344 → **0.286/0.285** | 1.044/1.040 → **0.869/0.886** |
| `surround3` 11520×2160 | 0.101/0.085 → 0.080/0.077 | 0.156/0.155 → 0.159/0.160 | 0.668/0.605 → **0.432/0.432** | 0.382/0.341 → **0.297/0.291** | 1.306/1.186 → **0.968/0.961** |
| `single` 3840×2160 | 0.027/0.029 → 0.026/0.028 | 0.054/0.053 → 0.053/0.055 | 0.204/0.203 → **0.093/0.091** | 0.136/0.136 → **0.124/0.122** | 0.422/0.420 → **0.295/0.295** |
| `surround3` 3840×2160 | 0.081/0.082 → 0.078/0.081 | 0.054/0.053 → 0.055/0.055 | 0.268/0.264 → **0.106/0.106** | 0.136/0.137 → **0.131/0.131** | 0.540/0.536 → **0.369/0.373** |
| `single` 1920×1080 | 0.024/0.024 → 0.025/0.025 | 0.019/0.019 → 0.019/0.019 | 0.067/0.067 → **0.021/0.021** | 0.037/0.036 → **0.036/0.036** | 0.147/0.146 → **0.101/0.100** |
| `surround3` 1920×1080 | 0.072/0.071 → 0.069/0.069 | 0.021/0.020 → 0.019/0.020 | 0.125/0.123 → **0.040/0.045** | 0.042/0.042 → **0.043/0.043** | 0.259/0.256 → **0.171/0.177** |

Visible pairs unchanged: 1,520 and 1,609 at 11520×2160, 1,229 and 1,384 at 3840×2160, 935 and
1,070 at 1920×1080.

### The same frame with nothing in it, after

Same cameras as the tables above but turned to face empty sky, 120 frames.

| Layout, resolution | cull | Hi-Z | resolve | total |
|---|---|---|---|---|
| `single` 11520×2160 | 0.014 | 0.464/0.452 | **0.084/0.079** | 0.561/0.545 |
| `surround3` 11520×2160 | 0.034/0.037 | 0.459/0.452 | **0.085/0.083** | 0.577/0.572 |
| `single` 3840×2160 | 0.014 | 0.096/0.091 | **0.027/0.027** | 0.137/0.132 |
| `single` 1920×1080 | 0.010 | 0.021/0.021 | **0.0092** | 0.040/0.040 |

### The fixed-versus-scene-dependent split, after

The finding that provoked the work was that **Hi-Z is entirely fixed** and **the resolve is about
45% fixed**. Both halves moved, but only one of them changed shape.

- **Hi-Z is still entirely fixed** — 0.452 ms empty against 0.430 loaded at 11520×2160, which is
  the same number — and it is still the largest single pass of a frame at that resolution. It is
  now 26% cheaper rather than a different shape: it reads the whole visibility buffer twice a
  frame and writes a mip 0 the cull pass reads, and both of those are the resolution. **It is
  bandwidth-bound at about 86% of the card's peak**, so what is left is not overhead to remove;
  the next step is to move fewer bytes, and every way of doing that found so far changes the
  visible set (see below).
- **The resolve is now about 25% fixed**, not 45%: 0.081 ms empty against 0.320 loaded on the
  helmets at 11520×2160, where it was 0.184 against 0.381. The sky no longer costs a read *or* a
  write; what remains of the empty cost is the covered-tile boundary and the fragment pass itself.
- **A whole empty 3840×2160 view costs 0.13–0.14 ms**, where E9 measured 0.28–0.30, of which
  0.09 is still its Hi-Z pyramid.
- **The fixed share of a whole frame** at 11520×2160 `single` on the helmets fell from 66% to
  **57%** (0.553 ms of 0.972), and in absolute terms from 0.78 ms to 0.55.

### What did not pay

- **Two 32-bit loads instead of one 64-bit load** of each visibility word in the Hi-Z's first
  dispatch, which E9's brief suggested as a way to read only the depth half. It cannot save DRAM
  traffic — the transaction granularity is 32 bytes and reading one word in two touches every
  sector either way — and measured *worse*: Hi-Z 0.431 → 0.435 ms at 11520×2160 `single` loaded
  (inside the spread), 0.098 → 0.106 at 3840×2160, and 0.021 → 0.026 at 1920×1080, where the extra
  instructions matter and the bytes never did. Reverted. The coverage mask needs the low half
  anyway, so the question is now moot as well as answered.
- **Computing the coverage mask from both Hi-Z builds, and keeping all four 64-bit words live to
  OR them.** The first working version of change 3 did both, and it cost the Hi-Z 0.02 ms at
  11520×2160 and 17% at 1920×1080 `surround3` — a change that gives some of the resolve's win
  straight back. Folding the two halves of each word into one `uint` as it is read, and writing the
  mask only from the build the resolve actually reads, took the cost back inside the noise.
- **Starting the pyramid at half resolution** — reducing 2×2 visibility words straight into a mip
  0 that is half the size — was not tried, because it cannot be done without changing the cut.
  `occluded()` picks `mip = coarse - 2` where `2^coarse >= size` in mip-0 texels, so **any cluster
  whose screen rectangle is four texels or less across reads mip 0**, which is most of a distant
  scene. A half-resolution mip 0 is a `min` over 2×2, which is farther than either source, so the
  test rejects strictly less often and the visible set grows. It would not change the picture —
  occlusion culling never does — but it is not the byte-identical change this work was allowed to
  make, and it would have to be justified on its own terms, with the cut it produces measured.

### Follow-ups this leaves

- **The Hi-Z reads the visibility buffer twice a frame** and the second read usually finds it
  unchanged: in a static frame occlusion pass 2 draws *nothing*, so the second pyramid is the first
  one recomputed. An early-out on the pass-2 count would roughly halve the Hi-Z, and it would do so
  only when the camera is still — a benchmark with a fixed camera would show a win a moving frame
  does not get, which is exactly why it was not taken without a moving-camera measurement to go
  with it. The general form is rebuilding only the tiles pass 2 drew into.
- **The mask is 32×32 because the Hi-Z workgroup is.** A finer classification would skip more of a
  silhouette's neighbourhood; it would need its own pass or a smaller tile, and the smaller tile
  costs the Hi-Z its six-mip fold.
- **A compute resolve** was not measured. The fragment pass is now within 25% of its own empty
  cost, and the remaining time is per-covered-pixel work that a compute pass would do identically;
  the reason to try one is scheduling (overlap with the next frame's cull), not the resolve itself.

## Caveats

One machine, one driver, one afternoon, and a CPU that was 18% busy with other work throughout — the GPU was quiet before every run and every number is a GPU timestamp, so the ratios hold and the absolute milliseconds are upper bounds.

**Neither corpus is geometry-bound at this resolution**, which is the biggest limit on what these numbers can say. The helmet grid is 58,700 pairs with 4,564 visible and the heightfield 46,626 with 1,432; 80% of both frames is Hi-Z and resolve. Every conclusion about *pixels* is solid and every conclusion about *geometry* — most of all the peripheral LOD lever — is measured on a scene too light to stress it. A second data point should be a scene with 10⁵–10⁶ visible pairs, which is what [04 §4.8](../plan/04-renderer.md#48-reference-renderer-and-objective-optimization)'s corpus is for.

**Timing the views separately orders them.** A GPU timer zone writes its timestamps at ALL_COMMANDS on both ends, so bracketing each view's passes puts a synchronisation point between the views. The per-view numbers are therefore what each view costs alone, and the frame totals are an upper bound on what the views would cost if the driver were free to overlap them. The same has always been true of the per-pass numbers, so the comparison between layouts is fair; a layout that could overlap more would be undersold.

**The surround geometry is idealized.** The eye distance is the one that makes a monitor subtend the vertical field of view, the bezel gap is zero, and the monitors are assumed identical. A real arrangement has a measured eye distance, a bezel, and possibly unequal monitors; `Surround3` takes all of that, and none of it was varied here.

**`--peripheral-lod` was measured on the flat surround** so that it compares directly against the flat row. On the 30° layout the side monitors see more world, so the same multiplier would save more.
