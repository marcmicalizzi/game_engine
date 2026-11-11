# landform (domain, capability)

**Purpose.** An offline landform solver (roadmap W95, stages A and B of G36's erosion and
hydrology): a **region sketch** — the lattice, what each edge is, outlets, uplift, how hard the
rock is, a seed, the time and the step, and optionally a ground to start from — becomes an
**eroded ground**: an authored heightfield the tile set builds as it is
([tileset](tileset.md#the-source-engineterrainheightfield1)), in whole millimetres, and on the
same lattice **the fields the later solvers read** (drainage area, receiver, slope, erosion rate,
fill, sediment). Rivers cut and lay down ([deposition](#deposition)), so a valley that is dammed
or whose base level rose has a floor, a plain at a range's foot takes its fans, and a lake or the
sea fills from its rivers' mouths. Mountains are the largest subject in the owner's
reference reels, and a range authored as ridge lines with a profile reads as smooth cones; what
makes a range read as one is that it has been eroded, so the solver is a landscape evolution model
in the published sense. The sources, every constant's range and what was read of each are on the
grounding page, [experiments/landform.md](../experiments/landform.md).

**Why a capability, and why it registers nothing.** It is data and arithmetic with no GPU, no
generator and no tick, so it lives in `domain/` and is removable ([ADR-0027](../adr/0027-additive-capabilities.md)):
`ENGINE_WITH_LANDFORM=OFF` and the minimal build take it out, and `engine-content landform` then
says the capability is absent. Its output is content built ahead, like a tile set; a host calls
`landform::solve`, and nothing in the engine asks whether the capability is there.

**It does not depend on the tile set.** A capability may not depend on another, so the solver
writes and reads the heightfield's header by its documented shape (`engine.landform.TerrainHeightfield`
is `engine.tileset.HeightfieldHeader` field for field). What holds the two together is the host:
`engine-content landform` reads every heightfield it writes back through `tileset::read_heightfield`
when the build has the tile set, and refuses its own output if the tile set does.

## The sketch

`engine.landform.Sketch` (`domain/landform/schemas/landform.schema`), a JSON file:

| Field | What | Range, default |
|---|---|---|
| `width`, `height`, `spacing_mm`, `origin_mm` | The lattice: points a row and rows, the spacing, and point (0, 0) in the world, a whole number of spacings, so the lattice is a window of the world's at its spacing, as a heightfield's is | 8 to 16,384 points; 100 mm to 1 km; default 16 m |
| `edges` | Each side `Open` (held at the base level; rivers leave), `Closed` (nothing crosses) or `Sea` (held at the sea bed's depth). West is −x, north −z | all `Open` |
| `base_level_m` | What an open edge is held at. Absent with a starting heightfield: its own heights there | −11,000 to 9,000 m |
| `sea` | `level_m`, and for a run with no starting ground `shelf_m` and `depth_m`: the bed falls from the level a shelf's width in from a sea edge to the depth at the edge | required by a sea edge |
| `outlets` | Points held at a level anywhere in the lattice | — |
| `border_reach_m` | Uplift is tapered to nothing over this distance from every open or sea edge | 16 cells |
| `uplift` | `regional_mm_per_yr`, plus `ridges` (a polyline, `rate_mm_per_yr` on it, varied along the line by `variation` over `variation_length_m` ([uplift along a ridge](#uplift-along-a-ridge)), falling to nothing `reach_m` away as (1 − (r/reach)²)²; the largest of overlapping ridges counts), plus a coarse `raster` read bilinearly over the region; and the whole made uneven over the plan by `variation` over `variation_length_m` ([valleys that are not a comb](#valleys-that-are-not-a-comb)) | 0 to 100 mm/yr; a ridge's variation 0.35 over 6 km; the plan's 0.7 over 3 km |
| `rock` | `resistance` 0 to 1 (the world formation's ordering), uniform, or the beds of `formation`, the scene's own `engine.scene.Formation`, read in the rock's frame ([structure](#structure)); `contrast`, the erodibility at 0 over the erodibility at 1; `variation` and `variation_cells`, a seeded variation of the erodibility | 0; no formation; 2 (stage C: inside the published 1.2 to 2.8 for a range's own rocks; it was 10, which solved a banded sketch to a land of cliffs); ±25% over 16 cells |
| `erodibility_per_yr` | K of the stream-power law at m = 1/2, n = 1 | 10⁻⁸ to 10⁻², default 5 × 10⁻⁶ |
| `channel_area_m2` | The drainage area a channel begins at: a point draining less is hillslope, lowered by diffusion and the critical slope alone | 0 to 10⁹; default 0, the rivers cut at every point |
| `deposition` | G, the deposition coefficient ([deposition](#deposition)): a river lays down G / N of the sediment that reaches a point draining N cells. 0 is the detachment-limited law: nothing settles anywhere, standing water included | 0 to 10, default 1 |
| `sediment_erodibility` | The erodibility of sediment over `erodibility_per_yr` | 1 to 100, default 2 |
| `delta_share` | The share of what reaches a lake's water that settles at the river's mouth, up to the water's level; the rest levels the lake's floor | 0 to 1, default 0.5 |
| `divide_below_slope` | Where the steepest fall from a point is under this its flow divides among all its lower neighbours ([flow that divides](#flow-that-divides)); at this and above it all goes to the steepest. 0 never divides | 0 to 1, default 0.1 |
| `lateral_erosion` | The erodibility of a river's bank over its bed's ([lateral erosion](#lateral-erosion)). 0 is none | 0 to 10, default 1 |
| `channel_width_m` | A channel's width where it drains one square kilometre; the `channel_width` field is this times the square root of the discharge in square kilometres | 0.1 to 1,000 m, default 5 |
| `ice` | An ice age, or none ([ice](#ice)): `ela_m`, the equilibrium line; `full_m` and `melt_m`, the height above it at which all snowfall becomes ice and the depth below it at which a glacier loses as much; `erodibility`, the ice's over the rivers'; `width_m` and `thickness_ratio`, a glacier's width at a square kilometre's flux and its thickness over its width; `coupling` and `adverse_slope`, the reach down a glacier's line over which its erosion reads the ice's surface (in coupling lengths of one thickness each way) and the bed's adverse slope over the surface's at which it stops; `from_yr` and `until_yr`, the years of the run it stands in | none; 500 m and 250 m; 2; 500 m and 0.25; 2 and 1.5; from 0 to the run's end |
| `diffusivity_m2_per_yr` | Hillslope diffusivity | 0 to 1, default 0.005 |
| `critical_slope`, `critical_slope_resistant` | The gradient no slope stands above, for rock of resistance 0 and 1 | 0.2 to 4; 0.8 and 1.6 |
| `seed`, `roughness_m` | The seed of the starting roughness, the erodibility's variation and the routing's diagonal weights | 1; 1 m |
| `duration_yr`, `step_yr` | Whole years; the run is `duration_yr / step_yr` steps. No steps writes the starting ground's own fields | 1,000,000; 5,000 |
| `start` | An `engine.terrain.heightfield/1` header beside the sketch: the ground the run starts from and refines. It must cover the lattice and may be coarser (it is read bilinearly) | — |

A sketch outside a range is refused by a sentence naming the field.

**What the uplift and the erodibility decide together.** At steady state a channel's slope is
(U/K) A^−1/2, so U/K, a length, is the range's steepness: 1 mm/yr over 2 × 10⁻⁵ is 50 m, over the
default 200 m. Where that slope would exceed the critical slope — every point draining less than
(U / (K Sc))² square metres — the ground is a threshold hillslope, a plane at the critical slope
between the gullies that are large enough to hold their own. So **U/K over the spacing says how
much of a range is planar**: a steepness of 400 m on a 32 m lattice leaves hillslopes 190 cells in
area with nothing on them, and reads as facets; 100 m on 16 m leaves hillslopes 48 cells in area,
and the walls are cut into ribs. And **relief is distance**: a summit stands no higher than the
critical slope times its distance to base level, so a taller range is a wider region or a higher
base level, never a steeper `critical_slope` (the proof of concept's hand-off found this the hard
way).

**Why the channel threshold is off by default.** Field channel heads drain a thousand to tens of
thousands of square metres, and a model that stops the rivers below such an area is common. Tried
on a 16 km range at 16 m (2026-11-10), 2,000 m² left every slope at the critical angle a smooth
plane between long straight valleys: less dissected, and nearer the cones this solver exists to
replace. With the rivers cutting at every point the same slopes carry rills a cell or two wide,
which is the lattice's scale and no finer, and reads as gullied. The knob is there for ground that
should be smooth (soil-mantled fells).

## The step

Each step of `step_yr` years, in this order ([the grounding page](../experiments/landform.md#the-model) has the sources):

1. **Uplift**: every point that is not held rises by its rate times the step.
2. **The flood** (Priority-Flood with ε, Barnes, Lehman and Mulla 2014) from every held point, on
   the surface quantized to 1/1024 mm. Its queue holds one 64-bit key a point, the height above
   the cell index, so **a tie in height goes to the lower cell index**. ε is one unit.
3. **Receivers**: each point's steepest lower neighbour of eight on the filled surface, which
   always exists, so every point drains to a held point. A diagonal's fall is weighed by
   1 / (2 − r), r a number in [0, 1) hashed from the seed and the cell (Fairfield and Leymarie
   1991's Rho8, the number fixed to the cell): on a plane the flow's mean direction is then the
   plane's own, where the plain steepest-of-eight sends every cell down the same lattice
   direction and cuts parallel rills.
4. **Discharge**: in reverse flood order, a point adds its cells to its receivers' by its shares
   of its flow among them.
5. **The rivers**: erosion and deposition together, implicit over the flow network
   ([deposition](#deposition), [the sweeps](#the-rivers-sweeps)): a reach above its base level
   and above the sea moves to (z + d + F b) / (1 + F), F = K Δt times its stream power in cells
   (√N over the distance to its receiver), b its receiver's height, d what the river lays down
   there. A receiver below the sea counts as the sea's level. Standing water (a lake's members,
   the sea) is not cut; it takes the sediment that reaches it.
6. **Hillslopes**: explicit linear diffusion in as many substeps as stability needs, then the
   slope limiter in flood order. What a slope sheds is the next step's rivers' to carry from
   that point.

After the last step the limiter runs to convergence, the ground is rounded to whole millimetres
once, and **the fields are routed again from exactly those millimetres**, so what the field file
says is true of the heightfield beside it and not of the step before.

### Deposition

Stage A's rivers only cut, so every valley was a V to its mouth. Stage B is the erosion and
deposition form of the same law (Davy and Lague 2009), in the form and with the implicit solution
of Yuan, Braun, Guerit, Rouby and Cordonnier 2019 ([the grounding](../experiments/landform.md#deposition-the-sources)):

    ∂z/∂t = U − K √A S + (G / A) Q_s

Q_s is the sediment flux reaching the point: everything the points above it lost and did not lay
down again. G, the sketch's `deposition`, is dimensionless (Davy and Lague's Θ: a settling
velocity over the runoff). At G = 0 nothing settles and the law is stage A's. At steady state a
river stands (1 + G) times as steep and no more concave, which the test holds.

- **One solve for both.** The deposit depends on the flux, the flux on the heights above, and
  the heights on the receiver below, so the step is solved by sweeps (Yuan's equation 20): the
  flux of the last iterate, each point after its donors; then the heights, each point after its
  receiver, with that deposit in the numerator. It ends when no height moves by a millimetre
  (`k_river_tolerance_m`, what the heights round to), or after 32 sweeps; a run counts its
  sweeps (`Landform::river_sweeps`, in the command's summary), eight a step in a range with a
  plain at its foot. **Why not the cut first and the
  deposit after**, which is one pass each and was tried first: a deposit laid on top of a cut
  that has already been made stands a step's worth of sediment (metres) above the channel's
  grade, the next flood routes round it, and a steady range measured 2.4 times as steep in its
  large rivers and 1.9 in its small, where the law says 2 in both. Solved together the bed
  never leaves its grade and the factor is 1.91 to 2.00.
- **The discrete form is Yuan's**: a point lays down G / N of the flux from above (N its
  drainage area in cells, itself included; its own cut is not in the flux), and never more than
  reached it.
- **Sediment is a layer.** Each point keeps the thickness of what was laid on it and nothing has
  removed since. A river cuts sediment first, at `sediment_erodibility` times the erodibility
  (Yuan's runs use 1, 2 and 4), and where it goes through into rock the height is the one at
  which the time to strip the layer and the time to cut the rock make the step, each at the rate
  the final slope gives: one division, and the plain formula again when the two erode alike.
  Creep takes from the layer first and adds to it; a slope that fails loses its layer first. A
  point that loses more than it gains in a step gets no layer from what it gained: the two went
  on together, and the rock was never covered.
- **A lake** is the points the flood raised by a centimetre or more, grouped by the point their
  depression spills over. It is not cut. Of what reaches its water, `delta_share` settles at the
  mouth, on the point and its lower neighbours, the lowest first, up to the sill's level: a
  platform at the water's level with a front the slope limiter holds to the critical slope, a
  delta. The rest, and what the mouth had no room for, is laid from the lake's deepest point up
  to one level, so **a lake's floor is level** however the flood happened to cross it, and
  stage A's straight lines in a dammed valley's floor are gone. A lake that is full passes the
  rest to its sill.
- **The sea** takes everything that reaches it, the same way and up to its level; what a point
  has no room for goes on along the bed. A point brought to the sea's level is land the next
  step, so a delta's plain rises above the water by the river's own deposit.
- **Toward an edge that drains G is tapered** with the uplift, over `border_reach_m`. Without it
  a small river meets an open edge on its own fan (the border test's first row stood 17.4 m up
  over a 100 m cell in the first build, where its bound is 2 m), and the ground no longer meets
  the tile set's flat plain beyond the edge.
- **The budget.** `Landform::budget` (and the command's summary) says where the material went,
  in cubic metres: eroded = deposited + left + unrouted, and change = uplifted − left −
  crept_out − settled − unrouted. `left` went out through the held points by river; `crept_out`
  across them by creep; `settled` is what the settling after the last step removed and
  `unrouted` what the last step's slopes shed, both of which have no later step to carry them
  and leave the model (1.2% of what was eroded in the golden sketch).

**What it makes, and what it does not.** A valley whose mouth is dammed, or whose trunk has
aggraded, fills to a floor that is level across and widens downstream (the test: 180 m wide
where the fill begins and 335 m at the mouth, within 7.6 m across). A range's rivers lay a plain
of coalesced fans at its foot, each river crossing it on its own grade, as Yuan et al. report
for this model; a single massif beside a plain leaves a fan whose contours bow away from it
(the test). **A fan was not a cone at the scale of one mouth** while flow had one receiver a
point; since stage C it divides on the fan and the cone is there
([flow that divides](#flow-that-divides)). **Small rivers lay their load down within a few points** (G / N is large where N
is small), so the foot of every slope takes a little; that is colluvium, not scree: nothing
here stands at an angle of repose. And **a valley inside a range that is rising everywhere has
no floor of sediment at steady state**, because every point of it is losing ground; a floor of
sediment needs a base level that rose or an uplift that stopped, which a sketch gives by where it
puts its uplift and its outlets. A floor cut in rock it now has, where its river is large enough
to wear its banks ([lateral erosion](#lateral-erosion)).

### Flow that divides

Stage B's flow had one receiver a point, so a river's water and its load crossed a plain as one
thread a point wide: a fan was whatever many steps of that thread's wandering averaged to (a
dome), and a river on its own plain was a line. Stage C divides the flow where the ground is
gentle ([the grounding](../experiments/landform.md#flow-that-divides-the-sources)):

- **The rule** is the multiple-flow-direction rule of Quinn, Beven, Chevallier and Planchon
  1991: a point's flow goes to every lower neighbour in proportion to the slope toward it times
  the contour length it crosses (half a spacing toward a cardinal neighbour, √2/4 toward a
  diagonal). Over the two distances that is the fall times two and the fall times one, whole
  numbers of the filled surface's units.
- **Only where the ground is gentle.** Qin, Zhu, Pei, Li, Zhou and Yang 2007 make the rule's
  exponent rise with the slope, 1.1 on a plain to 10 on a slope of 1, because flow on steep
  ground does converge. Here the choice is two-valued so that the powers are none: where the
  steepest fall is under `divide_below_slope` (0.1, where Qin's exponent passes 2) the flow
  divides at Quinn's exponent of 1; at 0.1 and above it all goes to the steepest neighbour, as
  before (Rho8). A river in a valley a point wide has one lower neighbour and is unchanged; on
  a fan, a floor or a plain it spreads. A gully on a slope at the critical angle is still cut
  by one thread, which is what cuts it.
- **Shares are whole.** A point's shares are sixteen-bit parts of 32,768 (`k_share_one`), the
  receiver taking what the divisions left, and the discharge is counted in 1/32,768 of a cell
  in 64 bits, each receiver getting the whole-number part of its share and the receiver the
  rest. So **a divided flow is conserved exactly**: the held points' discharges sum to the
  lattice, an integer identity the test holds.
- **The rivers' law with several receivers** is FastScape's (Yuan et al. 2019's implementation):
  each receiver's term is K Δt √(the discharge that goes to it) over the distance to it, the
  reach moves to (z + d + Σ F_k z_k) / (1 + Σ F_k), and the sediment a point passes on goes to
  its receivers by the same shares. One receiver is the single-flow law, bit for bit.
- **What reads it.** `discharge` in the field file is this divided discharge, in whole cells,
  and `channel_width` a width from it; `drainage_area` and `receiver` stay the one thread down
  each point's steepest neighbour, which is what a solver that wants a channel's course reads.
  `route_divided` is the public form for a ground the solver did not make.

**What it makes.** One mouth's fan is a cone: on an arc 500 m from a single mouth the ground
varies by a small part of its height above the plain, where one thread left lobes (the test has
the two numbers). A river on a floor it has widened is as many points wide as the floor. **What
it costs**: a divided network has more levels and its sweeps converge more slowly, 13 a step
where the single thread took 8 on the same ground (4,013 against 2,402 over 300 steps of the
512 by 512 sketch), because a plain's points now read several receivers each.

### Lateral erosion

A river that only cuts down has no floor: stage B's valleys were a V to the thread, and a floor
needed a base level that rose. Rivers widen their valleys by wearing the bank on the outside of
each bend, and stage C does it by Langston and Tucker 2018's model, as their own implementation
(Landlab's `LateralEroder`) states it
([the grounding](../experiments/landform.md#lateral-erosion-the-sources) says what was read of
the paper and what of the code):

- **The rate** at which a reach wears its bank is K_l A S / R: A its drainage area (here its
  discharge), S its slope, K_l the bank's erodibility (the sketch's K times `lateral_erosion`,
  times the sediment's factor where the bank is sediment and the rock's resistance where it is
  rock), and 1 / R the bend's curvature from the direction the most water arrives by and the
  direction it leaves by: 0.23, 0.67 and 1.37 over the spacing for a straight reach, a bend of
  45 or 135 degrees, and a right angle.
- **The bank** is the neighbour beside the reach, square to the way it leaves, on the outside of
  the bend; on a straight reach one side or the other by a number hashed from the seed, the step
  and the cell (the implementation draws a random one). From a diagonal reach it is the cardinal
  neighbour on that side, which touches the receiver too.
- **The volume** worn in a step, the rate over the water's depth (0.4 Q^0.35, Q the area times
  4 × 10⁻⁶ m/s: the implementation's constants) and one spacing, is kept at the bank's point
  from step to step. When it reaches what stands there above the reach's receiver, the point is
  lowered to the receiver's height. **What has to be worn** is the two published bank models
  joined by what the bank is made of: all of its rock (their "total block erosion"), and of its
  sediment only what the water reaches (their "undercutting-slump": loose ground falls in when
  it is undercut).
- **What it lost** is the next step's rivers' to carry from that point, like what a slope sheds,
  so the budget closes. The slope limiter then brings the wall above the new floor down behind
  it, which is where most of the volume comes from.
- One pass a step in cell order, on one thread, over the reaches that drain 64 cells or more
  (below that the rate, which goes with the area, would not take a bank in a run).

**What it makes.** A floor more than a point wide wherever a river is large enough and its
valley old enough; once the floor is wider than a point the flow divides over it, each thread
carries less, and the widening slows by itself. **Terraces**: when a river with a floor cuts
down again (a base level that fell, an uplift that rose), one thread takes the steepest way and
incises, the rest of the floor is left above it, and the scarp between them stands at the edge
of what the river has worn sideways since, at the critical slope (the test: a base level
dropped 20 m). They are strath terraces: a run that starts from a ground starts with no
sediment layer, so what is left is cut rock.

### Valleys that are not a comb

**The count** (2026-11-10, the two-range sketch at 512 by 512 and 32 m, 300 steps; of the
points draining a quarter of a square kilometre or more down a slope of 0.03 or more, so a
plain's channels are not counted: the junctions a kilometre of channel, the share of points
running due north or south, which is straight down these ranges, and the entropy of the eight
directions, 3 bits at most):

| Run | Junctions / km | Straight down the range | Direction entropy |
|---|---|---|---|
| stage B (and stage C with everything below off) | 0.20 | 44% | 2.77 bits |
| a ridge's rate varied along its line by 0.9 over 3 km (stage B's own lever, turned up) | 0.26 | 41% | 2.85 |
| a start rough by ±100 m, white | 0.20 | 62% | 2.48 |
| flow that divides, alone | 0.14 | 63% | 2.47 |
| lateral erosion, alone | 0.17 | 62% | 2.53 |
| an uplift raster of seeded rates 0 to 0.8 mm/yr, 1 km cells, over the whole region | 0.39 | 36% | 2.93 |
| the uplift uneven over the plan, 0.5 of the local rate over 5 km (the first form tried) | 0.23 | 43% | 2.82 |
| the same as a fraction of the kernel's square root, 0.5 over 5 km | 0.26 | 42% | 2.83 |
| the same, 0.7 over 3 km (the default) | 0.29 | 39% | 2.88 |
| everything on, 0.5 over 5 km | 0.24 | 59% | 2.60 |

What it says:

- **It is the uplift's symmetry.** A ridge's kernel depends only on the distance to its line,
  so each flank is a ramp, a ramp's fall lines are parallel, and the rivers that form on it
  stay where they formed (a model of this kind keeps an "extreme memory" of its first surface:
  Kwang and Parker 2019, known here from its summary only). The one run that halved the comb was the one
  whose uplift was uneven in two dimensions at about the valleys' own spacing.
- **It is not the routing's symmetry**: dividing the flow leaves the mountain network as it
  was and makes the count worse, because the fans it builds are steeper and their radial
  channels, which do run straight down the range, come into the count.
- **And not, at these rates and in this time, the missing piracy**: lateral erosion alone took
  banks all along the trunk valleys and captured nothing in 1.5 million years. Kwang, Langston
  and Parker 2021 report it reorganising a network over ten million.
- So the lever is `uplift.variation`: seeded noise over the plan (four octaves from
  `variation_length_m` down, each half the wavelength and 1/√2 the amplitude, on a lattice of
  the world's own metres so a place has its number whatever lattice reads it) times a fraction
  of the rate about the point. **The fraction is of the kernel's square root** and not of the
  kernel: tried first as a multiple of the local rate it did little, because a kernel's flank
  rises slowly and a fraction of little is less; the raster that worked was uneven on the
  flanks as much as on the crest.

**It is reduced, not gone**, and the page should not claim more: 0.29 junctions a kilometre
against 0.20, 39% against 44%, where the raster reached 0.39 and 36% by raising the plains as
well, which a range does not. In the relief the valleys now differ in size, branch, and leave
the crest at angles; a person still sees that most of them cross the range the short way, which
is also what the rivers of a real linear range do (Hovius 1996, "Regular spacing of drainage
outlets from linear mountain belts": the title is its finding, and it was not read here). What
would take it further is in
[what still reads wrong](#pictures-and-what-still-reads-wrong).

### Ice

A sketch may have an ice age (`ice`: the equilibrium line's altitude, how fast snow becomes ice
above it and ice melts below it, the ice's erodibility, a glacier's width and thickness for its
flux, and the years it stands). The model is **Hergarten 2021's stream-power form for valley
glaciers** ([the grounding](../experiments/landform.md#ice-the-sources) says what was read and
what each number is), chosen because it is the published scheme that fits a solver whose every
decision is a sum, a product, a division and a square root: the ice's thickness is not solved
from its flow, so there is no ice-flow time step (a shallow-ice solve at 16 m wants steps of
hours), and its erosion is the rivers' law with other numbers, solved in the same sweeps.

Each step the ice stands:

1. **The flux.** A point makes ice at the share min((s − ELA) / `full_m`, 1) of its snowfall,
   s the height of the ice's surface there last step; below the line the share is negative,
   (s − ELA) / `melt_m`, a loss. The flux is the sum down the bed's own receivers (the one
   thread: ice does not divide here), in cells making ice in full. Bare ground below the line
   makes and loses nothing; a point the glacier covered last step passes its loss on toward
   the glacier's line, so **a glacier pays for the melt over the whole of its swath** and ends
   where its catchment's ice is spent. That last is this solver's reading: Hergarten's sum takes
   the loss of every point of the catchment, ice on it or not. Tried first as a loss over a
   width from the flux at each point of the line, it melted every slope's ice within four
   points of the line, because a slope's parallel threads each claimed a glacier's width.
2. **Width and thickness.** A glacier is `width_m` wide where its flux is a square kilometre's
   and its width goes with the flux to the power 0.3 (Hergarten's equation 13); its ice is
   `thickness_ratio` of its width thick. **The swath** is every point within half that width of
   the glacier's line whose bed is not above the ice there **and which drains to that line**
   (Hergarten's own definition: a swath is grown from a line point through its donors; W126).
   Each point walks down its own line; a line point q stands for every line point from the
   receiver to q once the point is within half of q's width of q; the point takes the largest
   flux of those and the ice that flux makes, and **never more than its receiver has**, so a
   step down in the ice from a point to its receiver is only ever the glacier's own thinning
   along its line. (The power is the one in the solver that is not a square root:
   `det::exp(0.3 * det::log(x))`, once a point of a line a step.) **The region's edge is not a
   snout**: a held point is outside the model (no flux is counted there), so it carries the
   thickest ice of the points that drain to it, and a glacier that leaves the region reads no
   step in its surface there. Stage C stamped a disc about each line point instead, which put
   a glacier's ice on points that drain out of its swath; each of those read the swath's
   margin as a snout a glacier thick, and that was the trench at a held edge
   ([the count](../experiments/landform.md#the-held-edge-counted-and-mended)). **The step the
   ice comes finds it twice**: a glacier pays for its melt over the swath it had last step,
   and with none yet it would melt along its line alone and run to the region's edge whatever
   the line's height.
3. **Erosion** is in the rivers' own update. Under a glacier a point's stream power is
   the ice's erodibility times the square root of its effective flux (the point's own
   catchment to the 1/4 times the swath's flux to the 3/4: Hergarten's equation 22, square
   roots of square roots) plus the meltwater's (the rivers' law on the cells that are not
   ice). **The ice's term works on the slope of the ice's surface**: it draws the bed toward
   the level at which that surface would be flat from the point to the point its erosion
   reads, which beside a glacier's line is the receiver (so a tributary's bed is worn to the
   trunk's ice and not to its bed, and hangs above it) and on a glacier's own line is a
   coupling's reach down it (the next paragraph). The meltwater's term draws the bed toward
   the receiver's bed, as a river's does. Under ice the flow does not divide, no bank is
   taken, a hollow in the bed is not a lake, and nothing is laid down in the sweeps.

   **Basins the ice digs.** Ice flows down its surface, not its bed, and its surface is
   smooth over several thicknesses whatever the bed does, so a glacier goes on wearing a bed
   that rises under it. Along a glacier's own line (the points whose own flux is their
   swath's) the slope is read from the point p to the point q twice `coupling` thicknesses
   down the line (Kamb and Echelmeyer 1986's coupling length, one to three thicknesses for a
   valley glacier; the reach is the downstream half of their averaging window), the ice's
   level is T = b_q + h_q − h_p, and its power is over the reach's length. Where T is below
   the receiver's bed b_r the bed would be adverse, and **the published limit bounds it**:
   an adverse bed steeper than `adverse_slope` times the ice surface's slope shuts the
   erosion down (the supercooling threshold, 1.2 to 1.7), which is the closed form
   T′ = T + (b_r − T) D / (D + c d), D the reach and d the step to the receiver. And
   **meltwater wears a bed down and never fills it**: the two terms are one implicit update
   toward their mean by their powers, and where that update ends below the receiver's bed
   the water's term has pulled the bed up, which an erosion law cannot do, so there the ice
   works alone with its own part of the power toward T′. Without that last a basin was as
   deep as the ice's share of the power let it be, next to nothing in a glacier's lower
   reach where its meltwater is most
   ([what was measured](../experiments/landform.md#the-basins-as-built-and-what-they-are-worth)).
   A point of the swath beside the line keeps the step to its receiver: the surface across a
   glacier is level, and W106 measured the trough's section gone when every point read a
   reach.
4. **Till.** What the ice carries melts out with the ice: at a point that loses a share of the
   ice reaching it, that share of the sediment reaching it is laid as till, spread over the
   glacier's width there, kept aside until the step's books are done and then added to the
   ground and to the sediment. A point takes till up to the glacier's thickness and no more (a
   moraine stands no higher than the ice that built it); the rest goes on with the meltwater
   and the rivers lay it down as outwash by their own law. This melt-out rule is this solver's.

**What it makes, measured** (the tests, W126's numbers): a trunk under ice for 100,000 years
is worn 25 m below the same valley under a river at the junction, down to the level of the edge
it leaves through and no lower, and is three times as wide a hundred metres above its floor
(108 points over three sections against 34; the swaths there are 75): a floor the swath wide
between walls that hang above it by the step in the ice at the swath's margin. A tributary's
bed meets the trunk's swath at its steepest step (14 m in one 50 m point where the river's
steepest is 2.7 m). A glacier whose line is above its valley's floor ends inside the region
(with the line 250 m above the valley's head, half way down an 8 km valley at its farthest)
and leaves no point below the region's base level. In the two-range sketch
([pictures](#pictures-and-what-still-reads-wrong)) with the line at 800 m the ice covers the
upper valleys, an eighth of the lattice, and those are wider and softer with open heads; below
its reach the valleys are the rivers'.

**What it does not make, said plainly**:

- **It makes a trough, and a mature one is flatter than a rock trough is measured to be.** By
  the stated measure (a section's width half way up its lowest hundred metres over its width
  at the hundred; a section y = a x^b measures (1/2)^(1/b)) the river's trunk is b = 1.2 and
  the glacier's passes through the published range and out of it: 1.5 to 2.3 after 10,000 to
  20,000 years, 3 to 9 after 40,000, 4.5 at the test's 100,000. Graf 1970's troughs run 1.5 to
  2.0 and Svensson 1959's valley is "only slightly" off 2; the widest range found is 1.0 to
  3.5 with most between 1.3 and 2.5
  ([the grounding](../experiments/landform.md#the-troughs-section-on-the-mended-ice) says how
  far each was read). **Why**: every point of a swath is worn toward the line with the line's
  own flux, so once the trunk nears its base level the swath is ground to a floor whose cross
  fall is a few times the fall along it, and the walls outside the swath hang above it by the
  step in the ice. That is the U of Hergarten's own sentence ("introduced by the swath
  construction rather than predicted"). **What is missing** for a parabola that lasts is the
  erosion's fall across a glacier's width (the published section models wear in proportion
  to the sliding speed squared, greatest under the thickest ice); it is not built, and the
  test holds the lower bound only. The level floor of the photographs is a different matter
  and mostly not rock: a lake's fill and outwash in a basin the ice dug below the river's
  grade. The solver has the fill and not the basin.
- **The trench at a held edge is mended** (W106 found it, W126 counted and mended it:
  [the count](../experiments/landform.md#the-held-edge-counted-and-mended)). Stage C's trunk
  ended at 1,041 m against an edge held at 1,273 m, with 2,407 points under the edge. Now none
  is, the trunk meets the edge with no step, and the held point carries the ice of the point
  that drains to it (322 m either side of the edge). Stage C's "196 m below the river's bed",
  "1.7 times as wide" and a hanging step of 40 m were that trench's.
- **A hanging valley's step is not tall here.** What the rule gives is where a tributary's
  step stands (where its own ice meets the trunk's swath) and that it cannot be worn away from
  below; how tall it grows depends on how long the swath's margin stays on one point. In the
  test's valley the trunk's ice thins from 320 m to 294 m over the run, the swath narrows by a
  point, and the step at the new margin is 14 m (9 m before the basins' rule). A tributary much thinner than its trunk would
  hang by most of the difference; no test valley has one yet.
- **Creep leaves hollows of centimetres under ice.** A hollow under a glacier is not a lake
  and nothing is laid in it, so what hillslope creep raises unevenly on a floor the ice has
  ground level stays as closed hollows 9 to 14 cm deep (measured in both test valleys; with
  the creep off there are none, to the millimetre). They are under the 16 m lattice's
  quantum of interest and are said here so nobody reads them as basins.
- **Basins are dug in rock, and a sketch that lays till buries them** (W126). With nothing
  laid down, a valley fed from a firn plateau holds 130 points of standing water a metre or
  more deep along six kilometres of its floor (the deepest 7.5 m) where the same valley under
  its rivers holds none; the depth goes with the limit (2.4 m at 0.5, 12.2 m at 6), and the
  first rises the axis' bed shows stand at 0.89 of the ice surface's slope, under the 1.5.
  **But the till rule lays what the ice carries exactly where the ice melts, up to the ice's
  thickness, and that is where the basins are**: with deposition on, the same valley holds
  water behind mounds of till (819 points, to 40 m), not in rock basins, and the two-range
  sketch with a colder ice age (the line at 600 m, the ice gone one step before the end)
  holds 11 points of a metre of water, where stage C's solver held 2. **So a person looking
  at the two-range ground still sees no trough lake.** The rock floor under a basin's fill is
  the height less `sediment`; the flat's extent as a field of its own, and the rock floor as
  one, are not written yet (the field file is unchanged at version 3). No cirque and no
  tarn: a cirque is this rule under a glacier that ends near its head, and wants the sketch's
  ice to have a history of stands. Till is still a sheet and mounds, not a ridge: the
  moraines' rule ([designed](../experiments/landform.md#the-basins-the-ice-digs-the-sources-and-the-rule),
  the melt-out laid on the ice's margin) is what would stop it burying the basins, and is the
  next thing to build.
- **Glaciers are long.** All the snow above the line in a catchment feeds the ice and only the
  glacier's own surface melts it, so a catchment with tall walls sends its ice far below the
  line. `melt_m` (half of `full_m` by default) is the knob.
- **The ice is fast.** At the default erodibility of twice the rivers' a trunk is worn 2 mm a
  year. An ice age in a sketch is tens of thousands of years, not hundreds.
- No crevasses, no ice surface as geometry, no snow: the field file's `ice` is what the snow
  and glacier solver starts from.

### The rivers' sweeps

Stage B's sweeps walked the flood's order, which visits the lattice by height and so in no order
of memory, and each visit recomputed a square root and three divisions from a dozen arrays
indexed by cell. They were most of a step. Stage C lays the step's network out once **in the
sequence the sweeps walk it** and walks that:

- **Levels.** A held point is level 0; any other point is one more than its highest receiver.
  A point reads its receivers (lower levels) for its height and its donors (higher levels) for
  its flux, never its own level.
- **The sequence** is the points by level, and within a level by the place of the receiver that
  set the level, so the donors of points that follow each other follow each other too: a
  breadth-first order of the network (Braun and Willett 2013's stack, by generation), in which
  a sweep's reads of the level above and of the level below both run forward through memory.
- **Beside each point**, by its place: its kind this step (held, a lake's member, a slope or sea
  bed that only passes its flux on, a reach), the constants of its update (K Δt of its sediment
  and of its rock times its stream power, the share of the flux it lays down, its sediment),
  and its edges, each a run: its receivers with the weight of each one's height in its base
  level, its donors with the share of each one's flow that comes to it.
- **A sweep** is the flux, levels from the top down, each point gathering what its donors pass
  in the fixed order of the eight directions; then the heights, levels from the bottom up.
- **The sweeps start from the last step's answer**: a reach near its grade moves by nearly the
  same amount every step, so the first guess is the ground less what the rivers took last time.
  The fixed point is the same; the sweeps that would find it again from nothing are saved where
  the ground is young (118 sweeps to 107 over twenty steps from a plain) and not where it is
  old: a mature range still takes eight a step, which says the count there is set by a few
  points that keep moving a millimetre (a lake's rim, a bed changing from sediment to rock)
  and not by the guess. Finding those is what would halve the sweeps.
- **In parallel, by level.** A level's points write only their own entries, so a level runs on
  the job system in chunks (`SolveOptions::level_grain` points each, where the level holds two
  chunks or more) and the bytes are the same at any thread count; the largest move of a sweep
  is a maximum, which does not care what order the chunks report in. **What it buys at the
  sizes solved here is nothing, and the page should say so**: a network has as many levels as
  its longest river has points (`Landform::network_levels`, in the command's summary), so the
  bench's 1,024 by 1,024 sketch, twenty steps old, stands in 735 levels of 1,400 points, and an
  older ground in more and narrower ones; a level that small costs less to walk than to hand
  out. It pays where a level is tens of thousands of points wide (a lattice 8,192 a side and up). What
  would pay at every size is the published other way (Braun and Willett's: catchments are
  independent), which here needs the catchments that flow divides between joined first; it is
  not built.

What the layout cost and bought, measured as a pair ([performance notes](#performance-notes)):
a solve of a mature ground takes half the time, and the sweeps are under two fifths of it. The
layout itself (the shares, the levels, the sequence, the constants and the edges) is a fifth of
the new step and the flood another fifth: every part now costs about the same, which is where to
look next.

### The flood's cost

Stage A's flood put every point through its queue, in order of height, which is no order of
memory: at 2,048 points a side it was nearly the whole step, and its cost a point tripled from
256 a side. **What the profile said** (`landform.walk`, the lattice walked in the flood's order
with each point reading its eight neighbours and no queue): the walk is 119 ms of a route of
812 ms at 2,048 a side, so it is the queue that costs and not the order of memory; the table is
under [performance notes](#performance-notes).

**The queue now sees only the ground that might not drain.** A point with a path to a held
point that falls by a unit or more at every step is never raised by the flood: its filled
height is its own. So:

1. Every point's height is quantized, and every point finds its lowest neighbour, in memory
   order and in parallel.
2. One walk in memory order follows each point's chain of lowest neighbours to its end. A chain
   that ends at a held point is **drained**; one that ends in a pit or a flat is not.
3. The queue floods the points that are not drained, seeded by the drained points that touch
   them. Those seeds have their final heights, and pops ascend, so each raised point is raised
   by the same neighbour, to the same height, as in the flood of everything.
4. The order is every point sorted by (filled height, index): a stable radix sort by height,
   twelve bits a pass over the bits the heights span, of points that start in index order.
   The pops of the whole lattice were exactly that sort, since a push is never below the pop
   that made it.
5. Where more than half the points might not drain (a rough plain, the first steps of a run)
   the queue would have nearly every point anyway and the sort would be work on top of it, so
   the flood of every point is run as it was. Both give the same bytes.

**The bytes did not move**: the filled surface, the sills, the order, and so every pin of the
tests, are what the flood of every point gave. This is the published idea (Barnes, Lehman and
Mulla 2014's second algorithm keeps pit cells off the priority queue; Zhou, Sun and Fu 2016 and
Wei, Zhou and Fu 2019 grow regions so that most points never enter it, cutting the queue's
traffic by about 70%) taken to the form this solver's total order allows: no point outside a
depression's catchment enters the queue at all, and the order comes from a sort. Cordonnier,
Bovy and Braun 2019's basin graph, which never fills, was not taken, because it changes the
receivers inside a depression and with them every byte. The sort keeps two buffers of eight
bytes a point for the run.

### Structure

A range in uniform rock has valleys evenly spaced and summits that are the same on every side,
because nothing in it has a direction. Rock has one: beds. The sketch's `rock.formation` is **the
scene's own `engine.scene.Formation`** (W85; [rocks](rocks.md), "Geology on the world formation
(W85)"; [the formation field](../experiments/formation-field.md)), copied into the sketch, and a
point's resistance is the `resistance` of the bed its surface cuts.

- **The beds rise with the rock.** The formation is read in the rock's own frame: a point of
  rock now at height z stood at z less the point's uplift so far before it was raised, and the
  bed coordinate is taken there. So a bed is carried up through the surface as the ground is
  cut down through it, the rock at a point changes as the run goes on, and where uplift differs
  from place to place the beds arch over it. Reading the bed at the surface's present height in
  a fixed frame, as stage A's elevation bands did, makes flat-lying layers that never move.
- **Which beds.** The macro beds where the formation has them (members, tens to hundreds of
  metres: what a lattice of metres to tens of metres can hold), else the beds. A bed thinner
  than a point's fall is sampled, not averaged, and reads as noise: give the solver beds the
  lattice resolves.
- **What resistance does** is what it did: it divides the erodibility by up to `contrast` and
  raises the critical slope toward `critical_slope_resistant`. It is read once a step, after
  the uplift, for every point.
- **How it is reached.** Through `domain/gfx/formation.h`, the field's one CPU definition and
  the double-precision twin the rocks solver reads: `pack_formation`, `formation_coordinate`
  (the folds through `det::sin`) and `formation_bed`. `gfx` is a domain module and not a
  capability, so the dependency is an ordinary one; nothing of the device is touched.
- **It replaced the table of resistance by elevation band**, which a sketch may no longer
  carry: `rock.bands` is refused as an unknown field. No migration is registered, because a
  sketch file stores no version to migrate from.
- **The field.** `resistance`, a byte a point, is the rock at the solved surface: where a hard
  bed crops out.

**What it makes** (the test: a block rising between two open edges in beds that dip 15° toward
one of them, 300 m of soft rock under 200 m of hard a tenth as erodible): the ground on a hard
bed stands 2.2 times as steep as on a soft one, a step at each outcrop; the two sides of the
divide differ, the side that cuts across the beds a tenth steeper than the side that falls with
them (1.09 to 1, against 1.0002 to 1 in uniform rock), and a hard bed's outcrop is 1.93 times as
wide down the dip, where the surface runs with it as a plate, as across the beds, where it is an
edge; the divide stands 3.4 points off the middle. **What it does not make**: a cuesta's cliff
over its own debris slope, which needs the rock solvers; and at n = 1 the published result is
that contacts steepen without a fixed ratio (Perne et al. 2017), so the 2.1 is this sketch's and
not a law.

### Uplift along a ridge

A ridge line raised evenly along its length has a crest of one height and valleys evenly spaced
down both sides, which is what the first agent's range showed and no range does. A ridge's
`rate_mm_per_yr` now varies along its line by `variation` either way (default 0.35), by a seeded
noise of the **station**, the distance along the line: four octaves from `variation_length_m`
(default 6 km) down, each half the wavelength and 1/√2 the amplitude of the one before, so its
power spectrum falls as the wavenumber squared, which is what transects of real topography do
([the grounding](../experiments/landform.md#slopes-and-steepness-the-sources)). The station is
the plan's own, so the same line gives the same uplift whatever lattice reads it. The test reads
the uplift off a ridge raised for one step with no erosion: at `variation` 0.5 the rate runs
from 1.48 to 2.45 mm/yr about 1.84, and the mean squared difference between stations 1,600 m
apart is 10.7 times that between stations 200 m apart; at 0 the line is even to the millimetre.

**The defaults against real ranges.** At the defaults (1 mm/yr on a ridge, K = 5 × 10⁻⁶, G = 1)
a river's steepness is (1 + G) U / K = 400 m, which at a reference concavity of 0.45 and a 10 km²
basin is a normalized steepness of about 180 m^0.9: the top of what the San Gabriel Mountains
show, where they erode at about a millimetre a year. The critical slope's default went from 0.9
to **0.8** (38.7°), between the 37° modal slope of that range's threshold hillslopes and the 40°
fitted to the Olympic Mountains. **The default sketch** (one default ridge across an 8 km square
at 16 m, a million years) has a mean slope of 0.51 (27°) and **27.5% of its land within a
twentieth of the critical slope**; at 0.9 it was 0.55 and 24.5%. So the defaults do not leave
the land at the critical slope. A sketch can: the first agent's valley raised its ridges at 3 to
4 mm/yr over K = 3 × 10⁻⁵ and measured 0.66, and with deposition the same sketch is steeper
still (0.71), because a river that carries sediment stands 1 + G times as steep. A range that
is to read as dissected at a 16 m lattice wants a steepness of tens of metres, not hundreds
([the sketch](#the-sketch)).

### The slope limiter

No slope stands above its point's critical slope (Roering, Kirchner and Dietrich 1999's law has
flux growing without bound there). The limiter lowers a point to the largest height that keeps
the gradient's length at the critical slope: from the lower neighbour along x and the lower along
z it is the upwind step of the eikonal equation, (a + b + √(2f² − (a − b)²)) / 2 with f the
critical slope times the spacing, and the four diagonal neighbours then hold the slope along a
diagonal to the same bound. **Why not a limit along the eight directions alone**: that makes a
failed slope an eight-sided pyramid with creases along the lattice, which is what the first
relief showed; the eikonal step makes it a cone. What the limiter removes in a step is sediment
flux from that point in the next ([deposition](#deposition)); only what the settling after the
last step removes leaves the model.

### The borders

The proof of concept's two worst landform defects were a ramp switched on in one row at a
region's edge and a fixed sea bed met as a 370 m underwater wall.

- **An open edge** is held at its base level, and uplift is tapered to nothing toward it over
  `border_reach_m` by a smoothstep, so the ground meets the edge at the edge's level and with no
  slope. Past a heightfield the tile set holds the edge flat, so the solved ground joins that
  plain without a step. The test's bound: the first row within 2 m of the edge over a 100 m cell,
  and no row's slope more than 0.2 from the row before it (measured 1.45 m and 0.11); without the
  taper the first row stands 89 m up.
- **A closed edge** is a wall nothing crosses. Two regions that share one do not exchange water;
  solving neighbours so their shared edge agrees is not stage A (below).
- **The sea** is a level and its bed is ground like any other: in the lattice, routed across to
  the sea edge's held points, diffused, and held to the critical slope. Only the stream-power
  term stops at the waterline. So a land surface cut or raised beside the sea cannot leave a
  wall, and a starting ground that has one loses it (the test starts from the 370 m wall and ends
  with no step between neighbours above the critical slope).
- **A point below the sea's level is under water wherever it is**: a basin inland of the coast
  that lies below the level is not cut by rivers either.

## The same bytes everywhere

A sketch solves to the same bytes on MSVC, GCC and Clang, at x86-64-v3 and v2, and at any thread
count (`determinism_tests.cpp` pins the hash of the heightfield and of each field).

- Every decision is IEEE `+ − × ÷`, `sqrt`, `floor` and integers. No C library function is in one:
  the ridge kernel and the noise are polynomials and integer hashes, and m = 1/2 makes A^m a
  square root ([what determinism decided](../experiments/landform.md#what-determinism-decided)).
- The flood's order is a total order on (height, cell index).
- The job system runs only the loops whose points do not read each other's results: the uplift
  and hardness fields, the receivers, the diffusion (a Jacobi step), the output fields, the
  constants and edges of the step's network, and a level of the rivers' sweeps
  ([the rivers' sweeps](#the-rivers-sweeps): a level's points read other levels only, and a
  point's flux is a gather over its donors in a fixed order, never a scatter). The flood, the
  discharge, the levels, the step's books, the lakes and the limiter walk an order and are
  serial. The worker test runs the sweeps at a grain of eight points, so a small lattice's
  levels do go to the workers.
- The rivers' sweeps end on a comparison of heights, which is the same comparison everywhere
  because the heights are; a lake's members and a deposit's neighbours are sorted by height and
  then by cell, a total order; and the budget's sums are taken in one order whatever the workers.
- Nothing depends on the order tiles activate, because nothing here is per tile.

## The outputs

`write_landform` writes, into one directory, for a stem `<name>`:

- **`<name>.json` and `<name>.f32`**, `engine.terrain.heightfield/1` with `vertical_mm_per_unit` 1:
  each float is a whole millimetre, which a float32 holds exactly below 2²⁴ (16,777 m; the solver
  refuses a ground outside ±16,000 m), and the tile set's build turns back into the same integer.
- **`<name>.fields.json`** and a raw grid a field, below.
- From the command, **`<name>.relief.png`** and **`<name>.drainage.png`**: a shaded relief lit from
  the north-west at 45°, the sea tinted, and the same with the channels drawn by their area. For a
  person to look at; nothing reads them.

### The field file

`engine.landform.fields/3` (`engine.landform.Fields`; version 2 added `sediment` and `resistance`,
version 3 `discharge`, `channel_width`, `ice` and `till`; a reader of an older version is refused by the
version, since the solver that wrote it is not this one). What a later solver (channels, scree, soil,
ecology, snow) reads:

- **The lattice is the heightfield's**, point for point: `width`, `height`, `spacing_mm`,
  `origin_mm`. Point (i, j) stands at `origin + (i, j) × spacing` in the world's millimetres, and
  every grid is little-endian, rows of x in order of z.
- **The fields**:

  | Name | Type | Unit | Notes |
  |---|---|---|---|
  | `drainage_area` | u32 | cells draining through the point, itself included | times `spacing²` is the area. Exact integers |
  | `receiver` | u8 | the neighbour the point drains to, 0 to 7: east, south-east, south, south-west, west, north-west, north, north-east (`k_receiver_dx`, `k_receiver_dz`; +z is south) | 255 at a held point, which drains out of the region |
  | `slope` | i32 | the fall to the receiver over the distance to it, parts per million | zero or less where the way out crosses a depression: the receiver is the filled surface's |
  | `erosion_rate` | i32 | micrometres a year removed by the last step, net of what it laid down | negative where the ground gained; zero for a run of no steps and at held points |
  | `fill` | u32 | millimetres under the depression's spill | zero on draining ground; a tarn's depth |
  | `sediment` | u32 | millimetres of sediment under the point | what rivers, lakes, the sea and creep laid down and nothing has removed since; zero where rock is at the surface and for a run of no steps. The rock's surface is the height less this |

  | `resistance` | u8 | the resistance of the rock at the surface, 0 to 255 | the bed of the sketch's formation the solved surface cuts ([structure](#structure)), or the sketch's uniform value. Where it is high a hard bed crops out: a ledge, a cliff band, the cap of a scarp |

  | `discharge` | u32 | the cells whose water passes through the point when flow divides on gentle ground ([flow that divides](#flow-that-divides)), rounded to whole cells | equal to `drainage_area` wherever nothing above the point divided. Where it is large over a width of points the river is that wide a braid or the fan that broad; where `drainage_area` is large and this is not, the one thread is a choice among many |
  | `channel_width` | u32 | millimetres: the sketch's `channel_width_m` times the square root of `discharge` in square kilometres | the downstream hydraulic geometry's exponent of one half (Leopold and Maddock 1953, not read: the classical value) with an authoring coefficient, 5 m at one square kilometre by default. A channels solver that has its own relation reads `discharge` and ignores this |

  | `ice` | u32 | millimetres of ice over the point at the last step the ice stood ([ice](#ice)) | the glacier's extent and thickness: with a sketch whose ice lasts to the run's end, the ice of today; with one whose ice ended, the last glaciation's reach (its trimline is where this meets zero). Zeros with no ice age |
  | `till` | u32 | millimetres of the sediment that melted out of the ice and nothing has removed since | never more than `sediment`. Thick in a band across a valley is a moraine; thin and wide is ground moraine |

- **What the snow and glacier solver should take from `ice`**: where to put a glacier's
  surface (the bed plus this), where it steepens over a step in the bed (an icefall: `slope`
  under ice), and that snow lies deepest where this is not zero and the ground is gentle.
  **Lakes**: `fill` above zero inside a former glacier's reach is a tarn or a trough lake.
  **Waterfalls**: a hanging valley's mouth is where a tributary's `slope` jumps at the edge of
  the trunk's `ice`. **Soil and scatter**: `till` is boulder-rich, unsorted ground.
- **What a channels solver should take from the two**: its course from `receiver` and
  `drainage_area` as before; its width from `channel_width`; and where `discharge` spreads over
  several points beside the course, the bed it may braid or move in (with `sediment` under it,
  an alluvial floor; with none, a strath).
- **What each later solver should take from `sediment`:**
  - **Scree and rock** (the cliffs, the deposits): rock is at the surface where it is zero, so
    an outcrop or a cliff belongs only there; a slope near the critical angle with a metre or
    more is a debris slope, and the foot of a face where it thickens is where a cone or an
    apron stands. It is not an angle of repose: the solver lays no talus.
  - **Soil**: the parent material. Zero is soil weathered from the rock in place, thin on a
    fast-eroding point (`erosion_rate`); alluvium and lake fill are deep, fine and level; a
    thin layer at a slope's foot is colluvium.
  - **Flood beds and channels**: a channel on sediment is alluvial (a gravel bed, bars, a bed
    that can move across its floor: the run of sediment across the valley is the width it has
    to move in); a channel on none is cut in rock (steps, pools, no bars).
  - **Ecology and the sward**: a level floor on deep sediment beside a channel is the wet
    meadow and the riparian strip; a fan's surface drains fast; lake fill at the water's level
    is fen. `fill` above zero is still open water.
  - **Water**: where the sediment is deep the water table is the river's; a lake whose `fill`
    is small over deep `sediment` is nearly silted up.

- **A window**: `read_field_window(json, header, name, x0, z0, w, h, out, bytes, error)` reads
  points [x0, x0 + w) by [z0, z0 + h) at byte offset `(j × width + i) × size`. A tile asks for
  the points its cells stand on by index: `i = (x_mm − origin_mm[0]) / spacing_mm`.
- **At a region's edge**: the fields hold nothing past it, and a window that leaves the lattice
  is refused, not held (a held edge would invent drainage). A held point's `receiver` is 255 and
  its `drainage_area` is everything that leaves through it; the held points' areas sum to the
  lattice. On a closed edge nothing leaves. A point under the sea has a receiver and an area like
  any other; a reader that wants land compares the height with `sea_level_mm`.
- **What it was solved from**: `sketch_hash`, `solver_version` (bumped whenever the same sketch
  would solve to different bytes), `route_seed` (what `landform::route` takes to find the same
  receivers from the heights), `steps` and `step_yr`. Each field carries the hash of its grid.

`landform::route` is the flood, the receivers and the area of any ground with any held points, for
a tool that wants the routing of a ground the solver did not make.

**A request to the channels generator** (another row's module, so a request and not an edit):
take a channel's head and course from `drainage_area` and `receiver` over the window its region
covers, in place of its own D8 lattice and a local 4 m slope
([the grounding page](../experiments/landform.md#a-request-to-the-channels-generator)).

## The command

```powershell
engine-content landform <sketch.json> <out dir> [--name <stem>] [--jobs <n>] [--no-relief]
```

Solves the sketch, writes the files above (the stem is the sketch's file name), reads the
heightfield back through the tile set's reader, and prints a JSON summary: the lattice, the
steps, the lowest and highest point, the mean slope of the land, the held, flooded and sea
points, the largest drainage area and the seconds the solve took. Then
`engine-content terrain-tiles <out dir>/<stem>.json …` builds the tile set from it as from any
authored heightfield. Exit 0, 1 (a file or a range), 2 (usage).

## What every solver of this wave holds, and where this one stands

1. **The same bytes everywhere**: held, above.
2. **Tile-stable**: the output is on the world's lattice and the tile set cuts it into tiles; the
   solve is of a region whole, so there is no tile to depend on.
3. **Nothing exists before its tile**: not held, by kind. This is not a placement generator: a
   drainage network cannot be solved a tile at a time, since a point's area is everything above
   it. It is the plan above the tile, built once and offline, and the tile set streams what it
   wrote.
4. **Merged, with a far form**: the heightfield's far forms are the tile set's far chain.
5. **It writes to the ground, and the others read it**: it writes the ground itself, and the
   field file is how it is read.
6. **Grounded in published research**: [experiments/landform.md](../experiments/landform.md).
7. **Budgets stated**: below. No triangles and no per-tile cost; an offline solve's seconds and
   the bytes it holds while it runs.

## Not stage A

Each follows, and none is here: the basins the ice digs and what fills them, cirques, and
moraines as ridges (designed in stage D and not built: [ice](#ice),
[the grounding](../experiments/landform.md#stage-d-w106-the-sources-and-the-design-that-is-not-built)); scree at an angle of repose; soil depth; snow; a general m and n; neighbouring regions solved so a shared edge
agrees; the rivers' sweeps in parallel by catchment (by level they are, and it buys nothing at
these sizes: [the rivers' sweeps](#the-rivers-sweeps)); baking far-terrain maps from near output; anything
in the editor. The limits of the model as it stands are on the grounding page.

## Invariants

- Every point's path of receivers ends at a held point, and the held points' areas sum to the
  lattice.
- No pair of neighbours stands steeper than the upper point's critical slope, land or sea bed
  (to the two millimetres rounding adds).
- The fields are the routing of the heightfield written beside them: `route` over the heights
  and the held points with `route_seed` returns the same receivers and areas.
- The output is a function of the sketch (and the starting ground): not of the thread count, the
  compiler, the CPU baseline or the C library.
- A run of no steps writes the starting ground's heights untouched.
- What is eroded is deposited, leaves through a held point, or is the last step's unrouted
  shed: `budget.eroded_m3 = deposited_m3 + left_m3 + unrouted_m3`, to nine digits.
- `sediment` is never more than the ground gained there: the rock's surface, the height less
  the sediment, only falls relative to the rock.

## Public API

`include/domain/landform/landform.h`: `validate_sketch`, `solve`, `route`, `route_divided`, `read_sketch`,
`read_start_field`, `write_landform`, `read_fields`, `read_field_window`, `shaded_relief`.

## Depends on

`base`, `containers`, `math`, `hash`, `json`, `schema`, `io`, `jobs`, `landform_schemas`.

## Testing

`tools/dev.ps1 test -Filter "^landform$"`. All CPU; the whole suite is about 50 s in `msvc-debug` on a loaded machine (2026-11-10, 36 threads busy: stage C's cases are the last rows' seconds, and its divided flow and banks added about half again to the seconds of the cases that were there, the worker test's four solves most of all at 8 s). Stage E's five new cases add about 13 s (2026-11-11), most of it the two that solve a run to each of its twenty steps: a test that bounds what the ice left by the thickest ice that ever stood has to know it, and the solver keeps only the last step's.

| Test | Holds | Fails when |
|---|---|---|
| the golden sketch solves to the pinned bytes | the hash of the heightfield and of each of the seven fields, for a sketch that uses every part of the model (a formation of dipping, folded beds among them); pinned on MSVC, reproduced by Clang and GCC | any toolchain, baseline or change of arithmetic moves a byte |
| one, four and sixteen workers | the serial solve's bytes from each | a parallel loop reads what another chunk writes |
| the flood breaks ties by cell index | a flat's order, receivers and ε, a pit's fill | the queue's order is not total, or a diagonal beats a cardinal of equal fall |
| every point drains | receivers' paths end at held points; held areas sum to the lattice | the flood or the receivers leave a point stranded |
| a uniform uplift's steady state | the mean erosion rate within 1% of the uplift; slope against area regresses to −0.5 within 0.02 (measured 0.4999); Hack's exponent in 0.5 to 0.7 (measured 0.616) | the incision is not the stream-power law, or the routing does not make a network |
| the ground meets an open edge without a step | the first row within 2 m, no bend above 0.2 a row; and that without the taper both fail | the border taper is lost |
| the sea bed takes part | no neighbours above the critical slope on land, across the coast or on the bed (with nothing laid down, and again on a coast the rivers build out, delta fronts included); a 370 m underwater wall gone | the limiter or the routing stops at the waterline |
| a starting heightfield is refined | the 800 m low-pass moves under a twentieth of the relief (measured 9.5 m of 407 m) while single points move 30 m; no steps leaves the heights untouched | a refine replaces its ground |
| the files round-trip | the heightfield read back whole millimetres; a field's window equals memory; a window past the edge is refused | the formats drift |
| a sketch outside a range is refused | six refusals, each naming its field | validation is lost |
| what is eroded is deposited or leaves | the two identities of the budget to nine digits on the golden sketch; the settled and unrouted share under 2%; the same with G = 0, where everything cut leaves | a pass lays down what did not arrive, or loses what did |
| with deposition a steady river stands 1 + G times as steep | the steepness factor S √A K / U over the channels in 1.85 to 2.05 at G = 1 (measured 1.955: the discrete form's (N − 1) / N), the mean rate within 2% of the uplift, the concavity within 0.03 of 0.5 | the deposit is not Yuan's, or is laid after the cut and not with it (which measured 2.14, rising with area) |
| a dammed valley fills to a floor | a V-shaped valley whose mouth is held 90 m above its bed: the run of fill of 5 m or more about the axis is 5 points or more wide in the lower third, more than 1.5 times the upper third's (measured 6.7 against 2.2), level across within 8 m where the V fell 87 m over the same width (measured 7.6 m), over 50 m of fill; run for what it was written for, one thread and no banks, as the steady-state cases are | nothing is laid down, or a lake is filled along the flood's order |
| a river that moves across a dammed valley's fill keeps a floor of its own (W106; 0.4 s: two solves) | the same valley with the defaults, beside the same with no bank taken: the river's own floor (the points within the bank rule's water depth of its bed) is wider than a thread and no narrower than with no bank (3.3 points a column against 2.3); the step beside it is taller than with none (9.1 m against 4.9 m: the aprons' foot cut to a scarp) and no taller than the ground rose in the run (30 m), the one bound there is, since the rule takes a bank of fill whatever its height ([what was found](../experiments/landform.md#the-two-checks-stage-c-restated-and-what-was-found)); the run's whole cross fall, 12.9 m, under that over the one thread's 8 m | no bank is taken on fill, or a taken bank is raised or left above its reach |
| a massif's rivers leave a fan on the plain | 400 m out from a massif's foot the ground stands over 3 m up opposite it (measured 9.9 m), a metre or more lower 700 m to either side (6.9 and 8.3 m), and 1,700 m away under what an even sheet of the same load along the line would stand at (2.4 and 3.0 m against 5.5 m, the crown over it; W106: by the shares none of the facing mouths' water reaches those points, and what does is the massif's flanks' rivers, so "under a metre" asked for a plain the sketch never left alone, and "under half the crown" was not derived) | the load is not laid where a river leaves the range |
| under dipping beds a ridge's two sides differ and a hard bed stands as a step | beds dipping 15° toward one edge of a rising block: the mean gradient on hard rock over 1.5 times that on soft (measured 2.2); across the beds over 1.05 times that down the dip (measured 1.09), and within 1% of 1 in uniform rock (measured 1.0002); a hard bed's outcrop over 1.5 times as wide down the dip as across (measured 1.93) | the beds are not read, or are read in a frame that does not rise with the rock |
| a ridge's uplift varies along its line | one step with no erosion reads the uplift off the line: at variation 0.5 the mean within 15% of the rate, every station inside the half either way, a range over 0.6 mm/yr, and the mean squared difference at 1,600 m over four times that at 200 m (measured 10.7); at 0 even to the millimetre | the variation is lost, is white, or leaves its bounds |
| flow that divides conserves its discharge (`rivers_tests.cpp`, 0.01 s) | on a cone falling at 0.03, 3,970 of 4,032 points divide, every point's shares sum to 32,768 and the held points' discharges to the lattice, exactly; with `divide_below_slope` 0, and on the same cone at 0.5, no point divides | a share is lost or made in the division, or steep ground divides |
| a single mouth's fan is a cone (2.2 s: two solves of a hundred steps) | a funnel-shaped catchment drains through one mouth onto a level plain for 200,000 years: on an arc 500 m from the mouth the ground stands over 2 m above the plain (measured 28.2 m) and varies along the arc by under a third of that (5.4 m, 19%); with one receiver a point the same arc measures 13.9 m and 3.7 m (27%) | the flow does not divide on the fan, or the load is not carried by the shares |
| a river's plain is wider than a point, and cutting down leaves a terrace (2.7 s: three solves) | a straight valley draining 20 km², banks three times as erodible as the bed, 400,000 years: the lower floor (points within 2 m of the lowest of their column) is 3 points wide or more (measured 8.8) and a point wider than with no lateral erosion (6.6), with over a hundred banks taken (11,310). Then the base level falls 20 m for 8,000 years: the river cuts down over a quarter of the fall in the last kilometre (7.5 m) into a floor narrower than the one it left (4.1 points against 10.2), and of the columns that owe a terrace (cut 5 m, old floor 3 points wider than the new) three quarters keep old floor within 1.5 m of where it stood (10 of 10, 6.5 points each) with its edge beside the new floor (10 of 10) | no bank is taken, or a river that cuts down takes its whole floor with it |
| a range raised unevenly is not a comb (3.9 s: two solves of a hundred steps on a lattice that holds a dozen valleys a side) | a 9.6 km range along x raised evenly and with the default unevenness: of the mountain channels, the junctions a kilometre rise by over 15% (measured 0.149 to 0.359) and the share running straight down the range falls (60% to 43%) | the unevenness is lost, or is of the local rate and vanishes on the flanks |
| ice leaves a trough where a river leaves a V, and a tributary hangs (`ice_tests.cpp`, 1.2 s: two solves of twenty steps) | a trunk valley falling at 0.02 with a tributary at 0.12, 100,000 years under ice and under a river: by the stated measure (width half way up the lowest 100 m over width at the 100 m: V 0.5, parabola 0.71, box 1) which is the exponent of a section y = a x^b, the river's trunk is under the glacial range's b = 1.5 (measured 1.19) and the glacier's at 1.5 or more (4.90: a floor between walls, past the published range, as [ice](#ice) says); the glacier's section at the 100 m is at least its swath wide (107 points over three sections against swaths of 75) and half again the river's (34); the tributary's steepest step is taller than the river's (14.1 m against 2.7 m) and stands where its ice meets the trunk's swath (the ice 50 m or more thicker below it: 224 m to 294 m); the trunk's bed at the junction is below the river's (1,290 m against 1,315 m) and not below the edge, under 100 m or more of ice (298 m). W126 restated three of these from what the mended ice must give: stage C's widths, its 50 m at the junction and its step of five times the river's were the trench at the held edge | the ice wears only its line, or a tributary is graded to the trunk's bed |
| a glacier that leaves through a held edge is graded to that edge (W126; 0.7 s) | the same trunk with its ice leaving through the open edge and the creep off: no point of the region under the edge's height (stage C: 2,407, the lowest by 274 m), no standing water over 10 mm, the trunk's axis never rising toward the edge by over a millimetre and never above its start, the last point inside within a metre of the edge (measured 0), and the held point carrying the ice of the point that drains to it (322.4 m on both) | a glacier's ice is put on points that drain out of its swath, or a held point is read as a snout |
| a glacier that ends inside its region leaves the edge alone (W126; 4.3 s: the run solved to each of its twenty steps, so the test knows the farthest and the thickest the ice ever was) | a valley falling at 0.03 with the line 250 m above its head, nothing laid down and the creep off: the valley glacier's snout (ice within two rows of the axis) past column 40 in its first two steps and 40 columns or more inside the lattice at its farthest (column 82 of 160); no point under the edge's height; no standing water deeper than the thickest ice that stood on the point, and none on ground no ice reached (measured none anywhere) | a swath's margin or the region's edge is read as a snout, or the step the ice comes runs it to the edge |
| a glacier digs basins its rivers cannot, and they hold water (W126; 6.5 s: the run solved to each of its twenty steps, and four more solves) | a valley under a firn plateau, the line at 1,750 m, nothing laid down and the creep off: under its rivers alone no point holds a metre of water and none over 10 mm; under ice some do, on the valley's floor (130 points, columns 132 to 197 of 200, the deepest 7.5 m); none where no ice ever stood; the deepest is shallower with the adverse-slope limit at 0.5 than at the default 1.5 (2.4 m) and no deeper at 1.5 than at 6 (12.2 m). Printed and not held: no point holds more water than the thickest ice that stood on it; reading the slope to the next point (`coupling` 0) the same valley holds 505 points to 34 m, a pit where each step's snout stood | the ice digs no basin, digs one where no ice stood, or the limit does not bound the dig |
| what a glacier's basin holds is filled from its head once the ice has gone (W126; 1.0 s) | the same valley with deposition, 60,000 years of ice and 40,000 of rivers after: water stands when the ice leaves (624 points of a metre or more, to 40 m, most of it behind till); less stands at the end (none), the points that held it gained sediment (3,667 m over a cell in all), and the rock's surface under each, the height less the sediment, is no higher than it was (624 of 624) | the flood does not find the lake, the rivers do not fill it, or the fill is counted as rock |
| a bed under ice rises no steeper than the limit lets it (W126; 0.4 s) | the same valley solved step by step to the first step that leaves the axis' bed rising (the third), so every rise was made under the ice the field file records: each rise to the next point, over the ice surface's slope from the point to the point twice `coupling` thicknesses on, is under `adverse_slope` (0.89 at 1.5 over 5 rises; 0.26 at 0.5 over 3), to two parts in a hundred for the sweeps' millimetre | the limit is not applied, or is applied to another slope than the one the solver reads |
| a glacier ends where its ice is spent, and leaves till (0.5 s) | the one valley of the snout's row (falling at 0.03, no plateau) with the line 100 m above its head: over 300 points under ice (1,532), the last of it past column 40 and 8 columns or more inside the lattice (column 146 of 160, which is the upper sides' slope ice: the row above measures the valley glacier), a hundred points or more with half a metre of till (2,673), and till never more than the sediment there | the melt is not charged to the glacier, or till is laid that is not sediment |
| an ice age solves to the same bytes on one, four and sixteen workers (0.7 s) | the heightfield, the ice, the till, the sediment and the discharge of a 30,000-year ice age from each, and the first three pinned on MSVC for Clang and GCC to reproduce | a loop of the ice reads what another chunk writes, or `det::exp` and `det::log` differ by toolchain |
| a lake's floor fills level, and its mouth builds a delta | a basin fed for ten thousand years: its floor away from the mouths level within 50 mm (measured 0 and 3 mm) under water still metres deep; with `delta_share` 0.5 the shelf under the mouths holds a quarter more sediment (measured twice) and the floor stands lower | the fill follows the flood's order, or the mouths get nothing |

## Performance notes

`engine_landform_bench` (`tools/dev.ps1 bench -Filter 'landform.*'`), `msvc-release`, 2026-11-10,
an i9-10980XE, the solve on four workers. **The machine was busy every time** (`--wait-quiet` gave
up: other processes used 43% to 65% of the CPU, `machine_state` in each run's JSON), so every
figure is an upper bound, and two columns taken an hour apart differ by the load as well as by
the code.

| Row | What | Stage A (2026-11-10, 63 to 75% others) | Stage B, the flood of every point (65% others) | Stage B as it stands (43% others) |
|---|---|---|---|---|
| `landform.route/2048` | the flood, the receivers and the area of a **young** ground: a rough plain two steps old, where nearly every point might not drain | 1.27 s | 812 ms | 870 ms |
| `landform.route/1024` | | 153 ms | 139 ms | 158 ms |
| `landform.route/256` | | 6.8 ms | 6.3 ms | 6.9 ms |
| `landform.route_drained/2048` | the same of a ground with no depression in it, which is what the rivers leave | — | — | **312 ms** |
| `landform.route_drained/1024` | | — | — | 59 ms |
| `landform.route_drained/256` | | — | — | 2.6 ms |
| `landform.walk/2048` | the lattice walked in the flood's order, each point reading its eight neighbours: the flood's memory traffic with no queue | — | 119 ms | — |
| `landform.walk/1024`, `/256` | | — | 21.8 ms, 0.80 ms | — |
| `landform.solve/2048` | **a 2,048 by 2,048 sketch at 16 m solved to 100,000 years in twenty steps** | **27.1 s** | 117.3 s (the queue kept off drained ground, no fallback; 59% others) | **117.6 s** (38% others) |
| `landform.solve/1024` | the same at 1,024 | 4.91 s | — | 17.7 s |
| `landform.solve/256` | the same at 256 | 0.218 s | — | 0.584 s |
| `landform.area_power/0`, `/1` | √A over a million areas, and `det::exp(0.45 * det::log(A))` | 1.66 ms, 69.3 ms | — | — |

**What the flood's profile said.** Stage A's reading was that the flood misses the cache because
it visits the lattice in order of height. The walk says otherwise: visiting four million points
in that order and reading their neighbours is 119 ms of a route of 812 ms. The rest is the queue,
a push and a pop of a 64-bit key for every point. So the cut is to keep points off the queue
([the flood's cost](#the-floods-cost)), and it is 312 ms against 812 ms (2.6 times) where the
ground drains, 62 ns a point less. Where it does not (the young ground of `landform.route`) the
flood of every point is run as it was, after a classification that found it had to be: 7% slower
at 2,048 a side in these two runs, inside what the load moved.

**Stage C, the sweeps laid out in their own sequence** ([the rivers' sweeps](#the-rivers-sweeps)),
2026-11-10, `engine-content landform --jobs 5` in `msvc-release`, **each row a pair**: the base's
executable (44c80a0a, built in the same tree and copied out) and this one's run one after the
other, twice, on a machine other agents were building on (36 threads at 56% to 100% while these
ran), so a row's two columns saw the same load and its seconds are upper bounds.

| Sketch | Base | Stage C's first commit (the sweeps alone) | |
|---|---|---|---|
| the bench's sketch, 1,024 by 1,024 at 16 m, twenty steps from a plain | 25.7 s, 30.2 s, 30.4 s, 29.2 s, 23.1 s | 16.2 s, 16.5 s, 18.1 s, 15.0 s | 118 sweeps before, 107 after; 735 levels |
| the same at 2,048 by 2,048 | 159.6 s, 166.2 s | 79.2 s, 130.5 s (the second beside this row's own other solves) | 123 sweeps before, 104 after; 1,787 levels |
| two ranges about a valley, 512 by 512 at 32 m, 300 steps (a mature ground) | 113.3 s | 57.7 s | 2,396 sweeps and 2,402: eight a step either way; 629 levels |

**And what the rest of stage C costs.** The same 512 by 512 sketch with the defaults stage C
ends with (flow that divides, banks, the uneven uplift) takes 105 s, about the base's time
again: the divided network has 855 levels and its sweeps take 13 a step (4,093 over the run,
51 s of the 105). With an ice age of 76 steps as well, 105 s. The picture's region (1,024 by
1,024, 300 steps) took 458 s, where stage B's took 385 to 513 s: stage C bought back with
the layout what its new processes spend.

Where the 57.7 s went (`Landform::timing`, the command's summary): the sweeps 21.5 s, the
network's layout 12.7 s, the flood and the receivers 12.2 s, the step's books 4.9 s, creep and the
limiter 4.8 s. Every part is now within a factor of two of every other, and four of the five walk
the lattice in the flood's order on one thread. The solve holds about 250 bytes a point while it
runs (stage B's 110, and the network's 140: sixteen for a point's shares, eight for its discharge,
its level, its two places and its anchor, ten doubles of constants and iterates, and about
twelve bytes an edge each way).

**Before stage C the sweeps were the cost.** A step with deposition solves its rivers in about eight sweeps,
each two walks of the order over a dozen arrays, where stage A's step had one. `landform.solve`
at 2,048 a side went from 27.1 s to 117.6 s (1.4 µs a point a step, of which the flood is about a seventh), and the picture's region (1,024 points a
side, 300 steps) from 75 s to 385 to 513 s. What follows is to keep a step's river data in the
order's own sequence, so a sweep reads memory in order, and the flow tree's levels in parallel.
The solve holds about 110 bytes a point while it runs (stage A's 54: the ground, the lift and
hardness fields, the filled surface, the order, the receivers, the areas and a scratch surface;
and stage B's 56: the sediment, the rivers' iterate, the shed a step carries to the next, the
tapered G, each point's sill, its rock's resistance, and the two buffers of the order's sort):
460 MB at 2,048 points a side, 1.8 GB at 4,093. Nothing is resident afterwards, nothing scales with the world's
size at run time, and there are no triangles and no per-tile cost: the tile set's are the
heightfield's.

**Stage E, the ice's swath by a walk and the basins' rule** (W126, 2026-11-11). The same
two-range sketch (512 by 512 at 32 m, 300 steps, 76 of them ice), `engine-content landform
--jobs 5` in `msvc-release`, the base's executable (b8eb339a, built in the same tree and
copied out) and this one's run alternately, the machine at 76% to 100% CPU before each run
(eleven other agents building), so every figure is an upper bound:

| Pair | Base | This solver | |
|---|---|---|---|
| after the swath's mend alone | 106.3 s, 89.6 s | 103.3 s, 111.5 s | |
| with the basins' rule | 106.9 s, 92.0 s | 114.4 s, 107.0 s | the sweeps 51.6 and 45.1 s against 54.9 and 49.8 s; the network 18.8 and 16.1 s against 19.8 and 18.5 s; the flood, **whose code did not change**, 16.4 and 14.0 s against 18.6 and 18.6 s |

So nothing is measured beyond the load: the part that did not change moved by more than the
parts that did. What the ice's new work is, in the ice steps only: a walk of at most 96
points down each point's line for its swath (in place of a disc stamped about every point
of a line), a walk of at most 64 for its reach, and a second evaluation of the update for a
point whose joint update ends under its receiver's bed. The solve holds 20 more bytes a
point while it runs when the sketch has ice (a line's own thickness, the reach's far point
and its length), and four arrays of the network (28 bytes a point) in place of one (8).

## Pictures, and what still reads wrong

**Stage E** (W126, 2026-11-11; the solver's own relief and drainage maps, outside the
repository; no `engine-view` run). The trough test's valley and the firn valley of the basins'
tests, each from stage D's solver and from this one, and the two-range sketch with its ice age
as it was and with a colder one (the line at 600 m, the ice gone one step before the end).
Beside the reels' glacial valleys (r26's level green floor with a river between walls that
rise from its edge to glaciers at the head; r08's sheer-walled valley with a lake on its
floor):

- **The trough's valley**: stage D's map shows the trench, a dark wedge that widens to the
  east edge; this one's is a band of even width, floor between walls, from the junction to the
  edge. At 128 by 96 points the map says no more than the numbers do.
- **The two ranges**: the glaciated valleys are crisper than stage D's, whose swaths smeared
  them into wide pale ribbons, and their floors read as narrow flats between steeper walls.
  **It still reads as a range cut by rivers.** No lake stands in any trough (11 points of a
  metre of water in a quarter of a million), no bowl sits at a valley's head, no ridge crosses
  a valley, and below the ice's reach the valleys are the V's they were. What r26 shows and
  this lacks, in the order a person would say it: the level floor the width of the valley
  (fill in a basin, which here the till buries before a lake can stand in it), the lake, the
  ice at the head, and everything under 32 m (rock, scree, the river's own banks).

**Stage D** (W106, 2026-11-11): **the first pictures of the ice-age ground as the renderer draws
it** (stage C's 1,024-point sketch with its ice age, read at 4 m by a run of no steps and built
as a tile set, both workarounds below; `engine-view --offscreen`, traced, 1,600 by 900, flat
tints and no material pack; eight views in two runs, outside the repository). What they show,
plainly, beside the reels (r03 and r04's valley floors under ribbed walls, r08's U-valley with
sheer granite walls, r22's cirque lake ringed by cliffs, r26 and r46's flat-floored troughs):

- **From a summit and from the air it reads as an eroded range**: branching valleys, sharp
  crests, spurs, a far range with snow on its crest, a green main valley. Those two views hold.
  The solver's relief of the same ground (stage C's `alpine-b`, written then and first read
  now) shows where the ice was as broad pale ribbons in the upper valleys that narrow to V's
  below its reach: wider valleys, and nothing else that says ice.
- **From inside a glaciated valley it does not read as one.** Looking down a side valley from
  its head (the best of the floor views), the section is an open, round-bottomed V between
  smooth walls at 35°: Svensson's parabola, and nothing a person would call a trough. No level
  floor, no step, no lake, no bowl at the head, no ridge across it. The solver's own map said
  the same in numbers: of 2,969 flooded points, 46 lie where the last ice stood.
- **Two of the four views from a valley floor are a blank slope**: at eye height in a valley a
  few hundred metres wide the frame is one wall of one tint, with nothing on it finer than the
  16 m lattice. r03's wall is ribs, ledges and scree; here it is a plane.
- **The main valley's floor is rolling fans**, smooth green domes with the range's front as
  rounded grey cones behind: desert hills, not r03's flat floor with a gravel river under a
  rock wall.
- **The beds are a staircase**: a hard bed's outcrop crosses a slope as a saw-toothed stripe a
  mask texel wide, and on the summit's near slope the 4 m masks are plainly visible as blocks.
- So, in the order a person objects: no rock and no scree at any scale under 16 m (the rock
  solvers' and the material's); no basin, lake, cirque or moraine (the design above, not
  built); no river with banks (the channels solver's); and the fans' domes.

**Stage C** (2026-11-11; the same sketch as stage B's, two ranges about a valley, 1,024 points at
16 m, 1.5 million years, solved by the command, its shaded relief and drainage map outside the
repository; no `engine-view` run was made, so nothing here is of the ground as the renderer
draws it). Beside stage B's map of the same sketch:

- **The valleys branch and bend.** They differ in size, leave the crest at angles, and join
  each other inside the range before they reach its foot; the divide wanders. Stage B's ran
  from crest to foot side by side.
- **The main valley has a river with a width**: a band several points wide where the flow
  divides over its floor, a lake where a fan has dammed it, and a floor of sediment the length
  of it. The piedmont's fans are cones with a few large rivers where there were many parallel
  threads.
- **With an ice age** (the same sketch at 512 points and 32 m, the line at 800 m for 380,000
  years, ended 20,000 years before the end) the upper valleys are wider and softer and their
  heads open; the lower valleys are unchanged.

Beside the reels (r03's valley floor under a ribbed wall with a side valley hanging above it),
what a person still objects to:

- **Most valleys still cross the range the short way.** The comb is reduced by a third by the
  count and is not gone. An uplift that is a line and a kernel will always have a short way;
  what would take it further is a sketch drawn as blocks and faults, or structure that steers
  the rivers (the strike valleys of dipping beds, which a contrast of 2 no longer cuts).
- **A trough has no level floor and no walls.** The ice's valleys are wide parabolas. r03's
  floor is flat for hundreds of metres and its wall stands up from it; that is the ice's flow
  across the section, and it is not here.
- **No cirque, no tarn, no moraine ridge.** Heads are open bowls with no lip; the till is a
  sheet where the ice melted, not a ridge across the valley.
- **The river on its floor is a band of lattice points, not a channel with banks**: at 16 m a
  river is one to five points, and what the picture needs is the channels solver cutting a bed
  of `channel_width` along it.
- **Everything is still smooth below 16 m**, and the hanging valleys have no waterfall: both
  are other solvers'.

**Stage B** (2026-11-10, the same 16 km region of two ranges about a valley, 1,024 points at
16 m, 1.5 million years, outside the repository). The sketch had to change with the model: its
ridges at 3 to 4 mm/yr over K = 3 × 10⁻⁵ solve, with deposition, to a comb of straight valleys
at a mean slope of 0.71 (a river that carries sediment stands twice as steep), and with hard
beds at a contrast of ten to 0.90, a land of cliffs. At 0.9 to 1.6 mm/yr over K = 1.2 × 10⁻⁵, no
regional uplift, a contrast of three and the default variation along the ridges, the relief
shows what stage B added: **a fan at every valley's mouth** on the plains north and south of the
ranges, each with its river's channels spreading over it, coalescing into an apron; **a floor
along the main valley**, on up to 23 m of sediment; crests that rise and fall along their length
and valleys no longer evenly spaced; and the beds as bands that cross the spurs, wider on the
side that falls with the dip. Drawn by `engine-view` (from the 16 m ground read at 4 m, as
stage A's were): from the main valley's floor the frame is a level floor, the smooth lobes of
the fans, banded slopes and a summit that stands off-centre, where stage A's same view was one
blank slope. Beside the reels (r03 and r04) a person still objects that:

- **Everything is smooth.** The fans are domes and the spurs dunes: nothing is finer than the
  lattice, the diffusion has rounded what the rivers cut, and there is one flat material. The
  reels' ground is rock, scree, turf and boulders at every scale below 16 m.
- **The floor has no river.** The channel is one point wide, and reading the ground at 4 m
  loses it; the reels' floors are a river with bars and cut banks. That is the channels
  solver's, from `drainage_area`, `receiver` and `sediment`.
- **A hard bed's outcrop is a saw-tooth**, a point wide where it crosses a slope, where the
  reels' beds are ledges and cliff bands with debris under them.
- **The faces between the gullies are still planes** at the critical slope, and the valleys of
  a range, though uneven now, still run straight down it side by side.
- **The floor is a trough, not a plain**: the two aprons meet along the river at their own
  slopes, and nothing has levelled it but one lake's fill.

**Stage A** (the same day, before deposition). A 16 km region of two ranges about a valley, 1,024 points at 16 m, 1.5 million years
(outside the repository, as every generated image is). The shaded relief is the first picture:
it reads as two eroded ranges, with branching valleys, spurs between them and crests that wander,
which a range of ridge lines with a profile does not. Drawn by `engine-view` from a tile set, from
2.6 km up it still reads as an eroded range; from a crest and from the valley floor it does not
hold. Beside the reference reels (r03 and r04, the alpine valleys), what a person would object to:

- **The faces are planes.** Nearly every slope stands at the critical angle with nothing on it
  finer than the 16 m lattice, and under one flat material that reads as dunes or folded paper,
  where the reels' walls are ribs, gullies, ledges and scree at every scale. The solver's part of
  that is the lattice (a finer one, or a refine of this ground at 4 m, which is minutes to tens of
  minutes) and the missing processes; the rest is the rock solvers' and the terrain material's.
- **No flat valley floor, no fans, no scree aprons.** Both reels stand on an alluvial floor under
  walls with pale debris cones at the foot of every gully. Nothing is deposited here, so every
  valley is a V to its mouth.
- **A channel is one point wide and follows the lattice**, and the bilinear surface between the
  points of a diagonal channel is a chain of pits (117,731 flooded points appeared when the 16 m
  ground was read at 4 m). A channel needs the channels solver's cut, not the heightfield's
  resolution.
- **The valleys of a straight range are evenly spaced and parallel**, because the uplift along a
  ridge line is uniform. A sketch with uneven rates, a raster or resistant bands breaks it; the
  model does not do so by itself.
- **Far peaks still read as cones**: seen from the valley floor the crest's summits are small
  symmetric pyramids, since a threshold slope is the same angle on every side. Asymmetry in the
  reels comes from structure (dipping beds) and ice, neither of which is here.
- **A depression's floor keeps straight lines**: where a rising ridge dams a valley the lake that
  forms is routed along the flood's order, and its floor shows it.

**The tile set could not build a heightfield whose spacing is one of its own far levels', and
now can.** Stages A to D met a 16 m ground refused at open (`16000.tiles` "is not this level's
archive": the finest level and the far chain's 16 m level were both named by their spacing), and
their pictures were taken from the same ground read at 4 m by a landform run of no steps. Since
2026-11-11 a far level's archive has a name of its own (the tile set's change, merged that
morning), so a solved ground builds at the spacing it was solved at and the 4 m read is no
longer needed; no picture has yet been taken that way.
