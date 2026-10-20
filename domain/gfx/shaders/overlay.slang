// The overlay pass's pipelines (systems/renderer, `overlay.h`; ADR-0054): 2D triangles in the
// target's pixels, textured and coloured, blended "over" the picture after the frame. The vertices
// and indices are pulled through device addresses, so the pipeline has no vertex input and binds
// no index buffer: draw `index_count` vertices and the vertex stage reads its own index.
//
// **What a fragment writes depends on where it lands** (`OverlayDrawParams::encoding`; renderer.md,
// "The overlay"). On an SDR target the colours are written as they arrive, with no curve: the
// target already holds the output encode's values (display.slang, `display_output`), and an
// immediate-mode UI's colours and its blending are authored in exactly that space — the overlay's
// picture before HDR existed, to the bit. For an HDR frame the colours are decoded from that SDR
// transfer to linear light, where 1.0 is the UI's white: into the UI layer the frame's encode
// composites at paper white (`k_overlay_layer`), or, for a frame with no linear path, encoded here
// at paper white for the HDR target itself (`k_overlay_pq`, `k_overlay_scrgb`, `k_overlay_linear`).

#include "display.slang"

[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(2, 0)]] SamplerState g_samplers[];

static const uint k_overlay_encoded = 0;  // renderer::OverlayEncoding::Encoded
static const uint k_overlay_layer = 1;    // ::Layer
static const uint k_overlay_pq = 2;       // ::Pq
static const uint k_overlay_scrgb = 3;    // ::ScRgb
static const uint k_overlay_linear = 4;   // ::Linear

// Mirrors renderer::OverlayDrawParams (56 bytes, the push block).
struct OverlayDrawParams {
  uint64_t vertices;  // renderer::OverlayVertex[]: five words each (x, y, u, v, rgba)
  uint64_t indices;   // uint[]
  float2 scale;       // 2 / the target's size
  float2 translate;   // -1, -1
  uint first_index;
  uint vertex_offset;
  uint texture;  // a bindless sampled-image slot
  uint sampler;  // a bindless sampler slot
  uint encoding;  // k_overlay_*
  float paper_white_nits;
};

[[vk::push_constant]] ConstantBuffer<OverlayDrawParams> g_params;

struct VsOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
  float4 color : COLOR0;
};

[shader("vertex")]
VsOut vs_overlay(uint vertex_id : SV_VertexID) {
  const uint index = ((uint*)g_params.indices)[g_params.first_index + vertex_id];
  // Read as words rather than as a struct, so the 20-byte stride is the C++ side's whatever layout
  // rule a pointer's pointee would otherwise be given.
  uint* v = (uint*)g_params.vertices + (g_params.vertex_offset + index) * 5;
  const float2 position = float2(asfloat(v[0]), asfloat(v[1]));
  const uint rgba = v[4];
  VsOut out;
  // Pixels, y down, onto Vulkan's clip space, which is y down too: the pass sets an unflipped
  // viewport over the whole target.
  out.position = float4(position * g_params.scale + g_params.translate, 0.0, 1.0);
  out.uv = float2(asfloat(v[2]), asfloat(v[3]));
  out.color = float4(float(rgba & 0xffu), float((rgba >> 8) & 0xffu), float((rgba >> 16) & 0xffu),
                     float(rgba >> 24)) / 255.0;
  return out;
}

[shader("fragment")]
float4 fs_overlay(VsOut in) : SV_Target {
  const float4 c = in.color * g_textures[g_params.texture].Sample(g_samplers[g_params.sampler], in.uv);
  if (g_params.encoding == k_overlay_encoded) return c;
  // The UI's colour in linear light, 1.0 its white: the inverse of the SDR transfer the colour was
  // authored through (display.slang's `display_encode`, a 1/2.2 power), so a colour stands at the
  // same place under HDR's paper white as under SDR's white.
  const float3 linear_color = pow(saturate(c.rgb), 2.2);
  // Into the UI layer: straight alpha, which the hardware blends "over" in linear light (the layer
  // is sRGB-stored) into a layer cleared to zero, leaving premultiplied colour and coverage there.
  if (g_params.encoding == k_overlay_layer) return float4(linear_color, c.a);
  // Straight onto an HDR target with no linear path under it: encoded here, at paper white.
  if (g_params.encoding == k_overlay_linear) return float4(linear_color, c.a);
  const float3 nits = linear_color * g_params.paper_white_nits;
  if (g_params.encoding == k_overlay_scrgb) return float4(nits / 80.0, c.a);
  return float4(pq_encode(bt709_to_bt2020(nits)), c.a);
}
