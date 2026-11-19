// A compiler canary (docs/experiments/msvc-ob2-avx2-miscompile.md): the shape of code MSVC 19.44
// and 19.51 miscompile at /O2, reduced from `domain/rocks`' cliff solver, where it was found by a
// determinism pin and by nothing else. An aggregate passed in memory, by value, to two calls in one
// expression, whose two results the optimizer then pairs: once the function holding the two calls
// is inlined, the second call's copy of the argument is never written and the callee reads stale
// stack. It is silent: no warning, no crash, a plausible number.
//
// Compiled with the tree's own flags, like every test, so it says whether *this* build's compiler
// and flags reach the fault: it passes where `weathered` stays out of line (`/Ob1`, since it is not
// declared inline) and fails where the build inlines it (`/Ob2`:
// `-DENGINE_MSVC_RELEASE_CODEGEN=inline` or `full`). A change of flags or of compiler that walks
// into the fault fails here first, by name, instead of in whichever pin happens to cover it.
#include <core/base/macros.h>
#include <core/base/types.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace engine;

namespace {

struct Point3 {
  f64 x = 0.0;
  f64 y = 0.0;
  f64 z = 0.0;
};
struct Pair {
  f64 a = 0.0;
  f64 b = 0.0;
};

inline f64 mix(f64 a, f64 b, f64 t) { return a + (b - a) * t; }
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

// Value noise: IEEE `+ - * /` and floor only, so its answer is the same bits on every toolchain.
// The argument is by value on purpose: 24 bytes, passed in memory, which is what the fault needs.
inline f64 noise3(u64 seed, Point3 p) {
  const f64 fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
  const i64 i = static_cast<i64>(fx), j = static_cast<i64>(fy), k = static_cast<i64>(fz);
  const f64 tx = fade(p.x - fx), ty = fade(p.y - fy), tz = fade(p.z - fz);
  f64 c[2][2];
  for (i64 b = 0; b < 2; ++b) {
    for (i64 a = 0; a < 2; ++a)
      c[b][a] =
          mix(lattice_value(seed, i, j + a, k + b), lattice_value(seed, i + 1, j + a, k + b), tx);
  }
  return mix(mix(c[0][0], c[0][1], ty), mix(c[1][0], c[1][1], ty), tz);
}

// The two calls in one expression. **Not declared inline, on purpose**: `/Ob1` leaves it out of
// line and the build is right; `/Ob2` inlines it and, on a compiler with the fault, is wrong.
Pair weathered(Pair uv, u64 seed, f64 amount) {
  const Point3 q{uv.a / 7.0, uv.b / 7.0, 0.25};
  return {uv.a + amount * (noise3(seed, q) - 0.5), uv.b + amount * (noise3(seed + 1u, q) - 0.5)};
}
// The same, declared inline: what `/Ob1` inlines too, which is main's release flags.
inline Pair weathered_inline(Pair uv, u64 seed, f64 amount) {
  const Point3 q{uv.a / 7.0, uv.b / 7.0, 0.25};
  return {uv.a + amount * (noise3(seed, q) - 0.5), uv.b + amount * (noise3(seed + 1u, q) - 0.5)};
}
// The same, never inlined, its calls in two statements: what the source means.
ENGINE_NO_INLINE Pair weathered_reference(Pair uv, u64 seed, f64 amount) {
  const Point3 q{uv.a / 7.0, uv.b / 7.0, 0.25};
  const f64 first = noise3(seed, q);
  const f64 second = noise3(seed + 1u, q);
  return {uv.a + amount * (first - 0.5), uv.b + amount * (second - 0.5)};
}

// Each cell's mean of its four corners, both fields alike: the pairing the fault needs.
template <class Uv>
void middles(u32 columns, u32 rows, const Uv& uv, f64 tile, std::vector<f64>& out) {
  const auto middle_of = [&](u32 p, u32 q, f64& ma, f64& mb) {
    Pair middle;
    for (u32 dy = 0; dy <= 1; ++dy) {
      for (u32 dx = 0; dx <= 1; ++dx) {
        const Pair m = uv(p + dx, q + dy);
        middle.a += 0.25 * m.a;
        middle.b += 0.25 * m.b;
      }
    }
    ma = middle.a / tile;
    mb = middle.b / tile;
  };
  for (u32 q = 0; q < rows; ++q) {
    for (u32 p = 0; p < columns; ++p) {
      f64 ma = 0.0, mb = 0.0;
      middle_of(p, q, ma, mb);
      out.push_back(ma);
      out.push_back(mb);
    }
  }
}

u32 differing(const std::vector<f64>& got, const std::vector<f64>& want) {
  u32 n = got.size() == want.size() ? 0u : 1u;
  for (usize i = 0; i < got.size() && i < want.size(); ++i)
    if (std::memcmp(&got[i], &want[i], sizeof(f64)) != 0) ++n;
  return n;
}

u64 fnv1a(const std::vector<f64>& values) {
  u64 h = 14695981039346656037ull;
  for (const f64 v : values) {
    unsigned char bytes[sizeof(f64)];
    std::memcpy(bytes, &v, sizeof(f64));
    for (const unsigned char c : bytes) {
      h ^= c;
      h *= 1099511628211ull;
    }
  }
  return h;
}

std::string compiler() {
#if ENGINE_COMPILER_MSVC
  return "MSVC " + std::to_string(_MSC_FULL_VER);
#else
  return __VERSION__;
#endif
}

// 40 by 12 cells, 960 values. The inputs come through volatiles so that nothing is folded.
struct Canary {
  std::vector<f64> plain;      // through `weathered`
  std::vector<f64> inlined;    // through `weathered_inline`
  std::vector<f64> reference;  // through `weathered_reference`
};
Canary run_canary() {
  constexpr u32 k_columns = 40, k_rows = 12;
  volatile f64 one = 1.0;
  volatile u64 bump = 1;
  const f64 irregularity = 0.35 * one;
  const u64 seed = 0x1234567ull + bump;
  std::vector<f64> along(k_columns + 2), up(k_rows + 2);
  for (u32 i = 0; i < along.size(); ++i)
    along[i] = 0.61 * static_cast<f64>(i);
  for (u32 j = 0; j < up.size(); ++j)
    up[j] = 0.478 * static_cast<f64>(j) - 2.0;
  Canary c;
  middles(
      k_columns, k_rows,
      [&](u32 i, u32 j) { return weathered(Pair{along[i], up[j]}, seed, 2.0 * irregularity); }, 2.0,
      c.plain);
  middles(
      k_columns, k_rows,
      [&](u32 i, u32 j) {
        return weathered_inline(Pair{along[i], up[j]}, seed, 2.0 * irregularity);
      },
      2.0, c.inlined);
  middles(
      k_columns, k_rows,
      [&](u32 i, u32 j) {
        return weathered_reference(Pair{along[i], up[j]}, seed, 2.0 * irregularity);
      },
      2.0, c.reference);
  return c;
}

}  // namespace

