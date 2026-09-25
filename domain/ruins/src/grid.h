#pragma once

// The integer arithmetic the assembler and the block layer share (docs/subsystems/ruins.md, "Why
// this shape"): seeded draws through the engine's hash, sixteenths of a turn as a Q14 table, the
// footprint's quarter turns, and a wall's box on the centimetre grid. Private to the module — a
// building is a function of these, so there is one copy of each, and the block layer cannot drift
// from the assembler by reimplementing one of them slightly differently.

#include <core/base/types.h>
#include <core/hash/hash.h>
#include <domain/ruins/assembler.h>

#include <algorithm>
#include <cmath>

namespace engine::ruins::grid {

// Every choice draws from the building's seed, a constant naming what the draw is for, and an
// index, so adding a draw for one purpose moves no other.
inline u64 draw(u64 seed, u64 purpose, u64 index) noexcept {
  return hash_combine(hash_combine(seed, purpose), index);
}

// A value in [0, n) from the draw's top 32 bits: multiply-shift, so there is no division and no
// modulo bias worth the name.
inline u32 pick(u64 value, u32 n) noexcept {
  return static_cast<u32>(((value >> 32) * static_cast<u64>(n)) >> 32);
}

// cos of k * 22.5 degrees, Q14. It turns a building's centimetres into the world's, which is part
// of every decision downstream of it, so it is a table and not the C library.
inline constexpr i32 k_cos14[16] = {16384,  15137,  11585,  6270,  0, -6270, -11585, -15137,
                                    -16384, -15137, -11585, -6270, 0, 6270,  11585,  15137};
constexpr i32 cos14(u32 step) noexcept { return k_cos14[step & 15u]; }
constexpr i32 sin14(u32 step) noexcept { return k_cos14[(step + 12u) & 15u]; }

// Rotation about +y by `step` sixteenths: (x, z) -> (c x + s z, -s x + c z), rounded to the
// nearest centimetre (C++20 defines >> on a negative value as the arithmetic shift).
inline void rotate_cm(u32 step, i64 x, i64 z, i64& out_x, i64& out_z) noexcept {
  const i64 c = cos14(step);
  const i64 s = sin14(step);
  out_x = (c * x + s * z + 8192) >> 14;
  out_z = (-s * x + c * z + 8192) >> 14;
}

// Quarter turns: 0 +x, 1 -z, 2 -x, 3 +z. A left turn seen from above is +1, a right turn -1.
inline constexpr i32 k_dx[4] = {1, 0, -1, 0};
inline constexpr i32 k_dz[4] = {0, -1, 0, 1};
constexpr u8 left_of(u8 q) noexcept { return static_cast<u8>((q + 1u) & 3u); }
constexpr u8 right_of(u8 q) noexcept { return static_cast<u8>((q + 3u) & 3u); }

inline i64 isqrt_ceil(i64 v) noexcept {
  if (v <= 0) return 0;
  i64 r = static_cast<i64>(std::sqrt(static_cast<f64>(v)));
  while (r * r > v)
    --r;
  while (r * r < v)
    ++r;
  return r;
}

// The axis-aligned box a wall occupies, centre line +/- half its thickness, and past each vertex
// by as much, so that it covers the corner squares.
struct Rect {
  i32 x0, z0, x1, z1;
};

inline Rect side_rect(const Assembler::Side& s, i32 half) noexcept {
  const i32 x1 = s.x0 + k_dx[s.dir] * s.length_cm;
  const i32 z1 = s.z0 + k_dz[s.dir] * s.length_cm;
  return Rect{std::min(s.x0, x1) - half, std::min(s.z0, z1) - half, std::max(s.x0, x1) + half,
              std::max(s.z0, z1) + half};
}

inline bool overlaps(const Rect& a, const Rect& b) noexcept {
  return a.x0 < b.x1 && b.x0 < a.x1 && a.z0 < b.z1 && b.z0 < a.z1;
}

// A piece of debris of radius `r` that landed at (x, z) in the building's frame, pushed out of
// every wall across the wall it landed in to the nearer face, as often as it takes: false when it
// cannot be put clear. The assembler's debris and the block layer's fallen blocks settle the same
// way.
inline bool settle_clear(const Vector<Assembler::Side>& sides, i32 half, i32 r, i32& x,
                         i32& z) noexcept {
  bool clear = false;
  for (u32 pass = 0; pass <= sides.size() && !clear; ++pass) {
    clear = true;
    for (const Assembler::Side& other : sides) {
      const Rect rc = side_rect(other, half);
      if (x <= rc.x0 - r || x >= rc.x1 + r || z <= rc.z0 - r || z >= rc.z1 + r) continue;
      clear = false;
      if ((other.dir & 1u) == 0) {  // along x: out across z
        z = z - (rc.z0 - r) < (rc.z1 + r) - z ? rc.z0 - r - 1 : rc.z1 + r + 1;
      } else {
        x = x - (rc.x0 - r) < (rc.x1 + r) - x ? rc.x0 - r - 1 : rc.x1 + r + 1;
      }
    }
  }
  return clear;
}

inline f32 metres(i64 cm) noexcept { return static_cast<f32>(cm) * 0.01f; }

}  // namespace engine::ruins::grid
