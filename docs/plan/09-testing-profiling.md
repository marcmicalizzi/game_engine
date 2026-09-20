# 09 — Testing and Profiling Architecture

## 9.1 Test taxonomy

| Kind | Location | Gate |
|---|---|---|
| Unit and property tests (module invariants, schema round trips, migrations) | per-module `tests/` | every change |
| Determinism and replay (sim hash after N ticks from seed + input log) | `systems/simulation/tests` | every change |
| Deformable-volume golden replays (cage state hash after N ticks, one replay per scenario in [§9.6](#96-deformable-volume-experiments)) | `systems/deformation/tests` | every change |
| Renderer golden images (FLIP against goldens per scene × resolution × GPU class) | `content/golden` | subset per change; full nightly including 11520×2160, 7680×4320, 1080×3840, 5120×1440 |
| Reference-vs-real-time error (FLIP against converged path trace) | nightly | per-scene threshold |
| Performance regression (frame-time medians over K runs, per pass, per-machine baseline) | nightly on GPU runners | fail on a significant regression above 3% |
| Memory budgets (per-tag high-water marks) | nightly | budget file per target config |
| Hot-type size table (`static_assert`) and per-frame allocation count | build; every headless run | every change |
| Banned-container and hot-path lint (clang-tidy project checks) | build | every change |
| World-state invariants (conservation, no orphan references, required regions reachable) | after every headless run | every change |
| Content validation (all rules in [07 §7.4](07-content-pipeline.md#74-validation-rules-automatic)) | content build | blocks the build |
| Character parameter-space sweep (the validators of [§9.7](#97-character-validators) over corners, samples, modules and poses) | content build for a base or module; full sweep nightly | blocks the build for the base or module that fails |
| Fuzzing (destruction sequences, input, save/load points, protocol messages) | nightly | crash or invariant failure |
| Playtest bots (utility bots × profiles × seeds) | nightly | completion rate, stuck rate, budgets |
| Agent-content validation (canon, style, boundary contracts) | on proposal | blocks promotion |
| Protocol and MCP contract tests (schema conformance, error messages) | every change | |
| Migration corpus | every change | |

## 9.2 Determinism infrastructure

- A sim hash per tick covering persistent state and RNG streams.
- A divergence bisector: find the first tick where two runs differ and dump both states.
- A "record everything" mode for bug reports: input, events, and LLM outputs.
- A replay viewer in the editor with scrubbing and state inspection.

## 9.3 Profiling infrastructure (before optimizing anything)

- Tracy instrumentation in every system; GPU zones via timestamp queries; capture export as part of the `profile` tool result.
- Per-pass GPU timings and counters (occupancy, bandwidth via vendor APIs where available) in a structured frame report.
- Memory by tag; allocation counts per frame (should approach zero in steady state); page-residency statistics.
- Streaming statistics: requests, hits, latency, evictions.
- Simulation statistics: per-system time, event rates, LOD tier populations, scheduler queue depth.
- A frame-budget file per target configuration; violations are telemetry events agents can query.

## 9.4 Benchmark scene corpus

Representative: interior, urban street, forest vista, destruction sequence. Pathological: 10^6 emitters, a mirror room, a foliage wall, 10^5 instances, ultra-thin geometry, a 48:9 wide-FOV view, 10^5 NPCs at LOD2, 10^4 debris bodies. All versioned in `content/test-scenes/`, each with a converged reference image set.

## 9.5 Agent-facing test surface

`run_tests(filter)`, `benchmark(scene_set, resolution_set)`, `validate(scope)`, `replay(run_id, until)`, `bisect_divergence(run_a, run_b)`. All return structured results with artifact URIs, and every failure includes the minimal reproduction command.

## 9.6 Deformable volume experiments

The eight scenarios that must run before solver details are argued about ([05 §5.14](05-simulation.md#514-deformable-volumes), E19–E26 in [10 §10.5](10-roadmap-risks.md#105-experiments-to-run-before-committing)). Each becomes a permanent scene in `content/test-scenes/` with its replay and its budget row, so the first measurement is also the regression test. Pass criteria are proposals until the first numbers exist; a failing criterion is a decision to make, not a bar to move quietly.

| ID | Scenario | Setup | Metrics | Pass criteria |
|---|---|---|---|---|
| E19 | Soft volume around a rigid internal structure, compressed | 300–800-element lattice cage around a two-bone rigid core; a rigid press drives 30% of the volume's depth over 0.5 s, holds 2 s, releases | Element strain, volume drift, recovery error, µs per tick, elements per ms | No element passes through the core; volume within 5% of rest under load and 2% after release; shape recovers to within 1 mm RMS of rest 1 s after release; ≤ 120 µs per tick on one performance core at two sub-steps of eight iterations |
| E20 | Animated character with secondary soft-tissue motion | Standard-skeleton locomotion set (walk, run, hard stop, jump landing) with a bound soft-tissue volume; run at 60 Hz and at 30 Hz | Lag and overshoot against the bone, self-intersection with the skeleton's bodies, residual jitter in a held pose, cost per tick | Zero interpenetration of the skeleton's collision bodies across the clip set; a held pose settles below 0.1 mm per tick within 0.5 s; the 60 Hz and 30 Hz results agree within 2 mm RMS with the same authored parameters |
| E21 | Hand and object contact: localized deformation and recovery | Rigid hand collider grips, holds, and releases a soft prop 20 times over 10 s; a single-finger press as the localized case | Penetration depth, contact area against the analytic value, recovery time, kinetic energy after release, locality of the displacement | Max penetration ≤ 10% of the mean cage cell size; no energy gain across the 20 grips (post-release kinetic energy decays monotonically); elements more than two cells from the contact move < 5% of the contact displacement |
| E22 | Deformable object pressed against a rigid surface | A cushion dropped on a plane, then loaded with a rigid body of 1× and of 5× its mass, held 60 s | Plane penetration, creep under constant load, contact jitter, volume preservation | No plane penetration at any tick; drift < 1 mm over 60 s once settled; surface velocity below 1 mm/s one second after settling (no limit cycle); volume within 5% of rest |
| E23 | Multiple material layers with different stiffness | Three bonded layers (stiff, muscle, fat) over one cage at 1:10:100 and again at 1:100:10,000 compliance ratios, under poke, press, and shear | Convergence at a fixed iteration count, strain ratio between layers, boundary separation, µs per tick | The stiff layer's strain under the same load is ≤ 1/10 of the soft layer's (the ratio is visible, not washed out by the solver); no separation at a `bonded` boundary; the ratio at which convergence fails at eight iterations is measured and becomes the validator limit in [07 §7.10](07-content-pipeline.md#710-deformable-volume-assets) |
| E24 | Many deformable entities with simulation LOD transitions | 64 deformable characters on a scripted camera path that crosses every tier boundary in both directions, 60 s | CPU ms per tick and GPU ms per frame by tier population, pop magnitude at transitions, tier changes per entity per minute, FLIP against an all-T0 run | Total CPU within the 1.5 ms tick budget of [05 §5.14](05-simulation.md#514-deformable-volumes); no single-tick surface discontinuity above 2 mm during a blend; fewer than 2 tier changes per entity per minute on the path; FLIP under the scene's nightly threshold outside the interaction radius |
| E25 ([results](../experiments/e25-deformed-clusters.md); the renderer half is done, the solver half is not) | Coarse cage to a very high-detail rendered mesh | An 8M-triangle asset with a cluster DAG and a 512-element cage; camera sweeps 0.5 m to 50 m; 16 deformed instances | Transfer pass ms against cut vertex count, template instantiation ms and AS memory, cracks at cluster and LOD boundaries, FLIP against deforming the full mesh directly | Transfer time is linear in cut vertex count and independent of source triangle count within 5%; no crack at any cluster or LOD boundary, deformed or not; total GPU within the 0.5 ms budget at 3840×2160; FLIP against direct deformation under the scene's nightly threshold |
| E26 | Cavity interaction: reaching into soft tissue, clinging, and recoil ([13 §13.7](13-reference-consumer-games.md#137-reference-interactions)) | A 600-element closed cage with a pressure constraint set into a rigid wall: a stiff outer layer attached to the wall, two softer inner layers, an `adhesive` inner boundary, two authored folds. A scripted arm — a kinematic capsule chain driven by an animation clip — inserts 0.3 m over 1 s, holds 1 s, and withdraws at 0.5 m/s; ten insert-and-withdraw cycles for stability | Resistance force against insertion depth; cling duration and the force at release; recoil settle time; volume error at full insertion and at rest; µs per tick at T0 and GPU ms per frame on the RTX 5090 and on the Titan X baseline target; fold visibility at 0.4 m and at 3 m against an all-cage-resolution reference and against the T2 authored-animation tier | Resistance rises monotonically with depth and is repeatable within 5% across the ten cycles; adhesion holds until the break stress is exceeded and releases over the authored release time ±1 tick, with no energy gain across the ten cycles; recoil settles below 0.5 mm/s within 0.75 s with no limit cycle; volume within 5% of rest at full insertion and 2% after recoil; the stiff outer layer limits the cavity mouth's stretch to the authored maximum with no element passing the wall; ≤ 150 µs per tick at T0 on one performance core, inside the GPU budget of [05 §5.14](05-simulation.md#514-deformable-volumes), and the baseline machine runs the same scene inside its own tick budget or the criterion becomes the tier it has to drop to; folds read at 0.4 m and fade without popping by 3 m |

**E25's renderer half, out of order (2026-09-17).** E25 runs as a Phase 1 spike ([10 §10.5](10-roadmap-risks.md#105-experiments-to-run-before-committing)) because the deformed-vertex pool's layout and the cluster pages' per-vertex binding slot cannot be retrofitted once assets are built, and the spike could not wait for a solver. What has been measured is therefore the *transfer and rendering* half against procedural deformers — pass cost against cut size on two meshes three orders of magnitude apart in triangle count, cluster-template instantiation against per-frame rebuilds, and raster-against-ray agreement under deformation — with the results and their caveats in [docs/experiments/e25-deformed-clusters.md](../experiments/e25-deformed-clusters.md). The row's own criteria (cracks at cluster and LOD boundaries under a *cage*, FLIP against deforming the full mesh, the 0.5 ms budget at 3840×2160 for the whole scenario) stay open until the cage and the solver exist; the scene, its replay, and its budget row are part of that later work.

**E19 re-measured, 2026-09-18 ([ADR-0029](../adr/0029-deformable-volume-budgets.md), [results](../experiments/e19-lattice-cage.md)).** The row stays **Measured** and not Done. Two of its four criteria now pass everywhere — volume within 2% after release and shape within 1 mm RMS a second later, in eighteen of twenty configurations against twelve of eighteen — and two still fail: 0 to 5 free elements reach the rigid core (0 at the default cage, against 1 to 12 before), and volume under load is 20–26% below rest at every budget. Neither failure is about the cage. **The pass criteria themselves are what have to move**, and this is the place to say so rather than to move them quietly:

- *"Volume within 5% of rest under load"* is unanswerable as written. The press is a **displacement**, driven by a kinematic plate that is infinitely heavy, so the cage cannot win the argument whatever its material and the number measures the fixture rather than the solver. A force-controlled press turns it into a material property. Until the fixture changes, this criterion is a statement about plates.
- *"≤ 120 µs per tick on one performance core"* predates knowing how cages share a pool. The measured cost of the default T0 cage is 0.80 ms on one worker, and the budget that matters is the wall-clock one ADR-0029 defines for the whole deformable set — which the world now measures and reports. The per-core figure should become a budget-row check against `physics::WorldStats::soft_body_budget`.
- *"No element passes through the core"* counts particle **centres** inside a capsule, so a particle a micrometre in counts the same as one halfway through. A penetration depth, and a contact representation that matches the cage's cell size, would make it a measurement of contact rather than of sampling.

Two lessons from the re-run generalize past E19 and are worth carrying into every row here. **A re-measurement needs its control in the same session**: the fixture grew a mode that runs the failing configurations exactly as they first ran, beside the fixed ones, because otherwise "the fix did this" cannot be told from "the machine was quieter today" — and on a shared desktop that alternative is real. **And a run needs the machine's state recorded next to its numbers**; E19's first pass did not have it, so every figure from it is an upper bound, and saying so costs one sentence where rediscovering it costs a day.

**Golden replays for cage state.** Every volume with `gameplay_relevant` set contributes its cage positions and velocities to the per-tick sim hash ([§9.2](#92-determinism-infrastructure)), and each scenario above ships a recorded replay (seed, input log, schema version) in the golden corpus. CI replays them on every change and requires a bit-identical final cage state, the same bar the sim replay already meets. A replay that diverges is bisected by tick with both cage states dumped. GPU-side deformation is excluded by construction: it is derived from the cage, never read back, and never enters a hash.

## 9.7 Character validators

A character system is checked by **sweeping its parameter space and computing numbers**, not by looking at a character ([ADR-0032](../adr/0032-characters-are-parameter-vectors.md) decision 12, [07 §7.11](07-content-pipeline.md#711-characters)). The sweep is the corners of the macro space plus a low-discrepancy sample of its interior, times the module set, times a pose set drawn from the standard locomotion and extreme-pose clips. Every validator below produces a number and a threshold, which is what makes the system developable by an agent that does not look at anything and reviewable by a human who reads a report rather than a picture ([12 §12.6](12-ai-usage-policy.md#126-how-this-project-develops-a-character-system)).

| Validator | What it reports | Why it is here |
|---|---|---|
| Inverted and degenerate triangles | Counts and the worst offenders, per sample | The first thing an extreme parameter combination breaks |
| Self-intersection and interpenetration depth | Maximum depth and where, per sample and pose | A shape that reads fine in the neutral pose can pass through itself at a corner of the macro space |
| Module boundary-loop continuity | Position gap, normal angle, and skin-weight disagreement along the loop, at every DAG level | A graft's seam is the one place a module can crack; **E29** decides whether it survives simplification at all |
| Volume and proportion bounds | Per-region volume and limb proportion against the base's declared ranges | Catches a rig that drove a channel past what it was sculpted for |
| Joint-regressor sanity | Joint inside the surface, chain lengths positive, no crossed or inverted chains | A regressed joint outside the body is an animation defect that looks like a clip defect |
| Skin-weight normalization and influence count | Sum, maximum influences, unnormalized vertices | Same rule the rig validators of [07 §7.4](07-content-pipeline.md#74-validation-rules-automatic) already apply, evaluated on the baked shape |
| Texel-density ratio | Across the character and against each garment | A garment that is half the body's density shows as a resolution seam, and it is invisible in a wireframe |
| LOD attribute error | `geometry::measure_lod_attribute_error` per cut, in texels and degrees ([geometry](../subsystems/geometry.md#what-the-simplifier-is-given-and-why)) | The number **E27** is judged on: whether one DAG holds its bounds across the base's parameter space |
| Garment penetration | Body vertices outside the garment that covers them, by depth | The failure the section-visibility mask exists to prevent, measured rather than assumed |
| Cage fit | Every render vertex inside a cell, at the sampled extremes ([07 §7.10](07-content-pipeline.md#710-deformable-volume-assets)) | One cage per base archetype only works if it still encloses the body at the corners of the space |
| Parameter-range conformance | Any axis outside its declared range, named | Where the adult age range of [ADR-0032](../adr/0032-characters-are-parameter-vectors.md) decision 13 is enforced for a vector that reached the build |

Three properties make the set worth building before the content it checks. It is **deterministic**: a fixed sample set and fixed poses, so two runs agree and a regression is a diff. It is **attributable**: a failure names the parameter vector, the module and the pose, so the repro is a line rather than a hunt. And it **replaces pictures** for exactly the content agents may not look at, which is why the review item of [06 §6.10](06-agent-tooling.md#610-multi-agent-roles-review-and-the-human-director) carries a validator report and no capture.
