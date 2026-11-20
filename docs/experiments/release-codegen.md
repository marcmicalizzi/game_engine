# T193: the release preset's inlining and link, measured

- **Date:** 2026-11-18 to 2026-11-19. **Machine:** the owner's i9-10980XE (18 cores, 36 threads), 128 GB, Windows 11, MSVC 14.51, `msvc-release` at x86-64-v3. **State:** shared with other agents' builds and tests throughout, and restarted twice in the middle (a hard reset at 21:59 and Windows updates at 01:42); each table says what the machine was doing.
- **Question.** `msvc-release` is CMake's `RelWithDebInfo`. Every performance figure the project takes comes from it, and nobody had measured what its inlining level and its link cost the engine's own code. T186 found one case where they mattered: an `atomic_ref` word access compiled to a library call reached through an incremental-link thunk, doubling a deque push and pop. `domain/physics` already gives Jolt `/Ob2`, measured at 6.70 against 2.43 ms a step ([physics](../subsystems/physics.md)); `cmake/EngineNav.cmake` measured Recast both ways and found nothing ([nav](../subsystems/nav.md)).
- **Decision, as measured:** the engine's own targets build with `/Ob2` and link `/INCREMENTAL:NO /OPT:REF /OPT:ICF` in `RelWithDebInfo` (`ENGINE_MSVC_RELEASE_CODEGEN=full`, `cmake/EngineReleaseCodegen.cmake`; [what to run](../ci/what-to-run.md#the-release-presets-code-generation-t193)).
- **The default is `cmake` (batch 69's stack, 2026-11-19).** Under `full` the whole `msvc-release` suite failed `profiler` (the filled capture's report counted 37 to 39 of its 40 zones, 2 outside) and `rocks` (the committed escarpment's bytes, 12883758243324567365 against the pin 5793705113405651943), both passing in debug; `full` becomes the default when both are understood and every determinism pin holds under it (a row of the next batch). The measurements below stand.
- **Both are understood, every pin holds, and the default is still `cmake` (T197-2, batch 70, 2026-11-19).** `profiler`'s case was the report's own fault, mended; `rocks`' bytes were [a compiler fault](msvc-ob2-avx2-miscompile.md), mended in source where `/Ob2` reached it and searched for everywhere else. What is left is the fault itself: the canary in `core/base` plants its shape, and its first case fails under `/Ob2` on this compiler by design, so `base` is red under `full` until the build carries a workaround or the compiler is mended (["`full` run whole"](#full-run-whole-t197-2-batch-70)).

## What the preset passes, and from where

Read from `build/msvc-release/compile_commands.json` and `build.ninja`. Modules, tests, benches and `engine-view` get the same compile line:

| | `msvc-release` before T193 (CMake's `RelWithDebInfo`) | a plain `Release` | where it comes from |
|---|---|---|---|
| optimization | `/O2 /Ob1 /DNDEBUG` | `/O2 /Ob2 /DNDEBUG` | `CMAKE_CXX_FLAGS_RELWITHDEBINFO`, CMake's default |
| debug information | `-Z7` (in the object) | none | `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded`, which `tools/dev.ps1 configure` passes for ccache ([the compiler cache](../ci/local-linux.md#the-compiler-cache)) |
| `/Zo` (where inlined code came from) | on | n/a | MSVC turns it on whenever `/Z7` or `/Zi` meets optimization |
| whole program | none (`/GL`, `/LTCG` never set) | none | |
| executable link | `/debug /INCREMENTAL` | `/INCREMENTAL:NO` | `CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO`, CMake's default |
| `/OPT` | `NOREF,NOICF` (the default under `/debug`, and `/INCREMENTAL` refuses `REF`) | `REF,ICF` (the default without `/debug`) | the linker's defaults |

The exceptions already in the tree: Jolt's `/Ob2` (`cmake/EnginePhysics.cmake`), the ASan presets' `/INCREMENTAL:NO` (`cmake/EngineOptions.cmake`), and a game module's relocatable SDK build, linked `/INCREMENTAL:NO` (`cmake/EngineGameModuleBuild.cmake`).

**Why the preset is `RelWithDebInfo` and stays so:** the release presets compile Tracy's zones in, and the CPU sampling done on this machine (cdb stack snapshots of an `engine-view` run, outside the repository) and Tracy's own symbol resolution need each binary's PDB. Every variant below keeps `/Z7` and `/debug`.

## The variants

One build tree, built three times: (a) as above; (b) `/Ob2` on the engine's own targets; (c) (b) plus `/INCREMENTAL:NO /OPT:REF /OPT:ICF` on their executables and modules. Third-party targets and the host tools (ADR-0034) keep `/Ob1` in all three. Every variant carries T186's deque and T193's `Counter` mend.

## Build time and size

| | (a) `/Ob1`, incremental | (b) `/Ob2`, incremental | (c) `/Ob2`, `/OPT:REF,ICF` |
|---|---|---|---|
| executables and DLLs (173) | 1,337 MB | 1,362 MB | **644 MB** |
| `.ilk` files | 3,820 MB | 4,055 MB | none (two stale, 8 MB) |
| PDBs | 14,463 MB | 15,607 MB | 15,607 MB |
| objects (2,031) / libraries | 3,439 / 718 MB | 3,559 / 730 MB | as (b) |
| the release tree | 24.5 GB | 26.0 GB | **21.4 GB** |
| `engine-view.exe` | 62,892 KB | 63,708 KB | **43,486 KB** |
| the 40 benches | 246.7 MB | 251.6 MB | 69.6 MB |

**Compile time.** The whole of (b), every compile a cache miss, took 826 s at `-j8` (others' builds absent at its start, present for part of it). One translation unit at a time with no cache, beside other work: `scene_renderer.cpp` 123 s with `/Ob1`, 120 s with `/Ob2`, its object 34.14 and 34.13 MB; `gpu_scene.cpp`'s object 6.58 against 6.60 MB. So `/Ob2` costs the compile nothing measurable here: the engine's hot code is in headers and classes `/Ob1` already inlined, and what `/Ob2` adds is the small unmarked helpers. **Link time.** (c) relinked its 171 executables and modules in 26 s at `-j8`; a non-incremental link writes the whole image every time, which a one-file edit's relink notices more than a whole build does (not measured). The size falls because an incremental image pads every function for later patching and keeps an `.ilk` beside it, and `/OPT:REF,ICF` drops and folds what nothing calls.

**ccache.** The flags are in every object's cache key, so the first build of `msvc-release` after this merges is a whole recompile in every checkout (about 3.5 GB of objects and the 15 GB of PDBs written again a tree); it is paid once.

## What must not move: the determinism pins

The tests that hold a toolchain-independent answer, run in each variant with no driver (`tools/dev.ps1 test -Preset msvc-release -NoDriver -Jobs 3 -Filter '^(geometry|texture|content_build|flora|landform|walls|tileset|soil|physics|world|matgen|engine_content|engine_cli)$'`): the geometry, texture and content build golden hashes, the landform, walls, tileset, soil and flora determinism cases, physics' state, the world's save and `engine_cli`'s NPC replay state hashes. **13 of 13 passed in each of (a), (b) and (c)**; no value moved, nothing was re-pinned. That is the expected answer: `/Ob` changes which calls are inlined, never the arithmetic, and MSVC's `/fp:precise` contracts nothing either way (ADR-0035).

**Corrected the same day (T197): the list was short, and the sentence above is true of the language and not of this compiler.** `rocks` was not among the thirteen, and under (b) and (c) its escarpment pin moves: MSVC 19.51 leaves a by-value argument's second copy unwritten once `weathered_uv` is inlined, which `/Ob2` allows and `/Ob1` did not. The fault is the compiler's, `/Ob2` only reaches more of it, and main's `/Ob1` reaches it too wherever the same shape sits in an `inline` function: [the miscompile, its reduction and the ways out](msvc-ob2-avx2-miscompile.md). Thirteen suites holding in all three variants showed that those thirteen builders do not contain the shape, not that inlining is safe.

## The benches

**The machine was never quiet.** The window the coordinator cleared on 2026-11-19 from 07:25 had other agents' renderer and `engine-view` tests in it (others' CPU 24 to 59 % at each bench's start), and every bench's 60 s `--wait-quiet` ran out, so that window yielded one run of (a). The pairs were then taken as a rotation a, b, c, a, b, c from 08:53 to 09:35, `--repeats=5`, no driver, beside the merge stack's builds, this row's own affected test run and a CPU-only bench of its own: **others' CPU at each bench's start had a median of 97 % (61 to 100 %)**. Read every row below as an upper bound taken under load. The pairs ran over the frame's CPU side and the core: `jobs`, `math`, `anim`, `animation`, `events`, `geometry`, `nav`, `scene_gen`, `sim`, `scatter`, `scene_collision`, `world` (102 rows). Set aside for time, one run each or none: `landform` (25 min a run: a 2,048² solve a repeat), `physics` (15 min, Jolt's own `/Ob2` in every variant), `npc` (7.5 min), `terrain` (14 min), `renderer` (over 30 min with no driver), `matgen`, `doc`, `ecs` (flecs, third party), and the small modules outside the frame.

A row **moved** when the two variants' run medians do not overlap and differ by more than the larger of the two run-to-run ranges and the within-run deviations (`analyze.py` in the row's scratch):

| row | (a) `/Ob1` | (b) `/Ob2` | (c) `/Ob2` + `/OPT` | moved |
|---|---|---|---|---|
| `jobs.deque.push_pop` (256) | 5.82 µs | 2.59 µs | 3.65 µs | b −56 %, c −37 % (spread 36 %) |
| `animation.tick.lod3` | 120.0 µs | 62.2 µs | 67.8 µs | b −48 %, c −44 % (spread 20 %) |
| `jobs.nested.saturated/8` | 7,850 µs | 4,426 µs | within | b −44 % (spread 39 %) |
| `world.store.tile` | 983 µs | 768 µs | within | b −22 % (spread 21 %) |
| `animation.pool.churn/4096` | 41.9 µs | 51.2 µs | within | b +22 % (spread 6 %) |
| `animation.pool.churn/256` | 2.91 µs | 3.20 µs | within | b +10 % (spread 8 %) |
| `anim.clip.sample.keys/241` | 4.85 µs | 5.65 µs | within | b +17 % (spread 15 %) |
| `anim.skeleton.local_matrices.one` | 0.23 µs | 0.31 µs | within | b +38 % (spread 12 %) |
| `sim.tiers.assign/8` | 1,207 µs | within | 1,861 µs | c +54 % (spread 50 %), also against b |

**a against b: 8 of 102 rows moved, 4 faster and 4 slower; a against c: 3 moved, 2 faster and 1 slower; 99 inside their spread.** The faster rows are the ones the question was about: the job system's push and pop (T186's thunk, now gone for every word the deque touches), a nested job tree, the animation tick and a world store's tile. The slower ones are sub-microsecond `anim` rows and the pool's churn in (b) alone, and a multi-threaded tier assignment in (c) alone whose spread is half its value; none of them moved in both (b) and (c), which is what a real cost of `/Ob2` would do. On a machine at 97 % that is all the speed this can claim: **no row is slower in both inlining variants, the hot paths that moved got faster, and the rest is inside the noise.** A quiet re-take of this rotation (the same command, the row's `bench-run.ps1`) is what would settle the small rows.

**Why `full` and not `inline`:** the link is where the certain gain is (half the binaries, no `.ilk`, 3 GB less tree, less written on every relink: the owner watches the drives), it changes no code, and with `/OPT:REF,ICF` the incremental thunks the T186 finding went through are gone.

## `full` run whole (T197-2, batch 70)

2026-11-19, the tree at batch 70's base with `rocks`' mend, `build/msvc-release` configured `-DENGINE_MSVC_RELEASE_CODEGEN=full`, a second tree beside it configured `cmake` for the pairs. The machine was shared all evening (five rows and the other coordinator's device runs); each figure says what stood beside it.

**The suite, no driver** (`tools/dev.ps1 test -Preset msvc-release -NoDriver -Jobs 3`, beside two scans compiling at 22 jobs): 179 of 183 pass.

| failed | what it was |
|---|---|
| `base` | **the canary's first case: 480 of 960 values wrong, as it is under `/Ob2` on this compiler by design.** The one failure that is `full`'s |
| `docs_check` | this row's own script citing a section not yet written; passes since |
| `jobs` | "parallel_for covers the range exactly once and runs on workers": no worker took part. Run again by case, ten rounds, the two builds in turn, others' CPU 80 to 100 %: **4 of 10 fail under `full`, 2 of 10 under `cmake`**. A saturated machine, not the flags |
| `scene_collision` | a wall-clock case (135.9 ms against a bound of 120). The same ten rounds: 10 of 10 pass on both builds |

**The pins**, all in that run: `rocks` (126 cases, the escarpment's bytes), `geometry`, `texture`, `content_build`, `flora`, `landform`, `walls`, `tileset`, `soil`, `physics`, `world`, `matgen`, `math`, `engine_content.1` and `.2`, `engine_cli` and `engine_cli.npc_replay`, and `profiler`: **every one passes, no value moved, nothing re-pinned.** `tools.check_no_listeners` passes.

**The end-to-end tests on the device** (`-Jobs 1`, one GPU process at a time, the GPU's line long all evening): of 18 tests, 13 pass: `tools.game_sdk`, the three `*.generators`, `engine_host`, `engine_host.host_ws`, `engine_cli`, `engine_cli.gpu`, `engine_cli.npc_replay`, `engine_editor`, `mcp_bridge` and its two `mcp_check` tests. `engine_view.frame_loop` **failed one case of seven**, "a streamed world's swaying trees allocate nothing in their second flight": 7 allocations in frames 54 to 69, no buffers, flown beside 24 compile jobs. Run again by name once those had ended, the two builds in turn (others' CPU 5 to 48 %): **0 allocations under `full`, twice, and 0 under `cmake`, twice**. The load, not the flags. `engine_view.1` to `.4` each stood half an hour in the GPU lock's line and gave up (exit 75): no verdict.

**The benches**, the rotation T193 could not take quiet: cmake, full, cmake, full, `--repeats=5 --wait-quiet=60`, no driver, 19:11 to 19:36; others' CPU at the runs' starts 3 to 74 % (the wait ran out every time, so not quiet; T193's rotation stood in a median of 97 %). 72 rows of `jobs`, `math`, `anim`, `animation`, `geometry`. A row **moved** when both `full` takes lie outside both `cmake` takes on one side and the means differ by over 10 %.

| row | `cmake` (two takes) | `full` (two takes) | |
|---|---|---|---|
| `animation.tick.lod3` | 78.7 / 78.2 µs | 21.0 / 22.2 µs | −72 % |
| `jobs.deque.push_pop` (256) | 3.29 / 3.28 µs | 2.28 / 2.19 µs | −32 % |
| `animation.graph.control/100000` | 5.94 / 3.69 ms | 3.35 / 3.39 ms | −30 % (the `cmake` takes 61 % apart) |
| `animation.graph.control/10000` | 541 / 341 µs | 308 / 307 µs | −30 % (as above) |
| `jobs.deque.push_steal` | 5.05 / 5.06 µs | 3.90 / 4.38 µs | −18 % |
| `anim.clip.sample.keys/61` | 3.75 / 4.57 µs | 3.44 / 3.47 µs | −17 % |
| `animation.graph.additive/1000` | 5.08 / 4.00 ms | 3.90 / 3.94 ms | −14 % |
| `animation.graph.control/1000` | 33.6 / 33.3 µs | 29.3 / 29.6 µs | −12 % |
| `animation.graph.layered/10000`, `sample/10000` | 47.3 / 38.7, 37.3 / 30.5 ms | 37.8 / 37.7, 29.7 / 29.7 ms | −12 % |
| `geometry.limit.build.fixture` | 11.9 / 11.6 ms | 10.9 / 9.7 ms | −12 % |
| `jobs.parallel_for.small/8` | 13.6 / 14.3 µs | 16.5 / 18.5 µs | **+26 %** (others' CPU 74 % at the last `full` run's start) |
| `anim.skeleton.skinning.crowd` | 198.9 / 197.4 µs | 269.3 / 215.8 µs | **+22 %** (the `full` takes 25 % apart) |
| `anim.skeleton.skinning.one` | 0.72 / 0.74 µs | 1.02 / 0.74 µs | +21 % (one `full` take is `cmake`'s) |

**All 72 rows: `full` is 4.7 % faster as a geometric mean, the two `cmake` takes differing between themselves by 4.4 %; `math` −0.6 %, `anim` +0.3 %, `geometry` −3.7 %, `jobs` +8.5 % (six rows: two moved faster, one slower), `animation` −11.9 %.** T193's findings hold where they were clear (the deque, the animation tick) and its doubtful slower rows (`animation.pool.churn`, `anim.clip.sample.keys/241`, `local_matrices.one`) are not among the rows that moved here; `parallel_for.small/8` and the skinning rows are the ones a quiet take should look at.

**The frame budgets, a pair for three runs of the six** (`tools/frame-budget.ps1 -Exe <each tree's engine-view> -WaitQuiet 120`, `cmake` from 20:01 and `full` from 20:50, the GPU lock's line up to six deep, about ten minutes a flight): **no run flown under `full` is over its budget.**

| run | `cmake`: GPU total, CPU (median / p99, ms) | `full` | |
|---|---|---|---|
| `erg-walk-1080p-rt` | 0.720 / 1.043, 0.232 / 0.876; pass, quiet | 0.726 / 1.271, 0.225 / 0.640; pass, others' CPU 7 to 11 % | within |
| `endless-1080p-csm` | 0.975 / 1.626, 0.442 / 0.767; pass, others' CPU 9 to 13 % | 0.988 / 1.813, 0.432 / 0.711; pass, 7 to 13 % | within |
| `endless-surround3-rt` | 4.287 / 5.646, 0.587 / 0.922; three p99 lines over on a busy machine (inconclusive), session unlocked | 4.094 / 4.974, 0.542 / 0.889; pass, **session locked**, so its other baseline | not a pair: the session state differs |
| `erg-walk-surround3-rt` | 3.787 / 4.613, 0.339 / 0.997; pass, quiet | not flown | |
| `overlook-1080p`, `static-1080p-rt` | cannot be flown in this checkout: `content/samples/Lantern` is fetched, not committed, and the row downloads nothing | | |

The CPU's side of a frame is the same or a little less under `full` in each run flown on both (the medians 0.225 against 0.232, 0.432 against 0.442 ms), and the GPU's is the same, as it should be: inlining does not reach it. **What `full` still owes on the device:** the other three budget runs as a pair, flown where the samples are (the gate's tree), and the four `engine_view` parts that gave up waiting.

## Stacks

One cdb snapshot of variant (c)'s `engine_geometry_bench` with its PDB, during `geometry.binding_apply`: `engine::operator*+0x6 [inlined in …transfer<1,0>+0x137]`, then `transfer<1,0>`, `transfer_terms<1>`, `apply_binding`, the bench runner, `main`. The inlined call is still named and placed, from the `/Zo` information, so a sampler that reads inline frames loses nothing in that stack. A debugger steps less predictably through `/Ob2` code than `/Ob1` (inlined calls have no frame to stop in); that was not tried here, and a session that needs it sets `-DENGINE_MSVC_RELEASE_CODEGEN=cmake`.
