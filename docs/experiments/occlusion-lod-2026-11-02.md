# Occlusion-aware LOD on the v25 walk (R118, 2026-11-02)

What the occlusion term of the LOD threshold ([renderer](../subsystems/renderer.md#occlusion-aware-lod))
can return on the forest proof of concept's v25 walk, and what it did return. The term multiplies a
transition's LOD thresholds by `1 + k h`, h the hidden share of the transition's sphere in the Hi-Z.

## The facts before the design

**Occlusion under `--raster rt`: there was none.** `resolve_settings` turns two-pass occlusion culling
off whenever the frame traces shadows (`out.occlusion = ... && !out.shadows`) and allows it only on
`--raster hw` and `--raster vertex`, because the acceleration structures are built from one visible
list and pass 2's survivors are a second one. So the owner's frames (`--raster rt --shadows rt`,
`"occlusion": false` in their reports) built no Hi-Z at all, and every pair inside the frustum that the
LOD cut kept was built into the frame's cluster structures, seen or not. The term therefore needed its
own pyramid on that path: R118 builds one from each frame's finished visibility buffer (the traced one
on the ray path) and the next frame's single cull pass reads it **for the term alone**
(`gfx::k_cull_hiz_lod_only`). Plain occlusion culling there is not an option, and is not what the
term is: last frame's depth would drop a pair that came into view since, and a pair hidden from the
eye must still be built because it casts — so on the ray path **the gain is the cut's level, not its
pair count**.

**The Hi-Z's source** is `hiz_build.slang` over the visibility buffer's depth word, the same build
two-pass culling runs (`add_hiz` in `scene_renderer.cpp`), placed after the trace; it writes no tile
mask. In a frame that builds structures every view reads the cut view's pyramid ([one cut for every
view](../subsystems/renderer.md#one-cut-for-every-view)).

**The walk.** The owner's v25 run (`forest_wildlife_poc/reports/lab/20261101-115716-655`), marker 1
(tick 47452, owner frame 6693) to marker 2 (owner frame 6991), every second recorded pose: 150 path
frames, each walk held 16 frames at marker 1 first, the whole repeated three times with
`renderer.lod.occluded_cycle 166` so the three repetitions take k = 0, 4 and 8
(`renderer.lod.occluded_scale 8`). The owner's flags otherwise (`--terrain-tiles --terrain-far 6
--time-rate 0 --world-rate 1 --time-of-day 15 --no-lights --lod 1 --deform-pool-mib 64 --rt-budget-mib
0 --raster rt --shadows rt`, flora motion on at wind 0.7), `--offscreen`, the v24 derived-data copy as
`--ddc` (nothing was cooked into it). Camera path and scripts: the R118 scratch directory
(`make-walk.ps1`, `walk3.json`, `holds.json`, `stats.ps1`, `IdDiff.cs`).

## The histogram: what the lever can return before it is built

One flight with the debug counters (`renderer.lod.occluded_histogram`), 1920 × 1080, msvc-release, RTX
5090, 2026-11-02 03:00Z — beside R115's and R116's flights and with 2.4 GB of physical memory free, so
its timings are not quoted. Every drawn pair's **bounding sphere's** hidden share h, counted per frame
over the 150 walk frames of the k = 0 repetition (on average 832,458 drawn pairs a frame, 2,751 with
no h — their rectangle off the screen or across the near plane — and 53,379 casters):

| h | at the occlusion test's mip (≤ 5 × 5 texels) | one mip finer (≤ 9 × 9) |
|---|---|---|
| [0, 0.1) | 7.0 % | 4.9 % |
| [0.1, 0.5) | 3.7 % | 3.8 % |
| [0.5, 0.9) | 6.4 % | 4.7 % |
| [0.9, 1) | 1.7 % | 4.0 % |
| 1 (wholly hidden) | **81.3 %** | **82.5 %** |
| over half hidden | **89.4 %** | **91.2 %** |
| over 90 % hidden | **83.0 %** | **86.5 %** |

**Four drawn pairs in five are wholly hidden from the eye.** On the ray path nothing culls them: they
are the forest behind the forest, built every frame for the primary rays that never reach them and for
the shadow rays that do.

## What the term returned

### Counters, k = 0 / 4 / 8 (the same flight)

Per walk frame, averaged over the 150 frames of each repetition. "Band" is the transitions whose
ratio of projected error to threshold lay where a factor could decide them, so they read the Hi-Z;
"coarser" and "finer" count drawn pairs the plain thresholds would not have drawn, and pairs they
would have drawn that the term did not (both include the cross-fade's band, which is why k = 0 has
some).

| | k = 0 | k = 4 | k = 8 |
|---|---|---|---|
| drawn pairs | 832,458 | 836,995 | 828,283 |
| transitions reading the Hi-Z | 0 | 341,942 | 357,244 |
| drawn, coarser than the plain cut | 9,175 | 17,027 | 15,386 |
| plain cut's pairs not drawn | 1,496 | 41,804 | 48,912 |

The frame's timings, the captures and the default follow in this page's next revision (R118's measurement flights were queued behind R115's and R116's for the GPU).

