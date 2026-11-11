# The site a placement reads, and kinds by ecotope rule (G144, 2026-11-10)

The grounding for `scene_gen::Site` ([scene_gen](../subsystems/scene_gen.md), "The site") and for
the scatter kinds' responses ([scatter](../subsystems/scatter.md), "Kinds by site"). The want is the owner's
"vegetation obeys the ground": forest on one aspect and grass on the other, a treeline with a
stunted band below it, green where water reaches. Before this row trees stood to the summits and
the forest proof of concept composed masks and placement lists by hand, because nothing in the
engine derived a site from the ground. This page is what is known about which terrain variables
predict where plants stand, the alpine treeline, and how a species' response to a gradient is
modelled; each number the code pins is here with its source and its range.

## What the reels show

Four of the owner's reference reels carry the subject (catalogue text and contact sheets under
`D:\workspace\game_engine_local\inspiration\_frames\`):

- **r06** "aspect-controlled vegetation (forest on one side, grass on the other)": a dense conifer
  forest on the shaded slope of a dry mountain valley, open grass on the sunny one, the boundary
  following the ridge where the aspect turns. In dry country the sun-facing slope is the one that
  loses its trees, so a heat-load rule must be able to say "only where it is cool", not only
  "higher where it is warm".
- **r27** "forest only on shaded aspects; ridges gullied into regular spurs": the same, in patches
  that follow each spur's shaded flank.
- **r21** "the treeline breaks into patches following gullies": the trees reach highest in the
  hollows, which is a curvature response.
- **r02**, **r49** "abrupt bare rock spires above treeline": a forested slope ending against rock.

## Which terrain variables predict where plants stand

The variables a species distribution model reads from a terrain model, and the ones this row
builds (Austin 2007, "Species distribution models and ecological theory: a critical assessment and
some possible new approaches", Ecological Modelling 200: 1–19, separates *direct* gradients a
plant responds to — temperature, water, light — from the *indirect* terrain variables that stand
in for them, which is all a site query can offer):

| Variable | What it stands for | Built |
|---|---|---|
| Elevation | temperature, by the lapse rate (about 0.6 K per 100 m: Rita et al. 2023 use 0.006 K m⁻¹) | yes |
| Slope | soil depth, drainage, stability | yes, rise over run |
| Aspect, folded into a heat load | the radiation a slope receives and its afternoon warmth | yes: the downslope direction and a dot product |
| Curvature | convergence of water and soil (hollows) and exposure (crests) | yes, the negative Laplacian |
| Topographic position (a point's height against its neighbourhood's mean) | the same as curvature at a hillslope's scale | no: a row of its own (below) |
| Distance to water | moisture, flooding, the riparian gradient | yes, to a channel's bank and a body's outline |
| Wetness from drainage area (Beven and Kirkby 1979, "A physically based, variable contributing area model of basin hydrology", Hydrological Sciences Bulletin 24: 43–69: ln(a / tan β)) | soil water | **no**: it needs the upslope area, a field the landform solver (W95) will give; nothing local is honest about it |

**The scale.** The literature computes these from terrain models of 10 to 30 m cells, the scale of
a stand and a few crowns; at half a metre (scatter's own slope step) a slope is a boulder's face.
The site's derivatives are taken **5 m either side of the point** (`k_site_step_mm`), a 10 m
baseline: the finest of that range, so a gully 20 m wide is still seen as one. It is a design
choice, stated, not a fit.

### The heat load

McCune and Keon (2002, "Equations for potential annual direct incident radiation and heat load",
Journal of Vegetation Science 13: 603–606; two errors corrected in McCune 2007, JVS 18: 751–754)
fit the annual direct radiation a slope receives from its latitude, slope and aspect, and give a
*heat load* that **folds the aspect about the north-east to south-west line**, so the warmest slope
faces 225° (south-west, the afternoon sun on ground already warmed) and the coolest 45°: folded
aspect = 180° − |aspect − 225°|. For the southern hemisphere 315° replaces 225°. Their regression
needs `exp`, `cos` and `sin` of the point's slope and aspect, which a site query would have to
evaluate per point through the C library.

**What the site computes instead** is the cosine of incidence of a reference sun on the ground: the
ground's unit normal dotted with a direction at the folded bearing (225° north of the equator, 315°
south of it) and at the noon elevation of the equinox, 90° − |latitude|, divided by its value on
level ground so that **1 is level ground, more is sun-facing, less is shade-facing**, clamped at 0
(a slope steep enough to face away from that sun altogether). The sun's direction is computed once,
when the site is built, from the scene sky's latitude (`engine.scene.Sky.latitude_deg`; 30° where a
scene names no sky, the schema's default) with `core/math/deterministic.h`'s sine and cosine; per
point it is three products and two sums. It is not their regression, and it does not integrate the
year; it reproduces the ranking the index is used for (hottest on steep south-west slopes, coolest on
steep north-east ones, the spread widening with latitude) at the cost of a dot product. At 46.5° N a
30° slope facing 225° reads 1.39 and one facing 45° reads 0.34.

**Sunward tilt**, the second number from the same vector: the horizontal part of the unit normal
along the sun's bearing, sin(slope) × cos(aspect − bearing), from −1 to 1, 0.5 on a 30° slope that
faces it. It is what the treeline's aspect shift is linear in, so the shift is zero on level ground
and grows with the slope, as the radiation difference does.

## The alpine treeline

**Its definition and its temperature** (Körner's conventions, as summarised by Smith, Germino,
Hancock and Johnson 2003, "Another perspective on altitudinal limits of alpine timberlines", Tree
Physiology 23: 1101–1112): the **timberline** is the upper limit of closed forest, the **treeline**
the line joining the highest groups of trees at least 3 m tall, the **tree species line** the
highest individual trees, which are stunted (krummholz); the band from the first to the last is the
treeline ecotone, "tens to hundreds of metres" wide depending on how steep the gradient is. The
treeline follows an isotherm: Paulsen and Körner (2014, "A climate-based model to predict potential
treeline position around the globe", Alpine Botany 124: 1–12) fit 376 natural treelines with a
growing season of days whose mean exceeds 0.9 °C, at least 94 days long, averaging 6.4 °C.

**Its elevation by latitude** (Körner 1998, "A re-assessment of high elevation treeline positions
and their explanation", Oecologia 115: 445–459, and Körner's treeline pages, as compiled in
Wikipedia's "Tree line" table, read 2026-11-10): Andes of Bolivia 18° S 5,200 m; Khumbu 28° N
4,200 m; Rocky Mountain National Park 40° N 3,550 m on south-west slopes and 3,250 m on north-east;
Wyoming 43° N 3,000 m; the Swiss Alps 47° N 2,200 m; Abisko 68° N 650 m. A scene's ground has no
datum (its 0 is wherever its author put it), so the engine does not compute a treeline from the
latitude: a kind's upper elevation limit is authored in the scene's metres, and these are the numbers
an author sets it against.

**Its shift with aspect is regional, and can be zero or negative**, which is why it is a number of the
kind's and not a constant of the engine's:

| Where | Warm (equator-facing) minus cold slopes | Source |
|---|---|---|
| Swiss Alps | none found | Paulsen and Körner 2001, "GIS-analysis of tree-line elevation in the Swiss Alps suggests no exposure effect", Journal of Vegetation Science 12: 817–824: tree crowns are coupled to the free air, and a slope's sunshine warms its soil more than its trees |
| Southern Alps, New Zealand (*Nothofagus*, little disturbed) | +78 m | Rita et al. 2023, "Topoclimate effect on treeline elevation depends on the regional framework", Ecology and Evolution 13: e9733 (mean treeline 1,060 ± 173 m) |
| Rocky Mountain NP, Colorado | +300 m (south-west 3,550, north-east 3,250 m) | Körner 1998, by the table above |
| Apennines (*Fagus*, shaped by people) | −114 m | Rita et al. 2023 (mean 1,589 ± 178 m) |

**Curvature.** Case and Buckley (2015, "Local-scale topoclimate effects on treeline elevations: a
country-wide investigation of New Zealand's southern beech treelines", PeerJ 3: e1334) find treelines
lowest on concave slopes and rising abruptly from concave through flat to convex, most points 100 to
250 m below their area's highest; the owner's r21 shows the reverse, trees highest in the gullies,
where snow and water collect in a drier range. Both are a curvature response of a kind's density,
with either sign.

**Stunting through the band.** A tree at the timberline is a forest tree (20 to 30 m for a spruce);
at the treeline it is at least 3 m by definition; above it, krummholz, under 1 to 2 m. So the stature
falls by a factor of about ten across the ecotone, and the band is tens to hundreds of metres deep.
The kind's stunting is a smooth curve from its full scale at the band's foot to `krummholz_scale` at
the upper limit, and stays there through the soft edge above it, where the density thins to nothing:
the stunted, scattered trees of the tree species line.

## How a species' response to a gradient is modelled

The classical response of a species to one gradient is **unimodal**: a bell (the Gaussian response
model, an optimum and a tolerance: ter Braak and Looman 1986, "Weighted averaging, logistic
regression and the Gaussian response model", Vegetatio 65: 3–11), or one of the hierarchical
shapes of Huisman, Olff and Fresco (1993, "A hierarchical set of models for species response
analysis", Journal of Vegetation Science 4: 37–46), which add the **plateau** a species holds over
the middle of its range and the skew of an asymmetric limit. A kind's response here is the plateau:
1 over `[min, max]`, falling to 0 over `soft` beyond each edge on a smoothstep (3t² − 2t³, products
and sums, no `exp`), and the responses multiply. The plateau over the bell for three reasons: an
author states a range and a tolerance as numbers they can read off a source ("treeline 2,200 m,
ecotone 150 m"), the edge lands where it is set, and a kind inside every range places exactly as
it did before. A response is a probability of keeping a candidate, thinned by the candidate's own
hash, so it is the same on every tile and toolchain.

## The numbers pinned

| Number | Value | Source and range |
|---|---|---|
| `k_site_step_mm` | 5,000 mm either side (10 m baseline) | DEM scales of 10–30 m; design choice |
| Heat load's sun | bearing 225° (north) / 315° (south), elevation 90° − \|latitude\| | McCune and Keon 2002's fold; the equinox noon sun |
| Default latitude | 30° | `engine.scene.Sky.latitude_deg`'s default, what the sky draws |
| `k_site_water_reach_m` | 128 m | the riparian zone's few tens of metres (Naiman and Décamps 1997) with room; a response's limit past it is refused |
| `aspect_shift_m` default | 0 | Swiss Alps: none; NZ +39 per side (78 m between aspects); Colorado +150 (300 m); Apennines −57 |
| `krummholz_m` default, `krummholz_scale` default | 0, 1 (no band) | ecotone tens to hundreds of metres; a ratio of about 0.1 (3 m over 20–30 m) |
| The example's treeline | upper limit 520 m in the scene's metres, `soft` 60 m, `aspect_shift_m` 80, `krummholz_m` 140, `krummholz_scale` 0.15 | a valley whose 0 is its floor; the band and ratio from the rows above |

## What it costs

Measured in `msvc-release` on 2026-11-10 by `engine_scene_gen_bench` (`scene_gen.site.*`, 1,024
samples an iteration over a 2 km square) and `engine_scatter_bench` (`scatter.tile.*`), five
repeats each. **The machine was not quiet**: the harness waited 60 s and proceeded with other
processes using 32–39% of the CPU (this row's own `msvc-no-ecs` build and a Linux container among
them), GPU idle; the numbers are upper bounds.

| What | Median | Per item |
|---|---|---|
| `scene_gen.site.form`: elevation, slope, direction, heat load, sunward, curvature on a polynomial ground | 56.9 µs / 1,024 | **56 ns a sample**, five ground queries of about 6 ns each and the arithmetic |
| `scene_gen.site.water`: the distances, over 40 channels of 40 segments 5 m long packed 25–50 m apart and 8 lakes | 518 µs / 1,024 | **506 ns a sample**: a dense network files a few hundred segments in each 64 m cell |
| `scene_gen.site.all` | 588 µs / 1,024 | 574 ns a sample |
| `scatter.tile.unruled`: a 32 m tile of one kind at 3 m spacing, 0.06 a m², on a 31° side | 79.5 µs | the lattice, about 60 placements |
| `scatter.tile.ruled`: the same with elevation, aspect shift, krummholz, heat load and distance rules | 80.3 µs | **+0.8 µs a tile** (one channel in reach) |

**What scales.** A ruled kind samples only the candidates that survive its density, region,
exclusions and hard limits — about the placements it makes — so the rules add (placements a tile) ×
(five ground queries + the water's walk). On the synthetic grounds above a ground query is a few
nanoseconds; on a scene's own provider it is that provider's point cost (the dunes' few
microseconds, scatter.md, "A moving ground"), so on the dunes the same tile would add about 60 × 5 ×
3 µs ≈ 1 ms, and a kind with only an elevation response one query a placement. **Resident**: the
site object itself (about 200 bytes), 48 bytes a channel segment and 24 a channel cell — a 5 m
segment files in about 25 of the 64 m cells — so the forest proof of concept's 10.6 km of stream at
5 m would keep about 1.4 MB; nothing per tile, and nothing for a scene with no water. A water
sample's cost grows with how many segments share a cell; a network filed at a finer cell, or each
segment's box tested first, is the lever if a scene's water is denser than the bench's.

## What the pictures show

A valley written for the row (not committed: `content/test-scenes/alpine` holds G125's
specimens at metre scale and has no terrain): two ridges 600 m high along x, 1.4 km apart, on the
waves ground at 46.5° N, 14:30, with one scatter kind of `spruce-tall.flora.json` (4–6 m spacing,
0.7–1.1 scale) over 800 × 1,600 m; the ruled variant adds `elevation` `max` 480 m, `soft` 40 m,
`aspect_shift_m` 80, `krummholz_m` 100, `krummholz_scale` 0.15. Captured with `engine-view
--offscreen --time-of-day 14.5`, release build, 1600 × 900, under the GPU lock.

- **With the rule**, the forest stops on a line that follows the contour, higher on the
  south-facing (sunny) side than on the north-facing one, and the trees shrink through the band to
  scattered krummholz before the bare slope: the shape of r06's and r27's slopes and of Körner's
  ecotone. The unruled forest stands to the crests.
- **What still reads as painted** beside the reels: the bare ground above the treeline is the waves
  provider's sand, where r06 has alpine grass and r02/r49 rock; the treeline is one smooth contour,
  where r21's breaks into tongues up the gullies (a `curvature` response would give them, and the
  ridges here have no gullies to follow); every tree is one recipe at one form, where a real
  treeline changes species and the krummholz is a mat, not a shrunk spruce; the forest's edge at
  its region's end is a straight line; and the rough-ridge variant's 70 m spikes are not a
  mountainside at all. The unruled smooth valley could not be drawn: its 35,201 trees need 19.2
  million pairs, past the 16.8 million the visibility id names, which the ruled one's fewer trees
  fit under.

## What is not built

- **Wetness.** No wetness figure: the honest local inputs are the distance to water and the
  curvature, which are named for what they are. The topographic wetness index needs the upslope
  area, which W95's landform solver will give as a field; the site gains it then. The masks' wetness
  band that the sward reads (flora.md, "Growth forms by site") is a water body's `wet_m`, which the
  site's distance to the body already measures.
- **Topographic position** at a hillslope's scale (a point against the mean of a ring of 50–300 m):
  eight more ground queries a point; a row of its own if curvature at 10 m is not enough.
- **Shade from the placed canopy** and soil depth: G77's next stage and a later row.
