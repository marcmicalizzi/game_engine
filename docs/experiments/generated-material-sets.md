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
