# ADR-0008: Slang for all shaders

- **Status:** Accepted
- **Date:** 2026-09-12
- **Plan references:** docs/plan/08-toolchain.md §8.4

## Context

The renderer needs one shader language for raster, compute, ray tracing, and the reference path tracer, with modules and generics to avoid preprocessor permutation explosions, reflection for the bindless pipeline, and a path to D3D12 if a backend is added later.

## Decision

Slang (Khronos-hosted, Apache-2.0 with LLVM exception) is the only shader language. Slang modules and link-time specialization replace `#ifdef` permutations. SPIR-V is emitted directly; DXIL via DXC if ever needed. Shaders hot-reload in the editor and are precompiled and cached by hash for shipping. The material graph compiles to Slang. The real-time path and the reference path tracer share BSDF, light, and camera modules.

## Consequences

One language, one reflection path, one permutation strategy. The Slang C++ API is stable in practice but not formally frozen; versions are pinned.

## Revisit when

Slang stops being maintained or a required target is unsupported.
