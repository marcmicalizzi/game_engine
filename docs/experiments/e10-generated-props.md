# E10: how much of what a generator makes can the engine take as it comes

- **Question ([10 §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing)):** the pass rate of ML-generated props through the validators, and the repair yield — which 07 §7.7 asks for before the pipeline may depend on generation ("measure the pass rate before depending on it"). The **first pass** (2026-09-22) is 20 props from one image model and one image-to-3D service, where the row asks for 200 across services; the **second pass** (2026-09-23, [below](#pass-2-the-same-twenty-remeshed-to-a-triangle-budget)) is the same twenty images remeshed to a triangle budget, beside the owner's Tripo run at the same budgets and a mechanical repair of the atlas fault the first pass found; the owner's two local TRELLIS.2 meshes and the post-processing sweep [follow](#local-generation-the-owners-two-trellis2-samples); then [the collapse check](#the-collapse-check-how-far-the-lod-collapses) the second pass asked for, calibrated and applied to every set, and [TRELLIS.2 from the same twenty images](#trellis2-from-the-same-twenty-images) at the same budgets, the third generator on the one image set; and, 2026-09-24, [the pipeline's own atlas](#repack-the-pipelines-own-atlas) — the repair the collapse check pointed at — over all three sets.
- **Date:** 2026-09-22. **Machine:** Intel Core i9-10980XE (18 cores, 36 threads), 64 GB, Windows 11 Pro 26200; **GPU:** RTX 5090, driver 610.88, Vulkan. **Build:** `msvc-release` at `5d57bb7` (`engine-content`, `engine-view`, `engine-image`, copied by the harness).
- **Machine state:** shared and busy, WARNING raised on all 40 capture runs of each measured pass. Of the first pass's two harness runs, the second (the one reported): other processes at **10–46% of the CPU**, the GPU **7–96% busy** with 7.6–8.0 GB of 32.6 GB in use, the GPU lock free at the start and held at the end by another agent timing a morph stage. The first run: others at **12–100%** of the CPU, the GPU 5–16% busy. **The picture metrics and the container bytes were identical in both passes, all 20 assets, to the last digit**, so they do not depend on load; **the build milliseconds are upper bounds** — one asset's build took 3× as long in the first pass as in the second, under a 100% CPU spike from another process. The image batch held the GPU lock; the captures did not need it ([content-generation](../content-generation.md#the-e10-harness-toolse10-harnessps1)).
- **Decision:** the E10 row in 10 §10.5 is **Measured** (not Done: 80 generated props of the 200, two services and one local model; the repair yield is measured for one repair), and 07 §7.4 carries status notes on which validators the pipeline needs first. The second pass revised the first on two points, marked where they occur: the "zero-area triangles" were points only as stored, and density is the failure only at the extremes. The collapse check revised the second: its 17 of 20 is **11 of 20**, marked where it occurs.

## Setup

**Subjects.** Twenty Desert Survival props ([13 §13.1](../plan/13-reference-consumer-games.md#131-consumer-a--desert-survival)), general-audience, no people and no creatures beyond bones: three kinds of rock, two cacti, a dead tree, driftwood, a cattle skull and an animal's rib bones, a clay water jar, a leather water skin, a rope coil, a wooden crate, a canvas tent bundle with its pegs, an oil lamp, a rusted tin cup, a well bucket, a cart wheel, a broken pot and a bleached signpost. The list, with a category and a rough size per prop for the scale validator that does not exist yet, is [content/generation/e10-desert-props.json](../../content/generation/e10-desert-props.json).

**Images.** Each subject wrapped in the template `image-to-3d/1` — *one object, isolated and centred, the whole object filling about 85% of a square frame, three-quarter view from slightly above, plain light-gray background, soft even light, no cast shadow, no text, no logo, no people, product photograph* — through the owner's Krea 2 turbo workflow in ComfyUI at 1024×1024, 8 steps, CFG 1, a seed derived from each name. Twenty images in **90 s** under one GPU lock (about 4.1 s an image once the model is resident), and ComfyUI's memory freed afterwards. All twenty were usable inputs; the signpost's post is cut off by the bottom of the frame. **Regenerating one from its sidecar gave a byte-identical PNG.**

**Meshes.** Meshy's `POST /openapi/v1/image-to-3d` with the body the owner's first task was verified with — `{"ai_model":"latest","should_texture":true,"enable_pbr":true,"topology":"triangle"}`, the image as a `data:` URI — five tasks in flight at a time. **All 20 succeeded on the first attempt, 30 credits each: 600 credits, balance 1,235 → 635** (the floor is 165 and this agent's cap was 700). A task took **100 s median** on the service (63 s for the signpost, 435 s and 592 s for the two meshes of six and seven million triangles). The service does not report what `latest` resolved to, so every sidecar records the alias and the date. Each GLB carries one material and three textures (base colour, metallic-roughness, normal), like the characters before them.

**Tripo.** The same twenty images, byte for byte, are in the owner's manifest (`D:\workspace\game_engine_local\generated\tripo\manifest-2026-09-22.json`, with a README saying which image to upload under which name and where to drop the GLBs); `tools/generate.ps1 ingest` takes them back when they exist.

**The measurement.** `tools/e10-harness.ps1` over the twenty GLBs: `engine-content build` and `stats`, then `engine-view` twice from the same framing orbit (640×640, `--orbit 16 --frames 4 --shadows off`) at the default LOD threshold of **1 px** and at **0.05 px** (effectively the leaves), then `engine-image compare`. Pass means **0 warnings**, **smallest atlas island ≥ 1 texel of a 4096 atlas**, and **coarse-against-finest FLIP ≤ 0.02** — the value that passes every known-good build and fails every known-bad one in the seam fix's measurements ([geometry](../subsystems/geometry.md#what-the-simplifier-is-given-and-why): defective 0.0241 and 0.0223, fixed 0.0115 and 0.0083, FlightHelmet 0.0173).

```powershell
pwsh tools/generate.ps1 image -Workflow D:\workspace\game_engine_local\workflows\krea2-turbo.json -Subjects content/generation/e10-desert-props.json
pwsh tools/generate.ps1 manifest -Backend tripo-folder -Images D:\workspace\game_engine_local\generated\comfyui\2026-09-22
pwsh tools/generate.ps1 3d -Backend meshy -Images D:\workspace\game_engine_local\generated\comfyui\2026-09-22 -Yes
$names = ((Get-Content -Raw content/generation/e10-desert-props.json | ConvertFrom-Json).subjects.name) -join ','
pwsh tools/e10-harness.ps1 -Folder D:\workspace\game_engine_local\generated\meshy\2026-09-22 -Only $names
```

(`-Only` because the same folder holds an earlier probe of the coordinator's, which is not part of the set.)

## Results

**Pass rate: 6 of 20 (30%).** Nothing failed to import and nothing raised a warning: every failure is one of the two checks the existing validators do not make.

| prop | triangles | islands | seam % | smallest island, texels (triangles) | median island, texels | container MB | build s | PSNR dB | FLIP (object only) | result |
|---|---|---|---|---|---|---|---|---|---|---|
| animal-ribcage | 1,464,002 | 1,140 | 15.4 | 1.01 (1) | 6,732 | 84.8 | 7.3 | 33.1 | 0.0237 (0.122) | fail: flip |
| barrel-cactus | 7,551,206 | 13,858 | 20.8 | 0 (18) | 168 | 413.0 | 47.7 | 25.6 | 0.0817 (0.159) | fail: island, flip |
| basalt-slab | 238,514 | 332 | 13.3 | 1.7 (1) | 21,400 | 20.1 | 0.9 | 37.6 | 0.0165 (0.044) | pass |
| broken-pot | 113,068 | 327 | 20.3 | 9.41 (1) | 24,979 | 10.9 | 0.4 | 42.3 | 0.0103 (0.023) | pass |
| canvas-tent-bundle | 2,138,398 | 2,136 | 14.0 | 0 (1) | 858 | 117.8 | 10.2 | 32.5 | 0.0432 (0.089) | fail: island, flip |
| cart-wheel | 679,864 | 1,060 | 15.1 | 0 (1) | 4,762 | 42.8 | 2.8 | 41.5 | 0.0066 (0.048) | fail: island |
| cattle-skull | 775,300 | 511 | 12.4 | 0 (1) | 11,875 | 47.4 | 3.2 | 36.9 | 0.0140 (0.071) | fail: island |
| clay-water-jar | 81,934 | 226 | 24.8 | 28.96 (1) | 43,794 | 9.2 | 0.3 | 43.0 | 0.0102 (0.024) | pass |
| dead-tree | 384,686 | 420 | 16.6 | 8.31 (1) | 18,006 | 28.1 | 1.5 | 35.6 | 0.0128 (0.090) | pass |
| driftwood-log | 1,122,166 | 562 | 9.8 | 2 (1) | 14,039 | 65.3 | 4.5 | 33.7 | 0.0250 (0.080) | fail: flip |
| eroded-limestone | 2,088,158 | 1,449 | 14.1 | 0 (10) | 2,834 | 114.9 | 9.7 | 36.8 | 0.0302 (0.061) | fail: island, flip |
| leather-water-skin | 836,676 | 823 | 12.8 | 1.75 (2) | 6,284 | 50.3 | 3.3 | 37.7 | 0.0151 (0.066) | pass |
| oil-lamp | 1,013,104 | 1,089 | 14.5 | 0 (1) | 4,148 | 60.1 | 4.6 | 35.2 | 0.0166 (0.065) | fail: island |
| rope-coil | 6,417,470 | 1,958 | 9.2 | 0 (1) | 3,052 | 332.2 | 37.1 | 31.4 | 0.0652 (0.099) | fail: island, flip |
| saguaro-cactus | 1,225,482 | 912 | 11.4 | 0 (1) | 4,251 | 68.5 | 5.6 | 36.5 | 0.0104 (0.111) | fail: island |
| sandstone-boulder | 573,788 | 220 | 9.5 | 0 (1) | 38,137 | 34.9 | 2.4 | 35.6 | 0.0387 (0.072) | fail: island, flip |
| signpost | 191,036 | 2,792 | 41.7 | 0 (4) | 1,038 | 17.9 | 0.8 | 41.2 | 0.0069 (0.044) | fail: island |
| tin-cup | 232,284 | 398 | 17.3 | 14 (1) | 21,494 | 19.0 | 0.9 | 40.1 | 0.0108 (0.033) | pass |
| well-bucket | 1,308,642 | 2,532 | 17.3 | 0 (1) | 351 | 76.3 | 6.0 | 34.8 | 0.0247 (0.059) | fail: island, flip |
| wooden-crate | 413,152 | 1,509 | 18.0 | 0 (1) | 2,877 | 29.1 | 1.6 | 35.8 | 0.0251 (0.052) | fail: island, flip |

| column | mean | worst | best |
|---|---|---|---|
| triangles | 1,442,447 (median ~806,000) | 7,551,206 barrel-cactus | 81,934 clay-water-jar |
| clusters | 34,542 | 187,302 barrel-cactus | 2,100 clay-water-jar |
| DAG levels | 13.7 | 16 barrel-cactus, rope-coil | 11 broken-pot |
| warnings | 0 | 0 | 0 |
| islands | 1,713 | 13,858 barrel-cactus | 220 sandstone-boulder |
| seam fraction | 16.4% | 41.7% signpost | 9.2% rope-coil |
| smallest island, texels | 3.4 | 0 (twelve props) | 29.0 clay-water-jar |
| container | 82.1 MB (1.64 GB for the twenty) | 413.0 MB barrel-cactus | 9.2 MB clay-water-jar |
| build (upper bound) | 7.5 s | 47.7 s barrel-cactus | 0.3 s clay-water-jar |
| PSNR, coarse vs finest | 36.3 dB | 25.6 barrel-cactus | 43.0 clay-water-jar |
| FLIP, coarse vs finest | 0.0244 | 0.0817 barrel-cactus | 0.0066 cart-wheel |
| FLIP over the object alone | 0.071 | 0.159 barrel-cactus | 0.023 broken-pot |

**Why the fourteen failed.** Five fail the island check alone, two the FLIP check alone, seven both.

- **Island, 12 props — and two different defects under one threshold.** In **8** the atlas is sound (median island 2,800–38,000 texels, seams 9–18%) and the sub-texel island is **one triangle whose three UVs coincide** (ten triangles for the limestone): a UV-hygiene fault, repaired by folding those triangles into the neighbouring island — no repack, no rebake. *(Corrected by the second pass: the UVs coincide only in the container, which stores them as half floats; in the source files not one of these triangles has zero UV area. The repair now in the build folds every island under a texel as stored, and lifted this pass from 6 to 11 of 20.)* In **4** the atlas really is fragmented: the barrel cactus (13,858 islands, median 168 texels), the well bucket (median 351), the canvas bundle (median 858) and the signpost (41.7% of its vertices on a seam).
- **FLIP, 9 props — all of them dense.** Every FLIP failure has more than 400,000 triangles, and across the twenty the object-only FLIP follows **log triangle count at r = 0.81** while it follows the **seam fraction at r = −0.29** and the median island at −0.49. The heat maps say the same thing as the correlation: the error is spread over the surface rather than along seams. On the crate the coarse cut loses the rivet heads of the iron brackets and lays a darker shading wedge across a plank; on the barrel cactus the spines thicken and lose their tips; on the rope the fibre twist smooths out. That is ordinary LOD behaviour on meshes whose detail lives in the geometry: the cut keeps positions within a pixel, but the normals it interpolates, and features smaller than a pixel's error, move — and 07's repair chain names the fix, *decimate and bake*, which puts that detail into the normal map where a coarse level keeps it.
- **Ten would pass after the cheap repair alone** if it worked as the diagnosis says: cart wheel, cattle skull, oil lamp and saguaro fail only on a single degenerate UV triangle. That is the estimated repair yield of a UV cleanup pass; nothing was repaired, so it is not a measured one.

**The best two.** The **clay water jar** (82,000 triangles, 226 islands, smallest 29 texels, a 9 MB container built in 0.3 s) and the **broken pot** (113,000 triangles, 327 islands, 11 MB) are the cleanest in every column, and their coarse cuts are hard to tell from their leaves: PSNR 43.0 and 42.3 dB, a faint faceting of the jar's glossy body in the heat map and nothing else. Smooth, closed, single-material ceramics are what this service does best, and what the engine handles best: low triangle counts because there is no surface detail to spend them on, and a few large islands.

**The worst two.** The **barrel cactus** is 7.55 million triangles — hundreds of spines, every one modelled as geometry — in a **413 MB** container that took 48 s to build, with 13,858 atlas islands of a median 168 texels and an 18-triangle island of zero area. Its coarse cut (4,945 pairs against 37,950 at the finest) thickens and blunts the spines everywhere, PSNR 25.6 dB, FLIP 0.082. The **rope coil** is 6.4 million triangles and 332 MB for an object the subject list sized at half a metre; its atlas is fine (median island 3,052 texels, 9.2% seams) and its coarse cut is still FLIP 0.065, because the twist of every strand is geometry that the cut smooths. Both would be a few tens of thousands of triangles and a normal map in a hand-made asset.

**What the numbers do not see.** Judged by eye from the captures and Meshy's thumbnails, nineteen of the twenty are the object that was asked for; the signpost's second board came back as a small bent stub (the input image had cropped the post and partly hidden that board). No validator here could have said so — that is 07 §7.4's visual rule (distance to a reference), or a person.

## What surprised me

1. **The failure is density, not the atlas.** The two Meshy characters made atlas fragmentation the expected failure ([10 §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing), E10 row), and the harness's thresholds were built around it. On props it is the minority case: the typical atlas here (220–2,800 islands, 9–25% seam vertices) is no worse than the FlightHelmet's (239 islands, 27.5%), and the picture error barely follows it. What the picture error follows is the **triangle count** — a median of about 800,000 triangles for props the list sized at 0.1–4 m, and **1.64 GB of containers for twenty props**.
2. **Nothing the existing validators check ever fired.** Zero import failures and zero warnings over 29 million triangles: Meshy's output is structurally clean. Everything that matters about it is a property the build does not yet report as a warning.
3. **"The smallest island rounds to zero" was usually one triangle.** That finding carried over from the characters — twelve of twenty have an island of no area — but in eight of them it is a single UV-degenerate triangle in an otherwise sound atlas, which is a different defect with a trivial repair. The characters' write-up did not record how many triangles their zero island had; it may have been the same thing.
4. **The whole-frame FLIP measures the framing as much as the asset.** Across the twenty it follows the object's share of the frame at r = 0.70; the object-only mean follows it at 0.02. The 0.02 threshold came from whole-frame numbers on characters of unknown coverage, so a prop that fills the frame (the sandstone boulder, 54%) is judged more harshly than a thin one (the saguaro, 9%, object-only FLIP 0.111 and a pass on this check).
5. **The service and the tools were the reliable part.** Twenty of twenty tasks succeeded first time at exactly 30 credits, images regenerate byte for byte from their sidecars, and two harness passes under different load agree to the last digit on every picture metric.

## What it decides

- **Which validator warnings the content pipeline needs first**, in order:
  1. **A triangle budget per category** (already a line of [07 §7.4](../plan/07-content-pipeline.md#74-validation-rules-automatic); the subject list now carries the categories). It is the dominant cost — memory, build time, and the picture error of every coarse level — and the repair it triggers, decimate and bake, is the head of 07 §7.7's repair chain.
  2. **A UV-degenerate rule** (a new `geometry.*` warning): triangles whose UV area is zero, with their count and surface share. Twelve of twenty would have raised it; the repair is mechanical.
  3. **Atlas fragmentation as a warning with a distribution, not a minimum**: how many islands are below N texels and how much of the surface they cover. The smallest island alone cannot tell one bad triangle from ten thousand bad islands.
- **Atlas fragmentation is not the common failure on generated props**, as it appeared to be from the two characters; it is real (four of twenty) and it compounds density (three of those four are also FLIP failures), but decimate-and-bake comes before repack in the repair chain.
- **The picture check should be judged over the object, not the frame**, before the threshold is used to accept or refuse anything automatically. Recalibrating it needs the seam-fix reference builds' coverage, which is one harness run over the owner's local reproducers.

It does **not** decide whether generated props are usable in the game — every pass here is an unrepaired asset at a triangle count no shipping prop would have — nor anything about Tripo or the local models, nor the repair yield, which needs the repair chain to exist.

## Caveats

- **Twenty props, one image model, one service, one set of Meshy options.** The row asks for 200 across services. Meshy was run as it was verified — no remesh — and its `should_remesh` with a `target_polycount` would attack the dominant failure directly; that is a second batch and the owner's decision, not this pass's.
- **One viewpoint.** The fixed orbit sees each prop from one side (the cart wheel edge-on), so a defect on the far side, or on a face seen edge-on, is invisible to the picture check. A second orbit angle per asset would halve that blind spot for two more captures each.
- **The FLIP threshold rests on three reference builds** and on a whole-frame mean (above). The island threshold is a hard line at one texel that a single degenerate triangle crosses.
- **Build milliseconds are upper bounds** from a machine whose other tenants used up to 46% of the CPU (100% in the first pass).
- **Semantic fitness was judged by eye**, once, from thumbnails and captures.

The generated images and meshes, their sidecars, the harness's reports, captures and heat maps are the owner's local data under `D:\workspace\game_engine_local\` (`generated\` and `e10\meshy-2026-09-22\`) and are not committed.

## Pass 2: the same twenty, remeshed to a triangle budget

- **Question.** The first pass read density as the common failure. The second tests that like for like: the same twenty images through the same service and model, with the one change the first pass named — Meshy's remesh to a **triangle budget per category**.
- **Date:** 2026-09-23. **Machine:** as above. **Build:** `msvc-release` of this change, on `7430bd8` — the harness records the base commit because the change was measured before it was committed. Two things in these binaries differ from the first pass's, the UV repair and the atlas-island measure ([geometry](../subsystems/geometry.md#uv-degenerate-triangles-the-repair)), so the first pass and the owner's Tripo run were **re-measured with the same binaries** (`e10\meshy-2026-09-22-repaired\`, `e10\tripo-2026-09-23-repaired\`), and every comparison below is within one build.
- **Machine state:** shared and busy. Over the five harness runs other processes used **8–100% of the CPU** and the GPU was **3–97% busy** with 6.8–19.6 GB of 32.6 GB in use, and another agent's TRELLIS.2 sweep held the GPU lock at the start and the end of every run (the captures do not take it). The picture metrics and the counts do not depend on load; the build seconds are upper bounds.
- **Decision:** "What the second pass decides", below.

**Budgets.** Each subject in [the subject list](../../content/generation/e10-desert-props.json) now names a `budget` class, and the list carries a `triangle_budgets` table: **hand 50,000** (tin cup, oil lamp, leather water skin, rope coil, broken pot, clay water jar, well bucket), **medium 120,000** (cattle skull, barrel cactus, canvas tent bundle, cart wheel, wooden crate, signpost, basalt slab, driftwood log, animal ribcage) and **large 250,000** (saguaro, dead tree, sandstone boulder, eroded limestone). Why those: all three sit at or under the harness's `-DenseTriangles` line of 250,000, above which a picture-error failure is diagnosed as detail carried by geometry; and the cluster LOD makes source density a disk and build cost rather than a frame cost, so a budget is about what the **coarse** cut can still represent, not about the finest cut. They are also the face limits the owner used for the Tripo run by hand the same day (triangles, PBR, 2K textures), so the two services meet at one budget.

**Meshes.** Meshy's openapi v1 as documented on 2026-09-23: `should_remesh` defaults to **false** for Meshy 6 and 7 (changelog, 2026-01-22) and `latest` has meant `meshy-7.1` since 2026-09-18 — so the first pass was `meshy-7.1` with **no remesh at all**, and every triangle it had was the reconstruction's; `topology` is read only when remeshing, so the first pass's `"topology":"triangle"` did nothing; `target_polycount` counts faces, 100 to 300,000, and is approximate. The request was the first pass's body plus `"should_remesh": true, "target_polycount": <the subject's budget>` (`tools/generate.ps1 3d -Backend meshy -Remesh -Subjects content/generation/e10-desert-props.json`; the budget class, the number and where it came from are in every sidecar and ledger line). **Twenty-one tasks, every one first time at 30 credits — a remesh costs nothing extra — 630 credits, balance 635 → 5**, which is all of it, as the owner authorized: the twenty, and one probe below. A task took **176 s median** on the service (125–493 s, the barrel cactus the slowest again). The triangle counts came back between 8.3% under and 4.4% over their targets: **2.48 million triangles for the twenty against the first pass's 28.9 million, and 376 MB of containers against 1,642 MB.**

```powershell
$names = ((Get-Content -Raw content/generation/e10-desert-props.json | ConvertFrom-Json).subjects.name) -join ','
$images = 'D:\workspace\game_engine_local\generated\comfyui\2026-09-22'
pwsh tools/generate.ps1 balance                                    # 635
pwsh tools/generate.ps1 3d -Backend meshy -Images $images -Only $names -Subjects content/generation/e10-desert-props.json -Remesh -MinBalance 0 -MaxCredits 635 -Yes
pwsh tools/generate.ps1 3d -Backend meshy -Images $images -Only tin-cup -Subjects content/generation/e10-desert-props.json -Remesh -Topology quad -Date 2026-09-23-quad -MinBalance 0 -MaxCredits 35 -Yes
pwsh tools/e10-harness.ps1 -Orbit 16 -Folder D:\workspace\game_engine_local\generated\meshy\2026-09-23 -Only $names
pwsh tools/e10-harness.ps1 -Orbit 16 -Folder D:\workspace\game_engine_local\generated\meshy\2026-09-22 -Only $names -Out D:\workspace\game_engine_local\e10\meshy-2026-09-22-repaired
```

(Every capture in this section is at the first pass's fixed `--orbit 16`, so the three sets compare with it and with each other; the harness now frames the whole bounds by default, which is a different picture, and `-Orbit 16` reproduces these. The first remeshed task, the tin cup, went alone to confirm the price and the triangle count before the other nineteen; the quad probe went to a folder of its own because the tin cup's name was taken in the day's.)
**Pass rate: 17 of 20 (85%)** on the harness's own criteria — against **11 of 20** for the first pass's meshes with the same build (6 of 20 without the repair) and **10 of 20** for Tripo's. Two fail on FLIP (the eroded limestone and the sandstone boulder, the two large rocks, both at 250,000) and one on the island check (the tin cup, whose last sub-texel island is among the repair's five unrepaired triangles). That 85% is misleading, and the reason is the most useful thing the second pass found (point 2 below). *(Revised by [the collapse check](#the-collapse-check-how-far-the-lod-collapses): re-judged with it, the same measurements pass **11 of 20** — the six that do not coarsen fail it — while the first pass's 11 and Tripo's 10 do not move. The result columns below keep the verdict as first judged, then the one now.)*

| prop | budget | triangles | islands | seam % | smallest island, texels (triangles) | median island, texels | container MB | build s | PSNR dB | FLIP (object only) | coarse / finest clusters | result |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| animal-ribcage | medium | 125,067 | 9,144 | 76.6 | 1 (1) | 62 | 21.5 | 0.9 | 35.3 | 0.0171 (0.077) | 0.75 | pass |
| barrel-cactus | medium | 120,509 | 48,793 | 96.4 | 1 (1) | 26 | 26.3 | 1.0 | identical | 0.0000 (0.000) | 1.00 | pass → **fail: collapse** |
| basalt-slab | medium | 123,840 | 1,835 | 43.3 | 1 (1) | 137 | 17.4 | 0.8 | 36.9 | 0.0169 (0.041) | 0.40 | pass |
| broken-pot | hand | 51,216 | 1,179 | 50.8 | 1 (1) | 616 | 10.2 | 0.3 | 43.3 | 0.0087 (0.026) | 0.51 | pass |
| canvas-tent-bundle | medium | 121,570 | 9,597 | 78.7 | 1 (1) | 16 | 19.5 | 0.9 | 38.8 | 0.0076 (0.016) | 0.91 | pass → **fail: collapse** |
| cart-wheel | medium | 121,463 | 10,297 | 77.0 | 1 (1) | 12 | 19.7 | 0.7 | 40.5 | 0.0055 (0.049) | 0.74 | pass |
| cattle-skull | medium | 124,695 | 4,749 | 62.0 | 1 (1) | 46 | 19.1 | 0.7 | 38.5 | 0.0099 (0.057) | 0.61 | pass |
| clay-water-jar | hand | 51,363 | 902 | 45.7 | 1.09 (1) | 286 | 9.4 | 0.3 | 42.0 | 0.0119 (0.027) | 0.67 | pass |
| dead-tree | large | 259,670 | 4,256 | 42.9 | 1 (1) | 14 | 26.4 | 1.5 | 36.5 | 0.0110 (0.076) | 0.33 | pass |
| driftwood-log | medium | 123,455 | 6,554 | 70.0 | 1 (1) | 36 | 20.8 | 0.9 | 36.3 | 0.0153 (0.055) | 0.74 | pass |
| eroded-limestone | large | 259,168 | 4,673 | 44.8 | 1 (1) | 17 | 26.7 | 1.9 | 35.4 | 0.0266 (0.065) | 0.52 | fail: flip |
| leather-water-skin | hand | 50,806 | 5,813 | 85.5 | 1 (1) | 39 | 14.5 | 0.4 | 37.4 | 0.0095 (0.035) | 0.86 | pass → **fail: collapse** |
| oil-lamp | hand | 50,355 | 6,275 | 86.4 | 1 (1) | 84 | 14.1 | 0.4 | 44.2 | 0.0022 (0.010) | 0.97 | pass → **fail: collapse** |
| rope-coil | hand | 52,177 | 13,555 | 95.9 | 5.23 (1) | 200 | 16.4 | 0.5 | identical | 0.0000 (0.000) | 1.00 | pass → **fail: collapse** |
| saguaro-cactus | large | 257,800 | 18,759 | 75.1 | 1 (1) | 27 | 29.4 | 2.3 | 44.0 | 0.0029 (0.046) | 0.70 | pass |
| sandstone-boulder | large | 258,744 | 2,355 | 34.4 | 1 (1) | 24 | 23.8 | 2.0 | 38.2 | 0.0244 (0.054) | 0.34 | fail: flip |
| signpost | medium | 123,016 | 718 | 33.5 | 2.84 (1) | 7,767 | 16.3 | 0.9 | 42.9 | 0.0062 (0.045) | 0.44 | pass |
| tin-cup | hand | 49,744 | 1,976 | 59.2 | 0 (1) | 66 | 11.8 | 0.4 | 40.2 | 0.0084 (0.022) | 0.69 | fail: island |
| well-bucket | hand | 48,456 | 4,548 | 84.8 | 1 (1) | 94 | 14.5 | 0.4 | 40.9 | 0.0041 (0.009) | 0.87 | pass → **fail: collapse** |
| wooden-crate | medium | 109,987 | 8,213 | 74.9 | 1 (1) | 14 | 18.7 | 0.7 | 40.4 | 0.0102 (0.022) | 0.68 | pass |

### Pass 1, pass 2 and Tripo, prop by prop

All three measured with the same build. "Coarse / finest clusters" is the share of the finest cut's visible clusters the coarse cut (1 px) still draws: how far the LOD collapsed.

| prop | triangles: pass 1 · pass 2 · Tripo | FLIP whole frame: pass 1 · pass 2 · Tripo | FLIP object only | coarse / finest clusters | islands | smallest island, texels | result: pass 1 · pass 2 · Tripo (→ with the collapse check) |
|---|---|---|---|---|---|---|---|
| animal-ribcage | 1,464,002 · 125,067 · 112,288 | 0.0237 · 0.0171 · 0.0239 | 0.122 · 0.077 · 0.082 | 0.16 · 0.75 · 0.52 | 1,140 · 9,144 · 299 | 1.01 · 1 · 18 | fail · pass · fail |
| barrel-cactus | 7,551,206 · 120,509 · 117,277 | 0.0943 · 0.0000 · 0.0332 | 0.149 · 0.000 · 0.075 | 0.15 · 1.00 · 0.68 | 13,230 · 48,793 · 2,435 | 1 · 1 · 4 | fail · pass → **fail** · fail |
| basalt-slab | 238,514 · 123,840 · 115,426 | 0.0165 · 0.0169 · 0.0104 | 0.044 · 0.041 · 0.036 | 0.26 · 0.40 · 0.27 | 332 · 1,835 · 66 | 1.7 · 1 · 140 | pass · pass · pass |
| broken-pot | 113,068 · 51,216 · 49,264 | 0.0103 · 0.0087 · 0.0163 | 0.023 · 0.026 · 0.044 | 0.38 · 0.51 · 0.37 | 327 · 1,179 · 66 | 9.41 · 1 · 80 | pass · pass · pass |
| canvas-tent-bundle | 2,138,398 · 121,570 · 119,244 | 0.0456 · 0.0076 · 0.0394 | 0.090 · 0.016 · 0.087 | 0.16 · 0.91 · 0.59 | 2,104 · 9,597 · 194 | 1 · 1 · 16 | fail · pass → **fail** · fail |
| cart-wheel | 679,864 · 121,463 · 113,662 | 0.0076 · 0.0055 · 0.0077 | 0.060 · 0.049 · 0.051 | 0.26 · 0.74 · 0.52 | 1,048 · 10,297 · 308 | 1 · 1 · 16 | pass · pass · pass |
| cattle-skull | 775,300 · 124,695 · 119,764 | 0.0140 · 0.0099 · 0.0131 | 0.070 · 0.057 · 0.077 | 0.19 · 0.61 · 0.40 | 510 · 4,749 · 242 | 2.6 · 1 · 24 | pass · pass · pass |
| clay-water-jar | 81,934 · 51,363 · 47,844 | 0.0102 · 0.0119 · 0.0257 | 0.024 · 0.027 · 0.047 | 0.45 · 0.67 · 0.61 | 226 · 902 · 47 | 28.96 · 1.09 · 154 | pass · pass · fail |
| dead-tree | 384,686 · 259,670 · 239,476 | 0.0128 · 0.0110 · 0.0130 | 0.090 · 0.076 · 0.089 | 0.15 · 0.33 · 0.17 | 420 · 4,256 · 188 | 8.31 · 1 · 21 | pass · pass · pass |
| driftwood-log | 1,122,166 · 123,455 · 119,368 | 0.0250 · 0.0153 · 0.0186 | 0.080 · 0.055 · 0.058 | 0.15 · 0.74 · 0.38 | 562 · 6,554 · 136 | 2 · 1 · 8 | fail · pass · pass |
| eroded-limestone | 2,088,158 · 259,168 · 248,306 | 0.0337 · 0.0266 · 0.0287 | 0.059 · 0.065 · 0.071 | 0.33 · 0.52 · 0.32 | 1,444 · 4,673 · 163 | 1.5 · 1 · 9 | fail · fail · fail |
| leather-water-skin | 836,676 · 50,806 · 48,567 | 0.0151 · 0.0095 · 0.0160 | 0.066 · 0.035 · 0.038 | 0.17 · 0.86 · 0.62 | 823 · 5,813 · 186 | 1.75 · 1 · 1 | pass · pass → **fail** · pass |
| oil-lamp | 1,013,104 · 50,355 · 48,798 | 0.0167 · 0.0022 · 0.0239 | 0.065 · 0.010 · 0.067 | 0.23 · 0.97 · 0.65 | 1,086 · 6,275 · 423 | 1.11 · 1 · 40 | pass · pass → **fail** · fail |
| rope-coil | 6,417,470 · 52,177 · 49,138 | 0.0560 · 0.0000 · 0.0002 | 0.100 · 0.000 · 0.000 | 0.18 · 1.00 · 1.01 | 1,955 · 13,555 · 96 | 1 · 5.23 · 96 | fail · pass → **fail** · pass |
| saguaro-cactus | 1,225,482 · 257,800 · 245,488 | 0.0103 · 0.0029 · 0.0147 | 0.118 · 0.046 · 0.109 | 0.11 · 0.70 · 0.19 | 910 · 18,759 · 141 | 1.06 · 1 · 18 | pass · pass · pass |
| sandstone-boulder | 573,788 · 258,744 · 235,606 | 0.0302 · 0.0244 · 0.0392 | 0.069 · 0.054 · 0.071 | 0.14 · 0.34 · 0.27 | 219 · 2,355 · 67 | 5.9 · 1 · 20 | fail · fail · fail |
| signpost | 191,036 · 123,016 · 111,853 | 0.0068 · 0.0062 · 0.0225 | 0.047 · 0.045 · 0.084 | 0.44 · 0.44 · 0.28 | 2,777 · 718 · 412 | 1 · 2.84 · 3 | pass · pass · fail |
| tin-cup | 232,284 · 49,744 · 47,182 | 0.0108 · 0.0084 · 0.0107 | 0.033 · 0.022 · 0.025 | 0.24 · 0.69 · 0.58 | 398 · 1,976 · 96 | 14 · 0 · 39 | pass · fail · pass |
| well-bucket | 1,308,638 · 48,456 · 47,736 | 0.0277 · 0.0041 · 0.0228 | 0.060 · 0.009 · 0.041 | 0.20 · 0.87 · 0.69 | 2,442 · 4,548 · 201 | 1 · 1 · 14.5 | fail · pass → **fail** · fail |
| wooden-crate | 413,150 · 109,987 · 102,764 | 0.0276 · 0.0102 · 0.0221 | 0.052 · 0.022 · 0.048 | 0.27 · 0.68 · 0.60 | 1,491 · 8,213 · 1,524 | 1.43 · 1 · 3 | fail · pass · fail |

**What the correlations say, over the twenty of each set** (Pearson r; the harness's `flip_follows`, plus the collapse share):

| against | pass 1 | Tripo | pass 2 |
|---|---|---|---|
| whole-frame FLIP ~ log triangles | 0.77 | 0.28 | 0.48 |
| object-only FLIP ~ log triangles | 0.81 | 0.72 | 0.68 |
| whole-frame FLIP ~ coverage | 0.72 | 0.42 | 0.15 |
| object-only FLIP ~ coverage | 0.00 | −0.52 | −0.56 |
| coarse share ~ seam fraction | 0.72 | 0.62 | **0.94** |
| whole-frame FLIP ~ coarse share | −0.37 | −0.11 | **−0.64** |
| median coarse share | 0.20 | 0.52 | **0.70** |

### What the second pass shows

1. **The remesh does what it says, at no extra cost.** Every task met its budget within 9%, at the same 30 credits, and the median container fell from 50 MB to 19 MB. The two extremes the first pass diagnosed as density are gone: the barrel cactus's 7.55 million triangles are 120,509, the rope coil's 6.4 million are 52,177, and neither carries its detail in geometry any more.

2. **Meshy's triangle remesh fragments the atlas so far that the LOD cannot coarsen the mesh — and the FLIP check passes a mesh that does not coarsen.** The remeshed atlases have 718 to 48,793 islands and 34% to 96% of their vertices on a seam (the first pass: 219 to 13,230 islands, 9% to 42%), and their median island is 12 to 616 texels on nineteen of the twenty (the first pass: 191 to 43,800). A seam vertex is one the LOD builder may not collapse across, so at the default threshold the coarse cut still draws a **median 70% of the finest cut's clusters** — against 20% for the first pass's meshes and 52% for Tripo's — and that share follows the seam fraction at **r = 0.94**. The barrel cactus and the rope coil draw **exactly** their finest cut at the coarse threshold, so their two captures are identical and they "pass" at FLIP 0.0000; four more draw 86–97% (the oil lamp, the canvas bundle, the well bucket, the leather water skin). Over the twenty, the whole-frame FLIP *falls* as the share rises (r = −0.64): **the check rewards a mesh for not simplifying**, because it compares a cut with the finest cut and a cut that did not coarsen has nothing to lose. Six of pass 2's seventeen passes are that; **eleven** pass and coarsen to at most three quarters of their finest cut. Tripo's rope coil (share 1.01, FLIP 0.0002) is the same case, which is why it passes. *(Revised by [the collapse check](#the-collapse-check-how-far-the-lod-collapses): Tripo's rope is not the same case. Its atlas is sound — 96 islands, 26% of vertices on a seam — and at this view its finest cut is sparse, 1.7 pairs per 1,000 object pixels against 4.0 to 13.6 for the six; it does not coarsen at a close view because its strands are geometry at the scale of the view, like a low-poly game asset's, not because its atlas stops the simplifier.)* The two TRELLIS.2 samples showed the same relation between one segmenter and another (below).

   **The quad probe says whose the fragmentation is.** The twenty-first task remeshed the same tin cup image with `topology: quad` at the same budget (25,000 quads, 45,747 triangles once triangulated; `e10\meshy-2026-09-23-quad\`): **89 atlas islands against 1,976, 19% of its vertices on a seam against 59%, a median island of 666 texels against 66**, a smallest island of 4 texels with nothing to repair, a 9.8 MB container against 12.4 MB, and a coarse cut of 59% of its finest against 69%; it passes at FLIP 0.0079. The fragmentation belongs to the *triangle* remesh — "a decimated triangle mesh", in Meshy's own description of that topology — and not to remeshing. The triangle remesh also splits its normals along every chart border it cuts (on the tin cup all 22,862 seam pairs differ in normal as well as UV; none of the first pass's 12,339 do), which is what made the repair take a neighbour's normal as well as its UV ([geometry](../subsystems/geometry.md#uv-degenerate-triangles-the-repair)).

3. **Density matters at the extremes, not in the middle.** The owner's Tripo run at the same budgets gives the comparison the first pass could not. For the same object, Tripo's whole-frame FLIP is close to Meshy's first-pass FLIP despite an eightfold difference in triangles — sandstone boulder 0.0392 against 0.0387, canvas bundle 0.039 against 0.043, eroded limestone 0.029 against 0.030, animal ribcage 0.0239 against 0.0237, each as first measured — while the two extreme-density props dropped hard: the barrel cactus from 7.5 million triangles to 117,000 went from 0.082 to 0.032, the rope coil from 6.4 million to 49,000 from 0.065 to 0.0002 (the latter partly point 2: Tripo's rope does not coarsen either). Across a set, the whole-frame FLIP follows log triangle count at r = 0.77 when the set spans 82,000 to 7.5 million triangles and at 0.28 to 0.48 when nothing is over 260,000. The residual FLIP failures are object- and coverage-driven: the eroded limestone and the sandstone boulder fail in all three sets, at 0.024 to 0.039, and they are the props that fill the most frame with the most fine relief. The object-only mean keeps following log triangles (0.68 to 0.81) for a different reason: it divides by coverage, and the densest props in every budget are the thin ones (the saguaro, the dead tree) whose silhouette is most of the object — object-only FLIP is *anti*-correlated with coverage in both budgeted sets (−0.52, −0.56), which is the division overcorrecting.

4. **The repair's yield, measured.** The first pass estimated that a UV cleanup would lift it from 6 to 10 of 20. Built with the repair, the same meshes pass **11 of 20**: every one of the twelve island failures is gone, and the five props that failed on the island alone (cart wheel, cattle skull, oil lamp, saguaro, signpost) pass; the other seven also failed on FLIP and still do. Props the repair did not touch reproduce their first measurement to the last digit; the ones it folded shift their FLIP a little either way (the barrel cactus from 0.082 to 0.094, the sandstone boulder from 0.039 to 0.030), because an island folded into its neighbour changes which collapses the simplifier may make.

   | prop | islands before · after | smallest before · after | FLIP before · after | result before · after |
   |---|---|---|---|---|
   | animal-ribcage | 1,140 · 1,140 | 1.01 · 1.01 | 0.0237 · 0.0237 | fail: flip · fail: flip |
   | barrel-cactus | 13,858 · 13,230 | 0 · 1 | 0.0817 · 0.0943 | fail: island, flip · fail: flip |
   | basalt-slab | 332 · 332 | 1.7 · 1.7 | 0.0165 · 0.0165 | pass · pass |
   | broken-pot | 327 · 327 | 9.41 · 9.41 | 0.0103 · 0.0103 | pass · pass |
   | canvas-tent-bundle | 2,136 · 2,104 | 0 · 1 | 0.0432 · 0.0456 | fail: island, flip · fail: flip |
   | cart-wheel | 1,060 · 1,048 | 0 · 1 | 0.0066 · 0.0076 | fail: island · pass |
   | cattle-skull | 511 · 510 | 0 · 2.6 | 0.0140 · 0.0140 | fail: island · pass |
   | clay-water-jar | 226 · 226 | 28.96 · 28.96 | 0.0102 · 0.0102 | pass · pass |
   | dead-tree | 420 · 420 | 8.31 · 8.31 | 0.0128 · 0.0128 | pass · pass |
   | driftwood-log | 562 · 562 | 2 · 2 | 0.0250 · 0.0250 | fail: flip · fail: flip |
   | eroded-limestone | 1,449 · 1,444 | 0 · 1.5 | 0.0302 · 0.0337 | fail: island, flip · fail: flip |
   | leather-water-skin | 823 · 823 | 1.75 · 1.75 | 0.0151 · 0.0151 | pass · pass |
   | oil-lamp | 1,089 · 1,086 | 0 · 1.11 | 0.0166 · 0.0167 | fail: island · pass |
   | rope-coil | 1,958 · 1,955 | 0 · 1 | 0.0652 · 0.0560 | fail: island, flip · fail: flip |
   | saguaro-cactus | 912 · 910 | 0 · 1.06 | 0.0104 · 0.0103 | fail: island · pass |
   | sandstone-boulder | 220 · 219 | 0 · 5.9 | 0.0387 · 0.0302 | fail: island, flip · fail: flip |
   | signpost | 2,792 · 2,777 | 0 · 1 | 0.0069 · 0.0068 | fail: island · pass |
   | tin-cup | 398 · 398 | 14 · 14 | 0.0108 · 0.0108 | pass · pass |
   | well-bucket | 2,532 · 2,442 | 0 · 1 | 0.0247 · 0.0277 | fail: island, flip · fail: flip |
   | wooden-crate | 1,509 · 1,491 | 0 · 1.43 | 0.0251 · 0.0276 | fail: island, flip · fail: flip |

   On the remeshed set the repair folded 16,040 islands (18,134 triangles) across nineteen props and took the island check from 1 of 20 to 19 of 20; on Tripo's, 7 islands in four props, from 16 to 20 of 20; on both TRELLIS.2 samples, 81 islands, from 0 to 2 of 2. It does not make a fragmented atlas sound and is not meant to: the island check is a floor, and a fragmented atlas that clears it is still fragmented, which is what point 2 is about.

5. **The FLIP line: an object-only threshold separates a little better, not well enough to judge by, and the default stays whole-frame 0.02.** The 0.02 line was drawn to separate one defect — coarse levels interpolating the texture across unrelated atlas islands — from its fix, on three reference builds whose framing coverage nobody recorded, and the first pass asked whether the object-only mean would do better. That defect can be reproduced on every prop here, because `engine-content build --uv-seams none --uv-weight 0` is the builder before the fix ([geometry](../subsystems/geometry.md#what-the-simplifier-is-given-and-why)); so each of the sixty props (the first pass's, Tripo's and the second pass's) gives a **known-bad** build and a **known-good** one, each coarse cut captured at the first pass's settings (640×640, `--orbit 16`) and compared with the clean build's finest capture:

   | metric | AUC, bad above good | best single line → share classified right | today's line |
   |---|---|---|---|
   | whole-frame FLIP mean | 0.72 | 0.03 → 67% | 0.02 → **65%** |
   | object-only FLIP mean (÷ coverage) | 0.78 | 0.09 → **74%** (0.07 in pass 1, 0.09 Tripo, 0.08 pass 2) | — |
   | whole-frame FLIP p95 | 0.78 | 0.13 → 75% | — |
   | CPU UV error, share of samples over 8 texels at a 25% budget (not a picture) | 0.75 (0.97 pass 1, 0.95 Tripo, 0.56 pass 2) | — | — |

   **Paired, the picture sees the defect every time**: the defective build's FLIP is above the clean build's for 58 of 60 props — the other two do not coarsen at all — by a median factor of 1.65 (1.05 to 8.2). **Unpaired, no single number can**: the spread between props, whole-frame FLIP 0.000 to 0.094 among the clean builds, is wider than the defect's effect. Object-only is better on the pooled numbers and still not a line to judge by. Its best threshold moves between sets, and it does not remove errors so much as move them: it fails the thin clean props (the saguaro, the dead tree, the ribcage — 9% to 29% of the frame, object-only 0.08 to 0.12) and passes compact defective ones (the clay jar, the tin cup and the broken pot built with the defect, 0.03 to 0.05). Applied to the three sets it would lift Tripo from 10 to 19 of 20, and the props it would pass are the sandstone boulder, the eroded limestone and the canvas bundle — the ones whose coarse cuts visibly lose their relief. So **the harness keeps whole-frame FLIP ≤ 0.02 as its default**, reports the object-only mean beside it as before, and the line is what it always was: a screen behind which a person reads the heat map, not a verdict. The measure that did separate the defect cleanly on sound atlases, the CPU UV error at a fixed budget, fails on the fragmented second pass, where the clean builds already score 0.2 to 0.93; and what made the second pass's picture checks pass for the wrong reason, the collapse share, is not a picture metric at all. (Calibration data: the owner's local `e10` notes, not committed.)

### What the second pass decides

- **A triangle budget per category works, as a request to the generator.** Both services meet it within 9%; the subject list carries it and `tools/generate.ps1` takes it from there ([content generation](../content-generation.md#meshy-and-the-credit-rules)). It removes the extreme densities, which were the only place density was the failure.
- **The validator the pipeline needs next measures how far the LOD collapses**, not how many islands an atlas has: the coarse cut's share of the finest at the harness's reference view, which the harness records (`coarse_visible_pairs`, `finest_visible_pairs`) but does not yet judge *(it does now: [the collapse check](#the-collapse-check-how-far-the-lod-collapses))*. A mesh that cannot coarsen passes every picture check the harness makes while costing its finest cut at every distance — six of the second pass's seventeen passes, and Tripo's rope *(which turned out to be another case: sound atlas, sparse on screen)*. The island distribution explains it (r = 0.94), and the repair is upstream, in the generator's settings (the quad remesh, TRELLIS.2's segmenter), before it is a repack.
- **Meshy's remesh should be asked for `topology: quad`** for anything the engine will draw at more than one distance: on the one prop tried, the same budget gave a sound atlas instead of the most fragmented of the three services'. One probe; the twenty with quads is the next batch, and it needs Meshy credits the owner has said they will not buy.
- **The UV-degenerate rule is a build step, not a warning** (`geometry.uv_degenerate`, [07 §7.4](../plan/07-content-pipeline.md#74-validation-rules-automatic) status note): the first pass's 6 of 20 becomes 11 of 20 with nothing done to the assets.
- **The FLIP line stays where it is**, for the reasons in point 5.

### Caveats (second pass)

- **One probe for the quad claim**, and one view for the collapse share: one orbit at 640×640 and a 1 px threshold. Closer, every mesh draws near its finest cut; farther, the gap between a sound and a fragmented atlas only widens.
- **The budgets were given, not derived.** The pass shows that a coarse cut can represent a prop at them, not that 50,000 triangles is right for a tin cup.
- **The known-bad builds are one defect.** The calibration says how well a threshold separates the seam defect from its fix; it says nothing about how well it separates a coarse cut that drops a rivet from one that does not, which is what the first pass's dense failures were.
- **Busy machine:** another agent's TRELLIS.2 sweep held the GPU lock through every run, and other processes reached 100% of the CPU. The pictures and the counts are unaffected; the build seconds are upper bounds.

The meshes, sidecars, the ledger, the five reports and the calibration captures are the owner's local data under `D:\workspace\game_engine_local\` (`generated\meshy\2026-09-23\`, `generated\meshy\2026-09-23-quad\`, and `e10\`) and are not committed.

## Local generation: the owner's two TRELLIS.2 samples

Two meshes the owner made on this machine with TRELLIS.2 (open weights, run locally through ComfyUI on the RTX 5090, from Krea 2 turbo images; [content generation](../content-generation.md)), measured by the same harness at the same settings (reports `e10\trellis-2026-09-22\` and `e10\trellis-2026-09-23\`, built at `6ea756f` and `3a7ddd5`). They are one tree each, not a set, and they differ in the one setting that turned out to matter: how TRELLIS.2's UV unwrap cuts the surface into pieces (its *segmenter*).

| | palm (`pec` segmenter, 700k face target) | bare tree (`adaptive` segmenter, 500k) |
|---|---|---|
| triangles | 683,939 | 499,892 |
| atlas islands | 27,429 | 1,818 |
| vertices on a seam | 87.1% | 30.4% |
| smallest island, texels | 0 (2 triangles) | 0 (1 triangle) |
| atlas used | 48.6% | 47.6% |
| coarse cut, share of the finest cut's clusters | **75%** | **23%** |
| FLIP whole frame / object only | 0.0386 / 0.070 | 0.0109 / 0.108 (object-only unreliable: the tree covers 10% of the frame) |
| time to generate on the 5090 | 276 s | — |
| harness | fail (island, flip) | fail (island) |

**The lesson is the segmenter, not the model.** The palm's `pec` unwrap cut the surface into 27,429 islands and put 87% of its vertices on a seam, and a seam is a vertex the LOD builder may not collapse across ([geometry](../subsystems/geometry.md#what-the-simplifier-is-given-and-why)), so its coarse cut at the default threshold still draws **three quarters** of its finest cut: the mesh cannot get cheaper with distance, which is the whole point of the cluster LOD. The bare tree's `adaptive` unwrap made 1,818 islands and 30% seams, and its coarse cut draws 23% — an ordinary LOD. **The segmenter decides how far the LOD can collapse**, and it is the same relation Meshy's triangle remesh shows across twenty props in pass 2 below (coarse share against seam fraction, r = 0.94). But `adaptive` is not a setting to switch on everywhere: its cost grows **faster than linearly with the number of surface pieces**, and on foliage — hundreds of separate leaves — it never finishes, which is why the palm was made with `pec`. And that points at the real conclusion for foliage: **a generated tree crown is the wrong asset class for a single mesh.** A crown is thousands of small separate parts, which is the worst case for an atlas and for a cluster DAG at once; the engine's answer is a trunk-and-branch mesh from the generator and the leaves as a foliage system (cards or instanced leaves), not a better unwrap of a single mesh.

Both islands of zero texels were the UV-degenerate kind: the build's repair (pass 2 below) folds them — the palm's 48 islands of under a texel (122 triangles) and the tree's 33 — and both then clear the island check. Re-measured with it (`e10\trellis-repaired-2026-09-23\`), the bare tree **passes** (FLIP 0.0121 whole frame) and the palm still fails on FLIP (0.042), and still draws 70% of its finest cut at the coarse threshold. The framing coverage of both moved between the builds the reports were made with (the palm from 55% of the frame to 42%) with the renderer's own changes, so rows are compared within one run. TRELLIS.2's own settings, the segmenter among them, are swept in the next section.

## TRELLIS.2 post-processing sweep

- **Question:** the owner's local TRELLIS.2 workflow ([content generation](../content-generation.md#text-to-mesh-in-one-workflow-comfyui-3d-trellis2-and-pixal3d)) makes a textured mesh from a prompt in minutes on the RTX 5090, at no marginal cost. Its post-process — remesh, decimate, unwrap, bake — decides whether the engine can take the result, and each setting has a price. Which settings, for a prop? The owner's two samples had suggested one answer: the palm (`pec` segmenter, 700,000 faces) came back with 27,429 atlas islands and 87.1% of its vertices on a seam, the bare tree (`adaptive`, 500,000) with 1,818 islands and 30.4% — but they were two different meshes, one of them foliage.
- **Date:** 2026-09-23. **Machine:** as above (i9-10980XE, 64 GB, RTX 5090, Windows 11). **Generator:** `tools/generate.ps1 3d -Backend comfyui-3d` (the sidecars name the tree it ran from, `7430bd8` plus the uncommitted backend that is now `tools/generate.ps1`), ComfyUI 0.37.1, PyTorch 2.10.0+cu130, the workflow at SHA-256 `7f37addd…`, `trellis_2_int8_convrot.safetensors`. **Measured with:** `tools/e10-harness.ps1` over `msvc-release` built from `80d2b73` (build stamp checked, none stale), fit framing (`--orbit 20.8`).
- **Machine state:** shared and busy; recorded per run in each sidecar (`machine_state`, sampled every 15 s) and quoted in the table. Whole-machine CPU ranged 6–100% during the runs, and ComfyUI itself kept 1.6 cores busy through a `pec` run and 7–15 through an `adaptive` one. The GPU lock was held for the whole batch on its behalf, so no other GPU job ran, but the owner's and other agents' CPU work did: the unwrap seconds of `adaptive` (CPU-bound, many-threaded) are upper bounds; the `pec` and GPU stages barely moved between runs.

### Setup

One subject, the E10 list's **wooden crate** (a hard-surface prop — planks, battens, iron corner brackets — whose Meshy version failed the first pass on an island and on FLIP), through the `image-to-3d/1` template, at the image seed the tool derives for that name (53052348199092), so that every run starts from the **same image**. The four TRELLIS samplers keep their fixed seeds, and the eight runs went through ComfyUI in one GPU-lock batch with the node cache warm: runs 2–8 reused the image and the reconstruction outright (44–50 nodes cached, recorded in each sidecar), so the geometry entering the post-process was identical **by construction**, not only by seed. From the backend's defaults (`adaptive`, 3 smoothing iterations, 100,000 faces, weld 0.0002, texture 2048), one factor at a time, with `pec` repeated where the segmenter might interact with the factor:

```powershell
pwsh tools/gpu-lock.ps1 run -Purpose "E10 TRELLIS.2 sweep" -Exec "pwsh -File sweep.ps1"   # eight `generate.ps1 3d -Backend comfyui-3d` runs, -NoFree on all but the last
pwsh tools/e10-harness.ps1 -Folder D:\workspace\game_engine_local\generated\trellis\2026-09-23 -Only <the eight> -Out D:\workspace\game_engine_local\e10\trellis-sweep-2026-09-23
```

### Results

| run | segmenter | smooth | faces | weld | triangles | islands | seam % | smallest island (texels) | median island (texels) | atlas used % | pairs coarse / finest | FLIP (object) | GLB MB | unwrap s | run s | CPU % min–max (mean); ComfyUI cores |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| base | adaptive | 3 | 100k | 0.0002 | 99,801 | 1,620 | 39.0 | 0 (1 tri) | 26 | 85.0 | 466 / 848 | 0.0168 (0.069) | 14.6 | 115.9 | 407 † | 13–100 (46); 7.0 |
| segmenter | **pec** | 3 | 100k | 0.0002 | 99,801 | **762** | 45.0 | 0.07 (1 tri) | **541** | 78.4 | 486 / 859 | 0.0139 (0.051) | 14.4 | **2.5** | 78 | 6–26 (20); 1.6 |
| weld | adaptive | 3 | 100k | **0.001** | 99,603 | 1,634 | 39.2 | 0 (1 tri) | 26 | 84.9 | 424 / 883 | 0.0145 (0.061) | 14.6 | 111.1 | 191 | 12–72 (45); 10.9 |
| weld | pec | 3 | 100k | **0.001** | 99,603 | 775 | 45.2 | 0.26 (2 tri) | 515 | 75.1 | 498 / 897 | 0.0113 (0.047) | 14.0 | 2.5 | 74 | 12–45 (26); 1.6 |
| faces | adaptive | 3 | **700k** | 0.0002 | 699,330 | 3,162 | 17.0 | 0 (1 tri) | 10 | 85.0 | 926 / 2,241 | 0.0172 (0.069) | 36.2 | **522.0** | 615 | 7–100 (56); 15.1 |
| faces | pec | 3 | **700k** | 0.0002 | 699,330 | 1,625 | 25.7 | 0 (1 tri) | 166 | 68.6 | 1,231 / 2,649 | 0.0141 (0.055) | 36.2 | 9.3 | 81 | 11–41 (20); 1.6 |
| smoothing | adaptive | **0** | 100k | 0.0002 | 98,789 | 1,636 | 39.0 | 0 (1 tri) | 20 | 82.7 | 423 / 820 | 0.0124 (0.064) | 14.6 | 96.3 | 202 | 16–73 (41); 10.2 |
| smoothing | adaptive | **20** (the workflow's) | 100k | 0.0002 | 99,794 | 1,678 | 39.3 | 0 (1 tri) | 22 | 83.8 | 476 / 889 | 0.0134 (0.058) | 14.4 | 103.8 | 211 | 11–70 (40); 10.3 |

† The one run with nothing cached: image 9.6 s, the four reconstruction samplers 148 s, remesh 19 s, decimate 4 s, unwrap 116 s, the colour, normal and occlusion bakes 74 s (the occlusion bake alone 52 s). Every other run skipped the first two, and those with the same smoothing skipped the remesh. Coverage was 19–27% in every run, so the object-only FLIP is usable here.

For reference, measured the same way (fit framing, the same binaries):

| mesh | triangles | islands | seam % | median island (texels) | atlas used % | pairs coarse / finest | FLIP (object) | GLB MB |
|---|---|---|---|---|---|---|---|---|
| Meshy's wooden crate (E10 first pass) | 413,152 | 1,509 | 17.9 | 2,877 | 61.4 | 315 / 1,195 | 0.0135 (0.055) | 19.5 |
| the owner's palm (TRELLIS.2, `pec`, 700k, 4096 texture) | 683,939 | 27,429 | 87.1 | 112 | 48.6 | 6,429 / 9,741 | 0.0345 (0.118) | 71.8 |
| the owner's bare tree (TRELLIS.2, `adaptive`, 500k) | 499,892 | 1,818 | 30.4 | 98 | 47.6 | 609 / 3,569 | 0.0073 (0.125, unreliable at 6% coverage) | 44.5 |

At the first pass's `--orbit 16` the palm measured FLIP 0.0386 and the tree 0.0109; the new framing shows each whole, so each is smaller in the frame and scores lower ([content generation](../content-generation.md#the-e10-harness-toolse10-harnessps1), "The framing").

### What it found

1. **On the same geometry, `adaptive` makes more and smaller islands than `pec`, not fewer.** 1,620 islands with a median of 26 texels against 762 with a median of 541 at 100,000 faces; 3,162 (median 10) against 1,625 (median 166) at 700,000. `adaptive` packs the atlas tighter (85% used against 68–78%) and puts fewer vertices on a seam (39% against 45%; 17% against 26%), but a median island of 26 texels in a 4096 atlas is about 13 texels square in the 2048 texture this prop carries, which no mip below the first can sample as itself. The owner's two samples pointed the other way because they were different meshes: the palm's 27,429 islands are its fronds, not its segmenter.
2. **`adaptive` costs 45–56× the unwrap time**: 96–116 s at 100,000 faces and 522 s at 700,000, on 7–15 CPU cores, against 2.5 s and 9.3 s for `pec`. A cached `pec` run was 74–81 s end to end whatever the face count; an `adaptive` one 191–211 s at 100k and 615 s at 700k. On this crate the unwrap grew 4.5× for 7× the faces — superlinear, but nothing like the palm crown, whose many separate pieces are what made `adaptive` run for hours.
3. **No run passes the island check, and every run passes FLIP.** All eight have a sub-texel island of one or two triangles (0 to 0.26 texels), and with medians below 1,024 texels the harness calls every atlas fragmented: whichever segmenter, the atlas needs the repair E10's first pass named (fold the degenerate triangles, repack) before this pipeline's props pass unattended. The coarse cut stays within FLIP 0.0113–0.0172 of the finest in all eight, at 100,000 faces and at 700,000 alike.
4. **100,000 faces costs nothing in the picture and saves 60% of the file**: FLIP 0.0113–0.0168 against 0.0141–0.0172 at 700,000, a 14.0–14.6 MB GLB against 36.2 MB, a 16 MB container against 48 MB. The coarse cut keeps 48–57% of the finest cut's pairs at 100k and 41–46% at 700k: a denser mesh simplifies further, but it has further to go.
5. **Smoothing and weld distance are in the noise** on this crate: 0, 3 or 20 Taubin iterations and a weld of 0.0002 or 0.001 move the islands by under 4%, the seams by under half a point and FLIP by ±0.002 with no direction. The tooltip's warning that many iterations round off sharp edges did not show in any number here; the crate's edges are soft to begin with.
6. **Against Meshy's crate**, TRELLIS.2 with `pec` at 100,000 faces has a quarter of the triangles, the same picture error (0.0139 against 0.0135) and a smaller file (14.4 MB against 19.5), but a worse atlas: half the islands, a fifth their median size, and 45% of vertices on a seam against 18%.

### A second prop, for the segmenter: the sandstone boulder

The crate overturned the reason the segmenter default was `adaptive`, so one smooth organic prop settled it: the E10 list's sandstone boulder, at its own derived seed (225752572791587), `pec` then `adaptive` with the second run's image and reconstruction cached, everything else at the defaults.

| boulder | islands | seam % | smallest island (texels) | median island (texels) | atlas used % | pairs coarse / finest | FLIP (object) | GLB MB | unwrap s | run s | CPU % min–max (mean); ComfyUI cores | E10 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| TRELLIS.2, **`pec`**, 100k | **237** | 38.3 | **35.4** (11 tri) | **10,618** | 51.2 | 393 / 774 | 0.0129 (0.048) | 14.5 | **1.4** | 171 (nothing cached) | 16–82 (32); 4.0 | **pass** |
| TRELLIS.2, `adaptive`, 100k | 354 | 25.8 | 2.8 (1 tri) | 312 | 48.1 | 353 / 716 | 0.0135 (0.048) | 13.1 | 42.7 | 100 | 10–62 (28); 8.5 | **pass** |
| Meshy (E10 first pass), for reference | 220 | 9.5 | 0 (1 tri) | 38,137 | 70.3 | 378 / 2,658 | 0.0216 (0.074) | 21.4 | — | — | — | fail: island, FLIP |

The same pattern as the crate: `adaptive` makes half as many islands again and a thirtieth the median size, in 30× the time, with fewer seam vertices its one gain. **Both boulders pass E10's checks** — the first TRELLIS.2 meshes to — at 100,000 triangles, where Meshy's boulder, at 574,000, fails the island check and, framed whole, FLIP as well (0.0216; 0.0387 at the first pass's closer framing).

### What it decides

- **`pec` is the backend's default segmenter.** Two props, a hard-surface one and a smooth organic one, with the geometry held identical: `pec` made fewer and larger islands on both, in 1.4–9.3 s against 43–522 s. `adaptive` stays available (and refused on foliage); its only measured advantage is fewer seam vertices, which the engine pays for in vertex duplication and not in picture error.
- **100,000 faces, texture 2048 and smoothing 3 stay** — the last on the node's own guidance, since the numbers do not choose between 0, 3 and 20.
- **The island check fails some TRELLIS.2 props for the reasons it failed most of Meshy's**: a degenerate UV triangle or two (the crate, under both segmenters), plus genuinely small islands. The UV-degenerate rule and the atlas repair E10's first pass asked for apply unchanged; a boulder shows the local generator can already pass without them.

### Caveats

- **Two props, one image and one set of reconstruction seeds each.** The segmenter's effect on the island count is large and in the same direction on both, which is enough to change a default, not to promise it for every shape; foliage is excluded by rule, and a thin prop (a pole, a signpost) is the case not measured.
- **Machine state**: the unwrap and run seconds of `adaptive` are upper bounds from a shared machine; the picture metrics and the counts do not depend on load.
- **The sidecars of this batch record `bake_seconds` as null**, from a bug fixed after it (a role id PowerShell handed back wrapped, which the timing table then failed to find); every bake node's own seconds are in `timings.nodes`, which is where the numbers above come from. *(Confirmed fixed: the boulder's two runs after it record 53.3 and 60.5 s, and every one of the twenty [below](#trellis2-from-the-same-twenty-images) records it.)*

The meshes, their sidecars and `runs.jsonl` are under `D:\workspace\game_engine_local\generated\trellis\2026-09-23\`, and the harness's reports, captures and heat maps under `e10\trellis-sweep-2026-09-23\` and `e10\trellis-boulder-2026-09-23\`; none is committed.

## The collapse check: how far the LOD collapses

- **Question.** The second pass found six meshes that pass every picture check because their LOD does not collapse — the coarse cut is the finest cut, so it has nothing to lose — while they cost their finest cut at every distance. The harness recorded the two cuts' visible pairs and judged neither. Where does a line on the coarse cut's share of the finest go, set from assets known to be good rather than by taste?
- **Date:** 2026-09-23. **Machine:** as above. **Build:** `msvc-release` at `921bf45` (build stamps checked, none stale; the harness's own changes are in `tools/`, which the binaries are not made of). **Framings:** 640×640, coarse `--lod 1` against finest `--lod 0.05`, at both framings E10's reports use: the three sets' fixed **orbit 16** and the harness's default **fit** framing (orbit 20.8, the whole bounds in frame), which puts the same object at about 0.6 of the area.
- **Machine state:** the Khronos runs on a quiet card (others 4.5–25.5% of the CPU, the GPU 3–18% busy, the lock free); the three sets re-captured at the fit framing while the TRELLIS.2 batch [below](#trellis2-from-the-same-twenty-images) held the GPU lock (others 7–85% of the CPU, the GPU 3–100% busy with 7.2–22.0 GB in use). The counts and the pictures do not depend on load.

**The known-good set.** The nine Khronos samples on this machine (`tools/fetch-samples.ps1`, in the main checkout's `content/samples`): Avocado, BoomBox, Corset, FlightHelmet, Fox, Lantern, RiggedFigure, SciFiHelmet and Suzanne — props, the two skinned samples in their bind pose, and hand-made game assets whose DAGs all collapse to a single root. Sponza is not in the pinned set, and the four morph samples were not fetched here; they are one or two clusters each, which the gate below would not judge anyway. The harness takes a `.gltf` beside its buffers now, as well as a `.glb` (reports `e10\khronos-samples-orbit16-2026-09-23\`, `e10\khronos-samples-fit-2026-09-23\`).

| sample | triangles | islands | seam % | orbit 16: coarse / finest pairs = share, at finest pairs per 1,000 object px | fit: the same | clusters per DAG level |
|---|---|---|---|---|---|---|
| Avocado | 682 | 3 | 21.2 | 7 / 7 = 1.00 at 0.1 | 8 / 8 = 1.00 at 0.1 | 8 4 2 1 |
| BoomBox | 6,036 | 20 | 22.0 | 49 / 61 = 0.80 at 0.6 | 42 / 62 = 0.68 at 1.0 | 68 39 20 11 6 3 2 1 |
| Corset | 18,324 | 98 | 29.4 | 91 / 134 = 0.68 at 1.3 | 88 / 163 = 0.54 at 2.7 | 203 113 62 34 19 10 5 3 1 |
| FlightHelmet | 94,722 | 237 | 27.5 | 396 / 860 = **0.46** at 6.3 | 307 / 866 = **0.35** at 11.7 | 1102 599 311 167 89 47 23 12 5 1 |
| Fox (skinned) | 576 | 14 | 60.4 | 8 / 8 = 1.00 at 0.1 | 8 / 8 = 1.00 at 0.2 | 8 5 3 2 1 |
| Lantern | 5,394 | 147 | 44.1 | 28 / 70 = **0.40** at 3.1 | 25 / 67 = **0.37** at 4.9 | 75 46 25 12 7 2 |
| RiggedFigure (skinned) | 256 | 1 | 0 | 6 / 6 = 1.00 at 0.1 | 6 / 6 = 1.00 at 0.2 | 6 3 2 1 |
| SciFiHelmet | 23,358 | 137 | 30.0 | 223 / 233 = 0.96 at 1.3 | 203 / 238 = 0.85 at 2.3 | 267 151 81 45 26 15 8 2 1 |
| Suzanne | 3,936 | 3 | 0 | 35 / 35 = 1.00 at 0.2 | 36 / 36 = 1.00 at 0.4 | 42 22 12 6 3 2 1 |

**The share alone cannot be the check.** Six of the nine known-good assets draw 80–100% of their finest cut at the coarse threshold at orbit 16, the SciFiHelmet 96% — a 23,000-triangle game asset with a sound atlas whose DAG collapses from 267 clusters to one. Nothing is wrong with them: at a close view their triangles are already several pixels each, and a 1 px threshold has nothing to shed. What tells them apart from a mesh that *cannot* coarsen is how dense the finest cut is on screen, which the harness now reports as **pairs per 1,000 pixels of the object** (`finest_pairs_per_kpx`, the finest cut's pairs over the object's coverage of the frame): the SciFiHelmet sits at 1.3 and 2.3 at the two framings, and every mesh the second pass showed not coarsening at **4.0 or more** (the well bucket at orbit 16; 4.7 to 13.6 for the rest). So the check is judged where the finest cut has at least **3** pairs per 1,000 object pixels (`-MinCollapseDensity 3`), between the two; below it the report says "too sparse to judge". (The DAG's own shape does not tell them apart either: rebuilt, the second pass's barrel cactus and rope coil also collapse to one root, 3,410 and 1,252 leaves. What stops them is the error each level carries, which only a view turns into pixels.)

**Where the line goes.** Over the sets, at the two framings:

| set | orbit 16: median share · judged · highest that passes · fails | fit: the same | pass rate at orbit 16, before → with the check | at fit |
|---|---|---|---|---|
| Khronos samples | 0.96 · 2 of 9 · 0.46 · none | 0.85 · 2 of 9 · 0.37 · none | 8 → 8 of 9 | 8 → 8 of 9 |
| E10 pass 1 (Meshy, no remesh) | 0.19 · 19 of 20 · 0.44 · none | 0.19 · 20 of 20 · 0.47 · none | 11 → 11 of 20 | 16 → 16 of 20 |
| Tripo (same budgets) | 0.52 · 14 of 20 · 0.68 · none | 0.34 · 19 of 20 · 0.60 · rope coil 0.84 | 10 → 10 of 20 | 17 → **16** of 20 |
| Meshy quad remesh (tin cup) | 0.59 · 0 of 1 · — · none | 0.44 · 1 of 1 · 0.44 · none | 1 → 1 of 1 | 1 → 1 of 1 |
| E10 pass 2 (Meshy triangle remesh) | 0.69 · 19 of 20 · 0.75 · **six**: barrel cactus 1.00, rope coil 1.00, oil lamp 0.97, canvas bundle 0.91, well bucket 0.87, leather skin 0.86 | 0.61 · 20 of 20 · 0.74 · **five**: barrel cactus 1.00, rope coil 1.00, oil lamp 0.95, leather skin 0.83, well bucket 0.79 | 17 → **11** of 20 | 19 → **14** of 20 |

(RiggedFigure fails the island check at both framings, as before the check: it carries no UVs, so its one island has no area. The fit framing passes more props than orbit 16 on FLIP alone — the object is smaller in the frame, and the whole-frame mean with it — which is the framing effect [the harness's page](../content-generation.md#the-e10-harness-toolse10-harnessps1) describes, not a change in the meshes.)

The judged known-good assets draw at most **0.46** (FlightHelmet at orbit 16); every E10 mesh not made by Meshy's triangle remesh draws at most **0.68** at orbit 16 (Tripo's barrel cactus, itself 54% seams); the six that do not coarsen draw at least **0.86**. The line is **0.75** (`-MaxCollapseShare`): between the highest that coarsens and the lowest that does not at the framing all three sets were measured at, and a statement a reader can use — *the LOD must shed at least a quarter of the finest cut at the reference view*. It passes every known-good asset at both framings, with the SciFiHelmet the one that needed the density gate; it fails exactly the six at orbit 16, and five of them framed whole, where the canvas bundle coarsens to 0.74 and slips under. Framed whole it also fails one mesh the second pass did not name — **Tripo's rope coil**, 0.84 at 3.2 pairs per 1,000 px — whose atlas is sound (96 islands, 26% seams): its strands are geometry at the scale of the view, the first pass's rope failure in a smaller form, and the diagnosis says the geometry is why rather than blaming the atlas.

### What the check decides

- **The harness judges the collapse** (`checks.collapse`, `collapse_share`, `finest_pairs_per_kpx`; [content generation](../content-generation.md#the-e10-harness-toolse10-harnessps1)), and `-FromReport` applied it to every E10 report on this machine: the second pass's **17 of 20 becomes 11 of 20**; the first pass's 11 and Tripo's 10 do not move at orbit 16; of the owner's two local TRELLIS.2 samples, the palm as first measured (share 0.755) now also fails on the collapse, while the repaired palm (0.700) and every mesh of the post-processing sweep (0.41–0.57) pass it. The palm's is the foliage lesson again: its crown's fronds are the separate pieces, not the unwrap.
- **It separates what the second pass saw by eye**: Meshy's triangle remesh stops the LOD on six props of twenty; the quad remesh and Tripo on none at the E10 framing.
- **The share is a property of a mesh at a view.** A check that ignored the view would fail every low-poly game asset at a close framing; the density gate is what lets one line serve both framings, and it rests on one known-good asset (the SciFiHelmet) on the sparse side and one generated one (the pass-2 well bucket at 4.0) on the dense side.

### Caveats (the collapse check)

- **Nine known-good assets, of which two are dense enough to judge.** The line's lower bound (0.46) and the gate's (2.3) each rest on one or two samples; a dense hand-made prop set — Sponza's pieces, or the owner's own — would test both.
- **The known-bad side is one generator setting.** Six meshes from Meshy's triangle remesh define it; a mesh that does not coarsen for another reason (the rope framed whole) is caught by the same line, but none was chosen for it.
- **One view per framing, one resolution.** The share and the density both move with distance, resolution and threshold; the line is calibrated at 640×640 and 1 px against 0.05 px, and says nothing about any other setting.
- **Off Windows there is no coverage**, so no density, and every mesh is judged on its share alone.

The Khronos and fit-framing reports, captures and heat maps are under `D:\workspace\game_engine_local\e10\` (`khronos-samples-*-2026-09-23\`, `*-fit\`); every re-judged report keeps its earlier verdicts beside it as `report.pre-collapse.json`. None is committed.

## TRELLIS.2 from the same twenty images

- **Question.** Meshy and Tripo were given the twenty ComfyUI images; the owner's local TRELLIS.2 workflow draws its own. Given the same bytes and the same budgets, what does the local model make of them, beside the three sets already measured — and what does that say about the three generators at one budget on one image set?
- **Date:** 2026-09-23. **Machine:** as above; ComfyUI 0.37.1, PyTorch 2.10.0+cu130, the workflow at SHA-256 `7f37addd…`, `trellis_2_int8_convrot.safetensors`. **Generator:** `tools/generate.ps1 3d -Backend comfyui-3d -Images` ([image in](../content-generation.md#image-in-the-same-picture-every-service-was-given)) from `921bf45` plus this change's `tools/` (every sidecar says `tools_dirty: true`). **Measured with:** `tools/e10-harness.ps1` over `msvc-release` at `921bf45` (stamps checked, none stale), at orbit 16 like the three sets and at the fit framing.
- **Machine state:** the batch held the GPU lock throughout (20:10–21:32 UTC); per run, whole-machine CPU 0–100% (means 13–32%), the GPU 0–100% busy (means 20–69%), ComfyUI 1.5–5.2 cores. The fit-framing captures of [the collapse check](#the-collapse-check-how-far-the-lod-collapses) ran beside its first three runs, and the owner's Blender session was open (it honours the lock). The harness runs: others 2–43% of the CPU (up to 100% for the fit run, beside an `msvc-debug` build), the GPU 0–10% busy, the lock held by another agent's test suite (captures do not take it). Generation and build seconds are upper bounds; the counts and pictures are not load-dependent.

**The run.** The twenty images of `generated\comfyui\2026-09-22\`, byte for byte the ones Meshy and Tripo were given, each subject decimated to its class's budget (`-Budget`: 50,000 / 120,000 / 250,000), segmenter `pec` (the backend's default since [the sweep](#trellis2-post-processing-sweep)), texture 2048, smoothing 3, weld 0.0002, the four reconstruction samplers at their fixed seeds:

```powershell
$env:ENGINE_GPU_LOCK_OWNER = 'agent-e10-trellis'
pwsh tools/gpu-lock.ps1 run -Purpose "E10: 20 props through TRELLIS.2 from the E10 images" -Exec "pwsh -File trellis-images-batch.ps1"
#   inside: generate.ps1 3d -Backend comfyui-3d -Workflow <krea2-turbo-to-trellis2-or-pixal3d.json> -Images generated\comfyui\2026-09-22
#           -Subjects content/generation/e10-desert-props.json -Budget -Segmenter pec -TextureResolution 2048 -Date 2026-09-23-e10
#   the tin cup alone first with -NoFree, its sidecar checked, then the other nineteen; ComfyUI's models freed at the end
pwsh tools/e10-harness.ps1 -Folder generated\trellis\2026-09-23-e10 -Only <the twenty> -Orbit 16 -Out e10\trellis-2026-09-23-e10-orbit16
pwsh tools/e10-harness.ps1 -Folder generated\trellis\2026-09-23-e10 -Only <the twenty> -Out e10\trellis-2026-09-23-e10-fit
```

**Twenty of twenty came back first time, in 79 minutes of generation** (median 184 s, 82 s for the saguaro to 826 s for the rope coil; no marginal cost). Of the time, the reconstruction took 44% (median 57 s; 16.7 s to 493 s, growing with how much surface the object has — the rope's strands, the barrel cactus's spines, the canvas folds) and the colour, normal and occlusion bakes 40% (median 93 s, **recorded in every sidecar**: the sweep's null `bake_seconds` is fixed); remesh, decimation and the `pec` unwrap together about 8%. Seventeen met their budget within 1%; the rope came back 11% under, the driftwood 5% and the skull 4%. Every sidecar verifies against the schema, names its picture as its input and `derived_from`, and carries the image's own prompt. (One timing is misattributed: the broken pot's unwrap reads 0.01 s and its decimation 6.5 s, because a node's time is the gap between the socket's messages and the loop that reads them also samples the machine every 15 s; the sum of the three is right.)

**All four sets, one image set, at the E10 framing** (orbit 16, 640×640, 1 px against 0.05 px; reports `meshy-2026-09-22-repaired`, `meshy-2026-09-23`, `tripo-2026-09-23-repaired`, `trellis-2026-09-23-e10-orbit16`). Each cell is Meshy pass 1 · Meshy pass 2 · Tripo · **TRELLIS.2**; generation seconds are Meshy's service time (pass 1 · pass 2) and TRELLIS.2's on the 5090 — Tripo's were made by hand and are not recorded. Results are with the collapse check.

| prop | triangles | islands | seam % | coarse / finest pairs | FLIP, whole frame | GLB MB | generation s | result |
|---|---|---|---|---|---|---|---|---|
| animal-ribcage | 1,464,002 · 125,067 · 112,288 · **119,878** | 1,140 · 9,144 · 299 · **3,754** | 15 · 77 · 25 · **76** | 0.16 · 0.75 · 0.52 · **0.88** | 0.0237 · 0.0171 · 0.0239 · **0.0036** | 49.1 · 17.6 · 5.9 · **20.9** | 142 · 176 · **134** | fail: flip · pass · fail: flip · **fail: collapse** |
| barrel-cactus | 7,551,206 · 120,509 · 117,277 · **118,688** | 13,230 · 48,793 · 2,435 · **6,776** | 21 · 96 · 54 · **91** | 0.15 · 1.00 · 0.68 · **1.00** | 0.0943 · 0.0000 · 0.0332 · **0.0000** | 225.3 · 23.1 · 7.0 · **22.5** | 593 · 493 · **455** | fail: flip · fail: collapse · fail: flip · **fail: collapse** |
| basalt-slab | 238,514 · 123,840 · 115,426 · **119,998** | 332 · 1,835 · 66 · **146** | 13 · 43 · 12 · **26** | 0.26 · 0.40 · 0.27 · **0.64** | 0.0165 · 0.0169 · 0.0104 · **0.0113** | 14.4 · 14.0 · 6.8 · **14.0** | 82 · 131 · **125** | pass · pass · pass · **pass** |
| broken-pot | 113,068 · 51,216 · 49,264 · **49,552** | 327 · 1,179 · 66 · **1,846** | 20 · 51 · 20 · **75** | 0.38 · 0.51 · 0.37 · **0.95** | 0.0103 · 0.0087 · 0.0163 · **0.0029** | 8.0 · 8.4 · 3.3 · **11.5** | 90 · 128 · **251** | pass · pass · pass · **fail: collapse** |
| canvas-tent-bundle | 2,138,398 · 121,570 · 119,244 · **118,912** | 2,104 · 9,597 · 194 · **6,133** | 14 · 79 · 20 · **92** | 0.16 · 0.91 · 0.59 · **1.00** | 0.0456 · 0.0076 · 0.0394 · **0.0000** | 66.9 · 15.8 · 6.2 · **21.9** | 209 · 208 · **384** | fail: flip · fail: collapse · fail: flip · **fail: collapse** |
| cart-wheel | 679,864 · 121,463 · 113,662 · **119,262** | 1,048 · 10,297 · 308 · **2,052** | 15 · 77 · 25 · **65** | 0.26 · 0.74 · 0.52 · **0.71** | 0.0076 · 0.0055 · 0.0077 · **0.0035** | 26.6 · 16.0 · 6.8 · **18.4** | 90 · 155 · **173** | pass · pass · pass · **pass** |
| cattle-skull | 775,300 · 124,695 · 119,764 · **114,800** | 510 · 4,749 · 242 · **4,196** | 12 · 62 · 22 · **85** | 0.19 · 0.61 · 0.40 · **1.00** | 0.0140 · 0.0099 · 0.0131 · **0.0000** | 28.9 · 15.3 · 6.0 · **21.4** | 89 · 144 · **167** | pass · pass · pass · **fail: collapse** |
| clay-water-jar | 81,934 · 51,363 · 47,844 · **49,410** | 226 · 902 · 47 · **699** | 25 · 46 · 18 · **59** | 0.45 · 0.67 · 0.61 · **0.88** | 0.0102 · 0.0119 · 0.0257 · **0.0039** | 7.0 · 7.6 · 3.2 · **12.2** | 70 · 125 · **208** | pass · pass · fail: flip · **fail: collapse** |
| dead-tree | 384,686 · 259,670 · 239,476 · **249,782** | 420 · 4,256 · 188 · **2,160** | 17 · 43 · 14 · **62** | 0.15 · 0.33 · 0.17 · **0.47** | 0.0128 · 0.0110 · 0.0130 · **0.0112** | 18.6 · 19.2 · 10.5 · **24.1** | 92 · 168 · **106** | pass · pass · pass · **pass** |
| driftwood-log | 1,122,166 · 123,455 · 119,368 · **113,769** | 562 · 6,554 · 136 · **5,125** | 10 · 70 · 17 · **89** | 0.15 · 0.74 · 0.38 · **1.00** | 0.0250 · 0.0153 · 0.0186 · **0.0000** | 39.3 · 17.0 · 7.7 · **19.8** | 103 · 159 · **166** | fail: flip · pass · pass · **fail: collapse** |
| eroded-limestone | 2,088,158 · 259,168 · 248,306 · **248,046** | 1,444 · 4,673 · 163 · **5,021** | 14 · 45 · 12 · **66** | 0.33 · 0.52 · 0.32 · **0.87** | 0.0337 · 0.0266 · 0.0287 · **0.0049** | 64.6 · 19.4 · 10.6 · **24.3** | 181 · 214 · **211** | fail: flip · fail: flip · fail: flip · **fail: collapse** |
| leather-water-skin | 836,676 · 50,806 · 48,567 · **49,468** | 823 · 5,813 · 186 · **1,594** | 13 · 85 · 29 · **77** | 0.17 · 0.86 · 0.62 · **0.99** | 0.0151 · 0.0095 · 0.0160 · **0.0000** | 30.6 · 12.7 · 3.9 · **14.6** | 102 · 148 · **264** | pass · fail: collapse · pass · **fail: collapse** |
| oil-lamp | 1,013,104 · 50,355 · 48,798 · **49,630** | 1,086 · 6,275 · 423 · **1,738** | 15 · 86 · 39 · **83** | 0.23 · 0.97 · 0.65 · **1.00** | 0.0167 · 0.0022 · 0.0239 · **0.0000** | 35.3 · 12.4 · 3.7 · **15.5** | 100 · 185 · **147** | pass · fail: collapse · fail: flip · **fail: collapse** |
| rope-coil | 6,417,470 · 52,177 · 49,138 · **44,575** | 1,955 · 13,555 · 96 · **5,888** | 9 · 96 · 26 · **98** | 0.18 · 1.00 · 1.01 · **1.00** | 0.0560 · 0.0000 · 0.0002 · **0.0000** | 183.4 · 15.0 · 5.6 · **20.5** | 435 · 322 · **826** | fail: flip · fail: collapse · pass · **fail: collapse** |
| saguaro-cactus | 1,225,482 · 257,800 · 245,488 · **248,256** | 910 · 18,759 · 141 · **2,468** | 11 · 75 · 12 · **63** | 0.11 · 0.70 · 0.19 · **0.56** | 0.0103 · 0.0029 · 0.0147 · **0.0070** | 40.3 · 21.3 · 11.3 · **23.3** | 107 · 233 · **82** | pass · pass · pass · **pass** |
| sandstone-boulder | 573,788 · 258,744 · 235,606 · **249,967** | 219 · 2,355 · 67 · **350** | 9 · 34 · 9 · **34** | 0.14 · 0.34 · 0.27 · **0.52** | 0.0302 · 0.0244 · 0.0392 · **0.0234** | 21.4 · 16.8 · 9.0 · **19.3** | 99 · 222 · **130** | fail: flip · fail: flip · fail: flip · **fail: flip** |
| signpost | 191,036 · 123,016 · 111,853 · **119,938** | 2,777 · 718 · 412 · **483** | 42 · 34 · 23 · **42** | 0.44 · 0.44 · 0.28 · **0.42** | 0.0068 · 0.0062 · 0.0225 · **0.0101** | 12.6 · 12.9 · 7.5 · **16.3** | 63 · 174 · **172** | pass · pass · fail: flip · **pass** |
| tin-cup | 232,284 · 49,744 · 47,182 · **49,869** | 398 · 1,976 · 96 · **264** | 17 · 59 · 21 · **43** | 0.24 · 0.69 · 0.58 · **0.79** | 0.0108 · 0.0084 · 0.0107 · **0.0052** | 13.3 · 10.1 · 3.2 · **12.0** | 92 · 132 · **195** | pass · fail: island · pass · **fail: collapse** |
| well-bucket | 1,308,638 · 48,456 · 47,736 · **49,795** | 2,442 · 4,548 · 201 · **1,282** | 17 · 85 · 30 · **72** | 0.20 · 0.87 · 0.69 · **0.97** | 0.0277 · 0.0041 · 0.0228 · **0.0003** | 44.7 · 12.8 · 4.4 · **13.6** | 140 · 193 · **295** | fail: flip · fail: collapse · fail: flip · **fail: collapse** |
| wooden-crate | 413,150 · 109,987 · 102,764 · **119,993** | 1,491 · 8,213 · 1,524 · **681** | 18 · 75 · 44 · **39** | 0.27 · 0.68 · 0.60 · **0.57** | 0.0276 · 0.0102 · 0.0221 · **0.0201** | 19.5 · 15.4 · 6.5 · **14.6** | 88 · 201 · **274** | fail: flip · pass · fail: flip · **fail: flip** |

| set | median triangles | median islands (median island, texels) | median seam % | median coarse / finest | median FLIP | GLB MB, median (twenty) | container MB, median (twenty) | pass, orbit 16 (without → with the collapse check) | pass, fit |
|---|---|---|---|---|---|---|---|---|---|
| Meshy pass 1 (no remesh) | 805,988 | 979 (5,600) | 14.8 | 0.19 | 0.0166 | 29.7 (950) | 48.8 (1,642) | 11 → 11 | 16 → 16 |
| Meshy pass 2 (triangle remesh to budget) | 121,516 | 5,281 (43) | 72.4 | 0.69 | 0.0091 | 15.4 (303) | 18.9 (376) | 17 → 11 | 19 → 14 |
| Tripo (face limit at the budget) | 112,975 | 187 (29,106) | 21.8 | 0.52 | 0.0203 | 6.4 (129) | 9.4 (192) | 10 → 10 | 17 → 16 |
| **TRELLIS.2** (`pec`, decimated to the budget) | 118,800 | 1,949 (594) | 68.6 | **0.88** | **0.0035** | 18.8 (361) | 20.4 (397) | **18 → 5** | **20 → 8** |

### What it shows

1. **Image in works, and makes the comparison fair.** One command took the same twenty images through the same graph with the drawing nodes removed, and every sidecar says so; nothing about the reconstruction changed except where its picture came from.
2. **TRELLIS.2's atlas at these budgets fragments as far as Meshy's triangle remesh, and its LOD collapses least of the four.** A median of 1,949 islands and 69% of vertices on a seam (Meshy's remesh: 5,281 and 72%); **13 of 20 fail the collapse check** at orbit 16 and 12 framed whole, six of them drawing exactly their finest cut (the barrel cactus, the canvas bundle, the cattle skull, the driftwood, the oil lamp and the rope) and two more 97–99% of it (the well bucket, the leather skin). The share follows the seam fraction at r = 0.80 and 0.83 at the two framings — the same relation, and the same cause, as the second pass.
3. **Without the collapse check it would have been the best set.** 18 of 20 at orbit 16 and 20 of 20 framed whole, with the lowest median FLIP of the four (0.0035) — because the whole-frame FLIP *falls* as the seams rise (r = −0.82): the meshes that cannot coarsen have nothing to lose. With the check it is **5 of 20** at orbit 16 and 8 of 20 framed whole. This is the case the check was written for, met by the first set it was not calibrated on.
4. **The sweep chose `pec` on the two props it handles.** Its crate and boulder come out here as they did there — the crate 681 islands, 39% seams, a coarse cut of 0.57; the sandstone boulder 350 islands, 34%, 0.52 — and both coarsen. The other eighteen are the case the sweep did not try: thin, intricate or organic shapes (bone, rope, spines, folded cloth, driftwood), where the unwrap cuts thousands of islands whatever the budget. `pec` made fewer islands than `adaptive` on the crate and the boulder; it does not make few on these. So the next TRELLIS.2 sweep belongs on the cattle skull and the driftwood log, and on the unwrap's other settings (padding, the atlas resolution, unwrapping a coarser remesh and baking onto the budgeted mesh), not on the crate again.
5. **The three generators at one budget, on one image set, fail in two different ways.** **Tripo** gives the soundest atlas by an order of magnitude (median 187 islands, 22% seams, median island 29,106 texels), the smallest files (a median 6.4 MB GLB, 9.4 MB container), and a LOD that collapses on every prop — and the largest coarse-cut picture error, 10 FLIP failures at orbit 16 (3 framed whole), where a sound atlas at 50,000–250,000 triangles loses relief the finest cut shows: the first pass's density lesson at the budget. **Meshy's triangle remesh** and **TRELLIS.2** both fragment the atlas until the LOD stops (6 and 13 props of 20), which buys them the smallest picture errors for the wrong reason. Meshy unremeshed (pass 1) coarsens best of all (0.19), at seven times the triangles and four to eight times the container bytes. **No generator gives both a collapsing LOD and a clean coarse picture on most props**: the best pass rate at orbit 16 is 11 of 20 (both Meshy passes; Tripo 10), and framed whole 16 of 20 (Meshy pass 1 and Tripo). The repair chain is needed either way, and it is a different repair per generator — decimate and bake detail into the normal map for Tripo (07 §7.7's first repair), a sounder unwrap or a repack and rebake for Meshy's remesh and TRELLIS.2, which comes first, before anything the engine's builder can do.
6. **Cost.** TRELLIS.2 is free at the margin and took 79 minutes of the 5090 under the lock for the twenty (median 184 s a prop), against Meshy's 100 s (pass 1) and 176 s (pass 2) of service time at 30 credits a task; Tripo's time is the owner's. The local model's files are the largest of the budgeted three (a median 18.8 MB GLB at texture 2048, against Tripo's 6.4 MB at its 2K).

### What this decides

- **`pec` stays the default, with a status note, not a verdict.** It is still the better of the two segmenters on the geometry measured side by side; on the eighteen props of other shapes neither was tried, and the collapse check now says, per mesh, when an unwrap has stopped the LOD.
- **For generated props the unwrap is now the first thing to fix, for two of the three generators.** The collapse check measures it, the island distribution explains it, and the E10 report of every run carries both.
- **The E10 row stays Measured**: 80 generated props through the validators, two services and one local model, with the same twenty images.

### Caveats (TRELLIS.2 from the same images)

- **One set of TRELLIS.2 settings**: the backend's defaults at the three budgets. The unwrap is the variable that matters, and it was not swept here.
- **The int8 checkpoint** (`trellis_2_int8_convrot`) is a community conversion; the reconstruction a full-precision checkpoint makes may differ, and the provenance of the file is not recorded ([content generation](../content-generation.md#the-local-models-licences-as-published)).
- **Load.** The batch ran beside another agent's captures for its first three props and the owner's open Blender session; the generation seconds are upper bounds (the barrel cactus's 302 s reconstruction overlapped the captures).
- **One timing is misattributed** (the broken pot's unwrap, above); the per-node times are the gaps between socket messages, and a machine sample every 15 s can delay reading a burst of them by about a second.

The meshes, their sidecars, the images the reconstruction saw, the atlas renders and `runs.jsonl` are under `D:\workspace\game_engine_local\generated\trellis\2026-09-23-e10\`; the batch script and its log under `e10\trellis-images-batch-2026-09-23.*`; the two reports under `e10\trellis-2026-09-23-e10-orbit16\` and `e10\trellis-2026-09-23-e10-fit\`. None is committed.

## Repack: the pipeline's own atlas

- **Question.** The passes above ended on "the pipeline needs its own unwrap, repack and rebake, so that a generator's atlas stops deciding whether the engine's LOD can work". With one (`engine-content build --atlas repack`, [atlas](../subsystems/atlas.md)), how far does each set's LOD now collapse, against how far it could with no atlas constraint at all; what does the rebake cost the picture and the build; and which props can it not take?
- **Date:** 2026-09-24. **Machine:** as above. **Build:** `msvc-release` of this change on `85cd34c` (build stamps checked, none stale), and spot-checked after rebasing it onto `3d34c60`: eight props of the three sets gave the same collapse shares, seam fractions, FLIP and one-camera comparisons to the last digit, their containers larger only by the canonical vertex id section that commit added.
- **Machine state:** the repack runs on a quiet machine — others 0.2–22% of the CPU, the GPU 0–3% busy, the lock free at both ends; the own-atlas and ceiling runs beside this change's own debug builds, others 3–67% of the CPU, the GPU 1–10%. The counts and pictures do not depend on load; the build milliseconds of the second group are upper bounds.

**Three builds of every prop** of the three sets E10 compares on one image set — Meshy's triangle remesh (pass 2), Tripo, TRELLIS.2 — at orbit 16, 640×640, 1 px against 0.05 px (reports `e10\atlas-<set>-orbit16-{keep,ceiling,repack}\`):

```powershell
pwsh tools/e10-harness.ps1 -Folder <set> -Only <the twenty> -Orbit 16 -Out e10\atlas-<set>-orbit16-keep
pwsh tools/e10-harness.ps1 -Folder <set> -Only <the twenty> -Orbit 16 -Out e10\atlas-<set>-orbit16-ceiling -BuildArgs '--uv-seams none --uv-weight 0'
pwsh tools/e10-harness.ps1 -Folder <set> -Only <the twenty> -Orbit 16 -Out e10\atlas-<set>-orbit16-repack -Atlas repack -CompareKept
```

The **own atlas** is the build as before. The **ceiling** tells the simplifier there are no UV seams and no attribute error at all: it is how far the LOD could collapse if the atlas were no constraint, the bound any repack is working towards — and its pictures are wrong wherever it collapses across an island, so its FLIP is not a quality number. The **repack** is `--atlas repack` at its defaults, and `-CompareKept` also draws its finest cut and the own atlas's from one camera, with the renderer's orbiting lights off (below).

| set | pass rate: own → repack (ceiling) | median coarse / finest pairs: own → repack (ceiling) | props that do not coarsen | median seam vertices | median islands | coarse-vs-finest FLIP failures | declined by the gate | build, median ms: own → repack |
|---|---|---|---|---|---|---|---|---|
| Meshy, triangle remesh | 11 → 11 (6) | 0.69 → **0.46** (0.39) | 6 → 3 | 70% → 24% | 4,749 → 157 | 2 → 5 | 1 | 746 → 11,691 |
| Tripo | 10 → 10 (6) | 0.52 → **0.45** (0.38) | 0 → 0 | 21% → 21% | 186 → 131 | 10 → 10 | 1 | 493 → 10,529 |
| TRELLIS.2 | 5 → **8** (8) | 0.88 → **0.81** (0.72) | 13 → 10 | 66% → 40% | 1,846 → 732 | 2 → 2 | **9** | 620 → 11,934 |

Prop by prop, each cell is the coarse cut's share of the finest, own atlas → repack (ceiling in brackets), the seam fraction own → repack, and the result own → repack:

| prop | Meshy (triangle remesh) | Tripo | TRELLIS.2 |
|---|---|---|---|
| animal-ribcage | 0.75 → 0.67 (0.55); 77 → 33%; pass → pass | 0.52 → 0.59 (0.43); 25 → 26%; flip → flip | 0.88 → 0.81 (0.76); 76 → 40%; collapse → collapse |
| barrel-cactus | 1.00 → declined (1.00); collapse | 0.68 → declined (0.53); flip | 1.00 → declined (1.00); collapse |
| basalt-slab | 0.40 → 0.23 (0.20); 43 → 13%; pass → flip | 0.27 → 0.23 (0.15); 12 → 12%; pass → pass | 0.64 → 0.40 (0.41); 26 → 10%; pass → pass |
| broken-pot | 0.51 → 0.37 (0.29); 51 → 20%; pass → pass | 0.37 → 0.28 (0.22); 20 → 14%; pass → flip | 0.95 → declined (0.65); collapse |
| canvas-tent-bundle | 0.85 → 0.70 (0.57); 79 → 28%; collapse → flip | 0.59 → 0.66 (0.53); 20 → 23%; flip → flip | 1.00 → declined (0.95); collapse |
| cart-wheel | 0.72 → 0.44 (0.25); 77 → 24%; pass → pass | 0.52 → 0.49 (0.44); 25 → 21%; pass → pass | 0.71 → 0.67 (0.52); 65 → 40%; pass → pass |
| cattle-skull | 0.61 → 0.44 (0.32); 62 → 21%; pass → pass | 0.40 → 0.45 (0.33); 22 → 23%; pass → pass | 1.00 → declined (0.72); collapse |
| clay-water-jar | 0.67 → 0.55 (0.43); 46 → 15%; pass → pass | 0.61 → 0.54 (0.52); 18 → 13%; flip → flip | 0.88 → 0.91 (0.72); 59 → 45%; collapse → pass (too sparse to judge) |
| dead-tree | 0.33 → 0.22 (0.12); 43 → 18%; pass → pass | 0.17 → 0.18 (0.14); 14 → 19%; pass → pass | 0.47 → 0.32 (0.27); 62 → 29%; pass → pass |
| driftwood-log | 0.74 → 0.57 (0.39); 70 → 19%; pass → pass | 0.38 → 0.39 (0.33); 17 → 18%; pass → pass | 1.00 → declined (0.88); collapse |
| eroded-limestone | 0.52 → 0.46 (0.39); 45 → 13%; flip → flip | 0.32 → 0.37 (0.30); 12 → 14%; flip → flip | 0.87 → 0.69 (0.77); 66 → 26%; collapse → flip |
| leather-water-skin | 0.86 → 0.64 (0.54); 85 → 28%; collapse → pass | 0.62 → 0.59 (0.55); 29 → 31%; pass → pass | 0.99 → declined (0.92); collapse |
| oil-lamp | 0.97 → 0.88 (0.69); 86 → 36%; collapse → collapse | 0.65 → 0.65 (0.57); 39 → 33%; flip → flip | 1.00 → declined (0.92); collapse |
| rope-coil | 1.00 → 1.00 (1.00); 96 → 29%; collapse → collapse | 1.01 → 1.00 (0.97); 26 → 33%; pass → pass | 1.00 → declined (1.00); collapse |
| saguaro-cactus | 0.69 → 0.42 (0.13); 75 → 26%; pass → pass | 0.19 → 0.19 (0.14); 12 → 21%; pass → pass | 0.56 → 0.47 (0.37); 63 → 39%; pass → pass |
| sandstone-boulder | 0.34 → 0.21 (0.19); 34 → 6%; flip → flip | 0.27 → 0.25 (0.22); 9 → 8%; flip → flip | 0.52 → 0.38 (0.40); 34 → 8%; flip → flip |
| signpost | 0.44 → 0.26 (0.13); 34 → 12%; pass → pass | 0.28 → 0.23 (0.16); 23 → 17%; flip → flip | 0.42 → 0.28 (0.21); 42 → 19%; pass → pass |
| tin-cup | 0.69 → 0.61 (0.41); 59 → 26%; island → island | 0.58 → 0.55 (0.44); 21 → 15%; pass → pass | 0.79 → 0.72 (0.67); 43 → 25%; collapse → pass |
| well-bucket | 0.87 → 0.71 (0.59); 85 → 35%; collapse → pass | 0.69 → 0.66 (0.58); 30 → 34%; flip → pass | 0.97 → declined (0.90); collapse |
| wooden-crate | 0.68 → 0.39 (0.22); 75 → 26%; pass → flip | 0.60 → 0.41 (0.38); 44 → 21%; flip → flip | 0.57 → 0.57 (0.47); 39 → 29%; flip → pass |

**What the rebake costs the picture**, from the one-camera comparison of each repacked prop's finest cut with its own atlas's (declined props are not in it; they are their own atlas), and the build's own measure of each rebaked texture against its source on the surface:

| set | repacked | one camera: median FLIP (worst) · median PSNR (worst) | colour texture: median of the mean / p99 error, 8-bit levels | normal map: median of the mean / p99 error, degrees | folded triangles, median (worst) | PNG bytes, median |
|---|---|---|---|---|---|---|
| Meshy | 19 | 0.0069 (0.0206) · 46.7 dB (36.0) | 3.5 / 21 | 3.0 / 30 | 0.22% (1.8%) | 11.7 MB |
| Tripo | 19 | 0.0037 (0.0116) · 50.3 dB (36.5) | 0.9 / 6 | 0.6 / 11 | 0.02% (0.5%) | 9.2 MB |
| TRELLIS.2 | 11 | 0.0062 (0.0206) · 43.1 dB (33.3) | 8.5 / 137 | 3.0 / 44 | 0.36% (2.1%) | 13.8 MB |

**The comparison was wrong the first time, and the reason is worth keeping.** The harness's first one-camera comparisons put a repacked Tripo clay jar — three textures that matched their sources to a tenth of a level at every sample — 4 levels redder than its own atlas over the whole jar, and a Meshy limestone 11 levels brighter. The textures were not the cause: with every texture removed from both builds the two pictures still differed by as much. The renderer places its two orbiting point lights from the scene's bounds (1.35 radii out, a range of 4 radii; `systems/renderer` `lighting.cpp`), and the bounds are the union of the leaf clusters' spheres — which a different atlas changes, as it changes the default framing. With those lights off the jar's pictures differ by 0.09 of a level on average, and `-CompareKept` now switches them off; the sun is a direction and stays. The same fact makes any comparison of two builds of one mesh through the renderer's default lights a comparison of two lightings.

### What it shows

1. **On the generator whose atlas was the problem, the repack takes most of the way to the ceiling.** Meshy's triangle remesh goes from 0.69 to 0.46 of the finest cut at the coarse threshold, where no atlas at all would allow 0.39; its seams from 70% of vertices to 24%, its islands from a median of 4,749 to 157. Three props still do not coarsen: the barrel cactus (declined: its spines fold 18% of it on the proxy's charts), the oil lamp (0.88, where its ceiling is 0.69) and the rope (1.00 at the ceiling too — geometry, as E10 found).
2. **The pass rate does not move for Meshy, and that is the check working.** Of the six that did not coarsen, two now pass and a third coarsens and fails the coarse-vs-finest FLIP instead; two that passed now fail that FLIP too — a LOD that coarsens has something to lose, which the ceiling shows at its extreme (6 of 20 pass, 11 FLIP failures). The repack turns "cannot coarsen" into "coarsens and has to be judged on its picture", which is the question E10 was asking all along.
3. **Tripo's atlases were already sound, and the repack neither helps nor hurts much.** 0.52 → 0.45 (ceiling 0.38) with the seams unchanged at 21%; the pass rate is still 10 of 20 and Tripo's failures are still the coarse picture — detail in geometry at a budget, the first repair of 07 §7.7, which a repack cannot touch.
4. **TRELLIS.2 is limited by its geometry, and the gate says so first.** Nine of twenty are declined — the rope (50% of its triangles fold or flatten), the barrel cactus and the canvas bundle (17–20%), the skull, the driftwood, the lamp, the bucket, the leather skin, the broken pot (5–10%) — and the ceiling shows seven of those nine do not coarsen even with no atlas. Of the eleven it repacks, the LOD collapses further on most (the limestone 0.87 → 0.69, the tin cup 0.79 → 0.72) and the pass rate rises from 5 to 8. The thin, intricate shapes that fold a 2,000-triangle proxy are the same ones the unwrap fragmented: the problem is one surface, not the atlas on it.
5. **The picture it bakes is close to the source's, and where it is not the cause is known.** From one camera the median repacked prop is 0.004–0.007 FLIP from its own atlas (PSNR 43–50 dB) and the worst 0.02, the size of the line the coarse cut is itself held to. On the Meshy limestone the parts separate cleanly: the normal-map conversion halves the error a plain resample makes (FLIP 0.0111 against 0.0179), and baking at twice the texture's side takes it to 0.0076 ([atlas](../subsystems/atlas.md#what-it-costs-the-picture)). TRELLIS.2's colour p99 is high (a median of 137 levels); the folded and refused triangles that share texels with a neighbour are 0.4–2.7% of the props it repacked, and the one-camera numbers above are what that costs in the picture.
6. **It costs ten seconds a prop.** Builds take a median 10.5–11.9 s against 0.5–0.75 s with the own atlas, 9–10.5 s of it charting and packing — the proxy, its transfer and LSCM on 50,000–250,000 triangles — and the PNGs it writes are 9–14 MB a prop, which is where the container's bytes go.

### What it decides

- **`keep` stays the default; `repack` is the setting for generated props from a generator whose atlas fragments** — Meshy's triangle remesh and TRELLIS.2 at these budgets, told apart by the harness's own fragmentation diagnosis (more than 30% of vertices on a seam, or a median island under 1,024 texels). On a sound atlas it buys a few hundredths of the coarse cut for ten seconds and a second rebake of every texture. The status note in [07 §7.4](../plan/07-content-pipeline.md#74-validation-rules-automatic) records this.
- **The gate stays at 5%.** Nothing measured sat near it: the worst prop it repacked leaves 2.7% of its triangles folded or flat (TRELLIS.2's cart wheel), the least-bad it declined 5.4% (the broken pot), and a declined prop is its own atlas, never worse.
- **What is left for TRELLIS.2 and the rope is geometry**: decimation to a budget with the detail baked into the normal map (07 §7.7's first repair), which needs this step's rebake with a high-poly source, not another atlas.

### Caveats (repack)

- **One proxy size and one chart cost.** `--atlas-proxy 2000` and xatlas's default `maxCost` throughout. On three Meshy props (ribcage, canvas bundle, oil lamp) a proxy of 500 cut the seams by 2–5 points and folded more on all three, and 4,000 made more charts and more seams; `--atlas-chart-cost` 4,000 and 8,000 changed the ribcage's 168 proxy charts to 177. The seams left (24–40% on the fragmented sets) are mostly the borders between the *pieces* a chart's triangles fall into on a mesh with non-manifold edges and loose parts — the ribcage's 21 proxy charts at a proxy of 500 still made 346 islands — which is where a better transfer would earn more than a different chart count.
- **Only textured materials, and every one of these props has one.** An untextured material keeps its UVs by design.
- **The ceiling is a bound, not a target.** Its seam-free collapses draw textures across islands; no atlas reaches it without the texture moving.
- **One camera, one resolution, lights off.** The one-camera comparison has the sun and the sky only; with the orbiting lights on, the two builds are lit differently for a reason that has nothing to do with the atlas (above).

The reports are under `D:\workspace\game_engine_local\e10\` (`atlas-<set>-orbit16-{keep,ceiling,repack}\`, and the rebased spot checks in `atlas-<set>-orbit16-repack-rebased\`); none is committed.
