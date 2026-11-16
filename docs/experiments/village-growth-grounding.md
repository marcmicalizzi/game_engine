# A village that grew: grounding (G206)

Recorded 2026-11-15 for the village filler's grown plan ([village](../subsystems/village.md),
roadmap G206), beside [the row plan's grounding](village-plan-grounding.md), which still holds
for the unit of layout, a tenement's frontage, the street's width and the size classes. The
owner's complaint is the brief: the row plan "reminds me of more modern planned street, whereas a
fantasy village is more medieval and more a natural evolution of where buildings end up, much
less grid-like". This page is what is published about how a medieval village came to its shape,
read before the grown plan was designed, with each number the planner pins, where it came from
and how far its source was read.

**What kind of reading this was.** One evening's search by a research agent whose page fetcher
reads HTML and returned no PDF as text: the Historic England *Medieval Settlements* guide (the
first source to read, HEAG210) refused it again (HTTP 403, three copies), British History Online
asked for a CAPTCHA, and the two procedural papers' PDFs (Emilien et al. 2012, Galin et al. 2010)
came back as compressed bytes, so **no constant of either paper was read**. What was read whole
are county Historic Environment Record entries (which reproduce the guide's text), English
Heritage's site histories, an Internet Archaeology article in part and the Medieval Settlement
Research Group's research framework in part. Roberts (1987), Roberts and Wrathmell (2000), Taylor
(1983) and Lewis, Mitchell-Fox and Dyer (1997) had no open text. Every figure below is restated
from its source, not quoted; "(snippet)" marks a figure seen only in a search engine's digest of a
page that would not open.

## Sources

| # | Source | Read |
|---|---|---|
| HE | Historic England (2018), *Medieval Settlements*, Introductions to Heritage Assets (HEAG210) | **not opened** (403); its text as list entries reproduce it in WW and MC |
| WW | Lincolnshire HER, West Wykeham deserted village, scheduled monument (NHLE 1004989) | whole |
| MC | Northamptonshire HER, Mallows Cotton deserted village (DNN2131) | whole |
| WC | Northamptonshire HER, West Cotton (DNN3571) | whole |
| FX | Northamptonshire HER, Faxton croft 29 (MNN10410); Wythemail toft (MNN23398); Kelmarsh tofts (MNN131560) | whole, each |
| WP | English Heritage, Wharram Percy, "Description" | whole |
| GA | English Heritage, Gainsthorpe, history | whole |
| HT | English Heritage, Hound Tor deserted village, history | whole |
| WH | Jones, Dyer and Page (2006), "Changing settlements and landscapes: medieval Whittlewood", *Internet Archaeology* 19 | in part (1.1, 4.1, 5.3) |
| MF | Medieval Settlement Research Group, research framework, review of research 2007-16 | in part (reorganisation and growth) |
| ML | S. Mileson, "Living like common people", *Current Archaeology* 381 | whole |
| HF | Herefordshire Through Time, "How were village houses constructed" | whole |
| NW | Keys to the Past, West Whelpington (N9556) | whole |
| DU | University of Hull, *Beresford's Lost Villages*, Durham | whole |
| GR | Historic England, *Heritage Calling* (2024), "The history of England's village greens" | whole |
| LE | Historic England list entries for Cottam (1017068), Uckerby, Coxhoe (1019918), East Tanfield (1016260), Kirby (1418356); RCHME Northamptonshire vol. 3, Catesby | **snippets only** |
| WK | Wikipedia: Haufendorf, Angerdorf, Rundling, Hollow way, Village green, Longhouse, Nucleated village | whole, **as leads** (their primaries were not opened) |
| EM | A. Emilien, A. Bernhardt, A. Peytavie, M.-P. Cani, E. Galin (2012), "Procedural generation of villages on arbitrary terrains", *The Visual Computer* 28, 809-818 | **abstract and digests only** |
| GL | E. Galin, A. Peytavie, N. Maréchal, E. Guérin (2010), "Procedural generation of roads", *Computer Graphics Forum* 29(2), 429-438; and Galin, Peytavie, Guérin, Beneš (2011), "Authoring hierarchical road networks" | **abstracts and digests only** |

## What a grown village is, and that regular ones stood beside it

- **Both kinds existed, and in one village.** HE (via WC, WW) describes plan forms running from
  the highly irregular to planned layouts of tofts and crofts running back from a main road.
  Planned rows are dated: Wheldrake late eleventh to early twelfth century, Silverstone's West End
  laid out about 1100 (WH), Wharram Percy's three rows built in stages from about 1166 (WP). But a
  village "tended not to be planned in a single phase by its lord" (ML), Akeley grew from two foci,
  the church's and the manor's, that ran together with planned extensions (WH), and at Wharram one
  row is "less obviously planned" and the cottars' row has smaller, less regular plots (WP). Up to
  80 % of northern settlements show some planning (DU, citing Roberts 2008). **So one generator
  with an `irregularity` from a regular row plan (0) to a grown village (1) is grounded**, and so is
  irregularity within one village: the founding tenements regular-ish, the later cottages not.
- **A grown village has a nucleus and grows outward along its ways**: the clustered *Haufendorf*
  of irregular plots and farms of very different sizes, the *Angerdorf* round a lens-shaped green
  with its houses eaves to the road (WK); EM's model grows houses from seeds along roads, "new
  roads attract houses" (abstract). The planner's order — plots taken nearest the meeting of the
  ways first, the later ones further out and squeezed between — is that rule; its weights are
  choices, since EM's interest functions were not read.

## The ways

- **A way follows the ground and an older route**: West Wykeham's main hollow way is part of the
  road between two neighbouring places and forks in a V at the village's end (WW); Wharram's east
  row stands along the track into the village (WP). A way is therefore found **between the places
  it comes from and goes to** — the entry's waypoints — never ruled between them.
- **How it is found**: GL's model is a least-cost path over the terrain, with a cost growing with
  slope, curvature and obstacles, on a discrete mask of neighbours (the 2010 abstract and digests;
  the cost's constants were not read). The planner's is the same form: a path over a grid of
  `station_m` cells on the ground, each step costing its length times (1 + (grade / 8 %)^2), a
  grade over 25 % costing ten times more, and a seeded wander term up to a quarter of the length at
  `irregularity` 1 so a way bends on flat ground too. **8 %, 25 % and the wander are choices**:
  8 % is about the grade a loaded cart takes without a second horse, from no source opened here.
- **Hollow ways** are 0.5 to 2.5 m deep and 4.5 to 9 m wide (MC 2 m deep, 4.5 m wide; WW 1 to 2 m
  deep over 160 m; Catesby to 2.5 m down a hillside, Kirby 9 m wide, snippets). The planner does not
  cut a way hollow (the tread is benched level across, as before); the street's default 6.1 m
  stands within the measured widths.
- **A way widens where ways meet**: no opened source gives a funnel's or a widening's measure. The
  street's edge on each side wanders by up to a quarter of the street's width at `irregularity` 1
  (a choice), and opens into the green below.

## The green

- **Greens are many, their sizes wide**: about 3,650 registered greens cover 3,298 ha in England, a
  mean of about 0.9 ha (WK, modern registrations; the mean is this page's division); Car Colston's
  two are 12 ha, Duncan Down 21 ha (GR). Many began unplanned on the edge of common grazing; some
  were laid out (GR). An *Angerdorf*'s green is lens-shaped, with its pond on it and the church,
  the school and the smithy built on it later (WK).
- **What the planner builds**: a lens where the ways meet (or, with one way, at its middle): the
  street opens to `green_m` (24 m by default) across over three times that length, each side's
  edge a half-sine, so its area is about two thirds of width times length, 0.15 ha at the default.
  **A choice at the small end** of what is attested, sized for a test scene of 256 m; a scene may
  ask 40 to 80 m.
- **Encroachment**: cottages facing a green often stand on former common (a popular page, not a
  source). The planner's cottages may stand with their front on the green's edge pulled in, a
  choice in the spirit of it.

## Plots

| What | The planner | From | Standing |
|---|---|---|---|
| A founding tenement's frontage | the row plan's 2 to 4 perches, then stretched or shrunk by up to a quarter at `irregularity` 1 | LE: Cottam just over 20 m, Coxhoe 6 to 10 m, Uckerby up to 40 m (snippets); the row plan's sources | the range sits inside the measured ones |
| Its depth | the entry's `depth_perches`, varied by up to 30 % | LE: Cottam 35 m, Uckerby 60 m (snippets); WP: the villein rows regular in size | the variance is a choice |
| A gap between two tenements | up to two perches, drawn small more often than large, at `irregularity` 1; none at 0 | none | **a choice**: a grown row is not back to back, and the gaps are where cottages come later |
| A cottage's plot | 1 to 1.5 perches by half the depth, one storey | WP: the cottars' row smaller and less regular; HF: a cottar's house one room of about 5 by 3.5 m; ML: families split plots to house the young | sizes a choice within those |
| A plot's turn off square to its way | up to 20° at `irregularity` 1: toward the contour, toward a long axis running east and west, and a seeded part | FX: Wythemail's longhouse first across the slope, then along it; HT: a longhouse nearly always on a slope, its byre the lower half; WK: Dartmoor longhouses east-west with their openings south (weak) | the 20° and the weights are choices; the planner takes the contour (it keeps a pad low) |
| A house's setback inside its toft | the entry's `setback_m`, as before | ML: a yeoman's house set well back, gable to the road; WK: an Angerdorf's eaves to the road | no measure found: **a choice**. A house's distance from the way varies because the toft's front does (the street's wandering edge, the green), not the setback |
| A back lane | behind three or more founding tenements of one row that are built, 2 m wide, a toft's margin of 1.5 m behind their backs | HE via WW: back lanes often ran behind the crofts; WK: a planned village's back lane at the far end of each plot | the run of three and the widths are choices |

## Where the church and the well stand

- **The church**: "usually at the end of the village" with the manor (HE via WW); West Wykeham's
  stands in its corner on the highest ground, its churchyard 42 by 24 m and the church 15.1 by
  7.3 m (WW); Gainsthorpe's church about 180 m off (GA); a Rundling's almost always outside (WK).
  The planner **reserves a plot named for it**: of the founding plots taken in the outer half of the
  village's order, the one on the highest ground, made 42 by 24 m (WW's churchyard), left without a
  house (no church structure exists yet).
- **The well**: no opened source places one. It stands at the green's middle, where the ways meet,
  **a choice**; an Angerdorf's pond stands on its green (WK). A pond is not built (it would be a
  water body the scene authors).

## Size

A village of 10 to 40 houses over 200 to 500 m: Wharram about 40 peasant houses (WP), West
Whelpington 25 house sites over about 400 m round a green (NW), Mallows Cotton about 500 by 200 m
(MC), West Cotton about 205 by 218 m (WC), Durham's West Hartburn about 12 tofts (DU). The hamlet
and village classes are the row plan's grounding's.

## What a regular plan is at `irregularity` 0

No wander, no gaps, no turn, whole perches and the entry's depth exactly, and no cottages: the row
plan of [the earlier grounding](village-plan-grounding.md) along a way found over the ground (on
level ground, the straight line between the waypoints to within the cell's rounding). The green and
the church stay the entry's to ask, since regular green villages are attested (DU: Durham's
two-row plan faces across a street or a green).

## What to read next

The two procedural papers in full, for their slope costs and interest weights (a text extraction of
their PDFs); HE's guide; Roberts (1987) for measured plans and the green's funnel mouths; Taylor
(1983) on village growth and shift; any source that measures a house's distance from its street or
places a well.
