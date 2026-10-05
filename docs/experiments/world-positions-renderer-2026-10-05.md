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

**Not measured in this stage.** The ground detail tests' far case (`domain/gfx/tests/ground_detail_tests.cpp`, "points clamped onto their triangle") still draws its sand as world-space vertices under an identity instance, so it reads what it read; a version with the sand as a mesh instance placed far out, which should fall to the origin's count, is not written yet.

## Stage 2 (picture-2): the cascades snap to the world

Stage 1 anchored the cascades' texel snapping at the corner of the eye's 64 m cell, which kept the translation suite's maps byte-identical and made every cascade take a sub-texel step whenever the eye crossed into another cell. `fit_shadow_cascades` now snaps in the world's light space in f64 (`snap_in_world`) and expresses the snapped centre relative to the eye afterwards ([renderer](../subsystems/renderer.md#cascaded-shadow-maps)).

**The test that matters** (`renderer: a cascade's texels hold their place in the world as the eye crosses a cell`, CPU): the eye stepped a 1024th of a metre at a time, twelve steps, across the corner of a cell (x = 64 and z = 128 at once), four cascades of 1.17 cm to 2.09 m texels, by the origin and 156,250 cells (10,000 km) out. How far each cascade's grid moved against the world, in texels, at the worst step:

| Anchor | by the origin | 10,000 km out |
|---|---|---|
| the eye's cell's corner (stage 1) | — | 0.46, 0.14, 0.46, 0.48 |
| the world, in f64 (now) | 2.8e-5, 1.6e-5, 3.2e-5, 3.1e-5 | 3.4e-5, 1.7e-5, 2.8e-5, 3.6e-5 |

The remaining hundred-thousandths of a texel are the frame-space centre's float32 rounding. (The stage-1 row by the origin was not printed: the test stops at the first failing site's messages; the run with the old anchor failed 64 assertions over both sites.)

**The translation suite's cascaded case** (`world_translation_tests.cpp`, RTX 5090): ids and depth still byte-identical at every move; colour byte-identical outside a band two pixels either side of every shadow edge — 1,302 to 2,211 of the 24,576 pixels, with 93 to 272 shadowed pixels and every lit one compared outside it, and 0 bytes differing there at 6,548, 156,250 and 1,562,500 cells. Inside the band 173 to 266 colour bytes differ, the shadows' filtered edges falling on the texel grid differently. The twelve millimetre steps 10,000 km out against the origin's: 0 bytes outside the band over the twelve frames.
