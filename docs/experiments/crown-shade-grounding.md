# A crown's shade: what is published, and the numbers taken from it

Roadmap R69, stage 7 (2026-11-12). The grounding for the crowns' priors
([renderer](../subsystems/renderer.md#stage-7-a-crowns-shade-as-a-prior),
`domain/gfx/crown_prior.h`): the light inside a tree's crown, as the literature on canopies has it,
what the tree solver's own crowns hold, and what was chosen. The measurements the stage was judged
by are on [the cache's page](gi-cache-2026-11-07.md#stage-7-a-crowns-shade-as-a-prior).

## Why a prior at all

Stage 6 counted what the cache's coarse cells hold of a crown: the same light as the fine ones,
**flattened** — the mean of leaves in the open and leaves in the shade. What is fine inside a crown
is the crown's own shade, and that is known from the tree alone, before a ray is traced. Take it
out of what the cache holds and what is left — the light arriving at the crown from the sky and
the stand round it — is smooth, so a coarse cell holds it well, fills in a frame and is read alone.

## The sources, and how far each was read

Read for this stage on 2026-11-12, through a search engine's summaries and a page reader (no file
was opened but the pages named); a book is cited for what the secondary sources say it holds, and
says so.

| Source | What it gives | How far read |
|---|---|---|
| Monsi and Saeki 1953, "Über den Lichtfaktor in den Pflanzengesellschaften und seine Bedeutung für die Stoffproduktion", Jpn. J. Bot. 14:22–52 | Light under a cumulative leaf area index F falls as I = I₀ e^(−K F); K is 1 for horizontal leaves and falls to about 0.44 for leaves standing at 90° | **Not opened** (German, not online). The law and the two values as the search's summary quotes them from secondary sources (a review in J. Forest Research and Kyushu University reprints) |
| Ross 1981, *The Radiation Regime and Architecture of Plant Stands* | The projection function G(θ): the share of unit leaf area a plane across the path sees; the gap fraction along a direction is exp(−G(θ) Ω L / cos θ) | **Not opened** (a book). The form as Chianucci et al. state it ("Ross-style"), from the search's summary |
| Campbell and Norman 1998, *An Introduction to Environmental Biophysics*, ch. 15 | The ellipsoidal leaf angle family; the spherical member's G = 0.5 in every direction, so a beam's extinction per unit leaf area index is 1 / (2 cos ψ) | **Not opened** (a book). G = 0.5 for the spherical distribution as Yan et al. 2021 and the summary of Pisek et al. state it |
| Yan et al. 2021, J. Remote Sensing, on the G-function in coniferous canopies | "G is often set to 0.5 … only valid for spherical" leaf angles; a constant G costs up to 53 % of a retrieved leaf area at the zenith and under 10 % between 45° and 65° | Abstract and the summary's quotations |
| Pisek et al. 2013 (and the 2012 AGU abstract), "Is the spherical leaf inclination angle distribution a valid assumption for temperate and boreal broadleaf tree species?" | Measured on 58 (later over 80) broadleaf species: most are **not** spherical; planophile or plagiophile is the better default | Abstract |
| Chianucci et al. 2018, Annals of Forest Science | P(θ) = exp(−G(θ) Ω L / cos θ); the clumping index Ω as effective over true leaf area | The summary's quotation of the model |
| Hosoi and Omasa 2009 (ISPRS laser scanning proceedings) and a 2020 comparison of indirect methods | Leaf area **density** of single crowns from terrestrial lidar voxels; the methods hold for densities under 3 m²/m³ and voxels of 0.3 m | Titles and the summary only: the proceedings' PDF did not render as text, so **no measured profile was read** |
| A JRC study of savanna broadleaf trees by terrestrial lidar against harvested leaves | Single trees of 30 to 530 m² of leaf, crown leaf area index 0.8 to 7.2 | The summary |
| SpeedTree Modeler manual, "Ambient lighting" and the glossary (Unity's hosting of it) | Ambient occlusion is computed for the tree by the tool and "darkens crevices and the interior of a tree"; "the interior of the tree should be dark but not black" and the outside white; `Brightness`, `Contrast`, `Min`, `Max`; an occlusion map refines it per material | The page, through a reader. It does **not** say the result is stored per vertex; a forum answer (Unreal's) says the exported tree carries it as the vertex colour, which is the common knowledge and is not documented on the page read |
| GPU Gems 3, ch. 4, "Next-Generation SpeedTree Rendering" | Leaf lighting in a shipped tree renderer | **Not opened**; listed by the search |

What this does not have: a measured leaf area density profile for an oak or a spruce read at
first hand, and Ross's and Campbell and Norman's own pages. The numbers below that lean on them
say so.

## The numbers, each against its source

- **Beer-Lambert in leaf area along a path**: the fraction let through a path of s metres in
  foliage of u m² of one-sided leaf a cubic metre is **exp(−G u s)** — Monsi and Saeki's law with
  the leaf area index written as density times path, which is Ross's form for a direction.
- **G = 0.5** (`k_crown_g`): the spherical leaf angle distribution's, the one value that does not
  depend on direction. Pisek et al. say real broadleaves are flatter than that (a planophile
  crown shades more from above and less from the side, G towards 1 at the zenith and under 0.5 at
  the horizon); Yan et al. say the same of conifers' shoots. The spherical value is kept because
  the grid is marched in every direction with one G, and a per-species leaf angle is the solver's
  to hand over when it has one — the page of the tree solver records none. **This is the model's
  largest stated error**: tens of percent of optical depth along a single direction, less in a
  hemisphere's mean.
- **No clumping index**: Ω stands for leaves gathered into shoots and crowns where a model spreads
  them evenly. The grid is built from the tree's **own leaves** in cells of a quarter metre to a
  metre, so clumping at the scale of a branch and above is in the grid itself. Clumping inside a
  cell (a conifer's needles on a shoot) is not: the solver's cards stand for shoots, and a card's
  share that is leaf is the next number.
- **A card's share that is leaf, 0.7** (`k_crown_card_fill`): the solver places
  `leaf_area_index × footprint ÷ (leaf_width × leaf_length²)` cards ([flora](../subsystems/flora.md),
  "The budget of a crown"), so a card of `leaf_length²` carries `leaf_width` of it as blade, 0.7 on
  the default broadleaf. One constant for every species: a species whose `leaf_width` is far from
  it has its density off by that ratio, and the solver's own number per mesh is the mend.
- **Diffuse light is a mean over directions, not a beam** (the brief's third point; Campbell and
  Norman's diffuse extinction is the hemisphere's integral of the beam's): the prior is the mean of
  exp(−G u s) over a hemisphere's directions, **two** of them a point — the upper hemisphere and
  the lower — because a leaf's light comes from the hemisphere about its own normal, of which
  (1 + n.y) / 2 is above the horizon (`gfx::crown_prior`). Twelve directions a hemisphere, three
  rings of equal solid angle by four spokes: against a 64 × 128 quadrature of the closed form in a
  ball of uniform leaf the grid stands 0.012 off at most over seven points
  (`gi cache key: a crown's prior in a ball of uniform leaf is Beer-Lambert's closed form`).
- **The floor, 1/16** (`k_crown_floor`): past an optical depth of 2.8 the light deep in a crown is
  the leaves' own scattering more than the sky's, which no transmitted fraction describes; and a
  cell's count is kept in sixteenths of a sample. Below the floor the interior reads as the floor:
  brighter than the model, which is the side shipped tools err on ("dark but not black").
- **The grid**: 16 cells an axis at most, a quarter metre at least (`k_crown_cells`,
  `k_crown_cell_min_m`), two bytes a cell, 8 KiB a mesh at most; a mesh whose leaves stand under
  1.5 m gets none (`k_crown_min_height_m`: a sward and a litter of leaves are shaded by their own
  solvers' ground images, [flora](../subsystems/flora.md), "Near and far").

## What the tree solver's crowns hold (counted)

The load logs each mesh's leaf (`crown prior`, `--log info`): the cards' area times 0.7, and the
volume of the grid cells that hold a leaf. The grove's species, 2026-11-12:

| mesh | leaf triangles | leaf area, m² | leafy cells, m³ | density, m²/m³ | mean prior over the leaves | least |
|---|---|---|---|---|---|---|
| oak (`engine.flora.Broadleaf`) | 40,428 | 397.5 | 872.0 | **0.46** | 0.41 | 0.125 |
| spruce, tall (`engine.flora.Conifer`) | 16,124 | 405.4 | 275.3 | **1.47** | 0.20 | 0.029 |
| spruce, young | 2,128 | 34.3 | 8.6 | **4.0** | 0.29 | 0.039 |
| date palm | — | — | — | — | no grid | |

The oak's 0.46 m²/m³ is inside what the lidar studies treat as ordinary (their methods hold to
3 m²/m³); the young spruce's 4.0 is past it — a small dense cone whose cards overlap — and its
interior sits on the floor. The palm has no grid: its fronds are not alpha-tested cards, and its
leaves read as everything else does. Two rules keep what is counted leaf: a distance form reads
its source's grid (its own finest cards are merged ones), and a mesh whose alpha-tested
triangles average over 0.05 m² — a far forest's impostors — has none (`k_crown_card_max_m2`);
counted as leaf, those gave the forest crowns of 10 to 170 m² a cubic metre. The forest's crowns are on
[the cache's page](gi-cache-2026-11-07.md#stage-7-a-crowns-shade-as-a-prior) with its flight.

## What shipped foliage does, and why this is not that

The industry's practice is occlusion computed once by the tree tool and carried by the tree:
SpeedTree computes it for the model, darkening "the interior of a tree", with a floor and a
ceiling the artist sets. It is carried per vertex. Here it is carried **per mesh, as a grid**:

- it costs no byte of any vertex (a byte a vertex is an eighth more attribute data on every vertex
  of the forest, foliage or not, and the position stream's 16-bit grid and the attributes' eight
  bytes have none to spare);
- it is the same field for every LOD of the mesh — a far crown's merged cards read what the near
  crown's leaves read — where a vertex attribute has to survive the simplifier;
- a trunk under the crown and a twig inside it can read the same field, with no seam of their own;
- it is derived from the built mesh at load (the leaves' triangles), so no container section, no
  cache key and no cross-toolchain pin moves; the arithmetic is still IEEE and `engine::det`, and
  a CPU case pins a grid's hash.

What it gives up: detail under a cell's size (0.6 m in a 10 m crown), which is where a single
leaf's own shadow on its neighbour lives, and which the direct light's traced shadow already draws.

## Where the budget ruled

- Twelve directions and a half-cell step, at load, on one thread, once: 0.9 s for the forest's 93
  crowns (7.8 million leaf triangles). The first build marched every alpha-tested mesh, 5,027 of
  them, in 44 s; the march belongs in the tree's content build, with the solver's own leaf area,
  when a world has thousands of species.
- One G and one card fill for every species (above).
- The prior is the tree's **own** crown only. A neighbour's shade is the smooth field the cache
  holds; a stand so dense that crowns interlock is still described crown by crown.
