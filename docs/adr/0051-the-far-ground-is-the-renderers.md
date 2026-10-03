# ADR-0051: The far ground is the renderer's: coarser tile levels past the world's rings, fed by filtered lattices

- **Status:** Proposed
- **Date:** 2026-10-03
- **Plan references:** docs/plan/04-renderer.md §4.3 (terrain), §4.9 (streaming and residency); docs/plan/03-data-model.md §3.7 (one tile grid, many consumers). Builds on [ADR-0050](0050-the-ground-is-drawn-from-the-worlds-tiles.md) and answers its "far tier" revisit note differently from how it was written.
- **Docs touched:** [renderer](../subsystems/renderer.md#ground-to-the-horizon), [scene_gen](../subsystems/scene_gen.md#the-tile-source), [world](../subsystems/world.md#an-authored-world), [terrain](../subsystems/terrain.md), [apps](../subsystems/apps.md), [the experiment](../experiments/far-ground-2026-10-03.md)

## Context

The world's tiles (ADR-0050) end at the world's outermost ring, 2 km on the endless desert. From a few hundred metres up that is a straight edge, and past it the sky model's planet in a flat colour. ADR-0050's revisit note expected the fix to be "a tile of several of the world's tiles", needing "the world's ring to hold tiles at more than one size". The questions:

1. **Whose are the far levels** — the world's ring (more rings, larger tiles) or the renderer's.
2. **How they are laid out** so that every level meets the next with the seam rules of ADR-0050 (no crack, no T-junction, one description per shared vertex), with a boundary on the near side that is the world's ring — a band of whole tiles, with hysteresis and a budget, not a square.
3. **What heights a coarse lattice carries.** A 64 m lattice that point-samples crests 20 m apart is any height between floor and crest, shimmers as they move, and draws a wrong silhouette.

## Decision

1. **The far levels are the renderer's.** The world streams tiles round its observers with records, collision, a store and a walker; ground 50 km away needs none of that, and a ring 100 km across would be millions of the world's tiles. `TerrainTileSet` lays the far levels out itself from the camera, after the world's rings, and nothing in `systems/world` changes.
2. **They are tile levels of the same mechanism** (ADR-0050 decision 1): slots, arenas, the pool's terrain stage, fields per level over a window, the freeze and carry-over, the swap. Far level *k* has twice the spacing of level *k − 1* (the first `far_ratio` times the outermost ring's), tiles of `far_cells` cells — so twice the tile — and a **square window** twice the half-side, centred on a multiple of the next level's tile and moved by the rings' margin rule. A level draws its square less the next finer level's square; the first draws its square less the world tiles the rings draw, as a **hole mask** of whole world tiles in each far tile.
3. **The squares nest whatever the camera does.** The first square holds every tile the rings can hold from wherever its centre may be, and every square is at least six of the next level's tiles across its half-side, which keeps it inside the next one's (two centres are at most a margin and half a snap from the camera each). So every level's border is on the next level's lattice and every hole is whole tiles of the level round it. A ring's tile is drawn only a tile inside the first square, so every tile round it is drawn by a ring or the first far level.
4. **The seams are ADR-0050 decision 3's, unchanged.** A ring's tile beside the first far level collapses its edge onto that level's lattice and names the level in the vertices it shares; a far tile's border along the next level does the same; a far tile's hole border is locked and drawn from its own level, which the finer tile beside it names. The level a vertex names is a sixteenth of a turn now (sixteen levels: the grid, seven rings, eight far levels), where it was an eighth.
5. **The tile source answers at a spacing and a filter** (`TileSourceOps::heights(..., filter_mm, ...)`, `travel_m(..., filter_mm)`): at 0 the ground at each point, as before; at a far level's spacing what that lattice can carry. The dunes keep the bands whose cell is at least four of its points and stand the rest in with their mean height, flat and still; the time-lapse's cadence for a far level is the fastest band it keeps. A source built ahead answers a filter from a coarser lattice it stores (its mip chain) and refuses one it was not built with.

## Consequences

- The ground goes on to 78 km in every direction with the defaults (6 far levels; 115 km from the last square's centre along the axes and 162 km to its corners), past where flat ground meets the planet's horizon line from any height up to 1.9 km, so the sky's planet does not show below the horizon (five, the first default, left a band of it from about 500 m up), at a fixed device cost reserved at load; [the experiment](../experiments/far-ground-2026-10-03.md) says what is measured of it.
- The world's ring and its consumers are untouched; a host with no world (the renderer's tests) gets far levels from the same set.
- The far levels draw the source's filtered heights, which the collision never reads: nothing walks there.
- Every level, near or far, shares the one surface time and its own per-frame bound; a far level's pairs span weeks of game time (its kept bands are slow), so it blends rather than steps.
- A far level re-centres every quarter of its half-side the camera travels; the finest does so about every 1.1 km on the endless desert and builds about 210 far tiles (its new strip, the row whose border flag it cleared, the coarser level's tiles the strip uncovered): 33 ms of worker wall offscreen against 13 for an ordinary rebuild, 17 ms to carry its pair over the strip, and 2.4 ms of the frame thread's staging ([the experiment](../experiments/far-ground-2026-10-03.md#a-far-squares-move)).
- The rings build about twice the tiles with far levels beside them (71 a rebuild at 100 m/s against 32): an outer ring's tile collapses its edges and corners onto the far level, so the boundary's motion re-keys the outer tiles round it, diagonally too. That is decision 4's seam rule at work, and the larger share of what a far level costs a rebuild.
- The edge that remains is where the last level ends; past it the sky model's planet. The aerial-perspective table ends at 32 km, so ground beyond it is drawn no hazier than at 32 km (renderer.md, "Ground to the horizon").

## Alternatives rejected

- **More world rings with larger tiles** (ADR-0050's note). The world's ring holds one tile size; a ring of 256 m "tiles" would be thousands of world tiles each carrying records, store rows and collision, or a second tile grid in the world with its own consumers — for ground nobody walks on.
- **A clipmap of whole squares with no hole mask**, the first square's hole being a square. The rings are a band of whole tiles with hysteresis, never a square; a square hole would either overlap the rings or leave a gap between them.
- **Skirts between the far levels.** ADR-0050 rejected them for the near levels for the reason that holds here: a skirt fills a crack with a wall; it does not remove the T-junction.
- **Point samples at the far spacing.** The shimmer and the wrong silhouette above; the CPU test measures both.
- **Supersampling the source for a box filter.** Sixteen evaluations a point for the bands a spacing carries, to round off what a point sample of them already draws right.

## Revisit when

- An authored world arrives: its content build writes the mip chain (world.md, "An authored world").
- The aerial-perspective table reaches past 32 km, or the sky's planet takes its albedo from the ground: the far edge's hand-over changes.
- A camera climbs past a few kilometres: more far levels, or a horizon on a curved ground.
