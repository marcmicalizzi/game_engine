# ADR-0065: A texture's levels are resident by an exact bound, trimmed by what the frames sampled

- **Status:** Accepted (2026-11-17)
- **Date:** 2026-11-16
- **Plan references:** docs/plan/04-renderer.md §4.9 (streaming and residency), [ADR-0063](0063-memory-is-held-under-one-budget-and-by-travel.md) (one budget, a governor over pools, the envelope by travel), [ADR-0036](0036-built-textures-in-the-engines-own-container.md) (the `.tex` container)
- **Docs touched:** [texture](../subsystems/texture.md) ("Levels resident by what the frame samples"), `docs/roadmap.md` (R172)

## Context

The renderer uploads every level of every built texture a scene names, at load, and keeps it
whole. The forest's 37 furnishing props cost 1,078 MiB in a small scene; G14 showed that a cap at the
cook does not take it back, because a generated prop's atlas is not over-dense for a walker at 4K.
What was not known is how much of a resident chain the frames read. R172 measured it with a pass
that takes every pixel the way the resolve does and keeps, per texture, the finest level its
samples asked for (`renderer.textures.feedback`; texture.md has the method and every number):

- the village's interior walk (48 textures, 168 MiB resident): **56 % of the resident bytes were
  never asked for by any frame of the walk** at 3840 x 2160, 58 % at 1920 x 1080; one frame asks a
  median of 12.0 MiB at 4K and 1.9 MiB at 1080p;
- a residency that keeps what was asked within the last 8 s holds a median 44 MiB at 4K and 11 MiB
  at 1080p, and has to bring in 1.6 MiB/s on average, at most 14 MiB in one frame;
- the finest level asked moves slowly: in a median frame no texture's changes, at the 99th
  percentile three do.

