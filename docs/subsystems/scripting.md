# scripting (foundation)

**Purpose.** The Luau host for content logic — quest conditions, dialogue logic, NPC routines: the small, numerous, often-edited functions that [08 §8.2](../plan/08-toolchain.md#82-gameplay-and-scripting-layer) does not want in C++ and cannot express as declarative data. A `ScriptContext` owns one sandboxed Luau VM and any number of scripts in it; the host calls a script's named function (`on_tick(self, dt)`, `evaluate(quest, player)`) with a budget, the script reads document state through read-only views generated from the schema IDL, and it returns **data** — a JSON-like value the host turns into document commands. It is the Luau half of experiment E7 ([write-up](../experiments/e7-luau-spike.md)); the C++ hot-reload and C# halves are not built.

What it deliberately does not do, in v0: let a script mutate anything (mutation stays one schema-typed command at a time, [03 §3.3](../plan/03-data-model.md#33-transactions-diffs-and-merges); a console program changes a document only by calling the protocol, [below](#programs-the-console)), keep a coroutine alive across calls, persist anything a script keeps in its own module state, debug a script, or run native code.

**Why this shape.**

- **Functions over read-only state that return data**, not scripts that drive the world. A condition that returns `true` or an NPC routine that returns `{ action = "flee" }` is a pure function of its inputs and the tick, so it replays, it can be run speculatively (an agent asking "would this quest complete?"), and every change it causes is an attributed command in the journal like an agent's or an editor's. A script with write access would be a second, unattributed mutation path around the transaction system.
- **One VM for many scripts.** Each script gets its own global table (a sandboxed thread's), so scripts cannot see each other, while the heap, the string table and the metatables are shared. E7 measured what the other choice costs: a VM per instance is about 300 KB against 174 bytes an instance in a shared VM, and 3–10 times the tick time ([write-up](../experiments/e7-luau-spike.md#results)).
- **Generic bindings, not a binding per type.** The schema descriptors already say everything a read needs — each field's name, kind and offset, each enum's names — so one metatable serves every schema struct, keyed by the descriptor. A generated per-type binding would be a second copy of that, emitted into every schema module whether a script ever reads it or not and linked into every game. The generic path costs one field lookup per read; string atoms make that a binary search over a few 4-byte entries (below).
- **A step budget, not a time budget.** The budget that stops a runaway script counts safepoints, which is deterministic: the same call over the same inputs uses the same number of steps on every machine, so a replay can never diverge because one machine was slower. A wall-clock budget exists as an optional backstop and is off.
- **In the foundation layer, and optional.** The architecture puts the scripting host in L1 so that domain and systems modules can bind to it; it depends only on core and on `io` and `tunables`. It is still a capability (ADR-0027): nothing below it needs it, a game that does not script should not link a VM, and `ENGINE_WITH_SCRIPTING=OFF` is the proof.
- **Luau, pinned.** Typed Lua, MIT, sandboxing designed in (read-only tables, per-thread environments, an interrupt), a bytecode compiler that is a library, and a type checker that is a library too. Version 0.739, fetched by `cmake/EngineScripting.cmake`; the VM and the compiler link privately into this module, nothing outside `src/` includes `lua.h`, and the analysis library links only into the tests.

**Owned data.** One `lua_State` per context; per script its chunk name, file path, content hash, generation and frozen global table; the function table (`FunctionId` → name → registry reference, re-resolved on reload); the binding slots (object pointer, type, serial, version); a per-type field-lookup table; the error of the last failure.

## The sandbox, and where each guarantee is enforced

| Guarantee | Where it is enforced | Why there |
|---|---|---|
| No `io`, `os`, `debug`, `require`, `loadstring`, `load`, `dofile` | never opened, or not in Luau's VM at all | a library that is not opened has no surface to audit |
| No wall clock (`os.time`, `os.clock`, `os.date`) | `os` is never opened | time comes from `engine.tick()` and `engine.fixed_step()`, which the host sets from its fixed-step clock |
| No random seed the script did not receive | `math.random`/`math.randomseed` removed (Luau seeds them from an address) | `engine.random(seed, index)` is a pure function of its arguments |
| No `print`, `gcinfo`, `getfenv`, `setfenv` | removed before the global table is frozen | stdout is not a channel; heap size is allocator-dependent; `setfenv` reaches another function's environment |
| No change to shared state | `luaL_sandbox`: every library table, the string metatable and the global table are read-only | one script cannot change what another one calls |
| No new globals at run time | each script's global table is frozen once its top level has run | a script's globals are its entry points; assigning one from inside a function — Lua's classic bug — is an error at that line |
| No reaching the view metamethods | view metatable has `__metatable = "locked"` and is read-only | a script cannot swap `__index` under the engine |
| Memory limit | the VM's allocator | the only place every byte passes; see below |
| Step budget | the VM's interrupt | Luau calls it at every loop back edge, call and return, so any unbounded computation passes through it once per iteration |
| Errors are statuses | Luau built with `LUA_USE_LONGJMP=1`; every call is a `lua_pcall` with a message handler | see "Errors" |

**The memory limit** is a byte count on the VM heap, checked in the allocator (`scripting.memory_limit_mb`, default 64 MiB). At the limit a request fails, Luau raises its out-of-memory error inside the protected call, the call returns `MemoryExceeded`, and a full collection runs before the next call so the VM is usable at once. It is enforced **only while a script runs**: loading, pushing arguments and converting results are host-side work bounded by the host's own data, and an allocation refused there would have no protected call to land in. Every byte the VM owns goes through the engine heap under the `scripting` tag, so the memory report attributes it and `mem::allocation_counter()` sees it.

**The step budget** counts safepoints per call (`scripting.step_budget`, default 1,000,000; 0 is unlimited). A trivial function uses one or two, a quest condition tens to hundreds. Past the budget the next safepoint raises an ordinary Luau error; a script that catches it with `pcall` only reaches its next safepoint, which raises again, so the budget cannot be caught and ignored. The message is a string made once at context creation, so raising it allocates nothing. `scripting.time_budget_us` adds a wall-clock deadline checked every 1,024 steps; it is off because it makes a call's outcome depend on the machine.

**Known holes.** `tostring` of a table or a function prints its address, and `pairs` over a table keyed by tables iterates in address order; neither is deterministic across runs, and the sandbox does not hide them. A script that writes either into its result breaks replay. `math.sin`, `math.exp`, `math.pow` and the other transcendental functions call the C library, so the same script can give a different last bit under MSVC's runtime and glibc — deterministic on one platform, not across them; the arithmetic operators are the same everywhere, because Luau is compiled in the tree with floating-point contraction off ([ADR-0035](../adr/0035-no-floating-point-contraction.md)). Module-level state (`local cache = {}` at the top of a script) persists across calls until the next reload and is not saved anywhere.

## Errors

Every failure is a `Status` and `last_error()` says `chunk`, `line` and `message`; nothing is thrown across the boundary. The message handler every call runs under finds the first interpreted frame above the error, which is where the script was when it failed — including for a budget error, which fires inside the loop that exceeded it, and for a memory error, where Luau runs the handler too. The status comes from what the interrupt or the allocator recorded, not from the message, so a script cannot fake `BudgetExceeded` by raising the same text. A compile error carries the line the compiler reported. The `chunk:line:` prefix Luau puts on a message is stripped, since it is in the fields.

Luau raises by throwing a C++ exception by default, through whatever frames lie between the raise and the protected call — which here are engine functions compiled with `-fno-exceptions` on GCC and Clang. `LUA_USE_LONGJMP=1` makes it a `longjmp` instead, the configuration Luau uses for its C API. The cost is one rule for this module's code, written at the top of `src/context_impl.h`: **a function a script can call holds no object with a destructor at any point where it can raise**, because `longjmp` runs none.

## The generated bindings

A schema object reaches a script as a **view**: a 24-byte tagged userdata (`View`) whose metatable's `__index` looks the property up in the object's `TypeInfo` and pushes the field as the IDL types it:

| Schema | Luau |
|---|---|
| `bool` | boolean |
| integers, floats | number; a 64-bit integer only when a double holds it exactly — past 2^53 the read is an error, never a silently different number |
| `string`, `bytes` | string |
| `id128` | 32 lowercase hex characters |
| `vec2`, `vec3` | `vector` (vec2 has z = 0) |
| `vec4`, `quat` | a read-only `{x, y, z, w}` table |
| `worldpos`, `dvec3` | a read-only `{x, y, z}` table of numbers (f64), never a `vector`: a Luau vector is float32, and a world position must not become one (ADR-0053) |
| `json` | read-only tables |
| enum | the enumerator's name; a value with no name reads as its number, as schema JSON does |
| struct | a nested view |
| `T?` | nil or the value |
| `T[]`, `T[N]`, `map<K, V>` | a read-only table, built on each read |

A property that does not exist is an error naming the type and the property (`engine.scripting_test.Character has no property 'helth'`), never nil — nil is a value, and a misspelt name returning it is the bug that goes unnoticed longest. Assignment is refused with the type and the property; so is `table.insert` into an array read, because the table is read-only. Arrays and maps are copied into a table rather than viewed through another proxy so that `#`, `ipairs`, `table.find` and generalized iteration work and the declared type `{ T }` is simply true; the cost is O(n) per read, so hoist a collection read out of a loop.

**Field lookup by atom.** Luau gives every string an atom when it is created (`lua_Callbacks::useratom`). The context assigns one to every field name of every registered schema struct before the VM exists, so a property name in a script — created when the chunk loads — carries its atom, and `__index` finds the field by a binary search over the type's `(atom, field)` pairs. A name with no atom (built at run time before any type named it, or past the 32,767 atoms an `int16` holds) falls back to comparing names, and works the same.

**Lifetime.** A view never points at host memory unchecked. `bind(object)` gives the object a slot and a **root view** that reads whatever the slot holds, cached in the registry so passing a binding to a call allocates nothing. A **nested view** (`npc.attributes`) points into the object and carries the slot's version. `rebind()` bumps the version (the root view follows the new object; nested views of the old one are stale) and `unbind()` bumps both counters. An `Arg::object` or an `engine.object(id)` result is bound for that one call and unbound when it returns. A stale view reads as an error — "this object view is no longer valid" — never as memory, which is what makes it safe for a script to keep a view in its module state.

**Following references.** `engine.object(id)` asks the host's `ObjectResolver` for the object with that id and returns a view valid for the rest of the call, or nil. It is how a condition follows `quest.giver` or an inventory's item ids. v0 has no typed lookup: it returns `any` in the definitions, and a script that wants checking casts (`engine.object(id) :: Item`).

## Content logic: calls, arguments, results, the engine API

`load(name, source)` compiles and runs a script's top level, which defines its global functions; `find_function(script, name)` resolves one into a `FunctionId` that survives reloads; `call(function, args, result)` calls it. An `Arg` is a trivially copyable 24-byte value — nil, boolean, number, borrowed string, object (for the call), binding, borrowed JSON (a read-only table). The first result, when asked for, becomes a `JsonValue`: nil to null, integral numbers to integers, sequences `1..n` to arrays, string-keyed tables to objects (keys sorted, so the output does not depend on table iteration order), an empty table to an empty array, vectors to `[x, y, z]`, views to their schema JSON. A mixed or holey table, a function, a NaN or a cycle is `InvalidArgument` with the reason.

The engine API is the global `engine`, typed in the definition file:

| Function | What |
|---|---|
| `engine.log(level, message, fields?)` | a record in the `scripting` log category with the script's chunk and line as the fields `script` and `line`, plus up to 14 typed fields; levels `trace`, `debug`, `info`, `warn`, `error` — no `fatal`, a script does not get to stop the engine. The position is a field rather than the record's `file` because sinks keep `file` as a pointer and expect `__FILE__` |
| `engine.tunable(name)` | a tunable's value: number, boolean, or an enum's choice name. A lookup scans the registry, so read it once. Tunables are hardware-selected knobs; a script that lets one change its *output* makes that output machine-dependent |
| `engine.tick()`, `engine.fixed_step()` | what the host set with `set_time()` — its fixed-step clock, never the wall clock |
| `engine.random(seed, index)` | the index-th number in [0, 1) of the stream named by seed: `mix64`/`hash_combine` from `core/hash`, a pure function |
| `engine.object(id)` | the resolver, above |

**The tick driver.** `ScriptingSystem` calls `on_tick(self, dt)` for every tier-0 instance under the context's budgets, logs the first failure of a tick and counts the rest — one broken script must not stop ten thousand working ones — and collects the non-nil results. A steady-state tick of routines that read properties and return nil **allocates nothing, VM or host**: the test counts `mem::allocation_counter()` over a hundred ticks.

## Programs: the console

`run_program(name, source, host, args)` is the module's other shape, and it exists for one host: **engine-host's console** ([apps](apps.md#the-console-luau-scripts-over-the-protocol)), where a coding agent on the local machine writes a script and runs it against the live engine. It is a client, not a gameplay script, and the rule above that scripts return data and never mutate is a rule for content logic in a game: a program changes nothing itself either, but it *calls*, and each call is a method of its host — for the console, a protocol request through the same dispatcher a JSON-RPC line goes through, with the host's attribution, its role gate and the read-only rule, and a command in the journal. That is the sanctioned mutation path, the one an agent over MCP or an editor takes; the console only lets the agent compose, loop, branch and keep local state between the calls instead of making them one tool call at a time. It reuses the VM, the sandbox (no io, os, debug, require or loadstring), the memory limit, the step budget, the error report and the schema descriptors; it does not reuse bindings, hot reload or the content-logic engine API.

What a program sees: its own writable global table, reading through to the frozen libraries; `print`, whose line (arguments through `tostring`, tab-separated, newline) goes to the host's `ProgramHost::print`; and an `engine` table of `engine.call(method, params)`, `engine.json(value [, pretty])` (a value's JSON text — `print` shows a table as its address), and **one function per method the host lists**, in nested tables named by the dots: `engine.doc.apply{...}` for `doc.apply`, `engine.render.capture{...}`, and `engine.ping()` for `engine.ping`, since the `engine` namespace is the table itself. A name whose place is taken (`engine.call`, or a method whose namespace is another method) gets no function and is reached through `engine.call`. `args` are the chunk's `...`. The whole program is **one call**: one step budget for the run, not per method call, so a runaway loop stops whatever it calls, and a `pcall` that catches the budget's error only reaches the next safepoint, which raises it again.

**Params are read through the method's schema type** (`HostMethod::params`, `to_json_typed` in `values.cpp`), never by a binding per method. Luau has one table type where JSON has two and one number type where the schema has integers and floats, so the untyped conversion has to guess, and guesses an empty table as `[]`; the descriptors settle exactly that — an empty table is `[]` for an array field and `{}` for a struct or a string-keyed map, a float field gets a float — and decide nothing else: a field the type does not have, or a value of the wrong type, converts untyped and is reported by the protocol's own params reader in its own words (-32602), so the console's errors are the protocol's. A value with no JSON form (a function, a NaN, a mixed table) is refused before the host sees it, naming its path (`doc.apply: params: commands[2].value: a function has no JSON form`). **Results are ordinary tables** the program may change and send back, not the read-only tables content logic gets. **A failed method call raises a table**, `{method, code, message, data}`, with a `__tostring` of `"doc.apply: error 1008: ..."`, so `pcall` hands the script the code and the data without parsing a message; uncaught, `last_error()` reports it at the line that made the call, as `"<method>: error <code>: <message>"` plus the data as JSON. A table a script raises itself is reported as its JSON.

Two things cost time to learn here, and are why the code is shaped as it is. **The API goes into the program's globals before the chunk is loaded** (`compile_chunk`'s `prepare`): `luau_load` resolves a chunk's imports — `engine.doc.apply`, `engine.tick` — against the environment as it is then and caches what it finds, so an API installed after loading left `engine.tick` resolving to the frozen content-logic function. **A C function a script called pushes its results through `lua_checkstack`, not `lua_rawcheckstack`**: the raw form grows the stack without moving the calling frame's own top, and a nested result outgrew the twenty slots a C function starts with — Luau's debug build stops on it, a release build would write past the frame. And an empty `JsonValue` string's view may carry a null pointer, which `lua_pushlstring` refuses. `tests/program_tests.cpp` holds all three, against a fake host that records what it was asked: the typed params, results and error tables, the names, print and arguments, the sandbox, a forty-level result, and one budget across many calls.

## Hot reload

`load_file(path)` watches the file; `poll_changes()` — the way `gfx::ShaderLibrary::poll_changes()` treats a shader save — stats every watched file, and for one whose modification time or size moved reads it, compares its content hash with the loaded chunk's, and `reload()`s it. A reload compiles the new source and runs its top level into a fresh global table **first**, and only when both succeed swaps it in and re-resolves every `FunctionId` of that script by name; a compile or run error keeps the last good chunk, is logged once (the file's time is recorded before the attempt, so a broken save is not retried on every poll), and is in `last_error()`. A file that vanishes for a moment, as an editor saving by rename does, keeps the chunk. A function the new chunk no longer defines calls as `NotFound` until a later reload defines it again. Bindings and views are unaffected: they belong to the context, not the chunk.

## The typed API

`emit_type_definitions(types)` writes a Luau definition file (`.d.luau`) from the same descriptors the bindings read: the engine API, each enum as an exported union of its enumerator names as string singletons, and each struct as a `declare extern type` whose properties are all `read` — which is what the binding is. Types reachable from the roots are included, enums first by name, structs after the structs they contain, and a simple name is qualified (`engine_doc_Layer`) only when two emitted types share it, so the file depends on the set of types and not on the order they were listed in.

Loaded into Luau's analyser, it turns a misspelt property, a wrong enum literal passed to the API, a write to a read-only property or a wrong return type into a diagnostic with a line number before the script runs. **Stock `luau-analyze` cannot load a definition file** — it has no such flag at 0.739 — so the checker is Luau's analysis library driven directly (`Luau::Frontend::loadDefinitionFile`, then `check`, strict mode, the new solver). In v0 that code is `tests/luau_check.cpp`, linked only into the tests; exposing it to agents as a protocol method (`script.check`, with `script.types` for the file) is the first "Not yet". [E7](../experiments/e7-luau-spike.md#what-the-type-file-catches) has what it catches and what it misses.

## Invariants (tested)

- Every forbidden library and function is absent; escape attempts — patching a library, the string metatable, the `engine` table, `rawset` on `_G`, a new global at run time, `setfenv` — fail with the error text.
- An allocation loop stops at the memory limit with `MemoryExceeded` and the heap back near where it started; the next call succeeds, and so does the limit a second time.
- An infinite loop, a loop that `pcall`s its way around the budget, and unbounded recursion all end as statuses; the step count of a call is the same on every run.
- A runtime error, an error in a helper, an error with a table as its object, and a compile error each come back with chunk, line and message, and nothing is thrown.
- Every schema kind reads with the type the definition file declares; a u64 past 2^53 and a missing property are errors; assignment is refused at every depth; views follow the binding, go stale on rebind, unbind and at the end of the call that passed them.
- A reload that does not compile, or whose top level fails, keeps the last good chunk; a `FunctionId` survives reloads.
- The same script over the same document gives byte-identical JSON in two contexts and on repeated calls; `engine.random` is a pure function of its arguments.
- The definition file loads into the analyser with no errors, declares exactly the functions the runtime's `engine` table has, and the analyser reports the deliberate error in the sample on its line — and the corrected sample runs with the answer its types promised.
- A steady-state tick allocates nothing; a call re-entered from inside a call is refused.

**Public API.** `foundation/scripting/script_context.h`: `Status`, `status_name`, `ScriptError`, `ScriptLimits`, `ScriptId`, `FunctionId`, `BindingId`, `ObjectRef`, `object_ref`, `Arg`, `ObjectResolver`, `ContextStats`, `ScriptContext`. `foundation/scripting/type_definitions.h`: `emit_type_definitions`, `emit_engine_api_definitions`. `foundation/scripting/scripting.h`: `ScriptInstance`, `TickResult`, `ScriptingSystem`, and the registration constants.

**Depends on.** `base`, `containers`, `memory`, `hash`, `ids`, `json`, `log`, `time`, `schema`, `tunables`, `io`; Luau's VM and compiler, privately.

**Tunables.** `scripting.memory_limit_mb` (64), `scripting.step_budget` (1,000,000), `scripting.time_budget_us` (0, off), read by `ScriptLimits::from_tunables()` when a context is created; a context's limits do not change under a running script.

**Testing.** `tools/dev.ps1 test -Filter scripting`. `tests/scripting_tests.cpp` is the sandbox, the budgets, errors, results and arguments, determinism, the engine API and the tick driver; `tests/binding_tests.cpp` the bindings over `tests/scripting_test.schema`, which has one struct of every schema kind; `tests/reload_tests.cpp` hot reload through a `TempDir`; `tests/typed_api_tests.cpp` the definition file, the analyser and E7's qualitative row. Benchmarks: `tools/dev.ps1 bench -Preset msvc-release -Filter 'scripting.*'`, and the command lines at the top of `bench/scripting_bench.cpp` for the ones that print their own JSON lines.

## Performance notes

Measured on 2026-09-24, msvc-release, on the i9-10980XE development machine **under load** (other agents' test suites at 14–40% of the CPU and the GPU lock held by others; two runs agree within 1–3% on these rows), so they are upper bounds; the machine state and every number are in the [E7 write-up](../experiments/e7-luau-spike.md#results). The ones that set expectations:

| What | Cost |
|---|---|
| A budgeted call, nothing passed | 74 ns |
| One number in, one `JsonValue` out | 115 ns |
| One scalar property read through a view | 53 ns |
| `on_tick` over 10,000 instances in one VM (four reads, returns nil) | 2.39 ms a tick, 239 ns an instance, no allocation |
| The same with one VM per instance | 24.8 ms a tick, and 300 KB an instance against 174 bytes |
| An empty context: VM pages / live objects | 238 KB / 48 KB, created in 47–56 µs |
| Compile, load and run a 200-line script | 0.45 ms |
| Hot reload of that script, from the poll that sees the edit | 1.5–1.9 ms |
| Polling a watched file that has not changed | 250–300 µs (`io::stat_file`) |
| GC pause (one incremental step) at 0.23–0.38 GB/s of allocation | p99 16–26 µs, max 0.2–0.5 ms |
| Executable size added (scripting + Luau VM and compiler) | 1,689,088 bytes |

The hot path is the property read, not the call: a routine that reads ten properties spends seven times its call overhead on reads.

What is not optimized yet, and would be first: a property read could use Luau's direct userdata field access (`lua_registeruserdatadirectfieldget`, marked experimental at 0.739) instead of an `__index` call frame; enum names could be pinned so a GC cycle never frees one that the next read re-creates; hot reload should watch for change notifications, or stat once rather than three times, instead of polling `io::stat_file`, whose cost is `foundation/io`'s; and a context could share compiled bytecode across loads of the same script. **Poll hot reload at a low rate** (a few times a second), not every frame.

## Not yet

- **Mutations.** Scripts return data; the host issues commands. A script-facing command API — a function that returns a command list the host validates and applies as one transaction — is the natural next step and keeps the journal the only mutation path.
- **Coroutines or scheduling across ticks.** A routine that waits ("walk to the well, then drink") needs a coroutine kept alive between calls, which needs its state saved with the world. Within one call coroutines work; yielding out of a call is an error.
- **Debugging.** No breakpoints, stepping or stack inspection; errors carry the chunk and line.
- **The typed API for agents.** `script.check` and `script.types` over the protocol (engine-host is being changed on another branch); until then the checker is a test helper. A typed `engine.object` (per resolver type) is not designed.
- **The C++ and C# comparison.** E7 asks for the same gameplay feature in C++ hot-reload, Luau and C#; this module is the Luau third ([08 §8.2](../plan/08-toolchain.md#82-gameplay-and-scripting-layer) status note).
- **Per-game script sets.** Where a game's scripts live, how they are named in the document, and a bytecode cache in `ddc/` are not decided.
- **64-bit integers.** Luau 0.739 has an `integer` type behind a flag; until it ships, an `i64`/`u64` field past 2^53 cannot be read.

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, the rest of `foundation/`, the render graph, the scheduler, or another capability, and it can be removed from the build the same way.

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | none | stands alone |
| Component and event types | none of its own: it reads every registered schema type through the descriptors; `tests/scripting_test.schema` is test-only | not needed |
| Tick scheduler entry | `scripting::ScriptingSystem`, phase `scripting::k_phase` (`systems`) | register when the scheduler lands |
| Render-graph passes | none | scripts return data; they do not draw |
| Content-build derived step | none yet | a bytecode cache is a later step |
| Protocol methods | none yet | `script.check`, `script.types` (see "Not yet") |
| Tunables | `scripting.memory_limit_mb`, `scripting.step_budget`, `scripting.time_budget_us` | done |
| LOD policy | `ScriptingSystem::lod_tier()`: tier 0 ticks, tier 1 is dormant; no hysteresis in v0 | done, boundary a placeholder |
| Determinism | `scripting::k_determinism` = `hashed`: a script's output is a function of its bytes, its arguments and the tick, because the sandbox removes the wall clock and unseeded randomness and the budget counts steps rather than time | done; the known holes are above |
| Zero cost when unused | no linked code with the switch off; linked and unused, no VM exists until a `ScriptContext` is made, and the tunables are the only static state | done |
| Tests and size table | `tests/*_tests.cpp`, `tests/size_table.cpp` | done |
| Bench | `bench/scripting_bench.cpp`, plus the two size probes | done |
| Removal proof | `ENGINE_WITH_SCRIPTING`, off in the minimal build | works |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_SCRIPTING=OFF`) drops the module, its tests, its bench, and the Luau download; the module disappears from `build/<preset>/modules.json` and is listed there under `disabled_capabilities`. Everything else builds and passes.
