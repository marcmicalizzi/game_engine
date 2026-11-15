# dwellings (domain)

**Purpose.** The plan of a small house: its rooms as a graph a person recognises, the inner walls
between them, a doorway between every pair of rooms the graph joins, a stair a walker can climb,
its hearths, and where its outer doors and windows sit (`interiors.h`, roadmap G66); and the
**rural filler**, which chooses that house from a plot, a household and a seed (`rural_filler.h`,
roadmap G72). It is plan 14's [fillers](../plan/14-structures.md#the-fillers), step 6 and the
rural filler's choice. It meshes nothing and reads no ground: a plan is description data in integer
centimetres, and the structures capability's mesher draws it.

**Why this shape.**

- **A plan apart from the mesher.** The interior is a pure function of a clear rectangle, a plan
  type, a storey count and a seed. Kept apart from the mesher it is tested in numbers (every room
  reached, every stair with its headroom) in a fraction of a second with no mesh built, and a
  hand-authored shell asks for it the same way a filler does.
- **A capability of its own, where plan 14 puts the rural filler inside `structures`.** The plan's
  argument against splitting ([§14.3](../plan/14-structures.md#143-the-description-the-mesher-and-the-fillers),
  "Why not five") is that the solvers share the description, the kit and the stair rule. The
  interior plan shares none of the mesher's code: `interiors.cpp`, `rural_filler.cpp` and `tour.cpp`
  need `base`, `containers` and `hash`, and only `describe.cpp` names the description. The edge
  runs one way, from this module to the description it fills, as the village filler's does
  (`engine_capability_requires(dwellings structures)`). The documentation check also allows no
  page without a module, and the legs that built this on one night each needed a page. If the two
  are folded together later it is a move of files; no plan changes.
- **Four plan types, by what could be grounded** ([the grounding](../experiments/vernacular-interiors.md#plan-types)):
  the one-room cot, the hall and parlour, the cross-passage house and the two-up two-down. Each is a
  row of **bands**: rooms side by side along one axis, every inner wall running the full depth
  across it. That is what these houses are (one room deep, a bay or two a room), and it makes every
  rule a statement about intervals.
- **The stair is a straight enclosed flight in a band of its own**, the strip: a level foot landing
  entered through a doorway in the strip's side, the flight, a level head landing on the upper
  floor with a doorway to the chamber on each side. The historic stair is a winder beside the stack;
  the walker's going of 0.28 m at the walking line and the collision proxy's 5 cm cut hold no
  winder, and a straight flight between two walls needs no rail to be a wall. The price is
  [the house's depth](../experiments/vernacular-interiors.md#the-stair): 5.1 m clear where the stair
  runs.
- **Validators read the plan and repair nothing** (plan 14 §14.3): `plan_interior` runs
  `validate_interior` on what it laid out and refuses a plan that breaks a rule, naming the rule,
  so a rule the layout cannot keep at some size is a refusal with a sentence, never a house with a
  window behind a wall.

**Owned data.** `InteriorPlan`: `Room`, `InnerWall`, `Doorway`, `OuterOpening`, `Hearth`, `Stair`,
in the building's frame (x along the front wall, z away from the lane, the outer walls' inner faces
at 0 and at the clear width and depth). `RuralChoice`: the type, storeys, hearths and clear
rectangle a filler chose, and the footprint over the outer walls.

## The plan types

| Type | Ground storey, in order | Outer door | Hearth | Upper storey |
|---|---|---|---|---|
| `Cot` | hall | front wall, off-centre toward the end away from the hearth | the far gable | none |
| `HallParlour` | hall, (strip,) parlour | front wall, into the hall beside the partition | the hall's gable; a second in the parlour's | a chamber each side of the strip |
| `CrossPassage` | (inner room,) hall, passage, (strip,) service | front and back, opposed, into the passage | in the hall, backing onto the passage; a second in the service room's gable | a chamber over the hall end (two with an inner room, the far one reached through the near), the strip, a chamber over the service end |
| `TwoUpTwoDown` | front room, strip, back room, away from the lane | front wall, into the front room at the end away from the hearth | the front room's side wall; a second in the back room's | a chamber in front and one behind |

A room that is not a strip or a passage is **habitable**: it gets windows until its glass is a tenth
of its floor (one at least, four at most), front wall first, then the back, then the gables, each
centred in the widest free stretch of its wall, with piers kept to corners, doors and stacks. A
stair's head landing looks out through the wall its flight climbs toward. Rooms on the ground
storey join in a chain: a doorway through every partition, and where the strip stands between two
rooms, through both its sides at the foot landing, so the landing is the lobby between them.

## The stair

`stair_risers(rise)` is the storey's height over 210 mm, rounded up; the going is 280 mm; each
landing at least a metre. `Stair` carries the strip, the foot, the flight and the head as
rectangles, the direction the flight rises, and the rise in millimetres; `tread(i)` and
`tread_mm(i)` are the treads a mesher and a collision builder both read, so the two cannot differ.
The upper floor has no slab over `well()`, the flight's run across the strip: headroom over the
flight is to the upper storey's ceiling, and `stair_headroom_cm` is the one function that says so.
The well ends where the flight does, at both ends: the last riser is the head landing's edge (the
mesher leaves a gap between the top tread and the floor under a longer well), and the foot landing
keeps the upper floor over it, so a storey under 2.3 m floor to floor (2.1 m under a 20 cm floor)
is refused where one of 2.25 m passed while the well ran over the foot.

## The rural filler

`choose_rural(plot, household, style, seed)`:

| Household | Asks for |
|---|---|
| 1–2 persons | a cot |
| 3–4 | a hall and parlour on one floor; with means 2, two storeys and a second hearth |
| 5–6 | a hall and parlour on two storeys; with means 0, a cross-passage house on one |
| 7 and more | a cross-passage house: one storey with an inner room (means 0), two storeys (means 1), two storeys with an inner room and a second hearth (means 2) |

**What is fixed and what varies.** The household and the plot fix the type, the storeys and the
hearths; nothing is drawn for them. A plot too small for what was asked gets the next plan down, in
this order: the same rooms as a hall and parlour on two storeys, the two-up two-down (5.1 m of
frontage where a hall and parlour wants 6.4 m), the hall and parlour on one floor, the cot;
`RuralChoice::demoted` says it happened. The seed varies what one type's houses differ in along a
lane: the clear length and depth on a 10 cm module inside the type's measured range, which end the
hearth is at (the plan mirrored), where the hall ends, the passage's width, the door's place in a
cot's front. The type fixes the rest. The ridge runs along the longer side.

The filler never looks at the ground: whoever places the house cuts or fills its pad.

## The description a plan becomes

`describe(plan, choice, name, seed, out)` (`describe.h`) writes the plan and the shell it stands in
as the structures capability's `engine.structure.Description`
([structures](structures.md)), and `fill_rural(plot, household, style, seed, name, out)` is the
whole filler: choose, plan, describe, and the mesher's own `validate`. The description's frame is
the plan's moved by half the outer wall's thickness on both axes (`description_offset_cm`): the
plan lies between the outer walls' inner faces, the description on the walls' centre lines, both
from (0, 0), the front wall along z = 0 and the lane on its -z side.

| The plan's | becomes |
|---|---|
| the clear rectangle and the outer wall's thickness | the footprint; four exterior walls a storey (left, back, right, front, the footprint's order). A wall names one room: the one its door opens into, else the first that meets it. A wall a room (split at each partition, joined as a star) was weighed and left: the mesher keeps a pier of 15 cm and half the wall's thickness, 40 cm, between an opening and its wall's end, and the head landing's 60 cm window stands in a strip a metre wide |
| storeys | `Storey` records: 2.4 m floor to floor under an upper storey and 2.5 m from the upper floor to its plate, 2.4 m to the plate for a house of one; a one-room house is open to its roof, divided rooms are ceiled. The upper storey is that high because a thatch's eave hangs 54 cm under the plate: at 2.2 m the first pictures showed every upper window's head behind it |
| `Room` | `Room`, its id its index, its outline on the centre lines. An inner room is a `parlour`, a stair's compartment a `passage`; `name` keeps the plan's word |
| `InnerWall`, `Doorway` | `Wall{inner}` from one exterior wall's centre line to the other's (a T at each end), `Opening{doorway}` |
| `OuterOpening` | `Opening{door}` or `{window}` in its side's wall, `at` measured the way that wall runs |
| `Stair` | `Stair{straight}`: the foot at the middle of the first riser, the direction, the risers, a going of 28 cm, the well. Its width is the strip's clear width and 2 cm, so the steps run a centimetre into each wall and no face of a step lies in a wall's face |
| `Hearth` | `Chimney`: a wall stack runs through its wall and stands 8 cm proud outside, as the mesher's example does; every stack rises from the footing to 75 cm over the roof where it comes through. The recess, the raised hearth and the timber over the opening are the mesher's, from the chimney's `hearth_*` fields (the two `Box` members this row wrote before the mesher drew a hearth are gone) |
| the choice's ridge | one gable `Roof` over the outer faces, its pitch, overhang and covering the mesher's example's ([the shell's grounding](../experiments/vernacular-shell.md)). **Where an outer door stands under the eave** on the storey the roof sits on (every house of one storey), the overhang is cut back until the eave's edge clears the door's head by 2 cm: 32 cm at a 2.4 m plate under a fifty degree roof and a 2.0 m head. The mesher's validator refuses a way out under a lower eave, and the walker needs 1.85 m clear under the frame, a head of 1.93 m or more |

