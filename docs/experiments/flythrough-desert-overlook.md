# Flythrough: desert overlook — a frame that streams, culls and occludes across a scene

- **Question ([09 §9.4](../plan/09-testing-profiling.md#94-benchmark-scene-corpus) direction note of 2026-09-23):** every renderer number before this one came from copies of one asset seen from one camera. What does a frame cost along a path through a scene with terrain, landmarks from 100 m to 2.6 km, instanced foliage and props — at the owner's four resolutions — and, now that there are **real occluders**, does two-pass occlusion culling pay for itself? [E1 on Pascal](e1-pascal-rerun.md) found it a net cost on scenes with nothing to occlude and asked for exactly this scene before anyone decides its default.
- **Date:** 2026-09-24. **Machines:** the development desktop — Intel i9-10980XE (18 cores / 36 threads), 64 GB, Windows 11 Pro, **RTX 5090** (32 GB, driver 610.88, Vulkan 1.4.341), `msvc-release`; and the headless server — Intel Xeon E5-2670 (Sandy Bridge-EP, 8 cores / 16 threads, no AVX2), 31 GB, Gentoo, **TITAN Xp** (Pascal, 12 GB, driver 580.178.04, Vulkan 1.4.312), `linux-server` (GCC 14.3.1, x86-64-v2) through `tools/remote-build.ps1`. **Build:** main at `794824d` ("shadows: clusters the cone test drops still cast") plus this change; the vertex path's per-cluster draw-argument change had not landed when these numbers were taken.
- **Machine state, RTX 5090:** every run under the machine-wide GPU lock (`agent-flythrough`), each started with `--wait-quiet 600`; at the start of every one of the 36 measured runs other processes used under 10% of the CPU (median 4.0%) and the GPU was at most 12% busy. **17 of 36 runs were quiet at both ends and 19 printed the WARNING** — on the sample taken *after* the run, which is this process's own last frames draining (up to 98% GPU) and, in two runs, another agent's build starting (others at 21.2% and 77.5% of the CPU on the end sample). The numbers are GPU timestamps, so the flagged runs are upper bounds by the rule and indistinguishable from their neighbours in practice: the three repeats of a frame agree, and a per-frame median over them is what every table reports. About 8 GB of the card belonged to the desktop and resident processes throughout.
- **Machine state, TITAN Xp: quiet.** Each run started only once the server's 1-minute load average was under 1.0 (0.88–0.99 at the start, after 30–70 s of waiting) and `nvidia-smi` showed the GPU at 0%; the harness sampled all four runs quiet at both ends, other processes at 1.7–7.5% of the CPU and the GPU idle, and printed no WARNING. No clock locking: the card was at 1,809–1,847 MHz, 62–74 °C and 84–88 W as each run ended. The renderer held 1.26 GB of the card's 12 GB.
- **Decision:** occlusion culling's default and the Hi-Z are the open questions this answers; the answer is a status note in [04 §4.3](../plan/04-renderer.md#43-geometry) and a direction in [09 §9.4](../plan/09-testing-profiling.md#94-benchmark-scene-corpus). Nothing in the renderer's defaults changed in this change.

## Setup

**The scene** is [content/test-scenes/desert-overlook](../../content/test-scenes/desert-overlook/README.md): a 5.12 km procedural dune field (2049 × 2049 vertices, 8.39 M triangles, 187,660 clusters, cached in the derived-data root), two rock ridges and an oasis basin; five landmarks — skyscraper 270 m, office tower 150 m, cathedral 95 m, mosque 55 m, airplane wreck 48 m — 100 m to 2.6 km from the path; 64 palms scattered around the oasis; 25 props along the approach and at the oasis. 90 instances; 239,833 (instance, cluster) pairs with the Khronos substitutes and 876,537 with the owner's landmarks. **Two asset sets on one set of placements:** the committed **substitutes** are seven Khronos samples fitted to the slots' sizes (Lantern, Corset, FlightHelmet, Suzanne, BoomBox, SciFiHelmet; 3.9 k–95 k triangles), and the **overlay** replaces the six landmark slots with the owner's Tripo models of 2026-09-23 by content hash (1.84–2.00 M triangles and 47–51 k clusters each, and a 243 k-triangle palm) from a local manifest that is never committed. Every run's summary names the scene file, the path file and every mesh by hash; the identities measured were `fbe4cf5cd623eb82` (substitutes) and `42831381368dd2a5` (overlay).

**The path** is 40 s at 60 fps, 2,401 frames: low through the dunes with every landmark behind ridge A; up a saddle whose face fills the frame; over the crest, where the landmarks come into view; up 60 m for an overlook that pans from the skyscraper to the office tower; down into the oasis as ridge B takes the far three back out of view; low over the palms to the wreck. The [scene's README](../../content/test-scenes/desert-overlook/README.md) names the frames at which each landmark is hidden, partly hidden and in view, measured with `--census-pixels` against a run with occlusion culling off.

**The harness** is new in this change ([renderer](../subsystems/renderer.md#scenes-camera-paths-and-flythroughs), [apps](../subsystems/apps.md#flythroughs)): `engine-view --benchmark` flies the path offscreen, three repeats, each after 240 frames and at least 2 s at the first camera — a count alone does not warm a card that draws the frame in 0.1 ms — with frames kept in flight and every frame's GPU milliseconds per pass read from `gfx::GpuTimer` when its slot comes around. A frame's figure is the **median of its three repeats**; the tables give the median, p95 and p99 of those over the 2,401 frames. `--census` then flies the path again, untimed, reading every frame's visible list back for pairs by DAG level and by mesh. 11520×2160 is drawn as the owner's three-monitor surround (`--views surround3`, flat). **Shadows are `auto` by default in both hosts** — ray-traced on the RTX 5090, off on the TITAN Xp, which has no ray queries — and every occlusion row passes `--shadows off`, because a frame that traces shadows runs one visible list and turns occlusion culling off; the ray-traced shadows are measured separately, with `--shadows rt`, and their rows count the extra casters the shadow chain keeps (`shadow_casters`, the clusters the cone test drops from the picture that can still face a light).

```powershell
tools/fetch-samples.ps1
tools/gpu-lock.ps1 run -Purpose "flythrough" -Exec "pwsh tools/flythrough.ps1 -Set substitute -Census"
tools/gpu-lock.ps1 run -Purpose "flythrough" -Exec "pwsh tools/flythrough.ps1 -Set overlay -Census"
# ray-traced shadows, raster auto, a 5% page budget, and the invariance check:
pwsh tools/flythrough.ps1 -Set overlay -Shadows rt -Occlusion on -Resolutions 1920x1080,3840x2160,11520x2160
pwsh tools/flythrough.ps1 -Set overlay -Raster auto -Occlusion on
pwsh tools/flythrough.ps1 -Set overlay -Occlusion on,off -Resolutions 3840x2160 -PageBudgetPct 5 -Census
engine-view --scene <scene.json> --camera-path <camera-path.json> --verify-occlusion --width 1920 --height 1080 --shadows off
```

The TITAN Xp runs were the same engine-view flags from a shell loop on the server, each started only once the 1-minute load average was under 1.0 and the GPU under 5% busy.

## Results

### RTX 5090: the frame along the path, shadows off

Milliseconds of GPU work per frame (the sum of the timed passes), median / p95 / p99 over the path's frames; the raster pass is the mesh path's (`hw`); visible pairs median / p95.

| set | resolution | occlusion | frame | cull | raster | Hi-Z | resolve | visible pairs |
|---|---|---|---|---|---|---|---|---|
| substitute | 1920×1080 | on | 0.118 / 0.146 / 0.151 | 0.028 | 0.023 | 0.021 | 0.046 | 1,522 / 3,511 |
| substitute | 1920×1080 | off | **0.079** / 0.106 / 0.113 | 0.010 | 0.025 | — | 0.044 | 2,597 / 4,169 |
| substitute | 2560×1440 | on | 0.167 / 0.210 / 0.217 | 0.028 | 0.034 | 0.029 | 0.075 | 1,749 / 4,162 |
| substitute | 2560×1440 | off | **0.119** / 0.163 / 0.173 | 0.010 | 0.037 | — | 0.072 | 3,097 / 5,103 |
| substitute | 3840×2160 | on | 0.345 / 0.425 / 0.443 | 0.036 | 0.064 | 0.088 | 0.154 | 2,243 / 5,404 |
| substitute | 3840×2160 | off | **0.238** / 0.329 / 0.348 | 0.014 | 0.070 | — | 0.155 | 4,085 / 6,788 |
| substitute | 11520×2160 | on | 1.258 / 1.495 / 1.524 | 0.093 | 0.286 | 0.409 | 0.472 | 3,540 / 7,577 |
| substitute | 11520×2160 | off | **0.829** / 1.094 / 1.118 | 0.038 | 0.295 | — | 0.487 | 5,532 / 9,269 |
| overlay | 1920×1080 | on | 0.134 / 0.199 / 0.218 | 0.038 | 0.029 | 0.021 | 0.047 | 3,798 / 18,453 |
| overlay | 1920×1080 | off | **0.097** / 0.153 / 0.173 | 0.020 | 0.033 | — | 0.045 | 9,561 / 19,880 |
| overlay | 2560×1440 | on | 0.195 / 0.276 / 0.303 | 0.045 | 0.041 | 0.029 | 0.076 | 4,323 / 22,083 |
| overlay | 2560×1440 | off | **0.147** / 0.226 / 0.255 | 0.022 | 0.049 | — | 0.074 | 13,841 / 23,635 |
| overlay | 3840×2160 | on | 0.375 / 0.509 / 0.554 | 0.059 | 0.072 | 0.085 | 0.157 | 5,525 / 27,543 |
| overlay | 3840×2160 | off | **0.281** / 0.412 / 0.457 | 0.035 | 0.087 | — | 0.160 | 20,778 / 29,564 |
| overlay | 11520×2160 | on | 1.297 / 1.524 / 1.551 | 0.136 | 0.286 | 0.397 | 0.482 | 9,178 / 35,110 |
| overlay | 11520×2160 | off | **0.872** / 1.133 / 1.165 | 0.071 | 0.302 | — | 0.497 | 24,797 / 37,647 |

The pass columns are medians; the p95 and p99 of each pass are in the runs' summaries. The wall time per frame, which adds the barriers, the gaps and the CPU, is 0.11–1.45 ms: 27–40% over the sum of the passes at 1080p and 12–16% at 11520×2160.

### The occlusion answer, phase by phase (RTX 5090, shadows off)

What occlusion culling removed and what it cost, by stretch of the path: the frame with it and without it, the pairs it culled, and where the difference went — the second cull pass (`cull+`), the Hi-Z build, and what it saved in the raster pass (`raster−`) and the resolve (`resolve−`). Medians over the stretch's frames.

| set, resolution | stretch | frame on / off | ratio | pairs on / off (culled) | cull+ | Hi-Z | raster− | resolve− |
|---|---|---|---|---|---|---|---|---|
| overlay, 4K | approach, landmarks behind ridge A (0–850) | 0.342 / 0.279 | 1.23 | 308 / 17,739 (98%) | 0.024 | 0.084 | 0.044 | 0.002 |
| overlay, 4K | the saddle's face (851–858) | 0.415 / 0.417 | **0.99** | 7 / 26,766 (100%) | 0.024 | 0.087 | 0.125 | −0.010 |
| overlay, 4K | crest and overlook (859–1440) | 0.374 / 0.274 | 1.36 | 20,736 / 22,788 (9%) | 0.024 | 0.084 | −0.004 | 0.007 |
| overlay, 4K | descent past ridge B (1441–1920) | 0.464 / 0.362 | 1.28 | 25,893 / 27,367 (5%) | 0.024 | 0.084 | −0.001 | 0.004 |
| overlay, 4K | oasis and wreck (1921–2400) | 0.375 / 0.271 | 1.38 | 5,141 / 6,874 (25%) | 0.024 | 0.085 | 0.004 | 0.002 |
| overlay, 11520×2160 | approach | 1.259 / 0.847 | 1.49 | 905 / 19,180 (95%) | 0.067 | 0.397 | 0.040 | 0.012 |
| overlay, 11520×2160 | the saddle's face | 1.552 / 1.201 | 1.29 | 29 / 27,869 (100%) | 0.061 | 0.397 | 0.126 | −0.014 |
| overlay, 11520×2160 | crest and overlook | 1.219 / 0.798 | 1.53 | 25,634 / 27,482 (7%) | 0.065 | 0.397 | 0.017 | 0.019 |
| overlay, 1080p | approach | 0.124 / 0.091 | 1.36 | 315 / 4,446 (93%) | 0.020 | 0.021 | 0.012 | −0.001 |
| overlay, 1080p | the saddle's face | 0.132 / 0.128 | 1.03 | 7 / 12,604 (100%) | 0.018 | 0.021 | 0.036 | −0.001 |
| substitute, 4K | approach | 0.318 / 0.228 | 1.39 | 308 / 3,655 (92%) | 0.024 | 0.089 | 0.026 | −0.003 |
| substitute, 4K | the saddle's face | 0.389 / 0.331 | 1.17 | 7 / 4,787 (100%) | 0.021 | 0.092 | 0.067 | −0.013 |

**Occlusion culling does its job and does not pay on this card.** With real occluders it removes 92–98% of the pairs behind ridge A and all of them at the saddle's face — 26,766 pairs of palms, terrain and landmarks down to 7 — and the frame is still slower at every resolution and on both sets, by 33–52% over the whole path. The one stretch where it breaks even is the saddle's face at 4K, where it culls everything and saves 0.125 ms of raster against 0.108 ms of Hi-Z and second cull.

### Ray-traced shadows (RTX 5090)

`--shadows rt`: every light's shadow is a ray query against acceleration structures built each frame from the frame's own visible list, which turns occlusion culling off.

| set | resolution | frame | rt chain | resolve | shadow casters (median) | device memory |
|---|---|---|---|---|---|---|
| substitute | 1920×1080 | 2.401 / 2.863 / 2.922 | 2.256 / 2.695 / 2.761 | 0.097 | 53 | 3.1 GB |
| substitute | 3840×2160 | 3.136 / 3.841 / 3.946 | 2.709 / 3.267 / 3.379 | 0.333 | 106 | 3.2 GB |
| substitute | 11520×2160 surround | 4.359 / 5.903 / 6.322 | 2.857 / 3.614 / 3.728 | 1.114 | 157 | 6.5 GB |
| overlay | 1920×1080 | 3.790 / 5.696 / 5.915 | 3.609 / 5.448 / 5.659 | 0.107 | 112 | 9.5 GB |
| overlay | 3840×2160 | 6.011 / 7.791 / 8.125 | 5.534 / 7.104 / 7.486 | 0.361 | 208 | 9.6 GB |
| overlay | 11520×2160 surround | 7.784 / 10.532 / 10.834 | 6.215 / 8.858 / 9.167 | 1.217 | 267 | **20.6 GB** |

*(2026-09-24, later the same day: the device memory column is what a chain allocated for every pair of the scene in every view held, and most of the rt chain column was 90 bottom-level builds one after another. Both changed; [below](#the-ray-tracing-chain-sized-by-the-frame) has the same runs before and after.)*

### `--raster auto` (RTX 5090)

The mesh path and the software rasterizer split at 32 px of projected cluster diameter. **`auto` turns occlusion culling off by design** — occlusion runs only on the paths with no software pass — so these rows are the occlusion-off rows with the split added: 0.101 / 0.144 / 0.266 / 0.963 ms (substitute) and 0.107 / 0.160 / 0.298 / 1.043 ms (overlay) at 1080p / 1440p / 4K / 11520×2160, **13–28% slower than `hw`** with occlusion off, the raster pass 1.4–1.8× the mesh path's. E1's result carries over to a scene: the software rasterizer loses on this card at every size the path draws.

### Streaming along the path (RTX 5090, overlay, 4K)

The overlay's page table is 541 MB. At a quarter of it (135 MB) the path's whole working set fits — 578 pages resident by the end, nothing evicted, 282 pages (31 MB) uploaded over the first flight and none in the repeats — and the frame costs what the unstreamed frame costs. At **5% (27 MB, about 237 pages resident)** the budget binds everywhere:

| 5% budget, per flight of 2,401 frames | occlusion on | occlusion off |
|---|---|---|
| uploads | 2,178–2,579 pages, 243–289 MB | 1,932–2,055 pages, 216–230 MB |
| evictions | 3,918–4,465 | 4,691–4,843 |
| visible pairs, median / p95 | 1,574 / 16,853 (unstreamed: 5,525 / 27,543) | 4,725 / 17,146 (unstreamed: 20,778 / 29,564) |
| frame, median / p95 / p99 | 0.369 / 0.460 / 0.506 | 0.265 / 0.362 / 0.409 |

About 5–7 MB/s of page traffic and 100–120 evictions a second hold the picture at a coarser cut, which the frame does not pay for — the copies are a transfer pass the timers do not bracket, and the coarser cut draws fewer pairs. **Occlusion culling does not shrink the working set**: at the quarter budget both runs uploaded exactly the same 282 pages in the same stretches (127 / 0 / 21 / 81 / 53), because a page is requested by the LOD cut before the occlusion test, so an occluded palm still asks for its pages. And a streamed flight is **not repeatable**: its pages land when their reads do, so 2,211 of 2,401 frames drew different pairs across the three repeats, against none unstreamed.

### The ray tracing chain sized by the frame

*Added 2026-09-24, later the same day* ([renderer](../subsystems/renderer.md#the-ray-tracing-chains-memory)). The 20.6 GB above was the follow-up to answer, and the first thing measured was what the chain actually builds along the path against what it allocated. Same machine, `msvc-release`, `--shadows rt --raster hw` (occlusion culling off, as shadows force), three repeats after 240 frames of warm-up, the path's 2,401 frames each. **What a frame builds** — its drawn clusters plus its shadow casters, per frame over all 7,203 frames of a run:

| set | resolution | built: median / p95 / max | frame to frame, largest rise | allocated for (`views × pairs`) |
|---|---|---|---|---|
| substitute | 1920×1080 | 2,697 / 4,226 / 4,671 | +1.7% | 239,833 |
| substitute | 11520×2160 surround | 5,691 / 9,441 / 10,061 | +1.1% | 719,499 |
| overlay | 1920×1080 | 9,640 / 20,026 / 21,879 | +1.7% | 876,537 |
| overlay | 11520×2160 surround | 25,141 / 37,957 / 43,291 | +1.4% | 2,629,611 |

The allocation was **40 to 72 times the largest frame of the path**, and the demand never moved by more than 1.7% from one frame to the next (p99 0.6–0.9%): a capacity kept a quarter above the demand has dozens of frames of warning before it runs out. The structures are now sized that way — a step above what recent frames built, grown ahead of the demand and shrunk only after a window of frames that peaked under half of it, with a budget as the ceiling and a stated rule for what a frame past it drops — and the bottom-level structures are one set built in one command instead of one per instance. Before and after, the same runs:

| set | resolution | device memory, before → after | chain's bytes: before → after (peak) | capacity at the end (peak) | rt chain, median / p95 (ms) | of it: CLAS · BLAS | frame, median (ms) |
|---|---|---|---|---|---|---|---|
| substitute | 1920×1080 | 3,078 → **1,670 MiB** | 1,531 → **79 MB** (472) | 4,096 (65,536) | 2.542 / 3.315 → **0.400 / 0.756** | 0.108 → 0.110 · 0.184 | 2.693 → 0.548 |
| substitute | 11520×2160 surround | 6,516 → **2,235 MiB** | 4,593 → **134 MB** (474) | 12,288 (65,536) | 3.032 / 4.043 → **0.451 / 0.697** | 0.196 → 0.196 · 0.141 | 4.612 → 2.019 |
| overlay | 1920×1080 | 9,490 → **4,261 MiB** | 5,595 → **192 MB** (533) | 12,288 (65,536) | 4.074 / 6.289 → **0.606 / 1.064** | 0.289 → 0.291 · 0.188 | 4.257 → 0.783 |
| overlay | 11520×2160 surround | **20,643 → 5,054 MiB** | 16,784 → **461 MB** (540) | 53,248 (65,536) | 7.075 / 9.993 → **0.967 / 1.484** | 0.647 → 0.633 · 0.186 | 8.823 → 2.559 |

The milliseconds are percentiles over all 7,203 frames of a run, from the re-take the next paragraph but one describes.

**Device memory fell by 1.4 to 15.2 GiB** — the landmarks' surround from 20.6 GB to 5.1 GB, a quarter of the card instead of two-thirds — and the chain's own bytes by 19 to 36 times at the end of a run; the peak is the first allocation of 65,536 clusters (400 MB), which the policy shrinks after its first window. **The chain also got six to seven times faster** (6.4 to 7.3 at the median), and not because of the memory: the cluster build (`clas`) is the same to within 2%. What went is the 90 bottom-level builds, each waiting for the last because they shared a scratch buffer; they are one build now, 0.14–0.19 ms, and the frame's median is 2.3 to 5.4 times shorter. The capacity moved 2 to 27 times a run — three flights and their warm-ups, the overlay surround growing about eight times a flight as the path climbs from 5,000 clusters to 43,000 and shrinking once at the end — and **not one frame of any run dropped a structure**: the demand never outran a growth. **The pictures did not change**: the twelve marker captures, with `--shadows rt`, of all four configurations and of `--raster rt` at 1080p on both sets are byte-identical before and after, 72 of 72.

**Machine state, and which numbers were taken twice.** Every run under the GPU lock (`agent-rt-memory`). The memory columns, the capacities and the demand table are from the first pass: device memory is this process's own figure from `VK_EXT_memory_budget` and the chain's bytes are its own allocations, neither depends on who else is on the card, and the re-take repeated every one to within 2 MiB. The milliseconds do depend on it, and **none of the first pass's are in the table**: every run of that pass printed the busy-machine WARNING, and the owner was using the GPU without the lock while its after runs started. Every timing cell is from a re-take the same afternoon, one lock hold per row — its before and after runs minutes apart — released for 60 s between rows so that a short job could get in, each run waiting for a quiet machine and refusing a busy one (`--require-quiet`). **The card was quiet for all of it** — 5–9% busy at every start, which is the desktop's compositor driving three monitors. **The CPU was not**: other processes held 6.8–10% of it at the six starts that passed and 9–24% at the ends, so seven of the eight runs carry the WARNING, five of them for the CPU alone, and the two 1080p after runs were refused twice (15–24%) and were then taken on the same quiet card without the refusal, starting at 22–26%. Two end samples read the GPU busier — 72% after the substitute surround's before run, 36% after the substitute 1080p's after run — and neither overlapped the frames: each of those runs' three flights agree to within 1.1% in every third of the path. The re-take moved no median by more than 3% from the first pass; the p95s moved by up to 7%.

### The streaming run that uploaded nothing

*Added 2026-09-24, later the same day* ([renderer](../subsystems/renderer.md#admission-never-waits-on-a-read-it-cannot-start)). The run in "What surprised me" — 60,034 requests over the path, no upload, 38 pinned pages resident, a cut at the roots — was a **circular wait**, not luck and not a lost read. The pool is filled in page order (parent before child), a load slot comes back only when its page is filled, and admission starts reads in priority order. With reads slower than frames the eight load slots fill with pages behind the walk's head; a page admitted after them with a lower index — a coarser page some cluster asked for a moment later — gets no slot, becomes the head, and can never start its read, while the loads that hold every slot can never be staged because they are behind it. The pool stops, admission stops with it, and requests pile up for the rest of the run. Its precondition is reads that take several frames, which is a **cold file cache**: the stuck run was the first after a build, reading the containers off disk, and the three reruns found them in memory. The fix is one rule — the page at the head of the walk always gets a load, taking one from the page furthest behind it when every slot is held — and a trace that shows the state frame by frame (`pending`, `loads_in_flight` and `pool_pages` in every `.jsonl` record).

**Reproduced, and gone.** The runs that establish it are the stuck run's own command — the landmarks overlay at 1920×1080, occlusion culling off, shadows off, a 5% page budget (27.1 of 541.6 MiB: 4,729 pages, 270 pool slots), pages read from the containers — and the same command with the substitute set (12.3 of 245.2 MiB: 2,143 pages, 125 slots), which the TITAN Xp can run as well, having no copy of the landmarks. Each is flown once (`--repeat 1`, 240 frames of warm-up) with the file cache made cold first: on the RTX 5090 each run reads a fresh copy of the containers written with unbuffered I/O (`robocopy /J`), so none of its pages is in memory; on the TITAN Xp every container's pages are dropped from the page cache (`dd iflag=nocache count=0`, which needs no privilege for a file one can read, and which `fincore` confirms leaves none resident). "Stalled" is a pool that stopped for good with all eight loads out; "before" is the streamer as it was.

| machine | set | build | runs | stalled | stalled runs: pool / manager / queue / loads out, frozen to the end of the path | the others: uploads over the path, visible pairs (median) |
|---|---|---|---|---|---|---|
| RTX 5090 | overlay | before | 10 | **7** | six from the warm-up: 4–28 / 15–37 / 9–23 / 8, no upload; one from frame 1,978: 225 / 234 / 60 / 8 | 2,581–2,610, 8,907–8,912 |
| RTX 5090 | overlay | after | 10 | **0** | — | 2,541–2,614, 8,757–9,033 |
| RTX 5090 | substitute | before | 10 | **4** | all from the warm-up: 33–46 / 45–55 / 14–33 / 8, no upload | 1,427–1,597, 2,118–2,130 |
| RTX 5090 | substitute | after | 10 | **0** | — | 1,376–1,556, 2,121–2,128 |
| TITAN Xp | substitute | before | 10 | **1** | from the warm-up: 37 / 47 / 12–28 / 8, no upload | 1,311–1,548, 2,155–2,164 |
| TITAN Xp | substitute | after | 10 | **0** | — | 1,371–1,527, 2,160–2,170 |

Every stalled run was in exactly the state the mechanism predicts and none other: the pool frozen, the manager holding 9 to 12 pages more that the pool never got, all eight loads out, and the queue not draining. Eleven of the twelve closed during the warm-up, within the first seconds at the first camera, which is when the most pages are asked for at once; one closed 33 s into the path, at frame 1,978, and froze a nearly full pool for the remaining 422 frames — so a run that starts well is not safe either. A stalled overlay run drew 177–2,568 pairs where a normal one drew 8,907–8,912, as the flythrough's stuck run drew 2,605. The stuck flythrough was the first run after its build and its three reruns were normal; a cold cache is what turns the order problem into a wait, and here the wait was likelier with the landmarks (seven in ten) than without (four). On the TITAN Xp eight of the ten before runs, the stalled one among them, shared the server with another agent's test suite (a load average near 14, and 67–87 s a run against 32–35 s for the other two and for every after run), so its one in ten is a count of the event and not a rate to compare with the other rows. After the change no run of either set on either machine froze for as much as 100 frames. Twenty clean runs on the RTX 5090 against seven and four in ten before put the chance that the stall merely went unobserved under one in ten million (0.3^10 × 0.6^10).

**The synthetic source makes it every run on the RTX 5090.** `admission never waits on a read it cannot start` (`streaming_tests.cpp`, [renderer](../subsystems/renderer.md#admission-never-waits-on-a-read-it-cannot-start)) serves the 129-grid heightfield's pages from a source whose loads land six frames after they begin, during a fly-in, and it is the same wait every time: the streamer before the rule has 12 of the 21 admitted pages in its pool after 1,200 frames, the last of them landed at frame 48, and all eight loads out; the streamer after converges in 89 frames having given up one load. On the TITAN Xp the same fly-in never fills the slots behind the head (the streamer after gives up no load and converges in 83 frames), so the case is a regression test for the rule on the RTX 5090 and a convergence test elsewhere.

### TITAN Xp: the baseline tier, substitute set

The server has no mesh shaders, so `--raster hw` resolves to the vertex path (`cluster_vertex.slang`, the cull pass's indirect draw), and no ray queries, so there are no shadows to turn on. The substitute set only: the owner's landmarks were not copied to the server.

| resolution | occlusion | frame | cull | raster (vertex) | Hi-Z | resolve | visible pairs |
|---|---|---|---|---|---|---|---|
| 1920×1080 | on | 0.779 / 0.996 / 1.051 | 0.085 | 0.129 | 0.144 | 0.418 | 1,520 / 3,559 |
| 1920×1080 | off | **0.648** / 0.887 / 0.924 | 0.057 | 0.160 | — | 0.436 | 2,495 / 4,127 |
| 2560×1440 | on | 1.289 / 1.676 / 1.753 | 0.086 | 0.236 | 0.239 | 0.720 | 1,780 / 4,189 |
| 2560×1440 | off | **1.099** / 1.510 / 1.596 | 0.057 | 0.274 | — | 0.767 | 3,045 / 5,046 |

The wall time per frame is 0.87–1.42 ms, 12–38% over the passes. By stretch:

| resolution | stretch | frame on / off | ratio | pairs on / off (culled) | cull+ | Hi-Z | raster− | resolve− |
|---|---|---|---|---|---|---|---|---|
| 1080p | approach (0–850) | 0.736 / 0.639 | 1.15 | 311 / 2,429 (87%) | 0.028 | 0.144 | 0.060 | 0.018 |
| 1080p | the saddle's face (851–858) | 0.948 / 0.957 | **0.99** | 7 / 2,894 (100%) | 0.026 | 0.143 | 0.187 | −0.018 |
| 1080p | crest and overlook (859–1440) | 0.750 / 0.629 | 1.19 | 2,609 / 2,766 (6%) | 0.027 | 0.144 | 0.022 | 0.030 |
| 1080p | descent past ridge B (1441–1920) | 0.923 / 0.788 | 1.17 | 3,142 / 3,610 (13%) | 0.028 | 0.144 | 0.020 | 0.020 |
| 1080p | oasis and wreck (1921–2400) | 0.805 / 0.646 | 1.25 | 1,379 / 1,926 (28%) | 0.029 | 0.144 | 0.008 | 0.017 |
| 1440p | approach | 1.223 / 1.092 | 1.12 | 315 / 2,812 (89%) | 0.029 | 0.239 | 0.094 | 0.044 |
| 1440p | the saddle's face | 1.609 / 1.672 | **0.96** | 7 / 3,414 (100%) | 0.027 | 0.238 | 0.337 | −0.022 |
| 1440p | crest and overlook | 1.207 / 1.048 | 1.15 | 3,096 / 3,294 (6%) | 0.028 | 0.239 | 0.045 | 0.063 |
| 1440p | descent past ridge B | 1.506 / 1.324 | 1.14 | 3,686 / 4,436 (17%) | 0.029 | 0.239 | 0.041 | 0.047 |
| 1440p | oasis and wreck | 1.352 / 1.112 | 1.22 | 1,698 / 2,237 (24%) | 0.030 | 0.239 | 0.010 | 0.044 |

**On the baseline tier occlusion culling is closer, and still a net cost**: 20% over the path at 1080p and 17% at 1440p, against 33–52% on the RTX 5090, and it pays at the saddle's face at both resolutions. Two things move it. The vertex path rasterizes a hidden pair at **28–99 ns** — 0.060 ms for 2,118 pairs behind ridge A at 1080p, 0.337 ms for 3,407 at the saddle's face at 1440p, the spread being how much of the screen the hidden pair covers — ten to twenty times the RTX 5090's mesh path, so each culled pair is worth more; and the resolve's empty-tile skip, which rides on the Hi-Z (E1 on Pascal), is worth 0.02–0.06 ms of the resolve wherever there is sky. Against them stand a Hi-Z of 0.144 ms at 1080p and 0.239 ms at 1440p — seven to eight times the RTX 5090's — and 0.028 ms of second cull. The break-even is then roughly 2,300–5,900 culled pairs a frame, and the substitute set never has more than about 5,000 pairs in view. **The overlay would**: its palms put 18,600–27,000 pairs in view at the overlook and in the grove, and on the RTX 5090 occlusion culled 4,100–12,600 of the overlay's pairs a frame behind ridge A and at the saddle at 1080p — on the Pascal card's per-pair cost, a stretch where it would plausibly pay. The baseline tier's default is decided by that run, not by this one. *(2026-09-24, later the same day: these per-pair costs are the vertex path's capacity draw's. The vertex path now draws a culled cut indexed ([E1 on Pascal, "After"](e1-pascal-rerun.md#after-the-vertex-path-draws-the-cuts-own-triangles)), which takes 42–48% off the raster pass of a grid of partly filled clusters and costs a few percent more on a frame-filling terrain, so a hidden pair is worth less than measured here and the break-even moves further out; the overlay run should be taken with the indexed draw.)*

The TITAN Xp's cut is the RTX 5090's to within the terrain's clustering: the GCC v2 build clusters the terrain into 187,697 clusters against MSVC v3's 187,660 (239,870 pairs against 239,833), and the median visible pairs agree to within 4% (1,520 against 1,522 at 1080p with occlusion on, 2,495 against 2,597 with it off). The passes compare across the two cards; the cuts are close, not identical.

### Occlusion culling changes no visible surface — and the invariant needs restating

`--verify-occlusion` drew all 2,401 frames at 1920×1080 twice, with and without occlusion culling, in lockstep, and compared the (instance, cluster) under every pixel and every colour byte.

| card, set | frames differing | pixels, surface differs | of those: a nearer surface lost | depths tied exactly | colour bytes differing |
|---|---|---|---|---|---|
| RTX 5090 (mesh path), substitute | 64 of 2,401 | 69 | **0** | 69 | 195 |
| RTX 5090 (mesh path), overlay | 1,118 of 2,401 | 3,673 | **0** | 3,673 | 8,838 |
| TITAN Xp (vertex path), substitute | 60 of 2,401 | 62 | **0** | 62 | 180 |

**Not one pixel lost a nearer surface to occlusion culling.** Every differing pixel is two surfaces at exactly the same depth: palm fronds in 3,640 of the overlay's 3,673 (the Tripo palm's leaves, and the SciFiHelmet's parts in the substitute, are coincident back-to-back faces), a few terrain and landmark pixels in the rest; on the TITAN Xp's vertex path, 57 of 62 are the SciFiHelmet palms, the same story on the other rasterizer. The visibility buffer's 64-bit atomic max breaks a depth tie on the payload, `visible_index << 8 | triangle`, and the visible list's order is the cull pass's append order — culling changes it, and nothing makes it repeatable anyway: two identical checks of the same build disagreed by 9 and 13 tied pixels. The renderer's own occlusion test compares a wall of cubes, which has no coincident faces, and could not have seen this.

**Update, 2026-09-24 (later the same day): the ties are gone, and the invariant is the strict one again.** The visibility id became the scene's pair, so a depth tie goes to the larger (pair, triangle) whichever run, occlusion pass or rasterizer drew it ([gfx](../subsystems/gfx.md), "The tie rule"; what it cost is [visible order](visible-order.md)). The same check with that build, the same flags and the same scene identities (`fbe4cf5cd623eb82`, `42831381368dd2a5`):

| card, set | frames differing | pixels, surface differs | of those: a nearer surface lost | depths tied exactly | colour bytes differing |
|---|---|---|---|---|---|
| RTX 5090 (mesh path), substitute | 0 of 2,401 | 0 | 0 | 0 | 0 |
| RTX 5090 (mesh path), overlay | 0 of 2,401 | 0 | 0 | 0 | 0 |
| RTX 5090 (vertex path, indexed draw), substitute | 0 of 2,401 | 0 | 0 | 0 | 0 |
| TITAN Xp (vertex path, indexed draw), substitute | 0 of 2,401 | 0 | 0 | 0 | 0 |

Occlusion culling on and off now give the same id channel and the same colour on every frame of the path, so the invariant is back to what the renderer's occlusion test has always asserted — **identical visibility buffers with and without occlusion culling** — over a real path and not only over a wall of cubes. The depth classification stays in the check, so that a difference, if one ever comes back, says at once whether it is a lost surface or a tie.

## What surprised me

- **Occlusion culling cannot pay where it culls most.** Behind ridge A it removes 98% of the pairs and saves 0.044 ms of raster at 4K against 0.108 ms of Hi-Z and second cull. In a visibility-buffer renderer the resolve is per pixel whatever was culled, so the only thing culling saves is rasterization — and the mesh path rasterizes this scene's occluded pairs at **2.5–4.7 ns a pair** at 4K (0.044 ms for 17,431 pairs behind ridge A, 0.125 ms for 26,759 at the saddle), while the Hi-Z is a fixed cost of the resolution — 0.021 ms at 1080p, 0.085 at 4K, 0.40 at 11520×2160 over three views — and the second cull a fixed 0.02–0.07 ms over every pair of the scene. At 4K occlusion breaks even at roughly 23,000–43,000 culled pairs a frame and at 11520×2160 at roughly 100,000–210,000; the densest frame of this path culls 27,000. **On the TITAN Xp the Hi-Z costs as much as the whole raster pass** — 0.239 ms at 1440p against 0.236–0.274 ms for the vertex path — and the frame is dominated by the resolve (0.72–0.77 ms, 60–70% of it), which occlusion culling does not touch except through the tile skip.
- **The frame is cheap and flat.** 0.08–0.10 ms at 1080p and 0.83–0.87 ms at 11520×2160 without shadows, whether the landmarks have 5 k or 2 M triangles: the overlay adds 636,704 pairs to the scene and 5–23% to the frame's median, the most at 1080p where there is least else. What the overlay changes is the **tail** — p95 0.41 against 0.33 ms at 4K — and the tail is the palms. 64 palms carry 18,600–27,000 of the 20,000–30,000 visible pairs at the overlook and in the grove, at DAG levels 3–4: the palm is the E10 prop whose fragmented atlas stops it coarsening (45.8% seam vertices, [E10](e10-generated-props.md)), so a palm 300 m away still draws ~340 clusters. The 2-million-triangle landmarks at 0.6–2.6 km draw 285–1,315 pairs each at the overlook.
- **Ray-traced shadows are the frame.** 2.3–6.2 ms of acceleration-structure chain against 0.1–1.2 ms for everything else; the chain grows with the instance count (90 bottom-level builds a frame) and the cut, not with the resolution. And **its memory is the scene's, not the cut's**: the CLAS set is sized `views × pairs`, 20.6 GB for the overlay as a surround — two-thirds of the card for a frame that builds about 25,000 clusters. *(Both answered the same day: the chain is sized by what the frames build and its bottom-level structures are one build, [below](#the-ray-tracing-chain-sized-by-the-frame).)*
- **The first frames of a warm-up had no occlusion history, and nobody had noticed.** The history ping-ponged on the caller's frame number, so a warm-up that holds frame 0 made pass 1 of the path's first frame read a buffer written two frames earlier or never. Fixed in this change (the parity is now the renderer's own frame count); pictures unchanged; repeats now agree frame for frame.
- **The first streaming run at 1080p with occlusion off never admitted a page**: 60,034 requests over the path, 0 uploads, 38 pinned pages resident from start to end, and a cut at the roots (2,605 pairs against 9,561 unstreamed). Three reruns of the same command admitted pages normally. A load that never completes, or a dropped read that leaves the manager believing the pool is behind, would block admission for good in exactly this way ("nothing new is admitted while the pool is behind the manager", [renderer](../subsystems/renderer.md#geometry-streaming-the-gpu-half-of-pages-and-residency)); it is reported for the streaming owner rather than chased here. *(Answered the same day: a circular wait between the pool's page-order staging and admission's priority order, closed when reads are slower than frames — a cold file cache; [below](#the-streaming-run-that-uploaded-nothing).)*

## What it decides

**Occlusion culling should not be on by default on the RTX 5090 class**, and the reason is structural, not a tuning miss: on this scene it removes most of what is hidden and adds 33–52% to a frame that is almost all fixed cost. **On the baseline tier it is a smaller net cost (17–20%) and not yet a settled one**: the vertex path makes a culled pair worth ten to twenty times more, the substitute set has too few pairs to reach the break-even, and the overlay's palms would. Recorded as a status note in [04 §4.3](../plan/04-renderer.md#43-geometry): the default stays as it is until the overlay has been flown on the TITAN Xp and one of the cheaper Hi-Z variants measured, and the decision — plausibly per tier — is then a settings change with these rows as its evidence. **What would change the answer:** a Hi-Z that costs a fraction of today's (half-resolution mip 0, or reusing the previous frame's pyramid — each changes the cut and needs its own measurement, [04 §4.6](../plan/04-renderer.md#46-extreme-displays)); per-pair work downstream of the cut that grows (deformation, the acceleration-structure build — though occluded casters must still cast); or more pairs in view at a rasterizer's per-pair cost, which is what the overlay on the baseline tier would test. Decoupling the resolve's tile skip from the Hi-Z moves the answer the other way, since occlusion culling would stop being the only way to get it.

**The invariance test was restated**, in the harness and in [renderer](../subsystems/renderer.md#scenes-camera-paths-and-flythroughs): occlusion culling never removes a visible surface (`frames_culled_visible`, 0 over both sets and both rasterizers, 7,203 frames); which of two surfaces at exactly one depth a pixel showed was decided by the visible list's order and was not an invariant of anything. **Restored the same day** (the update above): with the pair as the id, a tie is a function of the scene, the path shows 0 differing pixels, and the invariant is again that occlusion culling does not change the picture.

**What it does not decide:** anything about lighting beyond direct sun, sky and two point lights (no GI, no denoiser, no post), about textures streaming (textures are uploaded whole), or about the Maxwell TITAN X of plan 04's baseline row.

## The frame against plan 04's budget

| configuration | plan 04 target | this path, shadows off (median / p99) | with ray-traced shadows |
|---|---|---|---|
| 11520×2160 surround, RTX 5090 | 60 fps target, 30 floor: 16.7 / 33.3 ms | 0.83–0.87 / 1.12–1.17 ms, 5% of the target | 4.4–7.8 / 6.3–10.8 ms, 26–47% |
| 3840×2160, RTX 4080/5080 class (measured on a 5090) | 60 fps: 16.7 ms | 0.24–0.28 / 0.35–0.46 ms | 3.1–6.0 / 3.9–8.1 ms |
| 2560×1440, TITAN Xp (Pascal, above plan 04's Maxwell floor), substitute set | 30 fps: 33.3 ms | 1.10 / 1.60 ms (occlusion off), 1.29 / 1.75 ms (on): 3–5% | — (no ray queries) |

What fills the rest of the budget does not exist yet — global illumination, a denoiser, post-processing and an upscaler — and the ray-traced shadow chain as it is today would take half of the surround's 60 fps budget with the owner's landmarks.

## Follow-ups

- **A stable depth tie-break** for the visibility buffer, so that coincident surfaces resolve the same way whatever the list order — a key the cull pass's append order does not own. For the owner of the visibility encoding.
- **Decouple the resolve's empty-tile skip from occlusion culling** (E1 on Pascal's follow-up), then measure occlusion culling on the Hi-Z variants; the flythrough is now the scene to decide it on.
- ~~**Size the cluster acceleration structures by the cut, not by `views × pairs`**: 20.6 GB for a frame that builds 25,000 clusters.~~ **Done the same day** ([The ray tracing chain sized by the frame](#the-ray-tracing-chain-sized-by-the-frame)).
- ~~**The stuck streaming run** above: 0 admissions over 2,401 frames, once in four 1080p/4K runs.~~ **Done the same day** ([The streaming run that uploaded nothing](#the-streaming-run-that-uploaded-nothing)).
- **A page whose read fails every time** still holds every page behind it in page order, as it must for the ancestor closure; a container that cannot be read should be refused when the source opens it, not found out one frame at a time. For the streaming owner.
- **A stall test that stalls on every device.** `admission never waits on a read it cannot start` reproduces the wait on the RTX 5090 and not on the TITAN Xp, because it reaches admission through a cut whose page order is the device's; one that feeds the manager requests in a chosen order would hold the rule everywhere.
- **The palm's LOD**: E10's repair for fragmented atlases decides whether 64 palms cost 20,000 pairs or 2,000.
- **Terrain as clusters is fine; its materials are coarse.** A cluster is one material, so sand and rock meet on cluster edges; a splat texture or a per-vertex blend is the terrain path of 04 §4.3, not this scene's business.
- **The E10 props.** The direction note asked for E10's generated props along the path; they are the owner's generated outputs too, so they belong in the overlay beside the landmarks rather than in the committed scene. The props here are Khronos samples at human scale in both sets, and adding the E10 set is an overlay manifest and six more `overlay` hashes on the prop slots.
- **The overlay on the TITAN Xp**, which decides the baseline tier's occlusion default (above). It means copying the owner's six landmarks and the palm to the server outside the repository — his call, since they are his paid-plan outputs — and fits the card: the renderer held 3.6 GB for the overlay at 1080p and 1440p on the RTX 5090 without shadows, against the TITAN Xp's 12 GB.
- **The Maxwell TITAN X**, when it is reachable, with the same command lines.

## Caveats

- One path through one scene, one camera speed. The occlusion answer is a statement about this renderer's costs, and the break-even pairs scale with them; a scene built to maximize occluded pairs (a city block) is the next test.
- The substitutes are placeholders. They are fitted to the landmarks' sizes and keep the path's occlusion, but they have 5–95 k triangles against 2 M, so the substitute rows measure the terrain, the palms and the harness more than the landmarks.
- The terrain is one mesh of 187,660 clusters with one uniform-scale instance, which the cull pass tests pair by pair like any other; a terrain path with its own culling (04 §4.3) would change the cull column.
- Windows for the RTX 5090, Linux for the TITAN Xp; the GCC v2 and MSVC v3 builds cluster the procedural terrain to slightly different counts (E1 on Pascal found the same), so cross-card comparisons are of passes, not of identical cuts.
- No clock locking on either machine. Warm-ups of at least 2 s before every flight, and a per-frame median over three flights, are what the numbers rest on.
