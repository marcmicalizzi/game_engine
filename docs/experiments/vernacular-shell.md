# The shell of a vernacular house: the numbers, and where they come from

Status: grounding for [structures](../subsystems/structures.md) (roadmap G65), 2026-11-14. This
page is the **shell's**: wall thickness and span by material, storey heights, door and window
sizes, roof pitch and depth by covering. The interiors' numbers (plan types, room sizes, stairs,
hearths) and the village's (plots, lanes) have pages of their own. It says what each number is,
its range, its source, how far the source was read, and which numbers have **no source yet** and
stand as stated guesses.

The description itself carries no default a source would argue with: every number below is a
field of `engine.structure.Description` that a filler or an author sets. What this page grounds
is the **example cot** (`structures::example_cot`), the validator's ranges, and the mesher's own
constants (a frame's section, a lintel's bearing).

## Sources, and how far each was read

| # | Source | Read |
|---|---|---|
| 1 | Thatching Info, "A Beginner's Guide" (thatchinginfo.com), a working thatcher's reference site | The page, whole, through a fetch that returned its numbered statements; quoted below |
| 2 | Building Conservation Directory, "Thatched Roofs: An Introduction" (buildingconservation.com, 1995) | The page, whole; it gives lifespans and ridge intervals and **no** pitch or depth |
| 3 | New Forest National Park Authority, "Chalk and clay cob" (historic buildings guidance leaflet) | Its search summary only: the PDF came back unreadable. Numbers taken from the summary are marked (summary) |
| 4 | Designing Buildings Wiki, "Cruck frame" | The page, whole |
| 5 | Building Conservation Directory, "Early Timber Doors" | The page, whole; it gives plank thickness only |
| 6 | East Dorset District Council, "Cob Walls in East Dorset" (planning guidance) | Search summary only |
| 7 | A builders' forum thread on an old rubble cottage (walls about 500 mm, bedroom ceilings "barely 2 metres") | Search summary only: one anecdote, used as a sanity check and nothing more |
| 8 | G. A. T. Middleton, *Modern Buildings* vol. 2, "Doors and Framings" part 2 (chestofbooks.com), a textbook of about 1900 | Its ledged-door section, whole (second reading, below) |
| 9 | New Forest NPA, "Chalk and clay cob", the web page of source 3 | The page, whole (second reading): it replaces 3's summary |
| 10 | Building Conservation Directory, "Earth Buildings and Their Repair" | The page, whole (second reading) |
| 11 | Thatching Info, "Laying Standard Thatch Coatwork" | The page, whole (second reading) |
| 12 | Thatching Info, "Ridging a Thatched Roof" | The page, whole (second reading) |
| 13 | Building Conservation Directory, "Thatching with 'Long Straw'" | The page, whole (second reading) |
| 14 | Thatch Advice Centre, "Eaves" | The page, whole (second reading) |
| 15 | *Notes on Building Construction*, "Rubble, General Remarks" (chestofbooks.com) | The page, whole; one sentence cut by its scan |
| 16 | Historic Scotland, a guide to stone floors | **Search snippet only**: the document did not open |
| 17 | Mid Suffolk and West Suffolk councils, leaflets on limewash and painting historic buildings | **Search snippets only** |
| 18 | Thatch Advice Centre, maintenance pages | **Search snippets only** |
| 19 | Mid Suffolk council's guide to windows in historic buildings; Cotswold District Council, "Traditional Casement Windows" (2013); a joinery federation's glazing factsheet | **Search snippets only** |

**The second reading** (rows 8 to 19, and 5 and 6 again; the night of 2026-11-14) was made by a
research sub-agent through a fetch that returns a summariser's account of a page and not its
text. Every number taken from it below is a close paraphrase, none a quotation, and should be
checked against its page before it is pinned harder than it is here. What that reading looked
for and **did not find** is listed under each table, because a missing number is a choice the
mesher made and not a fact.

Not reached tonight, and what the page would want from them: R. W. Brunskill, *Illustrated
Handbook of Vernacular Architecture* (wall materials by region, plan and section dimensions) and
*Houses and Cottages of Britain*; M. W. Barley, *The English Farmhouse and Cottage*; the Devon
Earth Building Association's leaflets on cob. They are books and leaflets not online in a form a
fetch reads.

