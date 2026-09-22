# E10: how much of what a generator makes can the engine take as it comes

- **Question ([10 §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing)):** the pass rate of ML-generated props through the validators, and the repair yield — which 07 §7.7 asks for before the pipeline may depend on generation ("measure the pass rate before depending on it"). This is the **first pass**: 20 props from one image model and one image-to-3D service, where the row asks for 200 across services.
- **Date:** 2026-09-22. **Machine:** Intel Core i9-10980XE (18 cores, 36 threads), 64 GB, Windows 11 Pro 26200; **GPU:** RTX 5090, driver 610.88, Vulkan. **Build:** `msvc-release` at `5d57bb7` (`engine-content`, `engine-view`, `engine-image`, copied by the harness).
- **Machine state:** shared and busy, WARNING raised on all 40 capture runs of each measured pass. Second pass (the one reported): other processes at **10–46% of the CPU**, the GPU **7–96% busy** with 7.6–8.0 GB of 32.6 GB in use, the GPU lock free at the start and held at the end by another agent timing a morph stage. First pass: others at **12–100%** of the CPU, the GPU 5–16% busy. **The picture metrics and the container bytes were identical in both passes, all 20 assets, to the last digit**, so they do not depend on load; **the build milliseconds are upper bounds** — one asset's build took 3× as long in the first pass as in the second, under a 100% CPU spike from another process. The image batch held the GPU lock; the captures did not need it ([content-generation](../content-generation.md#the-e10-harness-toolse10-harnessps1)).
- **Decision:** the E10 row in 10 §10.5 is **Measured** (not Done: Tripo, the local image-to-3D models and the repair yield are still to come), and 07 §7.4 carries a status note on which validators the pipeline needs first.

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

- **Island, 12 props — and two different defects under one threshold.** In **8** the atlas is sound (median island 2,800–38,000 texels, seams 9–18%) and the sub-texel island is **one triangle whose three UVs coincide** (ten triangles for the limestone): a UV-hygiene fault, repaired by folding those triangles into the neighbouring island — no repack, no rebake. In **4** the atlas really is fragmented: the barrel cactus (13,858 islands, median 168 texels), the well bucket (median 351), the canvas bundle (median 858) and the signpost (41.7% of its vertices on a seam).
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
