# Lights by reach: the grounding, the design and the numbers

Roadmap R87 (2026-11-23, batch 74). The renderer's page has the rules that follow
([renderer](../subsystems/renderer.md#lights-by-reach)); this page has what was read, what was
taken from it, the numbers chosen and the measurements.

## The question

Until R87 the resolve walked every light of the frame's array at every pixel: the stand-in's two
(or a sky's moon), the four fires nearest the eye, and every lamp up to 64 (R86). A lamp past its
reach cost a distance test and a branch; one in reach its BSDF and, under traced shadows, its ray.
R86 measured the walk at 64 lamps that all reach every pixel: 4.1 ms more of a traced frame at 4K
([renderer](../subsystems/renderer.md#lamps), "What it costs"). The row asks for a pixel to pay for
the lights that reach it, for the fires to join the lamps past four without a nearest-four rule
that pops, and for the nearest few to be shadowed.

## What was read

| Source | Read how | What it says that matters here |
|---|---|---|
| Olsson, Billeter and Assarsson, "Clustered Deferred and Forward Shading", HPG 2012 | **The paper not read** (the preprint's fetch failed; tried again in R87's mend, 2026-11-24: the preprint arrives as compressed PDF bytes with no readable text, and the Eurographics library and the Chalmers listing refuse the fetch); its abstract and the Chalmers listing through a search. **What it bears on: the grid's kind** — its clusters are view-space (screen tile × depth slice), built over the camera's samples, which is the choice this page turns down for one in the frame's space that off-screen readers can ask | Grouping view samples by their 3D position (screen tile × depth slice) maps lights to samples better than tiles alone, whose 2D groups take every light along a tile's depth range; per-cluster back-face culling of lights by normal is an option. |
| Harada, McKee and Yang, "Forward+: Bringing Deferred Lighting to the Next Level", Eurographics 2012 (short) | The authors' project page (takahiroharada.github.io/forward+); **the paper not read** | Screen tiles of about 16 × 16 pixels after a depth prepass; lights culled per tile on the GPU either by a group per tile testing every light (gather) or a thread per light writing light-tile pairs that are then sorted (scatter); the shading pass reads the tile's list. |
| Filament's documentation, "Physically based rendering in Filament" (google.github.io/filament), sections on punctual lights and clustered forward rendering | Read through the fetch tool, whole | The punctual light's falloff windowed to zero at a radius of influence, E = I / max(d², 0.01²) · ⟨1 − d⁴/r⁴⟩², credited to Karis 2013: "window the inverse square function in such a way that the majority of the light's influence remains unaffected". Lights assigned to **froxels**: 80 × 80-pixel tiles by 16 exponential depth slices (the first slice set by hand, 0.1 to 5 m), at most 32 lights a froxel in an index list ended by a sentinel, assigned on the GPU by compute, or **on the CPU by rasterizing each light into the froxels it touches**, which the text says culls more tightly. |
| Karis, "Real Shading in Unreal Engine 4", SIGGRAPH 2013 course notes | **Not read** (the fetched PDF had no readable text; tried again in R87's mend from two hosts, the same); its falloff through Filament's citation above. **What it bears on: the falloff's window**, `(1 − (d/r)⁴)²` over the squared distance, whose exact zero at the radius is what makes the cull invisible; the engine's form drops Karis's `+1` in the denominator and floors both `r²` and `d²` at 1e-8 (`brdf.slang`), which gives a light of radius zero a reach of 1e-4 m, so the grid takes no reach under that (R87's mend) | The windowed inverse square the engine already uses. |
| Ward, "Adaptive Shadow Testing for Ray Tracing", Eurographics Rendering Workshop 1991 (R213) | Read through the fetch tool, whole, from the author's Radiance pages (floyd.lbl.gov/radiance/papers/erw91) | Sort the sources in front of a point by their potential contribution, test them in decreasing order, and stop when the untested remainder is below a tolerance of what the tested ones gave; add each untested source at an estimated visibility — the running ratio of hit to tested shadow rays over the render so far, overall and per source — "not randomly accepted or rejected", so the result is biased but not noisy. A certainty parameter weighs how much of the remainder the stop counts. Measured: at a 20% tolerance and 50% certainty about 80% fewer shadow tests with an average pixel error under 5%; render time roughly in proportion to the share of sources tested; the estimate is crudest early in a render and worst on fully lit surfaces, and the visibilities are assumed independent ("an approximation"). |
| Walter, Fernandez, Arbree, Bala, Donikian and Greenberg, "Lightcuts: A Scalable Approach to Illumination", SIGGRAPH 2005 (R213) | **The paper not read** (both of the authors' PDF hosts arrive as compressed bytes with no readable text); its project page's abstract, and the 2% threshold and ray counts through a search of the authors' course slides (Cornell CS6620, 2009; the 2012 many-lights course notes) | Lights in a binary tree; at each point a cut through it whose clusters each carry an upper bound on their error (visibility bounded by 1), refined until every cluster's bound is under a relative threshold of the total — **2%, from Weber's law** (a contrast below about 2% of the signal is not seen); "a few hundred shadow rays" for thousands of lights (the kitchen: 259 a pixel for 4,608 area lights). The bound is the same shape as the cap's: what is not traced is bounded by its unshadowed contribution. |
| Bitterli, Wyman, Pharr, Shirley, Lefohn and Jarosz, "Spatiotemporal Reservoir Resampling for Real-Time Ray Tracing with Dynamic Direct Lighting" (ReSTIR), SIGGRAPH 2020 (R213) | NVIDIA's publication page through the fetch tool; **the paper not read** | Millions of dynamic emissive triangles under 50 ms a frame with at most 8 rays a pixel, by resampling light samples **from neighbouring pixels and from earlier frames**: a stochastic pick that is noisy by itself and settles through that reuse. |
| Lagarde and de Rousiers, "Moving Frostbite to PBR", SIGGRAPH 2014 course notes | **Not read**; the search results show Unity's render pipeline citing it for `SmoothDistanceAttenuation`, a window of the squared distance over the squared radius, called a non-physical limit to the light's influence | The same choice: a finite radius is a limit put on a light so it can be culled, and the window keeps the inverse square everywhere but near the radius. |

## What was taken

**The reach was already finite, and exact.** `brdf_distance_attenuation` in `brdf.slang` (and
`distance_attenuation` in `brdf_reference.h`) is `(1 − (d/r)⁴)² / d²`, Karis's window without his
`+1` in the denominator, clamped so that at and past the reach the ratio is 1 and the window 0:
**zero exactly**, not small. A fire's reach is where its brightest flicker falls to 0.05 lux (R85),
a lamp's its block's `reach_m` or the same rule (R86), the stand-in's two lights 4 × the scene's
radius. So no falloff had to change: `brdf.slang` and `brdf_reference.h` are untouched, and a walk
that skips a light whose sphere misses the point adds the same terms in the same order. Any light
whose window is above zero at a point must be walked there; any other may be walked or not, and a
light walked where it does not reach returns from `light_sample` before it adds anything.

**A grid in space, not a list per screen tile.** Every published structure above is built over the
camera's samples: a tile's frustum, a froxel. That serves the camera's pixels and nothing else, and
the array has readers off the screen — a reflection's hit, a bounce's hit, the reference path
tracer's vertices — which would each need a walk of their own. Filament's CPU path shows the other
way round: rasterize each light into cells. So the engine's structure is **a uniform grid in the
frame's space** (the eye at the origin, ADR-0053) over the union of the finite lights' spheres,
one bit a light in each cell, and every reader asks it with its own point:

| Reader | What it asks | What it costs |
|---|---|---|
| The resolve's pixel (`fs_resolve`) | Its reconstructed point's cell | One cell lookup (a subtract, a multiply, a compare, a word read) and a bit walk over the lights that reach the cell. |
| The water's traced reflection (`hit_shade.slang`) | Today lit by the sun alone, no point light: asks nothing | Would be the same lookup at its hit point. |
| The bounce's hit (`gi_update.slang`) | Today the sun alone: asks nothing | The same. |
| The reference path tracer (`path_trace.slang`) | Today `frame_lighting`'s lights only (the stand-in's two or the moon), no fire or lamp: walks them all | `light_walk_begin` with no grid walks the array as before; with one, the lookup at each vertex. |

A bit per light rather than an index list (Filament's) because the walk must keep the array's
order, which a bitmask in increasing order gives for nothing and an index list only if it is
sorted, and because a cell's words are fixed by the light count (a word for each 32), so the
cells are one array with no counts or offsets and no list can overflow.

**Built on the CPU, every frame.** The array is filled on the CPU (`add_fire_lights`,
`add_lamp_lights`) and copied into the frame slot's host-visible resolve block; the grid is built
from the same array beside it, into a scratch sized once, and copied behind the sky's block in the
same buffer, which nothing on the GPU writes. No pass, no barrier, and nothing to declare beyond
what the resolve already reads through `ResolveParams::lights`.

## The numbers chosen

- **The room**: `gfx::k_light_grid_words` = 65,536 words (256 KiB a frame slot, and as much again
  of scratch): cells × words a cell never pass it. A frame with more lights has fewer, larger
  cells; nothing is ever left out, so there is no overflow to count.
- **The cell's edge**: the cube root of the grown spheres' box over the room, never under a
  quarter of the smallest reach (`k_light_grid_cell_floor`): a cell much smaller than the spheres
  buys little culling (a cell of a quarter-reach holds the lights within about 1.4 reaches of any
  point in it) and costs words. With 64 lamps of 8 m on lantern-street's 60 m square (and the
  moon, walked first), 2 words a cell, the box is 69 m by 16 m by 69 m and the edge the floor's,
  2 m.
- **The margin**: a light's reach is grown by a 256th of itself, a centimetre and a 64th of the
  cell's edge (`gfx::light_grid_margin`): the first two for the f32 arithmetic of the distance the
  window is computed from, the last for the f32 arithmetic that finds the reader's cell (its error
  is at most dims · 2⁻²³ of a cell). A grown sphere only sets more bits, and an extra bit costs a
  distance test and changes nothing.
- **The least reach** (R87's mend): `brdf_distance_attenuation` divides by `max(r², 1e-8)`, so its
  window is above zero wherever d² < max(r², 1e-8), and a light of radius zero with an intensity
  lights the points within 1e-4 m of it (at 5e-5 m, (d/r)² = 0.25 and the attenuation is about
  3.5e8). The builder takes no reach under 1e-4 m, so such a light keeps its bits; the first build
  skipped it, and the culled walk would have dropped it there.
- **Inputs that are not numbers** (R87's mend): a NaN or infinite position or reach builds no
  grid, before any of it reaches a cast (the first build cast `ceil(NaN)` to an integer, which
  MSVC makes 2⁶³, and grew the edge forever); the room test is taken in f64, so a finite box too
  long for any integer count of cells grows the edge rather than overflowing one.
- **The leading lights**: lights with no reach (a directional light: the moon) are walked at every
  point. They come first in the array today; a directional light behind a finite one would make
  the walk's order differ from the array's, so the builder refuses that array and the readers walk
  it whole.

## The fires, and which lights are shadowed

**Every fire is lit**, in the scene's order, up to `k_max_fires` (64): the four-nearest rule is
gone, and with it the only choice of lights that depended on the eye. The lamps' room went from 64
to 256 (`k_max_lamps`), since a pixel no longer pays for the room; the array is 322 lights, 20 KiB
a frame slot.

**Every light that reaches a pixel and is not too faint to test is shadowed** under `--shadows rt`
— the rule R86 left, with R115's faint-light test (Ward 1991) — so the shadow rays at a pixel are
bounded by the lights that reach its cell, and the set is a function of the point and the lights
alone: no ranking, no eye, nothing to flicker as the eye moves. **A cap on the rays a pixel traces
("the nearest few") is not built**: it would choose among the lights that reach a point, and any
choice that is not a function of the point alone pops; the design that would keep it steady is a
ranking by each light's unshadowed contribution at the point with ties to the lower index, a few
kept, the rest at the visibility the pixel's own tested lights found (Ward's estimate, which the
faint-light rule already uses). It is left for when a scene measures more rays at a pixel than the
frame can pay for — which the fires below did, and R213 built it ([the cap](#the-cap)). On lantern-street at most **5** of its 64 lamps reach one point and at most
**16** of the 256 (counted over a 2.5 cm lattice of a lamp cell at heights 0 to 3 m, the ground
and the walls, by the 8 m spheres; R87's mend). The fires below are the case that measures more.

## The measurement

**lantern-street** ([its README](../../content/test-scenes/lantern-street/README.md)): 4, 16, 64 and
256 lamps of 40 cd and an 8 m reach on 2 × 2 to 16 × 16 grids over the same 60 m square among 64
walls, and the street with none; 3840 × 2160, `--raster rt --shadows rt`, the owner's settings (the
bounce with `renderer.gi.guide` and `renderer.gi.memory`, `renderer.reconstruction.accumulation=on`,
`renderer.shadow.micromaps=true`), `--no-cache` on both sides, the held path's frames 240 to 480
(241 frames a flight). Base: `cc95d783a` built in `msvc-release` and copied out; tip: the branch
(`eab100e1d`), same build. RTX 5090, 2026-11-23 22:54 to 23:30, under the GPU lock; **machine
state**: the harness's warning on every flight, other processes 2 to 60 % of the CPU at the
flights' starts (other agents building) and the GPU 0 to 6 % busy at each start, so the numbers
are upper bounds; two flights of each cell, interleaved base and tip, agree within 0.01 ms.

| lamps | base resolve, median (p95, p99) | tip resolve, median (p95, p99) | base − tip |
|---:|---|---|---:|
| 0 | 0.814 (0.827, 0.832); again 0.813 | 0.812 (0.822, 0.826); again 0.820 | 0.00 |
| 4 | 0.851 (0.862, 0.867); again 0.849 | 0.826 (0.839, 0.843); again 0.820 | 0.03 |
| 16 | 1.008 (1.021, 1.025); again 1.012 | 0.884 (0.897, 0.904); again 0.887 | 0.12 |
| 64 | 1.702 (1.740, 1.747); again 1.711 | 0.966 (0.981, 0.984); again 0.971 | 0.74 |
| 256 | refused: past the 64 a frame lit | 1.372 (1.392, 1.396); again 1.374 | — |

So **the lamps' share of the resolve went from 0.89 ms to 0.15 ms at 64 lamps**, and 256 lamps
cost 0.56 ms where 64 cost 0.89 before. What is left grows with the lamps that reach a pixel and
its cell's coarseness, not with the frame's count: 64 to 256 is four times the lamps at the same
reach, so four times the lamps reaching each point. A street with none costs what it cost.
Nothing else in the frame moved (the GPU total moved by the resolve's difference), and no frame
allocated.

### The fires, measured

R87's mend (2026-11-24, from the row's independent read). **Every fire is lit and traced now
where main lit the four nearest**, and a fire's reach (71 to 82 m at the flicker's peak) covers a
street, so the grid culls almost nothing between neighbouring fires: N fires within about 70 m are
N shadow rays at every pixel at night where main traced four. Measured on lantern-street with its
lamps replaced by N campfires (250 cd, 1800 K, flicker 0.6, no smoke; the fixture's README says
how the variants are made), at midnight, everything else as above: 3840 × 2160, `--raster rt
--shadows rt`, the owner's settings, `--no-cache` on both sides, frames 240 to 480. Base:
`cc95d783a` (the copy the row measured against); tip: the branch with the mend. RTX 5090,
2026-11-24 02:09 to 02:28, under the GPU lock, two flights of each cell interleaved base and tip;
**machine state**: the harness's warning on every flight, other processes 3 to 100 % of the CPU at
the starts (other agents building), the GPU 0 % busy at every start, free commit 41 to 57 GB, and
one other engine-view process alive from 02:12 on (not on the card during the lock); upper bounds.
Milliseconds, medians (p99) of the resolve, and the frame's median:

| fires | base resolve | tip resolve | base frame | tip frame | what base lit |
|---:|---|---|---:|---:|---|
| 0 | 0.808, 0.805 (0.823) | 0.814, 0.823 (0.836) | 2.58 | 2.60 | — |
| 4 | 1.066, 1.066 (1.084) | 1.078, 1.083 (1.099) | 2.84 | 2.86 | all 4 |
| 8 | 1.069, 1.068 (1.086) | 1.425 (1.444) | 2.86 | 3.21 | the nearest 4 |
| 16 | 1.061, 1.058 (1.074) | 2.176 (2.211) | 2.85 | 3.97 | the nearest 4 |
| 32 | 1.039, 1.061 (1.082) | 3.759, 3.757 (3.802) | 2.84 | 5.59 | the nearest 4 |
| 256 lamps of 400 m and 64 fires (the grid's worst) | refused | 33.03 (34.39) | — | 34.85 | — |

Base at 8 and 16 fires is from the first group (02:09 to 02:16), as are tip's 32 and the worst
case (the tip before the eye rule below, which moved tip's 8 and 16 fires by 0.02 ms); the rest
are the second group (02:23 to 02:28). **What the extra fires cost**: about 0.095 ms of the
resolve a fire past four at 4K (3.76 − 1.08 over 28), its BSDF and its shadow ray at nearly every
pixel. **Past four the base's number is of a picture missing fires**: main lit the four nearest
the eye and left the rest dark, so its flat line is not a cost the tip should meet but a light it
did not draw. The worst case is the 322-ray bound reached: 33 ms of resolve.

**At four fires or fewer the tip was slower, and two things were found.** The first group put tip
at 1.087 and 1.100 against base's 1.047 and 1.062 at four fires (+0.04 ms) while four lamps of 8 m
were 0.03 ms faster: four fires reach every pixel, so the grid culls nothing and its walk (a cell
lookup, then the bits) costs more than the plain loop over the same four. **Mended**: a grid
whose cell at the eye holds every light is not used, and the frame walks the whole array as main
did (`gfx::light_grid_culls_at`, the same picture either way); the second group put the gap at
+0.012 and +0.017. **What is left is the walk's code in the module**: the street with no light at
all is +0.006 and +0.018 the same hour (and the forest pair +0.04, above), the 141 instructions
every resolve variant grew by. Four lamps of 8 m stay faster with the mend (0.818 and 0.824
against 0.854 and 0.856), and 64 (0.959 and 0.973 against 1.709 and 1.702).

**What it costs the processor.** The grid is built in the frame's record on the CPU, in the
`passes` section of a frame record. The first build tested every cell of each light's box against
its sphere: 1.14 ms of the frame's CPU at 64 lamps and 2.56 ms at 256 (base 0.07 ms). Building a
row of cells along x at once — with the row's distance from the light in y and z, the cells the
sphere covers are those its x-interval of half-width √(r² − dy² − dz²) meets, the same test solved
for x — set the same bits for **0.26 ms at 64 lamps and 0.71 ms at 256** (medians; base 0.085 ms;
16 lamps 0.10, 4 lamps 0.06), and the resolve's numbers did not move (64 lamps 0.968 and 0.972 ms,
256 lamps 1.388 and 1.389 ms, the same hour as base's 1.695). That is the memset of the cells, the
bits, and the copy into the slot's buffer; the frame's lights are steady, but their positions are
the eye's frame's, so the grid is rebuilt each frame (a grid kept in the world's cells and only
re-anchored when the eye moves would cost nothing on a still street: a proposed next step).
**The worst build** is every light over the whole box, nothing culled: its bits are the cells
times the lights, about 5,957 cells × 322 lights ≈ 1.9 million at the room's limit. Measured on
lantern-street's worst variant (256 lamps of 400 m and 64 fires, 320 lights each reaching the
whole box; R87's mend, the tip before the eye rule below): **3.06 ms of the frame's CPU** (the
`passes` median), where 32 fires cost 0.19 and none 0.04.

**The picture**: with the bounce's history the last frame of a flight is not the same bytes from
run to run even for one binary (base against base differs), so the pictures were compared in
flights with `renderer.gi.mode=off` and `renderer.reconstruction.accumulation=off`: base and tip
draw **the same bytes** at 16 and at 64 lamps (and base twice the same bytes). The tests hold the
same on one renderer with the bounce's frame (`lamp_tests.cpp`).

**A scene with no lights costs what it cost: the forest staging.** `--no-lights`, standing, the
owner's top setting (3840 × 2160, `--raster rt --shadows rt`, micromaps, the bounce with its guide
and memory, the accumulation), 300 frames, the resolve's median over frames 100 to 299, each side's
first load (a cold compile of the resolve: 401 s for base, 465 s for the tip) not a side of the
pair: **base 9.343 ms (its first load 9.354), tip 9.381 ms (its first load 9.410)**, so +0.04 ms
(0.4 %), inside the row's 0.05 ms; peaks 18.85 and 18.83 GiB of private commit. Machine state: other
agents' builds beside it (the CPU 4 to 29 % busy at the starts), and another engine-view running at
the tip's measured start; an upper bound. **The forest does name a finite light**: the staging's
scene names a campfire of 250 cd, and `--no-lights` sets `renderer.lights` off, which drops the
stand-in's two but not the fires (`record_frame` adds them whatever that setting is;
[forest-resolve](forest-resolve-2026-11-01.md) already says its `--no-lights` array holds the moon
and the campfire). So the tip builds a grid of one light there, of a reach about 82 m at the
flicker's peak, and the +0.04 ms pays for that grid's cells and the walk over them as well as for
the walk's code in the module (every pixel's leading-light loop asked through `light_walk_next`).
The flight did not record `Stats::light_grid_cells`; a frame record of the forest would.

**The modules' size** (`fs_resolve`'s instructions, counted by walking each module's SPIR-V from
`OpFunction` to `OpFunctionEnd`, as [resolve-cold-compile](resolve-cold-compile.md) counts them):
every resolve variant grows by **141 instructions** (2,940 bytes); the forest's
(`visibility_resolve_rt_water_terrain_stochastic`) 326,306 → 326,447. No module crosses
`cmake/shader-costs.txt`'s heavy line either way, so no row of it moves (`build.shader_pool` passes
in the row's `test -Affected`).
## The cap

R213 (2026-11-24, batch 75). The renderer's page has the rule
([renderer](../subsystems/renderer.md#a-cap-on-the-shadow-rays-a-pixel-traces)); this section has what
it was taken from and what it measured.

**What the published methods bound, and what they need.** Ward (1991) is the shape: rank the
sources at a point by what they could add, test them in that order, stop when the untested rest is
under a tolerance of what the tested gave, and add the rest at an estimated visibility rather than
by a coin, so the result is biased and not noisy. Lightcuts (2005) bounds the same way, cluster by
cluster — an untraced group's error is at most its unshadowed contribution, visibility bounded by
1 — and stops at 2% of the total, Weber's fraction. Clustered and tiled shading bound the *lights* a
cell or tile holds (Filament's 32 a froxel, above), not the rays a pixel traces; with every light of
a cell shadowed that bound is the cell's count, which a fire's 71 to 82 m reach makes the frame's.
Stochastic light sampling (ReSTIR, 2020) traces a few rays a pixel by picking lights in proportion
to their contribution and settles the noise by reusing picks across neighbours and frames. **The
engine's rule decides between them**: a picture is judged with no temporal pass behind it, so the
frame has to be right by itself and the same point has to pick the same lights every frame. That
rules out a stochastic pick and leaves Ward's order with Lightcuts' bound.

**What was taken.** Ward's ranking, by each candidate's unshadowed **irradiance** at the point (its
luminance there times `|n·l|`) rather than its BSDF-weighted radiance, because the irradiance does
not move with the eye and the radiance's specular lobe does: a point's traced set is then a function
of the point, its normal and the lights. Ties go to the lower index, as R87's page asked. Ward's
stop, with Lightcuts' 2% as the default share (`renderer.shadow.light_cap_share`), against what the
traced lights were found to deliver, and a hard cap of rays (`renderer.shadow.light_rays`, 8) because
a fragment shader keeps its ranking in registers. Ward's estimate for the rest, taken from the
pixel's own tests instead of the render's history, which a pixel drawn by itself does not have.

**The error of each fill.** With the untraced candidates' unshadowed light U at a pixel and their
true visibilities v_i in [0, 1], the estimate (the traced candidates' visibility weighted by their
irradiance, v̂) moves the pixel by Σ U_i |v̂ − v_i| ≤ U·max(v̂, 1 − v̂); lit unshadowed (fill 1) by
Σ U_i (1 − v_i) ≤ U; left out (fill 2) by Σ U_i v_i ≤ U. All three are bounded by U, and where the
cut stopped before the cap U's irradiance is at most the share of the traced lights' delivered
irradiance. The estimate is exact where every light is seen (v̂ = 1 and every v_i = 1), which is
what the renderer's case holds; it is worst where the traced and the untraced lights' visibilities
differ, which a walled street makes common (below).

**The picture's error, measured**: lantern-street's fire variants at midnight, 3840 × 2160, `--raster
rt --shadows rt`, the bounce and the accumulation off (with them the last frame is not the same
bytes run to run), the tip's binary, frame 300 captured with the cap on against the same flight with
`renderer.shadow.light_rays=0` (every ray traced), counted by the largest channel difference of the
8-bit picture. 3840 × 2160 is 8,294,400 pixels.

| fires | cap | fill | pixels that differ | pixels over 2 of 255 | worst |
|---:|---:|---|---:|---:|---:|
| 8 | 8 | estimate | 26,173 | 0 | 2 |
| 16 | 8 | estimate | 1,191,424 | 4,205 (0.05%) | 7 |
| 16 | 4 | estimate | 2,437,999 | 151,032 (1.8%) | 20 |
| 32 | 8 | estimate | 3,773,181 | 50,717 (0.61%) | 9 |
| 32 | 4 | estimate | 3,779,385 | 782,343 (9.4%) | 18 |
| 32 | 8 | unshadowed | 3,920,602 | 636,697 (7.7%) | 28 |
| 32 | 8 | left out | 500,343 | 13,407 (0.16%) | 10 |

So **8 rays is the default and 4 is not**: at 16 and 32 fires four rays put 2 to 9% of the picture
past 2 of 255 where eight put 0.05 to 0.6%. **The fills are the street's lesson**: the eight
nearest fires are mostly in view of a point and the far ones mostly behind the walls, so the traced
lights' visibility over-states the rest's; lit unshadowed is far worse, and left out is better than
the estimate here — it differs from every ray's picture at 0.5 million pixels where the estimate
differs at 3.8 million, since most of the untraced fires are in fact hidden — and would be worse in
an open square where the far fires are seen. The estimate stays the default because it is exact
where everything is seen and its bound is the tightest of the three; a fill that knows how far a
light is from the traced ones (the walls between are what makes the far fires dark) is a proposed
next step.
