#pragma once

// **Posing a scene's trees every frame, on the frame's thread and a pool of its own**
// (docs/subsystems/renderer.md, "The flora stage", "Who poses the trees"). `flora_part_poses` is
// one instance's parts; this is every swaying instance of a scene: each one's wind and the walker's
// capsule taken into its mesh frame, its parts posed into its run of the frame's part-pose table,
// its push state advanced, and the frame's two answers (how many parts the walker bent, whether any
// part of any tree is off its rest pose) gathered.
//
// **Why a pool, and why its own.** A forest of 4,358 trees posed every part on one thread for about
// 20 ms a frame in a release build, at any resolution, while the GPU drew it in 4 to 5
// (docs/experiments/forest-cpu-2026-10-21.md). An instance writes only its own run of the table,
// its own run of the push state and its own record, and reads only what the load built, so runs of
// instances are independent and the work splits without a lock. The renderer has no frame-time job
// system to borrow — the time-lapse's pool is `MovingGround`'s, sized to leave the frame's thread
// CPUs of its own, and a page read's is its host's — so the poser makes one, and only for a scene
// with enough parts to pay for waking it (`k_flora_chunk_parts`); a scene of a few trees poses
// them inline, as it always did.
//
// **The same bytes for any worker count.** Each instance runs exactly the serial loop's arithmetic
// in the same order, so a pose and a push state do not depend on which thread computed them; the
// chunks' counts are summed and their "moving" flags or-ed, which no order changes. The chunk
// table is fixed at load. `flora_poser_tests.cpp` holds every worker count to the serial loop byte
// for byte, with and without a walker.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/flora_motion.h>
#include <systems/renderer/flora_motion.h>

#include <memory>
#include <span>

namespace engine::jobs {
class JobSystem;
}

namespace engine::renderer {

// One swaying instance as the poser reads it: its scene instance, its mesh, where its run of the
// table starts, its seed, its mesh frame's inverse and its last frame index.
struct FloraInstance {
  u32 instance = 0;
  u32 mesh = 0;
  u32 first_part = 0;
  u32 seed = 0;
  Vec3 inverse[3] = {};  // rows: the instance's 3x3 inverted, world to mesh
  f32 inverse_scale = 1.0f;
  u64 last_tick = 0;
  bool posed = false;
};

// What every instance of a frame shares: the wind in the ground plane (a unit `wind_to`), its
// strength, the sway's clock, the frame index the push eases by, and the walker.
struct FloraPoseFrame {
  Vec2 wind_to{1.0f, 0.0f};
  f32 wind_strength = 0.0f;
  f64 time_s = 0.0;
  u64 frame_index = 0;
  const FrameFlora* flora = nullptr;  // the walker; null is none
};

struct FloraPoseResult {
  u32 pushed_parts = 0;  // parts the walker bent this frame (any ease)
  bool moving = false;   // any part of any tree off its rest pose
};

// What the poser reads, all of it the load's and none of it written by a frame but `instances`
// (their last tick), `state` (one per part) and the table.
struct FloraPoseInputs {
  std::span<FloraInstance> instances;
  std::span<const FloraMesh> meshes;
  std::span<const Vec4> bounds;  // per mesh: mesh-space centre and radius of its rest vertices
  std::span<const gfx::InstanceDesc> placed;
  std::span<FloraPartState> state;
  gfx::FloraPartPose* region = nullptr;  // this frame's region of the table, every tree's parts
};

// Poses instances [begin, end) of `in`: the loop every chunk runs, and the whole of the serial
// path.
FloraPoseResult flora_pose_instances(const FloraPoseInputs& in, const FloraPoseFrame& frame,
                                     u32 begin, u32 end) noexcept;

// The fewest parts a chunk is cut to, and so the fewest a scene needs for a second thread: tens of
// microseconds of posing in a release build, several times a sleeping worker's wake-up
// (`engine_renderer_bench`'s `renderer.flora.pose.*` rows; renderer.md, "Who poses the trees").
inline constexpr u32 k_flora_chunk_parts = 512;
// The most chunks: `jobs::JobSystem::parallel_for` keeps 64 in place before it allocates.
inline constexpr u32 k_flora_max_chunks = 64;

// `renderer.flora.workers`: the poser's threads besides the frame's own; 0 is automatic, one per
// performance CPU but the frame thread's, never more than the chunks can use.
u32 flora_workers_tunable() noexcept;

class FloraPoser {
 public:
  FloraPoser() noexcept;
  ~FloraPoser();
  ENGINE_NON_COPYABLE(FloraPoser);

  // Cuts `instances` into chunks of about equal part counts, at least `k_flora_chunk_parts` each,
  // at most `k_flora_max_chunks`, and four for each thread; when there are two or more, starts a
  // job system of `workers` threads (0: automatic; one chunk: none, every frame inline).
  // `chunk_parts` is the fewest parts a chunk is cut to, `k_flora_chunk_parts` but for the bench
  // that measured it. Allocates: call it at load, after the instances are laid out.
  void prepare(std::span<const FloraInstance> instances, std::span<const FloraMesh> meshes,
               u32 workers, u32 chunk_parts = k_flora_chunk_parts);
  void reset() noexcept;

  // One frame: every chunk on the pool, the calling thread helping, or every instance inline when
  // there is no pool. Allocates nothing.
  FloraPoseResult pose(const FloraPoseInputs& in, const FloraPoseFrame& frame) noexcept;

  u32 chunk_count() const noexcept { return chunks_.empty() ? 0u : chunks_.size() - 1; }
  u32 worker_count() const noexcept;  // 0: posed inline

 private:
  Vector<u32> chunks_;  // chunk_count() + 1 instance indices: chunk c is [chunks_[c], chunks_[c+1])
  std::unique_ptr<jobs::JobSystem> jobs_;
};

}  // namespace engine::renderer
