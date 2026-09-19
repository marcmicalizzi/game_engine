# ADR-0031: The minimum CPU is x86-64-v3

- **Status:** Accepted
- **Date:** 2026-09-19
- **Plan references:** docs/plan/04-renderer.md §4.1, docs/plan/08-toolchain.md §8.5 and §8.9, docs/plan/10-roadmap-risks.md §10.8, docs/plan/11-performance-principles.md §11.4
- **Docs touched:** `AGENTS.md`, `docs/plan/04-renderer.md` §4.1, `docs/plan/08-toolchain.md` §8.5 and §8.9, `docs/plan/10-roadmap-risks.md` §10.8, `docs/plan/11-performance-principles.md` §11.4, `docs/subsystems/platform.md`, `docs/subsystems/physics.md`, `docs/subsystems/image.md`, `docs/ci/self-hosted-runners.md`

## Context

Until now the whole tree was compiled to the instruction set of one machine: the project's
minimum test box, an **i7-980** (Westmere, 2010 — SSE4.2 and POPCNT, no AVX, no LZCNT, no F16C).
That floor was never written down as a decision; it arrived as a *consequence*. Jolt's CMake
publishes `/arch:AVX2` on the `Jolt` target as **PUBLIC**, which would have reached every target
that links physics, so `cmake/EnginePhysics.cmake` turned `USE_AVX*`, `USE_LZCNT`, `USE_TZCNT`
and `USE_F16C` off to protect that machine — and the rest of the tree followed, with
[04 §4.1](../plan/04-renderer.md#41-goals-targets-non-goals) promising that "the build carries no
ISA above x86-64 baseline". A dependency's default therefore decided the engine's minimum CPU,
and a machine that is one board away from being replaced decided it for everybody.

The facts that force the question now:

- **Every CPU Windows 11 supports has x86-64-v3.** Microsoft's supported-processor lists start at
  Intel 8th generation and AMD Zen; the oldest part on them, and the oldest part that meets the
  TPM 2.0 and instruction requirements at all, is well above Haswell. A Windows-first engine
  whose floor is a 2010 part is below the floor of its own operating system.
- **The market has moved past it.** Most current titles list a 2015–2017 part as their *minimum*,
  and a growing number refuse to start without AVX or AVX2 — the games that do it name it in
  their system requirements rather than treating it as an implementation detail.
- **The machines that need the old floor are the project's test hardware, not its users'.** Two
  of them: the Windows desktop above (i7-980, Westmere, 2010 — SSE4.2 and nothing above it,
  paired with a Maxwell Titan X) and the headless Linux GPU server (**Xeon E5-2670, Sandy
  Bridge-EP, 2012 — AVX, but no AVX2, FMA, BMI1/2, F16C, LZCNT or MOVBE**, paired with a Pascal
  Titan Xp). They are the only two GPU machines in the project that are not the development
  desktop, and the Xeon is the *only* Linux GPU coverage there is. The Westmere's board is being
  replaced with a 4th-generation-or-newer part; the Xeon is not going anywhere.
- **The minimum GPU is a different question and does not move.** The GTX Titan X (Maxwell) is
  still the baseline tier ([04 §4.1](../plan/04-renderer.md#41-goals-targets-non-goals)): no mesh
  shaders, no ray tracing, 30 fps at reduced settings. A Maxwell card in a Haswell board is an
  ordinary machine; the two floors were only ever coupled because they happened to be in the
  same box.

Alternatives considered:

| Option | Why not |
|---|---|
| Keep the x86-64-v2 floor tree-wide | It is below the floor of the OS the engine targets first, and it is paid by every user so that one 2010 machine can run the same binary. |
| x86-64-v4 (AVX-512) as the baseline | A minority of shipping parts have it, Intel disabled it on its hybrid consumer parts, and the ones that do have it disagree about frequency behaviour. It belongs behind dispatch. |
| An intermediate "AVX but not AVX2" level, for the Sandy Bridge server | x86-64 defines v2, v3 and v4 and nothing between, so the level would have to be invented and named here. It would add a third column to every build matrix, a third `ENGINE_CPU_BASELINE` value to test and a third set of Jolt options, for one machine — and what it buys is the half of v3 that matters least here: AVX without FMA, without AVX2's integer operations and without BMI is 256-bit float arithmetic and little else. Two levels, one of which is only ever a test configuration, keeps the matrix small, which is the whole reason the microarchitecture levels are named rather than assembled from `-m` flags. |
| No baseline at all — runtime dispatch everywhere | Dispatch is a cost at every call site and a combinatorial cost in testing. It is the right tool for the *top* of the range (one or two kernels that want AVX-512), not for the bottom. |
| `-march=native` | Makes a binary a property of the machine that built it. A baseline is a promise to users and has to be named, not detected. |

## Decision

**The engine's promised minimum CPU is x86-64-v3**: AVX2, FMA, BMI1, BMI2, F16C, LZCNT and MOVBE
— Intel Haswell (2013) and AMD Zen and newer — with **4 cores / 8 threads** as the floor (an
i7-4770 or a Ryzen 5 1600) and **6 or more cores recommended**. The minimum GPU stays the GTX
Titan X (Maxwell).

1. **One switch, applied in one place.** `ENGINE_CPU_BASELINE` is `v3` (default) or `v2`.
   `cmake/EngineCpuBaseline.cmake` adds the flag to the top-level directory scope *before* any
   `include()`, `add_subdirectory()` or `FetchContent_MakeAvailable()`, so the engine's modules,
   apps, tests and benches and every third-party library built in this tree share one baseline:
   `/arch:AVX2` on MSVC and clang-cl, `-march=x86-64-v3` on GCC and Clang, and **no flag at all**
   for `v2` on MSVC, where the x64 default's SSE2 plus this tree's explicit SSE4.2 intrinsics is
   already what that baseline means.
2. **A dependency never chooses the baseline; it is told.** Jolt's `USE_AVX`, `USE_AVX2`,
   `USE_LZCNT`, `USE_TZCNT` and `USE_F16C` follow `ENGINE_CPU_BASELINE`
   (`cmake/EnginePhysics.cmake`), so the `JPH_USE_*` defines its headers change shape with agree
   with the flags its library was built with. `USE_AVX512` and `USE_FMADD` stay off at every
   baseline — the first because AVX-512 is dispatch-only, the second because
   `CROSS_PLATFORM_DETERMINISTIC` ignores it. [08 §8.9](../plan/08-toolchain.md#89-the-cpu-baseline-and-what-each-dependency-does-about-it)
   has the table of what every other dependency does: meshoptimizer and SDL3 dispatch at run
   time and need nothing set, Tracy's `/arch:AVX2` is in a file only its server-side tools
   include, and flecs, SQLite, Recast and stb have no instruction-set opinion at all.
3. **AVX-512 is dispatch-only, forever, unless a later ADR says otherwise.** No target is ever
   compiled with it. A kernel that wants it selects it once per batch from
   `platform::cpu_features()` ([11 §11.4](../plan/11-performance-principles.md#114-branch-free-hot-paths-and-constexpr-dispatch)),
   and a kernel with no AVX-512 variant is the normal case rather than an omission.
4. **`v2` is a supported *test* baseline, not a shipping one.** `msvc-release-v2`,
   `linux-clang-debug-v2` and `linux-gcc-release-v2` are `-DENGINE_CPU_BASELINE=v2`; every other
   preset is v3. Both of `.github/workflows/gpu.yml`'s GPU jobs build one, because both of those
   machines are x86-64-v2. What v2 is *not* is a promise to users: the engine's minimum CPU is
   v3, and a game shipped on this engine says so. The distinction matters because it decides what
   a v2 build is allowed to cost — nothing, as long as the tree still compiles and passes there.
5. **A binary that meets a CPU below its baseline says so and leaves.** `platform::require_cpu_baseline()`
   prints one line naming the build's baseline, the missing features, the CPU, and the way out,
   and exits **78**. It is called as the first statement of every app's `main()`, of the shared
   test main, and of the shared bench main, and it runs again from an initializer that sorts
   ahead of every dynamic initializer in the binary. Its translation unit — and the whole of
   `core/platform` with it — is compiled without the arch flag, so the check itself cannot be the
   instruction that traps. See
   [platform](../subsystems/platform.md#the-startup-check-and-why-this-module-gives-up-the-arch-flag).

## What it measured

**AVX2 is not free, and it is not a win. It is a trade, and the trade is different per kernel.**
That is the headline, and it was not the expected one.

**How.** Both sides are the same source tree, built twice: `msvc-release-v2` is the flags the tree
had before this change — on MSVC, `ENGINE_CPU_BASELINE=v2` means *no* `/arch` flag and Jolt's
`USE_AVX*` off, which is byte-for-byte the old configuration, checked against
`compile_commands.json` and `CMakeCache.txt` — and `msvc-release` is x86-64-v3. i9-10980XE (18
cores, 36 threads), `RelWithDebInfo`, Jolt's cross-platform determinism on in both.

**Machine state: not quiet, and the method is what carries the numbers instead.** This box was
between 25% and 100% others' CPU for the whole session (a `--require-quiet` run was refused
thirty times in a row), so no absolute figure here is a cost — every one is an upper bound
([bench](../subsystems/bench.md#measuring-on-a-shared-machine)). Two things make the *comparison*
survive that. The two sides were **alternated** — v2, v3, v2, v3, four to eight times each, seconds
apart — so both saw the same waves of load rather than different ones. And the column compared is
the **minimum** over those runs of the harness's own `min/iter`, which is the sample least
contaminated by whatever else was running. The rows that decide anything were then re-taken in a
second and a third independent session: **v2 and v3 each reproduced to better than 1%** on every
one of them (animation LOD0: 25,422 / 25,381 / 25,488 µs at v2 against 29,014 / 29,179 / 29,358 at
v3), which is the evidence that these are ratios and not noise.

Microseconds per iteration unless stated, lower is better:

| Benchmark | v2 (old flags) | v3 (AVX2) | Δ | worst others' CPU |
|---|---|---|---|---|
| `physics.step.boxes_1000/1` | 2,396.1 | 2,234.5 | **−6.7%** | 27.5% |
| `physics.step.boxes_1000/4` | 1,060.5 | 1,116.0 | **+5.2%** | 27.5% |
| `physics.step.boxes_1000/8` | 731.2 | 780.6 | **+6.8%** | 27.5% |
| `physics.step.soft_cube_512/108` (512-particle cage, 8 iterations, 1 worker) | 960.3 | 976.8 | +1.7% | 100% |
| `physics.e19.tick/216010081` (ADR-0029's default 216-element cage) | 428.4 | 484.3 | **+13.1%** | 37.7% |
| `animation.tick.lod0` (10⁴ × 23 joints, every tick, sampled and skinned) | 25,487.9 | 29,357.9 | **+15.2%** | 54.2% |
| `animation.tick.lod1` (every second tick, no cross-fade) | 13,126.6 | 15,114.1 | **+15.1%** | 54.2% |
| `animation.tick.lod2` (every fourth tick, sampled, **not skinned**) | 4,145.7 | 4,126.7 | −0.5% | 54.2% |
| `animation.tick.lod3` (nothing animated) | 80.4 | 81.4 | +1.3% | 54.2% |
| `animation.tick.mixed` (a plausible tier distribution) | 4,551.9 | 5,173.0 | **+13.6%** | 54.2% |
| `animation.pool.churn/65536` | 500.8 | 499.1 | −0.3% | 54.2% |
| `image.flip.render_4k` (one LDR-FLIP over a 3840×2160 pair) | 2,709,206 | 2,450,933 | **−9.5%** | 25.8% |
| `image.deflate.render_4k.default` | 82,076 | 81,604 | −0.6% | 100% |
| `sim.tiers.assign/0` | 2,364.6 | 2,261.8 | −4.4% | 51.5% |
| `sim.tiers.assign/1` | 1,285.8 | 1,234.1 | −4.0% | 51.5% |
| `sim.tiers.assign/4` | 913.6 | 873.4 | −4.4% | 51.5% |
| `sim.tiers.assign/8` | 654.1 | 647.9 | −0.9% | 51.5% |
| `sim.wheel.insert` | 67,557.6 | 68,030.6 | +0.7% | 30.3% |
| `sim.wheel.advance` | 335,299.2 | 331,855.8 | −1.0% | 30.3% |
| `sim.wheel.cancel` | 15,957.9 | 16,112.2 | +1.0% | 30.3% |
| `sim.wheel.tick` | 0.11 | 0.10 | −1.1% | 30.3% |
| `nav.build.tile/64` (64 m tile) | 32,107.7 | 32,485.1 | +1.2% | 54.2% |
| `ecs.tick.four_systems/4` | 1,235.1 | 1,227.1 | −0.6% | 96.6% |
| FlightHelmet cluster build, `engine-content build --jobs 1` (ms) | 167.1 | 172.1 | +3.0% (medians 182.2 → 180.4, −1.0%) | 8–22% |

Release executables, `build/<preset>/bin`:

| | v2 | v3 | Δ |
|---|---|---|---|
| six executables, total | 20,147,712 B | 20,143,104 B | **−0.02%** |
| every static library in the tree | 127,214,900 B | 126,873,508 B | −0.27% |

### Where AVX2 buys nothing, honestly

**Most of the tree.** `sim.wheel` (a timer wheel: integer bucket arithmetic and linked-list
splices), `image.deflate` (a hash-chain matcher over bytes), `nav.build.tile` (Recast's voxel
pipeline, which has no intrinsics at all), `ecs.tick` (flecs, plain C), and `animation.pool.churn`
(a LIFO free list) all move by **1% or less in either direction**, which on this machine is
indistinguishable from nothing. None of them is float-heavy, and none of them has a loop the
vectorizer can widen. That is the expected result for an engine whose hot paths are mostly
pointer-chasing and integer work, and it is worth stating plainly: **the baseline change is not a
free speed-up, and nineteen of the twenty-three rows above are within ±7%.**

**The content build is unmoved, and its output is identical.** The FlightHelmet build's
`build_key`, `source_hash` and content `hash` are **bit-identical** between the two builds, which
matters more than its 3% of timing noise: a `.clusters` file built on the v2 GPU runner and one
built on a v3 desktop are the same file, so the derived-data cache key does not have to carry the
baseline.

**The binary does not grow.** The whole point of `/arch:AVX2` is wider code, and the six
executables came out 4,608 bytes *smaller*. There is no code-size argument on either side of this
decision.

### The two real wins

**FLIP is 9.5% faster**, reproduced in three sessions (−9.6%, −9.5%, −9.5%). It is the one kernel
here that is a dozen separable `f32` filters over a dozen `f32` planes with no data-dependent
branches, which is exactly the shape a vectorizer can widen from 128 to 256 bits, and the
optimization loop of [04 §4.8](../plan/04-renderer.md) waits on this number every time it compares
a frame to the reference.

**A single-threaded Jolt step is 6.7% faster** — and four- and eight-worker steps of the *same*
scene are 5–7% **slower**. That pattern is not a contradiction, it is the shape of Intel's AVX
frequency licensing on this part: one core running 256-bit code boosts, eighteen cores running it
do not. It is also a warning about generalizing this measurement, because the effect is a property
of a Cascade Lake-X workstation and not of AVX2.

### The one real regression, and where it is

**`animation.tick.lod0` is 15.2% slower**, and the tier rows say precisely where. LOD1 (no
cross-fade interpolation) regresses by the same 15.1%; LOD2, which differs from LOD1 in doing **no
skinning**, does not regress at all (−0.5%); the pool does not move. Dividing the tick rows by
their update counts separates the two halves of an instance's cost:

| per instance-update | v2 | v3 | Δ |
|---|---|---|---|
| sampler (LOD2: sample, no skin) | 1.658 µs | 1.651 µs | −0.4% |
| skinning (LOD1 − LOD2: `local_to_model` + `skinning_matrices`) | 0.967 µs | 1.372 µs | **+42%** |

So the sampler — 23 `slerp`s with their `acos` and two `sin`s, which is what the brief expected to
*gain* from AVX2 — is completely unmoved, and **the 46 `Mat4` products that build a 23-joint
skeleton's skinning matrices are about 42% slower under `/arch:AVX2`**. A 4×4 matrix product is
four 128-bit rows; MSVC given 256-bit registers appears to make a worse choice for it. This is a
codegen problem in one kernel, not an argument about the baseline, and the follow-up is to fix the
kernel — `domain/anim`'s `local_to_model`/`skinning_matrices` — rather than to give one module an
exemption from the tree's instruction set.

## Consequences

**Easier.** The compiler may use AVX2, FMA and BMI in every engine translation unit without a
dispatch table, which is where FLIP's 9.5% came from and is also code that no longer has to be
written by hand. Jolt is built the way its author ships it rather than with five options turned
off. A future SIMD kernel starts from 256-bit registers instead of 128, and the engine's minimum
is now a sentence about the market rather than an accident of one dependency's default.

**One regression to pay off, named.** `domain/anim`'s `local_to_model`/`skinning_matrices` are
~42% slower under `/arch:AVX2`, which is 15% of the animation tick at the tiers that skin. The
decision stands anyway — it is one kernel against a tree-wide baseline, the animation module's own
documented numbers now carry both figures, and the fix belongs in the kernel — but it is a real
cost that this ADR is buying and not a rounding error. **Do not fix it by exempting a module from
the baseline**; `engine_strip_cpu_baseline()` exists for `core/platform` and for nothing else, and
a second caller of it would be this decision quietly unravelling.

**Harder, and honestly.** Three extra presets to keep configuring, and a second configuration is a
second thing to be wrong; both GPU jobs pinned to one of them; and a class of failure that did not
exist before — a v3 binary started on a v2 machine — which is exactly what decision 5 exists to
turn from a crash into a sentence. The startup check is itself a small amount of machinery whose
correctness rests on two build properties rather than on reading the code.

**The `v2` presets exist for legacy *test* hardware, and there are two such machines.**

| Machine | CPU | GPU | Preset |
|---|---|---|---|
| Windows desktop (the garage box) | Intel i7-980, Westmere, 2010: SSE4.2, POPCNT, no AVX | GTX Titan X (Maxwell) | `msvc-release-v2` |
| Headless Linux GPU server | Intel Xeon E5-2670, Sandy Bridge-EP, 2012: AVX, no AVX2/FMA/BMI1/BMI2/F16C/LZCNT/MOVBE | GTX Titan Xp (Pascal) | `linux-clang-debug-v2` (GCC 14.3 is also on it, for `linux-gcc-release-v2`) |

Those two are the baseline-tier GPU coverage of
[docs/ci/self-hosted-runners.md](../ci/self-hosted-runners.md), and the Xeon is the only Linux
machine with a GPU this project has. **What retires the `v2` presets is both machines going, not
either**: the Westmere's board swap to a 4th-generation-or-newer part *and* a replacement for the
Sandy Bridge server. Until then a `v2` build is not a courtesy to one old box, it is how the engine
is tested on real Vulkan drivers at all.

**Forbidden now.** Compiling any target with AVX-512. Letting a dependency's arch flag reach the
tree (`USE_*` options that are not derived from `ENGINE_CPU_BASELINE`). `-march=native`, in any
preset, for any reason. Claiming in documentation that the engine runs on a CPU below v3 —
including the sentence in [04 §4.1](../plan/04-renderer.md#41-goals-targets-non-goals) that this
ADR removed.

**Must now be done.** Keep the two GPU jobs on their `*-v2` presets, and keep the v2 build
working: it is not a configuration anybody may let rot, because it is the one the GPU suites run
in. When *both* machines are replaced: change `windows-maxwell` and `linux-pascal` back to
`msvc-release` and `linux-clang-debug` in `.github/workflows/gpu.yml` and in
[self-hosted-runners](../ci/self-hosted-runners.md), delete the three `*-v2` presets and the `v2`
branch of `EngineCpuBaseline.cmake`, and supersede this ADR. The startup check stays either way —
it is about users' machines, not about ours.

**For plan 11's runtime-selected kernels** ([§11.4](../plan/11-performance-principles.md#114-branch-free-hot-paths-and-constexpr-dispatch)):
this decision is a statement about *where runtime selection starts*, and it makes the variant
machinery smaller than the plan assumed. AVX2, FMA, BMI1/2 and F16C are compiled in
unconditionally, so a kernel needs variants only where 512-bit registers change the loop's shape,
and an ISPC target list is `avx2` plus `avx512skx` rather than a ladder from SSE2. Experiment
**E3**'s calibration trigger — two owned machines disagreeing on a tunable by more than 10% of
frame time — is also the evidence that would justify writing an AVX-512 variant at all; none of
the owned machines is an AVX-512 part today.

## Revisit when

- **Both v2 machines are replaced** — the i7-980's board *and* the Sandy Bridge server. Delete the
  three `v2` presets and this decision's whole second configuration. Replacing only one of them
  changes nothing, which is why the presets are named for a baseline rather than for a machine.
- **A third baseline starts to look necessary.** The alternatives table rejects an "AVX but not
  AVX2" level for the Sandy Bridge server on the grounds that it is one machine and buys the
  least interesting half of v3. A *second* AVX-only machine, or a measurement showing that the
  v2 build of a hot path on that server is misleading enough to change a decision, is what would
  reopen it.
- **Steam's hardware survey, or the games this engine competes with, put the AVX-512 share past
  the point where a dispatched kernel is worth its testing cost** — and a measured kernel here
  shows a gain worth having. Even then the answer is a variant, not a baseline.
- **A target platform without AVX2 becomes real.** A console, a handheld, an ARM port, or a cloud
  instance type whose hypervisor masks parts of v3. `EngineCpuBaseline.cmake` already answers the
  non-x86 case with "no arch flag"; a new *x86* floor would need this ADR superseded.
- **The startup check's cost or its pre-main initializer causes a problem** — a platform where
  `.CRT$XCT` or `constructor(101)` does not behave as documented. The explicit call from each
  `main()` is the fallback that would remain.
