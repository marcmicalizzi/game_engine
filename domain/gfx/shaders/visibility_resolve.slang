// Fullscreen resolve of the 64-bit visibility buffer (docs/plan/04-renderer.md §4.3 step 4):
// the first material pass. For every covered pixel the cluster and triangle id lead back to the
// triangle's three world-space vertices; screen-space barycentrics against the pixel center,
// made perspective-correct with the vertices' clip-space w, reconstruct the surface position,
// and the triangle plane gives the normal (flat shading until vertex attributes arrive). A
// per-cluster material index selects albedo and emissive, and a directional sun plus a sky
// hemisphere light the surface. Debug modes show cluster colors, triangle shading, depth, or
// normals. Empty pixels (value 0: depth 0 is the far plane and id 0 never occurs there) take the
// sky color.

struct ClusterDesc {
  uint vertex_offset;
  uint triangle_offset;
  uint vertex_count;
  uint triangle_count;
  float3 center;
  float radius;
};

struct Position {
  float x;
  float y;
  float z;
};

// Mirrors gfx::ResolveMaterial (32 bytes).
struct ResolveMaterial {
  float4 albedo;    // rgb linear, w roughness
  float4 emissive;  // rgb linear, w metallic
};

// Read through a device address; mirrors gfx::ResolveParams (176 bytes).
struct ResolveParams {
  float4 sky;     // rgb, shown for empty pixels and used as the hemisphere ambient
  float4 sun;     // xyz normalized direction towards the light, w intensity
  float4 camera;  // xyz position
  column_major float4x4 view_proj;
  uint64_t visibility;         // uint64_t[width * height]
  uint64_t clusters;           // ClusterDesc[]
  uint64_t vertices;           // Position[]
  uint64_t triangles;          // uint[]: packed local indices
  uint64_t materials;          // ResolveMaterial[]
  uint64_t cluster_materials;  // uint[cluster_count]: material index per cluster
  uint width;
  uint height;
  uint mode;  // 0 cluster colors, 1 with triangle shade, 2 depth, 3 shaded, 4 normals
  uint pad;
};

struct ResolvePush {
  uint64_t params;
};

[[vk::push_constant]] ConstantBuffer<ResolvePush> g_push;

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

float edge(float2 a, float2 b, float2 c) {
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Surface position and flat normal of the triangle `id` names, at the pixel center.
void reconstruct(ResolveParams* params, uint id, float2 pixel_center, out float3 position, out float3 normal) {
  ClusterDesc* clusters = (ClusterDesc*)params->clusters;
  uint* triangles = (uint*)params->triangles;
  Position* positions = (Position*)params->vertices;
  const ClusterDesc cluster = clusters[id >> 8];
  const uint packed = triangles[cluster.triangle_offset + (id & 0xff)];
  float3 p[3];
  float4 clip[3];
  float2 screen[3];
  const float2 size = float2(float(params->width), float(params->height));
  for (uint i = 0; i < 3; ++i) {
    const Position v = positions[cluster.vertex_offset + ((packed >> (8 * i)) & 0xff)];
    p[i] = float3(v.x, v.y, v.z);
    clip[i] = mul(params->view_proj, float4(p[i], 1.0));
    const float2 ndc = clip[i].xy / clip[i].w;
    screen[i] = float2((ndc.x * 0.5 + 0.5) * size.x, (0.5 - ndc.y * 0.5) * size.y);
  }
  // Screen-space barycentrics, then perspective correction through 1/w.
  const float area = edge(screen[0], screen[1], screen[2]);
  float3 b;
  if (abs(area) > 1e-12) {
    b.x = edge(screen[1], screen[2], pixel_center) / area;
    b.y = edge(screen[2], screen[0], pixel_center) / area;
    b.z = 1.0 - b.x - b.y;
    b = clamp(b, 0.0, 1.0);
    b /= max(b.x + b.y + b.z, 1e-6);
    const float3 bw = b / float3(clip[0].w, clip[1].w, clip[2].w);
    b = bw / max(bw.x + bw.y + bw.z, 1e-12);
  } else {
    b = float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0);
  }
  position = b.x * p[0] + b.y * p[1] + b.z * p[2];
  normal = normalize(cross(p[1] - p[0], p[2] - p[0]));
  if (dot(normal, params->camera.xyz - position) < 0.0) normal = -normal;  // two-sided
}

[shader("fragment")]
float4 fs_resolve(FullscreenOut in) : SV_Target {
  ResolveParams* params = (ResolveParams*)g_push.params;
  const uint2 pixel = uint2(in.position.xy);
  if (pixel.x >= params->width || pixel.y >= params->height) return params->sky;
  uint64_t* visibility = (uint64_t*)params->visibility;
  const uint64_t value = visibility[pixel.y * params->width + pixel.x];
  if (value == 0) return params->sky;
  const uint id = uint(value & 0xffffffffu);
  const float depth = asfloat(uint(value >> 32));
  switch (params->mode) {
    case 0: return float4(id_color(id), 1.0);
    case 1: return float4(id_color(id) * (0.7 + 0.3 * frac(float(id & 0xff) * 0.6180339887)), 1.0);
    case 2: return float4(depth, depth, depth, 1.0);
    default: break;
  }
  float3 position;
  float3 normal;
  reconstruct(params, id, float2(pixel) + 0.5, position, normal);
  if (params->mode == 4) return float4(normal * 0.5 + 0.5, 1.0);

  ResolveMaterial* materials = (ResolveMaterial*)params->materials;
  uint* cluster_materials = (uint*)params->cluster_materials;
  const ResolveMaterial material = materials[cluster_materials[id >> 8]];
  const float n_dot_l = max(dot(normal, params->sun.xyz), 0.0);
  const float3 ambient = params->sky.rgb * lerp(0.15, 0.35, normal.y * 0.5 + 0.5);
  const float3 color = material.albedo.rgb * (n_dot_l * params->sun.w + ambient) + material.emissive.rgb;
  return float4(pow(max(color, 0.0), 1.0 / 2.2), 1.0);  // linear to display on a UNORM swapchain
}
