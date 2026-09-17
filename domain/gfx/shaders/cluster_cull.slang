// GPU cluster culling and LOD selection over a scene of instances (docs/plan/04-renderer.md
// §4.2, §4.3): one thread per (instance, cluster) **pair**, every test done in the instance's
// frame. The thread finds its instance by binary search over the instances' `first_pair` prefix
// sum, reads the cluster of that instance's mesh, moves the bounding sphere, the normal cone,
// and the two LOD spheres into world space, and runs the same frustum, backface, DAG-cut, and
// Hi-Z occlusion tests the single-mesh pass ran. Survivors append `{instance, cluster}` to a
// visible list, and the same atomic is the group count of the indirect draw that follows, so no
// CPU round trip sits between culling and drawing.
//
// Two-pass occlusion culling: pass 1 considers the pairs drawn last frame and tests them
// against last frame's Hi-Z; pass 2 considers everything pass 1 did not draw and tests it
// against the Hi-Z rebuilt from pass 1's depth. Both passes mark what they draw in `flags`,
// which becomes next frame's `prev_flags`; both arrays are indexed by pair. Pass 0 is a single
// pass without occlusion.

#include "scene.slang"

// Mirrors geometry::ClusterLodDesc (48 bytes).
struct ClusterLodDesc {
  float4 own;  // xyz center, w radius
  float4 parent;
  float own_error;
  float parent_error;
  uint level;
  uint group;
};

