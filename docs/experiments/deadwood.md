# Deadwood and tree remains (G101)

Research read 2026-10-28, before implementation. This is a deterministic **asset cook**, not a
prediction of time since death, impact forces, decay rate or safe structural loading. The source
tree supplies its species, maturity, dimensions, branching, bark UVs and leaf shapes unchanged.

## Grounding

| Quantity/rule | Source read | Value used | Budget trade |
|---|---|---|---|
| Distinct fresh, weathered and decomposed log structures | Parks, Bull & Torgersen, 1997, [PNW-GTR-390](https://www.fs.usda.gov/pnw/pubs/pnw_gtr390.pdf), printed pp. 34–35 | Fresh falls retain bark/branches and can be supported above soil; weathered logs lose both and contact soil; advanced remains lose integrity | Independent bark, branch and contact controls; no universal age-to-shape curve |
| Snags can have broken tops and hollow interiors | Same field guide, printed pp. 30–33 | Snag and snapped-stem recipes; separate hollow log/stump forms | Reuse source stems; no independent generic cylinder replacing species geometry |
| Five qualitative decay classes | [OSU/USFS woody detritus programme](https://andrewsforest.oregonstate.edu/sites/default/files/lter/pubs/webdocs/reports/detritus/main_index.shtml), visual characteristics section | Sound wood can support itself; advanced rotten pieces lose shape and spread on the ground | Hollow radius and vertical collapse are independent authored controls; class numbers are not numerical material properties |
| Wood bends and has direction-dependent properties | Hiziroglu, 2016, [Oklahoma State FAPC-162](https://extension.okstate.edu/fact-sheets/strength-properties-of-wood-for-practical-applications), viscoelasticity and MOE/MOR sections | Contact deflects branches before breaking them; fracture variation follows the stem's axial coordinate | Geometric quadratic deflection with an authored limit; no use of dry-lumber strengths as green/dead branch strengths |
| Living tree architecture and species dimensions | [Profile audit](tree-profile-audit.md), [allometry](tree-allometry.md), [maturity](tree-maturity.md) | Exact input FloraFile passed through `solve_flora`; no new species table | Uses the source recipe's existing detail ladder |

The publications do **not** establish a universal retained-foliage fraction, branch-break angle,
hollow-wall thickness, fracture wavelength or deadwood lifetime. Fresh leaves are an explicit
owner requirement. These remain author choices, not measurements attributed to the sources.

## Authored quantities and numerical policy

| Quantity | Value used and reason | Trade/limit |
|---|---|---|
| Kind defaults | fallen/limb: all foliage and branches; snag: no foliage, .35 branch retention; snapped: no foliage; log/stump: bole only | Presets for the requested visual states, not empirical decay classes; explicit fractions override them |
| Default interval | stump: lesser of 1 m and .2 source length; snapped: .45 source length; log: .6 source length | Authored examples; metres override them. Detached limb selects a source part and its descendants |
| Broken branch remainder | .08–.20 of its length; zero branch retention leaves none | Seeded representation, not a failure probability derived from wood strength |
| End fracture | default .12 m, allowed 0–2 m; two deterministic waves at 13 and 19 rad/m, .3/.2 amplitude about .5 | Source tessellation limits splinter resolution; no subpixel fibres or new fracture simulator |
| Bark patches | .25 m circumferential by .8 m axial cells in source UV metres | Material exposure only: no bark slab thickness, loose flakes or peeling strips |
| Hollow | inner/exterior radial ratio 0–.9 | Inward shell plus boundary rims; only bole-only logs/stumps; source axis is the cavity approximation |
| Collapse | 0–.85 vertical compression; pitched horizontally; an upper radial strip opens as collapse grows | Geometric collapse of the shell, not volume conservation or fungal erosion |
| Contact fitting | quadratic weight along source part; maximum .15 part lengths by default, allowed 0–.5; four constraint passes | Authored bend ceiling; exceeding it clips .06 arc weights before contact. An unsafe new fracture removes that branch and descendants. No claim to species rupture strength |
| Contact tolerance | default .01 m, allowed .001–.1 m | Sampling checks vertices and edge midpoints; terrain/support features smaller than those spans need a finer source mesh |
| Support shape | capsule, radius .01–20 m, segment within +/-1000 m, up to 64 | Author-supplied approximation to another tree's bole/limb, not automatic discovery or scene physics |
| Terrain | flora's local grid, 2–513 per axis, .03125–4 m spacing, +/-1000 m heights | No extrapolation; site data is part of the recipe and rebuilds on changes |
| Topology keys | 10 micrometres, area squared threshold 1e-18 m4 | Keys match boundary seams without changing positions; degenerates removed before cluster build |
| Budgets | stump 8k, log 20k, limb 60k, snag 100k; full falls/snaps source triangles +16,384 closure reserve, explicit 128–4m cap | Initial owner-tunable allocations, not accepted scene budgets. Insufficient cap is refused by name |
| Flat exposed wood | linear RGB (.52,.32,.15), roughness .9 | Stand-in when no species/wood pack role or explicit wood set exists; not measured reflectance |

## Validation and measurements

Validation completed 2026-10-29 UTC. Windows `tools/dev.ps1 test -Affected` selected 123 checks
because content fixtures reach every module: 122 passed, with the initial missing schema-doc page
the sole failure. After adding/indexing that page and rebuilding the final explicit precision
casts, `deadwood`, `docs_check`, `tools.docs_check` and `lint.banned_patterns` all passed (4/4).
The affected run includes the original living-flora pins, renderer, collision, viewer, zero-allocation
frame loop, CLI, editor, content cooker and MCP checks; none of the code checks failed or skipped.
Linux Clang's selected 19 checks likewise had only the initial documentation-index failure;
the corrected docs and deadwood were rechecked successfully (4/4). Lint/licenses and formatting
passed. This checkout's two Linux volumes were pruned after validation. The merge's full
multi-configuration and release frame-budget gate remains the coordinator's job.

Captured 2026-10-29 using this branch's own MSVC debug binaries, offscreen, on RTX 5090.
Artifacts and exact cameras/recipes: `D:/workspace/game_engine_local/deadwood/2026-10-29/`.
`gallery.png` shows the nine forms; individual close captures show the hollow walls, opened
collapsed roof, snag, leafless fall and retained conifer foliage. Flat stand-in materials make
the geometry inspectable; leaf cards retain the living source's untextured rectangular outlines.
The owner's visual acceptance is still a separate decision.

| Fixture | Source triangles | DAG clusters / levels | Container bytes | Build warnings |
|---|---:|---:|---:|---:|
| Living oak, identical source recipe | 18,606 | 617 / 9 | 2,400,388 | 0 |
| Fresh oak fall | 10,586 | 561 / 9 | 1,294,576 | 0 |
| Bare oak fall | 2,486 | 105 / 7 | 241,680 | 0 |
| Standing snag | 4,934 | 226 / 7 | 515,824 | 0 |
| Snapped trunk | 242 | 10 / 3 | 20,592 | 0 |
| Hollow stump | 312 | 14 / 4 | 29,136 | 0 |
| Hollow log | 720 | 24 / 5 | 53,808 | 0 |
| Collapsed log | 492 | 20 / 5 | 46,944 | 0 |
| Detached limb | 834 | 46 / 5 | 94,288 | 0 |
| Fresh spruce fall | 7,358 | 293 / 8 | 678,288 | 0 |

The living container includes its existing bake; the damaged one uses its own generic DAG.
The nine remains total 27,964 finest triangles. The gallery plus its 8,192-triangle ground
allocates 1,478 cluster-instance pairs, below the 2^24 pair cap. Bounds and storage are measured
facts, independent of machine load. Build timings in the logs are **not** performance claims.

Matched fresh-oak captures use a 960x640 view at distances 5, 20 and 60 m, sun azimuth/elevation
135/40 degrees, CSM, with 1 px versus .05 px LOD thresholds. FLIP means are respectively
0, .000487135 and .00126944 (SSIM 1, .999366, .997626). These are whole-image errors over a
small distant object, not proof of unchanged leaf silhouette. The normal cut at 20 m draws
198 oak clusters and 38 ground clusters. All four shadow cascades together draw 392 pairs;
the deformed pool uses 443,868 bytes with no overflow. Two fixed parts per instance still use
the flora path: the one-tree scene allocates a 960-byte pose table including renderer padding.

`fresh-oak-20-m.frames.jsonl` records a short **debug smoke measurement**, 20 frames, two
repeats, 10 warmup frames, `--wait-quiet 15`: zero frame allocations/buffers and zero census
mismatches; median GPU .182688 ms, shadow .040432 ms, deformation .013672 ms. It is not a
release frame-budget acceptance or a forest-scale claim. `machine_state`: quiet=true;
other CPU 8.8542% start / 9.375% end; GPU utilisation 0% at both samples; GPU memory
6,854 / 32,607 MiB; lock held on this run's behalf at both samples. The source-only build is
not a matched timing baseline, so no speedup is inferred.

Initial complete-container pins: collapsed oak `237cacf9e023dabf`, fresh spruce
`76a3cc6c196c3032`, both reproduced by Linux Clang 18, using no material pack and 64 KiB pages
(the CLI captures use its default 128 KiB pages). They include positions, quantization,
normals, UVs, hierarchy, materials, motion and recipe identity. Windows cases also exercise
all six forms, slope contact, leaning support success/refusal, independent branch/leaf loss,
sub-segment intervals, strict input validation, source preservation and insufficient budgets.

Two issues caught before handoff are now regressions: clipping a bent branch could create a
below-ground fracture vertex; and normalizing a very small valid cap face could yield a zero
normal. Final contact pruning and an area-normal fallback fix them. Clang required explicit
f64 promotions in fracture waves; those casts leave the Windows hash pins unchanged.

The source tree's golden hashes must not move. Deadwood identity includes the canonical source,
flora version, deadwood version, pose, contact geometry, damage controls and triangle cap. No
output depends on thread order, world float coordinates or the C library's trigonometry.
