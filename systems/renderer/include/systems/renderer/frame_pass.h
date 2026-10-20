#pragma once

// **The renderer's frame-pass registration point** (docs/subsystems/renderer.md, "Frame passes";
// ADR-0027, ADR-0054). A capability that draws into the frame between the material resolve and the
// overlay — a participating medium (systems/volumetrics), and whatever else composites onto the
// shaded picture before the UI — registers one function with `SceneRenderer::add_frame_pass`, and
// the renderer calls it inside every shaded frame's render-graph build with this context. The pass
// declares what it reads and writes through `graph` and never records a barrier of its own (gfx.md,
// "Render graph v0"); everything it needs from the frame is here, and nothing in the renderer names
// the capability.
//
// **What the colour image holds.** A frame with a pass registered is resolved into an image of the
// renderer's own, `R16G16B16A16Sfloat`, in the resolve's `gfx::DisplayEncoding::Linear`: the
// exposed radiance before the display's curve, the transfer and the dither (renderer.md, "The
// output encode") — so a medium composites light onto light, and the picture is still quantized
// once, by the encode pass the renderer draws after every frame pass from that image into the
// frame's target (ADR-0052). Alpha is 1 where the resolve wrote a pixel and 0 where it left the
// stand-in sky's clear (a scene with no sky, whose uncovered pixels the resolve discards); a pass
// that changes such a pixel writes its alpha 1, and the encode then encodes it like any other,
// where an untouched one is the clear to the byte. The overlay composites after the encode, as it
// always did.
//
// **Without a pass registered nothing of this exists**: the resolve draws into the frame's target
// as it always did, with the same pipeline and the same block, so the frame is the bytes it was.
// A pass runs only in a shaded frame (`gfx::ResolveMode::Shaded`) of a path with a visibility
// buffer; a debug view, `--raster direct` and the ramp draw exactly what they drew.

#include <core/base/types.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/gpu_timer.h>
#include <domain/gfx/render_graph.h>
#include <domain/gfx/rhi.h>

#include <string>

namespace engine::gfx {
class Device;
}

