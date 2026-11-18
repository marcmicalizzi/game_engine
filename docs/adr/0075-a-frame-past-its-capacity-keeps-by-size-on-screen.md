# ADR-0075: A frame past its chain's capacity keeps by size on screen, and a tie is never broken

- **Status:** Proposed
- **Date:** 2026-11-18
- **Plan references:** docs/plan/04-renderer.md §4.4 (ray tracing: the budget manager's "priority by screen contribution"), [ADR-0025](0025-cluster-acceleration-structures.md) (cluster acceleration structures), [ADR-0068](0068-the-traced-chain-holds-what-frames-asked-for.md) (the traced chain holds what frames asked for)
- **Docs touched:** [renderer](../subsystems/renderer.md) ("Past the capacity, by size on screen", and a pointer from "The ray tracing chain's memory"), `docs/roadmap.md` (R174)

## Context

The ray tracing chain holds a capacity of cluster structures a step above what recent frames
built, under a budget that is a share of the device. A frame that wants more keeps whole
instances, drawn structures before casters, and spends the capacity along a drop order first-fit:
an instance that does not fit is skipped and the next one tried. The order is the layout's: a
scene read whole in instance order, a streamed world's prefix in slot order and its tail's blocks
by ring, tile key and first slot, each block's slots in slot order. So the kept set follows the
order the placements reached the renderer. Handed the same placements in another order (G205's
hand-off cells against the whole hand-over on the engine meadow, `--raster rt`), the same budget
drops 19,674 instances' structures in both runs and the traced pictures differ in 3.5% of a
3840 × 2160 view; under traced primaries a dropped instance is a hole. R170 had already made the
rasterized picture there a function of the scene (the cross-fade's seed by placement).

The alternatives weighed (renderer.md has the numbers):

- **Size the chain so it never drops.** Frames can want more than the chain holds before a growth
  lands, so this is holding the union: 2,629,611 structures, 16.2 GB at the driver's 6,144 bytes,
  on the desert overlook's surround. The budget is a ceiling by design.
- **Keep the walk over a placement order.** No GPU change, but the guarantee holds only where every
  block's rank is a function of the placements it holds, and inside the crossing ring what is kept
  is a hash's pick rather than what shows most.
- **A key the scene and the camera fix, cut as a prefix.** A histogram over a quantized key with
  each item's structures, a scan to the crossing bin, and a second histogram over a placement hash
  inside it.

## Decision

1. **A frame past its capacity keeps by a key of the instance and the eye, never by a place in a
   list.** The key is the instance's size on screen — the squared distance from the eye to its
   mesh's sphere's centre against the square of the sphere's radius times its largest axis scale,
   as `asuint(d²) − asuint(r²)` quantized to 15 bits — then 16 bits of a hash of its placement
   (its cell relative to the eye's and its 3×4), with the item's kind placed so that under traced
   primaries every drawn structure precedes every caster, and otherwise the drawn precede the
   casters at one size.
2. **Each capacity is spent as a prefix of the keys.** The cut is the least key at which the
   structures keyed at or below it pass the capacity; everything below it is kept, everything else
   dropped. The frame's own structures and the cache's references are cut separately.
3. **A tie is never broken.** Items with equal keys are kept or dropped together.
4. **The rule has a CPU mirror the shader is held to**, `gfx::rt_keep` with `gfx::rt_keep_key`,
   as `geometry::lod_selects` mirrors the cull pass's LOD test; the keys are computed without
   contraction so the CPU reproduces them to the bit.
5. **The layout's drop order goes**: `rt_drop_order`, its per-slot buffers, the first-fit walk and
   its one-thread fallback. Nothing is kept beside the new rule.

Code complies when no pass that decides what a frame past its capacity keeps reads an instance's
index, slot, block or position in any list except to address that instance's own data.

## Consequences

- Two runs that hold the same placements in any order keep the same instances' structures at the
  same capacity, so their traced pictures and traced shadows are the same; a scene moved by whole
  cells with its camera keys alike.
- What a frame at its budget shows changes: the instances it drops are the smallest on screen
  rather than the layout's last, so under traced primaries its holes are the smallest the capacity
  allows, and the farthest ring's casters going first becomes the same rule at a finer grain.
- A prefix gives up first-fit's use of the room the items at the cut leave — less than their own
  structures — and a cut can keep nothing only when one instance wants more than the whole
  capacity, which a budget's floor excludes and a growth in flight can show for a frame or two.
- Every frame records two more passes (empty when it fits, about 5 µs); the chain holds 8 bytes a
  slot and 1 MiB more and 32 bytes a slot and the order's copies fewer; the CPU no longer lays an
  order out at every tail change.
- The capacity policy, its bounds and its counters are unchanged in meaning.

## Revisit when

- The waste at the cut, measured as the room left after it, is a material share of the capacity on
  a real walk — then a second round over the keys past the cut, against the room left, is the
  measured next step.
- A traced frame's structures are no longer kept or dropped by whole instance (a partitioned top
  level, structures by cluster across frames), which changes what an item is.
- Something other than screen size is shown to predict what a viewer misses better (a shadow's
  footprint for casters, a light's reach).
