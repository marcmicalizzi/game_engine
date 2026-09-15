# gfx (domain)

**Purpose.** The RHI (docs/plan/04-renderer.md, ADR-0006): a thin, API-neutral layer over Vulkan 1.3/1.4 that the render graph and every renderer system build on. Phase 1 starts here with device discovery: enumerate the physical devices the loader exposes with the properties needed to choose a device and a capability tier, so experiment E2 (cluster acceleration structures on the development GPU) has data before any rendering code exists.

**Today.** `enumerate_adapters()` creates a throwaway Vulkan 1.3 instance, fills one `AdapterInfo` per physical device (name, vendor, ids, type, API version, driver name and info, device-local memory, queue families, and presence of each extension in `extensions_of_interest()`), assigns a tier (`raster`, `rt` with acceleration structures and ray-tracing pipelines, `rt-cluster` when `VK_NV_cluster_acceleration_structure` is present), sorts discrete devices first and by memory, and destroys the instance. `gpu.adapters` exposes the same through the protocol, so `engine-cli gpu.adapters` reports a machine's capabilities in the schema-typed form.

**No SDK required.** The Vulkan headers and volk come through FetchContent (`cmake/EngineGraphics.cmake`, tag `vulkan-sdk-1.4.357.0`); volk loads the driver's loader library at run time. Without a loader, `enumerate_adapters` returns false with a message and everything else keeps working, which is how build and test machines without GPUs behave. The SDK arrives with validation layers and shader tooling.

**Invariants (tested).** Every adapter has a name, a `1.x.y` API version, every extension of interest reported present or absent, at least one queue family (a graphics one unless the device is a CPU implementation), and a tier from the fixed set; the list is sorted discrete-first. On a machine without Vulkan the function fails cleanly and `vulkan_available()` agrees.

**Extensions of interest.** Acceleration structures, ray-tracing pipelines, ray queries, position fetch, RT maintenance, mesh shaders, NVIDIA cluster and partitioned acceleration structures, EXT and NV memory decompression, descriptor buffers, fragment shading rate and density map, swapchain, shader objects, cooperative matrices. Names rather than header macros, so an older header still compiles and reports an extension absent.

**Public API.** `domain/gfx/adapter.h`: `extensions_of_interest`, `vulkan_available`, `enumerate_adapters`, `describe_adapters`. Types from `schemas/gfx.schema`: `AdapterType`, `QueueFamilyInfo`, `AdapterInfo`.

**Depends on.** `base`, `containers`, `log`, `schemas`; Vulkan-Headers and volk (third_party/LICENSES.md).

**Testing.** `tools/dev.ps1 test -Filter gfx`. The test prints every adapter and extension it finds, so a CI log or a `ctest -V` run on a new machine is a capability report.

**Next.** Instance and device creation with the feature chain the renderer needs (dynamic rendering, synchronization2, buffer device address, descriptor indexing, timeline semaphores), the queue plan, Vulkan Memory Allocator, the command and synchronization primitives the render graph uses, validation layers behind an option, and the first offscreen clear-and-readback test that the capture API grows from. Experiments E1 and E2 follow.
