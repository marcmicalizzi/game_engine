#include <core/hash/hash.h>
#include <systems/renderer/terrain.h>

#include <algorithm>
#include <bit>
#include <cmath>

namespace engine::renderer {

namespace {

// splitmix64: the generator every seeded choice below draws from, so the same seed is the same
// terrain on every machine that computes `sin` the same way.
u64 next(u64& state) noexcept {
  state += 0x9E3779B97F4A7C15ull;
  u64 z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

f32 unit(u64& state) noexcept {
  return static_cast<f32>(next(state) >> 40) / static_cast<f32>(1u << 24);
}

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

u64 mix_f32(u64 h, f32 v) noexcept { return hash_combine(h, std::bit_cast<u32>(v)); }

}  // namespace

// The terrain as a function: the dune waves are drawn from the seed once, so evaluating a whole
// grid — or a scene's worth of grounded instances and ruins — costs the waves and not the
// generator. This was a private class of this file until 2026-09-24, built afresh by every
// `terrain_height` call; reading 1,000 ruins over the desert overlook asked it about forty thousand
// times and spent 45–64 ms doing so (docs/subsystems/ruins.md, "Performance notes").
TerrainSampler::TerrainSampler(const TerrainDesc& desc) noexcept : desc_(&desc) {
  u64 state = static_cast<u64>(desc.seed) * 0x2545F4914F6CDD1Dull + 0x6A09E667F3BCC909ull;
  // One prevailing wind: transverse dunes run across it, so every wave's crest is within about
  // 25 degrees of perpendicular to it and the field reads as a dune sea rather than as noise.
  const f32 wind = unit(state) * 2.0f * k_pi;
  for (u32 i = 0; i < k_waves; ++i) {
    Wave& w = waves_[i];
    const f32 angle = wind + (unit(state) - 0.5f) * 0.9f;
    const f32 wavelength = desc.dune_wavelength * (0.45f + 1.15f * unit(state));
    const f32 k = 2.0f * k_pi / std::max(wavelength, 1.0f);
    w.kx = std::cos(angle) * k;
    w.kz = std::sin(angle) * k;
    w.phase = unit(state) * 2.0f * k_pi;
    // Crests meander along their length: a second wave across the first bends it.
    const f32 meander_k = k / (3.0f + 3.0f * unit(state));
    w.mx = -std::sin(angle) * meander_k;
    w.mz = std::cos(angle) * meander_k;
    w.meander = 0.4f + 0.8f * unit(state);
    w.meander_phase = unit(state) * 2.0f * k_pi;
    w.amplitude = std::pow(wavelength / std::max(desc.dune_wavelength, 1.0f), 1.2f);
  }
  // Normalized so the field's peak-to-trough is about `dune_height`, and centred on zero: s^2
  // for s = (1 + sin u) / 2 averages 3/8 and has a standard deviation of about 0.365, and a sum
  // of waves with unrelated phases spans about four of its standard deviations. Dividing by the
  // plain sum of the amplitudes instead — the first version — made a 5 m dune field 2 m high,
  // and the LOD builder, rightly, turned the ground in front of the camera into a handful of
  // clusters a few hundred metres across.
  f32 variance = 0.0f;
  for (const Wave& w : waves_)
    variance += (0.365f * w.amplitude) * (0.365f * w.amplitude);
  const f32 scale = variance > 0.0f ? 0.25f * desc.dune_height / std::sqrt(variance) : 0.0f;
  for (Wave& w : waves_)
    w.amplitude *= scale;
  const f32 roll_angle = wind + 1.2f;
  const f32 roll_k = 2.0f * k_pi / std::max(desc.dune_wavelength * 11.0f, 1.0f);
  roll_kx_ = std::cos(roll_angle) * roll_k;
  roll_kz_ = std::sin(roll_angle) * roll_k;
  roll_phase_ = unit(state) * 2.0f * k_pi;
}

f32 TerrainSampler::dunes(f32 x, f32 z) const noexcept {
  f32 h = 0.0f;
  for (const Wave& w : waves_) {
    const f32 bend = w.meander * std::sin(w.mx * x + w.mz * z + w.meander_phase);
    const f32 s = 0.5f + 0.5f * std::sin(w.kx * x + w.kz * z + w.phase + bend);
    h += w.amplitude * (s * s - 0.375f);
  }
  // A slow roll under the dunes, so the ground is not a plane with ripples on it.
  return h + 0.35f * desc_->dune_height * std::sin(roll_kx_ * x + roll_kz_ * z + roll_phase_);
}

f32 TerrainSampler::ridge_weight(f32 x, f32 z) const noexcept {
  f32 best = 0.0f;
  for (const TerrainRidge& r : desc_->ridges) {
    const f32 d = segment_distance(Vec2{x, z}, r.from, r.to);
    if (d >= r.width) continue;
    best = std::max(best, 0.5f + 0.5f * std::cos(k_pi * d / std::max(r.width, 1e-3f)));
  }
  return best;
}

f32 TerrainSampler::basin_weight(f32 x, f32 z) const noexcept {
  f32 best = 0.0f;
  for (const TerrainBasin& b : desc_->basins) {
    const f32 dx = x - b.center.x;
    const f32 dz = z - b.center.y;
    const f32 r = std::sqrt(dx * dx + dz * dz) / std::max(b.radius, 1e-3f);
    if (r < 1.0f) best = std::max(best, 1.0f - r);
  }
  return best;
}

f32 TerrainSampler::height(f32 x, f32 z) const noexcept {
  f32 ridges = 0.0f;
  f32 ridge_mask = 0.0f;
  for (const TerrainRidge& r : desc_->ridges) {
    const f32 d = segment_distance(Vec2{x, z}, r.from, r.to);
    if (d >= r.width) continue;
    const f32 profile = 0.5f + 0.5f * std::cos(k_pi * d / std::max(r.width, 1e-3f));
    // Two octaves of rock: the roughness scales with the profile, so the ridge's foot blends
    // into the sand and its crest is broken.
    const f32 rock = 0.65f * value_noise(desc_->seed + 17u, x / 23.0f, z / 23.0f) +
                     0.35f * value_noise(desc_->seed + 29u, x / 8.5f, z / 8.5f);
    ridges += r.height * profile * (1.0f + r.roughness * rock);
    ridge_mask = std::max(ridge_mask, profile);
  }
  f32 basins = 0.0f;
  f32 flatten = 1.0f;
  for (const TerrainBasin& b : desc_->basins) {
    const f32 dx = x - b.center.x;
    const f32 dz = z - b.center.y;
    const f32 r = std::sqrt(dx * dx + dz * dz) / std::max(b.radius, 1e-3f);
    if (r >= 1.0f) continue;
    const f32 bowl = 1.0f - r * r;
    basins -= b.depth * bowl * bowl;
    flatten = std::min(flatten, smoothstep(0.35f, 1.0f, r));
  }
  return dunes(x, z) * flatten * (1.0f - 0.6f * ridge_mask) + ridges + basins;
}

f32 terrain_height(const TerrainDesc& desc, f32 x, f32 z) noexcept {
  return TerrainSampler(desc).height(x, z);
}

f32 terrain_ridge_weight(const TerrainDesc& desc, f32 x, f32 z) noexcept {
  return TerrainSampler(desc).ridge_weight(x, z);
}

f32 terrain_basin_weight(const TerrainDesc& desc, f32 x, f32 z) noexcept {
  return TerrainSampler(desc).basin_weight(x, z);
}

namespace {

u32 material_at(const TerrainSampler& field, f32 x, f32 z) noexcept {
  const f32 basin = field.basin_weight(x, z);
  if (basin > 0.55f) return 3;
  if (basin > 0.15f) return 2;
  return field.ridge_weight(x, z) > 0.3f ? 1u : 0u;
}

}  // namespace

u32 terrain_material(const TerrainDesc& desc, f32 x, f32 z) noexcept {
  return material_at(TerrainSampler(desc), x, z);
}

u32 terrain_majority_material(const TerrainDesc& desc, std::span<const Vec3> points) noexcept {
  const TerrainSampler field(desc);
  u32 votes[k_terrain_materials] = {};
  for (const Vec3& p : points)
    ++votes[material_at(field, p.x, p.z)];
  u32 best = 0;
  for (u32 m = 1; m < k_terrain_materials; ++m)
    best = votes[m] > votes[best] ? m : best;
  return best;
}

u64 terrain_hash(const TerrainDesc& desc) noexcept {
  u64 h = hash_bytes("engine.terrain", 14);
  h = hash_combine(h, k_terrain_version);
  h = hash_combine(h, desc.size);
  h = mix_f32(h, desc.extent);
  h = hash_combine(h, desc.seed);
  h = mix_f32(h, desc.dune_height);
  h = mix_f32(h, desc.dune_wavelength);
  h = hash_combine(h, desc.ridges.size());
  for (const TerrainRidge& r : desc.ridges) {
    h = mix_f32(mix_f32(mix_f32(mix_f32(h, r.from.x), r.from.y), r.to.x), r.to.y);
    h = mix_f32(mix_f32(mix_f32(h, r.height), r.width), r.roughness);
  }
  h = hash_combine(h, desc.basins.size());
  for (const TerrainBasin& b : desc.basins) {
    h = mix_f32(mix_f32(mix_f32(mix_f32(h, b.center.x), b.center.y), b.radius), b.depth);
  }
  return h;
}

bool build_terrain_mesh(const TerrainDesc& desc, Vector<Vec3>& positions, Vector<u32>& indices,
                        Vector<Vec2>& uvs, std::string* error) {
  if (desc.size < 2 || desc.size > k_terrain_max_size || !(desc.extent > 0.0f)) {
    if (error != nullptr) {
      *error = "terrain: size must be within 2.." + std::to_string(k_terrain_max_size) +
               " and extent positive";
    }
    return false;
  }
  const TerrainSampler field(desc);
  const u32 n = desc.size;
  const f32 extent = desc.extent;
  positions.clear();
  indices.clear();
  uvs.clear();
  positions.reserve(n * n);
  uvs.reserve(n * n);
  for (u32 zi = 0; zi < n; ++zi) {
    const f32 v = static_cast<f32>(zi) / static_cast<f32>(n - 1);
    const f32 z = -extent + 2.0f * extent * v;
    for (u32 xi = 0; xi < n; ++xi) {
      const f32 u = static_cast<f32>(xi) / static_cast<f32>(n - 1);
      const f32 x = -extent + 2.0f * extent * u;
      positions.push_back(Vec3{x, field.height(x, z), z});
      uvs.push_back(Vec2{u, v});
    }
  }
  // Counter-clockwise seen from +y, the classic heightfield's winding (renderer.md).
  indices.reserve((n - 1) * (n - 1) * 6);
  for (u32 zi = 0; zi + 1 < n; ++zi) {
    for (u32 xi = 0; xi + 1 < n; ++xi) {
      const u32 a = zi * n + xi;
      indices.push_back(a);
      indices.push_back(a + n);
      indices.push_back(a + 1);
      indices.push_back(a + 1);
      indices.push_back(a + n);
      indices.push_back(a + n + 1);
    }
  }
  return true;
}

}  // namespace engine::renderer
