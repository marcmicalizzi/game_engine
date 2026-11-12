# What the resolve costs at 3840 × 2160 traced, term by term (R148, 2026-11-11)

Roadmap R148. The owner judges the engine at 3840 × 2160 with traced primaries, traced shadows and
the bounce (`--raster rt --shadows rt`, `renderer.gi.mode=bounce`). On the forest proof of
concept's v41 the resolve is 9.8 ms of a 16.1 ms frame at the lake (main, be3826d9, this evening)
and 13 to 17.5 ms at the stream's poses (last night's tables), and [R135's map](frame-cost-map-2026-11-07.md)
divides it only into the eight stages of `renderer.resolve.stop_after`. This page divides it by
**term** and by **what the pixel shows**, on the engine's own scenes, takes out what an exact cut
reaches, and says what the rest would take. It also says what the resolve costs to **build**,
which the frame does not show and this night's machine did.

**The answer.** On the engine's own scenes at 3840 × 2160 traced with the bounce, three things are
the resolve, and none of them is the material:

1. **The indirect light's read is the largest term wherever a surface is on screen**: 1.2 to 1.3 ms
   of a 2.8 to 3.2 ms resolve over open ground (0.15 to 0.32 µs a covered pixel, dearest on a
   slope), 1.17 of 2.7 ms under the elder grove. Extrapolated to a frame that is all surface it is
   2 ms and more. It is the indirect light's row's (R69) and was not touched.
2. **A scene that has water pays for the water module on every pixel**: the same ground drawn
   with and without two ponds in the scene resolves in 3.20 and 2.46 ms from a pose with no water
   in view; 0.18 ms of the difference is the wet ground's lookup, the rest is the module's own
   weight. It is the water's row's (S101).
3. **The sun's ray costs what it meets**: nothing over open ground (0.02 ms for every ground pixel
   of a frame), 0.46 ms of 2.7 under two megaflora crowns where no candidate reaches the shader at
   all, and on the forest 4 to 8.5 ms by R135's map, most of it the masks of casters that carry no
   micromap. The forest was not flown by this row tonight (see "The forest").

What an exact cut reaches in the resolve's own files is small, and two were made: **the pixel's
triangle is fetched and projected once** (it was up to five times), and **a layered ground's root
is no longer read twice**. Together 0.05 to 0.10 ms of a 1.8 to 2.9 ms resolve (2 to 5 %) on
ground, 0.03 ms (1.2 %) under trees, the capture's bytes unmoved, and 21 to 27 KB less SPIR-V a
variant. A third, the ground's 64-bit integer work done in 32 bits, was built, measured at 0.045 ms
on plain layers and nothing on stochastic ones, and taken out again.

## How it was measured

