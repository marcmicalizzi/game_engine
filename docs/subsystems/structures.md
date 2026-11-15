# structures (domain)

**Purpose.** A building is a **description**, and this module owns it: the type
(`engine.structure.Description`, `domain/structures/schemas/structures.schema`), the one **mesher**
that turns a description into geometry by material role, the **walk records** beside the mesh
(floors, walls, stair flights and openings as plain records, for collision, walking tests and
validators), the **document record** that holds a description where it stands
(`engine.structure.Building`, roadmap D37), and the mesh generator `structure`, which builds a
`<name>.structure.json` file into a `.clusters` container like any other generated mesh. It is the
first built piece of [plan 14](../plan/14-structures.md) (roadmap G65, with the least of G64 and
G70 the mesher needs).

It does **not** lay out rooms or choose where a door goes (the interiors' generator and the rural
filler, G66 and G72), place a building on a ground or lay out a village (G69), or make a collision
body (scene_collision's, from the mesh's proxy and these records, S89). A filler writes
descriptions; nothing here writes one but `example_cot`, and nothing re-derives a description
behind its author's back.

**Why this shape.** Plan 14 §14.1's stance: the description is the authored artefact and a grammar
is only a filler, so everything that reads a building — the mesher, the validators, collision, an
agent reviewing a village in numbers — reads the same integers. Three choices follow.

