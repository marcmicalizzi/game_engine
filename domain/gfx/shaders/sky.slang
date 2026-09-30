// The sky (docs/subsystems/gfx.md, "The sky"; domain/gfx/sky.h says why these tables): a physical
// atmosphere — Rayleigh and Mie scattering and ozone absorption over a spherical planet — lit by a
// sun and a moon, after Hillaire's "A Scalable and Production Ready Sky and Atmosphere Rendering
// Technique" (EGSR 2020), with the sun's disc, the moon's lit disc and a table of stars over it.
//
// Nothing here declares a binding, a push constant or an entry point, as `brdf.slang` does not: the
// tables are buffers read through the addresses in `SkyParams`, so the resolve, the reference path
// tracer and the tables' own passes (sky_luts.slang) include this file and pass their own block in.
// domain/gfx/tests/sky_reference.h is the same arithmetic in double precision on the CPU, and the
// sky test holds the GPU to it.
//
// **Where the numbers come from.** The air's coefficients, the lights' directions and strengths, the
// celestial frame and the star table are the frame's (`SkyParams`, filled by the renderer from the
// scene's sky provider); what is here is only what to do with them. Kilometres inside the air, metres
// in the world, radiance in sun units (sky.h).

#ifndef ENGINE_SKY_SLANG
#define ENGINE_SKY_SLANG

static const float k_sky_pi = 3.14159265358979323846;

// Mirrors gfx::k_sky_* in sky.h.
static const uint k_sky_transmittance_width = 256;
static const uint k_sky_transmittance_height = 64;
static const uint k_sky_multiscatter_size = 32;
static const uint k_sky_view_width = 192;
static const uint k_sky_view_height = 108;
static const uint k_sky_aerial_width = 64;
static const uint k_sky_aerial_height = 32;
static const uint k_sky_aerial_depth = 32;
static const float k_sky_aerial_distance_km = 32.0;
static const float k_sky_min_altitude_km = 0.01;
static const uint k_sky_star_face_cells = 32;
static const float k_sky_star_max_sigma = 1.745329e-3;
static const uint k_sky_moon = 1u;
static const uint k_sky_stars = 2u;
static const uint k_sky_sun_disc = 4u;
static const uint k_sky_max_views = 8;

// Mirrors gfx::SkyView (80 bytes).
struct SkyView {
  column_major float4x4 inv_view_proj;
  float4 pixel;  // x radians a pixel spans at the view's centre
};

// Mirrors gfx::SkyParams (1008 bytes).
struct SkyParams {
  float4 rayleigh;          // rgb scattering /km at the ground, w scale height km
  float4 mie_scattering;    // rgb, w scale height
  float4 mie_extinction;    // rgb, w asymmetry g
  float4 ozone;             // rgb absorption at the peak, w the peak's height
  float4 radii;             // x planet, y top of the air, z ozone half width
  float4 ground_albedo;     // rgb: the region's, as the air sees it
  float4 local_albedo;      // rgb: the ground round the point, the ambient's lower half
  float4 night;             // rgb night-sky glow, sun units / sr
  float4 sun;               // xyz towards it, w angular radius
  float4 sun_illuminance;   // rgb outside the air
  float4 moon;              // xyz, w angular radius
  float4 moon_illuminance;  // rgb outside the air, w albedo
  float4 camera;            // xyz eye (m), w altitude in the tables (km)
  float4 celestial_x;
  float4 celestial_y;
  float4 celestial_z;
  float4 exposure;          // x mode, y EV100 or compensation, z knee lux, w slope
  float4 stars;             // x count, y scale
  uint64_t transmittance;
  uint64_t multiscatter;
  uint64_t sky_view;
  uint64_t aerial;
  uint64_t frame;
  uint64_t star_list;
  uint64_t star_cells;
  uint64_t star_index;
  uint flags;
  uint view_count;
  float shoulder;  // where the display's shoulder starts (sky_tone); 1 is a clip
  uint pad;
  SkyView views[8];
};

// Mirrors gfx::SkyFrame (208 bytes).
struct SkyFrame {
  float4 sh[9];
  float4 ground;
  float4 sun_ground;
  float4 moon_ground;
  float4 exposure;  // x display per sun unit, y EV100, z metered lux, w the sky's share (lux)
};

