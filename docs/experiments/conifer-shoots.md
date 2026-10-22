# Conifer shoots: a spruce that reads as foliage (G80, 2026-10-22)

**What prompted it.** The owner's forest stress test (`forest_wildlife_poc`, three spruce recipes
of 17 to 21 m on a 40,000-triangle budget among birches and oaks) drew its spruces as bare brown
branch skeletons with a faint speckle, where a real spruce reads as a dense dark-green mass with its
branches mostly hidden. The structure — straight trunk, whorls, a cone — was right; the foliage was
not there to see. The second ask: real conifers are never that regular, so the seed should leave
branches out. This page is why the needles were invisible, what a spruce's shoots are, the numbers
the solver takes from the literature, where the triangle budget forced a trade, and the rates of the
missing and dead branches. The rules that follow are in [flora](../subsystems/flora.md#the-conifer).

## Why the needles were invisible

Rendered offscreen at 1600×900 under the sun, with the forest's own material pack, the old solver
reproduced the owner's picture exactly; with the same geometry and no pack (the flat needle colour,
every card opaque) the same trees read as solid green strips. So the geometry put cards where they
should be, and four things together made them vanish:

1. **The pack's needle image was 17 % opaque and periodic.** The forest's `conifer/needles` base
   colour is a fishbone — a midrib and evenly spaced needles three texels wide — covering 17.2 % of
   the 512² image (measured: alpha ≥ 0.5 at every other texel). Coverage-preserving mips
   ([texture](../subsystems/texture.md#alpha-tested-masks)) keep 17 % at every level only for an
   irregular mask: a periodic one filters to one uniform alpha at the level whose texel spans its
   period, and no scale cuts a uniform level at a fraction, so at distance the cards went from 17 %
   to speckle or nothing.
2. **One layer of cards, laid level.** Each branch carried a single spray, 24 cards a side, 0.42 m
   long, splayed 60° forward and lifted only 15° out of a level plane. A level plane is edge-on to
   an eye at its height, which is where most of a forest's crowns are seen from; and one layer of a
   17 % image is 17 % of the crown, whatever the angle.
3. **A fringe, not a branch system.** The foliage was a strip about 0.36 m wide either side of each
   branch from a sixth of the way out — the whole of a spruce's second- and third-order shoots
   collapsed into one row of cards.
4. **Branches three times too thick.** A branch's base radius was 0.03 m + a quarter of the trunk's
   radius there: 0.12 m at the crown's foot of a 0.38 m trunk, a 24 cm limb. Colin and Houllier's
   largest branch of a whorl is about 7 cm through on such a tree (below). The tubes were what the
   eye found.

## What a spruce's shoots are

| What | Source | What it says | What the solver does with it |
|---|---|---|---|
| Needles 10–25 mm, alive 5–7 years | [waldwissen.net, *Die Fichte*](https://www.waldwissen.net/de/lebensraum-wald/baeume-und-waldpflanzen/nadelbaeume/die-fichte-picea-abies) (Bavarian state forestry); Reich et al. 1996, [*Tree Physiology* 16](https://experts.umn.edu/en/publications/evidence-that-longer-needle-retention-of-spruce-and-pine-populati/) | "Die Lebensdauer der Nadeln beträgt 5 bis 7 Jahre"; Norway spruce of 670–1235 m provenances grown in a Polish lowland common garden kept 6.4–7.2 needle cohorts, and longer retention at altitude is largely an acclimation, not a provenance trait | `needle_years` (default 6, 2 to 15): with `shoot_growth_m` (0.2 m a year) a branch is needled on its outer 1.2 m, and the inner part of a long branch is bare wood — the dark interior a real spruce shows through its gaps |
| Three habits: comb, brush, plate | waldwissen.net (above): "die Kammfichte mit schlaff herabhängenden Ästen, die Bürstenfichte mit kurzen, bürstenartigen Ästen, die … Platten- oder Kreuzfichte mit horizontal vom Hauptast angeordneten Verzweigungen" | The second-order branches hang as curtains from the first-order ones (comb, the lowland form), stand short and brushy, or lie level in the first-order branch's plane (plate, the mountain form) | `pendulous` (default 0.7, between brush and comb): a lateral leaves its branch at 55° in the branch's level plane, dipped by 0.35 × `pendulous`, and turns towards the ground by 3 × `pendulous` radians over its length; its spray's plane holds the branch's direction, so a hanging lateral is a curtain face-on to an eye beside the branch and a level one a plate |
| Laterals with their parent's length; flatter up the orders | *Formalization of crown structure in Norway spruce: a model of higher order branching* ([AGRIS record](https://agris.fao.org/search/ru/records/6471e5372a40512c710e8376)) | The number and length of laterals rise with the parent shoot's length; the elevation angle falls with branch order and with the shoot's youth; the divergence angle does not depend on order | Laterals per metre of a branch's outer part (`laterals_per_m`), each as long as its age allows: `lateral_length_m` (0.7 m) a needled reach in from the tip, 0.3 of it at the tip, never more than 0.4 of its branch |
| Shade shoots flat, sun shoots bushy | Stenberg, Kangas, Smolander and Linder 1999, *Tree Physiology* ([Duke record](https://scholars.duke.edu/publication/791885)) | The shoot silhouette to total needle area ratio (STAR) and specific needle area fall as canopy openness rises: a shade shoot carries about three times the silhouette per needle mass of a sun shoot, flatter and less clumped | Every card lifts 10° out of its spray's plane at the crown's foot and 35° at its top: flat shade sprays low down, bushy sun shoots above |
| The thickest branch of a whorl | Colin and Houllier 1992, *Ann. Sci. For.* 49:511 (the equation as quoted by a later study; the paper's own text was not reachable); Hein, Mäkinen, Yue and Kohnle 2007, [*For. Ecol. Manage.*](https://www.sciencedirect.com/science/article/abs/pii/S0378112707000357) | MAXD = 15.9 mm + 0.978 mm per cm of breast-height diameter; at 350 stems/ha the largest branch of a whorl passes the 4 cm grading limit | A branch's base radius is MAXD/2 × (its length over the crown's reach)^0.8, the trunk's breast-height diameter taken as 0.85 of its diameter at the ground: 3.2 cm for the default 0.3 m trunk, 3.8 cm for the forest's 0.38 m, never under 6 mm or over half the trunk there |
| Whorl and interwhorl branches | Mäkinen et al. 2003, [*Forestry* 76:525](https://academic.oup.com/forestry/article-abstract/76/5/525/517568); the 2019 Austrian needle-area study ([BFW](https://www.forestscience.at/artikel/2019/1/abschaetzung-von-neu-zugewachsener-nadelflaeche-der-fichte.html)), which models whorl and interwhorl branches separately | Between the whorls a spruce grows smaller interwhorl branches, a category of its own in the branch models | `interwhorl` (default 2) branches between two whorls, 0.45 ± 0.15 of the whorl length there, at a seeded azimuth each, on a stream of their own |
| A dead-branch keeper | waldwissen.net (above): "Die Fichte ist diesbezüglich jedoch als *Totast-Erhalter* eine Ausnahme"; stands are often branch-free to 15–20 m only once the dead branches break; Mäkinen et al. 2003 and Hein et al. 2007 model living and dead branches per whorl separately | Close spacing kills the lower branches, and a spruce keeps them, dead, for decades | `dead_share` (default 0.12) of the crown's whorls from the lowest are dead: bare wood, four fifths as thick, broken at 35–90 % of their length, no needles. A forest recipe for a closed stand raises it |

**What a card stands for.** A real spruce branch carries 20 to 40 second-order laterals a metre
(several on each year's shoot, both sides), each a frond of third-order shoots 0.2–0.3 m wide, so a
branch's foliage is three or four fronds deep wherever it is seen through. The solver builds 10
laterals a metre and makes each card a **foliated shoot system** — 0.45 m on a branch's own axis,
0.36 m on a lateral (`needle_length_m` and 0.8 of it) — wide enough that a lateral's spray reaches
over the next one's on the same side: the unbuilt laterals' area is folded into the built ones'
cards. That is the budget's trade, and the reason a pack's needle image should be a whole shoot
(below).

## The budget

The default and the three recipes at full detail (`engine-content flora`, msvc-release; before =
main at 7a235b70):

| Recipe | Before: triangles (bark / needles), parts | After: triangles (bark / needles), parts |
|---|---|---|
| default (16 m, crown 3.2 m) | 12,882 (5,510 / 7,372), 136 | 23,742 (7,058 / 16,684), 179 |
| `spruce-tall` (22 m) | 13,058 (5,670 / 7,388), 136 | 23,450 (7,326 / 16,124), 182 |
| `spruce-young` (4 m) | 3,442 (1,430 / 2,012), 36 | 4,062 (1,934 / 2,128), 50 |
| forest `spruce-0` (17 m, crown 3.6 m) | 13,034, 136 | 25,978, 180 |
| forest `spruce-1` (19 m) | 12,990, 136 | 28,014, 187 |
| forest `spruce-2` (21 m) | 13,130, 136 | 29,218, 188 |

Every one fits its budget with no rung walked (the forest's 40,000 has 11,000 to spare). The wood
grew by the interwhorl branches (one part each); the needles more than doubled. The first cut of
the design (16 cards a side, 7 laterals a metre, 0.28 m cards) was 38,974 on the default, walked a
rung, and still read as speckle at 40 m: its small cards were a few pixels each there, holding a
few texels of a sparse image. Fewer than half the cards along each shoot, each longer, on more laterals,
reads as a mass at 40 m and costs 40 % less.

**What it costs to draw.** A stand of 49 forest `spruce-1` trees at about 4.5 m spacing (500 stems
a hectare), 1920×1080, CSM shadows, the forest's own material pack, a 10 s walk from 40 m into the
stand (601 frames, RTX 5090, 2026-10-22; the harness's `machine_state`: other processes 4–6 % CPU,
the GPU 2–9 % busy before each run, under the machine-wide GPU lock): GPU total median 1.14 ms
before, **2.35 ms** after (p95 1.24 → 2.68); the shadow maps 0.62 → 1.48 ms and the hardware
rasterizer 0.10 → 0.28 ms, since the cards are alpha-tested and cast into every cascade. The trees
now cover most of the frame, where before the sky showed through them.

## Branches the seed leaves out

Real whorls vary — Mäkinen et al. and Hein et al. model the branch count per whorl and the living
share of it as distributions, not constants — but we found no published rate for "a branch slot
empty" or "a whorl half empty", so the rates are chosen, small, and fields:

- `missing_branches` (default 0.05): a branch is missing; half of those leave a **broken stub**, one
  twelfth of the branch, dead.
- `sparse_whorls` (default 0.08): a whorl is sparse, each of its branches missing with even odds.
- `dead_share`, above, is grounded: the lowest whorls of a stand-grown spruce are dead and kept.

All three draw from a `fate` stream of their own — one draw a whorl and four a branch slot, whatever
they decide — and a lateral's draws come from its slot's own stream, so a missing branch moves no
other branch, lateral or card (the test holds every surviving part's pivot and direction to the
bit).

## What a pack's needle image should be

The solver gives each card the whole image (u along the shoot it hangs from, v out from it). For a
spruce that image should be **one foliated shoot**, its axis along v from the card's base: a twig
with its side shoots and needles all round them, about 25–35 % of the square opaque (a shoot's
outline is a narrow oval; inside it the needles should be nearly solid), every needle at least two
texels wide at 512, and **irregular** — needles of varied length, angle and spacing — so the build's
coverage-preserving mips have something to keep. The forest's fishbone fails the last two; a
drawn shoot of 28 % coverage used for the captures read as a mass at 40 m.

## Not yet

- **Far cards for the conifer.** The broadleaf and the ancient tree have an impostor chain
  ([flora](../subsystems/flora.md#the-impostor-chain)): branch-group cards baked from the finest
  leaves, then a tree star. The conifer has none, so beyond a few hundred metres it is the LOD DAG's
  simplification of its sprays.
- **A pine.** Pines carry needles in fascicles at the shoot tips and long bare inner branches; the
  same solver with a short `needle_years`, no laterals and long cards approximates one, not well.
- **Needle colour by age** (the current year's shoots paler) and **dead twigs on dead branches**.
- **The interwhorl count from data**: two is chosen; the models above give it per tree size.
