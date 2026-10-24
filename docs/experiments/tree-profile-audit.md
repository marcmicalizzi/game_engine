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
