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
threshold in the house's own frame (whole millimetres), which way out of the door is, how far the
frame's origin stands over the pad, and the mesh as a recipe (the bytes of a mesh generator's
file, [scene_gen](scene_gen.md#a-tiles-meshes)). The plan turns the house by quarter turns until
its door faces the street, then by the row's own direction, and refuses it if it does not fit.
`box_house` is the stand-in the plan's own tests use, a box with a door in its long side that turns
gable-on where the frontage is too narrow for its length.

**`src/houses.cpp` is the one file that names the capabilities a house comes from**: the rural
filler ([dwellings](dwellings.md), `fill_rural`) writes a building description, and
[structures](structures.md) validates it and gives its **walk records**
(`structures::walk_records`: floors, walls, openings, stair flights, solid members), which are all
this module reads of a house — never its mesh:

- **the household is drawn from the plot's seed**: three to eight persons, and the means that give
  the storeys the entry asks (the filler's two-storey types are its `means` 2). The rectangle the
  filler may fill is the frontage less half a metre each side by the depth behind the setback, so
  the filler sizes the house to the plot. Its own seed is the household's and the frontage's, not
  the plot's, so **two plots with one household and one frontage get one description and share
  one mesh** (the generator's case counts eight houses of five meshes);
- the box is the outer faces of the ground storey's exterior walls; the entrance is the first
  door between the outside and a ground-storey room, in a wall along an axis;
- a description's origin is its ground floor's top, and the frame stands **3 cm over the pad**, so
  the threshold (2 cm more) is a 5 cm lip. The capsule's round foot meets that lip's edge 34° from
  level and walks up it as a slope (the walker's limit is 40°). At 12 cm, a step the walker's
  0.35 m step-up should take, the first walk of this row stood at the door: a step-up lifts the
  capsule under the lintel. The foundation's depth keeps the walls' feet in the ground;
- **the walker's rules are held here** (plan 14 §14.4's, the least of S89's validators): the
  entrance and every doorway on the way through at least 0.66 m clear across (the capsule's 0.6 m
  and 3 cm each side, after the frame's 6 cm a side) and 1.85 m clear in height (after the
  frame's head and the threshold, 8 cm: a 1.90 m door is 1.82 m clear and the 1.8 m capsule with
  its 2 cm of padding does not enter it, which the first walk also found); the roof's underside
  at the eaves' edge at least 1.82 m over the floor where the entrance is under an eaves side (the
  collision's proxy keeps the roof, and a cot's 1.56 m eaves stopped the walker 0.72 m short of
  the wall); a flight's rise at most 0.25 m and its going at least 0.20 m. A house that fails one
  is refused with the sentence (the rule, the opening or stair, what was measured), its plot
  counted `refused`, and the plan keeps the first such sentence (`VillagePlan::first_refusal`);
- the recipe is the description's canonical JSON, named by its own source hash.

**The way through it is found, not assumed** (`route_through`): the rooms' doorways from the walk
records give the chain of rooms from the entrance to the first stair's foot and from its head to a
window of the upper storey; within a room the way is a breadth-first search over a 10 cm grid of
that storey's free floor — inside a floor's outline, 34 cm from every wall's face and solid member,
off a flight's steps and a well — pulled straight wherever a straight stretch is clear; a doorway
is crossed by a straight stretch through its middle, half a metre each side of the wall's faces.
The search's neighbour order is fixed, so the same house gives the same way.

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

## The wear

A lane drawn as one band of one material reads as a ribbon laid on a lawn, which is what the first
pictures of this scene showed. What makes a village street read as one is that the ground is worn
by degrees: the middle beaten to stone and dust, a margin of bare earth where feet and wheels
wander, a verge of short trodden grass out to the plots' fronts, and bare ground where people stand
at a door. `ground_regions_json(layer, plan, names)` writes that as mask regions
(`engine.scene.TerrainMaskRegion`), in the order they are laid, each over the last:

| Region | Outline | Layer (default name) | Feather inward |
|---|---|---|---|
| a lane's **verge** | the street's whole width, front to front | `verge` | a metre, or as far as the wear if that is nearer |
| a lane's **wear** | the tread and `k_wear_margin_mm` (45 cm) past each edge | `worn` | 90 cm, so the tread's surveyed edge is the middle of the fade |
| a lane's **tread** | the tread less 45 cm each side | `lane` | 45 cm |
| a built plot's **door yard** | a T: the path from inside the tread's wear to the plot's front, and from there to the wall the ground `yard_m` (1.6 m) each side of the door, kept 30 cm inside the plot's sides | `worn` | 40 cm |

The fades are wide on purpose: the mask level blends by the materials' heights
([tileset](tileset.md#the-mask-level), `height_blend`), so across a feather the stones of one layer
stand through the other and the edge is ragged at the scale of the texture, not a line at the
scale of the polygon. An empty layer name leaves its regions out, so a scene with two layers to
spare names two. The widths are choices, marked so on
[the grounding page](../experiments/village-plan-grounding.md#the-wear-and-the-boundaries); they
are not the entry's fields except `yard_m`, because nothing but the picture reads them.

The wear changes no height and hands no shape: the cover still keeps off the tread, the pads and
the paths alone, so a tuft may stand in a yard's corner or on the verge, as weeds do.

## The boundaries

A plot a person would call a toft has an edge. `plot_boundaries(layer, plan, out)` gives the lines
that edge stands on, whole millimetres: a built plot's front, its two sides and its back, with
**one gateway on the front where the door's path crosses it** (its middle as arc length from the
line's first point). A side two built neighbours share belongs to the earlier plot's line and is
left out of the later one's, so nothing stands twice on one line; a plot with both sides its own is
a closed loop, one with a side taken is an open run that starts or ends at the neighbour's corner,
and one with both taken is its front and its back.

**The village stands nothing on those lines itself.** What a boundary is made of — a hedge, a
bank, a fence, a wall — is another capability's, and the lines are general: a plan's geometry and
no building's name. `engine-content village` writes them twice in its output, as `boundaries` (the
lines as they are) and as `walls`, the same lines as a ready `walls` placement entry
([walls](walls.md): `points_mm`, `closed`, `gateways_cm`) 1.1 m high with 1.5 m gateways, which is
what the scene pastes. A wall, because it is the one boundary the engine can build today; the
grounding page says what the sources give for banks, ditches and walls and that a hedge is the
commoner case this cannot yet stand.

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
refused by the reader, with the entry named. The wear's materials and the boundaries are not handed
across (a planned mask region is not a list of `GroundPlan` yet, and a generator cannot stand
another's entry): `engine-content village <scene> <out.json>` writes, with the plan's summary,
`mask_regions` to paste into `terrain.masks.regions` (the palette needs the layers `verge`, `worn`
and `lane`) and `walls` to paste into `placements` after the village.

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
- A lane's wear is three bands, each inside the last; a yard holds the ground before its door, a
  metre along the wall from it and the path's end at the tread, and not the plot's back.
- Every side of every built plot is on exactly one boundary line, and a front's gateway is on the
  front, square to the street from the path's end.
- The plan is the same bytes on every toolchain (`plan_hash`, pinned on MSVC, reproduced by GCC
  and Clang in the containers).
- An entry out of range is refused with a sentence naming the field.

## Public API

`include/domain/village/village.h`: `k_village_version`; the house seam (`HouseAsk`, `House`,
`HouseFn`, `box_house`); the plan (`LaneStation`, `LaneLine`, `PlotState`, `Plot`, `VillagePlan`,
`layer_in_range`, `plan_village`, `plan_hash`); what it is handed over as (`ground_edits`,
`footprints`, `plan_json`, `house_placement`, `house_point`); the wear (`GroundLayers`,
`k_wear_margin_mm`, `ground_regions_json`) and the boundaries (`BoundaryLine`, `plot_boundaries`).
`include/domain/village/flight.h`: `WalkPoint`, `walk_points`, `flight_json`.

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

On `content/test-scenes/village/` (2026-11-14, `msvc-release`, an RTX 5090; the machine shared
with other agents' builds and GPU suites, other processes at 88 to 99 % of the CPU through the
flight, so every time is an upper bound).

**The plan**: 14 plots along 129 m of street, 8 houses built (a village), 6 spare, none steep, wet,
unfit or refused; 17 ground edits (a tread, 8 pads, 8 paths), 82 shapes, 11 mask regions (a lane's
three and eight yards) and 8 boundary lines. `village.plan` (2026-11-15, `msvc-release`, the
stand-in box house, other processes at 84 % of the CPU, so upper bounds): **13 µs** for a street
of 8 houses, 150 µs for 64, 1.13 ms for 256, the medians; 1.7 to 4.4 µs a house.

**The houses** (the rural filler's, through the structures mesher at `k_structure_version` 2): six
distinct meshes for eight houses, **2,210 to 2,872 triangles and 92 to 121 clusters each**, 14,584
triangles and 609 clusters together, against plan 14 §14.7's estimate of 30,000 triangles for an
exterior and 45,000 for an interior: the mesher is plain surfaces, with no kit of parts yet. **A
house's mesh is 181 to 244 KB as its container** (the file the renderer loads, its DAG and its
pages; 1.22 MB the six), about 83 bytes a triangle. The collision's proxies of two houses are
5,046 triangles and 126 KB (`structure_walk_tests.cpp`, a debug build).

**What the cluster LOD makes of a house** (`village_tests.cpp`, the last case: 2160 lines, a 60°
field of view, a one-pixel threshold, the camera before the front):

| House | Finest | 50 m | 200 m | 1 km | Root |
|---|---|---|---|---|---|
| the two largest (2,872 triangles) | 117 and 121 clusters | 2,835 in 83 | 1,847 to 2,168 in 42 to 54 | 1,425 to 1,460 in 33 | 376 to 427 |
| the four others (2,210 triangles) | 92 to 94 clusters | 2,210 in 65 | 1,454 to 1,510 in 34 to 36 | 945 to 1,071 in 24 to 25 | 382 to 397 |

At 50 m a house draws whole. **A kilometre off, where it is about 25 pixels wide, it still draws
43 to 51 % of its triangles, two and a half times its root**: the DAG's error for large flat
plaster and thatch surfaces stays under a pixel only that slowly, so a village seen from a ridge
pays about a thousand triangles a house until an authored far form stands in (plan 14 §14.7; the
structures page says the same of its cot).

**The scene whole** (with the walls and the wear, 2026-11-15): 2,789 instances, 8,146 clusters,
142,400 pairs, 51 meshes. Of those the eight houses are 609 clusters; the cover's six plants 1,348;
and **the walls' kit of 39 solved modules 6,177 clusters, 142,936 triangles and 13.8 MB of
containers** — eleven times the houses' memory for the boundary round them, resident whatever the
number of plots (the walls page's trade: a kit, not a mesh a tile). A scene that walls its plots
pays that once; a scene with no `walls` entry pays nothing.

**The frame, inside against outside** (`engine-view --camera-path camera-path.json --offscreen
--terrain-tiles --shadows rt --tunable renderer.gi.mode=bounce --time-of-day 15 --width 3840
--height 2160 --frames 600 --repeat 2 --benchmark`; native resolution, no reconstruction; GPU
milliseconds, the median over each stretch's frames of both repeats; the two repeats drew the same
pairs):

| Pass | Outside (the lane and the path, frames 0 to 232) | Inside (the rooms, the stair, the window, frames 250 to 599) |
|---|---|---|
| total | 6.36 | 5.18 |
| resolve | 4.35 | 2.85 |
| gi_trace (the bounce) | 0.90 | 1.01 |
| gi_fold | 0.12 | 0.11 |
| rt (records, CLAS, BLAS, TLAS) | 0.48 | 0.47 |
| hw raster | 0.29 | 0.27 |
| deform | 0.12 | 0.10 |

(The scene with its walls and its wear, 2026-11-15 at 7e615e23, the scene's own hour, other
processes at 100 % of the CPU and another row's test holding a GPU device: upper bounds. Before
the walls and the wear, at f11c6f3e, the totals were 6.02 and 5.03: the walls cost the street a
third of a millisecond, most of it the resolve's and the raster's.)

**A room costs less than the street, not more**: the resolve falls by a third indoors, because the
picture is plaster, flagstone and boards — each one tiled set — where outdoors most pixels are the
terrain's stochastic three-layer material and the sky; the bounce's sample pass costs a fifth
more indoors, where every sampled pixel's ray meets a wall within a few metres and is lit again.
The chain's cost is the scene's and does not change at the door (nothing is culled by being
indoors). The whole flight's GPU median is 5.38 ms (p95 6.70).

**A room's light, measured and not mended** (2026-11-15; house 0's cross-passage, whose door
faces west, at 15:00 with the sun 45° up and 22° off the door's normal; 1280×720; the mean of the
picture's luminance out of 255, at the exposure the sky metered). The renderer was not edited:
these are what its next rows start from.

| Setting | The lane | Looking back at the door from inside | The passage | An upper room |
|---|---|---|---|---|
| traced shadows, `gi.mode=bounce` | 99 | 38 | 38 | 31 |
| traced shadows, `gi.mode=sky` | 86 | 16 | 10 | 9 |
| traced shadows, `gi.mode=off` | 120 | 135 | 145 | 147 |
| cascaded shadow maps (no ray queries) | 120 | 135 | 145 | 147 |
| no shadows | 125 | 152 | 159 | 178 |
| bounce, exposure +3 stops | 209 | 95 | 96 | 79 |

- **Without a traced cache a room is brighter than the street** (the third and fourth rows, the
  same to a digit): the resolve's ambient is the sky's harmonics with nothing in the way, so every
  wall is lit as if the house had no roof and the ceiling is lit from below by the ground's share.
  That is every device without ray queries.
- **The sun does come in at the door**: its patch on the flags is drawn right under traced shadows
  at 15:00, the wall beside it lifts with the bounce, and it is absent at 12:00 (the sun is along
  the wall) and at 18:00 (the sun is 18° up and the house across the street shades the door).
  What barely comes in is the sky: with the cache fed by sky rays alone the walls beside an open
  door are 10 to 16 of 255, and with the bounce the passage is a level 28 to 38 from the door to
  its far end, where a room lit by a doorway falls off with every metre from it. These are the
  cache settled, not caught young: a still camera gives the same means to 0.2 of 255 after 8
  frames and after 600.
- **No setting darkens a corner or the meeting of wall and ceiling**: there is no short-range
  occlusion term in any of them.
- **The exposure is the sky's** (`ev100` 14.97 indoors and out, from the sky's light and not the
  picture's): a room is a tenth to a third of the street's brightness, and three stops more
  exposure gives a room a person would accept while the doorway burns out. Nothing adapts.
- **No light leaked through a wall's thickness** in the 14 flight pictures and three stills of
  each traced setting; nothing here is lit by a lamp or a fire, so a local light's leak was not
  exercised.

## Testing

`tools/dev.ps1 test -Filter village`: the cases above, with no device. The walk through a house is
the collision's case ([scene_collision](scene_collision.md)); the scene is
`content/test-scenes/village/`. **End to end** (`apps/engine_view/tests/village_view_tests.cpp`, on
a GPU): engine-content writes a level ground, plans a street of two houses and writes the flight;
engine-view draws every marker with the ids beside, and the case holds that the houses are meshes
with clusters, that every frame the flight calls inside is one building from within (seven tenths
of its pixels one instance's, over three clusters or more, a twentieth or less empty) and that the
load dropped, withheld, buried and floated nothing.

## Not yet

- **No document records.** Plan 14's village filler writes `engine.structure.Village` and
  `Building` records the author then owns (`structure.fill_village`); this one is a scene
  generator, and its plan is the function that writer will call. Until then a village is
  regenerated from its entry at every load, which is what plan 14 says an authored village must
  not be.
- **Row plans only**: no agglomeration, no radial or grid plan, no green with a pond, no back
  lane, no church, well, woodpiles or carts. A bend's inside corner is left open.
- **The wear and the boundaries are pasted, not planned** (`GroundPlan` has no mask list, and an
  entry cannot hand another generator's entry): a change to the plan means running the tool and
  pasting again, and the scene's README says so.
- **A boundary is a wall or nothing.** No hedge, bank and ditch, or fence can be stood on the
  lines yet (a hedge needs a generator that plants along a line; a fence a mesh generator of posts
  and rails); the lines do not choose which sides a plot bounds, and a wall on a pad's rim stands
  on the pad's bank where a house is within 10 cm of its plot's side.
- **A lane has no camber and no ruts**, the wear is the same along its whole length (no puddle,
  no wider wear at a door or a bend), and the plots' gardens are whatever the scene's cover grows.
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
