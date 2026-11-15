# The plan of a village: grounding (G69)

Recorded 2026-11-14 for the village filler ([village](../subsystems/village.md), roadmap G69, plan
[14 §14.3](../plan/14-structures.md#the-village-filler)). The owner's rule
(`AGENTS.md`, "Anything that represents the real world is grounded in its research first"): what is
known about how a rural village is laid out, found and read before the filler was designed, with
each number the filler pins, where it came from, and **how far the source was read**. The houses'
own dimensions (walls, storeys, rooms, stairs) are the other two legs' pages; this page is the
plan: plots, lanes, how a house stands to its lane, and how many houses make what.

**What kind of reading this was.** One evening's search by a research agent with a page fetcher
that returns a digest of most HTML pages; three sources were read as their own text (Roberts 1982,
the Burford panel, parts of two survey reports), the rest are digests, abstracts or search
snippets and are marked so. Historic England's *Medieval Settlements* guide, the obvious first
source, refused the fetcher (HTTP 403) and was **not read**. **No opened source gives a measured
village toft frontage or a village street's width in metres**: the frontage below rests on town
burgage plots and on the unit planned villages were laid out in, and is marked as an inference.
Nothing here is a fit to a survey; where the page says "a choice", it is one. Every figure below
is restated from its source, not quoted.

## Sources

| # | Source | Read |
|---|---|---|
| R | B. K. Roberts, *Rural Settlement: An Historical Perspective*, Historical Geography Research Series 9 (1982), hgrg.org.uk | the whole text extracted; the sections on classification, thresholds and origins closely |
| B | Victoria County History, *Burford* explore panel 2 (one page, PDF) | whole |
| S | J. Sheppard, "Metrological analysis of regular village plans in Yorkshire", *Agricultural History Review* 22 (1974) 118-135 | **abstract only** (a search result); the article was not opened |
| A | Alnwick Civic Society, "Rods, poles and perches" | a digest of the whole page |
| D | Designing Buildings, "The history of the dimensions of roads" (citing the *Leges Henrici Primi*, c. 1114-18, and the Statute of Winchester, 1285) | a digest of the whole page |
| I | gov.uk Rights of Way Advice Note 11 (withdrawn), on inclosure awards (Inclosure Consolidation Act 1801 s. VIII; the Glatton award, 1820) | a digest of the page |
| W | A summary of J. G. Hurst's description of Wharram Percy's houses (a university course page), and an excerpt at medievalists.net | digests |
| G | English Heritage, Gainsthorpe medieval village, history page | a digest of one page |
| H | University of Hull, deserted medieval villages project page (the Medieval Settlement Research Group's and R. Jones's thresholds) | one section |
| Y | Brigantia Archaeological Practice, *Yoden* village report (2004) | text extracted and searched, not read whole |

## Plan forms

Roberts (R) classifies a village's plan by three attributes: its **shape** (rows, or an
agglomeration), its **regularity** (regular or irregular; a regular agglomeration is a grid or a
radial plan) and whether it has a **green**, giving about ten basic families, with multiple-row
and composite (polyfocal) plans built from them. Row plans dominate northern England, among them
the regular **two-row** village whose rows face each other across a street or a green; composite
plans are commoner in the Midlands and the south. Regular plans are dated to about the 1080s to
1130s and on into the thirteenth century. A row is a run of **tofts** (the house plots), each
house with its toft behind it, running back to a **back lane** or the field's edge (R; the Yoden
report (Y) says the same of a Durham two-row green village; an excavation report on Laxton has
its tofts and crofts running from the two main streets back to back lanes, text extracted).
Roberts himself asks what separates an unsurfaced street from a green, which is why this filler
has one street form with a width: a green is a wide street.

