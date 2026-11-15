# village (domain, capability)

**Purpose.** A row-plan village laid out along lanes on the scene's ground (roadmap G69; plan
[14 §14.3](../plan/14-structures.md#the-village-filler)): the scene generator `village`. From an
entry that names one or more lanes, a house count and a seed it makes **one plan** — each lane
stationed on the ground, plots of whole perches along each side, a house on every plot the ground
allows, standing on a pad and facing the lane — and hands that plan over three ways: to the ground
as edits (the treads, the pads and the paths to the doors, benched into the tile set's heights), to
the entries after it as the shapes those occupy (so grass, shrubs and trees keep off them), and to
the host as one placement a house whose mesh is a building description the structures capability
meshes. Every number of the plan is grounded, or marked as a choice, in
[the village plan's grounding](../experiments/village-plan-grounding.md).

**What it deliberately does not do.** It makes no mesh and knows no building: a house is to the
plan a box on the ground with a door ([The house seam](#the-house-seam)). It lays out row plans
only. It writes no document records ([Not yet](#not-yet)).

## The plan

`plan_village(layer, ground, context, house filler)` is the whole of it, and what follows is why
each step is as it is.

- **A placement generator with a ground plan, like the routes.** Plan 14 describes the village
  filler as a writer of document records (`structure.fill_village`). The records (D37) did not
  exist when this was built, and the gate's target was a scene: so the filler is first a scene
  generator on the per-tile contract ([scene_gen](scene_gen.md#what-an-entry-plans-for-the-ground)),
  which is also what gives it the ground hook, the exclusion and the tile's meshes without a line
  in any host. The plan itself (`plan_village`) is a pure function of the entry, the seed and the
  unedited ground, with no generator state in it, so the record writer is a second caller of the
  same function and not a second filler.
- **The plan stands on the ground before any edit** (`Context::uncut()`), because the pads and
  the treads are edits of that ground: the open that hands the edits over and the open on the
  edited ground read the same heights and make the same plan. It is made again at every open and
  kept in no file: a plan is microseconds a plot ([Budgets](#budgets)), where the routes' search
  is seconds and earns its cache.
- **Plots in whole perches from the lane's first point, in integer millimetres.** A regular row
  plan is a surveyed thing: frontages in whole units of the perch, plots back to back along the
  street. Each plot's frontage is drawn between the entry's least and most perches by the engine's
  hash of (seed, lane, segment, side, place in the row), so a plot's width depends on where it is
  in its row and on nothing else: inserting a house count, or changing another lane, moves no plot.
- **A gap on the inside of a bend**, sized by the tangent of half the turn (from the two
  directions' cross and dot products: no angle is taken), so two segments' plots never overlap. A
  real row fills that corner with a wedge-shaped plot; this leaves it as a green. The outside of a
  bend runs to the corner.
- **Houses in order along the lane, the two rows alternating, until the entry's count stands.**
  A village smaller than its street leaves the far plots spare, as a planned village that never
  filled did, rather than scattering its houses along the whole length.
- **A plot is left empty, and counted, rather than forced**: where the ground under the house's
  footprint differs by more than `max_pad_step_m` (a pad that tall is an earthwork, plan 14's rule
  for a lot too steep for its plinth), where a corner or the middle stands in water, a channel's
  corridor or a hole (the scene's exclusion), where the house does not fit its frontage or its
  depth, or where the house filler has none. The summary says how many of each.
- **A pad is a `Bench` patch at the mean of the ground under the footprint** (its four corners and
  its middle), reaching `pad_margin_m` past the walls: cut on the uphill side, filled on the
  downhill, feathered into the ground by the tile set's faces
  ([tileset](tileset.md#ground-edits-that-raise-as-well-as-lower)). The mean, not the lowest or the
  highest corner, because it moves the least earth and leaves the door's sill nearest the ground
  outside it.
- **A lane's tread is a `Bench` line** along its stations, its top the ground at each station
  smoothed by a five-point mean so a hummock under one station puts no step in it; **a path** runs
  square to the street from each door to the tread's near edge, from the pad's level to the
  tread's. All three are also the shapes handed to later entries, and nothing else is: a plot's
  garden is ground like any other, for the scene's cover to grow on.

## The house seam

`HouseFn` is the only thing the plan knows of a house: asked with a plot (frontage, depth, the
setback), a seed and a storey count, it answers a `House` — the footprint's box and the door's
threshold in the house's own frame (whole millimetres, the origin on the pad), which way out of the
door is, the ground floor's height over the pad, and the mesh as a recipe (the bytes of a mesh
generator's file, [scene_gen](scene_gen.md#a-tiles-meshes)). The plan turns the house by quarter
turns until its door faces the street, then by the row's own direction, and refuses it if it does
not fit. `src/houses.cpp` is the one file that names the structures capability; `box_house` is the
stand-in the plan's own tests use, a box with a door in its long side that turns gable-on where
the frontage is too narrow for its length.

So **a hand-authored building in a scene file needs nothing of this module** (it is a mesh
placement whose recipe is its description), and **a game's own house filler** is another `HouseFn`.

## The way through a house

A house may say the way through it (`House::route`): feet on its floors, in its own frame, from
just inside the door through its rooms' doorways, up its stair and to a window, with the point
outside that a person at the window looks at. `walk_points` (`flight.h`) puts the lane, the path
and the door in front of that route and turns it into the world: one list of points that the
collision's case walks a capsule along ([scene_collision](scene_collision.md#structures)) and that
`engine-content village --flight` writes as a camera path at eye height, a key and a marker a
point. The route is the house filler's to give because only it knows the rooms; the plan adds
nothing to a house that gives none.

## In a scene

```json
"placements": [
  {"generator": "village", "params": {
     "name": "ridge foot", "seed": 7, "houses": 8,
     "lanes": [{"name": "street", "waypoints": [[96, 0, -60], [96, 0, 60]]}]}},
  {"generator": "scatter", "params": { "...": "the cover, after the village so it keeps off it" }}
]
```

The terrain must be a tile set (`provider: "tiles"`): a planned edit on any other ground is
refused by the reader, with the entry named. The treads' material is not handed across (a planned
mask region is not a list of `GroundPlan` yet): `engine-content village <scene> <out.json>` writes
each tread as a mask region to paste into `terrain.masks.regions`, with the plan's summary.

A streamed scene gets a house with the tile its origin stands on, its mesh named by the tile
(`tile_meshes`), and every tile is handed the shapes that reach within 2 m of it; a scene read
whole gets everything at once. Houses of one description share one mesh.

## Owned data

The entry type (`engine.village.VillageLayer`, `Lane`, `Rows`), and while an entry is open its plan:
the lanes' stations, the plots (224 bytes each) and each built plot's house with its recipe.
Nothing is kept on disk.

## Invariants

Each is held by a case of `tests/village_tests.cpp`.

- Frontages are whole perches within the entry's range; a row's plots follow each other with no
  gap and no overlap; no two plots of any rows overlap, round a bend included.
- A built house's door is its nearest point to the lane, the setback behind the plot's front; the
  engine's own rotation of the house's frame by the placement's quaternion lands the door where
  the plan put it.
- Under a built house's whole footprint the tile set's edited ground is the pad's level to a
  millimetre, and six metres behind the pad it is the ground as it was.
- The shapes cover the treads, the pads and the paths and not a garden; the tiles' shapes
  together are the whole read's.
- A plot too steep or in water is empty and counted, and the houses stand further along.
- The plan is the same bytes on every toolchain (`plan_hash`, pinned on MSVC, reproduced by GCC
  and Clang in the containers).
- An entry out of range is refused with a sentence naming the field.

## Public API

`include/domain/village/village.h`: `k_village_version`; the house seam (`HouseAsk`, `House`,
`HouseFn`, `box_house`); the plan (`LaneStation`, `LaneLine`, `PlotState`, `Plot`, `VillagePlan`,
`layer_in_range`, `plan_village`, `plan_hash`); what it is handed over as (`ground_edits`,
`footprints`, `tread_regions_json`, `plan_json`, `house_placement`, `house_point`).

## Depends on

`base`, `containers`, `math`, `hash`, `json`, `schema`, `schemas`, `log`, `io`, `scene_gen`,
`tileset`, `village_schemas`.

## LOD policy and determinism stance

**LOD.** A house is one placement whose mesh carries its own cluster DAG, so its level of detail
is the renderer's cut like any mesh's; the village adds no tier of its own and no far form (a
village impostor past the DAG's root is plan 14 §14.7's, not built). A tread, a pad and a path are
ground edits: each level of the tile set draws one while its spacing is finer than the edit's
width and closes it past that (W98's cell rule).

**Determinism.** Derived, and the same bytes everywhere: every place is whole millimetres; a
direction is products, sums, one square root and two divisions; a choice is the engine's hash; no
C library function decides anything. The plan does not depend on the thread count (it is serial)
or on the order tiles activate (a tile is handed a slice of the one plan).

## Budgets

The plan is made at each open of the entry: measured by `village.plan` (`tools/dev.ps1 bench
-Filter 'village.*'`), see [the measurements](#what-was-measured). A scene with no `village` entry
never calls the module (the registry), and a build with `ENGINE_WITH_VILLAGE=OFF` does not carry
it.

## What was measured

Filled in by the row's final runs: the plan's time, a house's triangles, pairs and memory, the
village's, and the frame inside a room against the frame outside.

## Testing

`tools/dev.ps1 test -Filter village`: the cases above, with no device. The walk through a house is
the collision's case ([scene_collision](scene_collision.md)); the scene is
`content/test-scenes/village/`.

## Not yet

- **No document records.** Plan 14's village filler writes `engine.structure.Village` and
  `Building` records the author then owns (`structure.fill_village`); this one is a scene
  generator, and its plan is the function that writer will call. Until then a village is
  regenerated from its entry at every load, which is what plan 14 says an authored village must
  not be.
- **Row plans only**: no agglomeration, no radial or grid plan, no green with a pond, no back
  lane, no church, well, fences, woodpiles or carts. A bend's inside corner is left open.
- **The tread's material is pasted, not planned** (`GroundPlan` has no mask list).
- **A lane has no camber, ruts or worn verge**, and the plots' gardens are whatever the scene's
  cover grows.
- **A house's pad is level**: a house on a slope gets no plinth or stilts, and the plan leaves the
  plot empty past `max_pad_step_m`.

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph,
the scheduler, or another capability, and it can be removed from the build the same way.

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | `engine_capability_requires(village tileset)` | declared; off when tileset is off |
| Schema types | `domain/village/schemas/village.schema` | the entry's types |
| Scene generator | `placement:village` (`src/generator.cpp`), with `plan_ground` and `tile_meshes` | registered |
| Tick scheduler entry | none | not needed: nothing ticks |
| Render-graph passes | none | not needed: houses are placements, the lanes and pads the ground's |
| Content-build derived step | none | the plan is made at each open |
| Protocol methods | none | not built ([Not yet](#not-yet)) |
| Tunables | none | every number is the entry's |
| LOD policy | above | the meshes' own DAGs and the tile set's cell rule |
| Determinism | above | derived; the same bytes on every toolchain |
| Zero cost when unused | the registry: a scene with no `village` entry never calls it | holds |
| Tests and size table | `tests/village_tests.cpp`, `tests/size_table.cpp` | in place |
| Bench | `domain/village/bench/village_bench.cpp` | `village.plan` |
| Removal proof | `ENGINE_WITH_VILLAGE`, off in the minimal build | works |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_VILLAGE=OFF`) drops the
module, its tests, and its bench; the module disappears from `build/<preset>/modules.json` and
is listed there under `disabled_capabilities`. Everything else must still build and pass.
