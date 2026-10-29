# The opaque shadow proxy, and what opacity micromaps would take (2026-10-28, R109)

R109 asks for a cheaper answer for an alpha-tested caster's shadow ray. The rays' resolve on the forest is mostly the masks: [the shadow hybrid's page](shadow-hybrid-2026-10-26.md#the-masks) measured 27.7 of the expanded forest's 36.8 ms at 11520×2160 going to them. Stage 1, built here, is the cheaper of the two levers that page named: **a masked cluster whose alpha coverage is high enough is solid to a shadow ray**, its mask never looked up ([renderer](../subsystems/renderer.md#the-opaque-shadow-proxy) has the mechanism). Stage 2, written down at the end and not built, is opacity micromaps.

**The finding**: the proxy works and costs nothing when it is off, but it **cannot buy the masks' cost at a threshold that leaves the picture alone**, because the clusters the rays spend their time in — the canopy near the eye — are the sparse ones. Above a coverage of a half it changes nothing measurable; at 0.3 it saves 13% of the resolve and closes the sun flecks on the ground under the canopy; at 0.2 a third, and the picture moves by six FLIP points. **The default stays off** (`renderer.shadow.opaque_coverage` 0). The lever is stage 2.

## Setup

`msvc-release` engine-view at fa41167c plus this change, offscreen, the proof of concept's v20 scene (`world/v20/base.json`, `world/v5/materials`) read where it is, a copy of `world/v20/ddc` as `--ddc`, and the POC's run arguments with the clock stopped so every capture is the same frame: `--terrain-tiles --terrain-far 6 --time-rate 0 --world-rate 0 --no-lights --time-of-day 15 --lod 1 --deform-pool-mib 64 --rt-budget-mib 1024 --raster hw --shadows rt`. Two held camera paths: `grass-clearing-path.json` (a clearing with a broadleaf at its edge, the canopy's shadow on the grass) and `expanded-forest-path.json` (the wide path: inside the stand, the ground under the canopy). The proxy's threshold is set with `--tunable renderer.shadow.opaque_coverage=<t>`; "masks" is 0 (no table, the loop as before R109), "opaque" is `renderer.shadow.opaque_rays=true` (every caster solid: the floor, and the wrong picture).

- **Pictures**: `--capture` after 30 frames at 1600×900, compared against the masks' picture with `engine-image compare --flip --json` (LDR-FLIP, the image module's; mean and 95th percentile over the picture).
- **Time**: `--benchmark` over 120 frames after 30 of warm-up, `--wait-quiet 60`, the median `gpu_ms.resolve` and `gpu_ms.total`. 1600×900 for every threshold; 11520×2160 for masks, 0.3 and opaque on the expanded forest, under `tools/gpu-lock.ps1 run`.
- **The bake**: `--startup-trace` and `--log info` on one run.

**Machine state.** Shared: two other engine agents were building and testing through the night, and one held the GPU lock for its tests while the 1600×900 runs went (they were not under the lock; the wide ones were). Each run's `machine_state` (other processes' CPU, GPU utilisation, at start → end): expanded forest at 1600×900 masks 20%/– → 20%/0%, 0.5 18%/– → 13%/1%, 0.3 9%/0% → 10%/99%, 0.2 16%/– → 15%/12%, opaque 11%/0% → 11%/100%; the clearing masks 10%/0% → 11%/0%, 0.5 10%/0% → 10%/0%, 0.3 9%/0% → 10%/98%, 0.2 27%/0% → 28%/1%, opaque 12%/2% → 45%/13%; the wide runs masks 6%/0% → 9%/100%, 0.3 9%/1% → 10%/0%, opaque 9%/0% → 16%/15%. Every number is an upper bound. The rows agree with each other where they should (0.5–0.85 within 2% of the masks on both views) and the differences that matter are many times the spread.

## The table

| view | threshold | resolve, ms | total, ms | FLIP mean | FLIP p95 |
|---|---|---:|---:|---:|---:|
| expanded forest, 1600×900 | masks (0) | 3.04 | 9.97 | — | — |
| | 0.85 | 3.10 | 10.02 | 0 | 0 |
| | 0.7 | 3.09 | 10.02 | 0.0005 | 0 |
| | 0.5 | 3.08 | 10.56 | 0.0014 | 0.0001 |
| | 0.4 | 3.08 | 10.17 | 0.0014 | 0.0001 |
| | **0.3** | **2.64** | 10.17 | **0.0155** | **0.099** |
| | 0.2 | 2.11 | 9.74 | 0.064 | 0.380 |
| | opaque | 0.74 | 7.75 | 0.078 | 0.428 |
| grass clearing, 1600×900 | masks (0) | 1.46 | 7.32 | — | — |
| | 0.85 | 1.49 | 7.98 | 0 | 0 |
| | 0.7 | 1.38 | 7.32 | 0 | 0 |
| | 0.5 | 1.50 | 7.33 | 0.000003 | 0 |
| | 0.4 | 1.49 | 13.92 | 0.000003 | 0 |
| | 0.3 | 1.48 | 12.28 | 0.0003 | 0 |
| | 0.2 | 1.08 | 7.17 | 0.019 | 0.124 |
| | opaque | 0.50 | 7.49 | 0.030 | 0.183 |
| expanded forest, 11520×2160 | masks (0) | 34.27 | 54.38 | | |
| | 0.3 | 29.70 | 49.78 | | |
| | opaque | 11.62 | 31.61 | | |

(The clearing's 0.4 and 0.3 totals are another process's load — the resolve beside them is the masks' — and the 0.85 rows' small rises are noise of the same kind: none of them changed a pixel.)

**What the curve says.** The masks cost the expanded forest's resolve 2.3 ms of 3.0 at 1600×900 and 22.6 of 34.3 at 11520×2160 (masks against opaque), as R102 found. A threshold anywhere from 0.4 up leaves both the picture and the time where they were: the clusters dense enough to pass it are few, or not where the rays go. The time starts to move only at 0.3, and with it the picture: the FLIP map is dark everywhere but on the ground under the canopy near the eye, where the sun flecks between the leaves' shadows close (single pixels to 0.5). At 0.3 the proxy saves 4.6 ms of the wide resolve's 34.3 (13%; a fifth of the masks' 22.6), for 1.5 FLIP points mean and a tenth at the 95th percentile; at 0.2, 0.94 ms of 2.3 at 1600×900 for six points. A leaf card is mostly transparent (the leaf inside its quad), so a cluster of them keeps well under half its surface, and the bytes that would let the proxy fire without moving the picture are the coarse levels' and the far canopy's, which the rays barely reach. (The bytes were not histogrammed; this is read off the curve.)

The brief's rule — a threshold that moves the picture by more than a few FLIP points at 1600×900 is reported, not shipped — passes 0.3 on the mean and fails it where it matters: the change is concentrated in the forest's sun flecks, the part of the light a person looks at. So the default is 0, and the mechanism stays behind its tunable for the owner to weigh. The budgeted runs cannot move either way: none of the five draws an alpha-tested material under the rays (the overlook runs `--shadows off`; the erg and the endless desert are terrain alone), and with the tunable at 0 no table is made and the shader's loop is the one before.

**What it costs.** One byte per cluster of the scene, resident: 1.37 MB on the v20 forest, of which 319,484 clusters are masked and carry a non-zero byte. The bake is one dispatch at load: 11.7 ms on the v20 forest on the RTX 5090, within the renderer's creation (the startup trace's `shadow coverage` stage). `ResolveParams` grows by one 16-byte row (448 → 464). A frame with the tunable at 0 reads nothing new.

