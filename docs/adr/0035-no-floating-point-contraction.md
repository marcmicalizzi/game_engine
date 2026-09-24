# ADR-0035: No floating-point contraction anywhere in the tree; the content build's bytes are a function of its input

- **Status:** Accepted
- **Date:** 2026-09-24
- **Plan references:** docs/plan/07-content-pipeline.md §7.3 (derived data, addressed by its inputs), docs/plan/08-toolchain.md §8.9 (the CPU baseline and what each dependency does about it), docs/plan/05-simulation.md §5.10 (determinism). Builds on [ADR-0031](0031-minimum-cpu-x86-64-v3.md), whose x86-64-v3 baseline is what brings FMA into reach of the compilers.
- **Docs touched:** `AGENTS.md` (the content-build paragraph and a convention), [geometry](../subsystems/geometry.md#the-same-bytes-from-every-toolchain), [apps](../subsystems/apps.md), [assets](../subsystems/assets.md), [atlas](../subsystems/atlas.md), [anim](../subsystems/anim.md), [math](../subsystems/math.md), [gfx](../subsystems/gfx.md), [local Linux builds](../ci/local-linux.md), [self-hosted runners](../ci/self-hosted-runners.md), [docs/plan/07-content-pipeline.md §7.3](../plan/07-content-pipeline.md#73-content-build-the-derived-data-graph), [docs/plan/08-toolchain.md §8.9](../plan/08-toolchain.md#89-the-cpu-baseline-and-what-each-dependency-does-about-it), [E1 on Pascal](../experiments/e1-pascal-rerun.md), and [the experiment](../experiments/content-build-determinism.md)

## Context

A compiler may **contract** `a*b + c` into one fused multiply-add, rounding once where the source
rounds twice. GCC does it by default across whole inlined expressions (`-ffp-contract=fast`, kept
for C++ even in ISO mode) at `-O1` and above; Clang does it within one source expression
(`-ffp-contract=on`) at every optimization level, `-O0` included; both only where the instruction
exists, which since ADR-0031 is every preset but the three v2 ones. MSVC's `/fp:precise` — the
default, and what this tree uses — never does (checked on MSVC 14.51: `a*b + c` at `/O2 /arch:AVX2`
compiles to `vmulss` and `vaddss`, and to one `vfmadd` only with `/fp:contract`). So one source was
three arithmetics: MSVC and every v2 build; GCC at v3; Clang at v3.

The tree had decided to live with that ([anim](../subsystems/anim.md), "Determinism";
[local Linux builds](../ci/local-linux.md#what-the-first-v3-runs-found)): a kernel whose result is
compared to a tolerance, or never leaves the machine, is faster fused and loses nothing, and the
two test failures the first v3 runs found were fixed with a margin in a producer and a stated bound
in a comparison. Physics was the one exception, because Jolt's cross-platform determinism asks for
it.

On 2026-09-24 the content build turned out to be a result that does leave the machine. Its output
is a derived-data cache entry addressed by a hash of its *input* — source bytes, options, a version
constant — and read by whoever finds it: a shared cache, the package-tests zip, a container copied
beside its source, harness numbers compared between the desktop and the GPU server. From the same
glTF bytes, MSVC, GCC 13 at v2 and GCC 14 at v2 built byte-identical containers on eleven sources,
and GCC 13 and Clang 18 at v3 each built different LOD DAGs under the same key (Suzanne 88, 87 and
87 clusters). The divergence was inside meshoptimizer's clusterizer, sphere fit and simplifier, and
it was contraction: GCC at v3 with `-ffp-contract=off` built MSVC's bytes on all eleven
([the experiment](../experiments/content-build-determinism.md)).

The alternatives on the table:

1. **A toolchain identity in the cache key and in the container**, so no machine mistakes another's
   output for its own. It gives up every cross-toolchain cache hit, and it is not even one identity
   per compiler: GCC contracts at `-O2` and not at `-O0`, so a debug and a release build of the same
   compiler at the same baseline disagree, and the key would have to name the optimization level
   too. It makes the content build's output a function of its input *and* of how the tool was
   built, which is the thing 07 §7.3 says a derived node is not.
2. **No contraction in the content build's own code** — meshoptimizer, `domain/assets`,
   `domain/atlas` and `domain/geometry`'s builder sources, by a per-target and per-source flag. This
   was built first and **measured not to hold**. `core/math` is header-only, so a function such as
   `dot(Vec3, Vec3)` that the compiler does not inline is a weak definition in every object that
   uses it, and the linker keeps the first it meets. At `-O0` nothing is inlined; in
   `linux-clang-debug`'s `engine-content` the copy kept was `main.cpp`'s, compiled with
   contraction (two `vfmadd`), while `gltf.cpp`'s own copy, compiled without, was discarded — and
   three of eleven containers came out with normals a snorm16 step apart. At `-O2` the same happens
   to any function the optimizer declines to inline. The hole is structural: every binary that can
   run the content build (`engine-content`, `engine-view` on a cache miss, `engine-host`, their
   tests) links contracting code, so a per-module rule is only as good as the inliner's mood.
3. **No contraction anywhere**: `-ffp-contract=off` for every target, before anything is fetched or
   added. With it, the same `linux-clang-debug` build agrees with MSVC on all eleven.

## Decision

1. **Every target in the tree is compiled with `-ffp-contract=off`** on GCC and Clang (the
   `/clang:` spelling on clang-cl), third-party libraries included; `cmake/EngineFpContraction.cmake`
   adds it at the top-level directory right after the CPU baseline, for the same reason the baseline
   is added there. cl.exe gets nothing, because it has nothing to turn off. The definition
   `ENGINE_FP_CONTRACTION=0` says so to source that cares. **Nothing turns it back on**: a kernel
   that wants a fused multiply-add writes `std::fma` or an FMA intrinsic (under
   `ENGINE_CPU_BASELINE_V3`), which computes the same thing on every compiler that builds it and is
   visible to its reader.
2. **The content build's output is a function of its source and options alone** — not of the
   compiler, the instruction set or the C library — and the derived-data cache key therefore names
   none of them. What the builders compute that reaches a container uses IEEE arithmetic and `sqrt`
   only (both correctly rounded everywhere), never a C library transcendental in a decision: the
   cone refit, which called `std::acos`, `std::cos` and `std::sin`, now works in cosines with the
   margin's cosine and sine written out as constants.
3. **It is pinned by golden hashes**: `domain/geometry/tests/determinism_tests.cpp` (the builder,
   the pages and the container on an exact fixture) and `apps/engine_content/tests/
   determinism_tests.cpp` (the same through a GLB with a transformed node, via `engine-content
   info`'s per-section hashes). The tables were taken on MSVC; CI's v3 GCC and Clang presets are
   where a regression shows. A mismatch is a bug unless the builder was changed on purpose, in which
   case the table is replaced together with a bump of `k_cluster_cache_version`.

## Consequences

- A container is the same bytes on the desktop, in the Linux containers and on the v2 server, so a
  cache or a zip can move between them, and `k_cluster_cache_version` went to 13 to retire the v3
  Linux entries that were not.
- **The price is paid by GCC and Clang at x86-64-v3 only**, and it is what fusion had bought them
  over MSVC. Under GCC 13 at v3 the content build is about 12% slower — the six 1.8–2.0 M-triangle
  Tripo landmarks 59.0 s instead of 52.7 s, forty E10 props 26.7 s instead of 24.1 s — which is
  where MSVC's build already was (56.6 s and 22.1 s, unchanged). The runtime benches that do float
  work, contraction on against off (lower median of two rounds; the experiment has every row):
  `core/math` from +0.6% (`mat4_mul`) and +4–7% (`transform_point`) through +13–17% (quaternion
  `mul`, `rotate`, `normalize`) and +21–34% (`compose`, `mat4_from_transform`, `transform_aabb`,
  `mat4_inverse`) to +57% (`mat3_from_quat`) and +61% (`transform_inverse`); anim's skeleton
  kernels +10% to +31% (the crowd skinning +19%); the limit surface and the surface binding
  +8% to +12%; animation's tick +4%; nav's tile builds 0 to +6%; the PNG encoder and the sim
  scheduler nothing outside noise; the FLIP image metric, a tool, +27%. MSVC's build of the same
  benches is slower than GCC's without contraction in most of those rows — anim's humanoid tree
  556 ns against 322, the crowd skinning 199 µs against 135, `mat4_inverse` 2,073 ns against 1,451 —
  so what the Linux v3 build gives up is mostly a lead it had over the primary platform, not ground
  under it. v2 builds could not contract and change nothing. The way back for a kernel where it matters is explicit: an FMA intrinsic or `std::fma`
  under `ENGINE_CPU_BASELINE_V3`, which would make MSVC's v3 build faster as well.
- The same source now computes the same floats on MSVC and on GCC and Clang at either baseline,
  which the anim test's two spellings of one product already show (it compares with `==` on every
  build now) and which replay and lockstep across platforms (05 §5.10) will want. It is not a
  promise that every float agrees across platforms: the C library's transcendentals still differ,
  which is why inputs made with `std::sin` (the gfx tests' terrain, the renderer's procedural
  heightfield) are different inputs on MSVC and glibc.
- The FMA-shaped defences written for the v3 builds stay, because they are about more than the CPU
  compiler: `k_cone_margin` also covers the GPU's arithmetic, and the surface binding's
  displacement form is correct whatever contracts.
- The tree no longer has a place where contraction is decided per module, so there is nothing to
  keep in step between modules — and nothing a new target can forget.

## Revisit when

- A kernel's measured cost without contraction matters enough that `std::fma` or intrinsics would
  not recover it — the answer is still explicit fusion, not the flag.
- A platform is added whose default arithmetic is not IEEE as written (a compiler that contracts
  under a flag this file does not know, or `-ffast-math` in a dependency): the golden hashes will
  fail first, and this file is where the new spelling goes.
- The content build moves to the GPU, whose arithmetic this decision does not reach.
