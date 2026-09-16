# Working in this repository

This file is for every contributor, human or agent. It is short on purpose; the detail lives in the linked documents.

## Where things are

| Path | What |
|---|---|
| `docs/plan/` | The technical plan. Start at `docs/plan/README.md`. It is the source of intent for everything below. |
| `docs/adr/` | Architecture decision records. Numbered, immutable once accepted. Reverse a decision with a new ADR that supersedes the old one. |
| `docs/subsystems/` | One page per module: purpose, owned data, invariants, public API, how to test. |
| `schemas/` | IDL source of truth (document types, protocol, events, components). `schemas/README.md` documents the language. `tools/schemac` compiles it; generated code lives under `build/` and is never hand-edited. Add a type by editing a `.schema` file and rebuilding. |
| `core/ foundation/ domain/ systems/ apps/ game/` | Source, one directory per module, layered lowest to highest. |
| `tools/` | PowerShell 7 scripts and small C++ tools. No Python. |
| `content/` | Test scenes, golden images, migration corpus. |
| `third_party/` | Vendored dependencies and `LICENSES.md`. Permissive licenses only (ADR-0014). |

## Build, test, lint

One script drives everything, identically for humans, agents, and CI:

```powershell
tools/dev.ps1 configure [-Preset msvc-debug]   # locates Visual Studio, runs CMake with the preset
tools/dev.ps1 build     [-Preset msvc-debug]
tools/dev.ps1 test      [-Preset msvc-debug] [-Filter <regex>]
tools/dev.ps1 bench     [-Preset msvc-release] [-Filter <glob>]  # runs engine_*_bench; JSON lines in build/<preset>/bench/
tools/dev.ps1 lint                             # banned-pattern lint (also runs as a CTest test)
tools/dev.ps1 format                           # clang-format over the tree
tools/dev.ps1 modules   [-Preset msvc-debug]   # prints build/<preset>/modules.json
```

The engine as a server, and its command-line client, build into `build/<preset>/bin/`:

```powershell
build/msvc-debug/bin/engine-cli engine.methods                                  # the method catalogue
build/msvc-debug/bin/engine-cli --doc ./world --create --name World session.info
build/msvc-debug/bin/engine-cli --doc ./world doc.apply '{"commands":[...],"attribution":{"actor":"me","role":"environment","task":"t1","rationale":"why"}}'
build/msvc-debug/bin/engine-cli --doc ./world doc.undo
build/msvc-debug/bin/engine-host --stdio                                        # JSON-RPC 2.0, one request per line
build/msvc-debug/bin/engine-cli gpu.adapters                                   # Vulkan devices, extensions, capability tier
```

Every method's parameters and result are schema types in `schemas/protocol.schema`; `engine-cli schema.describe '{"type":"engine.protocol.ApplyParams"}'` explains any of them. See `docs/subsystems/apps.md` and `protocol.md`.

Presets are in `CMakePresets.json`. Debug builds carry asserts and iterator checking; `msvc-asan` adds AddressSanitizer; the release presets compile Tracy profiling zones in (`ENGINE_TRACY`). Every change must build and pass tests in `msvc-debug` before it is committed. CI (`.github/workflows/ci.yml`) builds and tests `msvc-debug`, `msvc-release`, `linux-clang-debug`, and `linux-gcc-release` on every push and pull request.

## Layering (enforced by CMake)

```
L0 core        platform, memory, containers, math, jobs, log, schema runtime, serialization, hash, time, ids
L1 foundation  io/vfs, asset-db, tunables, profiling, telemetry, calibration, scripting-host
L2 domain      gfx (RHI + render graph), physics, nav, audio, ecs, doc, protocol, ddc, geometry
L3 systems     renderer, streaming, simulation, destruction, deformation, ui, animation, ...
L4 apps        engine-host, editor, mcp-bridge, engine-cli, content-build
L5 game        per-game code
```

A module is declared once, in its `CMakeLists.txt`:

```cmake
engine_module(NAME containers LAYER core DEPS base)
engine_module_tests(NAME containers SOURCES tests/flat_map_tests.cpp)
engine_module_bench(NAME containers SOURCES bench/containers_bench.cpp)   # optional; smoke-run under CTest
engine_app(NAME engine_cli OUTPUT engine-cli SOURCES main.cpp DEPS json platform E2E_TESTS tests/cli_tests.cpp)  # executables
engine_shaders(NAME gfx_tests SOURCES shaders/fill.slang)   # .slang -> SPIR-V at build time, embedded as <shaders/fill.spv.h>
```

`engine_module()` refuses a dependency on a higher layer or on a module that has not been declared yet, so `add_subdirectory` order is lower layers first. The module graph is written to `build/<preset>/modules.json` after configure; read that rather than parsing CMake.

