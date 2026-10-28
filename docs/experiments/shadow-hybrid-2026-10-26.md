# Rays near, maps far, on the forest (2026-10-26, R102)

**Question.** The forest proof of concept's expanded forest at 11520×2160 spent 39.5 ms in the resolve under traced shadows against 5.0 ms under the cascaded maps on the same pixels ([the night's context](../subsystems/renderer.md#rays-near-maps-far)). If most of those 34 ms are rays from far pixels, a per-pixel choice by distance — rays near the eye, maps far from it, blended across a band — gives most of them back. Does it, what distance, and what does the frame cost?

**Answer, in short.** It does not, on these views, with the cascades as they stand at 5552a2b3:

1. **The rays' cost is near the eye, and it is the alpha-tested masks.** With every caster traced opaque (`renderer.shadow.opaque_rays`, a measurement switch) the resolve falls from 29.2 to 6.0 ms on the clearing and from 36.8 to 9.1 ms on the expanded forest at 11520×2160: three quarters of the resolve is the masks' candidate evaluation. Moving the rays' horizon in to 100 m takes the clearing's resolve to 19.7 ms and the forest's to 34.1 ms — the forest's view is mostly within 50 m (trunks and canopy), so no distance split saves its rays.
2. **The cascades cost more beside the chain than alone.** The hybrid draws the maps on a frame that builds the chain, and the chain's frame draws the rigid forms at their frontier and has no occlusion culling ([static structures](../subsystems/renderer.md#static-structures-for-rigid-forms)), so the cascades draw the camera's *finer* cut: 5.53 million pairs into the maps on the clearing at 2560×1440 against 2.37 million under the maps alone, and the shadow passes 41.4 ms against 18.7. At 11520×2160 the shadow passes are 99–108 ms against 46–53 under the maps alone.
3. **The cascades' texel is never as fine as the pixel.** The fit's four 2048² cascades split the camera's depth logarithmically, so every cascade is the same ratio of texel to pixel at its far end and about 5.6 times that at its near end: at 11520×2160 the texel is 5.7 pixels at the far end of each of the first three cascades and 32 at their starts (1.9 to 10.5 in the last), and at 2560×1440 1.44 and 8. The brief's criterion — the distance past which a cascade's texel is no coarser than the picture's pixel — has no answer on this fit.

So the hybrid is built, tested and available (`--shadows hybrid`), and **`auto` stays the rays on a device that traces**: on every view measured the hybrid's frame was two to three times the rays' alone. What would change that is in the second and third points: the maps fit to start at the rays' distance and the cascades' cut chosen from their own texels rather than the chain's frontier (the cascades' work of the same night), and a cheaper answer for alpha-tested casters (below).

## Setup

`msvc-release` engine-view at 5552a2b3 plus this change, offscreen, the proof of concept's frozen v14 scenes and camera paths read where they are (`reports/quality-v15/engine-c1ee971d/runs/{expanded-forest,grass-clearing}-rt-wide-*/scene.json`, `world/v14/{expanded-forest,grass-clearing}-path.json`), a copy of `world/v14/ddc` as `--ddc`, and the runs' own arguments (`--terrain-tiles --terrain-far 6 --time-rate 0 --world-rate 1 --no-lights --time-of-day 15 --lod 1 --deform-pool-mib 64 --rt-budget-mib 1024`, flora motion on at wind 0.7, `--raster hw`, `--frames 600 --warmup 0 --repeat 1 --wait-quiet 60`). Medians of `gpu_ms` over the run's frames. The band is the default 20 m throughout. `rt` and `csm` are untouched by the change, so their rows are 5552a2b3's.

**Machine state.** Shared, and not quiet: other sessions held the GPU lock for their own captures for most of the night, and `--wait-quiet 60` gave up waiting more often than not. Each row says the GPU's and other processes' CPU use at the run's start and end as the harness recorded them (`machine_state`); every number is an upper bound, and a row whose GPU was busy at both ends (marked †) is not to be compared on its own. The conclusions above rest on rows that agree with each other and with an idle-GPU row beside them.

