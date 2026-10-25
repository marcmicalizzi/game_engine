# Grass sward: research and acceptance (G88)

Reserved on 2026-10-25 for the sward solver: grass as ground coverage, with blade
patches near the eye, clump cards beyond them, and the ground material far away.
This is new work; G75's isolated tufts remain complete and unchanged.

The solver builds on W72's ground masks and W73's streamed placements. Field sway
and caster policies are separately briefed engine work. The owner confirmed on
2026-10-25 that existing cooked paint and distance forms cover the other seams.

Research, implementation and acceptance are in progress. Measurements below distinguish
the passing reference patch from the unfinished family. The owner's visual acceptance remains required.

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

Current ladder: 256 blades/m² × 5 triangles near, half as many × 5 coarse with width scaled by
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

## Measurements and unresolved acceptance, 2026-10-25

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
geometry/attribute family pin is 3702055350956951526. Version 4 also fixes card roughness to
match blades/ground; the geometry pins remain v3's. Existing kinds retain their old bake defaults.

## Static diagnostics and verification

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
