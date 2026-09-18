// Importance sampling for the reference path tracer (docs/plan/04-renderer.md §4.8): the random
// number stream, the two lobes the BSDF of `brdf.slang` is sampled with, their densities, and the
// multiple-importance-sampling weight. No bindings and no entry points, like `brdf.slang`, so a
// CPU twin can mirror it: `domain/gfx/tests/brdf_reference.h` carries these same routines in
// double precision and the sampling tests hold the pair to the three properties that matter — a
// density that integrates to one, a sample/eval/pdf triple that agrees under Monte Carlo, and a
// white furnace that comes back white.
//
// What is here and what is next door. `brdf.slang` holds the *evaluation*: D, the
// height-correlated Smith **visibility** (G over the 4 n.l n.v of the denominator), Fresnel. A
// sampler needs a different member of the same family — the one-directional masking term G1,
// which the visible-normal density is written in — and nothing in the resolve wants it, so it
// lives here rather than widening the file every raster shader includes.
//
// The generator is a PCG hash (O'Neill 2014) over (pixel, sample index, seed) rather than a
// counter carried between dispatches: a pixel's Nth sample is the same sequence whichever
// dispatch produces it, so a capture split into ten dispatches of ten samples is the same image
// as one dispatch of a hundred, and a re-run with the same seed is the same bytes.

#ifndef ENGINE_SAMPLING_SLANG
#define ENGINE_SAMPLING_SLANG

#include "brdf.slang"

// ---- random numbers -------------------------------------------------------------------------

uint pcg_hash(uint v) {
  const uint state = v * 747796405u + 2891336453u;
  const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}

// The stream for one sample of one pixel. Two rounds so that neighbouring pixels and adjacent
// sample indices do not share low bits, which shows as a visible correlation pattern otherwise.
uint rng_seed(uint2 pixel, uint sample_index, uint seed) {
  const uint mixed = pcg_hash(pixel.x * 73856093u ^ pixel.y * 19349663u ^ seed * 83492791u);
  return pcg_hash(mixed + sample_index * 2654435761u);
}

float rng_next(inout uint state) {
  state = state * 747796405u + 2891336453u;
  uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  word = (word >> 22u) ^ word;
  return float(word) * (1.0 / 4294967296.0);
}

float2 rng_next2(inout uint state) {
  const float x = rng_next(state);
  const float y = rng_next(state);
  return float2(x, y);
}

// ---- frames ---------------------------------------------------------------------------------

// A branchless orthonormal basis around a unit vector (Duff, Burgess, Christensen, Hery,
// Kensler, Liani, Villemin 2017). Handedness is arbitrary and unused: nothing here is
// anisotropic, so any frame around the normal gives the same distribution.
void onb_from_normal(float3 n, out float3 tangent, out float3 bitangent) {
  const float s = n.z >= 0.0 ? 1.0 : -1.0;
  const float a = -1.0 / (s + n.z);
  const float b = n.x * n.y * a;
  tangent = float3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
  bitangent = float3(b, s + n.y * n.y * a, -n.y);
}

float3 to_local(float3 v, float3 tangent, float3 bitangent, float3 normal) {
  return float3(dot(v, tangent), dot(v, bitangent), dot(v, normal));
}

float3 to_world(float3 v, float3 tangent, float3 bitangent, float3 normal) {
  return tangent * v.x + bitangent * v.y + normal * v.z;
}

float sampling_luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// ---- the diffuse lobe -----------------------------------------------------------------------

// Malley's method: a point in the unit disc lifted onto the hemisphere. Local frame, z up.
float3 sample_cosine_hemisphere(float2 u) {
  const float r = sqrt(u.x);
  const float phi = 2.0 * k_brdf_pi * u.y;
  return float3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u.x)));
}

float cosine_hemisphere_pdf(float n_dot_l) { return max(n_dot_l, 0.0) / k_brdf_pi; }

// ---- the specular lobe ----------------------------------------------------------------------

// Smith's one-directional masking term for GGX. `brdf.slang` carries the height-correlated
// *visibility* the evaluation uses; the visible-normal density is written in this one.
float ggx_g1(float n_dot_v, float alpha) {
  const float a2 = alpha * alpha;
  const float denom = n_dot_v + sqrt(a2 + (1.0 - a2) * n_dot_v * n_dot_v);
  return 2.0 * n_dot_v / max(denom, 1e-9);
}

