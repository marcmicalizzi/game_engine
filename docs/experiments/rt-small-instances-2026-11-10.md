# What many small instances cost the ray tracing chain (R143, 2026-11-10)

Roadmap R143. The forest proof of concept's v40 world added 28,000 small rigid instances — 15,866
wall stones of five meshes (432 triangles and 17 to 20 clusters each), 10,089 cobbles of six (1,728
triangles, 42 to 69 clusters), talus — and both its capture runs at 1920×1080 under
`--rt-budget-mib 1024` logged `the ray tracing chain dropped instances' structures wanted=124490
built=117271 capacity=117271 limit=117271 instances=1491`, where v39 had dropped none. Its
maintainer read that as "a structure per instance does not survive kit-built content". The owner's
v38 walk at 3840×2160 had logged `wanted=86971 built=86016 capacity=338072 limit=338072
instances=320`, a capacity far over what was built. This page is what was measured about both, what
was changed, and what was not.

Everything here is the RTX 5090 (32,404 MiB of heaps, driver 610-series), `msvc-release`,
`engine-view --offscreen` under the GPU lock, on the v40 world with the proof of concept's
engine-only flags (`--terrain-tiles --terrain-far 6 --time-of-day 15 --lod 1 --deform-pool-mib 64
--rt-budget-mib 1024 --shadows rt`, the flora's motion on, micromaps on, the bounce on). **The
machine was never quiet**: six renderer rows, a merge gate and the solver rows shared it, and every
run records other processes on 30 to 50% of the CPU and the GPU's memory 21 to 23 GB full. So a
single run's milliseconds are upper bounds; a cost is a pair of runs taken back to back in one
lease, before and after, from two executables copied out of the tree (the base is 76d96ced).

## Which bound, and what filled it

**The 117,271 is the budget, in cluster structures of the frame's own set.** `--rt-budget-mib 1024`
over `rt_bytes_per_cluster()`, which with opacity micromaps is 9,156 bytes on this card (8,832 of
structure reserved at the driver's most for a cluster of 124 triangles and 64 vertices, the rest
its record, address, size and share of the bottom-level set): 1,073,741,824 / 9,156 = 117,271. It
counts cluster structures the frame builds itself — one per visible cluster of a **deforming**
instance — not instances, not references to the resident cache, not the rigid forms' structures
built once, and not bytes. The proof of concept's launcher names that budget; with none named the
device's rule gives this card 2,952 MiB and 338,072 (the owner's v38 log), so the owner's walk and
the proof of concept's captures ran under different budgets.

**The frame that dropped 1,491 is reproduced on main**: the capture path's fifteen views at one
frame a second, `--raster hw`, frame 13 (`v40-flood-bed`): `wanted=124428 built=117271`, 1,491
instances. The census of that frame's visible list, by mesh (`--census`):

| what | visible pairs | how the chain holds it |
|---|---:|---|
| rigid distance forms (rings 1 to 3: 4,698 forms) | 2,586,326 | one structure a mesh, built once (static forms) |
| trees of the inner ring | 67,292 | sway: the frame's own structures, every frame |
| understory of the inner ring | 46,603 | sway: the frame's own |
| sward of the inner ring | 46,724 | untraced under a rasterized picture; the frame's own under the ray path |
| far forest | 16,180 | — |
| roots and deadwood | 13,740 | rigid: references to the resident cache |
| **wall stones** | **6,445** | rigid: references to the resident cache |
| **cobbles** | **475** | rigid: references to the resident cache |
| other rock | 287 | rigid: references |

The frame's own 124,428 are the swaying trees and understory of the inner ring and their casters;
the same frame referenced 62,311 cached structures and built 33,940 into the cache (it is a camera
cut). **The stones are 6,920 visible pairs of 2.79 million and none of the 124,428**: they are
rigid, so the inner ring's are cached instances and the rest are static forms' instances. v39 "on
the same flags" was other views; this view's undergrowth is what passed the budget, by 6%.

**What that drop cost the picture**: under `--raster hw` the picture is the rasterizer's, so the
1,491 instances — the outer of the two rings whose plants sway, 80 to 190 m away — lost their
*shadows*, not their surfaces; the line's "instances" are instances whose drawn clusters had no
structure. Under the ray path the same line is 1,491 holes.

