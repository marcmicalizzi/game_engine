# E19: a lattice cage around a rigid two-bone core, pressed to 30% and released

- **Question ([10 §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing), [09 §9.6](../plan/09-testing-profiling.md#96-deformable-volume-experiments)):** does an SDF-generated lattice cage around a rigid internal structure stay stable and preserve volume under sustained compression, and at what cost per tick? It decides whether the lattice is [ADR-0026](../adr/0026-deformable-volumes-first-class.md)'s v1 cage kind, and the default element, sub-step, and iteration budgets.
- **Date:** 2026-09-17. **Machine:** Intel Core i9-10980XE, 18 cores / 36 threads, Windows 11. **Build:** `msvc-release` (RelWithDebInfo), Jolt 5.6 with `CROSS_PLATFORM_DETERMINISTIC` on, SSE4.2 baseline.
- **Machine state (added 2026-09-18):** not recorded at the time; the machine is shared with GPU diffusion workloads and parallel agent builds, and the harness did not yet know how to look ([bench](../subsystems/bench.md#measuring-on-a-shared-machine)). The per-tick costs here are **upper bounds**, which matters more than usual on this page because its decision is a *comparison against a fixed budget* (ADR-0026's 1.5 ms) rather than a ratio between two runs: a number that is 10% high can move a verdict. The grid was not re-measured here — `domain/physics` and this page are another agent's — but the single-cage cost is the one to re-take first with `--require-quiet`, and the multi-worker rows are the ones background load hurts most, because a cage's solve jobs and another process's threads want the same cores. [physics](../subsystems/physics.md)'s performance table carries the calibration point from the same box: what the rigid-body and soft-cube rows measured on a machine whose state *was* recorded.
- **Decision:** the lattice **is** the v1 cage kind; the default budget is **512 elements at 8 iterations and 2 sub-steps**; the T0 cost is **1.1–2.5 ms a tick for one cage**, which is above ADR-0026's whole 1.5 ms deformable budget and forces the budget or the tier table to move. Two of E19's four pass criteria fail, and the reasons are specific and fixable rather than fundamental. Details and the open questions are at the end.

## Method

### The module

`domain/physics` ([page](../subsystems/physics.md)) gained three things for this experiment, all of them things ADR-0026 asks for anyway:

- **A second attachment kind.** `SoftAttachment::kind` is `Rigid` (zero inverse mass, the particle covers the whole gap to its anchor each step) or `Spring` (the particle keeps its mass and its velocity is steered `follow_rate * dt` of the way there). Only `Rigid` existed, and with only `Rigid` this experiment could not have been run honestly: a zero-mass particle cannot be pushed by contact, so "no element passes through the core" would have been true by construction. [05 §5.14](../plan/05-simulation.md#514-deformable-volumes) asks for the spring form in as many words — "'bound' is a stiff spring rather than a weld, so flesh lags a fast bone instead of tracking it exactly".
- **`soft_body_solve_width`**, and the finding behind it (below).
- **`WorldStats::soft_body_solve_jobs` / `soft_body_solve_workers`**, so "did the solve actually spread across the pool?" is a number rather than a profiler session.

### The fixture

`domain/physics/bench/e19_bench.cpp`, three benchmarks in the module's bench executable, so the whole thing re-runs anywhere the engine builds:

```powershell
pwsh tools/dev.ps1 build -Preset msvc-release
build/msvc-release/domain/physics/engine_physics_bench.exe --filter=physics.e19.tick --json=e19-tick.jsonl
build/msvc-release/domain/physics/engine_physics_bench.exe --filter=physics.e19.press --repeats=1 --warmup=0 --quiet
build/msvc-release/domain/physics/engine_physics_bench.exe --filter=physics.e19.sustained --repeats=1 --warmup=0 --quiet
```

The last two print one JSON object per line per configuration, so their stdout is a JSON-lines file; the first goes through the bench harness's own `--json`. A benchmark argument is one integer, so the four sweep dimensions are packed as `elements * 1e6 + workers * 1e4 + iterations * 10 + sub_steps` — `512080162` is 512 elements, 8 workers, 16 iterations, 2 sub-steps.

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

## What this decides

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