TEST_CASE("compiler canary: an aggregate by value to two calls in one expression is copied twice") {
  // The answer, the same bits from every toolchain: FNV-1a over the 960 doubles.
  constexpr u64 k_answer = 14465588424151197931ull;
  const Canary c = run_canary();
  REQUIRE(c.reference.size() == 960);
  CHECK(fnv1a(c.reference) == k_answer);
  const u32 wrong = differing(c.plain, c.reference);
  CHECK_MESSAGE(wrong == 0, compiler()
                                << " miscompiles this build: " << wrong
                                << " of 960 values are not the scalar answer. The second call's "
                                   "copy of a by-value aggregate is not written once the caller is "
                                   "inlined; see docs/experiments/msvc-ob2-avx2-miscompile.md "
                                   "before changing a flag to make this pass");
}

// The shape main's release flags already reach: the same function declared inline, which `/Ob1`
// inlines. A warning and not a failure while the tree has no workaround (the page, "What it
// means"): it says in every release log that this build's compiler has the fault. The row that
// lands the workaround turns this into a CHECK.
TEST_CASE("compiler canary: the same through a function declared inline (reported, not failed)") {
  const Canary c = run_canary();
  const u32 wrong = differing(c.inlined, c.reference);
  WARN_MESSAGE(wrong == 0, compiler() << " miscompiles the inline form in this build: " << wrong
                                      << " of 960 values; docs/experiments/"
                                         "msvc-ob2-avx2-miscompile.md");
}
