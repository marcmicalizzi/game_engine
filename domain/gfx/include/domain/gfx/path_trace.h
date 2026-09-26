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
#include <domain/gfx/visibility_resolve.h>

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

// Mirrors PathTraceParams in path_trace.slang. 272 bytes.
struct PathTraceParams {
  Mat4 inv_view_proj;  // clip to world, for the primary rays
  Vec4 camera{};       // xyz eye position
  Vec4 sky{};          // rgb, the same value ResolveParams::sky carries
  Vec4 sun{};          // xyz normalized direction towards the light, w intensity
  // rgb: the ground's albedo, the same value ResolveParams::ground carries. An escaped ray below
  // the horizon returns this ground lit by `sun` and `sky`, the lower half of the environment the
  // resolve's hemisphere term stands for, so the two integrators see one ground. w is unused.
  Vec4 ground{k_neutral_ground_albedo, k_neutral_ground_albedo, k_neutral_ground_albedo, 0.0f};
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
  // How far a ray leaves a surface along its geometric normal: `ray_bias` world units plus
  // `ray_bias_steps` (below) steps of the hit surface's own grid — the resolve's offset exactly
  // (`ResolveParams::shadow_bias`, `shadow_bias_steps`; `ray_offset` in material.slang), because
  // the hit is rebuilt off the same grid and the two must start their shadow rays alike.
  f32 ray_bias = 0.0f;
  // u32[width * height]: 1 where any sample's **primary** ray hit geometry. It exists because the
  // real-time path writes the sky straight out as a display-space colour for an uncovered pixel,
  // while the reference carries linear radiance and would have to round-trip it through
  // `pow(sky, 2.2)` and back — and that round trip moves `sky.g` = 0.70 across the 178.5 byte
  // boundary, putting a one-byte difference on every background pixel of every comparison. So the
  // tonemap writes the background as the resolve writes it wherever nothing covered the pixel,
  // and the transform where something did (which is what a box filter over a silhouette wants).
  u64 coverage = 0;
  // The packed RGBA8 the resolve writes for an uncovered pixel, **quantized on the CPU**. The
  // shader cannot do it: `0.70f * 255` is 178.49999695 exactly, and rounding that product to
  // float32 lands on 178.5 — half a byte away from where it started and on the wrong side of the
  // boundary — while the fixed-function UNORM conversion the resolve's target does converts the
  // same float exactly and writes 178. So the one value that has to match to the byte is computed
  // where there is precision to spare (`pack_unorm_rgba8`) and handed over ready.
  u32 background = 0;
  f32 ray_bias_steps = 0.0f;  // steps of the hit surface's own grid added to `ray_bias`
};

// A linear-space colour into the byte a UNORM target would hold, in double precision so the
// product never rounds across a boundary. Alpha is always opaque.
inline u32 pack_unorm_rgba8(Vec4 color) noexcept {
  auto quantize = [](f32 v) {
    const f64 clamped = v < 0.0f ? 0.0 : (v > 1.0f ? 1.0 : static_cast<f64>(v));
    return static_cast<u32>(clamped * 255.0 + 0.5);
  };
  return quantize(color.x) | (quantize(color.y) << 8) | (quantize(color.z) << 16) | (255u << 24);
}
static_assert(sizeof(PathTraceParams) == 272);
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
