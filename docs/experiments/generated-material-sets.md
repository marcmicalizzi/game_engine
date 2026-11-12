# Generated material sets: the grounding (G148, 2026-11-10)

What published work says about each thing the [matgen](../subsystems/matgen.md) capability draws,
the number the generator took, and **how well each was checked**. The owner's rule (2026-10-20) is
that a model of something real starts from its literature; this page is also honest about what
one night's reading reached. Three marks are used:

- **read (secondary)**: the statement was read on a page that quotes or summarises the source; the
  page is named. No primary paper was opened tonight (two came back unreadable).
- **cited, not read**: the standard reference for the statement, from the field's textbooks; the
  number is the commonly quoted one and was **not** checked against the source tonight.
- **chosen**: the generator's own number, with the reason.

Everything marked "cited, not read" is a row under "Not verified" at the end.

## A bed of clasts

**The size scale.** Wentworth's grade scale (Wentworth 1922, "A scale of grade and class terms for
clastic sediments", J. Geology 30:377-392): sand 0.0625 to 2 mm, granule 2 to 4, pebble 4 to 64,
cobble 64 to 256, boulder above 256 mm. Krumbein's phi (Krumbein 1934, "Size frequency
distributions of sediments", J. Sedimentary Petrology): phi = -log2(D / 1 mm), so one phi is a
factor of two. *Read (secondary): Wikipedia, "Grain size".* The recipe's `sorting_milliphi` and
`truncate_milliphi` are in these units.

**What a surface count states.** Wolman's pebble count (Wolman 1954, "A method of sampling coarse
river-bed material", Trans. AGU 35(6)) picks about a hundred stones off the bed and measures each
one's intermediate (b) axis. *Read (secondary): summaries of the method (Potyondy and Bunte's
notes; a state agency's procedure); the paper itself not read.* Kellerhals and Bray (1971,
"Sampling procedures for coarse fluvial sediments", J. Hydraulics Division ASCE 97(HY8):1165-1180)
showed the grid count is the one surface procedure directly comparable to a bulk sieve analysis.
*Read (secondary): the paper's abstract.*

**What follows for a generator** (arithmetic, not a citation). A grid point lands on a clast with
a chance proportional to the area the clast shows, so the count's distribution is the clasts'
distribution weighted by D squared. For a lognormal with ln-deviation s, weighting by D squared
moves the median up by 2 s squared in ln D and leaves s alone. So a recipe that states the
**count's** D50 and sorting draws its clasts **by number** from the same normal in phi moved
**down** by 2 s squared, s = sigma ln 2; the tests then count over every texel (the grid count
in its limit) and hold D50, D84 / D50 and D50 / D16 to the stated ones. The distribution is cut
`truncate` phi either side of the median (two, by default), symmetric in phi so the median stays;
a normal cut at two sigma has its 16th and 84th percentiles at 0.933 sigma, which is the ratio the
test expects. That a gravel surface is near-normal in phi is the usual working approximation
(*cited, not read*: Folk and Ward 1957 for the graphic measures; Bunte and Abt 2001, USDA
RMRS-GTR-74, for gravel-bed practice).

**The default bed** (`gravel/bed`). D50 45 mm, sorting 1 phi, so D16 23.6 mm, D84 85.9 mm, the
cut at 11 and 180 mm: very coarse pebbles to small cobbles, the "rounded cobbles 5 to 20 cm" the
reference reels' catalogue reads off a clear mountain river, at the fine end of it so a one-metre
tile holds a few hundred clasts. *Chosen.* A real armoured bed's surface is coarser than what
lies under it by an armour ratio (surface D50 over subsurface D50) of about 2 where the armour is
well developed (*read (secondary)*: a poster's summary of Hassan et al. 2006); the surface is the
only layer a set draws, so the ratio is not a parameter. Oak Creek's distributions (Milhous 1973;
Parker and Klingeman 1982), which would have pinned a measured D16, D50 and D84, could **not** be
read tonight.

