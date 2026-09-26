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

// How much of a uniform environment the specular lobe reflects towards the eye: the GGX lobe
// integrated over the hemisphere, which is the second factor of the split-sum approximation
// (Karis 2013), in the analytic fit Karis gave for mobile ("Physically Based Shading on Mobile",
// 2014) — F0 times a scale plus a bias, both functions of roughness and n.v. Face on it is about
// F0 at any roughness; towards grazing it climbs to one for a mirror and stays within a few
// hundredths of F0 for a rough surface, whose lobe spreads over directions that mostly miss the
// eye. The resolve's sky term is weighted by it (visibility_resolve.slang). Until 2026-09-25 that
// term used bare Schlick at n.v instead, which is the mirror's curve at every roughness: a rough
// stone wall seen edge-on, or any texel of a normal map turned away from the camera (n.v clamped
// to zero), reflected the whole sky as a white-blue sheen, which is the "reflective instead of
// matte" of the second owner session (docs/experiments/second-interactive-session-2026-09-25.md).
float3 brdf_env_specular(float3 f0, float roughness, float n_dot_v) {
  const float r = clamp(roughness, k_min_roughness, 1.0);
  const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
  const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
  const float4 k = c0 * r + c1;
  const float a004 = min(k.x * k.x, exp2(-9.28 * max(n_dot_v, 0.0))) * k.x + k.y;
  const float2 ab = float2(-1.04, 1.04) * a004 + k.zw;
  return max(f0 * ab.x + ab.y, float3(0.0));
}

// ---- the environment: the sky above the horizon, the ground below it ---------------------------
//
// The resolve's indirect term sees an environment of two halves, the sky above the horizon and the
// ground below it (material.slang, `sky_radiance` and `ground_radiance`). Until 2026-09-25 there
// was no ground: the sky's own ramp ran on past the horizon (0.15 of the sky colour straight down),
// so a surface turned towards the ground saw a dim blue sky under it
// (docs/experiments/third-interactive-session-2026-09-25.md, whose dunes asked for this).

// The radiance of the ground: a Lambertian plane of albedo `ground_albedo` lit by the sun and the
// sky, rho * (E_sun / pi + L_sky). E_sun is the sun's irradiance on a horizontal surface,
// `sun_intensity * max(sun_dir.y, 0)`, which the Lambert lobe turns into radiance by dividing by
// pi; `sky` is the sky's radiance, which is also the ambient an up-facing surface takes. So the
// ground is what the resolve itself draws for a lit horizontal patch of that albedo, less its
// Fresnel and its specular lobe. **No shadow**: it stands for the ground *around* the point, which
// the sun lights whether or not the point itself is in shade — that is the whole reason the term
// exists — and a shadowed pixel therefore keeps it (the resolve's sun-shadowed pixels are the model
// with the sun's direct term removed, the ground's bounce of it included).
float3 brdf_ground_radiance(float3 ground_albedo, float3 sun_dir, float sun_intensity,
                            float3 sky) {
  return ground_albedo * (sun_intensity * max(sun_dir.y, 0.0) / k_brdf_pi + sky);
}

// The hemisphere ambient: the mean radiance, weighted by the cosine, that a surface of normal n
// receives from an environment that is `sky` above the horizon and `ground` below it, each uniform.
// The share of the cosine lobe above the horizon is exactly (1 + n.y) / 2 — the sky view factor of
// a plane tilted by acos(n.y) — so the blend is exact for two uniform halves: a face turned up sees
// the sky alone, a vertical one half of each, and one turned straight down the ground alone.
float3 brdf_hemisphere_ambient(float3 sky, float3 ground, float normal_y) {
  return lerp(ground, sky, clamp(normal_y * 0.5 + 0.5, 0.0, 1.0));
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
