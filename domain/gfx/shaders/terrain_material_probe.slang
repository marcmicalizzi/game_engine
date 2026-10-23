#define MATERIAL_TERRAIN 1
#include "material.slang"

struct ProbeParams {
  uint64_t output;
  uint count;
  uint pad;
};
[[vk::push_constant]] ConstantBuffer<ProbeParams> g_probe;

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
}