// ---- the air -----------------------------------------------------------------------------------

// Scattering and extinction at a height above the ground (km): Rayleigh and Mie falling off
// exponentially, and ozone in a tent `radii.z` either side of its peak, absorbing and not scattering.
// Below the ground (a surface under the world's y = 0) the air is the ground's.
struct SkyMedium {
  float3 rayleigh;    // scattering
  float3 mie;         // scattering
  float3 extinction;  // everything: Rayleigh's, Mie's own extinction, ozone's absorption
};

SkyMedium sky_medium(SkyParams* sky, float height) {
  const float h = max(height, 0.0);
  SkyMedium m;
  const float dr = exp(-h / sky->rayleigh.w);
  const float dm = exp(-h / sky->mie_scattering.w);
  const float dozone = max(0.0, 1.0 - abs(h - sky->ozone.w) / sky->radii.z);
  m.rayleigh = sky->rayleigh.rgb * dr;
  m.mie = sky->mie_scattering.rgb * dm;
  m.extinction = m.rayleigh + sky->mie_extinction.rgb * dm + sky->ozone.rgb * dozone;
  return m;
}

// Rayleigh's phase function, and Cornette and Shanks' for Mie (Henyey-Greenstein with the Rayleigh
// term that makes it tend to Rayleigh's as g goes to zero), of the cosine between the view ray and
// the direction towards the light: both peak looking at the light.
float sky_phase_rayleigh(float nu) { return 3.0 / (16.0 * k_sky_pi) * (1.0 + nu * nu); }

float sky_phase_mie(float nu, float g) {
  const float g2 = g * g;
  const float k = 3.0 / (8.0 * k_sky_pi) * (1.0 - g2) / (2.0 + g2);
  const float denom = 1.0 + g2 - 2.0 * g * nu;
  return k * (1.0 + nu * nu) / (denom * sqrt(denom));
}

// ---- rays in the air, by radius and zenith cosine ----------------------------------------------
//
// A ray is its start's radius `r` (km from the planet's centre), the start's height above the ground
// `h` (the same thing less the planet's radius, carried separately because a float radius of 6,360
// km has a step of half a metre and every quantity below that matters is a difference of two such
// radii), and the cosine `mu` of its zenith angle there. Bruneton's formulation.

// The height at distance t along the ray: r(t) - R written so that no two large numbers are
// subtracted, (h0 (2R + h0) + t (t + 2 r0 mu)) / (r(t) + R).
float sky_height_along(float bottom, float r0, float h0, float mu, float t) {
  const float r2_less = h0 * (2.0 * bottom + h0) + t * (t + 2.0 * r0 * mu);  // r(t)^2 - R^2
  const float r = sqrt(max(bottom * bottom + r2_less, 0.0));
  return r2_less / (r + bottom);
}

// Whether the ray meets the ground: it points down and the discriminant r^2 mu^2 - (r^2 - R^2) of
// its meeting the planet's sphere is not negative.
bool sky_hits_ground(float bottom, float r, float h, float mu) {
  return mu < 0.0 && r * r * mu * mu >= h * (2.0 * bottom + h);
}

// The nearer root of t^2 + 2 r mu t + (r^2 - R^2) = 0 as the product of the roots over the farther,
// (r^2 - R^2) / (-r mu + sqrt(disc)): the textbook -r mu - sqrt(disc) subtracts two numbers of
// 6,000 km to get ten metres, which is a ray from the eye to the ground at its feet with a float's
// half-metre step in its length.
float sky_distance_to_ground(float bottom, float r, float h, float mu) {
  const float c = h * (2.0 * bottom + h);
  const float disc = r * r * mu * mu - c;
  const float far = -r * mu + sqrt(max(disc, 0.0));
  return far > 0.0 ? max(c, 0.0) / far : 0.0;
}

