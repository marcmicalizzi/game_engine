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

**What it can be trusted for, and what not.** Trusted: conservation of water and of every carried
quantity to the rounding of the sums; a depth that never goes negative; a lake at rest, exactly, over
any bed; dam breaks and bores at first order, with the energy a bore loses; the ground's take by
Green and Ampt; the same bytes at any thread count and on MSVC, GCC and Clang. **Not trusted: any
steady flow over a bed that steps between neighbouring cells by a fraction of the depth or more** —
a dam's lip, a weir, a ledge in a stream, terrain sampled coarser than its relief under thin water.
There the level upstream stands too high by about q Δz / (h √(g h)) (q the discharge per width, Δz
the step, h the depth beside it), and refining the cells does not help ("The hollow").

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
at construction; a step allocates nothing, with or without a job system (the rows are dealt in at most
64 chunks, which `jobs::parallel_for` holds without allocating; `rows_per_job` is raised to make it so).

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
(the depth after the outflow alone is non-negative), so **the transport** makes no new extremum. The
rest of the step can: a drain, a sink and the ground's take remove water and leave the carried
amount, so the concentration rises; a cell drained to nothing keeps a stranded amount with no water
to carry it; and a cell under the dry depth still exports mass at a face but no carried quantity.
Whether a sink takes its share of what the water carries is the design's to decide (W173).

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
of either sign (expected to fall with the cell; measured at one grid only). `dissipation()` gives out its positive part plus the bed
friction's work ρ g n² |**u**|³ / h^(1/3) (`Ledger::friction_work_j`). At a bore it is the grounding
page's reading ([§4](../experiments/water-state-grounding.md#4-breaking-as-something-simulated)): the jump's head
loss (h₂ − h₁)³ / (4 h₁ h₂) times the flux through it, which the bore's case holds to 25 % for the
signed loss (+9 % measured) and to 50 % for the given-out figure (+27 %, larger because the positive
part drops the negative residues that cancel in the sum). **A head the scheme loses for no physical
reason is given out as dissipation too**: the loss at the hollow's wall ("The hollow") appears as a
dissipation at a calm lake's lip, and a foam fed by it would make foam there; a consequence of that
defect, which goes with it.

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
| `a bowl's water sloshes as Thacker's planar surface` | SWASHES §4.2.2, one period at 0.08 and 0.04 m cells | L1 order ≥ 0.5, the check with teeth; the finest L1 ≤ 0.27 and its mean velocity's error ≤ 0.32 of the speed are regression pins (measured and 15 %), not bounds of correctness | L1 0.417, 0.233 (order 0.84); velocity off 0.50, 0.28 |
| `a hollow fills to its lip and spills at critical depth over it` | W176's geometry (below), 0.125 m cells, Q 0.1 and 0.02 m³/s, both starts, 900 s; optimised builds only | critical flow on the lip, settled, outflow, count, both starts, the crest energies' ratio (below); the lake's level is not held | below |
| `a hollow at twice the cell is recorded beside it` | the same at 0.25 m, Q 0.1 | the same less the lip | below |
| `a hollow's crest in one dimension` | the section alone, one cell wide, q 0.05 m²/s | critical flow on the lip; the depth lost across the wall within 25 % of q Δz / (h_L abs(a⁻)) | below |
| `a hollow's lake stands at E_c plus the crest's friction over a vertical wall` | the same run against W176's bound on the lake's level | **an expected failure** (`doctest::should_fail`): it fails today; the day it passes the suite fails and says a scheme holds moving water over a step | fails, as expected |
| the bumps, the sill, the ledge, the perturbed and emerged lakes, section 8's graded and wall hollows | W199's cases (below) | the lakes, the ledge and the graded hollow held; the bumps and the sill reported | "Moving water over a bed that steps" |
| `a dam break's bytes over a flat bed are the same on every toolchain` | Stoker's 800-cell run's hash | the hash MSVC gave, 11759616244214831310 | pinned |
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