**Form.** Zingg's classes split b/a and c/b at two thirds into discs, spheres, blades and rods;
Sneed and Folk's maximum projection sphericity is (c^2 / (a b))^(1/3) (Sneed and Folk 1958,
"Pebbles in the lower Colorado River, Texas", J. Geology 66:114-150). *Read (secondary): both
formulas, in summaries; Zingg 1935 itself not confirmed.* The generator draws b/a from 0.55 to
0.90 and c/b from 0.40 to 0.85, independently (*chosen*: across the sphere and disc and blade
classes about the two-thirds lines), which gives a mean sphericity of 0.65 to 0.66; the test
holds it to 0.60 to 0.75, the range river pebbles are usually quoted in (*cited, not read*).
A clast lies on its a-b plane, so the plan view shows a and b and the height c.

**Roundness.** Krumbein's visual chart (1941) runs 0.1 to 0.9; Powers (1953, "A new roundness
scale for sedimentary particles", J. Sedimentary Petrology 23:117-119) names six classes on a
logarithmic scale. *Read (secondary): the six classes and their logarithmic spacing (Wikipedia,
"Roundness (geology)"); the class limits commonly quoted (very angular 0.12 to 0.17, angular 0.17
to 0.25, subangular 0.25 to 0.35, subrounded 0.35 to 0.49, rounded 0.49 to 0.70, well rounded 0.70
to 1.0) are cited, not read.* River transport rounds (alluvium of large rivers highly rounded,
of small ones less; *read (secondary)*, qualitative). The defaults: 0.60 for the bed and 0.65 for
the bar's pebbles (rounded), 0.20 for scree (angular: it fell, it was not carried). *Chosen
within the classes.* In the generator roundness sets the outline (a circle cut by a polygon whose
edges stand at the circle for 1 and at the inscribed polygon for 0) and the top (an ellipsoid's
cap against a bevelled, ridged plateau).

**Imbrication.** Clasts in a gravel bed dip upstream, a-b plane against the flow, and rolled
clasts lie with the a-axis across it. *Read (secondary): Wikipedia, "Imbrication
(sedimentology)", and a 2024 paper on a gravel bar's fabric.* The dip: the figures found tonight
put fluvial means near 30 to 35 degrees with ranges of 12 to 55 (*read (secondary)*, search
summaries only, low confidence; Rust 1972 and Johansson 1976 are the primaries, *cited, not
read*). The default is 30 degrees, each clast between half and one and a half times that.
**Traded:** a set is a height field, and a top that leans cannot show more rise than the clast's
own thickness, so each lean is capped at 0.8 c / a — about 20 degrees for the mean clast. The
shingling reads; the true dip does not fit in a height field.

**Scree.** Scree is sorted by fall, coarser toward the foot, and a slope stands near its angle of
repose only where it is being fed or cut (*read (secondary)*: Wikipedia, "Scree", which cites
Statham 1976, "A scree slope rockfall model", Earth Surface Processes 1:43-62, and Kirkby and
Statham 1975, J. Geology 83:349-362; neither read). That talus is openwork — clast on clast, no
matrix — and angular is the textbook description and was **not** confirmed in what was read
tonight. The default (`rock/scree`): D50 110 mm, sorting 0.9 phi, roundness 0.2, flatter and
longer blocks (b/a from 0.5, c/b 0.3 to 0.8), no imbrication, each block tilted up to 18 degrees
about any axis, and `openwork` 1: the gaps are voids over a lower layer of blocks, not a filled
matrix. One set cannot follow the fall sorting down a slope; that is the placement's (rocks.md,
the deposits), which chooses which set a cone's foot and head take.

**Wet against dry.** A rough absorbing surface is darker under a film of water because light
scattered at the surface is partly trapped by total internal reflection in the film (Lekner and
Dorf 1988, "Why some things are darker when wet", Applied Optics 27:1278-1280, extending
Angstrom 1925); Twomey, Bohren and Mergenthaler (1986, Applied Optics 25:431-437) explain it by
more forward scattering, which Zhang and Voss (2006) did not find borne out. *Read (secondary):
abstracts and summaries; the one number read is Angstrom's sand, albedo 0.182 dry and 0.091 wet.*
The generator multiplies every albedo by `1 - 0.5 wet` and moves roughness toward `0.2 + 0.25
roughness` (the film's highlight over the stone's). **Not modelled:** a dark surface darkens
proportionally less than a pale one (the models above say so; one factor is used for all).

