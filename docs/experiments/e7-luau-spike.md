# E7: the Luau arm and the C++ hot-reload arm — what each costs, and what each catches of an agent's mistakes

- **Question ([10 §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing)):** E7 asks for *the same gameplay feature in C++ hot-reload, Luau and C#*, compared on agent error rate, iteration time and performance, to decide the scripting layer ([08 §8.2](../plan/08-toolchain.md#82-gameplay-and-scripting-layer)). This page is the Luau arm alone: what embedding Luau the way 08 §8.2 recommends — content logic as functions over read-only document state that return data — costs the engine per call, per property, per instance, per reload, in GC pauses, in binary size and in memory; and what a type file generated from the schema changes about the mistakes an agent makes writing a quest condition.
- **Date:** 2026-09-24. **Machine:** Intel Core i9-10980XE, 18 cores / 36 threads, one cache domain, 64 GB, Windows 11. **Build:** `msvc-release` (RelWithDebInfo, x86-64-v3; per-tag memory tracking compiled in, which every VM allocation pays for — see "Caveats"). **Versions:** Luau 0.739, interpreter only (no native code generation).
- **Machine state: not quiet, WARNING raised on every group of both runs.** Two complete runs, one after the other, each group a separate invocation so each carries its own before-and-after sample. Other processes used 14–40% of the CPU (other agents' full test suites) and the GPU was 4–13% busy; the machine-wide GPU lock was held throughout, by another agent's test suite and, for the second run's reload, context and GC groups, by the owner's interactive session on the surround. One group was measured at 99.8% other-process CPU (the first run's `tick.shared`) and its 10,000-instance row is not used; the second run's is. `--require-quiet` could not have been met in the window (a lock held by someone else is never quiet) and the lock was reserved for the owner, so these are **upper bounds**; the two runs agree within 1–3% on every call, read, tick and compile row, and within 20–30% on the reload rows (bound by the file system) and on context creation (bound by page allocation), which is where the load showed.
- **Decision (2026-09-24):** none yet, and none possible from one arm. **Update 2026-10-10:** the C++ hot-reload arm is built and measured on the same feature, and the two arms' comparison is in [The C++ hot-reload arm](#the-c-hot-reload-arm), with what it decides; the sections above it are the Luau arm as measured on 2026-09-24. The [08 §8.2 status note](../plan/08-toolchain.md#82-gameplay-and-scripting-layer) records what was built and recommends building the C++ hot-reload arm next and deciding C# from its results rather than building it.

## Setup

`foundation/scripting` ([scripting](../subsystems/scripting.md)) is the host: one `ScriptContext` owns a sandboxed Luau VM; scripts are loaded into it, each with its own global table; the host calls a script's named function with arguments under a memory limit and a step budget and gets the first result back as a `JsonValue`. Schema objects reach a script as read-only views whose metatable reads fields through the schema descriptors (`TypeInfo`), with field names resolved by string atom. It is the module the engine would ship, not a harness written for the measurement.

The benchmarks are `foundation/scripting/bench/scripting_bench.cpp`, run through `foundation/bench` (calibrated iteration counts, one warmup, seven timed repeats, medians). Memory, the VM floor and the GC pause distribution are not times, so those benches also print one `{"e7": ...}` JSON line each. The binary-size row is two small executables built beside the bench (`bench/size_probe.cpp`). To reproduce:

```powershell
pwsh tools/dev.ps1 build -Preset msvc-release
$b = "build/msvc-release/foundation/scripting"
& "$b/engine_scripting_bench.exe" --filter=scripting.call.* --require-quiet
& "$b/engine_scripting_bench.exe" --filter=scripting.read.* --require-quiet
& "$b/engine_scripting_bench.exe" --filter=scripting.tick.* --require-quiet
& "$b/engine_scripting_bench.exe" --filter=scripting.compile.* --require-quiet
& "$b/engine_scripting_bench.exe" --filter=scripting.reload.* --require-quiet
& "$b/engine_scripting_bench.exe" --filter=scripting.context.* --require-quiet
& "$b/engine_scripting_bench.exe" --filter=scripting.gc.* --repeats=1 --warmup=0 --require-quiet
dir "$b/engine_scripting_size_probe_*.exe"
pwsh tools/dev.ps1 test -Filter scripting   # the qualitative row prints its diagnostics (ctest -V)
```

The fixtures, in short:

| Bench | What one iteration is |
|---|---|
| `call.empty` | `empty()` with nothing passed and no result asked for |
| `call.roundtrip` | `echo(x)`: one number in, converted back to a `JsonValue` |
| `read.baseline` | `base(o)` with a bound object passed and a number returned, reading nothing |
| `read.ten` | `o.a + o.b + … + o.j`: ten scalar fields (f32, i32, u32, bool, f64, i16, u8) of a bound `Ten` |
| `read.mixed` | `#npc.name`, `npc.faction == "Drifters"`, `npc.position.x`, `npc.attributes.strength`, `npc.alive` |
| `tick.shared/N` | `ScriptingSystem::tick()`: `on_tick(self, dt)` for N bound characters in one VM; the routine reads four properties, branches, and returns nil |
| `tick.separate/N` | the same routine and objects, one `ScriptContext` per instance |
| `compile.200_lines` | `reload()` of a 200-line script: compile, load, run the top level, swap |
| `reload.poll_idle` | `poll_changes()` over 16 watched files, none changed |
| `reload.poll_hit` | `poll_changes()` after one 200-line file was rewritten: stat, read, hash, compile, run, swap |
| `context.create` | construct and destroy a `ScriptContext`: VM, sandbox, engine API, field-name atoms |
| `gc.pauses/N` | one frame: N tables of four fields (one a string built per table) written into a retained 20,000-slot ring, so the live set is steady and each frame's tables replace as many that become garbage; 2,000 frames per run |

## Results

**The call and the property read.** Medians, nanoseconds.

| Bench | Run 1 | Run 2 | What it says |
|---|---|---|---|
| `call.empty` | 74.6 | 73.7 | the fixed cost of a protected, budgeted call: registry lookup, `lua_pcall` under the message handler, the budget's bookkeeping, releasing the call's temporary bindings |
| `call.roundtrip` | 114.6 | 115.0 | +40 ns for one number in and one `JsonValue` out |
| `read.baseline` | 119.3 | 118.6 | the read benches' call: a bound object passed, a number returned, nothing read |
| `read.ten` | 652.5 | 646.6 | **53 ns per scalar property**, (ten − baseline) / 10 |
| `read.mixed` | 537.4 | 521.8 | 81–84 ns per read of a string, an enum's name, a vector's component, a nested struct's field and a boolean — the nested struct is a new 24-byte view each time |

**`on_tick` over many instances**, one shared VM against one VM per instance. Medians per tick; the routine reads four properties, branches and returns nil, so the tick allocates nothing in steady state (the tests assert it for the shared case).

| Instances | One shared VM | One VM each | One each costs |
|---|---|---|---|
| 1,000 | 237 µs — 237 ns an instance | 780–794 µs — 0.79 µs an instance | 3.3× |
| 10,000 | 2.39 ms — 239 ns an instance | 24.8 ms — 2.48 µs an instance | 10.4× |

| Memory per instance | One shared VM | One VM each |
|---|---|---|
| VM pages | 49 B at 1,000, 74 B at 10,000 | 303,656 B |
| Luau's live bytes | 64–74 B | 49,152 B |
| Everything the process allocated | 432 B at 1,000, 174 B at 10,000 (the context's own ~240 KB spread over fewer instances at 1,000) | 306,926 B |

Ten thousand instances are **1.7 MB and 2.4 ms a tick in one VM, and 3.1 GB and 24.8 ms in ten thousand**. In one VM an instance costs a view (a 24-byte userdata), a registry reference and the host's binding slot and tick record; the per-instance tick time does not move between 1,000 and 10,000.

**Loading, compiling and reloading.** Medians.

| Bench | Run 1 | Run 2 |
|---|---|---|
| `context.create` (VM, sandbox, engine API, field-name atoms) | 47.0 µs | 56.0 µs |
| `compile.200_lines` (compile, load, run the top level, swap) | 453.9 µs | 454.5 µs |
| `reload.poll_hit` (one 200-line file changed: stat, read, hash, compile, run, swap) | 1.45 ms | 1.87 ms |
| `reload.poll_idle` (16 watched files, none changed) | 3.99 ms | 4.78 ms |

So an edit saved in an editor is live **1.5–1.9 ms after the poll that sees it**, and the latency an author feels is the polling interval. Polling is not free: 250–300 µs per watched file per poll on this machine, all of it `io::stat_file` (below).

**The VM floor.** An empty context: **238,216 bytes of VM pages** holding **47,908 bytes of live objects** (the libraries, the engine API, the metatables, the interned strings); 240,374 bytes allocated process-wide, the context's own tables included. With the 9-line routine loaded: 320,016 bytes of pages, 49,040 live. The floor is Luau's allocator, not the sandbox: small objects come from 16 KiB pages (32 KiB above 512 bytes) kept per size class, and an empty VM already holds about fifteen of them, mostly empty.

**GC pauses under a steady allocation load.** 2,000 frames per run after the ring is full; a pause is one incremental GC step, timed from the interrupt Luau calls at the start and end of each (`ScriptContext::record_gc_pauses`). Two runs per row; the allocation rate is what the frames sustained back to back, which is far above what a script at 60 frames a second would ask for (66.8 KB a frame is 4 MB/s at 60 Hz).

| Tables a frame | Allocated a frame | Rate during the run | p50 | p90 | **p99** | max | Time in GC |
|---|---|---|---|---|---|---|---|
| 500 | 66.5–66.9 KB | 283–376 MB/s | 0.4–0.6 µs | 1.1–1.5 µs | **16.3–25.4 µs** | 208–291 µs | 36–37% |
| 5,000 | 631–636 KB | 231–314 MB/s | 0.5–0.7 µs | 1.3–1.7 µs | **18.6–26.0 µs** | 246–514 µs | 34–35% |

The p99 pause is **about 20 µs, and at most 26 µs, at 0.23–0.38 GB/s** with a steady 20–23 MB heap, and it does not grow with the allocation per frame: the collector is incremental, and allocating ten times as much per frame makes ten times as many steps (883,000 against 96,000), not longer ones. The total does grow: a third of the time of an allocation-heavy script goes to collection.

**What it adds to a binary** (`msvc-release`). The probe with a script call is 2,889,216 bytes against 1,200,128 without: **+1,689,088 bytes**, of which `.text` +1,381,338, `.rdata` +214,313, `.pdata` +65,424, `.data` +17,116, `.reloc` +16,401. That is the scripting library and Luau's VM, compiler, parser and bytecode writer together. The static libraries as built are much larger because RelWithDebInfo keeps debug information in them — `engine_scripting.lib` 2,459,240 bytes, `Luau.VM.lib` 2,433,482, `Luau.Compiler.lib` 9,574,850, `Luau.Ast.lib` 8,985,106, `Luau.Bytecode.lib` 9,221,538, `Luau.Common.lib` 608,244 — and `Luau.Analysis.lib`, 269,194,448 bytes, is linked only by the tests.

## What the type file catches

Three quest conditions, written the way an agent writes one from a prose description of the quest before it has looked at the schema: **A** is correct; **B** guesses names (`status` for `state`, `hp` for `health`, `requiredLevel` for `required_level`); **C** compares against an enum name that does not exist (`"Complete"` for `"Completed"`), passes the log level as `"information"`, assigns to a property, and returns a number from a function that promises a boolean. Each was checked by Luau's analyser in strict mode with the new solver, three ways: against the generated type file (`emit_type_definitions` over `Quest`, `Character`, `Item`, `Kinds`) with the parameters annotated; with only the engine API's definitions and the parameters unannotated, since without the file there are no types to annotate them with; and with no definitions at all, which is stock `luau-analyze`. The scripts are in `foundation/scripting/tests/typed_api_tests.cpp`, and the test asserts every count below.

| Condition | With the type file | Without it (engine API only) | At run time (first error reached) |
|---|---|---|---|
| A (correct) | clean | 1 false positive: "Unknown type used in < comparison; consider adding a type annotation to 'player'" | returns `true` |
| B (guessed names) | all 3: "Key 'status' not found in external type 'Quest'" (line 3), `hp` (4), `requiredLevel` (5) | 0 of 3; 2 unrelated diagnostics ("Unknown type used in < comparison", "Value of type 'unknown' could be nil") | line 3: "engine.scripting_test.Quest has no property 'status'" — and only that one |
| C (misuse) | 3 of 4: the log level (line 4, "Expected … got '"information"'"), "Property xp of type 'Character' is read-only" (5), "Expected this to be 'boolean', but got 'number'" (6). **Missed**: `quest.state == "Complete"` (3) | 1 of 4, the log level — the engine API's own definitions — plus a solver message on line 5 about an `add<a, unknown>` type function that describes no mistake | line 4: the log level; line 5 would fail next; line 6 never fails, it returns 0 to a host expecting a boolean; line 3 never fails, it is always false |

**Seeded mistakes: 7. Caught before the script runs: 6 with the type file, 1 without it.** Every diagnostic the type file produced was one of the seeded mistakes; without it, four of the five diagnostics were noise an agent would have to read past. The run time catches a missing property or an assignment only on the line it reaches with the inputs it was given — one per run — and catches neither the wrong return type nor the wrong enum literal at all.

**What the type file cannot catch.** Comparing a `QuestState` with `"Complete"` is not a type error to Luau's solver: `==` between a union of string singletons and another string is allowed, so the condition type-checks and is silently always false. It is the most plausible mistake of the seven — the names are close — and it is the one both the analyser and the run time miss. A lint over equality against an enum-typed property, or an `engine.enum` accessor that rejects unknown names, would close it; neither exists.

**What it takes to use.** Stock `luau-analyze` cannot load a definition file (no such flag at 0.739), so the checker here is Luau's analysis library driven directly (`tests/luau_check.cpp`). For an agent to get this while it writes a script, that checker has to be reachable — a `script.check` protocol method is the obvious form, and it is not built.

## What surprised me

1. **A VM per instance is not a way to isolate scripts; it costs 1,800 times the memory.** The obvious sandbox — one VM per NPC, so no script can see another — costs 300 KB an instance, and the live data in it is 48 KB: the rest is the page allocator's per-size-class pages, which is a fixed cost of a Luau state, not of anything the engine put in it. It is also 3.3× slower per tick at 1,000 instances and 10.4× at 10,000, because every call lands in a different, cold VM. Per-script global tables in one VM give the isolation that matters (a script cannot see or change another's globals, and the shared libraries are frozen) for 174 bytes an instance.
2. **The call is cheap; the property read is the cost.** A whole protected, budgeted call is 74 ns; one scalar property read is 53 ns, so a routine that reads ten properties spends seven times its call overhead on reads. Each read is an `__index` metamethod call into the host — a Luau call frame, the atom lookup, the push — and the atom lookup is not the expensive part. Luau 0.739 has an experimental direct field access for tagged userdata that skips the call frame (`lua_registeruserdatadirectfieldget`); it is the first thing to try, and the reason it was not used here is that its API is marked subject to change.
3. **Hot reload is bound by the file system, not the compiler.** Compiling, loading and running a 200-line script is 0.45 ms; noticing that its file changed is 0.25–0.3 ms per watched file per poll, because `io::stat_file` makes three `std::filesystem` calls (status, size, last write time), each its own query of the file system. Where the time goes inside them was not profiled. Polling every frame would cost a millisecond per four scripts. The host polls when asked; a directory change notification, or one stat call instead of three, is the fix, and it belongs to `foundation/io`, not here.
4. **Collection is a third of an allocation-heavy script's time, in pauses too short to matter.** The p99 incremental step is about 20 µs and the worst of 96,000–884,000 steps is half a millisecond, but a script that builds tables every frame spends 34–37% of its time in them. The tick path measured above allocates nothing, which is the shape to keep: return nil on the common path, build a table only for the rare result.
5. **The type file's miss is the most plausible mistake.** `quest.state == "Complete"` against an enum whose name is `"Completed"` passes the analyser and fails silently at run time. See "What the type file catches".

## What it decides

For the Luau arm, that the shape 08 §8.2 recommends is buildable at a cost the numbers above make concrete: a budgeted call is about 75 ns, a scalar property read about 53 ns, and ten thousand scripted instances in one VM tick in 2.4 ms on one thread — 14% of a 60 Hz frame, with no steady-state allocation — and that one VM per instance is not a viable way to isolate scripts (300 KB and 2.5 µs a tick per instance at 10,000, against 174 bytes and 0.24 µs); per-script global tables in one VM are. A game that wants ten thousand scripted routines every tick has to spread them over workers or ticks; one that wants them for quest conditions and the NPCs near an observer does not. It also decides that the type file is worth generating: it moves most of an agent's mistakes from "whichever one a test happens to reach" to "all of them, with line numbers, before anything runs".

What it does **not** decide is E7's question. There is no C++ hot-reload or C# implementation of the same feature, so there is no comparison of agent error rate, iteration time or performance across languages, and nothing here says Luau beats either. The error-rate row is three scripts one agent wrote with mistakes it chose to seed — a demonstration of what the checker sees, not a measurement of how often agents make those mistakes.

## Caveats

- **One machine, one compiler.** Everything is `msvc-release` on the i9-10980XE; the Linux builds ran the tests, not the benches. The interpreter is sensitive to how its dispatch loop is compiled (Luau already turns one MSVC optimization off for `lvmexecute.cpp`), so a GCC or Clang build is likely to differ by more than the usual few percent.
- **Memory tracking was compiled in.** `msvc-release` is RelWithDebInfo, which keeps the engine's per-tag allocation tracking on, and every page the VM allocates or frees pays a sharded lock and a hash-table update for it. That lands on context creation, compile and reload, and the GC rows, not on the call and read rows, which allocate nothing in steady state. A shipping build has tracking off.
- **Interpreter only.** Luau's native code generator was not built; for arithmetic-heavy routines it is typically a multiple faster, and for the property reads measured here, which are metamethod calls into the host, it would change little.
- **The GC rows are one workload.** Small tables with one string each, a steady 20,000-table live set, the default GC goal (200%) and step multiplier; a different live-set size or goal moves the pause distribution.
- **The size row is one executable.** The probes link the engine modules the scripting library needs, so the difference is the scripting library plus Luau's VM, compiler, parser and bytecode writer. A shipping build that loaded bytecode compiled at content-build time could drop the compiler; that split was not measured.

## The C++ hot-reload arm

- **Question:** the same as above, for E7's first candidate — *C++ hot-reload modules (gameplay DLLs, unity builds)* ([08 §8.2](../plan/08-toolchain.md#82-gameplay-and-scripting-layer)) — so that the comparison has two arms: the same feature, the same rows, the same seeded mistakes.
- **Date:** 2026-10-10. **Machine:** the same i9-10980XE, Windows 11. **Build:** `msvc-release` (RelWithDebInfo, x86-64-v3, memory tracking compiled in, as above); the rebuild row also in `msvc-debug`. MSVC 14.51.
- **Machine state.** Every group started on a machine the harness called quiet after `--wait-quiet` (other processes at 6–10% of the CPU, the GPU idle), and some ended less quiet: the GPU lock was taken by another agent's GPU tests during the C++ run 1 and the Luau `compile` group (both CPU-only, but the harness flags a held lock as an upper bound), and the Luau reload group ended with others at 23.5%. `--require-quiet` refused twice because of that lock, so the numbers carry the harness's WARNING and are upper bounds, like the Luau arm's. The Luau rows were **measured again in the same window**, back to back with the C++ ones, so the two columns below were taken under the same load; they agree with the 2026-09-24 numbers within 1–5%. Two C++ runs agree within 1–5% on every row but the swap (5–23% spread, bound by the file system and the loader).

### Setup

`foundation/hotload` ([hotload](../subsystems/hotload.md)) is the host: a `ModuleHost` loads a game module — a shared library built by `engine_game_module()` that links **no engine library** — from a per-load copy, calls its exports through a table of function pointers, and on a change of the watched file loads the new build beside the old one, migrates the state from the old build's schema descriptor to the new one's, runs the new build's `on_load`, and only then swaps and unloads the old. Everything the module needs from the engine comes through a `HostApi` of function pointers; it reads host objects through **views by descriptor**, resolving field paths once in `on_load` and reading by handle (`HostApi::read`) — the Luau binding's read without the VM, with the name lookup moved to load time.

The feature is `game/sandbox_npc`, the Luau fixtures written as C++: `empty`, `echo`, `base`, `ten` (ten scalar fields of a `Ten`), `mixed` (`#npc.name`, `npc.faction == "Drifters"`, `npc.position.x`, `npc.attributes.strength`, `npc.alive`) and `on_tick(self, dt)` (the same reads, the same branches, nothing returned on the common path), over host objects of the same shapes (`foundation/hotload/tests/hotload_test.schema`). The bench is `foundation/hotload/bench/hotload_bench.cpp`; the rebuild row is a compiler run, timed with the build tool:

```powershell
pwsh tools/dev.ps1 build -Preset msvc-release
$b = "build/msvc-release/foundation/hotload"
& "$b/engine_hotload_bench.exe" --wait-quiet=1200 --json=e7-cpp.jsonl
# rebuild.200_lines: change one constant in game/sandbox_npc/routines.cpp (exactly 200 lines),
# then time the module target alone, three times, inside the developer environment:
Measure-Command { ninja -C build/msvc-release engine_game_sandbox_npc }
dir build/msvc-release/bin/game/sandbox_npc.dll, "$b/engine_hotload_size_probe_*.exe"
pwsh tools/dev.ps1 test -Filter hotload   # the seeded-mistake table prints its rows (ctest -V)
```

| Bench | What one iteration is | Luau twin |
|---|---|---|
| `call.empty`, `call.roundtrip` | the export called through the pointer the host hands out; one number in and out | `call.empty`, `call.roundtrip` |
| `read.baseline`, `read.ten`, `read.mixed` | a bound view passed; nothing, ten scalars, or the five mixed reads through `HostApi::read` | the same three |
| `tick.module/N` | `on_tick(self, dt)` for N bound characters, the export's pointer read once a tick | `tick.shared/N` |
| `tick.static/N` | the same routine compiled into the bench, reading the struct: the floor, with no boundary and no views | — |
| `reload.poll_idle` | `poll_changes()` on the watched library, unchanged | `reload.poll_idle` ÷ 16 files |
| `reload.swap` | `reload()`: copy, OS load, describe, checks, migrate, `on_load`, swap, unload the old build | `reload.poll_hit` (stat, read, hash, compile, swap) |
| `rebuild.200_lines` | a saved edit to the module's 200-line body to a rebuilt library, `ninja` on the module target, unity build | `compile.200_lines` |
| `migrate/N` | N state objects through the descriptors, per state | — (a Luau reload keeps no state) |

### Results, beside the Luau arm

Medians; "Luau" is the same rows measured in the same window.

| Row | C++ run 1 | C++ run 2 | Luau | Luau ÷ C++ |
|---|---|---|---|---|
| `call.empty` | 3.1 ns | 3.2 ns | 74.3 ns | **24×** |
| `call.roundtrip` | 5.4 ns | 5.3 ns | 116.5 ns | 22× |
| `read.baseline` | 5.4 ns | 5.5 ns | 119.7 ns | 22× |
| `read.ten` | 75.8 ns | 78.2 ns | 619.9 ns | 8.2× |
| one scalar property, (ten − baseline) / 10 | **7.0 ns** | 7.3 ns | 50.0 ns | **7.1×** |
| `read.mixed` | 49.0 ns | 51.2 ns | 509.2 ns | 10× |
| one mixed read, (mixed − baseline) / 5 | 8.7 ns | 9.1 ns | 77.9 ns | 9.0× |
| `on_tick` × 1,000 | 16.16 µs — 16.2 ns an instance | 16.20 µs | 233.6 µs — 234 ns | **14×** |
| `on_tick` × 10,000 | 165.9 µs — 16.6 ns an instance | 166.5 µs | 2.363 ms — 236 ns | **14×** |
| the routine as engine code (`tick.static`) | 1.9 ns an instance at both sizes | 1.9 ns | — | — |
| host memory per bound instance | 33.6 B (1,000), 37.1 B (10,000) | | 174–432 B | 5–12× |
| a poll of one unchanged file | 240 µs | 227 µs | 256 µs (4.095 ms over 16) | the same: `io::stat_file` |

**From a saved edit to the new code running:**

| Step | C++ | Luau |
|---|---|---|
| Compile | `rebuild.200_lines`: **2.62, 2.49, 2.43 s** in `msvc-release`; 2.37, 2.57, 2.24 s in `msvc-debug` — of which `ninja` with nothing to do is 0.56 s (release) and 0.67 s (debug) | `compile.200_lines`: **0.445 ms** (compile, load, run the top level, swap) |
| Swap | `reload.swap`: **35.6 ms, 34.0 ms**; split (medians of fifteen, quiet): copy 1.0 ms, **OS load 29.4 ms**, migrate and `on_load` 17.6 µs, unload the old build 0.45 ms | included above; `reload.poll_hit` (stat, read, hash, compile, swap) is 1.475 ms |
| State | carried: 37 bytes of schema JSON for `sandbox_npc`'s state; `migrate` is 1.49–1.54 µs a state | none carried: module-level state restarts |
| **Total, from the poll that sees the edit** | **about 2.5 s** (2.2–2.6 s compiling, 31–36 ms swapping) | **about 1.5 ms** |

**What it adds to a binary** (`msvc-release`): the module is a separate file of **55,808 bytes** (76,288 in debug) holding the 200-line body, the fixtures and its state descriptor; the host adds **151,552 bytes** to an executable (`engine_hotload_size_probe_with` 1,290,240 against `_without` 1,138,688: the host, the schema JSON walker it migrates with, and the loader), against Luau's **+1,689,088**.

### The seeded mistakes: agent error rate, like for like

"Agent error rate" here means what the Luau section measured: a set of mistakes seeded into a quest condition the way an agent writes one and, for each, **who catches it before it runs** — for Luau, the analyser with the generated type file; for C++, the compiler with the engine's flags (`/W4 /WX`, `-Wall -Wextra -Werror`) and then the loader (`describe`, the ABI and descriptor checks, and `on_load`, which runs before any export can be called). The condition is the Luau section's condition A as a module (`evaluate(quest, player)`, a `ConditionFn`), seeded one mistake at a time and called over three quests whose right answers are true, false and false. Rows 1–7 are the Luau arm's seven, in its order; rows 8–16 are mistakes C++ can make and Luau cannot, or not in the same form. `foundation/hotload/tests/seeded_mistakes_tests.cpp` compiles, loads and runs every row with the build's own compiler on every test run and asserts each outcome (row 15 is its rename test, row 16 `hotload_tests.cpp`'s incompatible-state test).

| # | Mistake | Luau type file | C++ compiler | C++ loader / run time |
|---|---|---|---|---|
| 1 | `status` for `state` | caught | — | **refused**: "Quest has no field 'status'" |
| 2 | `hp` for `health` | caught | — | **refused**: "Character has no field 'hp'" |
| 3 | `requiredLevel` for `required_level` | caught | — | **refused** |
| 4 | `"Complete"` for the enumerator `Completed` | **missed** (always false at run time) | — | **refused**: "QuestState has no enumerator 'Complete' (it has NotStarted, Active, Completed, Failed)" |
| 5 | log level `information` for `info` | caught | **caught** (`LogLevel::Information` is not a member) | |
| 6 | writes `player.xp` | caught (read-only) | **caught** (there is no write function) | |
| 7 | returns a number where it promised a boolean | caught | **missed**: an integer converts to `bool` (MSVC's C4800 is off by default; GCC and Clang warn about none of it under `-Wall -Wextra`) | **runs, wrong answer** (true for a stage of 2) |
| 8 | keeps a borrowed string (`Value::str`) past the call | n/a (strings are values) | — | loads; **undefined behaviour** once the object is gone (not run) |
| 9 | keeps a view past the call it was bound for | n/a to the type file (the run time refuses a stale view) | — | **the host refuses the read** (`StaleView`, counted and logged); the module runs on with Nil and answers wrong |
| 10 | built against another `module_abi.h` | n/a | — | **refused** (`AbiMismatch`) |
| 11 | an export of a signature the host does not call (`const View&`) | n/a | **caught** (`export_function`'s `static_assert`) | |
| 12 | the entry point written by hand without `extern "C"` | n/a | — | **refused** (`NoEntryPoint`) |
| 13 | state kept in a module global instead of the schema state | the same hazard (module-level state restarts on reload), and the only state Luau has | — | **runs wrong after a reload**: the count restarts |
| 14 | an unbounded loop | not a type error; the **step budget stops it** at run time | — | **hangs the host** (not run) |
| 15 | a state field renamed in the schema | n/a (no state is carried) | — | carried as dropped and added: **reported, not refused, and the value is lost** |
| 16 | a state field retyped incompatibly (u32 → vec3) | n/a | — | **refused**, naming the field; the old build runs on |

**Totals.** The shared seven: **6 of 7 caught before the code runs on both arms**, and not the same six — the C++ loader catches the enum literal the Luau analyser lets through (resolving names once at load makes an unknown enumerator a refusal), and the C++ compiler lets through the integer returned as a boolean that the type file catches. Over all sixteen: compiler 3, loader 7, refused at run time 1, ran wrong 3, not run 2 — **10 of 16 before running**; of the nine C++-only rows, two are process-level failures (undefined behaviour, a hang) that Luau's sandbox makes impossible or turns into a status. Written as the Luau section's conditions B and C, all mistakes at once: B is one refusal listing all three names, as the type file names all three in one check; C takes **three rounds** — the compiler's two errors, then the loader's enumerator, then a wrong answer nobody reports — where the type file names three of C's four in one.

The caveat of the Luau section holds here too: these are mistakes one agent chose to seed, a demonstration of what each checker sees, not a measurement of how often agents make them.

### What surprised me in the C++ arm

1. **The swap is the OS loader scanning a new file, not the migration.** Of the 31–36 ms, 29 ms is `LoadLibraryExW` on a freshly written DLL; the migration and `on_load` are 18 µs and the copy 1 ms. A copy written a few seconds earlier and loaded again takes 5 ms, a fresh one 28 ms, every time (measured with `NativeLibrary.Load` beside the host; real-time protection is on on this machine): the signature of a scan of every new executable image on its first map. The per-load copy does not cause a scan the build would not — the build's output is a new file too — but every reload pays one. Excluding the build tree from real-time scanning would remove most of it; that is a security setting and the owner's call, not this module's.
2. **The boundary costs 8.7× against engine code, and the view read is the cost, as in Luau.** `on_tick` through the module is 16.4 ns an instance against 1.9 ns for the same routine compiled into the engine. The call is 3 ns; the rest is two reads at about 7 ns, each an indirect call, a slot and version check, a type check and a switch on the field's kind. It is still 14× cheaper than Luau's 235 ns, where the call is 74 ns and a read 50.
3. **Resolving names at load catches what neither the type file nor the compiler would.** A misspelt field, a misspelt enumerator and a path through a non-struct are all refusals before any export runs, listed together; the Luau analyser's one miss is the C++ loader's catch. The price is that the loader catches them, after a 2.5 s build, and not the compiler in an editor; a generated header of field-path constants would move them to the compiler, and schemac does not emit one.
4. **A rename silently resets a field.** State migrates by name, so renaming a state field is a removal and an addition: reported in the reload report and the log, not refused, and the value is gone. That is the IDL's own evolution rule and the right default for a document; for a module being iterated on it is a trap an agent will walk into.
5. **A fifth of the rebuild is the build tool.** `ninja` with nothing to do takes 0.56–0.67 s over the tree's graph before it compiles the one unity translation unit; the compile and link are about 1.9 s, the same in release and debug, because the unit's cost is the core headers the schema tables pull in, not optimization.

### What the two arms decide

**Performance: C++, by 22–24× a call, 7–9× a property read, 14× an `on_tick`** (16 ns against 235 ns an instance: ten thousand routines tick in 0.17 ms instead of 2.4 ms), with 5–12× less memory an instance and a tenth of the binary. Engine code with no boundary is another 8.7× (1.9 ns).

**Iteration time: Luau, by three orders of magnitude on the machine's side** — 1.5 ms from the poll that sees an edit to the new code running, against about 2.5 s (2.2–2.6 s compiling, 31–36 ms swapping). For a person tuning a routine in a running game that is the difference between instant and a pause. For an agent the gap is smaller than it reads: an agent's own turn takes seconds to tens of seconds, so a 2.5 s build is a fraction of a round trip, and what costs an agent is the number of round trips — the error-rate criterion. The C++ swap carries state, where a Luau reload restarts it.

**Agent error rate: a tie on the shared mistakes, Luau on the rest.** Both arms catch 6 of the 7 shared mistakes before running (different ones). C++ adds a class of mistakes Luau cannot make — lifetime, ABI, state layout, a hang — of which the loader catches the ABI ones and an incompatible state change, and nothing catches the dangling pointer, the hang, the state kept in a global or the rename. Luau's sandbox turns a hang into a status and has no dangling-pointer class at all; native code takes the host down with it.

**The plan's recommendation stands**, and the numbers say why. **C++ for systems**: the code that runs per entity per tick, where 14× (and 8.7× more inside the engine) matters and the author can be held to the lifetime rules; hot-reloadable modules are worth keeping for iterating on that code with its state carried, at a 2.5 s build. **Luau for content logic**: quest conditions, dialogue, NPC routines — small, numerous, edited constantly — where 1.5 ms iteration and a sandbox matter more than 220 ns an instance, and the cost is affordable (2.4 ms for ten thousand routines a tick). **Declarative data for the bulk**, which this experiment does not touch. Two cheap fixes would close each arm's miss: an `engine.enum` accessor or a lint over enum comparisons in Luau (the Luau section's own suggestion), and a schemac-generated header of field-path constants for C++ modules, which would move rows 1–4 from the loader to the compiler.

**Would a C# arm change it? No, and it need not be built (G47).** On each criterion C# would land between the two measured arms rather than past either: its calls and host reads cross a managed–native boundary (more than C++'s 7 ns a read, plausibly less than Luau's 50), its edit-to-running time is a compile plus a runtime's hot-reload path (seconds, like C++, with edits the runtime cannot apply forcing a restart), and it adds what the plan already counted against it: a runtime of tens of megabytes against Luau's 1.7, a second garbage collector beside the engine's heap, and an ahead-of-time story per platform. Its one plausible win is the error-rate proxy — with generated typed bindings a C# compiler would catch all seven shared mistakes (it does not convert an integer to a boolean) and it has no dangling pointers — but that margin is one row over each arm, and the two fixes above close the same rows in place at none of the cost. The recommendation is to decide G47 from the C++ arm, without building C#, and to reopen it only if Luau's error rate in practice — once the checker is reachable by agents (`script.check`, roadmap G49) — turns out much worse than this proxy. That decision, and G48's, are the owner's.

### Caveats of the C++ arm

- **One machine, one compiler, Windows.** The Linux containers build and run the tests (`dlopen`, `-z defs`), not the benches. The swap's 29 ms is this machine's antivirus; on Linux, or with the build tree excluded, the swap is the copy, the loader and the 18 µs. And on Linux a build with Tracy compiled in (the release presets) never unloads an old build: Tracy replaces `dlclose` with a no-op so it can symbolize closed libraries, so each reload leaves one module mapped ([hotload](../subsystems/hotload.md#building-a-module)).
- **The rebuild is the module alone.** A change to a `core/` header a module includes rebuilds everything that includes it; a change to the host rebuilds the host. Nothing here measures a change that crosses the boundary.
- **A module's view reads are generic.** `HostApi::read` switches on the field's kind on every read, as the Luau binding does; a module that knew its types at compile time could read through typed accessors, and a batch export (one call for N instances) would amortize the call. Neither was built, because the comparison is with the Luau arm's shape.
- **The seeded mistakes are seeded.** Sixteen mistakes one agent chose, each tested on its own (and the Luau section's B and C as written); not a frequency, and not a study of agents writing either language from scratch.
