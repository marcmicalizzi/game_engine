// The sky's tables and the frame's sums (docs/subsystems/gfx.md, "The sky"; sky.h, sky.slang): five
// compute entry points, each pushing only the address of the frame's `SkyParams`.
//
//   transmittance_main  once a scene: the transmittance to the top of the air, 256 x 64
//   multiscatter_main   once a scene, after it: Hillaire's Psi_ms, 32 x 32
//   sky_view_main       every frame: the sky from the eye, 192 x 108
//   aerial_main         every frame: the air between the eye and a surface, 64 x 32 x 32 slices
//   sky_frame_main      every frame, after both: the ambient term's coefficients, the lights at the
//                       eye and the exposure (`SkyFrame`), one workgroup
//
// The first two depend only on the air and the ground's albedo, so the renderer builds them when a
// scene's sky is made; the last three depend on where the sun, the moon and the eye are, and are
// rebuilt every frame, which is what lets a day run at any rate with no history to go stale.
// domain/gfx/tests/sky_reference.h builds every one of them again in double precision.

#include "sky.slang"

struct SkyPush {
  uint64_t params;
};

[[vk::push_constant]] ConstantBuffer<SkyPush> g_push;

// ---- the transmittance ------------------------------------------------------------------------
//
// 256 midpoint steps, not Hillaire's 40: his aerosol is a clean air's (a vertical depth of 0.005),
// and a desert's haze is thirty times that with a scale height of 1.2 km, which a 2.5 km step reads
// at its midpoint as a third of its mean — 3% of the blue light straight up at a turbidity of 2.5
// (sky_tests.cpp). The table is built once a scene, so the steps cost nothing a frame.

static const uint k_sky_transmittance_steps = 256;

[shader("compute")]
[numthreads(8, 8, 1)]
void transmittance_main(uint3 id : SV_DispatchThreadID) {
  SkyParams* sky = (SkyParams*)g_push.params;
  if (id.x >= k_sky_transmittance_width || id.y >= k_sky_transmittance_height) return;
  const float2 uv = (float2(id.xy) + 0.5) /
                    float2(float(k_sky_transmittance_width), float(k_sky_transmittance_height));
  float r;
  float h;
  float mu;
  sky_transmittance_ray(sky, uv, r, h, mu);
  const float bottom = sky->radii.x;
  const float d = sky_distance_to_top(bottom, sky->radii.y, r, h, mu);
  const float dt = d / float(k_sky_transmittance_steps);
  float3 depth = float3(0.0);
  for (uint i = 0; i < k_sky_transmittance_steps; ++i) {
    const float t = (float(i) + 0.5) * dt;
    depth += sky_medium(sky, sky_height_along(bottom, r, h, mu, t)).extinction * dt;
  }
  float4* out = (float4*)sky->transmittance;
  out[id.y * k_sky_transmittance_width + id.x] = float4(exp(-depth), 1.0);
}

// ---- Psi_ms (Hillaire 2020, section 5.5) ------------------------------------------------------
//
// At a height h, with a light of unit illuminance at zenith cosine mu_light: the second order of
// scattering L_2 arriving at the point over the whole sphere, with the ground's reflection, and the
// transfer f_ms — the share of isotropically scattered light the sphere of air round the point sends
// back to it — both with an isotropic phase function over 64 directions of 20 steps. Every higher
// order is then a geometric series, Psi_ms = L_2 / (1 - f_ms).

static const uint k_sky_ms_directions = 8;  // a side: 64 directions
static const uint k_sky_ms_steps = 20;

