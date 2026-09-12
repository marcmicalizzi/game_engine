# ADR-0006: Vulkan primary behind a thin API-neutral RHI; Windows first, Linux second; D3D12 deferred

- **Status:** Accepted
- **Date:** 2026-09-12
- **Plan references:** docs/plan/08-toolchain.md §8.3, docs/plan/02-architecture.md §2.6

## Context

As of September 2026, cluster acceleration structures exist on Vulkan only through NVIDIA's `VK_NV_cluster_acceleration_structure` with no KHR/EXT version, while D3D12 specifies cross-vendor cluster AS in the DXR Tier 2.0 draft (preview expected late 2026). Linux is a stated target; mobile is a later consideration; the team is small; the development GPU is an RTX 5090. An earlier draft chose D3D12 on the strength of its roadmap.

## Decision

Vulkan 1.3/1.4 is the primary and only implemented graphics API. Windows first, Linux second, with CI on both from Phase 1. The RHI is thin and API-neutral, modeled on Vulkan concepts that map cleanly to D3D12 (dynamic rendering, synchronization2, buffer device address, descriptor indexing or buffers, indirect commands, timeline semaphores); no API handles leak above it. Cluster RT is NVIDIA-only for now; AMD and Intel take the fallback path. GPU decompression uses `VK_EXT_memory_decompression` where available and CPU decompression on efficiency cores otherwise.

## Consequences

One backend covers both platforms and the development machine gets cluster RT immediately. PIX and DirectStorage GPU decompression are unavailable until a D3D12 backend exists. Windows and Linux may differ where a vendor extension exists on one only; capability tiers make gaps explicit.

## Revisit when

DXR Tier 2.0 cluster AS ships retail and AMD/Intel cluster RT matters for the target audience, or E2 shows the Vulkan path materially behind.
