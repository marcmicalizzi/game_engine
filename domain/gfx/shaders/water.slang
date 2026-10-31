// The resolve's water branch (docs/subsystems/water.md, "Shading"; roadmap S63): a pixel of a
// material with `k_material_water` (gfx::set_water_material) is not shaded by the BSDF but by
// this, at the pixel, with no second pass — what the eye sees of a shallow pool is
//
//   - the sky's radiance along the view reflected off a wave-perturbed normal, by Fresnel (Schlick,
//     the water's index 1.33),
//   - the sun's glitter: the GGX lobe of the resolve's own BSDF (brdf.slang) on that normal at the
//     body's roughness, shadowed as the surface is,
//   - and the bottom: the body's shallow colour fading to its deep colour by the depth under the
//     pixel — sampled from the body's depth map (below), or the mesh's vertex depth (UV.y) for a
//     body with none — lit as a level surface by the sun and the sky's ambient,
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

// **Open water** (gfx::k_material_water_open; docs/subsystems/water.md, "To the horizon"): the
// body's outline is the near shore of a sea, and the resolve draws the same water past it as a
// plane at the level. Its sides that face the sea are not a shore, so an open body's shore fades
// by **depth**, over the depth the waves calm in, rather than by the distance from the outline:
// `water_edge` is what `water_radiance`'s uv.x (and the volume's) is given — the mesh's distance
// from the outline for a closed body, unchanged, and for an open one `shore` times the share of
// `k_water_calm_depth` the water has, so the fade is whole where the waves are.
static const uint k_material_water_open = 128u;  // gfx::k_material_water_open
// The depth the plane takes where nothing is under it (past the depth map's square, a pixel with
// no surface behind the sea): the bottom gone.
static const float k_water_open_deep = 1000.0;

float water_edge(WaterParams w, uint flags, float edge, float depth) {
  if ((flags & k_material_water_open) == 0) return edge;
  return w.shore * clamp(depth / k_water_calm_depth, 0.0, 1.0);
}

// **An open sea's waves from a spectrum** (gfx::k_material_water_spectrum, water_spectrum.h;
// docs/subsystems/water.md, "The spectrum"): in place of the four sines, the sum of
// `k_water_waves` Gerstner waves drawn from JONSWAP at the scene's wind and the body's fetch with
// Donelan-Banner spreading, made on the CPU at load and packed raw into the five material entries
// after the water material's next one. Each wave's phase is reduced to a fraction of a cycle term by
// term as the four sines' is, its frequency a whole number of cycles a period, its share faded by
// the footprint and the shore's calm alike; the Gerstner crest sharpens the normal by its
// steepness (Q k a). The silhouette stays flat: only the normal is evaluated.
static const uint k_material_water_spectrum = 256u;  // gfx::k_material_water_spectrum
static const uint k_water_waves = 16;  // gfx::k_water_waves; the entries are scene.slang's 112 bytes

// Mirrors gfx::WaterWave (32 bytes).
struct WaterWave {
  float2 direction;
  float k;
  float amplitude;
  float cycles;
  float phase;
  float steepness;
  float pad;
};

// The table of the water material at `index` of the table at `materials` (an address).
WaterWave* water_waves(uint64_t materials, uint index) {
  return (WaterWave*)(materials + uint64_t(index + 2u) * uint64_t(k_resolve_material_bytes));
}

float3 water_normal_spectrum(WaterWave* waves, float2 p, float time, float footprint,
                             float depth) {
  float2 slope = float2(0.0, 0.0);
  float lift = 0.0;
  const float calm = clamp(depth / k_water_calm_depth, 0.0, 1.0);
  for (uint i = 0; i < k_water_waves; ++i) {
    const WaterWave wave = waves[i];
    const float lambda = k_water_two_pi / wave.k;
    const float along = dot(wave.direction, p) / lambda;
    const float travel = frac(wave.cycles * (time / k_water_period_s));
    const float cyc = frac(frac(along) - travel + wave.phase);
    const float keep = footprint > 0.0 ? clamp(lambda / (2.0 * footprint) - 1.0, 0.0, 1.0) : 1.0;
    const float share = calm * keep;
    const float angle = k_water_two_pi * cyc;
    slope += wave.direction * (wave.amplitude * wave.k * share * cos(angle));
    lift += wave.steepness * share * sin(angle);
  }
  return normalize(float3(-slope.x, 1.0 - lift, -slope.y));
}

// The waves' normal for a water material: the spectrum's where it has one, the four sines'
// otherwise.
float3 water_normal_of(WaterParams w, uint flags, uint64_t materials, uint index, float2 p,
                       float time, float footprint, float depth) {
  if ((flags & k_material_water_spectrum) != 0)
    return water_normal_spectrum(water_waves(materials, index), p, time, footprint, depth);
  return water_normal(w, p, time, footprint, depth);
}