// Read through a device address; mirrors gfx::CullParams (400 bytes).
struct CullParams {
  float4 planes[6];  // inward-facing, normalized: dot(n, p) + w >= 0 inside
  float4 camera;     // xyz position, w = znear
  float4 lod;        // x = proj_scale, y = threshold_px, z = lod enabled, w = frustum enabled
  // x = projected cluster diameter in pixels below which a cluster goes to the software
  // rasterizer; y = mode: 0 everything hardware, 1 everything software, 2 split by x
  float4 raster;
  uint cluster_count;
  uint plane_count;
  uint count_index;  // u32 of draw_args that counts hardware survivors (0 mesh tasks, 1 vkCmdDrawIndirect)
  uint cone_cull;    // 1: reject clusters whose normal cone faces away from the camera
  uint64_t clusters;    // ClusterDesc[]
  uint64_t lods;        // ClusterLodDesc[]
  uint64_t visible;     // uint2[pair_count]: {instance, cluster} of the hardware survivors
  uint64_t draw_args;   // uint3: group counts of vkCmdDrawMeshTasksIndirectEXT; x counts survivors
  uint64_t sw_visible;  // uint2[pair_count]: the software-rasterized survivors
  uint64_t sw_args;     // uint3: group counts of vkCmdDispatchIndirect for the software pass
  // Occlusion culling.
  column_major float4x4 view_proj;
  uint64_t hiz;         // float[]: farthest reversed-Z depth per texel, mips back to back; 0 disables
  uint64_t prev_flags;  // uint[pair_count]: drawn last frame (pass 1 candidates)
  uint64_t flags;       // uint[pair_count]: drawn this frame (written by passes 1 and 2)
  uint hiz_width;       // mip 0 size
  uint hiz_height;
  uint hiz_mips;
  uint pass;            // 0 single pass, 1 or 2 as above
  uint hiz_offsets[16]; // element offset of each mip
  uint64_t instances;   // InstanceDesc[instance_count]
  uint64_t meshes;      // MeshDesc[]
  uint instance_count;
  uint pair_count;
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

// meshoptimizer's cone test on the snorm8 packing (geometry::cluster_backfacing is the CPU
// reference): every triangle faces away when the direction from the camera to the apex lies
// within the cone. A cutoff of 1 marks a cluster without a usable cone. `apex` and `axis` are
// the cluster's, already moved into world space by the instance's transform.
bool backfacing(ClusterDesc cluster, float3 apex, float3 axis, float3 camera) {
  const float cutoff = float(int(cluster.cone) >> 24) / 127.0;
  if (cutoff >= 1.0) return false;
  const float3 to_apex = apex - camera;
  const float distance = length(to_apex);
  if (distance <= 1e-12) return false;
  return dot(to_apex * (1.0 / distance), axis) >= cutoff;
}

float3 cone_axis(ClusterDesc cluster) {
  return float3(int(cluster.cone << 24) >> 24, int(cluster.cone << 16) >> 24,
                int(cluster.cone << 8) >> 24) / 127.0;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cull_main(uint3 thread_id : SV_DispatchThreadID) {
  CullParams* params = (CullParams*)g_push.params;
  const uint pair = thread_id.x;
  if (pair >= params->pair_count) return;

  if (params->pass == 1) {
    uint* prev = (uint*)params->prev_flags;
    if (prev[pair] == 0) return;
  } else if (params->pass == 2) {
    uint* flags = (uint*)params->flags;
    if (flags[pair] != 0) return;  // drawn in pass 1
  }

  InstanceDesc* instances = (InstanceDesc*)params->instances;
  const uint instance_index = instance_of_pair(instances, params->instance_count, pair);
  const InstanceDesc instance = instances[instance_index];
  MeshDesc* meshes = (MeshDesc*)params->meshes;
  const uint index = meshes[instance.mesh].first_cluster + (pair - instance.first_pair);

  ClusterDesc* clusters = (ClusterDesc*)params->clusters;
  const ClusterDesc cluster = clusters[index];
  const float3 center = mul(instance.world, float4(cluster.center, 1.0)).xyz;
  const float radius = cluster.radius * instance.scale_max;

  if (params->lod.w > 0.0) {
    for (uint p = 0; p < params->plane_count; ++p) {
      const float4 plane = params->planes[p];
      if (dot(plane.xyz, center) + plane.w < -radius) return;
    }
  }
  // A non-uniform scale shears the cone open, so the test only runs where the scale is uniform.
  if (params->cone_cull != 0 && (instance.flags & k_instance_uniform_scale) != 0) {
    const float3 apex = mul(instance.world, float4(cluster.cone_apex, 1.0)).xyz;
    const float3 axis = normalize(mul(instance.world, float4(cone_axis(cluster), 0.0)).xyz);
    if (backfacing(cluster, apex, axis, params->camera.xyz)) return;
  }
  if (params->lod.z > 0.0) {
    ClusterLodDesc* lods = (ClusterLodDesc*)params->lods;
    const ClusterLodDesc lod = lods[index];
    const float4 own_sphere =
        float4(mul(instance.world, float4(lod.own.xyz, 1.0)).xyz, lod.own.w * instance.scale_max);
    const float4 parent_sphere = float4(mul(instance.world, float4(lod.parent.xyz, 1.0)).xyz,
                                        lod.parent.w * instance.scale_max);
    const float own = projected_error(own_sphere, lod.own_error * instance.scale_max,
                                      params->camera.xyz, params->camera.w, params->lod.x);
    const float parent =
        projected_error(parent_sphere, lod.parent_error * instance.scale_max, params->camera.xyz,
                        params->camera.w, params->lod.x);
    if (!(own <= params->lod.y && parent > params->lod.y)) return;
  }
  if (params->hiz != 0 && occluded(params, center, radius)) return;
  if (params->pass != 0) {
    uint* flags = (uint*)params->flags;
    flags[pair] = 1;
  }

  // Hardware or software rasterization: by projected size unless the mode forces one path.
  bool software = params->raster.y == 1.0;
  if (params->raster.y == 2.0) {
    const float distance = max(length(center - params->camera.xyz) - radius, params->camera.w);
    const float diameter_px = 2.0 * radius / distance * params->lod.x;
    software = diameter_px < params->raster.x;
  }
  if (software) {
    Atomic<uint>* count = (Atomic<uint>*)params->sw_args;
    const uint slot = count->add(1);
    uint2* visible = (uint2*)params->sw_visible;
    visible[slot] = uint2(instance_index, index);
  } else {
    Atomic<uint>* count = (Atomic<uint>*)(params->draw_args + uint64_t(params->count_index) * 4);
    const uint slot = count->add(1);
    uint2* visible = (uint2*)params->visible;
    visible[slot] = uint2(instance_index, index);
  }
}
