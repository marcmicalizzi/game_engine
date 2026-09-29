#include <core/hash/hash.h>
#include <domain/scene_gen/terrain_features.h>
#include <domain/texture/texture_build.h>
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

u64 mix_f32(u64 h, f32 v) noexcept { return hash_combine(h, std::bit_cast<u32>(v)); }

std::span<const scene::Ridge> ridges_of(const Vector<scene::Ridge>& ridges) noexcept {
  return std::span<const scene::Ridge>(ridges.data(), ridges.size());
}
std::span<const scene::Basin> basins_of(const Vector<scene::Basin>& basins) noexcept {
  return std::span<const scene::Basin>(basins.data(), basins.size());
}

// ---- the waves: the renderer's own ground provider ----------------------------------------------

// The terrain as a function: the dune waves are drawn from the seed once, so evaluating a whole
// grid — or a scene's worth of grounded instances and ruins — costs the waves and not the
// generator. This was a private class of this file until 2026-09-24, built afresh by every
// `terrain_height` call; reading 1,000 ruins over the desert overlook asked it about forty thousand
// times and spent 45–64 ms doing so (docs/subsystems/ruins.md, "Performance notes"). Since
// 2026-09-27 it is a ground provider like any other (scene_gen.md), registered by this file, so a
// scene's default terrain goes down the same path as one a capability draws.
constexpr u32 k_waves = 6;
struct Wave {
  f32 kx = 0.0f;
  f32 kz = 0.0f;
  f32 phase = 0.0f;
  f32 mx = 0.0f;
  f32 mz = 0.0f;
  f32 meander = 0.0f;
  f32 meander_phase = 0.0f;
  f32 amplitude = 0.0f;
};

struct WavesGround {
  scene::Terrain entry;  // the seed, the dune height and wavelength, the ridges and basins
  Wave waves[k_waves];
  f32 roll_kx = 0.0f;
  f32 roll_kz = 0.0f;
  f32 roll_phase = 0.0f;
  // The prevailing wind the waves' crests run across, as a unit vector: the one wind the waves
  // have, and so the one their surface detail lies across at every time.
  f32 wind_x = 1.0f;
  f32 wind_z = 0.0f;
};

WavesGround* make_waves(const scene::Terrain& entry) {
  auto* g = new WavesGround{};
  g->entry = entry;
  u64 state = static_cast<u64>(entry.seed) * 0x2545F4914F6CDD1Dull + 0x6A09E667F3BCC909ull;
  // One prevailing wind: transverse dunes run across it, so every wave's crest is within about
  // 25 degrees of perpendicular to it and the field reads as a dune sea rather than as noise.
  const f32 wind = unit(state) * 2.0f * k_pi;
  g->wind_x = std::cos(wind);
  g->wind_z = std::sin(wind);
  for (u32 i = 0; i < k_waves; ++i) {
    Wave& w = g->waves[i];
    const f32 angle = wind + (unit(state) - 0.5f) * 0.9f;
    const f32 wavelength = entry.dune_wavelength * (0.45f + 1.15f * unit(state));
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
    w.amplitude = std::pow(wavelength / std::max(entry.dune_wavelength, 1.0f), 1.2f);
  }
  // Normalized so the field's peak-to-trough is about `dune_height`, and centred on zero: s^2
  // for s = (1 + sin u) / 2 averages 3/8 and has a standard deviation of about 0.365, and a sum
  // of waves with unrelated phases spans about four of its standard deviations. Dividing by the
  // plain sum of the amplitudes instead — the first version — made a 5 m dune field 2 m high,
  // and the LOD builder, rightly, turned the ground in front of the camera into a handful of
  // clusters a few hundred metres across.
  f32 variance = 0.0f;
  for (const Wave& w : g->waves)
    variance += (0.365f * w.amplitude) * (0.365f * w.amplitude);
  const f32 scale = variance > 0.0f ? 0.25f * entry.dune_height / std::sqrt(variance) : 0.0f;
  for (Wave& w : g->waves)
    w.amplitude *= scale;
  const f32 roll_angle = wind + 1.2f;
  const f32 roll_k = 2.0f * k_pi / std::max(entry.dune_wavelength * 11.0f, 1.0f);
  g->roll_kx = std::cos(roll_angle) * roll_k;
  g->roll_kz = std::sin(roll_angle) * roll_k;
  g->roll_phase = unit(state) * 2.0f * k_pi;
  return g;
}

