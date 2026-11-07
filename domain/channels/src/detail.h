#pragma once

// What the channels' sources share: the range checks, the hydraulic-geometry power law and the
// arithmetic every decision is made in, with no C library call (core/math/deterministic.h).
#include <core/hash/hash.h>
#include <core/math/deterministic.h>
#include <domain/channels/channels.h>

#include <cmath>
#include <string>

namespace engine::channels::detail {

inline constexpr f64 k_pi = 3.141592653589793;

inline bool fail(std::string& error, std::string_view field) {
  error = "engine.channels: invalid " + std::string(field);
  return false;
}
inline bool in(f64 v, f64 lo, f64 hi) { return v >= lo && v <= hi; }  // a NaN fails
inline f64 wide(f32 v) { return static_cast<f64>(v); }
inline f64 powd(f64 x, f64 y) { return x <= 0 ? 0.0 : det::exp(y * det::log(x)); }
inline f64 smoothstep(f64 lo, f64 hi, f64 v) {
  const f64 t = hi > lo ? (v - lo) / (hi - lo) : (v >= hi ? 1.0 : 0.0);
  const f64 c = t < 0 ? 0.0 : (t > 1 ? 1.0 : t);
  return c * c * (3 - 2 * c);
}
// Downstream hydraulic geometry, `a (A / 1 km^2)^b` (Leopold & Maddock 1953).
inline f64 hydraulic(f64 at_km2, f64 exponent, f64 area_m2) {
  return at_km2 * powd(area_m2 / 1e6, exponent);
}
inline f64 round_to(f64 v, f64 scale) { return std::round(v * scale) / scale; }
inline i64 floor_div(i64 a, i64 b) {
  const i64 q = a / b;
  return q - (a % b < 0 ? 1 : 0);
}
inline u32 neighbour(u32 i, u32 w, u32 h, i32 dx, i32 dz) {
  const i32 x = static_cast<i32>(i % w) + dx;
  const i32 z = static_cast<i32>(i / w) + dz;
  return x < 0 || z < 0 || x >= static_cast<i32>(w) || z >= static_cast<i32>(h)
             ? ~0u
             : static_cast<u32>(z) * w + static_cast<u32>(x);
}

// splitmix64; a draw's float is its top 53 bits scaled by a power of two, exact on every target.
struct Rng {
  u64 state;
  explicit Rng(u64 seed) : state(seed) {}
  u64 next() {
    state += 0x9E3779B97F4A7C15ull;
    return mix64(state);
  }
  f64 unit() { return static_cast<f64>(next() >> 11) * (1.0 / 9007199254740992.0); }
};
// A hash of a world place to [-1, 1]: the bed's relief at a vertex, the same in every piece that
// holds the vertex.
inline f64 place_noise(u64 seed, i64 x_mm, i64 z_mm) {
  const u64 h = hash_combine(hash_combine(seed, static_cast<u64>(x_mm)), static_cast<u64>(z_mm));
  return static_cast<f64>(mix64(h) >> 11) * (2.0 / 9007199254740992.0) - 1.0;
}

inline bool surface_in_range(const ChannelSurface& s, std::string& error) {
  if (!in(wide(s.tile_m), .05, 20)) return fail(error, "surface tile_m");
  if (!in(wide(s.roughness), .02, 1)) return fail(error, "surface roughness");
  if (s.color.has_value())
    for (f32 c : *s.color)
      if (!in(wide(c), 0, 1)) return fail(error, "surface color");
  return true;
}

}  // namespace engine::channels::detail
