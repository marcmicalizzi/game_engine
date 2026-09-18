#pragma once

// The CPU mirror of domain/gfx/shaders/brdf.slang and of the shading block of
// visibility_resolve.slang, in double precision: the same Cook-Torrance BSDF (GGX, Smith
// height-correlated visibility, Schlick Fresnel), the same windowed inverse-square falloff, the
// same sky hemisphere, the same tangent frame and normal-map perturbation, and the same display
// gamma. The shading and attributes tests render a known surface on the GPU and compare the
// pixels against this, so the lighting model is pinned by a second implementation rather than by
// numbers nobody can rederive. Changing brdf.slang means changing this file in the same commit;
// the tests then say by how much the two disagree.

#include <core/base/types.h>
#include <core/math/math.h>

#include <cmath>

namespace engine::brdf_ref {

struct Dvec3 {
  double x = 0.0, y = 0.0, z = 0.0;
};

inline Dvec3 operator+(Dvec3 a, Dvec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Dvec3 operator-(Dvec3 a, Dvec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Dvec3 operator*(Dvec3 a, Dvec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline Dvec3 operator*(Dvec3 v, double s) { return {v.x * s, v.y * s, v.z * s}; }
inline Dvec3 operator-(Dvec3 v) { return {-v.x, -v.y, -v.z}; }
inline double dot(Dvec3 a, Dvec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Dvec3 cross(Dvec3 a, Dvec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Dvec3 normalize(Dvec3 v) { return v * (1.0 / std::sqrt(dot(v, v))); }
inline Dvec3 splat(double s) { return {s, s, s}; }
inline Dvec3 dvec3(Vec3 v) {
  return {static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)};
}
inline Dvec3 dvec3(Vec4 v) {
  return {static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)};
}
inline double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
inline Dvec3 lerp(Dvec3 a, Dvec3 b, double t) { return a + (b - a) * t; }

inline constexpr double k_pi = 3.14159265358979323846;
inline constexpr double k_min_roughness = 0.045;

struct Surface {
  Dvec3 position;
  Dvec3 normal;  // unit, out of the surface
  Dvec3 view;    // unit, towards the eye
  Dvec3 albedo;  // linear, after the albedo texture
  double roughness = 1.0;
  double metallic = 0.0;
};

// Mirrors gfx::ResolveLight, plus what the resolve's shadow ray decides about it: `shadowed`
// removes the light's whole contribution, which is what a hit between the surface and the light
// does in fs_resolve (the ambient and emissive terms never see a shadow ray).
struct Light {
  Dvec3 position;
  double radius = 0.0;
  Dvec3 color;
  double intensity = 0.0;
  Dvec3 direction;  // spot only, towards the lit surface
  bool spot = false;
  double cos_inner = 1.0;
  double cos_outer = 0.0;
  bool shadowed = false;
};

inline double alpha_of(double roughness) {
  const double r =
      roughness < k_min_roughness ? k_min_roughness : (roughness > 1.0 ? 1.0 : roughness);
  return r * r;
}

inline Dvec3 f0_of(Dvec3 albedo, double metallic) { return lerp(splat(0.04), albedo, metallic); }

inline double d_ggx(double n_dot_h, double alpha) {
  const double a2 = alpha * alpha;
  const double d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
  const double denom = k_pi * d * d;
  return a2 / (denom < 1e-9 ? 1e-9 : denom);
}

inline double v_smith(double n_dot_v, double n_dot_l, double alpha) {
  const double a2 = alpha * alpha;
  const double v = n_dot_l * std::sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
  const double l = n_dot_v * std::sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
  const double denom = v + l;
  return 0.5 / (denom < 1e-9 ? 1e-9 : denom);
}

inline Dvec3 f_schlick(Dvec3 f0, double cos_theta) {
  const double m = clamp01(1.0 - cos_theta);
  const double m2 = m * m;
  return f0 + (splat(1.0) - f0) * (m2 * m2 * m);
}

inline Dvec3 eval(const Surface& s, Dvec3 light_dir) {
  const double n_dot_l = dot(s.normal, light_dir);
  const double n_dot_v = dot(s.normal, s.view);
  if (n_dot_l <= 0.0 || n_dot_v <= 0.0) return {};
  const Dvec3 h = normalize(light_dir + s.view);
  const double n_dot_h = clamp01(dot(s.normal, h));
  const double v_dot_h = clamp01(dot(s.view, h));
  const double alpha = alpha_of(s.roughness);
  const Dvec3 f = f_schlick(f0_of(s.albedo, s.metallic), v_dot_h);
  const Dvec3 specular = f * (d_ggx(n_dot_h, alpha) * v_smith(n_dot_v, n_dot_l, alpha));
  const Dvec3 diffuse = (splat(1.0) - f) * s.albedo * ((1.0 - s.metallic) / k_pi);
  return diffuse + specular;
}

inline Dvec3 direct(const Surface& s, Dvec3 light_dir, Dvec3 radiance) {
  const double n_dot_l = dot(s.normal, light_dir);
  return eval(s, light_dir) * radiance * (n_dot_l > 0.0 ? n_dot_l : 0.0);
}

inline double distance_attenuation(double distance_sq, double radius) {
  const double r2 = radius * radius;
  const double ratio = clamp01(distance_sq / (r2 < 1e-8 ? 1e-8 : r2));
  const double window = 1.0 - ratio * ratio;
  return window * window / (distance_sq < 1e-8 ? 1e-8 : distance_sq);
}

inline double spot_attenuation(Dvec3 to_surface, Dvec3 direction, double cos_inner,
                               double cos_outer) {
  const double span = cos_inner - cos_outer;
  const double t = clamp01((dot(to_surface, direction) - cos_outer) / (span < 1e-4 ? 1e-4 : span));
  return t * t;
}

// The tangent frame of one triangle, from its own positions and UVs: the CPU mirror of
// `tangent_frame` in visibility_resolve.slang. Solves dp1 = T du1 + B dv1, dp2 = T du2 + B dv2,
// then Gram-Schmidt orthonormalizes against the shading normal. False when the UV area is
// degenerate, which is when the resolve leaves the normal alone.
inline bool tangent_frame(const Dvec3* p, const Vec2* uv, Dvec3 normal, Dvec3& tangent,
                          Dvec3& bitangent) {
  const Dvec3 edge1 = p[1] - p[0];
  const Dvec3 edge2 = p[2] - p[0];
  const double du1 = static_cast<double>(uv[1].x) - static_cast<double>(uv[0].x);
  const double dv1 = static_cast<double>(uv[1].y) - static_cast<double>(uv[0].y);
  const double du2 = static_cast<double>(uv[2].x) - static_cast<double>(uv[0].x);
  const double dv2 = static_cast<double>(uv[2].y) - static_cast<double>(uv[0].y);
  const double area = du1 * dv2 - du2 * dv1;
  if (std::fabs(area) < 1e-12) return false;
  const double r = 1.0 / area;
  Dvec3 t = (edge1 * dv2 - edge2 * dv1) * r;
  Dvec3 b = (edge2 * du1 - edge1 * du2) * r;
  t = t - normal * dot(normal, t);
  const double t_length = std::sqrt(dot(t, t));
  if (t_length < 1e-12) return false;
  tangent = t * (1.0 / t_length);
  b = b - (normal * dot(normal, b) + tangent * dot(tangent, b));
  const double b_length = std::sqrt(dot(b, b));
  if (b_length < 1e-12) return false;
  bitangent = b * (1.0 / b_length);
  return true;
}

// The shading normal a tangent-space normal map gives, from the three UNORM bytes a sampler
// returns: remap to -1..1, scale the tangential part, renormalize, rotate into the frame. The
// mirror of the normal-map block of fs_resolve; `normal_scale` of 0 gives `normal` back.
inline Dvec3 map_normal(Dvec3 normal, Dvec3 tangent, Dvec3 bitangent, const u8* texel,
                        double normal_scale) {
  Dvec3 n{static_cast<double>(texel[0]) / 255.0 * 2.0 - 1.0,
          static_cast<double>(texel[1]) / 255.0 * 2.0 - 1.0,
          static_cast<double>(texel[2]) / 255.0 * 2.0 - 1.0};
  n.x *= normal_scale;
  n.y *= normal_scale;
  const double length_sq = dot(n, n);
  if (length_sq < 1e-12) return normal;
  n = n * (1.0 / std::sqrt(length_sq));
  return normalize(tangent * n.x + bitangent * n.y + normal * n.z);
}

// The whole shaded branch of fs_resolve: sun, analytic lights, sky hemisphere (diffuse by normal
// elevation plus a Fresnel sliver so metals are not black), emissive. Linear, before the gamma.
// `sun_shadowed` and `Light::shadowed` are what the resolve's shadow rays found: a shadowed light
// contributes nothing, and nothing else about the surface changes.
inline Dvec3 shade(const Surface& s, Dvec3 sun_dir, double sun_intensity, Dvec3 sky,
                   const Light* lights, u32 light_count, Dvec3 emissive,
                   bool sun_shadowed = false) {
  Dvec3 color = sun_shadowed ? Dvec3{} : direct(s, sun_dir, splat(sun_intensity));
  for (u32 i = 0; i < light_count; ++i) {
    const Light& light = lights[i];
    if (light.shadowed) continue;
    const Dvec3 to_light = light.position - s.position;
    const double distance_sq = dot(to_light, to_light);
    double attenuation = distance_attenuation(distance_sq, light.radius);
    if (attenuation <= 0.0) continue;
    const Dvec3 light_dir = normalize(to_light);
    if (light.spot) {
      attenuation *= spot_attenuation(-light_dir, normalize(light.direction), light.cos_inner,
                                      light.cos_outer);
    }
    color = color + direct(s, light_dir, light.color * (light.intensity * attenuation));
  }
  const Dvec3 ambient = sky * (0.15 + 0.20 * (s.normal.y * 0.5 + 0.5));
  const double n_dot_v = dot(s.normal, s.view);
  const Dvec3 fresnel = f_schlick(f0_of(s.albedo, s.metallic), n_dot_v > 0.0 ? n_dot_v : 0.0);
  color = color + ambient * (s.albedo * (1.0 - s.metallic) + fresnel) + emissive;
  return color;
}

// Linear to the byte a UNORM target holds: the 1/2.2 display gamma, then round to 8 bits.
inline u8 display(double linear) {
  const double v = std::pow(linear < 0.0 ? 0.0 : linear, 1.0 / 2.2);
  return static_cast<u8>(std::lround((v > 1.0 ? 1.0 : v) * 255.0));
}

// The world point the resolve reconstructs at the center of pixel (px, py) on a horizontal plane:
// the camera ray through that pixel, intersected with the plane. For a planar surface this is
// exactly what perspective-correct barycentric interpolation of the triangle's vertices gives,
// which is what reconstruct() computes, so the reference shades the point the GPU shaded.
inline Dvec3 pixel_on_plane(Dvec3 eye, Dvec3 target, Dvec3 up, double fov_y, double aspect,
                            u32 width, u32 height, u32 px, u32 py, double plane_y) {
  const double t = 1.0 / std::tan(fov_y * 0.5);
  const Dvec3 f = normalize(target - eye);
  const Dvec3 right = normalize(cross(f, up));
  const Dvec3 camera_up = cross(right, f);
  const double ndc_x = 2.0 * (static_cast<double>(px) + 0.5) / static_cast<double>(width) - 1.0;
  const double ndc_y = 1.0 - 2.0 * (static_cast<double>(py) + 0.5) / static_cast<double>(height);
  const Dvec3 dir = f + right * (ndc_x * aspect / t) + camera_up * (ndc_y / t);
  return eye + dir * ((plane_y - eye.y) / dir.y);
}

}  // namespace engine::brdf_ref