namespace engine::renderer {

struct Camera;
struct FrameFire;
struct SceneData;
struct ShadowCascades;

// Where in the frame a pass runs. One stage so far: after the resolve has written every view's
// shaded pixels and its sky, before the encode and the overlay.
enum class FramePassStage : u8 {
  AfterResolve = 0,
};

// Passes a renderer holds at once.
inline constexpr u32 k_max_frame_passes = 4;
// The colour image a frame pass reads and writes: the resolve's linear radiance, as a half holds
// it.
inline constexpr gfx::Format k_frame_pass_format = gfx::Format::R16G16B16A16Sfloat;

// One view of the frame as a frame pass reads it. Its eye is the frame's origin (ADR-0053), so
// `view_proj` maps a point in the frame's space — metres from the eye — to clip space, and
// `clip_to_eye` (its inverse) takes a pixel's clip-space point and reversed-Z depth back there.
struct FramePassView {
  u32 x = 0;  // the view's rectangle of the colour image
  u32 y = 0;
  u32 width = 0;
  u32 height = 0;
  // Its region of the visibility buffer: `source_width` x `source_height` u64 words from
  // `visibility`, depth (reversed-Z float bits) in the high 32 bits, 0 where nothing was drawn.
  u32 source_width = 0;
  u32 source_height = 0;
  u64 visibility = 0;
  Mat4 view_proj;
  Mat4 clip_to_eye;
  Mat4 clip_to_ray;  // the view's rotation and projection: a pixel's ray direction (view_ray.slang)
  bool panini = false;  // the region holds a wider rectilinear source the view resamples
};

// The sky's buffers, for a pass that reads a sky through `FramePassContext::sky_params` and has to
// declare the reads (every one is written by this frame's sky passes before the resolve).
struct FramePassSky {
  gfx::RgBuffer transmittance;
  gfx::RgBuffer multiscatter;
  gfx::RgBuffer view;
  gfx::RgBuffer aerial;
  gfx::RgBuffer frame;
};

// What a pass reports back to the frame, read by the renderer when the callback returns.
struct FramePassReport {
  // The distance at which the medium's transmittance falls to 2% (renderer.md, `Stats::
  // visibility_m`), from the field the pass computed on the CPU — gameplay reads visibility from
  // the field, never from the picture (plan 04). Negative: the pass has no medium to report.
  f32 visibility_m = -1.0f;
};

struct FramePassContext {
  gfx::RenderGraph* graph = nullptr;
  gfx::GpuTimer* timer = nullptr;
  gfx::BindlessSet* bindless = nullptr;
  const gfx::Device* device = nullptr;
  const SceneData* scene = nullptr;
  // The frame slot being recorded, of `frames_in_flight`: a pass's per-frame host-visible blocks
  // are one per slot, written while the slot's previous frame is known to be done.
  u32 slot = 0;
  u32 frames_in_flight = 0;
  u64 frame_index = 0;
  f64 dt_s = 0.0;  // `FrameDesc::dt_s`: the host's step since its last frame
  // The colour image, read and written: exposed linear radiance, `R16G16B16A16Sfloat`, the whole
  // target (every view's rectangle in it). `color_storage` is its storage-image slot in `bindless`
  // (binding 1), `color_sampled` its sampled-image slot (binding 0).
  gfx::RgImage color;
  u32 color_storage = gfx::BindlessSet::k_invalid_slot;
  u32 color_sampled = gfx::BindlessSet::k_invalid_slot;
  u32 width = 0;
  u32 height = 0;
  // The frame's depth: the visibility buffer every view rasterized into or traced (renderer.md,
  // "Frame passes" says why this and not a depth image: it is the one depth every path writes, and
  // it is already the buffer the resolve read). Read it as `gfx::Access::ComputeRead`.
  gfx::RgBuffer visibility;
  u32 view_count = 0;
  const FramePassView* views = nullptr;
  // The camera and the frame's origin (ADR-0053): no absolute position reaches the GPU. A pass that
  // anchors something to the world evaluates it in the eye's frame from an offset computed here on
  // the CPU, in f64, the way `instance_from_eye` places an instance.
  const Camera* camera = nullptr;
  WorldEye eye{};
  WorldPos eye_world{};
  gfx::FrameEye gpu_eye{};
  f32 znear = 0.1f;
  // The frame's clocks (clocks.h; renderer.md, "One world clock, and the day's length"): world
  // seconds, which a medium's motion reads (a flame's noise, the smoke's and the clouds' drift
  // distance), and the sky's hour, absolute game seconds — `FrameDesc::sky_time_s` resolved
  // against the scene's epoch — which the weather reads (the wind record's hour and the plumes').
  f64 world_time_s = 0.0;
  f64 sky_time_s = 0.0;
  // The sun: the direction towards it, and its light — with a sky, its illuminance outside the air
  // (sky units; `gfx::SkyParams::sun_illuminance`); without one, the stand-in's white intensity.
  Vec3 sun_direction{0.0f, 1.0f, 0.0f};
  Vec3 sun_light{1.0f, 1.0f, 1.0f};
  // The stand-in's ambient sky radiance (the resolve's upper hemisphere), for a scene with no sky.
  Vec3 ambient{0.0f, 0.0f, 0.0f};
  // The frame's `gfx::SkyParams` (sky.slang), 0 for the stand-in; its `frame` holds the exposure
  // the resolve exposed the picture at, which a pass's light has to be multiplied by too.
  u64 sky_params = 0;
  FramePassSky sky;
  // The sun's cascaded shadow maps (renderer.md, "Shadows"): when `cascades` is set, the atlas
  // (read it as `gfx::Access::SampledRead`), the address of this frame's `gfx::ShadowMapParams`
  // (the fit, the atlas's bindless slots, the light frame) and the fit on the CPU.
  bool cascades = false;
  gfx::RgImage shadow_atlas;
  u64 shadow_maps = 0;
  const ShadowCascades* fit = nullptr;
  // Where the pass's report goes; the renderer folds it into `Stats` when the callback returns.
  FramePassReport* report = nullptr;
  // The frame's fires (fire.h; renderer.md, "Fires"): the scene's own and its placed props', burnt
  // at `world_time_s` — the same ones whose lights are in the frame's light array — for a medium to
  // draw their flames and smoke. None: `fire_count` 0.
  const FrameFire* fires = nullptr;
  u32 fire_count = 0;
};

// A pass: adds its passes to `context.graph` and fills `context.report`. False, with `error`, fails
// the frame (nothing is submitted beyond an empty command buffer). It is called between
// `begin_frame` and `submit_frame`, on the thread that records the frame.
using FramePassFn = bool (*)(const FramePassContext& context, void* user, std::string* error);

}  // namespace engine::renderer
