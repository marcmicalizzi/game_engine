# A tile set read as archives with deep queues (2026-10-24)

Roadmap W76 and W54. The startup trace ([W69](startup-trace-forest-2026-10-24.md)) found 46 of the forest's 72 OS-cold seconds were neither CPU nor a lock: 7,428 small tile files read one at a time, at queue depth one, to stand the region's instances on the ground (`scene expand`) and to build the terrain's first fields (`ground start`). This page measures the format that replaced them — one archive a level with a table of contents ([tileset](../subsystems/tileset.md#the-archive)) — and the reads that replaced one at a time: a batch of range reads with up to `depth` in flight ([reading a level](../subsystems/tileset.md#reading-a-level), [io](../subsystems/io.md#queued-reads-a-deep-queue-from-one-thread-roadmap-w54)).

**The result**: the forest's cold launch to nearby-stable went from **72.8–79.5 s to 13.9 s** (warm 17.2 s to 12.6 s); `scene expand` from 40.4–40.9 s to 1.2–1.5 s cold, `ground start` from 14.0–16.0 s to 0.26–0.30 s, the frame thread's first `tile rebuild` from 8.8–8.9 s to 2.4–3.1 s, and the process's time "neither on a CPU nor reading" before ready from 50–52 s to 0.9–2.6 s. Read whole as one batch, the finest level's 132 MB arrives in 0.14 s cold (965 MB/s) where its 7,345 files took 82 s (1.6 MB/s).

## What was run

