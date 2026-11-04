# Alpine mats and dwarf scrub (G125)

Research read on 2026-11-04, before the solver design. This extends the optional
[flora](../subsystems/flora.md) capability. It represents vegetative summer forms,
not growth through time. Existing species and their cache identities are retained.

| Quantity | Source and observation | Value used | Budget trade |
|---|---|---|---|
| Moss campion (*Silene acaulis*) stature and leaves | [Canadian Museum of Nature flora](https://nature.ca/aaflora/data/www/casiac.htm): height 2–5 cm; leaves usually 6–12 mm long and 1–1.3 mm wide; opposite leaves, extensively branched prostrate stems and compact erect shoots | 3 cm cushion relief; 9 × 1.2 mm leaves, each 0.9–1 of nominal; opposite pairs | Sampled shoots carry three pairs; hidden interior branching is omitted |
| Cushion spread | [E-Flora BC](https://linnet.geog.ubc.ca/Atlas/Atlas.aspx?sciname=Silene+acaulis), Illustrated Flora of BC: thick cushions up to 40 cm wide | 8 cm default radius; 4–20 cm authored radius range | 1536 shoots by default; increasing radius does not enlarge leaves, and may require more shoots |
| Encrusted saxifrage (*Saxifraga paniculata*) | [Native Plant Trust flora](https://gobotany.nativeplanttrust.org/species/saxifraga/paniculata/): alpine/subalpine ledges, stalkless leaves 5–50 mm; [Royal Botanic Gardens Victoria treatment](https://hortflora.rbg.vic.gov.au/taxon/ad94c1ac-5340-11e7-b82b-005056b0018f) search excerpt describes spoon-shaped leaves to about 3 cm and incurved margins; the latter page could not be opened | 25 × 8 mm outer blades, inner blades down to 35% of that size, in low rosettes; 7 rosettes, 18 leaves each | Width, rosette count and arrangement are authored, not measured population statistics; no teeth, lime pores or flowering scape |
| Snowbed willow (*Salix herbacea*) | [Canadian Museum of Nature flora](https://nature.ca/aaflora/data/www/wlsahe.htm): shoots 0.5–5 cm high; rhizomatous colonies; leaves 6–21 (exceptionally 34) mm long, 6–17 (31) mm wide; most branching below ground | 1.5 cm aerial shoots, 16 × 14 mm rounded blades; 3 leaves per shoot, alternate | 96 aerial shoots; each connects to the nearest earlier shoot in its pocket, an authored acyclic network approximating prostrate branching |
| Willow spread | [RHS](https://www.rhs.org.uk/plants/16253/salix-herbacea/details): low creeping mat, spread 0.1–0.5 m | 16 cm radius; adjustable 4–20 cm | Sparse occupied shoots expose the woody runners instead of scaling a tree crown |
| Rock-pocket planting | Native Plant Trust's ledge habitat above; no quantitative universal shelter threshold found | Explicit authored pocket centres and a local planting plane in each recipe; sparse saxifrage fixture uses one rosette per pocket | No automatic inference of soil or shelter from a bounding sphere; the scene author identifies actual soil pockets |
| Sampling and shape | Engineering choices constrained by the habits above, not field measurements | Golden-angle area sampling; seeded angular jitter ±0.15 radians; cushion dome sqrt(1-r²); rosette radial spread, leaf lift 0.1–0.9; rounded willow outline; spoon-shaped saxifrage outline | Foliage occupancy 0.05–1, shoot count 1–2048; all retained organs survive budget changes |
| Tessellation and source caps | Engineering budgets, owner adjustable | 4 then 2 blade intervals, 5 then 3 woody sides; default caps 32k campion / 4k saxifrage / 8k willow; six far triangles included | Refuse a cap below the coarsest representation; never enlarge or drop leaves to fit |
| Distance form | Existing flora CPU baker and cluster authored levels | Three orthogonal cards including a horizontal view, 128 px per tile, geometry-derived bounds/error and coverage fit | No camera-facing shader; subpixel detail and parallax are approximated, high-angle coverage requires review |
| Motion/collision/shadows | Existing low-cover policy; no measured alpine biomechanics found | Rigid `none`, zero compliance; no collision; untraced, nearest cascade, rest pose | Avoid hundreds of CPU part poses for centimetre-high mats; wind response is deliberately unmodelled |
| Surface | Authored appearance, not measured reflectance | Campion dark green, saxifrage grey-green, willow green; leaf roughness .7, wood .85; cup .06 of blade width | Separate bark/leaf material roles; leaves map a whole image, wood uses metres |

The recipe's slope is a local dy/dx, dy/dz plane, not a host-mesh intersection.
Pocket centres are relative to its anchor and may have individual elevation offsets.
Only planting roots are constrained to the pockets; foliage may overhang their rim.
Dense cushions, sparse pockets and snowbed colonies are different authoring choices;
none is a claim that every alpine plant requires shelter.

The remaining shape coefficients are authored tessellation choices, not additional
botanical measurements. For normalized blade position `t`, cushion width is
`1 - 0.6t`, saxifrage width is `0.15 + 0.85 sin(0.8 pi t)`, and willow width is
`0.12 + 0.88 sin(pi t)`; each terminates in one tip. Saxifrage advances along
`sin(pi t / 2)` and willow along `(1 - cos(pi t)) / 2`, keeping their tips blunt
with few vertices. Cushion leaf lift is 0.35 of length, with opposite pairs 2 mm
apart vertically; willow lift is 0.2, with leaves at 0.4/0.7/1 of shoot height.
Willow shoot radii taper from 0.8 to 0.3 mm and runners from 1 to 0.8 mm. Those
stem dimensions are visual approximations without measured diameter data; they
carry no mechanical interpretation. The 12 mm saxifrage profile height is
structural metadata: actual rosette height follows blade length and lift.

## Validation and measurements

Evidence is under `D:/workspace/game_engine_local/alpine/2026-11-04/`: text recipes,
camera paths, source containers, compiler/test logs, offscreen images and frame records.
The committed [specimen scene](../../content/test-scenes/alpine/README.md) reproduces
the views at metre scale. It has four plant patches and three granite boulders;
no plant instance is enlarged. The captures use 1280×720, CSM, noon, a fixed sun,
and the RTX 5090. These are author inspection images, not an owner's acceptance.

### Shape and distance checks

The first close views exposed an open cushion, pointed saxifrage leaves and radial,
spoke-like willow wood. The revised cushion has 1536 shoots and three opposite pairs
in an 8 cm root radius; the rosette's shorter inner leaves and blunt tips distinguish
it from an agave; willow's nearest-earlier-root connections replace radial spokes.
The sampling remains authored, not a botanical growth simulation. The rock-pocket
view shows plants between boulders, with some leaves occluded by their shelter.
The flat material colours expose silhouette; surface microdetail remains absent.

An ordinary decimated leaf DAG was unacceptable: the 5 m image had 185 green pixels
against 704 in the finest image, and 20 m had 12 against 48. This is a diagnostic
colour mask (`G > 1.08 R` and `G > 1.12 B`) over these flat-colour fixtures, not a
general segmentation metric. Whole-frame error would hide the loss because almost
all of the image is background. The final authored hierarchy retains all leaf
triangles until the fitted three-plane far form takes over; its top view preserves
the low habit. Version 2 invalidates the development containers with the destructive
generic leaf DAG. No established species' output or version was changed.

The default source and serialized-container costs are:

| Fixture | Near triangles | Parts | Total DAG clusters | Container bytes |
|---|---:|---:|---:|---:|
| Campion, seed 17 | 27,648 | 1,537 | 739 | 2,007,740 |
| Saxifrage, seed 23 | 882 | 8 | 19 | 124,892 |
| Willow, seed 31 | 5,841 | 97 | 185 | 527,860 |
| Three rock pockets, seed 41 | 378 | 4 | 9 | 70,228 |

Each source budget additionally reserves six far triangles. The dense cushion
uses the two-interval blade rung at its 32,000 cap. Containers include motion and
structure metadata, baked images and every resident level; they are shared by
instances. `none` motion adds no CPU part-pose run, and no hot record grew. The
authored hierarchy also reduces the cushion container from 4,742,652 to 2,007,740
bytes by omitting the intermediate decimated DAG. This is not a claim about an
unbounded field of cushions: these are rich detail patches with a measured specimen
scene, not a new automatic ground-cover placement system.

The default-seed distance-form audit uses `proj_scale=1080`, geometric threshold
1 px, foliage threshold 5 px and fade zero. Counts below are **resident triangles**:
the near frontier plus any coarser levels a more distant camera might select, not
triangles all drawn at once.

| Profile | 5 m | 20 m | 60 m | 240 m |
|---|---:|---:|---:|---:|
| Campion | 27,654 | 27,654 | 6 | 6 |
| Saxifrage | 888 | 6 | 6 | 6 |
| Willow | 2,050 | 2,050 | 6 | 6 |

The deterministic test folds all serialized section payloads, including images,
for seed 1, eight shoots and 64 px tiles. The MSVC values are
15342167867693253831 / 14353891800973444185 / 15090448681600777050 in the table's
species order; Clang 18 on Linux reproduced all three. The complete flora tests
passed on MSVC and all 18 selected Linux CTest targets passed, including flora,
geometry, renderer, content build, scene collision and the view/content apps.
The Linux compiler caught two implicit float-to-double promotions in the planting
normal; the conversions are now explicit, preserving the same output bytes.

Windows `msvc-debug test -Affected` selected 128 targets because the new scene
fixtures are global test inputs. It passed 127 targets and skipped `engine_view`
after a child process exhausted its GPU-lock wait; a viewer-only retry met the
same contention. After the three competing CTest runs ended, the deferred
viewer-only retry passed in 788.93 seconds, completing all 128 selected targets.
The separate frame-loop allocation target passed in the initial run. These
wall times include shared-machine waiting, not isolated rendering cost. Lint,
documentation and formatting checks passed; the Linux worktree volumes were
pruned. The normal merge gate remains the coordinator's responsibility.

After the hierarchy correction, the path's nominal 5/20/60/240 m captures count
708/38/3/0 green pixels versus the finest cut's 704/48/3/0. The 5 m loss is fixed;
the 20 m cut still under-covers by ten pixels, where some patches have reached their
far cards. At 60 m there are only three green pixels, and at 240 m none, so neither
is evidence of a detailed silhouette. Mean whole-frame FLIP is respectively
0.000332206 / 0.0000548259 / 0.00000926522 / 0.000000490425, including the rocks.
Those small means must be read alongside the foreground counts, not used alone as
acceptance. The colour probe and comparison JSON are saved beside the images.

`engine-content info` reports approximate foliage handovers at 1080 rows of
20.49 / 15.60 / 39.46 / 22.31 m for the fixture's campion / rosette / willow /
pockets (the foliage threshold is 5 px; the geometric thresholds are farther out).
At the 720-row capture resolution they are about 13.7 / 10.4 / 26.3 / 14.9 m.
The actual blend includes the group sphere and the fade band. Separate fixed-camera
`exact-*.png` images, with their `*-path.json` inputs, cover exact 5/20/60/240 m
and these approximate handovers; `final-captures/` holds the close views.

### Frame-cost sample and its limits

`capture-and-measure.ps1` runs the same close-view camera path over the three rocks
alone, then the rocks and four patches, at 1280×720, CSM, 60 sampled frames, three
repeats and 60 warm-up frames (plus the harness's minimum warm-up time). Both runs
hold the GPU lock and use `--wait-quiet 30`. This is **msvc-debug on the RTX 5090**,
not a release frame-budget gate; the merge runs that gate.

| Run | GPU median / p95 (ms) | CPU median (ms) | Resident pairs | Max visible / shadow pairs |
|---|---:|---:|---:|---:|
| Rocks only | 0.52885 / 0.62201 | 0.42765 | 324 | 90 / 368 |
| Rocks + alpine patches | 0.58080 / 0.742315 | 0.47385 | 1276 | 884 / 945 |

Both summaries report `quiet=false`: baseline `machine_state` CPU others
51.39% → 100%, GPU utilization 4% → 3%, GPU memory 6195 → 6242 MiB of 32607;
alpine CPU others 96.89% → 98.09%, GPU utilization 4% → 2%, memory 6179 → 6178 MiB.
The session was unlocked and the GPU lock belonged to this run throughout.
The observed 0.052 ms median GPU difference is **not an isolated solver cost**:
concurrent builds/tests make these shared-machine samples upper bounds, and the
different contention prevents attributing that small delta. Full states and frame
records remain in `baseline-frames.jsonl` and `alpine-frames.jsonl`.
