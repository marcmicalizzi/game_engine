# Sampling over time: what dense foliage needs, and how temporal anti-aliasing, DLAA, DLSS, FSR and XeSS sit in this renderer

Roadmap R153, 2026-11-14. A measurement, a design and a proposed decision. Nothing on this page is
built: no default changes, no shader changes, no engine code was added for it. What is built
afterwards, in what order and with which vendors, is the owner's choice from
[the stages](#part-4-the-plan-in-stages) and [the proposed decision](#proposed-decision).

**What was asked.** The owner, after two walks of the forest (2026-11-14): "just for figuring out
visual artifacts from just so many leaves creating interference patterns (it's not a visual defect
persey but it is noticeable), and for frame rates too, maybe it's time to start thinking about TAA,
DLSS, DLAA and FSR". And the rule it sits under, in the owner's words the same day: "no upscalers
is more about not relying on them for performance as a crutch [...] upscalers are necessary, but
the big rule on that is to never rule that not optimizing performance further is acceptable because
we can just push the dlss more to performance mode. So the big caveat is that our performance
measurements and performance observations from a walk are taken without upscalers, and that we're
never thinking it's 'good enough' just because an upscaler takes it to an acceptable framerate."
The plan already expected them ([04 §4.1](../plan/04-renderer.md) names an upscaler in two of its
three targets and §4.2's step 8 is "TAAU/DLSS/FSR/XeSS, optional frame generation"); this page is
how they are built here, and the rule that keeps them from hiding a regression.

**In one paragraph.** The interference is undersampling, and it is large: on a crown at the 4K
target's pixel pitch a drawn pixel differs from the same view supersampled by 29 display codes rms,
a slow pan changes a crown pixel by 30 codes from frame to frame where the supersampled picture
changes by 4, and nine tenths of what flickers under a swaying wind is aliasing and not motion.
On the engine's oaks 95 to 99 % of it sits in pixels that hold an edge between instances or
against the sky, and on the forest's hillsides 88 % in pixels that hold more than one triangle:
coverage, which no shading change reaches and which accumulation over frames removes. The
plainest accumulation there is (a tenth of each new offset frame, no reprojection) takes the
swaying crown from 24 codes of error to 7 and its flicker below the supersampled picture's own;
on near leaves, which really move, it trails them, which is what motion vectors are for. Three
things found on the way are **content
faults that no accumulation fixes**, and are rows of their own: the hardware rasterizers draw a
quarter less crown than the rays at 120 m (the masks' mips against level 0), the far crowns hold
eight or more triangles in a pixel on four pixels in ten, and the cross-fade's screen door is a
function of the pixel alone, so accumulation can never blend it. The design is the engine's own
accumulation first, on every machine and deterministic, then a registration point for a
reconstruction provider, then internal resolutions below the output's, with the vendors' SDKs as
optional capabilities a consumer supplies and that never enter the repository
([ADR-0014](../adr/0014-apache-2-license-and-dependency-policy.md)). Every budget, pair and walk
stays at native resolution with reconstruction off, and the tools refuse anything else.

## Part 1: what the interference is

### How it was measured

**A supersampled picture of the same view, with no engine change.** The renderer has no sub-pixel
offset today (nothing jitters: `ViewSet::update` builds each view's projection from the camera
alone; the reference path tracer jitters inside a pixel, but its transport is its own and its
frames take no cross-fade, so its difference from the frame is not sampling alone). The offset
frames were made with the camera path instead: **the eye stands still and the look direction turns
by a fraction of a pixel**. The eye does not move, so the LOD cut, every mip choice and every
parallax are one view's; a turn is a homography of the picture, and with a narrow field of view it
is a uniform shift to within tan² of the half field. So every set is a **512 × 512 crop of the
3840 × 2160, 60° target at the target's own pixel pitch** (0.5346 mrad a pixel at the centre: a
vertical field of 15.585°), where a shift is uniform to 1.9 % at the crop's edge. Each view is 64
frames on an 8 × 8 grid of offsets inside one pixel, each drawn after eight warm frames at its own
camera (`--marker-captures`, every frame a marker), with the id channel beside it
(`--capture-channels ids`). The **reference** is the 64 frames averaged in linear light (decoded
with the 2.2 power the output encode applies): the box-filtered picture of that view, which is
also what any accumulation of offset frames converges to at a held camera. **As drawn** is one of
the 64 (the offset nearest the centre). Differences are in display codes of luma, 0 to 255,
because the question is what is seen. The method's own check: a frame looked one whole pixel aside
equals the first frame read one pixel over in 97.9 to 99.9 % of the central pixels of the
leaf-cards views (85 to 88 % on the forest's hillsides and 99.6 % on its near view; the hillsides'
shortfall was not separated: with eight or more triangles in a pixel the 1.9 % of non-uniformity
moves some samples across an edge, and a streamed world's state may differ between two markers).

**The pan and the sway come from the same frames.** One row of the grid is a camera turning an
eighth of a pixel a frame; the supersampled picture slid by the same eighths is assembled from the
same 64 frames (a pixel's window slid by k/8 takes the columns of offsets at or past k from this
pixel and the rest from its neighbour). For the wind, a path of 120 steps of 16 offsets (4 × 4) at
`--world-rate 0.0625`: sixteen frames span one sixtieth of a second of world time, as drawn is one
offset's frame a step, the reference the step's mean, two seconds at 60 Hz.

**Settings**: main at 61ccce67, its release binaries copied out of the tree, RTX 5090,
`--offscreen --raster rt --shadows rt --no-lights --lod 1 --dither off`, the flora
swaying at `renderer.flora.wind_strength=0.7` (the world still, `--world-rate 0`, in the static
sets), `renderer.shadow.micromaps=true`, **`renderer.gi.mode=off`**: under the bounce the same
binary does not draw the same bytes twice, and a difference of frames has to be the offsets'
alone. The dither is off for the same reason (it is a function of the pixel, so it would not
average away). **The machine was not quiet** (00:22 to 01:08 local on 2026-11-14: other agents
building and testing, CPU 59 to 97 % busy, GPU 0 to 15 % busy with 5.8 GB held by others); nothing
here is a time, so that does not touch the numbers. The scripts (the path writer, the run wrapper
and the analysis, a C# file compiled by PowerShell) are scratch, not committed: stage 2's `mean`
mode ([below](#how-the-invariants-stay-testable)) replaces them with the renderer's own.

**The scenes.** `content/test-scenes/leaf-cards` (four solver oaks of 12 m whose 0.24 m leaf cards
sample a procedural leaf with its outline in alpha, `make-leaf.ps1` run first), from three
distances: **near**, 30 m from a crown (a leaf card 15 px long, the finest leaves drawn);
**mid**, 121 m (a card 3.7 px, the first levels of the impostor chain); **far**, 411 m (a card
1.06 px, the chain's last card levels before the star). And **the forest's staging** on this main
at the owner's settings, the bounce off: two views across open ground to a hillside of crowns
(670 m and 860 m to the target) and one near view. A "crown" row below is a rectangle round the
crowns alone; a "frame" row is every pixel the scene covers in at least one sample.

### What the difference holds

| view, traced primaries | as drawn − reference, rms / median / over 8 codes | a pixel's spread over the 64 offsets, mean | power past 0.375 cycle/px, as drawn / reference | slow pan, frame-to-frame change, as drawn / reference |
|---|---|---|---|---|
| leaf-cards near, frame | 16.4 / 1.1 / 23 % | 10.8 | 39.4 / 9.1 (4.3×) | 16.2 / 3.4 |
| leaf-cards mid, frame | 17.9 / 0.0 / 26 % | 10.6 | 52.2 / 9.2 (5.6×) | 18.4 / 2.6 |
| **leaf-cards mid, crowns** | **29.2 / 13.3 / 64 %** | not taken | 73.8 / 12.5 (5.9×) | **30.3 / 4.3** |
| leaf-cards far, frame | 14.2 / 0.0 / 13 % | 5.5 | 17.2 / 1.2 (14.5×) | 17.5 / 1.5 |
| **leaf-cards far, crowns** | **28.8 / 9.4 / 53 %** | not taken | 137.7 / 10.1 (13.7×) | **35.6 / 3.1** |
| forest, hillside at 670 m, frame | 10.1 / 2.9 / 28 % | 8.2 | 13.8 / 1.4 (9.6×) | 12.3 / 1.2 |
| forest, hillside at 860 m, frame | 10.0 / 2.7 / 27 % | 7.9 | 13.9 / 1.4 (9.6×) | 12.1 / 1.2 |
| forest, near ground and trunks | 2.6 / 0.0 / 1.2 % | 0.6 | 0.7 / 0.2 (4.0×) | 2.7 / 0.8 |

Codes are display luma codes; power is the mean square of the Hann-windowed picture in the bands
past 0.375 cycles a pixel (the lattice's limit is 0.5), in codes². Read the spectrum's shape as
well as its size: the reference's power falls with frequency or stays level, and the drawn frame's
**rises towards the limit** wherever there are crowns (leaf-cards mid, the frame, by band of 1/16
cycle a pixel from 0.19 up: 9.5, 10.8, 12.6, 15.6, 17.2, and 19.4 past 0.5 in the corners, where
the reference holds 3.9, 3.4, 3.3, 3.4, 3.2 and 2.6). Power that rises to the limit is power from above it, folded back: that is the
"interference pattern", and its beat against the lattice moves whenever the camera or the leaves
do.

**By what a pixel holds** (the id channel of all 64 offsets: a pixel is *one triangle* when every
offset names the same triangle, *one instance, several triangles* when the triangles differ inside
one instance, and *an edge* when the offsets name several instances or the pixel is partly empty):

| view | one triangle: share of pixels, of the squared difference, rms | one instance, several triangles | an edge between instances or against the sky |
|---|---|---|---|
| leaf-cards near, frame | 28 %, 0.3 %, 1.7 | 37 %, 2.4 %, 4.2 | 35 %, **97.3 %**, 27.4 |
| leaf-cards mid, frame | 42 %, 2.6 %, 4.5 | 27 %, 2.6 %, 5.6 | 32 %, **94.8 %**, 31.0 |
| leaf-cards mid, crowns | 0.6 %, 0.0 %, 4.6 | 12 %, 0.6 %, 6.4 | 87 %, **99.4 %**, 31.1 |
| leaf-cards far, frame | 70 %, 2.4 %, 2.6 | 13 %, 0.6 %, 3.0 | 17 %, **97.0 %**, 34.5 |
| forest, hillside at 670 m | 27 %, 12.5 %, 6.9 | 17 %, 10.6 %, 7.9 | 55 %, **76.9 %**, 11.9 |
| forest, hillside at 860 m | 28 %, 12.4 %, 6.6 | 22 %, 17.5 %, 9.1 | 50 %, **70.2 %**, 11.9 |

So, cause by cause:

1. **Coverage: leaves and leaf cards near or under a pixel against the lattice.** Three quarters to
   all of the squared difference. A leaf card's mask on the ray path is sampled at level 0
   ([renderer](../subsystems/renderer.md#alpha-tested-geometry)), so each ray sees one texel's
   verdict, and at 121 m 9.7 % of the covered pixels hold **eight or more distinct triangles**
   (0.8 % under `--raster hw`, whose masks are mip-filtered and so cut blobs, not leaves); on the
   forest's hillsides **42 % of pixels hold eight or more triangles and 27 to 31 % four or more
   instances**. One sample of such a pixel is a draw from its contents. **Accumulation removes
   this**: it is the definition of the reference.
2. **Shading inside one triangle** (the shadow ray's one sample at a mask's edge, a texture's
   detail, a specular lobe): 1.7 to 6.9 codes rms, 0.3 to 12.5 % of the squared difference, the
   larger figures the forest's ground under crowns, whose shadow is one level-0 ray a pixel.
   Accumulation removes it too, provided the shadow's sample moves with the offset (it does: the
   ray leaves the offset pixel's point).
3. **What only shows in motion.** A held camera over a still world draws the same bytes every
   frame (the engine's invariant: no test of tonight's, every renderer test's), so at rest the
   pattern is still: a moiré, not a shimmer. It becomes a shimmer under any motion. Under the wind
   alone, camera held, two seconds at 60 Hz:

   | view | frame-to-frame change, as drawn / reference | temporal deviation, as drawn / reference | share of the frame-to-frame energy that is aliasing |
   |---|---|---|---|
   | leaf-cards near | 12.6 / 4.2 | 21.6 / 14.7 | 89 % |
   | leaf-cards mid, crowns | 12.2 / 3.5 | 23.9 / 9.6 | 92 % |
   | leaf-cards far, crowns | 11.7 / 3.4 | 22.4 / 6.1 | 92 % |

   and under the slow pan above 98 to 99 % (30.3 against 4.3, 35.6 against 3.1). **Accumulation
   removes it to the extent the history follows the surface**, which is Part 2's motion vectors.
   What the plainest accumulation does without them, emulated from the same frames (one offset
   frame a step into an exponential history, no reprojection, no clamp):

   | view | error against the step's reference, as drawn → accumulated (a tenth a frame) | frame-to-frame change, as drawn → accumulated (reference) | with a quarter a frame |
   |---|---|---|---|
   | leaf-cards mid, crowns | 24.2 → 7.5 | 12.3 → 2.9 (3.5) | 8.8, 7.4 |
   | leaf-cards far, crowns | 26.9 → 6.5 | 11.8 → 3.0 (3.4) | 9.6, 7.8 |
   | leaf-cards near (one second) | 16.2 → 11.3 | 12.7 → 2.3 (4.2) | 8.3, 5.5 |

   The last column is the error and the frame-to-frame change at a quarter a frame. A tenth a
   frame is about nineteen frames of history, and 29 / √19 is 6.7: on the far crowns the residual
   is the sample count's, as it should be. Its change from frame to frame falls *below* the
   reference's, which is the history lagging the leaves, and on the near view, where a leaf is
   fifteen pixels and really moves, that lag is most of what is left (11.3 codes, and better at a
   quarter a frame than at a tenth): the ghosting that reprojection and a clip exist to bound.

### What survives in the reference: content, not sampling

**A pattern that survives in the converged reference is a content fault.** Four were found.

1. **The rasterizers and the rays do not draw the same crown.** The supersampled crown rectangle
   at 121 m is covered 50.7 % under traced primaries and **38.8 % under `--raster hw`**, 23 % less;
   at 411 m 77.0 % against 64.4 %, 16 % less; at 30 m, where the masks are magnified, 83.4 % of
   the frame against 83.7 %, the same. The rays test level 0, the rasterizers a mip chosen by the quad's
   derivatives, and the chain's coverage is not level 0's at these distances although the build
   keeps each level's share of passing texels ([texture](../subsystems/texture.md#alpha-tested-masks)):
   which of the two the build's rule fails for (the leaf's own image, or the impostor cards' baked
   atlases, which may not go through that build) was not found tonight. It matters beyond the
   picture: a traced shadow is cast by level 0's crown onto a picture `--raster hw` draws thinner.
   No accumulation touches it. Row R155 below.
2. **The far crowns are finer than a pixel by an order.** Eight or more triangles in a pixel on
   42 % of the forest hillsides' pixels says the cut is drawing leaf-scale geometry where a pixel
   is tens of leaves wide. Accumulation averages it (that is the 10 codes), but the work is paid
   every frame for detail that only the average shows, and it is the case every reconstruction
   method handles worst (below). The answer is the content's: the impostor chain's coverage error
   and the far forest taking over where a group's leaves fall under about a pixel, their masks
   pre-filtered. It is a row for the foliage's owner with this page's number as its measure
   (R156).
3. **The cross-fade's screen door is a function of the pixel alone**
   ([renderer](../subsystems/renderer.md#the-cross-fade)): 1.0 % of the crown's pixels at 121 m
   and 2.4 % at 411 m differ between `--lod-fade 0.1` and `0`, and no figure above moves with it,
   so it is not the interference. But because the hash holds nothing of the frame, an accumulated
   picture keeps the door: each pixel shows the same level in every frame. Stage 2 gives the hash
   the offset's phase when accumulation is on, and the fade becomes a blend.
4. **The leaves stand in rows.** The mid reference shows the twigs' leaves as regular ladders; the
   solver spaces a twig's cards evenly, and an even spacing near the lattice's is what a moiré
   needs. It is the content's own regularity, visible only because everything else has been
   averaged away, and it belongs to the flora's phyllotaxis, not here.

## Part 2: what the renderer must provide

Every method below, the engine's own and every vendor's, takes the same inputs: a frame drawn at a
sub-pixel offset, where each pixel's surface was in the last frame, the depth, the exposure, and a
hint where the history must not be trusted. The design is from the code as it stands
([renderer](../subsystems/renderer.md), [gfx](../subsystems/gfx.md)).

### Motion vectors from a visibility buffer

**No rasterizer writes a velocity.** A pixel holds `depth << 32 | pair << 8 | triangle`; the
rasterizers and the ray path write that word and nothing else, and that stays. Motion is a
**compute pass over the visibility buffer** (`motion.slang`, between the visibility passes and
the resolve, one thread a pixel, its own timer zone), because a provider wants it as an image, the
ray path has no fragment stage to add an output to, and the resolve by pixel class would otherwise
write it from sixteen modules. It writes two half floats a pixel: where this pixel's surface point
was in the last frame's picture, in pixels, with neither frame's offset in it. By kind:

| What the pixel holds | Where its point was | What it reads |
|---|---|---|
| nothing (the sky) | the view ray through last frame's rotation: a direction, no position | `clip_to_ray` and last frame's view-projection, both eye-relative |
| a rigid instance that did not move (most of any scene: imported meshes, rocks, buildings, the ground's tiles and rings) | the point from the word's depth and the pixel's ray, plus this frame's eye minus last frame's (computed on the CPU in f64, handed as a float vector: [ADR-0053](../adr/0053-world-positions-are-f64-and-the-gpu-sees-none.md)), through last frame's view-projection | the visibility word only |
| a rigid instance that moved (`SceneRenderer`'s moved instances, an actor's rigid parts) | the same point taken back through this frame's instance transform and forward through last frame's | the pair table, the instance, and a table of last frame's transforms for the instances a frame moved (60 bytes each: a cell and a 3×4), flagged in `InstanceDesc` |
| a deformed instance: a swaying tree or grass, a skinned or morphed actor, a water body's waves, a dune ground in time-lapse | the triangle's three corners **as last frame's inputs deform them**, at the pixel's barycentrics | below |

**The ground's tiles and rings that come and go need nothing**: a tile is a rigid instance at its
corner and the ground under a pixel is the same ground whichever level draws it, so the depth row
is right through a ring change, and where a coarser level's surface differs the validity test
below sees it. **A scene with nothing deformed and nothing moved never leaves the first two rows**:
one word read a pixel.

**A deformed instance: three ways, and the one proposed.** The deformed-vertex pool is
suballocated per frame, a block per visible entry ([renderer](../subsystems/renderer.md#the-deformed-vertex-pool)),
so last frame's position of a vertex is not at this frame's address.

- (a) *Keep last frame's pool and find the block*: a second pool, and last frame's pair-to-slot
  table per view (4 bytes a pair: 67 MB a view on the forest's 16.7 million pairs). Three loads a
  pixel, no arithmetic. Wrong for one frame wherever the cut changed (the pair was not drawn last
  frame), which a cross-fade band is all the time.
- (b) *Deform the three corners again in the motion pass with last frame's inputs*: no memory, exact
  at a cut change, but the stages' arithmetic three times a pixel: at 3840 × 2160 that is 25
  million vertex evaluations a frame where the pool pass does at most 5.6 million for the same
  0.42 ms, so about 1.9 ms by proportion, and three times that on a surround.
- (c) **The pool pass writes each vertex twice: this frame's position and last frame's**, the second
  from last frame's inputs, into a block beside the first (the same slot, a second buffer). Every
  stage's per-frame input already exists for the frames in flight (`k_joint_slots` copies of the
  joint palettes, the flora's part poses, the water's clock, the morph weights), so "last frame's"
  is the slot before. The motion pass then loads three positions and interpolates. **Exact at a
  cut change, independent of resolution and of the number of views** (the pool is per cut), and
  general: a stage a capability adds later gets motion by being run twice, with nothing in the
  motion pass naming it. It costs the pool pass again (0.42 ms on the forest's lake frame, the
  same on a surround) and the pool's bytes again (8 MiB by default, 64 at the owner's setting).

(c) is proposed; stage 1 measures (a) and (c) on the forest before it keeps one.

**Water, whose surface moves in the shader.** A body's waves are geometry (the pool's water
stage), so its surface has motion like any deformed instance. What the motion cannot follow is
what the pixel *shows*: the ground under the water refracted, the scene reflected, and a channel's
surface, whose ribbon lies still on its bed while the flow's phase runs through the shader
([water](../subsystems/water.md#channels)). Two things, both what the vendors' documents ask for:
a channel pixel's motion adds **the flow's own velocity** (`water_channel_flow` already has the
speed and direction over the triangle; FSR's manual: effects such as scrolling UVs "should be
included in the motion calculation"), and every water pixel writes a **reactivity value**, the
share of its colour that is not its own surface's (transmission plus reflection), into an 8-bit
mask the reconstruction reads as "trust the history less here" (FSR's reactive and
transparency-and-composition masks, XeSS's responsive pixel mask, DLSS-RR's transparency layer).
The participating medium's frame pass writes the same mask where it is thick. A scene with no
water and no medium never creates the mask.

### The sub-pixel offset, the level's bias, and what is not offset

**One place**: `ViewSet::update`, which turns the frame's camera into each view's projection and
its `clip_to_ray`. An offset of (jx, jy) pixels is a shear added to the projection's third column,
so the rasterizers' pixels, the traced primaries' rays (they are made from `clip_to_ray`) and the
resolve's reconstruction (it meets the pixel's own ray with its triangle) all move together, and
"where a pixel meets its triangle" stays exact. The sequence is Halton(2, 3), the phase the frame
index modulo 8 at native resolution and `ceil(8 n²)` at a ratio of n (18, 23, 32 and 72 at 1.5,
1.7, 2 and 3: FSR's manual and XeSS's guide give the same counts), every view of a set at the same
phase. **Not offset**: the cull's frustum and the LOD's scale (a cut must not change with the
phase), the occlusion pass (pass 2 tests this frame's own Hi-Z, so it stays exact), the dither
(the pixel's place), the motion vectors (both frames' matrices without their offsets), and the
overlay.

**The texture level's bias** is one scalar where the resolve already computes a footprint
(`screen_barycentrics` and the gradients it hands on): zero at native resolution, and
log2(internal / output) at a ratio, which is −0.58, −0.77, −1 and −1.58 at the four ratios (FSR's
manual subtracts one more, −1.58 to −2.58; the engine's own starts from the plain logarithm and
the closing measurement of stage 3 says whether the extra level pays on foliage, where the XeSS
guide warns that more bias costs bandwidth and adds flicker). The ray path's masks stay at level
0. **The LOD threshold stays in output pixels**: at a ratio of n the frame's `lod_px` is divided
by n, so the cut is the output's. That is deliberate and it is why the cull, the pool and the
acceleration structures do not get cheaper with the ratio: the detail is what the reconstruction
resolves.

### When the history is invalid

The renderer has something most do not: **an exact name for the surface under every pixel**. Last
frame's visibility buffer is kept (the two ping-pong), and a pixel's history is looked up where
its motion says, with three answers:

- *the same instance* at that place: the history is this surface's, used at full weight and
  clipped to the neighbourhood;
- *another instance*, or a depth that disagrees by more than the pixel's own depth slope: a
  disocclusion, the history discarded (the pixel shows this frame's sample and converges again);
- *off the view*: discarded, or looked up in the neighbouring view of a set.

**A disocclusion inside a crown is the hard case**, and what the literature does about it is
worth saying exactly, because a depth test alone fails there: every pixel of a crown is a
disocclusion every frame (a leaf moves and the sky or another leaf shows), so rejecting on depth
rejects everything and the crown is drawn at one sample, flickering. The practice (Karis 2014;
Salvi 2016; Yang, Liu and Salvi 2020, which is the survey): take the motion of the **nearest
depth in a 3 × 3** so a thin feature's edge carries the feature's motion and not the background's;
do not reject on depth where the neighbourhood itself is mixed, but **clip the history to the
neighbourhood's colour distribution** (mean ± γσ in YCoCg, γ about 1), which in a crown is wide
(leaf and sky are both in it) so the history survives, and on a clean edge is narrow so a ghost is
cut; and weigh the blend by luminance so one bright sample does not own a pixel. What that leaves,
by the same sources: thin features dimmer and softer than supersampling would draw them (the clip
pulls a one-pixel twig towards its neighbours), and ghosts wherever the neighbourhood is as varied
as the ghost. FSR adds per-pixel *locks* for one-pixel features and a *luminance instability* term
that widens the clip where the input itself flickers (its manual, read tonight); the learned
methods replace the clip with a network. **This renderer's instance test is the cheap first line**:
inside one tree a wrong leaf's history is a leaf's, green on green and clipped; across instances
the history goes. Whether the instance or the pair is the right grain for a crown against another
crown of the same species is stage 2's measurement.

A camera cut, a teleport and a load are `FrameDesc::history = reset`: the hosts say it, as the
vendors' interfaces all require (FSR's `reset`, the same in the others).

### Exposure, and what is drawn before and after

**Exposure is a declared rule, not a meter on the picture**
([renderer](../subsystems/renderer.md#exposure)): the frame's value is known on the CPU before a
pixel is drawn, so the reconstruction is handed the exact scalar and the pre-exposed linear colour,
and no method's auto-exposure is ever on (DLSS's guide: "strongly advised to provide exposure";
XeSS's: auto-exposure "causes a measurable performance impact").

**The seam exists**: a frame with a frame pass registered already resolves into a linear
half-float image of the renderer's own and ends on one encode pass
([renderer](../subsystems/renderer.md#frame-passes)). Reconstruction is the step between them:

| At the internal size, offset | The reconstruction | At the output's size, not offset |
|---|---|---|
| the cull, the pool, the visibility passes (raster or rays), the motion pass, the resolve (shading, traced shadows, the bounce's read, water), the frame passes that composite light (the medium) | reads the linear colour, the motion, the depth (the visibility word's high half, copied to a float image for a vendor), the masks, the exposure, the offset; writes the linear colour at the output's size | the encode (`display_output`: the tone curve, the transfer, the dither, once: [ADR-0052](../adr/0052-the-picture-is-quantized-once-with-dither.md)), the overlay and the UI, the HDR composite at paper white, the capture's colour |

The dither and the single quantization are untouched. A capture's `ids`, `depth` and `normals`
are the internal frame's and say their size in their header; with reconstruction off they are the
target's, as now.

### Three views side by side

History, motion and the offset are per view, like every per-view slice of a `ViewSet`
([renderer](../subsystems/renderer.md#multi-view-a-viewset-over-one-scene)). A flat surround is
three thirds of one rectilinear frustum, so its history can be read as one image across the seams;
a turned one's views have their own projections, and a point that crosses a seam is looked up in
the neighbour by projecting last frame's point through each view (three tries at most). The first
build treats a seam crossing as a disocclusion, and the measurement that decides whether that is
enough is a turn of the camera across a turned surround with the columns at the seams compared to
the centre's. A vendor's method takes one context per view (Streamline keys everything by a
viewport id; FSR and XeSS contexts are per output), or one over the whole target for a flat
surround. A Panini view accumulates in its output: its resolve already reads the source at a
fractional place, which is where the offset enters, and it is the point-sampled periphery that page
says temporal accumulation is the fix for. `ViewQuality` gains the ratio per view (the `View`
already carries a source extent, "the seam a resolution scale would use"), so the side views of a
surround may run at a coarser ratio than the centre: the owner's rule that the periphery matters
less, stated as a setting.

### The frame loop's no-allocation rule

Every image this adds (the motion, the two histories, the second visibility buffer, the masks, a
vendor's depth copy) is made where the linear image is made today, at the first frame that needs
it or at a resize, and never by a frame in steady state; the second pool block and the moved
transforms' table are sized at load. `engine_view.frame_loop` flies its three scenes once more
with the engine's accumulation on. **A vendor's library allocates inside itself** (its context's
memory is its own: FSR's manual gives 218 MB at 3840 × 2160 in its quality mode); the engine's
counters do not see it, and a provider's page has to say what it measured.

### What the rays could give back

Today the resolve asks one sun ray a pixel (a level-0 mask loop where it meets a leaf) and the
bounce reads a world-space cache. Once frames accumulate:

- **The sun's ray may be asked of fewer pixels a frame** (a checkerboard that alternates with the
  phase), which is the half-rate rule of [the resolve](../subsystems/renderer.md#a-shading-rate-by-region)
  made temporal instead of spatial: under the elder grove's crowns the sun's query is 0.46 of a
  2.70 ms resolve (R148's table) and on the forest it has been most of the resolve at a surround. Half the rays for the
  same converged picture is the upper bound; what comes back net is a measurement (R162), and it
  is counted at native resolution like any other cost.
- **A soft sun**: one ray towards a point on the sun's disc that changes with the phase converges
  to a penumbra for no more rays. A picture change, the owner's to want.
- **The bounce must not be filtered twice.** Its cache already remembers frames (a foliage cell 64
  of them). The accumulation over it is anti-aliasing of its read and nothing more: no stage here
  may lower the cache's ray count on the grounds that the accumulation hides the noise, since two
  histories in series add their lags. The rule, general: **one history per signal**. A term made
  stochastic *for* the accumulation (the fade's hash, a soft sun) changes with the phase; a term
  with a history of its own does not.

### How the invariants stay testable

The picture is a function of the scene in every test today, and stays so:

- **Off is the default and is exact**: no offset, no bias, no motion pass, no history image made.
  A case holds a frame with the feature compiled in and off to the bytes of the frame before it.
  Every existing test runs off.
- **`mean` mode is the converged-frame case**: accumulation with weight 1/n and no clip over one
  whole cycle at a held `FrameDesc` equals, to the float's rounding, the mean of the n offset
  frames drawn one by one with accumulation off. That is this page's reference made by the
  renderer, it is a function of the scene and the camera, and it is what `capture()` and
  `--marker-captures` give when accumulation is on (they already draw eight frames at the marker's
  camera before the picture: one cycle).
- **Motion is held to the CPU**: a frame looked one whole pixel aside has motion (1, 0) within
  0.02 px on every pixel whose id did not change; a swaying palm's, a skinned fox's, a moved
  instance's and a dune ground's motion equal the CPU mirrors' two positions projected
  (`gfx::flora_bend` and the others exist) within 0.05 px.
- **A replay with history draws the same bytes**: the accumulation is a per-pixel function of the
  frames before it, with no atomic and no order in it, so two flights of one path from a reset
  are byte-identical on one device (with the bounce off, as every such comparison is today).
- **A vendor's output is never compared to bytes** and never runs in CI (no SDK is there): the
  registration point is tested with the engine's own provider and a null one that copies its
  input.

### What each input costs

Derived (bytes are exact; milliseconds are estimates until stage 1 and 2 measure them; nothing
here was measured, because nothing here exists):

| Input | Bytes a pixel | 3840 × 2160 | 11520 × 2160 | Time, estimated on the RTX 5090 at 3840 × 2160 |
|---|---|---|---|---|
| motion, two half floats | 4 | 33 MB | 100 MB | 0.1 to 0.3 ms: a word read and a matrix a pixel where nothing is deformed, three loads more where it is |
| last frame's visibility buffer | 8 | 66 MB | 199 MB | none (the buffers swap) |
| history colour, two half-float images | 16 | 133 MB | 398 MB | inside the accumulation |
| the linear image (exists already under a frame pass) | 8 | 66 MB | 199 MB | the encode it implies exists already |
| the reactivity mask, only with water or a medium | 1 | 8 MB | 25 MB | inside the resolve |
| the pool's second block (way (c)) | the pool's budget again | 8 MiB default, 64 MiB at the owner's setting | the same | the pool pass again: 0.42 ms on the forest's lake frame, whatever the size |
| the accumulation itself | | | | 0.3 to 0.6 ms: nine neighbours, five history taps, one write (about 50 bytes read and 8 written a pixel) |

At 3840 × 2160 that is about 300 MB and an estimated 0.8 to 1.3 ms for the engine's own
accumulation at native resolution, on a lake frame of 16.96 ms. **A scene drawn with it off costs
nothing**: no image is made and no pass recorded. **An imported asset gets all of it**: motion is
from the instance's transform, the pool or the depth, and the masks from a material's own flags;
nothing reads a generator's output.

## Part 3: the candidates

Read on 2026-11-14, through a fetch tool's extraction of each page (so "read" means the whole text
passed through an extractor asked specific questions, and a quotation is the extractor's; where a
figure is from an earlier version I knew and was not on tonight's page it is marked *earlier*). No
SDK or binary was downloaded, and nothing of this project was sent anywhere.

| Method | What was read, its version and date | Inputs | Vulkan | Licence |
|---|---|---|---|---|
| **The engine's own** (accumulation at native; then at a ratio) | this page; Karis 2014, Salvi 2016, Yang, Liu and Salvi 2020 from the published versions, not reopened tonight | Part 2's | it is the renderer | Apache-2.0, the engine's |
| **DLSS super resolution and DLAA** (NVIDIA) | the Streamline repository's front page (SDK 2.14.16; no date on the page) and its `ProgrammingGuideDLSS.md`, whole; `github.com/NVIDIA/DLSS` front page (no version on it); `LICENSE.txt` there, whole. **Not read**: the DLSS Programming Guide itself (a PDF; third-party copies are dated 18 Dec 2025) | "depth, motion vectors, render-res input color and final-res output color"; exposure "strongly advised"; the offset in pixels; matrices without the offset; one set of tags per viewport id; the internal size asked of the SDK per mode (*earlier*: 67, 58, 50 and 33 % a side) | yes (Vulkan 1.2 or higher) | **NVIDIA RTX SDKs License, 14 March 2024**, proprietary |
| **DLSS ray reconstruction** | `ProgrammingGuideDLSS_RR.md`, whole (2.14.16) | a *noisy* ray-traced colour, diffuse and specular albedo, normals, roughness, motion, depth, specular motion or hit distance; "completely overrides DLSS (Super Resolution)"; HDR input only; no dynamic resolution | the page's examples are D3D; Vulkan fields exist | the same |
| **DLSS frame generation** | `ProgrammingGuideDLSS_G.md`, whole (2.14.16) | the final colour, the colour before the UI, the UI's alpha, depth, motion; Reflex through Streamline is required | yes, with limits: no vsync with it on Vulkan, no dynamic multi-frame generation on Vulkan, Streamline takes over acquire and present | the same; the plugin is "prebuilt DLLs only" |
| **FSR 3.1 upscaler** (AMD, analytic) | the manual at gpuopen.com for 3.1.4: the first 100,000 characters whole and the jitter, bias, reset and stages of the rest; the SDK repository's front page ("AMD FSR SDK 2.4.0 'Redstone'", upscaler 3.1.6; no date) | colour, depth, motion (all opaque, alpha-tested and alpha-blended objects), exposure or its auto flag, a reactive mask and a transparency-and-composition mask, `reset`; ratios 1.0 (native AA), 1.5, 1.7, 2, 3 | yes ("Vulkan 1.x") | **not verified**: the front page says the *samples* are MIT; the SDK's own licence file was not found at the two paths tried |
| **FSR 4 upscaler, ML frame generation, ray regeneration** | the same front page (upscaler 4.1.2, ray regeneration 1.2.1); AMD's article of 10 December 2025 | as FSR 3.1's | the front page: "DirectX 12 or Vulkan applications", and "Vulkan is currently not supported for Ray Regeneration and Neural Radiance Caching" | as above, unverified |
| **XeSS super resolution** (Intel) | the repository's front page ("XeSS 3"); the XeSS-SR Developer Guide 2.0 (© 2025), whole; `LICENSE.txt`, whole | the offset, the colour, and either dilated motion at the output's size or motion at the internal size with depth; exposure (a value, a texture or auto); a responsive pixel mask; ratios 1.0 (native AA), 1.3, 1.5, 1.7, 2, 2.3, 3 | yes: Vulkan 1.1 with `shaderStorageImageWriteWithoutFormat` and `mutableDescriptorType` | **Intel Simplified Software License, October 2022**: "in binary form only", unmodified, proprietary |

From a search's summaries only, not from a primary page: a DLSS 4.5 SDK with dynamic multi-frame
generation has been released, and a "DLSS 5" was announced for the autumn of 2026 for the RTX 50
series, with no date as of late August. Neither changes anything below; both say the vendors'
side moves faster than a design should chase.

**Against the project's four machines.**

| | RTX 5090 (desktop, the owner's surround) | RTX 4070 laptop | GTX Titan X, Maxwell (the garage box, 1440p) | Titan Xp, Pascal (the headless Linux server) |
|---|---|---|---|---|
| the engine's own | yes | yes | yes | yes |
| DLSS, any part | yes | yes (how many frames it may generate is asked of the SDK) | **no**: not an RTX card (the SDK answers "no supported adapter") | **no** |
| FSR 3.1 upscaler | yes | yes | should run (Vulkan 1.x is all its manual asks); not tried | should run if the SDK's Vulkan backend builds on Linux; not checked |
| FSR 4 and the other ML parts | **no**: AMD's article: they require RDNA 4 (Radeon RX 9000); other hardware gets "analytical" paths | no | no | no |
| XeSS-SR | yes if the two Vulkan features are there | the same | the same, to be probed (`engine-cli gpu.adapters`) | **no**: the SDK is Windows libraries |

Two things follow. **Only the engine's own runs on all four**, and it is the only one a test can
hold to bytes. And **FSR's ML parts cannot be run on any machine the project has**, so a provider
for them could be written here and verified nowhere.

**Licence against [ADR-0014](../adr/0014-apache-2-license-and-dependency-policy.md).** The ADR
already names this case: "proprietary SDKs (NVIDIA RTX SDKs License, DLSS/FSR/XeSS binaries,
DirectStorage) never enter the repository or the engine binary and are exposed only as optional
plugin modules the game developer fetches under the vendor's terms". Tonight's reading confirms
each term it rests on:

- **NVIDIA's** grants distribution "as incorporated in object code format into a software
  application" only, bars standalone distribution, says "You may not use the SDK in any manner that
  would cause it to become subject to an open source software license", licenses DLSS "only for
  their use in systems with NVIDIA GPUs", requires NVIDIA's marks on a splash screen or credits,
  and requires notice to NVIDIA before a commercial release. None of that can live in an
  Apache-2.0 repository or its `third_party/`. Streamline's own framework is under an MIT-style
  notice (NVIDIA, 2023) and could be vendored, but it is a loader for binaries that cannot, and
  since its 2.7.32 the binaries are not in its repository either.
- **Intel's** is binary-only and unmodified with its notice reproduced: permissive in spirit,
  not in ADR-0014's list, and a binary: a plugin.
- **AMD's** is the open question: if the SDK's source is MIT as its earlier releases were, the
  FSR 3.1 upscaler *could* be vendored under ADR-0014. This page proposes not to: AMD ships it to
  be loaded as its own signed library, the engine's own method covers the same ground on the same
  hardware, and one integration route for every vendor is simpler than two. If the owner wants an
  open analytic upscaler in the tree, its licence file is the first thing to read.

**How each is integrated**, in this renderer's terms: all of them are a call between the linear
image and the encode with Part 2's inputs; DLSS through Streamline wants its own `sl` calls round
device creation and (for frame generation only) the swapchain, which is why frame generation
touches the presenting hosts and the others do not; FSR and XeSS are a context made per output
size and one dispatch a frame.

**Known weaknesses on thin alpha-tested foliage and on animated water**, from the documents and
the literature, not from pictures taken here: every method needs motion for alpha-tested and
vertex-animated geometry and smears foliage whose wind is not in it (FSR's manual says so in as
many words; it is the commonest fault in shipped games, and this renderer can give exact motion
for every swaying vertex); sub-pixel features dim, soften or flicker under a clip, and more at a
ratio, where a one-pixel leaf at the output is a third of a pixel at the input (the reason FSR has
locks, and the reason Part 1's second content fault matters more with an upscaler than without);
a more negative level bias brings back the flicker it sharpens (XeSS's guide); and a surface
whose colour is not its own (water's refraction and reflection, a flowing pattern) ghosts unless
it carries a mask or the flow is in the motion (FSR's two masks, XeSS's responsive mask, DLSS-RR's
transparency inputs exist for exactly this). The learned methods are reported to hold thin
detail better than the analytic ones; no source read tonight gives a number, and the measurement
that would is this page's method run on each provider's output.

## Part 4: the plan, in stages

The shape the brief expected holds, with two changes: the content rows (R155, the rasterizers'
thinner crowns, and R156, the far crowns' sub-pixel geometry) run beside stage 1 because no method
fixes them and upscaling makes them worse, and the measurement rule is stage 0 because it has to
exist before the first figure with reconstruction on does. The row ids are proposals; the
coordinator numbers them.

| Stage | What is built | What it buys | The measurement that closes it |
|---|---|---|---|
| 0 | **The measurement rule in the tools** ([the proposed decision](#proposed-decision), points 5 to 8): the summary's `reconstruction` block, the budget gate's and the flythrough matrix's refusals | no figure with reconstruction on can be mistaken for a cost | a summary with reconstruction on is refused by `tools/frame-budget.ps1` and has no top-level `gpu_ms`; one with it off is byte-for-byte today's but for the new block |
| 1 | **The inputs** (R154): the offset in `ViewSet::update`, last frame's eye, views and moved transforms, the pool's second block, `motion.slang`, the kept visibility buffer, a `motion` capture channel. Off by default; nothing reads them yet | everything after it; and this page's reference from the renderer itself | off is today's bytes; motion against the CPU within 0.05 px on the five kinds of Part 2; the motion pass and the second pool block measured at 3840 × 2160 and 11520 × 2160 on the forest's lake, with ways (a) and (c) as a pair |
| beside 1 | **A masked material's coverage is one coverage** (R155): find why the rasterizers' mips draw less crown than level 0, and fix the chain or the baked atlases | a traced shadow and a rastered crown agree; the baseline tier's forests stop thinning with distance | leaf-cards mid and far: the supersampled crown's coverage under `--raster hw` within 2 % of the traced one's (23 % and 16 % apart today) |
| beside 1 | **The far crowns' geometry against the pixel** (R156, the foliage's owner): the coverage error and the far forest taking over before a group's leaves are under a pixel, masks pre-filtered | less work for detail only an average shows; the case every reconstruction handles worst, made rare | on the forest's two hillsides: pixels holding eight or more triangles under 10 % (42 % today) at no more than today's native frame time |
| beside 1 | **The native frame on every machine** (R161): the forest's lake and a hillside flown at native resolution on the laptop, the garage box and the server, and at 11520 × 2160 on the desktop | the table below with measurements in place of arithmetic | a flythrough summary per machine and size on one main |
| 2 | **The engine's own accumulation at native resolution** (R157): the pass between the frame passes and the encode, the instance test, the clip, `mean` mode, the fade's hash taking the phase | the interference's answer on every machine; deterministic; DLAA's equal in kind | on leaf-cards' mid crowns: a held view within 8 codes rms of the reference after 16 frames (29.2 drawn); the slow pan's and the wind's frame-to-frame change within 1.5× the reference's (30.3 and 12.2 drawn against 4.3 and 3.5); after a reset no pixel holds more than 2 % of the frame before it at the eighth frame; a replay's bytes twice; the frame loop allocating nothing; its cost at native as a pair on the forest |
| 3 | **A registration point for a reconstruction provider** (R158), as sky providers and frame passes register: one struct of inputs, one output, the engine's own as the provider built in, a null one for the tests | the vendors become additions, not edits | the engine's own through the point draws stage 2's bytes; the null provider's frame is the resolve's |
| 4 | **Internal resolution below the output's** (R159): the ratio per view in `ViewQuality`, the cut held to the output's pixels, the level's bias, the longer sequences | the frame rate the lower machines and the surround need | at each ratio on leaf-cards and the forest, FLIP and this page's flicker figures against the native accumulated picture; the summary's two sizes; stage 0's refusals holding |
| 5 | **A vendor's provider as an optional capability** (R160), DLSS super resolution and DLAA first: a module off by default whose SDK the consumer supplies by path | the learned method's quality on the two RTX machines | the build with the switch off and the minimal build unchanged; nothing of the SDK in the tree or a package unless its consumer puts it there; this page's figures on its output beside the engine's own |
| 6 | **Rays given back under accumulation** (R162): the sun's ray on alternating pixels | milliseconds at native resolution, which is the only place they count | a native pair on the forest; the converged picture within the bound stage 2 set |
| later | DLSS ray reconstruction (it wants a noisy path-traced input this renderer does not make in real time), frame generation (the presenting hosts, Reflex, and on Vulkan no vsync with it), FSR and XeSS providers | | each its own decision |

### What a ratio is, and what it buys on today's frame

| Mode (the engine's names; FSR's ratios) | a side | 3840 × 2160 | 11520 × 2160 | 2560 × 1440 | pixels |
|---|---|---|---|---|---|
| native (accumulation only; DLAA) | 1.0 | 3840 × 2160 | 11520 × 2160 | 2560 × 1440 | 100 % |
| quality | 1/1.5 | 2560 × 1440 | 7680 × 1440 | 1707 × 960 | 44 % |
| balanced | 1/1.7 | 2259 × 1271 | 6776 × 1271 | 1506 × 847 | 35 % |
| performance | 1/2 | 1920 × 1080 | 5760 × 1080 | 1280 × 720 | 25 % |
| ultra performance | 1/3 | 1280 × 720 | 3840 × 720 | 853 × 480 | 11 % |

DLSS asks its SDK for the size per mode and XeSS names the same ratios one step apart (its
"quality" is 1.7); a provider maps the engine's ratio to its nearest mode and the summary records
the size it actually drew at.

**Today's measured frame**: the forest's staging, the lake's path, 3840 × 2160, traced primaries
and shadows, the bounce on, main at 61ccce67 on the RTX 5090 (row T129's flight of 23:22 on
2026-11-13, a median of 120 frames, the machine shared): **16.96 ms**.

| Scales with the pixels | ms | Does not | ms |
|---|---|---|---|
| the resolve (shading, the sun's rays, the bounce's read, water) | 10.69 | the cull (per pair, per view) | 0.58 |
| the primary rays | 1.97 | the acceleration structures' chain (records, cluster and bottom-level builds, the top level) | 1.33 |
| the bounce's rays | 0.90 | the pool pass and its allocator | 0.45 |
| the frame passes (the medium; the encode stays at the output's size) | 0.88 | the bounce's fold | 0.11 |
| the sky | 0.04 | the frame thread's CPU (about 1.3 ms a frame here) | |
| **together** | **14.4** | **together** | **2.5** |

So a ratio of n costs about 2.5 + 14.4 / n² ms plus the reconstruction (an estimated 0.5 to 1 ms
for the engine's own at that output): **about 9.5 ms in quality, 8 in balanced, 6.7 in
performance and 4.7 at a third**, against 17 native, and never under about 3 ms whatever the
ratio. The saving is less than the pixels' share wherever the bias makes each pixel's textures
dearer. These are arithmetic on one measured frame, not measurements.

**The surround and the lower machines.** No frame of this main was measured tonight at
11520 × 2160 or on the other three machines, so there is no honest figure to scale; what exists is
older: the forest's v25 walk at 11520 × 2160 drew in 44.2 ms on 2026-11-02 and a dense v34 view in
95 ms on 2026-11-07, both with the resolve and the primary rays as nearly all of it
([renderer](../subsystems/renderer.md#where-the-resolves-time-goes)), which is the same
proportion: performance mode would put such frames near a third of their native time, with the
cull of three views and the union's structures as the floor. The two older cards cannot trace, so
their frame is the vertex path and the cascaded maps, whose raster cost follows the cut and not
the pixels: a ratio buys them less than it buys the 5090, and how much is row R161's flight on
each. The plan's own targets ([04 §4.1](../plan/04-renderer.md)) are 60 frames a second at
2560 × 1440 on an RTX 4070 class "internal resolution ≥ 67% with upscaler", which is quality mode
exactly, and at 3840 × 2160 on a 4080 or 5080 class "with upscaler".

## Proposed decision

*Written as an ADR's body; the coordinator numbers it at the merge.*

### Context

Dense alpha-tested foliage drawn at one sample a pixel differs from its own supersampled picture
by tens of display codes and flickers under any motion ([Part 1](#part-1-what-the-interference-is));
the frame rates the plan promises below the top machine and on a surround assume an upscaler
(04 §4.1). The owner's rule until 2026-11-14 was "performance first, no upscalers", and its
meaning, in the owner's words that day, is that an upscaler is never the reason a cost is
accepted. ADR-0014 keeps proprietary SDKs out of the repository and the engine binary.

### Decision

1. **The renderer samples over time.** Its inputs (a sub-pixel offset per frame, motion for every
   pixel from the visibility buffer, last frame's visibility buffer, a reactivity mask) are the
   renderer's own, computed from what any mesh has: a transform, a deform stage, a depth. No
   input keys on a generator.
2. **The engine's own accumulation is built first, at native resolution, and is the reference
   implementation**: Apache-2.0, on every device the engine runs on, deterministic (the same
   frames from a reset draw the same bytes), and held by tests. It is **off by default** until the
   owner turns it on after walking it.
3. **Reconstruction is a registration point** (`systems/renderer`): one provider per renderer,
   the engine's own built in. A provider is handed the inputs and writes the output image; nothing
   below a host names one.
4. **A vendor's method is an optional capability whose SDK its consumer supplies** (ADR-0014,
   ADR-0027): a module that is off by default and in the minimal build, compiled only when the
   consumer names the SDK's directory, whose headers, libraries and binaries are never committed
   and never in `third_party/`, and that the engine is complete without. A package carries a
   vendor's binary only when its consumer adds it under the vendor's terms.
5. **Every measurement is native.** Every frame budget, cost pair, benchmark and observation
   from a walk is taken with reconstruction off at the output's own resolution. **A row's merge
   fact is never a figure with reconstruction on**, and **no cost is accepted because a
   reconstruction mode makes the frame rate acceptable**: a frame that misses its budget natively
   has missed it.
6. **The tools hold it.** `renderer::ResolvedSettings` carries what ran, as it does for every
   setting, and every flythrough summary and frame record has a `reconstruction` block: the
   provider (`off`, `engine`, a vendor's name), the mode, the internal and the output sizes.
   With a provider other than `off`, or an internal size other than the output's, the summary's
   times are written under `reconstructed.gpu_ms` and **the top-level `gpu_ms` is absent**, so a
   tool that reads a cost finds none. `tools/frame-budget.ps1` refuses such a summary with an exit
   code of its own, as it refuses a run it cannot trust today; `tools/flythrough.ps1`'s matrix and
   `--benchmark` never turn reconstruction on unless asked by a flag that names it; a session's
   `--report` carries the block, so a walk's observation states it.
7. **A figure with reconstruction on is labelled and never alone**: it reads
   "reconstructed (provider, internal size → output size)" and stands beside the native figure of
   the same flight on the same machine.
8. **The one exception is reconstruction's own cost**: a row that changes a provider or its
   inputs reports the pair on and off, natively sized, and its merge fact is that difference.
9. **Under a ratio the cut is the output's**: the LOD thresholds are held in output pixels, so
   reconstruction never coarsens geometry, and what it saves is shading.
10. **One history per signal**: a term that already remembers frames (the bounce's cache) is not
    given a second history, and no ray count is lowered on the grounds that accumulation hides
    its noise without a native pair that shows the picture holds.

### Consequences

The engine gains about 300 MB of images at 3840 × 2160 and an estimated millisecond when the
feature is on, and nothing when it is off. Tests keep comparing bytes, because off is the default
and the converged frame is a function of the scene. A consumer with only static imported meshes
gets the whole of it. A vendor's quality on thin foliage is available on the hardware that vendor
supports and nowhere else; the engine's own is the floor everywhere. The content faults this page
found are not reconstruction's to fix and are rows of their own. The owner's surround gets a ratio
per view. Frame generation, if ever built, reports the rendered frame's time as the frame's time
and the presented rate as a second, labelled figure, by point 7.

### Revisit when

A vendor's licence changes so that its SDK could be vendored; the engine gains a stochastic
real-time light path (ray reconstruction then has an input); or a walk shows the engine's own
accumulation is not good enough on a machine no vendor's method runs on.

## Sources

Read 2026-11-14 unless marked.

- NVIDIA, *NVIDIA RTX SDKs License* (v. March 14, 2024) with the RTX supplement,
  `github.com/NVIDIA/DLSS`, `LICENSE.txt`: whole, through an extraction. Its sections 1c, 2a to 2c,
  4a to 4g and the supplement's 1, 3, 4 and 7 are what Part 3 quotes.
- NVIDIA, Streamline SDK 2.14.16, `github.com/NVIDIA-RTX/Streamline`: the front page,
  `docs/ProgrammingGuideDLSS.md`, `ProgrammingGuideDLSS_RR.md`, `ProgrammingGuideDLSS_G.md` and
  `license.txt`, each whole, through an extraction. The DLSS Programming Guide (PDF) was not read.
- AMD, *FidelityFX Super Resolution 3.1.4 (FSR3) - Upscaler*, the manual at gpuopen.com: its first
  100,000 characters whole, and the jitter, mip bias, reset and stage descriptions from the rest.
  AMD FSR SDK 2.4.0 "Redstone", `github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK`: the front
  page. *AMD FSR Redstone for developers and the neural rendering future*, gpuopen.com,
  10 December 2025: whole. The SDK's licence file: not found, not read.
- Intel, *XeSS Super Resolution (XeSS-SR) Developer Guide 2.0* (© 2025) and `LICENSE.txt` (Intel
  Simplified Software License, October 2022), `github.com/intel/xess`: whole, through an
  extraction; the repository's front page.
- B. Karis, "High-Quality Temporal Supersampling", SIGGRAPH 2014 course *Advances in Real-Time
  Rendering*; M. Salvi, "An Excursion in Temporal Supersampling", GDC 2016 (variance clipping);
  L. Yang, S. Liu, M. Salvi, "A Survey of Temporal Antialiasing Techniques", *Computer Graphics
  Forum* 39(2), 2020. Cited from the published versions as known; not reopened tonight, so a
  detail attributed to them here is to be checked against them when stage 2's design is written.
- Already cited by the pages this builds on: Castaño, "Computing Alpha Mipmaps", 2010; Golus,
  "Anti-aliased Alpha Test", 2017 ([renderer](../subsystems/renderer.md#alpha-tested-geometry),
  [texture](../subsystems/texture.md#alpha-tested-masks)).

## What was not done

- **No canopy seen from inside on the forest.** One load of the staging was flown for five views;
  the watchdog ended it at 3.7 GB of free commit (other work on the machine took the rest) after
  three: two hillsides and a near view that turned out to be ground and trunks. The near canopy's
  figures are leaf-cards' near view.
- **The bounce on.** Every set has `renderer.gi.mode=off`, so what the bounce's own frame-to-frame
  difference adds to a held view is not on this page.
- **No cost was measured**: every millisecond in Part 2 is an estimate, and stage 1 and 2 close
  on measurements.
- **The cause of the rasterizers' thinner crowns** was not found, only measured.
- **No picture of any vendor's output** was made or looked at; their weaknesses are from their
  documents and the literature.
