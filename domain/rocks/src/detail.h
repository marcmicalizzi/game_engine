#pragma once

// What the rock solver's two halves share (rocks.md): the f64 arithmetic every body is computed
// in, the seeded streams and the value noise — the boulders, outcrops and scree in solver.cpp, the
// cliffs in cliff.cpp — and the one body the cliff borrows from the radial solver, a fallen block.
// Private to the module: nothing outside `src/` includes it.
//
// Arithmetic only (IEEE `+ - * /` and `sqrt` in f64, contraction off, ADR-0035), so a body is the
// same bits on every toolchain; moving these out of solver.cpp moved no byte of a committed recipe
// (the pins in tests/rocks_tests.cpp say so).
#include <core/base/types.h>
#include <core/math/deterministic.h>
#include <domain/assets/gltf.h>

#include <cmath>
#include <schemas/rocks.h>

namespace engine::rocks::detail {

inline constexpr f64 k_pi = 3.14159265358979323846;

struct D3 {
  f64 x = 0.0;
  f64 y = 0.0;
  f64 z = 0.0;
};
inline D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline D3 operator*(D3 a, f64 s) { return {a.x * s, a.y * s, a.z * s}; }
inline f64 dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline D3 cross(D3 a, D3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline f64 length(D3 a) { return std::sqrt(dot(a, a)); }
inline D3 normalized(D3 a) {
  const f64 l = length(a);
  return l > 0.0 ? a * (1.0 / l) : D3{0.0, 1.0, 0.0};
}
inline f64 wide(f32 v) { return static_cast<f64>(v); }
inline f64 clamp01(f64 t) { return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t); }
inline f64 lerp(f64 a, f64 b, f64 t) { return a + (b - a) * t; }

// The polynomial smooth minimum (Quilez): `min(a, b)` with the corner rounded over a band `k`
// wide. Arithmetic only, so it is the same bits everywhere.
inline f64 smooth_min(f64 a, f64 b, f64 k) {
  if (k <= 0.0) return a < b ? a : b;
  const f64 h = clamp01(0.5 + 0.5 * (b - a) / k);
  return b + (a - b) * h - k * h * (1.0 - h);
}

inline u64 mix64(u64 x) {
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBull;
  x ^= x >> 31;
  return x;
}

// splitmix64: integer arithmetic on the seed, a draw's float its top 53 bits scaled by a power of
// two, so it is exact on every target.
struct Rng {
  u64 state;
  explicit Rng(u64 seed) : state(seed) {}
  u64 next() {
    state += 0x9E3779B97F4A7C15ull;
    return mix64(state);
  }
  f64 unit() { return static_cast<f64>(next() >> 11) * (1.0 / 9007199254740992.0); }  // [0, 1)
  f64 range(f64 lo, f64 hi) { return lo + (hi - lo) * unit(); }
  // A direction uniform on the sphere: z uniform in [-1, 1], an azimuth uniform.
  D3 direction() {
    const f64 z = range(-1.0, 1.0);
    const f64 a = range(0.0, 2.0 * k_pi);
    const f64 s = std::sqrt(1.0 - z * z);
    return D3{s * det::cos(a), z, s * det::sin(a)};
  }
};

inline f64 lattice_value(u64 seed, i64 i, i64 j, i64 k) {
  const u64 h = mix64(
      seed ^ mix64(static_cast<u64>(i) + mix64(static_cast<u64>(j) + mix64(static_cast<u64>(k)))));
  return static_cast<f64>(h >> 11) * (1.0 / 9007199254740992.0);
}
inline f64 fade(f64 t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

// Value noise in three dimensions, in [0, 1].
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

// The flat PBR constant a rock of `type` draws with (rocks.md, "The model by type").
assets::Material rock_material(RockType type);

// A fallen block (rocks.md, "Cliffs", the talus): one of the scree's fragments — a body of
// half-extents `half` (x and z across, y up) of the type's weathering, sharpened, unbedded, cut by
// three joint sets and two facets of its own from `seed`, at the cube-sphere rung `n` — sunk
// `buried` of its height, yawed, and emitted with its centre's foot at `at`. Appends to `out`.
void emit_fragment(RockType type, D3 half, D3 at, f64 yaw, f64 buried, u64 seed, u32 n, f64 tile_m,
                   assets::MeshData& out);

}  // namespace engine::rocks::detail
