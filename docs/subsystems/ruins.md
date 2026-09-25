# ruins (domain, capability)

**Purpose.** The ruin assembler of [07 §7.6](../plan/07-content-pipeline.md#76-procedural-generation-volume-content)'s direction note of 2026-09-24 — *ruined desert structures are assembled from the kit, never one asset*. From a **kit** (wall members described as data beside their meshes), a **world seed**, a **tile coordinate** and a **height query**, it makes one ruined building as **instances of the kit's meshes**: a footprint of walls at right angles, a corner member at every join, openings with their lintels on the walls still standing, a ruin state per wall, the debris the fallen stones make, and the sand drift each wall asks the terrain for. It builds no geometry, touches no GPU, and does not build the sand. Two callers use it today: the renderer's scene reader, for a scene's `ruins` entry ([renderer](renderer.md#scenes-camera-paths-and-flythroughs)), and `engine-content ruins`, which writes one tile's building as a scene fragment ([apps](apps.md#engine-content-ruins-a-tiles-ruined-building-as-a-scene-fragment)). An optional capability ([ADR-0027](../adr/0027-additive-capabilities.md)): `ENGINE_WITH_RUINS=OFF`, or a `*-minimal` preset, leaves it out, and then the scene reader and `engine-content` refuse ruins with a sentence.

**Why this shape.**

- **Pure, and integer in every decision.** A building is a function of (kit, world seed, tile, wind) and of nothing else — no clock, no thread, no global — because the endless desert materializes and drops tiles as the player moves, and a tile dropped and materialized again must be the same ruin; because a shared seed has to give the same desert on every player's machine ([13 §13.1](../plan/13-reference-consumer-games.md), "Generation is deterministic from a seed"); and because the derived-node form (below) is content-addressed like every other derived output. So the seed mixing is the engine's `hash_combine` over (world seed, tile x, tile z) — never `std::hash`, whose value is the standard library's choice — every draw is `hash_combine(hash_combine(building seed, purpose), index)`, so a draw added for one purpose moves no other, and every choice the grammar and the ruin rule make is integer arithmetic on **centimetres and Q10 fractions**, converted from the kit's metres once, when the kit is read. The only floats are the kit's numbers as parsed (`from_chars`, correctly rounded everywhere), the height the terrain query returns, and the metres an instance is written in; no decision reads any of them. Turning a building to one of sixteen yaws is a Q14 table and a rounding shift, not `sin` and `cos`, so the centimetres a debris block is tested in are the same on every C library. That is the content build's rule ([geometry](geometry.md#the-same-bytes-from-every-toolchain), ADR-0035) applied to generation, and it is pinned the same way: a golden hash in `apps/engine_content/tests/determinism_tests.cpp`, taken on MSVC and checked by GCC and Clang.
- **Corners are kit members.** Two walls that meet by overlapping their end blocks show the overlap: a doubled course, a block through a block, two ruin lines that disagree. The kit's generator already lays a corner properly — E33's L-corner interlocks its quoins course by course ([E33](../experiments/e33-hard-surface-kits.md#members-for-a-building-assembler-the-corner-the-doorway-and-the-footprint)) — so the assembler puts that member at every join and fills only the wall **between** the corners with sections. A section never has to know it is at a corner, and the joins are hidden where the kit hides them. A kit may also carry an **inside** corner for a reflex join (an L's or a U's notch, a courtyard's inner wall); without one the outside corner is turned half round, which stands in geometrically (its two arms swap) at the cost of facing its weathered side inward.
- **Sand is emitted as a declaration, not built.** The sand against a wall is the deformable surface's ([05 §5.13](../plan/05-simulation.md#513-deformable-surfaces-and-soft-bodies)): it is terrain, it drifts with the wind, it buries a doorway between two visits ([13 §13.1](../plan/13-reference-consumer-games.md), "Structure and sand interaction"), and it is persistent state. A skirt baked into a wall mesh cannot do any of that, and a skirt built here would be a second terrain with no owner. So each wall face emits a `Drift` — the face's ends, its outward normal, a height against it (higher on the windward face) and a reach — for the deformation layer to take when it exists. E33's members have a sand skirt baked in; the kit's far future is members without it.
- **Lengths come from sockets, not meshes.** A ruined member's mesh reaches past its joins — E33's 2 m section is 5.1 m of mesh with its rubble and sand — so the kit says where a neighbour joins as a pair of **sockets** on the wall's centre line, and which side is outside. The loader checks each pair is the shape its kind claims (a straight member's run along +x with the outside at +z; a corner's incoming wall arriving along +z) and derives the lengths from them.
- **The footprint is on the centre line, in whole modules.** Every straight member is a whole number of the kit's **module** long, and every wall's fill between its corners is a whole number of modules that the kit's sections can make (an unbounded knapsack over the section lengths, computed once per kit). On the centre line an outside and an inside corner consume their arms symmetrically, so a footprint closes by integer arithmetic; on the outer face the wall's thickness would enter every reflex join. A shape whose closing walls come out a fraction of a module — E33's corner, whose arms are 2.68 m and 3.68 m on the centre line, closes no L or U in a 2 m module — falls back to a rectangle, deterministically.

**Owned data.** A `Kit` (the file converted: members in centimetres, rules in Q10, members by kind, the fillable table, the corners' arms, the margin). An `Assembler`'s scratch (walls, losses, candidates), reused across buildings. The `Output` a call appends to: `Instance`s, `Drift`s and `Site`s. Nothing is global.

## The kit

`engine.scene.RuinKit` (`schemas/scene.schema`), JSON beside the meshes. The real kits live outside the repository with their meshes; the tests make a kit of boxes (below).

| field | what |
|---|---|
| `format`, `name`, `profile` | `"engine.ruin-kit.v1"`; the profile is the generator's parameter set (`ashlar`, `brick`, `boxes`) |
| `module` | every straight member is a whole number of these, metres |
| `thickness`, `wall_height` | the wall's, metres; heights and the ruin rule's fractions are of `wall_height` |
| `course_height`, `block_length` | the profile's, carried for a block-by-block representation and a far tier; not read |
| `members[]` | `name`, `kind` (`Section`, `Corner`, `Doorway`, `Window`, `Debris`, `InsideCorner`), `mesh` (relative to the kit), optional `hash`, `offset` and `yaw_deg` (a multiple of 22.5) bringing the mesh into the member's frame, `height` (its top line), `sockets[]` (position, direction of travel, outside, on the centre line at ground level), an opening's `opening_start`/`opening_end`/`sill`/`head`, a debris piece's `radius`, a `weight` |
| `rules` | the grammar's and the ruin rule's parameters (below) |

**The member's frame.** The wall's first socket at the origin, the wall running along +x, the building's inside toward −z (the footprint is walked with its inside on the left, seen from above). A section's sockets are at the origin and at (length, 0, 0); an outside corner's at (0, 0, −arm_in), where the incoming wall arrives travelling +z, and at (arm_out, 0, 0); an inside corner's at (0, 0, +arm_in), arriving travelling −z. Members of one kind and length that differ only in `height` are the ruin rule's **height variants**.

## The grammar and the ruin rule

For one building, in order, each step from its own draws:

1. **Footprint.** A shape by weight — rectangle, L (a rectangle with a corner cut away: one inside corner), U (a notch into the back: two), courtyard (an outer rectangle and a court's own wall inside it, whose four corners all turn right) — and a fill per free wall in `[min_fill, max_fill]` modules, the nearest fillable one. The closing walls follow from the others. A footprint is kept when every wall's fill is a whole, fillable number of modules, the rings close, no two walls overlap (neighbours in a ring share only their corner square), and its bounding circle **with the kit's margin** (the farthest debris and drift reach past the centre line) fits the tile at any yaw. Otherwise the fills' range is halved and tried again, and after eight tries the rectangle, which always closes.
2. **Placement.** A yaw of `16 / yaw_steps` sixteenths of a turn, the footprint's centre put in the tile with whatever slack the circle leaves, and one **base height** for the whole building: the lowest ground under any vertex or module boundary of any wall, on the centimetre grid, less `embed`. The walls meet at one level and nothing floats; the uphill side is buried, which is what sand does.
3. **Each wall's state.** Its outward normal is compared with the wind (sixteenths again): within three of it is **windward**, four side-on, farther **lee**; a court's wall is lee. A draw against `collapse` (+`windward` if windward) makes it **collapsed**, against `breach` (+half) **breached**, else **intact**; a court's wall halves both. A collapsed wall stands to a level in `[collapse_min, collapse_max]` (lower by half `windward` on the windward side), broken by up to a tenth per module so the top line is not a ruler; a breached wall has a gap `breach_min`–`breach_max` wide somewhere in its fill and climbs back to full height over two modules either side. **Exposed corners collapse more**: on the outer ring, a wall that is coming down loses up to `corner_drop` more near an outside corner, falling off over `corner_reach`.
4. **Corners, openings, sections.** Every wall starts with the corner at its start vertex — the outside corner where the ring turns left, the inside corner (or the outside one turned half round) where it turns right — at the height the lower of its two walls asks for, never below `min_height`. A wall that is not collapsed gets an opening with chance `opening`: a doorway or window by weight (doors twice as likely on a lee wall, where the drift is lower), at a place where the runs either side are fillable and the wall stands the member's full height over every module of it — **never on a run that has come down**. The runs are filled with sections: any lengths on an intact wall, the shortest on a wall that has come down, so its broken top has the resolution of one section. Each section takes the height its stretch of wall asks for (the lowest of its ends and middle): the tallest variant not taller than that, or, when every variant is taller, the shortest one **sunk** into the ground by the difference, at most `max_sink` of its height. A section asked for less than `min_height` is left out: a gap.
5. **Debris**, after every wall is decided and from its own draws: each corner, section and gap drops `debris_per_module` pieces per module of wall fully brought down, in proportion to what it lost, beside the wall (`debris_outward` of them outside), up to `debris_spread` past its face. A piece that lands in any wall is pushed across that wall to its nearer face, and one that cannot be put clear, or would leave the tile, is not placed. It lies on the ground where it lands — its own height query, not the building's base.
6. **Drifts and the site.** One `Drift` per wall face, `drift_windward`, the mean, or `drift_lee` high by the face's facing, reaching `drift_reach`; and the `Site`.

## Invariants

Each is a test in `tests/ruins_tests.cpp` unless it says otherwise.

- One (kit, world seed, tile, wind) is one building, bit for bit: assembled twice, alone or among three hundred, and on one thread or on a pool of one worker or five beside the caller. `assemble_tiles` gives each job a contiguous run of tiles and joins the runs in tile order.
- The same holds on every toolchain: `engine-content ruins` over 64 buildings hashes to the number pinned in `apps/engine_content/tests/determinism_tests.cpp` (`0x3667b0bbaa0d803c`, 3,272 instances), on one thread and on three — taken on MSVC 14.51 and reproduced by Clang 18 at `linux-clang-debug` and GCC 13 at `linux-gcc-release` in the Linux container on 2026-09-24.
- Every join has its corner: exactly one corner instance per wall, at the wall's start vertex.
- No two walls overlap; every ring closes; every wall's fill is whole modules and its pieces lie inside it without overlapping each other.
- The footprint, its debris and its drift fit the tile.
- Openings are never on a collapsed wall, and the wall stands intact over them.
- Debris lies on the terrain where it lands and outside every wall's footprint by its radius; nothing that stands floats.
- The windward walls come down more than the lee ones (49% against 30% of walls collapsed over 600 buildings with the default rules).
- The far tier (`Detail::walls`) keeps every wall piece identical and drops only the debris.
- A kit that breaks the module, lies in its sockets, turns a member by other than a sixteenth, has no corner, or asks for impossible rules is refused with a sentence naming the member.

## Where it runs

- **The scene reader** (`renderer::read_scene_file`), for a scene's `ruins` entry (`engine.scene.RuinScatter`): the kit read once per file, its meshes appended after the file's own (so the file's indices keep meaning what they meant, and the terrain stays last), the buildings on the `count` tiles of the square the world seed ranks first — or on every tile its `density` admits, the endless desert's rule — assembled on the terrain's height and handed to the renderer as plain instances. `engine-view --scene` and `render.load` get it with no flag. The renderer links this capability only where it is configured in, the way engine-view links `animation`; the scene reader has no registration point for placement generators, and one generator does not make one worth designing — a second (a building grammar with interiors, a road network) would, and then the link is replaced by a scene-generator registration point ([ADR-0037](../adr/0037-scene-reader-links-ruins-where-configured.md), accepted as that interim; it records the alternatives).
- **`engine-content ruins`**, the derived-node form for an authored place: one tile's building (or a region's) written as a **scene fragment** — an `engine.scene.Scene` of the kit's meshes, one instance per piece carrying its `RuinTag`, the building in `ruin_sites` and its walls' sand in `sand_drifts` — which `engine-view --scene` draws like any scene. It stands on flat ground at `--ground`; a terrain query needs the renderer, which the content build does not link. `ruins-kit` writes the synthetic kit.
- **Tile materialization** for the endless desert is the same call, `Assembler::assemble(placement, tile, out)`, one tile at a time into an output whose arrays stop growing — the world streaming system that calls it does not exist yet.

What happens to a ruin afterwards — a wall brought down, a doorway buried — is the persistent store's ([03 §3.5](../plan/03-data-model.md#35-persistent-world-state)); this module is only the ruin's initial state.

## The synthetic kit

`make_synthetic_kit` / `write_synthetic_kit` (`synthetic_kit.h`): every member a GLB of axis-aligned boxes already in its own frame, so nothing binary is committed and the tests, the end-to-end test and the measurements make it where they need it. By default a 2 m module, a 0.6 m wall 2.4 m high, sections of 1 and 5 modules (E33's 2 m and 10 m runs) at three heights (100%, 55%, 30%), outside corners with 2 m arms at the same three heights, a doorway and a window one module wide, and two debris blocks: 13 members. `e33_sized_options()` is the same with E33's sizes: a 0.64 m wall, the ashlar L-corner's arms (2.68 m and 3.68 m on the centre line), and no openings.

## The E33 kit as a kit

The ashlar members under `game_engine_local\blender-kits\ruined-wall\out\` assemble, described by `ashlar-kit.json` beside them (the 2 m and 10 m sections, the L-corner and the doorway; module 2 m, walls 0.64 m thick and 2.8 m high); `game_engine_local\ruins\` holds a ruin assembled from it — world seed 6, tile (0, 0): a courtyard of 32 pieces with two doorways — as a scene fragment and as a scene over a terrain, and its captures. None of it is committed. Three findings are for the kit generator, and one was the assembler's:

- **The doorway is 3.2 m**, which is not a whole number of the 2 m module the 2 m and 10 m sections set, so no wall can take it and close. The real-kit description declares it a 4 m slot with the 3.2 m mesh centred in it; the generator should export openings at a whole number of modules.
- **The corner's arms (4 m and 3 m on the outer face, 3.68 m and 2.68 m on the centre line) close only rectangles and courtyards** in a 2 m module. Arms of a whole number of modules each (or an inside corner whose arms cancel the outside one's) would let the L and U through.
- **Every member was exported as a free-standing wall**: eroded free ends, and its own rubble and sand skirt. Assembled, each join shows two broken ends and two skirts overlap. For assembly the generator's footprint mode wants joined ends (`free_start`/`free_end` false), no skirt (the drift is the terrain's), and the debris exported as members of its own.
- **Its doorway is 3.0 m high in a kit whose walls are 2.8 m.** The assembler asked a wall to stand an opening member's full height over it, which no wall of that kit can, so no doorway was ever placed until the requirement was capped at the intact wall. The synthetic kit could not have found it: its members are exactly the wall's height.

## Public API

- `domain/ruins/kit.h` — `Kit`, `Member`, `Rules`, `PieceKind`, `piece_kind_name`, `kit_from_schema`, `read_kit_file`, `to_cm`, `to_q`, `k_q_one`, `k_piece_kinds`.
- `domain/ruins/assembler.h` — `TileCoord`, `Ground`, `HeightFn`, `Shape`, `shape_name`, `WallState`, `Detail`, `detail_for_distance`, `Placement`, `Instance`, `Drift`, `Site`, `Output`, `Assembler` (and its `Side`), `building_seed`, `tile_rank`, `tile_has_building`, `choose_tiles`, `assemble_tiles`, `hash_output`, `yaw_step_from_degrees`, `step_cos`, `step_sin`, `instance_translation`, `instance_yaw_step`.
- `domain/ruins/fragment.h` — `make_fragment`, `write_fragment`.
- `domain/ruins/synthetic_kit.h` — `SyntheticKitOptions`, `e33_sized_options`, `SyntheticKit`, `make_synthetic_kit`, `write_synthetic_kit`.
- `domain/ruins/ruins.h` — all of the above, the capability checklist, and `k_determinism`.

**Depends on.** `base`, `containers`, `math`, `hash`, `json`, `schema`, `schemas` (the kit and the fragment are `engine.scene` types), `io`, `log`, `jobs`. **Depended on by** `renderer`, where this capability is configured and nowhere else: the scene reader expands a scene's `ruins` entries through it. That link is **the one exception to [ADR-0027](../adr/0027-additive-capabilities.md)'s rule** that a module which is not a capability never depends on one, accepted as an interim by [ADR-0037](../adr/0037-scene-reader-links-ruins-where-configured.md), and it is replaced by a scene-generator registration point the moment a second generator exists — that is the ADR's revisit trigger. `engine-content` and engine-view's tests link it the way apps link any capability, when it is there.

## Testing

`tools/dev.ps1 test -Filter ruins` runs the unit tests (`tests/ruins_tests.cpp`, no device, no files: the kit of boxes is made in memory), the renderer's `ruins_scene_tests.cpp` (a scene with a terrain and a ruins entry read and loaded with no device, into a derived-data root in the test's scratch directory), `engine-content`'s `ruins_tests.cpp` and the ruins row of its `determinism_tests.cpp`, and engine-view's `ruins_view_tests.cpp`, which draws four buildings over the terrain offscreen, counts the pieces against the assembler's, and checks the picture differs from the bare terrain's in more than 2% of its pixels (it skips with exit 3 on a machine with no Vulkan device). The grammar's properties run over 250 buildings for each of four kits: the default, one with an inside corner, one with a 1 m module and no one-module section (so the fills are searched), and the E33 sizes.

## Performance notes

Measured 2026-09-24 on the development machine (Intel Core i9-10980XE, 36 logical CPUs, 64 GB; RTX 5090), `msvc-release`.

**The assembler, on the CPU** (`bench/ruins_bench.cpp`, seven repeats after `--wait-quiet` held off for 300 s while another agent's suite ran; the header's `machine_state`: 7–10% of the CPU in other processes, the GPU idle with 6.6 GB of 32.6 GB in use). The synthetic kit on flat ground:

| what | median | min |
|---|---|---|
| one building, the output cleared between them (`ruins.assemble.building`) | 6.65 µs | 6.61 µs |
| 1,000 buildings into one output, one thread (`ruins.assemble.1000/0`) | 6.66 ms | 6.65 ms |
| the same on the pool, 4 workers and the caller (`/4`) | 3.08 ms | 2.98 ms |
| the same, 8 workers and the caller (`/8`) | 1.98 ms | 1.97 ms |

A building allocates nothing once its output's arrays have grown to it: its walls, losses and candidates are the `Assembler`'s scratch, reused. **Read from a scene over a real terrain it cost seven times that**, because `renderer::terrain_height` drew its dune field again on every call and a building asks the ground some forty times (every module boundary of every wall, and every debris block). The scene read now holds one `renderer::TerrainSampler` for the whole read, whose heights are the direct function's bit for bit (the renderer's flythrough test compares them on a grid, and the buildings hash to the same `f85b2f0d8e2e1665` either way). The desert-overlook terrain's 1,000 buildings, `ruins assembled` over seven `engine-view --offscreen` reads each, `msvc-release`, 2026-09-24, the CPU 10–53% busy with other agents' builds:

| scene read of 1,000 ruins over the desert overlook | median | range |
|---|---|---|
| the dune field drawn per call (`terrain_height`) | 43.9 ms | 43.4–58.6 ms |
| one `TerrainSampler` for the read | 23.9 ms | 23.3–27.5 ms |

What is left is the height itself (six waves of `sin` each, three ridges and a basin) and the assembly: the same buildings on flat ground cost 6.7 ms.

**Instances per building** (`engine-content ruins … --region -16,-16,31,31 --count 1000`, world seed 2026, 32 m tiles; the far tier is `--walls`):

| kit | pieces per building, mean (min–max) | far tier | of which debris | corners | sections | doorways / windows | shapes (rect / L / U / court) |
|---|---|---|---|---|---|---|---|
| synthetic, default | 50.6 (10–126) | 17.1 (4–47) | 33.5 | 5.1 | 10.8 | 0.72 / 0.48 | 586 / 278 / 27 / 109 |
| synthetic, E33's member sizes | 48.4 (10–113) | 14.1 (4–35) | 34.3 | 4.4 | 9.7 | — | 891 / 0 / 0 / 109 |
| E33's ashlar kit as described (no debris members) | 15.0 (4–41) | 15.0 | — | 4.9 | 9.1 | 0.94 / — | 768 / 0 / 0 / 232 |

Debris is two thirds of a building's pieces with the default rules, which is why the far tier drops it.

**The cull pass, on the GPU** (engine-view offscreen, 1920 × 1080, the desert-overlook terrain — 2,049² vertices, 187,660 clusters — and its camera path resampled to 600 frames, three repeats, occlusion on, shadows off; under the GPU lock, `ENGINE_GPU_LOCK_OWNER=agent-ruins`, released between groups; the summaries' `machine_state`: 15–50% of the CPU in other processes — a Linux container build of this branch ran beside it — so flagged not quiet, and the GPU ours. A run of the same three before the branch was rebased onto `0f95024`, at 6–20% of the CPU in other processes, agreed within 1% in every column.) Every run drew the same visible pairs in every repeat.

| scene | pieces | pairs | visible pairs, median (p95; max) | cull ms, median (p99) | raster (hw) ms | all passes ms, median | GPU memory |
|---|---|---|---|---|---|---|---|
| the terrain alone | — | 187,660 | 804 (1,244; 1,339) | 0.026 (0.028) | 0.019 | 0.113 | 438 MiB |
| + 1,000 ruins of the synthetic kit | 49,973 | 240,109 | 4,056 (9,774; 20,020) | 0.030 (0.032) | 0.024 | 0.128 | 438 MiB |
| + 100 ruins of the E33 ashlar kit | 1,564 | 8,760,850 | 5,405 (17,257; 24,246) | 0.202 (0.225) | 0.027 | 0.308 | 1,225 MiB |

**What it says.** The cull pass is one thread per (instance, cluster) pair over **every level of every member's DAG** ([renderer](renderer.md)), so its cost follows pairs, and a ruin's pairs are its pieces times each member's clusters: a box is one cluster, an E33 member 2,335 (the 2 m section) to 13,163 (the 10 m wall). A thousand ruins of boxes add 52,000 pairs and four thousandths of a millisecond; a hundred ruins of the E33 kit add 8.6 million pairs and 0.18 ms — about 0.02 ms per million pairs — and 787 MB of GPU memory, of which the four meshes and their textures are a few hundred and the rest grows with the pairs. **A thousand E33 ruins would be about 90 million pairs** (extrapolated, not run): some 1.8 ms of cull a frame before a triangle is drawn, and gigabytes of per-pair buffers. So the kit-section representation is cheap in instances and expensive in pairs at E33's density (the ashlar 10 m wall is 526,000 triangles, about 19,000 a square metre of wall face, [E33](../experiments/e33-hard-surface-kits.md#what-it-decides)), and the far tier E33 asked for is a *pair* budget as much as a triangle one. The block-by-block representation is the other side of the trade: a 10 m wall of 267 block instances of a dozen block meshes of a few clusters each is some hundreds of pairs against the section's 13,163, but hundreds of instances, and at distance every block still draws its coarsest cluster where the section draws one. That is the comparison [07 §7.6](../plan/07-content-pipeline.md#76-procedural-generation-volume-content) asks for, and these three rows are its first numbers.

## Capability contract (ADR-0027)

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | none | stands alone |
| Component and event types | `engine.scene.RuinKit`, `RuinMember`, `RuinSocket`, `RuinRules`, `RuinScatter`, `RuinTag`, `RuinSite`, `SandDrift` in `schemas/scene.schema`, beside the `Scatter` they sit with | done |
| Tick scheduler entry | none: nothing ticks; a building is a function of its seed and tile | not needed |
| Render-graph passes | none: the output is instances the renderer already draws | not needed |
| Content-build derived step | `engine-content ruins`: a scene fragment for an authored place | done, as a command; a manifest step with the footprint and seed as document parameters is next |
| Protocol methods | none yet | not needed |
| Tunables | none: every parameter is the kit's, and the kit is content | not needed |
| LOD policy | `detail_for_distance`: past eight tiles a tile keeps its walls and drops its debris; below that, each member's own cluster LOD | done |
| Determinism | `ruins::k_determinism` = `derived`, and bit for bit on every toolchain | done |
| Zero cost when unused | a scene without `ruins` never calls the module; switched off, it is not linked | done |
| Tests and size table | `tests/ruins_tests.cpp`, `tests/size_table.cpp` (`Instance` 28 bytes, `Drift` 40, `Site` 48) | done |
| Bench | `bench/ruins_bench.cpp`: `ruins.assemble.building`, `ruins.assemble.1000` | done |
| Removal proof | `ENGINE_WITH_RUINS`, off in the minimal build | works |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_RUINS=OFF`) drops the module, its tests, and its bench; it disappears from `build/<preset>/modules.json` and is listed there under `disabled_capabilities`, the renderer builds without it and refuses a scene with `ruins`, and `engine-content ruins` says the build has no ruins capability.
