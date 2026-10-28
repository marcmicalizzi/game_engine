# Exposed structural tree roots

G92, 2026-10-27. Research read before implementation.

## Grounding

- [UC IPM root-system overview](https://ipm.ucanr.edu/PMG/V/S-WO-VEGG-RS.004.html):
  about 70% of roots can occupy the upper foot of soil; actual depth depends on
  species, drainage and compaction. Roots commonly extend beyond the crown.
- [UF/IFAS root problems](https://gardeningsolutions.ifas.ufl.edu/plants/trees-and-shrubs/trees/tree-root-problems/):
  the root flare is visible at the soil surface; the root zone is wide and shallow.
- [USFS white oak](https://research.fs.usda.gov/silvics/white-oak):
  adult tapered laterals replace the juvenile taproot; most main branches in the
  cited study were within 53.3 cm of the surface. Root/crown area ratios of 3.4–5.8
  concern the complete system, not the exposed wood we render.
- [USFS American beech](https://research.fs.usda.gov/silvics/american-beech):
  mature heart roots are generally shallow, more so than sugar maple/yellow birch.
- [USFS western redcedar](https://research.fs.usda.gov/feis/species-reviews/thupli):
  swollen/buttressed base and a shallow root system.

These sources justify shared tapered laterals, branching, basal flare and
site-dependent exposure. They do not supply universal root counts, exposed lengths
or age thresholds. Our numeric shape controls are explicitly authored approximations.
No buried absorbing-root network, root mechanics, soil displacement or growth simulation
is claimed. Uprooting/root plates are a later extension.

## Implementation contract

A separate roots asset retains shared trunk/crown meshes. Its recipe records local
terrain heights and constraints, so its content key includes the ground it was fit
to. The C++ terrain sampling helper accepts scene_gen::Ground plus a world position,
yaw and uniform scale, preserving f64 world coordinates. Terrain is sampled at cook
time, never per frame; changed terrain or a moved tree requires a new root cook.

The species supplies an exposure/flare tendency; trunk radius supplies a smooth
small-tree suppression. Seeded primary roots taper and meander, with subordinate
laterals. Root count and exposed reach are authored visual parameters, not botanical
measurements. Crown radius is the default visible reach, not the extent of the
biological root system.

Controls include enabled, maximum radius, an optional rectangular planting footprint
and circular exclusions. Constraints include root width, not just centre lines.
No space means no root geometry. Disabled roots do no terrain sampling. Root ends
sink below the lowest sampled ground across their width. Sharp terrain changes
terminate and bury a root before the break rather than bridging it into empty air.
Finite sampled heightfields are the supported ground representation; overhangs and
caves need a different surface query.

The source budget reduces cross-section sides before refusing insufficient detail;
it never lengthens or enlarges roots to fill a budget. Rigid roots use mesh LOD,
independently of the canopy's cards. Material roles use bark with metre UVs. The
terrain fit must remain intact at reduced detail; validation covers that boundary.

## Authored parameters and budget

These are visual controls, not botanical measurements: root expression fades from
zero at 4 cm stem radius to full at 25 cm. A mature tree has 5–9 main roots by
default (override: 3–16), each initially 33–45% of stem radius and reaching 50–95%
of the allowed crown radius. A secondary lateral branches at 30–45% of its parent
path with 55% of its radius and 45% of its length. Beech, cedar and willow use an
exposure tendency of 1.2; oak, pine and walnut 0.8; other supported trees 1.0.
Site exposure, rather than species alone, remains the primary control.

Paths sample at most 22 cm apart, or half a terrain-grid cell. A sampled gradient
over 1.25 stops growth before the break; the last 28% of the accepted path sinks
smoothly to an endpoint 2.5 cm below the lowest of five transverse ground samples.
This guarantees the sampled endpoint, not arbitrary terrain finer than the grid.
The 12,000-triangle default chooses eight or four cross-section sides; both LODs
retain every longitudinal station. Roots are a separate standard-detail placement,
so applications can omit distant roots without changing the tree's canopy LOD.

## Validation

Seven focused root cases cover determinism, finite unit normals, the budget,
disabled/sapling/no-room cases, complete-width planting bounds, obstacle clearance,
slope/cliff endpoint burial, world-coordinate sampling and yaw/scale, cache keys,
maturity dimension resolution and the rigid two-rung container.

Engine renders inspected on the RTX 5090: flat ground, a 35% slope, a 3 m drop,
a 3 x 2 m bed with an exclusion, roots off and the coarse rung. The 15 m oak has a
45 cm stem radius and the same bark material on both assets. Source counts:
flat/slope 7,232 triangles, drop 6,544, confined bed 1,808; each has two authored
rungs. The bare ground deliberately exposes joins and burial. It does not assess
blending into Forest's grass/soil materials or a forest-scale placement budget.

The running author checks found one incorrect cliff assertion on both Windows and
Linux: x=2.00348 m lies in the grid's interpolated drop cell [2, 2.25], not beyond
a vertical wall. Its endpoint top is y=-0.09501 m, below the interpolated ground.
The test now checks that interpolated height and requires 1.9 cm burial. All 18
printed cliff endpoints satisfy the corrected predicate (minimum 2.5 cm burial).
The owner requested no further rebuild or merge gate; recompiling and running the
corrected test belongs to the overnight session. Linux Clang debug completed 58/59 affected CTest groups, with only that assertion
failing; its author volumes were pruned. Windows affected checks were still running
when the owner authorized landing without another build or gate. Review artifacts live outside the
repository under `tree-growth/2026-10-27/roots/` in the local workspace.
Machine state: shared development machine; no performance claim before measurement.
