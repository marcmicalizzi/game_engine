# The birch: a fuller crown and seeded clumps (G81, 2026-10-22)

**What prompted it.** The owner's forest stress test (`D:\workspace\forest_wildlife_poc\`, three
birch recipes of `stems: 1`, `limbs: 14`, `droop: 0.8` on a 36,000-triangle budget) showed the
birches **too sparse**: a few long limbs carrying strings of leaves, sky everywhere through the
crown, where a silver birch has a full, fine-twigged ovoid crown. The second ask: **a birch that
comes up naturally or in a forest has a decent chance of several trunks.** Version 1 of
`engine.flora.Birch` was three named orders — stems, 12 limbs, 12 twigs a limb — and 12 cards a
side on each twig: 312 twigs and 7,488 cards of 9 cm on the default, a realised leaf area index
well under 1. This page is the grounding for version 2: the sources, the constants chosen, where
the triangle budget forced a trade, and what the captures showed. The rules that follow are in
[flora](../subsystems/flora.md#the-birch).

## The model and its sources

The crown is the [allometric recursion](tree-allometry.md#the-model-and-its-sources) the broadleaf,
the ancient tree and the shrub already grow — the pipe model, a species-constant terminal unit
(Corner's rules), Horton–Strahler orders, WBE's length ratio, the leaf count by the leaf area index
— with Betula's constants and one habit the kit did not have: the **pendulous** branchlets.

| What | Source | What it says | What the solver does with it |
|---|---|---|---|
| **Size and habit of Betula pendula** | Vedel and Lange 1971, *Trees and Bushes*, pp. 141–143, as the [Betula pendula](https://en.wikipedia.org/wiki/Betula_pendula) article quotes it | 15–25 m tall, exceptionally 31; trunks slender, usually under 40 cm across; twigs slender, hairless and **often pendulous**; leaves **3–7 cm long**, triangular with a broad wedge-shaped base, on short slender stalks | `height_m` 14 by default (3–30), `stem_radius_m` 0.13, `leaf_length_m` 0.05 (the middle of 3–7 cm) with `leaf_width` 0.7 (a broad triangle); the branchlets hang (`droop`, below) |
| **Crown form: narrow in a stand, wider in the open** | Haeussler and Coates 1986, as the [Fire Effects Information System's Betula papyrifera review](https://www.fs.usda.gov/database/feis/plants/tree/betpap/all.html) (Uchytil 1991) quotes it | "In forests it typically has a slender trunk with a narrow crown; in openings it has a wider crown spreading out from near the base" | a narrow ovoid: limbs a share of the stem's height (`limb_length` 0.18) along an ellipse widest two fifths of the way up the crown; the default reaches 4.3 m from its foot at 14 m (0.30 of the height; the owner's open-grown reference photograph is about 0.27 a side). A recipe for an open-grown tree raises `limb_length` and lowers `crown_base` |
| **Leaf area index of silver birch** | Varik et al. 2011 (three Estonian stands of 14, 32 and 45 years, LAI **2.5–4.4**); Uri et al. 2007 (an 8-year natural stand on abandoned farmland, LAI **3.7**, specific leaf area 15.0 m² kg⁻¹) | LAI, one-sided leaf area over ground area, of whole stands | `leaf_area_index` 4.5 over the **crown's own footprint** (the convex hull of its shoot tips). A stand's LAI averages its crowns over the gaps between them as well, so a crown's own is higher than its stand's: 4.5 is the stands' upper end, chosen after the captures (below) showed 3.5 still reading thin from 30 m. That choice is ours, not a measured crown LAI, and we say so |
| **The terminal unit** | Corner's rules (axial conformity: smaller leaves on thinner shoots; [tree allometry](tree-allometry.md#the-model-and-its-sources)) | the twig and its leaf are the species', not the tree's | the branchlet is 1.5 mm in radius at its base and 0.6 m long (`twig_radius_m`, `twig_length_m`). We found no measurement of a silver birch's terminal long shoot; the radius is the oak's 4 mm scaled by Corner's rule from its 24 cm leaf to the birch's 5 cm (leaf area a sixteenth, radius a quarter: 1 mm) and then raised to 1.5 mm, which with a 0.6 m branchlet holds the leaf area at nearly one leaf a card (below) |
| **Branching ratio, length ratio, branch angle** | — | We found no table of Horton's ratio, a length ratio or insertion angles measured on Betula (Mäkinen 2002, *Trees* 16, models silver birch branch angles from stand density, but we could not read its numbers) | the general values the other species use: `branching_ratio` 4, `length_ratio` 0.63, `area_exponent` 2; `limb_angle_deg` 40, the upper limbs a half more upright, and the recursion's laterals at the same angle — read off the habit (ascending main branches) and the reference photograph, not measured |
| **Apical control** | Hallé, Oldeman and Tomlinson 1978 (via [tree allometry](tree-allometry.md#the-model-and-its-sources)) | a young excurrent crown keeps its leader | `leader_share` 0.35 (the oak 0.3): the stem runs through the crown to its top |

### Multi-stem clumps

| What | Source | What it says |
|---|---|---|
| **Where sprouts come from** | Zasada et al. 1978, via the [FEIS review](https://www.fs.usda.gov/database/feis/plants/tree/betpap/all.html); Hytönen 2019, ["Stump diameter and age affect coppicing of downy birch"](https://link.springer.com/article/10.1007/s10342-019-01175-5), *Eur. J. For. Res.* | sprouts typically arise from the stump base or the root collar; in downy birch over half within 2 cm of the ground |
| **How often a stump sprouts** | Zasada et al. 1978 (paper birch); Hytönen 2019 (downy birch, six stands in northern Finland) | 80–90% of 40–50-year-old paper birches sprouted after cutting, 40–50% of 100–125-year-old ones; downy birch stumps that did not sprout rose with stand age, 9% at 10–12 years, 14% at 15–16, 27% at 22–24; larger stumps sprout more; sprouting ability declines after about 40–60 years (Perala and Alm 1990) |
| **How many stems survive** | Perala 1974 via FEIS; Johansson 1991 (*Scand. J. For. Res.*, two-year-old silver birch stumps); a Minnesota DNR careful-harvest fact sheet (paper birch) | sprout mortality is high in the first five years, "leaving a few to several sprouts per clump"; silver birch's living stumps averaged 1.3–1.4 sprouts five years after cutting; the Minnesota sheet gives "usually 2–4 trees per clump at maturity" (we read that line in a search summary and could not open the PDF to check it) |
| **Other routes to several stems** | Foster and King 1986, via FEIS; Mikola 1942, [*Silva Fennica*](https://www.silvafennica.fi/article/7356) | multiple-stemmed paper birches are relatively common from browsing by moose and snowshoe hares; in northern Finland sprouting is birch's most important way of regenerating, in southern Finland a minor one; downy birch sprouts more than silver birch |

**What the solver does with it.** A birch whose `stems` is 0 lets the seed choose, from a
splitmix64 stream of its own: one draw against `clump_chance` (default **0.3**), and for a clump
one more stem at even odds at each count up to `max_stems` (default **5**) — half the clumps two
stems, a quarter three, an eighth four, the rest five (1,000 seeds: 344 clumps, 162 of two, 93 of
three, 38 of four, 51 of five). The literature gives no single frequency, because it depends on
how the stand began: a forest that grew from seed in a southern clearing is mostly single-stemmed,
a stand regenerated after cutting or fire, or browsed, is full of clumps, and silver birch sprouts
less than downy or paper birch. 0.3 is our reading of "a decent chance" in a mixed natural forest,
within that range rather than measured; a recipe for a coppice or a browsed stand raises it, and
one for planted silver birch sets `stems: 1`. The clump's shape follows the sprouting: feet on a
ring nine tenths of the stoutest stem's radius out at the ground (a stump's root collar), each stem leaning out by
about `stem_spread_deg` (15° by default, the stoutest half that) and straightening towards the
light, the stoutest first, the others 0.6–0.95 of its radius and, by elastic similarity
(H ∝ D^(2/3)), shorter. "A few to several sprouts per clump" is why five is the most.

## The budget, and where it was traded

A 14 m birch at the default LAI carries some 160 m² of leaf, about **69,500 cards** at full detail
(the branchlets' length allows cards of 6.0 cm rather than the leaf's 5: each side of a branchlet
holds its cards end to end, a card's base its width over the sine of its angle) — **152,945
triangles**. The forest gives a birch 36,000 and has hundreds of them. Version 1 met its budget by
growing few twigs; version 2 grows the whole structure and lets the **cards stand for more
leaves**: the leaves fill what the wood leaves of the budget, two triangles a card, each card grown
so the crown keeps its leaf area. While the cards would have to grow past twice the finest, the
wood gives way first (the finest wood folded, rows, cross-sections, the folded orders up to the
part class, the coarsest tubes), then cards up to four times the finest, and only then the
broadleaf's merge into branch-group cards — which no stem count reaches on 36,000. So a card on the
forest budget is 12–16 cm and carries the area of six to ten leaves (the short shoots' pairs along a long
shoot), and the crown keeps its LAI on every rung that does not merge.

| | Version 1 | Version 2 |
|---|---|---|
| Default birch (`{"species":"birch"}`; version 1 two stems, version 2 the seed's choice, one) | 18,796 triangles (bark 3,820, leaves 14,976), 312 twigs, 7,488 cards of 9 cm, 338 parts | **49,997** (bark 13,949, leaves 36,048), 3,685 branchlets over 9 orders, 18,024 cards of 11.7 cm, realised LAI 4.23 over 38.4 m², 42 parts |
| The forest recipe (`birch-1`: 14 m, `stem_radius_m` 0.16, one stem, 14 limbs, droop 0.8, 36,000) | 10,760 triangles, 328 clusters | **35,996** (bark 1,132, leaves 34,864), 6,061 branchlets, 17,432 cards of 12.6 cm, LAI 4.29 over 42.8 m², 59 parts, 1,016 clusters |
| `birch.flora.json` (14 m, two stems, 50,000) | 18,796 | 50,000 (bark 1,976), 24,012 cards of 11.6 cm, LAI 4.22 over 50.5 m², 68 parts |
| A clump on 36,000, two to five stems | — | 36,000 each; cards 13.8, 14.8, 15.7 and 15.7 cm; bark 2,012 to 4,571; 69 to 144 parts; LAI 4.22 |
| Everything at its most expensive (five 30 m stems of 0.5 m, 24 limbs, branchlets of 1 mm) on 8,000 | 5,352 | 6,941 after 13 rungs, merged |

**What the forest pays.** Each birch is now its whole budget rather than a third of it: 36,000
triangles and about 1,000 clusters where version 1 drew 10,760 and 328. The impostor chain
([flora](../subsystems/flora.md#the-impostor-chain)) is what keeps that affordable at a distance:
the birch now builds it like the broadleaf, its first branch-group cards switching in about 20 m
from a group at 1080p and its star of planes at about 390 m (`engine-content info` on `birch-clump.flora.json`).
We did not measure the forest's frame before and after; that belongs to the forest repository's
own benchmark.

## What the captures showed

Offscreen, msvc-release, 1600 × 1200, the stand-in sun, under the GPU lock (shared with another
agent's captures, which do not change a picture), scratch files outside the repository: a row of
the forest recipe, a single 14 m birch on 50,000 and a three-stem clump on 36,000, 9 m apart, from
13 m and from 30 m. **Version 1** read as the owner saw it: a few limbs, leaf strings, the sky
through everything, and close up its sprays already collapsed by the LOD into long strips.
**Version 2** reads as a birch close up — a narrow ovoid of fine pendulous branchlets, the white
stems through it, the clump's three stems leaning out from one collar under one shared crown — and
the first try (LAI 3.5, cards 15° out of their spray's plane) still read thin from 30 m: the cards
of a hanging branchlet lie in nearly vertical planes, edge-on from half the azimuths. Lifting them
30° out of the plane and taking the LAI to 4.5 filled it; 20 limbs rather than 14 evened out the
lower crown. It stays more see-through than the owner's photograph of an open-grown tree, whose
crown is wider and whose leaves are lit through; the flat-coloured cards with no leaf image (G60's
material packs give them one) are the largest remaining difference.

## What the forest's recipes should change

`stems: 1` asks for a single stem; to let the seed choose, drop it (or set `stems: 0`) and give
each recipe a few seeds, since the stem count is drawn per seed. `limbs: 14` keeps version 1's
count; dropping it takes the new default of 20. `droop: 0.8` is fine (the default is 0.7). For a
stand regenerated after cutting or browsed by moose, `clump_chance` 0.5 or more; for planted
silver birch, keep `stems: 1`. The 36,000 budget holds a full crown with cards of about 12 cm; a
near-camera hero birch would use 80,000 or more for cards near the leaf's own size.
