# R81: the traced structures' memory (2026-11-17)

Roadmap R81, with R62 beside it. Batch 67 measured a village walk at 3840×2160 under traced
primaries and shadows holding 3.7 GB of device memory before textures, **2.1 GB of it ray-tracing
structures** (R172's memory report), and the owner's rule is that every result is read for how it
scales down, memory first. This page says where those bytes are, by kind and by what asked for
them, on four scenes; what the published practice is for sharing, packing and choosing a traced
level; and what follows. The decision is
[ADR-0067](../adr/0067-the-traced-chain-holds-what-frames-asked-for.md); the rule that was built
is in [renderer](../subsystems/renderer.md#a-guess-is-not-a-demand).

## How it was measured

`engine-view --offscreen --width 3840 --height 2160 --raster rt --shadows rt --benchmark`, the
bounce on (`renderer.gi.mode=bounce`), each scene's own camera path, `--memory-report`, on the RTX
5090 (32,607 MiB), `msvc-release`. The memory report's `place` block splits `structures` by what
sizes each part since this row (`PlaceBytes::structure`): the same buffers it always summed. The
chain's own counters are the summary's `rt` block.

- **The village** (`content/test-scenes/village`, `--terrain-tiles`): a walk, 2,621 frames.
- **A static world** (`content/test-scenes/static-world` without its two fetched Khronos samples:
  a fresh worktree has no `content/samples` and this row downloads nothing; 64 instances of five
  kit meshes, no generator, no terrain).
- **The oasis** (`content/test-scenes/oasis-palms`): swaying palms over terrain, the flythrough,
  from batch 67's flight of the same main (the lease here ran out before it). The desert overlook
  needs the fetched samples too and was not flown; its chain is the table in
  [renderer](../subsystems/renderer.md#the-ray-tracing-chains-memory).
- **The streamed forest** (`content/test-scenes/streamed-forest`): three thousand trees through
  the world's four rings.

**The machine's state.** The Windows session was locked for every flight of this page, and
between twenty and thirty agents were building: other processes held 49 to 100 % of the CPU. So
every frame time here is an upper bound and is only ever read against the other half of its pair,
flown in the same lease. Bytes do not depend on either.

## Where the bytes are

MiB by kind. "Held" is main's policy, the first capacity kept; "given back" is this row's build
at the end of the same flight:

| part | sized by | village, held | village, given back | static world | streamed forest, held | streamed forest, given back |
|---|---|---:|---:|---:|---:|---:|
| the frame's own cluster sets (chunks) | the capacity × the driver's most a slot | 1,939.5 ¹ | 72.0 | 15.0 | 2,787 ² | 276.0 |
| records, record-to-entry, addresses, sizes | the capacity | 25.3 ¹ | 0.9 | 0.1 | 25 ² | 2.5 |
| bottom-level set | the capacity and the references | 61.0 ¹ | 15.2 | 0.4 | | 19.7 |
| scratch | the largest build | (with it) | 1.9 | 14.9 | | 59.9 |
| top level and its records | the instance slots | 4.8 | 4.8 | 0.0 | 5.0 | 5.0 |
| per instance and per pair | the instances, the pairs | 2.7 | 2.7 | 0.0 | 6.8 | 6.8 |
| the resident cache | its meshes' clusters twice, to its ceiling | 54.7 | 54.7 | 21.2 | 511.7 | 511.7 |
| rigid forms built once | the forms, packed | 0 | 0 | 0 | 3.2 | 3.2 |
| **structures** | | **2,088.0** | **152.3** | **51.7** | **3,446** ² | **884.9** |
| the scene's device total, by its own count | | 3,373.7 | 1,438.0 | 55.3 | | 1,367.7 |
| the process on the device (`gpu_memory_used_mib`) | | 4,017 | 2,135 | 735 | | 2,528 |

¹ Main's binary has no split: its total is its own (`place.structures_mib`), the chunks and the
tables are its capacity of 331,000 at 6,144 and 80 bytes, and the bottom level with the scratch
is what is left. ² The forest was flown with this row's build only, which gave the guess back
two frames into the measured flight: its bytes while it held it are `rt.peak_bytes` (3,613.6 MB)
and the capacity of 330,936 at 8,832 and 80 bytes; the last column is the end of that flight, at
32,768.

The oasis was flown by batch 67 with the same main (`<scratch>\b67\R172-3\lease-c2\o4-main`), and
not again here: its chain is 162 MiB (`rt.bytes`), 119 of it the frame's sets and 34 the cache.

**What the frames asked for**, a frame, over each flight:

| | village | static world | oasis | streamed forest |
|---|---:|---:|---:|---:|
| pairs a frame could build (the union, a view) | 439,533 | 1,780 | 20,298 | 1,490,944 |
| the estimate (less cached, static, untraced) | 392,643 | 0 | 17,425 | 1,084,364 |
| the budget as clusters (the limit) | 331,000 | 1,780 | 20,298 | 330,942 |
| **the capacity held** | **331,000** | 1,780 | 20,298 | **330,936** |
| structures a frame built itself, median / most | 2,793 / **7,161** | 0 / 0 | 2,505 / 5,527 | 14,157 / **50,122** |
| references to the cache, most | 8,337 | 596 | 429 | 1,638 |
| the cache: slots, occupied at the end | 9,193, 5,417 | 2,495, 930 | 5,747, 1,375 | 60,074, 3,045 |
| pairs of rigid forms built once | 0 | 0 | 0 | 398,035 |
| capacity over the most a frame built | **46×** | | 3.7× | **6.6×** |

(Batch 67's own run of the village walk, the one its 2.1 GB is from, held 330,125 for frames
that built at most 6,457: 51×. This worktree's village differs a little from it: a warning names
a sward mesh the fresh checkout could not read, on both sides of every pair.)

Three things are in those tables.

1. **Batch 67's 2.1 GB is a guess, held.** The village's chain is 2,088 MiB of which the frames'
   own structures need 44 (7,161 slots at 6,415 bytes). `first_rt_capacity` starts a scene too
   large to hold whole at every pair a frame could build, clamped to the budget; the policy then
   holds it for the shrink window (120 s since R152) and halves it once a window after that. The
   estimate counts every level of every DAG of every tile slot and every instance in and out of
   view, and cannot do better before a frame has been culled: it is 55 times the village's demand
   and 22 times the forest's. And the budget it is clamped to is a sixteenth of the card, so the
   number measured what the card is. This is the whole of what the batch saw, and stage 1 below
   takes it back.
2. **The cache's slots are allocated whole.** The streamed forest's cache is at its ceiling (506
   MiB of slots on this card, 256 on a card of 16 GB or less) for 3,045 occupied slots: a scene
   whose instances come and go keeps every mesh cacheable, 60,504 clusters twice over, and the
   slots are made at create. At the driver's reservation the occupied ones are 26 MiB. Once the
   guess is gone this is the largest part of the forest's chain (58 % at the end of the flight).
3. **Everything is at the driver's most.** 6,144 bytes a slot (8,832 where a scene's sets allow
   opacity micromaps), whatever the cluster holds. The forest's rigid forms, packed since R143,
   occupy 41 % of their reservation (3,366,828 of 8,112,128 bytes) and v40's occupied 18 %; a
   static world whose frames build nothing holds 15 MiB of frame set and 15 of scratch for its
   1,780 pairs, because a chain that fits its whole union is given it.

## By instance class, and what is built more than once

- **Rigid at rest and rigid whose cut changes** are the same class to the chain: a rigid
  instance's clusters are in the resident cache, built once whichever instance or frame draws
  them, and a change of its cut changes which it references. What each instance still owns is its
  bottom-level structure over those references: 160 to 308 bytes a reference by the driver's
  slope here, and the whole bottom-level set is 0.4 MiB on the static world, 15.2 on the village
  (3,488 instances' structures a frame, most of them a few clusters) and 19.7 on the forest: **one
  to two percent of the chain while the guess is held, and fifteen to twenty megabytes in any
  case** (a tenth of the village's chain once the guess is gone). The same triangles are built
  twice in one case only: a cluster's drawn form and its caster form are two slots (the
  caster is built non-opaque so that primary rays pass it).
- **Far forms** are built once a mesh, packed, and instanced: 398,035 pairs of the forest's rings
  cost 3.2 MiB.
- **Deformed instances** are what a frame builds: every (instance, cluster) pair of the cut, every
  frame. In the streamed forest's inner ring 107 swaying trees of four meshes hold 118,401 pairs
  over 4,038 distinct clusters, **twenty-nine pairs a distinct cluster**, and a frame builds up to
  50,122 structures, theirs and the ground's, because each instance's sway moves its vertices
  differently. This is the only place the same source triangles are built many times, and they
  are not the same triangles once deformed. What takes it down is already policy and not sharing: a far field at
  rest references the cache (R152), and a swaying mesh whose sway is under
  `renderer.forms.still_px` stands still.
- **The ground** (a world's tile slots, the terrain stage's) is built by the frame too, each tile
  its own geometry: nothing to share. It is the village's 7,161.

So R81 as the roadmap wrote it, one bottom-level structure a mesh shared by its rigid instances,
would take back some of those fifteen to twenty megabytes: R101's cache and R94's forms did the sharing that had
bytes in it, and R143 reached the same answer from the forest's stones.

## What is published

Read on 2026-11-17, each at the address given; a quotation is the page's own words.

- **NVIDIA's `vk_lod_clusters` sample** (github.com/nvpro-samples/vk_lod_clusters: `README.md`,
  `docs/blas_sharing.md`, `docs/blas_merging.md`, `docs/blas_caching.md`), the reference use of
  the cluster extension with a continuous LOD. *Sharing*: "BLAS Sharing enables the re-use of a
  BLAS from a higher detailed instance for other instance's that can safely use it", chosen so
  "that an instance doesn't become 'under-detailed'"; its results: frame time "from `3.9` to
  `1.6` milliseconds", "BLAS memory from `62 MB` to `9 MB`", and the price, "the shared BLAS will
  cause objects afar to use more detail than necessary", which "can be mitigated by introducing
  more than just one shared BLAS for different LoD levels". *Caching*: "the cached BLAS is based
  on a single discrete LoD level". *Merging*: one structure for the instances that would have had
  their own, "based on the highest available detail that is resident". The rasterizer there keeps
  its own cut ("Ray tracing will render more clusters even with culling than raster"), so the
  traced level is not the rasterized one. *Allocation*: "We are leveraging a sparse buffer for the
  CLAS allocation, so we can grow it on demand. This is crucial as the host-side estimates are way
  too conservative (e.g. often 3-5x bigger than actual)", and new cluster structures "are built
  into scratch space first, so the allocation logic can use their accurate build sizes".
- **NVIDIA, "Best practices: using NVIDIA RTX ray tracing"**
  (developer.nvidia.com/blog/best-practices-using-nvidia-rtx-ray-tracing): "Instantiate BLASes
  when possible"; compaction's "reduction in memory consumption depends on the geometries but can
  be up to about 50%" and "it is most practical to do for BLASes that are only built one time";
  "In hybrid rendering, using the same LOD for rasterization and ray tracing can be considered.
  It's an efficient way to avoid self-intersection artifacts such as surface shadowing itself",
  and where the levels do not match "enabling back face culling is often needed in ray tracing to
  prevent the self-intersections".
- **The cluster extension's operations** (docs.vulkan.org, the reference page of
  `VkClusterAccelerationStructureOpTypeNV`): `MOVE_OBJECTS` moves a cluster structure, a template
  or a cluster bottom-level structure; in the size mode it returns the size of existing cluster
  structures; moving a cluster structure means rebuilding the bottom-level structures that hold
  it. (The page's sentences were read through a summary that shortened them; the three facts are
  as it gave them and no number rests on them.)
- **Unreal's Nanite** (dev.epicgames.com, "Nanite Virtualized Geometry"): under ray tracing "the
  Fallback Mesh is used for Nanite-enabled meshes by default"; `r.RayTracing.Nanite.Mode 1` is
  "(Experimental) Initial support for native ray-tracing of Nanite meshes", which "preserves all
  detail while using significantly less GPU memory than zero-error Fallback Meshes".
- **This engine's plan** (`docs/plan/04-renderer.md` §4.4) had the same fallback: "per-mesh static
  BLAS at 2–3 fixed LODs chosen by distance; accept the LOD mismatch and mitigate with ray bias
  and normal-offset tricks", and R94 measured the mismatch as shadow shards on the picture's own
  receivers and declined it.

What the sources agree on: share by instancing where the geometry is the same; a shared
structure is only ever *finer* than an instance needs, never coarser, unless self-shadowing is
fought with biases and back-face culling; a structure kept across frames is compacted or packed
to its built size, and reservations are several times what is built.

## What follows

[ADR-0067](../adr/0067-the-traced-chain-holds-what-frames-asked-for.md) has the decision. In
stages, by the bytes each holds:

1. **A guess is given back** (built, below).
2. **The cache holds what it has built.** Slots made in chunks as they are occupied, to the same
   ceiling, where today the ceiling is allocated at create: the forest's 506 MiB becomes the 26
   its 3,045 occupied slots reserve. The cache already builds with explicit destinations, so a
   slot's address is arithmetic over chunks as a frame record's is (R152).
3. **Slots by the cluster's own triangle count.** T129 measured the driver's reservation as
   linear in the triangles a set allows (8,832 bytes at 124, 4,736 at 64, 2,560 at 32, with
   micromaps); a cache of classes with their own stride needs no move and no variable allocator.
   Then, where it still pays, the built size by `MOVE_OBJECTS`.
4. **The frame's own set by the same classes** (T138's design, in renderer.md): this is the part
   a card with little memory fills when the forest is in view.
