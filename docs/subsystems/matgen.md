# matgen (domain)

**Purpose.** Tiled PBR sets by role, **generated**: a recipe of whole numbers builds the four
images a material pack's set holds — base colour, normal, roughness, height — for the roles the
engine's solvers ask a pack for and no pack has: a gravel bed, a sand bar, scree, rock faces,
ground floors. A photographed or diffusion-made pack is the owner's to make
([materials](materials.md), [content generation](../content-generation.md#material-packs)); a
deterministic, research-grounded one is the engine's, and with it a checkout that has fetched
nothing still draws gravel as gravel instead of a flat colour (roadmap G148; the sources and each
number against them: [generated material sets](../experiments/generated-material-sets.md)).

**Why this shape.** Three decisions:

- *A set is derived data, never a committed image.* A recipe (`engine.matgen.Set`) is integers
  only, so its canonical JSON is the same bytes everywhere and `set_key` (that text and
  `k_matgen_version`) addresses what it builds. `engine-content matgen` writes sets into a pack
  directory under the git-ignored `content/materials/`; the repository holds the generator and
  the recipes' defaults, as code.
- *It attaches where a pack does.* Every solver already resolves a part's set by role through
  `materials::resolve_set` over a `materials::Pack`. A generated set is a role in a pack whose
  directory the lookup reads **behind** the fetched one (`materials::pack_at`, below), so no
  solver, host or renderer file changed to use one, and a fetched role always wins.
- *The generator knows nothing of the image's edges.* Every clast, crystal, joint and leaf lives
  on the torus of the tile; see "A set tiles by construction".

It is a capability with no registrar: nothing links it but `engine-content` and its own tests, so
`ENGINE_WITH_MATGEN=OFF` removes the command and nothing else.

## How a set is built

`build_set(set, images, stats, error, window)` validates the recipe, fills a field of texels
(linear albedo, roughness, height in metres) by the recipe's kind, and finishes it the same way
for every kind:

- **Hollows.** The height is blurred (a wrapped box filter, twice, in integers) and a texel
  lying under the surface around it is darkened by `cavity_permille` times how far under it is
  (full at the kind's own depth: 0.6 of the median clast's thickness, a rock's relief, 4 mm of a
  floor). The resolve has no occlusion at this scale, and a bed whose gaps are as bright as its
  tops reads as a painted floor; a photographed set carries the same shade. *Traded:* it is shade
  baked into albedo, right for sky light and wrong under a low sun, and a knob so a renderer that
  gains micro-occlusion can turn it off.
- **Wetness.** `wet_permille` multiplies albedo by `1 - 0.5 wet` and moves roughness toward
  `0.2 + 0.25 roughness`.
- **The images.** Base colour through the texture build's own sRGB tables
  (`texture::linear_to_srgb8`); the normal from the height's wrapped central differences in
  metres per metre, OpenGL (+Y up the image), so its strength is the surface's real slope and
  `normal_scale` stays 1; roughness; height over the set's own range (`SetStats::height_min_m`,
  `height_max_m`).

### A set tiles by construction

A noise takes a lattice coordinate of any size and wraps its nodes by its own period; a clast,
a thallus or a leaf has its centre in integer sub-texels (a 256th of a texel) and every texel it
touches is wrapped; a joint trace is a level of a phase that winds the torus a whole number of
times; the hollows' blur wraps. No function knows where the image starts. `Window` moves that
start by whole texels, and **the test builds each kind from a window half a tile and three
texels over and requires every image to be the first build's bytes, rotated — zero bytes
different**. That is exact, not close, because every coordinate is reduced before it is used:
texels a side are a power of two, so a lattice coordinate is exact; a cell's point is taken
within its own cell; offsets from a centre are integer differences; the blur sums integers.
The seam is then a column like any other, and a second test says so directly (the step across
the edge lies inside the range of the steps within).

## Granular sets

A bed of clasts ([grounding](../experiments/generated-material-sets.md#a-bed-of-clasts)).

- *Sizes.* The recipe states what a pebble count would read — `d50_um`, `sorting_milliphi`,
  cut `truncate_milliphi` either side — which is the distribution **by area shown**. Clasts are
  drawn **by number** from the same normal in phi moved down by 2 s squared (s the sorting in
  ln D), until their plan areas sum to `cover_permille` of the tile; the cut is raised to three
  texels and lowered to a third of the tile where the image cannot hold the stated one.
- *Form.* The intermediate axis b is the drawn size; a = b over a draw from the `ba` range, c =
  b times a draw from the `cb` range; a clast lies on its a-b plane. The outline is the unit
  circle of the clast's frame cut by a polygon of five to eight edges standing at the circle for
  a well rounded clast and at the inscribed polygon for a very angular one; the top is an
  ellipsoid's cap, or a bevelled plateau broken by one ridge. Both go from block to pebble
  between roundness 0.15 and 0.65, not evenly over the scale: the roundness classes are
  logarithmic, and a "rounded" pebble has no faces left (an even mix left the default bed's
  pebbles faceted like cut stones, seen in its normal map).
- *Fabric.* The a-axis lies across the flow (the set's +u) for `alignment_permille` of the
  clasts; each top leans by the imbrication dip (half to one and a half times `imbrication_deg`)
  down the flow and by up to `scatter_deg` about any axis, capped at what the clast's thickness
  can show.
- *Placing.* Largest first, each at hashed positions on the torus, accepted where at most two of
  thirteen probes of its outline are already taken (clasts lean on each other); one that finds no
  place in 24 tries goes to a lower layer, seen down the gaps. The surface is the highest top at
  each texel.
- *Between the clasts.* A matrix of grains (`matrix_um`) of the same lithologies, a quarter of a
  clast's thickness down — or, with `openwork_permille`, voids: the matrix deeper and darker,
  and that share of the drawn clasts laid again, elsewhere, a layer down, because what shows
  down a gap in a talus is more blocks. `cover_permille` is the plan area drawn, as a share of
  the tile; what shows is less by the clasts' overlap (`SetStats::clast_cover`: 0.80 of the
  default bed for 1.0 drawn).
- *Colour.* Each clast is one lithology of the mix by share, its tone its own, with a grain
  inside it (`speckle`), a broad mottle, and for some a pale vein straight across. A clast past
  D50 strays from its lithology's tone less, by the square root of D50 over its size: the few
  largest clasts of a tile are its landmarks, and a pale or dark one among them is what the eye
  finds again every period ("Repetition").
- *Embedded* (`buried_permille`, G151). A filled bed's matrix stands higher by half that share of
  the median clast's thickness, and each clast settles into it by up to the share of its own;
  where the matrix's surface is above a clast's top the texel is matrix — the lower clasts lie
  half buried — and silt drapes a clast's foot where the matrix nearly reaches it (0.12 of a
  median thickness). The matrix's grains pale toward a silt by 0.6 of the share, and are drawn as
  cells with darker edges under a 2 cm mottle, so a matrix is grit and not a paste.
  `SetStats::buried_share` is the surface where the matrix stands over a clast (0.10 of the
  default bed; an unburied bed's smallest clasts also lie under the matrix's own level, 0.03).
  The default's 0.30 sits in the 25 to 50 % embeddedness class of Platts, Megahan and Minshall
  (1983); 0.35 left a third of the bed's surface as matrix in the first engine picture, a paste
  between the stones.
- *Film* (`film_permille`). A stable top carries a thin brown-olive film strongest on its crown
  and none down its sides; a fifth of the clasts carry none (turned or scoured), the rest
  0.4 to 1.15 of the strength.
- *Broken faces* (`broken_permille`). That share of the clasts has one more edge, a straight cut
  between 0.4 and 0.75 of the outline's radius, whose bevel stays sharp however rounded the rest
  is, and whose rock is 18 % paler: a fresh fracture.
- *Several depths* (`depth_layers`, openwork only). The drawn clasts are laid again once a layer
  down, for each of 1 to 3 layers, each layer a further 1.2 median thicknesses down and 0.72 as
  bright, so a gap shows blocks wedged at every depth and not one floor of them; the voids' floor
  lies under the deepest.
- *Roofed blocks.* An angular clast's top is cut by two fracture planes, not one, so it is a roof
  and not a plate.
- *Fines* (`fines_permille`, openwork only). An old talus fills from the surface down; that share
  (about: a one-octave noise cut at a level) of the voids is filled with fines a third of a median
  thickness down, which cover the lower layers there.

`SetStats` reports the count over every texel that shows a clast (`d16_mm`, `d50_mm`, `d84_mm`),
the share that is matrix, the clasts drawn and those in each layer, and the drawn clasts' mean
maximum projection sphericity.

## Rock sets

A rock face ([grounding](../experiments/generated-material-sets.md#a-rock-face)): crystals as a
cellular pattern at `grain_um`, each a mineral of the mix by modal share; relief of `relief_um`
half range from five octaves; up to two joint sets, the second across the first, each trace
shown along part of its length; a weathering rind over `rind_permille` of the face (the crystals'
contrast muted toward the rock's mean, tinted, stained) against fresh breaks a flake lower, the
patches cut from a low noise at the share's quantile so the share is exact; pits; bedding
laminae; and crustose lichen on the rind only. **Lichen by exposure is a parameter of the set, not
a mask**: one set is one exposure, and a mask by aspect over a mesh is the cover capability's.

What G152 added, each from the judgement of the first sets in the engine ("What the sets look
like"):

- *Form above the decimetre* (`form_um`): scallops a third of a metre across (dishes between
  sharp rims, a face that spalls in flakes, at 0.15 `form_um` at a cell's rim) and ribs half a
  metre apart running down the face (`form_um` peak to peak). **A broken step was drawn too and
  is gone** (`k_matgen_version` 3): 1.4 `form_um` on the 0.5 level of a two-period noise, whose
  contour is a set of curved closed loops. A rock set is sampled stochastically, which cuts that
  contour into offset pieces, and under a low sun each piece read as a lit and a shaded crescent:
  crescent stamps over the granite, the wet granite and the limestone, on a plane and on a block
  (`captures\sheet-near-after2.png` in the row's scratch). Softening the scallops (0.6 to 0.15)
  had not removed them; removing the step was the test of the diagnosis ("In the engine"). A
  step that would survive the sampling is a near-straight trace that winds the torus, as a joint
  does; not drawn.
- *Joints as worn gaps*: a floor of fines its own width (the width wandering along the trace from
  0.25 to 0.85 of `joint_width_um`), shoulders rounded as a quarter circle over two and a half
  widths, four widths deep; the hollows' shade does the rest.
- *Fresh breaks* keep 70 % of the crystals' contrast over the rind's and are stained a little
  too (newer, not new), behind a sharp edge (0.8 `relief_um` deep over a share step of 0.006).
- *Run-off streaks* (`streak_permille`): dark stripes down the face from every joint floor, near
  black, as the cyanobacterial and lichen films of the Alps' ink stripes
  ("Tintenstriche"). **Down the image is down the face** (+V up, the normal map's convention); a
  set laid with a rotation turns its streaks with it. For each texel, how far below the nearest
  source it lies, capped at 0.45 m, is an integer found by two sweeps down each column round the
  torus, so the streaks tile and do not depend on the window; they fade with that distance and
  are split into strands by a noise 3 cm across and 60 cm long. `SetStats::streak_share` is the
  face they darken by more than 15 %.
- *Lichen colonies* (`lichen_max_mm`): thalli from 2 mm across up to the recipe's largest
  (granite 120 mm), drawn log-uniformly with a skew to the small (an old face holds a few large
  colonies among many young ones), until their discs reach the Poisson cover; outlines lobed by a
  6 mm noise and broken by a 25 mm one; older and darker at the centre and eroded through in
  places on a colony past 40 mm, brighter at the margin; where two meet each keeps the texels
  nearer its own centre, so neighbours stop at each other. Five species: the map lichen with its
  black prothallus, the orange of *Xanthoria elegans*, grey, dark and white crusts.
- *Moss in a wet face's joints* (`moss_permille`): shoot tips over the joint floors and
  shoulders along that share of the joints' length, raised three joint widths.
  `SetStats::moss_cover`.

## Ground sets

A floor ([grounding](../experiments/generated-material-sets.md#a-floor)): the parts of the
sward's floor bake ([flora](flora.md#the-ground-under-the-sward-g141)) as sets — moss (shoot tips
in cushions) and soil (crumbs) split at the moss share's quantile, dead leaf fragments as a
Poisson cover of `thatch_permille`, live leaves with a midrib and tapered tips as one of
`live_permille`. `SetStats` reports the three covers.

### A floor at standing height (G187)

The parts above are all a few millimetres across, and G149's pictures showed what that gives at
1.7 m: flat felt colour. A standing eye reads a floor by what lies between 5 cm and 1 m
([grounding](../experiments/generated-material-sets.md#a-floor-at-standing-height-g187-2026-11-11)),
so the ground kind (`Ground` version 2) now builds that band, every part a field of the recipe
and zero in a recipe that does not ask for it:

- **A patch mosaic** (`patch_permille`): a field of 0.6 m and 0.2 m octaves that sets, where it
  is high, a turf's tall rejected tufts (longer, darker blue-green leaves, more of them dead)
  against its short-grazed lawns (shorter, yellow-green), a soil's and a moss's moist against dry,
  and where fresh straw lies over decayed litter.
- **Tufts** (`tuft_mm`): live leaves fan out from a tiller's centre, 6 to 20 a tuft, each rising
  from its base (its far end paler, lit) and lifting the tuft's top; `dead_permille` of them are
  straw. Without tufts a leaf lies anywhere, as before.
- **Cushions** (`cushion_mm`) of moss and **clods** (`clod_mm`) of soil: cells of a cellular field
  whose lookup is warped by half a cell (a polygon's straight edges read as crazy paving); a
  soil's clods domed by `relief_um`, a moss carpet's relief smooth hummocks of `relief_um` at the
  cushions' scale (the cells' domes creased at every seam, and under a low sun the creases drew
  crazy paving in the engine); a moss cushion each its own tone between lit yellow-green, the shaded body and a
  dry brown and its own vigour (0.75 to 1.25 of the tone), the seam between two showing only where
  a second field parts them; a soil's clods cracked along some of their edge, not all of it.
- **Stones** part buried in the soil (`stones_permille`, `stone_min_mm` to `stone_max_mm`, radii
  log-uniform so small ones are many), ellipses whose crowns stand a quarter to three fifths of
  their minor axis above it, each filmed with the soil's colour (more at its foot).
- **Litter in two layers** (`decay_permille`): the decayed share first and dark, fresh straw
  over it on a mosaic of its own 0.22 m across, and lying with the lodged grass where
  `aligned_permille` asks, along a direction field 0.8 m across.
- **Mounds** (`mound_um` at `mound_spacing_mm`): a low undulation under everything, so the normal
  map shows at 15 m that the floor is not a plane. A thufur's 10 to 25 cm is the ground's
  geometry, not the texture's: the set carries a tenth of it.
- **The hollows' shade** reads over two centimetres and is full at a centimetre deep where a
  recipe has tufts or cushions (5 mm otherwise); it was 4 mm over 6 mm, a crumb's scale.

**A layer's share is the share drawn.** A clustered layer does not cover by the Poisson law (a
tuft's leaves overlap each other), so the litter and the leaves are laid until their cover
reaches the asked share; `SetStats::thatch_cover` and `live_cover` are the asked shares to within
a fragment's area.

The tile is 2 m at 2,048 texels (1 mm a texel) for all four, so the mosaic has a dozen patches a
tile. What says the floor is no longer flat are the measures under "What a standing eye reads".
Over seeds 61 to 63 at 256 texels (`msvc-debug`; the base is `k_matgen_version` 3 with its 0.5 m
tiles, this is version 5), with the bounds the test holds each role to — **chosen** between the
two, so the base fails every floor; the landmark ceiling the base meets except the moss:

| Role | contrast at 5 cm: base, now, floor | at 25 cm: base, now, floor | relief RMS mm: base, now, floor | landmark: base, now, ceiling |
|---|---|---|---|---|
| `ground/turf` | 0.018–0.021, 0.056–0.068, > 0.04 | 0.002–0.005, 0.035–0.051, > 0.02 | 2.6–2.8, 5.8–7.9, > 4 | 0.10–0.11, 0.14–0.15, < 0.30 |
| `ground/moss` | 0.035–0.058, 0.133–0.140, > 0.08 | 0.008–0.024, 0.068–0.080, > 0.04 | 0.44–0.65, 7.0–9.6, > 3 | 0.22–0.46, 0.26–0.39, < 0.45 |
| `ground/soil` | 0.053–0.065, 0.106–0.118, > 0.08 | 0.014–0.025, 0.037–0.059, > 0.03 | 0.57, 3.6–4.7, > 2 | 0.31–0.43, 0.22–0.26, < 0.40 |
| `ground/thatch` | 0.027–0.030, 0.045–0.046, > 0.035 | 0.006–0.012, 0.023–0.025, > 0.018 | 1.43, 4.2–5.6, > 2.5 | 0.14–0.17, 0.11–0.13, < 0.30 |

No source gives a photographed floor's block contrast at these scales; the floors say only that
the structure the base lacked is there. The thatch's is the weakest gain: a litter mat is one
material, and its decayed patches are held soft (fresh straw is laid over a decimetre-scale
mosaic of its own, at a rate that falls to 0.4 where the mosaic is high) because hard-edged ones
read as a cow's hide.

## Wood sets

`wood/plank` and `wood/log` (G188; [grounding](../experiments/generated-material-sets.md#weathered-wood-g188-2026-11-11)):
a softwood face, **its grain along the image's u**, as a sawn board shows it (`form: sawn`) or as
the outside of a log does (`form: log`), weathered toward grey.

- **The figure** is a phase across the tile — whole bands of `figure_um`, so it tiles — that
  wanders `wander_mm` across the grain over 0.7 m along it, and climbs `spiral_ddeg` (a log's
  spiral grain) as a whole number of bands over the tile's length. Each band is earlywood then
  `latewood_permille` of latewood, the share filtered over the texel's footprint in the phase so a
  band narrower than a texel keeps its tone instead of aliasing. Where the face runs along a ring
  the phase's gradient falls, the bands widen and fold into a flat-sawn board's arches.
- **Weathering**: earlywood toward a pale silver, latewood a darker grey, standing `relief_um`
  proud of the eroded earlywood; how far each place has greyed is `grey_permille` plus or minus
  `grey_patch_permille` over a 0.25 m field, so a board is grey to brown; `stain_permille` of dark
  streaks along the grain; fibres as fine streaks that follow the bands.
- **Knots** (`knots_per_m2`, `knot_min_mm` to `knot_max_mm`): the rings bend round each over three
  of its radii; its body dark, ringed and proud; a third are dead knots with a dark rim.
- **Checks** (`checks_per_m2`, `check_min_mm` to `check_max_mm`, `check_width_um`,
  `check_depth_um`): splits that follow one band (they open along the grain, wherever it wanders),
  widest at their middle, dark and deep.
- **Bark** (`bark_permille`, a log's): patches where a field long along the stem is lowest, exactly
  the share; plates `bark_plate_mm` long and fissured between, standing 6 mm proud so the
  hollows' shade darkens the wood at their edge.

**A consumer maps u along a member's length**, in metres: a 0.2 m board then shows a 0.2 m band
of a 2 m tile, and boards at different offsets do not repeat each other's band. A projection that
takes u from a world axis (the bridges' box mapping in `domain/bridges/src/mesh.cpp`, at this
row's base) runs the grain across a member that lies along the other axis; that is the
consumer's to turn.

## The defaults

`default_sets()`, sorted by role; `engine-content matgen` builds them all.

| Role | Kind | Tile | What | Asked for by |
|---|---|---|---|---|
| `gravel/bed` | granular | 2 m | D50 45 mm, 1 phi, rounded, imbricated, wet through (a bed lies under its water), five lithologies, embedded 0.30, a film on its tops, 12 % broken | a channel's bed ([channels](channels.md)) |
| `sand/bar` | granular | 0.5 m | sand with a 12 % cover of granules and small pebbles, dry | a channel's bars |
| `rock/scree` | granular | 3 m | D50 110 mm, angular and roofed, openwork three layers deep, fines in 15 % of its voids, no fabric | named for scree and deposits; see "Not yet" |
| `rock/granite` | rock | 1.5 m | a monzogranite, 3.5 mm, 92 % rind (an old face), 16 % lichen up to 120 mm, form 22 mm, streaks | boulders, cliffs, deposits, a soil profile's stones ([rocks](rocks.md)) |
| `rock/granite_wet` | rock | 1.5 m | the same wet through, a darker rind, 4 % lichen, strong streaks, moss in 55 % of its joints | a recipe that names it |
| `rock/basalt` | rock | 1.5 m | a dark groundmass, vesicles, a rusty rind, form 15 mm | `RockType::Basalt` |
| `rock/limestone` | rock | 2 m | micrite, solution pits, a grey rind, 15 % lichen, form 25 mm, strong ink stripes | `RockType::Limestone`, the caves |
| `rock/sandstone` | rock | 1.5 m | buff grains, twelve laminae, one joint set, form 12 mm | `RockType::Sandstone` |
| `gravel/bed.1`, `.2`, `sand/bar.1`, `.2`, `rock/scree.1`, `.2` | granular | as their role | the same recipe from other seeds ("Repetition") | a consumer that picks a variant by place |
| `ground/soil` | ground | 2 m | loam in 35 mm clods, cracked, 8 % stones of 5 to 40 mm part buried, moist and dry patches, a little litter, mounds | the sward's cook (`--sward-assets`), a terrain layer |
| `ground/moss` | ground | 2 m | a moss carpet in 150 mm cushions of their own tones, wet and dry patches, a little litter, mounds | the sward's cook, a terrain layer |
| `ground/thatch` | ground | 2 m | a litter mat: decayed below, fresh straw over it in patches, lying with the lodged grass | the sward's cook, a terrain layer |
| `ground/turf` | ground | 2 m | a grazed sward: live leaves in 110 mm tufts, short yellow-green lawns against tall dark rejected tufts, a fifth of the leaves dead, over litter, moss and soil | a terrain layer that names it, the meadow |
| `wood/plank` | wood | 2 m | sawn softwood boards weathered grey to brown: flat-sawn figure, latewood ridges, checks, knots, stains | a bridge's deck, rails and posts ([bridges](bridges.md)) |
| `wood/log` | wood | 2 m | the outside of a debarked log, silvered: spiral grain, long drying checks, knots, bark on 15 % | a bridge's logs, sills and stringers |

Rock sets are `stochastic` (R131: a rock set is laid over cliffs many times its repeat, as
`tools/pack-materials.ps1` defaults every `rock/*` role); the granular and ground sets are
tiled, `rock/scree` included, because three offsets of a bed of clasts blended is no bed.

## Repetition

A tiled set repeats at its period, and what shows the repeat is not the tile's texture but its
few most salient features: the brightest or largest cobble, a dark patch, a vein, seen again in
plain rows to the horizon (the first gravel bed in the engine, G149; the tiling literature frames
the goal as output with no visible repetition — Cohen et al. 2003's Wang tiles, Wei et al.
2009's survey — and sets no rule for how large a tile must be against its largest feature, so the
measure and its bounds here are this row's own). G153 acts where a recipe can:

- **The measure** (`SetStats::landmark_contrast`, `block_contrast`): the albedo's luminance over
  a 16 by 16 grid of blocks, and the largest and the RMS departure of a block from the tile's
  mean as a share of it. A block that stands out is a landmark seen again every tile. Measured on
  the base generator's 1,024-texel pack (the same measure over its base colour PNGs): the gravel
  bed 1.04 (one sixteenth of the tile twice the mean: one pale cobble filling it), the scree
  0.89, the sand bar 0.29, granite 0.37, turf 0.10. The test holds the tiled granular roles under
  bounds over three seeds ("Testing").
- **A larger tile where a role's features are large**: the gravel bed's tile is 2 m (its D84 is
  86 mm, its cut 180 mm: a one-metre tile held a dozen clasts past D84, each a landmark), the
  scree's 3 m; and the largest clast is a sixth of the tile at most.
- **The largest clasts' tone held** toward their lithology's (above, "Colour").
- **Variants**: `default_sets` writes `<role>.1` and `<role>.2` beside each tiled granular role,
  the same numbers from seeds mixed from the role's (`variant_of`, `k_tiled_variants`). **Who
  picks one is not this module's** (the resolve and the hosts' lookup are closed to it): a
  consumer would hash the lattice cell its surface's tile falls in (the terrain's tile corner, a
  channel's station) to pick one of the three, so neighbouring tiles differ and the same place
  always draws the same one; the seams between variants are then the seams between two beds,
  which a height blend or a placement's own edge has to carry. Asked of the hosts under "Not
  yet".

### What a standing eye reads

Three measures every build takes of its own result (`SetStats`, G187), for the band a standing
eye reads a surface by, between a few centimetres and a metre: `contrast_5cm` and
`contrast_25cm`, the RMS Weber contrast of the albedo's luminance averaged over blocks of the
power-of-two side nearest 5 cm and 25 cm (0 where the tile holds fewer than two such blocks a
side), and `relief_rms_mm`, the height's RMS about its mean. A surface whose structure is all a
few millimetres across averages out to one tone over 5 cm blocks and reads as flat colour at
1.7 m, whatever its texels hold; the landmark measure above is the other side of the same band,
a ceiling where these are floors.

## The command

`engine-content matgen [--out <pack dir>] [--texels <n>] [--role <role>]... [--set <recipe.json>]... [--force]`

builds every default (or the roles and recipe files named) into a pack directory: a role's
directory holds `base_color.png`, `normal.png`, `roughness.png`, `height.png`, a 4 by 4
`metallic.png` of zeros (a pack role names five maps) and `set.json`, which records the key the
images were built for, the recipe and what the build measured. **A set whose `set.json` records
the recipe's key and whose five files exist is not rebuilt**; the sidecar is written last, so a
run that died leaves no key. `pack.json` beside them is a pack's manifest, each role with its
key under `provenance`, so the pack's hash — and every generated mesh's build key — moves when a
recipe or the generator does. One JSON line a set and one for the pack.

### The default

`--out` defaults to `<repo>/content/materials/generated`. `materials::pack_at(dir)` reads
`dir/pack.json` and, behind it, `dir/generated/pack.json` (`materials::merge_packs`): a role the
fetched pack has is the fetched pack's, every other role the generated one's, and where there is
no fetched pack the generated one is the pack. Every host finds its default pack through
`pack_at(default_pack_dir(...))`, so after one `engine-content matgen` a worktree's rocks,
channels, soil and caves resolve their roles with no flag. The textures are built from the PNGs
by the texture step as any pack's are (`<ddc>/textures/<key>.tex`).

**What that leaves** (the hosts' files were not this row's): the sets are built by a command,
not on demand — a scene read does not call the generator when a role is missing; `tools/fetch-materials.ps1`
replaces the destination's contents, so fetching a pack removes `generated/` and the command is
run again (seconds, below); and a caller that names its own pack directory (every test) gets the
generated sets only by building them there.

## Same bytes everywhere

Every decision is a hash, an integer, IEEE `+ - * /`, `sqrt`, `floor` or the engine's own
`det::exp`, `det::log`, `det::sin`, `det::cos`; the sRGB encode is the texture build's committed
table. The build is single-threaded, so there is no thread count for the bytes to depend on.
`matgen_tests.cpp` pins the four images' hash for six defaults at 256 texels, taken on MSVC in
`msvc-debug` and re-taken for `k_matgen_version` 2 (G149 to G153; the turf's pin did not move,
since the ground kind did not change). **The Linux containers reproduce the version-2 pins**:
`linux-clang-debug` (2026-11-11 10:18, at 87eed81b) and `linux-gcc-release` (10:40, at
7ac75070) and `linux-gcc-release-v2` (13:13, at e4e3bfc6), `-Filter "matgen|materials"`, every
test passed. Version 3 (the step removed; the granite and sandstone pins re-taken):
`linux-clang-debug` (14:25, at 39896b51) and `linux-gcc-release` (14:35) passed;
`linux-gcc-release-v2` did not run for version 3 (the container lock was not had before the
row's hand-back). Version 4 (G188) added the two wood pins and moved no other; version 5
(G187) moved the turf's and added the moss's, the soil's and the thatch's. What the containers
made of versions 4 and 5 is under "What it measured". The streaks' distance below a
source is an integer from two sweeps round each column, the lichen's owner of a texel is decided
by an f64 comparison of two quotients of the same arithmetic, so neither adds an operation a
toolchain rounds its own way. Nothing was needed that `core/math/deterministic.h` lacks: a normal draw is Box and Muller's
from `det::log` and `det::cos`, a cube root is `det::exp(det::log(x) / 3)`, a tangent
`det::sin / det::cos`.

## Budgets

A set costs nothing in a frame and nothing per tile: it is built once per recipe, by a command,
and from then on it is a pack's images — three textures a role the renderer uploads (colour,
roughness and normal; height is carried), 1.4 MB each at 1,024 squared and 5.6 MB at 2,048 in
the `.tex` container (the figures [materials](materials.md#the-first-pack-2026-10-19) measured
for a pack's maps of those sizes), shared by every mesh that samples the role. Nothing scales
with the world's size. What a build costs is under "What it measured".

## What it measured

`engine-content matgen --force` in `msvc-release`, every default, 2026-11-10 23:27 to 23:31 on
the shared machine (RTX 5090 box, 36 threads): **not a quiet run** — about twelve agents were
building and testing, the processors read 65 % busy at the end and free memory fell under 2 GB
at the start, so each figure is an upper bound. One thread; `build` is `build_set`, `write` the
five PNGs.

| Set | 1,024: build ms | write ms | PNG bytes | 2,048: build ms | write ms | PNG bytes |
|---|---|---|---|---|---|---|
| `gravel/bed` | 873 | 426 | 4,975,020 | 5,117 | 1,470 | 16,698,883 |
| `sand/bar` | 1,276 | 833 | 4,255,260 | 2,691 | 1,246 | 13,420,868 |
| `rock/scree` | 1,552 | 813 | 4,880,654 | 3,664 | 1,344 | 17,910,380 |
| `rock/granite` | 1,818 | 778 | 3,044,649 | 7,181 | 1,099 | 9,220,140 |
| `rock/granite_wet` | 2,294 | 614 | 2,812,091 | 7,174 | 1,155 | 8,468,409 |
| `rock/basalt` | 1,696 | 338 | 3,008,115 | 8,826 | 1,222 | 10,145,953 |
| `rock/limestone` | 2,598 | 572 | 3,459,416 | 9,344 | 1,492 | 11,728,415 |
| `rock/sandstone` | 2,554 | 746 | 2,911,626 | 6,866 | 1,167 | 9,887,204 |
| `ground/moss` | 1,013 | 357 | 4,796,841 | 5,815 | 1,733 | 15,933,246 |
| `ground/soil` | 983 | 315 | 5,227,616 | 5,973 | 6,132 | 20,619,558 |
| `ground/thatch` | 1,494 | 332 | 5,621,561 | 6,044 | 1,467 | 20,830,493 |
| `ground/turf` | 1,424 | 386 | 6,300,743 | 6,167 | 1,461 | 22,902,104 |

The whole command: **28.8 s and 51.3 MB of PNG at 1,024, 98.1 s and 177.8 MB at 2,048**, twelve
sets, 73 files; run again with nothing changed, 3.3 s and 1.8 s (it reads twelve sidecars). An
earlier run of the same binary at 1,024 beside heavier load took 67 s, which is how far these
move with the machine. The rock kind is the dearest a texel (1.7 to 2.2 microseconds at 2,048):
some eighty lattice hashes in f64 for its crystals, relief, stain, joints and patches; nobody
has tried to make it cheaper, because it is paid once. Laying a floor's leaves walked each
leaf's whole bounding square at first (17.9 s for the thatch at 2,048); walking only the texels
near the leaf's line made it 6.0 s and the same bytes.

**Version 2** (G149 to G153), `engine-content matgen --force` in `msvc-release` at 1,024, every
default and the six variants, 2026-11-11 09:38 on the same shared machine, **not a quiet run**:
the processors read 77 % busy before and 65 % after, 21 GB free. Build ms a set: gravel bed 927
(G148: 873), sand bar 802 (1,276), scree 1,109 (1,552), granite 1,884 (1,818), granite wet 1,864
(2,294), basalt 2,103 (1,696), limestone 2,204 (2,598), sandstone 1,687 (2,554), moss 771, soil
785, thatch 1,108, turf 1,148; each variant within 15 % of its role. No set takes more than 1.25
times what it took (basalt, beside a different load), so none more than twice. The whole command
with eighteen sets: 31 s.

**Versions 4 and 5** (G188, G187), `engine-content matgen --force` in `msvc-release`, every
default, 2026-11-11 20:46 to 20:48 on the shared machine, **not a quiet run**: the processors
read 76 % busy before and 71 % after, 1.3 GB of memory free at the start (seven rows and the
main coordinator's five building beside it), so each figure is an upper bound. The whole command,
twenty sets: 81 s. The six sets at 2,048 texels, build and write ms, PNG bytes:

| Set | build ms | write ms | PNG bytes |
|---|---|---|---|
| `ground/moss` | 7,449 | 6,096 | 19,968,317 |
| `ground/soil` | 6,812 | 1,346 | 21,388,489 |
| `ground/thatch` | 8,279 | 1,457 | 24,082,966 |
| `ground/turf` | 9,275 | 1,311 | 27,488,617 |
| `wood/log` | 3,562 | 616 | 6,477,648 |
| `wood/plank` | 2,899 | 990 | 8,878,660 |

The ground sets at 2,048 cost about what G148's took at 2,048 (5.8 to 6.2 s beside lighter
load), from four times the texels of their old 1,024 default; the turf's leaves are the dearest
part (tens of thousands of fragments laid until their cover is reached). A ground or wood role
now uploads three textures of 2,048 squared — 5.6 MB each in the `.tex` container by
[materials](materials.md#the-first-pack-2026-10-19)' figures, 16.8 MB a role where its 1,024
form cost 4.2 MB; six roles, 101 MB if every one is resident. Nothing scales with the world.

Resident while a set is built: 72 bytes a texel at most (the field's 20, the hollows' three
integer planes, the granular kind's top, id, taken, rise and break planes — 14 — or the rock
kind's patch, owner, reach, source and streak planes — 17 —, or the ground kind's patch, mound,
underlay and two cover planes — 20 —, or the wood kind's phase, gradient and bark planes — 16 —,
the four images' 8) — about 75 MB at 1,024 squared, 302 MB at 2,048 (G148's: 61 bytes) — all
freed when the images are written.

**Owned data.** The recipe type (`schemas/matgen.schema`), the default recipes, `k_matgen_version`.
The pack directory `engine-content matgen` writes is derived data.

**Invariants.**

- A set built from any window is the first window's bytes rotated.
- The grid count over a bed reads the recipe's D50 and sorting; a rock's fresh share and a
  floor's covers are the recipe's (a floor's litter and leaves to within a fragment's area).
- A ground set's contrast at 5 cm and 25 cm and its relief stand over the floors "A floor at
  standing height" names; a wood set's grain runs along u.
- No sixteenth of a tiled granular set stands out from its mean past the bounds "Repetition"
  names.
- Streaks and moss exist only where a recipe asks for them.
- A recipe out of range is refused with the field named, and builds nothing.
- The key moves with any field of the recipe and with `k_matgen_version`.
- The same recipe builds the same bytes on every toolchain.

**Public API.** `include/domain/matgen/matgen.h`: `Set` and its parts (from the schema),
`Window`, `SetImages`, `SetStats`, `read_set`, `validate_set`, `canonical_json`, `set_key`,
`build_set`, `default_sets`, `default_set`, `variant_of`, `k_matgen_version`, `k_tiled_variants`.

**Depends on.** `base`, `containers`, `math`, `hash`, `json`, `schema`, `log`, `texture` (the sRGB
tables; the mip chain in the tests), `matgen_schemas`. No capability.

**Testing.** `tools/dev.ps1 test -Filter matgen`: the rotation test over ten defaults (the wet
granite's streaks and moss among them, and the two wood sets); the seam's step; the grid count over five seeds (the
cover of what is drawn held on the same bed unburied); scree against the bed; a rock's shares
and a floor's covers over three seeds each; every default's mean albedo and roughness inside its
material's range; the landmark measure under its bounds over three seeds for each tiled role,
and the variants the same recipe from other seeds (G153); a rock face's form, streaks and moss
where asked and absent where not (G152); a bed part buried and a talus layers deep (G151); a wood
set's grain along u (the colour's step along a row under 0.7 of its step down a column), its
latewood share, checks, knots and bark where asked and none where not, over three seeds (G188,
5.5 s); each ground role's standing-height measures over their floors and its landmark under its
ceiling, over three seeds (G187, 7.7 s: twelve floors at 256 texels on 2 m tiles); a mip
chain's mean; a recipe read, refused and keyed; the pins, nine roles (about 48 s in `msvc-debug`
on a loaded machine, 2026-11-11 20:34; the longest case the grid count's 8.3 s: it builds each
seed's bed twice, buried and not). `materials`' tests hold
`merge_packs` and `pack_at`'s generated pack; `apps/engine_content/tests/matgen_tests.cpp` runs
the command (two roles built, read back through `pack_at`, built again and skipped, a third role
joining the manifest, the refusals; 1.1 s). Benchmarks: `tools/dev.ps1 bench -Filter 'matgen.*'`.

**Performance notes.** Nothing here is in a frame: a set is built once per recipe by the
command. Measured figures are in "What it measured" above.

## What the sets look like, and what a person would object to

### The ground and wood sets in the engine (G187, G188, 2026-11-11 evening)

Drawn as G149 drew its sets (`engine-view`, `msvc-release`, offscreen, 1,600 by 900, the earth sky
at 15:30 in July at 46.5 degrees north): each set a stripe of one flat ground, the camera at 1.7 m
looking two metres ahead and then fifteen metres along; "before" is the base's sets (built by the
base's own `engine-content`, version 3), "after" version 6, both drawn by this row's
`engine-view` (the renderer did not change). Two passes: version 5's pictures showed the moss's
cushion domes creased at every seam into crazy paving and the boards' weathering patches as a
checker at 15 m, which version 6 removed; its pictures are the ones judged. Beside the reels
r01, r04 and r19 (turf, a grazed alpine sward; r19's grey plank footbridges and silvered logs):

- **Turf** is no longer one flat green: at 1.7 m it shows tufts, dead leaves and a mottle of
  lighter and darker sward, and at 15 m a patchy lawn. Beside r01 and r04 it is still too even
  and too smooth: their turf's dark gaps between tufts and its bright blade tips come from
  geometry (the sward's blades), and a texture alone reads as a mown lawn, not a grazed alpine
  sward.
- **Moss** reads as a carpet of patches, but its patches are hard-edged cells of one tone each
  (a camouflage print at 1.7 m); a moss carpet's tone changes softly across a cushion.
- **Soil** reads as stony bare soil; its small stones are too uniformly pale specks.
- **Thatch** is the weakest: a straw-coloured litter with soft mottles, still close to one flat
  tone at 15 m.
- **Planks** read as weathered grey-brown boards with flat-sawn arches, knots and checks; on a
  ground plane at 15 m the 2 m tile's repeat is faint. Beside r19 the boards are a little too
  brown and too clean: its decks are grey with dark wet stains along their joints.
- **Logs**: the silvered wood and its long checks read; the bark patches read as cracked mud
  more than as bark, and at 15 m they repeat in a lattice on a plane (a log of 4 m shows two).

### In the engine (G149, 2026-11-11)

All twelve drawn by `engine-view` (`msvc-release`, offscreen, 1,600 by 900, the earth sky at
15:30 in July at 46.5 degrees north, no material pack but the generated one) as stripes of one
flat ground, each 16 m wide, from 1.7 m looking at the ground two metres ahead and fifteen metres
along it, and the four rock sets with a rock type on a block of that type from 2.5 m; before (the
G148 generator's pack) and after, from the same `engine-view` and derived-data root. Judged
beside the owner's reels r04 (lichen-splashed granite boulders, orange), r49 (a cobble bed and a
pale gravel bar), r19 (slabby boulders and scree), r03 and r23. Worst first, after the change:

1. **The four ground sets (`ground/turf`, `moss`, `soil`, `thatch`) read as flat colour** at
   1.7 m: felt green, felt brown. They hold their detail at the scale of a blade, which the
   sward's cook reads; as a terrain layer they are no floor. Not changed by this row.
2. **The rock sets on a plane read as a pattern at 15 m**: every set's tile carries the same
   density of joints, colonies and pale fresh breaks, and a flat plane shows it. Two pictures
   of the granite, the wet granite and the limestone were covered in crescent stamps
   (`sheet-near-after.png`, and `sheet-near-after2.png` after the scallops were softened); with
   the broken step removed (`k_matgen_version` 3, `sheet-near-after3.png` and
   `sheet-block-after3.png`) they are gone on the plane and on the block, and what reads in their
   place is speckled rock crossed by joint lines, pale fresh-break blotches and lichen spots.
   The blocks did not go flat: their form above the decimetre is the mesh's. Beside r04 the
   orange colonies are too few and too small, the rock too pale and too evenly speckled, the
   fresh breaks read as pale paint on the wet granite and the limestone, and the joints still
   read as cut lines more than gaps.
3. **`rock/basalt`** reads as dark slabs with polygonal joints, but its lichen reads as confetti:
   white and orange discs bright on near-black rock.
4. **`sand/bar`** reads as dry sand with a scatter of granules, flat: no ripples, no wet margin
   (one set is one state).
5. **`rock/scree`** — before, flat plates on a dark floor; after, a talus of angular blocks at
   three depths with small dark voids and a few pockets of fines. Beside r19 the blocks are all
   one pale tone (no weathered or lichened faces), their tops still mostly planes, and the fines
   pockets read as flat beige pools.
6. **`gravel/bed`** — before, polished pebbles in plain rows to the horizon; after, a bed whose
   rows are gone at fifteen metres, its stones packed and part buried in a grit, some broken,
   some filmed. Beside r49 the stones are too glossy (a wet bed's roughness under air) and the
   film too faint to read as the reel's olive-brown.

### Before the engine (G148, 2026-11-10)

Judged 2026-11-10 from the images themselves (base colour and normal at 1,024 squared), beside
the owner's reference reels r03 and the cobble riffle's motion sheet (a clear river over
cobbles, a pale gravel bar), r19 (a torrent among grey boulders under scree), r01 (meadow
floors) and r23 (dark wet rock under moss). **Not judged in the engine**: the night's GPU lock
was never had, so no set was seen on a mesh, under the sky, or at a distance; that is the first
thing to do with them. The nearest thing looked at was a CPU preview — the base colour lit
through the set's own normal map by one raking light, straight down, two tiles by two — in
which the gravel reads as a pebble bed with no seam, its largest cobbles plainly repeating
every metre, and the scree as crushed angular stone lying on a dark floor rather than a deep
talus: its gaps are too wide and too flat, and the lower layer of blocks is lost in them. The
bar is a photograph, and none of these is one:

- *`gravel/bed`* reads as a bed of rounded pebbles and small cobbles of several rocks, shingled
  down the flow, some crossed by a vein. Beside r03: every clast is a clean oval of one tone —
  no banding, pitting, weathering rings or broken faces; the reel's bed is olive-brown under
  algae and silt where this is neutral grey; nothing is larger than 18 cm or half buried; the
  sand between is uniform.
- *`rock/scree`* reads as angular blocks over dark gaps with more blocks below. Beside r19:
  each block is a convex polygon with one ridge, where talus is slabs and prisms with fracture
  faces; the gaps are flat dark pools; there are no fines in pockets and no lichen on old
  faces.
- *`rock/granite`* reads as a speckled granite at arm's length. The lichen thalli read as
  stamped patches of too even a colour, the fresh breaks as pale paint, the joints as pen
  lines; there is no run-off staining and no form above the decimetre.
- *`rock/granite_wet`* is the same face darker and smoother, evenly: no sheen that varies, no
  drips, none of r23's moss in the joints.
- *`ground/turf`* reads as mown turf from above. Beside r01 the leaves are straight needles of
  one width with no clumps, curl, seed heads or forbs.
- `sand/bar`, `rock/basalt`, `rock/limestone`, `rock/sandstone`, `ground/soil`, `ground/moss`
  and `ground/thatch` were built and measured and not looked at closely.

## Not yet

- **Built on demand.** A scene read that finds a role missing could call `build_set` through a
  registration point and the derived-data cache, as a tile's mesh is built; it needs the hosts'
  lookup (`systems/renderer/src/scene.cpp`, `domain/content_build`), which this row did not edit.
- **`rock/scree` has no caller.** The rocks capability resolves scree and deposits by the
  rock's type (`rock/granite`...), so a scree mesh takes the rock face's set; the set here is for
  a ground layer or a recipe that names it until the deposits ask for a granular role.
- **Lithology from the formation.** The bed's mix and the scree's rock are the recipe's, not the
  scene's formation's; a recipe per catchment is the hook.
- **Height is carried and unread** by generated meshes ([materials](materials.md), "Not yet"); a
  bed of cobbles wants parallax or displacement at one metre.
- **Roughness is one channel in a BC7 slot**, as every pack's.
- **Fall sorting, ripples, a wet margin**: one set is one state; a bar's ripples and the sorting
  down a cone are the placement's or a later kind's.
- **The openwork lower layers** are the same blocks laid again, a layer at a time; a talus's
  blocks wedged at every depth would be a placement of their own.
- **Picking a variant by place.** The variants exist; no consumer picks one yet (the terrain's
  layers, a channel's bed and the hosts' lookup are other rows' files): a request under the
  row's hand-back. A placement of whole clasts in the resolve, or several sets by a mask, are the
  other cures, neither this module's.
- **Streaks run down the image.** A rock set laid on a mesh by triplanar or with a rotation has
  no idea which way is down; a streak points the way the image's -V points on that face.
- **A floor's tone across a cushion** (G187): a moss cushion and a sward's patch take one tone
  a cell, so their edges are hard; a tone that falls off across the cell, or two overlapping
  cells blended, is the next step. The thatch needs structure of its own above the decimetre
  (lodged swathes, tussock bases) to stop reading as one tone at 15 m.
- **A grazed sward's dark gaps and lit tips** are the sward's blades' to draw (flora's), not a
  texture's: the turf set is the floor under them and turf where none stand.
- **Wood the way a member is mapped** (G188): the sets run their grain along u; a consumer that
  maps u from a world axis runs it across a member that lies along the other one. A log's bark
  is a patch on a flat image; a log mesh with its own bark (and a bark set) would carry it
  better. The deck's wet stains along its joints and between boards belong to the consumer's
  mesh (per-board placement), not the set.
- **Ground and wood variants**: only the tiled granular roles have them (G153).
- The numbers marked not verified on the grounding page.

## Capability contract (ADR-0027)

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | none | stands alone |
| Schema types | `domain/matgen/schemas/matgen.schema` | the recipe |
| Tick scheduler entry | none | nothing runs in a tick |
| Render-graph passes | none | a set reaches the renderer as a pack's images |
| Content-build derived step | `engine-content matgen`, keyed by `set_key` | a pack directory |
| Protocol methods | none | |
| Tunables | none | every parameter is the recipe's, and in the key |
| LOD policy | the texture build's mip chain; the far form is the set's mean | held by a test |
| Determinism | derived; the same bytes from every toolchain | pinned |
| Zero cost unused | no registrar, no static state | |
