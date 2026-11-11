# Exposed bedrock in a slope (G142)

2026-11-10. The grounding for `domain/rocks`' exposure rule and the `ledges` generator
([rocks](../subsystems/rocks.md#exposed-bedrock-g142)): where bedrock crops out of a soil-mantled
slope, how layered rock makes steps, how a step depends on the beds' dip against the slope, and
how high a ledge stands against its bed. It follows [the formation field](formation-field.md),
whose `resistance` per bed nothing drew, and [G132's diagnosis](v29-cliff-diagnosis.md), which
asked for an exposed-band contract.

**How each number was read.** "Source" means the paper's own text or its publisher's figure page;
"abstract" its abstract; "secondary" a later paper or a reference work that reports it, named.
Where a primary text could not be reached the line says so. Nothing here is fitted to a picture.

## Where rock shows

**Soil is produced fastest where it is thinnest, and a convex nose carries the least.** Heimsath,
Dietrich, Nishiizumi and Finkel (1997, *Nature* 388, 358–361, doi:10.1038/41056) measured soil
production against depth on convex noses in Tennessee Valley, California: production falls
exponentially with depth, `(77 ± 9) e^(-(0.023 ± 0.003) h)` (source, the caption of their figure 3;
the units, m/Myr and h in centimetres, are from a secondary report of the same function and were
not on the caption page), and "hillslope curvature (a surrogate for soil production) varies
inversely with soil depth" (abstract). With transport proportional to slope, a steady soil has
`production = (rho_s / rho_r) K (-laplacian z)`; their K is 50 cm² a year and their density ratio
`rho_s / rho_r` 0.5 (source, the same caption). Setting the depth to zero gives the convexity at
which production can no longer keep a soil:

    -laplacian z = 77e-6 m/yr / (0.5 * 0.005 m²/yr) = 0.031 1/m

That is our arithmetic on their numbers, not a sentence of theirs. **`critical_curvature` defaults
to 0.03 1/m.** They surveyed at 1 to 3 m and took curvature from the eight nearest grid
neighbours (source, figure 2's caption).

**Above about 30 degrees soil becomes patchy, and past 45 degrees there is none.** DiBiase,
Heimsath and Whipple (2012, *Earth Surface Processes and Landforms* 37, 855–865,
doi:10.1002/esp.3205; read as chapter 3 of DiBiase's 2011 dissertation, which is the same paper)
mapped rock exposure in the San Gabriel Mountains from a 1 m lidar surface, the local slope "the
dip of a 3x3 m plane fitted at each point": "below catchment-mean slopes of 30°, ... bedrock
exposure is limited; above mean slopes of 30°, rock exposure index increases strongly with mean
slope", and "we have not found soil-covered slopes or scree slopes in the SGM with slopes steeper
than 45°". Their index is the share of cells steeper than 45°, which matched mapped bedrock with
R² 0.99. Their steepest catchments, at 42 to 43° of mean slope, are still only 25 to 29 % rock:
45° is a *local* slope, not a catchment's. Their outcrops "range in scale from 0.1-100 m" and
"vertical cliff faces greater than 10 m are rare". **`soil_slope_deg` defaults to 30 and
`rock_slope_deg` to 45**, both local slopes.

**The scale.** The rule's slope and convexity are central differences an arm of `scale_m` (4 m)
either side of a point. Roering, Kirchner and Dietrich (1999, *Water Resources Research* 35,
853–870, doi:10.1029/1998WR900090; source) gridded their hillslopes at 4 m and DiBiase fitted
3 m planes; Heimsath's curvature is from a grid of a few metres. A finer arm reads the ground's
own roughness as noses, a coarser one loses the nose.

**What the rule does with them.** `exposure_fraction` is 0 under the soil slope whatever the
curvature; on planar ground it rises linearly to 1 at the rock slope; a convexity adds
`convexity / critical_curvature` (clamped to ±1), so a nose bares its rock early and a hollow
keeps its fill; and `2 steep - 1` is a floor under the sum so that the rock slope ends every
mantle, hollow or not. **The thresholds are the sources'; the sum is ours.** The literature has a
law for soil depth against curvature on gentle convex ground and a count of rock against slope on
steep ground, and no single law across both. Roering's flux law
`q = K grad z / (1 - (|grad z| / S_c)²)`, with `S_c` 1.25 (51°) in the Oregon Coast Range, joins
them in principle, but its erosion rate at a point depends on the distance to the divide, which a
function of a point and its neighbours cannot know. The linear rise between the two slopes is
the simplest function with DiBiase's two ends.

**Rock is patchy at every slope short of the last.** DiBiase's table of catchments gives the
share of ground that is rock against the mean slope: 0.1 % at 16.2°, 1.7 % at 28.1°, 2.5 % at
31.8°, 8.2 % at 35.7°, 16.7 % at 38.0°, 21.3 % at 39.8°, 26.1 % at 42.0° and 28.6 % at 42.7°
(source, table 3.2 of the dissertation chapter), rising "approximately linearly" past 30°. A rule
that exposed every resistant bed along its whole trace wherever the slope passed 30° would draw
far more rock than that, as stripes. So the fraction is read as a share of ground: rock shows
where it is above a seeded cover field (`soil_cover`, patches `patch_m` across, 12 m by default),
and a bed stands from nothing to its whole exposed height over a quarter of the fraction's range
past the cover. **The patch size is ours** (DiBiase's outcrops are "0.1-100 m"), and so is the
field: two octaves of value noise, stretched so the share of points that show rock rises about
evenly with the fraction (measured on planar ground: 18 % of points at 32°, 47 % at 36°, 77 % at
40°, all from 44°). That share is of points where a resistant bed *would* show, not of area that
*is* rock (a bed's trace is a fraction of the slope), so it is not DiBiase's index and is not
fitted to it; the two agree only in rising from the same slope to the same slope.

## How layered rock makes steps

**Resistant beds stand as risers, weak ones weather back into benches.** The form is old enough
to have named a rock: "trap", from the Swedish for a stair, because "superposed sheets of basalt
weather with well-marked step-like or terraced features" (*Encyclopaedia Britannica* 1911,
"Basalt"). In the Colorado Plateau "differences in the composition, hardness, arrangement, and
thickness of the rock layers determine their ability" to form cliffs, ledges or slopes, and a cliff
former stands only while its cap does: "even a few feet of the Kayenta protect the rock beneath"
(Lohman, USGS Bulletins 1327 and 1393; source). Selby (1980, *Zeitschrift für Geomorphologie* 24,
31–51; secondary, Moore, Sanders, Dietrich and Glaser 2009, *ESPL* 34, 1339–1352) "showed that
variations in rock mass strength along a vertical cliff profile correlated with changing
lithology", with "steeper slopes on stronger rocks". We did not reach a primary text that states
a riser's height as a function of its bed's thickness; the rule below is geometry.

**A ledge's height is its bed's thickness, measured up.** A resistant unit of thickness `T`
across the beds whose normal leans `dip` from vertical is `T / cos(dip)` thick along a plumb
line. Where the ground has cut down through it, its top plane stands that far above its base's
trace. The solver stands the unit to `stand * T / cos(dip)`, `stand` the exposure fraction times
the resistance's factor, never more than `max_step_m` (4 m: DiBiase's "cliff faces greater than
10 m are rare" is about faces, and a unit thicker than the cap is a cliff's business).

**The step depends on the dip against the slope's aspect.** The top of a ledge is the unit's
bedding plane. Where the beds dip into the hill or lie level (the **scarp slope**: "the outcrop of
the caprock forms a steeper or even cliff-like frontslope", after Cotton 1952 and Simonett 1968
as the standard definition of a cuesta reports them; secondary), that plane leaves the ground
quickly, the tread is short and the riser crisp: a tread's width is
`h / (tan(slope) - tan(apparent dip))` for a riser of height `h`. Where they dip with the hill
(the **dip slope**, "a long and gentle backslope" that follows the resistant stratum) the
denominator falls, the tread widens into a slab, and where the beds dip as steeply as the ground
there is no riser at all: the bed *is* the ground. The solver does not choose between the two
cases; both fall out of intersecting the bedding plane with the ground. Selby's finding on joint
orientation, that joints dipping out of a slope "can more easily accommodate sliding and are
unfavorable for cliff stability" (secondary, Moore et al. 2009), is the reason a dip slope's
risers are low; the solver gets low risers there from the geometry alone and does not model the
sliding.

**Joints cut a bed into blocks about as long as the bed is thick.** Narr and Suppe (1991,
*Journal of Structural Geology* 13, 1037–1048, doi:10.1016/0191-8141(91)90055-N; abstract): the
Monterey Formation's rocks "show a nearly constant ratio of layer thickness to joint spacing of
about 1.3". Bai and Pollard (2000, *J. Struct. Geol.* 22, 43–57; abstract): "the critical
fracture spacing to layer thickness ratio varies between 0.8 and 1.2". **`joint_ratio` defaults
to 1.3** (thickness over spacing), so a 2 m unit's blocks are about 1.5 m long; Ladeira and Price
(1981) is the usual citation for the ratio failing in beds thicker than about half a metre
(secondary, Ji 2023), which is why it is a parameter and why the spacing is clamped to 0.5 to
8 m.

## Outcrops, not stripes

Read for the second pass of roadmap G142 (2026-11-10), after the first solver's ledges read from
across a valley as every bed's stripe. Marked as the first pass's numbers are: read in the
source's text, in its abstract only, or secondary.

**Resistant beds are continuous in the rock and discontinuous at the surface.** Marshall and
Roering (2014, *JGR Earth Surface* 119, 1395–1417, doi:10.1002/2013JF003004; read in the
dissertation chapter that reproduces the paper) map the Tyee Formation's resistant sandstone beds
in two Oregon Coast Range watersheds: they "form cliff-like, vertical faces" (mapped as a
gradient of 1 or more), are "a meter to tens of meters" thick (measured 1 to 15 m), and are
"horizontally continuous but not contiguous". They make 12 % of one watershed's topography and
under 4 % of the other's; in the second "very few continuous outcrops" exceed 100 m, and within a
350 m radius resistant beds average about 5 % and never pass 12 % (0 to 30 % in the first).
**`stretch_m` defaults to 32 m** so an outcrop is a crag well under that 100 m (60 m, tried
first, drew lines a hundred metres long), and
**`stretch_share` 0.4 with `member_share` 0.5** leaves a fifth of a resistant bed's trace able to
crop out before the slope's own exposure and the soil's patches are applied, inside the 0 to 30 %
the paper measured over ground. The two shares themselves are ours, not the paper's.

**Cliff formers stand between slope formers.** The same paper: gentle soil-mantled valleys perch
above the cliff-forming beds, which make knickpoints; fracture spacing is 12.9 ± 1.7 m in the
resistant beds against 0.6 m in the typical ones, and tensile strength 2.5 times as high. The
Grand Canyon's cliff-and-slope profile is the textbook case (US National Park Service, "Geologic
Formations": the Coconino Sandstone and the Redwall Limestone form cliffs, the Bright Angel and
Hermit shales form slopes; secondary, and its thicknesses were seen only in snippets that
disagree). The formation's `macro_beds` are that grouping where a scene gives them. Where it
gives none, **the rule's own members, `member_m` 12 m across, are authored**: a seeded field
along the bed coordinate, with no source for its size but the 1 to 15 m the resistant beds
measured. Duszyński, Migoń and Strzelecki (2019, *Earth-Science Reviews* 196, 102890) review
escarpment retreat; only its abstract was read and no number is taken from it.

**Rock on a steep soil-mantled slope is a patchwork.** Milodowski, Mudd and Mitchard (2015,
*Earth Surface Dynamics* 3, 483–499, doi:10.5194/esurf-3-483-2015; read): on granitoid slopes
mapped from 1 m lidar, 1.5 and 3 % of hillslope pixels are rough enough to be rock above the
knickpoints (modal gradients about 0.5 and 0.4) against 15 and 29 % below them (about 0.9 and
0.8); the transition is "gradual and patchy", the steep slopes a "patchwork of bedrock outcrops
and discontinuous soil cover". It reports no patch-size statistics. Heimsath, DiBiase and Whipple
(2012, *Nature Geoscience* 5, 210–214; abstract): soils thin and become patchier with erosion
rate, with no abrupt switch to bedrock.

**Not found:** the exposed share of a single bed's trace, the spacing between outcrops along
strike, a correlation length of soil depth on steep slopes, or a source that puts outcrops on
noses and gully heads by a number (the curvature term of the rule above is what stands for it).

## How a jointed block weathers

Read for the same pass, for the ledges' blocks.

**Corners go before edges, edges before faces.** Domokos, Jerolmack, Sipos and Török (2014,
"How river rocks round", *PLoS ONE* 9(2), e88657; read in the preprint) model abrasion as retreat
at a speed proportional to curvature, in three kinds of event: at a vertex (Gaussian curvature),
along an edge (mean curvature), and on a face (uniform). In their first phase edges and corners
round while the axes keep their length; a cube rounded to its inscribed sphere keeps π/6 of its
volume. That is collision, not chemical weathering. For weathering the order is the usual
statement of spheroidal weathering (alteration "greatest along the corners of each block, followed
by the edges, and finally the faces": secondary, an encyclopaedia paragraph citing Ollier 1971,
*Earth-Science Reviews* 7, 127–141, which was not reached). **No ratio was confirmed.** The
solver's 3 : 2 : 1 for a corner, an edge and a face is the count of faces the attack comes
through, which is geometry and ours.

**No edge radius by lithology was found**, nor a sourced ranking; the solver reuses the rock
types' own `rounding` (rocks.md, "The model by type"), which is authored. Limestone's solution
flutes are 1.2 to 2.5 cm wide and 2 to 6 mm deep (secondary, a teaching resource; Mottershead and
Lucas 2000, *Zeitschrift für Geomorphologie* 44, 491–512, abstract: lithology changes flute form
significantly): under a 1 m lattice's resolution, so they are the material's and not the mesh's.

**Weak beds weather back under hard ones.** No measured recess depth on a natural cliff was
found. On fresh roadcuts shale under sandstone or limestone is undercut 2.5 to 51 mm a year,
median 20 (Niemann 2007, GSA Annual Meeting abstract; fresh cuts, so upper bounds). Rock varnish
is typically about 100 µm thick and takes about 10,000 years to coat a surface in the US
Southwest (secondary, attributed to Liu and Broecker 2000 and to Dorn): a fresh break is the
rock's own colour and an old surface is darker, which is the material's to carry.

## A block as a body

Read for G142c, the third pass on the ledges, by a research sub-agent; how each number was read is
said beside it. None of the sources below was reached in full except where it says so.

**Joints open into gulls where a cap rock creeps over a weaker bed** (cambering). Cooper's
account of the Bath University cutting in *Mass Movements in Great Britain* (Geological
Conservation Review vol. 33, ch. 6, citing Hawkins 1977; **full text read**): gulls **up to 0.4 m
wide in a massive bed 1.9 m thick**; in the thinly bedded 2 m above it slip is spread bed by bed
and "no fracture penetrates through to the surface"; block dips of 26 and 30° there and up to 37°
on the Great Oolite, throws of 0.82 and 1.7 m, and "each individual block has rotated downslope, as
well as being lowered". Barron et al. 2016 (*Geomorphology* 268, the Cotswold escarpment; **full
text read**): open joints up to about 1.5 m in a cap 25 m or more thick, a foundered block rotated
at least 30°. A BGS page: widths "from millimetres to tens of metres". Hollingworth, Taylor and
Kellaway 1944 (*QJGS* 100, 1–44): up to 40 yards and dips to 40° (a search snippet; secondary). **No
relation of aperture to distance from the edge was found** (Hutchinson 1991 was not reached).

So the solver's cleft is at most a fifth of the unit's thickness (0.4 / 1.9 = 0.21) and 0.4 m,
never more than a fifth of the joints' spacing (ours: a ledge's blocks are a metre or two, not a
cap's tens), and narrow for most joints (the cube of a draw: ours). The least width, half the
rock's edge wear, is ours: a worn joint is never shut. A crept block moves out by up to a fifth of
the thickness (ours, the gull's ratio) and **turns 1.1 to 4.6 degrees** (ours): the cambered dips
above are of caps tens of metres thick, and a ledge's block turned that far would have toppled off
its tread (the block criterion, width over height under the tangent of the base's dip: Goodman and
Bray 1976, read only as restated by others).

**Undercuts.** Niemann 2007 (GSA Abstracts with Programs 39(6), 439; **abstract read**): shale
undercutting resistant sandstone and limestone ledges on eight Appalachian roadcuts at 2.5 to 51
mm a year, median about 20 (0.1 to 2.0 inches); Shakoor and Rogers 1992, before it, 0.5 to 3.75
inches. Those are fresh cuts and rates, not depths. **No natural recess depth was found.** The
solver's undercut (a third of the step high, 0.42 to 0.6 of it deep, 0.6 m at most, times the
contrast in resistance over 0.6) is authored.

**Rounding.** Only the order (corners, edges, faces) is supported, as before. No edge radius by
lithology or block size was found.

**Joint spacing.** Bai and Pollard 2000 (*J. Struct. Geol.* 22, 43–57; abstract, on a mirror):
spacing over thickness at saturation about 1.0, 0.8 to 1.2. Narr and Suppe 1991 (*J. Struct.
Geol.* 13, 1037–1048; abstract, on a mirror): thickness over spacing about 1.3 (the default
`joint_ratio`), spacing over its median log-normal; no spread was found, so the solver's stations
still move by up to three tenths of the spacing (ours).

## What is authored, not measured

- **A bed's `resistance`** is an index from 0 to 1 the formation's author gives it, not a rock
  mass strength. `min_resistance` (0.5) and the factor from 0.35 at the threshold to 1 at
  resistance 1 are authoring choices.
- **A joint block's state**: the share of its step it keeps (55 to 100 %, changing by up to a
  third along it), one block in five fallen out, four in ten having lost one or two rows off the
  top, joint planes up to 20° off the dip direction, stations moved by up to three tenths of the
  spacing. Seeded variation with no source; the picture they aim at is a ledge that is broken
  rather than extruded.
- **Rows.** A bed parts into rows about 0.45 m thick (`k_parting_m`), unequal by a seeded weight,
  each standing back under the row above by up to `relief_m`. Parting along a bed's own laminae is
  real (McKee and Weir 1953, the formation page's source for bed thickness, is the terminology; it
  was not re-read for this row) but the number is ours.
- **The sum of the slope and curvature terms**, and **the soil's patches** (above).

## What was traded for triangles

- **The lattice is the ground's, 1 m.** A front's line is piecewise straight at that spacing; a
  finer lattice would follow the plane's trace more closely at four times the triangles.
- **A worn edge is one face, a worn corner one cut, a flute two scales of noise at the rows'
  own vertices.** A round is a chamfer shaded as one with the face under it; a curve would be
  two or three faces more an edge. 43 triangles a metre of front against a budget of 48; then
  (G142c) 54.7 against 56, the clefts' flanks and rims and the undercut row being the eight.
- **Nothing is built behind a tread's back or under a front's foot** but 8 cm of tread and a foot
  that goes on 0.15 of the step under the ground (5 cm at least, `bury_m` at most), and a face
  the ground covers whole is not drawn. The share of triangles above the ground is what that
  buys: 68 % against the 13.9 % G132 counted on the front of a closed box.
- **The far form** drops the blocks, the rows, the wear and every step under 0.8 m, on a 4 m
  lattice: 2.0 triangles a metre of front against 43.

## Measured

A 64 m tile of the test hillside (`tests/ledges_tests.cpp`: 36.9°, noses and hollows 40 m apart,
five beds dipping 12°; 13 of its 31 bed tops crop out somewhere in the tile), one thread.

| | near form | far form |
|---|---|---|
| Triangles (vertices, clusters) | 5,255 (5,810; 245) | 121 (148; 6) |
| Front, m | 123 | 59 |
| Triangles a metre of front | 43 | 2.0 |
| Source mesh, bytes | 248,980 | 6,188 |
| Sampling the ground, ms | 0.32 to 0.52 (5,329 heights) | 0.02 to 0.03 (361) |
| Solving, ms | 11.1 to 12.1 | 0.37 to 0.45 |
| Building the container (a cluster DAG a material, merged), ms | 33.0 to 35.0 | 0.90 to 1.00 |

The times are `msvc-release`, three runs of `ledges: what a tile costs`, on an i9-10980XE with
**18 to 35 % of the processor busy** (other agents' builds and a container build) when they were
taken: upper bounds, not costs. With every bed cropping out and no wear (the first pass) the same
tile was 37,492 triangles, 51 to 57 ms to solve and 208 to 286 ms to build, beside a busier
machine. The ground there is a function, so the sampling is the solver's share of it and not a
tile source's.

**When each is paid.** `tile`, the call a tile's activation makes, is a search of the entry's
tiles and one placement: microseconds, with the solver or without it. The solver's work is paid
earlier, because a streamed tile may name only a mesh the scene listed when it was read: the
sampling and the solve once a tile and form when the scene **opens**, and the container once a
mesh when the scene reader **builds** it, a cache hit after the first read. Both scale with the
region's tiles, not the world's, and neither with the frame. **For a region a kilometre square
of 64 m tiles (256 tiles) of ground like the test hillside's**: `open` solves 512 patches, about
256 × (0.4 + 11.6 + 0.02 + 0.4) ms = 3.2 s on one thread and less where a tile has no rock (a
patch with nothing standing stops at its triangles' first test), and writes a recipe of about
12 KB (the heights) for each form that has ledges; the reader then builds each listed mesh once,
about 34 ms a near form, 9 s for all of them, into the derived-data cache. What stays resident
is the entry's list (a path and two indices a tile: under 100 KB for the region) and the built
meshes of the tiles in the rings that draw them, about 250 KB of source mesh a near tile. That
`open` solves a region nobody is near is the placement contract's (a mesh must be listed at
scene read); the solver itself is a function of the tile.

**The cliff faces in tiers** (`tests/cliffs_generator_tests.cpp`, a 256 m block at a budget of
20,000 a line): 2 faces and 12,204 triangles on the 30 m fin and the planar slope, 10 faces and
17,124 triangles on a quadrant of the escarpment's ridge; the escarpment scene itself is 38
faces. Tracing the scene's four blocks and solving its 38 recipes once each (the lowest tier of
a line twice, for its ground patch) takes 1.0 s in `msvc-release` and 6.3 s in `msvc-debug`.

## The pictures, and what they lack

Captures are under the row's scratch directories, outside the repository (the hand-backs name
them); nothing here is a golden. Two scenes: a scratch hillside 36 to 41° steep with noses and
hollows under a scatter of fescue, hair-grass and bracken, and a scratch copy of the escarpment
with the formation, a `ledges` layer above its faces and sand-coloured treads. The second pass's
captures (engine-view offscreen, 1280 × 720, `--shadows csm`, `--sun 120,35`, msvc-debug, RTX
5090, 2026-11-10) had the engine's material pack, whose one rock set, `rock/sandstone`, is a
saturated yellow; the scenes tint it (0.5, 0.46, 0.9) toward ochre, and it is still the first
thing a person sees.

**What changed in them.** From across the valley the hillside's rock is now five or six crags,
each a stack of three or four short ledges, with turf between; before, every resistant bed drew a
line across the hill. At a walker's height a ledge is a row of separate lumps with worn top edges
and cut-back ends, soil on their treads, textured; before, crisp prisms in one flat colour. The
escarpment's flank is rock from its foot to the round of its crest in one bedded mass, where the
seated single box stood on its lower two fifths; from on the crest behind the east faces the
top tier's top is a band at the ground's own edge and nothing hollow shows.

Set beside the owner's reference reels (contact sheets r12 and r23: stepped basalt under moss,
a bedded gorge at a fall), a person would still object to:

- **The rock is a colour and a tiled set.** No staining, no lichen, no wet, no moss on a riser,
  no darker old surface beside a paler break; the reels' rock is all of those before it is a
  shape. The pack's yellow makes it worse.
- **Lumps, not rock.** A block is a prism with one worn edge and two cut corners. The reels'
  blocks are rounded on every side, undercut, split, and never two alike; their joints are
  clefts with plants in them.
- **A crag is still stripes inside.** Its ledges are level rows of about one height, a metre or
  so, because the beds are; the reels' steps run from a hand's breadth to tens of metres in one
  view, and the tiered faces that carry the large end are another generator's and another look.
- **Treads are a flat colour.** They sit beside the turf but carry no blades, and the scatter's
  plants stand through a tread's back; in the reels a tread is thick moss rounded over its lip.
- **Nothing lies below.** No block under the gap it fell from (the gaps are handed over, nothing
  takes them yet), no scree, no worn soil at a foot. The cliff's own talus is still smooth cones.
- **The tiers do not read.** A face's tiers meet without a bench or a change of rock, so the
  flank is one wall with level bedding; the reels' cliffs step back at each member, with turf on
  each bench.
- **No picture of the far form.** A streamed run of the hillside drew the near form at every
  marker; whether the far form holds at its ring was not seen.
