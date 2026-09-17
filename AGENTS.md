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

tools/new-capability.ps1 -Name cloth -Layer systems -Deps "base containers math" [-WithSchema] [-WithBench] [-WithProtocol]
```

`new-capability.ps1` scaffolds a new capability — module, system skeleton, LOD policy, determinism stance, tests, size table, docs page, and the `ENGINE_WITH_<NAME>` switch that has to be removable — as ADR-0027 requires.

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

Presets are in `CMakePresets.json`. Debug builds carry asserts and iterator checking; `msvc-asan` adds AddressSanitizer; the release presets compile Tracy profiling zones in (`ENGINE_TRACY`); `msvc-minimal` and `linux-clang-minimal` set `ENGINE_MINIMAL`, which switches every optional capability off and is the proof that nothing in the tree depends on one (ADR-0027). Every change must build and pass tests in `msvc-debug` before it is committed. CI (`.github/workflows/ci.yml`) builds and tests `msvc-debug`, `msvc-release`, `linux-clang-debug`, `linux-gcc-release`, and `linux-clang-minimal` on every push and pull request. Hosted runners have no GPU driver, so every GPU test skips there; `.github/workflows/gpu.yml` runs the same suite weekly on the two self-hosted baseline-tier machines that do, set up as `docs/ci/self-hosted-runners.md` describes.

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
engine_module(NAME cloth LAYER systems OPTIONAL DEPS base containers)     # a capability: ENGINE_WITH_CLOTH, ADR-0027
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
- Mesh shaders: one workgroup per cluster, geometry through device addresses, and the mesh stage must write `SV_PrimitiveID` per primitive or the fragment stage reads garbage. GPU-mirrored structs (`geometry::ClusterDesc`, `gfx::MeshDesc`) are pinned by size tables and mirrored by hand in every shader that reads them.
- Positions reach the rasterizers and the resolve on one 16-bit grid per mesh, three `u16` a vertex instead of three floats (`geometry::quantize_positions`, `ClusterMesh::quantized`/`quant_origin`/`quant_scale`; `gfx::MeshDesc` and the `load_position` helper duplicated in the four shaders that read positions). The builders fill it last, so a merged mesh has one grid and welded meshes stay crack-free. The float `vertices` stay for the acceleration structure builders, which still read them.
- Raster passes flip the viewport (negative height): clip space is y-up like `core/math`, front faces are counter-clockwise in y-up space. Present through `Swapchain` + `FrameContext::end_frame(PresentSync)` + `RenderGraph::set_final_layout`; never include SDL outside `foundation/window`.
- Normal cones: every `geometry::ClusterDesc` carries a cone (apex plus snorm8 axis and cutoff) and the cull pass drops backfacing clusters when `CullParams::cone_cull` is set. Counter-clockwise seen from outside is the front; build two-sided meshes with `normal_cones = false` rather than disabling the test. Culling must never change the picture, and the cone test asserts it does not.
- Ray tracing: acceleration structures (`gfx/acceleration.h`) take one geometry per cluster so a hit's GeometryIndex and PrimitiveIndex are the visibility buffer's cluster and triangle; top-level structures live in the bindless set at binding 3 (`RaytracingAccelerationStructure g_scenes[]`, only on devices with `DeviceFeatures::acceleration_structure`). Ray-traced visibility must match the rasterized cut word for word; the ray query test asserts it. On NVIDIA the geometry is cluster acceleration structures (`gfx/cluster_acceleration.h`, ADR-0025) built every frame from the cull pass's visible list by `clas_records.slang` (its records must stay byte-identical to `write_cluster_build_records`; the records test asserts it); the KHR path is the fallback, and the two must stay pixel-identical (the cluster AS test asserts that too). Acceleration structure builds and traversals go through the render graph as `Access::AccelerationBuildRead/Write` and `Access::RayQueryRead`, never through hand-written barriers in a pass body.
- Cluster LOD: `geometry::ClusterLodDesc` carries two spheres and two errors per cluster; a cluster draws when projected own error <= threshold < projected parent error. The GPU pass (`cluster_cull.slang`) and `geometry::lod_selects` must stay the same formula; the cull test checks them against each other.
- Visibility buffer: u64 per pixel, depth (reversed-Z float bits) << 32 | (cluster << 8 | triangle), written with 64-bit atomic max by both `fs_visibility` (hardware) and `cluster_sw_raster.slang` (software); the two must stay pixel-compatible and the visibility test compares them. GPU time comes from `gfx::GpuTimer`, never from CPU clocks.
- Occlusion culling is two-pass against a Hi-Z of the farthest depth (`hiz_build.slang`, `CullParams::pass/hiz/flags`); it must never change the picture, and the occlusion test asserts identical visibility buffers with and without it.
- The material resolve (`visibility_resolve.slang`, `gfx::ResolveParams` via address) reconstructs position from perspective-correct screen barycentrics and interpolates the vertex attributes; materials are a table indexed per cluster and lights are an array of `gfx::ResolveLight`. The BSDF itself lives in `domain/gfx/shaders/brdf.slang` — Cook-Torrance GGX with Smith height-correlated visibility and Schlick Fresnel, no bindings and no entry points — because the reference path tracer includes the same file; a shader that includes it names it in the `DEPENDS` of its `engine_shaders()`. The shading and attributes tests pin the model against `domain/gfx/tests/brdf_reference.h`, the same formulas in double precision on the CPU: change the lighting model and change the reference in the same commit, then say by how much the two still agree.
- The baseline tier is `cluster_vertex.slang`: same clusters, same visibility buffer, same cull pass (with `CullParams::count_index` = 1 and `vkCmdDrawIndirect`), no mesh shaders. Anything added to the mesh path's visibility output must be added there too; the vertex path test asserts the two buffers are identical.
- glTF import (`domain/assets`, `load_gltf`/`load_gltf_memory`) flattens the default scene into one `MeshData` — world-space positions, inverse-transpose normals, UVs, widened indices, per-primitive ranges, metallic-roughness materials — ready for `build_clusters` with `attribute_source(mesh)`. It accepts triangles only and does not decode images (bytes or URI are handed on), touch the GPU, or read skins, animations, cameras, or compressed buffer views.
- The content build writes what the renderer loads: `engine-content build <mesh.gltf> <out.clusters>` imports a glTF file, clusters it per material, and writes a `.clusters` container (`geometry::cluster_file.h`) holding the LOD DAG, a material index per cluster, the materials, and the image paths; `engine-content info` prints a file's header, sections, and counts as one JSON line, and `stats` the content-build metrics. A container also records **what it was built from** — the source's content hash and the build key over it and the options — so that `engine-content build-all <manifest.json>` can skip an entry whose output is already the answer; a container recording no identity is rebuilt rather than trusted, and an incremental build never decides from a timestamp. The per-primitive builds inside one mesh and the per-mesh builds inside one manifest both run on the job system's performance pool (`--jobs <n>`), and **the output must not depend on the thread count**: merge by primitive index, never by completion order. The container's sections are **append-only**: a kind is never renumbered or reused, and a reader skips a kind it does not know, so adding data to the format keeps older builds reading it. `engine-view --mesh` reads a `.clusters` file as readily as a glTF, and everything downstream draws the same picture from either.
- Derived data lives under the git-ignored `ddc/`, content-addressed and rebuildable, and is **never committed**. A built mesh is `ddc/clusters/<hash>.clusters`, where the hash covers the source's bytes, the build options, and a version constant (`geometry::cluster_cache_key`/`cluster_cache_path`, `assets::source_mesh_hash`); the root is `<repo>/ddc`, found by walking up to the directory holding this file, or `--ddc <dir>`. `engine-content build --cache` fills it, `engine-view --mesh <file.gltf>` reads it and fills it on a miss, and `--no-cache` skips it. A change that makes an old entry wrong bumps `k_cluster_cache_version`.
- Sample assets: `tools/fetch-samples.ps1` downloads a pinned, license-checked (CC0 and CC-BY only) set of Khronos glTF models into `content/samples/`, which git ignores; `engine-view --mesh content/samples/<Model>/<file>` renders one and `content/samples/LICENSES.md` records the licenses. Never commit binary assets; extend the script instead.
- `engine-view --frames N --capture out.png` is how to look at what the renderer draws; read the PNG.
- Input reaches a game as tick-stamped `input::RawEvent`s through `input::InputState` (`begin_tick`/`feed`/`end_tick`, then `pressed`/`held`/`axis`/`axis2`), never as `window::Event`: `foundation/input` does not depend on `foundation/window`, so a headless run and a replay feed the same code. Bindings live in an `ActionMap` that serializes to JSON and hashes; an `input::InputLog` records every event and replays it bit for bit, and refuses a map whose hash differs. Games never read window events directly; the conversion happens once, in whoever owns the window. A device SDL has no gamepad mapping for (wheel, pedals, shifter, flight stick) comes through as a raw joystick with bare axis, button, and hat indices, and **the gamepad and joystick slot spaces are separate**: `RawEvent::device` is told apart by the event's `Source`, never by the slot number. A hat binding names one direction, `input::hat_code(hat_index, direction)`. `engine-input` is the probe that characterizes a device nobody here owns; `docs/subsystems/apps.md` has the two commands to send its owner. **What comes back is committed**: `content/input-logs/` holds one recording per device, replayed on every build by `foundation/input`'s corpus test against a table of measured expectations, and a new device means a `.jsonl`, a section in that directory's README (the only place a slot is tied to a device name), and a row in the table. A device the driver names badly or not at all gets a row in `window`'s name table instead of a workaround at the call site; force feedback is `window`'s too, and **opening a wheel stops its own centring spring**, so whoever opens one sets a spring or a damper and keeps it running. See `docs/subsystems/input.md` and `docs/subsystems/window.md`.
- Authored layers merge structurally, never by text: `doc::merge_layers` (`domain/doc/merge.h`, `doc.merge`) works per object, by `Id128`, and per property, so a rename is a property change and two edits to different properties of one object both survive. It reports conflicts instead of refusing, since refusing would lose the work on both sides; `$type` and `$parent` name the record's own fields in a report. The merged layer is canonical, so swapping the two sides gives byte-identical JSON when nothing conflicts, and the merge test asserts it.
- Images are decoded on the CPU by `image::decode_image`/`read_image` (PNG, JPEG, TGA, BMP, always to 8 bits per channel) and encoded by `image::encode_png`; stb_image lives in that one module and is never included anywhere else. GPU upload is `gfx`'s (`upload_image_2d`), and the compressed texture formats belong to a future texture pipeline, not to the decoder.
- Shaders load through `gfx::ShaderLibrary`: embedded bytes ship, the build's `shaders/manifest.json` wins while developing, and edited `.slang` files recompile and reload in a running `engine-view`; a failing save prints slangc's diagnostics and keeps the last good shader. Reflection is read from the SPIR-V (`reflect_spirv`), so mirrored C++ structs can be checked against `push_constant_bytes`.
- Logging: `ENGINE_LOG_INFO(category, "short static message", log::field("key", value), ...)` with a category defined once per module (`ENGINE_LOG_CATEGORY_DEFINE`). Data goes in typed fields, never interpolated into the message; no `printf`-style logging in engine code; report output that is the product of a tool (the bench table, CLI output) is the exception. See `docs/subsystems/log.md`.
- Every new module gets a `docs/subsystems/<module>.md` page and a `tests/` directory in the same change. New **capabilities** — a system, its component and event types, a solver or backend, an LOD policy — are scaffolded by `tools/new-capability.ps1` and follow ADR-0027's checklist: one module in the layer it belongs to, `engine_module(... OPTIONAL)` so `ENGINE_WITH_<NAME>=OFF` and the minimal build prove the rest of the tree does not depend on it, attachment only through the registration points (schema types, the scheduler's table, `RenderGraph::add_pass`, a `derived` content-build step, the protocol's method table, tunables), an LOD policy and a determinism stance, and no edit to `core/`, `foundation/`, the render graph, the scheduler, or another capability. See `docs/plan/02-architecture.md` §2.8.
- Run-time parameters that a kernel or system reads (batch sizes, spin counts, budgets, thread counts) are `tunables::Int/Float/Bool/Enum` objects from `foundation/tunables`, read once outside hot loops. Core-layer modules take config structs instead and the app layer fills them from tunables. Data-selected dimensions (formats, modes) are compile-time variants, never tunables.
- Commit messages: imperative subject under 72 characters, a body that says why, and the attribution trailer the harness supplies. Reference ADRs and plan sections when a change implements them.

## Recording decisions

Anything that would surprise a future contributor gets an ADR: copy `docs/adr/0000-template.md`, take the next number, fill in context, decision, consequences, and when to revisit. Accepted ADRs are never edited except to mark them superseded.
