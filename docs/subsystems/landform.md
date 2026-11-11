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
| `uplift` | `regional_mm_per_yr`, plus `ridges` (a polyline, `rate_mm_per_yr` on it, falling to nothing `reach_m` away as (1 − (r/reach)²)²; the largest of overlapping ridges counts), plus a coarse `raster` read bilinearly over the region | 0 to 100 mm/yr |
| `rock` | `resistance` 0 to 1 (the world formation's ordering), uniform or by `bands` of elevation (`top_m` ascending); `contrast`, the erodibility at 0 over the erodibility at 1; `variation` and `variation_cells`, a seeded variation of the erodibility | 0; 10; ±25% over 16 cells |
| `erodibility_per_yr` | K of the stream-power law at m = 1/2, n = 1 | 10⁻⁸ to 10⁻², default 5 × 10⁻⁶ |
| `channel_area_m2` | The drainage area a channel begins at: a point draining less is hillslope, lowered by diffusion and the critical slope alone | 0 to 10⁹; default 0, the rivers cut at every point |
| `deposition` | G, the deposition coefficient ([deposition](#deposition)): a river lays down G / N of the sediment that reaches a point draining N cells. 0 is the detachment-limited law: nothing settles anywhere, standing water included | 0 to 10, default 1 |
| `sediment_erodibility` | The erodibility of sediment over `erodibility_per_yr` | 1 to 100, default 2 |
| `delta_share` | The share of what reaches a lake's water that settles at the river's mouth, up to the water's level; the rest levels the lake's floor | 0 to 1, default 0.5 |
| `diffusivity_m2_per_yr` | Hillslope diffusivity | 0 to 1, default 0.005 |
| `critical_slope`, `critical_slope_resistant` | The gradient no slope stands above, for rock of resistance 0 and 1 | 0.2 to 4; 0.9 and 1.6 |
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
4. **Drainage area**: in reverse flood order, a point adds its cells to its receiver's.
5. **The rivers**: erosion and deposition together, implicit along the flow tree
   ([deposition](#deposition)): a reach above its receiver and above the sea moves to
   (z + d + F z_r) / (1 + F), F = K Δt √N over the distance in cells, d what the river lays down
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
  receiver, with that deposit in the numerator. It ends when no height moves by a tenth of a
  millimetre (`k_river_tolerance_m`), or after 64 sweeps. **Why not the cut first and the
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
  and leave the model (0.7% of what was eroded in the golden sketch).

**What it makes, and what it does not.** A valley whose mouth is dammed, or whose trunk has
aggraded, fills to a floor that is level across and widens downstream (the test: 135 m wide
where the fill begins and 335 m at the mouth, within 6.4 m across). A range's rivers lay a plain
of coalesced fans at its foot, each river crossing it on its own grade, as Yuan et al. report
for this model; a single massif beside a plain leaves a fan whose contours bow away from it
(the test). **A fan is not a cone at the scale of one mouth**: flow has one receiver a point,
so a river's deposit is laid along one path and the cone is what many steps of avulsion
average to. **Small rivers lay their load down within a few points** (G / N is large where N
is small), so the foot of every slope takes a little; that is colluvium, not scree: nothing
here stands at an angle of repose. And **a valley inside a range that is rising everywhere has
no floor at steady state**, because every point of it is losing ground; a floor needs a base
level that rose or an uplift that stopped, which a sketch gives by where it puts its uplift and
its outlets.

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
  and hardness fields, the receivers, the diffusion (a Jacobi step) and the output fields. The
  flood, the accumulation, the rivers' sweeps, the lakes and the limiter walk an order and are
  serial.
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

`engine.landform.fields/2` (`engine.landform.Fields`; version 2 added `sediment`). What a later solver (channels, scree, soil,
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

Each follows, and none is here: the glacial mode (troughs, cirques, overdeepened basins);
**resistance from the formation field's dipping beds**, rising with the rock (stage B's second
part, not reached: the elevation bands stand until it replaces them); **uplift that varies along
a ridge line, and defaults grounded against published slope distributions** (the third, not
reached); **a flood that keeps ground with no depression off the heap** (the fourth, not
reached: the flood is still nearly the whole step); flow that divides, so one mouth's fan is a
cone and a channel on its plain is more than a point wide; scree at an angle of repose and
terraces; soil depth; snow; a general m and n; neighbouring regions solved so a shared edge
agrees; the flow tree's levels in parallel; baking far-terrain maps from near output; anything
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

`include/domain/landform/landform.h`: `validate_sketch`, `solve`, `route`, `read_sketch`,
`read_start_field`, `write_landform`, `read_fields`, `read_field_window`, `shaded_relief`.

## Depends on

`base`, `containers`, `math`, `hash`, `json`, `schema`, `io`, `jobs`, `landform_schemas`.

## Testing

`tools/dev.ps1 test -Filter "^landform$"`. All CPU; the whole suite is about 11 s in `msvc-debug` (stage B's five cases are 3.7 s of it, and the sweeps added 3 s to stage A's).

| Test | Holds | Fails when |
|---|---|---|
| the golden sketch solves to the pinned bytes | the hash of the heightfield and of each field, for a sketch that uses every part of the model; pinned on MSVC, reproduced by Clang and GCC | any toolchain, baseline or change of arithmetic moves a byte |
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
| a dammed valley fills to a floor | a V-shaped valley whose mouth is held 90 m above its bed: the run of fill of 5 m or more about the axis is 5 points or more wide in the lower third, more than 1.5 times the upper third's (measured 6.7 against 2.7), level across within 8 m where the V fell 87 m (measured 6.4 m), over 50 m of fill | nothing is laid down, or a lake is filled along the flood's order |
| a massif's rivers leave a fan on the plain | 400 m out from a massif's foot the ground stands over 3 m up opposite it (measured 4.3 m), a metre or more lower 700 m to either side (2.8 and 2.6 m), and under a metre 1,700 m away (0.9 m) | the load is not laid where a river leaves the range |
| a lake's floor fills level, and its mouth builds a delta | a basin fed for ten thousand years: its floor away from the mouths level within 50 mm (measured 0 and 3 mm) under water still metres deep; with `delta_share` 0.5 the shelf under the mouths holds a quarter more sediment (measured twice) and the floor stands lower | the fill follows the flood's order, or the mouths get nothing |

## Performance notes

`engine_landform_bench` (`tools/dev.ps1 bench -Filter 'landform.*'`), `msvc-release`, 2026-11-10,
an i9-10980XE, the solve on four workers. **The machine was busy** (`--wait-quiet=120` gave up:
other processes used 63% of the CPU at the start and 75% at the end, `machine_state` in the
bench's JSON), so every figure is an upper bound.

| Row | What | Median | A point |
|---|---|---|---|
| `landform.solve/2048` | **a 2,048 by 2,048 sketch at 16 m solved to 100,000 years in twenty steps** | **27.1 s** | 323 ns a step |
| `landform.solve/1024` | the same at 1,024 | 4.91 s | 234 ns a step |
| `landform.solve/256` | the same at 256 | 0.218 s | 166 ns a step |
| `landform.route/2048` | the flood, the receivers and the area of one ground | 1.27 s | 302 ns |
| `landform.route/1024` | | 0.153 s | 146 ns |
| `landform.route/256` | | 6.8 ms | 104 ns |
| `landform.area_power/0` | √A over a million areas, as the solver does it | 1.66 ms | 1.7 ns |
| `landform.area_power/1` | `det::exp(0.45 * det::log(A))`, a general exponent | 69.3 ms | 69 ns |

So a million years of a 2,048-point region at 5,000-year steps is about four and a half minutes,
and a 1,024-point region solved for 1.5 million years (the picture's) took 75 s. **The flood is
nearly the whole step** (1.27 s of 1.35 s at 2,048), and its cost a point triples from 256 to
2,048 points a side: it visits the lattice in order of height, which is no order of memory, so at
four million points every visit misses the cache. What follows from that is on the "Not stage A"
list: routing ground with no depressions from its own slopes in memory order and flooding only
the pits, and the flow tree's levels in parallel (Barnes 2019). A general exponent m would add
69 ns a point a step, a fifth of today's step; m = 1/2 costs 1.7 ns.

The solve holds about 90 bytes a point while it runs (stage A's 54: the ground, the lift and
hardness fields, the filled surface, the order, the receivers, the areas and a scratch surface;
and stage B's 36: the sediment, the rivers' iterate, the shed a step carries to the next, the
tapered G and each point's sill): 378 MB at 2,048 points a side, 1.5 GB at 4,093. **The bench's
table above is stage A's**: it was not run again after deposition (the machine had other rows'
builds on it), and a step now adds the rivers' sweeps, two walks of the order each, where it had
one; in `msvc-debug` the golden sketch went from 0.31 s to 0.54 s. Nothing is resident afterwards, nothing scales with the world's
size at run time, and there are no triangles and no per-tile cost: the tile set's are the
heightfield's.

## Pictures, and what still reads wrong

2026-11-10, a 16 km region of two ranges about a valley, 1,024 points at 16 m, 1.5 million years
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

**The tile set cannot build a heightfield whose spacing is one of its own far levels'.** The 16 m
ground was refused at open (`16000.tiles` "is not this level's archive"): the finest level and
the far chain's 16 m level are both named by their spacing. The pictures were taken from the same
ground read at 4 m by a landform run of no steps, which builds (as the 1 m grounds of the test
scenes do). A spacing of 8, 32, 64, 128 or 256 m, the default chain's other levels, would
presumably collide the same way; 16 m is the one that was tried. That is the tile set's to fix,
and a row.
