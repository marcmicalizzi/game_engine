# walls (domain)

**Purpose.** Dry-stone walls along a line over the ground (roadmap G145; the solver-needs report's §3.9, the wall half). Two generators in one capability: the mesh generator `wall` solves one **module** — a stretch of wall a few metres long, every face stone, pinning, through-stone, cover stone and cope stone as geometry — from a recipe (`engine.walls.Module`, `<name>.wall.json`), and the placement generator `walls` reads a layer of lines (`engine.walls.Layer`, the placement entry's own parameters), plans every line once, and stands modules along it by **station**. It does not do fences, retaining walls or terraces (they need the ground's edit, W93's seam), field systems (where walls run is a village plan's), the stone's lithology from the formation, moss and lichen (the cover solver's), or a photographed stone kit (an asset set). The sources, every number chosen and where structure was traded for triangles: [dry-stone walls](../experiments/dry-stone-walls.md).

**Why this shape.** The question the brief put first is whether a placement generator can build a mesh when a tile activates. It cannot: `PlacementGeneratorDesc::meshes` is read once, when the scene is, and a `tile` call only places instances of what it named ([scene_gen](scene_gen.md#the-two-kinds)). So a wall reaches the renderer as **a small kit of solved modules placed by station**, never as a mesh a tile: 39 meshes at the default two variants (a stretch and a fallen stretch at five rising grade classes each, a head and a broken stretch at nine signed ones, and the far strip), resident whatever the world's size, each a merged mesh of every stone. A hundred metres of wall is 33 instances (one a module), against the proof of concept's 1,900 stone instances, each of which was a traced structure and a run of pairs. The other way — a merged mesh a tile — would need a tile to add a mesh to the resident scene, which no host does today; it would save the module's repetition (two variants a grade) and the overlap at its joints, and is the request under [Not yet](#not-yet).

**Owned data.** The kit's recipes it writes under the derived-data root (`<ddc>/walls/wall-<hash>.wall.json`, the cliffs' rule for where: the layer's `recipes`, else the host's `--ddc`, else the repository's `ddc/`), and per open entry its plan (`walls::Plan`): every line's vertices and stations, its slots, its far pieces and a tile index — bytes a module, so what scales with the world is the entries alone.

## Stations

A line is integer millimetres in the entry (`WallLine.points_mm`, x, z pairs; `closed` joins the last to the first). **A station is arc length from the line's start in whole centimetres**: each segment's length is one correctly rounded square root of an integer (a segment is at most 10 km, so its squared length is exact in an f64), rounded to the centimetre, and the stations are their running sum. Everything along the line is decided by station and the layer's seed, never by tile (`make_plan`, `src/stations.cpp`):

- **Runs**: the stretches between vertices and gateways. A gateway is a station in `gateways_cm` with `gateway_cm` clear (3 m by default) between two heads; a run shorter than 60 cm is left open.
- **Slots**: each run is cut into `round(length / module_cm)` modules of equal length (each 85 % to 118 % of the 3 m module), their ends the millimetres their stations interpolate to (`point_at`, one rounded division). At a vertex each module runs on past it by its half-width times the tangent of half the turn (to 120°), so the corner's outside is closed; inside, the two overlap.
- **Kinds**: the first module of a run that starts at a gateway or an open line's start is a **head**, the last before one a head turned end for end; the rest are stretches. The variant is a draw of (seed, line, start station).
- **The ruin** (`ruined`, 0 to 0.9): of a line's stretches (heads excluded), `round(ruined × count)` fall, exactly, in stretches of `gap_modules` from starts ranked by a draw of their station; a standing stretch beside a fallen one is **broken** toward it. A fallen stretch keeps its footing and a ragged course and has its stone lying at its foot on both sides.
- **Pieces**: up to four standing modules of a run under one far strip.
- **Tiles**: a slot or a piece is realised by the tile its middle's integer millimetres fall in (`tile_of`); `occupies` is a binary search of that index.

So the wall is continuous across every border and the same whichever tile comes first: two modules either side of a border meet at the same station's millimetres, and each is stood on the ground by itself.

## Standing on the ground

`stand_slot` asks the ground (`Ground::floor_mm`, what a building stands on) at the module's two ends and at both faces' feet (beyond the scarcement) at five places along it, all integer millimetres. The module's grade is the chord's; it takes the **grade class** nearest (0, 0.125 … 0.5 rising for a stretch and a fallen one, turned end for end on a falling grade; −0.5 … 0.5 for a head and a broken one, whose wall end decides which way they face) and is pitched from its class onto the chord by at most half a class (3.6°), its length stretched so its foot spans the chord. It is **sunk** by as much as the chord stands above the ground anywhere sampled beyond its footing's depth (10 cm), so the footing's bed is under the ground at every foot sampled. On a plane up to a grade of 1 in 2 along the wall and 1 in 5 across it, no foot is more than 5 mm above the ground (`stations_tests.cpp`); between samples on curved ground the gap is the ground's sag over 1.5 m, and two neighbours whose sinks differ meet with that difference in their cope's line.

**Courses level, the top following the ground.** Within a module every bed is level (TCV: "lay the stones on the true horizontal, whatever the slope"), the footing steps up the slope, and the cover and cope follow the ground: a module is solved at its class's grade, not pitched onto it. The pitch that remains is at most 3.6°, which the eye does not read as a raked course.

## The module

`solve_module` (`src/module.cpp`) lays a module as a waller does, in the module's frame (x along the wall from −L/2 to L/2, y up, its foot on y = grade × x):

- **The footing** first: the largest stones (15 to 19 cm, 32 to 60 cm long), set 10 cm below the ground and 6 cm proud of each face.
- **Sweeps** along each face: each stone bedded level on the highest point under it, the hollows under it filled by **pinnings** (the small stones knocked into a face), its height graded from 15 cm at the foot to 6 cm under the cover (±25 %), its face length 1.5 to 4 times its height, its tail into the wall at least its face's length; its face split into four planes about a proud point, so the stone is angular and flat-bedded, not a rounded lump. A stone ends short of a rise in what is under it, and a stone that ends against a step is brought level with it (a jumper, to 28 cm).
- **Joints crossed**: an end is kept 4 cm or more off every joint below (moved past it, or short of it, or over the step), and a stone that starts after a gap starts off one too. Measured over 18 modules (three grades, three seeds, a stretch and a head), 1.2 % of joints still run through three courses and 0.6 % through four: where a course ends against a step too tall for one stone, beside a through-stone's packing or a footing stepping up a slope. `module_tests.cpp` holds those as bounds; a running joint is a fault TCV names, so this is listed under [Not yet](#not-yet).
- **Through-stones**: one row, as many as keep them under 0.80 m apart, half a spacing in from each end (0.75 m in a 3 m module, so a run stretched to 118 % keeps TCV's 1 m centres), each laid at its own height in 0.52 to 0.60 m once the faces there are within 14 cm of it, its ends moved off the joints below, proud of both faces by 3 to 10 cm.
- **The cover** across the full width on the line of the body's top (60 to 90 mm, 2 to 4 cm proud), and **the cope** on edge, vertical, 5 to 12 cm thick along the wall, 0.27 m ±8 % high, its top corners jittered so the line is ragged; at a head the first is a large square topstone.
- **A head** is flush and vertical at its end, its courses alternating a **tie** across the whole wall with **runners** along it (TCV, Wall heads). **A broken stretch** stands for its first 30 % and falls in a ragged slope to its footing, its stone at its foot; **a fallen stretch** is its footing and a ragged course with its stone lying within 1.4 m of either face, most of it close.
- **The hearting** is a dark closed core between the faces' tails, so a joint shows shadow and not the field.

**The stone's colour** is a 128 × 128 palette the solver writes into the container (16 × 16 swatches of 8 × 8 mottled texels: dark, mid, blue-, brown-grey, weathered light and iron-stained slabs; a row for the mid form's mosaic and one for the hearting), each stone taking one swatch, so no two neighbours are one grey.

## The generator

`walls` (`src/placement.cpp`) is `PlacementGeneratorDesc` whole: `open` reads `engine.walls.Layer` (an unknown field refused with its path), plans the lines and writes the kit's recipes where they go (a recipe already there is not written again); `meshes` names the kit in `kit_index` order; `tile` stands the slots whose middle falls in the tile, or in a far ring the pieces; `occupies` asks the tile index; `representation` is 1 from `far_ring` out and 0 inside it and outside a world; `expand` is every slot, in plan order, which is every tile's modules. A placement's `id` is its slot's (or piece's) number in the plan, its `tag` 1 for a fallen stretch (the ruins' rubble tag, so a host splits a pop the same way). Walls are structures at sites their entry names, so like the ruins they do not ask the exclusion ([scene_gen](scene_gen.md#exclusion)): a wall a stream crosses is drawn through it, and a crossing (a culvert, a gap) is a later row.

## Forms and the LOD policy

| Form | What | Triangles (3 m stretch) | Reaches the renderer as |
|---|---|---|---|
| near | every stone | about 4,650 (1,550 a metre) | the module's finest clusters |
| mid | the courses as bands (proud at a band's middle, sunk at its joint, the bands' heights the grading's), the cover as a slab, the cope as blocks of three stones at their highest, a fallen stretch's stone as a low heap | about 700 | an authored level of the module's DAG at 2 cm |
| outline | the battered faces and the top as one box | 16 | an authored level at 8 cm |
| strip | the outline, a metre long, stretched over a piece of up to four modules | 16 | its own mesh, in the far rings |

The near form is decimated as every mesh is and cut where its error reaches the mid form's (`build_cluster_lod_levels`; [geometry](geometry.md)), so the cut picks stones near, bands from about 20 m at the default threshold, and the outline beyond. A streamed world's rings from `far_ring` out (the third by default, past 8 tiles at the default rings) place the strips instead of the modules (`representation` 1). The sward's hand-off by distance (`Placement::handoff`) is not used: its band is the sward's own tunable (25 ± 4 m) and its thinning inside the band is the sward's parts', so a wall handed off there would switch from stones to an outline at 25 m; the DAG's levels hand over per cluster by projected error instead, and the ring hands the modules to the strips far out.

## Telling the other placements where it is

A module is a placement like any other, so a later entry reads it as an earlier footprint (`Context::earlier`; [scene_gen](scene_gen.md#earlier-placements)): a circle of the module's bounds' radius in the ground's plane, about 1.6 m round its middle. Grass, litter and scatter after it in the scene's order keep clear of the wall, and the circles overlap at the joints (3 m apart), so nothing grows through it. What a **band-shaped footprint** would do better: the circle clears 1.6 m either side at a module's middle where the wall is 0.35 m half-wide, so the sward stands back from the wall instead of growing to its foot, and a fallen stretch's stone (to 1.4 m) is covered only near the middle. A footprint as a segment and a half-width would clear the wall's own band and grade the cover at its foot. It is the request below.

## Determinism

Every decision is integers (millimetres, centimetres) or draws of `hash_combine`; the solver's arithmetic is f64 sums, products, divisions and correctly rounded square roots, a sine and a cosine from `core/math/deterministic.h` for a fallen stone's turn, and no C library call in a decision. `determinism_tests.cpp` pins four modules' bytes (a stretch, a head, a broken and a fallen one: positions, normals, UVs, the three forms' indices and the palette's PNG) and a layer's placements 419 km out on curved ground. Output does not depend on the order tiles come in or the thread count (`placement_tests.cpp`).

## Budgets

The measured numbers, with the machine's state, are in the table under [the grounding page](../experiments/dry-stone-walls.md#what-the-proof-of-concept-measured-and-what-this-must-beat) and here:

| For 100 m of wall | Value |
|---|---|
| Instances near (modules) | 33 |
| Instances far (strips) | 9 |
| Triangles near, all modules at full detail | 153,606 |
| Triangles at the mid form | 23,220 |
| Triangles at the outline | 528 |
| Traced structures | one an instance: 33 near, 9 far (the proof of concept: about 1,900) |
| Kit resident | 39 meshes at two variants, whatever the world's size |

## A photographed kit

What a kit of real stones would need, when one is made (the solver-needs report's §4): face stones in three course bands (15, 10 and 6 cm high) by lithology, through-stones (30 cm, the wall's width plus 20 cm), cover slabs, cope stones on edge (27 cm), pinnings, and a hearting texture; each with the face it shows and its bed's plane marked, so the solver can place a kit stone where it now lays a split block.

## Not yet

- **Running joints**: 1.2 % of joints run through three courses (see [the module](#the-module)); a solver that takes a step by two stones, or plans a course's joints over a whole run, would close it.
- **A mesh a tile**: the kit repeats two variants a grade class; a tile that builds its own merged wall would not repeat and would continue its courses across module joints (a host would have to take a mesh at activation).
- **Band footprints**: as above.
- **Collision**: walls are drawn, not walked against; the scene collision reads placements' meshes and would take them as any mesh.
- **The ground at the wall**: no wear at a gateway, no worn strip along the foot; the rubble of a fallen stretch lies on the module's foot plane, so on a slope across the wall it floats or sinks by the cross-grade times its distance (up to 0.3 m at 1 in 5).
- **Lithology**: one palette of a slabby metamorphic stone; reading the formation (`rocks.md`, "Geology") would choose the palette and the course heights by rock.
- **Moss and lichen** on the cope and the north face (the cover solver's).

## Public API

`include/domain/walls/walls.h`: the solver (`solve_module`, `ModuleReport`, `Stone`, `StoneRole`, `module_in_range`, `batter`, `body_height`, the through-stone constants, `k_scarcement_m`), the recipe (`read_file`, `canonical_json`, `source_hash`, `build_container`, `k_wall_version`, `k_mid_error_m`, `k_far_error_m`), the plan (`make_plan`, `Plan`, `LinePlan`, `Slot`, `Piece`, `TileRef`, `slot_at`, `point_at`, `tile_of`, `refs_on`), the kit (`kit_size`, `kit_index`, `kit_recipes`, the grade classes) and the standing (`stand_slot`, `Standing`, `stand_piece`). `schemas/walls.schema`: `engine.walls.Module`, `ModuleKind`, `WallLine`, `Layer`. Registrations: `placement:walls`, `mesh:wall`.

## Depends on

`base`, `containers`, `math`, `hash`, `json`, `schema`, `log`, `io`, `image` (the palette's PNG), `assets` (the mesh generator registry), `geometry` (the cluster DAG), `texture` (the container's texture records), `scene_gen`. No other capability. The capability's LOD policy is its forms above; its determinism stance is *derived* (it makes content, read by nothing in the simulation).

## Testing

`engine_walls_tests` (`tools/dev.ps1 test -Filter walls`), about 1.1 s in `msvc-debug`:

- `stations_tests.cpp`: the plan is the same at two tile sizes and every station finds its slot; a gateway has a head either side and the ruin takes exactly the share asked on every line; a module's footing is under the ground on planes to 1 in 2 along and 1 in 5 across, pitched at most half a class; two modules either side of a tile's border meet within a millimetre.
- `module_tests.cpp`: joints crossed (the bounds above); height, batter, through-stones and cope inside the ranges pinned from the sources; courses graded, a head flush and tied, a fallen stretch low with its stone at both sides; the container through the mesh generator registry.
- `placement_tests.cpp`: through the registry, every tile in two orders against the whole read, the far ring's strips, the refusals; a hundred metres at every form.
- `determinism_tests.cpp`: the pins above, reproduced by Clang and GCC in the containers.
- `engine_walls_bench`: `walls.solve_module` (a kit entry's miss) and `walls.stand_tile` (a tile's activation).

**A lesson from building it (2026-11-10, MSVC).** The scaffold's empty sources were linked into the three hosts first; when the real sources replaced them, ninja relinked `engine-view` but the executable still carried none of the capability's code, so `engine_view.generators` failed with `placement:walls mesh:wall` missing and a scene naming `walls` was refused. Deleting the host executables and building again fixed it. A capability scaffolded and built empty before its first real source should do the same before trusting a host's run.