f32 waves_dunes(const WavesGround& g, f32 x, f32 z) noexcept {
  f32 h = 0.0f;
  for (const Wave& w : g.waves) {
    const f32 bend = w.meander * std::sin(w.mx * x + w.mz * z + w.meander_phase);
    const f32 s = 0.5f + 0.5f * std::sin(w.kx * x + w.kz * z + w.phase + bend);
    h += w.amplitude * (s * s - 0.375f);
  }
  // A slow roll under the dunes, so the ground is not a plane with ripples on it.
  return h + 0.35f * g.entry.dune_height * std::sin(g.roll_kx * x + g.roll_kz * z + g.roll_phase);
}

// The same arithmetic in the same order as before the generator existed: the waves' heights,
// their cache entries and the fixtures' pinned numbers do not move.
f32 waves_height(const void* state, f32 x, f32 z) noexcept {
  const auto& g = *static_cast<const WavesGround*>(state);
  const scene_gen::TerrainFeatures f = scene_gen::terrain_features(
      g.entry.seed, ridges_of(g.entry.ridges), basins_of(g.entry.basins), x, z);
  return waves_dunes(g, x, z) * f.flatten * (1.0f - 0.6f * f.ridge_mask) + f.ridges + f.basins;
}

void waves_destroy(void* state) noexcept { delete static_cast<WavesGround*>(state); }

// Their prevailing wind, at every time: the waves do not move.
bool waves_wind(const void* state, f64, f32& x, f32& z) noexcept {
  const auto& g = *static_cast<const WavesGround*>(state);
  x = g.wind_x;
  z = g.wind_z;
  return true;
}

// The waves have no time, no rings and no tiles of their own: a point function, which the registry
// samples a point at a time for a grid; and one wind, which their detail lies across.
constexpr scene_gen::GroundOps k_waves_ops{
    .destroy = &waves_destroy, .height = &waves_height, .wind = &waves_wind};

bool waves_make(const scene::Terrain& entry, const scene_gen::Context&,
                scene_gen::GroundProvider& out, std::string*) {
  out = scene_gen::GroundProvider(&k_waves_ops, make_waves(entry));
  return true;
}

constexpr scene_gen::GroundProviderDesc k_waves_provider{.name = "waves", .make = &waves_make};
const scene_gen::Registrar k_waves_registrar{k_waves_provider};

}  // namespace

// ---- the description ----------------------------------------------------------------------------

std::string_view terrain_provider(const TerrainDesc& desc) noexcept {
  if (!desc.provider.empty()) return desc.provider;
  return desc.generator == TerrainGenerator::dunes ? std::string_view("dunes")
                                                   : std::string_view("waves");
}

scene::Terrain terrain_entry(const TerrainDesc& desc) {
  scene::Terrain t;
  t.size = desc.size;
  t.extent = desc.extent;
  t.seed = desc.seed;
  t.dune_height = desc.dune_height;
  t.dune_wavelength = desc.dune_wavelength;
  t.ridges = desc.ridges;
  t.basins = desc.basins;
  t.generator = desc.generator == TerrainGenerator::dunes ? scene::TerrainGenerator::Dunes
                                                          : scene::TerrainGenerator::Waves;
  t.time = desc.time_s;
  t.sand_flux = desc.sand_flux;
  if (desc.has_bands) {
    Vector<scene::TerrainBand> bands;
    for (const TerrainBand& band : desc.bands) {
      scene::TerrainBand b;
      b.name = band.name;
      b.kind = band.kind == 1 ? scene::DuneKind::Barchan : scene::DuneKind::Transverse;
      b.height_min = band.height_min;
      b.height_max = band.height_max;
      b.cell = band.cell;
      b.share = band.share;
      b.length_min = band.length_min;
      b.length_max = band.length_max;
      b.stoss = band.stoss;
      b.bend = band.bend;
      b.sinuosity = band.sinuosity;
      b.spread_deg = band.spread_deg;
      b.sharpness = band.sharpness;
      b.side_days = band.side_days;
      b.sharp_days = band.sharp_days;
      b.couple = static_cast<scene::DuneCouple>(band.couple <= 2 ? band.couple : 0);
      b.couple_width = band.couple_width;
      b.far = band.far;
      b.celerity_scale = band.celerity_scale;
      bands.push_back(std::move(b));
    }
    t.bands = std::move(bands);
  }
  t.storms_per_year = desc.storms_per_year;
  t.storm_strength = desc.storm_strength;
  t.diurnal_strength = desc.diurnal_strength;
  t.diurnal_peak_hour = desc.diurnal_peak_hour;
  t.diurnal_veer_deg = desc.diurnal_veer_deg;
  t.diurnal_veer_hour = desc.diurnal_veer_hour;
  t.storm_gain = desc.storm_gain;
  for (const TerrainDesc::StormGain& g : desc.storm_gains) {
    scene::StormGain sg;
    sg.storm = g.storm;
    sg.gain = g.gain;
    t.storm_gains.push_back(sg);
  }
  t.provider = std::string(terrain_provider(desc));
  if (desc.has_detail) t.detail = desc.detail;
  return t;
}

