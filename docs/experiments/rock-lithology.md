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

## G97: fresh breaks and weathered exteriors

The [USGS fresh-rock field account](https://www.usgs.gov/observatories/calvo/news/when-it-comes-analyzing-rocks-only-freshest-samples-will-do)
shows both lighter fresh interiors and a lighter altered rind surrounding a darker core: there
is no universal brightness ordering. [NPS's rock-coating account](https://www.nps.gov/articles/desertvarnish.htm)
distinguishes thin exterior coatings from the underlying rock, describes varied dark and pale
coatings, and identifies chipping as a way to expose the substrate. Read 2026-10-28. These
sources motivate explicit exterior colour and roughness controls, not a claim that every old
surface is dark, rough, or covered in desert varnish.

The optional `surface_maps.rind` is an authored coating approximation. Its default brown tint
(0.18, 0.12, 0.07 linear RGB), roughness 0.9, amount 0.8 and grain multiplier 0.6 are artistic
starting values, not measurements. Coverage is `amount * geology.weathering * (1 - fresh)`.
The fresh mask follows the same ownership weights as the hull's smooth minimum: support and
joint planes start old, low-wear facets expose fresh material, bounded chips replace that
provenance at the same resolution fade used for geometry, and nonzero conchoidal scars expose
their bowls. The mask is welded before chart duplication and interpolated on the same triangles
as the material bake. It never invents separate mineral coordinates on a fracture.

The coating blends base colour and roughness and scales grain normals. It introduces no fake
height edge, lighting or AO into the maps. No runtime mask, extra texture, triangle, or shader
is added. Temporary masks cost one double per welded point and chart vertex while baking a
body. A zero amount, zero exposure or fully fresh surface gives the previous uncoated response.
This is not geological chronology: joint faces may also be fresh in nature, and later flaking,
dissolution and granular recession are not individually classified by this mask. It is not a
volumetric weathering rind or a destruction interior. Those remain separate work.

Two new committed recipes, `granite-rind` and `obsidian-rind`, demonstrate opposite brightness
orderings with explicit colours and roughness. Local `rocks/2026-10-28/rinds/review.html` shows
paired uncoated/coated renders, produced by `capture.ps1` from identical geometry, lighting and
camera within each pair. The 2048 maps and triangle counts (76,800 / 150,528) are unchanged
between each pair. Both preserve exposed mineral fields; the granite study still looks mottled
and overly smooth, and the obsidian boundary sometimes reveals radial sampling. This is a
material capability study, not photographic acceptance. All captures use the RTX 5090, 1200x900,
finest clusters, CSM and a 45/45 sun; machine state was not sampled, so no timing claim is made.

The first Windows run passed every new case and geometry fingerprint; its only failure was
the intentionally obsolete container key. Version 6 changes that key to `d849ea7d16afcd40`;
the mesh stays `b1774f2e4eb1d414`. An incremental rebuild then hit Windows LNK1168 because the
same checkout's engine-view was still doing the paired captures. Finish captures before
relinking that executable. The focused final build is retried after the captures exit.

Final validation: **41/41 rock cases pass on MSVC debug and Linux Clang debug** (4,349,577
Windows assertions), including fresh-substrate preservation, unchanged geometry, actual mixed
provenance for granite/basalt/obsidian, zero amount/exposure, coated chart corners and invalid
rind input. **4/4 Windows rock CLI cases pass (70 assertions)**. Lint, licensing and documentation
checks pass. Full affected suites and merge gates remain waived by the owner. No runtime hot
type grows. This checkout's Linux volumes were pruned (3.71 GB).

Both exports pass the standard E10 **1 px versus 0.05 px** comparison: granite FLIP **0.01538**,
obsidian **0.01427**, limit 0.02, zero import warnings/repairs and six UV islands each. Both
pass collapse criteria. These are 640-square whole-asset views, one per rock, under the GPU
lock; the report records no quiet machine-state sample and supports no performance claim.
Raw records and reproduced binaries are in local `rinds/e10/`. This does not qualify arbitrary
angles, lower atlas resolutions or all existing recipes for photographic realism.

## G98: fracture rims and sampling

[NPS's obsidian account](https://www.nps.gov/articles/archeology-obsidian-mvp-yellowstone-stones.htm)
describes smooth glassy fracture surfaces and sharp flaked margins. This motivates preserving
both smooth faces and sharp transitions rather than globally faceting or rounding the mesh.
[Kobbelt and Botsch, Feature Sensitive Mesh Processing (2003)](https://cg.cs.tu-dortmund.de/publications/2003-features.pdf),
sections 2–3, explains why a regular grid crossing a sharp feature produces normal artifacts
that extra subdivision alone does not eliminate. Read 2026-10-28. Its lesson here is to spend
existing samples on features and preserve normal discontinuities; this implementation is not
the paper's extended marching cubes or a complete feature reconstruction algorithm.

The bounded offline pass aligns radial samples near sharp hull-plane intersections, selects
cell diagonals against the generating surface, and splits shading normals only where adjacent
surface directions differ sharply. Seeded sub-cell offsets reduce regular rows away from rims.
Any relocation that would invert or excessively compress an incident directional triangle is
refused. Rounded blends remain rounded; the feature-fit band fades when the geometric rounding
is wide enough to resolve. Numerical bounds and shading thresholds are authored discretization
choices, not geological measurements. No additional triangles or runtime material channels are
introduced. Radial topology, UV charts and source quality budgets remain the existing contracts.

The sample offset is at most 0.1 cell per free cube coordinate. Rim projection is limited to
0.45 angular cell, fades by the rounded band relative to 0.4 cell, and retains at least 20%
of each initial incident directional triangle's area. Diagonal alternatives require each
triangle to exceed 5% of the directional cell area; otherwise the safe initial connectivity
is retained. Normal filtering uses cosine 0.8 (about 37 degrees) on fresh/unweathered faces,
falling to 0.5 (60 degrees) on fully weathered old faces. All are discretization choices.
Only hull-plane intersections receive explicit fitting; chip and bowl boundaries and later
surface recession still depend on sampling. The charts remain structured and a full feature
remesher would be needed to remove that limitation.

Paired 1200-by-900 finest-cut captures at identical cameras and light are in local
`D:/workspace/game_engine_local/rocks/2026-10-28/sampling/review.html`, alongside the exports,
camera paths and raw records. Obsidian and slate use scalar PBR; granite uses the baked rind.
Obsidian/slate breaks are sharper, granite changes more subtly, and patterned furrows remain.
These images do not establish photographic realism. The existing fresh-block test's area in
eight broad normal patches rises from 93.38% to 99.26%; this is a planar-face diagnostic, not a
realism score. A new sharp-edge test finds distinct shading normals on all 398 geometric rims
turning over 60 degrees, while checking closure, unit normals and the triangle budget.

| Study | Triangles, unchanged | Export vertices before / after | E10 FLIP, 1 px vs 0.05 px | Coarse / finest visible pairs |
|---|---:|---:|---:|---:|
| Granite rind | 76,800 | 40,461 / 62,150 | 0.01450 | 307 / 612 |
| Obsidian fractured | 150,528 | 76,836 / 95,550 | 0.00840 | 94 / 431 |
| Slate slab | 150,528 | 77,277 / 92,885 | 0.00704 | 127 / 777 |

Crease attributes are not free: these exports add 20–54% vertices. Temporary per-body adjacency,
directions and face normals are O(surface samples), with no runtime pass or new GPU structure.
All three standard E10 studies pass the 0.02 FLIP and collapse limits, with zero warnings or
repairs and six UV islands. E10 uses 640-square whole-asset views with shadows off under the
GPU lock on the RTX 5090. Its `machine_state` is unsampled; build timings are shared-machine
upper bounds, not performance claims. The copied binaries and report are under `sampling/e10/`.

Validation: 42 rock cases / 5,660,533 assertions pass on MSVC debug and Linux Clang debug,
including the intentionally updated v7 mesh/container fingerprints; four CLI cases / 70
assertions pass. The baker regression checks analytical barycentric fracture interpolation
on both diagonal orientations. Lint, documentation and whitespace checks pass. Linux checkout
volumes were pruned. Full affected suites and merge gates were not repeated under the owner's
explicit gate waiver; this follow-up remains on `codex/rock-structure`, not merged or pushed.

## G99: exposed chip edges and unequal beds

The owner requested these remaining two improvements followed by a local merge with full
gates deferred to the combined nightly run. Re-read 2026-10-28:
[Kobbelt and Botsch](https://cg.cs.tu-dortmund.de/publications/2003-features.pdf), sections 2–3,
on aligning samples with surface discontinuities, and the
[Hong Kong Geological Survey](https://hkss.cedd.gov.hk/hkss/en/publications-and-resources/educational-materials/hong-kong-geology/chapter-4/index.html)
on preferential weathering along discontinuities and at block corners. The
[NPS differential-erosion example](https://www.nps.gov/places/differential-erosion.htm) describes
harder sandstone layers forming shelves as softer surrounding material retreats. These support
coherent layers with unequal resistance and exposed fracture boundaries. The procedural station
distribution below is an authoring approximation, not measured stratigraphy or a metamorphic
deformation simulation.

Solver v8 follows the minimum of enclosing cuts and the maximum of each finite chip's four
planes. It fits both chip-to-exterior boundaries and creases within the winning chip, checking
the projected point against the visible envelope to reject hidden planes. Partially faded chips
retain regular sampling. G98's movement bound, area guard, diagonal selection and normal filter
remain; no extra triangles, material channels or runtime pass are introduced.

The zero-rounding basalt stress capture exposed why positional fitting alone is insufficient:
some snapped vertices sharpened the chip but increased tilted-triangle shading nearby. A move
at a chip boundary now also has to reduce the incident area-weighted normal disagreement with the unsmoothed
piecewise-planar envelope (before warp/weathering). This rejects locally harmful fits. It adds
offline envelope evaluations and one surface-sized position array. It does not claim that a
fixed chart becomes an exact fracture graph; extremely sharp zero-rounding rims can still show
sampled steps, and curved scars remain smooth sampled transition bands.

Fabric stations receive a seeded offset of up to 0.2 authored beds plus continuous lateral
wandering up to 0.08 beds, both scaled by `irregularity`. Consecutive stations therefore remain
ordered with thickness between 0.44 and 1.56 authored beds. Mapping the enclosing interval back
to its canonical bed index is continuous; geometry and PBR use the same mapping, preserving
their alignment. Zero irregularity retains regular stations; zero bed spacing remains unbedded.
Riser width uses the local thickness and detail fading uses the minimum possible thickness,
so the lower source rungs do not carry unresolved thin layers. No controls or schema fields are
added. These numerical bounds are chosen discretization limits, not rock constants.

The initial run also caught a real coarse-cliff regression: sub-cell station offsets entered
the macro block coordinate even when surface relief had faded out, reducing usable culling
cones below the existing test's threshold. Macro-coordinate station variation now fades at
the same minimum-width resolution limit. Applying the normal safeguard to ordinary hull rims
also degraded coarse-fragment culling, so it is restricted to chip boundaries. The unchanged
cone test passes; broad folding stays.

The accepted comparison set is in the owner's local
`D:/workspace/game_engine_local/rocks/2026-10-28/chips/review.html`, with the final E10 report
under `chips/e10-final/`. Before is solver v7; after is the v8 working tree built over
`e7204397`. Matching 1200x900 captures isolate geometry; the granite rind also exercises the
existing material export. These are diagnostic views, not a photorealistic scene benchmark.

| Asset | Triangles (unchanged) | Vertices v7 / v8 | E10 coarse / finest pairs | Whole-frame FLIP |
|---|---:|---:|---:|---:|
| Basalt chip stress | 76,800 | 51,641 / 53,533 | 103 / 375 | 0.00592 |
| Granite rind | 76,800 | 62,150 / 62,150 | 297 / 611 | 0.01401 |
| Slate slab | 150,528 | 92,885 / 91,852 | 73 / 560 | 0.00614 |

All three pass E10 with zero warnings, zero zero-area/UV repairs and six UV islands. Vertex
counts change because crease-normal splitting follows the new geometry; triangle budgets do
not change. E10 uses 640-square whole-asset views with shadows off under the GPU lock on the
RTX 5090. Its `machine_state` is unsampled; this run makes no offline-build performance claim.
Slate coverage is 12.3%, so its object-normalized FLIP is not a reliable metric; use the
whole-frame comparison and heat map. The zero-rounding diagnostic still shows sampled steps,
so this closes the bounded chip-fitting and unequal-bed work, not exact remeshing of every rim.

Validation: all 43 rock cases pass on MSVC debug and Linux Clang debug, including v8 geometry
and container fingerprints, mesh closure, source budgets, coarse-cliff culling and the new
ordered-bed continuity regression. Four exporter cases / 70 assertions pass. Formatting,
lint, documentation and whitespace checks pass; this checkout's Linux volumes were pruned.
The owner explicitly waived full affected suites and merge gates for this local merge, with
those deferred to the combined nightly run. No remote push is part of this change.

## G117: terrain contact under cliff talus

The 2026-10-29 report points at the forest v20 capture: exposed horizontal shelves under the
cliffs. Inspection confirmed that the apron used the line's foot height for its toe and a
flat underside one metre lower, regardless of the descending terrain outside the line.
The generator measured ground along the cliff foot but serialized no supporting terrain.
This is a missing contact input, separate from broad face panels and repeating textures.

Grounding: the [BGS talus definition](https://webapps.bgs.ac.uk/lexicon/lexicon.cfm?pub=TALU)
(read 2026-10-29) describes angular debris accumulating in irregular heaps/sheets at the foot
of slopes through gravity and weathering. It supports a terrain-supported apron, not a
suspended plate. This repair does not simulate rockfall transport or infer a stable slope;
the existing authored talus angle controls nominal extent, not the final terrain-draped slope.

The generator now performs a preliminary solve to measure the debris footprint, samples
terrain in the recipe's local frame, and serializes `CliffGround` in the derived recipe.
Sampling starts at half the tracing spacing, capped at 1 m; a one-cell margin protects the
footprint. Spacing doubles only when needed to stay within the explicit 2049 points per axis
and 1,048,576 total sample limits, refusing beyond 32 m. These are bounded offline authoring
costs, not runtime allocations. Heights are bilinearly interpolated; no terrain is modified.

The outer apron progressively takes the terrain height difference with a smooth weight:
zero at its attachment to the face, one at the toe. The toe remains buried by the existing
half-burial plus 0.3 m offset. Every underside vertex is at least 1 m below sampled ground
and the nominal top; fallen blocks receive the same contact correction. These offsets are
engineering overlap margins, not measured deposit thicknesses. Triangle budgets are unchanged.
Missing ground retains standalone recipes' old reference-plane behavior. Invalid samples or
an insufficient patch are refused rather than silently extending its border heights.

Solver v9 invalidates prior generated cliff caches. Soil v2 acknowledges the new pinned rock
dependency; embedded radial stones are unchanged and their geometry pins must remain unchanged.
Terrain contact is accurate to the sampled patch and apron mesh; sub-cell ravines, exact
terrain stitching and self-intersections on tightly bent cliff lines are not solved here.

Validation: 44 rock cases / 5,738,957 assertions on MSVC debug, and the rock suite on Linux
Clang debug pass. The new falling-slope regression checks every apron underside/toe index at
Low and High quality, closedness, unchanged triangle budgets, cache sensitivity to terrain and
refusal of incomplete patches. Seven soil cases / 224,626 assertions pass on both compilers;
only its version metadata/content hashes changed, with all geometry and cooked-terrain pins
retained. Five exporter cases / 78 assertions pass. Full affected suites/merge gates were not
run in this focused follow-up.

The local review lives at `D:/workspace/game_engine_local/rocks/2026-10-29/terrain-contact/`:
`scene.json` retains frozen v20 terrain, sky, water dependencies and the three cliff generators;
unrelated instances and placements are removed. Seven cliff recipes acquired 18,901 samples
total, 1,608–3,444 per patch, all at 1 m. Matching 1600x900 cameras, HW visibility and RT shadows
show the floating shelves before and ground-supported aprons after. Before uses the pinned
v20 Release executable (4726acd7); after uses the current branch Debug build over 1a3de3ea.
This is a geometric diagnostic across those builds, not a pixel-equivalence or performance test.
Machine state: shared machine, RTX 5090, GPU lock held; no quiet timing measurement.

The first material pack from the frozen replay was refused because unrelated birch map paths
were absent. Scalar captures are retained as `*-scalar.png`; the textured comparison uses a
local two-role pack with the retained granite/basalt maps and their original provenance/hashes.
No frozen source or POC launcher was changed. The broad rectangular cliff faces, exposed end
walls and repeating material pattern remain visible and require further work; G117 only closes
the missing terrain-contact input. Existing cached recipes need regeneration by the new
generator, or an explicit `fit_cliff_ground` bake, to acquire supporting terrain.

## G118: the v20 waxy surface diagnosis

The retained forest v20 geology review shows gneiss variants 03, 09 and 10. A reduced copy
keeps those exact instances, transforms, terrain, daylight and camera and removes vegetation
and unrelated placements. It reproduces the broad, soft surfaces. The immutable v20 source
and the POC launchers are not edited. Separate granite captures isolate the surface bake.
The default granite roughness is already .75 and gneiss .72; simply raising these would not
restore missing resolved surface normals.

Grounding (read 2026-10-29): PBRT 4e's
[microfacet treatment](https://pbr-book.org/4ed/Reflection_Models/Roughness_Using_Microfacet_Theory)
separates the aggregate scattering of unresolved microgeometry from resolved shape. Its
[texture sampling treatment](https://pbr-book.org/4ed/Textures_and_Materials/Texture_Sampling_and_Antialiasing)
requires filtering frequencies against the surface footprint. Those principles motivate
separately filtered normal detail, rather than increasing roughness or enlarging grains when
texture resolution falls. The following scales and amplitudes are authored approximations,
not measured granite or gneiss spectra.

The former bake derived height only from the mineral grain field. Five normal-relief octaves
now bridge geometry and grains at .12, .06, .03, .015 and .0075 m. Each fades independently
between two and four atlas texels per wavelength. Their height amplitude is .12 times the
wavelength, multiplied by lithology's granular response, material variation and
`.35 + .65 * weathering`; the individual grain coefficient changes from .055 to .16.
These are height-field coefficients, not the final peak displacement of the mesh: the bake
only changes tangent normals. Geometry, colour and scalar/texture roughness are unchanged.
Obsidian's low granular response and explicit zero granular/variation remain respected;
the existing rind multiplier scales the complete relief without adding coating-border ridges.
Atlas resolution and memory budgets remain unchanged. Extra evaluation is offline only.
Runtime normal-map mip variance continues through the existing texture/renderer path.

Solver v10 invalidates previous baked maps, including any local v9 caches from the G117
terrain-contact review. Soil v3 acknowledges the pinned rock dependency: it does not enable
baked rock maps and its geometry is unchanged. Full affected suites and merge gates remain
deferred.

Validation: 45 rock cases / 5,738,978 assertions pass on MSVC debug; rocks and soil pass on
Linux Clang debug. The new planar-chart regression uses intentionally unresolved 2 mm grains
at 256 and 512 atlas resolutions, checks that larger relief survives, that higher resolution
retains more detail, that glass stays much smoother and that zero granular/variation is flat.
Existing tests retain deterministic pixels, chart-corner colour continuity, rind behavior and
unchanged geometry. Four rock CLI cases / 70 assertions pass, including embedded-map GLB
reimport. Lint, documentation, formatting and whitespace checks pass. Linux volumes are pruned.

Local evidence: `D:/workspace/game_engine_local/rocks/2026-10-29/surface/`. Both sides use the
same Debug build base (1a3de3ea plus the unmerged G117 changes), camera and illumination; only
the bake changes. Granite and variant 02 normal-XY RMS over the first chart rise from .01898
to .08004 and .00991 to .06255 respectively (decoded RGB8, every second interior texel).
Their geometry is unchanged: 76,800 triangles / 57,280 vertices for granite, with identical
source budgets. This is a normal-strength diagnostic, not a perceptual score.
The v20 scene's finest-geometry diagnostic looks essentially identical to the 1 px LOD view.
Multiplying the new normals by four breaks up the face further but produces excessive broad
bumps; that is retained as a diagnostic, not made the new default. The matched 1024 change is
subtle and is not accepted as fully fixing the waxy visual by itself. Broad facet smoothness
and insufficient visible mineral contrast still need authoring attention.
The 2048 diagnostic restores some finer grain but retains the bland broad material. It has
four times the pixels per map, so increasing the global default is not justified by this view.
Machine state: shared machine, RTX 5090, manual captures under the GPU lock; no quiet timings
or runtime performance gains are claimed. Texture dimensions, shader work and triangle budgets
are unchanged by the code fix; baking does additional offline field evaluations.

The photographed-material comparison is now verified. The same three rocks with the existing
local CC0 Poly Haven `rock_boulder_dry` set show substantially stronger mineral colour and
surface breakup and read more like stone. The capture binds three shared rock textures
(32 distinct scene textures including 29 terrain textures); this is not a flat fallback.
The set is a generic stone diagnostic, not a calibrated gneiss specimen. It replaces the
procedural maps rather than layering with them: the copied recipes omit `material.surface_maps`
and the local material pack supplies `rock/granite` and `rock/gneiss`. The POC itself is unchanged.
This separates a repaired normal-bake weakness from the remaining material-authoring gap:
more geometry, larger atlases or stronger normals alone did not reproduce this improvement.
Lithology-specific mineral contrast, weathered/fresh surface variation and richer material
content remain necessary before claiming photorealistic procedural rocks.

Capture lesson: `engine-view --materials` takes the pack directory, and map filenames in
`pack.json` must be relative to that directory. Absolute map paths are refused. A successful
render process can still have fallen back to scalar materials, so verify texture bindings and
the resulting image. `engine-content rocks --glb` does not accept that material-pack option;
use the scene render path for this comparison. The final local gallery contains the corrected,
textured capture, and retains the normal-only, resolution and geometry diagnostics separately.
