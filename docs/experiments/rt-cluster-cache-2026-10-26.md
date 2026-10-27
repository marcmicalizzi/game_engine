# The resident cluster structure cache (R101, 2026-10-26)

The ray tracing chain built a cluster acceleration structure for every visible (instance, cluster)
pair, every frame. The forest proof of concept's expanded forest at 11520×2160 under traced
shadows wanted 222,455 of them a frame against the 1 GiB budget's 166,937, dropped instances on
101 of 101 frames and spent 33.9 ms of its 98.5 ms frame in the chain (`reports/quality-v15/
engine-c1ee971d/HANDOFF.md`, run `expanded-forest-rt-wide-20261026-190424`, on a shared machine:
upper bounds). Those structures are copies: a forest is a few dozen meshes placed tens of
thousands of times. R101 builds a rigid mesh's cluster structures once, the first frame any
instance draws them, into a scene-sized cache every instance and frame references; the mechanism,
the budgets and the rule for a hit are in [renderer](../subsystems/renderer.md#the-resident-cluster-structure-cache).

## What the tests measured

On the rigid procedural grid (`rt_capacity_tests.cpp`, RTX 5090, `msvc-debug`): one mesh of 717
clusters, nine instances; a flight over four distances builds 288 structures into the cache (288
of 1,435 slots), referenced 56 times by the last frame, and **a second flight over the same cuts
builds none**; colour and ids are the per-frame build's bytes on the mesh path with traced shadows
and on the ray path. A cache of 199 slots — the near cut alone occupies 198, the far one 52,
both 214 — evicts 16 slots at each move between them, never overflows, and draws the per-frame
build's bytes. A rigid bar between two skinned ones: the chain builds the two skinned bars' 2
structures from the pool and references the rigid one's 1 cached structure, where the per-frame
chain built 3; the same bytes on the mesh and ray paths.

## Inputs and method

The proof of concept's frozen v14 scenes as its v15 report on c1ee971d ran them, read where they
are (`runs/expanded-forest-rt-wide-20261026-190424/scene.json`,
`runs/grass-clearing-rt-wide-20261026-190351/scene.json`, the `expanded-forest-path.json` and
`grass-clearing-path.json` camera paths) with the run's own arguments — `--terrain-tiles
--terrain-far 6 --time-rate 0 --world-rate 1 --no-lights --time-of-day 15 --lod 1
--deform-pool-mib 64 --rt-budget-mib 1024 --tunable renderer.flora.motion=true --tunable
renderer.flora.wind_strength=0.7 --raster hw --shadows rt --frames 600 --warmup 0 --repeat 1` —
`--offscreen` in place of the window and `--ddc` a copy of the run's DDC; and the desert overlook
(`content/test-scenes/desert-overlook/`) at 1920×1080 with `--shadows rt`, three flights. Before:
5552a2b3 (main), built in a worktree of its own; after: the R101 branch. Both `msvc-release`,
`--wait-quiet 60`, every run's device under the machine-wide GPU lock
(`ENGINE_GPU_LOCK_ON_DEVICE=1`). The machine was shared with other agents' builds: at the runs'
starts the GPU was 1–4% busy (but for the clearing at 1600×900 before, 100%: another job's tail)
and other processes held 4.5–38.2% of the CPU; at the ends up to 100% of the GPU (the run's own
frames) and 6.5–90.3% of the CPU. By the harness's rule every number is an upper bound.

## Before and after

GPU milliseconds are medians over the flight; "wanted" the frame's own structures at its peak, and
after R101 the cached references beside them.

| scene | before: frame / chain / CLAS / resolve | after: frame / chain / CLAS / resolve | wanted before → after (+ cached references) | drops (frames; instance events) | chain bytes |
|---|---|---|---|---|---|
| forest 11520×2160 | 97.06 / 33.70 / 3.46 / 36.98 | 95.93 / 34.89 / 3.46 / 36.93 | 222,455 → 222,435 (+20) | 619; 132,466 → 619; 136,180 | 1,658 → 1,925 MB |
| forest 1600×900 | 12.49 / 3.15 / 1.81 / 3.28 | 12.70 / 3.36 / 1.81 / 3.28 | 88,589 → 88,584 (+5) | none | 1,498 → 1,764 MB |
| forest 1600×900, forms cut by distance | 38.77 / 33.07 / 3.40 / 3.18 | **9.31 / 3.42** / 1.80 / 3.36 | 190,734 → 88,584 (+102,150) | 644; 2,939,216 → **none** | 1,340 → 1,633 MB |
| clearing 11520×2160 | 67.68 / 4.02 / 2.24 / 28.76 | 67.81 / 4.10 / 2.23 / 28.94 | 98,329 → 97,996 (+333) | none | 1,678 → 1,944 MB |
| clearing 1600×900 | 21.26 / 4.71 / 0.91 / 1.61 (GPU busy at start) | 11.21 / 2.44 / 0.90 / 1.69 | 43,633 → 43,436 (+197) | none | 837 → 1,103 MB |
| desert overlook 1920×1080 | 0.54 / 0.35 / 0.13 / 0.15 | **0.42 / 0.24 / 0.00** / 0.15 | 6,167 → 0 (+6,167) | none | 97 → 342 MB |

The overlook's p95 went 0.67 → 0.52 ms (frame) and 0.41 → 0.25 ms (chain). The "after" forest at
11520×2160 is the build that keeps each instance's cached mark in its count words; the first
build read each instance's record on the drop rule's one thread and measured a 62.9 ms chain
there, which is why it does not.

**The cache, after.** 43,690 slots of 6,144 bytes (the 256 MiB ceiling; the scene's 137 cached
meshes hold 487,852 clusters, twice which is past it), 266 MB with its tables, on the forest and
the clearing; 262 MB on the overlook. Occupied at the end: the forest 18, the clearing 331, the
forest with distance-cut forms 8,527, the overlook 11,867. No eviction and no miss without room in
any run. The reference capacity settled at 4,096 (8,192 on the overlook, 131,072 on the
distance-cut forest), at 220 bytes a reference under a 1,220,161-reference ceiling.

**Warm-up.** The forest with distance-cut forms built all 8,527 of its structures before its
first timed frame (the load's first frame) and none after: frames to steady, zero. The overlook's
first flight built 9,378 over 1,559 of its 2,401 frames as new clusters entered the cut — at most
68 in a frame, against the 6,167 the frame used to build every frame — and the second and third
flights built none; 2,489 more were built by the frames before the first timed one. The cache's
memory is allocated at create, so none of it is a reallocation in a flight.

## What it shows

**The expanded forest's chain is its ground.** Its 222,455 structures a frame are terrain levels
— the world tiles and the far levels, which the pool's terrain stage deforms every frame (224,591
pool entries a frame in the v15 report) — and its trees were already R94's static forms
(35,448 instances over 171 forms). The cached instances reference 20 clusters a frame. Its 34 ms
chain is `ranges_main`'s one-thread drop rule walking 55,442 instances, every frame, because the
ground alone wants more than the 1 GiB budget holds; the CLAS build itself is 3.5 ms. What would
move it is not R101: a per-pair cache for deformed instances whose deformation stands still (the
ground at `--time-rate 0`, a tile's heights once the pool has posed them), or a drop rule that is
not one thread — both written down as the next steps, not built here.

**Where the cut is rigid copies, the cache is what R94 was and more.** With the forms cut by
distance the frame stops building any of them, keeps every instance, and is 4.2 times faster at
1600×900 than the per-frame build of the same cut — without R94's fixed frontier, so the far trees
draw at their own distance's cut. The overlook, all rigid instances of a few meshes, builds no
structure of its own at all.

**What it costs is memory, up front.** 256 MiB of slots at the driver's worst case per cluster on
any scene with a rigid mesh, needed or not. Sizing the slots by demand (grown, like the frame's
capacity, on a trend), or packing them at the size each structure has, would take that back.
## 2026-10-27: the merge's frame budgets, and the default

Batch 42's gate (the frame budgets of ADR-0018, `tools/frame-budget.ps1` on the release build at
ed92e9bb) found one row over: `endless-surround3-rt`'s `gpu_ms.rt`, the records chain plus the
CLAS, BLAS and TLAS builds plus the cache's passes. The scene caches nothing — its one authored mesh
is a placeholder of one cluster, and every ring slot deforms — so the cache's cost there is the
cost of having one. The three runs below are the same scene, path, resolution and flags, on the
same binary but for the tunable; the machine was not quiet (others used about a third of the GPU,
8–17 % of the CPU, the same state as the batch 41 run the budget was last met in), so every figure
is an upper bound, and the two cache-on runs agreeing to 0.001 ms is what makes the difference
readable.

| run | `rt` median / p99 (ms) | `blas` | `clas` | `resolve` | frame median | cache slots | references |
|---|---|---|---|---|---|---|---|
| batch 41 (before R101), 2026-10-26 18:10 | 0.971 / 1.396 | 0.270 | 0.325 | 1.935 | 4.236 | — | — |
| batch 42, cache on (default as merged), 2026-10-27 08:56 | 1.151 / 1.666 | 0.266 | 0.322 | 1.953 | 4.394 | 3 | 0 |
| batch 42, cache on, the quiet re-fly, 09:02 | 1.150 / 1.629 | 0.266 | 0.321 | 1.953 | 4.393 | 3 | 0 |
| batch 42, `renderer.rt.cache=false`, 09:07 | 1.064 / 1.595 | 0.264 | 0.324 | 1.951 | 4.308 | 0 | 0 |

The budget's `rt` median is 0.951 ms with a 1.096 ms limit. Half of the 0.18 ms is the cache's
own passes with nothing to do (scan, threshold, evict, assign, references and the empty build, six
graph passes a frame); the other half is in the records chain with the cache off, which is the
`ranges_main` and `emit_main` this change restructured: the one-thread instance loop now keeps two
prefix sums, and the endless desert has tens of thousands of ring-slot instances for it to walk.
The two erg runs moved the same way by less (`erg-walk-1080p-rt` 0.366 → 0.382 ms, surround
0.606 → 0.623), and the table above shows the forest's chain 33.70 → 34.89 ms and the clearing's
4.02 → 4.10 before this was measured.

**Decision (the merge, 2026-10-27).** `renderer.rt.cache` defaults to **false** until a cache that
references nothing costs nothing and the instance loop is parallel (roadmap R106); the cache, its
tests (which turn it on for themselves) and its counters stay, and `renderer.rt.cache=true` is what
a rigid scene — the overlook, the forest with its forms cut by distance — asks for meanwhile. The
default was chosen over a rebaseline because the budget exists to refuse exactly this: a cost on
every scene for a gain on some.
