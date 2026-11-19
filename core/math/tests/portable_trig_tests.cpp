// The portable sin, cos and atan2 (core/math/portable_trig.h): a table of their outputs pinned by
// hash, and a few exact values. The table's inputs are made from integers by exact or correctly
// rounded IEEE arithmetic, so they are the same bits everywhere; the hash was taken on MSVC when
// the two moved here from engine-view (2026-11-19), and GCC and Clang must reproduce it. A failure
// on one toolchain and not another is a bug in the functions (a C library call, a contraction the
// tree's flags let through), not a value to replace. Engine-view's `fly_sin_cos` and `fly_atan2`
// forward to these, so its recorded sessions' trajectories pin the same functions a second way.
#include <core/hash/hash.h>
#include <core/math/portable_trig.h>

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace engine;

namespace {

// The hash of every output over the table: angles from -40 to 40 radians in steps of 1/64 (the
// walker's and the camera's reach and beyond), and atan2 over a 65 x 65 grid of (y, x) in [-8, 8]
// by eighths, axes and origin included.
u64 table_hash() {
  u64 h = k_hash_seed;
  for (i32 i = -40 * 64; i <= 40 * 64; ++i) {
    const f32 angle = static_cast<f32>(i) / 64.0f;
    f32 s = 0.0f;
    f32 c = 0.0f;
    portable_sin_cos(angle, s, c);
    h = hash_combine(h, std::bit_cast<u32>(s));
    h = hash_combine(h, std::bit_cast<u32>(c));
  }
  for (i32 yi = -32; yi <= 32; ++yi) {
    for (i32 xi = -32; xi <= 32; ++xi) {
      const f32 a = portable_atan2(static_cast<f32>(yi) / 4.0f, static_cast<f32>(xi) / 4.0f);
      h = hash_combine(h, std::bit_cast<u32>(a));
    }
  }
  return h;
}

}  // namespace

TEST_CASE("portable trig: the table's outputs are pinned (MSVC; GCC and Clang reproduce it)") {
  // Taken on MSVC (msvc-debug, x86-64-v3) on 2026-11-19, when the two moved from engine-view.
  constexpr u64 k_pinned = 0x7d3ed0e441d8cd6dull;
  const u64 h = table_hash();
  if (h != k_pinned)
    std::printf("portable trig table hash: 0x%016llx\n", static_cast<unsigned long long>(h));
  CHECK(h == k_pinned);
}

TEST_CASE("portable trig: exact values and the edges") {
  f32 s = 1.0f;
  f32 c = 0.0f;
  portable_sin_cos(0.0f, s, c);
  CHECK(s == 0.0f);
  CHECK(c == 1.0f);
  // Not a turn's angle, or not a number: the answer for zero, never a loop that does not end.
  for (const f32 bad : {std::numeric_limits<f32>::infinity(), std::numeric_limits<f32>::quiet_NaN(),
                        2.0e6f, -2.0e6f}) {
    portable_sin_cos(bad, s, c);
    CHECK(s == 0.0f);
    CHECK(c == 1.0f);
  }
  CHECK(portable_atan2(0.0f, 0.0f) == 0.0f);
  CHECK(portable_atan2(std::numeric_limits<f32>::quiet_NaN(), 1.0f) == 0.0f);
  CHECK(portable_atan2(0.0f, 1.0f) == 0.0f);
  CHECK(portable_atan2(1.0f, 0.0f) == static_cast<f32>(1.57079632679489661923));
  CHECK(portable_atan2(0.0f, -1.0f) == static_cast<f32>(3.14159265358979323846));
  CHECK(portable_atan2(1.0f, 1.0f) == static_cast<f32>(0.78539816339744830962));
  // Against the C library, to within an f32 ulp or two (the library is not the reference for the
  // bits, only for the value).
  for (i32 i = -400; i <= 400; ++i) {
    const f32 angle = static_cast<f32>(i) / 64.0f;
    portable_sin_cos(angle, s, c);
    CHECK(std::fabs(s - std::sin(angle)) <= 2.4e-7f);
    CHECK(std::fabs(c - std::cos(angle)) <= 2.4e-7f);
    const f32 a = portable_atan2(std::sin(angle), std::cos(angle));
    CHECK(std::fabs(a - std::atan2(std::sin(angle), std::cos(angle))) <= 4.8e-7f);
  }
}
