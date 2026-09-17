// Fullscreen resolve of the 64-bit visibility buffer into a color target for display and
// captures: id-hashed cluster colors (mode 0), a per-triangle shade on top (mode 1), or the
// reversed-Z depth as gray (mode 2). Empty pixels (value 0: depth 0 is the far plane and id 0
// never occurs there) take the sky color. The material resolve that replaces this later reads
// the same buffer.

struct ResolveParams {
  float4 sky;  // first, so the 16-byte member needs no padding in either language
  uint64_t visibility;
  uint width;
  uint height;
  uint mode;
  uint pad;
};

[[vk::push_constant]] ConstantBuffer<ResolveParams> g_params;

struct FullscreenOut {
  float4 position : SV_Position;
};

[shader("vertex")]
FullscreenOut vs_fullscreen(uint vertex_id : SV_VertexID) {
  // One triangle covering the viewport: (-1,-1), (3,-1), (-1,3).
  FullscreenOut out;
  const float x = vertex_id == 1 ? 3.0 : -1.0;
  const float y = vertex_id == 2 ? 3.0 : -1.0;
  out.position = float4(x, y, 0.0, 1.0);
  return out;
}

float3 id_color(uint id) {
  const uint cluster = id >> 8;
  const uint h = cluster * 2654435761u;
  return float3((h >> 0) & 255, (h >> 8) & 255, (h >> 16) & 255) / 255.0;
}

[shader("fragment")]
float4 fs_resolve(FullscreenOut in) : SV_Target {
  const uint2 pixel = uint2(in.position.xy);
  if (pixel.x >= g_params.width || pixel.y >= g_params.height) return g_params.sky;
  uint64_t* visibility = (uint64_t*)g_params.visibility;
  const uint64_t value = visibility[pixel.y * g_params.width + pixel.x];
  if (value == 0) return g_params.sky;
  const uint id = uint(value & 0xffffffffu);
  const float depth = asfloat(uint(value >> 32));
  if (g_params.mode == 2) return float4(depth, depth, depth, 1.0);
  float3 color = id_color(id);
  if (g_params.mode == 1) color *= 0.7 + 0.3 * frac(float(id & 0xff) * 0.6180339887);
  return float4(color, 1.0);
}
