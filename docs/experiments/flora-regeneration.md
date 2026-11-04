# Regeneration and damaged young growth (G126)

Research checked 2026-11-04. This is a static juvenile shoot model inside flora, not
a temporal growth, light-transport or herbivore simulation. Existing recipes keep their
accepted output. A separate optional `regeneration` block selects this model.

## Grounding

| Quantity or rule | Read source | Value used | Budget trade |
|---|---|---|---|
| Sugar maple suppression and terminal loss | [USFS Silvics](https://research.fs.usda.gov/silvics/sugar-maple), early growth and vegetative reproduction: prolonged suppression; five-year shade-grown seedlings about 1.27 m; opposite branching predisposes terminal-loss forks | Opposite, decussate nodes; two replacement shoots below a lost leader. Height is authored, not inferred from age. | A few explicit shoot cohorts rather than annual bud histories |
| White oak advance regeneration | [USFS Silvics](https://research.fs.usda.gov/silvics/white-oak), seedling development: repeated dieback/resprouting; root age can exceed shoot age substantially | Alternate leaves; replacement growth low on the surviving stem. No root-age estimate. | One visible recovery episode |
| White pine leader damage | [UNH Extension field manual](https://extension.unh.edu/resource/field-manual-managing-eastern-white-pine-health-new-england), white pine weevil: laterals below a killed leader turn upwards, often competing | Whorled lateral shoots; replacement leaders originate below the break, no basal coppice | Two representative competing leaders; count is authored |
| Spruce suppression | [Growth dynamics of the Norway spruce and silver fir understory in continuous cover forestry (2020)](https://iforest.sisef.org/contents/?id=ifor3183-012): apical dominance ratio falls with light deficiency | Suppression shortens leader extension more than lateral reach | An authored static blend, not a fitted response curve |
| Silver birch regeneration | [Forest Research](https://www.forestresearch.gov.uk/tools-and-resources/tree-species-database/131561-silver-birch-sbi-2/): light-demanding pioneer, browsing affects stem form, natural regeneration can exceed 10,000 seedlings/ha | Slender alternate shoots; groups are authored finite samples | No claim that a group density predicts recruitment |
| Shrub forms | [Beaked hazel FEIS](https://www.fs.usda.gov/database/feis/plants/shrub/corcor/all.html), [red-osier dogwood FEIS](https://www.fs.usda.gov/database/feis/plants/shrub/corser/all.html); existing [shrub grounding](shrub-growth-forms.md) | Hazel ascending basal shoots and alternate leaves; dogwood multiple shoots and opposite leaves | Three basal shoots per young shrub, an authored sample |
| Leaf dimensions | Existing [profile audit](tree-profile-audit.md), [birch](birch.md), [shrub grounding](shrub-growth-forms.md) | Maple 10 x 8 cm; oak 12 x 6 cm; birch 5 x 3.5 cm; hazel 7 x 5 cm; dogwood 9 x 4.5 cm | Fixed blades, never enlarged to meet a budget |
| Pine needles | [Oregon State University](https://landscapeplants.oregonstate.edu/plants/pinus-strobus) (five per fascicle, 5–13 cm); [Scion factsheet](https://nzfri.scionresearch.com/Content/Projects/nzfri/keys/cultivated-pines/key/cultivated_pines/Media/Html/Pinus_strobus.htm) (6–14 cm, 0.7–1 mm wide) | 8 cm x 0.9 mm reference needles, five per fascicle | Flat two-triangle needles; sampled fascicles, no cotyledons |
| Spruce needles | [Waldwissen/WSL](https://www.waldwissen.net/de/lebensraum-wald/baeume-und-waldpflanzen/nadelbaeume/die-fichte-picea-abies): spiral needles 10–25 mm | 20 mm reference length; 1 mm width is an authored approximation | Flat two-triangle needles; sampled density, not a fitted leaf-area model |

## Explicit authoring approximations

No measured universal node count, stem slenderness, recovery length, browse probability,
or obstacle-response coefficient was found in the read sources. They must not be described
as botanical constants. The following table records the solver's authored controls.
Obstacle avoidance is a geometric constraint on authored
local obstacles; it does not imply that a plant senses a rock at a distance. Suppression
describes an already suppressed form, independently of leaf size and damage.

| Profile | Basal radius / reference height | Lateral reach / suppressed height | Branch angle from parent | Branches/node | Basal stems | Source cap |
|---|---:|---:|---:|---:|---:|---:|
| Maple | 0.007 | 0.27 | 48 degrees | 2 | 1 | 12,000 |
| Oak | 0.010 | 0.32 | 55 degrees | 1 | 1 | 12,000 |
| Birch | 0.005 | 0.23 | 38 degrees | 1 | 1 | 12,000 |
| Pine | 0.008 | 0.30 | 65 degrees | 4 | 1 | 18,000 |
| Spruce | 0.008 | 0.28 | 80 degrees | 5 | 1 | 18,000 |
| Beaked hazel | 0.009 | 0.32 | 40 degrees | 1 | 3 | 24,000 |
| Red-osier dogwood | 0.007 | 0.34 | 48 degrees | 2 | 3 | 24,000 |

Suppression multiplies height by `1 - 0.55*s`; lateral reach also gets `1 + 0.5*s`,
and 35% of leaf nodes are omitted at maximum suppression. These controls describe a form;
they are not a quantitative light-response curve, and do not imply that light-demanding birch
survives prolonged suppression like maple. Group height varies from 75% to 100% of reference
height. Stem radius has a 1 mm floor; lateral radii start at 32% of the parent with taper by
attachment height. Lateral cohorts start at 45 cm, with `2 + floor(height_m)` stations through
the middle 60% of the stem above its basal quarter. This finite cohort count stands in for
branching history. Direct leaves have `3 + floor(suppressed_height*2)` nodes, lateral shoots
`3 + floor(reach*8)`. Leaf size varies 90–100% of the table dimensions, with 4% tip droop.
Pine fascicles use `max(4,floor(axis_length*25))` nodes, five needles per node with a 0.12-radian
fan, and spruce uses `max(8,floor(axis_length*200))` single spiral needles. These densities are
authored samples, not measurements of total leaf area. Needle dimensions do not depend on plant
height. The first untextured shoot-card captures looked like broad leaves; explicit narrow
needles replaced those cards before the first implementation commit.

Selected browsed laterals retain 45% of their original length. Broken tips retain 55% of
their base radius; intact tips taper to 15%. Replacement shoots have 35% of the basal radius,
four leaf nodes, and extent `suppressed_height * max(0.25,leader_loss) * recovery`. They start
at 8% of surviving stem length for basal recovery, otherwise 85%. Two representatives are
used for opposite/evergreen profiles, one for alternate broadleaves. No measured universal
replacement count or healing time is claimed. Shrub stems shorten by 12% per stem and fan
out with a 0.28 horizontal/up ratio; tree main axes use 0.04. Dogwood stems are red, the
others brown; the default materials are flat swatches pending artist material bindings.

The minimum resolved shoot length is 1 mm. A smaller positive recovery is an error, not a
silently omitted shoot: a subnormal recovery fraction can otherwise round every axis station
to the attachment point. The regression case checks refusal before any mesh/motion output.

Every axis has 16 intervals and transports its frame along its solved path. Desired direction
blends 35% toward up along the axis and 3% seeded lateral wander, then 60% of it with 40%
of the preceding direction. Obstacles influence direction within radius plus stem radius plus
three steps. If the candidate collides, sixteen golden-angle directions with vertical component
0.25 are tried. The full swept step must clear every sphere by stem radius plus 1 mm; failure
is explicit. A leaf is enclosed in a capsule along its undrooped centreline, with radius
half its width plus 4% length for droop. The initial length-plus-width sphere stripped every
leaf from the 40 cm obstacle specimen; the capsule keeps blades that fit. It makes no claim about tropism, loading,
root contact, collision after wind deformation, or obstacle detection by a real plant.

The budget reduces only tube sides (8, 6, 4), reserving 32 triangles for the optional far star;
flat broken tips use two fewer triangles than the conservative reservation per shoot. Neither
branches nor leaves are enlarged or deleted by the budget. An impossible cap is refused.
These controls and limits are also in the [subsystem contract](../subsystems/flora.md#regeneration-and-damaged-young-growth).

## Validation and measurements

The twelve committed specimens have the following source/storage counts. These are geometry
and serialized byte counts, independent of machine load; no time claim is derived from them.
All use their profile's default cap and a 128-pixel far tile. Add 32 far-star triangles to the
near count; every specimen, including the obstacle after the capsule correction, has a far star.

| Specimen | Near triangles | Motion/structural parts | Cooked bytes |
|---|---:|---:|---:|
| Maple, 0.8 m | 1,392 | 6 | 185,944 |
| Oak, 0.8 m | 816 | 4 | 121,992 |
| Birch, 0.8 m | 814 | 4 | 112,576 |
| Pine, 0.8 m | 2,896 | 10 | 276,920 |
| Spruce, 0.8 m | 3,794 | 12 | 322,608 |
| Beaked hazel, 0.8 m | 2,444 | 10 | 252,940 |
| Dogwood, 0.8 m | 4,192 | 16 | 393,292 |
| Nine maple seedlings | 2,484 | 10 | 236,984 |
| Suppressed spruce | 7,340 | 22 | 566,140 |
| Broken/recovering pine | 4,362 | 12 | 370,484 |
| Browsed/recovering hazel | 4,030 | 16 | 343,116 |
| Oak around obstacle | 266 | 2 | 99,296 |

The new cases cover all seven profiles
from 0.15 to 4 m, parent-first parts, nonzero-area triangles, leaf/needle dimensions, exact
collision eligibility, damage, budget refusal, obstacle clearance and retained foliage,
canonical round trips, external-image invalidation, every serialized section and extracted
forms at 5/20/60/200 m. The seven new section pins are folded in section order. Existing
species' pins are not edited.

The new blade-aspect check initially used a 0.02% relative tolerance for every profile.
Subtracting float vertex positions to measure millimetre-wide needles at metre-scale
coordinates exceeded that tolerance. Its evergreen tolerance is 0.3%; broadleaves retain
0.02%. This accommodates coordinate precision, not different needle dimensions. No existing
test or mature-species pin was relaxed.

The Linux warning policy requires explicit recipe-float to solver-double conversions. Keep
the single-precision intermediate in `height_m * radius_per_height` and `1 - leader_loss`
before widening it: hoisting those conversions would change the already pinned MSVC geometry.

Captures, recipes, cameras, cook reports and reproduction scripts live in
`D:/workspace/game_engine_local/flora-regeneration/2026-11-04/`. The single-mesh views are
1280 x 720 at 5/20/60 m (also 200 m for the seven base profiles), sun azimuth/elevation
135/45 degrees, CSM, frozen time. Close views supplement these. `specimens.png` frames
the committed scene. `capture.ps1` reproduces the full set; `refresh.ps1` recooks the five
specimens changed by visual review. Both force the cook: an unchanged version number during
local development otherwise reuses the earlier draft's container even after recompilation.
The scene capture also supplies the camera explicitly and disables the draft cache.

Visual limits: default broadleaves remain untextured diamond silhouettes; needle ribbons and
millimetre stems are faint at several metres and subpixel at long distances. These views test
geometry and LOD continuity, not a finished botanical material treatment. An obstacle can
still exclude most leaf nodes, and the far representation approximates its silhouette without
knowing the sphere. No owner visual acceptance is claimed. No accepted adult kind changes.

### Recorded specimen comparison

MSVC debug, RTX 5090, 1280 x 720, CSM, fixed sun 135/45 degrees, the committed camera supplied
explicitly. `measure.ps1` keeps the same twelve meshes resident in both runs: `baseline.scene.json`
translates their instances 10 km out of view, and `visible.scene.json` puts them back. Each has
60 sampled frames, three repeats and at least one second of warmup per repeat, under the GPU
lock with `--wait-quiet 5`. This is a small specimen comparison, not a production forest budget
or a release-build gate. The merge owns the frame-budget gate.

| Quantity | Off-frustum baseline | Visible specimens |
|---|---:|---:|
| GPU total median / p99 | 0.128896 / 0.140450 ms | 0.160960 / 0.169130 ms |
| Shadow pass median | 0.026432 ms | 0.025600 ms |
| Visible cluster-instance pairs | 0 | 119 |
| Shadow pairs | 48 | 115 |
| Resident meshes / instances / clusters | 12 / 12 / 941 | 12 / 12 / 941 |
| Flora parts / pose-table bytes | 124 / 30,240 | 124 / 30,240 |
| Texture bytes | 25,165,824 | 25,165,824 |
| Steady-frame engine allocations / buffers | 0 / 0 | 0 / 0 |

**Machine state:** both runs were busy. Baseline `cpu_others_pct` was 100.0 at both samples,
`gpu_util_pct` 2 -> 3 and `gpu_memory_used_mib` 6239 -> 6285. Visible-run CPU was
93.75 -> 99.8972%, GPU 3 -> 2%, memory 6262 -> 6385 MiB. Both samples say the GPU lock was
ours and the session was unlocked. The observed GPU median difference is 0.032064 ms; the
busy-machine figures are upper bounds, not an isolated per-plant cost. CPU pose means were
0.22175 / 0.17071 ms with maxima 45.04 / 55.35 ms, so neither is a useful intrinsic solver
or animation cost on this loaded machine. Full frame records and machine-state objects are in
`baseline.bench.jsonl` / `visible.bench.jsonl` and their logs beside the captures.

The 24 MiB of atlas textures belongs to twelve prototypes and is shared by their instances;
it is not 24 MiB per plant. This first version does not pack different juvenile species into
one atlas. A group shares one star; `impostor_px:0` opts out where no far form is needed.
The prototype tables and existing motion buffers are the only runtime storage added by these
instances; the solver itself stays at cook time. There is no new tile-generation state to measure.

Linux `linux-clang-debug` passes all 12 selected checks (`flora|scene_gen|renderer|engine_view|docs|lint`),
including the seven MSVC section pins and the unchanged mature-species pins. Its first build
exposed implicit float-to-double promotions; making the conversions explicit, with the original
intermediates preserved as described above, fixed the warnings without changing geometry.
The checkout's two Linux volumes were pruned after the successful run.

Windows `tools/dev.ps1 test -Preset msvc-debug -Affected` selected 128 checks because the
committed recipe fixtures reach every consumer. Its first run passed 126, failed none, and
skipped `renderer` and `engine_view` when their shared-GPU lock deadlines expired (exit 75;
8,482.18 seconds overall). A targeted retry of `^(renderer|engine_view)$` passed both:
947.41 and 1,903.01 seconds, respectively, 2,850.63 seconds overall, exit 0. Thus every selected
check has a passing result; the waits are shared-machine contention, not performance evidence.

After the final explicit conversions and tiny-recovery guard, a rebuilt Windows `flora` run
passed all 124 cases and 69,417,155 assertions, with no skips, in 399.84 seconds (401.07 seconds
including CTest overhead). The seven new section pins and all mature-species pins agree on
Windows and Linux. Final lint, licence and documentation checks pass. Full affected, final-flora
and retry CTest logs are retained beside the captures as `windows-affected-LastTest.log`,
`windows-flora-final-LastTest.log` and `windows-retry-LastTest.log`.
