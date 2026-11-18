# scene_collision (systems, capability)

**Purpose.** Static collision bodies from the scene, kept round a walker: the **ground** as a heightfield a tile, sampled from the scene's ground provider (`scene_gen::GroundProvider`: the renderer's waves, the terrain capability's dunes) at a collision spacing, and the **placements** — what the scene's placement generators put on the tile, the ruins' walls, corners and rubble — as one static compound a tile of each piece's proxy, in a `physics::World`, for a `physics::CharacterBody` ([physics](physics.md#the-character)) to walk on. It is a world consumer ([world](world.md#the-consumers)): the bodies of a tile exist while the walker's tile ring holds the tile, and are made and let go as the ring moves, within the ring's budget. It builds nothing the renderer draws and links no generator — each is found in the scene-generator registry by the name the scene gives ([scene_gen](scene_gen.md)); what it holds can be drawn over the picture for a look ([Seeing the collision](#seeing-the-collision)). Its first host is engine-view's walk mode ([apps](apps.md#walking)); a game's player, and later an NPC at LOD0/1, walk on the same bodies.

## Why this shape

- **A consumer of its own ring, stepped at the walker's ticks.** A character's contacts are sorted by body id ([physics](physics.md#the-character)), and body ids come out of the physics world's free list in creation order, so *when* a body is made decides which id it gets and so, bit for bit, where a walker that touches it ends up. A consumer registered in a host's drawing ring — engine-view's `--world` ring, updated once a frame from the camera — would make bodies at frame boundaries, which a live window and its offscreen replay place at different ticks. So the walker's host keeps a ring of its own, one ring of `walk.collision.radius_tiles` round the walker's feet, and updates it once a tick, before the step (`view::Walker`); the same session and its replay make and let go of the same bodies at the same ticks, and a recorded walk replays bit for bit. The ring is the world capability's own (`world::World`), with its hysteresis, its budget and its tile order: the rule for which tiles are held is the one every other consumer is held to, not a second one.
- **Registered after the ground and the placements.** In a world that also holds the scene's ground and placements consumers ([world](world.md#the-consumers), "The order"), this row goes after them: a tile's collision is made from what is on the tile, after it is there, and let go before it goes. engine-view's walker keeps a ring of its own (above), where it is the only row.
- **Where the placements come from.** A **streamed** scene (`renderer::SceneData::streamed`, `--world` or a scene's `world` block) keeps its placement entries for the world, and this consumer opens each entry's generator again and asks it for the tile (`scene_gen::PlacementGeneratorDesc::tile`) in the representation the scene's **inner ring** draws — what is drawn round the walker: the ruins' blocks where the scene lays blocks near, their sections otherwise. A scene **read whole** has its placements among its instances already, so a tile's placements are the instances whose bounds reach into it — the generator's `tile` is the whole read restricted to the tile, so this is the same set, and it also gives the scene's own meshes a body (an instance counted by every tile it reaches, so a wall across a tile edge is whole from either side). The terrain's instance is the ground's, and a skinned instance moves, so neither gets one.
- **One compound a tile, one proxy a mesh.** A ruin laid in blocks is several hundred pieces; a body each would put hundreds of bodies a tile in the broadphase. A static compound (`physics::World::create_compound`) is one body a tile with its own tree over its pieces, and its pieces are shared shapes: **one proxy per mesh and scale**, however many instances.
- **The proxy is the mesh's own LOD DAG cut at 5 cm, never the full-detail mesh.** The choices the brief named were the member's own coarse level, its convex hull, or its boxes. A hull or boxes fill a doorway and a window — a walker could not pass through the ruins' own openings, and a fallen wall's broken top would be a sheer box — so the proxy is the member's **coarse level**: the cut of its cluster DAG where each cluster's own error is within `walk.collision.mesh_error_m` (5 cm) and its parent's is not — `geometry::lod_selects`' rule with the threshold in metres instead of pixels — the coarsest surface that stays that close to the full one, welded by source vertex so the backend sees shared edges (and finds its internal ones), as a static triangle mesh. For a member of the kit of boxes, or a block of the synthetic block kit, that is the mesh itself (a box's 12 triangles, a low block's 44); for a 3 m test ball of 8,192 triangles it is 510, whose surface stands at most 4.2 cm off the leaves' (the test below); a scanned member of thousands of clusters is cut to its coarse clusters and never collides as its leaves. The error is in the mesh's own units divided by the instance's largest scale, so a scaled instance gets its own proxy at the right error.
- **The broadphase is rebuilt once an update that changed bodies** (`commit`, and after a refresh that rebuilt a tile). The backend's broadphase trees give back the nodes of the bodies taken out of them only when a tree is rebuilt, which `physics::World::step` does and a world like this one — static bodies and a character, never stepped — never would. So a walker's world ran its trees out of nodes after a few thousand tile changes: the ring-change bench (`scene_collision.ring_change`, three tiles in and three out an update) found it within seconds as Jolt's `QuadTree: Out of nodes!` and an assert, which in a Debug build on Windows is a modal abort box on the owner's screen that holds the process until somebody clicks it (2026-09-28). A rebuild of a handful of bodies — two a tile, nine tiles — costs microseconds. The long-walk test runs seven thousand bodies through a world of 256 with it, but did not fail without it (single bodies added into a root with a free slot allocate no node); the bench, whose updates add six bodies at once, is the case that does, which is a reason it is run.
- **The ground is a heightfield on the world's lattice.** A tile's heightfield covers the lattice points from the one at or west of its west edge to the one at or east of its east edge — 34 × 34 at the default 1 m on 32 m tiles, because the backend stores heights in 2 × 2 blocks and wants an even count — on the world's grid at the spacing (`scene_gen::ring_lattice`), so two neighbouring tiles share their edge samples bit for bit and the seam is closed. The heights are the provider's `grid` at its own time: exactly its `height` at those points. The backend quantizes them to 8 bits within each 2 × 2 block's range, half a millimetre at most measured on the test's saddle.

## The ground moves

**Still authored terrain uses the same native source by default** (2026-10-23, W67). `GroundProvider::tiles()` preserves the set's filter and data refusals through the registry, so a collision consumer needs no typed tileset accessor. The authored bowl/walk test now exercises this default path; lattice heights remain the source's to the bit, and missing tile requests fail instead of creating zero-height bodies. Its source has no time and remains static alongside the streamed renderer layout.

Under a time-lapse ([renderer](renderer.md#the-dunes-in-time-lapse)) the drawn sand is not the ground's function at one time: each terrain level draws **two fields and a blend**, `a (1 − t) + b t`, the pair timed by how far the fastest band travels and crossed within a per-frame bound. The collision follows **the drawn pair, not the true surface**, so the walker stands on the sand the player sees, cross-fade and all: the host hands the consumer the finest level's pair and blend once a frame (`GroundTime`, from `renderer::TerrainMotion::level_stats`), and every tile keeps the two fields on its own lattice and the heights its body was built with.

**What is walked on is what is drawn** (2026-09-30, [ADR-0050](../adr/0050-the-ground-is-drawn-from-the-worlds-tiles.md), proposed). The ground's heights come from a **tile source** (`scene_gen::TileSource`, [scene_gen](scene_gen.md#the-tile-source)): the ground provider seen as tiles (`GroundProvider::tiles()`, its own heights to the bit) unless `create` is handed another — the one the renderer draws the world's tiles from, where it does ([renderer](renderer.md#the-ground-from-the-worlds-tiles)). Then the heightfields are that source's heights on the collision's lattice, still or at the drawn pair's two times, and the pair handed over is the finest *tile* level's, so a tile some other source made — one built ahead of time and read back from disk — is the ground the walker stands on as well as the one drawn. The provider still stands the placements: a generator is handed its floor. engine-view hands its walker the renderer's source (`Walker::start`), and a walk over the erg drawn from the tiles is the same walk, to the walker's state chain, as over the grid, with the feet within 10 cm of the finest tile level as drawn (`apps/engine_view/tests/walk_view_tests.cpp`).

**The rule** (`refresh`, once a tick): a tile whose drawn pair changed has the new field evaluated — when the level handed over, its old b is its new a, the same heights to the bit, and only the new b is evaluated; and a tile whose body's heights stand off the drawn heights at any of its samples — the pair blended exactly as the renderer blends it — has its body rebuilt at the drawn blend: **the tiles under the walker** (the one holding its feet and any its capsule's radius reaches, which `refresh`'s `reach` names) once they stand more than **`k_underfoot_error_m` (1 mm)** off, at every update and outside the budget; **every other tile** once it stands more than **`walk.collision.ground_error_m` (5 cm)** off, at most `walk.collision.max_refreshes` (2) of them a tick, nearest the walker first. Both surfaces are the same piecewise-linear interpolation between samples on a lattice, so the largest difference is at a sample and the rule bounds it everywhere on the collision's lattice.

**Why a millimetre under the walker** (2026-09-29, [the experiment](../experiments/walk-in-time-lapse-2026-09-29.md)). Until then the tile under the walker kept to the 5 cm too, and the camera is the walker's eye: standing on the erg with the dunes at 600 game seconds a real second, the owner saw the ground "change smoothly for about a second, then jump in height", over and over. His frame records say the drawn sand never jumped — its largest move 0.2–1.3 mm a frame, no frame more than 1.8 times its neighbours' — and his eye stood still while the sand sank away under it, then dropped 4.65 cm in one frame when the tile was rebuilt, 45 times in 65 s, every 0.6–3.6 s. What stopped it after about 20,000 ticks was the wind, not the rule: the record's next two game days are calm, the renderer's next pair spanned 2.2 of them, the sand slowed about thirty-fold and a 5 cm step would have taken minutes. A millimetre is under a pixel at the feet on his 11520 × 2160 and does not step; the price is rebuilds of the walker's tile — 189 against 21 standing 20 s at 600 on the erg in the model — and it is a constant rather than a tunable because a smaller one buys nothing the eye can see and rebuilds the tile every frame.

## The walker goes with the ground

`follow(walker)`, which the walker's host calls in place of `refresh` once a tick, before the character's step: the refresh round the walker's feet, within its capsule's radius, and then the walker moved with what that did under it.

- **Standing on the ground, it is carried.** When the character stands (`physics::Ground::OnGround`) on a held tile's heightfield — the ray straight down from just above its feet, past the round bottom's rise on the steepest ground it stands on, meets a tile's ground body and not a placement — it is moved straight up or down by the change of the collision ground under its feet: it keeps the offset it stood at (a slope's rise under the capsule's round bottom, the backend's padding), so it neither hovers nor sinks, and its eye rises and falls with the drawn sand a frame at a time.
- **A walker is never below the collision ground after a refresh.** Wherever it is — in the air, on a placement the sand has buried, carried or not — feet the ground has risen past are lifted onto it, at the collision ground's height there, which is the ground provider's drawn pair blended on the collision's lattice.
- **Its velocity.** Both go through `physics::CharacterBody::teleport`, which leaves it standing where it is put with no velocity: the vertical speed of a fall or a jump the sand caught is dropped, as a landing drops it, and the horizontal speed is the input's, which every step sets anew — so a walker walking or sprinting up a rising dune keeps walking. Being carried is not climbing: engine-view's summary counts `climb_m` from where the sand left the walker.
- **On still ground `follow` is `refresh` and nothing else**: no carry, no lift, no teleport in the character's hash chain, so a recorded walk there replays bit for bit as before.

**Why the collision has to move the walker.** The backend's virtual character moves only by its own sweep ([physics](physics.md#the-character)), and a rebuilt heightfield is a new static body under it that nothing moves it with. Sand that dropped away left it standing on its old contact until the rebuild snapped it down, which was the jump; sand that rose closed over its feet, because a heightfield's contact is one-sided and pushes nothing back up through it — and the character's slope hold, which zeroes a standing character's velocity on a walkable contact, zeroes the backend's penetration recovery with it, so in the model the sand buried a standing walker 15 cm in a minute at 600 and 18 cm in ten seconds at 3,600, and at 8,640 and above went over the capsule's round bottom and it fell through, as the owner did at a week a second on 2026-09-29.

**What the tests hold** (`tests/time_lapse_tests.cpp`, on the committed erg's dunes, no device: the renderer's own cadence rule, `terrain_blend_frame` and fields that arrive on time, as an offscreen run waits for them, or timed as a window times them with the keep-up rule; a frame is four 240 Hz ticks, a host's `follow` before each step). **After every tick's refresh the tile under the walker is within a millimetre of the drawn heights at every sample** — 0.97 mm at a game day a real second over 600 frames, 52 fields and 1,495 rebuilds (4.96 cm under the 5 cm rule). Between two frames the drawn sand moves on and the collision is last frame's until the next tick catches it up, so the stalest a rebuild under the walker finds is the millimetre plus one frame's move of the drawn sand — which the renderer bounds at a quarter of its spacing a frame (0.375 m on the erg's 1.5 m grid) and which measured 13.0 cm at a day a second on the erg (the waves reshaping to a few days' wind, not their travel): 13.1 cm. A standing walker stays on the collision ground to within its capsule's slope offset and the backend's 2 cm padding: 0.07 mm at a day a second on the erg's floor (13.6 cm before it was carried). **Standing at the owner's start at 600** for 1,200 frames, its feet move at most 1.03 mm a frame against the sand's 0.14 and never jump (three jumps of 4.65 cm before). **On rising sand at every rung of the dune ladder** — 600, 3,600, 8,640, 86,400 and 604,800, each where a scan of the erg found the sand rising from the committed time, the pairs timed as a window times them — the walker is never under the collision ground after a refresh, never more than 0.8 mm under the drawn ground after a frame, and never jumps; before, it was buried 15 and 18 cm at the two slow rungs and fell through at the three fast ones, 0.9 m, 95 m and 132 m under. **At the game's own rate the sand is all but still**: ten minutes of real time at a game second a second moved it 8.2 mm at the walker's tile, which was rebuilt 8 times, a millimetre at a time, and nothing else was (none was before). `tests/scene_collision_tests.cpp` holds the rule at the renderer's own bound: a ground rising 0.375 m a frame for three game seconds and falling as fast for three carries a standing walker up 22.5 m and down again, never under the collision ground, within 0.9 mm of the drawn ground, its every frame's move within 0.5 µm of the ground's, and catches a jump from it (lifted 0.31 m) — before, the walker fell through on the first rising frame and ended 28 m under; and on still ground a walker `follow`ed and one only stepped hash the same. A still ground — no time-lapse, or a ground that does not move — is never refreshed and `refresh` returns at once.

Under a live window's time-lapse the pair and blend follow the renderer's clock, which the display drives; a walk over moving sand therefore replays against the sand the replay draws, and only a walk on still ground is guaranteed to replay bit for bit ([apps](apps.md#walking)). The rule for moving sand changed on 2026-09-29 without a new walk version (`view::k_walk_version`), because it changes nothing a still-ground walk does and a moving-sand walk never replayed bit for bit; a recording from before walks over the same sand differently.

## Far from the origin

**A tile 10,000 km out collides as one by the origin does** ([ADR-0053](../adr/0053-world-positions-are-f64-and-the-gpu-sees-none.md), 2026-10-05). The owner walked the endless desert 420 km out and the walk lost motion: the feet were float32, the ground's heightfield was offset by its tile's corner in float32 (a field 420 km out on a 3.1 cm grid), and the height under the feet was asked for at float32 feet, so `walk.max_ground_error_m` was one float step, 3.06 cm ([far from the origin](../experiments/far-from-origin-2026-10-04.md), Part 2). Now:

- **A tile's bodies stand at the tile in f64 and hold small numbers.** The ground's heightfield is a body at its lattice window's first point (`i0 * spacing`, a `WorldPos`) with the field in its own frame from there; the placements' compound is a body at the tile's corner with each piece placed from the corner. The physics world keeps both in double and collides in floats relative to the bodies ([physics](physics.md#far-from-the-origin)).
- **A height is asked for at a `WorldPos`** (`ground_height`, `refresh`): the tile and the cell are found from the point's millimetres in f64, and the fractions across the cell from the point's millimetres less the window's first point's, which are integers — a difference of two numbers within a tile of each other, so a lattice point lands on a whole number exactly, anywhere, and the answer there is the tile source's own float.
- **The window's index is i64 and refused past the source's i32.** The tile source's window is indexed in i32 (`scene_gen::TileSourceOps`), which at the collision's spacing s reaches 2³¹ s: 2.1e9 m at the default metre, 2.7e8 m at the tunable's finest 12.5 cm. A tile past it is refused with a log line rather than asked for the heights of a wrapped index. The pieces' bins and the walker's tile are keyed the same way (2026-11-15, roadmap F11): a piece whose reach passes the i32 tile range is binned on no tile, and a walker past it stands on none, where until then both cast their i64 tile index to i32 and named some other tile.
- **A scene read whole is read where its instances stand** (2026-10-05). A piece's place is the instance as the GPU holds it — its cell and the 3x4's translation column, the cell's local, summed in f64 (`gfx::instance_cell`, `to_world`) — and its rotation and scale the 3x4's linear part; a tile's bins are its bounds taken in the cell's frame and then to the world's millimetres from the cell's corner in f64. Nothing forms the float32 world matrix the renderer does not hold (`renderer::instance_world_matrix`, which this module read until then and which put a box 419 km out 1 mm off, 10,000 km out up to half a metre). `scene_collision_tests.cpp` holds it: a box's face 419 km, 10,000 km and 1e8 m out is met by a ray at the origin's distance to the bit.
- **A streamed placement is read where its generator put it** (2026-10-06). A placement generator's `scene_gen::Placement` carries a `WorldPos` ([scene_gen](scene_gen.md#placements-far-from-the-origin)); the mesh's fit is turned and scaled in float32 (the size of the mesh) and added to it in f64, which is where the renderer stands the same placement (`renderer::instance_translation`), and the compound holds the sum less the tile's corner. Until then the placement was a float32 world position widened in one helper, `placement_position`, marked as the last `ADR-0053 seam` on the way in, and it is gone. **What a prop scattered on a far tile lost to it**: its generator computed its place in float32 world coordinates and asked the ground at float32 metres, so it stood up to half a float's step from where the generator meant it — 1.6 cm at 419 km, half a metre at 10,000 km, 4 m at 1e8 m — on the ground's height at that rounded place, and two pieces meant to touch could open or close a gap of up to a whole step. The collision stood it exactly where the renderer drew it, so what was walked into was what was seen; what was lost was the generator's intent, and the fix was the generators' interface, not this module's. `scene_collision: a streamed placement collides where its generator put it, far out` holds it: a box 16.123456789 m into its site's tile — a place no float32 holds 419 km out — has its west face met by a ray at the origin's distance from the site to 2 µm at 419 km, 10,000 km and 1e8 m.

The far case (`scene_collision: the ground at 419 km, 10,000 km and 1e8 m is the tile source's`) builds the ring round each far site over a tile source whose heights repeat every 16 m, so every site stands on the origin's ground: on the window's 576 lattice points and 576 points between them `ground_height` is the origin's to the bit, a ray straight down onto the backend's field lands where it lands by the origin to the bit, and a character walked ten seconds across a tile's edge is within a millimetre of the origin's walk — measured 0 at 419 km, 1.4 µm at 10,000 km and 85 µm at 1e8 m, where f64 steps by 15 nm and the capsule's step-up and stick-to-floor decisions carry the rounding.

## Owned data

The tiles it holds (a heightfield body and shape, a compound body and shape, and the heights and fields a tile was built with), its copy of what the proxies are cut from when the host did not hand one ([The collision's own copy](#the-collisions-own-copy)), the proxies (one built shape per mesh and scale, `physics::BuiltShape`, kept for the consumer's life and held by every compound that places them, never in the world's table), the builds ahead (a tile's heights and its compound's built shape, until the tile takes them or the build is dropped), the open placement generators of a streamed scene, and a scene read whole's bins of instances by tile. The bodies and shapes are the physics world's; this consumer is the only thing that makes or destroys them.

**What gets no body.** The terrain's mesh (`SceneData::terrain_mesh`: the ground is the heightfield), a skinned instance (it moves), and since 2026-10-17 **a water body's mesh** — one whose `renderer::SceneMeshInfo::water` is set ([water](water.md#wading)): the walker stands on the ground under the surface and wades by the depth the water capability answers, so the surface is never something to stand on. Meshes are keyed by their index in the scene, and these are the marks read beside it.

**A tree collides by its wood** (2026-10-19, roadmap S66). A mesh that carries the motion contract (`renderer::SourceMesh::has_motion`: a generated tree's sections 35 to 37, [flora](flora.md#motion-attributes)) is a body like any other mesh, but its proxy keeps a triangle only when **all three corners are of a part that stays a collider** (`renderer::flora_part_collides`): only a catalogue `parts` species, and only the parts its structural table flags load-bearing or **collider**, including part zero (`geometry::k_structural_collider`: a shrub's stem at least 1.25 cm in radius at the ground, [flora](flora.md#the-shrub)); with no structural table, a known `parts` species retains the legacy trunk-zero rule (the palm and older broadleaf/conifer contracts). A present flagless table never makes part zero a collider. Catalogue `field` and `none` species contribute no triangles, even on part zero or with a structural flag; unknown motion species contribute none. This decision is made at proxy construction from the catalogue, independently of the motion tunable (G90, 2026-10-26). **The understory** (2026-10-21, roadmap G75): a fern, a grass tuft and a herb have **no body at all** — their part 0 is a root with no geometry and no other part is the trunk or flagged — and a shrub collides by its stout stems alone, never by its leaves, its twigs or a stem a walker bends aside (a dogwood's thumb-thick stems). **What a graphics preset may drop never collides**: a kind whose instances carry a body is never of the `rich` detail tier, and the flora reader refuses a recipe that says it is ([flora](flora.md#detail-tiers)), so leaving a tier out never changes what a walker meets. Fronds, leaves, curtains and twigs are left out — through each corner's part id in the motion vertex records, which are per cluster vertex and so cover every level the proxy is cut from — so the walker walks into a palm's crown, where the renderer's push bends the fronds away from its capsule ([renderer](renderer.md#the-flora-stage)), and is still stopped by the trunk. All three corners rather than one, because a triangle where a frond meets the trunk is the frond's to bend. The rule holds whether or not the trees sway (`renderer.flora.motion`), so a walk collides the same either way. `Stats::flora_triangles_left_out` counts what the proxies left out. Tested by `scene_collision: a tree's fronds are walked through and its trunk stops the walker`: a slab across the path, marked part 1, leaves the proxy, a walker going north passes through it and stops at the trunk's face less its radius.

**The far forest never collides** (2026-11-08, roadmap P62). A far-forest tile's mesh ([renderer](renderer.md#the-far-forest), ADR-0059) is every streamed tree's coarsest form on a world tile — the whole-tree impostor star where the flora container has one — merged into one rigid mesh and drawn as one instance of the load's prefix, and only where the ring's tail does not draw that tile's trees: never round a walker, whose tile the tail always holds. W88 bins the prefix's instances by the tiles their bounds reach, so from then on every far tile was a compound piece of its own tile and of every neighbour its bounds overhang, its cards a crown wide through every trunk and invisible, since the tail was drawing the real trees there. `create` now marks the meshes `SceneData::far_forest` names and gives them no body (`Stats::far_forest_left_out` counts the instances). The trees themselves still collide by their wood, through the authored entry the tail streams. `tests/far_forest_tests.cpp` holds it: a trunk 0.4 m square with a far tile's star of two cards 3 m wide over it stops a walker at the card's face 1.5 m out when the star is binned, and with the star named a far tile the walker passes there and stops at the trunk's face less its radius.

**How it was found** (the owner's v35 walk, 2026-11-07; engine-needs item 9, marks 1 and 6). The walker stood held in open ground at mark 1, (90.35, 104.65), 1.55 m from birch-0's trunk, and at mark 6, (−1473.3, 1225.6), 3 m south of pine-0's. A probe of the v35 scene loaded through engine-view's loader (`read_scene_file`, `authored_rules` at 2160 lines, `load_scene`: 5,335 prefix instances, 21,009 meshes, 11 streamed entries, a far forest of 4,753 tiles merging 89,741 trees), the consumer round each mark and the capsule swept along the recorded push, named the body: at mark 1 tile (0, 1)'s compound, member mesh 18953 "far forest (1, 1)", prefix instance 3279 standing at the tile corner (64, 0, 64) and reaching 1.9 m over the edge into tile (0, 1), its triangle 3 met 2 cm from the capsule with a horizontal normal (−0.999, 0, 0.051), a proxy of 512 triangles whose bounds are the whole tile's 66 × 72 m; at mark 6 tile (−24, 18)'s compound, member mesh 16304 "far forest (−24, 19)", prefix instance 630, its triangle 189 met 9 cm north with the normal (−0.896, 0, −0.444) of a card at the pine. Pushed for two seconds the walker moved 6 mm at mark 1 (1.6 m at mark 6, sliding along the card). With the far meshes left out, the same sweeps meet nothing but the rising ground and the walker walks on 1.84 m into birch-0's trunk at mark 1 and 2.61 m into pine-0's at mark 6, stopped at their wood. None of the brief's suspects was it: the trees' proxies stood where their wood is (birch-0's 67 triangles within 0.76 m of its axis, pine-0's 102 within 0.52 m, against crowns of 3.6 and 5.2 m at the 5 cm cut), at their scale, at their instances. The game's actors and the player's body were left out of the probe (no `--game`, no `--player`): neither is static collision, and the hit is a prefix instance's.
**The proxies are cut from the collision's own copy** ([below](#the-collisions-own-copy)), never from the scene's float streams after `create`, which a host is then free to give back.

**Vines and cover never collide** (2026-10-21, roadmap G76, G78; [cover](cover.md#collision)). A climbing plant or organic cover is its host's companion mesh, and the cover capability emits it under the same motion contract: part 0 a root with no geometry, every stem, leaf, film, cushion, tuft, bracket and cap on a later part with no structural flag. So the rule above leaves every one of its triangles out of the proxy and the companion gets no body at all — a rock's moss does not trip a walker, a wall's ivy does not stop one — with no code here that names the capability.

## The collision's own copy

(2026-11-15, roadmap T138; `include/systems/scene_collision/proxy_source.h`.) **A proxy is cut from a copy the collision keeps, not from the scene's float geometry.** Until T138 `cut_proxy` read the scene's merged float positions, packed triangles and per-vertex source indices (`SceneData::lod.mesh`) every time a tile asked for a mesh at a scale it had not cut yet, so a host with a walker had to keep all three for the session — about 2.1 GB of host memory on the forest's staging, all of it resident — while the renderer had measured everything it needed from them at the upload ([renderer](renderer.md), "The float geometry goes too where no collision reads it"). Now `ProxySource::build(scene, mesh_error_m)` copies, once, before anything gives those streams back, exactly what a cut can select, and `SceneCollision::create` is handed it (`proxies`) or builds its own from the scene there and then:

- **Only meshes that can have a body**: one the scene's own instances place (not a skinned one) or a streamed entry names; never the terrain's, a water body's or a far forest tile's ([What gets no body](#owned-data)).
- **Only the clusters a cut can select.** A cut takes a cluster when its own error is within the threshold and its parent's is not, so every cluster a threshold `t` selects has a parent's error above `t`; the copy keeps a mesh's clusters whose parent's error is above its **floor**, and every cut at or above the floor is in it. The threshold is `mesh_error_m` over the placement's largest scale, so the floor is `mesh_error_m` over the largest scale the mesh will be placed at, less 1% for the two ways that scale is computed: the scene's own instances say theirs (`InstanceDesc::scale_max`), and a streamed entry's tile, which the load cannot see, is allowed **`k_copy_scale_headroom` (4) times the mesh's fit** — past every scatter range and tree recipe in the tree, and two DAG levels finer than the default cut. A placement larger than that is cut at the floor instead, coarser than `mesh_error_m` by the ratio, and counted (`Stats::proxies_coarsened`), as is a consumer whose own `mesh_error_m` is finer than the copy's (engine-view builds the copy at the smaller of the walk's and the draw's).
- **Only the triangles a proxy keeps**: a tree's wood (`renderer::flora_part_collides`), decided once at the copy, since it depends on the mesh and never on the scale; the cut still counts what each selected cluster left out (`flora_triangles_left_out` is what it was).
- **Compacted**: a cluster's points (position and weld key, 16 bytes) are only those its kept triangles use, its triangles keep the scene's packing of three 8-bit corners (4 bytes, and 2 more for the first corner's flora part, which the debug draw reads), a cluster record is 24 bytes, and one that keeps no triangle is 12 (below).

**A proxy cut from the copy is the proxy the scene's floats gave, word for word**, at any threshold at or above the floor: the same clusters in the same order, the same triangles, the same weld keys, so the remap meets the same first vertex of every key. The suite's answers are unchanged (the 3 m ball's 510 triangles and 4.2 cm, every walk's stop, `world_query_tests`' pinned hash of five casts and three supports queries), and `scene_collision: a scene whose floats are gone collides from the collision's own copy` builds a scene twice, copies one and releases its floats (`renderer::release_uploaded_streams` with `HostReaders::none`), and holds every held proxy against the old cut of the other's floats, vertex for vertex and index for index, then walks a character into the wall it holds and stops it at the face less its radius (0.06 s in `msvc-release`); a consumer asking for an error of zero from a copy built at 5 cm cuts the floor and counts both proxies. A tile's own mesh ([A tile's meshes](#a-tiles-meshes)) goes through the same cut: `tile_proxy_for` copies the one mesh it loaded into scratch at the one threshold it asks, so there is one cut, not two.

**What it weighs** (engine-view's load logs it: `the collision's own copy`, with `bytes`, `scene_float_bytes`, and the clusters and triangles kept of the copied meshes'). Measured 2026-11-15 with `--collision-draw` offscreen, `msvc-release`, the machine shared with other agents' builds and GPU work (the bytes do not depend on it):

| Scene | Copy | The scene's float streams | Share | Clusters kept | Triangles kept |
|---|---|---|---|---|---|
| `content/test-scenes/streamed-forest` (13 meshes, trees streamed) | 2,037,574 B | 61,154,772 B | 3.3% | 1,728, and 48,822 emptied, of 54,053 | 65,217 of 1,914,726 |
| `content/test-scenes/grove` (4 meshes) | 78,570 B | 5,821,308 B | 1.3% | 60, and 1,685 emptied, of 4,109 | 2,177 of 233,208 |
| the test's 3 m ball at 1.7 times its size and a wall | 33,160 B | 239,984 B | 14% | 25 of 180 | 1,984 of 16,312 |

What makes a forest's copy small is the wood, not the floor: a generated tree's DAG is coarse enough that most of its clusters have a parent's error above the 1.2 cm floor a headroom of four gives a 5 cm cut, but nearly all its triangles are leaves, needles and twigs, which no proxy keeps (3.4% of the streamed forest's triangles are wood). A cluster that keeps none of its triangles is still selected by some cut, and the cut counts what it leaves out, so it is kept as an **emptied** record of its two errors and that count (12 bytes; `ProxySource::Emptied`) rather than a cluster's 24: on the streamed forest 48,822 of them, 586 KB of the 2.0 MB (they were 1.2 MB of 2.6 as whole records). At the streamed forest's share the forest staging's 2.1 GB of floats leaves a copy of about 70 MB. The copy is held for the session, like the descriptors; a copy by page with the world's tiles, or proxies built at the content build, is the next cut if a world's copy grows large.

**What a host does** (engine-view, [apps](apps.md#walking)): `view::copy_collision_geometry` builds the copy after the GPU scene is made and before `attach_page_source` or `view::release_host_geometry` can take the floats, when a walker will walk (`interactive.on`) or `--collision-draw` will draw; the walker (`Walker::start`) and the draw (`CollisionView::create`) are handed it, and the release passes `HostReaders::none` in every session. A walker over a `--stream` scene had been cutting from the arrays `attach_page_source` had already emptied; it now cuts from the copy too. `engine-view: a walk into a ruin's wall is stopped at the wall` (`apps/engine_view/tests/walk_ruins_view_tests.cpp`, on a device) holds it end to end: the walk logs the copy and the release with `kept_for=nothing`, is stopped at the wall's face less its radius, and replays to the same trajectory offscreen.
## Structures

(2026-11-14, roadmap S89; plan [14 §14.4](../plan/14-structures.md#144-walkability-and-collision).)
**A building is a placement, so it collides as every placement does, and nothing here names one.**
A house the village filler stands ([village](village.md)) is a mesh its tile hands as a recipe —
the building description — so its proxy is built from that recipe with no device
([A tile's meshes](#a-tiles-meshes)): the house's own LOD DAG cut at 5 cm and welded. That is the
shell's walls with their thickness, the inner walls, the floors and the stair's treads as the
mesher made them, and a doorway is open because the proxy is the surface and never a hull
([Why this shape](#why-this-shape)). A hand-authored description placed by a scene file, and an
imported glTF house, get the same proxy by the same path. What a walker needs of the building is
therefore the **mesher's** to give — a door at least 0.8 m wide and 2.0 m high, a riser under the
walker's 0.35 m step, headroom over a stair — and plan 14's table is held by the description's
validators, not here.

`tests/structure_walk_tests.cpp` (where the village and the structures capabilities are linked;
no device) holds it on a streamed scene of one village entry and no terrain: the walker starts on
the lane, follows the house's own way through it ([village](village.md#the-way-through-a-house)) —
the path, the doorway, each room's doorway, the stair — and must reach every point, ending on the
upper floor at a window, standing; and a second walker set down on the pad beside the doorway and
walked at the house is stopped at the wall's face, a capsule's radius short of it — the face being
the plot's own setback behind its front, read from where the plan put the door, since a grown
village sets each house back by its own draw. A second case stands each two-storey house type the
rural filler builds alone on the floor (a test generator hands the filler's own description on
one tile) and walks a capsule up its stair, from a step before the first riser to a step past the
last, for three seeds a type; it is the case that found a stair whose upper floor ended over its
first riser ([dwellings](dwellings.md#the-stair)).

**Not built of S89**: a no-collision flag per part (a thatch fringe or a rope's strands are walls
where the proxy keeps them), kinematic bodies (a door's leaf), and the nav agent held to the
walker's numbers ([Not yet](#not-yet)).

## Props

(2026-11-17, roadmap P45; [apps](apps.md#props-the-objects-a-game-places).) **A prop a game
places is a static body the walker stops at, while the tile it stands in is held.** A host hands
its props with `SceneCollision::set_props` — each a `PropBody`: a key (a game prop's id), a scene
mesh, the origin as a `WorldPos`, a turn, a uniform scale and the body the scene declared
(`renderer::PropCollider`, from `engine.scene.Prop.collision`) — on the tick they changed, and
`update_props` runs on every tick after the ring's update and before the step. Three bodies:

- **The mesh** (the default, `collision` absent): the prop's own LOD DAG cut at
  `walk.collision.mesh_error_m` and welded — the proxy every placement gets, at the prop's scale,
  through the same cache (`proxy_for`), so two crates share one shape — placed where the renderer
  stands the instance: the mesh's fit through the turn and scale, its offset added to the origin in
  f64. The collision's own copy keeps every prop mesh that collides by its mesh, at the streamed
  entries' headroom over its fit, since no load placed it and a module may scale it
  ([The collision's own copy](#the-collisions-own-copy)).
- **A box** (`"collision":{"shape":"Box","half_extents":[x,y,z],"center":[x,y,z]}`): metres in the
  prop's frame at scale 1, scaled by the module's scale — for a prop whose mesh is thin, open or
  heavy, where a box is what a walker should meet.
- **None** (`"shape":"None"`): a marker, a curtain.

**A body a prop, not a piece of its tile's compound.** A module moves one prop at a tick, and a
compound would be built again whole for it, with every other piece of the tile; a prop's body is
made, moved and let go alone. **Made with its tile, let go with it**: `update_props` gives a body to
every prop whose tile is held (`covers` its origin) and has none, and takes it from one whose tile
went, or which a later `set_props` left out or gave another shape (mesh, scale or collider) — all
let go first, then made, each in key order — so the bodies, their ids and a walk that touches one
are the same in a replay (the ring's rule, [Why this shape](#why-this-shape)). A prop handed again
unchanged keeps its body. **A prop that only moved is moved**, never made again: its body is put at
the new place (`set_body_transform`), and if the new place is off the held tiles it is let go. The
broadphase is rebuilt once an update that made or let go a prop's body, for the reason the tiles'
is (a removed body's node comes back only with a rebuild), and **never for a move**. A box is made
static and, at its first move, made once again as a kinematic body in `Layer::Kinematic`, the tree
the physics library keeps for things that move, then only moved; a mesh's proxy has no inside, the
library moves it only as a static body, so a moving mesh prop is moved where it is, in the static
tree, whose nodes over its path stay widened until the next tile that comes or goes rebuilds the
trees. A prop declared `None` is not kept past `set_props`. A cast that meets a prop is
`HitKind::prop`, its `instance` the prop's key.

### What a moving prop costs

(2026-11-18.) The case `scene_collision: a prop moved every tick costs the walker's tick a move,
not a rebuild` (`tests/far_forest_tests.cpp`; no device; 0.4 s in `msvc-debug`) stands N
crates on the held tiles round a standing walker, moves k of them a centimetre a tick, and times
300 ticks of what the walker's host does at 240 Hz — the ring's update, the props' list built and
handed when one moved (`walk.cpp`), `update_props`, the character's step — one tick at a time. It
asserts that a move makes no body and that a tick allocates nothing in steady state. Medians of a
tick in microseconds, the lowest and the median of three runs, "before" each move a body made again
and the broadphase rebuilt; `msvc-release` with the machine at 37 to 76 % CPU, `msvc-debug` at 76
to 100 %, with about twenty-five other rows building and testing, so upper bounds; against the
tick's 4,167 µs:

| N | k | collider | release before | release after | debug before | debug after |
|---:|---:|---|---:|---:|---:|---:|
| 30 | 0 | mesh | 2.4 / 2.4 | 2.4 / 2.4 | 37 / 52 | 50 / 53 |
| 30 | 1 | mesh | 11.3 / 11.5 | 3.9 / 3.9 | 303 / 340 | 72 / 74 |
| 30 | 30 | mesh | 40 / 48 | 14 / 14 | 781 / 857 | 291 / 295 |
| 300 | 0 | mesh | 2.5 / 2.5 | 2.5 / 2.5 | 38 / 57 | 58 / 59 |
| 300 | 1 | mesh | 74 / 75 | 12.6 / 12.8 | 2,404 / 2,484 | 142 / 145 |
| 300 | 30 | mesh | 104 / 104 | 21.6 / 22 | 2,792 / 3,184 | 366 / 373 |
| 300 | 30 | box | 103 / 104 | 18.8 / 19.7 | 2,921 / 2,991 | 318 / 329 |
| 300 | 30 | none | 15.6 / 15.6 | 4.7 / 4.7 | 172 / 173 | 73 / 77 |

Before, one animal moving among 300 standing props cost the walker's tick 75 µs in release (2.4 ms
in debug, more than half the tick), nearly all of it the broadphase rebuilt over every body for one
move — and that over this case's bodies alone: a walk's broadphase also holds every held tile's
ground and placements, which the rebuild walks too. After, a move is the body put at its place,
about 0.3 µs a moved prop in release (k = 30 against k = 1), and what a moving prop still costs
beside it is the hand-over: the host builds and hands the whole list when any prop moved, and
`update_props` walks it against the held set (the 300-prop row's 9 µs over the 30-prop row's at
k = 1 in release). That walk is linear in the props that exist; a hand-over of only the props that
changed is the next step if a game moves many props among thousands. The debug k = 0 rows run no
new code; their difference is the machine's. No tick allocated, before or after.

**What a game with moving creatures should declare**: a box (`"collision":{"shape":"Box",...}`)
the size of the creature's body, which after its first move is a kinematic body in the tree the
physics library keeps for things that move, or `None` for a creature the walker should pass
through (a bird, a fish, an insect), which costs nothing past the hand-over. Declaring nothing gives
it the default, its mesh's proxy: right for a prop that stands, and for a creature it is a static
mesh body moved every tick, which widens the static tree the tiles' ground and placements live in
until a tile comes or goes, and stops the walker on every limb of the mesh rather than its body.

**A kinematic body is a surface like a static one** (2026-11-18): every query that asks what
stops or carries the walker, or what a module's cast hits, takes `scene_collision::k_scene_surfaces`
— the static layer and the kinematic one, every body the collision makes and nothing else in the
world. They are a module's `Walker::cast`, the camera's boom (`cast_boom`), a drop's and a
teleport's highest surface below (`Walker::drop`), the moving ground's "standing on the ground"
ray (`follow`) and a piece's top support (`piece_supports`). Until then they took the static layer
alone, so a box made kinematic by its first move dropped out of all of them while the walker's
capsule, which meets every layer but `Query`, still stopped at it: a module's cast met what stood
behind the crate, the boom passed through it and a teleport onto it landed under it. A door, a
kinematic box too, is met the same way: the boom stops short of a closed door rather than passing
through the leaf. `scene_collision: a moved box prop is met by the walker's and a module's casts`
holds it, a ray down onto the box and a sphere at its face before and after it moves; the same
case hands one key twice and holds that the first handed is kept (`Stats::prop_duplicates`) and no
body is left behind.

### A prop's proxy

(2026-11-18, a review of P45.) **A mesh prop's proxy is one a mesh and rounded scale, built off
the tick, and let go when no prop names it.** Until then a body's proxy came from `proxy_for`, the
placements' cache, keyed by the exact float bits of the scale `decompose` read back out of the
turned and fitted matrix: a miss cut the proxy and built its mesh shape inline on the walker's tick,
each new module scale (rocks placed at 0.8 to 1.2) was a new build, and every entry was kept for the
consumer's life. (The review's further worry, that a fresh yaw would miss because a turned column's
length is 1 only within an ulp, did not hold on the base: ten yaws at two scales made two proxies,
`msvc-release`. The key is now free of the turn by construction.) Now props have a table of their
own (`SceneCollision::PropProxy`):

- **The key is what makes two proxies the same**: the scene mesh and the module's scale rounded to
  a step, and nothing of the turn. The proxy's scale is the module's times the fit's own column
  lengths (`decompose` of the fit alone), its turn the prop's times the fit's, so the yaw never
  reaches the key. The step is `mesh_error_m / (2 reach)`, where the reach is how far the fitted
  mesh can stand from its origin (its 16-bit grid's cube through the fit, an upper bound), rounded
  to the nearest and anchored at 1, so a prop at its own size is exactly scale 1: the body is built
  and placed at the rounded scale, which moves no point of it more than a quarter of the walk's
  error off the drawn mesh, on top of the cut's own error. For the walk's 5 cm a metre crate's step
  is 0.020, a 3 m boulder's 0.007; props scattered at 0.8 to 1.2 are at most 20 and 60 proxies. A
  scale within the step keeps the body; a new step is a new shape (a body let go and made).
- **A declared prop's proxy at its own size is built at the load** (`create`, from
  `SceneData::props`): the scene names what its game may place, and the first placement of one at
  scale 1 costs the tick nothing. Those stay resident for the consumer's life.
- **Any other is built on the job system** `create` was handed, and the prop's body is made at the
  `Config::prop_build_updates`-th `update_props` after the one that first wanted it (engine-view's
  walk: `k_walk_prop_build_updates`, 12 ticks, 50 ms at 240 Hz) — taken ready, waited for if a
  worker is still on it (`Stats::prop_proxy_waits`), built there if none started it
  (`prop_proxy_inline`). **A count of updates, never a clock**, so the body is made at the same tick
  in a recording and its replay whatever the workers did, and with no job system at the same tick
  too (built on it). For those 12 ticks the new prop has no body: the walker can pass through a prop
  of a mesh and size never placed before for a twentieth of a second after it appears. A host with
  no job system and the tests' default (`prop_build_updates` 0) build it on the update that wants
  it, as before.
- **A drop, a teleport and a walk's first tick take every pending proxy in reach now**
  (`update_props(true)`): each mesh prop on a held tile whose proxy is not ready has it built at that
  update, or waited for if a worker has it, and its body made there. Those are the updates that
  look for every body at once (a drop casts straight down for the highest surface), and before
  2026-11-18 a drop onto a boulder of a new size met the floor under it and left the walker inside
  its shell for the twelve ticks. It is a step of the tick sequence, never a clock: engine-view's
  walker passes it at a drop and at the first step after one (walk.h `set_props`), a replay drops
  and steps at the same ticks, and so takes the same proxies at the same update. Its cost is the
  build on that tick, which a drop already pays for the ring's tiles.
- **A save holds no proxy state, and needs none.** When a body appears depends on the table's
  history (which keys are resident, which are pending and until which update), and a fresh consumer
  has none of it. But engine-view writes a game's state only when the session finishes
  (`--game-state`, game_runner.h `finish`), so no session continues past its save, and a walk
  restored from one begins, as every walk does, with a drop, which takes every proxy in reach: from
  that update the resident set is the props in reach and the declared, what a session that had
  walked there holds too, and every body in reach is made. A replay restores from its recording's
  header and makes the same drop at the same tick, so it matches the recorded session. What would
  differ is a session that continued past a save, which nothing makes: there, a prop whose proxy was
  pending at the save would get its body at the restore's first tick instead of up to
  `k_walk_prop_build_updates` ticks later. A save taken mid-session would have to carry each pending
  proxy's remaining updates.
- **Let go when no prop in reach names it, so with the tiles**: at an update whose props' set
  changed, whose ring moved, or at which a prop moved off the held tiles, each proxy is counted over
  the held props **whose tile is held**, and one none of them names (and not declared) goes, a proxy
  a job is still building kept aside until the job returned (`Stats::prop_proxies`,
  `prop_proxy_builds`, `prop_proxies_freed`). So the table is bounded by the props round the
  walker, not by the ground it walked: until 2026-11-18 it was counted over every prop handed, and
  a proxy built while its prop was near stayed until the game stopped handing the prop. Coming back
  builds it again, its body at its promised update, which is the price: a walker going to and fro
  across a tile's edge rebuilds a new size's proxy each time the ring lets the tile go (the ring's
  hysteresis bounds how often). A body holds the backend's reference to its shape (`adopt_shape`), so a
  proxy let go never takes a body's shape with it.

The placements' proxies (`proxy_for`, a tile's pieces) keep their own cache, built on the
collision's workers ahead of the ring; they are bounded by the scene's meshes and the generators'
scales, and are not this rule's. What an imported or hand-authored asset gets: the same, by its
mesh's grid and fit, with nothing declared beyond `engine.scene.Prop`; a scene with no mesh props
builds nothing at the load and pays nothing on the tick.

Six cases hold it (`tests/far_forest_tests.cpp`, no device). `a mesh prop's proxy is one a mesh
and rounded scale, whatever its yaw, and goes when no prop names it`: one crate at ten yaws at its
own size and at 1.1 is one build, for 1.1; 1.103 keeps the body and builds nothing (with the
rounding and the letting go switched off it fails: three bodies made, three builds, three proxies
resident after the props went and none let go); the walker stops at the rounded crate's face within
a quarter of the error; and handed none the 1.1 proxy goes while the declared one stays (0.4 s in
`msvc-debug`). `a prop's proxy is built off the tick, its body made at the update promised` (1.9 s):
a declared boulder at 1.2 gets its body at the twelfth update after its hand-over, with workers and
without. `a drop takes every pending prop's proxy now, so it lands on a boulder`: the boulder at
1.5 handed on a walking update has no body yet and a ray straight down meets the floor; the drop's
update after it makes the body, and the ray meets the boulder's top within the cut's error; handed
at the drop itself it is built there; with workers and without. `a restored walk's first update has
the bodies the saved walk had`: four props, three at sizes of their own, handed to one consumer and
updated twenty times, and to a fresh one at a drop's update, give the same bodies, the same proxies
resident and the same tops to the bit. `a prop's proxy goes with its tile,
and comes back with it`: a boulder at 1.2 still handed, the ring moved a kilometre away, loses its
body and its proxy while the declared two stay, and back it is built again. `a prop of a new mesh arriving mid-run costs the walker's tick no build` (0.4 s) measures
it: 300 crates of which 30 move a centimetre a tick, and at tick 100 a boulder 3 m across (17,958
triangles; the collision's own copy of it keeps 3,322) handed with them. Microseconds of a tick,
`msvc-release`, two alternating runs, with the machine at 38 to 66 % CPU and other rows building:

| the boulder | the arrival's tick, before | after | the worst tick after it, before | after | its body, before / after |
|---|---:|---:|---:|---:|---|
| at its own size (declared) | 342 / 382 | 87 / 86 | 32 / 32 | 30 / 40 | at once / at once |
| at 1.37 | 641 / 614 | 41 / 74 | 66 / 35 | 329 / 309 | at once / 12 updates on |
| at 1.37, no job system | — | 53 / 30 | — | 642 / 698 | — / 12 updates on |

Before, the proxy was cut and built on the arrival's tick: 0.3 ms at the boulder's own size and
0.6 ms at a new one, both on the tick, with the body made beside it. After, a declared prop at its
own size costs the arrival's tick its body (the remaining 86 µs: the body made and the broadphase
rebuilt round 301 bodies); at a new size the arrival's tick hands the build to a worker and the
tick twelve updates on makes the body, waiting there for the 0.2 ms of the build a worker had not
finished — the case's ticks run back to back, not at 240 Hz, so its twelve updates give the worker
about 0.3 ms where a walk's give it 50 ms, and in a walk that wait is nothing. With no job system
that tick builds it, as the arrival's tick did before. The ticks that do not change the set (the
move-cost case's rows above, run again beside these) measured the same before and after within the
machine's noise, and no tick of either case allocated in steady state.

`scene_collision: a game's prop stops a walker, by its mesh or its box, while its tile is held`
(`tests/far_forest_tests.cpp`, beside the far forest's case, whose scene is built the same way; no
device) holds the rest: a crate a metre on a side declared as a prop and placed by nothing stops a
walker going north at its face less the capsule's radius; handed again unchanged nothing is made;
moved 3 m the body is moved, nothing made or let go, and it stops the walker there; at twice the
scale the face is a metre out; a declared box 0.25 m deep stops it at the box's face, and moved
twice it is made again once, as a kinematic body, and stops the walker at its new place; `None` and
a removal let it through; the ring moved a kilometre away lets the body go with the tile, and back
it is made again; a cast names the prop by its key.

## Doors

(2026-11-18, roadmap S91; [ADR-0070](../adr/0070-a-door-is-a-kinematic-box-held-by-the-walker.md);
plan [14 §14.4](../plan/14-structures.md#doors-the-first-kinematic-bodies).) **A door is the
scene's first body that moves**: a box on the kinematic layer the size of its leaf, about an upright
hinge (`DoorBody`: the hinge's foot, the closed heading and the swing in integer millidegrees, the
leaf's half sizes, the speed, the state and the angle reached). A host hands its doors with
`set_doors` and calls `step_doors(&walker)` on every tick after the ring's update and before the
walker's step; `toggle_door(key)` at a tick sends a door toward its other state; `door(key)` reads
it back, which is what a saved village keeps. **Made with its tile, let go with it**, in key order,
as a prop's body ([Props](#props)); its state is the world's and goes on while its tile is away.
**Its angle moves by an integer a tick** (`k_door_speed_mdeg` = 375, 90 degrees in a second at 240
Hz) and the box is put at the pose that integer names with `set_body_transform`, never driven by
`move_kinematic`, because this world is never stepped (ADR-0070 decisions 2 and 3). **The turn is
the same bits on every compiler** (`door_rotation`): the integer is reduced to a half turn either
way and the half angle's sine and cosine are core/math's deterministic ones (`det::sin`,
`det::cos`), never the C library's, which until 2026-11-18 made a walk through a door replay
differently on MSVC and glibc although the angle was the same; `scene_collision: a door's turn is
the same bits on every compiler` pins it over two turns at an eighth of a degree. **`set_doors`
keeps the numbers in range and one door a key**: a speed of 1 to 180 degrees a tick (none the
default), a swing within a whole turn either way, so the integer swing never overflows; the first
door given of a key is kept and a later one refused and counted (`DoorStats::refused`), so `door`
and `toggle_door` answer for one door. **Left**: a door whose leaf already stands in the walker
(a walker teleported into its arc) holds both ways, since every next angle meets it; it should
move away from the walker and hold only toward it, which needs the overlap's depth at the two
angles compared, not a yes or no. **A door that
would enter the walker holds its angle**: the leaf at the next angle is tested against the capsule
— both upright, so the test is exact in the leaf's frame — and a door that would come within
`k_door_hold_margin_m` (5 cm, past the character's contact padding, so a leaf never presses the
walker into stepping aside for it to creep after) stays where it was that tick and counts a hold (`DoorStats::holds`). The walker collides as `Moving`, so
its sweep meets the kinematic layer: a closed door is a wall, an open one a wall at its angle. A
cast that meets a leaf is `HitKind::door`, its `instance` the door's key. **What it costs**: a
scene with no doors pays an empty check a tick; a door costs a transform write on the ticks it
turns and nothing standing. **A swing is a move** (2026-11-18), as a prop's is ([What a moving
prop costs](#what-a-moving-prop-costs)): the box is put at its angle and its node in the kinematic
tree follows it, and the broadphase is rebuilt only at a step that made or let go a door's body.
Until then every tick a door swung rebuilt it over every body the world holds. `scene_collision: a
swinging door costs the walker's tick a move, not a rebuild` (0.2 s in `msvc-debug`) stands 30
doors and 300 static boxes round a standing walker, keeps k of them swinging, and times 300 ticks
of the ring's update, `step_doors` and the step; it asserts every swinging door moves every tick
and that no tick allocates. Medians of a tick in microseconds, `msvc-release`, two alternating runs
with the machine at 73 to 87 % CPU and other rows building, so upper bounds:

| doors swinging | before | after |
|---:|---:|---:|
| 0 of 30 | 2.9 / 4.8 | 4.9 / 2.8 |
| 1 of 30 | 57 / 68 | 5.5 / 3.2 |
| 30 of 30 | 69 / 80 | 18.7 / 18.7 |

Before, one door swinging cost the tick about 60 µs over 330 bodies, all of it the rebuild, and a
walk's broadphase also holds every held tile's ground and placements; after, a swinging door costs
about half a microsecond (30 against 0), the leaf test against the walker and the transform write.

Three cases in `tests/far_forest_tests.cpp` (no device): a closed door across a doorway stops a
walker at the leaf's face less its radius, toggled it swings open in exactly 240 ticks and the
walker goes through; a door closing on a walker in its arc holds short of it, counting holds, and
finishes once the walker steps away; and two worlds given the same toggles at the same ticks reach
the same angle, the tile let go takes the body and keeps the door, and the body is made again when
the tile comes back.

**Not built of S91**: the leaf meshed on its own with its pivot on the hinge (the mesher draws it
in the house's mesh, G65), doors handed from a building's description, the walker's interact
action and a game module's service (an ABI step), the state written back through the document
(D37), and a closed door as an area navigation opens.

**What a door from a building's description needs** (2026-11-18). The description already says
everything a door is: an opening with a leaf (`structure::Opening::leaf`, `leaf_open`, and the jamb
it hangs on, `hinge_at_to`), which `structures::walk_records` turns into a `WalkLeaf` — the hinge's foot and the leaf's tip
in the building's frame, its foot and head heights, its thickness and the angle it stands at — so
`DoorBody`'s hinge, closed heading, half sizes and swing follow from it with no rule of the
collision's own. What is missing is the way from the description to the scene, in three pieces, and
none of them belongs in this module: **(1)** the structure mesh generator writes each leaf into the
built container as a section of its own (append-only, as the perches and the structural table are,
`geometry::cluster_file.h`), so a tile-handed house and an imported asset that declares doors carry
them the same way and the collision reads them through `renderer::load_placement_mesh` as it reads
a mesh's perches; **(2)** the mesher leaves the leaf out of the house's mesh, or marks its
triangles as no collider (S89's no-collision flag per part), since the proxy today is cut from a
mesh that holds the leaf standing at `leaf_open`, and a kinematic leaf beside it would leave the
drawn open leaf a wall whatever the door's state; **(3)** a tile's gather makes one `DoorBody` a
leaf of each placed piece, keyed by the piece's name (`support_placement`) and the opening's
index, so a door is the same door whenever its tile comes in. Until then a host hands doors itself
(`set_doors`), and nothing in a scene does. **Piece (1) is built** (2026-11-18): the structure
generator writes every leaf into its container as section 53 (`geometry::ClusterFileDoor`,
[geometry](geometry.md#doors)), shut, with its pivot, the way it swings and the angle it stands at,
so `DoorBody`'s hinge, heading and swing are the record's and the angle needs no arc tangent; the
loaded scene does not carry it to this module yet, and pieces (2) and (3) stand as written.

## Holes

(2026-11-07, roadmap W87; [terrain](terrain.md#holes).) A terrain's `holes` take the ground away under them, as the drawn ground's meshes leave their cells out. On the collision lattice a sample is **no sample** — the backend's no-collision height, `physics::k_heightfield_hole`, Jolt's `cNoCollisionValue` — where all four cells round it are holes by the terrain's cell rule, so the triangles that use it have no collision and the opening the walker falls through lies inside the one drawn (at the drawn levels' finer spacing the same rule takes the rim cells closer to the outline). `ground_height` answers none over a cell any of whose corners is no sample. A walker that steps in falls to whatever stands below — a cave passage's floor, a body like any placement's (the passage is a mesh, and its proxy is cut from its DAG, [caves](caves.md)). Only a tile whose window a hole's bounds reach pays: one bounds test a hole a tile, and then a copy of its heights with the hole samples set (`SceneCollision::holes_reach`, `hole_sample`). `tests/terrain_hole_tests.cpp` holds it: a 6 m round hole over flat ground with a floor 4 m below — no ground at the centre or 1.9 m out, the ground 4.5 m out, a ray straight down meeting the floor through the hole and the ground beside it, and a walker 6 m west walking east falls in and stands on the floor at −4 m.
## Channels cut into the ground

(2026-11-07, roadmap W89; [tileset](tileset.md#channels-cut-into-the-ground).) A channel's cut is written into an authored ground's heights when its set is built, so the collision, which reads the set's source, stands on the cut ground with **no change of its own**: the heightfields hold the bed and the banks' faces, and `ground_height` inside the cut answers the bed. `tests/channel_cut_tests.cpp` (where the tile-set capability is linked) holds it: a channel 4 m wide and 0.8 m deep with banks at a rise of 0.5 over flat ground 2 m up answers 1.2 m on its line, 1.5 m a metre either side, 2 m at the top of the bank and past it, the set's heights to the bit on the collision's lattice, and a character walked across it goes down to the bed and up onto the far floodplain.

**Raised ground the same way** (2026-11-10, roadmap W98; [tileset](tileset.md#ground-edits-that-raise-as-well-as-lower)): an edit that fills — a causeway, a berm, a pad, a deposit's surface — is written into the same heights, so the collision stands on it with no change of its own either. `tests/channel_cut_tests.cpp`, "the ground on a fill is its crest, and a walker stands on it": a causeway 4 m across its crest and 1.5 m over flat ground 2 m up answers 3.5 m on its crest, 2.563 m two metres past it (down its face at one on two and rounded into the ground), 2.167 m six metres out and the ground at ten, the set's heights to the bit, and a character set down over the crest stands on it (0.7 s in `msvc-debug`).

## The entries before this one

(2026-11-11, roadmap T114; [scene_gen](scene_gen.md#the-shapes-a-tile-occupies).) A streamed scene's entries are asked for a tile in the scene's order, and each is handed what the ones before it put there: **their placements' discs and then the shapes they said they occupy** in `Context::earlier`, and **those shapes in the tile's exclusion**, a copy of the scene's made the first time an entry of the tile hands one. That is `world::PlacementTiles::build`'s rule for the drawn tile, followed here word for word in `gather_placements`, so the collision's tile is the drawn tile: a disc at the placement's millimetre, its mesh's reach on the ground (`renderer::ground_radius`, measured once a mesh a gather, or the tile's own word for a mesh it handed) times its larger ground scale. Until T114 `context_for` handed no `earlier` and `gather_placements` no shape, so an entry that kept off a tread, a mouth's apron or a tree's disc on the drawn tile made bodies on them here — posts nobody could see, standing on the path. The lists are the gather's own (a build ahead gathers on a worker), and a polygon's points are copied into them (`scene_gen::FootprintList`), since the scratch the entry wrote them into is the next entry's by then. One thing still differs from the drawn build: the drawn tile hands a generator `Context::stand`, the ground under a time-lapse as it stood at the tile's build, and the collision hands none.

`tests/scene_collision_tests.cpp`, "a later entry keeps off a tread's band as on the drawn tile": a tread entry hands a band 1.5 m either side of z = 16 m and stands one post, and a probe entry stands a post on every point of a 2 m lattice nothing earlier covers; the collision's probes on tile (0, 0) are the drawn tile's, point for point (239, the band's row of 16 and the post's point left out), nothing is under the band's line, and a walker going east along it walks its 15 m unstopped (0.4 s in `msvc-debug`). Run against the gather before T114 the collision's probes are not the drawn tile's, straight down on the band's line meets a probe's top 1.5 m up, and the walker stops at x 1.38, against the first probe on the band.

## Seeing the collision

(2026-11-08, roadmap P62; `include/systems/scene_collision/collision_draw.h`.) An invisible wall is named by looking at it: `CollisionDraw` is a frame pass registered at the renderer's registration point ([renderer](renderer.md#frame-passes), `SceneRenderer::add_frame_pass`) that draws, over the shaded picture, every held tile's **proxy triangles as wireframe** — one colour a tile's compound, the hue from the tile's coordinates, the walker's own tile at full brightness — the **ground's heightfield lattice** as lines in teal (bright under the walker), so the ground's collision is seen against the drawn ground, and the walker's **capsule** in yellow. engine-view's `--collision-draw` ([apps](apps.md)) is its host.

- **Depth-tested against the frame's own depth.** The pass is one compute shader (`shaders/collision_lines.slang`), a thread a line and a view: the segment is clipped to the near side in clip space, projected, clipped to the view's rectangle and walked a pixel at a time, and a pixel is written into the linear colour image where the line's reversed-Z depth is at least the visibility buffer's there less 1% of it (`DrawOptions::depth_tolerance`), so a line on the surface it outlines shows through that surface's own depth and a line behind a hill or a trunk does not. A compute walk rather than a line pipeline because the frame's depth is the visibility buffer, a buffer the frame-pass context hands over, not a depth attachment; a resampled (Panini) view is skipped, since its region of the buffer is not its picture.
- **Made on the CPU each frame it is on, in the frame's space.** The lines are built at record time from the consumer's held tiles (`held_tile`, `held_piece`), in metres from the frame's eye with the difference taken in f64 (ADR-0053), and copied into a host-visible buffer a frame slot, grown to the next power of two and never shrunk, so a steady frame makes no buffer. A tile's pieces are kept only when the consumer was made with `Config::keep_shapes`, which keeps each proxy's triangles and each held tile's pieces on the CPU beside the backend's shapes and changes no body.
- **A budget of lines a frame** (`DrawOptions::max_lines`, 1,048,576, 32 MB a frame slot at most): the tiles within the radius are taken nearest first, whole, and the first that would pass the budget is dropped with every farther one; `DrawStats` counts what was drawn and dropped and the draw logs when the dropped count changes.
- **Whose collision.** The draw keeps a consumer of its own (`CollisionView`): a physics world, one ring round a point with no budget, the walk's numbers — the same bodies a walker standing there meets, since a consumer's tiles are a function of the scene and those numbers. engine-view stands it at the walker's feet when walking and at the frame's eye in flight, so a camera path can look at a mark. Its ground is the ground provider seen as tiles, which is the authored set's own heights to the bit ([The ground moves](#the-ground-moves)).
- **Off, nothing exists**: no consumer, no pass, no buffer, and the frame is the bytes it was (frame_pass.h's contract; `tests/collision_draw_tests.cpp` captures it before, with and after).

`tests/collision_draw_tests.cpp` holds it: the lines are three a held proxy triangle plus the lattices' 2 n (n − 1) a tile and the capsule's; with room for five of nine tiles the four corner tiles, the farthest, are the ones dropped; and on a device a box over the hills colours over a hundred pixels inside its projected outline and none outside it, and with the pass removed the frame is the bytes it was before.
## Invariants

Each is a test in `tests/scene_collision_tests.cpp` (no device, no generator capability: a ground provider and a placement generator of the test's own, meshes whose DAGs the geometry module builds) unless it says otherwise.

- The ground is a heightfield a tile on the provider's lattice: 9 tiles, 9 bodies and 9 × 34 × 34 samples round a walker at a tile's centre; straight down onto the collision the provider's height to 0.5 mm on the lattice (the backend's quantization), and `ground_height` the backend's triangulation to 0.3 mm between.
- Bodies come and go with the ring within its budget: two tiles an update from a first fill, the walker's own among them; walking 2 km east and back with a post on every tile never more than 9 tiles and 18 bodies held, the physics world's body count always the consumer's, every body made either live or let go, and the walker's ground always the provider's; then 1,200 updates a whole tile apart with no budget, 7,846 bodies made in all through a world of 256, and no failure.
- A scene read whole collides as its instances: one proxy a mesh however many instances, a wall's top 3 m up where its box's is, a piece across a tile edge counted in both tiles, and a character walking at the wall stopped at its face less its radius and padding.
- Dense sward has no proxy: all 4,800 triangles of 400 parentless flagless blades, including zero, are excluded; the paired flagged trunk retains 12 triangles and stops the same walker. The POC route A/B remains for its session to resume; this synthetic regression establishes the engine rule, not the cause of that route symptom.
- The far forest never collides: a far tile's star over a trunk, binned, stops a walker 1.5 m out; named in `SceneData::far_forest` it is left out and the trunk stops the walker at its face less its radius (`tests/far_forest_tests.cpp`).
- The collision draw's lines are the held proxies' edges and the lattices', its budget drops the farthest tile first, and its pass draws where the proxy is and nowhere else; off, the frame is the bytes it was (`tests/collision_draw_tests.cpp`, [Seeing the collision](#seeing-the-collision)).
- A tree collides by its wood: a frond's triangles leave its proxy, a walker passes through the frond and is stopped by the trunk ([What gets no body](#owned-data)).
- The understory collides by a shrub's stout stems alone: a walker passes a frond (a part of no wood) and a 9 mm stem (no flag), 24 triangles leaving the proxy, and is stopped by a 22 mm stem flagged a collider at its face less its radius (`scene_collision: a walker passes a fern, a tuft and a thin stem and stops at a hazel's`).
- A vine's or a cover's companion gets no body: a slab across the path all on part 1 under an empty part 0 keeps no proxy triangle, its twelve left out, and straight down where it stands meets the floor (`scene_collision: a vine's or a cover's companion gets no body at all`).
- A proxy is the coarsest cut within the error: at 0 the leaves (8,192 triangles of a ball), at 5 cm 510, whose surface stands within 5 cm of the leaves' along eight rays.
- A proxy cut from the collision's own copy, after the scene's floats are released, is the old cut of the floats word for word, and a walker is stopped by it; a cut asked under the copy's floor cuts the floor and is counted ([The collision's own copy](#the-collisions-own-copy)).
- A streamed scene's **prefix** collides as its instances too (W88, 2026-11-07): what the load keeps out of the streamed sources — an actor, a deformed instance, a mesh whose reach passes a tile (`kept_reach`, so a 50 m cliff slab) — stays in `scene.instances`, and `create` bins those by the tiles their bounds reach whether or not the scene streams; a tile's compound then holds the streamed entries' placements and the bin's instances together. The binning was bound to "a scene read whole" because the streamed case was written when the prefix held nothing a walker could meet; the owner walked through the cliff. Skinned instances, the terrain mesh and water still have no static body.
- A streamed scene collides as each generator's tile, asked in the inner ring's representation; a generator the executable does not carry is refused with the registry's sentence; a streamed placement far out collides where its generator put it ([Far from the origin](#far-from-the-origin)).
- Handed a tile source of the test's own — a plane the provider does not make — the ground held is that source's, at every point asked, and not the provider's.
- A moving ground stays within `ground_error_m` of the drawn one after every refresh, and the walker's tile within `k_underfoot_error_m`, under a travelling ripple; a still ground is never refreshed.
- A walker goes with a ground rising and falling at the renderer's per-frame bound, is never under the collision ground after a refresh, and a jump the rising ground catches is lifted onto it; on still ground `follow` is `refresh` (above).
- On the erg in time-lapse and at the game's own rate, standing at the owner's start at 600, and on rising sand at every rung of the dune ladder (`tests/time_lapse_tests.cpp`, where the terrain capability is): above.
- At 419 km, 10,000 km and 1e8 m the ground held is the origin's to the bit on and between the lattice points, the backend's field is the origin's, and a walk across a tile's edge is the origin's to a millimetre ([Far from the origin](#far-from-the-origin)).
- The ground built ahead of the ring changes no body: a 239 m walk across 30 tiles with the builds ahead and without has the same 4,800 character states; a tile the ring activates while its build is pending costs that tile's build at most ([The ground built ahead of the ring](#the-ground-built-ahead-of-the-ring)).
- The placements built ahead change no body: the same walk over 24 slabs a tile at four scales, stepped onto all the way, has the same 4,800 states with the generators' tiles, the proxies and the compounds built on a worker; and a tile left before activation leaks no shape — the physics world's table holds the held tiles' shapes and nothing else, and alive outside it are the cached proxies and the pending builds' compounds, none once the consumer is gone ([The placements built ahead](#the-placements-built-ahead)).
- The tunables' defaults are the documented ones; the consumer's ring is one ring of the radius, with the budget.

## Public API

`include/systems/scene_collision/scene_collision.h`: `k_determinism`; the tunables' readers `spacing_tunable`, `radius_tunable`, `max_tiles_tunable`, `mesh_error_tunable`, `ground_error_tunable`, `max_refreshes_tunable`, `prefetch_tunable`, and since T166 `prefetch_workers_tunable`, `lead_tunable`, `mesh_copies_tunable` and `k_lead_max_tiles`; `k_underfoot_error_m`; `Config` and `config_from_tunables`; `ring_params_from_tunables`; `GroundTime`; `Stats` (with `carries`, `max_carry_m`, `lifts` — past a tenth of a millimetre, below which it is a step's gravity settling — and `max_lift_m`; the builds ahead's `prefetch_*`, `placements_ahead`, `ahead_ground_ns` and `ahead_placement_ns`, and the kept copies' `mesh_copy_hits`, `mesh_copy_reads` and `mesh_copy_bytes`); and `SceneCollision` — `create` (with the tile source its ground comes from, when not the provider's, and the job system the ground is built ahead on), `consumer`, `set_ground_time`, `prefetch` (before the ring's update), `refresh` (at a `WorldPos`, with the walker's `reach`), `follow`, `ground_height` (under a `WorldPos`), `stats`, `streamed`, and what is held for a debug draw — `held_tiles`, `held_tile` (`HeldTile`: the corner, the lattice and its heights), `held_piece` (`HeldPiece`: the mesh, its instance or entry, its place and the proxy's triangles and parts, with `Config::keep_shapes`) and `lattice_hole`. `proxy_source.h`: `ProxySource` (`build`, `add`, `mesh`, `clusters`, `emptied`, `points`, `sources`, `triangles`, `parts`, `bytes`, `counts`) and `k_copy_scale_headroom`, the collision's own copy `create` takes as `proxies` ([The collision's own copy](#the-collisions-own-copy)); `Stats::proxies_coarsened`. `collision_draw.h`: `DrawLine`, `DrawCapsule`, `DrawOptions`, `DrawStats`, `body_color`, `collision_lines` (the lines on the CPU), `CollisionView` (a consumer of its own round a point) and `CollisionDraw` (the frame pass: `create`, `set`, `follow_eye`, `set_capsule`, `destroy`). Positions are `WorldPos` ([Far from the origin](#far-from-the-origin)); the walker is a `physics::CharacterBody`, whose feet are too.

## Depends on

`base`, `containers`, `math`, `hash`, `jobs` (the ground built ahead), `log`, `time`, `tunables`, `schemas` (a streamed scene's `engine.scene.WorldRings`), `geometry` (the DAG a proxy is cut from), `gfx` (the scene's instances as uploaded), `scene_gen` (the ground's interface and the generators' registry), `renderer` (the scene as loaded), `physics`, `world` (the ring and its consumer table) and `sim` (the observer set). `engine_capability_requires(scene_collision physics world)`: with either off — the minimal build, or `ENGINE_WITH_PHYSICS=OFF` — this is off too, and engine-view's walker follows the ground instead ([apps](apps.md#walking)).

## What a module can ask

**The collision is what a game module's casts and supports read** (roadmap A43; hotload ABI 10, [apps](apps.md#what-a-module-can-ask-the-world)). A cast is the physics world's own ray or sphere against the static bodies this consumer holds; the consumer's part is to say **what it met** and **where a small animal can stand**, from the tiles it holds.

- **What a cast met** (`describe_hit(body, child)`): the held tile whose ground or placements the body is, and for a tile's compound the piece the hit's `child` is — `physics::RayHit::child`, the compound's child in the order it was built with ([physics](physics.md#which-piece-a-cast-hit)). That order is the tile's piece records', so **every tile keeps its pieces' records now** (`Tile::pieces_held`: mesh, instance, entry, offset, rotation, proxy — 48 bytes a piece), where until A43 they were kept only with `Config::keep_shapes` for the debug draw; `held_tile`'s `pieces` still reads 0 without `keep_shapes`. `covers(at)` and `covers_segment(a, b)` say whether held tiles cover a point and a segment's (x, z) (sampled every eighth of a tile), which is how a cast's answer says whether a miss means a clear path.
- **What it sees** is what stops a walker, with nothing added: the ground's heightfield and each placement's proxy, a tree by its wood (above, "What gets no body"). A crown's leaves and twigs have no body and a cast passes them; a trunk and the parts the structural table marks load-bearing or a collider stop it. What lies outside the held tiles is not seen at all.

**Supports** (`supports_near(at, radius, min_size, out)`): the places within `radius` of a point, over the held tiles' pieces, at least `min_size` large, the nearest `out.size()` of them, nearest first — ties in the tiles' order (sorted by key) and each tile's piece order, so the answer is a function of the held tiles and not of the order they came in — with `found` counting every one and `covered` whether the disc's bounding square lay over held tiles. **What the two numbers mean** (A45): `radius` is a distance in three dimensions from the point asked to the support's own point on the surface, so a limb 10 m up is not near a point on the ground under it, and a piece is looked at only when its bounding sphere comes within `radius` of the point; `min_size` is compared with `size_m`, which is **a width a foot spans** for every kind — a perch's declared size, a limb's diameter at the point, a top's bounds' smaller width across (until A45 a top's was half that, so `min_size` asked twice as much of a top). A piece's supports come from **one** of three sources, the first it has:

1. **The perches its mesh declares** — `renderer::SourceMesh::perches`, the container's section 43 or a glTF's nodes ([geometry](geometry.md#perches), [assets](assets.md#perches)) — at the piece's place, rotation and scale: the point, its `up` (turned as a normal is under a non-uniform scale) and the declared size times the scale's smallest axis.
2. **The parts of its structural table** (a generated tree's section 37 with the motion parts of 35): every part with a radius whose axis, turned into the world, lies within 60° of level — so the trunk and the risers are left out — at the point of the axis nearest the asker, moved onto the axis's top by the radius there (the world's up less its share along the axis), the diameter the size and the part's id the `part`. Load-bearing or not: the table's flag is for what may hang from a part, and a bird perches on a branch a walker could not stand on. Where the tip is thinner than `min_size`, the point is moved toward the base to where it is not.
3. **The top of its proxy's bounds**: a ray straight down the bounds' top centre from a quarter metre above, onto this piece itself (the tile's compound body and this child, or no support), kept where the surface there is within 40° of level (the walker's slope limit, `WalkParams::max_slope_deg`), the smaller horizontal width the size.

**Which way a limb runs, and which piece it is** (A45, hotload ABI 12). A limb's support carries `along`, the part's axis turned into the world, unit, from its base toward its tip, so a bird can sit along the branch or across it; a perch and a top carry zero (any way round). Every support carries `placement`, the piece's name among every piece of the world (`support_placement`): a scene instance's index; a streamed placement's entry with the name its generator gives it (`Placement::id`, the `authored` entry's place in the scene file); else its entry, its tile and its place among the entry's placements on that tile, hashed, with the top bit set — the same whenever and in whatever order the tile comes in, because a generator's tile is a function of the tile. Until then a streamed placement's support named only its entry, so every tree of one entry gave one name to its limbs of the same part and every rock of it one name to its top, and a module's reservation book took one bird on any of them for all of them. Each piece record keeps the two `u32`s that name it (`PieceRecord::id`, `index`), 56 bytes a piece where it was 48.

**A tree with neither** — a mesh whose motion contract is of the parts kind but which declares no perch and carries no structural table — offers a module its top alone, and the consumer says so once, the first time a module asks near it (`"a tree with no structural table and no perch offers a module its top alone"`, with its mesh, entry and instance). Every tree the flora solvers build carries the table since A45 (the broadleaf and the conifer were the two that did not; [flora](flora.md#the-structural-table)); a palm still has none and gets its top. **An imported tree** (a glTF or a bought container) has no motion contract at all: it gets the perches its file declares (a node named `perch…` or carrying `extras.perch.size_m`, [assets](assets.md#perches)) and, without them, the top of its bounds — the way to give a bought tree its branches is to declare them.

**A streamed world answers as a scene of instances does.** The forest proof of concept reported every support kind at zero with 416 animals in its streamed forest (its need 94). Reproduced against this consumer it was not the consumer's: `world_query_tests.cpp`'s streamed case (the same tree, post and boulders as placements of a generator, one boulder sharing the tree's tile) finds the limb, the perch and both tops on the code before A45 too, and the forest run's own summary reads `"support_queries": 0` — its module asks only for a bird within its own range of the walker, and none was. Its second counter ("Support sources (scene instances, bits)") counts instance answers alone, which a streamed world never gives.

**Why on the proxy.** The supports of one mesh at one scale are the same for every instance, so they are computed once with its proxy — the bounds of the triangles the proxy was built from, and the perches or the limbs scaled into its frame (`Proxy::supports`, `lo`, `hi`) — and a query only turns them by each piece's place: a rejection by the piece's bounding sphere, then a handful of perches or a tree's few hundred parts. A piece with no proxy (no surface to stand on) has no supports, which keeps "a place to stand" and "something that stops a walker" one idea.

**What it costs.** Nothing per frame and nothing while nobody asks: the records and the supports are built with the tile and the proxy. A query reads the held tiles (nine at the default ring) and allocates nothing. Measured in `world_query_tests.cpp` (no device, three pieces; 2026-11-14, the machine shared with other agents' builds and GPU runs, so upper bounds): a thousand rays in one step **0.40 ms in `msvc-release`** (0.4 µs a ray; 17.5 ms in `msvc-debug`), a thousand supports queries **0.81 ms** (28 ms in `msvc-debug`; 1,146 supports returned). Taken again with A45's records (`along`, `placement`, 56-byte piece records; 2026-11-15, `msvc-release`, the processors 71 to 97 % busy with other agents' builds, so again upper bounds): a thousand rays **0.60 ms**, a thousand supports queries **1.25 ms** (1,146 supports returned) — no slower than A43's beyond what the load explains. In a session (apps.md's end-to-end case) three queries — the orchard tree's 25 parts within 5 m the dearest — took 34 µs together in `msvc-release`.

**Testing** (`tests/world_query_tests.cpp`, no device): a tree of a trunk, a horizontal limb and a leaf slab (its motion contract and structural table in memory), a post declaring a 4 cm perch, and a ball for a boulder, on a floor. A ray stops at the trunk's face (3.8 m from 4 m out) and names the tree's instance; straight down through the leaf slab it reaches the floor; down onto the limb it stops on the limb's top; a 0.25 m sphere at the ball stops a radius short of it. The supports near the limb are the limb's top above the axis point nearest the asker, never the trunk, and none for a foot larger than the limb; near the post its perch alone; near the ball its top within the 5 cm proxy's sagitta of 1.4 m and facing up; from the middle all three, nearest first, `found` past the capacity. The same world streamed (A45: each piece a placement of the test's own generator, a second boulder sharing the tree's tile so a top is found on a compound of several children) gives the limb with its axis (+x) and the tree's placement name, the perch with no axis, both tops with their width across (1.5 to 1.6 m for the 1.6 m ball cut within 5 cm) and two names for the two rocks. The answers — five casts and three supports queries, `along` and `placement` included since A45 — are pinned by a hash of their bits, which GCC and Clang in the Linux containers reproduce.

## The tunables

Read once, by the host, into its walk header (engine-view's `walk.collision` block), so a replay collides with the recording's numbers.

| Tunable | Default | What it is, and what it costs |
|---|---|---|
| `walk.collision.spacing_m` | 1.0 | The ground heightfield's sample spacing. A metre is 34 × 34 samples a 32 m tile: 4,624 bytes of heights on the CPU (13,872 with a time-lapse's two fields) and 4,400 in the backend, measured below; half a metre is four times both. The erg's drawn grid is 1.5 m (0.5 m round the camera with `--terrain-rings`), so a metre is finer than what is drawn without the rings, and the walker's ground error against the drawn grid is in engine-view's summary |
| `walk.collision.radius_tiles` | 1.5 | The ring's radius, in tiles, to a tile's centre: the 3 × 3 tiles round the walker's own at a tile's centre, four at a corner — at least 25 m of ground in every direction, and 9 tiles held at most |
| `walk.collision.max_tiles` | 2 | Tiles made an update, the budget (twice as many let go): a walker crosses a tile in 21 s at a walk and 6 s at a sprint, and a tile's ring of three comes in as it approaches, so two an update spreads a crossing over two ticks. The first fill round a camera a walker is dropped from is unlimited |
| `walk.collision.mesh_error_m` | 0.05 | How far a proxy may stand off its mesh |
| `walk.collision.ground_error_m` | 0.05 | How far the collision ground away from the walker may stand off the drawn one under a time-lapse before its tile is rebuilt; under the walker it is `k_underfoot_error_m`, a millimetre, which is not a tunable (above) |
| `walk.collision.max_refreshes` | 2 | Tiles of refresh work a tick under a time-lapse, besides the ones under the walker |
| `walk.collision.prefetch_tiles` | 1 | How far past the ring, in tiles, a tile's ground is built ahead on the job system ([The ground built ahead of the ring](#the-ground-built-ahead-of-the-ring)); 0 builds nothing ahead. Not in the walk's header: it changes no body and no step, so a recording replays the same whatever it is |
| `walk.collision.prefetch_workers` | 4 | The workers engine-view's walker gives the builds ahead, unpinned ([Ahead of a running walker](#ahead-of-a-running-walker)); the slots are twice this. Not in the walk's header, for the same reason |
| `walk.collision.lead_s` | 4 | Seconds of the walker's velocity the builds also look along, at most 4 tiles; 0 builds round the walker alone. Not in the walk's header |
| `walk.collision.mesh_copies_mib` | 64 | MiB of tile-handed meshes' copies kept between builds; 0 reads a tile's meshes for every build. Not in the walk's header |

## What it costs

**Bodies.** At most two a tile — the ground's heightfield, and one compound when the tile has pieces — so nine tiles are at most 18 bodies however many pieces they hold; the long-walk test's world never held more.

**Bytes** (`physics::World::shape_memory`, measured by the tests). At the default metre a tile's ground is **4,400 bytes in the backend and 4,624 of heights on the CPU** — 81 KB for nine tiles — and three times the CPU's under a time-lapse, which keeps the drawn pair's two fields beside the body's heights. A compound is about 110 bytes a piece (three of nine pieces: 984 bytes), so a building of the kit of boxes laid in sections, about 88 pieces, is some 10 KB; its pieces' proxies are about 16 bytes a triangle (two meshes of 268 triangles: 4,256 bytes), held once per mesh however many buildings use them.

**Time** (`msvc-release`, [bench](bench.md#measuring-on-a-shared-machine)). A ring change — the walker a tile further on, three tiles made (a 34 × 34 heightfield and a compound of a hundred blocks each), three let go, and the broad phase rebuilt — is **618 µs median and 375 µs at best** (`scene_collision.ring_change`, seven repeats, 2026-09-28; the machine was 95% busy with this change's own builds when the run started and 17% when it ended, and another agent held the GPU lock, so both are upper bounds): about 0.2 ms a tile made, so the default budget of two a tick costs under half a millisecond on the one tick in several seconds that crosses a tile's edge. A tick that changes no tile — the ring's update from the feet and a still ground's refresh returning at once — is **1.09 µs** (`scene_collision.tick`, same run). With the placements' shapes built through `BuiltShape` and adopted (2026-10-24, W71; the bench builds nothing ahead, so this is the path a tick that builds its own tiles takes): ring change **386 µs** median and 385 at best, tick 1.10 µs, a frame of moving sand 140 µs (others used 15–22% of the CPU and another agent held the GPU lock: upper bounds). **A frame of moving sand under a standing walker** — the blend on by two millimetres, the walker's tile rebuilt at it (its heightfield and body, the broadphase after), the walker carried, and the other tiles rebuilt as the 5 cm rule comes due, a third of one a frame — is **121 µs median and 119 at best** (`scene_collision.follow`, 2026-09-29, a repeat 130 and 120; others used 12% and 19% of the CPU and the GPU was 7–13% busy, so upper bounds): the frame's first tick pays it, and at 600 on the erg the walker's tile comes due every frame or few. Under a time-lapse a tile's refresh adds two evaluations of the ground's field over the tile's samples when its pair changes, which for the dunes is the terrain capability's cost and is not measured here: engine-view's summary reports the walker's field evaluations and its largest refresh in milliseconds ([apps](apps.md#walking)).

### The ground built ahead of the ring

**The expensive half of a tile is built before the ring takes it; the bodies are still made on the tick** (2026-10-24, roadmap W70 for the ground, W71 for the placements). `prefetch(at)`, which the walker's host calls once a tick before the ring's update, has every tile whose centre is within `Config::prefetch_radius_tiles` of the walker — engine-view sets the ring's radius plus **`walk.collision.prefetch_tiles` (1)** — and is neither held nor already built, its ground's heights and its placements' shapes ([below](#the-placements-built-ahead)) built on the job system `create` was handed (engine-view's walker owns a job system for it: one unpinned worker until T166, four since, [Ahead of a running walker](#ahead-of-a-running-walker)). When the ring activates the tile, the activation takes the build:

- **finished**: taken as they are (`Stats::prefetched`);
- **running on the worker**: the tick waits for that tile's build alone (`prefetch_waits`, `prefetch_wait_ns`, `max_prefetch_wait_ns`) — never for the queue behind it, and never for anything the renderer's rebuild is doing;
- **not started**: the tick claims it and builds it itself (`prefetch_inline`), which is what the activation cost before, and the worker skips it when its turn comes.

**Why the bodies stay where they were.** Body creation order decides body ids, and body ids decide a character's contact order and so a recorded walk ([Why this shape](#why-this-shape)); the heights are an input the tick does not decide. So the build ahead is exactly the call the activation would have made — the same source, the same time, the same window — and the bodies it makes and their order stay on the tick the ring activates the tile: `scene_collision: the ground built ahead of the ring changes no body` walks a character 239 m across 30 tiles with the builds ahead and without, and its 4,800 states are the same. A build is made only where the ground is **still and read through a tile source** (an authored set, a host's source), or where there is no ground: a moving ground is evaluated at the drawn time the tick sets, and a provider's own `grid` is not one a worker may share — nor, then, is the ground a generator is handed to stand its placements on. Builds the walker left behind (past the radius and a tile) are let go, and a build is freed only after its job has returned. `scene_collision: a tile activated with its build pending waits for that tile alone` fills a ring at once while a worker is still building 21 tiles from a source that takes 40 ms a tile, builds past the ring held shut till it ends, and the tick asks the source only for tiles it builds (T199).

### The placements built ahead

**The same build carries the tile's placements** (2026-10-24, roadmap W71). W70 left them on the tick, and with them the half of an activation that grows with what a tile holds: after the ground's reader stopped stalling, a tile of the forest POC's elder grove still cost its tick 7.8 ms, nearly all of it placements ([the measurement](../experiments/walk-stalls-2026-10-24.md)), and a tile of the streamed v8 forest (W73) holds a grove's trees, each at its own scale and so its own proxy. Now the worker that builds a tile's heights also:

- **asks each streamed entry's generator for the tile** (or takes the whole read's bin): beside the other builds where the generator says its `tile` is reentrant, and otherwise under one lock for every generator, one thread at a time, the worker's or the tick's, since such a generator's state may be scratch it writes ([Ahead of a running walker](#ahead-of-a-running-walker), T169);
- **finds or builds each piece's proxy**: the DAG cut at 5 cm and welded, and its mesh shape built by the backend (`physics::World::build_mesh`, [physics](physics.md#shapes-built-on-a-worker)) — the expensive half, a bounding-volume tree over the cut's triangles — into the cache, one proxy per mesh and scale, under the cache's lock, whichever thread asks first;
- **builds the tile's compound shape** over those proxies (`build_compound`), its pieces in the order the activation would have placed them.

**What stays on the tick**: the compound's shape is adopted into the physics world's table (`adopt_shape`, a slot and a reference), and the ground's heightfield shape and both bodies are made, ground first, in the ring's order — the activation's order, so the body ids and every contact a walker makes are the same: `scene_collision: the placements built ahead of the ring change no body` walks 239 m over 30 tiles of 24 slabs each, at four scales, stepping onto and over them, with the builds ahead and without, and its 4,800 states are the same (the tick's placements 6.8 ms over the walk without, 3.4 ms with, the rest the worker's 5.0 ms, in a debug build; the 3.4 are the activations whose build had not started, which the tick builds itself). Nothing in a shape depends on the thread that built it: the cut is a function of the mesh, the scale and the error, the backend builds the same tree from the same triangles, and the compound's children are in the generator's order.

**The leak rule.** A shape a build made is a `physics::BuiltShape`, which is in no table and holds the backend's reference: a compound a tile took is adopted and the build's reference dropped; **a build dropped before its tile was activated releases its compound with it**, once its job has returned; the proxies are the consumer's cache, kept for its life and released with it. So the physics world holds, at any time, the held tiles' heightfields and compounds in its table and, alive outside it, the cached proxies and the compounds of builds still pending — and nothing else. `scene_collision: a tile left before activation leaks no shape` holds it by the world's counts (`shape_count`, `built_shape_count`): builds round a camera finish, the ring takes nine tiles (nine compounds in the table, the others' still the builds'), the walker jumps 3 km and the builds left behind are freed with their compounds (four built shapes alive: the proxies), the ring there holds nothing, the walk back builds the nine again from the cached proxies, and with the consumer gone nothing it built is alive.

**Ahead of a walk that has not started.** A switch to walking drops the walker and fills its ring at once, on the tick it was pressed, so a build ahead queued on that tick is mostly not started when the ring takes it and the tick builds it itself — after the POC's flight to the elder grove, three of the seven tiles. So engine-view's walker also builds ahead **while flying**: every flying tick hands the camera to `prefetch` (`view::Walker::look_ahead`, [apps](apps.md#walking)), whose once-a-metre rule and radius are the walk's, so the tiles under the camera are ready when F is pressed and builds the flight leaves behind are let go. It makes no body and changes no step, so a recorded session replays the same.

**What it costs the tick now**: an activation whose build was ready spends its ground's move, a slot for the compound and the two bodies; one whose build was running waits for that tile alone; one whose build had not started builds it, which is what every activation cost before. The worker's time is in `Stats::ahead_ground_ns` and `ahead_placement_ns`, off every tick, and engine-view's summary carries both ([apps](apps.md#walking)).

### Ahead of a running walker

**A player's walk outran the build ahead** (T166, 2026-11-16). On the owner's walk of the forest proof of concept on batch 66, eight frames, 3.7 s in all, were this consumer building tiles on the frame thread, the worst 1,039 ms. Two kinds, read from the frame records' walk blocks:

- **A drop after a fast flight.** F pressed at 170 to 800 m/s fills the walker's ring at once (`Walker::drop`, the ring's unlimited first fill): 4 to 7 tiles, none ready — a flight at that speed leaves every build round the camera behind — so the tick built them one after another, 125 ms a tile on average and up to 500.
- **A sprint behind such a drop.** At 5 m/s a tile is six seconds of running, and the radius plus a tile should have had it ready; but one worker held about twenty builds the drop's radius had queued **in tile order**, so the tile the runner reached next, 3 to 4 s later, had not started, and the tick built it (108 to 248 ms).

A forest tile was dear because each build read every tile-handed mesh's whole container again (its scratch kept the last one alone): about 100 MB and 34 to 135 ms of a tile's 170 to 500 ms in the startup trace's `cluster read`, a few tens more hashing the recipe (`generator inputs`).

**What changed** (every one of them changes no body, so a recorded walk replays as before; the tests below hold it):

- **Several workers.** engine-view's walker gives the builds `walk.collision.prefetch_workers` (4) unpinned workers, where it gave one, so the ring a drop fills is built on all of them at once and the tick builds one of its tiles itself (`take_build`): a fill takes about as many tiles' time as the ring has tiles over the workers and the tick, not one per tile. A scene mesh's proxy is now cut and built outside the proxies' lock, as a tile-handed mesh's always was, so four workers building a forest's trees — each at its own scale, so each its own proxy — do not take turns behind it.
- **Handed over by need** (`Config::prefetch_slots`, which the walker sets to twice its workers). A build waits in the consumer until its turn and is handed to the job system most urgent first: its need is the time until the walker, going on at its velocity, comes within the ring's radius of the tile's centre — zero for a tile the ring wants now, and those are always handed over at once — and a tile off the walker's line waits by its distance at the walker's pace. At most the slots are waiting in the job system or running there, so a tile the runner reaches next is never queued behind tiles beside it, and a fast flight's builds do not pile up behind each other: a build the flight left behind is let go before any worker reaches it.
- **A lead along the walker's velocity** (`Config::lead_s`, `walk.collision.lead_s`, 4 s, at most `k_lead_max_tiles` = 4 tiles): `prefetch` takes the walker's velocity — engine-view's, from its place a tick ago, walking or flying, and zero at a drop — and builds round the point that far ahead as well as round the walker.
- **The meshes a tile hands are kept between builds** (`Config::mesh_copy_bytes`, `walk.collision.mesh_copies_mib`, 64 MiB): a build reads a recipe's container once and keeps of it only what a proxy is cut from — the collision's copy (`ProxySource::add`, [The collision's own copy](#the-collisions-own-copy)) at a floor `k_copy_scale_headroom` finer than the scale it was first asked at, and the perches, structural table and motion parts the supports read — so the next tile's placement of the same recipe, at a scale up to four times larger, is cut from the copy, word for word the cut a fresh read gives (proxy_source.h, "The floor"); a larger one reads it again and replaces the copy. The least recently used copies nobody is cutting from go first past the budget, and `Stats::mesh_copy_hits`, `mesh_copy_reads` and `mesh_copy_bytes` count them.

**Measured on the forest's staging** (2026-11-17, a pair in one lease: a replayed sprint, a flight at 800 m/s and a drop, twice — the smallest flight that reproduced the owner's drop, an input log of 32 key events replayed offscreen with `--replay-input`, 2,460 frames at 3840 x 2160, `--raster rt --shadows rt`, the bounce on, `--terrain-async`; the Windows session locked, other processes 44 and 48 % of the CPU and the GPU 99 to 100 % busy, so upper bounds): the drop's fill of seven tiles **962 ms before, 339 ms after** — seven built on the tick before; two ready, three waited for (126 ms) and two built on the tick after — and the run's worst frame 979 to 352 ms, frames over 33 ms 50 to 39, the median 17.24 to 17.20 ms. The second drop and both sprints found their tiles ready on either build. The engine's own streamed forest does not reproduce it: its procedural ground has no tile source, so nothing there is built ahead, and its 28 activations took 18 ms in all.

**Side by side** (T169, 2026-11-17). After the above, a drop at 800 m/s still cost 339 ms, because nothing the flight passed is a tile the ring then wants and the four workers' builds of the ring took turns at one place: `gather_placements` asked every generator's `tile` under one lock for every generator, since a generator's state may be scratch it writes (the ruins' assemblers were). A generator now says at registration whether its `tile` may be asked on several threads at once (`scene_gen::PlacementGeneratorDesc::tile_reentrant`, [scene_gen](scene_gen.md#a-tile-asked-on-several-threads)); a reentrant one is asked on the thread building the tile, beside the others, and one that says nothing keeps the lock, so nothing outside the tree changes. Every engine generator with a `tile` says so but two another row held that night (the channels, the village), and the ruins by a change: a call borrows its assemblers from a spare list. The bodies are untouched — still made on the tick in the ring's order — and the tile is the same bytes: the generator's answer is a function of its state, the tile and the context, the proxies are keyed by mesh and scale whoever builds them, and the compound's children are in the generator's order. On the 40 ms tile below, a ring of nine took 430 ms under the lock, one call at a time, and 88 ms side by side, five calls at once on four workers and the tick (2026-11-17, a debug build beside other agents' builds: upper bounds). The forest's drop pair was left for the merge's gate to fly (T166's replayed drop, the T166 build against this one).

**What is left**: a drop at 800 m/s still waits for its ring, now about one tile's build over the threads that build it rather than the generators' sum; the next step is a drop whose ring the walker waits for with its body held where it was, counted in ticks the input log can replay, or tiles cheaper still.

**Outrun** (T172, 2026-11-17). The owner's walk of the forest on batch 67 stalled 1,410 ms at a drop: a flight at 1,375 m/s, a hover of 0.83 s, F. Five of the ring's seven tiles were ready, one was waited for (27 ms) and one built on the frame thread in 1,360 ms, while the four workers built others in 0.25 to 1.1 s each — and at the hover's start all four were still on builds the flight had asked for and passed. On that world a build took 0.4 s at the median (87 builds, 37.6 s of wall time and 11.7 s of processor: the threads waited most of it), and a camera at 1,375 m/s crosses the builds' reach, 2.5 tiles of 64 m each way, in 0.23 s: every build a fast flight hands over is finished after the camera has gone, and holds a worker the hover then wants. So `submit_builds` hands **nothing** over on a tick whose velocity crosses the reach's diameter (`2 × prefetch_radius_tiles × tile`) in less time than a build takes here — an estimate (`Stats::build_estimate_s`) of the builds that ran, each weighed a quarter against the estimate before it — counted in `Stats::prefetch_outrun`. Builds already handed over run on; the flight lets go of the ones no worker has reached, as before. A walker (a few metres a second) and a drop (no velocity) are never outrun, and before any build has run nothing is. It decides only which builds are made and when, so no body changes and a recorded walk replays as before. `scene_collision: a flight that outruns the builds hands none over` holds it with builds of 40 ms: nothing handed over at 20 km/s, the same place's builds handed over at a walker's pace and at a drop.

**Where a build's time goes** (T172): the startup trace names, inside each `collision build`, every generator's `tile` (`collision generator`, the generator's name as the detail) and, for one that is not reentrant, the wait for the generators' lock apart (`collision generator lock`), a scene mesh's proxy cut and built (`collision proxy`), a tile-handed mesh's (`collision tile proxy`, around the kept copy's read), the copy of the scene's exclusion an entry's shapes are added to (`collision exclusion`) and the compound (`collision compound`). The owner's trace named 200 ms of those 37.6 s.

**The pair** (T172-2, 2026-11-18, one lease, the session locked, others' processor 73 to 94 %): the staging cut round the owner's drop, the owner's first 15 s replayed. Main's code: the drop's frame 1,016 ms, one tile built on the frame thread in 995 ms, six ready, 161 builds handed over. With the outrun rule: 594 ms, nothing built on the frame thread, one tile waited for 574 ms, six ready, 101 builds handed over. The rule took the inline build away; the wait that is left is a worker's build of the same tile, started at the hover, still running when the camera lands — still a stall a person feels, and nearly all of it the sward's mask reads (below).

**Where the sward's tile waits** (T172-2). On the staging cut the sward's `tile` was 32.5 s of the 46.9 s of 75 builds, at 28 % of a processor: blocked most of each call. Each call now ends with a `sward split` stage of no length whose detail is the call's wall time by part — the mask reads (and how many), the site, the ground, the exclusion, the forbs and the rest ([flora](flora.md#grass-sward-g90)). In the pair's second flight, of the sward's 28.2 s in 89 collision builds, **27.8 s were mask reads**: 512 a call, 305 ms a call at the median; the site, the ground, the exclusion and the forbs together 0.33 s. The same 512 reads in the rendered world's calls of the same flight took about 1.4 ms a call. The mask level ([tileset](tileset.md)) answers every reader through one lock and one read window of about 2 MiB, refilled under that lock whenever a tile is outside it; the sward read each of a 64 m tile's 64 mask tiles (8 m on the forest's level) once per row of cells, and the collision's four workers, on tiles far apart, each found the window moved and refilled it under the lock the others waited on. A call now reads each mask tile it covers once. A pair of its own in one lease (the same cut and flight, locked, others' processor 25 % then 42 to 49 %): the sward's call in a collision build 199 ms at the median before, 59 ms after; a build 354 ms before, 122 ms after (104 and 162 builds); the drop's frame 290 ms before (a wait of 271 ms), 118 ms after (99 ms); the flight's waits 1,140 ms before, 382 ms after. The 64 reads a call that are left still take 56 ms of each call's 59: the level's one window, refilled under its one lock for whichever reader is outside it, is the next thing to change, not the sward.

**What a walker stands on when it outruns all of this**: what it always stood on. A tile's bodies are still made on the tick the ring activates it, in the ring's order, so a walker never stands where its ring has no body and never falls through the ground; a build not started is built on the tick (`prefetch_inline`) and one running is waited for (`prefetch_waits`), and both are in every frame record's walk block ([apps](apps.md#walking)) — the inline build is the last resort, and it is counted.

**Testing.** `scene_collision: builds ahead of a runner, on four workers by need, change no body` walks the slabs' 239 m with four workers, the slots and a 4 s lead, and its 4,800 states are the ones whose ticks built everything themselves. `scene_collision: a drop's ring is built on every worker at once` fills a ring of nine tiles from a source that takes 40 ms a tile with one worker and every build queued, and with four workers by need: the ring's tiles alone are handed over, and three build at once (T199). `tile_mesh_tests.cpp`'s `a tile's mesh cut from a kept copy is the proxy a fresh read gives` walks rocks handed at two scales with copies and without: the same proxies, triangle for triangle and byte for byte, and the same rays' answers to the bit. `scene_collision: a drop's ring asks a reentrant generator side by side` fills that ring of nine with the 40 ms in the generator's `tile`, once under the lock (at most one call at once, and nine calls' 40 ms at least) and once reentrant (two; T199). `scene_collision: a ring built side by side on eight threads is the ring built alone` builds two rings of 21 tiles over the slabs and the tile-handed rocks on the tick alone and twice on eight workers: every held piece, proxy triangle and ray's answer the same, word for word.

### A tile's meshes

**A mesh a tile handed collides too** (R145, 2026-11-11; the contract is [scene_gen](scene_gen.md#a-tiles-meshes), the drawn half [world](world.md#a-tiles-meshes-residency-by-reference)). A generator's `tile` may hand, with its placements, meshes only that tile names — a ledge, a scree patch, a wall's run — and a placement that names one (`resident + k`) had no body: `gather_placements` skipped every index past the entry's resident list, so a walker went through whatever a converted solver placed. Now such a placement gets a piece like any other, and its proxy is:

- **keyed by the mesh's identity and the scale** (`renderer::placement_mesh_identity`: the generator and the recipe's bytes), never by where a tile listed it, so two tiles handing one recipe share one shape and the proxy a walk finds does not depend on the order its tiles came in;
- **built from the recipe, with no device and no GPU scene**: `renderer::load_placement_mesh` under the scene's own options (`SceneData::mesh_options`) is the load's per-mesh pass — the mesh generator registry and the derived-data cache — so the collision builds into the cache entry the renderer's residency reads, and whichever of the two asks second pays a read. The cut is the one every proxy has (`cut_proxy`: the coarsest clusters within `walk.collision.mesh_error_m`, welded), from the mesh's own DAG rather than the scene's merged one;
- **built outside the proxies' lock**: a recipe on a cold cache is a solve, and the tick must not wait behind a worker's. Two threads may then build one key at once; the second finds the first's and drops its own. A build keeps the last mesh it loaded, so a tile's two placements of one mesh at two scales load it once;
- **resident by the collision's own tiles**: a gather takes one reference per proxy it names, the reference goes to the tile with its body and is given back when the ring lets the tile go — or when a build ahead is dropped unactivated — and a proxy nobody names is released with its shape (`Stats::tile_proxies`, and the triangles and bytes fall with it). That is the residency the drawn mesh has, by the collision's ring instead of the renderer's, and it is why this half of the cache does not only grow.

A mesh that will not build (a generator this executable does not carry, a recipe its generator refuses) has no body and says so once per build, with the generator's sentence; the tile's other pieces are unaffected. **What it costs**: `tile_mesh_tests.cpp`'s first case holds nine tiles' rocks — 18 proxies (each rock at two scales) of 3,946 triangles, 63,940 bytes of shapes and 2,592 of compounds — in a debug build beside other agents' work.

### What an activation costs

`Stats` splits an activation's wall time (2026-10-24, roadmap W70): `ground_sample_ns` (and its most once, `max_ground_sample_ns`) — the ground's heights asked of the tile source, or the provider's `grid`, on the tick; `placement_ns` — the placements gathered from their generators or bins, their proxies cut and built and the compound built, on the tick (since W71 zero for a tile whose build ahead did it; a build the tick ran itself counts here); `body_ns` — the heightfield shape, the compound adopted into the table, and the bodies the physics world made. A build's wait is `prefetch_wait_ns`, and the worker's own time `ahead_ground_ns` and `ahead_placement_ns`. engine-view's summary carries the totals and its frame records the frame's share ([apps](apps.md#walking), "The frame record's walk"). After a flight to the forest POC's summit, a first walking frame's 637.7 ms of activation was 636.8 ms of ground heights — the tile set reader's lock, held by the terrain's rebuild — and 0.9 ms of bodies ([the measurement](../experiments/walk-stalls-2026-10-24.md)).

## LOD policy and determinism stance

[ADR-0027](../adr/0027-additive-capabilities.md) asks for both, in writing.

**LOD.** Bodies exist only for the tiles of the walker's own ring — one ring of 1.5 tiles — so collision is LOD0 and nothing else, which is plan 05 §5.11's "bodies exist for LOD0/1 only" for static geometry; past the ring there is no body at all. Within it the proxy is the placement's own coarse level at a fixed error, not a distance-dependent one: a walker only ever touches what is within a metre of it.

**Determinism.** `k_determinism` = `hashed`. The bodies are a function of the ring's events, the scene and the numbers the host read once: the same ticks with the same walker make and let go of the same bodies in the same order, and the physics world gives them the same ids, which is what a replay needs (above). A generator's `tile` is itself a function of (entry, seed, tile, ground) ([scene_gen](scene_gen.md)). The one input from outside the tick is a moving ground's drawn time (above).

## Testing

`tools/dev.ps1 test -Filter scene_collision`: the cases above, all without a device; the erg's four where the terrain capability is linked. Where the tile-set capability is linked, `tests/tileset_ground_tests.cpp` walks on an authored ground: the `tiles` provider made through the registry from a built set, the set's own source handed to the collision as the renderer's is, the ring's heightfields the set's heights to the bit on their lattice and never outside the four points round a point between, and a character walked 10 s from a bowl's side across its floor and the tiles' shared edge with its feet within 10 cm of the set's surface (4.4 cm measured, the capsule over 3 cm of roughness at 50 cm; [tileset](tileset.md)). `tests/tile_mesh_tests.cpp` (R145; no device, 1.0 s together in a debug build) is a tile's own mesh: the fixture's rock a tile ([scene_gen](scene_gen.md#a-tiles-meshes)) built when the ring takes its tile and not before, a ray's hit on the dome's top, a capsule dropped onto it coming to rest 1.96 m up and one walked along the floor into it stopped at its edge, every proxy gone when the walker leaves and nothing alive once the consumer is; tiles handing one recipe sharing one proxy, and a rock wanted again read from the cache rather than built; and builds ahead that are never activated giving their proxies back. `tools/dev.ps1 bench -Filter 'scene_collision.*'`: a ring change, a tick that changes nothing and a frame of moving sand (above). engine-view's end-to-end walks ([apps](apps.md#walking)) are this consumer in its host.

## Not yet

- **The proxy cache of the scene's own meshes only grows** (a tile-handed mesh's proxy goes with its last tile: [A tile's meshes](#a-tiles-meshes)). A proxy is kept for the consumer's life, one per mesh and scale, and since W71 a build ahead caches the proxies of tiles the walker never takes too: after the POC's flight to the grove 238 proxies against 16, after its flight to the summit 504, about 1.6 KB a tree's trunk ([the measurement](../experiments/walk-stalls-2026-10-24.md#after-the-placements-were-built-ahead-too-w71)). A tree of a streamed forest is at its own scale, so the cache rarely shares one; letting go of a proxy no held tile and no pending build places — the backend's reference count says which — is the next step, done on the tick while no build runs.
- **Nothing is built ahead under a time-lapse, or over a provider's own grid** ([The ground built ahead of the ring](#the-ground-built-ahead-of-the-ring)): the erg's ruins under moving dunes still gather, cut and build their placements on the activation's tick. Their shapes do not depend on the time, so they could; what keeps them on the tick is the ground a generator is handed, which a worker may read only where it is a tile source's.
- **The character rig's contribution** — bone capsules against the bodies, foot placement, footprints stamped into the sand's deformation — is a layer over the capsule and these bodies, not a change to them.
- **Streamed ground tiles are not used**: the ground's heights are the provider's, sampled here, not the world's ground consumer's tiles with their deformation overlays ([world](world.md#the-consumers)); a footprint in the overlay does not dent the collision yet.
- **Nothing moves**: every body is static. A door, a platform or a falling block would be a kinematic or dynamic body of the physics world beside these.
- **The rings' and the tiles' finer drawn ground** (`--terrain-rings` or `--terrain-tiles`, 0.5 m round the camera) is followed in time but sampled at the collision's own spacing, so the drawn ground near the walker can be finer than the collision's: on the erg's tiles that is what the walker's 10 cm ground error is.
- **No overlap query or character-against-character** collision (physics.md, "What is not wrapped yet").

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph, the scheduler, or another capability, and it can be removed from the build the same way. Outside its directory it added one read to `domain/physics` (`World::shape_memory`, what the backend holds for a shape, which its costs are reported in) beside the character this capability's host walks, and (2026-10-24, W71) the shapes a worker builds and the world's thread adopts (`BuiltShape`, `build_mesh`, `build_compound`, `adopt_shape`, [physics](physics.md#shapes-built-on-a-worker)), which nothing about this consumer is in.

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | `engine_capability_requires(scene_collision physics world)` | declared; off when either is off |
| Component and event types | none: nothing here is persistent; the bodies are rebuilt from the scene | not needed |
| Tick scheduler entry | none: the ring updates between ticks, as every world consumer's does (world.md, "Between ticks") | not needed |
| Render-graph passes | the collision draw, a frame pass at `SceneRenderer::add_frame_pass` registered only when a host asks for it ([Seeing the collision](#seeing-the-collision)) | done |
| Content-build derived step | none: a proxy is cut from the DAG the scene already loaded | not needed |
| Protocol methods | none: nothing over `render.*` walks yet | not yet |
| Tunables | `walk.collision.*` (above) | done |
| LOD policy | one ring round the walker; the proxy's fixed error | done |
| Determinism | `scene_collision::k_determinism` = `hashed` | done |
| Zero cost when unused | no linked code without it; a host that never walks makes no consumer, no physics world and no body | done |
| Tests and size table | `tests/scene_collision_tests.cpp`, `tests/time_lapse_tests.cpp`, `tests/far_forest_tests.cpp`, `tests/collision_draw_tests.cpp`, `tests/size_table.cpp` (`GroundTime` 32 bytes) | done |
| Bench | `bench/scene_collision_bench.cpp`: `scene_collision.ring_change`, `.tick`, `.follow` | done |
| Removal proof | `ENGINE_WITH_SCENE_COLLISION`, off in the minimal build | works |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_SCENE_COLLISION=OFF`, or `-DENGINE_WITH_PHYSICS=OFF`) drops the module, its tests, and its bench; the module disappears from `build/<preset>/modules.json` and is listed there under `disabled_capabilities`, and engine-view's walker follows the ground ([apps](apps.md#walking)).
