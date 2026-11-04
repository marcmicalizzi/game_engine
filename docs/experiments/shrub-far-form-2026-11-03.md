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

## The fixture, drawn

`engine-view` msvc-release at 85eda802 (before) and at G123's commit (after), offscreen,
1920×1080, the POC's `isolated.json` (the dogwood alone at its scene transform, no ground), its v22
material pack, a derived-data cache of the fixture's own, `--raster rt --shadows rt --no-lights
--time-of-day 15 --lod 1 --lod-fade 0 --tunable renderer.flora.motion=false`, a camera path holding
the owner's two marker poses (12.5 and 10.1 m, yaw as the owner looked) and then two more on the
same bearing at 25 and 50 m from the plant, each captured with `--marker-captures` and
`--capture-channels ids`. **The levers**: `--foliage-lod 0.001` forces the near mesh (every
coverage error is then too large), `--foliage-lod 1000` forces the star. The number is the id
capture's `covered` (pixels with any id; nothing else is in the picture):

| camera | near mesh | before: default | before: star forced | after: default | after: star forced |
|---|---|---|---|---|---|
| 12.5 m (marker 1) | 23,651 | 31,144 (the star, +31.7%) | 31,144 | **23,651 (the mesh)** | 28,397 (+20.1%) |
| 10.1 m (marker 2) | 36,526 | 48,255 (the star, +32.1%) | 48,255 | **36,526 (the mesh)** | 43,882 (+20.1%) |
| 25 m | 5,734 | — | 7,756 (+35.3%) | 7,093 (the star, +23.7%) | 7,093 |
| 50 m | 1,358 | — | 1,923 (+41.6%) | 1,760 (the star, +29.6%) | 1,760 |

At the owner's two poses the default now draws the near mesh pixel for pixel. Drawn, the fitted
star still covers a fifth to three tenths more pixels than the mesh, where the coverage bake's
ring measured 1.07: a pixel here is one sample of the visibility buffer, the atlas is filtered
(bilinear, its mips coverage-preserving) before the half mask, and the camera is within 3° of the
horizon between two of the star's directions, where the doubling is largest, while the ring pools
eight such views with eight at ±30°. It is a
third less excess than before at every distance; closing the rest is the G121 kind of work (the
fit against the drawn coverage, not the ring's), and the stem rule keeps it to where a stem is
sub-pixel.

## What it costs

The owner's v27 scene (`reports/lab/20261103-081847-467/scene.json`: 6,024 meshes, 244,225
instances, of which the shrub-growth species are 5,014 bearberry, 1,795 hazel, 973 blackberry, 131
sagebrush and 60 dogwood, over 15 meshes), flown along R115 stage 2's walk — the v25 run's records
6693–6993, marker 1 to marker 2, one key a record (301 frames), after W82's `make-path.ps1` — with
the owner's flags (`--terrain-tiles --terrain-far 6 --time-rate 0 --world-rate 1 --time-of-day 15
--no-lights --lod 1 --deform-pool-mib 64 --rt-budget-mib 0 --raster rt --shadows rt --tunable
renderer.flora.motion=true --tunable renderer.flora.wind_strength=0.7`), no game module (so no
animal moves), offscreen at 2160 rows, `--wait-quiet 120`, under the GPU lock, the shared v24
derived-data copy. Every run recorded `quiet: false`: other agents were building beside it (CPU
others 7–98% at a run's start, GPU 2–5% busy at its start, so the GPU itself was ours). Each pair
was flown twice, the second time in the reverse order; Δ is the median over the 301 frames of
each frame's after minus before, with its quartiles. Shrub pairs are the census's visible pairs of
the 15 shrub-growth meshes (a cluster is at most 128 triangles).

| width | flight | GPU median, before → after (ms) | p95 | Δ median (p25…p75) | trace pass | shrub pairs, median | visible pairs |
|---|---|---|---|---|---|---|---|
| 3840 | 1 | 21.34 → 21.02 | 29.20 → 28.77 | −0.17 (−0.92…+0.48) | 4.84 → 4.22 | 770 → 1,227 | 882k |
| 3840 | 2, reversed | 21.02 → 20.82 | 28.33 → 28.35 | −0.11 (−0.73…+0.73) | 4.30 → 4.54 | — | — |
| 11520 | 1 | 44.27 → 45.21 | 59.23 → 60.77 | **+2.28 (+1.43…+3.10)** | 11.13 → 12.89 | 1,248 → 1,823 | 1.52 M |
| 11520 | 2, reversed | 43.65 → 44.98 | 58.14 → 60.00 | **+1.93 (+1.05…+2.93)** | 11.03 → 12.70 | — | — |

At 3840×2160 the change is inside the run-to-run spread. At 11520×2160 it costs about 2 ms of
44 (4.5%), all of it in the trace pass (+1.7 ms; the cluster builds, the deformation and the
resolve did not move): the surround view's sides pass close beside the walk's shrubs, and a ray
through a shrub's alpha-masked leaves and thin stems costs more than one through sixteen quads.
The visible pairs moved by under 600 of 1.5 million. **What the run cooked**: the after binary
built the 15 shrub-growth containers again (36.2 MB) and their 30 star atlases (50 MB) into the
shared v24 cache copy, keyed by `k_shrub_growth_version` 2; the before binary cooked nothing.

**The knob**: `k_stem_px` (one pixel of mean basal stem) in `shrub_growth.cpp`; two pixels would
halve every shrub's switch distance and so quarter the extra ground the near meshes cover, at the
price of a doubled stem two pixels wide when the star comes in. `--foliage-lod` moves it with every
foliage switch at run time.

## What belongs to the growth solver

Nothing measured here does. The near mesh is right at both of the owner's distances (the default
now draws it, pixel for pixel the forced near mesh); the doubling is the star's four directions
each carrying the whole thicket, and its distance was a coverage error scaled by a leaf. The
recipe's stems set the rule only as the size a far eye must not resolve.