The published shapes (texture.md cites each): **virtual texturing** (a page table over the world's
texels, a feedback buffer, a fixed pool of pages: Barrett 2008, van Waveren 2009, Chen 2015);
**sampler feedback streaming** (D3D12's MinMip map, an atomic minimum of the finest mip asked per
region of a tiled resource, read back and reset each frame, a load on a lower value and an unload
after a period unused); **mip streaming of whole textures** (Unreal Engine: a wanted resolution
per texture from each primitive's world bounds and texel size against the view, a pool, and under
it one mip at a time dropped, non-visible and least recently seen first); and **sparse residency**
(Vulkan's sparse images with a driver-chosen mip tail).

## Decision

1. **Whole textures by level, not pages.** A texture is resident from a top level `r` down: an
   image whose level 0 is the chain's level `r`, sampled through the same `SampleGrad` with no
   shader change, since a sampler on the smaller image reads level `max(lambda - r, 0)` of it, which
   is level `max(lambda, r)` of the whole. No page table: an indirection in every sample of the
   resolve at 3840 x 2160 with no upscaler is the cost the page has refused since R145, for unique
   texels over a world this engine does not have.
2. **The bound decides what is released without changing a pixel.** Per texture, the finest
   level any pixel can read: from each mesh's measured density (G14's measure, taken at its **least** over every
   triangle of every level of the mesh's cluster DAG rather than the 1 % figure a cap uses: the
   least texels a metre is the most a pixel can magnify; the first leg's draft said "maximum"
   for the resolution it asks, and stage 1 found the wording read the other way), at
   each live instance's scale, and the nearest any observer comes to the instance within the lead
   (ADR-0063 decision 3's segment). Levels finer than it are released at once and never change the
   picture; an imported asset gets it from its own triangles, with no statement. The bound is
   evaluated over the instances that exist now, so a mesh the tail places larger raises it as it
   arrives (G14's "open" meshes need no special case).
3. **The feedback trims only under pressure.** What the frames sampled (the measurement pass in a
   production form: one pixel in sixteen a frame, rotating, its cost stated before it is on) says
   which levels between the bound and the floor nobody looked at: occluded, facing away, behind.
   Those are trimmed only when the pool is over its ceiling, a texture to the finest level asked
   within a hold (8 s), least recently asked first: the governor's "coarsen" step for textures.
4. **The floor stays.** Every texture keeps its levels of 128 texels a side and smaller (0.4 % of
   a 2K BC7 chain): the far tier's lookups and the rays' explicit level-0 reads of far surfaces
   never find nothing.
5. **A promotion comes in off the frame and late is softer, never a stall.** The added levels are
   read from the `.tex` by range on a worker, admitted inside the R147 commit allowance (bytes a
   frame), and swapped in through a second bindless slot (pair) so that no descriptor a frame in
   flight reads is rewritten; the material entries that name the texture are rewritten by the
   frame's upload pass behind the barrier that waits for frames in flight, and the old image goes
   after `k_joint_slots + 1` frames, as a leaving tile texture does. Until it lands the texture
   draws its resident levels. A settled view promotes nothing, so the steady frame loop allocates
   nothing.
6. **The readers that name level 0** keep what they read: an alpha-tested material's base colour
   (the rays' mask tests at any distance, and the micromap bake) is held whole until the rays take
   their level from a ray cone; the bounce's hit colour reads the resident top, a mean of what it
   read before; the reference path tracer loads whole.

## Accepted, and the two open questions settled (2026-11-17)

The owner handed the first leg's two questions to the coordinator ("Use your judgement on those,
based on what fits best for the objectives of the engine"), and these are the coordinator's
judgement under that delegation:

1. **The shape is accepted** as decided above: whole textures by level, the bound first, the
   feedback's trim only under pressure, promotion off the frame through a second slot pair. It costs
   a scene with no pressure nothing, an imported asset gets it with no statement of its own, and no
   sample pays an indirection; virtual texturing buys its page tables only for very large unique
   textures, which this engine's tiled sets and prop atlases are not.
2. **The floor is a tunable, `renderer.textures.floor`, 128 texels a side by default** at the top
   tier (about 22 KB a BC7 texture, 65 KB a material of three): the floor is the failure picture
   after a cut, and 128 is the smallest that does not read as a blur on a near prop at 4K. A lower
   tier may take 64; stage 2 sets that by measurement under an 8 GB budget.
3. **The hold is a minimum age, not a clock** (8 s, a tunable when stage 2 builds it): under no
   pressure nothing is trimmed; under pressure levels go least recently asked first and only as far
   as the ceiling needs, and nothing younger than the hold goes, so a look round and back never
   reads again. ADR-0063's open decisions stay unpresumed: the ceiling is whatever that ADR's budget
   gives.

Stage 1, the bound, is built (R172's second leg; texture.md has what it does and what it measured).
Stage 2, the feedback's trim, is not.

## Consequences

- A texture is a pool in ADR-0063's sense: bytes held, bytes releasable now without changing a
  pixel (above the bound), floor (the tails), and a trim (decision 3). With no governor (today) the
  pool keeps the bound and never trims; its ceiling is then `renderer.textures.budget_mib`'s kind
  of tunable, and the three questions ADR-0063 leaves open plug in as follows:
  - *the budget's form*: whichever number is chosen gives this pool a ceiling; the bound needs none;
  - *being outrun*: a promotion backlog whose drain time passes the lead is the texture form of
    flight: wanted levels fall to the bound at the observer's place now, not ahead;
  - *fast travel*: covered, release to the floor and promote at the destination with the whole
    machine; shown, a preload observer's bound counts as an observer's.
- Two slot pairs a managed texture: the bindless set's 16,384 sampled images hold it for any scene
  measured so far (the forest's 5,392 textures would take 21,568 slots with variance pairs: the
  forest needs the swap by descriptor update after the pair's last reader, or a larger set).
- A measurement pass exists and stays off by default (`renderer.textures.feedback`).
- Forbidden from here: a texture uploaded whole where the bound says no pixel reads its top; a
  frame that waits for a level; a level released below the floor.

## Revisit when

- A single unique texture passes about 8K a side (a terrain's own image): its pages would pay for
  their indirection.

- The production feedback's cost is measured over a flythrough at 3840 x 2160: if it passes 0.1 ms,
  the trim rides on the resolve's own derivatives instead of a pass of its own.
- A scene's textures are mostly unique texels over terrain (a painted world, not a layered one):
  then virtual texturing's pages pay for their indirection.
- Sparse residency is measured on the three test cards: if a bind is cheap and the mip tail is
  small, decision 5's slot pairs give way to binding levels in place.
