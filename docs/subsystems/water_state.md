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
