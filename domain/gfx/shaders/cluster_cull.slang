// GPU cluster culling and LOD selection (docs/plan/04-renderer.md §4.3): one thread per
// cluster, frustum test on the cluster sphere, the DAG cut test on the two error bounds from
// domain/geometry/cluster_lod.h (own error at or under the threshold, parent error over it),
// and optionally an occlusion test against a Hi-Z pyramid of the farthest depth. Survivors
// append their index to a visible list, and the same atomic is the group count of the indirect
// draw that follows, so no CPU round trip sits between culling and drawing.
//
// Two-pass occlusion culling: pass 1 considers the clusters drawn last frame and tests them
// against last frame's Hi-Z; pass 2 considers everything pass 1 did not draw and tests it
// against the Hi-Z rebuilt from pass 1's depth. Both passes mark what they draw in `flags`,
// which becomes next frame's `prev_flags`. Pass 0 is a single pass without occlusion.

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
  float4 own;  // xyz center, w radius
  float4 parent;
  float own_error;
  float parent_error;
  uint level;
  uint group;
};

// Read through a device address; mirrors gfx::CullParams (376 bytes).
struct CullParams {
  float4 planes[6];  // inward-facing, normalized: dot(n, p) + w >= 0 inside
  float4 camera;     // xyz position, w = znear
  float4 lod;        // x = proj_scale, y = threshold_px, z = lod enabled, w = frustum enabled
  // x = projected cluster diameter in pixels below which a cluster goes to the software
  // rasterizer; y = mode: 0 everything hardware, 1 everything software, 2 split by x
  float4 raster;
  uint cluster_count;
  uint plane_count;
  uint pad0;
  uint pad1;
  uint64_t clusters;    // ClusterDesc[]
  uint64_t lods;        // ClusterLodDesc[]
  uint64_t visible;     // uint[cluster_count]: hardware-rasterized survivors
  uint64_t draw_args;   // uint3: group counts of vkCmdDrawMeshTasksIndirectEXT; x counts survivors
  uint64_t sw_visible;  // uint[cluster_count]: software-rasterized survivors
  uint64_t sw_args;     // uint3: group counts of vkCmdDispatchIndirect for the software pass
  // Occlusion culling.
  column_major float4x4 view_proj;
  uint64_t hiz;         // float[]: farthest reversed-Z depth per texel, mips back to back; 0 disables
  uint64_t prev_flags;  // uint[cluster_count]: drawn last frame (pass 1 candidates)
  uint64_t flags;       // uint[cluster_count]: drawn this frame (written by passes 1 and 2)
  uint hiz_width;       // mip 0 size
  uint hiz_height;
  uint hiz_mips;
  uint pass;            // 0 single pass, 1 or 2 as above
  uint hiz_offsets[16]; // element offset of each mip
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

// True when every pixel the sphere's screen rectangle touches holds a surface nearer than the
// sphere's nearest point. Conservative: anything touching the near plane counts as visible.
bool occluded(CullParams* params, float3 center, float radius) {
  float2 lo = float2(1e30, 1e30);
  float2 hi = float2(-1e30, -1e30);
  for (uint i = 0; i < 8; ++i) {
    const float3 corner = center + radius * float3((i & 1) != 0 ? 1.0 : -1.0, (i & 2) != 0 ? 1.0 : -1.0,
                                                   (i & 4) != 0 ? 1.0 : -1.0);
    const float4 clip = mul(params->view_proj, float4(corner, 1.0));
    if (clip.w <= 1e-5) return false;
    const float2 ndc = clip.xy / clip.w;
    lo = min(lo, ndc);
    hi = max(hi, ndc);
  }
  const float3 to_camera = params->camera.xyz - center;
  const float distance = length(to_camera);
  if (distance <= radius) return false;
  const float4 nearest = mul(params->view_proj, float4(center + to_camera * (radius / distance), 1.0));
  if (nearest.w <= 1e-5) return false;
  const float depth = nearest.z / nearest.w;  // reversed Z: larger is nearer
  if (depth >= 1.0) return false;

  const float w = float(params->hiz_width);
  const float h = float(params->hiz_height);
  const int ix0 = int(clamp(floor((lo.x * 0.5 + 0.5) * w), 0.0, w - 1.0));
  const int ix1 = int(clamp(floor((hi.x * 0.5 + 0.5) * w), 0.0, w - 1.0));
  const int iy0 = int(clamp(floor((0.5 - hi.y * 0.5) * h), 0.0, h - 1.0));
  const int iy1 = int(clamp(floor((0.5 - lo.y * 0.5) * h), 0.0, h - 1.0));
  // Pick the mip whose texels are at least a quarter of the rectangle, so the footprint is at
  // most 5x5 texels and stays close to the rectangle (a 2x2 footprint at the next coarser
  // mips would cover up to four times the area and touch far-away depth, culling less).
  const uint size = uint(max(ix1 - ix0, iy1 - iy0)) + 1;
  const uint coarse = size <= 1 ? 0 : firstbithigh(size - 1) + 1;  // 2^coarse >= size
  uint mip = coarse >= 2 ? coarse - 2 : 0;
  mip = min(mip, params->hiz_mips - 1);
  const uint mip_w = (params->hiz_width + (1u << mip) - 1) >> mip;
  const uint mip_h = (params->hiz_height + (1u << mip) - 1) >> mip;
  const uint tx0 = uint(ix0) >> mip;
  const uint tx1 = min(uint(ix1) >> mip, mip_w - 1);
  const uint ty0 = uint(iy0) >> mip;
  const uint ty1 = min(uint(iy1) >> mip, mip_h - 1);
  float* hiz = (float*)params->hiz;
  const uint base = params->hiz_offsets[mip];
  float farthest = 1e30;
  for (uint ty = ty0; ty <= ty1; ++ty) {
    for (uint tx = tx0; tx <= tx1; ++tx) {
      farthest = min(farthest, hiz[base + ty * mip_w + tx]);
    }
  }
  return depth < farthest;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cull_main(uint3 thread_id : SV_DispatchThreadID) {
  CullParams* params = (CullParams*)g_push.params;
  const uint index = thread_id.x;
  if (index >= params->cluster_count) return;

  if (params->pass == 1) {
    uint* prev = (uint*)params->prev_flags;
    if (prev[index] == 0) return;
  } else if (params->pass == 2) {
    uint* flags = (uint*)params->flags;
    if (flags[index] != 0) return;  // drawn in pass 1
  }

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
  if (params->hiz != 0 && occluded(params, cluster.center, cluster.radius)) return;
  if (params->pass != 0) {
    uint* flags = (uint*)params->flags;
    flags[index] = 1;
  }

  // Hardware or software rasterization: by projected size unless the mode forces one path.
  bool software = params->raster.y == 1.0;
  if (params->raster.y == 2.0) {
    const float distance = max(length(cluster.center - params->camera.xyz) - cluster.radius, params->camera.w);
    const float diameter_px = 2.0 * cluster.radius / distance * params->lod.x;
    software = diameter_px < params->raster.x;
  }
  if (software) {
    Atomic<uint>* count = (Atomic<uint>*)params->sw_args;
    const uint slot = count->add(1);
    uint* visible = (uint*)params->sw_visible;
    visible[slot] = index;
  } else {
    Atomic<uint>* count = (Atomic<uint>*)params->draw_args;
    const uint slot = count->add(1);
    uint* visible = (uint*)params->visible;
    visible[slot] = index;
  }
}
