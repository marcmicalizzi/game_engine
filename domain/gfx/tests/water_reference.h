#pragma once

// The resolve's water branch (domain/gfx/shaders/water.slang) in double precision on the CPU: the
// same waves, the same Fresnel, the same bottom and the same glitter, written out again so the
// shading test (water_tests.cpp) holds the GPU to a second implementation rather than to numbers
// someone wrote down. Change one and the other in the same commit (docs/subsystems/water.md).
//
// Two things are mirrored exactly rather than in double: the phase hash, which is integer
// arithmetic, and the frequencies' rounding to whole cycles per period, which is an integer once
// rounded. Everything else is double, the GPU's float within Vulkan's precision of it.

#include "brdf_reference.h"

#include <core/base/types.h>
#include <domain/gfx/visibility_resolve.h>
#include <domain/gfx/water_spectrum.h>
#include <domain/gfx/water_swash.h>

#include <algorithm>
#include <cmath>

namespace engine::water_ref {

using brdf_ref::Dvec3;

inline constexpr double k_ior = 1.33;
inline constexpr double k_f0 = ((1.33 - 1.0) / (1.33 + 1.0)) * ((1.33 - 1.0) / (1.33 + 1.0));
inline constexpr double k_g = 9.81;
inline constexpr double k_period_s = 1024.0;
inline constexpr double k_two_pi = 6.283185307179586476925286766559;
inline constexpr double k_calm_depth = 0.25;
inline constexpr double k_length[4] = {1.0, 0.62, 0.38, 0.24};
inline constexpr double k_amplitude[4] = {1.0, 0.55, 0.32, 0.18};
inline constexpr double k_turn[4] = {0.0, 0.55, -0.8, 0.35};

struct Params {
  Dvec3 shallow;
  Dvec3 deep;
  double roughness = 0.08;
  double depth_fade = 3.0;
  double amplitude = 0.0;
  double wavelength = 1.0;
  double chop = 0.0;
  double shore = 0.0;
  double dir_x = 1.0;
  double dir_z = 0.0;
  u32 seed = 1;
};

inline Params params_of(const gfx::ResolveMaterial& m) {
  Params p;
  p.shallow = brdf_ref::dvec3(m.albedo);
  p.roughness = static_cast<double>(m.albedo.w);
  p.deep = brdf_ref::dvec3(m.uv_transform);
  p.depth_fade = static_cast<double>(m.uv_transform.w);
  p.amplitude = static_cast<double>(m.uv_offset.x);
  p.wavelength = static_cast<double>(m.uv_offset.y);
  p.chop = static_cast<double>(m.uv_offset.z);
  p.shore = static_cast<double>(m.uv_offset.w);
  p.dir_x = static_cast<double>(m.normal_scale);
  p.dir_z = static_cast<double>(m.occlusion_strength);
  p.seed = m.sampler;
  return p;
}

inline double phase(u32 seed, u32 wave) {
  u32 h = seed * 0x9E3779B9u + (wave + 1u) * 0x85EBCA6Bu;
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  h ^= h >> 16;
  return static_cast<double>(h >> 8) * (1.0 / 16777216.0);
}

inline double frac(double v) { return v - std::floor(v); }

// The waves' normal at body-local (x, z) at clock `time`, with `footprint` metres a pixel.
inline Dvec3 normal(const Params& w, double x, double z, double time, double footprint,
                    double depth) {
  double sx = 0.0;
  double sz = 0.0;
  const double calm = brdf_ref::clamp01(depth / k_calm_depth);
  for (u32 i = 0; i < 4; ++i) {
    const double lambda = w.wavelength * k_length[i];
    const double a = w.amplitude * k_amplitude[i] * calm;
    const double turn = w.chop * k_turn[i] * (0.5 * 3.14159265358979);
    const double c = std::cos(turn);
    const double s = std::sin(turn);
    const double dx = w.dir_x * c - w.dir_z * s;
    const double dz = w.dir_x * s + w.dir_z * c;
    const double k = k_two_pi / lambda;
    const double cycles = std::floor(std::sqrt(k_g * k) * (k_period_s / k_two_pi) + 0.5);
    const double along = (dx * x + dz * z) / lambda;
    const double travel = frac(cycles * (time / k_period_s));
    const double cyc = frac(frac(along) - travel + phase(w.seed, i));
    const double keep = footprint > 0.0 ? brdf_ref::clamp01(lambda / (2.0 * footprint) - 1.0) : 1.0;
    const double g = a * k * keep * std::cos(k_two_pi * cyc);
    sx += dx * g;
    sz += dz * g;
  }
  return brdf_ref::normalize(Dvec3{-sx, 1.0, -sz});
}

// water.slang's `water_normal_spectrum` (gfx::k_material_water_spectrum, water_spectrum.h): the
// table's Gerstner waves summed in double, each wave's numbers the float the GPU reads.
inline Dvec3 normal_spectrum(const gfx::WaterWave* waves, double x, double z, double time,
                             double footprint, double depth) {
  double sx = 0.0;
  double sz = 0.0;
  double lift = 0.0;
  const double calm = brdf_ref::clamp01(depth / k_calm_depth);
  for (u32 i = 0; i < gfx::k_water_waves; ++i) {
    const gfx::WaterWave& wave = waves[i];
    const double k = static_cast<double>(wave.k);
    const double dx = static_cast<double>(wave.direction.x);
    const double dz = static_cast<double>(wave.direction.y);
    const double lambda = k_two_pi / k;
    const double along = (dx * x + dz * z) / lambda;
    const double travel = frac(static_cast<double>(wave.cycles) * (time / k_period_s));
    const double cyc = frac(frac(along) - travel + static_cast<double>(wave.phase));
    const double keep = footprint > 0.0 ? brdf_ref::clamp01(lambda / (2.0 * footprint) - 1.0) : 1.0;
    const double share = calm * keep;
    const double angle = k_two_pi * cyc;
    const double g = static_cast<double>(wave.amplitude) * k * share * std::cos(angle);
    sx += dx * g;
    sz += dz * g;
    lift += static_cast<double>(wave.steepness) * share * std::sin(angle);
  }
  return brdf_ref::normalize(Dvec3{-sx, 1.0 - lift, -sz});
}

// water.slang's `water_edge`: what the shore fades by — the distance from the outline for a closed
// body, and for an open one (gfx::k_material_water_open) the shore times the share of the calm
// depth the water has.
inline double edge(const Params& w, u32 flags, double from_edge, double depth) {
  if ((flags & gfx::k_material_water_open) == 0) return from_edge;
  return w.shore * brdf_ref::clamp01(depth / k_calm_depth);
}

inline double schlick(double cos_theta) {
  const double m = brdf_ref::clamp01(1.0 - cos_theta);
  return k_f0 + (1.0 - k_f0) * (m * m * m * m * m);
}

// water.slang's `water_radiance`.
inline Dvec3 radiance(const Params& w, Dvec3 n, Dvec3 v, double from_edge, double depth,
                      Dvec3 sun_dir, Dvec3 sun, Dvec3 ambient, Dvec3 sky) {
  const double shore = w.shore > 0.0 ? brdf_ref::clamp01(from_edge / w.shore) : 1.0;
  const double n_dot_v = std::max(brdf_ref::dot(n, v), brdf_ref::k_min_n_dot_v);
  const double fresnel = schlick(n_dot_v);
  const Dvec3 body = brdf_ref::lerp(w.shallow, w.deep, brdf_ref::clamp01(depth / w.depth_fade));
  const Dvec3 lit = sun * (std::max(sun_dir.y, 0.0) / brdf_ref::k_pi) + ambient;
  const double sin2_t = (1.0 - n_dot_v * n_dot_v) / (k_ior * k_ior);
  const double cos_t = std::sqrt(std::max(1.0 - sin2_t, 1e-4));
  const double slant = depth * (1.0 / cos_t - 1.0);
  const double absorb = std::exp(-slant * (3.0 / w.depth_fade));
  const Dvec3 under = body * lit * absorb;
  Dvec3 glitter{};
  const double n_dot_l = brdf_ref::dot(n, sun_dir);
  if (n_dot_l > 0.0) {
    const Dvec3 half_sum = sun_dir + v;
    const Dvec3 h = brdf_ref::dot(half_sum, half_sum) > 1e-12 ? brdf_ref::normalize(half_sum) : n;
    const double alpha = brdf_ref::alpha_of(w.roughness);
    const double n_dot_h = brdf_ref::clamp01(brdf_ref::dot(n, h));
    const double v_dot_h = brdf_ref::clamp01(brdf_ref::dot(v, h));
    glitter = sun * (schlick(v_dot_h) * brdf_ref::d_ggx(n_dot_h, alpha) *
                     brdf_ref::v_smith(n_dot_v, n_dot_l, alpha) * n_dot_l);
  }
  return under * (1.0 - fresnel * shore) + (sky * fresnel + glitter) * shore;
}

// ---- the volume tier (water.slang, "the volume tier") -------------------------------------------

inline constexpr double k_diffuse_path = 1.25;

struct Optics {
  Dvec3 extinction;
  Dvec3 scattering;
  double transmission = 1.0;
};

// water.slang's `water_optics_of`, of the entry after the water material.
inline Optics optics_of(const gfx::ResolveMaterial& m) {
  Optics o;
  o.extinction = brdf_ref::dvec3(m.albedo);
  o.transmission = static_cast<double>(m.albedo.w);
  o.scattering = brdf_ref::dvec3(m.emissive);
  return o;
}

struct Path {
  Dvec3 t;
  double length = 0.0;
  double depth = 0.0;
};

// water.slang's `water_path`: the view refracted at `n`, run down to the bottom's plane, whose
// depth here is `depth` and whose slope over body-local (x, z) is (`slope_x`, `slope_z`).
inline Path path(Dvec3 n, Dvec3 v, double depth, double slope_x, double slope_z) {
  const double cos_i = std::max(brdf_ref::dot(n, v), brdf_ref::k_min_n_dot_v);
  const double eta = 1.0 / k_ior;
  const double cos_t = std::sqrt(std::max(1.0 - eta * eta * (1.0 - cos_i * cos_i), 0.0));
  Path p;
  p.t = brdf_ref::normalize((-v) * eta + n * (eta * cos_i - cos_t));
  const double down = std::max(-p.t.y, 1e-3);
  const double den = std::max(down - (slope_x * p.t.x + slope_z * p.t.z), 0.25 * down);
  p.length = std::max(depth, 0.0) / den;
  p.depth = p.length * down;
  return p;
}

// water.slang's depth map (`water_depth_map_of`, `water_depth_sample`): the entry's UV frame and
// the 16-bit texels the image's red and green hold, sampled bilinearly in double as the sampler
// does — texel coordinates u N - 1/2, the four neighbours, clamp-to-edge — after the UV is clamped
// half a texel inside the map.
struct DepthMap {
  const u16* texels = nullptr;
  u32 size = 0;
  double u0 = 0.0;
  double v0 = 0.0;
  double uv_per_m = 0.0;
  double half_texel = 0.0;
  double depth_max = 0.0;
};

inline DepthMap depth_map_of(const gfx::ResolveMaterial& entry, const u16* texels, u32 size) {
  DepthMap d;
  d.texels = texels;
  d.size = size;
  d.u0 = static_cast<double>(entry.uv_offset.x);
  d.v0 = static_cast<double>(entry.uv_offset.y);
  d.uv_per_m = static_cast<double>(entry.uv_offset.z);
  d.depth_max = static_cast<double>(entry.uv_offset.w);
  d.half_texel = static_cast<double>(entry.uv_transform.w);
  return d;
}

inline double depth_sample(const DepthMap& d, double x, double z) {
  const double n = static_cast<double>(d.size);
  const double u = std::clamp(d.u0 + x * d.uv_per_m, d.half_texel, 1.0 - d.half_texel);
  const double v = std::clamp(d.v0 + z * d.uv_per_m, d.half_texel, 1.0 - d.half_texel);
  const double tx = u * n - 0.5;
  const double tz = v * n - 0.5;
  const double fx0 = std::floor(tx);
  const double fz0 = std::floor(tz);
  const double ax = tx - fx0;
  const double az = tz - fz0;
  const auto at = [&](double i, double j) {
    const u32 ii = static_cast<u32>(std::clamp(i, 0.0, n - 1.0));
    const u32 jj = static_cast<u32>(std::clamp(j, 0.0, n - 1.0));
    return static_cast<double>(d.texels[jj * d.size + ii]) / 65535.0 * d.depth_max;
  };
  const double top = at(fx0, fz0) * (1.0 - ax) + at(fx0 + 1.0, fz0) * ax;
  const double bottom = at(fx0, fz0 + 1.0) * (1.0 - ax) + at(fx0 + 1.0, fz0 + 1.0) * ax;
  return top * (1.0 - az) + bottom * az;
}

// water.slang's `water_path_map`: the refracted view marched against the map in
// `k_march_steps` fixed steps out to `k_march_reach` times the flat answer, one secant in the
// step that passes under the ground, and the bottom's normal from one texel's forward differences.
inline constexpr u32 k_march_steps = 6;
inline constexpr double k_march_reach = 3.0;

inline Path path_map(const DepthMap& d, double x, double z, Dvec3 n, Dvec3 v, double depth,
                     Dvec3& n_b) {
  const double cos_i = std::max(brdf_ref::dot(n, v), brdf_ref::k_min_n_dot_v);
  const double eta = 1.0 / k_ior;
  const double cos_t = std::sqrt(std::max(1.0 - eta * eta * (1.0 - cos_i * cos_i), 0.0));
  Path p;
  p.t = brdf_ref::normalize((-v) * eta + n * (eta * cos_i - cos_t));
  const double down = std::max(-p.t.y, 1e-3);
  const double step = k_march_reach * std::max(depth, 0.0) / down / k_march_steps;
  double s_a = 0.0;
  double f_a = std::max(depth, 0.0);
  double s = step * k_march_steps;
  for (u32 i = 1; i <= k_march_steps; ++i) {
    const double s_b = step * i;
    const double f_b = depth_sample(d, x + p.t.x * s_b, z + p.t.z * s_b) - s_b * down;
    if (f_b <= 0.0) {
      s = f_a - f_b > 1e-9 ? s_a + (s_b - s_a) * (f_a / (f_a - f_b)) : s_b;
      break;
    }
    s_a = s_b;
    f_a = f_b;
  }
  p.length = s;
  p.depth = s * down;
  const double hx = x + p.t.x * s;
  const double hz = z + p.t.z * s;
  const double texel_m = 2.0 * d.half_texel / d.uv_per_m;
  const double d0 = depth_sample(d, hx, hz);
  const double dx = depth_sample(d, hx + texel_m, hz) - d0;
  const double dz = depth_sample(d, hx, hz + texel_m) - d0;
  n_b = brdf_ref::normalize(Dvec3{dx / texel_m, 1.0, dz / texel_m});
  return p;
}

inline double gather(double a, double s) { return a > 1e-5 ? (1.0 - std::exp(-a * s)) / a : s; }
inline Dvec3 gather3(Dvec3 a, double s) { return {gather(a.x, s), gather(a.y, s), gather(a.z, s)}; }
inline Dvec3 exp3(Dvec3 a) { return {std::exp(a.x), std::exp(a.y), std::exp(a.z)}; }

// water.slang's `water_sun_under`.
inline Dvec3 sun_under(Dvec3 sun_dir, double& cos_l) {
  const double up = std::max(sun_dir.y, 0.0);
  const double eta = 1.0 / k_ior;
  cos_l = std::sqrt(std::max(1.0 - eta * eta * (1.0 - up * up), 1e-4));
  return {sun_dir.x * eta, cos_l, sun_dir.z * eta};
}

// water_refract.slang's `water_submerged_sun` (S86): the sun's beam at `depth` under a level
// surface — Schlick's share of `sun` let in at the sun's angle, dimmed by Beer–Lambert over the
// refracted path up to the surface, depth / cos l — and `cos_l`, that path's cosine with the
// vertical.
inline Dvec3 submerged_sun(Dvec3 extinction, Dvec3 sun_dir, Dvec3 sun, double depth,
                           double& cos_l) {
  (void)sun_under(sun_dir, cos_l);
  const Dvec3 sun_in = sun * (1.0 - schlick(std::max(sun_dir.y, 0.0)));
  return sun_in * exp3(extinction * -(std::max(depth, 0.0) / cos_l));
}

// water.slang's `water_volume`.
inline Dvec3 volume(const Optics& o, const Path& p, Dvec3 n_b, Dvec3 albedo, Dvec3 sun_dir,
                    Dvec3 sun, double sun_surface, double sun_bottom, Dvec3 ambient) {
  double cos_l = 1.0;
  const Dvec3 l = sun_under(sun_dir, cos_l);
  const Dvec3 sun_in = sun * (1.0 - schlick(std::max(sun_dir.y, 0.0)));
  const Dvec3 sky_in = ambient * (1.0 - k_f0);
  const Dvec3 down_sun = exp3(o.extinction * (-p.depth / cos_l));
  const Dvec3 down_sky = exp3(o.extinction * (-p.depth * k_diffuse_path));
  const Dvec3 lit =
      sun_in * (sun_bottom * std::max(brdf_ref::dot(n_b, l), 0.0) / brdf_ref::k_pi) * down_sun +
      sky_in * down_sky;
  const Dvec3 bottom = albedo * lit * exp3(o.extinction * (-p.length)) * o.transmission;
  const double mu = p.length > 0.0 ? p.depth / p.length : 1.0;
  const Dvec3 a_sun = o.extinction * (1.0 + mu / cos_l);
  const Dvec3 a_sky = o.extinction * (1.0 + mu * k_diffuse_path);
  const Dvec3 scattered =
      o.scattering * (sun_in * (sun_surface / (4.0 * brdf_ref::k_pi)) * gather3(a_sun, p.length) +
                      sky_in * 0.25 * gather3(a_sky, p.length));
  return bottom + scattered;
}

// water.slang's `water_radiance_volume`: `radiance` with `under` in place of the painted bottom.
inline Dvec3 radiance_volume(const Params& w, Dvec3 n, Dvec3 v, double from_edge, Dvec3 sun_dir,
                             Dvec3 sun, Dvec3 sky, Dvec3 under) {
  const double shore = w.shore > 0.0 ? brdf_ref::clamp01(from_edge / w.shore) : 1.0;
  const double n_dot_v = std::max(brdf_ref::dot(n, v), brdf_ref::k_min_n_dot_v);
  const double fresnel = schlick(n_dot_v);
  Dvec3 glitter{};
  const double n_dot_l = brdf_ref::dot(n, sun_dir);
  if (n_dot_l > 0.0) {
    const Dvec3 half_sum = sun_dir + v;
    const Dvec3 h = brdf_ref::dot(half_sum, half_sum) > 1e-12 ? brdf_ref::normalize(half_sum) : n;
    const double alpha = brdf_ref::alpha_of(w.roughness);
    const double n_dot_h = brdf_ref::clamp01(brdf_ref::dot(n, h));
    const double v_dot_h = brdf_ref::clamp01(brdf_ref::dot(v, h));
    glitter = sun * (schlick(v_dot_h) * brdf_ref::d_ggx(n_dot_h, alpha) *
                     brdf_ref::v_smith(n_dot_v, n_dot_l, alpha) * n_dot_l);
  }
  return under * (1.0 - fresnel * shore) + (sky * fresnel + glitter) * shore;
}

// ---- the wave as height (S100; water_waves.slang, step for step in double) -----------------------
// `field` is the WaterSurface's field as the CPU holds it: (depth, arrival) per texel.

inline u32 hash2(i32 x, i32 y) {
  u32 h = static_cast<u32>(x) * 0x8DA6B343u ^ static_cast<u32>(y) * 0xD8163841u;
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  h ^= h >> 16;
  return h;
}

inline double value_noise(double x, double y) {
  const double cx = std::floor(x);
  const double cy = std::floor(y);
  double fx = x - cx;
  double fy = y - cy;
  fx = fx * fx * (3.0 - 2.0 * fx);
  fy = fy * fy * (3.0 - 2.0 * fy);
  const i32 ix = static_cast<i32>(cx);
  const i32 iy = static_cast<i32>(cy);
  const auto v = [](u32 h) { return static_cast<double>(h >> 8) * (1.0 / 16777216.0); };
  const double a = v(hash2(ix, iy));
  const double b = v(hash2(ix + 1, iy));
  const double e = v(hash2(ix, iy + 1));
  const double g = v(hash2(ix + 1, iy + 1));
  const double ab = a + (b - a) * fx;
  const double eg = e + (g - e) * fx;
  return ab + (eg - ab) * fy;
}

struct Field {
  double depth = 0.0;
  double arrival = 0.0;
  double depth_grad[2] = {0.0, 0.0};
  double arrival_grad[2] = {0.0, 0.0};
};

inline Field field_at(const gfx::WaterSurface& s, const float* field, double x, double z) {
  Field f;
  const u32 n = s.size;
  if (field == nullptr || n < 2) return f;
  const double last = static_cast<double>(n - 1);
  const double t = static_cast<double>(s.texel_m);
  const double gx = (x - static_cast<double>(s.origin.x)) / t;
  const double gz = (z - static_cast<double>(s.origin.y)) / t;
  const double cx = std::clamp(gx, 0.0, last);
  const double cz = std::clamp(gz, 0.0, last);
  const double ix = std::min(std::floor(cx), last - 1.0);
  const double iz = std::min(std::floor(cz), last - 1.0);
  const double fx = cx - ix;
  const double fz = cz - iz;
  const u32 i = static_cast<u32>(ix);
  const u32 j = static_cast<u32>(iz);
  const auto at = [&](u32 a, u32 b, u32 c) { return static_cast<double>(field[2 * (b * n + a) + c]); };
  for (u32 c = 0; c < 2; ++c) {
    const double a = at(i, j, c), b = at(i + 1, j, c), e = at(i, j + 1, c), h = at(i + 1, j + 1, c);
    const double v = (a + (b - a) * fx) + ((e + (h - e) * fx) - (a + (b - a) * fx)) * fz;
    const double dx = gx >= 0.0 && gx <= last ? ((b - a) + ((h - e) - (b - a)) * fz) / t : 0.0;
    const double dz = gz >= 0.0 && gz <= last ? ((e - a) + ((h - b) - (e - a)) * fx) / t : 0.0;
    if (c == 0) {
      f.depth = v;
      f.depth_grad[0] = dx;
      f.depth_grad[1] = dz;
    } else {
      f.arrival = v;
      f.arrival_grad[0] = dx;
      f.arrival_grad[1] = dz;
    }
  }
  return f;
}

inline double train_shape(double h0, double k0, double d, double theta) {
  if (!(d > 0.0) || !(h0 > 0.0)) return 0.0;
  const double shoaled = h0 * gfx::water_shoaling(gfx::water_wavenumber(k0, d), d);
  const double limit = gfx::k_water_breaking_gamma * d;
  const double height = std::min(shoaled, limit);
  const double b = std::clamp(shoaled / limit, 0.0, 1.0);
  const double m = 1.0 + 2.0 * b;
  const double base = std::max(0.5 + 0.5 * std::cos(theta), 0.0);
  return height * (std::pow(base, m) - 1.0 / std::sqrt(3.14159265358979323846 * (m + 0.25)));
}

inline double train_cycles(const gfx::WaterSurface& s, double t, double arrival) {
  const double ct = t / static_cast<double>(s.period_s);
  const double ca = arrival / static_cast<double>(s.period_s);
  return (ct - 7.0 * std::floor(ct / 7.0)) - (ca - 7.0 * std::floor(ca / 7.0));
}

inline double facing(const gfx::WaterSurface& s, const Field& f) {
  if ((s.flags & gfx::k_water_surface_open) != 0) return 1.0;
  const double len = std::hypot(f.depth_grad[0], f.depth_grad[1]);
  if (!(len > 1e-3)) return 1.0;
  const double d = -(static_cast<double>(s.wind.x) * f.depth_grad[0] +
                     static_cast<double>(s.wind.y) * f.depth_grad[1]) / len;
  return 0.25 + 0.75 * std::clamp(d, 0.0, 1.0);
}

inline double offshore_weight(const gfx::WaterSurface& s, double d) {
  const double k0 = std::max(static_cast<double>(s.k0), 1e-6);
  const double lo = gfx::k_water_train_shallow_kd / k0;
  const double hi = gfx::k_water_train_deep_kd / k0;
  const double x = std::clamp((d - lo) / (hi - lo), 0.0, 1.0);
  return x * x * (3.0 - 2.0 * x);
}

// The height over the still level (no tide), water_waves.slang's `water_height` at no footprint.
inline double height(const gfx::WaterSurface& s, const Field& f, double x, double z, double t) {
  const double d = f.depth;
  const double w_off = s.k0 > 0.0f && s.height_m > 0.0f ? offshore_weight(s, d) : 1.0;
  double off = 0.0;
  if ((s.flags & gfx::k_water_surface_spectrum) != 0) {
    for (u32 i = 0; i < gfx::k_water_waves; ++i) {
      const gfx::WaterWave& wave = s.waves[i];
      const double lambda = k_two_pi / static_cast<double>(wave.k);
      const double along = (static_cast<double>(wave.direction.x) * x +
                            static_cast<double>(wave.direction.y) * z) / lambda;
      const double travel = frac(static_cast<double>(wave.cycles) * (t / k_period_s));
      const double cyc = frac(frac(along) - travel + static_cast<double>(wave.phase));
      off += static_cast<double>(wave.amplitude) * std::sin(k_two_pi * cyc);
    }
  } else {
    for (u32 i = 0; i < 4; ++i) {
      const double lambda = static_cast<double>(s.wavelength) * k_length[i];
      const double a = static_cast<double>(s.amplitude) * k_amplitude[i];
      const double turn = static_cast<double>(s.chop) * k_turn[i] * (0.5 * 3.14159265358979);
      const double c = std::cos(turn), sn = std::sin(turn);
      const double dx = static_cast<double>(s.direction.x) * c - static_cast<double>(s.direction.y) * sn;
      const double dz = static_cast<double>(s.direction.x) * sn + static_cast<double>(s.direction.y) * c;
      const double k = k_two_pi / lambda;
      const double cycles = std::floor(std::sqrt(k_g * k) * (k_period_s / k_two_pi) + 0.5);
      const double along = (dx * x + dz * z) / lambda;
      const double travel = frac(cycles * (t / k_period_s));
      const double cyc = frac(frac(along) - travel + phase(s.seed, i));
      off += a * std::sin(k_two_pi * cyc);
    }
  }
  double train = 0.0;
  if (w_off < 1.0 && d > 0.0 && s.height_m > 0.0f && s.period_s > 0.0f) {
    const double c = train_cycles(s, t, f.arrival);
    const double env = 0.55 + 0.45 * (0.5 - 0.5 * std::cos(k_two_pi * (c + 0.5) / 7.0));
    train = env * facing(s, f) *
            train_shape(static_cast<double>(s.height_m), static_cast<double>(s.k0), d,
                        k_two_pi * frac(c));
  }
  return w_off * off + (1.0 - w_off) * train;
}

inline double swash_shape(double u) {
  const double q = 0.4;
  const double sv = u < q ? u / (2.0 * q) : 0.5 + (u - q) / (2.0 * (1.0 - q));
  return 4.0 * sv * (1.0 - sv);
}

inline double front(const gfx::WaterSurface& s, const Field& f, double x, double z, double t) {
  if (!(s.run_up_m > 0.0f) || !(s.period_s > 0.0f)) return 0.0;
  const double c = train_cycles(s, t, f.arrival);
  const double k = std::floor(c);
  const u32 j = static_cast<u32>(static_cast<i32>(k) + 7 * 64) % 7u;
  const double group = 0.55 + 0.45 * (0.5 - 0.5 * std::cos(k_two_pi * (static_cast<double>(j) + 0.5) / 7.0));
  const double along = std::max(static_cast<double>(s.along_m), 1e-3);
  const double variation = 0.85 + 0.3 * value_noise(x / along + 17.3, z / along + 5.1);
  const double r = static_cast<double>(s.run_up_m);
  const double reach = r * group * variation * facing(s, f);
  const double z_lo = -0.2 * r;
  return z_lo + (reach - z_lo) * swash_shape(c - k);
}

inline double smoothstep(double a, double b, double v) {
  const double x = std::clamp((v - a) / (b - a), 0.0, 1.0);
  return x * x * (3.0 - 2.0 * x);
}

// `water_vertex_lift`: how far above the still level the pool puts a vertex at (x, z), `edge`
// metres in from the outline, at the clock t and the tide's phase. `decided`, when given, says
// whether the vertex is far enough from the front's edge (1 cm) that float and double agree on
// which side of it the vertex is.
inline double vertex_lift(const gfx::WaterSurface& s, const float* field, double x, double z,
                          double edge, double t, double tide_phase, bool* decided = nullptr) {
  if (decided != nullptr) *decided = true;
  const Field f = field_at(s, field, x, z);
  const double tide =
      s.tide_m > 0.0f ? static_cast<double>(s.tide_m) * std::sin(k_two_pi * tide_phase) : 0.0;
  const double fade = (s.flags & gfx::k_water_surface_open) != 0 && s.fade_m > 0.0f
                          ? smoothstep(0.0, static_cast<double>(s.fade_m), edge)
                          : 1.0;
  const double water = (tide + height(s, f, x, z, t)) * fade;
  if (field == nullptr) return water;
  const double d = f.depth + tide;
  const double r = static_cast<double>(s.run_up_m);
  const double band = 0.2 * r + 0.05;
  if (d >= band) return water;
  const double g = -d;
  const double fr = front(s, f, x, z, t);
  if (decided != nullptr) *decided = std::fabs(fr - g) > 0.01;
  const double bank = fr > g ? tide + g + std::min(0.5 * (fr - g), 0.25 * r + 0.02)
                             : tide + std::min(g, 1.15 * r + 0.1) - gfx::k_water_bank_park_m;
  const double share = 1.0 - smoothstep(0.0, band, d);
  return water + (bank - water) * share;
}

}  // namespace engine::water_ref
