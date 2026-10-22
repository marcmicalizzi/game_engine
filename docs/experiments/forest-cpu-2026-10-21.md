# The forest's frame thread (2026-10-21)

Roadmap R93. A forest stress test — the forest wildlife proof of concept, another session's repository, 4,262 swaying trees (265,080 parts) among 4,358 instances — drew **20.7 ms median frames at 720p with 3.6 to 5.2 ms of GPU**, and 39.5 ms at 11520×2160. This page is where that time went, the two fixes that took it back, and the numbers before and after. A second agent appends a section for the third fix (instance placement without a GPU wait).

## The machine

RTX 5090 (32 GB), Intel i9-10980XE (18 cores, 36 threads, Cascade Lake, no efficiency cores), Windows 11, `msvc-release` builds. The box is shared with other agents' builds and GPU jobs, so every run records the harness's `machine_state` (`docs/subsystems/bench.md`, "Measuring on a shared machine"); `--require-quiet` is too strict here and was not used.

## The cause, and how it was found

**Stack samples, not guesses.** With no administrator rights for a profiler, the coordinator sampled an `msvc-release` engine-view with its PDB by attaching `cdb` (`~*kc 40; qd`) 25 times, 300 ms apart, to a run of the forest case offscreen (the script lives outside the repository, beside the owner's other perf tooling). **Every mid-frame sample of the render thread was inside `renderer::flora_part_poses`**, called from `GpuScene::flora_frame` inside `SceneRenderer::record_frame`: the CPU posed every part of every swaying tree, on the frame's thread, every frame — about 20 ms at any resolution, which is why the frame time hardly moved between 720p and 11520×2160 while the GPU time did. With the sway really off, the 720p frame is 0.64 ms (the CPU's `passes` 20.41 → 0.03 ms).

**And a second bug hid the first.** The harness's "wind off" runs passed `--tunable renderer.flora.motion=false --tunable renderer.flora.wind_strength=0.7`, and engine-view kept only the last `--tunable`, so every one of them swayed. A repeated flag now combines with the earlier ones (f036afeb; [apps](../subsystems/apps.md)).

## The fixes

1. **Repeated `--tunable` flags combine**, in order, so a later value for a name wins as it does within one list.
2. **The trees are posed on a pool**, to the serial loop's bytes: `renderer::FloraPoser` cuts the swaying instances at load into chunks of about equal part counts (at least 64 parts, at most 64 chunks) and runs them on a job system of its own with the frame thread helping; `renderer.flora.workers` sets its threads, 0 automatic (35 here). Why it is bit-exact, who owns the pool and how the chunk size was measured: [renderer](../subsystems/renderer.md#who-poses-the-trees).

## Before and after

**Method.** The before binary is `msvc-release` of main at aa52a546, the after one of branch `perf-forest` (aa28d1bc), each copied out of its build tree and run from the copy. Each case is the PoC's own `run.json` argument list with `--report` and `--benchmark` redirected, `--require-quiet` dropped, `--offscreen` in place of the window, present and pacing flags, and **one** `--tunable` comma list for both binaries (so the before binary's last-wins reading cannot change the configuration). The 1,200-frame input replay (20 s of world time at 60 ticks a second) drives each run; statistics are over the 901 frames from 5 s of world time on, not the summary's whole-run percentiles. Every case ran twice, alternating before and after, all twelve runs inside one `tools/gpu-lock.ps1 run`. Cells are median / p95 / p99 in ms; `frame_loop` is the summary's allocations / device buffers after warm-up.

| case | binary | run | frame | cpu | cpu passes | gpu total | frame_loop | machine state (start → end) |
|---|---|---|---|---|---|---|---|---|
| forest-720 (1280×720, hw, csm, sway on) | before | 1 | 21.15 / 21.96 / 27.98 | 21.08 / 21.88 / 27.88 | 20.54 / 21.23 / 23.62 | 3.28 / 3.72 / 3.88 | 0 / 0 | others' CPU 6 → 13%, GPU 4 → 13% |
| | after | 1 | **3.53** / 4.00 / 4.21 | 1.76 / 2.08 / 2.24 | **1.29** / 1.60 / 1.72 | 3.39 / 3.89 / 4.10 | 0 / 0 | 9 → 3%, 3 → 38% |
| | before | 2 | 20.96 / 21.96 / 23.60 | 20.89 / 21.88 / 23.53 | 20.37 / 21.34 / 22.95 | 3.29 / 3.79 / 4.05 | 0 / 0 | 8 → 5%, 7 → 16% |
| | after | 2 | **3.61** / 4.20 / 4.57 | 1.79 / 2.18 / 2.37 | **1.34** / 1.66 / 1.81 | 3.53 / 4.08 / 4.36 | 0 / 0 | 3 → 6%, 4 → 99% |
| forest-720-still (sway off: control) | before | 1 | 0.64 / 0.96 / 1.20 | 0.37 / 0.46 / 1.07 | 0.034 / 0.043 / 0.096 | 0.589 / 0.810 / 0.916 | 0 / 0 | 9 → 4%, 2 → 53% |
| | after | 1 | 0.65 / 1.18 / 1.34 | 0.37 / 1.06 / 1.19 | 0.035 / 0.096 / 0.098 | 0.590 / 0.840 / 0.922 | 0 / 0 | 8 → 7%, 7 → 2% |
| | before | 2 | 0.64 / 1.18 / 1.28 | 0.37 / 1.06 / 1.16 | 0.034 / 0.097 / 0.102 | 0.589 / 0.810 / 0.898 | 0 / 0 | 13 → 7%, 3 → 97% |
| | after | 2 | 0.64 / 0.98 / 1.26 | 0.37 / 0.46 / 1.10 | 0.034 / 0.044 / 0.098 | 0.588 / 0.823 / 0.950 | 0 / 0 | 3 → 3%, 3 → 2% |
| full-surround (11520×2160, rt, rt shadows, sway on, offscreen) | before | 1 | 39.63 / 44.73 / 63.16 | 21.98 / 26.76 / 44.56 | 20.93 / 24.19 / 40.31 | 16.90 / 18.14 / 18.88 | 0 / 0 | 5 → 5%, 13 → 37% |
| | after | 1 | **22.19** / 24.45 / 25.20 | 4.16 / 6.06 / 6.59 | **2.94** / 3.90 / 4.32 | 17.04 / 18.20 / 18.71 | 0 / 0 | 7 → 13%, 2 → 36% |
| | before | 2 | 39.47 / 47.77 / 62.84 | 21.86 / 29.99 / 45.32 | 20.80 / 26.25 / 39.47 | 16.83 / 18.10 / 18.59 | 0 / 0 | 9 → 7%, 2 → 3% |
| | after | 2 | **22.21** / 24.61 / 25.89 | 4.47 / 6.19 / 6.75 | **3.20** / 4.05 / 4.40 | 16.93 / 18.07 / 18.54 | 0 / 0 | 6 → 7%, 3 → 35% |

The harness samples whole-GPU utilisation, so a run's end figure may include its own last frames; the GPU lock was ours throughout, so no other measurement shared the GPU, though other processes may have used it. **What it says:** at 720p the frame went from 21.0 to 3.6 ms, the posing (inside `cpu.passes`) from 20.4 to 1.3 ms, about 16 times, and the frame is now the GPU's (3.4 ms) plus a little; at 11520×2160 the frame went from 39.5 to 22.2 ms and the posing from 20.9 to 3.1 ms, and what remains is the GPU's 17 ms. The still control did not move, and no run allocated in the frame loop. The surround's posing costs more than the 720p's (3.1 against 1.3 ms) with the same trees; not investigated — a longer frame leaves the workers asleep longer, and the ray-traced frame's driver work shares the CPUs — and it is still a seventh of what it was.

## The pictures are the same bytes

Frame 300 of the same replay at 1280×720 (hw, csm, wind 0.7, sway on), captured offscreen by the before binary, by the after binary with automatic workers (64 chunks, 35 workers), with `renderer.flora.workers=0` named, and with `renderer.flora.workers=1` (8 chunks, one worker): all four PNGs are the same 1,436,707 bytes, SHA-256 `31D3C746…DEAFF04`. The same frame with `renderer.flora.motion=false` is a different picture (`7DFB20F9…64ED2F`), so the trees were swaying in the compared frames. The renderer's `flora_poser_tests.cpp` holds the same thing for the poses themselves, byte for byte, with and without a walker.
