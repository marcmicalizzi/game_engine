#pragma once

// The double-precision mirror of shaders/wet_ground.slang (roadmap S99; docs/experiments/shore.md,
// "Wet ground"): Lekner and Dorf's darkening by porosity, the roughness under saturation and a
// film, the film's weight, and the capillary fringe's curve. Change one and the other in the same
// commit, and say by how much they still agree.

#include <algorithm>

namespace engine::gfx::wet_reference {

// Lekner and Dorf (1988): r_e = 0.066, n = 1.333, r_i = 1 - (1 - r_e) / n^2.
inline constexpr double k_external = 0.066;
inline constexpr double k_index = 1.333;
inline double internal() { return 1.0 - (1.0 - k_external) / (k_index * k_index); }
inline double through() { return (1.0 - k_external) * (1.0 - internal()); }

inline constexpr double k_porosity_full = 0.3;
inline constexpr double k_damp_roughness = 0.7;
inline constexpr double k_film_roughness = 0.05;
inline constexpr double k_film_from = 0.85;

inline double saturate(double x) { return std::clamp(x, 0.0, 1.0); }
inline double lerp(double a, double b, double t) { return a + (b - a) * t; }
inline double smoothstep(double e0, double e1, double x) {
  const double t = saturate((x - e0) / (e1 - e0));
  return t * t * (3.0 - 2.0 * t);
}

// The wet albedo of a dry one: a (1 - r_e)(1 - r_i) / (1 - r_i a), never above a.
inline double darkening(double a) {
  return std::min(through() / (1.0 - internal() * saturate(a)), 1.0);
}
inline double albedo(double a, double s, double porosity) {
  const double weight = saturate(s) * saturate(porosity / k_porosity_full);
  return a * lerp(1.0, darkening(a), weight);
}
inline double film(double s, double f) {
  if (f < 0.0) return 0.0;  // no film at all: the resolve's k_shadow_wet_film_off
  const double standing = saturate((s - k_film_from) / (1.0 - k_film_from));
  return saturate(std::max(f, standing * standing));
}
inline double roughness(double r, double s, double f) {
  const double sat = saturate(s);
  const double damp = r * lerp(1.0, k_damp_roughness, sat);
  return lerp(damp, std::min(damp, k_film_roughness), film(sat, f));
}
inline double fringe(double h, double fringe_m) {
  if (!(fringe_m > 0.0)) return h < 0.0 ? 1.0 : 0.0;
  if (h <= 0.0) return 1.0;
  return 1.0 - smoothstep(0.0, fringe_m, h);
}

}  // namespace engine::gfx::wet_reference
