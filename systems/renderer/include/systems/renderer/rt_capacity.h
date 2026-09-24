#pragma once

// How many cluster acceleration structures the frame's ray tracing chain holds room for
// (docs/subsystems/renderer.md, "The ray tracing chain's memory").
//
// **The structures are sized by the frame, not by the scene.** Until 2026-09-24 the chain was
// allocated for every (instance, cluster) pair of the scene in every view — `views * pairs`
// clusters, each reserved at the driver's worst case of 6,144 bytes — because any pair *could* be
// in some frame's cut. The desert overlook with the owner's landmarks as a three-monitor surround
// is 2.63 million such clusters, 16.2 GB of cluster structures and 20.6 GB of device memory in
// all, for a path whose frames build at most 43,291. What a frame builds is its cut plus its
// shadow casters, which the cull pass counts and the records pass totals on the device; this is
// the policy that keeps the allocation a step above that number instead.
//
// **Grow ahead, shrink late, never every frame.** The demand is known one to `frames_in_flight`
// frames late (the renderer never reads a buffer back inside a frame), so the capacity keeps
// `headroom` above it: it grows as soon as a frame's demand eats into half of that headroom, to the
// demand plus the whole headroom rounded up to a `step`, and it shrinks only when a whole window of
// `shrink_frames` frames has peaked below half of what it holds — to that peak plus the headroom.
// Between those events nothing is allocated: a resize is a wait for the device and a new set of
// buffers, which is a hitch, so it happens on a trend and not on a frame. Along the desert path the
// demand moves by at most 1.7% a frame (docs/experiments/flythrough-desert-overlook.md), so a 25%
// headroom is many frames of warning.
//
// **The budget is a ceiling, and what happens above it is stated.** `limit` is the most the
// capacity may reach (`RenderSettings::rt_budget_mib`, or the `renderer.rt.budget_mib` tunable,
// turned into clusters by what the driver says one costs). A frame that wants more than the
// capacity holds — past the budget, or in the frames before a growth lands — keeps what fits and
// drops the rest **whole instances at a time, every instance's drawn clusters before any shadow
// caster** (gfx::ClusterRecordParams): an instance that lost its drawn clusters casts no shadow
// and, under the ray path, is missing from the traced picture; one that lost only its casters
// loses the shadows of the surfaces facing away from the camera. `Stats::rt_*` counts every such
// frame and the renderer says so once.
//
// This class is the arithmetic only — no device, no buffers — so the hysteresis can be tested on
// the CPU. `SceneRenderer` feeds it each completed frame's demand and resizes the `GpuScene` when
// it answers with a different capacity.

#include <core/base/types.h>

namespace engine::renderer {

struct RtCapacityConfig {
  u32 limit = 0;          // the most the capacity may be, in clusters: the budget
  u32 step = 4096;        // capacities are multiples of this (and at least one step)
  u32 headroom_pct = 25;  // kept above the demand
  u32 shrink_frames = 240;
};

// The `renderer.rt.*` tunables, read once: the budget in MiB, the headroom, the shrink window and
// the step. `RenderSettings::rt_budget_mib` overrides the budget when it is not zero.
u32 rt_budget_mib_tunable() noexcept;
u32 rt_headroom_pct_tunable() noexcept;
u32 rt_shrink_frames_tunable() noexcept;
u32 rt_step_tunable() noexcept;
inline constexpr u32 k_default_rt_budget_mib = 1024;
// The first capacity of a scene too large to hold whole within the budget, before any frame has
// said what it builds: 65,536 clusters is 400 MB on the RTX 5090, above every first frame the
// desert overlook's path draws at any resolution. A scene whose every pair in every view fits in
// less is allocated for all of them, exactly as before, and so never drops anything.
inline constexpr u32 k_initial_rt_clusters = 65536;

class RtCapacity {
 public:
  // Starts at `capacity` (clamped to the limit) with an empty shrink window.
  void reset(const RtCapacityConfig& config, u32 capacity) noexcept;
  // One completed frame wanted `wanted` structures. Returns the capacity the set should have from
  // now on, which is `capacity()` unless this frame crossed a threshold; the caller resizes when
  // the two differ and then calls `resized`.
  u32 observe(u32 wanted) noexcept;
  // The capacity that holds `wanted` with the headroom, rounded up to the step and clamped to the
  // limit. What `observe` grows to.
  u32 target(u32 wanted) const noexcept;
  // What a caller that must not drop anything this frame (a frame somebody waits for) grows to:
  // `target(wanted)`, with the shrink window restarted from `wanted`, so the growth is not undone
  // by a window that never saw the frame that asked for it.
  u32 grow_to(u32 wanted) noexcept;
  void resized(u32 capacity) noexcept;

  u32 capacity() const noexcept { return capacity_; }
  u32 limit() const noexcept { return config_.limit; }
  const RtCapacityConfig& config() const noexcept { return config_; }

 private:
  RtCapacityConfig config_;
  u32 capacity_ = 0;
  u32 window_peak_ = 0;
  u32 window_frames_ = 0;
};

}  // namespace engine::renderer
