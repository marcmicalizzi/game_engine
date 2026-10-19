# 14 — Structures: Buildings, Villages, and Architecture in Trees

Status: recorded 19 October 2026 from the owner's request for a fantasy-RPG proof of concept. This chapter is a plan; **nothing in it is built**. The numbers it quotes about today's engine come from the pages it links, and the ones it estimates are marked as estimates for the first measurement to replace. Its roadmap rows are G64–G70, D37, S89–S91, A42 and P48, and it is written against G63 (the structural table, in progress beside it) ([roadmap](../roadmap.md)).

## 14.1 The problem, and the stance

The owner's fantasy RPG lives in its own repository and pins the engine, as Desert Survival does ([13 §13.1](13-reference-consumer-games.md#concept), the direction note on repositories; [ADR-0047](../adr/0047-reference-games-are-licensed-apart-from-the-engine.md) for the licensing). Its first proof of concept is **a village with a forest**. After that come **elven villages that span trees**: buildings in and between exceptionally large trees, rope bridges, ramps, interiors, exteriors and roofs, plus ordinary villages and towns.

**The owner has no modelling skill and does not intend to acquire any.** Generative 3D is weakest at exactly this kind of content. Its size and its quality move in opposite directions. Of the Tripo corpus, 349 of 438 meshes passed E10's checks and the rest failed on topology; that figure is the owner's own, from a run whose numbers are not in this tree yet ([E10](../experiments/e10-generated-props.md) has the 20-prop pilot). A house, a platform clamped to a particular limb, or a bridge between two particular trees is something no prompt produces in place.

The stance follows from that:

