#define MATERIAL_TERRAIN 1
// Every R136 read rule built in, the four the engine ships without included, so their arithmetic
// is held here whichever the engine's default selects.
#define TERRAIN_FOOTPRINT_LEVERS 31
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

[shader("compute")]
[numthreads(64, 1, 1)]
void filter_main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_probe.count) return;
  const float r = float(id.x % 8) / 8.0;
  const float loss = float(id.x / 8) / 8.0;
  const float scale = float(id.x % 3) * 0.5;
  const float v = brdf_normal_variance(loss, scale);
  const float g = brdf_geometric_alpha2(float3(r * 0.1, 0, 0), float3(0, loss * 0.2, 0));
  ((float4*)g_probe.output)[id.x] = float4(v, g, brdf_filter_roughness(r, v, g),
                                        brdf_filter_roughness(r, 0, 0));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void footprint_main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_probe.count) return;
  ResolveMaterial material = (ResolveMaterial)0;
  material.albedo = float4(1, 1, 1, 0.5);
  material.albedo_texture = k_no_texture;
  material.metallic_roughness_texture = k_no_texture;
  material.emissive_texture = k_no_texture;
  material.normal_texture = g_probe.pad;
  material.normal_scale = 1;
  material.flags = k_material_mipped | k_material_normal_variance;
  const float lod = 8.0 * float(id.x) / float(g_probe.count - 1);
  const float rho = exp2(lod) / 256.0;
  const float2 dx = float2(rho, 0), dy = float2(0, rho);
  const float loss = g_textures[NonUniformResourceIndex(g_probe.pad + 1)].SampleGrad(
      g_samplers[0], float2(0.5), dx, dy).r;
  const float filtered = sample_material_grad(material, float2(0.5), dx, dy).roughness;
  material.flags = k_material_mipped;
  const float legacy = sample_material_grad(material, float2(0.5), dx, dy).roughness;
  const float mix = terrain_filter_combination(0.0625, loss, float3(loss, 0, 0));
  ((float4*)g_probe.output)[id.x] = float4(filtered, loss, legacy, mix);
}

// R99's responses (renderer.md, "Sand and snow"): the snow's albedo by grain, the lobe's weight as
// the metallic word, Lekner and Dorf's wet darkening, and the glint at a footprint, at the far eye
// probe_main uses, each against its double mirror in terrain_material_tests.cpp; five float4 a
// point, the glint's two levels (R125) in the last two.
[shader("compute")]
[numthreads(64, 1, 1)]
void response_main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_probe.count) return;
  const int i = int(id.x);
  FrameEye eye = (FrameEye)0;
  eye.cell = int3((i & 1) == 0 ? 6548 : -6548, 0, (i & 2) == 0 ? 1562500 : -3);
  eye.local = float3(63.999, 0.125, -0.25);
  eye.residual = float3(0.00001, 0, 0);
  const float3 point = float3(float(i) * 0.0173, -float(i) * 0.25, float(i) * -0.0291);
  const float3 n = normalize(float3(float(i % 3) - 1.0, 2.5, float(i % 5) - 2.0));
  const float grain = 0.03 + float(i % 64) * 0.078;
  const float a = float(i % 17) / 16.0;
  const float w = 1.0 + float(i % 7) * 0.5;
  const float footprint = 0.3 + float(i) * 0.137;
  const float density = (i % 4) == 0 ? 1.0 : 0.6;
  const float alpha = 0.1 + float(i % 9) * 0.07;
  const TerrainGlint g =
      terrain_glint(eye, point, n, footprint, density, alpha, (i % 2) == 0 ? 0x534E4F57u : 0x53414E44u);
  float4* output = (float4*)g_probe.output;
  output[5 * i] = float4(terrain_snow_albedo(grain), terrain_lobe_metallic(a, w));
  output[5 * i + 1] = float4(terrain_wet_darkening(float3(a, 0.5 * a, 0.1)), 0.0);
  output[5 * i + 2] = float4(g.normal, g.lit ? 1.0 : 0.0);
  output[5 * i + 3] = float4(g.chance, g.blend, float(g.size_mm), float(g.level_lit));
  output[5 * i + 4] = float4(g.chance0, g.draw0, g.chance1, g.draw1);
}

