# The second owner session, over the ashlar ruins: what it found

- **Question ([09 §9.4](../plan/09-testing-profiling.md#94-benchmark-scene-corpus)):** the owner flew the desert with a hundred ruins of the ashlar kit and reported two things — (1) **the ruins look reflective instead of matte**; (2) **green and grey ground textures shift on different position thresholds**, patches of the ground changing as he moved, with the marker key pressed where he saw it. Where does each come from, and what fixes it?
- **Date:** 2026-09-25. **Machine:** Intel i9-10980XE, 64 GB, Windows 11 Pro; **GPU:** RTX 5090 (32 GB). **The session:** 12:47 UTC, a `msvc-release` build of main at `d952a5e` (the recording's header names it, not dirty), `fly-ashlar-100.json` — the desert terrain at 2049 × 2049 over 5.12 km (seed 23, 8 m dunes, three ridges, the basin of the oasis at (0, 60)) and 100 buildings of E33's ashlar kit v2 (`kit-ashlar-cc0`, [E33](e33-hard-surface-kits.md)) — at 11520×2160 `--views surround3`, `--shadows csm`, `--start 0,5,-600 0,-4`, `view.fly.speed` 30, over main's derived-data root. **The diagnosis:** `msvc-release` builds of this change's parent (`d952a5e`) and of each fix, replaying the recording offscreen at 3840×720 `surround3` with the session's other flags over a copy of that root.
- **Machine state:** the session's own summary — other processes at 7.5% of the CPU at the start and 24.6% at the end, the GPU 17% and 6% busy with 9.8 and 9.9 GB of its 32.6 GB held by other tenants, the GPU lock the owner's. Every result below is a picture, an id buffer or a count, which a busy machine does not change; no timing from a replay is used, and the replays ran under the GPU lock (`agent-visual`).
- **Decision:** two engine fixes, neither in the kit — the terrain's colour is a map over its UVs rather than a material voted per cluster ([renderer](../subsystems/renderer.md#invariants)), and the resolve's sky term weights its specular share by the lobe's own reflectance rather than by bare Schlick ([gfx](../subsystems/gfx.md)) — and a test that holds the first at the owner's own marker cameras. The session's frame times, which paired, are the third item and have [a section of their own](#present-pacing) with its own machine and machine state: a display pacer by default.

## The recording

`D:\workspace\game_engine_local\flythrough\interactive-2026-09-25T1226-input.jsonl` (2,368 events after its header; 16,677 ticks at 240 Hz, 69.5 s) and `interactive-2026-09-25T1226-frames.jsonl` (5,677 frames: GPU 2.24 ms median and 3.11 ms p99; the frame time bimodal, 7.19 ms median and 22.6 ms p95, which is the present-pacing item of the [first session](first-interactive-session-2026-09-24.md#1-the-stutter-near-an-asset), settled in [Present pacing](#present-pacing) below). The header names the default map's hash (`95496522437046028`), so the marker is scancode 16, M, and the owner pressed it six times:

| Marker | Tick | Camera | What he had just done |
|---|---|---|---|
| 1 | 11,413 | (−209, 26, −10), yaw −105°, pitch −11° | stopped, looking east over the basin at the ridges |
| 2 | 12,063 | (−162, 17, 3), the same angles | flew forward with W (ticks 11,554–11,951) |
| 3 | 12,543 | (−181, 20, −2), the same angles | backed off with S (12,274–12,432) |
| 4 | 12,859 | (−161, 17, 3), the same angles | forward again with W (12,625–12,789), to within 0.3 m of marker 2 |
| 5 | 14,465 | (−46, 10, 37), yaw −105°, pitch −2° | flew down to the basin's edge |
| 6 | 14,899 | (−29, 9, 42), the same angles | forward with W (14,612–14,752) |

Markers 1 to 4 are a deliberate test: back and forth along one line, stopping to press M, and coming back to marker 2's spot for marker 4. The replay of the recording flew **the session's trajectory to the bit**, hash `65e2dabaad1853d3` both times, which is what makes every picture below the one the owner saw at that tick (at a third of his resolution).

## Method

1. **Replay at the markers** with `--marker-captures` and `--capture-channels ids,depth,normals`: the six markers, each drawn from its own camera after eight frames there.
2. **Replay along the moves.** A copy of the recording with its M presses replaced by presses every 10 or 20 ticks through the four moves (73 presses; `make-marker-log.ps1`, the first session's script — M moves nothing, so the copy flies the same trajectory), drawn once shaded with ids and once `--view albedo`, the base colour unlit: the only view in which the ground's colour is not also the moving point lights' and the shading's.
3. **Change the cut, not the camera.** At each marker, `--view albedo` with `--lod 1` and with `--lod 2`: the same camera, two cuts of the LOD DAG. A colour that is a function of the ground cannot change between them except where the cut's own geometric error moves the surface under a pixel.
4. **A kit member up close.** A scene of two members (`ashlar-section-2m-h100`, `ashlar-corner-h100`) on a small terrain and a three-marker camera path — face on, along the wall at a grazing angle, and the corner — drawn shaded and in albedo by each build.
5. **The kit's own data.** The GLB's material records and its embedded images, read out of the binary chunk; the metallic-roughness image's channels measured over all its texels; the texture it builds into, read with `engine-content info`.

Everything is under `D:\workspace\game_engine_local\flythrough\diagnosis-2026-09-25\`: `markers\` (before, terrain fix, both fixes), `lod-cut\` (where the albedo moved between the two cuts, before and after), `sheets\move-b-albedo-before-top-after-bottom.png`, `closeup\` (scene, path and captures), `helmet\`, and the scripts (`Vis.cs` and `Glb.cs` are the analysis, loaded by `vis.ps1`).

## (2): the ground — a material voted per cluster

**Cause.** The terrain carried four materials — sand, ridge rock (the grey), basin sand and the basin's floor (the green) — and drew each cluster with the one most of its vertices were (`terrain_majority_material`). A cluster of the LOD DAG is a patch whose area roughly doubles with each level, and a coarse cluster votes over a larger patch than its children did, so near every border the vote comes out differently at different levels. As the camera moves, the cut moves, and the borders of the rock and the green floor move with it by whole clusters: a staircase outline that re-forms at every change of the cut, which is exactly "patches shifting at position thresholds". The [first session](first-interactive-session-2026-09-24.md) found the terrain's patches came from the scene's merge scrambling the material map and fixed that; the map it fixed was still a map from *cluster* to material, which is the part this session caught.

**Evidence.** The same camera, two cuts (`--lod 1` against `--lod 2`), the terrain's pixels in the albedo view:

| Marker | terrain pixels | cluster changed | colour moved > 8/255, before | after |
|---|---|---|---|---|
| 1 | 2,312,012 | 682,919 | 80,174 (3.47%) | 247 (0.011%) |
| 2 | 2,346,359 | 525,269 | 61,472 (2.62%) | 216 (0.009%) |
| 3 | 2,330,537 | 919,735 | 99,226 (4.26%) | 301 (0.013%) |
| 4 | 2,347,910 | 522,156 | 62,044 (2.64%) | 253 (0.011%) |
| 5 | 1,948,125 | 316,193 | 79,896 (4.10%) | 375 (0.019%) |
| 6 | 1,969,784 | 346,200 | 80,756 (4.10%) | 534 (0.027%) |

Before the fix every pixel whose colour moved was on a changed cluster and none on an unchanged one: 11–25% of the pixels whose cluster changed changed colour. **The cluster columns are the same numbers after the fix** — the change moves no geometry and no cut, only where the colour comes from. Along the moves, the albedo that changed by more than 8 between consecutive captures (the camera moving 1–2 m between them, so edges move too) fell from 59,000–99,000 pixels a pair to 23,000–34,000 on average per move; the rest is the ruins' and the ridges' own edges crossing pixels, which any moving picture has. `sheets\move-b-albedo-before-top-after-bottom.png` is four consecutive captures of move B: the staircase border between sand and basin sand re-forming at every frame above, one smooth gradient below.

**Other causes, ruled out.** Every pixel whose colour moved under a cut change was on a changed cluster, so it was not a texture's mip selection or a sampler's address mode (the terrain had no texture); the albedo view has no shadow in it, so not the cascades' edges; and occlusion culling does not change the picture ([renderer](../subsystems/renderer.md#invariants)), which its own test holds.

**Fix.** The terrain is one material now, and its colour and roughness are two maps over its UVs, baked from the same features (`terrain_surface`: the four materials' values, blended across a band of 0.1 of the feature's weight around each threshold, ten to twenty metres on this desert; `bake_terrain_maps`, one texel per grid cell, 2048² here). Every level of the DAG keeps source vertices and their UVs, and the UV is an affine function of the ground's x and z, so a point of the ground is one colour whichever cluster draws it. The maps are PNG bytes in the terrain's images and go through the texture step like a GLB's embedded textures: mipmapped BC7, built once into the derived-data root (the bake and the PNG encode 646 ms and the texture build 410 ms for both, on a cold cache in `msvc-release`, beside other agents' work — upper bounds). `k_terrain_version` is 5, so an old entry is not found and the terrain is clustered again once (43 s for this one, the bake included). **The picture changes on purpose**: the borders are soft instead of a staircase, and they sit where the features put them rather than where a cluster's vote did (`markers\terrain-fix-*`).

**The test** (`flythrough_tests.cpp`, "the ground's colour does not move when the LOD cut does") draws this session's terrain, cropped to 1.28 km at 5 m a cell, at the owner's six marker cameras from two cuts (1 and 3 pixels) at 480×270 and compares the albedo where both show the terrain. Measured: the cut changes under 8.7% of the terrain's pixels; with the per-cluster vote put back the colour moved on 1.32% of them (15% of those whose cluster changed, 2.3% at the worst camera), with the maps on 0.013% (0.15%, 0.037%). It holds the maps to under 1.5% of the changed pixels and 0.2% of the worst camera's terrain, a decade from each side. The merge test that used the terrain as its multi-material fixture now uses a glTF of two primitives, since the terrain has one material.

## (1): the ruins — a mirror's sheen on rough stone

**Where it was not.** The three usual suspects, in the order they were checked:

- **(a) The channel mapping.** glTF packs roughness in G and metallic in B. The kit's `block_orm` goes to a **BC7** linear texture (`options_for_roles`: BC5 would have kept R and G and lost the metallic), which keeps all four channels in place; the resolve reads `mr.g` as roughness and `mr.b` as metallic and multiplies the material's factors (`sample_material_grad`, `material.slang`); the kit's materials have no `metallicFactor` and no `roughnessFactor`, so both factors are glTF's 1. The attributes test holds exactly this against the CPU reference with a texture whose G and B differ per texel. Right.
- **(b) The kit's ORM.** The member GLBs embed the atlas's images as they are (`export_image_format='AUTO'`; the embedded `block_orm` is the atlas's file byte for byte, 2,904,083 bytes), and they are **JPEG** at quality 92, written by the atlas script with R = 1, G = roughness and B = 0. Over its 4096² texels: **R mean 0.996, G mean 0.763** (1st percentile 0.51, median 0.84, 99th 0.90), **B mean 0.004** (99th percentile 0.039, maximum 0.18). The mortar's is R 0.997, G 0.614, B 0.0035. So JPEG's chroma subsampling and quantization do leave a little metallic where roughness changes sharply, up to 18% in a few texels, but it averages to 0.4%: rough, dielectric stone. Not it.
- **(c) The sampler and the mip chain.** The ORM builds into a full 13-level BC7 chain and is sampled with the triangle's own UV derivatives through the kit's `LINEAR_MIPMAP_LINEAR` sampler, like every other slot. Not it either, as the next experiment shows without reading the texture at all.

**The experiment that settled it.** The resolve built with **roughness forced to 1 and metallic to 0** for every material, everything else unchanged: the grazing close-up keeps its sheen. Over the section member's pixels the share whose blue exceeds their red — warm stone under a warm sun only turns blue where it reflects the sky — is 13.5% as drawn, **13.1% at roughness 1**, and 6.2% with the fix below; the blue channel's mean 73.8, 73.3 and 55.9 (`closeup\captures\before-shaded-*`, `old-ambient-m0-r1-*`, `after-shaded-*`). No value in the kit's map could have made those walls matte.

**Cause.** The resolve's sky term was `ambient * (albedo * (1 − metallic) + F_schlick(F0, n·v))`: the specular share of the sky weighted by Schlick's Fresnel at the view angle. That is a **mirror's** curve — it ignores roughness — and it climbs to 1 as n·v falls to 0: along a wall seen edge-on, over the chamfers and pits of a strong normal map, and at every texel whose mapped normal faces away from the camera (n·v clamped to 0 gives exactly 1). There a rough sandstone block reflected the whole sky, a white-blue sheen on the mortar and the block edges; the far sand at grazing angles got the same. A rough surface's lobe spreads over directions that mostly miss the eye, and integrated over the hemisphere it reflects a few hundredths of the environment at grazing, not all of it.

**Fix.** `brdf_env_specular` (`brdf.slang`): the GGX lobe's reflectance of a uniform environment, the split-sum approximation's second factor in Karis' analytic fit — F0 · a + b, both functions of roughness and n·v. Face on it is about F0 at any roughness; at grazing it rises to 1 for a mirror and stays near F0 for rough stone (0.036 at this kit's roughness of 0.76). The CPU reference (`brdf_reference.h`) carries the same fit, and the shading and attributes tests hold the GPU to it within 1 of 255. **The path tracer agrees with the resolve better for it** — its escaped rays see the same sky and it integrates the lobe properly — which is the evidence that the fit is the right model and not just a darker one: the reference test's four scenes went from **0.0206, 0.0218, 0.0281 and 0.0144 FLIP** to **0.0131, 0.0140, 0.0215 and 0.0034** (PSNR +5.2, +4.9, +2.4 and +16 dB), measured the same day on the same machine, and every scene of the reference corpus (`tools/ci/reference-compare.ps1`) by 23–32%: **the FlightHelmet** at `--orbit 22`, 640×480, against 1024 samples at three bounces, from **0.0168 to 0.0128** FLIP (32.9 to 34.5 dB), the heightfield 0.0348 to 0.0268, helmet-grid 0.0050 to 0.0037, thin-geometry 0.0053 to 0.0036, wide-frustum 0.0014 to 0.0010 and shredded-atlas 0.0575 to 0.0424 ([renderer](../subsystems/renderer.md#reference-renderer)). The helmet's own change is 0.0088 FLIP between the two pictures: the leather's and the wooden base's grazing sheen goes, and the goggles' glass, which is smooth, keeps its reflection (`helmet\before-after-reference.png`, before, after, reference). **The whole desert changes a little too**: sand at roughness 0.92 no longer takes a Fresnel share of the sky at grazing, so the ground is 6–7% darker in display terms in the close-ups (mean luma 177 to 167 face on, 177 to 164 along the wall) and less washed out towards the horizon (`markers\both-fixes-*`).

**What was not changed, and why.** The kit: its ORM is not the cause, so the generator was not touched and no member regenerated. The atlas script should still write its ORM and normal maps as **PNG** — JPEG's 4:2:0 chroma subsampling and quantization put up to 18% of metallic where roughness steps, and smears a normal map's x against its y — which is the kit author's call and costs a regeneration of every member; it is recorded here for the next kit build. A roughness view (`--view roughness`, `--view metallic`) was not added: the resolve's modes are `gfx::ResolveMode`, a `domain/gfx` public header another change is working in, and the forced-roughness build answered the question a view would have.

## Present pacing

- **Question:** the owner flew the ruins scene by hand on 2026-09-25 at 11520×2160 and reported that performance felt good; the session's frame log said otherwise — frame times bimodal, a median of 7.2 ms and a p95 of 22.6 ms, 2,785 of 5,676 frames over 20 ms, while the GPU needed 2.24 ms a frame and the CPU 0.36 ms. The first session's write-up had left the same alternation as "the present-pacing item" ([first session](first-interactive-session-2026-09-24.md#1-the-stutter-near-an-asset)). What sets the cadence, and what fixes it?
- **Date:** 2026-09-25. **Machine:** Intel i9-10980XE (18 cores / 36 threads), 64 GB, Windows 11 Pro; **GPU:** RTX 5090 (32 GB), driver 610.88, Vulkan presentation through NVIDIA's DXGI-layered present layer (`VK_LAYER_NV_present`). **Display:** NVIDIA Surround — the three 4K monitors are **one** Windows display, 11520×2160 at 82 Hz (12.2063 ms a refresh by the driver's own report), plus a 1920×1080 60 Hz monitor above them. **Build:** `msvc-release` of this branch.
- **Machine state:** every measured run below was taken under the GPU lock (`agent-pacing`) with the harness's samples in its summary; other processes used 7–52% of the CPU and the GPU was 0–40% busy at one end or the other of each run, so every run raised the WARNING and **every duration is an upper bound**. The comparisons rest on ratios and on the shape of distributions taken minutes apart on the same machine, which the load does not change: the frames that pair do so at 2 ms of GPU work in a 12.2 ms refresh.
- **Decision:** a display pacer, on by default for FIFO-family presentation (`--pace`, [apps](../subsystems/apps.md#pacing)); the frame log's waits, display times and drawn pose (`FrameRecord` version 6, `FlythroughSummary` version 7); the window's presentation choices as flags; and two driver behaviours written down in [gfx](../subsystems/gfx.md).

### The two recordings

Both sessions' frame logs (`D:\workspace\game_engine_local\flythrough\interactive-2026-09-24T2253-frames.jsonl` and `…2026-09-25T1226-frames.jsonl`) were read again frame by frame. `frame_ms[i]` is the length of iteration *i−1* of the loop.

| | 2026-09-24 (desert overlook, `--shadows rt`) | 2026-09-25 (ruins, `--shadows csm`) |
|---|---|---|
| frames | 17,716 | 5,676 |
| frame_ms median / p95 / p99 | 12.20 / 23.81 / 24.13 | 7.19 / 22.56 / 23.13 |
| frames over 20 ms | 8,689 | 2,785 |
| lag-1 autocorrelation of frame_ms | −0.994 | −0.947 |
| pairs from | frame 243 (3.0 s, after one 31.4 ms frame) | frame 22 (after the load's 78 and 52 ms frames) |
| the short iteration / the long / the pair | 1.37 / 23.03 / **24.41 ms** | 2.43 / 21.98 / **24.42 ms** |
| short iteration less the GPU time of the frame it waited for | 0.10 ms | 0.16 ms |
| breaks in the alternation after it set | 108 | 26 |
| ticks per pair (long + short), commonest | 0+6 (4,432), 1+5 (3,031), 0+5 (1,217) | 1+5 (1,632), 0+6 (757), 0+5 (390) |

**The period is exactly two refreshes**: 24.41–24.42 ms a pair, twice the driver's 12.206 ms, so the display was never missed — two frames were shown in every two refreshes. The CPU's share is the same in both halves of a pair (0.34–0.39 ms), and the ticks follow the waits (the frame after the long wait runs five or six ticks, the one after the short none or one). The one thing the short iteration tracks is **the GPU time of a frame**: it is that frame's GPU milliseconds plus 0.10–0.16 ms (correlation 0.33–0.49 against it; nothing else logged correlates). So in each pair the loop waited 22–23 ms for a vblank and then only for a GPU frame that had started at it. Both sessions settled into it after a hitch and never left: the first was even for its first 3 s, the second paired from its 22nd frame.

### Instruments

Each windowed frame now records where its time went ([apps](../subsystems/apps.md#pacing)): `wait_ms` (the frame slot, `begin_frame`), `acquire_ms`, `present_ms`, `pace_ms`, `submit_ms` (inside `cpu_ms`), the camera the frame drew and the simulated time it stands for (`pose_*`, added for [the strafing stutter](#the-strafing-stutter)), and, where the swapchain offers VK_EXT_present_timing, `shown_ms` and `latency_ms` from each present's first-pixel-out time; the summary gains a `presentation` block. `--windowed` replays the owner's recording **in the window**, at its recorded pace, with a frame log — the fair before and after — and every run below is that replay of today's recording (`…T1226-input.jsonl`), cut to 820 frames (10 s) or 1,640 (20 s) with `--frames`.

Two things the instruments found about the driver before they found anything about the loop, both in [gfx](../subsystems/gfx.md):

- **Reading display times blocks.** `vkGetPastPresentationTimingEXT` took 10.4–11.2 ms a call whatever its flags, so a loop that polled it every frame was paced by the poll (and looked like 12.2 ms frames with 11.9 ms of "CPU" in them). The driver's queue is now set 16,384 deep and read once, after the run.
- **Display times read early.** The only time domain on offer is the present stage's own; mapped onto our clock through `vkGetCalibratedTimestampsKHR`, a paced frame with 8.3 ms of work between sampling and presenting was reported shown 3.6 ms after sampling. The intervals between display times are exact; their offset from anything else is at least 5 ms short, so `latency_ms` is compared between runs, never read as a number. The differences below are the reliable part.

### Reproducing it, and separating the variables

A 1280×720 window on the same display — composed by DWM, since it does not cover the display — paced evenly with FIFO, with or without present timing and with two images or three; one frame in flight and FIFO-relaxed did not (10 s each):

| 1280×720, composed | frame p50 / p95 / p99 ms | >20 ms | present ms | display p5–p95 ms | sample → display (reported) |
|---|---|---|---|---|---|
| FIFO, 3 images, 2 in flight | 12.21 / 12.87 / 13.28 | 0 | 10.80 | 5.4–18.9 | 59.5 ms |
| the same without present timing | 12.21 / 12.30 / 12.42 | 2 | 10.68 | — | — |
| 1 frame in flight | 12.17 / 36.38 / 36.81 | 46 | 10.79 | 4.8–19.6 | 36.1 ms |
| 2 images | 12.20 / 12.95 / 13.58 | 0 | 10.78 | 5.2–19.3 | 59.5 ms |
| mailbox / immediate / FIFO-latest-ready | 0.7–0.8 (1,300 fps) | 0 | 0.10 | — | 2.2–2.8 ms |
| FIFO-relaxed | 12.19 / 23.77 / 24.43 | 133 | 10.85 | 4.7–19.9 | 59.6 ms |

The spanned window — the owner's 11520×2160 with its title bar pushed off the top of the display, and a `--borderless` one at the display's corner (approved by the owner for this; 10 s each) — reproduced the pairs, and **only when the swapchain did not ask for display times**:

| 11520×2160 | frame p50 / p95 / p99 ms | >20 ms of 790 | wait / present ms | display p5–p95 ms | sample → display (reported) |
|---|---|---|---|---|---|
| FIFO, no present timing (**the owner's configuration**) | 12.20 / 24.39 / 36.33 | 124 | 0.09 / 9.30 | — | — |
| the same, borderless | 12.20 / 24.36 / 36.19 | 117 | 0.06 / 9.43 | — | — |
| the same, three more runs (two of them with present ids and waits) | 12.20 / 24.3–24.4 / 36.2–36.3 | 113, 136, 131 | 0.06–0.07 / 9.3 | — | — |
| FIFO **with** present timing | 12.21 / 12.93 / 13.42 | 0 | 2.60 / 9.11 | 12.18–12.26 | 50.1 ms |
| the same, borderless / repeated | 12.20–12.21 / 12.9 / 13.4–13.9 | 0, 0 | 2.6 / 9.2 | 12.21 | 50.0–50.1 ms |
| 1 frame in flight (with timing) | 12.20 / 13.43 / 36.86 | 32 | 0.10 / 9.57 | 12.18–12.23 | 28.6 ms |
| 2 images (with timing) | 12.21 / 12.96 / 13.79 | 0 | 2.60 / 9.16 | 12.19–12.21 | 50.1 ms |
| mailbox (with timing) | 12.21 / 23.67 / 24.02 | 86 | 0.05 / 11.61 | 12.19–12.23 | 52.7 ms |
| FIFO-latest-ready (with timing) | 12.21 / 23.55 / 23.98 | 70 | 0.11 / 9.69 | 12.19–12.22 | 64.5 ms |
| **FIFO + the display pacer** (its first build; the same at depth 1) | 12.28 / 13.56 / 14.53 | 0 | 0.05 / 0.07 | 12.19–12.21 | 3.4 ms |
| the same, borderless | 12.30 / 13.54 / 14.46 | 0 | 0.05 / 0.07 | 12.19–12.23 | 3.5 ms |

What the instrumented paired runs show, frame by frame: in the even rhythm every present blocks 11.5 ms and nothing else waits; in the paired rhythm one iteration's present returns in 0.05–0.35 ms and the next waits **14.1 ms for its frame slot** and 9.3 ms in its present — 0.6 ms and 23.7 ms. Two throttles are in the loop, the driver's present queue and the frame slot, and the slot is gated by the display, because a frame's GPU work waits on its swapchain image and the image comes back when the display flips past it. In the even rhythm the present queue absorbs each refresh; in the paired one two images come back at one refresh, two GPU frames run back to back after it, and the slot and the present take turns absorbing two refreshes at once. Either is stable; a hitch moves the loop from the first to the second.

**The owner's configuration today — FIFO, three images, two frames in flight, no pacer, no present timing — is the pairing one.** Nothing else measured is: decorated or borderless made no difference, two images behave as three, one frame in flight misses refreshes outright (36 ms frames), and mailbox and FIFO-latest-ready, which a window covering the display is paced through like FIFO, pair all the same.

**The present-timing observation** (open: a signature, not a mechanism). A FIFO chain created with VK_EXT_present_timing never paired — five runs, 0–1 frames over 20 ms — and every FIFO chain created without it did — six runs, 113–243 — present ids and waits alone changing nothing; mailbox and FIFO-latest-ready paired with it as well. With timing, the frame-slot wait is 2.6 ms on every frame — the GPU time plus half a millisecond, i.e. each frame's image came back at its own refresh and its GPU work ran then — and it never showed two images coming back at one refresh. What NVIDIA's present layer does differently for a timed chain is not visible from here; the practical consequences are that a measurement of the loop as the owner flies it must not ask for display times (`--no-present-timing`), and that present timing is not a fix to rely on — it costs a blocking read to be of any use, and the loop it produces still queues 50 ms deep.

### The fix: sample when the last frame has been shown

`--pace display` (and `auto`, the new default for FIFO-family presentation) waits, before a frame polls its window and reads the clock, until the frame presented last has been shown (`vkWaitForPresent2KHR`). Each frame then starts one refresh after the one before on the display's own clock with one frame queued, and the slot, image and present waits all fall under 0.1 ms. Deeper work is handled by `view::DisplayPacer`: two missed refreshes within 32 frames send it one frame deeper for 240 frames, and it tries again with a doubling hold (see [apps](../subsystems/apps.md#pacing) for the rule and why misses are counted the way they are).

**Before and after, the owner's recording replayed in the spanned window, 20 s each** (the two "after" runs on the committed build but for one addition made after them, which none of them reached: two timed-out waits in a row stop the pacer waiting for 120 frames, for a window nobody can see; the first 30 frames of each run left out, so the load's own frames are not counted):

| frame_ms | before: the owner's configuration (`--pace off --no-present-timing`) | before, with present timing (`--pace off`) | after (default), run 1 | after (default), run 2 |
|---|---:|---:|---:|---:|
| 0–2 | 283 | 3 | 3 | 0 |
| 2–8 | 0 | 2 | 26 | 0 |
| 8–10 | 1 | 6 | 48 | 7 |
| 10–11 | 8 | 11 | 105 | 118 |
| 11–12 | 165 | 283 | 328 | 493 |
| 12–13 | 896 | 1,243 | 755 | 670 |
| 13–14 | 12 | 51 | 236 | 285 |
| 14–16 | 2 | 10 | 69 | 36 |
| 16–20 | 0 | 0 | 36 | 1 |
| 20–24 | 104 | 0 | 1 | 0 |
| 24–28 | 99 | 1 | 2 | 0 |
| ≥ 28 | 40 | 0 | 1 | 0 |
| **over 20 ms** | **243** | 1 | 4 | **0** |
| p50 / p95 / p99 ms | 12.20 / 24.39 / 36.18 | 12.21 / 12.94 / 13.59 | 12.21 / 14.41 / 18.13 | 12.18 / 13.65 / 14.17 |
| waits: slot / present ms (median) | 0.06 / 9.27 | 2.75 / 9.03 | 0.05 / 0.10 | 0.05 / 0.07 |
| shown at the refresh (12.21 ± 1 ms) | — | 1,602 of 1,603 | 1,497 of 1,601 | 1,608 of 1,609 |
| sample → display, reported (see above) | — | 50.0 ms | 12.6 ms | 3.5 ms |
| machine: others' CPU / GPU busy, start / end | 11 / 13 %, 4 / 0 % | 21 / 8 %, 14 / 25 % | 15 / 23 %, 20 / 27 % | 17 / 10 %, 20 / 26 % |

The paced frames sample on a clock 12.2 ms apart on average but not to the microsecond (run 2: 125 of 1,610 frames under 11 ms, 322 over 13): the present wait returns up to a millisecond and a half either side of its usual moment. The display saw none of that — every frame of run 2 reached it one refresh after the one before. **Run 1 began composed**: for its first 340 frames the display times show the compositor's pattern (a frame 18–21 ms after the last, the next 4–6 ms after it), something on the desktop kept DWM from handing the window the display, and the pacer, seeing waits come late, went a frame deeper for three of its four changes; from frame 340 the window was flipped directly and ran as run 2 did. It is kept because it is what a notification over the owner's window will do.

**With more work.** A frame with 6 ms of synthetic CPU work after sampling (8.3 ms with the GPU's; 10 s, a flag used for this run only and not committed) still made every refresh at depth 1 — 0 of 790 frames off the refresh, frame p99 14.1 ms — having gone one frame deeper for 240 frames after the load's hitches and come back; the unpaced loop with the same work did not pair in 10 s either (p99 13.4 ms, present 5.8 ms: the CPU's 6 ms leaves no room for two frames in one refresh) but kept its deep queue. So at 82 Hz a depth-1 frame has about a refresh for its work, and the reported 3.5 ms "latency" of the paced runs is the display times reading early: a frame that did 8.3 ms of work after sampling was reported shown 3.6 ms after sampling. **What the timestamps do measure is the difference**: pacing brought frames to the display 46.5 ms sooner after they sampled their input than the unpaced loop with present timing (50.0 against 3.5 ms reported), about 3.8 refreshes; since a paced frame needs at least its 8.3 ms, the unpaced loop's true sample-to-display time is 55–60 ms and the paced one's 8–13 ms, about one refresh.

**In a composed window** (1280×720, 10 s each) the same pacer lowers the reported sample-to-display time from 68 to 24 ms but samples on an uneven clock (p95 18.6 ms against the unpaced loop's 12.5), because DWM's present waits return anywhere from about 5 to 20 ms apart; it spends most of its time a frame deeper there. `--pace off` keeps the even sampling cadence at the cost of the latency. The owner's window is not composed.

### The strafing stutter

After flying the build with the visual fixes (`6050961`, still the old presentation), the owner reported one more thing: a slight stutter while strafing with A or D with the mouse held on an asset — "one frame in between each frame is in a previous position" — whenever lateral motion and a turn coincide, and never otherwise; the recording is `interactive-2026-09-25T1526-input.jsonl` (5,727 frames, paired from frame 16: 2,847 pairs of 2.72 ms and 21.69 ms, 24.42 ms a pair). The hypothesis was a pose that goes backwards in display time. To test it the frame log now carries the camera each frame drew (`pose_position`, `pose_yaw`, `pose_pitch`) and the simulated time it stands for (`pose_time`, the fraction of a tick past the tick before the last that `FlySession::camera_at` draws).

**The pose never went backwards**, in any of the four runs below: 0 frames whose `pose_time` was earlier than the frame before. What the old loop gets wrong is how far and in what order each part of the camera moves:

- **Uneven steps.** The frames of a pair start 0.6 ms and 23.7 ms apart, but each reads the clock after its frame-slot wait, so the cameras they draw stand **15 ms and 9 ms** apart, alternately, instead of 12.2 — and whenever the rhythm stumbles one frame lands barely after the other (3.4 ms, 0.4 ms). Replaying the new recording in the spanned window for 20 s (others' CPU 20 % and 11 %, GPU 14 % and 29 % busy at the ends of the unpaced run; 11 % and 17 %, 4 % and 1 % of the paced one): pose steps p5 3.7, median 12.2, p95 27.1 ms, with 90 of 1,609 frames (5.6%) advancing less than half a refresh — a near-repeat of the frame before. Under the pacer: p5 10.8, median 12.2, p95 13.6 ms, none under half a refresh, and each frame's pose step within 1.4 ms (p5–p95) of the display's own step.
- **A turn on every other frame, out of step with the strafe.** The window is polled once a loop iteration, and pointer motion is fed at the next tick; in the paired rhythm the polls are 24 ms apart, so the turn arrives on every other displayed frame while the strafe, which is keys held, advances every tick. A live session driven through the window's own event queue (`--inject-input`, a synthetic log of D held and 1 pixel of pointer a tick, 30 m/s, at 11520×2160, 5 s each) shows it frame by frame: unpaced, the strafe per displayed frame alternated 0.28 m and 0.46 m while the turn alternated 0.50° and 0.25° **in the opposite phase** — the frame that moved less turned more — so an asset held under the pointer is pushed one way and pulled back on alternate frames: the frame in its previous position. The turn missed what its own time step called for by 0.13° median, 0.56° p95. **Paced**: strafe 0.32–0.41 m a frame (p5–p95), turn within 0.014° median and 0.17° p95 of its step.

| Live strafe and turn, 11520×2160 | camera step p5 / p50 / p95 | strafe a frame p5 / p50 / p95 | turn off its step p50 / p95 / max | frame p95 | machine: others' CPU, GPU busy (start / end) |
|---|---|---|---|---|---|
| `--pace off --no-present-timing` (the old loop) | 8.78 / 12.15 / 15.55 ms | 0.016 / 0.364 / 0.467 m | 0.126 / 0.564 / 0.615° | 24.10 ms | 13 / 10 %, 21 / 1 % |
| default pacer | 10.92 / 12.21 / 13.51 ms | 0.317 / 0.366 / 0.405 m | 0.014 / 0.172 / 0.407° | 13.55 ms | 9 / 6 %, 25 / 29 % |

**What survives the pacer** is at most one tick of turn (0.126° at 1 pixel a tick): the pointer motion a frame reads enters the camera whole at one tick, while the position the frame draws is interpolated between the last two, so the two can disagree by up to a tick as the tick count per frame goes 3, 3, 2. Stamping pointer motion with the time the platform delivered it (SDL events carry one) rather than the tick of the poll would let it interpolate like the position does; it changes what a recording's ticks mean, so it is left as a follow-up rather than done here.

**The owner's recording carries the old rhythm**: its pointer events are stamped every five or six ticks, because that is how often the loop that recorded them polled. Replayed under the pacer its turns still land on every other frame (318 of 769 strafing frames turned by nothing, against 141 unpaced, the paced frames now lining up with the recording's two-frame beat), so a recording made before this change cannot show the fix; a session flown with the pacer records motion every refresh.

### What the owner should expect at 11520×2160

- **A steady 82 Hz with nothing waiting on presentation.** Every frame sampled one refresh after the last and shown one refresh after the last: frame time 12.2 ms median, p95 13.7–14.4 ms, p99 14.2–18.1 ms, 0–4 frames over 20 ms in 1,610 against 243 in the configuration he flew today. The title's number will read 12.2 ms and stop alternating with 1–2 ms.
- **The camera moves by a refresh a frame, and the mouse reaches every frame.** With the pairs the drawn camera stepped 15 ms and 9 ms of its time on alternate frames (now and then barely at all) and the window's input was read every 24 ms, so a turn landed on every other frame, out of step with a strafe; now every frame steps 11–13.5 ms and carries its own share of the turn. That is the strafing stutter he reported this afternoon ([below](#the-strafing-stutter)) and the half of the first session's near-object stutter interpolation could not reach.
- **About four refreshes less input lag**: 46.5 ms less from input to display by the display times, roughly 55–60 ms down to one refresh.
- **Room for real GPU work.** Frames with 8.3 ms of work still made every refresh; past about a refresh the pacer queues one frame more (one refresh more lag) rather than halving the frame rate, and tries to come back every few seconds.
- **The GPU's 2.15 ms is unchanged**; it is still idle five-sixths of every refresh, which is headroom, not waste, now that it no longer shows up as alternating frame times.
- **If something covers part of the window** (a notification, a window of another program), DWM composes it for as long as it is there, the display cadence goes uneven and the pacer drops a frame deeper, as run 1's first four seconds did. `--pace off` restores today's loop exactly.

### Windowed runs made

All under the GPU lock as `agent-pacing`, none while the owner was flying: **30 runs at 1280×720** on the Surround display's middle monitor (3 of 820 frames before the instruments were settled, 5 of 160–400 frames finding the timing poll and the time domain, 21 of 820 frames — about 10 s — for the tables above, and one 5 s injected session trying the strafe probe), and **28 at 11520×2160**, approved by the owner beforehand: 12 of 820 frames (7 decorated, 5 borderless), 4 of 820 separating the present-timing effect, 4 of 1,640 (20 s) for the before and after, 2 of 820 with synthetic work, 2 of 1,640 with the final pacer, 2 of 1,640 replaying the strafing recording, and 2 injected strafe-and-turn sessions of 5 s. Their frame logs are in the session's scratch directory and not kept.

### What is not settled

- **Why a timed chain does not pair** (above): the driver's, and worth a line to NVIDIA if it matters again.
- **The display times' offset.** Latencies here are differences; an absolute one needs a second clock (a photodiode, or a driver whose present stage domain calibrates to the host's).
- **Composed windows.** The pacer's sampling there is only as even as DWM's present waits; pacing on a grid phase-locked to them would even it out at the cost of the milliseconds the grid sits behind the latest wait, and was not tried.
- **Pointer motion stamped when it happened**, not at the poll ([The strafing stutter](#the-strafing-stutter)): the last tick of turn the pacer leaves.
- **Displays with separate clocks.** The owner's three monitors are one Surround display with one clock. A window spanning two independent displays was not measured.
- **Exclusive fullscreen** (VK_EXT_full_screen_exclusive, offered by the driver) was not tried: the borderless window covering the display already gets the display flipped to it, and the pacer made the cadence exact there.

## Traced shadows on the surround (2026-09-25, evening)

The owner flew this session with `--shadows csm` because `--shadows rt` refused to start at 11520×2160 `surround3`: *engine-view: scene: the driver gave no cluster acceleration structure sizes*.

**Cause.** Not the frame's size: the **view count**. The ray tracing chain names every cluster it builds by its visible entry, as its base geometry index, and every per-frame set is created able to name any entry a frame could produce — the three views' first runs of the visible list and, with shadow casters, their run `k_caster_run` behind them: `3 × 3 × 3,699,274 − 1 = 33,293,465`. The RTX 5090 reports `maxClusterGeometryIndex` **16,777,215** (driver 610.88, the record's 24 bits), so `gfx::cluster_set_build_sizes` refused the capacity probe's limits before the driver was asked anything, and the renderer reported it as the driver's silence. Bisected offscreen under the GPU lock with this branch's parent: 640×360 `surround3` refused exactly as 11520×2160 did; 11520×2160 and 1920×1080 with one view (11,097,821 with casters) started and drew; and 11520×2160 `surround3 --no-shadow-casters` (11,097,821 without them) started and drew. The "640×360 works" report was a single view. The desert overlook never met it — 239,833 pairs are 2,158,496 on a surround with casters — and the ashlar ruins are fifteen times its pairs.

**Fix** ([renderer](../subsystems/renderer.md#the-ray-tracing-chains-index-space), [gfx](../subsystems/gfx.md)). `gfx::DeviceFeatures::cluster_max_geometry_index` carries the device's limit, and `renderer::resolve_settings` checks the scene's pairs in every view against it: the shadow casters go first (the shadows of surfaces facing away from the camera, which the capacity's rule also gives up first), and only if the drawn clusters alone do not fit do traced shadows become the cascaded maps and `--raster rt` the rasterizer — each with one warning naming the pairs, the views, the index asked for and the limit. Here the drawn clusters fit (11,097,821), so the owner's line starts with traced shadows and without casters:

```
warn  renderer shadow casters off reason="the scene's pairs in every view need more cluster geometry indices than the device has" pairs=3699274 views=3 geometry_index=33293465 device_limit=16777215
```

Nothing the frame builds comes near the capacity: the chain held 65,536 clusters (its first allocation, 549 MB, under a budget of 159,949 clusters) and no frame dropped a structure.

**Measured.** The session's recording (`…T1526-input.jsonl`) replayed offscreen, whole — 4,201 frames — at 11520×2160 `surround3`, a `msvc-release` build of this change, one run each, under the GPU lock with the owner's session locked and `--wait-quiet 120`, which found the machine quiet before both. Others' CPU 8.6% and 11.8% at the ends of the traced run, the GPU 1% and 0% busy with 10.1–10.3 GB of the card held by other tenants; 8.1% and 16.7%, 0% and 99% for the maps' run (the end sample is the run's own last frames). The harness still flagged both as upper bounds. A first take over the flight's first 1,640 frames, beside this change's debug build, gave a GPU frame of 5.07 and 2.60 ms and a chain of 3.14 ms; it is a different stretch of the flight, so its passes differ with the cut (the CLAS build 0.79 ms against 0.66) rather than with the load.

| | `--shadows rt` (no casters) | `--shadows csm` |
|---|---|---|
| GPU frame, median / p95 / p99 | **5.01 / 5.89 / 6.35 ms** | 2.61 / 2.88 / 2.99 ms |
| of it: the ray tracing chain | 2.94 / 3.46 / 3.60 | — |
| — the CLAS build · the bottom-level build | 0.66 / 1.07 · 0.22 / 0.26 | — |
| the resolve | 1.41 / 2.02 / 2.73 | 0.98 / 1.09 / 1.11 |
| the cull pass, median / p95 (with the cascades' runs under `csm`) | 0.18 / 0.19 | 0.32 / 0.36 |
| the maps' draw, median / p95 | — | 0.52 / 0.56 |
| clusters built a frame, median / p95 / max | 27,237 / 44,314 / 52,864 | — |
| the chain's capacity at the end (peak); resizes | 20,480 (65,536); 3 grows, 2 shrinks, no frame dropped a structure | — |
| device memory, this process | 2,030 MiB | 1,658 MiB |
| frame, median / p95 (offscreen loop) | 9.52 / 11.03 ms | 2.81 / 3.21 ms |

**The chain is 2.9 ms and most of it is not the builds.** The CLAS and bottom-level builds are 0.9 ms of it; the other 2.1 ms is the rest of the chain, which has no timer zone of its own: the three records passes, the copy of each bottom-level address into its top-level record, and the top-level build. The desert overlook's landmarks on the same surround spend 0.97 ms on the whole chain ([renderer](../subsystems/renderer.md#the-ray-tracing-chains-memory)), and what differs most between the two scenes is the **instance count** — 11,172 here against 90 — ahead of the pairs (11.1 million slots in every view against 0.7 million, which `emit_main` walks one thread each, cheap at any count). Two parts of the chain are serial in the instances and are the first suspects, unmeasured: `ranges_main` is **one thread** that walks every instance four times (the demand, the drawn clusters, the casters, the prefix sum and the bottom-level records), and the address copy is one `vkCmdCopyBuffer` of **11,172 regions of eight bytes**. A zone per pass would settle it. At 82 Hz (12.2 ms a refresh) either shadow fits; the maps are the cheaper one on this scene by 2.4 ms of GPU time.

## The ruins' traced shadows (2026-09-26)

- **Question:** with `--shadows rt` the ruins cast no shadow on the sand at all, while the cascaded maps throw long wall and rubble shadows across it — and the two looked as if they disagreed about which faces of a wall face the sun ([What remains](#what-remains), second item). Which path is right about the sun, why do the rays miss the ruins, is it old or new, and what fixes it?
- **Date:** 2026-09-26. **Machine:** the same i9-10980XE and RTX 5090, driver 610.88. **Build:** `msvc-release` of `e9284c7` and of the fix; `af1b071` for the bisect. **Reproduction:** `engine-view --scene fly-ashlar-100.json --ddc <main's> --replay-input …T1526-input.jsonl --width 3840 --height 2160 --offscreen --frames 900 --capture f900.png --view shadow|shaded --shadows rt|csm`, each under the GPU lock. Captures, the offset variants, the benchmarks and the scripts are in `D:\workspace\game_engine_local\flythrough\diagnosis-2026-09-26-rt-shadows\`.
- **Machine state:** pictures and pixel counts are what a busy machine does not change. The timings below were taken with `--wait-quiet 120`, which found the machine quiet before each run; other processes then held 4.5–25% of the CPU and 9.2–10.7 GB of the card at the ends of the runs, the card 5–8% busy at the starts (the 94–99% at some ends is the run's own last frames), the owner's session unlocked. By the harness's rule every duration is an upper bound.
- **Decision:** a shadow ray leaves its surface by one step of **that surface's own** 16-bit grid plus 2^-18 of the scene's reach, not by a thousandth of the scene's radius ([renderer](../subsystems/renderer.md#ray-traced)).

**Which path is right about the sun: both.** The cascade fit and the ray take the same `FrameLighting::sun`, a direction *towards* the light (`fit_shadow_cascades` passes it on as `ShadowMapParams::light_dir`, also towards the light; the resolve traces `params->sun.xyz` and tests `dot(normal, sun)` with it), and the shadow view's grey — a surface facing away, where nothing is asked — is decided by the one N·L test both implementations share. At frame 900 every one of the **541,498** pixels the traced shadow view paints grey is grey in the maps' view too; the maps' 624 more are penumbra values that happen to be the same byte. What looked like disagreement was **white patches** on the camera-facing, sun-facing-away sides of the walls under `rt`, where the maps have black: normal-mapped texels whose shading normal faces the sun on a face that does not. Such a texel is in its own wall's shadow, the maps say so, and the ray said otherwise because it started too far out.

**Why the rays missed.** The ray's offset along the geometric normal was `1e-3 × SceneData::radius`, fixed since the first traced shadows (`47a97be`, 2026-09-17): a few millimetres on the samples that picked it, **4.1 m** on this scene's 5 km desert (`render.load` gives its bounding sphere as 4,110.5 m about (−557, −2, 117), wider than the terrain's 2,560 m half-width because the scene's bounds are made of each instance's bounding sphere, `update_scene_bounds`). The kit's walls are at most 2.7 m tall, so every ray from the sand started above every wall, and every ray from a wall face started 4.1 m out and cleared the wall's top. A build with the offset set to 0.1 m for the whole frame threw every wall's and every block's shadow the maps throw. The ruins' structures were in the top-level structure all along, as `--raster rt` showed; nothing about masks, flags, transforms or the chain was involved.

**Old, not new.** The same frame at `af1b071`, before the ground-bounce commits and the CLAS fallback, gives the same picture: 332,097 pixels the maps shadow and the rays light (329,912 at `e9284c7`). Its grey differs from `e9284c7`'s on 38,530 pixels, which is `2a88800` flipping shading normals that lie behind their triangle; its shadows do not.

**The rule, and why both of its terms.** The rebuilt surface is off the mesh's grid, the structures are the float positions it was rounded from, half a step apart per axis — so one step of the receiver's grid, 7.8 cm on the terrain and 0.06 mm on a wall — and float arithmetic at up to 4.7 km from the origin, 2^-18 of the scene's reach, 1.8 cm here. The same frame's shadow view under each variant, counted against the maps' (which this change does not touch — their captures are byte-identical before and after):

| offset | maps black, rays white | maps white, rays black | what it is |
|---|---:|---:|---|
| a thousandth of the radius (before) | 329,912 | 11,964 | no ground shadow, lit patches on dark walls |
| none | 0 | 4,917,966 | acne everywhere |
| 2^-18 of the reach only (1.8 cm) | 0 | 2,324,214 | the terrain's rounding (up to 3.9 cm vertically) under the ray |
| one grid step only | 61 | 23,703 | the kit's blocks speckle: their 0.06 mm grid is finer than the float arithmetic 500 m out |
| one step and 2^-20 (4.5 mm) | 112 | 12,782 | some of the speckle left |
| **one step and 2^-18 (the rule)** | **376** | **6,069** | edges, dents in a block narrower than the maps' filter, crenellation tops |
| two steps and 2^-16 (23 cm on the terrain) | 19,739 | 121 | the contact shadows round the rubble's feet gone |
| 0.1 m everywhere | 613 | 2 | right here, wrong on a 2 cm prop or a 50 km scene |

The last row is the closest to the maps and is not the rule because it is a number chosen for this scene: what the offset has to clear is the receiver's grid and the scene's float precision, and those are what it is made of.

**Before and after.** `before\f900-rt-shadow.png` is the sand lit everywhere, the walls grey with white patches; `after\f900-rt-shadow.png` has the near wall's long shadow across the sand towards the camera, each block of rubble's shadow at its foot, and the walls' dark sides black where the maps have them black — `after\f900-rt-vs-csm-flips.png` marks the 6,445 pixels where the two still give opposite answers. The shaded views differ on 803,518 pixels, the shadows and the point lights' shadows that moved with them; the maps' shaded and shadow captures are byte-identical to before.

**What it costs.** Rays that start where the geometry is have to look at it: a ray that began 4.1 m above the terrain left its bounding boxes at once, and one that begins 10 cm above tests the clusters round its origin. The chain is unchanged; the resolve is not. The same builds, back to back:

| | before (`e9284c7`) | after | `--shadows csm` (after) |
|---|---|---|---|
| frame 900, 3840×2160, GPU frame (resolve) | 4.06 (0.56) ms | 3.88 (0.87) ms | 1.26 (0.40) ms, maps 0.46 |
| frames 880–919, median GPU frame (resolve) | 3.62 (0.56) ms | 4.01 (0.87) ms | 1.26 (0.40) ms |
| the whole recording, 11520×2160 `surround3`, GPU frame median / p95 / p99 | 5.24 / 6.18 / 6.55 ms | **6.62 / 7.83 / 8.70 ms** | 2.64 / 2.96 / 3.16 ms |
| — the resolve, median / p95 | 1.41 / 2.02 | **3.06 / 3.80** | 0.98 / 1.09 |
| — the ray tracing chain, median / p95 | 3.12 / 3.78 | 3.09 / 3.68 | — |
| frame, median / p95 (offscreen loop) | 10.23 / 11.56 ms | 11.58 / 13.33 ms | 2.91 / 3.54 ms |
| device memory, this process | 2,032 MiB | 2,032 MiB | 1,659 MiB |

Frame 900 alone moves with the chain, which varies frame to frame (3.19 ms before, 2.70 after on the same cut of 21,701 clusters); the forty frames around it are the fairer pair. Yesterday's take of the whole recording ([above](#traced-shadows-on-the-surround-2026-09-25-evening): 5.01 ms median rt, 2.61 csm) is 4% under today's run of the same unfixed build, which is the machine and not the code. **Correct traced shadows cost this scene 1.65 ms of resolve at the surround**, 1.4 ms of GPU frame at the median; at 82 Hz (12.2 ms) the frame still fits with room, and the maps stay 4 ms cheaper. Where it goes was not measured; the likeliest place, and the first to look at if it matters, is the terrain's own clusters round every ray's origin, since the rays from open sand are most of the pixels and nearly all of them escape.

**The test** (`shadow_map_tests.cpp`, "a shadow ray leaves the ground by the ground's own grid, not the scene's") puts the fixture's four casters on a ground that is one mesh 5 km across with its grid's rows 3 cm off its floats, in a scene 3.9 km in radius, traces it, and compares every interior ground pixel with a CPU ray from the lifted point the rule gives: 0 differ. With the old offset every shadowed pixel differs (297 to 903 per caster); with the float term alone 32,513 to 34,370 of the lit pixels go black.

**The reference corpus**, whose path tracer takes the same offset: `tools/ci/reference-compare.ps1` on the fixed `msvc-release` build (its machine-state spans flagged every scene as an upper bound, others at 12–32% of the CPU) gives 0.0131, 0.0242, 0.0034, 0.0389, 0.0034 and 0.0009 FLIP for the FlightHelmet, the heightfield, the helmet grid, the shredded atlas, thin geometry and the wide frustum, against 0.0132, 0.0235, 0.0037, 0.0390, 0.0035 and 0.0010 recorded after the ground bounce ([the third session](third-interactive-session-2026-09-25.md)); every threshold holds and none needed re-pinning. The heightfield's +0.0007 is the one move above noise, on the scene whose offset shrank most (from 1 cm to 0.35 mm).

**Found on the way, not fixed:** the resolve rebuilds a triangle with a vertex behind the camera at the wrong point (the fixture's ground scaled a hundredfold put a 12.5 m triangle under the camera, and both shadow paths shadowed 13,500 of its pixels wrongly). It is in [What remains](#what-remains) and in [renderer](../subsystems/renderer.md#ray-traced).

## What remains

- **Shadow casters on the ruins' surround.** The chain names a cluster by its visible entry, so a surround of this scene can name its drawn clusters and not its casters ([Traced shadows on the surround](#traced-shadows-on-the-surround-2026-09-25-evening)). Naming a hit by its dense record index, which the capacity bounds, with a record-to-entry table the emit pass writes, would keep them on any scene; it costs a dependent load in the ray pass and the path tracer ([gfx](../subsystems/gfx.md)).
- ~~**The ruins cast no traced shadow on the ground, and the two shadows disagree about the sun**~~ — **resolved 2026-09-26** ([The ruins' traced shadows](#the-ruins-traced-shadows-2026-09-26)): the shadow ray's offset was a thousandth of the scene's radius, 4.1 m here, and now is a grid step of the surface's own mesh; the two paths never disagreed about the sun. What was written here before the diagnosis: found while looking at the fixed surround, not caused by it, and not diagnosed. At frame 900 of the same recording, one view at 3840×2160, `--view shadow`: under `--shadows csm` the near wall and the rubble throw long shadows across the sand and their camera-facing sides are lit; under `--shadows rt` the sand around them is lit everywhere and the same sides are grey, facing away from the sun. Casters on and off give the same capture, and so does the three-view surround, so it is not the casters and not this change. `--raster rt` draws the ruins, so their structures are in the top-level structure the shadow rays trace. Either the two paths aim the sun differently or the ray path's shadow misses the kit's geometry; `renderer: mapped and ray-traced shadows agree away from the filter's edge` passes on its own scene, so what differs is something about this one — the scene file's sun, the kit's instances or the terrain beneath them. The captures are in this work's scratch directory and not kept; frame 900 of `…T1526-input.jsonl` reproduces them in a minute.
- **A triangle that reaches behind the camera is rebuilt at the wrong point** (found 2026-09-26 while writing the offset's test, [above](#the-ruins-traced-shadows-2026-09-26)): the resolve's screen-space barycentrics divide by a clip w that is negative for a vertex behind the eye, so both shadow paths, and the shading, read a wrong position on such a triangle. Homogeneous barycentrics where some w ≤ 0 would fix it without moving any other pixel ([renderer](../subsystems/renderer.md#ray-traced)). **Fixed 2026-10-03**: the error was the clamp of the screen barycentrics before the perspective correction, not the divide, and the resolve now takes homogeneous barycentrics everywhere ([gfx](../subsystems/gfx.md), "Where a pixel meets its triangle").
- **The chain's 2.1 ms outside the builds** on the same surround: a timer zone per records pass and for the address copy, then whatever it names — most likely what is serial in the scene's 11,172 instances (`ranges_main`'s single thread, the copy's one region an instance).
- **Normal-map filtering into roughness.** A normal map's mip chain is renormalized, so the variance a distant texel averages away is lost rather than moved into roughness (Toksvig, LEAN); a far normal-mapped wall is smoother than its near self. It was not what the owner saw — the sheen was at every distance and at roughness 1 too — but it is the next thing a far ruin's gloss would come from, and it is the texture pipeline's (`domain/texture`, the ORM's mips reading the normal map's).
- **The kit's JPEG maps** (above): PNG for the ORM and the normal maps at the next regeneration.
- **The terrain's maps at 4097.** One texel a cell makes 4096² maps for the largest terrain `k_terrain_max_size` allows, 21 MiB each built; a scene that big may want a coarser map, which is a one-line change to `terrain_map_side` and a version bump.
- **Present pacing**: fixed by the display pacer ([Present pacing](#present-pacing)); what it leaves open is that section's last list.
