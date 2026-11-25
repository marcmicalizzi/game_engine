# Where the frame waits on the card (R219, 2026-11-24)

Roadmap R219, from the owner's walk of the forest's v59 on batch 74's binaries (the proof of concept's engine needs, item 179). The owner reads about 400 W in the forest where a full load of the card draws 600 to 610 W; at 3840 × 2160 the walk's frame is 20.9 ms with the GPU's timeline full, and Nsight's GPU metrics over the walk's poses (one run the owner made that afternoon) found the SMs active all the time, warps in flight at 30 % of the maximum and an instruction issued on 17 % of cycles, with DRAM at 2 to 5 % of its peak. So the card is not starved, not transferring and not spilling: its warps wait on latency inside the passes. This page asks which passes hold that wait, what in them waits, and what returns it, with what an agent may measure — the frame records' pass timers and `nvidia-smi dmon`, which needs no rights — and prepares the one capture that can split the counters by pass, which only the owner may run.

**In four lines.** *(Filled in at the end of stage 1.)*

## How it was measured

- **The scene and the settings: the owner's.** The forest staging (`<local>\staging\v41\scene-staging.json`, its own derived-data root and `materials-b69`), offscreen, 3840 × 2160 one view, `--raster rt --shadows rt`, micromaps on, `renderer.gi.mode=bounce` with `renderer.gi.guide=true` and `renderer.gi.memory=true`, `renderer.reconstruction.accumulation=on`, flora motion at wind 0.7 with `renderer.wind.speed_m_s=3`, `--no-lights --time-of-day 15 --world-rate 1 --lod 1 --deform-pool-mib 64 --rt-budget-mib 0 --time-rate 0 --terrain-tiles --terrain-far 6 --terrain-async`, `--memory-report`. With accumulation on a record carries its times under `reconstructed.gpu_ms` (ADR-0064's rule: never a cost on its own); every figure below is read from there and set beside a flight of the same settings, never against a native figure.
- **One path, two parts.** `stand-walk.json` (in the row's scratch, built from the staging's two paths): the lake-range standing pose (`standing-path.json`'s, 80, 14, 130 looking north-west across the lake) held 20 s, frames 0 to 600 at 30 fps; a cut to the grounded walk's start held 3 s (601 to 690, left out); then the grounded walk (`walk-grounded.json`, x 180 to 540 at z 1509, 4.5 m/s, the eye 1.7 m over the ground) for 80 s, frames 691 to 3090. Read: the standing frames 30 to 600 and the walk's 721 to 3090, each record's own milliseconds, the median and the 99th percentile.
- **The card's own figures beside every flight**: `nvidia-smi dmon -s pucvmt -o DT -d 1` from before the load to after the flight, read over each part's wall-clock window (the records' `frame_ms` summed back from the exit line of the flight's log): power (W), the SM-busy figure (`sm`: the share of the second some kernel ran, not how full the SMs were), the memory controller (`mem`), the graphics clock, PCIe receive and transmit. Each row gives the median and the 10th and 90th percentiles.
- **The binary**: main's release build at `0ac9fbaf` (main `962a9bd6d`'s code; nothing after it touches the engine), copied out of the tree. Every flight uses it; the pass labels this row adds are not in it and are off by default.
- **The machine**: RTX 5090 (32 GB, driver 621.24), i9-10980XE, 64 GB, Windows 26300; shared tonight with seven other rows' builds, tests and flights, the GPU lock held for each flight. Each flight's state is in its row: other processes' CPU, the free commit and physical memory at its start, what else held the card (`nvidia-smi` at the start and the end: the desktop's processes only, for every flight below). **The machine was hard-reset at 20:44** (cause unknown; the flights before it are kept where their records are whole) and every flight after it starts only with 30 GB of physical memory free (36 GB for a wide one, with 22 GB free on the device), and leaves two minutes between flights for the card's shorter users.

## The base flight

Two flights of the base settings 35 minutes apart (19:56 beside three debug builds, no `dmon` beside it; 20:29 beside one, the CPU's other processes at 52 % and 41 %, 58 GB of commit free) agree to 1 % in every pass, so a difference of a few percent between two flights below is a difference.

| GPU ms, median / p99 | standing (frames 30–600) | walking (721–3090) |
|---|---:|---:|
| the GPU's frame (`gpu_frame.span_ms`) | 17.04 / 19.28 | 20.47 / 26.31 |
| the frame's wall (`frame_ms`) | 20.25 / 60.88 | 20.45 / 61.76 |
| `resolve` | **9.33** / 9.89 | **7.34** / 9.71 |
| `trace` (the primary rays) | 1.54 / 1.55 | 2.99 / 4.94 |
| `rt` (the chain) | 1.44 / 1.46 | 3.46 / 4.06 |
| of it `clas` / `blas` / `tlas` | 0.20 / 0.20 / 0.59 | 1.99 / 0.42 / 0.52 |
| `gi_trace` / `gi_fold` | 1.02 / 0.31 | 1.55 / 0.36 |
| `frame_passes` (the medium, the accumulation's encode) | 1.56 / 3.35 | 1.40 / 1.67 |
| `motion` | 0.80 / 1.01 | 1.08 / 2.60 |
| `deform` | 0.64 / 0.85 | 0.81 / 2.41 |
| `cull` | 0.14 | 0.26 |
| visible pairs / shadow casters | 1,100,210 / 199,415 | 406,272 / 641,958 |
| cluster structures built a frame | 6,189 | 92,228 |
| **power, W** (p10, p90) | **378** (346, 409) | **376** (351, 394) |
| SM busy, % | 96 (29, 100) | 100 (70, 100) |
| memory controller, % | 18 (7, 19) | 19 (15, 22) |
| graphics clock, MHz | 2,872 | 2,880 |
| PCIe receive / transmit, MB/s | 204 / 144 | 1,364 / 1,194 |

**What the base says before anything is switched off.** The staging reproduces the owner's walk: 376 W where he read about 400, the SMs busy every second, the memory controller at a fifth, the clock pinned at 2.88 GHz with no power or thermal limit (`pviol` and `tviol` 0 throughout). The resolve is the frame's largest pass in both parts, more than half of the standing frame. Two things the frame records had not said: (1) **the walk moves 1.2 to 1.4 GB/s over PCIe each way with `upload_bytes` 0 in every frame**, against 0.1 to 0.2 GB/s standing — the streamed world's own traffic (tiles, textures, the mesh room) or the driver's, which the frame record does not count; the forest session saw 0.5 GB/s on the owner's replay and could not place it either; (2) **standing, the frame's wall is 3.2 ms longer than the GPU's own span** (20.25 against 17.04 ms, `elsewhere_ms` 0.01, `wait_late_ms` 3.46), so about a sixth of the standing frame is not inside any frame's timestamps; walking, the two are equal.
