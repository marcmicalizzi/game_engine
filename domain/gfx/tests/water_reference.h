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

}  // namespace engine::water_ref