**Albedo by rock.** The lithologies' linear albedos are inside broadband textbook ranges —
granite 0.20 to 0.35, limestone 0.35 to 0.55, dry sand 0.35 to 0.42 — which are **not** visible-
band figures and were read on course and reference pages, not in a spectral library. Basalt
(0.07), vein quartz (0.63), schist (0.12) have no figure read at all. *Cited, not read*: the USGS
Spectral Library (version 7) and the ECOSTRESS library are where each should be checked.

## A rock face

**Granite's minerals.** A granite has quartz as 20 to 60 % of quartz + alkali feldspar +
plagioclase; a monzogranite has alkali feldspar as 35 to 65 % of its feldspar (the IUGS
classification; Le Bas and Streckeisen 1991, J. Geol. Soc. 148:825-833). *Read (secondary):
Wikipedia, "Granite".* The default's modal shares — quartz 28, alkali feldspar 30, plagioclase
32, biotite 10 — give quartz 31 % of the three and alkali feldspar 48 % of the feldspar: a
monzogranite. *Chosen inside the field.* The minerals' colours (grey quartz, a cream-pink alkali
feldspar, white-grey plagioclase, black biotite) are hand-specimen descriptions; their albedos
are *chosen*, and the weathered face's mean is held to the broadband granite range above.

**Grain.** 3.5 mm, medium-grained by the usual limits for phaneritic rocks (fine under 1 mm,
medium 1 to 5, coarse above; *cited, not read* — the BGS Rock Classification Scheme is where to
check). At a 1.5 m tile and 1,024 texels a crystal is 2.4 texels; at 2,048, 4.8.

