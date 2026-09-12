# ADR-0012: Adopted libraries: Jolt, Recast, SDL3, Tracy, mimalloc, SQLite, meshoptimizer; flecs pending E6

- **Status:** Accepted
- **Date:** 2026-09-12
- **Plan references:** docs/plan/08-toolchain.md §8.6, docs/plan/04-renderer.md §4.10

## Context

Building physics, navigation, windowing, profiling, allocation, persistence, or mesh processing from scratch would consume the schedule without differentiating the engine. Each candidate below is permissively licensed and current as of September 2026.

## Decision

Adopt: Jolt Physics (rigid and soft bodies, MIT), Recast/Detour (navigation, zlib), SDL3 (windowing and input, zlib), Tracy (profiling, BSD-3), mimalloc (allocator, MIT), SQLite (persistence, public domain), meshoptimizer including its cluster-LOD builder (MIT), Slang (ADR-0008), Vulkan Memory Allocator and volk (MIT), FLIP (BSD-3), Intel Open Image Denoise for the reference path (Apache-2.0), Steam Audio and miniaudio (Apache-2.0, MIT/public domain), Dear ImGui for tools (MIT), doctest for tests (MIT). Evaluate flecs (MIT) as the ECS under experiment E6 with EnTT or a small custom store as fallback. Build in-house: render graph, cluster runtime, software rasterizer, residency manager, ray scheduler, job system, tunables, document model, transactions, protocol codegen, sim scheduler, event log, LOD framework, support graphs, ReSTIR, baseline denoiser.

## Consequences

Every entry appears in `third_party/LICENSES.md` with its license. Hot systems own their own data regardless of ECS choice.

## Revisit when

E6 results, or a library's license or maintenance status changes.
