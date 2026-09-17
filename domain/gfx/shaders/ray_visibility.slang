// Primary visibility by ray query (docs/plan/04-renderer.md §4.4; experiment E2's image match):
// one ray per pixel center through the same view-projection the rasterizer used, against a
// top-level structure with one instance per scene instance whose bottom level has one geometry
// per visible cluster of that instance, so a hit's GeometryIndex is the **visible index** of the
// pair and PrimitiveIndex the triangle. The output is the visibility buffer's 64-bit word
// (reversed-Z depth bits << 32 | visible_index << 8 | triangle), so the raster and ray-traced
// pictures compare word for word. On the cluster path each CLAS record carries its base geometry
// index, which is already the visible index; on the KHR path the geometries of a bottom-level
// structure start at 0, so `instance_base[InstanceID()]` shifts them. Empty pixels write 0.

[[vk::binding(3, 0)]] RaytracingAccelerationStructure g_scenes[];

// Read through a device address; mirrors gfx::RayVisibilityParams (176 bytes).
struct RayVisibilityParams {
  column_major float4x4 view_proj;
  column_major float4x4 inv_view_proj;
  float4 camera;    // xyz position
  uint64_t output;         // uint64_t[width * height]
  uint64_t instance_base;  // uint[instance_count]: the visible index of each instance's first
                           // geometry; 0 when GeometryIndex is already the visible index
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
    uint visible_index = query.CommittedGeometryIndex();
    if (params->instance_base != 0) {
      uint* base = (uint*)params->instance_base;
      visible_index += base[query.CommittedInstanceID()];  // the scene instance
    }
    const uint triangle = query.CommittedPrimitiveIndex() & 0xff;
    value = (uint64_t(asuint(depth)) << 32) | uint64_t((visible_index << 8) | triangle);
  }
  uint64_t* output = (uint64_t*)params->output;
  output[id.y * params->width + id.x] = value;
}