Every coordinate of a description lies within 15 m of its origin, the roof's outer faces and a
stack's proud face among them, so a house's clear length is at most 14.1 m at the default wall
(`max_clear_cm`); the filler's ranges stop there.

**What an imported or hand-authored building gets.** Nothing here is the filler's alone: a
description written by hand with rooms, inner walls, doorways and a stair is meshed and walked by
the same code, and `plan_interior` lays a plan into any clear rectangle an author names.

| timber and furniture ([below](#timber-and-furniture)) | a `Box` a member, in the room it stands in; the one member of each furniture piece that covers its footprint `solid`, the rest and all the timber not |

**What a house's seed varies in its shell.** Eight houses of one village are not one wall, one
window and one door: `describe` states the mesher's optional fields
([structures](structures.md)) from draws on the house's seed, one draw a statement and a purpose
each, so a field added later moves no other.

| Stated | Drawn | Fixed |
|---|---|---|
| `Wall.wave` on the exterior walls | 8, 12 or 16 mm, 12 twice as likely | a partition stays flat: waved, its faces cost a nine-room house two thousand triangles |
| `Wall.top_thickness`, a batter | two one-storey houses of three: seven eighths of the foot's thickness at the plate (three quarters read as a tent in the mesher's pictures) | never on two storeys (the mesher carries a lean cleanly on one) |
| `Roles.wall_outer`, the limewash | of sixteen houses six the aged base, five white, three cream, one ochre, one pink | the inside stays the base |
| `Roof.role`, the thatch's age | of sixteen houses eight the base (some years old), six old, two new | |
| `Roof.course`, `course_step_mm`, `eave_cut` | courses 26, 30 or 34 cm apart, their butts standing 10, 12 or 15 mm; the eave cut square on three houses of four, rolled on the fourth | the pitch, the coat and the verge are the mesher's example's |
| `Description.arris` | 6 or 12 cm | 12 under a batter |
| `Description.plinth_proud` | 0, 3, 4 or 5 cm | |
| a window's `mullions` and shutters (`leaf`, a pair on a window 80 cm wide or more) | one of four a house: glazing bars; mullions and shutters; mullions; bars and shutters | one mullion to every 17 cm of light; shutters folded back; none on a window under 70 cm (the head landing's), whose leaf would stand out of its reveal |
| an outer door's `leaf` | | every outer door: ledged, opening inward, open a quarter turn (the walk goes through it), hung on the jamb nearer its room's corner so the open leaf stands by a wall |
| a partition's doorway as a `door` with a leaf | two of three, and which room it opens into | hung on the jamb nearer the outer wall, open a quarter turn; a landing's 80 cm doorway stays bare, because a frame would leave less than the walker's 66 cm |

