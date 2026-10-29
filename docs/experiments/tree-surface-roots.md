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

## G102 morphology review (2026-10-28)

The v19 forest review identifies angular profiles and a distinct trunk join. Its
review tree is a beech with resolved basal radius 0.6600000262 m. The companion
uses a 0.25 m ground grid and a 3.960000157 m reach. These are site inputs, not
botanical constants. The following primary papers were read before redesign:

- [Coutts, Nielsen and Nicoll (1999), *The development of symmetry, rigidity and
  anchorage in the structural root system of conifers*](https://doi.org/10.1023/A:1004578032481)
  ([author full text](https://www.researchgate.net/publication/304425451_The_development_of_symmetry_rigidity_and_anchorage_in_the_structural_root_system_of_conifers)):
  structural roots differ in size, direction, branching and stiffness; mechanical
  reinforcement includes T- and I-shaped sections. A symmetric set of identical
  cones would discard the morphology this review describes.
- [Danjon, Fourcaud and Bert (2005), *Root architecture and wind-firmness of mature
  Pinus pinaster*](https://doi.org/10.1111/j.1469-8137.2005.01497.x)
  ([author full text](https://www.researchgate.net/publication/7547199_Root_architecture_and_wind-firmness_of_mature_Pinus_pinaster)),
  pp. 390–391: ZRTc uses **relative cross-sectional area loss**, averaged from the
  axis origin, greater than 1.25% per centimetre. This is not a local radius decay
  constant. Their alternative ZRTt radius is 2.2 times **DBH**, not basal diameter.
  Rooting depths were 0.74–1.28 m; shallow/deep compartment boundaries were 33%/75%
  of each tree's maximum rooting depth. These do not specify that every surface
  root dives at one depth or distance.
- [Nicoll and Ray (1996), *Adaptive growth of tree root systems in response to wind
  action and site conditions*](https://doi.org/10.1093/treephys/16.11-12.891)
  ([author full text](https://www.researchgate.net/publication/8778317_Adaptive_growth_of_tree_root_systems_in_response_to_wind_action_and_site_conditions)),
  pp. 893–895: the measured upper/lower vertical distances from the biological
  centre have mean ratios 4.24, 3.58, 2.60 and 1.20 at 0.5, 0.75, 1.0 and 1.25 m
  from the stem centre. These are **not height/width ratios**. Reinforcement is
  strongest near the stem; sections farther out become more ovoid. The 0.6 m
  shallow/deep split is a study grouping, not a universal root depth.
- [Crook, Ennos and Banks (1997), *The function of buttress roots*](https://doi.org/10.1093/jxb/48.9.1703),
  pp. 1706–1707, Table 1: tropical buttressed laterals have mean height/width 7.9,
  compared with 1.07 for non-buttressed Mallotus. Laterals remain straight for
  over 2 m; sinkers occur on only some buttressed laterals. This supports a
  stem-to-lateral reinforcing wedge, but copying tropical plate dimensions onto
  every temperate species would not be justified.
- [Namm and Berrill (2016), *Tanoak coarse root morphology*](https://doi.org/10.4236/ojf.2016.61001),
  equation 1 and Table 3: taper depends on proximal circumference and path
  distance from the lignotuber (2,151 segments from 12 trees). It is a fitted
  tanoak model, not a universal two-rate law for the supported forest species.

### Junction information boundary

The current recipe cannot identify an exact trunk surface. `roots_for_tree`
copies species, seed and two dimensions; it discards age/site/vitality, trunk
height, lean, taper and the resolved wood relief. In `kit.cpp`, `build_tube`
multiplies the nominal radius by basal flare, angular fluting, ovality and knot
terms in the transported trunk frame. The flare span is
`min(0.3 * axis.length, 3 * r0)`; `maturity.cpp::age_wood` scales its amplitude.
Ancient trees use a different, explicitly authored buttress surface.

Consequently a circular collar reconstructed from basal radius alone cannot be
asserted equal to that surface. An exact separate-asset join needs a serialized
resolved boundary (including its frame and normals) or equivalent complete tree
inputs and a shared evaluator. That is a schema/resolver extension, outside the
G102 file allocation; duplicating guessed species coefficients in `roots.cpp`
would conceal the missing information. No botanical taper rate or flare dimension
is pinned by this review alone.

### Close-view tessellation checkpoint

This is a partial G102 change, not acceptance of the morphology or trunk join.
The canonical stations, quadratic radius taper, elliptical section and overlapping
trunk attachment remain unchanged. Spline fitting, a defensible two-rate taper
calibration and a continuous resolved collar still require work.

The unaccepted version 2 candidate prioritizes the first 2 m of primary roots for 16 sides, with eight on
the rest. Budget fallback uses 12, then eight near sides, then four everywhere;
the coarse authored level always uses four. This distance and these side counts
are engineering detail choices, not measurements from the papers. They do not
change a root's radius, length, count, ground stations, endpoints or mask. The
cost of a closed path is twice the sum of its ring side counts, including unequal
ring transitions and both end fans. The 12,000-triangle default and refusal when
four sides cannot fit are retained.

For a circular section, maximum chord error is `r * (1 - cos(pi / sides))`:
0.07612 r at eight sides, 0.01921 r at sixteen. An ellipse multiplies the unit
bound by its largest semi-axis. The four-sided coarse error also includes the
change in section frame/size between both segment ends, bounding the difference
between triangulations of the ruled strip. The finest source still has zero
reduction error in the cluster hierarchy; an already four-sided source reports
zero, with the cook's existing 0.0001 m hierarchy floor.

The unchanged v19 review recipe, cooked in this worktree:

| Quantity | Version 1 | Version 2 |
|---|---:|---:|
| Source triangles | 5,312 | 7,760 |
| Cooked vertices | 5,251 | 7,055 |
| Clusters, both levels | 98 | 126 |
| Authored levels | 2 | 2 |
| Container bytes | 244,384 | 318,368 |

The extra 2,448 source triangles fit with 4,240 triangles still unspent. No third
LOD level, runtime solver, wind pose, collider or always-resident per-world state
was added. Cooked geometry and its streamed resident data can grow with the
number of root assets/placements; the table is one asset, not a forest-wide
memory measurement. Machine state: shared development machine; these are geometry
and file counts, not performance measurements. Cook times are unqualified and
are not used as before/after performance evidence.

### Validation checkpoint: not accepted

The baseline and candidate were captured offscreen at the v19 review site's
camera, using the matching tree/root transforms and bark, a private copy of the
DDC, hardware rasterization and traced shadows. The extra near facets round the
root cross-sections visibly. The angular paths and distinct attachment to the
trunk remain visible; these captures do not establish a continuous junction.
The viewer reports `machine_state: null` and `quiet: false` for these capture
runs despite the requested quiet wait, so no performance comparison is made.

The E10 helper's unchanged fit orbit, offscreen cuts and coverage calculation
were applied to the cooked root containers directly (the normal harness accepts
glTF and would rebuild the authored hierarchy). At 640 by 640, four frames,
LOD errors 1 px and 0.05 px, shadow mode off:

| Quantity | Version 1 | Version 2 |
|---|---:|---:|
| Coarse / finest visible pairs | 61 / 61 | 89 / 89 |
| Finest pairs per 1,000 object pixels | 3.6863 | 5.2484 |
| Coarse / finest share | 1.0 | 1.0 |
| FLIP mean | 0 | 0 |

Both fail the existing collapse rule: coarse share must be at most 0.75 when
density is at least 3 pairs per 1,000 object pixels. The identical cuts explain
the zero image error; it is not evidence of successful reduction. Neither the
threshold nor the declared geometric error was reduced to obtain a pass. E10's
offscreen host does not record machine state; these are selection counts, not
timings. G102 therefore remains unaccepted pending the coordinator's review of
this inherited collapse failure and the junction's missing resolved inputs.

Two added solver tests exercise budget fallback without changing the coarse
surface or exposed mask, reject degenerate transition triangles, and compare
samples on both fine and coarse triangle surfaces against the declared error.
The nine focused root cases pass 81,476 assertions on MSVC. These tests cover
the tessellation checkpoint only; the requested two-rate taper, exposed-section
morphology and exact flare equality tests still require their implementations.

## 2026-10-29 resolved collar implementation plan

G102 resumes across the schema/resolver boundary identified above. Serialize a sampled lower
stem surface (positions, normals and bark UVs), extracted from the solved trunk triangles,
rather than reconstructing species relief from nominal radius. A public binding helper lets
consumers reuse an already solved tree; the convenience recipe helper may solve once at cook
time. Keep this boundary in the root recipe/cache key. It is not runtime per-placement work.

Add an outward-flared sleeve, with lobes following the surviving primary root directions.
Its upper edge embeds slightly into the sampled trunk, with zero flare derivative and matching
normal/UV there; its lower edge is buried against the same terrain query. This is an overlapping
separate-asset junction, not a Boolean union or a watertight welded tree/root mesh. The sleeve
cannot fix different material sets or a consumer that places the companion in a different frame.
Rigid roots only cover the low-weight trunk base; large wind bends can still separate assets.

Primary root tops gain an asymmetric reinforcing ridge near the stem, declining over three
basal radii; the lower envelope and buried ends stay unchanged. Use 2 basal radii of added
height at the hidden origin. These are restrained authored visual scales, not the tropical
7.9 height/width ratio. Existing root reach, counts, terrain stations and tails stay fixed.
The collar keeps identical triangles at both LODs so the junction cannot collapse independently.
Budget accounting includes it; no silent budget overrun. A constrained site that cannot contain
the entire collar omits it, retaining only the constrained roots. Roots-off omits both.

The shipping candidate uses roots version 3: version 2 already appears in local caches from
the unaccepted overnight tessellation experiment. This prevents either candidate from reusing
the other's standalone-root containers. The new collar adds exactly 297 source vertices and
512 triangles at the helper's 9 x 32 resolution, retained in both authored LODs. Root ridges
move existing vertices and add no triangles. The 12,000-triangle default still includes both.

The flat review uses a 15 m oak with 0.45 m basal radius. Binding now takes its resolved
7.2 m crown reach (the older comparison recipe used 6 m), so the distal exposures are not a
controlled performance comparison. The final candidate's flat source has 9,088 triangles.
Machine state: shared development machine, captures serialized under the GPU lock; no timing
or frame-cost conclusions are drawn. Flat, slope, terrain-edge, tight-bed, disabled and coarse
views are recorded externally under tree-growth/2026-10-29/root-flare-species.
The earlier E10 default-cut checkpoint above is not declared fixed by these visual comparisons.

The binder reads only the first (bark) primitive and motion part zero. Foliage can inherit that
motion part, so scanning all vertices for the bottom ring incorrectly treated leaf UV v=0
as a trunk base. The regression covers beech, cedar, birch, willow, conifer and orchard.
A tilted ground ring is sampled above its highest rim vertex. The angular seam follows the
actual bark UV seam at each height, preventing interpolation across the UV wrap.

Ancient trees are deliberately excluded from the fitted sleeve: their source trunk has authored
buttress fins and recessed openings, so a ring loft would bridge that topology and could narrow a
village entrance. Their existing buttresses and standalone root ridges remain supported.

## Author validation, 2026-10-29

Windows msvc-debug affected coverage: 60 CTest groups passed; engine_view exhausted the
whole-executable GPU deadline twice on this shared machine. The same 142 default-enabled
viewer cases then passed in four ordered, non-overlapping ranges (1–36, 37–72, 73–108,
109–142). Ranged case lists were checked against the full list, and the separately registered
frame-loop test passed in the affected run. No assertions, skips or GPU lock rules changed.
The final ancient-stem safeguard was rebuilt and passed all 10 root cases (37,664 assertions).
Linux Clang debug passed all 61 affected groups; the final guard passed a further five groups
(flora, engine_content, lint, licences and docs). Worktree Linux volumes were pruned afterward.
These are author checks, not the full merge gate or quiet performance measurements.
The prior root E10 collapse checkpoint remains unresolved.
