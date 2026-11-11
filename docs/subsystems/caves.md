# caves (domain, capability)

**Purpose.** Caves a walker enters and explores, as a solver's output. Two stages share the module:

- **The network** (roadmap G146, 2026-11-10; the owner's request for caves a player walks into and explores; the forest proof of concept's solver needs §3.11): a placement generator, `caves`, that plans a whole cave system in the scene's formation from a seed and a small description, finds its openings against the ground, and hands the renderer the network's one closed surface cut into **cells** joined by **portals**, each cell a mesh with its own cluster DAG built through the mesh-generator registry (`cavecell`) and the derived-data cache.
- **The single passage** (G134, 2026-11-06): one swept shell from a hole in the ground in and down (`engine.caves.Passage`, a scene's `<name>.cave.json`, the mesh generator `cave`).

The network is the shape the capability is going to: it is what holds junctions, halls, floors and mouths. The passage stands beside it tonight only because retiring it was not reached; nothing new should be built on it ([Not yet](#not-yet)).

What the capability does not own: the terrain's hole (the terrain's, `scene_gen::TerrainHoles`; the network says what it must be, below), the light inside (the renderer's: [Light](#light)), the collision body (`scene_collision`'s proxy of each cell's mesh), and which cells are resident (the streaming path's; the network says what a portal is, below).

**Grounded.** [The network's grounding](../experiments/cave-networks.md): what decides a cave's pattern (Palmer 1991), vadose passages down dip and phreatic tubes along strike, levels at former base levels, the scallop's Reynolds number (Curl 1966), the beam a hall's roof is and the cover that holds it (Hatzor and others 2010), with each number marked as read that night, derived, or still owed a reading. [The passage's grounding](../experiments/caves.md): the sections' proportions and the stalactite's shape.

## The network

### Why this shape

- **A plan above the tile, a realisation in it** (the solver rules, 2 and 3). A cave crosses tiles and its openings depend on the ground anywhere along it, so one deterministic plan is built once for the whole entry, in `open`, and held: 768 stations and 246 KB for the 825 m test network. Everything a tile is asked for is read from it.
- **Laid out in the bed's own coordinates.** A station is placed by (along strike, down dip, across the beds) from the member's middle, and only then turned into a point of the world through the formation's own function (`gfx::formation_coordinate`, folds included). A passage therefore stays in the rock without being checked into it afterwards, a tube's long axis is in the bedding because that is the frame it is drawn in, and a level is where the bed meets a height because that is how its dip offset is solved.
- **A field on the world's lattice, not a sweep.** The single passage is a swept ring: 79 triangles a metre (5,064 for 64 m), and no way to join two of them, give one a floor, or cut one by the ground. The network's surface is the zero of one scalar field sampled on a lattice and meshed by surface nets. Measured on the test network: **503 triangles a metre of passage at the default 0.25 m lattice** (415,016 for 825 m: 320,167 rock, 57,391 sediment floor, 14,076 water, 11,458 breakdown, 11,924 apron), **120 a metre at 0.5 m**; the proof of concept's hand-built cave was about 900 a metre on the same 0.25 m lattice. That is six times the sweep before LOD, and it buys junctions, halls, floors, sumps, mouths and bedding ledges as one closed surface with no special case anywhere. `caves.md` rejected a voxel mesh for one passage on that budget; for a network the trade goes the other way because (a) the surface is cut into cells and a cell nobody can see is not drawn or (after the streaming path's change) not resident, (b) each cell has a cluster DAG, so what is far down a passage is drawn coarse, and (c) the sweep's saving was in a case, a single tube, that a network is not. `voxel_m` is the knob (0.125, 0.25 or 0.5 m): a finer lattice quadruples the triangles and carries scallops half as long.
- **A cell's recipe is everything its surface is a function of.** A mesh generator is handed a file and no ground, so a cell's `.cavecell.json` carries the stations and halls whose bounds reach the cell, the formation, the water table, and the ground's heights under any mouth near it, all in whole millimetres. The field is evaluated from the plan's anchor in every cell alike and a vertex lies on a 1/1024 m grid of the world, so **a vertex two cells share is one point exactly** and nothing is welded or stitched: the cells are cuts of one surface.

### The plan

`plan_network(network, ground, tile, plan, error)` (`network.h`, `src/network_plan.cpp`). In order:

1. **The host rock.** The scene's formation (`engine.scene.Formation`, W85) is repeated in the entry (`Network.formation`), as the cliffs' layer does, because a placement generator is handed its own entry and nothing else of the scene; with none the beds are level through `origin`. `member` names one of its `macro_beds` or `beds`: the occurrence nearest `origin` is the host. `member_margin_m` (1 m) is how far every passage stays from the member's top and base: a tube's half height is capped by it, a canyon is cut no deeper than it, a loop's amplitude is what is left. The test network's least rock between a passage and the member's edge is 5.49 m. Halls and shafts are exempt and the page says so: breakdown stopes upward out of the soluble bed, and that is how a hall reaches daylight.
2. **Base level** is the lowest ground within the reach (`base_level_auto`), sampled every 8 m: the valley floor. It is the water table.
3. **A trunk a level**, along strike where the member's middle is at the level's height: in dipping beds that is `level_spacing_m / sin(dip)` further up dip for each level (65 m at 8 degrees and 9 m), in level beds the trunks are stacked across the beds and a level the member does not hold is left out with a sentence. A trunk meanders (about 11 widths long), loops gently across the beds, widens downstream as the root of the distance, and is a tube twice as wide as high unless that leaves a walker no headroom, in which case it is as round as 1.2 to 1 (the grounding's range). Downstream is the end whose ground is lower. The lowest trunk's head plunges 4 m under the water table: a sump.
4. **Feeders** (canyons, keyholes for their first third) come into each trunk from up dip, `feeders_per_100m` of them, **graded to the trunk's floor**: the canyon's floor meets the trunk's at the junction. **Connectors** run from each level down dip to the next, graded at both ends: two a pair of levels, which is the network's loops. A **bypass** leaves the lowest trunk and rejoins it. **Crawls** are tubes too low to walk that pinch out.
5. **Tried against the ground.** A strand keeps what has `min_cover_m` over its roof. Where the cover runs out, it runs on into daylight if daylight is within twenty stations (a **mouth**), and stops there in a **choke** otherwise.
6. **Halls**: an ellipsoid's upper part 13 to 18 m across the short way over a pile, on the upper levels first. One whose roof has less cover than `collapse_cover` (a third) of its span **has fallen in**: a fissure climbs from the dome to the ground, an opening nobody declared. The same network under a doline over the hall gains a mouth (the test does exactly that).
7. **Leads**: up to `openings` blind ends (a trunk's downstream end first, then the feeders' heads, the shallowest first) follow the bed in the direction of the nearest daylight within `lead_reach_m`, sixteen directions tried, or climb a shaft where the cover is under `max_shaft_m`. A network with no opening is planned, and says "sealed".
8. **The graph**: strands are split where anything joins them into edges between nodes; an edge has a kind and a level, a node of degree one says what it is (choke, pinch, sump, mouth). The test network: 13 strands, 26 edges, 25 nodes, 2 loops, 7 dead ends, 3 mouths.
9. **Clearance.** At every station the walker's capsule (`walker_radius_m`, `walker_height_m`) is tried in the section analytically: floor to roof, and the width at the height of each of the capsule's spheres, with a ledge's depth of slack because the walls' detail is not in the section. An edge is `walkable` when every station passes, no step is steeper than 0.8, and it is not a crawl, a shaft or a sump. `reachable_m` is what a walker reaches from a mouth over walkable edges: 487 of 525 walkable metres in the test network.
10. **Cells, portals, mouths**, below.

Every position is rounded to a whole millimetre before anything reads it; nothing in the plan depends on the tile that asked or on a thread (it is built on one).

### Mouths

A mouth is **found**: the ground's points that lie inside the void. Round every strand end that reached daylight the plan samples the ground on the lattice's columns and asks the field at each ground point; the zero of that is the opening's outline (marching squares, a closed polygon whose every point is on the ground), and the same window of heights goes into the recipes of the cells near it, where the ground cuts the field: rock exists only under the ground, so the tube's end in the open air is not surface, and where the void's floor would stand over the ground the ground is the floor.

**The mesh owns its rim.** Round the opening, out to `apron_m` (1.75 m) from the void, the cell's mesh includes the ground itself, as rock (surface class `apron`): it stands 3 cm over the terrain's surface at the rim and sinks 6 cm under it at its outer edge, so its own edge is never seen and it covers whatever the terrain's hole leaves ragged. This is what a real mouth looks like, bare limestone round the dark, and it is how the picture is right where the hole allows it.

### What the terrain's hole must be

A request to whoever owns the terrain's holes (W87's files were another row's on the night this was written); nothing here edits them.

Today the scene's terrain takes `holes` as authored entries, and a placement generator cannot add one. So `engine-content cave-plan` writes `<stem>.holes.json`, the mouths' holes as `engine.scene.TerrainHole` polygons, to paste into the scene's terrain; the plan carries the same polygon as `PlanMouth::hole`. The polygon is the opening **grown by 0.6 of the apron** (1.05 m), because of the hole's two limits:

1. **A hole is whole cells** (the cells with four corners inside it), so its edge is a staircase reaching up to a cell's diagonal inside the polygon. With the polygon 1.05 m outside the opening, the 0.5 m level's teeth (0.71 m) end 0.34 m short of the opening and the apron (to 1.75 m) covers them; the 1 m level's (1.41 m) reach 0.36 m into the opening, which is what is seen from the distance that level is drawn at. **Asked for:** a hole's rim clipped to its polygon (the cell's triangles cut along the outline), after which the polygon is the opening itself and the apron can shrink to a lip.
2. **A hole is a polygon in plan**, so a mouth in a steep face is a sliver: the test network's mouths are in 28 to 32 degree ground and are 3.8 to 6.5 m deep in plan (`PlanMouth::plan_depth_m` says it for each), but in a 70 degree face a 3 m tube's mouth is about a metre deep, under two cells, and in a cliff it is nothing. **Asked for:** an opening the heightfield's plan cannot say: either a hole given as a volume (the ground's triangles inside it dropped, whatever their slope) or a face mesh that owns the cliff round the mouth, which is the cliffs' generator's shape and would take the mouth's outline in the face's own frame. The plan has the outline as points on the ground with their heights, which either form can take.
3. **A generator cannot write a hole.** The holes are part of the terrain entry and built before any placement opens. **Asked for:** a ground edit with a polygon outline whose kind is "no surface" — the terrain's edits (`TerrainEdit`, W98: a cut, a fill or a bench along a line or over a patch; not in this row's tree, so this is written against its description and not its header) are the place for it, a fourth kind beside those three, given by the mouth's outline as the plan has it (points on the ground, whole millimetres) and read by the terrain's tiles when they are built, as an edit's heights are. With that the paste from `holes.json` goes, and items 1 and 2 are asked of the edit rather than of the hole.

Also asked of the placements' exclusion: the sward, the litter and the scatter stay off the apron, not only out of the hole. With a footprint that can be a polygon (W98's `Footprint`: a disc, a band or a polygon) that is the hole's polygon grown to the apron's reach, `apron_m` from the opening, which the plan can hand over as it hands the hole; today nothing carries it and a mouth's lip will have grass on it.

### The field

`src/field.h`, `src/field.cpp`. One scalar over the world, positive in rock:

- **A segment** between two stations of an edge is its section swept along the chord: a tube is an ellipse in the bed's frame (wide across the passage in the bedding, high along the beds' normal), a canyon a rounded box in the plumb frame, a keyhole both; the frames are interpolated between the stations' tangents so the surface does not facet. A hall is an ellipsoid. Distances are first-order estimates, exact at the surface: products, sums, square roots, a division.
- **Floors.** Each segment's void is cut by its sediment floor (`fill`, 0.22 of a tube's height, with 5 cm of smooth relief so it is not a machine's plane; the floor is the line through the segment's two stations carried on past its ends, because a floor held at an end's height inside the end cap undercut the next segment's and made every sloping passage a flight of steps a station long, which the first pictures showed: `k_network_version` 3), a hall's by its breakdown pile (a cone at about 30 degrees with slabs a bed thick stepped on it). Then the water table cuts everything: under it there is no surface, at it a level one (class `water`): the sump.
- **The union.** Within an edge, the nearest segment. Between edges and halls, a **smooth minimum two lattice spacings wide**, folded in the order of the edges' ids: two voids that meet at a shallow angle meet in a fillet, and the blade of rock between them ends where it would be thinner than the lattice can carry. Without it a bypass leaving its trunk left a knife edge the surface could not hold.
- **The walls' detail**, added to every candidate alike before the union so it is the same number in every cell. **Bedding ledges**: the formation's own beds, each bed's `resistance` (carried by W85 and until now drawn by nothing) standing a resistant bed out by `ledge_m` (0.18 m) and a weak one back, the resistance **averaged over two lattice spacings of bed coordinate** as the terrain's bands are averaged over a pixel, so a parting thinner than the lattice fades into its neighbours instead of aliasing into a slot a cube cannot hold. A hall's roof takes 2.2 times the ledge: a stoped ceiling is steps. **Scallops**: bowls dissolved into the wall on a lattice laid along the flow, each steep on its upstream side and long on its downstream one, the deepest of the bowls a point is in, so two bowls meet in a crest; their length is Curl's `22,500 nu / v` for `flow_speed_m_s` (0.59 m at 5 cm/s) and never under 2.4 lattice spacings. Shorter scallops are real and are the material's ([Materials](#materials)).
- **No section is narrower than 2.2 lattice spacings.** A crawl pinches out at that, not at nothing.
- **Under a mouth, the ground**: the field is the lesser of the void's and the ground's, the air over the ground counting as outside.

