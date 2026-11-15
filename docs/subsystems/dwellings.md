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
  interior plan shares none of the mesher's code: it needs `base`, `containers` and `hash`. The edge
  runs one way, from this module to the description it fills, as the village filler's does. If the
  two are folded together later it is a move of files; no plan changes.
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
The upper floor has no slab over `well()`, the strip less the head landing: headroom over the
flight is to the upper storey's ceiling, and `stair_headroom_cm` is the one function that says so.

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
| the clear rectangle and the outer wall's thickness | the footprint; four exterior walls a storey (left, back, right, front, the footprint's order). A wall names one room: the one its door opens into, else the first that meets it |
| storeys | `Storey` records: 2.4 m floor to floor under an upper storey, 2.2 m to the plate for a house of one; a one-room house is open to its roof, divided rooms are ceiled |
| `Room` | `Room`, its id its index, its outline on the centre lines. An inner room is a `parlour`, a stair's compartment a `passage`; `name` keeps the plan's word |
| `InnerWall`, `Doorway` | `Wall{inner}` from one exterior wall's centre line to the other's (a T at each end), `Opening{doorway}` |
| `OuterOpening` | `Opening{door}` or `{window}` in its side's wall, `at` measured the way that wall runs |
| `Stair` | `Stair{straight}`: the foot at the middle of the first riser, the direction, the risers, a going of 28 cm, the well. Its width is the strip's clear width and 2 cm, so the steps run a centimetre into each wall and no face of a step lies in a wall's face |
| `Hearth` | `Chimney`: a wall stack runs through its wall and stands 8 cm proud outside, as the mesher's example does; every stack rises from the footing to 75 cm over the roof where it comes through. Before it, two `Box` members: the hearth stone and the bressumer over the opening |
| the choice's ridge | one gable `Roof` over the outer faces, its pitch, overhang and covering the mesher's example's ([the shell's grounding](../experiments/vernacular-shell.md)) |

Every coordinate of a description lies within 15 m of its origin, the roof's outer faces and a
stack's proud face among them, so a house's clear length is at most 14.1 m at the default wall
(`max_clear_cm`); the filler's ranges stop there.

**What an imported or hand-authored building gets.** Nothing here is the filler's alone: a
description written by hand with rooms, inner walls, doorways and a stair is meshed and walked by
the same code, and `plan_interior` lays a plan into any clear rectangle an author names.

**Budgets.** Measured by `tests/describe_tests.cpp` over 324 houses (`msvc-debug`, the mesher at
`k_structure_version` 1): a cot is 488 to 640 triangles, a two-storey hall and parlour about 2,050,
a two-up two-down about 1,760, and the largest, a two-storey cross-passage house with an inner room
13.5 m by 5.5 m clear, 2,848. The test holds every house under 6,000. What the cluster DAG does to
them at a distance is the mesher's ([structures](structures.md)).

## The filler as a tool

`engine-content dwelling <out-dir> [--name <n>] [--persons <n>] [--means <0|1|2>] [--frontage <cm>]
[--depth <cm>] [--seed <n>] [--scope whole|exterior|interior]` runs the filler once and writes
three files: `<n>.structure.json`, the mesh generator `structure`'s recipe, which a scene names like
any generated mesh; `<n>.plan.txt`, each storey drawn as text; and `<n>.walk.json`, the tour and
the views in metres in the description's frame (`tour`: points with the floor's height under each
and the room; `views`: an eye and a point looked at, a room). It prints one JSON line: the type,
storeys, rooms, the clear and outer sizes, whether the plot demoted the house, and the triangles.
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
  550–700 mm, its pitch is at most 42°, and every tread has 2.1 m of headroom, over storeys from
  2.25 m to 3.0 m.
- No window's span, with its pier, meets an inner wall's end, a stack or another opening.
- The validators find a slab over the flight, a low upper storey, a window moved in front of a
  wall, a doorway removed, a doorway narrower than the walker, a stack in a doorway's approach, and
  a room with no window.
- A rectangle too small for its type is refused with a sentence.
- Every house the filler plans, over three plots, nine household sizes, three means and forty
  seeds, passes the same rules and stands inside its plot; the household fixes the type; sizes stay
  in the measured ranges; a plot too small for a cot is refused.

**Determinism.** Integer centimetres and millimetres, the engine's hash of the seed and a purpose,
one draw a statement; no floating point and no C library call in the module. `hash_interior` covers
every field of every record, and `tests/determinism_tests.cpp` pins it over every type and over the
filler's houses, on MSVC, reproduced by Clang and GCC in the containers. `k_interior_version` and
`k_rural_filler_version` move with any change to what is laid out.

**Public API.** `include/domain/dwellings/dwellings.h` (the two below), `interiors.h`
(`plan_interior`, `validate_interior`, `reachable_rooms`, `stair_headroom_cm`, `hash_interior`,
`interior_min_extent`, `draw_interior`), `rural_filler.h` (`household_asks`, `choose_rural`,
`plan_rural`), `tour.h` (`tour_interior`, `room_views`).

**Depends on.** `base`, `containers`, `hash`.

**Testing.** `tools/dev.ps1 test -Filter dwellings`.

**Performance notes.** No hot path: a plan is made once when a filler or an author asks. A house's
plan is a few hundred bytes (the size table pins each record). **A scene with no dwelling pays
nothing**: the module registers nothing and runs nothing until called.

## What is not built

- **Lofts and half storeys**, the lobby-entry house and the winder: the grounding page says why.
- **Furniture** (G66 names it): no row is grounded yet.
- **Footprints that are not rectangles**, and a back range or outshut: plan 14's rural filler takes a
  polygon or a circle; this one takes a rectangle, because every grounded plan is one.
- **`structure.fill` as a protocol method**, and the filler's provenance on a document record: they
  come with the records (D37).

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph,
the scheduler, or another capability, and it can be removed from the build the same way.

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | none | the plan stands alone |
| Component and event types | none | a plan is plain records; nothing ticks |
| Tick scheduler entry | none | nothing ticks |
| Render-graph passes | none | it draws nothing |
| Content-build derived step | none | a plan is made when asked, never cached |
| Protocol methods | none | `structure.fill` comes with the records |
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
