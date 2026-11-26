// The BSDF the material resolve and the reference path tracer share (docs/plan/04-renderer.md
// §4.3, "Materials"): Cook-Torrance with a GGX/Trowbridge-Reitz normal distribution, Smith
// height-correlated visibility (Heitz 2014, "Understanding the Masking-Shadowing Function in
// Microfacet-Based BRDFs"), Schlick's Fresnel with F0 = lerp(0.04, albedo, metallic) — or the
// dielectric's own F0 and F90 for a material that declares KHR_materials_ior or _specular (R234,
// below) — and a Lambert diffuse lobe that takes what the specular lobe does not reflect and what
// the metallic parameter does not remove. Everything is linear color, per steradian, and in world space.
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

// n.v is clamped to this rather than rejected (`brdf_eval`). Filament's MIN_N_DOT_V.
static const float k_min_n_dot_v = 1e-4;

struct BrdfSurface {
  float3 albedo;    // linear base color
  float3 normal;    // unit, out of the surface
  float3 view;      // unit, towards the eye
  float roughness;  // perceptual; alpha is its square
  float metallic;   // 0 dielectric, 1 conductor
  // The dielectric's own reflectance (R234; `gfx::ResolveMaterial::specular`, "a dielectric's own
  // reflectance" below): rgb its F0 less 0.04, w one less its F90. Zero, which every material that
  // declares neither KHR_materials_ior nor KHR_materials_specular holds, is the 4 % and the 1 every
  // material drew before, and takes the lines it always took.
  float4 specular;
};

// Perceptual roughness to the GGX width.
float brdf_alpha(float roughness) {
  const float r = clamp(roughness, k_min_roughness, 1.0);
  return r * r;
}

// R97: isotropic width fit, not an exact convolution of GGX (whose slope variance diverges).
// Toksvig's mean-normal shortening is measured before BC5; normal strength scales its variance.
// The final roughness feeds both D and Smith visibility through brdf_alpha. See normal-filtering.md.
float brdf_normal_variance(float shortening, float normal_scale) {
  const float loss = saturate(shortening);
  return normal_scale * normal_scale * loss / max(1.0 - loss, 1e-4);
}

float brdf_filter_roughness(float roughness, float normal_variance, float geometric_alpha2) {
  const float widening = 2.0 * max(normal_variance, 0.0) + max(geometric_alpha2, 0.0);
  if (widening == 0.0) return roughness;  // absent/flat data retains the legacy value exactly
  const float alpha = brdf_alpha(roughness);
  return sqrt(sqrt(min(alpha * alpha + widening, 1.0)));
}

