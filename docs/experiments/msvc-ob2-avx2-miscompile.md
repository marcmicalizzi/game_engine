# MSVC 19.44 and 19.51 at `/O2`: a by-value aggregate's second copy is never written

- **Date:** 2026-11-19 (batch 69, roadmap T197). **Machine:** the owner's i9-10980XE, Windows 11. **Compilers:** every MSVC toolset installed there, 14.16.27023 to 14.51.36231 (table 1); the tree builds with 14.51.36231 (`cl` 19.51.36256, Visual Studio 18).
- **Main's release flags are exposed.** The reduction below, with the function that holds the two calls declared `inline`, is miscompiled by the `msvc-release` compile line as it stands on main:
  `cl /DWIN32 /D_WINDOWS /EHsc /O2 /Ob1 /DNDEBUG -std:c++20 -MD -Z7 /arch:AVX2 /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8` gives 480 wrong values of 960. So does the v2 baseline's line (no `/arch`). `msvc-debug` (`/Od`) is not reached.
- **What it is.** An aggregate passed in memory, by value, to two calls in one expression. Once the function holding the two calls is inlined into a caller that treats the two results as a pair, the compiler makes two stack copies of the argument, one a call, and **never writes the second**: the second callee reads whatever the stack held. No warning, no crash, a plausible number that changes when unrelated code moves.
- **How it was found: by a determinism pin, and by nothing else.** T193 made `/Ob2` the release preset's default, and `rocks`' "the committed escarpment is closed, holds its budget, and is its bytes" gave a mesh hash that was not the pin. No test of behaviour failed, in a suite of 182: the cliff still closed, held its budget and looked like a cliff. A pinned answer that every toolchain must reproduce was the only thing in the tree able to see a wrong number that is still a reasonable one. That is the lesson, and it cuts both ways: the pins found it, and code with no pin has no such witness.
- **What it means for `/Ob2` as the default** is at the end; in one line: `/Ob2` did not cause the fault and `/Ob1` does not avoid it, it only inlines fewer of the functions that can carry it.

## Where it was found

`domain/rocks/src/cliff.cpp`, `weathered_uv`, which the box emitter's chart lambda calls for a cell's four corners:

```cpp
ChartUv weathered_uv(ChartUv uv, u64 seed, f64 amount) {
  const D3 q{uv.a / 7.0, uv.b / 7.0, 0.25};
  return {uv.a + amount * (noise3(seed, q) - 0.5), uv.b + amount * (noise3(seed + 1u, q) - 0.5)};
}
```

`noise3(u64, D3)` takes its point by value: three doubles, 24 bytes, which the x64 convention passes as a pointer to a copy the caller makes. `weathered_uv` is not declared `inline`, so `/Ob1` leaves it out of line, where its code is right (two copies, six stores). With `/Ob2` it is inlined into the lambda and that into the emitter's `chart_of`, whose loop adds a quarter of each result to a running pair. The listing of that function (`/FA`), at the two calls:

```asm
lea    rdx, QWORD PTR $T2[rsp]
vdivsd xmm0, xmm8, xmm12                 ; uv.a / 7
vmovsd QWORD PTR $T2[rsp], xmm0
vmovsd xmm0, QWORD PTR __real@3fd0000000000000
vdivsd xmm1, xmm7, xmm12                 ; uv.b / 7
vmovsd QWORD PTR $T2[rsp+16], xmm0
vmovsd QWORD PTR $T2[rsp+8], xmm1
call   noise3                            ; noise3(seed, &$T2): the copy was written
mov    rcx, QWORD PTR tv1181[rsp]        ; seed + 1
lea    rdx, QWORD PTR $T3[rsp]           ; a second copy, at $T3 ...
vmovaps xmm6, xmm0
call   noise3                            ; ... which no instruction of the function writes
vunpcklpd xmm1, xmm6, xmm0               ; the two results as one vector
vsubpd xmm2, xmm1, xmm13
```

`$T3` appears twice in the function, both times as that `lea`. Three things say this is the compiler and not the source: the listing; the wrong hash itself moving when an unrelated statement was added elsewhere in the function (12883758243324567365, then 4076465862834460944: a correct compile of one meaning has one answer); and the reduction below, which holds no pointer, no uninitialized read and no unspecified order, and which every other compiler and setting agrees on.

