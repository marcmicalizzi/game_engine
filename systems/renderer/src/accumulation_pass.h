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
  // **Reactive surfaces** (R158 leg 3; renderer.md, "Sampling over time: the accumulation"): a
  // pixel whose material is water is always clipped and takes at least `reactive_weight` of the
  // new frame. 0 addresses in a scene with no water material, and then nothing is read.
  u64 materials = 0;          // gfx::ResolveMaterial[]
  u64 cluster_materials = 0;  // u32 per cluster
  u64 instances = 0;          // gfx::InstanceDesc[]
  f32 reactive_weight = 0.0f;
  u32 filter = 0;  // k_accumulate_filter_*
  // **Where each pixel's point was in depth a frame ago** (R194; `gfx::MotionParams::prev_depth`):
  // f32[width * height] for this view, which a history tap's depth in last frame's visibility
  // buffer is held to. 0: not kept, and no tap is held to a depth.
  u64 prev_depth = 0;
  // **The slots new to their instances this frame** (R194 leg 2; `MotionPass::fresh_address`):
  // `uint2 {begin, end}` runs, sorted. A pixel of one is held to its depth alone; a tap whose name
  // this frame's table puts in one names nothing. 0 / 0: none, every name and vector of the frame
  // stands unless the frame is not `k_accumulate_trusted`.
  u64 fresh = 0;
  // u32[8]: this frame's census (`AccumulationPass::Census`), counted by wave; 0: none.
  u64 census = 0;
  u32 fresh_count = 0;
  u32 pad = 0;
};
static_assert(sizeof(AccumulateParams) == 176, "accumulate.slang's AccumulateParams");

inline constexpr u32 k_accumulate_history = 1u;  // there is a frame before this one to read
inline constexpr u32 k_accumulate_mean = 2u;     // the running mean at the pixel's own place
inline constexpr u32 k_accumulate_names = 4u;    // last frame's names can be read through `pairs`
// **Every vector can be trusted to name where its surface was** (R194): the scene's layout did not
// change under this frame (`MotionPass::tables_changed`), so a deformed instance's second write
// read last frame's inputs and every mover the host moved is in the moved table. Without it a
// tap's motion matching the pixel's proves nothing (a tail's mover has the camera's vectors, a
// swaying tree its instance's), and only names and depths decide.
inline constexpr u32 k_accumulate_trusted = 8u;
// How a history is read between its texels (`renderer.reconstruction.history_filter`).
inline constexpr u32 k_accumulate_filter_bilinear = 0u;
inline constexpr u32 k_accumulate_filter_catmull_rom = 1u;

class AccumulationPass {
 public:
  struct Options {
    bool mean = false;  // `Accumulation::Mean`
    f32 weight = 0.1f;  // a new frame's share once the history is long
    f32 clip_still = 0.0f;
    f32 clip_moving = 0.0f;
    f32 motion_match = 0.25f;                      // `renderer.reconstruction.motion_match`, pixels
    f32 reactive_weight = 0.25f;                   // `renderer.reconstruction.reactive_weight`
    u32 filter = k_accumulate_filter_catmull_rom;  // `renderer.reconstruction.history_filter`
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
  // **What the frame that last used `slot` took from its history** (R194 leg 2; the frame record's
  // `history_*`): the pixels that had one to read, took one, took one with their own vectors
  // trusted, and were of an instance new to its slot, as the pass counted them; the fresh runs the
  // frame read and whether its tables could be followed at all. Read once the slot's frame is
  // complete (its statistics are folded); the next frame recorded in the slot clears it.
  struct Census {
    u32 pixels = 0;
    u32 taken = 0;
    u32 trusted = 0;
    u32 fresh = 0;
    u32 runs = 0;
    bool whole = false;
  };
  Census census(u32 slot) const noexcept;
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
  Vector<gfx::BufferResource> census_;  // per frame slot: u32[8], host-visible
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
