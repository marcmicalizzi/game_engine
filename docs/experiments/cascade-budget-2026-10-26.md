# The expanded forest's cascade budget (R103)

Dates: 2026-10-26 (the opt-in experiment, a Codex night session, below) and 2026-10-27 (the default, its stages). Status: stage 1 (the cascades cut by their own texels) and stage 2 (far casters at rest, per cascade) are the default; the stages after them are in their own sections as they land.

**Question.** On the forest proof of concept's v18 expanded forest at 11520×2160 the cascaded frame was 35.9 ms, of which the four 2048² cascades' depth draws were 20.9 (7.33 million pairs a frame, every cascade a run of the camera's cut at the camera's own LOD to the far side of the scene), and the same frame refused 1.59 million deformed-vertex pool entries after placing 1.12 million in a 768 MiB pool: every swaying tree deformed into each of four maps. The traced frame of the same scene places 245,740 entries and refuses none. How much of that can a cascade leave out by drawing no finer than its own texels, without changing the shadow it casts?

**Answer, in short.** Most of it. A cascade now accepts a transition the camera's test refuses when its world error is at most half of the cascade's texel and its coverage error at most two texels — the coarser of the picture's cut and the texels', never a finer one than the picture's ([renderer](../subsystems/renderer.md#cascaded-shadow-maps), "The cascades' own texels"). On the expanded forest at 11520×2160 the maps draw 1.48 million pairs instead of 7.33 million, the shadow passes take 9.04 ms instead of 21.01 and the frame 24.24 instead of 36.58, and the pool refuses nothing at its default; at 1600×900, 0.60 million instead of 1.77 million, 6.10 ms instead of 10.48. The picture moves by 0.0065 FLIP (43.9 dB) at 1600×900, of which 0.0057 is the cascades' cross-fade switched off, which an independent cut needs.

## The default (2026-10-27)

### The rule, and why it is the union

The opt-in experiment of 2026-10-26 (below) replaced the camera's projection by the cascade's orthographic one: `error × scale_max / texel` against one texel. That is a cut of its own, and it is **finer** than the picture's wherever a texel is finer than the picture's threshold — at 2048² on a heightfield seen from engine-view's orbit it drew 491 pairs where the picture's cut drew 336 — and a finer map than the picture is as wrong a receiver as a coarser one: the map then holds triangles the resolve does not reconstruct. So the rule kept is the camera's test **ORed** with the texels': a transition is acceptable when the camera accepts it or its world error is at most `cascade_error_m` (its coverage error at most `cascade_cover_m`). Each acceptance holds for a cluster whenever it holds for its parent, so the union does too, and the cut is still one cluster on every DAG path; it is the coarser of the two cuts, so a cascade never draws more than the picture's cut, and where the texel is finer than the picture's threshold it draws the picture's cut to the byte. `domain/gfx`'s cull test holds the GPU to that rule cluster for cluster against `geometry::projected_error`, the formula `lod_selects` uses.

### The threshold ladder

The thresholds are the cascade's texel times a number, and the numbers were chosen by what they did to the forest's picture. Each row is the v18 expanded forest's standing path at 1600×900, frame 30, `--capture`, compared by `engine-image compare` (FLIP) with the same frame drawn from the picture's cut alone; the pairs are that frame's maps (the stderr `shadow cascade summary`). The same capture taken twice is identical (FLIP 0, the noise floor). RTX 5090, `msvc-release`, offscreen, under the GPU lock; the machine was shared (other processes 4–71% of the CPU, the GPU 1–4% busy at every start, the busy ends are the run's own frames).

