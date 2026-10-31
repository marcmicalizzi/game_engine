# ADR-0059: The far forest is the renderer's, one prefix instance per world tile

- **Status:** Proposed
- **Date:** 2026-10-31
- **Plan references:** docs/plan/04-renderer.md §4.3; docs/plan/10-roadmap-risks.md (R67, R111)
- **Docs touched:** [renderer](../subsystems/renderer.md#the-far-forest), [flora](../subsystems/flora.md#the-impostor-chain), [roadmap](../roadmap.md) (R111)

## Context

A streamed world draws its own trees tile by tile ([W73](../subsystems/renderer.md#authored-instances-per-tile)):
inside the world's outer ring a tile's trees are its distance forms, past it nothing. From the
forest proof of concept's mountain top (v23: 243,809 authored instances, 64 m tiles, rings of 0.75,
3, 10 and 20 tiles) the forest therefore ends in a line 1,280 m out, and the visibility id's pair
cap (2^24, the owner's) withholds whole outer-ring tiles on top of it (242 in the owner's run), which
leave holes inside the ring too. The owner asked for trees that go to a lower representation
instead of vanishing.

The ground solved the same problem with far levels laid out by the renderer in nested squares
round the camera, each a terrain level of slots and arenas the GPU scene reserves at load and fills
as the squares move ([ADR-0051](0051-the-far-ground-is-the-renderers.md)). The question was whether
the trees take that shape.

Facts that decided it:

- **The forest is finite and authored.** Every tree is in the scene file and read at load; a far
  tree is not generated from a lattice, so a coarser far level has nothing coarser to sample. The
  ground's levels exist because the ground is infinite and each level is a filtered lattice.
- **Pairs are not the budget for a far tile.** A tree's coarsest form is its whole-tree star (16
  planes) or its DAG's decimated top: a tile's trees merged are a handful of clusters. The v23
  region's whole far forest, every tile at once, is 93,454 pairs in the prefix (1,812 tiles, 59,178
  trees; [the measurement](../experiments/far-forest-2026-10-31.md)), where its streamed form was
  millions.
- **The hand-off must be exact.** The ring decides by world tile, and withholds by world tile, so a
  far form that hands off at any other granularity either draws a tile twice or leaves the
  withheld tiles empty.
- **The GPU scene's slot machinery is not this task's to change** (it is another agent's work this
  night), and the prefix already takes a load's own meshes and instances and moves a loaded
  instance through the table flip without waiting (`move_instances`, "Moving an instance").

## Decision

1. **The far forest is the renderer's and is built at load.** When a world streams its authored
   instances (W73) and the host asks (`RenderSettings::far_forest`, engine-view's `--far-forest`,
   on by default), `stream_authored` takes every streamed instance whose fitted height is at least
   `renderer.far_forest.min_height_m` (4 m) and whose mesh's coarsest form has at most
   `renderer.far_forest.max_triangles` (512), and merges them **per world tile** into one rigid
   mesh: every instance's coarsest form — the clusters of its DAG whose parent error is terminal,
   which is the whole-tree star where the flora container has one — with the instance's transform
   applied, the tile's corner its origin, one part per (mesh, material). Each tile is a mesh after
   every other and an instance after the prefix: the far forest is its own run of the prefix,
   which never moves.
2. **One world tile is one far tile.** The hand-off is the tail's own block list: a far tile is
   drawn unless the tail holds a block named by its key (`far_tile_key`, the world's `tile_key`)
   with instances in it. A tile the ring withholds (the pair cap's) or does not hold is the far
   forest's. The moves ride the same table flip as the tail's change, so no frame draws a tile
   twice or not at all. A far tile the tail draws is **parked**: its instance moved a million
   kilometres below its corner (`k_far_park_depth_m`), where no view, cascade or camera ray
   reaches, because the GPU scene has no per-instance switch and its slot records are not this
   change's. This is a departure from ADR-0051's nested squares: the far forest has no lattice to
   coarsen, and its squares would have to hand off to world tiles anyway.
3. **The cut thins it by measured errors.** A star's part carries a second level, the **cross**:
   the star's planes in two of its four directions, a right angle apart, held to the largest
   distance any vertex of a dropped plane lies from a kept one, times the instance's scale — the
   error the star itself is held to in flora.md, measured the same way. Every other part is one
   level that never coarsens, which is what its mesh draws from that far. No coarser stand-in is
   invented: a coverage-preserving coarser form (an unsliced two-plane bake, R92's octahedral
   frames) is the flora capability's to bake.
4. **The far forest casts as any rigid instance does.** It carries no caster policy: a policy's
   bits would be lost at the first hand-off, because `GpuScene::place_instances` rewrites an
   instance's flags from its placement alone (reported to the owner of that file). So under traced
   shadows its drawn clusters are in the chain and cast, and the cascades take it where their
   frusta reach it — the two farthest, at the proof of concept's view distances. Untraced is the intent once a move keeps
   the policy.
5. **Each far tile is a derived-data entry** (`<ddc>/far_forest/<key>.far`), keyed by
   `k_far_forest_version`, the tile, the world's tile size and every merged instance's mesh form
   (its roots' bytes and materials), fit and placement; the materials are copied from the
   instances' meshes at every load with their built textures' records and none of their bytes,
   so the GPU scene finds the textures the species uploaded.

## Consequences

- From the mountain top and the pond the forest goes on past the ring and fills the tiles the pair
  cap withheld, at the cost of its pairs in the prefix (counted by `account_pairs` with the rest of
  the prefix, so the world's consumer admits the tail round it) and its triangles.
- The far forest is built for the whole authored set at load, not as tiles come into a square: the
  prefix and its memory are the whole set's, and a world larger than its authored region costs
  nothing more. A world whose authored set outgrows the prefix (millions of far tiles) would want
  ADR-0051's slots; nothing here forbids adding them.
- A tile's far form and its streamed form never draw together, so there is no cross-fade at the
  ring: a tile switches whole when the ring takes it or lets it go, as the ring's own forms do.
- `GpuScene::place_instances` should keep an instance's caster policy and static-structure bits
  across a move; until it does the far forest is traced and cascaded.

## Revisit when

- `place_instances` keeps the policy bits: make the far forest untraced and in no cascade.
- A scene's authored region is large enough that its far tiles' prefix pairs or memory matter
  against the cap or the device (the renderer page has the arithmetic): lay the far tiles out in
  slots round the camera, as the far ground's are.
- The flora capability bakes a coarser whole-tree form than the star (R92), or the conifers get
  theirs (G83): the far forest takes it as a level, by its error.
