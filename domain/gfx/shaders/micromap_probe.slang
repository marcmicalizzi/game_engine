// The traversal half of R120's record-level test (rt_cluster_tests.cpp, "a CLAS's opacity micromap
// decides the traversal"): one ray per pixel of a `size` x `size` grid, from (0, 3, 2) through the
// grid's point on y = 0 over [-1.5, 1.5]^2, against one top-level structure of the bindless set,
// writing 1 where a triangle was committed. Opaque geometry and RAY_FLAG_CULL_NON_OPAQUE, so a
// micro-triangle the traversal reads as transparent is no hit at all and an opaque one is.

[[vk::binding(3, 0)]] RaytracingAccelerationStructure g_scenes[];

struct MicromapProbeParams {
  uint64_t output;  // uint per pixel
  uint scene;       // the top-level structure's bindless slot
  uint size;
};
[[vk::push_constant]] ConstantBuffer<MicromapProbeParams> g_params;

[shader("compute")]
[numthreads(8, 8, 1)]
void probe_eye(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_params.size || id.y >= g_params.size) return;
  const float2 at = (float2(id.xy) + 0.5) / float(g_params.size) * 3.0 - 1.5;
  RayDesc ray;
  ray.Origin = float3(0.0, 3.0, 2.0);
  ray.Direction = normalize(float3(at.x, 0.0, at.y) - ray.Origin);
  ray.TMin = 0.0;
  ray.TMax = 100.0;
  RayQuery<RAY_FLAG_CULL_NON_OPAQUE> query;
  query.TraceRayInline(g_scenes[g_params.scene], RAY_FLAG_CULL_NON_OPAQUE, 0xff, ray);
  while (query.Proceed()) {
  }
  uint* out = (uint*)g_params.output;
  out[id.y * g_params.size + id.x] = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 1u : 0u;
}