// S99's wet response (wet_ground.slang, terrain_material.slang's `terrain_wet`) over a grid of
// albedo (17) x saturation (11) x porosity (6) x film (3), three float4 a point, against
// wet_reference.h in wet_ground_tests.cpp: the wet sample's albedo and roughness, the film's
// weight, the fringe at a height, and whether s = 0 gave the dry sample's bits back.
[shader("compute")]
[numthreads(64, 1, 1)]
void wet_main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_probe.count) return;
  const uint i = id.x;
  const float a = float(i % 17u) / 16.0;
  const float s = float((i / 17u) % 11u) / 10.0;
  const float porosity = float((i / 187u) % 6u) / 5.0;
  const float film = float((i / 1122u) % 3u) / 2.0;
  MaterialSample dry = (MaterialSample)0;
  dry.albedo = float3(a, 0.5 * a, 0.1 + 0.8 * a * a);
  dry.roughness = 0.04 + float(i % 13u) * 0.075;
  dry.metallic = 0.0;
  const MaterialSample wet = terrain_wet(dry, s, film, porosity);
  const MaterialSample same = terrain_wet(dry, 0.0, film, porosity);
  const float h = (float(i % 23u) - 3.0) * 0.05;
  const float fringe_m = 0.025 + float(i % 5u) * 0.25;
  float4* output = (float4*)g_probe.output;
  output[3 * i] = float4(wet.albedo, wet.roughness);
  output[3 * i + 1] = float4(wet_ground_film(s, film), wet_ground_fringe(h, fringe_m),
                             wet_ground_roughness(dry.roughness, s, 0.0), 0.0);
  const bool identical = all(asuint(same.albedo) == asuint(dry.albedo)) &&
                         asuint(same.roughness) == asuint(dry.roughness);
  output[3 * i + 2] = float4(identical ? 1.0 : 0.0, 0.0, 0.0, 0.0);
}

// R136's footprint rules (renderer.md, "What a far pixel reads"): the projections kept on a plane
// tilted from flat to vertical in 1/100 degree steps, past two cells (far 1) and under one (far 0),
// in the first `count` points; then, in 512 more, the one-tile, flat-maps and layer-weight ramps
// over their inputs, against their double mirror in terrain_material_tests.cpp.
[shader("compute")]
[numthreads(64, 1, 1)]
void reads_main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_probe.count + 512u) return;
  float4* output = (float4*)g_probe.output;
  if (id.x < g_probe.count) {
    const float theta = 1.57079632679 * float(id.x) / float(g_probe.count - 1u);
    const float3 n = float3(sin(theta), cos(theta), 0.0);
    // sample_terrain_material's triplanar weights.
    float3 axes = n * n;
    axes *= axes;
    axes /= max(axes.x + axes.y + axes.z, 1e-12);
    const float3 far = terrain_kept_projections(axes, 1.0);
    const float3 near = terrain_kept_projections(axes, 0.0);
    output[id.x] = float4(far.x, far.y, near.x, near.y);
    return;
  }
  const uint j = id.x - g_probe.count;
  const float t = float(j) / 128.0;  // 0 to 4
  output[id.x] = float4(terrain_one_tile(t), terrain_flat_maps(0.25 * t),
                        terrain_far_layer_weight(t / 128.0, 1.0),
                        terrain_far_layer_weight(t / 128.0, 0.0));
}

// R136's tiles by weight: the lattice's weights and gain at `count` points of a 64 by 64 grid over
// one cell's skewed square, then the same points' weights and gain once a tile under 1/128 has
// been ramped out (`terrain_hex_kept`), two float4 a point.
[shader("compute")]
[numthreads(64, 1, 1)]
void kept_main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_probe.count) return;
  const float2 f = (float2(float(id.x % 64u), float(id.x / 64u)) + 0.5) / 64.0;
  const HexTiles hex = hex_tiles(17u, 4u, f, 0x9e3779b9u, 0.0);
  const HexTiles kept = terrain_hex_kept(hex);
  float4* output = (float4*)g_probe.output;
  output[2 * id.x] = float4(hex.weight, hex.gain);
  output[2 * id.x + 1] = float4(kept.weight, kept.gain);
}
