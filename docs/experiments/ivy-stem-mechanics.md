# Ivy stem mechanics and the flora catalogue (R95)

2026-10-26. Machine state: literature audit; no runtime timing is inferred from the sources.
The cover solver already emits species `ivy`, a geometry-less root and weighted hanging tips.
The missing catalogue row made the renderer reject that motion contract, including its ring forms.
The new row uses `parts` and the trees' default caster policy (traced, every cascade, posed).
The `cover` row remains `none` with zero compliance.

## Sources read and limits

[Rowe, Isnard and Speck (2004), *Diversity of Mechanical Architectures in Climbing Plants:
An Evolutionary Perspective*, J. Plant Growth Regulation 23, 108–128](https://doi.org/10.1007/s00344-004-0044-0)
was the starting point. Its publisher's accessible abstract distinguishes stiff spanning stages
and compliant supported regions; the full article was not accessible. It supplies no numeric
Hedera modulus in the material accessed here.

[Speck and Rowe (2001), *Die Wuchsform Liane*, pp. 876–877](https://www.zobodat.at/pdf/Mitt-Bad-Landesver-Natkde-Natschutz-Freiburg_NF_17_0875-0893.pdf)
explicitly distinguishes ivy's flexible, attached axes from stiffer unattached searchers and
self-supporting crown branches. It cites **Gartner and Rowe (2000), Mechanics and Hydraulics of
the Woody Root-Climber Ivy (Hedera helix), Plant Biomechanics 2000, pp. 83–84**; that original
numeric dataset was not recovered. No stage-specific Hedera stem E or EI is claimed here.
The often found **109 MPa** ivy measurement concerns attachment roots, not stems, and is not
used. Nor is leaf-cuticle stiffness a stem modulus.

The numeric analogue is a primary stem experiment by the requested authors:
[Isnard, Speck and Rowe (2003), *Mechanical architecture and development in Clematis*,
New Phytologist 158, 543–559](https://data0.eklablog.com/sisnard/perso/newphyto2003.pdf),
pp. 550–551, figures 5–6. Fresh stems were kept humid and tested mostly in four-point bending.
The values below are copied as text, not estimated from graph pixels:

| C. vitalba stem stage | Structural Young's modulus, MN m−2 (= MPa), mean ± SD |
|---|---:|
| I, young | 3074 ± 1083 |
| II | 3126 ± 784 |
| III | 979 ± 478 |
| IV, oldest | 772 ± 261 |

On the individual axis Vit 1 the authors report I from **3.5 to 4031 mm4** and EI from
**7 × 10−3 to 5 N m2**, over **15 m**. These are whole-stem structural properties, not clear-wood
properties. Older liana material becomes softer while increasing diameter can still raise EI.
Young searching shoots are stiffer than older supported regions: youth alone does not imply
arbitrarily soft material. Clematis is a petiole climber, not ivy; transferring it is an explicit
bounded architecture analogue, not a species measurement.

For comparison with the catalogue's `oak` row, use the same source as
[G86's wood audit](tree-profile-audit.md#sources-and-interpretation), but its **modulus of
elasticity**, not rupture strength: [USDA Wood Handbook (2021), chapter 5, table 5–3a,
page 5–6](https://research.fs.usda.gov/download/treesearch/62244.pdf), white oak, **Green**,
static bending E **8,600 MPa** (MOR **57,000 kPa**). The metric table's headings and row were
checked separately. Clear straight-grained wood is an approximation to a whole living stem.

## Mapping to the four renderer numbers

For the same circular section and length, I = pi d^4 / 64 and angular compliance at a given
moment is proportional to 1/EI. Select the stiffer young-climber analogue conservatively:
**8600 / 3074 = 2.7977**, rounded to **2.8** times oak's existing authored angular response.
The young mean ± SD gives a comparison envelope of **2.07–4.32×**; this is a sensitivity bound,
not a confidence interval for ivy. The older-stage mean would give **11.14×**, which is not
applied to these hanging young tips. At a hypothetical common **5 mm diameter**, the selected
moduli give EI **0.0943 N m2** for the analogue and **0.2638 N m2** for oak. Smaller diameters
increase compliance by the fourth power; supported ivy also need not carry a tree crown.

Multiply oak's `[0.006, 0.03, 0.07, 0.12]` by 2.8 to obtain
**`[0.0168, 0.084, 0.196, 0.336]` radians** for trunk, limb, twig and non-woody part respectively.
These remain renderer response coefficients under its normalized wind, not SI beam compliances
or measured leaf mechanics. The common multiplier preserves the authored class ratios; there
is no measured ivy leaf-to-stem ratio here. The root has no geometry; held vertices have weight
zero; only free tips take their part's bend. No tree row, solver geometry, container version,
hot-type size or frame-loop allocation changes.

## Validation

The catalogue regression checks ivy's motion kind, caster policy and multiplier. The viewer's
`two ivy vines` case grows two seeded vines over a smooth host, renders 320×320 offscreen, checks
that no rigid warning is logged, compares calm with motion disabled, and counts changed pixels
under wind. Both have 0 calm pixels changed (worst channel difference 0/255); wind changes 2,520 and 2,237 of 102,400 pixels. Focused Windows validation passes 1 viewer case / 34 assertions and 11 renderer flora/mask cases / 357 assertions. The dense-sward and paired-tree selection passes 3 cases / 22,692 assertions. Full final author checks are recorded in the branch report.
