# Streamed-tree frame-thread allocations (W73, 2026-10-26)

The old frame-loop gate accepted equal rigid and swaying second-flight allocation counts: 308
in each, zero device buffers, as recorded by
[R100](rt-pair-accounting-2026-10-26.md#validation). Tile arrivals, departures and ring changes
are steady state. Both flights must instead allocate zero.

The own before build is main at `5552a2b3` with bounded debug allocation-site capture added; the
after build adds retained placement scratch and per-tile instance/tag storage. Both are this
worktree's `msvc-debug` binaries, offscreen on the i9-10980XE / RTX 5090, with matching paths and
settings and `--wait-quiet 60` under the machine-wide GPU lock. Debug stack attribution is active
in both builds: allocating frames include its overhead, so timings are not isolated allocator
costs. The frame counter itself is unchanged.

The small scene and 96-frame path are exactly the streamed-trees test's oak/spruce grove, two
rings of 32 m tiles, 160x96, 12 warm-up frames and two repeats. The frozen expanded forest is
the POC's `reports/quality-v15/engine-c1ee971d/runs/expanded-forest-rt-wide-20261026-190424/scene.json`;
its `Reproduce.cmd` and `run.json` were read, never run. Its v14 DDC was copied to scratch
(579 files, 2,959,684,230 bytes). Its hold camera is translated 160 m on x over eight seconds,
preserving the target offset and crossing two 64 m boundaries. Those eight-second camera keys are
resampled to 180 frames, flown twice, with
12 warm-ups, hardware raster, traced shadows, 1600x900, six far levels, fixed sand/sky, world
rate 1, 64 MiB deformation pool and 1024 MiB RT budget, flora motion on at wind strength 0.7.
No POC cache, script or binary is written or executed.

The [renderer](../subsystems/renderer.md#the-frame-loop-allocates-nothing),
[world](../subsystems/world.md#the-consumers) and
[memory](../subsystems/memory.md) pages describe the retained storage and debug capture.
Scratch holds the executable/PDB before pair, reports, records, raw stack diagnostics and
symbolization, including the per-frame site counts. Results and machine states are below.

## Allocation attribution

Counts below are the **before second flight**, by source call site. The two tile-vector call
expressions are `systems/world/src/placement_tiles.cpp:230` (instances) and `:231` (tags) at
`5552a2b3`; MSVC's return-address line mapping reports `:231` for both, and their vector types
distinguish them. The authored output reserve is `domain/authored/src/authored.cpp:164`
(return address `:165`). No code in the authored generator needed changing: the consumer now
retains and sizes its output before asking it for a tile.

| Grove site | Allocations | Frames | Count on an allocating frame | Work and fix |
|---|---:|---:|---:|---|
| Authored placement output | 82 | 48 | 1–5 | Tile arrival or ring form change; one output per entry, reserved for its largest tile at load |
| Tile instances | 113 | 48 | 1–8 | Fresh tile block on arrival or rebuild; exact per-tile storage pooled at load, rebuild in place |
| Tile tags | 113 | 48 | 1–8 | Parallel tag vector, same retained tile storage |
| Total | 308 | 48 of 96 | 3–20 | No pose or ray-tracing allocation in this case |

The forest's before second flight has 2,896 allocations: authored output 138, tile instances
1,209 and tags 1,209, sward output 184 (`domain/flora/src/sward_placement.cpp:164`), mask output
21 (`:93`), archive fetch buffers 57 + 57 (`domain/tileset/src/archive.cpp:232–233`), mask reader
scratch 19 (`domain/tileset/src/masks.cpp:882`), and two table-copy vector growths
(`systems/renderer/src/gpu_scene.cpp:1369,1379`). Source lines here are the symbolized return
addresses unless distinguished above. Scratch CSVs carry every repeat/frame/site/count/byte
record, and matching raw stacks and PDBs are retained.

## Upload audit

Tile arrivals upload **zero images and zero texture bytes**: the scene's resident meshes and
distance forms already hold their materials and share each distinct image from load.
`upload_materials` is not on this path. Tile commits still stage instance slots (96 bytes each),
with traced shadows their top-level records (64 bytes each), and the live-instance expansion
list (4 bytes per listed instance); the GPU expands the pair table. Flora records/poses use
existing mapped tables. W47's serial image uploads at scene load are unchanged.

## Exact grove second-flight counts

All omitted frames have zero allocations. The work column distinguishes tile arrival from a
ring representation change; both rebuild through the same consumer. Column sums are 82, 113,
and 113. The same per-frame allocation columns are written to scratch CSV alongside `cpu_ms`
and `frame_ms` for each before/after run.
| Frame | Placement output | Tile instances | Tile tags | Work |
|---:|---:|---:|---:|---|
| 3 | 1 | 1 | 1 | tile arrived |
| 4 | 1 | 1 | 1 | tile arrived |
| 10 | 1 | 1 | 1 | tile arrived |
| 11 | 1 | 1 | 1 | tile arrived |
| 12 | 1 | 1 | 1 | tile arrived |
| 14 | 1 | 1 | 1 | ring form changed |
| 15 | 1 | 1 | 1 | ring form changed |
| 17 | 3 | 7 | 7 | tile arrived |
| 19 | 1 | 1 | 1 | tile arrived |
| 20 | 1 | 1 | 1 | tile arrived |
| 21 | 3 | 3 | 3 | ring form changed; tile arrived |
| 24 | 2 | 4 | 4 | tile arrived |
| 25 | 1 | 1 | 1 | tile arrived |
| 26 | 2 | 2 | 2 | ring form changed; tile arrived |
| 27 | 1 | 1 | 1 | tile arrived |
| 28 | 3 | 7 | 7 | ring form changed; tile arrived |
| 31 | 2 | 2 | 2 | tile arrived |
| 32 | 1 | 2 | 2 | tile arrived |
| 33 | 1 | 2 | 2 | tile arrived |
| 34 | 3 | 3 | 3 | ring form changed; tile arrived |
| 35 | 4 | 6 | 6 | ring form changed |
| 37 | 1 | 1 | 1 | ring form changed |
| 38 | 1 | 1 | 1 | tile arrived |
| 39 | 1 | 2 | 2 | tile arrived |
| 40 | 1 | 2 | 2 | tile arrived |
| 41 | 2 | 4 | 4 | tile arrived |
| 42 | 5 | 5 | 5 | ring form changed |
| 44 | 1 | 1 | 1 | ring form changed |
| 45 | 2 | 2 | 2 | tile arrived |
| 47 | 3 | 4 | 4 | tile arrived |
| 49 | 3 | 7 | 7 | ring form changed |
| 50 | 2 | 2 | 2 | ring form changed |
| 52 | 2 | 2 | 2 | tile arrived |
| 53 | 1 | 3 | 3 | tile arrived |
| 54 | 3 | 3 | 3 | tile arrived |
| 55 | 1 | 1 | 1 | ring form changed |
| 56 | 4 | 8 | 8 | ring form changed |
| 58 | 1 | 1 | 1 | ring form changed |
| 62 | 2 | 2 | 2 | ring form changed; tile arrived |
| 63 | 3 | 3 | 3 | ring form changed |
| 65 | 1 | 3 | 3 | ring form changed |
| 68 | 1 | 1 | 1 | ring form changed |
| 69 | 1 | 1 | 1 | ring form changed |
| 76 | 1 | 1 | 1 | ring form changed |
| 77 | 1 | 1 | 1 | ring form changed |
| 80 | 1 | 1 | 1 | ring form changed |
| 83 | 1 | 1 | 1 | ring form changed |
| 84 | 1 | 1 | 1 | ring form changed |

## Validation and size

Linux Clang debug, filter `memory|renderer|world|engine_view|docs|lint`: **13/13 CTest entries
passed**, 102.08 s testing, 6m 19s including build, after 1,502 s waiting for the container lock.
This checks compilation and CPU paths; the container does not validate desktop GPU rendering.
`tools/linux-build.ps1 -Prune` removed this checkout's two volumes, reclaiming 3.66 GB.

The targeted Windows CPU checks pass: memory 17 cases / 78 assertions (the capture test adds
5 assertions), world 40 / 72,138 (the finite-storage regression adds 1 case / 30 assertions),
and authored scene 3 / 8,634. The latter includes the new finite-count coverage checks.
Format touched 1,214 source files; banned-pattern lint and the license/documentation checks pass.
No hot type, GPU-mirrored layout, schema version, image pin, or capacity budget changes.
The own debug executable grows from 39,272,448 to 39,282,176 bytes (+9,728); its PE `.text`
virtual size grows from 24,121,829 to 24,130,021 bytes (+8,192), both builds carrying attribution.

Before machine_state, start → end (both GPU locks mine, sessions unlocked, total VRAM
32,607 MiB): grove other CPU 99.829% → 100%, GPU utilization 2% → 6%, VRAM 11,662 → 11,672 MiB;
forest other CPU 100% → 42.361%, GPU 0% → 23%, VRAM 16,207 → 17,602 MiB. Both quiet waits
expired. These are shared-machine upper bounds, not isolated timing costs.

## Before and after

The per-frame records below are the second flight. Parentheses give first-flight allocations /
device buffers; every second-flight device-buffer count is zero.

| Scene / build | Allocations (first flight allocs/buffers) | Allocating frames / maximum per frame | `cpu_ms` median / p95 | `frame_ms` median / p95 |
|---|---:|---:|---:|---:|
| Grove before | 308 (598/106) | 48/96 / 20 | 0.769 / 0.976 | 4.620 / 12.118 |
| Grove after | **0** (256/106) | **0/96 / 0** | 0.598 / 0.782 | 0.770 / 1.033 |
| Frozen forest before | 2,896 (3,185/1) | 102/180 / 92 | 15.724 / 17.459 | 69.654 / 238.650 |
| Frozen forest after | **156** (609/1) | 21/180 / 16 | 13.203 / 21.338 | 58.279 / 241.078 |

All 192 grove records and all 360 forest records agree before/after on visible pairs, shadow
casters, built/wanted RT records, instances and pairs. That is a cut/accounting comparison;
renderer flora capture cases separately hold rendered-byte identity. No image pin changed.

After machine_state, start → end (locks mine, owner `codex-night-sol-alloc`, sessions unlocked,
own CPU 0% at both forest samples, grove 0% → 0.174%): grove other CPU 6.424% → 9.896%, GPU
2% → 18%, VRAM 16,193 → 16,197 MiB; forest other CPU 18.056% → 25.874%, GPU 100% → 1%,
VRAM 22,925 → 21,257 MiB of 32,607. The grove passed its quiet wait; the forest's expired.
These states differ substantially from the baseline. The counts establish removal of allocation
churn; the timings are shared-machine upper bounds, not a measured isolated speedup.

**What remains in the larger POC**, outside the grove's 308: 154 allocations in sward-mask reads
and two one-time table-copy capacity growths, not pose computation or acceleration-structure
record resizing. The mask reader and sward implementation are outside this task's assigned files;
no changes were made there. The existing table-update vectors grow at frame 3 of the second
crossing (one each for instance and RT record copies). They and the mask reader remain debt;
this change does not claim a zero-allocation full POC. It does remove all authored placement
output and tile instance/tag allocations there, plus the sward output allocations because entry
output scratch is now kept for every generator.

| Remaining forest site (symbolized return address) | Count | Frames | Count per allocating frame |
|---|---:|---:|---:|
| `domain/flora/src/sward_placement.cpp:93`, local mask words | 21 | 20 | 1–2 |
| `domain/tileset/src/archive.cpp:232`, archive run list | 57 | 18 | 3–6 |
| `domain/tileset/src/archive.cpp:233`, archive read-request list | 57 | 18 | 3–6 |
| `domain/tileset/src/masks.cpp:882`, mask-window fetch list | 19 | 18 | 1–2 |
| `systems/renderer/src/gpu_scene.cpp:1369`, instance copies | 1 | frame 3 | 1 |
| `systems/renderer/src/gpu_scene.cpp:1379`, RT record copies | 1 | frame 3 | 1 |

Windows frame-loop gate (night of 2026-10-26–27): **4 cases / 2,943 assertions passed** in
344.80 s, with all three path scenes available. Every second flight is **0 allocations /
0 device buffers**, including the streamed grove both rigid and swaying. First flights:
overlook 0/0, erg 0/0, endless desert 58/6, rigid grove 178/106, swaying grove 256/106.
The overlook's seven pinned Khronos assets were fetched with `tools/fetch-samples.ps1` at
`90d7ede14c7e280af263824604b427a1ca02cb66` (CC0-1.0), assembled in scratch and moved as complete
directories into this worktree's ignored `content/samples`; no binary asset is committed.

The skipped GPU modules' retry passed **gfx 102 cases / 419,418 assertions** and
**renderer 260 cases / 4,148,417 assertions**, including the flora capture comparisons; the
module's four explicitly marked diagnostic/sample cases retain their normal skip status
(banding and sand-motion instruments, plus the sample-only shadow-caster and cascade sweeps).
No rendered-byte tolerance or image expectation was changed.

The complete precommit Windows affected set is **99/99 validated** across the initial run and
GPU-lock retries. The initial run took 11,615.78 s and passed 96 entries; gfx, renderer and viewer
exited 75 after waiting for the shared GPU. Gfx and renderer then passed; viewer required further
lock-skip retries and finally passed in 1,279.77 s: **140 cases / 46,195 assertions**, with its
four frame-loop cases separately selected by their own gate above. No assertion failure,
raised timeout, weakened expectation or test-suite GPU lock was used. These elapsed times
include contention and are not performance measurements.
