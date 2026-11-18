# ADR-0068: The traced chain holds what frames asked for, shares at the cluster, and packs what it keeps

- **Status:** Proposed
- **Date:** 2026-11-17
- **Plan references:** docs/plan/04-renderer.md §4.4 (ray tracing: the cluster path, the fallback's fixed levels, the budget manager), docs/plan/13-reference-consumer-games.md ("RT relevance LOD"), [ADR-0025](0025-cluster-acceleration-structures.md) (cluster acceleration structures), [ADR-0063](0063-memory-is-held-under-one-budget-and-by-travel.md) (one budget, a governor over pools)
- **Docs touched:** [renderer](../subsystems/renderer.md) ("A guess is not a demand", "What a place holds"), [apps](../subsystems/apps.md) (`--memory-report`), [the measurement](../experiments/rt-structures-memory-2026-11-17.md), `docs/roadmap.md` (R81, R62)

## Context

Batch 67 measured a village walk at 3840×2160 under traced primaries and shadows holding 3.7 GB
of device memory before textures, 2.1 GB of it ray-tracing structures, and read that as what a
4 GB card would be asked for. Roadmap R81 proposed the remedy it had carried since September: one
bottom-level structure a mesh, shared by its rigid instances.

[The measurement](../experiments/rt-structures-memory-2026-11-17.md) says the bytes are somewhere
else. By kind, on every scene flown:

- **The frame's own cluster set is the whole of it, and it was sized by a guess.** A scene too
  large to hold whole starts its chain at an estimate of every pair a frame could build, clamped
  to the budget, and the capacity policy holds a capacity for its shrink window (two minutes) and
  then only halves it. The village's frames build at most 6,457 structures; the chain held the
  budget's 330,125, fifty-one times that, for the whole flight. The budget is a share of the card
  (a sixteenth, never under 512 MiB), so "2.1 GB" is the RTX 5090's sixteenth and not the scene's
  need; a 4 GB card would have held its own floor, an eighth of itself, for the same frames.
- **Every slot of a set is reserved at the driver's most.** 6,144 bytes a cluster of up to 124
  triangles (8,832 where the set allows opacity micromaps), whatever the cluster holds. The rigid
  forms' structures, packed in batch 59, take 18 % of their reservation; NVIDIA's own sample says
  host-side estimates are "often 3-5x bigger than actual" and builds into scratch first so that
  its allocator can use the built sizes.
- **What rigid instances share is already shared.** A rigid cluster is built once into the
  resident cache and referenced by every instance and frame that draws it; a far ring's rigid form
  has one bottom-level structure a mesh. What an instance still owns is its bottom-level structure
  over those references, 160 to 308 bytes a referenced cluster: 15 MiB on the village and 20 on
  the streamed forest, one or two percent of the chain while the guess is held.
- **What is built again every frame is what moves**: deformed instances and the ground's ring
  slots, each (instance, cluster) pair of the cut, per view.

The alternatives on the table, each read in its source:

1. *One bottom-level structure shared by the rigid instances of a mesh* (R81 as written; the
   "BLAS sharing" of NVIDIA's `vk_lod_clusters`): the others reuse the structure of the instance
   that needs the most detail. The sample's page gives the price: "the shared BLAS will cause
   objects afar to use more detail than necessary", and the traced level is no longer the
   rasterized one. It saves bottom-level builds and their bytes (62 MB to 9 MB in the sample's
   scene), which here are the 15 to 20 MiB above.
2. *A small set of cuts by distance band* (the sample's "caching": a structure at "a single
   discrete LoD level"; its mitigation "more than just one shared BLAS for different LoD
   levels"): this renderer's static forms are that, with the raster side drawing the band's cut
   too, which is what keeps the two pictures the same.
3. *A traced level coarser than the drawn one* (the plan's own fallback, "RT LOD is coarser than
   raster LOD by design"; Unreal's default, where "the Fallback Mesh is used for Nanite-enabled
   meshes" under ray tracing): NVIDIA's guidance is that the same level for both "is an efficient
   way to avoid self-intersection artifacts such as surface shadowing itself", and that where the
   levels differ back-face culling "is often needed". R94 measured exactly that shadowing here and
   declined a bias for the rays.
4. *Compaction and packing* (the same guidance: compaction saves "up to about 50%" and "is most
   practical to do for BLASes that are only built one time"; the cluster extension's
   `MOVE_OBJECTS` moves a built cluster structure and, in its size mode, returns the size of an
   existing one).

## Decision

1. **A capacity no frame asked for is a guess, and is given back.** The first capacity of a chain
   that does not hold its whole union is provisional: once `renderer.rt.first_seconds` (10) of
   frames and the shrink window's frames have been observed, and the scene has stopped growing, a
   guess more than twice what those frames wanted with their headroom goes to that fit at once,
   with no floor and no memory of it as a peak. A chain that holds its whole union is a guarantee
   (no frame can want more) and keeps the ordinary window. Everything a frame *did* ask for keeps
   R116's rules: the window, the minute's peak, the floor. (Stage 1, built with this ADR.)
2. **Rigid geometry is shared at the cluster, and at the mesh only where the rasterizer draws the
   shared cut too.** The cache and the static forms are the two sharing levels. A bottom-level
   structure shared by instances whose rasterized cuts differ is not built: it saves the smallest
   part of the chain and gives up the invariant below. Moving the static forms' boundary inward
   (a band of fixed cut inside the inner ring) is permitted as a *raster* decision, measured as
   one, never as a traced-only cut.
3. **Nothing the picture draws is traced coarser than it is drawn.** The three invariants stand
   as they are, with their tests: traced visibility is the rasterized cut word for word (the ray
   query test, `rt cache: rigid structures built once trace the per-frame build's bytes`,
   `engine-view: a rigid form's structures built once trace the per-frame build's bytes`); a
   shadow is cast by the frame's own cut at the picture's LOD; the KHR and cluster paths are
   pixel-identical (the cluster AS test). What may differ, when a later stage needs it, is only
   what no pixel shows: a caster outside every view's frustum may be built at a coarser cut whose
   projected error is bounded and stated, since no pixel has a cut of it to disagree with. That
   exception is named here and not taken: it needs its own measurement of a shadow's edge.
4. **A structure kept across frames is not kept at the driver's reservation.** The cache's slots
   go by the cluster's own triangle count first (classes with their own stride, the builds already
   explicit), then to their built size by a move. The frame's own set, rebuilt every frame, takes
   the same classes (T138's design). Each is a stage with its own pair.
5. **The chain's ceilings are shares of the card on small cards too.** The budget's 512 MiB floor
   and the cache's 256 MiB floor are an eighth and a sixteenth of a 4 GB card. Whether the floors
   follow the card below 8 GB is the owner's to decide with the governor of ADR-0063; this ADR
   records that with decision 1 the floors are ceilings again and not what a small scene holds.

## Consequences

- The village's walk holds its demand a step above, not the card's sixteenth; a card with 8 GB or
  4 GB holds the same megabytes for it as the 5090 does (the measurement's last section).
- A view that asks for much more than the first ten seconds did, in the frames before the growth
  lands, is short for the two frames the read-back takes, as any view is after the chain has
  shrunk: the guess used to cover the first two minutes of a session against that, and no longer
  does. A frame somebody waits for (`render_offscreen`) is drawn complete as before.
- R81's shared bottom-level structure is declined with its numbers; R81 becomes the row for this
  ADR's stages. R62 (the build *time* budget) is untouched by it.
- The place report names the chain's parts, so the next stage's pair is read off one block.

## Revisit when

- A scene's bottom-level set passes a tenth of the smallest card's budget (50 MiB), or its `blas`
  zone a millisecond of the frame: decision 2's arithmetic no longer holds. (It is 15 MiB and
  0.05 ms on the village, a tenth of that scene's chain once the guess is gone.)
- A second vendor ships cluster structures, or the KHR path is wired into the renderer: packing
  and moves must be restated for it.
- The governor of ADR-0063 is built: the provisional window becomes its question (a pool under
  pressure may give a guess back sooner).
- A walk shows a hole or a missing shadow in the first seconds after a load that the old guess
  would have covered.
