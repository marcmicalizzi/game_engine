#pragma once

// **Which level of each texture a frame's samples asked for** (docs/subsystems/texture.md, "Levels
// resident by what the frame samples"; roadmap R172). One pass (shaders/texture_feedback.slang),
// two uses:
//
// - **The measurement**, off unless `renderer.textures.feedback` names a file: a frame pass of the
//   renderer's own takes every pixel of every view the way the resolve does, works out for each
//   textured slot of its material the level a 16x anisotropic trilinear sample reads first, and
//   keeps the smallest per bindless slot; the table comes back one frame slot later through a
//   host-visible copy, and the CPU folds it into a per-texture account written as one JSON line a
//   frame and a closing line per texture. Beside it, the same frame runs the residency's sparse
//   pattern (one pixel in sixteen) into a second table, so the line says what the sparse one
//   missed, and counts the pixels a texture the residency trimmed drew softer than they asked.
// - **The residency's own** (stage 2; `create_residency`): one pixel in sixteen, a phase that steps
//   through each 4x4 tile frame by frame, over the run of slots the residency manages, folded into
//   `GpuScene::texture_residency_fold`. Not a frame pass: `SceneRenderer::record_frame` records it
//   after the resolve, so it draws nothing through the linear image; its zone is "texture
//   residency".
//
// **What the measurement costs, and why it is a frame pass.** A frame pass is the renderer's
// registration point for "a compute pass over the visibility buffer after the resolve", so the
// measurement edits no hub; the price is that a frame with any frame pass draws through the linear
// image and the encode (frame_pass.h), which the measurement's own numbers then include.
//
// **The account** (per texture, a level being `max(0, floor(lambda))` of the smallest lambda over
// the whole chain: the slot's resident top added to the resident image's lambda):
// - `asked` this frame: the finest level any pixel read, or none;
// - the bytes resident against the bytes of the levels from the finest asked down (what a residency
//   by this frame's samples would hold), and against the levels from the finest asked over the
//   whole run (what no frame asked for at all);
// - the levels a residency that keeps what was asked within the last `hold` seconds would have had
//   to bring in this frame, and their bytes: how fast the asked-for level moves as the eye does.

#include <systems/renderer/frame_pass.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene_renderer.h>

#include <string>

namespace engine::renderer {

class TextureFeedback {
 public:
  // `renderer.textures.feedback`: the file to write, empty for none.
  static std::string path_wanted();
  // The measurement.
  bool create(const gfx::Device& device, GpuScene& scene, gfx::ShaderLibrary& shaders,
              u32 frames_in_flight, const std::string& path, std::string* error);
  // The residency's own: over the scene's managed run (`GpuScene::texture_residency_run`).
  bool create_residency(const gfx::Device& device, GpuScene& scene, gfx::ShaderLibrary& shaders,
                        u32 frames_in_flight, std::string* error);
  // Folds the slots still in flight (the caller has waited for the device) and writes the closing
  // lines.
  void destroy() noexcept;
  static bool record_thunk(const FramePassContext& context, void* user, std::string* error);
  // The frame's passes; `context` needs the graph, timer, bindless set, slot, frame index, step,
  // views, visibility and eye.
  bool record(const FramePassContext& context, std::string* error);
  u64 device_bytes() const noexcept;

  // The hold windows the account simulates, in seconds of the frames' own time.
  static constexpr u32 k_holds = 3;
  static constexpr f64 k_hold_s[k_holds] = {0.5, 2.0, 8.0};
  static constexpr u32 k_max_levels = 16;
  // The sparse pattern's tile: one pixel in `k_stride * k_stride`, at the phase `phase(frame)`.
  static constexpr u32 k_stride = 4;
  static void phase(u64 frame, u32& x, u32& y) noexcept;

 private:
  struct Texture {
    GpuScene::TextureRecord record;
    u64 level_bytes[k_max_levels] = {};
    i32 finest_ever = -1;        // the finest level any frame asked for; -1 none yet
    i32 finest_last = -1;        // the last folded frame's
    f64 asked_at[k_max_levels];  // the last time each level (or a finer one) was asked
    i32 held[k_holds];           // the finest level each hold kept after the last fold
    // The same, as the sparse pattern saw it (the 8 s hold only).
    i32 sparse_ever = -1;
    f64 sparse_asked_at[k_max_levels];
  };

  bool create_common(const gfx::Device& device, GpuScene& scene, gfx::ShaderLibrary& shaders,
                     u32 frames_in_flight, u32 table_words, std::string* error);
  void fold(u32 slot);
  void refresh_textures();
  u64 bytes_from(const Texture& t, i32 level) const noexcept;

  bool residency_ = false;
  const gfx::Device* device_ = nullptr;
  GpuScene* scene_ = nullptr;
  gfx::ComputePipeline pipeline_;
  // The measurement's table: the full pass's codes (`slot_count_` words), the sparse pass's, and
  // the four softer counts; the residency's: its run's codes alone.
  gfx::BufferResource table_;
  Vector<gfx::BufferResource> readback_;  // per frame slot: the table, host-visible
  Vector<gfx::BufferResource> params_;    // per frame slot: two blocks a view, host-visible
  Vector<gfx::BufferResource> tops_;      // per frame slot: each slot's resident top (measurement)
  Vector<u8> pending_;                    // per frame slot: a table copy not folded yet
  Vector<u64> pending_frame_;             // and the frame it was recorded for
  Vector<f64> pending_time_;
  u32 slot_base_ = 0;
  u32 slot_count_ = 0;
  u32 table_words_ = 0;
  u32 view_count_ = 0;
  u32 view_width_[k_max_views] = {};
  u32 view_height_[k_max_views] = {};
  Vector<Texture> textures_;  // by `scene_->texture_records` order
  Vector<i32> by_slot_;       // bindless slot -> index into textures_, -1 none
  u64 records_serial_ = 0;    // `GpuScene::texture_records_serial` when the list was made
  f64 time_s_ = 0.0;          // the frames' own clock: `FramePassContext::dt_s`, or 1/60 s
  u64 frames_ = 0;
  // The lines, kept and written once at `destroy` (reserved at create, so a frame's fold appends
  // without allocating until the reserve is spent).
  std::string path_;
  std::string lines_;
};

}  // namespace engine::renderer
