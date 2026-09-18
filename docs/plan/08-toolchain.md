# 08 — Build, Toolchain, and Dependencies

Third-party status and licenses below were verified against upstream repositories and vendor documentation on 12 September 2026.

## 8.1 Language

**Runtime core: C++20**, with C++23 features where both MSVC and Clang support them.

Reasons: the graphics, physics, and RT ecosystem is C++; agents have their largest corpus of relevant engine code in C++; Slang, Jolt, DirectStorage, meshoptimizer, and the NVIDIA SDKs are C++-first.

Mitigations for agent-written C++, since memory safety is the primary hazard:
- CI builds with clang-cl and AddressSanitizer/UndefinedBehaviorSanitizer; warnings as errors.
- Banned-pattern linting: raw `new`/`delete`, C arrays, unchecked indexing outside audited hot loops.
- Spans and views at module boundaries; code generation for all serialization.
- Fuzz tests on every parser and protocol handler.
- Track sanitizer findings per thousand lines of agent-written code per phase.

**This is decided, not open.** C++ is the project owner's language and is as close to the hardware as the project needs. Rust was considered for compiler-enforced memory safety in agent-written code and rejected on ecosystem and ownership grounds; the mitigations above carry that weight instead. Inline assembly is not planned: intrinsics and ISPC cover SIMD, and any asm block must come with a benchmark proving the compiler could not get there.

