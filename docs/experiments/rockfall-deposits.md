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