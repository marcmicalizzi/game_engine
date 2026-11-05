# The detached block shadow on the v29 foothill ledge (R132, 2026-11-04)

**Verdict: the renderer's.** The block is the ground shadowing itself in the sun's cascaded maps: a cascade cut by its own texels (R103) drew a terrain tile's coarser clusters, and on the v29 ground those stand far enough above the finer surface the picture draws to shadow a ten-metre patch of open ground. No mesh, deposit or proxy casts it. Since this change a terrain level never takes a cascade's texel cut ([renderer](../subsystems/renderer.md#the-ground-takes-the-pictures-cut-in-every-cascade)), and the block is gone.

## What was looked at

The forest proof of concept's frozen reproduction `reports/quality-v29/repro-ledge-wall/` (read as text): its `scene.json` (five foothill-ledge rock recipes and their instances, the world tiles over `world/v29/terrain/region.heightfield.json`, seven water bodies, the earth sky at 46° N on day 250) and `ledge-close-path.json` (one pose held, the eye at (−660.6, 128.1, 1148.8) looking at (−625.4, 116.1, 1134.6)). The rock session's diagnosis had removed the five ledge meshes and their instances and found the shadow remained ("its terrain/CSM origin needs separate renderer diagnosis").

Every capture here is `engine-view` from this worktree's `msvc-release` build, offscreen at 1920×1080, the reproduction's flags (`--raster hw --shadows csm --terrain-tiles --terrain-far 6 --time-rate 0 --time-of-day 15 --sun-rate 0 --no-lights`, the v22 material pack), 20 frames, under the GPU lock, from scratch copies of the scene, the path, the terrain tiles' derived data and the rock session's derived data. The sun is the sky's: 34.45° up at azimuth 148.5° (18,286 lux of sky). A count is the pixels whose shadow view (`--view shadow`: white lit, black shadowed) is darker than a quarter grey, sampled every second pixel in each axis and multiplied by four; **the block** is the rectangle x 1000–1340, y 680–900, which holds the whole patch and nothing else that is dark. These are pictures, not timings: the machine was shared with four other agents' GPU work throughout, and no millisecond here is a cost.

## The hypotheses, one variable at a time

| Capture (scratch `b52/R132/cap/`) | What changed | Block pixels | Whole frame | Reading |
|---|---|---:|---:|---|
| `base-shadow` | nothing: the reproduction | 34,436 | 34,448 | reproduced: a polygonal patch of shadow on open ground in front of the ledge |
| `rt-shadow` | `--shadows rt` | 0 | 36 | the rays see no caster there (the 36 are the debris' own small shadows) |
| `terrain-shadow` | every mesh and instance removed | 34,436 | 34,764 | the terrain alone casts it (plus a sliver at x 715, y 580) |
| `terrain-rt-shadow` | the same, `--shadows rt` | 0 | — | the rays see nothing over the terrain alone either |
| `t-far0` | `--terrain-far 0`: no far levels | 34,436 | — | not a far level drawn over a near one |
| `t-lodfine` | `--lod 0.05`: a finer picture | 36,696 | — | not the picture's own LOD |
| `t-casc1` | `--shadow-cascades 1` | 0 | — | a property of the cascades' cut, not of the ground |
| `t-texel-off` | `renderer.shadow.texel_lod=false` | **0** | — | **the texel cut draws it** |
| `t-err005` | `renderer.shadow.texel_error=0.05` (default 0.5) | 0 | — | a tenth of the default accepts nothing that casts |
| `t-err0001`, `t-err0` | 0.001, 0 | 0 | — | |
| `t-cover0` | `renderer.shadow.texel_cover=0` | 34,436 | — | not the coverage half of the cut |
| `fix-base-shadow` | this change | **0** | **0** | the block and the sliver gone |
| `fix-terrain-shadow` | this change, terrain alone | 0 | 0 | |
| `fix-rt-shadow` | this change, `--shadows rt` | 0 | 36 | the rays' picture unchanged |

**What casts it, from the light's side.** Two cameras on the sun's side of the patch looking back at it (`t-from-sun-25`, 25 m along the sun's direction, and `t-from-high`, from above the original eye) see the patch with nothing between it and them: the caster is in no camera's cut. A camera on the patch looking at the sun (`t-look-sun`) sees the sun clear of the hill behind. So the patch is lit in the world and shadowed only in the maps, and the maps' extra geometry is what the texel cut adds: coarser clusters of the ground itself.

## Why the texel cut shadows the ground

R103's rule accepts a transition in a cascade when its DAG error is at most half the cascade's texel, on the argument that half a texel is the maps' constant depth bias, so a receiver drawn coarser than the picture stays inside the bias. Two things break that for the ground:

- **A terrain level's DAG error bounds its rest heights, not what it draws.** The pool's terrain stage moves every vertex onto the field it draws, and a world tile draws the vertices it shares with a coarser tile from that tile's level ([renderer](../subsystems/renderer.md#the-ground-from-the-worlds-tiles)); the clusters' spheres are padded for it, but the error is the rest mesh's. A coarser cluster drawn from the field can stand well off the finer cut over the same ground.
- **A world error is deeper along a slanting light.** A surface offset by e along its normal is e / (N·L) further along the light from the receiver beneath it; with the lookup's normal offset (three eighths of a texel times 1 − N·L) the bias holds for e up to 0.375 + 0.125 N·L texels, so half a texel is over the line at any sun but overhead. That margin is small for a rigid mesh whose error is its real deviation; it is not small for a terrain level whose error is not.

The block's sweep agrees: a tenth of the default error draws nothing that casts, and the coverage half of the cut, which a terrain level does not carry, changes nothing.

## The fix and what it costs

`cluster_cull.slang` takes `cascade_accepts` only for an instance that is not a terrain level, so the ground in every cascade is the picture's cut — the receiver's own triangles at the depth the resolve's receiver plane predicts, which is the rule that held 0 acne on the Khronos samples before R103. Everything else keeps the texel cut. The cascades' pairs on the reproduction (the `engine-view: shadow cascade summary` line, cascades 0 to 3):

| | c0 | c1 | c2 | c3 | sum |
|---|---:|---:|---:|---:|---:|
| the reproduction, before | 379 | 904 | 4,711 | 5,623 | 11,617 |
| the reproduction, after | 497 | 1,119 | 5,002 | 6,137 | 12,755 |
| terrain alone, before | 52 | 762 | 4,358 | 5,476 | 10,648 |
| terrain alone, after | 170 | 977 | 4,649 | 5,990 | 11,786 |
| terrain alone, texel cut off | 170 | 977 | 4,649 | 6,462 | 12,258 |

The terrain alone after the change is the texel-off cut in the first three cascades, as it should be; the last differs by the water bodies, which are rigid and keep the cut. The ground is a few thousand pairs a cascade (a tile level is about 5,500 pairs live), so the change adds about a tenth to this scene's cascades and a smaller share to a forest's, whose cascades are trees: 1.48 million pairs at 11520×2160 on the v18 forest after R103.

## The test

`terrain shadows: the ground takes the picture's cut in every cascade` (`systems/renderer/tests/terrain_shadow_tests.cpp`): the moving-dunes fixture of the case beside it under the sun at 34°, 20° and 12°, the cascades drawn with the texel cut and without it, held to the same pairs, the same ids and the same bytes. Without the change it fails on all three suns — 58 against 109 pairs, 9 sand pixels different at 34°; 61 against 112 and 16 at 20°; 66 against 117 and 36 at 12° — the same mechanism as the ledge, smaller because 64 m of dunes is a smaller ground than the v29 foothills.

## What it does not settle

The same depth argument applies, at its small margin, to a rigid mesh that receives its own coarser clusters at a low sun; the Khronos case and the simplified-receiver case hold that at today's suns, and nothing in this reproduction showed it. If one does, the cascade's accepted error wants the light's elevation in it, or the resolve's bias the accepted error over N·L.
