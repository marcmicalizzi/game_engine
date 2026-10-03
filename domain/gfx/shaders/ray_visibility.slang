// Primary visibility by ray query (docs/plan/04-renderer.md §4.4; experiment E2's image match):
// one ray per pixel center through the same view-projection the rasterizer used, against a
// top-level structure with one instance per scene instance whose bottom level has one geometry
// per visible cluster of that instance, so a hit's GeometryIndex is the **visible index** of the
// pair and PrimitiveIndex the triangle. The output is the visibility buffer's 64-bit word
// (reversed-Z depth bits << 32 | pair << 8 | triangle), so the raster and ray-traced pictures
// compare word for word: the entry leads to the {instance, cluster} and so to the scene's pair,
// which is what every rasterizer writes (scene.slang, `pair_of`). On the cluster path each CLAS
// record carries its base geometry index, which is already the visible index; on the KHR path the
// geometries of a bottom-level structure start at 0, so `instance_base[InstanceID()]` shifts
// them. Empty pixels write 0.
//
// **The tie rule** (docs/subsystems/gfx.md): the rasterizers settle two surfaces at exactly one
// depth with the 64-bit atomic max, so the larger (pair, triangle) wins. A ray query settles them by
// traversal order — the first of the two hits the traversal meets is committed and the second is
// not closer — and that order is the acceleration structure's. So once the closest hit is known the
// pass looks again over exactly that distance with every candidate reported, and keeps the larger
// key, which is the rasterizers' answer. The second look is bounded by the hit itself: only boxes
// that contain the hit point are entered, and on a surface with nothing coincident it finds the
// one triangle it already has.
//
// **Why two traversals and not one** (docs/experiments/visible-order.md). The second look is a
// second descent per hit pixel, which cost the heightfield's trace pass 71-74% on the RTX 5090 and
// the helmet grid's 7-10%. One traversal with every candidate forced non-opaque and the loop
// committing the nearest itself was 16% — and wrong: a commit shrinks the ray to *exclude* its own
// distance on that driver, so a coincident candidate met after the first is never reported and
// the traversal order decides again (the tie case lost all 2,857 pixels). The same loop without
// committing is correct but reports everything behind the surface too: 52-56% on the heightfield
// and 26-51% on the helmet grid, and it grows with the depth complexity a ray passes through,
// where the second look's cost is bounded by one descent to one point.

#include "scene.slang"
#include "view_ray.slang"

[[vk::binding(3, 0)]] RaytracingAccelerationStructure g_scenes[];

// Read through a device address; mirrors gfx::RayVisibilityParams (208 bytes).
struct RayVisibilityParams {
  column_major float4x4 view_proj;
  column_major float4x4 clip_to_ray;  // clip to the world direction of the eye's ray (view_ray.slang)
  float4 camera;    // xyz position
  uint64_t output;         // uint64_t[width * height]
  uint64_t instance_base;  // uint[instance_count]: the visible index of each instance's first
                           // geometry; 0 when GeometryIndex is already the visible index
  uint width;
  uint height;
  uint scene;        // bindless slot of the top-level structure
  uint caster_base;  // the first shadow caster's visible entry; ~0 when there are none
  uint64_t visible;    // uint2[]: {instance, cluster} per entry; 0 is {0, entry}
  uint64_t instances;  // InstanceDesc[]
  uint64_t meshes;     // MeshDesc[]
  uint64_t pad;
};

struct RayPush {
  uint64_t params;
};

[[vk::push_constant]] ConstantBuffer<RayPush> g_push;

// The visible entry a hit names.
uint hit_entry(RayVisibilityParams* params, uint geometry, uint instance_id) {
  if (params->instance_base == 0) return geometry;
  uint* base = (uint*)params->instance_base;
  return geometry + base[instance_id];  // the scene instance
}

// The id a hit writes: the scene's pair of its entry, and the triangle.
uint hit_key(RayVisibilityParams* params, uint entry, uint primitive) {
  const uint2 pair = visible_entry(params->visible, entry);
  InstanceDesc* instances = (InstanceDesc*)params->instances;
  const InstanceDesc instance = instances[pair.x];
  MeshDesc* meshes = (MeshDesc*)params->meshes;
  return (pair_of(instance, meshes[instance.mesh], pair.y) << 8) | (primitive & 0xff);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void trace_main(uint3 id : SV_DispatchThreadID) {
  RayVisibilityParams* params = (RayVisibilityParams*)g_push.params;
  if (id.x >= params->width || id.y >= params->height) return;
  // Pixel center to clip space: row 0 is the top of the image, clip y points up.
  const float2 ndc = float2((float(id.x) + 0.5) / float(params->width) * 2.0 - 1.0,
                            1.0 - (float(id.y) + 0.5) / float(params->height) * 2.0);
  // The direction from the camera's rotation and projection alone, never a world point less the
  // eye, which far from the origin turned every ray by a pixel or more (view_ray.slang).
  const float3 origin = params->camera.xyz;
  const float3 direction = view_ray_direction(params->clip_to_ray, ndc);

  RayDesc ray;
  ray.Origin = origin;
  ray.Direction = direction;
  ray.TMin = 0.0;
  ray.TMax = 1e30;
  // Everything the picture draws is built opaque. The one non-opaque geometry is a **shadow
  // caster**: a cluster the cull pass's cone test kept out of the picture but not out of the
  // shadows (clas_records.slang). Culling non-opaque geometry is what keeps it out of this ray,
  // so the traced picture stays the rasterized one word for word.
  RayQuery<RAY_FLAG_CULL_NON_OPAQUE> query;
  query.TraceRayInline(g_scenes[params->scene], RAY_FLAG_CULL_NON_OPAQUE, 0xff, ray);
  while (query.Proceed()) {
  }

  uint64_t value = 0;
  if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
    const float t = query.CommittedRayT();
    const float3 p = origin + direction * t;
    const float4 clip = mul(params->view_proj, float4(p, 1.0));
    const float depth = clip.z / clip.w;  // what the rasterizer's SV_Position.z holds
    uint key = hit_key(params,
                       hit_entry(params, query.CommittedGeometryIndex(),
                                 query.CommittedInstanceID()),
                       query.CommittedPrimitiveIndex());
    // The second look, at exactly the committed distance (the interval is widened by an ulp's
    // worth each way so that an implementation whose bounds are exclusive still reports it, and
    // the candidates are then held to the distance exactly). Forcing every candidate non-opaque is
    // what makes the traversal report them rather than commit one; it also reports the shadow
    // casters the first query culled, which the entry range leaves out again.
    RayDesc tie = ray;
    tie.TMin = t * 0.999999;
    tie.TMax = t * 1.000001;
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> scan;
    scan.TraceRayInline(g_scenes[params->scene], RAY_FLAG_FORCE_NON_OPAQUE, 0xff, tie);
    while (scan.Proceed()) {
      if (scan.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
      if (scan.CandidateTriangleRayT() != t) continue;
      const uint entry = hit_entry(params, scan.CandidateGeometryIndex(),
                                   scan.CandidateInstanceID());
      if (entry >= params->caster_base) continue;
      key = max(key, hit_key(params, entry, scan.CandidatePrimitiveIndex()));
    }
    value = (uint64_t(asuint(depth)) << 32) | uint64_t(key);
  }
  uint64_t* output = (uint64_t*)params->output;
  output[id.y * params->width + id.x] = value;
}
