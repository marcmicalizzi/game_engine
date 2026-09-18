#pragma once

// The reference renderer (docs/plan/04-renderer.md §4.8, docs/subsystems/renderer.md "Reference
// renderer"): a GPU path tracer over the same scene, the same materials, the same BSDF, the same
// lights and the same camera as the real-time path, converged to a noise floor, whose pictures
// are what the real-time path is measured against.
//
// **It uses a `SceneRenderer`; it is not one.** A reference render is one real-time frame —
// which is what builds the frame's cluster acceleration structures out of its own visible list —
// followed by N compute dispatches that trace those structures and accumulate. Nothing about the
// frame changes for it except the one thing sharing requires: `FrameDesc::lod_px`, which
// `finest` sets to 0 so that the cut under the structures is the source geometry rather than the
// frame's LOD selection. A capture therefore says **which geometry it is a reference for**, and
// both answers are useful: at the frame's own threshold the comparison isolates shading and
// transport, at threshold 0 it also measures what the LOD cut costs.
//
// clang-format off
//     ReferenceRenderer reference;
//     reference.create(device, scene, renderer, {}, &error);
//     ReferenceFrame frame;
//     reference.render(camera, {.spp = 512, .max_bounces = 3}, frame, &error);
//     // frame.color is RGBA8 through the resolve's own display transform; frame.hdr is linear.
// clang-format on
//
// It needs a device that can trace the frame's structures — cluster acceleration structures and
// ray queries — and a `SceneRenderer` whose resolved settings run the chain at all
// (`ResolvedSettings::rt_chain`, which `--shadows rt` or `--raster rt` turns on).
// `reference_available` says so before anything is created, the way `check_availability` does
// for the renderer.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/gfx/device.h>
#include <domain/gfx/gpu_timer.h>
#include <domain/gfx/shader_library.h>
#include <domain/gfx/vulkan.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/view_set.h>

#include <span>
#include <string>

namespace engine::renderer {

// What a reference render is asked for. The defaults are a quick look rather than a converged
// picture; the noise floor per scene is measured in docs/subsystems/renderer.md.
struct ReferenceSettings {
  u32 spp = 64;         // total samples per pixel
  u32 max_bounces = 3;  // scattering events after the primary hit; 1 is direct plus one bounce
  // Build the frame's acceleration structures from the **finest** clusters (LOD threshold 0)
  // instead of the frame's own cut, which is what makes the picture a reference for the source
  // geometry rather than for the geometry the real-time frame chose.
  bool finest = false;
  u32 seed = 1;   // the same seed is the same bytes
  u32 batch = 8;  // samples per dispatch: progress granularity, and how long one submission runs
  // The furnace configuration: the environment is the sky colour in every direction, primary
  // rays included, and **the sun and the analytic lights are off** — a furnace with a sun in it
  // does not test energy conservation, it tests the sun. What a white surface returns is then its
  // directional albedo times the environment, which is how the integrator is checked.
  bool uniform_sky = false;
  // Sample the pixel centre instead of jittering inside it. The reference box-filters a pixel
  // and the rasterizer point-samples it; a comparison that is about shading rather than about
  // antialiasing turns the jitter off.
  bool pixel_center = false;
  u64 frame_index = 0;  // drives the light orbit, exactly as a capture's frame number does
  // **An animated instance's pose**, in the same two spans `FrameDesc` takes. A reference render
  // is one frame, so what it draws is one pose: the caller ticks its world, hands over the
  // matrices, and the reference path traces the character where the tick put it. Nothing else was
  // needed for that — the preliminary frame writes the deformed-vertex pool and builds the
  // acceleration structures from it, and the reference traces those — which is the whole reason
  // this is a pass-through and not a feature (docs/subsystems/renderer.md, "An animated
  // instance"). Empty spans leave every instance at rest, as they do for a frame.
  std::span<const anim::JointMatrix> joints;
  std::span<const InstanceJoints> instance_joints;
};

// What one came back with. `hdr` is the linear radiance **mean** — the accumulator already
// divided by its sample count — and `color` is the same thing through the display transform the
// resolve ends on, so a reference image and a real-time image can be compared as they are.
struct ReferenceFrame {
  u32 width = 0;
  u32 height = 0;
  u32 samples = 0;        // samples per pixel actually accumulated
  Vector<f32> hdr;        // 4 per pixel: linear rgb, then the sample count
  Vector<u8> color;       // 4 per pixel, RGBA8
  f64 trace_ms = 0.0;     // GPU milliseconds of the integrator alone, summed over the batches
  f64 seconds = 0.0;      // wall time of render(), including the real-time frame and the readbacks
  u32 visible_pairs = 0;  // the cut the structures were built from, which `finest` changes
};

// Called after each batch with the samples finished and the total. Returning false stops the
// render and leaves `ReferenceFrame` holding what had accumulated, which is a valid — noisier —
// picture, because accumulation is progressive. A plain function pointer rather than a
// `std::function`: the engine's rule, and nothing here needs a closure.
using ReferenceProgress = bool (*)(u32 done, u32 total, void* user);

// Whether this device and these resolved settings can render a reference at all. False with
// `why` filled in, which the hosts report the way they report `availability_message`.
bool reference_available(const ResolvedSettings& resolved, const gfx::DeviceFeatures& features,
                         std::string* why = nullptr);

class ReferenceRenderer {
 public:
  struct Desc {
    // A shader manifest to prefer over the embedded SPIR-V; empty looks for
    // `<exe dir>/../shaders/manifest.json` and uses the embedded bytes when there is none. The
    // reference keeps its own library rather than reaching into the `SceneRenderer`'s, because a
    // manifest entry shadows an embedded one only when the manifest is loaded second.
    std::string shader_manifest;
  };

