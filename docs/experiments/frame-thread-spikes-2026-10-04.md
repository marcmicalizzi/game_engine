# The frame thread's spikes on the endless flight (2026-10-04)

- **Question.** On the endless desert at 11520×2160 (`surround3`, six far levels) the GPU's frame is about 4 ms and the frame thread's CPU time is 4 ms at the median, but it spikes: the owner's last flight read `cpu_ms` median 4.1, p95 11.8, p99 14.0, max 93.0, and `submit_ms` 3.4 at the median. Where does each spike's time go, and what removes it? The suspects named on 2026-10-03 were the render graph's recording (26–97 ms on some frames) and a staging buffer made every frame in `GpuScene::terrain_prepare` ([world tiles in a window](world-tiles-window-2026-10-03.md), [ground to the horizon](far-ground-2026-10-03.md)).
- **Date:** 2026-10-04. **Machine:** the owner's desktop, i9-10980XE (36 logical CPUs), RTX 5090. **Build:** `msvc-release` of `main` at `70f88d15` with the per-stage timing below added and nothing else changed ("before"), and this branch ("after").
- **Machine state:** shared throughout — other agents building and testing, and for part of the time another process holding most of the GPU's memory. Every run waited up to 120 s for a quiet machine (`--wait-quiet 120`) under the GPU lock and none was quiet at both ends; each run's `machine_state` is quoted with it. Every number is an upper bound; the attribution rests on where the time went, which no run disagreed on.
- **Decision:** no ADR. The fixes are the renderer's and `gfx`'s ([renderer](../subsystems/renderer.md#what-a-frame-waits-for), [gfx](../subsystems/gfx.md)).

## Setup

- **The flight**: [`desert-endless`](../../content/test-scenes/desert-endless/README.md)'s own camera path — 12 km at 100 m/s, 7,201 frames — offscreen at 11520×2160 `--views surround3`, `--repeat 1 --warmup 60`, once with `--shadows rt` and once with `--shadows csm`:

  ```powershell
  tools/gpu-lock.ps1 run -Purpose "flight" -Exec "build/msvc-release/bin/engine-view.exe --scene content/test-scenes/desert-endless/scene.json --camera-path content/test-scenes/desert-endless/camera-path.json --width 11520 --height 2160 --views surround3 --shadows rt --benchmark run.jsonl --repeat 1 --warmup 60 --wait-quiet 120 --log warn"
  ```

- **Where the time goes** (new, and kept: [renderer](../subsystems/renderer.md#what-a-frame-waits-for), [apps](../subsystems/apps.md#flythroughs)): every frame record carries `cpu` — the renderer's `begin_frame` past its wait for the slot, and inside `submit_frame` the tables, `terrain_prepare`, declaring the passes, the graph's compile, its recording, the submission, and the pass whose recording took longest, from a clock the render graph now reads around every pass (`RenderGraph::pass_cpu_ns`). Offscreen, `cpu_ms` is `submit_frame`; the stages sum to it within a few microseconds.
- **The owner's flights** (`D:\workspace\game_engine_local\flythrough\endless-2026-10-04T0902`, `T0910`, `T0912`, windowed, recorded on `70f88d15`), summarized by a script frame by frame — and two of them replayed offscreen from their input logs (`--replay-input ... --benchmark`) on the after build, which runs the session's ticks and the renderer but not the window.

## The owner's windowed flights

| | T0912, traced shadows | T0910, maps | T0902, maps (long) |
|---|---|---|---|
| frames | 507 | 589 | 21,785 |
| `cpu_ms` median / p99 / max | 4.11 / 13.9 / 93.0 | 1.07 / 7.4 / 84.9 | 1.05 / 5.6 / 96.1 |
| frames over 8 / 16 / 25 ms | 73 / 4 / 1 | 4 / 1 / 1 | 104 / 15 / 5 |
| `submit_ms` median / p99 / max | **3.40 / 10.1 / 15.5** | 0.35 / 1.0 / 1.9 | 0.34 / 0.96 / 4.1 |
| pacer depth | 2 (498 frames) | 1 | 1 |

- **The max is the first frame** in all three (93, 85 and 96 ms): the first frame records and submits everything once. It is not a spike of the flight.
- **With traced shadows, the spikes are the submission**: 73 frames over 8 ms, nearly all with `submit_ms` 9.8–15.5 ms and the camera standing still, and a 3.4 ms submission at the median against 0.35 with the maps. Nothing in those frames uploaded tiles or rebuilt.
- **With the maps, the long flight's 104 frames over 8 ms** are neither the submission (0.4–1 ms) nor the ground's share (`terrain.host_ms` 0 in all but one); they are the rest of a window's `cpu_ms` — the renderer's recording or the window's own work — which a record did not break down before this change.

## Before: where the offscreen frames went

| `--shadows rt`, before | median | p95 | p99 | max |
|---|---|---|---|---|
| `cpu_ms` | 3.53 | 6.55 | 10.11 | 288.4 |
| of it, the graph's recording | 3.38 | 6.29 | 9.74 | 288.0 |
| of that, the slowest pass | 3.02 | 5.60 | 8.96 | 287.0 |
| the submission | 0.065 | 0.10 | 0.24 | 23.6 |
| the tables, the terrain, declaring, compiling | 0.004, 0.028, 0.028, 0.019 | | | |

Machine state: other processes 7.7% of the CPU and the GPU 8% busy (10.1 GB of its memory) at the start; 60% and 99% (12.6 GB) at the end.

- **One pass is the frame**: in 7,196 of 7,201 frames the slowest pass was **"tlas instances"**, and it was 3.0 of the frame's 3.5 ms at the median. Of the 163 frames over 8 ms (27 over 16, 17 over 25), 161 were in the recording and 157 of those in that pass — the 288 ms frame was 287 ms in it — and the other six were a submission (23.6 ms), a compile and four passes once each (`resolve`, `reset`, `sky view`, `terrain upload`), every one of them in the stretch below.
- **What the pass is**: one `vkCmdCopyBuffer` of each instance's bottom-level address into its top-level record — **47,849 regions of eight bytes**, one an instance, because the far levels hold two slots for every tile each square can hold and every slot is an instance. The driver encodes a copy region by region on the recording thread; a region list that long is milliseconds of the frame's thread every frame, whatever else the frame does. That is why recording "a graph that is the same graph" took 26–97 ms on 2026-10-03: the graph was the same, and one of its commands was 47,849 commands.
- **Why it spiked rather than only costing 3 ms**: the spikes cluster — 128 of the 163 between frames 3,000 and 5,000, while the machine's load rose (60% of the CPU and the GPU 99% busy by the end sample) — and the frame thread spends 3 ms inside the driver every frame, so it is in that call whenever it is descheduled or the driver stalls. Their size is the machine's; where they land is the pass's. No run with the pass gone spiked there.
- **With the maps** the same flight had **no frame over 8 ms** (`cpu_ms` median 0.48, p99 0.81, max 2.96; other processes 8.7% and 15.7% of the CPU at the two ends, the GPU 8% and 30% busy): the maps build no chain and record no such copy.
- **The ray tracing chain's capacity steps** are `begin_frame`'s, not `cpu_ms`'s offscreen: the one shrink of the flight (65,536 → 16,384 clusters, frame 161) waited 31.7 ms for the device there. In a window that wait is in `wait_ms`.
- **`terrain_prepare`** was 0.028 ms at the median and at most 7.5 ms: its per-frame staging buffer is a rule broken (below), not a spike's cause on this flight.

## The causes, in order of size

1. **The top-level records' addresses as a 47,849-region copy** (`tlas instances`): 3.0 ms of every traced-shadow frame, and 157 of its 163 frames over 8 ms. **Removed**: a dispatch of a thread an instance writes the same eight bytes into the same place (`tlas_references.slang`); the GPU builds the same structures, and the flight's cut is the same pairs frame by frame (visible pairs, structures built and wanted, and casters compared over all 7,201 frames: none differs).
2. **The windowed submission with traced shadows** (3.4 ms at the median, 10–15 ms spikes in T0912): offscreen the same submission is 0.07 ms, so in the window it is waiting, not working — the schema's note that a submission waiting on a swapchain image is a presentation wait wearing the CPU's name. Its rise with traced shadows coincides with the pacer at depth 2 and a frame thread that spent 3.5 ms recording; **not measured after the change** in a window (below).
3. **A device buffer made on the frame's thread for every chunk staged and every frame's slot records, and for every field**: a broken rule rather than a measured spike — `terrain` was 0.03 ms at the median offscreen — which the long flight could not see, because it counted engine allocations and a buffer is the driver's; it counts device buffers now. **Removed**: the staging ring and the kept field buffers ([renderer](../subsystems/renderer.md#what-a-frame-waits-for)).
4. **Load from the rest of the machine**: frames of 5–36 ms in passes that cost under a millisecond otherwise (`reset`, `sky frame`, the submission), clustered in time, in runs whose end sample had another process holding 30.7 of the GPU's 32.6 GB or 88% of the CPU. Not the engine's; quoted so that the after runs' few long frames are read for what they are.

## After

The same flight, frame-thread `cpu_ms` (ms), and the frames over 8, 16 and 25 ms. "After" is the dispatch with the staging ring; "after, kept fields" adds the kept field staging, and ran while this branch's debug build compiled beside it.

| | median | p95 | p99 | max | over 8 / 16 / 25 ms | machine state, start → end (others' CPU, GPU busy) |
|---|---|---|---|---|---|---|
| traced shadows, before | 3.53 | 6.55 | 10.11 | 288.4 | 163 / 27 / 17 | 7.7% → 60%, 8% → 99% |
| traced shadows, after | **0.43** | 0.56 | **0.71** | **3.27** | **0 / 0 / 0** | 9.0% → 20%, 8% → 6% |
| traced shadows, after, kept fields | 0.44 | 0.69 | 0.90 | 4.39 | 0 / 0 / 0 | 21% → 88%, 4% → 8% |
| maps, before | 0.48 | 0.64 | 0.81 | 2.96 | 0 / 0 / 0 | 8.7% → 16%, 8% → 30% |
| maps, after, kept fields | 0.48 | 0.77 | 1.09 | 35.9 | 3 / 2 / 1 | 9.1% → 33%, 4% → 7% (30.7 of 32.6 GB of the GPU's memory held by others at the end) |

- **With traced shadows the recording went from 3.38 to 0.30 ms at the median** and the slowest pass from 3.02 ms ("tlas instances") to 0.12 ms (whichever pass that frame was slowest in); no frame of either after run passed 4.4 ms, including the one that ran beside a build with 88% of the CPU busy.
- **With the maps nothing changed but the load**: the three long frames of the after run (35.9 ms in the submission, 21.3 in `reset`, 14.4 in `sky frame`) fell within 400 frames of each other while another process took the GPU's memory to 30.7 of 32.6 GB — passes that cost 0.1–0.2 ms in every other frame. They are the machine's (cause 4 above).
- **The cut is the same**: visible pairs, structures built and wanted, and shadow casters agree frame by frame over all 7,201 frames between the before and after traced-shadow runs.
- **Staging allocates nothing**: the long flight of `terrain_tiles_gpu_tests.cpp` (three far levels, 260 frames, each drawn) makes **0 device buffers** on the frame's thread over its second half, the ring holding at most 492,064 of its 33,554,432 bytes and never overflowing, and **0 engine allocations** on the host's half, as before.

## The owner's sessions replayed offscreen (after)

| | T0912 (traced shadows) | T0902 (maps) |
|---|---|---|
| frames | 726 | 16,011 |
| `cpu_ms` median / p99 / max | 0.24 / 0.73 / 4.6 | 0.31 / 1.03 / 12.7 |
| frames over 8 ms | 0 | 1 |

Machine state: T0912 other processes 18–23% of the CPU, the GPU 11–37% busy; T0902 5–26%, the GPU 4%.

- An offscreen replay's `cpu_ms` is the session's ticks and the renderer's `submit_frame`; it does not run the window — events, the title, the present, the pacer — nor the world's update. The T0902 replay's two longest frames (12.7 and 7.2 ms) had renderer stages of 0.3 ms between them: the rest is the session's own ticks, two frames in 16,011, not traced further.
- So **the long windowed flight's 104 frames over 8 ms are not reproduced offscreen** by either the renderer or the session: what is left of them is the window's own work. The records now carry the renderer's stages in a window too, so the owner's next flight says how much of a windowed spike is the renderer's.

## What is open

- **The windowed numbers after the change**: no window was opened here (the owner works at this desktop); the next windowed flight's records answer whether the traced-shadow submission falls to the maps' 0.35 ms, and the `cpu` block names the stage of any frame that still spikes.
- **A window's own work** (events, title, present, the world's update) has no breakdown in the record; the renderer's stages and `terrain.host_ms` are what a windowed spike can be checked against.
- **The top-level structure is built over every instance every frame**, 47,849 records of which most are empty slots with a null reference; the dispatch made writing them cheap, and the build itself is the GPU's (`gpu_ms.rt`), not measured apart here.
