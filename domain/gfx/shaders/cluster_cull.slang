// GPU cluster culling and LOD selection (docs/plan/04-renderer.md §4.3): one thread per
// cluster, frustum test on the cluster sphere, then the DAG cut test on the two error bounds
// from domain/geometry/cluster_lod.h (own error at or under the threshold, parent error over
// it). Survivors append their index to the visible list, and the same atomic is the group
// count of the indirect mesh-task draw that follows, so no CPU round trip sits between culling
// and drawing.

struct ClusterDesc {
  uint vertex_offset;
  uint triangle_offset;
  uint vertex_count;
  uint triangle_count;
  float3 center;
  float radius;
};

// Mirrors geometry::ClusterLodDesc (48 bytes).
struct ClusterLodDesc {
  float4 own;     // xyz center, w radius
  float4 parent;
  float own_error;
  float parent_error;
  uint level;
  uint group;
};

// Read through a device address; mirrors gfx::CullParams (176 bytes).
struct CullParams {
  float4 planes[6];     // inward-facing, normalized: dot(n, p) + w >= 0 inside
  float4 camera;        // xyz position, w = znear
  float4 lod;           // x = proj_scale, y = threshold_px, z = lod enabled, w = frustum enabled
  uint cluster_count;
  uint plane_count;
  uint pad0;
  uint pad1;
  uint64_t clusters;    // ClusterDesc[]
  uint64_t lods;        // ClusterLodDesc[]
  uint64_t visible;     // uint[cluster_count]
  uint64_t draw_args;   // uint3: group counts of vkCmdDrawMeshTasksIndirectEXT; x counts survivors
};

struct CullPush {
  uint64_t params;
};

[[vk::push_constant]] ConstantBuffer<CullPush> g_push;

static const float k_terminal_error = 3.402823466e+38;

float projected_error(float4 sphere, float error, float3 camera, float znear, float proj_scale) {
  if (error >= k_terminal_error) return k_terminal_error;
  const float distance = length(sphere.xyz - camera) - sphere.w;
  return error / max(distance, znear) * proj_scale;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cull_main(uint3 thread_id : SV_DispatchThreadID) {
  CullParams* params = (CullParams*)g_push.params;
  const uint index = thread_id.x;
  if (index >= params->cluster_count) return;

  ClusterDesc* clusters = (ClusterDesc*)params->clusters;
  const ClusterDesc cluster = clusters[index];

  if (params->lod.w > 0.0) {
    for (uint p = 0; p < params->plane_count; ++p) {
      const float4 plane = params->planes[p];
      if (dot(plane.xyz, cluster.center) + plane.w < -cluster.radius) return;
    }
  }
  if (params->lod.z > 0.0) {
    ClusterLodDesc* lods = (ClusterLodDesc*)params->lods;
    const ClusterLodDesc lod = lods[index];
    const float own = projected_error(lod.own, lod.own_error, params->camera.xyz, params->camera.w, params->lod.x);
    const float parent =
        projected_error(lod.parent, lod.parent_error, params->camera.xyz, params->camera.w, params->lod.x);
    if (!(own <= params->lod.y && parent > params->lod.y)) return;
  }

  Atomic<uint>* count = (Atomic<uint>*)params->draw_args;
  const uint slot = count->add(1);
  uint* visible = (uint*)params->visible;
  visible[slot] = index;
}
