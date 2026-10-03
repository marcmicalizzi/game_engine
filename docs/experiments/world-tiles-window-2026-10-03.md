# The world's tiles keeping up with a window (2026-10-03)

- **Question.** Does the ground drawn from the world's tiles ([renderer](../subsystems/renderer.md#what-a-frame-waits-for), [ADR-0050](../adr/0050-the-ground-is-drawn-from-the-worlds-tiles.md), proposed) keep up with a camera in a window at 100 m/s and at 30 m/s — how far behind the camera does the finest ring fall, in metres and frames, and does any frame wait or hitch for the tiles — and what were the unexplained frames of [the offscreen flight](world-tiles-flight-2026-09-30.md)?
- **Date:** 2026-10-03. **Machine:** the owner's desktop, i9-10980XE (36 logical CPUs), RTX 5090, `msvc-release`. **Build:** this branch: rebuilds of changes, the window's tile budget and worker bound, the start's first layout.
- **Machine state:** shared; other agents built and tested beside every run. Windowed 1080p: other processes 11–23% of the CPU, the GPU 5–7% busy at the start and end samples. Offscreen: 9–24% of the CPU and the GPU 6% busy at 1080p (99% at the 30 m/s run's end sample), 15–25% at the surround size's 100 m/s run and 20–100% at its 30 m/s one; the "before" run began with 77% of the CPU busy. Every number is an upper bound; the ratios within a run stand.
- **Decision:** the tile rebuild costs what changed, a window's tile staging has its own budget (`renderer.terrain.tile_upload_mib`, 4) and a window's terrain pool half the machine's threads (`renderer.terrain.workers`); an interactive session's first layout is built round where it starts.

## Setup

- **The scene**: [`content/test-scenes/desert-endless`](../../content/test-scenes/desert-endless/README.md), still sand, the card's default shadows (traced), its sky.
- **100 m/s**: the scene's own camera path — offscreen all 12 km (7,201 frames, `--repeat 1 --warmup 30`); in the window its first minute (6 km east, across the old erg's edge at x = 3,072).
- **30 m/s**: the same track's first 2 km with its times stretched by 10/3, 66.7 s, flown the same two ways.
- **In a window** (`engine-view --windowed --camera-path ... --benchmark`, new here: [apps](../subsystems/apps.md#pacing)): a 1920×1080 window flies the path at its own speed in real time — the frame it draws is the path's frame at the wall time since the first, so a slow frame skips path frames instead of slowing the camera — under the present ceiling and the display pacer (the display refreshes every 12.2 ms), and writes a record per presented frame.
- **The lag**: every frame, the camera against where it was when the layout drawn was asked for (`terrain.layout_lag_m`, `layout_lag_frames`); offscreen it is zero, since every frame waits for its rebuild.
- Each run under the GPU lock alone, `--wait-quiet 60`; the process's private bytes and working set sampled each second.

## In a window

| | 100 m/s, 1080p | 30 m/s, 1080p |
|---|---|---|
| the finest ring's lag, metres median / p99 / max | 21.7 / 40.0 / 50.0 | 2.5 / 5.5 / 8.0 |
| the same in frames | 11 / 19 / 24 | 4 / 9 / 13 |
| a rebuild's worker wall, ms median / p99 / max | 67.6 / 91.0 / 100.1 | 23.7 / 63.5 / 72.2 |
| tiles built / let go a second; rebuilds swapped | 1,674 / 1,646; 362 in 60 s | 573 / 548; 889 in 66.7 s |
| the frame thread's tile work (`host_ms`), median / p99 / max | 0.02 / 4.02 / 7.70 | 0.03 / 2.05 / 3.30 |
| the engine's CPU a frame (`cpu_ms`), median / p99 / max | 5.56 / 13.9 / 62.0 (frame 0) | 4.42 / 13.0 / 56.8 (frame 0) |
| GPU ms a frame, median / p99 / max | 1.48 / 3.74 / 4.26 | 1.32 / 2.37 / 7.54 |
| frame time, median / p99 / max | 19.2 / 27.6 / 93.9 (frame 1) | 19.2 / 28.0 / 90.5 (frame 1) |
| frames over 50 ms after the first two | 0 | 0 |
| process private bytes / working set at the end | 2.67 GB / 503 MB | 1.18 GB / 450 MB |

**No frame waits for a tile, and none hitches.** The frame thread's share of the ground is the staging of a rebuild's tiles and the swap's slot records: 0.02 ms at the median, 4 ms at the 99th percentile, under 8 ms at worst. The frame time is the display's — the pacer waits a refresh for the last present (`pace_ms` 13.5 ms at the median) — and after the first two frames none took 50 ms.

**How far behind.** At 100 m/s a rebuild takes 68 ms of worker wall at the median for about 4.6 frames of travel, and the next is asked as soon as one is swapped, so the drawn layout is 11 frames and 22 m behind the camera at the median, 50 m at the most. The finest ring is 1.5 tiles (48 m) round the camera, so at 100 m/s the camera flies over the 1 m level's tiles — already resident, drawn by the layout before — for about half the time, and the 50 cm tiles arrive behind it; at 30 m/s the lag is 2.5 m at the median and 8 m at the most, inside the finest ring. No frame drew a hole: the layout drawn is whole until the next is swapped in whole.

**What the first windowed run found** (the same build, with the pool at a worker a logical CPU): the frames hitched to 50–65 ms every 124 frames — every 2.2 s, at each move of the 2 m level's field window — and five renderer frames took 25–36 ms, every one in the graph's recording (the slow-frame line, below), with nothing in the graph to record: the frame thread had lost its time slices to the 35 workers a window's move and a rebuild fill. With the pool at half the logical CPUs (18), no frame after the first two took 50 ms and no renderer frame 25 ms; the lag went from 10 m to 22 m at the median, 37 to 50 at the most, because the rebuilds have half the threads.

## Offscreen

| | 100 m/s, 1080p, before | 100 m/s, 1080p | 30 m/s, 1080p |
|---|---|---|---|
| frame wall ms (the frame waits for its tiles), median / p99 / max | 38.0 / 84.1 / 425 | 18.7 / 56.4 / 140 | 18.4 / 53.4 / 80.7 |
| the frame thread's tile work, median / p99 / max | 32.8 / 78.6 / 201 | 13.5 / 52.1 / 137 | 5.3 / 49.8 / 92.7 |
| a rebuild's worker wall, ms median / p99 / max | 24.8 / 61.0 / 75.4 | 12.5 / 43.7 / 63.0 | 5.7 / 46.6 / 68.0 |
| renderer CPU ms, median / p99 / max | 2.83 / 5.37 / 8.14 | 2.84 / 5.69 / 8.58 | 2.82 / 8.25 / 18.0 |
| GPU ms, median / p99 / max | 1.52 / 2.42 / 4.30 | 1.59 / 2.50 / 5.70 | 1.24 / 1.90 / 2.84 |
| tiles built / let go a second | 1,964 / 1,951 | 1,964 / 1,951 | 576 / 552 |
| chunks resident at the end; device bytes | 14,475; 612.8 MB | 14,475; 612.8 MB | 14,482; 612.8 MB |
| private bytes after the first fill / at the end | 2.95 / 2.27 GB | 2.94 / 2.59 GB | 2.92 / 1.10 GB |

At 11,520×2,160 over three views (`--views surround3`), the same tiles: 100 m/s — frame wall 21.0 / 95.1 / 282 ms, the tile work 14.0 / 86.3 / 272, a rebuild 12.9 / 60.1 / 214, GPU 3.29 / 4.30 / 5.48, renderer CPU 2.98 / 9.39 / **117**; 30 m/s — frame wall 20.6 / 83.1 / 329, the tile work 4.35 / 60.0 / 200, a rebuild 3.87 / 51.5 / 79.4, GPU 2.82 / 3.61 / 4.37, renderer CPU 4.13 / 23.9 / **295**; 612.8 MB of slots and arenas, private bytes 3.48 GB at most, the working set 350–470 MB. Other processes took 25% and 15% of the CPU at the 100 m/s run's ends and 20% and **100%** at the 30 m/s run's.

"Before" is this branch before the rebuilds of changes (the whole-set hand-over and key scan), with 77% of the CPU busy with other work at its start. Offscreen every frame waits for its rebuild, so the frame wall is the host's whole cost of the frame's tiles: halved by the rebuilds of changes, which build exactly the same tiles (235,731 built and 234,100 let go in both, 14,475 held at the end) — the cost they took away was the bookkeeping, not the building. The worker's own phases at 100 m/s before the change: 7.5 ms deciding what to build (every held tile's key), 15.6 ms building 32 tiles on the pool (28 ms of CPU on the source's heights and 130 on the meshes and DAGs, about ten-way parallel), 3.8 ms putting the lists back.

## The unexplained frames of 2026-09-30

- **The renderer's long CPU frames come back at the surround size and not at 1080p**, and now say where. Three offscreen 1080p flights (18,400 frames, the 12 km path twice and the 30 m/s track) never took the renderer over 18 ms; at 11,520×2,160 the 100 m/s flight had 20 frames over 25 ms, all past frame 3,493 — the stretch past the old erg's edge where the 2026-09-30 run had its 230–457 ms — and the 30 m/s one 37, past frame 1,666, at most 295 ms. The slow-frame lines (new: `SceneRenderer::submit_frame` logs a frame whose recording took over 25 ms, by part) name **the graph's recording** for all but one — 26–97 ms in `graph.compile` and `execute`, a pass list that takes 3–6 ms in every other frame — and **the terrain's copies and slot records** (`GpuScene::terrain_prepare`, 111 ms) for frame 3,493. Neither is work that grew: `terrain_prepare` writes 96 bytes a level and stages a few hundred slot records, and the graph is the same graph. What the long frames share is the process's memory: 3.1–3.5 GB of private bytes with a working set of 350–470 MB, at the surround size half a gigabyte more than at 1080p, on a machine whose other work took up to all of its CPU. A frame that touches pages the system took back pays for them where it touches them, and `terrain_prepare` makes a host-visible staging buffer a frame for the slot records, which can be the allocator's next block. Not removed here; what is next is to count the frame's page faults beside its parts, and to stage the slot records and the tiles through one persistent ring rather than a buffer a chunk (below). In a window the same pages cost the same, but the windowed runs above, at 1080p, had none.
- **The frames a window's tile work starved** are gone: with a worker on every logical CPU, the five renderer frames over 25 ms of the first windowed run were all in the graph's recording, the frame thread descheduled while every core ran tile jobs. The 2026-09-30 surround run's 1.4 s rebuild — one in 7,198, where the rest took under 160 ms — fits the same pressure on the same stretch.
- **The largest offscreen frames are a level's window moving.** Offscreen the 1080p flight's longest frames — 209 ms at frame 5,882, the frame the 2026-09-30 run's 302 ms was — are the frames whose host work is the pairs step carrying a moved window's fields over (`rings.max_pairs_ms` 171 ms before, 109 in the window): the field worker evaluates the strip the 4 m and 2 m levels' windows moved into. A window does it on the field worker behind the frames; offscreen waits for it by design. Frame 5,833's 425 ms in the "before" run had 37 ms of tile work and 2.9 ms of renderer CPU in it and is the machine's (the run began with 77% of the CPU busy).

## What surprised me

- The owner's first flight (2026-10-02) did not stream at the start because the first layout was built round the orbit camera — the old erg's centre, 2 km from his `--start` — so the world's first update asked for a whole ring, 12,844 tiles and eight seconds of rebuild behind the frames, while he flew at 100 m/s away from it; walking let the rebuilds catch up. A replayed session started there now rebuilds nothing.
- Half the threads cost the lag less than they gave the frames: 10 → 22 m at the median at 100 m/s, and no hitch. The rebuild was about ten-way parallel on 35 workers anyway.

## What is left

- **The surround size's long renderer frames**: count the frame's page faults beside its parts (the slow-frame line), and stage what the frame thread uploads — the tiles' chunks, a buffer each today, and the slot records, a buffer a frame — through one persistent host ring sized at load, so no frame makes or frees a host-visible buffer.
- **The time-lapse's window drop** (the 2026-09-30 page's fourth item: a level whose window moves drops its fields after b, and the sand dips there): not touched here; the fix written there stands.
- **More threads for the rebuild, not for its bookkeeping**: at 100 m/s a rebuild of about 70 tiles takes 68 ms of worker wall on 18 threads, most of it the meshes' cluster DAGs (130 ms of CPU for 32 tiles), about ten-way parallel. The lag at 100 m/s is that wall; a cheaper DAG for the outer rings' 8- and 16-cell tiles is where it would come down.

## Caveats

One machine, a shared one; still sand (a time-lapse adds the fields' evaluation to the same pool); the window at 1920×1080 only — a window of the owner's 11,520×2,160 covers his whole desk for the minute it runs, and the coordinator asked that it not be run there; the surround size is measured offscreen, where its tiles are the same work.
