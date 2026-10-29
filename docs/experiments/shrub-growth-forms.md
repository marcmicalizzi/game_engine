# Shrub growth forms and thickets (G107)

2026-10-29. Extends flora with five named profiles without changing the old `shrub`
solver, its recipes, or tree output. Research was read before the implementation.
These are static architectural samples, not growth simulation or species identification meshes.

## Grounding

| Quantity | Source read | Value used | Budget trade |
|---|---|---|---|
| Beaked hazel (*Corylus cornuta* subsp. *cornuta*) height, leaf dimensions and ascending clonal habit | [USFS FEIS, Corylus cornuta](https://research.fs.usda.gov/feis/species-reviews/corcor), Botanical Description: 4–6 m; leaves 3–10 by 2–7.6 cm; clonal stems at about 0.6 m spacing in the cited Cloquet study. [UBC E-Flora](https://linnet.geog.ubc.ca/Atlas/Atlas.aspx?noTransfer=0&sciname=Corylus+cornuta) also describes 1–3 m plants | Reference 4 m; allowed 1–6 m; blades 6.3–7 by 4.5–5 cm; ascending multistem stool | 12 sampled stems, 7 laterals and 3 terminal shoots per lateral; counts are authored, not measured |
| Bearberry (*Arctostaphylos uva-ursi*) low spreading habit and leaves | [OregonFlora, Flora of Oregon](https://oregonflora.org/taxa/index.php?taxon=2907): prostrate, mat forming, usually below 3 dm; blades 1–2.5 by 0.5–1.5 cm. [Virginia Tech dendrology](https://dendro.cnre.vt.edu/dendrology/syllabus/factsheet.cfm?ID=421) corroborates mat form | Reference envelope 0.18 m, allowed 0.05–0.30; blades 1.8–2 by 0.9–1 cm | Low trailing axes and short ascending laterals; rooting contact is represented by a planting plane, no root mesh |
| Himalayan blackberry (*Rubus armeniacus*) cane form and foliage | [USFS FEIS](https://research.fs.usda.gov/feis/species-reviews/rubspp), Botanical Description and Fuels: canes generally 1–7 m long, arching then trailing; standing live and dead canes coexist. [Oregon State](https://landscapeplants.oregonstate.edu/plants/rubus-armeniacus): palmately compound, 3–5 leaflets of 4–8 cm, biennial stems | Reference 1.5 m arch, allowed 0.5–3.4 m; 2 m horizontal reach; 5.4–6 cm leaflets; 3 or 5 per palmate fan | The two foliage forms alternate by stem slot; they do not assign biological age (sources differ on which age bears which count). Prickles, flowers, fruit and explicit petioles omitted |
| Red-osier dogwood (*Cornus sericea*) thickets, opposite branches/leaves | [USFS FEIS](https://research.fs.usda.gov/feis/species-reviews/corser), Botanical Description: 1–6 m, rooting lower stems, opposite branches; [Alberta Agriculture](https://www1.agric.gov.ab.ca/%24department/deptdocs.nsf/all/agdex990): 2–3 m high, 2 m spread, opposite leaves 6–12 cm | 2.5 m reference; 1–6 m accepted; three colonies with centres within 0.75 m, overlapping 1 m stem reaches; blades 8.1–9 cm; paired scaffold stations and leaf nodes | Interweaving is overlapping curved stems, not collision-aware stem avoidance or a simulated clonal growth history |
| Big sagebrush (*Artemisia tridentata*) dry-site, silvery many-branched habit | [Oregon State](https://landscapeplants.oregonstate.edu/plants/artemisia-tridentata): up to 4.5 m, alternate sessile leaves 1–4 cm, sun and well-drained sites; [NPS Arches](https://www.nps.gov/arch/learn/nature/asteraceae_artemisia_tridentata.htm): height 0.4–3 m and dry shrub communities | 1.2 m reference, 0.4–4.5 m envelope; blades 2.25–2.5 cm; sparse terminal foliage at 45% node occupancy | Small generic blades stand for the leaf envelope; three-toothed tips and surface hairs need species materials or finer geometry |

Each source is descriptive evidence for a form and dimensional envelope, not evidence for every
control below. No species measurements of branch insertion, stem radii, individual crown LAI,
dead-stem fraction, or wind compliance were obtained. The attempted NRCS sagebrush dead-branch
PDF could not be fetched; its search summary is **not** used as a quantitative mortality source.
Bearberry's 0.05 m and blackberry's 0.5 m lower recipe limits are authored preview bounds,
not quoted botanical minima: the bearberry description gives an upper height, while FEIS's
cited blackberry stands are 0.8–3.4 m tall (mean 1.5 m). Blackberry, dogwood and sagebrush
blade widths are proportional authoring choices; the cited dimensions constrain their lengths.

## Explicit authoring and numerical choices

| Control | Hazel / bearberry / blackberry / dogwood / sagebrush | Reason and cost |
|---|---|---|
| Reference height, m | 4 / .18 / 1.5 / 2.5 / 1.2 | Within the source envelopes; not a mean |
| Horizontal stem reach, m | .8 / .65 / 2 / 1 / .65 | Shape approximation; multiplied by `spread` and height ratio |
| Reference stem radius, m | .022 / .003 / .006 / .012 / .018 | Authored scaffold sizes; H^(3/2) radius scaling inherits [elastic similarity](tree-allometry.md), not a species fit |
| Leaf length × width, m | .07×.05 / .02×.01 / .06×.04 / .09×.045 / .025×.006 | Seeded 0.9–1 size factor; widths are chosen within available descriptions or proportionally where no width measurement was found |
| Stems × colonies | 12×1 / 8×1 / 9×1 / 12×3 / 7×1 | Representative geometry density, not stems per square metre |
| Laterals × terminal shoots × leaf nodes | 7×3×8 / 6×3×6 / 8×2×4 / 8×3×8 / 5×3×8 | Two sampled branch orders; all remain across budget rungs |
| Terminal shoot length, m | .25 / .06 / .18 / .24 / .09 | Unmeasured authored seasonal-shoot proxies, independent of plant height |
| Dead basal fraction | .08 / .05 / .22 / .10 / .08 | Authored demonstration values; rounded to nearest stem, seeded ranks; zero and one supported. Small stands can round to zero |
| Default total triangle cap | 30,000 / 12,000 / 30,000 / 90,000 / 16,000 | Includes a reservation for the 32-triangle far form; owner-tunable |

All remaining numeric choices in `shrub_growth.cpp` are sampling, mesh or presentation controls:
16 intervals per structural axis; four meshing rungs with rows (16,8,3), (12,6,2), (8,4,2),
(4,2,1) and sides (6,4,3), (5,4,3), (4,3,3), (3,3,3). Radii of laterals are .32 of the stem,
terminal radii the smaller of .35 lateral radius and .006 twig length; tips are .18 of base.
This is a bounded sampled scaffold, **not the legacy solver's full pipe recursion**.
Stems vary from .65 to 1 of the reference; colony centres use a sunflower disk (radius .75 reach
unless explicit), stem feet a .18 m stool (.08 m for bearberry). Seeded heading jitter is .25 rad,
curve sideways amplitude .15 reach. Ascending stems use x=reach*t², y=height*t; arching canes
x=reach*t, y=height*(4t(1-t)+.1t)/1.050625; bearberry y=height*(.12t+.28sin(pi*t));
dogwood x=reach*(1.5t-.5t²), y=height*(1.8t-.8t²); sagebrush x=reach*(.6t+.4t²).
These curves encode the descriptive habits without claiming measured trajectories.

Laterals occupy .22–.92 of a stem, are .35–.60 reach long, rise .4 (.12 for bearberry),
and turn upward .25 rad. Terminals occupy .35–.95 of a lateral, depart ±.9 rad, rise .15
more, and turn .2 rad. Both wander .025 rad per structural interval. Leaves occupy .12–.94
of each terminal, turn by the golden angle (dogwood pairs by 90°), with .12 rad jitter,
forward bias .45 and sag .04 blade lengths. Blackberry leaflet fans span ±1.1 rad.
The frame's .95 dot-product guard avoids degenerate normal projections. Exact zeros/ones,
32 hash slots per shoot and distinct million-offset RNG streams are numerical conventions.

Linear RGBA leaf factors are (.12,.28,.06), (.08,.22,.06), (.08,.23,.05), (.10,.26,.055),
(.32,.37,.24); bark factors (.31,.23,.16), (.30,.12,.08), (.25,.18,.10), (.37,.065,.045),
(.29,.26,.21), alpha 1. Roughness .65 leaf/.85 bark. These are provisional flat colours,
not measured reflectance. Dead stems share bark material, have blunt tips and no leaves.
The inherited shrub compliance (.01,.07,.10,.14) is likewise an authored analogue.

## Validation and remaining limits

The specimen recipes and reproducible capture/compare scripts are under
`D:/workspace/game_engine_local/shrubs/2026-10-29/`, outside the repository. Each specimen has
its recipe, cooked container, scene and camera alongside normal and finest images at 5, 20,
60 and 160 m. These are 1600×900, noon, wind disabled, shadows off, in the branch's
`msvc-debug` viewer on an RTX 5090. They isolate morphology; they are not performance samples.
The finest comparison uses both geometric and foliage thresholds of 0.000001 pixels.

| Fixture | Source triangles | Motion parts | Container clusters | Container bytes | One-instance pose table bytes | Maximum whole-frame FLIP over the four views |
|---|---:|---:|---:|---:|---:|---:|
| Hazel, seed 17 | 17,018 | 349 | 525 | 1,848,820 | 84,240 | .00128719 |
| Bearberry, seed 23 | 9,600 | 201 | 314 | 1,348,344 | 48,720 | .000812735 |
| Blackberry, seed 31 | 13,314 | 226 | 429 | 1,421,636 | 54,720 | .00125745 |
| Dogwood, seed 41 | 69,168 | 1,189 | 2,236 | 6,953,280 | 285,840 | .00184921 |
| Sagebrush, seed 53 | 6,591 | 148 | 199 | 780,060 | 36,000 | .00347481 |
| Three blackberry colonies, seed 61 | 38,306 | 676 | 1,215 | 3,679,596 | 162,720 | .00223039 |
| Dead canes, seed 61 | 9,252 | 226 | 289 | 660,896 | 54,720 | .000355205 |
| Dry slope, seed 71 | 6,119 | 148 | 185 | 730,052 | 36,000 | .0031648 |

The container includes DAG levels and baked images, and is shared by instances; the pose table
is runtime memory. Source counts differ from the seed-1 unit test for stochastic leaf occupancy.
The largest picture difference is sagebrush at 5 m. All 32 whole-frame FLIP means are below
E10's .02 reference, but a distant shrub occupies little of a frame, so that is not evidence of
fine silhouette accuracy. The ordinary E10 folder driver accepts glTF, not flora recipes;
these comparisons use its `engine-image compare --flip` path on the two viewer captures.

The 160 m view does **not** prove the far handover: the inherited sliced impostor's parallax
error keeps it out until much farther away. `far.ps1` reads `engine-content info` and captures
at 1.5 times the nominal geometric switch distance (scaled from 1080 to 900 pixels), plus two
bounding radii to clear the fade band. Its census records one visible cluster in each normal
view, versus the following finest counts: hazel 287 at 368 m, bearberry 169 at 280 m,
blackberry 249 at 805 m, dogwood 1,357 at 558 m, sagebrush 102 at 251 m, the thicket 675 at
1,038 m and dry slope 86 at 241 m. Bare canes have no far form. These very distant images
are a handover check, not a useful close inspection; no intermediate branch-card tier is added.

Visual review: the five silhouettes differ, live and bare cane systems coexist, and the dry-slope
feet follow the planting plane. There is no ground mesh in these isolation views. Leaves remain
simple diamonds and blackberry fans look angular; sagebrush is deliberately sparse, with fine
twigs becoming subpixel. Bearberry samples an open spreading architecture with sparse coverage;
it does not fill a closed ground-cover mat. No photograph-level material fidelity or owner visual acceptance is
claimed. The vertical far form's overhead limitation remains as documented in the subsystem.

The new seed-1 combined geometry, motion and structural pin is `0x7bb6204d0b5e55c4` on MSVC
and Linux Clang 18;
the old shrub pin `0x011d85ebe0232489` is unchanged. The source-budget ladder preserves all
motion rows and leaf positions, while mixed mortality, actual blade lengths at both height limits, planting planes,
canonical source identity and container baking have executable checks.

### 64-instance cost sample

`benchmark.ps1` and `benchmark-csm.ps1` reproduce an 8×8 stand at 3.5 m spacing, with a
20–60 m camera path, 31 recorded frames, three repeats, at least 60 frames/two seconds of
warmup per repeat and `--wait-quiet 30`. The comparison keeps the camera, count and spacing;
the legacy shrub and new species have different geometry and coverage, so it is **not a
speedup experiment**. All use this branch's debug build, RTX 5090, 1600×900 and the GPU lock.

| Kind | Mean frame ms, traced / cascaded shadows | Mean GPU ms, traced / cascaded | Cascaded shadow GPU ms | Mean selected cascade pairs | Pose table bytes for 64 | Scene pair capacity |
|---|---:|---:|---:|---:|---:|---:|
| Legacy shrub, unchanged baseline | .661 / .642 | .548 / .394 | .137 | 13,995.7 | 522,720 | 55,424 |
| Beaked hazel | 2.030 / 1.958 | .312 / .252 | .049 | 168.2 | 5,361,120 | 33,600 |
| Bearberry | 1.311 / 1.412 | .258 / .204 | .030 | 105.2 | 3,087,840 | 20,096 |
| Blackberry | 1.325 / 1.507 | .294 / .277 | .075 | 151.2 | 3,471,840 | 27,456 |
| Red-osier dogwood | 4.710 / 4.692 | .298 / .259 | .057 | 113.0 | 18,263,520 | 143,104 |
| Big sagebrush | 1.035 / 1.079 | .267 / .208 | .031 | 103.7 | 2,273,760 | 12,736 |

**Machine state:** 11 of 12 samples report `quiet:false`; only the dogwood traced sample
reports `quiet:true`. Across their saved start/end `machine_state`, `cpu_others_pct` ranges
6.14–27.60 and `gpu_util_pct` 2–40; the GPU lock is the run's own. For example the baseline
traced sample records other CPU 10.80→20.88%, GPU 4→5%, and GPU memory 5,979→5,822 MiB.
Treat the busy timings as upper bounds, not isolated costs; all raw states are retained in
`benchmark-summary.json` and the per-kind JSONL files.

The principal added cost is **CPU posing and pose memory**, not this sample's GPU work:
each sampled shoot has its own part. Dogwood's 76,096 parts across 64 instances take a mean
4.32/4.45 ms to pose in traced/cascaded runs (that pose statistic includes warmup); legacy's
2,176 parts take .218/.220 ms. The caster policy still includes every cascade and tracing;
the counts above are the selected DAG cuts, not a reduced caster policy. These small isolated
stands do not replace the merge gate's frame-budget scenes or prove a dense forest budget.
No new runtime hot type or allocation path is introduced; `ShrubProfile` is cold metadata.
All 12 stand runs recorded zero steady-state engine allocations and zero device-buffer creations
in their `frame_loop` blocks.

Windows `msvc-debug`'s affected selection covered all 124 entries: 123 passed initially and
`engine_view` exhausted the shared GPU-wait deadline. The final focused flora run passed all
98 cases, including the height-bound regression. After three viewer deadline skips in total,
waiting for two competing viewer suites to exit allowed the viewer-only retry to pass
(981.28 s). Thus every selected entry has a passing run; no test failure was waived.
Format, lint and the documentation check passed. Logs are retained in the external `validation/`
folder beside the captures.

Linux `linux-clang-debug` passed all 14 selected entries (`flora|geometry|scene_gen|renderer|
engine_view|docs|lint`), including the cross-toolchain pin, followed by a successful focused
flora rerun with the added height-bound regression. Its GPU-dependent cases retain the
container's existing no-driver skips. The checkout's two Linux volumes were pruned afterward.

No seasonal growth, competition, physical branch interpenetration resolution, fruits, prickles,
root/rhizome geometry, species microdetail or dynamic mortality. The slope fields describe one
local planting plane, not a sampled terrain heightfield. The owner’s POC remains read-only.
