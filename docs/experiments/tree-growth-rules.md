# Shared tree growth rules (G85, 2026-10-23)

**G86 follow-up:** [Profile audit and foliage refinement](tree-profile-audit.md) supersedes this
page's blanket source budget and small pine/spruce shoot cards. The measurements below remain
the G85 record. The audit records current budgets, green wood traits, retained visual parameters,
willow coverage and seated ancient joints.

The owner's forest POC asks for species expressed by shared growth structures, supported branch
junctions, irregular trunks, and fuller ancient landmark and dwelling trees. The forest's
`reports/quality-v4/engine-week-plan.md` also requires stable branch/perch anchors across detail
levels. This work extends flora's axes, pipe-model crown and whorled-shoot structures; it does not
add one solver per botanical name. G74's light-driven growth remains separate.

## Grounding before implementation

| Source | Observation used | Implementation consequence |
|---|---|---|
| [NC State, Before the Cut](https://content.ces.ncsu.edu/before-the-cut) | A subordinate branch has a collar wrapping around trunk wood; reaction wood differs on the upper and lower sides. | A supported, asymmetric collar and gravity-aligned oval section, with smooth axial decay, replace the perfect tube silhouette. This is a geometric approximation, not a welded cambium or stress simulation. |
| [USFS, western redcedar](https://research.fs.usda.gov/silvics/western-redcedar) | Mature bases are often fluted; branches droop; scale leaves form flat sprays. | Strong basal fluting, irregular branch stations and flattened sprays. Cedar here means Thuja plicata, not Cedrus. |
| [Minnesota DNR, sugar maple](https://www.dnr.state.mn.us/trees/sugar-maple.html) | Opposite leaves, 3–5 inches long, rounded crown. | Opposite branching pairs, a rounded crown and 0.10 m leaf cards. |
| [Missouri conservation, American elm](https://mdc.mo.gov/discover-nature/field-guide/american-elm) | Spreading limbs form a fan-shaped crown. | Ascending scaffold limbs with finer, spreading and drooping growth. |
| [NC State, American beech](https://plants.ces.ncsu.edu/plants/fagus-grandifolia/) | Low branches and bark remaining smooth with age. | Low, layered crown, gentle trunk relief rather than cedar's flutes. |
| [Oregon State, black walnut](https://landscapeplants.oregonstate.edu/plants/juglans-nigra) | Alternate compound leaves 30–60 cm long, 15–23 leaflets of 5–13 cm. | Alternate growth, terminal rachis sprays with eight pairs of 0.08 m leaflet cards. |
| [NC State, white ash](https://plants.ces.ncsu.edu/plants/fraxinus-americana/) | Opposite pinnate leaves, usually seven leaflets. | Opposite growth and short compound sprays. |
| [Scion, eastern white pine](https://nzfri.scionresearch.com/Content/Projects/nzfri/keys/cultivated-pines/key/cultivated_pines/Media/Html/Pinus_strobus.htm) | Whorled growth, crown broadening with age; erect branch ends with dense foliage. | A broader whorled crown with shorter retained foliage zones and upturned ends. Cards represent shoots, not individual needles. |
| [NC State, eastern redcedar](https://plants.ces.ncsu.edu/plants/juniperus-virginiana/) | Dense evergreen crown, mature scale foliage, fibrous bark. | A compact, irregular evergreen scaffold with scale-spray cards, distinct from Thuja's flat fans. |
| [Kennedy, old-growth Douglas-fir](https://faculty.washington.edu/mkenn/Pareto/BRANCHPRO3.html) | Delayed adaptive reiteration continuously renews foliage within existing branches. | Renewed shoots along inner ancient limbs, not foliage concentrated at the distant tips. |
| [Sillett and Van Pelt 2007](https://esajournals.onlinelibrary.wiley.com/doi/10.1890/06-0994.1) | Reiterated trunks and limb crotches carry much of old redwoods' arboreal habitat. | Ancient trees need a layered interior crown and massive branching supports as well as a distant silhouette. This does not certify a platform's load capacity. |

The existing [allometry](tree-allometry.md), [birch](birch.md), [conifer](conifer-shoots.md) and
[megaflora mechanics](megaflora-habits.md) remain the sources for pipe radii, twig stopping sizes,
sprouting and mechanical limits. Crown ratios, collar amplitudes, fluting amplitudes and renewal
fractions chosen below are bounded visual model parameters, **not measured botanical constants**.
The species names identify representative habits, not every member of a genus.

## Acceptance and budget trade

Every new profile must parse, solve within its triangle budget, keep finite valid geometry and
parent-first motion records, and reproduce its bytes across toolchains. Changing detail must not
move the growth skeleton. Tube relief uses the existing ring topology. Straight structural runs
retain their exact envelope for existing platform anchors. Ancient leaf dimensions stay at their
species size; fullness comes from branch distribution and renewal. Compare all three ancient
habits and the village recipes, including a close junction view, before claiming visual acceptance.

## Natural trees and authored village forms

The owner's clarification during review separates the representative natural megaflora, whose
habits already read reasonably, from the fantasy village's requirement for **some wide, full
canopies**. The natural emergent/spreading/mixed presets retain their limb counts, bands and fork
levels. Their shared refinement is shorter automatic straight runs, collar relief and renewed
interior growth. G82's deeper study of the default emergent remains separate.

The [village recipes](../../content/flora/species/README.md) deliberately author different
scaffolds: twelve limbs in a narrow upper band make the spreading village crown continuous;
fourteen limbs over a taller band make the layered alternative. Both use the same solver and
mechanical cap. No new fantasy-only solver or oversized leaf is introduced. These recipes require
new platform anchors; they do not migrate the game's existing platforms in place.

Each straight run carries a renewed upright shoot whose length is `branch_length * (run +
0.35 * past)`, with radius 0.3 of the run tip. Inner segment crowns use radius 0.55 of their
segment and laterals 1.2 times the terminal crown's length rule. These are explicitly authored
renewal fractions. The load approximation includes the renewed carrier and crown (section
fraction 0.09) and the inner crown (0.55 squared times 1.2 = 0.363), including an outward lever.
Renewal has its own non-straight structural carrier, so folding fine shoots into a parent's motion
part never puts off-axis foliage on an exact straight platform part.
For an explicit `limb_run_m`, that upright attaches at the trunk-side crotch (at most 0.25 m
past the nominal bark), rather than obstructing the reserved walking span. Its load follows this
inward attachment while retaining the outward crown lever. The existing offscreen limb-walking test checks this
through actual scene collision.
The denser crown shortened elder-grove's level walking fixture below its required 8 m test span.
Its authored limb radius is therefore 1.8 m instead of 1.6 m; the strength limit and walk-test
assertions remain unchanged. The game's existing recipes are not silently resized.

## Geometry review and cost

Flat-colour captures live outside Git under `D:/workspace/game_engine_local/tree-growth/`.
The camera, source recipes, build logs and PNGs are kept together there. Comparisons use the same
camera and light, with the version-5 baseline built from `84c183ac`, this branch's starting point.
These are geometry counts, not timing claims; concurrent builds make the
recorded build milliseconds unsuitable for a speed comparison.

| Form | Previous source triangles | Revised source triangles | What changed |
|---|---:|---:|---|
| Natural emergent, default 100 m recipe | 321,954 | 363,097 | Seven main limbs retained; inner renewal and shorter automatic runs; parts 496 → 625. |
| Natural spreading, seed 5, 50 m / 3 m core | 365,401 | 337,581 | Six main limbs retained; the additional interior wood also enters the mechanical cap, reducing reach. |
| Wide village, seed 101, nominal 80 m / 6 m core | — | 1,397,142 | 741,742 wood and 655,400 leaf triangles; 850 motion parts; footprint 3,217.42 m², measured LAI 5.354; individual leaf cards remain 0.30 m. |

The wide village form fits the 1.5 M source budget without merging its leaves. Its source has
905,757 terminal shoots but only 327,700 leaf cards: LAI distributes the foliage, rather than
putting a leaf on every simulated twig. The resulting connected canopy is the purpose of this
variant; a natural emergent is not forced to share it.

The original game anchor's explicit seven-limb scaffold and requested 20 m straight runs still
produce separated crown clusters in its refreshed capture (560,947 source triangles). Adopting
the continuous village crown means adopting the new dwelling recipe and re-anchoring its
platforms; a solver update alone does not replace those authored dimensions.

The ten-species gallery exposed two quality traps. A 60,000-triangle **source** budget sent the
deciduous reference trees into the older merged-card fallback, producing large opaque sheets in
the flat-colour inspection. Their default source allowance is now 1,000,000; the tests require the
reference trees to retain their finest leaf rung. An explicitly low budget still opts into that
existing approximation. The evergreen card length means a **foliated shoot or scale fan**, not
an individual needle. The profiles use 0.08–0.12 m base shoot/fan cards, with additional lateral
sprays and staggered stations in cedar and juniper; pine has longer, densely foliated laterals.

`engine-view --finest` only changes the reference tracer. Raster geometry inspection requires
`--lod 0.01`; at the normal 1 px cut, a mesh built with both bake switches zero loses many fine
leaves. That is a distance-representation limitation, not evidence that the growth solver made no
foliage. Production deciduous recipes should retain the baked chain. Evergreen chain work remains
G83. Detailed leaf outlines, bark textures, scars and a watertight cambium union remain separate.

The village's 32 px branch tiles / 256 px whole-tree bake retained the full canopy at the normal
1 px raster cut, but visibly planar transitions remain at this medium distance. The full-geometry
and baked captures are both retained; a fuller silhouette does not close that representation issue.
The committed ten-species scene was also loaded and captured through `engine-view --scene`, and
close captures inspect the cedar's base and the oak's supported junctions. Cedar's reference
recipe retains its full 71,018-triangle source: forcing it under 60,000 had also reduced the trunk
ring detail, masking the fluting this comparison is meant to inspect.

| Species fixture, seed 17 | Source triangles | Leaf/leaflet or shoot-card length |
|---|---:|---|
| maple | 188,027 | 0.100 m individual leaf/leaflet |
| oak | 361,137 | 0.120 m individual leaf/leaflet |
| elm | 740,827 | 0.090 m individual leaf/leaflet |
| walnut | 719,453 | 0.080 m individual leaf/leaflet |
| beech | 795,893 | 0.090 m individual leaf/leaflet |
| ash | 216,816 | 0.085 m individual leaf/leaflet |
| pine | 49,306 | foliated shoot / scale fan |
| spruce | 22,638 | foliated shoot / scale fan |
| cedar | 71,018 | foliated shoot / scale fan |
| juniper | 23,622 | foliated shoot / scale fan |

These counts are for the unbaked gallery sources. The broadleaf chain adds its own baked levels;
the source count is not the number of triangles submitted at every viewing distance.

The existing bake determinism fixture changes only its whole-tree colour/normal pair to
`2dadc33c6d0481b3` / `d80ce0bc824994c4`, reproduced on MSVC and Clang. The leaf-card pair remains
unchanged. This is the expected effect of the supported wood contours and their derivative
normals, not a relaxed determinism check.
The end-to-end content fixtures also agree on MSVC and Clang: the broadleaf container folds to
`d7eee5e66c8230e4`, and the 100,000-triangle ancient to `eb792d4ab6ed71a1`, including embedded
bakes and solver version 6. Those existing pins move with the intended geometry changes.

## Validation

- Windows `msvc-debug`: `tools/dev.ps1 test -Affected` exercised 121 CTest entries, including the
  renderer and allocation-free frame-loop checks. The changed bake/container byte pins and the
  reserved-limb regression were corrected; the final flora, scene-collision, engine-view,
  engine-content, lint, licence and documentation recheck passed **7/7**. The full run's log is
  `build/tree-windows-affected-ctest.log`; the final recheck is `build/tree-windows-final-ctest.log`.
- The actual GPU walk now follows a 13.6392 m level run: at six seconds it has moved 8.7344 m,
  with its feet 0.0012 m below the nominal bark. The rising run is 22.6784 m; the same marker is
  8.0212 m along, with its feet 0.0108 m above the bark. Both original walk assertions pass.
- Linux `linux-clang-debug`: the matching selection exercised 120 CTest entries. After the same
  deterministic pins were updated, the final flora/content/lint/licence/docs recheck passed
  **5/5**, including the CPU regression for clear renewal placement and the minimum walking
  span (`build/tree-linux-walk-final.log`). MSVC and Clang reproduce the recorded shape and
  container hashes. GPU walking was verified on Windows.
- Rendered the ten-species gallery, all three natural ancient habits, the original game anchor,
  the wide/layered village forms and close cedar/oak views. Refreshed the original anchor capture
  after the reserved-run correction. Generated assets remain outside Git.

The checkout's Linux volumes were pruned after validation. No quiet performance measurement is
claimed: the machine was shared with the forest preview and other work. The shared merge gate
still owns the full preset matrix and frame-budget checks; G83/G84 retain the evergreen distance
representation and forest performance follow-ups.
