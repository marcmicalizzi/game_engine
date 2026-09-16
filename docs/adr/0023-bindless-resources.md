# ADR-0023: Bindless resources through descriptor indexing and device addresses

- **Status:** Accepted
- **Date:** 2026-09-16
- **Plan references:** docs/plan/04-renderer.md §4.2, docs/plan/08-toolchain.md §8.3, ADR-0006

## Context

The renderer is GPU-driven: culling, LOD selection, and material resolve run on the GPU over device-resident scene data, and a visibility-buffer material pass touches any texture of any material in one dispatch. Per-draw descriptor sets cannot express that. Vulkan offers two bindless models: descriptor indexing (core 1.2: runtime descriptor arrays, partial binding, update after bind) and `VK_EXT_descriptor_buffer` (descriptors written into ordinary device memory). Buffers can bypass descriptors entirely through buffer device addresses (core 1.2). The development GPU supports all three; descriptor buffers are not universal across vendors and driver generations. ADR-0006 left the choice open.

## Decision

One global descriptor set per device, bound at set 0 by every pipeline, holds three unbounded, partially bound, update-after-bind arrays: sampled images, storage images, and samplers. Resources register once and receive a stable 32-bit index that shaders receive through push constants or through device-resident scene data. Buffers are never in the set: every buffer a shader reads is reached through its device address (`VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`, Slang pointers), so the set holds only what device addresses cannot express. Per-draw and per-dispatch data travel in a push-constant block of at most 128 bytes: indices and addresses, never values. Slot release is deferred until the GPU has finished the frames that could still read the slot, through the timeline value `FrameContext` exposes.

Descriptor buffers are not adopted now. They become an optional fast path only if profiling shows descriptor update or bind cost on the critical path, behind the same `BindlessSet` interface.

## Consequences

Every pipeline layout is the same shape (set 0 plus push constants), which keeps the render graph, the material system, and shader code simple: a material is a struct of indices and addresses in a device buffer. Descriptor updates are cheap and happen at resource creation, not per draw. Non-uniform indexing must be marked (`NonUniformResourceIndex`) in shaders that index by per-pixel material. The universal path works on every Vulkan 1.2 device with `descriptorIndexing`, which the device creation already requires. The set's capacities are configured per project and clamped to the device's update-after-bind limits; running out of slots is a reported error, not a hidden limit (ADR-0017).

## Revisit when

Descriptor update cost appears in profiles at scale, or a target platform lacks update-after-bind support.
