# routes (domain)

**Purpose.** Footpaths over a region's ground, as geometry and as wear (roadmap W96; the forest proof
of concept's solver needs 3.6: "grid-A* routes painted as ~22 m hard-edged corridors that read as
rectangles … A trail is geometry and wear"). A scene's `routes` placement entry names waypoints; one
plan joins them by footpaths that keep under a grade cap and merge where they meet; and each path is
written into the ground as a tread benched into it (a W98 ground edit), into the terrain's masks as
the tread's material, and into the placements' exclusion as band footprints, with a trampling
gradient any placement can ask. It does **not** place meshes yet (steps, exposed roots and stones in
the tread are later rows'), and it does not make paved lanes, twin tracks, braided paths on flats,
game trails or bridges.

**Why this shape.** A route crosses tiles, so it is **one plan** above them, made once and kept as a
derived file, exactly as the channels' network is (channels.md, "One plan"): what a tile realises is
read from it, never decided by the tile, so a tread that crosses a tile's border is continuous and the
world drawn from another start is the same world. The plan is a **least-cost search on a lattice**
because a footpath is the line people actually walk, and the published account of how fast people
walk on a slope is Tobler's hiking function; the grade cap and the tread's width are the Forest
Service's trail classes for hikers, and the bench is its notebook's full bench (sources and numbers:
[grounding](../experiments/routes.md)). The search carries a **heading**, because without a cost for
turning, every zigzag up a slope that keeps under the cap costs the same and the tie-break, not the
ground, would decide where the switchbacks stand. The tread is a **ground edit** (W98's `Bench`
line), not a mesh laid on the ground and not paint: the tile set writes it into every level that can
draw it, the collision and the placements stand on it, and the far levels close it by the cell rule
as they close a channel.

**Owned data.** The plan file (`<ddc>/routes/plan-<key>.route-plan`) and the `engine.routes` schema
types. The edits, the mask regions and the band footprints it writes are the scene's once pasted.

## The plan

1. **The lattice.** The layer's region at `spacing_m` (2 m), its heights sampled from the ground the
   entry is handed, to the whole millimetre, a row a job.
2. **The cost.** A step to one of 16 headings (the eight neighbours and the eight knight's moves)
   costs the time Tobler's function gives for its grade, the mean of both directions, in whole
   milliseconds. A step whose rise is more than `grade_cap` times its run is **steep**. A turn costs
   `turn_s` (45 to 90 degrees) or `switchback_s` (past 90); under about 27 degrees nothing.
3. **The legs.** Route by route in the layer's order, waypoint pair by pair (each waypoint moved to its
   nearest lattice point): Dijkstra over (cell, heading) with steep steps forbidden; where that finds
   nothing, again with steep steps allowed at `steep_penalty` times their time, and the leg is counted
   as steep. Every step a leg takes makes its two cells **trodden**, and a step between trodden cells
   costs `trodden` of its time for every later leg: so a later route joins an earlier one.
4. **The network.** The union of every leg's steps, cut into lines at every end, junction and
   waypoint: two routes to one place share their trunk once.
5. **The lines.** Each line's corners cut by `smooth_passes` of Chaikin's (a cut that would take the
   stretch between its two new points over the cap is not made), and **stationed** every `station_m`
   (0.5 m), x and z in whole centimetres.
6. **The tread's top** at a station is the lowest ground across the tread there (the centre and both
   edges): a **full bench**, the whole tread on cut ground. Along the line it is held under the cap by
   the lower envelope of cones of slope `grade_cap` over those heights, a pass each way in whole
   millimetres, on every stretch that is not steep; where the line crosses ground steeper than the
   lattice saw, the tread is cut down to keep its grade (`max_hold_m` says how far). Then it is sunk
   `sink_m` (4 cm).

**The grade cap holds on the tread exactly** wherever a path under it existed: no stretch between two
stations rises more than the floor of `grade_cap` times its run in millimetres. Where it did not, the
stretch is marked steep, its metres are counted (`over_cap_m`), and the tread follows the ground
there: stage 4's steps are what such a stretch wants.

### One plan

