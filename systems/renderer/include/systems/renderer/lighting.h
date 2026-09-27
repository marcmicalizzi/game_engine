#pragma once

// The lights a frame is drawn with (docs/subsystems/renderer.md). One sun, one sky, and the two
// point lights that orbit the scene, in one function because **two integrators have to agree
// about them or a comparison measures the disagreement**: `SceneRenderer::record_frame` fills a
// `gfx::ResolveParams` from this and `ReferenceRenderer` fills a `gfx::PathTraceParams` from the
// same call, so the reference path tracer and the real-time resolve light the same scene with the
// same numbers at the same frame index (docs/plan/04-renderer.md §4.8).
//
// These are the renderer's own stand-in lights until a scene carries authored ones: the sun stands
// where `RenderSettings` or the `renderer.sun.*` tunables put it, the sky is a constant, and the
// two point lights — one warm, one cool — stand still, or with `orbit_lights` orbit out of phase as
// the frame index advances so the BSDF's specular response sweeps across a surface. **Nothing moves
// with the frame by default** (since 2026-09-27): the orbit was frame-driven, and a time-lapse's
// sand, lit by a warm light a scene's radius away that went round it every eight seconds, read as a
// sun racing across the sky (renderer.md, "The dunes in time-lapse"). There is no day and night.
// Reach and intensity scale with the scene's radius, intensity with its square because the falloff
// is inverse square, so a 2 cm mesh and a 20 m heightfield look alike. The ground they light is the
// scene's own, `SceneData::ground_albedo`: a terrain's sand, or a neutral grey for a scene without
// one.

#include <core/base/types.h>
#include <core/math/math.h>
#include <domain/gfx/visibility_resolve.h>

#include <optional>

namespace engine::renderer {

struct SceneData;
struct RenderSettings;

// The sun every frame had before it could be moved, normalize(0.4, 0.8, 0.45): 48.37 degrees from
// +x towards +z, 53.03 above the horizon. The tunables' defaults are these, and a sun at them is
// this vector to the bit, so a picture that did not ask for a sun is the picture it always was.
inline constexpr f64 k_default_sun_azimuth_deg = 48.3664606634298;
inline constexpr f64 k_default_sun_elevation_deg = 53.034893494453584;

// The direction towards the sun, azimuth in the ground plane from +x towards +z and elevation above
// it, degrees; exactly the old fixed vector at the defaults.
Vec3 sun_direction(f64 azimuth_deg, f64 elevation_deg) noexcept;

// What `frame_lighting` lights with: the point lights or not, orbiting or not, and the sun.
struct LightingOptions {
  bool lights = true;
  bool orbit = false;
  Vec3 sun = sun_direction(k_default_sun_azimuth_deg, k_default_sun_elevation_deg);
};
// A request's: its sun, or the `renderer.sun.azimuth_deg` / `elevation_deg` tunables' when it names
// none.
LightingOptions lighting_options(const RenderSettings& settings);

// The two point lights beside the sun.
inline constexpr u32 k_frame_lights = 2;

// The sky, in one place, because **three** things have to be the same number. It is the resolve
// pass's clear value and `gfx::ResolveParams::sky`, which `sky_is_clear` ties together — the
// shader discards an empty pixel instead of writing a colour the clear already put there, so two
// spellings would make an uncovered pixel take whichever the clear said, silently. And it is the
// reference path tracer's background, quantized on the CPU from this constant: a third spelling
// would put a one-byte difference on every empty pixel of every comparison, which is a mistake
// already made once here and measured (docs/subsystems/renderer.md, "Reference renderer").
inline constexpr Vec4 k_sky{0.55f, 0.70f, 0.90f, 1.0f};

// **How far a ray leaves a surface** (`FrameLighting::shadow_bias` and `shadow_bias_steps`;
// `ray_offset` in material.slang; docs/subsystems/renderer.md, "Shadows"), in two parts because
// there are two gaps between the surface a pixel is rebuilt on and the one a ray is traced against:
//
// - **One step of the surface's own 16-bit grid**, through its instance's largest scale. Rounding
//   to the grid moves a vertex at most half a step on each axis, so √3/2 of a step along any
//   normal; one step clears it. It is the receiver's own, so the 5 km terrain gets 7.8 cm and a
//   4 m wall standing on it 0.06 mm.
// - **2^-18 of the scene's reach** (the distance from the origin to the far side of its bounds,
//   which no coordinate exceeds), for the float arithmetic the grid says nothing about: a world
//   position is a matrix product of the grid's floats and the ray is transformed back into the
//   instance's space. 2^-18 is 32 to 64 ulps of the largest coordinate; 1.8 cm on the ashlar ruins'
//   5 km scene, 4 µm on a Khronos sample a metre across.
//
// Measured on the ruins at frame 900 of the owner's 15:26 recording (renderer.md): the terrain
// with no grid term is acne over 2.3 million pixels, the kit with no float term speckles, and both
// together leave the rays and the cascaded maps disagreeing on 6,445 of 8.3 million pixels, every
// one at an edge or in a dent the maps' texels cannot hold.
inline constexpr f32 k_shadow_bias_steps = 1.0f;
inline constexpr f32 k_shadow_bias_relative = 1.0f / 262144.0f;

struct FrameLighting {
  Vec4 sky{};  // rgb: the background an uncovered pixel shows, and the hemisphere's upper half
  Vec4 sun{};  // xyz normalized direction towards the light, w intensity
  // rgb: the ground's albedo, `SceneData::ground_albedo` — the hemisphere's lower half is this
  // ground lit by `sun` and `sky` (gfx::ResolveParams::ground, PathTraceParams::ground). w unused.
  Vec4 ground{};
  gfx::ResolveLight lights[k_frame_lights];
  u32 light_count = 0;
  // How far a ray leaves a surface along its geometric normal: `shadow_bias` world units plus
  // `shadow_bias_steps` steps of **the surface's own** 16-bit position grid (`ray_offset` in
  // material.slang; gfx::ResolveParams and gfx::PathTraceParams carry both). The grid term is the
  // gap between the surface a pixel or a hit is rebuilt on and the float one the acceleration
  // structures hold; the world term is float error, sized by the scene's reach. Until 2026-09-26
  // it was one number, a thousandth of the scene's radius, which is 4.1 m over the ashlar ruins'
  // 5 km terrain: every ray from the sand started above every wall (renderer.md, "Shadows").
  f32 shadow_bias = 0.0f;
  f32 shadow_bias_steps = 0.0f;
};

// With `orbit`, `frame_index` drives the point lights' orbit, exactly as it drives the deformation
// phase, so frame N of a capture and frame N of a reference render are lit identically; without it
// they stand where frame 0 puts them. `lights` false leaves the two point lights out and the count
// at zero, which is what `--no-lights` asks for.
void frame_lighting(const SceneData& scene, u64 frame_index, const LightingOptions& options,
                    FrameLighting& out);

}  // namespace engine::renderer
