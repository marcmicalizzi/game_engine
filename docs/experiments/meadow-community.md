# The meadow community: species by site (G140, with G139)

**Row** G140 (and G139), 2026-11-10. **Lands on** [flora](../subsystems/flora.md), "The meadow
community" and "Where a site field draws the boundary". **Code** `domain/flora/include/domain/flora/meadow.h`,
`domain/flora/src/sward_placement.cpp`.

The forest proof of concept's solver needs (§2.3 items 1, 2, 6 and 7; §3.2): "A meadow community, not
one forb. Six to ten forb species by site, plus sedges, rushes, mosses and dwarf shrubs, with height,
density and flowering by wetness, soil, slope, light and grazing, from published plot cover data; reel
r01 is the model." Until this row the sward placed one forb mesh (an oxeye daisy) in a third of its
cells whatever the site.

Each number below is marked: **read** (in the source's own text), **abstract** (from its abstract or a
restatement, not the paper), **secondary** (a page that cites the source), or **chosen** (a design
value, with why).

## The model: a community as species along niche axes

- **The niche axes are Ellenberg's indicator values.** Ellenberg's moisture (F, 1-12) and light (L,
  1-9) values place a species on a gradient by where it is found; Hill, Mountford, Roy and Bunce (1999,
  *Ellenberg's indicator values for British plants*, ECOFACT vol. 2, Technical Annex, ITE/HMSO)
  recalibrated them against British vegetation data. The engine's site becomes an F and an L
  (below), and each species' published F and L are its optima, so the table is the publication's and
  not a tuning. The scale's anchors as Hill et al. word them (**read**): F5 "moist-site indicator,
  mainly on fresh soils of average dampness"; F7 "dampness indicator, mainly on constantly moist or
  damp, but not on wet soils"; F9 "wet-site indicator, often on water-saturated, badly aerated soils";
  L7 "generally in well lit places, but also occurring in partial shade"; L8 "light-loving plant rarely
  found where relative illumination in summer is less than 40 %"; L5 semi-shade.
- **A species answers its axes with a Gaussian.** ter Braak and Looman (1986, *Vegetatio* 65, 3-11)
  fit a bell-shaped response of an optimum u and a tolerance t to occurrence along a gradient
  (**abstract**); the cover is written `c · exp(-(x - u)^2 / 2 t^2)` on each axis, the product over
  the two, which is the standard restatement (**secondary**). Past three tolerances together the
  species is cut to absent (**chosen**: the Gaussian holds 99.7 % of its mass within 3 t, and a cover
  quantized to 1/65536 otherwise leaves a sedge's 1/65536 on a dry slope's every cell).
- **Tolerances** (**chosen**): 1.0 F unit and 1.5 L units for every species. Hill et al.'s values are
  integers, each placing a species in one class; a tolerance of one class is the narrowest the scale
  can say, and light is the weaker axis in an open meadow (most of its species are L7 and L8). A
  species-specific tolerance would come from fitting plot data (ter Braak and Looman's method); this
  row read no such fit.
- **The maximum c is a cover-abundance class's midpoint** (**chosen**, from the classes **read** in the
  JNCC *National Vegetation Classification: Users' Handbook*, Rodwell 2006, §5.3): Domin 4 is 4-10 %
  (taken as 0.07), Domin 5 11-25 % (0.18), Domin 6 26-33 % (0.30). The class a species gets is chosen
  from its constancy in the communities below. The handbook's warning (**read**): "the total of all
  the Domin values for the species can exceed 100 % cover" — which is why the rule scales a cell's
  covers down when they sum past one, rather than refusing.

## The communities the table is drawn from

- **MG3, *Anthoxanthum odoratum - Geranium sylvaticum* grassland**, the northern hay meadow (Rodwell
  1992, *British Plant Communities* vol. 3, pp. 49-55): its constants include *Conopodium majus*,
  *Geranium sylvaticum*, *Ranunculus acris* and *Sanguisorba officinalis* among the grasses; "dense
  growth of grasses and herbaceous dicotyledons up to 60-80 cm high", *Geranium sylvaticum* "usually
  the most prominent and exceptionally may account for 90 % of the cover" (**secondary**: the
  publisher's chapter page quoting the book). It is the mesic sward and the shade edge here.
- **MG8, *Cynosurus cristatus - Caltha palustris* grassland**, the wet margin's flood meadow
  (Floodplain Meadows Partnership's constancy table for MG8, **read** but its text extraction pairs
  names and numbers about a row apart, so these are approximate): *Ranunculus acris* about 73 %,
  *Filipendula ulmaria* about 66 %, *Cardamine pratensis* about 57 %, *Caltha palustris* 39-48 %,
  *Trifolium pratense* 35-48 %, *Carex nigra* about 21 %, *Juncus effusus* 10-17 %.
- **Constancy classes** (**read**, Rodwell 2006, §7.3): I 1-20 %, II 21-40 %, III 41-60 %, IV 61-80 %,
  V 81-100 %; IV and V are "constant".
- **Not read**: M23 (the rush pasture), the Habitats Directive's 6520 mountain hay meadow species list,
  and any per-species Domin range for MG3. The rush's and the sedge's covers are chosen from MG8's
  constancy and a rush pasture's physiognomy.

## The table

F and L: Hill et al. (1999), the final British values, **read** from the published table (its "#" marks a
value the authors changed by judgement, "D" one taken direct from Ellenberg). Cover: **chosen** class
midpoints. Height, heads and colour: **chosen** (not found in a source read tonight; typical field-guide
values), and they drive the stand-in herb only, never a cover.

| Species | Role | F | L | Cover (Domin) | Where it stands | Height m | Flower (stand-in) |
|---|---|---:|---:|---|---|---:|---|
| *Caltha palustris* | forb | 9 | 7 | 0.18 (5) | the wet margin | 0.30 | yellow, 3 heads |
| *Filipendula ulmaria* | forb | 8 | 7 | 0.18 (5) | the wet margin | 1.0 | cream panicle |
| *Cardamine pratensis* | forb | 8 | 7 (L uncertain in the table's text) | 0.07 (4) | the wet margin | 0.35 | lilac |
| *Carex nigra* | sedge | 8 | 7 | 0.18 (5) | the wet margin | 0.40 | none |
| *Juncus effusus* | rush | 7 | 7 | 0.30 (6) | damp ground | 0.80 | none |
| *Ranunculus acris* | forb | 6 | 7 | 0.18 (5) | the mesic sward | 0.60 | yellow |
| *Vaccinium myrtillus* | dwarf shrub | 6 | 6 | 0.18 (5) | the shade edge | 0.30 | none |
| *Geranium sylvaticum* | forb | 5 | 6 | 0.30 (6) | mesic sward, shade edge | 0.60 | violet |
| *Trifolium pratense* | forb | 5 | 7 | 0.18 (5) | the mesic sward | 0.35 | magenta heads |
| *Conopodium majus* | forb | 5 | 6 (#) | 0.07 (4) | mesic sward, shade edge | 0.40 | white umbel |
| *Leucanthemum vulgare* | forb | 4 | 8 | 0.07 (4) | the dry slope | 0.50 | white |
| *Lotus corniculatus* | forb | 4 | 7 | 0.07 (4) | the dry slope | 0.15 | yellow |
| *Thymus polytrichus* | forb | 4 | 8 | 0.07 (4) | the dry slope | 0.10 | mauve |

Ten forbs, a sedge, a rush and a dwarf shrub. Hill et al. give *Thymus polytrichus* as their own
example of L8 and F4 and *Cardamine pratensis* of F8 (**read**), which checks the table's alignment
where the PDF's text layout drifts. Mosses are not in the table: a moss mat is the floor's (G141's
`SwardFloorMix`), not a plant that stands.

## The site as F and L

The site (`scene_gen::SiteSample`, G144) gives the slope, the heat load, the curvature and the
distances to a channel's bank and a water body's outline. The rule (`MeadowRule`, defaults
**chosen**, each with its reason):

- **The ground's own F**: `moisture_base` (5, Hill's "fresh soils of average dampness": level open
  ground of a hay meadow) minus `heat_dry` (2) per unit of heat load over level ground's (McCune and
  Keon 2002's load, as the site computes it: a 30° slope facing the sun at 50° N is about 1.46, so
  about one F class drier, and facing away about 0.27, one and a half wetter), minus `slope_dry` (2)
  per unit of rise over run (a 30° slope another class drier: steep ground drains), plus
  `hollow_wet` (20) per 1/m of concavity (a hollow of Laplacian 0.05/m one class wetter, a crest one
  drier). Held to 1..9. A topographic wetness index (Beven and Kirkby 1979, ln of the contributing area
  over the slope's tangent) is the published form of this; the site has no contributing area, so the
  rule stands on its two local terms, slope and curvature, and the heat load's aspect.
- **The water's reach**: within `wet_reach_m` (10 m) of a channel's bank or a body's outline F rises
  on a smoothstep to the wet margin's 9 at the edge; the paint's cooked wet band, where a scene has one
  and no water entry, does the same. 10 m is chosen: a stream meadow's wet margin is metres to tens of
  metres wide in the reels (r04's banks, r01's river meadow), and riparian vegetation changes over a
  few metres across a bank (Naiman and Decamps 1997, the sward's riparian factor's source).
- **L**: `light_open` (7.5, between the meadow's L7 and L8 species) falling to `light_shade` (5,
  Ellenberg's semi-shade) with the paint's shade layer's weight.

## The stature rule reads the same F (G137, G140)

The sward's form already took a wetness 0..1 (G137); it is now `(F - moisture_base) / (9 -
moisture_base)`, so the tall tussock stands where the rushes and the marsh marigold do, and the slope
it reads is the site's (a 10 m baseline) rather than half a cell's. A scene with only the paint's wet
band places the forms it did: the band lifts F to 9 and the wetness to 1.

## Where a site field draws the boundary (G139)

G138's band (`band_m`, 3 m) turns metres into score by the mosaic noise's gradient alone. Where the
wetness or the slope moves the stature score across zero, the score's true gradient is the noise's plus
the field's, and the band narrowed to the field's ratio: a slope threshold made a line. The band now
reads the whole gradient: the noise's (analytic, as G138) plus the fields' (the fields' part of the
score taken again one cell along x and one along z, forward differences), summed as vectors.

*Measured* (`sward forms: where the slope draws the boundary the band keeps its width (G139)`): a valley
floor `z = a x^2` whose slope passes 22.5° at the tile's middle, the noise's weight 0.02 so the slope
draws the boundary, six worlds: the share of tall patches across 1 m columns, its width read as a
smoothstep ramp's (the sum over columns of `s (1 - s)` is 9/70 of a ramp's length, and its 5-95 %
width 0.7293 of that): **2.95 m against a band of 3 m**; a band of 0 m gives 0 m (a line). G138's rule
on the same ground gives, by its own formula, 3 m times the noise's gradient over the score's, about
0.1 m. Held to a fifth either side, the bound G138's own test holds.

*Where it stops*: the shade layer's gradient is not taken (it would mean sampling the paint around
every cell), so a shade edge drawn by the paint alone still narrows the band; and with the noise's
weight exactly 0 `sward_site_form` multiplies the band by 0 and draws a line, because it reads a
gradient in the noise's units (a request under the subsystem page's "Not yet").

## What the reels say the result still lacks

r01 (the model): a tall meadow of mixed species at different heights with magenta clover heads, white
and yellow small flowers, dense. r04: short, saturated turf down to a glacial stream's edge, few
flowers. The rule gives the community and the covers, and the far form carries the near ring's
flowers into the card ring at their colour and coloured area (flora.md, "The meadow community").
What the eye reads in r01 also needs the whole density (the pose table's cap holds a tile to 256
plants, 6 % of its cells, where r01's meadow is a flower or more in every square metre: a `field`-kind
plant or a static far form would lift it), the herbs' leaf and flower sets (flat colour here), and
wetness in the sward's colour (r04's banks are a greener, brighter turf than the slope above). No
picture was taken for this row: the result has not been looked at beside the reels.

## The meadow at whole density (G167)

G166's pictures of this community showed not one flower head: 256 plants a 64 m tile, each a
`parts` herb. G167 makes the plant a field (flora.md, "The meadow at whole density") and reads a
cover by its definition.

**A cover is a share of the ground.** Domin's cover is the share of a plot the species' shoots cover
seen from above (the NVC Users' Handbook, read for G140). So a species whose plant has a crown of
radius r stands at `1 / (π r²)` plants a square metre where it grows, and covers its cover's share
of the ground: in a cell of side s a drift of `n = s² / (π r²)` plants (held to 8) on a disc of
`r sqrt(n)`, drawn with probability `cover · s² / (n π r²)`. Before G167 a cell held one plant at
the cover's probability, which made a species' plants `cover` a square metre — a clover with a
cover of 0.17 stood one plant in six square metres, where by its crown it stands about five in
every square metre it grows in.

**The crowns are chosen** (`crown_m`, sward_placement.cpp's table): measured crown or clump widths
were not found for most of the thirteen (the sub-agent's search, 2026-11-11). What was read: *Geranium
sylvaticum* is 1–5 rosette shoots and 1–3 flowering shoots joined by a rhizome of 3–10 cm, a clumped
patch (Wagner et al. 2026, Biological Flora of Britain and Ireland No. 308, J. Ecol. 114,
doi 10.1111/1365-2745.70225, full text): 15 cm chosen. *Leucanthemum vulgare* 0.1–0.5 m spread is a
garden figure (RHS): 8 cm chosen for a meadow's single shoot. The rest, 6 cm (*Cardamine*,
*Conopodium*) to 20 cm (*Filipendula*), are chosen from the plants' sizes and say so.

**A check against counts of flowers** (not a fit): Wehn et al. (2019, Data in Brief 25:104065), 1 m²
plots in early-cut semi-natural hay meadows at Botiza, Romania, 3 August 2016, read: *Lotus
corniculatus* 29.6 ± 10.1 and *Leucanthemum vulgare* 28.1 ± 15.0 flowering units a square metre
(whether plots without the species are in the mean is not stated). A drift of eight bird's-foot
trefoils carries 32 heads in its square metre, and of eight ox-eye daisies eight; the trefoil is
the right order and the daisy, at one head a shoot, a quarter of it — a daisy stand is denser than
its crown says, or carries several shoots a plant.

**The flowering months** (`bloom_from`, `bloom_to`). PLANTATT, the BSBI atlas and Stace were not
read (their tables could not be fetched); what was read, secondary except where said: *Caltha
palustris* March to May (the Floodplain Meadows Partnership; the Wildlife Trusts give March to July,
NatureSpot to August: the specialist's is taken), *Filipendula ulmaria* June to September,
*Cardamine pratensis* April to June, *Ranunculus acris* April to October, *Trifolium pratense* May to
October, *Conopodium majus* May to June, *Leucanthemum vulgare* June to September, *Lotus
corniculatus* May to September, *Thymus polytrichus* June to September (the Wildlife Trusts' "when to
see" pages, which cite no flora), and *Geranium sylvaticum* June to August (Fitter and Peat 1994, as
quoted by Wagner et al. 2026, read; in a Pennine Dales hay meadow its flowering had ended by 5 July
1988, Smith and Jones 1991 as quoted there). On the community scene's day 200 (19 July) the wet
margin's marsh marigold and cuckooflower and the mesic sward's pignut are past flowering; meadowsweet,
buttercup, clover, wood cranesbill, ox-eye daisy, trefoil and thyme are in flower.

**The budget, counted** (`meadow: a cell's plant is its cell's from any tile order`, the test's
graded 64 m tile on its slope with the whole community, MSVC): the odds sum past one in every
cell, so at the default cap the near ring holds **4,096 drifts a tile, 2,623,872 triangles** (640 a
square metre, beside the sward's own 1,280 near and 768 coarse a patch), and the card ring **524 far
forms, 167,664 triangles**. A drift is 264 to 3,144 triangles (`meadow: a drift is the field
kind`: a forb out of flower 264, the sedge's and the rush's 384, a buttercup's 864 with 40 heads,
a thyme's 1,416 with 48, a meadowsweet's 3,144 with 192), eight plants of one parentless part
each. Before G167 the same tile held 256
herbs of 48 to 498 triangles, at most 127,000. Cluster LOD thins a drift with distance like any
mesh; the drifts are no instance of the pose table, so the CPU poser and the pose table's slots
are unchanged by them.

**The frame, measured** (`engine-view --benchmark` along `content/test-scenes/meadow/community`'s
camera path, 1920 × 1080, CSM shadows, `msvc-release`, the base's binaries (be3826d9) against
04ac9a60's, one GPU lease, 2026-11-11 21:05, RTX 5090; `--wait-quiet 30` ran anyway: the harness's
`machine_state` recorded 90 % then 85 % of the CPU in other processes for the before, 64 % then
71 % for the after, the GPU idle at both ends, so the CPU figures are upper bounds):

| | before (G140, 256 herbs a tile) | after (G167, drifts as a field) |
|---|---|---|
| GPU total, median / p95 / p99 | 2.315 / 2.452 / 4.139 ms | 2.182 / 2.330 / 4.330 ms |
| cull, median | 0.231 ms | 0.277 ms |
| hardware raster, median | 0.344 ms | 0.367 ms |
| sun's cascades, median | 0.704 ms | 0.532 ms |
| deformed-vertex pool, median | 0.199 ms | 0.186 ms |
| CPU frame, median / mean | 0.967 / 1.126 ms | 0.392 / 0.577 ms |
| flora posed on the CPU | 3,141 instances, 82,712 parts, 0.56 ms mean, 4.63 ms max | none |
| pose table | 20,166,240 bytes | 480 bytes |
| field instances, meshes | 146,443, 32 | 189,155, 52 |

The meadow at its whole density draws for less than it did at 6 %: a herb was posed on the CPU and
cast its swaying pose into every cascade, and a drift is posed on the GPU and casts into the
nearest cascade at rest, so what the cull and the raster gained (0.07 ms) the cascades and the
CPU's poser more than gave back. The after's leaves were the herb's green in this run
(04ac9a60); the measured albedo (f2b0581e) changes no triangle or pass.

**What the pictures say** (the community scene's four markers, before and after, one derived-data
root, 2026-11-11; beside reels r01 and r04). At eye height the meadow now carries flowers: clover's
magenta and thyme's purple drifts and ox-eye daisies on the dry flank, specks of colour across the
floor to 40 m; at the wet margin the sedge and rush drifts close the sward between the grass
patches, darker and denser than the flank, and with the leaves at the blade's albedo they read as
graminoids rather than the first run's pale fans. What a person still objects to beside r01: the
heads are few and small for a meadow in flower (one drift a cell; most of the wet margin's forbs are
past flowering on 19 July, and meadowsweet's cream heads are sparse); the grass is still thin
blades in visible rows over a flat floor; from 50 m the flowers vanish into one olive tint with
darker blotches where the near ring draws (the far form's colour carries no flower: G170's); the
light is flat. The marsh marigold's yellow, which the wet margin should show in May, needs the
scene's day.

**Wet against dry, for G168** (read: Punalekar et al. 2016, Remote Sensing 8(2):112, field
spectroscopy of a floodplain meadow, MG4): the wet community (*Carex*, *Juncus acutiflorus*,
*Filipendula*, *Cardamine*) and the dry one were "spectrally very similar", the dry slightly
brighter overall, the difference significant only at 400–541 nm and 666–681 nm, the leaf area index
the same; leaf chlorophyll 31.5 µg/cm² for *Carex acuta* against 32.6 for *Sanguisorba*, and 23.5
for *Juncus acutiflorus*. So a wet margin does not read darker for its leaves' pigment; what a
person sees is the wet soil between them and the dry slope's standing dead, which is where G168's
colour has to come from.
