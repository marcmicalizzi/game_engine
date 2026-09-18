# E2: cluster acceleration structures against the first-class path

- **Question (docs/plan/10-roadmap-risks.md §10.4):** can ray tracing geometry be built from the same clusters the rasterizer selected, per frame, at acceptable build cost and memory, with an image that matches the raster picture; and does the Vulkan path (`VK_NV_cluster_acceleration_structure`) do it, or is D3D12 with NVAPI needed?
- **Date:** 2026-09-17. **GPU:** NVIDIA GeForce RTX 5090, driver 610.88, Vulkan 1.4.341, Windows 11.
- **Machine state (added 2026-09-18):** not recorded at the time; the machine is shared with GPU diffusion workloads and parallel agent builds, and the harness did not yet know how to look ([bench](../subsystems/bench.md#measuring-on-a-shared-machine)). The build and trace times are therefore **upper bounds**, and the *memory* figures are not affected at all — they are structure sizes the driver reports, not timings. [ADR-0025](../adr/0025-cluster-acceleration-structures.md) turns on the memory ratio (2.9–3.3× less) and the build ratio (1.2–2.6× faster), both measured in the same runs, so the decision stands; the absolute microseconds should be re-taken with `--require-quiet` before anything budgets against them. A run of this today would also report the card's `gpu_memory` and `machine_state`, which is what would have said whether the 17 GB a diffusion job holds was resident while these structures were built.
- **Decision:** [ADR-0025](../adr/0025-cluster-acceleration-structures.md). Vulkan stays the primary API; cluster acceleration structures are the RT geometry path on NVIDIA GPUs that expose the extension, and one KHR bottom-level structure per cluster set is the fallback everywhere else.

## Setup

Both paths trace the same LOD cut that the rasterizer drew, through the same ray-query shader (`ray_visibility.slang`), into the visibility buffer's own 64-bit word (reversed-Z depth bits, cluster id, triangle id), so the three pictures compare word for word:

- **Raster:** the cut through the vertex-shader cluster path (`cluster_vertex.slang`), the same buffer the renderer uses.
- **KHR path (`gfx/acceleration.h`):** one `VK_KHR_acceleration_structure` bottom-level structure with one triangle geometry per cluster of the cut (float3 positions, 16-bit local indices), so `GeometryIndex()` is the cluster within the cut and `PrimitiveIndex()` the triangle; one instance in a top-level structure.
- **Cluster path (`gfx/cluster_acceleration.h`):** one cluster acceleration structure (CLAS) per cluster of the cut, built in one `vkCmdBuildClusterAccelerationStructureIndirectNV` from a device array of per-cluster records (8-bit local indices, float3 positions, the cluster id as the base geometry index so `GeometryIndex()` names the cluster without the vendor intrinsic), then a cluster bottom-level structure over the CLAS addresses the first build wrote, then the same top-level structure. Implicit destinations: the driver packs the CLAS into one buffer and reports each one's address and size.
- **Scene:** the engine-view heightfield's LOD DAG (`build_cluster_lod`, 124-triangle clusters). Two cuts: the frame's cut at a 1 px threshold from a fixed camera on a 129×129 terrain, and every leaf of the 257×257 terrain engine-view renders by default. GPU times from timestamp queries; memory from the structures' buffer sizes (`vkGetAccelerationStructureBuildSizesKHR`, and the per-CLAS sizes the build reports).
- **Where:** `domain/gfx/tests/rt_cluster_tests.cpp` (the cluster path, run twice) and `rt_tests.cpp` (the KHR path alone). Both skip on GPUs without the features.

Ray queries traverse cluster acceleration structures from a compute pipeline with no opt-in flag; only ray tracing pipelines carry `VkRayTracingPipelineClusterAccelerationStructureCreateInfoNV`.

## Results

| Cut | Clusters | Triangles | KHR BLAS | CLAS + cluster BLAS | KHR build | CLAS build + cluster BLAS build | Trace KHR | Trace CLAS |
|---|---|---|---|---|---|---|---|---|
| Frame, 1 px, 640×480 | 85 | 7,082 | 444 KB | 151 KB + 5 KB | 0.273 ms | 0.063 + 0.039 ms | 13.0 µs | 12.0 µs |
| All leaves, 1280×720 | 1,357 | 131,072 | 8,108 KB | 2,378 KB + 59 KB | 0.348 ms | 0.080 + 0.203 ms | 23.2 µs | 24.4 µs |

- **Memory:** the cluster path takes 2.9× less at the frame scale and 3.3× less for the whole mesh: about 1,750 bytes per 124-triangle cluster, 14 bytes per triangle, against 62 bytes per triangle in the KHR structure.
- **Build:** 2.6× faster at the frame scale (0.10 ms against 0.27 ms) and 1.2× faster for the whole mesh (0.28 ms against 0.35 ms). The CLAS build itself barely grows with the cluster count (63 µs to 80 µs for 16× the clusters); the cluster bottom-level build over 1,357 references is the part that scales (39 µs to 203 µs). The top-level builds are 22 to 26 µs either way.
- **Trace:** the same within 5% in both directions. Cluster structures cost nothing to trace against on this GPU.
- **Image:** the two ray-traced pictures are identical to the pixel (0 differences on 172,290 covered pixels). Against the rasterizer both differ on 8 silhouette pixels of coverage and name the other triangle of a shared edge on 0.5% of pixels, with depth agreeing to 4e-6; that is the rasterizer's edge rule against the ray's, not geometry.

## Caveats

- One GPU, one driver; the Titan X and Titan Xp have no ray tracing, so there is no second data point until an RTX 40-series or AMD RDNA machine is reachable. The fallback path is what AMD and Intel will run.
- Static cuts built once. A frame builds the cut every frame from the GPU's own cull output; the record array and the `srcInfosCount` device address exist for exactly that, but the per-frame pipeline (records written by a shader, the cluster bottom-level address patched into the instance record on the GPU) is not built yet.
- Cluster ids reach the shader through the base geometry index (24 bits) because the prebuilt Slang compiler exposes no cluster-id intrinsic; that caps a bottom-level structure at 16 million clusters, which is not a limit that matters.
- Templates (`VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_TRIANGLE_CLUSTER_TEMPLATE_NV`, instantiated per instance with new positions) were not measured; they are the tool for skinned and deforming clusters.
- Build times at this scale are dominated by fixed cost. The cluster BLAS build's growth (0.15 µs per cluster reference) is the number to watch at 50,000-cluster cuts.