namespace {

const scene_gen::GroundProviderDesc* provider_of(const TerrainDesc& desc) noexcept {
  return scene_gen::GeneratorRegistry::global().find_ground(terrain_provider(desc));
}

}  // namespace

bool terrain_provider_known(const TerrainDesc& desc) noexcept {
  return provider_of(desc) != nullptr;
}

bool terrain_moves(const TerrainDesc& desc) noexcept {
  const scene_gen::GroundProviderDesc* found = provider_of(desc);
  return found != nullptr && (found->flags & scene_gen::k_ground_moves) != 0;
}

bool terrain_has_rings(const TerrainDesc& desc) noexcept {
  const scene_gen::GroundProviderDesc* found = provider_of(desc);
  return found != nullptr && (found->flags & scene_gen::k_ground_rings) != 0;
}

// ---- the sampler --------------------------------------------------------------------------------

TerrainSampler::~TerrainSampler() = default;

TerrainSampler::TerrainSampler(const TerrainDesc& desc) noexcept : desc_(&desc) {
  const scene::Terrain entry = terrain_entry(desc);
  const scene_gen::GeneratorRegistry& registry = scene_gen::GeneratorRegistry::global();
  const std::string_view name = terrain_provider(desc);
  const scene_gen::GroundProviderDesc* found = registry.find_ground(name);
  scene_gen::Context context;
  context.world_seed = desc.seed;
  if (found == nullptr) {
    error_ = "the terrain " + registry.unknown_ground(name);
  } else if (!found->make(entry, context, provider_, &error_) || !provider_.valid()) {
    if (error_.empty())
      error_ = "the terrain's ground provider \"" + std::string(name) + "\" refused it";
    provider_.reset();
  }
  // Whatever could not be made, the waves over the same description: made directly, not through
  // the registry, so a sampler always has a ground.
  if (!provider_.valid()) provider_ = scene_gen::GroundProvider(&k_waves_ops, make_waves(entry));
}

Vec2 TerrainSampler::wind(f64 time_s) const noexcept {
  f32 x = 1.0f;
  f32 z = 0.0f;
  if (!provider_.wind(time_s, x, z)) return Vec2{1.0f, 0.0f};
  return Vec2{x, z};
}

f32 TerrainSampler::ridge_weight(f32 x, f32 z) const noexcept {
  return scene_gen::ridge_weight(ridges_of(desc_->ridges), x, z);
}

f32 TerrainSampler::basin_weight(f32 x, f32 z) const noexcept {
  return scene_gen::basin_weight(basins_of(desc_->basins), x, z);
}

f32 TerrainSampler::features(f32 x, f32 z, f32& ridge_mask, f32& flatten) const noexcept {
  const scene_gen::TerrainFeatures f = scene_gen::terrain_features(
      desc_->seed, ridges_of(desc_->ridges), basins_of(desc_->basins), x, z);
  ridge_mask = f.ridge_mask;
  flatten = f.flatten;
  return f.ridges + f.basins;
}

f32 terrain_height(const TerrainDesc& desc, f32 x, f32 z) noexcept {
  return TerrainSampler(desc).height(x, z);
}

// The surface weights are the scene's ridges and basins alone: no ground is made for them.
f32 terrain_ridge_weight(const TerrainDesc& desc, f32 x, f32 z) noexcept {
  return scene_gen::ridge_weight(ridges_of(desc.ridges), x, z);
}

