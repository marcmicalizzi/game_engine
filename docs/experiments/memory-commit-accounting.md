# Where the forest's commit goes (T129, 2026-11-13)

**The question.** Walking the forest proof of concept at 3840 × 2160 with traced primaries, traced shadows and the bounce, the owner saw **75.9 GB of private commit before the first frame, 79 GB later, and a working set of 22 GB** (the forest session's engine needs 79 and 83). The same world two days earlier committed 65.7 GB with a 36 GB working set. On a machine with 64 GB of memory and a commit limit of 118 GB, that leaves 8 GB free during a walk, and one load died of it. Who holds the commit nobody touches?

**The instrument.** `engine-view --memory-report` ([apps](../subsystems/apps.md#--memory-report-where-the-commit-is)): the address space walked by kind and by state with each allocation's resident share ([platform](../subsystems/platform.md#where-the-commit-is)), the owners the engine can name, the engine heap's live blocks of 16 MiB or more by call stack ([memory](../subsystems/memory.md#who-sized-the-largest-blocks)) and the device's buffers of 16 MiB or more by call stack ([gfx](../subsystems/gfx.md#what-the-device-holds)). Stacks were symbolized afterwards with `cdb -z engine-view.exe` against the run's own PDB.

## What was run

- **The world**: the forest's staging copy on this main (`<scratchpad>\v41\scene-staging.json`, v41 migrated to b7681ac9; the derived-data cache `<scratchpad>\v41\ddc`, `load.materials` `D:/workspace/forest_wildlife_poc/world/v35/materials`). It is not the owner's v43 world: it has no sea and no islands, no game module, no animals and no player.
- **The settings**: the owner's lab run `20261113-121148-576`, its renderer values only: `--width 3840 --height 2160 --raster rt --shadows rt --no-lights --time-of-day 15 --world-rate 1 --lod 1 --deform-pool-mib 64 --rt-budget-mib 0 --terrain-tiles --terrain-far 6 --time-rate 0`, `renderer.flora.motion=true`, `renderer.flora.wind_strength=0.7`, `renderer.shadow.micromaps=true`, `renderer.gi.mode=bounce`; offscreen, never a window.
- **Two shapes of run**: a **warm load** of six frames (the staging's own camera), and a **hop flight**: 75 views, the first key of each of the v40 paths, one a second (`<scratchpad>\b64\T129\hop-path.json`), the shape of the forest's own `views.json` that took v43 from 78.5 to 10.8 GB of free commit.
- **The machine**: RTX 5090 (32 GB), 64 GB of memory, commit limit 117.7 GB, shared with two coordinators' agents building, testing and flying (S105's water tests and T131's engine-view held the GPU between my leases; my own debug build ran beside P2). A commit figure does not move with load the way a time does, but a working set does: under memory pressure Windows trims working sets, so a resident figure from a run whose free commit fell low (P2's first load: 8.2 GB) is a floor, not the process's own.

| Run | Binary | Whole | Peak private (sampler) | Peak working set | Free commit start / lowest |
|---|---|---|---|---|---|
| C1, cold load | main (0aa8e44a) | 289 s | 31.9 GB | 17.0 GB | 71.1 / 39.9 GB |
| W1, warm load | main | 179 s | 32.0 GB | 17.1 GB | 69.3 / 36.7 GB |
| F1, hop flight | T129 r1 | 381 s | 41.7 GB | 16.4 GB | 68.5 / 26.6 GB |
| F2, hop flight | T129 r2 | 328 s | 41.7 GB | 16.5 GB | 66.5 / 14.7 GB |
| P2-1A, warm load | T129 r3 (report, no cut) | 223 s | 32.0 GB | 17.1 GB | 43.5 / 8.2 GB |
| P2-4B, warm load | T129 r4 (report and the merge cut) | 217 s | 31.4 GB | 15.9 GB | 42.0 / 24.8 GB |

**The staging does not reproduce the owner's 76 to 79 GB**: a warm load commits 32 GB and the hop flight 42 GB. The owner's v43 world is larger (the sea, the islands, the falls), and the walk ran a game module with 416 animals and a player; the split below is the staging's, and the forest session should run its walk once with `--memory-report` to give its own (a window logs the two `view.memory` lines; see "Not measured").

## The split

At the flight's exit (F2) and the warm load's (P2-1A), in MiB:

| Who | Hop flight committed | of which resident | Warm load committed | Line that sizes it |
|---|---|---|---|---|
| **The driver's backing for device-local memory** | **16,237** | ~0 | **13,913** | every device-local `gfx::create_buffer` and image (`domain/gfx/src/device.cpp`, `create_buffer` → `vmaCreateBuffer`): one byte of private commit per byte on the GPU |
| The C runtime's heaps (all `malloc`, the engine heap inside) | 17,519 | ~16,000 | 17,217 | — |
| of which the engine heap, live | 15,830 | | 15,526 | `mem::allocate` |
| of which the merged scene geometry (CPU copy) | 4,672 | 3,608 | 4,621 | `geometry::merge_cluster_meshes` ← `renderer::load_scene` |
| The device's host-visible blocks | 812 | 477 | 512 | upload, readback, the flora poses |
| Private with no named owner | **7,710** | ~0 | 1,029 | the driver's other allocations; see below |
| Mapped files | 11 | 3 | | |
| Images (the executable, DLLs) | 353 | 65 | | |
| **The process's commit** | **42,376** | WS 16,313 | **32,751** | |

**The driver's backing, measured alone** (`domain/gfx/tests/device_memory_tests.cpp`, RTX 5090, driver of 2026-11): **512 MiB uploaded to a device-local buffer raised the process's commit by 514 MiB and its private memory by 513 MiB, of which 1.2 MiB resident**; a 256 MiB host-visible buffer raised the commit by 256.5 MiB. On Windows the driver reserves, as private commit in the process, a system-memory backing for every device-local allocation, which nothing touches unless the GPU memory is evicted. In the walk the regions show as private, `PAGE_READWRITE | PAGE_WRITECOMBINE` (`0x404`), 16 to 64 MiB each: 450 allocations, 14,340 MiB committed, 10 MiB resident on the warm load, against 13,913 MiB of device-local blocks. **Every byte the engine puts on the GPU costs a byte of the commit limit**: the GPU's 19.2 GB in the owner's walk is 19 GB of the 57 GB nobody touches there. Linux charges nothing for it.

**What the GPU holds, by the line that sized it** (F2, the device's buffers of 16 MiB or more, 12,482 MiB of the 16,237; the rest is images and smaller buffers):

| MiB | What | Line |
|---|---|---|
| 2,848 (6 chunks, largest 552) | the traced chain's cluster storage | `systems/renderer/src/gpu_scene_rt_step.cpp`, `make_rt_step`: `create_cluster_storage(device, limits, out.chunks[k])` with `limits.max_clusters = rt_chunk_size(in.capacity, ...)`, every cluster reserved at the driver's most |
| 448 + 277 + 83 | the chain's bottom-level set, its scratch, the top level | `make_rt_step` (`create_cluster_blas_set`, `create_scratch`); `lay_out_ray_tracing` (`create_tlas`) |
| 506 + 353 | the structure cache; `create_ray_tracing` | `GpuScene::create_rt_cache`, `GpuScene::create_ray_tracing` |
| 1,410 + 940 + 768 + 705 + 470 + 99 + 99 | the scene's geometry (positions, attributes, triangles, the cluster tables) | `GpuScene::upload_geometry` |
| 384 + 256 + 256 | the pair tables' working set and table sets | `GpuScene::fit_tables` (`create_working_set`, `create_table_set`) |
| 359 + 201 (host-visible) + 192 | the flora's parts, poses and the relayout | `GpuScene::create_flora`, `make_flora_poses`, `relayout` |
| 348 | materials | `GpuScene::upload_materials` |
| 316 + 294 | the opacity micromaps' packed set and bake | `GpuScene::build_static_forms`, `gfx::bake_opacity_micromaps` |
| 256 | terrain | `GpuScene::create_terrain` |

The traced chain is the one worst-case-shaped consumer: its storage is reserved at the driver's most per cluster for the step's capacity (a step above what recent frames built, R152), about 3.6 GB on the GPU and so 3.6 GB of commit. Everything else is the scene's data at its size.

**The merged scene geometry on the CPU** (the engine heap's largest blocks, F2): positions 1,327 MiB (`Vector<Vec3>`), attributes 885, `u32` streams 520, 442 and 387, quantized 664, cluster and LOD descriptors 108 each — 4.67 GB committed, 3.6 GB resident. The 1.06 GB not resident was the arrays' doubling: `merge_cluster_meshes` appended part by part, so each array's last growth committed up to twice what it held. The CPU copy itself is touched and kept for the session (the acceleration structure builders' float positions, the static forms, collision and the pickers read it).

**Smaller blocks committed whole and never touched** (F2): `world::PlacementTiles::create`'s `Vector<SceneInstance>` 64.6 MiB, 0.1 resident (`systems/world`, the other coordinator's module tonight); `GpuScene::fit_tables`' `Vector<FloraInstance>` 21.7 MiB, 1.2 resident; the JSON reader's vectors 16 MiB, 5 untouched.

**Private with no named owner grows with frames** (1.0 GB after the warm load's six frames, 7.7 GB after the hop flight's 75 and 7.8 GB after 120 frames held at the south lake, F3 with r4). F3 by protection: the write-combined regions (`0x404`) are 525 allocations, 16,671 MiB committed, 32 MiB resident, against 16,243 MiB of device-local blocks — the driver's backing and nothing else; plain read-write (`0x4`) is 3,485 allocations, 24,668 MiB committed, 16,220 resident, and the C runtime's heaps account for 16,944 of it. **So the 7.8 GB no owner names is ordinary read-write memory, outside every heap, never touched, in 4 to 16 MiB allocations (the size bucket holds 2,535 of them, 18.7 GB committed, 10.7 resident), and it appears over the first hundred frames.** Not the driver's backing of freed GPU memory (that would be write-combined), not the engine heap (its blocks are the heap's). The candidates were the driver's own host-side pools and another library's direct `VirtualAlloc`; it was the second (next section).

## Who made the unnamed commit

**It is the profiler's allocator, once per thread that names itself, and the threads were two per tile mesh.** Two small scenes at the same settings (the grove, the streamed forest: no tile meshes) hold 0.7 GB with no owner and do not grow over 150 frames, so the growth is not frames. On the staging it comes in the ten seconds the world's tiles arrive (F3's log: tick 0 activates 1,256 tiles, tick 1 deactivates and reactivates them, 4,850 meshes loaded, 780 of them running the texture pass). **D1**: the same run (r4, the south-lake path, 30 frames), with `cdb` attached once the scene's ray tracing was ready and a breakpoint on `NtAllocateVirtualMemory(Ex)` printing every request of 4 MiB or more with its stack (`<scratchpad>\b64\T129\leg2\bp.txt`, `cdbload.ps1`). 2,138 requests after the load:

| Requests | MiB | Type | Stack |
|---|---|---|---|
| **1,558** | **6,232** | reserve and commit, read-write, 4 MiB each | `tracy::_rpmalloc_mmap_os` ← `tracy_malloc` ← `tracy::SetThreadNameWithHint` ← `jobs::JobSystem::worker_main` |
| 39 | 156 | the same | Tracy's thread-locals at a thread's start (`__dyn_tls_init`) |
| 541 | 5,474 | commit inside a reserved range | `malloc` (the C runtime heap's segments: a heap, counted as one) |

The exit's split matched F3's (7,739 MiB with no owner). **1,558 threads is 779 pools of two**: `renderer::load_placement_textures` (`systems/renderer/src/scene.cpp`) made a `jobs::JobSystem` of one performance and one efficiency worker for each tile mesh's texture pass and joined it at the pass's end, 780 times in the run. Each worker names itself to the profiler (`ENGINE_PROFILE_THREAD`), Tracy keeps the name in its own allocator, rpmalloc, and rpmalloc's first allocation on a thread maps that thread 64 spans of 64 KiB at once, committed; the name stays in the first span for the life of the process, so the 4 MiB mapping is never returned when the thread ends (Tracy's on-demand mode changes nothing: names are kept for a profiler that connects later). [profiling](../subsystems/profiling.md) has the rule that follows: a pool made and joined per call costs 4 MiB a worker each time. The ~0.7 to 1 GB of no-owner commit that remains is the same mechanism for the run's long-lived pools (the load's, the pages', the ground's, the flora poser's, the chain's freer, the pipeline batches': one to four dozen threads each) plus the driver's own pools, and does not grow.

**The cut**: the tile mesh's texture pass runs on the thread that built the mesh (`prepare_scene_textures` with no pool looks up and probes on the calling thread, and makes a pool of two only when a `.tex` must be built, which a warm cache never does). Measured below.

## The first cut: the merge reserves its arrays

`merge_cluster_meshes` now sums the parts' vertices, triangles and clusters and reserves every stream once before the first append (`domain/geometry/src/cluster_lod.cpp`). Measured as a pair in one lease (P2: A then B, the warm load, both with the report; the second A and B were not run, free commit under the floor):

| | A (no cut) | B (the cut) | B − A |
|---|---|---|---|
| process commit at exit | 32,751 MiB | 31,892 MiB | **−859 MiB** |
| peak commit (the OS's) | 33,085 MiB | 32,230 MiB | −855 MiB |
| the merge's blocks, committed / resident | 4,621 / 3,555 | 3,761 / 3,584 | −860 / +29 |
| peak private (sampler, 5 s) | 31.98 GB | 31.40 GB | −0.58 GB |
| whole run | 223 s | 217 s | −6 s |

The merged arrays are the bytes they hold and nothing is copied twice; the picture and every byte uploaded are unchanged (the same appends into larger first blocks).

**The frame's time, the report off** (P3, one lease, alternating, main's binary A against the branch's B with the cut and the report compiled in and not asked for; the staging's south-lake view held for 120 frames at the owner's settings, `--warmup 0`; other agents at 16 % of the CPU, not quiet):

| | 1A main | 2B branch | 3A main | 4B branch |
|---|---|---|---|---|
| GPU total, median ms | 16.967 | 16.960 | 16.963 | 16.953 |
| resolve, median ms | 10.704 | 10.699 | 10.688 | 10.703 |
| frame, median ms | 17.081 | 17.092 | 17.070 | 17.074 |
| peak private (sampler) | 41.64 GB | 40.50 GB | 40.26 GB | 40.65 GB |

Unchanged within 0.01 ms (4B's p95 of 44.5 ms is one hitch beside another agent's work; its median is the others'). The sampler's peak moves by a gigabyte between two runs of one binary, which is why the cut is judged by the report's exit commit above and not by it. **A held view grows the same ten gigabytes the hop flight did** (32 GB after six frames, 40 to 42 GB after 120 at one place). It looked like frames; it is the world's tiles arriving, which the path's jump from the staging's camera makes happen twice ("Who made the unnamed commit", above).

## The second cuts

Leg 2, 2026-11-14. Two cuts, each landing in an owner of its own, so one pair of runs measures both: **the tile mesh's texture pass makes no pool** (above) and **engine-view releases the uploaded host streams** (`renderer::release_uploaded_streams`: the merged scene's 16-bit positions, attributes and canonical vertex ids, given back once every `GpuScene::create` of the run is done; [renderer](../subsystems/renderer.md), "The scene's host copy after the upload", has what stays and why). Q2, one lease: A is leg 1's binary (r4: the report and the merge's reservation), B is the branch at 246a2e07; the staging's south-lake path held 120 frames at the owner's settings, the report on for the commit pair and off for the frame pair; other agents at 24 to 91 % of the CPU, free commit 54 to 57 GB at each start.

| exit, MiB | A (r4) | B (the cuts) | B − A |
|---|---|---|---|
| **process commit** | **41,404** | **33,369** | **−8,035** |
| peak commit (the OS's) | 41,660 | 33,776 | −7,885 |
| private, no named owner | 7,743 | 1,414 | **−6,329** (the pools) |
| the engine heap, live | 15,909 | 14,212 | **−1,697** (the streams) |
| the C runtime's heaps | 16,930 | 15,239 | −1,691 |
| device-local blocks (and the driver's backing) | 16,243 | 16,243 | 0 |
| working set | 16,395 | 14,697 | −1,698 |
| peak private (sampler, 5 s) | 40.43 GB | 32.83 GB | −7.6 GB |

The streams B released, from its log: 16-bit positions 538 MiB, attributes 717 MiB, vertex ids 442 MiB (1,780 MB in all). The no-owner commit that remains, 1.4 GB, is the long-lived pools' 4 MiB a thread and the driver's own pools, and no longer grows with the tiles.

**The frame's time** (the report off, alternating; medians):

| | 1A | 2B | 3A | 4B |
|---|---|---|---|---|
| GPU total, ms | 16.968 | 16.959 | 16.976 | 16.658 |
| resolve, ms | 10.714 | 10.709 | 10.728 | 10.392 |
| frame, ms | 17.090 | 17.100 | 17.104 | 16.834 |
| others' CPU at the end | 57 % | 51 % | 24 % | 49 % |

Unchanged: neither cut is on the frame's path, and the 0.3 ms 4B shows is the run-to-run mode this machine has tonight — the commit pair's A ran at 16.648 total and 10.396 resolve, B at 17.004 and 10.721.

## The traced chain's storage

The third cut on the list, the traced chain's cluster storage (2,848 MiB on the GPU and as much of commit; `gpu_scene_rt_step.cpp`, `make_rt_step` → `gfx::create_cluster_storage`), is **not made**. It is reserved at the driver's most per cluster because the frame's set builds with implicit destinations, and R143 measured that naming smaller totals does not change what the driver reserves ([rt-small-instances](rt-small-instances-2026-11-10.md#what-a-structure-of-the-frames-own-set-reserves)). Two shapes would size it by what is built, both a change to the records-to-TLAS chain on the frame's path: a second set with a smaller per-cluster most for the clusters of meshes that never pass it (a forest's leaf cards: 64 vertices, about 30 triangles), or explicit destinations packed from sizes the device computes in the frame. `cluster set: what a set reserves follows its per-cluster most` (`domain/gfx/tests/clas_records_tests.cpp`) asks the driver what a cluster reserves at each most, so the design row starts from the driver's numbers (RTX 5090, driver of 2026-11, 65,536 clusters a set):

| triangles a cluster (most) | 124 | 96 | 64 | 48 | 32 | 16 |
|---|---|---|---|---|---|---|
| bytes a cluster, plain | 6,144 | 4,736 | 3,328 | 2,560 | 1,792 | 1,152 |
| bytes a cluster, with micromaps (the owner's setting) | 8,832 | 6,912 | 4,736 | 3,712 | 2,560 | 1,536 |

**The reservation is linear in the triangles a cluster may hold and does not move with its vertices** (64 or 32 reserve the same at every row): about 44 bytes a triangle plain and 68 with micromaps, over a floor. A forest's leaf-card clusters hold 28 to 41 triangles (R143's counts), so a second set at 48 triangles reserves 3,712 bytes a cluster where the one set reserves 8,832: if four clusters of five a frame builds are cards, the chain's 2,848 MiB becomes about 1,500, on the GPU and in the commit alike. That is the design row's to build — the records pass routes each record to its class's set and chunk, both sets feed the one bottom-level build, and `renderer::RtCapacity`'s rules hold per set — with the frame pair at 3840 × 2160, traced, the bounce on, as its merge fact.

## What this means for holding memory by distance and speed

- **What grew with travel was a leak, now gone**: each textured tile mesh that arrived cost 8 MiB of commit for the rest of the process (two pool threads' profiler heaps), so a walk's commit rose with every tile it brought in, 6.2 GB on the staging's double arrival alone; after the cut, a tile that leaves gives back what it brought.
- **Released behind a walker today**: anything that lives in a buffer sized by the frame or the ring — the traced chain's steps (released after `renderer.rt.shrink_seconds`, 120 s), the pair tables, the flora's tail — and with it, on Windows, its backing commit. A policy that moves fast has to count the GPU's bytes as the process's too.
- **Not releasable until something is restructured**: the scene's geometry, on the GPU (4.5 GB, one global set of buffers built at load) and on the CPU (4.5 GB, the one merged mesh), is held whole for the session wherever the walker is; releasing it behind a walker needs the static scene split into pageable pieces (the cluster pages `--stream` already has for meshes). Of the CPU copy, 1.7 GB is now given back after the upload; the 2.1 GB left (float positions, triangles, vertex sources, descriptors) is read by the crown priors, the tail trees' padding and the collision, and goes when those take their own (renderer.md, "The scene's host copy after the upload"). The opacity micromaps and the structure cache are the same: built once for the whole scene.
- **The ceiling is the commit limit, not the memory**: 64 GB of memory and a 118 GB limit means about 54 GB of commit can be promised beyond what is touched; the driver's backing spends that at the GPU's rate. A policy row should take its budget from free commit, not free memory.

## Not measured

- The owner's own walk (v43, the game module, the animals, a window): run it with `--memory-report` and read the two `view.memory` lines, or the summary line's `memory` block, which a window's line now carries too.
- Linux: the walk and the report are tested in the container; no forest number. rpmalloc maps the same way there, so the pools' cost was the same on Linux; Linux charges no commit for the GPU's bytes.