// A half vector from the distribution of **visible** normals (Heitz 2018, "Sampling the GGX
// Distribution of Visible Normals"): stretch the view into the hemispherical configuration,
// sample the projected area of the visible hemisphere as a disc, unstretch. Local frame, z up,
// `v` pointing away from the surface. Sampling visible normals rather than the whole
// distribution is what keeps the estimator's variance finite at grazing angles, where half the
// ordinary D-sampled normals are masked and thrown away.
float3 sample_ggx_vndf(float3 v, float alpha, float2 u) {
  const float3 vh = normalize(float3(alpha * v.x, alpha * v.y, v.z));
  const float len_sq = vh.x * vh.x + vh.y * vh.y;
  const float3 t1 =
      len_sq > 0.0 ? float3(-vh.y, vh.x, 0.0) * rsqrt(len_sq) : float3(1.0, 0.0, 0.0);
  const float3 t2 = cross(vh, t1);
  const float r = sqrt(u.x);
  const float phi = 2.0 * k_brdf_pi * u.y;
  const float p1 = r * cos(phi);
  float p2 = r * sin(phi);
  const float s = 0.5 * (1.0 + vh.z);
  p2 = (1.0 - s) * sqrt(max(0.0, 1.0 - p1 * p1)) + s * p2;
  const float3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
  return normalize(float3(alpha * nh.x, alpha * nh.y, max(0.0, nh.z)));
}

// Density of a **direction** produced by reflecting `v` about a VNDF half vector, per steradian.
// D_v(h) = G1(v) max(0, v.h) D(h) / v.z, and the reflection's Jacobian is 1 / (4 v.h), so the
// v.h cancels and what is left is G1 D / (4 v.z). Zero where the pair cannot have been produced.
float ggx_vndf_pdf(float3 v, float3 l, float alpha) {
  if (v.z <= 0.0 || l.z <= 0.0) return 0.0;
  const float3 h = normalize(v + l);
  if (dot(v, h) <= 0.0) return 0.0;
  return ggx_g1(v.z, alpha) * brdf_d_ggx(h.z, alpha) / max(4.0 * v.z, 1e-9);
}

// ---- the lobe pick --------------------------------------------------------------------------

// How often the specular lobe is sampled: the Fresnel term at the view angle against what the
// diffuse lobe has left to reflect, which is the split the BSDF itself makes. Clamped away from
// 0 and 1 so that a lobe which is improbable but not impossible still gets samples — the pdf
// below accounts for the clamp exactly, so the estimator stays unbiased whatever the value is.
float specular_probability(float3 albedo, float metallic, float n_dot_v, float3 fresnel) {
  const float specular = sampling_luminance(fresnel);
  const float diffuse = (1.0 - metallic) * sampling_luminance(albedo) * (1.0 - specular);
  const float total = specular + diffuse;
  if (total <= 1e-6) return 0.5;
  return clamp(specular / total, 0.05, 0.95);
}

// The density of a direction under the two-lobe mixture, which is what the estimator divides by:
// one sample of a mixture is weighted by the *mixture's* density, never by the density of the
// lobe that happened to produce it.
float bsdf_pdf(float3 v, float3 l, float alpha, float p_specular) {
  return p_specular * ggx_vndf_pdf(v, l, alpha) +
         (1.0 - p_specular) * cosine_hemisphere_pdf(l.z);
}

// ---- multiple importance sampling -------------------------------------------------------------

// Veach's power heuristic with beta = 2. Every light the engine has today is a delta light, so
// nothing a BSDF ray can hit is also sampled directly and every weight below is 1; the function
// and its CPU twin are here, tested, for the area and environment sampling that will change that.
float mis_power_heuristic(float pdf_a, float pdf_b) {
  const float a = pdf_a * pdf_a;
  const float b = pdf_b * pdf_b;
  const float total = a + b;
  return total > 0.0 ? a / total : 0.0;
}

#endif  // ENGINE_SAMPLING_SLANG
