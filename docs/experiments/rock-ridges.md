# G131: connected rock ridges and pinnacles

2026-11-04. **Design proposal; no new recipe or solver is implemented.** Assessed against
`9d7e1815b2b7ffd9ab3f5863f1fefc61c88d1843`, after fast-forwarding the rock worktree.
The forest POC's v28 range needs a narrow rock ridge with two exposed steep sides and a
connected, broken crest, eventually viewed at climbing distance. Two intersecting cliff
objects do not meet that requirement. This work does not change shared release binaries,
author the POC, or run merge gates.

## What can be authored today

| Feature | Current contract | Boundary |
|---|---|---|
| Broad multi-peak mountain silhouette and cirque basin | Authored terrain heights | A heightfield cannot represent vertical walls, overhangs or multiple surfaces over one horizontal point. Its sampling and far-field LOD still determine the silhouette. |
| Long stratified buttress or cirque headwall backed by terrain | `Cliff`, with a curved foot line, per-point height/batter, lithology and geology | One carved outward face; the back and top close a box. Concave paths still need valid geometry. There is no glacier/cirque erosion solve. |
| Separate angular or weathered boulder/pinnacle-like block | `Boulder`; `Outcrop` for several bodies | A boulder is a radial body with bounded dimensions; an outcrop is several closed bodies, not a connected mountain ridge. |
| Talus and scree below a rock face | Cliff deposits and the separate G128 deposit generator | Terrain-fitted static authoring, not a structural union with the parent rock or simulated rockfall. |
| Narrow ridge with steep faces on both sides and one broken crest | **Needs G131** | Neither an exposed cliff back nor two overlapping front sheets supplies coherent flanks, crest, ends and geological coordinates. |
| Branched arêtes meeting at a horn; caves through the ridge | **Beyond the initial G131 topology** | Requires a junction/volume representation. Do not present overlapped ridges or authored CSG as a supported substitute. |

The relevant implementation is `domain/rocks/src/cliff.cpp`: six shared-border box faces,
front carving evaluated in `(along, height - datum_y, 0)`, a buried back, and support-limited
front rows. A closed mesh is not by itself a ridge: the exposed back needs the same quality
of geological structure as the front, and the cap needs a deliberately constructed crest.
The existing `Cliff` remains useful and keeps its public contract.

## Geological grounding

Read before choosing the representation; sources checked 2026-11-04.