## Stage 2: what opacity micromaps would take (written down, not built)

An **opacity micromap** subdivides a triangle into 4^L micro-triangles and stores 1 bit (2-state: opaque, transparent) or 2 bits (4-state: also "unknown opaque" and "unknown transparent") for each; the traversal hardware resolves the known states itself, and only an unknown micro-triangle reaches the candidate loop. For a leaf card that is most of its area decided without the shader — the transparent corners never become candidates and the leaf's interior commits as opaque — and the mask is looked up only along the leaf's edge. Where the proxy trades the picture for time, micromaps keep the picture (4-state, with the edge left to the mask) and pay in memory and build time.

- **The device**: `VK_EXT_opacity_micromap` (`VkPhysicalDeviceOpacityMicromapFeaturesEXT`; the subdivision limits are `VkPhysicalDeviceOpacityMicromapPropertiesEXT::maxOpacity2StateSubdivisionLevel` / `maxOpacity4StateSubdivisionLevel`, to be read on each device). Whether this machine's driver exposes it was not checked: `engine-cli gpu.adapters` reports only the extensions the engine asks for, so the next stage starts with `vulkaninfo` on the RTX 5090 and on the two Pascal runners (which have no hardware traversal of micromaps, and on which the KHR path would keep today's loop).
- **The cluster path takes them.** `VkClusterAccelerationStructureBuildTriangleClusterInfoNV` (VK_NV_cluster_acceleration_structure) carries `opacityMicromapArray` (a device address; 0 is none for that cluster), `opacityMicromapIndexBuffer` (per-triangle indices into the array; 0 means the triangle's own index), `opacityMicromapIndexType` and its stride, and VUID-10881 requires `VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_CLUSTER_OPACITY_MICROMAPS_BIT_NV` in the input's flags when any cluster names an array ([the reference page](https://docs.vulkan.org/refpages/latest/refpages/source/VkClusterAccelerationStructureBuildTriangleClusterInfoNV.html)). So the masked casters need not move to the KHR path: `clas_records.slang`'s per-cluster record gains two addresses and an index type, and the chain's input flags one bit. The KHR fallback attaches a micromap with `VkAccelerationStructureTrianglesOpacityMicromapEXT` on the triangles' geometry ([VK_EXT_opacity_micromap](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_opacity_micromap.html)). The four special indices (`VK_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_OPAQUE_EXT`, `_FULLY_TRANSPARENT_EXT`, `_FULLY_UNKNOWN_OPAQUE_EXT`, `_FULLY_UNKNOWN_TRANSPARENT_EXT`) let a triangle that is wholly one state cost no micromap at all.
- **The primary rays and the shadow rays want different states.** A primary ray must not see a leaf's edge as solid (the picture is the rasterizers', to the pixel), so it keeps the unknown states for the mask; a shadow ray may take the 2-state answer (`VK_GEOMETRY_INSTANCE_FORCE_OPACITY_MICROMAP_2_STATE_EXT` per instance, or the ray flag where the shading language has it) and never run the loop at all, at the price of an edge a micro-triangle wide — the honest version of this page's proxy, per micro-triangle instead of per cluster. Which the shadow rays take is the measurement stage 2 makes, with this page's FLIP method.
- **The bake.** meshoptimizer, already fetched at v1.2 (`cmake/EngineGraphics.cmake`), has an experimental generator: `meshopt_opacityMapMeasure` picks a level per triangle from its UV footprint, `meshopt_opacityMapRasterize` fills a triangle's states from the alpha texture with bilinear filtering **at a fixed cutoff of 0.5**, and `meshopt_opacityMapCompact` deduplicates identical entries and replaces uniform ones with the special indices. The engine's materials carry their own cutoff (`set_alpha_mask`), so the bake rescales the alpha around it first, and it runs where the masks are known: a derived step of the content build beside the `.tex` (keyed by the mask's bytes, the UVs and the level), written as a new append-only container section, so a mesh pays it once; the flora solver's leaf cards all sample a few atlas regions, which is what the compaction is for.
- **The memory.** 4-state at level L is 4^L × 2 bits a triangle: 16 bytes at level 3, 64 at level 4, before compaction. Uncompacted, the v20 forest's 319,484 masked clusters at up to 124 triangles would be 0.6 GB at level 3 and 2.5 GB at level 4 — too much — but a leaf card's two triangles map the same atlas region as every other card of its species, so after deduplication the arrays are per *species and level*, kilobytes to a few megabytes, and the per-triangle index (4 bytes, or 2) is what scales with the scene: about 160 MB of indices at 4 bytes for 40 M masked triangles, or none at all where a cluster's triangles are its card's in order and the index buffer is left 0. That arithmetic is the first thing stage 2 measures, on the forest's actual clusters.
- **What it does not change**: the deformed-vertex path. A swaying leaf's positions move, its UVs do not, so its micromap is its rest pose's; the cluster templates' instantiate path (`instantiate_cluster_templates`) has to carry the same two addresses, which is to be read in the specification before it is assumed.

## Not done

- The coverage bytes were not histogrammed; the claim about where the masked clusters' coverage lies is read off the threshold curve.
- No run with flora motion on (`--world-rate 1`): the pictures had to be the same frame. The masks' cost with the leaves swaying is R102's figure; the proxy's saving, a fraction of the masks', would follow it.
- The leaf-card test scene (`content/test-scenes/leaf-cards`) and the POC's understory views were not measured.