**What holds** (`msvc-release`, the four runs on threads of their own): the crest's last column of cells stands at the critical section's specific energy, 0.0947 m against E_c = 0.0951 m at Q = 0.1 (its depth 0.0644 m against y_c = 0.0634 m) and 0.0322 m against 0.0325 m at Q = 0.02, inside the bound; the lake is settled (its 10 s mean moved 9e-9 m and 1.5e-5 m over the last τ, its range over the last 10 s 1.3e-9 m and 1.4e-6 m); the outflow over the last 10 s is 0.1000 and 0.01999 m³/s; the water is counted to 1.3e-11 m³ of 90 m³ and 1.6e-11 of 18; the two starts end within 2e-5 m of each other; and the crest energies' ratio is 2.944 against the law's 5^(2/3) = 2.924, inside the two bounds' sum (0.29). (The lakes' levels' ratio, 2.728, would pass the same test, but for the wrong reason: each level is wrong by the step's loss.) A run is 81,142 and 73,971 steps; the four took 86 s on four threads in `msvc-release` on a shared machine. At 0.25 m cells the lip's column reads 0.0927 m (the crest two cells long, not asserted) and the lake 0.1309 m.

**What does not: the lake stands too high, because of the step.** The lake's level over the sill is
0.1315 m at Q = 0.1 (38 % over E_c) and 0.0482 m at 0.02 (48 % over), at 0.125 m cells and the same at
0.25 m (0.1309 m), from both starts — far outside W176's bound of 5 % of E_c, so that part of W176's
specification **fails today and is not asserted**; the case prints it. The one-dimensional case
places it: with no sideways contraction the crest's last cell stands at y_c (0.0644 m against
0.0634 m) with the critical section's specific energy (0.0947 m against 0.0951 m) — the flux sets
critical flow on the lip, as W176's specification expects — while the lake stands at 0.1279 m: a head
of 0.033 m (35 % of E_c) is lost at the hollow's upstream face, where the bed steps up 0.5 m under
0.13 m of water; the entrance's contraction in two dimensions adds about 4 mm.

**Why** (*derived* from the scheme and the cases; the published account below is read only as far as
its abstract). At the face between the lake's last cell (depth h_L, speed u_L) and the crest the
reconstruction (3.3) hands the face the lake's speed on the depth over the crest, H = h_L − Δz, so
the face's left state carries only q H / h_L of the discharge q. In a steady state the rest can come
only from the central-upwind mass flux's numerical viscosity, a⁺a⁻ (h_R − h_L)/(a⁺ − a⁻), which needs a
depth difference across the face of about H − h_1 ≈ q Δz / (h_L ∣a⁻∣), a⁻ ≈ u_L − √(g H): 0.037 m at
Q = 0.1 (39 % of E_c) and 0.014 m at Q = 0.02 (42 %). Δx does not appear in it, which is why the
loss does not fall with the cell (0.1315 m at 0.125, 0.1309 m at 0.25) and why a second-order form,
which still meets a step at a face, would not remove it. The one-dimensional case holds the measured
H − h_1 to this within 25 %. Sun and Xing (2023, arXiv 2308.09265; abstract read) describe the same
artifact for first-order schemes at a discontinuous bed: a spurious spike from the numerical
viscosity in the water height at the step, which refinement does not shrink and which for subsonic
flow makes the scheme converge to a wrong solution; they propose a central flux for the mass at the
step. A lake at rest is exactly at rest over the step; a lake that carries an outflow over it is
not at the right level.

**What it asks of the scheme.** A reconstruction that keeps moving steady states, not only the lake
at rest, at a bed discontinuity. The smallest published candidate found: Berthon and
Michel-Dansac's fully well-balanced hydrodynamic reconstruction (arXiv 2501.03975; abstract read;
its text under CC BY-NC-ND 4.0, so only what a reading of the paper itself justifies may be taken,
and nothing copied), which extends the hydrostatic reconstruction to moving steady states with any
flux and reduces to it at rest; its positivity is not stated in the abstract. To be read whole
before it is built, then held by W176's bound. **W176 is changing its specification**: a vertical
wall is a bed discontinuity on which no shallow-water scheme is owed a unique answer, so the 5 %
bound moves to a graded approach (the bed's rise across a face near the sill a small fraction of the
local depth), at Δx and Δx/2 with the error falling, and the vertical wall stays a reported case;
the graded approach is built since 2026-11-26 and passes (below).