- **Engine**: this branch, `msvc-release`. `engine-content terrain-read` ([apps](../subsystems/apps.md#engine-content-terrain-synth-and-terrain-tiles-an-authored-terrain)) for the reads alone; `engine-view --offscreen --startup-trace` with W69's arguments and replay (the POC's v7 lab scene, 12,330 instances, the lake spawn, 1600×900, `--raster hw --shadows csm`, `--terrain-tiles --terrain-far 6 --time-rate 0`) for the load. The "before" is W69's own `msvc-release` build at fa343854 (a copy of its executable), run back to back with this branch on the same copies, so the two differ only in the tile set's format and reader.
- **The data**: the forest's v7 set, rebuilt from the POC's authored heightfield (4,097 × 7,169 points at 50 cm) by this branch's `terrain-tiles` into archives — **every level's hash is the POC's v7 manifest's**, `64ab7e6c796d426e` for the finest and the six far levels' likewise, so every one of the 7,427 tiles is the same bytes, now in seven files (133.8 MB). The old format is a copy of the POC's own set (7,428 files). The mask level is the POC's v9 scene's (`world/v9/base.json`, 7,345 tiles at 1 m, 55.3 MB of records), built by this branch's `terrain-masks` into one archive, against a copy of the POC's v7 mask files. Everything read-only from the POC and copied into scratch; nothing was written into the POC or its cache.
- **Cold without administrator rights**: each cold run reads a fresh copy made with `robocopy /J` (unbuffered, so its pages are not in the OS's standby list), as W69 did; the warm run reads the same files again. The `unbuffered` rows read the archive with `FILE_FLAG_NO_BUFFERING`, which never touches the cache, as a second cold method that needs no copy. A fresh copy is also a file Defender has not seen, which a file a tile pays for 7,345 times and an archive once; that is the case a player meets after a build or an update.
- **Volumes**: the `terrain-read` rows are on `D:` (a striped volume over three Samsung 990 Pro 4 TB, where the POC lives); the `engine-view` rows on `C:` (one Samsung 990 Pro 2 TB, where W69's copies were, so its numbers compare).
- **Machine**: i9-10980XE (18 cores, 36 threads), RTX 5090, Windows 11 26200, shared with other agents' builds and test runs. The `terrain-read` table is the first full pass, others' CPU 10–30% at each run's ends except where marked (a second pass under 50–100% others ran up to 8× slower in places and is not quoted); the `engine-view` runs had others' CPU 25–60% at their ends (each row says). Every number is an upper bound.

## The load (engine-view, the lake spawn, `C:`)

| run | `scene expand` ms | `tile rebuild` ms | `ground start` ms | scene_loaded | first_frame | **nearby_stable** | neither ms | others' CPU start/end |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| files, cold (a) | 40,393 | 8,900 | 16,016 | 50,545 | 78,894 | **79,471** | 51,841 | 53% / 58% |
| files, cold (b) | 40,860 | 8,788 | 14,012 | 46,620 | 72,420 | **72,807** | 50,366 | 40% / 53% |
| files, warm | 1,695 | 3,699 | 1,386 | 8,879 | 16,858 | **17,198** | 1,129 | 42% / 35% |
| archives, cold (a) | 1,484 | 3,110 | 302 | 9,936 | 27,699 | **28,094** | 2,573 | 60% / 28% |
| archives, cold (b) | 1,171 | 2,373 | 256 | 7,416 | 13,526 | **13,914** | 941 | 25% / 41% |
| archives, warm | 543 | 2,414 | 199 | 6,796 | 12,280 | **12,622** | 942 | 34% / 32% |

`scene expand` is the 12,330 instances stood on the ground one `height_mm` at a time ([the reader's three shapes](../subsystems/tileset.md#reading-a-level)): after the change it still asks point by point, so each missed tile is one positional read of the open archive at depth one — 1.2–1.5 s cold, of which 0.6–0.8 s is its CPU; the 39 s that were opens and reads of small files are gone, and warm it is a third of what it was (no open and close a tile). `tile rebuild` and `ground start` are windows, now batches. Archives cold (a) spent 11.6 s between `gpu_scene` and its first frame that (b) did not (the driver's pipeline cache after the old binary's run, by the stage it fell in), so (b) is the one to quote; it is within 1.3 s of warm. Both formats read 4.4–4.5 GB before ready, nearly all of it the textures W69 named; the tile set's share is the 171 MiB `scene expand` reads (the finest level and its rereads under the 64 MiB cache) and the windows' 150 MiB.

## The reads alone (`terrain-read`, the finest level, `D:`)

The finest level: 7,345 tiles of 17,988 bytes, 132.1 MB.

| shape | depth | runs | cold s | cold MB/s | warm s | warm MB/s |
|---|---:|---:|---:|---:|---:|---:|
| **files**, one at a time (the old format) | 1 | 7,345 | **82.07** | 1.6 | 0.94 | 141 |
| archive, **points** (a point a tile, scattered: the load's standing) | 1 | 7,345 | **1.53** | 86 | 0.19 | 683 |
| archive, **level** in 256 KiB runs | 1 | 525 | 0.29 | 461 | 0.13 | 1,055 |
| | 8 | 525 | 0.17 | 790 | 0.13 | 1,043 |
| | **32** | 525 | **0.14** | **965** | 0.13 | 984 |
| | 128 | 525 | 0.15 | 866 | 0.16 | 831 |
| archive, level, a record a read | 1 | 7,345 | 0.90 | 146 | 0.15 | 861 |
| | 32 | 7,345 | 0.33 | 400 | 0.18 | 723 |
| | 128 | 7,345 | 0.23 | 577 | 0.15 | 865 |
| archive, **windows** of 8 × 8 tiles, scattered, each a batch | 32 (8 used) | 1,017 | 0.29 | 457 | 0.24 | 540 |
| | 1 | 1,017 | 0.34 | 384 | 0.15 | 891 |
| unbuffered level, 256 KiB runs | 1 / 8 / 32 / 128 | 525 | 0.24 / 0.18 / **0.12** / 0.14 | 558 / 765 / 1,103 / 956 | — | — |
| unbuffered level, 1 MiB runs / 4 MiB runs | 32 / 8 | 127 / 32 | 0.13 / 0.14 | 991 / 975 | — | — |
| unbuffered points | 1 | 7,345 | 1.08 | 150 | — | — |

**The mask level** (7,345 tiles, 55.3 MB of records): the files one at a time **99.2 s** cold (0.56 MB/s; 1.16 s warm, with others' CPU at 57–62%), the archive read the renderer's way — every tile in order, twice — **0.064 s** cold in 2 MiB windows (others at 43% and 82% at its ends: an upper bound) and 0.036 s warm.

### What the depths say

- **The cost was the files, not the reads.** A point's miss went from 11.2 ms (an open, a read and a close of a fresh 18 KB file) to 0.2 ms (a positional read of an open archive), cold. The archive at depth one, point by point, is 54× the files; nothing else here matters as much, and it is what the load uses today.
- **Depth pays until the reader's own work is the cost.** Read whole in 256 KiB runs, depth 8 takes the level from 0.29 s to 0.17 s and 32 to 0.14 s; 128 is no faster. Past 32 the reader is CPU-bound — 7,345 records allocated, validated, copied out of the staging ring and published under the lock, about 17 µs a tile, which is what the warm rows are at every depth. Read a record at a time, depth keeps paying to 128 (0.90 to 0.23 s), because 7,345 small reads are the device's latency, not its bandwidth. **`tileset.read_depth` is 32** (the level's best; a window's batch rarely has more than eight runs to issue), and **`tileset.read_run_kib` 256** (1 and 4 MiB runs are no faster, and 256 KiB keeps the staging ring of a deep batch at 8 MiB).
- **Windows**: an 8 × 8 window is eight runs, one a row, so its depth stops at eight; scattered over the level they cost twice the level read whole (0.29 against 0.14 s), the price of 1,017 runs instead of 525 and of a batch's set-up a window. That is the shape the terrain's rebuilds have.

### W54: what the pool could not reach, and what the queue gained

`read_ranges_async` reads each range on an Efficiency worker that blocks in its read, so the device sees as many reads at once as the pool has workers. Raw — every record of the level its own 18 KB read, into one buffer, nothing done between them (`terrain-read --pattern pool` and `--pattern queue`, others' CPU 14–30%):

| | in flight | cold s | cold MB/s | warm s |
|---|---:|---:|---:|---:|
| the pool, 4 workers | 4 | 0.262 | 504 | 0.038 |
| the pool, 16 workers | 16 | 0.205 | 645 | 0.031 |
| `read_queued`, one thread | 4 | 0.261 | 506 | 0.075 |
| | 16 | 0.249 | 530 | 0.077 |
| | 32 | 0.205 | 646 | 0.079 |
| | **128** | **0.140** | **941** | 0.076 |

At the same depth the two are the same (the device does not care who issued the read); what the queue buys is the depth without the threads — 128 reads in flight from the one thread that wanted them, 1.9× the four workers a pool might spare, 1.5× sixteen. Warm, the pool is faster (the cache's copies run on sixteen cores; the queue's run on one), which is why the archive reader coalesces adjacent records into runs, so a warm batch is a few hundred copies and not thousands. **The completion path is events, not a port**: an `OVERLAPPED` with an event of its own a read, reaped in order, because a completion port is bound to its handle for life and two threads queueing reads of one handle would take each other's completions; at these rates (50,000 reads a second at depth 128) the event per read is not where the time goes, so nothing here says a port would buy more. **io_uring was not built**: nothing here measured a Linux disk, and the Linux containers this tree tests in run under Docker's default seccomp profile, which refuses `io_uring_setup`, so the path could not be tested where it would ship; POSIX keeps `pread` in order behind the same request (W54 stays open for it).

### Unbuffered reads in production: no

Unbuffered, the level read whole is 0.12 s at depth 32 against 0.14 s through the cache from a cold copy — 12% — because the cache's copy is a small part of a read the reader's own work dominates; 1 and 4 MiB runs gain nothing more. Against that, an unbuffered read is never in the cache for the next launch, and the warm launch (12.6 s here, against 13.9 s cold) is the common one. `tileset.unbuffered` stays off; it is a measurement's switch. A reader whose work per byte is far smaller than this one's (a page streamed straight to the GPU) would be the case to measure again.

## The archive against the files, besides the reads

- **The build is faster**: the forest's set, 7,427 tiles, built in 1.53 s (`msvc-release`) into seven files; W69's uncooked run spent 10.3 s in `scene file` building the same set as 7,428 files.
- **A reader holds the archive open**: on Windows that keeps a `--force` rebuild into the same directory from replacing it while a viewer has the set open (a rename over an open file is refused); builds into the cache never meet it, since another build is another key ([tileset](../subsystems/tileset.md#the-archive)).
- **The cache budget still decides the rereads**: `scene expand` reads 171 MiB for a 132 MB level because the default 64 MiB budget holds half the finest level and the instances are scattered over all of it. Moving the standing to tile activation (the load's work, W69's list) removes that too; until then `tileset.cache_mib=160` would hold the forest's finest level whole.

## What was seen and not fixed

- `scene expand` still stands every instance in the region before the first frame, point by point; the batches are there for it (`TileSet::prefetch_level`, or the standing moved to tile activation), and `systems/renderer/src/scene.cpp` is the load agent's tonight.
- The renderer's mask load reads every tile twice, once only to count them (`terrain_materials.cpp`); with the archive's windows that costs 0.03 s, but the count is the table's size and needs no read.
- `ground prepare`'s `tile rebuild` (2.4 s warm) is still on the frame thread and is now CPU, not IO.
