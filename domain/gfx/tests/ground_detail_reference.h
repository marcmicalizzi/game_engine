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

// A lattice index as the shader's 32-bit integers carry it: the integer `n` (a whole number in a
// double, as large as 10,000 km in millimetre cells makes it) wrapped to its low 32 bits, which is
// what the hash reads. The mirror takes every lattice from the world position itself, in double,
// and never from the block's frame: it is the function the frame has to reproduce, so it does not
// get to use it (gfx.md, "Far from the origin").
inline i32 index(double n) { return gfx::ground_wrap(n); }
inline i32 index(double n, i32 plus) { return gfx::ground_wrap(n + static_cast<double>(plus)); }

inline double smoothstep(double edge0, double edge1, double x) {
  const double t = brdf_ref::clamp01((x - edge0) / (edge1 - edge0));
  return t * t * (3.0 - 2.0 * t);
}

inline double fade(double cycles) { return 1.0 - smoothstep(0.25, 0.5, cycles); }

struct Ripple {
  double height = 0.0;
  double gx = 0.0;
  double gz = 0.0;
  double turn = 0.0;   // the sum's phase as a fraction of a turn, (0, 1] (tests read it)
  double taper = 0.0;  // |S|^2 / (|S|^2 + c^2)
};

// `asymmetry` is the profile's windward share as drawn: the scene's, or the filter's eased one.
// `scale` multiplies the wavelength and the height: the spacing's, 1 without it.
// `(wx, wz)` and `spread` are the ripples' direction and kernel spread here, `travel` how far they
// have moved along it (`ground_ripple`'s last three).
inline Ripple ripple(const gfx::GroundDetailParams& d, double x, double z, double asymmetry,
                     double scale, double wx, double wz, double spread, double travel) {
  const double cell = static_cast<double>(d.cell);
  const double qx = x / cell;
  const double qz = z / cell;
  const double bx = std::floor(qx);
  const double bz = std::floor(qz);
  const double k = k_two_pi / (static_cast<double>(d.wavelength) * scale);
  // The base wavelength's phase whatever the scale (ground_ripple: a ripple s times as long moves
  // 1/s as far, and a whole number of base wavelengths of travel moves nothing).
  const double shift = k_two_pi * travel / static_cast<double>(d.wavelength);
  const double inv_r2 = 1.0 / (cell * cell);
  double sr = 0.0, si = 0.0;  // S
  double xr = 0.0, xi = 0.0;  // dS/dx
  double zr = 0.0, zi = 0.0;  // dS/dz
  for (i32 j = -1; j <= 1; ++j) {
    for (i32 i = -1; i <= 1; ++i) {
      const double cx = bx + i;
      const double cz = bz + j;
      for (u32 n = 0; n < gfx::k_ground_impulses; ++n) {
        u32 h = hash(index(cx), index(cz), d.seed, n);
        const double ux = unit(h);
        h = pcg(h);
        const double uz = unit(h);
        const double ox = (qx - (cx + ux)) * cell;
        const double oz = (qz - (cz + uz)) * cell;
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
        const double phase = k * (dx * ox + dz * oz) + phase0 - shift;
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
  const double dtaper = 2.0 * c2 / (denom * denom);
  const double dtx = (sr * xr + si * xi) * dtaper;
  const double dtz = (sr * zr + si * zi) * dtaper;
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
  const double amplitude = static_cast<double>(d.amplitude) * scale;
  out.turn = turn;
  out.taper = taper;
  out.height = amplitude * taper * profile;
  out.gx = (dtx * profile + tpx * dprofile) * amplitude;
  out.gz = (dtz * profile + tpz * dprofile) * amplitude;
  return out;
}

// The block's own wind, spread and no travel.
inline Ripple ripple(const gfx::GroundDetailParams& d, double x, double z, double asymmetry,
                     double scale = 1.0) {
  return ripple(d, x, z, asymmetry, scale, static_cast<double>(d.wind.x),
                static_cast<double>(d.wind.y), static_cast<double>(d.wind.z), 0.0);
}

// `ground_steer`.
inline void steer(const gfx::GroundDetailParams& d, double& wx, double& wz, Dvec3 normal) {
  const double ny = normal.y > 1e-4 ? normal.y : 1e-4;
  const double gx = normal.x / ny;
  const double gz = normal.z / ny;
  const double gl = std::sqrt(gx * gx + gz * gz);
  if (!(gl > 1e-6)) return;
  const double fx = gx / gl;
  const double fz = gz / gl;
  const double sr = static_cast<double>(d.steer_gain) * gl;
  const double r = sr < 0.5 ? sr : 0.5;
  const double along = (wx * fx + wz * fz) * r;
  const double bx = wx - fx * along;
  const double bz = wz - fz * along;
  const double len = std::sqrt(bx * bx + bz * bz);
  if (!(len > 1e-6)) return;
  const double ux = bx / len;
  const double uz = bz / len;
  const double cs = wx * ux + wz * uz;
  const double cos_max = static_cast<double>(d.steer_cos_max);
  if (cs >= cos_max) {
    wx = ux;
    wz = uz;
    return;
  }
  const double sn = wx * uz - wz * ux;
  const double s =
      sn >= 0.0 ? static_cast<double>(d.steer_sin_max) : -static_cast<double>(d.steer_sin_max);
  const double ox = wx;
  const double oz = wz;
  wx = ox * cos_max - oz * s;
  wz = oz * cos_max + ox * s;
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
  const i32 ix = index(fx0);
  const i32 iz = index(fz0);
  const i32 ix1 = index(fx0, 1);
  const i32 iz1 = index(fz0, 1);
  const double ux = fx * fx * (3.0 - 2.0 * fx);
  const double uz = fz * fz * (3.0 - 2.0 * fz);
  const Noise2 v00 = lattice2(ix, iz, seed, stream);
  const Noise2 v10 = lattice2(ix1, iz, seed, stream);
  const Noise2 v01 = lattice2(ix, iz1, seed, stream);
  const Noise2 v11 = lattice2(ix1, iz1, seed, stream);
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
  const i32 ix = index(fx0);
  const i32 iz = index(fz0);
  const i32 ix1 = index(fx0, 1);
  const i32 iz1 = index(fz0, 1);
  const double ux = fx * fx * fx * (fx * (fx * 6.0 - 15.0) + 10.0);
  const double uz = fz * fz * fz * (fz * (fz * 6.0 - 15.0) + 10.0);
  const double dux = 30.0 * fx * fx * (fx * (fx - 2.0) + 1.0);
  const double duz = 30.0 * fz * fz * (fz * (fz - 2.0) + 1.0);
  const u32 h00 = hash(ix, iz, seed, stream);
  const u32 h10 = hash(ix1, iz, seed, stream);
  const u32 h01 = hash(ix, iz1, seed, stream);
  const u32 h11 = hash(ix1, iz1, seed, stream);
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
  const double bx = std::floor(qx);
  const double bz = std::floor(qz);
  const double ax = -fz;
  const double az = fx;
  const double inv_a2 = 4.0 / (cell * cell);
  const double width = static_cast<double>(d.streak_width);
  const double inv_b2 = 4.0 / (width * width);
  Streak out;
  for (i32 j = -1; j <= 1; ++j) {
    for (i32 i = -1; i <= 1; ++i) {
      const double cx = bx + i;
      const double cz = bz + j;
      for (u32 n = 0; n < gfx::k_ground_streak_impulses; ++n) {
        u32 h = hash(index(cx), index(cz), d.seed, 32u + n);
        const double ux = unit(h);
        h = pcg(h);
        const double uz = unit(h);
        const double ox = (qx - (cx + ux)) * cell;
        const double oz = (qz - (cz + uz)) * cell;
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

// `ground_patch`: two channels in [-1, 1].
inline void patch(const gfx::GroundDetailParams& d, double x, double z, double& v0, double& v1) {
  const double size = static_cast<double>(d.patch_size);
  const Grain3 a = gradient_noise3(x, z, size, d.seed, 64u);
  const Grain3 b = gradient_noise3(x, z, size * 0.5, d.seed, 65u);
  const double k = 0.5 / (static_cast<double>(gfx::k_ground_noise_rms) * 1.118034);
  const auto clamp1 = [](double v) { return v < -1.0 ? -1.0 : (v > 1.0 ? 1.0 : v); };
  v0 = clamp1((a.value[0] + b.value[0] * 0.5) * k);
  v1 = clamp1((a.value[1] + b.value[1] * 0.5) * k);
}

// `ground_grainflow`: the lanes' value and gradient at (x, z) down the fall line (fx, fz).
struct Flow {
  double value = 0.0;
  double gx = 0.0;
  double gz = 0.0;
};

// `ground_episode`.
inline double episode(double phase, double share) {
  const double t = phase / share;
  if (!(t < 1.0)) return 0.0;
  const double up = brdf_ref::clamp01(t / 0.05);
  const double down = brdf_ref::clamp01((t - 0.3) / 0.7);
  return up * up * (3.0 - 2.0 * up) * (1.0 - down * down * (3.0 - 2.0 * down));
}

// `ground_smooth_d`: a smoothstep and its derivative.
inline void smooth_d(double e0, double e1, double x, double& v, double& dv) {
  const double t = brdf_ref::clamp01((x - e0) / (e1 - e0));
  v = t * t * (3.0 - 2.0 * t);
  dv = 6.0 * t * (1.0 - t) / (e1 - e0);
}

// `GroundTongue`.
struct Tongue {
  double len = 0.0, half0 = 0.0, a1 = 0.0, k1 = 0.0, p1 = 0.0, a2 = 0.0, k2 = 0.0, p2 = 0.0;
  double split = 0.0, split_at = 0.0;
};

// `ground_lobe`.
inline double lobe(double v, double hw, double tau) {
  const double r = v / hw;
  const double r2 = r * r;
  if (!(r2 < 1.0)) return 0.0;
  const double q = 1.0 - r2;
  double toe = 0.0, unused = 0.0;
  smooth_d(0.0, 1.0, (tau - 0.8 + 0.12 * r2) / 0.2, toe, unused);
  return q * q * (1.0 - toe);
}

// `ground_tongue`.
inline double tongue(const Tongue& g, double x, double tl) {
  const double tau = tl / g.len;
  if (!(tau > 0.0 && tau < 1.0)) return 0.0;
  const double grow = 0.3 + 0.7 * tau;
  const double centre =
      grow * (g.a1 * std::sin(g.k1 * tl + g.p1) + g.a2 * std::sin(g.k2 * tl + g.p2));
  const double pinch = 1.0 + 0.12 * std::sin(1.7 * g.k2 * tl + g.p1);
  const double half_w = g.half0 * (0.6 + 0.8 * tau) * pinch;
  const double u = x - centre;
  const double r = u / half_w;
  const double r2 = r * r;
  double chute = 0.0;
  if (r2 < 1.0) {
    const double q = 1.0 - r2;
    chute = -0.3 * q * q;
  }
  double v = 0.0, dv = 0.0;
  smooth_d(g.split_at, g.split_at + 0.25, tau, v, dv);
  const double s = g.split * v;
  const double d = s * 0.55 * half_w;
  const double hw = half_w * (1.0 - 0.3 * s);
  const double pair = lobe(u - d, hw, tau) + lobe(u + d, hw, tau);
  const double ro = 2.0 * d / hw;
  const double overlap = ro * ro < 1.0 ? (1.0 - ro * ro) * (1.0 - ro * ro) : 0.0;
  const double lobes = pair / (1.0 + overlap);
  double lw = 0.0, cw = 0.0, head = 0.0;
  smooth_d(0.35, 0.8, tau, lw, dv);
  smooth_d(0.2, 0.55, tau, cw, dv);
  smooth_d(0.0, 0.1, tau, head, dv);
  return head * (lw * lobes + (1.0 - cw) * chute);
}

// One direction's tongues (`ground_lane_set`).
inline Flow lane_set(const gfx::GroundDetailParams& d, double x, double z, u32 k) {
  const double angle = static_cast<double>(k) * (k_two_pi / 16.0);
  const double dx = std::cos(angle);
  const double dz = std::sin(angle);
  const double ax = -dz;
  const double az = dx;
  const double width = static_cast<double>(d.flow_width);
  const double spacing = 2.0 * width;
  const double seg = static_cast<double>(d.flow_length);
  const double sa = x * ax + z * az;
  const double t = x * dx + z * dz;
  const double mc = std::floor(sa / spacing + 0.5);
  const double e = 0.002;
  Flow out;
  for (i32 i = -1; i <= 1; ++i) {
    const double m = mc + i;
    // The golden ratio's fraction of the lane's index in 32-bit fixed point, as the shader takes
    // it: exact for every lane, so the two agree to the bit 10,000 km out.
    const double stagger = unit(static_cast<u32>(index(m)) * 0x9E3779B9u) * seg;
    const double j = std::floor((t - stagger) / seg);
    u32 h = hash(index(m), index(j), d.seed, 48u + k);
    const double jitter = (unit(h) - 0.5) * 0.5 * spacing;
    h = pcg(h);
    const double wscale = 0.7 + 0.6 * unit(h);
    h = pcg(h);
    const double lenf = 0.45 + 0.5 * unit(h);
    h = pcg(h);
    const double startf = unit(h) * (1.0 - lenf);
    double amp = 1.0;
    if (d.flow_share < 1.0f) {
      h = pcg(h ^ 0x68E31DA4u);
      const double phase = static_cast<double>(d.flow_clock) + unit(h);
      amp = episode(phase - std::floor(phase), static_cast<double>(d.flow_share));
    }
    if (!(amp > 0.0)) continue;
    Tongue g;
    g.len = lenf * seg;
    const double tl = t - (j * seg + stagger + startf * seg);
    if (!(tl > 0.0 && tl < g.len)) continue;
    const double off = sa - (m * spacing + jitter);
    g.half0 = 0.5 * width * wscale;
    h = pcg(h);
    g.a1 = width * (0.13 + 0.29 * unit(h));
    h = pcg(h);
    g.k1 = k_two_pi / (8.0 + 8.0 * unit(h));
    h = pcg(h);
    g.p1 = k_two_pi * unit(h);
    g.a2 = 0.3 * g.a1;
    h = pcg(h);
    g.k2 = k_two_pi / (2.0 + 2.0 * unit(h));
    h = pcg(h);
    g.p2 = k_two_pi * unit(h);
    h = pcg(h);
    g.split = unit(h) < 0.3 ? 1.0 : 0.0;
    h = pcg(h);
    g.split_at = 0.45 + 0.2 * unit(h);
    if (std::fabs(off) > 1.6 * spacing) continue;
    const double height = tongue(g, off, tl);
    const double dh_dx = (tongue(g, off + e, tl) - height) / e;
    const double dh_dt = (tongue(g, off, tl + e) - height) / e;
    out.value += amp * height;
    out.gx += amp * (ax * dh_dx + dx * dh_dt);
    out.gz += amp * (az * dh_dx + dz * dh_dt);
  }
  return out;
}

// `ground_grainflow`: the two directions nearest the fall line (fx, fz), blended by the angle.
inline Flow grainflow(const gfx::GroundDetailParams& d, double x, double z, double fx, double fz) {
  const double sector = k_two_pi / 16.0;
  double turn = std::atan2(fz, fx) / sector;
  if (turn < 0.0) turn += 16.0;
  const double k0 = std::min(std::floor(turn), 15.0);
  const double t = brdf_ref::clamp01((turn - k0 - 0.4) / 0.2);
  const double w = t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
  const u32 i0 = static_cast<u32>(k0);
  const u32 i1 = (i0 + 1u) % 16u;
  const Flow a = lane_set(d, x, z, i0);
  const Flow b = lane_set(d, x, z, i1);
  const double norm = 1.0 / std::sqrt((1.0 - w) * (1.0 - w) + w * w);
  return {(a.value * (1.0 - w) + b.value * w) * norm, (a.gx * (1.0 - w) + b.gx * w) * norm,
          (a.gz * (1.0 - w) + b.gz * w) * norm};
}

struct Shading {
  Dvec3 normal;
  Dvec3 albedo;
  double roughness = 1.0;
  double weight = 0.0;
  double exposure = 1.0;
  double streak = 0.0;
  double flow = 0.0;
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
    double weight = mask * slope * out.exposure;
    if ((d.flags & gfx::k_ground_motion) != 0u) weight *= static_cast<double>(d.ripple_live);
    double wx = static_cast<double>(d.wind.x);
    double wz = static_cast<double>(d.wind.y);
    double spread = static_cast<double>(d.wind.z);
    if ((d.flags & gfx::k_ground_steering) != 0u) steer(d, wx, wz, normal);
    double scale = 1.0;
    if ((d.flags & gfx::k_ground_spacing) != 0u) {
      const double climb =
          -(normal.x * static_cast<double>(d.wind.x) + normal.z * static_cast<double>(d.wind.y)) /
          (normal.y > 1e-4 ? normal.y : 1e-4);
      const double sc = 1.0 + static_cast<double>(d.spacing_gain) * climb;
      const double lo = static_cast<double>(d.spacing_min);
      const double hi = static_cast<double>(d.spacing_max);
      scale = sc < lo ? lo : (sc > hi ? hi : sc);
    }
    if ((d.flags & gfx::k_ground_patches) != 0u) {
      double v0 = 0.0, v1 = 0.0;
      patch(d, position.x, position.z, v0, v1);
      scale *=
          static_cast<double>(d.patch_mid) * std::exp2(v0 * static_cast<double>(d.patch_half_log2));
      const double sp = spread * (1.0 + v1 * static_cast<double>(d.patch_defects));
      spread = sp < 0.9 ? sp : 0.9;
    }
    const double fx =
        std::fabs(dpdx.x * wx + dpdx.z * wz) + std::fabs(-dpdx.x * wz + dpdx.z * wx) * spread;
    const double fy =
        std::fabs(dpdy.x * wx + dpdy.z * wz) + std::fabs(-dpdy.x * wz + dpdy.z * wx) * spread;
    double cycles = (fx > fy ? fx : fy) / (static_cast<double>(d.wavelength) * scale);
    double travel = 0.0;
    if ((d.flags & gfx::k_ground_motion) != 0u) {
      travel = static_cast<double>(d.travel);
      const double moving =
          static_cast<double>(d.travel_per_frame) / (static_cast<double>(d.wavelength) * scale);
      cycles = cycles > moving ? cycles : moving;
    }
    const double scene_asymmetry = static_cast<double>(d.asymmetry);
    const double asymmetry = 0.5 + (scene_asymmetry - 0.5) * fade(2.0 * cycles);
    const double f = fade(cycles);
    out.weight = weight;
    out.fade = f;
    const double strength = weight * f;
    if (strength > 0.0 || full) {
      const Ripple r = ripple(d, position.x, position.z, full ? scene_asymmetry : asymmetry, scale,
                              wx, wz, spread, travel);
      out.ripple = r.height / (static_cast<double>(d.amplitude) * scale);
      if (strength > 0.0 && !full) {
        const double s = normal.y * strength;
        out.normal = brdf_ref::normalize(Dvec3{normal.x - r.gx * s, normal.y, normal.z - r.gz * s});
      }
    }
    const double slope_scale = static_cast<double>(d.slope_scale);
    const double full_variance =
        slope_scale * (1.0 / scene_asymmetry + 1.0 / (1.0 - scene_asymmetry));
    const double drawn_variance = slope_scale * (1.0 / asymmetry + 1.0 / (1.0 - asymmetry)) * f * f;
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
    // The shader's own precision, which is its local coordinate's: the point less the block's
    // frame origin (zero for a block no frame was set on, where it is the world position).
    const double ax = std::fabs(position.x - static_cast<double>(d.origin_x));
    const double az = std::fabs(position.z - static_cast<double>(d.origin_z));
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
  if ((d.flags & gfx::k_ground_grainflow) != 0u) {
    const double fall_tan =
        (normal.x * static_cast<double>(d.wind.x) + normal.z * static_cast<double>(d.wind.y)) /
        (normal.y > 1e-4 ? normal.y : 1e-4);
    const double weight = mask * smoothstep(static_cast<double>(d.flow_tan_start),
                                            static_cast<double>(d.flow_tan_full), fall_tan);
    out.flow = weight;
    if (weight > 0.0 && !full) {
      const double lx = std::sqrt(dpdx.x * dpdx.x + dpdx.z * dpdx.z);
      const double ly = std::sqrt(dpdy.x * dpdy.x + dpdy.z * dpdy.z);
      const double footprint = lx > ly ? lx : ly;
      const double share = static_cast<double>(d.flow_share);
      const double width = static_cast<double>(d.flow_width);
      const double f =
          fade(footprint / (static_cast<double>(gfx::k_ground_flow_body) * width)) *
          (d.flow_share < 1.0f ? fade(static_cast<double>(d.flow_clock_step) / share) : 1.0);
      if (f > 0.0) {
        const double len = std::sqrt(normal.x * normal.x + normal.z * normal.z);
        const double fx = len > 1e-6 ? normal.x / len : static_cast<double>(d.wind.x);
        const double fz = len > 1e-6 ? normal.z / len : static_cast<double>(d.wind.y);
        const Flow lanes = grainflow(d, position.x, position.z, fx, fz);
        const double strength = weight * f;
        out.albedo =
            out.albedo * (1.0 + static_cast<double>(d.flow_albedo) * strength * lanes.value);
        const Dvec3 n = out.normal;
        const double s =
            static_cast<double>(d.flow_slope) * static_cast<double>(d.flow_width) * strength * n.y;
        out.normal = brdf_ref::normalize(Dvec3{n.x - lanes.gx * s, n.y, n.z - lanes.gz * s});
      }
      const double fn = static_cast<double>(d.flow_normal);
      const double lost = weight * weight * fn * fn * (1.0 - f * f) * (share < 1.0 ? share : 1.0);
      if (lost > 0.0) {
        const double alpha = out.roughness * out.roughness;
        out.roughness = std::sqrt(std::sqrt(alpha * alpha + lost));
      }
    }
  }
  return out;
}

// The ergs' numbers, field for field: content/test-scenes/desert-erg/scene.json, `terrain.detail`,
// which turns every second-pass term on. The GPU cases in both modules draw them; the renderer's
// rings test, which can read that scene (it names the dune generator), holds this copy to the file.
inline gfx::GroundDetailDesc erg_numbers() {
  gfx::GroundDetailDesc d;
  d.ripple_wavelength = 0.12f;
  d.ripple_height = 0.008f;
  d.ripple_asymmetry = 0.75f;
  d.ripple_defects = 0.35f;
  d.slope_start_deg = 22.0f;
  d.slope_end_deg = 30.0f;
  d.grain_size = 0.02f;
  d.grain_albedo = 0.08f;
  d.grain_roughness = 0.05f;
  d.lee_start_deg = 10.0f;
  d.lee_end_deg = 18.0f;
  d.grain_finest = 0.001f;
  d.grain_normal = 0.06f;
  d.streak_start_deg = 0.0f;  // the second pass's streaks, replaced by the grainflow below
  d.streak_full_deg = 0.0f;
  d.streak_width = 0.4f;
  d.streak_length = 2.0f;
  d.streak_albedo = 0.06f;
  d.streak_roughness = 0.04f;
  d.streak_normal = 0.012f;
  d.spacing_gain = 2.5f;
  d.spacing_min = 0.8f;
  d.spacing_max = 1.6f;
  d.flow_start_deg = 24.0f;
  d.flow_full_deg = 30.0f;
  d.flow_cell = 10.0f;
  d.flow_width = 0.6f;
  d.flow_normal = 0.05f;
  d.flow_albedo = 0.015f;
  d.flow_widening = 0.3f;
  d.flow_length = 24.0f;
  d.flow_share = 0.5f;
  d.flow_turnover = 800.0f;
  d.patch_size = 30.0f;
  d.patch_min = 0.7f;
  d.patch_max = 1.4f;
  d.patch_defects = 0.5f;
  d.steer_max_deg = 20.0f;
  d.steer_gain = 2.0f;
  d.ripple_celerity = 2000.0f;
  d.flatten_start = 1.6f;
  d.flatten_end = 2.2f;
  return d;
}

// The world point at screen position (sx, sy) — pixels, fractional — on the plane through
// `plane_point` with normal `plane_normal` (any length): the camera ray through that point of the
// pixel, which a footprint is the derivative of. The ground's detail reads the normal as much as
// the point (the exposure, the spacing and the streaks are functions of it), so a sloped ground is
// a plane of its own and not a height over a level one.
inline Dvec3 point_on_plane(Dvec3 eye, Dvec3 target, Dvec3 up, double fov_y, double aspect,
                            u32 width, u32 height, double sx, double sy, Dvec3 plane_point,
                            Dvec3 plane_normal) {
  const Dvec3 dir =
      brdf_ref::pixel_direction(eye, target, up, fov_y, aspect, width, height, sx, sy);
  return eye +
         dir * (brdf_ref::dot(plane_point - eye, plane_normal) / brdf_ref::dot(dir, plane_normal));
}

// The horizontal plane `plane_y`, which is `brdf_ref::pixel_on_plane` at any point of the pixel:
// the same arithmetic as the general form's with a normal straight up, term for term.
inline Dvec3 point_on_plane(Dvec3 eye, Dvec3 target, Dvec3 up, double fov_y, double aspect,
                            u32 width, u32 height, double sx, double sy, double plane_y) {
  return point_on_plane(eye, target, up, fov_y, aspect, width, height, sx, sy,
                        Dvec3{0.0, plane_y, 0.0}, Dvec3{0.0, 1.0, 0.0});
}

// The footprint the resolve measures at pixel (px, py) of that plane: the point's change per pixel
// along the screen's two axes, by central differences a hundredth of a pixel wide.
inline void plane_footprint(Dvec3 eye, Dvec3 target, Dvec3 up, double fov_y, double aspect,
                            u32 width, u32 height, u32 px, u32 py, Dvec3 plane_point,
                            Dvec3 plane_normal, Dvec3& dpdx, Dvec3& dpdy) {
  const double cx = static_cast<double>(px) + 0.5;
  const double cy = static_cast<double>(py) + 0.5;
  constexpr double h = 0.005;
  const auto at = [&](double sx, double sy) {
    return point_on_plane(eye, target, up, fov_y, aspect, width, height, sx, sy, plane_point,
                          plane_normal);
  };
  dpdx = (at(cx + h, cy) - at(cx - h, cy)) * (1.0 / (2.0 * h));
  dpdy = (at(cx, cy + h) - at(cx, cy - h)) * (1.0 / (2.0 * h));
}

inline void plane_footprint(Dvec3 eye, Dvec3 target, Dvec3 up, double fov_y, double aspect,
                            u32 width, u32 height, u32 px, u32 py, double plane_y, Dvec3& dpdx,
                            Dvec3& dpdy) {
  plane_footprint(eye, target, up, fov_y, aspect, width, height, px, py, Dvec3{0.0, plane_y, 0.0},
                  Dvec3{0.0, 1.0, 0.0}, dpdx, dpdy);
}

}  // namespace engine::ground_ref