## Moving water over a bed that steps

(Roadmap W199, 2026-11-26: the cases that will judge a reconstruction keeping moving steady states,
built and measured on today's scheme before any change to it; `moving_water_tests.cpp` and the
section 8 cases of `hollow_tests.cpp`.) A case today's scheme fails prints its numbers and asserts
only what holds and must keep holding (nothing that is not a finite number, the count, the
settling); the bound the changed scheme is to meet is a named constant beside it, not yet asserted.
The bumps are Berthon and Michel-Dansac's §6.2.2 runs (arXiv 2501.03975, read at the source for the
cases' set-up: g 9.81, (0, 25), Z = max(0, 0.05 (x − 8)(12 − x)), the start h + Z = H0 with q = q0
everywhere, q0 imposed at the left edge, h = H0 at the right where the outflow is subcritical), which
are SWASHES §3.1's; the edges' imposed values are the case's own (an open edge, and the edge cell's
discharge or depth written back after every step), so no edge kind is added. The exact depth is
Bernoulli's head solved per cell (the cubic h³ + (z − E) h² + q²/2g = 0, *derived*; SWASHES §3.1's
cubics are this with E from the outflow's depth or the crest's critical one), and the shock is where
the two depths keep q²/h + gh²/2. The error e_v = √((1/Δx) Σ |v_{i+1} − v_i|²) over neighbouring
cells, for v = q and B = q²/2h² + g(h + z), is the paper's (§6.2.2, the 1/Δx weight read in its
markup). Measured on MSVC 14.51, 2026-11-26 (the 75-cell grids in every build, the finer ones and
the hollows in optimised builds):

| case | cells | e_q | e_B | max ∣h − exact∣ | the paper's hydrostatic first order (HLL, 75 cells) |
|---|---|---|---|---|---|
| subcritical bump, q0 4.42, H0 2, 500 s | 75 / 150 | 0.131 / 0.0918 | 0.302 / 0.208 | 0.031 / 0.016 | 7.73e-2, 1.79e-1 (its reconstruction: 1.06e-14, 2.73e-14) |
| transcritical bump, q0 1.53, free outflow, 125 s | 75 / 150 / 300 | 0.065 / 0.045 / 0.031 | 0.247 / 0.160 / 0.106 | 0.028 / 0.014 / 0.0073, the largest within two cells of the crest | 3.74e-2, 1.45e-1 (4.73e-14, 4.50e-14) |
| bump with a shock, q0 0.18, H0 0.33, 1000 s | 75 / 150 | 0.091 / 0.106 | 1.18 / 1.63 | 0.013 / 0.0068 away from the shock; the shock's cell 11.83 / 11.75 m against 11.666 m | 5.72e-2, 6.92e-1 (7.09e-2, 8.17e-1: not to improve) |
| supercritical sill: q 0.5 at 0.1 m (Fr 5.05) over 0.05 m and 2 m, 0.05 m cells, 10 s | 200 | 0.079 | 0.015 | 0.0050 | — (this case's own; the top stays supercritical, E 1.374 m against E_c + z 0.492 m) |

The errors are of the paper's order and fall with the cell (the reference's central-upwind flux is
not HLL, so they are not the same numbers); its reconstruction holds the two smooth bumps to
10⁻¹⁴, and `k_bump_steady_bound` (1e-11) and `k_bump_depth_bound` (1e-10) are what the change will
be held to. The lakes beside a dry bed, held today and to stay held: **the dyadic lake with dry
islands, every wet cell beside an island given ±2⁻⁵⁶ m²/s**, 20 s: surface drift 0, fastest
8.0e-16 m/s, nothing clamped (held to 1e-13 m and 1e-11 m/s); **the paper's emerged lake** (its
§6.2.1: the bed exp(1 − 1/(1 − (4(x − ½))²)) on (0, 1), top 1, under a surface at 0.5, 50 cells, to
t = 1; `det::exp`): L2 errors 0 and 0 at rest (the paper's hydrostatic 1.85e-17 and 1.24e-16), and
q 2.1e-18 with the hair; **a slow lake against a ledge** (0.3 m at 0.03 m/s against a 0.5 m ledge
carrying a 1 mm film, closed edges, 20 s): the ledge's water never above its start (0.005 m³; 0.0044
at the end, the film draining back). And **the flat bed's bytes are pinned**: Stoker's 800-cell run's
state hash, 11759616244214831310 on MSVC, which a change to what a face does where the bed steps
must leave standing.