// The farther root of t^2 + 2 r mu t + (r^2 - top^2) = 0, which is positive from inside the air: for
// a ray that climbs, (top^2 - r^2) / (r mu + sqrt(disc)), which subtracts nothing.
float sky_distance_to_top(float bottom, float top, float r, float h, float mu) {
  // r^2 (mu^2 - 1) + top^2 = r^2 mu^2 + (top^2 - R^2) - (r^2 - R^2)
  const float room = (top - bottom) * (top + bottom) - h * (2.0 * bottom + h);  // top^2 - r^2
  const float root = sqrt(max(r * r * mu * mu + room, 0.0));
  if (mu > 0.0) return max(room, 0.0) / max(r * mu + root, 1e-9);
  return max(0.0, -r * mu + root);
}

// ---- the tables' texels ------------------------------------------------------------------------

// A unit range and the coordinate that puts 0 and 1 on the first and the last texel's centre.
float sky_tex_from_unit(float x, float n) { return 0.5 / n + x * (1.0 - 1.0 / n); }
float sky_unit_from_tex(float u, float n) { return (u - 0.5 / n) / (1.0 - 1.0 / n); }

// Bilinear through an address: texel (i, j) is at ((i + 0.5) / width, (j + 0.5) / height), and a
// coordinate past the first or the last texel's centre reads that texel. `wrap_x` wraps the first
// axis instead (an azimuth).
float4 sky_bilinear(uint64_t table, uint width, uint height, float2 uv, bool wrap_x) {
  float4* texels = (float4*)table;
  const float px = uv.x * float(width) - 0.5;
  const float py = uv.y * float(height) - 0.5;
  const float fx = floor(px);
  const float fy = floor(py);
  const float wx = px - fx;
  const float wy = py - fy;
  int x0 = int(fx);
  int x1 = x0 + 1;
  const int w = int(width);
  if (wrap_x) {
    // The coordinate is in [0, 1], so a neighbour is at most one texel past either end. Not `%`: a
    // remainder of a negative int is what SPIR-V's two remainders disagree about, and the one this
    // compiles to read column 63 for column -1.
    x0 = x0 < 0 ? x0 + w : (x0 >= w ? x0 - w : x0);
    x1 = x1 < 0 ? x1 + w : (x1 >= w ? x1 - w : x1);
  } else {
    x0 = clamp(x0, 0, w - 1);
    x1 = clamp(x1, 0, w - 1);
  }
  const int y0 = clamp(int(fy), 0, int(height) - 1);
  const int y1 = clamp(int(fy) + 1, 0, int(height) - 1);
  const float4 a = texels[y0 * w + x0];
  const float4 b = texels[y0 * w + x1];
  const float4 c = texels[y1 * w + x0];
  const float4 d = texels[y1 * w + x1];
  return lerp(lerp(a, b, wx), lerp(c, d, wx), wy);
}

// ---- the transmittance table (Bruneton's parameterization) -------------------------------------

float2 sky_transmittance_uv(SkyParams* sky, float r, float h, float mu) {
  const float bottom = sky->radii.x;
  const float top = sky->radii.y;
  const float big_h = sqrt((top - bottom) * (top + bottom));
  const float rho = sqrt(max(h * (2.0 * bottom + h), 0.0));
  const float d = sky_distance_to_top(bottom, top, r, h, mu);
  const float d_min = top - r;
  const float d_max = rho + big_h;
  const float x_mu = (d - d_min) / max(d_max - d_min, 1e-6);
  const float x_r = rho / big_h;
  return float2(sky_tex_from_unit(saturate(x_mu), float(k_sky_transmittance_width)),
                sky_tex_from_unit(saturate(x_r), float(k_sky_transmittance_height)));
}

// The inverse: the ray a texel stands for.
void sky_transmittance_ray(SkyParams* sky, float2 uv, out float r, out float h, out float mu) {
  const float bottom = sky->radii.x;
  const float top = sky->radii.y;
  const float x_mu = sky_unit_from_tex(uv.x, float(k_sky_transmittance_width));
  const float x_r = sky_unit_from_tex(uv.y, float(k_sky_transmittance_height));
  const float big_h = sqrt((top - bottom) * (top + bottom));
  const float rho = big_h * x_r;
  r = sqrt(rho * rho + bottom * bottom);
  h = rho * rho / (r + bottom);
  const float d_min = top - r;
  const float d_max = rho + big_h;
  const float d = d_min + x_mu * (d_max - d_min);
  mu = d <= 0.0 ? 1.0 : (big_h * big_h - rho * rho - d * d) / (2.0 * r * d);
  mu = clamp(mu, -1.0, 1.0);
}

