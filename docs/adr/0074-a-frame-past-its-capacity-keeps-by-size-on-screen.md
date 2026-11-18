# ADR-0074: A frame past its chain's capacity keeps by size on screen, and a tie is never broken

- **Status:** Proposed
- **Date:** 2026-11-18
- **Plan references:** docs/plan/04-renderer.md §4.4 (ray tracing: the budget manager's "priority by screen contribution"), [ADR-0025](0025-cluster-acceleration-structures.md) (cluster acceleration structures), [ADR-0068](0068-the-traced-chain-holds-what-frames-asked-for.md) (the traced chain holds what frames asked for)
- **Docs touched:** [renderer](../subsystems/renderer.md) ("Past the capacity, by size on screen", and a pointer from "The ray tracing chain's memory"), [gfx](../subsystems/gfx.md) (`gfx::rt_keep`), `docs/roadmap.md` (R174)

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

A review of the first design (2026-11-18) found the key and the prefix cut sound and four places
where "a function of the scene and the views, and of nothing else" did not hold as written: the
cache's misses it has no slot for, the capacity itself, histograms left for the next frame to
find clean, and a CPU mirror "to the bit" of arithmetic the GPU does not fix. The decisions below
take each in.

## Decision

1. **A frame past its capacity keeps by a key of the instance and the eye, never by a place in a
   list.** The key is the instance's size on screen — the squared distance from the eye to its
   mesh's sphere's centre against the square of the sphere's radius times its largest axis scale,
   as `asuint(d²) − asuint(r²)` quantized to 15 bits — then 16 bits of a hash of its placement
   (its cell relative to the eye's and its 3×4), with the item's kind placed so that under traced
   primaries every drawn structure precedes every caster, and otherwise the drawn precede the
   casters at one size. The eye is view 0's: a set's views share one.
2. **Each capacity is spent as a prefix of the keys.** The cut is the least key at which the
   structures keyed at or below it pass the capacity; everything below it is kept, everything else
   dropped. The frame's own structures and the cache's references are cut separately.
3. **A tie is never broken.** Items with equal keys are kept or dropped together.
4. **The rule has a CPU mirror the shader is held to**, `gfx::rt_keep` with `gfx::rt_keep_key`,
   as `geometry::lod_selects` mirrors the cull pass's LOD test. The key's centre is its own
   explicit fixed-order expansion, one `precise` operation a line (never `instance_point`, whose
   dots may be fused), and `rt_keep_key` mirrors that expansion and nothing else. **The sphere is
   the GPU's** (`mesh_bounds_main`, through `length()`, which Vulkan does not round correctly): the
   CPU takes it as an input and never recomputes it, and the GPU case reads it back from the device.
5. **The guarantee is stated at one capacity.** The capacity is an input: a growth lands when the
   worker finishes and the shrink windows are seconds, so a frame in flight cuts at a capacity its
   history chose. A frame somebody waits for is drawn at the budget's limit when it wants more, so
   its capacity is a function of the scene, the camera and the budget. No hysteresis and no change
   to the capacity policy.
6. **The guarantee is stated for a frame whose cache built every miss.** The cache's assignment
   places the misses it has no slot for in the frame's own set in the order they were appended,
   and a miss that set has no room for either draws nothing; which keys are misses is the cache's
   history. How many misses go unbuilt is a function of the kept set, the capacity and the cache's
   size, and a frame with any is outside this decision and counted (`RtStats`). Keying that
   overflow is its own roadmap row; bounding it inside this cut was weighed and not taken (it
   needs a count of distinct cache keys over the kept prefix, two passes over the cached pairs
   and a word a key, and on frames that fit as well).
7. **Every table the cut reads is cleared every frame**, by a pass the render graph is told about,
   and the keep block is imported naming its last frame's use, so no frame's cut depends on
   another frame having cleaned up.
8. **The layout's drop order goes**: `rt_drop_order`, its per-slot buffers, the first-fit walk and
   its one-thread fallback. Nothing else decides what a frame past its capacity keeps.

Code complies when no pass that decides what a frame past its capacity keeps reads an instance's
index, slot, block or position in any list except to address that instance's own data, and when
nothing it reads was left by an earlier frame. The cache's assignment of the misses it has no slot
for reads a place in the misses' list; it decides where a structure is built, which the picture
cannot see, until a miss goes unbuilt, and such a frame is the counted exception of decision 6.

## Consequences

- Two runs that hold the same placements in any order keep the same instances' structures at the
  same capacity, so their traced pictures and traced shadows are the same, wherever no cache miss
  went unbuilt; a scene moved by whole cells with its camera keys alike.
- What a frame at its budget shows changes: the instances it drops are the smallest on screen
  rather than the layout's last, so under traced primaries its holes are the smallest the capacity
  allows, and the farthest ring's casters going first becomes the same rule at a finer grain.
- The kept set moves with the camera at the cut, and when the eye crosses a cell every member of
  the crossing bin is picked again, since its hash is of the cell relative to the eye's. The pops
  are of the smallest instances on screen; a counter (read by no decision) says how many.
- A prefix gives up first-fit's use of the room the items at the cut leave — less than their own
  structures, which at a small named budget can be the whole capacity: one large near instance
  first in key order and past the capacity alone keeps nothing. A bounded second round over the
  key-sorted groups past the cut keeps the guarantee and is built when that is measured to matter.
- Every frame records a clear of the cut's tables and two more passes, empty when it fits (about 6
  to 7 µs); the chain holds 8 bytes a slot and 1 MiB more and 32 bytes a slot and the order's
  copies fewer; the CPU no longer lays an order out at every tail change.
- The capacity policy, its bounds and its counters are unchanged in meaning.

## Revisit when

- The waste at the cut, measured as the room left after it, is a material share of the capacity on
  a real walk — then the second round over the keys past the cut, against the room left, is the
  measured next step.
- Frames past their budget are found to overflow the cache too, so that decision 6's exception is
  more than a corner — then the overflow is keyed.
- The kept-state counter shows pops a viewer notices — then a stateless damping (the eye snapped
  to a coarse lattice for the key) is weighed, never a rule that reads the last frame's set.
- A traced frame's structures are no longer kept or dropped by whole instance (a partitioned top
  level, structures by cluster across frames), which changes what an item is.
- Something other than screen size is shown to predict what a viewer misses better (a shadow's
  footprint for casters, a light's reach).
