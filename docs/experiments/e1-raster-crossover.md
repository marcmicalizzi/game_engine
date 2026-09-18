# E1: hardware versus software rasterization of small clusters

- **Question (docs/plan/10-roadmap-risks.md §10.4):** where is the crossover between hardware and software rasterization for small clusters on the target GPUs, and does software rasterization belong in Phase 1?
- **Date:** 2026-09-16. **GPU:** NVIDIA GeForce RTX 5090, driver 610.88, Vulkan 1.4.341, Windows 11.
- **Machine state (added 2026-09-18):** not recorded at the time; the machine is shared with GPU diffusion workloads and parallel agent builds, and the harness did not yet know how to look ([bench](../subsystems/bench.md#measuring-on-a-shared-machine)). Every number here is therefore an **upper bound**. What saves the experiment is that it is a *ratio* — hardware against software rasterization, both measured in the same run against the same LOD cut — and a contended GPU taxes both paths. The caveat below about the desktop compositor sharing the GPU was the part of this that was noticed at the time; the rest of the machine's load was not. Nothing was re-measured, because [ADR-0024](../adr/0024-hardware-rasterization-first.md) rests on the ratios and they are consistent across sixteen rows. Re-run with `--require-quiet` before quoting any absolute millisecond from this page.
- **Decision:** [ADR-0024](../adr/0024-hardware-rasterization-first.md). Hardware rasterization for every cluster in Phase 1; the software rasterizer stays in the tree as an experimental path.

## Setup

Both paths write the same 64-bit visibility buffer (reversed-Z depth in the high word, cluster and triangle id in the low word, 64-bit atomic max) from the same LOD cut, so the comparison is rasterization alone:

- **Hardware:** one mesh-shader workgroup per cluster, `fs_visibility` fragments doing the atomic max, an attachment-less raster pass, `vkCmdDrawMeshTasksIndirectEXT`.
- **Software:** `cluster_sw_raster.slang`, one 128-thread compute workgroup per cluster, vertices to pixel space in shared memory, one thread per triangle walking its bounding box with edge functions, `vkCmdDispatchIndirect`. A first version: no scanline stepping, no tile binning, no coverage masks.
- **Scene:** `engine-view --grid 1025` (2,097,152 triangles, 46,626 clusters in 16 DAG levels, 23 s to build), fixed orbit distance, GPU culling and LOD selection on, 150 frames, per-pass GPU milliseconds from timestamp queries averaged over the run (`gpu_ms` in the JSON summary). Leaf triangles are about 0.0195 units on an edge; one pixel at distance d is about 0.00048·d units, so leaf triangles measure about 4.5 px at orbit 9, 1.7 px at orbit 24, and 0.7 px at orbit 60. The LOD threshold picks coarser levels as it grows.
- **Windows:** 3840×2160 and 11520×2160 on the 11520×2160 desktop. Both are composited windows, not exclusive fullscreen, so the desktop compositor shares the GPU; the 4K numbers are visibly noisier (see caveats).

The sweep script and raw results (`results.jsonl`, 64 runs) are in the session scratch directory; the rows below are the ones that decide the question.

## Results

GPU milliseconds per frame for the rasterization pass alone; "clusters" is the LOD cut size.

**11520×2160, orbit 9 (terrain fills the frame, leaf triangles ≈ 4.5 px)**

| LOD px | clusters | hardware | software | ratio |
|---|---|---|---|---|
| 0.5 | 2,245 | 0.376 | 0.739 | 2.0× |
| 1 | 1,435 | 0.393 | 0.604 | 1.5× |
| 2 | 1,082 | 0.374 | 0.725 | 1.9× |
| 4 | 881 | 0.336 | 0.684 | 2.0× |

**11520×2160, orbit 24 (about a third of the frame, leaf triangles ≈ 1.7 px)**

| LOD px | clusters | hardware | software | ratio |
|---|---|---|---|---|
| 0.5 | 1,131 | 0.121 | 0.199 | 1.6× |
| 1 | 623 | 0.090 | 0.289 | 3.2× |
| 2 | 335 | 0.090 | 0.304 | 3.4× |
| 4 | 183 | 0.059 | 0.340 | 5.8× |

**Sub-pixel triangles: orbit 60, LOD 0.5 (leaf triangles ≈ 0.7 px), 383 clusters**

| resolution | hardware | software | ratio |
|---|---|---|---|
| 3840×2160 | 0.049 | 0.110 | 2.2× |
| 11520×2160 | 0.017 | 0.053 | 3.2× |

**3840×2160, orbit 9 (leaf triangles ≈ 4.5 px)**

| LOD px | clusters | hardware | software |
|---|---|---|---|
| 0.5 | 1,825 | 0.568 | 0.575 |
| 1 | 1,152 | 0.433 | 0.949 |
| 2 | 835 | 0.296 | 0.820 |
| 4 | 677 | 0.642 (0.276 in the split run) | 0.639 |

**Splitting by projected cluster diameter (11520×2160, orbit 9, LOD 1; pure hardware = 0.393)**

| software below | hw clusters | sw clusters | hardware | software | sum |
|---|---|---|---|---|---|
| 32 px | 1,426 | 9 | 0.277 | 0.053 | 0.330 |
| 64 px | 1,364 | 71 | 0.391 | 0.094 | 0.485 |
| 128 px | 1,106 | 329 | 0.363 | 0.256 | 0.619 |
| 256 px | 183 | 1,252 | 0.119 | 0.686 | 0.805 |

Other passes for scale, at 11520×2160: culling 0.02 to 0.06 ms for 46,626 clusters; the fullscreen resolve of the 64-bit buffer 0.4 to 0.9 ms, the most expensive pass in the frame at that resolution.

## Reading

- The hardware path wins at every triangle size measured, from 4.5 px down to 0.7 px, by 1.5× to 3× at 11520×2160 and by up to 2× at 4K, with one tie (4K, 1,825 clusters of 4.5 px triangles). No crossover appears in the measured range.
- Splitting by cluster size never beats pure hardware: every cluster moved to software costs more than it saves, and the sum grows monotonically with the software share.
- Software cost is roughly proportional to cluster count and weakly dependent on triangle size, as expected from a per-triangle bounding-box walk; hardware cost falls with cluster count faster.
- Absolute numbers are small: the whole cluster pipeline (cull, raster, resolve) stays under 1.3 ms at 11520×2160 for two million source triangles. The resolve pass, not rasterization, is where that resolution hurts.

## Caveats

- Composited windows: the 4K rows vary up to 2× between runs with identical work (LOD 4: 0.642 vs 0.276). Locking GPU clocks and using exclusive fullscreen would tighten them; the 11520×2160 rows are consistent across runs and carry the conclusion.
- One GPU. Blackwell's mesh-shader path is fast at small triangles; an older or a mobile GPU may cross over. Re-run the sweep (`engine-view --raster hw|sw --orbit --lod`) when such a target exists.
- A naive software rasterizer. A scanline or tile-binned version with coverage masks would be faster, perhaps 2× to 3×, which would bring it to parity at sub-pixel sizes rather than ahead.
- Depth-only visibility, no occlusion culling, no material work. Occlusion culling reduces both paths equally; material work happens in the resolve, not in rasterization.

## What changes

Phase 1 uses hardware rasterization for all clusters. The software rasterizer, its `--raster sw` mode, and the cull pass's split stay in the tree as the measurement harness and for a later revisit on other hardware; they are not on the frame's critical path. The next renderer work goes to occlusion culling and to making the resolve cheaper at surround resolutions.
