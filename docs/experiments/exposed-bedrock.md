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
- **A front is planar between rows** and a block is a prism. Weathered rounding and a fracture
  surface are the radial solver's ([rock structure](rock-structure.md)) and have no counterpart
  on a sheet yet.
- **Nothing is built behind a tread's back or under a front's foot** but 8 cm of tread and a foot
  that goes on a quarter of the step under the ground (5 cm at least, `bury_m` at most). The
  share of triangles above the ground is what that buys: 66 % against the 13.9 % G132 counted on
  the front of a closed box.
- **The far form** drops the blocks, the rows and every step under 0.8 m, on a 4 m lattice: 2.1
  triangles a metre of front against 36.

## Measured

A 64 m tile of the test hillside (`tests/ledges_tests.cpp`: 36.9°, noses and hollows 40 m apart,
five beds dipping 12°, 31 bed tops cropping out), one thread.

| | near form | far form |
|---|---|---|
| Triangles (vertices, clusters) | 37,492 (48,673; 1,793) | 1,533 (1,810; 69) |
| Front, m | 1,041 | 728 |
| Triangles a metre of front | 36 | 2.1 |
| Source mesh, bytes | 2,007,440 | 76,316 |
| Sampling the ground, ms | 0.35 to 0.46 (5,329 heights) | 0.03 to 0.04 (361) |
| Solving, ms | 51 to 57 | 1.6 to 1.9 |
| Building the container (the cluster DAG), ms | 208 to 286 | 7.2 to 8.0 |

The times are `msvc-release`, three runs of `ledges: what a tile costs`, on an i9-10980XE
**beside other agents' builds, 50 to 58 % of the processor busy when they were taken**: upper
bounds, not costs. The ground there is a function, so the sampling is the solver's share of it
and not a tile source's.

**When each is paid.** `tile`, the call a tile's activation makes, is a search of the entry's
tiles and one placement: microseconds, with the solver or without it. The solver's work is paid
earlier, because a streamed tile may name only a mesh the scene listed when it was read: the
sampling and the solve once a tile and form when the scene **opens** (about 55 ms and 2 ms a tile
that has ledges; less for one that has none, which stops at its first triangle without a
standing bed), and the container once a mesh when the scene reader **builds** it, a cache hit
after the first read. Both scale with the region's tiles, not the world's, and neither with the
frame. What is kept resident is the entry's list (a path and two indices a tile) and the built
meshes.

**Bytes a tile, on a scene.** The escarpment's east flank (224 m by 288 m of 32 m tiles, beds 1.5
to 6 m thick, `cliff_slope_deg` 50): 19 of 63 tiles have ledges; their near forms are 17,592
triangles together (2,038 the largest), 1,789,906 bytes of built containers on disk (200,290 the
largest, about 94 KB a tile that has any) and 233,038 bytes of recipes (12 KB each: the heights).

## The pictures, and what they lack

Captures are under the row's scratch directory, outside the repository (the hand-back names
them); nothing here is a golden. Two scenes: a scratch hillside 36 to 41° steep with noses and
hollows under a scatter of fescue, hair-grass and bracken, and a scratch copy of the escarpment.
A worktree has no material pack, so every rock is its type's flat colour.

Set beside the owner's reference reels (contact sheets r12, r23, r44 and r47: stepped basalt and
moss, bedded ledges at a fall, a grass slope under a cliff), a person would object to:

- **No rock surface.** Flat colour, planar faces, crisp prisms. The reels' rock is rounded at
  every edge, fluted, stained, lichened and wet, and its beds are told apart by colour.
- **The treads are bare.** In the reels a tread is turf or moss to its lip and the rock is the
  riser. Here nothing grows on a tread and the scatter's plants stand through its back.
- **Regular in height.** The formation repeats every few metres and every resistant bed crops
  out, so from across the valley the ledges are stripes at even spacing, broken along their
  length by the soil's patches and the fallen blocks but not across the slope.
- **Nothing lies below them.** No fallen block under the gap it left, no scree, no soil worn at
  a foot.
- **One scale.** The reels' steps run from a hand's breadth to tens of metres in one view; a
  ledge here is 0.15 to 4 m, and the faces above that are another generator's boxes.