How the search went, for whoever meets the next one: `#pragma auto_inline(off)` round halves of the translation unit, then round single functions, with the tree configured `/Ob2`, named `weathered_uv` in a dozen builds of one file; `#pragma fp_contract(off)` and `float_control(precise, on)` over the whole file changed nothing, which ruled contraction out; recording the function's inputs and outputs made the fault vanish (the recording gave the results a second use), and recording one step later showed values that were not a rounding apart but had lost the noise altogether, which is what sent the search to the listing.

## Table 1: which compilers

The reduction, the callee declared `inline` (`/DREPRO_INLINE`), `/O2 /Ob1`, with and without `/arch:AVX2`; and as written (not `inline`), `/O2 /Ob2 /arch:AVX2`. All three rows of a toolset agree.

| toolset | `cl` | Visual Studio | |
|---|---|---|---|
| 14.16.27023 | 19.16.27054 | 2017 (15.9) | right |
| 14.29.30133 | 19.29.30159 | 2019 (16.11) | right |
| 14.44.35207 | 19.44.35228 | 2022 (17.14) | **miscompiled**, 480 of 960 |
| 14.51.36231 | 19.51.36256 | 18 | **miscompiled**, 480 of 960 |

It entered between 19.29 and 19.44; no toolset between them is installed here, so that is as close as this page gets. `/Od` is right on all four.

## Table 2: which flags (toolset 14.51.36231)

"as written" is the reduction below; "`inline`" is the same with the function that holds the two calls declared `inline`, or `__forceinline`, which behaves the same.

