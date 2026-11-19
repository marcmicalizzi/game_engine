# R4: render graph aliasing and async compute (design, 2026-11-18)

*Batch 69, a design row written while the batch's gate held the machine: nothing was built, run or flown for it. Every byte below is computed from the formulas in the code at cad063f8 (`systems/renderer/src/scene_renderer.cpp`, `gpu_scene.cpp`, `domain/gfx/src/render_graph.cpp`); every millisecond is read from a page that measured it, named where it is used; a number that needs a run is marked **to measure** with the command an implementing row runs.*

Roadmap R4, open since Phase 1 ([plan 11 §11.7](../plan/11-performance-principles.md#117-gpu): "Async compute for acceleration-structure builds, denoiser preparation, and streaming decompression, scheduled by the render graph"; [plan 04 §4.2](../plan/04-renderer.md#42-frame-architecture): "render graph (automatic barriers, transient aliasing, async-compute scheduling)"). The graph's declarations are trustworthy since batch 68 (R184: a declared write names the read), which is what both halves of this row stand on.

## The answer in six lines

1. **The graph owns no memory today.** The renderer creates no graph transient at all: every buffer and image a frame touches is made by the renderer or the scene at load or resize and *imported* into each frame's graph (86 imports in `record_frame`), because the graph's own transients are allocated at every `compile()` and freed at every `reset()`, which the frame loop's no-allocation rule forbids.
2. **Two pixel targets are made and never touched on the defaults.** Under traced shadows occlusion is off and the Hi-Z's LOD rule is off by default, so the Hi-Z pyramid is never built or read; the depth image is read only by the direct path and by the resolve's pixel classes, which are off by default. That is **74 MiB at 3840×2160 and 222 MiB at 11520×2160** under `--raster rt --shadows rt`, given back by making them only when a mode reads them — more than aliasing returns anywhere on today's frame.
3. **Aliasing returns tens of megabytes, not hundreds.** A visibility-buffer frame has almost no full-screen transients: the visibility buffer lives the whole frame and is read after it, and the picture is shaded straight from it. What has a lifetime shorter than the frame (the shadow cascades' runs, the half-rate answers, the shadow atlas, the frame passes' linear image, the UI layer, the structure builds' scratch) shares **nothing on the owner's 4K rt/rt frame, at most the build scratch (2 to 60 MiB) when a frame pass or the half-rate pass is on, and at most about 74 MiB on hw/csm at 4K with a frame pass and the HDR overlay** — up to 3.5 % of a 4K village walk's 2,135 MiB, under 1 % of an 8 GB card. Frostbite's frame graph saved 570 of 1,042 MB at 4K because a deferred frame is mostly transients; this one is not.
4. **The pair-sized working set is where the frame's memory is**, and its waste is sizing, not overlap: every run of the visible list is as long as the pair stride. At the 2^24 pair cap one view with four cascades holds 896 MiB of visible list alone. That is a separate row (proposed below), not aliasing.
5. **Async compute has nothing to overlap inside an rt/rt frame**, the owner's performance target: the chain (cull → pool → cluster structures → bottom → top) is on the critical path to the primary trace, and everything after the trace reads it. It has one good candidate in `--raster hw --shadows rt`: the chain's builds beside the hardware raster, NVIDIA's own example pairing, a bound of the smaller of the two (8.8 ms at 11520×2160 on the dense forest, R135). Across frames it would need the frame's one visible list, its pool and its chain doubled.
6. **Recommendation**: build the give-back first (S), then the graph's lifetimes as a report (S), then a persistent transient heap for buffers with an aliasing barrier and a poison mode (M) — cheap, and the poison mode turns an undeclared access into a visible fault, which is R186's guard on a second axis; images when post and denoise passes arrive. Async compute: one measured experiment on the hw-raster, traced-shadow frame after the heap; decline it on rt/rt within the frame, decline cross-frame overlap until the working set is sized by what survives, and never on Maxwell.

## 1. The graph as it is

**`RenderGraph` v0** (`render_graph.h`, `render_graph.cpp`): passes declare uses with an `Access`; `compile()` validates that every read has a producer, allocates one physical resource per transient (`create_buffer` / `create_image_2d` at compile, destroyed at `reset()`), and derives the barriers by walking passes in declaration order with one `State` per resource. A buffer barrier is a global `VkMemoryBarrier2` (no buffer, no range), an image barrier is per image with its layout. One command list, one queue; passes record in declaration order. The graph knows a transient's size, and an imported resource's size and initial state only.

**The renderer imports everything.** `grep` finds no `graph.create_buffer` or `graph.create_image` under `systems/`; `scene_renderer.cpp` imports 86 resources a frame, `cull_instances.cpp`, `accumulation_pass.cpp`, `exposure_pass.cpp`, `reconstruction_pass.cpp`, `texture_feedback.cpp` and `volumetrics.cpp` a few each. The reason is the rule that the frame loop allocates nothing in steady state ([renderer](../subsystems/renderer.md#the-frame-loop-allocates-nothing)): a graph that allocated its transients at every compile would allocate every frame. So the graph cannot alias anything today — it does not know which memory is whose — and the work of this row is as much *moving resources under the graph* as it is the allocator.

**The queues.** `Device::create` already picks a compute-only family for async compute and a transfer-only family where the device has them, and keeps `compute_queue` and `transfer_queue` (`backend/vulkan/vulkan.h`); nothing records to either. `FrameContext` signals one timeline semaphore per frame ([gfx](../subsystems/gfx.md)).

## 2. Every target a frame touches, by class

### Pixel-sized: the renderer's own (`SceneRenderer::Targets::create`, `create_*`)

MiB at the three sizes the brief names (11520×2160 is `--views surround3`, three 3840×2160 views side by side); "first write" and "last read" are passes of `record_frame` in its order (reset … cull … hardware, hiz, shadow cull … shadow raster, direct, software, the chain, trace, motion, half sun, gi, resolve classify, resolve, frame passes, overlay, frame encode, stats).

| target | format, bytes a pixel | 1920×1080 | 3840×2160 | 11520×2160 | made when | first write → last read | class |
|---|---|---:|---:|---:|---|---|---|
| visibility (`vis`) | u64 a source pixel a view | 15.8 | 63.3 | 189.8 | always | `reset` → `resolve`, the frame passes, texture residency; read again after the frame by a capture (`read_last_frame`), and by the motion pass's copy | **whole frame and after: never aliased** |
| Hi-Z pyramid and coverage (`hiz`) | f32, mip 0 at the source size, about 5.33 B a pixel | 10.6 | 42.2 | 126.7 | always | `hiz` → the **next** frame's `cull` (R118's "last frame's") and this frame's `half sun`, `gi sample`, `resolve` | **history when occlusion is on; never built nor read under traced shadows on the defaults** |
| depth (`targets.depth`) | D32, 4 | 7.9 | 31.6 | 94.9 | always | the direct path's depth; or `resolve classify` (clear) → `resolve` with `renderer.resolve.classes`; a census reads it back | **unused on the defaults** except `--raster direct` |
| half-rate answers (`half`) | u32 a 2×2 block, 1 | 2.0 | 7.9 | 23.7 | at the first frame that records the pass (R126's rule: at 2.5:1 and wider) | `half sun` → `resolve` | transient |
| shadow atlas | D32, `shadow_map`² × cascades (2048, 4) | 64.0 | 64.0 | 64.0 | with `csm` | `shadow raster` (clear) → `resolve` | transient |
| frame passes' colour (`linear_`) | RGBA16F, 8 | 15.8 | 63.3 | 189.8 | with a frame pass or the HDR overlay | `resolve` → frame passes → `frame encode` | transient |
| UI layer | RGBA8 sRGB, 4 | 7.9 | 31.6 | 94.9 | HDR with the overlay | `overlay` (clear) → `frame encode` | transient |
| colour target | the display's, 4 (SDR, HDR10) or 8 (scRGB) | 7.9 | 31.6 | 94.9 | offscreen only (else the swapchain's) | `frame encode`/`resolve` → the capture's copy | imported, read after the frame |
| motion and previous visibility | u32 + u64, 12 | 23.7 | 94.9 | 284.8 | `renderer.reconstruction.inputs` (off) | `motion` → the reconstruction; the copy is next frame's | history |
| accumulation | two RGBA16F, 16 | 31.6 | 126.6 | 379.7 | `renderer.reconstruction.accumulation` (off) | across frames | history |

### Fixed-size and scene-sized

| resource | size | class |
|---|---|---|
| the world-space cache (`gi_cache_`) | `renderer.gi.cache_mib`, 128 MiB by default, plus the guides when `renderer.gi.guide` | history (folded every frame, read by the next) |
| the sky's tables | transmittance and multiple scattering once a scene; sky-view, aerial, frame every frame (small) | the per-frame ones are transients by lifetime, kilobytes |
| parameter, resolve, statistics blocks | a few kilobytes a frame slot, host-visible | per slot, read by the host: never aliased |
| the volumetrics' froxels and clouds (`systems/volumetrics`) | cells × 8 B × views × grids; the clouds' quarter-resolution image | read by the next frame's medium where it accumulates (**to read** against `volumetrics.md` before any move) |

### Pair-sized: the scene's working set (`GpuScene::create_working_set`)

These scale with `pair_stride_` (the pair count, or a capacity above it for a streamed world), `V` views and `C` cascades, not with pixels. Bytes a pair, and MiB at the engine's largest scene (1.4 M pairs, [cull-by-instance](cull-by-instance.md)) and at the cap (2^24 = 16.78 M):

| buffer | bytes a pair | rt/rt, one view: 1.4 M / cap | hw/csm (C = 4), one view: 1.4 M / cap | lifetime |
|---|---|---:|---:|---|
| visible list | 8 × (3V + C) | 32.0 / 384 | 74.8 / 896 | the view runs: `cull` → `trace`, `gi sample`, the chain's records; **the cascade runs: `shadow cull` → `shadow raster`** |
| pair to entry | 4 × V | 5.3 / 64 | 5.3 / 64 | `cull` → `resolve` |
| occlusion flags, two | 8 × V | 10.7 / 128 | 10.7 / 128 | history (the previous frame's set is read) |
| pair table | 8 | 10.7 / 128 | 10.7 / 128 | scene data |
| cross-fade (coverage scenes) | 8 × (V + 1) | 21.4 / 256 | 21.4 / 256 | `cull` → `resolve` |
| vertex path's records and indices | (2V + C) × record, and an index budget | baseline tier only | | `cull`/`expand` → the draw: transient |

The village's `frame` block of the memory report — these plus the deformed-vertex pool, its tables, the joints and the flora's poses — is **449 MiB** at 3840×2160 under rt/rt ([R81](rt-structures-memory-2026-11-17.md), "What a card with 8 GB and with 4 GB holds"). The report does not split it further; stage 1 below adds that split.

### The traced chain's per-frame part

Built every frame from the visible list: the frame's cluster sets (72 MiB on the village, 276 on the streamed forest once R81's guess is given back), the records and tables, the bottom-level set, and the build **scratch** (1.9 MiB on the village, 59.9 on the forest). Lifetimes: the sets and the bottom and top levels from `clas` to the last reader of the top level (`resolve`'s shadow rays, `gi sample`); the scratch from `clas` to `tlas` only.

### Totals by class, 3840×2160, one view, defaults

- **Whole frame or longer** (never aliased): visibility 63.3, the world-space cache 128, the colour target 31.6 when offscreen, the per-slot blocks, the chain's frame sets and levels (≥ 72 on the village), the pair-sized buffers that live from the cull to the resolve.
- **Made and never touched**: Hi-Z 42.2 and depth 31.6 on rt/rt (**73.8 MiB**); depth 31.6 on hw/csm.
- **Transients by lifetime**: none on rt/rt at 16:9 with no frame pass; half 7.9 where R126's rule turns it on; atlas 64 and the cascade runs (42.7 at 1.4 M pairs) on hw/csm; linear 63.3 and the UI layer 31.6 when those features are on; scratch 1.9 to 60.

## 3. What could share memory, and what it gives back

Two resources can share when neither's [first write, last read] interval holds a pass of the other's, both are made and freed by the graph, and their memory types allow it. The intervals above give these pairings, by my reading of `record_frame`; a resource read and written by the *same* pass (the resolve reads the atlas and half and writes the linear image) cannot share with it.

| frame | what dies | what is born after it | shares, MiB at 1080p / 4K / 11520 |
|---|---|---|---|
| rt/rt, 16:9, SDR, no frame pass (the owner's 4K target) | scratch at `tlas` | nothing that is not whole-frame | **0 / 0 / 0** |
| rt/rt at 2.5:1 (11520, R126's half on) | scratch at `tlas` | half at `half sun` | — / — / ≤ min(23.7, scratch): **1.9 on the village, 23.7 on the forest** |
| rt/rt with a frame pass (exposure, volumetrics) | scratch at `tlas` | linear at `resolve` | ≤ min(linear, scratch): 1.9 to 15.8 / 1.9 to 60 / 1.9 to 60 |
| hw/csm with a frame pass (no chain is built: no scratch) | the cascade runs at `shadow raster` (split out of the visible list) | linear at `resolve` | ≤ min(linear, runs): 15.8 / **42.7** / 42.7 at 1.4 M pairs |
| hw/csm, HDR with the overlay | the atlas at `resolve` | the UI layer at `overlay` | 7.9 / **31.6** / 64 |

**The largest sum on today's frame is about 74 MiB** (hw/csm at 4K with a frame pass and the HDR overlay, at the engine's largest scene's 1.4 M pairs), and **at most 24 MiB on rt/rt** (11520, the half-rate answers over the forest's scratch). Against the village walk's **2,135 MiB** on the device at 3840×2160 after R81 that is 3.5 % and 1.1 %; against an **8 GB card** (the 4070 laptop) 0.9 % and 0.3 %. At the pair cap the cascade runs alone are 512 MiB, but a scene at the cap has 896 MiB of visible list, and its problem is the list's sizing (§6, the proposed row).

**What the defaults allocate for nothing is larger**: making the Hi-Z only under occlusion or the Hi-Z LOD rule (`hiz_lod`: `renderer.lod.occluded_scale` > 0, default 0) and the depth image only for the direct path or the pixel classes gives back **18.5 / 73.8 / 221.6 MiB** on rt/rt and **7.9 / 31.6 / 94.9** on hw/csm at the three sizes, with no change to the graph. That is stage 1.

**Why a deferred engine's numbers do not carry over.** Frostbite's frame graph (Yuriy O'Donnell, *FrameGraph: Extensible Rendering Architecture in Frostbite*, GDC 2017, the slides on slideshare.net/DICEStudio, read through a text extraction): "Non-aliasing memory layout (4K, DX12 PC)", "1042 MB total"; "Aliasing memory layout (4K, DX12 PC)", "472 MB total", "570 MB saved"; at 720p 147 MB without aliasing and 76 to 80 MB on the three platforms. Those are G-buffer layers, lighting buffers and a post chain, each full-screen and alive for a few passes. The visibility buffer has none of them: the material is evaluated in the resolve from 8 bytes a pixel and written once ([plan 04 §4.2](../plan/04-renderer.md#42-frame-architecture), step 4's "compact GBuffer" was never built). The transients this frame will grow — a temporal reconstruction at an internal resolution ([temporal-sampling](temporal-sampling.md)), denoisers (step 6), bloom and depth of field (step 8) — are the deferred kind, and that is when aliasing's returns arrive: the machinery is cheap to have in place before them.

## 4. What the graph needs

**Lifetimes from the declarations.** `compile()` already walks every use in pass order; it records each resource's first and last pass. That is all aliasing needs from the graph, because since R184 a declared use is the truth about what a pass's barriers order — and *only* about that (below).

**An allocator for transients that does not allocate in the frame.** VMA (v3.4.0, `vk_mem_alloc.h`, the page "Resource aliasing (overlap)", read from the fetched source in a build tree) gives the pieces and no policy: one allocation sized and aligned to the largest of the resources and with `memoryTypeBits` the AND of theirs ("allocation size = max(size of each image)", "allocation alignment = max(alignment of each image)", "allocation memoryTypeBits = bitwise AND(memoryTypeBits of each image)"); `VMA_ALLOCATION_CREATE_CAN_ALIAS_BIT` so a dedicated block stays aliasable; `vmaCreateAliasingBuffer2` / `vmaCreateAliasingImage2` to create and bind at an `allocationLocalOffset`. The design:

- **A persistent heap the graph keeps across frames** (`TransientHeap`, owned by the graph object the renderer already keeps alive): one `CAN_ALIAS` allocation for buffers, and later one for attachment images, sized at the renderer's create and resize from the largest plan any frame shape has needed. Each frame's `compile()` packs that frame's transients into it by first-fit on offsets over the intervals, in pass order (deterministic: the same passes give the same offsets). A frame whose plan does not fit grows the heap at that frame, counted as an allocation; `engine_view.frame_loop` still asserts that the second flight makes none.
- **Resources are created once per (offset, description) and cached**, like the attachment views are now, so a steady frame creates no `VkBuffer` either.
- **Buffers with buffers, attachments with attachments.** VMA warns that "Some GPUs may expose multiple memory types suitable e.g. only for buffers or images with COLOR_ATTACHMENT usage", and mixing linear buffers and optimal images in one block brings `bufferImageGranularity` into the offsets; NVIDIA's guidance prefers dedicated allocations for attachments "especially on pre-Turing GPUs" (*Vulkan Dos and Don'ts*, NVIDIA developer blog, 2019, updated 2025). Two heaps avoid both questions; the shadow atlas and the linear image are the only attachment candidates today.

**What the first use of an aliased resource must do.** VMA's page: "treat a resource after aliasing as uninitialized - containing garbage data", and for an image "an image memory barrier ... with `oldLayout` = `VK_IMAGE_LAYOUT_UNDEFINED`". So:

- the graph emits an **aliasing barrier** at the hand-off: source the previous occupant's last stage and access, destination the new resource's first, plus `UNDEFINED → layout` for an image. Today's `State` is per resource; the heap keeps a per-range "last occupant" so the barrier names the right source;
- **the first use must be a write that covers what is read later**: a clear (`TransferWrite`, a `LoadOp::Clear` attachment) or a full overwrite. A first use that reads — `ComputeReadWrite`, `FragmentReadWrite`, an atomic max into the visibility buffer, `LoadOp::Load` — is refused at compile unless a clear is declared before it. The graph cannot see coverage; it can see the access kind, and the poison mode below catches the rest.

**What cannot alias**, as a rule the graph enforces by construction (only `create_*` transients are packed; these stay imported):

- histories: the Hi-Z when occlusion is on, the occlusion flags, the world-space cache, motion's previous visibility and pool, the accumulation, exposure's state, the volumetrics' accumulated medium;
- anything read after the frame: the visibility buffer (captures, `read_last_frame`), the depth image when a census reads it, the colour target, the statistics and feedback blocks the host reads, the page feedback;
- anything a frame in flight still reads: the table sets, the record staging, the joint and pose regions per slot;
- the chain's sets and levels while the ray-tracing capacity policy owns them (R81, [ADR-0068](../adr/0068-the-traced-chain-holds-what-frames-asked-for.md)); its scratch can alias.

**Frames in flight.** Today every imported per-frame target (the visibility buffer, the visible list) is one buffer for all frames in flight, and that is safe because one queue executes submissions in order and each frame's first barrier waits on the last. A transient heap shared by frames in flight is safe on the same terms. It stops being safe the day a frame's passes run on two queues across a frame boundary (§5): then the heap is per slot.

**The fault it turns from invisible to visible.** A pass whose shader reads a buffer through a device address it never declared (the class R186 audits this batch) is ordered today by accident: the buffer is its own memory, so a missing barrier reads values that are late or early but *of that resource*, and on most drivers in order anyway. Aliased, the same undeclared read meets **another resource's bytes**. That is worse and better: worse because an undeclared reader of an aliased transient is now always wrong, better because it is wrong loudly. The design makes it loud on purpose:

- a **poison mode** (`renderer.graph.alias_poison`, debug and the tests): at every hand-off the graph fills the new occupant with a pattern no pass writes (all ones: a NaN in every float, a maximal visibility word), so a read that happens before the declared first write, or that the graph does not know about, changes the picture;
- the cases capture each scene **with aliasing off, on, and on with poison**, and assert the three byte-identical (§7). An undeclared access to a transient fails the third.

What poison cannot see: an undeclared read of a resource that is *not* a transient (an imported history read through an address), which stays R186's guard's to catch; and a read of bindless images, which no declaration names.

## 5. Async compute

### What does not depend on the frame's raster

Read from `record_frame`'s declarations (the passes' reads and writes, §2's intervals):

| work | depends on | read by | inside the frame, beside what |
|---|---|---|---|
| the chain: records, the cache's passes, `clas`, `blas`, `tlas` | the cull's visible list, the pool, the tables | `trace` (rt raster), `gi sample`, the resolve's shadow rays | **the hardware raster, Hi-Z, software raster** when primaries are rasterized; **nothing** when they are traced |
| the deformed-vertex pool (`deform cache`, `deform alloc`, `deform`) | the cull | every rasterizer, the chain, the resolve | nothing: everything after it reads it |
| the world-space cache (`gi sample`, `gi fold`) | **this frame's visibility buffer** (it samples pixels) and the chain | the resolve | nothing in the frame; only the resolve of a *later* frame if it read last frame's cache, which changes the picture |
| the water's wind (`water wind`) | the host's upload | the materials' readers | it is a transfer of half a kilobyte a body: nothing to gain |
| the sky's per-frame tables | the frame's hour | the resolve, the ray path | 0.01 to 0.04 ms (R135): nothing to gain |
| page and terrain uploads | the host | the cull and everything after | the transfer queue's, not compute's; already off the frame's path when the staging ring is ahead |

So **an rt/rt frame is a chain from the cull to the trace with nothing beside it**: the primary rays need the top level, the cache needs the primary rays' visibility, the resolve needs both. What could overlap is **the next frame's front** (its cull, pool and chain, 8.4 ms at 3840×2160 on the forest: cull 0.4, deform 1.4, `rt` 6.6 — R135's table) beside **this frame's tail** (the resolve, 11.8 ms there, and the frame passes). That needs the next frame's visible list, pair-to-entry table, pool, chain sets and argument blocks to be other buffers than this frame's resolve reads: the `frame` block and the chain's sets twice, **about +0.5 GB on the village** (449 + 72 MiB) and more on the forest. That is the opposite of what this row's other half is for.

A frame with **rasterized primaries and traced shadows** (`--raster hw --shadows rt`) has the one textbook pairing: after the cull and the pool, the hardware raster (graphics: mesh shaders, the visibility buffer's 64-bit atomics) and the chain's builds (compute) read the same visible list and pool and write disjoint things, and the join is before `half sun`, `gi sample` and the resolve. R135 measured on the dense forest at 11520×2160: `hw` 8.8 ms and the chain 9.9 ms (11.0 under rt; `clas` 6.3); R127 the mesh path 5.3 ms at 3840. **The bound is the smaller of the two, 8.8 ms of an 83.2 ms frame at 11520 and about 5 at 3840**; the return is what the vendors say it is only after measuring, below.

### What the vendors and the literature say

- **NVIDIA, *Advanced API Performance: Async Compute and Overlap*** (Vladimir Bondarev and Sriharsha Niverty, NVIDIA developer blog, 2021-10-22): "build acceleration structure is a compute workload and is fairly easy to move onto the async queue", with the example "Move 'A' (Build Acceleration Structure) to async compute over 'B' (shadow mapping and gbuffer)"; pair by unit throughputs, not occupancy alone ("Don't only focus purely on the SM warp occupancy, start by looking at unit throughputs"); "Don't overlap workloads with high L1 and L2 usage and VRAM throughput metrics"; "Don't use long async compute workloads unless they can finish comfortably before the dependency on the sync queue"; "Don't overlap RTCore workloads"; "Inter-frame overlap can provide substantial perf gains" … "provided the application is able to invoke Present() from the compute queue"; "Verify the behavior across different GPU tiers"; and "Low resolution usually means less pixel warps and thus more idle time for the SMs". The only architecture it names is Ampere.
- **NVIDIA, *Vulkan Dos and Don'ts*** (NVIDIA developer blog, 2019-06-06, updated 2025-01-14): "Don't overlap compute work on the graphics queue with compute work on a dedicated asynchronous compute queue", which "may lead to gaps in execution"; "Each vkQueueSubmit() has a significant performance cost on CPU"; "VkSharingMode is ignored by the driver, so VK_SHARING_MODE_CONCURRENT incurs no overhead".
- **Frostbite's frame graph** (O'Donnell, GDC 2017, as above): async compute "Opt-in per render pass", a "Sync point at first use of output resource on another queue", "Resource lifetimes automatically extended to sync point", a "Memory increase", and it "Can hurt performance if misused". The same graph that aliases schedules the second queue, and the second costs the first memory.
- **Maxwell and Pascal.** Ryan Shrout, *The GeForce GTX 1080 8GB Founders Edition Review*, PC Perspective, 2016-05-17, page 2, from NVIDIA's briefing: "In Maxwell, this load balancing had to work with a fixed partitioning model", which turns "any shift in the workloads" into "unwanted idle time"; Pascal adds "dynamic load balancing". NVIDIA's own GP104 whitepaper was **not read** (not found in the search); AnandTech's review of the same card no longer serves its pages. AMD's *Asynchronous Compute Deep Dive* (GPUOpen, GDC 2017) was fetched but its text could not be extracted: **not read**.

### On the project's cards

- **RTX 5090** (Blackwell) and **RTX 4070 laptop** (Ada): both after Ampere, where NVIDIA calls compute over compute "very efficient"; the laptop has fewer SMs and runs lower resolutions, where the guidance expects more idle SMs. Both carry the chain. Whether the hardware raster leaves the units the builds want idle depends on what the raster is limited by: its 64-bit atomics are L2 traffic, which the guidance says not to overlap with more memory traffic, and the builds read the geometry. **To measure**, both cards, as stage 5 says.
- **Titan Xp** (Pascal, the GPU server): dynamic balancing, no RT cores, no mesh shaders. Whether it carries a chain at all is its capability tier's answer; a frame with no chain has nothing large to move. Not a target.
- **Titan X** (Maxwell, the baseline tier): fixed partitioning; no chain. **Async compute off**, by rule.

### What cross-queue ordering costs here

- **Submissions**: a frame becomes at least three (graphics up to the fork, the compute queue's, graphics after the join) where it is one; NVIDIA prices each on the CPU, and this frame's recording is already 4 to 7 ms on the forest (R135).
- **A timeline semaphore** at the fork and at the join (Frostbite's "sync point at first use"); a resource written on one queue and read on the other is `CONCURRENT` (free on NVIDIA's driver, as read) rather than a queue-family ownership transfer.
- **The barriers**: the graph's buffer barriers are global memory barriers in one command buffer; across queues they become semaphore waits with stage masks, and the graph must split its passes into per-queue command buffers in declaration order. The tie rule makes the visibility buffer's content independent of execution order, so the picture does not depend on the overlap.
- **The timers**: `GpuTimer` zones are one queue's timestamps. Two queues need a pool per queue and a frame span measured from the first timestamp to the last; the frame record's `gpu_ms` stops being a sum of buckets (the overlap is exactly the difference), and every comparison against an older record must say so.
- **The hazard class**: on one queue an undeclared read is ordered by luck more often than not; across queues it is a race every frame. R186's guard is a prerequisite.

### Can the engine's one-origin, one-list frame take it?

**Inside a frame, yes.** The frame's one origin (`gfx::FrameEye`) is in each frame's own parameter blocks, so two queues read the same eye. The one visible list is written by the cull and only read after; the chain and the hardware raster both read it. Two details: a buffer the graph tracks as one resource is serialized as one, so the shadow cascades' runs (written by `shadow cull` into the same list) must be recorded before the fork or split into their own buffer — but traced shadows and cascades are exclusive, so the hw/rt frame has neither a cascade run nor a second occlusion pass (occlusion is off under traced shadows). **Across frames, not without doubling** the list, the pool, the pair-to-entry table and the chain's sets, as above; and the Hi-Z history the next cull reads is written early in this frame, which is the one cross-frame edge that already works.

### Recommendation

- **Decline async compute inside the rt/rt frame**: there is no work beside the chain to overlap, and that frame (3840×2160, traced primaries and shadows, the bounce on) is the one the owner judges performance by.
- **Build one experiment**: the chain on the compute queue for `--raster hw --shadows rt`, after the transient heap (stage 5), measured as a pair on the 5090 and on the 4070 laptop; keep it only where it returns at least a measured millisecond at 3840×2160 on the forest staging, and off by a capability rule otherwise.
- **Decline cross-frame overlap** until the pair-sized working set is sized by what survives (§6): it would double the largest per-frame buffers to win back at most the chain's 6.6 ms at 4K, and it needs the next frame's CPU recording ready a frame earlier.
- **Never on the baseline tier.**

## 6. Proposed, unnumbered: the working set sized by what survives

Every run of the visible list is as long as the pair stride, "as many entries as any one draw can produce" (`create_working_set`'s comment), and R173 measured about 1 % of pairs surviving the cull. At the cap one view with four cascades holds **896 MiB** of visible list and three views **1,664 MiB**, against the 64 MiB a 4K frame's visibility buffer needs. A run sized by a capacity kept a step above what recent frames appended — the traced chain's rule since R81 ("a guess is given back") — with the frame that overflows drawing what fits and counting what it dropped, returns more than every aliasing pairing in §3 together on any scene past a few million pairs. It is the cull's and the scene's row, not the graph's.

## 7. Stages, in build order

Every stage keeps the picture byte-identical: captures of color, ids, depth and normals (`render.capture`, or `engine-view --capture` with the channels) on **the village at 3840×2160 under `--raster rt --shadows rt` with the bounce, and the oasis under `--raster hw --shadows csm`**, before and after, and the existing renderer cases below untouched. Each measures with the flight R81 used: `engine-view --offscreen --width 3840 --height 2160 --raster rt --shadows rt --tunable renderer.gi.mode=bounce --scene content/test-scenes/village/<scene>.json --camera-path <its path> --benchmark <out.jsonl> --memory-report`, main's copied-out release build against the branch's in one lease, and the same with `--raster hw --shadows csm`, at 1920×1080 and 11520×2160 (`--views surround3`) as well.

| # | stage | effort | measures | pins |
|---:|---|:-:|---|---|
| 1 | **Give back what no pass reads**: the Hi-Z made only under occlusion or `hiz_lod`, the depth image only for the direct path or `renderer.resolve.classes`, both made lazily at the frame that first needs them (as `half` is), freed at a settings change; and a `targets` block in the memory report (the renderer's own pixel targets, the world-space cache, the atlas) with the `frame` block split by buffer | S | `gpu_memory_used_mib` and the new block, before and after: expect −73.8 MiB at 4K rt/rt and −221.6 at 11520 (**to measure**) | the renderer's capture, visibility, occlusion, Hi-Z LOD and resolve-class cases unchanged; a new case: a renderer created for rt/rt holds no Hi-Z or depth bytes and draws the same ids as one that does |
| 2 | **Lifetimes, reported**: `compile()` records each resource's first and last pass; a plan of which transient-eligible resources could share, by first-fit, deterministic; printed by the memory report as `graph.alias_mib` with the pairings. No memory changes | S | the report at the three sizes on both frames, against §3's table | `domain/gfx` render-graph cases: intervals and the plan pinned on synthetic graphs; an imported resource and a history are never planned; the plan is the same for the same passes |
| 3 | **A transient heap for buffers**: the persistent `CAN_ALIAS` heap, buffers created once per offset, the aliasing barrier, the refusal of a read-first first use, the poison mode; moved under it: the shadow cascades' runs (split out of the visible list, with their argument block), the structure builds' scratch, the half-rate answers, the vertex path's indices. `renderer.graph.alias` on by default once the pins hold | M | memory as stage 1; frame time as a pair (the aliasing barriers are new and must cost under the noise); `frame_loop`'s allocation count | captures byte-identical with aliasing off, on, and on with poison, on both frames; `engine_view.frame_loop` (no allocation in the second flight); `vertex_path` and cascade cases; a graph case that a first-use read without a declared clear is refused |
| 4 | **Attachment images**: a second heap for the atlas, the linear image, the UI layer and the depth when the direct path makes it; `UNDEFINED` first layouts; the `memoryTypeBits` check at create, falling back to separate allocations where it fails | M | as 3; and the same pair on the Titan Xp server for the pre-Turing guidance (`tools/remote-build.ps1 -Host <the server's alias> -Test -Filter renderer`) | as 3; the HDR overlay's case (`overlay: on HDR10 its white is paper white…`) byte-identical |
| 5 | **The chain on the compute queue** for `--raster hw --shadows rt`: per-queue command buffers, a fork after the pool and a join before `half sun`, `gi sample` and the resolve, timeline semaphores, `CONCURRENT` buffers, a timer pool per queue and the frame's span in the record; behind a tunable, off on any device without a compute-only family and on the baseline tier | M | the pair at 3840 and 11520 on the forest staging and the village, on the 5090 and (the owner's) the 4070 laptop, with an Nsight GPU Trace of one frame each for the unit throughputs NVIDIA says to read first | the ray-query and cluster structure cases (traced visibility word for word, KHR and cluster pixel-identical) and the shadow cases, with the queue on and off |

**Stages 1 and 2, built** (batch 69, R186-2; [renderer](../subsystems/renderer.md#the-targets-a-frame-reads), [gfx](../subsystems/gfx.md)): the Hi-Z and the depth image are made only where the settings or a frame read them, at the create or at the frame that first asks, and `RenderGraph::lifetimes` with `renderer.graph.lifetimes` reports each resource's first and last pass. **What the device gave back** (debug `engine-view`, the grove over seven frames of its path under `--raster rt --shadows rt`, the device's own `gpu_memory_used_mib`; "held" is the same binary with `renderer.lod.occluded_histogram` and `renderer.resolve.classes=draws`, which make both targets as every create did before, and nothing else of size; 2026-11-18 20:43, the lease ours, other processes 100 % of the CPU, the GPU idle beside it, the session unlocked):

| size | held, MiB | lean, MiB | given back, MiB | §2's formula, MiB |
|---|---:|---:|---:|---:|
| 1920×1080 | 583 | 573 | 10 | 18.5 |
| 3840×2160 | 735 | 660 | 75 | 73.8 |
| 11520×2160 | 1,074 | 850 | 224 | 221.6 |

At 1080p the two targets are small enough for VMA to place inside blocks the device already counts, so the device's figure moves by less than their bytes; at 4K and wider each is an allocation of its own and the device's figure is the formula's. Not built of the two: the memory report's `targets` block and the `frame` block's split (the renderer's `hiz_bytes()` and `depth_bytes()` say what it holds), and stage 2's sharing plan (`graph.alias_mib`).

What a later stage takes up: post and denoise passes made as graph transients from their first commit (plan 04 §4.2 steps 6 and 8, and the reconstruction's internal-resolution targets), which is where §3's percentages grow toward Frostbite's.

## 8. What this design declines, and why

- **Aliasing histories or anything read after the frame**, at any saving: the visibility buffer is the frame's identity for every capture and census.
- **Aliasing across frames in flight** while one queue orders them: it gains nothing a per-frame heap does not.
- **A general allocator for variable-sized transients** (a buddy or TLSF heap): first-fit over one frame's intervals in a heap sized by the largest plan is enough for the handful of transients this frame has, and VMA's virtual allocator is there if that changes.
- **Async compute inside an rt/rt frame and across frames**, for §5's reasons.
- **Moving the world-space cache's update to read last frame's visibility** so it could overlap the resolve: a frame of lag in the indirect light changes the picture.

## 9. The owner's questions

1. **Async compute only serves rasterized primaries with traced shadows.** You judge performance with traced primaries. Should stage 5 be built at all before a reason to fly `--raster hw --shadows rt` exists? *Leaning*: not before stages 1 to 3, and then only as the measured experiment, kept where the pair returns a millisecond at 4K.
2. **Cross-frame overlap** (the next frame's chain beside this frame's resolve, the only async win on rt/rt) costs about half a gigabyte on the village and more on the forest. *Leaning*: decline until the working set is sized by what survives; ask again with that row's numbers.
3. **Aliasing machinery now, when it returns tens of megabytes.** *Leaning*: yes for stages 1 to 3: stage 1 alone returns more than aliasing, stage 2 is a report, and stage 3's poison mode is a second guard on the fault class this batch is closing; images (stage 4) when the first post or denoise pass lands.

## 10. What was not read or measured

- No flight, capture or memory report was taken for this page (the gate held the machine). Every MiB is a formula's; the `frame` block's split, the per-pass timings under hw/rt at 3840 on today's main, and every async return are **to measure** by the stages.
- The volumetrics' froxel grid and whether its medium accumulates across frames were not read; it is not moved by any stage until it is.
- NVIDIA's Pascal whitepaper and AMD's GDC 2017 deep dive were not read (above); nothing here rests on them.
- `bufferImageGranularity` and the memory types on the four cards were not read from the devices; stage 4 reads them at create.
