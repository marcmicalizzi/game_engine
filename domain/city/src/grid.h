#pragma once

// The integer arithmetic the plan and the grammar share (docs/subsystems/city.md, "Determinism"):
// seeded draws through the engine's hash, a 64-step Q14 turn table, Q16 fractions and snapping to
// the module. Private to the module, so there is one copy of each and the plan and the grammar
// cannot drift apart by reimplementing one slightly differently.

#include <core/base/types.h>
#include <core/hash/hash.h>
#include <domain/city/plan.h>

#include <algorithm>

namespace engine::city::grid {

// Every choice draws from a seed, a constant naming what the draw is for, and an index, so a draw
// added for one purpose moves no other.
inline u64 draw(u64 seed, u64 purpose, u64 index) noexcept {
  return hash_combine(hash_combine(seed, purpose), index);
}

// A value in [0, n) from the draw's top 32 bits (multiply-shift).
inline u32 pick(u64 value, u32 n) noexcept {
  return static_cast<u32>(((value >> 32) * static_cast<u64>(n)) >> 32);
}

// A Q16 fraction in [0, 1) from the draw's low 16 bits.
inline i32 unit_q(u64 value) noexcept { return static_cast<i32>(value & 0xffffu); }

// True with probability q (Q16).
inline bool chance(u64 value, i32 q) noexcept { return unit_q(value) < q; }

// A value in [lo, hi] from a draw.
inline i32 between(u64 value, i32 lo, i32 hi) noexcept {
  if (hi <= lo) return lo;
  return lo + static_cast<i32>(pick(value, static_cast<u32>(hi - lo + 1)));
}

// x * q (Q16), rounded to nearest, on 64 bits.
inline i64 mul_q(i64 x, i64 q) noexcept { return (x * q + (x >= 0 ? 32768 : -32768)) / 65536; }

// cos of k/64 of a turn, Q14. A table and not the C library: the coastline and the mountain axis
// are decisions, and `cos` is the library's choice.
inline constexpr i32 k_cos64[64] = {
    16384,  16305,  16069,  15679,  15137,  14449,  13623,  12665,  11585,  10394,  9102,
    7723,   6270,   4756,   3196,   1606,   0,      -1606,  -3196,  -4756,  -6270,  -7723,
    -9102,  -10394, -11585, -12665, -13623, -14449, -15137, -15679, -16069, -16305, -16384,
    -16305, -16069, -15679, -15137, -14449, -13623, -12665, -11585, -10394, -9102,  -7723,
    -6270,  -4756,  -3196,  -1606,  0,      1606,   3196,   4756,   6270,   7723,   9102,
    10394,  11585,  12665,  13623,  14449,  15137,  15679,  16069,  16305};
constexpr i32 cos64(u32 step) noexcept { return k_cos64[step & 63u]; }
constexpr i32 sin64(u32 step) noexcept { return k_cos64[(step + 48u) & 63u]; }

// Snaps down, to nearest and up to a multiple of `m` (floor division: correct for negatives).
inline i32 floor_to(i32 v, i32 m) noexcept {
  const i32 q = v / m;
  return (q * m > v ? q - 1 : q) * m;
}
inline i32 round_to(i32 v, i32 m) noexcept { return floor_to(v + m / 2, m); }
inline i32 ceil_to(i32 v, i32 m) noexcept { return -floor_to(-v, m); }

inline i32 floor_div(i64 v, i64 m) noexcept {
  const i64 q = v / m;
  return static_cast<i32>(q * m > v ? q - 1 : q);
}

inline bool intersects(const Rect& a, const Rect& b) noexcept {
  return a.x0 < b.x1 && b.x0 < a.x1 && a.z0 < b.z1 && b.z0 < a.z1;
}
inline bool contains(const Rect& outer, const Rect& inner) noexcept {
  return inner.x0 >= outer.x0 && inner.z0 >= outer.z0 && inner.x1 <= outer.x1 &&
         inner.z1 <= outer.z1;
}

// Squared distance from a point to a rectangle (0 inside), on 64 bits.
inline i64 dist2_to_rect(i64 x, i64 z, const Rect& r) noexcept {
  const i64 dx = x < r.x0 ? r.x0 - x : (x > r.x1 ? x - r.x1 : 0);
  const i64 dz = z < r.z0 ? r.z0 - z : (z > r.z1 ? z - r.z1 : 0);
  return dx * dx + dz * dz;
}

// Quarter turns: 0 +x, 1 -z, 2 -x, 3 +z.
inline constexpr i32 k_dx[4] = {1, 0, -1, 0};
inline constexpr i32 k_dz[4] = {0, -1, 0, 1};

}  // namespace engine::city::grid
