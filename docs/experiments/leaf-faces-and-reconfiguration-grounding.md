# A leaf's two faces, and a blade in the wind (R233, W177, grounding)

(2026-11-25, roadmap R233 and W177, the grounding both start from; pages only, nothing built or
run.) The owner walked the forest at 28 m/s on batch 75's binaries and marked two things: leaves
the wind has turned are lit wrongly ("the underside of the leaves being exposed makes the lighting
look weird"), and "with a lot of movement the leaves look flat". R233 gives a two-sided leaf an
underside; W177 lets a blade bend. This page is what both are made from: what is published on the
reflectance of a broadleaf's upper face against its lower, on which face a storm shows, and on how a
blade folds and rolls as the wind rises. **Sections 5 and 6 are readings for the two rows and are
kept apart from the sources**; nothing in them is a decision and no number on this page is pinned in
code.

What the engine has today, which this page does not repeat: the foliage optics a two-sided material
declares (a transmitted share of the reflectance, and a sphere's share of normals for needles),
their sources and their cost ([foliage, lit from both sides](foliage-two-sided-light.md),
[renderer](../subsystems/renderer.md#foliage-lit-from-both-sides),
[flora](../subsystems/flora.md#foliage-optics)); the storm's load on a crown, the Vogel exponents
read for whole crowns and the hold at 42 m/s ([the wind's page](wind.md#the-trees-in-a-storm),
[flora](../subsystems/flora.md#motion-in-a-storm)).

## How the sources were read

One pass over the web on the evening of 2026-11-25, through the fetch and search tools only, as
text. **No PDF was read**, and no publisher's full text where the publisher refused the fetch
(MDPI, Elsevier, Wiley, Taylor and Francis, Springer and the Royal Society all did); abstracts were
reached through Europe PMC, Crossref, Semantic Scholar, OpenAlex and AGRIS. Each source in
[the list](#the-sources-how-far-each-was-read-and-under-what-licence) says how far it was read —
**read** (the text itself, for what is taken), **abstract** (the abstract and nothing past it),
**secondary** (a page that restates it), **search summary only** (not read: never used as a
constant) — and under what licence its text or data stands, where that was stated. **No code was
read from any source.** Two leaf spectral libraries were found under CC BY 4.0 with both faces
measured; their files are 7 to 14 MB of spectra and were **not reduced** for this page (a fetch tool
cannot average a few hundred columns of a CSV faithfully), so no ratio by band on this page comes
from them. A number derived on this page from read numbers is marked *derived*, with its arithmetic.

## 1. A broadleaf's upper face against its lower

### What is measured, and in which direction

- **The lower face of a broadleaf reflects more in the visible and less in the near infrared.**
  Hovi, Raitio and Rautiainen (2017) measured both faces of 13 broadleaved and 12 coniferous boreal
  species with a double integrating sphere (directional-hemispherical reflectance and transmittance,
  400 to 2300 nm kept, 4 July to 14 September 2016): abaxial visible reflectance was clearly higher
  than adaxial, most of all in the broadleaves; in the near and shortwave infrared the abaxial face
  reflected less and transmitted more. **The visible transmittance showed no effect of the side.**
  The leaf's side explained up to 69 % of the variance of a broadleaf's reflectance, 31 % of its
  transmittance and 59 % of its albedo (reflectance plus transmittance); in conifers 13 %, 3 % and
  16 %. The red edge's inflection lay 11.3 nm lower on a broadleaf's lower face (4.6 nm in
  conifers). Among conifers *Abies* and *Pseudotsuga* were paler beneath (their stomata are
  there), *Pinus* darker beneath and *Larix* hardly different. The broadleaves include
  *Acer platanoides*, *Betula pendula*, three poplars (*Populus balsamifera*, *P. tremula*,
  *P. tremuloides*), *Quercus robur*, *Salix caprea*, *Sorbus aucuparia* and *Tilia cordata*. The
  values by band are in the paper's figure 4, which is an image and was not read.
- **Seven more species, the same direction.** Lu, Lu, You, Wang, Liu and Omasa (2018) measured both
  faces of white poplar, narrow-leaved oleaster, Manchurian lilac, a weeping Siberian elm
  (*Ulmus pumila* var. *pendula*), Virginia creeper, a grapevine and stag's-horn sumac through
  spring, summer and autumn 2015 (a leaf clip with its own lamp, 400 to 1000 nm): in every species
  the upper face reflected much less than the lower in 400 to 700 nm and more in the near infrared,
  the two crossing in the red edge (716 to 732 nm, least different at 727 nm). **The largest
  differences were white poplar's and oleaster's**, whose lower faces carry dense hairs; the others
  differed less. Their numbers too are in figures only.
- **Through the season.** Lukeš, Neuwirthová, Lhotáková, Janoutová and Albrechtová (2020, abstract)
  followed both faces of *Betula pendula*, *Fagus sylvatica*, *Acer pseudoplatanus*,
  *A. platanoides* and *Sorbus aucuparia* from spring to autumn and carried them to a canopy with
  DART on days 115, 212 and 285: taking the two faces as equal **under-estimated the canopy's
  reflectance by up to 20 %** (relative) in the visible and near infrared, most in off-nadir
  backscattering views. What the abstract does not give is how the leaf-level difference itself
  changes from spring to autumn; Hovi et al. followed birch and aspen from May to October and
  report the spectra's rapid changes in early spring and late autumn, but not by side.
- **Two published leaf libraries carry both faces as data**, each under CC BY 4.0: Hovi and
  Rautiainen (2022), the 25 species above with the upper face as `A` and the lower as `B` (sun and
  shade leaves, a May-to-October series for birch and aspen); Kuusinen, Lakaniemi, Hovi and
  Rautiainen (2022), aspen, goat willow and the two alders, both faces, June 2022, sunlit and shaded
  branches. Layout as read from the second: one spectrum a row, a column a nanometre from 350 to
  2500. They are where a ratio by band for these species is computed from, not read.

### Why the faces differ

- **The mesophyll's order.** A dorsiventral leaf has its palisade under the upper face and its
  spongy mesophyll under the lower. Light entering from above passes the compact palisade before it
  is scattered; light entering from below is scattered by the spongy layer's air spaces before
  the chlorophyll-rich palisade absorbs it, so more comes back out (Lu et al. 2018's discussion,
  read; the E-TRAINEE course's account, secondary, citing Vogelmann 1993). Needles and grasses have
  an undifferentiated mesophyll, which is why their faces differ little.
- **Hairs.** Dense hairs on a lower face raise its reflectance almost uniformly from 400 to
  1350 nm (Eller 1977, a search summary only); white poplar and oleaster, the densest, differ most
  (Lu et al. 2018, read). Removing them changes a leaf's reflectance even at low densities, though
  the indices built from visible bands were the least affected (Levizou et al. 2005, abstract).
  Where hairs are sparse they make no consistent difference to the specular part (Neuwirthová et
  al. 2021, abstract, on *Hieracium* and *Arabidopsis*).
- **Wax and the cuticle.** White poplar carries its wax on the upper face and its hairs on the lower
  (E-TRAINEE, secondary). The specular part of a leaf's reflectance is at the surface, wavelength
  independent and set by the cuticle's roughness, the diffuse part inside the leaf
  ([foliage, lit from both sides](foliage-two-sided-light.md#what-the-literature-says), Bousquet
  et al. 2005); a cuticle's specular reflection is fully polarised near 55° (Grant 1987, through
  E-TRAINEE, secondary). That the waxy upper face is the glossier and the hairy or stomatal lower
  face the matter is the common botanical reading; **no measurement of each face's specular lobe
  was read**.

### Which species are two-coloured

| group | species | lower face | read |
|---|---|---|---|
| strongly two-coloured, a felt of hairs beneath | white poplar (*Populus alba*), narrow-leaved oleaster | white to grey, the largest measured differences | Lu et al. 2018 |
| | silver maple (*Acer saccharinum*) | downy and silver, shown in a light breeze; slender stalks let the leaves flip | encyclopaedia, read |
| | whitebeam (*Sorbus aria*) | nearly white | encyclopaedia, read |
| paler beneath by the mesophyll, few hairs | the boreal broadleaves above (Norway maple, oak, birch, aspen, goat willow, rowan, linden), beech, sycamore | brighter in the visible, less so than the felted leaves; white poplar's difference larger than small-leaved linden's | Hovi et al. 2017; Lukeš et al. 2020; E-TRAINEE |
| hardly | larches | no difference | Hovi et al. 2017 |
| the other way | pines | darker beneath | Hovi et al. 2017 |

**The engine's broadleaves** ([flora](../subsystems/flora.md#shared-growth-rules-and-named-species-g85)):
maple, oak, elm, walnut, beech, ash, birch, willow, the orchard's apple and the default
`broadleaf` (an oak). Measured with both faces in what was read: Norway maple (Hovi et al., in
the library) and sycamore (Lukeš et al., abstract only, no open data) for maple; pedunculate oak
(the library); a weeping Siberian elm (elm, the nearest; Lu et al. 2018's figures only); beech
(Lukeš et al., abstract only, no open data); silver birch (the library); goat willow (willow, the
nearest genus, the library; the weeping willow itself was not found); rowan (*Sorbus aucuparia*,
the library), the nearest to the orchard's *Malus* of the pome-fruit trees measured, which is a
guess of the genus's neighbour, not a measurement. Whitebeam's nearly white underside is a
description, not a measurement. **Not found measured on both faces: walnut, ash, apple.** No paper
read names one of the engine's species among the felted ones.

### PROSPECT and the two faces

PROSPECT (Jacquemoud and Baret 1990, and its later versions) is a stack of plates: a leaf's
reflectance and transmittance for light arriving on its first face, from a structure number and its
pigments and water. It has **one** reflectance per leaf; nothing in it is a lower face, which is why
its users measure and invert the upper face, and why a dorsiventral model was built: Stuckens et al.
(2009, a search summary of the abstract and a conference abstract) gave the two faces of a four-layer
plate model separate light-diffusion rates to hold their near-infrared difference, within 1.3 % of
both spectra and as good as PROSPECT 5 at retrieving contents; PROLIB (Shi et al. 2024, title read,
its claims a search summary only) simulates both faces from 0.4 to 5.7 µm. The engine's
[foliage page](foliage-two-sided-light.md#what-the-literature-says) ran PROSPECT for one leaf's
green (0.134 at 550 nm); that number is an upper face's.

## 2. Which face a storm shows

- **The direction is published; the amount is not found.** That wind turns leaves and mixes the two
  faces' signals is stated (a remote-sensing source in a search summary; Lukeš et al.'s DART work
  exists because it matters), and the silver maple's encyclopaedia entry says a light breeze shows
  the undersides. Vogel (2009, abstract) names petioles that twist readily as one of what lets a
  leaf reconfigure, and poplars' flattened petioles as what sets their leaves fluttering in light
  winds is in a search summary only (Roden 2003, not read).
- **No measurement was found of the share of a crown's visible leaf area that is underside at a
  given wind speed.** A wind tunnel on three rubber trees to 17.5 m/s found the leaves' zenith angles
  staying unimodal at 0° while their azimuths turned bimodal (a search summary only, not read).
  So "a wood turning silver" has no published number on this page: the row that needs one measures
  its own crowns (section 5).

## 3. A blade in the wind

### The shapes, and when each sets in

- **Vogel (1989, abstract)**, the start: single leaves and clusters of several species in turbulent
  winds of 10 and 20 m/s. Drag coefficients on the leaf's original area **fell as the speed rose
  for every leaf but single white oak leaves**. Leaves with acute bases and short petioles had the
  highest area-specific drag, fluttered erratically and most often tore; **leaves with lobed bases
  and long petioles fluttered little and rolled into increasingly acute cones**; pinnately compound
  leaves made cylinders with their leaflets layered alternately, the lowest drag of all; white
  poplar's single leaves were unstable at every speed yet resisted damage to 30 m/s, and its
  clusters made stable cones. The speeds at which each shape sets in are not in the abstract.
- **Vogel (2009, abstract)**: winds of 20 m/s or more at the leaf tear leaves; single leaves and
  clusters curl into low-drag stable cones and cylinders where the blade's shape, a petiole that
  twists readily and enough instability at low speed allow it.
- **The sequence on one leaf.** Shao, Chen and Lin (2012, a London plane leaf, *Platanus
  acerifolia*; a search summary of the abstract only): with the Reynolds number on the blade's
  length a leaf passes from static deformation to large slow sway, reconfiguration into a delta
  wing, tip flapping, fast vibration of the blade and coupled vibration of twig and leaf, each
  change abrupt at a critical number. No speed was read.
- **Cones damp the flutter.** Miller et al. (2012, abstract) filmed wild ginger and violet leaves and
  modelled sheets: a cone holds a stable recirculation in its wake and shakes less than a U-shaped
  sheet, whose vortex shedding gives larger forces; their tunnel ran 1 to 10 m/s (Miller and
  Santhanakrishnan, arXiv 1310.5615, read; tulip poplar and a maple among the leaves; no onset speed
  given).
- **A shoot's leaves close together.** In a whole sweet cherry in a large wind tunnel (Tadrist et
  al. 2018, abstract) the foliage's motion is single leaves' flutter at low wind and the branches'
  buffeting by turbulence at high, because the flutter depends weakly on the speed and the buffeting
  strongly; drag and light interception show a third mechanism, **leaves bending and overlapping**,
  which limits each leaf's flutter. The crowns' frontal areas at 20 m/s, 20 to 37 % of still air for
  three hardwoods (Vollsinger et al. 2005), are on [the wind's page](wind.md#the-trees-in-a-storm).

### The drag's exponent

Drag grows as U^(2+V), V the Vogel exponent (Gosselin 2019, read, its eq. 4; 0 for a rigid bluff
body):

| what | V | b = 2 + V | source, how read |
|---|---|---|---|
| a tulip tree leaf (*Liriodendron*), rolling into a cone | about −1 | about 1 | Vogel 1989 as Gosselin 2019 reports it (read) |
| a single white oak leaf | positive | over 2 | Vogel 1989, as Gosselin reports it; no value |
| a rectangular plate, large deformation (Cauchy number over 100) | −2/3 | 4/3 | Gosselin 2019 eq. 7 (read) |
| a disc cut along a radius, rolling into a cone | −4/3 | 2/3 | Schouveiler and Boudaoud 2006, as Gosselin 2019 gives it (read) |
| a fibre, large deformation | −2/3 | 4/3 | Gosselin 2019 (read) |
| a dense poroelastic body (a crown's model) | about −1 | about 1 | Gosselin 2019 eq. 10 (read) |
| loblolly pine, 1 m and a branch, 8 to 19 m/s | −1.1 | 0.9 | Vogel 1989 in Gosselin's table 1 (read) |
| Scots pine, 5 m, 8 to 19 m/s | −0.72 | 1.28 | Mayhead 1973 in Gosselin's table 1 (read) |
| lodgepole pine and western hemlock, 5 m, 8 to 20 m/s | about −1 | about 1 | Rudnicki et al. 2004 in Gosselin's table 1 (read) |

**Where reconfiguration starts**: for plates, filaments and poroelastic balls, once the Cauchy
number C_Y (the fluid's load over the body's stiffness) is about 1, or C_Y over the structure's
own ratio γ is 1 to 3 (Gosselin 2019, read); the review warns against fitting one exponent across
the transition. **No leaf's onset speed was read**: Vogel's tests started at 10 m/s, where his leaves
were already reconfiguring; a 6 m/s threshold past which walnut, peach and pepper leaves bend at
their tips rather than their bases is a search summary only.

### Flutter before it

- Vortex shedding off a bluff body at Reynolds numbers 10² to 10⁵ is at a Strouhal number of about
  0.2 (Gosselin 2019, read): shedding is set by the dimension across the flow, so a
  blade 5 cm wide at 5 m/s would shed at about 20 Hz (*derived*, f = 0.2 U / w, w the width the
  wind meets), which is a scale for the fastest flutter, not a measurement of a leaf.
- A quaking aspen's leaf flutters at about 3 to 5 Hz in 2 to 4 m/s of wind (a review's figure, a
  search summary only); Roden (2003, search summary only) drove a modelled aspen leaf at 3 Hz; a
  model cottonwood leaf's flutter rose with the speed to about 4.5 Hz at 16 knots (search summary
  only). Tadrist, Julio, Saudreau and de Langre (2015) explain a leaf's flutter as torsional
  galloping (title read; the abstract was not reachable).
- None of these is the engine's: its flutter is the part's sway, 0.9 Hz over the square root of the
  part's length ([flora](../subsystems/flora.md#motion)); a leaf's own flutter is a second, faster
  motion inside it.

### Which leaves cup, which roll, which tatter

| leaf form | in a high wind | source |
|---|---|---|
| lobed base, long petiole (tulip tree, and Vogel's like it) | rolls into an acute cone; little flutter | Vogel 1989 (abstract) |
| acute base, short petiole | flutters erratically, tears most | Vogel 1989 (abstract) |
| pinnately compound (ash, walnut among the engine's) | a cylinder of leaflets layered alternately; least drag | Vogel 1989 (abstract) |
| white poplar, single | unstable at every speed, undamaged at 30 m/s; clusters make stable cones | Vogel 1989 (abstract) |
| white oak, single | flutters violently and tears at moderate speeds; drag grows faster than U² | Vogel 1989 through Gosselin 2019 |
| London plane | sway, a delta wing, tip flapping, fast vibration, by critical numbers | Shao et al. 2012 (search summary) |
| a shoot | leaves bend and overlap | Tadrist et al. 2018 (abstract) |

Which face a rolled leaf turns outward was **not found**: whether a cone closes with the upper face
inside, so that a rolled leaf shows its underside, is the question that joins W177 to R233.

## 4. What was not found published

- A per-band ratio of lower to upper reflectance, for any of the engine's species, as text: every
  read paper gives them in figures. The CC BY 4.0 libraries above hold them as data.
- Walnut, ash and apple measured on both faces; a weeping willow measured at all.
- Each face's specular lobe, measured.
- How the faces' difference moves through a season, by side.
- The share of a crown's visible leaves that are underside at a wind speed.
- A broad leaf's reconfiguration onset speed, and its fold angle or roll radius against speed.
- Which face a rolled leaf shows.

## 5. A reading for R233, kept apart from the sources

- **What the ratio is to be, by band.** What was read says it is above 1 in the visible: Hovi et
  al. 2017 as a statement of the region for the group (abaxial visible reflectance notably higher,
  particularly in broadleaves; their values by band and species are in a figure that was not
  read), and Lu et al. 2018 over 400 to 700 nm for every one of its seven species. That it holds
  in each of the renderer's three bands, and for each species, is what the library's reduction has
  to confirm. It is larger for felted leaves (white poplar and oleaster measured; silver maple and
  whitebeam by description) than for the rest, and below 1 in the near infrared, which the renderer
  does not draw. **Its value by band is not in what was read**, so the row's
  first step is a reduction of Hovi and Rautiainen's library (CC BY 4.0; credit the authors and the
  dataset's DOI on the page that takes a number) — for each species, the mean of `B` over `A` in the
  bands the renderer's colours stand for, sun and shade leaves apart — done by a tool outside the
  repository and its numbers written on this page with the method. Until then **zero is the
  stand-in**, as the row's text says: a material that declares no back face draws what it drew.
- **The groups the numbers would fill.** From the library: maple from Norway maple, oak (and the
  default `broadleaf`) from pedunculate oak, birch from silver birch, willow from goat willow, the
  apple from rowan (section 1's nearest genus). Elm from the Siberian elm in Lu et al. 2018, whose
  values are in figures only: they would have to be digitised from the figures (the article is CC
  BY, so it is allowed), and the page that takes them says so. **Beech and sycamore are not
  reachable**: Lukeš et al. measured them, but only the abstract was read and their data is not
  open, so beech takes the library's broadleaf mean until a measurement is reached. Walnut and ash
  take the same mean until one is measured. A felted group (white poplar and oleaster from Lu et
  al.'s figures, the same digitising) serves an asset that declares it.
- **The transmitted share takes one colour whichever face is lit.** Hovi et al. found the visible
  transmittance not affected by the leaf's side (in the near and shortwave infrared it was: the
  lower face reflected less and transmitted more), so in the bands the renderer draws the light
  through a leaf may be taken as the same lit from above or below. That rests on their measurement,
  the one read source that measured transmittance on both faces, not on a general rule. The engine states transmission as a share of the
  reflectance ([foliage](foliage-two-sided-light.md#the-model)); with a second face's albedo, the
  share must stay **a share of the upper face's** (or be stated as a colour of its own), never of
  whichever face the eye sees, or a turned leaf would transmit more for being paler.
- **Which face of a card is the upper.** Each of the three leaf builders in
  `domain/flora/src/kit.cpp` winds a blade's front towards a normal it computes, and each of those
  normals points up: `build_spray`'s `upper` is the up direction's component perpendicular to the
  twig (the crown passes a `side` across the twig and level, so `upper` is `up` less its part
  along the twig); `build_simple_leaves` (and `build_compound_leaves`, whose normal is built the
  same way from a level lateral) and `build_leaf_card` flip their normal whenever its y is below 0.
  So the front is the sky's side by construction, which a case has yet to hold; the leaf image is
  mapped the same on both faces. A bought plant's glTF says only which winding is the front. The row must settle that **the
  front is the upper face** for every two-sided material that declares a back, hold the solver's
  sprays to it with a case, and say so for imported assets.
- **The specular lobe.** Nothing measured was read for either face; the common reading (a glossier,
  waxy upper face, a matter, hairy lower one) would be a roughness each face declares, and the row
  must decide whether one roughness serves until a measurement does.
- **What it must not do.** Key on a species or a generator: the back face is a property any
  two-sided material may declare (the row's text), and the felted group is an asset's declaration.

## 6. A reading for W177, kept apart from the sources

- **A shape that could follow the speed.** With a leaf's exponent near −1 past onset (Vogel's tulip
  tree, the poroelastic theory) and its drag coefficient on the reconfigured area taken as fixed,
  the frontal area falls as U₀ / U past an onset speed U₀ (*derived*: U² A(U) ∝ U^(2+V) with
  V = −1). A blade folded about its midrib with each half turned by φ from flat shows cos φ of its
  area to a wind square on, so **φ(U) = acos(min(1, U₀ / U))** (*derived*): with U₀ at 5 m/s (chosen
  for illustration; no source gives it), 60° at 10 m/s, 76° at 20 m/s, 80° at the owner's 28 m/s. A blade rolled into a tube of one turn
  shows 2r / w = 1/π, 0.32 of its width (*derived*), about where three hardwoods' crowns stood at
  20 m/s (20 to 37 %). No U₀ for a leaf was found published (section 4): Vogel's leaves were reconfiguring
  at 10 m/s, and C_Y ≈ 1 is where to compute one from a blade's stiffness, which this page did not
  read. These are a reading's curves, to be checked against Vogel's and Shao's pictures, not a model
  read from a source.
- **Where it stops mattering, in pixels** (*derived*; a 60° horizontal field of view assumed): a
  pixel subtends 3.0 × 10⁻⁴ rad at 3840 wide and 6.0 × 10⁻⁴ at 1920. A 10 cm blade (the maple's
  order) spans a pixel at 330 m and 170 m; a fold's relief, half of a 5 cm width at sin 60°,
  2.2 cm, spans a pixel at 72 m and 36 m, and eight pixels at 9 m and 4.5 m. A 5 cm birch leaf
  halves each. So **a fold drawn as geometry matters within about ten metres**, its shading out to a
  few tens, and past where a leaf is under a pixel only its effect on the crown's mean colour
  (more underside, less area) is left, which R233's albedo and the crown's lean already carry.
- **Which builder makes which leaf.** Three builders in `domain/flora/src/kit.cpp` make the
  broadleaves' blades, and `crown_emit_leaves` chooses by the profile's leaf placement
  (`species.cpp`):
  - `build_simple_leaves`: the named branching profiles' leaves — maple (`opposite_simple`), oak,
    elm, beech and the rest of the branching family (`alternate_simple`) — and the willow's
    curtains, the birch's full-detail path and the ancient tree's simple crowns. Each blade is its
    own rectangle of four vertices and two triangles, u from 0 to 1 across it and v along it, with
    **no vertex shared** with another blade (the code says so). `build_compound_leaves` makes ash's
    (`opposite_compound`) and walnut's (`alternate_compound`) leaflets the same way, on the
    unmerged rung only.
  - `build_spray`: whatever crown keeps the `spray` placement (the orchard tree's spurs through
    `emit_spray_part`, the birch's aggregate-card ladder) and a compound profile's merged rungs;
    the conifers' `build_bough` is the same strip, folded. A strip of cards along the twig, two triangles a card, **the twig
    vertices shared** between neighbouring cards, each card u from i to i + 1.
  - `build_leaf_card`: a spray's terminal leaf and the understory's blades. A diamond of four
    vertices whose two triangles share the edge from base to tip, u = 0.5 on it.
- **What two triangles cannot do.** On a rectangle of `build_simple_leaves` or
  `build_compound_leaves` the midrib runs at u = 0.5 from the base to the tip, and the triangles
  share the diagonal from one base corner to the opposite tip corner: a fold about the midrib needs
  a vertex pair on it, two more triangles a blade, so a near blade's leaves cost twice their
  triangles. The same holds for a `build_spray` card. **`build_leaf_card`'s diamond is the
  exception**: its shared edge is its midrib, so a fold there moves the two side vertices and needs
  no new vertex. A roll needs more rows on every builder.
- **What the shading alone could carry.** On the forest's named species (separate blades) a normal
  tilted per vertex across the blade's width — the two vertices at u = 0 one way, the two at u = 1
  the other — cups each blade on its own, since no vertex is another blade's; written by the flora
  stage from the frame's load it costs no triangle, and whether the deformed-vertex pool carries a
  normal for it to write is a question for the row. On a `build_spray` card the twig vertices are
  shared with the neighbouring cards, so a per-vertex normal cannot cup one card without the next;
  there the cup has to come per pixel, from the card's own u (i to i + 1) and the load, a few
  instructions on foliage pixels. Either shows a cupped leaf's light, not its outline.
- **Questions the row must answer, with the evidence each way.**
  - *Geometry near, shading farther, nothing past.* The pixels above favour it; the cost is near
    cards' leaf triangles doubled, which the pool's flora stage pays per cut, as the sway does.
  - *Fold or roll, by leaf form.* Vogel: lobed bases with long petioles roll into cones, compound
    leaves into cylinders, acute bases flutter and tear. A per-material declaration (a fold, a roll,
    neither) keys on the asset, as the engine requires, not on a species.
  - *Which face a reconfigured leaf shows.* Not found; it decides whether W177's shape exposes
    R233's underside, and the two rows share it.
  - *Flutter.* A leaf's own flutter at a few hertz (aspen's 3 to 5 Hz, search summary only) is
    faster than the part's 0.9 Hz sway; Miller et al. and Tadrist et al. say reconfiguration and
    overlap damp it as the wind rises, so a flutter that grows with the wind and then falls once
    the leaf has rolled is what the sources describe. No amplitude was read.
  - *Tattering and loss.* Leaves tear past about 20 m/s at the leaf (Vogel 2009); the engine builds
    none, as it builds no failure.
  - *Still air.* φ is zero up to U₀, so a crown in still air and on a mean day draws what it drew.

## The sources, how far each was read, and under what licence

| source | what is taken | read | licence |
|---|---|---|---|
| Hovi, Raitio, Rautiainen (2017), *Silva Fennica* 51(4) 7753 | both faces of 25 boreal species; abaxial brighter in the visible, darker in the NIR; no side effect on visible transmittance; variance shares; REIP 11.3 and 4.6 nm; conifers' exceptions; methods and dates | read (HTML) | CC BY-SA 4.0 |
| Hovi, Rautiainen (2022), Mendeley Data nvgjcn5nsx v1 | the library: A upper, B lower, sun and shade, birch and aspen May to October | the landing page and the file list; data not read | CC BY 4.0 |
| Kuusinen, Lakaniemi, Hovi, Rautiainen (2022), Mendeley Data 3hswxj5nff v1 | aspen, goat willow, two alders, both faces; one spectrum a row, 350 to 2500 nm | the landing page and the layout; data not reduced | CC BY 4.0 |
| Lu, Lu, You, Wang, Liu, Omasa (2018), *Plant Methods* 14, PMC5812224 | seven species, three seasons; adaxial lower in the visible in every species; crossing at 716 to 732 nm; white poplar and oleaster differ most; the mesophyll's and hairs' account | read (the full text through Europe PMC) | CC BY |
| Lukeš, Neuwirthová, Lhotáková, Janoutová, Albrechtová (2020), *Remote Sens. Environ.* 246, 111862 | five Central European species through a season; one-sided canopies up to 20 % low | abstract | Elsevier; not open |
| Lu, Lu (2015), *Int. J. Remote Sens.* 36, 1447 | white poplar and Siberian elm, both faces | abstract | Taylor and Francis |
| Levizou, Drilias, Psaras, Manetas (2005), *New Phytol.* 165, 463 | trichome removal on white poplar, olive, oleaster | abstract | Wiley |
| Neuwirthová, Lhotáková, Lukeš, Albrechtová (2021), *Remote Sens.* 13, 4144 | sparse hairs, no consistent specular effect | abstract | CC BY (MDPI) |
| Neuwirthová et al. (2023), *Eur. J. Remote Sens.* | the faces' asymmetry changes trait retrievals | abstract | not stated in what was read |
| E-TRAINEE, Heidelberg, "Optical parameters of foliage" | palisade and spongy; white poplar's wax above, hairs below; poplar against linden; Grant's polarisation | read (secondary) | CC BY-SA 4.0 (text) |
| Eller (1977), *J. Exp. Bot.* | lower-surface hairs raise reflectance 400 to 1350 nm | search summary only | — |
| Bousquet, Lachérade, Jacquemoud, Moya (2005) | specular at the surface, diffuse inside | as [foliage](foliage-two-sided-light.md#what-the-literature-says) read it (snippet) | — |
| Jacquemoud, Baret (1990), PROSPECT | one reflectance per leaf, a plate stack | recalled, the model's form | — |
| Stuckens et al. (2009), the dorsiventral leaf model; Shi et al. (2024), PROLIB, *Remote Sens. Environ.* 114140 | separate diffusion for the two faces; both faces to 5.7 µm | search summaries only (PROLIB's title read) | — |
| *Acer saccharinum*, *Sorbus aria*, the encyclopaedia's articles | silver, downy undersides shown in a breeze; a nearly white underside | read | CC BY-SA 4.0 |
| Vogel (1989), *J. Exp. Bot.* 40, 941 | 10 and 20 m/s; drag coefficients falling; cones, cylinders, flutter and tearing by leaf form; white poplar to 30 m/s | abstract (the publisher's page) | Oxford University Press |
| Vogel (2009), *New Phytol.* 183, 13 | tearing past 20 m/s; cones and cylinders; petioles that twist | abstract (Europe PMC) | Wiley |
| Gosselin (2019), *J. Exp. Bot.* 70, 3533 | the exponent's definition; leaf, plate, cone, fibre and poroelastic exponents; table 1; onset at C_Y ≈ 1; Strouhal 0.2 | read (the publisher's HTML, to its references) | © the author, published by OUP, all rights reserved; numbers only taken |
| Miller, Santhanakrishnan, Jones, Hamlet, Mertens, Zhu (2012), *J. Exp. Biol.* 215, 2716 | cones' stable wakes; U-shapes shed more | abstract (Crossref) | not stated |
| Miller, Santhanakrishnan, "Broad leaves in strong flow", arXiv 1310.5615 | 1 to 10 m/s; species; no onset speed | read (ar5iv) | arXiv; no licence stated |
| Miller, Herschlag, Santhanakrishnan (2008), arXiv 0810.1975 | leaves roll into cones, cutting flutter and drag | abstract | arXiv non-exclusive distribution licence 1.0 |
| Tadrist, Saudreau, Hémon, Amandolese, Marquier, Leclercq, de Langre (2018), *J. R. Soc. Interface* 15, 20180010 | flutter low, buffeting high; leaves bend and overlap | abstract (Europe PMC) | not stated in the record |
| Tadrist, Julio, Saudreau, de Langre (2015), *J. Fluids Struct.* | flutter by torsional galloping | title only | — |
| Shao, Chen, Lin (2012), *Acta Mech. Sin.* 28, 583 | London plane's sequence of states | search summary only | — |
| Vollsinger et al. (2005); Rudnicki et al. (2004) | crowns' frontal areas at 20 m/s | as [the wind's page](wind.md#the-trees-in-a-storm) read them | — |
| Aspen flutter at 3 to 5 Hz; Roden (2003); a cottonwood model; a rubber-tree tunnel; a 6 m/s leaf threshold | — | search summaries only, no number used | — |
| Not reached | the faces' values in Hovi et al.'s and Lu et al.'s figures; Eller (1977) and Eller and Willi (1977); Grant (1987); Slaton et al. (2001); Vogel (1989)'s full text; Schouveiler and Boudaoud (2006) itself; de Langre's reviews | — | — |
