# The shrub's far form: where the star stands in for a thicket (2026-11-03)

Roadmap G123; [flora](../subsystems/flora.md#the-shrub) has the rule,
[the impostor chain](../subsystems/flora.md#the-impostor-chain) the star and its coverage bake, and
[the coverage under the mask](../subsystems/flora.md#the-coverage-under-the-mask) the dither.

## The defect

The forest proof of concept's v27 handoff (2026-11-03, `reports/v27-controls-plants/HANDOFF.md`,
"Plant mark", read as text): a red-osier dogwood, `understory-v22-dogwood-2` (recipe
`{"species":"red_osier_dogwood","shrub_growth":{"seed":220164,"detail_tier":"standard"}}`, the
owner's v27 scene's instance 193119), seen from 10.1 and 12.5 m reads as a dense, doubled
silhouette; it survives `--raster rt` and mesh, `--lod-fade 0`, and a fresh bake, and goes away with
`--foliage-lod 0.001`, which forces the near mesh. So the far form, not the growth solver.

## What the star's numbers were

The finishing step of `solve_shrub_growth` builds the broadleaf's star (`tree_star`: four vertical
directions 45° apart, four depth slices each, every slice baking its whole triangles) straight over
the finest geometry, with no branch-group level beneath it. Measured by
`domain/flora/tests/shrub_far_form_tests.cpp` (msvc-debug, CPU only; each profile at its default
recipe, the dogwood at the POC's):

| species | triangles | basal stems, mean diameter | star's geometric error (parallax) | feature (a leaf) | D | coverage error | star by coverage, 1080p, from the sphere's edge | kept, plain mask |
|---|---|---|---|---|---|---|---|---|
| red-osier dogwood (seed 220164, standard) | 69,168 | 36, 20.5 mm | 0.484 m | 4.26 cm | 0.32 | 1.38 cm | 2.9 m | 1.349 |
| beaked hazel | 17,018 | 12, 36.1 mm | 0.299 m | 3.96 cm | 0.51 | 2.01 cm | 4.2 m | 1.717 |
| bearberry | 9,600 | 8, 5.1 mm | 0.237 m | 0.94 cm | 0.76 | 0.72 cm | 1.5 m | 1.899 |
| blackberry | 13,570 | 9, 10.2 mm | 0.709 m | 3.27 cm | 0.75 | 2.45 cm | 5.1 m | 2.038 |
| big sagebrush | 6,575 | 7, 30.2 mm | 0.206 m | 0.82 cm | 0.71 | 0.59 cm | 1.2 m | 2.055 |

The 1080p projection is 55° of vertical field (the POC's camera), 1,037 px a radian; at the
owner's 2160 rows the distances double. The dogwood's sphere is 2.94 m, so its star drew from
5.8 m at 1080p and 8.8 m at 2160 rows — inside the owner's 10–12.5 m.

**(a) The selection.** The coverage error is `feature_m × D`, and `feature_m` is what the level
stands in for. For a tree's star it is a top-level card, metres; for a star straight over the
leaves it is a leaf, centimetres, though the star stands in for the whole thicket. A doubled stem's
two images lie up to the parallax apart (0.48 m on the dogwood, 50 px at 10 m at 1080p), and the
stem itself is 2.1 px wide there: the doubling is plainly resolved.

**(b) The coverage.** The plain mask's star draws every stem and leaf once per direction; over the
coverage bake's ring the five stars drew 1.35 to 2.06 times the geometry's silhouette, the "dense"
half of what the owner saw. Baked with the coverage kept and fitted (`bake_matched`), they draw
1.019 (hazel, scale 112/256), 0.998 (bearberry, 122), 1.001 (blackberry, 105), 1.074 (dogwood, 137)
and 1.006 (sagebrush, 110); D hardly moves (the dogwood 0.323 to 0.313).

**(c) The representation.** Four directions is the wrong form for a sparse thicket only where its
stems are resolved: there every one is drawn up to four times, by up to the parallax. Two directions
(R92's view-chosen plane) would draw each stem once from most views and halve neither the parallax
nor the slices' depth; it would let the star in where a stem's single image is off by the parallax
and no further, which for the dogwood is the geometric error, 0.48 m — honest at a pixel only at
about 500 m at 1080p. So R92 would buy the coverage the dither already buys and the doubling, not the
distance; the stems' pixel is what decides where any planar form may stand in.

## The rule

The star's coverage error is at least the basal stems' mean diameter times the default foliage
threshold (5 px), so it reaches the threshold where a mean stem is one pixel; and the star is baked
with the coverage kept. The dogwood's star now switches at 24.2 m from its centre at 1080p and
45.5 m at 2160 rows (test-measured, `select_lod` 1 % either side).
