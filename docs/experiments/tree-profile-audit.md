# Tree profile audit and foliage refinement (G86)

This audit accompanies solver version 7. The owner's approved forms are retained unless the
reported foliage defect or the new green-wood scaffold bound changes them. A number in a
profile is not automatically a measured botanical constant. The tables distinguish observations,
empirical comparisons and retained authoring values; they do not retrospectively claim G85 was
fitted to a dataset.

## Sources and interpretation

- **S**: USFS *Silvics of North America*, linked per species below. Height and DBH depend on site,
  age and competition. A maximum or a stand-grown height/DBH pair does not justify any smaller
  open-grown pair automatically.
- **C**: Bechtold (2003), [Crown-diameter prediction models for 87 species of stand-grown trees](https://research.fs.usda.gov/download/treesearch/6206.pdf), table 3, equation 4.
  D is DBH in **inches**, predicted largest crown width is **feet**. The equations below are
  comparison targets, not multipliers on trunk radius. Coefficients were checked against the
  rendered table (pages 277–278). CVs range from 21% for Norway spruce to 37% for American elm.
  Open-grown crowns and village trees are outside this calibration. The solver's ground core
  diameter is not DBH, and its scaffold path length is not total crown radius. Neither crown-base
  fraction nor branch angle can be recovered from this width equation. We retain those existing
  defaults rather than present an unsupported inversion as research.
- **A**: Pfisterer (2000), [Modern models in tree architecture](https://arboritecture.org/pdf/pfisterer/modern-models-in-tree-architecture-as-a-helpful-tool-for-natural-pruning-pfisterer-2000.pdf), pages 214–216, applying Hallé et al.'s architectural models to temperate trees.
  Pine/maple follow rhythmic Rauh I; oak/walnut Rauh II; elm/beech Troll. **Thuja and Juniperus
  belong to Cupressaceae and the less regular Attims model, not the regular Massart tiers of
  Picea or true Cedrus.** Ash's Rauh-like assignment is a solver approximation; this paper does
  not establish a species-specific assignment for *F. americana*. A growth family is a coarse
  approximation, not a full architectural-development simulation.
- **P**: Wilson (2000), [Apical control of branch growth and angle in woody plants](https://bsapubs.onlinelibrary.wiley.com/doi/10.2307/2656846).
  Apical control motivates a dominant leader and subordinate laterals; it supplies no universal
  numerical pipe-area share for these species. The retained leader fractions are authoring values.
- **L**: Bréda (2003), [Ground-based measurements of leaf area index](https://academic.oup.com/jxb/article/54/392/2403/621920), and
  [Asner, Scurlock and Hicke (2003), global LAI synthesis](https://verso.uidaho.edu/esploro/outputs/journalArticle/Global-Sythesis-of-Leaf-Area-Index/996632179501851).
  LAI depends on stand, method, clumping and season. Bréda's table of extinction coefficients
  (for example Picea 0.28–0.37) is **not a table of LAIs**. Neither review establishes a distinct
  defensible fixed LAI for each of these ten reference trees. Each broadleaf profile now owns its
  retained 4.5 target independently. The evergreen solver uses shoot cohorts and spray coverage;
  it never consumed the broadleaf 4.5. Assigning a higher spruce LAI there would have done nothing.
- **W**: USDA *Wood Handbook* (2021), [chapter 5](https://research.fs.usda.gov/download/treesearch/62244.pdf), table 5-3a, **green** small clear specimen modulus of rupture, MPa.
- **D**: Miles and Smith (2009), [Specific gravity and other properties of wood and bark](https://research.fs.usda.gov/download/treesearch/34185.pdf), table 1A, **average green weight of wood, kg/m³**. Pages 8–12 were visually checked. These are rough population estimates, not branch-specific measurements. They include water; basic specific gravity ×1000 alone does not.
- **N**: Lundström et al. (2007), [Fresh-wood bending: Norway spruce](https://academic.oup.com/treephys/article/27/9/1229/1674430), abstract: 207 fresh specimens from one mature stem; MOR 23–90 MPa, fresh density 430–1100 kg/m³. The selected 50 MPa/800 kg/m³ pair lies within those ranges; it is an authored representative pair, not a reported mean or a correlated sample.

## How to read each profile table

H is reference height, r the **ground core radius in metres**, c the crown-base fraction, q the
scaffold path fraction of H (evergreens: crown radius/H), angle is from the parent axis. The
radius scales as (H/Href)^1.5, a retained model choice, not a species regression. Leader is pipe
area share, upturn and droop are dimensionless controls. These are not angles measured in the
field. Wood amplitudes are collar / flare / fluting / oval / knot as fractions of core radius.
No quantitative distribution for those amplitudes, or these species' hardwood insertion angles,
was found in the read sources. Their existing values remain explicit authored parameters,
bounded by the family input ranges and qualitative architecture. The five amplitudes are visual
surface relief; mechanical radii exclude them.

For the six broadleaf profiles: 10 scaffold limbs (elm 6); opposite species use 6 initial laterals
and branching ratio 5, alternate species 5 and 4. These counts and the default recursive length
ratio 0.63 are resolution/shape choices, not an empirical count of buds. Terminal leaves retain
the size sources already recorded in [G85](tree-growth-rules.md). Walnut and ash cards are
leaflets. Pine's effective lateral length is 1.4 m (overriding its .7 m base twig value); spruce
now uses .7 m, matching the accepted generic spruce. The evergreen leader share is unused.

### maple: sugar maple (*Acer saccharum*)

| Quantity | Source / interpretation | Value used |
|---|---|---|
| Height / stem | [S](https://research.fs.usda.gov/silvics/sugar-maple): 27–37 m, DBH 76–91 cm. Our smaller 18 m reference is an authored, submature/open-grown form, not a sampled mature stand mean. | H 18 m; r 0.34 m, retained |
| Crown width / crown base | C: 9.9644 + 1.0545 D; c and q are retained authoring values, not this regression's coefficients | c 0.32; q 0.38 |
| Architecture / insertion | A: Rauh I; opposite, decussate; numerical angle unmeasured for this profile | 48° at crown base, adjusted by height and seed |
| Apical control / orientation | P and habit; numerical controls authored | leader 0.4; upturn 0.65; droop 0.1 |
| Terminal unit / LAI | G85 leaf-size sources; L does not supply a species-specific reference LAI | 0.1 m leaf/leaflet; twig 0.55 m; LAI target 4.5 retained |
| Wood mechanics | W (green MOR), D (green wood weight) | 65 MPa; 881 kg/m³ |
| Wood surface | G85 habit observation, amplitudes not measured | .30 / .30 / .05 / .04 / .05 |
| Source budget | G86 reference-tree quality check; owner-tunable, not a botanical constant | 200,000 triangles |

### oak: white oak (*Quercus alba*)

| Quantity | Source / interpretation | Value used |
|---|---|---|
| Height / stem | [S](https://research.fs.usda.gov/silvics/white-oak): 24–30 m, DBH 91–122 cm. The stocky 16 m reference keeps the accepted open-grown form; it is below the cited mature height range. | H 16 m; r 0.46 m, retained |
| Crown width / crown base | C: 5.9658 + 1.5212 D; c and q are retained authoring values, not this regression's coefficients | c 0.28; q 0.48 |
| Architecture / insertion | A: Rauh II; alternate; numerical angle unmeasured for this profile | 65° at crown base, adjusted by height and seed |
| Apical control / orientation | P and habit; numerical controls authored | leader 0.22; upturn 0.55; droop 0.1 |
| Terminal unit / LAI | G85 leaf-size sources; L does not supply a species-specific reference LAI | 0.12 m leaf/leaflet; twig 0.65 m; LAI target 4.5 retained |
| Wood mechanics | W (green MOR), D (green wood weight) | 57 MPa; 1009 kg/m³ |
| Wood surface | G85 habit observation, amplitudes not measured | .45 / .40 / .09 / .06 / .09 |
| Source budget | G86 reference-tree quality check; owner-tunable, not a botanical constant | 400,000 triangles |

### elm: American elm (*Ulmus americana*)

| Quantity | Source / interpretation | Value used |
|---|---|---|
| Height / stem | [S](https://research.fs.usda.gov/silvics/american-elm): 12–18 m on adverse sites, about 24 m on medium sites, 30–38 m and DBH 122–152 cm on good dense sites. The 20 m form lies between site classes, not at their common DBH. | H 20 m; r 0.45 m, retained |
| Crown width / crown base | C: 3.5702 + 2.1201 D; c and q are retained authoring values, not this regression's coefficients | c 0.3; q 0.48 |
| Architecture / insertion | A: Troll; alternate, arching; numerical angle unmeasured for this profile | 28° at crown base, adjusted by height and seed |
| Apical control / orientation | P and habit; numerical controls authored | leader 0.22; upturn 0.1; droop 0.65 |
| Terminal unit / LAI | G85 leaf-size sources; L does not supply a species-specific reference LAI | 0.09 m leaf/leaflet; twig 0.65 m; LAI target 4.5 retained |
| Wood mechanics | W (green MOR), D (green wood weight) | 50 MPa; 892 kg/m³ |
| Wood surface | G85 habit observation, amplitudes not measured | .40 / .35 / .08 / .06 / .08 |
| Source budget | G86 reference-tree quality check; owner-tunable, not a botanical constant | 800,000 triangles |

### walnut: black walnut (*Juglans nigra*)

| Quantity | Source / interpretation | Value used |
|---|---|---|
| Height / stem | [S](https://research.fs.usda.gov/silvics/black-walnut): Good-site mature trees reach 30–37 m and DBH 76–102 cm. The accepted 18 m reference is a smaller authored tree. | H 18 m; r 0.38 m, retained |
| Crown width / crown base | C: 9.3378 + 1.1537 D; c and q are retained authoring values, not this regression's coefficients | c 0.38; q 0.42 |
| Architecture / insertion | A: Rauh II; alternate; numerical angle unmeasured for this profile | 52° at crown base, adjusted by height and seed |
| Apical control / orientation | P and habit; numerical controls authored | leader 0.3; upturn 0.5; droop 0.12 |
| Terminal unit / LAI | G85 leaf-size sources; L does not supply a species-specific reference LAI | 0.08 m leaf/leaflet; twig 0.45 m; LAI target 4.5 retained |
| Wood mechanics | W (green MOR), D (green wood weight) | 66 MPa; 913 kg/m³ |
| Wood surface | G85 habit observation, amplitudes not measured | .35 / .30 / .08 / .05 / .07 |
| Source budget | G86 reference-tree quality check; owner-tunable, not a botanical constant | 800,000 triangles |

### beech: American beech (*Fagus grandifolia*)

| Quantity | Source / interpretation | Value used |
|---|---|---|
| Height / stem | [S](https://research.fs.usda.gov/silvics/american-beech): Typical height 18–24 m, up to 37 m under optimum conditions. The Lake States table reaches DBH 51 cm at 26.8 m/250 years; our short, thick open-grown reference is not that stand-grown allometry. | H 18 m; r 0.4 m, retained |
| Crown width / crown base | C: 11.5347 + 1.1368 D; c and q are retained authoring values, not this regression's coefficients | c 0.22; q 0.42 |
| Architecture / insertion | A: Troll; alternate, layered; numerical angle unmeasured for this profile | 76° at crown base, adjusted by height and seed |
| Apical control / orientation | P and habit; numerical controls authored | leader 0.4; upturn 0.25; droop 0.2 |
| Terminal unit / LAI | G85 leaf-size sources; L does not supply a species-specific reference LAI | 0.09 m leaf/leaflet; twig 0.5 m; LAI target 4.5 retained |
| Wood mechanics | W (green MOR), D (green wood weight) | 59 MPa; 865 kg/m³ |
| Wood surface | G85 habit observation, amplitudes not measured | .25 / .35 / .025 / .04 / .035 |
| Source budget | G86 reference-tree quality check; owner-tunable, not a botanical constant | 900,000 triangles |

### ash: white ash (*Fraxinus americana*)

| Quantity | Source / interpretation | Value used |
|---|---|---|
| Height / stem | [S](https://research.fs.usda.gov/silvics/white-ash): Massachusetts dominant/codominant table: 21 m and DBH 25 cm at 40 years, 27 m/43 cm at 70 years. Our 20 m thick-stemmed reference is an open-grown artistic choice, not a fit to those paired measurements. | H 20 m; r 0.36 m, retained |
| Crown width / crown base | C: 6.1054 + 1.3213 D; c and q are retained authoring values, not this regression's coefficients | c 0.4; q 0.38 |
| Architecture / insertion | A: Rauh-like approximation; opposite, decussate; numerical angle unmeasured for this profile | 42° at crown base, adjusted by height and seed |
| Apical control / orientation | P and habit; numerical controls authored | leader 0.45; upturn 0.65; droop 0.1 |
| Terminal unit / LAI | G85 leaf-size sources; L does not supply a species-specific reference LAI | 0.085 m leaf/leaflet; twig 0.3 m; LAI target 4.5 retained |
| Wood mechanics | W (green MOR), D (green wood weight) | 66 MPa; 801 kg/m³ |
| Wood surface | G85 habit observation, amplitudes not measured | .30 / .30 / .06 / .05 / .06 |
| Source budget | G86 reference-tree quality check; owner-tunable, not a botanical constant | 240,000 triangles |

### pine: eastern white pine (*Pinus strobus*)

| Quantity | Source / interpretation | Value used |
|---|---|---|
| Height / stem | [S](https://research.fs.usda.gov/silvics/eastern-white-pine): Virgin-forest trees commonly reached 46 m and DBH 102 cm. Height at age 50 spans site indices 18.3–36.6 m; 24 m is an intermediate authored size, not a mature maximum. | H 24 m; r 0.4 m, retained |
| Crown width / crown base | C: 4.7990 + 1.0655 D; c and q are retained authoring values, not this regression's coefficients | c 0.42; q 0.26 |
| Architecture / insertion | A: Rauh I; whorled; numerical angle unmeasured for this profile | 80° at crown base, adjusted by height and seed |
| Apical control / orientation | P and habit; numerical controls authored | leader 0.5; upturn 0.9; droop 0.1 |
| Terminal unit / LAI | G80/G85 foliage sources; shoot/fan, not one needle; LAI not used | 0.45 m shoot/fan; twig 0.7 m; cohort-driven coverage |
| Wood mechanics | W (green MOR), D (green wood weight) | 34 MPa; 561 kg/m³ |
| Wood surface | G85 habit observation, amplitudes not measured | .40 / .35 / .07 / .05 / .07 |
| Source budget | G86 reference-tree quality check; owner-tunable, not a botanical constant | 60,000 triangles |

### spruce: Norway spruce (*Picea abies*)

| Quantity | Source / interpretation | Value used |
|---|---|---|
| Height / stem | [G80](conifer-shoots.md): Retains the 20 m example used by the G80 spruce family; not a North American native and not a mature-height calibration. | H 20 m; r 0.34 m, retained |
| Crown width / crown base | C: 4.5152 + 1.0654 D; c and q are retained authoring values, not this regression's coefficients | c 0.18; q 0.19 |
| Architecture / insertion | A: Massart; regular whorls; numerical angle unmeasured for this profile | 95° at crown base, adjusted by height and seed |
| Apical control / orientation | P and habit; numerical controls authored | leader 0.5; upturn 0.35; droop 0.5 |
| Terminal unit / LAI | G80/G85 foliage sources; shoot/fan, not one needle; LAI not used | 0.45 m shoot/fan; twig 0.7 m; cohort-driven coverage |
| Wood mechanics | N, selected within fresh-wood ranges | 50 MPa; 800 kg/m³ |
| Wood surface | G85 habit observation, amplitudes not measured | .30 / .35 / .05 / .04 / .04 |
| Source budget | G86 reference-tree quality check; owner-tunable, not a botanical constant | 60,000 triangles |

### cedar: western redcedar (*Thuja plicata*)

| Quantity | Source / interpretation | Value used |
|---|---|---|
| Height / stem | [S](https://research.fs.usda.gov/silvics/western-redcedar): The Silvics entry supports fluted bases, drooping branches and flat sprays. Its 54.3 m/592 cm DBH record tree is not a typical reference; 24 m/.55 m radius remain an authored intermediate size. | H 24 m; r 0.55 m, retained |
| Crown width / crown base | C: No coefficient in this eastern-species table; c and q are retained authoring values, not this regression's coefficients | c 0.16; q 0.22 |
| Architecture / insertion | A: Attims; irregular stations and flat sprays; numerical angle unmeasured for this profile | 105° at crown base, adjusted by height and seed |
| Apical control / orientation | P and habit; numerical controls authored | leader 0.5; upturn 0.8; droop 0.35 |
| Terminal unit / LAI | G80/G85 foliage sources; shoot/fan, not one needle; LAI not used | 0.12 m shoot/fan; twig 0.65 m; cohort-driven coverage |
| Wood mechanics | W (green MOR), D (green wood weight) | 35.9 MPa; 433 kg/m³ |
| Wood surface | G85 habit observation, amplitudes not measured | .45 / .70 / .22 / .10 / .10 |
| Source budget | G86 reference-tree quality check; owner-tunable, not a botanical constant | 80,000 triangles |

### juniper: eastern redcedar (*Juniperus virginiana*)

| Quantity | Source / interpretation | Value used |
|---|---|---|
| Height / stem | [S](https://research.fs.usda.gov/silvics/eastern-redcedar): USDA plant fact sheet gives a common 10–40 ft (3–12 m) range. Silvics reports 17–18 m at 50 years on best alluvial sites. The 10 m reference represents the smaller common habit. | H 10 m; r 0.22 m, retained |
| Crown width / crown base | C: 4.5904 + 1.3444 D; c and q are retained authoring values, not this regression's coefficients | c 0.12; q 0.25 |
| Architecture / insertion | A: Attims; irregular scale foliage; numerical angle unmeasured for this profile | 65° at crown base, adjusted by height and seed |
| Apical control / orientation | P and habit; numerical controls authored | leader 0.5; upturn 0.65; droop 0.1 |
| Terminal unit / LAI | G80/G85 foliage sources; shoot/fan, not one needle; LAI not used | 0.08 m shoot/fan; twig 0.4 m; cohort-driven coverage |
| Wood mechanics | W (green MOR), D (green wood weight) | 48 MPa; 593 kg/m³ |
| Wood surface | G85 habit observation, amplitudes not measured | .35 / .45 / .16 / .10 / .08 |
| Source budget | G86 reference-tree quality check; owner-tunable, not a botanical constant | 30,000 triangles |

## Mechanical scope

Each named profile carries its own green MOR and density. The shared first-order scaffold limit
is L ≤ sqrt(0.3 MOR r / (2 rho g)), with MOR converted to Pa and g = 9.81 m/s². This is the
level pipe-model self-weight relation derived on [G79](megaflora-habits.md), with the same 0.6
load-duration factor and 2 safety divisor. It prevents a narrow authored stem from carrying an
arbitrarily long main branch. It is applied to broadleaf scaffold lengths and evergreen branch
axes **before** their fine shoots are generated; it is independent of mesh budget. Requests below
the limit keep their original bits. Generic legacy families have zero mechanical traits and keep
their prior default geometry.

This limited check does not sum the actual recursive crown loads, foliage mass, snow, wind or
platform loads. The ancient still uses its separate recursive segment load calculation, including
G85 renewal growth. Do not infer a real dwelling load rating from either model. The controls and
material estimates are visible and revisable; no strength values are inferred from bark textures.

## The reported foliage defects

Captures use identical cameras, recipes and material bindings at 5, 20 and 60 m, at both normal
1 px and near-finest 0.01 px raster cuts. They live with the logs under
D:/workspace/game_engine_local/tree-growth/2026-10-24/. The generic accepted spruce is captured
with the same explicit needle material as the new spruce, avoiding an opaque flat-colour comparison.

The new spruce was sparse at 0.01 px too. G85 reduced its whole-shoot card from the accepted
0.45 m to 0.10 m, and the lateral reach from 0.7 m to 0.5 m. Pine's 0.12 m card similarly
represented only a fraction of a shoot system. Restore 0.45 m cards for both and spruce's 0.7 m
laterals. These are **aggregate shoot systems** covering omitted finer laterals under G80's
budget trade, not 45 cm needles. White pine's real needles are 6–13 cm in fascicles of five
([Scion identification key](https://nzfri.scionresearch.com/Content/Projects/nzfri/keys/cultivated-pines/key/cultivated_pines/Media/Html/Pinus_strobus.htm)); the image depicts several fascicles/shootlets. Spruce uses G80's whole-shoot image.
The wider cards restore source coverage; normal/near-finest comparisons do not justify closing
the separate evergreen baked-chain work (G83).

### Willow

[Oregon State's Salix babylonica](https://landscapeplants.oregonstate.edu/plants/salix-babylonica)
describes a typically 9–12 m tree with similar spread, thin pendulous branchlets and linear-
lanceolate leaves 7.5–15 cm × 1.3–2 cm. [NSW PlantNET](https://plantnet.rbgsyd.nsw.gov.au/cgi-bin/NSWfl.pl?lvl=sp&name=Salix~babylonica&page=nswfl)
gives 5–18 cm × 10–25 mm. It is native to China; planted weeping forms in cold regions may be
hybrids. This profile is a weeping ornamental habit, not a claim of a native North American species.

| Quantity | Source | Value used / trade |
|---|---|---|
| Tree and leaf length | OSU / PlantNET | 10 m Forest recipe, .12 m leaf length; .14 m generic default retained |
| Pendulous habit | Slender hanging branchlets in both flora descriptions | Existing arching scaffold and downward curtain axes retained; not labelled negative gravitropism (which means growth against gravity) |
| Curtain density | No measured curtain count found | 7 limbs × 7 branches × 16 curtains = 784 sprays, up from 210; these are aggregate rendering units |
| Foliage orientation | Coverage from an eye beside the crown, G80/G81 card precedent | 30° out-of-plane lift, from 0°, so the sheet is less often edge-on |
| Leaf width / outline | 1.3–2 cm measured leaves | The current connected strip/card representation and the Forest's generic leaf image do **not** pin that width or lanceolate outline. The .12/.14 m control is length. A dedicated willow atlas remains a material refinement; no false leaf-width claim is made here. |
| Budget | Existing ladder | Forest .12 m recipe: 45,932 triangles, four coarsening steps; twice the old leaf triangles and 3.73× the curtains within 60,000. Coarsening reduces strip segmentation, not curtain count. |

## Junction refinement and compatibility

Ancient named segments start with a smaller ring buried 0.75 parent-tip radii inside the parent's
end, then smoothly recover their nominal centre/radius over two child-base radii. The buried ring
has 0.45 times the smaller parent/child core radius. These are geometric seating choices, not
measured collar allometry. Surface relief ramps in; the segment base does not grow a flared open
rim outside the parent's capped end. Normals account for the centre shift. No new rings or
triangles are added. Skeleton axes, platform runs, radii, part IDs and crown layout remain unchanged.
It is an overlapping surface join, not a watertight cambium union. Other species retain the exact
original tube arithmetic and are checked against their old determinism pins.

Natural emergent, spreading and mixed forms and the original game anchor have paired captures,
as well as the reported wide village recipe. [G79](megaflora-habits.md) records current automatic
run shares and distinguishes the village's authored fantasy proportions from natural references.

## Catalogue and cache contract

The geometry-level flora species catalogue owns the names, named-profile budgets and all four
wind compliance classes. It remains available with the optional solver disabled. Flora profiles
refer to IDs; diagnostics and renderer stiffness read the same catalogue. Compile-time checks
reject missing/duplicate profile coverage, missing motion values and invalid budgets. Unknown
motion species are rejected instead of receiving an unrelated fallback.

Tree.triangle_budget = 0 (including omission) resolves through the catalogue. Explicit budgets
remain explicit. Generic families retain their own schema defaults. Beech's 900,000 allows both
the default seed and seed-17 reference to retain individual leaves; it is not the old blanket
million-triangle default. Larger source budgets remain available explicitly for larger trees.
The solver cache version rises 6 → 7, so containers change identity even for unchanged legacy
geometry. History comments retain every superseded deterministic hash with its reason.

## Capture results and source cost

**Machine state:** shared Windows workstation, RTX 5090. Manual offscreen captures held the
machine-wide GPU lock; CPU builds/tests could run alongside them. These are visual and geometry
checks, not quiet timing measurements. No frame-cost claim is made from the build/capture logs.

There are 42 before/candidate pairs at 1200 × 900, CSM, sun 35/40, identical material bindings.
The baseline copies the G85 release binaries (fa343854; flora unchanged through 405e14a5).
The candidate uses this branch's debug binaries. Recipes, camera paths, logs and review.html are
beside the images in the external folder above. The candidate subdirectory is named `accepted`;
that denotes the final implementation pass, not owner approval.

| Forest recipe | Before source triangles | After | Result |
|---|---:|---:|---|
| spruce-new-2 | 24,050 | 28,934 | More laterals and whole-shoot coverage; wood remains 7,358 triangles |
| pine-0 | 49,306 | 49,306 | Larger whole-shoot cards; topology unchanged |
| willow-1 | 28,413 | 45,932 | 784 curtains; 43,904 leaf triangles; four budget steps, below 60,000 |
| ancient-wide | 1,397,142 | 1,397,142 | Seated branch surfaces; no additional triangles or structural changes |

At 60 m, the normal and near-finest evergreen cuts both retain the restored crown coverage.
This does not replace G83's missing baked chain. From below the wide ancient, the exposed flared
continuation rims are replaced by smoothly widening wood inside the parent end. The natural
emergent/spreading/mixed and original game-anchor pairs preserve their overall silhouettes.

All six generic-spruce PNGs (three distances, two cuts) are byte-identical before and after.
The maple, oak, elm, walnut, beech, ash, cedar and juniper reference PNGs are also byte-identical.
Their new default budgets cause no coarsening for these seed-17 references. Source triangles are
188,027 / 361,137 / 740,827 / 719,453 / 795,893 / 216,816 / 71,018 / 23,622 respectively.
Pine is 49,306 and spruce 27,246 in the reference gallery, also without budget coarsening.
Legacy broadleaf, conifer, birch, orchards, palm and understory retain their previous geometry pins.

## Determinism history

| Check | Previous | Version 7 | Reason |
|---|---|---|---|
| Named-profile shape aggregate | 7516634195967486933 | 10800499079816480223 | Pine/spruce shoot coverage |
| Willow shape | 6ed17ceedeee2d85 | 02ea9f23128342d3 | Curtain count and spray lift |
| Ancient shape | 61446da58690bc66 | fbaf29c9d6486049 | Seated segment surfaces |
| Palm container | f5e8a710737c8bfa | 9606eab973575a61 | Versioned build key only |
| Rigid palm container | a290e40bd1940582 | 340996a15b4ed8a3 | Versioned build key only |
| Broadleaf full-container fold | d7eee5e66c8230e4 | 7fd0e2e92e3ce1de | Versioned build identity only |
| Ancient full-container fold | eb792d4ab6ed71a1 | 8138cfd3401a1070 | Surface/bake changes and versioned identity |

The first row is decimal; the other hashes are hexadecimal. Every old value also remains beside
its test. The synthetic renderer grove now names `broadleaf` motion traits explicitly; the
previous invented name `grove` depended on the removed unknown-species fallback.

The Windows limb-walking test exposed a separate sample-count error: markers start at t=1 s,
but `ceil(held / speed) + 1` checked an extra second. Its seventh marker was at arc 14.9512 m
on a 13.6392 m straight run, where the old continuation collar had accidentally caught it.
The first six markers stayed within 1.5 mm of the actual run surface. The test now samples
through `ceil(held / speed)`, explicitly requires each checked arc to be on the run, and keeps
the same height/lateral tolerances and minimum progress assertion. Walking past the run remains
reported separately; it is not a promise to stand on nonexistent straight wood.

## Forest staging

The Forest directory has no Git repository. Per the owner's choice, its ground-material update
is staged under the external folder's `forest-staged/world/v8/base.json`, with a source SHA-256
guard and README; the active scene is untouched. The 2 m masks, four independently authored water
regions and three existing materials produce 1,881 tiles (14,152,644 bytes) within the four-layer
budget. Snow remains disabled while the Forest palette's actual snow asset is pending. Fixed
ancients and streaming placement are unchanged. This is content-build validation, not visual
approval of the complete Forest scene.

## Author validation (2026-10-24)

This section records the first G86 refinement, before the maple correction below.

`tools/dev.ps1 test -Affected -Preset msvc-debug` selected 122 test groups: 119 passed;
the renderer grove fixture, engine-content container pins and viewer sampling error above
accounted for all failures. After their corrections and the final rebuild:

- The flora, engine-content, lint and docs CTest selection passed 4/4.
- The corrected renderer `flora poser:*` selection passed 3/3 cases, 46 assertions.
- The corrected GPU limb-walk selection passed 1/1 case, 42 assertions; the six on-run markers
  retained their 1.3–1.5 mm contact offsets. No contact tolerance was widened.
- The requested Linux clang-debug filter `flora|renderer|engine_content|engine_view|docs|lint`
  completed; renderer/content rechecks passed 3/3 groups, and final viewer and flora rechecks
  each passed. Device-dependent cases skip normally in the container without a Vulkan ICD;
  Windows supplies their GPU coverage.
- Standalone lint, docs and format passed; final touched C++ files were formatted again.

The full shared merge matrix and frame budgets belong to the coordinator's gate. Primary
binaries are installed there, after integration; this branch's review does not replace that gate.

## Maple terminal-shoot correction

The owner's close-up exposed two shared-kit approximations: class-zero terminal wood was omitted
even on the finest broadleaf rung, and leaf images repeated along a connected two-sided strip.
The Forest purple maple image is one complete leaf, with its stalk at bottom centre; its PBR
channels cannot supply the missing twig or fix the squeezed, uniformly coplanar placements.

Grounding checked on 2026-10-24: [Minnesota DNR](https://www.dnr.state.mn.us/trees/sugar-maple.html)
describes simple, opposite leaves 3–5 inches long. [Oregon State](https://landscapeplants.oregonstate.edu/plants/acer-saccharum)
gives blades 7.5–15 cm long/across and petioles usually 5–7.5 cm. [Illinois' arrangement guide](https://woodyplantstutorial.nres.illinois.edu/arrangement/arr-opposite.html)
distinguishes opposite leaves in distichous and decussate arrangements. The implementation uses
decussate nodes as a representative arrangement; it does not assert that every maple shoot or
every light environment maintains exact right angles between its blades.

The shared `LeafPlacement::opposite_simple` rule, selected only by maple, keeps the existing
0.10 m card and width ratio 0.8, and retains terminal twig tubes.
The source image includes a short stalk as well, so its visible blade is shorter than the card;
the stalk should meet the bottom-centre edge of a single-leaf image. The 15–95% leafy shoot extent,
node spacing based on 1.2 card widths,
90–100% blade scale, ±18% station jitter, ±8-degree spread and ±20-degree blade roll are authored
approximations, not measured distributions. The node-spacing cap avoids compressing 24 pairs
into a roughly half-metre twig; the report exposes the resulting LAI rather than silently
claiming the target. Leaf area is card area before alpha, as with the existing report.

Twig wood enters the triangle planner. Unmerged maple keeps all wood orders;
smaller source budgets fall back to merged sprays and coarser wood as before. The existing baked
branch-group and whole-tree levels remain the distance representation. No new motion parts,
structural axes, public schema fields, textures or shaders are introduced. A dedicated leaf
stream keeps seed determinism independent of skeleton growth. Solver version 8 invalidates
cached meshes; unrelated solver geometry remains pinned.

A trial with 0.05 m, three-sided geometric petioles cost 619,378 source triangles for Forest's
seed-17 maple (versus 188,027 before this correction). It was rejected. The accepted representation
uses the stalk already in the leaf image and spends geometry on the supporting twig instead.
That stalk is shorter than the botanical range above: a documented budget/material approximation,
not a claim that the current atlas reproduces sugar-maple petiole proportions.

The final source mesh for Forest's seed-17 purple maple changes as follows (same recipe,
materials and tool options; the recipe's explicit allowance is one million, but the new tree
also fits the unchanged 200,000 default without merging):

| Quantity | Before | After |
|---|---:|---:|
| Source triangles | 188,027 | 193,114 |
| Wood triangles | 21,883 | 82,354 |
| Leaf triangles | 166,144 | 110,760 |
| Terminal twigs | 6,719 | 6,719 |
| Motion parts | 250 | 250 |
| Reported card-area LAI | 3.7302 | 2.7114 |
| Built clusters, including the LOD hierarchy | 3,446 | 7,093 |
| Built pages | 35 | 68 |

Independent blades cannot share the old strip's vertices or collapse along its connections;
retaining twig tubes also adds wood geometry. The source triangles rise 2.7%, while the built
hierarchy is larger. Baked branch-group/tree levels remain important at distance. These are
deterministic content counts, not a measured Forest frame-cost claim; frame budgets still belong
to the shared merge gate. The smaller card-area LAI is deliberate spacing, not a new target.

Three before/after camera pairs (crown, inside crown, underside), 1200×900, use Forest's actual
purple leaf material and identical lighting with `--lod .01`. Inspection confirms separate,
unsqueezed maple silhouettes on fine wood. At crown distance the existing baked cards still use
the solver's green colour; material-aware baking is separate. Machine state: shared workstation,
RTX 5090, captures under the GPU lock; no quiet timing measurement. Files and reproducible camera/
capture scripts are in `D:\workspace\game_engine_local\tree-growth\2026-10-24\maple-followup\`,
with `review.html` and `manifest.json`. The active Forest files and materials were not modified.

Follow-up validation: Windows `test -Affected -Base HEAD` (the preceding G86 commit) passed all
38 selected groups, including flora, the renderer, viewer, frame-loop, editor and content builder.
Linux clang-debug ran the identical affected-module filter: 37/38 groups passed initially; the
only failure was the two whole-container expectations that still recorded solver version 7.
After updating those generator-identity pins, engine-content and docs passed 2/2. Maple's geometry
hash agrees across MSVC and Clang. Linux device-dependent cases skip without an ICD; Windows
provided the GPU coverage. Standalone format (1,193 files), lint and docs passed. This checkout's
two Linux volumes were pruned (3.58 GB); the shared caches and other checkouts were retained.
No full merge matrix or primary binary installation was performed by this author session.

## Terminal foliage and distance follow-up

G87, 2026-10-24, implemented and author-validated; shared merge gate pending. The owner's maple review extends the visible supporting twig and
independent blade approach to oak, elm, beech, birch, willow, ash, walnut and the broadleaf
(spreading/mixed) village ancient canopies. The emergent conifer habit keeps its foliage rule.

Botanical grounding: NC State describes simple alternate leaves for
[white oak](https://plants.ces.ncsu.edu/plants/quercus-alba/),
[American elm](https://plants.ces.ncsu.edu/plants/ulmus-americana/),
[American beech](https://plants.ces.ncsu.edu/plants/fagus-grandifolia/),
[silver birch](https://plants.ces.ncsu.edu/plants/betula-pendula/) and
[weeping willow](https://plants.ces.ncsu.edu/plants/salix-babylonica/).
Willow's leaves are narrow and its leafy shoots pendulous; birch also retains its fine drooping
shoots. These sources support alternate attachment, not copying maple's paired nodes. The
representative alternate azimuth and seeded angular/size variation are authored approximations.
Oregon State describes [black walnut](https://landscapeplants.oregonstate.edu/plants/juglans-nigra)
as alternate, pinnately compound (15–23 leaflets, terminal often absent), and
[white ash](https://landscapeplants.oregonstate.edu/plants/fraxinus-americana) as opposite and
pinnately compound (usually 7 leaflets). A compound leaf therefore needs a rachis attached to a
woody twig; the leaflet-bearing axis is not another woody branch. Existing species leaf dimensions
remain the starting point, and any spacing/count limit must report realised area rather than
inflate a blade to satisfy the LAI target.

LOD investigation: before G87 the shared path decimated bark, replaced foliage subtrees with single
planes using 16 representative tiles, then replaced the tree with four static vertical planes.
The CPU bake shaded generic solver factors and substituted an ellipse for textured
alpha. These were approximation sources relevant to the owner's Forest symptom.
The [SpeedTree card generator](https://docs9.speedtree.com/modeler/doku.php?id=card_generator)
and [Unity tree LOD documentation](https://docs.unity.com/en-us/engine/6000.7/manual/creating-environments/script-terrain/terrain-trees/terrain-tree-lod)
distinguish geometric foliage reduction and far billboards: baking is one rung, not all LOD.
Acceptance requires actual-material near/middle/far captures, including below and across a large
canopy; source budgets and built hierarchy footprint must be reported alongside the pictures.

G87 implements alternate independent blades for oak, elm, beech, full-detail birch, willow and
broadleaf ancient crowns. Ash uses opposite whole leaves with three paired leaflet nodes and a
terminal leaflet; walnut uses alternate whole leaves with eight pairs and no terminal leaflet.
The 0.30/0.45 m rachises are thin green geometry, not additional woody motion parts. Leaflet
dimensions remain the species profile's. Conifer foliage and maple's opposite placement retain
their specialized rules. Each compound terminal unit is one whole leaf: its rachis replaces
the terminal woody tube, rather than adding two whole leaves and another twig at every tip.
Ash and walnut retain their 2 mm terminal-radius growth rule. Oak uses 3.5 mm, an authored
density/budget choice rather than a universal measured twig radius. Reference budgets increase
to 400,000 triangles for ash and 1,000,000 for walnut and elm to retain their individual leaves
and crown coverage; these are ceilings, not quotas.

Alternate-node capacity allows overlapping leaves pointing in different azimuths: the selected
simple-leaf capacity is one node per 0.4 leaf widths (two nodes per stored pair), with
seeded station and orientation variation. This spacing is an authored coverage compromise, not
a measured internode constant. Realised blade area includes the 0.9–1 size variation. Merged
simple-leaf sources retain actual blade dimensions on retained shoots instead of scaling each
blade to the missing subtree's entire extent. A birch source budget too small to retain its
physical leaves uses the existing aggregate-card fallback rather than sacrificing crown coverage;
the full-detail path requires a larger source budget before the distance hierarchy can reduce it.

The three-plane subtree volumes and four-slice/four-view far tree are a bounded authored
approximation motivated by [Decoret et al., *Billboard Clouds for Extreme Model Simplification*
(2003)](https://graphics.cs.yale.edu/publications/billboard-clouds-extreme-model-simplification).
That work represents geometry with textured transparent planes; this implementation does not
claim its optimized greedy plane selection. Actual base-colour images and alpha now supply both
the CPU bake and coverage comparison; image-byte dependencies prevent a stale green bake after
changing a maple's leaf texture. Bark UV tiling is included; normal/roughness texture detail is
still approximated by the existing geometric-normal bake and material roughness.

The owner's mushroom-crown direction adds independent `Ancient` scaffold controls (schema v5).
The village comparison requests primary 2× and secondary 1.5×, keeping the straight run and all
finer nominal lengths unchanged. The same mechanical cap includes the longer scaffold and its
crown's new lever arms. A short, strong-limb regression isolates these multipliers and checks
every other shoot's length; an overloaded real recipe may still shorten the entire limb under
the pre-existing safety rule. Default multipliers are 1, so natural habits are not widened by
this new authoring option. These ratios are the fantasy art direction, not research constants.

Alternate broadleaf and broadleaf ancient fine tubes use a closed polygon tip instead of an extra cone beyond the end
ring, saving two triangles per folded tube. Named load-bearing limbs retain their seated joins
and existing surface. The saving is spent retaining foliage before moving to a coarser source
representation; it does not remove supporting axes or change the structural table.

The non-mega profiles still describe a mature architectural tree, not an ageing simulation.
The owner's autumn oak photograph (2026-10-24) is a useful structural reference: a clear lower
trunk, substantial unequal ascending forks and progressively finer twig networks. Crown base,
spread and seed can approximate that outline today. Age-dependent leader replacement, selective
shedding and reiteration remain [G74](tree-allometry.md#g74-what-self-organising-growth-would-add);
this foliage/LOD change does not claim to implement them or infer the photographed tree's age.
Growth context also matters independently of size: the [USFS white-oak account](https://research.fs.usda.gov/silvics/white-oak)
distinguishes the spreading crown and shorter bole of open-grown trees from the taller clear
trunk and compact crown of forest-grown trees. That supports separate maturity and exposure
controls in a future profile pass; it is not an identification of the photographed oak. The
[Palubicki et al. model](https://www.algorithmicbotany.org/papers/selforg.sig2009.html) underlying
G74 uses competition for light/space and internal signals to generate development. A static
mature-tree profile could improve the architecture sooner without claiming that simulation.

### G87 source cost and coverage

Actual Forest v8 seed-17 recipes, with their explicit one-million-triangle ceilings and material
paths, produce the following source geometry. LAI here is the solver's blade-area accounting;
alpha masks further reduce visible area. These deterministic counts are not timing measurements.

| Profile | Source triangles | Leaf cards | Realised LAI | Source merge |
|---|---:|---:|---:|---:|
| Oak | 311,751 | 123,822 | 3.943 | 0 |
| Elm | 825,319 | 345,144 | 4.065 | 0 |
| Beech | 708,969 | 293,094 | 3.169 | 0 |
| Ash | 301,995 | 91,609 | 1.257 | 0 |
| Walnut | 848,883 | 324,928 | 3.196 | 0 |

The corresponding G86 gallery LAIs were 4.380, 4.099, 2.726, 1.258 and 3.452. An intermediate
attempt reduced fine-shoot counts and lost roughly half the leaf area; it was rejected. The
final compound construction retains the original terminal growth radius, and the alternate
simple leaves use overlapping azimuths to preserve coverage. Reference-tree regressions require
full-detail source leaves and conservative per-profile coverage floors, not one LAI for all trees.

For the actual 80 m village ancient (seed 101, 1.5-million source ceiling), the matched baseline
and 2x/1.5x scaffold recipes use the same final solver and leaf rules:

| Measure | Baseline lengths | Wider scaffold |
|---|---:|---:|
| Source triangles | 1,192,295 | 1,422,959 |
| Wood triangles | 536,895 | 537,047 |
| Leaf cards | 327,700 | 442,956 |
| Crown footprint, m2 | 3,217.421 | 4,349.029 |
| Realised LAI | 4.968 | 4.968 |
| Blade length, m | 0.3 | 0.3 |
| Source merge | 2 | 2 |
| Motion parts | 850 | 850 |
| Built hierarchy clusters | 50,355 | 56,983 |
| Built hierarchy vertices | 2,968,399 | 3,441,086 |
| Pages | 429 | 497 |

The wider footprint is about 35% larger while leaf area per footprint is maintained. Near/far
review uses matched 60/150/350/800 m cameras at 1200x900, full geometry versus normal LOD. The
static planes remain an approximation: reduced crowns can look fuller and overhead views are
not view-facing billboards. Tiny compound rachises currently share bark shading in the far
whole-tree bake. Willow retains geometric LOD, without a new authored bake chain.

Actual red and green Forest maples are also captured at matched 25/60/140/350 m cameras. The
red variant retains its red colour in reduced detail. All review artifacts and staged Forest
recipes are external under `D:/workspace/game_engine_local/tree-growth/2026-10-24/species-lod`
and `forest-staged`; the active Forest scene and primary engine binaries are unchanged.

The Windows GPU run exposed the documented raster/ray alpha-filtering difference in the fade
fixture: raster masks use implicit mips, ray masks level zero. The new oblique card planes and
larger atlases produced 12,652/4,608,000 differing pixel IDs with fading and 12,421 without it.
Using decoded level-zero images to isolate the fade decision reduces those to 355 and 329;
vertex/mesh raster IDs match exactly. The original 0.1% ID limit and all fade/popping assertions
remain unchanged. This is a fixture correction, not a change to production filtering: cached
raster and ray masks can still differ at distance. The focused GPU case passes 264 assertions.

The viewer's whole-scene streaming fixture exposed the same 16 m tile-bound assumption as the
renderer fixture: two enlarged transparent card bounds stayed in the persistent prefix, leaving
138 streamed trees where the fixture requires all 140. The picture still matched all 57,600
pixels. Its tile size is now 32 m, retaining the all-140 assertion and exact-pixel comparison;
the separate ring-handover fixture retains its original 16 m tiles.

### G87 author validation

The final Windows `test -Preset msvc-debug -Affected -Base HEAD` selected 59 CTest groups and
completed with 57 passing. Its two failures are preserved in the external evidence: the renderer
passed 247 of 248 cases with only the two fade ID-agreement assertions failing, and the viewer
passed 122 of 123 cases with only the streamed-tree count failing. Corrected focused Windows
runs pass the fade case (264 assertions), both renderer authored-scene cases (1,539 assertions),
and both viewer authored-scene cases (25 assertions). The viewer matches all 57,600 pixels and
records 120 handovers over 45,797 tile pixels with none changed. The unchanged remainder of the
affected run passed, including frame-loop, editor, content builder, lint, licences and docs.
These are a 57/59 run plus focused corrections, not a claim of one green 59/59 Windows run.
The standalone flora suite also passes all 62 cases and 63,931,321 assertions.

Linux Clang debug passes all 59 affected groups (714.11 s). Following the fixture-only changes,
renderer plus docs passes 2/2 (51.92 s), and viewer plus docs passes 2/2 (15.13 s). Production
code did not change after the full Linux run. Device-dependent Linux cases skip without an ICD;
the focused Windows cases ran on the RTX 5090. Format and the final docs/whitespace checks pass.
This checkout's recreated Linux volumes are pruned after the follow-ups, retaining shared caches
and other checkouts. The complete logs, including original failures, are under the external
`species-lod/validation` directory named above. Full integrated matrices, remote GPU checks and
frame budgets remain the shared merge gate's; G86/G87 are not marked merged and G74 remains open.