`RuralStyle::dressed = false` writes the plain shell. `tests/describe_tests.cpp` holds that eight
seeds of one household give at least five different shells (they give eight), that some and not
all are battered, mullioned and shuttered, and that every leaf hangs in a door or a window and
stands open.

**Budgets.** Measured by `tests/describe_tests.cpp` over 324 houses (`msvc-debug`, the mesher at
`k_structure_version` 5) and by `engine-content dwelling` on five of them: bare (`--bare all`),
with the timber and the furniture (`--bare dress`), and as the filler writes them.

| House | Bare | With timber and furniture | Dressed, as written | Members |
|---|---|---|---|---|
| a cot, 4.9 m by 4.1 m clear | 1,416 | 1,644 | 3,512 | 19 |
| a hall and parlour on one floor | 1,046 | 1,514 | 4,284 | 39 |
| a two-up two-down | 2,030 | 3,242 | 7,510 | 101 |
| a hall and parlour on two storeys | 2,374 | 3,538 | 8,597 | 97 |
| a two-storey cross-passage house with an inner room, 13.5 m by 5.5 m clear | 3,172 | 4,636 | 11,488 | 122 |

**The walls' wander is the cost**: a waved face is a 45 cm grid, and it more than doubles a house
(the largest of the 324 is 11,914). A member is a closed box of twelve triangles, so the timber and
the furniture are a third of a plain house and an eighth of a dressed one; a leaf is 84 triangles,
a shutter 48. The test holds every house under 16,000, where it held 6,000 before the walls
wandered; plan 14's budget for a building's exterior is 30,000. A bare cot was 488 to 640 triangles
and the largest house 2,848 at `k_structure_version` 1: the rest of the bare column is the mesher's
thatch, roof timbers and hearth. What the cluster DAG does to them at a distance is the mesher's
([structures](structures.md)).

