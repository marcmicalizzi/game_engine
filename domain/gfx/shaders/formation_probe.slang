// The formation's functions (formation.slang) at points the test names by their frame: an eye
// cell and a local offset from its corner, with a box width for the filter (gfx formation_tests).
#include "formation.slang"

struct FormationProbePoint {
  int3 cell;
  float width;
  float3 local;
  float pad;
};

struct FormationProbeParams {
  uint64_t output;     // five float4 per point (below)
  uint64_t formation;  // one FormationParams
  uint64_t points;     // FormationProbePoint[]
  uint count;
  uint pad;
};
[[vk::push_constant]] ConstantBuffer<FormationProbeParams> g_formation_probe;

[shader("compute")]
[numthreads(64, 1, 1)]
void formation_probe_main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_formation_probe.count) return;
  const FormationProbePoint q = ((FormationProbePoint*)g_formation_probe.points)[id.x];
  const uint64_t address = g_formation_probe.formation;
  FormationParams* f = (FormationParams*)address;
  const FormationPoint p = formation_coordinate(f, q.cell, q.local);
  FormationSequence* beds = (FormationSequence*)(address + k_formation_beds_offset);
  FormationSequence* macro = (FormationSequence*)(address + k_formation_macro_offset);
  const FormationBedSample b = formation_bed(beds, p.bed, p.lateral);
  const FormationBedSample m = formation_bed(macro, p.macro, p.lateral);
  float4* output = (float4*)g_formation_probe.output;
  output[5 * id.x] = float4(p.bed, p.macro, float(b.index), float(m.index));
  output[5 * id.x + 1] = float4(p.gradient, b.fraction);
  output[5 * id.x + 2] = formation_filtered(beds, p.bed, q.width, p.lateral);
  output[5 * id.x + 3] = float4(p.lateral, formation_filtered(macro, p.macro, q.width, p.lateral).x,
                                b.resistance);
  // The band as the resolve asks for it (T94: its member shortcut is in `formation_band` alone),
  // over a footprint along the gradient that makes the box `width` wide where that is over soft_m.
  const float3 along = p.gradient * (0.25 * q.width / max(dot(p.gradient, p.gradient), 1e-12));
  output[5 * id.x + 4] = formation_band(address, p, along, float3(0.0, 0.0, 0.0));
}
