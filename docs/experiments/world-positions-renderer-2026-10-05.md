# World positions in the renderer, stage 1 (2026-10-05)

[ADR-0053](../adr/0053-world-positions-are-f64-and-the-gpu-sees-none.md) decision 3, for instances and the camera: the GPU never holds or computes an absolute position. `gfx::InstanceDesc` holds a 64 m `WorldCell` and a 3x4 from mesh space to that cell (still 96 bytes), a frame's one origin is its eye (`gfx::FrameEye`), every view matrix is built with the eye at the origin, and every reader — the cull, the four rasterizers, the resolve, the ray visibility pass, the top-level records, the path tracer, the cascades and the lights — measures an instance from the eye with `instance_from_eye` (`shaders/scene.slang`, `engine::relative` operation for operation). [gfx](../subsystems/gfx.md#the-frames-origin) and [renderer](../subsystems/renderer.md#the-frames-origin) have the design; this page has the numbers. The ground's tiles are unchanged (stage 2).

## The machine

RTX 5090, Windows 11, `msvc-release`. The "before" build is `main` at `30c4ad70`, built from an exported copy of the tree; the "after" build is this branch. Every run under `tools/gpu-lock.ps1 run` with `--wait-quiet 60`.

## Cost

Offscreen `engine-view --benchmark`, 11520×2160 `--views surround3`, `--repeat 1 --warmup 60 --wait-quiet 60`: the erg's `walk-path.json` (1,201 frames, `--terrain-rings`) and the endless desert's `camera-path.json` (7,201 frames). GPU milliseconds at the median of the frames, per pass, before → after.

| Run | cull | hw | shadow (maps) | rt (structures) | resolve | total |
|---|---|---|---|---|---|---|
| erg, `csm` | 0.167 → 0.171 | 0.279 → 0.280 | 0.436 → 0.442 | — | 2.098 → 2.122 | 3.530 → 3.565 |
| erg, `rt` | 0.094 → 0.096 | 0.286 → 0.286 | — | 0.606 → 0.605 | 2.315 → 2.328 | 3.441 → 3.458 |
| endless, `csm` | 0.155 → 0.160 | 0.416 → 0.424 | 0.474 → 0.497 | — | 1.733 → 1.784 | 4.215 → 4.428 |
| endless, `rt` | 0.064 → 0.067 | 0.314 → 0.319 | — | 0.943 → 0.945 | 1.849 → 1.860 | 4.272 → 4.303 |

**Machine state.** No run but one reported a quiet machine (`quiet` true only for the before `endless rt`): other agents' builds and this branch's own were on the CPU, 5 to 17% at a run's start and up to 100% at the end of one (before, `endless csm`); the GPU was at 0% at every run's start and other processes held 10.6 GB of the card's 32. The GPU spans are the passes' own timers, which the CPU's load does not enter, but each figure is one run of the path, so a difference here is an upper bound.

