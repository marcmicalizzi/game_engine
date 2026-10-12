# scatter (systems)

**Purpose.** The scatter generator ([07 §7.6](../plan/07-content-pipeline.md#76-procedural-generation-volume-content), roadmap G37): a capability that places instances of a few meshes over the ground **deterministically, by rule** — rocks, dry scrub and debris on the desert now, and the placer the vegetation rows (G54's tree solver, G55's ecology) will stand on. A **scatter layer** is data (`engine.scatter.Layer`): a seed, a region, a density field, rules per placed kind, exclusions and a budget. It registers as the placement generator `scatter` in the scene-generator registry ([scene_gen](scene_gen.md), [ADR-0046](../adr/0046-scene-generators-register-themselves.md)), so a scene names a layer the way it names the ruins or the city — `{"generator": "scatter", "params": <layer>}` in its `placements` — and nothing below a host names it. It does not author meshes (beyond a procedural rock to look at), grow anything, simulate, or write documents.

**Why this shape.** Three choices carry it, each against an alternative that was on the table.

- **A placement generator, not a system with a tick.** A layer's output is a pure function of (layer, seed, tile, ground), which is exactly the contract a placement generator already has (`expand`, `tile`, `meshes`, `occupies`, `representation`), and the renderer's scene reader and the world's placements consumer already turn `scene_gen::Placements` into instances that cull and LOD like everything else. So the capability adds no render pass, no scheduler entry and no instance path of its own; what it owns is where things go.
- **A lattice and two rounds of local maxima, not Bridson's queue.** Bridson's algorithm grows a pattern from a seed point, so a point's fate depends on the order the pattern reached it: run per tile, two tiles disagree at their border; run over the union, a tile depends on which neighbours were present. Streaming needs the opposite — **a tile's placements a function of the layer, the seed and the tile alone**, so streaming a tile in never changes a placement already visible. So every cell of a global lattice (side r / √2 for a kind of spacing r, integer millimetres) holds one candidate at a hashed place with a hashed priority, and a candidate is kept by two rounds of Luby's rule: in round one, a candidate with no higher-priority candidate within r; in round two, among the candidates round one neither kept nor shadowed (none kept within r), the same rule again. A candidate's fate depends on the candidates within 3 r, which any rectangle computes from the lattice over its own cells and a six-cell apron. That is the **border apron**: a point near a tile border is decided by the tile its position is in, with the neighbour's candidates in view, and the same point is decided the same way by any request that contains it. No two kept points of a kind are within r, by construction; the pattern is blue noise, a little less dense than Bridson's maximal one (0.48 / r² against about 0.7 / r²), which a sparse desert never reaches.
- **Integers and a hash where it decides.** Places are integer millimetres of the world (ADR-0053's lattice), the priorities and every draw a 64-bit hash, distances compared in i64; the density field, the slope, the scale and the rotation are f64 `+ - * /` and `sqrt`, which IEEE rounds the same on every toolchain with floating-point contraction off ([ADR-0035](../adr/0035-no-floating-point-contraction.md)). Nothing calls the C library inside a decision: `sin_cos` and the uphill yaw's atan2 are the module's own polynomials. That is what lets a hash pinned on MSVC hold on GCC and Clang, as geometry's does ([geometry](geometry.md#the-same-bytes-from-every-toolchain)).

## What a layer is

`systems/scatter/schemas/scatter.schema`, the capability's own types (`engine.scatter.*`, version 1):

| Field | What |
|---|---|
| `seed` | the layer's; 0 takes the world's (the scene terrain's) |
| `tile_size` | the layer's tile edge, 32 m by default; a streamed world's must be the same |
| `region` | `Scene` (every tile a streamed world asks for, without end), `Tiles` (`tile_min`..`tile_max`, both included) or `Polygon` (`[x, z]` world metres, even-odd) |
| `density` | placements a square metre, shared by the kinds by `weight`: `Constant` (`per_m2`), `Noise` (`per_m2` times a value-noise fBm of world position: `octaves`, `wavelength`, `gain`, `lacunarity`, mapped from [`low`, `high`] to [0, 1]) or `Mask` (`per_m2` times the red channel of a `.tex` file over `mask_min`..`mask_max`, zero outside) |
| `kinds[]` | per placed kind: `mesh` (a glTF, GLB or `.clusters` path relative to the scene, as `engine.scene.MeshPlacement` names one) or `rock` (the procedural rock, below); `weight`; `spacing` (no two of the kind closer); `clearance` (none closer to an earlier kind's placement); `slope_min_deg`/`slope_max_deg`; `height_min`/`height_max`; `stand` (`Surface`, the ground at its own time, or `Floor`, the ground that does not move under it); `yaw` (`Random`, `Uphill` — the mesh's +z up the slope — or `Fixed`, plus `yaw_deg`); `scale_min`/`scale_max`; `align` (the mesh's +y to the ground's normal); `sink` (metres below the ground at scale 1); `far` (the budget, below) |
| `exclude` | `circles` (a `worldpos` and a radius: a landmark, an editor's placement), a `mask` (`.tex`, nothing where its red is at least `mask_threshold`), `layers` (another layer's file and a clearance: nothing within it of that layer's placements) |
| `budget` | `per_tile`: the most placements one tile keeps, of every kind |
| `meshes` | where the procedural rocks' GLBs are written, relative to the scene; empty is `scatter` beside it |

A layer is **scene data, not a document record**: it has no `materialize` declaration and its output is never written back as records. What an editor edits one by one is `engine.scene.MeshPlacement` ([doc_scene](doc_scene.md)); a layer's output becoming records an editor can override is a row of its own when it is wanted (below, "Not yet").

## The pipeline, and the order it runs in

For each kind, in the layer's order, over a rectangle of the world:

1. **The Poisson-disc**: the lattice candidates of the rectangle's cells and their apron, the two rounds, the kept points whose place is in the rectangle.
2. **The density**: a point is kept with probability `density(x) · share / (k_packing / r²)` — the field at the point, the kind's share of the weights, over what the lattice packs at that spacing — by a hash of its own candidate, so the density is met in expectation wherever it is under the cap (the tests hold it within 5% over 512 m squares, constant, by weight, noise and mask) and a kind past the cap is as dense as its spacing allows.
3. **The region**.
4. **The exclusions that need no ground**: circles, the mask, the other layers (each evaluated here from its own file, seed and rules — never asked of another entry, ADR-0046 decision 4 — over the rectangle grown by its clearance; a layer an exclusion names may not exclude by layers itself, so the evaluation is one level deep), and the clearance from the earlier kinds, which are placed over the rectangle grown by that clearance so a point sees every earlier point near it whichever rectangle asked.
5. **The ground** (`scene_gen::Ground`, the view every placement generator is handed: the surface or the floor in whole millimetres): the height, then the slope by central differences half a metre either side where a rule needs it (a slope limit, an uphill yaw, alignment).
6. **The placement**: the scale, the yaw, the alignment and the height less the sink, as a `Point` (integer millimetres in x and z, f64 height, a float32 quaternion, the yaw, the scale, the kind, and the budget's rank).

Then, per tile, the **budget's cap** keeps the `per_tile` placements of lowest rank (a hash of each one's candidate, not the order they were made in), and the **ring's mask** leaves out the kinds a far ring does not place. A tile's output is sorted by (kind, z, x), so two runs give the same bytes.

**Where exactness ends.** The lattice's cells are i64 millimetres, so a layer is exact wherever a cell reaches; the density's noise reads f64 metres, exact to a nanometre at 10,000 km. A rectangle's apron is six cells, so a kind's work per tile grows as (tile / spacing + 12)²: a 32 m tile at a 1 m spacing is about 2,700 candidates.

## Determinism

`k_determinism` is **"derived"**: a layer is rebuilt wherever it is asked for and is never part of the simulation's state. **The tile rule**: a tile's placements are a function of the layer, the seed, the tile and the ground alone — the same asked alone or beside its neighbours, in any order, on any thread — and the tiles of a block are the block, point for point, with no point at a border lost or doubled. **The bytes**: `hash_points` over the pinned layer's 16 tiles is `1ab4c8d051dbb2cb` (516 placements) and the procedural rock's GLB `7a6687b85300637a`, taken on MSVC; `tests/scatter_tests.cpp` holds both, so GCC and Clang must reproduce them, and a change that moves them moves every scattered scene and says so in its commit.

## The budget is the LOD policy

A capability declares its LOD policy (ADR-0027); this one's is a budget, because what costs is **pairs** (an instance times its clusters, every one visited by the cull pass every frame), and a smaller mesh is not the answer until the tree solver's cards exist:

- **`far` per kind**: in a streamed world, a ring whose nearest tile is farther from the camera than `far` places none of that kind (`Placer::ring_kind_mask`; the ring's inner radius in tiles times the tile edge). The set a ring keeps only shrinks outwards, so the generator's `representation` is the number of kinds it leaves out, and a tile moving between two rings of one tier is not built again. **Far tiers place fewer kinds, not smaller meshes.**
- **`per_tile`**: the most placements a tile keeps, decided before the ring's mask, so a far ring draws a subset of a near ring's placements and a ring change never moves or adds one.

A whole read has no camera, so it places every kind and only `per_tile` applies; the cull pass and the cluster LOD do the rest.

## Where it enters the renderer

**A whole read** (no `world`, or a `placements` entry with `"stream": false`, `engine.scene.PlacementEntry` version 2): the scene reader opens the layer, expands its region (refusing a region of the whole scene, which has no whole), and its placements become plain `SceneInstance`s, each at its `WorldPos` with its rotation and scale ([renderer](renderer.md#scenes-camera-paths-and-flythroughs)). The kinds' meshes are appended once and shared by every instance, so a thousand rocks are one mesh's clusters loaded once through the derived-data cache. More than 32 tiles go to the job system's performance pool in contiguous runs joined in tile order, so the instances are the ones one thread makes.

**A streamed world** (`world`, `stream` true): the reader asks for the meshes, and the world's placements consumer (`world::PlacementTiles`) calls `tile` for each tile its ring activates, in that ring's tier, and hands the tail to `set_dynamic_instances` ([world](world.md#the-consumers), [renderer](renderer.md#instances-that-come-and-go)). **What is not possible yet**: a world that draws its ground from tiles (`world.ground`, desert-endless) refuses streamed placements, because a scene whose instances come and go has neither the deformed-vertex pool the tiles are drawn through nor the ray tracing chain ([renderer](renderer.md#the-ground-from-the-worlds-tiles), "What is refused"). That is why `stream: false` exists: a **bounded** layer on such a world is read whole with the scene. An endless layer streamed beside the tiles needs those two laid out over a tail, which is renderer work of its own.

## The procedural rock

A kind with no `mesh` places a `Rock`: an icosahedron subdivided `detail` times by midpoints, each vertex pushed out along its direction by a seeded value noise of `roughness`, stretched to the half-extents `size_x`, `size_y`, `size_z`, lowered so `buried` of its height is under the origin, welded by exact position and given area-weighted normals, with one metallic-roughness material of the given linear colour — `detail` 2 is 320 triangles. Its GLB is written once, when it is not there, into the layer's mesh directory under `rock-<hash>.glb`, the hash of its fields and `k_rock_version` (the way the city writes its proxy boxes beside its plan): a file already there under the name is the right one. A scene directory that holds one ignores it (`content/test-scenes/desert-endless/.gitignore`): nothing binary is committed.

## The desert's layer

[`content/test-scenes/desert-endless/`](../../content/test-scenes/desert-endless/README.md) carries one layer, `erg-rocks-and-scrub`, read whole (`stream: false`) over a polygon a kilometre wide along the camera path's 12 km: two procedural rocks — a sandstone-coloured boulder (6 m spacing, slope under 18°, aligned, 0.6–1.8 scale) and a dark low tuft standing in for dry scrub (4 m spacing, 2.5 m clear of a boulder, slope under 24°) — at a noise density of up to 0.006 a square metre, at most 12 a tile. The E10 prop corpus is the owner's and outside the repository, so the scene names no mesh of it; a kind's `mesh` is where one goes. What it costs against the frame budgets is in the README.

## Owned data

The layer as read (`Placer`, immutable once opened, so any number of tiles may be placed on any number of threads), the masks it decoded, the excluded layers' placers, and the procedural rock's file. No state between calls: a tile is computed whole each time it is asked for.

## Invariants

- A tile's placements are a function of (layer, seed, tile, ground): the same twice, in any tile order, alone or beside its neighbours; the tiles of a block are the block (`scatter_tests.cpp`).
- No two placements of a kind are closer than its spacing, nor a kind's placement nearer an earlier kind's than its clearance, across every border.
- Slope, height, region and every exclusion hold for every placement.
- The density is met within 5% over a large region wherever it is under the spacing's cap.
- The budget keeps at most `per_tile` a tile, a subset of the uncapped tile; a far ring's tile is a subset of a near ring's.
- `tile` over the region's tiles is `expand`.
- The pinned hash is the same on MSVC, GCC and Clang.

## Public API

`include/systems/scatter/scatter.h`: `Placer` (`open`, `place_tile`, `place_rect`, `occupies`, `region_tiles`, `ring_kind_mask`, `density_at`), `Point`, `position`, `RectMm`, `Mask`, `kind_meshes`, `make_rock`, `rock_glb`, `rock_file_name`, `hash_points`, `sin_cos`, `k_packing`, `k_placement_generator`.

## Depends on

`base`, `containers`, `math`, `hash`, `json`, `schema`, `schemas`, `io` (masks, excluded layers, the rock's file), `log`, `jobs` (a whole read's pool), `time`, `scene_gen` (the registry and the ground), `texture` (a `.tex` mask), `scatter_schemas`. No renderer, no document, no other capability; the tests link the renderer and `gfx` for the scene case.

## Testing

`tools/dev.ps1 test -Filter scatter`. `tests/scatter_tests.cpp`, over a synthetic ground of two planes and with no renderer: sin and cos within 2e-15 of the C library; the same bytes twice and in reverse tile order; a tile alone, beside its neighbours and as part of a 4 × 4 block, with the spacings and the clearance across every border; the packing measured over 4 km² against `k_packing` to 1%; slope and height limits on the planes; the density within 5% (constant, two kinds by weight, a noise against its own integral, a half-lit `.tex` mask with nothing in its dark half); circles, a mask, another layer's file and an earlier kind exclude, and a nested layer exclusion is refused; the budget's cap and the far ring's subset; the pinned hash; and the generator through the registry — the procedural rock written once, `tile` over the tiles equal to `expand`, an unknown field refused with its path, a whole read of the whole scene refused, and a streamed layer whose grid is not the world's refused. `tests/scatter_scene_tests.cpp`: a scene of the renderer's waves naming a layer is read as one rock mesh and about 300 instances standing on the ground — to the bit against its surface and within the drawn grid's cell — the same on a second read, and on a GPU 223 of 295 of them in the capture's ids channel from 40 m up. `tests/size_table.cpp` pins `Point` (56 bytes). The removal proof is `msvc-minimal` and `linux-clang-minimal`, which build and pass without it.

## Performance notes

A whole read of the desert's layer (about 12,700 tiles) is measured in its README. A tile's cost is the lattice over its cells and apron, times the kinds, plus five ground queries for each candidate a slope rule keeps — the ground is the cost on the dunes, a few microseconds a query — and a kind with a clearance places the earlier kinds over its apron again. Nothing here runs per frame: a whole read runs once with the scene, a streamed tile once per activation. Not measured yet: a streamed layer's cost per activation in a flight.

## Not yet

- **Document records.** A layer's output becoming `MeshPlacement` records an editor can move, delete or pin, with the layer re-run around them, is the document path (`materialize` or a derived layer); this slice writes none.
- **Exclusion by another layer's footprints** beyond the simple case: another layer's placements as circles of one clearance, from its file. Footprints as shapes (a building's outline, a road's band), other generators' output (the ruins' walls, the city's lots) and mutual exclusion between entries need the order rule to hand one generator's output to the next, which ADR-0046 decision 4 forbids today.
- **Streaming beside ground tiles**: the renderer's dynamic tail with the pool and the ray tracing chain (above).
- **The tree solver** (G54) and its cards for the far tiers, and ecology (G55): the placer is what they will stand on; until then a far tier places fewer kinds.
- **A moving ground**: a kind standing on the surface stands on it at the scene's time; under a time-lapse the dunes move and the placements do not.
- Distance to roads and water (§7.6's list): no roads or rivers exist yet (G38).

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph, the scheduler, or another capability, and it can be removed from the build the same way. Outside its directory it changed the hosts' generator lists (engine-view and engine-host link it for its registrar) and the scene schema's `PlacementEntry.stream` with the reader's one condition on it — the renderer's, not a capability's.

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | none | stands alone |
| Schema types | `systems/scatter/schemas/scatter.schema` (`engine.scatter.Layer`) | done |
| Scene-generator registry | the placement generator `scatter` (`src/scene_generator.cpp`) | done |
| Tick scheduler entry | none: it runs when a scene is read or a tile streamed | not needed |
| Render-graph passes | none: its output is instances the renderer draws | not needed |
| Content-build derived step | the procedural rock's GLB, named by its hash | written on first read |
| Protocol methods | none: a layer reaches a host as a scene's placements entry | not needed |
| Tunables | none: a layer's budget is its own data | not needed |
| LOD policy | the budget: `far` per kind by ring, `per_tile` per tile | done |
| Determinism | `scatter::k_determinism` = `derived`; the pinned hash | done |
| Zero cost when unused | a scene that names no layer never calls it; OFF removes it | done |
| Tests and size table | `tests/scatter_tests.cpp`, `tests/scatter_scene_tests.cpp`, `tests/size_table.cpp` | done |
| Bench | none: nothing per frame; the scene read is measured in the desert's README | not yet |
| Removal proof | `ENGINE_WITH_SCATTER`, off in the minimal build | works |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_SCATTER=OFF`) drops the module, its schema library and its tests; the module disappears from `build/<preset>/modules.json` and is listed there under `disabled_capabilities`. Everything else still builds and passes, and a scene naming `scatter` is refused with the registry's sentence.