// The transmittance from (r, h) along mu to the top of the air, out of the table: what light
// arriving from that direction keeps. The caller has made sure the ray does not meet the ground.
float3 sky_transmittance(SkyParams* sky, float r, float h, float mu) {
  return sky_bilinear(sky->transmittance, k_sky_transmittance_width, k_sky_transmittance_height,
                      sky_transmittance_uv(sky, r, h, mu), false)
      .rgb;
}

// The same, or nothing where the planet is in the way: the light a point receives from a
// direction.
float3 sky_transmittance_to_light(SkyParams* sky, float r, float h, float mu) {
  if (sky_hits_ground(sky->radii.x, r, h, mu)) return float3(0.0);
  return sky_transmittance(sky, r, h, mu);
}

// ---- the multiple-scattering table (Hillaire's Psi_ms) ----------------------------------------

float2 sky_multiscatter_uv(SkyParams* sky, float h, float mu_light) {
  const float n = float(k_sky_multiscatter_size);
  return float2(sky_tex_from_unit(saturate(mu_light * 0.5 + 0.5), n),
                sky_tex_from_unit(saturate(h / (sky->radii.y - sky->radii.x)), n));
}

float3 sky_multiscatter(SkyParams* sky, float h, float mu_light) {
  return sky_bilinear(sky->multiscatter, k_sky_multiscatter_size, k_sky_multiscatter_size,
                      sky_multiscatter_uv(sky, h, mu_light), false)
      .rgb;
}

// ---- the sky-view parameterization (Hillaire's) ------------------------------------------------
//
// Across: the world azimuth from +x towards +z. Down: the zenith angle, the horizon at the middle and
// the rows crowded towards it from both sides by a square law. The horizon is where the eye's own
// height puts it: a little below the level at altitude, which the zenith angle of the ray grazing the
// planet (`zenith_horizon`) says.

// The zenith angle of the horizon from a height h, and the angle between the horizon and the nadir.
void sky_horizon(SkyParams* sky, float h, out float zenith_horizon, out float beta) {
  const float bottom = sky->radii.x;
  const float r = bottom + h;
  const float v_horizon = sqrt(max(h * (2.0 * bottom + h), 0.0));
  beta = acos(clamp(v_horizon / r, -1.0, 1.0));
  zenith_horizon = k_sky_pi - beta;
}

float sky_azimuth(float3 dir) {
  const float a = atan2(dir.z, dir.x);
  return a < 0.0 ? a + 2.0 * k_sky_pi : a;
}

// A direction's coordinate in a table of this parameterization, from an eye at height h.
float2 sky_view_uv(SkyParams* sky, float h, float3 dir) {
  float zenith_horizon;
  float beta;
  sky_horizon(sky, h, zenith_horizon, beta);
  const float zenith = acos(clamp(dir.y, -1.0, 1.0));
  float v;
  if (zenith < zenith_horizon) {
    const float c = sqrt(max(1.0 - zenith / zenith_horizon, 0.0));
    v = (1.0 - c) * 0.5;
  } else {
    const float c = sqrt(max((zenith - zenith_horizon) / beta, 0.0));
    v = c * 0.5 + 0.5;
  }
  return float2(sky_azimuth(dir) / (2.0 * k_sky_pi), v);
}

// The direction a table coordinate stands for.
float3 sky_view_direction(SkyParams* sky, float h, float2 uv) {
  float zenith_horizon;
  float beta;
  sky_horizon(sky, h, zenith_horizon, beta);
  float zenith;
  if (uv.y < 0.5) {
    const float c = 1.0 - 2.0 * uv.y;
    zenith = zenith_horizon * (1.0 - c * c);
  } else {
    const float c = uv.y * 2.0 - 1.0;
    zenith = zenith_horizon + beta * c * c;
  }
  const float azimuth = uv.x * 2.0 * k_sky_pi;
  const float s = sin(zenith);
  return float3(s * cos(azimuth), cos(zenith), s * sin(azimuth));
}