| world error (texels) | coverage error (texels) | pairs in the maps (by cascade) | FLIP mean | PSNR (dB) |
|---:|---:|---|---:|---:|
| the picture's cut | — | 1,771,028 (84,984 / 319,424 / 643,469 / 723,151) | 0 | — |
| 0.0001 (the fade alone) | 0.0001 | 1,533,333 (78,098 / 290,651 / 550,291 / 614,293) | 0.0057 | 44.40 |
| 0.5 | 0.5 | 891,260 | 0.0062 | 44.20 |
| 0.5 | 1 | 714,572 | 0.0062 | 44.20 |
| **0.5** | **2** | **601,178 (75,125 / 225,434 / 221,726 / 78,893)** | **0.0065** | **43.95** |
| 0.5 | 5 | 426,820 | 0.0223 | 34.93 |
| 1 | 0.5 | 766,975 | 0.0111 | 41.66 |
| 1 | 1 | 669,716 | 0.0111 | 41.66 |
| 1 | 2 | 556,349 | 0.0112 | 41.54 |
| 2 | 1 | 568,414 | 0.0227 | 37.35 |

What the ladder says. **The world error is what moves the picture**: at half a texel the near dapple under the canopy is the picture's cut's but for the fade, at one texel it is visibly another sampling of it (0.011), at two another again (0.023). Half a texel is also the constant half of the maps' depth bias (`k_shadow_bias_texels`), so a receiver the map holds coarser than the picture draws it stays inside the bias; that is the reason the number is half and not a quarter (0.25 texels with the coverage at the picture's proportion gave 797,882 pairs for the same 0.0061). **The coverage error is what costs the far cascades**, and it moves the picture only past two texels: one and two give the same picture at either world error, five triples its change. Two texels is half the filter's 4×4-texel footprint. The default is the bold row, `k_shadow_texel_error_texels` 0.5 and `k_shadow_texel_cover_texels` 2.

**The fade.** An independent cut cannot share the cascades' one fade table per pair (`fade_cascade_block`): the last cascade's cull would overwrite the others' splits. So a cascade cut by its texels switches every transition at once, and the first row of the ladder is that alone: 0.0057 FLIP, most of the default's 0.0065, the shadow of a foliage cluster mid-transition drawn whole on one side rather than dithered between two. A fade table per cascade would cost eight bytes a pair per cascade, 537 MB at the forest's 16.8 million pairs, to buy that back, so the cascades have none; the picture's own fade is untouched.

### The forest, before and after

The v18 expanded forest's standing path, the context sheet's arguments (`--terrain-tiles --terrain-far 6 --time-rate 0 --world-rate 1 --no-lights --time-of-day 15 --lod 1 --deform-pool-mib 64 --rt-budget-mib 1024`, flora motion on at wind 0.7, `--raster hw --shadows csm`), 600 frames, `--offscreen`, `--wait-quiet 120`, a scratch copy of `world/v18/ddc`, `msvc-release` at this change, each run under `tools/gpu-lock.ps1 run`. Medians of `gpu_ms`; "before" is `--tunable renderer.shadow.texel_lod=false`, which is main's cut.

| frame | cut | pairs in the maps | shadow ms | frame ms | pool used | pool refused | machine state (GPU start→end, other CPU start→end, GPU memory) |
|---|---|---|---:|---:|---:|---:|---|
| 11520×2160 | before | 7,330,300 (470,964 / 1,869,858 / 2,494,402 / 2,495,076) | 21.01 | 36.58 | 805 MB | 1,591,523 entries, 1.13 GB | 3→100%, 8.7→14.7%, 9,353→11,416 MiB; not quiet |
| 11520×2160 | after | 1,484,413 (174,837 / 1,132,523 / 98,720 / 78,333) | 9.04 | 24.24 | 426 MB | 0 | 2→100%, 8.7→9.9%, 9,154→11,192 MiB; not quiet |
| 1600×900 | before | 1,771,028 (84,984 / 319,424 / 643,469 / 723,151) | 10.48 | 12.78 | 801 MB | 0 | 4→13%, 4.7→5.6%, 8,623→10,587 MiB; quiet |
| 1600×900 | after | 601,178 (75,125 / 225,434 / 221,726 / 78,893) | 6.10 | 8.32 | 279 MB | 0 | 2→1%, 6.6→12.7%, 8,406→10,391 MiB; not quiet |

