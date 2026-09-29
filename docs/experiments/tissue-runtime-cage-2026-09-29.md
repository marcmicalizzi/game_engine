# A runtime cage from the certified supine pair

- **Question ([05 §5.16](../plan/05-simulation.md#516-characters-at-run-time), the 2026-09-26
  addendum "the engine first"; [07 §7.10](../plan/07-content-pipeline.md#710-deformable-volume-assets)):**
  what a runtime cage derived from a certified ten-node reference body is — its size against
  [ADR-0029](../adr/0029-deformable-volume-budgets.md)'s budget, its mass ledger, how far it is
  from the reference in each state it carries.
- **Date:** 2026-09-29. **Machine:** Intel Core i9-10980XE (18 cores, 36 threads), 64 GB, Windows 11
  Pro 26200. **Build:** `msvc-debug` (MSVC 14.51.36231). No GPU is involved.
- **Machine state:** CPU load 2% before and 4% after the timed runs (`Win32_Processor.LoadPercentage`);
  no GPU job and no other build running during them. Every number below except the timings is a
  function of the input files and does not depend on the machine.
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
  same displacement.** K = 100 kPa against μ of 350–1,000 Pa is nearly incompressible, and linear
  tetrahedra lock; the P2 cells the reference was certified on do not.

## What it decides

The `cage.*` rows' thresholds: mass 1%, volume 2%, boundaries 2 mm, field 2 mm, 1 µm at the cage's
nodes — each passed by every carried state of both bodies, the supine state's boundary and field by
12% and 10%, and each failed by what the failed standing reference would have been. It does not
decide whether the cage is good enough to simulate on: that is a runtime question, and a separate
measurement.

## Caveats

Two fixtures that share their construction, rest and supine state to the digits shown; one synthetic
body; one method. A reference whose corner boundary is larger than the budget would need boundary
collapses, which these fixtures did not exercise (the synthetic slab and the tests' block did, on
flat faces).
