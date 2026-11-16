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
| A spruce shoot, measured (read 2026-11-14, second leg) | Palmroth, Stenberg, Smolander, Voipio and Smolander 2002, *Tree Physiology* 22:1185–1192, "Fertilization has little effect on light-interception efficiency of *Picea abies* shoots", [the author's copy](https://samposm.kapsi.fi/papers/fertilization.pdf), Table 3 (305 one-year-old shoots of control and fertilized trees in Sweden, 1995 to 1998, canopy openness 0.02 to 0.96) | spherically averaged **STAR 0.103 to 0.209**, the largest silhouette over total area 0.115 to 0.364; needles **5.6 to 20.8 mm long, 0.57 to 1.9 mm wide and 0.81 to 1.5 mm thick**; **9 to 33 needles a centimetre of twig**; total needle area 2.9 to 12.7 cm² a centimetre of twig; total over projected needle area 2.5 to 4.0 |
| A pine shoot, measured (read the same night) | the operational-method paper on a conifer shoot's intercepted light in *Tree Physiology*, [its author's copy](https://samposm.kapsi.fi/papers/light_interception.pdf) (*recalled* as Smolander and Stenberg 2001, 21:797–803; the file's header did not survive the text extraction, its summary and results did) | one Scots pine shoot from a stand in central Finland: spherically averaged **STAR 0.141**; the needles' shading of each other took 44 % of what they would have intercepted spread out |
| The mean STAR | *recalled*: 0.147 for Scots pine (Oker-Blom and Smolander 1988) and 0.161 for Norway spruce (Stenberg et al. 1995), the figures the canopy radiation models quote; the two rows above bracket them and were read | a shoot shows about 60 % of the silhouette its needles would spread out flat: 4 × STAR |
| What foresters rate | USDA Forest Service, Forest Inventory and Analysis, crown-condition indicator (the Phase 3 crowns fact sheet and Schomaker et al. 2007, GTR SRS-102, as their records describe them; the guides themselves are PDFs not opened tonight) | **foliage transparency**: the skylight that passes through the live, normally foliated part of the crown, estimated in steps of 5 % against a printed density–transparency card; **crown density**: what blocks the light, branches included |
| A crown's own openness | Canham, Coates, Bartemucci and Quaglia 1999, *Can. J. For. Res.* 29:1775–1783 (abstract); Astrup and Larson 2006, *For. Ecol. Manage.* 228:241–250 (abstract) | crown openness is the fraction of sky seen through the crown of one tree of a species; it does not depend on the angle of view; individual crowns of interior British Columbia's conifers block more light than eastern broadleaves' and their transmission tracks shade tolerance |
| The numbers of those two | *recalled*: transparency of a healthy conifer rated 10 to 25 %; crown openness of the shade-tolerant conifers (hemlock, redcedar, the firs, spruce) under 15 % and of the pines 15 to 20 %. From search records on the second leg, the reports themselves still unopened (their PDFs came back empty): foliage transparency classed **normal at 0 to 20 %, moderate 21 to 40 %, severe over 40 %** in a study of the crown indicators' thresholds; and Missouri's softwoods of 2000 to 2003 (shortleaf pine, eastern redcedar) averaging a **crown density of 49.5 %** (USDA GTR SRS-113) | **a healthy conifer's crown covers about 75 to 90 % of its outline**, the spruces and firs at the top of that and the pines at the bottom, and by Astrup and Larson from any side. **An open point**: crown density is rated against the whole crown's outline, gaps between branches included, and this page's outline (the hull through the branch tips) is nearer that than the foliated part transparency is rated in; if density is the right comparison a field crown is nearer 50 % than 80. The target here stays the transparency reading, because the solver's own branches with opaque cards cover 77 to 87 % and the owner's eye called 41 to 67 % too sparse; it is a reading, not a measurement of a tree |

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

**The same from measured shoots** (Palmroth et al.'s table, read on the second leg, which needs
no needle count): a shoot's silhouette a centimetre of twig is STAR times its needle area a
centimetre, from 0.103 × 2.9 = 0.3 cm² to 0.209 × 12.7 = 2.7 cm², about 1 cm² at the middle of
both ranges (0.15 × 7), in an envelope two needle lengths' sine wide: 2 × 13 mm × sin 50° = 2 cm
for the sample's needles. **About half the envelope, from a seventh to all of it**, the sun shoots
at the low end and the shade shoots at the high one. The spruce estimate above stands; the pine's
has one measured shoot behind it (STAR 0.141) and no needle area.

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

## The engine's own sprays

The second leg (2026-11-14, late): `matgen` builds `spruce/needles`, `conifer/needles` and
`pine/needles` ([matgen](../subsystems/matgen.md#spray-sets) has how a spray is drawn and why;
[generated material sets](generated-material-sets.md#needle-sprays-g203-2026-11-14) each number
against its source), so a checkout with no pack has a spray and a pack that drops its own gets
one. The images' opaque share at level 0 (`SetStats::spray_cover`, alpha at a half): spruce
52.8 %, conifer 52.9 %, pine 57.2 %, every stroke four texels wide or more at 1,024.

**Measured as above**: the same pine (seed 17, 34 m) and spruce (seed 41, 15 m) with no needle
image named, so the pack's role is read, and the forest's `spruce-0` (the bare `conifer`), from
a materials root holding only the generated pack; the branch's release build, `--shadows rt`,
`--lod 1`, the bounce off, sixteen offsets a view (2026-11-14 23:30, RTX 5090; other processes at
80 % of the CPU, which no coverage depends on). Coverage inside each set's **own** outline,
percent; the forest's images and the scratch spray are the branch's rows of the table above:

| tree | m | forest image, rt | scratch 45 % spray, rt | **generated, rt** | **generated, hw** | hw against rt |
|---|---|---|---|---|---|---|
| pine (57.2 %) | 10 | 64.1 | 90.1 | **93.1** | 93.1 | 0.0 % |
| | 30 | 49.0 | 72.7 | **75.7** | 75.6 | −0.1 % |
| | 100 | 44.6 | 71.3 | **73.5** | 73.2 | −0.4 % |
| | 300 | 44.5 | 73.3 | **76.6** | 75.6 | −1.3 % |
| spruce (52.8 %) | 10 | 76.9 | 84.7 | **85.2** | 85.3 | +0.1 % |
| | 30 | 70.8 | 79.0 | **79.9** | 80.0 | +0.1 % |
| | 100 | 68.3 | 75.0 | **65.6** | 64.9 | −1.1 % |
| | 300 | 68.6 | 75.3 | **76.6** | 73.6 | −3.9 % |
| conifer (52.9 %) | 10 | 76.0 | | **86.7** | 86.7 | 0.0 % |
| | 30 | 68.3 | | **79.4** | 79.4 | 0.0 % |
| | 100 | 66.3 | | **74.9** | 74.4 | −0.7 % |
| | 300 | 68.0 | | **78.4** | 76.0 | −3.1 % |

- **The pine is in its band** (70 to 80 % from 30 m out; at 10 m the window is inside the crown,
  where opaque cards read 97) and the conifer in the spruce's (75 to 85) at every distance but
  100 m, where it is 74.9. The hardware rasterizer is within 2 % of the rays everywhere but the
  two spruces at 300 m (3.1 and 3.9 % under, where the forest's images were 6.8 and 9.2 % under
  and main 27 to 30): the far form's atlases, as above.
- **The spruce at 100 m reads 65.6 %, and the number is the outline's, not the crown's.** Each
  column is rated inside its own hull, and a spray changes the hull: the generated spruce's is
  13,122 pixels there where the scratch spray's was 10,930 and the opaque cards' 13,633. It
  covers 8,608 pixels of it, more than the scratch spray's 8,198 and the forest image's 7,435,
  and fewer than the opaque cards' 10,593. A spray that fills its card to the far end draws the
  branch tips the tapered scratch spray left empty, so the hull grows by a fifth while the
  covered pixels grow by a twentieth: the outer band is foliage at the tips with sky between.
  **A hull is a poor outline for comparing images**; rated inside one outline for all (the opaque
  cards', the geometry's own envelope) the same view is 54.5 % for the forest's image, 60.1 % for
  the scratch spray, 63.1 % for the generated one and 77.7 % for opaque cards.

The generated sprays again, each rated inside **another image's outline** of the same view (the
run repeated, the same numbers to the tenth; rt):

| tree | m | inside the scratch spray's outline (its own figure) | inside the forest image's (its own) | inside the opaque cards' (their own) |
|---|---|---|---|---|
| pine | 10 | 93.1 (90.1) | 93.1 (64.1) | 93.1 (97.2) |
| | 30 | 76.2 (72.7) | 76.0 (49.0) | 75.1 (77.3) |
| | 100 | 73.9 (71.3) | 75.2 (44.6) | 72.8 (77.1) |
| | 300 | 77.8 (73.3) | 89.0 (44.5) | 74.5 (79.7) |
| spruce | 10 | 86.4 (84.7) | 87.3 (76.9) | 82.2 (87.1) |
| | 30 | 80.7 (79.0) | 81.4 (70.8) | 77.9 (83.2) |
| | 100 | 78.4 (75.0) | 78.7 (68.3) | 63.1 (77.7) |
| | 300 | 76.7 (75.3) | 79.0 (68.6) | 74.1 (78.9) |

Inside the outline the scratch spray was rated in, the generated spruce is 76.7 to 86.4 % and the
pine 73.9 to 77.8 % from 30 m: the bands asked for (75 to 85, 70 to 80), a point and a half over
at the spruce's 10 m. What the last column says of the spruce at 100 m is not answered by an
image: between the scratch spray's outline and the opaque cards' there is a band at the branch
tips that the form drawn at that distance (a baked card group a branch) covers thinly with either
spray, and the generated one leaves a scatter of single pixels in it.

**What the pictures show** (the agent's reading of its own captures, a sky at 15:30 in July, the
bounce on, 3840 × 2160 pitch; before is main with the forest's images, after the branch with the
generated sets). The crowns are closed: no sky shows through a spruce at any distance, and the
pine is a canopy where it was a haze of dashes. **What reads wrong, plainly**: (1) *the cards are
plates*. With a thin image a card was a see-through lace and its flatness did not show; filled,
the pine's cards read as broad fans and the spruce's as dark blades at 10 m, and from under the
stand the pines read as a canopy of large flat leaves. That is the solver's geometry (cards of
0.45 m and their merged rungs, which tile the spray across a plate in rows), not the image, and
it is now the first thing wrong with a conifer up close. (2) *The crowns are dark* on their
shaded side: nothing passes a card (a set has no translucency), so the dense crown shades
itself where the thin one was lit through. (3) *A scatter of single pixels* round the spruce at
100 m. (4) From 100 m the crown is a blob of the right density with less of a spruce's layered
outline than the thin image's speckle suggested.

**What it does not do.** Opaque cards are the ceiling (77 to 87 %), and a spray 53 to 57 % opaque
reaches nine tenths of it. The rest is the cards' count, which the owner judged fine, and the
far form. A card is still a plane, whatever is painted on it.

**A stand's frame** (the 49 trees, 3840 × 2160, rt and rt, the bounce on, 120 frames, GPU medians;
2026-11-14 23:31, **not quiet**: other processes at 58 to 82 % of the CPU; the scene's sky stood
at midnight in this run, so the sun's shadow rays were the moon's; one run a side, and the sides
differ in build and in image, so it is what the dense stand costs beside the thin one and not a
pair): the generated sprays on the branch 4.62 ms a frame (resolve 2.09, primary rays 0.53, the
bounce's trace 0.55), the forest's images on main 6.43 (2.94, 0.83, 0.70). Denser is cheaper, as
the first leg found.

**For the forest.** In the first 4 MB of the staging's scene, 1,278 pine, 650 spruce and 12
conifer recipes name no needle image and read the pack's `pine/needles`, `spruce/needles` and
`conifer/needles` (7 pines and 6 spruces name their own). The pack's three roles win over the
engine's, so the engine's sprays show there only once the pack's manifest drops those roles (or
points them at denser images) and `engine-content matgen --out <pack>/generated` has been run
with this build; either changes the pack's hash, which re-keys every generated mesh built
against it.

