# scene_gen (domain)

**Purpose.** The registration point a scene generator attaches through ([ADR-0046](../adr/0046-scene-generators-register-themselves.md), [02 §2.8](../plan/02-architecture.md#28-adding-a-capability)): the two kinds a generator is — a **ground provider** and a **placement generator** — and the one table, `GeneratorRegistry::global()`, the renderer's scene reader and the world ring look a generator up in by the name a scene gives it. A capability registers a constant-initialized descriptor from its own source with a static `Registrar`; a host links the capabilities its configuration has; nothing below the host names one. A scene naming a generator the executable does not carry is refused with a sentence that names it and says what the executable has (`unknown_ground`, `unknown_placement`). A plain module, not a capability: every executable that reads a scene has at least the renderer's own ground in it.

**Why it exists.** Three generators expand content the renderer draws and the world streams — the ruins, the dunes and Island City — and before this module each reached the reader and the ring by its own route: the ruins and the dunes through an `#if` in the renderer and in the world and a dependency declared only where the capability was configured ([ADR-0037](../adr/0037-scene-reader-links-ruins-where-configured.md), the one exception to [ADR-0027](../adr/0027-additive-capabilities.md) decision 1, taken a second time), the city by no route at all but a fragment written to disk. Each new generator would have added an `#if`, a conditional dependency and a consumer file in two modules that are not capabilities. What the three have in common, seen from the reader and the ring, is what this module states: a generator is a pure function of the scene's parameters, the world seed and a tile, and its output is either **ground** (a height, a floor and a grid of itself, a function of position and time) or **placements** (instances of a few meshes, with tags).

## Who registers, and who looks

**Status, 2026-09-27.** Every generator the tree has is on the registry; this section says where each stands.

| Generator | Kind | Registered by | Looked up by |
|---|---|---|---|
| `waves` | ground (wind) | `systems/renderer/src/terrain.cpp` — the renderer's own, so the default terrain takes the same path a capability's does | the renderer's `TerrainSampler`, for every terrain that names it or names nothing |
| `dunes` | ground (moves, wind, rings, tiles) | `domain/terrain/src/scene_ground.cpp` (its tiles `ground_tiles.cpp`) — the terrain capability; `ENGINE_RENDERER_TERRAIN`, `ENGINE_WORLD_TERRAIN` and both modules' links to `terrain` are gone | the renderer's `TerrainSampler` (the scene read, the mesh, the time-lapse, the rings), by `provider` or the old `generator` enum; the world's ground consumer (`world::GroundTiles`, which replaced `TerrainTiles`) |
| `ruins` | placements (`expand`, `meshes`, `tile`, `occupies`, `representation`) | `domain/ruins/src/scene_generator.cpp` — the ruins capability; `ENGINE_RENDERER_RUINS`, `ENGINE_WORLD_RUINS` and both modules' links to `ruins` are gone | the scene reader (`expand_placements`: a `ruins` entry, or a `placements` entry naming it) and the world's placements consumer (`world::PlacementTiles`, which replaced `RuinsTiles`) |
| `city` | placements (`expand`, `meshes`, `tile`, `occupies`, `representation`) | `domain/city/src/scene_generator.cpp` — the city capability, which reached the reader before only as a fragment written to disk | the scene reader (a `placements` entry naming it: `expand` is what the fragment writer writes, in memory) and the world's placements consumer (`tile` is the plan's per-tile query at the ring's detail) |
| `earth` | sky (`state`, `atmosphere`, `stars`) | `domain/sky/src/sky_provider.cpp` — the sky capability (2026-09-30) | the scene reader (a `sky` entry, to check it) and the renderer's `SkyPass` (every frame's lights and heavens, [renderer](renderer.md#the-sky)) |

**The fixed features.** A terrain entry's ridges and basins (`engine.scene.Terrain.ridges`, `basins`) are the scene's, added by every ground the same way — the waves multiply their dunes down over them and add them, the dunes thin their sand over them and add them — so their arithmetic is here, beside the entry's type, in one copy (`terrain_features.h`: `terrain_features`, `ridge_weight`, `basin_weight`). It was the renderer's until the dunes moved into the terrain capability, and it moved expression for expression.

## Owned data

The table of descriptors: `GroundProviderDesc` and `PlacementGeneratorDesc`, by name, in two lists. It owns the descriptors' *registration*, not the descriptors — each is a `constexpr` object in its capability's own source — and holds no state of any generator: a generator's state is what its `make` or `open` allocated, owned by the handle the caller holds (`GroundProvider`, a placement generator's `void*` it closes).

## The two kinds

**A ground provider** (`GroundProviderDesc { name, make, flags }`) is made from a scene's terrain entry (`engine.scene.Terrain`, as parsed) into a `GroundProvider`: a table of plain functions (`GroundOps`) over the state `make` allocated, which answers:

- `height(x, z)`: the surface at the ground's own time, metres; thread-safe.
- `floor(x, z)`: what a building stands on — the ground that does not move while the surface does (the dunes migrate over the interdune floor and bury what stands there; with the waves the two are the same). Null: the surface.
- `grid(lattice, window)`: a window of heights on a `Lattice` at its own time — the scene's grid, computed exactly as the renderer's mesh computes it, or the world's grid at a spacing in millimetres — what a mesh is built from. Null: `height` at each point, which is the same heights to the bit.
- `evaluate(t, lattice, window, blocks)`: the same window at another game time, in 64 × 64 blocks a caller spreads over as many jobs as it likes: the time-lapse's re-evaluation. Null for a ground with no time; `moves()` says which.
- `travel_m(from, to)`: how far its fastest feature travels between two times, which a moving ground's cadence is timed by.
- `wind(t)`: the wind the ground's surface detail lies across at game time t — the direction the sand moves over (x, z), a unit vector, continuous in time — which the renderer lays the sand's ripples across ([renderer](renderer.md#the-sand-close-up)) without linking the provider. The dunes answer their ripple term's rule, the day's wind turned in from yesterday's over two hours ([terrain](terrain.md#what-the-effect-needs)); the waves their one prevailing wind. Null: the ground says none, and the renderer lays them along +x. It was added to the table on 2026-09-29, after `travel_m`, so a descriptor that names neither is the one it was.
  *Design, not built*: the ripples' exposure is a function of the normal and the wind and cannot see ground a feature shelters — the level floor downwind of a brink, a ruin's lee. A provider that knows its shelter would answer it as a map channel beside the sand share, and the renderer multiply the exposure by it ([renderer](renderer.md#the-sand-close-up), "What the exposure cannot see").
- `transport(t)`: what moves the ripples at game time t — the sand the ground's wind has carried across a metre of width since its epoch, m² (a path length), and the wind's strength over its record's mean. The renderer turns the first into the ripples' travel and flattens them by the second ([renderer](renderer.md#the-sand-close-up), "Ripples that move"); the dunes answer from the wind record's integral. Null for a ground with none, whose ripples stand.
- `make_rings(extent, spacing)`: the rings round a camera (`GroundRings`, over `GroundRingsOps`: the layout rule, the re-centre rule and the chunks' cluster DAGs), when the ground has them.
- `open_tiles(records, tile)`: its tiles in a world ring (`GroundTiles`, over `GroundTilesOps`), their per-tile records kept wherever the world keeps them (`TileRecords`), when it has any.

`GroundProviderDesc::flags` (`k_ground_moves`, `k_ground_rings`, `k_ground_tiles`) say what every ground a provider makes can do, so a host can decide a setting before it has a scene's sampler. `GroundProvider::view()` is the `Ground` a placement generator is handed: the surface and the floor as a plain function and a context.

### Sky providers

**A third kind, added 2026-09-30** ([ADR-0048](../adr/0048-the-sky-is-a-providers-model-drawn-by-the-renderer.md), `include/domain/scene_gen/sky.h`): a **sky provider** (`SkyProviderDesc { name, make }`) is made from a scene's sky entry (`engine.scene.Sky`, as parsed; its `provider` names it, and an empty one means `k_default_sky`, "earth") into a `SkyProvider` over `SkyOps`:

- `state(time_s, SkyState&)`: where the sun and the moon are at a game time — world directions, their illuminances outside the air (the sun's in units of its mean, the moon's in the sun's), angular radii, the moon's albedo and lit fraction — the celestial frame's world axes the stars turn with, and the calendar (the day of the year counted on from the scene's, and the local hour) for a summary. A closed function of the time, thread-safe.
- `atmosphere()`: the air, the same at every time (`Atmosphere`: the planet's radii, Rayleigh, Mie and ozone coefficients and heights, the night's own glow), kilometres and inverse kilometres.
- `stars()`: the star table, brightest first (`Star`: a celestial direction, a visual magnitude, a colour of luminance 1); empty for a sky that draws none.

**Why the sky is a provider and not the renderer's.** What a sky *is* — an atmosphere's coefficients, an ephemeris on a calendar at a latitude, a table of stars — is content a studio with another planet, two moons or a real catalogue replaces, and removable with the capability that registers it (`domain/sky` registers "earth", [sky](sky.md)). Drawing pixels from it is the renderer's and `domain/gfx`'s, which need nothing from a provider but these numbers ([renderer](renderer.md#the-sky)). **The provider is a function of game time alone**, the same seconds since the world's midnight epoch the ground's `wind(t)` reads, which is what ties the sun's hour, the wind's day and the moon's month to one clock ([renderer](renderer.md#one-clock)). The scene reader makes the provider once to check the entry — a name the executable does not carry is refused with `unknown_sky`'s sentence, an entry the provider cannot draw with the provider's own — and the renderer makes it again for the frames. `GeneratorRegistry::find_sky`, `sky_names`, `unknown_sky`, and a third `Registrar` constructor, as for the other two kinds.

**A placement generator** (`PlacementGeneratorDesc`) runs one scene entry — its parameters as JSON (`engine.scene.PlacementEntry.params`, or a legacy field's object) — through `open` (read what the entry names and check it, once), then `expand` (the whole entry, a scene read whole) or `meshes` and `tile` (a streamed scene: the meshes it holds resident, and one tile at a time in the tile's ring), and `close`. Its output is `Placements`: instances of named meshes (a path the reader resolves through the derived-data cache like any scene mesh, a name, a content hash), each with a world transform and a tag, and a count of the things made. `tile` must agree with `expand`: a tile's placements are the whole expansion restricted to the tile. `occupies(tile)` and `representation(ring)` let a streaming host skip what a tile does not need: a tile with nothing on it, a ring change that draws the same.

## The order rule

**Ground first, then placements in the scene's order**, in the reader and in the ring. Ground is what placements stand on, so the reader makes the terrain's provider before it opens the first placement entry and hands each its `view()`, and walks the entries in the file's order (the `ruins` entries, then `placements`); the ring registers its ground consumer before its placements consumer — one function, `world::add_scene_consumers`, so no host spells it — and the placements consumer asks the entries in the same order, so a tile's ground is built before anything on it and goes after it ([ADR-0040](../adr/0040-the-tile-ring.md)'s declared order; [world](world.md#the-consumers), "The order"). Both halves are held by tests that swap two entries and find the ground still first: the renderer's `scene_gen_scene_tests.cpp` for the reader and the world's `scene_order_tests.cpp` for the ring, each with generators of its own and no capability. **A generator sees the ground it is handed and nothing of any other** (`Context`: the world seed, the tile size, the scene's directory, the ground, a streamed world's block and ring, and a job pool made on first ask). A generator that wants another's output asks the ground, never the registry, which keeps each generator a function of (entry, seed, tile, ground) — the per-tile determinism the ruins and the city rest on.

## The registrar pattern

```cpp
// In the capability's own source, beside the functions it names:
constexpr scene_gen::PlacementGeneratorDesc k_ruins{
    .name = "ruins", .open = &open, .close = &close, .expand = &expand,
    .meshes = &meshes, .tile = &tile, .occupies = &occupies, .representation = &representation};
const scene_gen::Registrar k_ruins_registrar{k_ruins};
```

The descriptor is constant-initialized; the `Registrar` adds it to `GeneratorRegistry::global()` during static initialization (a function-local static, so a registrar in any translation unit finds the registry constructed whatever order the units initialize in). The module holding it is **`WHOLE_ARCHIVE`**, or the linker drops the object nothing references and the registration with it. A second descriptor under a name another holds stops the program at start with a message — two generators under one name would make what a scene means depend on link order — while the same descriptor twice is a library linked into two images of one process, and fine. `GeneratorRegistry::add` returns the same answer as a value, which is what the tests hold.

## Adding a generator

1. Write the functions and the descriptor in the capability's own module (`src/scene_generator.cpp` by convention), and a `Registrar` beside it; make the module `WHOLE_ARCHIVE` and give it `scene_gen` as a dependency.
2. Pick a name no other generator of the kind has — the capability's own name is the convention (`ruins`, `dunes`, `city`) — and read the entry's parameters with the schema's reader where they have a schema type, so an unknown field is refused with its path.
3. Link the capability into the hosts that should carry it (engine-view, engine-host), guarded by `if(TARGET engine::<name>)`. Nothing in `renderer` or `world` changes: a scene names it in `placements` (`{"generator": "<name>", "params": {...}}`) or as its terrain's `provider`.
4. Test the generator through the registry from the capability's own tests (a scene is not needed: make a `Context`, `open`, `expand`, `tile`), and that `tile` over every tile is `expand`.

## Invariants

- One descriptor per name per kind; the lists are sorted by name when read (`ground_names`, `placement_names`), so a sentence and a summary are the same whatever order the registrars ran in.
- A generator is a function of its entry, the world seed, the tile and the ground it is handed; its output on any number of threads is the same.
- A ground's `grid` is its `height` at the lattice's points, to the bit; `evaluate` at its own time is its `grid`.
- A placement generator's `tile` over a set of tiles is its `expand` restricted to them.
- No module that reads a scene or streams a tile depends on a generator's capability. `engine_module()` refuses it in every configuration — a module that is not a capability listing one, or a capability listing another it has not declared with `engine_capability_requires` ([08 §8.5](../plan/08-toolchain.md#85-build-system-and-ci)) — so `modules.json` cannot show `renderer -> ruins`, `renderer -> terrain`, `world -> ruins` or `world -> terrain` again; only hosts (`engine_app`) and test targets link generators.

## Public API

`include/domain/scene_gen/terrain_features.h`: `TerrainFeatures`, `terrain_features`, `ridge_weight`, `basin_weight` — the scene's ridges and basins.

`include/domain/scene_gen/scene_gen.h`: `TileCoord`, `Ground`, `Context`, `PlacementMesh`, `Placement`, `Placements`, `PlacementGeneratorDesc`, `Lattice` (`scene_lattice`, `ring_lattice`, `window_blocks`, `k_height_block`), the rings' types (`RingSpec`, `RingPlace`, `RingsLayout`, `RingChunkRef`, `RingHeights`, `GroundRingsOps`, `GroundRings`), the tiles' (`TileRecords`, `GroundTilesOps`, `GroundTiles`), `GroundOps`, `GroundProvider`, `GroundProviderDesc` and its flags, `GeneratorRegistry`, `Registrar`.

`include/domain/scene_gen/sky.h`: `SkyState`, `Atmosphere`, `Star`, `SkyOps`, `SkyProvider`, `SkyProviderDesc`, `k_default_sky`, `sky_provider_name`.

## Depends on

`base`, `containers`, `math`, `hash`, `json` (an entry's parameters), `schema` and `schemas` (the scene's entries as parsed: `engine.scene.Terrain`, `engine.scene.WorldRings`), `jobs` (the pool a generator is offered), `log`, and `geometry`: a ground's ring chunk is a cluster LOD DAG, so the rings' table names `geometry::ClusterLodMesh`. Nothing above the domain layer, and no capability.

## Testing

`tools/dev.ps1 test -Filter scene_gen`: `tests/scene_gen_tests.cpp` registers a ground and a placement generator from its own source and finds them by name; holds the unknown-name sentence word for word; refuses a second descriptor under one name and takes the same one twice; and samples a ground with no grid of its own on a lattice, the same heights as its point function. A still sky of its own (`scene-gen-test-still`) is found by name, listed, refused under a second descriptor, made from an entry, moved from handle to handle, and an entry naming no provider names "earth". `tests/size_table.cpp` pins `Placement` (48 bytes), `TileCoord` and `RingPlace`.

The consumers' side is tested where they are, with generators registered from the tests' own sources, so it runs in every configuration — `msvc-minimal` and `linux-clang-minimal` included, where no generating capability is built: the renderer's `scene_gen_scene_tests.cpp` reads scenes naming a test ground and a test placement generator (instances of the generator's meshes standing on the ground's floor; the ground made before any entry is opened and the entries expanded in the file's order, swapped when the file swaps them; a streamed read opening and expanding nothing; both unknown-name sentences and a generator's own refusal after where the entry is), and the world's `scene_order_tests.cpp` streams a tile of such a scene through the ground and placements consumers ([world](world.md#the-consumers)). **The proofs** ([ADR-0046](../adr/0046-scene-generators-register-themselves.md) decision 5), as of 2026-09-27: `msvc-minimal` and `linux-clang-minimal`, every generator's capability off and `scene_gen` still built, build and pass (57 of 57 tests each), the renderer's registry tests among them; `msvc-no-ecs` builds and passes (76 of 76); `modules.json` in the all-on build shows the renderer and the world depending on `scene_gen` and on no generator, and a configure that adds `ruins` to the renderer's dependencies, or drops the world's declared `store` edge, stops with the sentence `engine_module()` now says.

Each placement generator is tested through the registry in its own capability (`domain/ruins` and `domain/city`, `tests/scene_generator_tests.cpp`: `tile` over the entry's tiles is `expand`, and the refusals); the dunes through the renderer's `terrain_generator_tests.cpp` (a terrain naming its provider, the enum reading as one, sampler, provider and mesh to the bit) and the world's `ground_tiles_tests.cpp`, each linking the terrain capability as a host does.

## Performance notes

A lookup by name is a walk of a handful of descriptors, once per scene read or world made. A generator's per-point height is a call through a pointer, which is why every grid is one call a block: the mesh's grid, the time-lapse's 64 × 64 blocks, a ring's chunk. Nothing here runs per frame.
