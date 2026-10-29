# Forest litter and debris accumulation (G103)

Research read 2026-10-29, before implementation. A static appearance cook, not a prediction
of mass, decomposition, transport or fire. [Capability rules](../subsystems/litter.md).

| Quantity | Source and evidence read | Value used / budget trade |
|---|---|---|
| Litter versus duff | Brown 1974, [INT-16](https://research.fs.usda.gov/download/treesearch/28647.pdf), printed pp. 7–9: fresh leaves, needles, twigs, bark and fruits; decomposed bound material is duff | Six kinds; no duff geometry or invented mass-to-particle conversion |
| Fine wood | Brown, pp. 1 and 9: diameter bins 0–.25, .25–1 and 1–3 inches; cones/bark excluded from woody counts | .006 m twig and .016 m branch defaults are in first two bins (inch = .0254 m). Lengths .35/.7 m are authored fragments |
| White oak leaf | [Native Plant Trust](https://gobotany.nativeplanttrust.org/species/quercus/alba/): blade 79–230 mm by 40–165 mm, lobed | .12 by .065 m, unchanged across detail; 64 leaves per patch is authored sampling |
| White pine | [Missouri Department of Conservation](https://mdc.mo.gov/discover-nature/field-guide/eastern-white-pine): needles 3–5 inches; cones 4–8 inches, numerous thin unarmed scales | .10 m needles, .001 m authored width; .15 m cones, .04 m authored width. 400 separate fallen needles or 5 cones per patch |
| Discontinuity | Sanchez-Lopez et al. 2023, [Ecological Modelling 481](https://doi.org/10.1016/j.ecolmodel.2023.110369), abstract/publisher search text: crown inputs explain heterogeneity in a 5 m model of longleaf pine litter | Patchiness supported qualitatively; Florida mass constants not transferred to this forest |
| Redistribution | Bilby and Heffner 2016, [Forest Ecology and Management 369](https://doi.org/10.1016/j.foreco.2016.03.031), publisher abstract/introduction: wind, vegetation, slope and litter type affect transport | Depression and obstacle-edge biases are authored proxies. No universal measured gain/band found; .8 m band and .65 gains remain editable |
| Bark and curl | Brown identifies bark; sources do not give flake-size or dry-curvature distributions | .09 by .035 m flakes, 20/patch, curl .35 of length; .001 m thickness. Authored, not material mechanics |
| Shape interpolation | Engineering shape functions, not fitted botanical coefficients | Leaf half-width: `.06 + .94 sin(pi t)` envelope times `.72 + .28 cos(8 pi t)` lobes; sheet rise `curl * length * ((t-.5)^2 + .2 side^2)`. Needle half-width tapers from .5 to .1 of nominal width; bend `.002 t^2` m. Cone radius envelope `.2 + .8 sin(pi t)`, core radius .3 width, scales .5 width; scale base .65 radius, tip 1.08 radius, half angle .34 rad and half length .65 row spacing. These coefficients make bounded source geometry; they do not predict drying or species morphology |
| Deposition | No site survey supplied | .45 m particle-centre radius, .025 m centre depth; quadratic radial falloff, hashed yaw/height. 2 m cells, .65 m enclosing radius, .12 background chance, ±.1 cell jitter; exposed gaps |
| Source resolution | Engineering choices | Leaf/bark 12/6/3 segments; needles 3/2/1; cones 8/6/4 rows, 8 scales/row over a closed eight-sided core. Budgets leaf 4096, needles 4800, cone 4096, bark 2048, twig 12000, tangle 24000. Never drop requested particles to meet budget |
| Placement bounds | Engineering controls | .1 m depression normalization; central differences at extent; slope cap .6; cell 1..16 m; 4096 candidates/tile and 1048576/window default; nonzero 32-bit IDs validated before work |
| Needle distance form | Coverage check during G103: unconstrained decimation erased the bed in the 1.7 m view | Every needle keeps one chord quad at .0005 m error; quadratic bend .002 m has at most .0005 m chord deviation. 800 terminal triangles/default bed; subpixel raster aliasing remains |
| Small wood | Existing [deadwood model](deadwood.md) and flora oak | Same public deadwood solver; source height >=.35 m, radius >=.003 m, fracture relief .005 m, source budget 8000. Twig uses a cut stem; tangle uses several small bare falls. No second wood mesher |
| Appearance | Authored fallback, not measured optics | Linear RGB (.18,.085,.028), needles (.22,.12,.045), roughness .9, metallic zero; roles litter/leaf, litter/needles, litter/cone, litter/bark, litter/wood. Sheets use 0..1 UV; wood inherits its UVs. Study soil RGB (.075,.045,.025), roughness .95 |

Obstacle circles only include earlier entries anchored on the same tile so streamed and whole
reads agree. Adjacent-tile overhangs are not inferred. A tangent-plane placement approximates
ground; highly curved surfaces need smaller patches. Small source stems are an analogue for
fragments, not a botanically classified distal twig. Curl, density and optical defaults are not
research measurements.

## Validation and measurements, 2026-10-29

The Windows debug affected run completed 116 tests with no failures, then was stopped during
`engine_view` at the owner's request; `engine_view.frame_loop` was also omitted. Existing view
tests do not select litter, and the offscreen captures below exercise its registration and draw
path. The seven remaining app checks passed in a separate filtered run, including the new
content-cook regression: 123 completed passes in total, two view checks incomplete/omitted.
This is not a complete affected-suite pass. Linux Clang debug passed 18 filtered checks,
then four follow-up checks covering litter, the executable content cook, docs and lint after
the last test additions. Linux GPU-specific cases may skip without a driver. Both checkout
container volumes were pruned after validation. Formatting, banned-pattern/license lint and
documentation checks pass. Release, disabled-capability and frame-budget gates remain the
merge coordinator's responsibility.

The six seed-17 recipes have 3072/2400/1360/1600/2625/8536 source triangles respectively
(leaf/needles/cone/bark/twig/tangle), all below their profiles' budgets. MSVC debug and Linux
Clang debug agree on the six complete-container pins. Pins are new; no existing kind's pin or
shape changes. Tests cover mesh validity, strict budgets, source wood reuse, schema rejection,
registry round trips, empty output on failure, substrate weights, obstacle bands, hollows,
tile order, ID/work bounds and precision 16 billion metres from the origin.

Review outputs are under `D:/workspace/game_engine_local/captures/2026-10-29/forest-litter/`:
recipes, `.clusters`, capture scripts, exact cameras, per-capture stats, finest/coarse pictures
and FLIP heat maps. `study` and `scene` have 5/20/60 m horizontal-offset views at sun azimuth
45°, elevation 35°. The flat fallback is deliberately simple; `poc-soil-study` and `leaf-close`
use the POC's existing soil texture paths in a separate external scene. They are a material
integration preview, **not a full POC-world run**. No POC file or script was changed or executed.

At the close 960×720 camera, 1 px LOD versus .05 px: mean FLIP leaf/cone/bark 0, needles
.004983, twig .000393, tangle .002607, below the E10 .02 picture threshold. Visible pairs are
147→147, 65→25, 65→65, 77→77, 37→15 and 121→50. This is a direct engine-host comparison of
generated recipes, not a claim that the GLB ingestion harness ran. The unchanged cuts of leaves,
cones and bark at close range need their geometry for the silhouette.

At 5/20/60 m horizontal offset, height .3 times offset, 960×720 and 1 px LOD, cooked-file
visible pairs are leaf 147/11/2, needles 25/25/25, cone 65/5/1, bark 77/1/1, twig 1/1/1,
tangle 37/1/1. The needle terminal form is already active at 5 m; it deliberately retains
the same particles past that distance. There is no sudden far-card handover. Fine lines can
still disappear between pixels, which geometry retention cannot fix without a filtered far form.
The six 65536-byte-page containers occupy 440752, 410176, 178800, 223984, 204928 and 672560 bytes,
respectively; these are shared once across instances. An intermediate content-tool cook gave
an older needle chain; a fresh uninstrumented object and recook reproduced the current direct
build. The app-level cook/read-back regression now checks terminal coverage in addition to
the module's hash and registry checks.

The path comparison is MSVC **debug**, RTX 5090, 1280×720, CSM, 120 path samples, three repeats,
60 warmup frames and at least two seconds of warmup, `--wait-quiet 30 --census`, with the GPU lock.
Both runs report **quiet=false**: other CPU work 8.19–12.79%, GPU activity 2–3%, total GPU memory
about 6840 MiB of 32607 MiB. These are upper-bound observations on a shared machine, not isolated
incremental costs, and do not replace the merge's release frame-budget gate.

| Quantity | Ground + 12 stumps | With 93 litter patches |
|---|---:|---:|
| Scene pairs / 16777216 limit | 892 | 14659 |
| Maximum visible pairs | 125 | 562 |
| Maximum shadow pairs | 125 | 127 |
| GPU total median, ms | .19632 | .19928 |
| GPU shadow median, ms | .031456 | .033184 |
| Frame median / p95, ms | .37845 / .46921 | .37985 / .50036 |
| Driver process memory, MiB | 192 | 192 |
| Steady frame allocations / device buffer creations | 0 / 0 | 0 / 0 |

The four 32 m tiles add 13767 candidate pairs in total (3441.75/tile averaged over this fixed
seed), 23.25 accepted patches/tile on average. A placement is the existing 64-byte CPU record;
the GPU instance is the existing 96-byte record. For this entry those records are 5952 CPU and
8928 GPU bytes, excluding vector capacity, shared mesh data, pair tables and renderer scratch.
The unchanged driver MiB figure is allocator granularity, not zero memory use.

Visual limitations observed: flat fallback colours show facets at macro distance; narrow needles
alias below a pixel; finite source patches remain visibly distinct; tangent-plane placement can
intersect curved ground. No claim of calibrated deposition density or whole-world visual acceptance.
