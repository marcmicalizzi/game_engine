# The forest's load-in, traced from launch to ready (2026-10-24)

Roadmap W69. The owner reported long load-ins on the forest POC with stretches of no memory growth, on builds before main at fa343854. This page is the first end-to-end trace of it, taken with `engine-view --startup-trace` ([apps](../subsystems/apps.md#--startup-trace-from-launch-to-ready), [profiling](../subsystems/profiling.md#the-startup-trace)), and what it says the time is spent on. It measures; the fixes it points at belong to the residency work ([04 §4.9](../plan/04-renderer.md#49-streaming-and-residency), W51) and to the agents who own those files tonight, and are listed at the end.

## What was run

- **Engine**: this branch on main at fa343854, `msvc-release` (Tracy compiled in, not connected), `engine-view --offscreen` 1600×900, `--raster hw --shadows csm`, the POC's v7 lab arguments otherwise (`--lod 1 --terrain-tiles --terrain-far 6 --time-rate 0 --world-rate 1 --time-of-day 15 --no-lights --deform-pool-mib 64 --rt-budget-mib 1024`, flora motion on), **no `--game`** (the POC's module is its binary, not run here). The scene is the POC's v7 staged scene (`reports/lab/20261024-005619-110/scene.json`: 59 meshes, 12,330 instances, 2.048 × 3.584 km of native streamed terrain), read-only, with its terrain's tile cache pointed at a copy.
- **Caches**: the POC's `world/v7/ddc` (832 MB: 364 MB of `.clusters`, 339 MB of `.tex`, 149 MB in 7,428 tile files, water) **copied** into a scratch directory, so nothing here could write into the POC's cache. "OS-cold" is a copy made with `robocopy /J` (unbuffered, so its pages are not in the standby list), read for the first time; "OS-warm" is the next run over the same copy. The source assets the scene names (recipes, material PNGs and JPGs on `D:`) were whatever the OS still held. No run here could drop the OS's cache (no administrator), so OS-cold covers the derived data and not the sources.
- **Spawns**: a replay of 2,400 ticks (10 s at 240 Hz) that stands for ten ticks and then holds W, starting at the core (`forest-ground`, −10, 2.16, 23.3), the lake (`south-lake`, 80, 12, 130) and the mountain (the summit, 168, 583.062, 1960) — the POC's own inspection views and its summit walk.
- **Machine**: i9-10980XE (18 cores, 36 threads), RTX 5090, Windows 11 26200, shared with other agents' builds. Each run waited for the CPU under 30% busy for six seconds before it started (the column says what it measured); the summary's `machine_state` is quoted per row. Machine state: the numbers below, WARNING raised on every run (other processes' CPU 7–36% at a run's ends); stage wall times are upper bounds and their `cpu_ms` are not.

## The milestones

Milliseconds from the process's launch.

| run | caches | main | device | scene_loaded | gpu_scene | first_frame | controllable | collision | nearby_stable | others' CPU start/end |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| lake | OS-cold derived data | 654 | 1,161 | 47,692 | 58,091 | 71,758 | 71,758 | 71,758 | **72,203** | 24% / 20% |
| lake | OS-warm | 659 | 1,148 | 8,487 | 13,894 | 16,361 | 16,361 | 16,361 | **16,749** | 33% / 33% |
| lake | evicted by the cold copy's reads | 639 | 1,130 | 45,365 | 57,195 | 71,924 | 71,925 | 71,925 | **72,307** | 13% / 7% |
| lake | OS-warm, quiet-ish | 675 | 1,164 | 7,514 | 12,620 | 15,370 | 15,370 | 15,370 | **15,689** | 22% / 24% |

The first launch of a freshly linked executable adds 1.5–2 s before `main` (2,237 and 2,621 ms against 654–675 on a repeat): the loader and an on-access scan of a new 25 MB binary, which a player's installed build pays once.

Offscreen, a frame waits for its ground's layout, so controllable, collision and nearby-stable arrive with the first frame or within 0.5 s of it: **everything before the first frame is the load-in**, and the player is never ahead of the ground. In a window the layout lags instead, and the same work arrives as the first steps' stall.

## The critical path (OS-warm lake, 15.7 s)

The main thread's serial stages; nothing ran in parallel with them except the terrain and texture workers inside them.

| stage | wall ms | CPU ms | read MiB | what it is |
|---|---:|---:|---:|---|
| before `main` | 675 | | | loader, static initialization |
| `device` | 483 | 422 | 0.8 | Vulkan instance and device |
| `scene file` | 1,793 | 1,797 | 174 | of which `scene expand` 1,728: the tiles provider opened and **every instance stood on the ground** — 12,330 height queries across the whole 7.3 km² region, which read the tile files under them |
| `animation clips` | 223 | 219 | 131 | 13 clip GLBs |
| `load scene` | 4,333 | 3,203 | 2,199 | 64 meshes; `cache textures` 2,815 ms and 1,569 MiB of it over 59 calls, `cluster read` 396 ms and 253 MiB, `source hash` 206 ms and 195 MiB |
| `ground prepare` | 3,377 | 2,531 | 148 | `tile rebuild`: the world's tiles round the spawn, every level, built on the frame thread |
| `gpu scene` | 1,671 | 1,500 | 1,258 | `upload materials` 1,282 ms and 1,146 MiB, `upload geometry` 216 ms |
| `renderer` | 192 | 203 | 116 | pipelines from the driver's cache; **9.1 s** on the first run after a rebuild, when the driver compiled them |
| `ground start` | 1,747 | 156 | 155 | the frame thread waiting on the terrain workers' first fields |
| `session` | 389 | 391 | 224 | the walker: the collision ring's first fill, the tiles under it |
| to `first_frame` | ~450 | | | three frames in flight, read back |

**The process read 4.4 GB before it was ready** (every run, cold or warm), from 832 MB of derived data and the sources. The trace says where the difference comes from:

- **Each source image is read and hashed twice a launch, and some many times.** `load scene`'s `cache textures` (`build_cache_textures`, once per mesh) reads every image a mesh's materials name to find its built `.tex` by the image's bytes — 1.57 GB over 59 meshes, because the meshes share their bark, leaf and rock sets and each mesh reads its own copy — and then `upload materials` (`find_built_textures`) reads and hashes the same files again to find the same keys: another 1.15 GB. Every texture was already built (`built=0` on every mesh). That is the "redundant validation" class: the key is a function of the bytes, and nothing remembers it for the length of the process. It is also a thread pool created and joined per mesh (`jobs::JobSystem pool(...)` in `build_cache_textures`), 59 times.
- **The heights of every placement in the region are read before anything else.** `scene expand` puts all 12,330 instances on the ground through the tile set, so its first read touches the tile files under every tree in the region, near the spawn or 3 km from it; cold, that is 7,428 small files read one at a time on the main thread.

## Cold against warm: where the 56 seconds go

OS-cold over OS-warm, same derived data, same spawn:

| stage | OS-warm ms | OS-cold ms | cold CPU ms |
|---|---:|---:|---:|
| `scene file` (`scene expand`) | 1,873 | **39,644** | 3,469 |
| `load scene` | 5,226 | 6,659 | 3,422 |
| `ground prepare` | 3,456 | 8,509 | 2,531 |
| `ground start` | 1,422 | **12,563** | 266 |
| everything else | 4,772 | 4,828 | |

`before_ready` agrees: of the cold run's 72 s, 19.7 s on a CPU, 5.5 s reading at 4 MiB/s or more, and **46.4 s neither** — the main thread blocked on small reads of the tile files (scene expand: 39.6 s wall over 3.4 s CPU, 171 MiB), then on the terrain workers doing the same for the first fields (ground start: 12.6 s wall over 0.27 s CPU). That is the owner's "stretches of no memory growth": not CPU work, not a lock, not the driver — the tile files arriving one at a time, for placements and levels mostly far from where the player stands. Memory is flat because 7,428 files of 20 KB each grow nothing.

## The spawns

| spawn | caches | scene_loaded | gpu_scene | first_frame | nearby_stable | read before ready | others' CPU start/end |
|---|---|---:|---:|---:|---:|---:|---|
| lake | OS-warm | 7,514 | 12,620 | 15,370 | 15,689 | 4,407 MiB | 22% / 24% |
| core | OS-warm | 7,125 | 12,351 | 14,747 | 17,428 | 4,401 MiB | 18% / 64% |
| mountain | OS-warm | 7,503 | 12,873 | 18,975 | 18,975 | 4,403 MiB | 26% / 37% |
| lake | **uncooked** (an empty cache of our own) | 73,201 | 78,773 | 81,369 | 81,833 | 4,325 MiB | 20% / 6% |
| lake | the same cache, second run | 7,476 | 21,769 | 25,201 | 25,638 | 4,406 MiB | 78% / 49% |

The work before ready does not depend on the spawn beyond the ground round it: `scene_loaded` and `gpu_scene` are within 0.5 s of each other at all three spawns and the bytes read within 6 MiB, because the scene file, the meshes, the textures and the GPU scene are the whole region's wherever the player starts. The mountain's 6 s from `gpu_scene` to its first frame is the renderer and the summit's ground; the core's 2.7 s from its first frame to `nearby_stable` is the world's ring still settling as the walk began. That is what a bounded initial working set would change (below).

**Uncooked**, the derived data is built in the run: the tile set from the authored heightfield in `scene file` (10.3 s), every mesh and texture in `load scene` (61.6 s wall, 40.8 s of the frame thread's CPU and 543 s of the process's — the builds run on the job system's workers), and nothing else changes; the run after it reads what it built, no entry missed (`cache entry ignored` never fired in any run: **no accidental rebuilds**), and its slower `gpu scene` (7.3 s over 2.5 s CPU) is the 78% the other processes held at its start.

## What the trace says the residency work would save

Measured, not fixed here (the files are the residency agent's and the stall agent's tonight):

- **Placement heights for the spawn's neighbourhood only**: `scene expand` is 1.7 s warm and **39.6 s OS-cold**, nearly all of it the region's tile files; a first fill bounded to the spawn's rings reads a few hundred of the 7,428.
- **The image hash kept for the process** (one read per distinct image, shared by `build_cache_textures` and `find_built_textures`): about 2.7 GB of the 4.4 GB read before ready and about 4 s of the warm 15.7 s (`cache textures` 2.8 s, the hashing share of `upload materials` 1.3 s); one pool for all the meshes instead of one each.
- **Meshes and textures for the spawn's neighbourhood first**: `load scene` and `gpu scene` load all 64 meshes and every texture before the first frame (6 s warm, 8.5 s cold), whatever is visible from the spawn.
- **A pipeline cache of the engine's own**: the 9.1 s of `renderer` on the first run of a new build is the driver compiling every pipeline; a `VkPipelineCache` saved beside the derived data would make it the second run's 0.2 s on a player's first launch too.

## What was seen and not fixed

- None of the above was changed: each is in `systems/renderer/src/scene.cpp`, `gpu_scene.cpp` (timing only, tonight) or the tile set's reads (the stall agent's).
- `ground prepare`'s `tile rebuild` runs on the frame thread (2.4 s of its CPU) where an offscreen run waits for it anyway; in a window it is the same work behind the first frame.
- Offscreen hides the window's lag: here readiness follows the first frame because the frame waits for its layout. The window's live session records its stages but not yet the per-frame milestones (apps.md).

## After W75

Roadmap W75 built three of the fixes above in this page's own instrument ([renderer](../subsystems/renderer.md#what-the-load-reads), [texture](../subsystems/texture.md#trusted-not-read), [content_build](../subsystems/content_build.md#one-hash-per-image-and-a-packs-from-its-manifest), [gfx](../subsystems/gfx.md#the-pipeline-cache)): one identity per distinct source image per load and a material pack's from its index, a built texture trusted from its `.tex` header, one job pool per load with the meshes loaded on it, and the engine's own pipeline cache on disk. Measured on the v7 scene at the lake with this page's arguments and replay, offscreen 1600×900, `msvc-release`, the before binary main at d70d34bc and the after the W75 branch on it (both built in the same worktree, copied out so their paths stay fixed), over a scratch copy of the POC's `world/v7/ddc` (the first run of d70d34bc rebuilt the trees into it, G87 having moved `k_flora_version`; every row below reads the same entries). Each run waited for the CPU under 30% for six seconds; `machine_state`'s other processes' CPU at the run's start and end is in the last column. The machine: i9-10980XE (18 cores, 36 threads), RTX 5090 at driver 617.42, Windows 11 26200, shared with other agents.

**Milestones and bytes**, milliseconds from launch:

| run | caches | scene_loaded | gpu_scene | first_frame | nearby_stable | read before ready | peak commit | others' CPU |
|---|---|---:|---:|---:|---:|---:|---:|---|
| before | OS-warm | 7,805 | 12,250 | 13,484 | **13,853** | 4,498 MiB | 13.3 GiB | 7% / 14% |
| after | OS-warm | 3,308 | 7,115 | 8,298 | **8,697** | **2,074 MiB** | 5.0 GiB | 19% / 10% |
| before | OS-cold copy (`robocopy /J`) | 47,299 | 53,444 | 55,488 | **55,861** | 4,498 MiB | 13.3 GiB | 27% / 24% |
| after | OS-cold copy | 40,946 | 46,515 | 48,654 | **49,050** | 2,074 MiB | 5.0 GiB | 21% / 17% |
| before | uncooked (empty cache) | 96,061 | 107,986 | 110,526 | **110,871** | 4,400 MiB | 13.5 GiB | 21% / 15% |
| after | uncooked | 38,746 | 42,707 | 44,778 | **45,445** | 2,449 MiB | 5.1 GiB | 75% / 72% |
| before | that cache, next run | 18,227 | 23,737 | 25,527 | 25,891 | 4,498 MiB | 13.3 GiB | 43% / 52% |
| after | that cache, next run | 3,341 | 7,537 | 8,825 | 9,184 | 2,074 MiB | 5.0 GiB | 19% / 12% |

The uncooked "after" ran beside other work holding 72–75% of the CPU, so its 45.4 s is an upper bound; the "before" row beside it had 15–21%. The before row's next run (25.9 s) was slow in `scene expand` (10.5 s wall over 2.4 s CPU: the tile files it had just written, read back beside 43–52% of other load), not in anything W75 touched.

**The stages W75 changed** (main thread, wall / CPU ms / MiB read while the stage ran):

| run | caches | `load scene` | `gpu scene` | `renderer` |
|---|---|---|---|---|
| before | OS-warm | 4,807 / 3,625 / 2,270 | 1,676 / 1,562 / 1,282 | 172 / 172 / 114 |
| after | OS-warm | **414 / 188 / 659** | 1,063 / 953 / 469 | 168 / 156 / 112 |
| before | OS-cold copy | 6,757 / 3,438 / 2,270 | 2,019 / 1,703 / 1,282 | 178 |
| after | OS-cold copy | **838 / 281 / 659** | 1,476 / 1,125 / 469 | 204 |
| before | uncooked | 84,213 / 57,781 / 2,171 | 2,386 / 1,516 / 1,282 | 190 |
| after | uncooked | **27,169 / 12,516 / 1,034** | 1,053 / 859 / 469 | 305 |

- **Source images read**: before, every texture's source once per mesh that named it in `cache textures` (1,593 MiB over 59 calls and 319 images) and again in `upload materials`; after, **0 bytes and 0 opens** on a cooked load — 146 distinct textures over 332 references, all trusted from their headers, the 24 pack files they come from answered by the pack's index (`textures trusted`: `trusted=146 built=0 hashed=0 from_index=24`). The first after-run over a cache the index did not cover yet read the 24 pack files once (96 MiB) and wrote it. `upload materials` fell from 1,266 ms and 1,170 MiB to 649 ms and 357 MiB: it reads each `.tex` once and no source.
- **Off the main thread**: the 64 meshes load as jobs over 36 workers and the main thread (`meshes` 241 ms wall on the main thread, 1.6–1.7 s of other threads' busy time warm; 73 s of it uncooked, where the trees are built in parallel and the main thread's `load scene` CPU fell from 57.8 s to 12.5 s). The serial loop's 4.4 s of warm mesh work is 0.24 s of wall.
- **Peak commit** fell from 13.3 to 5.0 GiB. Not traced here; the one change that touches it is that the load now makes one job system where it made 59, each of 37 threads, and this is consistent with a per-thread allocator cache outliving its thread (seen, not fixed; below).
- **The new critical path, warm (8.7 s)**: before `main` 0.4 s, `device` 0.5 s, `scene file` 1.8 s (`scene expand`: every placement stood on the ground), `animation clips` 0.2 s, `load scene` 0.4 s, `ground prepare` 2.7 s (`tile rebuild` on the frame thread), `gpu scene` 1.1 s (the upload, serial), `renderer` 0.2 s, `ground start` 0.3 s, `session` 0.3 s, three frames 1.2 s. Cold, `scene expand` is 38.9 s of the 49 s. Neither is W75's (the residency and the tile set's agents' tonight).

**The pipeline cache**, with the driver's own cache pointed at a new empty directory each run (`__GL_SHADER_DISK_CACHE_PATH`), so every launch stands for the first launch of a new build:

| run | `renderer` (wall / CPU ms) | of which `pipelines` | nearby_stable | others' CPU |
|---|---:|---:|---:|---|
| before (no cache of the engine's) | 8,400 / 8,172 | 6,610 | 21,780 | 17% / 14% |
| after, first launch (cache empty, written at exit: 2.0 MiB) | 7,444 / 7,406 | | 15,964 | 12% / 11% |
| after, second launch (cache loaded) | **336 / 328** | | 8,791 | 8% / 6% |
| after, `ENGINE_PIPELINE_CACHE=0` | 8,070 / 8,078 | | 16,671 | 19% / 21% |

**The streamed forest** (`content/test-scenes/streamed-forest`, its camera path, 120 frames, a cache of its own, the repository's material pack absent so its trees draw their recipes' maps): `load scene` uncooked 12,852 → **8,210 ms** (five trees and the terrain built in parallel; 34 textures built in one run of the step), next run 372 → **260 ms** with its 34 textures trusted (others' CPU 12–24% at the starts).

**Seen and not fixed**: the upload (`GpuScene::create`) is still serial after the load and does not overlap the next mesh's read (gpu_scene.cpp, tonight another agent's); a GLB's source hash (195 MiB warm) is the one source read a cooked load still makes; the peak commit's fall above wants its own look at `core/memory`'s per-thread caches; `scene expand` and `tile rebuild` are this page's items (1) and the ground's, unchanged.

## The window (W79)

Roadmap W79 ([apps](../subsystems/apps.md#the-loading-presentation)) moved a window's CPU stages onto a load thread and made the device's stages pump between their slices. Measured on 2026-10-25 with the v7 staged scene at the lake in a **window** — the case this is about, which `--offscreen` cannot show — 1600×900, `msvc-release` on the W79 branch, the arguments above (`--raster hw --shadows csm --lod 1 --terrain-tiles --terrain-far 6 --time-rate 0 --world-rate 1 --time-of-day 15 --no-lights --deform-pool-mib 64 --rt-budget-mib 1024`, flora motion on, no `--game`), a five-second camera path held at the lake spawn (80, 12, 130) instead of a walk, `--no-vsync`. The window was on a **private desktop** of the measuring script's own (`CreateDesktopW`, as the tests' `test_desktop.h` does), never the owner's, and the process took the GPU lock itself (`ENGINE_GPU_LOCK_ON_DEVICE=1`). The derived data was a scratch copy of the POC's `world/v7/ddc` with the scene's tile `ddc` pointed at it (the first run rebuilt W76's tile archives into it; every row below reads them). "Before" is the same binary with `--load-on-main`, which is the load as it was: every stage on the main thread and no message pumped until the hand-over. The machine was shared with other agents' builds all evening and **every row is an upper bound**: others' CPU at the run's start and end is in the last column.

| run | longest gap between two pumps | where | ready (`player_controllable`) from `main` | loading frames | ms a loading frame | others' CPU |
|---|---:|---|---:|---:|---:|---|
| before (`--load-on-main`) | **6,848 ms** | the whole load | 8,843 | — | — | 61% / 47% |
| after, the stages' boundaries pumping | 739 ms | `gpu scene` | 8,627 | 145 | 0.55 | 44% / 46% |
| after, the same | 949 ms | `gpu scene` | 9,181 | 148 | 0.61 | 50% / 41% |
| before | **9,230 ms** | the whole load | 11,251 | — | — | 100% / 93% |
| before | **8,373 ms** | the whole load | 11,022 | — | — | 80% / 89% |
| after, and a pump between texture uploads | **180 ms** | `gpu scene` | 15,726 | 271 | 1.01 | 52% / 43% |
| after, the same | **287 ms** | `gpu scene` | 10,113 | 180 | 0.80 | 93% / 93% |
| after, the same, OS-cold derived data (a `robocopy /J` copy) | **377 ms** | `gpu scene` | 13,098 | 244 | 1.19 | 58% / 80% |

- **The longest unpumped gap** fell from the whole load (6.8–9.2 s: Windows marks a window *Not Responding* after 5 s) to **0.18–0.38 s**, warm and OS-cold. With only the stage boundaries pumping, `gpu scene`'s `upload materials` (one stage of ~0.6 s of texture uploads) left 0.7–0.95 s; one `profiling::progress_pump()` after each texture's upload in `GpuScene::create` took that to under 0.3 s. The load thread's own gaps were 8–88 ms (the main thread waits in 5 ms looks and draws a frame every 33 ms).
- **The pump does not slow the load beyond the noise**: at the quietest comparable pair (others' CPU 41–61%), ready came at 8,843 ms before and 8,627 and 9,181 ms after (−2.4 % and +3.8 %). The rows at 80–100 % others' CPU say only that a busy machine stretches both.
- **A loading frame costs 0.55–1.2 ms** of the main thread (ImGui's layout of one line and a bar, the presenter's clear, overlay and present), 145–271 of them over a load.
- **Not measured here**: the first launch of a new build, where `renderer` compiles its pipelines for seconds — those are one `vkCreate*Pipelines` call each and pump between pipelines only at the stage's boundaries; and `nearby_stable` in a window (the path's five seconds end before the ring settles at the lake, as the offscreen rows above say it settles 0.3–0.5 s after the first frame there).
