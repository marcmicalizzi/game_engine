#pragma once

// The CPU mirror of domain/gfx/shaders/sky.slang and sky_luts.slang, in double precision: the same
// air (Rayleigh, Mie, the ozone tent), the same rays by radius and zenith cosine with the same
// height-along-a-ray formula, the same four tables in the same parameterizations with the same
// bilinear reads, the same frame's sums (nine harmonics, the ground, the lights at the eye, the
// exposure), the same discs and star spots, and the same direction through a pixel
// (view_ray.slang). `sky_tests.cpp` builds the tables on the GPU and here and holds each texel,
// each sum and each pixel of a resolve to this; the renderer's sky tests hold the reference path
// tracer's background to it. Changing sky.slang means changing this file in the same commit; the
// tests then say by how much the two disagree.
//
// Like brdf_reference.h it is a second implementation, not the first one compiled twice: what it
// shares with the shader is the formulas and their order, never code.

#include "brdf_reference.h"

#include <domain/gfx/sky.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace engine::sky_ref {

using brdf_ref::dot;
using brdf_ref::Dvec3;
using brdf_ref::dvec3;
using brdf_ref::normalize;
using brdf_ref::splat;

inline constexpr double k_pi = 3.14159265358979323846;

struct Dvec4 {
  double x = 0.0, y = 0.0, z = 0.0, w = 0.0;
};
inline Dvec4 operator+(Dvec4 a, Dvec4 b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
inline Dvec4 operator*(Dvec4 a, double s) { return {a.x * s, a.y * s, a.z * s, a.w * s}; }
inline Dvec4 lerp4(Dvec4 a, Dvec4 b, double t) { return a + (b + a * -1.0) * t; }
inline Dvec3 rgb(Dvec4 v) { return {v.x, v.y, v.z}; }
inline Dvec3 exp3(Dvec3 v) { return {std::exp(v.x), std::exp(v.y), std::exp(v.z)}; }
inline Dvec3 div3(Dvec3 a, Dvec3 b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }
inline Dvec3 max3(Dvec3 a, double m) {
  return {a.x > m ? a.x : m, a.y > m ? a.y : m, a.z > m ? a.z : m};
}
inline double saturate(double v) { return brdf_ref::clamp01(v); }
// sky.slang's `sky_absorbed`: 1 - exp(-x), by the same series below 0.01.
inline double absorbed1(double x) {
  return x < 1e-2 ? x * (1.0 - x * (0.5 - x * (1.0 / 6.0))) : 1.0 - std::exp(-x);
}
inline Dvec3 absorbed(Dvec3 x) { return {absorbed1(x.x), absorbed1(x.y), absorbed1(x.z)}; }
inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The frame's block in double: what `gfx::SkyParams` says, widened.
struct Sky {
  Dvec3 rayleigh;
  double rayleigh_height = 8.0;
  Dvec3 mie_scattering;
  double mie_height = 1.2;
  Dvec3 mie_extinction;
  double mie_g = 0.8;
  Dvec3 ozone;
  double ozone_peak = 25.0;
  double bottom = 6360.0;
  double top = 6460.0;
  double ozone_half_width = 15.0;
  Dvec3 ground_albedo;
  Dvec3 local_albedo;
  Dvec3 night;
  Dvec3 sun;
  double sun_radius = 0.0;
  Dvec3 sun_illuminance;
  Dvec3 moon;
  double moon_radius = 0.0;
  Dvec3 moon_illuminance;
  double moon_albedo = 0.0;
  Dvec3 camera;
  double altitude = static_cast<double>(gfx::k_sky_min_altitude_km);
  Dvec3 celestial_x, celestial_y, celestial_z;
  double exposure_mode = 0.0, exposure_ev = 0.0, exposure_knee = 10.0, exposure_slope = 0.5;
  double star_scale = 1.0;
  double shoulder = 1.0;
  u32 flags = 0;
};

inline Sky from_params(const gfx::SkyParams& p) {
  Sky s;
  s.rayleigh = dvec3(p.rayleigh);
  s.rayleigh_height = static_cast<double>(p.rayleigh.w);
  s.mie_scattering = dvec3(p.mie_scattering);
  s.mie_height = static_cast<double>(p.mie_scattering.w);
  s.mie_extinction = dvec3(p.mie_extinction);
  s.mie_g = static_cast<double>(p.mie_extinction.w);
  s.ozone = dvec3(p.ozone);
  s.ozone_peak = static_cast<double>(p.ozone.w);
  s.bottom = static_cast<double>(p.radii.x);
  s.top = static_cast<double>(p.radii.y);
  s.ozone_half_width = static_cast<double>(p.radii.z);
  s.ground_albedo = dvec3(p.ground_albedo);
  s.local_albedo = dvec3(p.local_albedo);
  s.night = dvec3(p.night);
  s.sun = dvec3(p.sun);
  s.sun_radius = static_cast<double>(p.sun.w);
  s.sun_illuminance = dvec3(p.sun_illuminance);
  s.moon = dvec3(p.moon);
  s.moon_radius = static_cast<double>(p.moon.w);
  s.moon_illuminance = dvec3(p.moon_illuminance);
  s.moon_albedo = static_cast<double>(p.moon_illuminance.w);
  s.camera = dvec3(p.camera);
  s.altitude = static_cast<double>(p.camera.w);
  s.celestial_x = dvec3(p.celestial_x);
  s.celestial_y = dvec3(p.celestial_y);
  s.celestial_z = dvec3(p.celestial_z);
  s.exposure_mode = static_cast<double>(p.exposure.x);
  s.exposure_ev = static_cast<double>(p.exposure.y);
  s.exposure_knee = static_cast<double>(p.exposure.z);
  s.exposure_slope = static_cast<double>(p.exposure.w);
  s.star_scale = static_cast<double>(p.stars.y);
  s.shoulder = static_cast<double>(p.shoulder);
  s.flags = p.flags;
  return s;
}

// sky.slang's `sky_tone1`: the display's shoulder on one exposed channel.
inline double tone(double x, double s) {
  if (x <= s || s >= 1.0) return x;
  return s + (1.0 - s) * (1.0 - std::exp(-(x - s) / (1.0 - s)));
}

// ---- the air ------------------------------------------------------------------------------------

struct Medium {
  Dvec3 rayleigh, mie, extinction;
};

inline Medium medium(const Sky& s, double height) {
  const double h = height > 0.0 ? height : 0.0;
  const double dr = std::exp(-h / s.rayleigh_height);
  const double dm = std::exp(-h / s.mie_height);
  const double dz = std::max(0.0, 1.0 - std::fabs(h - s.ozone_peak) / s.ozone_half_width);
  Medium m;
  m.rayleigh = s.rayleigh * dr;
  m.mie = s.mie_scattering * dm;
  m.extinction = m.rayleigh + s.mie_extinction * dm + s.ozone * dz;
  return m;
}

inline double phase_rayleigh(double nu) { return 3.0 / (16.0 * k_pi) * (1.0 + nu * nu); }

inline double phase_mie(double nu, double g) {
  const double g2 = g * g;
  const double k = 3.0 / (8.0 * k_pi) * (1.0 - g2) / (2.0 + g2);
  const double denom = 1.0 + g2 - 2.0 * g * nu;
  return k * (1.0 + nu * nu) / (denom * std::sqrt(denom));
}

// ---- rays ---------------------------------------------------------------------------------------

inline double height_along(double bottom, double r0, double h0, double mu, double t) {
  const double r2_less = h0 * (2.0 * bottom + h0) + t * (t + 2.0 * r0 * mu);
  const double r = std::sqrt(std::max(bottom * bottom + r2_less, 0.0));
  return r2_less / (r + bottom);
}

inline bool hits_ground(double bottom, double r, double h, double mu) {
  return mu < 0.0 && r * r * mu * mu >= h * (2.0 * bottom + h);
}

inline double distance_to_ground(double bottom, double r, double h, double mu) {
  const double c = h * (2.0 * bottom + h);
  const double disc = r * r * mu * mu - c;
  const double far = -r * mu + std::sqrt(std::max(disc, 0.0));
  return far > 0.0 ? std::max(c, 0.0) / far : 0.0;
}

inline double distance_to_top(double bottom, double top, double r, double h, double mu) {
  const double room = (top - bottom) * (top + bottom) - h * (2.0 * bottom + h);
  const double root = std::sqrt(std::max(r * r * mu * mu + room, 0.0));
  if (mu > 0.0) return std::max(room, 0.0) / std::max(r * mu + root, 1e-9);
  return std::max(0.0, -r * mu + root);
}

inline double tex_from_unit(double x, double n) { return 0.5 / n + x * (1.0 - 1.0 / n); }
inline double unit_from_tex(double u, double n) { return (u - 0.5 / n) / (1.0 - 1.0 / n); }

// A table in double, float4 texels in the GPU's order.
struct Table {
  u32 width = 0;
  u32 height = 0;
  std::vector<Dvec4> texels;
  Dvec4 at(i32 x, i32 y) const { return texels[static_cast<usize>(y) * width + x]; }
};

inline Dvec4 bilinear(const Table& t, double u, double v, bool wrap_x) {
  const double px = u * t.width - 0.5;
  const double py = v * t.height - 0.5;
  const double fx = std::floor(px);
  const double fy = std::floor(py);
  const double wx = px - fx;
  const double wy = py - fy;
  const i32 w = static_cast<i32>(t.width);
  i32 x0 = static_cast<i32>(fx);
  i32 x1 = x0 + 1;
  if (wrap_x) {
    x0 = x0 < 0 ? x0 + w : (x0 >= w ? x0 - w : x0);
    x1 = x1 < 0 ? x1 + w : (x1 >= w ? x1 - w : x1);
  } else {
    x0 = std::clamp(x0, 0, w - 1);
    x1 = std::clamp(x1, 0, w - 1);
  }
  const i32 y0 = std::clamp(static_cast<i32>(fy), 0, static_cast<i32>(t.height) - 1);
  const i32 y1 = std::clamp(static_cast<i32>(fy) + 1, 0, static_cast<i32>(t.height) - 1);
  return lerp4(lerp4(t.at(x0, y0), t.at(x1, y0), wx), lerp4(t.at(x0, y1), t.at(x1, y1), wx), wy);
}

struct Tables {
  Table transmittance, multiscatter, sky_view, aerial;  // aerial: height = 32 * slices
};

// ---- transmittance ------------------------------------------------------------------------------

inline void transmittance_uv(const Sky& s, double r, double h, double mu, double& u, double& v) {
  const double big_h = std::sqrt((s.top - s.bottom) * (s.top + s.bottom));
  const double rho = std::sqrt(std::max(h * (2.0 * s.bottom + h), 0.0));
  const double d = distance_to_top(s.bottom, s.top, r, h, mu);
  const double d_min = s.top - r;
  const double d_max = rho + big_h;
  const double x_mu = (d - d_min) / std::max(d_max - d_min, 1e-6);
  u = tex_from_unit(saturate(x_mu), gfx::k_sky_transmittance_width);
  v = tex_from_unit(saturate(rho / big_h), gfx::k_sky_transmittance_height);
}

inline void transmittance_ray(const Sky& s, double u, double v, double& r, double& h, double& mu) {
  const double x_mu = unit_from_tex(u, gfx::k_sky_transmittance_width);
  const double x_r = unit_from_tex(v, gfx::k_sky_transmittance_height);
  const double big_h = std::sqrt((s.top - s.bottom) * (s.top + s.bottom));
  const double rho = big_h * x_r;
  r = std::sqrt(rho * rho + s.bottom * s.bottom);
  h = rho * rho / (r + s.bottom);
  const double d_min = s.top - r;
  const double d_max = rho + big_h;
  const double d = d_min + x_mu * (d_max - d_min);
  mu = d <= 0.0 ? 1.0 : (big_h * big_h - rho * rho - d * d) / (2.0 * r * d);
  mu = clampd(mu, -1.0, 1.0);
}

inline Dvec3 transmittance_integral(const Sky& s, double r, double h, double mu, u32 steps = 256) {
  const double d = distance_to_top(s.bottom, s.top, r, h, mu);
  const double dt = d / steps;
  Dvec3 depth{};
  for (u32 i = 0; i < steps; ++i) {
    const double t = (i + 0.5) * dt;
    depth = depth + medium(s, height_along(s.bottom, r, h, mu, t)).extinction * dt;
  }
  return exp3(depth * -1.0);
}

inline Table build_transmittance(const Sky& s) {
  Table t;
  t.width = gfx::k_sky_transmittance_width;
  t.height = gfx::k_sky_transmittance_height;
  t.texels.resize(static_cast<usize>(t.width) * t.height);
  for (u32 y = 0; y < t.height; ++y) {
    for (u32 x = 0; x < t.width; ++x) {
      double r, h, mu;
      transmittance_ray(s, (x + 0.5) / t.width, (y + 0.5) / t.height, r, h, mu);
      const Dvec3 tr = transmittance_integral(s, r, h, mu);
      t.texels[static_cast<usize>(y) * t.width + x] = {tr.x, tr.y, tr.z, 1.0};
    }
  }
  return t;
}

inline Dvec3 transmittance(const Sky& s, const Tables& tables, double r, double h, double mu) {
  double u, v;
  transmittance_uv(s, r, h, mu, u, v);
  return rgb(bilinear(tables.transmittance, u, v, false));
}

inline Dvec3 transmittance_to_light(const Sky& s, const Tables& tables, double r, double h,
                                    double mu) {
  if (hits_ground(s.bottom, r, h, mu)) return {};
  return transmittance(s, tables, r, h, mu);
}

// ---- Psi_ms ------------------------------------------------------------------------------------

inline Dvec3 multiscatter(const Sky& s, const Tables& tables, double h, double mu_light) {
  const double n = gfx::k_sky_multiscatter_size;
  const double u = tex_from_unit(saturate(mu_light * 0.5 + 0.5), n);
  const double v = tex_from_unit(saturate(h / (s.top - s.bottom)), n);
  return rgb(bilinear(tables.multiscatter, u, v, false));
}

inline Table build_multiscatter(const Sky& s, const Tables& tables) {
  const u32 n = gfx::k_sky_multiscatter_size;
  Table t;
  t.width = t.height = n;
  t.texels.resize(static_cast<usize>(n) * n);
  const double uniform_phase = 1.0 / (4.0 * k_pi);
  for (u32 y = 0; y < n; ++y) {
    for (u32 x = 0; x < n; ++x) {
      const double mu_light = unit_from_tex((x + 0.5) / n, n) * 2.0 - 1.0;
      const double h = saturate(unit_from_tex((y + 0.5) / n, n)) * (s.top - s.bottom);
      const double r = s.bottom + h;
      const Dvec3 light{std::sqrt(std::max(1.0 - mu_light * mu_light, 0.0)), mu_light, 0.0};
      Dvec3 l_sum{}, f_sum{};
      for (u32 i = 0; i < 8; ++i) {
        for (u32 j = 0; j < 8; ++j) {
          const double theta = 2.0 * k_pi * (i + 0.5) / 8.0;
          const double phi = std::acos(1.0 - 2.0 * (j + 0.5) / 8.0);
          const Dvec3 dir{std::cos(theta) * std::sin(phi), std::cos(phi),
                          std::sin(theta) * std::sin(phi)};
          const double mu = dir.y;
          const double nu = dot(dir, light);
          const bool ground = hits_ground(s.bottom, r, h, mu);
          const double t_max = ground ? distance_to_ground(s.bottom, r, h, mu)
                                      : distance_to_top(s.bottom, s.top, r, h, mu);
          const double dt = t_max / 20.0;
          Dvec3 radiance{}, transfer{};
          Dvec3 throughput = splat(1.0);
          for (u32 k = 0; k < 20; ++k) {
            const double ts = (k + 0.5) * dt;
            const double hs = height_along(s.bottom, r, h, mu, ts);
            const double rs = s.bottom + hs;
            const Medium m = medium(s, hs);
            const Dvec3 scattering = m.rayleigh + m.mie;
            const double mu_s = (r * mu_light + ts * nu) / rs;
            const Dvec3 in_s =
                transmittance_to_light(s, tables, rs, hs, mu_s) * scattering * uniform_phase;
            const Dvec3 taken = div3(absorbed(m.extinction * dt), max3(m.extinction, 1e-9));
            radiance = radiance + throughput * in_s * taken;
            transfer = transfer + throughput * scattering * taken;
            throughput = throughput * exp3(m.extinction * -dt);
          }
          if (ground) {
            const double mu_g = (r * mu_light + t_max * nu) / s.bottom;
            radiance = radiance + throughput *
                                      transmittance_to_light(s, tables, s.bottom, 0.0, mu_g) *
                                      s.ground_albedo * (std::max(mu_g, 0.0) / k_pi);
          }
          l_sum = l_sum + radiance;
          f_sum = f_sum + transfer;
        }
      }
      const Dvec3 l2 = l_sum * (1.0 / 64.0);
      const Dvec3 fms = f_sum * (1.0 / 64.0);
      const Dvec3 psi = div3(l2, splat(1.0) - fms);
      t.texels[static_cast<usize>(y) * n + x] = {psi.x, psi.y, psi.z, 1.0};
    }
  }
  return t;
}

// ---- the sky-view parameterization --------------------------------------------------------------

inline void horizon(const Sky& s, double h, double& zenith_horizon, double& beta) {
  const double r = s.bottom + h;
  const double v = std::sqrt(std::max(h * (2.0 * s.bottom + h), 0.0));
  beta = std::acos(clampd(v / r, -1.0, 1.0));
  zenith_horizon = k_pi - beta;
}

inline double azimuth(Dvec3 d) {
  const double a = std::atan2(d.z, d.x);
  return a < 0.0 ? a + 2.0 * k_pi : a;
}

inline void view_uv(const Sky& s, double h, Dvec3 d, double& u, double& v) {
  double zh, beta;
  horizon(s, h, zh, beta);
  const double zenith = std::acos(clampd(d.y, -1.0, 1.0));
  if (zenith < zh) {
    const double c = std::sqrt(std::max(1.0 - zenith / zh, 0.0));
    v = (1.0 - c) * 0.5;
  } else {
    const double c = std::sqrt(std::max((zenith - zh) / beta, 0.0));
    v = c * 0.5 + 0.5;
  }
  u = azimuth(d) / (2.0 * k_pi);
}

inline Dvec3 view_direction(const Sky& s, double h, double u, double v) {
  double zh, beta;
  horizon(s, h, zh, beta);
  double zenith;
  if (v < 0.5) {
    const double c = 1.0 - 2.0 * v;
    zenith = zh * (1.0 - c * c);
  } else {
    const double c = v * 2.0 - 1.0;
    zenith = zh + beta * c * c;
  }
  const double az = u * 2.0 * k_pi;
  const double sn = std::sin(zenith);
  return {sn * std::cos(az), std::cos(zenith), sn * std::sin(az)};
}

inline Dvec3 light_scattering(const Sky& s, const Tables& tables, const Medium& m, double r,
                              double h, double mu_light, double nu, Dvec3 illuminance) {
  Dvec3 single{};
  if (!hits_ground(s.bottom, r, h, mu_light)) {
    single = transmittance(s, tables, r, h, mu_light) *
             (m.rayleigh * phase_rayleigh(nu) + m.mie * phase_mie(nu, s.mie_g));
  }
  const Dvec3 multiple = multiscatter(s, tables, h, mu_light) * (m.rayleigh + m.mie);
  return illuminance * (single + multiple);
}

inline void march(const Sky& s, const Tables& tables, double r0, double h0, Dvec3 dir, double t_max,
                  u32 samples, Dvec3& radiance, Dvec3& throughput) {
  const double mu = dir.y;
  const bool moon_on = (s.flags & gfx::k_sky_moon) != 0u;
  const double nu_sun = dot(dir, s.sun);
  const double nu_moon = dot(dir, s.moon);
  double t_prev = 0.0;
  for (u32 i = 0; i < samples; ++i) {
    const double f = static_cast<double>(i + 1) / samples;
    const double t_next = t_max * f * f;
    const double dt = t_next - t_prev;
    const double t = t_prev + 0.5 * dt;
    t_prev = t_next;
    const double h = height_along(s.bottom, r0, h0, mu, t);
    const double r = s.bottom + h;
    const Medium m = medium(s, h);
    Dvec3 in_s = light_scattering(s, tables, m, r, h, (r0 * s.sun.y + t * nu_sun) / r, nu_sun,
                                  s.sun_illuminance);
    if (moon_on) {
      in_s = in_s + light_scattering(s, tables, m, r, h, (r0 * s.moon.y + t * nu_moon) / r, nu_moon,
                                     s.moon_illuminance);
    }
    radiance =
        radiance + throughput * in_s * div3(absorbed(m.extinction * dt), max3(m.extinction, 1e-9));
    throughput = throughput * exp3(m.extinction * -dt);
  }
}

inline Table build_sky_view(const Sky& s, const Tables& tables) {
  Table t;
  t.width = gfx::k_sky_view_width;
  t.height = gfx::k_sky_view_height;
  t.texels.resize(static_cast<usize>(t.width) * t.height);
  const double h = s.altitude;
  const double r = s.bottom + h;
  for (u32 y = 0; y < t.height; ++y) {
    for (u32 x = 0; x < t.width; ++x) {
      const Dvec3 dir = view_direction(s, h, (x + 0.5) / t.width, (y + 0.5) / t.height);
      const bool ground = hits_ground(s.bottom, r, h, dir.y);
      const double t_max = ground ? distance_to_ground(s.bottom, r, h, dir.y)
                                  : distance_to_top(s.bottom, s.top, r, h, dir.y);
      Dvec3 radiance{};
      Dvec3 throughput = splat(1.0);
      march(s, tables, r, h, dir, t_max, 32, radiance, throughput);
      if (ground) {
        const double mu_sun = (r * s.sun.y + t_max * dot(dir, s.sun)) / s.bottom;
        Dvec3 lit = s.sun_illuminance * transmittance_to_light(s, tables, s.bottom, 0.0, mu_sun) *
                    std::max(mu_sun, 0.0);
        if ((s.flags & gfx::k_sky_moon) != 0u) {
          const double mu_moon = (r * s.moon.y + t_max * dot(dir, s.moon)) / s.bottom;
          lit = lit + s.moon_illuminance *
                          transmittance_to_light(s, tables, s.bottom, 0.0, mu_moon) *
                          std::max(mu_moon, 0.0);
        }
        radiance = radiance + throughput * lit * s.ground_albedo * (1.0 / k_pi);
      }
      t.texels[static_cast<usize>(y) * t.width + x] = {radiance.x, radiance.y, radiance.z, 1.0};
    }
  }
  return t;
}

inline Dvec3 view_radiance(const Sky& s, const Tables& tables, Dvec3 dir) {
  double u, v;
  view_uv(s, s.altitude, dir, u, v);
  return rgb(bilinear(tables.sky_view, u, v, true));
}

// ---- aerial perspective ------------------------------------------------------------------------

inline Table build_aerial(const Sky& s, const Tables& tables) {
  const u32 w = gfx::k_sky_aerial_width;
  const u32 hgt = gfx::k_sky_aerial_height;
  const u32 depth = gfx::k_sky_aerial_depth;
  Table out;
  out.width = w;
  out.height = hgt * depth;
  out.texels.resize(static_cast<usize>(w) * hgt * depth);
  const double h0 = s.altitude;
  const double r0 = s.bottom + h0;
  const bool moon_on = (s.flags & gfx::k_sky_moon) != 0u;
  for (u32 y = 0; y < hgt; ++y) {
    for (u32 x = 0; x < w; ++x) {
      const Dvec3 dir = view_direction(s, h0, (x + 0.5) / w, (y + 0.5) / hgt);
      const double mu = dir.y;
      const double nu_sun = dot(dir, s.sun);
      const double nu_moon = dot(dir, s.moon);
      Dvec3 radiance{};
      Dvec3 throughput = splat(1.0);
      double t_prev = 0.0;
      for (u32 k = 1; k <= depth; ++k) {
        const double f = static_cast<double>(k) / depth;
        const double t_end = static_cast<double>(gfx::k_sky_aerial_distance_km) * f * f;
        const double dt = (t_end - t_prev) / 2.0;
        for (u32 sub = 0; sub < 2; ++sub) {
          const double t = t_prev + (sub + 0.5) * dt;
          const double hh = height_along(s.bottom, r0, h0, mu, t);
          const double hc = std::max(hh, 0.0);
          const double r = s.bottom + hc;
          const Medium m = medium(s, hc);
          Dvec3 in_s = light_scattering(s, tables, m, r, hc, (r0 * s.sun.y + t * nu_sun) / r,
                                        nu_sun, s.sun_illuminance);
          if (moon_on) {
            in_s = in_s + light_scattering(s, tables, m, r, hc, (r0 * s.moon.y + t * nu_moon) / r,
                                           nu_moon, s.moon_illuminance);
          }
          radiance = radiance + throughput * in_s *
                                    div3(absorbed(m.extinction * dt), max3(m.extinction, 1e-9));
          throughput = throughput * exp3(m.extinction * -dt);
        }
        t_prev = t_end;
        out.texels[(static_cast<usize>(k - 1) * hgt + y) * w + x] = {
            radiance.x, radiance.y, radiance.z, (throughput.x + throughput.y + throughput.z) / 3.0};
      }
    }
  }
  return out;
}

inline void aerial(const Sky& s, const Tables& tables, Dvec3 dir, double distance_m,
                   Dvec3& inscatter, double& transmittance_out) {
  const u32 w = gfx::k_sky_aerial_width;
  const u32 hgt = gfx::k_sky_aerial_height;
  const u32 depth = gfx::k_sky_aerial_depth;
  double u, v;
  view_uv(s, s.altitude, dir, u, v);
  const double slice = std::min(std::sqrt(std::max(distance_m * 0.001, 0.0) /
                                          static_cast<double>(gfx::k_sky_aerial_distance_km)) *
                                    depth,
                                static_cast<double>(depth));
  const double k = std::floor(slice);
  const double wgt = slice - k;
  auto plane = [&](u32 index) {
    Table p;
    p.width = w;
    p.height = hgt;
    p.texels.assign(
        tables.aerial.texels.begin() + static_cast<std::ptrdiff_t>(index) * w * hgt,
        tables.aerial.texels.begin() + static_cast<std::ptrdiff_t>(index + 1) * w * hgt);
    return bilinear(p, u, v, true);
  };
  Dvec4 lo{0.0, 0.0, 0.0, 1.0};
  if (k >= 1.0) lo = plane(static_cast<u32>(k) - 1);
  Dvec4 hi = lo;
  if (k < depth) hi = plane(static_cast<u32>(k));
  const Dvec4 value = lerp4(lo, hi, wgt);
  inscatter = rgb(value);
  transmittance_out = value.w;
}

inline double height_of(Dvec3 world) {
  return std::max(world.y * 0.001, static_cast<double>(gfx::k_sky_min_altitude_km));
}

inline Dvec3 light_at(const Sky& s, const Tables& tables, Dvec3 world, Dvec3 dir,
                      Dvec3 illuminance) {
  const double h = height_of(world);
  return illuminance * transmittance_to_light(s, tables, s.bottom + h, h, dir.y);
}

// ---- the frame's sums --------------------------------------------------------------------------

inline void sh_basis(Dvec3 n, double basis[9]) {
  basis[0] = 0.282095;
  basis[1] = 0.488603 * n.y;
  basis[2] = 0.488603 * n.z;
  basis[3] = 0.488603 * n.x;
  basis[4] = 1.092548 * n.x * n.y;
  basis[5] = 1.092548 * n.y * n.z;
  basis[6] = 0.315392 * (3.0 * n.y * n.y - 1.0);
  basis[7] = 1.092548 * n.x * n.z;
  basis[8] = 0.546274 * (n.x * n.x - n.z * n.z);
}

struct Frame {
  Dvec3 sh[9];
  Dvec3 ground;
  Dvec3 sun_ground, moon_ground;
  double exposure = 0.0, ev = 0.0, lux = 0.0, sky_lux = 0.0;
};

inline Frame frame(const Sky& s, const Tables& tables) {
  Frame f;
  const u32 rows = gfx::k_sky_sh_rows;
  const u32 cols = 2 * rows;
  const double d_theta = 0.5 * k_pi / rows;
  const double d_phi = 2.0 * k_pi / cols;
  Dvec3 level{};
  for (u32 row = 0; row < rows; ++row) {
    for (u32 col = 0; col < cols; ++col) {
      const double theta = (row + 0.5) * d_theta;
      const double phi = (col + 0.5) * d_phi;
      const double sn = std::sin(theta);
      const Dvec3 dir{sn * std::cos(phi), std::cos(theta), sn * std::sin(phi)};
      const Dvec3 radiance = view_radiance(s, tables, dir) + s.night;
      const double d_omega = sn * d_theta * d_phi;
      double basis[9];
      sh_basis(dir, basis);
      for (u32 c = 0; c < 9; ++c)
        f.sh[c] = f.sh[c] + radiance * (basis[c] * d_omega);
      level = level + radiance * (dir.y * d_omega);
    }
  }
  // The meter takes the sky's mean over its dome (sky_luts.slang says why): pi times it is half the
  // integral, which the first harmonic holds before the ground is added.
  const Dvec3 dome = f.sh[0] * (1.0 / (2.0 * 0.282095));
  const double h = s.altitude;
  const double r = s.bottom + h;
  f.sun_ground = s.sun_illuminance * transmittance_to_light(s, tables, r, h, s.sun.y);
  if ((s.flags & gfx::k_sky_moon) != 0u)
    f.moon_ground = s.moon_illuminance * transmittance_to_light(s, tables, r, h, s.moon.y);
  const Dvec3 lit = f.sun_ground * std::max(s.sun.y, 0.0) + f.moon_ground * std::max(s.moon.y, 0.0);
  f.ground = s.local_albedo * (lit + level) * (1.0 / k_pi);
  f.sh[0] = f.sh[0] + f.ground * (0.282095 * 2.0 * k_pi);
  f.sh[1] = f.sh[1] + f.ground * (-0.488603 * k_pi);
  for (u32 c = 0; c < 9; ++c)
    f.sh[c] = f.sh[c] * (c == 0 ? 1.0 : (c < 4 ? 2.0 / 3.0 : 0.25));
  f.lux = 128000.0 * luminance(lit + dome);
  f.sky_lux = 128000.0 * luminance(dome);
  f.ev = s.exposure_mode >= 0.5
             ? s.exposure_ev
             : gfx::sky_exposure_ev100(f.lux, s.exposure_ev, s.exposure_knee, s.exposure_slope);
  f.exposure = gfx::sky_exposure_scale(f.ev);
  return f;
}

inline Dvec3 ambient(const Frame& f, Dvec3 n) {
  double basis[9];
  sh_basis(n, basis);
  Dvec3 sum{};
  for (u32 c = 0; c < 9; ++c)
    sum = sum + f.sh[c] * basis[c];
  return max3(sum, 0.0);
}

// ---- the discs, the stars, the background
// --------------------------------------------------------

inline bool disc(Dvec3 dir, Dvec3 centre, double radius, double& sfrac, Dvec3& across) {
  sfrac = 0.0;
  across = {};
  const double c = dot(dir, centre);
  if (c <= std::cos(radius)) return false;
  const Dvec3 q = dir - centre * c;
  const double len = std::sqrt(dot(q, q));
  sfrac = std::min(len / std::sin(radius), 1.0);
  across = len > 0.0 ? q * (1.0 / len) : Dvec3{};
  return true;
}

inline Dvec3 sun_disc(const Sky& s, Dvec3 dir) {
  double sf;
  Dvec3 across;
  if (!disc(dir, s.sun, s.sun_radius, sf, across)) return {};
  const Dvec3 limb{0.48, 0.60, 0.72};
  const double mu = std::sqrt(std::max(1.0 - sf * sf, 0.0));
  const double solid = 2.0 * k_pi * (1.0 - std::cos(s.sun_radius));
  const Dvec3 darkened = splat(1.0) - limb * (1.0 - mu);
  const Dvec3 mean = splat(1.0) - limb * (1.0 / 3.0);
  return div3(s.sun_illuminance * darkened, mean * solid);
}

inline Dvec3 moon_disc(const Sky& s, Dvec3 dir) {
  double sf;
  Dvec3 across;
  if (!disc(dir, s.moon, s.moon_radius, sf, across)) return {};
  const Dvec3 n = across * sf - s.moon * std::sqrt(std::max(1.0 - sf * sf, 0.0));
  const double lit = std::max(dot(n, s.sun), 0.0);
  return s.sun_illuminance * (s.moon_albedo * lit / k_pi);
}

// The star table as the shader reads it: celestial directions and rgb irradiance, and the cells.
struct Stars {
  std::vector<Dvec3> direction;
  std::vector<Dvec3> irradiance;
  std::vector<u32> cells;  // k_sky_star_cells + 1 offsets
  std::vector<u32> index;
};

inline u32 star_cell(Dvec3 d) {
  const double ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
  u32 face;
  double u, v;
  if (ax >= ay && ax >= az) {
    face = d.x > 0.0 ? 0u : 1u;
    u = d.y / ax;
    v = d.z / ax;
  } else if (ay >= az) {
    face = d.y > 0.0 ? 2u : 3u;
    u = d.x / ay;
    v = d.z / ay;
  } else {
    face = d.z > 0.0 ? 4u : 5u;
    u = d.x / az;
    v = d.y / az;
  }
  const double n = gfx::k_sky_star_face_cells;
  const u32 cx = std::min(static_cast<u32>(std::max((u * 0.5 + 0.5) * n, 0.0)),
                          gfx::k_sky_star_face_cells - 1);
  const u32 cy = std::min(static_cast<u32>(std::max((v * 0.5 + 0.5) * n, 0.0)),
                          gfx::k_sky_star_face_cells - 1);
  return (face * gfx::k_sky_star_face_cells + cy) * gfx::k_sky_star_face_cells + cx;
}

inline Dvec3 stars(const Sky& s, const Stars& table, Dvec3 dir, double sigma) {
  const Dvec3 d{dot(dir, s.celestial_x), dot(dir, s.celestial_y), dot(dir, s.celestial_z)};
  const u32 cell = star_cell(d);
  const double sg = clampd(sigma, 1.0e-6, static_cast<double>(gfx::k_sky_star_max_sigma));
  const double inv = 1.0 / (2.0 * sg * sg);
  Dvec3 sum{};
  for (u32 i = table.cells[cell]; i < table.cells[cell + 1]; ++i) {
    const u32 star = table.index[i];
    if (dot(d, table.direction[star]) <= 0.0) continue;
    const Dvec3 c = cross(d, table.direction[star]);
    sum = sum + table.irradiance[star] * (std::exp(-dot(c, c) * inv) * inv / k_pi);
  }
  return sum * s.star_scale;
}

inline Dvec3 night_glow(const Sky& s, Dvec3 dir) { return dir.y >= 0.0 ? s.night : Dvec3{}; }

inline Dvec3 background(const Sky& s, const Tables& tables, const Stars* table, Dvec3 dir,
                        double pixel) {
  Dvec3 radiance = view_radiance(s, tables, dir) + night_glow(s, dir);
  const double h = s.altitude;
  const double r = s.bottom + h;
  if (hits_ground(s.bottom, r, h, dir.y)) return radiance;
  Dvec3 light{};
  if ((s.flags & gfx::k_sky_sun_disc) != 0u) light = light + sun_disc(s, dir);
  if ((s.flags & gfx::k_sky_moon) != 0u) light = light + moon_disc(s, dir);
  if ((s.flags & gfx::k_sky_stars) != 0u && table != nullptr)
    light = light + stars(s, *table, dir, pixel * 0.7);
  if (light.x > 0.0 || light.y > 0.0 || light.z > 0.0)
    radiance = radiance + light * transmittance(s, tables, r, h, dir.y);
  return radiance;
}

// view_ray.slang's `view_ray_direction`, which sky.slang's `sky_pixel_direction` is: a pixel's
// clip-space (x, y) at depth 0 — the point at infinity — through the view's clip-to-ray matrix,
// whose xyz is the direction itself. The matrix's own floats, widened; no eye anywhere in it.
inline Dvec3 pixel_direction(const Mat4& clip_to_ray, double ndc_x, double ndc_y) {
  Dvec3 d;
  double* out[3] = {&d.x, &d.y, &d.z};
  for (usize row = 0; row < 3; ++row) {
    *out[row] = static_cast<double>(clip_to_ray.at(row, 0)) * ndc_x +
                static_cast<double>(clip_to_ray.at(row, 1)) * ndc_y +
                static_cast<double>(clip_to_ray.at(row, 3));
  }
  return normalize(d);
}

inline Tables build_tables(const Sky& s) {
  Tables tables;
  tables.transmittance = build_transmittance(s);
  tables.multiscatter = build_multiscatter(s, tables);
  tables.sky_view = build_sky_view(s, tables);
  tables.aerial = build_aerial(s, tables);
  return tables;
}

}  // namespace engine::sky_ref
