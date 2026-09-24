#include <foundation/tunables/tunables.h>
#include <systems/renderer/rt_capacity.h>

namespace engine::renderer {

namespace {

// Read once, when a scene is created or a renderer is (docs/subsystems/renderer.md, "The ray
// tracing chain's memory"). They live in this file because it is the one every reader links:
// `GpuScene` turns the budget into a cluster count and `SceneRenderer` reads the other two.
tunables::Int rt_budget_mib{"renderer.rt.budget_mib", k_default_rt_budget_mib, 16, 1 << 20,
                            "Most device memory the per-frame cluster acceleration structures "
                            "may take, in MiB; a frame past it drops whole instances' structures, "
                            "shadow casters first"};
tunables::Int rt_headroom_pct{"renderer.rt.headroom_pct", 25, 0, 400,
                              "Room the cluster acceleration structures keep above what recent "
                              "frames built, as a percentage of it"};
tunables::Int rt_shrink_frames{"renderer.rt.shrink_frames", 240, 1, 1 << 20,
                               "Frames the ray tracing chain's demand must stay under half its "
                               "capacity before the capacity shrinks"};
tunables::Int rt_step{"renderer.rt.step_clusters", 4096, 1, 1 << 24,
                      "The ray tracing chain's capacity moves in multiples of this many clusters "
                      "(4,096 is 24 MiB of cluster structures on the RTX 5090)"};

u64 round_up(u64 value, u64 step) noexcept { return (value + step - 1) / step * step; }

}  // namespace

u32 rt_budget_mib_tunable() noexcept { return static_cast<u32>(rt_budget_mib.get()); }
u32 rt_headroom_pct_tunable() noexcept { return static_cast<u32>(rt_headroom_pct.get()); }
u32 rt_shrink_frames_tunable() noexcept { return static_cast<u32>(rt_shrink_frames.get()); }
u32 rt_step_tunable() noexcept { return static_cast<u32>(rt_step.get()); }

void RtCapacity::reset(const RtCapacityConfig& config, u32 capacity) noexcept {
  config_ = config;
  if (config_.step == 0) config_.step = 1;
  if (config_.shrink_frames == 0) config_.shrink_frames = 1;
  capacity_ = capacity < config_.limit ? capacity : config_.limit;
  window_peak_ = 0;
  window_frames_ = 0;
}

u32 RtCapacity::target(u32 wanted) const noexcept {
  const u64 with_headroom = u64{wanted} + u64{wanted} * config_.headroom_pct / 100;
  u64 capacity = round_up(with_headroom > 0 ? with_headroom : 1, config_.step);
  if (capacity > config_.limit) capacity = config_.limit;
  return static_cast<u32>(capacity);
}

u32 RtCapacity::observe(u32 wanted) noexcept {
  window_peak_ = wanted > window_peak_ ? wanted : window_peak_;
  ++window_frames_;
  // Grow as soon as the demand has eaten half the headroom: the next frames are already in flight
  // and will want about as much again, and the demand is only known this late.
  const u64 trigger = u64{wanted} + u64{wanted} * config_.headroom_pct / 200;
  if (trigger > capacity_ && capacity_ < config_.limit) {
    window_peak_ = wanted;
    window_frames_ = 0;
    return target(wanted) > capacity_ ? target(wanted) : capacity_;
  }
  // Shrink only on a whole window, and only when even its peak left more than half unused, so a
  // path that swings between two cuts settles on the larger rather than resizing on every swing.
  if (window_frames_ >= config_.shrink_frames) {
    const u32 fit = target(window_peak_);
    window_peak_ = 0;
    window_frames_ = 0;
    if (u64{fit} * 2 <= capacity_) return fit;
  }
  return capacity_;
}

u32 RtCapacity::grow_to(u32 wanted) noexcept {
  window_peak_ = wanted;
  window_frames_ = 0;
  const u32 fit = target(wanted);
  return fit > capacity_ ? fit : capacity_;
}

void RtCapacity::resized(u32 capacity) noexcept {
  capacity_ = capacity < config_.limit ? capacity : config_.limit;
}

}  // namespace engine::renderer