// Tokuyoshi/Kaplanyan 2021, eq. (13): projected-space conservative isotropic kernel.
// Derivatives must describe this triangle's unperturbed shading normal, never adjacent ids.
float brdf_geometric_alpha2(float3 dn_dx, float3 dn_dy) {
  return min((dot(dn_dx, dn_dx) + dot(dn_dy, dn_dy)) / k_brdf_pi, 0.18);
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

// ---- a dielectric's own reflectance (R234; KHR_materials_ior, KHR_materials_specular) ----------
//
// docs/subsystems/gfx.md, "A dielectric's own reflectance", has the two specifications' formulas
// and why the record carries them as it does. The material's record holds the dielectric's F0 and
// F90 as the extensions define them — F0 = min(((ior - 1) / (ior + 1))^2 * specularColor, 1) *
// specular and F90 = specular — less the stand-in's, so a record of zeros is the 4 % and the 1 of
// every material before (`gfx::set_dielectric_specular` packs it). A metal's are the base colour and
// 1 whatever the record says; the metallic parameter blends the two as it always blended 0.04 with
// the base colour. The diffuse lobe gives up the largest channel of the dielectric's Fresnel, the
// specification's `max_value`, so a coloured coating does not tint what it lets through.
//
// **A material that declares neither takes the lines it took**: every function below branches on
// `brdf_declares_specular` to the expression it replaced, verbatim, rather than evaluating the
// general one with the stand-in's numbers, so the compiler sees the same arithmetic and a picture
// without the extensions is the bytes it was (an F90 of exactly 1 fed through a multiply-add is
// not always the same float as the constant it replaced, once a driver fuses it).

bool brdf_declares_specular(float4 specular) { return any(specular != float4(0.0)); }

// The dielectric's F0, per channel, and the surface's F0 and F90 with the metallic blend.
float3 brdf_dielectric_f0(float4 specular) { return float3(0.04) + specular.rgb; }
float3 brdf_surface_f0(BrdfSurface s) {
  if (!brdf_declares_specular(s.specular)) return brdf_f0(s.albedo, s.metallic);
  return lerp(brdf_dielectric_f0(s.specular), s.albedo, s.metallic);
}
float brdf_surface_f90(BrdfSurface s) { return 1.0 - s.specular.w * (1.0 - s.metallic); }

// Schlick's Fresnel towards a reflectance at grazing of `f90` rather than 1.
float3 brdf_f_schlick_f90(float3 f0, float f90, float cos_theta) {
  const float m = clamp(1.0 - cos_theta, 0.0, 1.0);
  const float m2 = m * m;
  return f0 + (float3(f90) - f0) * (m2 * m2 * m);
}

// The surface's Fresnel at `cos_theta`.
float3 brdf_fresnel(BrdfSurface s, float cos_theta) {
  if (!brdf_declares_specular(s.specular))
    return brdf_f_schlick(brdf_f0(s.albedo, s.metallic), cos_theta);
  return brdf_f_schlick_f90(brdf_surface_f0(s), brdf_surface_f90(s), cos_theta);
}

// The largest channel of the dielectric's F0: what its Fresnel's `max_value` is taken from (the
// Fresnel rises with F0 in every channel alike, so its largest channel is the largest F0's).
float brdf_dielectric_f0_max(float4 specular) {
  const float3 f0 = brdf_dielectric_f0(specular);
  return max(f0.x, max(f0.y, f0.z));
}

// What the diffuse lobe gives up at `cos_theta`, `f` the surface's Fresnel there: `f` itself for a
// material that declares neither, and for one that does the Fresnel of its dielectric's largest
// channel, blended with the base colour by the metallic parameter as F0 is.
float3 brdf_diffuse_fresnel(BrdfSurface s, float3 f, float cos_theta) {
  if (!brdf_declares_specular(s.specular)) return f;
  return brdf_f_schlick_f90(lerp(float3(brdf_dielectric_f0_max(s.specular)), s.albedo, s.metallic),
                            brdf_surface_f90(s), cos_theta);
}

// The diffuse lobe's normal-incidence share for a foliage sheet's transmitted and sphere's light,
// which takes 1 - F0 (below): the stand-in's F0, or the dielectric's largest channel blended.
float3 brdf_diffuse_f0(BrdfSurface s) {
  if (!brdf_declares_specular(s.specular)) return brdf_f0(s.albedo, s.metallic);
  return lerp(float3(brdf_dielectric_f0_max(s.specular)), s.albedo, s.metallic);
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
float2 brdf_env_fit(float roughness, float n_dot_v) {
  const float r = clamp(roughness, k_min_roughness, 1.0);
  const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
  const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
  const float4 k = c0 * r + c1;
  const float a004 = min(k.x * k.x, exp2(-9.28 * max(n_dot_v, 0.0))) * k.x + k.y;
  return float2(-1.04, 1.04) * a004 + k.zw;
}
float3 brdf_env_specular(float3 f0, float roughness, float n_dot_v) {
  const float2 ab = brdf_env_fit(roughness, n_dot_v);
  return max(f0 * ab.x + ab.y, float3(0.0));
}

// The same for a surface (R234): the split sum is F0 times the scale plus F90 times the bias
// (Karis 2013), and a material that declares neither extension takes the line above.
float3 brdf_env_specular_of(BrdfSurface s, float n_dot_v) {
  if (!brdf_declares_specular(s.specular))
    return brdf_env_specular(brdf_f0(s.albedo, s.metallic), s.roughness, n_dot_v);
  const float2 ab = brdf_env_fit(s.roughness, n_dot_v);
  return max(brdf_surface_f0(s) * ab.x + brdf_surface_f90(s) * ab.y, float3(0.0));
}

// The albedo the hemisphere's diffuse term takes (R234). The stand-in's hemisphere takes the whole
// albedo and no Fresnel off it, and a material that declares neither keeps exactly that; one that
// declares a reflectance gives up what its dielectric reflects of a uniform environment beyond what
// the stand-in's 4 % reflects (the largest channel, as `brdf_diffuse_fresnel` takes it), so a
// coating of 30 % does not add its reflection to a diffuse term that kept everything, and one that
// reflects less than 4 % keeps the albedo it always had.
float3 brdf_env_albedo(BrdfSurface s, float n_dot_v) {
  if (!brdf_declares_specular(s.specular)) return s.albedo;
  const float2 ab = brdf_env_fit(s.roughness, n_dot_v);
  const float own = brdf_dielectric_f0_max(s.specular) * ab.x + (1.0 - s.specular.w) * ab.y;
  const float stand_in = 0.04 * ab.x + ab.y;
  return s.albedo * clamp(1.0 - (max(own, 0.0) - max(stand_in, 0.0)), 0.0, 1.0);
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
//
// **n.v is clamped, not rejected** (Filament's `clampNoV`). A shading normal is an interpolation,
// or a normal map's texel, and not the surface: wherever it leans away from the eye while the
// triangle under it faces the eye — a grazing view across a crest whose smooth vertex normals lean
// over the far side, a normal map's texel turned away — n.v is negative for a point the camera
// plainly sees. Until 2026-09-25 this returned nothing there, so such a pixel lost every light and
// kept only the hemisphere ambient: a flat, constant grey with hard edges where n.v crosses zero
// inside a triangle, which is all the "grey seams" on the dunes of the third owner session were
// (docs/experiments/third-interactive-session-2026-09-25.md). Clamped, the diffuse lobe takes the
// light by n.l alone and the specular lobe's visibility term stays finite. Only a light behind the
// shading normal (n.l <= 0) contributes nothing.
float3 brdf_eval(BrdfSurface s, float3 light_dir) {
  const float n_dot_l = dot(s.normal, light_dir);
  if (n_dot_l <= 0.0) return float3(0.0);
  const float n_dot_v = max(dot(s.normal, s.view), k_min_n_dot_v);
  // With n.v allowed below zero the light can stand exactly opposite the eye, where l + v is zero
  // and the half vector undefined; the normal stands in for it there rather than a NaN.
  const float3 half_sum = light_dir + s.view;
  const float3 h = dot(half_sum, half_sum) > 1e-12 ? normalize(half_sum) : s.normal;
  const float n_dot_h = clamp(dot(s.normal, h), 0.0, 1.0);
  const float v_dot_h = clamp(dot(s.view, h), 0.0, 1.0);
  const float alpha = brdf_alpha(s.roughness);
  const float3 f = brdf_fresnel(s, v_dot_h);
  const float3 specular = f * (brdf_d_ggx(n_dot_h, alpha) * brdf_v_smith(n_dot_v, n_dot_l, alpha));
  const float3 diffuse =
      (float3(1.0) - brdf_diffuse_fresnel(s, f, v_dot_h)) * ((1.0 - s.metallic) / k_brdf_pi) * s.albedo;
  return diffuse + specular;
}

// Radiance leaving the surface towards the eye for one light of the given incident radiance.
float3 brdf_direct(BrdfSurface s, float3 light_dir, float3 radiance) {
  return brdf_eval(s, light_dir) * max(dot(s.normal, light_dir), 0.0) * radiance;
}

// ---- foliage: a two-sided material lit from both sides (R164) ----------------------------------
//
// docs/experiments/foliage-two-sided-light.md has the sources and the derivation. A material that
// declares foliage optics (gfx::set_foliage_optics; the flags word's top byte) is shaded by
// `brdf_foliage_direct` in place of `brdf_direct`, with two numbers:
//
// - `transmit`, the diffuse transmittance as a share of the diffuse reflectance (a leaf's two
//   spectra have one shape in the visible, so one colour, the albedo, serves both): light arriving
//   on the far side of the sheet comes through as Lambertian transmission (the bi-Lambertian leaf
//   of canopy radiative transfer, Ross 1981).
// - `spread`, the share of the surface whose normals are a sphere's rather than the sheet's: a
//   card of needles is opaque cylinders all round a twig, so what it shows the eye is the visible
//   half of a cloud of normals, whatever way the card's plane faces. That share answers the light
//   by the Lambert sphere's law: averaged over the normals the eye sees (weighted by n.v), the
//   cosine to the light is (2/3) phi(alpha), alpha the angle between the light and the eye and
//   phi(a) = (sin a + (pi - a) cos a) / pi, Lambert's phase law; the transmitted half takes
//   phi(pi - alpha). Backward scattering, as a shoot's is (Rautiainen et al. 2018).
//
// The sheet's share keeps `brdf_eval` whole (its specular lobe included); the sphere's is diffuse
// alone, through (1 - F0). With both numbers zero the result is `brdf_direct`'s, to the bit.
//
// **Reflected plus transmitted never passes one** (R164-3): the transmittance is `transmit` times
// the albedo but at most what the albedo leaves, 1 - albedo, channel by channel
// (`brdf_foliage_share`). A declared share is a property of the material and the albedo a texel's,
// so no import or pack can hold the pair under one for every texel; the response holds it here,
// and a leaf whose two sum to less (every flora leaf, every glTF the importer splits) is untouched.

// The transmittance's share of the albedo a foliage material takes, per channel: `transmit`, or
// less where transmit * albedo would pass 1 - albedo.
float3 brdf_foliage_share(float3 albedo, float transmit) {
  return min(float3(transmit), max(float3(1.0) - albedo, float3(0.0)) / max(albedo, float3(1e-6)));
}

// Lambert's phase law, normalized to one at zero phase: the light a Lambertian sphere returns
// towards an eye at `cos_phase` (the cosine of the angle between the light and the eye) over what
// it returns at zero phase.
float brdf_lambert_phase(float cos_phase) {
  const float c = clamp(cos_phase, -1.0, 1.0);
  const float a = acos(c);
  return max((sqrt(max(1.0 - c * c, 0.0)) + (k_brdf_pi - a) * c) / k_brdf_pi, 0.0);
}

// Whether a light from `light_dir` can reach a foliage surface's response at all: the sheet's
// front, its back through `transmit`, or the sphere's share from anywhere but straight behind.
bool brdf_foliage_lights(BrdfSurface s, float3 light_dir, float transmit, float spread) {
  const float n_dot_l = dot(s.normal, light_dir);
  return n_dot_l > 0.0 || (transmit > 0.0 && n_dot_l < 0.0) || spread > 0.0;
}

// The BSDF times the cosine for one incident direction, foliage's: what `brdf_direct` is with a
// radiance of one. The path tracer weighs a sampled direction by it. (Slang resolves a module's
// functions in any order; `brdf_foliage_diffuse_cos` is below.)
float3 brdf_foliage_eval_cos(BrdfSurface s, float3 light_dir, float transmit, float spread) {
  const float n_dot_l = dot(s.normal, light_dir);
  float3 sheet = float3(0.0);
  if (n_dot_l > 0.0) sheet = brdf_eval(s, light_dir) * n_dot_l;
  return (1.0 - spread) * sheet + brdf_foliage_diffuse_cos(s, light_dir, transmit, spread);
}

// What foliage adds to (1 - spread) of the sheet's `brdf_eval` times the cosine: the diffuse
// transmitted through the sheet's share and the sphere's share. The resolve takes the sheet's part
// from `brdf_direct`, which every pixel evaluates, and adds this, so a foliage pixel runs one GGX
// lobe and not a second copy of it (R164-2).
float3 brdf_foliage_diffuse_cos(BrdfSurface s, float3 light_dir, float transmit, float spread) {
  const float n_dot_l = dot(s.normal, light_dir);
  const float3 diffuse = (float3(1.0) - brdf_diffuse_f0(s)) *
                         ((1.0 - s.metallic) / k_brdf_pi) * s.albedo;
  const float3 share = brdf_foliage_share(s.albedo, transmit);  // R164-3: never past 1 - albedo
  const float3 back = share * max(-n_dot_l, 0.0);
  // phi(a) + share phi(pi - a) with one acos and one root (R164-2): sin(pi - a) = sin a and
  // cos(pi - a) = -cos a, so phi(pi - a) = (sin a - a cos a) / pi; `brdf_lambert_phase` is the
  // same law, one direction at a time.
  float3 sphere = float3(0.0);
  if (spread > 0.0) {
    const float c = clamp(dot(light_dir, s.view), -1.0, 1.0);
    const float a = acos(c);
    const float sin_a = sqrt(max(1.0 - c * c, 0.0));
    sphere = (2.0 / (3.0 * k_brdf_pi)) *
             (max(sin_a + (k_brdf_pi - a) * c, 0.0) + share * max(sin_a - a * c, 0.0));
  }
  return diffuse * ((1.0 - spread) * back + spread * sphere);
}

float3 brdf_foliage_direct(BrdfSurface s, float3 light_dir, float3 radiance, float transmit,
                           float spread) {
  return brdf_foliage_eval_cos(s, light_dir, transmit, spread) * radiance;
}

// The hemisphere ambient's diffuse weights for foliage: what multiplies the albedo, given the
// ambient the sheet's normal sees (`front`), the one its back sees (`back`), and the ones along the
// view and against it (`towards_eye`, `away_from_eye`). For an ambient linear in the normal (the
// sky's first two orders of harmonics, the stand-in's two halves), the n.v-weighted mean over the
// visible half of a sphere of normals is (5/6) A(v) + (1/6) A(-v), and its back's the reverse.
// `transmit` is the share per channel, `brdf_foliage_share` of the albedo the weights multiply.
float3 brdf_foliage_ambient(float3 front, float3 back, float3 towards_eye, float3 away_from_eye,
                            float3 transmit, float spread) {
  const float3 sheet = front + transmit * back;
  const float3 sphere = (5.0 / 6.0 + transmit / 6.0) * towards_eye +
                        (1.0 / 6.0 + transmit * (5.0 / 6.0)) * away_from_eye;
  return (1.0 - spread) * sheet + spread * sphere;
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
