// The sky mirror's sun against its sky on level ground (R182;
// docs/experiments/sun-and-sky-on-level-ground.md), with no device. What a shadow on open level
// ground keeps of the light beside it is the sky's diffuse light over the sun's and the sky's
// together, and both are the sky model's own: so the mirror (sky_reference.h, which is sky.slang in
// double) is integrated over its hemisphere here, by its sky-view table as the frame's sums read it
// and again by marching every direction without the table, and set beside three published
// clear-sky models at sun elevations of 10, 30, 50 and 70 degrees, for the aerosol the engine draws
// at four turbidities today and for the aerosol four Linke turbidities would name:
//
// - Bird and Hulstrom 1981 (SERI/TR-642-761): direct normal and diffuse horizontal irradiance from
//   the aerosol's optical depth at 380 and 500 nm, water vapour, ozone and the ground's albedo;
// - Ineichen and Perez 2002 (Solar Energy 73:151): the same from a Linke turbidity, which is what
//   the engine's `Sky.turbidity` says it is;
// - Perez, Ineichen, Seals, Michalsky and Stewart 1990 (Solar Energy 44:271): the luminous
//   efficacies of the direct and the diffuse light, which turn Bird's watts into the lux the
//   engine's sun units stand for (a blue sky is a quarter to a half more lumens a watt than the
//   sun's beam, so the diffuse share of illuminance is larger than that of irradiance).
//
// Their formulas were read on 2026-11-18 in two open implementations and not in the papers: pvlib's
// `clearsky.py` and `atmosphere.py` (Bird, Ineichen, the Kasten-Young air mass, Bird and Hulstrom's
// broadband aerosol depth, Kasten's Linke turbidity from Ineichen 2008) and Radiance's
// `gendaylit.c` (Perez's efficacy coefficients). The page says what each is and what was chosen.
//
// The case prints the table the page quotes and holds what the model does: its direct beam and its
// global-over-diffuse ratio stay within stated factors of the published ones **at the same
// aerosol**. The bounds are what was measured with a margin, and the page says which side of the
// published numbers the engine is on; a change to the sky's scattering or to the frame's sums moves
// the table, and a bound that then fails is the finding. What the case does not hold, because the
// engine does not meet it, is the turbidity's name: the air drawn for a turbidity T has the Linke
// turbidity 1.3 + 0.9 T (the table's `Linke` column), so its shadows are those of an air about one
// Linke unit hazier than the scene wrote. The page has the numbers and the design.
#include "sky_reference.h"

#include <domain/gfx/sky.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace engine;
namespace sref = engine::sky_ref;