1. **Structures come from solvers and parameters.** A structure is a recipe: a footprint, a style, its anchors, and a seed. A solver turns the recipe into cluster geometry, the same way a palm is solved from its `.flora.json` ([flora](../subsystems/flora.md)). Nothing is modelled by hand anywhere in the loop.
2. **Solved parts form a shared kit.** Beams, planks, posts, rope, thatch bundles and shingles are made by the solver, not imported. Every structure is assembled from them, so a village holds together visually and a fix to the plank fixes every plank.
3. **Materials come by role.** A part names a role, such as `timber/oak`, `thatch`, `rope` or `plaster`. The material pack answers each role with a tiled PBR set ([§14.6](#146-materials)).
4. **The editor places and parameterises.** The owner works with document records: a footprint drawn on the ground, an anchor picked on a tree, a style chosen from a list, a few numbers in a panel. Agents work the same records through the protocol.
5. **Generative models only make small props and materials.** That is where they are good: a stool, a lantern, a pot, a bark or plaster texture ([content generation](../content-generation.md)).

The split between engine and game follows plan 13's rule (owner, 2026-09-27): "a mechanism with parameters is the engine's, a choice of parameters, seeds and rules is the game's." So the solvers, the kit, the role table's mechanism and neutral default styles are the engine's. The elven style, the village layouts, the seeds and any assets belong to the game's repository, which registers anything of its own through [ADR-0046](../adr/0046-scene-generators-register-themselves.md)'s registry.

## 14.2 What exists to build on

| What | What it gives | What it lacks for structures |
|---|---|---|
| **The city's building grammar** ([city](../subsystems/city.md#the-building-grammar), [ADR-0044](../adr/0044-island-city-plan-and-building-grammar.md), proposed) | `Grammar::generate(lot, stage, building)` writes a **description**: floors, cores, spaces, units, walls as centre lines on a 2 m module with openings, doors, windows and furniture zones. It also writes an occupancy summary. Every decision is in integer centimetres and Q16, pinned by golden hashes, and checked by validators ([E18](../experiments/e18-island-city-yield.md): 200 of 200 buildings pass). Per-district style tables come with it ([parameters](../subsystems/city.md#parameters-and-style-tables)): 3.2 m floors, a 4.2 m ground floor, wall classes of 0.30/0.25/0.20/0.10 m, 1.8 m corridors. | **No meshes.** There are no roofs beyond a proxy slab, footprints are rectangles only, and everything stands at y = 0 with no terrain. The seven archetypes are urban (tower, mid-rise, office, terraced and detached houses, warehouse, civic shell), and the city is viewed as twelve proxy boxes. Its own [open list](../subsystems/city.md#what-is-left-open) names exactly what a village needs: kits attached to wall centre lines, corner and opening members, a facade far tier, the furnisher, non-rectangular footprints, terrain. |
| **The ruins' kit and ruin rule** ([ruins](../subsystems/ruins.md#the-kit), [grammar](../subsystems/ruins.md#the-grammar-and-the-ruin-rule)) | `engine.scene.RuinKit` defines members (Section, Corner, Doorway, Window, Debris, InsideCorner) with **sockets** on a wall's centre line. The grammar runs footprint (a rectangle, L, U or courtyard, chosen by a knapsack over section lengths) → placement → wall state by wind → members → debris. It is a pure function of kit, seed, tile and wind. | **The kit is boxes.** The real kit is E33's nineteen ashlar members made in Blender ([E33](../experiments/e33-hard-surface-kits.md)), which are not committed. "The kit" section lists seven things its schema cannot express. Members are imported meshes, not solved ones, and everything is at right angles. |
| **Scatter** ([scatter](../subsystems/scatter.md)) | A seeded lattice per kind, with density, slope and height limits. It honours **earlier placements' footprints**, but only as circles ([exclusion](../subsystems/scatter.md#exclusion-by-earlier-placements)). | Footprints as shapes, such as a building outline or a road band, are in its [not yet](../subsystems/scatter.md#not-yet). A village's yard, path and clearing need exactly that. |
| **Flora** ([flora](../subsystems/flora.md)) | Solved trees: the palm, broadleaf and conifer. The **motion contract** ([motion attributes](../subsystems/flora.md#motion-attributes), G61) is in container section 35 (`motion_parts`: per part a pivot, rest direction, length and parent; dense ids, parent-first, stable across the LOD ladder) and section 36 (per vertex a part id and bend weight). Nothing reads it yet (S66). Determinism is the content build's: the same bytes from every toolchain. | **No structural table today.** G63, in progress beside this chapter, adds append-only section 37: per part a base radius, a tip radius and a **load-bearing flag**. It also adds a megaflora species with trunks 4–25 m across, 40–150 m tall, buttress roots, hollows and near-horizontal major limbs. This chapter is written against that planned table. |
| **Document records** ([doc_scene](../subsystems/doc_scene.md#the-mapping), [tileset](../subsystems/tileset.md#the-record)) | `engine.scene.MeshPlacement` (mesh path, `worldpos`, rotation, yaw, scale) becomes one instance. A move is a move and a new mesh is a rebuild ([changes](../subsystems/doc_scene.md#changes)). `engine.terrain.Heightfield` is the model for a record **solved on read into the derived-data cache**: the record names parameters, the first read builds, later reads hit. | **No generator writes or reads records yet.** City, ruins and scatter are `derived` scene data, and `engine.flora.*` are parameter types for a mesh generator, not records. `engine.structure.*` records would be new. A placement's parent does not transform it, which an anchor needs ([not yet](../subsystems/doc_scene.md#not-yet)). |
| **The editor** ([apps](../subsystems/apps.md#engine-editor-the-engines-window-on-a-document), [engine_shell](../subsystems/engine_shell.md)) | Picking by the visibility buffer (14–65 ms in debug), translate and rotate gizmos (one `doc.apply` per drag), undo, redo and delete. `editor.import_mesh` copies a glTF, GLB or `.clusters` file in and places it where the view ray meets y = 0. A properties panel is driven by `schema.describe`, so a new record type gets editors for its fields without new UI code. | It cannot place a tree: `.flora.json` and `.rock.json` are not accepted. It cannot create a record of a chosen type from the shell, and it has no scale gizmo, no snapping and no multi-select (A24). The ground under the cursor is the y = 0 plane, not the picked surface. |
| **Scene collision** ([scene_collision](../subsystems/scene_collision.md)) | Static bodies round the walker. The ground is a heightfield tile at 1 m; placements are one compound a tile of each mesh's **proxy**, the LOD DAG cut at 5 cm (`walk.collision.mesh_error_m`), welded into a triangle mesh. A doorway stays open because hulls were rejected. A kit building of about 88 pieces costs about 10 KB. | **Nothing moves** ([not yet](../subsystems/scene_collision.md#not-yet)): every body is static, so a door, a moving platform or a swaying bridge is not covered. Only the terrain mesh, a skinned instance and water get no body, so a tree's **leaf cards would be walls**. |
| **The walker** ([physics](../subsystems/physics.md#the-character), [apps](../subsystems/apps.md#walking)) | `CharacterBody`, Jolt's virtual character: a 1.8 m capsule of 0.3 m radius with the eye at 1.65 m. It has a **40° slope limit** and climbs a **0.35 m step** by stepping up, over and down. It sticks to the floor down to `max(step, 0.5 m)` and jumps 0.82 m. It takes the ground's velocity when it stands on a moving body. | No inner body (it pushes nothing), no crouch or swim, and no "moving platforms beyond what the backend's ground velocity already gives" ([not yet](../subsystems/physics.md#what-is-not-wrapped-yet)). It cannot walk a scene with no terrain. |
| **Kinematic bodies** ([physics](../subsystems/physics.md#the-layer-table)) | `MotionType::Kinematic` and `move_kinematic(id, target, dt)` exist and are tested: a kinematic box pushes a dynamic one. The layer comment names "platforms, doors". | Static-against-kinematic contacts are off by design. Scene collision makes none of these bodies. |
| **Navigation** (`domain/nav/include/domain/nav/tile.h`) | Agent defaults: 0.6 m radius, 2.0 m height, **0.4 m climb, 45° slope**. Off-mesh links exist, so a ladder or a jump between platforms can be a link. | The nav agent is more generous than the walker. A step of 0.38 m is on the nav mesh but the walker cannot climb it. Structures are held to the walker's numbers ([§14.4](#144-walkability-and-collision)). |
| **Cluster geometry** ([geometry](../subsystems/geometry.md)) | A building is cluster geometry like everything else: 124 triangles and 64 vertices a cluster by default, a crack-free LOD DAG, and normal cones fit to what is drawn ([cones](../subsystems/geometry.md#normal-cones-fit-to-what-is-drawn)). One material per cluster. | Thin members (rope, railings, thatch edges) simplify the way a palm's leaflets do. Plank and rope parts need the coverage care [flora](../subsystems/flora.md#what-the-harness-taught) learned. |

## 14.3 The solver family

**Two capabilities, because there are two registration points.**

**`domain/structures`** is a mesh-generator capability, like flora and rocks. It registers with the content build's mesh-generator registry, so a `<name>.structure.json` recipe becomes a `.clusters` container in the derived-data cache. It holds the kit and four solvers that share it: the building, the tree-borne attachments, the bridge, and the stair and ramp (the attachment solver's and the building's common piece). A recipe in, a mesh out. It needs no ground and no document.

**`domain/village`** is a scene generator, like the ruins. It registers through [ADR-0046](../adr/0046-scene-generators-register-themselves.md)'s registry. It reads its ground through the tile source and earlier placements through `Context::earlier` ([scene_gen](../subsystems/scene_gen.md#earlier-placements)), and emits placements of structure recipes. It requires `structures` (`engine_capability_requires(village structures)`).

**Why two and not one.** A recipe solver is a pure function of the recipe. A village is a function of a ground, and its placements stream by tile. Keeping them apart means a game that places every house by hand carries no village generator ([ADR-0027](../adr/0027-additive-capabilities.md): unused costs nothing). It also means the minimal build proves the structures do not need it.

**Why not five.** The solvers share the kit, the role table, the anchor reader and the stair rule. Splitting them would make each a capability requiring the others, which is a graph of edges and no removability.

**Structures does not depend on flora.** The attachment solver reads a tree's **container**: sections 35–37, through `geometry`'s cluster-file reader. It never calls flora's code. A tree made by a game's own solver, with the same sections, carries platforms too.

Every solver writes a **description** first and a mesh second, as the city does. The description is the floors, the walls as centre lines with openings, the parts with their roles, the walk surfaces and the anchors used. Validators read the description: headroom, the step and slope numbers, a door's width, a platform's clearance. Agents reviewing a village read it too, in numbers rather than pictures. Then the description is meshed from the kit.

### The building solver

The input is a **footprint**: a rectangle, a polygon, or a circle of a radius. It also takes a floor count, a style, door and window wishes per side, a seed, and a ground sample under each corner. The steps are:

1. **Floors.** It sets a floor height by style. A sloped ground gets a plinth or stilts, by style, from the lowest corner.
2. **Walls on centre lines.** It reuses the city's convention (centre lines, openings as spans) so a later city mesher can reuse this one. Openings are placed by the style's rhythm with piers kept, the rule E18's first run taught.
3. **Stairs and ladders** between floors, from the shared stair rule ([§14.4](#144-walkability-and-collision)).
4. **The roof** by style: a gable, hip or cone over a round footprint, at the style's pitch and with eaves overhang. Its covering (thatch bundles, shingles, clay tile, bark) is laid as kit parts in courses.
5. **The exterior**: porches, balconies, chimneys, a sill and shutters. Each is a row in the style table with its own placement rule.
6. **The interior**, as a later stage (G66): rooms as a small graph (entry, hall, sleeping, hearth), each with floor, wall and ceiling roles, and **furniture placement rows**. The furniture is generated props or solved kit pieces: a bed is planks and posts. The city's furnisher (G44) and this one should be the same code.

**Style tables** are data, like the city's district styles. The engine ships neutral defaults: `timber-frame`, `thatch-cottage`, `shingle`, `tile`, `stone`, `log`. A style names the floor height, wall construction (framed panels, logs, coursed stone), roof form, pitch and covering, the trim and opening rhythm, and the role for each part.

**`elven-organic` is the exception.** It wants curved walls swept along a spline, roofs as leaf-shaped shells and no right angles. That is a second wall mesher behind the same description, not a table entry, and it is the game's style built on the engine's mechanism ([§14.10](#1410-risks-and-open-questions)).

### The tree-borne attachments solver

The input is a tree's container and an **anchor**: a part id from the structural table, a parameter along it (0 at the base, 1 at the tip), and an azimuth round it. The anchor's frame comes from section 35's pivot and direction and section 37's radius at that parameter. Kinds:

- **A platform** clamps to a trunk (a ring or a sector round it) or rests on one or more limbs flagged load-bearing. Its deck is planks on joists, with brackets or knee braces down to the trunk, and railings on every open edge. A platform is refused on a part that is not load-bearing, or where the part's radius at the anchor is under the style's minimum. The refusal names the part and the radius.
- **A stair or ramp round a trunk**: a helix at the trunk's radius plus a clearance, with treads cantilevered from the trunk or hung from a rail. It runs from a ground point or a platform to a platform.
- **A hut on a platform** is the building solver with a round or polygonal footprint inside the deck, its floor the deck.
- **A ladder** (a kit part) where a stair will not fit. It is an off-mesh link for navigation and is not walked by the walker ([§14.4](#144-walkability-and-collision)).

### The bridge solver

The input is two anchors: tree anchors, platform edges or ground posts. Kinds:

- **A rope bridge**: two hand ropes and two foot ropes as **catenaries** of a declared sag ratio, planks hung between the foot ropes at a spacing, and vertical ties. A catenary solve is iterative but closed under a fixed iteration count with polynomial functions, so it meets [flora's determinism rule](../subsystems/flora.md#determinism). The stance is no libm: `cosh` from a polynomial, the same bits on every toolchain.
- **A plank bridge**: stringers, decking and rails on posts, straight or with a shallow arch.

Bridges are static in v1. Sway comes later through the deformed-vertex pool's motion contract (G61's sections, written by the bridge solver as parts: each plank a part, its pivot on the rope) and a kinematic walk surface (S90).

### The village solver

The input is a region (a polygon on the ground), a seed, a house count and style mix, and rules for paths, fences, wells and a clearing. The steps are:

1. **Lots** by the city's plan logic scaled down: a path graph first from a centre (a well, a green) to the region's edges, then lots along paths.
2. **Each lot's footprint** sits on the **tile source's heights**, the way the ruins ask theirs. The village is refused on lots whose slope under the footprint exceeds the style's plinth or stilt allowance.
3. **Paths** as ground bands, **fences** as kit runs along lot edges, and **wells, woodpiles and carts** as placements.
4. **The forest's clearing**: the village's outline and paths are footprints that scatter (and G55's vegetation placement) honours. This needs [scatter's](../subsystems/scatter.md#not-yet) footprints as shapes rather than circles.

The village emits structure recipes, not meshes. Two houses with the same recipe share one built mesh in the derived-data cache.

### The kit of parts

Beams (square or hewn), planks (with a seeded warp and edge wear), posts, pegs, rope (a twisted helix of strands at a declared radius), thatch bundles, shingles, clay tiles, logs and coursed stone. Each is **solved**: a function of a few parameters and a seed, made at the size it is used, with UVs in metres from the part's own origin.

The kit is the shared vocabulary for two reasons:

- **Consistency.** Every plank in a village is the same construction at different lengths.
- **Destruction later** ([§14.8](#148-destruction-and-interaction-later)). A part knows what it is, so the support graph comes from the solver, not from guessing at a mesh.

A building is meshed as **one merged mesh per building**, not as thousands of instanced parts. The DAG can then simplify across part boundaries in the far field. Instancing parts would multiply instances and culling pairs by the part count, and hierarchical instancing (R66) does not exist. Identical recipes still share a mesh.

## 14.4 Walkability and collision

The walker's numbers are the design limits. The nav agent is held to them too (set `agent_max_climb` to the walker's step for a structures scene), so a path the navigation finds is one the player can walk.

| Element | Rule | From |
|---|---|---|
| Stair riser | ≤ 0.25 m (style default 0.20 m) | Walker step 0.35 m, with margin for the capsule's round bottom on a tread edge |
| Stair tread (going) | ≥ 0.28 m at the walking line, 0.6 m in from the inner edge on a spiral | Capsule radius 0.3 m |
| Ramp | ≤ 30° (style default 15°, a tree's wrap-round ramp up to 25°) | Walker slope 40°: 30° is the margin |
| Headroom | ≥ 2.1 m clear, so a spiral's pitch is ≥ 2.1 m plus the tread's depth per turn | Capsule 1.8 m plus a margin; nobody jumps on a stair |
| Doorway | ≥ 0.8 m wide, ≥ 2.0 m high | Capsule 0.6 m across; the ruins' walk test stops it 0.32 m from a face |
| Railing | ≥ 1.0 m high, a collision surface; balusters gaps visual only | Only the capsule's round bottom supports it, so a rail is a wall at any lean |
| Rope bridge sag | ≤ 0.15 of the span, so the end slope is ≤ about 31° | A catenary's end slope ≈ atan(4 · sag / span): 0.10 gives 22°, 0.15 gives 31°, 0.2 gives 39° (too close to the limit) |
| Plank gap | ≤ 0.10 m | The 5 cm collision proxy keeps a gap; the capsule does not fall through a gap narrower than its round bottom, but a foot-sized gap should read as one |

**Static structures need almost nothing new.** A platform, a stair, a ramp, a fixed bridge and a house are placements. Scene collision gives each a static proxy cut at 5 cm, which keeps doorways and stair treads (a 0.20 m riser is four times the cut's error). The walker climbs a riser by its step rule. So **stairs work today** if the riser stays under 0.35 m, and a ramp works up to 40°.

**What has to be added** (S89):

1. **A no-collision flag per part**, carried in the container and honoured by the proxy cut. A tree's leaf cards and twigs, a roof's thatch overhang and a rope's strands should not be walls. The walk surface of a rope bridge is its planks, and the hand ropes are a rail. Today only terrain, skinned instances and water get no body.
2. **Collision proxies for parts above a tile's ground**: a platform 40 m up is in the same tile as its tree. Scene collision already makes a compound a tile, so this is a check, not a change, but a 150 m tree's proxy will be the biggest compound it has seen.
3. **Walking a scene with no terrain** (a platform-only test scene). This is the walker's "next step" in [apps](../subsystems/apps.md#walking).
4. **Validators in the description** for every rule in the table, so a style that breaks one is refused with the rule's name, as the city's are ([validators](../subsystems/city.md#validators)).

**A swaying bridge needs a moving walk surface** (S90). Each plank becomes a kinematic box moved by `move_kinematic` to the rope's solved pose at the tick. The walker takes the ground's velocity when it stands on one, which the backend already gives. The rope itself is [05 §5.15](05-simulation.md#515-capability-inventory)'s "Ropes and cables" (S61): a two-ended anchor attachment, simulated at LOD0 and a catenary fit beyond. Drawing it goes through the deformed-vertex pool. Scene collision would then hold kinematic bodies beside its static ones, which it does not today. Until then a bridge is static and does not sway.

## 14.5 The authoring workflow

**A structure is a document record**, solved on read through the derived-data cache, as `engine.terrain.Heightfield` is ([tileset](../subsystems/tileset.md#the-record)). These are new types (D37), in a `schemas/structure.schema` beside the scene's:

- **`engine.structure.Building`**: a footprint (polygon or circle in metres, in the record's own frame), floors, a style name, opening wishes, a seed, a `worldpos` and yaw.
- **`engine.structure.Platform`**, **`.Stair`** and **`.Ramp`**: an **anchor** (the tree's record id, a part id, a parameter and an azimuth), a size, a style and railings.
- **`engine.structure.Bridge`**: two anchors (each a tree anchor, a platform record's edge or a ground point), a kind, a sag ratio, a width and a plank spacing.
- **`engine.structure.Village`**: a region polygon, a seed, a count and style mix, and its rules. It expands into the other records' recipes at read time and is never flattened into them. To hand-edit one house, the owner **pins** it, as the city's authored overrides do ([overrides](../subsystems/city.md#authored-overrides)): the pinned record overrides the generated one by lot id.

`doc_scene` maps a record to a recipe and the recipe to a mesh in the cache: the first read builds, a moved record is a move, and a changed parameter is a rebuild of that one mesh. **An anchored record moves with its tree.** Its transform is the tree placement's transform times the anchor's frame, which needs the placement hierarchy that doc_scene's "not yet" names. It also needs the tree's built container to be read before its attachments, so a tree's recipe change rebuilds its platforms. The anchor names a part id, and part ids are stable across the tree's LOD ladder but not across a change to its recipe. A recipe change that removes the part leaves the platform refused, with the reason in the record's status, rather than floating.

**What the owner does, step by step:**

1. Paint or import the ground (W66).
2. Place two ancient trees from recipes, which needs `import_mesh` to accept `.flora.json`.
3. Pick a limb or the trunk at a height with the **anchor picker**: the editor's pick already returns the pair and triangle, the triangle's vertices carry a part id (section 36), and section 37 gives the radius there. The panel shows the part, its height and whether it bears load.
4. Choose "platform" and a style; adjust its size in the properties panel (`schema.describe` drives it).
5. Pick a second anchor on the other tree and choose "rope bridge".
6. Draw a village region with the **footprint tool**: points on the picked ground, not on y = 0. Set a house count and a style mix, then pin and adjust any house.

At every step the result is drawn within a rebuild of one mesh (an estimate: a house's solve plus its cluster build at well under a second, to be measured).

**What the editor needs** (A42): creating a record of a chosen type, placing a recipe-made mesh, the anchor picker, the footprint tool on the picked surface, and a scale handle. Parameter panels come from the schema already. ADR-0054's tools UI (Dear ImGui drawn by the renderer) is the natural home for the picker and the footprint tool, which want the viewport's pick.

**The game's repository** holds the village's records, its styles (the elven one at least), its seeds, its generated props and textures, and any solver of its own. It registers that solver through the registry and needs no engine change. The engine's test scene for all of this is neutral: two megaflora trees, the default styles, no game names.

## 14.6 Materials

Every kit part names a **role**: `timber/oak`, `timber/pine`, `timber/weathered`, `bark`, `thatch`, `shingle/wood`, `tile/clay`, `rope`, `plaster`, `stone/ashlar`, `stone/rubble`, `iron`, `cloth`. The **material pack**, which G60's material lab work is producing, answers each role with a tiled PBR set (base colour, normal, roughness, height) at a `tile_m`. A game may override any role. The rocks and the flora recipes already carry a `tile_m` per set ([rocks](../subsystems/rocks.md#uvs-and-materials)), and the two types should converge with this one on a single role-keyed type.

**The limits today:**

- **Tiling only.** Repetition shows on large plaster and thatch faces.
- **No decals:** no dirt at a wall's foot, no moss on a north face, no grime under eaves.
- **No layered materials or blends** (R21), so weathering cannot fade over a beam.
- **One material per cluster.** Each role a building uses adds clusters at its boundaries, so a cottage of eight roles costs more clusters than one of three.
- **UVs within ±16** (`geometry.uv_range`), so a part's UV run is at most 32 tiles. Meshing UVs from each part's own origin keeps every part inside it.

**What would help, in order:**

1. **Trim sheets**: one texture holding planks, battens, trims and edges, one role, many parts. This cuts the material count, and with it the cluster count, more than anything else.
2. **Per-vertex or per-part tint and wear**, written by the solver from the seed, so two oak beams differ.
3. **Decals at solve time**, projected by rule (foot, eaves, around openings).
4. **R21's layered materials**, for moss and wet.

## 14.7 LOD, determinism, budgets

**LOD is the cluster DAG's.** A building is one mesh, its DAG built like any other. Far away its roof and walls collapse to a handful of clusters, and the thin parts (rope, rails, thatch fringe) go first. Beyond the DAG's root the far field wants a **village impostor**: a per-tile merged proxy or the vegetation impostors of R67 generalised. Neither is needed for a proof of concept seen from inside the village.

**Budgets, estimated.** These are numbers for the first build to measure, not results.

| Item | Finest triangles (est.) | Clusters at 124 tris, finest (est.) | With the DAG, about ×2 |
|---|---|---|---|
| Timber-frame house, exterior only | 30k | 240 | 480 |
| Round hut | 12k | 100 | 200 |
| Platform with railings | 5k | 40 | 80 |
| Rope bridge, 20 m | 8k | 65 | 130 |
| Spiral stair, one turn | 6k | 50 | 100 |
| House with a furnished interior | 75k | 600 | 1,200 |

At about 1.4 KB a cluster (two 48-byte descriptors, 64 vertices at 14 bytes, 124 triangles of indices), a **village of 50 exterior-only houses** is about 24,000 clusters, roughly 35 MB if every house is unique, and much less where recipes repeat. That is ten FlightHelmets ([geometry](../subsystems/geometry.md): 2,348 clusters), well inside what the renderer draws today. Interiors are what multiplies it ([§14.10](#1410-risks-and-open-questions)).

**Determinism** is flora's ([flora](../subsystems/flora.md#determinism)): every decision in integer millimetres and fixed-point, no libm (polynomial `cosh` for the catenary, polynomial trigonometry for the helix), the seed through the engine's hash. The output is the same bytes on every toolchain and pinned by golden section hashes. The cache key is the recipe's canonical bytes plus `k_structure_cache_version`. For an attachment, the key also covers the tree container's sections 35–37 hash, because the anchor reads them. The village's output is placements, so its stance is the ruins' `derived`: a function of region, seed and the tile source's heights.

## 14.8 Destruction and interaction later

- **Doors** (S91) are kinematic bodies on a hinge, moved by `move_kinematic` and opened by a game through the actor or prop API. The building solver emits a door as a separate part with its pivot, so the door is not baked into the wall's mesh.
- **Breakable walls and collapsing huts** take plan 05's support graph ([05 §5.8](05-simulation.md#58-destruction), S46–S49). Because a building is solved from parts, the support graph comes **from the description**: posts, beams, joists, wall panels, roof members, and for a platform the tree limb it rests on (G63's load-bearing flag). There is no heuristic over a mesh. A cut limb is a collapse event for every platform on it.
- **Fire** is [05 §5.15](05-simulation.md#515-capability-inventory)'s "Fire, heat and burning": a thermal block per material category, with burning as damage. Thatch and timber roles carry the category, so a solved building is ready for it when it exists. No roadmap row opens fire here.

## 14.9 The first proof of concept

**Target** (P48): two ancient trees (G63's megaflora, 40–60 m), **five platforms** (three on one tree, two on the other, at 8–25 m), **three rope bridges** (one between the trees, two between platforms on one tree), **two round huts** on platforms, **one spiral stair** from the ground to the first platform, and **one ground village of eight timber-frame houses and a well** in a clearing beside them, textured by role from the pack.

The scene is walkable. The walker climbs the stair, crosses every bridge, enters each hut and house through a door, and is stopped by every railing. It is captured from the ground and from the highest platform, and it opens and edits in the editor.

**Acceptance numbers to measure:**

- Every structure passes its validators.
- A walk through the stair, two bridges and a hut completes with no drop (the walk summary's `drops` is 0).
- The scene's cluster count and memory, against the estimates above.
- A frame's GPU time along a camera path through it.
- The solve and build time of each recipe.
- The rebuild latency after one parameter edit in the editor.

**Order of work:**

1. **G63** (in progress beside this chapter): the megaflora species and the structural table.
2. **G64 and G70** together: the kit of parts, and materials by role from the pack (G60). The kit can be tested in isolation: every part's bytes pinned, every part drawn.
3. **G65**: the building solver v1. Rectangular and round footprints, the default styles, an exterior and a single-room shell with doors, gable and cone roofs, stilts and plinths on slopes.
4. **D37**: the `engine.structure.*` records, doc_scene's mapping and the placement hierarchy for anchors.
5. **S89**: walkability. The per-part no-collision flag, the validators, the platform test scene with no terrain, and the nav agent set to the walker's numbers.
6. **G67**: platforms, railings, the spiral stair and huts on platforms, against G63's anchors.
7. **G68**: rope and plank bridges, static.
8. **G69**: the village solver (lots on terrain, paths, fences, a well, the clearing).
9. **A42**: the editor's record creation, recipe placement, anchor picker and footprint tool.
10. **P48**: the scene, the walk, the captures, the numbers, and the owner's walk through it.

Steps 3–5 can run in parallel once 2 is in. Steps 6 and 7 can too, once 3 and 4 are. After the proof of concept come **G66** (interiors), **S90** (sway) and **S91** (doors).

## 14.10 Risks and open questions

**For the owner:**

1. **Reuse the city's grammar or replace it?** Recommended: keep the city's *description* (centre-line walls, openings, integer centimetres, validators) as the shared intermediate. The building solver here writes it, and the city's grammar gains this chapter's mesher in place of its proxy boxes (P27). Do not stretch the city's urban grammar over a village: it is rectangles on a 2 m module at y = 0 with seven urban archetypes, and a village needs polygons and circles on slopes with rural archetypes. The cost is two grammars over one description, and the gain is that neither bends.
2. **Interiors' cost.** A furnished house is about 2.5 times its exterior in clusters (estimate). Fifty of them is 60,000 clusters and roughly 85 MB, all resident if nothing streams them. Island City's interior cells (P30: unloaded, coarse, active) are the answer at scale. Does the proof of concept need interiors at all, or are the huts' and houses' single-room shells enough until G66?
3. **How organic is "elven"?** Curved walls along splines and shell roofs are a second mesher, not a style row. If the elven look can be got from round and polygonal footprints with carved trim, organic roofs and living-wood railings, it stays in the style table and costs far less. A reference picture from the owner would settle it.
4. **Where the anchor picker lives.** It wants the viewport's pick, which is native, so it belongs with ADR-0054's tools UI rather than the web shell. That splits the editor's panels between two toolkits for a while. Is that acceptable, or should the shell's properties panel learn to show a pick result?

**Risks:**

- **The megaflora's own cost.** A 150 m tree is the biggest single mesh and the biggest collision compound the engine will have seen. G63 measures it, and every platform sits on it.
- **Anchor stability across recipe edits.** Part ids are stable across the ladder, not across a change to a tree's recipe. An edited tree can orphan its platforms. The refusal is loud by design, but the owner may expect platforms to follow a limb that moved.
- **Thin geometry in the far field.** Rope and railings shimmer and vanish under the DAG's simplification, as the palm's leaflets did. They want the same coverage-preserving treatment, or an alpha card (R65) at distance.
