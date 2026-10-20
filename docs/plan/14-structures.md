# 14 — Structures: Buildings, Villages, and Architecture in Trees

Status: recorded 19 October 2026 from the owner's request for a fantasy-RPG proof of concept, and revised 20 October 2026 with the owner's answers to its four open questions, which are now decisions ([§14.10](#1410-decisions-and-risks)). This chapter is a plan; **nothing in it is built**. The numbers it quotes about today's engine come from the pages it links, and the ones it estimates are marked as estimates for the first measurement to replace. Its roadmap rows are G64–G70, G72, D37, S89–S91, R86, R87, A42 and P48. It builds on G63 (the megaflora species and the structural table, done at a98f683a) ([roadmap](../roadmap.md)).

## 14.1 The problem, and the stance

The owner's fantasy RPG lives in its own repository and pins the engine, as Desert Survival does ([13 §13.1](13-reference-consumer-games.md#concept), the direction note on repositories; [ADR-0047](../adr/0047-reference-games-are-licensed-apart-from-the-engine.md) for the licensing). Its first proof of concept is **a village with a forest**. After that come **elven villages that span trees**: buildings in and between exceptionally large trees, rope bridges, ramps, interiors, exteriors and roofs, plus ordinary villages and towns.

**The game is mostly authored.** Its villages change state — a door left open, later a wall broken — but they are not regenerated. A village is what its author placed, and it stays that across sessions, saves and engine upgrades.

**The owner has no modelling skill and does not intend to acquire any.** Generative 3D is weakest at exactly this kind of content. Its size and its quality move in opposite directions. Of the Tripo corpus, 349 of 438 meshes passed E10's checks and the rest failed on topology; that figure is the owner's own, from a run whose numbers are not in this tree yet ([E10](../experiments/e10-generated-props.md) has the 20-prop pilot). A house, a platform clamped to a particular limb, or a bridge between two particular trees is something no prompt produces in place.

The stance follows from both:

1. **The description is the authored artefact.** A building is a **description**: its walls as centre lines (straight, arcs and splines) with openings, its floors, rooms and doors, its roof as a surface over its footprint, its style and its anchors, every position in integer centimetres, checked by validators. The document holds it as `engine.structure.*` records ([§14.5](#145-the-authoring-workflow)). **One deterministic mesher** turns a description into cluster geometry, the same way a palm is solved from its `.flora.json` ([flora](../subsystems/flora.md)). Nothing re-derives a description behind its author's back.
2. **A grammar is a filler, not the source.** A grammar fills a description from a few parameters — a footprint, a floor count, a style, a seed — when the author wants a starting point, or unattended with a seed when a dynamic area is first made. Either way its output is a description written into the document, which the author then owns. Re-running a grammar is a new fill, never an update of the old one.
3. **Solved parts form a shared kit.** Beams, planks, posts, rope, thatch bundles, shingles and the living-wood members are made by the mesher's kit, not imported. Every structure is assembled from them, so a village holds together visually and a fix to the plank fixes every plank.
4. **Materials come by role.** A part names a role, such as `timber/oak`, `thatch`, `rope` or `wood/living`. The material pack answers each role with a tiled PBR set ([§14.6](#146-materials)).
5. **The editor places and edits.** The owner works with document records: a footprint drawn on the ground, a fill asked for, a door moved along its wall, an anchor picked on a tree, a style chosen from a list, a few numbers in a panel. Agents work the same records through the protocol.
6. **Generative models only make small props and materials.** That is where they are good: a stool, a pot, a bark or plaster texture ([content generation](../content-generation.md)).

**What an engine upgrade may and may not change.** The description is document data, changed only by an edit or by a schema migration, so rooms, walls, openings, doors, floor heights and anchors are where the author put them in every engine version. A mesher change bumps `k_structure_cache_version` and rebuilds meshes, which may move triangles — a plank's warp, a course count, a curve's tessellation, the DAG's cut — but **never a room or a door**. Two things hold that: the **mesher's conformance test**, which meshes a committed corpus of descriptions and checks every opening clear where its description puts it to the millimetre, every floor at its height, every door's pivot on its hinge and every walk surface within the collision proxy's 5 cm; and the **validators**, which read the descriptions and so give the same verdicts across an upgrade that did not touch them. A grammar change changes only the fills made after it.

The split between engine and game follows plan 13's rule (owner, 2026-09-27): "a mechanism with parameters is the engine's, a choice of parameters, seeds and rules is the game's." So the description, the mesher, the kit, the fillers, the role table's mechanism and two neutral default style tables are the engine's. The elven style, the village records, the seeds and any assets belong to the game's repository, which registers anything of its own through the engine's registration points ([§14.3](#143-the-description-the-mesher-and-the-fillers)).

## 14.2 What exists to build on

| What | What it gives | What it lacks for structures |
|---|---|---|
| **The city's building grammar** ([city](../subsystems/city.md#the-building-grammar), [ADR-0044](../adr/0044-island-city-plan-and-building-grammar.md), proposed) | `Grammar::generate(lot, stage, building)` writes a **description**: floors, cores, spaces, units, walls as centre lines on a 2 m module with openings, doors, windows and furniture zones. It also writes an occupancy summary. Every decision is in integer centimetres and Q16, pinned by golden hashes, and checked by validators ([E18](../experiments/e18-island-city-yield.md): 200 of 200 buildings pass). Per-district style tables come with it ([parameters](../subsystems/city.md#parameters-and-style-tables)): 3.2 m floors, a 4.2 m ground floor, wall classes of 0.30/0.25/0.20/0.10 m, 1.8 m corridors. | **No meshes.** There are no roofs beyond a proxy slab, footprints are rectangles only, walls are straight, and everything stands at y = 0 with no terrain. The seven archetypes are urban (tower, mid-rise, office, terraced and detached houses, warehouse, civic shell), and the city is viewed as twelve proxy boxes. Its own [open list](../subsystems/city.md#what-is-left-open) names exactly what a village needs: kits attached to wall centre lines, corner and opening members, a facade far tier, the furnisher, non-rectangular footprints, terrain. |
| **The ruins' kit and ruin rule** ([ruins](../subsystems/ruins.md#the-kit), [grammar](../subsystems/ruins.md#the-grammar-and-the-ruin-rule)) | `engine.scene.RuinKit` defines members (Section, Corner, Doorway, Window, Debris, InsideCorner) with **sockets** on a wall's centre line. The grammar runs footprint (a rectangle, L, U or courtyard, chosen by a knapsack over section lengths) → placement → wall state by wind → members → debris. It is a pure function of kit, seed, tile and wind. | **The kit is boxes.** The real kit is E33's nineteen ashlar members made in Blender ([E33](../experiments/e33-hard-surface-kits.md)), which are not committed. "The kit" section lists seven things its schema cannot express. Members are imported meshes, not solved ones, and everything is at right angles. |
| **Scatter** ([scatter](../subsystems/scatter.md)) | A seeded lattice per kind, with density, slope and height limits. It honours **earlier placements' footprints**, but only as circles ([exclusion](../subsystems/scatter.md#exclusion-by-earlier-placements)). | Footprints as shapes, such as a building outline or a road band, are in its [not yet](../subsystems/scatter.md#not-yet). A village's yard, path and clearing need exactly that. |
| **Flora** ([flora](../subsystems/flora.md)) | Solved trees: the palm, broadleaf, conifer, birch, willow, orchard and the **ancient tree** ([megaflora](../subsystems/flora.md#megaflora): trunks 4–25 m across, 40–150 m tall, buttresses, doorway-sized hollows, straight near-horizontal limb runs "where platforms go"). The **motion contract** ([motion attributes](../subsystems/flora.md#motion-attributes)) is container sections 35 and 36, and the **structural table** ([the structural table](../subsystems/flora.md#the-structural-table)) section 37: per part a base and tip radius and the flags load-bearing and straight. A structure anchors to straight load-bearing parts, whose axis and radius hold to 2 mm. The default ancient tree is 5,631 clusters. The branching species share a kit of axes, tubes and sprays (`domain/flora/src/kit.h`). | The kit is private to flora (`src/`), and its bytes are pinned by every species' golden hashes. A walker on a bare limb drifts off a sloped run's crest and walks off any edge ([walking a limb](../subsystems/flora.md#walking-a-limb)): a ramp must be a deck with a rail, not the bare limb. |
| **Document records** ([doc_scene](../subsystems/doc_scene.md#the-mapping), [tileset](../subsystems/tileset.md#the-record)) | `engine.scene.MeshPlacement` (mesh path, `worldpos`, rotation, yaw, scale) becomes one instance. A move is a move and a new mesh is a rebuild ([changes](../subsystems/doc_scene.md#changes)). `engine.terrain.Heightfield` is the model for a record **solved on read into the derived-data cache**: the record names parameters, the first read builds, later reads hit. The materializer and `@writeback` rows carry a record's state into the world and back. | **No generator writes or reads records yet.** City, ruins and scatter are `derived` scene data, and `engine.flora.*` are parameter types for a mesh generator, not records. `engine.structure.*` records would be new. A placement's parent does not transform it, which an anchor needs ([not yet](../subsystems/doc_scene.md#not-yet)). |
| **The editor** ([apps](../subsystems/apps.md#engine-editor-the-engines-window-on-a-document), [ADR-0054](../adr/0054-the-tools-ui-is-dear-imgui-drawn-by-the-renderer.md), [engine_shell](../subsystems/engine_shell.md)) | One window: the **native Vulkan viewport** beside the **web shell** in WebView2, both clients of one in-process host. The viewport picks by the visibility buffer (14–65 ms in debug, the pair and triangle under the click), draws the selection box and the move and rotate gizmos on the renderer's overlay, and commits each gesture as one `doc.apply`; the shell's properties panel is driven by `schema.describe`, so a new record type gets editors for its fields without new UI code. `editor.import_mesh` copies a glTF, GLB or `.clusters` file in and places it where the view ray meets y = 0. | It cannot place a tree: `.flora.json` and `.rock.json` are not accepted. It cannot create a record of a chosen type from the shell, and it has no scale gizmo, no snapping and no multi-select (A24). The ground under the cursor is the y = 0 plane, not the picked surface. |
| **Scene collision** ([scene_collision](../subsystems/scene_collision.md)) | Static bodies round the walker. The ground is a heightfield tile at 1 m; placements are one compound a tile of each mesh's **proxy**, the LOD DAG cut at 5 cm (`walk.collision.mesh_error_m`), welded into a triangle mesh. A doorway stays open because hulls were rejected. A kit building of about 88 pieces costs about 10 KB. | **Nothing moves** ([not yet](../subsystems/scene_collision.md#not-yet)): every body is static, so a door, a moving platform or a swaying bridge is not covered. Only the terrain mesh, a skinned instance and water get no body; the flora stage's collision rule keeps a tree's wood and drops its cards (S66), but nothing yet lets a structure's part say the same. |
| **The walker** ([physics](../subsystems/physics.md#the-character), [apps](../subsystems/apps.md#walking)) | `CharacterBody`, Jolt's virtual character: a 1.8 m capsule of 0.3 m radius with the eye at 1.65 m. It has a **40° slope limit** and climbs a **0.35 m step** by stepping up, over and down. It sticks to the floor down to `max(step, 0.5 m)` and jumps 0.82 m. It sweeps against the moving and kinematic layers as well as the static one, and takes the ground's velocity when it stands on a moving body. | No inner body (it pushes nothing, and nothing pushes it but its own sweep), no crouch or swim, and no "moving platforms beyond what the backend's ground velocity already gives" ([not yet](../subsystems/physics.md#what-is-not-wrapped-yet)). It cannot walk a scene with no terrain. |
| **Kinematic bodies** ([physics](../subsystems/physics.md#the-layer-table)) | `MotionType::Kinematic` and `move_kinematic(id, target, dt)` exist and are tested: a kinematic box pushes a dynamic one. The layer comment names "platforms, doors". | Static-against-kinematic contacts are off by design. Scene collision makes none of these bodies, and nothing in a scene uses `move_kinematic`. |
| **Fires** ([renderer](../subsystems/renderer.md#fires), [volumetrics](../subsystems/volumetrics.md#fire)) | Since 2a2dac83 an emissive, lit prop: `engine.scene.Fire` on a scene or a prop is a point light in the frame's array (traced shadows under `rt`), its intensity and blackbody colour flickering by the clock and the seed, and a flame and smoke drawn by the medium. | Its colour is a blackbody temperature, at most **four** fire lights a frame are lit (`k_max_fire_lights`, the nearest the eye), and a scene with a medium draws its flame. A steady lantern of magic light is none of those ([§14.3](#globe-lanterns)). |
| **Navigation** (`domain/nav/include/domain/nav/tile.h`) | Agent defaults: 0.6 m radius, 2.0 m height, **0.4 m climb, 45° slope**. Off-mesh links exist, so a ladder or a jump between platforms can be a link. | The nav agent is more generous than the walker. A step of 0.38 m is on the nav mesh but the walker cannot climb it. Structures are held to the walker's numbers ([§14.4](#144-walkability-and-collision)). |
| **Cluster geometry** ([geometry](../subsystems/geometry.md)) | A building is cluster geometry like everything else: 124 triangles and 64 vertices a cluster by default, a crack-free LOD DAG, and normal cones fit to what is drawn ([cones](../subsystems/geometry.md#normal-cones-fit-to-what-is-drawn)). One material per cluster. | Thin members (rope, railings, thatch edges) simplify the way a palm's leaflets do. Plank and rope parts need the coverage care [flora](../subsystems/flora.md#what-the-harness-taught) learned. |

## 14.3 The description, the mesher and the fillers

**Two capabilities, because there are two consumers.**

**`domain/structures`** is a mesh-generator capability, like flora and rocks. It owns the **description** type, the **mesher** on curves, the **kit** (the living-wood members among it), the attachment and bridge solvers that write descriptions of their own, and the **rural filler**. It registers with the content build's mesh-generator registry, so a description — a record's, or a `<name>.structure.json` file's — becomes `.clusters` containers in the derived-data cache. It needs no ground and no document; the rural filler takes its ground samples as input. It registers the filler as a protocol method (`structure.fill`, a write) so the editor and agents reach it the same way.

**`domain/village`** is the village filler. It reads its ground through the tile source and earlier placements through `Context::earlier` ([scene_gen](../subsystems/scene_gen.md#earlier-placements)), lays paths and lots, calls the rural filler per lot, and **writes records**: one `engine.structure.Building` per house, fences, a well, the clearing's footprint. It is registered through the protocol's method table (`structure.fill_village`) and through [ADR-0046](../adr/0046-scene-generators-register-themselves.md)'s registry, so a game can add a filler of its own the same way. It requires `structures` (`engine_capability_requires(village structures)`).

**Why two and not one.** The description and the mesher are a pure function of the record. A village filler is a function of a ground and a region. Keeping them apart means a game that places every house by hand carries no village filler ([ADR-0027](../adr/0027-additive-capabilities.md): unused costs nothing), and the minimal build proves the structures do not need it.

**Why not five.** The solvers share the description, the kit, the role table, the anchor reader and the stair rule. Splitting them would make each a capability requiring the others, which is a graph of edges and no removability.

**Structures does not depend on flora.** The attachment solver reads a tree's **container**: sections 35–37, through `geometry`'s cluster-file reader. It never calls flora's code. A tree made by a game's own solver, with the same sections, carries platforms too. The living-wood kit's tube and branch helpers are **copied** from flora's kit rather than linked ([below](#the-living-wood-kit)).

### The description

The description is what the document holds and what everything else reads: the mesher, the validators, the walker's collision, navigation, an agent reviewing a village in numbers rather than pictures, and later the support graph ([§14.8](#148-destruction-and-interaction-later)). It is **the city's description, generalised**, and the decision that settles [§14.10](#1410-decisions-and-risks)'s first question: the city's centre-line walls, openings, spaces, doors, integer centimetres and validators are kept, and three things are added.

- **Walls are polylines of lines, arcs and splines.** A wall's centre line is a chain of segments: a straight line (the city's only kind, and the trivial case), a circular arc by its two ends and a bulge, or a cubic Bézier by four control points. Positions are integer centimetres; a parameter along a segment is Q16. A wall carries a thickness class, a height and a construction (framed panels, logs, coursed stone, living wood). Openings are spans of the wall's arc length with a sill and a head height, so a door or window on a curved wall is the same record as on a straight one.
- **Roofs are surfaces over a footprint.** A roof is a form over the closed footprint its walls make: **gable**, **hip** and **cone** for the rural tables, and a **shell** (a surface of revolution or a loft over the footprint's curve with a rise profile) and a **sweep** (a profile swept along a spline, for eaves and ridges that flow) for the organic one. Pitch, overhang and covering are the style's.
- **Rooms and doors are records the author owns.** Rooms are regions bounded by walls, each with floor, wall and ceiling roles and furniture rows (G66). A door sits in an opening with a hinge side, a swing and a leaf construction, and is a record of its own so it can carry its open state ([§14.4](#144-walkability-and-collision)).

The city keeps its own urban grammar as a filler of this description and takes the mesher for its proxies later (P27): the description type lives in `structures`, and `city` will require it when it does.

**Validators read the description**: headroom, the step and slope numbers, a door's width, a platform's clearance, the walls closing a footprint, a curve's radius against its wall's thickness (an arc tighter than its thickness folds). A failure names the rule, the record and what was measured, as the city's do ([validators](../subsystems/city.md#validators)), and is never repaired: the author or the filler fixes it.

### The mesher

**One mesher, on curves from the start**, so there is no second mesher later. A straight wall is an arc of zero bulge; a round hut is one closed arc; an organic wall is a chain of splines. The mesher:

1. **Tessellates each centre line** to a chord error by style (1 cm at the finest, an estimate), with the openings' ends, the junctions and the corners pinned into the tessellation so an opening is the width asked on every rung of the DAG — flora's rule for the ancient tree's doorways.
2. **Sweeps each wall's section** along its centre line, offset by half the thickness each side, and **joins walls** where they meet: a mitre for two segments of one chain, a butt or a lap for a T, chosen by construction.
3. **Cuts the openings** and lines them (jambs, sills, lintels as kit parts), and leaves each door's opening empty for its leaf.
4. **Builds the floors and the stairs** between them from the shared stair rule ([§14.4](#144-walkability-and-collision)).
5. **Builds the roof surface** and lays its covering (thatch bundles, shingles, clay tile, bark, living wood) as kit parts in courses along it.
6. **Places the exterior rows** (porches, balconies, chimneys, sills, shutters, carved trim, lanterns) from the style table.
7. **Writes three kinds of mesh per building**: the **exterior**, the **interior** (inner walls, ceilings, room finishes, furniture) and **one mesh per door leaf**, with its pivot on the hinge. They are separate because they live differently: the interior is resident only when the walker can reach it ([§14.7](#147-lod-determinism-budgets)), and a door moves.

Every evaluation is f64 from the integer inputs, with no libm: an arc's points by `det::sin` (the polynomial the fire's flicker uses), a Bézier by its polynomial, a catenary's `cosh` by a polynomial. The output is the same bytes on every toolchain ([§14.7](#147-lod-determinism-budgets)).

### The fillers

**The rural filler** is the old building solver, now a tool. Its input is a **footprint** (a rectangle, a polygon, or a circle of a radius), a floor count, a style, door and window wishes per side, a seed, and a ground sample under each corner. It writes a description:

1. **Floors.** A floor height by style. A sloped ground gets a plinth or stilts, by style, from the lowest corner.
2. **Walls on centre lines.** Openings by the style's rhythm with piers kept, the rule E18's first run taught.
3. **Stairs and ladders** between floors, from the shared stair rule.
4. **The roof** by style: a gable, hip or cone at the style's pitch with eaves overhang.
5. **The exterior**: porches, balconies, chimneys, sills and shutters, each a row in the style table with its own placement rule.
6. **The rooms**: a small graph (entry, hall, sleeping, hearth) with doors between them and furniture rows (G66). The furniture is generated props or kit pieces: a bed is planks and posts. The city's furnisher (G44) takes this one rather than writing its own.

The fill is offered from the editor ([§14.5](#145-the-authoring-workflow)): the author draws a footprint, asks for a fill, and edits what comes back. In a **dynamic area** the host calls the same filler unattended with the area's seed the first time the area is made, and writes the records into the area's generated layer, where they are the document's like any other. The filler records its own name, version and seed on the record as provenance, never as a recipe to re-run.

**Style tables** are data, like the city's district styles. The engine ships two:

- **`rural`**: timber frame, thatch cottage, shingle, tile, stone and log as rows. A row names the floor height, wall construction, roof form, pitch and covering, the trim and opening rhythm, and the role for each part.
- **`organic`**: curved walls swept along splines, shell and sweep roofs, carved trim, living-wood railings, posts and brackets, globe lanterns, and few right angles. It exercises every curved path of the mesher in the engine's own tests. It is named for what it is rather than "elven" because the elven look is a choice of parameters and therefore the game's: the game's elven table is a copy or an override of `organic` in its own repository.

### The living-wood kit

Carved trim, railings, posts and brackets in the organic style are **branch-shaped members**: an axis that grows from a base, turns and wanders by a seeded amount with a parallel-transported frame, and a tapered tube along it, which is exactly what flora's kit makes (`grow_axis`, `build_tube`, `branch_direction`, `pipe_radius_of` in `domain/flora/src/kit.h`). A railing is a run of such members between posts; a bracket is one grown from a trunk anchor out under a deck.

**Grafted onto solved trees, not grown by the tree solver.** A member whose base is a tree anchor takes its base radius and its bark role from the tree's structural table and material at that anchor, so where it meets the trunk it reads as growing out of it. It is part of the structure's mesh; the tree's recipe, solver and container are untouched, and a tree with no structures on it is the bytes it was.

**The helpers are copied, not linked** (ADR-0027). Linking would mean flora publishing its private `src/kit.h` as a public header — a change to another capability's contract — and `engine_capability_requires(structures flora)`: a game with houses and no trees would carry flora, and `ENGINE_WITH_FLORA=OFF` would switch the structures off with it. Worse, flora's kit is pinned by every species' golden hashes, so a change made for a railing would move every tree's bytes, and a change made for a tree would move every railing's. A copy is about 240 lines of header and the tube functions of `kit.cpp`, pinned by structures' own hashes. Revisit when a third user wants the same helpers: lift them then into one non-capability module both link, in a commit that moves no pinned byte (the same source, compiled with contraction off, is the same arithmetic).

### Globe lanterns

**A globe lantern is an emissive prop with a light**, on the pattern of `engine.scene.Fire` ([renderer](../subsystems/renderer.md#fires)): a prop's block puts a point light at the globe's centre, turned and scaled with the instance, in the frame's light array, and the globe's glass is an emissive material (`glass/lantern`; the material table's emissive slot, which the resolve already reads). **The fire's light does not cover it**, for three reasons read from the renderer's page: its colour is a blackbody temperature, and magic light is not one; at most four fire lights a frame are lit, the nearest the eye, where an organic village hangs dozens; and a scene with a medium draws a fire's flame and smoke. So the steady variant is a row of its own (R86): an `engine.scene.Lamp` block on a scene or a prop — a colour, an intensity and a reach, no flame, no smoke, no flicker — sharing the fire's placement and light code. Lighting more than four local lights a frame is a renderer question of its own (R87); until it is answered, the four nearest lamps and fires are lit and the rest glow by their emission alone.

### The tree-borne attachments solver

The input is a tree's container and an **anchor**: a part id from the structural table, a parameter along it (0 at the base, 1 at the tip), and an azimuth round it. The anchor's frame comes from section 35's pivot and direction and section 37's radius at that parameter, and holds to 2 mm on a part flagged straight, so **an anchor names a straight load-bearing part**. The solver writes a description, which the mesher meshes. Kinds:

- **A platform** clamps to a trunk (a ring or a sector round it) or rests on one or more load-bearing runs. Its deck is planks on joists, with brackets or knee braces down to the trunk (living-wood brackets in the organic style), and railings on every open edge. A platform is refused on a part that is not load-bearing, or where the part's radius at the anchor is under the style's minimum. The refusal names the part and the radius.
- **A stair or ramp round a trunk**: a helix at the trunk's radius plus a clearance, with treads cantilevered from the trunk or hung from a rail. It runs from a ground point or a platform to a platform. A ramp along a limb is a flat-topped deck with a rail, never the bare limb ([walking a limb](../subsystems/flora.md#walking-a-limb)).
- **A hut on a platform** is a description with a round or curved footprint inside the deck, its floor the deck, meshed like any building.
- **A hut in a hollow** uses `flora::ancient_hollows`' sill, directions and size, which come from the tree's parameters alone.
- **A ladder** (a kit part) where a stair will not fit. It is an off-mesh link for navigation and is not walked by the walker ([§14.4](#144-walkability-and-collision)).

### The bridge solver

The input is two anchors: tree anchors, platform edges or ground posts. Kinds:

- **A rope bridge**: two hand ropes and two foot ropes as **catenaries** of a declared sag ratio, planks hung between the foot ropes at a spacing, and vertical ties. A catenary solve is iterative but closed under a fixed iteration count with polynomial functions, so it meets [flora's determinism rule](../subsystems/flora.md#determinism). The stance is no libm: `cosh` from a polynomial, the same bits on every toolchain.
- **A plank bridge**: stringers, decking and rails on posts, straight or with a shallow arch.

Bridges are static in v1. Sway comes later through the deformed-vertex pool's motion contract (G61's sections, written by the bridge solver as parts: each plank a part, its pivot on the rope) and kinematic planks (S90) on the kinematic bodies S89 adds.

### The village filler

The input is a region (a polygon on the ground), a seed, a house count and style mix, and rules for paths, fences, wells and a clearing. The steps are:

1. **Lots** by the city's plan logic scaled down: a path graph first from a centre (a well, a green) to the region's edges, then lots along paths.
2. **Each lot's footprint** sits on the **tile source's heights**, the way the ruins ask theirs. A lot whose slope under the footprint exceeds the style's plinth or stilt allowance is left empty.
3. **Each house** is the rural filler's description for its lot.
4. **Paths** as ground bands, **fences** as kit runs along lot edges, and **wells, woodpiles and carts** as placements.
5. **The forest's clearing**: the village's outline and paths are footprints that scatter (and G55's vegetation placement) honours. This needs [scatter's](../subsystems/scatter.md#not-yet) footprints as shapes rather than circles.

Its output is records the author then owns ([§14.5](#145-the-authoring-workflow)). Two houses with the same description share one built mesh in the derived-data cache.

### The kit of parts

Beams (square or hewn), planks (with a seeded warp and edge wear), posts, pegs, rope (a twisted helix of strands at a declared radius), thatch bundles, shingles, clay tiles, logs, coursed stone, and the living-wood members. Each is **solved**: a function of a few parameters and a seed, made at the size it is used, with UVs in metres from the part's own origin.

The kit is the shared vocabulary for two reasons:

- **Consistency.** Every plank in a village is the same construction at different lengths.
- **Destruction later** ([§14.8](#148-destruction-and-interaction-later)). A part knows what it is, so the support graph comes from the description, not from guessing at a mesh.

A building is meshed as **one merged exterior, one merged interior and a mesh per door leaf**, not as thousands of instanced parts. The DAG can then simplify across part boundaries in the far field. Instancing parts would multiply instances and culling pairs by the part count, and hierarchical instancing (R66) does not exist. Identical descriptions still share their meshes, and every door of one construction and size shares one leaf.

## 14.4 Walkability and collision

The walker's numbers are the design limits. The nav agent is held to them too (set `agent_max_climb` to the walker's step for a structures scene), so a path the navigation finds is one the player can walk.

| Element | Rule | From |
|---|---|---|
| Stair riser | ≤ 0.25 m (style default 0.20 m) | Walker step 0.35 m, with margin for the capsule's round bottom on a tread edge |
| Stair tread (going) | ≥ 0.28 m at the walking line, 0.6 m in from the inner edge on a spiral | Capsule radius 0.3 m |
| Ramp | ≤ 30° (style default 15°, a tree's wrap-round ramp up to 25°) | Walker slope 40°: 30° is the margin |
| Headroom | ≥ 2.1 m clear, so a spiral's pitch is ≥ 2.1 m plus the tread's depth per turn | Capsule 1.8 m plus a margin; nobody jumps on a stair |
| Doorway | ≥ 0.8 m wide, ≥ 2.0 m high, measured with the leaf open | Capsule 0.6 m across; the ruins' walk test stops it 0.32 m from a face |
| Door swing | the leaf's arc clear of every wall, railing and other leaf; ≥ 0.9 m of floor clear inside the arc | A leaf that cannot open is a wall; a walker standing in the arc stops it ([below](#doors-the-first-kinematic-bodies)) |
| Railing | ≥ 1.0 m high, a collision surface; balusters gaps visual only | Only the capsule's round bottom supports it, so a rail is a wall at any lean |
| Rope bridge sag | ≤ 0.15 of the span, so the end slope is ≤ about 31° | A catenary's end slope ≈ atan(4 · sag / span): 0.10 gives 22°, 0.15 gives 31°, 0.2 gives 39° (too close to the limit) |
| Plank gap | ≤ 0.10 m | The 5 cm collision proxy keeps a gap; the capsule does not fall through a gap narrower than its round bottom, but a foot-sized gap should read as one |

**Static structures need almost nothing new.** A platform, a stair, a ramp, a fixed bridge and a house are placements. Scene collision gives each a static proxy cut at 5 cm, which keeps doorways and stair treads (a 0.20 m riser is four times the cut's error). The walker climbs a riser by its step rule. So **stairs work today** if the riser stays under 0.35 m, and a ramp works up to 40°.

**What has to be added** (S89):

1. **A no-collision flag per part**, carried in the container and honoured by the proxy cut. A roof's thatch overhang, a rope's strands and a living-wood railing's twigs should not be walls. The walk surface of a rope bridge is its planks, and the hand ropes are a rail. Today only terrain, skinned instances and water get no body, and the flora stage's wood-only rule is flora's own.
2. **Collision proxies for parts above a tile's ground**: a platform 40 m up is in the same tile as its tree. Scene collision already makes a compound a tile, so this is a check, not a change, but a large tree's proxy will be the biggest compound it has seen.
3. **Walking a scene with no terrain** (a platform-only test scene). This is the walker's "next step" in [apps](../subsystems/apps.md#walking).
4. **Validators in the description** for every rule in the table, so a style or an edit that breaks one is refused with the rule's name, as the city's are ([validators](../subsystems/city.md#validators)).
5. **Kinematic bodies in scene collision**, beside its static ones: the first moving thing a scene holds. Doors are the reason (below); S90's planks take the same path later.

### Doors: the first kinematic bodies

A working door is in the proof of concept, and it is the scene's **first kinematic body**: scene collision is static today, and the physics world's `move_kinematic` is tested but used by nothing in a scene ([physics](../subsystems/physics.md#the-layer-table)).

- **The record.** An `engine.structure.Door` is a child of its building's record: the opening it sits in, a hinge side and swing direction, the open angle, an angular speed, and its **state** (`closed`, `open`) with the angle it has reached. The state is the document's, written back at `Persist` through a `@writeback` row, so a door left open in a saved game is open when it is loaded — the "villages change state" the owner asked for.
- **The body.** The door's leaf is its own mesh ([§14.3](#the-mesher)) and its own **kinematic box** in the kinematic layer, sized to the leaf, made by scene collision with the building's static compound (which leaves the doorway open). Each tick the door's angle moves towards its state's target by the angular speed times the tick, in integer millidegrees, and the box is moved there by `move_kinematic` at the tick's rate; the drawn leaf follows through `SceneRenderer::move_instances`, which a dynamic scene carries through the flip without a wait.
- **Who opens it.** The walker's **interact** action (an input binding, so it is in the input log) toggles the door whose leaf the eye's ray meets within reach; a game module toggles any door through the host's game contract, at a tick. Either way the toggle is an input to the tick, so **a replayed walk opens the same doors at the same ticks** and the walk's hash covers them.
- **The walker against it.** The walker's sweep sees the kinematic layer, so a closed door is a wall and an open one a wall at its angle. A virtual character is not pushed by a body moving into it, so before each tick's move the door casts its leaf's box along the arc it is about to sweep against the walker's capsule; **a door that would enter the walker holds its angle** until the walker moves, as a real door stops at a body. The walker standing on nothing that moves, the slope hold and the ground velocity are as they were.
- **Navigation.** A doorway is on the nav mesh with its leaf open; a closed door is an area an agent opens before it crosses, through the same toggle.

**A swaying bridge needs a moving walk surface** (S90). Each plank becomes a kinematic box moved by `move_kinematic` to the rope's solved pose at the tick, on the kinematic bodies S89 adds. The walker takes the ground's velocity when it stands on one, which the backend already gives. The rope itself is [05 §5.15](05-simulation.md#515-capability-inventory)'s "Ropes and cables" (S61): a two-ended anchor attachment, simulated at LOD0 and a catenary fit beyond. Drawing it goes through the deformed-vertex pool. Until then a bridge is static and does not sway.

## 14.5 The authoring workflow

**A structure is a document record holding its description**, meshed on read through the derived-data cache, as `engine.terrain.Heightfield` is solved on read ([tileset](../subsystems/tileset.md#the-record)). The record is the description, not a recipe for one. These are new types (D37), in a `schemas/structure.schema` beside the scene's:

- **`engine.structure.Building`**: the description ([§14.3](#the-description)) in the record's own frame — the wall chains, openings, floors, rooms, roof forms — a style name, a `worldpos` and yaw, and the filler's provenance when a filler made it.
- **`engine.structure.Door`**: a child of its building, as [§14.4](#doors-the-first-kinematic-bodies) describes, with its state.
- **`engine.structure.Platform`**, **`.Stair`** and **`.Ramp`**: an **anchor** (the tree's record id, a part id, a parameter and an azimuth), a size, a style and railings.
- **`engine.structure.Bridge`**: two anchors (each a tree anchor, a platform record's edge or a ground point), a kind, a sag ratio, a width and a plank spacing.
- **`engine.structure.Village`**: a region polygon, a seed, its rules, and the provenance of the fill that wrote its houses. Its houses are its **children**, ordinary `Building` records written by the village filler; there is nothing to pin, because nothing regenerates them.

`doc_scene` maps a record to its meshes in the cache: the first read builds, a moved record is a move, and a changed description is a rebuild of that record's meshes. **An anchored record moves with its tree.** Its transform is the tree placement's transform times the anchor's frame, which needs the placement hierarchy that doc_scene's "not yet" names. It also needs the tree's built container to be read before its attachments, so a tree's recipe change rebuilds its platforms. The anchor names a part id, and part ids are stable across the tree's LOD ladder but not across a change to its recipe. A recipe change that removes the part leaves the platform refused, with the reason in the record's status, rather than floating.

**What the owner does, step by step:**

1. Paint or import the ground (W66).
2. Place two ancient trees from recipes, which needs `import_mesh` to accept `.flora.json`.
3. Pick a limb or the trunk at a height with the **anchor tool** in the viewport. The panel shows the anchor the click wrote: the part, its height, its radius and whether it bears load.
4. Choose "platform" and a style; adjust its size in the properties panel (`schema.describe` drives it).
5. Pick a second anchor on the other tree and choose "rope bridge".
6. Draw a footprint with the **footprint tool** (points on the picked ground, not on y = 0), ask for a fill, and edit what comes back: move a door along its wall, drag a wall's point, change a room's role. Or draw a village region, set a house count and a style mix, and fill it; every house is then a record to edit like any other.

At every step the result is drawn within a rebuild of one record's meshes (an estimate: a house's mesh plus its cluster build at well under a second, to be measured).

**The anchor picker is a viewport tool**, not a panel. The viewport is already native, its pick already returns the pair and triangle under a click, and its gizmo's results already go through the document as one `doc.apply` a gesture ([apps](../subsystems/apps.md#engine-editor-the-engines-window-on-a-document), [ADR-0054](../adr/0054-the-tools-ui-is-dear-imgui-drawn-by-the-renderer.md)). The anchor tool is a third tool beside move and rotate, chosen the same way (`editor.tool`, a key while the viewport has the keyboard, the shell's toggle). A click resolves the pick to an instance and through `doc_scene` to a **tree record**; the triangle's vertices carry a part id (section 36), the structural table gives that part's axis and radius (section 37), and the click's point gives the parameter and azimuth. The tool draws the anchor's ring on the overlay and **writes the anchor to the selected record** in one transaction; a part that is not straight and load-bearing is refused in the viewport's corner, naming the part. The web shell's properties panel shows the anchor's fields and edits them like any others. There is no split across toolkits beyond what the gizmo already is: the viewport resolves a click, the document holds the answer, the panel shows it. The footprint tool is the same shape: points on the picked surface, written as one transaction, the polygon drawn on the overlay.

**What the editor needs** (A42): creating a record of a chosen type, placing a recipe-made mesh, the anchor tool, the footprint tool on the picked surface (which also moves a wall chain's points), the fill as a command over `structure.fill`, and a scale handle. Parameter panels come from the schema already.

**The game's repository** holds the village's records, its styles (the elven one at least), its seeds, its generated props and textures, and any filler of its own, which it registers through the registry and needs no engine change. The engine's test scene for all of this is neutral: two ancient trees, the default styles, no game names.

## 14.6 Materials

Every kit part names a **role**. The rural roles: `timber/oak`, `timber/pine`, `timber/weathered`, `bark`, `thatch`, `shingle/wood`, `tile/clay`, `rope`, `plaster`, `stone/ashlar`, `stone/rubble`, `iron`, `cloth`. The organic roles: `wood/living` (living-wood members, which take the tree's `bark` where they graft onto one), `wood/carved` (trim and panels), `stone/pale`, `glass/lantern` (a globe's glass, emissive), `leaf/thatch` (shell roofs laid in leaf courses) and `metal/silver` (fittings). The **material pack**, which G60's material lab work is producing, answers each role with a tiled PBR set (base colour, normal, roughness, height) at a `tile_m`. A game may override any role, which is where the elven palette lives. The rocks and the flora recipes already carry a `tile_m` per set ([rocks](../subsystems/rocks.md#uvs-and-materials)), and the two types should converge with this one on a single role-keyed type.

**The limits today:**

- **Tiling only.** Repetition shows on large plaster and thatch faces.
- **No decals:** no dirt at a wall's foot, no moss on a north face, no grime under eaves.
- **No layered materials or blends** (R21), so weathering cannot fade over a beam and living wood cannot fade into the bark it grows from; the graft's join is a shared role, not a blend.
- **One material per cluster.** Each role a building uses adds clusters at its boundaries, so a cottage of eight roles costs more clusters than one of three.
- **UVs within ±16** (`geometry.uv_range`), so a part's UV run is at most 32 tiles. Meshing UVs from each part's own origin keeps every part inside it, and a curved wall's *u* runs along its arc length from its segment's start.

**What would help, in order:**

1. **Trim sheets**: one texture holding planks, battens, trims and edges, one role, many parts. This cuts the material count, and with it the cluster count, more than anything else.
2. **Per-vertex or per-part tint and wear**, written by the mesher from the seed, so two oak beams differ.
3. **Decals at mesh time**, projected by rule (foot, eaves, around openings).
4. **R21's layered materials**, for moss, wet, and the graft.

## 14.7 LOD, determinism, budgets

**LOD is the cluster DAG's.** A building's exterior is one mesh, its DAG built like any other. Far away its roof and walls collapse to a handful of clusters, and the thin parts (rope, rails, thatch fringe, living-wood twigs) go first. Beyond the DAG's root the far field wants a **village impostor**: a per-tile merged proxy or the vegetation impostors of R67 generalised. Neither is needed for a proof of concept seen from inside the village.

**Budgets, estimated.** These are numbers for the first build to measure, not results.

| Item | Finest triangles (est.) | Clusters at 124 tris, finest (est.) | With the DAG, about ×2 |
|---|---|---|---|
| Timber-frame house, exterior | 30k | 240 | 480 |
| Timber-frame house, interior (rooms, stairs, furniture: 1.5× the exterior) | 45k | 360 | 720 |
| Round hut, exterior (rural or organic) | 12k | 100 | 200 |
| Round hut, interior | 18k | 150 | 300 |
| Door leaf (shared by every door of one construction and size) | 1.5k | 12 | 24 |
| Platform with railings | 5k | 40 | 80 |
| Rope bridge, 20 m | 8k | 65 | 130 |
| Spiral stair, one turn | 6k | 50 | 100 |
| Globe lantern (glass and fitting) | 1k | 8 | 16 |

A house with its interior is 2.5 times its exterior, the estimate of 19 October. At about 1.4 KB a cluster (two 48-byte descriptors, 64 vertices at 14 bytes, 124 triangles of indices), a **village of 50 houses** is 24,000 clusters (about 35 MB) of exteriors if every house is unique, and 60,000 (about 85 MB) with every interior resident. That is what P30's interior cells (unloaded, coarse, active) are for at scale, and why the mesher writes the interior as a mesh of its own: with the interiors of the eight houses nearest the walker resident, the same village is about 30,000 clusters and 42 MB.

**The proof of concept, re-estimated with interiors** ([§14.9](#149-the-first-proof-of-concept)): eight houses at 1,200 clusters (9,600), two huts at 500 (1,000), five platforms (400), three bridges (390), one stair (100), a well (about 40), the door leaves (two constructions, 48) and a dozen lanterns (one shared mesh, 16) — **about 11,600 clusters and 16 MB of structures**, against about 5,200 clusters and 7 MB for the same scene with exteriors only. The two ancient trees add at most 11,300 clusters and 16 MB (the default 100 m tree's container is 5,631 clusters; a 40–60 m recipe on the same budget is no larger). **Every interior cell in the proof of concept is resident**: ten interiors are 7,000 clusters and about 10 MB, which no streaming is worth, so all of them are loaded at the scene's load and drawn by the cull pass like everything else, and the interior culling a closed door gives is the occlusion pass's. The interior meshes being separate is what lets a larger village make the same scene's interiors cells later without re-meshing anything.

**Determinism** is flora's ([flora](../subsystems/flora.md#determinism)): every decision in integer centimetres and fixed-point, no libm (polynomial `cosh` for the catenary, `det::sin` for arcs and the helix, polynomial Béziers), the seed through the engine's hash. The output is the same bytes on every toolchain and pinned by golden section hashes. The cache key is the description's canonical bytes plus `k_structure_cache_version`. For an attachment, the key also covers the tree container's sections 35–37 hash, because the anchor reads them. A filler is deterministic too — the same parameters, seed and ground give the same description — but that matters only to the filler's own tests: its output is document data, not a cache entry, and nothing re-runs it.

## 14.8 Destruction and interaction later

- **Doors** are no longer later: they are kinematic bodies in the proof of concept ([§14.4](#doors-the-first-kinematic-bodies); S89 for the bodies, S91 for the door itself). What stays later is a door that breaks, locks or is barred, which a game builds on the same record.
- **Breakable walls and collapsing huts** take plan 05's support graph ([05 §5.8](05-simulation.md#58-destruction), S46–S49). Because a building is described from parts, the support graph comes **from the description**: posts, beams, joists, wall panels, roof members, and for a platform the tree limb it rests on (G63's load-bearing flag). There is no heuristic over a mesh. A cut limb is a collapse event for every platform on it. A broken wall is an edit of the description written back to the document, so the damage persists the way a door's state does.
- **Fire** is [05 §5.15](05-simulation.md#515-capability-inventory)'s "Fire, heat and burning": a thermal block per material category, with burning as damage. Thatch and timber roles carry the category, so a solved building is ready for it when it exists. The renderer's fire (R85) can already draw a burning thatch's flame and light; no roadmap row opens the simulation here.

## 14.9 The first proof of concept

**Target** (P48): two ancient trees (G63's megaflora, 40–60 m), **five platforms** (three on one tree, two on the other, at 8–25 m), **three rope bridges** (one between the trees, two between platforms on one tree), **two round huts** on platforms in the `organic` style — curved walls, a shell roof, living-wood railings and brackets grafted to the trunk, globe lanterns — **one spiral stair** from the ground to the first platform, and **one ground village of eight timber-frame houses and a well** in a clearing beside them, in the `rural` style, textured by role from the pack. **Every house and hut has rooms and doors that open**: two to four rooms a house with furniture from the kit, a door on every doorway to the outside and between rooms.

The scene is walkable. The walker climbs the stair, crosses every bridge, opens a door into each hut and house and walks its rooms, and is stopped by every railing and every closed door. It is captured from the ground, from inside a house and from the highest platform at dusk with the lanterns lit, and it opens and edits in the editor.

**Acceptance numbers to measure:**

- Every structure passes its validators, and the mesher's conformance test passes on every description in the scene.
- A walk through the stair, two bridges, a hut and a house — opening the doors it passes — completes with no drop (the walk summary's `drops` is 0), and its replay is the same hash with the same doors open at the same ticks.
- A door held by the walker standing in its arc stops, and closes once the walker steps out.
- A door left open in a saved game is open after the load.
- The scene's cluster count and memory, against [§14.7](#147-lod-determinism-budgets)'s 11,600 clusters and 16 MB of structures.
- A frame's GPU time along a camera path through it, outside and inside.
- The mesh and build time of each record.
- The rebuild latency after one edit in the editor.

**Order of work:**

1. **G64 and G70** together: the kit of parts with the living-wood members, and materials by role from the pack (G60), both style tables' roles among them. The kit can be tested in isolation: every part's bytes pinned, every part drawn. **S89's kinematic half** can start beside them: kinematic bodies in scene collision, moved by `move_kinematic` at the tick, the walker against them and the swing held by the capsule, tested with a plain kinematic box.
2. **G65**: the description type and the mesher on curves — lines, arcs and Béziers from the start, gable, hip, cone, shell and sweep roofs, exterior, interior and door meshes, the validators, the conformance test.
3. **D37**, **G72** and the rest of **S89**, in parallel: the `engine.structure.*` records holding the description (the door's state with its write-back) and the placement hierarchy for anchors; the rural filler; the no-collision flag, the validators against the walker, the platform scene with no terrain and the nav agent at the walker's numbers.
4. **G66** (interiors: rooms, inner walls, stairs between floors, furniture from the kit) and **S91** (the door: its record, its leaf, the interact action and the game's toggle, navigation), in parallel once 2 and 3 are in.
5. **G67**: platforms, railings, the spiral stair, huts on platforms with living-wood brackets grafted to their trunk, against G63's anchors.
6. **G68**: rope and plank bridges, static.
7. **R86**: the steady lamp for the globe lanterns, beside 4–6 (it needs only R85).
8. **G69**: the village filler (lots on terrain, paths, fences, a well, the clearing), writing records.
9. **A42**: the editor's record creation, recipe placement, anchor tool, footprint tool and fill.
10. **P48**: the scene, the walk, the captures, the numbers, and the owner's walk through it.

Steps 1's two halves run in parallel from today. Step 2 is the long pole: the mesher on curves is one agent's work and nothing else in the list but S89's kinematic half and R86 can start before it lands. Step 3's three can run in parallel, and step 4's two once G65 and D37 are in (S91 also needs S89). Steps 5 and 6 can run beside step 4 once D37 is in. After the proof of concept come **S90** (sway), **R87** (lights past four) and the game's own elven table.

## 14.10 Decisions and risks

**Status note (2026-10-20): the owner's four answers are decisions.** The chapter of 19 October ended with four open questions; each is settled, and the sections above are written to them.

1. **The description is the authored artefact; the grammar is a filler** (§14.1's stance, [§14.3](#143-the-description-the-mesher-and-the-fillers), [§14.5](#145-the-authoring-workflow)). The document holds the shared building description as `engine.structure.*` records, one deterministic mesher turns it into meshes, the rural grammar and the village filler only write descriptions the author then owns, and an engine upgrade may move triangles but never rooms or doors. This answers the first question as recommended: the city's description is reused, generalised with curves, and the city takes the same mesher for its proxies later (P27); its urban grammar becomes one more filler.
2. **Interiors and working doors are in the proof of concept** ([§14.4](#doors-the-first-kinematic-bodies), [§14.7](#147-lod-determinism-budgets), [§14.9](#149-the-first-proof-of-concept)). G66 and S91 are P48's dependencies; a door is the scene's first kinematic body, which S89 adds to scene collision; the proof of concept is about 11,600 clusters and 16 MB of structures with every interior resident. This answers the second question.
3. **The organic style is curved, so the first mesher is built on curves** ([§14.3](#the-mesher), [the living-wood kit](#the-living-wood-kit), [globe lanterns](#globe-lanterns), [§14.6](#146-materials)). Walls are polylines with arcs and Béziers, roofs are surfaces over a footprint, there are two style tables (`rural`, and `organic` for the game's elven table to build on), a living-wood kit copied from flora's helpers and grafted onto trees' anchors, and globe lanterns as emissive props with a steady light (R86, because the fire's light does not cover it). This answers the third question: there is no second mesher.
4. **The anchor picker is a viewport tool** ([§14.5](#145-the-authoring-workflow)). A click resolves to a tree record and a structural part and writes the anchor to the record through the document, which the web shell's panel shows and edits. The fourth question is dropped: there is no split across toolkits beyond what the gizmo already is.

**Risks:**

- **The megaflora's own cost.** A large tree is the biggest single mesh and the biggest collision compound the engine will have seen (the default 100 m tree is 5,631 clusters; G63's numbers). Every platform sits on it.
- **A curve mesher's complexity in v1.** Wall junctions on curves (a mitre between two arcs of different radii, a T into a spline), openings on a curved wall, and roofs over a curved footprint are where a mesher's bugs live, and v1 takes all of them at once rather than after a straight-walled version. The mitigations are the conformance test over a corpus that starts with the curved cases, the validator that refuses an arc tighter than its wall, and building G65 with straight walls passing first inside the same type, so the rural proof of concept never waits on a shell roof.
- **Kinematic bodies and the walker.** A virtual character is not pushed by a body moving into it, so the door's swing has to be held by a cast against the capsule each tick; a door that opens into a walker on a stair, two doors swinging into each other, or a walker standing in a doorway while a game closes it are the cases that will find the gaps. Kinematic bodies streamed in with their tile must be made at fixed ticks and in a fixed order, as static ones are, or a replay diverges.
- **The living-wood kit's cost.** Branch-shaped members are tubes with many rings: a railing of grown members is several times the triangles of a sawn one, and the copied helpers are a second copy of flora's kit to keep honest. A railing's budget is measured in G64 before the organic table leans on it, and the helpers are lifted into a shared module when a third user appears.
- **Many lights.** An organic village is lit by its lanterns, and the frame lights four local lights today. Until R87, a dozen lanterns are four lights and the rest emission, which may not read as the owner intends at night.
- **Anchor stability across recipe edits.** Part ids are stable across the ladder, not across a change to a tree's recipe. An edited tree can orphan its platforms. The refusal is loud by design, but the owner may expect platforms to follow a limb that moved.
- **Descriptions outliving the mesher.** Because the description is never regenerated, a change to its schema is a migration of every authored village, with the migration corpus's discipline; a description field added carelessly is forever.
- **Thin geometry in the far field.** Rope, railings and living-wood twigs shimmer and vanish under the DAG's simplification, as the palm's leaflets did. They want the same coverage-preserving treatment, or an alpha card (R65) at distance.
