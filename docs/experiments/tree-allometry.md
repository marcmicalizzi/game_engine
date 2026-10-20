# Tree allometry: branching that follows the research (G73, 2026-10-20)

**What prompted it.** The owner's first walk under the [elder grove](../../content/test-scenes/elder-grove/README.md)
(2026-10-20) found the leaves enormous. The ancient tree was the broadleaf scaled up: six fixed
orders whatever its height, twigs 5 m long carrying sixteen cards a side 1.4 m long, opaque and
untextured, so each card read as one leaf longer than a person's arm. The owner's rule, which is
also botany's: **a very old, very large tree has the same leaves as a young one of its species,
many more of them, on many more orders of smaller branches off the larger ones**, and the structure
should come from what is known about tree form rather than from more tuning. This page is that
grounding: the model and its sources, the constants chosen and why, what the default recipes come
to, where the triangle budget forced a trade, and what the AAA pipelines do with a million leaves.
The rules that follow are in [flora](../subsystems/flora.md#allometry); this page is why.

## The model and its sources

| Rule | Source | What it says | What the solver does with it |
|---|---|---|---|
| **The pipe model; da Vinci's rule** | Leonardo's notebooks; Shinozaki, Yoda, Hozumi and Kira 1964 (*Jpn. J. Ecol.* 14, the pipe model theory); Eloy 2011, [*Phys. Rev. Lett.* 107, 258101](https://link.aps.org/doi/10.1103/PhysRevLett.107.258101) ([arXiv 1105.2591](https://arxiv.org/pdf/1105.2591)) | At a branching node the children's cross-sections sum to the parent's, d^Δ = Σ d_i^Δ, Δ = 2 in Leonardo's form; measured species fall in **1.8 < Δ < 2.3** (Eloy's survey of the earlier data), and a self-similar skeleton whose branches are sized to resist wind recovers the rule. Shinozaki's reading: a tree is a bundle of pipes, each serving a unit of leaf, and the sapwood area below a point is proportional to the leaf area above it. Murray's Δ = 3 is the vascular law of single conduits, not of wood. | `area_exponent` (default 2.0, range 1.8–3): every node of the recursion, R^Δ = Σ r^Δ exactly (the test holds every one of the default ancient tree's 71,597 nodes to 2e-7). |
| **A species-constant terminal unit (Corner's rules)** | Corner 1949; Olson, Aguirre-Hernández and Rosell 2009, [*Ecology Letters* 12](https://www.explorelifeonearth.org/people/OlsonAguirreRosell2009CornersRules.pdf); the WBE model's invariant petiole ([Savage et al. 2010](https://sperry.biology.utah.edu/publications/Savage%20et%20al%202010%20PNAS.pdf) restates it) | Leaf size scales with the twig's thickness — larger leaves on stouter twigs, wider spaced (axial conformity) — and the terminal twig and its leaf are a property of the species, not of the plant's size: a seedling's and a giant's are the same. | `twig_radius_m`, `twig_length_m`, `leaf_length_m`, `leaf_width` are species constants that never scale with `height_m`. The recursion stops where a shoot's thinnest child would be thinner than the twig. |
| **Self-similar orders (Horton–Strahler)** | Horton 1945, Strahler 1957; Leopold 1971, ["Trees and streams: the efficiency of branching patterns"](https://www.usgs.gov/publications/trees-and-streams-efficiency-branching-patterns), *J. Theor. Biol.* 31 | Branch counts fall geometrically with order, by a nearly constant bifurcation ratio Rb, in trees as in rivers; natural networks sit at Rb ≈ 3 to 5 ([Strahler number](https://en.wikipedia.org/wiki/Strahler_number)). We found no table of measured Rb by tree species and say so: 4 is the middle of the networks' range and the value WBE's length rule is usually quoted at. | `branching_ratio` (default 4): a shoot of the recursion carries Rb − 1 laterals and a leader. |
| **Space filling and area preserving (WBE)** | West, Brown and Enquist 1999, ["A general model for the structure and allometry of plant vascular systems"](https://www.nature.com/articles/23251), *Nature* 400 | Radii fall by n^(−1/2) a level (area preserving), lengths by n^(−1/3) (volume filling): 0.63 for four children. Tests since ([Price, Wing and Weitz 2011; Bentley et al. 2013](https://arxiv.org/pdf/1507.07820)) found branching **area preserving** within trees and branch **lengths not** following the volume-filling exponent, which changes across orders. | `length_ratio` (default 0.63): a child's length over its parent's — the one rule of the set the data say holds only roughly, so it is a field. The terminal twig's length is the species', whatever the ratio would have made it. |
| **Height on diameter (elastic similarity)** | McMahon 1973, ["Size and shape in biology"](https://www.sccs.swarthmore.edu/users/08/bblonder/phys120/docs/mcmahon.pdf), *Science* 179; McMahon and Kronauer 1976; Niklas 1994, *Plant Allometry* | A trunk just thick enough not to buckle under its own weight has D ∝ H^(3/2), H ∝ D^(2/3); measured slopes scatter about it and run steeper at small sizes ([O'Brien et al. 1995](https://repository.si.edu/items/30c5c1c2-5a77-4269-9f71-4d2814ce6eee)). | The broadleaf's `allometry_exponent` (default 2/3): a `trunk_radius_m` of 0 is 0.35 m × (H / 12 m)^(3/2), calibrated to the old default oak (open-grown, stout). The ancient tree sets its trunk outright: its buttressed core is the exception the recipe states. |
| **Crown spread with size** | WBE; O'Brien et al. 1995 | Crown radius ∝ D^(≈2/3) across trees (0.6–0.8 measured). | Already true of the broadleaf: its limbs are a share of the height (`limb_length`), so crown ∝ H ∝ D^(2/3). The ancient tree's crown is its limbs' reach, a recipe's. |
| **Leaf area by the leaf area index** | Asner, Scurlock and Hicke 2003, *Global Ecology and Biogeography* 12 (the global synthesis; [ORNL data set 584](https://commons.datacite.org/doi.org/10.3334/ORNLDAAC/584)); [BioNumbers 113517](https://bionumbers.hms.harvard.edu/bionumber.aspx?id=113517) | LAI, one-sided leaf area over ground area: deserts 1.3, temperate evergreen forests 5.1–6.7, closed forests generally 3–7; Körner gives about 5.5 for mixed temperate deciduous forest. These are stands; an open-grown tree's LAI over its own footprint is of the same order. | `leaf_area_index`: the crown's footprint (the convex hull of every shoot tip, seen from above) times the LAI over one card's area is the card count, spread over the twigs as evenly as integers go. The oak's 4 is the middle of the broadleaf range; the ancient tree's 3 is its low end, chosen for the budget (below). |
| **Shedding and apical control with age** | Hallé, Oldeman and Tomlinson 1978, *Tropical Trees and Forests: An Architectural Analysis* | A young crown is conical under a dominant leader; with age apical control weakens, the crown flattens and spreads, and the lowest limbs are shed. | `leader_share` (the leader's share of a node's area: the oak 0.3, the ancient tree 0.28, near the even 0.25 of an old crown) and the crown's base (`crown_base`, `limb_low_m`), both the recipe's. Deriving them from the tree's age is G74's. |

## The constants per species

| | Broadleaf (oak) | Ancient tree | Species constant? |
|---|---|---|---|
| `twig_radius_m` | 0.004 | 0.006 (Corner: a 0.3 m leaf needs a stout twig) | yes |
| `twig_length_m` | 0.7 (was 0.9) | 0.8 (was 5) | yes |
| `leaf_length_m` | 0.24 (a large oak leaf: *Q. macrocarpa*'s reach 0.3 m) | 0.3 (was 1.4) | yes |
| `leaf_width` (of the length) | 0.5 | 0.6 | yes |
| `area_exponent` | 2.0 | 2.0 | yes |
| `branching_ratio` | 4 | 4 | yes |
| `length_ratio` | 0.63 | 0.63 | yes |
| `leader_share` | 0.3 | 0.28 | recipe (G74 would derive it) |
| `leaf_area_index` | 4 | 3 | yes |
| trunk radius | allometric, 0.35 m at 12 m | 5 m, the recipe's | scales |
| `triangle_budget` default | 60,000 | 1,500,000 (was 300,000) | — |

The old fields that set the canopy become caps, so every committed recipe still reads: the
broadleaf's `twigs` and the ancient tree's `sub_branches` and `twigs` cap a shoot's laterals,
`leaves` caps a twig's cards a side, and `branches` is the limbs' own laterals.

## What the default recipes come to

From `tests/allometry_tests.cpp`, `engine-content flora` and the species tests, msvc-debug,
2026-10-20:

| | Default broadleaf, before → after | Default ancient tree, before → after |
|---|---|---|
| Orders | 4 → 8 | 6 → 12 |
| Terminal twigs | 441 → 3,384 | 1,800 → 214,834 |
| Shoots of the structure | 498 → 4,501 | 2,179 → 286,441 |
| Motion parts | 498 → 230 | 2,179 → 972 |
| Cards (each one leaf) | 10,584 cards of 0.24 m → 16,638 of 0.24 m | 57,600 cards of 1.4 m → 564,920 of 0.30 m |
| Crown footprint | — → 119.8 m² | — → 10,169 m² (a 57 m radius) |
| Realised LAI (mesh area over the leaves' hull) | — → 3.67 (the solver's own, over the tips' hull, 3.90) | about 1.9 (57,600 cards of 1.4 × 0.31 m at sin 50° over a footprint like today's) → 2.90 (solver 2.93) |
| Triangles | 27,356 → 49,157 (bark 15,881, leaves 33,276) | 196,301 → 1,419,335 (bark 289,495, leaves 1,129,840) |

The order count follows the sizes as the formula says: the broadleaf at 6, 12 and 24 m (trunk radius
0.12, 0.35 and 0.99 m by the allometry) has 6, 8 and 9 orders against 1 + log₄((R/r_twig)²) = 5.95,
7.45 and 8.95, and 495, 3,384 and 30,447 twigs, about half of (R / r_twig)² because a terminal twig's
radius lies anywhere in its pipe step (up to 2.07 r_twig) rather than at r_twig. The same species at
8 and 32 m draws cards of 0.240 m on both, and the ancient tree at 45 and 100 m cards of 0.300 m on
both.

**Why the pipe count is smaller than the trunk says.** The ancient tree's core is 5 m in radius, but
its limbs and its trunk's crown together carry the area of a 3.9 m one; the rest is the heartwood of
Shinozaki's disused pipes — the limbs shed below `limb_low_m`, whose pipes stay in the trunk. So
the recursion starts at the limbs, from the recipe's limb radius, and the trunk's core is a recipe's
number, as a giant's is.

## The budget

A real crown cannot be meshed leaf by leaf: the default ancient tree's 214,834 twigs and 564,920 cards
are 1.42 million triangles, the oak's 16,638 cards 33,276. Where the budget forces it, the solver
keeps the leaves' size and trades in this order ([flora](../subsystems/flora.md#the-budget-of-a-crown)):
the finest wood first (shoots of a centimetre or two are under a pixel past a few metres), then
rows and cross-sections, then the finer orders' wood folded into the parts they ride, and only then
the cards **merged up the hierarchy, finest first**: at merge level m every twig's cards gather on
its ancestor m orders up into one spray whose cards are as long as that sub-tree reaches — a
branch-group card. No rung changes a shoot, a part or the structural table.

| Ancient tree, budget | Triangles | Rungs | Merge | Longest card |
|---|---|---|---|---|
| 1,500,000 (default) | 1,419,335 | 0 | 0 | 0.30 m (the leaf) |
| 1,000,000 | 449,070 | 9 | 1 | 1.22 m |
| 250,000 | 140,410 | 10 | 2 | 2.52 m |
| 100,000 | 57,642 | 11 | 3 | 4.27 m |
| 40,000 | 34,946 | 12 | 4 | 7.29 m |
| 20,000 (the floor) | 15,964 | 27 | 12 | 8.28 m |

So the **ninth rung starts merging**, on any budget under about 1.15 million triangles, and its
card stands for a twig's sub-tree one order up: about 1.2 m. For the oak the third: 40,000 holds
38,396 at merge 0, and 25,000 merges to 13,576 triangles of 1.04 m cards. The default ancient
budget was raised to **1,500,000** because that is what holds the 100 m tree's leaves at their own
size with 6% to spare, and the elder grove affords two of them (below). `ancient-1m.flora.json`
(110 m, a million triangles) now sits on the first merged rung, 454,886 triangles of 1.22 m cards,
and is kept as that rung's example.

**Merged cards are where alpha-masked cluster textures belong** (R65): a branch-group card drawn
opaque is a green parallelogram a metre or more long, the very thing the owner objected to; with an
image of the sub-tree's leaves cut by its alpha it reads as foliage. That image should be **baked
by the content build from the solver's own finest geometry** (R89), so a far card is the near twigs
seen from afar and no lab image is needed.

## What it costs

**The elder grove at 1080p** (`engine-view --scene content/test-scenes/elder-grove/scene.json
--camera-path …/camera-path.json --offscreen --width 1920 --height 1080 --repeat 1 --warmup 60
--wait-quiet 60`, msvc-debug, RTX 5090, traced shadows, 2,281 frames, 2026-10-20). Neither run found
a quiet machine: before, others used 37.8% of the CPU and the GPU was 99% busy at the end; after,
31.6% and 39%. Both are upper bounds.

| | Before (main, 61e7ca55) | After |
|---|---|---|
| Elder A, elder B | 196,301 + 176,912 triangles | 1,419,335 + 1,050,182 |
| Scene clusters | 14,065 | 121,211 |
| GPU total ms, median / p95 / p99 | 0.78 / 1.03 / 3.06 | 2.02 / 2.35 / 2.62 |
| of which rt (BLAS + trace), CLAS, raster, resolve, pool pass | 0.35, 0.14, 0.035, 0.27, 0.070 | 1.27, 0.98, 0.078, 0.37, 0.081 |
| Visible pairs, shadow casters (median) | 1,717, 1,504 | 22,649, 20,010 |
| Deformed-vertex pool (the whole meshes) | 1.22 M vertices, 14.6 MB | 7.79 M vertices, 93.5 MB |
| Motion parts in the scene, motion stream | 9,076, 3.4 MB | 6,583, 29.7 MB |
| Frame thread (debug) median | 10.0 ms | 7.2 ms |
| GPU memory the engine used | 359 MiB | 1,096 MiB |

The GPU's two milliseconds are the cut: thirteen times the visible clusters, because leaves at
their own size are detail the 1-pixel threshold keeps drawing where 1.4 m cards collapsed early,
and the ray-tracing chain builds a cluster structure for every one of them. The frame thread is
faster because the parts fell (the twigs are no longer parts of their own). The pool is still under
the flora floor of 96 MiB (`renderer.flora.pool_mib`). **The container build** is the price at
content time: the default ancient tree takes 48.8 s in msvc-debug (68,013 clusters, 17 levels,
4,167,147 vertices) and the rising one 30.0 s, where they took 2.6 s.

**The flora stage's depth.** The renderer composes every part's frame parent-first on the CPU, so a
vertex's cost does not depend on its part's depth ([renderer](../subsystems/renderer.md#the-flora-stage)) and
there is no bound to raise: the deepest part chain is now nine parts (trunk, run, limb, six
recursion orders), and the pool pass went from 0.070 to 0.081 ms for six times the vertices. The
padding the stage sums over a chain fell from 14.0 m to 11.8 m. A part id is a `u16` per vertex, so
the 2,000-part cap keeps every tree far inside it.

**The collision proxy across the ladder** (the owner's addendum, measured, not changed). The
proxy is the mesh's triangles by part ([scene_collision](../subsystems/scene_collision.md)), so a
walker on a lower rung stands on a coarser tube. Between the default ancient tree's finest rung and
its floor (20,000, 27 rungs) the farthest a sample of either surface lies from the other is **0.12 m
on the runs and 0.44 m on the limbs past them**; between the finest and the 40,000 rung, 0.031 and
0.098 m (`tests/allometry_tests.cpp`, the symmetric distance over vertices, edge midpoints and
centroids). A walker on a limb would stand up to 44 cm off where another preset stands it, which is
the case for building the proxy from the structural table's tubes rather than the triangles.

## What the AAA pipelines do with a million leaves

- **SpeedTree** (the industry's tree middleware; [its runtime documentation](https://docs.unity3d.com/speedtree-runtime-sdk/manual/level-of-detail.html),
  [glossary](https://docs.unity3d.com/speedtree-modeler/manual/glossary.html)): near, leaf
  meshes (small pieces of geometry per leaf); then leaf cards — textured quads standing for a
  cluster of leaves; branches as geometry and **fronds** (alpha-cut planes standing for a whole
  branchlet with its leaves); leaf LOD by shrinking some cards away and growing the rest; and far, a
  **billboard** — one image, or several taken round the tree, chosen by the view's azimuth — cross
  faded in.
- **Octahedral impostors** (Brucks 2018, "Octahedral Impostors", shaderbits.com, the Fortnite
  trees): the tree rendered from a hemisphere or sphere of directions into an atlas laid out on an
  octahedron, blended between the three nearest views, with depth for parallax — a whole-tree
  impostor that holds from any elevation, which the azimuth-only billboard does not.
- **Billboard clouds** (Décoret, Durand, Sillion and Dorsey 2003, "Billboard clouds for extreme model
  simplification", SIGGRAPH): a set of textured planes fit to the geometry, view-independent;
  the [foliage mode of Simplygon's](https://documentation.simplygon.com/overview/concepts/impostor.html)
  impostors fits them through the whole volume of a crown; [displacement-mapped billboard
  clouds](https://www.cg.tuwien.ac.at/research/publications/2007/TR-186-2-07-01/TR-186-2-07-01-paper.pdf)
  (Mantler, Jeschke and Wimmer) add parallax. These are the hierarchical, branch-level impostors
  of the literature.
- **Nanite foliage** (Unreal Engine 5.7, [Epic](https://www.unrealengine.com/news/highlights-from-the-unreal-fest-stockholm-opening-session),
  experimental): Nanite assemblies (instanced branches), Nanite skinning for wind, and **Nanite
  voxels** for distant trees — no authored LODs, no cross fades; practitioners still pair Nanite
  wood with masked leaf cards near.
- **The owner's suggestion** — sprites for branches and leaves far, layered enough that the eye
  cannot tell, on exceptionally large trees (hundreds of feet tall and wide) — is the billboard
  cloud applied per branch group, and at a 100 m tree's scale it is the right one: a single
  whole-tree impostor is too coarse when the tree fills half the sky and the viewer stands under it.

**Which of these this engine already is.** The cluster DAG is the continuous geometric LOD that
SpeedTree's rungs approximate; the merge rule above is the **branch-group card** (SpeedTree's frond,
the billboard cloud's plane), placed by the solver's own hierarchy and sized to its sub-tree. What
each step needs:

1. **Alpha-masked rasterization (R65)** on every path — the three rasterizers, the visibility tie
   rule and the ray and shadow tests honouring the mask for every card and impostor, or a card's
   shadow is its parallelogram.
2. **Baked card textures (R89)**: the content build renders the finest structure's sub-trees into an
   atlas of branch-group images, colour, normal and alpha, keyed like any derived texture, so a
   merged card at any rung is that sub-tree's own picture.
3. **Octahedral whole-tree impostors (R89)** for the far field beyond where the DAG's coarsest cut
   is still more than a few hundred triangles a tree: for the elder grove's 100 m trees from a
   kilometre or so, for an oak from about 300 m.

**Where each rung would take over, the elder grove at 1080p** (a 60° vertical field, so a pixel at
distance d is 1.0 mm × d): a 0.30 m leaf is 3 pixels at 100 m and 1 at 300 m, so leaves at their own
size hold to about 300 m from elder A; a 1.22 m merge-1 card is a pixel at 1.2 km and a 4.3 m
merge-3 card at about 4 km; and the 115 m crown is 115 pixels across at a kilometre, where an
octahedral impostor of 128 pixels a view would replace it.

## G74: what self-organising growth would add

The recursion here is **architectural**: its branching follows from sizes and ratios, not from the
light. Palubicki, Horel, Longay, Runions, Lane, Měch and Prusinkiewicz 2009 ("Self-organizing tree
models for image synthesis", SIGGRAPH) grow a tree bud by bud, each bud's fate — grow, wait or die —
decided by the light it receives and a signal from the root (the extended Borchert–Honda model),
and Runions, Lane and Prusinkiewicz 2007 ("Modeling trees with a space colonization algorithm")
grow shoots towards free space. Either would add what this one fixes by hand: crowns that fill the
light they have and leave gaps where a neighbour shades them, the young-to-old shape change
(apical control and shedding as outcomes, not fields), asymmetric crowns at a forest's edge, and
reiteration — an old crown rebuilding itself from big limbs, Hallé's model. It would cost: the
branch count known only after growing (the budget then prunes, and a pruned branch moves every
motion id after it unless ids are assigned after the prune), a light field per growth step
(Palubicki's shadow grid, a few thousand voxels a tree, tens of growth cycles), the determinism
discipline over nearest-point searches ordered by float distances, and minutes rather than seconds
for a 100 m tree's 200,000 twigs. It is roadmap G74, proposed.
