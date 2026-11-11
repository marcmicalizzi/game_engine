# The formation's sequence: members, drawn beds, and variation along strike (W94, 2026-11-10)

The forest proof of concept's solver needs (2.5, item 3) ask for "thickness change, pinch-outs and
lenses, offsets at joints and faults, and a seeded bed-sequence generator past the 8-bed cap (a fine
sequence repeats within about 15 m)", and the owner has raised the banding's regularity three times.
This page is what the generator and the variation rest on: how thick beds are and how they are
ordered, how a section groups into the cliffs and slopes a hillside shows, how a bed changes along
its strike, and what of all that a camera can see at ten metres, a hundred and a kilometre. The
field itself (the coordinate, the fold, the box filter) is grounded on
[formation-field.md](formation-field.md); the mechanism is `engine.scene.Formation` version 2
(`schemas/scene.schema`), packed and evaluated by `domain/gfx/formation.h` and drawn by
`formation.slang` ([renderer](../subsystems/renderer.md#formation-bands)).

**How the sources were read.** One evening, through abstracts, records and secondary summaries: the
full texts of Talling, Krumbein and Dacey, Gibling, Rothman and others were behind publishers'
walls. Each line below says what was read. Where a number is the generator's own choice inside a
range a source gives, or a choice no source was read for, the line says so; nothing here is quoted
as measured that was not.

## How thick a bed is

- **The classes.** A bed is a layer of a centimetre or more; under that is a lamina. McKee and Weir
  (1953, *GSA Bulletin* 64:381-390), as tabulated on
  [Bed (geology)](https://en.wikipedia.org/wiki/Bed_(geology)) (secondary): very thin 1-5 cm, thin
  5-60 cm, thick 60-120 cm, very thick over 120 cm. The generator's floor is the centimetre
  (`generate_formation` clamps a draw to 10 mm).
- **The distribution is skewed and long-tailed, and which law fits depends on the setting.**
  Lognormal, or a mixture of lognormals, for turbidites: Talling (2001, *Sedimentology*
  48:1297-1329, the bibliographic record and later summaries) on the Marnoso-arenacea; Sylvester
  (2007, *Sedimentology*, summary only) on the Tarcau Sandstone, with a warning that log-log least
  squares is the wrong way to fit these. A power law for the number of beds thicker than h, with an
  exponent near 1 and reported between about 1/2 and 2 from deposit to deposit: Rothman, Grotzinger
  and Flemings (1994, *J. Sedimentary Research* 64:59-67, abstract summaries). Exponential frequency
  (many thin beds, few thick) across lithofacies, named units and peritidal cycles: Drummond and
  Wilkinson (1996, *J. Geology* 104:1-18, abstract). Carlson and Grotzinger (2001, *Sedimentology*
  48:1331-1351, abstract) read the lognormal as a power-law input filtered by erosion and
  amalgamation.
- **What the generator takes.** A lognormal about a median, per lithology, per member: the one
  family all four lines of work allow, with two numbers an author can read off an outcrop (the
  typical bed, and how unequal the beds are). **The parameters are not from a source**: no median or
  log standard deviation was read. The default log standard deviation, 0.7, is the one W85's fixture
  used; it puts 95% of a member's beds within a factor of four of the median either way (0.25 to 4
  times), which spans two of McKee and Weir's classes, as a described section typically does. The
  range is 0 (every bed its median) to 1.5. The deviate is the sum of six uniforms (Irwin-Hall),
  scaled to unit variance: within 4.25 standard deviations, exact in integers, and lighter in the
  tails than a normal (an excess kurtosis of -0.2), so the generator under-draws the very thick bed
  a power-law tail would give. Said plainly: a turbidite fan's occasional ten-metre bed among
  decimetre beds is not what this draws.

## How a sequence is ordered

- **Krumbein and Dacey (1969, *Mathematical Geology* 1:79-96, abstract level).** Two Markov models
  of a section. A chain sampled at a fixed interval has a diagonal (a lithology may follow itself),
  and simulating it always gives geometric thicknesses, so it fits only sections whose thicknesses
  are geometric. An **embedded chain** records only the changes of lithology, so its diagonal is
  zero, and the thickness of each unit is drawn separately from whatever distribution fits. The
  second is the one to use when thicknesses are lognormal.
- **A published embedded matrix** (Gingerich 1969, *J. Sedimentary Petrology* 39:330-332, as
  reproduced through Powers and Easterling 1982 on a secondary page that labels the four states only
  A to D, so the lithologies are not known here): upward transition counts A to B 37, A to C 3, A to
  D 2; B to A 21, B to C 41, B to D 14; C to A 20, C to B 25; D to A 1, D to B 14, D to C 1. What it
  shows without its labels: a real section's order is strongly non-uniform (A is followed by B 88%
  of the time) but not fixed.
- **What the generator takes.** Each member is an embedded chain of **two** states, a resistant
  lithology and a recessive one, with independent lognormal thicknesses. Two states with a zero
  diagonal alternate, so the order inside a member is fixed and all of its irregularity is in the
  thicknesses; the order at the scale that reads on a hillside is the **members'**, which the author
  lists. This is the smallest model that is Krumbein and Dacey's, and it is short of the matrix
  above: a third lithology that follows the first only sometimes (a coal, a conglomerate, a
  limestone marker) is not drawn. The thickness shares come out as the description says because both
  lithologies share one log standard deviation, so their mean thicknesses are in the ratio of their
  medians: the dominant lithology's median is `bed_m`, the other's `bed_m * minor / major`.

## Members: what a hillside shows as cliff and as slope

- **The Grand Canyon's section** (thicknesses as given on
  [Geology of the Grand Canyon area](https://en.wikipedia.org/wiki/Geology_of_the_Grand_Canyon_area),
  secondary): Kaibab Limestone 90-100 m, ledgy cliffs; Toroweap 60 m, ledge and slope; Coconino
  Sandstone 17-183 m, cliff; Hermit 30-274 m, slope; the Supai Group's four units 30-90 m (slope),
  about 90 m (cliff and slope), 30-60 m (ledge and slope) and 60-200 m (ledge and cliff); Redwall
  Limestone 120-240 m, cliff; Muav 41-252 m, cliff; Bright Angel Shale 82-137 m, slope; Tapeats
  Sandstone 30-100 m, cliff. So: **a member is tens of metres to a couple of hundred, cliff-formers
  and slope-formers alternate, and the units between ("ledge and slope") are a real third kind** —
  thin resistant beds in a weak interval.
- **What the generator takes.** A member is a thickness (0.5 to 1,000 m) and one number,
  `resistant`, the share of its thickness in resistant beds. Near 1 it is thick resistant beds
  parted by thin recessive ones, or by bedding planes alone at 1 (a cliff-former); near 0 thick
  recessive intervals with thin resistant ledges (a slope-former); in between, the ledge-and-slope
  kind. Its macro bed's resistance is `0.2 + 0.65 * resistant`, so the line the rocks' rules already
  draw at a half (`k_formation_cliff_resistance`) falls at a share of 0.46. **No source was read for
  the share at which a member turns from slope to cliff**; the half is the engine's existing
  convention and the report says so. Schumm and Chorley (1966) on scarp recession was not
  reachable.
- **The reels** (the owner's references, looked at before the design: r12, r23, r25). r23's
  amphitheatre is a lava pile: dark flows metres to ten metres thick standing as walls, thin red
  interflow beds and green benches between, no two flows the same thickness, each traceable right
  round the bowl while thickening and thinning, a few ending. That is a member with `resistant` near
  0.9, a wide spread, and the variation below.

## Variation along strike

- **Beds are continuous for a long way and change slowly.** Amy and Talling (2006, *Sedimentology*
  53:161-212, abstract): Marnoso-arenacea beds correlated 120 km along the basin and 30 km across;
  almost all thick beds cross the whole outcrop, thinning gradually; debrites end abruptly. So the
  default is a top that moves, not a bed that ends.
- **Lenses have a width for their thickness.** Gibling (2006, *J. Sedimentary Research*
  76:731-770, abstract and secondary summaries; the class table was not read): over 1,500 fluvial
  bodies, width-to-thickness from a few (valley fills, 2.5) through about 10 for channel fills to 70
  to 250 for point-bar sheets and several hundred for incised valleys. The generator's lens is a
  bed that stands whole for a stretch and is gone for a stretch, each about a cell of the lateral
  field: with the default 150 m cell and a 1 m bed that is a width-to-thickness near 150, inside
  the sheet range and above a single channel's. `lateral_m` is the author's (10 m to 100 km).
- **Which beds are lenses.** In a sandstone the shale breaks are the discontinuous beds, and their
  continuity differs by environment (Zeito 1965, record only; White and Willis 2000 warn that
  outcrop lengths are biased both ways); in a mudstone the sandstone bodies are (Gibling). So a
  member's **minor** lithology supplies its lenses, `lenses` of them (default a quarter; no
  distribution of lens share was read).
- **Faults.** Maximum displacement scales with trace length with an exponent near 1 (Cowie and
  Scholz 1992; Schlische and others 1996, *Geology* 24, on 201 small normal faults; Kim and
  Sanderson 2005; secondary summaries, coefficients not confirmed), and displacement is commonly
  under a hundredth to a few hundredths of the length. The field has **one** fault: a vertical
  plane and a throw (to 1,000 m). A population of small faults with lengths and tips, and joints
  that offset a bed by centimetres, are not here.
- **What the field takes.** A bed's **top** moves along its normal by `amplitude * g(p)`: g is one
  of two seeded fields in [0, 1], value noise on a lattice of `lateral_m` (and 2.618 times it) along
  a horizontal direction 35 degrees one side of the strike (and 50 the other), sharpened so a top
  rests at either end of its travel over about a third of the ground each. An ordinary top's
  amplitude is up to `variation` (default 0.3, at most 0.45) of the thinner of the two beds it
  parts, up or down; a lens's is its whole thickness, downward. Two directions rather than the
  strike alone, because with flat beds a field along one axis leaves every wall that runs along the
  other with beds that never change.
- **Compensation.** A top that moves down thickens the bed above by what the bed below lost: the
  section's thickness is the same everywhere (the sequence's first and last tops and every member's
  top do not move), which is the first-order behaviour of compensational stacking and also what
  keeps the period, and so the precision argument, whole.

