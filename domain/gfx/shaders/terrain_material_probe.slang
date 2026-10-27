#define MATERIAL_TERRAIN 1
#include "material.slang"

struct ProbeParams {
  uint64_t output;
  uint count;
  uint pad;
};
[[vk::push_constant]] ConstantBuffer<ProbeParams> g_probe;

// A smooth periodic stand-in for a texture (period 1 in both axes), so the stochastic blend can be
// checked against its double-precision mirror without a texture.
float terrain_probe_texture(float2 p) {
  const float k_two_pi = 6.28318530717958647;
  return 0.5 + 0.25 * sin(k_two_pi * (p.x + 2.0 * p.y)) + 0.15 * cos(k_two_pi * (3.0 * p.x - p.y));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void probe_main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_probe.count) return;
  const int i = int(id.x);
  FrameEye eye = (FrameEye)0;
  eye.cell = int3((i & 1) == 0 ? 6548 : -6548, 0, 1562500);
  eye.local = float3(63.999, 0.125, -0.25);
  eye.residual = float3(0.00001, 0, 0);
  const float3 point = float3(float(i) * 0.001, -float(i) * 0.25, float(i) * 0.125);
  const float3 n = normalize(float3(float(i % 3) - 1.0, 0.25, float(i % 5) - 2.0));
  const float3 slope = terrain_project_slope(float3(0.3, -0.4, 0.8660254), 0.75, n,
                                             float3(1, 0, 0), float3(0, 0, 1));
  const float3 flat = terrain_project_slope(float3(0, 0, 1), 1.0, n,
                                            float3(1, 0, 0), float3(0, 0, 1));
  float4* output = (float4*)g_probe.output;
  output[3 * i] = float4(terrain_phase3(eye, point, 1500.0), 0);
  output[3 * i + 1] = float4(slope, dot(slope, n));
  output[3 * i + 2] = float4(flat, length(normalize(n + slope)));
  // The stochastic lattice (R96) at the same far eye: projection i % 3, a turn of up to 0.7 rad on
  // odd points, and the variance-preserving blend of the stand-in texture at the three tiles.
  const uint axis = uint(i) % 3u;
  const float rotation = (i & 1) == 0 ? 0.0 : 0.7;
  const TerrainHex hex = terrain_hex(eye, point, axis, 1500.0, rotation);
  const float3 phase = terrain_phase3(eye, point, 1500.0);
  const float2 uv = axis == 0 ? phase.zy : (axis == 1 ? phase.xz : phase.xy);
  const float3 blended = terrain_hex_blend(hex, float3(0.5), float3(terrain_probe_texture(uv + hex.shift[0])),
                                           float3(terrain_probe_texture(uv + hex.shift[1])),
                                           float3(terrain_probe_texture(uv + hex.shift[2])));
  const uint base = 3u * g_probe.count + 5u * uint(i);
  output[base] = float4(hex.weight, hex.gain);
  output[base + 1] = float4(hex.shift[0], hex.shift[1]);
  output[base + 2] = float4(hex.shift[2], hex.turn[0]);
  output[base + 3] = float4(hex.turn[1], hex.turn[2]);
  output[base + 4] = float4(blended.x, 0, 0, 0);
}
