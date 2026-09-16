// The first pixels: a triangle generated from the vertex index, no vertex buffer. In Vulkan
// clip space y points down, so (-1,-1), (1,-1), (-1,1) covers the top-left half of the target.

struct VsOut {
  float4 position : SV_Position;
  float3 color : COLOR;
};

struct TriangleParams {
  float3 color;
  float scale;
};

[[vk::push_constant]] ConstantBuffer<TriangleParams> g_params;

[shader("vertex")]
VsOut vs_main(uint vertex_id : SV_VertexID) {
  const float2 corners[3] = {float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0)};
  VsOut out;
  out.position = float4(corners[vertex_id] * g_params.scale, 0.0, 1.0);
  out.color = g_params.color;
  return out;
}

[shader("fragment")]
float4 fs_main(VsOut in) : SV_Target {
  return float4(in.color, 1.0);
}