// The sky the eye sees along `dir`, out of the frame's table: every order of scattering of the sun
// and the moon between the eye and the top of the air — or, below the horizon, the ground the ray
// meets and what it reflects of them.
float3 sky_view_radiance(SkyParams* sky, float3 dir) {
  const float2 uv = sky_view_uv(sky, sky->camera.w, dir);
  return sky_bilinear(sky->sky_view, k_sky_view_width, k_sky_view_height, uv, true).rgb;
}

// ---- in-scattering along a ray ----------------------------------------------------------------

// 1 - exp(-x): what a step of air takes out of the light, written so that a short step keeps it. A
// float step of a few metres at the ground takes out a few millionths, which `1 - exp(-x)` rounds to
// nothing or to twice as much; the series is exact to 1e-8 below x = 0.01.
float sky_absorbed1(float x) {
  return x < 1e-2 ? x * (1.0 - x * (0.5 - x * (1.0 / 6.0))) : 1.0 - exp(-x);
}
float3 sky_absorbed(float3 x) {
  return float3(sky_absorbed1(x.x), sky_absorbed1(x.y), sky_absorbed1(x.z));
}
//
// One light's part of what a point of the air scatters towards the eye per unit length: the light
// that reaches the point (its illuminance outside the air, through the air to the point, and nothing
// where the planet is in the way) times the phase functions and the scattering there — single
// scattering — plus every higher order through Psi_ms, which is isotropic and needs no shadow.
float3 sky_light_scattering(SkyParams* sky, SkyMedium m, float r, float h, float mu_light, float nu,
                            float3 illuminance) {
  const float bottom = sky->radii.x;
  float3 single = float3(0.0);
  if (!sky_hits_ground(bottom, r, h, mu_light)) {
    const float3 t = sky_transmittance(sky, r, h, mu_light);
    single = t * (m.rayleigh * sky_phase_rayleigh(nu) +
                  m.mie * sky_phase_mie(nu, sky->mie_extinction.w));
  }
  const float3 multiple = sky_multiscatter(sky, h, mu_light) * (m.rayleigh + m.mie);
  return illuminance * (single + multiple);
}

// The in-scattered radiance and the transmittance along a ray from an eye at (r0, h0) in direction
// `dir` (zenith cosine dir.y: the world is flat) out to `t_max` km, in `samples` segments a square
// law apart (finer near the eye, where the air is densest along a level ray). Hillaire's
// energy-conserving step: over a segment of extinction e and scattering S, S (1 - exp(-e dt)) / e,
// with `sky_absorbed` for the bracket.
void sky_march(SkyParams* sky, float r0, float h0, float3 dir, float t_max, uint samples,
               inout float3 radiance, inout float3 throughput) {
  const float bottom = sky->radii.x;
  const float mu = dir.y;
  const float3 sun = sky->sun.xyz;
  const float3 moon = sky->moon.xyz;
  const bool moon_on = (sky->flags & k_sky_moon) != 0u;
  const float nu_sun = dot(dir, sun);
  const float nu_moon = dot(dir, moon);
  float t_prev = 0.0;
  for (uint i = 0; i < samples; ++i) {
    const float f = float(i + 1) / float(samples);
    const float t_next = t_max * f * f;
    const float dt = t_next - t_prev;
    const float t = t_prev + 0.5 * dt;
    t_prev = t_next;
    const float h = sky_height_along(bottom, r0, h0, mu, t);
    const float r = bottom + h;
    const SkyMedium m = sky_medium(sky, h);
    float3 s = sky_light_scattering(sky, m, r, h, (r0 * sun.y + t * nu_sun) / r, nu_sun,
                                    sky->sun_illuminance.rgb);
    if (moon_on) {
      s += sky_light_scattering(sky, m, r, h, (r0 * moon.y + t * nu_moon) / r, nu_moon,
                                sky->moon_illuminance.rgb);
    }
    const float3 absorbed = sky_absorbed(m.extinction * dt);
    radiance += throughput * s * absorbed / max(m.extinction, float3(1e-9));
    throughput *= exp(-m.extinction * dt);
  }
}

// ---- the aerial perspective table ---------------------------------------------------------------

