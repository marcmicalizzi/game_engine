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

**A gale (20 m/s), the drawn entries:**

| distance | trees | entries a tree | < 0.25 px | 0.25–0.5 | 0.5–1 | 1–2 | 2–4 | 4–8 | 8–16 | ≥ 16 | all |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| < 10 m | 35.7 | 3.5 | 0 | 0 | 1 | 5 | 17 | 22 | 14 | 67 | 127 |
| 10–20 | 72.0 | 3.4 | 0 | 4 | 14 | 31 | 61 | 14 | 0 | 124 | 248 |
| 20–40 | 254.0 | 3.6 | 13 | 49 | 102 | 192 | 101 | 3 | 43 | 416 | 918 |
| 40–80 | 695.3 | 2.5 | 161 | 222 | 344 | 123 | 7 | 99 | 404 | 358 | 1,717 |
| 80–160 | 219.9 | 3.0 | 86 | 144 | 93 | 5 | 7 | 103 | 118 | 106 | 662 |
| 160–320 | 6.9 | 12.4 | 5 | 23 | 3 | 10 | 1 | 0 | 1 | 41 | 85 |
| all | | | 266 | 441 | 556 | 366 | 194 | 242 | 579 | 1,113 | 3,757 |

The casters' run (7,620 a frame) has the same shape: 3,112 of them at 40 to 80 m and 2,575 at 80 to 160 m, 1,640 at 16 pixels or more.

**A light wind (3 m/s), the drawn entries**, the same walk: 2,988 a frame, 1,398 of them under a quarter pixel, 1,526 between half a pixel and 8, 105 past 8; 2.3 to 2.9 entries a tree.

**What decides the design.** In a gale there is no band of distance where the swaying trees stand under a pixel a frame: past 40 m 1,230 of 2,464 entries are trees whose tips move 4 pixels or more, and under a quarter pixel are 252. The tips are the leaves' flutter, whose speed grows with the wind and not with the distance, so its angle a frame falls only as 1/d over a sway that ends at the swaying rings' edge. A tree posed every k-th frame with k chosen to keep the step under a pixel would have k = 1 for nearly every tree, and its structures kept between would buy nothing. (At 60 frames a second the pixels halve and the conclusion does not move: 1,692 of the gale's 3,757 entries, 45 %, are 8 pixels or more at 30, so 4 or more at 60.)

## The cut, before and after

The staging's own swaying meshes draw two or three clusters a tree, so the cut has little room to grow there: its walk wanted 10,457 structures at the median in a light wind and 11,900 in a gale (×1.14), with nothing dropped. The engine's stand-in for the proof of concept's trees is `content/test-scenes/streamed-forest` (54 swaying trees of the flora capability with their card levels, 709 parts a tree), its camera path, 1600 × 900, `--raster hw --shadows rt`, main's binary (5801d60f) beside this row's, the session locked for both:

| | wanted, median / p95 / max | visible pairs (median) | casters | `rt` | `clas` | GPU frame |
|---|---|---:|---:|---:|---:|---:|
| main, 3 m/s | 5,159 / 23,503 / 29,957 | 38,124 | 54,290 | 0.426 | 0.147 | 1.255 |
| main, 20 m/s | 9,211 / 33,973 / 35,748 | 50,254 | 55,342 | 0.550 | 0.237 | 1.461 |

The case `flora motion: three palms at force 12 draw and build the calm cut` measures the same on three palms 16 m away: 33 visible pairs and 33 structures calm, 115 and 115 at force 12 with W154-2's cut (`renderer.flora.calm_cut=false`), 33 and 33 with this row's.

## The staging's tile set was lost on 2026-11-22

At 21:07 a cleanup of old scratch folders went through junctions into the staging's derived-data root: `v41\ddc\tilesets\7f92548978e944d0` kept its manifest and lost its archives (`250.tiles`, 2.7 GB, and the far levels), and main's binary refused the staging ("the ground provider tiles could not open its set"). This row wrote nothing into it: its flights read a root of its own (`<scratch>\b73\R206\ddc-root`: junctions to the staging's clusters, textures and far forest, plain copies of the small directories, and the tile set rebuilt by `engine-content ground sample` in two and a half minutes).
