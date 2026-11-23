# What a gale builds in the traced chain (R206, 2026-11-23)

Roadmap R206, the forest proof of concept's need 172. [renderer](../subsystems/renderer.md#what-a-gale-builds) has the rule this page's numbers chose.

## The finding, from the forest's own reports

The proof of concept's listening runs on batch 72's binaries (its `quality-v56` reports, `listen-light` and `listen-gale`: 1600 × 900, `--raster hw --shadows rt`, micromaps and the bounce off, a module with a replayed walker, `renderer.flora.wind_strength=0.7`, the hand wind `renderer.wind.speed_m_s` 3 and 20, 9,000 frames each; the session was not locked, other processes 12 to 27 % of the CPU):

| | wanted (median) | built | dropped instances / casters (whole run) | visible pairs | shadow casters | GPU frame | `rt` | `clas` |
|---|---:|---:|---|---:|---:|---:|---:|---:|
| 3 m/s | 27,936 | 27,936 | 0 / 0 | 1,231,434 | 131,022 | 8.44 ms | 1.83 | 0.61 |
| 20 m/s | 458,819 | 337,514 (the budget) | 3,150 / 2,270,013 | 1,520,242 | 273,464 | 17.44 ms | 8.93 | 6.77 |

The gale's 430,883 more structures a frame are its 288,808 more visible pairs and 142,442 more casters, to within the 367 of the frame's other changes: **the chain built what the cut asked for, and the cut had grown.** Both runs held the same 2,256 swaying instances (221,580 parts); the padding ladder (W154-2) stood at its calm rung at 3 m/s and stepped to rung 5 (load 15.2: every swaying record padded 7.6 times its calm bound) at 20 m/s, and the cull pass put that padding on the LOD spheres as on every other.

## The trees' motion a frame