// The slice coordinate of a distance: slice k (from 1) ends at D (k / depth)^2.
float sky_aerial_slice(float distance_km) {
  return sqrt(max(distance_km, 0.0) / k_sky_aerial_distance_km) * float(k_sky_aerial_depth);
}

// What the air between the eye and a surface `distance_m` away along `dir` adds to it and leaves of
// it: out of the frame's table, bilinear in direction and linear in the slice, with slice 0 the eye
// itself (nothing added, everything kept).
void sky_aerial(SkyParams* sky, float3 dir, float distance_m, out float3 inscatter,
                out float transmittance) {
  const float2 uv = sky_view_uv(sky, sky->camera.w, dir);
  const float s = min(sky_aerial_slice(distance_m * 0.001), float(k_sky_aerial_depth));
  const float k = floor(s);
  const float w = s - k;
  const uint plane = k_sky_aerial_width * k_sky_aerial_height;
  float4 lo = float4(0.0, 0.0, 0.0, 1.0);
  if (k >= 1.0) {
    lo = sky_bilinear(sky->aerial + uint64_t(uint(k - 1.0) * plane) * 16, k_sky_aerial_width,
                      k_sky_aerial_height, uv, true);
  }
  float4 hi = lo;
  if (k < float(k_sky_aerial_depth)) {
    hi = sky_bilinear(sky->aerial + uint64_t(uint(k) * plane) * 16, k_sky_aerial_width,
                      k_sky_aerial_height, uv, true);
  }
  const float4 v = lerp(lo, hi, w);
  inscatter = v.rgb;
  transmittance = v.a;
}

// ---- where a world point is in the air ----------------------------------------------------------

// Its height above the planet's ground (km), which is the world's y = 0, at least the tables' least.
float sky_height_of(float3 world) { return max(world.y * 0.001, k_sky_min_altitude_km); }

// The light that reaches a world point from a light outside the air in direction `dir` of
// illuminance `illuminance`: through the air above the point, or nothing below the planet's horizon.
float3 sky_light_at(SkyParams* sky, float3 world, float3 dir, float3 illuminance) {
  const float h = sky_height_of(world);
  return illuminance * sky_transmittance_to_light(sky, sky->radii.x + h, h, dir.y);
}

// ---- the ambient term ---------------------------------------------------------------------------

// The nine real spherical harmonics with the world's up (+y) as the pole, in the order `SkyFrame::sh`
// holds them.
void sky_sh_basis(float3 n, out float basis[9]) {
  basis[0] = 0.282095;
  basis[1] = 0.488603 * n.y;
  basis[2] = 0.488603 * n.z;
  basis[3] = 0.488603 * n.x;
  basis[4] = 1.092548 * n.x * n.y;
  basis[5] = 1.092548 * n.y * n.z;
  basis[6] = 0.315392 * (3.0 * n.y * n.y - 1.0);
  basis[7] = 1.092548 * n.x * n.z;
  basis[8] = 0.546274 * (n.x * n.x - n.z * n.z);
}

// The ambient radiance a Lambertian surface of normal n takes from the frame's environment: its
// irradiance over pi, from the coefficients the frame's sky pass projected and convolved. Clamped at
// zero, since nine coefficients of a sky with a sharp horizon can ring below it.
float3 sky_ambient(SkyFrame* frame, float3 n) {
  float basis[9];
  sky_sh_basis(n, basis);
  float3 sum = float3(0.0);
  for (uint i = 0; i < 9; ++i) sum += frame->sh[i].rgb * basis[i];
  return max(sum, float3(0.0));
}

// ---- the lights' discs and the stars -----------------------------------------------------------

// Limb darkening per channel, the linear law I(mu) = 1 - u (1 - mu) with u for about 680, 550 and
// 450 nm: the disc is dimmer and redder towards its edge.
static const float3 k_sky_limb = float3(0.48, 0.60, 0.72);

// Where on a disc of angular radius `radius` round `centre` the direction `dir` falls: the sine of
// its distance from the centre over the radius's, 0 at the centre and 1 at the edge; false outside.
bool sky_disc(float3 dir, float3 centre, float radius, out float s, out float3 across) {
  s = 0.0;
  across = float3(0.0);
  const float c = dot(dir, centre);
  if (c <= cos(radius)) return false;
  const float3 q = dir - centre * c;
  const float len = length(q);
  s = min(len / sin(radius), 1.0);
  across = len > 0.0 ? q / len : float3(0.0);
  return true;
}