Deterministic by construction: every input is whole millimetres, every point is evaluated from the plan's anchor, candidates are folded in id order, and the only transcendental is `det::sin` in the formation's fold.

### Cells

- **Cut.** An edge is cut into cells of at most `cell_length_m` (24 m) of centreline; an edge shorter than a third of that joins the cell it meets; a hall is a cell. The test network is 44 cells. A cell whose every roof is under the water has no surface and no mesh (`PlanCell::flooded`).
- **Who owns a piece of surface.** A quad of the surface (surface nets: a vertex a lattice cube the surface crosses, a quad a lattice edge it crosses) belongs to the cell that owns the point on the quad's air side, and a point is owned by the cell of the nearest station. So every quad of the network is exactly one cell's, a cut between two cells falls midway between two stations, and the vertices on the cut are in both meshes, bit for bit.
- **The mesh** (`solve_cell`): positions in the cell's own frame (its lattice's low corner, where its instance stands), normals from the field's gradient (so they run on across a cut), UVs the world's metres on the plane a triangle faces (so they run on from cell to cell and the bedding reads level on every wall; the seams between the three projections are where this still falls short: [Not yet](#not-yet)), one primitive a surface class: rock, sediment, breakdown, water, apron.
- **The container** (`build_cell_container`): a cluster DAG a class, merged, with the cell's open edges (a portal's ring, an apron's rim) and the seams between its classes **locked**, so two cells meet at every level of detail and a floor never parts from its wall. That lock is also why a cell's coarsest form is not small: the cut and the floor's two edges keep their vertices.
- **Closed.** Over the whole test network at 0.5 m: 118,462 edges, every one with two triangles but the 250 on the aprons' rims and 70 **pinches**, where two sheets of the surface pass through one lattice cube and share its vertex (four triangles on an edge; the surface is still closed, and no edge has an odd count). Surface nets cannot split such a cube; the smooth union, the filtered ledges and the least section are what brought the pinches under 0.06 % of the edges, and a triangle a pinch turns over is turned back to face where the field falls.

### What a host must know about a portal

A request to the streaming path (R142, the same night). Today a placement generator's meshes are all listed and resident at scene read, so `open` writes every cell's recipe and `meshes` names them all: the first read of a scene builds every cell (through the cache), and every cell's mesh is resident whether or not anyone is near. What the plan already holds for a host that loads by what can be seen (`Plan::portals`, `PlanPortal`):

- **A portal**: the two cells it joins (`k_outside` for the open air at a mouth), a point, a normal from the first to the second, and a half width and a half height: the rectangle the opening fits in. Between two cells of one edge it is the section at the cut; at a junction or a crossing it is listed for every pair of cells whose voids overlap, so the list is whole: 66 for the test network, 6 of them between cells that turned out not to touch (they cost a load and nothing else). The test holds the other direction: every two cells whose meshes share vertices have a portal.
- **What I would hand over at activation**: per tile, the cells whose bounds' middle is on it, each as a recipe's key (its source hash: the recipe is content, so the cache and another start agree) and the recipe's text to build through `cavecell`, never the mesh; and beside the placements, the portals of those cells. **The rule a host needs**: a cell is wanted when it is near, *or* when a wanted cell's portal to it is in the view's frustum and nearer than the view reaches, walked from the cell the eye is in (or from the mouths when the eye is outside). A mouth's portal is what makes the first cell wanted from outside; no cell past a portal the eye cannot see through is.
- **What the generator does until then**: in a far ring (`far_ring`, 2) only the cells that hold a mouth are placed, since nothing else can be seen from outside; near, every cell of the tile.
- **`Context::formation`**, when the streaming row's contract lands it: `Network.formation` goes, and the network reads the scene's.

### Dressing

A cell's speleothems are **one merged mesh a cell** (the recipe's `part` 1, built by the same generator), not an instance each: 427 drips in the test network would be 427 traced structures, and the wall stones' lesson (1,491 dropped structures for 15,866 instances) is the same one. It is its own mesh, with its own DAG and its own role (`cave/calcite`), so it leaves the picture before the walls do.

**By rule on the plan.** Drips stand in **rows along the joints** the water comes down: a joint every 2.5 m of an edge, `drip_per_m2` saying what share of them is wet, a wet one a row of drips across the passage; over a hall, rows across its floor. Each drip is marched up to the rock and down to the floor in the cell's own field, then by hash is a soda straw, or a stalactite of the ideal profile (radius as length to the 3/4) with, seven times in ten where the floor is dry, a stalagmite under it, a **column** where the two would meet, and a **flowstone boss** where the floor under the drip slopes. The test network: 427 drips, 186 stalagmites, 11 columns, 28 bosses, 58,336 triangles, within each cell's `triangle_budget`. A drip is not placed where the roof over it is within half a metre of the ground or past it (a mouth's tube runs on into the open air, and the first pictures had a stalactite hanging in the sky there), and every body is sunk 8 cm and three quarters of a lattice step into the rock and the floor, because the mesh's roof stands that far off the field's zero the drip was marched to (`k_network_version` 2). Draperies and flowstone as a sheet on a wall are not built. **A cell no drip falls in has no dressing mesh**: an empty mesh is not one a scene can load (the first scene read of a network stopped on exactly that, `merge_cluster_lod: no parts`), so `open` solves a cell's dressing once to see, a few milliseconds, and leaves a `.bare` marker beside the recipes for a cell found bare so the next read does not solve it again.

### Collision and clearance

The walker's collision is `scene_collision`'s proxy of each cell's mesh, as it is for any mesh: nothing here is new to it, and a cell's floor, walls and the water's surface are in the mesh. What the network adds is the plan's promise and its test: an edge the plan calls walkable passes a capsule, checked in the field at every second station (the capsule stands where its lower sphere first fits within a 0.4 m step of the lowest point under the centreline, because a floor is not a plane) and by rays in a cell's mesh at the quarter-metre lattice. Whether a streamed cell's mesh has a collision body at all is the streaming path's (the proof of concept's "a kept instance has no collision body in a streamed world").

### Materials

The roles a network's surfaces take from a pack: `rock/limestone` for the walls and the apron, `cave/sediment` for the floors, `cave/breakdown` for the piles, `cave/calcite` for the dressing; the water's surface is a dark smooth colour. Every set is placed stochastically (no repeat down a passage). The engine's one pack has none of these: the limestone and the breakdown fall back to its `rock/sandstone`, and the rest draw their flat colours. **Wanted in a pack**: a limestone with fine scallops in its normal map (the scallops under 0.6 m the lattice does not carry), a cave sediment (silt and cobbles), a fresh-broken limestone for breakdown, a wet calcite.

### The generator

`src/network_placement.cpp`, the placement generator `caves`, with the per-tile contract (`open`, `meshes`, `tile`, `occupies`, `representation`, `expand`). `open` plans and writes the recipes under `recipes`, or `caves` under the derived-data root, and beside them, named by the plan's hash, `network-<hash>.holes.json` (the holes its mouths ask of the terrain, for a person to paste into the scene's terrain until a placement can write one) and `network-<hash>.survey.json` (where things are: the mouths with the way out of each, the halls, every edge's floor under its centreline every fourth station; what a camera, a light or a prop is placed by); `tile` places the cells whose bounds' middle is on the tile, each instance at its cell's lattice corner with no rotation; a far ring keeps only mouth cells. A scene's entry is `{"generator": "caves", "params": { an engine.caves.Network }}`; the `Scene` root gains no field.

### The command

`engine-content cave-plan <network.json> <out dir> [--ground <valley>,<slope_x>[,<flank_m>]] [--doline <x>,<z>,<radius>,<depth>] [--tile <m>] [--cells] [--build]` plans a network against a hillside given by a few numbers (`y = max(valley, slope_x * x - max(0, z)^2 / flank)`) and writes `<stem>.plan.svg` (a plan view: passages at their width, coloured by kind, halls, mouths with the hole each asks for, dead ends named, portals), `<stem>.section.svg` (a long section along strike with no vertical exaggeration: roofs, floors, the ground over each trunk, base level), `<stem>.holes.json` and `<stem>.survey.json`; one JSON line of the plan's numbers; with `--cells` every cell solved (triangles a class, milliseconds), with `--build` every container built. It needs no GPU.

### Budgets

Measured on the test network (two levels in 24 m of limestone dipping 8 degrees, 160 m of strike, 825 m of passage, 44 cells, 3 mouths), the machine shared with seven other builds (an upper bound, not a quiet number).

| What | Number |
|---|---|
| The plan, resident a network | 246 KB (768 stations, 26 edges, 44 cells, 66 portals, three ground windows) |
| Scales with | metres of passage (a station a metre); the mouths' windows |
| Triangles a metre of passage, near | 503 at 0.25 m; 120 at 0.5 m |
| Triangles a cell at 0.25 m | 9,400 mean (415,016 in 44); 10,210 for 23.5 m of trunk |
| Dressing | 137 triangles a drip; at most `triangle_budget` (20,000) a cell |
| Recipes on disk | 3.5 MB of JSON for 78 files (two a cell) |
| Lattice points sampled a network at 0.25 m | 15.6 million |
| A cell's coarsest form | 11 clusters a container on average (863 root clusters in 78 containers of 13,177 clusters), each at most 124 triangles: under 1,400 triangles against 9,400 at full detail, because the cuts and the floors' edges are locked |
| The plan, built once at `open` | 27 ms in `msvc-release` (14 ms for one level over 160 m of strike, 40 ms over 640 m: `caves.plan_network`) |
| A tile's activation | a scan of the entry's cells (44 here) for the tile's: no solve, no ground query |
| A cell's solve, on a derived-data miss | 254 ms at 0.25 m, 34 ms at 0.5 m (`caves.solve_cell`, a trunk's cell); 22.4 s for the network's 78 solves, the worst cell (the hall) 1.17 s |
| A cell's container (the DAG) after its solve | 0.32 s mean: 25.0 s for the 78 |

