// The BSDF the material resolve and the reference path tracer share (docs/plan/04-renderer.md
// §4.3, "Materials"): Cook-Torrance with a GGX/Trowbridge-Reitz normal distribution, Smith
// height-correlated visibility (Heitz 2014, "Understanding the Masking-Shadowing Function in
// Microfacet-Based BRDFs"), Schlick's Fresnel with F0 = lerp(0.04, albedo, metallic), and a
// Lambert diffuse lobe that takes what the specular lobe does not reflect and what the metallic
// parameter does not remove. Everything is linear color, per steradian, and in world space.
//
// Nothing here declares a binding, a push constant, or an entry point, so a fragment, compute, or
// ray tracing stage includes it and passes its own surface in. Clearcoat, sheen, and transmission
// join it later the same way. domain/gfx/tests/brdf_reference.h evaluates these same formulas on
// the CPU in double precision; the shading and attributes tests compare the GPU against it.

#pragma once

static const float k_brdf_pi = 3.14159265358979323846;

// Below this the highlight of a light source is narrower than a pixel and D climbs past what a
// 32-bit float holds; clamping keeps it finite. Filament's value.
static const float k_min_roughness = 0.045;

struct BrdfSurface {
  float3 albedo;    // linear base color
  float3 normal;    // unit, out of the surface
  float3 view;      // unit, towards the eye
  float roughness;  // perceptual; alpha is its square
  float metallic;   // 0 dielectric, 1 conductor
};

// Perceptual roughness to the GGX width.
float brdf_alpha(float roughness) {
  const float r = clamp(roughness, k_min_roughness, 1.0);
  return r * r;
}

// Reflectance at normal incidence: 4% for dielectrics, the base color for conductors.
float3 brdf_f0(float3 albedo, float metallic) { return lerp(float3(0.04), albedo, metallic); }

// GGX / Trowbridge-Reitz normal distribution.
float brdf_d_ggx(float n_dot_h, float alpha) {
  const float a2 = alpha * alpha;
  const float d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
  return a2 / max(k_brdf_pi * d * d, 1e-9);
}

// Smith height-correlated visibility: G divided by the 4 n.l n.v of the microfacet denominator.
float brdf_v_smith(float n_dot_v, float n_dot_l, float alpha) {
  const float a2 = alpha * alpha;
  const float v = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
  const float l = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
  return 0.5 / max(v + l, 1e-9);
}

float3 brdf_f_schlick(float3 f0, float cos_theta) {
  const float m = clamp(1.0 - cos_theta, 0.0, 1.0);
  const float m2 = m * m;
  return f0 + (float3(1.0) - f0) * (m2 * m2 * m);
}

// The BSDF for one incident direction, without the cosine: diffuse plus specular, per steradian.
float3 brdf_eval(BrdfSurface s, float3 light_dir) {
  const float n_dot_l = dot(s.normal, light_dir);
  const float n_dot_v = dot(s.normal, s.view);
  if (n_dot_l <= 0.0 || n_dot_v <= 0.0) return float3(0.0);
  const float3 h = normalize(light_dir + s.view);
  const float n_dot_h = clamp(dot(s.normal, h), 0.0, 1.0);
  const float v_dot_h = clamp(dot(s.view, h), 0.0, 1.0);
  const float alpha = brdf_alpha(s.roughness);
  const float3 f = brdf_f_schlick(brdf_f0(s.albedo, s.metallic), v_dot_h);
  const float3 specular = f * (brdf_d_ggx(n_dot_h, alpha) * brdf_v_smith(n_dot_v, n_dot_l, alpha));
  const float3 diffuse = (float3(1.0) - f) * ((1.0 - s.metallic) / k_brdf_pi) * s.albedo;
  return diffuse + specular;
}

// Radiance leaving the surface towards the eye for one light of the given incident radiance.
float3 brdf_direct(BrdfSurface s, float3 light_dir, float3 radiance) {
  return brdf_eval(s, light_dir) * max(dot(s.normal, light_dir), 0.0) * radiance;
}

// Inverse square with Karis' window: the falloff reaches zero at the radius of influence with a
// zero derivative, so a light leaves no edge where its volume ends and culling it is exact.
float brdf_distance_attenuation(float distance_sq, float radius) {
  const float ratio = clamp(distance_sq / max(radius * radius, 1e-8), 0.0, 1.0);  // (d/r)^2
  const float window = 1.0 - ratio * ratio;                                       // 1 - (d/r)^4
  return window * window / max(distance_sq, 1e-8);
}

// Spot cone: 1 inside the inner angle, 0 outside the outer one, smooth between. `to_surface`
// points from the light towards the surface, like `direction`.
float brdf_spot_attenuation(float3 to_surface, float3 direction, float cos_inner,
                            float cos_outer) {
  const float t =
      clamp((dot(to_surface, direction) - cos_outer) / max(cos_inner - cos_outer, 1e-4), 0.0, 1.0);
  return t * t;
}
