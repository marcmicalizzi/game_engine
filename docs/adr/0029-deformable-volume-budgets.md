# ADR-0029: Deformable volume budgets after E19: a hero allowance, a cage that is one solve group, an authored volume preservation, and an enforced strain limit

- **Status:** Accepted
- **Date:** 2026-09-18
- **Plan references:** docs/plan/05-simulation.md §5.14, docs/plan/07-content-pipeline.md §7.10, docs/plan/09-testing-profiling.md §9.6
- **Docs touched:** `docs/subsystems/physics.md`, `docs/experiments/e19-lattice-cage.md`, `docs/plan/05-simulation.md` §5.14, `docs/plan/07-content-pipeline.md` §7.10, `docs/plan/09-testing-profiling.md` §9.6, `docs/plan/10-roadmap-risks.md` §10.5

Supersedes nothing. It refines the budget sentence of [ADR-0026](0026-deformable-volumes-first-class.md), which said "1.5 ms per 60 Hz tick for all deformable volumes" and marked it provisional until E19–E25 measured it. E19 has.

## Context

[E19](../experiments/e19-lattice-cage.md) pressed a lattice cage around a rigid two-bone core to 30% of its depth, held it, and released it, across three element counts, three worker counts, three iteration counts and two sub-step counts. Five things it found force this decision.

