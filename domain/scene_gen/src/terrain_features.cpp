// A scene terrain's ridges and basins (terrain_features.h). The arithmetic is the renderer's
// `terrain.cpp`'s as it was before the dune generator's provider moved into the terrain capability,
// expression for expression: the waves' pinned heights, their cache entries and the dunes' goldens
// all pass through it.
#include <core/hash/hash.h>
#include <core/math/math.h>
#include <domain/scene_gen/terrain_features.h>

#include <algorithm>
#include <cmath>

namespace engine::scene_gen {

namespace {

f32 smoothstep(f32 edge0, f32 edge1, f32 x) noexcept {
  const f32 t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// A lattice value noise in [-1, 1], smooth between lattice points: what roughens a ridge. The
// lattice value is a hash of the cell and the seed, so no table is kept.
f32 lattice(u32 seed, i32 x, i32 z) noexcept {
  const u64 h = mix64((static_cast<u64>(static_cast<u32>(x)) << 32 | static_cast<u32>(z)) ^
                      (static_cast<u64>(seed) * 0xD6E8FEB86659FD93ull));
  return static_cast<f32>(h >> 40) / static_cast<f32>(1u << 23) - 1.0f;
}

f32 value_noise(u32 seed, f32 x, f32 z) noexcept {
  const f32 fx = std::floor(x);
  const f32 fz = std::floor(z);
  const i32 ix = static_cast<i32>(fx);
  const i32 iz = static_cast<i32>(fz);
  const f32 tx = x - fx;
  const f32 tz = z - fz;
  const f32 sx = tx * tx * (3.0f - 2.0f * tx);
  const f32 sz = tz * tz * (3.0f - 2.0f * tz);
  const f32 a = lattice(seed, ix, iz);
  const f32 b = lattice(seed, ix + 1, iz);
  const f32 c = lattice(seed, ix, iz + 1);
  const f32 d = lattice(seed, ix + 1, iz + 1);
  return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sz;
}

// The distance from p to the segment ab, all in the xz plane.
f32 segment_distance(Vec2 p, Vec2 a, Vec2 b) noexcept {
  const f32 abx = b.x - a.x;
  const f32 abz = b.y - a.y;
  const f32 apx = p.x - a.x;
  const f32 apz = p.y - a.y;
  const f32 len2 = abx * abx + abz * abz;
  const f32 t = len2 > 0.0f ? std::clamp((apx * abx + apz * abz) / len2, 0.0f, 1.0f) : 0.0f;
  const f32 dx = apx - abx * t;
  const f32 dz = apz - abz * t;
  return std::sqrt(dx * dx + dz * dz);
}

}  // namespace

TerrainFeatures terrain_features(u32 seed, std::span<const scene::Ridge> ridges,
                                 std::span<const scene::Basin> basins, f32 x, f32 z) noexcept {
  TerrainFeatures out;
  for (const scene::Ridge& r : ridges) {
    const f32 d = segment_distance(Vec2{x, z}, r.from, r.to);
    if (d >= r.width) continue;
    const f32 profile = 0.5f + 0.5f * std::cos(k_pi * d / std::max(r.width, 1e-3f));
    // Two octaves of rock: the roughness scales with the profile, so the ridge's foot blends into
    // the sand and its crest is broken.
    const f32 rock = 0.65f * value_noise(seed + 17u, x / 23.0f, z / 23.0f) +
                     0.35f * value_noise(seed + 29u, x / 8.5f, z / 8.5f);
    out.ridges += r.height * profile * (1.0f + r.roughness * rock);
    out.ridge_mask = std::max(out.ridge_mask, profile);
  }
  for (const scene::Basin& b : basins) {
    const f32 dx = x - b.center.x;
    const f32 dz = z - b.center.y;
    const f32 r = std::sqrt(dx * dx + dz * dz) / std::max(b.radius, 1e-3f);
    if (r >= 1.0f) continue;
    const f32 bowl = 1.0f - r * r;
    out.basins -= b.depth * bowl * bowl;
    out.flatten = std::min(out.flatten, smoothstep(0.35f, 1.0f, r));
  }
  return out;
}

f32 ridge_weight(std::span<const scene::Ridge> ridges, f32 x, f32 z) noexcept {
  f32 best = 0.0f;
  for (const scene::Ridge& r : ridges) {
    const f32 d = segment_distance(Vec2{x, z}, r.from, r.to);
    if (d >= r.width) continue;
    best = std::max(best, 0.5f + 0.5f * std::cos(k_pi * d / std::max(r.width, 1e-3f)));
  }
  return best;
}

f32 basin_weight(std::span<const scene::Basin> basins, f32 x, f32 z) noexcept {
  f32 best = 0.0f;
  for (const scene::Basin& b : basins) {
    const f32 dx = x - b.center.x;
    const f32 dz = z - b.center.y;
    const f32 r = std::sqrt(dx * dx + dz * dz) / std::max(b.radius, 1e-3f);
    if (r < 1.0f) best = std::max(best, 1.0f - r);
  }
  return best;
}

}  // namespace engine::scene_gen
