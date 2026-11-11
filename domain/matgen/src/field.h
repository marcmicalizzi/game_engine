#pragma once

// The generators' shared ground (matgen.md, "How a set is built"): the texel field the three kinds
// fill, the hashes and the periodic noises they draw from, and the wrapped rasterizers. Internal
// to the module.
//
// **Everything here lives on the torus of the tile.** A noise takes a lattice coordinate of any
// size and wraps its nodes by its own period; a rasterizer takes a centre in integer sub-texels
// and wraps every texel it touches. So no function knows where the image starts, and a build that
// starts elsewhere (`Window`) is the same picture rotated, to the byte.
//
// **Nothing here calls the C library for a value**: `std::floor` and `std::sqrt` are exact, and
// the transcendentals are `core/math/deterministic.h`'s.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/deterministic.h>
#include <domain/matgen/matgen.h>

#include <cmath>

namespace engine::matgen::detail {

inline constexpr f64 k_pi = 3.14159265358979323846;
// A clast's or a fragment's centre is kept in units of a texel over this.
inline constexpr i64 k_sub = 256;

inline u64 mix64(u64 x) noexcept {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}
inline u64 hash3(u64 seed, u64 a, u64 b) noexcept {
  return mix64(mix64(seed ^ (a * 0x9E3779B97F4A7C15ull)) ^ (b * 0xC2B2AE3D27D4EB4Full));
}
inline f64 unit_of(u64 h) noexcept {
  return static_cast<f64>(h >> 11) * (1.0 / 9007199254740992.0);
}

// A stream of draws from one seed (splitmix64).
struct Rng {
  u64 state;
  explicit Rng(u64 seed) noexcept : state(mix64(seed)) {}
  u64 next() noexcept {
    state += 0x9E3779B97F4A7C15ull;
    u64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  f64 unit() noexcept { return unit_of(next()); }
  f64 range(f64 lo, f64 hi) noexcept { return lo + (hi - lo) * unit(); }
  // A standard normal draw (Box and Muller 1958), from the engine's own log and cosine.
  f64 normal() noexcept {
    const f64 u1 = 1.0 - unit();  // (0, 1]
    const f64 u2 = unit();
    return std::sqrt(-2.0 * det::log(u1)) * det::cos(2.0 * k_pi * u2);
  }
};

inline f64 clamp01(f64 x) noexcept { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }
inline f64 mix(f64 a, f64 b, f64 t) noexcept { return a + (b - a) * t; }
inline f64 smooth01(f64 t) noexcept {
  const f64 c = clamp01(t);
  return c * c * (3.0 - 2.0 * c);
}
inline f64 permille(u32 v) noexcept { return static_cast<f64>(v) * 0.001; }

// One texel while a set is built: linear albedo, roughness, height in metres.
struct Texel {
  f32 r = 0, g = 0, b = 0;
  f32 rough = 1;
  f32 height = 0;
};

// The field a kind fills. Stored texel (x, y) is the torus' texel ((x + window.x) mod n,
// (y + window.y) mod n); `vx`/`vy` give the unwrapped index a noise is asked at.
struct Field {
  u32 n = 0;
  f64 texel_m = 0;
  Window window;
  Vector<Texel> texels;

