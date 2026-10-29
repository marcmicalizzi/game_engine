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

### Version 1 validation

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

## Version 1 authoring and engineering constants

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

The version 1 soil plate used the rock solver's irregular body, not a new geological
model. Its geometry did not preserve a transported soil stratigraphy. That initial
implementation changed no pre-existing solver's output or determinism pins.

## Capture review follow-up (G105, 2026-10-29)

Initially reserved as G103/G104 in c8996048; renumbered to G105/G106 after checking
the concurrent shrub and deadwood branches, which had independently reserved those
IDs. Authored leaning contact is the deadwood follow-up's; this change adds none.

The v1 close captures were reviewed before changing coverage: plates read as broad
rectangular slabs with sparse radial roots, and paired banks as straight excavated
sections. G105 targets their shape within the existing triangle budgets. The original
studies above remain v1 evidence, not validation of the new output.

[Krisans et al. 2020](https://www.mdpi.com/1999-4907/11/11/1143)
(also reproduced in the author's [institute-hosted thesis](https://silava.lv/images/Produkti/Promocijas-darbi/2020-Krisans-Oskars-Promocijas-darbs.pdf))
models spruce root plates with an elliptical footprint and decreasing depth away
from the stem. This supports a tapered plate instead of an extruded rock slab.
The Latvian sample is distinct from the Polish dimension reference above: no new
universal thickness, root count or clod distribution is inferred from either.

[NRCS, Erosion and Sediment Delivery, pp. 4-6](https://www.nrcs.usda.gov/sites/default/files/2022-09/Erosion_%26_sediment_delivery__IA-NRCS_Procedures.pdf)
distinguishes localized eroding banks, headcuts and stable reaches, and curved
channel planforms. The visual approximation will share one curved profile between
mesh, excavation and attachments; it does not simulate drainage or predict bend
wavelength. Smooth buried ends represent a finite authored reach, not an active
headcut or a connected watershed network.

Composition remains G106: the bole is a stub, not a complete fallen crown; no
leaning tree-to-tree support example or automatic support discovery is claimed.
Steep excavated soil requires an explicit mask region's `material` override,
not its `base` field (which only replaces the prior's soil). The native mask
format still carries at most four layers per tile; wider blending remains engine work.

The earlier constants table describes v1. Version 2 changes the following authored
and engineering choices; the research supports the structure, not these amplitudes.

| Control | Version 2 choice | Budget/representation trade |
|---|---|---|
| Bank planform | smooth hash stations every 2.5 m, amplitude min(0.8 m, 12% span, 22% reach); offset measured from the central station | Shared by mesh and cook; finite authored curve, no drainage solver |
| Gully width | 70-100% nominal half-width, smooth stations every 1.7 m | Opposed banks remain separated; one continuous reach |
| Ends | smooth ramps over first 28% and last 38% span; 5 cm burial at caps, 2.5% minimum closed depth | Caps disappear below untouched ground; no active headcut claim |
| Plate | ellipse at 46% height; half-axes 50% width / 54% height; radial factor 0.82 plus 3/7/13 angular harmonics of 0.08/0.07/0.05 with seeded phases | Torn lobes replace rectangular silhouette; not a measured outline spectrum |
| Plate depth | half-thickness 0.22 m at stem, 0.025 m at rim; 0.055 m relief scaled by r(1-r) | Taper follows the research direction; nominal scale retained from v1 |
| Sampling | 64 sectors x 10 rings or 32 x 6; matched front/back rim samples | 2,560 or 768 triangles, closed at either rung |
| Clods | 48 closed aggregates, twenty triangles each, radius 1.8-5.3% of smaller plate dimension; vertex radii 80-120% nominal | 960 triangles at every rung; centimetre/millimetre soil grain still belongs to materials |
| Root curvature | angular bend 0.3 rad, reach 108-124% envelope; 2.5 cm surface offset plus 0.22 m outward tip and 0.18 m drop | Follows the envelope before hanging free; roots are rigid |
| Secondary/finer roots | two branches per major, 0.25-0.55 m lateral reach; three four-segment fine roots per branch, 0.12-0.37 m reach, radius 12% major | Four-sided closed tubes; source ladder retains every branch |

The solver cache version moves from 1 to 2. All four container pins change;
bank terrain pins change with the shared curve and ramps, while tree-throw pit
and mound heights retain their original equations. No other solver's output changes.

The first clod candidate's eight-face aggregates read as pyramids in the close
capture. The retained candidate uses twenty-face irregular icosahedra; the rejected
image is preserved with the local study. Tiny root facets also exposed a numerical
issue: the general vector normalizer returns zero below its length epsilon. The
soil builder now normalizes valid facet area explicitly, rejecting only squared
area below 1e-24; exact-weld closure is checked at both 5 mm and 45 mm root bases.

### Version 2 review and measurements

The paired study is `D:/workspace/game_engine_local/soil/2026-10-29-review/`:
`before/` holds v1 containers and original terrain; `after-final/` holds v2 cooks,
recipes and containers. Both draw through the same worktree-built renderer, so the
intervening shadow-map bias change does not confound the comparison. Cameras,
provenance and commands are beside the captures. `iteration-1/` preserves the
rejected eight-face clod image; `after/` is the initial candidate, not the final one.

Close views show the broken plate outline, curved roots and finer branches. The
24 m gully overview and end view show a continuous curved reach with unequal
ramps into undisturbed ground. Bands and flat fallback materials still look stylized;
there is no claim of photo-real soil, a branching drainage network or owner acceptance.
Regional coverage was not expanded. The 20 m full-frame RGB8 RMS against a
0.000001-pixel finest cut is 0.57/1.03/0.53/0.24/0.21 for scarp/gully/undercut/
fresh/weathered throw. It includes terrain and shadows, so it is an observation,
not a mesh-only error or an acceptance threshold.

RTX 5090, MSVC debug, 1280x720, CSM, sun 120/35 degrees. The matched camera path
approaches from 20 m to 5 m and crosses the feature; 120 frames, three repeats,
120 warmup frames, `--wait-quiet 10`, under the GPU lock. **Every before and after
run was non-quiet; these timings are upper bounds, not incremental costs.**

| Kind | Source triangles before/after | DAG clusters before/after | Container bytes before/after | GPU median before/after (ms) | After p95 (ms) | Shadow-pair median before/after |
|---|---:|---:|---:|---:|---:|---:|
| Scarp | 8,880 / 8,880 | 642 / 643 | 1,331,337 / 1,343,081 | 0.1782 / 0.1839 | 0.1942 | 460 / 516.5 |
| Gully | 11,456 / 11,456 | 813 / 819 | 1,660,984 / 1,669,272 | 0.2075 / 0.2089 | 0.2157 | 670 / 663 |
| Undercut | 10,128 / 10,128 | 723 / 714 | 1,501,468 / 1,486,796 | 0.1785 / 0.1805 | 0.1913 | 743.5 / 683 |
| Fresh throw | 12,992 / 14,112 | 983 / 1,028 | 2,033,354 / 2,139,850 | 0.1766 / 0.1785 | 0.1907 | 952 / 1,202 |
| Weathered throw | 11,264 / 11,424 | 841 / 823 | 1,746,139 / 1,713,547 | 0.1753 / 0.1759 | 0.1837 | 838.5 / 942.5 |

The fresh plate costs more visible and shadow work: visible-pair median 461 to
618.5 and shadow median 952 to 1,202. The detailed roots are not free. Source
triangles remain below the unchanged kind budgets, and every measured frame has
zero engine allocations and device-buffer creations. Reported process GPU memory
is 194 MiB before and after in every pair; this granularity does not establish a
zero memory cost. Container sizes above are serialized data, not resident GPU bytes.
The instance remains the engine's existing 96-byte record; no runtime data is added.

After-run `machine_state` CPU-others start/end percentages, in table order, are
26.86/26.00, 44.97/23.37, 30.02/21.66, 22.08/26.54 and 31.08/28.47; GPU utilization
is 1/1, 2/1, 2/69, 1/3 and 3/6 percent. End-of-run host device memory is respectively
5,887 / 5,896 / 5,836 / 6,102 / 5,824 of 32,607 MiB. All report an unlocked session
and this run's GPU lock. Full before/after states, counts and timings are retained
in `measurements.json` and each `summary.json`; the shared machine precludes
attributing these timing differences to the shape change alone.

### Version 2 verification

The Linux Clang debug filter
`soil|tileset|scene_gen|renderer|engine_view|engine_content|docs|lint`
passed 14/14 checks in 174.71 s. A final soil-only run
after the out-of-footprint guard and stone-helper cleanup passed in 3.41 s;
the worktree's two Linux build volumes were then pruned. All four container and
terrain pins match Windows, including every pinned container section.

The Windows debug affected run completed 37 of its 38 checks; `engine_view`
exhausted the shared GPU queue deadline and was skipped. The affected run took
5,402.29 s on the busy machine. The final-source soil rebuild/recheck passed in
6.81 s, but the accompanying viewer retry also exhausted its 1,800 s deadline.
Neither skipped viewer run counts as a pass. A further focused viewer retry uses
the documented local queue allowance of 3,600 s (CTest timeout 4,200 s), with the
same assertions and per-device lock. See [GPU-lock configuration](../subsystems/gpu_lock.md)
for the Windows environment needed to set that cache override. The initial direct
CMake attempt outside that environment failed; `dev.ps1 configure` restored the
configuration before the override and rebuild. No tracked build setting changed.

That final viewer retry passed in 3,182.98 s, completing coverage of all 38
affected Windows checks. Its long wall time includes shared GPU contention;
it is not a rendering-cost measurement. Lint, formatting and documentation checks
also pass. The paired close views, longer gully and LOD comparisons above were
reviewed before any expansion of regional coverage; composition remains G106.
