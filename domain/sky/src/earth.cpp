#include <domain/sky/earth.h>

#include <algorithm>
#include <cmath>

namespace engine::sky {

namespace {

constexpr f64 k_pi = 3.14159265358979323846;
constexpr f64 k_rad = k_pi / 180.0;

// The clean air's vertical optical depth at 550 nm: the Rayleigh scattering coefficient there,
// 13.558e-3 per km, times its 8 km scale height.
constexpr f64 k_rayleigh_depth_550 = 13.558e-3 * 8.0;
constexpr f64 k_aerosol_height_km = 1.2;
constexpr f64 k_aerosol_albedo = 0.9;
constexpr f64 k_aerosol_angstrom = 0.8;
constexpr f64 k_aerosol_g = 0.76;
// Hillaire's own aerosol, the least the air is given: a vertical depth of 0.0053 at 550 nm.
constexpr f64 k_least_aerosol_depth = 4.40e-3 * 1.2;

// A small generator with a fixed arithmetic, so the table is the same wherever it is built.
struct Pcg {
  u64 state = 0;
  u32 next() noexcept {
    const u64 old = state;
    state = old * 6364136223846793005ull + 1442695040888963407ull;
    const u32 xorshifted = static_cast<u32>(((old >> 18u) ^ old) >> 27u);
    const u32 rot = static_cast<u32>(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
  }
  f64 uniform() noexcept { return (static_cast<f64>(next()) + 0.5) / 4294967296.0; }
  f64 normal() noexcept {
    const f64 u1 = uniform();
    const f64 u2 = uniform();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * k_pi * u2);
  }
};

// A blackbody's radiance at a wavelength (nm) and temperature, up to a constant.
f64 planck(f64 nm, f64 kelvin) noexcept {
  const f64 l = nm * 1.0e-9;
  return 1.0 / (l * l * l * l * l * (std::exp(1.4388e-2 / (l * kelvin)) - 1.0));
}

// A B - V colour index's colour: its temperature (Ballesteros 2012), then a blackbody at the three
// wavelengths the renderer's channels stand for, relative to a 6,500 K one (white), with a
// luminance of 1.
Vec3 star_color(f64 bv) noexcept {
  const f64 t = 4600.0 * (1.0 / (0.92 * bv + 1.7) + 1.0 / (0.92 * bv + 0.62));
  const f64 r = planck(610.0, t) / planck(610.0, 6500.0);
  const f64 g = planck(550.0, t) / planck(550.0, 6500.0);
  const f64 b = planck(465.0, t) / planck(465.0, 6500.0);
  const f64 lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
  return Vec3{static_cast<f32>(r / lum), static_cast<f32>(g / lum), static_cast<f32>(b / lum)};
}

}  // namespace

scene_gen::Atmosphere earth_atmosphere(f64 turbidity) noexcept {
  scene_gen::Atmosphere air;
  const f64 t = std::clamp(turbidity, 1.0, 10.0);
  const f64 depth = std::max((t - 1.0) * k_rayleigh_depth_550, k_least_aerosol_depth);
  const f64 extinction_550 = depth / k_aerosol_height_km;
  const f64 nm[3] = {680.0, 550.0, 440.0};
  f32 ext[3];
  for (u32 c = 0; c < 3; ++c) {
    ext[c] = static_cast<f32>(extinction_550 * std::pow(nm[c] / 550.0, -k_aerosol_angstrom));
  }
  air.mie_extinction = Vec3{ext[0], ext[1], ext[2]};
  air.mie_scattering = air.mie_extinction * static_cast<f32>(k_aerosol_albedo);
  air.mie_height_km = static_cast<f32>(k_aerosol_height_km);
  air.mie_g = static_cast<f32>(k_aerosol_g);
  return air;
}

u32 star_count_for_magnitude(f64 magnitude_limit) noexcept {
  return static_cast<u32>(std::floor(std::pow(10.0, 0.68 + 0.5 * magnitude_limit)));
}

f64 star_irradiance(f64 magnitude) noexcept { return std::pow(10.0, -0.4 * (magnitude + 26.74)); }

Vector<scene_gen::Star> earth_stars(u32 seed, f64 magnitude_limit) {
  const u32 count = star_count_for_magnitude(magnitude_limit);
  Vector<scene_gen::Star> out;
  out.reserve(count);
  Pcg rng{static_cast<u64>(seed) * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull};
  // The north galactic pole, in the celestial frame: right ascension 192.859 degrees, declination
  // 27.128 degrees.
  const f64 pole_ra = 192.85948 * k_rad;
  const f64 pole_dec = 27.12825 * k_rad;
  const f64 pole[3] = {std::cos(pole_dec) * std::cos(pole_ra),
                       std::cos(pole_dec) * std::sin(pole_ra), std::sin(pole_dec)};
  for (u32 k = 0; k < count; ++k) {
    // The k-th brightest has k stars brighter than it: the counts' law inverted.
    const f64 m = (std::log10(static_cast<f64>(k) + 1.0) - 0.68) / 0.5;
    // How much the fainter stars crowd the Milky Way: a Gaussian 15 degrees wide in galactic
    // latitude, up to three and a half times the density far from it at the table's end.
    const f64 crowd = 0.3 + 0.5 * std::max(m - 2.0, 0.0);
    f64 d[3] = {0.0, 0.0, 1.0};
    for (u32 attempt = 0; attempt < 64; ++attempt) {
      const f64 z = 2.0 * rng.uniform() - 1.0;
      const f64 a = 2.0 * k_pi * rng.uniform();
      const f64 s = std::sqrt(std::max(1.0 - z * z, 0.0));
      d[0] = s * std::cos(a);
      d[1] = s * std::sin(a);
      d[2] = z;
      const f64 b =
          std::asin(std::clamp(d[0] * pole[0] + d[1] * pole[1] + d[2] * pole[2], -1.0, 1.0));
      const f64 band = b / (15.0 * k_rad);
      const f64 accept = (1.0 + crowd * std::exp(-band * band)) / (1.0 + crowd);
      if (rng.uniform() < accept) break;
    }
    // Two populations of naked-eye colour: white main-sequence A stars and orange K giants.
    const f64 bv = rng.uniform() < 0.45 ? 0.05 + 0.2 * rng.normal() : 1.1 + 0.3 * rng.normal();
    scene_gen::Star star;
    star.direction = Vec3{static_cast<f32>(d[0]), static_cast<f32>(d[1]), static_cast<f32>(d[2])};
    star.magnitude = static_cast<f32>(m);
    star.color = star_color(std::clamp(bv, -0.3, 1.9));
    out.push_back(star);
  }
  return out;
}

}  // namespace engine::sky