namespace {

constexpr f64 k_pi64 = 3.14159265358979323846;
constexpr f64 k_rad = k_pi64 / 180.0;
constexpr f64 k_klux = static_cast<f64>(gfx::k_sky_sun_lux) * 1.0e-3;  // klux in one sun unit

f64 lum(sref::Dvec3 c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; }

// ---- the engine's air at a turbidity -----------------------------------------------------------
//
// domain/sky's `earth_atmosphere` (src/earth.cpp; sky.md, "The air"), written out: `gfx` is below
// the sky capability and its tests cannot link it. An aerosol whose vertical optical depth at
// 550 nm is (T - 1) times the clean air's 0.108, never less than Hillaire's own, over 1.2 km, with
// an Angstrom exponent of 0.8, a single-scattering albedo of 0.9 and an asymmetry of 0.76.
constexpr f64 k_angstrom = 0.8;
constexpr f64 k_least_aerosol = 4.40e-3 * 1.2;

// The aerosol's depth at 550 nm the engine gives a turbidity today.
f64 engine_aerosol_550(f64 turbidity) {
  return std::max((turbidity - 1.0) * 13.558e-3 * 8.0, k_least_aerosol);
}

// The same aerosol at another wavelength, by its Angstrom exponent.
f64 aerosol_at(f64 aod550, f64 nm) { return aod550 * std::pow(nm / 550.0, -k_angstrom); }

// The region's albedo the air sees: the scene schema's default (`Sky.ground_albedo`, 0.3).
constexpr f64 k_region_albedo = 0.3;

gfx::SkyParams earth_air(f64 aod550, f64 sun_elevation_deg) {
  gfx::SkyParams p;
  p.rayleigh = Vec4{5.802e-3f, 13.558e-3f, 33.1e-3f, 8.0f};
  const f32 ext_r = static_cast<f32>(aerosol_at(aod550, 680.0) / 1.2);
  const f32 ext_g = static_cast<f32>(aerosol_at(aod550, 550.0) / 1.2);
  const f32 ext_b = static_cast<f32>(aerosol_at(aod550, 440.0) / 1.2);
  p.mie_extinction = Vec4{ext_r, ext_g, ext_b, 0.76f};
  p.mie_scattering = Vec4{ext_r * 0.9f, ext_g * 0.9f, ext_b * 0.9f, 1.2f};
  p.ozone = Vec4{0.650e-3f, 1.881e-3f, 0.085e-3f, 25.0f};
  p.radii = Vec4{6360.0f, 6460.0f, 15.0f, 0.0f};
  const f32 region = static_cast<f32>(k_region_albedo);
  p.ground_albedo = Vec4{region, region, region, 0.0f};
  // The default terrain's sand, which G218's albedo view read back as 0.843, 0.687, 0.470.
  p.local_albedo = Vec4{0.84f, 0.69f, 0.47f, 0.0f};
  p.night = Vec4{1.4e-9f, 1.6e-9f, 1.9e-9f, 0.0f};
  const f64 el = sun_elevation_deg * k_rad;
  p.sun = Vec4{static_cast<f32>(std::cos(el)), static_cast<f32>(std::sin(el)), 0.0f, 4.65e-3f};
  p.sun_illuminance = Vec4{1.0f, 1.0f, 1.0f, 0.0f};
  p.moon = Vec4{0.0f, -1.0f, 0.0f, 4.52e-3f};
  p.camera = Vec4{0.0f, 1.7f, 0.0f, gfx::k_sky_min_altitude_km};
  p.celestial_x = Vec4{1.0f, 0.0f, 0.0f, 0.0f};
  p.celestial_y = Vec4{0.0f, 1.0f, 0.0f, 0.0f};
  p.celestial_z = Vec4{0.0f, 0.0f, 1.0f, 0.0f};
  // The renderer's defaults: the rule's knee at 40 klux, three quarters of a stop a stop below it.
  p.exposure = Vec4{gfx::k_sky_exposure_auto, 0.0f, 40000.0f, 0.75f};
  p.flags = gfx::k_sky_sun_disc;
  p.view_count = 1;
  return p;
}

// The light a level surface takes from the upper hemisphere: the integral of `radiance(dir)` times
// the direction's cosine, by the midpoint rule over `rows` of zenith angle and four times as many
// of azimuth.
template <typename Radiance>
sref::Dvec3 level_irradiance(u32 rows, Radiance&& radiance) {
  const u32 cols = 4 * rows;
  const f64 d_theta = 0.5 * k_pi64 / rows;
  const f64 d_phi = 2.0 * k_pi64 / cols;
  sref::Dvec3 sum{};
  for (u32 row = 0; row < rows; ++row) {
    const f64 theta = (row + 0.5) * d_theta;
    const f64 sn = std::sin(theta);
    const f64 cs = std::cos(theta);
    for (u32 col = 0; col < cols; ++col) {
      const f64 phi = (col + 0.5) * d_phi;
      const sref::Dvec3 dir{sn * std::cos(phi), cs, sn * std::sin(phi)};
      sum = sum + radiance(dir) * (cs * sn * d_theta * d_phi);
    }
  }
  return sum;
}

// The sky along a direction above the horizon with no sky-view table between: the mirror's own
// march from the eye to the top of the air.
sref::Dvec3 marched_radiance(const sref::Sky& s, const sref::Tables& tables, sref::Dvec3 dir) {
  const f64 h = s.altitude;
  const f64 r = s.bottom + h;
  const f64 t_max = sref::distance_to_top(s.bottom, s.top, r, h, dir.y);
  sref::Dvec3 radiance{};
  sref::Dvec3 throughput = sref::splat(1.0);
  sref::march(s, tables, r, h, dir, t_max, 96, radiance, throughput);
  return radiance;
}

// ---- the published models ----------------------------------------------------------------------

// Kasten and Young 1989's relative optical air mass (pvlib `get_relative_airmass`).
f64 air_mass(f64 zenith_deg) {
  return 1.0 / (std::cos(zenith_deg * k_rad) +
                0.50572 * std::pow(6.07995 + (90.0 - zenith_deg), -1.6364));
}

struct Irradiance {
  f64 direct_normal = 0.0, diffuse = 0.0, global = 0.0;  // W/m^2; the last two on a level surface
};

// What the published models are given beside the aerosol. pvlib's defaults where it has one (the
// extraterrestrial irradiance, the ozone column, the aerosol's forward-scattered share); 1.5 cm of
// precipitable water is this case's choice, a temperate summer's.
constexpr f64 k_extraterrestrial = 1364.0;  // W/m^2
constexpr f64 k_ozone_cm = 0.3;
constexpr f64 k_water_cm = 1.5;
constexpr f64 k_forward_share = 0.85;

// Bird and Hulstrom's broadband aerosol depth from the depths at 380 and 500 nm.
f64 broadband_aerosol_depth(f64 aod380, f64 aod500) { return 0.27583 * aod380 + 0.35 * aod500; }

// Bird and Hulstrom 1981, at sea level (pvlib `clearsky.bird`).
Irradiance bird(f64 zenith_deg, f64 aod380, f64 aod500, f64 albedo) {
  const f64 am = air_mass(zenith_deg);
  const f64 t_rayleigh = std::exp(-0.0903 * std::pow(am, 0.84) * (1.0 + am - std::pow(am, 1.01)));
  const f64 am_o3 = k_ozone_cm * am;
  const f64 t_ozone = 1.0 - 0.1611 * am_o3 * std::pow(1.0 + 139.48 * am_o3, -0.3034) -
                      0.002715 * am_o3 / (1.0 + 0.044 * am_o3 + 0.0003 * am_o3 * am_o3);
  const f64 t_gases = std::exp(-0.0127 * std::pow(am, 0.26));
  const f64 am_h2o = am * k_water_cm;
  const f64 t_water =
      1.0 - 2.4959 * am_h2o / (std::pow(1.0 + 79.034 * am_h2o, 0.6828) + 6.385 * am_h2o);
  const f64 tau = broadband_aerosol_depth(aod380, aod500);
  const f64 t_aerosol =
      std::exp(-std::pow(tau, 0.873) * (1.0 + tau - std::pow(tau, 0.7088)) * std::pow(am, 0.9108));
  const f64 taa = 1.0 - 0.1 * (1.0 - am + std::pow(am, 1.06)) * (1.0 - t_aerosol);
  const f64 rs = 0.0685 + (1.0 - k_forward_share) * (1.0 - t_aerosol / taa);
  Irradiance out;
  out.direct_normal =
      0.9662 * k_extraterrestrial * t_aerosol * t_water * t_gases * t_ozone * t_rayleigh;
  const f64 cos_z = std::cos(zenith_deg * k_rad);
  const f64 direct_level = out.direct_normal * cos_z;
  const f64 scattered = k_extraterrestrial * cos_z * 0.79 * t_ozone * t_gases * t_water * taa *
                        (0.5 * (1.0 - t_rayleigh) + k_forward_share * (1.0 - t_aerosol / taa)) /
                        (1.0 - am + std::pow(am, 1.02));
  out.global = (direct_level + scattered) / (1.0 - albedo * rs);
  out.diffuse = out.global - direct_level;
  return out;
}

// Ineichen and Perez 2002, at sea level (pvlib `clearsky.ineichen`, no enhancement).
Irradiance ineichen(f64 zenith_deg, f64 linke) {
  const f64 am = air_mass(zenith_deg);
  const f64 cos_z = std::cos(zenith_deg * k_rad);
  Irradiance out;
  out.global = 0.868 * k_extraterrestrial * cos_z * std::exp(-0.0387 * am * linke);
  const f64 beam = (0.664 + 0.163) * k_extraterrestrial * std::exp(-0.09 * am * (linke - 1.0));
  const f64 beam_2 = out.global * (1.0 - (0.1 - 0.2 * std::exp(-linke)) / (0.1 + 0.882)) / cos_z;
  out.direct_normal = std::min(beam, beam_2);
  out.diffuse = out.global - out.direct_normal * cos_z;
  return out;
}

// The Linke turbidity of an air with a broadband aerosol depth and a water column, at an air mass:
// Kasten's pyrheliometric formula with Molineaux's clean dry air and water vapour depths, as
// Ineichen 2008 wrote it (pvlib `kasten96_lt`).
f64 linke_turbidity(f64 am, f64 aod_broadband) {
  const f64 clean_dry = -0.101 + 0.235 * std::pow(am, -0.16);
  const f64 water = 0.112 * std::pow(am, -0.55) * std::pow(k_water_cm, 0.34);
  return (9.4 + 0.9 * am) * (clean_dry + water + aod_broadband);
}

// That relation turned round at an air mass of 2, where a Linke turbidity is quoted: the aerosol
// at 550 nm an air of Linke turbidity `linke` holds beside its water vapour and its clean dry
// gases, never less than the engine's least. What `Sky.turbidity` would give if it were the Linke
// turbidity its page says it is (the page's design; nothing in the engine computes it yet).
f64 linke_aerosol_550(f64 linke) {
  const f64 am = 2.0;
  const f64 clean_dry = -0.101 + 0.235 * std::pow(am, -0.16);
  const f64 water = 0.112 * std::pow(am, -0.55) * std::pow(k_water_cm, 0.34);
  const f64 broadband_per_550 =
      broadband_aerosol_depth(aerosol_at(1.0, 380.0), aerosol_at(1.0, 500.0));
  return std::max((linke / (9.4 + 0.9 * am) - clean_dry - water) / broadband_per_550,
                  k_least_aerosol);
}

// Perez et al. 1990's luminous efficacies, lumens a watt, of the diffuse light on a level surface
// and of the direct beam, by the sky's clearness bin (Radiance `gendaylit.c`,
// `glob_h_diffuse_effi_PEREZ` and `direct_n_effi_PEREZ`). The air mass is Kasten and Young's here,
// where Perez used Kasten's of 1966: under a hundredth apart above 10 degrees.
struct Efficacy {
  f64 diffuse = 0.0, direct = 0.0, clearness = 0.0;
};

Efficacy perez_efficacy(const Irradiance& e, f64 zenith_deg) {
  static constexpr f64 k_bounds[9] = {1.0, 1.065, 1.230, 1.500, 1.950, 2.800, 4.500, 6.200, 12.01};
  static constexpr f64 k_diffuse[4][8] = {
      {97.24, 107.22, 104.97, 102.39, 100.71, 106.42, 141.88, 152.23},
      {-0.46, 1.15, 2.96, 5.59, 5.94, 3.83, 1.90, 0.35},
      {12.00, 0.59, -5.53, -13.95, -22.75, -36.15, -53.24, -45.27},
      {-8.91, -3.95, -8.77, -13.90, -23.74, -28.83, -14.03, -7.98}};
  static constexpr f64 k_direct[4][8] = {
      {57.20, 98.99, 109.83, 110.34, 106.36, 107.19, 105.75, 101.18},
      {-4.55, -3.46, -4.90, -5.84, -3.97, -1.25, 0.77, 1.58},
      {-2.98, -1.21, -1.71, -1.99, -1.75, -1.51, -1.26, -1.10},
      {117.12, 12.38, -8.81, -4.56, -6.16, -26.73, -34.44, -8.29}};
  const f64 z = zenith_deg * k_rad;
  Efficacy out;
  out.clearness =
      ((e.diffuse + e.direct_normal) / e.diffuse + 1.041 * z * z * z) / (1.0 + 1.041 * z * z * z);
  const f64 brightness = e.diffuse * air_mass(zenith_deg) / k_extraterrestrial;
  u32 bin = 7;
  for (u32 i = 0; i < 8; ++i) {
    if (out.clearness < k_bounds[i + 1]) {
      bin = i;
      break;
    }
  }
  out.diffuse = k_diffuse[0][bin] + k_diffuse[1][bin] * k_water_cm +
                k_diffuse[2][bin] * std::cos(z) + k_diffuse[3][bin] * std::log(brightness);
  out.direct =
      std::max(k_direct[0][bin] + k_direct[1][bin] * k_water_cm +
                   k_direct[2][bin] * std::exp(5.73 * z - 5.0) + k_direct[3][bin] * brightness,
               0.0);
  return out;
}

// ---- one row of the table ----------------------------------------------------------------------

struct Row {
  // The engine's mirror, klux by the model's 128 klux a sun unit, luminance-weighted.
  f64 direct_normal = 0.0, direct_level = 0.0;
  f64 diffuse_table = 0.0;  // the sky-view table over the hemisphere, finely
  f64 diffuse_march = 0.0;  // every direction marched, no table
  f64 diffuse_frame = 0.0;  // pi times the frame's ambient at a level normal: what the resolve adds
  f64 dome = 0.0;           // pi times the sky's mean radiance: the meter's sky
  f64 wall = 0.0, under = 0.0;  // pi times the ambient at a wall's normal (across the sun) and down
  f64 sphere = 0.0;  // pi times the ambient's mean over every normal: a needle card's sphere
  f64 ground_in_wall = 0.0;  // the sunlit ground's share of `wall`
  f64 ground_in_sphere = 0.0;
  sref::Dvec3 diffuse_share{};  // diffuse over global, each channel
  f64 ratio = 0.0;              // global over diffuse on level ground, by the table's integral
  f64 ratio_drawn = 0.0;        // the same as the resolve draws it (the frame's harmonics)
  // Bird's irradiance with Perez's efficacies, klux; and the irradiance models' ratios, in watts
  // and (Ineichen's too through Perez's efficacies) in lux.
  f64 bird_direct_normal = 0.0, bird_direct_level = 0.0, bird_diffuse = 0.0, bird_ratio = 0.0;
  f64 bird_ratio_watts = 0.0;
  f64 named_ratio_watts = 0.0, named_ratio = 0.0;  // Ineichen at the turbidity the scene names
  f64 linke = 0.0;                                 // the Linke turbidity of the air drawn
  f64 linke_ratio_watts = 0.0, linke_ratio = 0.0;  // Ineichen at that
};

f64 lux_ratio(const Irradiance& e, f64 zenith_deg) {
  const Efficacy k = perez_efficacy(e, zenith_deg);
  const f64 direct = e.direct_normal * k.direct * std::cos(zenith_deg * k_rad);
  const f64 diffuse = e.diffuse * k.diffuse;
  return (direct + diffuse) / diffuse;
}

// `aod550`: the aerosol drawn; `named`: the turbidity a scene would have written for it.
Row measure(f64 aod550, f64 named, f64 elevation_deg, const sref::Tables& air) {
  const sref::Sky s = sref::from_params(earth_air(aod550, elevation_deg));
  sref::Tables tables;
  tables.transmittance = air.transmittance;
  tables.multiscatter = air.multiscatter;
  tables.sky_view = sref::build_sky_view(s, tables);
  const sref::Frame f = sref::frame(s, tables);
  Row row;
  const sref::Dvec3 direct = f.sun_ground * s.sun.y;
  const sref::Dvec3 table =
      level_irradiance(96, [&](sref::Dvec3 d) { return sref::view_radiance(s, tables, d); });
  const sref::Dvec3 march =
      level_irradiance(48, [&](sref::Dvec3 d) { return marched_radiance(s, tables, d); });
  row.direct_normal = k_klux * lum(f.sun_ground);
  row.direct_level = k_klux * lum(direct);
  row.diffuse_table = k_klux * lum(table);
  row.diffuse_march = k_klux * lum(march);
  row.diffuse_frame = k_klux * k_pi64 * lum(sref::ambient(f, {0.0, 1.0, 0.0}));
  row.dome = f.sky_lux * 1.0e-3;
  row.wall = k_klux * k_pi64 * lum(sref::ambient(f, {0.0, 0.0, 1.0}));
  row.under = k_klux * k_pi64 * lum(sref::ambient(f, {0.0, -1.0, 0.0}));
  // The mean over every normal is the first harmonic alone; the ground's uniform lower half is
  // half of what a wall's cosine lobe and of what the whole sphere's take.
  row.sphere = k_klux * k_pi64 * lum(f.sh[0]) * 0.282095;
  const f64 ground_half = k_klux * k_pi64 * lum(f.ground) * 0.5;
  row.ground_in_wall = ground_half / row.wall;
  row.ground_in_sphere = ground_half / row.sphere;
  row.diffuse_share = sref::div3(table, table + direct);
  row.ratio = (row.direct_level + row.diffuse_table) / row.diffuse_table;
  row.ratio_drawn = (row.direct_level + row.diffuse_frame) / row.diffuse_frame;

  const f64 zenith = 90.0 - elevation_deg;
  const f64 aod380 = aerosol_at(aod550, 380.0);
  const f64 aod500 = aerosol_at(aod550, 500.0);
  const Irradiance b = bird(zenith, aod380, aod500, k_region_albedo);
  const Efficacy k = perez_efficacy(b, zenith);
  row.bird_direct_normal = b.direct_normal * k.direct * 1.0e-3;
  row.bird_direct_level = row.bird_direct_normal * std::sin(elevation_deg * k_rad);
  row.bird_diffuse = b.diffuse * k.diffuse * 1.0e-3;
  row.bird_ratio = (row.bird_direct_level + row.bird_diffuse) / row.bird_diffuse;
  row.bird_ratio_watts = b.global / b.diffuse;
  const Irradiance at_named = ineichen(zenith, named);
  row.named_ratio_watts = at_named.global / at_named.diffuse;
  row.named_ratio = lux_ratio(at_named, zenith);
  row.linke = linke_turbidity(air_mass(zenith), broadband_aerosol_depth(aod380, aod500));
  const Irradiance at_linke = ineichen(zenith, row.linke);
  row.linke_ratio_watts = at_linke.global / at_linke.diffuse;
  row.linke_ratio = lux_ratio(at_linke, zenith);
  return row;
}

// One aerosol's four elevations into `text`, each row held to what the mirror does.
void tabulate(std::string& text, const char* label, f64 named, f64 aod550) {
  // The two tables that are the air's alone, built once an aerosol.
  const sref::Sky air_sky = sref::from_params(earth_air(aod550, 45.0));
  sref::Tables air;
  air.transmittance = sref::build_transmittance(air_sky);
  air.multiscatter = sref::build_multiscatter(air_sky, air);
  for (const f64 elevation : {10.0, 30.0, 50.0, 70.0}) {
    const Row r = measure(aod550, named, elevation, air);
    char line[640];
    std::snprintf(
        line, sizeof(line),
        "%s %4.1f %5.3f %3.0f | %6.1f %6.1f %6.2f %6.2f %6.2f %6.2f | %6.2f %6.2f %6.2f  %4.2f  "
        "%4.2f | "
        "%.3f %.3f %.3f | %5.2f %5.2f | %6.1f %6.1f %6.2f %5.2f | %5.2f  %5.2f %5.2f  %4.2f "
        "%5.2f %5.2f\n",
        label, named, aod550, elevation, r.direct_normal, r.direct_level, r.diffuse_table,
        r.diffuse_march, r.diffuse_frame, r.dome, r.wall, r.under, r.sphere, r.ground_in_wall,
        r.ground_in_sphere, r.diffuse_share.x, r.diffuse_share.y, r.diffuse_share.z, r.ratio,
        r.ratio_drawn, r.bird_direct_normal, r.bird_direct_level, r.bird_diffuse, r.bird_ratio,
        r.bird_ratio_watts, r.named_ratio_watts, r.named_ratio, r.linke, r.linke_ratio_watts,
        r.linke_ratio);
    text += line;
    CAPTURE(named);
    CAPTURE(aod550);
    CAPTURE(elevation);
    // The sky-view table carries the hemisphere: its integral is the march's, to 3 % (measured:
    // under 1 %).
    CHECK(r.diffuse_table == doctest::Approx(r.diffuse_march).epsilon(0.03));
    // Nine harmonics hold a level surface's share of it to 6 % (measured: 5 % at 10 degrees, where
    // the sky is a bright ring at the horizon, and 2 % from 30 up).
    CHECK(r.diffuse_frame == doctest::Approx(r.diffuse_table).epsilon(0.06));
    // **The beam.** With the sun 30 degrees up or more and an aerosol up to 0.25 at 550 nm, the
    // mirror's direct normal light is Bird's through Perez's efficacy to a tenth (measured: 2 to
    // 7 % over). Below that and in a thicker haze it is held only to stay within 0.6 to 1.25 of
    // it: three wavelengths are not a spectrum (at 10 degrees and the thickest haze the mirror is
    // 0.65 of Bird's).
    const f64 beam = r.direct_normal / r.bird_direct_normal;
    CHECK(beam > 0.6);
    CHECK(beam < 1.25);
    if (elevation >= 30.0 && aod550 <= 0.25) {
      CHECK(beam == doctest::Approx(1.0).epsilon(0.10));
    }
    // **The balance.** Global over diffuse on a level surface, which is what a sunlit point is
    // over a shadowed one on open ground: the mirror's stays within 0.85 to 1.22 of Bird's with
    // Perez's efficacies at the same aerosol (measured 0.87 to 1.19: the mirror has less sky than
    // Bird at a low sun and more at a high one).
    const f64 balance = r.ratio / r.bird_ratio;
    CHECK(balance > 0.85);
    CHECK(balance < 1.22);
  }
}

}  // namespace

