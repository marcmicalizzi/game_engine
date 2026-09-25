#pragma once

// The terrain generator's integer arithmetic (docs/subsystems/terrain.md, "Integer in every
// decision"). Everything the dune field decides — where a primitive sits, how high it is, how far
// the wind has moved it by time t, what the surface is at a point — is computed here in integers:
// positions in millimetres (seeded choices are made in centimetres and scaled), heights in
// micrometres, fractions in Q16 (65536 is one) or Q15 for a sine, directions as binary angles
// (65536 to a turn) and unit vectors in Q14. So the field is the same bits on MSVC, GCC and Clang
// at either CPU baseline, with no dependence on the C library's `sin` or on the compiler's float
// contraction (ADR-0035), and the golden hashes in tests/terrain_tests.cpp hold on all of them.
// Floats appear only in the output a renderer reads: a height in metres, a normal, a crest line.
//
// **The sine is a table built at compile time by integer arithmetic** (a Taylor series in Q30,
// evaluated by the compiler in `constexpr`), so no two toolchains can disagree about it: a constant
// expression over integers has one value in C++.

#include <core/base/types.h>

namespace engine::terrain::fx {

inline constexpr i32 k_one_q16 = 65536;
inline constexpr i32 k_one_q15 = 32768;
inline constexpr i32 k_one_q14 = 16384;
// A binary angle: 65536 to a turn, so a quarter turn is 16384 and wrapping is `& 0xFFFF`.
inline constexpr u32 k_turn = 65536;
inline constexpr u32 k_quarter_turn = 16384;

// Division and remainder rounding towards negative infinity, for a positive divisor: the lattice
// cell of a coordinate and the day of a time must not change their rounding at zero.
constexpr i64 floor_div(i64 a, i64 b) noexcept {
  const i64 q = a / b;
  return (a % b != 0 && a < 0) ? q - 1 : q;
}
constexpr i64 floor_mod(i64 a, i64 b) noexcept { return a - floor_div(a, b) * b; }

constexpr i64 clamp_i64(i64 v, i64 lo, i64 hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
constexpr i64 abs_i64(i64 v) noexcept { return v < 0 ? -v : v; }
constexpr i64 min_i64(i64 a, i64 b) noexcept { return a < b ? a : b; }
constexpr i64 max_i64(i64 a, i64 b) noexcept { return a > b ? a : b; }

// sin and cos of a binary angle, in Q15 ([-32768, 32768]): a quarter-wave table of 1,025 entries
// with linear interpolation between them, which is within 2 of the true value in Q15 everywhere.
i32 sin_q15(u32 angle) noexcept;
inline i32 cos_q15(u32 angle) noexcept { return sin_q15(angle + k_quarter_turn); }

// The largest r with r * r <= v, exactly.
u64 isqrt(u64 v) noexcept;

// The length of (x, z), rounded down; exact for any pair whose squares sum below 2^63.
inline i64 length(i64 x, i64 z) noexcept {
  return static_cast<i64>(isqrt(static_cast<u64>(x * x + z * z)));
}

// The smooth falloff 1 - 3s^2 + 2s^3 of s in Q16, clamped to [0, 1]: 1 at 0, 0 at 1, zero slope
// at both ends. Every profile of the dune field is built from it, so the surface has continuous
// slopes wherever a profile does not ask for a crease (a slip face's brink and toe do).
constexpr i64 falloff_q16(i64 s) noexcept {
  if (s <= 0) return k_one_q16;
  if (s >= k_one_q16) return 0;
  const i64 s2 = (s * s) >> 16;
  const i64 s3 = (s2 * s) >> 16;
  return k_one_q16 - 3 * s2 + 2 * s3;
}

// A hash's top bits as a Q16 fraction in [0, 1).
constexpr i64 unit_q16(u64 h) noexcept { return static_cast<i64>(h >> 48); }
// lo + (hi - lo) * unit: a seeded draw in a range, integer.
constexpr i64 draw(u64 h, i64 lo, i64 hi) noexcept {
  return lo + (((hi - lo) * unit_q16(h)) >> 16);
}

}  // namespace engine::terrain::fx