**Section 8's hollows, as one-dimensional columns** (`Geometry::graded` and `wall`, `one_dimensional`:
section 8's geometry is the same across its 8 m, so a column is its run per metre at the unit
discharge; 300 s; **the level is the mean over the flat floor from 1 to 4 m, the floor less the
source's metre, and the last 20 s**, section 8's reading, confirmed by W176, the row that owns the
specification). **The graded hollow passes today**, both discharges, both cells, the error falling,
and is held:

| q (m²/s) | cell | graded: level over the sill | error outside E_c … E_c + friction | bound | wall: level | the wall's first-order limit |
|---|---|---|---|---|---|---|
| 0.05 | 0.125 | 0.09865 m (+3.7 %) | 2.31 mm | 4.76 mm | 0.12787 m (+34 %) | 0.1333 m |
| 0.05 | 0.0625 | 0.09771 m (+2.7 %) | 1.37 mm | 4.76 mm | 0.12824 m | |
| 0.01 | 0.125 | 0.03521 m (+8.2 %) | 0.92 mm | 1.63 mm | 0.04755 m (+46 %) | 0.0475 m |
| 0.01 | 0.0625 | 0.03474 m (+6.8 %) | 0.45 mm | 1.63 mm | 0.04781 m | |

Every run is settled (its 20 s mean moved under 6e-9 m over the last τ on the approach, 9e-7 m at
the wall), passes its inflow and counts its water to 1e-12. On the graded approach the hydrostatic
reconstruction carries the hollow's energy to the crest, as W199's reading of the scheme predicted:
the defect is the wall's, and the gain of a scheme that keeps moving states is to show on the bumps
and the wall, with the graded hollow the guard that nothing that worked broke.

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
- **The bumps held to their closed forms**: built and reported since 2026-11-26 ("Moving water over a
  bed that steps"), not held until a reconstruction keeps moving steady states (W199).
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
| Sun, Xing (2023), arXiv 2308.09265 | the artifact at a discontinuous bed: numerical viscosity, not shrinking under refinement, wrong convergence for subsonic flow | abstract | arXiv's non-exclusive distribution licence |
| Berthon, Michel-Dansac (2025), arXiv 2501.03975 | a fully well-balanced extension of the hydrostatic reconstruction, named as the candidate; for W199's cases (2026-11-26) the facts of §6.2.1–6.2.2: the beds, starts, edges, run lengths, g, the error measure and the printed errors of Tables 3, 4, 6–8 | abstract; §6 through a note of the arXiv HTML's own markup taken on 2026-11-26, numbers only | CC BY-NC-ND 4.0: numbers and facts, no text |
| SWASHES §3.1 (Delestre et al., arXiv 1110.0288) | the bumps' cases, as the paper above runs them; the cubics are re-derived here from Bernoulli's head, not taken | not read for W199; through the paper above | the paper's text arXiv's |
| W176's specification of the hollow | the geometry, the closed form at the crest, the bounds, the settling time | the roadmap row's own page section | the engine's |

## The design: a state in a world

(2026-11-25, roadmap W173; the decision is [ADR-0080](../adr/0080-water-carries-a-state.md), accepted 2026-11-26 with the owner's nine decisions each as recommended; the sources are [water-state-grounding](../experiments/water-state-grounding.md). This section is the shape the stages build towards. What this module holds today is the reference alone, described above; nothing below is built until its stage's row says so, and a figure marked *derived* is this page's arithmetic from read numbers, to be replaced by a measurement.)

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

**Single precision throughout in the first form** (ADR-0080, decision 3): 28 bytes a cell that change (with one carried scalar and the compensated sum's remainder), about 12 that do not, and 16 for what a step gives out (the speed, the two components of the stress at the bed, the dissipation); a step that reads one copy of the changing state and writes another holds two, so **about 84 bytes a cell on the card** (*derived*; the step W175 measured moves 32 of them). Smaller number types are a shortcut, and two of them are known not to work as they stand: a loam's take in one sixtieth of a second is 6 × 10⁻⁸ m ([grounding §3](../experiments/water-state-grounding.md#3-the-ground-takes-water-until-it-is-saturated), 13.2 mm/h), under the last bit of a 16-bit field over any useful range and under the last bit of a single-precision depth of one metre (1.2 × 10⁻⁷ m), so a naive step takes the wrong amount from the water (W175 measured 3.3 % too little just under a metre of depth; by the same arithmetic it is nearly double between one and two metres and nothing at all below two). The ground's take and the bed's change therefore go through a compensated sum, which costs a plane of the state; a coarser clock alone was measured to help only when sixty-four times coarser, and then in lumps; W175 holds "a pond over loam loses what the store gains" and "a bed that moves slowly moves" as cases before anything leans on either.

The surface a cell draws and balances is its depth plus its bed, and **the bed is a height from one datum for the whole domain**, so that a lake at rest stays at rest in single precision wherever in the world the domain stands and across every seam inside it; whether it does is the first thing W175 measures.

### Domains and tiles

**A domain is where a scene says water may have a state**: a pond's hollow, a reach of a stream with its banks, a beach's surf zone out to where the water is shallow against the wavelength, a place an author expects a dam. It is the scene's data. It names its cell size, one of four classes a factor of two apart (**0.25 m** for a stream's reach, **0.5 m** for a pool and a surf zone, **1 m and 2 m** for open water), and whether it is persistent.

**A tile is 64 cells a side** (16 m, 32 m, 64 m or 128 m by class), aligned to the world's 64 m cell grid, with positions local to the domain (ADR-0053). It is the unit that is allocated, wakes, sleeps and is counted, and a domain allocates only the tiles its footprint touches, so a narrow corridor does not pay for the square round it. At 84 bytes a cell a tile is 0.34 MB (*derived*). For scale, counted in tiles (*derived*):

| domain | class | tiles | cells | memory |
|---|---|---|---|---|
| a pond 100 m across | 0.5 m | 16 to 25 | 66,000 to 102,000 | 5.5 to 8.6 MB |
| a stream's corridor 10 m wide, 1 km long | 0.25 m | 63 along an axis to about 140 on a diagonal | 258,000 to 573,000 | 22 to 48 MB |
| a lake 500 m square | 1 m | 64 to 81 | 262,000 to 332,000 | 22 to 28 MB |
| a kilometre of beach, a 200 m surf zone | 0.5 m | 224 to 264 | 918,000 to 1,081,000 | 77 to 91 MB |

Memory is not what limits a domain on any card the engine runs on; the step's time and the moving surface's are.

**The edge of a domain is a seam with the far field, and water is counted across it.** Where a stream enters, the baked discharge is the inflow; where it leaves, what crosses is what the far field continues with; along a dry edge nothing crosses, and water that reaches a dry edge is reported, not lost. The difference between what a domain passed on and what the baked answer assumed is kept in a **ledger for that link**. The baked far field is steady and cannot take a transient, so the ledger's balance is spent in one place only: as a correction to the inflow of the next tile that wakes downstream on the same link, released over a stated time. Until then it is a count that says how much the picture and the books differ, and it is reported; it is never silently dropped. Two cases the ledger does not cover are stated here so that W194 answers them: a closed basin (a pond, a dam's lake) has no downstream link, so a difference in its stored volume stays with the tile and is never spent; and a lasting change of forcing (a source's rate changed, a body lodged in the flow) is, like a dam a player built, a change the bake does not know, so such a tile never settles to the baked answer and falls under ADR-0080's eighth decision for the owner. The bed has the same seam (W178's grounding): a baked river's bed is in transport equilibrium, not at rest, so a domain whose bed may move is fed the baked transport at its inlet and hands the excess back at its outlet, or it scours from its own edge.

### Level of detail

In the order the stages build it: **one domain, tiled from the start, at one cell size**; then **whole tiles asleep or awake** by need and by the observers' distance ([ADR-0010](../adr/0010-deterministic-sim-and-lod-contract.md)), a sleeping tile drawing its baked answer; then **nested tiles two to one**, a far tile at twice the cell, the coarse face receiving exactly the sum of what its fine faces sent over all of the fine side's sub-steps; and beyond every tile the baked far field. A tile retires when its state has settled to the baked answer within the bound W176's check states; an unsettled tile retires only beyond every observer's range (by distance with hysteresis, never a view frustum), its difference going to the ledger. Stepping far tiles less often and the cheaper waves where they are provably enough ([grounding §1](../experiments/water-state-grounding.md#what-hydrology-uses-instead-three-cheaper-waves)) are shortcuts and come after, each with its switch.

**Steep ground stays baked.** What was read: a uniform flow on Manning friction breaks into roll waves past a Froude number of 1.5, which is physics a solver should show and cannot hold still; the wet and dry schemes read were tested on gentle slopes only (about 0.05); and a decimetre grid resolves neither a step's jet nor its clasts. What was not read: the slope at which the hydrostatic assumption itself fails (two sources reached only as search summaries, so no number is used here). So the rule is by the steady solve's own classing of reaches: a step-pool or cascade reach keeps its reach-averaged answer, and its steps enter a live pool below them as sources of momentum, air and foam. The pools, the confluences, the water behind a dam and the shore are where the state runs.

### Tiers

| tier | what runs | what it costs a scene with no tile awake |
|---|---|---|
| baked | the steady solve and the closed forms, as today | nothing: no pass, no pool, no branch |
| mixed | baked everywhere; the state in tiles something woke (an event, a changed source, an observer near a marked domain), started from the baked answer and retired to it | nothing until a tile wakes |
| live | the state in every declared domain within the observers' range | the awake tiles' step and surface |

The tier is the game's setting; the cell classes, the range in tiles and the count of carried scalars scale it to a card. No part of the design assumes ray tracing or mesh shaders, and a host with no device runs baked.

### The clocks, and what a tick costs

The state advances with the simulation's ticks on **the world clock** (ADR-0056, ADR-0010). A tick is cut into the sub-steps the reference's own rule asks, Δt ≤ 0.45 Δx / (max(|u| + c) + max(|v| + c)), so the two can be compared step for step. By that rule (*derived*; c = √(g h)): a stream 5 cm deep at 1 m/s on 0.25 m cells may step 47 ms, a still pool a metre deep on 0.5 m cells 36 ms, a surf zone 3 m deep at 2 m/s on 0.5 m cells 17.5 ms, the same zone out to 10 m of water 11 ms, and a 2 m fall's foot on 0.25 m cells 14.6 ms. So **a stream and a pool take one sub-step a 60 Hz tick, a surf zone one or two, and deep or fast water more**. The count is one for a whole domain, taken from the fastest speed over all its awake tiles by a reduction in a fixed order (as the reference takes its maxima over its whole grid, so two tiles of one size never step a seam at different counts), and it is reported; a tick's cost is cells × sub-steps × the world's rate and **follows the water**, which is the truth of an explicit step. A ceiling on the count is a shortcut, not part of the first form. Whether the count may be taken from the tick before (a read-back is a tick late) is W191's to show against the reference.

The tide at a surf zone's seaward edge is a level by the sky's hour and the date. What is slow (a seasonal inflow, the water table and the store's deep drying, a shore's section) is **given at a calendar date** and never integrated through a gap (W187); a calendar jump restarts every awake tile from the steady answer for the new date.

### What drawing reads

Inside an awake tile the surface is no longer a mesh built at load. Three things change, in the stages' order. **The surface's heights**: a regular grid over the tile whose vertices take the state's level each tick in the deformed-vertex pool, drawn with bounds taken from the tile's own range of levels (a rest pose's cluster bounds and errors would be wrong for a surface that heaves) and with a level of detail the rasterizers and the ray chain already understand: the grid built as a cluster hierarchy whose every level carries an error set from its spacing (a flat rest pose would otherwise simplify to nothing), the cull pass's bounds padded by the tile's range of levels; the traced structures are built from the same positions, as every deforming mesh's are. **The shore** stays where it is drawn today, per pixel, where the level meets the bed: the resolve reads the tile's bed and level in place of the load-time depth map, so the waterline is not the grid's sawtooth. **The foam** is the carried scalar in place of the closed forms, under the same lace. Which of today's closed forms each stage switches off inside a tile is [the grounding page's table](../experiments/water-state-grounding.md#which-of-todays-closed-forms-become-consequences-and-which-stay); outside a tile every one of them stands.

The game's question about the water at a point keeps its guarantee (the same bits on every toolchain): it answers from the baked answer, and from a state only where that state is `hashed` (ADR-0080, decision 11 and the owner's decision 1).

### The budget

**Measured for the step alone, on one card** (W175, 2026-11-26), and set from measurements, not before them. The target is the frame the engine is judged on: 3840 × 2160 with traced visibility and shadows. Four lines are the state's: **the step** (cells × sub-steps × rate); **the surface** (the pool's stage over the awake tiles' vertices); **the traced structures** (an awake tile's surface is rebuilt or refitted every tick); **the resolve's reads** (the tile's bed, level and foam in place of the depth map). For the step alone W175 measured, on the RTX 5090 beside other work (so an upper bound), 0.098 to 0.106 ms a sub-step at 2,048² cells, which is 0.023 to 0.025 ms for a million cells, for a lake, a stream's reach and a beach with friction and no sources, and one sub-step a tick at 0.5 m cells for those three states. That is about an eighth of what this page had estimated from a 2010 card's published figure. It is the cost of depth and momenta only: it does not yet include writing the stress at the bed, the dissipation or a carried scalar, the ground's take, or a source; the older card is not measured; and the surface, the traced structures and the resolve's reads are not measured at all.

**By card** (proposed; W175's figure says that for depth and momenta the step is not what bounds the fastest class, so these ceilings wait on the full state's, the surface's and the structures' cost): the fastest class runs up to two million live cells (about 170 MB); the middle class half a million; the oldest cards the engine is tested on run baked, with up to a hundred thousand live cells where a scene asks. Each is a ceiling the load checks against the scene's declared tiles.

### First results (W175 and W184, 2026-11-26)

(Each row's result was read independently before it was merged.)

- **The step is cheap on the fastest card** (the budget, above), **and it is the same step as far as its cases reach**: on the dam breaks the card and the reference differ by about 1e-7 m in L1, a thousandth of the stated bound; the slope in two dimensions is compared cell by cell with all four edges open; and an independent reading of the shader against the reference's source found the same operations. Not yet shown: a cell-by-cell difference in two dimensions that is held and not only reported, and any second device.
- **A lake at rest needs the datum.** In single precision, with world heights near 2,000 m and no datum, every wet cell of a small lake moved (2,592 of 3,072 cells); with the bed as a height from the grid's own datum an exact lake did not move at all and an inexact one drifted by 6e-8 m. One datum for a domain is a requirement of the first form, not a nicety.
- **A slow exchange needs its own arithmetic.** A loam's take from a pond a metre deep came out 3.3 % short with the plain step, no better on a clock eight times coarser, 0.4 % short applied every 64 steps, and 0.001 % short with a compensated sum that costs 8 bytes a cell. The compensated sum is the first form's, and it adds a plane to the state.
- **The same bytes on every device are not free, and not yet shown.** A step built from addition, subtraction and multiplication alone cost 17 to 59 % more than the native one on the one card measured, and no second device has been compared. The shader is also compiled with contraction forbidden by a flag (`-fp-mode precise`), because the shading language's `precise` keyword was found to mark nothing when the compiler in use (Slang 2026.17.1) writes SPIR-V directly; other shaders in the tree that rely on the keyword are in the same position, which is a finding for their own rows. ADR-0080's first decision for the owner stands as recommended until a second device is measured.
- **The reference does not yet hold a lake's level over a lip the bed steps up to.** Where the bed rises between two cells by a fraction of the depth or more, the hydrostatic reconstruction carries too little of the discharge through the face and the level upstream stands too high: 38 and 48 % above the crest's critical energy at the two discharges of the hollow W176 first specified, with its half-metre wall, the same at twice the cell where that was measured and, by the derivation, independent of the cell. The mechanism is on the reference's sections above; the published account of it was read only as far as its abstract. So the reference is trusted today for conservation, a lake at rest, dam breaks and bores, the ground's take and its bytes, and **not** for a dam's lip, a weir, a ledge in a stream, or thin water over terrain sampled coarser than its relief; a foam fed by dissipation would also light up wrongly there, which a source taken from a resolved jump would not. The scheme needs a reconstruction that keeps moving water steady before W176's tie is built or W192's dam is judged: that is row W199.
### The stages

| row | stage | needs |
|---|---|---|
| W184 | the full equations on the CPU, the reference | — |
| W175 | the step on the card, held to the reference and measured | W184 |
| W176 | the check that a settled state stands where the steady solve says | W184 |
| W191 | the state in a running scene: one domain, tiled from the start, one cell size; sources and events on the world clock; advanced by ticks; pools on first use; absent free | W184, W175 |
| W192 | drawing reads the state: the surface, the shore per pixel, the traced structures; a pond that fills and a dam that spills, judged standing alone | W191, W199 |
| W193 | the foam as a consequence: fed by the energy lost at a bore or a jump, carried, fading; a stream's white and a wave's the same | W192 |
| W194 | the tiers: baked, live and mixed as the game's setting; waking by observers and retiring; the ledger at the seam; what a save carries | W192, W176 |
| W195 | tiles by distance and nested two to one, the range and the memory by card, the state's lines in the frame budgets | W194 |
| W196 | the surf: a corridor driven at an edge that generates the train and lets the reflected one leave (an edge the reference gains first), breaking as bores, the swash as a front | W193, W195 |
| W197 | the ground's store drawn: wet ground as a consequence, a losing reach | W192 |
| W198 | the first shortcuts, one switch each, each judged by the owner's eye | W195, T232 |
| W199 | the reference holds moving water over a bed that steps between cells, and gains second order | W184 |

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
| Tests and size table | `tests/water_state_tests.cpp`, `tests/hollow_tests.cpp`; `tests/size_table.cpp` pins `Ledger`, `CarriedLedger` and `Edges` (the state has no per-cell struct) | the cases above |
| Bench | `domain/water_state/bench/water_state_bench.cpp` | one step at three grids; the numbers are under "Testing" |
| Removal proof | `ENGINE_WITH_WATER_STATE`, off in the minimal build | works |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_WATER_STATE=OFF`) drops the
module, its tests, and its bench; the module disappears from `build/<preset>/modules.json` and
is listed there under `disabled_capabilities`. Everything else must still build and pass.