**What the filler builds of this**: the **row plan**, regular, one row or two, along a lane the
author draws (a polyline), the two rows facing across it; a second lane makes a second street. An
agglomeration, a radial or grid plan, a green with a pond, a church at the head of the street and
back lanes are **not built** ([village](../subsystems/village.md#not-yet)); nothing in a source
opened here says where a church or a well stands, so none is placed.

## Numbers

| What | Value the filler uses | From | Standing |
|---|---|---|---|
| The unit of layout | the **perch**, 5.03 m (16.5 ft), a parameter (`perch_m`) | A: the statute perch, 5.5 yards. S (abstract): about a hundred planned Yorkshire villages were most often laid out in an 18 ft perch (5.49 m), a few in a 20 ft one; a review of later work proposes a 15 ft short perch in central and eastern England (snippet only). | The statute value is the default; the sources disagree on the unit (15, 16.5, 18, 20 ft), which is why it is a parameter and why the range 4.5 to 6.1 m is accepted. |
| A plot's frontage on the street | **2 to 4 perches** by seed (10 to 20 m at the statute perch), whole perches | B (a town, not a village): early Burford plots a perch and a half wide, later twelfth-century ones three perches (7.5 and 15 m). Cricklade plots mostly 2 perches (about 10 m; snippet). A: Alnwick plots about 28 ft (8.5 m) wide by Conzen's survey. | **Inference.** No opened source measures a village toft's frontage. The range is the towns' 1.5 to 3 perches moved up by half a perch because a rural house stands long side to the street here (below) and needs its own length of frontage; a frontage narrower than a house's length plus its side gaps turns the house gable-on. |
| A plot's depth (the toft) | **8 perches** (40 m) by default, 2 to 30 accepted | R: Durham row plans have short rectangular tofts under 150 m from the street frontage; west Durham's long tofts are often about 200 m, Cumberland's over 300 m. B: Burford 14 to 28 perches (76 to 140 m). G: Gainsthorpe's paddocks about a quarter acre (0.1 ha). | Within the sources' range at its short end: 3 by 8 perches is 0.06 ha, a little under Gainsthorpe's quarter acre. A choice for a test scene's size; the range accepted reaches Roberts's 150 m. |
| The street between two facing rows | **6.1 m** (20 ft) between the plots' fronts | D: the *Leges Henrici Primi* asks of a highway that two wagons can pass (about 20 ft). I: the Glatton award's private roads are twenty feet broad; the 1801 Act's public carriage roads 30 ft or more. | A highway's and an award's figure used for a village street: an upper-middle value. Earthwork streets are far wider where they were greens or have eroded (High Worsall's up to 40 m, West Burton's hollow way 12 m across; snippets), so the width is a parameter up to 40 m. |
| The worn tread in the street | **3 m**, a parameter | none | **A choice**: one cart and a person beside it. No cart gauge was found in an opened source. The tread is what is benched and kept bare; the rest of the street is verge. |
| How a house stands | long side to the street, its door in that side, when the frontage holds it; otherwise gable-on with the door in the gable | R: each house has its toft behind it; a Herefordshire description of plots with a narrow front on the green and a long yard behind (one section). | **Inference** from the plot's shape; no opened source states orientation. |
| Setback of the house from the plot's front | **2 m**, 0 to 20 accepted | none | **A choice.** |
| Houses | a **hamlet** under 6, a **village** from 6; the scene's is 8 | H: the Medieval Settlement Research Group's minimum for a village is 6 households; Jones (2010) 6 to 60; others propose 15 or 20. R: Northumberland about 1600: a small village 4 to 8 farms, medium 10 to 16, large 20 to 21, the largest 28 to 35, and Roberts calls the thresholds hard to define. W: Wharram Percy about 30 houses in 1368, 16 in 1458. | The class is reported in the plan's summary (`hamlet`, `village`), and the count is the author's. |
| Spacing between neighbours | the plots' frontages, back to back | none | Derived: no source gives it apart from the frontage. |

A house's own size is the house filler's (the rural filler's page). For the plan it matters only
that the peasant houses the sources measure fit the frontages above: Wharram Percy's
thirteenth-century house about 15 ft wide and about twice as long (4.6 by 9 m), its
fourteenth-century longhouse about 20 by 80 ft (W); Gainsthorpe's houses 20 to 30 ft long (G).
A 9 m house long side to the street needs a 2-perch frontage with half a metre to spare each
side, a 12 m one 3 perches.

## Where the budgets and the engine rule instead

- **The ground under a house is a pad** (a `Bench` patch, [tileset](../subsystems/tileset.md)),
  level at the mean of the ground under the footprint's corners and centre. A plot whose footprint
  spans more than `max_pad_step_m` (1.5 m) of ground is left empty and counted, plan 14's rule
  for a lot too steep for its plinth. That number is a choice about earthworks a village would
  not dig, not a source's.
- **A lane is a ground edit and bare ground**, not a mesh: a `Bench` line the tile set writes into
  its heights, whose bands the cover keeps off. Its ruts, its camber and its verge's worn grass
  are not built.
- **Plots are laid out in whole perches from the lane's first point**, in integer millimetres, so
  the same entry gives the same village on every toolchain; a plot never straddles a bend of the
  lane (a gap is left on the inside of a bend), which a real row would have filled with a
  wedge-shaped plot.

## What to read next

Historic England's *Medieval Settlements* introduction (refused the fetcher), Sheppard's article
in full for the perch counts of real frontages, the Wharram Percy toft plans (the figure found was
an image), and Roberts's *The Making of the English Village* (1987) for measured plans. A measured
frontage from any of them replaces the inference in the table.
