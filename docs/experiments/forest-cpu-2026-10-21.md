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

## The third fix: a move waits for nothing (2026-10-22)

**The cause.** With the trees posed on a pool, the coordinator's stack samples of the 11520×2160 full case (208 moving animals, 22.2 ms against 17 ms of GPU) put the rest of the frame thread in `SceneRenderer::move_instances`: the PoC's actors are moved once a frame (`GameActors::flush`), and in a scene read whole the call waited for the device (`FrameContext::wait_idle`) and wrote the moved records with a blocking submission (`GpuScene::place_instances`, `gfx::submit_immediate`), because every frame in flight reads the one instance table. A host that moves anything every frame therefore paid its CPU time and its GPU time one after the other: at 720p with the sway off, 2.40 ms a frame against 0.69 ms of CPU and 1.29 ms of GPU.

**The fix.** The moved records ride the next frame's **record upload** (`GpuScene::prepare_records`): staged once each, with where they end, in the frame slot's own record staging, and copied into the table by a transfer pass the render graph orders after every frame in flight — the table is imported as last read at every stage, so the barrier in front of the copy waits on the GPU for every earlier command on the one queue — and before every pass of the frame that reads it. The terrain ring slots' records, which went through the same in-stream path inside `terrain_prepare`, go through it too. Why each reader sees one placement per frame, and why the staging is never rewritten under a copy: [renderer](../subsystems/renderer.md#moving-an-instance). The scene was not made dynamic, which would have changed its table layout and refused `--rt-templates`.

**Method.** As above, with three binaries — `before` (main at aa52a546), `after-a` (the flora fix, aa28d1bc) and `after-b` (this fix, a06aa3bc) — each copied out of its build tree, alternating within each repetition, each case twice; one `tools/gpu-lock.ps1 run` per case, the lock released between cases. `movement-720` is the surround matrix's 00-movement at 1280×720 with the sway off (`renderer.flora.motion=false,renderer.flora.wind_strength=0.7`), rt raster and rt shadows: the move wait alone. `full-surround` is 00-full at 11520×2160, rt and rt shadows, sway on: every fix together. `forest-720` is 00-forest at 1280×720, hw and cascaded maps, sway on, no animals: the control, which this fix should leave where after-a put it. Cells are median / p95 / p99 in ms over the frames from 5 s of world time on; `frame_loop` is allocations / device buffers after warm-up.

