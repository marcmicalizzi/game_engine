# Rock lithology and weathering

G94, 2026-10-28. Extends [G89](rock-structure.md). The owner's photographs are visual targets,
not measurements or reliable lithology identifications. Existing procedural geometry may change.

## Grounding before design

- [NPS, Granitic Weathering](https://www.nps.gov/places/geological-trail-station-1.htm):
  granular disintegration, exfoliation and rounded exposed surfaces can coexist in granite.
- [BGS, Relief](https://www.bgs.ac.uk/discovering-geology/geological-processes/relief/):
  sandstone and limestone can be resistant rocks. Neither name means universally soft.
- [Hong Kong Geological Survey, Minerals and Rocks](https://hkss.cedd.gov.hk/hkss/en/publications-and-resources/educational-materials/hong-kong-geology/chapter-1/index.html):
  origin divides rocks into three families; foliation follows aligned minerals, and metamorphic
  rocks can also be massive. Slate, schist and gneiss need different fabrics.
- [USGS, Acid Rain](https://pubs.usgs.gov/gip/acidrain/3.html): calcite in limestone and marble
  dissolves in weak acid. Dissolution is distinct from abrasion and granular disintegration.
- [NPS, Obsidian](https://www.nps.gov/articles/archeology-obsidian-mvp-yellowstone-stones.htm):
  glass has conchoidal fractures and sharp flaked edges, unlike crystalline joint blocks.

These sources support the qualitative processes, not universal numerical constants. All preset
dimensions, strengths and colours below are authored approximations for metre-scale assets.
Weathering is a normalized accumulated exposure, not years: climate, cementation, transport
and exposure history prevent a universal age-to-shape conversion. No destruction simulation is
implied. Destruction and terrain settling remain separate capabilities.

## Design and budget

Use one shared geological fabric for boulders, outcrops, scree and cliff faces. Rock types select
process mixtures; recipe overrides allow multiple processes on one specimen. Preserve the existing
closed radial-shell / closed cliff-sheet topology while adding irregular scars and differential
retreat. Features fade by their width relative to the source mesh cell; runtime cluster LOD remains
the renderer's. Low and medium build tiers reduce source triangle budgets; high retains the authored
budget. No extra runtime pass, shader or per-instance state is planned.

Cost is additional offline field evaluation per source vertex; source triangles remain bounded by
the recipe, runtime cost stays the selected cluster cut plus ordinary PBR shading. The offline
`RockGeology` recipe record is 56 bytes, alignment 4; no GPU-mirrored or runtime instance type grows.

The selected physical bed/cleavage spacings (metres) are sandstone 0.32, limestone 0.55, shale 0.09,
slate 0.12, schist 0.16 and gneiss 0.28; the other types default to no fabric spacing. They are
metre-scale examples, not bounds on those rocks in nature. Fold weights are respectively 0.02,
0.02, 0.04, 0.02, 0.3 and 0.65. Fold wavelength is twelve bed spacings plus 0.5 m, with a second
harmonic at 28% amplitude. This economical field describes a folded pattern, not a tectonic model.

Granular loss has three octaves starting at 0.28 m with half wavelength and 45% amplitude per
octave, maximum first amplitude 0.022 m. Solution pockets use 0.32–0.82 m wavelengths with up to
32% of wavelength recession before the body's safety clamp. These engineering scales intentionally
leave mineral grains, pore detail and composition to PBR maps. Geological fields fade between two
and four cells; bed risers span at least two cells. The radial safety clamp is 35% inward, 5% outward.
Six broad asymmetric scar bowls per body remain on coarse rungs. Old joints use the weathering
rounding, new facets 2–18% of it; a bowl can be smooth while its lip stays narrow.

## Visual review and remaining work

Local captures and scripts are under
`D:\workspace\game_engine_local\rocks\2026-10-28\lithology` (not committed). The review uses
the current debug engine, offscreen 1200 × 900, finest cut, sun 45°/45°, CSM, under the GPU lock.
It separates untextured geometry from one PBR wiring check using the owner's existing local
sandstone base-colour/normal/roughness set. That base-colour image is strongly yellow; the result
is not a material-quality reference and was not colour-corrected into an acceptance image.

The eight new studies distinguish thin slate, folded gneiss, scaly schist, smooth dark obsidian
scars, dissolved limestone, shale rubble, marble and a folded cliff. The first flaking field had
an abrupt per-bed sawtooth reset, visible as small jagged grooves; interpolating the flake retreat
with the adjoining sheet removes that discontinuity. Strong folded cliff cases also exposed
inward smooth normals at creases; those faces now split normals without moving positions or UVs.

This is a broader geological basis, **not photorealistic acceptance**. Exaggerated folds can still
look periodic, shallow chips are recesses rather than detached flakes, the radial shell cannot
make arbitrary overhangs, and the cliff's back/skirt ends still need burial in terrain. Fine
composition, veins and weathered-versus-fresh material masks are absent. Existing PBR textures
work, including recipe tint and roughness factors, but are not aligned to the geological field.
UV chart stretching/seams still need attention before close-up hero assets. Destruction will need
an interior and a separate fracture representation; none is implied by these surface controls.

The six high-detail boulder studies use 150,528 triangles each except marble (110,592); shale
rubble uses 186,432 across 24 pieces; the folded cliff uses 157,832 across eight closed components.
The low/medium/high source-budget multipliers are 10%/35%/100%, with topology floors. Runtime LOD
comparison is recorded separately from these source counts.

`check-controls.ps1` and the local `review.html` compare the same gneiss seed/camera at all three
source qualities, drawn at the finest runtime cut: 12,288 / 49,152 / 150,528 triangles. The broad
silhouette remains recognizable while narrow bedding and relief fade. A granite pair differs only
in exposure 0 versus 1: older joint margins soften while the fresh cut margins remain. These are
visual controls, not measurements of erosion rate or a guarantee of identical bounds across tiers.

## Runtime LOD evidence

`tools/e10-harness.ps1`, current debug binaries from the dirty G94 tree based on `832e9acb`,
RTX 5090, 2026-10-28; fresh binary checks passed. Fourteen exported GLBs were framed whole at
640 × 640, orbit 20.8, four frames, shadows off, offscreen through engine-host, original UVs kept.
The standard 1 px cut was compared with 0.05 px. All fourteen imported with zero warnings or
zero-area/UV repairs, valid atlas islands, and passed the collapse criterion. Twelve passed the
0.02 mean FLIP image-error limit. Two exceeded it:

| Asset | FLIP at 1 px | Visible pairs at 1 px / finest | FLIP at 0.5 px | Visible pairs at 0.5 px / finest |
|---|---|---|---|---|
| jointed-pinnacles | 0.02097 | 470 / 2,453 (19%) | 0.01258 | 891 / 2,453 (36%) |
| limestone-dissolved | 0.02223 | 251 / 1,220 (21%) | 0.01419 | 458 / 1,220 (38%) |

The follow-up changed only the runtime comparison cut to 0.5 px, retaining every acceptance
threshold; both passed all criteria while still shedding over 60% of visible pairs. This is a
quality/cost option, **not a pass at the default 1 px**, nor an automatic per-rock LOD policy.
Use the finer renderer cut for these detailed hero surfaces until geometric/material LOD can
preserve their shading more efficiently. The measurements cover one view per asset, not every
distance, light or motion transition. The low-coverage cliff/rubble studies have unreliable
object-normalized FLIP, as the harness reports; the quoted figures are whole-image means.

Raw records: local `lithology/e10/report.json` and `e10-half-pixel/report.json`, their Markdown
tables, binary copies, captures and FLIP heat maps. No threshold was relaxed and no failure was
removed from the standard report. The reports contain elapsed build times taken on a busy machine;
they are not quiet performance measurements.

## Machine state

The workstation is shared with other builds and GPU work. These captures establish appearance and
the tests establish topology/counts; no quiet before/after performance claim is made.

## Code validation

The rock tests cover all ten lithologies at all three source tiers; finite closed shells,
outward normals, connected part counts and triangle budgets; tilted/folded and extreme process
mixtures; group topology floors; invalid enums and NaNs; and PBR scalar factors surviving textured
container construction. Fifteen committed recipes and the default container have deterministic
fingerprints. MSVC and Linux Clang reproduce the same pins. `RockGeology` is pinned at 56 bytes,
alignment 4, for the offline recipe contract.

Linux Clang debug: **121/121 CTest entries passed** on 2026-10-28; this checkout's two container
volumes were pruned afterwards (3.71 GB). Its GPU-dependent cases may skip internally without a
hardware device; desktop GPU coverage comes from Windows and the E10 runs.

Windows MSVC debug: **122/122 CTest entries passed**, including all 31 rock test cases, renderer,
host, viewer, editor, content-build and frame-loop allocation checks. The affected selector includes
the full debug suite because committed content recipes are inputs to scene and application tests.
The shared-machine run took 5,192.98 seconds with concurrent builds and GPU users; that elapsed time
is not a solver-performance measurement. Documentation/link checks and `git diff --check` passed.
Release, sanitizer, remote-server and frame-budget merge gates were not repeated for this branch.
