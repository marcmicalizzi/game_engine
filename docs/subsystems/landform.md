# landform (domain, capability)

**Purpose.** An offline landform solver (roadmap W95, stage A of G36's erosion and hydrology): a
**region sketch** — the lattice, what each edge is, outlets, uplift, how hard the rock is, a seed,
the time and the step, and optionally a ground to start from — becomes an **eroded ground**: an
authored heightfield the tile set builds as it is ([tileset](tileset.md#the-source-engineterrainheightfield1)),
in whole millimetres, and on the same lattice **the fields the later solvers read** (drainage
area, receiver, slope, erosion rate, fill). Mountains are the largest subject in the owner's
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
5. **Incision** (the stream-power law, implicit along the flow tree, Braun and Willett 2013): in
   flood order, a point above its receiver and above the sea moves to (z + F z_r) / (1 + F),
   F = K Δt √N over the distance in cells. A receiver below the sea counts as the sea's level.
6. **Hillslopes**: explicit linear diffusion in as many substeps as stability needs, then the
   slope limiter in flood order.

After the last step the limiter runs to convergence, the ground is rounded to whole millimetres
once, and **the fields are routed again from exactly those millimetres**, so what the field file
says is true of the heightfield beside it and not of the step before.

### The slope limiter

No slope stands above its point's critical slope (Roering, Kirchner and Dietrich 1999's law has
flux growing without bound there). The limiter lowers a point to the largest height that keeps
the gradient's length at the critical slope: from the lower neighbour along x and the lower along
z it is the upwind step of the eikonal equation, (a + b + √(2f² − (a − b)²)) / 2 with f the
critical slope times the spacing, and the four diagonal neighbours then hold the slope along a
diagonal to the same bound. **Why not a limit along the eight directions alone**: that makes a
failed slope an eight-sided pyramid with creases along the lattice, which is what the first
relief showed; the eikonal step makes it a cone. What the limiter removes leaves the model (no
deposition in stage A).

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
  flood, the accumulation, the incision and the limiter walk an order and are serial.
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

`engine.landform.fields/1` (`engine.landform.Fields`). What a later solver (channels, scree, soil,
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
  | `erosion_rate` | i32 | micrometres a year removed by the last step | zero for a run of no steps and at held points |
  | `fill` | u32 | millimetres under the depression's spill | zero on draining ground; a tarn's depth |

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
deposition (terraces, fans, deltas, scree zones, and a flat valley floor); soil depth; snow;
reading the formation field's dipping beds for resistance; a general m and n; neighbouring
regions solved so a shared edge agrees; the flow tree's levels in parallel; baking far-terrain
maps from near output; anything in the editor. The limits of the model as it stands are on the
grounding page.

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

## Public API

`include/domain/landform/landform.h`: `validate_sketch`, `solve`, `route`, `read_sketch`,
`read_start_field`, `write_landform`, `read_fields`, `read_field_window`, `shaded_relief`.

## Depends on

`base`, `containers`, `math`, `hash`, `json`, `schema`, `io`, `jobs`, `landform_schemas`.

## Testing

`tools/dev.ps1 test -Filter "^landform$"`. All CPU; the whole suite is about 4.5 s in `msvc-debug`.

| Test | Holds | Fails when |
|---|---|---|
| the golden sketch solves to the pinned bytes | the hash of the heightfield and of each field, for a sketch that uses every part of the model; pinned on MSVC, reproduced by Clang and GCC | any toolchain, baseline or change of arithmetic moves a byte |
| one, four and sixteen workers | the serial solve's bytes from each | a parallel loop reads what another chunk writes |
| the flood breaks ties by cell index | a flat's order, receivers and ε, a pit's fill | the queue's order is not total, or a diagonal beats a cardinal of equal fall |
| every point drains | receivers' paths end at held points; held areas sum to the lattice | the flood or the receivers leave a point stranded |
| a uniform uplift's steady state | the mean erosion rate within 1% of the uplift; slope against area regresses to −0.5 within 0.02 (measured 0.4999); Hack's exponent in 0.5 to 0.7 (measured 0.616) | the incision is not the stream-power law, or the routing does not make a network |
| the ground meets an open edge without a step | the first row within 2 m, no bend above 0.2 a row; and that without the taper both fail | the border taper is lost |
| the sea bed takes part | no neighbours above the critical slope on land, across the coast or on the bed; a 370 m underwater wall gone | the limiter or the routing stops at the waterline |
| a starting heightfield is refined | the 800 m low-pass moves under a twentieth of the relief (measured 9.5 m of 407 m) while single points move 30 m; no steps leaves the heights untouched | a refine replaces its ground |
| the files round-trip | the heightfield read back whole millimetres; a field's window equals memory; a window past the edge is refused | the formats drift |
| a sketch outside a range is refused | six refusals, each naming its field | validation is lost |

## Performance notes

Measured by `tools/dev.ps1 bench -Filter 'landform.*'`; the table is filled by the stage that
measures (below). The solve holds about 54 bytes a point while it runs (the ground, the lift and
hardness fields, the filled surface, the order, the receivers, the areas and a scratch surface)
and nothing afterwards.
