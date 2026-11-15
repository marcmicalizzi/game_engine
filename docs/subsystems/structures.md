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
| `foundation`, `plinth_top` | How far below the origin the exterior walls reach into the ground, and the top of the plinth band at their foot. |
| `storeys[]` | `elevation` (the floor's top), `height` (to the top of this storey's walls: the next floor's top, or the wall plate), `floor_thickness`, and for the top storey `ceiling`. Storeys stack: each one's elevation is the one below's elevation plus height. |
| `walls[]` | A centre line `from` to `to` on a `storey`, a `thickness`, a `kind` (`exterior`, `inner`), the rooms on its left (`a`) and right (`b`), -1 for outside, and `bulge` (0: straight). |
| `openings[]` | A span of a wall: its centre `at` along the wall from `from`, its clear `width`, its `sill` and `head` above the storey's floor, and a `kind`: `door` (a frame and a threshold; the leaf is not built yet and the doorway stands open), `window` (a frame, glazing bars, a sill board), `doorway` (a bare opening). |
| `rooms[]` | An `id`, a `storey`, a `kind`, a name and an outline on the walls' centre lines. |
| `stairs[]` | A flight from a storey's floor to the next: its `foot`, the quarter turn it climbs along, its `width`, `risers` and `going`, the `well` it leaves in the floor above, and the rooms at its foot and head. |
| `roofs[]` | A `form` (`gable`, `mono`; `hip` is refused until it is meshed) over a rectangle (the outer faces of the walls that carry it), the ridge's axis, the `eaves` height, the `pitch` as rise over run in permille, the `overhang` at the eaves and the `verge` at the gables, and the covering's `thickness`. |
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
both faces; roofs over a non-empty rectangle with a pitch of 50 to 4000 permille; a straight stair
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
stair (its foot, direction, width, rise, going, risers and the height it lands on) and a `WalkBox`
a solid member. Each carries the index of what it came from, so a failure names a description's
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
storey 2.1 m to the wall plate and open to a thatched gable roof at fifty degrees, a door and a
window in the front wall, a window in the back, a stack on one gable. Its numbers and where they
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
- The validator refuses by name.
- A recipe's canonical form reads back to itself.

**Public API.** `include/domain/structures/structures.h`.

**Depends on.** `base`, `containers`, `math`, `hash`, `json`, `schema`, `log`, `io`, `assets`,
`geometry`, `texture`, `materials`, `structures_schemas`.

**Testing.** `tools/dev.ps1 test -Filter structures`. Benchmarks: `tools/dev.ps1 bench -Filter
'structures.*'`.

## Budgets

To be measured with the first pictures: the example cot's triangles by surface, its clusters, its
DAG's root, and the mesher's time. The test holds the cot under 4,000 triangles.

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
- A door's leaf as a mesh of its own, and the `Door` record with its state (S91).
- Glass in a window.
- The kit of solved parts beyond boxes: hewn beams, warped planks, thatch bundles, shingles (G64).
- The record drawn from a document (D37's other half), and the conformance corpus.