  ReferenceRenderer() noexcept = default;
  ~ReferenceRenderer();
  ENGINE_NON_COPYABLE(ReferenceRenderer);

  // `device`, `scene` and `renderer` must outlive this. The renderer is used to draw the frame
  // whose acceleration structures the reference traces, and is otherwise untouched.
  bool create(const gfx::Device& device, GpuScene& scene, SceneRenderer& renderer, const Desc& desc,
              std::string* error = nullptr);
  void destroy() noexcept;
  bool valid() const noexcept { return device_ != nullptr; }

  u32 width() const noexcept { return width_; }
  u32 height() const noexcept { return height_; }

  // One converged picture. Renders the real-time frame first (so the structures exist), then
  // `ceil(spp / batch)` dispatches, then the tonemap and the two readbacks.
  bool render(const Camera& camera, const ReferenceSettings& settings, ReferenceFrame& out,
              std::string* error = nullptr, ReferenceProgress progress = nullptr,
              void* user = nullptr);

  // Screen-sized buffers for a new size. The caller resizes the `SceneRenderer` too; this reads
  // its extent, so call it after.
  bool resize(std::string* error = nullptr);

 private:
  bool create_pipelines(std::string* error);
  bool create_targets(std::string* error);

  const gfx::Device* device_ = nullptr;
  GpuScene* scene_ = nullptr;
  SceneRenderer* renderer_ = nullptr;
  Desc desc_;
  gfx::ShaderLibrary shaders_;
  gfx::GpuTimer timer_;
  gfx::ComputePipeline trace_;
  gfx::ComputePipeline tonemap_;
  gfx::BufferResource accum_;   // float4 per pixel, device local
  gfx::BufferResource output_;  // u32 per pixel, packed RGBA8, device local + transfer source
  // u32 per pixel: 1 where a primary ray hit geometry. It is what lets the tonemap write an
  // uncovered pixel the way the resolve writes it, which keeps a one-byte quantization difference
  // off every background pixel of every comparison (see path_trace.h).
  gfx::BufferResource coverage_;
  gfx::BufferResource params_;  // one host-visible gfx::PathTraceParams
  gfx::BufferResource lights_;  // the frame's gfx::ResolveLight array, host-visible
  u32 width_ = 0;
  u32 height_ = 0;
};

}  // namespace engine::renderer
