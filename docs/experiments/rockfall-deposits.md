# Rockfall and sediment deposition (G128)

Research read 2026-11-04. This is static content authoring, not a rockfall hazard model,
geomorphic time integration, sediment mass balance or a hydraulic transport solver.

| Quantity / rule | Source read | Value used / budget trade |
|---|---|---|
| Talus inclination | Francou and Mante (1990), [Analysis of the Segmentation in the Profile of Alpine Talus Slopes](https://horizon.documentation.ird.fr/exl-doc/pleins_textes/divers19-08/010048761.pdf), pp. 54-56: modal slopes 34-36 degrees under massive walls; profile break 33-34 degrees | Default runout-envelope angle 35 degrees, authored 25-40. This bounds a geometric envelope, not the final surface's every triangle. |
| Size sorting | De Blasio and Saeter (2015), [Dynamics of grains falling on a sloping granular medium](https://www.boa.unimib.it/handle/10281/453717), repository abstract | Small grains tend to stop near impact; larger grains can reach the slope break. Coarse representative sizes grow with source distance, with mixed small fragments everywhere; no universal distribution was available. |
| Source / connectivity | [Utah Geological Survey, Rockfalls](https://geology.utah.gov/hazards/landslides/rockfalls/) | Requires an upslope rock source and downslope route. 50-degree source threshold is an authoring choice distinguishing cliffs from talus, not a measured universal onset. |
| Drainage | [Esri, How Flow Direction works](https://doc.arcgis.com/en/arcgis-online/analyze/how-flow-direction-works.htm), D8 | Steepest descent among eight neighbours, height then index total ordering; catchment area in square metres. Flats and pits terminate flow; no invented uphill routes or pit filling. |
| Deposition in channels | [EPA, Sediments](https://www.epa.gov/caddis/sediments), reduced velocity increases settling | Low-gradient convergent drainage is a placement proxy. Default 24 square metre contributing area, 12-degree slope cutoff and 0.12 m depth are authored controls, not a flow-velocity inference. |
| Gravel sizes | [USGS water-data definitions](https://ca.water.usgs.gov/data/waterdata/text/definition.html), search-index text (direct page refused HTTP 403) | Gravel 2-64 mm diameter. Visible gravel representatives use 8-64 mm diameter; smaller grains are the continuous bed/material, never enlarged to save triangles. |
| Runout / thickness | No calibrated local rockfall inventory supplied | 40 m maximum travel and 3 m maximum talus depth; authoring bounds, not predicted travel or delivered volume. Zero depth disables either mechanism. |
| Geometry budget | Engineering choice | Samples 0.25-8 m, explicit maximum grid budget 262144 by default. Patches at most 32 m; 96 fragments and 32000 triangles per default patch. Refuse inadequate budgets. Minimum fragment rung 192 triangles; source/detail rungs use the existing scree solver. |
| Contact / numerical bounds | Engineering choice | Bed underside 0.05 m below sampled ground. Depth below 1 mm is omitted. Fragments buried 30%; support follows the sampled triangular surface. World millimetres and local f32 meshes. |

The finite region is a catchment boundary: no supply or drainage outside it is inferred. A
head-priority propagation of source envelopes gives connected downslope talus; the envelope
loses tan(repose) metres of height per metre travelled. Strictly flat cells can carry an
already seeded envelope laterally in the same head-priority propagation, but never uphill.
The channel field is D8 contributing area, with deposits only below the authored gradient cap.
No erosion, event frequency, bedload discharge, lithologic source map or seasonal forcing is claimed.

Validation includes eight deposit cases (53,676 assertions on MSVC), the 35-degree envelope on a flat foot, deterministic mesh/container pins, channel convergence, translated world coordinates, reverse tile order, strict budgets, contact and closed topology including sparse diagonal contacts. Existing version-10 rock pins are unchanged.

The default patch edge is 8 m, explicitly `DepositLayer.patch_m`, rather than spreading the
96 representative fragments over a 32 m tile. It must divide the host tile and contain whole
sample spacings (at most 64). Patch corners and normals remain local; split boundary walls do
not bend the bed's top normals. Source positions are still world millimetres.

Additional engineering choices, not measured grain-population statistics: fragments use 65%
small representatives, 27% intermediate and 8% coarse; their half-length bands are 0.004-0.032 m,
0.08-0.3 of `fragment_m`, and 0.4-1 of it. Coarse share scales the latter bands by 0.35-1;
runout share rises from 0.2 to 1. Representative horizontal radius is capped at 0.2 sample
spacing to stay within its support cell, with centres jittered through the central half-cell.
Height/width factors 0.35-0.7 and 0.6-1 retain angular, flattened shapes; normal gradients use
0.01 sample spacing. These are budget and support choices, not universal sediment laws.
World heights outside +/-1e6 m, patch-local heights outside +/-10000 m, invalid samples and
unrepresentable coordinates are refused before generating geometry.

Sparse beds can otherwise meet only at a corner and make a four-way vertical edge after welding.
The solver fills an ambiguous checkerboard corner with a buried zero-depth support cell, and
charges it to the surface budget. A regression checks the welded closed topology of this case.
The work queue revisits only the four corners of each newly filled cell.

## Review scene and measured budget

The committed `content/test-scenes/rockfall-deposits/` scene uses a 64 m window, 0.5 m
samples (16,641 grid points), 8 m patches, 256 fragments and a 64,000-triangle patch budget.
Its 46 nonempty deposit patches total 2,281,008 source triangles; each contains
49,204-50,304, so none exceeds its budget. The usual cluster DAG contains 117,065 deposit
clusters, 7,041,241 duplicated LOD vertices and 241,524,028 bytes of deposit containers
(5,206,506-5,352,670 bytes per patch). These are complete LOD containers, not just the near
mesh. A full 32 m tile can contain 16 patches: its explicit worst case at this recipe is
1,024,000 source triangles and at most 85,642,720 measured container bytes. Lower fragment
counts are an authoring choice, not an automatic budget escape.

Offscreen captures and exact commands/recipes are under
`D:/workspace/game_engine_local/rockfall/2026-11-04/final/`: `before/` retains the source cliffs,
`after/` adds deposits, and `finest/` uses a 0.05 px cut against the ordinary 1 px cut.
Five views cover approximately 5, 20, 60 m, the drainage hollow and the far view. Sun azimuth
-30 degrees, elevation 45 degrees; static terrain and sun; 1280x720, MSVC debug, RTX 5090,
Intel Core i9-10980XE (18 cores / 36 logical processors).
The images are a flat-material geometry review, not final material art.

| Counter, 241-frame path | Before | With deposits |
|---|---:|---:|
| CPU frame median / p95, ms | 0.2521 / 0.3896 | 0.2641 / 0.4847 |
| GPU frame median / p95, ms | 0.176352 / 0.191104 | 0.201408 / 0.231968 |
| GPU allocation used, MiB | 155 | 317 |
| Resident instance/cluster pairs | 4,738 | 121,803 |
| Peak visible pairs | 294 | 1,661 |
| Peak CSM shadow pairs | 641 | 7,702 |
| Steady-frame allocations / new device buffers | 0 / 0 | 0 / 0 |

**Shared-machine upper bounds, not isolated costs.** Both runs used `--wait-quiet 60` and
recorded `quiet: false`. Before `machine_state` start/end other CPU was 34.55/34.38%, GPU
utilization 3/2%; after it was 80.73/42.53%, GPU utilization 5/63%. Both owned the GPU lock,
session unlocked. The end GPU reading can include the run itself. Full records are
`before.jsonl`, `after.jsonl` and `measurement-summary.json`. These figures are not a claim
about a release build, other GPUs or a merge frame-budget gate.

Observed limits: finite-window edges remain visibly clipped, low-resolution D8 channels
follow the grid, and independent patch normals can show seams under flat materials. The
example's pre-existing generated cliff face also intersects its analytic ridge; that face's
solver and its accepted output were left unchanged. Fine sediment has no generated PBR maps.
This first version is a deterministic static bake with representative grains, not mass-balanced
sediment transport, a dynamic pile or a calibrated rockfall runout predictor.

The plan owns 24 bytes per sample (ground, depth, coarse share and drainage) plus its 96-byte
record; fixed solver scratch adds 32 bytes per sample, with an additional variable propagation
queue. At 16,641 points the sample arrays and fixed scratch total 931,896 bytes, excluding
allocator capacity and mesh building. This is a layout count, not a peak-RSS measurement.

Ordinary (1 px) against effectively finest (0.05 px), identical camera and sun:

| View | Mean FLIP | SSIM |
|---|---:|---:|
| 60 m | 0.0113251 | 0.985556 |
| 20 m | 0.0125680 | 0.991178 |
| 5 m | 0.0014560 | 0.997565 |
| Drainage | 0.0140371 | 0.983540 |
| Far | 0.00602472 | 0.992070 |

All five whole-scene means are below E10's 0.02 image-error reference. This is a direct
scene-marker comparison through the recipe registry, not a GLB E10 harness run or an
object-only coverage gate. `lod-comparison.json` and `flip-*.png` preserve the measurements
and heat maps. The flat material makes the coarse channel shape readable but does not supply
the fine-grain texture a finished scene needs.

Final Linux validation: `linux-clang-debug`, filtered to
`rocks|scene_gen|renderer|world|scene_collision|engine_content|engine_view|docs|lint`, passed
17/17 checks, including the MSVC byte pins. This container does not exercise a native GPU.
The checkout's two Linux volumes were pruned afterward (4.02 GB); shared caches were retained.
`dev.ps1 lint`, `dev.ps1 docs` and clang-format verification also passed.

Final Windows validation: `msvc-debug` `dev.ps1 test -Affected` completed with 127 CTest
checks passed and only `engine_view` skipped after a GPU-lock timeout (exit 75); there were
no failed checks. The first isolated viewer retry also timed out waiting for that lock.
After the original competing retries finished, the next isolated `engine_view` run passed
(exit 0): 164 cases and 47,201 assertions, zero failures, four doctest-marked skips, in
905.64 seconds. The final affected build also passed. The full affected log, skipped retry
and passing viewer log are preserved beside the captures. Shared-machine elapsed times
are validation durations, not performance measurements. The merge-wide release, ASan,
GPU-server and frame-budget gates were not run on this branch; those remain the merge's.

## G143: what a face sheds (2026-11-10)

Research read 2026-11-10 for roadmap G143. As for G128 this is static authoring, not a hazard
model: the sources say what shape a talus cone has and how far a block runs, and the numbers below
are the ones the solver pins, each beside where it came from. Where a number is an authored
choice and not a measured constant, the table says so.

| Quantity / rule | Source read | Value used / trade |
|---|---|---|
| The supply | Mohadjer et al. (2020), *Temporal variations in rockfall and rock-wall retreat rates in a deglaciated valley over the past 11 k.y.*, Geology 48(6): 594-598, doi:10.1130/G47092.1, abstract (Lauterbrunnen, 5.2 km² of calcareous cliffs): 0.03-0.08 mm a year from 5.2 years of laser scans, 0.27-0.38 mm a year from the volume of the postglacial talus over about 11,000 years; Holocene rates read from talus volumes elsewhere span 0.015 mm a year (Scotland, relict talus) to 0.33-1.96 mm a year (Svalbard) and 0.3-0.8 mm a year (Yosemite), all through secondary summaries | `supply_m` is the wall's retreat since the ice left: the default 3.5 m is Mohadjer et al.'s 0.27-0.38 mm a year over 11,000 years (3.0-4.2 m), mid-range; the field's 0-50 m covers the summaries' 0.015-2 mm a year over the Holocene (0.2-23 m). Talus volume is face area times retreat, the method those papers invert. |
| Where a cone stands | Scree, [Wikipedia](https://en.wikipedia.org/wiki/Scree) (secondary, read for its synthesis of Statham and Kirkby); the reference reels r04, r19, r22 (the owner's): pale cones whose apexes sit in the gullies of the wall above | Rockfall is funnelled through a gully, so its supply arrives at the gully's foot, and a cone stands there where the gully drains more face than the faces between it shed. Face area is routed down steepest descent to its foot and each foot's supply spread along the feet by a kernel that conserves it: a cone is that rule's limit at one foot, a sheet its limit along a planar face, and nothing switches between them. |
| The upper slope | Francou and Mante (1990), [Alpine talus segmentation](https://horizon.documentation.ird.fr/exl-doc/pleins_textes/divers19-08/010048761.pdf), pp. 54-56: modal 34-36 degrees under massive walls; the geoguide below: 35-37 degrees upper | The cone's upper half falls at `repose_deg`, default 35, never steeper. Angular blocks "seldom more than 45" ([LibreTexts, Southard, The Controls on Downslope Movement](https://geo.libretexts.org/Bookshelves/Geography_(Physical)/The_Environment_of_the_Earth's_Surface_(Southard)/10%3A_Mass_Wasting/10.02%3A_The_Controls_on_Downslope_Movement)): the range stays G128's 25-40. |
| The concave lower profile | [Scottish Geology Trust geoguide](https://geoguide.scottishgeologytrust.org/page/2138.pdf): straight 35-37 degrees above, 31-34 degrees in the lower part, concave; Kirkby and Statham (1975), J. Geol. 83, read through the [Scree](https://en.wikipedia.org/wiki/Scree) synthesis and [Bithell et al. (2014)](https://www.repository.cam.ac.uk/bitstream/1810/247016/1/Bithell_et_al-2014-Earth_Surface_Processes_and_Landforms.pdf): an exponential (Poisson) distribution of travel distance gives the straight-then-concave form, the fast particles running out as a tail | Below half the cone's nominal length the slope eases linearly over one more cone length to `toe_deg`, default 31 (the bottom of the 31-34 band), and holds it to the toe. |
| Fall sorting | [Scree](https://en.wikipedia.org/wiki/Scree) after Kirkby and Statham: large blocks "fail to find a depositional niche" and roll to the foot; the review in [Dai et al. (2022)](https://www.junyang.hku.hk/_files/ugd/31db54_3e032bbb5615470094da69f199d93480.pdf): larger clasts downslope | Block size grows with the distance travelled past the talus' top, `0.5 + d / (d + block_decay_m)` times the source's block size, and a cone's fragments coarsen from apex to toe as G128's tongues do. |
| Run-out of shed blocks | Evans and Hungr (1993), *The assessment of rockfall hazard at the base of talus slopes*, Can. Geotech. J. 30(4): 620-636, abstract: an empirical **minimum shadow angle of 27.5 degrees**, measured from the top of the talus slope to the distal limit; Wieczorek et al., [USGS OFR 98-467](https://pubs.usgs.gov/of/1998/ofr-98-0467) (Yosemite): the same method over 25 outliers gave 22 degrees | No block lies where the line from the top of its talus falls at less than `shadow_deg`, default 27.5, range 15-45. 22 is reachable for a valley like Yosemite's. |
| How the count falls | Kirkby and Statham's exponential travel distances, as above | Expected blocks per square metre `blocks_per_100m2 / 100 * exp(-d / block_decay_m)`, default decay 12 m: an authored length, not a measured one. |
| Block size from the source | Ruiz-Carulla, Corominas et al., [in situ and deposited block size distributions](https://link.springer.com/10.1007/978-3-319-59469-9_4) (abstract): block size is the product of the joints' orientation, spacing and persistence, and the deposit's size distribution differs from the in situ one | The mean block edge is the source's joint spacing, `block_m` (default 1 m, 0.1-6); a block's thickness is the type's plate factor of its width, and **never thicker than the bed it broke from** where the layer has a formation. |
| Burial by age | No source read gives a burial rate; the reels (r04: boulders "partly buried in the meadow", grass tight round their bases) | `buried_min`-`buried_max`, default 0.15-0.5 of the height, drawn per block as its age. An authored range. |
| Rest | Angle of repose as the friction angle of loose angular debris (Francou and Mante; the libretexts page) | No block stands on a top steeper than `repose_deg`: past it the block would not stop there. |

**What G128 already did at a gully's foot.** The G128 envelope is a union of descents at the
repose angle from every steep sample, and its depth is capped by `talus_depth_m` everywhere. It has
no notion of supply: a gully delivers no more than the planar face beside it. Run on a 16 m face
with three gullies (the unit test's ground, `deposits: G128's envelope makes an apron, not a cone`),
the talus' depth along the foot is the same under the gullies as between them, and its outline
follows the foot line: 2.69 m deep half a metre below the foot under the three gullies and 2.69 m
between them, reaching 4.5 m from the foot in both (MSVC, the test's MESSAGE). So **G128's tongues
are not cones**: they are an apron of even depth, a slab with a flat top at the depth cap, with
tongues only where the ground below already channels them. The before pictures below show it.

**What G143's first agent did with it** (superseded the same day; [rocks](../subsystems/rocks.md#what-a-face-sheds) has the rule that replaced it): a cone
at each gully's mouth from its catchment's supply, straight at 35 degrees over a concave toe at 31;
blocks shed out to the 27.5 degree shadow line, thinning exponentially, the largest farthest; and
the formation's bed at the face for their rock. Measured on the unit test's face (MSVC): three
cones, one a gully, apexes 5.87 m over their mouths and 4.5 m up their gullies, none on the planar
face; 134 shed blocks out to 9.7 m from their talus tops, 61, 59 and 14 by thirds of that distance,
their mean half-widths rising 0.56, 0.68, 0.79 m.

**Pictures** (not committed; `D:/workspace/game_engine_local/fable_sub/scratch/b59/G143/`):
`scene/make-heightfield.ps1` writes the same face with three gullies as a heightfield, and
`scene/scene-before.json` / `scene-after.json` dress it with deposits (no channel sediment, whose
blanket over this meadow hid the rest; after: `cone_supply_m` 2, 30 blocks a 100 m², `block_m` 0.9,
decay 10 m). `captures/final-before/` is the base build, `captures/final-after/` this one, each from the meadow, at a walker's height and from across the
valley, 1600 x 900, CSM shadows, sun -30, 40, msvc-debug. Before: a flat-topped grey apron of even
depth along the whole foot. After: a cone in each gully's mouth on the same apron, blocks on the
cones' toes and the apron and a few out on the meadow. Still wrong beside the reels: the bed's
outline steps with its 0.5 m samples, a patch's normals still show at its edge as G128's did, and
the blocks are clean. Against reels r04, r19 and r22 they still lack the cones' pale coarse surface, lichen and
turf on the blocks, and any change to the ground beneath (rocks.md, "What still reads as placed").

**The second agent's pictures** (not committed; `D:/workspace/game_engine_local/fable_sub/scratch/b59/G143b/`,
`captures/before/`, `after/`, `funnels/`; the base `606c072a`'s engine-view copied out of the tree
against this row's, one derived-data cache, the engine's material pack unpacked, msvc-debug, 1600 x
900, CSM shadows, sun -30, 40). The first agent's face at the default supply (3.5 m) stands a
continuous talus apron about 14 m up its 24 m wall, the gullies' piles bulging it a little; the
funnel face at 1.5 m stands the same apron lower, with the gullies' cones only slightly proud,
because its spurs still shed most of the face. The outline against the meadow no longer steps with
the samples, and a scatter of the toe's clasts lies past it. Still wrong beside reels r04, r19 and
r22: the deposit is the bed's flat grey (the pack's one rock set is sandstone, so a granite deposit
draws untextured) and smooth between its fragments, not coarse and paler than its wall; the toe's
clasts lie in a line along the outline rather than thinning out over metres; shadows and shading
streak across the apron near blocks and along some patch borders; and the walker's camera stands
inside the taller apron, so that view shows nothing.

## G143's third pass: the shading fault, the supply by setting and the rubble (2026-11-11)

Read 2026-11-11 for G143's third pass. Most primary papers on talus sorting are paywalled; what
was read in full and what only as an abstract or a summary is said in each row.

| Quantity / rule | Source read | Value used / trade |
|---|---|---|
| A buried face sheds nothing | Sanders (2012), *Talus accumulation*, Geo.Alp 9: 82-99, read in full (Fig. 1, Fig. 6): the Fisher (1866) / Lehmann (1933) feedback, "perhaps no other depositional system with such a rigid negative feedback"; the talus steepens to its residual shear angle (about 35 degrees on carbonate talus), then onlaps the cliff, whose area and supply decay | A pile sheds the supply of the face above its own top only, so the talus never buries its wall and buries a tall wall's smaller share (rocks.md, "How much a face sheds"). |
| The default supply | Hinchliffe and Ballantyne (1999), Scott. Geogr. J. 115: 53-70, abstract only: basalt cliff in Trotternish, 4.3-7.8 m of retreat (mean 5.6) since deglaciation at 17.5 cal ka, about 80% of it between 17.5 and 11.5 ka, about 0.1 m a ka now | 1.2 m, a temperate wall's Holocene share (20% of 5.6 m, or 0.1 mm a year over 11.5 ka). |
| The supply by setting | Mohadjer et al. (2020), above (alpine, calcareous, 3.0-4.2 m over 11 ka); Hinchliffe and Ballantyne (4.3-7.8 m since 17.5 ka); Sanders (2012): 7-18 mm a year of talus accumulation in two Holocene rock-avalanche scars, up to about 1 m a year just after the event; summaries of Draebing et al. (2024, EPSL; not read): 1.2-1.4 mm a year 9-10 ka ago against 0.02-0.08 now | A layer sets `supply_m` for its setting; the page gives the range by setting. Long-term talus-volume rates run 3-10 times the modern measured ones (Mohadjer, Draebing), so a rate measured today is not a supply. |
| Talus height against wall height | No field ratio from a primary source was found | The law with the feedback gives it: at 1.2 m a 10 m face is buried 42%, a 100 m face 15%; held by the test at both. |
| Fall sorting, its shape | Francou (1988/89), Biuletyn Peryglacjalny, read in full (French): fall sorting is "commonly observed", the sorting expected "logarithmic rather than linear" after Gardner (1970), Carniel and Scheidegger (1974) and Francou (1988); Verdian, Sklar, Riebe and Moore (2021), Earth Surf. Dynam. 9: 1073-1090, read (Sec. 2.3): "size-selective transport leads to downslope coarsening, which is commonly observed on angle-of-repose slopes"; median b-axis 10-575 mm across 15 sites, about 1.42 times the cliff's fracture spacing; counter-evidence, Wegner et al. (2021), NHESS 21: 1159-1177, read: no clear size-runout relation at one of four sites | The clast size of the bed's relief grows log-linearly with the coarse share (which grows with distance from the apex): a tenth of the toe's size at the apex. No measured apex-to-toe ratio was found; the tenth is authored. |
| The surface is openwork | Verdian et al. (2021): fines sampled "through openings between particles at the surface"; Hill et al. (2020), Geogr. Ann. A 102: 118-140 (boulder fans, introduction read): "boulders with openwork texture" | The bed's relief is flat-topped, steep-sided blocks standing on each other with voids between (rocks.md, "The bed is rubble"); the share of its size a clast stands above the bed (0.35) is authored, no source measured protrusion. |
| Paler than its wall | No source found stating it; Verdian et al.: their talus "has not undergone much weathering after deposition" | The bed samples a granular role (`rock/scree`) before its rock's; the tint is the pack's, not a solver's. |

**The shading fault was the LOD, measured on the CPU.** Two neighbouring patches of the gullied test
face, their cluster DAGs cut at errors of 0 to 1 m: before, the tops parted along the shared line at
156 places by up to 12.7 cm, and seven of eight cuts kept a different chain of border vertices on
the two sides; after locking the border, none, and every cut keeps the finest chain. Inside a patch
no cut sank more than 1.1 mm past its own error before or after, so the fault was at the edges only
(rocks.md, "The coarse forms keep the patch's border").

**The third pass's pictures** (not committed; `D:/workspace/game_engine_local/fable_sub/scratch/b59/G143c/`:
`scene/` is the second agent's funnel face, its supply 1.5 m and 128 fragments a patch, with a camera
path of five views: from the meadow, a walker at the toe outside the apron, one metre over the bed,
across the valley at 80 m and at 160 m; `captures/before/` the base `44c80a0a`'s engine-view copied
out of the tree, `before-noshadow/` and `before-finest/` its across views with `--shadows off` and
with `--lod 0.05`, `after/` this pass's, one derived-data cache; a copy of row G148's generated pack
as `--materials`, msvc-debug, 1600 x 900, CSM shadows, sun -30, 40). **The experiment**: from across,
the base's patchwork of panels and wedges is there with the shadows off and gone with the finest
forms forced, so it was the coarse forms, as the CPU case found; after the lock it is gone at the
default LOD. **Before**, at one metre the bed was the granite set's smooth face with lichen; **after**,
it is rubble, clasts standing on each other with dark voids, and the toe's clasts lie past it over
metres. The pass's first after-run stuck toe clasts to the face above the apron (`captures/after-first/`),
which the gentle-ground rule removed. Still wrong beside reels r04, r19 and r22: the apron is one
dark even grey (the generated scree set's tone, darker than the reels' pale fans, and its 1 m tile
repeats as a honeycomb from across); the clasts are all flat-topped plates of one shape; the cones
stand barely proud of the apron on this face, which is the face's (its spurs shed most of it), not
a scene where gullies drain basins; the wall above is untextured ground, so the scree's contrast
with its wall cannot be judged; and from across the toe's clasts still read as a speckled band
along the outline.

## G183 and G182: cones that climb and the bed at one metre (2026-11-11)

T108, G183 and G182 ([rocks: A stone wears its rock](../subsystems/rocks.md#a-stone-wears-its-rock),
[Cones that climb their gullies](../subsystems/rocks.md#cones-that-climb-their-gullies),
[The bed is rubble](../subsystems/rocks.md#the-bed-is-rubble)). The sources were found and read by
one research sub-agent over published pages; how each was read is its column. Several things were
looked for and not found, and they are listed so the next pass does not look again in the same
places.

| Quantity / rule | Source read | Value used / trade |
|---|---|---|
| Cones below chutes, aprons below planar cliffs, cones that merge | Sanders, Ostermann and Kramers (2009), Facies 55: 345-373, full text: cones form below cliffs "differentiated into chutes and gorges" that focus the supply as point sources; aprons below planar cliffs; cones "laterally merge"; gorges between summits filled by talus dipping up to 50-60 degrees | The supply rule already routes a gully's face to its mouth; the kernel that spreads a foot's supply along the wall is now a cone's (its neighbours' wedges fall at the repose along the wall), so a gully's pile is a cone and the apron between is the spurs'. |
| Talus slope range | Wegner et al. (2021), NHESS 21: 1159-1177, full text: mean cone slopes 29-36 degrees, straight or slightly convex above, concave at the base | The default repose (35 degrees) is inside it; unchanged. |
| The buried rock-head | Bithell, Richards and Bithell (2014), ESPL 39: 1601-1610, full text: the Fisher-Lehmann rock-head, talus height at the wall from volume balance | The feedback G143's third pass added is held on gullied walls of 10 m and 100 m: cones bury 45% and 20% of them. |
| Cone apex height against what the gully drains | Not found as a measured law (Siewert et al. 2012, ESPL 37: 1542-1555, abstract only, reference talus volumes to their source areas for retreat rates, 0.33-1.96 mm a year, and give no functional form) | The volume balance gives it: `h³ = 1.5 V tan(repose) / a` for a cone against a face; the apex height goes as the cube root of the gully's supply. |
| Cone planform half angle; relief of the upper margin; rockfall in gullies against spurs | Not found (searched: Luckman's Treatise chapter, Rapp 1960, Church et al. 1979 abstract, Statham 1976 abstract) | The planform follows from the union of the piles' surfaces; the margin's relief is measured on the test walls and bounded by a design value (the apron at most four fifths of the cones), not a source's. |
| Clast shape on talus | Wegner et al. (2021): blocks over 0.5 m, median a/b axis ratio 1.73 (basalt) to 2.63 (gneiss); Sanders et al. (2009): angular to subangular clasts, openwork, "open megapores" below larger platy clasts, an indistinct preferred orientation | The bed's clasts (G182): elongated 1-2.5 to 1 in plan, angular, tilted at random about the slope, of several heights, with the bed between the largest sunk into hollows. |
| Clast protrusion, tilt angles, void depth, Zingg classes on talus | Not found (Bertran et al. 1997, Sedimentology 44, abstract only: individual particles give random orientation) | Authored and stated on the page. |

## G190: where a pile comes to rest (2026-11-12)

G190 ([rocks: Where a pile comes to rest](../subsystems/rocks.md#where-a-pile-comes-to-rest)), for the
forest proof of concept's gaps 41 and 42 (its asks audit and its deposits part, read as text). The
sources were read by the row's agent through a web search's summaries of them, not their full text,
and the column says so; no research sub-agent was run.

| Quantity / rule | Source read | Value used / trade |
|---|---|---|
| Source, transit and deposition zones | Dorren (2003), *A review of rockfall mechanics and modelling approaches*, Progress in Physical Geography 27(1): 69-87 (a search's summary of a secondary copy: free fall over slopes steeper than about 76 degrees, a bouncing rock turning to rolling as the mean slope falls below about 45, most of the energy lost at the first impact); the forest rockfall textbook's source, transition and deposition areas (search summary) | A face's fall is carried through every sample steeper than the deposit can stand on, and stands at the first one gentler: the transit zone carries, it does not hold. |
| The ground a pile can stand on | Francou and Mante (1990), the geoguide (as G143 read them): straight upper talus at 34-37 degrees over a concave base at 31-34; Statham (1976), *A scree slope rockfall model*, Earth Surface Processes 1: 43-62 (search summary: the dynamic friction between a moving clast and the debris already lying controls where it stops) | A pile is a surface at `repose_deg` easing to `toe_deg`, so it meets only ground gentler than the toe: **a sample steeper than `toe_deg` (31) carries**. The brief asked for "gentler than the repose"; at the repose itself the toe that eases below the ground's slope rises off it and the pile filled the slope as a wedge, six times what it shed (the base's 35-degree case), so the toe is the bound. |
| Where blocks that pass the talus stop | Evans and Hungr (1993), as G143 read it (minimum shadow angle 27.5 degrees from the talus' top) | Unchanged: the shed blocks run from the top of the talus along its surface to the shadow line. |
| The cut bound | No published step; the bed's own clasts (rocks.md, "The bed is rubble") | `k_cut_m` 0.22 m: the lowest the coarsest drawn clast stands (0.2 of 0.55 of a 2 m cell), a step the toe's clasts hide. Run once against the base (33ff2824): the 42-degree case's outline stood 7.43 m over the bare ground past it, past the bound. |
| A pile on a bench at a brink | Not found as a rule; Sanders et al. (2009)'s "gorges filled by talus" are the opposite case (talus that does fill a steep notch, from a chute above it) | Authored: a pile whose deposit covers ground steeper than the toe lower than its own foot by more than its height drapes the drop, and is moved over the brink to the ground below. |

### What G190 does on the tests' faces

`deposits_rest_tests.cpp`, msvc-debug: a 20 m face at 63 degrees over 12 m of drop to a meadow at
35 to 60 degrees, three seeds each (the seeds move only the edge's noise). The outline step is a
talus sample's top over a bare neighbour's ground less what a surface at the repose falls between
them: at most 0 where the surface meets the ground. The base column is the same case run once
against 33ff2824's solver.

| Slope below the face | Base: outline step, held of asked | G190: outline step, held of asked |
|---|---|---|
| 35 degrees | -0.05 m, 3,502 of 589 m³ (the wedge filled the slope) | -0.01 m, 240 of 650 m³ |
| 42 degrees | **7.43 m**, 3,977 of 587 m³ | -0.02 m, 868 of 867 m³ |
| 50 degrees | -0.08 m, 3,698 of 686 m³ | -0.02 m, 950 of 993 m³ |
| 54 degrees | (not run on the base) | -0.02 m, 986 of 977 m³ |
| 60 degrees | (not run on the base) | -0.02 m, 1,053 of 1,022 m³ |

Every foot was on ground steeper than the repose on the base, none is now; the worst cut of a kept
pile is under a millimetre. The forest's second world measured its beds on 54-degree ground at the
median (20% of the outline dropping over 5 m); here the median ground under the bed is the transit
slope's own below 54 degrees, where the wedge leans up it and meets it, and the meadow's (4.6
degrees) from 54 up, where the transit slope is itself a source. A 2 m (4 m) bench at the brink of a
45-degree, 10 m drop: all 65 (59) piles moved over the brink, none refused, no cut, holding 40%
(57%) of what they were asked. A transit slope that runs on past the region's edge: all 65 falls
refused, 863 m³ at 42 degrees and 1,621 at 50, and nothing stood. A 3 m channel with 56-degree
banks down a 20-degree hillside: 312 piles shed from its banks read on the cut ground alone, none
read against the ground before the cut.

### What a kilometre of deposits costs

G191 and T120, `deposits: what a kilometre of massif foot costs at load and a tile (measured)`,
skipped in the suite and run by hand: a 1,024 m by 192 m window at 1 m of a 60 m face over a skirt
grading through 50 to 35 degrees, 16 m patches, 64 m tiles; `msvc-release`, 2026-11-12 14:11-14:18,
the CPU at 52-73% with other agents' builds and GPU runs, 52-60 GB of free commit, so every time is
an upper bound, one run each. "Before" is what a host did at load until G191: every patch built and
held.

| | 128,000 triangles a patch (the forest's) | 32,000 (the default's density) |
|---|---:|---:|
| Patches, over tiles | 466 over 63 | 466 over 63 |
| Built and held at load before G191 | 466 meshes, 36.6 M triangles, 2.93 GB of containers, 249 s of work single-threaded | 466, 12.1 M, 0.99 GB, 79 s |
| Built at load now | none; recipes written: none (before: 466 files, 13.6 MB) | none |
| `open` (plan, patches checked) | 316 ms | 289 ms |
| The second open (T120; before, the plan again) | 43 ms | 37 ms |
| A 64 m tile's activation (its patches built), median and max | 3.9 s, 7.8 s | 1.4 s, 2.4 s |
| The largest patch | 3,934 clusters | 1,558 clusters |
| **Since G191's pieces** (2026-11-13 01:20, `msvc-release` at 8cd633a6, CPU 63-73% busy, one run) | | |
| Meshes handed for the 466 patches | 1,476 | 468 |
| Built, every mesh | 36.7 M triangles, 3.00 GB, 263 s of work single-threaded | 12.1 M, 0.99 GB, 78 s |
| The largest mesh | 2,442 clusters | 1,558 clusters |
| Built again at half its budget | none | none |
| One mesh's build, median and max | 169 ms, 526 ms | 172 ms, 339 ms |
| A 64 m tile's activation (its meshes built one after another), median and max | 4.1 s, 9.2 s | 1.3 s, 2.3 s |

The host builds a tile's meshes off the frame's thread and in parallel, so a tile's activation is
its patches' build spread over the pool, not a stall; at the forest's budget it is still four
seconds of work a 64 m tile, and the default's density costs a third of it. Before G191 the same
466 builds ran at load whatever the camera (the forest measured 40 s for 724 on twenty threads).
Resident for the entry: the plan, 29 bytes a sample (5.7 MB for this window), its blocks and 72 bytes
a candidate patch.

### What a walker sees in a streamed host

G191's review, 2026-11-13: `engine-view --scene <scene> --world --camera-path walk.json --windowed
--width 1600 --height 900 --ddc <cold root> --benchmark run.jsonl --world-log world.jsonl`,
`msvc-release` at 8cd633a6, RTX 5090. The scene is a kilometre of massif foot (a 60 m face over a
skirt grading through 50 to 35 degrees), a deposits layer 100 m by 960 m at 1 m samples, 16 m
patches at the forest's 128,000 triangles, 96 loose stones; 64 m tiles, rings of 1.5, 4 and 8
tiles; the walk 300 m along the foot at 2 m/s and eye height, 150 s in real time. The machine was
shared: other processes held 34 to 99.5% of the CPU across the runs, so every time is an upper
bound. An offscreen `--benchmark` flight of the same path is no measure of this: it draws its 4,500
frames in 2.5 s, faster than one tile's meshes build, and draws no deposit after its first fill.

| | `far_rings` 3, the default mesh room (256 MiB) | `far_rings` 3, a 2 GiB room | `far_rings` 1, the default room |
|---|---:|---:|---:|
| The first frame (the ring filled with no budget, every build waited for on the frame's thread) | 63.9 s | 63.8 s | 16.4 s |
| Deposits' meshes drawn after the first fill | 88 instances, then none from 66 s to the end | 541, 601 from 70 s to the end | 17 to 97, two tiles arriving every 32 s |
| Withheld | 32 tiles (105 meshes, 250.5 MiB resident) | none | none |
| A tile arriving in view | never | never (the 512 m outer ring holds the whole region from every point of the walk) | +3,233 visible pairs in one frame at 43.5 s, the tile's near edge about 63 m ahead |
| Frame times over the walk after the first frame, median / p95 / max | 12.2 / 23.2 / 506 ms (the update after the first fill) | 12.2 / 23.0 / 77.8 ms | 12.2 / 24.0 / 74.8 ms, the same within 2 s of an arrival |

What the default room does with `far_rings` 3: the first fill admits what fits, the ring moves, and
the nearest waiting tile asks for room; the room yields the farther drawn tiles one by one (the
tail falls 88, 80, 48, 40, 8, 0 over five updates) and still has no run for it. By a reading of the code
(not traced): what the room still holds is named by tiles that are waiting themselves, partly
admitted, and `PlacementTiles::yield_farthest` (`systems/world/src/placement_tiles.cpp`) takes only
drawn tiles, so nothing frees it. Every tile then waits for the rest of the walk. Nothing
in the deposits decides this; at the forest's density a 64 m tile of the foot is some 40 meshes and
a third of the room.

Not found, and looked for: a published runout of talus below the base of a transit slope; a law for
the volume a pile at the foot of a slope at its own repose holds (the wedge law gives zero section,
and the model under-holds there, 37% of the supply at 35 degrees; rocks.md says so).