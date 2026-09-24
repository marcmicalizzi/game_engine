# math (core)

**Purpose.** Header-only linear algebra: `Vec2/3/4`, `Vec2i/3i`, `Quat`, `Mat3/4`, `Transform3` (position, rotation, scale), `Aabb3`, scalar helpers, and the view/projection constructors the renderer will use.

**Conventions** (fixed; everything else builds on them):
- Right-handed, y up, forward is -z (view space looks down -z).
- Column-major matrices, column vectors: `p' = M * p`, `M = T * R * S`.
- Quaternions are `{x, y, z, w}` with `w` the scalar; a vector is rotated as `q v q^-1`; `q` and `-q` compare equal under `approx_equal`.
- Angles are radians.
- Projections produce zero-to-one **reversed** depth (near = 1, far = 0), with an infinite-far option, for float precision across large view distances. The Vulkan y-flip is a viewport matter, not a projection one.
- Layouts are GPU-friendly and trivially copyable: `Vec3` is 12 bytes, `Mat4` 64, `Transform3` 40. These are also the IDL's `vec2 vec3 vec4 quat` primitives.

**Invariants (tested).** `M * inverse(M) ≈ I`; `quat_from_mat3(mat3_from_quat(q)) ≈ q`; `compose`/`inverse` on `Transform3` agree with the equivalent matrix; `normalize` of a zero vector is zero, not NaN; `inverse` of a singular matrix is the identity rather than NaN.

**Public API.** `core/math/math.h`. Free functions: `dot`, `cross`, `length`, `normalize`, `lerp`, `min`, `max`, `reflect`, `rotate`, `slerp`, `conjugate`, `inverse`, `transpose`, `determinant`, `translation`, `scaling`, `mat3/4_from_quat`, `quat_from_mat3`, `quat_from_axis_angle`, `quat_from_euler`, `mat4_from_transform`, `transform_point`, `transform_direction`, `compose`, `transform_aabb`, `look_at`, `perspective_reversed_z`, `orthographic_reversed_z`.

**Depends on.** `base`.

**Testing.** `tools/dev.ps1 test -Filter math`. `the rewritten matrix builders give the old floats exactly` pins the change below: `mat4_from_transform`, `mat4_from_quat` and the `Mat4` product are compared with `==` against the forms they replaced, over 64 transforms that include a negative scale, a zero scale and a quaternion that is not unit length (the expansion does not normalize, and must not start to).

