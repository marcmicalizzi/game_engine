#pragma once

// Parameters of the visibility resolve (shaders/visibility_resolve.slang): the first material
// pass. Read through a device address; the fragment pass pushes only the address. Positions come
// off the same 16-bit grid the rasterizers read (`MeshDesc` in cluster_cull.h), so the resolve
// reconstructs exactly the triangle that was drawn. Materials are a flat table indexed per
// cluster, which is enough until the material graph arrives. Shading is the physically based
// BSDF of shaders/brdf.slang (GGX specular, Lambert diffuse) under a directional sun, a list of
// analytic lights, and a sky hemisphere.

#include <core/base/types.h>
#include <core/math/math.h>

namespace engine::gfx {

inline constexpr u32 k_no_texture = 0xFFFFFFFFu;

// Mirrors ResolveMaterial in visibility_resolve.slang. 48 bytes.
struct ResolveMaterial {
  Vec4 albedo{0.8f, 0.8f, 0.8f, 0.5f};  // rgb linear, w roughness
  Vec4 emissive{};                      // rgb linear, w metallic
  u32 albedo_texture = k_no_texture;    // bindless sampled-image slot, multiplies albedo
  u32 sampler = 0;                      // bindless sampler slot
  f32 uv_scale = 1.0f;
  u32 flags = 0;
};
static_assert(sizeof(ResolveMaterial) == 48);

inline constexpr f32 k_light_point = 0.0f;  // ResolveLight::direction_type.w
inline constexpr f32 k_light_spot = 1.0f;

// One analytic light. Mirrors ResolveLight in visibility_resolve.slang. 64 bytes.
// The falloff is inverse square windowed to zero at the radius of influence (Karis), so a light
// contributes nothing beyond it and culling by radius cannot change the picture.
struct ResolveLight {
  Vec4 position_radius{};  // xyz world position, w radius of influence
  Vec4 color_intensity{};  // rgb linear color, w radiant intensity scale
  Vec4 direction_type{};   // xyz spot direction (towards the lit surface), w k_light_point/_spot
  Vec4 spot{};             // x cos inner angle, y cos outer angle, zw unused
};
static_assert(sizeof(ResolveLight) == 64);

enum class ResolveMode : u32 {
  ClusterColors = 0,
  TriangleShade = 1,
  Depth = 2,
  Shaded = 3,
  Normals = 4,
  Uvs = 5,
};

// Mirrors ResolveParams in visibility_resolve.slang. 192 bytes.
struct ResolveParams {
  Vec4 sky{};     // rgb shown for empty pixels and used as the hemisphere ambient
  Vec4 sun{};     // xyz normalized direction towards the light, w intensity
  Vec4 camera{};  // xyz position
  Mat4 view_proj;
  u64 visibility = 0;         // u64[width * height]
  u64 clusters = 0;           // geometry::ClusterDesc[]
  u64 mesh = 0;               // MeshDesc (cluster_cull.h): the quantized positions and their grid
  u64 triangles = 0;          // u32[]: packed local indices
  u64 materials = 0;          // ResolveMaterial[]
  u64 cluster_materials = 0;  // u32[cluster_count]
  u64 attributes = 0;         // geometry::VertexAttributes[] parallel to vertices; 0 = flat shading
  u64 lights = 0;             // ResolveLight[light_count]
  u32 width = 0;
  u32 height = 0;
  u32 mode = static_cast<u32>(ResolveMode::Shaded);
  u32 light_count = 0;
};
static_assert(sizeof(ResolveParams) == 192);
static_assert(sizeof(ResolveParams) % 16 == 0, "the block is read as float4 rows on the GPU");

}  // namespace engine::gfx
