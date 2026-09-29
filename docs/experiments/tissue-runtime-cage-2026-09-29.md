# A runtime cage from the certified supine pair

- **Question ([05 §5.16](../plan/05-simulation.md#516-characters-at-run-time), the 2026-09-26
  addendum "the engine first"; [07 §7.10](../plan/07-content-pipeline.md#710-deformable-volume-assets)):**
  what a runtime cage derived from a certified ten-node reference body is — its size against
  [ADR-0029](../adr/0029-deformable-volume-budgets.md)'s budget, its mass ledger, how far it is
  from the reference in each state it carries — and, built as the physics module's soft body and
  settled under the supine load, how far it lies from the certified supine state.
- **Date:** 2026-09-29. **Machine:** Intel Core i9-10980XE (18 cores, 36 threads), 64 GB, Windows 11
  Pro 26200. **Build:** `msvc-debug` for the derivation, `msvc-release` for the settles' times
  (MSVC 14.51.36231). No GPU is involved.
- **Machine state:** the derivation's timed runs at 2% CPU load before and 4% after
  (`Win32_Processor.LoadPercentage`), nothing else running. **The settles' times were taken beside
  other agents' builds** — 15–28% load when each run started, 74–100% an hour earlier, `cl.exe`
  processes the top consumers — so they are upper bounds, not costs. Every number below except
  the times is a function of the input files: the positions are the same bytes on every run and in
  both configurations.
- **Decision:** none; the status notes in [05 §5.16](../plan/05-simulation.md#516-characters-at-run-time)
  and [07 §7.10](../plan/07-content-pipeline.md#710-deformable-volume-assets), and the thresholds of
  the `cage.*` rows ([tissue](../subsystems/tissue.md#how-far-the-cage-is-from-its-reference)).

## Setup

The inputs are the authoring side's native supine fixtures (`astra-supine-native-reference-2026-09-26`,
per case `reference-native.tissue`: B2a-supine `56c034f7…`, B2b-supine `afb39fc7…`), each checked
against its pinned SHA-256 and copied to a scratch directory before it was read; nothing of the packet
is committed and nothing was written into it. Each is a `Reference` region of 1,635 nodes and 846
ten-node cells over 275 corners (242 on the boundary, 33 inside), with four states: the
construction, `historical-prolongated-rest` (the rest), `failed-standing-reference` (the reference
role) and the certified supine endpoint (`B2a-supine` or `B2b-supine`, a response).

```
engine-content tissue cage B2a-supine.tissue B2a.cage.tissue --omit-state failed-standing-reference
engine-content tissue cage B2a-supine.tissue B2a.hero.cage.tissue --omit-state failed-standing-reference --hero
engine-content tissue validate B2a.cage.tissue --source B2a-supine.tissue --no-modes
```

The derivation is `corner-collapse-v1` ([tissue](../subsystems/tissue.md#cage-derivation)): the
corner tetrahedra, then half-edge collapses, interior first. The same measurements were taken on the
synthetic torus slab's ten-node body (`engine-content tissue example`'s `synthetic-quadratic.tissue`,
cells curved by up to 3.6% of an edge) to choose the rows' thresholds from more than one body.

## Results

**The derivation.** Without `--omit-state` both fixtures are refused: at the failed standing
reference the corner tetrahedron of cell 467 is inverted (−2.48 × 10⁻⁸ m³ in B2a, −3.70 × 10⁻⁸ m³ in
B2b), in a cell whose ten-node map the Bernstein bound proves positive only through its curvature.
With it:

| | Default budget | `--hero` |
|---|---|---|
| nodes, cells | 256, 746 | 275, 846 |
| collapses | 19 interior, 0 boundary | none |
| boundary nodes | 242, all kept | 242 |
| least SICN | 0.119319 (study019's cell, untouched) | 0.119319 |
| ADR-0029's verdict | `Ok` | `Ok` as a hero |
| derivation time, `msvc-debug` | 2.7 s | 1.6 s |
| validated alone / with `--source` | 1.4 s / 2.3 s; 23 rows, 0 errors, 0 warnings | the same |

**The mass ledger**, the same for both fixtures and both budgets to the digits shown:

| | kg | ml |
|---|---|---|
| reference, exact ten-node cells | 0.31940195 | 328.60283 |
| reference, linear subdivision | 0.31940194 | 328.60282 |
| cage | 0.31940191 | 328.60279 |
| difference | 4.34 × 10⁻⁸ (1.36 × 10⁻⁷ of the mass) | 4.5 × 10⁻⁵ |
| of which curvature | 1.08 × 10⁻⁸ | |
| of which reference outside the cage | 2.19 × 10⁻⁷ | |
| less the cage outside the reference | 1.86 × 10⁻⁷ | |

**How far the cage is from its reference** (the `cage.*` rows), B2a and B2b alike except where
shown:

| State | Volume, cage against reference | Boundaries' largest distance (chordal) | At the reference's nodes: p50 / p95 / max | Strain-energy share |
|---|---|---|---|---|
| construction | −1.4 × 10⁻⁷ | 5.8 × 10⁻⁵ mm | 0 / 0 / 2 × 10⁻¹³ mm | 1.04 (hero 1.00) |
| rest | −2.5 × 10⁻⁷ | 5.8 × 10⁻⁵ mm | 7.6 × 10⁻⁶ / 0.21 / 0.79 mm (hero: 0.00015 mm max) | — (the rest) |
| supine | −0.689% | 1.784 mm, reference node 999: 1.78 mm outside the cage's chords, 1.11 mm inside elsewhere | 0.128 / 0.730 / 1.806 mm (hero 0.100 / 0.701 / 1.806) | 1.78 (hero 1.97) |
| failed standing reference (not carried; what it would have read) | −1.91% (B2a), −2.13% (B2b) | 3.97 / 4.33 mm, node 784 | 0.195 / 1.30 / 4.01 mm (B2b 4.37) | 1.68 / 1.66 |

At the cage's own nodes every carried state is the reference's to 4.5 × 10⁻¹⁶ m: the carried state is
the reference's field there, evaluated by Newton's method in the ten-node cells and compared with
the node copied across.

**The settle** ([tissue](../subsystems/tissue.md#a-soft-body-from-a-runtime-region)): each cage
built as a soft body from its rest — 48 nodes held where they rest (`fixed-rim`, to the world), 121
sliding without friction on `historical-support`, every node against the frame `thoracic-proxy`,
gravity 9.798 m/s² along +y (the supine load, less the air) — stepped at 60 Hz until the 99th
percentile of the nodes' speeds stays under 0.1 mm/s for half a second, and measured against the
certified supine state carried onto the cage (which lies 2.36 mm from the rest at the median node,
5.02 at the 95th percentile, 5.40 at the largest):

```
engine-content tissue settle B2a.hero.cage.tissue --state B2a-supine --repeat
engine-content tissue settle B2a.cage.tissue --state B2a-supine --repeat
engine-content tissue settle B2a.cage.tissue --state B2a-supine --repeat --sub-steps 4
```

| Cage, settings | Settles | Steps, simulated | Time (`msvc-release`, one thread) | Distance from the certified state: p50 / p95 / max | Largest at | Its settled / certified move from the rest |
|---|---|---|---|---|---|---|
| 275 (hero), 8 iterations × 2 sub-steps | yes | 221, 3.7 s | 0.52 s, 2.4 ms a step | 1.580 / 3.547 / 4.029 mm | node 20, boundary, `surface` | 9.19 / 5.25 mm |
| 256, 8 × 2 | **no** | 3,000, 50 s | 6.5 s, 2.2 ms a step | 1.270 / 2.847 / 3.354 mm | node 19, boundary, `surface` | 8.54 / 5.27 mm |
| 256, 8 × 4 | yes | 747, 12.5 s | 3.2 s, 4.3 ms a step | 1.130 / 2.319 / 2.673 mm | node 20, boundary, `surface` | 7.81 / 5.25 mm |

The whole settled body moves further from the rest than the certified state does: median 3.56 mm
(275), 3.15 mm (256) and 2.66 mm (256, four sub-steps) against 2.36 mm; 95th percentile 7.9, 7.3
and 6.6 against 5.0 mm. No cell inverted; the strain clamp never needed a second sweep. **Two runs
of each gave the same bytes**, B2a and B2b gave the same numbers (their supine states are one), and
the 256-node cage 300 steps in reads 1.270 / 2.847 / 3.353 mm in `msvc-debug` and `msvc-release`
alike, to every digit printed.

**The 256-node cage at ADR-0029's two sub-steps does not settle.** Its 99th-percentile speed is
17.6 mm/s at 1 s, 7.4 mm/s at 2 s and 7.3 mm/s at 50 s (205 of its 256 nodes above 0.1 mm/s; the
fastest, a posterior node on the support, at 7.85 mm/s from 2 s through 200 s), while its shape
does not move: the distances at 5 s, 50 s and 200 s agree to 10⁻³ mm. It is a vibration about a fixed shape, and it
is the contact's: at 1,200 steps, damping of 20 per second leaves it at 4.4 mm/s, friction 0.2 at
1.9 mm/s, 16 iterations at 6.1 mm/s, 4 iterations at 7.7 mm/s, and switching off the strain clamp
changes nothing — while 4 collision sub-steps, which choose each node's contact plane twice as
often, settle it (and 16 iterations at 2 sub-steps, the same 32 position solves a step, do not).

The synthetic torus slab, for the thresholds: 147 corners and no collapse at the default budget
(100 nodes by 25 interior and 22 boundary collapses, the boundary moved by 2 × 10⁻⁹ m); mass and
volume −0.75% at every state (the curvature); the boundaries 0.66 mm apart at most; the field at its
nodes within 0.39 mm (0.41 mm at 100 nodes); energy share 0.93–0.95.

## What surprised me

- **The cage is the reference's own linear mesh, and the budget cost only the interior.** The
  supine body's 275 corners are 19 over the ambient budget, and its 33 interior nodes absorbed all
  19: no boundary node moved, so the frame's clearances, the containment and the construction's
  volume are the reference's to the rounding.
- **The failed standing reference is a ten-node state and not a four-node one.** Its cells are valid
  only because they curve, and the one cell whose corner tetrahedron turns over is next to cell 496,
  where the least Jacobian ratio (0.046) sits. A linear cage cannot carry it at any budget; the
  derivation says so instead of writing a cage that fails.
- **The certified supine state is 1.8 mm of curvature away from any four-node cage over these
  nodes.** The largest distance, at reference node 999, is an edge node 1.78 mm outside the chord
  between its corners — the same with the hero cage, because it is not a question of how many
  corners the cage keeps but of what a straight edge can follow.
- **A four-node cage of this material stores 1.8–2.0 times the reference's strain energy for the
  same displacement** — K = 100 kPa against μ of 350–1,000 Pa is nearly incompressible, and linear
  tetrahedra lock; the P2 cells the reference was certified on do not — **and yet settled, it sags
  further than the certified state.** Stiffer by the energy, softer in the settle: an XPBD
  relaxation at eight iterations is not the energy's minimum, and the posterior slides on a
  frictionless support that the certified solve held by a nodal contact. Which of the two moves the
  answer more is not something this measurement separates.
- **The answer depends on the solver's settings by a quarter.** The same 256-node cage reads 1.27
  mm at the median at two collision sub-steps and 1.13 mm at four; the hero reads 1.58. The fixed
  point of an XPBD relaxation is a function of its sub-steps and iterations, which is exactly why
  the definition makes a rest name its solver.
- **The backend's contact cost most of the afternoon.** A support surface oriented by its region's
  centroid put 135 of the 256 nodes up to 38 mm behind it, and Jolt, which treats a vertex up to
  10 cm behind a triangle as penetrating, inverted 370 cells in the first step; a node lying exactly
  on a support built from its own nodes gets no contact at all; a spring attachment sags 1.36 mm a
  step under gravity. [physics](../subsystems/physics.md#a-soft-body-from-a-tissue-region) has the
  four findings and what the soft body does about each.

## What it decides

The `cage.*` rows' thresholds: mass 1%, volume 2%, boundaries 2 mm, field 2 mm, 1 µm at the cage's
nodes — each passed by every carried state of both bodies, the supine state's boundary and field by
12% and 10%, and each failed by what the failed standing reference would have been. And the first
settle's numbers, **pinned as a regression, not passed** (`apps/engine_content/tests/tissue_tests.cpp`):
the contact is the physics module's own, not the face-based model the authoring side is still
declaring, and the 1.1–1.6 mm median and 2.7–4.0 mm largest distance from the certified supine
state is the gap later work closes. It does not decide ADR-0029's sub-step default: one cage that
vibrates at two sub-steps is a reason to measure more cages, not to double every cage's cost.

## Caveats

Two fixtures that share their construction, rest and supine state to the digits shown; one synthetic
body; one method. A reference whose corner boundary is larger than the budget would need boundary
collapses, which these fixtures did not exercise (the synthetic slab and the tests' block did, on
flat faces). One settle configuration family, on one machine that was not quiet; the times are
upper bounds, the positions are not affected.
