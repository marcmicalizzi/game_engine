#pragma once

// Parameters of the reference path tracer (shaders/path_trace.slang, docs/plan/04-renderer.md
// §4.8, docs/subsystems/renderer.md "Reference renderer"). Read through a device address; the
// compute pass pushes only the address, like every other pass of the frame.
//
// **The reference traces the same acceleration structures the real-time frame built.** There is
// no second scene and no second geometry path: `renderer::ReferenceRenderer` renders one frame
// through `SceneRenderer` so that the frame's cluster acceleration structure chain runs, then
// dispatches this shader against the top-level structure in bindless slot `scene`. A hit's
// GeometryIndex is the visible entry and its PrimitiveIndex the triangle, exactly as in
// `ray_visibility.h`, so the material a reference ray shades is the material the resolve would
// have shaded on that pixel. What the reference is a reference *for* therefore includes the LOD
// cut: a capture taken at the frame's own threshold measures shading and transport, and one taken
// at threshold 0 (`ReferenceSettings::finest`) measures those plus the cut.
//
// **Accumulation is progressive and seeded by pixel and sample index.** `accum` holds one
// `float4` per pixel — the running radiance sum in rgb and the sample count in w — and a dispatch
// adds `spp` samples numbered from `sample_base`. A pixel's Nth sample depends on nothing but
// (pixel, N, seed), so ten dispatches of ten samples are the same image as one of a hundred, and
// the same seed gives the same bytes.

#include <core/base/types.h>
#include <core/math/math.h>

namespace engine::gfx {

// PathTraceParams::flags.
// The environment is `sky` in every direction, primary rays included: the white furnace the
// integrator's energy conservation is checked with (docs/subsystems/renderer.md). Off, the sky
// is the resolve's two roles — the background a primary miss shows and the hemisphere ambient an
// escaped secondary ray picks up — so that the two pictures are comparable.
inline constexpr u32 k_pt_uniform_sky = 1u;
// Sample the pixel centre instead of jittering inside the pixel. The reference box-filters a
// pixel and the rasterizer point-samples it, which is a real difference at every silhouette; a
// comparison that is about shading rather than about antialiasing turns the jitter off.
inline constexpr u32 k_pt_pixel_center = 2u;

// Mirrors PathTraceParams in path_trace.slang. 240 bytes.
struct PathTraceParams {
  Mat4 inv_view_proj;  // clip to world, for the primary rays
  Vec4 camera{};       // xyz eye position
  Vec4 sky{};          // rgb, the same value ResolveParams::sky carries
  Vec4 sun{};          // xyz normalized direction towards the light, w intensity
  u64 accum = 0;       // float4[width * height]: radiance sum in rgb, sample count in w
  u64 output = 0;      // u32[width * height] packed RGBA8; read by the tonemap entry point only
  u64 clusters = 0;    // geometry::ClusterDesc[]
  u64 mesh = 0;        // MeshDesc[]
  u64 triangles = 0;   // u32[]: packed local indices
  u64 instances = 0;   // InstanceDesc[]
  u64 attributes = 0;  // geometry::VertexAttributes[]; 0 = flat shading
  u64 materials = 0;   // ResolveMaterial[]
  u64 cluster_materials = 0;  // u32[cluster_count]
  u64 visible = 0;            // u32x2[]: the frame's visible list; 0 reads {0, geometry_index}
  u64 lights = 0;             // ResolveLight[light_count]
  u32 width = 0;
  u32 height = 0;
  u32 light_count = 0;
  u32 scene = 0;        // bindless slot of the top-level structure
  u32 spp = 1;          // samples this dispatch adds
  u32 sample_base = 0;  // samples already in the accumulator
  u32 max_bounces = 3;  // scattering events after the primary hit; 1 is direct plus one bounce
  u32 seed = 0;
  u32 flags = 0;
  f32 ray_bias = 0.0f;  // world units along the geometric normal, off the surface
  u32 pad[4] = {};
};
static_assert(sizeof(PathTraceParams) == 256);
static_assert(sizeof(PathTraceParams) % 16 == 0, "the block is read as float4 rows on the GPU");

// Russian roulette starts after this many scattering events, so a short path is never cut and a
// long one is. Mirrored in the shader.
inline constexpr u32 k_pt_roulette_depth = 3;

// The compute dispatch covers ceil(extent / 8) groups in each of x and y, matching
// `[numthreads(8, 8, 1)]` in path_trace.slang.
inline constexpr u32 k_path_trace_workgroup = 8;
inline u32 path_trace_group_count(u32 extent) noexcept {
  return (extent + k_path_trace_workgroup - 1) / k_path_trace_workgroup;
}

}  // namespace engine::gfx
