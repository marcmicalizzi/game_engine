# nav (domain)

**Purpose.** The navigation mesh: tiles aligned to the world grid, built from triangle soup by
Recast's voxel pipeline; paths, nearest-point and walkability queries through Detour; off-mesh
links added and removed at run time so a destroyed wall becomes a route; an asynchronous rebuild
queue on `core/jobs`; a coarse region-connectivity graph for the LOD2/LOD3 tiers; and a minimal
crowd ([05 §5.11](../plan/05-simulation.md#511-integration-notes),
[§5.8](../plan/05-simulation.md#58-destruction),
[ADR-0027](../adr/0027-additive-capabilities.md)).
[Recast/Detour](https://github.com/recastnavigation/recastnavigation) 1.6.0 (zlib) is the backend
and does not appear in the public surface. An optional capability: `ENGINE_WITH_NAV=OFF`, or a
`*-minimal` preset, leaves the module, its tests, its bench, and the Recast download out of the
configuration entirely.

**Owned data.** A `NavTileData` owns one tile's bytes. A `NavMesh` owns the Detour tile mesh, one
query object, the stored polygon mesh of every resident tile, and the off-mesh link table. A
`RebuildQueue` owns the pending geometry copies and the finished tiles nobody has applied yet. A
`RegionGraph` owns the coarse nodes and edges. A `Crowd` owns its agents. Nothing else may hold a
backend object; nothing outside `src/` sees one.

**Invariants.**
- No public header names an `rc*` or `dt*` type, includes a Recast header, or is affected by
  Recast's build flags. The backend lives behind `Impl` in `src/`.
- A tile's bytes are a pure function of `(NavBuildParams, TileCoord, geometry)`. The same input
  always produces byte-identical output.
- Tiles are aligned to the world grid. `add_tile` refuses a tile built on a different
  origin or tile size, and `tile_containing()` is the only rule that maps a point to a tile.
- `NavBuildParams::tile_size` is an exact multiple of `cell_size`; `validate()` refuses anything
  else (see *The seam that is invisible* below).
- A handle is `SlotMap`-shaped: a stale or null `OffMeshLinkId` or `AgentId` fails every lookup
  rather than aliasing a live object.
- `RebuildQueue` builds on efficiency workers and mutates a `NavMesh` only inside `apply_ready`,
  on the calling thread, in tile order.
- A tile requested twice before it builds is built once, with the second request's geometry.
- A region-graph edge exists only where a tile-border portal or an off-mesh link does, so the
  coarse tier never claims a route the detailed mesh does not have.

**Public API.** `domain/nav/types.h`: `Status`/`status_name`, `TileCoord`/`tile_key`/
`tile_from_key`/`tile_containing`, `Area`/`area_name`/`default_flags_for`, the `k_flag_*`
constants, `PathFilter`, `OffMeshLinkId`, `AgentId`, `NavPoint`, `PathResult`, `RaycastHit`,
`path_length`. `domain/nav/tile.h`: `NavBuildParams`, `validate`, `TileRegionInfo`, `TilePortal`,
`NavTileData`, `TileBuildStats`, `TileGeometry`, `build_tile`, `tile_bounds`.
`domain/nav/nav_mesh.h`: `OffMeshLink`, `NavMeshOptions`, `NavMesh`, and `CrowdOptions`,
`AgentDesc`, `AgentState`, `Crowd`. `domain/nav/region_graph.h`: `RegionNode`,
`RegionGraphStats`, `k_invalid_region`, `RegionGraph`. `domain/nav/rebuild_queue.h`:
`RebuildQueueOptions`, `RebuildQueueStats`, `RebuildRequest`, `RebuildQueue`.

**Depends on.** `base`, `containers`, `math`, `jobs`, `log`, `time`.

---

## What is wrapped, and what the bytes of a tile are

`build_tile` runs Recast's pipeline — rasterize, filter, compact, erode, region, contour,
polygonize, detail — and returns a `NavTileData`: one flat byte buffer holding the **polygon
mesh**, plus a per-region summary and the tile's border portals.

It is the polygon mesh and not Detour's tile format on purpose, and that is the one structural
decision worth understanding before reading the code. An off-mesh link has to be baked *into* a
tile's Detour data (`dtNavMeshCreateParams::offMeshCon*`), and links are added and removed at run
time because [§5.8](../plan/05-simulation.md#58-destruction) says a collapse opens passages. So
something has to be able to produce Detour data for a tile more than once. Keeping the polygon
mesh as the stored form gives each tile exactly one source of truth:

- `NavMesh::add_tile` bakes Detour data from it (`dtCreateNavMeshData`, which also builds the
  bounding-volume tree).
- Adding or removing a link re-bakes the owning tile from the same bytes. That costs a BV tree
  over a few hundred polygons and **no voxelization** — about a hundredth of a rebuild.
- The bytes a determinism test compares are the bytes the builder produced, rather than a
  downstream encoding that also contains a tree built from them.

The cost is one decode per bake (a memcpy per section) and one extra copy of the polygon mesh
resident per tile, which is smaller than the Detour tile it produces.

The format is `src/tile_format.h`: a `TileHeader` — magic, version, tile coordinate, **the grid
the tile was built on**, bounds, cell sizes, agent dimensions, section counts — followed by
vertices, polygons, flags, areas, a region id per polygon, the detail mesh, the region table and
the portal table, each padded to four bytes. It carries no endianness or forward-compatibility
promise, so a buffer is a **cache entry, never an asset**; `from_bytes` rejects a version it does
not know rather than misreading it.

## The tile alignment rule, and the seam that is invisible

Tiles are aligned to the world grid ([05 §5.11](../plan/05-simulation.md#511-integration-notes)):
`NavBuildParams::origin` is the minimum corner of tile (0, 0), `tile_size` is the pitch in x and
z, and the tile's polygon mesh spans exactly that square. The grid travels **in the tile's own
bytes**, so a `NavMesh` and a `RegionGraph` fed the same tiles cannot be told different grids, and
`NavMesh::add_tile` refuses a tile whose grid does not match its own.

**`tile_size` must be an exact multiple of `cell_size`.** Recast's voxel grid for a tile is
`round(tile_size / cell_size)` cells wide, and `rcBuildPolyMesh` marks a polygon edge as a border
portal when its vertices sit at voxel 0 or voxel N of that grid. If the cell size does not divide
the tile size, the grid covers less ground than the grid says — 64 m at 0.3 m cells reaches
63.9 m — and the neighbouring tile's portal edges are 10 cm away. Detour links two tiles by
matching portal coordinates to within **one centimetre** (`overlapSlabs`, `dtAbs(apos-bpos) >
0.01f`), so it links nothing.

The failure has no symptom. Every tile builds, every tile is added, every query inside a tile
works, and no path ever crosses a tile border. `validate()` therefore refuses the pair, which is
the only place this is cheap to notice.

There is a second, related rule with the same shape: **hand `build_tile` geometry that reaches
past the tile's own square**, at least as far as `tile_bounds(params, coord, /*expanded=*/true)`.
The agent radius is eroded off the walkable area, so a floor that stops exactly at the tile border
leaves no polygon touching it, the tile gets no border portals, and it links to none of its
neighbours. Also silent. The tile tests assert both halves of this: a tile built from a flush
floor has regions and **no portals**, and the same floor with a margin has both.

## Areas, flags, and the filter

An **area** is what kind of ground a polygon is (`Ground`, `Water`, `Door`, `Jump`, `Hazard`);
a **flag** is whether a given agent may use it. Recast carries one byte of area per polygon all
the way through the pipeline, so a caller's per-triangle area assignment survives into the mesh;
`rcClearUnwalkableTriangles` is used rather than `rcMarkWalkableTriangles` precisely so that a
too-steep triangle *loses* its area instead of every triangle *gaining* `RC_WALKABLE_AREA`.
`default_flags_for()` is the single place that turns an area into flags.

`PathFilter::area_cost` is clamped to at least 1 when it is converted for the backend. A cost
below 1 makes Detour's Euclidean heuristic inadmissible, and the resulting paths are not merely
suboptimal, they are visibly wrong; clamping is cheaper than explaining that later.

The default filter admits every flag except `k_flag_disabled`, which includes `k_flag_jump` — so
by default an agent *will* use an off-mesh link. A test or a game that wants a walker who does not
jump asks for `include_flags = k_flag_walk`.

## The rebuild queue's threading contract

Three sentences, and they are the whole contract:

1. `request()` copies the geometry and returns. Any thread.
2. Builds run on the job system's **efficiency pool**, one tile per job, touching nothing but
   their own copy of the input.
3. `apply_ready()` installs finished tiles, **in tile order**, on the calling thread, and is the
   only thing that touches a `NavMesh`.

Tile order and not completion order is the determinism claim
([§5.15](../plan/05-simulation.md#515-capability-inventory): "hashed; rebuilds are ordered by
tile, never by completion time"). Whatever the workers did, the mesh ends a tick in the state it
would have ended in if every build had run on one thread.

The **efficiency** pool rather than the performance pool, deliberately: a rebuild is
latency-tolerant — nothing blocks on it, and the old tile stays walkable until the new one lands —
and a frame is not. On a machine with no efficiency cores, `core/jobs` runs that pool at
below-normal priority on performance CPUs ([jobs](jobs.md)), which is the same bargain.

**A request is queued, not started.** `dispatch()` starts what the priorities say should run, and
`apply_ready()` and `wait_idle()` call it so that a caller who wants one call per tick has one.
The separation is not bookkeeping: if a request started a build on the spot, the first request of
a frame would always take the free slot, and a queue that never went empty would be served in
arrival order whatever the observers were doing. With the split, a tick is `set_observers(...)`,
a burst of `request(...)`, and one `apply_ready(mesh, &graph)`.

**Coalescing.** The pending set is keyed by tile, so a tile requested twice before it builds is
built once, with the second request's geometry — building the first would produce a result already
known to be wrong. A tile requested again while it is *already building* is queued afresh, because
the in-flight build is reading the older geometry and there is no way to tell it otherwise.

**Priority and the starvation guard.** A pending tile's score is its horizontal distance to the
nearest observer minus `starvation_relief` metres per dispatch round it was passed over. Without
the second term a queue kept busy near an observer would never build a far tile at all, and a
bridge destroyed on the other side of the map would stay unwalkable forever. Past `max_pending`
the queue evicts the least urgent pending tile and returns `LimitReached`; that tile keeps its old
mesh, which is stale and walkable, and that beats an unbounded queue during a collapse.

**Shutdown waits.** There is no way to cancel a Recast build mid-pipeline, so `shutdown()` clears
the pending set and then blocks until the in-flight builds finish. A queue that freed itself under
a running build would be a use-after-free, not a fast shutdown.

## The region graph, and why a coarse tier exists

The region graph is one node per **connected component of one tile's walkable polygons** and one
edge per border portal between two of them, plus one edge per off-mesh link.

[05 §5.6](../plan/05-simulation.md#56-what-npc-scale-is-realistic) puts 10^5 scheduled NPCs in a
world and [§5.4](../plan/05-simulation.md#54-lod-tier-assignment) gives detailed simulation only
to the LOD0/LOD1 ones. A Detour `findPath` is a best-first search over polygons with a node pool,
a heap and a string-pulling pass; at a few microseconds each, 10^5 per tick is not a budget any
machine has. Most of those queries exist only to answer *roughly how far, and is it even
reachable* for a traveller who will be summarized rather than simulated. The region graph answers
that over a graph three to four orders of magnitude smaller — a handful of nodes per tile instead
of a few hundred polygons — and answers it with the same connectivity, because it is derived from
the same tile bytes.

**How it relates to the detailed mesh.** They are two views of one thing, not two systems:

| | detailed | coarse |
|---|---|---|
| unit | polygon | connected component of a tile |
| built from | the tile's polygon mesh | the same bytes, via `set_tile` |
| answers | a corridor of points | a distance, and reachability |
| used by | LOD0/LOD1 movers, the crowd | LOD2/LOD3 travellers, "can this NPC get there at all" |
| kept in sync | — | by construction: `apply_ready` feeds both from the same bytes |

**Reachability agrees exactly.** A region edge exists only where Recast wrote a border portal into
the polygon mesh, or where a link was added, so the coarse tier cannot claim a route the detailed
mesh lacks. Two stretches of a shared border join only when they overlap along the border *and*
their heights meet within the agent's climb, which is what stops the two landings of a stairwell
that share a tile border from being welded into one region.

**Distance is an estimate.** `estimate_distance` runs A* over the regions accumulating
portal-to-portal distance, so it can neither cut through a wall (it is not much shorter than the
truth) nor detour via a centroid (it is not much longer). Measured against `NavMesh::find_path` on
the test's 128 m two-tile-square scene with a divider, the ratio is **1.00 to 1.09**; the test
asserts a wider band (0.9 to 1.3) because the ratio is a property of the scene's region shapes and
a long thin region would push it out without anything being wrong. A traveller promoted to LOD1
gets a real corridor and the estimate is discarded.

Its resident cost is `TileRegionInfo` (44 B) and `TilePortal` (24 B) per region and per portal,
plus a `RegionNode` (52 B) and its edges. At four regions and ten portals a tile, a 10,000-tile
world spends about 2.6 MB.

## Recast's build knobs, and what the defaults do

Every option in [`cmake/EngineNav.cmake`](../../cmake/EngineNav.cmake) is there because the
default breaks something. Recast's CMake is written to be a standalone project with a demo:

| Option | Default | Set to | What the default does |
|---|---|---|---|
| `RECASTNAVIGATION_DEMO` | ON | OFF | Builds an SDL2 + OpenGL demo application, making the capability unbuildable on any machine without them. |
| `RECASTNAVIGATION_TESTS` | ON | OFF | Calls `enable_testing()` and `add_test(Tests Tests)` in a subdirectory of *our* build, so `ctest` on the engine runs Recast's suite; it also compiles a vendored Catch2 amalgamation beside our doctest. |
| `RECASTNAVIGATION_EXAMPLES` | ON | OFF | Unused in 1.6.0, but a default nobody chose. |
| `RECASTNAVIGATION_DT_POLYREF64` | OFF | OFF (kept) | Would widen `dtPolyRef` to 64 bits. A 32-bit ref is salt \| tile \| polygon and Detour keeps 10 bits of salt, which the defaults (1024 tiles, 4096 polygons a tile) fit in 10 + 12. Paths and the A* node pool are half the size this way; this is the switch to flip when a world needs more resident tiles than 22 bits allow. |
| `RECASTNAVIGATION_DT_VIRTUAL_QUERYFILTER` | OFF | OFF (kept) | Would make `dtQueryFilter::passFilter`/`getCost` virtual — an indirect call per visited polygon inside A* ([11 §11.4](../plan/11-performance-principles.md#114-branch-free-hot-paths-and-constexpr-dispatch)). The engine's filter is the stock one with different flags and costs and needs no subclass. |
| `BUILD_SHARED_LIBS` | (global) | OFF | Recast has no static/shared switch of its own and follows the global one. |
| `CMAKE_POLICY_VERSION_MINIMUM` | — | 3.5 | Recast 1.6.0 declares `cmake_minimum_required(VERSION 3.1)`; CMake 3.31 deprecates anything below 3.5 and CMake 4 refuses it. |
| `EXCLUDE_FROM_ALL` (FetchContent) | — | set | Keeps `DebugUtils` and `DetourTileCache`, which nothing here links, out of the build, and Recast's `install()` rules out of ours. |

Three things bit hard enough to be worth naming:

- **`src/recast.h` shadows `<Recast.h>` on Windows.** Recast's headers have no directory prefix,
  and MSVC matches include names case-insensitively, so a `src/recast.h` on the include path
  answers `#include <Recast.h>` with itself; `#pragma once` makes the second inclusion empty and
  every Recast type is suddenly undeclared with no hint of why. The same file compiles on Linux.
  The header is called `src/recast_backend.h`.
- **`rcBuildPolyMeshDetail` with a sample distance of 0 reads out of bounds and dies** (Recast
  1.6.0, reproduced on the bench scene). The guard that protects sliver polygons is
  `minExtent < sampleDist * 2`, which is dead when `sampleDist` is 0. `detail_sample_dist` below
  0.9 therefore **skips the detail stage entirely** rather than calling it with 0 — which is
  cheaper, and is what a caller asking for no detail mesh meant: `dtCreateNavMeshData` builds
  implicit detail from the polygons themselves when `detailMeshes` is null.
- **The `/Ob2` fix that domain/physics needs for Jolt is worth nothing here, and is not applied.**
  The suspicion was the same — CMake's `RelWithDebInfo` uses `/Ob1`, "inline only what is marked
  `inline`", and a voxel pipeline is small helpers all the way down — but measured both ways on
  the bench scene it is a wash: 7.35 / 32.9 / 135.8 ms with `/Ob1` against 7.39 / 32.7 / 135.8 ms
  with `/Ob2` for 32, 64 and 128 m tiles. Recast marks its own hot helpers `inline` in the
  headers, so `/Ob1` already has them. The flag is left off rather than kept "just in case",
  because an option nobody can justify is one the next person has to re-measure.

On the pipeline's own knobs: region sizes are given in **world metres** (the side of a square) and
converted to Recast's voxel areas in one place, because a caller who has to know the cell size to
ask for "drop islands under two metres across" will get it wrong the first time the cell size
changes. The heightfield's vertical extent is fitted to the geometry actually inside the tile
rather than taken from `height_min`/`height_max`: Recast clamps a triangle below the field onto
its floor, which would invent a walkable surface at the bottom of the column, and a column sized
for the whole world costs a span index per `cell_height` of it.

## LOD policy, determinism stance, and zero cost when unused

[ADR-0027](../adr/0027-additive-capabilities.md) asks every capability for all three, in writing.

**LOD.** Detailed paths at LOD0/LOD1, region-graph estimates beyond
([05 §5.15](../plan/05-simulation.md#515-capability-inventory)). The module provides both tiers
and holds no opinion about which tier an entity is in — tier assignment is the simulation
scheduler's. A tile is built on demand, so a region with no navigating agents in it has no tiles.
Crowd agents are a LOD0 construct: an entity demoted past LOD1 removes its agent and moves on its
corridor, and one demoted further moves on an estimate.

**Determinism.** `NavTileData` and `NavMesh` are **hashed**: a tile's bytes are a pure function of
its input, every query is a pure function of the mesh and its arguments, and `apply_ready` orders
results by tile so nothing downstream sees a scheduling artefact. Nothing here reads a wall clock
except the queue's own build-time telemetry, which no gameplay path reads. `Crowd` is **derived**:
DetourCrowd smooths velocities against a time step and replans through its own path queue, so
agent positions are not part of the sim hash; gameplay-relevant movement is a corridor from
`find_path` advanced by the sim.

**Zero cost when unused.** No instances and no linked code
([11 §11.10](../plan/11-performance-principles.md#1110-absent-capabilities-are-free)). A game
that never calls `build_tile` has no tiles, no queue, and no graph; a game with no crowd has no
`dtCrowd`, no proximity grid and no path queue, because the crowd is a separate class rather than
a member of `NavMesh`. With `ENGINE_WITH_NAV=OFF` the module, its tests, its bench and the Recast
download are all absent, and the minimal preset proves the rest of the tree does not reach for it.

## What is not wrapped yet

Detour's **tile cache** and its **temporary obstacles** (`dtTileCache`) — the cheap "a crate
landed here" path that does not need a rebuild; **jump-link generation**, so off-mesh links are
authored or produced by the destruction system rather than found automatically; **formation and
group movement** above the crowd; layered (multi-level) tiles, so a tile is one floor;
`dtNavMeshQuery`'s sliced A* (a path spread over several ticks), random-point queries,
local-neighbourhood and wall-distance queries; serialization of a whole mesh as one blob; dynamic
area marking (convex volumes, cylinders) after a tile is built; and per-agent query filters in the
crowd, which today shares one. None of them needs the public surface to change shape; each is an
addition.

**Testing.** `tools/dev.ps1 test -Preset msvc-debug -Filter nav`. Thirty-nine cases across five
files: a tile builds from a floor and spans exactly its grid square; a wall splits a tile into two
regions, with border portals when the geometry reaches past the tile and none when it stops at it;
the same input builds byte-identical bytes twice and survives a round trip through `from_bytes`
and `clone`; a tile with no geometry is a legal empty tile; the parameter validator refuses a cell
size that does not divide the tile size, a bad slope, too many vertices per polygon, and a tile
smaller than its own border; malformed geometry and a mismatched area array are refused; the tile
grid floors rather than truncates and the tile key round-trips for negative coordinates; per-
triangle areas survive the pipeline and decide which filter can reach where; the cheap settings
(half resolution, monotone regions, no detail mesh) build; a path goes around a wall and is
straight after the wall is destroyed and the tile rebuilt; a wall all the way across makes the
goal `Partial` rather than an error; an off-mesh link across a gap shortens the path, removing it
restores the long way, and a walk-only filter never used it; a link added before its tile is baked
in when the tile lands; `find_nearest` snaps and `raycast` stops at a wall; a corridor that does
not fit reports what did; a tile built on another grid is refused; the tile and polygon caps are
checked against the 32-bit polygon reference; a path crosses four tiles and becomes `NotFound`
when the far tile is removed; the region graph has one node per tile of open floor joined at the
borders, its estimate tracks the detailed path within a bounded factor, it never claims a route
the detailed mesh lacks, an off-mesh link joins two regions in it too, rebuilding a tile keeps it
in step and removing one leaves nothing dangling, and it can be fed from the bytes the mesh
already holds; the queue coalesces a doubly-requested tile into one build with the second input,
runs builds off the calling thread and applies on it, works with no job system at all, applies
nine tiles in tile order, serves the nearest observer first, evicts at the pending cap, drains
under the starvation guard, removes a tile whose floor was destroyed, and refuses a bad request
before copying anything; and crowd agents walk to a target, walk around a wall, respect their cap,
fail every call on a stale handle, refuse a target with no mesh near it, and refuse to start
without one.

The size table pins `TileCoord` and the two handles at 8 bytes, `PathFilter` at 28, `NavPoint` at
24, `PathResult` at 12, `RaycastHit` at 32, `TileRegionInfo` at 44 and `TilePortal` at 24 (the
coarse tier's whole resident cost), `RegionNode` at 52, `OffMeshLink` at 32, and `AgentDesc` and
`AgentState` at 48 and 56.

**Performance notes.** `tools/dev.ps1 bench -Preset msvc-release -Filter "nav.*"`, measured on an
i9-10980XE (18 cores), `RelWithDebInfo`, medians of 15 repeats. The scene is
[E11](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing)'s: a 1 m-cell
heightfield with a box on roughly one cell in twenty — about 10.6k triangles for a 64 m tile — and
the geometry handed to a build is exactly the tile's own square, so the numbers are the pipeline's
and not the caller's triangle rejection. One machine settles knob and layout decisions, not
cross-machine defaults ([11 §11.8](../plan/11-performance-principles.md)):

| benchmark | ms per tile |
|---|---|
| 32 m tile, 0.25 m cells, watershed + detail mesh | 7.2 |
| 64 m tile, same | 32.3 |
| 128 m tile, same | 133.4 |
| 32 m tile, 0.5 m cells, monotone, no detail mesh | 2.0 |
| 64 m tile, same | 8.4 |
| 128 m tile, same | 42.5 |

| benchmark | tiles per second |
|---|---|
| queue, sixteen 64 m tiles, 1 efficiency worker | 23.1 |
| queue, same, 4 workers | 41.8 |
| queue, same, 8 workers | 68.5 |

Three things the table says. **Cost is quadratic in the tile's side** at a fixed cell size — 4.5×
and 4.1× for each doubling — so tile size is the budget's main lever and it costs latency, not
throughput: a 128 m tile is one 133 ms job that cannot be split, where the same ground as four
64 m tiles is four 32 ms jobs that can. **The knobs are worth a factor of 3.2 to 3.9**, so E11's
"nav rebuild budget" is a range and not a number; halving the voxel resolution, switching to
monotone partitioning and dropping the detail mesh buys most of it, and costs region shape and
the ability to read a ramp's height correctly. **The queue scales 3.0× from one worker to eight**,
not 8×: the pipeline is memory-bound (a 64 m tile at 0.25 m cells is a 268 × 268 column
heightfield), SMT siblings share it, the efficiency pool runs below normal priority, and sixteen
tiles on eight workers is two rounds with a visible tail. The queue's per-tile cost (43 ms at one
worker against 32 ms standalone) also includes `dtCreateNavMeshData`, the region-graph update, and
the per-triangle reject over sixteen tiles' worth of geometry — which is the measurement behind
`TileGeometry`'s advice that a caller who can cheaply pre-cull should.

Other hot-path decisions: tile bytes are decoded into typed vectors per bake rather than read in
place, because the arrays are `u16` and `f32` views of a `Vector<u8>` and reading them in place
would be an aliasing and alignment bet against a pipeline that costs milliseconds; the A* scratch
in `RegionGraph` is reused across queries with an epoch stamp, so an estimate allocates nothing in
steady state and its priority queue is a plain `u64` heap of `(float bits << 32 | node)` with no
comparator; `NavMesh` keeps one `dtNavMeshQuery` and one polygon-corridor buffer sized at
`init`, so a query allocates nothing; and `Crowd::read_positions` is one pass over the caller's id
array.

`NavMesh` and `RegionGraph` are **not thread-safe**, and neither is a `dtNavMeshQuery`: one mesh
has one query object, so concurrent path queries need several, which is not wrapped yet. The
crowd has a query object of its own, so updating it does not contend with a caller's queries.