[shader("compute")]
[numthreads(8, 8, 1)]
void multiscatter_main(uint3 id : SV_DispatchThreadID) {
  SkyParams* sky = (SkyParams*)g_push.params;
  const uint n = k_sky_multiscatter_size;
  if (id.x >= n || id.y >= n) return;
  const float2 uv = (float2(id.xy) + 0.5) / float(n);
  const float mu_light = sky_unit_from_tex(uv.x, float(n)) * 2.0 - 1.0;
  const float bottom = sky->radii.x;
  const float top = sky->radii.y;
  const float h = saturate(sky_unit_from_tex(uv.y, float(n))) * (top - bottom);
  const float r = bottom + h;
  const float3 light = float3(sqrt(max(1.0 - mu_light * mu_light, 0.0)), mu_light, 0.0);
  const float uniform_phase = 1.0 / (4.0 * k_sky_pi);
  float3 l_sum = float3(0.0);
  float3 f_sum = float3(0.0);
  for (uint i = 0; i < k_sky_ms_directions; ++i) {
    for (uint j = 0; j < k_sky_ms_directions; ++j) {
      const float theta = 2.0 * k_sky_pi * (float(i) + 0.5) / float(k_sky_ms_directions);
      const float phi = acos(1.0 - 2.0 * (float(j) + 0.5) / float(k_sky_ms_directions));
      const float3 dir = float3(cos(theta) * sin(phi), cos(phi), sin(theta) * sin(phi));
      const float mu = dir.y;
      const float nu = dot(dir, light);
      const bool ground = sky_hits_ground(bottom, r, h, mu);
      const float t_max = ground ? sky_distance_to_ground(bottom, r, h, mu)
                                 : sky_distance_to_top(bottom, top, r, h, mu);
      const float dt = t_max / float(k_sky_ms_steps);
      float3 radiance = float3(0.0);
      float3 transfer = float3(0.0);
      float3 throughput = float3(1.0);
      for (uint s = 0; s < k_sky_ms_steps; ++s) {
        const float t = (float(s) + 0.5) * dt;
        const float hs = sky_height_along(bottom, r, h, mu, t);
        const float rs = bottom + hs;
        const SkyMedium m = sky_medium(sky, hs);
        const float3 scattering = m.rayleigh + m.mie;
        const float mu_s = (r * mu_light + t * nu) / rs;
        const float3 to_light = sky_transmittance_to_light(sky, rs, hs, mu_s);
        const float3 in_s = to_light * scattering * uniform_phase;
        const float3 absorbed = sky_absorbed(m.extinction * dt) / max(m.extinction, float3(1e-9));
        radiance += throughput * in_s * absorbed;
        transfer += throughput * scattering * absorbed;
        throughput *= exp(-m.extinction * dt);
      }
      if (ground) {
        const float mu_g = (r * mu_light + t_max * nu) / bottom;
        const float3 to_light = sky_transmittance_to_light(sky, bottom, 0.0, mu_g);
        radiance += throughput * to_light * max(mu_g, 0.0) * sky->ground_albedo.rgb / k_sky_pi;
      }
      l_sum += radiance;
      f_sum += transfer;
    }
  }
  const float count = float(k_sky_ms_directions * k_sky_ms_directions);
  // (sum * 4 pi / count) * the isotropic phase 1 / (4 pi): the mean over the sphere.
  const float3 l2 = l_sum / count;
  const float3 fms = f_sum / count;
  float4* out = (float4*)sky->multiscatter;
  out[id.y * n + id.x] = float4(l2 / (float3(1.0) - fms), 1.0);
}

// ---- the sky from the eye ---------------------------------------------------------------------

static const uint k_sky_view_steps = 32;

[shader("compute")]
[numthreads(8, 8, 1)]
void sky_view_main(uint3 id : SV_DispatchThreadID) {
  SkyParams* sky = (SkyParams*)g_push.params;
  if (id.x >= k_sky_view_width || id.y >= k_sky_view_height) return;
  const float2 uv =
      (float2(id.xy) + 0.5) / float2(float(k_sky_view_width), float(k_sky_view_height));
  const float h = sky->camera.w;
  const float bottom = sky->radii.x;
  const float r = bottom + h;
  const float3 dir = sky_view_direction(sky, h, uv);
  const bool ground = sky_hits_ground(bottom, r, h, dir.y);
  const float t_max = ground ? sky_distance_to_ground(bottom, r, h, dir.y)
                             : sky_distance_to_top(bottom, sky->radii.y, r, h, dir.y);
  float3 radiance = float3(0.0);
  float3 throughput = float3(1.0);
  sky_march(sky, r, h, dir, t_max, k_sky_view_steps, radiance, throughput);
  // Below the horizon the ray meets the planet's ground, which reflects the sun and the moon that
  // reach it: the albedo over pi times the illuminance on it.
  if (ground) {
    const float mu_sun = (r * sky->sun.y + t_max * dot(dir, sky->sun.xyz)) / bottom;
    float3 lit = sky->sun_illuminance.rgb * sky_transmittance_to_light(sky, bottom, 0.0, mu_sun) *
                 max(mu_sun, 0.0);
    if ((sky->flags & k_sky_moon) != 0u) {
      const float mu_moon = (r * sky->moon.y + t_max * dot(dir, sky->moon.xyz)) / bottom;
      lit += sky->moon_illuminance.rgb * sky_transmittance_to_light(sky, bottom, 0.0, mu_moon) *
             max(mu_moon, 0.0);
    }
    radiance += throughput * lit * sky->ground_albedo.rgb / k_sky_pi;
  }
  float4* out = (float4*)sky->sky_view;
  out[id.y * k_sky_view_width + id.x] = float4(radiance, 1.0);
}