| case | binary | run | frame | cpu | cpu passes | gpu total | frame_loop | machine state (start → end) |
|---|---|---|---|---|---|---|---|---|
| movement-720 (1280×720, rt, rt shadows, sway off) | before | 1 | 2.40 / 2.94 / 4.18 | 0.69 / 0.88 / 2.10 | 0.046 / 0.060 / 0.131 | 1.295 / 1.328 / 1.566 | 0 / 0 | others' CPU 9 → 5%, GPU 2 → 2% |
| | after-a | 1 | 2.42 / 4.21 / 4.54 | 0.70 / 2.11 / 2.27 | 0.048 / 0.125 / 0.158 | 1.298 / 1.486 / 1.570 | 0 / 0 | 17 → 7%, 11 → 58% |
| | after-b | 1 | **1.35** / 2.26 / 2.58 | 0.67 / 2.01 / 2.20 | 0.047 / 0.114 / 0.129 | 1.289 / 1.545 / 1.639 | 0 / 0 | 8 → 7%, 2 → 2% |
| | before | 2 | 2.40 / 3.15 / 4.28 | 0.69 / 1.02 / 2.14 | 0.043 / 0.063 / 0.149 | 1.293 / 1.475 / 1.570 | 0 / 0 | 8 → 7%, 10 → 55% |
| | after-a | 2 | 2.38 / 2.81 / 4.35 | 0.69 / 0.81 / 2.19 | 0.044 / 0.060 / 0.124 | 1.294 / 1.492 / 1.586 | 0 / 0 | 5 → 8%, 8 → 58% |
| | after-b | 2 | **1.36** / 2.34 / 2.89 | 0.67 / 2.08 / 2.29 | 0.048 / 0.122 / 0.137 | 1.293 / 1.534 / 1.631 | 0 / 0 | 11 → 4%, 9 → 2% |
| full-surround (11520×2160, rt, rt shadows, sway on) | before | 1 | 39.17 / 43.07 / 63.44 | 21.82 / 25.76 / 45.51 | 20.79 / 24.58 / 40.86 | 16.61 / 17.49 / 17.97 | 0 / 0 | 9 → 7%, 2 → 2% |
| | after-a | 1 | 22.40 / 24.47 / 25.26 | 4.37 / 5.89 / 6.50 | 3.10 / 3.93 / 4.39 | 17.16 / 18.24 / 18.80 | 0 / 0 | 6 → 7%, 12 → 79% |
| | after-b | 1 | **17.24** / 18.99 / 19.64 | 4.49 / 6.10 / 6.86 | 3.17 / 4.14 / 4.65 | 17.05 / 18.18 / 18.74 | 0 / 0 | 11 → 11%, 12 → 2% |
| | before | 2 | 39.19 / 44.75 / 67.10 | 21.87 / 27.72 / 49.90 | 20.84 / 24.67 / 44.51 | 16.59 / 17.43 / 17.80 | 0 / 0 | 5 → 6%, 9 → 37% |
| | after-a | 2 | 22.21 / 24.45 / 25.22 | 4.48 / 6.26 / 6.76 | 3.18 / 4.08 / 4.57 | 16.86 / 17.93 / 18.37 | 0 / 0 | 2 → 7%, 2 → 81% |
| | after-b | 2 | **17.24** / 18.92 / 19.92 | 4.49 / 6.16 / 6.68 | 3.21 / 4.07 / 4.62 | 17.02 / 18.14 / 18.75 | 0 / 0 | 6 → 5%, 9 → 4% |
| forest-720 (1280×720, hw, csm, sway on: control) | before | 1 | 20.91 / 21.40 / 32.12 | 20.84 / 21.32 / 32.02 | 20.34 / 20.79 / 26.92 | 3.280 / 3.747 / 3.894 | 0 / 0 | 17 → 4%, 7 → 20% |
| | after-a | 1 | 3.50 / 3.96 / 4.29 | 1.72 / 2.08 / 2.25 | 1.262 / 1.580 / 1.759 | 3.359 / 3.828 / 4.073 | 0 / 0 | 4 → 9%, 2 → 2% |
| | after-b | 1 | 3.49 / 3.91 / 4.16 | 1.85 / 2.18 / 2.29 | 1.416 / 1.729 / 1.808 | 3.332 / 3.775 / 4.001 | 0 / 0 | 7 → 4%, 2 → 2% |
| | before | 2 | 20.88 / 21.81 / 26.47 | 20.82 / 21.74 / 26.39 | 20.33 / 21.21 / 25.52 | 3.279 / 3.692 / 3.879 | 0 / 0 | 5 → 7%, 1 → 2% |
| | after-a | 2 | 3.51 / 3.94 / 4.27 | 1.67 / 1.97 / 2.20 | 1.229 / 1.522 / 1.677 | 3.353 / 3.824 / 4.024 | 0 / 0 | 2 → 3%, 2 → 69% |
| | after-b | 2 | 3.50 / 3.97 / 4.23 | 1.77 / 2.16 / 2.33 | 1.337 / 1.670 / 1.880 | 3.347 / 3.822 / 4.055 | 0 / 0 | 3 → 6%, 3 → 99% |

**What it says.** With the move wait gone the moving frame is the GPU's: at 720p 2.40 → 1.36 ms over 1.29 ms of GPU, and at 11520×2160 22.2 → 17.2 ms over 17.0 ms. The CPU's own time did not change (0.69 → 0.67 ms at 720p, 4.4 → 4.5 ms in the surround): the fix does not make the frame's work cheaper, it stops the frame thread waiting for the GPU to drain before it records the next frame, so the two now overlap. The `cpu` tails vary between runs of one binary (after-a's 720p p95 was 2.11 ms in one run and 0.81 in the other), so they say nothing about the fix either way; the frame times' tails fell with their medians. The forest control is where after-a left it (3.50 ms against 3.50); its posing reads 0.1 ms higher in after-b's runs, inside the run-to-run spread of a pool of 35 workers on a shared box, on a scene with nothing to move, where the only new work is an empty `prepare_records`. No run allocated or made a device buffer in the frame loop.

**The combined result** for the surround full case (medians of the two runs, ms):

| binary | frame | cpu | cpu passes | gpu total |
|---|---|---|---|---|
| before (aa52a546) | 39.18 | 21.85 | 20.82 | 16.60 |
| after-a: the trees posed on a pool | 22.31 | 4.43 | 3.14 | 17.01 |
| after-b: and a move waits for nothing | **17.24** | 4.49 | 3.19 | 17.03 |

**The pictures are the same bytes.** Frame 300 of the movement replay at 1280×720 (rt, rt shadows, sway off) — 299 actor flushes and 62,192 moves before it, so after-b drew it with frames in flight across every move, where before and after-a waited at each — captured offscreen by before, after-a and after-b twice: all four PNGs are the same 1,417,583 bytes, SHA-256 `75091B81…74E38438`. Frame 299 of the same replay is another picture (`6DEBC55E…CFE62E6F`), so the frames compared were moving. The renderer's `dynamic_instances_tests.cpp` ("moving instances: a scene read whole …") holds the same to the scene loaded with the moved placements, on the mesh, vertex and ray-traced paths with two and three frames in flight.