**The owner's 320 at a capacity of 338,072**: the line printed the capacity the scene held when the
frame was read back, two frames after it was recorded. 86,016 is 21 steps of 4,096: the frame was
built against a capacity of 86,016 while the growth to the budget was in flight (the walk's cache
had overflowed by tens of thousands of misses at a turn, which the frame's own set has to build:
R114), so the bound was the capacity, not the budget.

## What a small rigid instance costs

Per frame, on main, a wall stone in the inner ring (80 m): no cluster structure once its mesh's
clusters are resident (five meshes, about 90 clusters, in a cache of 60,074 slots); one reference
(eight bytes, and a key look-up in the emit pass) per cluster in its cut, drawn or caster; one
bottom-level structure in the frame's one build; one top-level instance. Past the inner ring it is
a static form's instance: one top-level instance referencing its mesh's structure, built once (a
ring-1 stone's form is 7 to 10 clusters, a ring-2 one's a single cluster).

**And one record in a bottom-level build that was handed a record for every instance slot.** The
`blas` zone along a walk on main at 1920×1080 (2,041 frames, the wall view held, a walk across the
wall and a run into the forest):

| frame | the frame's own structures | cached references | `blas` (ms) | `clas` (ms) | `rt` (ms) |
|---:|---:|---:|---:|---:|---:|
| 60 (the wall, held) | 90,360 | 19,879 | 2.18 | 1.89 | 4.91 |
| 540 (crossing the wall) | 101,310 | 22,455 | 2.19 | 2.09 | 5.16 |
| 780 | 50,586 | 39,498 | 2.14 | 1.02 | 4.80 |
| 1020 | 50,697 | 37,856 | 2.15 | 1.06 | 4.09 |
| 1860 | 103,192 | 31,345 | 2.19 | 2.11 | 5.20 |
| median of the run | 73,581 | 31,213 | 2.17 | 1.50 | — |

`blas` does not follow what the frame builds or references; it follows the 219,166 instance slots
the build is handed, about 10 ns each — as v18's 70,000 slots at 0.75 ms and v24's at 1.46 ms did
([the cluster cache's page](rt-cluster-cache-2026-10-26.md), [the overflow's](cluster-cache-overflow-2026-11-01.md)).
128,369 of the slots are static forms' instances, whose record is empty every frame. That is what
28,000 more instances cost the chain: 0.28 ms a frame of a build that looked at every record.

## What changed

