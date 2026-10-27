# Broadleaf blade dimensions

G91, 2026-10-27. The owner's review asks for larger non-sapling foliage with the
accepted leaf counts retained. Physical dimensions, rather than a uniform visual
multiplier, govern the correction.

## Sources read before implementation

Dimensions below are blade/leaflet lengths, not compound-leaf lengths. The selected
upper-range sizes are authored representatives within the cited envelopes, not
species averages or age-growth laws.

| Profile | Published evidence | Selected card length × width, cm |
|---|---|---|
| Sugar maple | [OSU](https://landscapeplants.oregonstate.edu/plants/acer-saccharum): 7.5–15 cm long and across, opposite; petiole 5–7.5 cm | 15 × 15 |
| White oak | [NC State](https://plants.ces.ncsu.edu/plants/quercus-alba/common-name/white-oak/): 4–9 × 2–4 inches (10.16–22.86 × 5.08–10.16 cm), alternate | 20 × 10 |
| American elm | [OSU](https://landscapeplants.oregonstate.edu/plants/ulmus-americana): 7.5–15 cm; [Virginia Tech via URI](https://web.uri.edu/wp-content/uploads/sites/2078/Ulmus-americana.pdf): 3–5 × 1–3 inches | 13 × 6.5 |
| American beech | [OSU](https://landscapeplants.oregonstate.edu/plants/fagus-grandifolia): 5–13 cm; [NC State](https://plants.ces.ncsu.edu/plants/fagus-grandifolia/common-name/ridge-beech/): half as wide | 12 × 6 |
| Black walnut | [OSU](https://landscapeplants.oregonstate.edu/plants/juglans-nigra): leaflets 5–13 × 2–5 cm; whole compound leaf 30–60 cm | leaflet 12 × 5 |
| White ash | [OSU](https://landscapeplants.oregonstate.edu/plants/fraxinus-americana): leaflets 5–15 cm, whole leaf 20–38 cm; ovate to ovate-lanceolate | leaflet 12 × 5; width an authored aspect ratio |
| Quaking aspen | [OSU](https://landscapeplants.oregonstate.edu/plants/populus-tremuloides): 4–7.5 cm, ovate to orbicular | 7.5 × 6.5; width an authored aspect ratio |
| Red alder | [OSU](https://landscapeplants.oregonstate.edu/plants/alnus-rubra): 7–13 cm, oval to rhombic | 13 × 8.5; width an authored aspect ratio |
| Silver birch | [OSU](https://landscapeplants.oregonstate.edu/plants/betula-pendula): 2.5–7.5 cm | audit: current 5 × 3.5 already within range |
| Weeping willow | [OSU](https://landscapeplants.oregonstate.edu/plants/salix-babylonica): 7.5–15 × 1.3–2 cm | audit: current 14 × 2.24 already near the upper envelope; do not enlarge |

A card includes the image's stalk and transparent margins: its blade is smaller
than the rectangle. The Forest oak image is a pointed-lobed oak image although
the growth profile is white-oak-like; that material mismatch is not evidence for
changing the species' branching. The reviewed maple image occupies most of its
width and includes a short stalk. These values do not promise a measured full
petiole length or normalize arbitrary user textures. The legacy generic broadleaf
already uses 24 cm cards and is not a small-leaf candidate. Orchard's 10 cm spray
is an aggregate representation; enlarging it cannot repair individual-blade shape.

## Count and spacing contract

Keep the existing allocation length/width, LAI allocation, twig capacity and
triangle plan. Only emission uses the revised independent length and width.
Thus each seed retains its woody skeleton, terminal units, leaf count, attachment
stations, random draws and source triangle count. The LOD bakes must be generated
from the revised geometry and its fitted bounds.

For a simple-leaf shoot of length L with C allocated pairs, occupied length is
0.8 L. Opposite maple has C nodes / 2C leaves, nominal node pitch 0.8 L / C.
Alternate species have 2C nodes / 2C leaves, pitch 0.8 L / (2C). The existing
bounded station jitter and orientation remain. Blade length is not internode
length: leaves overlap in projection and point away from their supporting shoot.
Do not inflate leaves to fill gaps on budget-merged branches.

Preserve the accepted small-tree appearance at heights at or below 3 m. Between
3 and 8 m, smoothly blend to the researched adult dimensions; above 8 m use the
adult values without further scaling as the tree ages. This is an explicit visual
authoring constraint, not a claim that all saplings have smaller leaves: [OSU's
identification guidance](https://landscapeplants.oregonstate.edu/plant-identification-examining-leaves)
notes variation within one plant and between lighting conditions. The existing
0.9–1.0 seeded size variation remains. Cache version 11 invalidates old bakes.

## Validation

Implemented and visually accepted. The affected Windows debug author run passed
all 38 selected test groups (2663.79 seconds), including lint and documentation.
The matching Linux Clang debug run passed all 38 groups (597.56 seconds); its
checkout-specific build volumes were pruned successfully. These are shared-machine
validation durations, not solver performance measurements. The shared merge gate
and downstream recook remain separate. Matched numerical and visual evidence follows.

### First measured comparison (2026-10-27)

Matched seed-17 default source counts are unchanged: maple 31,210 leaves / 110,824
triangles; oak 107,374 / 268,362; elm 286,510 / 669,300; beech 162,630 / 395,678.
Walnut retains 252,000 reported leaf cards, ash 44,730, aspen age 200 retains
26,008 and alder age 100 retains 109,964. The oak age-1 sapling retains 8.
These are the existing report's counts; compound foliage also carries its separate
rachis primitive. The new emission does not change that representation.

For maple, 3,790 terminal twigs and 31,210 leaves give about 4.12 opposite nodes
per twig: with the nominal 55 cm shoot, mean nominal pitch is 10.7 cm. Oak's
4,650 twigs and 107,374 alternating leaves give about 23.1 nodes per twig:
on a 65 cm shoot, nominal pitch is 2.25 cm. Rotation around the twig means
adjacent leaves need not occupy the same plane. These are ratios of totals,
not a measured distribution of every jittered internode.

The full card-area index changes from 1.527 to 4.296 for maple and from about
3.419 to 10.552 for oak. It must not be mistaken for solid blade area: the existing
Forest masks at alpha >= 128 occupy approximately 46% and 27% of their images,
respectively. That gives rough alpha-adjusted areas around 1.98 and 2.85 per unit
crown footprint after the correction. These are geometric coverage estimates,
not a measured forest-stand LAI or a claim about shaded leaf area. Actual occupied
width is about 97% of the maple card and 72% of the oak card, explaining why the
previous oak card's 5.4 cm width produced only about 3.9 cm of visible leaf.

The owner accepted the matched maple/whole-oak screenshots during this turn.
Artifacts, exact counts, mask audit and before/after review:
D:/workspace/game_engine_local/tree-growth/2026-10-27/leaf-sizing/review.html.

### Regression and review coverage

The focused 12-case run passes 5,484,043 assertions. The checks measure emitted
leaf edges across all eight named broadleaf profiles at 2, 8 and 16 m, and compare
a fixed crown with different emitted dimensions on fine and constrained budgets:
unchanged indices, leaf counts, woody vertices, motion binding and attachment
positions; changed length, width and area only.

The age-1 oak's baked container matches the previous accepted version in every
section except SourcePath (14) and SourceHash (15). Separate generic broadleaf
and ancient reference builds change only SourceHash (15), verifying that the
updated container pins are cache-identity changes, not unintended legacy geometry
changes. Named-species geometry has a new deterministic aggregate pin; the Linux
author run verifies it independently.

The review includes matched whole-tree comparisons for elm, walnut, beech, ash,
aspen and alder, the unchanged oak sapling and its transitional 15-year form,
plus textured maple/oak close, whole and default-LOD views. All capture commands
ran under the machine-wide GPU lock. These are visual/geometry checks on the
shared machine; no quiet performance benchmark or fragment-overdraw cost is
claimed. Larger leaves preserve source triangles but can shade more pixels.