- **The city's description, generalised, not a second one.** `domain/city` already describes a
  building as flat arrays: floors, walls as centre lines with a thickness and the spaces either
  side, openings as a span of their wall with a sill and a head, all integer centimetres
  ([city](city.md#the-building-grammar)). This description keeps those fields under the same names
  (`Wall.from`/`to`/`thickness`/`a`/`b`, `Opening.wall`/`at`/`width`/`sill`/`head`, a storey's
  `elevation` and `height`) and adds what a village house needs and the city never had: a wall's
  `bulge` (an arc; refused until it is meshed), **roofs**, **stairs**, **chimneys**, **material
  roles**, and rooms with outlines instead of rectangles. The city's urban grammar is not touched;
  it becomes one more filler of this type when it takes the mesher for its proxies (P27). What was
  *not* reused: the ruins' kit and assembler ([ruins](ruins.md#the-kit)) place imported members at
  sockets, which is the opposite of a solved shell, and the walls capability's solver
  ([walls](walls.md)) lays dry stone along a line over the ground and knows nothing of an opening.
- **One mesh a building, by surface, not a part an instance.** Plan 14 §14.3: instancing parts
  multiplies instances and culling pairs by the part count. A description meshes into nine
  **surfaces**, each one material, clustered apart and merged into one DAG
  (`geometry::merge_cluster_lod`), as a footbridge's three are ([bridges](bridges.md)).
- **The interior is described, not attached.** Inner walls, inner doorways, the floors between
  storeys and the stairs are fields of the same description, meshed by the same pass, so a wall is
  a wall whichever side of the house it is on and a doorway through an inner wall is cut by the
  code that cuts a window. What the interiors' generator adds is data; where it needs new geometry
  there is one file for it ([the interiors' seam](#the-interiors-seam)).

**Owned data.** The `engine.structure.*` schema types; `k_structure_version`; the surfaces and
their fallbacks; the walk records' layout.

## The description

Every length is whole **centimetres** in the building's own frame: x and z on the ground, y up,
the origin at the ground floor's top. "Left" of a direction is its left seen from above with y up:
the left of +x is -z, so an outline that runs **counter-clockwise seen from above** (+z first, then
+x) has its inside on its left.

| Field | What it is |
|---|---|
| `footprint` | The exterior walls' centre lines as one outline, `x0 z0 x1 z1 ...`: what every floor spans. |
| `foundation`, `plinth_top`, `plinth_proud`, `arris` | How far below the origin the exterior walls reach into the ground, the top of the plinth band at their foot, how far that band stands proud of the wall over it, and the radius the walls' outer corners are rounded to ([faces that are not planes](#faces-that-are-not-planes)). |
| `storeys[]` | `elevation` (the floor's top), `height` (to the top of this storey's walls: the next floor's top, or the wall plate), `floor_thickness`, and for the top storey `ceiling`. Storeys stack: each one's elevation is the one below's elevation plus height. |
| `walls[]` | A centre line `from` to `to` on a `storey`, a `thickness`, a `kind` (`exterior`, `inner`), the rooms on its left (`a`) and right (`b`), -1 for outside, `bulge` (0: straight), and, stated or absent, `top_thickness` (a batter) and `wave` (millimetres its faces wander). |
| `openings[]` | A span of a wall: its centre `at` along the wall from `from`, its clear `width`, its `sill` and `head` above the storey's floor, and a `kind`: `door` (a frame and a threshold), `window` (a frame, glazing bars or `mullions`, a sill board), `doorway` (a bare opening). **What hangs in it is stated or absent**: `leaf` (`none`, `ledged`), `leaves` (1, or a pair), `hinge_at_to`, `opens_right`, `leaf_open` (degrees from shut, 0 to 90) ([leaves and mullions](#leaves-and-mullions)). |
| `rooms[]` | An `id`, a `storey`, a `kind`, a name and an outline on the walls' centre lines. |
| `stairs[]` | A flight from a storey's floor to the next: its `foot`, the quarter turn it climbs along, its `width`, `risers` and `going`, the `well` it leaves in the floor above, and the rooms at its foot and head. |
| `roofs[]` | A `form` (`gable`, `mono`; `hip` is refused until it is meshed) over a rectangle (the outer faces of the walls that carry it), the ridge's axis, the `eaves` height, the `pitch` as rise over run in permille, the `overhang` at the eaves and the `verge` at the gables, the covering's `thickness`, its own `role` (empty: `roles.roof`), and, stated or absent, a thatch's `course`, `course_step_mm` and `eave_cut`. |
| `chimneys[]` | A stack's rectangle, its `base` and `top`, and a hearth's side, storey, width and head. |
| `boxes[]` | Plain members: an axis-aligned box drawn with a surface's role (a hearth stone, a bench, a post), `solid` or not. |
| `roles` | The material role of each of the nine surfaces. |
| `provenance` | The filler, its version and seed when a filler wrote it: a record, never a recipe to re-run. |

**The rooms' graph** is read from the description, not stored beside it: two rooms are joined
where a `door` or a `doorway` stands in a wall that names both, or a stair runs between them, and
a link with -1 on one side is an entrance (`room_links`, `rooms_reachable`).

**The record (D37).** `engine.structure.Building` is a `@kind(record)` type: a name, a
description, a `worldpos` and a yaw. It is the type a document holds and a filler writes into a
layer. As of this change **nothing maps it to an instance yet**: `doc_scene` draws
`engine.scene.MeshPlacement` records only, and a non-capability module cannot name a capability's
type, so the mapping wants a registration point of doc_scene's (a row this leaves). A scene file
reaches the mesher today the way it reaches any generated mesh: an instance or a placement whose
mesh is a `.structure.json` file.

### What a description must satisfy

`validate` refuses, with a sentence naming the rule, the element and what was measured, and
repairs nothing: 1 to 4 storeys that stack, heights of 100 to 600 cm; an outline of at least three
corners that does not cross itself, every coordinate within ±15 m (so UVs in metres stay inside
`geometry`'s ±16 range); walls 5 to 150 cm thick, at least as long as they are thick, straight
(`bulge` 0), naming rooms that exist; openings 30 to 400 cm wide with a head at least 20 cm over
the sill, **15 cm of wall kept between an opening and its wall's end (past the half thickness the
join takes) and between two openings**, and the head 5 cm under the wall's top at both jambs on
both faces; **a door or a doorway to the outside not under an eave lower than its own head**
(outside the wall over the opening's middle, the roof's underside stands at least as high as the
head: a steep roof's overhang drops fast, and the first cot's 45 cm eave at fifty degrees hung at
1.56 m over a 1.9 m door); roofs over a non-empty rectangle with a pitch of 50 to 4000 permille; a
hearth that leaves its stack a back and two cheeks; a straight stair
of 2 to 40 risers with a storey above it; a well only in an outline of axis-aligned edges.
The walker's rules of plan 14 §14.4 (riser, going, headroom, a doorway's width) are **not** held
here yet: they are S89's validators, over the walk records.

## The mesher

`solve(description, scope, parts, report, error)` fills one `MeshPart` a surface: positions in
metres, normals, UVs in metres of surface, indices. It runs the shell (`src/shell.cpp`) and then
the interior (`src/interior.cpp`).

**Walls.** Each wall is solved once (`solve_walls`): a frame on its centre line and where each of
its two faces ends. A face is then a grid: its **stations** are its ends, its openings' jambs and
the point where a gable's ridge crosses it; its **levels** are its foot, the plinth's top, its
openings' sills and heads, and its top at every station. A cell is kept, dropped (inside an
opening, or above the top), or cut by the top's line, which runs through the grid's corners; the
face's corners are shared between cells, so a face is one welded sheet with no T-junction in it.
An exterior wall's outer face starts at `-foundation` and wears the plinth's role up to
`plinth_top`; every other face starts at its storey's floor.

**A wall rises to what stands on it.** Below the top storey that is the next storey's floor. On
the top storey, under a roof, it is the roof's underside at that point of the ground, so a gable
wall grows its triangle and an eaves wall's top follows the slope, with nothing described twice;
an inner wall stops at the ceiling where the storey has one.

### Joins

Walls are joined so that **no two faces lie in one plane over one another** and a corner is closed
without a lap:

- **A corner or a star** (two or more walls ending at one point): each side's line runs to the
  facing side's line of the neighbour on that side — the wall met first turning left for the left
  side, the one met last for the right — so the two faces meet in one edge: a mitre for two walls
  of one thickness, and the same rule for unequal ones and for three or four at a point. Which
  neighbour is which is decided in exact integer arithmetic on the centre lines; where the lines
  meet is one division.
- **A T** (a wall ending on another's span): both its sides run to the host's near face and it has
  no end face there.
- **A free end** is closed with a face.

A wall's top is drawn only where nothing stands on it (no storey above, no roof over it).

### Faces that are not planes

A wall of earth under a thick render is not a box, and four fields say how far a description's
walls are from one. **Each is absent unless stated, and a description that states none of them
meshes to the same bits it did before they existed** (the test holds the first cot's pin). They
key on what the description states, never on a role's name or on who wrote it.

- **`Wall.top_thickness`: a batter.** An exterior wall's outer face leans in from the plinth's
  top (or its storey's floor) to its storey's top, where the wall is `top_thickness` thick, and
  stands plumb above that (a gable). The inner face stays plumb. The joins are solved a second
  time with every outer face leaned in, so a corner between two battered walls is the leaning
  line both faces end on; the wall's top is the roof's underside over the leaned face; a reveal's
  jamb follows the face, in two pieces where it crosses the level the lean starts at; a frame
  stands plumb, its set kept at the head, and a sill board and a lintel stand proud of the face
  where it is at their own height. The validator wants an exterior wall with one side outside and
  a top at least half the foot and 25 cm. A batter is one storey's: an upper wall stands on the
  lower one's top as the description says, and the mesher does not carry a lean through a floor.
- **`Description.plinth_proud`.** The plinth band's face stands that far outside the wall's face
  over it, with a level **ledge** between them at `plinth_top`, stopped at a door (whose reveal
  is drawn out to the plinth's face under that level). Joins are solved a third time for it.
- **`Description.arris`: rounded corners.** Where two exterior walls meet at a corner that is
  convex seen from outside, both outer faces stop short of it and an arc of three strips joins
  them, from the foot to the roof's underside, the plinth's band on its own larger arc with its
  ledge, the wall's leaning as its faces do. The arc is a rational quadratic through the two
  faces' ends (a circle's arc with no trigonometric function), its normals the circle's, so the
  corner is lit round. Re-entrant corners, stars and free ends stay sharp, and so do the arrises
  of an opening's reveal (a row this leaves: a cob reveal is as round as its corner). The strip
  meets each face along one straight line but not vertex for vertex: the face's grid has corners
  there the strip does not.
- **`Wall.wave`: a face that wanders.** Both faces of the wall are drawn as a grid about 45 cm
  square and each corner stands out from its plane by up to `wave` millimetres: smooth value
  noise from integer mixing (a swell about 90 cm across and a quarter as much at 37), with the
  normals of that surface, so the light shows it. It dies away over 30 cm toward every edge the
  face must meet exactly (an opening, an end, the plinth's ledge or the floor, the top), and it
  never falls behind the plane, so a partition's end and a floor's edge stay buried in it. This
  is what costs triangles: the cot goes from 1,504 to 3,672, nearly all of it this grid.

What a filler should know: with a batter the roof's rectangle is still the walls' outer faces
at their **foot**, so the eaves overhang the leaned face by the lean more than `overhang` says.

### Openings

An opening is cut out of both faces by the grid above, so it is exactly the width and height
asked on every level of the DAG that keeps the face's corners. Its **reveal** — two jambs, a head
and a sill through the wall's thickness — is drawn in the inner wall's role; a door's sill is its
**threshold**, 20 mm over the floor (`k_threshold_mm`) in the floor's role, which is also what
keeps the reveal's foot out of the floor's plane. Its **timbers** are closed boxes, each sunk a
centimetre into whatever it meets rather than laid against it: a frame set back from the outer
face (a door's 60 mm back and 100 mm deep, a window's 80 and 70), a window's glazing bars (an
upright about every 450 mm, a transom in a light over a metre tall) and its sill board proud of
the wall, and over an exterior opening a lintel bearing 150 mm on the wall either side. A window
has no glass: there is no transmissive material to draw it with (a row this leaves).

### Leaves and mullions

**A description that states no leaf has none**: `Opening.leaf` is `none` by default and the
opening stands open, as every opening did before the field. `ledged` hangs a leaf in the frame
of a `door` or a `window` (a `doorway` has no frame and the validator refuses one there):
upright boards of unequal widths, about 19 cm each so a door of 75 to 80 cm has four, every other
one 3 mm proud so the joints catch the light, on **ledges** across the face the leaf opens
toward (three to a door, two to a shutter under 1.2 m), each ledge sunk a centimetre into its
boards. A door's boards are 3 cm and its ledges 2.5 by 15; a shutter's 2 cm and 2 by 9. There
is no brace: the grounding page has ledged-and-braced doors from the nineteenth century only.
There is no ironwork either (strap hinges, a latch): none of the nine surfaces is iron.

**Where it hangs.** The leaf's face is flush with the frame's face on the side it opens to
(`opens_right` false: the wall's left, which is the inside of an exterior wall drawn
counter-clockwise), its ledges and then its boards behind, inside the frame's depth. Its pivot is
that face's edge at the hinge jamb (`hinge_at_to` picks the jamb; `leaves` 2 hangs one on each
and they meet in the middle), and `leaf_open` turns it about the pivot toward the side it opens
to, 0 to 90 degrees. The sine and cosine are a polynomial of sums and products
(`detail::turn_deg`), exact at 0 and 90, so every toolchain hangs a leaf on the same bits. Open a
quarter turn in a thick wall, the leaf lies in the reveal beside its hinge jamb and **takes its
own thickness, 5.5 cm, off the clear width**, as a real one does; shut, it closes the opening.
`leaf_places` (`src/emit.h`) is the one function the mesher and the walk records both ask.

**Mullions.** `Opening.mullions` (a window's; 0 keeps the glazing bars) stands that many square
timbers, 4.5 cm a side, **set on the cant** between the sill and the head in the frame's middle,
evenly spaced: the unglazed window of an early cottage, closed by its shutters. It is how a
window reads as one until there is glass to draw. The validator wants 12 cm a light.

### Roofs

A roof is a slab over its rectangle and its overhangs: the covering on top, at the eaves and at
the verges, and an underside that is the plane the walls rise to. The pitch is rise over run in
permille, so no decision takes a trigonometric function; a slope's length and its normal take one
square root.

**A coat 15 cm deep or more is a soft one** (a thatch), and a thinner covering keeps its planes;
the rule keys on the thickness the description states, not on a role's name. A soft coat's
profile, from the eave's foot to the ridge, is the eave's face for the lower half of its depth,
rounded over the top fifth, the slope, and on a gable a **ridge course** standing 6 to 8 cm proud
over the last 45 cm (a thatch is laid thickest and capped at its ridge); its normals are the mean
of the segments each corner joins, and straight up at the ridge. A thin covering's profile is the
eave's plumb face and the plane. The verge is the profile's cut end. UVs run **u up the slope and
v along the ridge**, so a thatch's stems, drawn along its set's u, lie up the roof.

**A thatch's courses and cut eave** are stated or absent (`Roof.course`, `course_step_mm`,
`eave_cut`; only a soft coat takes them, and a roof that states neither is the mesh it was).
`course` lays the slope between the eave and the ridge course in courses that far apart up it
(the nearest whole number of equal ones, at most 40): each course's surface falls
`course_step_mm` square to the slope from its own butts to the next course's, which stand as a
step, so no course stands above the coat's depth; every course's face keeps its own plane, so
the light shows the steps where a smoothed coat would wash them out. With courses the ridge
course is a **block**: its skirt's lower edge is one cut step of 6 cm. `eave_cut` cuts the eave
**square to the slope**: its face looks down and out over the coat's whole depth, the coat's
top edge stays where the overhang puts it, and the underside starts in from that edge by the
depth times the pitch over the slope's length (23 cm for 30 cm at fifty degrees; never nearer
the wall than 6 cm). The verge's end is cut to the same face. What the mesh still lacks is on
the grounding page: the brow course, liggers, a ridge's pattern, the sweep up to a chimney.

**The underside** is the covering's own role for a soft coat (a thatch shows its straw between
its battens) and the `boards` role for a thin one.

**The roof's timbers** are drawn where the top storey is open to them (no `ceiling`): a rafter 8
by 10 cm about every 45 cm under each slope from the wall plate to the ridge, standing between
the end walls (a rafter inside a gable wall would show through its face); under a soft coat a
batten 5 by 2.5 cm every 30 cm up each slope; on a gable a collar across every third pair at
three fifths of the rise. Each is a closed box sunk a centimetre into what it carries.

### Chimneys

A chimney is its stack as a box from `base` to under its cap, and the cap a course 4 cm wider.
A hearth (`hearth_side` 0 to 3) opens the stack on that side over its storey's floor: there the
stack is two cheeks and a back round a recess `hearth_depth` deep and `hearth_width` wide, up to
`hearth_head`, on a hearth raised 3 cm (which keeps it out of the floor's plane), with a timber
across the opening bearing on the cheeks. The validator holds a back of 10 cm and cheeks of 8.
The stack is not cut out of the wall or the roof it passes through: it is a solid sunk through
them, and what a description must do is stand it proud of a wall's face on the side its hearth
opens, by at least the recess.

### The interiors' seam

`src/interior.cpp` is where what stands inside the shell is meshed, and it is the one file the
interiors' generator (G66) extends. Inner walls and inner doorways need nothing there: they are
`walls` of kind `inner` and `openings` of kind `doorway`, and the shell meshes them with the rest
(the wall's thickness, the opening cut through both faces, the reveal, a butt join where it meets
another wall). What the file holds today:

- **Floors.** Storey 0's floor is one sheet at its elevation over the footprint, in the `floor`
  role. A storey above is two: boards on top and, `floor_thickness` below, the ceiling of the room
  under it. A floor's edge lies on the walls' centre lines, inside the walls, so it shares no plane
  with a wall's face. **A stair's `well` is cut out of the floor it lands on** (and out of the
  ceiling beneath), with the floor's depth drawn round the hole.
- **Ceilings.** The top storey's `ceiling` is a sheet at the wall plate.
- **Stairs.** A `straight` flight is a box a step, from 12 cm under its tread to its top, the last
  riser the landing's own edge. `ladder` and `quarter` are in the type and refused by the
  validator until they are meshed here.
- **Plain members.** `boxes` are drawn as closed boxes in their surface's role.

A new kind of interior part is a new function in that file, called from `mesh_interior`, drawing
through `src/emit.h`'s emitters (`polygon`, `box`, `triangulate`) into `ctx.part(surface)`, and
asking `ctx.wants(false)` so the scope split below keeps working.

### Exterior and interior

`Recipe.scope` picks which mesh of a description a file builds: `whole` (the default), `exterior`
(the outer faces and the plinth, the reveals and timbers of exterior openings, the roofs, the
chimneys) or `interior` (the inner faces, the inner walls, the floors, the ceilings, the stairs,
the members in a room). The two halves are the whole between them, triangle for triangle; a host
that wants an interior resident only where a walker can reach it (plan 14 §14.7) names two files.

## Materials by role

| Surface | Default role | Behind it | Then |
|---|---|---|---|
| `wall_outer` | `plaster/lime` | | a limewash colour |
| `plinth` | `stone/rubble` | `rock/granite` | grey |
| `wall_inner` | `plaster/lime` | | a limewash colour |
| `roof` | `thatch/straw` | | weathered straw |
| `timber` | `wood/plank` | | dark oak |
| `floor` | `stone/flag` | `rock/sandstone` | grey |
| `boards` | `wood/plank` | | brown |
| `ceiling` | `plaster/lime` | | a limewash colour |
| `chimney` | `stone/rubble` | `rock/granite` | grey |

A surface takes its role's set from the host's pack ([materials](materials.md)), else the role
behind it, else a flat colour, and the lookup says once which role is missing. UVs are metres of
surface (a wall's u along it and v up, a floor's x and z, a roof's u up the slope and v along the
ridge, a timber's u along its grain), so a set tiles at its own `tile_m`.

**The four roles no pack had are generated sets now** (`plaster/lime`, `stone/flag`,
`stone/rubble`, `thatch/straw`: [matgen](matgen.md#a-buildings-surfaces)), each a recipe of a
generator kind that stood already, built by `engine-content matgen` with the rest. They are the
least that gives each surface a set of its own: a thatch's courses, a wall's coursing and a
floor's bond are not in them, and that page says so.

## Walk records

`walk_records(description, walk, error)` gives what collision and a walking test need without
reading a mesh, in the building's frame, metres: a `WalkFloor` a storey (its height and its
outline), a `WalkWell` a stair (the hole in the floor above), a `WalkWall` a wall (its centre
line, thickness, foot and lowest top), a `WalkOpening` an opening (its two jambs' feet on the
centre line, its sill and head, the wall's thickness and the rooms either side), a `WalkFlight` a
stair (its foot, direction, width, rise, going, risers and the height it lands on), a `WalkBox`
a solid member, and a `WalkLeaf` a leaf **where it stands**: a plumb slab `thickness` deep about
the line from `hinge` to `tip`, from `y0` to `y1`, with the degrees it stands open. An opening's
own record is the same with or without a leaf, so a consumer that reads no leaves walks through
a shut door; one that wants a shut door to stop a walker, or an open one to narrow the way,
adds the slab. Each carries the index of what it came from, so a failure names a description's
element.

## The file and the container

A `<name>.structure.json` file is an `engine.structure.Recipe`: a description and a scope. The
mesh generator `structure` (registered with `assets`' mesh-generator registry, `GENERATOR
mesh:structure`) reads it, refusing an unknown field and an invalid description, clusters each
surface that has triangles, merges the DAGs, and writes a material a surface with its role's set
through the tile contract. The source hash is over the recipe's canonical JSON and
`k_structure_version`; the build key is `assets::generated_mesh_build_key`.

Normal cones are **off** for a structure's clusters: every face is one-sided, but a doorway shows
the far side of a room, and the cone test's saving on a few hundred clusters does not pay for a
mistake there (to be measured with the village).

## The example

`example_cot(width, depth, wall)` is a one-room cot: a room 6 by 4 m inside walls 60 cm thick, one
storey 2.4 m to the wall plate and open to a thatched gable roof at fifty degrees whose eaves
overhang 35 cm (so the eave's edge stands 1.98 m over the floor, clear of the door's head), a door
and a window in the front wall, a window in the back, a stack with a hearth on one gable. The
same description is committed as `content/structures/cot.structure.json`. Its numbers and where they
come from are on [the grounding page](../experiments/vernacular-shell.md).

## Forms and the LOD policy

A building's forms are its mesh's cluster DAG: the finest level is the mesher's triangles, and
every coarser level is the builder's simplification across surfaces' own clusters. A whole cot is
a few hundred triangles of wall and roof and a few hundred of timber, so the DAG's root is the
whole building at a size the far field can afford. There is no authored far form yet.

## Determinism

`k_determinism` is `derived`. Every input is an integer; a decision is a sum, a product, one
division or `std::sqrt` in f64 (contraction is off tree-wide, ADR-0035), and which wall is a
wall's neighbour is exact integer arithmetic. No C library function decides anything. The same
description is the same bytes on every toolchain; `structures_tests.cpp` pins the hash.

## Invariants

Each is a case in `tests/structures_tests.cpp`:

- The example meshes into every surface it has.
- Every triangle is wound the way its normals face, none is empty, and every UV is inside ±16.
- An opening is clear where its description puts it (to the millimetre at a jamb) and the wall
  beside it is two faces thick.
- Walls meet at their corners in one edge and rise to the roof's underside.
- An inner wall butts the wall it meets and reaches nothing outside; a well is a hole in the floor
  and the ceiling; a flight's steps rise to the landing.
- The exterior and the interior are the whole between them.
- A description that states no leaf, mullion, batter, wander, rounding or proud plinth is the
  mesh it was before those fields, to the bit.
- A battered, waved, round-cornered shell on a proud plinth is closed: a line from outside to
  inside through a corner meets the outer skin once at every height, and the ledge is there.
- A leaf is four boards and three ledges of closed timber, shut across its opening or, a quarter
  turn open, clear of it; the walk records carry it where it stands; a doorway takes none.
- The validator refuses by name.
- A recipe's canonical form reads back to itself.
- **The conformance corpus** (`content/structures/`, plan 14 §14.1): every committed description
  meshes, every opening of it is clear where it is described with the wall solid beside it, every
  room's floor is at its storey's height, every room is reached from outside, and the committed
  cot is `example_cot()` to the byte of its canonical form. A mesher change may move triangles and
  the mesh pins; it may not fail this.
- The DAG's cut of the cot never grows with distance, and a kilometre off it is under half the
  finest.
- A way out is not under an eave lower than its head (the validator's refusal).

**Public API.** `include/domain/structures/structures.h`.

**Depends on.** `base`, `containers`, `math`, `hash`, `json`, `schema`, `log`, `io`, `assets`,
`geometry`, `texture`, `materials`, `structures_schemas`.

**Testing.** `tools/dev.ps1 test -Filter structures`. Benchmarks: `tools/dev.ps1 bench -Filter
'structures.*'`.

## Budgets

Measured on the example cot (`structures_tests.cpp` prints them; 2026-11-14, `msvc-debug`, so
counts and no times):

| What | Value |
|---|---|
| Triangles, finest | 1,192 (the test holds it under 4,000) |
| Clusters, finest cut | 38; 60 with the DAG's coarser levels |
| The DAG's cut from 5 m and 50 m (2160 lines, 60° field of view, 1 px) | 1,192 triangles in 38 clusters: the finest |
| The cut from 200 m | 1,048 triangles in 33 clusters |
| The cut from a kilometre | 472 triangles in 14 clusters |
| The DAG's root | 194 triangles |
| Materials (surfaces with triangles) | 7 |

**Against plan 14 §14.7's estimate** (a timber-frame house's exterior at 30,000 triangles and its
interior at 45,000): the cot is 1,192 because nothing in it is a kit part yet. Its thatch is a
profile, not bundles; its boards are a face, not planks; its timbers are boxes. The budget a
house is held to is the plan's until the kit (G64) lands and is measured: **30,000 triangles for
an exterior, 2.5 times that with its interior**, and a house a few hundred triangles past a
kilometre.

**Why the cut barely coarsens inside 200 m.** The finest level is so coarse already that its
first simplifications move a surface by about 10 cm (a frame's section, a reveal's depth),
which is a pixel at 200 m. That is the right answer for a mesh this small, and it will stop
being one when the kit multiplies the triangles: then the first levels should drop the parts
nobody resolves (battens, glazing bars) long before the walls move, which wants per-part error
or an authored mid form, as the walls capability has ([walls](walls.md)).

The mesher's time is not measured in a release build yet (`structures.solve`,
`structures.walk_records` in the bench).

**What a scene with no structure pays.** Nothing: the generator is reached through the registry
by a file's name, and a scene that names no `.structure.json` never calls it.

**What a hand-authored description gets.** Everything: the mesher, the walk records and the
validator read a description and nothing else, so a file written by hand and one a filler wrote
are the same thing.

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph,
the scheduler, or another capability, and it can be removed from the build the same way.

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | none | this capability stands alone |
| Schema types | `domain/structures/schemas/structures.schema` | the description, the recipe, the record |
| Tick scheduler entry | none | a description is meshed at a content build |
| Render-graph passes | none | cluster geometry like any other |
| Content-build derived step | the mesh generator `structure` | registered |
| Protocol methods | none | `structure.fill` is the fillers' |
| Tunables | none | every number is the description's |
| LOD policy | the mesh's cluster DAG | above |
| Determinism | `structures::k_determinism` = `derived` | pinned |
| Zero cost when unused | the registry: no file, no call | |
| Tests and size table | `tests/structures_tests.cpp`, `tests/size_table.cpp` | |
| Bench | `domain/structures/bench/structures_bench.cpp` | `structures.solve`, `structures.walk_records` |
| Removal proof | `ENGINE_WITH_STRUCTURES`, off in the minimal build | |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_STRUCTURES=OFF`) drops the
module, its tests, and its bench; the module disappears from `build/<preset>/modules.json` and
is listed there under `disabled_capabilities`. Everything else must still build and pass.

## Not yet

- Arcs and splines (`Wall.bulge`), hip, cone, shell and sweep roofs (plan 14 §14.3).
- The `Door` record with its state (S91): a leaf is drawn where the description says and does
  not move; a leaf that swings is a mesh of its own.
- Glass in a window, and iron (strap hinges, latches): no surface draws either.
- The kit of solved parts beyond boxes: hewn beams, warped planks, thatch bundles, shingles (G64).
- The record drawn from a document (D37's other half).
- Rounded arrises at an opening's reveal and at a re-entrant corner; a batter carried through
  a floor; a leaned top and end face on a wall that stands free (they are drawn plumb).
- A thatch's brow course, liggers at the eaves and verges, a patterned ridge, and its sweep up
  round a chimney.
