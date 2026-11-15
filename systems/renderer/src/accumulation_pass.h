#pragma once

// **The engine's own accumulation at native resolution** (docs/subsystems/renderer.md, "Sampling
// over time: the accumulation"; docs/experiments/temporal-sampling.md, stage 2; ADR-0064; roadmap
// R158). A frame drawn at a sub-pixel offset is one sample of every pixel; this pass blends it into
// what the frames before it accumulated, looked up where the motion pass says each pixel's surface
// was and held to last frame's visibility buffer, so that a crown's edge is the average of its
// samples and not whichever one this frame drew.
//
// It is **a frame pass of the renderer's own** (frame_pass.h), registered at
// `FramePassStage::Reconstruction` when `renderer.reconstruction.accumulation` is not `off`, so it
// runs after every pass that composites light and before the encode, over the linear image those
// passes wrote. It writes **an image of its own**, which is both the frame's picture (the encode
// reads it in the linear image's place, through `FramePassReport::color`) and the next frame's
// history: two images, swapped each frame, 8 bytes a pixel each. Everything else it reads is the
// motion pass's (motion_pass.h): the vectors, last frame's visibility buffer, the frame's count
// since the last reset.
//
// A renderer without the switch holds a null pointer: no image, no pipeline, no pass.

#include <systems/renderer/frame_pass.h>
#include <systems/renderer/scene_renderer.h>

#include <string>

namespace engine::renderer {

class MotionPass;

// One view's block, behind one address (shaders/accumulate.slang mirrors it).
struct AccumulateParams {
  u64 visibility = 0;       // u64[width * height]: this view's region of this frame's buffer
  u64 prev_visibility = 0;  // the same region of last frame's
  u64 motion = 0;           // u32[width * height]: the motion pass's words for this view
  u64 pairs = 0;            // u32[2 * pair_count]: the scene's {instance, cluster} per pair
  u64 prev_motion = 0;      // u32[width * height]: last frame's motion words for this view
  u64 moved = 0;            // gfx::MovedInstance[moved_count], sorted by instance; 0: none
  u32 width = 0;
  u32 height = 0;
  u32 rect_x = 0;  // the view's rectangle of the three images
  u32 rect_y = 0;
  u32 color = 0;    // storage slot: the frame's linear colour
  u32 history = 0;  // storage slot: what the frames before accumulated
  u32 output = 0;   // storage slot: what this frame leaves
  u32 flags = 0;
  u32 pair_count = 0;
  f32 weight = 1.0f;        // this frame's share of a pixel whose history holds
  f32 clip_still = 0.0f;    // the colour clip's gamma for a pixel that did not move
  f32 clip_moving = 0.0f;   // and for one that moved a pixel or more; 0 or less: no clip
  f32 knee = 1.0f;          // the display curve the samples are averaged through: its knee
  f32 ceiling = 1.0f;       // and its ceiling (`FramePassContext::display_knee`, `display_ceiling`)
  f32 motion_match = 0.0f;  // pixels: a tap whose surface moved within this of the pixel's
  u32 moved_count = 0;
};
static_assert(sizeof(AccumulateParams) == 112, "accumulate.slang's AccumulateParams");

inline constexpr u32 k_accumulate_history = 1u;  // there is a frame before this one to read
inline constexpr u32 k_accumulate_mean = 2u;     // the running mean at the pixel's own place
inline constexpr u32 k_accumulate_names = 4u;    // last frame's names can be read through `pairs`

class AccumulationPass {
 public:
  struct Options {
    bool mean = false;  // `Accumulation::Mean`
    f32 weight = 0.1f;  // a new frame's share once the history is long
    f32 clip_still = 0.0f;
    f32 clip_moving = 0.0f;
    f32 motion_match = 0.25f;  // `renderer.reconstruction.motion_match`, pixels
  };

  bool create(const gfx::Device& device, GpuScene& scene, const MotionPass& motion,
              gfx::ShaderLibrary& shaders, u32 width, u32 height, u32 frames_in_flight,
              const Options& options, std::string* error);
  // The device is idle (every caller waits first).
  void destroy() noexcept;
  // The two images at the target's new size; the history does not survive it.
  bool resize(u32 width, u32 height, std::string* error);

  // The frame pass: `user` is the pass.
  static bool record_thunk(const FramePassContext& context, void* user, std::string* error);

  bool mean() const noexcept { return options_.mean; }
  // Frames accumulated into the picture the last recorded frame left, that frame included: 1 for a
  // frame with no history.
  u32 accumulated() const noexcept { return count_; }
  // What it keeps on the device: the two images and the per-slot blocks.
  u64 device_bytes() const noexcept;

 private:
  bool record(const FramePassContext& context, std::string* error);
  bool create_images(std::string* error);
  void destroy_images() noexcept;

  const gfx::Device* device_ = nullptr;
  GpuScene* scene_ = nullptr;
  const MotionPass* motion_ = nullptr;
  Options options_;
  gfx::ComputePipeline pipeline_;
  gfx::ImageResource images_[2];
  gfx::ImageViewHandle image_views_[2];
  u32 storage_slots_[2] = {gfx::BindlessSet::k_invalid_slot, gfx::BindlessSet::k_invalid_slot};
  u32 sampled_slots_[2] = {gfx::BindlessSet::k_invalid_slot, gfx::BindlessSet::k_invalid_slot};
  Vector<gfx::BufferResource> params_;  // per frame slot: AccumulateParams a view, host-visible
  u32 width_ = 0;
  u32 height_ = 0;
  u32 current_ = 0;         // the image the last recorded frame wrote
  bool written_ = false;    // it holds a picture
  u64 written_serial_ = 0;  // the motion pass's frame count when it was written
  u32 count_ = 0;
  u32 view_count_ = 0;
  u32 view_width_[k_max_views] = {};
  u32 view_height_[k_max_views] = {};
};

}  // namespace engine::renderer
