# city (domain, capability)

**Purpose.** Island City's generator ([13 §13.2](../plan/13-reference-consumer-games.md#132-consumer-b--island-city)): the **plan** of a whole island — coastline, districts, the street graph by class, blocks, lots, parks, plazas and civic reservations — and the **building grammar** that turns one lot of it into a building description, from footprint to furniture zones, with an **occupancy summary** for the NPC work to come. It also holds the **validators** whose pass rate is [E18](../experiments/e18-island-city-yield.md)'s yield, the **per-tile query** a world ring would materialize from, and the **proxy scene fragment** that lets a city be looked at before any kit exists. It builds no meshes, touches no GPU, places no furniture and simulates no one: the kits, the furnishing and the residents arrive later, on the description this module writes. An optional capability ([ADR-0027](../adr/0027-additive-capabilities.md)): `ENGINE_WITH_CITY=OFF`, or a `*-minimal` preset, leaves it out, and `engine-content city` then refuses with a sentence.

**Why this shape.** [ADR-0044](../adr/0044-island-city-plan-and-building-grammar.md) records the decisions and the alternatives; in short:

- **Two levels: a whole-island plan, then buildings per lot.** Island City is finite, and its streets are a property of the island, not of a tile: an arterial crosses dozens of tiles, a district is a region, and "a park within walking distance of every dwelling" is a question about the whole city. A street network assembled tile by tile could be connected, and free of dead-end arterials, only by luck, and every park rule would need a neighbourhood it cannot see. So the plan is made once, from the parameters, and cached as a derived node (`ddc/city/<key>/plan.json`); every building is then a pure function of (plan, lot), which a tile materializes on demand: a tile's content is the lots that touch it (`lots_in_tile`), and materializing a tile twice yields the same buildings. The plan is small (the default 4 km island is 9,165 lots in a 3.2 MB file) and cheap (21 ms in a debug build), so paying for it once is no burden; the buildings are the volume, and they are the part that is per tile.
- **Integers decide, floats only describe.** Every seeded choice is integer arithmetic on centimetres and Q16 fractions, every draw the engine's `hash_combine` of (seed, purpose, index), the coastline's and the mountain axis's angles a 64-step Q14 table rather than `cos`, and the authored metres are converted once, when read, with `llround` of the f32 widened to f64. Floats appear only in the proxy fragment's metres. So a plan at a seed and a building at a lot are the same bytes from MSVC, GCC and Clang, at x86-64-v2 and v3 — the content build's rule ([ADR-0035](../adr/0035-no-floating-point-contraction.md)) and the ruins' ([ruins](ruins.md)) applied to a city — pinned by golden hashes, and the output never depends on the thread count (merged by block and lot, never by completion order).
- **A description, not meshes.** A building is floors, cores, spaces, units, walls as centre lines with a thickness and openings, doors, windows and furniture zones. Meshes are the kits' business ([07 §7.6](../plan/07-content-pipeline.md#76-procedural-generation-volume-content)), the kits do not exist yet, and a description is what every later consumer reads — the kit assembler, the furnisher, the navigation mesh, the NPC routines, the interior-cell streaming of [13 §13.4](../plan/13-reference-consumer-games.md#134-requirements-the-consumers-add-or-sharpen) item 10. The walls sit on a 2 m module (a parameter), the ruins' default, so a kit in the ruins' socket convention can attach to them later; nothing here designs that attachment.
- **Proxies first.** Until a kit exists the city is drawn as a dozen unit boxes instanced per element and scaled to it — a district is tens of thousands of instances of twelve meshes, which is what the renderer's instance-of-mesh model is for — written as a scene fragment by `engine-content city fragment`. The scene reader does **not** link this generator: its registration point for generators is being designed on the owner's side ([ADR-0037](../adr/0037-scene-reader-links-ruins-where-configured.md)'s revisit), and until it exists the fragment is how a city is viewed.
- **The validators are the review loop.** Every building runs through the validators, which read the description and nothing of how it was made; a failure is categorized by the rule it broke and never repaired, so a report says which rule of the grammar to fix (13 §13.2, "Agent role").

**Owned data.** `Params` (an island's parameters, converted), a `Plan` (its districts, streets, graph, blocks, lots, parks and per-tile index), and the `Building`s the grammar fills — each a value the caller holds; nothing is global. The authored types, the plan file and the building file are the capability's own schema, `domain/city/schemas/city.schema` (`engine.city`).

## Parameters and style tables

`engine.city.IslandParams`, JSON; `engine-content city params <file>` writes the defaults with every table spelled out. A file may leave any table empty (and most fields out): what it omits is the default, and the plan's key is taken over the filled-in form, so two files that differ only in spelling defaults out are one island. Everything is metres, degrees and fractions as authored, converted once to centimetres, 64ths of a turn and Q16.

| field | default | what |
|---|---|---|
| `seed` | 1 | the island's seed: with the parameters, the only input the plan is drawn from |
| `radius`, `coast_roughness` | 2,000 m, 0.1 | the seeded outline: 64 vertices whose radii are the mean plus five seeded harmonics |
| `coastline` | empty | an authored polygon (metres, counter-clockwise from above) that replaces the outline |
| `mountain_deg`, `mountain_share` | 90°, 0.3 | the mountain's direction and the share of the island's extent along it the mountain and its wilderness take |
| `greenbelt` | 150 m | the band between the city and the mountain |
| `harbour_deg`, `harbour_width`, `harbour_depth` | 270°, 800 m, 350 m | a cosine bay carved into the coast |
| `tile_size` | 64 m | the world's tile ([03 §3.7](../plan/03-data-model.md#37-spatial-partition)): the per-tile query's unit |
| `arterial_spacing`, `arterial_jitter` | 480 m, 0.12 | the arterial grid, anchored at the island's centre, each line jittered by a draw |
| `district_size` | 800 m | a district's typical extent: how many district seeds the city gets |
| `district_mix` | below | each kind's share of the city |
| `styles` | below | one per district kind |
| `street_widths` | arterial 32 m, collector 22, local 14, alley 6, pedestrian 8 | a class's width, centre line to centre line of its kerbs' setback |
| `parks` | walking distance 400 m, central share 2.5% | the park rules |
| `civic` | 1 hospital, 1 university, 2 stations, 1 government | the civic reservations |
| `building` | below | the grammar's building-wide rules and the validators' limits |
| `hours` | retail 9–19 Mon–Sat, office 8–18 Mon–Fri, industrial 6–22 Mon–Fri, medical all day, education 8–17 Mon–Fri, civic 8:30–16:30 Mon–Fri, transport 5–1 every day | opening hours by workplace kind |
| `overrides` | empty | authored pins ([below](#authored-overrides)) |

**The district styles** (`engine.city.DistrictStyle`, the defaults `default_style(kind)` gives; the mix is downtown 8%, industrial 12%, old town 7%, waterfront 9%, campus 6%, mixed use 24%, residential 34%):

| kind | street pattern | block side | lot layout | lot width × depth | setbacks front/side/rear | coverage | floors | height | park share | archetypes |
|---|---|---|---|---|---|---|---|---|---|---|
| downtown | grid, midblock alleys | 80–140 m | rows backing on an alley | 26–44 × 24–64 m | 0 / 0 / 3 m | 0.7 | 10–40 | 140 m | 4% | towers 5, offices 4, mid-rise 1 |
| mixed use | coarse grid | 90–170 m | perimeter with a courtyard | 14–26 × 16–22 m | 0 / 0 / 2 m | 0.8 | 4–8 | 30 m | 6% | mid-rise 6, offices 1 |
| residential | closes (cul-de-sacs) | 60–240 m | rows round a close | 7–18 × 26–32 m | 4 / 1.5 / 6 m | 0.5 | 2–3 | 12 m | 8% | terraced 5, detached 4 |
| old town | organic: jittered lines, merges, lanes, squares | 36–130 m | perimeter | 9–16 × 13–18 m | 0 / 0 / 2 m | 0.85 | 3–5 | 20 m | 5% | mid-rise 6, terraced 2 |
| industrial and port | wide | 120–260 m | across the whole block | 40–80 m wide | 6 / 4 / 6 m | 0.6 | 1–3 | 16 m | 3% | warehouses 8, offices 1 |
| waterfront | coarse grid, a park strip on the coast | 70–160 m | perimeter | 14–26 × 16–22 m | 2 / 0 / 2 m | 0.75 | 4–10 | 40 m | 12% | mid-rise 6, offices 1 |
| campus | wide | 120–260 m | across the whole block, every lot civic | 50–120 m wide | 10 / 8 / 10 m | 0.45 | 3–6 | 30 m | 12% | civic shell |

The old town's `square_chance`, `lane_chance` and `merge_chance` are 0.12, 0.3 and 0.15; the dwelling mix (`unit_mix`, studio to three bedrooms) is 25/40/25/10% downtown, 20/35/30/15% in mixed use, 30/40/20/10% in the old town and 15/35/35/15% on the waterfront.

**The building rules** (`engine.city.BuildingRules`): a 2 m module; floors 3.2 m and a 4.2 m ground floor; walls 0.30 m exterior, 0.25 m core, 0.20 m party, 0.10 m partition; corridors 1.8 m (2.8 m in a civic shell); a 2.4 m service band; dwelling depths 6.4–8.8 m; two stairs from 4 floors, at least a third of the footprint's diagonal apart; 30 m from a unit's entry to a stair; stairs 2.8 m, elevator cars 2.4 m, shafts 1.4 m; an elevator from 5 floors and one per 60 units or 250 workplaces above the ground; spans of 9 m (frames), 7 m (houses) and 30 m (a hall's portal frames); doors 0.9 m, entrances 1.4 m, loading doors 4 m; windows 1.4 m from a 0.9 m sill to a 2.3 m head; a 1.6 × 2.0 m bed with 0.6 m of clearance; 10, 25, 80 and 20 m² a workplace in offices, shops, industry and civic buildings; and each room semantic's minimum (a living room 3.2 × 4.0 m, a bedroom 2.6 × 2.8, a bathroom 1.6 × 2.0, a WC 0.9 × 1.4, a shop floor 3.0 × 4.0, an open office 3.6 × 4.0, …).

**Which numbers are choices, not findings.** The plan (13 §13.8) leaves the island's size, the first archetypes and their proportions open. Every number above is a default chosen to be defensible — typical European block and lot dimensions, common building-code rules of thumb for egress, daylight and bedroom size — and a parameter; ADR-0044's open questions list them. None has been checked against a picture: the owner's fly-through over the proxy fragment is the first look.

## The plan

`generate_plan(params, plan, error, jobs)`, in order, each stage from its own draws and reading only what the stages before it fixed:

1. **The coastline and the mountain.** The seeded outline (or the authored polygon), the harbour carved into it; the mountain axis a Q14 direction, the city ending `greenbelt` short of where the mountain begins.
2. **The arterial grid** over the island's bounds, anchored at its centre so the same parameters give the same lines whatever the coast does, each jittered by its own draw and snapped to the module. The cells between arterials are **superblocks**, each classed city (any of nine sample points on land and short of the city limit), greenbelt, or nothing.
3. **Districts**: seed superblocks taken in the order the seed ranks them, each at least 0.6 of a district's extent from those taken, as many as the city's area over `district_size²`; every superblock goes to the nearest seed. Kinds follow the island's shape and the mix: in the order downtown, industrial, old town, waterfront, campus, mixed use, each kind takes the unassigned districts it scores best on while that brings its area nearer its share — downtown nearest the **anchor** (the city's centroid pulled 30% toward the harbour), industry and port nearest the harbour and only on the coast, the old town between the harbour and the anchor, the waterfront the most coastal, campus and mixed use nearest the anchor — and whatever is left is residential. Downtown takes at least one. Then the district pins.
4. **The central park**: the superblocks nearest the anchor that are not downtown, industrial or coastal, as many as its share asks for, each one park block with no internal streets.
5. **Streets and blocks, superblock by superblock**, by the district's pattern: a number of cells per axis that keeps each block inside the style's range (drawn per superblock), the middle line of three or more a **collector**, the rest **locals**; `Wide` patterns are all collectors; `Organic` jitters its lines by up to a fifth of a cell (never past the style's range), merges a cell with its neighbour by `merge_chance` where the merged block stays in range, makes a lane of a line **pedestrian** by `lane_chance`, and turns the block at a junction into a **square** by `square_chance`. A block is kept when its corners and centre are on land: in the city when every corner is short of the city limit, as **greenbelt** when one is past it and one short of the mountain.
6. **The graph**: nodes at every block corner and interior-street end, a segment for each stretch of a street between consecutive nodes on its line. Blocks off the graph's main component (cut off by the coast or the mountain) are dropped rather than left unreachable. Then the street pins, and **the lane rule**: a pedestrian lane that would end an arterial is a local street. Each block's buildable rectangle is its centre-line rectangle less half of each side's street.
7. **Parks by block** (after the park pins): the waterfront's blocks with water a block's extent beyond a side; then each district's **district parks**, the blocks nearest its centre, until its park share is met. **Civic blocks**: hospitals in mixed use (residential failing that), stations downtown (mixed use), government in the old town (downtown), each the block nearest the anchor and the next at least a kilometre from those taken; a campus district's lots are all university, and a city with no campus reserves universities in mixed use. A civic block outside a campus is one lot, never subdivided.
8. **Lots, block by block**, on the pool when there is one and joined in block order: by the style's layout — **perimeter** (a strip of lots along each side at a drawn depth, a courtyard left in the middle), **rows** (two rows back to back), **whole** (lots across the block fronting its wider long side), **alley rows** (two rows backing onto a service alley down the long axis, the alley an interior street joined to the two short sides), **close rows** (a close down the long axis from one short side, lots either side of it fronting it and a row across its head) — each run cut at a width drawn in the style's range, on a 10 cm grid. A lot's **archetype** is drawn by the style's weights among those that fit its width along the street and its depth; a lot nothing fits takes the first that does of mid-rise, detached, terraced and warehouse, and one too small for any is a pocket park.
9. **The walking distance**: every dwelling lot, in lot order, within `walk_distance` of a park (to the park's edge); where one is not, the lot of its block nearest the block's centre becomes a **pocket park**. Then the civic pins on lots, the final graph with the closes and alleys, and the per-tile index.

**Ids are stable, and composed from the arterial grid**, which no pin changes: a superblock is its cell's index; a block `superblock × 256 + its cell`; a lot `block × 1024 + its place in the block`; a street an arterial line (`0..`) or `2^20 + superblock × 1024 +` a line (`m`, or `256 + k`) or an interior street (`512 + the block's cell`). So a pin names the same thing whatever else is pinned; a district pin changes the pattern of the superblocks the district covers and renumbers what lies inside them, and nothing else.

**What the default island is** (`engine-content city plan` of the defaults, seed 1): 19 districts over 9.0 km² — 6 residential (3.45 km²), 4 mixed use (2.15), 2 old town (0.93), 2 downtown (0.80), 1 campus (0.73), 3 industrial (0.68), 1 waterfront (0.30); 434 streets (41 km of arterials, 38 of collectors, 92 of locals, 7.6 of alleys, 2.6 of pedestrian lanes) as 1,786 segments between 1,096 nodes; 631 blocks and 9,165 lots (3,541 mid-rise over shops, 2,595 detached houses, 2,457 terraced, 353 offices, 118 towers, 53 civic shells, 48 warehouses); 178 parks over 1.33 km² (a central park, 26 district parks, 67 pocket parks, 15 squares, 3 waterfront blocks, 66 greenbelt blocks); a hospital, two stations, a government building and 49 university lots.

## The plan as a derived node

The plan's **key** is `plan_key(params)`: the engine's hash of the parameters' canonical JSON (defaults filled in) combined with `k_generator_version`, which is bumped whenever a change makes the same parameters give a different plan or building. `engine-content city plan <params> --cache` writes `<ddc>/city/<key>/plan.json` (the root `<repo>/ddc` or `--ddc`) and, the second time, finds it there and makes nothing. The file (`engine.city.PlanFile`) carries the parameters, the coastline, the arterial lines, the mountain axis and every district, street, block, lot and park in integer centimetres, with the key and `hash_plan`; reading it back rebuilds the graph and the tile index with the functions that built them and refuses a file whose records do not hash to what it carries or whose generator version is not this build's. Derived data is never committed.

## Authored overrides

An authored layer is sparse: `overrides` pins a few ids and everything else is generated around them.

| pin | what it does |
|---|---|
| `District`, `district_kind` | the district's kind, after the rules gave it one; its superblocks take the new kind's pattern and lots |
| `Street`, `street_class` | the street's class (and width) along its whole length, after the graph is built; the lane rule never touches a pinned street |
| `Park`, `park` | the block becomes a park of that kind; `None` keeps the waterfront and district-park rules off it (a central or greenbelt superblock is not affected) |
| `Civic`, `civic` | the lot becomes a civic shell of that kind; `None` keeps the civic rules off its block |

A pin that names an id the plan does not have refuses the plan with a sentence ("no street 12345"), because an authored layer that silently stopped applying would be worse than one that fails.

## The per-tile query

`lots_in_tile(plan, tile)` is the lots whose **closed** rectangle touches the tile — closed, because a building's facade stands on its lot's line, and a proxy on a tile's edge belongs to the tile beyond it — from a sorted index of (tile, lot) built with the plan. `owner_tile(lot)` is the one tile holding the lot's centre, for a consumer that must see each building exactly once (a census of residents, a save). `tile_proxies(plan, tile, detail)` is everything a tile holds: the buildings of its lots generated at `detail`, their proxies and the ground's, keeping those anchored in the tile. Every proxy has one anchor (the centre of its bottom face) and the ground is cut along the tile grid first, so **a tile's content is the whole plan's content restricted to the tile**, piece for piece, and the tiles together are the whole; `plan_tests.cpp` asserts both over every tile of the small island.

**The world ring streams it** ([Where it attaches](#where-it-attaches)): the per-tile query is the placement generator `city`'s `tile`, which the world's generic placements consumer calls one tile at a time with the detail the LOD policy below gives the tile's ring — the consumer lives in `systems/world` and names no generator, so the city needed no edit to another capability to be streamed.

## Where it attaches

**The placement generator `city`** ([scene_gen](scene_gen.md), [ADR-0046](../adr/0046-scene-generators-register-themselves.md); `scene_generator.h`, `src/scene_generator.cpp`), registered from this module's own source by a `scene_gen::Registrar` — the module is `WHOLE_ARCHIVE` for it — so a scene names the city and neither the renderer nor the world links this capability:

```json
{"placements": [{"generator": "city", "params": {"plan": "ddc/city/<key>", "district": 1}}]}
```

The parameters are an `engine.city.CityEntry` (`city.schema`, read with the schema's reader, so an unknown field is refused with its path): `plan` (a directory holding `plan.json` — `engine-content city plan`'s output, the cache's `ddc/city/<key>/` — or the file, relative to the scene), one `tile` (`[x, z]`) or one `district` or neither, a `detail` (`Massing`, `Floors`, `Rooms`), and `meshes` (where the twelve proxy boxes are; empty is `proxy/` beside the plan file, and `open` writes them there when one is missing — they are a function of nothing, so a file already there is the right one). **`expand` is the fragment writer's content in memory**: a tile's proxies (`tile_proxies`, `rooms` by default), a district's buildings and ground (`floors`), or with neither every building lot and every street and park (`massing`, since a whole island in rooms is hundreds of thousands of instances) — one placement per proxy at the place its integer centimetres name, in f64 (`proxy_position`: one correctly rounded division, so a proxy 10,000 km out stands where the plan put it to a nanometre, [ADR-0053](../adr/0053-world-positions-are-f64-and-the-gpu-sees-none.md); the plan is world-anchored centimetres, so the city places in the world's own frame and has no site origin to place from), scaled as `make_fragment` scales its instance (`proxy_scale`, shared by both), each an instance of one of the twelve meshes (`city/massing`, …); the pool the reader offers generates the buildings past 32 lots, in runs of 256, appended in lot order. **`tile` is the per-tile query**: `tile_proxies` for the tile at `stage_for_distance` of the tile's ring's inner radius (rooms in the innermost ring, floors within 6 tiles, massing beyond), each building counted by the one tile that owns its lot (`owner_tile`); `occupies` is the plan's bounds and `representation` the ring's stage, so a tile moving between two rings of one stage is not built again. A streamed city's plan must be on the world's tile grid (`params.tile_cm`), or the entry is refused ("must be one grid"). Every placement's tag is 0 and every tile's kind 1: the city draws one kind of thing. `tests/scene_generator_tests.cpp` holds a district's expansion to the fragment `city fragment --district` writes, instance for instance, every streamed tile to `tile_proxies` at the ring's detail with each building counted once, and the refusals. A host links the capability for its registrar (engine-view and engine-host, where the configuration has it).

## The building grammar

`Grammar::generate(lot, stage, building)`, in the order 13 §13.2 writes it: footprint → structural grid → building type → floor count → vertical cores → entrances → service shafts → floor plans → units → room graphs → doors and windows → furniture zones. The building's frame puts its origin on the lot corner at the street's left, x along the front, z away from the street (`to_plan` maps a point to the plan, a rotation and never a reflection). Two layouts cover the seven archetypes.

**Bars** — the apartment tower, mid-rise apartments over shops, the office, the civic shell:

1. **Footprint and grid.** The lot less its setbacks (a tower and a civic shell stand at least 6 m from their side lines, so they are free-standing); a structural bay of three modules (6 m), or half the width where that is narrower, never below two modules. The depth is the deepest bar that still leaves two bays under the district's coverage: **double-loaded** (a unit on each side of a corridor) where two dwelling depths and a corridor fit, else **single-loaded**; each side as deep as the rules allow up to `unit_depth_max`. The width is whole bays under the coverage, a tower no wider than twice its depth. A facade on a side line with no setback has no daylight (it abuts the neighbour).
2. **Floors**: the district style's range clamped to the archetype's (a tower 8–60, a mid-rise 3–8, an office 2–40, a civic shell 2–8 and a station 2), drawn, held under the district's height.
3. **Cores**: one stair, or two from `two_stair_floors`, and more wherever the travel distance asks for them, spread evenly along the bar and **alternately on the street side and the back side** when there is one; the street-side core nearest the middle is the main one, holding the elevators (their count from the smallest unit, so the estimate never falls short of what is built) and the **service shaft** (beside the cars where the bays leave room, else a strip along the core's facade side, so the stair still opens onto the corridor). **A bar too narrow for the cores its height asks for keeps the tallest floor count whose cores still fit**, since fewer floors need fewer cars and, under the thresholds, one stair.
4. **Entrances**: the ground floor's lobby, the bay beside the main core, opens onto the street and the corridor; a mid-rise's shops open onto the street.
5. **Units**, floor by floor, each side's runs between its cores (and the lobby) cut left to right: dwellings by the district's mix — a studio, one, two or three bedrooms, each program's width the module multiple its rooms' minimums need (4, 6, 10 and 12 m by default), a run's remainder added to its last unit, a run too short for a studio a storage room; shops, office suites and civic departments at drawn widths of 3–5 or 4–8 modules. The ground floor's street side of a mid-rise is shops.
6. **Room graphs**, unit by unit: a dwelling is a service band against the corridor — a bathroom, the **entry**, and a WC from two bedrooms — and against the facade the **living room and kitchen** and the bedrooms side by side; an office suite a reception and a WC, an open office and a meeting room; a shop its shop floor on the street, a back room and a WC; a civic department one zone with no room graph (the civic shell is massing, cores and corridors).

**Houses** — terraced (built to both side lines, whose side walls are party walls) and detached (inside its side setbacks, exposed on four sides): a strip along one party wall holds the hall, the stair and the landings; the rest of the width is columns of rooms no wider than a house's span, front and back. The ground floor's front column is the kitchen and the rest living rooms, a WC behind the stair; an upper floor is bedrooms front and back, the first back one behind a bathroom. Two or three floors; a house is one unit spanning them.

**The warehouse**: a hall of 12 m bays behind a 6 m office strip (reception, WC, office), one floor twice the ground floor's height, loading doors on the back facade.

**Walls from the tiling.** A floor's spaces tile its footprint exactly (a test asserts it for every floor of 40 buildings), so every edge of every space is shared with another or on the facade. Each line is swept once; its stretches are labelled with the space on either side, and consecutive stretches with the same two spaces are one wall — a centre line between exactly two spaces (or one and the outside), which is what an opening needs to say what it connects. Its thickness follows from those two: exterior, core, party (between units, or a unit and circulation) or partition.

**Doors by the room-graph rule.** The entrances are the archetype's; circulation (stairs, elevators, the lobby, storage, every unit's entry but a shop's) opens onto the corridor; and inside a unit every room opens onto the most public room already reached that it shares a long enough wall with — an entry, a landing or a stair before a living room, a living room before a bedroom — so the doors are a tree rooted at the unit's entry. A room that shares no wall of a door's width with a reached one gets no door, and the reachability validator says so: the rule is the grammar's, the check is not. A shop's entrance goes at one end of its front, so a narrow shop keeps a window beside it (E18's first run found fourteen 6 m shops whose centred door left no pier for one). **Windows**: every habitable room, on each of its exterior walls on a facade with daylight, as many as the wall takes at a window's width with a pier either side, clear of the doors. **Furniture zones**, by semantic: a bed with its clearance, a kitchen run, a sofa and a table, a bath and a toilet, desks in rows, a counter and shelving, racking in a hall — zones, never furniture.

**Identity.** A building's seed is the island's seed and its lot's id through the engine's hash; its units and spaces are numbered in the order the grammar makes them. So (generator version, plan key, lot, unit) names the same apartment whenever it is generated again — the persistent identity 13 §13.2 asks for — and a persistent modification is keyed by it.

## The occupancy summary

What the NPC routine work will read to give residents homes and jobs (13 §13.2, "Dense NPC simulation"); it is filled at the `Floors` stage and nothing is built on it yet.

| field | what |
|---|---|
| `dwellings`, `dwellings_by_kind[5]` | homes: studios, one-, two- and three-bedroom flats, houses |
| `bedrooms`, `residents` | a studio houses one, a one-bedroom two, a two-bedroom four, a three-bedroom five, a house its bedrooms plus one |
| `workplaces[7]` | jobs by kind — retail, office, industrial, medical, education, civic, transport — a shop's floor area over 25 m², an office's over 10, a hall's over 80 plus its office strip's over 10, a civic department's over 20 |
| `hours` (the file) | the opening hours of each kind of workplace the building has, from the parameters |

Per unit (`BuildingUnit` in the file): its id, floor, kind, rectangle, entry, bedrooms, residents or workplaces and work kind — the record an NPC's home or job points at: *home: lot 8652811, unit 7 (floor 3)*.

## Validators

`validate_plan` and `validate_building`, each appending `Finding`s — the rule, the floor, the subject (a space, unit, lot, block, street or district), what was measured and what it had to be — to a `Report`. They read the description and nothing of how it was made. A building is validated on what its stage has: site and structure at `Massing`, egress and services too at `Floors`, everything at `Rooms`.

| validator | rule | what it guards |
|---|---|---|
| connectivity | `plan.connectivity` | the street graph is one component, and so are its vehicle streets |
| | `plan.dead_end_arterial` | no arterial ends where no other vehicle street continues it |
| blocks | `plan.block_size` | every block's sides are inside its district style's range (whole-superblock parks excepted) |
| parks | `plan.park_share` | each district's parks are at least its style's share of its area |
| | `plan.park_distance` | every dwelling lot is within the walking distance of a park |
| frontage | `plan.frontage` | every lot's front lies on its street's kerb, within the street's kept extent, and the street is not an alley |
| reachability | `reach.room` | every room from its unit's entry, through the unit's doors (and a house's stairs) |
| | `reach.unit` | every unit's entry from the street |
| | `reach.core` | every stair, elevator, corridor and lobby from an entrance |
| egress | `egress.stairs` | two stairs from `two_stair_floors` |
| | `egress.separation` | the farthest two stairs at least `stair_separation` of the footprint's diagonal apart |
| | `egress.travel` | every unit above the ground within `max_travel` of a stair along the corridor |
| daylight | `daylight.window` | every habitable room (living, bedroom, kitchen, shop floor, office, meeting room, department) has a window in an exterior wall |
| fit | `fit.room` | every room meets its semantic's minimum dimensions |
| | `fit.bed` | a double bed and its clearance fit every bedroom |
| | `fit.corridor` | every corridor is as wide as its building's corridor rule |
| services | `services.elevators` | enough elevators for the floors and the units or workplaces above the ground |
| | `services.shaft` | a service shaft reaching every floor of every bar |
| structure | `structure.span` | the structural bays within the structure's span (a frame, a house, a hall) |
| site | `site.setback`, `site.coverage`, `site.height` | the district's setbacks, coverage and height |
| | `site.frontage` | an entrance on the street's facade |

**E18's yield** (`choose_yield_lots`, `measure_yield`, `engine-content city yield`): `count` building lots taken from each archetype in turn in the order the seed ranks them, each generated at `Rooms` and validated; the table counts buildings passing without any finding, units passing (a unit fails with a finding on it or its rooms, or on its building), buildings passing each validator, and failures by rule — by archetype and in all. [E18](../experiments/e18-island-city-yield.md) has the numbers.

## The proxy fragment

`engine-content city fragment --plan <dir> --tile x,z | --district <id> --out <scene.json>` writes an `engine.scene.Scene` of proxy instances and, beside it (`proxy/`, or `--meshes`), the twelve proxy meshes: GLBs of one unit box each (a metre on a side, its bottom face centred on the origin), each its own colour — `massing`, `civic`, `slab`, `exterior_wall`, `interior_wall`, `core`, `window`, `road`, `lane`, `park`, `plaza`, `tree`. Every instance is axis-aligned, a translation and a scale with no rotation, named by its lot and floor. What a building contributes depends on the detail: `massing`, one box; `floors`, a slab a floor and a roof, its cores full height, four facade walls a floor and a glass band along each facade with daylight; `rooms`, every wall of every floor at its thickness and every window at its sill and head instead of the bands. The ground is each street segment and park cut along the tile grid, and trees on a jittered 14 m grid in every park that is not a square, drawn from the park's own seed. A tile defaults to `rooms`, a district to `floors`. The fragment is compact JSON: a district is tens of thousands of instances. `engine-view --scene <fragment>` draws it like any scene (on the owner's machine; nothing here has a GPU).

## LOD policy

`stage_for_distance(distance in tiles)`: **rooms** within 1.5 tiles, **floors** within 6, **massing** beyond. The stages are prefixes of one another — the massing is the same at every stage, and the cores, units and occupancy fixed at `Floors` are the ones `Rooms` fills (a test asserts it per archetype) — so a tile moving between rings refines or coarsens its buildings without changing them. A far tile pays for a footprint and a height; the rooms, walls and openings, which are nearly all of a building's cost ([Performance notes](#performance-notes)), only near.

## Determinism

`city::k_determinism` = `derived`: the plan and every building are content made from the parameters, never state the simulation ticks; what happens to a building afterwards is the persistent store's. And bit for bit on every toolchain: `plan_tests.cpp` pins the default island at seed 2026 (`0x0a734487921d1072`), `building_tests.cpp` one building per archetype on it, and `apps/engine_content/tests/determinism_tests.cpp` a 1.2 km island's plan on one thread and three, a district's proxy fragment and E18's yield over 70 of its buildings on one thread and three. Taken on Clang 18 (`linux-clang-debug`) and reproduced by GCC 13 (`linux-gcc-release`) on 2026-09-25; the owner's MSVC run is the third toolchain, and until it has passed the numbers are two compilers' agreement, not three.

## Invariants

Each is a test in `tests/` unless it says otherwise.

- One set of parameters is one plan, bit for bit, on one thread or on a pool of one worker or five beside the caller (`plan_tests.cpp`); one lot is one building, alone or among sixty, on any pool (`building_tests.cpp`).
- Blocks are on land, short of the city limit (greenbelt blocks, short of the mountain); their lots lie inside their buildable rectangles and never overlap; every segment lies on its street's line between two distinct nodes.
- The default island has every district kind, park kind, civic kind, archetype and street class; a civic reservation outside a campus is its block's only lot.
- The plan's own validators pass on islands of 900 m, 2 km and 3 km at five seeds.
- A tile's lots are exactly the lots whose closed rectangle touches it; a tile's proxies are the whole plan's restricted to it, materializing a tile twice gives the same proxies, and the tiles together are the whole (every tile of the small island, at massing and at floors).
- Each pin does what the table says, the rest of the plan is what the rules made, and a pin naming nothing refuses the plan.
- The plan file reads back to the same plan; a record changed by a centimetre, or another generator's file, is refused.
- The key is the parameters' and not their spelling.
- Each LOD stage is a prefix of the next; a floor's spaces tile its footprint; every wall parts two different spaces and an exterior wall lies on a facade; openings lie in their walls and zones in their rooms; the footprint lies in its lot.
- The occupancy summary counts the units' homes and jobs, and only dwellings have residents.
- Every validator passes a building as generated and fails the same building with what it guards taken away (`validate_tests.cpp`); every plan rule fails on a fixture that breaks it.
- The default island's 200 E18 buildings all pass, and the table is the same on any thread count; a limit the grammar does not read when it decides (a 7 m span, a 1.5 m bed clearance) is caught by the validator that guards it.

## Public API

- `domain/city/params.h` — `Params`, `Style`, `Rules`, `Hours`, `Pin`, `default_island_params`, `default_style`, `default_building_rules`, `params_from_schema`, `read_params_file`, `write_params_file`, `to_cm`, `to_step64`, `to_q16`, and the names and parsers of the enumerations.
- `domain/city/plan.h` — `Plan` and its records (`District`, `Street`, `Node`, `Segment`, `Block`, `Lot`, `Park`, `TileEntry`, `Rect`, `TileCoord`), `generate_plan`, `on_land`, `along_mountain`, `tile_key`, `tile_of`, `tile_rect`, `lots_in_tile`, `owner_tile`, `build_tile_index`, `plan_key`, `plan_cache_dir`, `hash_plan`, `lot_seed`, `plan_to_schema`, `plan_from_schema`, `write_plan_file`, `read_plan_file`, `PlanStats`, `plan_stats`, `k_generator_version`.
- `domain/city/building.h` — `Building` and its records (`Floor`, `Core`, `Space`, `Unit`, `Wall`, `Opening`, `Zone`, `Link`, `OccupancySummary`), `Grammar`, `generate_buildings`, `stage_for_distance`, `to_plan`, `footprint_on_plan`, `hash_building`, `building_to_schema`.
- `domain/city/validate.h` — `Finding`, `Report`, `Validator`, `validator_of`, `validator_name`, `validate_plan`, `validate_building`, `choose_yield_lots`, `YieldRow`, `YieldTable`, `measure_yield`.
- `domain/city/fragment.h` — `ProxyMesh`, `Proxy`, `append_building_proxies`, `append_ground_proxies`, `append_district_ground_proxies`, `tile_proxies`, `proxy_position` (the place in f64, what the generator places), `proxy_translation` (the float32 the fragment writes into a scene file's `translation`, an absolute float32 and marked as an ADR-0053 seam until that field is a `worldpos`), `proxy_scale`, `proxy_mesh_path`, `sort_proxies`, `hash_proxies`, `write_proxy_meshes`, `make_fragment`, `write_fragment`.
- `domain/city/scene_generator.h` — the placement generator `city` ([Where it attaches](#where-it-attaches)): `k_placement_generator`; its parameters are `engine.city.CityEntry` in `city.schema`.
- `domain/city/city.h` — all of the above, the capability checklist, and `k_determinism`.

**Depends on.** `base`, `containers`, `math`, `hash`, `json`, `schema`, `schemas` (the fragment is an `engine.scene.Scene`), `io`, `log`, `jobs`, its own `city_schemas`, and `scene_gen` (the registry the generator registers into). **Depended on by** no module: `engine-content` links it where this capability is configured, and engine-view and engine-host for the generator's registrar; the renderer's scene reader and the world's placements consumer find it by name.

## Testing

`tools/dev.ps1 test -Filter city` runs `tests/plan_tests.cpp`, `tests/building_tests.cpp`, `tests/validate_tests.cpp` and `tests/scene_generator_tests.cpp` (the generator through the registry, the plan written into a `TempDir`; no device, no files but the plan file's round trip in a `TempDir`; the islands in `tests/city_fixtures.h`, the default one at seed 2026 and a 900 m one), `engine-content`'s `city_tests.cpp` (the commands end to end into the test's scratch directory, the cache hit, the refusals) and the city row of its `determinism_tests.cpp`. Benchmarks: `tools/dev.ps1 bench -Filter 'city.*'`.

## Performance notes

Measured 2026-09-25 in this change's cloud session — **not** the development machine: a 4-core Intel Xeon cloud container ("@ 2.10GHz"), `linux-gcc-release`, `--require-quiet`, the header's `machine_state` 0% of the CPU in other processes at both ends, no GPU; two runs, which agreed within 7%. The default island at seed 2026 (7,990 building lots), E18's 200 lots:

| what | median (the second run) |
|---|---|
| the whole plan, one thread (`city.plan/0`) | 7.46 ms (7.97) |
| the same with the lots cut on 4 workers and the caller (`city.plan/4`) | 7.90 ms (8.06) |
| one building at `Massing` (`city.building/0`) | 0.105 µs |
| at `Floors` (`city.building/1`) | 2.8 µs |
| at `Rooms` (`city.building/2`) | 127 µs |
| the validators over one building at `Rooms` (`city.validate`) | 9.2 µs (9.6) |
| a 46-building district's proxies at `Floors` (`city.fragment.district`) | 0.86 ms |

**What it says.** A far tile costs nothing: massing is a tenth of a microsecond a building, floors three microseconds, so a whole island at the far ring is milliseconds. **Rooms are 45 times floors** — the walls' sweep, the doors' spanning tree and the windows — so the LOD policy's inner ring is the one to watch, and a tower (whose rooms are its floors times a floor's) is where the per-floor interior cells of 13 §13.4 item 10 will pay rather than materializing a whole tower at once; E16 is the measurement. The pool does not help the plan: cutting lots is a small part of it (the graph, the parks' passes and the districts are sequential), so `--jobs` stays for the building commands. From the release CLI, a district's fragment at `Floors` (`city fragment --district`, one thread, generation and proxies but not the JSON): 794 buildings and 45,263 instances in 10.2 ms, 370 buildings and 70,551 in 15.2 ms; the plan and its file, `city plan`, 12.5 ms.

## What is left open

- **The kits.** A kit in the ruins' socket convention attaches to the walls' centre lines on the module grid; the attachment, the kit's corner and opening members, and a facade's far tier are unbuilt.
- **Furniture and clutter.** Zones exist; the furnisher that fills them by the room's meaning ([07 §7.6](../plan/07-content-pipeline.md#76-procedural-generation-volume-content)) does not.
- **Point towers, courtyards, L-shaped footprints.** A tower is a slab (a bar at most twice as wide as it is deep); a perimeter block's courtyard is the space between lots, not a building's own; footprints are rectangles.
- **The station** is the civic shell's bar of departments with a transport workplace kind, not a concourse.
- **Terrain.** The plan reads the coastline and a mountain axis, not a heightfield; everything stands at y = 0. The island's terrain is the next generator's, and the building bases will ask it for heights the way the ruins ask theirs.
- **Interiors streamed by the tile.** A streamed tile in the inner ring generates its buildings at `rooms` on activation, 127 µs a building ([Performance notes](#performance-notes)); a tile of towers is where the per-floor interior cells of 13 §13.4 item 10 will pay, and nothing measures a ring of the city yet.

## Capability contract (ADR-0027)

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | none | stands alone |
| Component and event types | `engine.city`: `IslandParams` and its tables, `PlanFile` and its records, `BuildingFile` and its records, `Occupancy`, the enumerations, in `domain/city/schemas/city.schema` | done |
| Tick scheduler entry | none: nothing ticks; a plan and a building are functions of their inputs | not needed |
| Render-graph passes | none: the proxies are instances the renderer already draws | not needed |
| Scene generators | the placement generator `city` in `scene_gen`'s registry (`src/scene_generator.cpp`; [ADR-0046](../adr/0046-scene-generators-register-themselves.md)): a scene names it with a plan, and the world ring streams a tile's proxies through its per-tile query | done |
| Content-build derived step | `engine-content city plan --cache` writes `ddc/city/<key>/plan.json`; `building`, `fragment` and `yield` read it | done, as a command; a manifest step with the parameters as a document object is next |
| Protocol methods | none yet | not needed |
| Tunables | none: every parameter is the island's, and the island is content | not needed |
| LOD policy | `stage_for_distance`: rooms within 1.5 tiles, floors within 6, massing beyond | done |
| Determinism | `city::k_determinism` = `derived`, and bit for bit on every toolchain | done |
| Zero cost when unused | nothing calls the module unless `engine-content city` does or a scene names `city`; switched off, it is not linked | done |
| Tests and size table | `tests/plan_tests.cpp`, `tests/building_tests.cpp`, `tests/validate_tests.cpp`, `tests/scene_generator_tests.cpp`, `tests/size_table.cpp` (`Lot` 36 bytes, `Block` 72, `Street` 32, `Space` 28, `Wall` 32, `Opening` 20, `Proxy` 32, …) | done |
| Bench | `bench/city_bench.cpp`: `city.plan`, `city.building`, `city.validate`, `city.fragment.district` | done |
| Removal proof | `ENGINE_WITH_CITY`, off in the minimal build | works |

**Removing it.** `cmake --preset linux-clang-minimal` (or `-DENGINE_WITH_CITY=OFF`) drops the module, its schema library, its tests and its bench; it disappears from `build/<preset>/modules.json` and is listed there under `disabled_capabilities`, and `engine-content city` says the build has no city capability.
