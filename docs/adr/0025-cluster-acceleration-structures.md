# ADR-0025: Cluster acceleration structures on NVIDIA, KHR bottom-level structures elsewhere; Vulkan stays

- **Status:** Accepted
- **Date:** 2026-09-17
- **Plan references:** docs/plan/04-renderer.md §4.4, docs/plan/10-roadmap-risks.md §10.4 (E2), docs/experiments/e2-cluster-acceleration.md

## Context

The renderer's defining promise is that ray tracing and rasterization see the same geometry: the cluster is the unit of both, and the ray tracing structures are built from the clusters the rasterizer selected this frame. Cluster-level acceleration structures exist on NVIDIA through `VK_NV_cluster_acceleration_structure` on Vulkan and through NVAPI on D3D12; nowhere else yet. Experiment E2 was to measure whether the Vulkan extension delivers the build cost, memory, and image match the plan needs, or whether the primary API had to change, before the RT side of Phase 1 was designed around either answer.

## Decision

Vulkan remains the primary graphics API. On GPUs that expose `VK_NV_cluster_acceleration_structure`, ray tracing geometry is built per frame as one cluster acceleration structure per selected cluster plus a cluster bottom-level structure over them (`gfx/cluster_acceleration.h`). On every other ray tracing GPU, the fallback is a `VK_KHR_acceleration_structure` bottom-level structure with one geometry per cluster (`gfx/acceleration.h`). Both paths report the cluster through the hit's geometry index and the triangle through the primitive index, so shaders, the visibility buffer, and the tests are shared. Ray queries and the standard top-level structure serve both.

## Consequences

Measured on the RTX 5090 (E2): the cluster path takes 2.9× to 3.3× less memory (about 14 bytes per triangle against 62), builds 1.2× to 2.6× faster, traces at the same speed, and produces a pixel-identical picture; both paths match the rasterized cut word for word up to shared-edge pixels. The renderer therefore gets one RT geometry interface with two backends and no LOD mismatch on NVIDIA. The fallback keeps the plan's LOD-mismatch mitigations (ray bias, normal offsets) for AMD and Intel, where RT geometry will lag the raster cut. Cluster ids travel in the 24-bit base geometry index, which bounds one bottom-level structure at 16 million clusters. The per-frame GPU-driven build (records and counts written by the cull pass, the bottom-level address patched into the instance record on the GPU) is the next step and is what the extension's indirect build was designed for.

## Revisit when

A cross-vendor cluster acceleration structure ships (the DXR 2.0 draft or a Vulkan KHR/EXT equivalent), AMD's Dense Geometry Format becomes buildable from the cluster format, per-frame builds of 50,000-cluster cuts exceed their budget on the target GPUs, or a second ray tracing GPU class is measured and disagrees with the RTX 5090.
