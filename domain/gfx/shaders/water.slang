// The resolve's water branch (docs/subsystems/water.md, "Shading"; roadmap S63): a pixel of a
// material with `k_material_water` (gfx::set_water_material) is not shaded by the BSDF but by
// this, at the pixel, with no second pass — what the eye sees of a shallow pool is
//
//   - the sky's radiance along the view reflected off a wave-perturbed normal, by Fresnel (Schlick,
//     the water's index 1.33),
//   - the sun's glitter: the GGX lobe of the resolve's own BSDF (brdf.slang) on that normal at the
//     body's roughness, shadowed as the surface is,
//   - and the bottom: the body's shallow colour fading to its deep colour by the depth the mesh
//     carries under each vertex (UV.y), lit as a level surface by the sun and the sky's ambient,
//     darkened by the water's absorption along the slant of the refracted view,
//
// the reflection and the glitter faded in from the edge over `shore` (UV.x, the distance from the
// edge), so the very edge is the bottom alone: wet ground. The resolve applies the aerial
// perspective and the exposure after it, as to every surface.
//
// **Waves.** A sum of four directional sines over the body-local point (x, z) — the surface point
// less the instance's own place in the frame, so the waves are a function of where on the body a
// pixel is, never of the eye or a world coordinate (ADR-0053) — each a fixed fraction of the
// body's wavelength and amplitude, turned from the main direction by the chop, at a phase drawn
// from the body's seed and travelling at the deep-water speed sqrt(g k) **rounded to a whole
// number of cycles per `k_water_period_s`**, so the frame's clock may wrap at the period with no
// jump and a replay draws the same water (`ResolveParams::water_time_s`). Only the slope is
// evaluated (the mesh stays flat); a wave shorter than twice the pixel's footprint is faded out
// rather than aliased into sparkle, and every wave dies over the last quarter metre of depth.
//
// Mirrored in double by domain/gfx/tests/water_reference.h; change one and the other in the same
// commit, and say by how much the shading test still holds them together.
#ifndef ENGINE_WATER_SLANG
#define ENGINE_WATER_SLANG

#include "brdf.slang"
#include "material.slang"

static const float k_water_ior = 1.33;
// ((1.33 - 1) / (1.33 + 1))^2: what water reflects face on.
static const float k_water_f0 = 0.020059312;
static const float k_water_g = 9.81;
static const float k_water_period_s = 1024.0;  // gfx::k_water_period_s
static const float k_water_two_pi = 6.283185307179586;
// The depth over which the waves die at the shore, metres.
static const float k_water_calm_depth = 0.25;
// The four waves: wavelength and amplitude as shares of the body's, and the turn from the main
// direction as a share of the chop's ninety degrees.
static const float k_water_length[4] = {1.0, 0.62, 0.38, 0.24};
static const float k_water_amplitude[4] = {1.0, 0.55, 0.32, 0.18};
static const float k_water_turn[4] = {0.0, 0.55, -0.8, 0.35};

// A water material's numbers (gfx::set_water_material's packing).
struct WaterParams {
  float3 shallow;
  float3 deep;
  float roughness;
  float depth_fade;
  float amplitude;
  float wavelength;
  float chop;
  float shore;
  float2 direction;
  uint seed;
};

WaterParams water_of(ResolveMaterial m) {
  WaterParams w;
  w.shallow = m.albedo.rgb;
  w.roughness = m.albedo.w;
  w.deep = m.uv_transform.rgb;
  w.depth_fade = m.uv_transform.w;
  w.amplitude = m.uv_offset.x;
  w.wavelength = m.uv_offset.y;
  w.chop = m.uv_offset.z;
  w.shore = m.uv_offset.w;
  w.direction = float2(m.normal_scale, m.occlusion_strength);
  w.seed = m.sampler;
  return w;
}

// A wave's phase at the clock's zero, in [0, 1): an integer hash of the seed and the wave.
float water_phase(uint seed, uint wave) {
  uint h = seed * 0x9E3779B9u + (wave + 1u) * 0x85EBCA6Bu;
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  h ^= h >> 16;
  return float(h >> 8) * (1.0 / 16777216.0);
}

