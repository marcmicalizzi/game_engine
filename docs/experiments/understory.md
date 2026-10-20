# The understory: shrubs, ferns, grass tufts and herbs (G75, 2026-10-21)

The research the flora solver's four understory species are grounded in, the constants chosen from
it, what the recipes come to, what the understory test scene costs, and the designs for the three
rows that follow it: climbing plants (G76), forest placement (G77) and organic cover (G78, with the
renderer's R90). The rules that follow from it are in [flora](../subsystems/flora.md#the-understory);
the allometric recursion the shrub reuses is [tree allometry](tree-allometry.md)'s, whose sources
(the pipe model, Corner's rules, Horton–Strahler, WBE, elastic similarity, the leaf area index) are
not repeated here.

**Why.** The owner's fantasy RPG starts with a village in a forest, and a forest is not trees alone:
what a walker sees at eye height is mostly the shrub layer, the herb layer and the ground. Until
this row the solver had seven tree species and nothing under 3 m.

## A temperate forest's layers

| Layer | Height | What grows there | What it gets of the light |
|---|---|---|---|
| Canopy | the trees' crowns, 15–40 m | the overstorey trees | all of it, and it intercepts most |
| Understorey, or shrub layer | about 1 to 5 m | shrubs (hazel, elder, dogwood, hawthorn), saplings of the canopy's trees | what the canopy lets through |
| Herb layer | under about 1 m | ferns, grasses, sedges, flowering herbs, tree and shrub seedlings | what the canopy and the shrubs let through |
| Ground, or moss layer | the surface | mosses, liverworts, lichens, fungi, leaf litter | the least |

The herb layer is defined by height: **all vascular plants 1 m tall or less** in Gilliam's work
(Gilliam 2007, "The ecological significance of the herbaceous layer in temperate forest
ecosystems", *BioScience* 57(10): 845–858; his Fernow Experimental Forest studies use ≤ 1 m), and
0.5 to 1.5 m elsewhere in the literature, which his book chapter notes is why the term's
definitions vary (Gilliam 2014, *The Herbaceous Layer in Forests of Eastern North America*, 2nd
edn, OUP). It holds the most species of any stratum and under 0.1% of the forest's above-ground
biomass, which is why a scene's budget can afford many of them and why they matter to the look far
more than to the mass.

**How much grows below follows the light through the canopy.** Light under a canopy falls off as
Beer–Lambert's law of the leaf area above it, I / I0 = exp(−k · LAI) (Monsi and Saeki 1953), with
an extinction coefficient k of 0.3 to 1.3 across canopies and **0.53 to 0.67 measured in mature
hardwood stands** (the southern Appalachian stands of the USDA Forest Service's
[litter-fall comparison](https://research.fs.usda.gov/treesearch/4724); 0.53–0.54 used in two
German temperate stands by Battuvshin and Menzel 2022). At k = 0.5, 95% of the light is intercepted
by an LAI of 6, so a closed broadleaf canopy of LAI 4 to 6 leaves 5 to 14% of the light at the
shrub layer. That is the shape G77's placement takes: shade-tolerant ferns and shrubs under a
closed canopy, grasses and flowering herbs in its gaps and at its edges, where the transmitted
light is high; the scene below places by hand what that rule will place.

**What is and is not this row's.** Saplings are the tree species at a small `height_m` — free since
G73's allometric crown, which makes a 3 m oak a 3 m oak's structure and not a scaled 12 m one; the
grove's sapling recipe is `oak-sapling.flora.json` (a broadleaf at 3 m). **Leaf litter is the
ground's material**, not geometry: a forest floor's brown is a tiled set on the terrain, which this
row leaves to the ground. **Moss, lichen, fungi and algae are G78's** (below): cover grown over a
host's surface, not a plant standing on the ground.

## The four forms

### The shrub

A multi-stemmed woody plant branching at or near the ground. Hazel (*Corylus avellana*) is a large
shrub or small multi-stemmed tree, typically about 6 m and 4 to 8 m at most (the
[RHS](https://www.rhs.org.uk/plants/4511/corylus-avellana-(f)/details)), and "always forms more or
less ring-shaped multistemmed shrubs" whether grown naturally or coppiced (a
[Finnish study](https://journal.fi/msff/article/download/7815/5991/19432)); coppice stools carry
roughly ten to twenty-five rods after a decade (practitioners' counts; neglected stools fewer and
larger), cut on 7 to 15 year rotations at 1,000 to 2,000 stools a hectare (the
[conservation handbook](https://conservationhandbooks.com/?p=379)). Elder, dogwood and hawthorn are
the same habit at 2 to 6 m. The model:

- **Stems** from a **stool**: `stems` (1–40, default 16) basal stems, their feet filling the stool's
  disc as a sunflower does with the stoutest at the middle (a stool's oldest stems stand at its
  centre, its young shoots round the rim), each leaning out by up to `stem_spread_deg` (28°) the
  further from the middle it stands, and turning towards the light by `upturn`.
- **Sizes by elastic similarity**: the stoutest stem's radius is `stem_radius_m`, or, at its default
  of 0, 2.2 cm × (H / 4 m)^1.5 — H ∝ D^(2/3), McMahon's elastic similarity as the broadleaf's trunk —
  so a 4 m hazel's stoutest stem is 4.4 cm across, inside hazel's 2 to 8 cm. The others are 0.55 to
  1 of it, and a thinner stem is a younger one and shorter by the same law.
- **Each stem is the allometric recursion** (G73's, `kit.h`) to a terminal twig of the species' own
  size: a 1.6 mm, 0.45 m twig (a season's shoot), the pipe model at Δ = 2, Horton's branching ratio
  4, WBE's length ratio 0.63, a leader share of 0.35, laterals at 40°. So a bigger shrub is more
  shrub: 16 stems at 1 m are twigs themselves, at 4 m 889 twigs over 5 orders, at 5 m 2,047 over 6.
- **Leaves at species size**, one leaf a card: a hazel leaf is broadly ovate, 6 to 12 cm long and
  nearly as wide — `leaf_length_m` 0.10 and `leaf_width` 0.8 — and their count is the leaf area index
  over the shrub's footprint. **LAI 2.5**: a shrub layer runs 1 to 4; the default hazel realises 2.16
  over its 29.4 m² footprint, short of the 2.5 asked because a twig of six cards a side is longer than
  its 0.45 m and the kit's spray steps closer, narrowing each card (the trees' same rule).
- **Bramble fits as a variant** (`bramble.flora.json`): arching canes are stems with a negative
  `upturn` and a wide spread, so they lean out and bow back down towards the ground. What it does not
  have is a bramble's tip-rooting (a cane that reaches the ground roots and starts a new stool) and its
  prickles; the first is placement's (G77), the second a texture's.

### The fern

A crown of pinnate fronds from a rhizome. The male fern (*Dryopteris filix-mas*) has fronds 28 to
120 cm long (to 2 m in some floras) and 10 to 30 cm wide, the stipe under a quarter of the frond,
**20 to 30 pairs of primary pinnae** (17–38 and 25–50 in other floras), the lower pinnae shorter than
the longest ([UW-Green Bay herbarium](https://uwgb.edu/biodiversity-old/herbarium/pteridophytes/dryfil01.htm);
[NZ Flora](https://nzflora.info/factsheet/taxon/Dryopteris-filix-mas.html);
[NYBG](https://sweetgum.nybg.org/science/vh/monographs-details/?irn=26437)), in a shuttlecock of a
dozen or two fronds from one crown. Bracken (*Pteridium aquilinum*) is the other habit: single
fronds 0.3 to 1 m (to 2.75 m) from a creeping rhizome, at 23 to 42 fronds a square metre in dense
stands (Marrs and Watt 2006, *Journal of Ecology* 94, the Biological Flora; a
[2022 study of 15 sites](https://link.springer.com/article/10.1007/s11355-022-00528-4)) — so a
bracken stand is many plants of one to three fronds, placed densely, which is placement's and not
the plant's. The model:

- `fronds` (default 14) on the palm's crown rule scaled down: each one's angle from vertical
  `spread_deg` × √age, the youngest nearly upright at the crown's middle and the oldest leaning out
  furthest, on the golden angle; its length 0.75 to 1 of `frond_length_m` (0.9 m) with age.
- A frond is **one spray** (the kit's, with a card length per station): the stipe, `stipe` (0.22)
  of the frond, as a ribbon of 4 mm cards on the same surface, then `pinnae` (24) pairs of pinna
  cards whose length follows a lanceolate outline — sin(π (0.15 + 0.85 u)) of `pinna_length` (0.15 of
  the frond, so a 0.9 m frond is 27 cm across), the longest about two fifths up the blade, the lowest
  half of it, the tip a few percent — at `pinna_angle_deg` (70°) to the rachis.
- The rachis **arches under its own weight**: it turns towards the ground by `droop` (0.45) × 90°
  over its length, the older fronds more.
- A pinna is a card, not its pinnules: the pinnate-pinnatifid cut of a real pinna is an image's, and
  wants an alpha mask (R65).

### The grass tuft

A bunch of blades from one base. Tufted hair-grass (*Deschampsia cespitosa*), the woodland-edge
tussock grass: basal leaves 7 to 40 cm long (10 to 60 in GrassBase) and **2 to 5 mm wide**, flat or
inrolled, stiff; tussocks 30 to 120 cm with the culms
([Kew POWO](https://powo.science.kew.org/taxon/urn:lsid:ipni.org:names:397953-1/general-information);
[GrassBase](https://www.kew.org/data/grasses-db/www/imp02819.htm);
[NZ Flora](https://nzflora.info/factsheet/Taxon/Deschampsia-cespitosa.html)). The model: `blades`
(default 120) from a base of `base_radius_m` (4 cm), filling it as a sunflower does; the inner ones
upright, the outer ones leaning out to `spread_deg` (35°) and **arching** by `droop` (0.5) × 90°, the
outer ones more — a tussock's fountain; each 0.6 to 1 of `blade_length_m` (0.45 m) and
`blade_width_m` (4 mm) at its base, its edges straight to 0.65 of its length and closing to a point.
`crossed` makes each blade two crossed strips, which reads from every side at a distance at twice
the triangles. A fescue (`fescue.flora.json`) is the same at 22 cm and 2 mm. Flowering culms are
not drawn: a tuft is its leaves.

### The herb

A stem with leaves and flowers, 0.2 to 1.5 m. Three recipes, three habits:

- **A meadow flower's single head** (`daisy.flora.json`, the default): a 0.6 m stem, eight leaves on
  the golden angle up its lower 0.6, one flat head of sixteen white petals 2 cm long — an ox-eye
  daisy's (*Leucanthemum vulgare*) form, rays round a disc. The disc is the petals' own colour (one
  flower material; a yellow disc is a second material, not built).
- **A foxglove's spike** (`foxglove.flora.json`): *Digitalis purpurea* to 1.25 m in the wild (2 m in
  some floras), its inflorescence of **15 to 50 or more drooping flowers**, the corolla **3.5 to 5 cm**
  ([Flora of the Burren](https://flora-burren.linnaeus.naturalis.nl/linnaeus_ng/app/views/species/taxon.php?id=37892);
  [NZ Flora](https://nzflora.info/factsheet/Taxon/Digitalis-purpurea.html)): 1.2 m, twenty-two
  flowers up the top 0.4, each five petals 4.5 cm at 22° from its axis — a bell — nodding out 70° from
  the stem, purple.
- **A nettle's leafy stem** (`nettle.flora.json`): *Urtica dioica* 0.6 to 2 m, its leaves
  **opposite**, 4 to 15 cm, its flowers inconspicuous axillary catkins
  ([NC State Extension](https://plants.ces.ncsu.edu/plants/urtica-dioica/common-name/stinging-nettle)):
  1 m, twenty leaves in opposite pairs each turned a right angle from the last, 11 cm, no flowers.

## The constants chosen

| Species | Constant | Value | Where it comes from |
|---|---|---|---|
| shrub | stems | 16 (1–40) | hazel's ten to twenty-five rods a stool |
| shrub | stoutest stem | 2.2 cm × (H / 4 m)^1.5 radius | hazel's 2–8 cm stems; elastic similarity |
| shrub | twig | 1.6 mm radius, 0.45 m | a season's shoot; Corner's species-constant twig |
| shrub | leaf | 0.10 m, width 0.8, LAI 2.5 | hazel's 6–12 cm ovate leaf; a shrub layer's LAI of 1–4 |
| shrub | recursion | Δ 2, Rb 4, length ratio 0.63, leader 0.35, laterals at 40° | the trees' (tree-allometry.md) |
| fern | fronds | 14, 0.9 m, stipe 0.22 | the male fern's 0.3–1.2 m fronds, stipe under a quarter |
| fern | pinnae | 24 pairs, longest 0.15 of the frond, 70° | its 20–30 pairs; the blade 10–30 cm wide |
| tuft | blades | 120, 0.45 m, 4 mm | hair-grass's 7–40 cm, 2–5 mm leaves |
| herb | daisy | 0.6 m, 8 leaves of 7 cm, 16 petals of 2 cm | a meadow flower's head |
| herb | foxglove | 1.2 m, 22 flowers of 5 petals of 4.5 cm | its 15–50 flowers, 3.5–5 cm corollas |
| herb | nettle | 1 m, 20 opposite leaves of 11 cm | its opposite 4–15 cm leaves |

## The collision threshold

**A shrub's stem stops a walker when it is at least 1.25 cm in radius at the ground** (2.5 cm
across): `flora::k_shrub_collider_radius_m`. A stem pushed aside by a walker is a cantilever: the
force that deflects its top by δ over a length L is F = 3 E I δ / L³, with I = π r⁴ / 4. Green
hardwood's modulus of elasticity along the grain is about **9 GPa** (8.6 to 10.7 GPa for white oak,
northern red oak, white ash and sugar maple green; the USDA Forest Products Laboratory's *Wood
Handbook*, reproduced in [this table](https://amesweb.info/Materials/Youngs-Modulus-of-Wood.aspx)).
A walker leaning into a stem pushes about 150 N (some 15% of its weight); bending a stem 0.3 m — a
stride's width — over the metre to the walker's hip takes that force at r⁴ = 4 F L³ / (3 π E δ),
**r = 1.24 cm**. Below it a stem bends aside (a 0.8 cm dogwood stem takes 23 N); above it, it does
not (a 2 cm hazel stem takes 900 N). Stems are a shrub's parts, so the rule is by part; its leaves
never collide, nor do its recursion's shoots, which are well under the threshold at their bases.
Ferns, tufts and herbs have no body at all: their part 0 is a root with no geometry and nothing
else is the trunk or flagged.

## What the AAA pipelines do with ground cover

**How grass and ground cover are drawn at scale.** Every production renderer of the last decade
draws ground cover as **GPU-instanced cards or blades placed by density maps**, generated near the
camera and faded out with distance:

- **Ghost of Tsushima** (Wohllaib, "Procedural Grass in *Ghost of Tsushima*", GDC 2021) generates
  individual blades on the GPU per tile each frame, each with its own procedural shape and animation,
  from artist-painted density and type maps, at a density that falls with distance, its wind from a
  shared simulation ("Blowing from the West", GDC 2021, millions of blades); the blade's width is
  widened in view space at a distance so it does not alias away.
- **Horizon Zero Dawn** (Sanders, "Between Tech and Art: The Vegetation of *Horizon Zero Dawn*",
  GDC 2018) places all vegetation procedurally on the GPU from ecotope rules over density maps, in
  tiles streamed round the camera.
- **Unreal Engine's** foliage and grass systems (the landscape grass type, PCG, and since 5.x Nanite
  foliage) are instanced static meshes with a cull distance per type and a density scale per
  scalability preset, cross-quad or alpha-masked cards for grass, and wind and interaction as vertex
  offsets.

**What this engine has of that.** Each plant is a flora mesh with its own cluster LOD DAG, placed as
an instance — by the scatter generator where a rule can say where (density fields, spacing,
exclusions, a per-tile budget, and `far` per kind: a far ring places fewer kinds, [scatter](../subsystems/scatter.md#the-budget-is-the-lod-policy))
or by hand — swayed by the wind and pushed aside by a walker through the flora deform stage, every
plant its own phase. **What it lacks**: **density that thins with distance** within a kind (`far` is
all or nothing per kind, and every instance is a pair set the cull pass visits) — G77's; **alpha
masks** on cards (a pinna, a blade, a petal are opaque polygons, R65's); **per-blade GPU
generation** (every blade here is a vertex of a mesh, which is what bounds the herb layer's count,
below); and **detail tiers** a graphics preset honours (the field is declared and recorded, the
renderer honours none of it yet: the detail-tiers row's).

## What the recipes come to

The committed recipes through `engine-content flora` and `engine-content stats`, and E10's two
picture checks — the container's coarse (1 px) and finest (0.05 px) cuts captured offscreen through
engine-host at 640 × 640 from the fit orbit, FLIP from `engine-image compare` — by
`tools/e10-harness.ps1`'s own procedure with its build step pointed at `engine-content flora` (a
scratch copy outside the repository; msvc-debug, RTX 5090, captures under the GPU lock, 2026-10-21,
none of it a timing). The thresholds are E10's: 0 warnings, smallest island ≥ 1 texel of a 4096
atlas, coarse-against-finest FLIP ≤ 0.02, and the coarse cut ≤ 0.75 of the finest's pairs where the
finest has at least 3 pairs per 1,000 object pixels.

| Recipe | Triangles | Clusters / levels | Islands | Seam | Coarse / finest pairs | Collapse | PSNR | Verdict |
|---|---|---|---|---|---|---|---|---|
| `hazel` (4 m, 16 stems) | 18,651 | 721 / 11 | 1,052 | 8.5% | 150 / 283 | 0.53 | 44.4 | pass |
| `dogwood` (2.2 m, 24 thin stems) | 8,760 | 300 / 9 | 208 | 10.2% | 35 / 125 | 0.28 | 36.4 | pass |
| `bramble` (1.4 m, 12 arching canes) | 3,876 | 146 / 8 | 136 | 17.8% | 53 / 60 | **0.88** | 37.9 | **fails the collapse** |
| `male-fern` (0.9 m, 14 fronds) | 1,736 | 52 / 7 | 14 | 0 | 23 / 23 | — | identical | pass |
| `bracken` (1.3 m, 3 fronds) | 324 | 11 / 4 | 3 | 0 | 5 / 5 | — | identical | pass |
| `hair-grass` (0.45 m, 120 blades) | 1,080 | 55 / 7 | 120 | 0 | 22 / 22 | — | identical | pass |
| `fescue` (0.22 m, 90 blades) | 630 | 33 / 6 | 90 | 0 | 13 / 13 | — | identical | pass |
| `daisy` (0.6 m) | 190 | 7 / 3 | 10 | 13.8% | 4 / 4 | — | identical | pass |
| `foxglove` (1.2 m, 22 flowers) | 480 | 18 / 4 | 35 | 4.6% | 9 / 9 | — | identical | pass |
| `nettle` (1 m, 20 leaves) | 166 | 8 / 4 | 21 | 13.9% | 3 / 3 | — | identical | pass |
| `oak-sapling` (a 3 m broadleaf) | 4,412 | 160 / 8 | 157 | 21.4% | 44 / 62 | 0.71 | 42.8 | pass |

**The herb layer is its own far form.** A fern, a tuft or a herb is a few hundred to two thousand
triangles in a handful of clusters, under the collapse check's density at its own framing, and its
coarse cut is its finest: the DAG still coarsens it at a distance, and what costs is the instance,
not its triangles (below). **The bramble fails the collapse**, and it is the framing's: at the fit
orbit a 1.4 m plant is 2.2 mm a pixel, and an arching 6 mm cane's rows cannot be removed without
moving it by more than that, so the coarse cut keeps them; three variants (fewer, thicker canes;
longer twigs; half the arch) drew 0.81 to 0.95. Stems sized by girth (a 6 mm cane three sides round
rather than eight, which saved the hazel 1,325 triangles and the dogwood 3,000) moved it from 0.79
to 0.88. A bramble far enough away to matter is a few pixels, which the DAG serves; the recipe
stands, and the number is recorded rather than tuned away.

## The understory scene

[`content/test-scenes/understory/`](../../content/test-scenes/understory/README.md): five trees and
two saplings from the existing recipes round an octagonal clearing 14 m across; three scatter
layers — shrubs (hazel and dogwood, 0.022 a square metre, about twenty) in the canopy ring, the shade
(male fern, bracken, nettle by a noise density of up to 0.25 a square metre) in the same ring, the
light (hair-grass, fescue, daisies at 2 a square metre) in the clearing — kept 1.2 m clear of every
trunk; and foxgloves at the clearing's edge and brambles by the path placed by hand. **415
instances** (403 scattered, 12 by hand), 35,387 pairs, 5,405 clusters in fourteen meshes. A camera
path walks it at eye height for 44 s.

**What it costs at 1080p** (`engine-view --scene … --camera-path … --offscreen --width 1920 --height
1080 --repeat 1 --warmup 60 --wait-quiet 60 --benchmark`, msvc-debug, RTX 5090, traced shadows,
2,641 frames, 2026-10-21). None of the three runs found a quiet machine — other processes used 26 to
87% of the CPU and the GPU was 8 to 98% busy at either end — so every number is an upper bound.

| | Motion on | Motion off | Four times the herb layer, motion on |
|---|---|---|---|
| Instances, pairs | 415, 35,387 | 415, 35,387 | 724, 49,421 |
| GPU total ms, median / p95 / p99 | 0.88 / 1.14 / 1.35 | 0.74 / 1.13 / 1.45 | 1.00 / 1.43 / 1.53 |
| of which rt (BLAS + trace), CLAS, raster, resolve, deform, cull | 0.41, 0.14, 0.032, 0.33, 0.082, 0.020 | 0.38, 0.13, 0.030, 0.32, 0, 0.016 | 0.43, 0.15, 0.033, 0.34, 0.15, 0.021 |
| Visible pairs, shadow casters (median) | 1,961, 2,223 | 1,215, 2,274 | 1,915, 2,391 |
| Frame thread (debug) median / p95 | 14.1 / 22.1 ms | 0.79 / 1.38 ms | 27.5 / 31.3 ms |
| GPU memory the engine used | 445 MiB | 317 MiB | 350 MiB |

**The GPU is not the limit; the frame thread is.** The GPU's under-a-millisecond is the traced
shadows and the resolve, and four times the herb layer adds 0.12 ms. With the motion on, the frame
thread spends **about 33 µs an instance** (debug) composing every part's pose on the CPU
([renderer](../subsystems/renderer.md#the-flora-stage)), because every frond and every blade is a part:
a hair-grass tuft is 121 parts, so the clearing's tufts are most of the scene's parts. 415
instances cost 13.7 ms of it and 724 cost 27 ms, linear; motion off, 0.07 ms. (Visible pairs rise
with the motion because the sway pads every instance's spheres, which the LOD then sees as nearer.)
**What the pool and the frame afford**: the deformed-vertex pool is nowhere near its 96 MiB floor
(the deform pass is 0.08–0.15 ms); the frame thread, in this debug build, affords about 400 swaying
plants in a 16 ms frame and in a release build some five to ten times that (its passes are what the
debug runtime's iterator checks slow most; not measured here). A forest floor wants tens of thousands.

**What G77 must do to go further**: **swaying by distance** — instances past a few metres drawn rigid
or with one pose a kind (a shared phase), so the CPU composes parts only for the plants a walker is
among; **parts on the GPU** for the herb layer — a blade's or a frond's bend from the wind and the
walker is a function of its rest tangent and its weight, which a shader can evaluate without the
CPU's parent-first composition (the herb layer is one level deep); **density by distance** within a
kind (survivors scaled up), so the far herb layer is a fraction of the near one; and the detail
tiers' cut-off, so a low preset leaves the `rich` kinds out altogether.

## G76: climbing plants, a vine solver grown over a host mesh

The owner's question — can a vine be grown over a tree, a wall or a rock? — has a yes. **A growth
walk along the host's surface**: seeds at the host's base (or where a recipe says), and each step
the vine's tip moves along the surface's tangent plane, its direction a blend of **adhesion**
(towards the surface, so it stays on it), **phototropism** (towards the light: up, or towards the
canopy's gaps) and **gravity** (a stem that loses the surface hangs), with branching at a rate and
leaves at every node. The host's surface is queried through a small **BVH over its triangles** (the
closest point and its normal, the same query a decal or a footprint needs) or, cheaper, its
collision proxy. The output is the vine's own mesh — stems as the kit's tubes, leaves as its cards,
its own motion contract — **placed with the host** as a companion instance in the host's frame.
Static hosts first (walls, rocks, the ruins); a tree's load-bearing parts next (a vine on a trunk is
on part 0 and moves with it, its vertices riding the host's part through the motion contract);
a swaying host's finer parts later. The sources: Luft's ivy generator (Thomas Luft, "An Ivy
Generator", 2007), the growth walk with adhesion, gravity and random walk weights that every game
ivy tool descends from; and Hädrich, Beneš, Deussen and Pirk, "Interactive Modeling and Authoring
of Climbing Plants" (*Computer Graphics Forum* 36(2), Eurographics 2017), plants as connected
anisotropic particles that respond to their environment and their inner state — real time for 20
plants of 25,000 branches — which is the model to follow once a vine must grow round a moving
obstacle.

## G77: forest placement

What the scatter generator has: a seeded lattice of Poisson-disc candidates per kind, a density
field (constant, noise, or a mask texture), slope and height limits, exclusions by circles, masks,
other layers and earlier entries' footprints, a per-tile budget, and `far` per kind. What it lacks
for a forest: **a species mix that follows the ground** (soil, wetness, aspect — the ecotope rules
Horizon's placement uses); **understory density from canopy light** — the trees placed first, their
crowns' LAI over a ground grid giving the transmitted light by Beer–Lambert at k ≈ 0.5, and the
shrub, fern, grass and herb kinds' densities each a function of it (ferns and shrubs under a closed
canopy, grass and herbs in gaps and at edges) — which is a density field computed from an earlier
entry's placements, a seam the generator has the inputs for (`Context::earlier`) but no field for;
**clumping** (bracken's dense stands, a tussock meadow), which a Poisson disc's even spacing does
not give; and **ground cover by distance**, a kind thinned within its `far` rather than cut at it,
with the survivors scaled up to keep the cover's look, which is how every engine above keeps grass
affordable.

## G78: organic cover — moss, lichen, fungi and algae over a host mesh

Two problems sharing the vine's host-surface query.

**(a) A cover mask solved over the host's surface** from what the ecology says grows where: moss on
shaded, damp, upward-facing and (in the northern hemisphere) north-facing surfaces, in crevices and
at bases near the ground and water, more on rough and acidic bark than on smooth (Glime,
*Bryophyte Ecology*, the habitat chapters); crustose lichen on exposed rock and bark; algae's green
film on damp shade; fungi on dead and dying wood. Each a function of the host's **slope, aspect,
curvature or ambient occlusion**, the terrain's and the water's **proximity**, the canopy's
**shade** (G77's light field) and an **age**. The graphics literature has solved the same: Dorsey
and Hanrahan's weathering ("Modeling and Rendering of Metallic Patinas", SIGGRAPH 1996, and the
weathering line after it) and Desbenoit, Galin and Akkouche's lichen ("Simulating and Modeling
Lichen Growth", *Computer Graphics Forum* 23(3), Eurographics 2004: an open diffusion-limited
aggregation over the substrate constrained by its environment, rendered by instancing 3D lichen
shapes from an atlas).

**(b) Drawing it.** The mask's **smooth cover** — moss mats, lichen crusts, algae — is a second
material set blended per vertex or per texel over the host's own, which the renderer's per-cluster
material table cannot do today: a renderer row, **R90 "A blended cover layer: a second material set
under a solved mask"**, depends on the material system's sets (G60) and the mask's output. The cover
**that has shape** — moss cushions, fruticose lichen, bracket fungi and mushrooms — is small flora
kit geometry scattered over the host where the mask is dense, drawn as instances like the
understory.

**What G76 and G78 share, built once:** the host query (closest point, normal, curvature and
occlusion from a BVH over the host's triangles or its proxy), the seeding (points on the host's
surface by rule), and the output as the host's **companion mesh** (an instance in the host's frame,
its vertices riding the host's motion parts).
