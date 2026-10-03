// A test-only window on material.slang's reconstruction (domain/gfx/tests/reconstruct_tests.cpp):
// for every covered pixel of a visibility buffer, what the resolve takes for it — the barycentrics
// `screen_barycentrics` gives, the point and the UV `reconstruct_screen` interpolates at them, and
// the UV's and the point's change per pixel (`screen_uv_gradients`, `screen_position_gradients`) —
// called the way fs_resolve calls them, and written out as floats, so each can be held to a CPU ray
// against the triangle one pixel at a time. The resolve's picture is eight bits a channel, which is
// a millimetre's worth of nothing; these are the numbers the picture is made of.

#include "material.slang"
#include "scene.slang"

// Read through a device address; mirrors the test's `ProbeParams` (144 bytes).
struct ProbeParams {
  float4 camera;  // xyz the eye, as ResolveParams::camera
  column_major float4x4 view_proj;
  uint64_t visibility;  // uint64_t[width * height], as the rasterizer or the ray path wrote it
  uint64_t clusters;    // ClusterDesc[]
  uint64_t mesh;        // MeshDesc[]
  uint64_t triangles;   // uint[]: packed local indices
  uint64_t instances;   // InstanceDesc[]: one, the scene's
  uint64_t attributes;  // VertexAttributes[]
  uint64_t out;         // float4[6 * width * height]
  uint width;
  uint height;
};

struct ProbePush {
  uint64_t params;
};

[[vk::push_constant]] ConstantBuffer<ProbePush> g_push;

[shader("compute")]
[numthreads(8, 8, 1)]
void probe_main(uint3 id : SV_DispatchThreadID) {
  ProbeParams* params = (ProbeParams*)g_push.params;
  if (id.x >= params->width || id.y >= params->height) return;
  const uint pixel = id.y * params->width + id.x;
  float4* out = (float4*)params->out + 6u * pixel;
  const uint64_t word = ((uint64_t*)params->visibility)[pixel];
  if (word == 0) {
    for (uint k = 0; k < 6; ++k)
      out[k] = float4(0.0, 0.0, 0.0, 0.0);
    return;
  }
  // One instance of one mesh: the id's pair is the cluster, as fs_resolve reads it through a null
  // pair table and a null visible list.
  const uint id_bits = uint(word & 0xffffffffu);
  const uint cluster = id_bits >> 8;
  const uint triangle = id_bits & 0xffu;
  SceneGeometry geo;
  geo.clusters = params->clusters;
  geo.mesh = params->mesh;
  geo.triangles = params->triangles;
  geo.instances = params->instances;
  geo.attributes = params->attributes;
  geo.materials = 0;
  geo.cluster_materials = 0;
  geo.visible = 0;
  geo.normal_pool = 0;
  const InstanceDesc instance = ((InstanceDesc*)params->instances)[0];
  const float2 center = float2(id.xy) + 0.5;
  const TriangleFetch tri = fetch_triangle(geo, instance, 0u, cluster, triangle);
  const ScreenBarycentrics sb =
      screen_barycentrics(tri, center, params->view_proj, params->width, params->height);
  const Surface s = reconstruct_screen(geo, instance, 0u, cluster, triangle, center,
                                       params->view_proj, params->camera.xyz, params->width,
                                       params->height);
  float2 duv_dx;
  float2 duv_dy;
  screen_uv_gradients(geo, tri, center, params->view_proj, params->width, params->height, duv_dx,
                      duv_dy);
  float3 dp_dx;
  float3 dp_dy;
  screen_position_gradients(tri, center, params->view_proj, params->width, params->height, dp_dx,
                            dp_dy);
  out[0] = float4(sb.b, 1.0);
  out[1] = float4(s.position, float(triangle));
  out[2] = float4(s.uv, duv_dx);
  out[3] = float4(duv_dy, 0.0, 0.0);
  out[4] = float4(dp_dx, 0.0);
  out[5] = float4(dp_dy, 0.0);
}