**No C++26 static reflection.** As of September 2026 only GCC 16 ships it, behind a flag; Clang and MSVC do not. Schema code generation is the path ([02-architecture §2.5](02-architecture.md#25-repository-organization-for-agent-comprehension)).

## 8.2 Gameplay and scripting layer

Decision deferred to experiment **E7**. Candidates:

| Option | For | Against |
|---|---|---|
| C++ hot-reload modules (gameplay DLLs, unity builds) | One language, no FFI, maximum performance | Slower iteration; agents must get memory safety right |
| Luau (typed Lua, MIT, weekly releases) | Gradual typing, sandboxed, tiny, fast, built for many authors | Weak for performance-heavy systems |
| C# (.NET hosted) | Strong typing, large corpus, tooling | Runtime size, GC pauses, interop cost |

Recommendation to test: C++ for systems, **Luau for content logic** (quest conditions, dialogue logic, NPC routines), and **declarative data** for the bulk (state machines, quest graphs, behavior trees as document objects). Declarative-first is the agent-native choice: diffable, validatable, and no compiler in the loop.

## 8.3 Graphics API

Verified facts as of September 2026:

- **Cluster acceleration structures.** Vulkan has only NVIDIA's `VK_NV_cluster_acceleration_structure` (driver 570+, RTX 20-series and later); no KHR or EXT version exists. AMD is pursuing a different approach (Dense Geometry Format via a provisional AMDX extension). D3D12 specifies cluster AS, cluster templates, and partitioned TLAS in the DXR Tier 2.0 draft (spec v0.38, September 2026), with a preview expected late 2026 and a stated goal of working on existing RT hardware via driver updates. Today, cluster AS on D3D12 is reachable only via NVAPI.
- **DXR 1.2** (opacity micromaps, shader execution reordering) is retail on D3D12 since Agility SDK 1.619 (February 2026). Vulkan has equivalents as EXT/NV extensions.
- **GPU decompression.** DirectStorage 1.4 (GDeflate and zstd) is cross-vendor on Windows. Vulkan's `VK_EXT_memory_decompression` is implemented by NVIDIA only so far.
- **Neural reconstruction.** DLSS 4.5 Ray Reconstruction via Streamline (MIT framework, proprietary DLLs) on all RTX GPUs; AMD FSR Ray Regeneration on RDNA4 only; nothing shipping from Intel.

**Recommendation: Vulkan as the primary API, Windows first, Linux second.** With Linux a stated target, mobile a nice-to-have, and a small team, one backend that covers all of them outweighs the D3D12 roadmap advantage. On the project's own development hardware (RTX 5090) cluster acceleration structures are available today through the NVIDIA Vulkan extension, so the cluster-RT path can be built and measured immediately. AMD and Intel use the fallback path until a cross-vendor route exists.

This is a close call, and an earlier draft of this plan went the other way on the strength of D3D12's cross-vendor cluster-AS roadmap. What tipped it: Linux as a real target, the effort budget, Vulkan's coverage of Android for a possible mobile tier, and the fact that the fallback path is needed regardless.

Costs accepted: cluster RT is NVIDIA-only on Vulkan for now; GPU decompression is NVIDIA-only on Vulkan (CPU decompression on efficiency cores otherwise); PIX is unavailable (Nsight Graphics and RenderDoc cover Vulkan on both platforms). Windows and Linux will not have identical feature sets where a vendor extension exists on one and not the other; the capability-tier system makes those gaps explicit rather than silent.

Keep the RHI thin and API-neutral, modeled on Vulkan 1.3/1.4 concepts that map cleanly to D3D12: dynamic rendering, synchronization2, buffer device address, descriptor indexing or descriptor buffers, indirect commands, timeline semaphores. Add a D3D12 backend when DXR Tier 2.0 cluster AS ships retail *and* AMD/Intel cluster RT matters for the target audience. Experiment **E2** spikes cluster AS on both APIs in the first month of Phase 1 so the cost of a D3D12 backend is known rather than guessed.

## 8.4 Shaders

**Slang** (Khronos-hosted since November 2024, Apache-2.0 with LLVM exception, near-weekly releases): generics, interfaces, modules, reflection via C++ API and JSON, direct SPIR-V output, DXIL via DXC. Use Slang modules and link-time specialization instead of preprocessor permutations. Shader hot reload in the editor; offline compile and hash-keyed cache for shipping. The material graph compiles to Slang. Caveat: the C++ API is stable in practice but not formally frozen; pin versions.

## 8.5 Build system and CI

- **CMake + Ninja** with presets for MSVC and clang-cl; **vcpkg** manifest mode with a pinned baseline; **sccache** from day one; unity builds and precompiled headers for large modules.
- Single-command contract: `tools/dev build|test|bench|content|run` behaves identically for humans, agents, and CI.
- clang-format; clang-tidy with project checks; layer-dependency enforcement in the `engine_module()` CMake function; a license check driven by the dependency manifest and provenance metadata.
- **`tools/new-capability.ps1`** scaffolds a new capability — the module, the `engine_module(... OPTIONAL)` manifest and its `ENGINE_WITH_<NAME>` switch, the system skeleton with [ADR-0027](../adr/0027-additive-capabilities.md)'s registration points as a checklist in the header, tests, a size table, an optional schema and bench, the `docs/subsystems` page and its README row, and the `add_subdirectory` line — so that adding a capability is additive by construction ([02 §2.8](02-architecture.md#28-adding-a-capability)). It refuses to overwrite anything, and **its output is `clang-format` clean for a capability of any name**: every comment line and every include in the templates is interpolated with the name, so a line that fits for `cloth` overruns the column limit for `deformable_volume_field` and clang-format reflows it into the rest of its paragraph, and which include group the capability's own header belongs to depends on its layer. Both are computed rather than written out (`Split-LongCommentLine`, `Format-IncludeBlock`), because the first thing a new owner sees must not be a diff they did not write. `tools/new-capability.Tests.ps1` scaffolds into a temporary directory and checks the result — including running clang-format over every generated file with a short name, a long one and a `core`-layer capability, and asserting it changes nothing; it skips those checks with a note where clang-format is not installed rather than failing. It runs under CTest as `tools.new_capability`.
- **The minimal configuration.** `ENGINE_MINIMAL=ON` (the `msvc-minimal` and `linux-clang-minimal` presets) turns every optional capability off at once; the disabled modules disappear from `modules.json` and are listed there under `disabled_capabilities`. CI builds and tests it, which is the proof that nothing in the tree depends on a capability ([11 §11.10](11-performance-principles.md#1110-absent-capabilities-are-free)).
- **The capability graph.** `engine_capability_requires(<capability> <other>...)`, at the top of a capability's `CMakeLists.txt`, says that this capability cannot be built without another one — the case [ADR-0027](../adr/0027-additive-capabilities.md)'s "revisit when" anticipated and `systems/animation` was the first to hit, because a capability that *ticks* links `domain/ecs`, which is itself optional. A capability with an unsatisfied requirement resolves **off**, says so once on the configure's status line, and is listed in `modules.json` under `disabled_capabilities` like any other. It never switches the required capability *on*: `ENGINE_MINIMAL=ON` has to mean what it says. Without the edge, `engine_module()` still refuses the dependency outright, which is the right answer for a non-capability module and now names the function in its error.

  **The graph has a build of its own**, because neither of the two it had exercised it: `msvc-no-ecs` and `linux-clang-no-ecs` are everything on except `ENGINE_WITH_ECS`, so `systems/animation` has to follow `domain/ecs` off through its declared edge and the rest of the tree has to build and pass without either. All-on never consults the graph and all-off switches everything off for a different reason, so a capability that links another and forgot to declare the edge passes both and fails only here. CI runs the Linux one beside `linux-clang-minimal`.
- Tooling scripts are written for PowerShell 7 (`pwsh`, cross-platform) or built as small C++ tools by CMake, not Python, so the repo has no toolchain dependency beyond Visual Studio or Clang.
- CI: Windows GPU runners (NVIDIA and AMD); nightly extreme-resolution and reference-image jobs; replay determinism; migration corpus; sanitizer builds.
- Bazel/Buck2 were considered for hermeticity and remote caching and rejected for now because of Windows and graphics-SDK friction. Revisit if build time dominates agent iteration.

## 8.6 Dependency candidates

| Area | Library | License | Notes |
|---|---|---|---|
| Windowing, input | SDL3 (3.4.x) | zlib | |
| GPU memory, loader | Vulkan Memory Allocator 3.4; volk | MIT | D3D12MA if a D3D12 backend is added |
| Shaders | Slang (2026.x) | Apache-2.0 + LLVM exception | |
| Mesh processing | meshoptimizer 1.2 | MIT | Includes `clusterlod.h` DAG builder |
| Graph partitioning | METIS 5.2.1 | Apache-2.0 | Low activity since 2022; meshoptimizer's builder may suffice |
| Cluster LOD + cluster AS reference | nvpro `vk_lod_clusters`; RTXMG SDK 2.0 | Apache-2.0; NVIDIA RTX SDKs License | Reading material, not linked |
| ReSTIR | RTXDI 3.1 | **NVIDIA RTX SDKs License** | Proprietary: fine in a shipped game, not redistributable in an open engine. Treat as reference |
| Denoising | NRD 4.x | **NVIDIA RTX SDKs License** | Same caveat. OIDN 2.5 (Apache-2.0) for the reference path |
| Upscaling | Streamline 2.14 (MIT) + DLSS DLLs (RTX SDKs License); FSR SDK 2.3 (MIT + DLL EULA); XeSS | Mixed | |
| Image metric | FLIP 1.7 | BSD-3 | |
| Physics | Jolt 5.6 | MIT | Optional cross-platform determinism at ~8% cost |
| Navigation | Recast/Detour (main; last tag 1.6, 2023) | zlib | Tile-cache rebuild ~2 ms per tile in 2011 numbers; re-measure (E11) |
| ECS | flecs 4.1 | MIT | Evaluate (E6) |
| Persistence | SQLite | Public domain | |
| Allocator | mimalloc 3.x | MIT | |
| Profiling | Tracy 0.14 | BSD-3 | |
| Tools UI | Dear ImGui | MIT | |
| Game UI (candidate) | RmlUi 6.3 | MIT | HTML/CSS-like; decision deferred |
| Scripting (candidate) | Luau | MIT | |
| SIMD kernels | ISPC 1.31 | BSD-3 | Multi-target dispatch |
| CPU ray tracing (offline, tools) | Embree 4.4 | Apache-2.0 | Baking, validation, AI line-of-sight |
| Textures | KTX-Software/Basis; bc7enc | Apache-2.0; MIT | |
| I/O | Platform async I/O (IOCP, io_uring); `VK_EXT_memory_decompression` | n/a | DirectStorage only with a future D3D12 backend |
| Compression | zstd; meshoptimizer codecs | BSD; MIT | |
| Hashing | BLAKE3 or xxHash | CC0/Apache-2.0; BSD-2 | |
| JSON | simdjson (read); yyjson (write) | Apache-2.0; MIT | |
| glTF | fastgltf or cgltf | MIT | |
| USD interop | LightUSD 1.0-rc (formerly tinyusdz) | Apache-2.0 | Dependency-free; full OpenUSD 26.x not needed |
| Audio | miniaudio; Steam Audio 4.8 | MIT or public domain; Apache-2.0 | |
| Local LLM (Tier ≥ 1) | llama.cpp | MIT | |
| Testing | Catch2 or doctest | BSL-1.0; MIT | |
| Logging, formatting | fmt; spdlog | MIT | |

**Deliberately no external dependency**: render graph, cluster runtime pipeline, software rasterizer, job system, tunables and calibration, document model, transaction system, protocol code generation, sim scheduler, event log, LOD framework, support graphs.

Every entry above is compatible with the engine license policy in §8.8. The proprietary rows (RTXDI, NRD, DLSS/FSR DLLs) are listed as reading material or optional plugins, never as engine dependencies.

## 8.7 Hardware characterization, scaled to fit

**Stage 1 (Phase 0): detection, no benchmarking.** CPUID and `GetLogicalProcessorInformationEx` for cores, caches, cache domains (CCDs), and hybrid core classes; D3D12 feature and memory queries for the GPU. A topology-aware job system: worker pools per cache domain; a performance-core pool for sim and render; an efficiency-core pool for streaming, decompression, and inference; work stealing prefers the same domain.

**Stage 2 (Phase 1): tunables.** Every kernel registers parameters (batch size, prefetch distance, workgroup size, variant enum) with defaults, ranges, and a benchmark harness function. ISPC multi-target covers SIMD width. Parameters are read once outside hot loops; compile-time variants exist only where the inner loop's code shape changes.

**Stage 3 (only with evidence): calibration runner.** Runs registered harnesses on real engine data; stores a profile keyed by hash(CPU ID + microcode, memory configuration, GPU ID + driver, OS, engine version); adopts a non-default value only on a statistically significant gain of at least 5%; a regression guard reverts to defaults; optional low-frequency recalibration during load screens.

Trigger for Stage 3: two owned machines disagree on the best value of any tunable by more than 10% of frame time (experiment **E3**).

The intent behind hardware characterization in the brief was chiefly thread pinning and cache-domain/NUMA locality. That is all in Stage 1 and happens on day one without any benchmarking; the coding standards it implies are in [11 §11.5](11-performance-principles.md#115-threads-cores-and-memory-locality).

## 8.8 Engine license and dependency policy

**Decision: Apache-2.0.** Games built on the engine owe nothing but attribution and may be closed source; redistributors of the engine itself also preserve the NOTICE file. Apache-2.0 was chosen over MIT for its explicit patent grant, which matters for a graphics engine. Not GPL under any circumstances.

Dependency policy that follows from it:

| Dependency license | Policy |
|---|---|
| MIT, BSD, zlib, Apache-2.0, BSL-1.0, public domain | Allowed; vendored or via vcpkg |
| LGPL | Allowed only as a dynamically linked, user-replaceable library, and avoided where a permissive alternative exists |
| GPL, AGPL, SSPL, and other copyleft or source-available licenses | Not allowed |
| Proprietary SDKs (NVIDIA RTX SDKs License, DLSS/FSR/XeSS binaries, DirectStorage) | Never in the engine repository or binary. Exposed as **optional plugin modules** behind a stable interface; the game developer fetches the SDK and accepts its license. The engine is fully functional without them |
| Model weights | Only licenses permitting redistribution and commercial use; conditions reviewed individually ([12-ai-usage-policy](12-ai-usage-policy.md)) |

Consequences already reflected in this plan: ReSTIR and a baseline denoiser are implemented in-house; vendor upscalers and neural denoisers are plugins; every dependency in §8.6 is permissive. A license check runs in CI against the vcpkg manifest, the plugin manifests, and the provenance metadata of every asset.
