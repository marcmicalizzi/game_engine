# water_state (domain)

**Purpose.** The shallow-water equations as a finite-volume step on a regular grid, on the CPU, in
`double`: the reference every later form of the engine's simulated water is held to (roadmap W173,
stage 1, W184). A rectangle of square cells carries depth and the two momenta, the ground's store and
a few carried quantities; a step moves them over a bed the caller hands in at every step, with
wetting and drying, bed friction and sources, and gives out per cell the speed, the stress at the bed
and the rate of dissipation. It draws nothing, holds no world position, and is not the shipped path:
the card's `float` step (W175) and whatever is cut down from it for the frame are checked against
these cases within stated bounds. The grounding is [water-state-grounding](../experiments/water-state-grounding.md);
the decision that the engine's water carries a state is W173's.

**Why this shape.** The full equations are the only set the grounding page read that holds a pool's
backwater, a bore and a drying front together and keeps a lake at rest
([§7](../experiments/water-state-grounding.md#7-a-reading-for-the-design-kept-apart-from-the-sources)); the kinematic and local
inertial waves cannot pool or cannot drain, the diffusion wave's explicit step collapses on flat
water. Among the full equations' schemes the hydrostatic reconstruction (Audusse et al.) was taken
over the central-upwind scheme with Bollermann et al.'s wet/dry reconstruction because at first
order it is a few lines over any flux of the system without a bed, keeps the depth non-negative under
the ordinary step limit with no draining-time bookkeeping, and puts the bed only into a
face's reconstruction, so a bed that changes between steps needs nothing recomputed (below, "The
bed"). It is first order in space and time: W184 takes first order through every case first, and second
order only after. `double` because the reference is the answer, not the budget: the round-off it leaves
is the floor a `float` step is measured against, and its footprint is stated, not minimised.

**Owned data.** The state of one rectangle: depth, momenta, the ground's store and the carried
quantities, and the ledger of what entered and left it. The bed, the friction and the sources are
the caller's and are read, never kept.

**Public API.** `include/domain/water_state/water_state.h`: `Config`, `Inputs`, `Ledger`, `Solver`.

**Depends on.** `base`, `containers`, `math` (the deterministic `exp` and `log`), `jobs`.

## The state's layout

Structure of arrays, one `double` per cell in each, cell (x, y) at index `y * nx + x`, x east, y
north, cells `cell_m` square:

| array | what | unit |
|---|---|---|
| `depth()` | h | m |
| `momentum_x()`, `momentum_y()` | hu, hv | m²/s |
| `ground()` | water the ground's store holds, as a depth | m |
| `carried(k)`, k < `Config::carried` ≤ 4 | amount of quantity k per area; its concentration is `carried(k) / h` | unit · m |

The outputs of the last step, the same layout: `speed()` (m/s), `bed_stress_x()`/`bed_stress_y()`
(Pa, the friction law's), `dissipation()` (W/m²). The scratch the step needs — the velocities, the
mass, normal-momentum (twice, one per side), transverse-momentum and carried fluxes of every x face
(`nx + 1` a row) and y face (`ny + 1` rows of `nx`), and the per-row maxima and sums — is the
solver's, allocated once at construction; a step allocates nothing.

**Footprint** (*stated, not minimised*): the state is 32 + 8k bytes a cell, the outputs 32, the
scratch 16 for the velocities and 2 × (32 + 8k) for the faces, about **144 + 24k bytes a cell**, 192
with two carried quantities. A `float` step keeps what it keeps; this one is not its model for memory.

## The step

```cpp
water_state::Solver solver(config);                  // allocates everything
solver.depth()[i] = ...;                             // the initial state, written in place
water_state::Inputs in{bed, manning, sources};       // spans of nx * ny doubles; manning, sources may be empty
f64 dt = solver.step(in, max_dt, jobs);              // one step of min(stable, max_dt) s; 0 if the inputs do not fit
u64 n  = solver.advance(in, duration_s, jobs);       // to exactly duration_s, the last step shortened
```

`jobs` may be null (one thread); with a `jobs::JobSystem` the rows are dealt out
`Config::rows_per_job` at a time, which does not change a byte.

**The scheme.** Each step, in this order:

1. **Velocities and the step's length.** u = hu/h and v = hv/h, and zero where h is under
   `dry_depth_m` (Bollermann, Chen, Kurganov and Noelle 2014, section 2: ε = 10⁻⁹). The step is
   Δt = min(`max_dt`, C Δx / (max(|u| + c) + max(|v| + c))), c = √(g h), C = `cfl` = 0.45.
2. **The faces.** At a face between cells L and R (or a cell and its ghost), the hydrostatic
   reconstruction (Audusse, Bouchut, Bristeau and Sainte-Marie 2016, (3.3)–(3.5)): the face's bed
   z* = max(z_L, z_R), each side's depth h*_L = max(0, h_L + z_L − z*), h*_R = max(0, h_R + z_R − z*),
   and each side's state (h*, h* u, h* v) with its own cell's velocities. Between the two states the
   flux of the system without a bed, the central-upwind flux of Bollermann et al. (2.8),
   H = (a⁺ F(U⁻) − a⁻ F(U⁺)) / (a⁺ − a⁻) + a⁺ a⁻ / (a⁺ − a⁻) (U⁺ − U⁻), with the one-sided speeds
   a⁺ = max(u⁺ + √(g h⁺), u⁻ + √(g h⁻), 0) and a⁻ = min(u⁺ − √(g h⁺), u⁻ − √(g h⁻), 0) of (2.13)–(2.14),
   applied to the transverse momentum as its third component. x faces use u as the normal velocity,
   y faces v. Two identical states give the exact flux of that state (what the formula equals in
   exact arithmetic), which is what keeps a lake at rest at rest to the bit.
3. **The update.** The hydrostatic reconstruction's update (Audusse et al. 2016, (3.1)–(3.2)):
   U' = U − Δt/Δx (F_{i+1/2−} − F_{i−1/2+}) in each direction, where the normal momentum's flux on each
   side is H plus g/2 (h_i² − h*²) for that side's h*. The g/2 h_i² of the two faces of one cell cancel
   exactly, so each face stores H − g/2 h*_L² and H − g/2 h*_R² and the cell takes the difference; the
   bed enters only here, there is no other bed-slope term at first order.
4. **Clamp and sources.** A depth a rounding error below zero is set to zero and the water that
   added is counted (`Ledger::roundoff_m3`). Then the source: h += Δt s for s ≥ 0, and for s < 0 the
   lesser of Δt |s| and what the cell holds, counted in `Ledger::source_m3`.
5. **Friction.** Manning's law as the grounding page writes it,
   **τ**_b/ρ = g n² |**u**| **u** / h^(1/3) ([§1](../experiments/water-state-grounding.md#the-shallow-water-equations)),
   taken implicitly in its coefficient (*derived* here: a linearly implicit Euler step of
   d(h**u**)/dt = −g n² |**u**| (h**u**) / h^(4/3) with |**u**| held at its value after the update):
   (h**u**)' = (h**u**)* / (1 + Δt g n² |**u***| / h^(4/3)). It can stop a flow and never reverse
   one, at any Δt. h^(1/3) is `det::exp(det::log(h) / 3)`.
6. **Dry cells and outputs.** Under `dry_depth_m` the momenta are set to zero. The outputs are the
   final state's: the speed, the bed stress ρ g n² |**u**| **u** / h^(1/3), and the dissipation
   ρ g n² |**u**|³ / h^(1/3), the friction's work on the flow. **What the dissipation does not hold
   yet**: the energy the scheme loses at a bore or a jump, which is what the foam reads
   ([§5](../experiments/water-state-grounding.md#5-foam-as-a-consequence)); it is the next stage's,
   with the carried quantities' sources.

**The step's length**, and why depth stays non-negative (*derived*). At a face between reconstructed
states L and R, H_h − a⁺ h_L = [a⁺ h_L (u_L − a⁺) − a⁻ h_R (u_R − a⁺)] / (a⁺ − a⁻) ≤ 0, since a⁺ ≥ u_L,
a⁺ ≥ u_R and a⁻ ≤ 0; likewise H_h ≥ a⁻ h_R. So what leaves a cell through each of its four faces is at
most the face's speed times that side's reconstructed depth, which the reconstruction keeps at or under
the cell's own, and every face speed is at most max(|u| + c) (x) or max(|v| + c) (y) over the cells.
Then h' ≥ h (1 − 2 Δt/Δx (A + B)), non-negative for Δt (A + B)/Δx ≤ ½; C = 0.45 leaves room under it.
Positivity of the hydrostatic reconstruction under the homogeneous solver's step condition is
Audusse et al. (2004)'s result, read here through the 2016 paper; this is the same argument for the
flux used.

**Edges.** Each of the four is `closed` (a ghost mirroring the edge cell with its normal velocity
negated: the mass flux through it is zero to the bit) or `open` (a ghost copying the edge cell: what
the edge cell carries out leaves, and what crosses is summed into `Ledger::open_edge_out_m3`, out
positive).

**The bed.** `Inputs::bed_m` is read at every step and nothing derived from it is kept, so a step may
find it changed; the state does not move with it: each cell keeps its depth (its volume), and its
surface moves with the bed under it. The case that holds this is the next stage's.

**The ledger.** Summed per row and then over rows in row order: surface + ground + open_edge_out =
initial + source + roundoff, to round-off. The ground's store is in the state and the ledger; its
infiltration law is the next stage's.

## The cases

`domain/water_state/tests/water_state_tests.cpp`, each with its bound and why. Measured on MSVC
14.51, `msvc-debug`, 2026-11-25:

| case | what | bound | measured |
|---|---|---|---|
| `a lake at rest over a bed with dry islands stays exactly at rest` | 64 × 48 cells of 0.5 m, a surface at 1 m over a bed of 1/64 steps with islands at and above it, 20 s (more than 500 steps) | **zero**: not one bit of depth or momentum moves (every height is a binary fraction, so every reconstruction is exact) | 0 cells moved |
| `a lake at rest over an inexact bed stays at rest to round-off` | the same at 0.9 m over a bed no binary fraction holds | surface within 1e-13 m, no speed over 1e-11 m/s (a thousand times the ulp it starts from) | 1.1e-16 m, 1.9e-15 m/s |
| `a dam breaks on a wet bed as Stoker's solution, first order` | SWASHES §4.1.1: 10 m, dam at 5 m, 5 mm on 1 mm, 6 s; 200, 400, 800 cells | L1 order ≥ 0.5 between each pair (a first-order scheme's floor at a shock); the 800-cell relative L1 error ≤ 0.0045 (measured and 15 %); volume to 1e-12 | 0.01197, 0.00693, 0.00394; orders 0.79, 0.81 |
| `a dam breaks on a dry bed as Ritter's solution, first order` | SWASHES §4.1.2: the same onto a dry bed | the same order floor; 800-cell error ≤ 0.0058 (measured and 15 %) | 0.01228, 0.00798, 0.00502; orders 0.62, 0.67 |
| `water is counted to the last cell through wetting, drying and an open edge` | 48 × 40 cells of 0.5 m, a block of 0.3 m released down a rough 2 % slope, Manning n 0.03, a spring and a drain, out through an open east edge, 60 s | balance within 1e-12 of the water involved (each of some 10⁵ additions rounds by half an ulp); no negative depth; cells wetted and the drain's cells dried to zero | balance 1.4e-13 m³ of 36 m³; 1,438 cells wetted; 824 steps |
| `a step refuses inputs that do not fit the grid` | a bed of the wrong size | nothing changes, the step returns 0 | |

The dry bed's error is larger and converges more slowly than the wet one's: at the front the depth
falls to zero as the fan's square and the first-order scheme smears it; a second-order form has these
two numbers to beat.

**Still to come in W184** (2026-11-25): the changed bed (a lake at rest, the bed raised under part of
it, the volume the same and at rest again once the wave has gone); the ground's store and its law; the
carried quantities' sources and decay, with the bore's dissipation feeding the foam, two carried at
once; Thacker's bowl; a source filling a hollow to its lip and spilling at critical depth there (the
case W176 adds the steady solve's level to); the thread and toolchain cases; the bench's numbers.

## Determinism

`k_determinism` is `hashed`: a step is a function of the state's bytes, the inputs' bytes and Δt.
Only `+ − × ÷` and `sqrt` decide anything, the friction's cube root is `core/math/deterministic.h`'s
`exp` and `log`, the step's length is a maximum (the same in any order), and every sum over cells is
taken per row and then in row order, so the same inputs give the same bytes at any thread count; the
tree compiles with contraction off ([ADR-0035](../adr/0035-no-floating-point-contraction.md)), so they
are the same bytes on MSVC, GCC and Clang. `state_hash()` is FNV-1a over the state's arrays; the
cases that pin it at 1 and N threads and across the three toolchains are the next stage's.

## Testing

`tools/dev.ps1 test -Filter '^water_state' -NoDriver`. Benchmarks: `tools/dev.ps1 bench -Filter 'water_state.*'`
(one step of a 64², 256² and 512² grid, cells a second).

## Sources, and how far each was read

Read on 2026-11-25 through the fetch tool, as HTML renderings; equations only were taken, no text.

| source | what is taken | read | licence |
|---|---|---|---|
| Audusse, Bouchut, Bristeau, Sainte-Marie (2016), *Math. Comp.* 85, arXiv 1409.3825 | the hydrostatic reconstruction: (3.1) the update, (3.2) the two sides' fluxes and their corrections, (3.3) the reconstructed states, (3.4) the depths, (3.5) the face's bed | read (ar5iv rendering, the scheme's section) | arXiv's; not shown on the rendering, nothing copied but the equations |
| Audusse, Bouchut, Bristeau, Klein, Perthame (2004), *SIAM J. Sci. Comput.* 25, 2050–2065 | the scheme's origin and its positivity under the homogeneous solver's step condition | through the 2016 paper and the search engine's summary of the abstract; the paper itself not reached as text | not read |
| Bollermann, Chen, Kurganov, Noelle (2014), arXiv 1412.3580 | the central-upwind flux (2.8), the one-sided speeds (2.13)–(2.14), the step condition (2.15), ε = 10⁻⁹ | read (ar5iv rendering) | arXiv's; not shown on the rendering |
| Delestre et al., SWASHES, arXiv 1110.0288 | Stoker's (§4.1.1) and Ritter's (§4.1.2) solutions and their parameters; Thacker's bowl (§4.2.2) and the lakes at rest (§3.1) for the next stage | read (ar5iv rendering, those sections) | the paper's text arXiv's; its code is CeCILL-V2 and was **not** read or used |

## The design: a state in a world

(2026-11-25, roadmap W173; the decision is [ADR-0080](../adr/0080-water-carries-a-state.md), proposed; the sources are [water-state-grounding](../experiments/water-state-grounding.md). This section is the shape the stages build towards. What this module holds today is the reference alone, described above; nothing below is built until its stage's row says so, and a figure marked *derived* is this page's arithmetic from read numbers, to be replaced by a measurement.)

### What a cell holds

| quantity | unit | changes | in the reference | on the card, the first form |
|---|---|---|---|---|
| depth h | m | every step | double | 32-bit float |
| momentum hu, hv | m²/s | every step | double | two 32-bit floats |
| the ground's store | m of water held | on the store's clock | double | 32-bit float |
| carried scalars (the foam first; suspended sediment next) | amount per area, as the reference carries them | every step | double each | 32-bit float each, one to start |
| the bed's mobile layer, a working copy | m over the built ground | on the bed's clock | double | 32-bit float |
| the built ground's height | m from the domain's datum | only by an edit of the ground (W183) | double | 32-bit float |
| the bed's roughness | s m^(−1/3) | when the bed's form does | double | 32-bit float |
| the soil's class | an index | never | per cell | 8 bits |

**Single precision throughout in the first form** (ADR-0080, decision 3): 24 bytes a cell that change (with one carried scalar) and about 12 that do not, and a step that reads one copy of the changing state and writes another holds two, so **about 60 bytes a cell on the card** (*derived*). Smaller number types are a shortcut, and two of them are known not to work as they stand: a loam's take in one sixtieth of a second is 6 × 10⁻⁸ m ([grounding §3](../experiments/water-state-grounding.md#3-the-ground-takes-water-until-it-is-saturated), 13.2 mm/h), under the last bit of a 16-bit field over any useful range and under the last bit of a single-precision depth of one metre (1.2 × 10⁻⁷ m), so a naive step would add to the store what it never took from the water. The ground's take and the bed's change are therefore applied on a coarser clock of their own, each application far above the field's resolution, or through a compensated sum; W175 holds "a pond over loam loses what the store gains" and "a bed that moves slowly moves" as cases before anything leans on either.

The surface a cell draws and balances is its depth plus its bed, and **the bed is a height from one datum for the whole domain**, so that a lake at rest stays at rest in single precision wherever in the world the domain stands and across every seam inside it; whether it does is the first thing W175 measures.

### Domains and tiles

**A domain is where a scene says water may have a state**: a pond's hollow, a reach of a stream with its banks, a beach's surf zone out to where the water is shallow against the wavelength, a place an author expects a dam. It is the scene's data. It names its cell size, one of four classes a factor of two apart (**0.25 m** for a stream's reach, **0.5 m** for a pool and a surf zone, **1 m and 2 m** for open water), and whether it is persistent.

**A tile is 64 cells a side** (16 m, 32 m, 64 m or 128 m by class), aligned to the world's 64 m cell grid, with positions local to the domain (ADR-0053). It is the unit that is allocated, wakes, sleeps and is counted, and a domain allocates only the tiles its footprint touches, so a narrow corridor does not pay for the square round it. At 60 bytes a cell a tile is 0.25 MB (*derived*). For scale, counted in tiles (*derived*):

| domain | class | tiles | cells | memory |
|---|---|---|---|---|
| a pond 100 m across | 0.5 m | 16 to 25 | 66,000 to 102,000 | 4 to 6 MB |
| a stream's corridor 10 m wide, 1 km long | 0.25 m | 63 along an axis to about 140 on a diagonal | 258,000 to 573,000 | 15 to 34 MB |
| a lake 500 m square | 1 m | 64 to 81 | 262,000 to 332,000 | 16 to 20 MB |
| a kilometre of beach, a 200 m surf zone | 0.5 m | 224 to 264 | 918,000 to 1,081,000 | 55 to 65 MB |

Memory is not what limits a domain on any card the engine runs on; the step's time and the moving surface's are.

**The edge of a domain is a seam with the far field, and water is counted across it.** Where a stream enters, the baked discharge is the inflow; where it leaves, what crosses is what the far field continues with; along a dry edge nothing crosses, and water that reaches a dry edge is reported, not lost. The difference between what a domain passed on and what the baked answer assumed is kept in a **ledger for that link**. The baked far field is steady and cannot take a transient, so the ledger's balance is spent in one place only: as a correction to the inflow of the next tile that wakes downstream on the same link, released over a stated time. Until then it is a count that says how much the picture and the books differ, and it is reported; it is never silently dropped. The bed has the same seam (W178's grounding): a baked river's bed is in transport equilibrium, not at rest, so a domain whose bed may move is fed the baked transport at its inlet and hands the excess back at its outlet, or it scours from its own edge.

### Level of detail

In the order the stages build it: **one domain, tiled from the start, at one cell size**; then **whole tiles asleep or awake** by need and by the observers' distance ([ADR-0010](../adr/0010-deterministic-sim-and-lod-contract.md)), a sleeping tile drawing its baked answer; then **nested tiles two to one**, a far tile at twice the cell, the coarse face receiving exactly the sum of what its fine faces sent over all of the fine side's sub-steps; and beyond every tile the baked far field. A tile retires when its state has settled to the baked answer within the bound W176's check states; an unsettled tile retires only where no observer can see it, its difference going to the ledger. Stepping far tiles less often and the cheaper waves where they are provably enough ([grounding §1](../experiments/water-state-grounding.md#what-hydrology-uses-instead-three-cheaper-waves)) are shortcuts and come after, each with its switch.

**Steep ground stays baked.** What was read: a uniform flow on Manning friction breaks into roll waves past a Froude number of 1.5, which is physics a solver should show and cannot hold still; the wet and dry schemes read were tested on gentle slopes only (about 0.05); and a decimetre grid resolves neither a step's jet nor its clasts. What was not read: the slope at which the hydrostatic assumption itself fails (two sources reached only as search summaries, so no number is used here). So the rule is by the steady solve's own classing of reaches: a step-pool or cascade reach keeps its reach-averaged answer, and its steps enter a live pool below them as sources of momentum, air and foam. The pools, the confluences, the water behind a dam and the shore are where the state runs.

### Tiers

| tier | what runs | what it costs a scene with no tile awake |
|---|---|---|
| baked | the steady solve and the closed forms, as today | nothing: no pass, no pool, no branch |
| mixed | baked everywhere; the state in tiles something woke (an event, a changed source, an observer near a marked domain), started from the baked answer and retired to it | nothing until a tile wakes |
| live | the state in every declared domain within the observers' range | the awake tiles' step and surface |

The tier is the game's setting; the cell classes, the range in tiles and the count of carried scalars scale it to a card. No part of the design assumes ray tracing or mesh shaders, and a host with no device runs baked.

### The clocks, and what a tick costs

The state advances with the simulation's ticks on **the world clock** (ADR-0056, ADR-0010). A tick is cut into the sub-steps the reference's own rule asks, Δt ≤ 0.45 Δx / (max(|u| + c) + max(|v| + c)), so the two can be compared step for step. By that rule (*derived*; c = √(g h)): a stream 5 cm deep at 1 m/s on 0.25 m cells may step 47 ms, a still pool a metre deep on 0.5 m cells 36 ms, a surf zone 3 m deep at 2 m/s on 0.5 m cells 17.5 ms, the same zone out to 10 m of water 11 ms, and a 2 m fall's foot on 0.25 m cells 14.6 ms. So **a stream and a pool take one sub-step a 60 Hz tick, a surf zone one or two, and deep or fast water more**. The count is taken from the tile's own fastest speed by a reduction in a fixed order, and it is reported; a tick's cost is cells × sub-steps × the world's rate and **follows the water**, which is the truth of an explicit step. A ceiling on the count is a shortcut, not part of the first form. Whether the count may be taken from the tick before (a read-back is a tick late) is W191's to show against the reference.

The tide at a surf zone's seaward edge is a level by the sky's hour and the date. What is slow (a seasonal inflow, the water table and the store's deep drying, a shore's section) is **given at a calendar date** and never integrated through a gap (W187); a calendar jump restarts every awake tile from the steady answer for the new date.

### What drawing reads

Inside an awake tile the surface is no longer a mesh built at load. Three things change, in the stages' order. **The surface's heights**: a regular grid over the tile whose vertices take the state's level each tick in the deformed-vertex pool, drawn with bounds taken from the tile's own range of levels (a rest pose's cluster bounds and errors would be wrong for a surface that heaves) and with a level of detail that is the grid's own, by the tile's distance; the traced structures are built from the same positions, as every deforming mesh's are. **The shore** stays where it is drawn today, per pixel, where the level meets the bed: the resolve reads the tile's bed and level in place of the load-time depth map, so the waterline is not the grid's sawtooth. **The foam** is the carried scalar in place of the closed forms, under the same lace. Which of today's closed forms each stage switches off inside a tile is [the grounding page's table](../experiments/water-state-grounding.md#which-of-todays-closed-forms-become-consequences-and-which-stay); outside a tile every one of them stands.

The game's question about the water at a point keeps its guarantee (the same bits on every toolchain): it answers from the baked answer, and from a state only where that state is `hashed` (ADR-0080, decision 11 and the owner's decision 1).

### The budget

**Not yet measured**, and set from the measurement, not before it. The target is the frame the engine is judged on: 3840 × 2160 with traced visibility and shadows. Four lines are the state's: **the step** (cells × sub-steps × rate); **the surface** (the pool's stage over the awake tiles' vertices); **the traced structures** (an awake tile's surface is rebuilt or refitted every tick); **the resolve's reads** (the tile's bed, level and foam in place of the depth map). For the step alone, the published 530 million cells a second in single precision on a 2010 card, scaled by memory bandwidth, suggests about 0.2 ms a sub-step for a million cells on the fastest card the engine is measured on and three to five times that on the oldest (*derived, an estimate to be replaced*); W175 measures it at three grids on both.

**By card** (proposed, to be fixed from W175's numbers): the fastest class runs up to two million live cells (about 120 MB); the middle class half a million; the oldest cards the engine is tested on run baked, with up to a hundred thousand live cells where a scene asks. Each is a ceiling the load checks against the scene's declared tiles.

### The stages

| row | stage | needs |
|---|---|---|
| W184 | the full equations on the CPU, the reference | — |
| W175 | the step on the card, held to the reference and measured | W184 |
| W176 | the check that a settled state stands where the steady solve says | W184 |
| W191 | the state in a running scene: one domain, tiled from the start, one cell size; sources and events on the world clock; advanced by ticks; pools on first use; absent free | W184, W175 |
| W192 | drawing reads the state: the surface, the shore per pixel, the traced structures; a pond that fills and a dam that spills, judged standing alone | W191 |
| W193 | the foam as a consequence: fed by the energy lost at a bore or a jump, carried, fading; a stream's white and a wave's the same | W192 |
| W194 | the tiers: baked, live and mixed as the game's setting; waking by observers and retiring; the ledger at the seam; what a save carries | W192, W176 |
| W195 | tiles by distance and nested two to one, the range and the memory by card, the state's lines in the frame budgets | W194 |
| W196 | the surf: a corridor driven at an edge that generates the train and lets the reflected one leave (an edge the reference gains first), breaking as bores, the swash as a front | W193, W195 |
| W197 | the ground's store drawn: wet ground as a consequence, a losing reach | W192 |
| W198 | the first shortcuts, one switch each, each judged by the owner's eye | W195, T232 |

The bed (W178 to W180) attaches at W191 through the bed as an input, the stress and the wave's measure as outputs and a carried scalar with its exchange; bodies (W181) and plants (W182) at W192; the layered ground (W183) owns the mobile layer; the calendar (W187) is read from W194 on.

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph,
the scheduler, or another capability, and it can be removed from the build the same way.

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | none | this capability stands alone |
| Component and event types | none | none yet: the reference is driven by its cases; how a world saves a state is W173's design |
| Tick scheduler entry | none | none yet: a caller steps a `Solver`; which phase owns the water's tick is the design's |
| Render-graph passes | none | the reference draws nothing |
| Content-build derived step | none | nothing is precomputed |
| Protocol methods | none | not needed |
| Tunables | none | run-time parameters arrive in `Config`; whoever owns a solver fills it |
| LOD policy | none | the reference has one resolution; the water's grid and LOD are W173's design |
| Determinism | `water_state::k_determinism` = `hashed` | above, "Determinism" |
| Zero cost when unused | plan 11 §11.10: nothing exists until a caller makes a `Solver` | a scene with none allocates and runs nothing |
| Tests and size table | `tests/water_state_tests.cpp`; `tests/size_table.cpp` pins `Ledger` and `Edges` (the state has no per-cell struct) | the cases above |
| Bench | `domain/water_state/bench/water_state_bench.cpp` | one step at three grids; the numbers are the next stage's |
| Removal proof | `ENGINE_WITH_WATER_STATE`, off in the minimal build | works |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_WATER_STATE=OFF`) drops the
module, its tests, and its bench; the module disappears from `build/<preset>/modules.json` and
is listed there under `disabled_capabilities`. Everything else must still build and pass.
