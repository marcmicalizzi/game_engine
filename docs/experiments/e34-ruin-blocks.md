# E34: ruins laid block by block against ruins built from kit sections

- **Question ([10 §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing), E34; [07 §7.6](../plan/07-content-pipeline.md#76-procedural-generation-volume-content), the direction note and status note of 2026-09-24):** Desert Survival's ruins are assembled from a kit, and the plan named two representations of one assembled building to be measured against each other: wall **sections** — kit members with their own cluster LOD DAGs, one instance each — and **blocks**, the block-laying generator run per building so that every block is an instance of one of a dozen block meshes. The first numbers ([ruins](../subsystems/ruins.md#performance-notes)) showed sections to be cheap in instances and expensive in **pairs**, because the cull pass runs one thread per (instance, cluster) pair over every level of every member's DAG. What do blocks cost in pairs, instances, cull time, memory and load time, at 100 and 1,000 buildings and at three block fidelities; and what does one building cost the cull at a distance in each representation?
- **Date:** 2026-09-25. **Machine:** Intel Core i9-10980XE (18 cores, 36 logical CPUs), 64 GB, Windows 11 Pro 26200; **GPU:** RTX 5090, driver 610.88, Vulkan 1.4. **Build:** `msvc-release` at `4c53c92` (the block layer's commit; the rebase onto `ef4d6d9` that followed it changed docs and tools only).
- **Machine state:** shared and busy; every flythrough raised the busy-machine WARNING. Each group of runs held the GPU lock on its own (`ENGINE_GPU_LOCK_OWNER=agent-blocks`, released between groups; five other agents' full test suites and merge gates held it in between, and the last group waited 71 minutes for it), and each run measured anyway after `--wait-quiet 60` found no quiet minute. The summaries' `machine_state` is quoted per row: other processes at 7–47% of the CPU — and 100% for one row, marked — and the GPU ours when every run started, with 7.1–8.9 GB of 32.6 GB held by the desktop and others. GPU times are the renderer's timestamp queries, which CPU load moves little: the terrain-alone and synthetic-section rows reproduce [ruins](../subsystems/ruins.md#performance-notes)'s, taken on another day at 15–50%, to within 0.003 ms in every column. They are upper bounds all the same, and the CPU numbers (the bench, the scene read) more so: the bench ran at 7% others with another agent's full suite holding the GPU lock.
- **Decision:** none yet: a status note in [07 §7.6](../plan/07-content-pipeline.md#76-procedural-generation-volume-content). The numbers say what a block may cost and what the far tier and the handover have to do; they do not choose.

## Setup

**What was built** ([ruins](../subsystems/ruins.md#the-block-layer)): the block layer (`domain/ruins/blocks.h`), which lays the section assembler's own building — its footprint, frame, ruin states and openings — as blocks: courses in running bond along each wall's centre line, the two walls at a corner taking its square in turn with quoins, an opening a gap of whole courses with a lintel over it, a block standing under the ruin rule's line (broken per block by a seeded draw) only where the course under it carries it, and every block that did not stand dropped beside its wall as debris. So a section building and a block building from one seed are **the same ruin** — the same site, walls and drifts, field for field — and a row of one compares with a row of the other building for building.

**The kits.**

- *Sections, synthetic*: the kit of boxes (`engine-content ruins-kit`), one cluster a member; ruins.md's baseline, re-run in this session.
- *Sections, E33*: the ashlar kit's second iteration, `game_engine_local\blender-kits\ruined-wall\out\kit-ashlar-cc0\kit.json` ([E33](e33-hard-surface-kits.md#second-iteration-2026-09-24-the-kit-as-members-for-the-ruin-assembler)): nineteen textured members, 2 m module, 0.64 m walls 2.70 m high, sections of 2 m and 4 m and both corners at three heights, a doorway, a window, five debris stones. The members build to 641–5,863 clusters (the 4 m section 5,520 at full height, the inside corner 5,863) and the stones to 38–66. ruins.md's first E33 row used the first kit — a 10 m section of 13,163 clusters, free-standing walls with their own skirts — and is not this comparison.
- *Blocks*: the synthetic block kit (`engine-content ruins-block-kit --thickness 0.64 --course 0.3 --stretcher 0.6`) at three fidelities, laid on **the E33 kit's footprints**: fourteen untextured meshes — stretchers of 0.45, 0.6 and 0.75 m, a half, a 0.94 m quoin, a 1.5 m lintel and a 1.0 m sill, crisp and eroded — each a course (0.3 m) high and a wall (0.64 m) thick; nine courses a wall. `low` is a chamfered box of 44 triangles and **1 cluster**; `mid` a rounded, eroded block of about 5,000 triangles and **105–111 clusters**; `high` the same at about 50,000 triangles and **1,093–1,116 clusters**, which brackets E33's image-to-3D bricks: Tripo's sandstone brick builds to 1,212 clusters from 49,304 triangles and its brick block to 1,195. Running those bricks as a fourth fidelity would need a scale the block kit does not carry (they are normalised to a unit), and at 1,200 clusters a block they would be the `high` rows within 10% — refused at 100 buildings — so they were counted and not flown.

**The buildings**: world seed 2026, 32 m tiles, the `count` tiles of the square (−16, −16)–(15, 31) the seed ranks first (so the first 100 of the 1,000, and the first 20 of the 100), wind from 30°, over the desert overlook's terrain; the square covers the camera path. The E33 footprints' 100 buildings are 58 rectangles, 29 L's, 2 U's and 11 courtyards: **4,697 section pieces** (3,200 of them debris stones), or **71,502 blocks** (35,469 standing, 36,033 fallen), 715 a building (324–1,490). A thousand are 711,468 blocks.

**The flythrough**, as ruins.md's: `engine-view --offscreen` at 1920 × 1080, the desert-overlook terrain (2,049² vertices, 187,660 clusters) and its camera path resampled to 600 frames, three repeats, occlusion on, shadows off, the hardware rasterizer. The scenes and scripts are under `game_engine_local\ruins\blocks\` (never committed):

```powershell
engine-content ruins-kit D:\workspace\game_engine_local\ruins\blocks\kit
engine-content ruins-block-kit D:\workspace\game_engine_local\ruins\blocks\blocks-mid --fidelity mid --thickness 0.64 --course 0.3 --stretcher 0.6
pwsh D:\workspace\game_engine_local\ruins\blocks\make-scenes.ps1     # scene-<row>.json
$env:ENGINE_GPU_LOCK_OWNER = 'agent-blocks'
pwsh tools/gpu-lock.ps1 run -Purpose "E34 ..." -Exec "pwsh -NoProfile -File D:\workspace\game_engine_local\ruins\blocks\run-group.ps1 -Scenes blocks-mid-100,blocks-mid-200,blocks-mid-1000"
#   each: engine-view --scene scene-<row>.json --camera-path content\test-scenes\desert-overlook\camera-path.json
#     --offscreen --benchmark out\<row>.jsonl --frames 600 --repeat 3 --width 1920 --height 1080
#     --shadows off --ddc <local ddc> --log info --wait-quiet 60
pwsh D:\workspace\game_engine_local\ruins\blocks\parse.ps1          # the rows below
build\msvc-release\domain\ruins\engine_ruins_bench.exe --filter=ruins.* --wait-quiet=300
```

**The distance check** (`run-distance.ps1`, `dist.ps1`): one building alone over the terrain — tile (−8, −6), an L of 18 wall pieces and 25 debris stones in sections, or 577 standing and 286 fallen blocks — flown along the same path, one repeat, with `--census --census-pixels` (each frame's visible list read back and counted per mesh, and each mesh's pixels from an id capture) and **occlusion off**, so that the census counts what the frustum and the LOD cut keep, not whether ridge A hides the building. Its visible pairs are the census's count over its kit's meshes; the camera's horizontal distance to its centre (−241.2, −175.9) is the path sampled as `renderer::sample_camera_path` samples it: 1,100 m at the path's start, 190 m at its end.

## Results

**The flythrough.** Pairs are the scene's (instance, cluster) pairs, one cull thread each; the terrain alone is 187,660. Visible pairs are the ones the cull kept, over the path's frames.

| scene | pieces | pairs | visible pairs, median (p95; max) | cull ms, median (p99) | raster (hw) ms | all passes ms, median | GPU memory | others' CPU |
|---|---|---|---|---|---|---|---|---|
| the terrain alone | — | 187,660 | 804 (1,244; 1,339) | 0.026 (0.028) | 0.019 | 0.116 | 431 MiB | 30–35% |
| + 1,000 ruins, synthetic sections | 49,973 | 240,109 | 4,056 (9,774; 20,020) | 0.030 (0.032) | 0.024 | 0.131 | 431 MiB | 36–47% |
| + 100 ruins, E33 sections | 4,697 | 3,414,823 | 2,924 (5,908; 7,579) | 0.088 (0.096) | 0.023 | 0.184 | 2,219 MiB\* | 28–35% |
| + 100 ruins, low blocks | 71,502 | 259,162 | 4,239 (11,028; 24,667) | 0.030 (0.032) | 0.024 | 0.130 | 431 MiB | 9–19% |
| + 1,000 ruins, low blocks | 711,468 | 899,128 | 38,704 (112,159; 266,404) | 0.073 (0.077) | 0.067 | 0.216 | 496 MiB | 7–15% |
| + 100 ruins, mid blocks | 71,502 | 7,889,914 | 4,244 (11,057; 24,667) | 0.213 (0.235) | 0.030 | 0.327 | 801 MiB | 100%† |
| + 200 ruins, mid blocks | 145,001 | 15,808,879 | 9,662 (20,573; 49,384) | 0.424 (0.470) | 0.045 | 0.558 | 1,096 MiB | 27–32% |
| + 1,000 ruins, mid blocks | 711,468 | 76,845,988 | refused: more than 2^24 pairs | | | | | |
| + 20 ruins, high blocks | 13,535 | 15,162,910 | 1,836 (4,907; 6,993) | 0.362 (0.399) | 0.030 | 0.471 | 1,085 MiB | 7–15% |
| + 100 ruins, high blocks | 71,502 | 79,300,143 | refused: more than 2^24 pairs | | | | | |
| + 1,000 ruins, high blocks | 711,468 | 787,369,589 | refused: more than 2^24 pairs | | | | | |

\* The E33 members are textured and each uploads its own copy of the kit's atlas (the seventh finding in [ruins](../subsystems/ruins.md#the-kit)), up to nineteen copies, so most of this row's 1,788 MiB over the terrain is duplicated images and not the representation. The block kits are untextured. Compare the rows' memory within a representation, not across it. **The after number** (2026-09-25): the renderer now uploads one texture per distinct image per scene ([renderer](../subsystems/renderer.md#one-upload-per-distinct-image)), so the kit's 99 textures in 1,440 MiB are 6 in 80 MiB, the picture the same bytes. This row flown the same way — 1920 × 1080, the path, shadows off, but 60 frames and one repeat, in `msvc-debug`, on the same machine an hour apart — holds **684 MiB** where the build before it holds 2,220 MiB: **253 MiB over the terrain**, between the thousand low-block buildings' 65 and the hundred mid-block buildings' 370. Machine state: other processes at 6–11% of the CPU, the GPU ours, 7.5–9.0 GB of it in use with this run.

† Another process held every CPU for the whole of that run. Its GPU columns agree with the 200-building row's per pair to within 5%.

**Per building and per pair**, from the rows above (the terrain subtracted):

| representation | pairs a building | cull ms added per million pairs | GPU memory added per million pairs |
|---|---|---|---|
| synthetic sections (1,000) | 52 | — (0.004 ms in all) | — |
| E33 sections (100) | 32,272 | 0.019 | — (textures, above) |
| low blocks (100 / 1,000) | 715 | — (0.004 ms) / 0.066 | — / 91 MiB |
| mid blocks (100 / 200) | 77,023 / 78,106 | 0.024 / 0.025 | 48 / 43 MiB |
| high blocks (20) | 748,763 | 0.022 | 44 MiB |

**The pair cap.** A scene names at most 2^24 = 16,777,216 pairs, because the visibility buffer's id is `pair << 8 | triangle` in 32 bits ([gfx](../subsystems/gfx.md)); `GpuScene::create` refuses a scene past it with a sentence rather than drawing a wrong picture, so the three refusals above are the no-hidden-limits rule working, and they are findings, not failures. It is not raised here: the id's layout is `domain/gfx`'s and its shaders', and what the rows ask is not whether the cap should move but whether a representation that meets it at **about 215 buildings** (mid) or **about 22** (high) is one to use.

**The scene read and the load** (CPU; the renderer's `ruins assembled` line, on a pool of 35 workers and the caller when a scatter is more than 32 tiles — all but the 20-building row, which is one thread — and its `scene loaded` line, which covers every mesh from the derived-data cache and the instance table; the terrain alone is 747 ms of the latter):

| scene | ruins assembled | scene loaded |
|---|---|---|
| + 1,000 ruins, synthetic sections | 9.6 ms | 1,195 ms |
| + 100 ruins, E33 sections | 8.4 ms | 4,995 ms |
| + 100 / 1,000 ruins, low blocks | 15.6 / 83.2 ms | 1,014 / 950 ms |
| + 100 / 200 / 1,000 ruins, mid blocks | 24.2 / 36.0 / 86.4 ms | 935 / 1,044 / 1,034 ms, the last then refused |
| + 20 / 100 / 1,000 ruins, high blocks | 4.5 / 17.0 / 87.5 ms | 1,376 / 856 / 1,111 ms, the last two then refused |

**The block layer on the CPU** (`bench/ruins_bench.cpp`, seven repeats; the synthetic kit of boxes and the synthetic block kit on flat ground, 1,000 buildings of 684,962 blocks; the header's `machine_state`: 7.2% of the CPU in other processes at both ends, the GPU idle and its lock held by another agent's suite):

| what | median | min |
|---|---|---|
| one building laid block by block, the output cleared between them (`ruins.blocks.building`) | 67.8 µs | 67.7 µs |
| 1,000 buildings into one output, one thread (`ruins.blocks.1000/0`) | 74.9 ms | 74.8 ms |
| the same on the pool, 4 workers and the caller (`/4`) | 36.1 ms | 35.8 ms |
| the same, 8 workers and the caller (`/8`) | 22.2 ms | 21.3 ms |
| for comparison, the same buildings in sections (`ruins.assemble.building`, `.1000/0`) | 5.34 µs, 5.47 ms | 5.33 µs, 5.40 ms |

About 110 ns a block. Over the terrain the layer asks the ground once a fallen block, about 350 a building, which is most of the difference between the bench's 75 ms on one thread and the scene read's 83 ms on 36.

**The distance check** — the far building's visible pairs, frames binned by the camera's distance to it (it is in the frame, about 2,000 pixels, for the last 57 frames; before that it is off to the side or behind ridge A, which the census counts regardless with occlusion off):

| distance | frames | E33 sections | low blocks | mid blocks | high blocks |
|---|---|---|---|---|---|
| 190–250 m | 57 | 1,467 (1,369–1,467) | 863 | 863 | 863 |
| 250–400 m | 71 | 1,068 (782–1,354) | 863 | 863 | 863 |
| 400–600 m | 272 | 299 (0–767) | 863 | 863 | 863 |
| 600–800 m | 105 | 217 (217–300) | 863 | 863 | 863 |
| 800–1,100 m | 95 | 73 (61–217) | 863 | 863 | 863 |
| **all the building's pairs** | | 49,764 | 863 | 92,911 | 954,617 |

Median visible pairs, the range in brackets where it moves (the blocks' is 863 in every frame the building is in the frustum). **A block building is exactly one pair a block** — 577 standing and 286 fallen — at every distance on the path and at every fidelity: from 190 m out every block has fallen to its coarsest cluster, and there is nothing coarser than one cluster to fall to. The same building in sections falls from 1,467 visible pairs to 73.

The same building, 35 m away, in both representations, is `game_engine_local\ruins\blocks\out\far-e33-close.png` and `far-blocks-{low,mid,high}-close.png`: one L, its walls down to the same lines, the block one showing its courses in running bond and its fallen blocks heaped at the foot of the collapsed wall.

## What surprised me

- **The visible pairs are not the cost.** A hundred block buildings keep 4,239 pairs a frame at the low fidelity and 4,244 at the mid — the same, because along this path almost every block in view is at its coarsest cluster either way — while the mid rows cull a hundred times as many pairs to find them. The cull pass pays for **every level of every block's DAG**, and a 5,000-triangle block carries 108 clusters to be tested for a thing that, beyond 200 m, draws as one. The plan's framing — "a few hundred pairs against the section's thirteen thousand" — held only for blocks of one cluster.
- **Mid-fidelity blocks cost more pairs than E33's sections**, 77,000 a building against 32,000, although E33's members are scan-quality at 20,000–230,000 triangles each. A section spreads one DAG over two to four metres of wall; a block spends a DAG on each 0.6 m stone, and the levels a DAG needs to reach one cluster do not shrink with the object.
- **At distance the representations cross.** Near (under 250 m) the block building keeps fewer pairs than the section building (863 against 1,467); past 400 m the section building's DAG collapses toward a cluster a member (73 at a kilometre) while the block building stays at a cluster a block. So the far tier the section kit needs for triangles is also what blocks need for pairs, and more so.
- **Instances are nearly free; pairs are not.** 711,468 low-fidelity block instances add 0.047 ms of cull, 0.048 ms of raster and 65 MiB, and load in about 200 ms beyond the terrain; the cull's cost per pair does rise with the instance count (0.066 ms per million pairs at 711,000 instances against 0.019–0.025 at a few thousand — every pair's thread binary-searches the instances' `first_pair`), but a whole desert of one-cluster blocks is cheaper than a hundred E33 section buildings.
- **The 2^24 pair cap is the first wall.** Mid blocks meet it at about 215 E33 buildings in a scene and high blocks at about 22. Nothing in the endless desert draws that many at once, but a scene read is everything at once.

## What it decides

- **A block that carries a deep DAG is the wrong unit.** If blocks are drawn near the camera, they should be blocks of **one to a few clusters**, where the cull pays a pair a block (the low rows: a thousand buildings for 0.05 ms), and the detail a close block wants comes from its material — normal and height maps on a simple mesh — rather than from 5,000 triangles behind a 108-cluster DAG. A scan-quality block (the `high` rows, E33's image-to-3D bricks) costs 750,000 pairs a building, which is 20 buildings in a scene before the visibility id runs out.
- **Blocks cannot be the far tier.** At a kilometre the block building is 863 visible pairs to the section building's 73, and nothing in the block representation gets it lower: the far tier E33 asked for (a flat or blocky wall with a baked normal map, [E33](e33-hard-surface-kits.md#what-it-decides)) is needed by both representations, and by blocks sooner.
- **So the handover, when it is built, is blocks near and sections (or a slab) far, decided per tile.** With the block representation confined to the few tiles round the camera — nine tiles of 715 blocks at the mid fidelity are 700,000 pairs — the pair count is bounded by the near field and not by the desert, and the cap stops mattering. That is a materialization policy the capability's LOD policy (`detail_for_distance`) grows into, and the reason the block layer lays the section assembler's building: the handover can only avoid a pop if both sides are the same ruin, and they are.

## What it does not decide

- **The far tier's form**, and at what distance it takes over from sections: E33's question, still open.
- **The handover's mechanics** — the distance, and how two instance sets cross without a pop (the renderer has no cross-fade or dither between instance sets) — [ruins](../subsystems/ruins.md#the-block-layer)'s open question.
- **Which looks better.** The blocks are untextured and synthetic; E33's members are textured scan-quality geometry. No picture here judges either.
- **The baseline tier.** One GPU (RTX 5090, mesh shaders); the TITAN Xp's vertex path pays per pair and per visible cluster differently and was not run.
- **Shadows and ray tracing.** Shadows were off; a shadow cascade runs the visible list again per cascade, and a ray-traced frame builds a structure per visible entry, so both would multiply what the visible pairs cost, which these rows do not.

## Caveats

- **A busy machine**, as the header says: every flythrough row is an upper bound, and one (100 mid buildings) ran with every CPU taken by another process; its per-pair GPU cost is within 5% of its neighbour's.
- **One path, one seed.** The desert overlook's path passes most buildings at hundreds of metres and none closer than tens; a path through a ruin would draw mid and high blocks at their finer levels and raise their visible pairs, which these rows barely exercise (the mid and low rows keep the same visible pairs).
- **The distance check is one building**, never closer than 190 m on this path, with occlusion off.
- **GPU memory across representations is not comparable** (the E33 atlas duplication); within one it is. Since 2026-09-25 the duplication is gone, and the footnote under the first table has the E33 row without it.
- **Synthetic blocks, synthetic ruin rules.** The blocks are the kit's sizes and E33's course height, with the block layer's default rules (half the stones of a building fall, and all the fallen lie beside the walls: `debris_kept` 1); a real block kit, or rules that bury more debris, move the block counts.
