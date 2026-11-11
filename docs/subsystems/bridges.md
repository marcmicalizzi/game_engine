# bridges (domain)

**Purpose.** The crossing a footpath builds where it fords a stream (roadmap G147; the forest proof
of concept's solver needs §3.10, the footbridge half, and "Added by v40": four clapper bridges that
"read as a pile until one stands on it: slabs need to be slabs …, the approach needs a ramp, and the
trail must lead onto it"). At each ford a route's plan lists ([routes](routes.md), "Water") it
chooses the form a footpath of that class would have — **stepping stones**, a **clapper bridge** of
stone slabs on dry piers, a **foot log**, or **log stringers** under a plank deck — solves every
member in whole millimetres, and hands the bridge to the renderer as one mesh a tile. It writes the
approaches as ground edits that bring the tread up to the deck, and the bridge's plan as a footprint
the other placements keep out from under. It does **not** make road or cart bridges, arches or
suspension spans, a wall crossing a stream, or any response of the water to a pier.

**Why this shape.** A crossing is decided by the stream's section and the path, both of which are
plans above the tiles (the channels' and the routes'), so the bridge is **a function of its crossing
alone**: `engine-content bridges` reads the two plans and the ground once and writes each crossing
as integers — where the stream's centreline is, the path's direction square to it, the bankfull
width, the bed and the water, the bed's D84 and the ground along the path every 250 mm — and the
generator reads nothing else. So a bridge's identity is its route, line and station, its seed is
drawn from those, its members do not read the tile that realises it, and a scene read from another
start has the same bridges. The form is chosen by a **rule from the handbooks**
([grounding](../experiments/footbridges.md)), not by a weight or a draw. A bridge is **one mesh**,
not a kit of instances, because every instance is its own traced structure (the proof of concept's
wall stones cost 1,491 dropped structures as instances); and it is **a tile's own mesh**, handed as
a recipe with the tile ([scene_gen](scene_gen.md), "A tile's meshes"), because no two crossings
share one and nothing of a bridge should exist before someone is near it.

**Owned data.** The `engine.bridges` schema types (`Crossing`, `Layer`, `Bridge`) and the recipes
the generator hands. The edits and footprints the command writes are the scene's once pasted.

## The form

`choose_form` over the crossing (`bridges.h`; each constant against its source on the grounding
page):

1. **Stepping stones** where the bankfull depth is at most 500 mm and the width at most 4 m: a ford
   a hiker would otherwise wade (the trail notebook fords hikers where the water is 400 to 600 mm
   deep). The width limit is a choice: no handbook gives one.
2. **A clapper bridge** where the bed's D84 is at least 256 mm (boulders: the stone is to hand,
   which is the TCV's one condition for slabs) and the width at most 8 m.
3. **A foot log** where the span — the bankfull width and a 400 mm setback each side, so no support
   stands in the channel — is at most 5 m.
4. **Log stringers** under a plank deck to a 9 m span (the TCV's designs stop under nine metres).
5. Past that, **nothing**: the crossing stays a ford, counted by the command (`fords`), since a
   longer span wants an engineer and a form this row does not make.

The bankfull depth is the lower bank's top over the bed, read from the crossing's profile at the
bankfull width's edges.

## The members

Every member is a box in the bridge's frame (x along the path, z across, y up from the bed, whole
millimetres) with a shape, a material and the member it **rests on** (`Member::support`, or the
ground):

- **Stepping stones**, one every 600 mm along the path across the bankfull width (under the shorter
  adult step, 0.66 m), each 380 to 460 mm by 420 to 560 mm, its top 150 mm over the water at the
  scene's stage and its foot 150 mm into the bed; none where the ground is already dry footing.
- **A clapper**: an abutment on each bank and piers between, the supports evenly spaced so no slab
  is longer than 2.4 m centre to centre (Tarr Steps), each a stack of rough stone courses of about
  300 mm founded 200 mm under the ground; slabs 1.2 m wide, a tenth of their span thick (150 to
  250 mm), meeting over each pier with a 20 mm joint and bearing on the whole of each abutment. The
  slabs lie at the banks' level (the higher seat's, the lower bank's top, or 200 mm over the water,
  whichever is highest): a clapper keeps no freeboard, and floods go over it.