| flags | as written | `inline` |
|---|---|---|
| `/O2 /Ob1 /arch:AVX2` (main's `msvc-release`) | right (the function stays out of line) | **miscompiled** |
| the tree's whole release line (above) | right | **miscompiled** |
| `/O2 /Ob2 /arch:AVX2`, and plain `/O2 /arch:AVX2` | **miscompiled** | **miscompiled** |
| `/O2 /Ob3 /arch:AVX2` | **miscompiled** | |
| `/O2 /Ob1`, no `/arch` (the v2 baseline) | right | **miscompiled** |
| `/O2 /Ob2`, no `/arch` | right (by the inliner's choice, not by safety) | |
| `/O2 /Ob1 /arch:SSE4.2`, `/arch:AVX` | | **miscompiled** |
| `/O2 /Ob2 /arch:AVX`, `/arch:AVX512` | **miscompiled** | |
| `/O2 /Ob0` | | right |
| `/O1 /Ob2 /arch:AVX2` | right | |
| `/Od`, with `/Ob0 /RTC1` (`msvc-debug`) or `/Ob1` | right | right |
| `/O2 /Ob2 /arch:AVX2` + `/fp:fast` | 154 of 960 differ, which fast arithmetic alone could explain: not decided (the tree never uses it) | |
| + `/Qvec-`, `/Qpar-`, `/Zc:nrvo-`, `/GS-`, `/Oy-`, `/Gy /Gw` | **miscompiled** | |
| + **`/fp:strict`** | right | right (`/Ob1`) |
| + **`/d2SSAOptimizer-`** (undocumented: the whole SSA optimizer off) | right | right (`/Ob1`) |
| + **`/d2ssa-cse-`** (undocumented: its common-subexpression pass off) | right, also at `/Ob3`, `/arch:AVX512` and with the tree's whole line | right, with and without `/arch`, and `__forceinline` |
| + `/d2ssa-rse-`, `/d2ssa-pre-`, `/d2ssa-cfg-jt-` (its other passes that have a switch) | | **miscompiled** |

So it is not the inlining level, not AVX, not the vectorizer's switch, not the debug information. It needs `/O2`'s optimizer and a function inlined; the three switches that stop it are the rows in bold, and the narrowest points at the pass. That fits the listing, though it is an inference and not something read from the compiler: the second copy's stores are the same values as the first's, so a common-subexpression pass has reason to drop them as duplicates, and the call is left pointing at the copy they were for.

## Table 3: the shape (toolset 14.51.36231, `/O2 /Ob1 /arch:AVX2`, `inline`)

| variation | |
|---|---|
| the argument: three doubles (24 bytes) | **miscompiled** |
| two doubles (16), four doubles (32) | **miscompiled** |
| three floats (12 bytes) | **miscompiled** |
| three 64-bit integers | **miscompiled** |
| two floats (8 bytes: passed in a register, no copy) | right |
| the argument by `const&` | right |
| two *different* callees | **miscompiled** |
| a three-line callee kept out of line with `__declspec(noinline)` | **miscompiled** |
| the two calls in two statements, each result a named local | right |
| the caller's two results not treated alike (one used as it is, the other through `floor`) | right |
| the caller a plain loop that stores each result (no pairing of the two) | right |

**What must be true, then.** (1) An aggregate too large for a register (over 8 bytes; what its members are does not matter) is passed **by value** to **two calls inside one expression**, the same object to both; the callees need not be the same function, only not inlined themselves. (2) The function holding that expression is **inlined** into a caller in which the two results flow into the same arithmetic side by side, so that the optimizer packs them as the two lanes of one vector (`vunpcklpd` of the two return values in the listing). Break either and the code is right. The shape to look for in source is short: a braced pair, a two-argument constructor or one arithmetic expression that calls twice with the same by-value struct, in a function small enough to be inlined: `Vec2{f(seed, p), f(seed + 1, p)}` is its picture.

The second half of the shape is the optimizer's decision and not visible in source, so it was looked for in what the compiler wrote instead.

## Looking for other instances: the listings

**The signature in a listing** (`/FA`): inside one function, a compiler temporary (`$T<n>`) whose address is loaded into an argument register before a call and that nothing in the function writes. On its own that is far too common to read (a return slot looks the same, so does the `this` of a temporary being constructed, so does the empty allocator a `std::string` constructor takes: 25 such temps in `cliff.cpp` alone). What isolates the fault is **the same translation unit compiled twice, as the tree compiles it and with `/d2ssa-cse-`**: a temp that is address-only in the first listing, in a function and before a call where the second has none, is one whose stores that pass removed. Stores count whether made by the temp's name, by another name at the same frame offset, or through `rax` in a function that keeps the entry `rsp` there. On `cliff.cpp` at `/Ob2` that rule gives the two faulted sites (the chart lambda of both box emitters, before `noise3`) and nothing else.

Run over the engine's own library sources (`core/`, `foundation/`, `domain/`, `systems/`; 450 translation units, no tests, benches, apps, game modules or third-party code; each compiled twice to a listing in scratch and deleted, 3 GB a pass, five minutes at eight jobs), 2026-11-19, toolset 14.51.36231:

| | units clean | could not be compiled in this tree | candidates | of which the fault |
|---|---|---|---|---|
| `/O2 /Ob1 /arch:AVX2` (main) | 441 | 4 | 5, each read: an address compared and not passed; two lambdas' return slots; a closure written through `r11`; error strings reached through jumps | **none** |
| `/O2 /Ob2 /arch:AVX2` (`inline`, `full`) | 422 | 4 | 24: `cliff.cpp`; 20 whose callee is a `std::string` or `std::vector` constructor (its allocator argument: an empty object, nothing to write), a vector's own `memset`, or the array-constructor helper, set aside by that callee and not read one by one; two read (an indirect call's return slot, a lambda's return slot); one not resolved (`Document::apply`: the temp is paired, through a jump, with a call that takes no argument) | **`cliff.cpp`, both sites, and no other** |

So, by this detector: **main's release build carries no instance in the engine's library code, and `/Ob2` adds exactly the one the pin found.** What that does not cover: the four units that need generated headers this tree had not built (`systems/animation`'s `graph.cpp` and `animation.cpp`, `systems/muscle_sim`, `domain/deadwood`); tests, benches, the apps and game modules; an argument copy passed on the stack rather than in a register (a fifth argument onward); a frame alias other than `rax` (found by reading, once, `r11`); and a form of the fault that `/d2ssa-cse-` does not undo, if there is one. It is a search that found what it was built from and nothing more, not a proof.

In source, taking every multi-byte argument of a pure function by `const&` removes the first half of the shape wherever it is applied.

## The ways out, and what each costs

Not changed in this row: the tree's flags are as they were. Measured in a scratch configuration of `msvc-release`, the switch added to `CMAKE_CXX_FLAGS_RELWITHDEBINFO` (so third-party code carries it too, which a committed change would not need).

**Pins.** Eleven suites that hold a toolchain-independent answer (`rocks`, `math`, `geometry`, `texture`, `flora`, `landform`, `walls`, `tileset`, `soil`, `matgen`, `world`), built target by target and run with no driver: **11 of 11 pass under each of the three switches on main's flags (`/O2 /Ob1`), no value moved**, `/fp:strict` included, whose different constant folding was the worry. And where the fault was found, `/Ob2` (`ENGINE_MSVC_RELEASE_CODEGEN=inline`): `rocks`' escarpment pin and its whole suite hold again under each of the three. Not run under any of them: `physics`, `content_build`, the two apps' end-to-end suites, anything with a device.

**Cost.** Four short benches, `--repeats=3`, 45 rows, on main's flags, each switch against the median of three takes of the unchanged flags. **The machine was shared throughout** (others' CPU 11 to 100 % at the rows' starts), so the three baseline takes differ among themselves by 15 % as a geometric mean of the rows' spreads (`math` 8 %, `anim` 9 %, `jobs` 6 %, `geometry` 39 %): read `geometry` as noise in every column and the rest as upper bounds.

| | `/d2SSAOptimizer-` | `/fp:strict` | `/d2ssa-cse-` |
|---|---|---|---|
| `math` (17 rows), geometric mean | +52 % | +80 % | +18 % |
| `anim` (10) | +20 % | +14 % | +14 % |
| `jobs` (6) | +5 % | −2 % | −4 % |
| `geometry` (12; its baseline's own spread is 39 %) | +45 % | +19 % | +4 % |
| the rows that moved most | `math.mat4_from_quat` 3.6 times, `mat4_from_transform.aos` 3.1, `anim.skeleton.local_matrices.one` 3.4, `mat4_mul.batch` 2.0 | `math.mat4_mul.batch` 10.8 times, `mat4_mul.chain` 8.3, `transform_point.mat4` 6.3, `transform_aabb` 4.3 | `math.mat4_from_transform.soa` 2.4 times, `mat4_from_quat` 2.2, `mat4_from_transform.aos` 2.1, `anim.skeleton.local_matrices.one` 3.4, `math.transform_inverse` 1.6; those 5 rows of 45 moved by more than a quarter, the other 40 are within 20 % |

- **`/d2SSAOptimizer-`** turns the whole SSA optimizer off: the fault goes and so does a third to a half of the speed of the small-matrix kernels. Too wide.
- **`/fp:strict`** is documented and supported, and it holds every pin run here, but it stops the compiler assuming the default floating-point environment, and the matrix products pay eight to ten times. Not a release setting for this engine.
- **`/d2ssa-cse-`** turns off one pass of that optimizer. It is the cheapest by far, but it is not free (the three `mat4_from_*` rows and `local_matrices` above are real: their baseline takes agree to 3 %), it is **undocumented**, so a later toolset may drop or rename it (an unknown `/d2` switch is a fatal error, C1007, so that at least is loud), and it stops the fault because of where the fault is, not because anyone at the vendor said so.
- **A toolset without the fault**: 14.29 is installed and right (table 1), and is five years of optimizer and of C++20 behind what the tree is built and measured with; not tried on the tree.
- **In source**, where the shape is found: the argument by `const&`, or the two calls in two statements. Certain where applied, and only there.

None of these was chosen here. What the measurements say: if a switch, `/d2ssa-cse-` on the engine's own targets, with the canary's second case as a `CHECK` to say the day it stops working; and the vendor's fix is the only way out that costs nothing.

## The reduction

`cl /O2 /Ob2 /arch:AVX2 repro.cpp` prints `480 of 960 values differ: MISCOMPILED`; with `/DREPRO_INLINE` so does `cl /O2 /Ob1 repro.cpp`. It is plain C++ apart from the one `__declspec(noinline)`.

```cpp
// Reduced from domain/rocks/src/cliff.cpp (weathered_uv inside emit_box's chart_of): an aggregate passed by value to two consecutive calls of a function the compiler
// does not inline. cl /O2 /Ob2 /arch:AVX2: the second call's copy of `q` is never written.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using f64 = double;
using u64 = std::uint64_t;
using i64 = std::int64_t;
using u32 = std::uint32_t;

struct D3 {
  f64 x = 0.0;
  f64 y = 0.0;
  f64 z = 0.0;
};
inline f64 lerp(f64 a, f64 b, f64 t) { return a + (b - a) * t; }
inline u64 mix64(u64 x) {
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBull;
  x ^= x >> 31;
  return x;
}
inline f64 lattice_value(u64 seed, i64 i, i64 j, i64 k) {
  const u64 h = mix64(
      seed ^ mix64(static_cast<u64>(i) + mix64(static_cast<u64>(j) + mix64(static_cast<u64>(k)))));
  return static_cast<f64>(h >> 11) * (1.0 / 9007199254740992.0);
}
inline f64 fade(f64 t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
inline f64 noise3(u64 seed, D3 p) {
  const f64 fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
  const i64 i = static_cast<i64>(fx), j = static_cast<i64>(fy), k = static_cast<i64>(fz);
  const f64 tx = fade(p.x - fx), ty = fade(p.y - fy), tz = fade(p.z - fz);
  f64 c[2][2];
  for (i64 b = 0; b < 2; ++b) {
    for (i64 a = 0; a < 2; ++a)
      c[b][a] =
          lerp(lattice_value(seed, i, j + a, k + b), lattice_value(seed, i + 1, j + a, k + b), tx);
  }
  return lerp(lerp(c[0][0], c[0][1], ty), lerp(c[1][0], c[1][1], ty), tz);
}

namespace {

struct ChartUv {
  f64 a = 0.0;
  f64 b = 0.0;
};

// REPRO_INLINE / REPRO_FORCEINLINE: make it a function /Ob1 inlines too.
#if defined(REPRO_FORCEINLINE)
#define REPRO_KW __forceinline
#elif defined(REPRO_INLINE)
#define REPRO_KW inline
#else
#define REPRO_KW
#endif
REPRO_KW ChartUv weathered_uv(ChartUv uv, u64 seed, f64 amount) {
  const D3 q{uv.a / 7.0, uv.b / 7.0, 0.25};
  return {uv.a + amount * (noise3(seed, q) - 0.5), uv.b + amount * (noise3(seed + 1u, q) - 0.5)};
}

// The same function, kept out of line: what the source means.
__declspec(noinline) ChartUv weathered_uv_reference(ChartUv uv, u64 seed, f64 amount) {
  const D3 q{uv.a / 7.0, uv.b / 7.0, 0.25};
  return {uv.a + amount * (noise3(seed, q) - 0.5), uv.b + amount * (noise3(seed + 1u, q) - 0.5)};
}

template <class Uv>
void charts(u32 nb, u32 nc, const Uv& uv, f64 tile_m, std::vector<f64>& out) {
  const auto chart_of = [&](u32 p, u32 q, f64& ma, f64& mb) {
    ChartUv middle;
    for (u32 dy = 0; dy <= 1; ++dy) {
      for (u32 dx = 0; dx <= 1; ++dx) {
        const ChartUv m = uv(p + dx, q + dy);
        middle.a += 0.25 * m.a;
        middle.b += 0.25 * m.b;
      }
    }
    ma = middle.a / tile_m;
    mb = middle.b / tile_m;
  };
  for (u32 q = 0; q < nc; ++q) {
    for (u32 p = 0; p < nb; ++p) {
      f64 ma = 0.0, mb = 0.0;
      chart_of(p, q, ma, mb);
      out.push_back(ma);
      out.push_back(mb);
    }
  }
}

}  // namespace

int main(int argc, char**) {
  const u32 nb = 40, nc = 12;
  const f64 irregularity = 0.35 + 0.0 * argc;
  const u64 seed = 0x1234567ull + static_cast<u64>(argc);
  std::vector<f64> s(nb + 2), y(nc + 2);
  for (u32 i = 0; i < s.size(); ++i) s[i] = 0.61 * i;
  for (u32 j = 0; j < y.size(); ++j) y[j] = 0.478 * j - 2.0;
  const auto inlined = [&](u32 i, u32 j) {
    const ChartUv uv{s[i], y[j]};
    return weathered_uv(uv, seed, 2.0 * irregularity);
  };
  const auto reference = [&](u32 i, u32 j) {
    const ChartUv uv{s[i], y[j]};
    return weathered_uv_reference(uv, seed, 2.0 * irregularity);
  };
  std::vector<f64> got, want;
  charts(nb, nc, inlined, 2.0, got);
  charts(nb, nc, reference, 2.0, want);
  u32 differ = 0;
  for (std::size_t i = 0; i < got.size(); ++i)
    if (std::memcmp(&got[i], &want[i], 8) != 0) ++differ;
  std::printf("%u of %zu values differ: %s\n", differ, got.size(), differ == 0 ? "ok" : "MISCOMPILED");
  if (differ != 0) std::printf("first pair: got (%.17g, %.17g) want (%.17g, %.17g)\n", got[0], got[1], want[0], want[1]);
  return differ == 0 ? 0 : 1;
}
```

## The canary

`core/base/tests/compiler_canary_tests.cpp` is the reduction as a test, compiled like every test with the build's own flags, so it answers for *this* build: whether its compiler and its flags reach the fault.

- "an aggregate by value to two calls in one expression is copied twice": the function is not declared `inline`. It computes the 960 values through it and through a copy that is never inlined and makes its calls in two statements, and checks the first against the second and the second against a pinned FNV-1a of its bytes (14465588424151197931: arithmetic and `floor` only, so the same from every toolchain; **taken on MSVC, `/Od` and `/O2`, and not yet run on GCC or Clang**). It passes under `/Ob1` and **fails under `/Ob2`** on a compiler with the fault, naming the compiler's version.
- "the same through a function declared inline (reported, not failed)": the form main's flags reach. A doctest `WARN`, so every `msvc-release` log says the build's compiler has the fault without the suite going red for something the tree has no answer to yet. The row that lands a workaround turns it into a `CHECK`: with the workaround in, it must pass.

Shown once in the tree, 2026-11-19, building only `engine_base_tests`, with no driver: under `-DENGINE_MSVC_RELEASE_CODEGEN=inline` the first case fails ("MSVC 195136256 miscompiles this build: 480 of 960 values are not the scalar answer") and CTest's `base` fails with it; under `full` the same; under `cmake` both cases pass, the second with its warning ("miscompiles the inline form in this build: 480 of 960 values"), and `base` passes; in `msvc-debug` both pass with no warning. So on a tree whose default is `full` this test is red by design, and it is green on main's flags.

## What it means

- **For today's binaries.** Every `/O2` build of the tree with 14.44 or 14.51 can carry the fault wherever the shape occurs with an `inline` function, a member function defined in its class, or a template, which is most of the engine's hot code. One instance is known and it is *not* reached on main (`weathered_uv` is not inline, and its pin holds under `/Ob1`). The listings of the 446 library units that compile here show no other on main's flags ("Looking for other instances: the listings"), within what that search can see; the determinism suites hold on main, which covers the builders they pin.
- **For `/Ob2` (`ENGINE_MSVC_RELEASE_CODEGEN=full`) as the default.** T193's sentence "`/Ob` changes which calls are inlined, never the arithmetic" is true of the language and false of this compiler. `/Ob2` makes every unmarked small function a candidate, so it widens the exposure; it should not be the default again before a workaround is in and the canary's second case is a `CHECK` that passes.
- **For a report to the vendor.** This page is written to be attached as it stands: the versions, the flags, the reduction and the listing are all here. Whether to send it is the owner's decision.
- **Not established here:** the first toolset with the fault (between 14.29 and 14.44); whether `clang-cl` is affected (not installed here; GCC and Clang on Linux reproduce the pin `rocks` holds, so their code for the found instance is right); an instance outside the library code the listings were read for (tests, benches, apps, game modules, third party).
