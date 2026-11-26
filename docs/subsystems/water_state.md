# water_state (domain)

**Purpose.** The shallow-water equations as a finite-volume step on a regular grid, on the CPU, in
`double`: the reference every later form of the engine's simulated water is held to (roadmap W173,
stage 1, W184). A rectangle of square cells carries depth and the two momenta, the ground's store and
a few carried quantities; a step moves them over a bed the caller hands in at every step, with
wetting and drying, bed friction, sources, the ground's take and the carried quantities' sources,
decay and exchange with the bed, and gives out per cell the speed, the stress at the bed and the
rate of dissipation. It draws nothing, holds no world position, and is not the shipped path: the
card's `float` step (W175) and whatever is cut down from it for the frame are checked against these
cases within stated bounds. The grounding is [water-state-grounding](../experiments/water-state-grounding.md);
the decision that the engine's water carries a state is W173's, recorded in an ADR of its own.

**Why this shape.** The full equations are the only set the grounding page read that holds a pool's
backwater, a bore and a drying front together and keeps a lake at rest
([§7](../experiments/water-state-grounding.md#7-a-reading-for-the-design-kept-apart-from-the-sources));
the kinematic and local inertial waves cannot pool or cannot drain, the diffusion wave's explicit
step collapses on flat water. Among the full equations' schemes the hydrostatic reconstruction
(Audusse et al.) was taken over the central-upwind scheme with Bollermann et al.'s wet/dry
reconstruction because at first order it is a few lines over any flux of the system without a bed,
keeps the depth non-negative under the ordinary step limit with no draining-time bookkeeping, and
puts the bed only into a face's reconstruction, so a bed that changes between steps needs nothing
recomputed. It is first order in space and time: W184 takes first order through every case first,
and second order only after. `double` because the reference is the answer, not the budget: the
round-off it leaves is the floor a `float` step is measured against, and its footprint is stated,
not minimised. **What first order costs is measured below** — the dam breaks' and the bowl's errors,
and a head lost at a bed step under moving water ("The hollow") that the next form of the scheme has
to remove.

**Owned data.** The state of one rectangle: depth, momenta, the ground's store and the carried
quantities, and the ledger of what entered and left it. The bed, its roughness, the sources, the
soil's parameters and the carried quantities' sources and exchange are the caller's and are read,
never kept.

**Public API.** `include/domain/water_state/water_state.h`: `Config` (with a `CarriedConfig` per
quantity), `Inputs`, `Ledger` (with a `CarriedLedger` per quantity), `Solver`.

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
(Pa, the friction law's), `dissipation()` (W/m², "The dissipation"). The scratch the step needs —
the velocities, the per-row maxima and sums, and for every x face (`nx + 1` a row) and y face (`ny +
1` rows of `nx`) its mass flux, its normal momentum's flux twice (one per side), its transverse
momentum's flux, its energy's flux and each carried quantity's flux — is the solver's, allocated once
at construction; a step allocates nothing.

**Each face's flux is computed once and stored**, and both cells beside it take it from the store,
one subtracting and the other adding the same number. That is what makes the water (and each
carried quantity) conserved to the last rounding of the sums, never to a flux computed twice that
could differ in its last bit.

**Footprint** (*stated, not minimised*): the state is 32 + 8k bytes a cell, the outputs 32, the
velocities 16 and the faces 2 × (40 + 8k), about **160 + 24k bytes a cell**, 208 with two carried
quantities. A `float` step keeps what it keeps; this one is not its model for memory.

## The step

```cpp
water_state::Solver solver(config);            // allocates everything
solver.depth()[i] = ...;                       // the initial state, written in place
water_state::Inputs in;                        // spans of nx * ny doubles; all but bed_m may be empty
in.bed_m = bed;  in.manning = n;  in.source_m_per_s = s;
in.ground_conductivity_m_per_s = k;  in.ground_suction_m = psi;  in.ground_capacity_m = cap;
in.carried_source[0] = ...;  in.carried_exchange[1] = ...;
f64 dt = solver.step(in, max_dt, jobs);        // one step of min(stable, max_dt) s; 0 if the inputs do not fit
u64 n  = solver.advance(in, duration_s, jobs); // to exactly duration_s, the last step shortened
```

`jobs` may be null (one thread); with a `jobs::JobSystem` the rows are dealt out
`Config::rows_per_job` at a time, which does not change a byte. Each step, in this order:

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
   applied to the transverse momentum and to the energy as further components. x faces use u as the
   normal velocity, y faces v. Two identical states give the exact flux of that state (what the
   formula equals in exact arithmetic), which is what keeps a lake at rest at rest to the bit. A
   carried quantity's flux is the mass flux times the concentration of the cell it comes from.
3. **The update.** The hydrostatic reconstruction's update (Audusse et al. 2016, (3.1)–(3.2)):
   U' = U − Δt/Δx (F_{i+1/2−} − F_{i−1/2+}) in each direction, where the normal momentum's flux on each
   side is H plus g/2 (h_i² − h*²) for that side's h*. The g/2 h_i² of the two faces of one cell cancel
   exactly, so each face stores H − g/2 h*_L² and H − g/2 h*_R² and the cell takes the difference; the
   bed enters only here, there is no other bed-slope term at first order.
4. **The dissipation** of the transport, below.
5. **Clamp and sources.** A depth a rounding error below zero is set to zero and the water that
   added is counted (`Ledger::roundoff_m3`). Then the source: h += Δt s for s ≥ 0, and for s < 0 the
   lesser of Δt |s| and what the cell holds, counted in `Ledger::source_m3`.
6. **The ground's take**, below.
7. **Friction.** Manning's law as the grounding page writes it,
   **τ**_b/ρ = g n² |**u**| **u** / h^(1/3) ([§1](../experiments/water-state-grounding.md#the-shallow-water-equations)),
   taken implicitly in its coefficient (*derived* here: a linearly implicit Euler step of
   d(h**u**)/dt = −g n² |**u**| (h**u**) / h^(4/3) with |**u**| held at its value after the update):
   (h**u**)' = (h**u**)* / (1 + Δt g n² |**u***| / h^(4/3)). It can stop a flow and never reverse
   one, at any Δt. h^(1/3) is `det::exp(det::log(h) / 3)`.
8. **Dry cells and outputs.** Under `dry_depth_m` the momenta are set to zero. The outputs are the
   final state's: the speed, the bed stress ρ g n² |**u**| **u** / h^(1/3), and the dissipation.
9. **The carried quantities' sources**, below.

**The step's length**, and why depth stays non-negative (*derived*). At a face between reconstructed
states L and R, H_h − a⁺ h_L = [a⁺ h_L (u_L − a⁺) − a⁻ h_R (u_R − a⁺)] / (a⁺ − a⁻) ≤ 0, since a⁺ ≥ u_L,
a⁺ ≥ u_R and a⁻ ≤ 0; likewise H_h ≥ a⁻ h_R. So what leaves a cell through each of its four faces is at
most the face's speed times that side's reconstructed depth, which the reconstruction keeps at or under
the cell's own, and every face speed is at most max(|u| + c) (x) or max(|v| + c) (y) over the cells.
Then h' ≥ h (1 − 2 Δt/Δx (A + B)), non-negative for Δt (A + B)/Δx ≤ ½; C = 0.45 leaves room under it.
Positivity of the hydrostatic reconstruction under the homogeneous solver's step condition is
Audusse et al. (2004)'s result, read here through the 2016 paper; this is the same argument for the
flux used. The same bound makes a carried concentration a convex combination of its neighbours'
(the depth after the outflow alone is non-negative), so it makes no new extremum.

**Edges.** Each of the four is `closed` (a ghost mirroring the edge cell with its normal velocity
negated: the mass flux through it is zero to the bit) or `open` (a ghost copying the edge cell: what
the edge cell carries out leaves, and what crosses is summed into `Ledger::open_edge_out_m3`, out
positive, and each quantity's `CarriedLedger::open_edge_out`).

**The bed and its roughness.** `Inputs::bed_m` and `Inputs::manning` are read at every step and
nothing derived from either is kept, so a step may find them changed: a bed raised or scoured, a bed
whose ripples made it rougher. The state does not move with the bed: each cell keeps its depth (its
volume), and its surface moves with the bed under it; the next step starts from that.

**The ground's store** (`Inputs::ground_*`, all three or none). Green and Ampt's rate as the
grounding page reads it ([§3](../experiments/water-state-grounding.md#3-the-ground-takes-water-until-it-is-saturated)),
f = K (1 + ψ Δθ / F), with F the water the store already holds, integrated implicitly over the step
(*derived* here: F' = F + Δt K (1 + ψΔθ / F') has the one positive root
F' = ½ ((F + K Δt) + √((F + K Δt)² + 4 K Δt ψΔθ)), finite from F = 0, so a dry soil drinks hard and
then settles towards K, with only a square root). The take is the least of F' − F, the water in the
cell and the room the store has left (its capacity, the soil's depth times its deficit, is an input),
so a full store takes nothing: the loss falls from capillarity's to K and then to nothing. The
soil's three numbers are an input field (Rawls et al.'s by texture are on the grounding page). The
store does not drain or dry between wettings yet (SWMM's 2 to 14 days, the grounding page's): that is
the design's, with a store's life across a session.

**The dissipation.** Per cell, what the transport lost of the water's mechanical energy
E = ρ (h |**u**|² / 2 + g h² / 2 + g h z): D = −[(E' − E)/Δt + (G_e − G_w + G_n − G_s)/Δx], E' after the
update, with the energy's flux G carried through each face by the same central-upwind formula as the
other components (*derived* here; the residual of each cell's balance). Summed over the cells the
faces' fluxes cancel, so the total is exactly the scheme's loss of energy (`Ledger::scheme_loss_j`,
signed); in a cell it is the loss at a bore or a jump where one stands and a small residue elsewhere,
of either sign, which falls with the cell. `dissipation()` gives out its positive part plus the bed
friction's work ρ g n² |**u**|³ / h^(1/3) (`Ledger::friction_work_j`). At a bore it is the grounding
page's reading ([§4](../experiments/water-state-grounding.md#4-breaking-as-something-simulated)): the jump's head
loss (h₂ − h₁)³ / (4 h₁ h₂) times the flux through it, which the bore's case holds.

**The carried quantities.** Each rides the mass fluxes (step 2), and after the water's own update
(step 9) takes, in this order: what the dissipation makes, `CarriedConfig::per_joule` × D × Δt where
the speed is at least `min_speed_m_per_s` (the foam's source: the grounding page's §5 reads the cover
as proportional to the dissipation and gated by the speed that entrains air); an outside source,
`Inputs::carried_source[k]`; **the exchange with the bed**, `Inputs::carried_exchange[k]`, positive
lifted into the water and negative settling out of it, taken only as far as the water holds the
quantity; and an exponential decay over `decay_s`, its factor `det::exp(−Δt / decay_s)` the same in
every cell. The exchange is a hook and not a law: a later row (W178's sediment) computes the field
from the state's speed and bed stress before each step and hands it in, so the step stays the one
transport path and knows nothing of grains. Each quantity's `CarriedLedger` counts what it was given,
made, exchanged, lost to decay and carried out. Nothing in the step tells one quantity from another.

## The cases

`domain/water_state/tests/water_state_tests.cpp` and `hollow_tests.cpp`, each with its bound and
why. Measured on MSVC 14.51, `msvc-debug` unless named, 2026-11-25:

| case | what | bound | measured |
|---|---|---|---|
| `a lake at rest over a bed with dry islands stays exactly at rest` | 64 × 48 cells of 0.5 m, a surface at 1 m over a bed of 1/64 steps with islands at and above it, 20 s | **zero**: not one bit of depth or momentum moves (every height is a binary fraction, so every reconstruction is exact) | 0 cells moved |
| `a lake at rest over an inexact bed stays at rest to round-off` | the same at 0.9 m over a bed no binary fraction holds | surface within 1e-13 m, no speed over 1e-11 m/s | 1.1e-16 m, 1.9e-15 m/s |
| `a dam breaks on a wet bed as Stoker's solution, first order` | SWASHES §4.1.1: 10 m, dam at 5 m, 5 mm on 1 mm, 6 s; 200, 400, 800 cells | L1 order ≥ 0.5 between each pair (a first-order scheme's floor at a shock); 800-cell relative L1 ≤ 0.0045 (measured and 15 %) | 0.01197, 0.00693, 0.00394; orders 0.79, 0.81 |
| `a dam breaks on a dry bed as Ritter's solution, first order` | SWASHES §4.1.2 | the same floor; ≤ 0.0058 | 0.01228, 0.00798, 0.00502; orders 0.62, 0.67 |
| `water is counted to the last cell through wetting, drying, the ground and an open edge` | 48 × 40 cells of 0.5 m, 0.3 m released down a rough 2 % slope, n 0.03, a spring, a drain, sand on the east half (K 236 mm/h, ψΔθ 17 mm, a 10 mm store), an open east edge, 60 s | surface + ground + out − initial − sources within 1e-12 of the water involved; no negative depth; cells wetted, the drain's dried, stores filled and none over capacity | 2.8e-14 m³ of 36 m³; 2.4 m³ to the ground, all 960 stores full |
| `the ground takes ponded water at Green and Ampt's rate until it is full` | 1 m ponded over loam (K 13.2 mm/h, ψ 88.9 mm, Δθ 0.3) for an hour; a column with a 20 mm store | Green and Ampt's cumulative closed form within 1e-3 of itself; the small store stops at its capacity | 35.973 mm against 35.975 mm; 20 mm |
| `a raised bed keeps each cell's water and a rougher bed is read at once` | the dyadic lake at 0.5 m; at 2 s the bed under a block raised 0.125 m and Manning's n on another patch doubled; 400 s more | volume to 1e-12; surface range and its gap to the level the volume gives over the new bed under 1 µm, no speed over 1 µm/s | volume 2e-13 m³; range and gap under 1e-9 m; 4e-14 m/s (peak 0.14 m/s) |
| `two carried quantities are counted and the step does not know which is which` | the rough slope with a foam made by the dissipation (decay 4 s) and a sediment settling and lifted, 30 s; again with the two swapped | each account closes to 1e-12; swapped, every byte of each quantity and of the water is the same | foam 0, sediment 1.9e-14 |
| `a carried concentration makes no new extremum` | a concentration in [0.2, 0.8] over a dam's break on a bumpy bed, 20 s | inside [0.2, 0.8] to 1e-12 in every cell over 1 µm deep | 0.20023 to 0.79940 |
| `a bore's dissipation is the jump's head loss times the flux through it` | Stoker's case at 800 cells, 4 s to 6 s | the step's loss rate within 25 % of Bélanger's; the given-out dissipation within 50 %, four fifths of it within 0.5 m of the bore | +9 %; +27 %; 93 % |
| `a bowl's water sloshes as Thacker's planar surface` | SWASHES §4.2.2, one period at 0.08 and 0.04 m cells | L1 order ≥ 0.5; the finest L1 ≤ 0.27 and its mean velocity's error ≤ 0.32 of the speed (measured and 15 %) | L1 0.417, 0.233 (order 0.84); velocity off 0.50, 0.28 |
| `a hollow fills to its lip and spills at critical depth over it` | W176's geometry (below), 0.125 m cells, Q 0.1 and 0.02 m³/s, both starts, 900 s; optimised builds only | critical flow on the lip, settled, outflow, count, both starts, the heads' ratio (below) | below |
| `a hollow at twice the cell is recorded beside it` | the same at 0.25 m, Q 0.1 | the same less the lip | below |
| `a hollow's crest in one dimension` | the section alone, one cell wide, q 0.05 m²/s | critical flow on the lip | below |
| `the same bytes twice and at any thread count` | the rough slope with everything on, 20 s: twice on one thread, once on four workers three rows a job | the same state hash all three times | equal |
| `the slope's bytes are the same on every toolchain` | the same run's hash | the hash MSVC gave, 17390608002821259899 | pinned |
| `a step refuses inputs that do not fit the grid` | a bed of the wrong size | nothing changes, the step returns 0 | |

The dry bed's error is larger and converges more slowly than the wet one's: at the front the depth
falls to zero as the fan's square and the first-order scheme smears it. The bowl's slosh is damped
by the scheme's numerical dissipation (its mean velocity is 28 % short of the closed form's after one
period at 4 cm cells). A second-order form has all these numbers to beat.

## The hollow

W176's specification of the tie between a simulated lake and the steady solve's level (roadmap W176;
its section of the grounding page lands with that row): an 8 × 8 m hollow with a flat floor 0.5 m
under the sill and vertical walls, a crest 2 m wide and 0.5 m long at the sill's height, a 5 m channel
falling at 0.1 to a free edge, walls elsewhere; Manning's n 0.01; a source of Q over the hollow's
first metre; from the plan's level (2 cm under the sill) and from 2 E_c over it; 900 s of world time.
The crest's closed form: y_c = (q²/g)^(1/3), E_c = 1.5 y_c (0.0951 m at Q = 0.1, 0.0325 m at 0.02),
the friction over the crest at most 1.2 and 1.8 mm, the bound max(0.05 E_c, 1 mm). `run_hollow`
returns everything a run measured, so W176's comparison against the steady solve is a case beside
these on the same runs.

**What holds** (`msvc-release`, the four runs on threads of their own): the crest's last column of cells stands at the critical section's specific energy, 0.0947 m against E_c = 0.0951 m at Q = 0.1 (its depth 0.0644 m against y_c = 0.0634 m) and 0.0322 m against 0.0325 m at Q = 0.02, inside the bound; the lake is settled (its 10 s mean moved 9e-9 m and 1.5e-5 m over the last τ, its range over the last 10 s 1.3e-9 m and 1.4e-6 m); the outflow over the last 10 s is 0.1000 and 0.01999 m³/s; the water is counted to 1.3e-11 m³ of 90 m³ and 1.6e-11 of 18; the two starts end within 2e-5 m of each other; and the heads' ratio is 2.728 against 5^(2/3) = 2.924, inside the two bounds' sum (0.29). A run is 81,142 and 73,971 steps; the four took 86 s on four threads in `msvc-release` on a shared machine. At 0.25 m cells the lip's column reads 0.0927 m (the crest two cells long, not asserted) and the lake 0.1309 m.

**What does not: the lake stands too high, because of the step.** The lake's level over the sill is
0.1315 m at Q = 0.1 (38 % over E_c) and 0.0482 m at 0.02 (48 % over), at 0.125 m cells and the same at
0.25 m (0.1309 m), from both starts — far outside W176's bound of 5 % of E_c, so that part of W176's
specification **fails today and is not asserted**; the case prints it. The one-dimensional case
places it: with no sideways contraction the crest's last cell stands at y_c (0.0644 m against
0.0634 m) with the critical section's specific energy (0.0947 m against 0.0951 m) — the flux sets
critical flow on the lip, as W176's specification expects — while the lake stands at 0.1279 m: a head
of 0.033 m (35 % of E_c) is lost at the hollow's upstream face, where the bed steps up 0.5 m under
0.13 m of water; the entrance's contraction in two dimensions adds about 4 mm. That is the hydrostatic
reconstruction under moving water (*derived* from the cases, not from a source): it keeps each cell's
velocity and cuts its depth to the face's (3.3), so the lake's side of the step offers the face
water a fifth as deep at the lake's slow speed, and the face's flux, a Riemann problem between that
nearly still state and the crest's flow, passes Q only with more head than Bernoulli across the step
would ask. It does not fall with the cell (0.1315 m at 0.125, 0.1309 m at 0.25), because the step
stays a step. A lake at rest is exactly at rest over it; a lake that carries an outflow over a
step is not at the right level. **This is the first thing the next form of the scheme has to fix**:
a reconstruction that keeps a moving steady state (the "moving-water equilibria" literature) — read
first, then held by this case with W176's bound.

## Determinism

`k_determinism` is `hashed`: a step is a function of the state's bytes, the inputs' bytes and Δt.
Only `+ − × ÷` and `sqrt` decide anything, the friction's cube root and the decay's factor are
`core/math/deterministic.h`'s `exp` and `log`, the step's length is a maximum (the same in any
order), each face's flux is computed once, and every sum over cells is taken per row and then in row
order, so the same inputs give the same bytes at any thread count; the tree compiles with
contraction off ([ADR-0035](../adr/0035-no-floating-point-contraction.md)), so they are the same
bytes on MSVC, GCC and Clang. `state_hash()` is FNV-1a over the state's arrays; the two cases above
hold it at one and four threads and against MSVC's number on the Linux compilers.

**What a single-precision step on a card would have to change to give the same bytes on every
device.** IEEE 754 makes `+`, `−`, `×`, `÷` and `sqrt` correctly rounded, which is why this step
needs nothing else; Vulkan's SPIR-V precision rules keep only the first three: `+`, `−` and `×` of
32-bit floats are correctly rounded, while division and the square root are allowed an error of a
few ulp (and `exp`, `log` and `pow` more), so two drivers may round them differently. A `float` step
that is to be the same bytes on every card would therefore have to:

- replace **every division** — the velocities hu/h, the flux's 1/(a⁺ − a⁻), a carried quantity's
  concentration, the step's length, Δt/Δx, the friction's denominator — by a reciprocal built from
  `+ − ×` alone (a seed from the float's bits by integer operations, then a fixed number of Newton
  steps), or by multiplications by constants precomputed on the CPU;
- replace the **square roots** — the celerities √(g h), the speed, the ground's root — the same way
  (a bit-pattern seed and fixed Newton steps for 1/√x, then a multiplication);
- replace the **cube root and the decay's `exp`** by fixed polynomials in `+ − ×` over a range
  reduction done with integer operations, as `core/math/deterministic.h` does in `double`;
- forbid **contraction**: a SPIR-V compiler may fuse `a × b + c` unless the result is decorated
  `NoContraction` (Slang's `precise`), so every arithmetic the bytes depend on carries it;
- fix **denormals** and **the order of every sum**: flushing a denormal to zero is a device's choice
  unless the float-controls feature that preserves it is required, and a reduction must stay in a
  fixed order (no atomics in a sum), as the rows' sums here are.

The maximum that sets the step's length and the comparisons and branches on h are exact on every
device and need nothing.

## Not yet built

- **An edge that makes waves**: the surf row will need an edge that generates an incoming wave train
  and lets reflected waves leave through it (a generating-absorbing boundary); the reference has only
  closed and open edges.
- A second-order form (in space, with a limiter, and in time), and a reconstruction that keeps a
  moving steady state across a bed step ("The hollow").
- The ground's store draining and drying between wettings, and a water table for gaining reaches.
- A law for the carried quantities' exchange with the bed (W178's): the hook is in, the law is not.

## Testing

`tools/dev.ps1 test -Filter '^water_state' -NoDriver`; the hollow at W176's cell runs only in an
optimised build (`msvc-release`, the release containers), the rest everywhere. Benchmarks:
`tools/dev.ps1 bench -Filter 'water_state.*'`: one step of a 64², 256² and 512² grid (a lake with dry islands and a dam's break across it, Manning's n 0.03) on one thread. **Measured 2026-11-25, `msvc-release`, a busy machine** (the harness waited 60 s for quiet and went on: other processes used 48 % of the CPU, the GPU lock held by another session's flights), so the numbers are upper bounds on the cost: a median of 0.637 ms, 7.76 ms and 33.2 ms a step, **6.4, 8.4 and 7.9 million cells a second**. At about 190 bytes a cell read and written a step that is 1.5 GB/s, far under a core's bandwidth: the reference is bound by its arithmetic (a flux has two square roots and five divisions), as a reference may be.

## Sources, and how far each was read

Read on 2026-11-25 through the fetch tool, as HTML renderings; equations only were taken, no text.

| source | what is taken | read | licence |
|---|---|---|---|
| Audusse, Bouchut, Bristeau, Sainte-Marie (2016), *Math. Comp.* 85, arXiv 1409.3825 | the hydrostatic reconstruction: (3.1) the update, (3.2) the two sides' fluxes and their corrections, (3.3) the reconstructed states, (3.4) the depths, (3.5) the face's bed | read (ar5iv rendering, the scheme's section) | arXiv's; not shown on the rendering, nothing copied but the equations |
| Audusse, Bouchut, Bristeau, Klein, Perthame (2004), *SIAM J. Sci. Comput.* 25, 2050–2065 | the scheme's origin and its positivity under the homogeneous solver's step condition | through the 2016 paper and the search engine's summary of the abstract; the paper itself not reached as text | not read |
| Bollermann, Chen, Kurganov, Noelle (2014), arXiv 1412.3580 | the central-upwind flux (2.8), the one-sided speeds (2.13)–(2.14), the step condition (2.15), ε = 10⁻⁹ | read (ar5iv rendering) | arXiv's; not shown on the rendering |
| Delestre et al., SWASHES, arXiv 1110.0288 | Stoker's (§4.1.1) and Ritter's (§4.1.2) solutions and their parameters, Thacker's planar surface in a paraboloid (§4.2.2) | read (ar5iv rendering, those sections) | the paper's text arXiv's; its code is CeCILL-V2 and was **not** read or used |
| Green and Ampt (1911), Bélanger's jump, Rawls et al. (1993) | the infiltration rate, the jump's head loss, the soils' K and ψ | through the grounding page (§3, §4), which read them in the encyclopaedia's articles and TxDOT's manual | as the grounding page records |
| W176's specification of the hollow | the geometry, the closed form at the crest, the bounds, the settling time | the roadmap row's own page section | the engine's |

## The design: a state in a world

(Roadmap W173. The design of the state in a running world is written in its own change.)

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
| Tests and size table | `tests/water_state_tests.cpp`, `tests/hollow_tests.cpp`; `tests/size_table.cpp` pins `Ledger`, `CarriedLedger` and `Edges` (the state has no per-cell struct) | the cases above |
| Bench | `domain/water_state/bench/water_state_bench.cpp` | one step at three grids; the numbers are under "Testing" |
| Removal proof | `ENGINE_WITH_WATER_STATE`, off in the minimal build | works |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_WATER_STATE=OFF`) drops the
module, its tests, and its bench; the module disappears from `build/<preset>/modules.json` and
is listed there under `disabled_capabilities`. Everything else must still build and pass.
