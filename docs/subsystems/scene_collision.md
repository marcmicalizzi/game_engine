# scene_collision (systems, capability)

**Purpose.** Static collision bodies from the scene, kept round a walker: the **ground** as a heightfield a tile, sampled from the scene's ground provider (`scene_gen::GroundProvider`: the renderer's waves, the terrain capability's dunes) at a collision spacing, and the **placements** — what the scene's placement generators put on the tile, the ruins' walls, corners and rubble — as one static compound a tile of each piece's proxy, in a `physics::World`, for a `physics::CharacterBody` ([physics](physics.md#the-character)) to walk on. It is a world consumer ([world](world.md#the-consumers)): the bodies of a tile exist while the walker's tile ring holds the tile, and are made and let go as the ring moves, within the ring's budget. It draws nothing, builds nothing the renderer draws, and links no generator — each is found in the scene-generator registry by the name the scene gives ([scene_gen](scene_gen.md)). Its first host is engine-view's walk mode ([apps](apps.md#walking)); a game's player, and later an NPC at LOD0/1, walk on the same bodies.

## Why this shape

- **A consumer of its own ring, stepped at the walker's ticks.** A character's contacts are sorted by body id ([physics](physics.md#the-character)), and body ids come out of the physics world's free list in creation order, so *when* a body is made decides which id it gets and so, bit for bit, where a walker that touches it ends up. A consumer registered in a host's drawing ring — engine-view's `--world` ring, updated once a frame from the camera — would make bodies at frame boundaries, which a live window and its offscreen replay place at different ticks. So the walker's host keeps a ring of its own, one ring of `walk.collision.radius_tiles` round the walker's feet, and updates it once a tick, before the step (`view::Walker`); the same session and its replay make and let go of the same bodies at the same ticks, and a recorded walk replays bit for bit. The ring is the world capability's own (`world::World`), with its hysteresis, its budget and its tile order: the rule for which tiles are held is the one every other consumer is held to, not a second one.
- **Registered after the ground and the placements.** In a world that also holds the scene's ground and placements consumers ([world](world.md#the-consumers), "The order"), this row goes after them: a tile's collision is made from what is on the tile, after it is there, and let go before it goes. engine-view's walker keeps a ring of its own (above), where it is the only row.
- **Where the placements come from.** A **streamed** scene (`renderer::SceneData::streamed`, `--world` or a scene's `world` block) keeps its placement entries for the world, and this consumer opens each entry's generator again and asks it for the tile (`scene_gen::PlacementGeneratorDesc::tile`) in the representation the scene's **inner ring** draws — what is drawn round the walker: the ruins' blocks where the scene lays blocks near, their sections otherwise. A scene **read whole** has its placements among its instances already, so a tile's placements are the instances whose bounds reach into it — the generator's `tile` is the whole read restricted to the tile, so this is the same set, and it also gives the scene's own meshes a body (an instance counted by every tile it reaches, so a wall across a tile edge is whole from either side). The terrain's instance is the ground's, and a skinned instance moves, so neither gets one.
- **One compound a tile, one proxy a mesh.** A ruin laid in blocks is several hundred pieces; a body each would put hundreds of bodies a tile in the broadphase. A static compound (`physics::World::create_compound`) is one body a tile with its own tree over its pieces, and its pieces are shared shapes: **one proxy per mesh and scale**, however many instances.
- **The proxy is the mesh's own LOD DAG cut at 5 cm, never the full-detail mesh.** The choices the brief named were the member's own coarse level, its convex hull, or its boxes. A hull or boxes fill a doorway and a window — a walker could not pass through the ruins' own openings, and a fallen wall's broken top would be a sheer box — so the proxy is the member's **coarse level**: the cut of its cluster DAG where each cluster's own error is within `walk.collision.mesh_error_m` (5 cm) and its parent's is not — `geometry::lod_selects`' rule with the threshold in metres instead of pixels — the coarsest surface that stays that close to the full one, welded by source vertex so the backend sees shared edges (and finds its internal ones), as a static triangle mesh. For a member of the kit of boxes, or a block of the synthetic block kit, that is the mesh itself (a box's 12 triangles, a low block's 44); for a 3 m test ball of 8,192 triangles it is 510, whose surface stands at most 4.2 cm off the leaves' (the test below); a scanned member of thousands of clusters is cut to its coarse clusters and never collides as its leaves. The error is in the mesh's own units divided by the instance's largest scale, so a scaled instance gets its own proxy at the right error.
- **The broadphase is rebuilt once an update that changed bodies** (`commit`, and after a refresh that rebuilt a tile). The backend's broadphase trees give back the nodes of the bodies taken out of them only when a tree is rebuilt, which `physics::World::step` does and a world like this one — static bodies and a character, never stepped — never would. So a walker's world ran its trees out of nodes after a few thousand tile changes: the ring-change bench (`scene_collision.ring_change`, three tiles in and three out an update) found it within seconds as Jolt's `QuadTree: Out of nodes!` and an assert, which in a Debug build on Windows is a modal abort box on the owner's screen that holds the process until somebody clicks it (2026-09-28). A rebuild of a handful of bodies — two a tile, nine tiles — costs microseconds. The long-walk test runs seven thousand bodies through a world of 256 with it, but did not fail without it (single bodies added into a root with a free slot allocate no node); the bench, whose updates add six bodies at once, is the case that does, which is a reason it is run.
- **The ground is a heightfield on the world's lattice.** A tile's heightfield covers the lattice points from the one at or west of its west edge to the one at or east of its east edge — 34 × 34 at the default 1 m on 32 m tiles, because the backend stores heights in 2 × 2 blocks and wants an even count — on the world's grid at the spacing (`scene_gen::ring_lattice`), so two neighbouring tiles share their edge samples bit for bit and the seam is closed. The heights are the provider's `grid` at its own time: exactly its `height` at those points. The backend quantizes them to 8 bits within each 2 × 2 block's range, half a millimetre at most measured on the test's saddle.

## The ground moves

Under a time-lapse ([renderer](renderer.md#the-dunes-in-time-lapse)) the drawn sand is not the ground's function at one time: each terrain level draws **two fields and a blend**, `a (1 − t) + b t`, the pair timed by how far the fastest band travels and crossed within a per-frame bound. The collision follows **the drawn pair, not the true surface**, so the walker stands on the sand the player sees, cross-fade and all: the host hands the consumer the finest level's pair and blend once a frame (`GroundTime`, from `renderer::TerrainMotion::level_stats`), and every tile keeps the two fields on its own lattice and the heights its body was built with.

**The rule** (`refresh`, once a tick): a tile whose drawn pair changed has the new field evaluated — when the level handed over, its old b is its new a, the same heights to the bit, and only the new b is evaluated; and a tile whose body's heights stand more than **`walk.collision.ground_error_m` (5 cm)** off the drawn heights at any of its samples — the pair blended exactly as the renderer blends it — has its body rebuilt at the drawn blend. At most `walk.collision.max_refreshes` (2) tiles of that work a tick, nearest the walker first. Both surfaces are the same piecewise-linear interpolation between samples on a lattice, so the largest difference is at a sample and the rule bounds it everywhere on the collision's lattice.

**What that allows, and what the test holds** (`tests/time_lapse_tests.cpp`, on the committed erg's dunes, no device: the renderer's own cadence rule, `terrain_blend_frame` and fields that arrive on time, as an offscreen run waits for them; a frame is four 240 Hz ticks). **The largest error between the drawn height and the collision height allowed is 5 cm, after every tick's refresh, on every sample of the tile under the walker**; at a game day a real second it measured 4.96 cm over 600 frames, 52 fields and 1,139 rebuilds. Between two frames the drawn sand moves on and the collision is last frame's until the next tick catches it up, so the error the walker can meet before that tick is the bound plus one frame's move of the drawn sand — which the renderer bounds at a quarter of its spacing a frame (0.375 m on the erg's 1.5 m grid) and which measured 13.0 cm at a day a second on the erg (the waves reshaping to a few days' wind, not their travel) — 13.1 cm at the worst rebuild. A walker standing still meanwhile stays on the collision ground to within its capsule's slope offset, the backend's 2 cm padding and that last move: 13.6 cm at worst on the erg's floor, where sand that drops away leaves it to fall after it. **At the game's own rate the sand is still**: ten minutes of real time at a game second a second moved it 8.2 mm at the walker's tile and rebuilt nothing (the test asserts no rebuild). A still ground — no time-lapse, or a ground that does not move — is never refreshed and `refresh` returns at once.

Under a live window's time-lapse the pair and blend follow the renderer's clock, which the display drives; a walk over moving sand therefore replays against the sand the replay draws, and only a walk on still ground is guaranteed to replay bit for bit ([apps](apps.md#walking)).

## Owned data

The tiles it holds (a heightfield body and shape, a compound body and shape, and the heights and fields a tile was built with), the proxies (one shape per mesh and scale, kept for the consumer's life), the open placement generators of a streamed scene, and a scene read whole's bins of instances by tile. The bodies and shapes are the physics world's; this consumer is the only thing that makes or destroys them.

## Invariants

Each is a test in `tests/scene_collision_tests.cpp` (no device, no generator capability: a ground provider and a placement generator of the test's own, meshes whose DAGs the geometry module builds) unless it says otherwise.

- The ground is a heightfield a tile on the provider's lattice: 9 tiles, 9 bodies and 9 × 34 × 34 samples round a walker at a tile's centre; straight down onto the collision the provider's height to 0.5 mm on the lattice (the backend's quantization), and `ground_height` the backend's triangulation to 0.3 mm between.
- Bodies come and go with the ring within its budget: two tiles an update from a first fill, the walker's own among them; walking 2 km east and back with a post on every tile never more than 9 tiles and 18 bodies held, the physics world's body count always the consumer's, every body made either live or let go, and the walker's ground always the provider's; then 1,200 updates a whole tile apart with no budget, 7,846 bodies made in all through a world of 256, and no failure.
- A scene read whole collides as its instances: one proxy a mesh however many instances, a wall's top 3 m up where its box's is, a piece across a tile edge counted in both tiles, and a character walking at the wall stopped at its face less its radius and padding.
- A proxy is the coarsest cut within the error: at 0 the leaves (8,192 triangles of a ball), at 5 cm 510, whose surface stands within 5 cm of the leaves' along eight rays.
- A streamed scene collides as each generator's tile, asked in the inner ring's representation; a generator the executable does not carry is refused with the registry's sentence.
- A moving ground stays within `ground_error_m` of the drawn one after every refresh, under a travelling ripple; a still ground is never refreshed.
- On the erg in time-lapse and at the game's own rate (`tests/time_lapse_tests.cpp`, where the terrain capability is): above.
- The tunables' defaults are the documented ones; the consumer's ring is one ring of the radius, with the budget.

## Public API

`include/systems/scene_collision/scene_collision.h`: `k_determinism`; the tunables' readers `spacing_tunable`, `radius_tunable`, `max_tiles_tunable`, `mesh_error_tunable`, `ground_error_tunable`, `max_refreshes_tunable`; `Config` and `config_from_tunables`; `ring_params_from_tunables`; `GroundTime`; `Stats`; and `SceneCollision` — `create`, `consumer`, `set_ground_time`, `refresh`, `ground_height`, `stats`, `streamed`.

## Depends on

`base`, `containers`, `math`, `hash`, `log`, `time`, `tunables`, `schemas` (a streamed scene's `engine.scene.WorldRings`), `geometry` (the DAG a proxy is cut from), `gfx` (the scene's instances as uploaded), `scene_gen` (the ground's interface and the generators' registry), `renderer` (the scene as loaded), `physics`, `world` (the ring and its consumer table) and `sim` (the observer set). `engine_capability_requires(scene_collision physics world)`: with either off — the minimal build, or `ENGINE_WITH_PHYSICS=OFF` — this is off too, and engine-view's walker follows the ground instead ([apps](apps.md#walking)).

## The tunables

Read once, by the host, into its walk header (engine-view's `walk.collision` block), so a replay collides with the recording's numbers.

| Tunable | Default | What it is, and what it costs |
|---|---|---|
| `walk.collision.spacing_m` | 1.0 | The ground heightfield's sample spacing. A metre is 34 × 34 samples a 32 m tile: 4,624 bytes of heights on the CPU (13,872 with a time-lapse's two fields) and 4,400 in the backend, measured below; half a metre is four times both. The erg's drawn grid is 1.5 m (0.5 m round the camera with `--terrain-rings`), so a metre is finer than what is drawn without the rings, and the walker's ground error against the drawn grid is in engine-view's summary |
| `walk.collision.radius_tiles` | 1.5 | The ring's radius, in tiles, to a tile's centre: the 3 × 3 tiles round the walker's own at a tile's centre, four at a corner — at least 25 m of ground in every direction, and 9 tiles held at most |
| `walk.collision.max_tiles` | 2 | Tiles made an update, the budget (twice as many let go): a walker crosses a tile in 21 s at a walk and 6 s at a sprint, and a tile's ring of three comes in as it approaches, so two an update spreads a crossing over two ticks. The first fill round a camera a walker is dropped from is unlimited |
| `walk.collision.mesh_error_m` | 0.05 | How far a proxy may stand off its mesh |
| `walk.collision.ground_error_m` | 0.05 | How far the collision ground may stand off the drawn one under a time-lapse before its tile is rebuilt (above) |
| `walk.collision.max_refreshes` | 2 | Tiles of refresh work a tick under a time-lapse |

## What it costs

**Bodies.** At most two a tile — the ground's heightfield, and one compound when the tile has pieces — so nine tiles are at most 18 bodies however many pieces they hold; the long-walk test's world never held more.

**Bytes** (`physics::World::shape_memory`, measured by the tests). At the default metre a tile's ground is **4,400 bytes in the backend and 4,624 of heights on the CPU** — 81 KB for nine tiles — and three times the CPU's under a time-lapse, which keeps the drawn pair's two fields beside the body's heights. A compound is about 110 bytes a piece (three of nine pieces: 984 bytes), so a building of the kit of boxes laid in sections, about 88 pieces, is some 10 KB; its pieces' proxies are about 16 bytes a triangle (two meshes of 268 triangles: 4,256 bytes), held once per mesh however many buildings use them.

**Time** (`msvc-release`, [bench](bench.md#measuring-on-a-shared-machine)). A ring change — the walker a tile further on, three tiles made (a 34 × 34 heightfield and a compound of a hundred blocks each), three let go, and the broad phase rebuilt — is **618 µs median and 375 µs at best** (`scene_collision.ring_change`, seven repeats, 2026-09-28; the machine was 95% busy with this change's own builds when the run started and 17% when it ended, and another agent held the GPU lock, so both are upper bounds): about 0.2 ms a tile made, so the default budget of two a tick costs under half a millisecond on the one tick in several seconds that crosses a tile's edge. A tick that changes no tile — the ring's update from the feet and a still ground's refresh returning at once — is **1.09 µs** (`scene_collision.tick`, same run). Under a time-lapse a tile's refresh adds two evaluations of the ground's field over the tile's samples, which for the dunes is the terrain capability's cost and is not measured here: engine-view's summary reports the walker's field evaluations and its largest refresh in milliseconds ([apps](apps.md#walking)).

## LOD policy and determinism stance

[ADR-0027](../adr/0027-additive-capabilities.md) asks for both, in writing.

**LOD.** Bodies exist only for the tiles of the walker's own ring — one ring of 1.5 tiles — so collision is LOD0 and nothing else, which is plan 05 §5.11's "bodies exist for LOD0/1 only" for static geometry; past the ring there is no body at all. Within it the proxy is the placement's own coarse level at a fixed error, not a distance-dependent one: a walker only ever touches what is within a metre of it.

**Determinism.** `k_determinism` = `hashed`. The bodies are a function of the ring's events, the scene and the numbers the host read once: the same ticks with the same walker make and let go of the same bodies in the same order, and the physics world gives them the same ids, which is what a replay needs (above). A generator's `tile` is itself a function of (entry, seed, tile, ground) ([scene_gen](scene_gen.md)). The one input from outside the tick is a moving ground's drawn time (above).

## Testing

`tools/dev.ps1 test -Filter scene_collision`: the cases above, all without a device; the erg's two where the terrain capability is linked. `tools/dev.ps1 bench -Filter 'scene_collision.*'`: a ring change (below). engine-view's end-to-end walks ([apps](apps.md#walking)) are this consumer in its host.

## Not yet

- **The character rig's contribution** — bone capsules against the bodies, foot placement, footprints stamped into the sand's deformation — is a layer over the capsule and these bodies, not a change to them.
- **Streamed ground tiles are not used**: the ground's heights are the provider's, sampled here, not the world's ground consumer's tiles with their deformation overlays ([world](world.md#the-consumers)); a footprint in the overlay does not dent the collision yet.
- **Nothing moves**: every body is static. A door, a platform or a falling block would be a kinematic or dynamic body of the physics world beside these.
- **The rings' finer drawn ground** (`--terrain-rings`, 0.5 m round the camera) is followed in time but sampled at the collision's own spacing, so the drawn ground near the walker can be finer than the collision's.
- **No overlap query or character-against-character** collision (physics.md, "What is not wrapped yet").

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph, the scheduler, or another capability, and it can be removed from the build the same way. Outside its directory it added one read to `domain/physics` (`World::shape_memory`, what the backend holds for a shape, which its costs are reported in) beside the character this capability's host walks.

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | `engine_capability_requires(scene_collision physics world)` | declared; off when either is off |
| Component and event types | none: nothing here is persistent; the bodies are rebuilt from the scene | not needed |
| Tick scheduler entry | none: the ring updates between ticks, as every world consumer's does (world.md, "Between ticks") | not needed |
| Render-graph passes | none: collision is not drawn | not needed |
| Content-build derived step | none: a proxy is cut from the DAG the scene already loaded | not needed |
| Protocol methods | none: nothing over `render.*` walks yet | not yet |
| Tunables | `walk.collision.*` (above) | done |
| LOD policy | one ring round the walker; the proxy's fixed error | done |
| Determinism | `scene_collision::k_determinism` = `hashed` | done |
| Zero cost when unused | no linked code without it; a host that never walks makes no consumer, no physics world and no body | done |
| Tests and size table | `tests/scene_collision_tests.cpp`, `tests/time_lapse_tests.cpp`, `tests/size_table.cpp` (`GroundTime` 32 bytes) | done |
| Bench | `bench/scene_collision_bench.cpp`: `scene_collision.ring_change` | done |
| Removal proof | `ENGINE_WITH_SCENE_COLLISION`, off in the minimal build | works |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_SCENE_COLLISION=OFF`, or `-DENGINE_WITH_PHYSICS=OFF`) drops the module, its tests, and its bench; the module disappears from `build/<preset>/modules.json` and is listed there under `disabled_capabilities`, and engine-view's walker follows the ground ([apps](apps.md#walking)).