## Roof pitch and depth by covering

| Number | Value used | Range | Source |
|---|---|---|---|
| Thatch, least pitch | | 45° | 1: "a roof structure, with a minimum pitch of 45 degrees, is necessary for a successful thatched roof" |
| Thatch, wanted pitch | 50° (`pitch` 1192 permille) | 50° to 55° | 1: "50 to 55 degrees of pitch is more desirable" |
| Thatch, the coat's depth | 30 cm (`thickness`) | 22.5 to 37.5 cm | 1: "an overall thickness of anything between 9 and 15 inches (225/375mm)"; combed wheat reed at the low end |
| A thatch course | (not modelled) | about 15 cm | 1: "courses of thatch are usually around 6 inches (150mm) thick" |
| Eaves overhang over an earth wall | 35 cm (`overhang`): **under the range**, traded for the door (below) | 40 to 70 cm | 3 (summary): a cob wall wants a roof overhang of 400 to 700 mm; a thatching reference in the same search (summary) has straw eaves bundles overhang the wall by 300 to 375 mm, so a stone wall takes less |
| Verge overhang | 25 cm (`verge`) | | **no source**: a guess at about half the eaves' |
| Slate, stone slate, pantile pitch | (no example yet) | | **no source read**; a row for the filler's grounding |

`pitch` is rise over run in permille so that no decision takes a tangent: 45° is 1000, 50° is
1192 (tan 50° = 1.1918), 55° is 1428. The mesher lights a covering of 15 cm or more as soft,
which every thatch in the range above is and no slate is.

## Walls