## What reads, and at what distance

A pixel at 1080p and a 60 degree field is about a thousandth of the distance. Across the beds:

| Distance | A pixel | What is resolved | What the eye checks |
|---|---|---|---|
| 10 m | 1 cm | every bed, a parting of a few centimetres | the contact's sharpness (`soft_m`), a bed's own tone against its neighbour's (`tone`), a top that is not a ruled line |
| 100 m | 10 cm | beds over 20 cm; thin partings are a darker line | that no two beds are one thickness, that a ledge ends (a lens), that the pattern does not come round again up the slope |
| 1 km | 1 m | members, and the thickest beds of a cliff-former | cliff against slope, the members' unequal thickness, a fault's step; the beds are their member's mean |

The repeat is the thing a hundred metres shows: W85's sequence came round every 4.83 m in the
fixture and about 15 m in the proof of concept, several times in one view. A drawn section repeats
after the members' whole thickness, 264 m in the test fixture, which is more than most hillsides cut.

## The generator

`gfx::generate_formation` (formation.h), for each member in order:

1. The major lithology is the resistant one when `resistant` is a half or more. Its median is
   `bed_m`; the minor's is `bed_m * (1 - major) / major`, and under a centimetre the minor is left
   out (a member at 0 or 1 is one lithology in beds).