1. **The cost is 0.225 µs per element per iteration per sub-step**, within ±5% across the whole grid. A 512-element cage at the eight iterations and two sub-steps the plan assumes is therefore **1.8 ms a tick** — 1.2× ADR-0026's entire 1.5 ms budget, for one volume, against the 40–120 µs [plan 05 §5.14](../plan/05-simulation.md#514-deformable-volumes) estimated.
2. **A cage's solve is split by vertex batch, not by worker.** The backend partitions a cage's vertices into groups of at most 256 and gives each group's constraints their own update group, so a 512-element cage is a two-wide solve on a 36-thread machine and the solve jobs that find no group spin rather than idle. One cage is **1.2× to 1.7× slower on eight workers than on one**. Eight cages together are **6.5× faster** on eight workers than on one and cost less in total than a single cage does on the same pool.
3. **Sub-steps are the stability knob and iterations are not.** At 512 elements one sub-step took a permanent set at eight and at sixteen iterations while two sub-steps recovered at every iteration count; at 729, *raising* iterations turned recoveries into failures. The mechanism is cell inversion, not under-convergence.
4. **Volume compliance is not edge compliance, and it is not scale-free.** The constraint gradient of an edge is a unit vector and that of a tetrahedron's volume is an area; giving both the same number leaves volume preservation switched off and the cage takes a permanent set at two thirds of its volume.
5. **`limits.max_strain` is enforced by nothing.** [Plan 07 §7.10](../plan/07-content-pipeline.md#710-deformable-volume-assets) calls it "a per-element clamp the solver never exceeds"; measured held stretch was 1.76 to 2.41 against an authored 1.5, with a transient peak of 2.81 and one configuration diverging outright.

Two of E19's four pass criteria failed, both traceable to 5.

## Decision

1. **The budget is wall clock on the performance pool per tick, measured by the world and reported in its stats — not a CPU-time sum.** A CPU-time sum would price eight cages stepped together, which is the arrangement the engine wants, as if it were the worst one. **Ambient** deformables — every non-hero T0 volume plus all of T1 — keep ADR-0026's **1.5 ms**. A **hero allowance of 2.5 ms** exists on top, for at most one volume flagged `hero` at a time; the cavity interaction of [plan 13 §13.7](../plan/13-reference-consumer-games.md#137-reference-interactions) is the case it exists for. Worst case is 4 ms of a 16.7 ms tick. Over budget, the simulation LOD tiers demote **farthest-first** among ambient volumes; **the hero is never demoted by the budget**, because a hero that degrades under its own cost is the failure the allowance exists to prevent.
2. **The default T0 cage is at most 256 particles, at 8 iterations and 2 sub-steps.** 256 is one backend solve group: wider does not scale and costs *more* under parallelism. The content validator **warns above 256 for a non-hero volume** and **refuses above 800** for any volume.
3. **Volume preservation is authored dimensionless (0..1) and converted with the cell size.** The conversion is a material property, not a tuning constant: the backend's constraint is on six times the tetrahedron's volume, so equating XPBD's stored energy with a bulk modulus K gives `compliance = 36 · V_tet / K`, which for a cubic cell of side h is `6 h³ / K`, and the authored weight p scales the material's own bulk modulus as `K = (E/3)·p/(1−p)`. Compliance therefore goes as the **cube** of the cell size at a fixed material.
4. **`max_strain` is enforced**, by a per-element clamp applied in the position pass after the backend's solve: symmetric in engineering strain, position-only, velocity-consistent at the step's own dt, sweeping until the worst edge is inside the limit to a thousandth of its rest length.
5. **The numbers are re-measured on a quiet machine and on the Titan X baseline machine before the thresholds are tuned further.** The baseline machine is down until its power supply is replaced, so every figure here is one machine.

## Consequences

`domain/physics` gains `SoftBodyDesc::hero` and `max_strain`, a `SoftBodyBudget` in `WorldStats` carrying the ambient and hero wall clock and how far each is over, and `domain/physics/deformable.h` — the policy numbers, `cage_size_verdict()` and `volume_compliance_for()` — in a header the content pipeline can include without taking a dependency on the world. **The validator itself is a later content-pipeline task**; this ADR puts the constants and the verdict function where that task will read them, and nothing in `domain/physics` refuses a cage for being wide.

The measurement, re-run on 2026-09-18 with the machine's state recorded ([E19](../experiments/e19-lattice-cage.md)):

- **The permanent set is gone.** Eighteen of twenty configurations now return to 1.000 of rest volume within 17–67 ms and sit 0.00–0.30 mm from their own settled pose a second later. Before, three latched at 0.72–0.77 of rest volume and one diverged.
- **Held stretch is exactly 1.5000** — the authored limit — in every configuration that survives, against 1.76–2.41 before. Free particles inside the core fall from 1–12 to 0–5, and the default cage has **none**.
- **The clamp costs `edges × sweeps × 4.3 ns` a step**, which is **17 µs for the 256-element default cage** and **326 µs for a 512-element one**: a second reason the default is one solve group.
- **Two configurations regress**: 512 and 729 elements at sixteen iterations *and* two sub-steps now diverge where they used to latch or recover. The control runs attribute that to decision 3, not to decision 4 — the conversion's default preservation is stiffer than the hand-tuned compliance it replaces, and thirty-two XPBD sub-steps a tick is past what that stiffness conditions for. It is outside the default envelope of decision 2 and is the conditioning limit E23 exists to pin.

What is now forbidden: pricing the deformable budget as a sum of per-worker CPU time; a T0 cage above 256 elements that nobody deliberately authored; more than one hero volume in steady state; a generated cage whose volume compliance does not know its cell size; and an authored `limits.max_strain` that nothing reads.

E19 stays **Measured** rather than Done. Two of its four criteria still fail and neither is about the cage: volume under load is 20–32% below rest at every budget (the volume constraint is a soft constraint losing an argument with a kinematic plate driven through a third of the cage's depth), and the cost is 0.80 ms a tick for the default cage against a criterion of 120 µs.

## Revisit when

A hero cage needs more than about 600 elements, or E26 fails inside the hero allowance. The answer then is the engine's own XPBD solver with intra-cage graph colouring — CPU SIMD or GPU compute — behind the same abstraction, which is what removes the 256-vertex batch and with it the reason the default cage is this small. Also when the Titan X baseline machine is back and the thresholds can be set against two machines instead of one.
