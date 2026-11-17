# Houses that differ: the grounding for a village's houses (G208)

Written 2026-11-16 before G208's code, beside [the interiors' grounding](vernacular-interiors.md) and
[a village that grew](village-growth-grounding.md), for the forest session's needs 110 (one storeys
for the entry made every house alike at 30 m), 133 (14 houses asked, 12 built: a 6.09 m plot
refused) and 134 (an eight-person household drawn into a one-room cot). Read through a fetch tool
that returns a model's answer to a question put to the whole page, so "read whole" below means the
whole page was put to the question; "a search summary" means only a search engine's summary of the
page was seen.

## Means

| What | Source, how far read |
|---|---|
| Washington, County Durham, 1666: 78 % of households had a single hearth (all the non-payers among them) against the county's 52 %, put down to the parish's rural character; in the county 44 % of households paid nothing, and of the taxed just over half had one hearth and another third two or three | hugofox.com, Washington History Society, *Hearth tax 1666 and 1674* (read whole) |
| The exempt "probably about a third" of the heads of households of a parish, more in towns | The National Archives' hearth-tax guidance (a search summary) |
| Most liable householders had one or two hearths; a significant share paid nothing; no distribution is given | Wikipedia, *Hearth tax* (read whole) |
| The exempt of Guilden Morden all lived in houses of one hearth, a labourer's or smallholder's house of two to four rooms | guildenmorden.gov.uk, *The Hearth Tax* (a search summary) |

So **means are drawn per household and not set for a village** (`domain/village`, `house_draw`):
where the entry allows two storeys, a quarter of the tenements' households have no means for a
second hearth or a floor over (means 0), three eighths one hearth (means 1) and three eighths two
(means 2). That is a **labelled choice**: the sources give a third to nearly a half of a rural
parish's households exempt and most of the rest at one hearth, and a tenement on a toft is not the
poorest household of a village (those are the cottages, whose draw is unchanged: means 0 or 1, one
storey), so the exempt share is taken lower and the two-hearth share higher than Durham's. The
storeys then follow from the household as the rural filler already asks them
([dwellings](../subsystems/dwellings.md#the-rural-filler)): three or four persons of means 0 or 1 a
hall and parlour on one floor, five or six of means 0 a cross-passage house on one, seven or more of
means 0 a cross-passage house on one with an inner room. Nothing here reads a number of storeys from
a source; the sources fix how unequal the households were, and the filler's table turns that into
houses.

## The household and the house

- Mean household size in pre-industrial England "fewer than five" (Laslett's 91 parishes, 4.7); in
  Goodnestone-next-Wingham, 1676, a "clear positive relationship between household size and social
  status" (search summaries of Laslett 1972 and the Cambridge Group's papers; the table itself not
  read).
- Pauper households "particularly small in size and simple in structure" (Sokoll 1987, a search
  summary).

So the household is drawn **with** its house: a plot that holds only a smaller house than the
household asked holds the household that house sleeps (`dwellings::persons_housed`: a cot two, a
hall and parlour four on one floor and six with chambers over, a two-up two-down six, a
cross-passage house eight; labelled, from the filler's own table read backwards). The drawn
household is kept beside the fitted one in a house's description (`drawn_persons`).

## Narrow plots

| What | Source, how far read |
|---|---|
| Burgage plots are strips much longer than wide, narrow "to ration the valuable street frontage"; the building stands at the street end, access to the rear "sometimes through an arch or passageway" | search summaries of northumberlandarchives.com *What is a burgage?* and interactive.thamemuseum.org *Burgage Plots* |
| Plots "had a building at the street end, facing end on into the street, with a yard to the rear" (Great Yarmouth) | a search summary (the page refused the fetch: its certificate) |
| Emsworth: a "standard 13ft frontage", shops "with integral, covered side alleyways", gable-end roofs "only one burgage plot wide" | emsworthonline.co.uk, *Roofline outlines* (read whole) |
| Alnwick plots two or three perches wide; widths vary within a street in quarter fractions of a unit | alnwickcivicsociety.org.uk and the Scottish burgh papers (search summaries) |

A town's narrowest plots were built end-on, wall to wall, with a passage through or beside the
house. The village's houses are detached on their tofts with half a metre each side, and its
filler's narrowest house is a cot of one bay (5.5 m outer). **Traded**: an end-on house turned
with its door onto a side passage needs the filler's rooms laid in depth and a door the plan does
not face to the street, which the village does not draw yet; so G208 cuts no toft, and squeezes no
cottage into a gap, narrower than the filler's least house with its side gaps (6.6 m along the
way), and a toft too narrow to halve into two of those stays whole, as two neighbours' halves were
thrown together in a town when one could not be built. A toft whose chord on a wandering street
edge comes out narrower still is refused and counted, and the count is said at the cook.

## A stone face

| What | Source, how far read |
|---|---|
| Rubble walls were "nine times out of ten" found to have carried renders and limewash; exposed rubble was not meant to be seen, and its stripping is Victorian, the last "150 years or so" | buildingconservation.com, *Traditional lime finishes* (read whole; it is about churches) |
| Houses normally rendered; farm buildings and boundary walls usually left bare | an East Midlands historic farm buildings report (Archaeology Data Service), a search summary |
| Stone used as rubble, not worked into regular blocks, in vernacular building | designingbuildings.co.uk after Historic England's guide to traditional buildings, a search summary |

So **two houses in sixteen are stone-faced** (`dwellings::describe`, its own draw so the other
houses keep their wash): the rubble face the rest carry under their limewash, shown where the
wash is not drawn — the poorer or the weathered house. A labelled share; the role is
`stone/rubble`, the plinth's and the chimney's.

## Forms beyond one plan

- Outshut / catslide: "a smaller lower addition", its roof "one continuous roof with the main roof",
  "often at a shallower pitch"; lean-to (pent): the same, its roof not continuous (geograph.org.uk,
  *Roof Types*, read whole). **Not built**: the interior's plan bands run along the house's length
  only, and an outshut is a band along its back; a row of its own.