**Benchmarks.** `tools/dev.ps1 bench -Preset msvc-release -Filter 'math.*'` — `bench/math_bench.cpp`, one row per hot function, each a batch of 64 inputs laid out the way a caller loops over them (plus two *chains*, a product and a `compose` that read what the previous call wrote, because a hierarchy's forward pass is how both are used). The inputs are a fixed function of the index, so two builds start from the same bits.

## Aggregates returned by value, and the AVX2 copy

[ADR-0031](../adr/0031-minimum-cpu-x86-64-v3.md) moved the tree to x86-64-v3, and while [anim](anim.md#the-matrix-kernels-and-the-avx2-regression-they-used-to-be) was paying off the regression that ADR recorded it found a second one here: `mat4_from_transform` **77% slower** at v3 under MSVC, measured from `domain/anim`'s bench because this module had none. It has one now, and with it in hand the problem turned out to be wider than one function and narrower than "AVX2".

### What it was

Read from MSVC's `/FA` output for the same source at both baselines (`/O2 /Ob1`, the `msvc-release` flags), every row that regressed at v3 had the same thing in its loop and every row that did not had none of it: **32-byte `vmovups` copies of a result that had just been built, with narrower stores, in a stack temporary.**

- `mat4_from_transform` called `mat3_from_quat` **out of line** — saving and restoring ten XMM registers, returning nine floats through memory — then assembled two of the four columns of its `Mat4` in a temporary with 4-byte stores and copied them to the destination with one 32-byte load and store. A load wider than the stores that feed it cannot be forwarded from the store buffer; it waits for them to reach L1. At v2 the same copy is 16 bytes wide and MSVC builds the columns in registers with `shufps` first, so it pays far less.
- `operator*(const Mat4&, const Mat4&)` is the same disease with the arithmetic already right: MSVC vectorizes the product at *both* baselines (`mulps`/`addps`, four columns in 128-bit registers), and at v2 stores the four columns straight into the destination. At v3 it stores three of them into a temporary with 16-byte stores and copies them out with two **overlapping** 32-byte moves — each spanning two of those stores, so neither can forward. A chain of products, where the next one reads the columns this one wrote, pays it on the critical path.
- `inverse(Mat4)` and `compose` show it too: `inverse` is called out of line and its caller copies the 64-byte result out of the return slot with two 32-byte moves; `compose` builds its `Transform3` in a temporary and copies it with a 32-byte move and an 8-byte one.

That is the shape `domain/anim` found in its own kernels, and the lesson is the same: **`/arch:AVX2` did not make the arithmetic wider; it made the copies wider**, and a copy of something just written is the one place width is a liability. Nothing about it is specific to matrices — any trivially copyable aggregate of 32 bytes or more, built piecewise and then assigned, is a candidate — and MSVC's choice of whether to materialize the temporary at all depends on the caller: the same `compose` in a plain loop in a separate file had no temporary.

### The fix, and what did not work

- **`mat4_from_transform` and `mat4_from_quat` expand the rotation in place**: nine products, then the sixteen entries written as four `Vec4` columns of one braced return. There is no `Mat3` and no call, and MSVC keeps the whole thing in registers at both baselines and stores the result once. The expressions are `mat3_from_quat`'s, each column scaled exactly as `r.c[k] * scale` scaled it, so the floats are the same.
- **The `Mat4` product assigns its columns into a named result** (`Mat4 r; r.c[k] = a * b.c[k]; return r;`) instead of returning a braced aggregate of the four. With that shape MSVC stores each column straight into the destination at both baselines. Same operations in the same order, same floats.
- **Tried and rejected**, each by reading what MSVC generated for it at both baselines, so they are not retried: intrinsics stored into a local `Mat4` and returned (MSVC then builds the temporary and copies it with 32-byte moves anyway); intrinsics handed to the columns through `std::bit_cast<Vec4>` (the same); lanes extracted into sixteen scalar stores (no copy, but a chain's next product then reads 16-byte columns over four 4-byte stores, which cannot forward either); and the named-result shape for `compose`, which removed the temporary in a small loop and not in the bench's, so there is no source shape that pins MSVC's choice there. **Considered and not tried:** AVX intrinsics writing 32-byte halves so that the copy *can* forward — an `#ifdef __AVX2__` in a header is two definitions of one inline function across translation units compiled at different baselines (`core/platform` is one), which is an ODR violation waiting for a linker to pick the wrong copy.
- **Not changed, and why:** `compose` and `inverse(Mat4)` stay as they were. Neither is called on a hot path in the tree — `inverse` is a camera, a ray-trace parameter block and the glTF importer's normal matrix; `compose` has no engine caller at all — and the robust fix for an out-of-line function whose result is assigned is an out-parameter or a forced inline, which is an API decision to take when one of them becomes hot, with this bench's row to measure it.

**The one cost.** The product's named-result shape makes MSVC hoist all sixteen of `b`'s scalars into registers and spill `a`'s columns, so at **v2** it is 7% slower than the braced form was. ADR-0031 decision 4 says what a v2 build may cost — nothing *functional* — and the shipping baseline is v3, where the same change is 42–47% faster; the trade was taken with both numbers in hand.

### Numbers

`build/msvc-{release,release-v2}/core/math/engine_math_bench.exe`, i9-10980XE (36 threads), `RelWithDebInfo`. **Machine state: not quiet** — others used 18–100% of the CPU across the first session and 30–59% across the second, the GPU 5–13% ([bench](bench.md#measuring-on-a-shared-machine)) — so no absolute figure here is a cost, and the method carries the comparison, as it did for ADR-0031 and [anim](anim.md): the four binaries, {before, after} × {v2, v3}, **alternated seconds apart** for six rounds, and the column is the minimum over those rounds of each binary's own `min/iter`. A second session reproduced every row below to within 1% except one (`mat4_from_transform.aos` before, at v3: +3.7%). Nanoseconds per call, lower is better:

| Benchmark | v2 before | v2 after | Δ | v3 before | v3 after | Δ | v3 after against v2 after |
|---|---|---|---|---|---|---|---|
| `math.mat4_from_transform.soa` (the `anim::Pose` layout) | 14.98 | **6.50** | −57% | 18.50 | **5.76** | −69% | **−11%** |
| `math.mat4_from_transform.aos` | 17.23 | **5.86** | −66% | 20.42 | **5.72** | −72% | −2% |
| `math.mat4_from_quat` | 11.19 | **4.52** | −60% | 10.83 | **4.42** | −59% | −2% |
| `math.mat4_mul.batch` | 5.60 | 6.02 | **+7%** | 9.20 | **5.36** | −42% | **−11%** |
| `math.mat4_mul.chain` (each product reads the last) | 6.90 | 7.29 | **+6%** | 13.71 | **7.32** | −47% | 0% |
| `math.mat4_inverse` (unchanged) | 27.25 | 27.25 | 0% | 32.27 | 32.28 | 0% | +18% |
| `math.compose.batch` (unchanged) | 19.27 | 19.24 | 0% | 21.76 | 21.85 | 0% | +14% |
| `math.compose.chain` (unchanged) | 30.09 | 30.08 | 0% | 36.95 | 37.07 | 0% | +23% |
| `math.transform_aabb` | 28.90 | 28.90 | 0% | 26.70 | 26.79 | 0% | −7% |
| `math.transform_point.mat4` | 2.02 | 2.02 | 0% | 1.92 | 1.92 | 0% | −5% |

`mat3_from_quat`, the quaternion product, `rotate`, `normalize`, `slerp`, `inverse(Transform3)` and `transform_point(Transform3, Vec3)` are within 2% between every pair of columns, before and after; none of them builds a 32-byte aggregate.

**What it adds up to.** Before, the v3 build of the transform-to-matrix path was 23% *slower* than v2 and the product 64% (99% in a chain); after, both are at or below the v2 build — the transform 11% faster, the batch of products 11% faster, the chain equal — so the shipping baseline no longer pays for either. Through `domain/anim` the fix is **`anim.skeleton.local_matrices.one` 435 → 140 ns at v3** (from +77% over v2 to −14%); [anim](anim.md) says why `local_to_model` itself barely moved and where its remaining v3 cost lives.

**GCC 14 and clang 22, at x86-64-v2.** The same {before, after} pair built on the Linux GPU server ([remote Linux builds](../ci/remote-linux.md)) with GCC 14.3.1 and clang 22.1.8, `RelWithDebInfo`, alternated six rounds, twice. That Xeon is a Sandy Bridge and cannot run a v3 build, so this is the v2 half only — which is also the half where MSVC never regressed. The first session ran with the machine fully loaded by someone else (load average up to 20 on 16 threads); the second at 2 or below, with others at 16–33% of the CPU, and it reproduced the first to about 1% on every row. Nanoseconds per call, second session:

| Benchmark | GCC before | GCC after | Δ | clang before | clang after | Δ |
|---|---|---|---|---|---|---|
| `math.mat4_from_transform.soa` | 20.20 | 19.99 | −1% | 26.68 | 26.16 | −2% |
| `math.mat4_from_transform.aos` | 21.91 | 22.90 | **+5%** | 25.01 | 23.92 | −4% |
| `math.mat4_from_quat` | 17.04 | 17.00 | 0% | 20.86 | 20.88 | 0% |
| `math.mat4_mul.batch` | 24.40 | 24.44 | 0% | 39.86 | 39.58 | −1% |
| `math.mat4_mul.chain` | 66.39 | 63.87 | −4% | 39.69 | 37.94 | −4% |
| `anim.skeleton.local_to_model.one` (ns per 23 joints) | 1,568 | 1,569 | 0% | 964 | 1,005 | **+4%** |

**Neither compiler had the disease**, which is the finding: neither copies a freshly built `Mat4` through a temporary, so the rewrite is close to neutral for them, as it should be for a change aimed at one compiler's lowering of one idiom. Two rows moved the wrong way, both by 4–5% and both in both sessions: GCC's array-of-transforms loop, and clang's `local_to_model`, where it inlines `mat4_from_transform` and schedules the expansion differently. They are recorded rather than chased with a second, per-compiler source path; the shipping build on Windows is MSVC at v3, where the same change is −69% to −72%. Worth knowing beside them: GCC's product *chain* is 2.7× its batch while clang's is flat, a latency GCC's scheduling exposes and clang's hides, and it predates this change.

**Determinism.** Every function here is pure, so a result is bit-identical across runs and across worker counts by construction, and the test pins that the rewrite produced **the same floats as before** at both MSVC baselines (`/fp:precise` does not contract, so v2 and v3 emit the same operations in the same order). Under GCC and clang at x86-64-v3 the default floating-point contraction used to fuse some of these products into FMAs, so a v3 build's matrices differed from a v2 build's in the last bit on those compilers, as [anim](anim.md) found for its kernels. Since [ADR-0035](../adr/0035-no-floating-point-contraction.md) the whole tree is compiled with `-ffp-contract=off`, and every compiler at either baseline evaluates these as written. **This module is the reason that had to be tree-wide**: its functions are header-only, so one the compiler does not inline — every call at `-O0` — is a weak copy in each object that uses it, the linker keeps whichever it met first, and a flag on the caller's module says nothing about which copy runs ([geometry](geometry.md#the-same-bytes-from-every-toolchain) has the case: `dot(Vec3, Vec3)` in a debug Clang `engine-content`).

**Performance notes.** Scalar code, `constexpr` where the standard allows. SIMD versions arrive with ISPC kernels for the hot paths (culling, transforms, animation) rather than by widening these types; keeping `Vec3` at 12 bytes is deliberate for GPU buffer layouts. The rule the section below earned: a function that returns a `Mat4` or a `Transform3` by value builds it from values the optimizer can see — expanded in place, or column by column into a named result — rather than as a braced aggregate of out-of-line helpers' results, and a new hot one gets a row in the bench before it gets a caller.