f32 terrain_basin_weight(const TerrainDesc& desc, f32 x, f32 z) noexcept {
  return scene_gen::basin_weight(basins_of(desc.basins), x, z);
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
// Which of them is sand, which the sand's detail is drawn on: the plain's and the basin's.
constexpr f32 k_surface_sand[k_terrain_materials] = {1.0f, 0.0f, 1.0f, 0.0f};

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
  a.sand += (k_surface_sand[material] - a.sand) * t;
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
      // The sand share, which the sand's detail is weighted by, when the scene asks for the detail;
      // 255, as it always was, when it does not.
      metallic_roughness[o + 3] = desc.has_detail ? unorm8(s.sand) : u8{255};
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
  const std::string_view provider = terrain_provider(desc);
  if (provider != "waves") {
    if (provider != "dunes") {
      // A provider other than the two by its name, before the fields every provider may read, so
      // two providers over one description are two cache entries.
      h = hash_combine(h, 0x50524F5649444552ull);  // "PROVIDER"
      h = hash_combine(h, hash_bytes(provider.data(), provider.size()));
    }
    // Only here, so a waves terrain keeps the hash it always had. The generator's version is its
    // own (`terrain::k_generator_version`, 2 today), named as a number so the hash is the same
    // whether or not this build could draw it.
    h = hash_combine(h, 0x44554E4553ull);  // "DUNES"
    h = hash_combine(h, 2u);
    h = hash_combine(h, std::bit_cast<u64>(desc.time_s));
    h = mix_f32(h, desc.sand_flux);
    if (desc.storms_per_year > 0) {
      h = hash_combine(hash_combine(h, 0x53544F524Dull), desc.storms_per_year);  // "STORM"
      h = mix_f32(h, desc.storm_strength);
    }
    // The wind's day and the storms' gains, only when a scene asks for them.
    if (desc.diurnal_strength != 0.0f || desc.diurnal_veer_deg != 0.0f) {
      h = hash_combine(h, 0x444955524E414Cull);  // "DIURNAL"
      for (const f32 v : {desc.diurnal_strength, desc.diurnal_peak_hour, desc.diurnal_veer_deg,
                          desc.diurnal_veer_hour})
        h = mix_f32(h, v);
    }
    if (desc.storm_gain != 1.0f || !desc.storm_gains.empty()) {
      h = mix_f32(hash_combine(h, 0x4741494Eull), desc.storm_gain);  // "GAIN"
      for (const TerrainDesc::StormGain& g : desc.storm_gains)
        h = mix_f32(hash_combine(h, g.storm), g.gain);
    }
    if (desc.has_bands) {
      h = hash_combine(h, desc.bands.size());
      for (const TerrainBand& b : desc.bands) {
        h = hash_combine(h, hash_bytes(b.name.data(), b.name.size()));
        h = hash_combine(hash_combine(hash_combine(h, b.kind), b.couple), b.far ? 1u : 0u);
        h = hash_combine(hash_combine(h, b.side_days), b.sharp_days);
        for (const f32 v :
             {b.height_min, b.height_max, b.cell, b.share, b.length_min, b.length_max, b.stoss,
              b.bend, b.sinuosity, b.spread_deg, b.sharpness, b.couple_width})
          h = mix_f32(h, v);
        // Only when it is not the physical rate, so every scene before it keeps its hash.
        if (b.celerity_scale != 1.0f) h = mix_f32(hash_combine(h, 0x43454CULL), b.celerity_scale);
      }
    }
  }
  // The sand's detail puts the sand share in the metallic-roughness map's alpha, which changes the
  // maps' bytes only where a ridge or a basin makes some ground other than sand: only then is it
  // another cache entry. Its numbers are shading and never enter (terrain.h says why).
  if (desc.has_detail && (!desc.ridges.empty() || !desc.basins.empty()))
    h = hash_combine(h, 0x53414E444D41534Bull);  // "SANDMASK"
  return h;
}

gfx::GroundDetailDesc terrain_detail_desc(const TerrainDesc& desc) noexcept {
  const scene::TerrainDetail& d = desc.detail;
  gfx::GroundDetailDesc out;
  out.ripple_wavelength = d.ripple_wavelength;
  out.ripple_height = d.ripple_height;
  out.ripple_asymmetry = d.ripple_asymmetry;
  out.ripple_defects = d.ripple_defects;
  out.slope_start_deg = d.ripple_slope_start_deg;
  out.slope_end_deg = d.ripple_slope_end_deg;
  out.grain_size = d.grain_size;
  out.grain_albedo = d.grain_albedo;
  out.grain_roughness = d.grain_roughness;
  return out;
}

