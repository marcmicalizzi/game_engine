#include <core/hash/hash.h>
#include <domain/texture/texture_build.h>
#include <systems/renderer/terrain.h>

#if ENGINE_RENDERER_TERRAIN
#include <domain/terrain/dunes.h>
#endif

#include <algorithm>
#include <bit>
#include <cmath>

namespace engine::renderer {

// The terrain capability's field, when this build has it (terrain.h, "Two dune fields"). Without
// the capability the type exists and is never made, so the sampler's layout does not depend on the
// build.
#if ENGINE_RENDERER_TERRAIN
struct TerrainSampler::Dunes {
  terrain::DuneField field;
  i64 time_us = 0;
};

namespace {

terrain::FieldDesc field_desc(const TerrainDesc& desc) {
  terrain::FieldDesc f;
  f.seed = desc.seed;
  f.wind.seed = desc.seed;
  f.dune_height = terrain::to_mm(desc.dune_height);
  f.wavelength = terrain::to_mm(desc.dune_wavelength);
  // m^2 a year to cm^2 a day.
  f.wind.flux_cm2_per_day =
      static_cast<i32>(std::floor(static_cast<f64>(desc.sand_flux) * 10'000.0 / 365.0 + 0.5));
  for (const TerrainRidge& r : desc.ridges) {
    f.ridges.push_back(terrain::RidgeFeature{terrain::to_mm(r.from.x), terrain::to_mm(r.from.y),
                                             terrain::to_mm(r.to.x), terrain::to_mm(r.to.y),
                                             terrain::to_mm(r.width)});
  }
  for (const TerrainBasin& b : desc.basins) {
    f.basins.push_back(terrain::BasinFeature{terrain::to_mm(b.center.x), terrain::to_mm(b.center.y),
                                             terrain::to_mm(b.radius)});
  }
  if (desc.has_bands) {
    for (const TerrainBand& band : desc.bands) {
      terrain::BandMetres m;
      m.name = band.name;
      m.kind =
          band.kind == 1 ? terrain::PrimitiveKind::barchan : terrain::PrimitiveKind::transverse;
      m.height_min = band.height_min;
      m.height_max = band.height_max;
      m.cell = band.cell;
      m.share = band.share;
      m.length_min = band.length_min;
      m.length_max = band.length_max;
      m.stoss = band.stoss;
      m.bend = band.bend;
      m.sinuosity = band.sinuosity;
      m.spread_deg = band.spread_deg;
      m.sharpness = band.sharpness;
      m.side_days = band.side_days;
      m.sharp_days = band.sharp_days;
      m.couple = static_cast<terrain::BandCouple>(band.couple <= 2 ? band.couple : 0);
      m.couple_height = band.couple_height;
      m.far = band.far;
      f.bands.push_back(terrain::band_from_metres(m));
    }
  }
  return f;
}

i64 time_us_of(const TerrainDesc& desc) noexcept {
  return static_cast<i64>(std::floor(desc.time_s * 1'000'000.0 + 0.5));
}

}  // namespace

bool terrain_generator_available() noexcept { return true; }

bool terrain_bands_valid(const TerrainDesc& desc, std::string* error) noexcept {
  if (!desc.has_bands) return true;
  const terrain::FieldDesc f = field_desc(desc);
  if (f.bands.empty()) {
    if (error != nullptr) *error = "the band table is empty: leave it out for the default";
    return false;
  }
  return terrain::validate_bands(std::span<const terrain::BandDesc>(f.bands.data(), f.bands.size()),
                                 error);
}
#else
struct TerrainSampler::Dunes {};
bool terrain_generator_available() noexcept { return false; }
bool terrain_bands_valid(const TerrainDesc&, std::string*) noexcept { return true; }
#endif

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
TerrainSampler::~TerrainSampler() = default;

TerrainSampler::TerrainSampler(const TerrainDesc& desc) noexcept : desc_(&desc) {
#if ENGINE_RENDERER_TERRAIN
  if (desc.generator == TerrainGenerator::dunes) {
    generator_ = std::make_unique<const Dunes>(
        Dunes{terrain::DuneField(field_desc(desc)), time_us_of(desc)});
  }
#endif
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

f32 TerrainSampler::features(f32 x, f32 z, f32& ridge_mask, f32& flatten) const noexcept {
  f32 ridges = 0.0f;
  ridge_mask = 0.0f;
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
  flatten = 1.0f;
  for (const TerrainBasin& b : desc_->basins) {
    const f32 dx = x - b.center.x;
    const f32 dz = z - b.center.y;
    const f32 r = std::sqrt(dx * dx + dz * dz) / std::max(b.radius, 1e-3f);
    if (r >= 1.0f) continue;
    const f32 bowl = 1.0f - r * r;
    basins -= b.depth * bowl * bowl;
    flatten = std::min(flatten, smoothstep(0.35f, 1.0f, r));
  }
  return ridges + basins;
}

f32 TerrainSampler::height(f32 x, f32 z) const noexcept {
  f32 ridge_mask = 0.0f;
  f32 flatten = 1.0f;
#if ENGINE_RENDERER_TERRAIN
  if (generator_ != nullptr) {
    // The generator thins and flattens its own sand over the same features (terrain.md, "Fixed
    // features"); the rock and the bowls are added here, as the waves have them.
    const f32 sand = terrain::height_m(generator_->field.height_um(
        terrain::to_mm(x), terrain::to_mm(z), generator_->time_us, terrain::Detail::dunes));
    return sand + features(x, z, ridge_mask, flatten);
  }
#endif
  // The same arithmetic in the same order as before the generator existed: the waves' heights,
  // their cache entries and the fixtures' pinned numbers do not move.
  f32 ridges = 0.0f;
  f32 basins = 0.0f;
  for (const TerrainRidge& r : desc_->ridges) {
    const f32 d = segment_distance(Vec2{x, z}, r.from, r.to);
    if (d >= r.width) continue;
    const f32 profile = 0.5f + 0.5f * std::cos(k_pi * d / std::max(r.width, 1e-3f));
    const f32 rock = 0.65f * value_noise(desc_->seed + 17u, x / 23.0f, z / 23.0f) +
                     0.35f * value_noise(desc_->seed + 29u, x / 8.5f, z / 8.5f);
    ridges += r.height * profile * (1.0f + r.roughness * rock);
    ridge_mask = std::max(ridge_mask, profile);
  }
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

f32 TerrainSampler::ground(f32 x, f32 z) const noexcept {
#if ENGINE_RENDERER_TERRAIN
  if (generator_ != nullptr) {
    f32 ridge_mask = 0.0f;
    f32 flatten = 1.0f;
    const f32 floor =
        terrain::height_m(generator_->field.floor_um(terrain::to_mm(x), terrain::to_mm(z)));
    return floor + features(x, z, ridge_mask, flatten);
  }
#endif
  return height(x, z);
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

namespace {

// The desert's four materials, linear base colour and perceptual roughness, in
// `terrain_material`'s order. They were four file materials until 2026-09-25 and are the same
// values now that they are blended into one map.
constexpr f32 k_surfaces[k_terrain_materials][4] = {
    {0.84f, 0.69f, 0.47f, 0.92f},  // sand
    {0.47f, 0.39f, 0.32f, 0.78f},  // ridge rock
    {0.62f, 0.52f, 0.36f, 0.95f},  // basin sand
    {0.36f, 0.40f, 0.22f, 0.85f},  // the basin's floor
};

// Half the width of the band a threshold of `material_at` is blended across, in the feature's
// weight: the blend is exactly half-way at the threshold, so the hard classification is still
// what the map says on either side of it.
constexpr f32 k_surface_band = 0.05f;

f32 band(f32 threshold, f32 weight) noexcept {
  return smoothstep(threshold - k_surface_band, threshold + k_surface_band, weight);
}

TerrainSurface mix(TerrainSurface a, u32 material, f32 t) noexcept {
  const f32* s = k_surfaces[material];
  a.albedo = Vec3{a.albedo.x + (s[0] - a.albedo.x) * t, a.albedo.y + (s[1] - a.albedo.y) * t,
                  a.albedo.z + (s[2] - a.albedo.z) * t};
  a.roughness += (s[3] - a.roughness) * t;
  return a;
}

u8 unorm8(f32 v) noexcept { return static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

}  // namespace

// The same order of precedence as `material_at`: a basin over everything, a ridge over sand.
TerrainSurface terrain_surface(const TerrainSampler& field, f32 x, f32 z) noexcept {
  const f32 basin = field.basin_weight(x, z);
  const f32 ridge = field.ridge_weight(x, z);
  TerrainSurface s;
  s.albedo = Vec3{k_surfaces[0][0], k_surfaces[0][1], k_surfaces[0][2]};
  s.roughness = k_surfaces[0][3];
  s = mix(s, 1, band(0.3f, ridge));
  s = mix(s, 2, band(0.15f, basin));
  return mix(s, 3, band(0.55f, basin));
}

Vec3 terrain_sand_albedo() noexcept {
  return Vec3{k_surfaces[0][0], k_surfaces[0][1], k_surfaces[0][2]};
}

u32 terrain_map_side(const TerrainDesc& desc) noexcept { return std::max(desc.size, 2u) - 1u; }

void bake_terrain_maps(const TerrainDesc& desc, u32 side, Vector<u8>& base_color,
                       Vector<u8>& metallic_roughness) {
  // At most 4,096 a side (k_terrain_max_size - 1), so every offset below fits in 32 bits.
  const TerrainSampler field(desc);
  const u32 texels = side * side;
  base_color.resize(texels * 4);
  metallic_roughness.resize(texels * 4);
  const f32 extent = desc.extent;
  for (u32 row = 0; row < side; ++row) {
    const f32 v = (static_cast<f32>(row) + 0.5f) / static_cast<f32>(side);
    const f32 z = -extent + 2.0f * extent * v;
    for (u32 col = 0; col < side; ++col) {
      const f32 u = (static_cast<f32>(col) + 0.5f) / static_cast<f32>(side);
      const f32 x = -extent + 2.0f * extent * u;
      const TerrainSurface s = terrain_surface(field, x, z);
      const u32 o = (row * side + col) * 4;
      base_color[o + 0] = texture::linear_to_srgb8(s.albedo.x);
      base_color[o + 1] = texture::linear_to_srgb8(s.albedo.y);
      base_color[o + 2] = texture::linear_to_srgb8(s.albedo.z);
      base_color[o + 3] = 255;
      metallic_roughness[o + 0] = 255;
      metallic_roughness[o + 1] = unorm8(s.roughness);
      metallic_roughness[o + 2] = 0;
      metallic_roughness[o + 3] = 255;
    }
  }
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
  if (desc.generator == TerrainGenerator::dunes) {
    // Only here, so a waves terrain keeps the hash it always had. The generator's version is its
    // own (`terrain::k_generator_version`, 1 today), named as a number so the hash is the same
    // whether or not this build could draw it.
    h = hash_combine(h, 0x44554E4553ull);  // "DUNES"
    h = hash_combine(h, 1u);
    h = hash_combine(h, std::bit_cast<u64>(desc.time_s));
    h = mix_f32(h, desc.sand_flux);
    if (desc.has_bands) {
      h = hash_combine(h, desc.bands.size());
      for (const TerrainBand& b : desc.bands) {
        h = hash_combine(h, hash_bytes(b.name.data(), b.name.size()));
        h = hash_combine(hash_combine(hash_combine(h, b.kind), b.couple), b.far ? 1u : 0u);
        h = hash_combine(hash_combine(h, b.side_days), b.sharp_days);
        for (const f32 v :
             {b.height_min, b.height_max, b.cell, b.share, b.length_min, b.length_max, b.stoss,
              b.bend, b.sinuosity, b.spread_deg, b.sharpness, b.couple_height})
          h = mix_f32(h, v);
      }
    }
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
#if ENGINE_RENDERER_TERRAIN
  if (const TerrainSampler::Dunes* dunes = field.dunes_field(); dunes != nullptr) {
    // The generator's grid a block of vertices at a time: one gather per block rather than one per
    // vertex, and the same heights as `height` (the generator's rule: any gather that covers a
    // point gives it the same primitives, which meet by their maximum and add across bands).
    positions.resize(n * n);
    uvs.resize(n * n);
    constexpr u32 k_block = 64;
    terrain::Gather gather;
    for (u32 bz = 0; bz < n; bz += k_block) {
      for (u32 bx = 0; bx < n; bx += k_block) {
        const u32 ez = std::min(n, bz + k_block);
        const u32 ex = std::min(n, bx + k_block);
        const auto coord = [&](u32 i) {
          return -extent + 2.0f * extent * (static_cast<f32>(i) / static_cast<f32>(n - 1));
        };
        dunes->field.gather(terrain::to_mm(coord(bx)), terrain::to_mm(coord(bz)),
                            terrain::to_mm(coord(ex - 1)), terrain::to_mm(coord(ez - 1)),
                            dunes->time_us, nullptr, gather);
        for (u32 zi = bz; zi < ez; ++zi) {
          const f32 v = static_cast<f32>(zi) / static_cast<f32>(n - 1);
          const f32 z = -extent + 2.0f * extent * v;
          for (u32 xi = bx; xi < ex; ++xi) {
            const f32 u = static_cast<f32>(xi) / static_cast<f32>(n - 1);
            const f32 x = -extent + 2.0f * extent * u;
            f32 ridge_mask = 0.0f;
            f32 flatten = 1.0f;
            const f32 sand = terrain::height_m(dunes->field.height_um(
                gather, terrain::to_mm(x), terrain::to_mm(z), terrain::Detail::dunes));
            positions[zi * n + xi] = Vec3{x, sand + field.features(x, z, ridge_mask, flatten), z};
            uvs[zi * n + xi] = Vec2{u, v};
          }
        }
      }
    }
  } else
#endif
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
