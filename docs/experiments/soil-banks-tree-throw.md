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

MSVC debug builds and the soil tests pass; all 34 container-section hashes for each
of the four defaults and all four terrain hashes match Clang 18 on Linux. The
filtered Linux run passed 13/13 tests (soil, scene_gen, renderer, engine_view,
engine_content, documentation and lint). The command follow-up passed 7/7 Linux checks after the distant-scene configuration
fix. Windows `test -Affected` passed 123/123 (5,159 s shared-machine wall time); final-source soil and content-command follow-ups also passed. No existing
solver pin moved.

The five authored studies live at `D:/workspace/game_engine_local/soil/2026-10-28/`:
recipe, terrain, scene, provenance, cameras and captures at 5, 20, 60 and 120 m.
The 20 m comparison uses the same raster path at LOD 1 pixel and 0.000001 pixel;
it includes terrain, shadows and the feature, not an isolated object mask. Full-frame
RGB8 RMS differences are 0.54 (scarp), 0.75 (gully), 0.40 (undercut), 0.21 (fresh
throw), and 0.22 (weathered throw), out of 255. These are observations, not
acceptance thresholds. At 60/120 m the feature becomes a small silhouette on the
finite 32 m terrain study; there is no extra impostor switch.

The underside capture shows a recessed face beneath the lip, exposed stones and
hanging woody roots. Plate shapes and radial roots remain stylized; fallback
materials lack soil grain, clods and fine roots. The complete fallen tree and
vegetation clearing are outside this first solver. Visual acceptance is the owner's.

### Shared-machine measurements, 2026-10-28

RTX 5090, MSVC debug, 1280x720, CSM, sun 120/35 degrees, 5 m cameras, 30 frames,
three repeats, 120 warmup frames and the harness's two-second warmup floor.
`--wait-quiet 10` was used under the GPU lock. **Every run was non-quiet: these
numbers are upper bounds, not feature costs.** The before scene is the identical
cooked terrain with its feature mesh removed. Subtracting these noisy timings is
not justified (the scarp's after is lower). Raw frames and full `machine_state`
records are beside each scene and collected in `measurements.json`.

| Study | Source triangles | DAG clusters | Container bytes | GPU median before/after (ms) | After p95 (ms) | Visible pairs | Shadow pairs before/after |
|---|---:|---:|---:|---:|---:|---:|---:|
| Scarp | 8,880 | 642 | 1,331,337 | 0.1744 / 0.1692 | 0.1727 | 348 | 31 / 459 |
| Gully | 11,456 | 813 | 1,660,984 | 0.1990 / 0.2042 | 0.2055 | 383 | 18 / 677 |
| Undercut | 10,128 | 723 | 1,501,468 | 0.1730 / 0.1764 | 0.1781 | 462 | 32 / 747 |
| Fresh throw | 12,992 | 983 | 2,033,354 | 0.1712 / 0.1751 | 0.1766 | 476 | 191 / 1,016 |
| Weathered throw | 11,264 | 841 | 1,746,139 | 0.1707 / 0.1711 | 0.1733 | 419 | 147 / 848 |

The after runs' `machine_state` CPU-others start/end percentages were respectively
9.73/15.10, 23.50/18.36, 26.04/29.10, 22.91/17.42 and 15.09/24.01. GPU utilization
start/end was 0/68, 1/56, 0/0, 0/0 and 0/48 percent. All reported an unlocked
session, the GPU lock held on this run's behalf, and device memory 6,901/32,607 MiB.
The renderer's own allocation report was 208 MiB both before and after for every
study; allocator granularity makes this unsuitable for an incremental mesh-memory
claim. Container bytes above are serialized mesh plus DAG/material/recipe data,
not resident GPU bytes. Each ordinary instance has the existing 96-byte GPU
InstanceDesc; pair buffers and collision proxies remain the existing renderer and
collision costs. These finite studies add 642–983 pairs per feature; total pairs
are 3,544–3,890, below the 2^24 cap. Every measured frame reported zero engine
allocations. No regional placement/tile workload was introduced or measured. The
merge's frame-budget gate is still required; these studies do not replace it.

### Far origin and integration lessons

At (64,000,000, 0, 64,000,000) m, the fitted undercut recipe is byte-identical to
the origin cook and the surface/mesh alignment remains intact. Its scene uses the
existing world ground path. That path needs the source far chain to match its
outer ring: 500 mm, ratio 4, six levels for this study's 8 m / 16-cell outer ring.
A local `derived` directory is named explicitly because generated terrain outside
a checkout has no repository cache to discover.

**Unresolved observation (F24):** that far-origin review loses CSM casters. Its
shadow-fit log reports near/far 90,509,640/90,509,688 m and zero cascade pairs,
while the local scene reports a 0.1–27.6 m fit and 747 pairs. Geometry is present
in the saved image, but it lacks those cast shadows. The cause is not diagnosed
and no renderer code is changed in this capability branch.

Keep the schema LF-normalized: an accidentally mixed CRLF documentation line was
accepted by MSVC but emitted a literal carriage return in a reflected description,
which Clang correctly rejected. Normalize the source; never patch generated code.

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
