# ADR-0024: Hardware rasterization for all clusters in Phase 1; software rasterizer experimental

- **Status:** Accepted
- **Date:** 2026-09-16
- **Plan references:** docs/plan/04-renderer.md §4.3, docs/plan/10-roadmap-risks.md §10.4 (E1), docs/experiments/e1-raster-crossover.md

## Context

The renderer plan assumed a Nanite-style split: hardware rasterization for large clusters and a compute software rasterizer for clusters a few pixels across, where hardware triangle setup and 2×2 quad shading dominate. Experiment E1 was to find the crossover on the target GPUs before committing Phase 1 effort. Both paths now exist and write the same 64-bit visibility buffer, and the measurement ran on the development GPU (RTX 5090) at 3840×2160 and 11520×2160 with two million source triangles behind a GPU LOD cut.

## Decision

Phase 1 rasterizes every cluster through the hardware mesh-shader path into the visibility buffer. The software rasterizer stays in the tree as an experimental path (`engine-view --raster sw`, `CullParams::raster` split) and a measurement harness, off the frame's critical path.

## Consequences

The hardware path was faster at every triangle size measured, 4.5 px down to 0.7 px, by 1.5× to 3× at 11520×2160, and every split moved cost in the wrong direction. Keeping one path simplifies the visibility buffer contract, occlusion culling, and the material resolve; the rasterizer pass is under 0.4 ms at 11520×2160 for the scene measured, while the fullscreen resolve of the 64-bit buffer is the expensive pass at that resolution and becomes the next optimization target. The software rasterizer costs nothing to keep and remains available for GPUs without mesh shaders or for a smarter revision.

## Revisit when

A target GPU without mesh shaders or with slow small-triangle rasterization matters (an older discrete GPU, an integrated GPU, or mobile), or a scanline or tile-binned software rasterizer is written; rerun the E1 sweep on that hardware first.