As the channels' (channels.md, "One plan"): the key is the layer's canonical JSON with `plan` cleared,
`k_routes_version` and the lattice's heights; `ensure_plan` samples the lattice, reads the file on a
hit and plans on a miss, writing it (best effort). **A scene that carries its treads names its plan**
(`RouteLayer.plan`, the key `engine-content routes` printed): the treads changed the ground the key
was made from, so the generator reads that file and never plans over its own edits; a named plan that
is missing, or was made from another layer, is refused with the command that makes it.

### Determinism

The heights are whole millimetres; the steep test is products and one correctly rounded square root;
a step's time is `det::exp` rounded once; the queue is ordered by (cost, state), a total order; the
network's walks go in ascending cell order; the stations are whole centimetres and the tread's top
whole millimetres. The jobs only sample the lattice, a row each. So the plan is the same bytes at any
thread count and from any host (`routes_tests.cpp` holds both), and its bytes and the treads' JSON are
pinned on MSVC for Clang and GCC to reproduce.

## The tread

`tread_edits_json` writes each line as an `engine.scene.TerrainEdit`: mode `Bench`, its points the
stations thinned to those a straight run misses by more than 5 mm in x, z or y, `width_m` the tread's,
`bank_slope` the layer's `backslope` (1, the notebook's 1:1), `fill_slope` (0.5, W98's) and
`bank_round` the layer's `edge_round` (0.15). The tile set then cuts the uphill side and fills the
downhill, the tread flat across its width to the millimetre (W98's bench), and the faces meet the
ground: the tread test holds the edited ground two metres past each edge to the ground as it was, and
every 5 cm across to no step steeper than the faces.

**Why `edge_round` is not W98's 0.75.** The rounding band is a share of the face's height at the
point. A fill face at 1 on 2 below a tread on a 30 % side slope runs nearly parallel to the ground, so
at 0.75 the band covers the whole face and lifts the ground 8 cm two metres off the tread (the tread
test measured it): a soft shoulder, the dune W98's pictures show. At 0.15 the edge is a lip, as a
worn path's is in the reels.

`tread_regions_json` writes each line's tread as a `TerrainMaskRegion` (its two edges as a polygon,
in whole centimetres) naming `tread_material`, for the terrain's `masks.regions`, with a
`tread_feather_m` edge. `tread_footprints` writes it as W98's bands, one a straight run of stations
(within 5 mm), half-width the tread's; held by `Exclusion::add_footprints`, every generator that asks
`excluded_mm` leaves the tread bare. The test holds them to the tread and nothing else: a point 8 mm
inside either edge is covered at every station, one 12 mm outside is not.

## Wear

`wear_factor(d, wear_m, tread_cover)` is the share of the cover trampling leaves `d` metres past the
tread's edge: `tread_cover` (0) on the tread, 1 at `wear_m` (2 m) and beyond, a smoothstep between,
never decreasing. `RouteWear` builds the treads' segments into a 16 m grid and answers
`edge_distance_mm` and `cover_mm` at a point, read-only and safe from any number of threads. The
direction and the reach are the literature's (cover falls toward a trail's edge, little effect past
about 2 m); the smoothstep between is a choice, said so on the grounding page.