// ---- the air between the eye and a surface ------------------------------------------------------
//
// The sky-view table's parameterization with a third axis: one thread per direction marches out to
// `k_sky_aerial_distance_km` and writes, at the end of every slice, what the air so far has added
// (rgb) and the mean of what it has kept (a). It does not stop at the planet's ground: a surface is
// what ends a ray here, and a surface a little below the world's zero (a basin) still gets its air.

static const uint k_sky_aerial_substeps = 2;

[shader("compute")]
[numthreads(8, 8, 1)]
void aerial_main(uint3 id : SV_DispatchThreadID) {
  SkyParams* sky = (SkyParams*)g_push.params;
  if (id.x >= k_sky_aerial_width || id.y >= k_sky_aerial_height) return;
  const float2 uv =
      (float2(id.xy) + 0.5) / float2(float(k_sky_aerial_width), float(k_sky_aerial_height));
  const float h0 = sky->camera.w;
  const float bottom = sky->radii.x;
  const float r0 = bottom + h0;
  const float3 dir = sky_view_direction(sky, h0, uv);
  const float mu = dir.y;
  const float3 sun = sky->sun.xyz;
  const float3 moon = sky->moon.xyz;
  const bool moon_on = (sky->flags & k_sky_moon) != 0u;
  const float nu_sun = dot(dir, sun);
  const float nu_moon = dot(dir, moon);
  float3 radiance = float3(0.0);
  float3 throughput = float3(1.0);
  float t_prev = 0.0;
  float4* out = (float4*)sky->aerial;
  const uint plane = k_sky_aerial_width * k_sky_aerial_height;
  for (uint k = 1; k <= k_sky_aerial_depth; ++k) {
    const float f = float(k) / float(k_sky_aerial_depth);
    const float t_end = k_sky_aerial_distance_km * f * f;
    const float dt = (t_end - t_prev) / float(k_sky_aerial_substeps);
    for (uint s = 0; s < k_sky_aerial_substeps; ++s) {
      const float t = t_prev + (float(s) + 0.5) * dt;
      const float h = sky_height_along(bottom, r0, h0, mu, t);
      const float r = bottom + max(h, 0.0);
      const float hc = max(h, 0.0);
      const SkyMedium m = sky_medium(sky, hc);
      float3 in_s = sky_light_scattering(sky, m, r, hc, (r0 * sun.y + t * nu_sun) / r, nu_sun,
                                         sky->sun_illuminance.rgb);
      if (moon_on) {
        in_s += sky_light_scattering(sky, m, r, hc, (r0 * moon.y + t * nu_moon) / r, nu_moon,
                                     sky->moon_illuminance.rgb);
      }
      radiance += throughput * in_s * sky_absorbed(m.extinction * dt) /
                  max(m.extinction, float3(1e-9));
      throughput *= exp(-m.extinction * dt);
    }
    t_prev = t_end;
    out[(k - 1) * plane + id.y * k_sky_aerial_width + id.x] =
        float4(radiance, (throughput.x + throughput.y + throughput.z) / 3.0);
  }
}

// ---- the frame's sums: the ambient term, the lights at the eye, the exposure --------------------
//
// One workgroup. The upper hemisphere, `k_sky_sh_rows` rows of zenith angle by twice as many of
// azimuth at their midpoints, is read out of the sky-view table (plus the night's glow) and projected
// on the nine harmonics, and the level irradiance it gives is summed beside them. Then one thread
// finishes: the sun's and the moon's light at the eye through the air, the ground's radiance — its
// albedo over pi times the sun, the moon and the sky on a level surface, uniform below the horizon as
// the stand-in's was — added as the lower hemisphere's own two nonzero coefficients, the whole
// convolved with the cosine lobe (Ramamoorthi and Hanrahan: pi, 2 pi / 3, pi / 4, over pi), and the
// exposure metered from the sun's and the moon's light on a level surface and pi times the sky's
// mean radiance.

static const uint k_sky_sh_rows = 32;
static const uint k_sky_frame_threads = 256;

groupshared float4 g_sky_part[k_sky_frame_threads];

