#include "geology.h"

#include <domain/rocks/rocks.h>

namespace engine::rocks {

RockFamily rock_family(RockType type) noexcept {
  switch (type) {
    case RockType::Sandstone:
    case RockType::Limestone:
    case RockType::Shale: return RockFamily::Sedimentary;
    case RockType::Slate:
    case RockType::Schist:
    case RockType::Gneiss:
    case RockType::Marble: return RockFamily::Metamorphic;
    default: return RockFamily::Igneous;
  }
}

namespace detail {

Lithology lithology(RockType type) {
  // bed, fold, flaking, dissolution, granular, plate, conchoidal scars.
  switch (type) {
    case RockType::Sandstone: return {0.32, 0.02, 0.35, 0.05, 0.9, 0.55, 0.0};
    case RockType::Limestone: return {0.55, 0.02, 0.25, 0.9, 0.15, 0.65, 0.0};
    case RockType::Shale: return {0.09, 0.04, 0.95, 0.05, 0.5, 0.18, 0.0};
    case RockType::Slate: return {0.12, 0.02, 0.9, 0.0, 0.1, 0.16, 0.0};
    case RockType::Schist: return {0.16, 0.3, 0.85, 0.05, 0.3, 0.3, 0.0};
    case RockType::Gneiss: return {0.28, 0.65, 0.3, 0.05, 0.35, 0.65, 0.0};
    case RockType::Marble: return {0.0, 0.0, 0.15, 0.7, 0.25, 0.7, 0.0};
    case RockType::Obsidian: return {0.0, 0.0, 0.55, 0.0, 0.02, 0.5, 1.0};
    case RockType::Basalt: return {0.0, 0.0, 0.3, 0.05, 0.3, 0.8, 0.1};
    default: return {0.0, 0.0, 0.6, 0.05, 0.65, 0.8, 0.0};
  }
}

Fabric make_fabric(RockType type, const RockGeology& g, u64 seed) {
  Fabric f{};
  f.model = lithology(type);
  f.model.bed = wide(g.bed_m.value_or(static_cast<f32>(f.model.bed)));
  f.model.fold = wide(g.fold.value_or(static_cast<f32>(f.model.fold)));
  f.model.flaking = wide(g.flaking.value_or(static_cast<f32>(f.model.flaking)));
  f.model.dissolution = wide(g.dissolution.value_or(static_cast<f32>(f.model.dissolution)));
  f.model.granular = wide(g.granular.value_or(static_cast<f32>(f.model.granular)));
  const f64 dip = wide(g.dip_deg) * k_pi / 180.0;
  const f64 azimuth = wide(g.azimuth_deg) * k_pi / 180.0;
  f.tangent = D3{det::cos(azimuth), 0.0, det::sin(azimuth)};
  f.normal = f.tangent * det::sin(dip) + D3{0.0, det::cos(dip), 0.0};
  f.exposure = wide(g.weathering);
  f.irregularity = wide(g.irregularity);
  f.seed = mix64(seed ^ 0x47454F4C4F4759ull);
  return f;
}

f64 fabric_coordinate(const Fabric& f, D3 p) {
  const f64 wave = f.model.bed * 12.0 + 0.5;
  const f64 along = dot(p, f.tangent) / wave;
  const f64 phase = 6.0 * lattice_value(f.seed, 0, 0, 0);
  return dot(p, f.normal) + f.model.fold * wave * 0.25 *
                                (det::sin(along * 2.0 * k_pi + phase) +
                                 0.28 * det::sin(along * 4.0 * k_pi + p.y / wave));
}

f64 fabric_recess(const Fabric& f, D3 p, f64 cell) {
  const auto resolved = [cell](f64 width) { return fade(clamp01((width / cell - 2.0) * 0.5)); };
  const Lithology& m = f.model;
  f64 r = 0.0;
  if (m.bed > 0.0) {
    // Unequal retreat of strata/cleavage sheets; broad plate faces and short risers, not a
    // sinusoidal corrugation. A smooth interval preserves closedness at coarse source rungs.
    const f64 t = fabric_coordinate(f, p) / m.bed;
    const f64 k = std::floor(t);
    const f64 u = t - k;
    const f64 authored_edge = 0.09 + 0.18 * f.exposure;
    const f64 sampled_edge = 2.0 * cell / m.bed;
    const f64 edge = authored_edge > sampled_edge ? authored_edge : sampled_edge;
    const f64 blend = fade(clamp01(u / edge));
    const f64 a = lattice_value(f.seed, static_cast<i64>(k) - 1, 11, 9);
    const f64 b = lattice_value(f.seed, static_cast<i64>(k), 11, 9);
    const f64 retreat = lerp(a, b, blend);
    const f64 patch = fade(clamp01((noise3(f.seed + 5u, p * (0.55 / m.bed)) - 0.25) * 2.0));
    r += resolved(m.bed) * m.bed * (0.15 + 0.85 * f.irregularity) * (0.25 + 0.75 * f.exposure) *
         (0.65 * retreat + m.flaking * patch * (0.35 + 0.45 * retreat));
  }
  // Local, asymmetric solution pockets. These are depressions, not unsupported holes.
  const f64 pocket = 0.32 + 0.5 * f.exposure;
  const f64 n = noise3(f.seed + 19u, p * (1.0 / pocket));
  const f64 cavity = fade(clamp01((n - 0.42) * 3.2));
  r += resolved(pocket * 0.5) * m.dissolution * f.exposure * pocket * 0.32 * cavity;
  // Granular loss is only the mesoscopic component. Fine grains belong in the PBR normal map.
  f64 wavelength = 0.28;
  f64 amplitude = 0.022 * m.granular * (0.2 + 0.8 * f.exposure);
  for (u32 octave = 0; octave < 3; ++octave) {
    r += resolved(wavelength) * amplitude *
         (2.0 * noise3(f.seed + 37u + octave, p * (1.0 / wavelength)) - 1.0);
    wavelength *= 0.5;
    amplitude *= 0.45;
  }
  return r;
}

bool valid_rock_type(RockType type) {
  return static_cast<u32>(type) <= static_cast<u32>(RockType::Obsidian);
}
bool valid_quality(RockQuality q) {
  return q == RockQuality::Low || q == RockQuality::Medium || q == RockQuality::High;
}
u32 quality_budget(u32 budget, RockQuality q, u32 floor) {
  const u32 scaled = q == RockQuality::Low
                         ? budget / 10u
                         : (q == RockQuality::Medium ? budget * 35u / 100u : budget);
  const u32 wanted = scaled < floor ? floor : scaled;
  return wanted < budget ? wanted : budget;
}

bool geology_in_range(const RockGeology& g, std::string& error) {
  const auto check = [&](f32 v, f32 lo, f32 hi, const char* field) {
    if (v >= lo && v <= hi) return true;
    error = std::string("engine.rocks.RockGeology.") + field + " is out of range";
    return false;
  };
  return check(g.weathering, 0, 1, "weathering") && check(g.irregularity, 0, 1, "irregularity") &&
         (!g.bed_m || *g.bed_m == 0.0f || check(*g.bed_m, 0.01f, 20, "bed_m")) &&
         check(g.dip_deg, -85, 85, "dip_deg") && check(g.azimuth_deg, -180, 180, "azimuth_deg") &&
         (!g.fold || check(*g.fold, 0, 1, "fold")) &&
         (!g.flaking || check(*g.flaking, 0, 1, "flaking")) &&
         (!g.dissolution || check(*g.dissolution, 0, 1, "dissolution")) &&
         (!g.granular || check(*g.granular, 0, 1, "granular"));
}

void material_factors(const RockMaterial& m, Vec4& colour, f32& roughness) {
  if (m.tint) colour = Vec4{(*m.tint)[0], (*m.tint)[1], (*m.tint)[2], 1.0f};
  if (m.roughness_factor) roughness = *m.roughness_factor;
}
bool material_factors_in_range(const RockMaterial& m, std::string& error) {
  if (m.tint) {
    for (f32 c : *m.tint) {
      if (!(c >= 0.0f && c <= 1.0f)) {
        error = "engine.rocks.RockMaterial.tint is out of range (linear RGB 0 to 1)";
        return false;
      }
    }
  }
  if (m.roughness_factor && !(*m.roughness_factor >= 0.02f && *m.roughness_factor <= 1.0f)) {
    error = "engine.rocks.RockMaterial.roughness_factor is out of range (0.02 to 1)";
    return false;
  }
  return true;
}

}  // namespace detail
}  // namespace engine::rocks
