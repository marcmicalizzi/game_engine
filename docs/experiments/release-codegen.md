# T193: the release preset's inlining and link, measured

- **Date:** 2026-11-18 to 2026-11-19. **Machine:** the owner's i9-10980XE (18 cores, 36 threads), 128 GB, Windows 11, MSVC 14.51, `msvc-release` at x86-64-v3. **State:** shared with other agents' builds and tests throughout, and restarted twice in the middle (a hard reset at 21:59 and Windows updates at 01:42); each table says what the machine was doing.
- **Question.** `msvc-release` is CMake's `RelWithDebInfo`. Every performance figure the project takes comes from it, and nobody had measured what its inlining level and its link cost the engine's own code. T186 found one case where they mattered: an `atomic_ref` word access compiled to a library call reached through an incremental-link thunk, doubling a deque push and pop. `domain/physics` already gives Jolt `/Ob2`, measured at 6.70 against 2.43 ms a step ([physics](../subsystems/physics.md)); `cmake/EngineNav.cmake` measured Recast both ways and found nothing ([nav](../subsystems/nav.md)).
- **Decision:** the engine's own targets build with `/Ob2` and link `/INCREMENTAL:NO /OPT:REF /OPT:ICF` in `RelWithDebInfo` (`ENGINE_MSVC_RELEASE_CODEGEN=full`, `cmake/EngineReleaseCodegen.cmake`; [what to run](../ci/what-to-run.md#the-release-presets-code-generation-t193)).

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

## Stacks

One cdb snapshot of variant (c)'s `engine_geometry_bench` with its PDB, during `geometry.binding_apply`: `engine::operator*+0x6 [inlined in …transfer<1,0>+0x137]`, then `transfer<1,0>`, `transfer_terms<1>`, `apply_binding`, the bench runner, `main`. The inlined call is still named and placed, from the `/Zo` information, so a sampler that reads inline frames loses nothing in that stack. A debugger steps less predictably through `/Ob2` code than `/Ob1` (inlined calls have no frame to stop in); that was not tried here, and a session that needs it sets `-DENGINE_MSVC_RELEASE_CODEGEN=cmake`.