float3 sky_group_sum(uint thread, float3 value) {
  g_sky_part[thread] = float4(value, 0.0);
  GroupMemoryBarrierWithGroupSync();
  for (uint stride = k_sky_frame_threads / 2; stride > 0; stride >>= 1) {
    if (thread < stride) g_sky_part[thread] += g_sky_part[thread + stride];
    GroupMemoryBarrierWithGroupSync();
  }
  const float3 total = g_sky_part[0].xyz;
  GroupMemoryBarrierWithGroupSync();
  return total;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void sky_frame_main(uint3 thread_id : SV_GroupThreadID) {
  SkyParams* sky = (SkyParams*)g_push.params;
  const uint thread = thread_id.x;
  const uint rows = k_sky_sh_rows;
  const uint cols = 2 * k_sky_sh_rows;
  const float d_theta = 0.5 * k_sky_pi / float(rows);
  const float d_phi = 2.0 * k_sky_pi / float(cols);
  float3 sh[9];
  for (uint c = 0; c < 9; ++c) sh[c] = float3(0.0);
  float3 level = float3(0.0);
  for (uint k = thread; k < rows * cols; k += k_sky_frame_threads) {
    const uint row = k / cols;
    const uint col = k - row * cols;
    const float theta = (float(row) + 0.5) * d_theta;
    const float phi = (float(col) + 0.5) * d_phi;
    const float s = sin(theta);
    const float3 dir = float3(s * cos(phi), cos(theta), s * sin(phi));
    const float3 radiance = sky_view_radiance(sky, dir) + sky->night.rgb;
    const float d_omega = s * d_theta * d_phi;
    float basis[9];
    sky_sh_basis(dir, basis);
    for (uint c = 0; c < 9; ++c) sh[c] += radiance * (basis[c] * d_omega);
    level += radiance * (dir.y * d_omega);
  }
  for (uint c = 0; c < 9; ++c) sh[c] = sky_group_sum(thread, sh[c]);
  level = sky_group_sum(thread, level);
  if (thread != 0) return;
  // The sky's mean radiance over its hemisphere, times pi: what a level surface would get from it if
  // every part of the sky counted as much as the zenith. The first harmonic is 0.282095 of the
  // integral, and pi / (2 pi) of the integral is half of it.
  const float3 dome = sh[0] / (2.0 * 0.282095);

  const float h = sky->camera.w;
  const float r = sky->radii.x + h;
  const float3 sun_ground = sky->sun_illuminance.rgb * sky_transmittance_to_light(sky, r, h, sky->sun.y);
  float3 moon_ground = float3(0.0);
  if ((sky->flags & k_sky_moon) != 0u) {
    moon_ground = sky->moon_illuminance.rgb * sky_transmittance_to_light(sky, r, h, sky->moon.y);
  }
  const float3 lit = sun_ground * max(sky->sun.y, 0.0) + moon_ground * max(sky->moon.y, 0.0);
  const float3 ground = sky->local_albedo.rgb * (lit + level) / k_sky_pi;
  // The uniform lower hemisphere's projection: Y_0 over it is 0.282095 * 2 pi, Y_1 (the up axis)
  // 0.488603 * -pi, and every other harmonic nothing.
  sh[0] += ground * (0.282095 * 2.0 * k_sky_pi);
  sh[1] += ground * (-0.488603 * k_sky_pi);
  SkyFrame* frame = (SkyFrame*)sky->frame;
  for (uint c = 0; c < 9; ++c) {
    const float band = c == 0 ? 1.0 : (c < 4 ? 2.0 / 3.0 : 0.25);
    frame->sh[c] = float4(sh[c] * band, 0.0);
  }
  frame->ground = float4(ground, 0.0);
  frame->sun_ground = float4(sun_ground, sky->sun.y);
  frame->moon_ground = float4(moon_ground, sky->moon.y);
  // **The meter** (sky.h, "the exposure rule"): the sun's and the moon's light on a level surface,
  // and the sky's mean over its dome rather than its light on a level surface, which weighs the
  // horizon by its cosine — a dusk whose glow is all at the horizon would read dark on a level
  // meter and be exposed until the glow and the sand it lights were white.
  const float lux = 128000.0 * sky_luminance(lit + dome);
  const float ev = sky->exposure.x >= 0.5
                       ? sky->exposure.y
                       : sky_exposure_ev100(lux, sky->exposure.y, sky->exposure.z, sky->exposure.w);
  frame->exposure =
      float4(128000.0 * k_sky_pi / (2.5 * exp2(ev)), ev, lux, 128000.0 * sky_luminance(dome));
}