`engine-view --census --tunable renderer.rt.census_motion=true` (R206: `GpuScene::measure_motion`, `VisibleCensus::count_motion`) takes, every frame, each swaying tree's farthest part tip against the last frame's pose and its crown's distance, and sorts the frame's deforming entries of swaying trees by both: pixels at 3840 × 2160 and 55° (4.82 × 10⁻⁴ rad; at 1600 × 900 a pixel is 2.4 of these). On the forest staging (`<scratch>\v41`, batch 72's main with the census, its tile set rebuilt in a root of this row's own after the staging's was lost: below), the grounded walk (`walk-grounded.json`, 2,400 frames at 30 a second, so a frame is 1/30 s of world time), 1600 × 900, `--raster hw --shadows rt`, per frame:

**A gale (20 m/s), the drawn entries** (a tree counted only where its slot held the same tree, posed, the frame before; this row's cut):

| distance | trees | entries a tree | < 0.25 px | 0.25–0.5 | 0.5–1 | 1–2 | 2–4 | 4–8 | 8–16 | ≥ 16 | all |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| < 10 m | 35.8 | 2.7 | 0 | 0 | 1 | 5 | 14 | 16 | 9 | 51 | 96 |
| 10–20 | 72.0 | 3.2 | 0 | 4 | 14 | 30 | 55 | 11 | 0 | 114 | 228 |
| 20–40 | 254.0 | 3.2 | 13 | 49 | 98 | 161 | 80 | 2 | 43 | 361 | 807 |
| 40–80 | 693.0 | 2.3 | 160 | 220 | 336 | 86 | 5 | 98 | 402 | 307 | 1,614 |
| 80–160 | 217.6 | 3.0 | 85 | 142 | 89 | 5 | 6 | 102 | 115 | 100 | 644 |
| 160–320 | 6.8 | 8.1 | 4 | 20 | 2 | 1 | 0 | 0 | 1 | 26 | 55 |
| all | | | 262 | 434 | 541 | 287 | 160 | 229 | 570 | 959 | 3,443 |

The casters' run (7,380 a frame) has the same shape: 3,059 of them at 40 to 80 m and 2,556 at 80 to 160 m, 1,522 at 16 pixels or more. The first flight of the walk, before the filter and with main's cut, counted 3,757 drawn and 7,620 casters a frame in the same proportions.
**A light wind (3 m/s), the drawn entries**, the same walk: 2,988 a frame, 1,398 of them under a quarter pixel, 1,526 between half a pixel and 8, 105 past 8; 2.3 to 2.9 entries a tree.

**What decides the design.** In a gale there is no band of distance where the swaying trees stand under a pixel a frame: past 40 m 1,151 of 2,313 entries are trees whose tips move 4 pixels or more, and under a quarter pixel are 249; over the whole walk 1,237 of 3,443 move under a pixel a frame. The tips are the leaves' and twigs' flutter, whose amplitude the motion model grows with the load (about twelve times from 3 to 20 m/s) at a fixed frequency, so its speed grows with the wind and its angle a frame falls only as 1/d over a sway that ends at the swaying rings' edge. The measure is the farthest tip of a tree, so a few long chains dominate it: the column of 16 pixels and more holds trees at every distance (on `streamed-forest`'s path even at 160 to 320 m, which only a compounding chain explains), and the decision rests on the lower columns, not on it. A tree posed every k-th frame with k chosen to keep the step under a pixel would have k = 1 for nearly every tree, and its structures kept between would buy nothing. (At 60 frames a second the pixels halve and the conclusion does not move: 1,529 of the gale's 3,443 entries, 44 %, are 8 pixels or more at 30, so 4 or more at 60.)

## The cut, before and after

The staging's own swaying meshes draw two or three clusters a tree, so the cut has little room to grow there: its walk wanted 10,457 structures at the median in a light wind and 11,900 in a gale (×1.14), with nothing dropped. The engine's stand-in for the proof of concept's trees is `content/test-scenes/streamed-forest` (54 swaying trees of the flora capability with their card levels, 709 parts a tree), its camera path, 1600 × 900, `--raster hw --shadows rt`, main's binary (5801d60f) beside this row's, the session locked for both:

| | wanted, median / p95 / max | visible pairs (median) | casters | `rt` | `clas` | GPU frame |
|---|---|---:|---:|---:|---:|---:|
| main, 3 m/s | 5,159 / 23,503 / 29,957 | 38,124 | 54,290 | 0.426 | 0.147 | 1.255 |
| main, 20 m/s | 9,211 / 33,973 / 35,748 | 50,254 | 55,342 | 0.550 | 0.237 | 1.461 |
| R206, 3 m/s | 5,159 / 23,503 / 29,957 | 38,124 | 54,290 | 0.423 | 0.147 | 1.251 |
| R206, 20 m/s | 6,529 / 27,238 / 30,236 | 49,474 | 53,967 | 0.458 | 0.186 | 1.308 |

(Other processes held 15 to 100 % of the CPU and the card was otherwise idle; the session was locked for all four, so the milliseconds are upper bounds and only comparable with each other.) **A light wind is main's to the pair** (the ladder stands at its calm rung, the table is not read: wanted, visible pairs and casters identical). **In the gale the chain's structures fall by 29 % at the median** (9,211 → 6,529; 1.27 times the light wind's where they were 1.79) and `rt` plus `clas` from 0.787 to 0.644 ms, 1.12 times the light wind's 0.573. What is left of the gale's rise is what the culling spheres, which keep the rung's padding and rightly so, let through: pairs at the picture's edge the frustum test keeps, pairs the Hi-Z no longer hides behind nearer leaves (the occlusion test takes the padded sphere too), and casters the caster volumes admit. The census by kind says it is the deforming entries and nothing else: drawn and deforming (the swaying trees and the ground's levels) 2,328 at the median in the light wind, 6,005 in main's gale and 4,021 in this row's; the rigid forms' static structures 35,390, 35,486 and 35,486; the cache's references 128 in all three.

**On the forest staging at the owner's settings** (3840 × 2160, `--raster rt --shadows rt`, micromaps, the bounce with its guide and memory, the accumulation on, so the GPU times are the summary's `reconstructed` block; the grounded walk, 2,401 frames; each load carries the known 72 s at its start; the session locked, other processes 22 to 31 % of the CPU; main's binary and this row's back to back):

| | wanted, median / p95 / max | dropped | `rt` | `clas` | `rt` + `clas` | GPU frame | frame, median / p95 |
|---|---|---|---:|---:|---:|---:|---|
| main, 3 m/s | 92,069 / 105,099 / 108,706 | 0 | 3.202 | 1.987 | 5.189 | 19.45 | 19.70 / 24.76 |
| R206, 3 m/s | 92,072 / 105,217 / 108,881 | 0 | 3.197 | 1.985 | 5.182 | 19.43 | 19.65 / 24.74 |
| main, 20 m/s | 94,449 / 107,621 / 111,858 | 0 | 3.287 | 2.073 | 5.360 | 20.26 | 20.50 / 25.45 |
| R206, 20 m/s | 93,734 / 106,528 / 110,786 | 0 | 3.277 | 2.056 | 5.333 | 20.18 | 20.45 / 25.42 |

The staging does not reproduce the proof of concept's gale: its traced chain wants about 92,000 structures a frame in either wind, most of them the sward's untraced patches built because the picture is traced (66,638 a frame at the walk's start), and its swaying trees draw two or three clusters each, so their cut has little to grow by. Nothing is dropped on either side, the gale's `rt` plus `clas` is 1.03 times the light wind's on both, and a light wind costs what it cost.

The case `flora motion: three palms at force 12 draw and build the calm cut` measures the same on three palms 16 m away: 33 visible pairs and 33 structures calm, 115 and 115 at force 12 with W154-2's cut (`renderer.flora.calm_cut=false`), 33 and 33 with this row's.

## The staging's tile set was lost on 2026-11-22

At 21:07 a cleanup of old scratch folders went through junctions into the staging's derived-data root: `v41\ddc\tilesets\7f92548978e944d0` kept its manifest and lost its archives (`250.tiles`, 2.7 GB, and the far levels), and main's binary refused the staging ("the ground provider tiles could not open its set"). This row wrote nothing into it: its flights read a root of its own (`<scratch>\b73\R206\ddc-root`: junctions to the staging's clusters, textures and far forest, plain copies of the small directories, and the tile set rebuilt by `engine-content ground sample` in two and a half minutes).
