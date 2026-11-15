# Conifer crowns: how much of its outline a crown covers (G203, 2026-11-14)

**What prompted it.** The owner, walking the forest at 3840 × 2160 under traced primaries and
shadows: "it might be texture more than anything but the conifer style trees still look too
sparse, I think the number of needles is fine, it's just they're too thin maybe." This page is
what a real conifer's crown covers of its own outline, what the engine draws of the forest's pine
and spruces at 10, 30, 100 and 300 m, which part of the difference is the geometry, which the
needle image, which the texture chain and which the rasterizer, what was changed in the engine for
every alpha-tested asset, and what is left for the images. The rules that follow are in
[texture](../subsystems/texture.md#alpha-tested-masks), [renderer](../subsystems/renderer.md#alpha-tested-geometry)
and [flora](../subsystems/flora.md#the-conifer). It follows
[conifer shoots](conifer-shoots.md) (G80) and the two faults
[sampling over time](temporal-sampling.md) measured and did not explain.

**In one paragraph.** The owner is right, and it is the image. With every card opaque the solver's
pine and spruce cover 77 to 87 % of their outline, which is where foresters put a healthy crown;
with the forest's images, 16.6 % opaque on the pine and 25.6 % on the spruces, the pine covers 41
to 49 % past 30 m and the spruces 63 to 67 %. An image 45 % opaque puts the same trees at 71 to
79 %. Beside that the engine was drawing less than the images hold, in three places, all fixed
here for any alpha-tested asset: the texture build held each mip's *texel count* at the cutoff
where a sampler draws the *filtered surface* (the count-held levels of a needle mask draw a third
to five sixths of level 0's coverage), the encoder was taking a tenth of the spruce image's coverage at level 0 and
nothing gave it back, and the rasterizers tested the mask through an anisotropic sampler, which
averages a thin mask under the cutoff on every card at a slant. The hardware rasterizer drew these
crowns 27 to 31 % thinner than the rays at 300 m; it now draws within 1 % of them at 10 and 30 m
on every tree and at 100 m on all but the pine (4.5 % under), and 7 to 11 % under at 300 m on the
forest's thin images, where the far form's atlases are what is left (the second table below).
None of it costs a frame anything: the stand's frame at the owner's settings is 0.15 to 0.3 ms
*cheaper*, and the dense image's stand a third cheaper than the thin one's.

## What the literature says

Read tonight, and what each gives. A source marked *recalled* was not reachable tonight (its text
is a PDF this session could not open, or behind a bot wall), is quoted from memory of the
literature, pins nothing in the code, and is to be re-read before anything is pinned to it.

| What | Source | What it says |
|---|---|---|
| Norway spruce needles | [Wikipedia, *Picea abies*](https://en.wikipedia.org/wiki/Picea_abies); waldwissen.net as cited in [conifer shoots](conifer-shoots.md) | 15–25 mm long (10–25 mm), four-sided in cross-section, alive 5 to 7 years |
| A spruce needle's width | A Czech sample measured for needle-area formulas (*Journal of Forest Science* 68 (11), 2022, as its search record gives it; the paper itself not opened) | one sample needle 0.822 mm wide and 0.821 mm thick: about a millimetre, square |
| Scots pine needles | [Wikipedia, *Scots pine*](https://en.wikipedia.org/wiki/Scots_pine) | 2.5–5 cm long and 1–2 mm broad, in pairs, kept two to four years (to nine in the subarctic); vigorous young trees carry needles twice as long |
| The shoot as the unit of interception | Stenberg et al. 1995, *Tree Physiology* 15:705–712, and Stenberg, Kangas, Smolander and Linder 1999 (their abstracts, as in conifer shoots and as tonight's search records give them); Oker-Blom and Smolander 1988, *Forest Science* 34:894–906 (as later papers cite it) | STAR is a shoot's silhouette area over its total needle area, averaged over directions; needles that shade each other not at all would give 0.25 (the mean projection of a convex body is a quarter of its surface), a real shoot less; STAR falls as the canopy opens, so a shade shoot is flat and shows about three times the silhouette per needle mass of a sun shoot; ratios of total to projected needle area of 3.0–4.0 occur in spruce |
| The mean STAR | *recalled*: 0.147 for Scots pine (Oker-Blom and Smolander 1988) and 0.161 for Norway spruce (Stenberg et al. 1995), the figures the canopy radiation models quote | a shoot shows about 60 % of the silhouette its needles would spread out flat: 4 × STAR |
| What foresters rate | USDA Forest Service, Forest Inventory and Analysis, crown-condition indicator (the Phase 3 crowns fact sheet and Schomaker et al. 2007, GTR SRS-102, as their records describe them; the guides themselves are PDFs not opened tonight) | **foliage transparency**: the skylight that passes through the live, normally foliated part of the crown, estimated in steps of 5 % against a printed density–transparency card; **crown density**: what blocks the light, branches included |
| A crown's own openness | Canham, Coates, Bartemucci and Quaglia 1999, *Can. J. For. Res.* 29:1775–1783 (abstract); Astrup and Larson 2006, *For. Ecol. Manage.* 228:241–250 (abstract) | crown openness is the fraction of sky seen through the crown of one tree of a species; it does not depend on the angle of view; individual crowns of interior British Columbia's conifers block more light than eastern broadleaves' and their transmission tracks shade tolerance |
| The numbers of those two | *recalled*: transparency of a healthy conifer rated 10 to 25 %; crown openness of the shade-tolerant conifers (hemlock, redcedar, the firs, spruce) under 15 % and of the pines 15 to 20 % | **a healthy conifer's crown covers about 75 to 90 % of its outline**, the spruces and firs at the top of that and the pines at the bottom, and by Astrup and Larson from any side |

**What that asks of a picture.** Coverage inside the outline is a property of the crown, not of
the distance: a needle is a millimetre wide and under a pixel past three metres at this target's
pitch (0.535 mrad a pixel), so at every distance the owner stands at a crown is seen as the share
of each pixel its shoots cover, and that share is what the foresters' card rates. So the target
is one number per tree, **about 80 % for a spruce and 70 to 75 % for a pine, the same at 10 m and
at 300 m**, and a crown that holds it by threads a tenth of a pixel wide reads thinner than one
that holds it by masses, because one sample a pixel hits or misses a thread whole (sampling over
time, "What the difference holds").

**What a shoot's silhouette is, for an image's author.** By STAR's definition a shoot's mean
silhouette is STAR × its total needle area. For a 10 cm Norway spruce shoot of about 170 needles
of 15 mm and 3.3 mm round (a square needle 0.82 mm a side) that is 0.161 × 84 cm² = 13.5 cm²
inside an envelope 10 cm by 2.6 cm: **half opaque**; a flat shade shoot (STAR towards 0.22) is two
thirds. For a 10 cm Scots pine shoot of 100 to 150 pairs of 4 cm needles the same sum is 45 to
68 cm² in an envelope of 60: **three quarters opaque and more**, a bottle brush. The needle counts
are this page's estimates, not a source's, so the two figures are a guide and nothing is pinned to
them; what they say is that a shoot is a mass with a ragged edge, and an image of separate
strands a fifth or a quarter opaque is not a shoot.

## How it was measured

One tree alone against an empty background, the eye level with the middle of its crown at 10, 30,
100 and 300 m, at the 3840 × 2160, 60° target's pixel pitch in a 1024 × 1024 window (sampling over
time's method: the eye stands still and the look turns by a fraction of a pixel, sixteen offsets
on a 4 × 4 grid inside one pixel, the id channel captured with each). A pixel of a frame is
covered when its id is not empty; **the supersampled coverage** of a pixel is the share of the
sixteen frames that cover it. **The outline** is the convex hull of every pixel any frame covers,
over the crown's rows only (the pine's crown is 19.5 to 34 m up a 34 m tree, the spruces' 4.5 m to
their tips): the envelope through the branch tips, which is what a transparency rating is made
inside. At 10 m the window is 5.5 m across and holds a piece of the crown; at 300 m the crown is
60 to 90 pixels tall. One frame's coverage is within half a point of the sixteen's on every row
below (a point sample is unbiased; the sixteen are for the picture and the run lengths), so the
"supersampled reference" of each column is that column's own number.

The trees: **pine** and **spruce** are the forest's own recipes (`pine-0`, species `pine`, 34 m;
`spruce-new-0`, species `spruce`, 15 m; both name their needle image) and **conifer** its
`spruce-0` (the bare `conifer`, 17 m, the pack's `conifer/needles`); **flat** is the pine and the
spruce again with no image, every card opaque: the geometry's own coverage; **dense** the same two
with a generated spray 45.3 % opaque (a twig with side shoots filling the square, needles five
texels wide at 1024). Settings: `--raster rt` or `hw`, `--shadows rt`, `--lod 1`, `--dither off`,
the world still, `renderer.shadow.micromaps=true`, `renderer.gi.mode=off` (a coverage is an id,
which the bounce does not touch), the conifers' chain on as the forest builds it. Main is
ae9efac5's release binaries copied out of the tree; RTX 5090; the machine busy throughout (CPU 54
to 100 %, other agents building), which no figure here but the times depends on. An image's own
coverage is `engine-content texture <image> <out> --alpha-cutoff 128`, its `mask.coverage`: the
forest's pine image 0.166 (512²), its spruce image and the pack's `conifer/needles` 0.256 (1024²,
the same picture), the dense spray 0.453.

## What is drawn, and what draws less than it should

Coverage inside the outline, percent. *rt* is traced primaries, *hw* the hardware rasterizer.

| tree, image | m | main rt | main hw | hw against rt | every card opaque (rt) |
|---|---|---|---|---|---|
| pine, 16.6 % | 10 | 64.1 | 62.7 | −2.2 % | 97.2 |
| | 30 | 49.0 | 42.3 | −13.7 % | 77.3 |
| | 100 | 41.3 | 35.9 | −13.1 % | 76.9 |
| | 300 | 41.1 | 28.2 | −31.4 % | 78.5 |
| spruce, 25.6 % | 10 | 72.1 | 76.0 | +5.4 % | 87.1 |
| | 30 | 66.2 | 67.3 | +1.7 % | 83.2 |
| | 100 | 67.4 | 65.3 | −3.1 % | 77.2 |
| | 300 | 66.7 | 48.5 | −27.3 % | 78.3 |
| conifer, 25.6 % | 10 | 70.6 | 75.0 | +6.2 % | |
| | 30 | 63.3 | 64.2 | +1.4 % | |
| | 100 | 65.3 | 63.4 | −2.9 % | |
| | 300 | 65.7 | 46.0 | −30.0 % | |

Cause by cause:

1. **The geometry is not the fault.** With every card opaque the pine covers 77 to 79 % of its
   outline from 30 m out and the spruce 77 to 83 %: the solver's branches, laterals and cards put
   foliage where a crown of the literature's coverage has it. (At 10 m the window is inside the
   crown, where a line of sight passes more cards.) The owner's "the number of needles is fine"
   is this row.
2. **The image is most of it.** A card 16.6 % opaque makes the pine's 77 % crown a 41 to 49 %
   one: about half the lines of sight that meet a card pass through every card they meet. The
   spruces' 25.6 % image leaves 63 to 67 % of 77 to 83. The dense spray's 45.3 % gives 71 to 73 %
   on the pine and 75 to 79 % on the spruce (below): **the image's opaque share against the
   crown's coverage is the table to design an image by**, and both of the forest's are under what
   a shoot's silhouette is (above).
3. **The LOD cut and the far form are not it under the rays.** The same views at a tenth of the
   error threshold (the finest geometry everywhere) cover within 1.7 points of the cut's on every
   row (pine at 300 m 42.8 against 41.1; the spruces within 0.4): the chain's coverage fit
   (flora, "The coverage under the mask") holds under the sampler the rays use.
4. **The rasterizer drew a quarter to a third less at 300 m, and a seventh less of the pine from
   30 m**, which is sampling over time's first content fault with its cause: two faults in one
   test. *The chain*: the texture build scaled each level's alpha so that the share of **texels**
   at the cutoff stayed level 0's, and a sampler does not draw texels: a texel scaled just to the
   cutoff among lower neighbours draws nothing under a linear filter, since every blend with a
   neighbour is under the cutoff. On the build's own needle test the count-held levels of 128, 64,
   16 and 8 texels draw 60, 49, 32 and 49 % of level 0's coverage. *The tap*: the rasterizers
   sampled the mask through the material's sampler, which is anisotropic: it takes the level of
   the footprint's shorter axis and averages up to sixteen taps along the longer, and an average
   of a thin mask is under the cutoff. Most of a crown's cards are seen at a slant.
5. **The encoder took a tenth of the spruce image at level 0.** The build measured a level after
   the encoder so as to give back what BC7 moved, but measured texels: the spruce image's strands
   are four texels wide with soft edges, BC7 pulled their edges down, the count did not see it,
   and the filtered surface lost about a tenth of its coverage. Held as a sampler reads it, level
   0 of that image draws its own 25.6 %, and the spruces' crowns gain 4 to 5 points at 10 and
   30 m under the rays as well (72.1 to 76.9, 66.2 to 70.8), where the cards are the image
   itself. The pine's wider strands lost nothing and did not move.
6. **Threads.** At 100 m the pine's covered pixels come in runs of 3.1 with gaps of 4.2 under the
   rays, and 60 % of the outline's pixels are covered in some of the sixteen frames and not in
   others: the crown is a dither whose mean is right. The rasterizer's levels, once they hold
   the coverage (below), draw it as runs of 4.3 with gaps of 5.4 and 39 % partly covered. That
   difference is not a coverage fault and nothing here changes it: a ray tests level 0
   ([renderer](../subsystems/renderer.md#alpha-tested-geometry)).

## What changed, for any alpha-tested asset

- **A level's coverage is its filtered surface's** (`texture::alpha_coverage_filtered`): sixteen
  samples a texel, each the blend a bilinear tap makes there, in integers; every level is scaled
  to hold level 0's, the scale found by bisection with the clamp at 255 inside the measurement;
  the pass closest to the coverage is the one kept; the 1 × 1 level, which can hold all or
  nothing, is stored beside the cutoff so that its blend with the level above is that level's.
  Measured after the encoder, as before, so level 0 is given back what the encoder takes.
- **The rasterizers test one trilinear tap at the level of the footprint's shorter axis**
  (`mask_keeps`, `alpha_mask.slang`), never the anisotropic sampler's average. A first cut used
  the *longer* axis's level, the filtered answer: it brought the spruces within 2 % to 100 m and
  left 300 m 5 to 11 % short, because a card at a slant then reaches the chain's last few texels
  and the far form's atlas mixes its tiles there. One tap of the shorter axis's level is a point
  sample of a held level, and a sample along the longer axis, as a ray's is everywhere: it took
  the pine at 100 m from 11 % under the rays to 4.5, the conifer at 300 m from 11 to 6.8, the
  dense pine at 300 m from 5.9 to 1.5 and the opaque-card trees inside 2 %.
- **`engine-content texture --alpha-cutoff <code>`** builds an image as a material testing it
  would and prints each level's coverage: the number an image's author needs, and how the
  figures above were taken without opening an image.

Coverage inside the same outlines after it, percent; the rays' column moved only where level 0
did (cause 5) and where the far form's atlases recooked:

| tree, image | m | main rt | main hw | branch rt | branch hw | branch hw against rt |
|---|---|---|---|---|---|---|
| pine, 16.6 % | 10 | 64.1 | 62.7 | 64.1 | 64.3 | +0.3 % |
| | 30 | 49.0 | 42.3 | 49.0 | 49.3 | +0.6 % |
| | 100 | 41.3 | 35.9 | 44.6 | 42.6 | −4.5 % |
| | 300 | 41.1 | 28.2 | 44.5 | 39.6 | −11.0 % |
| spruce, 25.6 % | 10 | 72.1 | 76.0 | 76.9 | 76.7 | −0.3 % |
| | 30 | 66.2 | 67.3 | 70.8 | 70.8 | 0.0 % |
| | 100 | 67.4 | 65.3 | 68.3 | 67.9 | −0.6 % |
| | 300 | 66.7 | 48.5 | 68.6 | 62.3 | −9.2 % |
| conifer, 25.6 % | 10 | 70.6 | 75.0 | 76.0 | 75.9 | −0.1 % |
| | 30 | 63.3 | 64.2 | 68.3 | 68.2 | −0.1 % |
| | 100 | 65.3 | 63.4 | 66.3 | 65.9 | −0.6 % |
| | 300 | 65.7 | 46.0 | 68.0 | 63.4 | −6.8 % |
| pine, dense 45.3 % | 10 | 90.1 | 89.6 | 90.1 | 90.2 | +0.1 % |
| | 30 | 72.7 | 70.3 | 72.7 | 72.8 | +0.1 % |
| | 100 | 70.7 | 69.3 | 71.3 | 70.9 | −0.6 % |
| | 300 | 70.9 | 58.7 | 73.3 | 72.2 | −1.5 % |
| spruce, dense 45.3 % | 10 | 84.7 | 84.5 | 84.7 | 84.7 | 0.0 % |
| | 30 | 79.0 | 78.0 | 79.0 | 79.0 | 0.0 % |
| | 100 | 74.7 | 73.3 | 75.0 | 74.1 | −1.2 % |
| | 300 | 74.2 | 55.6 | 75.3 | 71.8 | −4.6 % |
| pine, opaque cards | 100 | 76.9 | 76.7 | 77.1 | 77.0 | −0.1 % |
| | 300 | 78.5 | 70.5 | 79.7 | 79.0 | −0.9 % |
| spruce, opaque cards | 100 | 77.2 | 76.0 | 77.7 | 77.5 | −0.3 % |
| | 300 | 78.3 | 63.1 | 78.9 | 77.4 | −1.9 % |

**What is left, and where.** The 2 % the row asked for holds at 10 and 30 m on every tree, at
100 m on all but the pine, and at 300 m on the opaque-card trees and the dense pine. It does not
hold at 300 m on the thin images (7 to 11 % under, from 27 to 31) nor on the pine at 100 m (4.5 %
under, from 13). Those are the distances where the conifer's far form is drawn (its branch cards
and its star), whose masks are baked atlases of many tiles, sparse where the crown is. **The cause
was not measured; this is the reading that fits the table**: the build holds a level's coverage
over the whole image, so a level's one scale leaves the sparse tiles under their share and the
dense ones over it, and in a crown the dense regions were covered already, so the loss shows and
the gain does not. If so the answer is a level that keeps its coverage **locally** (Yuksel's alpha
distribution, cited in flora, diffuses the error from texel to texel), a row of its own with this
table as its measure; an image of one spray, even across its square, would not need it, and the
dense pine does hold.

The rasterizer's crowns are made of larger pieces than the rays' at the same coverage (the pine
at 100 m: runs of 4.3 pixels with gaps of 5.4 against the rays' 3.4 and 4.1): a level's blobs
against level 0's strands.

## What a frame pays

A stand of 49 of the forest's pines and spruces 5 m apart, the eye 28 m off looking in, 3840 ×
2160, `--raster rt --shadows rt`, `renderer.gi.mode=bounce`, the flora swaying, 120 frames after
8 warm ones, GPU milliseconds from the engine's timers, medians; main's copied-out build against
the branch's, alternating in one lease (2026-11-14 21:41, RTX 5090; **the machine was not quiet**:
the harness recorded other processes at 96 to 100 % of the CPU, this row's own debug build and
container among them, and 6.8 GB of GPU memory held by others; these are GPU times):

| stand | build | frame | resolve | primary rays | hw column: frame |
|---|---|---|---|---|---|
| the forest's images (16.6 %, 25.6 %) | main | 9.17, 9.03 | 5.19, 5.10 | 2.00, 1.98 | 7.29 |
| | branch | 8.83, 8.88 | 4.93, 4.96 | 1.94, 1.93 | 7.23 |
| the dense image (45.3 %) | main | 6.33 | 3.44 | 1.21 | 5.33 |
| | branch | 6.32 | 3.38 | 1.21 | 5.27 |

So the change costs nothing and gives back 0.15 to 0.34 ms where a level-0 mask got its coverage
back (presumably fewer rays pass through a card to test the next; the pass counts were not read). **And density is not a cost under the
rays, it is a saving**: the stand with the 45 % image is 2.8 ms a frame cheaper than the same
stand with the forest's images (primary rays 1.21 against 2.00 ms, the resolve and its shadow
rays 3.44 against 5.19), because a ray that passes a sparse card goes on to test the ones behind
it, and a ray that stops does not. The brief expected the opposite. Memory: an image's bytes do
not depend on what is painted in it, so a card with a third of the memory pays the same for a
dense image as for a thin one; the opacity micromaps' bytes by image were not read.

## What it costs, and what recooks

**The key.** A mask's build has a version of its own (`texture::k_alpha_mask_version`, 2) mixed
into the key of a build with a cutoff and no other, so every alpha-tested image of a cache
recooks once and nothing else moves; `k_texture_cache_version` stays 2. The forest's three needle
images cook in 0.46 s (512²) and 1.2 to 1.3 s (1024²) each in msvc-release on the loaded machine
(the bisection's nine or ten measurements a pass are under a tenth of that; the rest is BC7, up to
four encodes a level as before). The seven-tree measurement scene, cold (seven solves, their
chains' bakes, every texture), loaded and drew its 448 frames in 122 s. A conifer's chain bakes
its atlases through the same build, so their textures recook too; the containers do not
(`k_flora_version` and `k_conifer_chain_version` did not move, and a tree's images are keyed by
the host).

## What an imported conifer gets, and a scene with none

Everything above keys on a material's alpha mode and nothing else: a bought conifer whose cards
carry a painted alpha gets the chain that holds its coverage, level 0 as its author painted it
after the encoder, the same crown from the hardware rasterizer, the vertex pipeline and the
cascades' depth draw as from the rays to within the figures below, and the `mask` report. A scene
with no alpha-tested material builds no mask, keys every texture as it did, and runs no changed
shader path.

## For a pack's author (the forest's pack among them)

- **The pine's image is the first thing to change**: 16.6 % opaque, the crown at 41 to 49 %
  where a pine's is 70 to 75. The spruces' 25.6 % leaves 63 to 71 where a spruce's is about 80.
- **Draw a shoot as a mass**: a twig with its side shoots filling most of the card, needles
  touching, the image **45 to 60 % opaque** (`engine-content texture <image> <out.tex>
  --alpha-cutoff 128`, `mask.coverage[0]`), every needle four texels or more at 1024 so the
  encoder leaves it standing, irregular. The spray used here is 45.3 % and reads as a crown at
  every distance (the pictures); a pine's tufts want more than a spruce's fronds.
- Flora's page said 25 to 35 %; that figure was a single narrow shoot in a square card, and the
  card is most of the crown's area. It is corrected there.

## Not done

- **The far form's atlases at 300 m** (above): 7 to 11 % under the rays on the thin images, the
  cause read from the table and not measured.
- **The engine's own default image.** `matgen` has no foliage set, so a checkout with no pack
  draws opaque cards (dense, and solid up close), and the generated spray above is a scratch
  script. A deterministic `conifer/needles`, `spruce/needles` and `pine/needles` in `matgen`,
  with its coverage pinned by the table above, is the next row.
- **The forest's staging was not loaded**: the branch changes every mask's key, so a load recooks
  into a root of its own, and the night's free commit stood between 4 and 50 GB against the
  40 GB floor.
- **The rays still test level 0** (cause 6): a ray cone choosing the level would draw the masses
  the rasterizer's levels draw, at the same coverage, and is a design of its own beside the
  opacity micromaps, which are baked from level 0.
- **The literature's numbers marked *recalled*** are to be re-read from the papers.
