# Grass sward: research and acceptance (G90)

Reserved on 2026-10-25 for the sward solver: grass as ground coverage, with blade
patches near the eye, clump cards beyond them, and the ground material far away.
This is new work; G75's isolated tufts remain complete and unchanged.

The solver builds on W72's ground masks and W73's streamed placements. Field sway
and caster policies are separately briefed engine work. The owner confirmed on
2026-10-25 that existing cooked paint and distance forms cover the other seams.

Research, implementation and acceptance are in progress. Measurements below distinguish
the passing reference patch from the unfinished family. The owner's visual acceptance remains required.

## Dense cover and management states (continuation, 2026-10-25)

The owner prioritizes dense grass to walk through, tall stands, short trodden/mown/grazed cover,
and sparse clumps, with AAA-quality assets and distance representations independently of future
AO, atmosphere and bloom. The continuation changes the unit of construction from an isolated
leaf to a basal shoot with several leaves, then gathers shoots into crowns. Oregon State's
[Bunch and Sod-forming grasses](https://forages.oregonstate.edu/regrowth/how-does-grass-grow/grass-types/bunch-and-sod-forming-grasses)
distinguishes crown-bound tillers from lateral sod growth; its examples include ryegrass and
cocksfoot as bunch grasses and bentgrasses as stoloniferous. Blair, Nippert and Briggs,
[Grassland Ecology (2014), morphology, pp. 398–401](https://www.k-state.edu/ecophyslab/pdf%27s/Grassland%20Ecology_Springer_Reference_series.pdf),
describe shoots as repeated sheath/blade units and explain protected basal growth after defoliation.
These sources justify grouping and clipping, not the authored sample counts or crown radius.

Implementation v5 uses 1–8 rendered leaves per shoot, 1–32 shoots per crown, a 0–1 gathering
parameter and a 0–0.25 m crown radius. Those ranges are authoring controls, not botanical constants.
Alternating leaf directions form a fan around a common root. Boundary correction moves whole crowns, preserving shared roots. Roots stay inside their cells; foliage overhang is included in actual mesh bounds. The near/coarse rungs share every shoot and taper shoulder; the
coarse rung reduces curve tessellation instead of replacing unrelated leaves with wider blades.
Cutting truncates the axis at a horizontal height and keeps the blade's remaining width, rather
than shortening a pointed leaf. Variable cutting height represents uneven grazing; it is an
appearance model, not a grazing ecology simulation. Species dimensions still bound uncut blades.

The dense candidate uses 1024 tall-fescue blades/m², growth range 0.45–1, and an explicit 8500
triangle/m² all-rung cap (5120 near + 3072 coarse + at most 96 cards = 8288). This is a high-quality
candidate, not a new silently enlarged default or a measured frame-budget allowance. The 2400
kind default remains; the low-cost candidate uses 384 blades and two segments (2400 all-rung).
Mown/grazed candidates budget the extra triangles at cut ends explicitly. Rendered leaf area
and projected coverage must be reported separately from biological shoot density.

Persistent walked tracks need runtime state beyond wind: a spatial bend-direction/flattening
field sampled at world roots, stamped by swept contacts, with decay/recovery and tile lifetime
rules. High graphics quality may retain that visual field; low quality must allocate none.
Long-term wear (shorter/sparser grass) is a separate authoring/gameplay mask, not the same as a
recovering bend. Existing sections 35/36 already supply roots, lengths and bend weights; section
41 remains exactly AO/thickness/hue/age. No extra section or CPU object per blade is proposed.
The engine-side field contract needs contact/history support before a persistent-track demo can
be claimed. The current branch's runtime does not implement that history.

The ComfyUI material lab remains available for appearance comparisons. First establish geometric
coverage and management silhouettes; then feed any selected leaf appearance consistently into
near material, cards and ground with provenance. A photographed/generated whole-sward tile must
not be pasted onto each individual blade. No new generated material is claimed in this step.
## Sources and chosen bounds

Leaf dimensions are leaf dimensions, **not culm height**. Growth interpolates within them;
it is an authored growth state, not calendar phenology. Species-specific seasonal colour,
straw fraction and seed production were not established by these sources.

| Quantity | Source and location | Value used | Budget trade |
|---|---|---|---|
| Tall fescue blade | [USFS FEIS, botanical description](https://research.fs.usda.gov/feis/species-reviews/scharu) | 5–70 cm long, 3–13 mm wide | Sample leaves, not whole plants |
| Perennial ryegrass blade | [Kew GrassBase description](https://powo.science.kew.org/taxon/urn:lsid:ipni.org:names:407493-1/general-information) | 3–20 cm, 2–6 mm | GrassBase paragraph; the Flora Zambesiaca width in the same page conflicts |
| Timothy blade | [Kew GrassBase](https://powo.science.kew.org/taxon/urn:lsid:ipni.org:names:415868-1/general-information) | 4–20 cm, 3–9 mm; spiciform panicle | Never use culm height as leaf length |
| Cocksfoot blade | [Kew GrassBase](https://powo.science.kew.org/taxon/urn:lsid:ipni.org:names:397309-1/general-information) | 10–45 cm, 2–14 mm; fascicled panicle | Folded leaf approximated as ribbon |
| Wood melick blade | [Kew GrassBase](https://powo.science.kew.org/taxon/urn:lsid:ipni.org:names:408184-1/general-information) | 5–20 cm, 3–7 mm; open panicle | Woodland profile; density unknown |
| Wavy hairgrass blade | [Kew GrassBase, Avenella flexuosa](https://powo.science.kew.org/taxon/urn:lsid:ipni.org:names:391947-1/general-information) | 5–20 cm, 0.3–0.8 mm; filiform involute | Rolled leaf approximated as ribbon; the old tuft's 4 mm is not this species' width |
| Bent blade | [Flora of China, Agrostis capillaris](https://www.efloras.org/florataxon.aspx?flora_id=2&taxon_id=242301747) | 2–15 cm, 1–4 mm; open panicle | Short growth state, not simulated mowing |
| Red fescue blade | [Kew GrassBase](https://powo.science.kew.org/taxon/urn:lsid:ipni.org:names:30010036-2/general-information) | 3–40 cm, 0.5–1 mm (exceptionally 2 mm) | Common width range |
| Field tiller density | [Praat 1995 thesis pp.150–151, PDF 167–168](https://mro.massey.ac.nz/server/api/core/bitstreams/0fbaf226-da07-47c6-9511-ecc60c49fc9e/content) | Ryegrass 5815–7414/m²; tall fescue 3080–3093/m² at 217 days after sowing | Treatment estimates, not universal defaults; other profiles use zero for unknown |
| Grassland LAI | [Scurlock, Asner & Gower 2001, Table 6 p.19](https://info.ornl.gov/sites/publications/Files/Pub57077.pdf), precursor to [Asner et al. 2003](https://doi.org/10.1046/j.1466-822X.2003.00026.x) | Filtered mean 1.71, range 0.29–5.0 | Report mesh one-sided area separately; full 2003 table not retrieved |
| Wind loading | [de Langre 2008 pp.145,159](https://yakari.polytechnique.fr/Django-pub/documents/delangre2008rp-1pp.pdf) | Small-deflection drag scales with speed squared; reconfiguration reduces it | No species EI found; CPU compliance is compatibility, not measured stiffness |
| Canopy frequency | [Py et al. 2006 Table 1 p.430](https://yakari.polytechnique.fr/Django-pub/documents/py2006rp-1pp.pdf) | Wheat 2.50 Hz, alfalfa 1.05 Hz at about 0.68–0.69 m | Neither is a universal grass frequency |
| Coherent gusts | [Finnigan 2000 abstract](https://www.annualreviews.org/content/journals/10.1146/annurev.fluid.32.1.519) | Canopy-scale coherent sweeps | Spatial wind field; full text unavailable |

Watson & Dallwitz's site could not be retrieved. Species-level Kew and Flora of China descriptions
replace it. Praat's field estimates are distinct from controlled-box ryegrass experiments with
much higher tiller counts. Seed-head habits are recorded; reproductive geometry is not yet built.

## Rendering model and explicit approximations

[Jahrmann & Wimmer 2017 §§3–5](https://www.cg.tuwien.ac.at/research/publications/2017/JAHRMANN-2017-RRTG/JAHRMANN-2017-RRTG-draft.pdf)
uses curved blades and spatially grouped patches. [Wohllaib 2021 slides 24,28–30](https://archive.thedatadungeon.com/ghost_of_tsushima_2020/documents/gdc_2021/gdc_2021_procedural_grass_in_got.pdf)
uses Bézier curves and their derivatives, with fewer vertices at distance. Our 2–3 segments and
256 blades/m² are budget choices, not their counts or botanical densities. The rest axis turns
with authored horizontal curvature coefficient 0.15–0.55 and ±0.2 radians twist; arc normalization preserves leaf
length. This is not an EI beam solution. A whole blade translates inward to stay in its cell.

[Sanders, The Vegetation of Horizon Zero Dawn, GDC **2018**, slides 23–30,58,83](https://media.gdcvault.com/gdc2018/presentations/gilbert_sanders_between_tech_and.pdf)
documents distance animation, terrain colour data and distinct visual/shadow geometry. The 2017
procedural-placement talk is different. These motivate colour continuity and reduced shadow
work, not this engine's performance or its nearest-cascade policy.

Historical v4 ladder: 256 blades/m² × 5 triangles near, half as many × 5 coarse with width scaled by
paired nominal leaf area, sixteen clumps/m² × three quads (96 triangles). Default 2400
triangles/m² covers **all levels** (2016 at full density). Up to sixteen variants use independent
deterministic streams. Pairing adjacent headings, keeping the longer axis, avoids the directional
bias of discarding even lattice indices. Card tiles are 128 pixels with one-pixel margins;
the existing baker supersamples 4×4. The cutoff is explicit, default 0.5. These are budget knobs.

AO (base 0.35 to tip 1), thickness (0.5), hue (0.4–0.6), green RGB (0.09,0.22,0.06), roughness
(0.7), and dry share (0.1) are uncalibrated appearance inputs. A gradient is not measured patch
sky occlusion. Main's resolve does not yet read the hue bytes. The ground uses the same colour.

## Confirmed wire contract (owner, 2026-10-25)

One parentless part per blade in section 35. Section 36 stores **linear arc fraction**, 0..65535;
the engine applies the bend shape. Phase derives from part index and instance seed on the GPU.
Section **41**, no other new section, stores u8 unorm ao, thickness, hue, age. Section 37 has no
collider or structural flags. One `sward` catalogue entry covers all botanical profiles.

Caster policy is engine-side: no traced structures, one nearest cascade, rest pose. Current-main
measurements must label `--shadows csm --shadow-distance` as a stand-in. Density is a linear R
paint image through `TerrainMaskPaint`; placements read the same cooked mask. The entry's far
ends at ring 1's floor, with no placements in outer rings. Runtime paint and far-ground wind
sheen remain future work.

## Historical v4 measurements and unresolved acceptance, 2026-10-25

The CPU coverage probe rasterizes near geometry, coarse geometry and masked cards into the same
256² orthographic frame, at four cardinal azimuths and 30° elevation, with 4×4 samples per pixel.
These are geometric measurements, not timing claims. Ryegrass seed 8801, variant 0:

| Density | Coarse relative difference, four views (%) | Cards relative difference, four views (%) |
|---|---|---|
| 0.25 | -1.25, +0.98, +2.03, -0.53 | +9.16, +10.35, +5.95, +6.39 |
| 0.50 | -3.56, +0.47, -0.78, -0.88 | +6.72, +6.23, +8.03, +6.49 |
| 0.75 | -0.45, +2.05, -0.11, -0.69 | +8.85, +2.51, +8.35, +6.42 |
| 1.00 | -0.78, +0.09, -0.63, -0.23 | +3.79, +3.13, +4.74, +4.77 |

The first implementation used two-segment, double-width coarse ribbons and 64-pixel cards:
full-density coverage fell 4–13% and 31–33% respectively. Retaining taper resolution, pairing
headings, and fixing the card UV inset improved this. Lowering the 128-pixel card cutoff to 0.35
overshot by about 40%; it is not the fix. Sparse cards and all other species still need acceptance.
The full-density reference's near coverage is about 7.3% of this projected frame; it is not LAI.
The first eye-level capture looks sparse and repetitive, and is not accepted as a convincing sward.

The complete eight-variant family (32 patches, four densities) then measured these worst absolute
differences across its four views. This broader result supersedes any inference from variant 0:

| Density | Coarse worst (%) | Cards worst (%) | Full container clusters | Card-only form clusters |
|---|---|---|---|---|
| 0.25 | 3.85 | 20.67 | 26 | 3 |
| 0.50 | 3.56 | 13.65 | 46 | 3 |
| 0.75 | 2.95 | 9.27 | 64–67 | 3 |
| 1.00 | 2.01 | 9.79 | 86–87 | 3 |

At a completely occupied 16×16 m tile this is at most 22272 resident source pairs in ring 0,
768 in ring 1 and zero in later rings. These are container-pair bounds, not visible draw counts.
The card forms are now explicitly selected and validated as cards only; the ordinary one-pixel
20 m cut retained 19 clusters at full density and did not meet the representation contract.
Per-frame deformed bytes and the shadow delta still need the frame measurements, not these bounds.

A read-only POC check of `world/v10/understory.json` in the 64 m square x=[-250,-186),
z=[-480,-416), around its grass-clearing camera, found 3624 grass tufts, all marked clearing,
2707 occupied 1 m cells and 976 occupied 2 m cells. Light proxy min/mean/max was
0.0672 / 0.985495 / 1. This counts occupied source cells; it does not claim the new fixture
recreates the POC's density at every texel. No POC file was changed.

The initial 1600×900 offscreen diagnostic used main's CPU motion, CSM and shadow distance 8 m,
while compilation was active. It reported 227161 tail pairs of 16777216 (1.35%), 3486 flora
instances, 1703424 posed parts and 654365808 tail bytes. Its CPU pose mean was 8.33 ms, but
`machine_state` was null and GPU timings were zero: this is **not a quiet performance result**,
nor does tail storage equal visible deformed-pool bytes. It demonstrates why field sway remains
necessary. This v1 diagnostic predates the heading-paired ladder and guaranteed card-only form.

Development pins: section 41 moved from 11938167440745581753 (v1) to 660343445336747083
(v2, three-segment coarse ribbons) to 17839441404926445449 (v3, heading pairing). The eight-variant
geometry/attribute family pin was 3702055350956951526. Version 4 also fixes card roughness to
match blades/ground; the geometry pins remain v3's. Existing kinds retain their old bake defaults.

## Historical v4 static diagnostics and verification

The completed 32-patch meadow was flown at sky hours 12 and 18, wind strength zero, with
CSM and shadow distance 8 m. The sky record reports sun elevations 64.42° and 14.98°;
use `sun.sky`, not the legacy non-sky orbit fields. Eight marker images near 1/5/20/80 m
are in the external `sward/2026-10-25/meadow/captures-sun12` and `captures-sun18` folders.
The near ribbons remain sparse and the far handover is visibly discontinuous; these are
acceptance candidates, not accepted pictures. No 3/8 m/s or travelling-gust sequence is claimed.

Diagnostic stationary 0.5 s path, two repeats, 120 warmup frames and the harness's minimum warmup,
Windows `msvc-debug`, 60 s quiet wait, GPU lock held. GPU medians in milliseconds:

| Resolution | Shadows | Total | Shadow pass | Deform pass | Harness quiet | Other CPU start/end (%) |
|---|---|---|---|---|---|---|
| 1600×900 | off | 0.927 | 0 | 0.489 | false | 8.28 / 11.46 |
| 1600×900 | CSM, 8 m | 1.236 | 0.330 | 0.423 | false | 8.85 / 6.27 |
| 11520×2160 | off | 5.588 | 0 | 0.727 | true | 8.16 / 8.00 |
| 11520×2160 | CSM, 8 m | 8.261 | 2.199 | 0.819 | false | 9.72 / 12.17 |

These include the fixture's trees and terrain. They compare shadows off/on, not grass absent/present,
and the busy CSM runs are upper bounds, not isolated sward costs. In particular, 2.199 ms does not
establish the requested <1 ms shadow increment. The engine's nearest-cascade/rest-pose policy
is still absent. No frame-budget baseline is set from these debug diagnostics.

All four runs had 229125 tail pairs plus 5841 prefix pairs (1.40% of 16777216); visible pairs were
11034 at 1600×900 and 18062 at 11520×2160. CSM shadow pairs were 6114 and 39835 respectively.
CPU motion reported 1703601 parts for 3489 flora instances, and 654388384 tail bytes. The public
flythrough record does not expose actual deformed-pool bytes; tail storage is not that measurement.
Its `frame_loop.last_frame=-1` also means this stationary diagnostic is not the requested meadow
allocation proof with moving tiles and blowing wind. Full machine-state records and raw frames
are retained with each external `bench-*.jsonl`.

Validation: `msvc-debug test -Affected` passed all 122 selected tests (4968.58 s on the shared
machine), including lint/docs, renderer and the existing frame-loop paths. Linux Clang passed
all 25 selected tests (522.59 s), including both new deterministic pins; its two worktree volumes
were pruned. Clang required explicit f32-to-f64 promotions; those preserve the original arithmetic.
The final Windows follow-up rebuilt successfully and passed flora (247.70 s) after that portability-only correction; both deterministic pins remain unchanged.

The walking-height review also calls for layered fans: the authored outer-leaf fraction now
raises horizontal curvature from the inner leaf's 0.2–0.55 toward 0.9–1.25 and vertical droop
from 0 to 0.65 in the normalized quadratic axis. Arc length is still normalized to the source
range. These are appearance choices, not measured species mechanics. Cutting retains the first
upward crossing so an arched tip beyond the cut is removed. Coarse width fitting is limited to
0.75–1.25 of natural width and never modifies near blades. Near/card captures remain necessary:
four-view covered area alone did not reveal the earlier cell seams.

## Engine hand-back for walked tracks

The CPU compatibility fix makes structurally non-woody `sward` roots pushable, including part
zero. It changes no tree-root policy and allocates nothing new per frame. Its existing 12-tick
recovery is useful for contact verification but too short for the requested visible walked path.

The field-kind implementation should treat these as three independent concerns:

- Immediate contact: stamp swept actor capsules/feet in world coordinates and sample the result
  at each blade root. Bend by the section-36 arc weight, keep roots fixed, and apply the same
  deformation to every position reader through the deformed-vertex pool. No per-blade CPU state.
- High-quality visual history: retain direction and flattening in optional tiled storage, with
  authored recovery time and bounded spatial/temporal lifetime. Clear/reinitialize newly mapped
  tiles, handle origin movement and teleport explicitly, and test departure/re-entry. Low quality
  allocates no history field and schedules no history update pass. This is a proposed interface,
  not an implemented allocation or a sourced biological recovery constant.
- Lasting wear: authored/gameplay management state selects shorter/sparser grass and matching
  ground paint. It must not disappear when graphics quality changes. Mown, grazed and worn recipes
  now provide distinct asset states; the transient bend field must not silently rewrite them.

The root cell may have leaf overhang; use actual mesh bounds and do not shrink those bounds to
the placement footprint. Section 41 and the motion sections are unchanged. No new container
section is requested. Final field/caster integration and a terrain walking replay remain open.

The v5 isolated captures deliberately omit terrain, wind and shadows. Dense cover now fills
most of the walking-height picture; the old cell-edge seams are absent. Card forms still
flatten the canopy from elevated angles, even where covered area matches. Mown/grazed outputs
correctly distinguish clipped and uneven tips but remain sparse as standalone lawn geometry
(modeled LAI approximately 0.27/0.30 in these authored samples). A dense short-grass surface
needs further representation work within the draw/streaming budgets; simply claiming the
covered-area fit as visual acceptance would hide both defects. The external comparison page
switches identical near/card cameras among close, walking height and overview.

## Engine hand-back: field sway (R95)

**Done on the engine side, 2026-10-25** (roadmap R95; [renderer: the field kind](../subsystems/renderer.md#the-field-kind),
[the caster policy](../subsystems/renderer.md#the-caster-policy)). The `sward` catalogue row is
`FloraMotionKind::field` with the caster policy `{traced: false, cascades: 1, rest_pose: true}`.
The pool pass bends each blade about its root from a per-frame block (wind, gust field, flutter
clock, walker) and the blade table; no CPU state, no pose table run, no tail record. The walker's
push is the immediate-contact term of the list above, in the same pass with no history: walked-track
history and lasting wear stay open.

**The travelling gust, grounded.** The mixing-layer analogy for plant canopies puts the coherent
canopy-top eddies' streamwise spacing at about eight shear lengths, `Λx ≈ 8 Ls` with
`Ls = U(h)/U'(h)`, and their convection at about `1.8 U(h)`
([Raupach, Finnigan & Brunet 1996](https://doi.org/10.1007/BF00120941); [Finnigan 2000](https://www.annualreviews.org/content/journals/10.1146/annurev.fluid.32.1.519);
the constants as applied in [a study of wind over emergent wetland vegetation, arXiv:1601.07960](https://arxiv.org/abs/1601.07960),
which also derives `Ls ≈ h/α` with `α ≈ 2.8` for an exponential canopy profile and measures
`Ls = 0.58 m` at `h = 1.75 m`). Chosen numbers, and where they were traded:

| Quantity | Value | From |
|---|---|---|
| honami wavelength | 1.4 m | `8 h/2.8` at the owner's knee height, `h = 0.5 m` |
| honami speed | 1.8 m/s | `1.8 U(h)` with `U(h) = 1 m/s` on a mean day: an assumption, not a measurement |
| sweep wavelength | 57 m | the same rule for a 20 m forest edge upwind of a clearing |
| sweep speed | 4.5 m/s | `1.8 U(h)` with `U(h) = 2.5 m/s` at the forest top: an assumption |
| flutter | 1.26 Hz | the honami's own `1.8 / 1.4`, between Py et al.'s alfalfa 1.05 Hz and wheat 2.50 Hz |
| wave pairs | ±20° of the wind, shares 0.2, 0.2, 0.3, 0.3 | authored, so fronts break into patches rather than run as lines |

Lean and flutter amplitudes are the trees' (`0.6 + 0.8 g`, `0.25 + 0.35 g` of the compliance) and
the compliance is Tuft's 0.20: still compatibility values, not a measured blade EI (de Langre 2008,
above). The speeds do not scale with the wind's strength, which would make the phase jump by the
change times the clock; a turning wind re-phases the gust far from the world's origin (the mean lean
does not jump).

## V5 asset measurements, 2026-10-25

Independent 256² four-view coverage measurements of seed 8801, variant 0, after fitting with
128² probes. These are geometry/coverage results, not frame timings. Each isolated stand has
64 copies of its patch, no terrain masking gaps, wind zero and shadows off; raw 1600×900
captures use matching near/card close, walking-height and overview cameras. Camera overview
is at 10 m for inspection, closer than the intended outer-ring card use. The table is these
seven authored recipes, not a claim that every species/density/seed has been accepted.

| State | Rendered blades/m² | Modeled LAI | All-rung triangles | Coarse max difference (%) | Card max difference (%) | Full/card clusters |
|---|---:|---:|---:|---:|---:|---:|
| Dense | 1024 | 3.531 | 8288 | 0.71 | 1.79 | 348 / 3 |
| Dense low | 384 | 1.303 | 2400 | 0.00 | 3.29 | 103 / 3 |
| Mown | 1024 | 0.265 | 10096 | 0.91 | 1.08 | 411 / 3 |
| Grazed | 768 | 0.304 | 7186 | 0.48 | 2.24 | 296 / 3 |
| Trodden | 192 | 0.034 | 1976 | 2.15 | 2.90 | 83 / 3 |
| Clumps | 192 | 0.454 | 1578 | 0.61 | 1.55 | 69 / 2 |
| Tall | 768 | 3.202 | 6240 | 0.50 | 2.14 | 262 / 3 |

Counts include geometric near/coarse rungs and actual nonempty card groups. Budgets conservatively
reserve every possible group and a cut-end triangle for every blade; a blade already below the
cut height stays pointed. The lower actual mown/grazed triangle counts follow from those uncut
leaves, not dropped cuts. Reference raw data and 42 PNGs live in the external
`sward/2026-10-25-v5/final-showcase`, with `comparison.html`, `measurements.json`, individual
recipes and provenance. The low recipe gives up coverage; it is a candidate, not a final quality
policy. The full patch library and ground transitions remain to be visually accepted.

V5 deterministic pins: section41 **8707601967473198806** and eight-variant geometry/attributes/
fitted-cutoff family **2792296357357641801**. Linux Clang reproduced both and passed all 25 selected
tests (649.46 s, 11m43s including build); its two worktree volumes were pruned. The focused CPU
contact regression passed 12 assertions. Windows `msvc-debug test -Affected -Base 84e83f84`
passed all 122 selected tests (3861.44 s), including renderer, content, viewport and frame-loop
checks. Committed content makes the current affected selector choose the whole suite.

Build notes: after the Sward schema grew, MSVC incremental links reported LNK1236 for existing
fern/companion object members in several consumers. Recompiling those objects was not sufficient
for every consumer; clearing only this worktree's `.ilk` files completed the links. No shared
compiler cache was purged and no compiler-cache launcher was enabled here. In the recipe-driven
PowerShell cook, the parameter `$Recipe` and local `$recipe` collide case-insensitively; the
local must use `$swardPatch` so a hashtable is not coerced to the parameter's string type.

The dense terrain family also cooked successfully: two variants at each of 25%, 50%, 75% and
100% density. Across these eight patches, the independent four-view maximum is 1.21% for the
coarse ribbons and 2.82% for cards. Full-density modeled LAI is 3.531/3.422; exact source values
and per-patch counts are retained in external `dense-meadow/family-measurements.json`.

The dense terrain replay completed with raw captures inside the stand, at the 5 m walking view,
20 m and 80 m. Near cover is continuous and the authored trail stays clear. The 80 m image has
an obvious card-to-ground boundary; the shared colour source alone does not make the transition
invisible. This remains a failed visual acceptance item. Captures use the RTX 5090, `msvc-debug`,
1600x900, wind zero, sky hour 12 (sun elevation 64.42 degrees), CSM with 8 m shadow distance as
the temporary stand-in for the absent caster policy. They are not quiet benchmark runs:
`machine_state=null`, `quiet=false`, uncollected GPU timings are zero. Do not interpret those
zeros as measured costs.

The final replay record retains 7,861,937 motion parts and 2,616,732,832 motion-tail bytes;
tail storage is not deformed-pool bytes. Final ring instances are `[0,1310,0]` and pairs
`[0,3930,0]`, with 5841 prefix pairs. These are final-camera values, not peak near-view counts.
They do not establish the pair/shadow budget. `frame_loop.last_frame=-1` likewise supplies no
allocation proof. The dense path still needs the engine's GPU field motion, explicit pool-byte
statistics, quality ladder and quiet frame-budget run. All camera, scene, density, provenance,
PNGs and the complete record are in external `sward/2026-10-25-v5/dense-meadow`.

The owner selected **mostly knee height** for the first tall-grass walkthrough on 2026-10-25.
Use that visual target for the dense/tall variants and subsequent stem/seed-head work, keeping
botanical dimensions within the cited species ranges. Waist/chest-height stands are not the
default acceptance target. This is an art-direction reference, not a new botanical constant.

### Next representation experiment

AMD's [procedural grass example](https://gpuopen.com/learn/mesh_shaders/mesh_shaders-procedural_grass_rendering/)
uses curved blades grouped into patches, reduces blade count while increasing width with distance,
and applies root darkening and spatial colour variation. Its later
[procedural generation sample](https://gpuopen.com/learn/work_graphs_mesh_nodes/work_graphs_mesh_nodes-procedural_generation/)
replaces dense patches with quads for its sparse LOD. These are concrete examples of multiple
representations, not evidence that one technique is universal across AAA games or a performance
prediction for this renderer.

For short turf, the next proposed comparison is a baked dense basal layer plus a smaller set of
geometric leaves, against a higher-count geometric patch, with identical footprint and cameras.
The source lawn used for baking can be denser than the runtime mesh. This is a proposal prompted
by the sparse clipped captures, not an implemented new tier. For tall cards, test vertical fans
and additional depth samples against the current three orthogonal planes: covered area must be
accompanied by elevated-view silhouette and parallax review. Both experiments must retain one
appearance source for near geometry, cards and ground. ComfyUI leaf-material variants can then
be compared through that same bake route; unrelated whole-grass textures would not establish
cross-tier consistency.

## Local-main integration, 2026-10-25

The owner authorized a targeted merge check, with the full matrix deferred to tonight's run.
The combined tree includes main `4a48e749` (tree maturity and rock detail). Main's tree catalogue
IDs remain unchanged and sward is appended after alder; the resolved maturity validation path
also validates sward. Both new test families and registrations remain present.

Independent branches had reserved G88 three times. Tree maturity keeps G88, rock detail is G89,
and sward is G90. Historical external capture labels and commit subjects retain their original
G88 label; the checked-in references now use the unique rows.

The scoped checks use existing controls: debug flora, geometry, scene generation, world,
scene collision, lint and docs; release counterparts plus the frame-loop allocation test,
renderer flora/motion/streamed-tail cases, authored streaming viewport cases, flora/content
canonical determinism cases and a live sward cook; one filtered Linux Clang run. No global gate
policy is changed. The unrelated Windows suites, remote GPU matrix and full frame-budget set
are deferred at the owner's request. Results are recorded when these integration checks finish.

Integrated debug checks: flora, geometry, scene_gen, world, scene_collision and both lint checks
passed (the eight-entry run took 470.92 s). Its docs check found three old G88 heading anchors;
those were corrected and `dev.ps1 docs` then passed (256 pages, 5753 links). Both sward pins
remain unchanged on the combined tree. No code fix was needed after conflict resolution.

Integrated release checks passed: 9/9 CTest entries including frame-loop allocations (169.81 s),
20 focused renderer cases / 1999 assertions, 4 authored-streaming viewport cases / 82 assertions,
and 9 flora/content determinism cases / 578 assertions. Cases outside those source-file filters
were deliberately not selected. The live release dense cook passed and retained source hash
2250113272344848541, 348 full clusters and 8288 all-rung triangles.

Linux Clang passed all 9 selected integration entries (358.75 s; 6m29s including build).
Both deterministic sward pins remain unchanged. The persistent gate's volumes remain warm.
The tested code is integration commit `51ef062a`; the follow-up changes only this record and
roadmap status. Raw results and the reproducible release harness are retained under external
`sward/2026-10-25-v5/integration`. This focused gate does not close the open grass visual or
performance acceptance items, nor claim the deferred full matrix has run on this tip.
