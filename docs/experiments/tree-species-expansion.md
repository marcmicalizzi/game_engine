# Additional temperate tree profiles

2026-10-29. G113. Research before implementation; no timing measurements.
These are representative species profiles over the branching solver, not genus-wide limits.

## Evidence and choices

- [Minnesota DNR, American basswood](https://www.dnr.state.mn.us/trees/american-basswood.html):
  dense rounded crown with a persistent main stem; alternate simple, nearly equally wide
  3–6 inch blades. Choose a 0.14 by 0.13 m adult card envelope and an upright rounded crown.
  Multi-stem sprouting is common but is outside this single-tree profile.
- [USFS Silvics, American basswood](https://research.fs.usda.gov/silvics/american-basswood):
  site and sprout origin strongly affect growth. The profile is an authored representative,
  not a prediction of age from size.
- [NC State Extension, black cherry](https://plants.ces.ncsu.edu/plants/prunus-serotina/):
  conical habit, 60–80 feet tall and 30–60 feet wide; alternate simple elongated blades,
  3–6 inches long and 1–3 inches wide. Choose 0.12 by 0.045 m blades, higher crown base
  and a narrower scaffold than sycamore.
- [USFS Silvics, black cherry](https://research.fs.usda.gov/silvics/black-cherry):
  site-dependent large forest trees versus smaller poorly formed trees. Retain the shared
  site-quality and growing-space controls.
- [NC State Extension, American sycamore](https://plants.ces.ncsu.edu/plants/platanus-occidentalis/):
  broad irregular crown, 75–100 feet tall and similarly wide in open settings; alternate,
  3–5 lobed blades 4–9 inches wide. Choose 0.18 by 0.18 m adult envelopes, a broader scaffold,
  lower leader dominance and stronger wandering than cherry. This means Platanus occidentalis,
  not the European sycamore maple.

Authored reference height/basal radius: basswood 23/0.42 m, cherry 24/0.34 m,
sycamore 28/0.65 m. Height saturation / old architecture start / old architecture completion
(years): 110/150/300, 90/120/250, 140/200/500. Potential height/basal radius:
30/0.65, 28/0.55, 35/1.0 m. These are visual interpolation scales, **not measured
lifespan limits or fitted age-growth curves**. All retain the existing sapling blade-size
blend and independent terminal wood. Wood-strength/density inputs use conservative existing
broadleaf support envelopes (45 MPa / 900 kg/m3), not claimed species measurements.
Existing species geometry is unchanged.

Leaf outline, lobing, serration and sycamore bark exfoliation require species-appropriate
material sets. The solver provides the dimensions, arrangement, scaffold and maturity.
No flowers, fruit, suckers or new texture assets are implied by these profiles.

Existing species IDs and geometry remain stable; new catalogue IDs are appended. New species
names already make distinct cache identities, so the shared tree solver version remains 11.
Root geometry has its own version. Maturity comparison captures use one existing oak material
set solely for comparing structure; final species-specific material work remains separate.

The visual review rejected the first basswood/cherry candidates as too narrow. Their authored
leader-area shares are now 0.32 and 0.38 (sycamore 0.22), routing more growth to laterals.
Trunk continuation fractions are 0.85, 0.90 and 0.70 respectively; the shared continuation
surface joins the upper branch. These are model controls fitted to the sourced crown habits,
not botanical measurements. The leaf envelopes above were not enlarged to hide sparse branching.

## Source geometry review

Machine state: shared development machine. These are deterministic source counts, not timing
or frame-cost measurements. Captures use seed 17 and growing_space 0.7, with one common
comparison material set and cameras fitted individually.
| Species | Stem age (years) | Source triangles |
|---|---:|---:|
| basswood | 2 | 842 |
| basswood | 80 | 140720 |
| basswood | 300 | 152516 |
| black_cherry | 2 | 870 |
| black_cherry | 80 | 215306 |
| black_cherry | 250 | 286820 |
| sycamore | 2 | 842 |
| sycamore | 80 | 122522 |
| sycamore | 500 | 463134 |

The authored profiles passed the Windows/Linux validation recorded in
[the root pass](tree-surface-roots.md#author-validation-2026-10-29), including deterministic
repeat solves, distinct profile hashes, sapling/adult growth and reduced-budget skeleton checks.
