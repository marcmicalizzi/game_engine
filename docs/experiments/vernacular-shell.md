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
| Eaves overhang over an earth wall | 45 cm (`overhang`) | 40 to 70 cm | 3 (summary): a cob wall wants a roof overhang of 400 to 700 mm; a thatching reference in the same search (summary) has straw eaves bundles overhang the wall by 300 to 375 mm, so a stone wall takes less |
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
| Floor to wall plate, one storey | 210 cm | | 7 (summary, one anecdote): upper ceilings "barely 2 metres". **No survey read** |
| Door, clear | 90 by 190 cm | | **no source**: 5 gives only the planks ("generally 25-32mm thick", two to four planks a door). The width is the walker's: plan 14 §14.4 wants 80 cm clear with the leaf open. A historic cottage door is commonly lower than 190; the walker's capsule is 180 |
| Window, clear | 80 to 90 wide, 85 tall, sill at 95 | | **no source**: small because early glass was (a council's casement guidance, in summary: the small panes then available kept casements modest) |
| Frame sections | door 60 by 100 mm, window 50 by 70 mm, glazing bars 30 mm | | **no source**: joinery practice as remembered, not read |
| Lintel | 140 mm deep, bearing 150 mm each side | | **no source** |
| Threshold | 20 mm | | the mesher's, to keep the reveal's foot out of the floor's plane; under the walker's 0.35 m step |

## What this leaves open

- The door, window, storey-height and joinery numbers stand without a source. They are fields or
  named constants, so a read of Brunskill or a county survey changes numbers, not shapes.
- A cob wall's taper, a thatch's courses and its ridge (flush or block) are not modelled.
- Pitch and depth for slate, stone slate and tile want the same table before a filler offers
  them.
