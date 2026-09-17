#pragma once

// Parameters of the visibility resolve (shaders/visibility_resolve.slang): the first material
// pass. Read through a device address; the fragment pass pushes only the address. Materials are
// a flat table indexed per cluster, which is enough until the material graph and per-vertex
// attributes arrive.

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

enum class ResolveMode : u32 {
  ClusterColors = 0,
  TriangleShade = 1,
  Depth = 2,
  Shaded = 3,
  Normals = 4,
  Uvs = 5,
};

// Mirrors ResolveParams in visibility_resolve.slang. 184 bytes.
struct ResolveParams {
  Vec4 sky{};     // rgb shown for empty pixels and used as the hemisphere ambient
  Vec4 sun{};     // xyz normalized direction towards the light, w intensity
  Vec4 camera{};  // xyz position
  Mat4 view_proj;
  u64 visibility = 0;         // u64[width * height]
  u64 clusters = 0;           // geometry::ClusterDesc[]
  u64 vertices = 0;           // Vec3[]
  u64 triangles = 0;          // u32[]: packed local indices
  u64 materials = 0;          // ResolveMaterial[]
  u64 cluster_materials = 0;  // u32[cluster_count]
  u64 attributes = 0;         // geometry::VertexAttributes[] parallel to vertices; 0 = flat shading
  u32 width = 0;
  u32 height = 0;
  u32 mode = static_cast<u32>(ResolveMode::Shaded);
  u32 pad = 0;
};
static_assert(sizeof(ResolveParams) == 184);

}  // namespace engine::gfx