2. The first bed's lithology is a hash bit; after it they alternate.
3. A bed's thickness is `round(median_mm * exp(sigma * z))` millimetres, z the six-uniform deviate
   of the bed's key, at least 10 mm; the bed that would pass the member's top takes what is left, so
   the members' tops are exactly where the description put them.
4. Its tone is its lithology's tint times `1 +- tone`, its resistance 0.85 or 0.2 within 0.1, its
   roughness offset -0.04 or +0.08, its top's travel a uniform share of `variation`, and a minor bed
   is a lens with probability `lenses` (never a member's last bed).
5. More than 512 beds in all is refused with a sentence naming the member.

Every step is an integer hash (splitmix64), `det::exp`, or whole millimetres, so the table is the
same bytes on every compiler (`formation_tests.cpp` pins the block's hash). The shader never runs
the generator: it reads the table.

## The defaults by setting

The defaults (a metre bed, a log deviation of 0.7, tops that travel 0.3, a quarter of the minor
beds lenses, a 150 m lateral length) are a shallow-marine sandstone and shale, the case the
sources above describe best. They are not every setting's. What a member of another kind should
say, as **guidance from the classes and the reels, none of it measured**:

| Setting | `bed_m` | `sigma` | `resistant` | `variation` | `lenses` | `lateral_m` |
|---|---|---|---|---|---|---|
| laminated or thin-bedded shale | 0.05-0.2 m | 0.4-0.7 | 0-0.3 | 0.1-0.2 | 0-0.1 | 500 m and up: sheets |
| shallow-marine sandstone and shale (the default) | 0.3-2 m | 0.6-0.9 | 0.3-0.9 | 0.3 | 0.25 | 100-300 m |
| river deposits: channel sandstone in mudstone | 1-5 m | 0.7-1.0 | 0.1-0.4 | 0.45 | 0.5-1 | 30-150 m (Gibling's channel fills, about 10 to 1) |
| a massive cliff-former: dune sandstone, reef limestone | 5-30 m | 0.5 | 1 | 0.1 | 0 | any |
| a lava pile (reel r23) | 3-15 m | 0.5-0.8 | 0.85-0.95 | 0.2-0.3 | 0.1 | 1-3 km: a flow runs for kilometres |

`lateral_m` is one number for the whole formation, so a section that mixes a river member with a
lava pile takes one length for both: the cost of two shared lateral fields ("Not done", below). The
generator's rules are held at both ends of what a member may say (`formation_tests.cpp`, "the
thinnest member and the thickest"): 5 cm beds in half a metre and 100 m beds in a kilometre, each
at the widest spread, the most travel and every minor bed a lens, over four seeds.

## Numbers pinned

| Number | Value | From |
|---|---|---|
| beds a table | 512 (W85: 8) | 98,624 bytes a scene; nine steps of a binary search; a 264 m section is about 150 beds |
| members | 1-64, each 0.5-1,000 m | the Grand Canyon's units, 17-274 m |
| bed floor | 10 mm | McKee and Weir's bed |
| `sigma` | 0-1.5, default 0.7 | **a choice**: lognormal family from Talling and others, no parameter read |
| `bed_m` | 0.05-100 m, default 1 m | McKee and Weir's thick bed; a lava flow's ten metres |
| member resistance | `0.2 + 0.65 * resistant` | **a choice**, so the existing half falls at a share of 0.46 |
| `variation` | 0-0.45, default 0.3 | **a choice**; 0.45 is the most at which two tops cannot cross |
| `lenses` | 0-1, default 0.25 | **a choice**; which beds, from Zeito and Gibling |
| `lateral_m` | 10 m-100 km, default 150 m | Gibling's width-to-thickness for a metre bed |
| second field | 2.618 times the first, 35 and -50 degrees off strike, gain 2.5 | **choices**: incommensurate, oblique, a third of the ground at each end |
| fault throw | 0-1,000 m, one plane | displacement well under the trace length; a description, not a population |

## What the pictures show

Three held views of the escarpment scene's ridge as one banded rock layer (the pack's sandstone),
1920 × 1080, W85's authored fixture against a drawn section of eight members (163 m, seed 7, a 6 m
fault), 2026-11-11; the captures are outside the repository. Beside the reels (r12, r23, r25):

- **Gone:** the corrugated-sheet look. The authored four beds come round every 4.83 m, some thirty
  times up the 120 m face, and at 500 m they are a moiré of even lines. The drawn section's beds
  are unequal, a massive member stands out as one dark band, thin-bedded intervals read as fine
  lines between, and nothing comes round.
- **Still a layer cake at 100 m and beyond: every contact is a ruled line.** The variation along
  strike is there and is too small to see. A top's travel is bounded by the **thinner** of its two
  beds (so two tops cannot cross), and next to a 20 cm parting that is under a decimetre; a top
  between two 2 m beds moves 0.6 m over a hundred metres, a fifth of a degree. And the tops with
  the most contrast, the members', do not move at all. r23's flows thicken and thin by a large part
  of themselves round the bowl. **What would fix it without a new table or shader:** the member
  tops moved by metres on the longer lateral field, each member's beds stretched in proportion
  between them (a lens taking no share), and the per-top travel and the lenses kept on the shorter
  field. The table already carries two amplitudes a bed; only `pack_formation_table` changes.
- **The fault is a razor cut**: correct for a throw with no relief, and it reads as a seam in a
  texture. A fault wants a gully or a step in the ground, which is the landform's.
- **Colour and relief are not this row's and dominate the picture**: one tiled sandstone on a smooth
  ridge is a painted ramp, whatever is painted on it. Resistant members want to stand as ledges
  (R105, G142).

## Not done, and why

- **Thick-tailed thicknesses** (a power law's rare very thick bed): the deviate is bounded.
- **Three or more lithologies and a transition matrix**: two states alternate.
- **A bed that ends at a point of its own.** Tops share two lateral fields, because the filter's
  prefix sums need a field common to every bed below; so the lenses on one field all pinch out
  along the same ground. With two fields and signed amplitudes it reads as two families.
- **Faults as a population, joints, and offsets at them.**
- **The members' own tops do not move**, so a member is the same thickness everywhere.
