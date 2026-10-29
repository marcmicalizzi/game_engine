# Fern and broadleaf ground plants (G103)

The existing fern and herb solvers remain in the optional flora capability. This extension
addresses repeated upright silhouettes with explicit crown, creeping and basal-leaf recipes.
Sources were read on 2026-10-29 before implementation. These are static authored states, not a
phenology simulation. No photograph or source texture is copied into the repository.

## Grounding

| Quantity | Source and reported observation | Value used | Budget trade / uncertainty |
|---|---|---|---|
| Male fern dimensions | [Brownsey and Perrie, Flora of New Zealand](https://nzflora.info/factsheet/Taxon/Dryopteris-filix-mas.html): fronds 240–1190 mm, primary pinnae 17–38 pairs, longest 50–180 mm long and 13–38 mm wide; erect rhizome | crown recipe 0.9 m nominal frond, 24 pairs, longest 0.135 m with 0.027 m width; existing 14-frond count retained | Pinnae remain whole cards; their pinnules, teeth, scales and sori are not explicit geometry. The budget can merge pinna pairs, not fronds. |
| Creeping fern habit | [NC State, Polypodium virginianum](https://plants.ces.ncsu.edu/plants/polypodium-virginianum/): creeping branching rhizomes, erect-to-arching fronds; height 6–12 inches | 0.28 m fronds emerging separately along a local 0.8 m rhizome | Colony extent and ten shoots are authored sampling, not a measured spacing or density. This is a polypody-inspired habit, not a faithful pinnatifid Polypodium blade. |
| Broad basal leaves | [NC State, Hosta](https://plants.ces.ncsu.edu/plants/hosta/): basal rosette, 3–6 inch blades, elongate petioles, separate flowering scapes and six free perianth lobes; foliage dies back in late fall/winter | nominal blade 0.135 m (0.1215–0.1485 m after jitter), width ratio 0.8, petiole 0.12 m; twelve six-lobed flowers on a 0.6 m scape | Petiole length, leaf count and flower count are authored, not measured averages. Blade and petiole are independent dimensions. Flower bells retain the existing petal-fan approximation. |
| Low rosette | [NC State, Plantago major](https://plants.ces.ncsu.edu/plants/plantago-major/common-name/plantain/): basal leaves 2–8 inches by 2–4 inches; leafless flowering stems below 12 inches | 0.14 m blade, width ratio 0.65, 0.035 m petiole, 80-degree spread | Example is vegetative: no claim that the existing flower fan models plantain's tiny spike flowers. |
| Seasonal retained fronds | [NC State, Dryopteris filix-mas](https://plants.ces.ncsu.edu/plants/dryopteris-filix-mas/): deciduous; coppery autumn fronds | explicit additional dead fronds, brown separate material | No measured retention fraction, calendar or decomposition rate was found. Count is authored and all triangles count against the same cap. |

## Geometry choices and their limits

The source descriptions establish the habits, not a curvature distribution. There is no fitted
beam stiffness or measured arch angle in this implementation. `arching` prescribes the tangent
angle `start + turn*t*t` on sixteen fixed intervals: curvature increases towards the tip,
leaving a rising stipe. The existing droop range supplies the total turn. These are authored
shape controls, bounded by the legacy 5–85 degree spread and 0–90 degree turn. If a frond reaches
the local ground plane it continues along it. Ground contact is local and flat; a sloping colony
must be placed in smaller pieces. Rhizomes themselves are underground and not meshed.

An arching or dead frond has a 3 mm tapered rachis ribbon and separate two-triangle pinnae,
with a lanceolate length envelope and full-detail width 20% of pinna length. The longest
example pinnae fall inside the source's width interval; the ratio itself is an approximation,
not a regression. A card is flat, without explicit secondary pinnules. Pinna stations start
above `stipe` and end below the tip. Pinnae below local support are clamped to 2 mm clearance.
The rachis uses `max(2, floor(2*pinnae/3))` rows, a tessellation choice, independent of the
sixteen-row structural axis. One frond therefore costs `2*rachis_rows-1 + 4*pinnae` triangles.
When the source triangle budget halves pinna stations, the remaining pinnae may widen by at
most twice their fine width; further budget reductions lose coverage instead of filling the gaps
with a solid spear. Distance LOD retains every emitted pinna, as described below. The legacy
connected spray is retained
when `arching=false` and the frond is alive. Every pinna rides its attachment's bend weight.

Creeping shoots span the requested extent symmetrically along a seeded azimuth. A sine with
amplitude `crown_radius_m` meanders across it. Neither this curve nor the seed jitter is claimed
as botanical growth. The existing independent per-frond streams and lengths are retained.
Dead fronds start at 78–86 degrees and turn another 25 degrees: an authored fallen skirt,
not a measured senescence trajectory. Their length, pinna profile and budget rules are live ones.

Broad leaves use a shared midrib with two edge vertices at each internal row, single base and
tip, widest at 40% of blade length and sinusoidal shoulders. Cupping is 0–0.3 blade widths
(default 0.05); the petiole is a 2 mm ribbon. These are explicit geometric approximations;
veins, surface roughness detail, damage and serration still require material content. Sixteen
fixed axis intervals keep the underlying blade curve independent of emitted row count.
Rows are 1 (legacy diamond) or 2–12; a two-row broad blade retains its width shoulder.
A new curved blade turns 0–90 degrees; curved dead leaves use 40 degrees and cup 0.12.
With `leaf_segments=1`, dead leaves retain the legacy flat diamond, projected above support.
Basal attachments sit 12 mm above the local origin and supported blade axes stop at 3 mm;
these are geometry clearances, not measured crown heights. The seven-step inner/outer spread
variation spans 80–100% of requested inclination and is an authored de-repetition choice.
Dead leaf inclination spans 82–87 degrees. Brown RGB (linear) is authored: fern .28/.13/.045,
herb .30/.16/.055; roughness .85. A pack or recipe skirt can replace these materials.

Schema caps (40 additional dead fronds/leaves, 4 m rhizome, .4 m petiole) bound authoring costs;
they are not species maxima. Other existing constants retain [G75's grounding](understory.md).
No frame-loop code or hot runtime type changes. Motion parts and structural rows are identical
at every budget for a given recipe. All parts remain non-colliding and no renderer change is needed.
The ground leaf stem is omitted only for a nonflowering basal rosette. Its leaves attach to root,
not the scape, so wind/contact movement of the scape cannot drag the whole rosette with it.

## Budgets and validation

Fern retains 4,000 default triangles and the 500–100,000 input range. Herb retains 2,500 and
1,000–50,000. Dead organs count exactly like live ones. Fern lowers pinna stations and refuses
an insufficient floor; herb lowers stem detail, broad-leaf rows (never below two), then petals.
A curved blade costs `4*rows-4` triangles, a legacy blade two, and a petiole two. No leaf, frond
or flower disappears to fit.

For distance LOD, arching ferns hand the cluster DAG two authored rungs: fine pinnae and rachis, then
the same pinnae with every fourth rachis row. This bypasses generic decimation of disconnected
cards. Each frond is one hierarchy element. Coarse strips reuse source vertices and bend weights;
the maximum fine-vertex distance to the coarse strip centreline bounds geometric error.
Four is an authored tessellation stride, not a botanical constant. At 24 pairs a frond costs
127 -> 103 triangles, at 16 pairs 83 -> 69, and the three-pair floor costs 15 -> 13.
Herbs and legacy ferns retain the existing decimator. No whole-plant impostor chain is added.
Far coverage must be judged in the captures, not inferred
from the source count. Shadow casters retain the catalogue's existing policy and therefore can
still cost work in every cascade; this extension makes no shadow-budget improvement claim.

`k_ground_plants_version` keys only fern/herb output. Legacy geometry hashes are held unchanged;
canonical schema v2 and the dedicated version invalidate these two kinds' cached containers.
Other species' keys and output stay unchanged. A future shared-kit change that moves fern/herb
output must also bump `k_ground_plants_version`; the global tree version no longer keys these kinds.
New recipes have distinct filenames and do not
silently change accepted scene instances or the read-only forest POC.

## Measured review scene

Captured on 2026-10-29 on the NVIDIA GeForce RTX 5090, own MSVC debug build, 1920x1080,
28 fixed instances, CSM shadows, HW raster, sun azimuth 130/elevation 45 degrees, wind off.
Three repeats of the 61-frame path, ten warmup frames. The comparison removes the new fields
from each recipe for the before case; it does not change any old checked-in recipe.

| Recipe | Before triangles | After triangles | Before container bytes | After container bytes |
|---|---:|---:|---:|---:|
| fern-arching | 1736 | 1778 | 123280 | 231280 |
| fern-autumn | 1736 | 2794 | 123280 | 362640 |
| fern-creeping | 880 | 830 | 63712 | 111648 |
| hosta-rosette | 158 | 480 | 17328 | 41648 |
| hosta-flowering | 374 | 822 | 30336 | 63728 |
| hosta-autumn | 138 | 480 | 11520 | 38624 |
| plantain-rosette | 150 | 264 | 14112 | 24816 |

| Scene metric | Before | After |
|---|---:|---:|
| Candidate pairs | 656 | 1688 |
| Motion parts | 444 | 504 |
| Pose table bytes | 107040 | 121440 |
| Peak shadow pairs | 385 | 2016 |
| GPU total median / p95 (ms) | 0.11696 / 0.135488 | 0.125408 / 0.139712 |
| CPU median / p95 (ms) | 0.3143 / 0.3607 | 0.3231 / 0.3861 |
| Shadow median (ms) | 0.02368 | 0.025472 |
| GPU memory used (MiB) | 172 | 236 |

Machine state (before): quiet=False; other CPU 7% -> 11.1%, GPU 4% -> 4%, occupied GPU memory 5328 -> 5350 MiB, session locked=False. GPU lock owned throughout. Wait-quiet limit was 60 seconds.

Machine state (after): quiet=False; other CPU 4.8% -> 7.5%, GPU 5% -> 47%, occupied GPU memory 5402 -> 5425 MiB, session locked=False. GPU lock owned throughout. Wait-quiet limit was 60 seconds.

These shared-machine timings are upper bounds, not an isolated solver cost or a frame-budget
gate. The 28-instance scene is a morphology review, not a density stress test. The merge gate
owns the standard frame-budget scenes. No per-frame renderer code or allocations were added.

## Pictures and remaining limits

The capture directory is `D:/workspace/game_engine_local/captures/2026-10-29/ground-plants-g103/`.
Each recipe's folder contains its before/after source, cooked container, camera, scene, image and
logs. Whole-scene markers are under `before-markers/` and `after-markers/`; `measurements.json`
retains the complete machine state and counters. `capture.ps1` forces recooking so an intermediate
prototype with the same unreleased version cannot survive as a cached result. Individual cameras
frame the full flower scape and the smaller creeping colony. No generated binary is committed.

The near review shows a rising centre and arching outer fern fronds with actual inter-pinna gaps,
separate rhizome shoots, broad cupped basal blades, a separate flowering scape and a low brown
skirt. The old connected spray looked like an undivided spear: changing the axis alone did not
solve that, which is why the opt-in form changes the pinna topology too. Flowers still look like
angular petal fans and the blades lack veins and surface texture; these captures use flat colours.

The nominal 5 m row view crops the two extreme columns; the individual views cover every plant,
and the 20/60 m views cover the whole row. Distances label camera z, so each column has its own
actual eye distance. Fine creeping-fern pinnae are already subpixel in the row view. At 60 m the
plants are small ground detail even at finest quality; no whole-plant impostor chain is added.

The first, uncommitted generic-decimation result retained only 191 fern-region foreground pixels
at 20 m versus 2,640 near finest. Those captures and counters remain in
`v1-decimation-evidence/`. The final authored fern rung retains 2,638 versus 2,640 (99.92%) in the
same region: x < 925, y 400..699, RGB L1 distance > 24 from the background pixel. It keeps the
fern silhouettes in the 20 m and 60 m comparisons. This is a coverage measurement of these
views, not a general image-equivalence or density claim. Broadleaf herbs retain the existing
decimator, so some leaf and flower coverage differences remain.

The paired `normal-cut-markers/` and `finest-markers/` runs disable shadows and keep culling on;
`--lod 0.000001` selects near-finest detail. `--no-cull` is unsuitable: CSM and deformed scenes
force culling back on. Near `after-finest.png` views expose the curved blades' full outline.
`engine-image compare` JSON is beside the captures: mean FLIP 0.001411 / 0.000392 / 0.000064 at
5 / 20 / 60 m. Full-frame averages are dominated by the empty background; the fern-region
coverage check is the meaningful test of the defect that prompted the authored rung.

This conservative fern floor preserves foliage by spending far triangles. Candidate pairs rise
from 656 to 1,688 and peak shadow pairs from 385 to 2,016 in the before/after review, so the higher
pair count and cascade workload must be considered before adopting these forms at forest density.
The POC remains untouched; captures are for the owner's visual acceptance, not an assertion
that they have accepted the kinds.

## Regression evidence

Fourteen focused Windows cases pass 812,658 assertions: unchanged legacy understory geometry
pins, new mesh/motion pins, finite nondegenerate geometry, range validation, all-organ budget
floors, stable structural and motion rows across budgets, above-ground rosettes at their angle
limit, canonical schema round trips, material roles, 37 cooked section hashes for each new kind,
and nonempty 5/20/60 m distance forms with valid motion and material remaps. Eight far-cut cases
(3/16/24/40 requested pinna pairs, 500/4,000 triangle caps) retain every pinna on every motion part.

New opt-in mesh/motion pins: arching/dead fern `e6dc052f5488d866`, creeping fern
`c0f29746609a198b`, broad flowering/dead rosette `8303a099b06b535e`, budget-stress rosette
`08cb427a915115c2`. They are first pins, not replacements for accepted shapes. The authored
hierarchy and cache version 2 were pinned before the first implementation commit. No accepted
shape pin moved. Schema v2 and the dedicated fern/herb version change their container identities only.
The 80-frond floor is refused at 500 triangles; the 80-leaf/40-flower herb fits its 1,000 cap with
861 triangles and the same 122 motion parts as its 8,606-triangle fine form. There is no new hot
runtime type or size-table change.

Windows validation first ran `tools/dev.ps1 test -Preset msvc-debug -Affected`: 123 checks
passed, and `engine_view` was skipped at the shared GPU queue deadline. After the final LOD
change, all 39 checks in its generated affected closure passed in 4,426.53 seconds, including
`engine_view` (1,474.76 seconds) and `engine_view.frame_loop` (132.22 seconds). The driver runs
this saved `Get-AffectedTests` regex with `-Filter`; it does not accept `-Affected` and `-Filter`
together. The original run covers unchanged modules, and the follow-up covers every module
reachable from the final code delta. Its exact paths, baseline tree, filter and both logs are
preserved in the capture directory's `validation/` folder. No check remains queue-skipped.

Final Linux validation: all 19 selected Clang debug checks passed in 415.91 seconds of tests
(8m 28s including build). This reproduces the MSVC
mesh/motion and cooked-section pins. Lint and documentation checks pass; formatting was run.