The GPU at 100% at a wide run's end is the run's own last frames; the harness's verdict ("not quiet") is on the other processes' CPU. Upper bounds, but the pairs are exact and the pool's refusals are counts. The resolve (6.40–6.42 ms wide, 0.38 ms at 1600), the rasterizer (5.20 / 0.53 ms) and the pool pass (1.39–1.51 / 0.56–0.62 ms) did not move. The wide frame's second cascade keeps most of its pairs (1.13 million of 1.87): it spans about 27 to 150 m of a 140° view, where the canopy's coverage errors are mostly over two of its 41 cm texels.

### Against the rays, on the Khronos samples

`renderer: cascaded shadow maps on the Khronos samples` (skipped by default; `msvc-release`, `ENGINE_SHADOW_NO_TIMING=1 ENGINE_SHADOW_PICTURE_LOD_ONLY=1`), the maps against the rays on seven samples from sixteen cameras. **On dad55f90 the case fails at the picture's cut**, which R103 does not change: 192 to 5,247 interior pixels the rays shadow are lit in the maps on six samples (the Lantern none) and one the other way on the SciFiHelmet, where the renderer page records 0. The case's own comment expects one cascade (the scene's bounds) per view, and every view now draws four; why is not settled here. The texel cut gives **the same interior answers, pixel for pixel in every count**, with fewer pairs:

| sample | interior px | acne / leak, the picture's cut | acne / leak, the texel cut | pairs in the maps, picture's → texels' |
|---|---:|---|---|---|
| FlightHelmet | 7,260,120 | 0 / 396 | 0 / 396 | 10,670 → 8,977 |
| SciFiHelmet | 6,625,642 | 1 / 913 | 1 / 913 | 7,705 → 7,345 |
| BoomBox | 7,345,029 | 0 / 192 | 0 / 192 | 1,616 → 1,389 |
| Lantern | 6,338,202 | 0 / 0 | 0 / 0 | 746 → 709 |
| Corset | 7,264,339 | 0 / 684 | 0 / 684 | 3,360 → 2,976 |
| Avocado | 6,327,435 | 0 / 2,137 | 0 / 2,137 | 320 → 320 |
| Suzanne | 6,992,547 | 0 / 5,247 | 0 / 5,247 | 1,402 → 1,402 |

A whole texel and half a texel with the coverage at the frame's foliage-to-geometry proportion (this change's first form) gave the same interior counts again; at a quarter of a texel the maps draw the picture's cut exactly (10,670 pairs on the FlightHelmet).

### On a simplified receiver

