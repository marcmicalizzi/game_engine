# E19: a lattice cage around a rigid two-bone core, pressed to 30% and released

- **Question ([10 §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing), [09 §9.6](../plan/09-testing-profiling.md#96-deformable-volume-experiments)):** does an SDF-generated lattice cage around a rigid internal structure stay stable and preserve volume under sustained compression, and at what cost per tick? It decides whether the lattice is [ADR-0026](../adr/0026-deformable-volumes-first-class.md)'s v1 cage kind, and the default element, sub-step, and iteration budgets.
- **Date:** 2026-09-17, re-measured 2026-09-18. **Machine:** Intel Core i9-10980XE, 18 cores / 36 threads, Windows 11. **Build:** `msvc-release` (RelWithDebInfo), Jolt 5.6 with `CROSS_PLATFORM_DETERMINISTIC` on, SSE4.2 baseline.
- **Decision:** the lattice **is** the v1 cage kind; the default budget is **at most 256 elements at 8 iterations and 2 sub-steps** ([ADR-0029](../adr/0029-deformable-volume-budgets.md), which replaced the 512 this page first recommended); the T0 cost of that cage is **0.80 ms a tick on one worker**. **Status: Measured, not Done** — two of the four pass criteria still fail after the fixes, and both are about the fixture and the budget rather than about the cage.

> **The 2026-09-17 numbers on this page were taken without recording what else the machine was doing.** This is a shared desktop: the owner runs GPU diffusion jobs on it and other agents compile on it. Nothing on this page from that date carries a machine state, so every figure from it is an **upper bound** of what a quiet machine would produce. The 2026-09-18 re-measurement below records the state beside every number, and it reproduces the original cost constant to within 3%, which is the best evidence available that the original run was not badly contended.

## Method

### The module

`domain/physics` ([page](../subsystems/physics.md)) gained three things for this experiment, all of them things ADR-0026 asks for anyway:

- **A second attachment kind.** `SoftAttachment::kind` is `Rigid` (zero inverse mass, the particle covers the whole gap to its anchor each step) or `Spring` (the particle keeps its mass and its velocity is steered `follow_rate * dt` of the way there). Only `Rigid` existed, and with only `Rigid` this experiment could not have been run honestly: a zero-mass particle cannot be pushed by contact, so "no element passes through the core" would have been true by construction. [05 §5.14](../plan/05-simulation.md#514-deformable-volumes) asks for the spring form in as many words — "'bound' is a stiff spring rather than a weld, so flesh lags a fast bone instead of tracking it exactly".
- **`soft_body_solve_width`**, and the finding behind it (below).
- **`WorldStats::soft_body_solve_jobs` / `soft_body_solve_workers`**, so "did the solve actually spread across the pool?" is a number rather than a profiler session.

Since [ADR-0029](../adr/0029-deformable-volume-budgets.md) (2026-09-18) it has three more, and they are what the re-measurement below tests: **`SoftBodyDesc::max_strain`** and the clamp that enforces it, **`volume_compliance_for()`** in `domain/physics/deformable.h`, and **`WorldStats::soft_body_budget`** — the wall clock the last step spent on deformables, split between the ambient set and a volume flagged `hero`, with how far each is over its budget and how many sweeps the clamp needed.

### The fixture

`domain/physics/bench/e19_bench.cpp`, three benchmarks in the module's bench executable, so the whole thing re-runs anywhere the engine builds:

```powershell
pwsh tools/dev.ps1 build -Preset msvc-release
build/msvc-release/domain/physics/engine_physics_bench.exe --filter=physics.e19.tick --json=e19-tick.jsonl
build/msvc-release/domain/physics/engine_physics_bench.exe --filter=physics.e19.press --repeats=1 --warmup=0 --quiet
build/msvc-release/domain/physics/engine_physics_bench.exe --filter=physics.e19.sustained --repeats=1 --warmup=0 --quiet
```

The last two print one JSON object per line per configuration, so their stdout is a JSON-lines file; the first goes through the bench harness's own `--json`. A benchmark argument is one integer, so the sweep dimensions are packed as `mode * 1e9 + elements * 1e6 + workers * 1e4 + iterations * 10 + sub_steps` — `512080162` is 512 elements, 8 workers, 16 iterations, 2 sub-steps with ADR-0029's fixes in, and `1512080162` is the same configuration as the experiment first ran it. Mode 2 is the strain clamp alone and mode 3 the compliance conversion alone.

**The cage.** `build_lattice_volume(n, spacing, ...)`, a regular n×n×n lattice with axis edges, face diagonals, all four body diagonals, and the six Kuhn tetrahedra per cell. The cube is **0.40 m on a side at every element count**, so the sweep is about cage *resolution* and not cage size; n = 7, 8, 9 gives 343 / 512 / 729 particles for the plan's nominal 300 / 500 / 800. Mass is `material.density` × volume from [07 §7.10](../plan/07-content-pipeline.md#710-deformable-volume-assets) — 1,000 kg/m³ over 0.064 m³, so 64 kg spread evenly over the particles. Edge compliance 1e-4 ("foam and flesh, not steel"); volume compliance **1e-8**, which is not a typo and is the subject of the first result below. Particle radius is a third of the cell, because a particle is a point to the solver otherwise and a point driven a few millimetres into the plate gets pushed out through whichever face is nearest, which is as often the side as the bottom.

**The core.** Two capsules of 40 mm radius in a shallow elbow through the cage's centre, kinematic and driven with `move_kinematic` every step. They are held still: E19 is about compression around a rigid internal structure, and a moving skeleton is E20's question. Cage vertices **inside** a capsule are `Rigid`-attached to it (an element inside the bone must not be pushable out of the bone); the shell within 45 mm of the surface is `Spring`-attached at 30/s (tissue, which lags and can be pressed). That split is what an SDF-generated cage around a skeleton produces anyway, and it is the only reason the interpenetration count means anything: 5–12 attached particles are inside the core by construction and are excluded, and everything counted is a free particle that got there.

**The press.** A kinematic box, 0.4 m thick, driven down over 0.5 s until the cage's vertical extent is 70% of its rest height — 30% of its depth, as the plan's row says — held for 1 s, then lifted clear over 0.33 s, then 3 s of recovery. The cage rests on a static ground plane throughout, so it is squeezed between the plate, the ground, and the core.

**The sustained case** is E22's shape on the same fixture, which costs nothing extra to run: the cage settles, a rigid box of 1× and of 5× its own mass is dropped on it, and it holds for 20 s.

### What is recorded

Peak and held **edge stretch ratio** (current length over rest, maximum over all edges), **volume ratio** from the closed cage surface by the divergence theorem, **recovery time** to 95% of rest height measured from when the plate starts to leave, **RMS offset** from the settled rest pose one second after release, **interpenetration count** (free particles strictly inside a core capsule), and **microseconds per tick**. A cage that produces a non-finite position is flagged `diverged` and its metrics freeze at the last finite pose, because a table of `nan` makes a reader do the work.

## Results

### The finding that was not on the list: volume compliance is not edge compliance

The module takes one `compliance` per constraint, in the XPBD sense, and the obvious thing to do — which this fixture did first — is to give the edges and the tetrahedra the same number. That is wrong, and not by a little.

XPBD divides a constraint's compliance by `dt²` and weighs it against `Σ wᵢ|∇C|²`. For an **edge** the gradient is a unit vector, so at 0.19 kg a particle that sum is about 10 and a compliance of 1e-4 (0.36 after the division) is a stiff edge. For a **tetrahedron's volume** the gradient is an *area*: for a 6 cm cell that sum is about 1e-4, four orders of magnitude smaller, and the same 1e-4 does not soften the constraint — it switches it off.

| Volume compliance | Volume at release | Volume 1 s later | RMS from rest 1 s later | Recovered to 95% height? |
|---|---|---|---|---|
| 1e-4 (same as the edges) | 0.665–0.681 | 0.665–0.682 | 67–73 mm | never, at any of the six iteration and sub-step combinations |
| 1e-8 | 0.999–1.000 | 1.000 | 0.02–0.05 mm | 17–133 ms |

The cage at 1e-4 does not sag and recover slowly; it takes a permanent set at two thirds of its volume and stays there. **The rule is that volume compliance has to scale with cell size** — the sum above goes as (cell edge)/(density), so a cage generated at half the element size wants half the volume compliance for the same stiffness. That is a content-pipeline concern, and it is the one thing here that has to reach [07 §7.10](../plan/07-content-pipeline.md#710-deformable-volume-assets) before assets are authored: `material.volume_preservation` is a 0–1 weight in that table, and whatever converts it into a compliance has to know the cell size. Every other number on this page was measured at 1e-8.

### How wide one cage's solve can go

`SoftBodySharedSettings::Optimize()` partitions a cage's **vertices** into batches of at most 256 and gives each batch's constraints their own update group; the constraints that straddle two batches go into one trailing group that is solved after the parallel ones. So the width of a cage's constraint solve is `ceil(vertices / 256)`, and it is a property of the cage:

| Cage | Particles | Solve width |
|---|---|---|
| E19 small | 343 | 2 |
| E19 default | 512 | 2 |
| E19 large | 729 | 3 |

**One 512-particle cage is a two-wide solve on a 36-thread machine**, and the third and eighth workers have nothing to claim — worse than nothing, because a solve job that finds no group yields and retries until the step's iterations are done, so on a pinned pool they are busy doing nothing. The cost table below is what that looks like.

### The press: 30% of the depth, held a second, released

Eight workers throughout, because the cage's shape does not depend on how many ways the solve
was split (the module's parallel test asserts that two cages stepped 120 times with 1 and with 8
workers give bit-identical particle positions). "Recovered" means the cage's vertical extent
reached 95% of its rest height, timed from the moment the plate starts to lift.

| Particles | Iterations | Sub-steps | Held height | Peak strain | Held strain | Min volume under load | Volume at release | Recovery | RMS at +1 s | Free particles in the core |
|---|---|---|---|---|---|---|---|---|---|---|
| 343 | 4 | 1 | 0.74 | 1.99 | 1.94 | 0.744 | 1.000 | 50 ms | 0.02 mm | 5 |
| 343 | 4 | 2 | 0.73 | 1.88 | 1.80 | 0.774 | 1.000 | 33 ms | 0.03 mm | 2 |
| 343 | 8 | 1 | 0.65 | 2.32 | 1.85 | 0.665 | 1.000 | 50 ms | 0.04 mm | 7 |
| 343 | 8 | 2 | 0.69 | 1.95 | 1.88 | 0.770 | 0.999 | 50 ms | 0.05 mm | 1 |
| 343 | 16 | 1 | 0.66 | 2.17 | 1.86 | 0.764 | 1.000 | 17 ms | 0.02 mm | 5 |
| 343 | 16 | 2 | — | — | — | — | — | **diverged at 0.68 s** | — | 2 |
| **512** | 4 | 1 | 0.66 | 2.16 | 2.06 | 0.663 | 0.998 | 133 ms | 0.01 mm | 8 |
| **512** | 4 | 2 | 0.71 | 2.06 | 2.06 | 0.760 | 1.000 | 50 ms | 0.01 mm | 1 |
| **512** | 8 | 1 | 0.65 | 2.09 | 2.06 | 0.679 | **0.724** | never | **54.7 mm** | 12 |
| **512** | **8** | **2** | 0.65 | 1.91 | 1.90 | 0.772 | 1.000 | 50 ms | 0.01 mm | 2 |
| **512** | 16 | 1 | 0.67 | 2.06 | 1.95 | 0.679 | **0.727** | never | **54.6 mm** | 11 |
| **512** | 16 | 2 | 0.72 | 1.84 | 1.76 | 0.772 | 1.000 | 50 ms | 0.00 mm | 1 |
| 729 | 4 | 1 | 0.67 | 2.81 | 2.29 | 0.747 | 1.000 | 50 ms | 0.01 mm | 2 |
| 729 | 4 | 2 | 0.70 | 2.44 | 2.22 | 0.724 | 0.999 | 50 ms | 0.42 mm | 3 |
| 729 | 8 | 1 | 0.66 | 2.81 | 2.02 | 0.729 | 1.000 | 50 ms | 0.38 mm | 2 |
| 729 | 8 | 2 | 0.70 | 2.40 | 2.37 | 0.698 | **0.772** | never | **51.3 mm** | 3 |
| 729 | 16 | 1 | 0.71 | 2.41 | 2.41 | 0.699 | **0.768** | never | **51.3 mm** | 3 |
| 729 | 16 | 2 | 0.70 | 2.37 | 2.32 | 0.699 | **0.773** | never | **51.3 mm** | 5 |

Reading it:

- **When the cage comes back, it comes back exactly.** Twelve of the eighteen configurations
  return to 1.000 of rest volume within 17–133 ms of the plate lifting and sit 0.00–0.05 mm RMS
  from their own settled pose a second later. E19's 1 mm RMS criterion is not close to being the
  binding one; it passes by two orders of magnitude.
- **When it does not, it takes a permanent set** at 0.72–0.77 of rest volume and about 54 mm RMS,
  and no amount of further simulation moves it. There is no slow creep back: it is a latch.
- **More work is not monotonically better.** At 512 particles, one sub-step recovers at four
  iterations and fails at eight and sixteen; two sub-steps recover at all three. At 729,
  *raising* the budget from 4 iterations to 8 or 16 turns a recovery into a latch. That is the
  signature of cell inversion rather than of under-convergence: once a tetrahedron is inside
  out, its volume constraint is pushing it the wrong way, and iterating harder pushes harder.
- **Volume under load is nowhere near the 5% criterion**, at any budget: the cage gives up
  23–34% of its volume while the plate is on it. That is not the contact boundary conditions —
  dropping the friction at both flat contacts from 0.6 to 0.1, so the cage is free to barrel
  outwards, moves the minimum by under a percentage point and changes no verdict in the table.
- **Strain is far outside the authored limit.** [07 §7.10](../plan/07-content-pipeline.md#710-deformable-volume-assets)
  gives `limits.max_strain` a default of 0.5, which is a stretch ratio of 1.5; the held ratio is
  1.76–2.41 and the transient peak reaches 2.81. The backend has no per-element strain clamp at
  all, so the limit in that table is currently a number nothing enforces.
- **Free particles do get inside the core**, 1 to 12 of them depending on the configuration. The
  ones that are inside by construction — the `Rigid`-attached elements that *are* the bone — are
  excluded from the count, so these are all tissue particles pressed through the capsule wall.

### Sustained load: E22's shape, early data

The same cage (512 particles, 8 iterations, 1 sub-step), settled, then carrying a rigid box of
1× and of 5× its own 64 kg for 20 s.

| Load | Height at 2 s | Height at 20 s | Creep | Oscillation after 2 s | Surface speed at 2 s | Surface speed at 20 s | Plane penetration | Volume at 20 s | µs/tick |
|---|---|---|---|---|---|---|---|---|---|
| 1× (64 kg) | 0.3984 m | 0.3989 m | **−0.53 mm** | 1.34 mm | 62.9 mm/s | **0.008 mm/s** | **0** | 0.997 | 1,290 |
| 5× (320 kg) | 0.3913 m | 0.3942 m | **−2.96 mm** | 5.33 mm | 123.5 mm/s | **0.025 mm/s** | **0** | 0.984 | 1,330 |

Every E22 criterion that a 20-second run can speak to passes, and comfortably: no plane
penetration at any tick, no creep at all (the height rises slightly as the initial overshoot
comes out — the negative numbers are recovery, not sag), the surface is still to 0.03 mm/s a
second after settling with no limit cycle, and volume holds within 1.6% even at five times the
cage's own weight. The cost barely moves between 1× and 5×, which is what one would expect from
a solver whose work is the constraint set and not the load.

This is **early data for E22, not E22**: that row wants 60 seconds and a cushion, and this cage
has a rigid core through the middle of it that a cushion does not.

### Cost per tick

`physics.e19.tick`, median of five repeats after calibration, of the cage **held** at 70% of its
rest height with its full contact set. Microseconds per 60 Hz tick.

**Iterations and sub-steps are each exactly linear, and element count very nearly so** (one
worker, the fastest configuration — see below):

| Particles | 4 it / 1 ss | 4 it / 2 ss | 8 it / 1 ss | 8 it / 2 ss | 16 it / 1 ss | 16 it / 2 ss |
|---|---|---|---|---|---|---|
| 343 | 345 | 680 | 650 | 1,283 | 1,235 | 2,441 |
| 512 | (1,255) | (2,069) | 907 | 1,801 | 1,769 | 3,499 |
| 729 | 711 | 1,414 | 1,347 | 2,694 | 2,611 | 5,213 |

The two 512-particle four-iteration cells are parenthesised because their spread was 43% and 10%
against under 2% everywhere else; they are noise, not a feature. Every other cell fits one
number:

> **0.225 µs per element per iteration per sub-step**, within ±5% across the whole grid — 0.221
> at 512/8/1, 0.237 at 343/8/1, 0.223 at 729/16/2.

That is the cost model the tier table needs, and it is simple enough to author against: a cage's
tick cost is its element count times its iteration count times its sub-step count times a
constant, and the constant is a property of the machine.

**More workers make one cage slower.** Every clean row in the grid says so:

| Particles | Iterations / sub-steps | 1 worker | 4 workers | 8 workers | 8 against 1 |
|---|---|---|---|---|---|
| 343 | 8 / 1 | **650** | 1,054 | 1,098 | 1.69× slower |
| 512 | 8 / 1 | **907** | 1,188 | 1,251 | 1.38× slower |
| 729 | 8 / 1 | **1,347** | 1,615 | 1,730 | 1.28× slower |
| 343 | 16 / 1 | **1,235** | 2,048 | 2,156 | 1.75× slower |
| 512 | 16 / 1 | **1,769** | 2,319 | 2,480 | 1.40× slower |
| 729 | 16 / 1 | **2,611** | 3,148 | 3,384 | 1.30× slower |
| 512 | 16 / 2 | **3,499** | (14,186) | 4,935 | 1.41× slower |

The parenthesised cell is the one other place in the grid where the spread went past a few
percent (136%); the eight-worker column beside it is clean and tells the same story.

This is the solve-width result cashed out. A 512-particle cage has two parallel constraint
groups; the backend still creates one solve job per worker, and the six that find no group to
claim spin and yield until the step's iterations are done. What they buy is a two-way split of
the constraint pass; what they cost is six threads of scheduling, six sets of cache lines
touching the same cage, and a barrier that cannot retire until the slowest of eight has noticed
there is nothing left. The larger the cage the smaller the penalty, which is the same statement
the other way round: at 729 particles there are three groups instead of two, and the penalty
falls from 1.7× to 1.3×.

**Across cages it scales properly.** The module's own soft-cube benchmark, same 8×8×8 lattice at
eight iterations and one sub-step, one cage against eight:

| | 1 worker | 4 workers | 8 workers | per cage at 8 workers |
|---|---|---|---|---|
| 1 cage, 512 particles | **965 µs** | 1,163 µs | 1,232 µs | 1,232 µs |
| 8 cages, 4,096 particles | 7,475 µs | 2,236 µs | **1,159 µs** | **145 µs** |

Eight cages at eight workers is a **6.45× speedup** over the same eight cages on one worker, and
it costs *less than one cage does* on the same pool. Per cage it is **8.5× cheaper**. The
backend's solve jobs take the next available constraint group from any active cage, so eight
two-group cages give eight threads something to do where one two-group cage gives two of them
something and six of them nothing.

The rule that falls out, and it is the most useful thing on this page:

> **Size cages for the solve width, and run them together.** Two 400-element cages beat one
> 800-element cage, and eight of them beat one by nearly an order of magnitude per cage. A
> deformation system that steps volumes in one world gets this for free; one that gives each
> volume its own world or its own step throws it away.

The rigid-body side is unchanged by any of this: 1,000 boxes step in 2.40 ms at one worker,
1.08 ms at four, 736 µs at eight, and 2.43 ms with no job system at all.

## Re-measured 2026-09-18, with ADR-0029's fixes

[ADR-0029](../adr/0029-deformable-volume-budgets.md) built the two things the section below asks
for — a volume compliance derived from the cell size, and a `limits.max_strain` that something
enforces — and the whole experiment was re-run. Everything in this section is `msvc-release` on
the same i9-10980XE.

### What the machine was doing

The 2026-09-17 numbers above were taken without recording this, which is why they are upper
bounds. This desktop is shared: the owner runs GPU diffusion jobs on it (full VRAM, 100% GPU) and
other agents compile on it. CPU figures are `\Processor(_Total)\% Processor Time` over 36 threads,
so 5% is about 1.8 threads busy.

| Run | CPU before | CPU after | GPU |
|---|---|---|---|
| Press grid (32 configurations) | 5.9, 8.0, 7.7% | 11.9, 9.3, 10.5% | 3.0 GB, 0% |
| Cost grid (63 configurations) | 3.1, 4.0, 4.2, 5.2% | 3.0, 2.9, 6.4% | 3.0 GB, 0% |
| Module step bench | 6.4, 8.1, 9.6% | 5.9, 1.7, 3.7% | idle |
| Sustained load | 8.0, 6.2, 9.6% | 8.0, 15.6, 24.5% | 3.1 GB, 0% |

The cost grid — the one the budget is written against — ran in the quietest window of the session
and every cell but four came out with a spread under 6%. Waiting for that window took three
attempts; one earlier run of the same grid caught a burst of 80–160% spread on its first ten
configurations and was thrown away rather than reported. **The press grid's own
`hold_us_per_tick` column is not the cost measurement** and two of its rows caught interference
(343/8 at 8.7 ms against a modelled 1.4); `physics.e19.tick` is the authority and it is clean.

### The fixture now has a control

The re-run is worthless as a comparison if the only difference between "before" and "after" is a
different day, so the bench grew a `mode` dimension and the failing configurations run **in the
same session** under four settings: `fixed` (both changes), `unfixed` (the fixture exactly as it
first ran), and `clamp-only` / `conversion-only`, which turn one change on at a time so a row that
moved can say which change moved it. The commands are unchanged; the controls are extra keys.

**The control reproduces the original failures to three decimal places**, which is what makes
everything else on this page comparable:

| Configuration, `unfixed` | 2026-09-17 | 2026-09-18 |
|---|---|---|
| 512 / 8 it / 1 ss | latched, volume 0.724, 54.7 mm RMS | latched, volume 0.724, **54.72 mm** |
| 729 / 8 it / 2 ss | latched, volume 0.772, 51.3 mm RMS | latched, volume 0.772, **51.26 mm** |
| 343 / 16 it / 2 ss | diverged at 0.68 s | diverged |
| 512 / 16 it / 2 ss | recovered, 1.000 | recovered, 1.000 |

### The press, with the fixes in

Twenty configurations, eight workers, `fixed`. "Sweeps" is the mean number of passes the strain
clamp took per tick of the hold.

| Particles | It | Sub | Held height | Peak strain | Held strain | Min volume | Volume at release | Recovery | RMS at +1 s | In core | Sweeps |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **216** | 8 | 1 | 0.64 | 1.500 | 1.500 | 0.79 | 1.000 | 67 ms | 0.00 mm | 3 | 2.0 |
| **216** | **8** | **2** | 0.64 | 1.500 | 1.500 | 0.79 | 1.000 | 50 ms | 0.00 mm | **0** | 2.3 |
| 343 | 4 | 1 | 0.75 | 1.500 | 1.500 | 0.75 | 1.000 | 50 ms | 0.03 mm | 3 | 6.9 |
| 343 | 4 | 2 | 0.72 | 1.500 | 1.500 | 0.78 | 1.000 | 33 ms | 0.01 mm | 4 | 6.7 |
| 343 | 8 | 1 | 0.73 | 1.500 | 1.500 | 0.78 | 1.000 | 17 ms | 0.01 mm | 2 | 7.2 |
| 343 | 8 | 2 | 0.70 | 1.500 | 1.500 | 0.79 | 1.000 | 50 ms | 0.03 mm | 2 | 8.0 |
| 343 | 16 | 1 | 0.70 | 1.500 | 1.500 | 0.79 | 1.000 | 17 ms | 0.00 mm | 3 | 8.4 |
| 343 | 16 | 2 | 0.72 | 1.500 | 1.500 | 0.80 | 1.000 | 33 ms | 0.01 mm | 2 | 9.9 |
| 512 | 4 | 1 | 0.76 | 1.500 | 1.500 | 0.74 | 1.000 | 33 ms | 0.01 mm | 2 | 15.0 |
| 512 | 4 | 2 | 0.77 | 1.500 | 1.500 | 0.74 | 1.000 | 33 ms | 0.00 mm | 1 | 13.1 |
| 512 | 8 | 1 | 0.70 | 1.500 | 1.500 | 0.77 | 1.000 | 17 ms | 0.00 mm | 1 | 14.6 |
| **512** | **8** | **2** | 0.68 | 1.500 | 1.500 | 0.80 | 1.000 | 50 ms | 0.00 mm | 1 | 12.4 |
| 512 | 16 | 1 | 0.68 | 1.500 | 1.500 | 0.80 | 1.000 | 17 ms | 0.00 mm | 2 | 14.0 |
| 512 | 16 | 2 | — | — | — | — | — | **diverged** | — | — | — |
| 729 | 4 | 1 | 0.73 | 1.500 | 1.500 | 0.75 | 1.000 | 50 ms | 0.04 mm | 3 | 12.0 |
| 729 | 4 | 2 | 0.73 | 1.500 | 1.500 | 0.78 | 1.000 | 50 ms | 0.07 mm | 3 | 11.0 |
| 729 | 8 | 1 | 0.69 | 1.500 | 1.500 | 0.78 | 1.000 | 17 ms | 0.22 mm | 2 | 11.0 |
| 729 | 8 | 2 | 0.73 | 1.500 | 1.500 | 0.78 | 1.000 | 50 ms | 0.30 mm | 5 | 8.9 |
| 729 | 16 | 1 | 0.68 | 1.500 | 1.500 | 0.79 | 1.000 | 17 ms | 0.09 mm | 3 | 10.5 |
| 729 | 16 | 2 | — | — | — | — | — | **diverged** | — | — | — |

Reading it against the table above:

- **The latch is gone.** Three configurations used to take a permanent set at 0.72–0.77 of rest
  volume and about 54 mm RMS, with no way back. None does now: eighteen of twenty return to
  **1.000** of rest volume and 0.00–0.30 mm of their own settled pose. The one criterion this page
  said passed by two orders of magnitude now passes everywhere rather than in twelve rows of
  eighteen.
- **The authored limit is the limit.** Held stretch is **1.5000** in every surviving row, against
  1.76–2.41 before, and the transient peak is 1.500 too rather than 2.81. `limits.max_strain` has
  stopped being a number nothing reads.
- **Elements stop getting into the core.** The free-particle count falls from 1–12 to 0–5, and
  ADR-0029's default cage is the only configuration ever measured at **zero**. The criterion still
  fails for everything else, but by a quarter of what it did.
- **Volume under load has not moved**, and was never going to: 0.74–0.80 of rest, the same
  20–26% the first run measured. The volume constraint is a soft constraint losing an argument
  with a kinematic plate driven through a third of the cage's depth, and no clamp changes that.
- **Two configurations regress**, and the controls say which change did it.

### What broke at sixteen iterations and two sub-steps, and which change broke it

512 and 729 elements at 16 iterations *and* 2 sub-steps now diverge where they used to recover and
latch. Running the four modes on the same three configurations in one session separates the
causes:

| Configuration | `unfixed` | `clamp-only` | `conversion-only` | `fixed` |
|---|---|---|---|---|
| 343 / 16 / 2 | diverged | diverged | diverged | **recovered, 1.000, 0.01 mm** |
| 512 / 16 / 2 | recovered, 1.000 | recovered, 1.000 | **diverged** | **diverged** |
| 729 / 16 / 2 | latched, 0.772 | latched, 0.772 (strain 1.500) | **diverged** | **diverged** |

The clamp alone never makes a configuration worse; the conversion alone reproduces both failures
exactly. The mechanism is that **Jolt's `mNumIterations` is an XPBD sub-step count, not a
Gauss-Seidel iteration count** — it integrates positions inside each one — so 16 iterations at 2
collision sub-steps is *thirty-two* position solves a tick at a sub-step of 1/1920 s, and the
compliance the conversion returns for the plan's default preservation of 0.8 (4.2e-9 at this
fixture's 5.71 cm cell) is 2.4× stiffer than the 1e-8 the first run found by hand. That
combination is past what the cage conditions for. It is outside ADR-0029's default envelope — 8
iterations, 2 sub-steps — and it is exactly the conditioning limit [E23](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing)
exists to pin.

The finding to carry forward: **raising the iteration count is not a safety margin.** It was
already true that it does not buy stability (sub-steps do); it is now measured that past the
default it can cost it.

### Cost, and what the clamp costs

`physics.e19.tick`, median of five repeats, one worker, the cage held at 70% of its rest height.
Microseconds per 60 Hz tick. Parenthesised cells had a spread above 60% and are noise, not a
feature — the same two cells were noise in the first run.

| Particles | 4 it / 1 ss | 4 it / 2 ss | 8 it / 1 ss | 8 it / 2 ss | 16 it / 1 ss | 16 it / 2 ss |
|---|---|---|---|---|---|---|
| **216** | — | — | **416** | **803** | — | — |
| 343 | 436 | 771 | 731 | 1,377 | 1,378 | 2,590 |
| 512 | (1,426) | (3,431) | 1,229 | 2,067 | 2,061 | 3,499 |
| 729 | (1,110) | (2,443) | 1,706 | 2,962 | 2,955 | 5,197 |

**The solve itself has not changed, and the control proves it.** The `unfixed` rows at 8
iterations and 1 sub-step — same cage, no clamp — are 399, 637, 903 and 1,337 µs at 216, 343, 512
and 729 particles, which is **0.231, 0.232, 0.220 and 0.229 µs per element per iteration per
sub-step**. The first run's constant was 0.225 ±5%. Two measurements of the same thing five
weeks apart agreeing to 3% is the strongest statement this page can make about its own numbers.

**The clamp is the difference, and its cost is a product of two numbers the world reports.**

| Particles | Edges | Sweeps a tick | Edge visits | Without clamp | With clamp | Clamp | Per visit |
|---|---|---|---|---|---|---|---|
| **216** | 1,940 | 2.0 | 3.9 k | 399 µs | 416 µs | **+17 µs** | 4.3 ns |
| 343 | 3,258 | 7.2 | 23.3 k | 637 µs | 731 µs | +94 µs | 4.0 ns |
| 512 | 5,068 | 14.6 | 73.7 k | 903 µs | 1,229 µs | +326 µs | 4.4 ns |
| 729 | 7,448 | 11.0 | 81.8 k | 1,337 µs | 1,706 µs | +369 µs | 4.5 ns |

So the clamp costs `edges × sweeps × 4.3 ns` and nothing else, and the sweep count is what the
cage's own geometry and load decide. **The default cage pays 4% of its tick for it and a
512-element one pays 27%**, which is a second, independent reason for ADR-0029's cage size that
has nothing to do with the solve width.

Two details of the clamp were measured rather than reasoned about, and both went against the
first guess:

- **The exit condition has to be a tolerance, not "a sweep corrected nothing".** With the
  obvious exit the press burned all sixteen sweeps on every one of the sixty hold ticks at every
  element count, because the solver re-stretches what the previous sweep pulled in and
  Gauss-Seidel keeps correcting a smaller amount forever. That was 370 µs a tick at 512 elements
  — a quarter of the step — for no change in the answer. Exiting when the worst edge is inside
  the limit to one part in a thousand gives the same 1.5000 held stretch at 2 to 15 sweeps.
- **The velocity correction belongs at the step's dt, not the backend's sub-step dt.** Matching
  the solver's own `v = (x − x_prev)/dt_sub` looks right and is wrong: the correction is up to 32
  times larger, overshoots, and 343 / 16 / 2 goes from recovering to diverging. At the step's dt,
  and with no velocity correction at all, the grid is indistinguishable (eighteen of twenty
  recover either way) — so the correction is kept, at the step's dt, because it is the one that
  cannot leave the stretch's energy in the velocity.

**The symmetric clamp is the expensive one and it is the right one.** Clamping only stretch
costs 2–3 sweeps instead of 7–15 and recovers the same eighteen configurations, but it lets
**1 to 21** free particles through the rigid core against the symmetric clamp's **0 to 5** — and
"no element passes through the core" is one of E19's four pass criteria while sweep count is not.
The compression half is also the half that stops a tetrahedron going inside out, which is the
mechanism behind every failure this page records.

**Everything else is where it was.** One cage is still slower on more workers (416 → 689 µs at
216 particles, 1,229 → 1,598 at 512, for 1.66× and 1.30×), eight 512-particle cages still cost
7.458 ms at one worker and **1.152 ms at eight** — a 6.47× speedup and 144 µs a cage — and 1,000
boxes still step in 2.376 / 1.071 / 0.744 ms at 1 / 4 / 8 workers. The first run's figures were
7.475 / 1.159 ms and 2.403 / 1.083 / 0.736 ms.

### Sustained load, re-run

Same cage (512 particles, 8 iterations, 1 sub-step), same 20 s, with the fixes in.

| Load | Height at 2 s | Height at 20 s | Creep | Oscillation | Surface speed at 20 s | Plane penetration | Volume at 20 s | µs/tick | Budget |
|---|---|---|---|---|---|---|---|---|---|
| 1× (64 kg) | 0.3983 m | 0.3990 m | **−0.73 mm** | 1.33 mm | **0.009 mm/s** | **0** | 0.997 | 1,289 | 1.25 ms |
| 5× (320 kg) | 0.3935 m | 0.3943 m | **−0.85 mm** | 5.78 mm | **0.010 mm/s** | **0** | 0.985 | 1,328 | 1.29 ms |

Every E22 criterion a 20-second run can speak to still passes, and the 5× recovery improved from
−2.96 mm to −0.85 mm. The cost is unchanged to within 0.3% (1,289 against 1,290 and 1,328 against
1,330 µs), which says what it should: a settled cage has almost no edge outside its limit, so the
clamp exits on its first sweep and costs nothing. The clamp is a cost you pay while something is
pressing on you.

### The budget, in the terms ADR-0029 states it

The `budget` column above is [ADR-0029](../adr/0029-deformable-volume-budgets.md)'s number:
wall clock on the performance pool per tick for the backend's soft-body phase plus the world's own
attachment pre-pass and strain clamp, measured by the world and reported in
`WorldStats::soft_body_budget`. It tracks the whole step to within 2–4% on this fixture, which is
the expected answer — the cage *is* the step here — and is what makes it a usable gate rather
than a profiler session.

Against the 1.5 ms ambient budget, at ADR-0029's defaults (216 elements, 8 iterations, 2
sub-steps):

- **One cage: 0.80 ms at one worker, 1.37 ms at eight.** The eight-worker figure is worse because
  a 216-element cage is one solve group and seven solve jobs spin; it is the cost of being the
  only deformable in the world, not the cost of the cage.
- **Eight cages together: about 1.0 ms**, extrapolating the module's 8 × 512-particle measurement
  (1.152 ms at eight workers, 8 iterations, 1 sub-step) to this element count and sub-step count.
  That is an extrapolation across a different material and one sub-step, not a measurement, and
  it is the number most worth measuring next — but it says ADR-0026's "8–16 volumes at T0" is
  reachable inside 1.5 ms at the smaller cage, where at 512 elements it was not.

## What this decides

**Status note, 2026-09-18: points 2, 3 and 4 below were the owner's to settle and
[ADR-0029](../adr/0029-deformable-volume-budgets.md) settled them.** The default element budget is
**at most 256, not 512** — one backend solve group, which the re-measurement adds a second reason
for (the strain clamp costs 17 µs on a 216-element cage and 326 µs on a 512-element one). The
budget became **1.5 ms ambient plus a 2.5 ms hero allowance, measured as wall clock**, and it is
reported per step by the world. Point 1 is unchanged and point 3's reasoning is unchanged and
now has a sharper edge: raising iterations past the default does not merely fail to buy
stability, it can cost it.

1. **The lattice is the v1 cage kind.** When it recovers it recovers to 0.00–0.05 mm RMS of its
   own rest pose and 1.000 of its rest volume; it carries five times its own weight for twenty
   seconds with no creep, no limit cycle, and no plane penetration; and its cost is a clean
   linear function of three authored numbers. Nothing here argues for a different cage kind at
   this element count, and the failures below are named mechanisms rather than "XPBD is not good
   enough".
2. **Default element budget: 512.** It is [07 §7.10](../plan/07-content-pipeline.md#710-deformable-volume-assets)'s
   `cage.target_elements` default already and the measurement agrees with it for a reason that
   was not obvious: 343 elements is not meaningfully cheaper per unit of work and has the same
   two-wide solve, while 729 is 1.5× the cost *and* the least stable of the three under the
   press (three of its six configurations latched). 512 is the largest cage that is still two
   batches, which is where the parallel penalty is smallest for the work done.
3. **Sub-steps: 2. Iterations: 8.** Sub-steps are the stability knob and iterations are not: at
   512 particles, one sub-step latched at 8 and at 16 iterations while two sub-steps recovered
   at 4, 8 and 16. Both cost the same per unit — 0.225 µs per element per unit — so a budget is
   better spent on the second sub-step than on doubling iterations. Eight iterations at two
   sub-steps is the cheapest configuration in the grid that recovered at every element count
   tested, which is the whole argument for it.
4. **The T0 budget does not survive contact with the measurement.** [05 §5.14](../plan/05-simulation.md#514-deformable-volumes)
   estimates **40–120 µs** for a 300–800-element T0 cage at two sub-steps of eight iterations and
   budgets **1.5 ms per tick for all deformable volumes**. The measured cost of exactly that
   cage is **1,801 µs**: fifteen to forty-five times the estimate, and 1.2× the entire budget for
   one volume. The estimate was not close, and the tier table's "8–16 volumes at T0 plus 30–60
   at T1" is unreachable by more than an order of magnitude at these parameters.

   What the numbers do support, if the deformable set is stepped in one world: **eight
   512-element cages at eight iterations and one sub-step cost 1.16 ms on eight workers**, which
   is inside the budget, and about 2.3 ms at two sub-steps, which is not. So the honest options
   are (a) raise the budget to about 2.5 ms, (b) keep 1.5 ms and accept four T0 cages at the
   recommended settings, or (c) keep 1.5 ms and drop T0 to one sub-step, which this experiment
   says is the setting that latches. This is the owner's decision and it is why the E19 row is
   marked *measured* rather than *done*.

## What failed, and why it is not the cage's fault

**Status note, 2026-09-18: the mechanism named at the end of this section was built
([ADR-0029](../adr/0029-deformable-volume-budgets.md) decision 4) and the criteria now stand as
follows.** The row stays **Measured** and not Done, because two of four still fail.

| Criterion ([09 §9.6](../plan/09-testing-profiling.md#96-deformable-volume-experiments)) | 2026-09-17 | 2026-09-18 | Verdict |
|---|---|---|---|
| No element passes through the core | 1–12 free particles | 0–5; **0** at the default cage | **Fails**, except at ADR-0029's default configuration |
| Volume within 5% of rest under load | 23–34% below | 20–26% below | **Fails**, and is a property of a displacement-controlled press rather than of the cage |
| Volume within 2% after release | 1.000 in 12 of 18 | **1.000 in 18 of 20** | **Passes** |
| Shape within 1 mm RMS 1 s after release | 0.00–0.05 mm in 12 of 18, 51–55 mm in the rest | **0.00–0.30 mm in 18 of 20** | **Passes** |
| ≤ 120 µs a tick on one core at 8 it / 2 ss | 1,801 µs at 512 elements | **803 µs** at the 216-element default | **Fails**, by 6.7× |

What remains, in the order it is worth doing: a **force-controlled press** (the volume criterion
is unanswerable against an infinitely heavy plate, and the fixture is what has to change, not the
solver); **contact depth rather than centre-inside-capsule** for the interpenetration count, and
a particle radius or a contact surface that matches the cage's cell; and a **budget criterion
written against the wall-clock budget ADR-0029 defines** rather than against a per-core figure
that predates the measurement of how cages share a pool.

The two paragraphs below are the 2026-09-17 analysis and are what ADR-0029 acted on.

Two of E19's four pass criteria fail, and both point at something missing from the module rather
than at the lattice.

- **"Volume within 5% of rest under load."** Measured: 23–34% below rest while the plate is on
  it, at every element, iteration, and sub-step count, and unchanged by dropping the contact
  friction from 0.6 to 0.1 so that the cage is free to barrel outwards. The volume constraint is
  a soft constraint being asked to resist a kinematic body driven through a third of the cage's
  depth, and it loses. Volume comes back exactly on release, so this is compliance and not
  damage. If 5% under load is the requirement, it needs either a genuinely hard volume
  constraint or a press that is specified as a force rather than as a displacement.
- **"No element passes through the core."** Measured: 1 to 12 free particles inside a core
  capsule, in every configuration. Particles are spheres of a third of a cell to the solver and
  the core is a capsule; a particle driven into the gap between the plate and the bone has
  nowhere to go, and the contact resolution puts it on the wrong side. Excluding the elements
  that are *inside the bone by construction* is what makes this a real count, and it is what the
  second attachment kind bought.

Both failures share a cause the module does not have a mechanism for:
[07 §7.10](../plan/07-content-pipeline.md#710-deformable-volume-assets)'s `limits.max_strain`
(default 0.5, a stretch ratio of 1.5) and `limits.max_velocity` are described there as "stability
constraints the solver clamps to, not suggestions", and **nothing clamps them**. The held edge
stretch is 1.76–2.41 and the transient peak reaches 2.81. A cage whose elements are allowed to
stretch to two and a half times their rest length will put particles places they cannot come back
from, which is also the direct cause of the latch: a tetrahedron that goes inside out has a
volume constraint that pushes it further inside out, and iterating harder pushes harder. That is
why **one configuration at 343 particles diverged outright** and why raising the budget at 729
particles turned recoveries into latches. A per-element strain clamp is the next thing to build,
and it is cheap: it is a clamp in the position pass, not a solver change.

## Caveats

- **The 2026-09-17 numbers carry no record of what else the machine was doing** and are upper
  bounds for that reason; see the re-measurement's first table for what a recorded state looks
  like. The two runs agree to 3% on the cost constant, which is evidence that the first was not
  badly contended, and is not the same thing as a record.
- **The baseline machine has not run this.** [ADR-0029](../adr/0029-deformable-volume-budgets.md)
  decision 5 asks for the Titan X box (i7-980, Westmere, SSE4.2, no AVX —
  [self-hosted runners](../ci/self-hosted-runners.md)) before the thresholds are tuned further,
  and it is down until its power supply is replaced. Every figure on this page is one CPU.
- **The 216-element rows are a 6-a-side lattice**, so "at most 256" is measured at 216 and not at
  256. A regular lattice can be 216 or 343 and nothing between.
- **One machine, one material.** Everything here is an i9-10980XE at one edge compliance, one
  density, and one press depth. The cost model's constant is a property of this CPU
  ([11 §11.8](../plan/11-performance-principles.md)); its *shape* — linear in elements,
  iterations and sub-steps, and the solve-width penalty — is not.
- **The cage is a cube and the core is two capsules.** A generated adaptive lattice has an
  irregular element size and a cage around a real skeleton has more bones with worse angles. The
  element counts are also what a regular lattice can be (343 / 512 / 729) rather than the plan's
  round 300 / 500 / 800.
- **The core is held still.** E19 is about compression around a rigid internal structure; a core
  driven by an animation clip is E20's question and would exercise the `Spring` attachment far
  harder than a stationary anchor does.
- **The press is a displacement, not a force.** Driving a kinematic plate to a fixed depth means
  the cage cannot win the argument, whatever its material. A force-controlled press would turn
  "volume under load" into a material property rather than a fixed-depth outcome, and is
  probably the better fixture for the 5% criterion.
- **The cross-cage scaling comparison is indicative, not exact.** The one-against-eight table
  comes from the module's own soft-cube benchmark, which uses a stiffer cage (1e-6) and 1 kg
  particles rather than E19's material, at one sub-step. The ratio is the point, not the
  absolute figures.
- **`interpenetration` counts particle centres**, not overlap volume, and a particle a
  micrometre inside the capsule counts the same as one halfway through it. A depth would be a
  better metric and is a small addition to the harness.
- **No replay, no sim hash, no golden scene yet.** [09 §9.6](../plan/09-testing-profiling.md#96-deformable-volume-experiments)
  wants each scenario to become a permanent scene in `content/test-scenes/` with its replay and
  budget row. This is a bench fixture; the scene is later work, and the cage state does not enter
  a hash yet because the deformation system that would own it does not exist.
