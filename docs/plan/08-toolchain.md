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
- **One CPU baseline for the whole tree** ([ADR-0031](../adr/0031-minimum-cpu-x86-64-v3.md), [§8.9](#89-the-cpu-baseline-and-what-each-dependency-does-about-it)). `ENGINE_CPU_BASELINE` is `v3` (x86-64-v3: AVX2, FMA, BMI1/2, F16C, LZCNT, MOVBE) for every preset but `msvc-release-v2`, `linux-clang-debug-v2` and `linux-gcc-release-v2`, which are `v2` — a **test** baseline for the project's two pre-v3 GPU runners, not a second shipping tier. It is applied once, from the top-level directory scope, so the engine and every third-party library built here share it; a dependency that publishes an arch flag of its own follows it (Jolt) rather than arguing with it. AVX-512 is never a baseline and is reached only through runtime dispatch (§8.7 Stage 2, [11 §11.4](11-performance-principles.md#114-branch-free-hot-paths-and-constexpr-dispatch)).
- Single-command contract: `tools/dev build|test|bench|content|run` behaves identically for humans, agents, and CI.
- clang-format; clang-tidy with project checks; layer-dependency enforcement in the `engine_module()` CMake function; a license check driven by the dependency manifest and provenance metadata.
- **`tools/new-capability.ps1`** scaffolds a new capability — the module, the `engine_module(... OPTIONAL)` manifest and its `ENGINE_WITH_<NAME>` switch, the system skeleton with [ADR-0027](../adr/0027-additive-capabilities.md)'s registration points as a checklist in the header, tests, a size table, an optional schema and bench, the `docs/subsystems` page and its README row, and the `add_subdirectory` line — so that adding a capability is additive by construction ([02 §2.8](02-architecture.md#28-adding-a-capability)). It refuses to overwrite anything, and **its output is `clang-format` clean for a capability of any name**: every comment line and every include in the templates is interpolated with the name, so a line that fits for `cloth` overruns the column limit for `deformable_volume_field` and clang-format reflows it into the rest of its paragraph, and which include group the capability's own header belongs to depends on its layer. Both are computed rather than written out (`Split-LongCommentLine`, `Format-IncludeBlock`), because the first thing a new owner sees must not be a diff they did not write. `tools/new-capability.Tests.ps1` scaffolds into a temporary directory and checks the result — including running clang-format over every generated file with a short name, a long one and a `core`-layer capability, and asserting it changes nothing; it names the binary and version it used, and skips those checks with a note where clang-format is absent or older than 10 (which is where `--dry-run` arrived) rather than failing. No version is pinned: 19.1.5, 22.1.3 and — since the local Linux container carries Ubuntu 24.04's — 18.1.3 were all measured against this scaffold's output and agree. **The verdict is `--dry-run --Werror`'s exit code, never a comparison of clang-format's stdout with the file's text** — PowerShell decodes a native process's stdout with the console's active code page, and these templates are full of em dashes and section signs, so a text round trip reports every generated file as differing on any console that is not UTF-8, which is the Windows default and what CTest gives you. It runs under CTest as `tools.new_capability`.
- **The minimal configuration.** `ENGINE_MINIMAL=ON` (the `msvc-minimal` and `linux-clang-minimal` presets) turns every optional capability off at once; the disabled modules disappear from `modules.json` and are listed there under `disabled_capabilities`. CI builds and tests it, which is the proof that nothing in the tree depends on a capability ([11 §11.10](11-performance-principles.md#1110-absent-capabilities-are-free)).
- **The capability graph.** `engine_capability_requires(<capability> <other>...)`, at the top of a capability's `CMakeLists.txt`, says that this capability cannot be built without another one — the case [ADR-0027](../adr/0027-additive-capabilities.md)'s "revisit when" anticipated and `systems/animation` was the first to hit, because a capability that *ticks* links `domain/ecs`, which is itself optional. A capability with an unsatisfied requirement resolves **off**, says so once on the configure's status line, and is listed in `modules.json` under `disabled_capabilities` like any other. It never switches the required capability *on*: `ENGINE_MINIMAL=ON` has to mean what it says. Without the edge, `engine_module()` still refuses the dependency outright, which is the right answer for a non-capability module and now names the function in its error.

  **The graph has a build of its own**, because neither of the two it had exercised it: `msvc-no-ecs` and `linux-clang-no-ecs` are everything on except `ENGINE_WITH_ECS`, so `systems/animation` has to follow `domain/ecs` off through its declared edge and the rest of the tree has to build and pass without either. All-on never consults the graph and all-off switches everything off for a different reason, so a capability that links another and forgot to declare the edge passes both and fails only here. CI runs the Linux one beside `linux-clang-minimal`.
- **Status, 2026-09-19: the Linux gate runs locally.** `tools/linux-build.ps1` builds and tests the four Linux presets in a container built from `tools/ci/linux.Dockerfile` — Ubuntu 24.04 (what `ubuntu-latest` is), the apt list `ci.yml` installs, CMake 3.28, Clang 18, GCC 13, PowerShell 7 — on the Windows development desktop, in about eight minutes warm and nineteen cold. The reason it exists is written down in [local Linux builds](../ci/local-linux.md): hosted CI refused every job for about a day in September 2026, roughly a hundred commits landed that no Linux compiler had seen, and **five real defects** came out of the first four runs — four of them warnings MSVC has no equivalent for, and one a determinism test comparing two bytes of struct padding that the allocator had been zeroing by luck. A gate that exists only on somebody else's machine is a gate that can be taken away; this one costs a command. It does not replace `ci.yml` and it draws no pictures — the GPU half is still [the two self-hosted machines](../ci/self-hosted-runners.md).
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
| UV charting and packing | xatlas (pinned commit) | MIT, with BSD-3 OpenNL inside | The content build's `--atlas repack` ([atlas](../subsystems/atlas.md)); build machines only |
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
## 8.9 The CPU baseline, and what each dependency does about it

[ADR-0031](../adr/0031-minimum-cpu-x86-64-v3.md) sets the promised minimum CPU to **x86-64-v3** (AVX2, FMA, BMI1/2, F16C, LZCNT, MOVBE; Haswell 2013 and Zen and newer, 4 cores / 8 threads) with AVX-512 dispatch-only, and keeps **x86-64-v2** as a test baseline for the two GPU runners that are older than that ([self-hosted runners](../ci/self-hosted-runners.md)). `cmake/EngineCpuBaseline.cmake` puts the flag on the top-level directory before anything is fetched or added, so it reaches every target in the tree at once: `/arch:AVX2` on MSVC and clang-cl, `-march=x86-64-v3` on GCC and Clang, and nothing at all for `v2` on MSVC, where the x64 default's SSE2 plus explicit SSE4.2 intrinsics is what that baseline already means.

Applying it in one place is only half the job: a dependency with an instruction-set opinion of its own has to be made to agree, and a dependency with **runtime dispatch** has to be understood rather than configured. What each one here does, read from the pinned source at the versions `cmake/Engine*.cmake` fetch:

| Dependency | Arch options of its own | What it does about the ISA | What we do |
|---|---|---|---|
| **Jolt** 5.6 | `USE_AVX`, `USE_AVX2`, `USE_AVX512`, `USE_SSE4_1/2`, `USE_LZCNT`, `USE_TZCNT`, `USE_F16C`, `USE_FMADD` | Puts the arch flag on the `Jolt` target as **PUBLIC**, so it reaches every target that links physics, and its headers change shape with the matching `JPH_USE_*` defines | The options follow `ENGINE_CPU_BASELINE` (`cmake/EnginePhysics.cmake`). `USE_AVX512` stays off (dispatch-only, and Jolt has no dispatch); `USE_FMADD` stays off at every baseline because `CROSS_PLATFORM_DETERMINISTIC` ignores it anyway |
| **meshoptimizer** 1.2 | none | **Runtime dispatch, conditionally.** `vertexcodec.cpp` compiles an SSSE3+POPCNT path with a `cpuid` fallback when the compiler was not told it has SSSE3, and drops the fallback when it was. Everything else in the library is scalar | Nothing to set. Under v3 the `cpuid` check disappears and the SSSE3 path becomes unconditional, which is correct because v3 contains SSSE3 |
| **SDL3** 3.4 | `SDL_ASSEMBLY`, `SDL_SSE*`, `SDL_AVX`, `SDL_AVX2`, `SDL_AVX512F` | **Runtime dispatch.** The options only decide whether a path is *compiled*; each is reached through `SDL_HasAVX2()` and friends, and the per-function targeting attributes keep the flags off the target | Nothing to set. SDL runs correctly at either baseline |
| **Tracy** 0.14 | `NO_ISA_EXTENSIONS` (in `cmake/config.cmake`) | `config.cmake` does `add_compile_options(/arch:AVX2)` and `-march=native` — but only the **server-side** tools include it (`profiler/`, `capture/`, `import/`, …), and this tree builds the client library alone | Nothing to set, and nothing to fear: the client's own `CMakeLists.txt` never includes that file. Worth re-checking on a version bump |
| **flecs** 4.1 | none | Plain C, no intrinsics anywhere in `src/` or `include/` | Nothing |
| **SQLite** 3.53 | none | Plain C amalgamation, no intrinsics | Nothing |
| **Recast/Detour** 1.6 | none | Plain C++, no intrinsics | Nothing |
| **xatlas** (pinned commit) | none (`XA_MULTITHREADED`, `XA_DEBUG` and friends are not ISA options) | Plain scalar C++, no intrinsics anywhere in `xatlas.cpp` | Nothing for the ISA: compiled by `cmake/EngineAtlas.cmake` in the top-level scope, so it carries the baseline flag like the engine. `XA_MULTITHREADED=0` is set for a different reason — its own scheduler starts a thread per CPU per atlas, whatever the caller's budget ([atlas](../subsystems/atlas.md)) |
| **stb_image** (pinned commit) | none | `STBI_SSE2` is defined unconditionally on x86-64 (where SSE2 is guaranteed) and `stbi__sse2_available()` is a compile-time `1` there; the NEON paths are the other branch | Nothing. Compiled into `foundation/image`'s own translation unit, so it follows the tree's baseline |
| **Vulkan-Headers, volk, VMA, cgltf, doctest** | none | Headers only | Nothing |
| **Slang** | n/a | A downloaded compiler binary, not built here | Nothing |

Two things follow that are easy to get backwards. A dependency with **runtime dispatch** is not a dependency that needs a baseline — raising ours changes what it selects, not whether it works — so meshoptimizer and SDL need no option and no watching beyond a version bump. A dependency that publishes a **PUBLIC** arch flag is the opposite: it sets the baseline for everything downstream of it, which is how Jolt's default came to be the thing that decided the whole tree's instruction set for a year. The rule that comes out of it: **a dependency never gets to choose the baseline; it is told.**

**Status (host tools, 2026-09-22).** The baseline reaches every target that ships — and, until [ADR-0034](../adr/0034-host-tools-build-for-the-build-machine.md), it also reached the one executable the build runs on the build machine, `tools/schemac`, which is how a v3 build on the Sandy Bridge GPU server died six minutes in with SIGILL and nothing printed. A host tool now compiles for the compiler's default (`engine_host_tool()`), a v3 build on a v2 machine builds to the end and fails only at the first engine binary it runs, with ADR-0031's one line, and `build.cpu_baseline` reads `compile_commands.json` and `build.ninja` after every build to check the classification: host tools and `core/platform` carry no instruction-set flag, everything else carries all of the baseline's, and nothing the build runs is anything but a declared host tool. The configure-time refusal that stood in for this (`ENGINE_ALLOW_UNRUNNABLE_BASELINE`) is gone.