The fixture's casters are their own roots, so the texel cut cannot move them (its four casters draw the same bytes and pairs either way, on both raster paths). `renderer: a cascade cut by its texels shadows a simplified receiver as the picture's cut does` is the case that can: a 129² heightfield, which shadows itself, from three orbits, at 64², 256² and 2048² maps, 480×270, drawn from the picture's cut, the texel cut, and the texel cut on the vertex path. Outside a band round every shadow edge as wide as the filter's footprint — four texels of the cascade holding the pixel's depth, in that pixel's size — the texel cut gives no opposite answer: 0 of 6,373 to 81,408 interior pixels per view. The pairs fall from 380–395 to 46–62 at 64², to 170–253 at 256², and stay at the picture's at 2048², where every texel is finer than a pixel and the colours are the same bytes; the vertex path's shadow is the mesh path's to the byte at every size, and the picture's ids never change. At 64² no pixel is interior (the footprint is wider than the picture's shadows); that size is there for the pairs and the paths.

## Far casters at rest (2026-10-27)

Stage 2. The rule ([renderer](../subsystems/renderer.md#cascaded-shadow-maps), "Far casters at rest, per cascade"): past the nearest cascade, a deformed instance whose reach from its rest pose (its cull padding, `bounds_padding × scale_max`) is under the cascade's texel takes no pool block there and casts its rest pose; a terrain level never does. The brief asked for no refused entries at the default pool; the texel cut had already given that (stage 1's table), so what is left to show is what the rule costs and gives on top of it, and how far a sway still reaches the shadow.

The same runs as stage 1's table (v18 expanded forest, standing path, 600 frames, `msvc-release`, offscreen, `--wait-quiet 120`, under the GPU lock), "off" being `--tunable renderer.shadow.rest_texels=0`, which is stage 1:

| frame | rest per cascade | shadow ms (median / p95) | frame ms (median / p95) | pool used a frame | pool refused | machine state (GPU start→end, other CPU start→end) |
|---|---|---:|---:|---:|---:|---|
| 11520×2160 | off | 9.07 / 9.69 | 24.18 / 25.26 | 425,996,904 B | 0 | 1→2%, 8.3→20.9% |
| 11520×2160 | on | 7.88 / 8.47 | 23.18 / 24.12 | 422,555,640 B | 0 | 2→100%, 22.7→10.4% |
| 1600×900 | off | 5.86 / 6.35 | 7.99 / 8.66 | 278,841,600 B | 0 | 1→100%, 10.0→33.6% |
| 1600×900 | on | 5.12 / 5.56 | 7.29 / 7.91 | 274,267,860 B | 0 | 2→1%, 50.1→31.9% |

None of the four runs was quiet by the harness's rule; stage 1's run of the "off" configuration (9.04 ms wide, 6.10 at 1600×900) is the repeat that says the shadow passes' noise between runs is a few hundredths of a millisecond wide and a quarter of one at 1600×900. The pairs in the maps do not change (the rule changes what an entry reads, not which entries there are). The pool falls by 3.4 MB wide and 4.6 MB at 1600×900, about 290,000 and 380,000 vertices: on this forest the texel cut had already left the far cascades mostly the rigid distance forms of the outer rings, which take no block at all, so the deformed trees still in cascades 1 to 3 are the near rings' few. Where the shadow passes' 0.7 to 1.2 ms comes from with so few vertices left out is not settled here; the pool pass of a swaying tree runs the part-chain bend on each, and the frame's pool pass outside the cascades (`gpu_ms.deform`, 1.55–1.57 wide) did not move.

**How far a sway still reaches the shadow** is set per tree by its reach against the cascade texels the fit gives the frame (`engine-view: shadow fit`):

| frame | cascade depths (m) | texels |
|---|---|---|
| 11520×2160 | 0.1–26.7, 26.7–150, 150–844, 844–4,745 | 7.4 cm, 41 cm, 2.34 m, 4.44 m |
| 1600×900 | the same | 2.8 cm, 16 cm, 88 cm, 4.94 m |

A tree whose sway can reach 1.8 m (the young palm's bound, [renderer](../subsystems/renderer.md#the-flora-stage)) shadows its sway to 150 m at 11520×2160 and to 844 m at 1600×900; one that can reach 2.6 m (the grove's broadleaves) to 844 m at both; nothing sways in the last cascade's shadow unless it can move more than 4.4 m. The reach is the bound at the saturating wind, so at the forest's 0.7 a tree's real sway is smaller than the texel it is held against, and the rule errs towards posing.

## The opt-in experiment (2026-10-26)

What follows is the Codex night session's record, kept as it was written but for its headings: the baseline, the sources, the first orthographic cut and the integration constraints it found. Its cut (the texels' projection alone, at one texel) was not the one adopted, for the reason "The rule" gives above; its remaining-work list is what the later stages of this page answer. Its "Validation recorded so far" section was superseded by the cases above and is not kept.

### Baseline evidence

The read-only forest POC run `expanded-forest-csm-wide-20261026-191419`, under
`reports/quality-v15/engine-c1ee971d/runs/`, reports 11520 × 2160, hardware raster,
four 2048² cascades, 6,914,605 shadow pairs in its last frame and no traced
casters. Its `summary.gpu_ms` reports 52.2821 ms for shadow passes (including
1.5592 ms for their culls), out of 68.3285 ms total. These are the report's
aggregates, not a fresh measurement on this branch.

**Machine state:** other-process CPU 3.7866% → 17.1875%, GPU utilization
3% → 2%, GPU memory 16,344 → 18,484 MiB, under the report's GPU lock.
These shared-machine figures are upper bounds, not isolated costs.

The command requested a 64 MiB deformation pool, but the report's actual pool
is **805,306,368 bytes (768 MiB)**: terrain raises the pool minimum. The last
reported allocation used 805,306,344 bytes, served 1,137,485 entries, and refused
1,151,125 entries / 809,592,468 bytes. Calling this a 64 MiB overflow conceals
the actual demand. The original report has no per-cascade pair breakdown.

### Constraints and sources

- [Renderer contract](../subsystems/renderer.md#cascaded-shadow-maps) and the
  [original CSM measurements](cascaded-shadow-maps.md): the current cut is the
  camera's. Coarsening it can create self-shadowing away from silhouette edges;
  a texel error bound alone is not evidence of a bound on shaded-image error.
- Zhang, Sun and Nyman, [GPU Gems 3, chapter 10](https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-10-parallel-split-shadow-maps-programmable-gpus),
  sections 10.2.1–10.2.2: split the receiver volume and fit an orthographic light
  projection, retaining potential casters towards the light. An orthographic
  error projects as world-space error divided by world units per texel; distance
  to the camera does not enter that projection.
- `fit_shadow_cascades` bounds receivers by camera depth, but extends each
  light-space depth range towards the scene's bounds. A receiver horizon is
  therefore not itself a caster-distance cutoff.
- R95 already supplies `DeformAllocParams::rest_beyond_m`. It rejects pool
  requests deterministically by instance-origin distance before allocation,
  independently of the atomic order. Zero applies only species rest policies;
  a positive distance also applies to every deforming instance beyond it.

The atlas layout, `ShadowMapParams`, and the PCF lookup belong to the shared
contract with the hybrid-shadow work. Changes must preserve that interface.

### First reproduction

The worktree's `msvc-debug` engine-view, offscreen, the copied DDC, the frozen
scene and its stationary eight-second path, 600 frames, `--wait-quiet 120`,
reproduces **6,914,605 pairs** at 11520 × 2160 and **52.8202 ms median** shadow
time (mean 52.9175, p95 54.0724). Machine state: other CPU 21.4539% → 13.8889%,
GPU 2% → 2%, 9,319 → 11,366 MiB, GPU lock held by this run. The machine did not
become quiet. These are upper bounds. At 1600 × 900 the first reproduction drew
1,693,325 pairs and took 17.0490 ms median shadow time, beside other CPU 100% at
both endpoints and GPU 5% → 0%; it is not an isolated cost.

The POC's path has identical position and target at both keys; it is a standing
camera. A separate scratch path translates position and target 12 m in x over
eight seconds for the walking comparison. No POC file is modified.

**An independent cut also needs independent fading.** R91's camera-cut cascades
share one fade table. Merely changing their error projection lets the last cull
overwrite the earlier cascades' splits. The experimental texel path uses hard
foliage transitions; the camera's fade and the default camera-cut path are unchanged.

### Independent texel-cut experiment

The opt-in is `--tunable renderer.shadow.texel_lod=true`; the new
`RenderSettings::shadow_texel_lod` is false by default. No atlas, resolve, PCF,
`ShadowMapParams`, normal-offset or depth-bias contract changed.

For cascade c, let t be its world units per texel, s the instance's maximum
scale and e the DAG transition's geometric error. The projected error is
`e * s / t`, with terminal errors kept terminal. The own transition must be
acceptable and the parent transition unacceptable, exactly as the camera's
two-sphere test. Coverage errors use the same projection and retain the
geometric-or-coverage acceptance rule. The geometric threshold is
`shadow_lod_scale` texels (1 by default); the foliage threshold is the frame's
foliage threshold times that multiplier, interpreted in texels. Moving the eye
without changing t does not change this projection.

This bounds the *projected error used by the selector*, not the final shaded
image. In particular, the coverage alternative can accept a larger geometric
error, and a simplified receiver's depth can cross the shadow comparison away
from a silhouette. The existing sample results are evidence against claiming
an unconditional PCF-band bound without new image measurements. The experiment
therefore does not replace the shipping camera cut.

Fresh paired standing runs, 600 frames each, RTX 5090, `msvc-debug`, offscreen,
no warmup, the frozen scene, scratch DDC and `--wait-quiet 120`:

| Size | Cut | Shadow median / p95 ms | Total median ms | Shadow pairs |
|---|---|---:|---:|---:|
| 1600 × 900 | camera | 17.1922 / 18.0333 | 19.9123 | 1,693,325 |
| 1600 × 900 | texel | 6.2048 / 6.6154 | 8.8218 | 496,545 |
| 11520 × 2160 | camera | 52.6281 / 53.7345 | 68.7875 | 6,914,605 |
| 11520 × 2160 | texel | 7.5618 / 8.0709 | 23.4703 | 548,772 |

**Machine state (start → end):**

| Run | Other CPU % | GPU utilization % | GPU memory MiB |
|---|---:|---:|---:|
| 1600 camera | 100 → 84.0278 | 2 → 41 | 8,600 → 10,556 |
| 1600 texel | 5.0523 → 16.2912 | 1 → 9 | 8,581 → 10,551 |
| 11520 camera | 70.9302 → 91.5683 | 6 → 100 | 15,135 → 17,183 |
| 11520 texel | 38.7755 → 35.5517 | 2 → 0 | 9,325 → 11,375 |

All held this run's GPU lock, the session was unlocked, and every run warned
about other activity. These are upper bounds taken under different contention;
the pair counts establish the work reduction, not a controlled speedup ratio.
All four runs report allocations only in frames 0 and 1 (11,815 allocations,
one device buffer), with none reported afterwards. The dedicated frame-loop
test remains the regression gate.

### Remaining work and integration constraints

R103 is not complete. The 1,280 m default horizon, explicit counted caster
omission, new distance-rest setting and per-cascade instance counts, persistent
cache and invalidation/write-count tests have not been implemented. No staged
1+2, 1+2+3 or 1+2+3+4 result is claimed.

- A receiver split at 1,280 m does not remove all distant casters: the depth
  sweep still reaches towards the light. Add a counted caster test explicitly.
  Changing the fit's horizon also moves the logarithmic near splits, so the
  requirement to preserve cascade 0's old bytes needs a deliberate fit policy.
- Do not turn on the existing positive `rest_beyond_m` and call it a tree
  policy. It applies to **every** deformed instance, including terrain. The
  terrain pool stage reconstructs the authored moving heightfield; its grid
  fallback is not a generic distant-sway approximation. Restrict a flora rule
  through the owned caster-policy flags, or coordinate an allocator contract.
- The atlas object already persists, but the graph imports it undefined and
  clears the entire row every frame. A cache needs a preserved import layout,
  per-tile clearing, and render-graph writes only for refreshed tiles. Cached
  matrices must be re-expressed at the current eye after an origin change.
- `ShadowMapParams` carries **one** light basis for all cascades; the PCF
  receiver slopes use it. Keeping maps at different sun orientations while
  publishing the latest basis is not the current lookup contract. A common
  quantized light frame, or an agreed per-cascade lookup change, needs image
  validation before claiming sun-angle caching.
- Moving near casters can overlap far cascades' light volumes. A distance rest
  rule alone does not make those cascades static. Invalidate on the actual
  caster/terrain/instance changes, not just camera position or elapsed time.

The resolve-side hybrid can consume these maps with its existing atlas and
parameter contract. It must not assume this branch has cached maps, a 1,280 m
horizon, or a proven far-receiver image bound.

The engine's current `renderer.sun.rate` default is **0** (`lighting.cpp`),
which holds the sky; `WorldClocks::day_length_s()` calls that an endless day.
The supplied runs do not override it. A future cache-cadence experiment must
report this default (no sun-driven refresh) separately from an explicitly
moving day, for example `--day-length 3600`; no finite default day length is
assumed here.

