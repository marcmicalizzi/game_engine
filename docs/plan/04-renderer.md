# 04 — Renderer Architecture

## 4.1 Goals, targets, non-goals

Goal: maximum image quality per millisecond, with runtime cost independent of source-asset complexity.

Provisional targets, to be confirmed once hardware is in hand:

| Configuration | Target |
|---|---|
| 2560×1440, RTX 4070-class | 60 fps hybrid RT, internal resolution ≥ 67% with upscaler |
| 2560×1440, GTX Titan X (Maxwell) with an i7-980: the project's minimum test machine (no mesh shaders, no ray tracing, SSE4.2 CPU) | Runs the baseline tier: cluster geometry through a vertex-shader path, raster lighting, no RT; 30 fps at reduced settings is the bar, and the build carries no ISA above x86-64 baseline so the CPU side runs unmodified |
| 3840×2160, RTX 4080/5080-class | 60 fps hybrid RT with upscaler |
| 11520×2160 surround, RTX 5090-class | 60 fps target, 30 fps floor; attention region at full shading rate, peripheral views at reduced rate |
| Reference mode | Converged path trace; minutes per frame acceptable |

The surround row is the configuration the project owner plays on. Well-optimized non-RT titles reach 70–100 fps there today and poorly optimized ones fail to hold 30; closing that gap with ray tracing enabled is the renderer's defining target.

Non-goals for v1: primary-visibility path tracing in real time; non-RT GPUs as a quality target (they run the baseline tier); mobile.

## 4.2 Frame architecture

GPU-driven, visibility-buffer, hybrid lighting. Passes, as render-graph nodes:

1. **Scene update.** Upload instance transform deltas; rebuild TLAS; BLAS builds/refits under budget (async compute, starts early).
2. **Culling.** Instance frustum + occlusion (Hi-Z from the previous frame, reprojected); hierarchical cluster culling + LOD selection by projected error; output cluster lists split by size class.
3. **Visibility raster.** All clusters via mesh shaders writing a 64-bit (depth | instance | triangle) visibility buffer with atomics; a compute software rasterizer for small clusters exists but measured slower than the hardware path at every triangle size on the development GPU (E1, ADR-0024) and stays experimental. Two-pass occlusion: pass 1 draws last frame's visible set and builds Hi-Z; pass 2 tests the rest.
4. **Material resolve.** Per pixel: reconstruct barycentrics from triangle ID, fetch attributes, evaluate material into a compact GBuffer. Materials are bindless; per-material-ID tile classification keeps divergence low.
5. **Lighting.** RT direct lighting with ReSTIR DI over all emitters; RT GI via ReSTIR GI fed by a world-space radiance cache; RT reflections (rough surfaces fall back to the cache); RT shadows for key lights; sky/atmosphere; volumetrics.
6. **Denoise and compose.** Per-signal denoisers, then composition.
7. **Forward/transparent.** Particles, glass, water (raster; RT refraction optional).
8. **Post and upscale.** Exposure, bloom, DoF, motion blur, then TAAU/DLSS/FSR/XeSS, optional frame generation.
9. **UI** in HUD coordinate spaces ([§4.7](#47-ui-coordinate-architecture)).

Infrastructure: render graph (automatic barriers, transient aliasing, async-compute scheduling); fully bindless resources; GPU-resident scene (instances, materials, lights, clusters in device buffers); indirect execution everywhere; one **GPU residency manager** spanning geometry pages, texture pages, and acceleration structures.

## 4.3 Geometry

**The cluster is the universal unit** of rasterization, streaming, and acceleration-structure construction. This is the near-irreversible renderer decision.

**Build** (content-build step, a `derived` node):
1. Split the mesh into clusters of ≤128 triangles (meshoptimizer).
2. Group 4–8 adjacent clusters (graph partition; METIS or own), simplify the group with locked boundaries (meshoptimizer simplifier), re-split into clusters. Repeat to a root. The result is a DAG where each cluster stores its own error bound and its parent group's. meshoptimizer 1.2 ships a cluster-LOD DAG builder (`clusterlod.h`) that covers steps 1–2; start from it rather than writing a partitioner. NVIDIA's open `vk_lod_clusters` sample (Apache-2.0) is the closest public reference for continuous cluster LOD with streaming, mesh-shader raster, and cluster-AS ray tracing from one hierarchy.
3. Quantize positions per cluster (16-bit relative to cluster bounds), pack normals/tangents/UVs; store clusters in fixed-size **pages** (~128 KB) ordered by DAG locality for streaming.
4. Per cluster: bounds, normal cone, LOD error, material ID, page/offset, and precomputed data for cluster acceleration-structure construction.

**Runtime**
- LOD selection: a cluster is drawn if its own projected error is below threshold and its parent group's is not. Evaluated per cluster in parallel; the hierarchy is only used for culling.
- Per-instance culling BVH over cluster groups, traversed with persistent threads or a multi-dispatch work queue.
- Small-triangle path: experiment **E1** measured a compute software rasterizer against mesh-shader rasterization from 4.5 px down to 0.7 px triangles on the RTX 5090 and found hardware faster throughout (docs/experiments/e1-raster-crossover.md); the software path is kept for other hardware and a smarter revision, not for Phase 1.
- Streaming: the GPU writes requested page IDs to a feedback buffer; the CPU streams pages (DirectStorage with GPU decompression where available); the residency budget evicts by last use.

**Special cases**
- **Skinned meshes**: clusters with skinning in compute into a per-frame vertex pool; lower cluster detail; per-frame BLAS refit with a bounded count.
- **Terrain**: heightfield with clipmap/CDLOD tessellation and virtual texturing initially; rocks and cliffs as cluster meshes. Converting terrain itself to clusters is a later optimization. The terrain displacement also reads the per-tile **deformation map** (snow, sand, and mud depth; grass bend direction) described in [05 §5.13](05-simulation.md#513-deformable-surfaces-and-soft-bodies), so walking through snow costs a texture fetch in the terrain path rather than geometry edits.
- **Vegetation**: instanced cluster meshes near, impostors far; alpha-tested foliage in RT via opacity micromaps.
- **Materials**: PBR (GGX metal/rough, clearcoat, sheen, transmission, subsurface approximation), layered via a small material graph compiled to Slang. The same BSDF code is used by the reference path tracer.

## 4.4 Ray tracing

**Acceleration structures**
- TLAS rebuilt every frame (cheap at 10^4–10^5 instances).
- BLAS strategy is backend-dependent behind one interface:
  - **Cluster-AS path**: build cluster-level AS from the *same* clusters the rasterizer selected this frame; assemble BLAS from them. Raster and RT geometry match exactly and share streaming. Availability as of September 2026: NVIDIA only, via NVAPI on D3D12 or `VK_NV_cluster_acceleration_structure` on Vulkan (RTX 20-series and up, driver 570+). Cross-vendor cluster AS is specified in the DXR Tier 2.0 draft (D3D12, preview expected late 2026) and has no Vulkan KHR/EXT equivalent. AMD is pursuing a compressed geometry block format (Dense Geometry Format) for AS builds instead; the cluster format should be encodable to it later. On the primary Vulkan backend this means cluster RT is NVIDIA-only until a cross-vendor extension appears; AMD and Intel take the fallback path, which is first-class rather than an afterthought.
  - **Fallback path** (any RT GPU): per-mesh static BLAS at 2–3 fixed LODs chosen by distance; accept the LOD mismatch and mitigate with ray bias and normal-offset tricks. RT LOD is coarser than raster LOD by design.
- **BLAS budget manager**: per-frame time budget for builds and refits; priority by screen contribution; fast low-quality builds first, background rebuild at high quality when idle. Destruction uses the same manager ([§4.5](#45-destruction-and-acceleration-structures)).

**Lighting techniques: adopt, do not invent**
- Direct: ReSTIR DI. NVIDIA's RTXDI is the reference implementation but is under the proprietary NVIDIA RTX SDKs License, which a permissively licensed engine cannot redistribute ([08 §8.8](08-toolchain.md#88-engine-license-and-dependency-policy)). The algorithms are published; ReSTIR is implemented in-house in Slang with RTXDI as reading material.
- GI: ReSTIR GI plus a world-space radiance cache (hash grid or surfels with multi-frame accumulation). Neural radiance caching later, where tensor hardware exists (NVIDIA's NRC library is experimental and under the same proprietary license).
- Reflections: RT below a roughness threshold, cache lookup above.
- Shadows: RT for the sun and hero lights; ReSTIR-sampled for the rest.
- Denoising: an in-house SVGF/ReBLUR-class denoiser is the portable baseline. NVIDIA NRD (same license as RTXDI) and vendor neural reconstruction (DLSS Ray Reconstruction on all RTX GPUs via Streamline; AMD FSR Ray Regeneration on RDNA4 only) are optional plugins that the game developer adds under the vendor's license. Intel Open Image Denoise (Apache-2.0) serves the offline reference path.
- Coherence: shader execution reordering where supported; ray sorting by direction and material for GI rays.

**Ray budget scheduling.** Rays per pixel are a function of screen-region importance, temporal stability, surface roughness, and disocclusion. Importance comes from display topology (center view high, periphery low) and from variance estimates. The scheduler is a GPU pass that emits per-tile ray counts consumed by the sampling kernels. This is where "expensive work proportional to perceptual effect" becomes concrete.

## 4.5 Destruction and acceleration structures

- **Pre-fractured** chunk hierarchies (built offline) have prebuilt BLAS per chunk. Destruction changes instance transforms and visibility; no rebuild.
- **Damage-state variants** (intact, damaged, destroyed, rubble cleared) are separate meshes with their own BLAS; distant and persistent destruction swaps variants.
- **Runtime fracture** (if adopted later) produces new meshes → fast low-quality BLAS builds through the budget manager, refit while pieces move, high-quality rebuild once settled.
- Debris bodies are numerous and short-lived: instance them into a shared per-chunk BLAS; drop them from RT after settling and beyond a distance.

## 4.6 Extreme displays

- **Multi-view rendering is first-class.** A `ViewSet` is N views sharing one scene, one culling hierarchy, one TLAS, one residency budget. Surround = 3 views with per-monitor frusta; a single ultrawide = 1 view, optionally with a Panini-style projection. Screen-space passes (TAA, SSR, denoise) run per view; RT is view-agnostic.
- **Foveation by attention region**: the user defines an attention region (default: the center monitor for surround, the whole display otherwise). Views or view sections outside it run at reduced internal resolution, coarser VRS rate, smaller ray budget, and cheaper effects (reflection roughness cutoff, volumetric step count, shadow detail). Surround players treat side monitors as peripheral vision, so aggressive cuts there are the expected behavior, not a compromise. The gradient is a user setting; the upscaler runs per view.
- **Presentation**: support both driver surround (one swapchain, views tiled) and multi-window (one swapchain per output). HDR metadata per output.
- **Coordinate hygiene**: no 16-bit screen coordinates anywhere; all screen-space structures are sized from the render config at startup; CI renders at 11520×2160, 7680×4320, 1080×3840 portrait, and 5120×1440 every night.

## 4.7 UI coordinate architecture

Adopt the brief's model as written:

```
RenderSurface        pixels of the whole output (or per view)
PhysicalDisplays[]   from the OS when available
ViewSet              render frusta
HudSafeRegion        rect in surface pixels; default = center 16:9 (surround) or full (single)
HudScale             user-set
UiSpace              Surface | View | HudSafe | WorldProjected | Reading
```

Every UI element declares its `UiSpace` and anchor. The layout engine resolves to surface pixels each frame. The HUD-safe editor is a small in-editor tool that writes `HudSafeRegion` to user config. Default behavior is correct for surround and ultrawide without per-element work. The game UI framework choice is deferred ([10-roadmap-risks §10.6](10-roadmap-risks.md#106-decisions-deliberately-deferred)); Dear ImGui serves tools now.

## 4.8 Reference renderer and objective optimization

- **GPU path tracer** in Slang sharing material/BSDF, light, and camera code with the real-time path; a compile-time flag selects the integrator. Unbiased, converged to a noise floor. Spectral rendering is not required.
- **Metrics**: FLIP (perceptual) as the primary gate, PSNR/SSIM secondary; per-region metrics so a periphery regression weighs less than a center one; a temporal-stability metric on short sequences.
- **Scene corpus** in `content/test-scenes/`, versioned, including pathological scenes: 10^6 emitters, foliage walls, mirror rooms, 10^5 instances, thin geometry, 48:9 frusta.
- **Optimization loop**: propose → build → run corpus at fixed resolutions → collect frame time, counters, FLIP → compare to baseline → accept if within error tolerance and faster. Exposed to agents as a single tool ([06-agent-tooling §6.9](06-agent-tooling.md#69-day-one-operations)). Noise handled by repeated runs and medians.

## 4.9 Streaming and residency

- One residency manager, three page types (geometry, texture, AS), one budget policy with per-type floors.
- Requests come from GPU feedback (geometry pages, virtual-texture pages) and from tile predictions based on observer velocity.
- I/O via platform async I/O (IOCP on Windows, io_uring on Linux) feeding a CPU decompression pool on efficiency cores, with GPU decompression through `VK_EXT_memory_decompression` where the driver supports it (NVIDIA today). meshoptimizer codecs for geometry, zstd or GDeflate for everything else. DirectStorage's GPU decompression requires a D3D12 device and is reserved for a future D3D12 backend.
- Priority = f(screen contribution, distance to observer, time since request), with a starvation guard for the current view.

## 4.10 Reuse versus build

| Build in-house | Reuse |
|---|---|
| Render graph, cluster pipeline, software rasterizer, residency manager, ray scheduler, view sets, UI layout, ReSTIR, baseline denoiser | Slang, VMA/volk, meshoptimizer (incl. clusterlod), METIS, FLIP, KTX/Basis, OIDN (reference path); Streamline/FSR/XeSS as optional developer-supplied plugins |

Build the thin RHI over Vulkan in-house: the renderer *is* the product and GPU-driven design needs full control. NVRHI is the fallback if velocity is poor (MIT; Vulkan and D3D12; mesh shaders; cluster AS only via NVAPI on D3D12), but note that its companion Donut framework has no RT passes and does not manage acceleration structures. The valuable parts of NVIDIA's samples are shaders and algorithms, which port regardless of RHI.