// The surface's normal at body-local (x, z) at the clock `time`, seconds in [0, period): the four
// waves' slopes, each faded by the pixel's `footprint` (metres) and all by the `depth`.
float3 water_normal(WaterParams w, float2 p, float time, float footprint, float depth) {
  float2 slope = float2(0.0, 0.0);
  const float calm = clamp(depth / k_water_calm_depth, 0.0, 1.0);
  for (uint i = 0; i < 4; ++i) {
    const float lambda = w.wavelength * k_water_length[i];
    const float a = w.amplitude * k_water_amplitude[i] * calm;
    const float turn = w.chop * k_water_turn[i] * (0.5 * 3.14159265358979);
    const float c = cos(turn);
    const float s = sin(turn);
    const float2 d = float2(w.direction.x * c - w.direction.y * s,
                            w.direction.x * s + w.direction.y * c);
    const float k = k_water_two_pi / lambda;
    // Whole cycles per period: the deep-water frequency sqrt(g k), rounded.
    const float cycles = floor(sqrt(k_water_g * k) * (k_water_period_s / k_water_two_pi) + 0.5);
    // The phase in cycles, each term reduced before it is added, so the angle cos sees is small.
    const float along = dot(d, p) / lambda;
    const float travel = frac(cycles * (time / k_water_period_s));
    const float cyc = frac(frac(along) - travel + water_phase(w.seed, i));
    // A wave shorter than twice the footprint is gone, and whole at four times it.
    const float keep = footprint > 0.0 ? clamp(lambda / (2.0 * footprint) - 1.0, 0.0, 1.0) : 1.0;
    slope += d * (a * k * keep * cos(k_water_two_pi * cyc));
  }
  return normalize(float3(-slope.x, 1.0, -slope.y));
}

// The radiance leaving the water towards the eye along `v` (unit, towards the eye), before the air
// between them: `n` the waves' normal, `uv` the mesh's (distance from the edge, depth), `sun_dir`
// towards the sun and `sun` its irradiance as it arrives (its shadow included; zero when it is
// down), `ambient` the light a level surface takes from the sky, `sky` the sky's radiance along
// the reflected view.
float3 water_radiance(WaterParams w, float3 n, float3 v, float2 uv, float3 sun_dir, float3 sun,
                      float3 ambient, float3 sky) {
  const float shore = w.shore > 0.0 ? clamp(uv.x / w.shore, 0.0, 1.0) : 1.0;
  const float n_dot_v = max(dot(n, v), k_min_n_dot_v);
  const float fresnel = brdf_f_schlick(float3(k_water_f0), n_dot_v).x;
  // The bottom, lit as a level surface, seen through the slant of the refracted view.
  const float3 body = lerp(w.shallow, w.deep, clamp(uv.y / w.depth_fade, 0.0, 1.0));
  const float3 lit = sun * (max(sun_dir.y, 0.0) / k_brdf_pi) + ambient;
  const float sin2_t = (1.0 - n_dot_v * n_dot_v) / (k_water_ior * k_water_ior);
  const float cos_t = sqrt(max(1.0 - sin2_t, 1e-4));
  const float slant = uv.y * (1.0 / cos_t - 1.0);
  const float absorb = exp(-slant * (3.0 / w.depth_fade));
  const float3 under = body * lit * absorb;
  // The sun's glitter: the BSDF's specular lobe at water's reflectance.
  float3 glitter = float3(0.0);
  const float n_dot_l = dot(n, sun_dir);
  if (n_dot_l > 0.0) {
    const float3 half_sum = sun_dir + v;
    const float3 h = dot(half_sum, half_sum) > 1e-12 ? normalize(half_sum) : n;
    const float alpha = brdf_alpha(w.roughness);
    const float n_dot_h = clamp(dot(n, h), 0.0, 1.0);
    const float v_dot_h = clamp(dot(v, h), 0.0, 1.0);
    const float f = brdf_f_schlick(float3(k_water_f0), v_dot_h).x;
    glitter = sun * (f * brdf_d_ggx(n_dot_h, alpha) * brdf_v_smith(n_dot_v, n_dot_l, alpha) *
                     n_dot_l);
  }
  return under * (1.0 - fresnel * shore) + (sky * fresnel + glitter) * shore;
}

// The direction the reflected view leaves the surface along, kept above the horizon: a steep wave
// that sends it down sends it into the water, which under the sky the mirror of the sky stands in.
float3 water_reflected(float3 n, float3 v) {
  float3 r = reflect(-v, n);
  r.y = abs(r.y);
  return normalize(r);
}

#endif  // ENGINE_WATER_SLANG