  void reset(u32 size, f64 tile_m, Window w) {
    n = size;
    texel_m = tile_m / static_cast<f64>(size);
    window = w;
    texels.assign(size * size, Texel{});
  }
  u32 index(u32 x, u32 y) const noexcept { return y * n + x; }
  u32 vx(u32 x) const noexcept { return x + window.x; }
  u32 vy(u32 y) const noexcept { return y + window.y; }
  // The stored index of a torus texel given as any integer.
  u32 stored(i64 torus, u32 origin) const noexcept {
    const i64 size = static_cast<i64>(n);
    i64 m = (torus - static_cast<i64>(origin)) % size;
    if (m < 0) m += size;
    return static_cast<u32>(m);
  }
};

// The lattice coordinate of unwrapped texel `v` of `n` for a lattice of period `k`: exact for the
// powers of two `n` is held to.
inline f64 lattice_of(u32 v, u32 n, u32 k) noexcept {
  return (static_cast<f64>(v) + 0.5) * static_cast<f64>(k) / static_cast<f64>(n);
}

inline u32 wrap_node(i64 i, u32 k) noexcept {
  const i64 period = static_cast<i64>(k);
  i64 m = i % period;
  if (m < 0) m += period;
  return static_cast<u32>(m);
}

// Value noise in [0, 1] on a lattice that repeats every `k` nodes, smoothstep-bilinear.
inline f64 lattice_noise(u64 seed, f64 x, f64 y, u32 k) noexcept {
  const f64 fx = std::floor(x), fy = std::floor(y);
  const i64 ix = static_cast<i64>(fx), iy = static_cast<i64>(fy);
  const u32 xa = wrap_node(ix, k), xb = wrap_node(ix + 1, k);
  const u32 ya = wrap_node(iy, k), yb = wrap_node(iy + 1, k);
  const f64 tx = x - fx, ty = y - fy;
  const f64 sx = tx * tx * (3.0 - 2.0 * tx), sy = ty * ty * (3.0 - 2.0 * ty);
  const f64 a = mix(unit_of(hash3(seed, xa, ya)), unit_of(hash3(seed, xb, ya)), sx);
  const f64 b = mix(unit_of(hash3(seed, xa, yb)), unit_of(hash3(seed, xb, yb)), sx);
  return mix(a, b, sy);
}

// The same with its own period along each axis: a streak's noise, fine across and long down.
inline f64 lattice_noise2(u64 seed, u32 vx, u32 vy, u32 n, u32 kx, u32 ky) noexcept {
  const f64 x = lattice_of(vx, n, kx), y = lattice_of(vy, n, ky);
  const f64 fx = std::floor(x), fy = std::floor(y);
  const i64 ix = static_cast<i64>(fx), iy = static_cast<i64>(fy);
  const u32 xa = wrap_node(ix, kx), xb = wrap_node(ix + 1, kx);
  const u32 ya = wrap_node(iy, ky), yb = wrap_node(iy + 1, ky);
  const f64 tx = x - fx, ty = y - fy;
  const f64 sx = tx * tx * (3.0 - 2.0 * tx), sy = ty * ty * (3.0 - 2.0 * ty);
  const f64 a = mix(unit_of(hash3(seed, xa, ya)), unit_of(hash3(seed, xb, ya)), sx);
  const f64 b = mix(unit_of(hash3(seed, xa, yb)), unit_of(hash3(seed, xb, yb)), sx);
  return mix(a, b, sy);
}

// `octaves` of it from period `k` up, each half the last one's weight, in [0, 1].
inline f64 fractal_noise(u64 seed, u32 vx, u32 vy, u32 n, u32 k, u32 octaves) noexcept {
  f64 sum = 0.0, weight = 1.0, total = 0.0;
  u32 period = k;
  for (u32 o = 0; o < octaves && period <= n; ++o) {
    sum += weight *
           lattice_noise(seed + o, lattice_of(vx, n, period), lattice_of(vy, n, period), period);
    total += weight;
    weight *= 0.5;
    period *= 2;
  }
  return total > 0.0 ? sum / total : 0.5;
}

// The nearest feature point of a cellular lattice of period `k` (Worley 1996): its cell's hash,
// the distance to it and to the second nearest, in cells.
struct Cell {
  u64 id = 0;
  f64 f1 = 0;
  f64 f2 = 0;
};
inline Cell cell_noise(u64 seed, f64 x, f64 y, u32 k) noexcept {
  const f64 fx = std::floor(x), fy = std::floor(y);
  const i64 ix = static_cast<i64>(fx), iy = static_cast<i64>(fy);
  // The point within its own cell, so no distance depends on which period the caller is in.
  const f64 tx = x - fx, ty = y - fy;
  Cell out;
  f64 best = 1.0e30, second = 1.0e30;
  for (i64 j = -1; j <= 1; ++j) {
    for (i64 i = -1; i <= 1; ++i) {
      const u64 h = hash3(seed, wrap_node(ix + i, k), wrap_node(iy + j, k));
      const f64 dx = (static_cast<f64>(i) + unit_of(h)) - tx;
      const f64 dy = (static_cast<f64>(j) + unit_of(mix64(h))) - ty;
      const f64 d = dx * dx + dy * dy;
      if (d < best) {
        second = best;
        best = d;
        out.id = h;
      } else if (d < second) {
        second = d;
      }
    }
  }
  out.f1 = std::sqrt(best);
  out.f2 = std::sqrt(second);
  return out;
}

// The offset of torus texel `t` from a centre in sub-texels, by the nearest image, in texels: an
// integer difference, so it is the same number wherever the image starts.
inline f64 offset_texels(i64 t, i64 centre_sub, i64 n) noexcept {
  const i64 period = n * k_sub;
  i64 d = (t * k_sub + k_sub / 2 - centre_sub) % period;
  if (d < -period / 2) d += period;
  if (d >= period / 2) d -= period;
  return static_cast<f64>(d) / static_cast<f64>(k_sub);
}

// A constituent picked by cumulative share from a unit draw.
inline u32 pick_constituent(const Vector<Constituent>& mix_of, f64 u) noexcept {
  f64 total = 0.0;
  for (const Constituent& c : mix_of)
    total += static_cast<f64>(c.share_permille);
  f64 at = u * total;
  for (u32 i = 0; i < mix_of.size(); ++i) {
    at -= static_cast<f64>(mix_of[i].share_permille);
    if (at < 0.0) return i;
  }
  return mix_of.empty() ? 0u : static_cast<u32>(mix_of.size() - 1);
}

// A dead or live leaf fragment laid on the field: a capsule of `half_m` half length and
// `width_m` across at `angle`, its width tapering to the tips when `taper`, blended over what is
// there by its coverage of each texel. Returns nothing; `cover` (when not null) accumulates the
// share of each texel under any fragment of the call's series.
struct Fragment {
  i64 cx = 0, cy = 0;  // sub-texels on the torus
  f64 half_m = 0;
  f64 width_m = 0;
  f64 dir_x = 1, dir_y = 0;
  f64 colour[3] = {0, 0, 0};
  f64 rough = 0.8;
  f64 lift_m = 0;
  f64 lift_max_m = 0;
  bool taper = false;
  f64 midrib = 0;  // how much paler the centre line is
};
void lay_fragment(Field& field, const Fragment& f, Vector<f32>* cover);

// The kinds (granular.cpp, rock.cpp, ground.cpp): each fills `field` and its part of `stats`, and
// says what depth of hollow counts as full shade (`cavity_m`) and how wide a hollow is
// (`cavity_px`).
struct Shade {
  f64 cavity_m = 0.002;
  u32 cavity_px = 4;
};
void build_granular(const Set& set, Field& field, SetStats& stats, Shade& shade);
void build_rock(const Set& set, Field& field, SetStats& stats, Shade& shade);
void build_ground(const Set& set, Field& field, SetStats& stats, Shade& shade);

}  // namespace engine::matgen::detail
