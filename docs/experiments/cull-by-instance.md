# R173 and R16: the cull's cost by the pair, and instances culled first

*2026-11-17, batch 68. The RTX 5090 of the development machine; every flight offscreen at 3840 × 2160 from release builds copied out of any checkout. The machine's processors were 100 % busy with other agents' builds throughout (the harness's `machine_state`: upper bounds for every CPU number; the GPU was otherwise idle and the lock ours). Scripts, scenes and logs: the row's scratch (`b68\R173\`), not committed.*

## The question

The cull runs one thread per (instance, cluster) pair of the whole pair space, holes included ([renderer](../subsystems/renderer.md#instances-that-come-and-go)). Each thread finds its pair's instance — a binary search over the instances' `first_pair`, or since W6 one load from the 8-byte pair table behind `renderer.cull.pair_table` — reads the instance, its mesh and the cluster, and tests. Batch 66 found the table **slower** than the search on one budget flown with the session locked (+0.04 ms on the camera's culls at 1080p) and left it off; batch 67's sweep measured the forest proof of concept's culls at about 1 ms a pass at 16.4 M pairs. R173 asked whether a 4-byte instance per pair or a hole marked in the table wins in every state, and R16 whether culling instances before their pairs beats both.

## The scenes

The engine's scenes stop at 1.4 M pairs, so the pair counts that matter were built: a scratch glTF of one rock (2,304 triangles, 51 pairs an instance with its LOD DAG, about the forest's 35) placed on a 4 m grid of 141², 397² and 573² (19,880 / 157,608 / 328,328 instances: **1.01 M, 8.04 M and 16.74 M pairs**, the last 0.2 % under the cap), seen from the grid's centre 6 m up, turning once round the horizon over 400 frames. About 1 % of the pairs survive a camera run (16.74 M: 91,212 drawn and 65,918 casters at the median), which is the forest's shape: most pairs belong to instances behind the camera or beside it. `streamed-forest` (0.6 M pairs, holes) is the engine's own scene beside them.

## The pair table against the search, both session states

Main's build (a1eae86f), the search (`renderer.cull.pair_table` false) against the table (true), flown back to back in one lease; medians / p99 in ms, every frame of every pair culling the same set (visible, casters, shadow pairs, built structures):

| Pairs | rt/rt + bounce, unlocked: search | table | rt/rt + bounce, locked: search | table | hw/csm, locked: cull search / table | shadow cull search / table |
|---:|---:|---:|---:|---:|---:|---:|
| 1.01 M | 0.053 / 0.058 | 0.036 / 0.039 | 0.063 / 0.556 | 0.042 / 0.365 | 0.087 / 0.070 | 0.127 / 0.089 |
| 8.04 M | 0.326 / 0.650 | 0.183 / 0.537 | 0.326 / 1.650 | 0.199 / 0.223 | 0.451 / 0.357 | 0.861 / 0.469 |
| 16.74 M | 0.752 / 1.153 | 0.354 / 0.653 | 0.766 / 3.554 | 0.359 / 0.420 | 1.201 / 0.648 | 1.974 / 1.139 |

`streamed-forest` rt/rt, locked: 0.036 searched, 0.026 read (its tiles stream at their own pace, so the two flights' sets are not comparable frame by frame).

**The table wins in every state at every scale**, by a third at 1 M pairs and by half at 16.7 M; the p99 is where the search loses worst (3.55 against 0.42 ms locked at 16.7 M). The bytes were never the cost: at the cap the table is 134 MB, about 0.075 ms of the card's bandwidth, against a pass of 0.35 to 0.75 ms. What a pair's thread costs is its work — the search's dependent loads, then the instance, the mesh, the cluster, the moved sphere, six planes and every caster volume — and nearly every thread does all of it to learn that its instance is behind the camera. Batch 66's locked loss was a 1080p frame light enough for the card to idle between frames; the traced 4K frame keeps it awake, locked or not. So neither a 4-byte instance (half of bytes that do not matter) nor a marked hole (a hole is a few percent of a dispatch since T154 numbers them away) is where the time is.

## What cluster renderers do at this scale

- NVIDIA's `vk_lod_clusters` sample (read in its source, `shaders/traversal_init.comp.glsl`): "A thread represents one instance"; the instance's box is tested against the frustum and, for rasterization, last frame's Hi-Z and a minimum pixel size, and only a visible instance's LOD root is seeded into the cluster traversal — an instance that fails is never expanded into clusters at all. For ray tracing the sample keeps culled instances and only coarsens their LOD, because rays meet what the camera does not see.
- Nanite (read in López's frame analysis, "A Macro View of Nanite", 2021, which the deep-dive slides' text could not be fetched for): an instance culling pass of frustum and occlusion against the view, then the persistent cluster culling over each surviving instance's hierarchy, in a main pass against last frame's Hi-Z and a post pass against the one rebuilt from it — the two-pass occlusion this engine's cull already has at the pair.
- Haar and Aaltonen, "GPU-Driven Rendering Pipelines" (SIGGRAPH 2015 Advances): instance culling (frustum and occlusion), then cluster chunk expansion, then cluster culling. **Not read**: the deck is images and its text could not be extracted here; the order is taken from a search summary of it and agrees with the two read above.

Each puts a pass over instances in front of the cluster pass, so the cluster pass is sized by what can be seen and not by what exists.

## What was built: the cull culls instances first

[renderer](../subsystems/renderer.md#the-cull-culls-instances-first) has the mechanism. Measured (main against this branch, one lease, medians / p99 ms):

Flown 2026-11-18 00:05 to 00:12 with the session **locked**: main searching (its default), main reading the table, and this branch (b4f68c1e's code, `renderer.cull.instances` on), back to back a scene in one lease; 3840 × 2160 `--raster rt --shadows rt` with the bounce, 400 frames; the cull's zone counts the instance pass, its fold and the spheres.

| Pairs | cull: search | table | instances first | frame total: search | table | instances first |
|---:|---:|---:|---:|---:|---:|---:|
| 1.01 M | 0.052 / 0.055 | 0.035 / 0.038 | **0.019 / 0.027** | 2.632 | 2.619 | 2.604 |
| 8.04 M | 0.325 / 0.343 | 0.181 / 0.202 | **0.112 / 0.181** | 3.736 | 3.598 | 3.531 |
| 16.74 M | 0.747 / 0.776 | 0.354 / 0.396 | **0.236 / 0.342** | 5.018 | 4.624 | 4.494 |

Every frame of the three culled the same set (drawn, casters, shadow pairs, built structures). Against main as it ships (searching) the instance pass takes 0.51 ms off the cull at 16.7 M pairs and the frame's total moves by as much (5.02 → 4.49 ms); against the table, 0.12 ms. What is left at the cap is mostly the instance pass itself — 328,328 threads, each moving a sphere through six planes and the caster volumes — and the survivors' pairs. **Not flown**: the hw/csm pair (two occlusion passes and four cascades, where the instance pass also culls by the Hi-Z) and `streamed-forest`'s were queued behind twenty leases and had not flown when the row handed back; nor an unlocked pair of the instance pass, though this traced 4K frame keeps the card awake either way (the table's pairs read the same locked and unlocked, above).

## The review's finding, and a second one under it (2026-11-18)

A read-only review of the branch (finding A1) said the instance pass's Hi-Z test was not monotone in the sphere: it compared the depth of the point nearest the eye in distance, `c − r c/|c|`, whose view depth is `(|c| − r) cos θ`, while the sphere's least view depth is `|c| cos θ − r` — `r (1 − cos θ)` nearer, 0.29 r at 45 degrees off the axis. A large instance sphere off the axis could therefore read deeper than a small cluster inside it, and a surface between the two depths hide the instance and the cluster with it. The pair test has the same flaw at a cluster's radius.

`renderer: occlusion keeps a surface in front of the Hi-Z off the view's axis` builds it: a dumbbell of two unit cubes 37 m apart along the view's axis, its sphere (radius about 19.4) centred at (60, 0, −60) from an eye looking down −z with a 90° vertical field, at 302 × 154; its near cube (view depth 41 to 42) in front of a wall whose face is at 42.95 and which covers the rest of its rectangle; read the old way the sphere's depth is 46.3. Occlusion on must draw what occlusion off draws, with the cubes as two clusters and as one (one primitive, one cluster whose own sphere is the large one), the instance pass on and off. On 3136fcdb's shader (a debug build of the case with the shader of the row's tip, flown 03:11 under the lock) three of the four ways lost the near cube (0 of its 10 pixels): two clusters with instances first (A1), and one cluster both ways (the pair test's own flaw, on main since occlusion culling). With the depth made the sphere's least (`sphere_nearest_depth`, cluster_cull.slang; the derivation is in its comment and in [gfx](../subsystems/gfx.md)), the one-cluster pair pass drew it — and both instances-first ways **still** lost it.

That was a second defect: the renderer told the instance pass the mesh table held `null_mesh() + 1` meshes, and a scene read whole has no null mesh (`null_mesh()` is 0), so only mesh 0's sphere was ever measured and every other mesh's instances were tested by an unmeasured float4 — a point at the instance's origin. The dumbbell is mesh 1. The grid of rocks has one mesh and the tiled world is dynamic, which is why neither showed it. `CullInstances::prepare` now takes the table's size and measures every entry. With both mends the case passes all four ways (03:22).

The row's nine GPU cases, which had never had a verdict, flown the same way: on the row's tip four fail — `dynamic instances: the cull draws the same with instances culled first and not` (colour differences of 2 and 7 pixels in every way), `flythrough: occlusion culling changes no pixel on the path, and culls what is hidden`, `renderer: a caster outside the camera's frustum casts into the maps and the rays` (922 differences) and `renderer: culling stays conservative at every phase of a bend` (no pixel covered); with the fix eight pass and the bend still fails.

**What the fix costs** (2026-11-18 03:22–03:24, one lease, the session locked, 37 to 60 % of the processors busy with others' work; 16.74 M pairs, 3840 × 2160, 400 frames, the row's tip and the fix alternated three times; the cull's median / p99 ms):

| | row's tip (3136fcdb) | fix |
|---|---:|---:|
| `--raster rt --shadows rt`, bounce | 0.2370–0.2378 / 0.340–0.342 | 0.2359–0.2364 / 0.342–0.344 |
| `--raster hw --shadows csm` | 0.2892–0.2906 / 0.300–0.302 | 0.2891–0.2912 / 0.302–0.302 |

Nothing measurable. The traced flight culls by no Hi-Z (traced shadows turn occlusion off), so the depth costs it nothing and it culls the same set every frame. The hw/csm flight culls by the Hi-Z in both passes and draws 81,582 pairs at the median against 81,576: six pairs the old depth rejected are kept, about one in 13,600 — which of them showed a pixel was not measured, nor how many instances the instance pass rejects (nothing reads its count back).

## What a weaker card should expect

The pass over every pair costs threads, not bandwidth, so it scales with the card's thread throughput: the Titan Xp (Pascal, 30 SMs against the 5090's 170, at lower clocks) runs each thread's chain of dependent loads at about the same latency with a sixth of the threads in flight, so the 16.7 M pass that is 0.35 to 0.75 ms here would be a few milliseconds there. The instance pass is a thread an instance (a fiftieth of the pairs at the forest's ratio) and then only the survivors' pairs, so it is the change that scales down; the pair table's read is a smaller second.
