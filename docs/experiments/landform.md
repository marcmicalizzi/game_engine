# Landform: the grounding of the solver (W95, stage A)

2026-11-10. This page backs `domain/landform` ([landform](../subsystems/landform.md)): what the
solver models, where each process and constant comes from, what was read and what was not, the
decisions determinism forced, and the model's limits. The measurements (the bench, the regressions
the tests hold) are at the end.

**The want.** Mountains are the largest subject in the owner's reference reels (116 scenes), and a
range authored as ridge lines with a profile reads as smooth cones: no drainage, no spurs, no
couloirs, near-symmetric peaks. What makes a range read as a range is that it has been **eroded**:
the reels' walls (r03, r04) are cut into ribs and gullies that branch downward, their crests wander
and their summits sit off-centre, and their valley floors are far flatter than their sides. A
heightfield gets that from a drainage network, not from a profile function, so the solver is a
landscape evolution model in the published sense: uplift raises the ground, rivers cut it in
proportion to the water they collect and the slope they run down, and hillslopes shed what the
rivers undercut.

## Status of each source

**Read** means the text was read for this page (through a fetch of the publisher's or the archive's
page); **abstract** means only the abstract; **secondary** means the number is quoted by a source
that was read, or by the forest proof of concept's notes, and the original was not; **not read**
means nothing was taken from it but its name. The proof of concept's research notes
(`landform-v31-grounding.md`) were leads to these publications and are not cited as sources.

| Process | Form used | Source |
|---|---|---|
| Depression filling and routing | Priority-Flood: seed a priority queue with the outlets, pop the lowest, raise each unvisited neighbour to at least the popped height plus ε, push it. Every point then has a path that never rises to an outlet | Barnes, Lehman and Mulla 2014, *Computers & Geosciences* 62:117–127 (arXiv:1511.04463). **Read**: Algorithm 1 (the flood), Algorithm 3 (the ε variant: "requires a total-order queue"), Algorithm 4 (flow directions), and the section on orders: a total order gives "a predictable, reproducible result", a strict weak order does not. Complexity O(n log n) for floating point, O(n) for integers |
| Fluvial incision | The stream-power law, ∂z/∂t = U − K A^m S^n, solved implicitly in O(n) along the flow tree, each point after its receiver | Braun and Willett 2013, *Geomorphology* 180–181:170–179. **Not read** (paywalled). The scheme as described by Cordonnier, Bovy and Braun 2019, *Earth Surf. Dynam.* 7:549 (**read**: Eq. 3, "a single flow receiver", "an implicit scheme of linear complexity", drainage area treated explicitly), and by Barnes 2019 (arXiv:1803.02977, **read**: the steps receivers, donors, order, accumulation, uplift, erosion; Algorithm 6, the backward-Euler update with Newton-Raphson for a general n; cells of one level of the order may run concurrently) |
| Exponents | m = 1/2, n = 1 | Stock and Montgomery 1999, *J. Geophys. Res.* 104(B3):4983–4993, **abstract**: rivers with stable base levels follow K A^(0.3–0.5) S^1. m/n = 0.5 sits inside the 0.4–0.6 "often reported" for the concavity (Lague 2014, *ESPL* 39:38, **secondary**: the PDF would not decode here) |
| Hillslope transport | Linear diffusion, ∂z/∂t = D ∇²z, plus a critical slope no hillslope stands above: the two regimes of the nonlinear law q = K S / (1 − (S/Sc)²), which is linear diffusion at low gradient and whose flux grows without bound as S nears Sc | Roering, Kirchner and Dietrich 1999, *Water Resour. Res.* 35(3):853–870. **Abstract only** (the publisher refused the fetch): the law's form and that it "reduces to linear diffusion at low gradients"; its model runs use K = 0.003 m²/yr (seen in the figure text of an indexed copy); its Oregon Coast Range Sc = 1.25 is **secondary** |
| Flow direction on a plane | A diagonal neighbour's fall is weighed by 1 / (2 − r), r random in [0, 1), in place of 1 / √2 (Rho8) | Fairfield and Leymarie 1991, *Water Resour. Res.* 27(5):709–717. **Abstract only**: its problem ("unless the terrain is rugged, the derived channels tend to run in parallel lines") and that it answers with a random direction. The weight is the formula as the method is usually given and was **not** checked against the paper; what can be checked is its consequence, derived here: on a plane whose fall line stands an angle t off an axis, the diagonal wins where r > 1 − tan t, so a share tan t of the cells step diagonally and the mean direction is the plane's. Here r is hashed from the seed and the cell, so a cell's choice does not change from step to step and the network holds still |
| The slope limit as a gradient | The largest surface under the ground whose gradient is at most the critical slope: the eikonal equation's upwind step, (a + b + √(2f² − (a − b)²)) / 2, swept to convergence | The standard first-order (Godunov) scheme and the fast sweeping method (Zhao 2005, *Math. Comp.* 74:603–627; **not read**: the scheme is written here from its textbook statement). No number comes from it |
| Base level | An outlet is a point held at its level (a Dirichlet boundary); a river cuts no lower than the sea | The convention of every source above (Cordonnier et al. 2019 route to "boundary nodes"); no number comes from it |

