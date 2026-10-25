# Tree maturity and representative species (G88)

Research/design record, 2026-10-25. Machine state: shared build/review run, `machine_state=null`, `quiet=false`; no timing claims.
This is a deterministic content-build model, not a demographic fit or a yearly simulation.
Age means the current stem's years, not the age of a clonal root system.

## Sources read before implementation

- [USFS quaking aspen](https://research.fs.usda.gov/silvics/quaking-aspen): usual mature stands
  20–25 m, much faster growth than shade-tolerant conifers; exceptional stems about 200 years.
  Ancient clones do not make each stem ancient. The web transcription's `in` for metre values
  is an OCR error (the accompanying feet conversions identify it).
- [USFS balsam fir](https://research.fs.usda.gov/silvics/balsam-fir): usual mature height
  12–18 m, DBH 30–46 cm; favorable sites reach 27 m; maximum age about 200 years.
- [USFS eastern hemlock](https://research.fs.usda.gov/silvics/eastern-hemlock): maturity
  250–300 years, longevity over 800; typical old stands exceed 30 m with 89–102 cm DBH.
  Suppressed 200-year saplings may have only 5–8 cm DBH. Site-index fits are specifically
  problematic because early suppression is common. Do not equate age and size.
- [USFS red alder](https://research.fs.usda.gov/silvics/red-alder): rapid juvenile height growth,
  about 9/16/24 m at ages 5/10/20 on good sites; structural maturity 60–70 years, usual maximum
  about 100. Favorable mature height 30–40 m, DBH 55–75 cm. This is western Alnus rubra.
- [USFS sugar maple](https://research.fs.usda.gov/silvics/sugar-maple): height growth almost
  stops at 140–150 years while diameter continues; mature stands can reach 300–400 years and
  27–37 m. First reproduction is not structural maturity.
- [USFS western redcedar](https://research.fs.usda.gov/silvics/western-redcedar): suppressed
  200-year trees only 7.6 m tall and 7.6 cm DBH occur. Old size is conditional, not inevitable.
- [USFS white oak](https://research.fs.usda.gov/feis/species-reviews/quealb): slow growth and
  exceptional longevity to 600 years support a much slower old-age scale than alder.

Existing [growth rules](tree-growth-rules.md), [profile audit](tree-profile-audit.md),
[allometry](tree-allometry.md), [birch](birch.md) and [conifer shoots](conifer-shoots.md) ground
leaf size, pipe radii, branch support, shoot retention, and architecture. The new aspen/alder
profiles use alternate simple blades; fir/hemlock use evergreen shoots, not giant single needles.

## Contract and limits

`tree.age_years = 0` disables maturity, preserving existing recipe dimensions and age behavior, except for the intentional trunk-tip density correction below.
With age enabled, `site_quality` controls size potential and growth rate, `growing_space`
controls crown width/lower branch retention and girth, and `vitality` controls leaf retention.
They are independent; old age does not imply dying. Positive `tree.height_m` and
`tree.trunk_radius_m` override the final dimensions without resetting architectural age.
Legacy recipe dimensions are reference dimensions in age mode, with final overrides in `tree`.

Each age is an independent deterministic solve. The same seed preserves reproducibility, not
persistent branch IDs across ages. The solver has no simulation tick, mortality distribution,
resource competition, reaction to neighboring trees, or demographic prediction. G74 remains open.
Curve coefficients and architecture blends are authored approximations, with species scales
chosen within the cited ranges. Longevity is an old-age scale, never a guaranteed death date.
Palm development is a separate monocot rule: early stem thickening, followed by height growth;
no invented dicot forks. Orchard structure remains the author's managed/pruned form.
## Envelope table and provenance

`height_years` is the authored time scale giving approximately 95% of potential height on the
reference site. `old_start → old_scale` controls chronological architecture, never mortality.
Radius is an authored basal radius (includes taper/flare), not a source's measured DBH/2.
The exponential curves are **not fitted yield models**. Site quality changes both rate and
potential; growing space changes girth/crown, not age. Girth saturates more slowly than height.

| Profile | Height years | Old blend years | Potential height / basal radius | Supporting source and scope |
|---|---:|---:|---:|---|
| oak / broadleaf | 130 | 180–600 | 28 m / .72 m | White oak source above; generic broadleaf adopts its scale, not a taxonomic identity |
| maple | 145 | 180–400 | 30 m / .60 m | Sugar maple source above |
| elm | 100 | 140–300 | 30 m / .85 m | [USFS American elm](https://research.fs.usda.gov/silvics/american-elm): often 175–200 years, some over 300; good forest sites 30–38 m, poor sites 12–18 m; open crowns fork low |
| walnut | 100 | 150–250 | 30 m / .65 m | [USFS black walnut](https://research.fs.usda.gov/silvics/black-walnut): good sites 30–37 m; [Wisconsin DNR](https://dnr.wisconsin.gov/sites/default/files/topic/ForestManagement/24315_45.pdf): maturity about 150, may live 250 |
| beech | 180 | 200–400 | 27 m / .55 m | [USFS American beech](https://research.fs.usda.gov/silvics/american-beech): 300–400 years; Lake States heights 17.4/25.6/26.8 m at 100/200/250 years; low open branches versus self-pruned forest stems |
| ash | 90 | 120–200 | 30 m / .55 m | [USFS white ash](https://research.fs.usda.gov/silvics/white-ash): 12/21/27 m at 20/40/70 years in one stand series; [NRCan](https://tidcf.nrcan.gc.ca/en/trees/factsheet/26): maximum age 200 |
| pine | 110 | 180–450 | 40 m / .70 m | [USFS eastern white pine](https://research.fs.usda.gov/feis/species-reviews/pinstr): common 200 years, may exceed 450; 40-year height 18.3 m and old forest height 46 m |
| spruce / conifer | 120 | 180–400 | 40 m / .65 m | [USFS Norway spruce](https://research.fs.usda.gov/feis/species-reviews/picabi): 30–61 m, strongest growth 20–60 years; native northern trees 300–400 years; generic conifer retains this contract |
| cedar | 200 | 300–1000 | 50 m / 1.30 m | Western redcedar source above; long suppression and fluted old bases |
| juniper | 100 | 150–300 | 16 m / .45 m | [USFS eastern redcedar](https://research.fs.usda.gov/feis/species-reviews/junvir): mature 12–21 m but dry-site 110-year trees under 6 m; [NPS](https://www.nps.gov/mana/learn/nature/witness-trees.htm): up to 300 years, excluding exceptional survivors |
| aspen | 55 | 80–200 | 25 m / .28 m | Quaking aspen source above; individual stem |
| fir | 70 | 100–200 | 20 m / .35 m | Balsam fir source above |
| hemlock | 250 | 300–800 | 38 m / .70 m | Eastern hemlock source above |
| alder | 40 | 65–100 | 32 m / .48 m | Red alder source above; ordinary old trees are decades old, not centuries |
| birch | 55 | 70–120 | legacy recipe dimensions | [Forest Research silver birch](https://www.forestresearch.gov.uk/tools-and-resources/tree-species-database/131561-silver-birch-sbi-2/): usually about 70 years, tallest 30 m; [Forestry England](https://www.forestryengland.uk/tree-species/silver-birch): up to 120 years |
| willow | 25 | 30–75 | legacy recipe dimensions | [UF/IFAS weeping willow](https://ask.ifas.ufl.edu/publication/ST576): often about 30 years in landscape conditions; 75 is an authored survivor scale, not a typical lifespan |
| orchard | 20 | 40–80 | legacy recipe dimensions | [Missouri Extension](https://extension.missouri.edu/publications/mg6): standard-rootstock apples over 25 ft and 50 years, dwarf forms under 10 ft and 15–20 years; this envelope represents a managed standard tree, not every rootstock |
| date palm | 100 | 80–150 | legacy recipe dimensions | [FAO](https://www.fao.org/4/t0681E/t0681e02.htm): over 100 years and about 24 m to the growing point; [FAO anatomy](https://www.fao.org/4/y4360e/y4360e05.htm): girth stops with full canopy, no vascular cambium, old leaf bases weather off |

Automatic dimensions remain inside the validated native height/radius envelope even at the best site. Explicit overrides are validated against that native envelope (for example birch: 30 m height / 1 m basal radius); an oversized explicit override returns the native field error. Derived angle, wander and crown-width adjustments clamp to their documented native ranges.

Native legacy dimensions replace the table's potential dimensions when age is enabled. Named
profiles use the table's envelope rather than their smaller, historical reference fixture.
New profiles use [USDA Wood Handbook 2021 chapter 5, table 5–3a](https://research.fs.usda.gov/download/treesearch/62244.pdf)
(green MOR/basic specific gravity) and [chapter 4, table 4–1](https://research.fs.usda.gov/download/treesearch/62243.pdf)
(sapwood moisture content). As in the earlier profile audit, density is rounded from
`1000 * basic_specific_gravity * (1 + moisture_content/100)`: quaking aspen 35 MPa/.35/113%
→ 746 kg/m³; red alder 45/.37/97% → 729; balsam fir 38/.33/173% → 901; eastern hemlock
44/.38/119% → 832. These are table-derived representative green-wood values, not paired samples
from the pictured tree. They feed the existing first-order scaffold limit, not a load certificate.
New leaf lengths (.065 m aspen, .10 m alder) and shoot cards (.32 m fir, .25 m hemlock), crown
reach, branch angles, and relief amplitudes are representative visual parameters.

## Trunk-tip density

The owner's current-build review identified an excessively dense ball on non-conifer trunk tops.
The broadleaf and birch used a large fraction of the whole trunk's pipe radius for a short crown
on only the upper 10–14% of the stem. Radius-only recursion then made many generations whose
lengths hit the same terminal-shoot floor. This created a compact knot of wood and foliage and
spent polygons on it. Willow and orchard have no such extra recursive tip crown.

The owner rejected the first correction's exposed conical termination. The final topology
continues each stem into an existing upper scaffold, with no redundant recursive top crown.
[NC State Extension's architecture guidance](https://content.ces.ncsu.edu/before-the-cut)
distinguishes excurrent central leaders (including some young oaks) from decurrent spreading
crowns (maple, ash and lacebark elm). This is architecture evidence, not a natural-tree rule
derived from its pruning diameter recommendations.
[UConn's aspen description](https://plantdatabase.uconn.edu/detail.php?pid=344) describes
conical youth becoming rounded; [NC State's silver birch profile](https://plants.ces.ncsu.edu/plants/betula-pendula/)
describes pyramidal youth becoming oval with pendulous branches. Existing species and age
scaffold angles therefore determine the continuation direction; there is no universal bend.

The stem remains unchanged through the highest scaffold station, including opposite siblings.
A cubic Hermite shoulder matches its tangent there and the continuing scaffold's tangent at
the join. Its reach is bounded by 65% of that scaffold's existing axis; the initial target is
four local stem radii or 12% of scaffold length, whichever is larger. Handles are at most 70% of the
endpoint separation, also bounded by the positive displacement along both endpoint tangents so thick, short joints do not fold back. These are authored geometric controls, not biological fitted constants.
A monotone radius profile preserves the original lower stem taper and blends into a section
buried inside the continuing branch (at most one quarter of its core radius, inside even its coarsest triangular section). Eight shoulder intervals and one reserved child-junction ring are budgeted on every rung;
the structure uses 32 samples independently of the polygon budget. Node endpoint, length,
radius and motion metadata follow the new axis. The final buried ring uses the continuing
child's exact part and bend weight, including the runtime u16 encoding; the stem motion ends
at the preceding shoulder ring. Its fixed arc and renormalized weights preserve the same part
table at every budget. This prevents independently bent tubes from exposing the cap in strong
wind. Other scaffold axes and pipe allocations stay
unchanged. The eliminated extra top recursion reduces actual shoots and leaf cost rather than
hiding leaves. Willow and orchard already hand off through their own forked models.

The CPU renderer regression uses runtime u16 bend weights at maximum supported wind over
8 directions and 24 times. Maple, beech and birch defaults and 8,000-triangle rungs, plus an
explicitly sized 600-year oak on 24,000 triangles, retain every cap vertex inside the deformed
scaffold tube. The smallest projected clearance is 9.0 mm (coarse birch). Bridge triangle areas
are 0.903–1.095 times rest area and none invert; normal agreement remains at least 0.954.
This is a tested envelope, not a proof for arbitrary authored dimensions or future wind models.

The foliage allocation footprint is the shoot-tip convex hull with a floor of
4 * leaf_length squared, representing the first leaves of a sapling whose woody tip hull can
approach zero. Ordinary tree, ancient-tree and shrub defaults exceed this shared floor.
It is allocation area, not a measured botanical crown projection.

## Validation

Final author validation passed: Windows `msvc-debug` build and `test -Affected -Base main`
passed all 122 groups; matching Linux Clang debug with warnings as errors passed all 121 groups.
The committed content examples select the complete content path, so the affected set reaches
all modules and the matching Linux filter is `.*`. This worktree's Linux build volumes were
pruned after validation. The merge's preset matrix and frame-budget gate remain separate.

The regressions cover maturity inputs and envelopes, local continuation and attachment
invariants, runtime-encoded wind binding, and existing ancient/shrub morphology. Intentional
broadleaf/birch mesh, motion and bake pins were refreshed after matched visual approval;
ancient and shrub geometry pins remain unchanged. The end-to-end broadleaf container fold
changes with its morphology and version identity. A baseline/final ancient container comparison
found only section 15 (SourceHash) changed by solver version 10: every geometry, motion and bake
payload is identical, so only its full-container identity pin changes. Linux warnings as errors
required explicit float-to-double casts, with operation order and arithmetic types preserved.

### Matched visual result and geometry cost

The final source was reviewed at whole-tree and trunk-junction scale against main `56a9284e`,
using seed 17, identical cameras and material settings. Oak, elm, beech, ash and walnut use the
existing authored cutout leaf textures read-only; finest geometry and foliage thresholds are
both `.001`. The redundant top crown is removed; a curved shoulder continues into an existing scaffold without moving lower attachments or leaving an exposed cone.
Ash was sparse outside the central plug in the baseline too. Coverage expectations now measure
supported outer foliage rather than demanding the removed redundant center. Leaf size and
physical spacing were not tightened to regain that leaf area. The ash fixture's retained-crown LAI is 0.59872 after removing its final redundant tip shoots; its regression floor is 0.59.

| Seed-17 default | Triangles before → final | Terminal twigs before → final |
|---|---:|---:|
| maple | 193,114 → 110,824 | 6,719 → 3,790 |
| oak | 311,751 → 268,362 | 5,674 → 4,650 |
| elm | 825,319 → 669,300 | 13,264 → 9,216 |
| walnut | 848,883 → 659,760 | 20,308 → 15,750 |
| beech | 708,969 → 395,678 | 12,388 → 6,870 |
| ash | 301,995 → 152,332 | 13,087 → 6,390 |
| birch | 49,999 → 50,000 | 3,685 → 2,340 |
| willow | 52,988 → 52,988 | separate model, unchanged |
| orchard | 11,954 → 11,954 | separate model, unchanged |

Birch keeps its fixed polygon budget, reallocating it away from the tip recursion. Generic
broadleaf's default has no extra terminal tip shoots and 2,312 lateral shoots (3,073 total nodes);
birch likewise has none and 2,340 lateral shoots (3,101 total). A best-site old oak remains within its 500,000-triangle
budget (307,600 triangles, 15,180 terminal twigs); an explicitly sized old oak on a 24,000 budget
uses 17,724 triangles. These counts are deterministic content costs, not frame-time measurements.

The age series covers aspen, fir, hemlock, alder, oak, birch, willow and date palm at 1, 15 and
species-appropriate old ages, plus oak at 120 and hemlock at 275. Each species also has a common
camera across all ages and nominal growth-height parameters in `review.html` (forked trunks
can end below that height), so independent close-up
framing does not conceal growth. Saplings have few scaffolds and foliage-bearing shoots;
mature/old forms add girth and architecture without imposing death or a universal century scale.
Age-series and birch views use clearly labeled fallback materials, not a new authored texture
pack. The existing cutout-material pairs above establish the canopy-coverage review.

Generated recipes, logs and captures live outside Git in
`D:/workspace/game_engine_local/tree-growth/2026-10-25/maturity/`.
