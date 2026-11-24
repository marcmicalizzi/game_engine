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
| Olsson, Billeter and Assarsson, "Clustered Deferred and Forward Shading", HPG 2012 | **The paper not read** (the preprint's fetch failed); its abstract and the Chalmers listing through a search | Grouping view samples by their 3D position (screen tile × depth slice) maps lights to samples better than tiles alone, whose 2D groups take every light along a tile's depth range; per-cluster back-face culling of lights by normal is an option. |
| Harada, McKee and Yang, "Forward+: Bringing Deferred Lighting to the Next Level", Eurographics 2012 (short) | The authors' project page (takahiroharada.github.io/forward+); **the paper not read** | Screen tiles of about 16 × 16 pixels after a depth prepass; lights culled per tile on the GPU either by a group per tile testing every light (gather) or a thread per light writing light-tile pairs that are then sorted (scatter); the shading pass reads the tile's list. |
| Filament's documentation, "Physically based rendering in Filament" (google.github.io/filament), sections on punctual lights and clustered forward rendering | Read through the fetch tool, whole | The punctual light's falloff windowed to zero at a radius of influence, E = I / max(d², 0.01²) · ⟨1 − d⁴/r⁴⟩², credited to Karis 2013: "window the inverse square function in such a way that the majority of the light's influence remains unaffected". Lights assigned to **froxels**: 80 × 80-pixel tiles by 16 exponential depth slices (the first slice set by hand, 0.1 to 5 m), at most 32 lights a froxel in an index list ended by a sentinel, assigned on the GPU by compute, or **on the CPU by rasterizing each light into the froxels it touches**, which the text says culls more tightly. |
| Karis, "Real Shading in Unreal Engine 4", SIGGRAPH 2013 course notes | **Not read** (the fetched PDF had no readable text); its falloff through Filament's citation above | The windowed inverse square the engine already uses. |
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
frame can pay for; lantern-street's densest pixel is reached by under 20 lamps.

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

**The picture**: with the bounce's history the last frame of a flight is not the same bytes from
run to run even for one binary (base against base differs), so the pictures were compared in
flights with `renderer.gi.mode=off` and `renderer.reconstruction.accumulation=off`: base and tip
draw **the same bytes** at 16 and at 64 lamps (and base twice the same bytes). The tests hold the
same on one renderer with the bounce's frame (`lamp_tests.cpp`).
