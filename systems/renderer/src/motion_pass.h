#pragma once

// **Sampling over time's inputs** (docs/subsystems/renderer.md, "Sampling over time: the inputs";
// ADR-0064; roadmap R154): everything a renderer with `renderer.reconstruction.inputs` on keeps
// that one without it does not — the frame's sub-pixel phase, last frame's views and eye, last
// frame's visibility buffer, the instances that moved and where they stood, the deformed-vertex
// pool as last frame's inputs wrote it, the motion buffer and the blocks its pass reads.
// `SceneRenderer` holds one behind a pointer that is null when the inputs are off, so a renderer
// without them makes no buffer, records no pass and draws the frame it always drew.
//
// A frame's order: `frame_offset` (the phase's offset, handed to `ViewSet::update`),
// `swap_visibility` (the frame draws into the buffer the frame before last drew into, and last
// frame's is kept), `prepare` (every view's block and the moved table, written into this frame
// slot's own buffers; then this frame becomes "last"), `previous` (the pool pass's block for its
// second write) and `record` (the pass's dispatches).

#include <domain/gfx/motion.h>
#include <systems/renderer/scene_renderer.h>

#include <span>
#include <string>

namespace engine::renderer {

// The most instances the moved table holds. An instance moved past it is not followed: its motion
// is the camera's, as a rigid instance's that never moved is, and the renderer says so once.
inline constexpr u32 k_motion_moved_capacity = 4096;

class MotionPass {
 public:
  // What a frame hands over: where its buffers are and what its pool pass read.
  struct Frame {
    u32 slot = 0;
    WorldPos eye{};  // the frame's camera, in f64: the eyes' difference is taken here
    gfx::FrameEye gpu_eye;
    u64 visibility = 0;                    // this frame's visibility buffer, its first word
    u64 vis_offset[k_max_views] = {};      // each view's first element of it
    u64 pair_entries[k_max_views] = {};    // each view's pair -> entry table, or 0
    u64 pairs = 0;
    u64 visible = 0;
    u64 instances = 0;
    u64 meshes = 0;
    u64 clusters = 0;
    u64 triangles = 0;
    u32 instance_count = 0;
    // The pool pass's inputs that change from frame to frame, as this frame's blocks carry them.
    u64 deform_table = 0;
    f32 deform_time = 0.0f;
    u64 flora = 0;
    // The scene's tables changed since the last frame (a streamed world's tail): an instance's
    // deform record is then not the one last frame's table holds at its index.
    bool tables_changed = false;
  };

  bool create(const gfx::Device& device, const GpuScene& scene, const ViewSet& views,
              u32 frames_in_flight, std::string* error);
  void destroy(const gfx::Device& device) noexcept;
  // The screen-sized two (the motion buffer, the kept visibility buffer), for a target resized.
  // The history does not survive it.
  bool resize(const gfx::Device& device, const ViewSet& views, std::string* error);
  // A camera cut, a teleport, a load: the next frame has no last frame (its motion is zero but
  // for the offset's) and the phase starts again.
  void reset() noexcept;

  void frame_offset(f32& x, f32& y) const noexcept;
  void swap_visibility(gfx::BufferResource& current) noexcept;
  // Before the scene writes `indices`' new placements: each one's transform as it stands, for the
  // instances not yet in the table. False once, when the table is full.
  bool note_moves(const GpuScene& scene, std::span<const u32> indices);
  void prepare(const Frame& frame, const ViewSet& views, const GpuScene& scene);
  // The pool pass's block for its second write: `current` with last frame's inputs and the second
  // pool. With no last frame to read (the first frame, one frame in flight, a table change) the
  // inputs are this frame's, and a deformed vertex's motion is its instance's and the camera's.
  gfx::DeformParams previous(const gfx::DeformParams& current) const noexcept;
  void record(gfx::CommandList cb, const gfx::ComputePipeline& pipeline, gfx::GpuTimer& timer,
              u32 slot) const;

  bool has_pool() const noexcept { return prev_pool_.buffer.valid(); }
  const gfx::BufferResource& output() const noexcept { return output_; }
  const gfx::BufferResource& prev_pool() const noexcept { return prev_pool_; }
  const gfx::BufferResource& prev_visibility() const noexcept { return prev_vis_; }
  u32 moved_count() const noexcept { return static_cast<u32>(moved_.size()); }
  // Whether the last prepared frame had a last frame to measure from.
  bool had_history() const noexcept { return had_history_; }
  // What the inputs keep on the device: the motion buffer, the kept visibility buffer, the second
  // pool, the blocks and the moved tables.
  u64 device_bytes() const noexcept;

 private:
  struct Last {
    bool valid = false;
    u32 slot = 0;
    WorldPos eye{};
    gfx::FrameEye gpu_eye;
    Mat4 view_proj[k_max_views];  // un-offset
    u64 deform_table = 0;
    f32 deform_time = 0.0f;
    u64 flora = 0;
  };

  gfx::BufferResource output_;    // u32 a source pixel, the views back to back
  gfx::BufferResource prev_vis_;  // last frame's visibility buffer
  gfx::BufferResource prev_pool_;
  Vector<gfx::BufferResource> params_;  // per frame slot: MotionParams a view, host-visible
  Vector<gfx::BufferResource> moved_buffers_;  // per frame slot: the moved table, host-visible
  Vector<gfx::MovedInstance> moved_;           // sorted by instance: last frame's transforms
  Last last_;
  // What `previous` hands the pool pass this frame.
  u64 prev_deform_table_ = 0;
  f32 prev_deform_time_ = 0.0f;
  u64 prev_flora_ = 0;
  bool prev_inputs_ = false;
  u32 view_count_ = 0;
  u32 view_width_[k_max_views] = {};
  u32 view_height_[k_max_views] = {};
  u64 phase_ = 0;
  bool had_history_ = false;
  bool full_warned_ = false;
};

}  // namespace engine::renderer
