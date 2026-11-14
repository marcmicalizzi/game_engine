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

**Private with no named owner grows in the flight** (1.0 GB after the warm load, 7.7 GB after the hop flight, all untouched, in 4 to 16 MiB regions). r2, the binary that flew F2, did not yet record protections; the next flight with r3 or later says whether it is write-combined (the driver keeping the backing of memory the chain's 30-odd resizes freed) or ordinary read-write. That is the first measurement of leg 2.

## The first cut: the merge reserves its arrays

`merge_cluster_meshes` now sums the parts' vertices, triangles and clusters and reserves every stream once before the first append (`domain/geometry/src/cluster_lod.cpp`). Measured as a pair in one lease (P2: A then B, the warm load, both with the report; the second A and B were not run, free commit under the floor):

| | A (no cut) | B (the cut) | B − A |
|---|---|---|---|
| process commit at exit | 32,751 MiB | 31,892 MiB | **−859 MiB** |
| peak commit (the OS's) | 33,085 MiB | 32,230 MiB | −855 MiB |
| the merge's blocks, committed / resident | 4,621 / 3,555 | 3,761 / 3,584 | −860 / +29 |
| peak private (sampler, 5 s) | 31.98 GB | 31.40 GB | −0.58 GB |
| whole run | 223 s | 217 s | −6 s |

The merged arrays are the bytes they hold and nothing is copied twice; the picture and every byte uploaded are unchanged (the same appends into larger first blocks). It cannot change a frame's time: nothing a frame reads moved. A flight pair was not flown (free commit stayed under the flight's floor from 22:10).

## What this means for holding memory by distance and speed

- **Released behind a walker today**: anything that lives in a buffer sized by the frame or the ring — the traced chain's steps (released after `renderer.rt.shrink_seconds`, 120 s), the pair tables, the flora's tail — and with it, on Windows, its backing commit. A policy that moves fast has to count the GPU's bytes as the process's too.
- **Not releasable until something is restructured**: the scene's geometry, on the GPU (4.5 GB, one global set of buffers built at load) and on the CPU (4.5 GB, the one merged mesh), is held whole for the session wherever the walker is; releasing it behind a walker needs the static scene split into pageable pieces (the cluster pages `--stream` already has for meshes) and the CPU copy dropped after upload or kept per page. The opacity micromaps and the structure cache are the same: built once for the whole scene.
- **The ceiling is the commit limit, not the memory**: 64 GB of memory and a 118 GB limit means about 54 GB of commit can be promised beyond what is touched; the driver's backing spends that at the GPU's rate. A policy row should take its budget from free commit, not free memory.

## Not measured

- The owner's own walk (v43, the game module, the animals, a window): run it with `--memory-report` and read the two `view.memory` lines; the summary line of a window does not carry the block yet.
- A flight pair for the merge cut, and the flight's unnamed 7.7 GB by protection.
- Linux: the walk and the report are tested in the container; no forest number.