## Structure, not mass: the third leg

2026-11-15. The coordinator's reading of the second leg's pictures, and the agent's own: the
generated sprays measured dense and read worse than the thin images they replaced. At 10 and
30 m the spruce was a stack of dark paddles, under the stand the pines were a ceiling of flat fans
with rows of holes, and the shaded sides were near black. The owner had said it: "the number of
needles is fine, it's just they're too thin maybe". The same needles, thicker; not a card filled
in as a mass. This leg is what a real bough looks like, what was changed so that a card reads as
one, and what is still wrong.

### What a bough looks like, and what its needles do with light

Each row says whether its source was **read** this leg, seen only as a search **snippet**, or
**recalled**; nothing is pinned to a recalled number.

| What | Source | What it says |
|---|---|---|
| A spruce's needles and twigs | Gymnosperm Database, [*Picea abies*](https://www.conifers.org/pi/Picea_abies.php) (read); Trees and Shrubs Online, *Picea abies* (read) | needles 1 to 2.5 cm (8–20(–30) × 1–2 mm), four-angled, green on all sides, on orange-brown twigs; "above shoot directed forward, those below more or less pectinate": all round the twig on top, parted below; second-order branches "somewhat to extremely pendulous" |
| A spruce's branching forms | Alexandrov, *Silvae Genetica* 1971 (snippet); waldwissen.net as on [conifer shoots](conifer-shoots.md) (read there) | comb, brush, compact and flat forms, told apart by how the second-order branches hang |
| A Scots pine's needles | Gymnosperm Database, *Pinus sylvestris* (read) | in pairs, (2.5–)4–6(–9) cm × 1.5–2 mm, slightly twisted, "glaucous … blue-green or grey-green", kept 2 to 6 (to 9) years on grey-buff shoots: tufts on the last years' growth, the wood behind bare |
| A needle transmits less than it reflects | Lukeš et al. 2013, *Remote Sensing Letters* 4(7):667 (snippet of the abstract); Hovi, Raitio and Rautiainen 2017, [*Silva Fennica* 51(4) 7753](https://silvafennica.fi/article/7753) (read: its figures 3 and 5, its section 3.1.2); Hovi et al. 2020, [*Silva Fennica* 54(2) 10270](https://www.silvafennica.fi/article/10270) (read) | pine and spruce needle transmittance is "clearly lower than reflectance"; current-year needles reflect and transmit more green than older ones; a pine needle's underside is darker in the visible; a spruce needle's visible transmittance is so small that a double integrating sphere reads it negative (−0.024 at 406 nm) |
| A shoot scatters backward | Rautiainen et al. 2018, *Remote Sensing* 10:207, section 3 and figures 3 and 4 (read) | Scots pine shoots "scatter most strongly in the backward directions with very little forward scattering in blue and red": a bough against the light is dark, and that is the bough, not a fault |
| A needle's reflectance in numbers | *recalled*; in every open source reached the visible-band numbers are in figures only (Hovi 2017 and 2022, Rautiainen 2018) | about 0.03–0.05 at 450 nm, 0.08–0.12 at 550, 0.04–0.06 at 670; transmittance 0.03–0.08 at 550 and 0.01–0.02 at 670. The recipes' colours (spruce 0.050, 0.100, 0.040; pine 0.052, 0.092, 0.056, linear) sit in that, and are to be checked against the published spectra |
| How shipped foliage cards are shaded | Unreal Engine's shading-model page, Two Sided Foliage (read); Sousa, *GPU Gems 3* chapter 16, listings 16-4 and 16-5 (read); Barré-Brisebois and Bouchard, GDC 2011 (its parameters read, its formula recalled); SpeedTree Modeler's leaf-generator properties (read); Sanders, GDC 2018, *The Vegetation of Horizon Zero Dawn* (an unofficial transcript, read) | a subsurface colour "define[s] the amount of light transmission" through a leaf; a back-face term of a wrapped `dot(-N, L)` and a view-dependent lobe times a painted map; **leaf normals turned from the card's own towards a direction from the crown's centre or the parent's spine** ("global smoothing", "puffiness", sliders with no published default); absolute-value normals on two-sided planes with translucency from a texture. No source reached gives a conifer card's size or count, a fold, or a blend fraction |

**What that asks for.** (a) *The image*: brushes, not threads and not a fill. A spruce twig with
its needles is three to four centimetres across and the card between twigs is open; a pine's
needles are tufts seven to ten centimetres across at the ends of bare shoots. (b) *The colour*: a
needle's own reflectance, about a tenth in the green; the first sprays' albedo was already there
and was then halved by a depth tone and a hollow shade, which is the texture doing the light's
work. (c) *Light through a bough*: **not a transmission lobe**. A needle passes a few percent of
green and almost no red or blue, and a shoot scatters backward; what comes through a real bough
is the sun between its twigs, which the outline's gaps carry and a traced shadow already
resolves. What a card lacks is the other half: needles stand all round a twig, so a bough is lit
by where it is on the crown, and a flat card with its own normal is lit by which way its plane
faces. (d) *The card*: published practice is normals turned outward and several small bent
cards a branch; the budget here pays for the first and for a fold.

### Which measure a hull is

The second leg left this open: Missouri's softwoods average a crown density of 49.5 % against
the 75 to 85 % this page aimed at. Read this leg: Schomaker et al. 2007 (USDA GTR SRS-102,
sections 1 and 2.3.5; the text extraction partly garbled) define **crown density** as "the
proportion of crown volume that contains biomass", the foliage, branches and reproductive
structures that "block light penetration through the crown", dead branches included, falling
with "gaps in the live crown"; and **foliage transparency** as "the absence of foliage where
foliage normally occurs". Randolph 2009 (GTR SRS-117 chapter 8, its page summary read) has
density rated against the projected crown outline and transparency as the skylight through "the
live, normally foliated portion of the crown"; large gaps where no foliage is expected are left
out of transparency (a field-guide snippet). Means, from search snippets of the state reports:
all trees of the southern states 44.6 % density and 17.0 % transparency, softwoods 18.4 %
transparency, eastern hemlock 57.8 % density, Vermont and West Virginia 48 to 49 % and 18 to
19 %.

**So this page's hull coverage is crown density, not the complement of transparency**: the hull
through the branch tips holds the gaps between branches, as the projected outline does, and a
field conifer blocks about half of it (45 to 58 % in what was reached), not four fifths. The
first two legs' target of "about 80 % for a spruce and 70 to 75 % for a pine" read transparency's
complement into an outline it is not rated in, and is withdrawn. The owner's eye was not wrong
about the thin images: at the same share a crown of threads a third of a pixel wide reads
thinner than one of brushes several pixels wide, which is "The threads" above, and the forest's
pine at 41 to 49 % of its hull is at or under the field mean with nothing in it thicker than a
strand. Opaque cards are still the solver's ceiling (77 to 87 %): a solver that drew every
twig would cover less than its cards do.

### What changed

- **The spray images** ([matgen](../subsystems/matgen.md#spray-sets)): one branchlet an image,
  laid out in millimetres on the card it lies on (0.2 m of branch wide, 0.45 m long, leaning 55
  degrees) so a twig is as wide on the tree as its recipe says. *Spruce and the bare conifer*:
  side shoots 46 mm apart to one side and the other, each with its own short twigs: several
  small boughs a card with open card between; needles 10 to 17 mm, 26 a centimetre as drawn,
  1.2 to 1.76 mm wide. *Pine*: whorls 95 mm apart, each lateral a tuft of pairs 40 to 60 mm long
  on its last 110 mm, bare wood between. Opaque share: spruce 25.3 %, conifer 25.5 %, pine
  33.2 % (41.8 % from another seed). A round back under each brush in the height, so the normal
  map shades a brush as a cylinder. The needles' own colour, 0.8 to 1.05 of it by depth.
- **The cards** ([flora](../subsystems/flora.md#the-conifer), `build_bough`): the same strip, the
  same triangles, folded at every station so that neighbouring cards are facets rolled 20 to 35
  degrees; one branchlet a full-detail spacing across and one a card length along, whatever the
  budget rung made of the card; the image's base at the shoot on both sides of it.
- **The normals**: each needle vertex's normal is its card's plus the crown's outward direction
  there, on the side of the card that faces out.
- **Triangles**: none added. The forest's pine is the triangles it was and its spruces theirs
  (26,000 to 29,200 on 40,000, [conifer shoots](conifer-shoots.md#the-budget)); vertices and
  parts the same. What recooks: every conifer container (`k_conifer_chain_version` 2, the
  conifers' own key, so no broadleaf's entry moves) and the generated pack
  (`k_matgen_version` 10, which re-keys every generated mesh built against a pack with a
  `generated` folder, as the second leg's 9 did).

**Two first cuts that did not read.** Side shoots every 20 to 30 mm with no twigs of their own
read at 10 m as a fern: a comb of thin parallel teeth. And a height that was zero between
needles made a cliff of the brush's whole depth at every needle's edge, so the normal map lay on
its side and every sunlit spray went dark.

### What is drawn now

Measured as the second leg's table was (the same trees and views, the pack's roles read from a
materials root holding only the generated pack, the branch's release build, `--shadows rt`,
`--lod 1`, the bounce off, sixteen offsets a view; 2026-11-15 02:33, RTX 5090, other processes at
23 to 41 % of the CPU, which no coverage depends on). Coverage inside each set's own outline,
percent, and the hardware rasterizer against the rays **in covered pixels** (a hull is a poor
outline to compare two rasterizers in, as the second leg found: at 300 m the rasterizer's hull is
a tenth to a fifth smaller than the rays' and its share of it reads higher while it covers
fewer pixels):

| tree (the image's opaque share) | m | forest image, rt | second leg's spray, rt | **third leg, rt** | **third leg, hw** | hw against rt, covered pixels |
|---|---|---|---|---|---|---|
| pine (33.2 %) | 10 | 64.1 | 93.1 | **84.8** | 84.8 | 0.0 % |
| | 30 | 49.0 | 75.7 | **68.1** | 68.2 | +0.1 % |
| | 100 | 44.6 | 73.5 | **67.3** | 69.1 | −1.0 % |
| | 300 | 44.5 | 76.6 | **64.3** | 70.9 | −2.1 % |
| spruce (25.3 %) | 10 | 76.9 | 85.2 | **75.7** | 76.0 | 0.0 % |
| | 30 | 70.8 | 79.9 | **70.2** | 69.9 | −0.2 % |
| | 100 | 68.3 | 65.6 | **69.1** | 70.6 | −1.3 % |
| | 300 | 68.6 | 76.6 | **67.8** | 78.7 | −9.6 % |
| conifer (25.5 %) | 10 | 76.0 | 86.7 | **76.2** | 76.5 | 0.0 % |
| | 30 | 68.3 | 79.4 | **69.1** | 69.1 | −0.1 % |
| | 100 | 66.3 | 74.9 | **66.4** | 68.5 | −0.6 % |
| | 300 | 68.0 | 78.4 | **68.5** | 72.0 | −7.4 % |

- **The spruces cover what they covered with the forest's image**, 66 to 76 % of their hull, as
  the same opaque share says they should: the change is what the share is made of. Under the
  rays at 100 m the spruce's covered pixels now come in runs of 10.5 with gaps of 3.7 and a third
  of the hull's pixels are covered in some of the sixteen frames and not in others; the pine's
  runs are 11.2 with gaps of 4.8, where the forest's pine image drew runs of 3.4 with gaps of 4.1
  and three fifths partly covered. That is "thicker" as a number.
- **The pine covers 64 to 68 % from 30 m where the forest's image left 44 to 49**, because a
  tuft is a third of its card and the forest's strands a sixth. Both are above the field's mean
  crown density (45 to 58 %), as the solver's branches are denser than a field crown's.
- **The hardware rasterizer is within 2.1 % of the rays in covered pixels everywhere but the two
  spruces at 300 m** (7.4 and 9.6 % under): the far form's atlases, as on the first two legs,
  not closed here.

**A stand's frame** (the 49 trees, 3840 × 2160, `--raster rt --shadows rt`, the bounce on, the
flora swaying, a sky at 15:30, 120 frames after 8 warm ones, GPU medians; main's copied-out
build with the forest's images against the branch with the generated sprays, alternating in one
lease, 2026-11-15 02:34; **not quiet**: the harness recorded other processes at 39 to 44 % of
the CPU; the sides differ in build and in image, so this is what the stand costs with each, not a
pair of builds): main 6.79 and 6.79 ms (resolve 3.17, the primary rays' trace 1.69, the bounce's
trace 0.79, the cluster structures 0.63); the branch 5.84 and 5.83 ms (2.72, 1.41, 0.69, 0.51).
The folded cards and the structured sprays cost a frame nothing and the stand is a seventh
cheaper, as denser cards were on the first two legs. Memory: the same vertices, triangles and
texture bytes as before.

**The pictures** are in the row's hand-back (3840 × 2160, rt and rt, the bounce on, a sky at
15:30 in July, the eye at 1.7 m: the spruce and the pine at 10, 30 and 100 m and the stand, main
with the forest's images against the branch with the generated sprays). The scene has no ground
under the trees, so nothing bounces up into a crown and no shadow falls anywhere it can be seen:
the undersides are darker than a forest's would be.

### What still reads wrong, plainly

The agent's reading of its own captures; the owner's eye is the judge.

1. **The spruce reads as a spruce at 10 and 30 m, and darker than main's.** Its boughs are
   boughs with needles, no paddle is left, and the outline is ragged at both distances. It is
   also about half as bright as main's with the forest's image: the recipe's colour is a needle's
   reflectance and the pack's paint is lighter. Where one facet catches the sun whole it is a
   light fleck with a card's outline, and every branchlet is the one image.
2. **The pine does not read as a pine from under it.** From the owner's eye at 10 and 30 m and
   from under the stand the crown is a dark mass of what look like small leaves. A tuft's needles
   are a third of a pixel wide at 10 m, so a tuft is drawn as a blob with a ragged edge; the
   crown is 20 to 34 m up, so it is seen from below, where every card shows its far side; and
   the cards of a pine on this budget are as long as a lateral, so the canopy is still made of
   broad fans, now textured. It is denser, which was the ask, and no longer a ceiling of leaves
   with rows of holes; it is not right.
3. **A card's far side takes no sun.** The resolve turns a two-sided card's normal to the eye's
   side of its plane and has no term for light arriving on the other side, so a bough seen from
   under or from behind is lit by the sky and the bounce alone. A real bough is dark against the
   light too (the table above), but not from every side at once: the needles round a twig
   show their lit faces to an eye at any side of it. The leaning normals give that to the side
   of a card that faces out of the crown and cannot give it to the other. What would: a
   material's flag that its normals are a volume's, which the resolve, the bounce's hit shading
   and the reference path tracer then do not turn (the alpha word has bits free), designed with
   the published back-face terms above and the needle's small transmittance; a row of its own in
   the renderer, not started.
4. **The forest's own thin images on the folded cards read as they did, a little fuller.** That
   is what a walk of the forest shows until its pack changes, since a fetched role wins, and the
   fold, the corrected v on one side, the tiled u and v on wider and longer cards and the leaning
   normals all reach those trees. Captured and measured the same way (the branch's build, the
   forest's recipes with their own images, 2026-11-15 02:54): the pine covers 66.1, 51.1, 47.4
   and 47.8 % of its hull at 10, 30, 100 and 300 m under the rays (64.1, 49.0, 44.6 and 44.5 on
   the first leg's cards), the spruce 77.7, 72.2, 70.6 and 70.1 (76.9, 70.8, 68.3, 68.6) and the
   bare conifer 76.9, 69.9, 68.3 and 68.1 (76.0, 68.3, 66.3, 68.0): one to three points up, from
   the cards a budget rung made wider or longer now carrying the image more than once. In the
   pictures the spruce at 10 m has a few more sunlit flecks and the pine is the haze of strands
   it was. The stand's frame with them on the branch: 6.62 ms against main's 6.79 (one run,
   other processes at 65 % of the CPU). So the owner's "too thin" is not answered in the forest
   by the engine alone: it needs the pack's images drawn as these are.
5. **At 100 m the pine is where the change shows most, and the spruce not at all.** The pine
   with the forest's image is a scatter of olive dots with the sky through all of it; with the
   generated tufts it is a closed dark crown with a layered outline. The spruce is the same
   small dark cone either way. Both are darker and bluer than main's, by the colour (item 1).
6. **A fold is two planes.** A card bent along its length, two crossed cards a shoot and smaller
   cards on more twigs each double the needle triangles or more and were not built.

## Not done

- **The third leg's**: no Linux container (the pins of all three legs are MSVC's), no GPU
  verdict of its own, a scene with ground under the trees not captured, the forest's staging
  not loaded, the reference reels not opened, a needle's reflectance still recalled.

## Not done

- **The far form's atlases at 300 m** (above): 7 to 11 % under the rays on the thin images, the
  cause read from the table and not measured.
- **The forest's staging was not loaded**: the branch changes every mask's key, so a load recooks
  into a root of its own, and the night's free commit stood between 4 and 50 GB against the
  40 GB floor. On the second leg it was not loaded either, for a second reason: a pack's hash is
  over its directory, so a materials root of the row's own re-keys every generated mesh of the
  staging (3,158 flora recipes in the first 4 MB of its scene alone), which is a cold build of
  the forest and not a recook of its masks.
- **The rays still test level 0** (cause 6): a ray cone choosing the level would draw the masses
  the rasterizer's levels draw, at the same coverage, and is a design of its own beside the
  opacity micromaps, which are baked from level 0.
- **The literature's numbers marked *recalled*** are to be re-read from the papers.