// The sun's disc: its illuminance spread over its solid angle and darkened towards the limb so the
// disc's mean is the illuminance over the solid angle, outside the air.
float3 sky_sun_disc(SkyParams* sky, float3 dir) {
  float s;
  float3 across;
  if (!sky_disc(dir, sky->sun.xyz, sky->sun.w, s, across)) return float3(0.0);
  const float mu = sqrt(max(1.0 - s * s, 0.0));
  const float solid = 2.0 * k_sky_pi * (1.0 - cos(sky->sun.w));
  const float3 limb = float3(1.0) - k_sky_limb * (1.0 - mu);
  return sky->sun_illuminance.rgb * limb / (solid * (float3(1.0) - k_sky_limb / 3.0));
}

// The moon's disc: a Lambertian sphere of the moon's albedo lit by the sun, seen from the eye, so its
// **phase is the sun's direction and its own**: the point of the sphere under `dir` has the normal
// that faces the eye there, and is lit where that normal faces the sun (the sun is 400 times further
// than the moon, so the direction from the moon to the sun is the eye's to within a fifth of a degree).
float3 sky_moon_disc(SkyParams* sky, float3 dir) {
  float s;
  float3 across;
  const float3 m = sky->moon.xyz;
  if (!sky_disc(dir, m, sky->moon.w, s, across)) return float3(0.0);
  const float3 n = across * s - m * sqrt(max(1.0 - s * s, 0.0));
  const float lit = max(dot(n, sky->sun.xyz), 0.0);
  return sky->sun_illuminance.rgb * (sky->moon_illuminance.w * lit / k_sky_pi);
}

// A direction's cell of the star table's cube: the face its largest component names, then a
// k_sky_star_face_cells square grid over the other two over that one. The renderer lists the stars
// by the same function (renderer::sky_star_cell), so the two cannot disagree about a cell.
uint sky_star_cell(float3 d) {
  const float3 a = abs(d);
  uint face;
  float2 uv;
  if (a.x >= a.y && a.x >= a.z) {
    face = d.x > 0.0 ? 0u : 1u;
    uv = d.yz / a.x;
  } else if (a.y >= a.z) {
    face = d.y > 0.0 ? 2u : 3u;
    uv = d.xz / a.y;
  } else {
    face = d.z > 0.0 ? 4u : 5u;
    uv = d.xy / a.z;
  }
  const float n = float(k_sky_star_face_cells);
  const uint cx = min(uint(max((uv.x * 0.5 + 0.5) * n, 0.0)), k_sky_star_face_cells - 1u);
  const uint cy = min(uint(max((uv.y * 0.5 + 0.5) * n, 0.0)), k_sky_star_face_cells - 1u);
  return (face * k_sky_star_face_cells + cy) * k_sky_star_face_cells + cx;
}

// The stars: each a point whose irradiance is spread over a Gaussian spot `sigma` radians wide (a
// pixel), so that the radiance summed over the pixels it touches times their solid angle is the
// star's irradiance, and a star is the same brightness whether it falls on a pixel's centre or its
// corner. The angle comes from the cross product, which holds a thousandth of a degree where
// `1 - cos` holds a fiftieth.
float3 sky_stars(SkyParams* sky, float3 dir, float sigma) {
  const float3 d = float3(dot(dir, sky->celestial_x.xyz), dot(dir, sky->celestial_y.xyz),
                          dot(dir, sky->celestial_z.xyz));
  const uint cell = sky_star_cell(d);
  uint* cells = (uint*)sky->star_cells;
  uint* index = (uint*)sky->star_index;
  float4* list = (float4*)sky->star_list;
  const float sg = clamp(sigma, 1.0e-6, k_sky_star_max_sigma);
  const float inv = 1.0 / (2.0 * sg * sg);
  float3 sum = float3(0.0);
  const uint end = cells[cell + 1];
  for (uint i = cells[cell]; i < end; ++i) {
    const uint star = index[i];
    const float3 at = list[2 * star].xyz;
    if (dot(d, at) <= 0.0) continue;
    const float3 c = cross(d, at);
    sum += list[2 * star + 1].rgb * (exp(-dot(c, c) * inv) * inv / k_sky_pi);
  }
  return sum * sky->stars.y;
}

