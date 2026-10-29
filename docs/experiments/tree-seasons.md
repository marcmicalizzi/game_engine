# Species seasons: design for a later pass

2026-10-29. G114. **Design only, not a shipped API or runtime feature.** Requested by owner.
No code, schemas, calendar ticks or default tree appearances change in this pass.

## Grounding

[US Forest Service, Why leaves change color](https://www.fs.usda.gov/Internet/FSE_DOCUMENTS/stelprd3817419.pdf)
describes pigment changes and leaf separation. [NOAA](https://www.noaa.gov/stories/cool-autumn-weather-reveals-nature-s-true-hues)
describes daylight and weather effects on timing and colour. Consequently neither a fixed
October date nor one shared species tint is a sufficient phenology model.
[Maryland Extension, marcescence](https://extension.umd.edu/resource/mystery-marcescence)
describes retained dead leaves in oak and beech. Winter retention must be independent of green
leaf survival; juvenile and sheltered lower growth can retain leaves differently.
[USFS, tamarack](https://research.fs.usda.gov/silvics/tamarack) is a deciduous conifer:
needle geometry and evergreen phenology must be separate classifications.

## Proposed ownership and data

The game maps its local climate, hemisphere, calendar and artistic controls to a normalized
phenological state. Flora remains a deterministic content solver; automatic calendar changes
are a separate consumer feature. Do not tie phenology to the solver's maturity age.

A species profile describes flushing, mature foliage, senescence, shedding and dormancy,
with independent blade expansion, live-leaf retention, dead-leaf retention and material palette.
Use explicit interpolation curves and artist overrides rather than a universal four-season enum.
Colours are material sets/palettes in linear-light workflows, not replacement of bark colour.
Species aliases such as maple must declare a representative taxon or allow palette overrides:
the current red-leaf cultivar and an autumn-red tree need not have the same summer palette.

Evergreen profiles retain their normal crown across winter; needle cohort turnover is distinct
from deciduous leaf drop. Oak/beech can retain brown leaves; ordinary deciduous profiles can
reach truly leafless geometry. Ancient fantasy habits select a phenology explicitly.
Basswood, cherry and sycamore begin as deciduous profiles; no universal hardcoded dates.

## Determinism and LOD

Assign foliage identities from seed + stable shoot identity + leaf node/side, before budget
reduction. Rank shedding with stable hashes and optional shoot-level correlation.
Retention thresholds select nested subsets: scrubbing a phase back and forth restores the same
leaves, and reduced retention cannot introduce new blades. Never rerun wood growth or consume
its RNG stream when colour or retention changes. Compound leaflets share their leaf's shedding
decision; evergreen needles need explicit cohort identity if turnover is implemented.

Initially cook a bounded set of authored seasonal states per species/seed/material combination.
Every near mesh, middle crown representation and distant bake comes from the same retained
foliage and material state. Leafless variants retain branch silhouette instead of green canopy
cards. Cache keys include phenology profile/version, quantized state, palette and actual texture
content hashes, in addition to tree recipe/solver version. No summer-green fallback for red
maples and no full-canopy distant card for bare trees.

Runtime swapping/cross-fading is later work. A consumer must select the same state at all LODs,
bound residency to adjacent states and keep collision/wood identities stable. Avoid one full
tree copy per calendar day. Prototype memory/cook costs before choosing state counts.

## Acceptance for implementation

- Sapling/adult/old, broadleaf/evergreen/deciduous-conifer/marcescent examples.
- Exact unchanged wood and stable motion identities across all phenological states.
- Nested deterministic retention including zero, with no orphan compound rachises.
- Near/middle/far walk-throughs of summer green, cultivar red, partial autumn and leafless.
- Multi-view silhouette and colour/coverage checks at actual LOD transitions, including megaflora.
- Round-trip validation, cache invalidation on palette texture changes, bounded memory and cook cost.
- No calendar feature or global seasonal default until explicitly requested.
