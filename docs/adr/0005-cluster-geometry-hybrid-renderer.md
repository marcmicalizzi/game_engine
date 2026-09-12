# ADR-0005: The cluster as the universal geometry unit; hybrid renderer; reference path tracer from day one

- **Status:** Accepted
- **Date:** 2026-09-12
- **Plan references:** docs/plan/04-renderer.md §4.2–4.4, §4.8

## Context

Virtualized cluster geometry and hardware ray tracing fight each other: per-frame cluster LOD selection versus prebuilt acceleration structures. Getting them to agree is the central renderer problem. Separately, the project needs an objective way to accept or reject optimizations.

## Decision

A cluster (up to 128 triangles, in a DAG LOD hierarchy) is simultaneously the unit of rasterization, streaming, and acceleration-structure construction. The real-time renderer is hybrid: rasterized primary visibility (mesh shaders for large clusters, compute software raster for small ones) into a visibility buffer, ray-traced secondary lighting (in-house ReSTIR direct and indirect, radiance cache, RT reflections and shadows). Cluster acceleration structures are used where the hardware offers them, with a first-class static multi-LOD BLAS fallback elsewhere. A GPU path tracer sharing BSDF, light, and camera code with the real-time path is built in the first renderer milestone and drives FLIP-based image-error gates.

## Consequences

The geometry build pipeline and cluster data format are near-irreversible. Primary-visibility path tracing in real time is a non-goal. Every rendering optimization is judged on frame time and image error together.

## Revisit when

E1 or E2 invalidates the software-raster crossover or the cluster-AS strategy.
