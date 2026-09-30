# ADR-0049: The ground is drawn from the world's tiles: the rings' levels laid out by the tile ring, fed by a tile source

- **Status:** Proposed
- **Date:** 2026-09-30
- **Plan references:** docs/plan/04-renderer.md §4.3 (terrain, deformed clusters), §4.9 (streaming and residency); docs/plan/03-data-model.md §3.7 (one tile grid, many consumers); docs/plan/05-simulation.md §5.13 (deformable surfaces). Builds on [ADR-0040](0040-the-tile-ring.md) (the tile ring and its consumers), [ADR-0043](0043-dunes-as-a-function-of-time.md) (dunes as a function of time) and [ADR-0046](0046-scene-generators-register-themselves.md) (ground providers by name), and revises the latter's first revisit note.
- **Docs touched:** [renderer](../subsystems/renderer.md#the-ground-from-the-worlds-tiles), [world](../subsystems/world.md#the-consumers), [scene_gen](../subsystems/scene_gen.md#the-tile-source), [scene_collision](../subsystems/scene_collision.md), [terrain](../subsystems/terrain.md#where-it-attaches), [apps](../subsystems/apps.md), status notes in [04 §4.3 and §4.9](../plan/04-renderer.md#49-streaming-and-residency)

## Context

The world streams tiles round its observers (`systems/world`), a ground provider can open its tiles in a world (`scene_gen::GroundOps::open_tiles`), and the walker's collision ground lives on the world's tile grid (`systems/scene_collision`). The renderer did not: it drew the terrain as one cluster mesh of a fixed extent built at load, or, with `--terrain-rings`, that mesh plus square rings of finer chunks round the camera that the ground provider makes (`make_rings`). The desert ended at the scene's `extent`.

What the owner asked for, and the questions that had to be answered to give it:

1. **The desert does not end.** A camera flying one way for an hour keeps having ground, with memory bounded by what is resident rather than by the distance flown.
2. **Tiles stay contiguous** — no crack, no T-junction gap and no lighting seam where two tiles meet, at one level or two, a time-lapse step apart, and far from the origin (the owner asked on 2026-09-27 whether tiles had been tested for it; they had not).
3. **The time-lapse still holds** across tiles: the per-frame bound, exact handovers, one surface time.
4. **What is drawn is what is walked on.**
5. **The consumer does not know who made a tile**: a procedural ground and a tile set authored and built ahead of time are two sources of the same thing.

The questions that decide the shape:

- **One mechanism or two.** The rings already draw finer ground as chunks of cluster DAGs in fixed GPU slots, moved by the pool's terrain stage at one surface time, swapped a changed chunk at a time. Tiles could be a second drawing path beside them, or the same path laid out differently.
- **What a tile is to the renderer, and who meshes it.** ADR-0046 expected "`GroundTilesOps` growing a mesh entry": the provider meshes a tile and the renderer draws the mesh.
- **How tiles at different levels meet.** A finer tile beside a coarser one has vertices between the coarser one's: a T-junction, which the rasterizer can open into a crack a pixel wide. The rings hide the same thing with skirts.
- **Where the time-lapse's fields live.** Per level over a window, as today, or per tile.
- **What decides which tiles are drawn.** The renderer's own rule round the camera, or the world's ring.

## Decision

1. **One drawing mechanism, two layouts.** A world tile is a **chunk of a terrain level**: one chunk a tile, in a slot of the level's, drawn through the pool's terrain stage from the level's fields — the rings' GPU half, verbatim (`GpuScene`'s slots and arenas, `terrain_chunk_upload`/`show`, `TerrainLevelDesc`, `TerrainMotion`'s re-centre, freeze, carry-over and swap). What differs is the layout: `TerrainRingSet` lays chunks out as squares round the camera, `TerrainTileSet` as the world's tile grid, one level per world ring. `GpuScene` and `TerrainMotion` take either through one interface, `TerrainLevelSet`. The scene's own grid stays level 0: with tiles it draws nothing (its hole covers the world) and remains the carrier of the terrain's material and UV frame, and the reference a tile is held to. **The tiles are meant to survive**: the rings stay for scenes that ask for them — nothing that does not ask for tiles changes — and become a layout nobody asks for once a scene of tiles draws everything a scene with rings does.
2. **A tile is heights on the world's lattice, and the renderer meshes it.** Tile (x, z) at level k is the ground on the world lattice at the level's spacing (the tile's edge over `cells(k)`, one level per world ring), sampled with a one-point apron; the renderer builds its grid (two counter-clockwise triangles a cell, the scene grid's diagonal), locks its border, and builds its cluster DAG. This revises ADR-0046's revisit note: **the source says heights, not meshes**, because a tile's mesh depends on its neighbours' levels, which only the drawing side knows (decision 3), and the pool draws every terrain vertex from the fields at its lattice point, so a mesh a source made would be thrown away but for its triangulation.
3. **Seams are exact by construction, and hold for any pair of levels and times.**
   - *One level:* every border vertex is locked, so every DAG level keeps it; it lies on the world lattice, which both tiles count from the same origin in the same integers; and the pool writes it at `origin + (i, j) × spacing` with the height and normal of the level's one description. Two tiles' copies of a vertex are the same floats. **The 16-bit grids do not have to agree**: each mesh keeps its own, and a rest position is only used to find its lattice point, which needs a quantization step under half the spacing (a 32 m tile's grid is about 0.5 mm).
   - *Two levels:* the finer tile's edge along a coarser neighbour keeps only the coarser lattice's points — every other edge vertex collapses onto the nearer kept one and the triangles it degenerates are dropped, which turns the edge row into fans — so the two tiles have the same vertices along the edge and no T-junction. **And every vertex a tile shares with a coarser one is drawn from that tile's level**: position, height and normal from the coarser level's description, named in the vertex's rest normal (a heightfield vertex's rest normal is free, since the pool writes the normals the resolve reads; a skirt vertex already says so there). A corner is drawn from the coarsest of the four tiles round it. Both sides then draw a shared vertex from one description: the same floats whatever pair and blend each level is at.
   - *A time-lapse step apart:* each level has one pair and one blend, and every vertex two levels share is drawn from one of them (above), so a border between two levels whose fields are at different steps is exact too.
   - *Far from the origin:* a lattice point is an integer times a spacing that is a whole number of millimetres; at 25 cm or more that is exact in f32 to 4,000 km.
   - The price: a tile's mesh is a function of its level **and its neighbours' levels**, so a tile is rebuilt when a neighbour changes level.
4. **The seam is a tile source** (`scene_gen::TileSource`): heights on a window of the world's lattice at a spacing and a game time, whether they move with time, and how far their fastest feature travels between two times (the time-lapse's cadence). Nothing else — not a provider's name, its parameters, a seed, a gather or a file. The procedural source is a ground provider's view of itself (`GroundProvider::tiles()`: its `evaluate`, or its `grid` for a ground that does not move); **a tile set built ahead and read back from the derived-data cache is another** (the renderer's tests build one). The renderer's tile levels and the collision's ground ask a tile source, and the same one: what is walked on is what is drawn.
5. **Fields per level, over a window that moves rarely.** A tile level's fields cover a square of its lattice holding every tile the level can hold while the camera stays within a margin of the square's centre; the window moves only when the camera leaves that margin (the rings' re-centre rule, on the window), and its pair is then carried over to the new window by the rings' pair step, which copies what the two windows share and evaluates the rest. A tile coming or going inside the window costs a mesh and no field work.
6. **The world's ring decides which tiles are drawn.** engine-view registers a consumer on its tile ring — hysteresis, budget, observers — that hands the renderer the held tiles and their rings after each update in which they changed; `TerrainMotion` takes a change as it takes a re-centre (rebuild the tiles whose key changed off the frame, upload them into free slots, swap them all in one frame). Without the world capability, and in the renderer's own tests, the layout is the ring's first-fill rule (a tile's ring is the band its centre's distance falls in), which is also the first layout the renderer builds before the world's first update.

## Consequences

- The desert does not end: a scene of tiles draws ground wherever its rings reach, and its device memory is the slots and fields its rings can hold, sized at load.
- Every guarantee the rings' tests hold — one surface time, the per-frame bound, exact handovers, a swap that changes tessellation and never a lattice point's height, the visibility invariants — holds for tiles through the same code, and the tiles' tests hold the seams by the visibility buffer.
- A neighbour's change of level rebuilds a tile, and a tile's rest heights are built on a worker, so a fast camera's cost is tiles built a second; the flights measure it.
- The deformed-vertex pool draws every tile vertex. A cluster the pool's budget had no room for draws its rest pose, as a ring's does, and a stitched vertex's rest normal is then its level's name rather than a normal: the pool's floor for terrain levels (`renderer.terrain.pool_mib`) is what keeps that from happening.
- A scene of tiles is not a dynamic scene: instances that come and go (a streamed world's ruins) have no pool, so ground tiles beside streamed placements are refused until the tail learns the pool.
- A tile source serves heights; **maps** — the colour and the sand share a tile's material samples — are still the scene's baked maps over its grid's UV frame, clamped at its edge. A tile set built ahead would bring its own; nothing takes them yet.
- ADR-0046's revisit note about a renderer drawing tiles is answered differently from how it was written, for the reason in decision 2.

## Alternatives rejected

- **A second drawing path for tiles.** Everything that makes the rings safe (slots, arenas, the pool stage, the freeze and swap, the padding across a rebuild, the tests over them) would have been written twice.
- **Skirts between levels, as the rings have.** A skirt fills a crack with a wall of the ground's colour; it does not remove the T-junction, and the brief's "no T-junction gap" is a statement about the mesh.
- **Every tile border at the coarsest spacing**, so no tile depends on its neighbours. It makes a tile a function of itself alone (a tile set built ahead could carry finished meshes), at the price of coarse lines every tile edge near the camera, and it does not scale to a far level coarser than the rest.
- **Midpoint vertices blended to lie on the coarse edge** instead of collapsed. They lie on the edge in exact arithmetic only; after projection they can miss by a rounding, which is the T-junction crack again.
- **Fields per tile** (a field block per tile slot and a level description per tile). No window ever moves, but a tile that arrives while a field is on its way needs its own block evaluated at every live time, and a field is installed only once every shown tile has one; the window with a margin gets the same result from machinery that exists and is tested.
- **The renderer deciding the tiles itself** from the camera. It would be a second LOD policy beside the world's, which is what ADR-0040 decision 1 exists to prevent.

## Revisit when

- A scene of tiles wants streamed placements beside it: the tail learns the pool (or tiles leave it), and this ADR's refusal goes.
- A tile set built ahead brings maps: the tile source grows maps and the slots a material each.
- The overlay (footprints) is drawn: the tile source's heights take the world's ground consumer's overlays, or the pool reads a deformation map (05 §5.13).
- The far tier: a level coarser than a tile is wide (a tile of several of the world's tiles) needs the world's ring to hold tiles at more than one size.
- The rings are dropped: when a scene of tiles draws everything a scene with rings does and none of the committed scenes asks for rings.