5. **The floors on small cards**, with the governor of ADR-0063: the owner's question below.

Not proposed: a bottom-level structure shared across differing cuts (fifteen to twenty megabytes
here, against the invariant), and a coarser traced level for anything a pixel shows.

## Stage 1: the pair

The rule is in [renderer](../subsystems/renderer.md#a-guess-is-not-a-demand): a first capacity
under the union is provisional and is given back after ten seconds of frames when they wanted
less than half of it. The village walk at 3840×2160, `--raster rt --shadows rt`, the bounce, main's
release build copied out of the tree (`<scratch>\b67\bin-main`) against this row's, alternating in
one lease on 2026-11-17 from 23:13; **the session was locked for all of it** and other processes
held half the CPU and more, so the milliseconds are upper bounds and only their differences are
read:

| run | structures | scene's device total | process on the device | capacity at the end | frame, median | GPU frame | of it `rt` | `clas` |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| main | 1,111.4 MiB ¹ | 2,397.1 | 3,047 | 167,936 ¹ | 6.143 ms | 5.956 ms | 0.403 | 0.143 |
| this row | **152.3 MiB** | **1,438.0** | **2,135** | 12,288 | 6.130 | 5.947 | 0.382 | 0.123 |
| main again | **2,088.0 MiB** | **3,373.7** | **4,017** | 331,000 | 6.147 | 5.965 | 0.404 | 0.145 |

¹ Main's first run cooked nothing but was slow to load beside other work, 112 s where the second
took 81, so its shrink window closed inside the flight and it halved once: what main holds two
minutes in. The second run is what batch 67 measured, the guess whole.

**1,936 MiB of structures given back, 1,882 MiB of the process's device memory, and no frame
time**: the GPU frame is 0.01 to 0.02 ms under main's, inside the noise of a locked card, with the
`clas` zone 0.02 ms shorter (one chunk to build into where the guess had six). No frame of this
row's flight built less than it wanted (`overflow_frames` 0, no instance dropped): the give-back
went to 8,192 and one growth took it to 12,288 as the walk's demand rose to 7,161.

**The streamed forest** was flown once, with an earlier build of this row (the first table): the
guess (330,936 clusters, 2.8 GB of chunks) went back two frames into the measured flight, to
12,288, and no frame of the 9,003 built less than it wanted. That flight found a fault, since
mended and held by a case: the chain's first growth afterwards went to 56,466 clusters for a
demand of 10,119, because a streamed world's chain is made for its prefix and made again when the
first tiles arrive, and the prefix's capacity was remembered as a peak though no frame had been
drawn; a capacity held before any frame is no longer remembered. **The forest was not flown again
after the mend** (the GPU's line was sixteen deep), so its last column is that earlier build's.

## What a card with 8 GB and with 4 GB holds

The chain's ceilings come from the card (`rt_capacity.h`): the budget is a sixteenth, never under
512 MiB, scaled by the micromaps' slot growth before the clamp; the cache's ceiling a sixty-fourth,
never under 256 MiB. A card with 8 GB and one with 4 GB therefore have the same two floors, an
eighth and a sixteenth of the smaller one.

**The village walk, flown** with `--rt-budget-mib 512`, the budget either card would have, this
row's build: while the guess stood the chain was 652 MiB (`rt.peak_bytes`, 83,689 clusters: the
whole budget); given back it is **the same 152.3 MiB the 5090 holds**, 211 MiB by `rt.bytes` in
both, and the process holds 2,135 MiB on the device in all at 3840×2160 (the scene's own count
1,438: 600 of tile arenas, 449 sized by the frame, 152 of structures, 145 of textures, 61 and 31
of companions and materials). Main on such a card holds the 652 for two minutes, 400 for the two
after, and so on down.

**The streamed forest, by the rules** (not flown at that budget): with micromaps the budget is
736 MiB on 8 GB and the 512 floor on 4 GB, 84,000 and 58,600 clusters at 9,160 bytes; the flight
wants 6,024 to 50,122, so both hold every frame of it, and the chain follows the demand between
53 and 438 MiB of slots. Beside it the cache allocates its ceiling whole, 256 MiB on either card
for the 26 its occupied slots reserve: after stage 1 that is the first thing a small card holds
for nothing, and it is stage 2.

**What does not follow the card**: the scratch (60 MiB on the forest), the micromaps' own ceiling
(1,024 MiB; the forest test scene's are 89), and the tile arenas and frame-sized tables that are
other rows'.

## What was not measured, and what went wrong

- The census by kind and ring (`--census` with `renderer.rt.census_log`) logs at a level the
  flights did not show, and on the streamed forest it took 21 minutes, under the GPU lock, of a
  lease asked for as ten. The by-class reading above is from the chain's own counters instead.
- The desert overlook and the full static world need fetched samples; the forest proof of
  concept's staging was not loaded (the brief's four scenes are the engine's own).
- What the built structures occupy inside their slots was not read back from the device: the
  ratios quoted are the packed forms' (measured at their build) and the sample's.
- No walk was flown that turns from a sparse view to a dense one inside the give-back's first
  seconds, which is the one place a frame in flight can now be short where it was not.
