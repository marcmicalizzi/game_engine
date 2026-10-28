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

## G95: angular fracture shapes (2026-10-28)

G94 was fast-forwarded into local main at the owner's request, with merge gates explicitly
waived. This follow-up remains in the rock worktree. The owner also requested continued improvement.

The [Hong Kong Geological Survey's weathering chapter](https://hkss.cedd.gov.hk/hkss/en/publications-and-resources/educational-materials/hong-kong-geology/chapter-4/index.html)
describes joint-bounded blocks, preferential corner/edge weathering, and curved exfoliation shells.
[NPS, Square Blocks to Round Rocks](https://home.nps.gov/articles/000/how-do-square-blocks-become-round-rocks-on-mount-desert-island.htm)
describes angular granite debris becoming rounded through shoreline abrasion. Together with the
obsidian source above, these support different fracture surfaces and subsequent edge wear, rather
than a spherical starting shape and the same bowl-shaped damage on every lithology.

Design: replace the residual unit ball with six enclosing oblique support planes; existing joint
sets and fresh cuts intersect that hull. Use bounded four-plane chips for crystalline and bedded
rock, keeping conchoidal bowls proportional to the lithology's glassy-fracture weight. The chip's
central plane and three side planes describe removal of a finite patch, not another infinite cut
through the whole body. Positive radial distances retain the closed-shell topology. Weathering
softens intersections; unresolved chips fade by source cell width. Sixteen seeded chips are a
representation budget, not a claim about fracture counts in nature. No extra source triangles,
runtime state, shader pass, or destruction simulation is introduced. Numerical widths, depths and
angles are authored approximations and will be checked in paired engine captures.

The six support normals have a dominant signed axis of 1 and other components in [-0.2, 0.2],
then normalize; distances are 0.82–1.08 in the unit body. This makes the hull bounded even with
zero joint sets. Chip widths are 0.12–0.35 unit lengths; depths are 0.035–0.13 times irregularity,
the flaking mixture, and reduced glassy weight. The side planes meet deeper than the central face
so a chip has a flat floor and oblique walls. The lip uses 12% of the old-joint smoothing band.
These constants are shape controls, not laboratory fracture measurements.

The four paired 1200 × 900 captures in the local `fractures/` directory use the same recipes,
camera, lighting and finest cut before/after; the camera is closer than the G94 gallery. Granite
and slate have straighter broken outlines and planar removal faces, while obsidian retains curved
scars. The geometry still has shallow surface relief and some grid-shaped rim artifacts. This is
an incremental silhouette improvement, not a photorealistic or detached-flake acceptance.

G95's focused E10 run: all four exports pass at the standard 1 px versus 0.05 px comparison,
with the same 0.02 mean FLIP limit: granite 0.01298, limestone 0.01945, obsidian 0.01069,
slate 0.00914. All import without warnings or repairs and pass the collapse/UV criteria. This
does not revalidate the ten other G94 exports. RTX 5090, current dirty G95 debug binaries based
on `5377ad30`; offscreen 640-square whole-asset framing, one view each. The harness recorded
the GPU lock free at both ends but no machine-state sample; no timing claim is made. Raw records
are in local `fractures/e10/report.json`. Source triangles remain 76,800 for granite and 150,528
for the other three, identical to G94.

Focused validation includes 33 rock test cases, with a new geometric planarity check (over 75%
of a fresh unjointed basalt block's area in eight normal groups within ten degrees), plus ten
lithologies at three seeds, zero joint sets, full chip strength and two source quality levels.
The first run's only runtime failures were the intentionally obsolete mesh/container fingerprints;
all topology checks passed. New pins are version 4. The full affected suites and merge gates are
not repeated for this iteration, following the owner's request to skip gates.

Final focused results: all **33/33 rock cases pass on MSVC debug and Linux Clang debug**, including
the new version-4 fingerprints; lint, licence and documentation/link checks pass. Clang initially
rejected three implicit float-to-double promotions in the new area-measurement test; explicit
casts fixed those warnings without changing solver output. The Windows rock test took 89.89 s
and Linux 64.11 s on the shared machine; neither is a quiet performance measurement. Offline
work increases by six support-plane evaluations and up to sixteen four-plane chips per vertex;
the source triangle budgets and runtime data layouts do not grow. No runtime performance claim
or full-scene acceptance is made. This checkout's Linux volumes are pruned after validation.

The fresh-block planarity measurement is 0.933778 (93.38% of surface area), above the 75%
regression threshold. This measures face structure only, not photographic realism.

## G96: geological surface maps

The [Polish Geological Institute's vein quartz account](https://www.pgi.gov.pl/en/1263-surowce/resources/rock/14077-vein-quartz.html)
describes mineral infills cutting rock masses, with variable thickness and orientation.
[BGS's gneiss classification](https://webapps.bgs.ac.uk/bgsrcs/rcs_details.cfm?code=GNSS)
describes irregular or discontinuous foliation with contrasting mineralogy between layers.
Read 2026-10-28. These motivate separate cross-cutting vein and folded compositional-band
fields, rather than treating every stripe as bedding. They do not supply PBR constants.

This pass bakes authored approximations into ordinary base-colour, metallic-roughness and
tangent-normal PNGs. The fabric coordinate is shared with geometry; the vein network is a
separate warped plane family. Colours, widths, grain scales, roughness and relief are artistic
controls, not measured specimens or deposit predictions. Metallic stays zero for these
dielectric rocks. No lighting, shadow or ambient occlusion is painted into base colour.

Scope: radial boulders, outcrops and scree. Each body has six padded atlas charts; one total
texture-size budget covers the entire recipe. Cliff face/talus atlasing and fresh-break masks
are deferred. Procedural maps and external tiled maps are mutually exclusive because their
UV layouts differ. The existing renderer, texture mips and cluster LOD consume the baked maps.

The implementation uses 256/512/1024/2048 square atlases independently of source triangle
quality. Each body gets six charts with four-texel padding; fewer than sixteen interior texels
per chart is refused. Unused cells and division remainders duplicate filled texels so distant
mips do not mix in black. A 2048 bake holds 36 MiB of RGB pixels for its three maps, plus encoded
images and mesh temporaries; runtime decoded/compressed memory is a separate cost. No runtime
type, shader pass or source triangle count grows. The GLB writer now carries these embedded PNGs
and material references as well as the container path.

Review artifacts are under the owner's local `rocks/2026-10-28/surfaces/`: `review.html`, three
paired engine captures, standalone GLBs, camera paths and reproduction scripts. The committed
recipes are `gneiss-veined`, `granite-minerals` and `sandstone-grain`; each uses 2048 maps and
the same geometry as its flat-material predecessor. The first 1024 studies were soft at close
range. A sharpened unwarped value field then printed square grain checks on granite; rotation,
domain warping and lower pigment contrast removed that obvious lattice. Final grain remains
mottled and synthetic in places, and radial-grid rims and overly smooth transitions remain.
These are isolated single-light studies, not photorealistic acceptance or a full-scene benchmark.

Focused final validation: **38/38 rock cases on MSVC debug and Linux Clang debug**, including
embedded-image determinism, dielectric channels, atlas corner continuity, unchanged positions
and topology, both grouped shapes, pack precedence, input refusals and padding. **4/4 rock CLI
cases (70 assertions)** pass on Windows, including a GLB export/reimport with all three map slots
and no import warning. Lint, license, documentation links and diff whitespace checks pass.
Full affected suites and merge gates remain waived by the owner for this iteration. The only
initial test failure after compilation was the expected version-5 container fingerprint; the
version-4 geometry fingerprints remain unchanged. This checkout's Linux volumes were pruned
(3.71 GB). The shared-machine test times are not performance measurements.

The final three GLB exports pass all E10 criteria at the standard **1 px versus 0.05 px**
comparison: gneiss FLIP **0.01491**, granite **0.01422**, sandstone **0.01564** (limit 0.02).
All import with zero warnings/repairs and six UV islands and pass the LOD-collapse criterion.
Source triangles are 150,528 / 76,800 / 110,592 respectively, unchanged from their flat versions.
RTX 5090, debug binaries based on `5a8c001b` plus this G96 change, 640-square whole-asset framing,
one view each, under `gpu-lock.ps1 run`. Raw records: local `surfaces/e10/report.json`.
This checks simplification of these three textured studies, not the other recipes or every view.