* [NPS, Nunataks, Arêtes, and Horns](https://www.nps.gov/articles/nunataksareteshorns.htm):
  an arête is a narrow, jagged divide between adjacent glaciers, with cols along its crest;
  horns have steep faces on at least three sides. **Design inference:** author unequal
  flanks, peaks and saddles together. A serrated extrusion with constant height is insufficient.
* [NPS, Cirques](https://www.nps.gov/articles/cirques.htm): these are amphitheatre-like
  depressions in mountain sides. **Design inference:** the ridge is one part of a basin/range
  composition; it must not generate a random ring of towers and call that a cirque.
* [USGS, Geology of Glacier National Park](https://www.usgs.gov/geology-and-ecology-of-national-parks/geology-glacier-national-park):
  the exposed range combines sedimentary/metasedimentary strata, folding and faulting with
  subsequent glacial erosion. **Design inference:** glacial form and lithology are separate
  inputs. The same bed field must extend through both flanks and the crest, rather than restart
  on each side or rotate with every bend in the ridge.
* [NPS, Yosemite Rockfall](https://home.nps.gov/yose/learn/nature/rockfall.htm), sections
  “What Is a Rockfall?” and “What Causes Rockfall?”: fractures, sheeting and weathering affect
  which slabs detach, leaving debris below. **Design inference:** joint-controlled breaks and
  patches of intact mass belong together; the crest should not receive uniform teeth or a
  continuous talus strip. Yosemite's granite behaviour is not a carbonate preset.

Lithology-specific erosion mixtures retain the qualifications in [rock lithology](rock-lithology.md).
No erosion rate, stability criterion, universal arête width or geological age is inferred from
these pages. Every range and budget below is a proposed engineering/authoring bound, not a
measured geological constant. This is an authored static shape informed by geomorphology, not
a glacial erosion or destruction simulation.

## Proposed contract

Add a distinct `RockShape::Ridge` and optional `RockFile.ridge` block, with schema-version and
cache-version changes when implemented. Keep `Cliff` and its placement generator unchanged.
Share internal lithology, weathering and material helpers; do not make callers turn off the
cliff back or supply a second reversed cliff. A pinnacle is an isolated high point in a short
ridge with descending ends; a radial multi-arm horn is a later topology, not a preset claim.

| Proposed input | Meaning and initial bounds |
|---|---|
| `points` | 2..4096 ordered controls on a non-branching, open horizontal spine; total length 1..1000 m. Each carries local x/z, crest y, left/right base y and left/right base width. Heights above both bases 1..200 m; positive widths 0.25..100 m. |
| Left/right side profiles | Up to eight ordered `(height_fraction, width_fraction)` controls per side, common along the recipe initially. Base `(0,1)`, crest `(1,0)`; intermediate heights strictly increasing, widths positive and at most 2. These allow shoulders and bounded bed undercuts, with independent profiles for the two sides. |
| Crest controls | Authored control heights establish peaks and cols. Bounded seeded crest loss adds unequal broken notches; maximum loss 0..20 m and spacing 0.25..50 m. Never lower a crest below either base plus the minimum height. Control points are mandatory stations at every source quality. |
| `type`, `geology`, `seed` | Existing lithology and weathering vocabulary; one shared geological field for the entire body. Left/right exposure may differ, but does not change the bed datum or fracture identity. |
| `formation_from_local`, `formation_seed` | Optional rigid transform into a shared geological coordinate frame, and a common seed. Identity and recipe seed by default. Finite doubles for the transform; no scale/shear. Included in canonical parameters. |
| `ground`, `buried` | Optional existing sampled ground vocabulary; finite terrain coverage must contain the requested contact footprint. Burial 0..20 m. The closed underside is below the terrain where contact is requested. No implicit terrain boolean. |
| `triangle_budget`, `quality` | 2,000..4,000,000 source triangles, with the existing Low/Medium/High budget fractions. Includes both flanks, underside and exposed ends. Debris is a separate deposit recipe. |
| `max_position_error_m` | Requested per-axis quantization error ceiling; proposed default 0.002 m for close inspection. Refuse a mesh whose bounds exceed it; a distant author may explicitly relax it. |
| `material` | Existing material roles and PBR texture/factor inputs. Shared metre scale and formation mapping described below. |

These names are design vocabulary, **not JSON that today's loader accepts**. Validation must
name the offending control/segment, required budget or precision. No silent spine splitting,
intersection repair, peak deletion or switch to two cliffs when the requested shape cannot fit.

## One surface around one volume

At each station, use the horizontal spine tangent, its right normal and world up as the local
frame. Build an ordered cross-section from the left buried base, up the left profile to **one
crest vertex**, down the right profile, and across the buried underside. Adjacent stations share
the same perimeter ordering. Connect their perimeter edges into outward-facing triangles and
triangulate the two end polygons. The crest is an edge of this surface, not a narrow quad cap
whose zero-width limit leaves duplicate vertices and degenerate triangles.

```text
                         one crest vertex
                                /\
                    left flank /  \ right flank
                         _____/    \
                        /           \__ shoulder
             left base /               \ right base
                       +----------------+
                          buried closure
```

Use a deterministic continuous tangent interpolation with non-overshooting centreline/height
interpolation; reject reversals and zero-length horizontal segments. Do not interpolate heights
with an unconstrained spline that invents a peak above the author's controls. Sparse control
points define the broad silhouette; extra stations sample geology without moving those controls.
An isolated high station with lower neighbours creates a pinnacle in the same connected volume.
End sections are shaped exposed rock faces when above ground, not undecorated vertical plugs.

Evaluate broad mass loss, joints and bed recesses in the common geological field. Each flank
receives its own exposure response, but both consume the same fracture planes and bed identities.
Near the shared crest, constrain side carving to converge to that crest and bound inward loss
by the available cross-section width. Do not independently displace two copies of the crest.
Normal/UV duplicates are allowed at a sharp edge; their positions retain one canonical vertex id.
Smooth weathering may soften the local edge without rounding the whole ridge into a tube.

Cross-sections must be simple polygons with positive area. A positive cross-section alone does
not prove a swept shape is embedded: tight bends can cross nearby stations, and distant arms can
intersect. Validate the swept envelope first, then final triangles with a build-time spatial
index, excluding only legitimate shared boundaries. Refuse non-adjacent intersections, inverted
triangles, zero-area faces and a crest erased by quantization. Check again after the 16-bit grid
is applied. The first implementation supports no forks, loops, arches or tunnels. An undercut
may vary width with height; it must not pierce the remaining volume.

## Coordinates, surfaces and terrain contact

Geology uses one 3D reference field in metres: `p_formation = formation_from_local * p_local`.
The dip, azimuth, folds, unequal beds and vein coordinates are evaluated there. The current
cliff's `(along, y - datum, 0)` convention is unsuitable for a bent bilateral ridge: its flattened
third coordinate would make the opposite faces repeat and the strata turn with the spine.

Keep the geological reference coordinate at each source vertex while building and displacing
the surface. Recesses, fracture masks and any baked surface values use that same reference;
the material follows the displaced rock rather than re-evaluating an unrelated field at its
new position. This is a material-coordinate deformation approximation, not a physically exact
cut through planar strata. Test bed/vein landmarks across both sides and the crest before
claiming a common formation. A later volume extractor could sample final positions directly.

For the existing tiled PBR route, use deterministic charts in the **formation frame**, with
metre-scaled UVs and per-chart tangent frames derived from the actual surface. Chart offsets
are whole texture repeats; they do not encode geological bed ids. Do not mirror the second
flank's normal map. The current renderer accepts these explicit UVs; seamless triplanar blending
must not be assumed to exist. Chart boundaries can remain visible with generic tile sets.

Cliff/ridge geological atlas baking is still missing. The first shape implementation must show
both a neutral material and the existing photographed PBR, so material detail cannot hide bad
topology. Formation-coherent colour/roughness/normal and fresh-break masks need a subsequent
bake path with its own texel budget. No procedural per-pixel geology loop is proposed here.
This geometry extension alone does not eliminate PBR repetition or qualify photorealism.

Ground samples position the buried roots/underside; they do not warp the crest to the terrain
or fill the gap between two independent sheets. Check the whole underside footprint, not just
the spine. Missing coverage is an error when terrain contact was requested. Terrain can still
occlude the lower body, and terrain edits are not automatic. G128 can supply separate debris
below either flank; it must not generate a ridge-wide smooth skirt by default. Matching debris
lithology and source regions is an integration task, not mass conservation.

## Source budgets, precision and runtime LOD

Let `ns` be the number of along-spine cells and `q` the perimeter edges of a station. With the
same perimeter count at each station, lateral faces cost `2 * ns * q` triangles. The two end
caps cost at most `2 * (q - 2)` after removal of collinear cap vertices. Thus reserve
`2 * ns * q + 2 * (q - 2)` before allocation; UV/normal splitting raises vertex bytes, not
triangle count. Carved end faces need subdivisions whose extra triangles are separately priced
before accepting the rung. The following counts exclude those extra end subdivisions and are
arithmetic examples, **not measured builds**:

| Straight 120 m ridge; 40 m high; 20 m wide base | Along cells | Perimeter edges | Base triangle bound |
|---|---:|---:|---:|
| About 1 m sampling | 120 | 104 | 25,164 |
| About 0.5 m sampling | 240 | 206 | 99,288 |
| About 0.25 m sampling | 480 | 410 | 394,416 |

Counts use two sloping sides of length approximately 41.23 m plus the 20 m bottom: each edge
chain is rounded up independently. Reuse the cliff metre ladder for source sampling. Price
mandatory crest/shoulder controls first, then choose the finest rung fitting the effective
quality budget; decline a budget below the mandatory topology. Detail below two to four cells
fades as in the existing fabric. Macro peaks and cols remain across quality tiers; narrowing
the source budget does not change the seed or replace the crest with a flat cap.

One 16-bit grid has a step of `extent / 65535` per axis. At 256 m that is 3.91 mm (maximum
rounding error 1.96 mm per axis); at 1 km it is 15.26 mm (7.63 mm error). A 1 km asset therefore
cannot satisfy the proposed 2 mm error ceiling. Report the actual grid, minimum resolved feature,
chosen cell and any lost detail. Features smaller than four grid steps are not credible geometry;
use PBR microrelief for smaller features. Precision is not improved by adding triangles.

Initially keep one ridge in one mesh/grid/DAG. A long ridge can be accepted with an explicitly
coarser error ceiling for distant use, but is not then qualified at climbing distance. Long,
close-inspection ridges require a later **solver-owned** partition: evaluate one formation,
assign identical shared boundary positions, coordinate quantization and constrain seam LODs.
Independent authored segments are not an acceptable solution to that requirement. This design
does not claim that the current cluster pipeline already provides those multi-mesh guarantees.

Runtime uses the existing cluster DAG, culling, streaming and screen-error cut. Source quality
and runtime LOD remain separate controls. Qualification must preserve the broken crest and thin
volume on coarse cuts, not just pass source topology. There is no asserted millisecond cost:
measure build peak memory/time, emitted vertex and texture bytes, cluster/page counts, visible
triangles and RT structure cost on a quiet run before choosing final defaults. The 4 M source
ceiling is an input limit, not a promise that multiple finest ridges fit the frame budget.

## Implementation and acceptance sequence

1. Add the recipe/validation/report and a neutral closed ridge with shared crest, asymmetric
   flanks and exposed ends. Keep existing cliff regression fixtures. Reports include triangle
   allocation, quantization error and refusal reasons. No placement generator is required to
   prove the standalone `.rock.json` path.
2. Apply common formation geology and lithology-dependent weathering; wire the existing PBR
   contract and metre UVs. Validate Limestone/Dolostone, Quartzite, Granite and a foliated type
   on the same spine. Preserve sharp joints alongside worn edges.
3. Qualify source qualities and DAG cuts from both flanks, along the crest, above the crest,
   and from exposed ends, at 1, 5, 30 and 150 m. Include a three-peak ridge with unequal cols,
   a single pinnacle, a bent ridge and a terrain-contact case. Flat/neutral captures precede
   textured ones. Include both shadows and silhouettes; do not judge only a favourable flank.
4. Tests: one connected component; every welded edge used twice with opposite directions;
   positive volume; no non-adjacent intersections or degenerate quantized triangles; bounded
   source cost; invalid curves refused; mandatory peak positions retained at every source
   quality; coherent bed/vein reference samples at crest/chart boundaries; deterministic
   parameters/bytes on MSVC and Clang. Exercise offset/rotated formation frames and unequal
   ground heights. Check source versus coarse LOD from all views and document any silhouette
   or shading failure rather than silently weakening thresholds.
5. Only after those results: qualify seamless geological atlases, long-ridge partitioning,
   multi-arm horn junctions and scene-generator integration as separate changes. If v28 needs
   an exposed kilometre-long climbing ridge or a branched horn immediately, it needs these
   additional features; the initial single-mesh ridge does not fulfil that larger scope.

## Assessment result

The smallest coherent extension is an independently shaped, closed ridge loft sharing one
crest and one formation field. It uses the existing rock build/material/LOD route and leaves
one-sided cliffs available for terrain-backed faces. No code, schema or rendered geometry
changed during this assessment; triangle and precision figures above are calculated bounds.
Visual acceptance, performance measurements and implementation tests remain to be done.
