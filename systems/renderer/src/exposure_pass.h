#pragma once

// **The eye's adaptation as a frame pass of the renderer's own** (systems/renderer/exposure.h for
// the model; docs/subsystems/renderer.md, "Exposure: the eye's adaptation"; roadmap R168).
// Registered at `FramePassStage::Exposure` — after every pass that composites light (a medium),
// before a reconstruction — when adaptation is on, it adds two compute passes over the linear image
// the resolve wrote:
//
//   1. **the histogram** (`exposure_histogram_main`): one pixel in each 2 x 2 tile of the metered
//      view, its luminance in cd/m^2 (the exposed value back through the exposure the resolve drew
//      at), counted with its centre weight into 128 quarter-stop bins, in workgroup memory first
//      and then into the frame's bins by integer atomics — so the counts are the same whatever
//      order the threads ran in;
//   2. **the adaptation** (`exposure_adapt_main`): one workgroup that reads the bins (and clears
//   them
//      for the next frame), averages the log luminance between the low and high shares of the
//      weight in a fixed order, and steps the persistent exposure towards the target.
//
// and names its state to the encode (`FramePassReport::exposure`), which multiplies the picture by
// the state's scale before the display's curve. Nothing of it exists without the switch.
//
// **Deterministic along a path**: the bins are integers, the average is one thread's loop, and the
// step's time is the host's (`FrameDesc::dt_s`) or, where the host gives none, the frame index's at
// `k_frame_hz` — the clock a camera path and a capture already advance by — so a frame at a given
// index of a given path is the same exposure in every run. The first frame metered, and a frame
// whose index went back (a new flight in the same renderer), take the target at once.

#include <systems/renderer/exposure.h>
#include <systems/renderer/frame_pass.h>
#include <systems/renderer/scene_renderer.h>

#include <string>

namespace engine::renderer {

// One frame's block, behind one address (shaders/exposure.slang mirrors it; 96 bytes).
struct ExposureParams {
  u64 histogram = 0;  // u32[k_exposure_bins], cleared by the adaptation after it reads them
  u64 state = 0;      // ExposureState, kept from frame to frame
  u64 report = 0;     // ExposureState: this slot's copy, read on the CPU
  u64 sky = 0;        // gfx::SkyParams, 0 for the stand-in
  u32 rect_x = 0;     // the metered view's rectangle of the linear image
  u32 rect_y = 0;
  u32 rect_width = 0;
  u32 rect_height = 0;
  u32 color = 0;  // storage slot of the linear image
  u32 flags = 0;  // k_exposure_snap
  f32 dt_s = 0.0f;
  f32 low = 0.1f;
  f32 high = 0.9f;
  f32 min_ev100 = -6.0f;
  f32 max_ev100 = 18.0f;
  f32 dark_tau_s = 1.5f;
  f32 cone_tau_s = 0.1f;
  f32 rod_tau_s = 0.4f;
  u32 pad0 = 0;
  u32 pad1 = 0;
};
static_assert(sizeof(ExposureParams) == 96, "exposure.slang's ExposureParams");

inline constexpr u32 k_exposure_snap = 1u;  // take the target at once: no frame before this one

class ExposurePass {
 public:
  bool create(const gfx::Device& device, GpuScene& scene, gfx::ShaderLibrary& shaders,
              u32 frames_in_flight, const ExposureOptions& options, std::string* error);
  // The device is idle (every caller waits first).
  void destroy() noexcept;

  // The frame pass: `user` is the pass.
  static bool record_thunk(const FramePassContext& context, void* user, std::string* error);

  // What the frame recorded last in this slot reached, read when the slot comes round again (so
  // `frames_in_flight` frames late, like the renderer's other GPU counts).
  const ExposureState& reported() const noexcept { return reported_; }
  // What the last recorded frame reached. The device must be idle.
  ExposureState latest() const noexcept;
  const ExposureOptions& options() const noexcept { return options_; }
  u64 device_bytes() const noexcept;

 private:
  bool record(const FramePassContext& context, std::string* error);

  const gfx::Device* device_ = nullptr;
  ExposureOptions options_;
  gfx::ComputePipeline histogram_pipeline_;
  gfx::ComputePipeline adapt_pipeline_;
  gfx::BufferResource histogram_;        // device-local u32[k_exposure_bins]
  gfx::BufferResource state_;            // device-local ExposureState
  Vector<gfx::BufferResource> params_;   // per frame slot: ExposureParams, host-visible
  Vector<gfx::BufferResource> reports_;  // per frame slot: ExposureState, host-visible
  ExposureState reported_;
  u32 last_slot_ = 0;
  bool recorded_ = false;  // a frame has been recorded since the pass was made
  bool cleared_ = false;   // the histogram has been zeroed once
  u64 last_frame_index_ = 0;
  u32 groups_x_ = 0;
  u32 groups_y_ = 0;
};

}  // namespace engine::renderer