// ---- what an uncovered pixel shows --------------------------------------------------------------

// The night sky's own glow, uniform above the horizon: airglow, zodiacal light and the unresolved
// stars, which keep a moonless night from being black.
float3 sky_night_glow(SkyParams* sky, float3 dir) {
  return dir.y >= 0.0 ? sky->night.rgb : float3(0.0);
}

// Everything an eye sees along `dir` with nothing in the way: the scattered sky out of the table, the
// night's glow, and the sun's disc, the moon's and the stars through the air along `dir` (which dims
// and reddens them towards the horizon and hides them below it). `pixel` is the angle a pixel spans,
// which a star's spot is as wide as. The ray path's pixels and the reference tracer's primary misses
// are this function too.
float3 sky_background(SkyParams* sky, float3 dir, float pixel) {
  float3 radiance = sky_view_radiance(sky, dir) + sky_night_glow(sky, dir);
  const float h = sky->camera.w;
  const float r = sky->radii.x + h;
  if (sky_hits_ground(sky->radii.x, r, h, dir.y)) return radiance;
  float3 light = float3(0.0);
  if ((sky->flags & k_sky_sun_disc) != 0u) light += sky_sun_disc(sky, dir);
  if ((sky->flags & k_sky_moon) != 0u) light += sky_moon_disc(sky, dir);
  if ((sky->flags & k_sky_stars) != 0u) light += sky_stars(sky, dir, pixel * 0.7);
  if (any(light > 0.0)) radiance += light * sky_transmittance(sky, r, h, dir.y);
  return radiance;
}

// The environment an escaped secondary ray of the reference sees: the scattered sky and the night's
// glow above the horizon, the ground below it (the ambient term's lower half) — never the sun's or
// the moon's disc, which the tracer reaches by next-event estimation, nor the stars.
float3 sky_environment(SkyParams* sky, SkyFrame* frame, float3 dir) {
  if (dir.y < 0.0) return frame->ground.rgb;
  return sky_view_radiance(sky, dir) + sky->night.rgb;
}

// A world ray's direction through a view's pixel, in the view's own clip space: the pixel centre's
// ndc through the inverse view-projection, from the eye.
float3 sky_pixel_direction(SkyParams* sky, uint view, float2 ndc) {
  const float4 ahead = mul(sky->views[view].inv_view_proj, float4(ndc, 0.5, 1.0));
  return normalize(ahead.xyz / ahead.w - sky->camera.xyz);
}

// Radiance to the display, before the transfer curve.
float sky_exposure(SkyFrame* frame) { return frame->exposure.x; }

// The display's shoulder (gfx::k_sky_shoulder in sky.h, which says why): an exposed channel at or
// below `s` is itself, one above it rolls off towards 1 with the line's slope where they meet.
float sky_tone1(float x, float s) {
  if (x <= s || s >= 1.0) return x;
  return s + (1.0 - s) * (1.0 - exp(-(x - s) / (1.0 - s)));
}
float3 sky_tone(SkyParams* sky, float3 exposed) {
  return float3(sky_tone1(exposed.x, sky->shoulder), sky_tone1(exposed.y, sky->shoulder),
                sky_tone1(exposed.z, sky->shoulder));
}

// The exposure rule (gfx::sky_exposure_ev100 in sky.h, which says why): incident-light metering at
// ISO 100, EV100 = log2(E / 2.5) for E lux, and below the knee only `slope` of each further stop.
// Less `compensation` stops.
float sky_exposure_ev100(float lux, float compensation, float knee_lux, float slope) {
  const float e = max(lux, 1.0e-12);
  const float knee = max(knee_lux, 1.0e-12);
  float ev = log2(e / 2.5);
  if (e < knee) ev = log2(knee / 2.5) + slope * log2(e / knee);
  return ev - compensation;
}

// Photometric luminance of a linear sRGB colour (Rec. 709 weights): how the rule reads a light.
float sky_luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

#endif  // ENGINE_SKY_SLANG
