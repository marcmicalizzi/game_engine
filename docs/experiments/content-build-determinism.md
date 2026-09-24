# Content-build determinism: the same bytes from every toolchain

- **Question ([07 §7.3](../plan/07-content-pipeline.md#73-content-build-the-derived-data-graph)):** the content build's output is a derived-data cache entry addressed by a hash of its input — the source's bytes, the options and `k_cluster_cache_version` — and read by whichever machine finds it. On 2026-09-24 two agents noticed that the desktop and the Pascal server build different LOD DAGs from what reads as the same terrain ([gfx](../subsystems/gfx.md): the cull test's 184 clusters against 181; [E1 on Pascal](e1-pascal-rerun.md) follow-up 5: the heightfield's 46,628 against 46,639; the occlusion test's 726 against 728). Is the output a function of the input, or also of the toolchain; where does it diverge; and what does making it a function of the input cost?
- **Date:** 2026-09-24. **Machines:** the desktop (Intel i9-10980XE, 18 cores / 36 threads, Windows 11, MSVC 14.51.36231) and, on it, the Linux container of [local Linux builds](../ci/local-linux.md) (Ubuntu 24.04, GCC 13.3.0, Clang 18.1.3, WSL2 VM with 12 CPUs); the headless GPU server (Intel Xeon E5-2670 Sandy Bridge, 8 cores / 16 threads, Gentoo, GCC 14.3.1) through [remote builds](../ci/remote-linux.md). **Builds:** `msvc-debug`, `msvc-release` (x86-64-v3); `linux-gcc-release` (GCC 13, v3), `linux-gcc-release-v2` (GCC 13, v2), `linux-clang-debug` (Clang 18, v3); `linux-server` (GCC 14, v2); and two experiment presets in a git-ignored `CMakeUserPresets.json`, `linux-gcc-release` and `linux-clang-debug` with `-ffp-contract=off` in `CMAKE_CXX_FLAGS`.
- **Machine state:** the bisection compares bytes and counts, which load does not move. The Linux cost measurements ran in one container under the machine-wide build lock (no other container build beside them), with the host 16.5% busy before and 19% after with other agents' Windows work; the benches' own headers recorded `cpu_others_pct` of 0–2% inside the VM (9.6% at the start of the first `math` run), no WARNING. The MSVC runs had the host at 14.5% before and 10.4% after, and the MSVC benches recorded 6–22% others, WARNING-level: they are upper bounds and context, not a comparison anything rests on. Every content-build binary alternated with the others inside each round, so the ratios are what to read.
- **Decision:** [ADR-0035](../adr/0035-no-floating-point-contraction.md) — no floating-point contraction anywhere in the tree, no C library call in a builder's decision, golden section hashes in the tests; `k_cluster_cache_version` 13.

## Setup

**The inputs.** Eleven sources, built by every toolchain from the same bytes: the 65×65 and 129×129 terrains the gfx and geometry tests make in memory, as MSVC made them, written to `.gltf` files; and the nine Khronos samples `tools/fetch-samples.ps1` fetches that carry geometry (Suzanne, SciFiHelmet, FlightHelmet, Avocado, BoomBox, Lantern, Corset, Fox, RiggedFigure). Each was built by relative path from one root, so even the container's `source_path` section is the same string everywhere, with `engine-content build <in> <out> --jobs 1`, and read with `engine-content info`, which now prints the hash of every section's payload.

**The probe** (a skipped doctest case, not committed): for a mesh in memory, the hash after each stage of the LOD build — the position remap, `meshopt_buildMeshletsFlex` with clusterlod's defaults and the meshlet vertex and triangle arrays, `meshopt_optimizeMeshletLevel`, `meshopt_computeClusterBounds` (spheres and cones separately), `meshopt_partitionClusters`, `meshopt_simplifyWithAttributes` over every level-0 partition (indices and errors), and then `build_cluster_lod`'s every output stream. It ran on the tests' two terrains as each toolchain makes them, on MSVC's bytes for them loaded from a file, and on a 129×129 terrain of integer bumps whose every float is exact.

## Results

### Before: who agrees with whom

Clusters built; **=** is a container whose every section hashes the same as MSVC's:

| Source | MSVC v3 (debug and release) | GCC 13 v2 | GCC 14 v2 (server) | GCC 13 v3 | Clang 18 v3 (debug) |
|---|---|---|---|---|---|
| terrain 65 (MSVC's bytes) | 184 | = | = | 182 | 182 |
| terrain 129 (MSVC's bytes) | 727 | = | = | 732 | 730 |
| Suzanne | 88 | = | = | 87 | 87 |
| SciFiHelmet | 596 | = | = | 598 | 593 |
| FlightHelmet | 2,356 | = | = | 2,356 | 2,359 |
| Avocado | 15 | = | = | 15 | 15 |
| BoomBox | 150 | = | = | 149 | 150 |
| Lantern | 167 | = | = | 167 | 167 |
| Corset | 450 | = | = | 443 | 447 |
| Fox | 19 | = | = | 19 | 19 |
| RiggedFigure | 12 | = | = | 12 | 12 |

MSVC and both v2 builds — three compilers, two C libraries — agree to the byte on all eleven. Every v3 Linux cell differs from MSVC's container, in at least the `clusters` and `lod` sections and in as many as thirteen, including those whose count happens to be the same; the two v3 builds also differ from each other.

### Where: the stage that diverges

On identical input (MSVC's terrain bytes loaded, and the exact terrain), every v2 build matches MSVC at every stage. GCC 13 and Clang 18 at v3 match at the position remap, at the clusterizer's meshlet counts and offsets, and at the partitioner, and diverge at:

| Stage | terrain 65 | terrain 129 | exact terrain 129 |
|---|---|---|---|
| `meshopt_buildMeshletsFlex` vertex and triangle order | same | same | **differs** |
| `meshopt_computeClusterBounds` spheres and cones | **differs** | **differs** | **differs** |
| `meshopt_partitionClusters` | same | same | same |
| `meshopt_simplifyWithAttributes` indices and errors | **differs** | **differs** | **differs** |
| `build_cluster_lod` output (counts) | 182 against 184 | 732 / 730 against 727 | 740 in 51 groups / 738 against 740 in 53 |

All of it is inside meshoptimizer, in float code whose only library call is `sqrtf` (correctly rounded by IEEE). The difference between the two groups of builds is **floating-point contraction**: GCC fuses `a*b + c` into one FMA by default (`-ffp-contract=fast`, at `-O1` and above), Clang within one expression (`-ffp-contract=on`, at every level including `-O0`), both only where the instruction exists — v3 — and MSVC's `/fp:precise` never (checked: `a*b + c` at `/O2 /arch:AVX2` is `vmulss` + `vaddss`, and one `vfmadd` only with `/fp:contract`). **GCC 13 at v3 with `-ffp-contract=off` on every target builds MSVC's bytes on all eleven sources.**

### The tests' terrain is a different input on each C library

The terrain the tests make in memory with `std::sin` and `std::cos` hashes differently on MSVC, on every glibc build without contraction (GCC 13 v2, GCC 14 v2, GCC 13 v3 with contraction off — all three the same), and on GCC and Clang at v3 with contraction (which fuse the fixture's own arithmetic as well). From MSVC's bytes, the server builds MSVC's 184 and 727 clusters; from its own it builds 181 and 728. **So the numbers that started this were the C library, not the builder**: the builders agreed between the desktop and the server all along, because the server is v2.

### Contraction off in the content build's code alone leaks

The first fix put `-ffp-contract=off` on meshoptimizer, `domain/assets`, `domain/atlas`, xatlas and `domain/geometry`'s builder sources, and nowhere else. GCC 13 at v3 then matched MSVC on all eleven and passed the golden hashes; **`linux-clang-debug` still differed on three** — FlightHelmet, the Lantern and the Corset, in `attributes` only: five normal components of FlightHelmet's 144,735 cluster vertices and one of the Lantern's, each one snorm16 step apart. The import computes those normals with `core/math`'s `dot`, `length` and `normalize`, which are header-only; at `-O0` nothing is inlined, so each object that uses them carries a weak copy and the linker keeps the first it meets. In that `engine-content`, `engine::dot(Vec3, Vec3)` came from `main.cpp.o` — compiled with contraction, two `vfmadd` — while `gltf.cpp.o`'s copy, compiled without, was dropped. With the flag on every target (the second experiment preset) the same Clang debug build agrees with MSVC on all eleven. At `-O2` the hole is smaller and still there: any such function the optimizer declines to inline.

### After: the tree without contraction

The golden-hash agreement table — the final tree on every toolchain, and the eleven sources compared section by section with MSVC's final containers:

| Build | eleven sources against MSVC | geometry golden (raster, ray tracing) | engine-content golden (129×129 GLB) | suite |
|---|---|---|---|---|
| `msvc-debug`, `msvc-release` (MSVC 14.51, v3) | the reference | pass (the table's source) | pass (the table's source) | full `msvc-debug`, under the GPU lock |
| `linux-gcc-release` (GCC 13, v3) | same, every section of all eleven | pass | pass | full (62 of 62), then filtered |
| `linux-clang-debug` (Clang 18, v3, `-O0`) | same, every section of all eleven | pass | pass | full (62 of 62), then filtered |
| `linux-server` (GCC 14, v2, the TITAN Xp's host) | same, every section of all eleven | pass | pass | filtered (geometry, assets, content, atlas, anim) |
| `linux-gcc-release-v2` (GCC 13, v2) | same (before the change, and nothing it changed applies at v2) | pass (per-module version) | pass (per-module version, smaller fixture) | filtered |

MSVC's containers are byte-identical before and after the change on all eleven — including the cones, whose refit no longer calls `std::acos`, `std::cos` or `std::sin` — except section 15, the build key, which carries `k_cluster_cache_version`. The goldens have teeth for the coarse case: GCC's `engine-content` from before the change fails the end-to-end table in thirteen sections. They do not have them for the linker leak above — the per-module Clang debug build's `engine-content` passes the end-to-end table whether the fixture carries four distinct normals, 1,089 or 16,641, while it moved a handful of snorm16 steps in three Khronos samples — which is why the flag is on every target rather than trusted to the tests.

## What it costs

MSVC's code does not change (it never contracted), and neither does a v2 build's (it cannot); the whole price is GCC's and Clang's at x86-64-v3. It was measured with GCC 13 in the container, in one session under the build lock: three `engine-content` binaries — `linux-gcc-release` from before the change (contraction on), the same code with `-ffp-contract=off` on every target (the experiment preset: what the tree does now), and the intermediate version with the flag on the content build's own code only — alternating inside every round, `build <mesh> --jobs 1`, the build's own `build_ms`. Then every bench that does float work from two build trees, contraction on (everything but the content build's code) and off everywhere, two rounds each, the lower median kept. The same content builds and four of the bench executables were then run from `msvc-release` on Windows, for what "off" has always meant there.

**The content build.** Fastest of two rounds, milliseconds:

| Tripo landmark | GCC 13 v3, contraction | off everywhere | off in the content build only | MSVC before | MSVC after |
|---|---|---|---|---|---|
| airplane-wreck | 10,853 | 12,452 (+14.7%) | 12,130 (+11.8%) | 12,364 | 11,900 |
| baroque-cathedral | 10,255 | 11,387 (+11.0%) | 11,021 (+7.5%) | 11,500 | 10,935 |
| desert-palm-tree | 1,302 | 1,364 (+4.8%) | 1,459 (+12.1%) | 1,086 | 1,108 |
| mosque | 9,981 | 11,267 (+12.9%) | 10,936 (+9.6%) | 10,779 | 10,511 |
| office-tower | 9,951 | 11,105 (+11.6%) | 10,994 (+10.5%) | 11,156 | 10,888 |
| skyscraper | 10,312 | 11,472 (+11.2%) | 11,188 (+8.5%) | 11,665 | 11,233 |
| **total** | **52,655** | **59,046 (+12.1%)** | **57,728 (+9.6%)** | **58,551** | **56,576** |

The forty Tripo E10 props (`generated/tripo/2026-09-23`, one round): 24,063 ms with contraction, 26,700 (+11.0%) without, 27,663 (+15.0%) with it off in the content build only; MSVC 22,727 before and 22,127 after. So a Linux v3 content build now costs about what the Windows one always has; the two "off" variants are within the rounds' noise of each other, and most of the difference from "on" is meshoptimizer — the code whose answer moved.

**The runtime benches**, GCC 13 at v3, median nanoseconds (the lower of two rounds), with MSVC's one run beside them:

| Bench row | contraction on | off | change | MSVC |
|---|---|---|---|---|
| `math.mat4_mul.batch` / `.chain` | 260 / 898 | 261 / 893 | +0.6% / −0.6% | 347 / 464 |
| `math.transform_point.mat4` / `.transform3` | 70 / 95 | 73 / 102 | +4.2% / +7.1% | 131 / 368 |
| `math.quat.mul` / `.rotate` / `.normalize` | 80 / 117 / 145 | 94 / 135 / 164 | +17.2% / +15.7% / +13.4% | 240 / 299 / 307 |
| `math.compose.batch` / `.chain` | 805 / 1,249 | 1,007 / 1,632 | +25.2% / +30.7% | 1,414 / 2,331 |
| `math.mat4_from_transform.aos` / `.soa` | 291 / 311 | 352 / 417 | +20.8% / +34.1% | 368 / 372 |
| `math.transform_aabb` | 1,301 | 1,683 | +29.3% | 1,722 |
| `math.mat4_inverse` | 1,093 | 1,451 | +32.7% | 2,073 |
| `math.mat3_from_quat` | 220 | 345 | +57.0% | 276 |
| `math.transform_inverse` | 306 | 493 | +61.1% | 383 |
| `anim.skeleton.local_matrices.one` | 116 | 129 | +10.5% | 142 |
| `anim.skeleton.local_to_model.one` | 266 | 320 | +20.5% | 551 |
| `anim.skeleton.skinning_matrices.one` | 86 | 113 | +31.0% | 148 |
| `anim.skeleton.skinning.crowd` | 113,126 | 135,027 | +19.4% | 198,872 |
| `geometry.limit.apply.cage800` | 351,325 | 392,707 | +11.8% | 409,754 |
| `geometry.binding.apply/8192` | 254,702 | 282,587 | +10.9% | 291,036 |
| `animation.tick.mixed` | 3,782,397 | 3,947,239 | +4.4% | 4,346,196 |
| `nav.build.tile/128` | 143,967,779 | 150,787,709 | +4.7% | — |
| `image.flip.render_4k` (a tool) | 3,782,496,651 | 4,800,049,876 | +26.9% | — |

Not shown: `sim` (scheduler and timing wheel within ±5% and noisy both ways), the PNG encoder and deflate (±1%), `geometry.limit.build` and `vertex_ids` (±1%), `animation.pool` (±1%). **In most rows where it bites, GCC without contraction is still faster than MSVC** — every anim row, the limit surface and the binding, the quaternion products, `compose`, `mat4_inverse`, `transform_aabb`, `mat4_from_transform.aos` — which is the sense in which the price is "what fusion had bought Linux over Windows"; MSVC is the faster of the two in `mat3_from_quat`, `transform_inverse`, `mat4_from_transform.soa`, `mat4_from_quat`, `mat4_mul.chain` and `quat.slerp`, three of which it was already faster in with contraction on. A kernel that wants the fusion back has an explicit route, which MSVC would share ([Follow-ups](#follow-ups)).

## What surprised me

- **The finding's numbers were not the finding.** Every count that started this came from the tests' `std::sin`, and the machines whose builders really disagreed — GCC and Clang at v3 against everything else — had not been compared, because nobody builds content in the Linux containers.
- **A per-module compiler flag is not a per-module property** when the arithmetic lives in a header. The flag decides how an object compiles the inline functions it contains; the linker decides which object's copy the program runs.
- **The same hole bites harder in debug builds than release ones**, and the direction is the opposite of what one guesses: GCC does not contract at `-O0` at all, while Clang does, because its front end marks the fusable pairs whatever the optimization level and the backend fuses them.

## What it decides

That the content build's output is a function of its input on every toolchain this project builds with, by construction and pinned by committed hashes, and that the cache key therefore carries no toolchain ([ADR-0035](../adr/0035-no-floating-point-contraction.md), [geometry](../subsystems/geometry.md#the-same-bytes-from-every-toolchain)). It does not decide anything about the GPU's arithmetic, which is why the normal cones keep their margin.

## Follow-ups

1. **The tests' and the renderer's procedural terrains** (`make_terrain` in eight gfx test files and two geometry ones, `systems/renderer/src/scene.cpp`'s heightfield) are made with `std::sin`, so they are different inputs on MSVC and on glibc, and cross-machine comparisons of their counts and of E1's workload stay approximate. Integer bumps, as the determinism fixtures use, or a sine written out as a polynomial, would make them the same mesh everywhere; that belongs to whoever owns those files.
2. **`--atlas repack` is not covered yet.** Three Tripo E10 props (well-bucket, wooden bucket, wooden crate) repacked by `msvc-release` and by `linux-gcc-release` without contraction differed in every geometry section. One cause was a sort: the LSCM solve in `domain/atlas/src/proxy_charts.cpp` summed a matrix entry's duplicate triplets in the order `std::sort` left equal keys, which is the library's choice and not the same in MSVC's and libstdc++; it is a `std::stable_sort` now (`k_repack_version` 2). After it, two of the three build the same clusters, vertices and triangles on both and differ only in `attributes` (the UVs) and the rebaked images; the crate (2,673 clusters on MSVC, 2,681 on GCC) still differs throughout. The remaining suspects are the C library's: xatlas calls `acosf`, `sinf`, `cosf` and `atan2f` while it charts, packs and parameterizes, and the rebake's sRGB tables call `std::pow`. Correctly rounded replacements for the first and constant tables for the second would close it; until then the repack's cache entries are per C library in fact while the key says otherwise ([atlas](../subsystems/atlas.md#determinism)).
3. **Kernels that want fusion back** — the math and anim rows above — can have it explicitly: `_mm_fmadd_ps` or `std::fma` under `ENGINE_CPU_BASELINE_V3`. That makes every v3 build agree with every other v3 build (MSVC's included, which would get faster too) and differ from v2 builds, which is a baseline difference and can be recorded as one.
4. **clang-cl** (`clangcl-debug`) takes the flag as `/clang:-ffp-contract=off`; no clang-cl is installed on the desktop, so that spelling is untested here.

## Caveats

One compiler version of each family. The bisection's claims are about bytes and survive any load; the costs are from one shared machine on one day, with every binary alternated inside each round so that load lands on all of them, and are upper bounds.