bool validate_terrain_detail(const scene::TerrainDetail& d, std::string* error) {
  const auto fail = [&](const char* sentence) {
    if (error != nullptr) *error = std::string("terrain.detail: ") + sentence;
    return false;
  };
  if (!(d.ripple_wavelength >= 0.01f && d.ripple_wavelength <= 1.0f))
    return fail("ripple_wavelength must be within [0.01, 1] m");
  if (!(d.ripple_height >= 0.0f && d.ripple_height <= 0.1f &&
        d.ripple_height <= 0.5f * d.ripple_wavelength)) {
    return fail("ripple_height must be within [0, 0.1] m and at most half the wavelength");
  }
  if (!(d.ripple_asymmetry >= 0.5f && d.ripple_asymmetry <= 0.95f))
    return fail("ripple_asymmetry must be within [0.5, 0.95]");
  if (!(d.ripple_defects >= 0.0f && d.ripple_defects <= 1.0f))
    return fail("ripple_defects must be within [0, 1]");
  if (!(d.ripple_slope_start_deg >= 0.0f && d.ripple_slope_start_deg < d.ripple_slope_end_deg &&
        d.ripple_slope_end_deg <= 90.0f)) {
    return fail(
        "the ripples' slope fade must rise: 0 <= ripple_slope_start_deg < "
        "ripple_slope_end_deg <= 90");
  }
  if (!(d.grain_size > 0.0f && d.grain_size <= 0.2f))
    return fail("grain_size must be within (0, 0.2] m");
  if (!(d.grain_albedo >= 0.0f && d.grain_albedo <= 0.5f && d.grain_roughness >= 0.0f &&
        d.grain_roughness <= 0.5f)) {
    return fail("grain_albedo and grain_roughness must be within [0, 0.5]");
  }
  return true;
}

u32 terrain_height_blocks(const TerrainDesc& desc) noexcept {
  return terrain_window_blocks(desc.size, desc.size);
}

bool evaluate_terrain_heights(const TerrainSampler& sampler, f64 time_s, u32 block_begin,
                              u32 block_end, std::span<f32> heights) noexcept {
  const TerrainDesc& desc = sampler.desc();
  return evaluate_terrain_window(sampler, time_s, terrain_scene_lattice(desc), 0, 0, desc.size,
                                 desc.size, block_begin, block_end, heights);
}

TerrainLattice terrain_scene_lattice(const TerrainDesc& desc) noexcept {
  return scene_gen::scene_lattice(desc.extent, desc.size);
}

TerrainLattice terrain_ring_lattice(i64 spacing_mm) noexcept {
  return scene_gen::ring_lattice(spacing_mm);
}

u32 terrain_window_blocks(u32 nx, u32 nz) noexcept { return scene_gen::window_blocks(nx, nz); }

// The provider's re-evaluation entry (`scene_gen::GroundOps::evaluate`): for the dunes, one gather
// a block and the same heights as `height` (the generator's rule: any gather that covers a point
// gives it the same primitives, which meet by their maximum and add across bands).
bool evaluate_terrain_window(const TerrainSampler& sampler, f64 time_s,
                             const TerrainLattice& lattice, i32 i0, i32 j0, u32 nx, u32 nz,
                             u32 block_begin, u32 block_end, std::span<f32> heights) noexcept {
  return sampler.provider().evaluate(time_s, lattice, i0, j0, nx, nz, block_begin, block_end,
                                     heights);
}

f64 terrain_band_travel_m(const TerrainSampler& sampler, f64 from_s, f64 to_s) noexcept {
  return sampler.provider().travel_m(from_s, to_s);
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
  // The ground's grid on the scene's lattice (`scene_gen::GroundOps::grid`), whose points are the
  // ones computed below, to the bit: the dunes a block of vertices at a time (one gather per block
  // rather than one per vertex), the waves a point at a time, and either way the same heights as
  // `height`.
  Vector<f32> heights(n * n);
  field.provider().grid(terrain_scene_lattice(desc), 0, 0, n, n,
                        std::span<f32>(heights.data(), heights.size()));
  positions.resize(n * n);
  uvs.resize(n * n);
  for (u32 zi = 0; zi < n; ++zi) {
    const f32 v = static_cast<f32>(zi) / static_cast<f32>(n - 1);
    const f32 z = -extent + 2.0f * extent * v;
    for (u32 xi = 0; xi < n; ++xi) {
      const f32 u = static_cast<f32>(xi) / static_cast<f32>(n - 1);
      const f32 x = -extent + 2.0f * extent * u;
      positions[zi * n + xi] = Vec3{x, heights[zi * n + xi], z};
      uvs[zi * n + xi] = Vec2{u, v};
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
