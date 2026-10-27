#pragma once

// The stochastic terrain sampler (R96, terrain_material.slang's `terrain_hex`, `terrain_hex_blend`
// and `terrain_hex_sample`) in double precision on the CPU: the reference its GPU form is held to,
// and the instrument the statistics, direction and normal tests measure it with. It follows the
// shader operation for operation where the shader is exact (the lattice's integer part, the hash)
// and in double where the shader rounds, so a disagreement is the shader's float, never a second
// formula. Change one and the other in the same commit (renderer.md, "Stochastic sampling").

#include <core/base/types.h>

#include <cmath>

namespace engine::test {

inline u32 terrain_hash(u32 x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

inline i64 terrain_floor_div(i64 a, i64 b) {
  if (a >= 0) return static_cast<i64>(static_cast<u64>(a) / static_cast<u64>(b));
  return -static_cast<i64>((static_cast<u64>(-a) + static_cast<u64>(b) - 1u) / static_cast<u64>(b));
}

struct TerrainHexRef {
  f64 weight[3] = {};
  f64 gain = 1;
  f64 shift[3][2] = {};
  f64 turn[3][2] = {};
  f64 frac[2] = {};  // the point in its lattice cell
};

// One projection plane's point: `cu`, `cv` the frame's integer cells along the plane's two axes
// and `fu_mm`, `fv_mm` the rest in millimetres (the eye's local and residual plus the point).
// `gamma` and `variance` exist for the comparison the experiments page quotes (a linear blend is
// gamma 1 without the variance term); the engine's sampler is gamma 7 with it.
inline TerrainHexRef terrain_hex_reference(i32 cu, i32 cv, f64 fu_mm, f64 fv_mm, f64 pitch_mm,
                                           f64 rotation, f64 gamma = 7.0) {
  const i64 pitch = static_cast<i64>(pitch_mm);
  const i64 den = 28 * pitch;
  const i64 iu = static_cast<i64>(cu) * 64000;
  const i64 iv = static_cast<i64>(cv) * 64000;
  const i64 nx = 97 * iu - 56 * iv;
  const i64 ny = 112 * iv;
  const i64 qx = terrain_floor_div(nx, den);
  const i64 qy = terrain_floor_div(ny, den);
  const f64 tx =
      (static_cast<f64>(nx - qx * den) + (97.0 * fu_mm - 56.0 * fv_mm)) / static_cast<f64>(den);
  const f64 ty = (static_cast<f64>(ny - qy * den) + 112.0 * fv_mm) / static_cast<f64>(den);
  const f64 wx = std::floor(tx), wy = std::floor(ty);
  const f64 f[2] = {tx - wx, ty - wy};
  const i64 bx = qx + static_cast<i64>(wx);
  const i64 by = qy + static_cast<i64>(wy);
  TerrainHexRef hex;
  hex.frac[0] = f[0];
  hex.frac[1] = f[1];
  i32 corner[3][2];
  f64 w[3];
  const f64 z = 1.0 - f[0] - f[1];
  if (z > 0.0) {
    w[0] = z, w[1] = f[1], w[2] = f[0];
    corner[0][0] = 0, corner[0][1] = 0, corner[1][0] = 0, corner[1][1] = 1;
    corner[2][0] = 1, corner[2][1] = 0;
  } else {
    w[0] = -z, w[1] = 1.0 - f[1], w[2] = 1.0 - f[0];
    corner[0][0] = 1, corner[0][1] = 1, corner[1][0] = 1, corner[1][1] = 0;
    corner[2][0] = 0, corner[2][1] = 1;
  }
  f64 sum = 0, squares = 0;
  for (u32 k = 0; k < 3; ++k)
    sum += (w[k] = std::pow(w[k], gamma));
  for (u32 k = 0; k < 3; ++k)
    squares += (w[k] /= sum) * w[k];
  for (u32 k = 0; k < 3; ++k)
    hex.weight[k] = w[k];
  hex.gain = 1.0 / std::sqrt(squares);
  const u32 seed = terrain_hash(static_cast<u32>(pitch) ^ 0x9e3779b9u);
  for (u32 k = 0; k < 3; ++k) {
    const u32 h0 = terrain_hash(static_cast<u32>(bx + corner[k][0]) ^
                                terrain_hash(static_cast<u32>(by + corner[k][1]) ^ seed));
    const u32 h1 = terrain_hash(h0);
    hex.shift[k][0] = static_cast<f64>(h0 >> 8) / 16777216.0;
    hex.shift[k][1] = static_cast<f64>(h1 >> 8) / 16777216.0;
    hex.turn[k][0] = 1;
    hex.turn[k][1] = 0;
    if (rotation > 0) {
      const f64 angle =
          (static_cast<f64>(terrain_hash(h1) >> 8) * (2.0 / 16777216.0) - 1.0) * rotation;
      const f64 c = std::cos(angle), s = std::sin(angle);
      const f64 rel[2] = {f[0] - corner[k][0], f[1] - corner[k][1]};
      const f64 r[2] = {rel[0] * (28.0 / 97.0) + rel[1] * (14.0 / 97.0), rel[1] * 0.25};
      hex.shift[k][0] += c * r[0] - s * r[1] - r[0];
      hex.shift[k][1] += s * r[0] + c * r[1] - r[1];
      hex.turn[k][0] = c;
      hex.turn[k][1] = s;
    }
  }
  return hex;
}

// The variance-preserving blend about `mean`; `variance` false is the plain linear blend.
inline f64 terrain_hex_blend_reference(const TerrainHexRef& hex, f64 mean, const f64 x[3],
                                       bool variance = true) {
  f64 sum = 0;
  for (u32 k = 0; k < 3; ++k)
    sum += hex.weight[k] * (x[k] - mean);
  return mean + (variance ? hex.gain : 1.0) * sum;
}

}  // namespace engine::test
