# A tree's base on sloping ground: seated, flared and rooted

G201, 2026-11-12. Grounding and design, pages only; the build is its own row and starts from
[the stages](#stages). It follows [surface roots](tree-surface-roots.md) (G92, G102), whose
reading it uses and whose ridge and one-mesh-a-tree companion it proposes to replace.

**The design in one sentence.** Every tree mesh carries its stem down below its collar as a skirt,
so no instance can show a gap, and carries its flare and a few rounded laterals as a **foot** that
the per-frame deformed-vertex pool shears to the ground's plane from two numbers an instance, so
thirteen thousand trees have roots from sixty meshes, in the trunk's own surface, with no mesh and
no traced structure of their own.

## Why

The owner walked the forest proof of concept on 2026-11-12: trees on sloped ground "have a gap
between the ground and the trunk on one side ... they need to be moved down a bit and/or have roots
added. roots add a lot to the realism of the tree."

The forest's authors measured it (their `reports/quality-v42/parts/trees/README.md`, read as text):

| What | Number |
|---|---|
| Trees, adult meshes | 74,503 instances of 60 meshes |
| Gap over 3 cm under the downhill rim | 8,880 (4,482 over 10 cm; worst 1.33 m); 57% of trees on 30 to 35° ground, 76% on 35 to 40°, under 1% below 10° |
| The base ring | horizontal, 1 to 7 cm above the origin, radius 1.28 to 1.53 times the core radius (cedar 1.86, a birch clump 2.1): 0.39 m (`pine-0`), 0.65 m (`spruce-old`), 0.98 m (`pine-old`), 1.14 m (`oak-old`) |
| Their workaround, seating | 14,458 trees lowered until the downhill rim is 2 cm under the ground: median 6.7 cm, 95th percentile 45 cm, worst 1.35 m; the uphill flare is buried by about twice the rim's drop |
| Their workaround, roots | one unique companion a tree (G92, G102): 3,216 to 6,528 source triangles, 260 to 370 KB of container, one traced structure set each; 2,000 placed (4,207 with the second tier) of 13,785 trees that deserve one |
| How the companions read | a tree that grows out of the slope at 9 to 16 m; at 2 to 4 m knife-edged fins up to 1.2 m tall (G102's ridge), flat-shaded at 8 sides, and a pale band at the join |
| The traced chain on the owner's walk (their item 54) | wanted 376,309 cluster structures, built 338,072 |

The gap is geometry: a `ground: true` instance stands on the ground at its origin
([renderer](../subsystems/renderer.md), "A scene file's instance with `ground: true`"), the base is
a horizontal ring of radius *r*, so on a slope of angle θ the downhill rim stands *r* tan θ above
the ground: 0.26, 0.57 and 0.98 m under `pine-old` at 15, 30 and 45°.

## Grounding

### The sources, and how far each was read

Read on 2026-11-12 unless it says otherwise. This machine has no PDF reader, so a source that
exists only as a PDF was not read, and is listed as such.

| Source | How far read |
|---|---|
| Perry (1982), [*The ecology of tree roots and the practical significance thereof*](https://auf.isa-arbor.com/content/8/8/197), J. Arboriculture 8(8): 197-211 | the whole text as the journal's page gives it; figures as captions only |
| Dumroese, Terzaghi, Chiatante, Scippa, Lasserre and Montagnoli (2019), [*Functional traits of Pinus ponderosa coarse roots in response to slope conditions*](https://www.frontiersin.org/journals/plant-science/articles/10.3389/fpls.2019.00947/full), Frontiers in Plant Science 10: 947 | the article's body, to character 100,000 of 121,224 (what was left is mostly its reference list); its tables through the page's text, where one header was ambiguous |
| Lee (2022), [*Quantitative tools for the prediction of pavement damages associated with urban trees*](https://auf.isa-arbor.com/content/48/4/217), Arboriculture and Urban Forestry 48(4): 217-232 | the whole text; its tables did not render, so no per-species coefficient but the ones the text names |
| Nicoll and Ray (1996), [*Adaptive growth of tree root systems in response to wind action and site conditions*](https://doi.org/10.1093/treephys/16.11-12.891), Tree Physiology 16: 891-898 | the abstract today; pp. 893-895 as G102's review read them from the author's text ([surface roots](tree-surface-roots.md)) |
| Di Iorio, Lasserre, Scippa and Chiatante (2005), [*Root system architecture of Quercus pubescens trees growing on different sloping conditions*](https://doi.org/10.1093/aob/mci033), Annals of Botany 95: 351-361 | the abstract only (the publisher's page); the PubMed Central copy refused the reader |
| Chiatante, Scippa, Di Iorio and Sarnataro (2003), [*The influence of steep slopes on root system development*](https://doi.org/10.1007/s00344-003-0012-0), J. Plant Growth Regulation 21: 247-260 | not opened; two sentences of its abstract as a search returned them from the AGRIS record |
| Nicoll, Berthier, Achim, Gouskou, Danjon and van Beek (2006), [*The architecture of Picea sitchensis structural root systems on horizontal and sloping terrain*](https://doi.org/10.1007/s00468-006-0085-z), Trees 20 | not opened (the publisher's page asks for a login); two sentences of its abstract as a search returned them |
| Nicoll, Achim, Mochan and Gardiner (2005), *Does steep terrain influence tree stability? A field investigation*, Canadian J. Forest Research 35: 2360-2367 | not opened; a search's summary of the first author's thesis |
| USDA Forest Service silvics, [eastern white pine](https://research.fs.usda.gov/silvics/eastern-white-pine) and [red spruce](https://research.fs.usda.gov/silvics/red-spruce) | the pages' passages on roots |
| Crook, Ennos and Banks (1997); Coutts, Nielsen and Nicoll (1999); Danjon, Fourcaud and Bert (2005) | not opened today; as G102's review read them ([surface roots](tree-surface-roots.md)) |
| Phipps (1974), *The soil creep-curved tree fallacy*, J. Research U.S. Geological Survey | not opened (the survey's page refused the reader); its abstract as a search summarised it |
| LaMarche (1968), *Rates of slope degradation as determined from botanical evidence, White Mountains, California*, U.S. Geological Survey Professional Paper 352-I | not opened (refused; a PDF); a fragment of its abstract through a search |
| Stoffel, Corona, Ballesteros-Canovas and Bodoque (2013), *Dating and quantification of erosion processes based on exposed roots*, Earth-Science Reviews 123: 18-34, and the erosion-rate studies it reviews | not opened; rates as a search returned them from the abstracts |
| Lamb, Scheingross, Amidon, Swanson and Limaye (2011), *A model for fire-induced sediment yield by dry ravel in steep landscapes*, J. Geophysical Research 116: F03006 | not opened (a PDF, and the publisher refused); its abstract through a search |
| Hoffman (2013), a research report to Boulder County on soil mounds beside trees | fetched and unreadable here (a PDF); one sentence through a search |
| Raile (1982), *Estimating stump volume*, USDA Forest Service Research Paper NC-224 | not read: a scanned PDF. It holds the butt-swell taper by species that would pin the flare's height |
| North et al. (2015) and Hilbert et al. (2020), trunk flare diameter against DBH, Urban Forestry and Urban Greening | not opened; their coefficients as Lee (2022) reports them |
| A study of the bark of *Quercus robur* roots (Trees; found by a search, its citation not confirmed) | not opened; two sentences through the search |

### How a tree meets level ground

| What | The range | Source |
|---|---|---|
| Major woody roots from the root collar | 4 to 11, growing horizontally; "usually three to five large roots" on eastern white pine, with "just a vestige of a taproot" | Perry 1982; USFS silvics |
| Where they attach | at or near the groundline, where the trunk shows a marked swelling, the butt swell | Perry 1982 |
| How far they stay structural | they branch and taper over 1 to 4 m from the trunk into rope-like roots 10 to 25 mm thick; the zone of rapid taper is within 2.2 times the DBH of the stem's centre (the radius Dumroese et al. use as the "root cage") | Perry 1982; Danjon et al. 2005 as G102 read it; Dumroese et al. 2019 |
| How deep they lie | framework roots usually less than 20 to 30 cm under the surface; red spruce's average rooting depth in Maine 33 cm, 56 cm at most | Perry 1982; USFS silvics |
| The flare's width at the ground | trunk flare diameter rises 1.3 to 1.9 m per metre of DBH for ordinary street trees measured at the stem-to-root transition; up to 4.5 (*Delonix regia*) when the tips of protruding roots and plate buttresses are measured; mean flare over mean DBH is 3.3 and 3.8 for two *Ficus* (this page's division of the paper's means) | North 2015 and Hilbert 2020 as reported by Lee 2022; Lee 2022 |
| A lateral's section near the stem | taller than wide by growth on its upper side: the vertical distance from the biological centre to the top over that to the bottom averages 4.24, 3.58, 2.60 and 1.20 at 0.5, 0.75, 1.0 and 1.25 m from the stem's centre on 46-year-old Sitka spruce (**not** a height over width); T-beam shapes on the leeward side, most at 0.5 m on shallow-rooted trees, I-beams farther out and to windward, ovoid beyond | Nicoll and Ray 1996 (abstract today, the ratios from G102's reading) |
| Height over width of a lateral | 1.07 without buttresses, 7.9 for tropical plate buttresses | Crook et al. 1997 as G102 read it |
| An I-beam's worth | nearly 300 times the stiffness of a round root of the same area | Nicoll et al. 2006 as Dumroese et al. cite it |

**What follows for the shape.** A temperate tree's base is a swelling of the stem that divides into
a handful of laterals, each **round to a little taller than wide**, with its extra wood on top for
its first metre, running out just under or at the surface and gone to rope within two DBH or so. A
plate buttress (height over width near 8) is a tropical form; nothing read supports one on a pine or
a spruce, which is why G102's ridge ("2 basal radii of added height at the hidden origin", 1.2 m on
`pine-old`) reads as a fin. The engine's bottom ring, 1.28 to 1.53 times the core radius, sits at
the low end of the 1.3 to 1.9 band and is kept.

**Not found with numbers**: the flare's height against the stem's diameter (Raile 1982 has it and
could not be read; the forestry convention of measuring diameter at breast height because it is
above the butt swell bounds it at about 1.3 m on an ordinary tree, which is an inference and not a
measurement); a beech's fluting and an oak's flare as dimensions; what the bark does across the
collar, beyond one unopened oak study a search returned (a root's bark is built very like the
stem's, and is 1 to 2 mm thick on a lateral), which if it holds says the foot wears the trunk's
bark at a finer relief. The page pins nothing on those and asks
[the next reels](#what-to-ask-of-the-next-reels).

### How a tree meets a slope

| What | The finding | Source |
|---|---|---|
| The root system's plane | "Roots grow parallel to the surface of the soil so that trees on slopes have sloping root systems" | Perry 1982 |
| Asymmetry, oak | five mature *Quercus pubescens* on a steep slope and five on a shallow one, excavated to 1 cm roots: slope changed the number, length and basal cross-sectional area of first-order roots; first- and second-order laterals cluster **up-slope**; the laterals and not the taproot give much of the anchorage | Di Iorio et al. 2005 (abstract; its slope angles were not in what was read) |
| Asymmetry, pine | five 32-year-old *Pinus ponderosa* (DBH 23 to 34 cm, about 16 m tall) on a slope of 30 to 50% (17 to 27°): in the top 30 cm first- and second-order root length and volume are greater in the **down-slope** quadrant; I-beam and T-beam sections form there at the branching from the taproot, and "most roots (83%) in the down-slope quadrant displayed T-beam development"; down-slope roots are read as loaded in compression, up-slope in tension; no difference between quadrants at 30 to 60 cm | Dumroese et al. 2019 |
| Asymmetry, in general | laterals emerge and elongate preferentially up-slope and down-slope | Chiatante et al. 2003 (two sentences, not opened) |
| Spruce | 24 forty-year-old Sitka spruce on about 30° and on the flat beside it: most non-directional characteristics of the root systems alike, and no overall effect of the terrain on anchorage | Nicoll et al. 2006 and 2005 (not opened) |
| Exposure of roots by erosion | roots form in the uppermost foot of soil and are progressively uncovered as a slope degrades; lowering of the surface dated from exposed roots runs 1.1 to 2.6 mm a year beside trails under Scots pine, 6 to 9 mm a year in gullies and marls, 2.6 to 16.5 mm a year rising with steepness in one study | LaMarche 1968; the studies Stoffel et al. 2013 review (abstracts through a search, none opened) |
| Soil held up-slope of a stem | vegetation stores sediment behind it on hillslopes steeper than the angle of repose, and releases it when it burns | Lamb et al. 2011 (abstract through a search) |
| The curved butt | work in eastern deciduous forest found no support for reading curved trunks, tilted trunks or up-slope trailing roots as signs of soil creep; curvature is the stem's own response to gravity and light after it has been tilted | Phipps 1974 (abstract through a search) |

**What follows.** The stem stays vertical and the root plate lies in the slope: the collar is not a
horizontal ring but the stem's section by a plane near the ground's. The strong laterals run along
the fall line, which side more by species (up-slope on the oak, down-slope on the pine), and their
reinforced sections sit in the top 30 cm, where they are seen. Erosion at millimetres a year
uncovers roots: at the trail-side rates, 5 to 13 cm in fifty years (those rates are from worn and
gullied ground; none was found for an undisturbed forest floor), while the up-slope side collects
what moves down the slope. **No number was found for the height of
the soil banked up-slope, for the exposed length of a down-slope lateral by slope angle, or for a
slope angle at which a sweep appears.** A basal sweep is not a property of sloping ground: it is a
tilted tree's recovery (a bank that failed, snow, a neighbour's fall), so the design leaves it to a
recipe's lean and does not drive it from the slope.

### What the eye reads at 2 m, 10 m and 50 m

Six of the owner's first-set sheets show trees near enough to judge
(`inspiration/_frames/sheets/`, opened: r08, r09, r37, r40, r43, r52); the second set's shots are
shores and streams and none shows a forest floor. **None is a close study of a root collar on a
slope**, as the forest's authors also found.

| Sheet | Distance | What is seen at the base |
|---|---|---|
| r52, ponderosa pines beside a trail on gently sloping ground | 2 to 3 m (the trunk at the frame's left), 5 to 15 m (the rest) | the nearest trunk broadens over its lowest half metre and goes into orange needle litter banked against it; the lower trunk is darker than the bole above; at 5 to 15 m the stems enter the litter with a slight swell and no separate root, their feet half hidden by tufts and small shrubs; nowhere a dark line under a trunk |
| r09, conifers on a steep bouldery river bank | 5 to 15 m | the stems leave the bank leaning out and curve up (a basal sweep on a bank, not on a hillside); the join is among boulders and litter |
| r08, roadside pines at dusk | 10 to 30 m | bases merge into understory and litter; the silhouette is continuous to the ground and no flare is readable |
| r40, dead trunks and a gnarled tree on gentle rocky ground | 3 to 20 m | feet hidden in scrub and bunch grass or behind rocks |
| r37, two trunks at arm's length, a forested slope across a lake | 1 to 2 m (feet out of frame), over 300 m | at distance only that every stem is vertical on a steep slope |
| r43, a cutbank beside a river | 20 to 40 m | a root mat at the bank's lip; no tree's base resolved |

With the arithmetic of a 3840 x 2160 frame and a 60° vertical field (about 2,060 pixels a radian;
the field is this page's assumption):

| | 2 m | 10 m | 50 m |
|---|---|---|---|
| A 0.3 m gap under the rim | 309 px | 62 px | 12 px: a dark slit under a trunk 40 px wide |
| A lateral 0.15 m thick | 155 px: its section, its bark and its join are all read | 31 px: read as a root, its section not | 6 px: not read as a shape |
| A flare 0.2 m proud of the stem | 206 px | 41 px | 8 px |

So **the gap matters at every distance and is the first stage**; the flare's lean into the slope
is read to 10 m and a little beyond; roots are a 2 to 15 m matter, and at 2 m everything about them
is judged, which is where the companion fails today. What the reels add to the literature is the
ground: a real foot is half hidden, in litter banked against it, and darker than the stem above.

## Design

The constraints are the engine's: a tree is an instance of a shared mesh in the visibility buffer
and in the traced structures; a rigid instance's cluster structures are shared with every other
instance of its mesh ([renderer](../subsystems/renderer.md#static-structures-for-rigid-forms), and
the resident cache after it), while a swaying tree's vertices already pass through the per-frame
[deformed-vertex pool](../subsystems/renderer.md#the-deformed-vertex-pool), one pose per part, and
its structures are built from the pool each frame; every instance of a unique mesh is its own run
of pairs and its own structures ([the chain's memory](../subsystems/renderer.md#the-ray-tracing-chains-memory)).

### 1. Seating

**Chosen: both, each for what the other cannot do. The mesh's skirt closes the gap; the
placement's footprint handles what a skirt cannot reach and gives the slope its two numbers.**

**The skirt (the flora solver; nothing per instance).** The trunk's tube is carried on below its
bottom ring: the flare's own curve for a quarter of the rim radius *r* (the radius grows by a tenth
at most), then straight down to a depth

    D = 1.1 r tan 45° + 0.15 m = 1.1 r + 0.15 m

which is 0.58 m on `pine-0`, 0.87 m on `spruce-old`, 1.23 m on `pine-old` and 1.40 m on `oak-old`.
On a plane of up to 45° through the origin the ground meets the skirt above its lowest ring at
every azimuth, so **no yaw and no slope to 45° shows a gap**; the 0.15 m is a margin for ground
that curves away under the rim, and what it does not cover is the footprint's (below). The
megaflora already does this (`k_sink_m` = 1 m, "so the flare meets any ground"); the branching
species do not (their bottom ring stands 1 to 7 cm above the origin, by the forest's measure), and
the palm was not checked. Open at the bottom, bark UVs continued (*v* below zero at the trunk's
own metres), bend weight zero. Cost: two rings, **four triangles a side: 64 on a 16-sided trunk, on
each of 60 meshes**, in every rung and every distance form; nothing per instance, no schema, and
every placement path (authored, scattered, streamed, placed by hand) gets it. Against the
workaround: the tree keeps its height, and the uphill flare is buried by *r* tan θ, not twice that.
It moves every tree's bytes (the mesh's box grows downward, so its 16-bit grid moves): the solver's
version is bumped and the pins re-taken, once.

**Seating by footprint (the host, at load and at tile activation).** The flora cook records a
**seat** in the container's generator section, where `engine-content info` prints a recipe's
canonical parameters: the rim's radius and height, the skirt's depth, the foot's height and reach.
For an instance whose mesh has a seat, the host samples the drawn ground (cuts, fills, benches and
planned edits included: it is the ground the instance already stands on) at the centre and at 16
points on the rim's circle, in whole millimetres, in fixed world directions so the answer does not
depend on the yaw, and

- lowers the instance by `max(0, drop - (D - 0.05 m))`, never raising it, where `drop` is the
  centre's height less the lowest sample: zero for nearly every tree, and the remedy for a rim that
  hangs over a tread's cut, a bank or the heightfield's edge (the forest's worst case, 1.35 m);
- fits a plane to the 16 samples by sums and products against a table of constants (no C library,
  no division but by constants) and keeps its gradient, quantised to two 16-bit words: **the two
  numbers an instance** that the flare reads. Once a mesh has laterals (stage 4) the fit takes a
  second circle at the foot's reach, 16 more queries, so the plane is the one the roots lie on and
  not only the rim's.

A mesh with no seat (a glTF prop) stands as it does today; a scatter kind's `sink` stays the
author's and the larger of the two applies. Cost: 17 ground queries for a seated instance where it
makes one today, once, off the frame's thread; 4 bytes a placement. A function of the instance and
the ground, so the whole read and a streamed tile agree and no order of activation moves it.

**`engine-content ground sample <scene.json> --points <file.jsonl>`** (or `--point x_mm,z_mm[,radius_mm]`),
for authors who place by hand: one JSON line a point, from the scene reader's own ground after its
planning pass, so it is the height a `ground: true` instance stands on, with the cuts, the benches
and the planned edits the forest's authors could not see: `y_m` (printed to round-trip), `hole`,
`excluded` (which of holes, channels, water and footprints cover the point), the gradient
`gx`, `gz`; and with a radius the disc's `min_m`, `max_m`, `drop_m`, the fitted plane's `gx`, `gz`
and largest residual, and `seat_m` for a given `--skirt`. `--uncut` asks the ground before its
cuts. A head line states the finest drawn spacing, which bounds the difference between this
bilinear answer and the drawn triangles.

### 2. A slope-aware flare

**Chosen: conform in the pool. The foot is a part of the tree's own mesh, sheared to the ground's
plane by its pose.**

**The foot.** The stem from the skirt's bottom to a height *h* = min(3 *r*, 0.3 of the trunk)
becomes a motion part of its own, the **foot**; the trunk's part begins at the foot's top ring and
bends about it (the flare is the stiff part of a stem). The two share that ring's positions, each
at rest there, so the join is watertight by construction. For an instance on a slope the foot's
vertices move vertically:

    y' = y + w(y) (g . xz),      w(y) = 1 - smoothstep(0, h, y),   w = 1 below the collar

where *g* is the placement's gradient times a **conformance** *c* = 0.8. At *c* = 1 the collar lies
in the ground's plane (Perry's sloping root system); under 1 the collar plane is flatter than the
ground, so at a distance ρ from the axis the foot stands (1 - *c*) ρ tan θ **out of the ground
down-slope and that much into it up-slope**: 0.11 m at the rim of `pine-old` on 30°, 0.25 m at
2.2 m. That is the exposure and the banking in one number, of the order the trail-side erosion
rates give in fifty years (5 to 13 cm); *c* itself is authored, since no source read gives the banked height or the
exposed length by slope, and it is the control the forest's authors found missing (their exposure
of 0.65, 0.80 and 0.92 changed nothing at the stem).

**Where it runs.** `gfx::FloraPartPose` (80 bytes, unchanged) already has a flag in `axis.w`: 0 is
"at rest, written through to the bit", 1 a bend. A third value marks a foot, whose pose carries *g*
and *h* in the words a bend uses for its pivot and angle. The flora stage's foot path is a dot product, a smoothstep and
a multiply-add where a bent vertex costs a sine, a cosine and two matrix products, and the
vertices it takes were the trunk's lowest rings, so **the stage gets no dearer**. The normal is
exact and closed: with *a* = (*w* g_x, *w'*(y) (g . xz), *w* g_z), the normal is
((1 + a_y) n_x - a_x n_y, n_y, (1 + a_y) n_z - a_z n_y), normalised; it goes where the bend's
turned normal goes.

**What it costs, against the alternative.**

| | Conform in the pool (chosen) | Base meshes by slope class, turned to the fall line |
|---|---|---|
| Triangles | none added | a collar and roots again per class: 4 classes x 60 meshes of 1,500 to 3,000 |
| Bytes | one part a mesh; one more 80-byte pose a swaying tree a frame (it already has one for each of its parts); 4 bytes a placement | 240 meshes of 100 to 300 KB (24 to 72 MB), resident as a kit; a second instance (96 bytes and its pairs) per near tree |
| Traced structures | none added: a swaying tree's clusters are built from the pool each frame already | cached and shared (about 6,000 slots at the driver's most, 37 to 53 MB), a reference per instance |
| Frame time | under the stage's noise by construction (above); to be measured as a pair | a second instance's culling and drawing per near tree |
| The fit | the actual plane under the tree | within 5° of it: 0.18 m of misfit at 2 m on 30 to 35°, to be buried |
| The join | none: it is the trunk's own tube | an overlapping sleeve at any yaw, a second surface |

- **The tie rule and the ray path** are untouched: an id is (pair, triangle), and every reader of a
  position (three rasterizers, the resolve, the cluster structures) takes it from the pool, so the
  rays still trace the rasterized cut word for word.
- **The LOD cut.** The cull tests rest-pose spheres padded by one number an instance
  (`flora_bounds_padding`); the foot's largest displacement, reach times |*g*|, joins it as a
  maximum (a vertex bends or conforms, never both): 0.9 m on 30° with a 2 m reach, where the sway
  already pads the test's young palm by 1.77 m. A shear of gradient *t* stretches lengths by at most
  (*t* + sqrt(*t*² + 4)) / 2, 1.33 at 30° and 1.62 at 45°, so the cook multiplies the foot's
  cluster errors by 1.62; the foot's cut is a little finer and the rest of the tree's is not
  touched. Within the foot the bark stretches vertically by up to 1.5 *r* tan θ / *h*: 13% at 15°,
  29% at 30°, 50% at 45°.
- **Determinism.** The two numbers are integer-millimetre samples through sums and products, the
  same bits on every toolchain; the shear is the device's float arithmetic, as the sway is. A tree
  on level ground has *g* = 0 and is written through at rest, to the bit.
- **What a walker meets** does not change: a tree collides by its trunk, and the foot is drawn,
  not walked on, until a later row says a root should trip or carry someone.
- **Who is not conformed.** A rigid instance has no pool block: a far ring's distance form (a
  static structure shared by its mesh), a tree with `renderer.flora.motion` off, a snag. It shows
  the skirt, which is why the skirt is stage 1 and the conform cannot replace it. The conform's
  amplitude is faded in over the outer part of the posed ring by the pose the CPU writes, so the
  hand-over from a form moves a flare by a few pixels over several metres and not in one frame.
  Giving a rigid thing a pose only to conform it takes it out of the shared structures and is not
  proposed.

**Something else, weighed: roots made by the placement at tile activation**, one merged mesh a
tile, fitted to the real ground (R142's tile meshes). It is the only form that follows rough
ground, a ledge or a bank exactly, and it is rigid and shared within the tile. It is a separate
surface with a join again, its bytes grow with the world (13,785 sites at even 100 KB are 1.4 GB
of derived data), and the root solver takes about a second a site today against a tile's
activation. Kept for hero ground, after the stages below, on the rounded section they define.

### 3. Roots that are shared

**Chosen: the laterals are part of the foot, on the tree's own mesh.** Every instance of the mesh
has them, the conform turns them into the slope, and nothing is unique to a tree.

**Their shape, by habit.** Authored inside the bands the grounding gives; the columns say which.

| Habit | Laterals (Perry: 4 to 11) | Height over width at the stem (1.07 unbuttressed; extra wood on top) | Run at the surface on level ground (structural within 2.2 DBH) |
|---|---|---|---|
| pine | 3 to 5 (USFS: "usually three to five large roots") | 1.0 to 1.2 | 0.8 to 1.5 DBH, low and rounded |
| spruce | 4 to 8 on a shallow plate (rooting depth 33 cm) | 1.0 to 1.3 | 1.5 to 2.2 DBH, tops at the surface |
| beech, cedar-like | 5 to 9, continuing the stem's flutes | up to 1.5 | 1.5 to 2.2 DBH |
| oak and the other broadleaves | 4 to 7 | 1.0 to 1.3 | 1.0 to 1.5 DBH |

A lateral leaves the foot between two flutes at 0.33 to 0.45 of the core radius (G92's authored
figure, kept), its section round with its centre lifted so the extra wood is on top (the T-beam,
drawn as a rounded shoulder and never a plate), ovoid by a metre out, tapering to nothing as it
dives. Its underside is carried down like the skirt, so no daylight shows under a root that the
conform lifts. G102's ridge and its fitted sleeve go when this lands: the foot is what they
approximated. **Budget: 600 to 1,500 triangles a mesh at the finest rung** (10 to 12 sides near the
stem, 6 at the dive, a ring every 0.15 to 0.25 m), against 3,216 to 6,528 a companion.

**The fall line.** The mesh's laterals stand at seeded, uneven azimuths and the instance's yaw is
free, so whichever point down-slope are the ones the conform exposes. A recipe may also put its
strongest pair on its ±z, and a scatter kind's existing `yaw: Uphill` then lays them along the fall
line, which is what the oak and pine studies describe; optional, since it also turns the crown.

**LOD, and where they stop.** They are the tree's own clusters, so the DAG coarsens them with the
tree. The cook marks the laterals' clusters as **near detail**, and the extraction of a distance
form leaves marked clusters out, so beyond the posed ring no root is drawn, traced or resident:
the "draw distance" is the ring the tree stops swaying in, at no bytes an instance. The forest's
item 60 (a draw distance a scene can state for an instance) is still wanted for everything that
stays a separate asset; one reach on the scene's mesh-table entry, tested in the cull against the
instance's distance, gives it without growing `gfx::InstanceDesc`, and belongs to the detail-tiers
row.

**Against the forest's numbers.**

| | v42: unique companions | The shared foot |
|---|---|---|
| Trees with roots | 2,000 (4,207 with tier 2) of 13,785 | every tree of a mesh whose stem is past the solver's size blend: all 13,785, and those on gentler ground |
| Meshes | one a tree | none added to the 60 |
| Derived data | 0.6 GB (1.3 GB with tier 2), 253 MB of recipes | 30 to 70 KB a mesh at the companion's 46 bytes a source triangle (244,384 bytes for 5,312): under 5 MB for the 60 |
| Triangles a tree | 3,216 to 6,528, unique | 600 to 1,500, shared |
| Traced structures | every companion's clusters in view, at any distance (98 to 126 clusters over a companion's two levels) | at most about 12 a tree at the finest cut, fewer with distance, none past the posed ring |

The chain on the owner's walk is short by 38,237 structures already (wanted 376,309, built
338,072). The foot does not fix that, and it adds to the demand only what near trees draw of their
roots: an estimate of 2 to 12 structures a tree within the posed ring, which the build row must
replace with a count from the test scene and one forest pair. The 2,000 companions leave the
budget when the foot replaces them.

### 4. One surface

The pale band is two assets: the companion has the trunk's bark images but not its tube, its UV
frame, or whatever grows on the trunk. With the foot the question mostly goes away, because **the
skirt, the flare and the laterals are the trunk's own primitive**: one material, the tube's *u*
round the stem and *v* in metres continued below the collar, the laterals mapped at the same
metres a texel.

Two things remain and neither exists yet. **What darkens and mosses the trunk must reach the
foot**: moss and lichen are the [cover](../subsystems/cover.md) capability's, grown on a host mesh
by exposure ("crevices and bases, upward faces" favour moss), so a host that includes the foot
gets moss on root tops by the rule it already has; but a cover on a conformed foot must ride the
foot's part as anything on a swaying host rides its part, and whether a tree's cover carries the
motion contract today was not established by this row. **The darker lowest metre** (damp, soil
splash, the litter line the r52 sheet shows) needs a term on the bark material keyed to height
above the collar, which the tube's *v* already is; the material has no such term.

### 5. The ground round the base

| What | Through | Exists? |
|---|---|---|
| Grass, litter and scatter stay off the collar and the exposed laterals | the placement hands shapes with its tile ([`Placements::shapes`](../subsystems/scene_gen.md#the-shapes-a-tile-occupies)): a disc at the collar and a band a lateral ([footprints as shapes](../subsystems/scene_gen.md#footprints-as-shapes)), from the laterals' lines the cook records in the seat, turned and scaled by the placement and lengthened down-slope by the conform; the exclusion holds them and every generator already asks it | the hand-over and the exclusion exist; the seat's lines and the scatter's hand-over of them do not. 80 bytes a shape; only laterals exposed for more than half a metre get a band |
| Litter banked against the up-slope side | the litter generator reading a shape's up-slope half as a density to raise | no |
| A bare, needle or mossy apron | a mask region written from placements | no: a mask is cooked from the scene, a streamed placement is not known to it, and a ground plan has no list for mask regions yet ([scene_gen](../subsystems/scene_gen.md#what-an-entry-plans-for-the-ground), "Not built") |
| Soil banked up-slope as ground | a ground edit a tree | declined: 10 to 25 cm over a metre is under what a 0.25 m lattice holds, and tens of thousands of edits a world is not what the edit list is for. The conform buries the up-slope foot instead, and the litter does the banking the eye reads |

The sward's patch already reaches half its diagonal past a shape's edge, so blades lean over a
root's flank and do not grow through it.

## Stages

The test scene for all of them: `content/test-scenes/tree-bases/`, one species (the spruce of
`content/flora/spruce-tall.flora.json`, and an oak row for the flare) on four ramps of 0, 15, 30
and 45°, eight yaws a ramp, with cameras 2, 10 and 50 m down-slope of each and one across the
slope at 10 m.

| Stage | What is built | What it returns to the eye | What holds it |
|---|---|---|---|
| 1. The skirt (**built**, below) | the tube carried down on every tree species' trunk; the solver's version and the pins | no gap under any tree at any distance or yaw, to 45°, at no cost an instance: the owner's defect, closed | a CPU test over the four ramps and 64 azimuths: the skirt's lowest ring is under the ground beneath it; every LOD cut and distance form keeps it; a GPU case on the scene, from the ids |
| 2. The footprint | the 17-sample seat and plane fit in the scene reader and the placements; `engine-content ground sample` | trees beside a tread, a cut bank and the lattice's edge sit; authors can ask the drawn ground | a tread benched under a rim; the whole read and the streamed tiles equal; pinned bytes of the fit for the containers |
| 3. The conformed foot | the foot part, the pose's third kind, its normal, the padding and the error scale, the fade at the ring's edge | at 2 to 10 m the flare leans into the slope: exposed down-slope, buried up-slope | the stage against a CPU reference; rest poses to the bit at *g* = 0; rays against raster word for word on a conformed tree; a cost pair at 3840 x 2160 under traced primaries and shadows |
| 4. The laterals | the rounded roots by habit in the foot; near-detail clusters left out of distance forms; shapes handed to the exclusion; G102's ridge and sleeve removed | roots on every tree at 2 to 15 m, round, in the trunk's bark, with no grass through them | the collapse check G102 left open, on the foot; a form without marked clusters; the chain's demand counted on the scene and on one forest pair |
| 5. Surface and ground | the bark's height term; the cover's host including the foot; litter banked up-slope; the apron's mask | the foot darker, mossed and half buried in litter, as the r52 sheet has it | pictures beside the reels |
| 6. Fitted roots for hero ground | a tile's merged roots fitted to rough ground | roots over ledges and banks | later, on the foot's section |

Stage 1 stands alone and is the one that answers the walk. Stages 3 and 4 share the foot and are
best built by one row; 2 can run beside 1.

### Stage 1 as built (2026-11-12, branch b62-G201-1)

Every tree mesh the solver makes carries the skirt ([flora](../subsystems/flora.md#the-trunks-skirt)
has the rule, each trunk kind and the re-pins): two rings under the stem's bottom ring, the first a
quarter of the rim's radius down with the radius grown by a tenth, the second straight under it at
*rho* tan 45° + 0.15 m below the origin. Two things differ from the design above, both found in the
build:

- **The depth is measured from the origin, not the stem.** `1.1 r + 0.15 m` assumed a stem on the
  axis; a birch clump's stems and a leaning palm's foot stand off it, and the ground under a stem
  0.3 m out at 45° is 0.3 m lower again. So the depth is the lowest ring's farthest point from the
  origin times tan 45° plus the margin: the design's number for a stem on the axis, deeper for one
  off it (the clump's 0.39 to 0.44 m where 1.1 *r* + 0.15 m would have given 0.30 to 0.34).
- **The seat in the generator section is left to stage 2**, its only reader; the skirt needs
  nothing recorded, the plan counts its triangles.

| Trunk kind | Sides | Skirt triangles | Rim | Bottom |
|---|---|---|---|---|
| date palm (default) | 16 | 64 | 0.23 m | -0.40 m |
| broadleaf (default) | 12 | 48 | 0.48 m | -0.68 m |
| broadleaf profiles at 10 m (maple to alder) | 12 | 48 | 0.08 to 0.34 m | -0.23 to -0.53 m |
| conifer (default) | 10 | 40 | 0.41 m | -0.60 m |
| evergreen profiles at 10 m (pine to juniper) | 10 | 40 | 0.14 to 0.36 m | -0.31 to -0.55 m |
| birch, one stem; a three-stem clump | 10; 8 a stem | 40; 96 | 0.17 m; 0.13 to 0.17 m | -0.34 m; -0.39 to -0.44 m |
| willow, orchard tree | 10, 12 | 40, 48 | 0.68 m, 0.20 m | -0.90 m, -0.37 m |
| ancient tree (20,000 rung) | 59 | 236 | 11.0 m (the buttresses) | -12.3 m |

The forest's own meshes were not rebuilt here (the proof of concept is read, never run); the
design's 0.58 to 1.40 m for their rims of 0.39 to 1.14 m is the same rule.

**What it costs, as built.** Containers from `engine-content flora`, main (33ff2824) against the
skirt, from the same recipes:

| Recipe | Triangles | Vertices | Clusters (finest) | Bytes |
|---|---|---|---|---|
| `spruce-tall` | 23,450 → 23,490 (+40) | 32,627 → 32,647 | 545 → 546 (381 → 381) | 2,537,860 → 2,525,160 |
| `oak` | 58,839 → 58,887 (+48) | 97,469 → 97,690 | 1,691 → 1,696 (1,020 → 1,020) | 5,489,180 → 5,484,592 |
| `date-palm` | 55,312 → 55,376 (+64) | 107,003 → 106,991 | 1,753 → 1,749 (799 → 799) | 4,259,404 → 4,259,148 |
| `willow` | 52,988 → 53,028 (+40) | 196,107 → 196,057 | 3,204 → 3,203 (1,516 → 1,516) | 7,452,592 → 7,451,216 |

The finest clusters do not change in number (the skirt's triangles join the trunk's own clusters);
the coarser levels move by a few clusters either way, the simplifier working on a grid the skirt
made a few percent coarser. The bytes fall a little where the star's atlas is baked (spruce, oak):
its PNG compresses differently with the trunk carried to the box's bottom.

A cost pair on `content/test-scenes/elder-grove` at 3840 x 2160, `--raster rt --shadows rt`, the
bounce on (`renderer.gi.mode=bounce`), the scene's 38 s path, from main's release binaries
(`b61\bin-main`) and this branch's copied out of the tree, alternated in one lease of the GPU lock
(the machine: other processes at 18 to 37 % of the CPU, the GPU at 99 % and mine):

| Run | Frame (median, p95) | Traced chain (CLAS, BLAS, TLAS, records) | Resolve | Trace | Pairs |
|---|---|---|---|---|---|
| main | 9.086, 12.032 ms | 1.037 ms | 4.660 ms | 1.728 ms | 106,392 |
| skirt | 8.985, 12.066 ms | 1.039 ms | 4.617 ms | 1.659 ms | 106,430 |
| main | 8.995, 11.993 ms | 1.035 ms | 4.615 ms | 1.692 ms | 106,392 |
| skirt | 8.945, 12.042 ms | 1.038 ms | 4.643 ms | 1.623 ms | 106,430 |

Inside the noise: the frame moves by -0.08 ms (the run-to-run spread is 0.09), the chain by
+0.003 ms, the pairs by 38 (the 21 trees' skirts). The picture at 2 m on the 30 degree ramp, before
and after, and the ids across the slope (main 103 gap pixels under the 30 degree ramp's spruce and
658 under the 45; the skirt none) are in the branch's hand-back.

### What the build needs from other rows

- **The flora solver** (`domain/flora`: the kit's tube, each species' trunk, the container's
  generator section): stages 1 and 4.
- **The pool's flora stage** (`deform.slang`, `domain/gfx/flora_motion.h`, the renderer's poses and
  `flora_bounds_padding`): stage 3.
- **Placements** (`domain/scene_gen`'s hand-over, the scatter's point, the scene reader's grounded
  instances): 4 bytes and 17 queries, stages 2 and 4.
- **Distance forms** (R94's extraction): leave near-detail clusters out, stage 4.
- **The detail-tiers row**: a reach on a mesh-table entry, for the forest's item 60.
- **The ground plan** (R144): a list of mask regions, if the apron is built.
- **The cover**: whether a tree's cover rides its host's parts, before stage 5.
- **The traced chain's row**: nothing asked; stage 4 reports what the foot adds to its demand.

## What to ask of the next reels

- **A tree's foot from 1 to 3 m, walked round**, up-slope, across and down-slope, on ground of a
  stated steepness, with something of known size in frame: a pine, a spruce, a beech, an oak. This
  is the picture every part of stages 3 to 5 is judged against, and no reel has it.
- **The same species on level ground**, the same way, for the flare's height and the laterals' run.
- **The up-slope side close**: what is banked against the stem, how high, and what it is made of.
- **The down-slope side close**: how far a lateral runs before it dives, whether daylight shows
  under it, how the soil under it looks.
- **A forested slope in profile at 10 and 50 m**, so the line where stems meet the ground is seen
  edge-on.
- **The bark across the collar**, dry and wet: where the stem's bark ends, the moss line, the
  litter line, a root top polished on a path.
- Later: trees on rock and boulders, a bank's undercut roots, and a curved butt with what tilted it.

## What could not be found or opened

- Di Iorio et al. (2005) beyond its abstract, so its slope angles and its counts by sector are not
  on this page; Chiatante et al. (2003) and both Nicoll papers on sloping terrain not at all.
- Any measurement of soil banked up-slope of a living stem (Hoffman's report could not be read),
  of a down-slope lateral's exposed length against slope angle, or of the flare's height against
  diameter (Raile 1982 is a scan).
- Anything opened on bark across the root collar (one oak study through a search only).
- The erosion rates, the creep fallacy and the vegetation dams are from abstracts through a
  search, not from the papers.
- The forest's count of trees inside the posed ring, which the estimate of the foot's traced
  demand needs; and whether a cover on a tree carries the motion contract.