Module layout: `include/<module>/` holds public headers only, `src/` the implementation, `tests/` unit and property tests, `bench/` micro-benchmarks written against `foundation/bench` (`ENGINE_BENCH_ARGS`, `state.keep_running()`, `bench::keep`). Public headers are the contract; nothing outside the module includes anything from `src/`.

## Rules that are checked

- **No banned containers or ownership types in engine code.** `std::map`, `std::set`, `std::unordered_map`, `std::unordered_set`, `std::list`, `std::deque`, `std::shared_ptr`, `std::function` (in hot paths) are replaced by `core/containers`. Allowed in `tools/`, `tests/`, and cold initialization marked with `// engine-lint: allow-std-container <reason>`. `tools/lint.ps1` enforces this and runs under CTest.
- **Size table.** Every hot type has an `ENGINE_EXPECT_SIZE(Type, size, align)` entry in its module's `tests/size_table.cpp`. Changing a hot type's size means updating the entry and saying why in the commit message.
- **Warnings are errors** on every compiler.
- **No allocations in the frame loop in steady state** (measured once the frame loop exists; the allocation counter is a CI metric).
- **Layering** as above.

## Rules that are reviewed

Read `docs/plan/11-performance-principles.md` before touching a hot path. In short: data layout and footprint first; iterate in memory order; no data-dependent branches inside inner loops (template on data-selected configuration, hoist hardware-selected parameters); pinned cache-domain thread pools; no hidden limits; measure before and after with the numbers in the change description.

Read `docs/plan/12-ai-usage-policy.md` before touching anything that involves a model at development or run time. In short: agent outputs are content and code, never training data; runtime models are unmodified third-party open weights; describe caching as caching.

## Conventions

- C++20, `snake_case` for functions and variables, `PascalCase` for types, `k_` prefix for constants, trailing underscore for private members (`size_`), `ENGINE_` prefix for macros. Standard-library-style aliases (`size_type`, `iterator`) are kept on containers so agents' habits transfer. `.clang-format` is authoritative; run `tools/dev.ps1 format`.
- Headers use `#pragma once`. Includes are ordered: own header, module headers, engine headers, third party, standard library.
- No exceptions across module boundaries; error returns use `Result<T>` from `core/base` once it exists, `bool` plus out-parameter until then. Exceptions are disabled in engine targets.
- Asserts: `ENGINE_ASSERT(cond, "message")` in debug, compiled out in release; `ENGINE_VERIFY` stays in release. Never assert on external input; validate it.
- Shaders are Slang only (ADR-0008), compiled at build time by `engine_shaders()` and embedded; entry point names are preserved, so a file may hold several. Keep shader sources beside the module that owns them.
- Mesh shaders: one workgroup per cluster, geometry through device addresses, and the mesh stage must write `SV_PrimitiveID` per primitive or the fragment stage reads garbage. GPU-mirrored structs (`geometry::ClusterDesc`) are pinned by size tables and mirrored by hand in the shader.
- Raster passes flip the viewport (negative height): clip space is y-up like `core/math`, front faces are counter-clockwise in y-up space. Present through `Swapchain` + `FrameContext::end_frame(PresentSync)` + `RenderGraph::set_final_layout`; never include SDL outside `foundation/window`.
- `engine-view --frames N --capture out.png` is how to look at what the renderer draws; read the PNG.
- Shaders load through `gfx::ShaderLibrary`: embedded bytes ship, the build's `shaders/manifest.json` wins while developing, and edited `.slang` files recompile and reload in a running `engine-view`; a failing save prints slangc's diagnostics and keeps the last good shader. Reflection is read from the SPIR-V (`reflect_spirv`), so mirrored C++ structs can be checked against `push_constant_bytes`.
- Logging: `ENGINE_LOG_INFO(category, "short static message", log::field("key", value), ...)` with a category defined once per module (`ENGINE_LOG_CATEGORY_DEFINE`). Data goes in typed fields, never interpolated into the message; no `printf`-style logging in engine code; report output that is the product of a tool (the bench table, CLI output) is the exception. See `docs/subsystems/log.md`.
- Every new module gets a `docs/subsystems/<module>.md` page and a `tests/` directory in the same change.
- Run-time parameters that a kernel or system reads (batch sizes, spin counts, budgets, thread counts) are `tunables::Int/Float/Bool/Enum` objects from `foundation/tunables`, read once outside hot loops. Core-layer modules take config structs instead and the app layer fills them from tunables. Data-selected dimensions (formats, modes) are compile-time variants, never tunables.
- Commit messages: imperative subject under 72 characters, a body that says why, and the attribution trailer the harness supplies. Reference ADRs and plan sections when a change implements them.

## Recording decisions

Anything that would surprise a future contributor gets an ADR: copy `docs/adr/0000-template.md`, take the next number, fill in context, decision, consequences, and when to revisit. Accepted ADRs are never edited except to mark them superseded.
