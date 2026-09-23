# E10: how much of what a generator makes can the engine take as it comes

- **Question ([10 §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing)):** the pass rate of ML-generated props through the validators, and the repair yield — which 07 §7.7 asks for before the pipeline may depend on generation ("measure the pass rate before depending on it"). The **first pass** (2026-09-22) is 20 props from one image model and one image-to-3D service, where the row asks for 200 across services; the **second pass** (2026-09-23, [below](#pass-2-the-same-twenty-remeshed-to-a-triangle-budget)) is the same twenty images remeshed to a triangle budget, beside the owner's Tripo run at the same budgets and a mechanical repair of the atlas fault the first pass found; the owner's two local TRELLIS.2 meshes are [at the end](#local-generation-the-owners-two-trellis2-samples).
- **Date:** 2026-09-22. **Machine:** Intel Core i9-10980XE (18 cores, 36 threads), 64 GB, Windows 11 Pro 26200; **GPU:** RTX 5090, driver 610.88, Vulkan. **Build:** `msvc-release` at `5d57bb7` (`engine-content`, `engine-view`, `engine-image`, copied by the harness).
- **Machine state:** shared and busy, WARNING raised on all 40 capture runs of each measured pass. Of the first pass's two harness runs, the second (the one reported): other processes at **10–46% of the CPU**, the GPU **7–96% busy** with 7.6–8.0 GB of 32.6 GB in use, the GPU lock free at the start and held at the end by another agent timing a morph stage. The first run: others at **12–100%** of the CPU, the GPU 5–16% busy. **The picture metrics and the container bytes were identical in both passes, all 20 assets, to the last digit**, so they do not depend on load; **the build milliseconds are upper bounds** — one asset's build took 3× as long in the first pass as in the second, under a 100% CPU spike from another process. The image batch held the GPU lock; the captures did not need it ([content-generation](../content-generation.md#the-e10-harness-toolse10-harnessps1)).
- **Decision:** the E10 row in 10 §10.5 is **Measured** (not Done: 60 generated props of the 200, two services and one local model; the repair yield is measured for one repair), and 07 §7.4 carries status notes on which validators the pipeline needs first. The second pass revised the first on two points, marked where they occur: the "zero-area triangles" were points only as stored, and density is the failure only at the extremes.

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
**Pass rate: 17 of 20 (85%)** on the harness's own criteria — against **11 of 20** for the first pass's meshes with the same build (6 of 20 without the repair) and **10 of 20** for Tripo's. Two fail on FLIP (the eroded limestone and the sandstone boulder, the two large rocks, both at 250,000) and one on the island check (the tin cup, whose last sub-texel island is among the repair's five unrepaired triangles). That 85% is misleading, and the reason is the most useful thing the second pass found (point 2 below).

| prop | budget | triangles | islands | seam % | smallest island, texels (triangles) | median island, texels | container MB | build s | PSNR dB | FLIP (object only) | coarse / finest clusters | result |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| animal-ribcage | medium | 125,067 | 9,144 | 76.6 | 1 (1) | 62 | 21.5 | 0.9 | 35.3 | 0.0171 (0.077) | 0.75 | pass |
| barrel-cactus | medium | 120,509 | 48,793 | 96.4 | 1 (1) | 26 | 26.3 | 1.0 | identical | 0.0000 (0.000) | 1.00 | pass |
| basalt-slab | medium | 123,840 | 1,835 | 43.3 | 1 (1) | 137 | 17.4 | 0.8 | 36.9 | 0.0169 (0.041) | 0.40 | pass |
| broken-pot | hand | 51,216 | 1,179 | 50.8 | 1 (1) | 616 | 10.2 | 0.3 | 43.3 | 0.0087 (0.026) | 0.51 | pass |
| canvas-tent-bundle | medium | 121,570 | 9,597 | 78.7 | 1 (1) | 16 | 19.5 | 0.9 | 38.8 | 0.0076 (0.016) | 0.91 | pass |
| cart-wheel | medium | 121,463 | 10,297 | 77.0 | 1 (1) | 12 | 19.7 | 0.7 | 40.5 | 0.0055 (0.049) | 0.74 | pass |
| cattle-skull | medium | 124,695 | 4,749 | 62.0 | 1 (1) | 46 | 19.1 | 0.7 | 38.5 | 0.0099 (0.057) | 0.61 | pass |
| clay-water-jar | hand | 51,363 | 902 | 45.7 | 1.09 (1) | 286 | 9.4 | 0.3 | 42.0 | 0.0119 (0.027) | 0.67 | pass |
| dead-tree | large | 259,670 | 4,256 | 42.9 | 1 (1) | 14 | 26.4 | 1.5 | 36.5 | 0.0110 (0.076) | 0.33 | pass |
| driftwood-log | medium | 123,455 | 6,554 | 70.0 | 1 (1) | 36 | 20.8 | 0.9 | 36.3 | 0.0153 (0.055) | 0.74 | pass |
| eroded-limestone | large | 259,168 | 4,673 | 44.8 | 1 (1) | 17 | 26.7 | 1.9 | 35.4 | 0.0266 (0.065) | 0.52 | fail: flip |
| leather-water-skin | hand | 50,806 | 5,813 | 85.5 | 1 (1) | 39 | 14.5 | 0.4 | 37.4 | 0.0095 (0.035) | 0.86 | pass |
| oil-lamp | hand | 50,355 | 6,275 | 86.4 | 1 (1) | 84 | 14.1 | 0.4 | 44.2 | 0.0022 (0.010) | 0.97 | pass |
| rope-coil | hand | 52,177 | 13,555 | 95.9 | 5.23 (1) | 200 | 16.4 | 0.5 | identical | 0.0000 (0.000) | 1.00 | pass |
| saguaro-cactus | large | 257,800 | 18,759 | 75.1 | 1 (1) | 27 | 29.4 | 2.3 | 44.0 | 0.0029 (0.046) | 0.70 | pass |
| sandstone-boulder | large | 258,744 | 2,355 | 34.4 | 1 (1) | 24 | 23.8 | 2.0 | 38.2 | 0.0244 (0.054) | 0.34 | fail: flip |
| signpost | medium | 123,016 | 718 | 33.5 | 2.84 (1) | 7,767 | 16.3 | 0.9 | 42.9 | 0.0062 (0.045) | 0.44 | pass |
| tin-cup | hand | 49,744 | 1,976 | 59.2 | 0 (1) | 66 | 11.8 | 0.4 | 40.2 | 0.0084 (0.022) | 0.69 | fail: island |
| well-bucket | hand | 48,456 | 4,548 | 84.8 | 1 (1) | 94 | 14.5 | 0.4 | 40.9 | 0.0041 (0.009) | 0.87 | pass |
| wooden-crate | medium | 109,987 | 8,213 | 74.9 | 1 (1) | 14 | 18.7 | 0.7 | 40.4 | 0.0102 (0.022) | 0.68 | pass |

### Pass 1, pass 2 and Tripo, prop by prop

All three measured with the same build. "Coarse / finest clusters" is the share of the finest cut's visible clusters the coarse cut (1 px) still draws: how far the LOD collapsed.

| prop | triangles: pass 1 · pass 2 · Tripo | FLIP whole frame: pass 1 · pass 2 · Tripo | FLIP object only | coarse / finest clusters | islands | smallest island, texels | result: pass 1 · pass 2 · Tripo |
|---|---|---|---|---|---|---|---|
| animal-ribcage | 1,464,002 · 125,067 · 112,288 | 0.0237 · 0.0171 · 0.0239 | 0.122 · 0.077 · 0.082 | 0.16 · 0.75 · 0.52 | 1,140 · 9,144 · 299 | 1.01 · 1 · 18 | fail · pass · fail |
| barrel-cactus | 7,551,206 · 120,509 · 117,277 | 0.0943 · 0.0000 · 0.0332 | 0.149 · 0.000 · 0.075 | 0.15 · 1.00 · 0.68 | 13,230 · 48,793 · 2,435 | 1 · 1 · 4 | fail · pass · fail |
| basalt-slab | 238,514 · 123,840 · 115,426 | 0.0165 · 0.0169 · 0.0104 | 0.044 · 0.041 · 0.036 | 0.26 · 0.40 · 0.27 | 332 · 1,835 · 66 | 1.7 · 1 · 140 | pass · pass · pass |
| broken-pot | 113,068 · 51,216 · 49,264 | 0.0103 · 0.0087 · 0.0163 | 0.023 · 0.026 · 0.044 | 0.38 · 0.51 · 0.37 | 327 · 1,179 · 66 | 9.41 · 1 · 80 | pass · pass · pass |
| canvas-tent-bundle | 2,138,398 · 121,570 · 119,244 | 0.0456 · 0.0076 · 0.0394 | 0.090 · 0.016 · 0.087 | 0.16 · 0.91 · 0.59 | 2,104 · 9,597 · 194 | 1 · 1 · 16 | fail · pass · fail |
| cart-wheel | 679,864 · 121,463 · 113,662 | 0.0076 · 0.0055 · 0.0077 | 0.060 · 0.049 · 0.051 | 0.26 · 0.74 · 0.52 | 1,048 · 10,297 · 308 | 1 · 1 · 16 | pass · pass · pass |
| cattle-skull | 775,300 · 124,695 · 119,764 | 0.0140 · 0.0099 · 0.0131 | 0.070 · 0.057 · 0.077 | 0.19 · 0.61 · 0.40 | 510 · 4,749 · 242 | 2.6 · 1 · 24 | pass · pass · pass |
| clay-water-jar | 81,934 · 51,363 · 47,844 | 0.0102 · 0.0119 · 0.0257 | 0.024 · 0.027 · 0.047 | 0.45 · 0.67 · 0.61 | 226 · 902 · 47 | 28.96 · 1.09 · 154 | pass · pass · fail |
| dead-tree | 384,686 · 259,670 · 239,476 | 0.0128 · 0.0110 · 0.0130 | 0.090 · 0.076 · 0.089 | 0.15 · 0.33 · 0.17 | 420 · 4,256 · 188 | 8.31 · 1 · 21 | pass · pass · pass |
| driftwood-log | 1,122,166 · 123,455 · 119,368 | 0.0250 · 0.0153 · 0.0186 | 0.080 · 0.055 · 0.058 | 0.15 · 0.74 · 0.38 | 562 · 6,554 · 136 | 2 · 1 · 8 | fail · pass · pass |
| eroded-limestone | 2,088,158 · 259,168 · 248,306 | 0.0337 · 0.0266 · 0.0287 | 0.059 · 0.065 · 0.071 | 0.33 · 0.52 · 0.32 | 1,444 · 4,673 · 163 | 1.5 · 1 · 9 | fail · fail · fail |
| leather-water-skin | 836,676 · 50,806 · 48,567 | 0.0151 · 0.0095 · 0.0160 | 0.066 · 0.035 · 0.038 | 0.17 · 0.86 · 0.62 | 823 · 5,813 · 186 | 1.75 · 1 · 1 | pass · pass · pass |
| oil-lamp | 1,013,104 · 50,355 · 48,798 | 0.0167 · 0.0022 · 0.0239 | 0.065 · 0.010 · 0.067 | 0.23 · 0.97 · 0.65 | 1,086 · 6,275 · 423 | 1.11 · 1 · 40 | pass · pass · fail |
| rope-coil | 6,417,470 · 52,177 · 49,138 | 0.0560 · 0.0000 · 0.0002 | 0.100 · 0.000 · 0.000 | 0.18 · 1.00 · 1.01 | 1,955 · 13,555 · 96 | 1 · 5.23 · 96 | fail · pass · pass |
| saguaro-cactus | 1,225,482 · 257,800 · 245,488 | 0.0103 · 0.0029 · 0.0147 | 0.118 · 0.046 · 0.109 | 0.11 · 0.70 · 0.19 | 910 · 18,759 · 141 | 1.06 · 1 · 18 | pass · pass · pass |
| sandstone-boulder | 573,788 · 258,744 · 235,606 | 0.0302 · 0.0244 · 0.0392 | 0.069 · 0.054 · 0.071 | 0.14 · 0.34 · 0.27 | 219 · 2,355 · 67 | 5.9 · 1 · 20 | fail · fail · fail |
| signpost | 191,036 · 123,016 · 111,853 | 0.0068 · 0.0062 · 0.0225 | 0.047 · 0.045 · 0.084 | 0.44 · 0.44 · 0.28 | 2,777 · 718 · 412 | 1 · 2.84 · 3 | pass · pass · fail |
| tin-cup | 232,284 · 49,744 · 47,182 | 0.0108 · 0.0084 · 0.0107 | 0.033 · 0.022 · 0.025 | 0.24 · 0.69 · 0.58 | 398 · 1,976 · 96 | 14 · 0 · 39 | pass · fail · pass |
| well-bucket | 1,308,638 · 48,456 · 47,736 | 0.0277 · 0.0041 · 0.0228 | 0.060 · 0.009 · 0.041 | 0.20 · 0.87 · 0.69 | 2,442 · 4,548 · 201 | 1 · 1 · 14.5 | fail · pass · fail |
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

2. **Meshy's triangle remesh fragments the atlas so far that the LOD cannot coarsen the mesh — and the FLIP check passes a mesh that does not coarsen.** The remeshed atlases have 718 to 48,793 islands and 34% to 96% of their vertices on a seam (the first pass: 219 to 13,230 islands, 9% to 42%), and their median island is 12 to 616 texels on nineteen of the twenty (the first pass: 191 to 43,800). A seam vertex is one the LOD builder may not collapse across, so at the default threshold the coarse cut still draws a **median 70% of the finest cut's clusters** — against 20% for the first pass's meshes and 52% for Tripo's — and that share follows the seam fraction at **r = 0.94**. The barrel cactus and the rope coil draw **exactly** their finest cut at the coarse threshold, so their two captures are identical and they "pass" at FLIP 0.0000; four more draw 86–97% (the oil lamp, the canvas bundle, the well bucket, the leather water skin). Over the twenty, the whole-frame FLIP *falls* as the share rises (r = −0.64): **the check rewards a mesh for not simplifying**, because it compares a cut with the finest cut and a cut that did not coarsen has nothing to lose. Six of pass 2's seventeen passes are that; **eleven** pass and coarsen to at most three quarters of their finest cut. Tripo's rope coil (share 1.01, FLIP 0.0002) is the same case, which is why it passes. The two TRELLIS.2 samples showed the same relation between one segmenter and another (below).

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
- **The validator the pipeline needs next measures how far the LOD collapses**, not how many islands an atlas has: the coarse cut's share of the finest at the harness's reference view, which the harness records (`coarse_visible_pairs`, `finest_visible_pairs`) but does not yet judge. A mesh that cannot coarsen passes every picture check the harness makes while costing its finest cut at every distance — six of the second pass's seventeen passes, and Tripo's rope. The island distribution explains it (r = 0.94), and the repair is upstream, in the generator's settings (the quad remesh, TRELLIS.2's segmenter), before it is a repack.
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
- **The sidecars of this batch record `bake_seconds` as null**, from a bug fixed after it (a role id PowerShell handed back wrapped, which the timing table then failed to find); every bake node's own seconds are in `timings.nodes`, which is where the numbers above come from.

The meshes, their sidecars and `runs.jsonl` are under `D:\workspace\game_engine_local\generated\trellis\2026-09-23\`, and the harness's reports, captures and heat maps under `e10\trellis-sweep-2026-09-23\` and `e10\trellis-boulder-2026-09-23\`; none is committed.