- **The stages** are R115's: `renderer.resolve.stop_after=cycle` ends every pixel's work after a
  named stage, a frame per stage ([renderer](../subsystems/renderer.md#where-the-resolves-time-goes)).
- **The terms and the classes** come from a scratch build that is never committed (the patch is
  `measure.patch` in the row's scratch directory): the stop word's unused bits carry a **class**
  (only the pixels of one class are shaded, the others return black after the visibility word, the
  pair table, the instance and the material's flags) and **kills** (a term skipped by a branch
  that is uniform over the draw: the layered ground's call, its layers with the mask still read,
  the formation's band, two of a stochastic layer's three tiles, a mesh's normal map, a material's
  own textures, the indirect light's read, the wet ground's lookup, the open sea's test, a sun ray
  not traced where the triangle's own plane faces away, the triangle's second to fifth fetch), and
  on the CPU side every caster solid and the water's reflection and refraction rays off. Under
  `cycle` frame f draws word f mod 80 of a fixed table: the eight stages for every class together
  and for each of five classes alone (the ground: a layered terrain; a rigid mesh; an alpha-tested
  one, the leaves; water; a deformed opaque one, the trunks and blades), the floors (every pixel
  killed before and after it is classified), each kill, and the picture four times. A pose is
  held for a whole number of cycles, the first left out, and a word's figure is the median of its
  frames (4 a word on the engine's scenes, GPU timers).
- **The classes' shares** are a capture of a word that writes the class instead of a colour, and
  the sun ray's candidates and masks R115's counting bit, both with no medium in the frame.
- **Pairs** are two executables copied out of the tree, alternated in one hold of the GPU lock
  (base, cut, base, cut), each a flight of the same path; a figure is the median of a pose's
  frames.
- **The machine** was never quiet: a dozen agents were building. Other processes held 55 to 100 %
  of the CPU in every run and the harness's `quiet` was false throughout, so every figure is the
  GPU's timer under load and a pair is read as a difference, not as two budgets.

**What the method cannot separate.** (a) A class alone costs more than its share of the whole
where classes interleave: a fragment shader runs in warps, and a warp that holds one leaf pixel
among branch pixels runs the leaf's path for the time it takes. Under the elder grove the three
covered classes alone sum to 3.85 ms where the picture is 2.70. A class's figure alone is an upper
bound; the lower one, the picture less a frame of every class but it, is in the forest's build of
the patch (`measure-v2.patch`, 88 words, which also kills the air and the sky's light on the
surface), flown on the elder grove only ("The 88-word cycle", below). (b) A kill leaves the term's registers
allocated: a fragment shader's registers are sized for its largest path, so a term's figure is the
work it does and not what compiling it in costs the rest (the water module, below, is that cost
measured from outside). (c) The cycle's picture frames read 3 to 10 % dearer than a plain flight's
(2.70 against 2.47 ms under the grove): a frame after a cheap frame, on a GPU that is not held
busy. Every difference on this page is inside one flight.

## The engine's own scenes at 3840 × 2160, traced, the bounce on

Resolve milliseconds, `msvc-release` at be3826d9 with the scratch patch, RTX 5090, 2026-11-11.

### ground-by-place (masked ground of colour layers, two ponds, a sky)

Four poses of its camera path held 400 frames each. The scene's own hour is night, so **no pixel
asks the sun** and the sun's stage is the cost of not asking. The shares are of the frame's pixels.

| | sandy shore | trail | mountain | boggy pond |
|---|---:|---:|---:|---:|
| sky / ground / water, % of pixels | 35 / 45 / 21 | 28 / 72 / 0 | 51 / 49 / 0 | (as the shore) |
| layers read a ground pixel | 1.59 | 1.73 | 1.41 | |
| **the picture** | **2.61** | **3.20** | **2.77** | **2.46** |
| decode (the floor: 0.10 with every pixel killed) | 0.11 | 0.11 | 0.11 | 0.11 |
| + reconstruct | 0.40 | 0.44 | 0.29 | 0.40 |
| + material | 0.58 | 0.81 | 0.59 | 0.60 |
| + lighting | 0.89 | 1.54 | 1.50 | 0.90 |
| + the sun's query (nobody asks) | 0.01 | 0.02 | 0.01 | 0.00 |
| + the lights' queries | 0.11 | −0.01 | −0.01 | 0.00 |
| + the air, the exposure, the sky's pixels | 0.15 | 0.15 | 0.17 | 0.14 |
| + the water and what is left | 0.36 | 0.14 | 0.11 | 0.31 |

A term off, the picture less it:

| term | sandy shore | trail | mountain | boggy pond |
|---|---:|---:|---:|---:|
| the indirect light's read (`scene_ambient`) | 0.56 | **1.20** | **1.31** | 0.59 |
| the layered ground, whole (`sample_terrain_material`) | 0.51 | 0.82 | 0.52 | 0.48 |
| — its layers alone (the mask still read) | 0.32 | 0.39 | 0.21 | 0.26 |
| — so the mask, the record and the map's frame | 0.19 | 0.43 | 0.31 | 0.22 |
| the triangle fetched and projected again (upper bound) | 0.13 | 0.20 | 0.12 | 0.11 |
| the wet ground's lookup, on pixels that are not water | 0.11 | 0.18 | 0.11 | 0.12 |
| the open sea's test | 0.01 | 0.01 | 0.01 | 0.00 |
| the material's own textures (the root's dead read) | 0.03 | 0.01 | 0.02 | 0.03 |
| the water's reflection ray / refraction ray / both | 0.11 / 0.05 / 0.14 | 0 | 0 | 0.06 / 0.02 / 0.09 |
| the sky's pixels alone, over the floor | 0.08 | 0.07 | 0.12 | 0.07 |
| the water's pixels alone, over the floor (21 % of the frame) | 0.78 | | | 0.67 |

Read per pixel: a ground pixel's indirect read is 0.15 (the shore), 0.20 (the trail) and 0.32 µs
(the mountain's slopes); its layered material 0.14 µs for 1.7 colour layers; its reconstruction
0.07 µs; a ground pixel whole is 0.52 µs. A water pixel costs 0.45 µs (an upper bound, by (a)), of
which its two traced rays are under a fifth. A sky pixel costs 0.02 to 0.03 µs: it reads its visibility word and the sky's table
and nothing walks the function to learn it is sky.

**The same ground without the ponds** (a scratch copy of the scene with its water, its fire and
its two pond regions removed, so the renderer draws with the module that has no water in it), the
trail's pose: **2.46 ms where the scene with ponds reads 3.20**, the same pixels on screen. The wet
ground's lookup is 0.18 of the 0.74; the open sea's test nothing; the rest is what a module with
the water compiled in costs every pixel that is not water (its registers, and the branch on the
material's flags in front of every stage). The split of 2026-10-18 into water and plain modules
bought this back for scenes with no water; a scene with water pays it whether or not a frame shows
any.

### elder-grove (two megaflora trees, willows, birches, a flat ground of the renderer's own)

One pose held 400 frames: sky 50 %, the ground and other rigid meshes 29 %, leaves 11 %, trunks and
branches 10 %. The stand-in sun is up and the stand-in's two lights are on.

| stage | ms | | term off | ms |
|---|---:|---|---|---:|
| **the picture** (2.47 in a plain flight) | **2.70** | | the indirect light's read | **1.17** |
| decode | 0.09 | | every caster solid | 0.14 |
| + reconstruct | 0.13 | | the material's own textures | 0.10 |
| + material | 0.15 | | the triangle fetched again (upper bound) | 0.07 |
| + lighting | 1.31 | | a mesh's normal map | 0.04 |
| + the sun's query | 0.46 | | a ray not traced where the triangle's own plane faces away | 0.03 |
| + the lights' queries | 0.67 | | | |

The sun's ray here meets **no candidate at all** (the counting bit: 0 candidates and 0 masks on
every asking pixel; the micromaps decide every leaf), so its 0.46 ms is traversal, and forcing every
caster opaque still returns 0.14 of it: a query that may meet a non-opaque candidate is dearer than
one that cannot, whether or not it meets one. 42 % of leaf pixels ask the sun and 72 % of those are
blocked; a quarter of the asking leaf pixels have their own triangle's plane facing away from the
sun, and 90 % of those are blocked. Alone, the leaves' pixels (11 % of the frame) cost 1.66 ms and
the trunks' (10 %) 1.56 against 0.63 for the rigid 29 %: the crowns are where the lighting, the
sun's ray and the lights' rays are spent, and (a) above is why the three do not sum.

### The 88-word cycle, flown on the elder grove (2026-11-11, 23:22)

`measure-v2.patch` on be3826d9, the same pose and flags, `--no-medium`, one flight, 400 frames, the
first cycle left out (3 to 4 frames a word, each word's spread 0.01 to 0.02 ms). The machine was
quieter than for the tables above: other processes at 16 % of the CPU, `quiet` false. It agrees
with the 80-word table: the stages' steps 0.09, 0.14, 0.15, 1.29, 0.48 and 0.68 against 0.09, 0.13,
0.15, 1.31, 0.46 and 0.67; the indirect read's kill 1.22 against 1.17; every caster solid 0.11
against 0.14; the material's textures 0.06 against 0.10; the triangle again 0.03 against 0.07; the
census the same shares to a tenth of a percent. Its picture word reads 2.82 against 2.70 (the
80-word flight's stages already summed to 2.81, above its own picture word). What it adds:

| class (share of pixels) | alone (upper bound) | the picture less every class but it (lower bound) | µs a pixel |
|---|---:|---:|---:|
| rigid meshes (29.3 %) | 0.68 | 0.42 | 0.17 to 0.28 |
| leaves (10.6 %) | 1.82 | 0.69 | 0.79 to 2.07 |
| trunks and branches (10.2 %) | 1.66 | 0.53 | 0.63 to 1.96 |
| sky (49.8 %) | 0.08 | 0.01 | under 0.01 |

The lower bounds and the floor (0.08) sum to 1.73 of the 2.82 ms picture: **1.1 ms of this frame
belongs to no class alone**, the warps where leaf, trunk and ground pixels meet, which is (a)
measured. A leaf pixel costs three to eight times a rigid one either way. The two kills it adds,
the air and the sky's light on the surface, are under 0.01 ms each here (the stand-in sky).

## The forest

Not measured. **One flight was made on 2026-11-12 and died before its first frame**: the
88-word binary on `scene-staging.json` with the six-pose path, the staging note's flags, 3840 ×
2160, `--raster rt --shadows rt`, the bounce, the pipeline cache on (the measure binary's resolve
pipelines in none of it), started at 00:24 with 59.4 GB of free commit and the machine near idle
(CPU 6 %), aborted 7.4 minutes in, ten seconds after the micromap stage lowered its level cap from 6
to 5: `assertion failed: p != nullptr, out of memory` in `core/memory`. A minute later the machine
had 61 GB free and no other process held more than 10 GB, so it was the run's own peak and not a
neighbour's. The staging note measured 52 GB for a cold-pipeline load of the same scene on main;
the log (warnings only) does not say whether the driver still held this binary's cold pipelines'
compile when the micromaps were built. A second flight was not made: nothing the row could change removes
that peak. What would: the measure binary's pipelines compiled into the cache by a run that stops
before the micromaps, or more free commit than the 59 GB this one had. Before that night: a forest run on be3826d9 needed 52 GB and, with a cold pipeline, 18 to 24
minutes to its first frame; the rule set during the night was 60 (later 55) GB of free commit
before a run, and the machine offered 35 to 54 GB each of the three times this row looked
(continuously from 20:46 to 21:16 and from 21:20 to 21:45, and at 21:47: 49.7 GB), after which the
row's budget went to its checks. The figure the table would take apart is main's own,
taken by the staging row (T116b) this evening: at the lake held, 3840 × 2160, traced primaries and
shadows, the bounce, 121 frames, other work at 50 to 59 % of the CPU — **frame 16.1 ms, resolve
9.78**, trace 1.91, the ray tracing chain 1.53 (clas 0.45, tlas 0.37, blas 0.27), frame passes
0.88, the bounce's trace 0.76, cull 0.57, deform 0.46.

What this page's per-pixel figures say of that 9.78 ms, as an estimate and not a measurement: with
every pixel covered, the indirect read is about 2 ms (0.2 to 0.3 µs a pixel), the reconstruction
0.6, the water module's weight 0.5 to 0.7, the air 0.2 to 0.3; the rest, some 6 ms, is the sun's
ray under and through the crowns, the water's own pixels at 0.45 µs and more, and the ground's
textured stochastic layers, in that order by [R135's map](frame-cost-map-2026-11-07.md) (the sun's
stage 4.1 to 8.6 ms at 3840 on v34, 2.5 to 5.9 of it the masks). The measure binary, its path
(six poses: the lake held, the open lake, under the canopy, the forest floor, the stream's reach,
the eastern coast) and its reader are in the row's scratch directory, proven on the elder grove;
what stops it is the load's memory, above.

## The cuts

**1. The triangle is fetched and projected once** (`reconstruct_screen_kept` and the `_at` forms in
`material.slang`; `fs_resolve`). The reconstruction fetched the pixel's triangle (the mesh's
record, the cluster's, the packed indices, the pool's slot, three positions) and put its corners
through the view's matrix for the barycentrics; then the UV's derivatives did both again for every
mipmapped material, the layered ground's footprint again, the tangent frame fetched it again for a
normal map, the normal's variance again and the ground's detail again: up to five fetches and four
projections a pixel, each inside its own branch, which the driver's compiler does not merge. The
kept pair is the same arithmetic on the same operands, so nothing moves.

**2. A layered ground's root is read once.** `fs_resolve` read the material's own maps at the
mesh's UV before it knew the material was a layered ground, and `sample_terrain_material` then
read the root again at the map's UV and replaced the first result whole.

**The pair** (3840 × 2160, traced, the bounce; resolve ms, the median of 340 frames a pose; two
runs of each executable alternated; other processes at 55 % of the CPU):

| scene and pose | base | base | cuts 1 + A | cuts 1 + A | Δ |
|---|---:|---:|---:|---:|---:|
| ground, plain layers, shore | 1.808 | 1.809 | 1.718 | 1.719 | −0.090 (−5.0 %) |
| — trail | 2.461 | 2.460 | 2.358 | 2.357 | −0.103 (−4.2 %) |
| — mountain | 2.278 | 2.276 | 2.198 | 2.200 | −0.078 (−3.4 %) |
| — pond's place | 1.822 | 1.830 | 1.726 | 1.744 | −0.091 (−5.0 %) |
| ground, stochastic layers, shore / trail / mountain / pond's place | 1.896 / 2.915 / 2.780 / 1.922 | | 1.812 / 2.851 / 2.729 / 1.818 | | −0.084 / −0.064 / −0.051 / −0.104 |
| elder-grove | 2.784 | 2.758 | 2.749 | 2.725 | −0.034 (−1.2 %) |

"A" is the cut taken out below; against a build with A alone, cut 1 by itself is −0.041, −0.069,
−0.052 and −0.054 ms on the plain ground's four poses. The final pair, base against the two cuts
as committed, with the captures compared, is in the table under it.

**The final pair**, base against the two cuts as committed (the same flights, two runs of each
alternated, other processes at 68 to 89 % of the CPU):

| scene and pose | base | base | cuts | cuts | Δ |
|---|---:|---:|---:|---:|---:|
| ground, plain layers, shore | 1.806 | 1.810 | 1.748 | 1.749 | −0.060 (−3.3 %) |
| — trail | 2.458 | 2.461 | 2.383 | 2.385 | −0.076 (−3.1 %) |
| — mountain | 2.278 | 2.278 | 2.219 | 2.218 | −0.060 (−2.6 %) |
| — pond's place | 1.815 | 1.814 | 1.757 | 1.775 | −0.049 (−2.7 %) |
| ground, stochastic layers, shore | 1.895 | 1.893 | 1.843 | 1.845 | −0.050 (−2.6 %) |
| — trail | 2.912 | 2.912 | 2.812 | 2.810 | −0.101 (−3.5 %) |
| — mountain | 2.779 | 2.778 | 2.718 | 2.718 | −0.061 (−2.2 %) |
| — pond's place | 1.938 | 1.898 | 1.876 | 1.849 | −0.056 (−2.9 %) |
| elder-grove | 2.780 | 2.782 | 2.751 | 2.750 | −0.031 (−1.1 %) |

**The picture.** With the bounce off, base and cuts draw the same bytes: 27 captures at
3840 × 2160 (three frames at each of the ground's four poses, plain and stochastic, and three
under the grove; the picture, and the two measurement words the base ignores), **0 pixels differ
in every one**. With the bounce on a capture cannot say: two runs of the base itself differ by one
code on 0.2 to 3.7 % of the ground's pixels (2 codes on a handful) and by up to 40 codes on 12 % of
the grove's, the cache's own variation from run to run, which is a finding for the indirect light's
row and not this one's (the picture is meant to be a function of the scene and the eye). The
reconstruction's probe (`reconstruct_tests.cpp`) holds the kept forms to the forms that fetch and
project for themselves, bit for bit, on every covered pixel of its cases.

**What the frame budgets should do**: nothing a tolerance sees. None of the budget scenes has a
layered ground or a water module; their resolves are 0.2 to 2.5 ms and cut 1 is worth 1 to 3 % of
that (the erg's and the endless desert's `resolve` medians should fall by 0.01 to 0.06 ms at
11520 × 2160 and not rise anywhere). No rebaseline is asked for.

**Measured and taken out: the ground's integer work in 32 bits.** A ground pixel divides the
frame's integer cell (64,000 mm a cell) by a pitch in whole millimetres in 64 bits: four divisions
for the mask's tile and point, three remainders a layer for its phases, two a projection of a
stochastic layer for its lattice. T94 found the formation's 64-bit divisions worth taking out this
afternoon, so the same was built for the ground: the same integers from 32-bit divisions for any
pitch under 2^24 mm, the 64-bit forms kept past 64,000 km (`cutA.patch` in the scratch directory;
it extends the mask sampler's test to a record 32,000 km out so that each form meets the reference,
and was dropped before that test was run). **It bought
0.045 ms on plain layers (1.804 → 1.758, 2.484 → 2.431, 1.809 → 1.765; the mountain inside the
noise) and nothing on stochastic ones (1.897 → 1.907, 2.915 → 2.945)**, for 25 KB more SPIR-V a
terrain variant. The card's 64-bit division is not what the ground costs. Not kept.

## What the resolve costs to build

Sixteen variants of one source (ray-traced or not, water or not, layered ground or not, stochastic
mesh materials or not). `slangc` with the build's flags, one variant at a time, on be3826d9
(this row; T117's `REPORT.md` has the same two ends and the driver's side):

| variant | SPIR-V, bytes | `slangc`, s | peak commit, MB |
|---|---:|---:|---:|
| plain | 371,728 | 3.2 | 409 |
| rt | 417,092 | 4.3 | 411 |
| stochastic / rt stochastic | 396,640 / 441,972 | 4.3 / 10.9 | 443 / 508 |
| terrain / rt terrain | 566,148 / 611,448 | 5.5 / 6.4 | 580 / 740 |
| terrain stochastic / rt terrain stochastic | 650,504 / 695,804 | 9.1 / 11.5 | 836 / 836 |
| water / rt water | 1,084,696 / 2,481,616 | 10.7 / 33.4 | 997 / 2,536 |
| water stochastic / rt water stochastic | 1,109,528 / 2,676,240 | 12.3 / 54.0 | 997 / 2,838 |
| water terrain / rt water terrain | 2,138,916 / 4,234,152 | 22.8 / 81.0 | 2,504 / 7,185 |
| water terrain stochastic | 2,462,456 | 58.4 | 3,939 |
| **rt water terrain stochastic** (the forest's) | **5,103,684** | **139.4** | **12,699** |
| all sixteen | 25.4 MB | 467 | |

**Why.** Slang emits one function with everything inlined, so a call site is a copy. The layered
ground's function holds **five copies of a layer's whole body** (the mask's four unrolled slots
and the unmasked loop), and a layer's body holds three unrolled stochastic projections of three
unrolled tiles of four reads each, three plain projections, the painted form, the formation's band
(four lookups of nine written-out halvings each) and the sand and snow responses. In a module with
no water that function is instantiated once and costs 0.19 MB (rt: 0.42 → 0.61). In a water module
it is instantiated again for the bottom under the water, and the ground's reduced form once for
each traced hit that decodes itself, behind the water's own block of 2.1 MB with its rays: the
ground adds 1.75 MB to the ray-traced water module (2.48 → 4.23) where it added 0.19 to the plain
one, and compiling goes from 33 s and 2.5 GB to 81 s and 7.2 GB. The shadow query is the other
multiplier: `shadow_visibility` has ten call sites in the water module and each is a copy of both
ray queries, the masked loop and the cascades' filter. So the sentence the build side wants: **the
bottom under the water reading the ground by `terrain_hit_material`'s reduced form (the masks'
weights over the layers' means, which is what a reflection already draws) instead of the full
layered function would take one of the two full copies out of every water-terrain variant**, at
the cost of the bed's texture under clear shallow water; and one shadow function called from its
ten or so places rather than inlined in each is the other large piece, if Slang can be made to
emit a call. Neither was built, so neither has a size: the differences above bound each under the
1.75 MB the ground adds.

**What the cuts did to it**: cut 1 takes 21 to 27 KB out of every variant (plain 371,728 →
350,720, −5.7 %; rt 417,092 → 396,084; rt water 2,481,616 → 2,460,628; the forest's 5,103,684 →
5,076,876, and its `slangc` 139 → 129 s at the same 12.6 GB). The cut that was taken out added
25 KB to every terrain variant.

## A fifth and a sixth layer

What two more layers a tile would cost the resolve as it stands, from the figures above, as an
estimate with its basis. The mask's side: a tile's table and its weights are one word a point, four
bytes, so a fifth and sixth layer are a second word a point: the mask's reads go from 4 (8 across
two mips) to 8 (16), on a term that is 0.19 to 0.43 ms of these frames in all (the record, the
map's frame and the mask together), so **under 0.2 ms at 3840 × 2160 for the wider mask on a frame
that is three quarters ground**. The layers' side is paid only where a pixel reads them: a layer
read costs 0.39 ms / (1.73 layers × 6.0 million ground pixels) = **0.04 µs a colour layer a pixel**
here, and the forest's textured stochastic layers several times that: R135's map has the whole
material stage at 1.6 to 3.1 ms at 3840 on v34, which if half of it is the ground's and a ground
pixel reads two layers is of the order of 0.1 to 0.2 µs a layer a pixel (an estimate; this row did
not fly the forest). A pixel that today reads two layers and would read three under a six-layer
tile pays one layer more; if a tenth of the ground's pixels did, that is of the order of 0.1 ms
on the forest at 3840 × 2160. What does
not scale with the pixels is the compile: the layer's body is unrolled per slot, so six slots are
seven copies of it where there are five, 40 % more of the ground's function in every terrain
variant (about 0.1 MB a plain variant, 0.7 MB of the forest's) unless the slots become a loop —
and T94 measured this afternoon that a loop in this function's leaf cost every ground pixel
0.08 ms at 1080p. So the format's cost is in the module's size before it is in the frame.

## The rows this leaves, most valuable first

| Term | Now | What would halve it | In the picture | Size |
|---|---|---|---|---|
| The indirect light's read | 1.2 to 1.3 ms of 2.8 to 3.2 on ground at 3840, 0.15 to 0.32 µs a pixel | R69's own (its cost matrix was being flown tonight): one level's cells where the young cells are confident, the cells' addresses found once a pixel and not per level and face | none if the blend's weight is zero where it is skipped | M, R69 |
| The sun ray's masks on the forest | 2.5 to 5.9 ms of the sun's 4 to 8.5 at 3840 (R135, v34) | micromaps on the casters no primary ray can reach (a caster outside every view's frustum is never met by a primary ray, which is the reason a caster carries none); then the query that cannot meet a candidate, by region | none: the same masks, decided by the traversal | M, the chain's |
| The water module on pixels that are not water | 0.5 to 0.7 ms a frame at 3840 with no water in view | the plain module for a frame whose views hold no water body, shore band or channel (the CPU knows the bodies' bounds); the wet ground's lookup behind a per-tile bit | none | M, S101 and the renderer |
| A query that may meet a candidate | 0.14 of 0.46 ms under the grove with no candidate met | two pipelines' worth is not needed: the opaque query for a receiver whose ray can be shown to cross no masked instance (a top-level mask bit for instances with a masked cluster) | none | M |
| The ground's mask, record and map frame | 0.2 to 0.4 ms on ground at 3840 | the mask's two mips read only inside the blend's band, the wetness bytes behind the tile's own "any wet" bit, the record's constants (the points' count, the mips' offsets) stored and not looped for | none | S |
| The resolve's build | 25 MB of SPIR-V, 467 s of `slangc`, 12.7 GB for the forest's variant | the bottom under the water by the reduced ground; the shadow query emitted once | the bed under shallow water loses its texture | M |
| The lights' queries | 0.67 ms under the grove for the stand-in's two lights | already has `light_tolerance`; the forest runs with no lights | — | — |

The frame's other large terms on the forest tonight, for the whole frame's sake (they are other
rows'): the primary rays' pass 1.9 ms, the ray tracing chain 1.5, the medium 0.9, the bounce's
trace 0.8, the cull 0.6 and the pool 0.5 of 16.1.
