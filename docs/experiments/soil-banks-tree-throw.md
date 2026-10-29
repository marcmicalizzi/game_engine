# Eroded soil banks and tree throw (G100)

Research read on 2026-10-28, before the solver design. This is a static morphology
solver, not a soil stability, hydraulic erosion or windthrow simulation.

## Grounding and budget choices

| Quantity or rule | Source read | Value used | Budget trade |
|---|---|---|---|
| Fresh spruce pit dimensions | [Zadrozny et al. 2017, table 1](https://iforest.sisef.org/contents/?id=ifor1959-010) | 1-year class: width 4.23 m, length 2.74 m, depth 0.95 m; plate height 2.35 m; mineral mound height 0.71 m | One authored reference profile, not universal tree allometry; ground is cooked on the source lattice |
| Weathered pits | Same table | 3-year and 5-year depths 0.65 and 0.52 m | Separate recipes, not a fitted time-evolution law |
| Undercut beneath a stronger surface layer | [Bentrup and Hoag 1998, pp. 27–28](https://www.nrcs.usda.gov/plantmaterials/idpmcpu116.pdf) | Layered bank, recessed lower face, supported surface lip, loose material at the foot | Static cross-section with a terrain excavation underneath; no stability claim |
| Root exposure and shallow lateral systems | [Surface-root research](tree-surface-roots.md) | Tapered, branching woody laterals; cut ends hang below the lip or attach to the lifted plate | Rigid tubes; no absorbing roots or wind animation |

The spruce measurements describe 75 uprooting sites in mountain stands in southern
Poland, grouped by age. They are not a paired longitudinal experiment. The solver
does not infer a DBH or soil volume conservation law from these group means.

Bank dimensions, layer thicknesses, root counts/radii, rock sizes, roughness and
colour are authoring controls: neither source supplies universal values. Their
defaults and bounds are documented in the schema, explicitly separate from the
measured spruce profile. Numeric sampling and topology constants are engineering
choices, not geological measurements.

## Integration design

One cook produces the edited heightfield and a recipe containing the original local
ground samples. Banks add closed layered soil meshes over excavated ground; a pit
and mound are actual edits to the heightfield. Both are consumed through existing
terrain and mesh registries. The native terrain is the lower surface beneath an
undercut; the bank mesh supplies the overhang and its collider. Stones use the
existing rock solver, and woody roots are rigid members of the soil mesh. A moved
feature or changed terrain requires a new cook.

## Validation

Implementation and measurements pending. Machine state: no timings taken yet.

## Explicit authoring and engineering constants

These values are not measurements of soil or root mechanics. They are bounded
source-mesh choices to be evaluated in the engine captures.

| Control | Value used | Reason and trade |
|---|---|---|
| Bank profile | depth 0.95 m, width 4.23 m and reach 2.74 m by default; undercut 0.3 m, at most half depth and 0.6 m | Reuses the reference scale for a compact study, without claiming these are typical banks |
| Horizons | organic 0.06 m, topsoil 0.22 m, lower material split at 75% of depth | Authored soil section; organic/topsoil boundaries are explicit mesh rows |
| Face relief | sinusoidal 0.025 m amplitude, frequency 7 rad/m, zero at lip and foot | Small soil aggregate/rill relief, not hydrology; zero at boundaries preserves joins |
| Bank ends and backing | last fifth fades; minimum buried depth 5%; backing 0.5 m beyond recess; foot burial 0.04 m | Closed end caps and ground overlap; not geotechnical support limits |
| Roots | 10 majors, 0.045 m base radius, 12 segments, eight/four sides, tip 6% of base; two six-segment branches at 45% radius on the plate | Static visible woody roots only; keeps count and centreline on budget changes |
| Hanging bank roots | 0.35 m buried start, 0.65 m outward reach, drop 75% of bank depth; 0.16 m lateral wander | Authored torn-root shape, not an inferred species architecture |
| Plate roots | elliptical spokes at 49% pit width and 52% plate height; starting at 42% height; 0.18 m droop and 0.3 m outward displacement | Visual approximation over the soil plate; no mechanical coupling |
| Plate body and bole | half-thickness 0.22 m, burial 0.06 m; 3 m bole stub, 0.24 m base radius | Representation of attached wood, not the whole fallen crown or a DBH inferred from the study |
| Stones | eight, 0.1 m nominal radius (80–100% seeded), 500 triangles each | Existing granite rock model; minimum nominal 0.0625 m respects its minimum half-extent |
| Source budgets and sampling | 12k/16k/24k by kind; cells 0.08/0.125/0.2/0.35/0.6 m; plate 5k/1.2k | Offline ladder, all requested parts retained; too-small budgets fail |
| Terrain | 25–250 mm source samples; local patch up to 513² values; feature plus at least one-cell apron | Bounds cook memory and ground/mesh mismatch; no frame-time sampling |
| Pit and mound | squared positive elliptical paraboloids; mound centre 0.75 pit lengths behind centre | Smooth compact footprints with zero slope at their edges, not a mass-conservation claim |
| Fallback materials | six linear RGB colours and roughness 0.95, listed in solver.cpp | Preview palette only, no claim of measured reflectance; role-based packs override it |

The sphere-like soil plate uses the existing rock solver's irregular body, not a
new geological model. Its geometry does not preserve a transported soil stratigraphy.
No existing solver output or determinism pin is changed.