## The table

| view | shadows | total | resolve | rt chain | maps (shadow) | wanted structures | capacity | horizon casters | casters built | GPU start/end, CPU others |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| clearing 11520×2160 | rt | 68.6 | 29.2 | 4.0 | — | 98,329 | 166,937 | 0 | 148,066 | 1/99%, 31/47% |
| | csm | 65.5 | 3.5 | — | 45.9 | — | — | — | — | 95/1%, 14/85% |
| | hybrid 50 m | 278.0† | 7.4 | 14.7 | 154.2 | 89,500 | 166,937 | 146,139 | 1,927 | 100/100%, 17/21% |
| | hybrid 100 m | 158.0 | 19.7 | 3.9 | 99.6 | 92,945 | 166,937 | 142,694 | 5,372 | 2/100%, 30/31% |
| | hybrid 200 m | 164.8 | 25.6 | 4.1 | 99.9 | 98,249 | 166,937 | 134,259 | 13,807 | 1/100%, 52/19% |
| | hybrid 400 m | 165.7 | 28.4 | 3.9 | 99.0 | 98,255 | 166,937 | 110,186 | 37,880 | 1/23%, 18/20% |
| clearing 2560×1440 | rt | 15.4 | 3.4 | 2.7 | — | 54,700 | 69,632 | 0 | 228,500 | 100/1%, 19/19% |
| | csm | 23.2 | 0.6 | — | 18.7 | — | — | — | — | 1/1%, 46/100% |
| | hybrid 50 m | 52.3 | 1.6 | 2.2 | 40.5 | 30,683 | 40,960 | 228,654 | 319 | 1/100%, 22/27% |
| | hybrid 100 m | 55.1† | 1.9 | 2.8 | 41.4 | 39,558 | 53,248 | 219,309 | 9,194 | 100/100%, 33/35% |
| | hybrid 200 m | 54.3 | 2.8 | 2.7 | 40.4 | 54,484 | 69,632 | 202,794 | 25,706 | 1/11%, 15/19% |
| | hybrid 400 m | 72.3 | 3.3 | 4.8 | 45.8 | 54,508 | 69,632 | 126,870 | 101,630 | 0/99%, 12/29% |
| forest 11520×2160 | rt | 95.0 | 36.8 | 33.5 | — | 222,455 | 166,937 | 0 | 33,011 | 1/1%, 23/38% |
| | csm | 69.6 | 5.1 | — | 53.2 | — | — | — | — | 1/70%, 100/95% |
| | hybrid 50 m | 201.2 | 33.4 | 34.1 | 108.1 | 222,290 | 166,937 | 17,914 | 15,097 | 99/1%, 39/47% |
| | hybrid 100 m | 201.0 | 34.1 | 34.6 | 107.3 | 222,372 | 166,937 | 17,832 | 15,179 | 7/2%, 20/18% |
| | hybrid 200 m | 222.1 | 38.8 | 38.2 | 113.9 | 222,426 | 166,937 | 17,772 | 15,239 | 4/100%, 90/21% |
| | hybrid 400 m | 202.7 | 37.1 | 33.4 | 107.5 | 222,432 | 166,937 | 15,953 | 17,058 | 2/100%, 23/99% |
| forest 2560×1440 | rt | 35.2† | 6.9 | 10.1 | — | 121,283 | 166,937 | 0 | 16,208 | 99/100%, 23/23% |
| | csm | 31.1 | 0.8 | — | 26.5 | — | — | — | — | 2/98%, 16/17% |
| | hybrid 50 m | 70.6 | 5.6 | 4.1 | 52.4 | 121,255 | 166,937 | 10,519 | 5,689 | 1/31%, 85/64% |
| | hybrid 100 m | 166.9 | 6.4 | 16.2 | 95.3 | 121,260 | 166,937 | 10,514 | 5,694 | 6/100%, 32/37% |
| | hybrid 200 m | 72.4 | 6.9 | 4.3 | 53.0 | 121,262 | 166,937 | 10,510 | 5,698 | 2/1%, 95/100% |
| | hybrid 400 m | 76.6† | 6.9 | 4.8 | 54.9 | 121,268 | 166,937 | 9,349 | 6,859 | 100/100%, 100/22% |