| Number | Value used | Range | Source |
|---|---|---|---|
| Cob wall thickness | 60 cm (`example_cot`'s `wall`) | about 60 cm at the foot, tapering, perhaps 30 cm at the wall plate | 3, 6 (summaries, not the documents' words): a Devon cob wall of about 600 mm; the taper is **not modelled**: a wall is one thickness |
| Rubble stone wall thickness | (the tests' 50 cm) | 30 to 61 cm; 40 cm at least for rubble | 7 and a masonry reference in the same search (summaries) |
| Plinth under cob | 30 cm above the floor (`plinth_top`) | 30 to 45 cm above the ground | 3 (summary): built off a stone plinth, normally 300 to 450 mm above ground level; some New Forest cob has none |
| Cob lift | (not modelled) | up to 60 cm | 6 (summary) |
| Bay | 6 m room (one long bay and a bit) | 3.6 to 4.8 m between cruck pairs; a small cottage one bay or two | 4: "spaced somewhere between 12ft (3.6m) and 16ft (4.8m) apart to form bays – a typical small cottage might comprise just one, or possibly two bays" |
| Span (the house's depth) | 4 m inside | | 4 gives a cruck blade "typically 5m-long", which at fifty degrees spans about 6.4 m between feet at most; a recorded cruck house in the same search is about 16 ft 6 in (5.0 m) deep (summary; the snippet was fragmentary). 4 m inside 60 cm walls is 5.2 m over the walls |

The example's 6 m by 4 m room is therefore at the generous end of a one-room cot: the filler's
grounding should bring a distribution, and one bay of 4.5 m is the commoner size.

## Storeys, doors and windows

| Number | Value used | Range | Source |
|---|---|---|---|
| Floor to wall plate, one storey | 240 cm | | 7 (summary, one anecdote): upper ceilings of about 2 metres. **No survey read.** The number is set by the walker, not a source: an eave's edge hangs `pitch x overhang` under the plate (at fifty degrees, 54 cm for a 45 cm overhang), the first cot's plate at 210 cm put its eave at 1.56 m across a 1.9 m door, and the walker is 1.8 m. 240 cm with a 35 cm overhang puts the edge at 1.98 m. A real cot solves it otherwise (a lower door, an eave lifted over it, a deeper plinth), which the description can say and the example does not |
| Door, clear | 90 by 190 cm | | **no source**: 5 gives only the planks ("generally 25-32mm thick", two to four planks a door). The width is the walker's: plan 14 §14.4 wants 80 cm clear with the leaf open. A historic cottage door is commonly lower than 190; the walker's capsule is 180 |
| Window, clear | 80 to 90 wide, 85 tall, sill at 95 | | **no source**: small because early glass was (a council's casement guidance, in summary: the small panes then available kept casements modest) |
| Frame sections | door 60 by 100 mm, window 50 by 70 mm, glazing bars 30 mm | | **no source**: joinery practice as remembered, not read |
| Lintel | 140 mm deep, bearing 150 mm each side | | **no source** |
| Threshold | 20 mm | | the mesher's, to keep the reveal's foot out of the floor's plane; under the walker's 0.35 m step |

## A cob wall's shape

| Number | Value used (the example cot) | Range | Source |
|---|---|---|---|
| Thickness at the foot | 60 cm | about 600 mm | 10 |
| Thickness at the plate | 45 cm (`top_thickness`) | "diminishes with height", perhaps no more than 300 mm at the wall plate | 9. The cot's wall is 2.1 m of cob, not two storeys of it, so it is taken three quarters of the way and not to 300: **a choice inside the source's range** |
| Which face leans | the outer | | **not found**: no source read gives a batter as a ratio or says which face; the outer is the common account and is chosen |
| Plinth height | 30 cm (`plinth_top`) | 300 to 450 mm above ground (a pamphlet's snippet); footings of brick or rubble 150 to 1,200 mm high | 10; the pamphlet in a search snippet only |
| Plinth proud of the cob | 4 cm (`plinth_proud`) | | **not found**: whether a plinth stands proud of, flush with or behind the face, and by how much. A choice |
| Corners | rounded to 12 cm (`arris`) | "gently rounded rather than sharp"; no radius | 6 (snippet). The radius is **a choice** |
| The face's wander | 12 mm (`wave`) | | **not found**: no source measures it. A choice, small enough to be a render over lifts and large enough to light |
| Lifts | (not modelled) | usually no more than 600 mm, visible on unrendered walls | 9 |
| Render | (the material's) | coats of 8 to 10 mm, three for about 20 mm | trade guides, snippets only |
| Roof overhang over cob | (the roof's `overhang`) | 400 to 700 mm | 9, which confirms source 3's summary |

## Leaves, shutters and unglazed windows

| Number | Value used | Range | Source |
|---|---|---|---|
| Boards to a door's leaf | 4 on a 90 cm opening (a board about 19 cm) | two or three upright planks on the earliest doors, four common from the seventeenth century, their widths unequal | 5 |
| A board's thickness | 3 cm | 25 to 32 mm | 5; 8 has 3/4 to 1 1/4 in |
| Ledges | 3 to a door | three or four | 5, 8 |
| Braces | none | ledged-and-braced doors date from the middle of the nineteenth century: a cottage door before 1800 is ledged only | 5 |
| Hinges and latch | **not drawn** (no iron surface) | wrought-iron straps, the early ones a loop on a pintle; tee hinges opposite the ledges; a thumb latch | 5, 8 |
| An unglazed window | mullions and shutters | early windows were unglazed, their mullions built into the frame, closed by hinged or sliding shutters | 19 (snippet) |
| A mullion's setting | on the cant | sometimes square, most often set on the cant (a "diamond" mullion) | 19 (snippet) |
| A glazed light's width | (not used: no glass) | no wider than about 450 mm a light; glazing bars 13 to 19 mm | 19 (snippets) |

**Not found, so chosen**: a historic leaf's width and height (the opening's, less its frame); a
ledge's section (2.5 by 15 cm) and where the ledges sit (20 cm from the head and the foot, one
between); a mullion's section (4.5 cm) and spacing (the description's count; the cot's are 16 to
17.5 cm apart); a shutter's boards (2 cm, two ledges 2 by 9 cm) and how it is hung (as a door's
leaf, inside, folding back into the reveal); which way a cottage door opens (inward here).

## What this leaves open

- The door, window, storey-height and joinery numbers stand without a source. They are fields or
  named constants, so a read of Brunskill or a county survey changes numbers, not shapes.
- A cob wall's taper, a thatch's courses and its ridge (flush or block) are not modelled.
- Pitch and depth for slate, stone slate and tile want the same table before a filler offers
  them.