## Constants, their ranges, and the defaults the schema pins

| Constant | Default | Range accepted | Status | Source or reason |
|---|---|---|---|---|
| m, n | 1/2, 1 | fixed | range | Stock and Montgomery 1999 (abstract): A^(0.3–0.5) S^1. Fixed because of determinism, below |
| Erodibility K | 5 × 10⁻⁶ yr⁻¹ | 10⁻⁸ to 10⁻² | authoring inside a range | Stock and Montgomery 1999 (abstract), for m = 0.4 in m^0.2/yr: granite and metamorphic rock 10⁻⁷–10⁻⁶, volcaniclastic rock 10⁻⁵–10⁻⁴, mudstone 10⁻⁴–10⁻². At m = 1/2 the unit is yr⁻¹ and a value converts by A^−0.1 (a fifth at 10 km²), so the default is a weak volcaniclastic or a strong sedimentary rock. What it fixes with the uplift is the steepness: at steady state S = (U/K) A^−1/2, so 1 mm/yr over the default is a steepness of 200 m |
| Hillslope diffusivity D | 0.005 m²/yr | 0 to 1 | range | Roering et al. 1999's runs, 0.003 (figure text); 10⁻³–10⁻² as the span of field values in a lecture summary of the literature (MIT OCW 12.163, **secondary**) |
| Critical slope Sc | 0.9 (42°) | 0.2 to 4 | secondary | Inside the 0.8–1.25 the proof of concept's notes quote for soil-mantled ridges, Roering's 1.25 at its top. Not re-read; a number to check against DiBiase et al. 2012 before a world pins it |
| Critical slope of the most resistant rock | 1.6 (58°) | Sc to 4 | authoring | Bare rock stands steeper than a soil-mantled slope; no source gives the figure |
| Resistance contrast | 10 | 1 to 1000 | authoring | A bed's `resistance` is the world formation's 0-to-1 ordering ([formation field](formation-field.md)); K falls from the sketch's value at 0 to a tenth of it at 1. Stock and Montgomery's lithologies span five orders, so ten between the beds of one range is modest |
| Seed roughness | 1 m | 0 to 100 | authoring | What breaks the symmetry of the starting plane so a network can form; it is gone from the result after the first few steps |
| Erodibility variation | ±25% over 16 cells | 0 to 0.9 | authoring | Rock is not uniform; without it a planar face at the critical slope sheds water along the lattice and its rills run parallel (the proof of concept's "couloir streaks") |
| Hack's exponent (a check, not an input) | — | 0.5 to 0.7 | secondary | Hack 1957 found 0.6; Gray 1961, 0.568; "empirically found to lie in the range from 0.5 to 0.7" (search extracts of Rigon et al. 1996 and its citers; none read in full) |

## What determinism decided

Every output byte has to be the same from MSVC, GCC and Clang, at either CPU baseline and at any
thread count. That decided four things.

1. **n = 1 and m = 1/2, fixed in stage A.** With n = 1 the backward-Euler update of a point is one
   division, z = (z + F z_r) / (1 + F), F = K Δt A^m / d: Barnes 2019's Newton form (Algorithm 6)
   reaches it in one exact step. With m = 1/2, A^m is one correctly rounded `sqrt`, and since
   A = N cells of spacing² and d is the spacing or its diagonal, F = K Δt √N (or that over √2): the
   spacing cancels and nothing but IEEE `* / +` and `sqrt` is in the incision. **A general m** costs
   `det::exp(m * det::log(A))` a point a step: `core/math/deterministic.h` has both, so it is
   covered and bit-identical across toolchains, at forty times the time of a square root:
   69 ns against 1.7 ns a point (the bench's `landform.area_power` rows, below), which would add
   a fifth to a step. **A general n** needs Newton's iteration, each
   round another such power of the height difference, and a convergence test whose count of rounds
   is itself a decision; `deterministic.h` covers the arithmetic, and it is what follows, not stage
   A, because field data often want n near 2 (Lague 2014, secondary) and that deserves its own tests.
2. **The flood runs on integers.** Heights are f64 metres between steps, and each step's flood
   works on the surface quantized to 1/1024 mm in a 64-bit key, the height above the index, so one
   integer comparison is the total order Barnes asks for and a tie in height goes to the lower cell
   index. ε is one unit, which is an exact increment and not `nextafter`.
3. **No C library in a decision.** `sqrt`, `floor` and IEEE arithmetic only; the ridge kernel and
   the noise are polynomials and integer hashes; contraction is off tree-wide (ADR-0035).
4. **Parallel only where the result cannot see it.** The uplift field, the quantization, the
   receivers (each point reads the filled surface, writes its own), the diffusion (a Jacobi step:
   read the old surface, write the new) and the output fields run on the job system. The flood, the
   accumulation, the incision and the slope limiter walk an order and are serial. Barnes 2019's
   levels of the order would run the last three in parallel with the same bytes; that follows.

## The model

A step of Δt years, over a lattice of points `spacing_mm` apart:

1. **Uplift.** z += U Δt at every point that is not held. U is the sketch's regional rate plus its
   ridges (the largest of the ridge kernels at the point, (1 − (r/reach)²)² of the line's rate) or
   its coarse raster, times the border taper (below).
2. **Flood and route.** Priority-Flood with ε from every held point; the receiver of a point is its
   steepest neighbour of eight on the filled surface, which always exists, so **every point drains
   to a held point**. The pop order is ascending in filled height, so it is also the order of the
   flow tree: a receiver is always popped before its donors.
3. **Drainage area.** In reverse pop order, a point adds its count of cells to its receiver's.
4. **Incision.** In pop order, a point above its receiver (and above the sea) moves to
   (z + F z_r) / (1 + F). A point in a depression is not cut (its receiver on the filled surface is
   not below it); its outlet is, at the whole basin's area, so a depression drains by its sill
   being cut, as a lake's does. A receiver below the sea counts as the sea's level: a river cuts to
   the sea and no lower.
5. **Hillslopes.** Explicit linear diffusion in as many substeps as stability needs
   (D Δt / spacing² at most 0.2 a substep), then the slope limiter, taken in pop order: a point
   is lowered to the largest height that keeps the gradient's length at the critical slope (the
   eikonal step from its lower neighbour along x and along z, and the same bound along each
   diagonal), so a slope a river has undercut fails back to the critical slope. What it removes
   leaves the model: there is no deposition in stage A.
6. After the last step the limiter runs to convergence (sweeps in four directions until nothing
   moves), so **no slope in the output exceeds its point's critical slope**, land or sea bed.

**What the first reliefs changed** (2026-11-10, a 384-point range at 32 m). The first limiter
bounded the slope to each of the eight neighbours, and the receivers were the plain steepest of
eight. The relief was facets: every failed slope an eight-sided pyramid creased along the lattice,
and on gentle ground rills in parallel along the diagonals, which is the proof of concept's
"couloir streaks" again. The eikonal step made the failed slopes cones, and Rho8's weight took the
rills off the lattice. The same run also showed how much of a range is planar when U/K is large
against the spacing ([landform](../subsystems/landform.md#the-sketch)): at a steepness of 400 m
on 32 m points nearly everything was at the critical slope.

**Resistant rock stands.** A point's resistance (uniform, or a table of bands by elevation, each a
0-to-1 resistance) divides its erodibility by up to the contrast and raises its critical slope
toward the resistant rock's. A band is read at the surface's present height: flat-lying beds in a
range that is being held up as fast as it is cut. Beds that dip and fold are the formation field's
(W85), and reading it here is what follows.

**The borders take part.** The proof of concept's two worst landform defects were a ramp switched
on in one row at a region's edge and a fixed sea bed met as a 370 m underwater wall. Here:

- An **open** edge's points are held at their base level and are the outlets. Uplift is tapered to
  nothing toward every open or sea edge by a smoothstep over `border_reach_m`, so the ground meets
  the edge at the edge's level **and with no slope**: beyond the heightfield the tile set holds the
  edge flat ([tileset](../subsystems/tileset.md#the-far-mip-chain-and-its-filter)), and the solved
  ground joins that plain without a step.
- A **closed** edge is a wall nothing crosses: a divide another region shares.
- The **sea** is a level, and its bed is ground like any other: it is in the lattice, it is routed
  across to the sea edge's held points, it diffuses and it is held to the critical slope, so a
  land surface cut or raised beside it cannot leave a wall. Only the stream-power term stops at
  the waterline. A starting heightfield that has such a wall loses it.

**A starting heightfield is refined, not replaced.** With one, the surface starts as that ground
(sampled bilinearly, so a coarse solve can seed a fine one) and held points keep its heights; a
short run cuts drainage into it and the test holds the change of its low-pass under a bound.

## Why the proof of concept's pictures did not beat the version before, and what follows from it

Its hand-off says the slopes and drainage measured right and the pictures did not improve. From its
text, five causes, and what this design does about each:

| Cause | Here |
|---|---|
| Summits capped by the strip's width: a summit cannot stand higher than the critical slope times its distance to base level, and valleys dense enough to read as alpine put base level within 500–800 m of every summit | A solver cannot change this; the page says it so a sketch's author sees it. Relief comes from distance to base level: the summary the command prints carries the highest point and the mean slope, and a range that must be taller needs a wider region or a higher base level, never a steeper Sc |
| Basins filled flat (47 grey patches in its relief) | The filled surface is never written: it routes water and is kept as the `fill` field, for a later solver to make tarns of |
| Lattice-aligned streaks on planar faces | The diagonal's weight is hashed for each cell (Rho8), so the mean direction of flow on a plane is the plane's, and the erodibility varies. One receiver a point remains; see the limits |
| One smooth tower where uplift was a point with a bonus | Uplift is lines with a reach or a raster, and resistance is a band table, so a ridge is long and its crest is cut by the heads of the valleys on both sides |
| Smooth eroded slopes with a pale tiled material read as dunes; jagged rock is not a heightfield's to hold | Not a landform solver's: rock that reads as rock is the cliffs' and the terrain material's (G131, G132, R105). The fields are written so those solvers can find the steep, the wet and the fast-eroding ground |

## Limits of the model

- **Detachment-limited, with no deposition.** Nothing is laid down: no flat alluvial valley floors
  (the reels' r03 and r04 both stand on one), no fans, terraces, deltas or scree aprons. Valleys
  are V-shaped to their mouths.
- **No ice.** No U-shaped troughs, cirques, hanging valleys or overdeepened basins.
- **One receiver a point, one of eight neighbours.** Flow never divides (no braiding, no fan),
  and a single cell's choice is still a lattice direction: the hashed weight makes the mean
  direction right, and at the lattice's scale the ground is as rough as that choice. Divides
  cannot migrate by less than a cell.
- **Area for discharge, uniform rain.** No orographic precipitation, no thresholds for incision.
- **First order in time, drainage area explicit**: a large step is stable and smears a transient;
  steady state is exact (the slope-area test holds it to the fourth decimal).
- **Resistance by elevation band**, not by a bed that rises with the rock.
- **A heightfield**: no overhang, no pinnacle, no two-sided crest.

## What follows stage A

The glacial mode (an equilibrium-line altitude, ice flux, troughs and cirques); deposition
(terraces, fans, deltas, scree zones, and with it a flat valley floor); soil depth; snow where
erosion and wind put it; reading the formation field's dipping beds for resistance; a general m and
n; the order's levels in parallel and a flood that skips ground with no depressions; baking
far-terrain maps from near output; anything in the editor.

## A request to the channels generator

`channels` today routes its own lattice over the ground and begins a channel where its own D8
area and a local 4 m slope say so. With a landform beside the heightfield it should read
`drainage_area` (and `receiver` for the path) for the window its region covers, so a channel's head
and course are the ones the ground was cut by. The contract it would read is on the subsystem page
("The field file"). A request, not an edit: `domain/channels` is another row's.

## Measurements

2026-11-10. The bench's table and the machine's state are on the subsystem page
([Performance notes](../subsystems/landform.md#performance-notes)); the regressions are the tests'
(`domain/landform/tests/landform_tests.cpp`, which prints them).

| Measure | Result | Against |
|---|---|---|
| Concavity at steady state (96 points at 200 m, 1 mm/yr, K 2 × 10⁻⁵, two million years, stream power alone) | slope against area regresses to −0.4999 | the 0.5 chosen; published 0.4 to 0.6 |
| The same run's mean erosion rate | 1,001.1 µm/yr; 97% of points within 10% of the uplift, 80% within 1% | 1,000 µm/yr. The rest sit on divides whose receivers change from step to step: the explicit drainage area |
| Hack's exponent, the longest path above a point against its area, over the 1,535 points draining eight cells or more | 0.616 | Hack 1957, 0.6; published 0.5 to 0.7 |
| An open edge with the default taper (16 cells), a ridge of 3 mm/yr 3 km away, 100 m cells | the first row 1.45 m above the edge; the slope changes by at most 0.11 a row over the first four | without the taper 89 m and 1.14 |
| A coast (the golden sketch with one critical slope, 0.9) | no neighbours steeper than 0.9 by more than 0.8 mm, land, waterline or bed | the rounding to whole millimetres |
| A starting ground with a 370 m underwater wall, fifty thousand years | the largest step between neighbours 89,978 mm over a 100 m cell | 370,000 mm before; the critical slope allows 90,000 |
| A 407 m massif at 50 m refined for forty thousand years at the default erodibility, nothing raised | its 800 m low-pass moves at most 9.5 m (2.3% of the relief); single points move 29.7 m | the bound held is a twentieth of the relief |
| A 2,048-point sketch at 16 m to 100,000 years in twenty steps, four workers, busy machine | 27.1 s | — |

What the first pictures showed, and what still reads wrong beside the reels, is on the subsystem
page ([Pictures](../subsystems/landform.md#pictures-and-what-still-reads-wrong)).