TEST_CASE("sky: the sun against the sky on level ground, against published clear-sky models") {
  std::string text =
      "\nklux, luminance-weighted, 128 klux a sun unit. T: the turbidity a scene names; aod: the "
      "aerosol's depth at 550 nm drawn for it. dn, dh: the sun's beam, normal and on the level. "
      "table, march, frame: the sky on the level by the sky-view table, by marching, and as the "
      "resolve adds it (pi x ambient). dome: the meter's sky. wall, under, sphere: pi x ambient at "
      "a wall's normal, facing down and over every normal (sand 0.84, 0.69, 0.47); gw, gs: the "
      "sunlit ground's share of wall and of sphere. share: diffuse over global by channel. G/D: "
      "global over diffuse on the level, by the table and as drawn. Bird+Perez: the same aerosol "
      "through Bird's model and Perez's efficacies. W: G/D in watts (Bird; Ineichen at T), lux: "
      "Ineichen at T through Perez. Linke: the Linke turbidity of the air drawn, and Ineichen at "
      "it in watts and lux.\n"
      "      T   aod  el |     dn     dh  table  march  frame   dome |   wall  under sphere    gw"
      "    gs | share r g b       |   G/D drawn | B+P dn     dh    dif   G/D | W Bird  I(T)   "
      "lux Linke  I(L)   lux\n";
  // The engine's air as it is drawn today, at the schema's default turbidity (2), the test
  // scenes' (2.5), and the page's "clear" and "hazy" (3, 5).
  for (const f64 turbidity : {2.0, 2.5, 3.0, 5.0}) {
    tabulate(text, "now  ", turbidity, engine_aerosol_550(turbidity));
  }
  // The air the same numbers would name as Linke turbidities over 1.5 cm of water (the page's
  // design): the mirror is held to the published models there as well.
  for (const f64 linke : {2.5, 3.0, 4.0, 5.0}) {
    tabulate(text, "linke", linke, linke_aerosol_550(linke));
  }
  MESSAGE(text);
}
