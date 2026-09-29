#pragma once

// The CPU mirror of domain/gfx/shaders/ground_detail.slang, in double precision: the same phasor
// ripples (the same lattice, the same hashes and draws — integer arithmetic, so the same kernels —
// the same kernel, profile, taper and analytic gradient), the same value-noise grain, the same
// footprint fades and the same roughness transfer. `ground_detail_tests.cpp` renders the detail on
// the GPU and holds every pixel to this, shaded by `brdf_reference.h`; the renderer's seam tests
// hold the detail view to it at each pixel's own world position. Changing ground_detail.slang means
// changing this file in the same commit, and the tests then say by how much the two disagree.

#include "brdf_reference.h"

#include <core/base/types.h>
#include <domain/gfx/ground_detail.h>

#include <cmath>

namespace engine::ground_ref {

using brdf_ref::Dvec3;

inline constexpr double k_two_pi = 6.28318530717958647692;

inline u32 pcg(u32 v) {
  const u32 state = v * 747796405u + 2891336453u;
  const u32 word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}

inline u32 hash(i32 x, i32 z, u32 seed, u32 stream) {
  u32 h = pcg(seed ^ (stream * 0x9E3779B9u));
  h = pcg(h ^ static_cast<u32>(x));
  return pcg(h ^ static_cast<u32>(z));
}

inline double unit(u32 h) { return static_cast<double>(h >> 8u) * (1.0 / 16777216.0); }

inline double smoothstep(double edge0, double edge1, double x) {
  const double t = brdf_ref::clamp01((x - edge0) / (edge1 - edge0));
  return t * t * (3.0 - 2.0 * t);
}

inline double fade(double cycles) { return 1.0 - smoothstep(0.25, 0.5, cycles); }

struct Ripple {
  double height = 0.0;
  double gx = 0.0;
  double gz = 0.0;
};

// `asymmetry` is the profile's windward share as drawn: the scene's, or the filter's eased one.
inline Ripple ripple(const gfx::GroundDetailParams& d, double x, double z, double asymmetry) {
  const double cell = static_cast<double>(d.cell);
  const double qx = x / cell;
  const double qz = z / cell;
  const i32 bx = static_cast<i32>(std::floor(qx));
  const i32 bz = static_cast<i32>(std::floor(qz));
  const double wx = static_cast<double>(d.wind.x);
  const double wz = static_cast<double>(d.wind.y);
  const double spread = static_cast<double>(d.wind.z);
  const double k = k_two_pi / static_cast<double>(d.wavelength);
  const double inv_r2 = 1.0 / (cell * cell);
  double sr = 0.0, si = 0.0;  // S
  double xr = 0.0, xi = 0.0;  // dS/dx
  double zr = 0.0, zi = 0.0;  // dS/dz
  for (i32 j = -1; j <= 1; ++j) {
    for (i32 i = -1; i <= 1; ++i) {
      const i32 cx = bx + i;
      const i32 cz = bz + j;
      for (u32 n = 0; n < gfx::k_ground_impulses; ++n) {
        u32 h = hash(cx, cz, d.seed, n);
        const double ux = unit(h);
        h = pcg(h);
        const double uz = unit(h);
        const double ox = (qx - (static_cast<double>(cx) + ux)) * cell;
        const double oz = (qz - (static_cast<double>(cz) + uz)) * cell;
        const double r2 = (ox * ox + oz * oz) * inv_r2;
        if (r2 >= 1.0) continue;
        h = pcg(h);
        const double phase0 = unit(h) * k_two_pi;
        h = pcg(h);
        const double js = (unit(h) * 2.0 - 1.0) * spread;
        const double jc = std::sqrt(1.0 - js * js);
        const double dx = wx * jc - wz * js;  // w cos + across sin, across = (-wz, wx)
        const double dz = wz * jc + wx * js;
        const double t = 1.0 - r2;
        const double env = t * t * t;
        const double denv = -6.0 * t * t * inv_r2;
        const double phase = k * (dx * ox + dz * oz) + phase0;
        const double cs = std::cos(phase);
        const double sn = std::sin(phase);
        sr += env * cs;
        si += env * sn;
        xr += cs * denv * ox - sn * env * k * dx;
        xi += sn * denv * ox + cs * env * k * dx;
        zr += cs * denv * oz - sn * env * k * dz;
        zi += sn * denv * oz + cs * env * k * dz;
      }
    }
  }
  Ripple out;
  const double m2 = sr * sr + si * si;
  if (!(m2 > 0.0)) return out;
  const double c2 = static_cast<double>(d.magnitude_floor);
  const double denom = m2 + c2;
  const double taper = m2 / denom;
  const double scale = 2.0 * c2 / (denom * denom);
  const double dtx = (sr * xr + si * xi) * scale;
  const double dtz = (sr * zr + si * zi) * scale;
  const double tpx = (sr * xi - si * xr) / denom;
  const double tpz = (sr * zi - si * zr) / denom;
  const double turn = std::atan2(si, sr) / k_two_pi + 0.5;
  const double a = asymmetry;
  double u = 0.0, du = 0.0;
  if (turn < a) {
    u = 0.5 * turn / a;
    du = 0.5 / a;
  } else {
    u = 0.5 + 0.5 * (turn - a) / (1.0 - a);
    du = 0.5 / (1.0 - a);
  }
  const double profile = -std::cos(k_two_pi * u);
  const double dprofile = std::sin(k_two_pi * u) * du;
  const double amplitude = static_cast<double>(d.amplitude);
  out.height = amplitude * taper * profile;
  out.gx = (dtx * profile + tpx * dprofile) * amplitude;
  out.gz = (dtz * profile + tpz * dprofile) * amplitude;
  return out;
}

// One channel pair of the value noise, each in [-1, 1].
struct Noise2 {
  double a = 0.0;
  double b = 0.0;
};

inline Noise2 lattice2(i32 x, i32 z, u32 seed, u32 stream) {
  const u32 h = hash(x, z, seed, stream);
  return {static_cast<double>(h & 0xffffu) * (2.0 / 65535.0) - 1.0,
          static_cast<double>(h >> 16u) * (2.0 / 65535.0) - 1.0};
}

inline Noise2 value_noise2(double x, double z, double size, u32 seed, u32 stream) {
  const double qx = x / size;
  const double qz = z / size;
  const double fx0 = std::floor(qx);
  const double fz0 = std::floor(qz);
  const double fx = qx - fx0;
  const double fz = qz - fz0;
  const i32 ix = static_cast<i32>(fx0);
  const i32 iz = static_cast<i32>(fz0);
  const double ux = fx * fx * (3.0 - 2.0 * fx);
  const double uz = fz * fz * (3.0 - 2.0 * fz);
  const Noise2 v00 = lattice2(ix, iz, seed, stream);
  const Noise2 v10 = lattice2(ix + 1, iz, seed, stream);
  const Noise2 v01 = lattice2(ix, iz + 1, seed, stream);
  const Noise2 v11 = lattice2(ix + 1, iz + 1, seed, stream);
  auto mix = [](double p, double q, double t) { return p + (q - p) * t; };
  return {mix(mix(v00.a, v10.a, ux), mix(v01.a, v11.a, ux), uz),
          mix(mix(v00.b, v10.b, ux), mix(v01.b, v11.b, ux), uz)};
}

// `ground_exposure`.
inline double exposure(const gfx::GroundDetailParams& d, Dvec3 normal) {
  const double fall =
      (normal.x * static_cast<double>(d.wind.x) + normal.z * static_cast<double>(d.wind.y)) /
      (normal.y > 1e-4 ? normal.y : 1e-4);
  return 1.0 -
         smoothstep(static_cast<double>(d.lee_tan_start), static_cast<double>(d.lee_tan_end), fall);
}

// `ground_gradient_noise3`: three channels of gradient noise at (x, z) on a lattice of `size` —
// the albedo's, the roughness's and the grain's height — each corner's three gradients drawn from
// one hash, five bits a component, with the quintic fade; and the height channel's gradient over
// the lattice's own coordinates (per cell).
struct Grain3 {
  double value[3] = {0.0, 0.0, 0.0};
  double gx = 0.0;
  double gz = 0.0;
};

inline void grain_gradient(u32 h, u32 channel, double& gx, double& gz) {
  const u32 bits = h >> (10u * channel);
  gx = (static_cast<double>(bits & 31u) - 15.5) * (1.0 / 15.5);
  gz = (static_cast<double>((bits >> 5u) & 31u) - 15.5) * (1.0 / 15.5);
}

inline Grain3 gradient_noise3(double x, double z, double size, u32 seed, u32 stream) {
  const double qx = x / size;
  const double qz = z / size;
  const double fx0 = std::floor(qx);
  const double fz0 = std::floor(qz);
  const double fx = qx - fx0;
  const double fz = qz - fz0;
  const i32 ix = static_cast<i32>(fx0);
  const i32 iz = static_cast<i32>(fz0);
  const double ux = fx * fx * fx * (fx * (fx * 6.0 - 15.0) + 10.0);
  const double uz = fz * fz * fz * (fz * (fz * 6.0 - 15.0) + 10.0);
  const double dux = 30.0 * fx * fx * (fx * (fx - 2.0) + 1.0);
  const double duz = 30.0 * fz * fz * (fz * (fz - 2.0) + 1.0);
  const u32 h00 = hash(ix, iz, seed, stream);
  const u32 h10 = hash(ix + 1, iz, seed, stream);
  const u32 h01 = hash(ix, iz + 1, seed, stream);
  const u32 h11 = hash(ix + 1, iz + 1, seed, stream);
  Grain3 out;
  for (u32 c = 0; c < 3; ++c) {
    double ax, az, bx, bz, cx, cz, dx, dz;
    grain_gradient(h00, c, ax, az);
    grain_gradient(h10, c, bx, bz);
    grain_gradient(h01, c, cx, cz);
    grain_gradient(h11, c, dx, dz);
    const double va = ax * fx + az * fz;
    const double vb = bx * (fx - 1.0) + bz * fz;
    const double vc = cx * fx + cz * (fz - 1.0);
    const double vd = dx * (fx - 1.0) + dz * (fz - 1.0);
    const double k1 = vb - va;
    const double k2 = vc - va;
    const double k3 = va - vb - vc + vd;
    out.value[c] = va + k1 * ux + k2 * uz + k3 * ux * uz;
    if (c == 2) {
      out.gx = ax + ux * (bx - ax) + uz * (cx - ax) + ux * uz * (ax - bx - cx + dx) +
               dux * (k1 + k3 * uz);
      out.gz = az + ux * (bz - az) + uz * (cz - az) + ux * uz * (az - bz - cz + dz) +
               duz * (k2 + k3 * ux);
    }
  }
  return out;
}

// A float's step at `m` metres (at least 1): the power of two at or under it times 2^-23.
inline double float_step(double m) {
  int exponent = 0;
  std::frexp(m > 1.0 ? m : 1.0, &exponent);
  return std::ldexp(1.0, exponent - 1 - 23);
}

// `ground_streaks`: the tongues' sum at (x, z) down the fall line `(fx, fz)`, and its gradient.
struct Streak {
  double value = 0.0;
  double gx = 0.0;
  double gz = 0.0;
};

inline Streak streaks(const gfx::GroundDetailParams& d, double x, double z, double fx, double fz) {
  const double cell = static_cast<double>(d.streak_length);
  const double qx = x / cell;
  const double qz = z / cell;
  const i32 bx = static_cast<i32>(std::floor(qx));
  const i32 bz = static_cast<i32>(std::floor(qz));
  const double ax = -fz;
  const double az = fx;
  const double inv_a2 = 4.0 / (cell * cell);
  const double width = static_cast<double>(d.streak_width);
  const double inv_b2 = 4.0 / (width * width);
  Streak out;
  for (i32 j = -1; j <= 1; ++j) {
    for (i32 i = -1; i <= 1; ++i) {
      const i32 cx = bx + i;
      const i32 cz = bz + j;
      for (u32 n = 0; n < gfx::k_ground_streak_impulses; ++n) {
        u32 h = hash(cx, cz, d.seed, 32u + n);
        const double ux = unit(h);
        h = pcg(h);
        const double uz = unit(h);
        const double ox = (qx - (static_cast<double>(cx) + ux)) * cell;
        const double oz = (qz - (static_cast<double>(cz) + uz)) * cell;
        const double u = ox * fx + oz * fz;
        const double w = ox * ax + oz * az;
        const double r2 = u * u * inv_a2 + w * w * inv_b2;
        if (r2 >= 1.0) continue;
        h = pcg(h);
        const double v = unit(h) * 2.0 - 1.0;
        const double t = 1.0 - r2;
        out.value += v * t * t * t;
        const double k = v * -6.0 * t * t;
        out.gx += (fx * u * inv_a2 + ax * w * inv_b2) * k;
        out.gz += (fz * u * inv_a2 + az * w * inv_b2) * k;
      }
    }
  }
  return out;
}

struct Shading {
  Dvec3 normal;
  Dvec3 albedo;
  double roughness = 1.0;
  double weight = 0.0;
  double exposure = 1.0;
  double streak = 0.0;
  double fade = 0.0;
  double ripple = 0.0;
  double grain = 0.0;
};

// `ground_detail_shade`: the same order of decisions, in double.
inline Shading shade(const gfx::GroundDetailParams& d, Dvec3 position, Dvec3 normal, Dvec3 dpdx,
                     Dvec3 dpdy, double mask, Dvec3 albedo, double roughness, bool full = false) {
  Shading out;
  out.normal = normal;
  out.albedo = albedo;
  out.roughness = roughness;
  if (!(mask > 0.0) && !full) return out;
  if ((d.flags & gfx::k_ground_ripples) != 0u) {
    const double slope = smoothstep(static_cast<double>(d.slope_cos_end),
                                    static_cast<double>(d.slope_cos_start), normal.y);
    if ((d.flags & gfx::k_ground_exposure) != 0u) out.exposure = exposure(d, normal);
    const double weight = mask * slope * out.exposure;
    const double wx = static_cast<double>(d.wind.x);
    const double wz = static_cast<double>(d.wind.y);
    const double spread = static_cast<double>(d.wind.z);
    const double fx =
        std::fabs(dpdx.x * wx + dpdx.z * wz) + std::fabs(-dpdx.x * wz + dpdx.z * wx) * spread;
    const double fy =
        std::fabs(dpdy.x * wx + dpdy.z * wz) + std::fabs(-dpdy.x * wz + dpdy.z * wx) * spread;
    const double cycles = (fx > fy ? fx : fy) / static_cast<double>(d.wavelength);
    const double scene_asymmetry = static_cast<double>(d.asymmetry);
    const double asymmetry = 0.5 + (scene_asymmetry - 0.5) * fade(2.0 * cycles);
    const double f = fade(cycles);
    out.weight = weight;
    out.fade = f;
    const double strength = weight * f;
    if (strength > 0.0 || full) {
      const Ripple r = ripple(d, position.x, position.z, full ? scene_asymmetry : asymmetry);
      out.ripple = r.height / static_cast<double>(d.amplitude);
      if (strength > 0.0 && !full) {
        const double s = normal.y * strength;
        out.normal = brdf_ref::normalize(Dvec3{normal.x - r.gx * s, normal.y, normal.z - r.gz * s});
      }
    }
    const double scale = static_cast<double>(d.slope_scale);
    const double full_variance = scale * (1.0 / scene_asymmetry + 1.0 / (1.0 - scene_asymmetry));
    const double drawn_variance = scale * (1.0 / asymmetry + 1.0 / (1.0 - asymmetry)) * f * f;
    const double lost =
        weight * weight * (full_variance > drawn_variance ? full_variance - drawn_variance : 0.0);
    if (lost > 0.0) {
      const double alpha = roughness * roughness;
      out.roughness = std::sqrt(std::sqrt(alpha * alpha + lost));
    }
  }
  if ((d.flags & gfx::k_ground_gradient_grain) != 0u) {
    const double lx = std::sqrt(dpdx.x * dpdx.x + dpdx.z * dpdx.z);
    const double ly = std::sqrt(dpdy.x * dpdy.x + dpdy.z * dpdy.z);
    const double footprint = lx > ly ? lx : ly;
    const double ax = std::fabs(position.x);
    const double az = std::fabs(position.z);
    const double step = float_step(ax > az ? ax : az);
    const double reach = footprint > 4.0 * step ? footprint : 4.0 * step;
    const double octaves = static_cast<double>(d.grain_octaves);
    double g[3] = {0.0, 0.0, 0.0};
    double sx = 0.0, sz = 0.0, kept = 0.0;
    double size = static_cast<double>(d.grain_size);
    for (u32 o = 0; o < d.grain_octaves; ++o) {
      const double f = fade(reach / size);
      if (f > 0.0 || (full && o == 0)) {
        const Grain3 n = gradient_noise3(position.x, position.z, size, d.seed, 16u + o);
        if (o == 0) out.grain = n.value[0] / (3.0 * static_cast<double>(gfx::k_ground_noise_rms));
        for (u32 c = 0; c < 3; ++c)
          g[c] += n.value[c] * f;
        sx += n.gx * f;
        sz += n.gz * f;
      }
      kept += f * f;
      size *= 0.5;
    }
    const double value_scale = static_cast<double>(gfx::k_ground_grain_rms) /
                               (static_cast<double>(gfx::k_ground_noise_rms) * std::sqrt(octaves));
    const double grain_normal = static_cast<double>(d.grain_normal);
    const double slope_scale =
        grain_normal * mask /
        (static_cast<double>(gfx::k_ground_noise_slope_rms) * std::sqrt(octaves));
    if (grain_normal > 0.0 && !full) {
      const Dvec3 n = out.normal;
      const double s = slope_scale * n.y;
      out.normal = brdf_ref::normalize(Dvec3{n.x - sx * s, n.y, n.z - sz * s});
      const double lost = grain_normal * grain_normal * mask * mask * (octaves - kept) / octaves;
      if (lost > 0.0) {
        const double alpha = out.roughness * out.roughness;
        out.roughness = std::sqrt(std::sqrt(alpha * alpha + lost));
      }
    }
    out.albedo = albedo * (1.0 + static_cast<double>(d.grain_albedo) * mask * g[0] * value_scale);
    out.roughness = brdf_ref::clamp01(out.roughness + static_cast<double>(d.grain_roughness) *
                                                          mask * g[1] * value_scale);
  } else if ((d.flags & gfx::k_ground_grain) != 0u) {
    const double lx = std::sqrt(dpdx.x * dpdx.x + dpdx.z * dpdx.z);
    const double ly = std::sqrt(dpdy.x * dpdy.x + dpdy.z * dpdy.z);
    const double footprint = lx > ly ? lx : ly;
    const double size = static_cast<double>(d.grain_size);
    const double coarse = fade(footprint / size);
    const double fine = fade(4.0 * footprint / size);
    double ga = 0.0, gr = 0.0;
    if (coarse > 0.0 || full) {
      const Noise2 v = value_noise2(position.x, position.z, size, d.seed, 16u);
      out.grain = v.a;
      ga += 0.65 * coarse * v.a;
      gr += 0.65 * coarse * v.b;
    }
    if (fine > 0.0) {
      const Noise2 v = value_noise2(position.x, position.z, size * 0.25, d.seed, 17u);
      ga += 0.35 * fine * v.a;
      gr += 0.35 * fine * v.b;
    }
    out.albedo = albedo * (1.0 + static_cast<double>(d.grain_albedo) * mask * ga);
    out.roughness =
        brdf_ref::clamp01(out.roughness + static_cast<double>(d.grain_roughness) * mask * gr);
  }
  if ((d.flags & gfx::k_ground_streaks) != 0u) {
    const double wx = static_cast<double>(d.wind.x);
    const double wz = static_cast<double>(d.wind.y);
    const double fall_tan = (normal.x * wx + normal.z * wz) / (normal.y > 1e-4 ? normal.y : 1e-4);
    const double weight = mask * smoothstep(static_cast<double>(d.streak_tan_start),
                                            static_cast<double>(d.streak_tan_full), fall_tan);
    out.streak = weight;
    if (weight > 0.0 && !full) {
      const double lx = std::sqrt(dpdx.x * dpdx.x + dpdx.z * dpdx.z);
      const double ly = std::sqrt(dpdy.x * dpdy.x + dpdy.z * dpdy.z);
      const double footprint = lx > ly ? lx : ly;
      const double f = fade(footprint / static_cast<double>(d.streak_width));
      if (f > 0.0) {
        const double len = std::sqrt(normal.x * normal.x + normal.z * normal.z);
        const double fx = len > 1e-6 ? normal.x / len : wx;
        const double fz = len > 1e-6 ? normal.z / len : wz;
        const Streak st = streaks(d, position.x, position.z, fx, fz);
        const double strength = weight * f;
        out.albedo =
            out.albedo * (1.0 + static_cast<double>(d.streak_albedo) * strength * st.value);
        out.roughness = brdf_ref::clamp01(out.roughness + static_cast<double>(d.streak_roughness) *
                                                              strength * st.value);
        const double slope = static_cast<double>(d.streak_slope);
        if (slope > 0.0) {
          const Dvec3 n = out.normal;
          const double s = slope * strength * n.y;
          out.normal = brdf_ref::normalize(Dvec3{n.x - st.gx * s, n.y, n.z - st.gz * s});
        }
      }
      const double sn = static_cast<double>(d.streak_normal);
      const double lost = weight * weight * sn * sn * (1.0 - f * f);
      if (lost > 0.0) {
        const double alpha = out.roughness * out.roughness;
        out.roughness = std::sqrt(std::sqrt(alpha * alpha + lost));
      }
    }
  }
  return out;
}

// The world point at screen position (sx, sy) — pixels, fractional — on the horizontal plane
// `plane_y`: `brdf_ref::pixel_on_plane` at any point of the pixel, which a footprint is the
// derivative of.
inline Dvec3 point_on_plane(Dvec3 eye, Dvec3 target, Dvec3 up, double fov_y, double aspect,
                            u32 width, u32 height, double sx, double sy, double plane_y) {
  const double t = 1.0 / std::tan(fov_y * 0.5);
  const Dvec3 f = brdf_ref::normalize(target - eye);
  const Dvec3 right = brdf_ref::normalize(brdf_ref::cross(f, up));
  const Dvec3 camera_up = brdf_ref::cross(right, f);
  const double ndc_x = 2.0 * sx / static_cast<double>(width) - 1.0;
  const double ndc_y = 1.0 - 2.0 * sy / static_cast<double>(height);
  const Dvec3 dir = f + right * (ndc_x * aspect / t) + camera_up * (ndc_y / t);
  return eye + dir * ((plane_y - eye.y) / dir.y);
}

// The footprint the resolve measures at pixel (px, py) of that plane: the point's change per pixel
// along the screen's two axes, by central differences a hundredth of a pixel wide.
inline void plane_footprint(Dvec3 eye, Dvec3 target, Dvec3 up, double fov_y, double aspect,
                            u32 width, u32 height, u32 px, u32 py, double plane_y, Dvec3& dpdx,
                            Dvec3& dpdy) {
  const double cx = static_cast<double>(px) + 0.5;
  const double cy = static_cast<double>(py) + 0.5;
  constexpr double h = 0.005;
  dpdx = (point_on_plane(eye, target, up, fov_y, aspect, width, height, cx + h, cy, plane_y) -
          point_on_plane(eye, target, up, fov_y, aspect, width, height, cx - h, cy, plane_y)) *
         (1.0 / (2.0 * h));
  dpdy = (point_on_plane(eye, target, up, fov_y, aspect, width, height, cx, cy + h, plane_y) -
          point_on_plane(eye, target, up, fov_y, aspect, width, height, cx, cy - h, plane_y)) *
         (1.0 / (2.0 * h));
}

}  // namespace engine::ground_ref