// The direction the reflected view leaves the surface along, kept above the horizon: a steep wave
// that sends it down sends it into the water, which under the sky the mirror of the sky stands in.
float3 water_reflected(float3 n, float3 v) {
  float3 r = reflect(-v, n);
  r.y = abs(r.y);
  return normalize(r);
}

// ---- the volume tier (RenderSettings::water_quality "high"; water.md, "Translucent by volume") ---
//
// A water material with `k_material_water_volume` is followed in the table by its **optics**
// (gfx::set_water_optics): the water's extinction and scattering per metre, its transmission, and
// the ground under it — the material the terrain draws with and where the body stands in that
// material's UV frame. The visibility buffer holds the surface and nothing beneath it, so the
// bottom is found analytically: the view refracted at the wave normal is marched down to the
// ground the body's depth map holds (`water_path_map`), or, for a body with no map, run to the
// plane the mesh's depth attribute says the ground is (the triangle's own depth and slope), the ground's
// colour is read there from the terrain's own maps, lit by the sun refracted into the water
// (shadowed where its ray leaves the surface) and the sky's light, and dimmed by Beer–Lambert
// along both paths; the water itself adds the light it scatters towards the eye on the way, which
// is what the deep tends to. Without the flag the branch is `water_radiance` above, unchanged.
static const uint k_material_water_volume = 32u;  // gfx::k_material_water_volume
static const uint k_water_no_ground = 0xffffffffu;
// The sky's light comes down through the water along paths 1.25 times the depth on average (a
// diffuse light's mean cosine of 0.8 under a level surface).
static const float k_water_diffuse_path = 1.25;
static const float k_water_four_pi = 12.566370614359172;

struct WaterOptics {
  float3 extinction;  // absorption plus scattering, per metre
  float transmission;  // the share of the bottom's light that reaches the surface beyond that
  float3 scattering;  // per metre, the turbidity's sediment included
  float2 ground_uv;   // the ground material's UV at the body's centre
  float ground_uv_per_m;
  uint ground;  // the ground's material index, or k_water_no_ground
};

WaterOptics water_optics_of(ResolveMaterial m) {
  WaterOptics o;
  o.extinction = m.albedo.rgb;
  o.transmission = m.albedo.w;
  o.scattering = m.emissive.rgb;
  o.ground_uv = m.uv_transform.xy;
  o.ground_uv_per_m = m.uv_transform.z;
  o.ground = m.sampler;
  return o;
}

// The refracted view under the surface: its direction (unit, downwards), the metres it runs to the
// bottom, and the bottom's depth where it lands.
struct WaterPath {
  float3 t;
  float length;
  float depth;
};

// `v` towards the eye, `n` the waves' normal, `depth` the bottom's depth under this point and
// `slope` its change per metre of body-local (x, z). Snell at 1.33 on the wave normal, written out
// rather than `refract` so the clamp on n.v is the Fresnel term's. The bottom is the plane through
// the depth with the triangle's slope, depth(x) = depth + slope.(x - here); the ray's own depth
// after s metres is s * down, so it lands at s = depth / (down - slope.t_xz) — kept no longer than
// four times the flat answer, where the bottom falls away about as fast as the ray does.
WaterPath water_path(float3 n, float3 v, float depth, float2 slope) {
  const float cos_i = max(dot(n, v), k_min_n_dot_v);
  const float eta = 1.0 / k_water_ior;
  const float cos_t = sqrt(max(1.0 - eta * eta * (1.0 - cos_i * cos_i), 0.0));
  WaterPath p;
  p.t = normalize(-v * eta + n * (eta * cos_i - cos_t));
  const float down = max(-p.t.y, 1e-3);
  const float den = max(down - dot(slope, p.t.xz), 0.25 * down);
  p.length = max(depth, 0.0) / den;
  p.depth = p.length * down;
  return p;
}

// ---- the depth map (gfx::set_water_depth_map; water.md, "The depth map") ---------------------
//
// With `k_material_water_depth_map` the entry after the water material names the body's depth map:
// the ground under the body on a square grid at a quarter of the mesh's spacing, a 16-bit depth
// in an RGBA8 image's red (high byte) and green (low byte). It is sampled bilinearly at the
// body-local point — the UV clamped half a texel inside the map, which is clamp-to-edge whatever
// the sampler's address mode — so the bottom the branch shades is the ground, not the water
// mesh's rings and fan of vertex depths, whose facets the eye saw through the surface.
static const uint k_material_water_depth_map = 64u;  // gfx::k_material_water_depth_map

