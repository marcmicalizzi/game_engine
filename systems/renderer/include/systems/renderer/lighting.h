#pragma once

// The lights a frame is drawn with (docs/subsystems/renderer.md). One sun, one sky, and the two
// point lights that orbit the scene, in one function because **two integrators have to agree
// about them or a comparison measures the disagreement**: `SceneRenderer::record_frame` fills a
// `gfx::ResolveParams` from this and `ReferenceRenderer` fills a `gfx::PathTraceParams` from the
// same call, so the reference path tracer and the real-time resolve light the same scene with the
// same numbers at the same frame index (docs/plan/04-renderer.md §4.8).
//
// These are the renderer's own stand-in lights until a scene carries authored ones: the sun is
// fixed, the sky is a constant, and the two point lights orbit out of phase — one warm, one cool
// — so the BSDF's specular response sweeps across a surface while the camera turns and metal
// reads as metal. Reach and intensity scale with the scene's radius, intensity with its square
// because the falloff is inverse square, so a 2 cm mesh and a 20 m heightfield look alike.

#include <core/base/types.h>
#include <core/math/math.h>
#include <domain/gfx/visibility_resolve.h>

namespace engine::renderer {

struct SceneData;

// The two orbiting point lights beside the sun.
inline constexpr u32 k_frame_lights = 2;

// The sky, in one place, because **three** things have to be the same number. It is the resolve
// pass's clear value and `gfx::ResolveParams::sky`, which `sky_is_clear` ties together — the
// shader discards an empty pixel instead of writing a colour the clear already put there, so two
// spellings would make an uncovered pixel take whichever the clear said, silently. And it is the
// reference path tracer's background, quantized on the CPU from this constant: a third spelling
// would put a one-byte difference on every empty pixel of every comparison, which is a mistake
// already made once here and measured (docs/subsystems/renderer.md, "Reference renderer").
inline constexpr Vec4 k_sky{0.55f, 0.70f, 0.90f, 1.0f};

struct FrameLighting {
  Vec4 sky{};  // rgb: the background an uncovered pixel shows, and the hemisphere ambient
  Vec4 sun{};  // xyz normalized direction towards the light, w intensity
  gfx::ResolveLight lights[k_frame_lights];
  u32 light_count = 0;
  // World units a ray leaves a surface by, along the geometric normal. A thousandth of the scene
  // radius — a couple of centimetres on the heightfield, well over the half grid step by which a
  // quantized position may differ from the float one the acceleration structures were built from,
  // and far under any feature that casts.
  f32 shadow_bias = 0.0f;
};

// `frame_index` drives the orbit, exactly as it drives the deformation phase, so frame 0 of a
// capture and frame 0 of a reference render are lit identically. `lights` false leaves the two
// point lights out and the count at zero, which is what `--no-lights` asks for.
void frame_lighting(const SceneData& scene, u64 frame_index, bool lights, FrameLighting& out);

}  // namespace engine::renderer
