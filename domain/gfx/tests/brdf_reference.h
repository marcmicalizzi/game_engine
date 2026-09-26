#pragma once

// The CPU mirror of domain/gfx/shaders/brdf.slang and of the shading block of
// visibility_resolve.slang, in double precision: the same Cook-Torrance BSDF (GGX, Smith
// height-correlated visibility, Schlick Fresnel), the same windowed inverse-square falloff, the
// same hemisphere of sky above and lit ground below and the ambient occlusion that weighs it, the
// same emissive term (the factor times the sRGB-decoded emissive texel), the same tangent frame
// and normal-map perturbation, and
// the same display gamma. The shading and attributes tests render a known surface on the GPU and
// compare the pixels against this, so the lighting model is pinned by a second implementation
// rather than by numbers nobody can rederive. Changing brdf.slang means changing this file in the
// same commit; the tests then say by how much the two disagree.

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

// brdf.slang's `brdf_env_specular`: the specular lobe's reflectance of a uniform environment in
// Karis' analytic fit, F0 * a + b.
inline Dvec3 env_specular(Dvec3 f0, double roughness, double n_dot_v) {
  const double r =
      roughness < k_min_roughness ? k_min_roughness : (roughness > 1.0 ? 1.0 : roughness);
  const double k[4] = {-1.0 * r + 1.0, -0.0275 * r + 0.0425, -0.572 * r + 1.04, 0.022 * r - 0.04};
  const double falloff = std::exp2(-9.28 * (n_dot_v > 0.0 ? n_dot_v : 0.0));
  const double a004 = (k[0] * k[0] < falloff ? k[0] * k[0] : falloff) * k[0] + k[1];
  const double a = -1.04 * a004 + k[2];
  const double b = 1.04 * a004 + k[3];
  const Dvec3 out = f0 * a + splat(b);
  return {out.x > 0.0 ? out.x : 0.0, out.y > 0.0 ? out.y : 0.0, out.z > 0.0 ? out.z : 0.0};
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

// An sRGB-encoded byte to linear light, as a GPU's sRGB view of a UNORM8 texel returns it: the
// IEC 61966-2-1 curve in double precision.
inline double srgb_to_linear(u8 code) {
  const double c = static_cast<double>(code) / 255.0;
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

// The emissive term of a material: its factor times the emissive texture's texel, which is sRGB
// colour and so is decoded to linear first. The mirror of `sample_material`'s emissive branch in
// material.slang; a material with no emissive texture is its factor alone.
inline Dvec3 emissive_of(Dvec3 factor, const u8* srgb_texel) {
  return factor * Dvec3{srgb_to_linear(srgb_texel[0]), srgb_to_linear(srgb_texel[1]),
                        srgb_to_linear(srgb_texel[2])};
}

// glTF's ambient occlusion from the map's red byte and the strength: 1 + strength * (r - 1).
inline double occlusion_of(u8 red, double strength) {
  return 1.0 + strength * (static_cast<double>(red) / 255.0 - 1.0);
}

// material.slang's `sky_radiance`: the sky's radiance, uniform above the horizon, 0.35 of its
// colour.
inline Dvec3 sky_radiance(Dvec3 sky) { return sky * 0.35; }

// brdf.slang's `brdf_ground_radiance` through material.slang's `ground_radiance`: a Lambertian
// ground of `ground_albedo` under the sun's irradiance on a horizontal surface and the sky's
// radiance. It takes the sun's intensity whether or not the shaded point is in the sun's shadow —
// it is the ground *around* the point — which is why `shade` below computes it before it asks.
inline Dvec3 ground_radiance(Dvec3 ground_albedo, Dvec3 sun_dir, double sun_intensity, Dvec3 sky) {
  const double horizontal = sun_intensity * (sun_dir.y > 0.0 ? sun_dir.y : 0.0);
  return ground_albedo * (splat(horizontal / k_pi) + sky_radiance(sky));
}

// brdf.slang's `brdf_hemisphere_ambient`: the sky above and the ground below, blended by the share
// of the normal's cosine lobe above the horizon, (1 + n.y) / 2.
inline Dvec3 hemisphere_ambient(Dvec3 sky, Dvec3 ground, double normal_y) {
  return lerp(ground, sky, clamp01(normal_y * 0.5 + 0.5));
}

// The whole shaded branch of fs_resolve: sun, analytic lights, the hemisphere (the sky above the
// horizon and the lit ground below it, `ground_radiance`, blended by the normal: diffuse, plus the
// specular lobe's share of it, `env_specular`, so metals are not black and rough dielectrics stay
// matte at grazing), emissive. Linear, before the gamma. `ground` is `ResolveParams::ground`, the
// ground's albedo. `sun_shadowed` and `Light::shadowed` are what the resolve's shadow rays found:
// a shadowed light contributes nothing, and nothing else about the surface changes — the ground's
// bounce of the sun included. `occlusion` is the material's ambient occlusion (`occlusion_of`),
// and it multiplies the hemisphere term alone — never a light, never the emission — which is the
// one place the resolve applies it.
inline Dvec3 shade(const Surface& s, Dvec3 sun_dir, double sun_intensity, Dvec3 sky, Dvec3 ground,
                   const Light* lights, u32 light_count, Dvec3 emissive, bool sun_shadowed = false,
                   double occlusion = 1.0) {
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
  const Dvec3 lit_ground = ground_radiance(ground, sun_dir, sun_intensity, sky);
  const Dvec3 ambient = hemisphere_ambient(sky_radiance(sky), lit_ground, s.normal.y);
  const double n_dot_v = dot(s.normal, s.view);
  const Dvec3 specular = env_specular(f0_of(s.albedo, s.metallic), s.roughness, n_dot_v);
  color = color + ambient * (s.albedo * (1.0 - s.metallic) + specular) * occlusion + emissive;
  return color;
}

// Linear to the byte a UNORM target holds: the 1/2.2 display gamma, then round to 8 bits.
inline u8 display(double linear) {
  const double v = std::pow(linear < 0.0 ? 0.0 : linear, 1.0 / 2.2);
  return static_cast<u8>(std::lround((v > 1.0 ? 1.0 : v) * 255.0));
}

// ---- the sampler's twins (domain/gfx/shaders/sampling.slang) ------------------------------
//
// The reference path tracer importance-samples the BSDF above, and a sampler is the half of a
// Monte Carlo integrator that is easiest to get subtly wrong: a density that does not integrate
// to one, or one that does not match the direction it claims to have produced, biases every
// picture by an amount no eye can name. These are the shader's routines in double precision, and
// `domain/gfx/tests/sampling_tests.cpp` holds them to the three properties that catch that — the
// density integrates to 1 over the hemisphere, sampling and evaluating and the pdf agree under
// Monte Carlo, and a white furnace comes back white.
//
// The generator is mirrored bit for bit rather than replaced by `<random>`, because it is the
// one part of the sampler whose *exact* values a capture's determinism depends on.

inline u32 pcg_hash(u32 v) {
  const u32 state = v * 747796405u + 2891336453u;
  const u32 word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}

inline u32 rng_seed(u32 pixel_x, u32 pixel_y, u32 sample_index, u32 seed) {
  const u32 mixed = pcg_hash(pixel_x * 73856093u ^ pixel_y * 19349663u ^ seed * 83492791u);
  return pcg_hash(mixed + sample_index * 2654435761u);
}

inline double rng_next(u32& state) {
  state = state * 747796405u + 2891336453u;
  u32 word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  word = (word >> 22u) ^ word;
  return static_cast<double>(word) * (1.0 / 4294967296.0);
}

// Duff et al. 2017, branchless.
inline void onb_from_normal(Dvec3 n, Dvec3& tangent, Dvec3& bitangent) {
  const double s = n.z >= 0.0 ? 1.0 : -1.0;
  const double a = -1.0 / (s + n.z);
  const double b = n.x * n.y * a;
  tangent = Dvec3{1.0 + s * n.x * n.x * a, s * b, -s * n.x};
  bitangent = Dvec3{b, s + n.y * n.y * a, -n.y};
}

inline Dvec3 to_local(Dvec3 v, Dvec3 tangent, Dvec3 bitangent, Dvec3 normal) {
  return Dvec3{dot(v, tangent), dot(v, bitangent), dot(v, normal)};
}

inline Dvec3 to_world(Dvec3 v, Dvec3 tangent, Dvec3 bitangent, Dvec3 normal) {
  return tangent * v.x + bitangent * v.y + normal * v.z;
}

inline double luminance(Dvec3 c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; }

inline Dvec3 sample_cosine_hemisphere(double u1, double u2) {
  const double r = std::sqrt(u1);
  const double phi = 2.0 * k_pi * u2;
  return Dvec3{r * std::cos(phi), r * std::sin(phi), std::sqrt(u1 < 1.0 ? 1.0 - u1 : 0.0)};
}

inline double cosine_hemisphere_pdf(double n_dot_l) {
  return (n_dot_l > 0.0 ? n_dot_l : 0.0) / k_pi;
}

// Smith's one-directional masking term; the visible-normal density is written in it.
inline double ggx_g1(double n_dot_v, double alpha) {
  const double a2 = alpha * alpha;
  const double denom = n_dot_v + std::sqrt(a2 + (1.0 - a2) * n_dot_v * n_dot_v);
  return 2.0 * n_dot_v / (denom < 1e-9 ? 1e-9 : denom);
}

// Heitz 2018. Local frame, z up, `v` away from the surface.
inline Dvec3 sample_ggx_vndf(Dvec3 v, double alpha, double u1, double u2) {
  const Dvec3 vh = normalize(Dvec3{alpha * v.x, alpha * v.y, v.z});
  const double len_sq = vh.x * vh.x + vh.y * vh.y;
  const Dvec3 t1 =
      len_sq > 0.0 ? Dvec3{-vh.y, vh.x, 0.0} * (1.0 / std::sqrt(len_sq)) : Dvec3{1.0, 0.0, 0.0};
  const Dvec3 t2 = cross(vh, t1);
  const double r = std::sqrt(u1);
  const double phi = 2.0 * k_pi * u2;
  const double p1 = r * std::cos(phi);
  double p2 = r * std::sin(phi);
  const double s = 0.5 * (1.0 + vh.z);
  const double one_minus = 1.0 - p1 * p1;
  p2 = (1.0 - s) * std::sqrt(one_minus > 0.0 ? one_minus : 0.0) + s * p2;
  const double z = 1.0 - p1 * p1 - p2 * p2;
  const Dvec3 nh = t1 * p1 + t2 * p2 + vh * std::sqrt(z > 0.0 ? z : 0.0);
  return normalize(Dvec3{alpha * nh.x, alpha * nh.y, nh.z > 0.0 ? nh.z : 0.0});
}

inline double ggx_vndf_pdf(Dvec3 v, Dvec3 l, double alpha) {
  if (v.z <= 0.0 || l.z <= 0.0) return 0.0;
  const Dvec3 h = normalize(v + l);
  if (dot(v, h) <= 0.0) return 0.0;
  const double denom = 4.0 * v.z;
  return ggx_g1(v.z, alpha) * d_ggx(h.z, alpha) / (denom < 1e-9 ? 1e-9 : denom);
}

inline double specular_probability(Dvec3 albedo, double metallic, double n_dot_v, Dvec3 fresnel) {
  (void)n_dot_v;
  const double specular = luminance(fresnel);
  const double diffuse = (1.0 - metallic) * luminance(albedo) * (1.0 - specular);
  const double total = specular + diffuse;
  if (total <= 1e-6) return 0.5;
  const double p = specular / total;
  return p < 0.05 ? 0.05 : (p > 0.95 ? 0.95 : p);
}

inline double bsdf_pdf(Dvec3 v, Dvec3 l, double alpha, double p_specular) {
  return p_specular * ggx_vndf_pdf(v, l, alpha) + (1.0 - p_specular) * cosine_hemisphere_pdf(l.z);
}

inline double mis_power_heuristic(double pdf_a, double pdf_b) {
  const double a = pdf_a * pdf_a;
  const double b = pdf_b * pdf_b;
  const double total = a + b;
  return total > 0.0 ? a / total : 0.0;
}

// One sample of the two-lobe mixture the path tracer draws from: the direction in the local
// frame, the BSDF times the cosine, and the mixture density. Returns false where the sample is
// below the surface, which is what the shader treats as the end of the path.
struct BsdfSample {
  Dvec3 direction;  // local frame, z up
  Dvec3 weight;     // f * cos / pdf: what the path's throughput is multiplied by
  double pdf = 0.0;
};

inline bool sample_bsdf(const Surface& s, Dvec3 v_local, double u_lobe, double u1, double u2,
                        BsdfSample& out) {
  const double n_dot_v = v_local.z;
  if (n_dot_v <= 0.0) return false;
  const double alpha = alpha_of(s.roughness);
  const Dvec3 fresnel = f_schlick(f0_of(s.albedo, s.metallic), n_dot_v);
  const double p_specular = specular_probability(s.albedo, s.metallic, n_dot_v, fresnel);
  Dvec3 l_local;
  if (u_lobe < p_specular) {
    const Dvec3 h = sample_ggx_vndf(v_local, alpha, u1, u2);
    l_local = h * (2.0 * dot(v_local, h)) - v_local;  // reflect(-v, h)
  } else {
    l_local = sample_cosine_hemisphere(u1, u2);
  }
  if (l_local.z <= 0.0) return false;
  out.pdf = bsdf_pdf(v_local, l_local, alpha, p_specular);
  if (!(out.pdf > 0.0)) return false;
  out.direction = l_local;
  // The BSDF is evaluated in world terms, but the surface here is its own local frame: the
  // normal is z and the view is `v_local`, so eval() can be called with the local vectors.
  Surface local = s;
  local.normal = Dvec3{0.0, 0.0, 1.0};
  local.view = v_local;
  out.weight = eval(local, l_local) * (l_local.z / out.pdf);
  return true;
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