struct WaterDepthMap {
  uint texture;
  uint sampler;
  float2 uv_origin;  // the map's UV at the body's centre
  float uv_per_m;
  float half_texel;  // in UV
  float depth_max;   // the depth a texel of 65535 is
};

WaterDepthMap water_depth_map_of(ResolveMaterial m) {
  WaterDepthMap d;
  d.texture = m.albedo_texture;
  d.sampler = m.samplers_mr_normal;
  d.uv_origin = m.uv_offset.xy;
  d.uv_per_m = m.uv_offset.z;
  d.depth_max = m.uv_offset.w;
  d.half_texel = m.uv_transform.w;
  return d;
}

// The depth at body-local (x, z): red and green are the 16-bit depth's two bytes, and the depth is
// linear in both, so the filtered pair is the filtered depth.
float water_depth_sample(WaterDepthMap d, float2 p) {
  const float2 uv = clamp(d.uv_origin + p * d.uv_per_m, d.half_texel, 1.0 - d.half_texel);
  const float4 t = g_textures[NonUniformResourceIndex(d.texture)]
                       .SampleLevel(g_samplers[NonUniformResourceIndex(d.sampler)], uv, 0);
  return (t.r * (65280.0 / 65535.0) + t.g * (255.0 / 65535.0)) * d.depth_max;
}

// The refracted view run down to the ground as a **height field**: `k_water_march_steps` fixed
// steps along the ray out to `k_water_march_reach` times the flat answer (the depth here over the
// ray's descent), the first step that passes under the sampled ground bracketing the hit, and one
// secant between that step's two ends. A level bottom lands exactly (the secant of a straight
// line); a bank that rises under the ray is met sooner and a bottom that falls away later, as the
// ground goes, never as the triangle under the pixel says. A ray that is still above the ground at
// the end of its reach stops there — the cap the plane had. `bottom_normal` is the map's slope at
// the hit, one texel's forward differences: the ground's normal there, (d', 1, d') normalized.
static const uint k_water_march_steps = 6;
static const float k_water_march_reach = 3.0;

WaterPath water_path_map(WaterDepthMap d, float2 here, float3 n, float3 v, float depth,
                         out float3 bottom_normal) {
  const float cos_i = max(dot(n, v), k_min_n_dot_v);
  const float eta = 1.0 / k_water_ior;
  const float cos_t = sqrt(max(1.0 - eta * eta * (1.0 - cos_i * cos_i), 0.0));
  WaterPath p;
  p.t = normalize(-v * eta + n * (eta * cos_i - cos_t));
  const float down = max(-p.t.y, 1e-3);
  const float step = k_water_march_reach * max(depth, 0.0) / down / float(k_water_march_steps);
  // f(s) = ground depth less the ray's depth: positive while the ray is in the water.
  float s_a = 0.0;
  float f_a = max(depth, 0.0);
  float s = step * float(k_water_march_steps);
  for (uint i = 1; i <= k_water_march_steps; ++i) {
    const float s_b = step * float(i);
    const float f_b = water_depth_sample(d, here + p.t.xz * s_b) - s_b * down;
    if (f_b <= 0.0) {
      s = f_a - f_b > 1e-9 ? s_a + (s_b - s_a) * (f_a / (f_a - f_b)) : s_b;
      break;
    }
    s_a = s_b;
    f_a = f_b;
  }
  p.length = s;
  p.depth = s * down;
  const float2 at = here + p.t.xz * s;
  const float texel_m = 2.0 * d.half_texel / d.uv_per_m;
  const float d0 = water_depth_sample(d, at);
  const float dx = water_depth_sample(d, at + float2(texel_m, 0.0)) - d0;
  const float dz = water_depth_sample(d, at + float2(0.0, texel_m)) - d0;
  bottom_normal = normalize(float3(dx / texel_m, 1.0, dz / texel_m));
  return p;
}

// (1 - e^(-a s)) / a, which is s where a is nothing: what a segment of length s through a medium
// of extinction a gathers of a source along it.
float water_gather(float a, float s) { return a > 1e-5 ? (1.0 - exp(-a * s)) / a : s; }
float3 water_gather3(float3 a, float s) {
  return float3(water_gather(a.x, s), water_gather(a.y, s), water_gather(a.z, s));
}

