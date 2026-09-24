# E1 on Pascal: the vertex path against the software rasterizer, on a GPU without mesh shaders

- **Question ([10 §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing), [ADR-0024](../adr/0024-hardware-rasterization-first.md) "Revisit when"):** E1 found no crossover between hardware and software rasterization of small clusters on the RTX 5090, and ADR-0024 says to rerun the sweep first on any target GPU without mesh shaders. This is that rerun, on the first such GPU the project can reach: does the vertex-shader path hold as the baseline tier, how does the software rasterizer compare there, and what does a frame cost on a 2017 card?
- **Date:** 2026-09-24. **Machine:** Intel Xeon E5-2670 (Sandy Bridge-EP, 8 cores / 16 threads, 2.6 GHz, no AVX2), 31 GB, Gentoo, kernel 7.2.3-gentoo; **GPU:** NVIDIA TITAN Xp (Pascal GP102, 12 GB, 2017), driver 580.178.04, Vulkan 1.4.312, PCIe x16, power limit 250 W. **Build:** `linux-server` (GCC 14.3.1, RelWithDebInfo, x86-64-v2, `ENGINE_WINDOW_BACKENDS=none`) of commit 9465a07, through `tools/remote-build.ps1`. **Control:** the RTX 5090 of [E1](e1-raster-crossover.md) (driver 610.88, Vulkan 1.4.341, Windows 11, i9-10980XE), `msvc-release` of the same commit.
- **Machine state, TITAN Xp: quiet at the start of every group; WARNING raised on 53 of 396 measured runs.** Every one of the 132 measurement groups started only once the 1-minute load average was under 1.0 and the GPU under 5% busy (the logged figures, re-read just after the decision, range 0.44–1.05); two other agents were building on the same CPU, and the driver waited **1,180 s** in all for them, 740 s once. Inside the runs the harness's own sampler put other processes at a **median 3% of the CPU, maximum 15.7%**; 22 runs crossed its 10% CPU threshold, and 32 its 20% GPU threshold on the sample taken *after* the run, which is this process's own frames draining (every sample taken before a run read 0%, and the card has no other client). The worst reads `WARNING: other processes used 15.7% of the CPU and the GPU was 0% busy; these numbers are upper bounds (cpu 16.2% (others 15.7%, own 0.5%), gpu 0% util 1028/12288 MiB, session unknown, gpu lock free)`. The flagged runs are indistinguishable from their neighbours: repeats agree within **1.3%** in every configuration. Nobody else uses that GPU: `nvidia-smi` showed 280–1,028 MiB in use, all of it this process's targets, and 0% busy before every group. After each warm-up the card sat at **1,809–1,860 MHz** SM and 5,702 MHz memory, 52–80 °C, at most 92 W.
- **Machine state, RTX 5090 control: WARNING raised on 142 of 195 runs, on the CPU and on the GPU's own tail.** Under the GPU lock (`agent-e1-pascal`); the GPU was 0–1% busy before every group and every run and after each warm-up ran at 2,760–2,910 MHz; about 8 GB of its 32 GB was held by other processes (the desktop and resident models) and the Vulkan budget never fell below 31.6 GB. Other agents were compiling: other processes used a median 9.6% and at most 45% of the CPU (90 runs over the 10% threshold; 101 runs over the GPU threshold on the after-run sample, which is the run's own work). The numbers are GPU timestamps of an otherwise idle card and the repeats agree within 3.4%, but by [the rule](README.md#the-machine-state-line) they are upper bounds; only the ratios are used.
- **Decision:** ADR-0024 stands — the vertex path is the baseline tier and wins 42 of the 44 configurations measured — with the two qualifications below recorded as a dated note under ADR-0024 in the [ADR index](../adr/README.md) and a status note in [04 §4.3](../plan/04-renderer.md#43-geometry). Nothing in the tree changed with this run. The change it pointed at — the vertex path drawing a cluster's own triangles — was built the same day and is measured in [After](#after-the-vertex-path-draws-the-cuts-own-triangles), which also corrects two things this page said about cluster capacity.

## Setup

**What E1 did, and what this run keeps.** E1 rasterized the procedural heightfield (`--grid 1025`: 2,097,152 triangles) through the mesh path and the software rasterizer from the same GPU LOD cut, at orbit distances 9, 24 and 60 (tenths of the scene radius; leaf triangles of about 4.5, 1.7 and 0.7 px at 2,160 rows), at LOD thresholds 0.5, 1, 2 and 4 px, at 3840×2160 and 11520×2160, and compared the **rasterization pass alone** — `gpu_ms.hw` against `gpu_ms.sw`, from `gfx::GpuTimer` timestamp queries, averaged over 150 frames. Its criterion: a crossover (the software path cheaper at some triangle size) would put software rasterization into Phase 1; none appeared. All of that is kept here, with the vertex path in the mesh path's place, because on this card `hw` resolves to `vertex` ([renderer](../subsystems/renderer.md#the-offscreen-contract)) and the mesh path does not exist: **the mesh path is unavailable on the TITAN Xp and is reported only from the 5090 control.** 1920×1080 and 2560×1440 are added to E1's two resolutions.

**What differs from E1's method, and why.**

1. **Offscreen, through `engine-host`'s `render.benchmark`**, not a composited `engine-view` window: the server has no display, and a composited window was E1's own named source of noise (its 4K rows varied 2× between identical runs).
2. **The camera is held still** at yaw 0, which is the frame-0 camera of E1's `--orbit`; E1's orbit turned about 52° over its 150 frames. A still camera keeps the cut identical in every frame and every repeat.
3. **Frames are counted in time, not in a fixed 150.** A first attempt with a 60-frame warm-up measured the card coming *up* from its idle clocks: the same 1080p configuration summed to 1.55, 0.77 and 0.71 ms of passes over three back-to-back repeats. Every configuration now gets a warm-up of about two seconds of GPU work (2,500 / 1,600 / 800 / 400 frames at 1080p / 1440p / 4K / 11520×2160) and then **three measured repeats** of 600 / 400 / 200 / 150 frames; the tables give the median of the three. The 5090 control used 4,000–20,000-frame warm-ups and 1,000–5,000-frame repeats for the same reason — its 1080p frame is 0.1 ms, and E1's 150 frames there would have been 15 ms of work on a card still in P8.
4. **Occlusion culling is off in the rows that repeat E1**, as it did not exist then, and is measured separately. Cone culling and LOD selection are on, as they are by default. Shadows are `off` explicitly (the TITAN Xp would resolve `auto` to off anyway; the 5090 would not).
5. **The heightfield is not E1's to the cluster.** The simplifier has changed since 2026-09-16 (it is attribute- and seam-aware, [04 §4.3](../plan/04-renderer.md#43-geometry)); today the TITAN Xp's build makes **46,639** clusters and the 5090's **46,628**, against E1's 46,626, and the cut at a given threshold is smaller than E1's (1,222 clusters at 11520×2160, orbit 9, LOD 1, against 1,435). Rows are compared within this run, never against E1's numbers. That the GCC x86-64-v2 build and the MSVC v3 build produce different cluster counts from the same procedural input was not investigated here; it was the same day ([content-build determinism](content-build-determinism.md)), and it is **not the same input**: the heightfield is made at run time with `std::sin` and `std::cos`, whose last bits differ between MSVC's C library and glibc, while from the same bytes the two builds make the same DAG.
6. **The FlightHelmet grid is added.** E1 measured only the heightfield. `grid_instances: 8` — 64 instances of the 94,722-triangle FlightHelmet, 2,356 clusters, 150,784 pairs — is the scene [E25](e25-deformed-clusters.md) and [renderer](../subsystems/renderer.md#performance-notes) measure at `--orbit 22`, and it is the one whose clusters are **not** full: the heightfield's clusters all hold 128 triangles, the helmet's leaves average 86. *(Corrected in [After](#after-the-vertex-path-draws-the-cuts-own-triangles): the build's capacity is 124 triangles, and neither cut is full — the heightfield's clusters hold 83 on average, the helmet grid's 54 at LOD 1 and 65 at LOD 0.25.)*
7. **Coverage is checked.** Each vertex/software pair at 1080p and 4K (heightfield) and 1080p and 1440p (helmet) was also captured through `render.capture` with the depth channel, whose `covered` count says whether the software pass drew the same pixels.

**Reproducing it.** One `engine-host --stdio` process per scene, fed one request per line and waiting for each response before the next. A group is a gated warm-up followed by three measured repeats; the gate (`/proc/loadavg` < 1.0 and `nvidia-smi` < 5%) runs before the warm-up, never between repeats:

```text
{"jsonrpc":"2.0","id":"load","method":"render.load","params":{"mesh":"","scene":"","grid":1025,"cache":false,"settings":{"raster":"vertex","shadows":"off","lod_px":1.0,"occlusion":false}}}
{"jsonrpc":"2.0","id":"gate-hf-vx-o9-l1-1920x1080-warm","method":"render.benchmark","params":{"scene":"scene1","width":1920,"height":1080,"orbit":{"distance":9,"yaw_deg":0},"settings":{"raster":"vertex","shadows":"off","view":"shaded","lod_px":1.0,"sw_px":32,"cull":true,"occlusion":false,"cone":true,"lights":true,"deform":"none"},"frames":2500}}
{"jsonrpc":"2.0","id":"m-hf-vx-o9-l1-1920x1080-r1","method":"render.benchmark","params":{ ...the same..., "frames":600}}
```

The raster column is `gpu_ms.hw + gpu_ms.sw` (the vertex path's draws time into `hw`; with occlusion on, both passes' draws); "sum of passes" is `gpu_ms.total`, the sum of the timed zones; "wall" is the run's `seconds` over its frames, which adds the barriers and the gaps between passes the zones do not cover. Everything is milliseconds per frame. The FlightHelmet was fetched with `tools/fetch-samples.ps1 -Name FlightHelmet` and copied to the server by hand, since `remote-build.ps1` syncs only what git tracks ([remote Linux builds](../ci/remote-linux.md)).

## Results

### The rows that repeat E1: heightfield, vertex path against the software rasterizer (TITAN Xp)

Raster pass alone, occlusion off; "clusters" is the cut. Leaf triangles are about 4.5 px at orbit 9 and 1.7 px at orbit 24 at 2,160 rows, and half that at 1,080 — but the cut is a LOD cut, and at a threshold of *t* pixels the triangles drawn are the ones whose simplification error projects under *t*, so the leaf size says how fine the source is rather than what is drawn.

**Orbit 9 (the terrain fills the frame)**

| resolution | LOD px | clusters | vertex | software | software ÷ vertex |
|---|---|---|---|---|---|
| 1920×1080 | 0.5 | 1,052 | 0.137 | 0.550 | 4.0× |
| 1920×1080 | 1 | 683 | 0.143 | 0.551 | 3.9× |
| 1920×1080 | 2 | 534 | 0.140 | 0.590 | 4.2× |
| 1920×1080 | 4 | 390 | 0.138 | 0.553 | 4.0× |
| 2560×1440 | 0.5 | 1,332 | 0.269 | 0.803 | 3.0× |
| 2560×1440 | 1 | 843 | 0.265 | 0.894 | 3.4× |
| 2560×1440 | 2 | 560 | 0.255 | 0.866 | 3.4× |
| 2560×1440 | 4 | 432 | 0.252 | 0.848 | 3.4× |
| 3840×2160 | 0.5 | 1,865 | 0.670 | 1.585 | 2.4× |
| 3840×2160 | 1 | 1,052 | 0.649 | 1.542 | 2.4× |
| 3840×2160 | 2 | 683 | 0.631 | 1.445 | 2.3× |
| 3840×2160 | 4 | 534 | 0.612 | 1.353 | 2.2× |
| 11520×2160 | 0.5 | 2,150 | 0.993 | 2.146 | 2.2× |
| 11520×2160 | 1 | 1,222 | 0.969 | 2.052 | 2.1× |
| 11520×2160 | 2 | 786 | 0.936 | 1.825 | 1.9× |
| 11520×2160 | 4 | 624 | 0.916 | 1.703 | 1.9× |

**Orbit 24 (about a third of the frame)**

| resolution | LOD px | clusters | vertex | software | software ÷ vertex |
|---|---|---|---|---|---|
| 1920×1080 | 0.5 | 561 | 0.032 | 0.140 | 4.4× |
| 1920×1080 | 1 | 325 | 0.029 | 0.189 | 6.5× |
| 1920×1080 | 2 | 190 | 0.027 | 0.269 | 9.9× |
| 1920×1080 | 4 | 95 | 0.025 | 0.368 | 14.5× |
| 2560×1440 | 0.5 | 702 | 0.052 | 0.242 | 4.6× |
| 2560×1440 | 1 | 401 | 0.052 | 0.344 | 6.6× |
| 2560×1440 | 2 | 211 | 0.049 | 0.425 | 8.6× |
| 2560×1440 | 4 | 121 | 0.049 | 0.415 | 8.6× |
| 3840×2160 | 0.5 | 1,038 | 0.125 | 0.431 | 3.4× |
| 3840×2160 | 1 | 561 | 0.127 | 0.548 | 4.3× |
| 3840×2160 | 2 | 325 | 0.125 | 0.553 | 4.4× |
| 3840×2160 | 4 | 190 | 0.120 | 0.520 | 4.3× |
| 11520×2160 | 0.5 | 1,038 | 0.127 | 0.435 | 3.4× |
| 11520×2160 | 1 | 561 | 0.128 | 0.549 | 4.3× |
| 11520×2160 | 2 | 325 | 0.126 | 0.557 | 4.4× |
| 11520×2160 | 4 | 190 | 0.122 | 0.518 | 4.2× |

**Orbit 60, LOD 0.5 (E1's sub-pixel row)**

| resolution | clusters | vertex | software | software ÷ vertex |
|---|---|---|---|---|
| 1920×1080 | 195 | 0.010 | 0.037 | 3.7× |
| 2560×1440 | 195 | 0.010 | 0.059 | 5.7× |
| 3840×2160 | 388 | 0.019 | 0.093 | 4.9× |
| 11520×2160 | 388 | 0.019 | 0.093 | 4.9× |

**The software rows on the heightfield are lower bounds.** The coverage captures agree within two pixels at orbit 60, at orbit 24 with LOD 0.5, and at orbit 24 at 1080p up to LOD 2; everywhere else the software pass **draws less of the picture** than the vertex path: at orbit 9 it misses 0.15% (1080p, LOD 0.5) to **6.2%** (1080p, LOD 4) of the covered pixels, and at 4K from 2.4% (LOD 0.5) to **24.9%** (LOD 4); at orbit 24 it misses up to 12.4% (4K, LOD 4). `cluster_sw_raster.slang` skips any triangle whose bounding box exceeds 64 × 64 pixels and any triangle with a vertex behind the camera, both by design ("larger triangles belong to the hardware path"), and both limits were in the shader E1 measured. A software pass that drew those triangles would cost more than the tables say, so every ratio above understates the vertex path's lead.

### The FlightHelmet grid (TITAN Xp)

64 instances at `--orbit 22`. The frame as it runs by default on this card — vertex path, occlusion on — with the software rasterizer's raster pass beside the vertex path's (both with occlusion off, since the software path has none). The coverage captures agree within two pixels in all four checked configurations, so the software column is a complete picture here.

| LOD px | resolution | pairs (occl. on) | cull | raster | Hi-Z | resolve | **sum, occlusion on** | sum, occlusion off | wall, on | software raster |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 1920×1080 | 1,713 | 0.066 | 0.060 | 0.130 | 0.142 | **0.399** | 0.330 | 0.47 | 0.099 (1.6× vertex) |
| 1 | 2560×1440 | 2,403 | 0.068 | 0.081 | 0.220 | 0.230 | **0.599** | 0.533 | 0.69 | 0.166 (2.0× vertex) |
| 1 | 3840×2160 | 3,426 | 0.067 | 0.115 | 0.475 | 0.483 | **1.140** | 1.097 | 1.31 | 0.340 (2.8× vertex) |
| 1 | 11520×2160 | 3,426 | 0.067 | 0.116 | 1.400 | 1.029 | **2.611** | 2.748 | 3.06 | 0.341 (2.8× vertex) |
| 0.25 | 1920×1080 | 5,724 | 0.067 | 0.190 | 0.130 | 0.145 | **0.533** | 0.472 | 0.60 | **0.139 (0.7× vertex)** |
| 0.25 | 2560×1440 | 7,534 | 0.069 | 0.248 | 0.219 | 0.238 | **0.774** | 0.714 | 0.86 | **0.221 (0.8× vertex)** |
| 0.25 | 3840×2160 | 11,086 | 0.070 | 0.367 | 0.475 | 0.494 | **1.406** | 1.373 | 1.57 | 0.427 (1.1× vertex) |
| 0.25 | 11520×2160 | 11,086 | 0.070 | 0.368 | 1.399 | 1.048 | **2.885** | 3.025 | 3.33 | 0.427 (1.1× vertex) |

At LOD 0.25 and 1080p the vertex path's raster pass is 0.199 ms with occlusion off and the software rasterizer's 0.139; at 1440p, 0.261 against 0.221. **That is a crossover**, the first E1's method has found.

### The frame on the heightfield, and what occlusion culling costs (TITAN Xp, vertex path)

Occlusion off → on. Two-pass occlusion culling draws the previous frame's visible set, builds the Hi-Z pyramid, culls the rest against it and draws what survives; with the camera held still pass 2 drew nothing in any run, and culling removed at most 0.5% of the heightfield's pairs and 3% of the helmet grid's. With occlusion on, the last Hi-Z build also writes a coverage word per 32 × 32 tile and **the resolve skips empty tiles** ([renderer](../subsystems/renderer.md#performance-notes)); that is gated on occlusion, so it is part of what turning occlusion on buys.

| camera | resolution | cull | raster | Hi-Z | resolve | sum of passes | wall per frame |
|---|---|---|---|---|---|---|---|
| orbit 9, LOD 1 | 1920×1080 | 0.024 → 0.050 | 0.143 → 0.144 | – → 0.129 | 0.476 → 0.471 | 0.642 → **0.794** | 0.70 → 0.86 |
| orbit 9, LOD 1 | 2560×1440 | 0.024 → 0.050 | 0.265 → 0.265 | – → 0.221 | 0.833 → 0.815 | 1.122 → **1.352** | 1.20 → 1.44 |
| orbit 9, LOD 1 | 3840×2160 | 0.023 → 0.051 | 0.649 → 0.654 | – → 0.474 | 1.846 → 1.788 | 2.518 → **2.967** | 2.67 → 3.14 |
| orbit 9, LOD 1 | 11520×2160 | 0.025 → 0.052 | 0.969 → 0.955 | – → 1.399 | 3.894 → 3.066 | 4.888 → **5.471** | 5.32 → 5.91 |
| orbit 24, LOD 1 | 1920×1080 | 0.029 → 0.049 | 0.029 → 0.032 | – → 0.129 | 0.259 → 0.187 | 0.317 → **0.398** | 0.37 → 0.48 |
| orbit 24, LOD 1 | 2560×1440 | 0.029 → 0.049 | 0.052 → 0.055 | – → 0.221 | 0.456 → 0.316 | 0.537 → **0.640** | 0.62 → 0.74 |
| orbit 24, LOD 1 | 3840×2160 | 0.029 → 0.050 | 0.127 → 0.128 | – → 0.473 | 1.024 → 0.676 | 1.180 → **1.327** | 1.33 → 1.49 |
| orbit 24, LOD 1 | 11520×2160 | 0.029 → 0.049 | 0.128 → 0.128 | – → 1.398 | 2.654 → 1.234 | 2.811 → **2.809** | 3.24 → 3.25 |
| orbit 60, LOD 0.5 | 1920×1080 | 0.030 → 0.050 | 0.010 → 0.011 | – → 0.129 | 0.194 → 0.099 | 0.234 → **0.289** | 0.30 → 0.39 |
| orbit 60, LOD 0.5 | 2560×1440 | 0.030 → 0.050 | 0.010 → 0.012 | – → 0.222 | 0.348 → 0.163 | 0.389 → **0.447** | 0.47 → 0.54 |
| orbit 60, LOD 0.5 | 3840×2160 | 0.029 → 0.049 | 0.019 → 0.021 | – → 0.473 | 0.805 → 0.346 | 0.853 → **0.889** | 1.01 → 1.05 |
| orbit 60, LOD 0.5 | 11520×2160 | 0.029 → 0.049 | 0.019 → 0.022 | – → 1.399 | 2.480 → 0.874 | 2.528 → **2.342** | 2.96 → 2.78 |

### The RTX 5090, same build of the tree

The control that separates "Pascal is different" from "the code changed since E1". Raster pass alone, occlusion off, except the Hi-Z column (occlusion on, mesh path).

| scene | resolution | clusters | mesh | vertex | software | vertex ÷ mesh | software ÷ mesh | Hi-Z |
|---|---|---|---|---|---|---|---|---|
| heightfield, orbit 9, LOD 1 | 1920×1080 | 722 | 0.0186 | 0.0199 | 0.2269 | 1.07× | 12.2× | 0.021 |
| heightfield, orbit 9, LOD 1 | 3840×2160 | 1,083 | 0.0538 | 0.0556 | 0.3583 | 1.03× | 6.7× | 0.088 |
| heightfield, orbit 9, LOD 1 | 11520×2160 | 1,246 | 0.0986 | 0.0985 | 0.4581 | 1.00× | 4.6× | 0.407 |
| heightfield, orbit 24, LOD 1 | 1920×1080 | 315 | 0.0076 | 0.0075 | 0.0971 | 0.99× | 12.8× | 0.021 |
| heightfield, orbit 24, LOD 1 | 3840×2160 | 558 | 0.0168 | 0.0177 | 0.2000 | 1.05× | 11.9× | 0.090 |
| heightfield, orbit 24, LOD 1 | 11520×2160 | 558 | 0.0255 | 0.0247 | 0.2062 | 0.97× | 8.1× | 0.409 |
| heightfield, orbit 60, LOD 0.5 | 1920×1080 | 188 | 0.0043 | 0.0038 | 0.0212 | 0.88× | 4.9× | 0.021 |
| heightfield, orbit 60, LOD 0.5 | 3840×2160 | 385 | 0.0064 | 0.0064 | 0.0388 | 1.00× | 6.1× | 0.091 |
| heightfield, orbit 60, LOD 0.5 | 11520×2160 | 385 | 0.0078 | 0.0071 | 0.0395 | 0.91× | 5.1× | 0.418 |
| FlightHelmet ×64, LOD 1 | 1920×1080 | 1,766 | 0.0068 | 0.0136 | 0.0479 | 2.00× | 7.0× | 0.020 |
| FlightHelmet ×64, LOD 1 | 2560×1440 | 2,478 | 0.0089 | 0.0181 | 0.0772 | 2.03× | 8.7× | 0.025 |
| FlightHelmet ×64, LOD 0.25 | 1920×1080 | 5,994 | 0.0152 | 0.0380 | 0.0525 | 2.50× | 3.5× | 0.019 |
| FlightHelmet ×64, LOD 0.25 | 2560×1440 | 7,894 | 0.0197 | 0.0503 | 0.0810 | 2.55× | 4.1× | 0.025 |

The row E1 decided on, 11520×2160 at orbit 9 and LOD 1, read 0.393 ms (mesh) and 0.604 ms (software), 1.5×, on 2026-09-16. Today the same card reads 0.099 and 0.458, **4.6×**. E1's mesh-path figure is four times today's for a cut only 15% larger (1,435 clusters against 1,246); the likeliest explanation is the one this run's first attempt reproduced — clocks that had not come up, in a composited window — though E1's run cannot be repeated to prove it. Either way E1's conclusion was right, and understated.

## What surprised me

- **The crossover exists on Pascal, and it is not where E1 looked.** On E1's own scene there is none: the vertex path wins all 36 heightfield configurations, by 1.9× to 14.5×, while the software pass is also dropping up to a quarter of the picture. But on the helmet grid at a fine cut (LOD 0.25, 5,724–7,534 pairs) the software rasterizer is **30% cheaper at 1080p and 15% cheaper at 1440p**, with identical coverage. At 4K and above, and at LOD 1, the vertex path wins again (1.1× to 2.8×).
- **Why: the vertex path pays for cluster capacity, not for triangles.** It draws a fixed 3 × 128 = 384 vertex invocations per visible cluster and collapses the ones past the cluster's triangle count, and every invocation loads its instance, cluster and mesh records ([`cluster_vertex.slang`](../../domain/gfx/shaders/cluster_vertex.slang)); the software pass transforms each of a cluster's vertices once into shared memory. On the heightfield every cluster is full and the two hardware paths are the same cost on the 5090 (0.88–1.07×); on the helmet grid, whose clusters are partly empty, **the vertex path is 2.0–2.55× the mesh path on the 5090** — the same inefficiency, which on a card with a slower geometry front end is enough to hand small clusters to the software rasterizer. *(Half right, and the numbers are off: the capacity is 124, so 372 invocations, and the heightfield's clusters are two thirds full, not full. On Pascal the invocations past a cluster's count turned out nearly free; what the draw paid for was one invocation per triangle **corner** of the ones within it — [After](#after-the-vertex-path-draws-the-cuts-own-triangles).)*
- **Software cost is not "roughly proportional to cluster count and weakly dependent on triangle size"** (E1's reading). At orbit 24 and 1080p the software pass goes from 0.140 ms for 561 clusters (LOD 0.5) to 0.368 ms for 95 clusters (LOD 4): 0.25 µs a cluster to 3.9 µs, because one thread walks one triangle's bounding box and a coarser cut has bigger triangles. The vertex path over the same rows falls from 0.032 to 0.025 ms.
- **The vertex path's cost on a frame-filling view is pixels, not clusters.** At orbit 9 it is flat across a 2.7× to 3.4× range of cut sizes (0.137–0.143 ms at 1080p, 0.916–0.993 ms at 11520×2160): the terrain covers every pixel, and the cost is the fragment stage's 64-bit atomic max.
- **The pass that matters on Pascal is the resolve, then the Hi-Z.** At 11520×2160, orbit 9, the resolve is 3.9 ms of a 4.9 ms frame, 80%; the raster pass is 1.0 ms. Across the two cards the passes scale very differently: raster 9.8× slower on the TITAN Xp than on the 5090, the resolve 11×, the Hi-Z only 3.4× (1.40 against 0.41 ms) — about the ratio of the two cards' memory bandwidth, and it is a fixed cost of about **0.056–0.062 ms per megapixel** on the TITAN Xp whatever is in the frame (0.13 ms at 1080p for a helmet grid covering 1.5% of the pixels).
- **Occlusion culling, as it runs by default here, is a net cost at every resolution up to 4K, and pays only at 11520×2160 on a mostly empty frame — and then only through the resolve.** On these scenes it culled at most 3% of the pairs. What it buys is the resolve's empty-tile skip, which saved up to 1.6 ms (and nothing on a frame the terrain fills); what it costs is the Hi-Z pyramid plus a second cull. Net: **+0.15 ms (+24%)** at 1080p on the frame-filling heightfield and +0.58 ms at 11520×2160 on the same view, +0.07 ms (+21%) on the helmet grid at 1080p, break-even at 11520×2160 on orbit 24, and a 0.14–0.19 ms saving at 11520×2160 where the frame is mostly sky. The tile skip does not need the pyramid; it needs the coverage words, which the first of the Hi-Z's dispatches writes.

## What it decides

**ADR-0024 against its own criterion.** The ADR said the software rasterizer "remains available for GPUs without mesh shaders", and asked that the sweep be rerun on one before relying on that. On the one measured:

1. **The vertex path holds as the baseline tier.** It is the faster rasterizer in 42 of 44 configurations, including every configuration of E1's scene; nothing here argues for a different default on Pascal.
2. **The software rasterizer is not the fallback for GPUs without mesh shaders — the vertex path is.** Where the ADR imagined a no-mesh-shader GPU as the software rasterizer's use, on the real one it loses by 1.9× to 14.5× on the heightfield. Its one win is the narrow case of many small, partly filled clusters at up to 1440p, worth 0.04–0.06 ms of a 0.5–0.8 ms frame.
3. **"No crossover, and every split moves cost the wrong way" does not carry over to Pascal.** E1's split table was taken on the 5090 and the heightfield, where the software pass loses everywhere. On the TITAN Xp there is a region where it wins, so a split by projected cluster size could beat pure vertex there. **The split was not measured**: on a device without mesh shaders `resolve_settings` turns `auto` into `vertex` (`systems/renderer/src/settings.cpp`), so the configuration does not exist on this card. That is a renderer change, not a benchmark fix, and it is the first follow-up below.
4. **Phase 1 stays as the ADR has it.** The amounts are small at the frame level and the cheaper fix is in the vertex path itself: drawing only a cluster's real triangles would remove most of the gap the software pass exploits, on every card that runs the vertex path, without a second list.

**What a frame costs on a 2017 card.** The visibility pipeline as it stands — cull, raster, Hi-Z, and a resolve with a sun, a sky, two point lights and bindless textures, but no shadows, no post-processing and no upscaler — on the helmet grid at the default settings (vertex path, occlusion on, LOD 1): **0.40 ms of GPU passes and 0.47 ms a frame at 1920×1080, 0.60 and 0.69 ms at 2560×1440** (0.53/0.60 and 0.77/0.86 at LOD 0.25). E1's heightfield at orbit 9, the frame-filling view: 0.79/0.86 ms at 1080p and 1.35/1.44 ms at 1440p. Against plan 04's baseline-tier bar of 30 fps at 2560×1440 on the Maxwell TITAN X, that is under 5% of a 33 ms frame on the Pascal card — but the Maxwell box has not been measured, and the lighting that will fill the rest of the frame does not exist yet.

**What it does not decide.** Whether to build the vertex-plus-software split (it has to be measured first); anything about Maxwell; anything about ray tracing, which this card cannot do ([04 §4.4](../plan/04-renderer.md#44-ray-tracing)).

## Follow-ups

- *(Demoted 2026-09-24: with the indexed draw the vertex path beats the software rasterizer in every configuration here, so nothing measured motivates this any more — [After](#after-the-vertex-path-draws-the-cuts-own-triangles).)* **Let `auto` split on the vertex path** (vertex for large clusters, software below `sw_px`) and rerun E1's split table and the helmet rows on the TITAN Xp. The cull pass already splits by projected diameter independently of `CullParams::count_index`; the change is in `resolve_settings` and `record_frame`'s raster-mode selection, plus keeping occlusion and ray-traced shadows off for a split as they are on the mesh path.
- ~~**Make the vertex path draw a cluster's real triangle count**, not its capacity: one indirect draw per visible cluster with `vertexCount = 3 × triangle_count` (`vkCmdDrawIndirectCount`), or clusters binned by triangle count. The helmet rows measure what it is worth: up to 2.5× on the 5090's vertex path, and the whole of the software rasterizer's win on Pascal.~~ **Done, 2026-09-24, and neither of those two ways** — the draw per cluster cost more than the whole pass on the TITAN Xp, and drawing only the real triangles three invocations a triangle did not help there. What worked is one indexed draw of the cut's triangles; see [After](#after-the-vertex-path-draws-the-cuts-own-triangles).
- **Decouple the resolve's tile skip from occlusion culling.** A coverage-only pass after the last raster pass, when occlusion is off, would give the resolve its saving (up to 1.6 ms here) without the 0.13–1.40 ms pyramid. And with the pyramid costing that much on Pascal, whether occlusion should default on for the baseline tier deserves a scene with real occluders before anyone decides; these two scenes barely have any.
- **Run the same requests on the Maxwell TITAN X** when the minimum-spec machine is reachable; it is plan 04's baseline-tier target, and Pascal is a generation above it.
- **The software rasterizer's silent drops.** Its 64 × 64-pixel bounding-box limit and missing clipping make pure `--raster sw` a picture with holes near the camera; any future measurement of it should check coverage, as this one did.

## Caveats

- One Pascal card, one driver, one CPU, two scenes. The two scenes differ in exactly the property that decided the question (full against partly filled clusters), which is luck rather than design; a corpus of generated props ([E10](e10-generated-props.md)) would say how common the helmet's case is.
- Linux only on the TITAN Xp; the 5090 control is Windows. The heightfield's cut differs between the two builds (46,639 against 46,628 clusters), so cross-card comparisons are of passes, not of identical workloads. The cause is the heightfield's `std::sin` on two C libraries, not the builder ([content-build determinism](content-build-determinism.md)); a heightfield made without the C library would make the workloads identical.
- A still camera. E1's orbit averaged over 52° of views; the cut and the occlusion numbers here are one view per configuration, and with a moving camera occlusion's pass 2 would have work to do.
- The timings are GPU timestamps around each pass; the wall column adds 4.5–35% (0.05–0.45 ms, median 12%), which is barriers and idle gaps between passes rather than work.
- No clock locking (the server allows no privileged commands). The warm-ups brought the card to the same 1,809–1,860 MHz before every configuration, and the repeats agree within 1.3%.

## After: the vertex path draws the cut's own triangles

- **Change:** the second follow-up above, built the same day. Where the device has `geometryShader` and `fullDrawIndexUint32` — both GPUs here — the vertex path's culled draw is **one indexed draw of exactly the triangles the cut's clusters hold**, with indices `slot << 8 | local vertex` so the post-transform cache shares a cluster's vertices between its triangles; the capacity draw stays for devices without them, for culling off, and as the fallback when a run's index budget runs out ([gfx](../subsystems/gfx.md), "Baseline tier"; [renderer](../subsystems/renderer.md)). The pictures are byte-identical to the capacity draw's (renderer captures, occlusion off and on, and the renderer's own case).
- **Date:** 2026-09-24. **Machines, drivers, method:** as above — `render.benchmark` offscreen through `engine-host --stdio`, a still camera at yaw 0, about two seconds of warm-up, three measured repeats, medians; occlusion off, cone culling on, shadows off. **Builds:** TITAN Xp, `linux-server` through `tools/remote-build.ps1`: *before* is a build whose vertex path is main's capacity draw (`cull_main` and `vs_cluster`, selected at run time in a prototype build of this branch — its numbers match this page's first run to within 2%), *after* is this change. RTX 5090, `msvc-release`: *before* is main at 794824d, *after* is this change, built and run back to back. The mesh path's numbers did not move (0.0186 ms at orbit 9 and 1080p before and after, as in the table above), which is the check that `cull_main` is untouched.
- **Machine state, TITAN Xp: quiet at the start of every group but one; WARNING on 10 of 135 runs for the CPU.** The same gate as above (1-minute load under 1.0, GPU under 5%, logged at 0.61–0.99), which waited 2,250 s in all — 1,710 s of it for another agent's flythrough run. Inside the runs other processes used a median 2.9% and at most 16.4% of the CPU; 13 after-run samples crossed the GPU threshold, which is this process's own frames draining. **The exception** is the first group, helmet grid at LOD 1 and 1080p with the capacity draw: a superseded prototype run of this work was still on the card and its first repeat started with the GPU 100% busy (1,946 MiB in use). Its figure, 0.0591 ms, agrees within 0.3% with the same configuration measured quiet in two earlier rounds (0.0590, 0.0593) and in the diagnostic round below. After each warm-up the card ran at 1,835–1,873 MHz, 48–71 °C. Repeats agree within **0.9%** (median 0.14%).
- **Machine state, RTX 5090: quiet GPU; WARNING on 28 of 135 runs for the CPU.** Under the GPU lock (`agent-vertex-draw`). The GPU was at most 1% busy before every run; 8.4–9.5 GB of its 32 GB was held by other processes. Other processes used a median 5.7% and at most 26.8% of the CPU (28 runs over the harness's 10% threshold, 2 over 20%), and 55 after-run samples crossed its GPU threshold, which is the run's own frames draining. Repeats agree within 2.2% (median 0.13%); by [the rule](README.md#the-machine-state-line) these are upper bounds, and only the before/after ratios are used.

### What the capacity draw was paying for (TITAN Xp)

Two diagnostic variants of the capacity draw, same cut, same build: *null*, whose fragment stage writes nothing, and *reject*, whose vertex shader runs in full and then places every vertex outside the clip volume so nothing rasterizes. Raster pass, ms:

| scene | capacity | fragment stage writes nothing | nothing rasterized |
|---|---|---|---|
| helmet grid, LOD 1, 1920×1080 | 0.0590 | 0.0584 | 0.0581 |
| helmet grid, LOD 0.25, 1920×1080 | 0.1958 | 0.1950 | 0.1947 |
| helmet grid, LOD 0.25, 2560×1440 | 0.2568 | 0.2564 | 0.2559 |
| heightfield, orbit 9, LOD 1, 1920×1080 | 0.1402 | 0.0277 | 0.0244 |

**On the helmet grid the raster pass is the vertex stage and nothing else**; on the frame-filling heightfield it is 80% the fragment stage's 64-bit atomics, which no change to the draw can touch. The helmet rows fit about **5.1–5.4 ns per 32-lane warp that holds at least one real vertex invocation**, and warps wholly past a cluster's triangle count cost close to nothing — which is why drawing *only the real triangles* does not help by itself: the warps it removes were already free, and it keeps one invocation per triangle corner, three per triangle, where a closed mesh has about one vertex for every two triangles, so each vertex is shaded about six times. The lever is vertex reuse.

### What was tried

Raster pass on the TITAN Xp, ms (cull pass in parentheses where the variant changed it; the capacity draw's cull is 0.037). Each was a prototype selected at run time in one build; the 5090's column is helmet LOD 0.25 at 1080p.

| draw | helmet LOD 1, 1080p | helmet LOD 0.25, 1080p | helmet LOD 0.25, 1440p | heightfield orbit 9, 1080p | RTX 5090 |
|---|---|---|---|---|---|
| capacity (before) | 0.059 | 0.196 | 0.257 | 0.140 | 0.0380 |
| one draw per cluster, `vkCmdDrawIndirectCount` | 0.507 | 0.581 | 0.615 | – | 0.0385 |
| the real triangles from a per-triangle table, one draw | 0.046 (0.042) | 0.196 (0.057) | 0.274 (0.066) | 0.164 (0.030) | 0.0235 |
| chunks of 32 triangles, instanced | 0.060 | 0.240 | 0.332 | 0.160 | 0.0272 |
| chunks of 8 triangles | 0.049 | 0.207 | 0.290 | – | 0.0247 |
| indexed, indices written by the cull pass's survivor thread | 0.023 (0.061) | 0.080 (0.120) | 0.107 (0.148) | – | 0.0458 |
| **indexed, indices written by an expansion pass (after; raster includes it)** | **0.032** (0.039) | **0.108** (0.041) | **0.145** (0.041) | 0.152 (0.026) | 0.0448 |
| software rasterizer, for scale | 0.097 | 0.137 | 0.218 | 0.544 | 0.0525 |

- **A draw per cluster** is the obvious design and the TITAN Xp's worst: `vkCmdDrawIndirectCount` over the visible list costs a fixed **0.48 ms plus 17 ns a draw** there (fit over six cuts from 1,766 to 11,693 draws), more than the whole pass it was meant to shorten; on the 5090 it costs 2.5 ns a draw and breaks even.
- **The real triangles, three invocations each**, from a per-triangle table the cull pass fills, is the 5090's best variant (−38%) and does nothing for the TITAN Xp at the fine cut (−23% at LOD 1, 0% at LOD 0.25 and 1080p, +7% at 1440p, +17% on the heightfield), for the reason above. Chunks, which trade the table for per-chunk instances, are worse on both.
- **Indexed** is the one that shares vertices, and the only one that helps Pascal; it is also the only one that makes the 5090's vertex path slower. The two cards disagree about the vertex front end, and the renderer follows the baseline tier's card, since the 5090 draws the mesh path. Its first version wrote each survivor's indices from the cull pass's own thread, one thread striding over its cluster's triangles, and that loop cost the cull pass up to 0.11 ms at 7,900 survivors — three quarters of what the draw saved at 1440p. A separate pass with a workgroup per survivor writes the same indices in consecutive lanes for a fraction of that, and its cost is inside the "after" raster figures.

### After, TITAN Xp

Occlusion off. "Vertices" is what the draw asks for: 372 per cluster for the capacity draw, three per triangle (the index count) for the indexed draw — the post-transform cache runs fewer vertex invocations than that. "Raster" is the expansion pass plus the draw after, the draw alone before; the software rasterizer's raster pass is from the same build and cut, for the comparison the first run made.

| scene | resolution | clusters | vertices, before → after | cull, before → after | raster, before → after | | software raster | sum of passes, before → after |
|---|---|---|---|---|---|---|---|---|
| helmet grid, LOD 1 | 1920×1080 | 1,766 | 656,952 → 287,175 | 0.0368 → 0.0390 | 0.0591 → **0.0316** | −47% | 0.0967 | 0.3245 → 0.2961 |
| helmet grid, LOD 1 | 2560×1440 | 2,478 | 921,816 → 410,406 | 0.0370 → 0.0394 | 0.0822 → **0.0430** | −48% | 0.1610 | 0.5250 → 0.4820 |
| helmet grid, LOD 1 | 3840×2160 | 3,556 | 1,322,832 → 615,450 | 0.0371 → 0.0399 | 0.1177 → **0.0654** | −44% | 0.3349 | 1.0850 → 1.0204 |
| helmet grid, LOD 0.25 | 1920×1080 | 5,994 | 2,229,768 → 1,169,982 | 0.0373 → 0.0406 | 0.1957 → **0.1078** | −45% | 0.1364 | 0.4666 → 0.3774 |
| helmet grid, LOD 0.25 | 2560×1440 | 7,894 | 2,936,568 → 1,621,815 | 0.0374 → 0.0413 | 0.2570 → **0.1453** | −43% | 0.2172 | 0.7065 → 0.5903 |
| helmet grid, LOD 0.25 | 3840×2160 | 11,693 | 4,349,796 → 2,546,205 | 0.0378 → 0.0426 | 0.3825 → **0.2235** | −42% | 0.4206 | 1.3663 → 1.1928 |
| heightfield, orbit 9, LOD 1 | 1920×1080 | 683 | 254,076 → 170,640 | 0.0234 → 0.0256 | 0.1424 → 0.1515 | +6% | 0.5486 | 0.6375 → 0.6499 |
| heightfield, orbit 9, LOD 1 | 2560×1440 | 843 | 313,596 → 211,275 | 0.0235 → 0.0258 | 0.2663 → 0.2881 | +8% | 0.8737 | 1.1185 → 1.1467 |
| heightfield, orbit 9, LOD 1 | 3840×2160 | 1,052 | 391,344 → 263,829 | 0.0233 → 0.0257 | 0.6513 → 0.7013 | +8% | 1.5277 | 2.5227 → 2.5651 |
| heightfield, orbit 24, LOD 1 | 1920×1080 | 325 | 120,900 → 81,282 | 0.0291 → 0.0305 | 0.0285 → 0.0342 | +20% | 0.1880 | 0.3149 → 0.3199 |
| heightfield, orbit 24, LOD 1 | 2560×1440 | 401 | 149,172 → 101,451 | 0.0292 → 0.0303 | 0.0520 → 0.0586 | +13% | 0.3367 | 0.5326 → 0.5370 |
| heightfield, orbit 24, LOD 1 | 3840×2160 | 561 | 208,692 → 140,577 | 0.0289 → 0.0302 | 0.1272 → 0.1397 | +10% | 0.5461 | 1.1674 → 1.1715 |
| heightfield, orbit 60, LOD 0.5 | 1920×1080 | 195 | 72,540 → 48,774 | 0.0300 → 0.0321 | 0.0099 → 0.0106 | +7% | 0.0362 | 0.2339 → 0.2340 |
| heightfield, orbit 60, LOD 0.5 | 2560×1440 | 195 | 72,540 → 48,774 | 0.0300 → 0.0321 | 0.0105 → 0.0130 | +24% | 0.0578 | 0.3890 → 0.3870 |
| heightfield, orbit 60, LOD 0.5 | 3840×2160 | 388 | 144,336 → 97,668 | 0.0290 → 0.0304 | 0.0194 → 0.0233 | +20% | 0.0927 | 0.8546 → 0.8413 |

**The crossover is gone.** The first run's two losing rows — the helmet grid at LOD 0.25, where the software rasterizer's raster pass was 0.139 ms against the vertex path's 0.199 at 1080p and 0.221 against 0.261 at 1440p — now read **0.108 against 0.136 and 0.145 against 0.217**: the vertex path is 1.27× and 1.49× cheaper, and 1.9× at 4K, where it had led by only 1.1×. At LOD 1 it leads by 3.1–5.1×, and it keeps every heightfield row, by 2.2× to 5.8×. The frame the helmet grid costs falls with it, by 6–19% of the sum of passes; `cull_vertex_main` adds 1.1–4.8 µs to the cull pass for the allocation.

**The heightfield pays 0.001–0.05 ms for it.** Its raster pass is 6–24% slower indexed, from 0.7 µs at the smallest cut to 50 µs at 4K on the frame-filling view — 0.3–2.0% of its sum of passes, and nothing it could see at the frame (+2% at orbit 9, within the resolve's noise elsewhere). What it is paying for is in the diagnostic round below.

### Where the indexed draw's time goes (TITAN Xp)

A diagnostic build of the same tree, never committed, timed the expansion pass in a zone of its own and offered two more fragment stages for the indexed draw: one that does not read `SV_PrimitiveID` (it writes a wrong triangle, at the same cost otherwise) and one that writes nothing. Same gate and method; 78 runs, all quiet at their start, other processes at most 9.4% of the CPU, repeats within 0.6%. ms, 1920×1080 unless noted; the capacity draw's figures marked † are the sweep's above, the rest this round's.

| scene | capacity draw | capacity, fragment writes nothing | expansion | indexed draw | indexed, no `SV_PrimitiveID` | indexed, fragment writes nothing |
|---|---|---|---|---|---|---|
| helmet grid, LOD 1 | 0.0596 | 0.0589 | 0.0091 | 0.0228 | 0.0199 | 0.0192 |
| helmet grid, LOD 0.25 | 0.1951 | 0.1950 | 0.0276 | 0.0798 | 0.0708 | 0.0703 |
| heightfield, orbit 9 | 0.1424 † | 0.0278 | 0.0070 | 0.1451 | 0.1468 | 0.0272 |
| heightfield, orbit 9, 2560×1440 | 0.2663 † | 0.0385 | 0.0079 | 0.2796 | 0.2771 | 0.0380 |
| heightfield, orbit 24 | 0.0285 † | 0.0136 | 0.0047 | 0.0296 | 0.0290 | 0.0114 |
| heightfield, orbit 24, 2560×1440 | 0.0520 † | 0.0170 | 0.0051 | 0.0523 | 0.0536 | 0.0155 |

- **On the helmet grid the draw itself fell by two thirds** (0.195 → 0.070 ms with nothing written, at LOD 0.25), which is the vertex reuse. Two costs came back with it: **the expansion**, 9–28 µs, a quarter of the new raster pass — about 2.4 times what writing 4.7 MB of indices and reading 1.6 MB of triangles costs at the card's 547 GB/s — and **`SV_PrimitiveID`**, 3–9 µs, 11–13% of the draw, although the grid covers 1.5% of the pixels: on Pascal it is a per-primitive cost, not a per-fragment one.
- **On the heightfield there was little to save.** With nothing written, the two draws are within 1–16% of each other, and the pass is mostly the fragment stage's atomics anyway (80% of it at orbit 9, half at orbit 24). The indexed draw's loss there is the expansion (5–8 µs) plus a fragment side 3–6% slower on the frame-filling view (0.118 against 0.115 ms at 1080p, 0.242 against 0.228 at 1440p) that `SV_PrimitiveID` does not explain — removing it changes nothing there — and that this round could not attribute. The same pixels are written with the same values in the same triangle order; what differs is how the hardware batches an indexed draw's primitives, which nothing here can observe.
- The capacity draw's LOD 1 row, re-measured quiet here, reads 0.0596 ms against the sweep's 0.0591.

### After, RTX 5090

Occlusion off. "Vertices" is what the draw asks for: 372 per cluster for the capacity draw, three per triangle (the index count) for the indexed draw — the post-transform cache runs fewer vertex invocations than that. "Raster" is the expansion pass plus the draw after, the draw alone before. The mesh path is the reference; the 5090 draws it by default and takes the vertex path only when asked.

| scene | resolution | clusters | vertices, before → after | cull, before → after | raster, before → after | | mesh raster | sum of passes, before → after |
|---|---|---|---|---|---|---|---|---|
| helmet grid, LOD 1 | 1920×1080 | 1,766 | 656,952 → 287,175 | 0.0103 → 0.0125 | 0.0134 → 0.0149 | +11% | 0.0068 | 0.0446 → 0.0479 |
| helmet grid, LOD 1 | 2560×1440 | 2,478 | 921,816 → 410,406 | 0.0101 → 0.0128 | 0.0181 → 0.0193 | +7% | 0.0088 | 0.0562 → 0.0600 |
| helmet grid, LOD 1 | 3840×2160 | 3,556 | 1,322,832 → 615,450 | 0.0116 → 0.0136 | 0.0269 → 0.0292 | +9% | 0.0147 | 0.1022 → 0.1078 |
| helmet grid, LOD 0.25 | 1920×1080 | 5,994 | 2,229,768 → 1,169,982 | 0.0101 → 0.0149 | 0.0379 → 0.0448 | +18% | 0.0152 | 0.0692 → 0.0802 |
| helmet grid, LOD 0.25 | 2560×1440 | 7,894 | 2,936,568 → 1,621,815 | 0.0101 → 0.0151 | 0.0500 → 0.0608 | +22% | 0.0195 | 0.0885 → 0.1040 |
| helmet grid, LOD 0.25 | 3840×2160 | 11,693 | 4,349,796 → 2,546,205 | 0.0116 → 0.0174 | 0.0773 → 0.0946 | +22% | 0.0287 | 0.1542 → 0.1837 |
| heightfield, orbit 9, LOD 1 | 1920×1080 | 722 | 268,584 → 179,706 | 0.0101 → 0.0110 | 0.0199 → 0.0196 | −2% | 0.0186 | 0.0780 → 0.0779 |
| heightfield, orbit 9, LOD 1 | 2560×1440 | 856 | 318,432 → 211,242 | 0.0101 → 0.0110 | 0.0293 → 0.0286 | −2% | 0.0277 | 0.1163 → 0.1162 |
| heightfield, orbit 9, LOD 1 | 3840×2160 | 1,083 | 402,876 → 268,050 | 0.0101 → 0.0111 | 0.0555 → 0.0536 | −3% | 0.0539 | 0.2242 → 0.2232 |
| heightfield, orbit 24, LOD 1 | 1920×1080 | 315 | 117,180 → 77,913 | 0.0101 → 0.0111 | 0.0074 → 0.0089 | +20% | 0.0075 | 0.0431 → 0.0451 |
| heightfield, orbit 24, LOD 1 | 2560×1440 | 394 | 146,568 → 98,178 | 0.0101 → 0.0110 | 0.0102 → 0.0111 | +9% | 0.0099 | 0.0586 → 0.0602 |
| heightfield, orbit 24, LOD 1 | 3840×2160 | 558 | 207,576 → 139,752 | 0.0101 → 0.0110 | 0.0176 → 0.0180 | +2% | 0.0168 | 0.1012 → 0.1020 |
| heightfield, orbit 60, LOD 0.5 | 1920×1080 | 188 | 69,936 → 47,070 | 0.0101 → 0.0110 | 0.0038 → 0.0062 | +63% | 0.0043 | 0.0315 → 0.0341 |
| heightfield, orbit 60, LOD 0.5 | 2560×1440 | 196 | 72,912 → 48,792 | 0.0101 → 0.0110 | 0.0042 → 0.0064 | +52% | 0.0048 | 0.0375 → 0.0402 |
| heightfield, orbit 60, LOD 0.5 | 3840×2160 | 385 | 143,220 → 95,154 | 0.0101 → 0.0111 | 0.0064 → 0.0083 | +30% | 0.0064 | 0.0604 → 0.0632 |

On the 5090 the indexed draw is **7–22% slower on the helmet grid**, 2–3% faster on the frame-filling heightfield, and a fixed 2 µs slower on small cuts, where the expansion pass is most of the difference; `cull_vertex_main` adds 1 µs at a few hundred survivors and 6 µs at 12,000. Against the mesh path, the vertex path goes from 1.07× to 1.05× on the frame-filling heightfield and from 2.5× to 3.0× on the helmet grid at LOD 0.25.

### What it decides

1. **The crossover on Pascal is gone, and with it the qualification the first run added to ADR-0024.** The vertex path is now the faster rasterizer in every configuration measured on the TITAN Xp, the helmet grid at LOD 0.25 included (1.27× at 1080p, 1.49× at 1440p, 1.88× at 4K), so "no crossover" holds on the one GPU without mesh shaders the project has, as it does on the 5090. Recorded as a second dated note under ADR-0024 in the [ADR index](../adr/README.md) and a status note in [04 §4.3](../plan/04-renderer.md#43-geometry).
2. **The vertex-plus-software split loses the case that motivated it.** It was a live question because the software pass beat the vertex path on small, partly filled clusters; it no longer does anywhere measured, so the split stays unbuilt, and its follow-up above is demoted to "only if a scene shows a crossover again".
3. **The indexed draw is the baseline tier's culled draw wherever the device has `geometryShader` and `fullDrawIndexUint32`.** It trades up to 0.17 ms on the helmet grid for up to 0.05 ms on the frame-filling heightfield; the renderer takes that trade by the feature alone rather than per scene, because the cost it avoids grows with the number of partly filled clusters, which is what real content has, and the cost it adds is at most 2.5% of a frame measured.
4. **The RTX 5090 disagrees, and it does not decide.** Its vertex path is 7–22% slower indexed on the helmet grid, but it never takes the vertex path unless asked. A baseline-tier GPU that behaves like the 5090 rather than like Pascal would need the choice made per device; none has been measured.

### Follow-ups

- **Measure a baseline-tier GPU that is not NVIDIA Pascal**: the Maxwell TITAN X (plan 04's target, and the likeliest to behave like Pascal) and any AMD or Intel device without mesh shaders. If one prefers the capacity draw, the choice becomes per device rather than per feature; `DeviceOptions::overrides` removing `geometryShader` already forces the capacity draw for such a comparison in-process, but `engine-host` has no way to ask for it, and a benchmark request that can would be the first thing to add.
- **The expansion runs at about 2.4 times its bandwidth floor** — 28 µs where writing and reading the bytes would take 12 at LOD 0.25 — and is a quarter of the new raster pass on the helmet grid. A thread per triangle rather than 64 lanes striding over a cluster, and one 12-byte store per triangle, are the first things to try.
- **The frame-filling heightfield's 3–6% on the fragment side** is not `SV_PrimitiveID` and not the triangle order; nothing measured here explains it.
- **Occlusion culling on** was not re-measured. Each hardware run draws indexed exactly as run 0 does, the second cull is the same entry point, and the Hi-Z and resolve are untouched, so nothing here predicts a change beyond the second expansion pass's fixed cost.
- **The index budget** is a constant (2^20 triangles, 12 MiB, per view and hardware run); the largest cut measured, the helmet grid at LOD 0.25 and 4K, uses 0.85 M of it, and a cut past it draws the rest through the capacity draw with a warning. If content routinely approaches it, it should be sized from the resolution and the LOD threshold rather than raised.