## Timber and furniture

`furnish.h`. What a room shows besides its walls, **every piece made of members** — axis-aligned
boards, posts and rails in whole centimetres — and every member a plain `Box` of the description,
so a hand-authored `.structure.json` states the same pieces and no mesher code is the filler's
alone. `add_piece(furnishing, kind, footprint, against, storey, room)` builds any furniture piece
into a rectangle its author names. The sizes and their sources are the grounding's
([timber](../experiments/vernacular-interiors.md#timber),
[furniture](../experiments/vernacular-interiors.md#furniture)).

| Piece | Members | Where |
|---|---|---|
| joists | one a joist, 12 by 10 cm at 52 cm centres | under an upper floor, a room at a time, across the ridge, their ends 2 cm in the walls; never over a stair's compartment |
| beam | one, 24 cm wide, 18 cm under the ceiling | along the ridge under the joists' middle, where they span 3 m or more |
| nosings | one a riser, 2 cm past it and 3 cm deep | every tread's edge and the head landing's |
| newel | one, 9 by 5 cm | a half-newel against the wall on the climber's right, astride the first riser and the last (5 cm of wall is all that stands between a landing's doorway and the flight) |
| table set | 20: a top, four legs, four rails, three stretchers, and two benches of a seat, two slab ends and a stretcher | a hall, a kitchen, a two-up two-down's parlour: under a window where one is free, else along any wall |
| settle | 5: a back, two ends, a seat, a front board | a heated room that is lived in: the nearest free place to the stack, within 3.2 m |
| bed | 10: four posts, a head and a foot board, two rails, a mattress and a bolster in the ceiling's white | a parlour with no chamber over the house, an inner room, a chamber, a cot's one room: a corner before the middle of a wall, far from the way in, its head or its side to the wall |
| chest | 4: two ends, a body, a lid | beside the bed |

**Never in the tour's way.** `furnish_interior` places each piece against a wall of its room on a
10 cm module, and a place is free when the piece keeps the tour's own 35 cm from the tour's line,
stands in no doorway's approach (90 cm each side, 20 cm past each jamb) and no outer door's (1 m,
for a leaf), not before a hearth (1.1 m) or in a stack, not before the upper storey's first window
(the one the village's walker is led to) and 10 cm from the piece before it. Only a settle is kept
from the other windows: its back stands over a sill. A piece that fits nowhere at its least size is
left out, so a room keeps its way through before it keeps its table: over 648 houses 740 of 784
lived-in rooms have a table, 584 of 704 heated ones a settle, and 928 of 944 slept-in rooms a bed
(`tests/furnish_tests.cpp` holds nine in ten, six in ten and nine in ten). A cot's one room takes
its bed before its table. `validate_furnishing` reads all of it back and repairs nothing.

**What varies, from the house's seed** (one draw a purpose and a room): which of two equal places
a piece takes, the bed's width (a 137 or a 122 cm mattress first), the chest's length (90 to
120 cm). What a room's use fixes: which pieces it gets and in what order they are placed. The
timber draws nothing.

**The walk records carry the furniture**: `structures::walk_records` lists a description's solid
boxes, so a table's top, a bench's seat, a settle's seat, a mattress and a chest's body reach
collision and a walker meets them; legs, rails and all the timber are seen and not walked into.
`RuralStyle::furniture` and `RuralStyle::timber` leave either out (a bare house is the same plan).

## The filler as a tool

`engine-content dwelling <out-dir> [--name <n>] [--persons <n>] [--means <0|1|2>] [--frontage <cm>]
[--depth <cm>] [--seed <n>] [--scope whole|exterior|interior] [--bare timber|furniture|dress|all]`
(`--bare` may be given more than once) runs
the filler once and writes three files: `<n>.structure.json`, the mesh generator `structure`'s
recipe, which a scene names like any generated mesh; `<n>.plan.txt`, each storey drawn as text; and
`<n>.walk.json`, the tour and the views in metres in the description's frame (`tour`: points with
the floor's height under each and the room; `views`: an eye and a point looked at, a room;
`pieces`: every piece of timber and furniture, its kind, room, members and footprint). `--bare`
leaves the timber, the furniture, the shell's dress or all three out of the description. It prints one JSON line: the type,
storeys, rooms, the clear and outer sizes, whether the plot demoted the house, the triangles, and
the pieces and members.
What it writes is a starting point its author owns; nothing runs it again.

## The tour and the views

`tour_interior(plan, outside_cm, points)` (`tour.h`) is a walk through the whole house read from
the plan: in at the first outer door, a depth-first visit of every room through the doorways the
graph has, the ground storey's rooms before the climb, and an end on the stair's head landing in
front of its window. Every leg is parallel to a wall and keeps 0.35 m from every wall's face and
every stack (the walker's capsule is 0.3 m in radius and stops 0.32 m from a face), except where it
crosses a doorway through its middle. It exists so that a walk test and a camera path for any house
a filler or an author wrote need nobody to measure the house: the collision's case walks the line,
and a flight follows it. `room_views(plan, eye_height, views)` gives one place to stand a room, just
inside the way in, and the point to look at: the room's farthest window, a passage's other door, a
stair's flight from its foot, the landing's window from the top of the flight.

`draw_interior(plan, storey, cell_cm)` draws a storey as text, for a person or an agent reviewing a
plan in a terminal.

**Invariants.** Each is a test (`tests/interiors_tests.cpp`, `tests/rural_filler_tests.cpp`,
`tests/tour_tests.cpp`):

- The tour reaches every room of every plan type and of the filler's houses, every leg parallel to
  a wall, every point 0.35 m inside its room and clear of every stack, every change of room through
  a doorway's middle or up a stair, ending on the head landing; every room has a view, and a
  habitable room's looks at a window.

- Every plan type at its least and at a generous size, mirrored and not, over seeds, passes every
  validator; every room is reached from an outer door.
- A stair's rise is at most 210 mm and at least 150 mm, twice the rise plus the going lies in
  550–700 mm, its pitch is at most 42°, and every tread and both landings have 2.1 m of headroom,
  over storeys from 2.3 m to 3.0 m; a storey of 2.25 m is refused for its foot landing.
- A door under an eave keeps its head under the eave's edge, in every household's house, and
  1.85 m clear under its frame (`tests/describe_tests.cpp`).
- No window's span, with its pier, meets an inner wall's end, a stack or another opening.
- The validators find a slab over the flight, a low upper storey, a window moved in front of a
  wall, a doorway removed, a doorway narrower than the walker, a stack in a doorway's approach, and
  a room with no window.
- A rectangle too small for its type is refused with a sentence.
- Every house the filler plans, over three plots, nine household sizes, three means and forty
  seeds, passes the same rules and stands inside its plot; the household fixes the type; sizes stay
  in the measured ranges; a plot too small for a cot is refused.
- Every furniture piece of 648 houses stands on its room's free floor, clear of the tour's line by
  35 cm (measured again in the test, from the tour's points), of the approaches, the stacks and the
  other pieces; a stair's compartment, a passage and a service room hold none; every piece is made
  of members inside its footprint with one solid; a wrong size is refused
  (`tests/furnish_tests.cpp`). The walk records hold a box for every solid member and no other
  (`tests/describe_tests.cpp`).
- Joists hang from the ceiling of every ground room under an upper floor but the stair's, at 52 cm
  centres, with no member under a door's head; a stair has a nosing a riser and two newels; a
  house of one storey shows no joists.

**Determinism.** Integer centimetres and millimetres, the engine's hash of the seed and a purpose,
one draw a statement; no floating point and no C library call in the module. `hash_interior` covers
every field of every record, and `tests/determinism_tests.cpp` pins it over every type and over the
filler's houses, `hash_furnishing` every piece and member (`tests/furnish_tests.cpp` pins it over
648 houses), and `tests/describe_tests.cpp` the descriptions' source hashes: pins taken on MSVC,
which Clang and GCC in the containers must reproduce. `k_interior_version`,
`k_rural_filler_version` and `k_furnish_version` move with any change to what is laid out, written
or placed.

**Public API.** `include/domain/dwellings/dwellings.h` (the three below), `interiors.h`
(`plan_interior`, `validate_interior`, `reachable_rooms`, `stair_headroom_cm`, `hash_interior`,
`interior_min_extent`, `draw_interior`), `rural_filler.h` (`household_asks`, `choose_rural`,
`plan_rural`), `furnish.h` (`add_piece`, `furnish_interior`, `timber_interior`,
`validate_furnishing`, `hash_furnishing`), `tour.h` (`tour_interior`, `room_views`), `describe.h`
(`describe`, `fill_rural`).

**Depends on.** `base`, `containers`, `hash`, `math`, and the `structures` capability with its
schema types (`structures_schemas`).

**What looked wrong in the first pictures** (one hall and parlour on a flat ground, `--shadows rt`,
`renderer.gi.mode=bounce`, 2026-11-14; a person's objections, most damaging first): a hearth was a
blank stone breast with a beam across it; every room was empty, with no furniture, no door leaves
and no glass; a ceiling had no joists and a stair no nosing, string or newel; and at the sky's
exposure a room lit by its windows was nearly black, so the pictures were taken four stops over
it. Since then the mesher cuts the hearth and hangs leaves and shutters, and the description
states the joists, the beam, the nosings, the newels and the furniture above. **No picture of a
furnished room has been judged yet** (2026-11-15: the pictures stood in the GPU's line when this
was written), so what the members look like in a room's light — a bed's linen drawn with the
ceiling's plaster, legs and rails as plain boxes with no chamfer, a stair with newels and no rail
— is still to be said from a picture. Still missing whatever the pictures show: glass, a string
under the stair, and the black room at the sky's exposure, which is the renderer's.

**Testing.** `tools/dev.ps1 test -Filter dwellings`.

**Performance notes.** No hot path: a plan is made once when a filler or an author asks. A house's
plan is a few hundred bytes (the size table pins each record). **A scene with no dwelling pays
nothing**: the module registers nothing and runs nothing until called.

## What is not built

- **Lofts and half storeys**, the lobby-entry house and the winder: the grounding page says why.
- **A stair's handrail and balusters**: a rail slopes and a `Box` does not; it waits for a member
  between two points in the description. **What a service room, a passage and a hearth hold**
  (a dresser, stools, chairs, a cupboard, the fire's irons): the grounding opened no inventory.
  **The furnisher as a table of rows** a city's rooms can share (G66's last clause, G44): the
  pieces and the placement are functions today.
- **Footprints that are not rectangles**, and a back range or outshut: plan 14's rural filler takes a
  polygon or a circle; this one takes a rectangle, because every grounded plan is one.
- **`structure.fill` as a protocol method**, and the filler's provenance on a document record: they
  come with the records (D37).

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph,
the scheduler, or another capability, and it can be removed from the build the same way.

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | `structures` | `engine_capability_requires(dwellings structures)`: it writes that capability's description |
| Component and event types | none | a plan is plain records; nothing ticks |
| Tick scheduler entry | none | nothing ticks |
| Render-graph passes | none | it draws nothing |
| Content-build derived step | none | a plan is made when asked, never cached |
| Protocol methods | none | `structure.fill` comes with the records; `engine-content dwelling` is the filler as a tool until then |
| Tunables | none | a caller's style is a struct; a plan is a pure function of it |
| LOD policy | `dwellings::k_lod`: none of its own | description data; the mesher's interior mesh and its DAG are what a distance changes |
| Determinism | `dwellings::k_determinism` | integer, pinned on three compilers |
| Zero cost when unused | nothing registered, nothing run until called | holds |
| Tests and size table | `tests/interiors_tests.cpp`, `tests/rural_filler_tests.cpp`, `tests/determinism_tests.cpp`, `tests/size_table.cpp` | pass |
| Bench | none | no hot path |
| Removal proof | `ENGINE_WITH_DWELLINGS`, off in the minimal build | works |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_DWELLINGS=OFF`) drops the
module and its tests; the module disappears from `build/<preset>/modules.json` and is listed there
under `disabled_capabilities`. Everything else must still build and pass.
## What the village's walk and pictures changed

2026-11-15, by the village's leg after this module's own agents had stopped (`describe.cpp`, each
with its reason beside the line; `k_descriptions_pin` re-taken, MSVC's alone until a container
runs it). The walker (a capsule 0.6 m across with 2 cm of padding, on a collision proxy of the
mesh good to 5 cm) was walked through a dressed, furnished house for the first time, and stopped
three times at things that were the house's:

- **A partition doorway made a door is 10 cm wider than the plan's doorway**, five each side: a
  frame takes 12 cm and an open leaf 5.5, so an 85 cm doorway left 67.5 cm, 3.5 more than the
  walker; at 95 cm it is the outer door's 77.5. The plan, its approach and the furniture's
  clearances are unchanged.
- **A partition's leaf never opens into a passage or a stair's strip**: a leaf a quarter turn
  open stands square to its wall, which in a passage a metre wide is across it.
- **A back door's leaf stands shut**: open, a cross-passage's back door stood 18 cm across the
  stair's doorway, which opens off the passage 15 cm from the back wall.

And one thing the pictures decided: **a roof states no courses and no cut eave**. With stepped
courses and a square-cut eave a street of roofs read as slate or shingle; with a smooth coat and a
rolled eave the same roofs read as thatch. The draws stay in `Dress`; the thatch's age still
varies by its role.
