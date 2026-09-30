// A test-only window on sky.slang (domain/gfx/tests/sky_tests.cpp): the functions the resolve and
// the reference path tracer call, evaluated at the test's own directions and points, so each can be
// held to its double-precision mirror (sky_reference.h) one number at a time.

#include "sky.slang"

struct ProbeParams {
  uint64_t sky;      // SkyParams*
  uint64_t queries;  // float4[2 * count]: (direction, pixel angle), (world point, distance m)
  uint64_t out;      // float4[5 * count]
  uint count;
  uint pad;
};

[[vk::push_constant]] ConstantBuffer<ProbeParams> g_params;

[shader("compute")]
[numthreads(64, 1, 1)]
void probe_main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_params.count) return;
  SkyParams* sky = (SkyParams*)g_params.sky;
  float4* queries = (float4*)g_params.queries;
  float4* out = (float4*)g_params.out;
  const float4 a = queries[2 * id.x];
  const float4 b = queries[2 * id.x + 1];
  const float3 dir = normalize(a.xyz);
  out[5 * id.x + 0] = float4(sky_background(sky, dir, a.w), 0.0);
  out[5 * id.x + 1] = float4(sky_ambient((SkyFrame*)sky->frame, dir), 0.0);
  float3 inscatter;
  float transmittance;
  sky_aerial(sky, dir, b.w, inscatter, transmittance);
  out[5 * id.x + 2] = float4(inscatter, transmittance);
  out[5 * id.x + 3] = float4(sky_light_at(sky, b.xyz, dir, sky->sun_illuminance.rgb), 0.0);
  // The scattered sky alone, with the table coordinate it was read at.
  const float2 uv = sky_view_uv(sky, sky->camera.w, dir);
  out[5 * id.x + 4] = float4(sky_view_radiance(sky, dir).r, uv, sky_stars(sky, dir, a.w * 0.7).r);
}
