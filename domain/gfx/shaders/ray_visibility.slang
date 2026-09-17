// Primary visibility by ray query (docs/plan/04-renderer.md §4.4; experiment E2's image match):
// one ray per pixel center through the same view-projection the rasterizer used, against a
// top-level structure whose bottom level has one geometry per cluster of the current cut, so a
// hit's GeometryIndex is the cut index and PrimitiveIndex the triangle. The output is the
// visibility buffer's 64-bit word (reversed-Z depth bits << 32 | cluster << 8 | triangle), so
// the raster and ray-traced pictures compare word for word. Empty pixels write 0.

[[vk::binding(3, 0)]] RaytracingAccelerationStructure g_scenes[];

// Read through a device address; mirrors gfx::RayVisibilityParams (176 bytes).
struct RayVisibilityParams {
  column_major float4x4 view_proj;
  column_major float4x4 inv_view_proj;
  float4 camera;    // xyz position
  uint64_t output;  // uint64_t[width * height]
  uint64_t cut;     // uint[]: cluster index per BLAS geometry; 0 when geometry index == cluster
  uint width;
  uint height;
  uint scene;  // bindless slot of the top-level structure
  uint pad;
};

struct RayPush {
  uint64_t params;
};

[[vk::push_constant]] ConstantBuffer<RayPush> g_push;

[shader("compute")]
[numthreads(8, 8, 1)]
void trace_main(uint3 id : SV_DispatchThreadID) {
  RayVisibilityParams* params = (RayVisibilityParams*)g_push.params;
  if (id.x >= params->width || id.y >= params->height) return;
  // Pixel center to clip space: row 0 is the top of the image, clip y points up.
  const float2 ndc = float2((float(id.x) + 0.5) / float(params->width) * 2.0 - 1.0,
                            1.0 - (float(id.y) + 0.5) / float(params->height) * 2.0);
  const float4 ahead = mul(params->inv_view_proj, float4(ndc, 0.5, 1.0));  // any depth in front
  const float3 origin = params->camera.xyz;
  const float3 direction = normalize(ahead.xyz / ahead.w - origin);

  RayDesc ray;
  ray.Origin = origin;
  ray.Direction = direction;
  ray.TMin = 0.0;
  ray.TMax = 1e30;
  RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
  query.TraceRayInline(g_scenes[params->scene], RAY_FLAG_FORCE_OPAQUE, 0xff, ray);
  while (query.Proceed()) {
  }

  uint64_t value = 0;
  if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
    const float3 p = origin + direction * query.CommittedRayT();
    const float4 clip = mul(params->view_proj, float4(p, 1.0));
    const float depth = clip.z / clip.w;  // what the rasterizer's SV_Position.z holds
    uint cluster = query.CommittedGeometryIndex();
    if (params->cut != 0) {
      uint* cut = (uint*)params->cut;
      cluster = cut[cluster];
    }
    const uint triangle = query.CommittedPrimitiveIndex() & 0xff;
    value = (uint64_t(asuint(depth)) << 32) | uint64_t((cluster << 8) | triangle);
  }
  uint64_t* output = (uint64_t*)params->output;
  output[id.y * params->width + id.x] = value;
}
