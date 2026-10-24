// The mask level's sampler (terrain_material.slang, `terrain_mask_sample`) at points the test names
// by their frame: an eye cell and a frame-local point, at a mip level (gfx terrain_material_tests).
#define MATERIAL_TERRAIN 1
#include "material.slang"

struct MaskProbePoint {
  int3 cell;
  float level;
  float3 local;
  float pad;
};

struct MaskProbeParams {
  uint64_t output;  // float4 weights, float4 {wetness, table as float bits, valid, 0} per point
  uint64_t record;  // one TerrainRecord
  uint64_t points;  // MaskProbePoint[]
  uint count;
  uint pad;
};
[[vk::push_constant]] ConstantBuffer<MaskProbeParams> g_mask_probe;

[shader("compute")]
[numthreads(64, 1, 1)]
void mask_probe_main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_mask_probe.count) return;
  const MaskProbePoint p = ((MaskProbePoint*)g_mask_probe.points)[id.x];
  FrameEye eye = (FrameEye)0;
  eye.cell = p.cell;
  const TerrainRecord rec = *((TerrainRecord*)g_mask_probe.record);
  const TerrainMaskSample s = terrain_mask_sample(rec, eye, p.local, p.level);
  float4* output = (float4*)g_mask_probe.output;
  output[2 * id.x] = s.weights;
  output[2 * id.x + 1] = float4(s.wetness, asfloat(s.table), s.valid ? 1.0 : 0.0, 0.0);
}
