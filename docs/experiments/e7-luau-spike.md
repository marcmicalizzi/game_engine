# E7, the Luau arm: what a Luau host costs, and what a generated type file buys an agent

- **Question ([10 §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing)):** E7 asks for *the same gameplay feature in C++ hot-reload, Luau and C#*, compared on agent error rate, iteration time and performance, to decide the scripting layer ([08 §8.2](../plan/08-toolchain.md#82-gameplay-and-scripting-layer)). This page is the Luau arm alone: what embedding Luau the way 08 §8.2 recommends — content logic as functions over read-only document state that return data — costs the engine per call, per property, per instance, per reload, in GC pauses, in binary size and in memory; and what a type file generated from the schema changes about the mistakes an agent makes writing a quest condition.
- **Date:** 2026-09-24. **Machine:** Intel Core i9-10980XE, 18 cores / 36 threads, one cache domain, 64 GB, Windows 11. **Build:** `msvc-release` (RelWithDebInfo, x86-64-v3; per-tag memory tracking compiled in, which every VM allocation pays for — see "Caveats"). **Versions:** Luau 0.739, interpreter only (no native code generation).
- **Machine state: not quiet, WARNING raised on every group of both runs.** Two complete runs, one after the other, each group a separate invocation so each carries its own before-and-after sample. Other processes used 14–40% of the CPU (other agents' full test suites) and the GPU was 4–13% busy; the machine-wide GPU lock was held throughout, by another agent's test suite and, for the second run's reload, context and GC groups, by the owner's interactive session on the surround. One group was measured at 99.8% other-process CPU (the first run's `tick.shared`) and its 10,000-instance row is not used; the second run's is. `--require-quiet` could not have been met in the window (a lock held by someone else is never quiet) and the lock was reserved for the owner, so these are **upper bounds**; the two runs agree within 1–3% on every call, read, tick and compile row, and within 20–30% on the reload rows (bound by the file system) and on context creation (bound by page allocation), which is where the load showed.
- **Decision:** none yet, and none possible from one arm. The [08 §8.2 status note](../plan/08-toolchain.md#82-gameplay-and-scripting-layer) records what was built and recommends building the C++ hot-reload arm next and deciding C# from its results rather than building it.

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
