#include <foundation/tunables/tunables.h>
#include <systems/renderer/rt_capacity.h>

#include <algorithm>

namespace engine::renderer {

namespace {

// Read once, when a scene is created or a renderer is (docs/subsystems/renderer.md, "The ray
// tracing chain's memory"). They live in this file because it is the one every reader links:
// `GpuScene` turns the budget into a cluster count and `SceneRenderer` reads the other two.
tunables::Int rt_budget_mib{"renderer.rt.budget_mib", 0, 0, 1 << 20,
                            "Most device memory the per-frame cluster acceleration structures "
                            "may take, in MiB; 0 follows the device (a sixteenth of its memory, "
                            "512 to 4,096 MiB); a frame past it drops whole instances' structures, "
                            "shadow casters first"};
tunables::Int rt_headroom_pct{"renderer.rt.headroom_pct", 25, 0, 400,
                              "Room the cluster acceleration structures keep above what recent "
                              "frames built, as a percentage of it"};
// R116 (2026-11-02): the window is wall time, not frames — a load's fast frames were a whole window
// of 240 three seconds after the start — and a shrink keeps half the last minute's peak, which a
// growth goes straight back to (rt_capacity.h).
tunables::Float rt_shrink_seconds{"renderer.rt.shrink_seconds", 10.0, 0.0, 86400.0,
                                  "Seconds of wall time the ray tracing chain's demand must stay "
                                  "under half its capacity before the capacity shrinks"};
tunables::Float rt_peak_seconds{"renderer.rt.peak_seconds", 60.0, 0.0, 86400.0,
                                "Seconds the ray tracing chain remembers the largest capacity it "
                                "held: a growth goes back to it, and a shrink keeps a share of it"};
tunables::Int rt_floor_pct{"renderer.rt.floor_pct", 50, 0, 100,
                           "The least share of the last peak_seconds' largest capacity a shrink "
                           "of the ray tracing chain leaves, as a percentage"};
tunables::Int rt_shrink_frames{"renderer.rt.shrink_frames", 240, 0, 1 << 24,
                               "Frames a shrink window of the ray tracing chain holds at the "
                               "least, beside its shrink_seconds"};
tunables::Int rt_peak_frames{"renderer.rt.peak_frames", 3600, 0, 1 << 24,
                             "Frames the ray tracing chain remembers the largest capacity it held "
                             "for at the least, beside peak_seconds"};
tunables::Bool rt_swap_resize{
    "renderer.rt.swap_resize", true,
    "Make the ray tracing chain's new buffers while the frames in flight read "
    "the old ones and free those when the frames complete, instead of waiting "
    "for the device first (R116)"};
tunables::Bool rt_swap_references{
    "renderer.rt.swap_references", true,
    "Swap a step of the cluster cache's references like the chain's own buffers, "
    "each frame slot reading its own copy of the cache's table, instead of "
    "waiting for the device (R130)"};
tunables::Bool rt_scan_near_capacity{
    "renderer.rt.scan_near_capacity", true,
    "Record the drop order's scan while the last read-back demand is at least half "
    "the ray tracing chain's capacity, not only after a frame that dropped (R130)"};
tunables::Int rt_step{"renderer.rt.step_clusters", 4096, 1, 1 << 24,
                      "The ray tracing chain's capacity moves in multiples of this many clusters "
                      "(4,096 is 24 MiB of cluster structures on the RTX 5090)"};

tunables::Bool rt_static_forms{
    "renderer.rt.static_forms", true,
    "Draw a streamed world's rigid distance forms at their fixed cut and "
    "trace them against structures built once, instead of building every "
    "instance's cut every frame"};

tunables::Bool rt_static_build{
    "renderer.rt.static_build", true,
    "With static forms, build their structures once; false draws them at "
    "their fixed cut but builds them every frame, to compare against"};

tunables::Int rt_initial_clusters{"renderer.rt.initial_clusters", k_initial_rt_clusters, 1, 1 << 24,
                                  "The ray tracing chain's first capacity is at least this many "
                                  "clusters where the scene could want more, before the first "
                                  "frame has said what it builds"};

tunables::Bool rt_caster_horizon{
    "renderer.rt.caster_horizon", false,
    "With traced shadows and a shadow distance, build no shadow caster "
    "farther from the eye than that distance"};

// The resident cluster structure cache (renderer.md, "The resident cluster structure cache"; R101).
tunables::Bool rt_cache{
    "renderer.rt.cache", true,
    "Build a rigid mesh's cluster structures once, into a cache every instance "
    "and frame references, instead of once per instance per frame (R101; on "
    "again since R106, which made a cache that references nothing cost nothing)"};
tunables::Int rt_cache_mib{
    "renderer.rt.cache_mib", 0, 0, 1 << 20,
    "Most device memory the resident cluster structure cache may take, in "
    "MiB; 0 follows the device (a sixty-fourth of its memory, 256 to 1,024 "
    "MiB); it takes what the scene's cached clusters need up to this, a full "
    "cache evicts the least recently drawn, and what a frame cannot place "
    "in it the frame builds itself"};
tunables::Int rt_cache_slots{"renderer.rt.cache_slots", 0, 0, 1 << 30,
                             "A ceiling on the cluster cache's slots beside its MiB, for "
                             "measurement and the eviction test; 0 is none"};
tunables::Int rt_reference_mib{"renderer.rt.reference_mib", k_default_rt_reference_mib, 1, 1 << 20,
                               "Most device memory the frame's references to the cluster cache "
                               "may take, in MiB; past it a frame drops whole instances"};

u64 round_up(u64 value, u64 step) noexcept { return (value + step - 1) / step * step; }

}  // namespace

u32 rt_budget_mib_tunable() noexcept { return static_cast<u32>(rt_budget_mib.get()); }
u32 rt_headroom_pct_tunable() noexcept { return static_cast<u32>(rt_headroom_pct.get()); }
f64 rt_shrink_seconds_tunable() noexcept { return rt_shrink_seconds.get(); }
f64 rt_peak_seconds_tunable() noexcept { return rt_peak_seconds.get(); }
u32 rt_floor_pct_tunable() noexcept { return static_cast<u32>(rt_floor_pct.get()); }
u32 rt_shrink_frames_tunable() noexcept { return static_cast<u32>(rt_shrink_frames.get()); }
u32 rt_peak_frames_tunable() noexcept { return static_cast<u32>(rt_peak_frames.get()); }
u32 rt_step_tunable() noexcept { return static_cast<u32>(rt_step.get()); }
bool rt_swap_resize_tunable() noexcept { return rt_swap_resize.get(); }
bool rt_swap_references_tunable() noexcept { return rt_swap_references.get(); }
bool rt_scan_near_capacity_tunable() noexcept { return rt_scan_near_capacity.get(); }
bool rt_static_forms_tunable() noexcept { return rt_static_forms.get(); }
bool rt_static_build_tunable() noexcept { return rt_static_build.get(); }
bool rt_caster_horizon_tunable() noexcept { return rt_caster_horizon.get(); }
u32 rt_initial_clusters_tunable() noexcept { return static_cast<u32>(rt_initial_clusters.get()); }
bool rt_cache_tunable() noexcept { return rt_cache.get(); }
u32 rt_cache_mib_tunable() noexcept { return static_cast<u32>(rt_cache_mib.get()); }
u32 rt_reference_mib_tunable() noexcept { return static_cast<u32>(rt_reference_mib.get()); }
u32 rt_cache_slots_tunable() noexcept { return static_cast<u32>(rt_cache_slots.get()); }

u32 rt_device_budget_mib(u64 device_local_bytes) noexcept {
  const u64 share = device_local_bytes / k_rt_budget_device_share / (u64{1} << 20);
  if (share < k_rt_budget_floor_mib) return k_rt_budget_floor_mib;
  if (share > k_rt_budget_ceiling_mib) return k_rt_budget_ceiling_mib;
  return static_cast<u32>(share);
}

const char* rt_budget_source_name(RtBudgetSource source) noexcept {
  switch (source) {
    case RtBudgetSource::Setting: return "setting";
    case RtBudgetSource::Tunable: return "tunable";
    case RtBudgetSource::Device: return "device";
  }
  return "device";
}

RtBudget resolve_rt_budget(u32 setting_mib, u32 tunable_mib, u64 device_local_bytes) noexcept {
  if (setting_mib != 0) return {setting_mib, RtBudgetSource::Setting};
  if (tunable_mib != 0) return {tunable_mib, RtBudgetSource::Tunable};
  return {rt_device_budget_mib(device_local_bytes), RtBudgetSource::Device};
}

u32 rt_device_cache_mib(u64 device_local_bytes) noexcept {
  const u64 share = device_local_bytes / k_rt_cache_device_share / (u64{1} << 20);
  if (share < k_rt_cache_floor_mib) return k_rt_cache_floor_mib;
  if (share > k_rt_cache_ceiling_mib) return k_rt_cache_ceiling_mib;
  return static_cast<u32>(share);
}

RtBudget resolve_rt_cache_mib(u32 tunable_mib, u64 device_local_bytes) noexcept {
  if (tunable_mib != 0) return {tunable_mib, RtBudgetSource::Tunable};
  return {rt_device_cache_mib(device_local_bytes), RtBudgetSource::Device};
}

void rt_drop_order(u32 prefix_slots, std::span<RtDropBlock> blocks, Vector<u32>& out) {
  std::sort(blocks.begin(), blocks.end(), [](const RtDropBlock& a, const RtDropBlock& b) {
    return a.rank != b.rank ? a.rank < b.rank
           : a.key != b.key ? a.key < b.key
                            : a.first_slot < b.first_slot;
  });
  u64 count = prefix_slots;
  for (const RtDropBlock& block : blocks)
    count += block.slots;
  out.resize(static_cast<u32>(2 + 2 * count));
  out[0] = static_cast<u32>(count);
  out[1] = 0;
  u32 at = 2;
  for (u32 s = 0; s < prefix_slots; ++s) {
    out[at++] = s;
    out[at++] = 0;
  }
  for (const RtDropBlock& block : blocks) {
    for (u32 s = 0; s < block.slots; ++s) {
      out[at++] = block.first_slot + s;
      out[at++] = block.rank;
    }
  }
}

u32 first_rt_capacity(u32 limit, u32 union_clusters, u64 estimate, u32 floor) noexcept {
  u64 first = estimate > floor ? estimate : u64{floor};
  if (first > union_clusters) first = union_clusters;
  if (first > limit) first = limit;
  return static_cast<u32>(first > 0 ? first : 1);
}

void RtCapacity::reset(const RtCapacityConfig& config, u32 capacity, f64 now) noexcept {
  config_ = config;
  if (config_.step == 0) config_.step = 1;
  if (!(config_.shrink_seconds >= 0.0)) config_.shrink_seconds = 0.0;
  if (!(config_.peak_seconds >= 0.0)) config_.peak_seconds = 0.0;
  if (config_.floor_pct > 100) config_.floor_pct = 100;
  capacity_ = capacity < config_.limit ? capacity : config_.limit;
  window_peak_ = 0;
  window_start_ = now;
  window_start_frame_ = 0;
  frames_ = 0;
  held_first_ = 0;
  held_count_ = 0;
}

bool RtCapacity::remembered(f64 until, u64 until_frame, f64 now) const noexcept {
  return now - until <= config_.peak_seconds || frames_ - until_frame <= config_.peak_frames;
}

u32 RtCapacity::target(u32 wanted) const noexcept {
  const u64 with_headroom = u64{wanted} + u64{wanted} * config_.headroom_pct / 100;
  u64 capacity = round_up(with_headroom > 0 ? with_headroom : 1, config_.step);
  if (capacity > config_.limit) capacity = config_.limit;
  return static_cast<u32>(capacity);
}

u32 RtCapacity::peak(f64 now) const noexcept {
  u32 most = capacity_;
  // The bottom that has not expired is the largest that has not (`hold` keeps them falling).
  for (u32 i = 0; i < held_count_; ++i) {
    const Held& held = held_[(held_first_ + i) % k_held];
    if (remembered(held.until, held.until_frame, now)) {
      most = held.capacity > most ? held.capacity : most;
      break;
    }
  }
  return most;
}

u32 RtCapacity::floor(f64 now) const noexcept {
  const u64 share = u64{peak(now)} * config_.floor_pct / 100;
  if (share == 0) return 0;
  u64 least = round_up(share, config_.step);
  if (least > config_.limit) least = config_.limit;
  return static_cast<u32>(least);
}

u32 RtCapacity::grow_target(u32 wanted, f64 now) const noexcept {
  const u32 fit = target(wanted);
  const u32 most = peak(now);
  return fit > most ? fit : most;
}

u32 RtCapacity::observe(u32 wanted, f64 now) noexcept {
  ++frames_;
  window_peak_ = wanted > window_peak_ ? wanted : window_peak_;
  // Grow as soon as the demand has eaten half the headroom: the next frames are already in flight
  // and will want about as much again, and the demand is only known this late. To the peak of the
  // last minute when that is more, so a view that comes back regrows in one step and not in a
  // staircase behind a demand that grows faster than the headroom.
  const u64 trigger = u64{wanted} + u64{wanted} * config_.headroom_pct / 200;
  if (trigger > capacity_ && capacity_ < config_.limit) {
    window_peak_ = wanted;
    window_start_ = now;
    window_start_frame_ = frames_;
    const u32 grown = grow_target(wanted, now);
    return grown > capacity_ ? grown : capacity_;
  }
  // Shrink only on a whole window — its seconds and its frames, so neither a load's fast frames nor
  // a handful of slow ones make one — and only when even its peak left more than half unused, so a
  // path that swings between two cuts settles on the larger rather than resizing on every swing;
  // and never below the floor, so a look away keeps half the chain it looked away from. The
  // window's peak includes this frame, and the headroom is kept above it, so the shrink never
  // takes away what this frame built.
  if (now - window_start_ >= config_.shrink_seconds &&
      frames_ - window_start_frame_ >= config_.shrink_frames) {
    u32 fit = target(window_peak_);
    window_peak_ = 0;
    window_start_ = now;
    window_start_frame_ = frames_;
    if (u64{fit} * 2 <= capacity_) {
      const u32 least = floor(now);
      fit = fit > least ? fit : least;
      if (fit < capacity_) return fit;
    }
  }
  return capacity_;
}

u32 RtCapacity::grow_to(u32 wanted, f64 now) noexcept {
  window_peak_ = wanted;
  window_start_ = now;
  window_start_frame_ = frames_;
  const u32 fit = grow_target(wanted, now);
  return fit > capacity_ ? fit : capacity_;
}

void RtCapacity::hold(u32 previous, f64 until) noexcept {
  // A capacity no larger than one held after it can never be the peak again: drop it from the top.
  while (held_count_ > 0) {
    const Held& last = held_[(held_first_ + held_count_ - 1) % k_held];
    if (last.capacity > previous) break;
    --held_count_;
  }
  if (held_count_ == k_held) {
    held_first_ = (held_first_ + 1) % k_held;
    --held_count_;
  }
  held_[(held_first_ + held_count_) % k_held] = Held{previous, until, frames_};
  ++held_count_;
}

void RtCapacity::relimit(u32 limit, u32 capacity, f64 now) noexcept {
  config_.limit = limit;
  resized(capacity, now);
}

void RtCapacity::resized(u32 capacity, f64 now) noexcept {
  const u32 next = capacity < config_.limit ? capacity : config_.limit;
  if (next != capacity_) hold(capacity_, now);
  capacity_ = next;
}

}  // namespace engine::renderer