1. **The bottom-level build is over the instances that reference anything** (`blas_compact_main`,
   [renderer](../subsystems/renderer.md#the-bottom-level-build-is-over-what-the-frame-drew)).
2. **The rigid forms' cluster structures are packed to the size they are**
   (`gfx::create_packed_cluster_set`).
3. **On a traced picture a frame past its capacity keeps every drawn structure before any caster.**
4. **The warning names its bound**, in cluster structures, against the capacity the frame was
   recorded with, once per bound.

### The pairs, before and after

RESULTS-TABLE

### The rigid forms at the size they are

STATIC-TABLE

## What a structure of the frame's own set reserves

The frame's own set is built with implicit destinations, so every structure is reserved at the
driver's most for a cluster of the set's limits: 124 triangles and 64 vertices, 8,832 bytes with
micromaps. A forest's clusters are not that: `birch-0` is 217,742 triangles in 7,730 clusters (28 a
cluster), `spruce-0` 21,950 in 533 (41), a sward patch 960 in 71 (14) — leaves are cards, four
vertices to two triangles, so a cluster is full at 64 vertices and about 30 triangles. The size
query takes the build's total triangles and vertices as well as its cluster count
(`ClusterSetLimits::max_total_triangles`, `max_total_vertices`, new and unused by the renderer);
`cluster set: what a set reserves follows its totals` prints what the driver does with them:

TOTALS-TABLE

## The pair space, by category

The visibility id names 2^24 (instance, cluster) pairs, and the cap is the owner's. v40 at these
settings holds 16.77 million in every frame recorded (`pairs` 16,754,309 to 16,776,661 of
16,777,216 at 1920×1080, 441 "the pair budget withholds tiles' placements" lines in a 34 s walk,
every one naming ring 3; the owner's 3840×2160 walks name ring 2). An account of the scene file's
own instances at the wall view, from the file and the loaded meshes' cluster counts (each instance
at the form its tile's ring draws; tile rings of 1.25, 3, 10 and 20 tiles of 64 m):

| category | ring 0 (full mesh) | ring 1 | ring 2 | ring 3 | pairs | share of 2^24 |
|---|---:|---:|---:|---:|---:|---:|
| trees | 44 instances, 151,326 | 760, 941,458 | 9,622, 2,048,007 | 26,499, 479,468 | 3,620,259 | 21.6% |
| understory | 444, 53,780 | 4,436, 219,769 | 50,178, 797,269 | 148,290, 9,601 | 1,080,419 | 6.4% |
| **wall stones** | 4,222, 77,918 | 4,457, 37,826 | — | 6,914, 1,058 | **116,802** | **0.70%** |
| roots and deadwood | 13, 1,658 | 113, 3,193 | 1,123, 5,055 | 1,978, 35,278 | 45,184 | 0.27% |
| other rock | 21, 6,049 | 51, 2,985 | 628, 5,569 | 711, 512 | 15,115 | 0.09% |
| **cobbles** | — | 232, 2,442 | 2,456, 3,336 | 1,979, 241 | **6,019** | **0.04%** |
| the file's instances | | | | | 4,883,798 | 29.1% |
| holes in the tail (`hole_pairs`) | | | | | 685,156 | 4.1% |
| the prefix (`pairs` of the scene read whole: the ground's slots, the water) | | | | | 286,045 | 1.7% |
| **the placement generators' tiles: the sward, the scatter** (the remainder) | | | | | about 10.9 million | **65%** |

The stones and cobbles are under 1% of the pair space, and a stone's ring-0 mesh is 17 to 20 pairs
for 432 triangles: its DAG has that many clusters because the rock generator's stones are built in
small clusters, which a merge by tile would fold away (the solver handoff's "merged, with a far
form"), but nothing in that returns ring 2. **What holds the pair space is the sward**: a patch is
71 to 143 clusters at 960 to 1,920 triangles — 14 triangles a cluster, where the format holds 124 —
and the generators' tiles are two thirds of the cap. PAIRS-4K

## Published practice

NVIDIA's `vk_lod_clusters` sample is the published reference for ray-traced cluster level of detail
with instances on this extension. Its per-frame path is this renderer's — traversal, a bottom-level
build per instance from cluster references, a top-level build — and its remedies for instancing
are three: **BLAS sharing** (one provider instance's structure used by every instance of the
geometry whose level range allows it, with the stated price that "the shared BLAS will cause
objects afar to use more detail than necessary", 3.9 to 1.6 ms and 62 to 9 MB of bottom-level
memory on its example), **BLAS caching** (one discrete-level structure a geometry, reused across
frames, for the far levels), and **BLAS merging** (one structure a geometry from the highest
resident detail). All three give an instance a traced level other than its rasterized one; the
sample accepts that, and its README notes that ray tracing "will render more clusters even with
culling than raster". Its cluster structures are persistent — built when a group streams in, moved
once to a persistent allocation — and it reports host-side size estimates "often 3-5x bigger than
actual", which is the reservation this page's packing removes for the rigid forms. Unreal's Nanite
traces a per-mesh fallback proxy instead of the rasterized clusters, and hides the mismatch with
screen-space traces and bias. Here the traced cut is the rasterized cut word for word, and the two
levels of sharing that hold to that already exist: the static forms are the sample's cached
far-level structure with the raster side fixed to the same cut, and the resident cluster cache is
its persistent cluster structures. Sharing a whole structure between inner-ring instances at one
cut was not built ([renderer](../subsystems/renderer.md#the-bottom-level-build-is-over-what-the-frame-drew) says why).

Sources: nvpro-samples/vk_lod_clusters, README and `docs/blas_sharing.md`, `docs/blas_caching.md`,
`docs/blas_merging.md`, `docs/clas_allocation.md`, `docs/streaming.md` (read 2026-11-10);
VK_NV_cluster_acceleration_structure.
