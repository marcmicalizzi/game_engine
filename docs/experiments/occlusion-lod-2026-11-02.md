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

### Why so little moves: where the hidden pairs' next level is

A second counters flight (the two markers held, the world frozen) asked of every drawn pair at least
half hidden how far its parent transition was from being accepted. At marker 1 (k = 0), of 947,258
such pairs:

| the parent transition | pairs | share |
|---|---|---|
| none: the pair is at its DAG's root | 600,001 | 63.3 % |
| within 2 × the threshold | 9,386 | 1.0 % |
| within 5 × | 875 | 0.1 % |
| farther (but not the root) | 336,996 | 35.6 % |

Marker 2: 472,752 at the root, 7,578 within 2, 1,455 within 5 and 232,575 farther. **Nearly two
thirds of what is hidden is already at its coarsest form** — the far forest's distance forms and the
trees' stars — and almost all the rest is more than five times its threshold from the next level,
past what any k on this page reaches. A threshold factor can move about one hidden pair in a
hundred. The forest behind the forest costs what it costs because it is *built at all*, not because
it is built too fine: the lever that would return it is a different one (below).

### Timings on the walk

msvc-release, RTX 5090, `--repeat 1 --wait-quiet 120` (no quiet machine was found: others held 51–71 %
of the CPU at the end of each flight; the GPU lock was this flight's, so no other engine flight shared
the GPU — every number is an upper bound). Medians of the 150 walk frames of each repetition, GPU ms.
"Both" is the term on both thresholds (`renderer.lod.occluded_foliage_only false`), "foliage" on the
coverage threshold alone (the default since). `rt_built` is the cluster structures the frame built.

| 1920 × 1080 `--raster rt` | k = 0 | both, k = 4 | both, k = 8 | foliage, k = 4 | foliage, k = 8 |
|---|---|---|---|---|---|
| visible pairs | 796,529 | 810,612 | 803,607 | 826,055 | 821,716 |
| structures built | 130,768 | 99,374 | 89,867 | 116,124 | 110,589 |
| cull / hiz | 0.49 / 0.01 | 0.51 / 0.01 | 0.51 / 0.01 | 0.51 / 0.01 | 0.51 / 0.01 |
| CLAS build | 3.02 | 2.30 | 2.11 | 2.87 | 2.65 |
| chain (rt) | 5.15 | 4.38 | 4.13 | 4.94 | 4.71 |
| trace | 1.19 | 1.14 | 1.14 | 1.39 | 1.34 |
| resolve | 2.44 | 2.45 | 2.45 | 2.35 | 2.35 |
| **frame** | **11.17** | **10.46** | **10.19** | **11.45** | **11.14** |

(The foliage flight's k = 0 column read 11.64 ms, 3.03 CLAS, 5.15 chain: the same frames a flight
later; compare each flight's columns with its own k = 0.)

| 11520 × 2160 `--raster rt`, both thresholds | k = 0 | k = 4 | k = 8 |
|---|---|---|---|
| visible pairs | 1,467,433 | 1,451,296 | 1,432,156 |
| structures built | 264,272 | 212,028 | 188,442 |
| cull / hiz | 0.51 / 0.20 | 0.56 / 0.20 | 0.56 / 0.20 |
| CLAS build | 6.08 | 4.80 | 4.35 |
| chain (rt) | 8.08 | 6.83 | 6.19 |
| trace | 10.26 | 10.00 | 9.92 |
| resolve | 23.26 | 23.30 | 23.38 |
| **frame** | **45.96** | **44.35** | **43.88** |

| 1920 × 1080 `--raster hw --shadows rt`, both thresholds | k = 0 | k = 4 | k = 8 |
|---|---|---|---|
| visible pairs | 796,529 | 814,254 | 805,439 |
| structures built | 121,115 | 91,943 | 81,939 |
| CLAS build | 2.87 | 2.23 | 1.94 |
| chain (rt) | 4.94 | 4.26 | 4.03 |
| resolve | 2.37 | 2.27 | 2.27 |
| **frame** | **11.55** | **10.89** | **10.54** |

The term's own costs: the pyramid is 0.01 ms at 1080p and 0.20 ms at 11520 × 2160, the cull pass's
reads 0.02–0.05 ms. What it returns is in the CLAS build of the swaying trees (deformed, rebuilt every
frame), which is where a coarser hidden crown saves anything: the visible pair count does not fall —
it rises a little, because across a scaled threshold the cross-fade's band is wider, and both sides of
a fading transition are drawn.

### The captures

Marker 1 and marker 2, each held 16 frames per k (`holds.json`, `occluded_cycle 16`) with the world
frozen (`--world-rate 0`, so nothing sways between the captures — the first capture flight, at world
rate 1, differed in half its pixels, mostly from the wind), 1920 × 1080, colour and ids. Changed pixels
against k = 0 at the same pose:

| | ids changed | colour changed | colour by more than 16 codes |
|---|---|---|---|
| both, k = 4, marker 1 | 7.8 % | 6.9 % | 3.4 % |
| both, k = 8, marker 1 | 12.7 % | 12.4 % | 7.6 % |
| both, k = 4, marker 2 | 10.9 % | 9.4 % | 3.6 % |
| both, k = 8, marker 2 | 16.1 % | 15.7 % | 6.9 % |
| foliage, k = 4, marker 1 | 2.2 % | 2.4 % | 1.7 % |
| foliage, k = 8, marker 1 | 7.0 % | 7.3 % | 5.5 % |
| foliage, k = 4, marker 2 | 2.1 % | 3.0 % | 2.0 % |
| foliage, k = 8, marker 2 | 6.2 % | 7.7 % | 4.6 % |

**Where.** On both thresholds the changed pixels are mostly **visible mid-distance trunks** (the same
instance, another cluster: a level down) and the crowns around them. The hidden share is the
*bounding rectangle's*, and a trunk is a thin thing in a large rectangle: in a forest the rectangle
of a trunk 20 m off is mostly nearer trees' leaves, so the trunk reads as hidden while every pixel of it
shows. The owner asked for exactly the opposite — keep the trunk, coarsen the crown — so the term now
moves the foliage threshold alone by default (`renderer.lod.occluded_foliage_only`). Then the changes
are a handful of mid-distance trees behind nearer ones, whose whole tree — the star is the whole tree,
trunk included — switches to its next card level or its star; the shadows of those trees on the
ground move with them (blue in the masks; `IdDiff.cs` in the scratch directory draws them).

## The decision

**`renderer.lod.occluded_scale` stays 0 (off).** On the owner's flags the term returns 0.2–0.5 ms of an
11.6 ms 1080p frame on the foliage threshold alone, 0.7–1.0 ms on both (1.6–2.1 ms of 46 ms at
11520 × 2160), for 2–16 % of the picture changed, and on both thresholds the change falls on what is
in plain view. That is not the trade the subjective-fidelity rule welcomes. It is built, tested and
measured, and a session can turn it on (`--tunable renderer.lod.occluded_scale=4`).

**What would return the hidden forest.** The histogram is the finding: four drawn pairs in five are
wholly hidden from the eye and nearly two thirds of them are already at their coarsest form. Under
`--raster rt` they are built because a shadow ray may meet them. Two levers follow, neither built:
a visibility measure that is the pair's own and not its rectangle's (last frame's ids give, per pair,
the pixels it actually showed — the trunk problem disappears), and, for a wholly hidden pair, a
representation for the shadow rays alone (an opaque proxy or a cheaper caster set built less often),
since the eye sees none of it.