**Joints and relief.** Two joint sets across each other, their traces 0.67 m apart on a 1.5 m
tile (*chosen*; a granite outcrop's joint spacing was **not** found tonight), each trace shown
only along part of its length. Micro-relief of 4 mm half range at the decimetre scale (*chosen*).

**The rind and the breaks.** A weathered face mutes the crystals' contrast and stains toward
grey-brown; a fresh break shows the crystals and sits a flake lower. The share of each is a
parameter (`rind_permille`: 80 % by default, 92 % for the default granite, an old face), the
patches' shape a low noise cut at the share's quantile. *Chosen*: no source was read for rind
colour or the share of a face that is fresh.

**Lichen by exposure: a parameter.** *Rhizocarpon geographicum*, the yellow-green map lichen of
exposed siliceous rock, grows about 0.6 to 0.9 mm a year in diameter once past its first few
millimetres (Bradwell and Armstrong 2007, "Growth rates of Rhizocarpon geographicum lichens: a
review with new data from Iceland", J. Quaternary Science, DOI 10.1002/jqs.1058: a mean of 0.64 mm a year in
southern Iceland; *read (secondary)*: the abstract). So the thalli the generator lays — discs 12
to 60 mm across — are decades to a century old, which is what "exposure" means here: the recipe's
`lichen_permille` is the cover of the rind, and a face fresh from a fall has none (no thallus is
drawn on a fresh break). It is a parameter of the set, not a mask: a mask over a mesh's faces by
aspect and age is the cover capability's (cover.md), which places lichen where the mesh is
exposed. Cover percentages by age were **not** found tonight; 12 % is *chosen*.

## What G149 to G153 added (2026-11-11)

One research pass (published sources only) reached almost no primary text: most papers came back
as PDFs the reader could not decode, so the marks below are mostly *read (secondary)* or *chosen*.

**Repetition** (G153). The tiling literature states the goal — output "with no unnatural
artifacts or repetition" is how Wei, Lefebvre, Kwatra and Turk (2009, "State of the art in
example-based texture synthesis", Eurographics STAR) frame it, and Cohen, Shade, Hiller and
Deussen (2003, "Wang Tiles for Image and Texture Generation", ACM TOG 22(3)) exist to avoid
visible periodic repetition — but *read (secondary)*, neither gives a rule for a tile's size
against its largest feature, and none was found that does. So the measure (the largest departure
of a sixteenth of the tile from its mean, as a Weber contrast) and its bounds are **chosen**, and
the bounds were set where the base generator's sets fail them (matgen.md, "Repetition").

**Embeddedness.** Platts, Megahan and Minshall (1983) define it as the degree to which the larger
particles are surrounded or covered by fine sediment, in classes 0 to 25, 25 to 50, 50 to 75 and
over 75 % (*read (secondary)*: the EPA's rapid bioassessment habitat table, which cites them).
The default bed's 0.30 is in the second class: *chosen within it*.

**Biofilm.** Periphyton films on stable stones are green, gold or brown, dark brown mats under
stable flow, and bed material of 2 to 16 mm is the main agent that scours them off (*read
(secondary)*: New Zealand's periphyton monitoring guides and a summary of an abrasion study). So
the film lies on the crowns and not the sides, and a fifth of the clasts carry none. Its tint is
*chosen*.

**Talus.** That coarse talus ranges from openwork with large voids to clast-supported with every
void filled, and fills with fines as it ages and with depth, is *read (secondary)* (Selby 1982 as
cited; Scapozza, Baron and Lambiel 2015, Permafrost and Periglacial Processes 26:67-83, borehole
air content falling with depth as a fine matrix fills the voids). **No porosity figure was
found**: the three layers, the 1.2 median thicknesses between them and the 15 % of voids filled
are *chosen*.

**Lichen colonies.** Crustose thalli grow radially from a centre (the review of Armstrong and
Bradwell 2010, "Growth of crustose lichens: a review", Geografiska Annaler A 92(1):3-17; *read*:
the abstract, which gives no numbers). *Read (secondary)*: Armstrong's 1983 sample in North Wales
held thalli of 2 to 65 mm across; the largest living *Rhizocarpon* thalli run past 150 mm;
*Xanthoria elegans* grows about 0.5 to 0.7 mm a year in radius for its first decades (Osborn and
Taylor 1975, as summarised). The defaults' largest thallus (120 mm for granite, 60 to 80 mm for
the others) and the skew to small thalli are *chosen* within that; the orange species is added
because the reels' boulders (r04) carry it. Colonies meeting at a line is *chosen*, from
photographs, not from a source found.

**Joints.** A Nigerian granite outcrop's mapped joint spacing averaged 1.6 to 2.5 m over all sets
(Saliu and Alade 2012, *read (secondary)*); sheeting joints are centimetres apart near the surface
and metres at depth (*read (secondary)*). The tile's 0.67 m between traces is closer than the
outcrop's: a 1.5 m tile has room for two traces a set, and a tile with fewer would show no joint
on most of a block. **Traded**: denser joints than a real face for a set that must show one on a
small mesh. Their worn profile (a floor of fines, rounded shoulders, four widths deep) is
*chosen*.

**Run-off streaks.** Black stripes where water runs off a rock face, formed mostly by lichens
with green algae or cyanobacteria ("Tintenstriche"; Pittino et al. 2024, Frontiers in
Microbiology, doi 10.3389/fmicb.2024.1430059, *read (secondary)*), driven by the rock's water
retention. **No width or length was found**: 0.45 m long, strands 3 cm across, are *chosen*.

**Form above the decimetre.** No source was read: scallops 0.33 m (at 0.15 of the form at a
cell's rim), ribs 0.5 m, and the amplitudes (granite 22 mm) are *chosen* to give a face a shape
the normal map shows at 15 m. A broken step of 1.4 times the form was drawn too and removed
(`k_matgen_version` 3): its curved contour, cut by the rock sets' stochastic sampling, read as
crescent stamps in the engine (matgen.md, "Rock sets").

## A floor

The parts and their numbers are the sward's floor's ([grass-sward.md](grass-sward.md#the-floor-under-a-sward-g141-2026-11-10),
G141): an underlay of moss and soil split at the moss share's quantile, dead leaf fragments 3 to
9 cm long and 1 to 4 mm wide as a Poisson cover (N fragments of mean area L w cover a share s
with probability 1 - exp(-N L w / A), so N = -ln(1 - s) A / (L w)), and the same linear albedos:
straw 0.18, 0.145, 0.09; moss 0.045, 0.075, 0.025; a moist loam 0.085, 0.065, 0.045; a live
grass leaf 0.055, 0.12, 0.035. This row adds what a set needs that a bake under blades did not:
moss drawn as shoot tips (a cellular pattern 1.8 mm across, paler at each tip), soil as crumbs
(5 mm, dark between), a fifth of the straw weathered grey, and live leaves that taper with a
paler midrib for the turf.

## A floor at standing height (G187, 2026-11-11)

G149's pictures showed the four ground sets as flat felt colour at 1.7 m: every structure in them
was a few millimetres across (a leaf, a crumb, a shoot tip) on a half-metre tile, and nothing
varied between 5 cm and 1 m, which is the band a standing eye reads a floor by — tufts, cushions,
clods, a lawn's patches. One research pass (a sub-agent, 2026-11-11 evening) looked for that
band's numbers. **Its fetch tool could not decode a PDF**, so everything below is **read
(secondary)** at best — an abstract or a search summary quoting the source — and several numbers
were not found and are **chosen**, said so in each row.

| What | Source and how it was read | The generator's number |
|---|---|---|
| A grazed montane sward stands 5 to 7 cm (short-sward alpine grazing; the eastern Alps' short-grass pastures 6 to 7 cm; upland grazed grassland 3.3 to 11.8 cm) | Inno4Grass leaflet, HBLFA Raumberg-Gumpenstein's pasture pages, a Grassland Science in Europe paper; read (secondary) | live leaves 40 to 110 mm long lying in the turf (chosen within it: a leaf lies longer than the sward stands) |
| A grazed sward is a mosaic of short, frequently grazed patches and tall, rejected ones; the short share falls from 0.72 to 0.19 as stocking falls | the Solling (Göttingen) long-term patch-structure study, abstract; read (secondary) | a patch field of 0.6 m and 0.2 m octaves sets each tuft's height, hue and dead share (`patch_permille` 600); **patch sizes in metres were not found**: 0.6 m is chosen |
| Tussock grasses: *Nardus stricta* tussocks under 140 cm² basal area (about 13 cm across) are common under short grazing; *Festuca* short-tussock grassland spaced regularly at about 20 cm | Common et al. 1998 and Grant et al. 1996 (BES Applied Ecology Resources records), Dickinson and Norton 2011 (NZ J. Ecol. 35, abstract); read (secondary) | a tuft (`tuft_mm`) of 110 mm, its 6 to 20 leaves fanning from its centre |
| Standing dead material in a pasture's herbage: about 15 % of its mass (a lowland ryegrass pasture; montane not found) | a Massey University thesis; read (secondary) | `dead_permille` 200 of the live layer's leaves, more in the tall tufts (chosen: a montane sward carries more dead than a fertile lowland one) |
| Thufur (earth hummocks) in subalpine grassland: 9 to 27 cm high (mean 16.9), about 1 m across (Mt Halla); 15 to 19 cm in a Rocky Mountain alpine basin | Mountain Research and Development 28(3), 2008; a 2007 Arctic, Antarctic and Alpine Research paper; read (secondary) | **not drawn at that height**: a hummock is the ground's geometry, not its texture. The set carries mounds of 0.9 m spacing and 20 mm half range (chosen) so the normal map shows the floor is not a plane at 15 m |
| A soil's reflectance falls with its water content, nonlinearly, similarly across soils as degree of saturation | Lobell and Asner 2002 (SSSAJ 66), abstract; read (secondary) | moist patches and hollows to 0.55 of the dry tone (chosen; the factor was not read) |
| Soil albedo from Munsell value, about 0.069 V - 0.114 (a moist dark topsoil of value 3 near 0.09) | Post et al. 2000 (SSSAJ), abstract; read (secondary) | the loam's 0.085, 0.065, 0.045 (G141's) stays: its luminance 0.068 sits at Munsell value 2.6 to 3 |
| Surface rock fragments: gravel cover 50 to 65 % at an alpine site, 18 to 31 % by volume in an alpine steppe soil | a USDA ecological-site description, an alpine-steppe soil paper; read (secondary) | `stones_permille` 80 on `ground/soil` (a meadow's bare soil, not a scree's: chosen well under the alpine site's gravel), 5 to 40 mm |
| A soil surface's random roughness, aggregates and crack spacing | not found as numbers (Römkens and Wang 1986's classes and the crack spacing of about three layer thicknesses were read as concepts) | clods of 35 mm with cracks between (chosen), 5 mm relief |
| Moss carpets' hummock and cushion sizes, moss reflectance, lit tips against a shaded interior | **not found** as numbers (Salko et al. 2023's *Sphagnum* spectra hold the reflectances; Rydin and Jeglum 2006 put lawns 5 to 20 cm above the water table) | cushions of 220 mm (version 7; 150 mm before) and 8 mm relief that overlap and merge, their tone blended across them between a lit yellow-green (0.085, 0.115, 0.030), the shaded body (G141's 0.045, 0.075, 0.025) and a dry brown (0.080, 0.065, 0.035): **chosen** |
| Litter darkens with decay, its colour changing mostly in its first year | a Beijing temperate-forest litter colour study (L\*a\*b\*), abstract; read (secondary); values not found | two litter populations: decayed below (0.065, 0.050, 0.036, chosen) and fresh straw over it (G141's), the fresh share set by the patch field |

What the eye reads at standing height, in the generator's terms: a tuft or a cushion is a cell a
decimetre across with its own tone and a dark seam round it; the hollow between them is baked
into the colour by the finishing pass's shade, now full at a centimetre under the surroundings
read over two centimetres (it was 4 mm over 6 mm, a crumb's scale); and the mosaic's patches are
large enough that a 2 m tile holds a dozen of them, so the tile is now **2 m at 2,048 texels**
(1 mm a texel). The measures that say the floor is no longer flat are in matgen.md ("A floor at
standing height").

## Weathered wood (G188, 2026-11-11)

Two sets for the bridges' timber (`wood/plank`, sawn boards; `wood/log`, the outside of a log),
grain along the image's u. Same research pass, same caveat: **read (secondary)** at best.

| What | Source and how it was read | The generator's number |
|---|---|---|
| Unfinished wood outdoors greys within months: UV degrades the lignin, rain leaches the products and extractives, staining fungi darken it; colours converge to a common grey whatever the species | the Wood Handbook's finishing chapter (Williams, FPL-GTR-190 ch. 16, 2010); Feist's 1992 FPL article; a SINTEF/NTNU colour study; read (secondary) | a grey of (0.21, 0.20, 0.185) linear, reached by `grey_permille` 800 to 850 with a 0.25 m patch field of ±0.15 (a board's) or ±0.2 (a log's) leaving browner wood where it is less weathered (chosen; L\*a\*b\* of weathered softwood not found) |
| Surface erosion about 6 mm a century for softwoods (Browne 1960); 5 to 10 mm vertically south-facing (Sell 1986); measured 34 to 101 µm a year over 16 years | Williams, Knaebe, Sotos and Feist 2001 (Wood and Fiber Science 33, Part I), abstract; read (secondary) | — |
| Earlywood erodes faster than latewood for the first seven years or so (33 to 58 µm a year by species), leaving the latewood standing as ridges | Williams, Knaebe and Feist 2001 (Part II), abstract; Arnold, Lemaster and Dost 1992 (Wood and Fiber Science 24); read (secondary). **The ridges' depth was not found**; the rates' difference over seven years gives about 0.15 to 0.2 mm | latewood proud by 0.4 mm on a board, 0.3 mm on a log (chosen: an old bridge's boards are past seven years, and the edges round) |
| Checks on weathered Scots pine boards: many under 3 mm deep and 10 to 30 mm long, deeper ones 3 to 10 mm deep and 30 to 150 mm long; radial faces check less | Venäläinen (Luke, Finland); read (secondary). Widths not found | board checks 10 to 150 mm long, up to 1.2 mm wide and 3 mm deep, 30 to the square metre (chosen within the lengths) |
| Ring width: spruce 1.12 mm (0.40 to 3.30), larch 1.53 (0.35 to 8.65), pine 1.52 (0.40 to 4.90); latewood 38 % of a spruce ring, 37 % pine, 33 % larch | Czajka (Drewno 197); read (secondary) | latewood 380 of a band. A flat-sawn face cuts the rings at a shallow angle, so it shows each ring several times wider than it is: the face's bands are 8 mm (chosen), and they wander 45 mm across the grain over 0.7 m along it, so they fold into the arches of flat-sawn figure where the face runs along a ring |
| Spiral grain: Norway spruce 2.1 to 2.6°, Scots pine 1.4 to 2.0° under bark | Hannrup et al. 2003 (Silvae Genetica 52); Hallingbäck et al. 2008; read (secondary) | a log's fibres climb 2.2° |
| Drying checks in a log are radial, narrow toward the pith and widen as it dries (tangential shrinkage more than twice the radial) | secondary summaries; widths up to 38 mm only anecdotal | log checks 150 to 900 mm long, up to 5 mm wide and 15 mm deep, 6 to the square metre (chosen) |
| Knots in graded timber are limited by knot-area ratios; diameters and frequency per metre are in EN 1309 / DIN 4074 tables | not read | 2 knots a square metre of board, 12 to 40 mm across, a third encased with a dark rim; a log's 1 a square metre, 25 to 70 mm (chosen) |
| Norway spruce bark is 9 to 32 mm thick (double) from 12.5 to 92.5 cm diameter | a Bosnian 393-tree study; read (secondary). Plate sizes and fissure depths **not found** | a log keeps bark on 15 % of its surface, plates 45 mm long up the stem, 6 mm proud (chosen) |

## The ranges the defaults are held to

`matgen_tests.cpp` holds each default's mean albedo (luminance of the mean linear colour, after
the hollows' shade and the wetness) and mean roughness to a range. The albedo ranges are the
material's range above times what the generator states it does to it:

| Role | Albedo range | From |
|---|---|---|
| `gravel/bed` | 0.06 to 0.20 | a mixed bed, 0.2 to 0.4 dry, wet through (times 0.5: a channel's bed lies under its water) and under its hollows' shade |
| `sand/bar` | 0.25 to 0.42 | dry sand 0.35 to 0.42, less the granules' shade |
| `rock/scree` | 0.12 to 0.35 | grey rock 0.3 to 0.4 with its voids in shade |
| `rock/granite` | 0.15 to 0.35 | granite 0.20 to 0.35, its rind and lichen below the fresh rock |
| `rock/granite_wet` | 0.06 to 0.18 | half of the dry |
| `rock/basalt` | 0.03 to 0.12 | chosen; no figure read |
| `rock/limestone` | 0.20 to 0.50 | limestone 0.35 to 0.55 under a grey rind |
| `rock/sandstone` | 0.18 to 0.45 | chosen about a buff quartz sand |
| `ground/soil`, `ground/moss` | 0.03 to 0.10 | the sward floor's moist loam and moss |
| `ground/thatch` | 0.08 to 0.20 | the sward floor's straw |
| `ground/turf` | 0.05 to 0.14 | live leaves over thatch |
| `wood/plank` | 0.10 to 0.30 | weathered grey softwood, its checks, knots and stains below the grey (chosen about the grey's 0.20) |
| `wood/log` | 0.08 to 0.28 | the same with bark patches and deeper checks |

## Not verified

Each of these is a number in the generator that rests on the literature as commonly quoted and
was not read in its source on 2026-11-10; each should be checked, and the default moved if the
source says otherwise:

- A measured gravel-bed surface distribution (D16, D50, D84) and its sorting, to replace the
  chosen D50 and one phi (Milhous 1973; Parker and Klingeman 1982; Bunte and Abt 2001).
- The mean sphericity of river pebbles, and the roundness of river gravel against talus as
  numbers (Sneed and Folk 1958; Powers 1953's class limits).
- The imbrication dip's mean and range from a primary source (Rust 1972; Johansson 1976).
- Talus as openwork and angular, its block size range, and its repose angle (Statham 1976;
  Kirkby and Statham 1975).
- A wet-to-dry albedo factor by dry albedo, in place of one 0.5 (Lekner and Dorf 1988, read whole).
- Visible-band albedos of every lithology, mineral, lichen, moss and litter here, from a spectral
  library.
- Granite's grain-size class limits, joint spacing in outcrop, and lichen cover against exposure
  age.
- (G149 to G153, 2026-11-11; none of these was read in its primary either.) Platts, Megahan and
  Minshall 1983's embeddedness classes, read whole; a talus's void fraction and how fast fines
  fill it with age (Sass 2006; Scapozza et al. 2015 read whole); crustose thallus size
  distributions on old faces and the growth rate of *Xanthoria elegans* (Armstrong and Bradwell
  2010 read whole, its PDF was not decodable; Osborn and Taylor 1975); the width and length of a
  run-off streak (Pittino et al. 2024 read whole); a rule for a tile's size against its largest
  feature, if the tiling literature has one (Wei et al. 2009 read whole).
