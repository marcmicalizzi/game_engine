# Surface colonization (G124)

The [cover capability](../subsystems/cover.md) already grows companion meshes. This extension
supplies ecological host information independently of shading roughness, surface age and mesh
names. It models static habitat suitability, not ecological succession or germination rates.

## Grounding, read 2026-11-04

| Quantity/rule | Source | Value used | Budget trade |
|---|---|---|---|
| Substrate and exposure are species dependent | [US Forest Service, Lichen Habitat](https://www.fs.usda.gov/wildflowers/beauty/lichens/habitat.shtml), light and substrate sections | Explicit siliceous/calcareous rock, masonry, living bark, dead wood, soil and sealed surfaces; crustose guild optionally restricted to rock chemistry, bark or wood | Categorical eligibility; no chemical simulation. The existing generic exposed-surface response is retained, not asserted for all lichens. |
| Moisture and shelter favour moss | [Jang and Viles, Moisture Interactions Between Mosses and Their Underlying Stone Substrates](https://doi.org/10.1080/00393630.2021.1892430) | Existing host-ray sky visibility; optional multiplier `1 - bias * sky` | Author-selected bias 0..1, default 0. Geometry supplies crevices; no displacement from a texture. Publisher blocked a direct fetch; indexed abstract/conclusions were readable, so no measured numerical rate is taken. |
| Nurse-log establishment depends on the seedbed, not simply elapsed time | [Fukasawa and Kitabatake 2024, seed-sowing experiment](https://pmc.ncbi.nlm.nih.gov/articles/PMC11148398/) | Dead wood, independent softening and penetrable rooting depth; no plant on an unspecified or fresh host | Categorical wood requirement and smooth dimensionless response, not a fitted seedling probability. Introduction, methods and discussion read through indexed full text; direct page presented a browser check. |
| Thick moss can exclude seedlings | [Woods, Maleta and Ortmann 2021](https://pmc.ncbi.nlm.nih.gov/articles/PMC8293789/), results/discussion | Author-selected inhibition ramp from 4 to 8 cm of existing mat | Study reports species/community effects and discusses an earlier experiment with 1.4, 3.7 and 7.8 cm mats. These are context, not universal thresholds; no automatic conversion from cover-mask intensity to mat depth. |

Fukasawa and Kitabatake used 62 already well-decayed class-IV pine logs and twelve tree species;
effects of rot type depended on species. That field class is not converted to the recipe's 0..1
decay intensity, and the solver does not claim a general advantage of brown over white rot.

The sources support distinctions and directions of response. No source supplies a universal
decay-to-fungus curve, nurse-log rooting-depth threshold or rosette size. The following are explicit
authoring approximations, exposed in the recipe or named here, rather than measured constants:

| Parameter | Value used and reason |
|---|---|
| Decay | Continuous 0..1, like the deadwood recipe's authoring intensity; smoothstep 0..0.25 establishes fungal and plant suitability. It is not a five-class field inventory or years. |
| Depth input domain | Both depths accept 0..1 m for shallow log-seedbed authoring. The ceiling is input validation, not a maximum depth in nature; the response saturates well below it. |
| Penetrable rooting depth | Smoothstep 0..0.03 m; zero prohibits plants, three centimetres saturates the small-plant proxy. |
| Mat suppression | `1 - smoothstep(0.04, 0.08, bryophyte_depth_m)`; shallow and deep mats remain separately authorable. |
| Surface orientation | Smoothstep 0..1 of upward normal; no vascular plants on vertical faces or undersides. |
| Plant density | Default 0 (off), 0..400 per m² of suitable surface; independent seeded rounding. No age multiplier: existing seedbed state is the input. |
| Plant shape | Six radial leaf cards, twelve triangles, 0.12 m nominal leaf (0.02..0.5 m), width 0.3 of length, outward/upward at 45 degrees, kit droop 0.1. Size factor 0.8 + 0.2 times the existing cover variant factor. Representative juvenile rosette, not a species model. |
| Plant fallback material | RGBA `(0.16, 0.34, 0.07, 1)`, roughness 0.8, two-sided: an authored green palette, not measured leaf reflectance. A `cover/plant` pack set replaces it. |
| Budget | Existing 80,000-triangle default and film-first/density ladder; plants share the density rung, material `cover/plant`. No additional scene instances or per-frame solver. |
| Sampling and boundaries | Existing `sample_m` refinement and 32-ray exposure. Last inclusive local box wins. Coating boundaries approximate the sampled mesh; shape roots additionally check the actual sampled point. |

The existing shapes, moisture coefficients, ages and sampling rules remain grounded and qualified
in [vine-cover](vine-cover.md). Legacy recipes retain the old geometry. The version-2 cover cache
identity records the new canonical fields; vine retains version 1 and its existing identity.

## Validation and limitations

The 2026-11-04 gallery uses the three recipes in `content/cover`, each with a 12,000-triangle
companion budget. Recipe ages, wetness and habitat intensities are authored examples, not surveyed
sites. Built with this branch's MSVC debug tools and default cluster LOD:

| Companion | Source triangles | All LOD clusters / levels | Container bytes | Host-mask bytes | Build / host time (s) |
|---|---:|---:|---:|---:|---:|
| Crevice moss | 1,037 | 35 / 6 | 696,668 | 619,820 | 70.90 / 17.23 |
| Exposed lichen | 10,441 | 285 / 9 | 1,310,140 | 619,820 | 76.27 / 14.38 |
| Nurse log | 10,070 | 336 / 8 | 806,172 | 23,404 | 12.71 / 9.94 |

Build times include the host; they were collected on the shared workstation without a quiet-state
sample and are upper bounds, not solver costs. The rock host has 76,800 source triangles and the
log 3,400. The dense rock host's vertex-ordered section-40 mask dominates the small moss
container; this extension does not compress that existing format. The log companion contains
40 moss cushions, 3 lichen tufts, 67 brackets, 20 mushrooms and 61 plants, plus 5,217 film triangles.

Bare/covered gallery measurements: RTX 5090, MSVC debug, 1200x800, normal 1-pixel LOD,
traced shadows, fixed `gallery-20m` camera and sun direction `(0.3995, 0.7990, 0.4494)`, 60 warmup and
120 measured frames. Both used `--wait-quiet 60` under the GPU lock and reported `quiet:false`.
The bare scene loads two host meshes / three instances; covered adds three companion meshes /
instances. Counts are useful; **the timings are busy-machine upper bounds, not a measured delta**.

| Metric | Bare hosts | With cover |
|---|---:|---:|
| CPU median (ms) | 0.39455 | 0.38150 |
| GPU total median (ms) | 0.187248 | 0.193824 |
| Traced-shadow median (ms) | 0.125888 | 0.129744 |
| Frame p95 (ms) | 0.65916 | 1.009545 |
| Resident GPU memory (MiB) | 289 | 296 |
| RT bytes | 65,203,946 | 77,783,692 |
| Scene pairs / visible pairs | 5,132 / 48 | 5,788 / 97 |
| Steady-frame allocations / device buffers | 0 / 0 | 0 / 0 |

Recorded `machine_state`: bare CPU others 99.963% at start and 100% at end, GPU 3% at both,
6,160/32,607 MiB used; covered CPU others 97.968% to 84.896%, GPU 2% at both,
6,165/32,607 MiB used. Both sessions were unlocked and held their own GPU lock
(`mine:true`, owner `claude-engine`, purpose `G124 corrected bare/covered offscreen measurements`).
The RT run measures traced shadows, not cascade rendering; the merge's frame-budget gate is
still required. No tile-placement generator or per-tile allocation was added.

Evidence lives under `D:/workspace/game_engine_local/surface-colonization/2026-11-04/`:
recipes, built containers, cameras, bare/covered PNGs at close views and 20/60/120 m,
build/stat reports, frame records and reproducible PowerShell scripts. The close views show
sheltered green moss, exposed pale lichen and fungi/rosettes on the log. At 60 and 120 m the normal
DAG cut reduces these to small surface patches. The film remains visibly patchy on coarse host
facets, and the plants are simple cards; neither is presented as a species-quality asset.
Visual acceptance by the owner and merge gating remain pending.

The same covered cameras were also captured at `--lod 0.05` and compared with the normal
`--lod 1` images using `engine-image compare`. Whole-image FLIP means: moss close 0.00892937,
lichen close 0.0100286, log close 0.00785188, gallery 20 m 0.00180498, 60 m 0.000331203 and
120 m 0.0000694057. These are below E10's 0.02 reference threshold, but the gallery's large
background dilutes the far-view scores; they are not a species acceptance or a claim that thin
film boundaries are exact. Both cuts and FLIP heat maps are retained for inspection.

Linux Clang debug passed all 127 selected tests (1,499.07 s), followed by `cover|engine_content`
on the final source (2/2, 74.37 s). The follow-up includes the selected generator version in CLI
reports and the inclusive 2 cm leaf boundary; source/test SHA-256s were compared with the tested
Linux snapshot before pruning this checkout's two volumes. Windows `test -Affected` selected 128
tests because the recipes reach the shared content corpus: 127 passed and `engine_view` skipped
after its 30-minute GPU-lock wait expired (total 9,314.04 s). The final rebuild and focused Windows
cover run passed 13 cases / 12,426 assertions (19.25 s). A viewer-only retry also exhausted the
GPU-lock allowance (1,804.67 s), with no assertion failure. A further viewer-only retry uses the
documented local `ENGINE_TEST_GPU_LOCK_WAIT_S=3600` allowance and passed (1,252.65 s);
assertions are unchanged. All 128 selected Windows tests therefore passed across the affected
run and viewer retry. The extended allowance is local build configuration, not a source change.
Lint, documentation checks and formatting passed; the merge matrix and frame budgets were not run.

Pins: existing cover container `09e1eaf9313d8022` becomes `e3e7eb494653eab9` for canonical metadata
and version 2 alone. Restoring only version-1 metadata reproduces the entire old container hash.
Vine remains `9cb906a303a4611a`; the new plant mesh/container are `25201b473dd0580a` /
`4fa3b4dddeb82baf`. The initial discovery run failed three placeholder expectations, then recorded
the MSVC pins that Linux also reproduces. No geometry tolerance was relaxed.

Checks caught two implementation edges: the CLI initially reported cover's version for vine too,
and a double-precision bound rejected the f32 leaf field's documented 0.02 m minimum. The CLI now
selects its generator's version; the minimum is the f32 endpoint explicitly widened to f64.
Clang rejected the initial implicit widening under `-Wdouble-promotion`; the explicit conversion
passes. A concurrent Windows rebuild also hit LNK1168 on the still-running graphics executable;
waiting for it to exit completed the links. A build can relink an otherwise unchanged test, so
finish that process before retrying a locked link.

Host habitat belongs to the cover recipe. A `.deadwood.json` host is read by the existing host
seam, but its decay metadata is not inferred: the recipe states the ecological seedbed explicitly.
This avoids a cover-to-deadwood capability dependency and ambiguous inference from visual decay.
Automatic host/companion placement, moving-host bindings, seasonal fruiting, species-specific
seedlings, real root geometry and R90's blended film remain outside this implementation.
