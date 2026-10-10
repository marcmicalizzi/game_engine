// The overlay pass's one pipeline (systems/renderer, `overlay.h`; ADR-0054): 2D triangles in the
// target's pixels, textured and coloured, blended "over" the encoded picture after the frame. The
// vertices and indices are pulled through device addresses, so the pipeline has no vertex input and
// binds no index buffer: draw `index_count` vertices and the vertex stage reads its own index.
//
// The colours are written as they arrive, with no curve: the target already holds the output
// encode's values (display.slang, `display_output`), and an immediate-mode UI's colours and its
// blending are authored in exactly that space. That is right for an SDR target only, which is why
// the renderer refuses an overlay on an HDR one (renderer.md, "The overlay").

[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(2, 0)]] SamplerState g_samplers[];

// Mirrors renderer::OverlayDrawParams (48 bytes, the push block).
struct OverlayDrawParams {
  uint64_t vertices;  // renderer::OverlayVertex[]: five words each (x, y, u, v, rgba)
  uint64_t indices;   // uint[]
  float2 scale;       // 2 / the target's size
  float2 translate;   // -1, -1
  uint first_index;
  uint vertex_offset;
  uint texture;  // a bindless sampled-image slot
  uint sampler;  // a bindless sampler slot
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
  return in.color * g_textures[g_params.texture].Sample(g_samplers[g_params.sampler], in.uv);
}
