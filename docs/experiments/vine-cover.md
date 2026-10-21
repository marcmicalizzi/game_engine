# Climbing plants and organic cover: the grounding (G76, G78)

The research the cover capability ([cover](../subsystems/cover.md)) is built on: how a root
climber walks a surface and holds it, where moss, lichen, algae and fungi grow on stone and bark,
the graphics models of both, the constants chosen from them, and what the overgrown scene
(`content/test-scenes/overgrown/`) costs. The designs these build were one paragraph each in
[the understory](understory.md#g76-climbing-plants-a-vine-solver-grown-over-a-host-mesh); the rules
that follow from this page are on the subsystem page.

**Why.** The owner asked whether a vine solver attached to an arbitrary mesh — a tree, a building —
is feasible, and then for moss, lichen and other organic cover beside it. Both read a host's surface
and grow on it by what the ecology says, so they are one foundation (a BVH over the host's triangles
and the exposure terms read from it) and two solvers.

## The vine

### What the graphics literature does

- **Luft's ivy generator** (Thomas Luft, "An Ivy Generator", 2007; GPL; described on
  [BlenderNation](https://www.blendernation.com/2007/03/26/awesome-creeping-ivy-generator/) and
  ported to Blender, whose port exposes the weights:
  [bf-extensions, 2010](https://lists.blender.org/pipermail/bf-extensions-cvs/2010-July/000324.html)).
  The ivy grows from a root by a walk under several forces: a **primary direction** (a weighted
  average of the earlier directions), a **random influence**, **adhesion** towards nearby surfaces,
  an **up-vector** that imitates phototropism, and **gravity**, which a stem far from any surface
  feels. Branching is a probability per step; leaves are placed along the stems. Luft says plainly
  that it is not a biological simulation but a simple way to convincing vegetation that adapts to a
  scene. The Blender port starts its primary, random, gravity and adhesion weights at 0.5 each and
  ends a branch once it has floated too far — what `max_free_m` is here. Every game ivy tool
  descends from it.
- **Hädrich, Beneš, Deussen and Pirk**, "Interactive Modeling and Authoring of Climbing Plants",
  *Computer Graphics Forum* 36(2): 49–61, Eurographics 2017
  ([Purdue PDF](https://www.cs.purdue.edu/homes/bbenes/papers/Haedrich2017EG.pdf)): a plant as
  connected **anisotropic particles** carrying light sensitivity, wind response and reactions to
  obstacles, its physics by shape matching, real time for 20 plants of 25,000 branches. It is the
  model to follow when a vine has to respond to a moving host or obstacle; for a static host grown
  once at content-build time, Luft's walk gives the same picture at a fraction of the machinery.

### What the biology says

- **Climbing strategies.** Climbers hold on by **adventitious rootlets** (ivy, *Hedera helix*;
  climbing hydrangea), **twining stems** round thin supports (honeysuckle, hops, wisteria),
  **tendrils** (grape, pea, passion flower; Virginia creeper's end in adhesive pads), or not at all —
  **ramblers** and scramblers lean on their support and hook over it with thorns (roses, bramble).
  **This engine models the root climber**, the one that grows *over* a surface rather than round a
  thin support, and the rambler's behaviour falls out of it on a surface the rootlets cannot hold:
  the stem leans on the surface and slides off it. Twining and tendrils need a support a few
  centimetres across and a model of the stem coiling round it, which is the particle model's job
  (above) and not built.
- **Ivy holds rough surfaces.** Its aerial rootlets cling to "tree bark, brick, stone, and other
  textured surfaces" (NC State Extension, [*Hedera helix*](https://plants.ces.ncsu.edu/plants/all/hedera-helix));
  the juvenile, climbing phase is the one that makes them
  ([NYBG](https://www.nybg.org/content/uploads/2018/02/NYBGEcoQuest_2018_February_HuntForHedera_MoreInformation.pdf)).
  So attachment is a function of the host's **roughness**, read per triangle from its material.
- **Sizes.** Juvenile leaves are palmately 3–5-lobed, **4 to 10 cm** long (the NYBG guide); a
  compact cultivar's stems are 2.5 mm across with 2.1 cm internodes (US Plant Patent PP21360, a
  cultivar and not the species); old stems reach **4–6 cm** (Smithsonian's Araliaceae guide) and,
  on old trees, up to 25 cm (NYBG — the two disagree, and the smaller is the common case). Ivy
  climbs **20 to 80 feet** on a support (NC State) and grows several feet a year once established
  (University of Maryland Extension).
- **Juvenile ivy is negatively phototropic** towards its support — it turns *towards* the dark wall
  — and climbs; Luft's "up-vector" stands for both. Here the light term is straight up, and the
  adhesion term carries the turn into the surface.

### The constants chosen (`engine.cover.Vine`, defaults)

| Field | Value | From |
|---|---|---|
| `step_m` | 5 cm | about two internodes (the cultivar's 2.1 cm, the species' longer): one leaf a step at the default spacing |
| `primary_weight`, `adhesion_weight`, `light_weight`, `gravity_weight`, `random_weight` | 1.0, 0.6, 0.45, 0.6, 0.35 | Luft's five forces; the port's 0.5s with the primary dominant and the random turn lower, so a stem on a wall wanders without tangling |
| `adhesion_distance_m` | 8 cm | how far a stem still finds the surface: a step and a half |
| `max_free_m` | 0.5 m | a stem that has run this far without a hold stops (the port's float-length rule) |
| `hold_roughness` | 0.5 | the material roughness at which rootlets hold: stone and bark (0.8–0.95) yes, glass or polished stone (≤ 0.3) no |
| `branch_chance`, `branch_angle_deg`, `branch_depth` | 0.04 a held step, 40°, 3 orders | one branch every 1.25 m of held stem |
| `stem_radius_m`, `tip_radius_m` | 12 mm, 1.5 mm | a tip 3 mm across (the cultivar's 2.5 mm); the stoutest stem a few centimetres across, the common old stem's lower range; the pipe model at Δ = 2 between |
| `leaf_length_m`, `leaf_width`, `leaf_spacing_m`, `petiole_m` | 6.5 cm, 0.95, 5 cm, 3 cm | the middle of 4–10 cm, nearly as wide, one a node |

## The cover

### What grows where

- **Moss** wants water: shade, damp, sheltered crevices, the base of a trunk or a stone near the
  ground, upward faces that hold water and debris, and the north side in the northern hemisphere
  (Glime, *Bryophyte Ecology*, the habitat chapters). Bark **texture is the strongest factor** an
  epiphyte survey finds — rougher bark holds water and is colonised more easily — while bark pH
  showed no effect in one study ([*The Natural Environmental Science Research* 7,
  1994](https://www.jstage.jst.go.jp/article/nesr/7/0/7_1/_article/-char/en)) and the strongest
  correlation in another (European oak and ash). In Killarney National Park moss dominates sheltered
  niches and lichens drier, sunnier places higher up the trunk, with zonation by both aspect and
  height ([Killarney, UEA](https://research-portal.uea.ac.uk/en/publications/factors-influencing-epiphytic-moss-and-lichen-distribution-within/));
  on Queen Charlotte Islands conifers a mat-forming moss was abundant at tree bases and not higher
  ([*The Bryologist* 105(3), 2002](https://bioone.org/journals/the-bryologist/volume-105/issue-3/0007-2745(2002)105[0451:BEOTCS]2.0.CO;2/Bole-Epiphytes-on-Three-Conifer-Species-from-Queen-Charlotte-Islands/10.1639/0007-2745(2002)105[0451:BEOTCS]2.0.CO;2.full)).
- **Crustose lichen** takes the exposed rock and bark the moss leaves: light, drought-tolerant,
  outcompeted where it is damp. It is **slow**: *Rhizocarpon* margins extend 0.02 to 2 mm a year
  across studies, typically 0.1 to 0.7 (Bradwell and Armstrong's Iceland thalli 0.64 mm/yr;
  Armstrong's Washington talus 0.07 mm/yr; [Armstrong and Bradwell's review](https://research.aston.ac.uk/en/publications/growth-of-crustose-lichens-a-review)).
- **Algae** (*Desmococcus*, *Trentepohlia*) make the green or orange film on damp, shaded bark and
  stone, smooth as well as rough, and arrive within a few years.
- **Fungi** fruit on **dead and dying wood**: bracket fungi on a trunk's or a log's side
  (*Trametes versicolor* caps **3 to 10 cm** wide:
  [North Carolina fungi database](https://ncrfungi.uark.edu/species/2_trametesVersicolor/trametesVersicolor.html),
  [Naturalis](https://mushrooms.linnaeus.naturalis.nl/linnaeus_ng/app/views/species/taxon.php?id=100808)),
  mushrooms on its top and at its foot.
- **Cushion moss** (*Grimmia pulvinata* on wall tops and rocks) forms cushions of about **2 to 10
  cm** (a species guide; the cushion's size sets its water relations: Zotz et al. 2000,
  [*New Phytologist*](https://jetzlab.yale.edu/publications/water-relations-and-carbon-gain-are-closely-related-cushion-size-moss-grimmia-pulvinata)).

### What the graphics literature does

Dorsey and Hanrahan, "Modeling and Rendering of Metallic Patinas" (SIGGRAPH 1996) and the
weathering line after it solve a surface's coating from its exposure; Desbenoit, Galin and
Akkouche, "Simulating and Modeling Lichen Growth" (*Computer Graphics Forum* 23(3), Eurographics
2004), grow lichen by an open diffusion-limited aggregation over the substrate constrained by its
environment and render it by instancing 3D lichen shapes from an atlas — the mask and the shaped
pieces here, with the aggregation replaced by a seeded noise for the colonies' patches (below).

### The rules (cover.md has them as a table)

Each holding triangle's **damp** is the climate's wetness, the base (within half a metre), shelter
(one minus the sky the surface sees within the crevice radius), the north face and the water's
nearness, less the sun. **Moss** is damp × shade × a face that holds water (a top best, a wall well,
an overhang poorly) × a rough substrate; **crustose lichen** is light × dryness × substrate; **algae**
damp × shade on anything; **fungi** damp × shade on dead wood only. Each is scaled by how far the
kind has come in the surface's age, 1 − exp(−age / τ), and moss takes its share first, lichen and
algae what it leaves.

| Constant | Value | From |
|---|---|---|
| τ moss | 8 years | **chosen, not measured**: moss cover on a new wall or stone within a decade or two; Glime's succession chapters are where to verify it |
| τ crustose lichen | 40 years | 0.1–0.7 mm a year of margin: a thallus a few centimetres across takes decades (lichenometry, above) |
| τ algae | 2 years | **chosen**: a film in a few wet seasons |
| τ fungi | 3 years | **chosen**: fruiting within a few years of a tree's death |
| base reach | 0.5 m | the splash and ground-moisture zone; the Queen Charlotte bases |
| water reach | 5 m | a stream's or a pool's humidity |
| crevice radius | 0.6 m | the shelter a surface sees: a crevice between two blocks, a buttress's foot |
| cushion radius | 2.5 cm | Grimmia's 2–10 cm cushions across |
| bracket reach | 5 cm | Trametes' 3–10 cm caps |
| patchiness, patch size | 0.7, 0.35 m | colonies in patches of tens of centimetres; chosen by eye, Desbenoit's aggregation is the grounded replacement |

## The overgrown scene: what it costs

`content/test-scenes/overgrown/`: a granite boulder from the rock solver (19,200 triangles), a 10 m
wall of the ruins' kit of boxes (12 triangles), and the oak from the flora solver (50,340), each with
ivy and with cover, every companion placed with its host's transform; ten instances with the
terrain, 7,294 pairs.

| Companion | Triangles | Clusters | What the solver made |
|---|---|---|---|
| ivy on the boulder | 1,610 | 68 | 3 seeds, 8 stems, 10.9 m of stem, 219 leaves, every step held |
| cover on the boulder | 5,223 | 159 | lichen 0.24 of the holding area, algae 0.18, moss 0.13 at 60 years |
| ivy on the wall | 53,082 | 2,155 | 6 seeds, the walk more random and branchier than the default |
| cover on the wall | 9,249 | 352 | the 12-triangle wall split to 25 cm for the mask: lichen 0.40, moss 0.17 at 120 years |
| ivy on the oak | 21,612 | 877 | 4 seeds, 84 stems, 150 m of stem up the trunk and into the limbs, tips at 7.1 m |
| cover on the oak | 1,787 | 70 | moss 0.17, lichen 0.13 on the bark and the load-bearing limbs |

**At 1080p** along the camera path (30 s, 1,801 frames, `msvc-debug`, RTX 5090, traced shadows,
2026-10-21): GPU total **0.54 ms median, 0.72 ms p95**; frame 0.83 ms median; the deform pass
0.022 ms (every companion rides the motion contract, at zero weight where it holds). The machine
was **not quiet**: `machine_state` recorded 50% CPU from others and the GPU at 6–7% from others, so
these are upper bounds. Building the six companions in `msvc-debug` took 0.2 to 7 s each, the
oak's dominated by solving the oak itself as the host (4.9 s).

**Where the budget was traded.** Ivy on a 10 m wall at the random walk the wall recipe asks for is
53,000 triangles — 80 m of stem, its 1,600 leaves two triangles each and its tubes five sides
round. The leaves are the cheaper half; a wall seen from across a street wants fewer, larger
leaves, which is what the budget's ladder (`triangle_budget`: coarser rows, then one leaf in two)
does when a recipe asks for less. The cover's film costs a triangle per sampled triangle wherever
the mask is above its threshold, and it is the stand-in for R90, which would cost nothing but a
second material read.
