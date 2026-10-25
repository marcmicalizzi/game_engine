# Rock structure and detail (2026-10-25)

Work in progress: G88. The owner's ten photographs motivate joint-bounded pinnacles,
layered escarpments, angular shore boulders and dense debris. They are visual references,
not measurements or enough evidence to identify lithology.

## Grounding before implementation

| Quantity or rule | Source read | Value used | Budget trade or limitation |
|---|---|---|---|
| Fracture sets organize pinnacles and admit weathering | [NPS, Station 4: Joints](https://www.nps.gov/places/geological-trail-station-4.htm) | Use the body's joint directions for internal surface fractures, with one spatial field across an outcrop | A radial surface can represent recessed joints, not detached fracture volumes |
| Roughness depends on measurement scale and lithology | [Abe and Deckert 2021, section 4.2](https://se.copernicus.org/articles/12/2407/2021/) | Separate metre-scale joint structure from successively finer surface relief | The paper's 0.1–1 cm self-affine range does not justify extrapolating a universal exponent to a mountain |
| Laboratory roughness exponents | Same paper, section 4.2: sandstone H = 0.66; limestone H = 0.69 at maximum sampling area | A declining octave spectrum is a rendering approximation; no claim that value noise reproduces the measured surfaces | Details below mesh sampling go to materials, rather than aliasing into geometry |

Joint spacing, opening and weathering depth vary by site and cannot be recovered metrically
from these photographs. Recipe controls expose physical lengths; defaults and demonstration
values are authoring choices, explicitly not measured geology. The existing four lithologies
remain procedural styles rather than site-calibrated geological simulations.

## Representation and authoring choices

The default surface is deliberately separate from the four pre-existing macro-shape profiles.
This change does not claim to calibrate those profiles to a geological site. In particular,
the photographs' large pinnacles should not be identified as granite solely from their shape.

| Choice | Value used | Why and limit |
|---|---|---|
| Default joint separation / full recess width / maximum depth | 0.65 / 0.12 / 0.12 m | Demonstration-scale controls for metre-sized boulders, not field measurements; all editable in `RockSurface` |
| Default relief wavelength / amplitude | 0.4 / 0.018 m | Keeps broad cut faces visible; the initial 0.035 m amplitude looked crumpled in untextured captures and was reduced |
| Irregular stations | Each integer station displaced by ±0.3 spacings | Bounded jitter keeps stations ordered (minimum separation 0.4 spacings) and permits an exact three-station neighbourhood; not a fitted joint-spacing distribution |
| Plane wandering | ±0.04 spacings, noise coordinate 0.7 / spacing | Small departures from planarity, shared by the formation; an artistic weathering approximation |
| Unequal penetration | Exposure noise at 0.9 / spacing, smoothly zero below 0.25 and full above 0.75; per-plane depth 35–100% | Intact bridges break up the continuous grid of the first prototype; these are authoring fractions, not rock strength values |
| Joint section | Cubic smoothstep over half the full width | Rounded weathered walls; no remeshing or fracture volume |
| Relief spectrum | Five octaves, wavelength ×0.5, amplitude ×0.6 | A finite rendering spectrum, not a measured Hurst law; the cited laboratory results explicitly do not establish this metre-scale spectrum |
| Feature sampling | Zero at two mesh cells, full at four, quintic transition | Sampling protection: joint **width**, not spacing, determines visibility; unresolved features are omitted, never widened to fit the mesh |
| Radial safeguard | Radius scale 0.8–1.05 | Prevents large authored recesses collapsing small fragments; deliberate departure from the requested depth when it would exceed the shell's limits |
| High-detail ladder | Add n = 160 and 192 | A 500,000-triangle boulder can spend 442,368 instead of stopping at 196,608; the limit still remains explicit |
| Temporary lattice index | `6 n² + 2` slots rather than `(n+1)³` | Only the shell exists: at n = 192, 884,744 rather than 28,756,228 bytes for this lookup (32.5× smaller); not a measurement of total solve memory |

An outcrop and scree already are authorable rock clusters: one recipe, one mesh instance,
many closed bodies, the normal cluster DAG for runtime detail and collision. The new
`jointed-pinnacles` and `shore-rubble` recipes demonstrate these existing grouping types.
They do not add a separate placement system. Scree is still spread on a plane with bounded
separation attempts: it is not gravity-settled debris, and intersecting fragments remain possible.

## Triangle counts

| Recipe | Before | After | Source budget after |
|---|---:|---:|---:|
| Granite boulder | 19,200 | 76,800 | 80,000 |
| Sandstone boulder | 19,200 | 110,592 | 120,000 |
| Basalt outcrop | 50,304 | 258,048 | 320,000 |
| Granite scree | 13,152 | 24,384 | 32,000 |
| Jointed pinnacles (new, eight bodies) | — | 352,512 | 400,000 |
| Shore rubble (new, forty-eight fragments) | — | 265,344 | 320,000 |

These are source-mesh counts, not per-frame triangle counts. The cliff face, its budget and its
talus fragments retain their previous geometry. Solver version 2 invalidates cached containers,
including cliffs, even where mesh bytes remain unchanged. Boulder/Outcrop/Scree schema version 2
adds `surface` and increases their default budgets to 80k/240k/24k; callers can explicitly lower
them. All six changed or new recipe meshes and the default mesh/container have determinism pins.

## Scope still open

This is a geometry improvement, not an AAA-quality completion claim. Photographic material
variation, mineral grain, staining, lichen, settled talus, fractured volumes, boulder overhangs,
and terrain-scale geological continuity remain separate work. Cliff ledge UV stretching is
unchanged. The neutral-material captures intentionally expose the geometry's remaining limits.

## Review and validation

Existing proof-of-concept recipes may change, as explicitly authorized by the owner on
2026-10-25. Captures, copied recipes, camera paths, exact command arguments, GLBs and reports
live outside the repository at
`D:\workspace\game_engine_local\rocks\2026-10-25\rock-structure`.
The four before recipes use their old budgets and `surface.strength: 0`; the old default mesh
hash is tested unchanged, establishing the baseline. Both sides use the same camera and sun.
Close views use the finest cut; the distance views below use the runtime 1-pixel LOD cut.

Visual review: the granite retains broader faces after reducing relief; sandstone's bedding
remains too regular, the pinnacle silhouette still reads as a group of columns, and shore
rubble needs contact with terrain and settling. The captures have no material textures and
do not demonstrate photographic surface quality. No image-based acceptance by the owner is
implied by passing numerical checks.

### Distance observations (shared machine, not a cost comparison)

One granite boulder, 1200×900, hardware raster, CSM shadows, 45° sun azimuth/elevation,
90 measured frames after 30 warmup frames, MSVC debug, RTX 5090. The camera is 5/20/60 m away
horizontally and 1 m above its target. Every run held the GPU lock and requested
`--wait-quiet 30`; every summary reports `quiet: false`. Times below are **upper bounds under
different contention**, not evidence of an improvement or regression. All runs recorded zero
steady-state frame allocations and buffers; total reported device memory was 181 MiB each
(renderer-wide accounting, not per-mesh memory). The source meshes have 19,200 / 76,800 triangles.

| Distance | Visible pairs before / after | Shadow pairs before / after | Median GPU ms before / after | Median CPU ms before / after |
|---|---:|---:|---:|---:|
| 5 m | 41 / 135 | 57 / 165 | 0.0891 / 0.0942 | 0.1828 / 0.1813 |
| 20 m | 4 / 14 | 4 / 14 | 0.0762 / 0.0745 | 0.1729 / 0.1721 |
| 60 m | 2 / 4 | 2 / 4 | 0.0741 / 0.0715 | 0.1722 / 0.1612 |

The additional source detail falls to four visible clusters at 60 m. Rocks use the normal
cluster DAG, with no separate impostor transition. Shadow pairs are the harness's reported
count, not a per-cascade triangle census.

Recorded `machine_state` start → end (session unlocked, GPU lock ours in every row):

| Run | Other CPU % | GPU utilization % | Whole-machine GPU MiB |
|---|---:|---:|---:|
| Before 5 m | 25.9 → 29.1 | 6 → 5 | 6158 → 6185 |
| After 5 m | 29.0 → 37.8 | 7 → 17 | 6277 → 6290 |
| Before 20 m | 28.6 → 19.7 | 7 → 54 | 6232 → 6219 |
| After 20 m | 21.7 → 26.6 | 6 → 52 | 6172 → 6190 |
| Before 60 m | 18.9 → 22.6 | 6 → 47 | 6159 → 6171 |
| After 60 m | 9.8 → 15.8 | 5 → 10 | 6155 → 6174 |

No many-instance landscape or frame-budget gate is claimed by these isolated observations.

### Coarse versus finest

E10, 640×640, fit orbit 20.8, 1-pixel versus 0.05-pixel LOD, offscreen through engine-host,
kept UVs, no shadows or textures. All six final recipes pass the unchanged 0.02 mean-FLIP
threshold and the LOD-collapse check, with no import warnings or repaired triangles. The
offscreen harness does not sample machine state, so its build times are not claimed as costs.

| Recipe | Mean FLIP | Visible pairs coarse / finest | Container MiB before / after |
|---|---:|---:|---:|
| Granite boulder | 0.01549 | 183 / 483 | 1.0 / 4.1 |
| Sandstone boulder | 0.01978 | 256 / 838 | 1.0 / 6.0 |
| Basalt outcrop | 0.01012 | 291 / 1821 | 2.8 / 14.1 |
| Granite scree | 0.00516 | 48 / 186 | 0.8 / 1.4 |
| Jointed pinnacles | 0.01831 | 344 / 2205 | — / 19.7 |
| Shore rubble | 0.01896 | 188 / 1900 | — / 14.7 |

The four simple baseline meshes also pass (`e10-before/`). Their mean FLIP in table order is
0.01064, 0.01015, 0.00496 and 0.00483. More surface detail increases both container storage and
the image difference caused by LOD simplification; the new versions remain within the existing
threshold. These container sizes include the cluster hierarchy, not just leaf geometry.

The original 800k-budget pinnacle prototype failed at FLIP 0.02054. Broader, shallower joints
(width 0.6 m, depth 0.22 m, relief 0.03 m) alone still failed at 0.02027. Reducing its source
budget to 400k made the mesh's feature filter remove detail too fine for that representation,
and passed at 0.01831 with half the container memory. This is a budget trade, not a relaxed
threshold or a fix to the remaining column-like silhouette. Reports are preserved in `e10/`,
`e10-refined/` and `e10-budget/` under the capture directory. The two sparse rubble pictures
cover less than 15% of their frames; their object-only FLIP estimates are unreliable, so the
reported metric is whole-frame mean and the heat maps were also inspected.
