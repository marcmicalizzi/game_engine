# bridges (domain)

**Purpose.** The crossing a footpath builds where it fords a stream (roadmap G147; the forest proof
of concept's solver needs §3.10, the footbridge half, and "Added by v40": four clapper bridges that
"read as a pile until one stands on it: slabs need to be slabs …, the approach needs a ramp, and the
trail must lead onto it"). At each ford a route's plan lists ([routes](routes.md), "Water") it
chooses the form a footpath of that class would have — **stepping stones**, a **clapper bridge** of
stone slabs on dry piers, a **foot log**, or **log stringers** under a plank deck — solves every
member in whole millimetres, and hands the bridge to the renderer as one mesh a tile. It hands the
host the approaches as ground edits that bring the tread up to the deck, and the bridge's plan as a
footprint the other placements keep out from under (G186). It does **not** make road or cart bridges, arches or
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
the generator hands. The approaches' edits and the footprints are the entry's: it hands them to the
host itself (G186), and nothing of them is pasted into the scene.

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
  adult step, 0.66 m), each 300 to 380 mm along the path by 380 to 500 mm, its top 150 mm over the water at the
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
- **Seats**: a timber bridge's underside stands where [the deck's height](#the-decks-height) puts
  it, and over the ground at both sills. A sill whose underside is in the ground under the whole of
  it lies there, sunk at most a third of its diameter (the specification's mud sills); one that
  stands over any of the ground beneath it — a bank rising across the sill — lies on a stone
  abutment of rough courses founded 200 mm under the lowest ground beneath the abutment. (G186:
  the test was the highest ground about the sill, which on a rising bank left the low side of a
  sill seated at the bank 35 mm in the air once the deck stopped standing a metre up.)
- **Steps** at each end of a clapper or a timber deck that stands more than a riser over the ground
  there (G186; [the ground and the neighbours](#the-ground-and-the-neighbours)).
- **A handrail** 1070 mm over the deck (the Forest Service and the NPS) where the deck stands more
  than 1 m over the bed — a choice: no source gives the fall height — on posts every 2 m or less on
  the planks, both sides, on a stringer bridge only. A foot log has none: a post on its 250 mm flat
  would take the walking surface, and the first pictures' two posts on the sills with a rail five
  metres between them read as a goalpost, not a rail. A foot log's rail (a rope, or a pole on
  outriggers from the sills) is a later row's.

The stones are **rough blocks**: the box's plan with each corner knocked off by its own draw (eight
sides), flat at the bed and the top, so a slab reads as split along its bedding. The draws are a
stream per member from the crossing's seed and its index.

## The deck's height

**Which forms keep a flood clearance and which sit at the bank** (G186; the sources, the readings
and the table in [the grounding](../experiments/footbridges.md#the-decks-height-by-form-and-span)).
G147 held every timber form's underside over bankfull by a bankfull depth — the trail bridge page's
estimate of the 50 to 100 year flood stage, which is a way to find a flood and not a clearance — and
a foot log over a 0.6 to 0.8 m brook stood a metre over its banks on two humps of fill, where reel
r19's crossings sit almost at the bank. Now:

| Form | Its underside (or top) | `bridges.h` |
|---|---|---|
| Stepping stones | each top 150 mm over the water at the scene's stage | `k_stone_rise_mm` |
| Clapper | the slabs on the banks' level, the higher seat's, or 200 mm over the water; overtopped in a flood | (G147) |
| Foot log; stringers of a clear span up to 6.1 m | **300 mm over the lower bank's top** (Devon County Council's path footbridge: its soffit 300 mm above the bank tops, the flood clearance relaxed on a small watercourse so long as the soffit is above bank level) | `k_bank_clearance_mm`, `k_minor_span_mm` |
| Stringers past 6.1 m | over the flood stage: bankfull plus a bankfull depth, at least 300 mm | `k_min_freeboard_mm` |

Every form is also on or over the ground at its sills, which raises it where a bank is higher than
the bankfull level. **The line at 6.1 m is a choice**: it is the Forest Service's *minor trail
bridge* (a wood bridge of 20 ft clear span or less), an inspection class, the one size the sources
draw for a small timber bridge; no source draws the line between "at the bank" and "over the flood"
by span. What it buys, measured on the tests' crossings: a foot log over a 3 m brook 0.8 m deep on
level banks walks 0.75 m over its banks where it walked 1.25 m, and is reached by three stones an
end where it was reached by two fills each 8 m long at 15 %.

## The mesh

`solve_bridge` makes every member a closed convex solid: a block a prism of its outline (28
triangles for a stone, 12 for a sawn member), a log a twelve-sided prism (44 triangles) whose
section is a constant table (no C library sine) cut flat at its hewn height, its sides with radial
normals. Each face's points are wound so its normal points away from the member's centre. UVs are
metres of surface, as the materials' `metres` layout reads them: a stone's on the plane the face is
nearest square to; a timber member's **along its grain** (G186) — u runs along a log's axis or a
sawn member's longest extent on every face but its ends, v along the face's other axis — so a wood
set whose grain runs along its image's u follows the plank across the deck, the post up and the log
along the span.

**Three materials, each a role through the tile contract** (G186; R147's seam,
[texture](texture.md#textures-that-arrive-after-the-load)). The members are three parts — the
**stone** (stones, slabs, piers, abutments), the **sawn timber** (planks, posts, rails) and the
**round timber** (a foot log, stringers, sills) — each clustered with the engine's DAG builder and
merged into one mesh with a material each, whose set is the recipe's role from the host's pack:
`stone_role` (`rock/granite` by default; a rock the pack lacks falls back to `rock/granite`, the
engine's generated set, before a constant, since any rock set reads better than flat grey),
`wood_role` (`wood/plank`) and `log_role` (`wood/log`). The container names the sets' images and
fills their texture records (`texture::fill_cluster_texture_records`), so a tile's bridge draws
textured wherever the host reads the derived-data cache. A material no member wears (a line of
stepping stones' timber) names no images. **A timber role the pack lacks draws its constant**: a
weathered sawn grey-brown (0.30, 0.27, 0.22) and a darker barked log (0.20, 0.17, 0.13), roughness
0.80 and 0.85. The base's packs have no `wood/` set; `wood/plank` and `wood/log` are the materials
row's (G188), under exactly those names.

## Forms and the LOD policy

The mesh's cluster DAG is its LOD: its finest level is every member; its root, the far form, is the
builder's own simplification (`BridgeReport::root_triangles`). No authored coarser form: a bridge is
small, the DAG's root holds its silhouette, and it is a tile's mesh, so beyond the tiles a bridge is
not drawn at all — the far rings carry the stream and the tread as ground. Triangles by form are
under "Budgets".

## The ground and the neighbours

- **The approaches are steps, then a short fill** (G186; the grounding's
  [approaches](../experiments/footbridges.md#the-approaches-steps-then-a-short-fill)). At each
  end of a clapper or a timber deck that stands more than a riser (`k_riser_mm`, 200 mm) over the
  ground at its end, rough stone steps (`MemberKind::step`, the stone's material) are laid on the
  ground out along the path: the risers equal and at most 200 mm, each step 300 mm along the path
  (`k_step_going_mm`), as wide as the tread or the deck to 1.2 m, bedded 100 mm under the lowest
  ground beneath it, at most four an end (`k_max_steps`), and none past where the ground already
  stands at its height. `BridgePlan::steps`, `approach_x` and `approach_top` say what each end
  laid and where a fill would have to meet the last step. **The fill** (`approach_edits_json`) is
  left only where the ground there is more than half a riser under that: a `Fill` line from the
  last step's outer edge out along the path, falling at the route's own grade cap (the approach is
  the path's tread), its top a riser under the last step, the tread or the deck wide, whichever is
  wider, with a metre of shoulder (`k_approach_shoulders_mm`, for the ground's lattice: a crest a
  tread wide is two or three points of a 25 to 50 cm heightfield) and the routes' 0.5 batter and
  0.15 rounding. So a small bridge on level banks has steps and no earthwork, and a stringer bridge
  past the minor span four steps an end and a fill a metre lower than G147's, starting 1.2 m
  further out. `Approach::step_mm` reports where the ground at a deck's end stands over the deck
  (none in the tests) and `Approach::steps` the steps; the command's line counts them
  (`approach_steps`). The source for the steps' numbers is none: they are an outdoor step's.
- **The notch where the channel's cut meets the approach** (G147's third objection: the cut won
  over the first half metre of the fill, which was listed before it). The steps stand on the
  crossing's profile, which is the ground **with the channel's cut in it**, so nothing the cut
  lowers is under them, and the fill, where there is one, starts 1.2 m past the deck's end, outside
  a cut's bank batter at the picture scene's slopes. So the notch is closed in this module for
  every form. **What would close it for a fill that must start at the deck's end** (none does now):
  `domain/tileset` letting a `Fill` listed after a `Cut` win on the bank outside the cut's bankfull
  width, or a mode of edit the cut leaves alone — a request, not this row's.
- **The footprint** (`bridge_footprint`): the rectangle of every member's plan, as W98's polygon in
  world millimetres, so the bed's stones, the sward and scatter keep out from under it.
- **The tread across the deck**: the route's own tread runs through the channel as its bench (the
  channel's cut wins there, routes.md "Water"), and the walker crosses on the deck's mesh, which the
  collision builds a proxy of as of any tile's mesh (R145).
- **Who hands them across: the entry, through the host's two seams** (G186, on R144's hook;
  [scene_gen](scene_gen.md), "What an entry plans for the ground" and "The shapes a tile
  occupies"). `plan_ground` hands each approach as an `engine.scene.TerrainEdit` read back from the
  JSON `approach_edits_json(layer, layer.approach_grade_permille)` writes, so an approach planned
  at the read and the same approach pasted into `terrain.tiles.edits` are one edit and build one
  tile set (`bridges ground plan: …`, below). It reads nothing but the layer: a crossing carries
  the ground it was sampled on, so the plan is a function of the entry and the order of the entries
  moves none of it. `expand` hands every bridge's footprint in `Placements::shapes`, and `tile`
  every footprint whose disc, grown by `k_shape_reach_mm` (2 m, the routes' wear: a choice), touches
  the tile, so a point of a streamed tile is under a deck exactly when it is on the whole read; both
  hosts add the shapes to the exclusion the later entries ask, so the sward, litter and scatter keep
  out from under the deck on either path. `occupies` answers for a tile with a bridge or a shape.
  A placement's `reach` (the disc later entries read before the mesh is built) is the furthest of
  the footprint's corners from the crossing's centre, the mesh's origin.

## In a scene

**`engine-content bridges <scene.json> <out.json> [--ddc <dir>] [--grade <permille>]`** reads the
scene's terrain with its edits (the channels' cuts, the treads) but without any bridge's approaches
— the pasted ones it finds by name are left out, and a `bridges` entry's planned ones are never
asked — and with what every other entry hands through its hook (`plan_ground`: a `routes` entry's
treads), each opened on the ground before any edit as the scene's reader opens it; its
`water_channels`; each `routes` entry's named plan (an entry that names none is refused: run
`engine-content routes` first); and, where a `channels` entry names its plan, the bed's D84 at the
nearest point of a link's line. For each crossing the routes list, the stream under its middle
station gives the centre, the direction square to it (the way the path runs), the bankfull width and
the water; the ground is sampled along the path every 250 mm to 6 m past each bank. It writes
`{placement, edits, footprints}`: **paste `placement` into the scene's `placements`, and nothing
else** — `edits` and `footprints` are what the entry hands the host itself, written so a person can
read them. The layer carries the routes' grade cap as `approach_grade_permille` (`--grade`
overrides it). Its JSON line counts the crossings, each form, the fords left, the triangles, the
approaches, the grade and the largest step.

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

The three bridges of the picture scene (`msvc-debug`, the containers the renderer built, 2026-11-11):

| Form | Members | Triangles, finest | Coarser DAG levels, all of them | Container |
|---|---|---|---|---|
| Stepping stones, a 2 m brook | 4 stones | 112 | 48 (one level) | 10.3 KB |
| A foot log, 3 m (sills on stone abutments) | log, 2 sills, 2 courses | 244 | 84 (one level) | 18.8 KB |
| Log stringers, 6 m, railed | 2 stringers, 2 sills and their abutments, the planks, posts and two rails | 944 | 417 (three levels) | 80 KB |

A clapper of 6 m (the bench's crossing) is three spans on two piers and two abutments of three to
four courses each and three slabs: about 20 rough blocks of 28 triangles, some 560. Every bridge is
under a thousand triangles at its finest and a few hundred from the far end of its tile; a mesh is
resident only while its tile is held.

`msvc-release`, `engine_bridges_bench --wait-quiet=120 --repeats=5`, 2026-11-11 14:12 local, on a
busy machine (the bench proceeded after 120 s: other processes at 68 to 97 % of the CPU, the GPU
lock held by another agent): every number is an upper bound. The bench's crossings: stones over a
2 m brook, a clapper of 6 m on a boulder bed, a foot log over 3 m, stringers over 7 m.

| What | Stones | Clapper | Foot log | Stringers |
|---|---|---|---|---|
| `plan_bridge` (what `open` pays a crossing) | 0.8 µs | 3.0 µs | 1.7 µs | 6.0 µs |
| `solve_bridge` (the plan and every solid) | 13 µs | 33 µs | 44 µs | 112 µs |
| `build_container` (a derived-data miss, once a bridge) | 0.28 ms | 1.4 ms | 0.70 ms | 4.2 ms |

A tile's activation costs nothing of the solver: `tile` copies a placement and a recipe the entry
made at `open`, and the mesh is the cache's (built once, off the frame's thread). What scales with
the world is the crossings, not the area: a crossing's layer entry is 1 to 2 KB of JSON (the picture
scene's three: 6.9 KB written; its profile is most of it), `open` plans each once and keeps its
recipe (the same size again), and a bridge's mesh exists only while its tile is held.

## Invariants

- The form rule at each form's smallest and largest crossing (`bridges: the form rule …`).
- Every member rests on the member it names or in the ground, and a spanning member has a support
  under each end; nothing below bankfull in the channel but a pier or a stone, no sill or abutment
  inside the bankfull width; a timber deck's underside where the deck's height puts it, and higher
  only where a sill's ground asks; stones a stride apart (`… every member is seated …`).
- Every member a closed solid wound outward (`… a closed solid wound outward`).
- The tiles are the whole read, and a bridge does not read its tile (`… the generator's tiles …`).
- A small bridge sits at its bank: a foot log over a 3 m brook and stringers over 5.4 m have their
  underside exactly 300 mm over the bank, the log walks at most 780 mm over it, both ends have
  steps of equal risers at most 200 mm and 300 mm long on the ground, and no fill
  (`… a small bridge sits at its bank …`; G147's rule fails it by 500 mm).
- Past the minor span, four steps an end and a fill from the last one's foot at the grade; the
  fill's top there is a riser under the last step; the footprint is the plan, steps included
  (`… the approaches …`).
- A tile hands the plan of every bridge that reaches it, and a point of the tile is under a deck by
  the tile's shapes exactly when it is by the whole read's, at 250 mm over nine tiles round a
  bridge on four tiles' corner; the hook hands `Fill` edits only (`… a tile hands the plan …`).
- The approaches the hook plans are the command's pasted ones read back from text, field for
  field, and the two tile sets are the same ground to the bit, and raised where the approaches are
  (`bridges ground plan: …`, `tests/ground_plan_tests.cpp`, compiled where the tile set is).
- The same bytes everywhere (`… pinned`).

**Public API.** `include/domain/bridges/bridges.h`.

**Depends on.** `base`, `containers`, `math`, `hash`, `json`, `schema`, `log`, `io`, `assets`,
`geometry`, `texture`, `materials`, `scene_gen`, `bridges_schemas`. No other capability: a scene
whose routes are switched off has no crossings to read.

**Testing.** `tools/dev.ps1 test -Filter bridges`. Benchmarks: `tools/dev.ps1 bench -Filter 'bridges.*'`
(the plan, the solved mesh and the container, per form).

## Not yet

- **The collision's bodies under a streamed deck**: the collision generates a streamed entry's
  placements with the scene's exclusion and no tile's shapes (scene_gen.md, "The shapes a tile
  occupies"), so a body of a later streamed entry can stand where the drawn tile left the ground
  bare under a deck. It is R144's, for every shape.
- **Wood sets in the pack**: `wood/plank` and `wood/log` (G188); the bridge names them already.
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
