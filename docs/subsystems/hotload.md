# hotload (foundation)

**Purpose.** Hot-reloadable C++ game modules: gameplay code compiled into a shared library (`.dll` on Windows, `.so` on Linux) that a running host loads, calls, watches and replaces while it runs, carrying the module's state across the swap through the schema descriptors of the old and the new build. It is the C++ arm of experiment E7, built so that the scripting decision ([08 §8.2](../plan/08-toolchain.md#82-gameplay-and-scripting-layer)) has two arms to compare instead of one; the comparison with the Luau arm, row for row, is in the [E7 write-up](../experiments/e7-luau-spike.md#the-c-hot-reload-arm). Its sibling is [scripting](scripting.md), the Luau host, which serves the same feature through a VM.

What it deliberately does not do, in v0: give a module write access to anything the host owns (a module reads host objects through views, as a Luau script does, and keeps its own state; changing the document stays a command), protect the host from a module that crashes or loops (a module is native code in the engine's process: there is no sandbox and no step budget), let a module hold owning types in its state, reload anything but the one library a host watches, or watch with change notifications rather than a poll.

**Why this shape.**

- **A module links no engine library.** Its include path is the engine's headers (`core/*/include` and this module's `module_abi.h`) and its link line is the C runtime and nothing of ours. Everything it needs from the engine — the heap, the log, the host's clock, reads of host objects — comes through the `HostApi` table of function pointers the host hands it. The reason is that a static library linked into a DLL is a second copy of that library's globals: a second registry of log categories the log configuration never reaches, a second tunables registry the console does not list, a second allocator whose memory the host's tracking never sees and whose frees would land in the wrong heap. Each of those is a bug that works in a test and fails in a long session. Linking nothing makes the question not arise; `-z defs` on Linux (and the MSVC linker's refusal of an unresolved symbol in a DLL) makes it a link error rather than a promise.
- **What that costs, and forbids.** It costs a function-pointer call per engine service and a host-side descriptor walk per property read (the bench has the numbers: [E7](../experiments/e7-luau-spike.md#the-c-hot-reload-arm)). It forbids, inside a module: `core/containers` and `std::string` across the boundary (their memory belongs to whoever allocated it), any engine function that is not inline in a header (`ENGINE_ASSERT` included: its handler lives in `core/base`'s library), the engine's log macros (use `hotload::log`), exceptions (the module compiles like the engine), and reading a host type's fields by its C++ layout — a module that included a host struct's header and read `npc->health` would be correct until the host's layout changed without the module being rebuilt, which is exactly the situation hot reload creates. A module may include header-only code freely: the schema descriptors' tables, `core/math`'s vectors, its own types.
- **Views by descriptor, resolved once.** A module names a host field by type and dotted path (`"engine.hotload_test.Character"`, `"attributes.strength"`) in `on_load`, gets a `FieldRef`, and reads by handle on every call (`HostApi::read`): the host checks the view's slot and version, checks that the field was resolved against the view's type, and reads the value at the offset the host's own descriptor gives, into a 48-byte `Value`. It is the Luau binding's read without a VM in the way, and with the name lookup moved out of the call: a misspelt field is found when the module loads, not when a test happens to reach the line. Views have the Luau views' lifetime rule (a stale view is an error, never memory), because a module keeping a view past the call it was bound for is the first lifetime mistake C++ makes.
- **State through the descriptors, not by layout.** A module's state is one struct declared in a `.schema` file beside it. `engine_game_module(... SCHEMAS ...)` runs schemac over it and compiles the generated tables into the module with `ENGINE_SCHEMA_NO_REGISTRAR` (the tables are constant data and header templates; only the registration needs a library), so each build carries the descriptor of the state it was compiled with. On a reload the host writes the old state as schema JSON through the old build's descriptor and reads it into the new build's state through the new one, with unknown fields ignored: a field added takes its default, a field removed is dropped, a field widened (`u16` to `u32`, `f32` to `f64`, an integer to a float) converts, and a field whose kind changed otherwise refuses the reload with a line naming it. The rules are the IDL's own evolution rules ([schemas/README.md](../../schemas/README.md)), because they are the same walker the document model uses for every schema object. A renamed field is a removal and an addition — reported, not refused, and its value lost.
- **A per-load copy.** The host loads `<shadow>/<name>.<pid>.<generation>.dll`, never the watched file: Windows keeps a loaded DLL's file locked, so the build could not write the new one while the old one runs; and glibc's `dlopen` returns the handle it already has for a path it has loaded, so loading the same name again would run the old code. The copy is deleted when its build is unloaded.
- **The old build runs until the new one is proven.** A reload loads the new build beside the old one, checks its ABI, its state type and its exports, migrates the state into a new object, and runs the new build's `on_load` — and only when every step has succeeded calls the old build's `on_unload`, swaps the context's state pointer and the export table, destroys the old state with the old build's own destructor, and unloads it. Any failure on the way leaves the old build running, logs once and is in `last_error()`; a refused file is not retried until it changes again (the same rule as a Luau script that does not compile).
- **A closed set of export signatures.** An export is a name, a signature number and a pointer. `export_function()` refuses at compile time a function whose type is not one of `module_abi.h`'s signatures, and the host refuses to hand out a function under a signature it was not exported with, so a module and a host that disagree about a call never make it. A function the host already holds must still be exported, under the same signature, by every later build, or the reload is refused (`MissingFunction`, `SignatureMismatch`): a held pointer to a function that no longer exists would be a call into an unloaded library.
- **In the foundation layer, and optional.** The host depends on core, `io` and nothing else; the game layer and anything else that wants gameplay DLLs can attach. It is a capability (ADR-0027): `ENGINE_WITH_HOTLOAD=OFF` and the minimal build drop the host, its tests, its bench, and every game module (`engine_game_module()` does nothing without it).

**Owned data.** Per host: the watched library's path, size and modification time; the loaded build (library handle, copy path, name, state type, state object, hooks, exports); the export table the host hands out (`FunctionId` → name, signature, current pointer); the binding slots (object, type, version); the resolved field paths (root type, offset, kind, path) — one entry per path, however often it is resolved; the last error and the last reload's report.

## The module ABI

`include/foundation/hotload/module_abi.h` is everything a module and the host share, and it includes nothing but `core/base/types.h`. A module:

- defines its entry point with `ENGINE_HOTLOAD_DESCRIBE(api, out) { ... }` — `extern "C"`, exported, the one symbol the host looks up (`engine_hotload_describe`) — and fills a `ModuleDesc`: `k_abi_version` and `sizeof(ModuleDesc)` as it saw them, a name, its exports, `on_load`/`on_unload`, and (through `describe_state<State>()` in `module_state.h`) its state type and the `TypeInfo` size it was built against. A non-zero return refuses the load; `hotload::fail()` says why.
- exports functions of the five signatures (`VoidFn`, `NumberFn`, `ViewNumberFn`, `TickFn`, `ConditionFn`), each taking the host's `ModuleContext*` first — the `HostApi` and the module's state, at one address for the module's whole life.
- resolves field paths in `on_load` (`hotload::resolve`, `hotload::enum_value`); **every path that does not resolve is listed in one refusal**, so a module with three misspelt names hears about all three at once.

Changing anything in the header changes `k_abi_version`, and `tests/size_table.cpp` pins every struct in it so a layout change cannot slip past.

| Status | When |
|---|---|
| `LoadFailed` | the OS loader refused the file (not an image, a missing dependency); no dialog ever opens (`SetThreadErrorMode`) |
| `NoEntryPoint` | no `engine_hotload_describe` (written without `extern "C"`, or not exported) |
| `DescribeFailed` | describe returned non-zero, with the module's line |
| `AbiMismatch` | built against another `module_abi.h`, or another `schema::TypeInfo` |
| `BadDescriptor` | a state type with an owning field, an export table with no name, a null pointer or an unknown signature |
| `StateIncompatible` | the new state type cannot take the old state (the field is named), or the state type's name changed |
| `LoadHookFailed` | `on_load` returned non-zero, or a field path or an enumerator did not resolve (all of them listed) |
| `MissingFunction`, `SignatureMismatch` | the new build drops, or retypes, a function the host holds |

## What a module's state may hold

Fixed-layout values only: booleans, integers, floats, `vec2`–`vec4`, `quat`, `worldpos`, `dvec3`, `id128`, enums, fixed arrays and nested structs of those. A string, bytes, JSON, an optional, an array or a map is refused when the module loads, naming the field: its memory would be allocated by the module's code, possibly from its own copy of the C runtime, and freed by whoever destroys the state, after the library that owned the allocator is gone. A module that needs a string keeps an id, or a fixed array of `u8`.

`transient` fields are not carried (they are not serialized); they start at their default after every reload.

## Building a module

```cmake
engine_game_module(NAME sandbox_npc
  SOURCES sandbox_npc.cpp routines.cpp
  SCHEMAS sandbox_npc.schema)
```

`cmake/EngineGameModule.cmake` builds `engine_game_<name>` as a `MODULE` library named `<name>.dll`/`.so` into `build/<preset>/bin/game/` (or `OUTPUT_DIRECTORY`): unity build on (one translation unit, so an edit is one compile and one link), hidden visibility, the engine's flags (the CPU baseline, floating-point contraction off, warnings as errors, no exceptions), `-fno-gnu-unique` on GCC (a GNU-unique symbol would make `dlclose` a no-op and the old build would stay mapped), and `-z defs` on Linux. It also writes `build/<preset>/game_modules/build_config.cmake`, the configuration of an **exported build command** that compiles a module from a directory of sources anywhere — schemac included — with the same compiler and flags:

```
cmake -DCONFIG=build/<preset>/game_modules/build_config.cmake -DSOURCE_DIR=<dir> -DOUTPUT=<file> -P cmake/EngineGameModuleBuild.cmake
```

The tests use it to compile second builds into their own scratch directories (a test owns nothing outside them), and it is what an editor or an agent session would call to rebuild a module outside the build tree. It records the MSVC environment the configure ran in, so it works from a shell without one.

**The game layer.** `game/` is the L5 directory, and `game/sandbox_npc` is its first module: E7's Luau fixtures as C++ (`empty`, `echo`, `base`, `ten`, `mixed`, `on_tick`) plus a 200-line body (`routines.cpp`) for the rebuild measurement. Nothing in the engine links a game module.

## Invariants (tested)

- A module loads, its exports are found by name and signature, a wrong signature or an unknown name is refused, it reads a bound host object through a view, and a poll with no change does nothing.
- A rebuilt module written over the watched file is swapped in on the next poll; the state is carried with a field added at its default, a field removed dropped and a field widened converted, the `FunctionId`s held before the swap call the new code, and the old build's copy is gone.
- A state type changed incompatibly is refused with the field's name, the old build keeps running and keeps its state, and the refused file is not retried until it changes.
- No entry point, a failing describe, another ABI, unresolvable field paths (all listed together), a build dropping a held function, and a file that is not a library are each refused, first and as a reload over a running build, and the running build is untouched.
- After `unload()` the copy is deleted and the watched file can be removed; on Linux the library is no longer mapped.
- Views read every kind the descriptors describe; a stale view (rebound, unbound) and a view of another type are refused and counted, and a refused read leaves the value Nil.
- A module's state type with an owning field is refused; migration through identical descriptors is the identity; two different state types are refused.
- A build compiled from source by the exported command onto the watched file is picked up by the poll and runs, with its new state field defaulted.
- E7's seeded mistakes (`tests/seeded_mistakes_tests.cpp`): each of fourteen mistakes compiled, loaded and run, and its outcome — compiler, loader, a refused read at run time, a wrong answer, or not run — asserted; and the Luau arm's conditions B and C as written.

**Public API.** `foundation/hotload/module_abi.h` (the module side): `View`, `FieldRef`, `ValueKind`, `Value`, `Result`, `LogLevel`, `HostApi`, `ModuleContext`, the signatures and `k_signature`, `Export`, `export_function`, `ModuleDesc`, `DescribeFn`, the module-side helpers, `ENGINE_HOTLOAD_DESCRIBE`. `foundation/hotload/module_state.h`: `describe_state`. `foundation/hotload/module_host.h` (the host side): `Status`, `status_name`, `FunctionId`, `HostConfig`, `ReloadReport`, `HostStats`, `ModuleHost`, `migrate_state`, `check_state_type`.

**Depends on.** `base`, `containers`, `memory`, `log`, `time`, `json`, `schema`, `io`; the platform loader (`LoadLibraryExW`, `dlopen`), in `src/dynamic_library.cpp` alone.

**Testing.** `tools/dev.ps1 test -Filter hotload`. `tests/hotload_tests.cpp` is the host against real modules built by `engine_game_module()` (`tests/modules/`: three builds of a counter and five builds the host must refuse) and one compiled from source in the test; `tests/seeded_mistakes_tests.cpp` is E7's error table, compiling about seventeen small modules (a minute on a loaded machine, hence the work timeout); `tests/size_table.cpp` pins the ABI. The tests that need modules or the compiler pass with a message inside a packaged test bundle, which carries neither. Benchmarks: `tools/dev.ps1 bench -Preset msvc-release -Filter 'hotload.*'`; the module's rebuild time is measured with the build tool (the write-up says how).

## Performance notes

Measured on 2026-10-10, `msvc-release`, on the i9-10980XE development machine (quiet at the start of every group by the harness, a GPU lock held by another agent for part of it, so upper bounds); every number and the machine's state are in the [write-up](../experiments/e7-luau-spike.md#the-c-hot-reload-arm), beside the Luau arm's measured in the same window. The ones that set expectations:

| What | Cost | Luau, the same row |
|---|---|---|
| A call into the module, nothing passed | 3.1 ns | 74 ns |
| One scalar property read through a view | 7.0 ns | 50 ns |
| `on_tick` over 10,000 bound characters | 166 µs a tick, 16.6 ns an instance, no allocation | 2.36 ms, 236 ns |
| The same routine as engine code, no boundary | 1.9 ns an instance | — |
| Host memory per bound instance | 34–37 B | 174–432 B |
| Rebuilding the 200-line module (unity, `ninja` on the target) | 2.4–2.6 s, of which 0.6 s is `ninja` itself | 0.45 ms to compile the script |
| `reload()`: copy, load, migrate, swap, unload | 34–36 ms: copy 1.0 ms, **OS load 29.4 ms** (an antivirus scan of the new image), migrate and `on_load` 18 µs, unload 0.45 ms | 1.5 ms from the poll |
| Migrating one state object (37 bytes of JSON) | 1.5 µs | — |
| Polling the watched file, unchanged | 227–240 µs (`io::stat_file`) | 256 µs a file |
| The module file / what the host adds to an executable | 55,808 B / 151,552 B | +1,689,088 B |

The boundary is the view read, not the call: a routine through the module costs 8.7× the same routine as engine code, and 1/14 of the Luau routine. **Keep per-entity hot loops in engine code**; a module is for gameplay logic iterated on with its state carried.

What is not optimized yet: the poll stats the file through `io::stat_file` (three file-system queries, the same cost the Luau arm's poll pays); a directory change notification would replace it. The copy reads and writes the whole library; a hard link or `CopyFileEx` would be cheaper and was not needed at this size.

## Not yet

- **Writes.** A module reads host objects and owns its state; a module-facing command API (return a command list the host applies as one transaction) is the same next step as the Luau arm's.
- **Several modules per host, and one module's views shared with another.** One host holds one module.
- **Arrays of owning types in views, maps.** A view reads scalars, strings, enums, vectors, ids, optionals and the elements of scalar arrays; a map or an array of structs is `Unsupported`.
- **Protection.** A module that dereferences garbage or loops forever takes the host with it; there is no budget and no sandbox, and none is planned for native code.
- **An editor or protocol front end** (`module.reload`, `module.build`) is not built; the exported build command is what it would call.

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, the rest of `foundation/`, the render graph, the scheduler, or another capability, and it can be removed from the build the same way. (Two shared files gained a line each: the root `CMakeLists.txt` includes `cmake/EngineGameModule.cmake` and adds the `game/` layer, and schemac's generated sources wrap their registration in `ENGINE_SCHEMA_NO_REGISTRAR` so a module can compile them; both are inert without this capability.)

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | none | stands alone |
| Component and event types | none of its own: a module's state is a schema type the module declares; `tests/hotload_test.schema` is test-only | not needed |
| Tick scheduler entry | none: reloading is a service a host drives between ticks (`k_phase` = `none`); a game's systems call module exports from their own ticks | not needed |
| Render-graph passes | none | not needed |
| Content-build derived step | none: a module is built by the build, not by the content build | not needed |
| Protocol methods | none yet | `module.reload`, `module.build` (see "Not yet") |
| Tunables | none: the shadow directory and the clock are a host's configuration | not needed |
| LOD policy | none: a module's functions are called by whoever ticks, at whatever tier they choose | not needed |
| Determinism | `hotload::k_determinism` = `hashed`: a module is native code compiled with the engine's flags, the clock it sees is the host's fixed step, and the host adds nothing machine-dependent | done |
| Zero cost when unused | no linked code with the switch off; linked and unused, no library is loaded and the log category is the only static state | done |
| Tests and size table | `tests/*_tests.cpp`, `tests/size_table.cpp` | done |
| Bench | `bench/hotload_bench.cpp`, over `game/sandbox_npc`, and the two size probes (`bench/size_probe.cpp`) | done |
| Removal proof | `ENGINE_WITH_HOTLOAD`, off in the minimal build | works |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_HOTLOAD=OFF`) drops the module, its tests, its bench and every game module; the module disappears from `build/<preset>/modules.json` and is listed there under `disabled_capabilities`. Everything else builds and passes.
