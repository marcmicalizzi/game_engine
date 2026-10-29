# Deadwood surfaces and supported leaning (G104)

Research read 2026-10-29, before the surface design. Root plates, roots and retained soil
remain owned by soil; this work changes only deadwood. Automatic scene support discovery
and mechanical equilibrium are outside the authored contact contract.

## Grounding and budget decisions

| Quantity/rule | Source read | Value used | Budget trade |
|---|---|---|---|
| Longitudinal versus transverse wood | Wiedenhoeft and Eberhardt, [Wood Handbook chapter 3](https://research.fs.usda.gov/download/treesearch/62242.pdf), pp. 3-5–3-7, 3-12 and 3-15 | Grain follows source axial UVs; every solid cut gets a centred planar island; transverse hollow rims use the source cross-section | Shared native maps, no fibre geometry or renderer change |
| Pore distribution | Same chapter, pp. 3-6–3-7: oak/ash ring porous, maple/birch diffuse porous; conifer tracheids | Ring-porous, diffuse-porous and conifer contrast groups; walnut darker, alder warmer | Typical vessel diameters are 50–200 micrometres (p. 3-12), below our texel size: do not draw millimetre holes and call them vessels |
| Appearance across cut planes | Bond and Rappold, [Virginia Tech ANR-64P](https://www.pubs.ext.vt.edu/content/pubs_ext_vt_edu/en/ANR/ANR-64/ANR-64.html), Wood Surfaces and Cross Section Characteristics | Growth rings centred on the cut, lighter sapwood vocabulary; source branch/trunk axes remain the geometry authority | Colour palette is an authored approximation, not calibrated species reflectance |
| White and brown decay are distinct | [USU Wood Decay Fungi](https://extension.usu.edu/planthealth/ipm/ornamental-pest-guide/diseases/wood-decay-fungi), White Rot/Brown Rot; [UC IPM](https://ipm.ucanr.edu/home-and-landscape/wood-decay-fungi-in-landscape-trees/), fungus table | Separate authored fibrous and cubical patterns; pale versus brown weathered wood; decay 0–1 independent of species and structural collapse | Surface relief only, no time-to-decay or strength prediction |
| Supported falls | [G101 grounding](deadwood.md), Parks et al. field guide | Grounded lower end and an explicit capsule touching retained wood on the visible supporting tree | Authored pose and contact, not a dynamics or automatic discovery solver |

The sources do not supply a universal growth-ring width, bark-loss contour, fracture-band
width, RGB triplet or roughness value. The following are explicitly rendering choices,
not fitted measurements or botanical constants.

| Authored quantity | Value used and reason | Cost/limit |
|---|---|---|
| Map size | 512 square by default; powers of two 128–1024; zero for flat debug comparison | Three maps (sRGB colour, tangent normal, packed roughness) per used exposed role; native texture mip/BC path |
| Side-grain repeat | 1 m; 128 circumferential / 8 axial noise cells, 4 mm smooth warp, 8 by 2 broad cells | Elongated bundles visible at the texel scale, not individual anatomical cells |
| End grain | 6 mm illustrative ring spacing, 1.5 mm irregularity; shared span enclosing the largest cut with 5% margin | Fade rings when spacing falls below 2–4 texels; small branch cuts retain metre scale |
| Relief | .5 mm fibres, .25 mm bands, up to 2 mm decay grooves; fracture adds 1 mm fibres | Baked normals only; no silhouette displacement, extra collision or shadow geometry |
| Decay checking | Warped 16 by 8 cell pattern per metre | Artistic cubical breakup; deliberately independent of species and age |
| Roughness | .73–.85 fresh, up to .97 decayed | Provisional dry-wood appearance, no wetness model |
| Bark loss | Smooth 5 by 1.5 plus 17 by 6 noise cells per metre, .75/.25 shares; .04 field band of dark fractured bark | One uniform subdivision, then up to two contour-only refinements above 12 cm UV spans, then shared-edge clipping; all added triangles count against the existing cap |
| Bark coverage | 0 all exposed, 1 intact; intermediate values threshold the coherent field | Coverage is a control, not an exact area fraction. No separate peel thickness or loose bark sheets |

End grain, longitudinal exposed wood and bark-fracture edges have distinct material roles.
Explicit `tree.materials.wood` overrides all three; otherwise `<species>/wood`,
`<species>/end_grain` and `<species>/bark_fracture` are resolved, with the wood pack role
as the generic fallback before native maps. Source bark and leaf sets remain unchanged.

## Validation

Linux Clang's selected 13 checks passed, including flora, deadwood, scene generators,
renderer/viewer integration, documentation and lint (291.66 s test time; build/test wall
5m39s after acquiring the shared build lock). The focused Windows deadwood checks passed.
The affected Windows run selected 124 checks: 123 passed, none failed, and `engine_view`
exhausted its 30-minute GPU queue deadline (exit 75). A second full viewer invocation also
exhausted that deadline (1,803.08 seconds). All 142 enabled viewer cases then passed using
the unchanged binary in six sequential file-ordered ranges: 1–24, 25–48, 49–72, 73–96,
97–120 and 121–142 (46,284 assertions in total). Each process retained the normal per-device
GPU lock and its normal queue deadline. No case failed or exhausted its deadline in these groups.
The concatenated `--list-test-cases` output for those ranges exactly matches the full list
of 142 enabled cases, with no omissions or overlap. Doctest's console summary counts all
cases passing filters before its range check, so its repeated "142 passed" summaries must
not be summed; the range manifests establish which cases actually execute.
The single-process viewer invocation remained queue-limited; the grouped runs establish
complete enabled-case coverage. Logs and the coverage manifest are retained in the external
capture folder's `validation/` directory. The normal merge gate remains a separate run.
The contended run took 8,948.95 seconds, including 1,740.60 seconds for renderer and 575.24
for volumetrics; these are shared-machine validation durations, not performance measurements.
The separate frame-loop allocation check passed. Final review found that material
seam edges could be mistaken for physical boundaries in a hollow log (surface area 2.48223
versus 2.38019 m2). Physical closures now precede bark partitioning; a regression requires
unchanged surface area for both solid and hollow remains. Contour-only refinement also
reduces the coarse polygon-shaped bark boundaries seen in the first close capture. The
final revision passes all 12 Windows deadwood cases (470,065 assertions), seven selected
Linux deadwood/docs/lint checks, and a final Linux deadwood recheck after adding the
catalogue's conifer aliases. The Linux build volumes were pruned after the runs.

Version 2 changes cut UVs, material partitioning and embedded fallback images. Its complete
64 KiB-page container pins are `d2e2c9f84d2b4f59` (collapsed oak) and `e1109ed169374c96`
(fresh spruce), reproduced on MSVC and Linux Clang. Superseded v1 values were
`237cacf9e023dabf` and `76a3cc6c196c3032` from dc2581d5. No living-flora solver or pin changes.
Timing claims require a quiet run with machine state.

## Captures and resource costs

Recipes, cameras, cooked containers, PNGs, comparison JSON and frame logs are retained in
`D:/workspace/game_engine_local/deadwood/2026-10-29-followup/`. The original v1 outputs remain
in the sibling `2026-10-29/` directory. `gallery-before.png` and `gallery-after.png` use the
same camera and renderer. Individual views cover bare oak, snag, stump, hollow/collapsed log,
limb and spruce; `surface-{oak,maple,pine,fibrous,cubical}.png` show the new material groups.
The textured lean uses existing POC oak bark/leaf image paths read as data; no POC file changes.

| Fixture | v1 / v2 finest triangles | v2 clusters / levels | v1 / v2 container bytes |
|---|---:|---:|---:|
| Fresh oak fall | 10,586 / 10,586 | 561 / 9 | 1,294,576 / 2,652,592 |
| Bare oak fall | 2,486 / 15,580 | 628 / 9 | 241,680 / 4,055,088 |
| Standing snag | 4,934 / 21,497 | 836 / 10 | 515,824 / 4,556,736 |
| Snapped trunk | 242 / 1,015 | 32 / 5 | 20,592 / 2,665,072 |
| Hollow stump | 312 / 312 | 10 / 2 | 29,136 / 1,980,304 |
| Hollow log | 720 / 2,514 | 77 / 6 | 53,808 / 2,776,816 |
| Collapsed log | 492 / 1,041 | 33 / 6 | 46,944 / 2,677,680 |
| Detached limb | 834 / 834 | 46 / 5 | 94,288 / 1,384,112 |
| Fresh spruce fall | 7,358 / 7,358 | 293 / 8 | 678,288 / 1,965,248 |
| New supported oak fall | — / 13,770 | 525 / 9 | — / 3,827,488 |

The nine original fixtures total 60,737 finest triangles, versus 27,964 in v1. All remain
within their existing recipe triangle caps. Partial bark coverage pays for the
contour subdivision; intact/bare surfaces avoid it. Map PNG payloads dominate the new container
cost. End/side material separation changes DAG clustering even where triangle counts are equal.
The supporting living oak is 57,874 triangles / 2,049 clusters and is unchanged source flora.

The 960x640 contact views at 5, 20 and 60 m compare 1 px against .05 px DAG cuts, sun 135/40
degrees, CSM. FLIP means are .000962437, .000411775 and .00209314; SSIM .998973, .999284 and
.997222. These whole-image scores include much background and do not certify branch silhouette.
`lean-2-m.png` shows the contact: the end of the fallen bole meets the standing trunk, with
retained branches fitted or broken around it. Its test checks visible trunk cross-sections,
not just the authored capsule. This is a static rest pose; the live supporting tree has no
runtime coupling to the fallen tree under wind.
Changing either tree's species, seed or geometry profile requires reauthoring the capsule
and rechecking contact; the fixture regression deliberately detects a changed supporting trunk.

Matched debug smoke measurements use 20 frames, two repeats, 10 warmup frames and
`--wait-quiet 15`; each records zero frame allocations/buffers. They are upper bounds from
a busy machine, not a release frame-budget acceptance or a forest-scale performance claim.

| Measurement | Gallery v1 | Gallery v2 | Lean at 20 m |
|---|---:|---:|---:|
| Cluster-instance pairs | 1,478 | 2,695 | 2,753 |
| Visible pairs | 100 | 110 | 1,308 |
| Shadow pairs, all cascades | 131 | 149 | 1,278 |
| Texture bytes | 15,360 | 8,644,608 | 30,363,648 |
| GPU used MiB | 148 | 148 | 224 |
| Median GPU ms | .132752 | .133544 | .170080 |
| Median shadow ms | .027184 | .027016 | .043888 |
| Median CPU ms | .459425 | .457425 | .582250 |

Device: NVIDIA RTX 5090, MSVC debug. Recorded `machine_state`, start/end: gallery v1 other
CPU 100/100%, GPU utilisation 7/18%, GPU memory 6019/6196 of 32607 MiB; gallery v2 other CPU
100/100%, GPU 3/3%, memory 5991/6021 MiB. Lean other CPU 32.4698/20.0340%, GPU 5/34%, memory
5634/5650 MiB. All have `quiet=false`, session unlocked and a valid GPU lock held on their
behalf. No timing improvement or regression is inferred from these samples.

## Visual limits left explicit

The fallback is procedural, not scanned wood. Longitudinal bundles and cubical decay are
visible; fine rings become subtle under minification. Close stumps still reveal the source
trunk's polygonal silhouette, and bark without an authored pack remains flat. The fracture
border is a dark surface band, not raised or peeling bark. Bark coverage can leave small
islands; it is not a simulation of bark shedding. No geometry displacement, dynamic support,
automatic support discovery, root plate or soil changes are included. The owner's review of
the retained captures remains the appearance acceptance.