Milliseconds. "Wanted" is the records pass's demand at the median, "capacity" what the chain held; the expanded forest at 1 GiB wants more than it holds in every mode that traces, so it drops instances every frame (101 overflow frames in the proof of concept's run, the same here). "Horizon casters" are the casters past the rays' reach that the hybrid did not build (`Stats::horizon_casters`); "casters built" the ones it did.

**What the horizon gave back.** On the clearing it is most of the casters — 148,066 built under `rt`, 5,372 at a 100 m reach — and the chain's demand fell from 98,329 to 92,945 structures; the chain's milliseconds did not move (4.0 against 3.9), because the clearing fits its capacity either way. On the expanded forest it is 17,832 of 33,011 casters, the demand 222,455 against 222,372: the forest's demand is its drawn pairs, which the horizon never touches, and it still overflows the 1 GiB budget. The cascades' cache of the same night will not change that either; what would is a chain that builds the drawn pairs within the rays' reach only, which is the records pass's to decide.

## The masks

| view, 11520×2160 | resolve, masks | resolve, opaque | the masks' cost |
|---|---:|---:|---:|
| clearing, rt | 29.2 | 6.0 (GPU 1/100%) | 23.2 |
| clearing, hybrid 100 m | 19.7 | 5.2 (1/1%) | 14.5 |
| expanded forest, rt | 36.8 | 9.1 (100/100%†) | 27.7 |
| expanded forest, hybrid 100 m | 34.1 | 8.8 (100/100%†) | 25.3 |

Opaque casters draw a different, wrong picture — every leaf card casts its whole quad — so the switch is a measurement and never a default. The masks are most of the remaining ray time on both views, so the next lever is a cheaper answer for an alpha-tested caster, not a shorter ray:

- **Opacity micromaps** (`VK_EXT_opacity_micromap`): the leaf's alpha baked per micro-triangle into states the traversal hardware resolves itself — fully opaque and fully transparent regions never reach the shader, only the "unknown" ones do. meshoptimizer, already vendored, has an opacity-map builder (`opacitymap.cpp`). Open, and to be read in the extension specifications before anything is designed: whether NVIDIA's cluster acceleration structures take micromaps at all, which if not would mean the masked casters move to the KHR path.
- **A shadow proxy for an alpha-tested cluster**: at its coarser foliage levels a canopy's cluster carries a coverage error already (renderer.md, "The foliage threshold"); a cluster whose coverage is high enough could be built opaque for the shadow rays only, its shadow a little too solid, and the masks kept for the clusters where a leaf's outline is still a few pixels. That is a records-pass decision (non-opaque or not per pair), so it belongs with the chain.
- **The rays' cut coarser** was the brief's first lever (`renderer.rt.lod_bias_px`), but R94 did not build it ([why](rt-static-forms-2026-10-25.md#what-was-not-built)). As a stand-in, the whole picture's LOD threshold: at `--lod 2` and `--lod 4` on the clearing the traced resolve is 27.3 and 26.8 ms against 29.2 at `--lod 1` (the hybrid's at 100 m 17.8 and 17.1 against 19.7). A coarser cut barely helps: the cost is per mask test, and a coarser canopy cluster has as many masked leaves in front of a ray.

## The cascades against the pixel

The fit of the last frame (`engine-view: shadow fit`, printed on stderr by a run with maps; the summary line's `shadow_fit`), with the pixel's footprint at distance d being d·2·tan(27.5°)/height:

| view | cascade depths (m) | texel (cm) at 11520×2160 | texel / pixel, far end … near end | texel (cm) at 2560×1440 | texel / pixel |
|---|---|---:|---|---:|---|
| clearing | 0.1–27.6 | 7.6 | 5.7 … 16 at 10 m | 2.9 | 1.44 … 4 at 10 m |
| | 27.6–155 | 43 | 5.7 … 32 | 16.2 | 1.45 … 8.1 |
| | 155–873 | 244 | 5.8 … 33 | 90.6 | 1.44 … 8.1 |
| | 873–4,907 | 444 | 1.9 … 10.5 | 513 | 1.44 … 8.1 |
| expanded forest | 0.1–26.7, 26.7–150, 150–844, 844–4,745 | 7.4, 41, 234, 444 | the same | 2.8, 15.6, 88, 494 | the same |

A logarithmic split gives every cascade the same depth ratio (5.6), so the texel-to-pixel ratio repeats per cascade, and the 11520×2160 picture's horizontal field of view (140°) widens every slice's sphere, and with it the texel, 2.6 times over 2560×1440's. **No distance has the maps' texel at most a pixel**, so the default ray distance could not be chosen by that rule. It stays at **100 m with a 20 m band**: nearer, the maps would be 6 to 32 texels a pixel at 11520×2160 where the eye looks hardest; at 100 m the clearing's resolve is 19.7 ms against the rays' 29.2, and at 400 m the hybrid saves almost nothing (28.4). Fit the cascades to start at the ray distance and the same four 2048² tiles span 100 m to 4.9 km in four ratios of 3.6: about 1.6 texels a pixel at the start of each at 2560×1440, and a distance chosen by the brief's rule becomes possible.

## The pictures

The clearing at 2560×1440 (`--frames 60 --capture`), under `rt`, `csm` and the hybrid at 100 m, shaded and in the shadow view, and the expanded forest's hybrid and `rt` shadow views, are outside the repository with this change's scratch files (the report names the folder). What they show:

- **Near the eye the hybrid is the rays' picture**, the trunk's leaf shadows and the near grass white where the sun reaches it.
- **Across the band the grass darkens.** The grass is a field species that is not traced ([the caster policy](../subsystems/renderer.md#the-caster-policy): `untraced` 15,704 field instances) but is drawn into the nearest cascades, so under the maps the grass shadows itself and under the rays it does not. The hybrid's band is where that difference shows, a strip of darker grass about 100–120 m out; under `csm` the whole field is the darker grey. It is the caster policy's choice made visible, not the blend's: the blend is linear and monotone (the `domain/gfx` test holds it), and the far side is the maps' exact bytes.
- **The far forest and the mountain** are the maps' answer, the same as `csm`'s.

## Again, over cascades cut by their texels (2026-10-27, R103)

The prescription above was taken: the cascades are cut by their own texels since R103 and can be fit from the ray distance out (`renderer.shadow.hybrid_fit`), and this table was taken again on the v18 forest and clearing ([the R103 write-up](cascade-budget-2026-10-26.md#the-hybrid-over-these-cascades-2026-10-27) has it whole). **The hybrid still loses on every view** — 121.1 ms against `rt`'s 90.9 on the expanded forest at 11520×2160, 73.7 against 49.1 on the clearing, 31.9 against 15.3 and 23.1 against 10.9 at 2560×1440 — while `csm` became the cheapest frame everywhere (22.7, 22.9, 9.6 and 7.8). The texel cut does not reach the hybrid's maps: in a frame with the chain a rigid distance form is drawn at its frontier, its LOD data patched so, and the cascades read the same data, so the far cascades draw five million pairs each where `csm`'s draw under a hundred thousand; and fitting the cascades from the ray distance puts the first two in the forms' rings too, which costs more. With the forms cut by the cascades' texels the hybrid would still be 2 to 41% over `rt`, because the rest of its frame is `rt`'s: the chain, the masks near the eye, and the chain frame's finer picture. `auto` stays `rt`.