All of these in `msvc-release` with other processes using 95 % of the CPU and the GPU busy (the bench's own warning): upper bounds. The solve samples every lattice point of a cell's bounds, 15.6 million for 415,016 triangles; sampling only near the surface is the obvious saving and is not done ([Not yet](#not-yet)).

## The single passage (G134)

A recipe (`engine.caves.Passage`, a scene's `<name>.cave.json`) and a seed in, one mesh out: the passage's shell — a closed surface wound to face inward, open only at the **collar**, the ring that meets the terrain's hole — and its stalactites, soda straws, stalagmites and columns, one mesh in the entrance's frame.

### The shell

- **The centreline**: the entrance, a vertical shaft of `shaft_m`, then a Catmull-Rom spline through the recipe's `points` (metres from the entrance), sampled every `ring_spacing_m` by arc length.
- **A ring a sample**, `ring_vertices` round, in the ring's frame: its tangent, and the profile's up — world up off the tangent, leaning to the passage's first heading where the tangent is near vertical. The shaft's rings are level circles of the hole's radius; the passage's first two metres blend from that circle into the first segment's profile.
- **Profiles** (`profile_point`): a phreatic tube is an ellipse `width × width / 2`; a vadose canyon a superellipse of exponent 4, `width × 3 width`; a keyhole the tube over a canyon 0.35 of its width and 1.2 tube heights deep.
- **Bounded roughness**: a smooth integer-hash value noise, at most `roughness` (4 %) of the distance from the axis, faded in over the first metre so the collar is exact.
- **Wound to face inward**, normal cones on; the far end is a rounded cap; the only open edge is the collar ring, **locked** in the DAG.
- **The collar** lies on the hole's circle at the ground: radius `entrance_radius_m` to a millimetre, at y 0, or at `collar_heights_m` for a hole in sloping ground, the recipe's ring **resampled by angle** (G134, 2026-11-08), faded out down the shaft. The terrain's opening is the cells wholly inside the hole ([terrain](terrain.md#holes)).

### Speleothems

Drip points over the ceiling, `drip_per_m2` per square metre; each a **soda straw** or a **stalactite** of the ideal profile with, seven times in ten, a **stalagmite** below; a **column** where the two would meet. Each body is a closed surface of revolution sunk 8 cm into the rock. **The triangle budget** (`triangle_budget`, 20,000) is per passage. The test passage is 5,064 shell triangles and 81 drips in 8,576.

## Light

Interior light is the renderer's. A carried light and the cave's indirect light are not this capability's; past the first cell from a mouth a network draws dark under a mode that shadows the sky and too bright under one that does not. Traced shadows hold the sun and a point light correctly inside (the surface is closed).

## Owned data

The network's and the passage's types (`schemas/caves.schema`), the plan, the solves. Nothing else: a cell, once built, is a `.clusters` container like any other.

## Invariants

- Every station of a passage lies inside the soluble member by `member_margin_m` across the beds (halls and shafts exempt, as said).
- A mouth's outline is a closed polygon whose points are on the ground, and lies inside the hole it asks for.
- The network's surface is closed but for the aprons' rims; no edge has an odd number of triangles; every face is wound toward the air.
- Two cells that share surface share its vertices exactly, and have a portal.
- An edge the plan calls walkable passes the walker's capsule.
- The same entry and ground are the same plan and the same cells' bytes on every toolchain: `k_network_version` is bumped when either changes.
- The passage: its shell two-manifold with the collar its one open loop, wound inward, the collar on the hole's circle within a millimetre; `k_caves_version`.

## Public API

`domain/caves/network.h`: `read_network`, `network_in_range`, `plan_network`, `plan_hash`, `bed_coordinate`, `PlanField`, `plan_svg`, `section_svg`, `holes_json`, `cell_recipe`, `read_cell_recipe`, `cell_recipe_json`, `cell_source_hash`, `solve_cell`, `boundary_vertices`, `build_cell_container`, `cell_origin`; the placement generator `caves`; the mesh generator `cavecell` for `<name>.cavecell.json`. `domain/caves/caves.h`: the passage (`read_passage`, `solve_passage`, `build_passage_container`, ...) and the mesh generator `cave`.

## Depends on

`assets` (the mesh, the generator registry), `geometry` (the cluster build), `materials` and `texture` (a pack's sets), `scene_gen` (the ground, the placement registry), `gfx` for one header, the formation's CPU definition (`domain/gfx/formation.h`), as the rocks do.

## LOD policy and determinism

LOD is each cell's cluster DAG and the far ring's rule (only mouth cells). Determinism stance "derived": content, never read back into a tick. The plan and a cell's mesh are pinned by hash in the tests.

## Testing

`domain/caves/tests/` (seconds in `msvc-debug` on the shared machine):

- `network_plan_tests.cpp`: the network is laid in the host rock at its levels with loops and dead ends, every station inside the limestone by the margin against the formation's own coordinate, trunks along strike and feeders along dip (0.16 s); its openings are found, each outline closed, on the ground and inside its hole, and a network with no way out says so (0.16 s); a hall under a doline falls in and gains a fissure (0.35 s); the plan's clearance (0.14 s); the plan is the same twice, moves nothing with the tile size, is pinned, and through the registry its tiles are its whole read in either order with only mouth cells far (3.7 s: it writes 78 recipes); the refusals (0.23 s).
- `network_cell_tests.cpp`: the whole network's surface is closed, nothing a cell owns is on its bounds' edge, the open edges are the aprons' on the ground, faces are wound toward the air, and every two cells that share vertices have a portal (19 s: every cell of a 110 m network is solved at 0.5 m, and in a debug build a cell is half a second); a walker's capsule passes every walkable edge in the field and by rays in a cell's mesh at 0.25 m (5.2 s); a cell is the same bytes, pinned for a trunk's cell, a mouth's and the hall, and the same from its recipe's text (2.4 s); the dressing (0.35 s); the container through the registry (0.25 s).
- `caves_tests.cpp`: the passage, as before.

`engine_caves_bench`: `caves.plan_network` by reach, `caves.solve_cell` by lattice spacing.

## Not yet

- **The single passage is still here.** A pit entrance is a network's shaft; the passage's recipe, solver and tests should go once a scene that used one is moved, and nothing should be built on them.
- **The terrain's hole** is pasted by hand from `holes.json`; its rim is whole cells; a mouth in a cliff has no hole ([What the terrain's hole must be](#what-the-terrains-hole-must-be)).
- **Every cell is listed and resident at scene read** ([What a host must know about a portal](#what-a-host-must-know-about-a-portal)).
- **UV seams** between the three projection planes: the limestone turns at each. The fix is a triplanar or a stochastic-triplanar material for a mesh (the proof of concept's engine-needs 36, the renderer's), not better UVs.
- **Pinches**: 70 edges in 118,462 at 0.5 m. A mesher that splits a cube's vertex by sheet (dual marching cubes) would end them.
- **One pattern**: a branchwork. No network maze, anastomosis or spongework.
- **A cell's coarsest form** keeps every vertex of its cuts and its floor's edges; a far form that gives them up together on both sides of a cut needs the two cells' DAGs to agree.
- **Draperies and wall flowstone; a stream** on the lowest level's floor (the water's own generator is a neighbour's); the mouths' talus; **plants at a mouth** (the placements' exclusion does not know the apron).
- **The grounding's owed readings**: each is marked on the grounding page.

## Hosts

engine-view, engine-host and engine-editor link the capability when it is built (their generator lists), so a scene's `caves` placement and its `.cavecell.json` meshes load in each; engine-content links it for `cave-plan` and `build`.

## Capability contract (ADR-0027)

- **Switch**: `ENGINE_WITH_CAVES` (ON by default); OFF, or `ENGINE_MINIMAL`, removes the module, its schema types, its tests and its bench; a scene's `caves` placement, `.cave.json` or `.cavecell.json` is then refused with the registries' sentences, and `engine-content cave-plan` says the capability is absent.
- **Registration points**: schema types (`schemas/caves.schema`), a placement generator (`caves`), derived data (the mesh generators `cave` and `cavecell`). No scheduler entry, render pass, protocol method or tunable.
- **Zero cost unused**: a scene that names none of them never calls into it.