- **A foot log**: a 480 mm log hewn flat on top to about 250 mm (the notebook's 18 in and 10 in),
  overhanging each sill by 250 mm; the sills are 300 mm logs across the path.
- **Log stringers**: two logs, 250 mm through at a 5 m span to 400 mm at 10 m (the TCV), hewn 50 mm
  for the planks' bearing (the specification), under 150 × 50 mm planks with 20 mm gaps across a
  900 mm deck.
- **Seats**: a timber bridge's underside stands over the bankfull level by the bankfull depth, at
  least 300 mm (the trail bridge page's flood stage at twice the bankfull depth), and over the
  ground at both sills. A sill whose underside is in the ground lies in it, sunk at most a third of
  its diameter (the specification's mud sills); one that stands over the ground lies on a stone
  abutment of rough courses founded 200 mm in it.
- **A handrail** 1070 mm over the deck (the Forest Service and the NPS) where the deck stands more
  than 1 m over the bed — a choice: no source gives the fall height — on posts every 2 m or less on
  the planks, or on the two sills beside a foot log, one side.

The stones are **rough blocks**: the box's plan with each corner knocked off by its own draw (eight
sides), flat at the bed and the top, so a slab reads as split along its bedding. The draws are a
stream per member from the crossing's seed and its index.

## The mesh

`solve_bridge` makes every member a closed convex solid: a block a prism of its outline (28
triangles for a stone, 12 for a sawn member), a log a twelve-sided prism (44 triangles) whose
section is a constant table (no C library sine) cut flat at its hewn height, its sides with radial
normals. Each face's points are wound so its normal points away from the member's centre. UVs are
metres of surface on the plane the face is nearest square to, as the materials' `metres` layout
reads them. The stone members and the timber members are two parts, each clustered with the
engine's DAG builder, merged into one mesh with a material each: the layer's `stone_role`
(`rock/granite` by default) and `wood_role` (`wood/plank`). **The pack has no `wood/` set today**, so
the timber draws its constant colour (a weathered grey-brown), as the walls' stone did before G148;
a `wood/plank` and a `wood/log` set are the materials row's to add.

## Forms and the LOD policy

The mesh's cluster DAG is its LOD: its finest level is every member; its root, the far form, is the
builder's own simplification (`BridgeReport::root_triangles`). No authored coarser form: a bridge is
small, the DAG's root holds its silhouette, and it is a tile's mesh, so beyond the tiles a bridge is
not drawn at all — the far rings carry the stream and the tread as ground. Triangles by form are
under "Budgets".

## The ground and the neighbours

- **The approaches** (`approach_edits_json`): at each end of a bridge whose deck stands over the
  ground there, a `Fill` line from the deck's end out along the path, falling at the route's own
  grade cap (the approach is the path's tread), as wide as the tread or the deck, with the routes'
  0.5 fill batter and 0.15 rounding. Its top at the deck's end is the deck, so in the plan the walker
  steps onto the bridge with **no step**; `Approach::step_mm` reports where the ground at a deck's
  end stands over the deck (none in the tests). What the tile set's lattice leaves of that (a 25 cm
  lattice samples the fill's top between its points) is not measured here.
- **The footprint** (`bridge_footprint`): the rectangle of every member's plan, as W98's polygon in
  world millimetres, so the bed's stones, the sward and scatter keep out from under it once a host
  hands it to the exclusion.
- **The tread across the deck**: the route's own tread runs through the channel as its bench (the
  channel's cut wins there, routes.md "Water"), and the walker crosses on the deck's mesh, which the
  collision builds a proxy of as of any tile's mesh (R145).
- **Who hands them across**: nobody at activation yet. `engine-content bridges` writes the edits for
  the `tiles` block's `edits` and the footprints as polygons; a host would call
  `approach_edits_json(layer, grade)` before the tile set is built and `bridge_footprint` for each
  crossing into `Exclusion::add_footprints` (scene_gen.md, "A tile's meshes", the room the contract
  leaves).

## In a scene

**`engine-content bridges <scene.json> <out.json> [--ddc <dir>] [--grade <permille>]`** reads the
scene's terrain with its edits (the channels' cuts, the treads) but without its own approaches,
its `water_channels`, each `routes` entry's named plan (an entry that names none is refused: run
`engine-content routes` first) and, where a `channels` entry names its plan, the bed's D84 at the
nearest point of a link's line. For each crossing the routes list, the stream under its middle
station gives the centre, the direction square to it (the way the path runs), the bankfull width and
the water; the ground is sampled along the path every 250 mm to 6 m past each bank. It writes
`{placement, edits, footprints}`: paste `placement` into the scene's `placements`, `edits` into
`terrain.tiles.edits`. Its JSON line counts the crossings, each form, the fords left, the
triangles, the approaches, the grade and the largest step.

The placement generator **`bridges`** (`tile_meshes`, no resident kit) realises each bridge in the
tile its centre falls in, as one instance of one recipe mesh (`<ddc>/bridges/bridge-<hash>.bridge.json`,
never written: the recipe travels with the tile), its reach the footprint's; `expand` lists all of
them in the layer's order. The mesh generator **`bridge`** builds a recipe.

## Determinism

Every decision is whole millimetres: the profile's interpolation and every member are integer
sums, products and rounded divisions; a member's draws are `mix64` of the crossing's seed; the only
square roots are the direction's (one, correctly rounded) and the hewn chord's (an integer search).
The mesh's positions are millimetres divided by 1000 once; its normals the face's Newell sum and one
square root. A plan and a mesh are pinned on MSVC for Clang and GCC (v3 and v2) to reproduce
(`bridges: the same bytes on every toolchain, pinned`). Nothing is threaded.

## Budgets

See the roadmap row and the report for the measured numbers; the triangles by form:

| Form (the bench's crossing) | Triangles, finest | Far form (the DAG's root) |
|---|---|---|
| Stepping stones, a 2 m brook | measured below | |
| A clapper, 6 m, boulder bed | | |
| A foot log, 3 m | | |
| Log stringers, 7 m | | |

What scales with the world: the crossings, not the area — a crossing is about 1 KB of layer (its
profile is most of it), and a bridge's mesh exists only while its tile is held.

## Invariants

- The form rule at each form's smallest and largest crossing (`bridges: the form rule …`).
- Every member rests on the member it names or in the ground, and a spanning member has a support
  under each end; nothing below bankfull in the channel but a pier or a stone, no sill or abutment
  inside the bankfull width; a timber deck's freeboard; stones a stride apart (`… every member is
  seated …`).
- Every member a closed solid wound outward (`… a closed solid wound outward`).
- The tiles are the whole read, and a bridge does not read its tile (`… the generator's tiles …`).
- The approaches meet the deck with no step, and the footprint is the plan (`… the approaches …`).
- The same bytes everywhere (`… pinned`).

**Public API.** `include/domain/bridges/bridges.h`.

**Depends on.** `base`, `containers`, `math`, `hash`, `json`, `schema`, `log`, `io`, `assets`,
`geometry`, `texture`, `materials`, `scene_gen`, `bridges_schemas`. No other capability: a scene
whose routes are switched off has no crossings to read.

**Testing.** `tools/dev.ps1 test -Filter bridges`. Benchmarks: `tools/dev.ps1 bench -Filter 'bridges.*'`
(the plan, the solved mesh and the container, per form).

## Not yet

- **Hosts that hand the approaches and the footprint across** at a scene's read and a tile's
  activation (another row's; what it would call is above).
- **Wood sets in the pack**: `wood/plank` and `wood/log`.
- **The bank under the bridge**: rip-rap round an abutment, the bank trodden bare at the approaches.
- **A crib abutment of logs**, a pier's cutwater, a slab's taper, a foot log's taper and bark left
  at its ends; lattice rails (reel r49).
- **A bridge across a bank that is not level along the stream**: the ground is read along the path
  only, so a sill or an abutment is seated on the profile's height at its x.

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph,
the scheduler, or another capability, and it can be removed from the build the same way.

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | none | it reads its crossings from its own layer |
| Component and event types | `domain/bridges/schemas/bridges.schema` (`Crossing`, `Layer`, `Bridge`) | done |
| Tick scheduler entry | none | not needed: nothing ticks; a bridge is decided when a scene is read |
| Render-graph passes | none | not needed: a bridge is a mesh |
| Content-build derived step | the recipes, built by the mesh generator `bridge` through the cache | done |
| Placement generator | `bridges` (`GENERATOR placement:bridges mesh:bridge`) | done |
| Protocol methods | none | not needed |
| Tunables | none | every number is the layer's or a cited constant |
| LOD policy | the mesh's cluster DAG; a tile's mesh, so nothing past the tiles | done |
| Determinism | above | done |
| Zero cost when unused | a scene with no `bridges` entry never calls it (the registry) | done |
| Tests and size table | `tests/bridges_tests.cpp`, `tests/size_table.cpp` (`Member`, 40 bytes) | done |
| Bench | `domain/bridges/bench/bridges_bench.cpp` | done |
| Removal proof | `ENGINE_WITH_BRIDGES`, off in the minimal build | works |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_BRIDGES=OFF`) drops the
module, its tests, and its bench; the module disappears from `build/<preset>/modules.json` and
is listed there under `disabled_capabilities`. Everything else must still build and pass.