// The sun's direction under a level surface, towards it, and the cosine it makes with the vertical
// (unit: its horizontal part is the sun's over 1.33, Snell's law, and the rest is `cos_l`).
float3 water_sun_under(float3 sun_dir, out float cos_l) {
  const float up = max(sun_dir.y, 0.0);
  const float eta = 1.0 / k_water_ior;
  cos_l = sqrt(max(1.0 - eta * eta * (1.0 - up * up), 1e-4));
  return float3(sun_dir.x * eta, cos_l, sun_dir.z * eta);
}

// The radiance coming up out of the water along the refracted view, before the surface's Fresnel
// split: the bottom (`albedo`, its normal `n_b`) lit by the sun through the water — `sun` the
// irradiance arriving at the surface, `sun_bottom` the share of it its shadow lets through where
// the ray to the bottom leaves the surface — and by the sky's light `ambient` (a level surface's,
// as the opaque path takes it), both dimmed on the way down, then dimmed again on the way up and
// passed by the transmission; plus the light the water scatters towards the eye along the path,
// isotropically, from the sun's beam (`sun_surface`, its shadow at the pixel) and the sky's light
// as they come down. Deep water is the second term alone: a colour the light makes, not one painted.
float3 water_volume(WaterOptics o, WaterPath p, float3 n_b, float3 albedo, float3 sun_dir,
                    float3 sun, float sun_surface, float sun_bottom, float3 ambient) {
  float cos_l;
  const float3 l = water_sun_under(sun_dir, cos_l);
  // What the surface lets into the water: Schlick at the sun's angle, and at normal incidence for
  // the sky's light.
  const float3 sun_in = sun * (1.0 - brdf_f_schlick(float3(k_water_f0), max(sun_dir.y, 0.0)).x);
  const float3 sky_in = ambient * (1.0 - k_water_f0);
  const float3 down_sun = exp(-o.extinction * (p.depth / cos_l));
  const float3 down_sky = exp(-o.extinction * (p.depth * k_water_diffuse_path));
  const float3 lit = sun_in * (sun_bottom * max(dot(n_b, l), 0.0) / k_brdf_pi) * down_sun +
                     sky_in * down_sky;
  const float3 bottom = albedo * lit * exp(-o.extinction * p.length) * o.transmission;
  // The view path from the surface (s = 0) to the bottom: at s the water is s * mu deep, the sun's
  // beam has come s * mu / cos_l through it and the sky's light s * mu * 1.25, and what is
  // scattered there comes back s through it. Isotropic: the sun's beam scatters a 4 pi-th of
  // itself per steradian, and the sky's light — `ambient` is a level surface's radiance, pi times
  // it its irradiance — a quarter of itself.
  const float mu = p.length > 0.0 ? p.depth / p.length : 1.0;
  const float3 a_sun = o.extinction * (1.0 + mu / cos_l);
  const float3 a_sky = o.extinction * (1.0 + mu * k_water_diffuse_path);
  const float3 scattered = o.scattering * (sun_in * (sun_surface / k_water_four_pi) *
                                               water_gather3(a_sun, p.length) +
                                           sky_in * 0.25 * water_gather3(a_sky, p.length));
  return bottom + scattered;
}

// `water_radiance` with the volume's light in place of the painted bottom: the same Fresnel, the
// same glitter, the same shore fade.
float3 water_radiance_volume(WaterParams w, float3 n, float3 v, float2 uv, float3 sun_dir,
                             float3 sun, float3 sky, float3 under) {
  const float shore = w.shore > 0.0 ? clamp(uv.x / w.shore, 0.0, 1.0) : 1.0;
  const float n_dot_v = max(dot(n, v), k_min_n_dot_v);
  const float fresnel = brdf_f_schlick(float3(k_water_f0), n_dot_v).x;
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

// The ground's colour at `uv` of its material: the base colour times its map, at the level the
// UV's derivatives select where the map is the content build's mipmapped one.
float3 water_ground_albedo(ResolveMaterial g, float2 uv, float2 duv_dx, float2 duv_dy) {
  float3 albedo = g.albedo.rgb;
  if (g.albedo_texture == k_no_texture) return albedo;
  const float2 at = material_uv(g, uv * g.uv_scale);
  if ((g.flags & k_material_mipped) != 0) {
    albedo *= g_textures[NonUniformResourceIndex(g.albedo_texture)]
                  .SampleGrad(g_samplers[NonUniformResourceIndex(g.sampler)], at,
                              material_duv(g, duv_dx * g.uv_scale),
                              material_duv(g, duv_dy * g.uv_scale))
                  .rgb;
  } else {
    albedo *= g_textures[NonUniformResourceIndex(g.albedo_texture)]
                  .SampleLevel(g_samplers[NonUniformResourceIndex(g.sampler)], at, 0)
                  .rgb;
  }
  return albedo;
}

#endif  // ENGINE_WATER_SLANG