**What it says.** The cull pass pays 2 to 4% (one `instance_from_eye` a pair: three subtractions and a select per axis, and the record's cell), the rasterizers 0 to 2%, the resolve 0.5 to 1.2% on the erg (its triangle fetch measures the instance from the eye), and the top-level dispatch's extra 48 bytes a record do not show in `rt`. The endless desert's `csm` total moved by 5%, more than the passes listed add up to (0.09 of its 0.21 ms); that run's p95 moved more than its median and it was not repeated, so it is reported and not explained. The frame thread's CPU did not grow beyond the runs' own spread (erg `csm` frame 3.57 → 3.64 ms at the median, erg `rt` 3.44 → 3.40).

## What moved by the origin

Three scenes by the origin, offscreen at 1920×1080, before and after with the same flags, compared byte by byte (RGB, of 255):

| Scene | Pixels that differ | Of those, by one level | Worst |
|---|---|---|---|
| the erg at the walk path's first camera, `--shadows csm`, `--time-of-day 16.5` | 429,594 (20.7%) | 428,466 | 16 (one pixel) |
| the dunes (`desert-dunes`), `--shadows rt` | 2 | 0 | 35 |
| the procedural heightfield (no scene), `--shadows rt` | 296 (0.014%) | 287 | 85 (one pixel) |

Everything a pass computes now goes through `instance_from_eye` and a view matrix with the eye at the origin, where it went through an absolute world matrix and a view matrix carrying the eye's translation: the same numbers rounded in a different order. On the dunes and the heightfield that is a handful of pixels on triangle edges, where coverage or a depth tie went the other way by a rounding (the worst, 35 and 85, are single silhouette pixels). On the erg it is a fifth of the picture by one level, from three things that changed by design: the sand's detail is evaluated at the frame corner measured from the eye in f64 and rounded once, rather than at the float eye's exact difference from it, which moves the ripples by up to 30 µm and their shading by a level; the cascades' centres are snapped from the eye's cell's corner instead of the world's origin, which moves every cascade by a fraction of a texel and the shadows' filtered edges with it (the pixels off by 2 to 16 lie along shadow edges); and the rays' float offset is sized from the eye rather than the origin.

## The translation suite

`systems/renderer/tests/world_translation_tests.cpp`, `msvc-debug`, the RTX 5090. Rigid cubes on a slab at whole 1024ths of a metre, 192×128, drawn by the origin and moved by 6,548, 156,250 and 1,562,500 cells, in two layouts (the eye over the cubes; the eye 1/1024 m short of the cell edge at x = 64 with cubes either side of it). Ids, depth and colour compared word for word:

| Path | 6,548 cells (419 km) | 156,250 (10,000 km) | 1,562,500 (1e8 m) |
|---|---|---|---|
| mesh shaders | identical | identical | identical |
| software rasterizer | identical | identical | identical |
| vertex path, indexed | identical | identical | identical |
| vertex path, capacity draw (no `geometryShader`) | identical | identical | identical |
| ray visibility | identical | identical | identical |
| mesh, traced shadows | identical | identical | identical |
| mesh, cascaded maps | identical | identical | identical |

Both layouts, every move: 0 colour bytes, 0 id words, 0 depths differ. And twelve frames with the eye stepped 1/1024 m at a time, 10,000 km out against the same steps by the origin, on the mesh path and with the cascaded maps: 0 differing bytes or words over the twelve, and each of the eleven steps moved the picture (a float32 eye there would not have moved). Under it, `domain/gfx/tests/world_eye_tests.cpp` holds `instance_from_eye` to `engine::relative` to the bit over 483 cases (instances and eyes from the origin to 1e8 m, 5 cm to 80 km apart, astride a cell's edge, a local that rounds to 64.0f) and holds `instance_from_eye` and `instance_point` to the same bits with each case moved by those three numbers of cells: no mismatch on the RTX 5090.

**Not measured in this stage.** The ground detail tests' far case (`domain/gfx/tests/ground_detail_tests.cpp`, "points clamped onto their triangle") still draws its sand as world-space vertices under an identity instance, so it reads what it read; a version with the sand as a mesh instance placed far out, which should fall to the origin's count, is not written yet. (Written in stage 3: [below](#the-far-out-sand-as-a-mesh-instance).)

## Stage 2 (picture-2): the cascades snap to the world

Stage 1 anchored the cascades' texel snapping at the corner of the eye's 64 m cell, which kept the translation suite's maps byte-identical and made every cascade take a sub-texel step whenever the eye crossed into another cell. `fit_shadow_cascades` now snaps in the world's light space in f64 (`snap_in_world`) and expresses the snapped centre relative to the eye afterwards ([renderer](../subsystems/renderer.md#cascaded-shadow-maps)).

**The test that matters** (`renderer: a cascade's texels hold their place in the world as the eye crosses a cell`, CPU): the eye stepped a 1024th of a metre at a time, twelve steps, across the corner of a cell (x = 64 and z = 128 at once), four cascades of 1.17 cm to 2.09 m texels, by the origin and 156,250 cells (10,000 km) out. How far each cascade's grid moved against the world, in texels, at the worst step:

| Anchor | by the origin | 10,000 km out |
|---|---|---|
| the eye's cell's corner (stage 1) | — | 0.46, 0.14, 0.46, 0.48 |
| the world, in f64 (now) | 2.8e-5, 1.6e-5, 3.2e-5, 3.1e-5 | 3.4e-5, 1.7e-5, 2.8e-5, 3.6e-5 |

The remaining hundred-thousandths of a texel are the frame-space centre's float32 rounding. (The stage-1 row by the origin was not printed: the test stops at the first failing site's messages; the run with the old anchor failed 64 assertions over both sites.)

**The translation suite's cascaded case** (`world_translation_tests.cpp`, RTX 5090): ids and depth still byte-identical at every move; colour byte-identical outside a band two pixels either side of every shadow edge — 1,302 to 2,211 of the 24,576 pixels, with 93 to 272 shadowed pixels and every lit one compared outside it, and 0 bytes differing there at 6,548, 156,250 and 1,562,500 cells. Inside the band 173 to 266 colour bytes differ, the shadows' filtered edges falling on the texel grid differently. The twelve millimetre steps 10,000 km out against the origin's: 0 bytes outside the band over the twelve frames.

## Stage 2 (picture-2): the ground's tiles at their corners

A terrain level's chunks — the world's tile levels, the far levels and a scene terrain's ring set — are instances at their lattice corners, with their vertices metres from them, and the pool's terrain stage works in whole millimetres from the corner ([renderer](../subsystems/renderer.md#the-grounds-tiles-are-placed-at-their-corners)). Same machine; "before" is `main` at `97d3bf4f` (stage 1 merged) built from an exported copy, "after" is the branch; `msvc-release`; every run under `tools/gpu-lock.ps1 run`.

### The owner's spot

The endless desert at (−419070, 80.24, −66781.11) looking along the sand (`owner-420km.json`, the survey's pose), offscreen 3840×2160, `--time-of-day 10 --shadows rt`, 90 frames. Before against after: 214,740 pixels differ (2.6%), 173,454 of them by one level; 49 blocks of 4×4 pixels differ by more than 20 levels, and they are of two kinds. A handful are silhouette pixels on the far ridges. The rest are **specks the change removed**: before, a dotted row of seven orange-brown specks on the sand left of centre (about x 169–219, y 1,016–1,022) and an orange dash (about x 1,087–1,092, y 666), the colour of the shading under the sand showing through a pin-hole between triangles; after, plain sand. A faint dashed diagonal line in the lower left (a border between two levels' tiles, a shading step of a level or two) is in both pictures and was not changed. The `--view normals` capture differs in 3.7% of pixels (151,654 by one level), the `--view tri` capture in 77% — its colours are the clusters' ids, and a tile's DAG is built from positions relative to its corner now, so its clusters are other clusters — and the far look (`owner-420km-far.json`, 150 m up) in 14.5%, nearly all by one or two levels.

**Not measured: the resolve's clamp count at the owner's spot.** engine-view has no counter for the points the resolve moves onto a triangle's edge (that count is the CPU mirror's, in `ground_detail_tests.cpp`, on its own quad of sand), and the test with a mesh instance placed far out that should read the origin's count was not written in this stage either.

### What moved by the origin

The same three scenes stage 1 captured, and two with tiles and rings, before against after (1920×1080, RGB of 255):

| Scene | Pixels that differ | Worst |
|---|---|---|
| the erg, `--shadows csm` (the cascades now snap to the world) | 1,963 (0.09%), 843 by one level | 12 |
| the dunes, `--shadows rt` | 0 | — |
| the procedural heightfield, `--shadows rt` | 0 | — |
| the erg with `--terrain-rings`, `--shadows rt` (the ring set's chunks at their corners) | 249 (0.012%), all by one level | 1 |
| the endless desert at its path's start (−2,000, 90, 0), `--shadows rt` (world tiles and far levels) | 104,285 (5.0%), 102,964 by one level | 4 |

The erg's are shadow edges whose filtered texels moved by a fraction of a texel against the scene (the grid's anchor is the world's origin now, not the eye's cell's corner). The tiles' are rounding: a lattice point reaches the frame through its tile's corner, and a tile's DAG is simplified from positions relative to the corner, which round differently from world coordinates.

### Cost

The endless desert's flight (`camera-path.json`, 7,201 frames), 11520×2160 `--views surround3`, `--repeat 1 --warmup 60 --wait-quiet 60`, the before and after runs interleaved (before rt, after rt, before csm, after csm, then csm again for both). GPU milliseconds at the median of the frames:

| Run | cull | deform | hw | shadow (maps) | rt (structures) | resolve | total |
|---|---|---|---|---|---|---|---|
| `rt` | 0.068 → 0.065 | 0.038 → 0.039 | 0.313 → 0.312 | — | 0.946 → 0.944 | 1.860 → 1.861 | 4.295 → 4.363 |
| `csm` | 0.129 → 0.130 | 0.054 → 0.055 | 0.293 → 0.291 | 0.475 → 0.481 | — | 1.741 → 1.739 | 4.045 → 4.124 |
| `csm`, again | 0.130 → 0.131 | 0.055 → 0.055 | 0.305 → 0.291 | 0.477 → 0.482 | — | 1.742 → 1.739 | 4.108 → 4.122 |

The frame thread's CPU (`FrameStats::cpu`, median / p95 / p99 ms): `rt` 0.407 / 0.629 / 0.931 → 0.418 / 0.597 / 0.950; `csm` 0.439 / 0.631 / 1.061 → 0.456 / 0.633 / 1.080; `csm` again 0.441 / 0.710 / 1.067 → 0.457 / 0.595 / 1.067. Its terrain share rose from 0.009 to 0.013 ms at the median.

**Machine state.** The first four runs reported a quiet machine (`quiet` true; other processes 1.7–9.1% of the CPU at a run's start or end, the GPU 0–1%); both `csm` repeats did not (`quiet` false: 22% and 27% GPU at their ends, from another session). Nothing of this branch was building during any run.

**What it says.** The pool pass's terrain stage pays for its integer corner arithmetic in the `deform` span: 1 to 4%, a few microseconds. The cascades' maps cost about 1% more (0.005 ms), within what a different snap of the same casters moves. The passes listed add up to under ±0.01 ms per run, and the totals moved by +0.07 (`rt`), +0.08 and +0.01 ms (`csm`): the two `csm` totals before differ from each other by 0.06 ms on the same binary, which is the size of the change, so a difference of that size here is not a cost. The frame thread's CPU rose by 0.01–0.02 ms at the median, 4 µs of it the terrain's share; its p95 and p99 did not move beyond the runs' own spread.

**Stage 1's endless `csm` total.** Stage 1 measured it at 4.215 ms before and 4.428 after (+5%), in one run each on a busy machine. Its "after" is this page's "before": 4.045 and 4.108 ms in two runs here, the first quiet — below stage 1's own "before". The 5% was the machine and not the change. (The pre-stage-1 tree, `30c4ad70`, was not rebuilt for a same-session comparison.)

### The translation suite's terrain

`world translation: the world's tiles moved by whole cells with their ground draw the same bytes` (`terrain_tiles_gpu_tests.cpp`, `msvc-debug`, RTX 5090): small world tiles (8 m, rings of 50 cm, 1 m and 2 m) with and without three far levels, over a ground of the test's own whose heights are a function of the lattice point measured from its own origin, moved with the camera by 6,548, 156,250 and 1,562,500 cells, at the finest cut. Mesh, software, vertex indexed, vertex capacity and ray visibility, both layouts, every move: **0 id words, 0 depths, 0 normal bytes and 0 colour bytes differ**. At the default cut about 10,000 of the 24,576 depths differed in a first version of the test: the tiles' UVs are in the scene grid's frame, so a tile 419 km out has other UVs than its twin, and the DAG's simplification weighs them — content, not precision; the finest cut is the lattice's own triangles.

Two existing tests that compared tiles drawn two ways were held to the bit and are now held to 16 float steps of depth, colour and normals still to the bit: the erg from tiles against the grid (6,690 of 156,172 pixels differ in some bit of depth, 11 by more than 16 steps, none in colour) and far tiles of four cells against six (0, 4, 1 and 15 pixels differ in some bit of depth in the four views, none past 16 steps). In both the same lattice point reaches the frame through two different corners.

## Stage 3 (picture-3): the files and the wire

The scene file's `Instance.translation`, a camera path's `CameraKey.position`/`target`, `RuinSite.origin`, and the protocol's `RenderCamera.position`/`target` and `RenderSceneInfo.center` are `worldpos` (f64), read without a float between the file and the frame's eye, and the camera path is sampled in f64 ([renderer](../subsystems/renderer.md#positions-in-the-files-and-on-the-wire)). Same machine; "before" is `main` at `39c43173` built from an exported copy (`msvc-release`, engine-view only), "after" is the branch.

The "after" captures were run twice, by picture-3 in its worktree and by picture-3b in its own from the same patch, and are byte-identical to each other.

### What moved by the origin

**The camera path's samples.** The erg's walk (`content/test-scenes/desert-erg/walk-path.json`: four keys on the ground at whole metres round x = −1,400 m, a cubic over 20 s, 1,201 frames at 60 fps) sampled the old way — keys as float32s, the Hermite in float32 — and the new way, emulated operation for operation in x and z (y holds the ground under a grounded key, which the emulation cannot evaluate): the eye moved in 1,136 of the 1,201 frames, by 0.046 mm on average and **0.22 mm at most** (frame 134), and the look-at point by at most 0.22 mm. A float's step at 1.4 km is 0.12 mm, so this is the float32 interpolation's rounding and nothing else. On a key the eye moves only in y, where 1.65 and the ground's height are now added in f64: a few micrometres. No instance in the erg is grounded; the endless desert's one grounded instance stands on its height at its whole millimetre, which by the origin is the height it had.

**The pictures** (`engine-view --offscreen`, 1920×1080; `pngdiff.ps1`, the largest channel difference per pixel):

| Picture | Pixels that differ | By 1 of 255 | Worst |
|---|---|---|---|
| erg, `csm`, frame 0 (the first key) | 3,381 (0.16%) | 3,369 | 8 |
| erg walk, `rt`, frame 299 | 46,158 (2.23%) | 46,125 | 114 |
| frame 599 | 55,941 (2.70%) | 55,925 | 81 |
| frame 899 | 18,387 (0.89%) | 18,385 | 3 |
| frame 1199 | 173,178 (8.35%) | 172,824 | 97 |
| frame 1200 (the last key) | 17,633 (0.85%) | 17,614 | 71 |
| erg on rings, `rt`, frame 2 | 1,288 (0.06%) | 1,282 | 21 |
| endless desert, `rt`, frame 2 | 268 (0.01%) | 268 | 1 |
| dunes, `rt`, orbit camera | 0 | | |
| heightfield, `rt`, orbit camera | 0 | | |

The two scenes without a path draw the same bytes, so nothing but the camera moved. With one, a sub-millimetre move of the eye shifts every pixel's sample on the sand by a fraction of the ripples' and grains' detail, which moves a few percent of the pixels by one level; the handful past a few levels (one to thirty a frame) are pixels a triangle's edge or a crest crossed. The desert overlook was not captured either time: its scene names the Khronos samples, which these worktrees have not fetched.

**The flythrough's numbers** (`--benchmark`, `--terrain-rings`, 1920×1080, one repeat after 30 warm-up frames, `--wait-quiet 60`; machine state as recorded at the run's start and end):

| | `rt` before | `rt` after | `csm` before | `csm` after |
|---|---|---|---|---|
| frames whose visible pairs differ | | 9 of 1,201, by 1 | | 28 of 1,201, by up to 5 |
| visible pairs, all frames | 2,287,238 | 2,287,241 | 1,466,968 | 1,466,992 |
| shadow pairs | | no frame differs | | 4 frames differ, by up to 18 (0.12%) |
| LOD levels | | no frame differs | | no frame differs |
| GPU total, median / p95 / p99 ms | 0.726 / 1.055 / 1.214 | 0.730 / 1.178 / 1.453 | 0.693 / 0.992 / 1.089 | 0.693 / 1.043 / 1.178 |
| others' CPU start → end; GPU busy at end | 14% → 19%; 5% | 9% → 14%; 9% | 16% → 16%; 47% | 4% → 10%; 97% |

The counts that are a function of the camera moved in under 3% of frames, by a pair or a few, as a 0.2 mm move of the eye moves a cluster across a LOD or frustum boundary now and then; no LOD level changed. The change adds a few dozen f64 operations a frame on the CPU and nothing on the GPU, and the medians agree to 0.6%. The tails are higher after, in this run and in a second one taken while a Linux container build held the CPU (`rt` 0.730 / 1.144 / 1.329 ms); the before and after runs are two hours apart on a shared GPU, so an A/B settled it: the two builds alternately, `rt`, each under the lock with `--wait-quiet 60` (others' CPU 9–18%, the GPU 8–97% busy at a run's end):

| Run | GPU total median / p95 / p99, ms |
|---|---|
| before, 1st | 0.725 / 1.221 / 1.516 |
| after, 1st | 0.723 / 1.211 / 1.531 |
| before, 2nd | 0.724 / 1.232 / 1.614 |
| after, 2nd | 0.727 / 1.235 / 1.634 |

Medians within 0.4%, p95 within 1%, p99 within 1.3%, either way round: the change costs nothing measurable, and the tails of the first runs were the hour's.

### The far-out sand as a mesh instance

`ground_detail_tests.cpp`, "sand placed far out as a mesh instance clamps what its origin twin does" (`msvc-debug`, RTX 5090, 36 s with the lock): the erg's sand drawn as the renderer draws a mesh — a quad metres from its site under one instance at the site's cell and local, every view at its own eye — at 420 km, 10,000 km and 1e8 m astride a cell's edge, three looks each, and again at each site's twin by the origin.

| Site | Points clamped, as a mesh instance (far / twin / the origin's own site) | As world-space vertices under an identity instance |
|---|---|---|
| 420 km, along the sand / at the feet / at a millimetre | 0 / 0 / 0 in each look | 45 / 138 / 44 |
| 10,000 km | 0 / 0 / 0 in each look | 413 / 5 / 18 |
| 1e8 m, astride a cell's edge | 0 / 0 / 0 in each look | no pixel inside the edge margin (a float's step there is 8 m) |

Every far view's visibility buffer hashes to its twin's, word for word; the far views, shaded, measure 1 of 255 on the picture and at most 2 on the detail view, the origin's tolerances; 11,720 to 25,600 pixels compared a view, none missing.

### The translation suite from a file

`apps/engine_cli/tests/render_tests.cpp`, "a scene file and its path written 10,000 km out draw the origin's bytes" (`msvc-debug`, RTX 5090, 235 s): a slab and 28 cubes and a smooth three-key camera path, written by the origin and again 156,250 cells out along x and back along z by their own numbers. engine-view flying the path (frames 3, 7, 11, 15 and 16; colour, ids and depth): all 15 files byte for byte. `render.capture` at two cameras named on the wire (the last key, and a point between keys a 1024th of a metre off a whole metre): colour, ids and depth byte for byte; `RenderSceneInfo.center` of the far scene is the origin's plus exactly the cells; and engine-view's last frame is the host's picture of the last key, byte for byte, at both sites.

## Stage 4 (picture-4): a tile's UVs from its corner

A world tile's UVs were in the scene grid's frame — `(lattice point − grid corner) / side`, in the thousands 420 km out — and the cluster DAG's simplification weighs UVs, so a tile's level-of-detail tree depended on where it stood. They are measured from the tile's corner now, its instance carries `gfx::k_instance_uv_from_corner`, and the resolve and the reference path tracer add the corner's place in the grid's frame back before the material's lookup (`gfx::terrain_uv_offset`; [renderer](../subsystems/renderer.md#the-grounds-tiles-are-placed-at-their-corners), [gfx](../subsystems/gfx.md#the-frames-origin)). `msvc-debug`, RTX 5090.

### The DAG

`world tiles: a tile built far out is its twin by the origin, cluster for cluster` (`terrain_tiles_tests.cpp`, CPU): a 16-cell tile of the inner level with a coarser neighbour on one edge, its ground a function of the lattice point measured from the ground's own origin, built by the origin and moved with its ground by 6,548, 156,250 and 1,562,500 cells, each DAG written as a `.clusters` container and its sections hashed as `domain/geometry`'s determinism test hashes them.

| | Vertex streams that differ | Sections that differ (of 33) |
|---|---|---|
| UVs in the grid's frame (before) | 281 of 281 (the UVs; positions and normals equal) | 7 at every move: clusters, LOD records, vertices, attributes, triangles, vertex sources, the 16-bit grid |
| UVs from the corner (after) | 0 | 0 at every move |

Twelve clusters in four levels either way: the far tile's tree was the same shape and other clusters.

### The translation suite's terrain at the default cut

`world translation: the world's tiles moved by whole cells with their ground draw the same bytes` (`terrain_tiles_gpu_tests.cpp`) now runs at the finest cut and at the default one (`lod_px` 1). At the default cut, of 24,576 pixels, on each of the five paths (mesh, software, vertex indexed, vertex capacity, ray visibility), both layouts (the world's rings, and with far levels, which gave the same counts), and each move:

| | Id words | Depths | Normal bytes | Colour bytes |
|---|---|---|---|---|
| before | 22,357–22,395 | 10,074–10,105 | 18,328–18,863 | 5,331–5,705 |
| after | 0 | 0 | 0 | 0 |

The finest cut was byte-identical before and is after.

### What moved by the origin, and the tolerances

**The erg drawn from tiles against the grid** (`world tiles: the erg from tiles is the grid's picture inside its extent`): unchanged — 156,172 pixels compared, 6,690 differ in some bit of depth and 11 by more than 16 float steps, **0 in colour**. The brief expected a colour tolerance here, since a tile's UVs are now far finer than the grid's (a half float between 0.5 and 1 steps by 2⁻¹¹, 19 cm of that test's 384 m grid); none was needed: the grid's maps there are 256 texels over 384 m and bilinear, and no byte moved.

**The seams test** (`world tiles: no crack, no T-junction and no lighting seam at any border`) failed one check: 50 km out, from above, at a pixel's cut, the largest normal step across a same-level border was 0.1126 rad against a bound of the largest step inside a tile (0.0917) plus a quantum (0.016). One pair, at x = 50,000 m: the side before the border a coarse cluster, flat (0.008 rad to the pixel before it), the side after it the lattice's own triangles on a crest (0.033 to the pixel after it); the step's excess over its neighbours, 0.080, is within the inside excess's 0.070 plus a quantum, so the normal field is continuous there and kinks. With the old UVs that view's DAGs were simplified from UVs of 390 and its largest inside step was 0.206, which the bound had been measured against. The step's bound at a coarser cut now also admits the finest cut's largest inside step of the same view (0.1316 here) — the steepest the lattice's own triangles turn the normal — since a border's two tiles are cut apart and one may draw the lattice there. No other check moved; the excess bounds are unchanged.

### Cost

The endless desert's flight (`camera-path.json`, 7,201 frames, world tiles and far levels), offscreen 11520×2160 `--views surround3`, `--repeat 1 --warmup 60 --wait-quiet 60`, each run under `tools/gpu-lock.ps1 run`, `msvc-release`. "Before" is `main` at `333b4e0e` built from an exported copy, "after" this branch; the two alternately, one pair after another. GPU milliseconds at the median of the frames:

| Run | cull | deform | hw | shadow (maps) | rt (structures) | resolve | total |
|---|---|---|---|---|---|---|---|
| `rt`, the offset in 64-bit integers | 0.065 → 0.066 | 0.039 → 0.039 | 0.312 → 0.311 | — | 0.944 → 0.947 | 1.861 → 1.888 | 4.358 → 4.385 |
| `csm`, the same | 0.131 → 0.130 | 0.055 → 0.057 | 0.293 → 0.295 | 0.481 → 0.482 | — | 1.739 → 1.764 | 4.141 → 4.182 |
| `rt`, again | 0.064 → 0.066 | 0.039 → 0.038 | 0.314 → 0.308 | — | 0.945 → 0.947 | 1.860 → 1.887 | 4.367 → 4.376 |
| `csm`, again | 0.130 → 0.129 | 0.056 → 0.056 | 0.294 → 0.289 | 0.481 → 0.481 | — | 1.738 → 1.762 | 4.145 → 4.136 |
| `rt`, the offset in float32 (committed) | 0.064 → 0.066 | 0.039 → 0.039 | 0.310 → 0.309 | — | 0.945 → 0.947 | 1.862 → 1.869 | 4.355 → 4.365 |
| `csm`, the same | 0.130 → 0.129 | 0.056 → 0.057 | 0.295 → 0.291 | 0.481 → 0.482 | — | 1.740 → 1.748 | 4.150 → 4.147 |

**What it says.** The first version formed the corner's offset as `gfx::terrain_corner_mm` does, in 64-bit integers, and the resolve paid 0.024–0.027 ms for it, 1.4%, in all four runs — the GPU emulates 64-bit integer arithmetic, and every shaded pixel of the ground ran it. The committed version does it in float32 (whole millimetres, exact within 16.7 km of the grid's corner, past which the UV clamps; `terrain_uv_offset`, gfx.md) and multiplies by the frame's UV units per millimetre instead of dividing by its side: the resolve pays 0.007–0.008 ms, 0.4–0.5%, which is the add, the flag's test and the instance words it reads. Every other pass moved by less than the runs' own spread (0.005 ms or less; the cull, the pool and the maps draw the same clusters), and the totals by +0.01 to −0.003 ms in the last pair. The frame thread's CPU did not move (0.417–0.424 ms at the median in `rt`, 0.452–0.464 in `csm`).

**Machine state.** Of the twelve runs, `quiet` was true for the first `rt` pair, the first `csm` "before", and the last `rt` "before" and `csm` pair; the others were marked not quiet by another session's GPU use at their ends (23–34% busy) or other processes' CPU (0.7–10.9% at a run's start or end). Nothing of this branch was building during any run. The GPU spans are the passes' own timers; the resolve's difference repeated within 0.003 ms in each version, so it is the change and not the hour.

## Stage 5: the ground's leftovers

The last absolute float32 the ground and its placements had (roadmap W61–W64, 2026-10-07), each held by tests at the three far sites and astride a cell's edge. `msvc-debug`, RTX 5090. What is measured here is positions, meshes and pictures, not time, so the machine's load does not enter them.

### Ring chunks in their corner's frame (W63)

A ring's chunk was built by the ground provider in the world's frame and moved to its corner by the renderer afterwards (`place_at_corner`); the provider builds it in its corner's frame now (`terrain::RingParams::chunk_frame`): positions and UVs from the chunk's lattice corner ([renderer](../subsystems/renderer.md#the-grounds-tiles-are-placed-at-their-corners)).

**The far twin** (`terrain rings: a chunk built in its corner's frame far out is its twin by the origin`, `domain/terrain`, CPU): rings 16 m either side at 50 cm and 64 m either side at a metre, over a ground that is a function of the lattice point measured from its own origin, moved with the camera; the camera inside a chunk, and a millimetre short of a cell's edge (the inner ring straddling it). A chunk mesh has seven streams compared (positions, normals, UVs, locks, triangles, and the grid and skirt counts), a chunk's DAG nine (clusters, vertices, vertex sources, attributes, triangles, the 16-bit grid, its origin and step, LOD records, level counts), the DAG built for the inner ring's first chunk.

| Move | Camera | Chunks | Mesh streams that differ: corner's frame / world's | DAG streams that differ: corner's / world's | Clusters: twin, corner's, world's |
|---|---|---|---|---|---|
| 419,072 m | in a chunk | 8 | 0 / 16 | 0 / 9 | 40, 40, 39 |
| 10,000 km | in a chunk | 8 | 0 / 16 | 0 / 9 | 40, 40, 42 |
| 1e8 m | in a chunk | 8 | 0 / 16 | 0 / 9 | 40, 40, 38 |
| 419,072 m | 1 mm short of a cell's edge | 6 | 0 / 12 | 0 / 8 | 47, 47, 47 |
| 10,000 km | the same | 6 | 0 / 12 | 0 / 9 | 47, 47, 50 |
| 1e8 m | the same | 6 | 0 / 12 | 0 / 9 | 47, 47, 45 |

In the world's frame two streams of every chunk differ — its positions and its UVs, the frame's absolute numbers — and its DAG is another tree. **By the origin** (`terrain rings: a chunk in its corner's frame is the world's chunk at its corner`) the corner's chunk is the world's less its corner to the bit over the 29 chunks of three rings at 50 cm, 1 m and 2 m, and a UV from the corner plus the corner's place in the frame is the world's UV exactly (the largest difference, in f64, 0): on the default dyadic lattices the move was exact, which is why the renderer could do it afterwards.

**The picture by the origin.** `engine-view --terrain-rings` over a 512 m dune terrain at 2 m (seed 23, three years in) with the rings at 32 m (50 cm) and 128 m (1 m), 640×360, the camera 3 m over the sand, four frames, offscreen, before (this branch's base, `8cb66941`) and after, under `tools/gpu-lock.ps1`: the ids differ at 82,922 of 230,400 pixels (159,772 of 691,200 words), because the cut names other clusters — the DAG is simplified from UVs measured from the corner, near zero, where they were the grid frame's ~0.5, and the simplifier weighs UVs; the colour at 5,149 pixels, 1.5 levels on average and 31 at most; the depth image at 454 pixels; the normal image at 7,226 (3.2 levels on average, 74 at most). The same sand at another tessellation, as a re-centre's rebuild is. The rings' tests in the renderer pass unchanged (`terrain rings*`: every level at one time and no step at a re-centre, culling and the rasterizers over the rings; `sand detail*` across the rings' and chunks' borders), and the translation suite's terrain case stays byte-identical at both cuts.

### A scatter's centre and a drift's ends in f64 (W61)

**A scene file's scatter** (`engine.scene.Scatter` version 2: `center` a `worldpos`; [renderer](../subsystems/renderer.md#positions-in-the-files-and-on-the-wire)). `renderer: a scene file's scatter far out is its twin by the origin, moved` reads two scatters of 16 palms' worth — one grounded on a sawtooth ground of millimetres, one at a height of its own — centred by the origin, at 419,072 m, 10,000 km and 1e8 m, and a 1024th of a metre short of a cell's edge 10,000 km out: 0 of 128 far places differ from their twin's plus the centre, 0 grounded instances stand off the height of the millimetre they are at, and at the cell's edge 11 instances stand west of it and 21 east. With the centre a float32 `vec2` and the place float32 world metres, as until now, a far instance stood on a float's grid there — 3.1 cm at 419 km, a metre at 10,000 km, 8 m at 1e8 m — and was grounded at that rounded point (by the arithmetic: the old reader cannot express the test). By the origin the place moves by the float32 sum's rounding, at most half a float's step at its coordinate (7.6 µm for the desert overlook's palms, within 175 m of their centre), and the ground is asked at the instance's whole millimetre where it was asked at its float32 point, a change of the slope times at most half a millimetre in height; this is the arithmetic, not a measurement of the overlook's picture.

**A wall's sand drift** (`ruins::Drift::from`, `to`, and `engine.scene.SandDrift` version 2; [ruins](../subsystems/ruins.md#far-from-the-origin)). `ruins: a wall's drift far out is its twin by the origin, moved by whole centimetres`: the tile grid's corner moved to 419,072 m, 10,000 km and 1e8 m, and 48 m past 10,000 km so a tile straddles a cell's edge, four tiles' buildings each time: of 20 drifts, 0 ends not moved by exactly the corner's centimetres, 0 not their centimetres divided once at the building's base, 0 read back from a fragment as other doubles. `Drift` grew from 40 to 72 bytes. The ruins' two determinism goldens moved because the drifts are in the output hash: `engine-content ruins` from `0x0d29923a1f3c328f` to `0xdfb858d685ebff89` (5,726 instances, as before) and laid in blocks from `0x42f769e8223c5041` to `0x9a6d5806eaa41f7c` (25,454 blocks, as before), taken on MSVC 14.51; every piece and site is the same, and only the drifts' hashed bits moved.

### The waves and the world's tiles (W62)

Decided rather than measured ([scene_gen](../subsystems/scene_gen.md#far-from-the-origin)): the waves keep their float32 arithmetic and the renderer draws the world's tiles only from a ground provider that says its heights are right wherever a cell reaches (`scene_gen::k_ground_millimetres`; the dunes). No picture moves: the waves never reached the tiles (the tiles also need a ground that moves, which the waves are not), and the dunes, the only ground tiles are drawn from, carry the flag. What the alternative would have cost was not measured: f64 phases for every wave, in its point function and its grid, would have moved the waves' heights by the origin by their float rounding, and with them the scene grid's mesh cache key and the fixtures' pinned numbers.