**What a placement would read.** The sward (row G140's `sward_placement.cpp` tonight) multiplies a
patch's density class and its height by `cover_mm` at the patch's centre, as it multiplies the
riparian factor today; scatter and litter drop a candidate where `cover_mm` is under a draw. No host
hands a `RouteWear` to a generator yet: that is R142's contract (below).

## In a scene

The placement generator **`routes`** opens the plan (named, on a ground that carries its treads; or
keyed from the ground) and places nothing: `tile` is empty and `occupies` false. It exists so a scene's
routes entry is a scene's, refused at load when its plan is missing or stale, and so the plan is where
a later row's meshes (steps, roots, stones) and R142's edits and footprints at activation come from.

**`engine-content routes <scene.json> <out.json> [--ddc <dir>] [--entry <i>]`** reads the scene's
terrain through the registry with its `edits` cleared (the ground before any tread), runs
`ensure_plan` for each `routes` entry, and writes `{edits, plans, mask_regions}`: paste `edits` into
`terrain.tiles.edits` (beside a channels' cuts), `mask_regions` into `terrain.masks.regions`, and each
plan key into its entry's `params.plan`. Its JSON line counts the legs, the steep legs, the lines, the
metres, the metres over the cap, the switchbacks, the deepest hold, the edits, the regions, the bands
and the output's bytes.

## Budgets

| What | Number | Machine |
|---|---|---|
| Plan, 2 km² of lattice | see the measurement table in the roadmap row (W96) | `msvc-release`, state quoted there |

What scales with the world: the plan file grows with the metres of route (one 32-byte station every
`station_m`, one cell index every lattice step); the search's working set with the region (16 states a
lattice point, 12 bytes each, freed after planning). Nothing is resident at run time but the edits the
scene carries, whose cost is the tile set's (W98: about 1 ms a tile for 16 benched treads).

## Invariants

- No stretch of a tread between stations climbs or falls more than the cap, except a steep stretch,
  which is counted (`routes: a route keeps under the grade cap, and says where it cannot`).
- Two routes to one place share their trunk (`… two routes to one place merge`).
- The plan is the same bytes at any thread count and from any host, and pinned
  (`… the same bytes from any thread count and any host, and pinned`).
- A plan is made once; a named plan is read whatever the ground; a stale or missing one is refused
  (`… one plan, read again and named on its own treads`).
- The tread is flat across its width at its top and meets the ground two metres past its edges
  (`… benched flat across a side slope and meets the ground`).
- The band footprints cover the tread and nothing else (`… exclude the tread and nothing else`).
- The wear is monotone, the tread's own on it and 1 past its reach (`… monotone and reaches the
  tread`).

**Public API.** `include/domain/routes/routes.h`.

**Depends on.** `base`, `containers`, `math`, `hash`, `json`, `schema`, `schemas`, `log`, `io`,
`jobs`, `geometry` (the derived-data root), `scene_gen`, `tileset` (the tests build the treads as
the tile set does), `routes_schemas`.

**Testing.** `tools/dev.ps1 test -Filter routes`. Benchmarks: `tools/dev.ps1 bench -Filter 'routes.*'`
(the planner over a 64, 128 and 256 point square, and the wear query).

## Not yet

- **A cross slope.** A `Bench` line is flat across; the notebook's outslope is at least 5 % and class
  3's target 5-10 %. A line needs a cross-slope field (W98's report asks the same).
- **The half rule and the target grade** (the notebook; IMBA): the cost has no term for the side slope
  under a step, so a line may run steeper than half its side slope; and only the short-pitch maximum
  is held.
- **Switchback landings and steps** (stage 4): a turn past 90 degrees is counted but not built as a
  landing at 5 % or less; a steep stretch is not built as steps.
- **A host that hands the treads across at activation**: until R142, the edits and regions reach a
  scene by pasting, and the bands and the wear reach a generator only through a test.

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph,
the scheduler, or another capability, and it can be removed from the build the same way.

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | `engine_capability_requires(routes tileset)` | declared; off when tileset is off |
| Component and event types | `domain/routes/schemas/routes.schema` (`RouteLayer`, `Route`) | done |
| Tick scheduler entry | none | not needed: nothing ticks; the plan is decided when a scene is read |
| Render-graph passes | none | not needed: the tread is the terrain's, drawn by it |
| Content-build derived step | the plan file under `<ddc>/routes/` | done |
| Placement generator | `routes` (`GENERATOR placement:routes`) | done; places nothing yet |
| Protocol methods | none | not needed |
| Tunables | none | every number is the layer's |
| LOD policy | the tread is a ground edit: each tile-set level draws it while its spacing is finer than the tread's width and closes it past that (W98's cell rule); the mask region carries it on as colour | done |
| Determinism | above | done |
| Zero cost when unused | a scene with no `routes` entry never calls it (the registry) | done |
| Tests and size table | `tests/routes_tests.cpp`, `tests/size_table.cpp` (`Station`, 32 bytes) | done |
| Bench | `domain/routes/bench/routes_bench.cpp` | done |
| Removal proof | `ENGINE_WITH_ROUTES`, off in the minimal build | works |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_ROUTES=OFF`) drops the
module, its tests, and its bench; the module disappears from `build/<preset>/modules.json` and
is listed there under `disabled_capabilities`. Everything else must still build and pass.
