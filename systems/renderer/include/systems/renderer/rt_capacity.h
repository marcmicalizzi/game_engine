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
// `headroom` above it: it grows as soon as a frame's demand eats into half of that headroom, and it
// shrinks only when a whole window — `shrink_seconds` of wall time and `shrink_frames` frames, both
// — has peaked below half of what it holds. Between those events nothing is allocated: a resize is
// a new set of buffers, which is a hitch, so it happens on a trend and not on a frame. Along the
// desert path the demand moves by at most 1.7% a frame
// (docs/experiments/flythrough-desert-overlook.md), so a 25% headroom is many frames of warning.
//
// **A view that comes back regrows once, and a glance frees nothing** (roadmap R116, 2026-11-02;
// docs/experiments/rt-capacity-2026-11-02.md). The policy remembers the largest capacity it held in
// the last `peak_seconds` (a minute) or `peak_frames` frames, whichever is longer. A growth goes to
// the larger of the demand plus the headroom and that peak, so the first growth after a shrink goes
// straight back to what the chain held, and a shrink goes to the larger of the window's peak plus
// the headroom and `floor_pct` of that peak (a half), so a shrink after a forest is to half the
// forest's chain, and the chain halves at most once a minute while the frames keep wanting little.
// Until then the window was 240 frames and the growth the demand plus the headroom: on the owner's
// v25 walk the load's fast frames were a whole window three seconds after the start (336,348 →
// 40,960), a look at the sea another (→ 12,288), and each way back grew in runs of five to eight
// resizes a second behind a demand that grew faster than the headroom, 48 resizes of 30–153 ms
// while 754 frames built less than they wanted. Wall time alone is not enough either way: a replay
// of that walk whose ground took a second a frame made a ten-second window of 14 frames and forgot
// a minute's peak within 90, so a window needs its frames too (the old 240), and a peak is kept for
// its frames too (3,600, a minute at 60 a second).
//
// **The budget is a ceiling, and what happens above it is stated.** `limit` is the most the
// capacity may reach (`RenderSettings::rt_budget_mib`, the `renderer.rt.budget_mib` tunable, or by
// default a sixteenth of the device's memory — `resolve_rt_budget` — turned into clusters by what
// the driver says one costs). A frame that wants more than the
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
#include <core/containers/vector.h>

#include <span>

namespace engine::renderer {

struct RtCapacityConfig {
  u32 limit = 0;              // the most the capacity may be, in clusters: the budget
  u32 step = 4096;            // capacities are multiples of this (and at least one step)
  u32 headroom_pct = 25;      // kept above the demand
  f64 shrink_seconds = 10.0;  // a shrink is judged by a window of this much wall time
  f64 peak_seconds = 60.0;    // how long a capacity held is remembered: the regrowth and the floor
  u32 floor_pct = 50;         // a shrink keeps at least this share of the peak
  u32 shrink_frames = 240;    // a window also holds at least this many frames
  u32 peak_frames = 3600;     // a capacity is also remembered for at least this many frames
};

// The `renderer.rt.*` tunables, read once: the budget in MiB, the headroom, the shrink window, the
// peak's memory, the floor and the step. `RenderSettings::rt_budget_mib` overrides the budget when
// it is not zero, and the budget tunable overrides the device's rule (below) when it is not zero;
// 0, its default, follows the device.
u32 rt_budget_mib_tunable() noexcept;
u32 rt_headroom_pct_tunable() noexcept;
f64 rt_shrink_seconds_tunable() noexcept;
f64 rt_peak_seconds_tunable() noexcept;
u32 rt_floor_pct_tunable() noexcept;
u32 rt_shrink_frames_tunable() noexcept;
u32 rt_peak_frames_tunable() noexcept;
u32 rt_step_tunable() noexcept;
// `renderer.rt.swap_resize` (true): a resize makes the new buffers while the frames in flight read
// the old ones and frees those when they complete, instead of waiting for the device (R116;
// `GpuScene::resize_ray_tracing`'s `retired`). Read at each resize; false is the comparison.
bool rt_swap_resize_tunable() noexcept;
// `renderer.rt.static_forms` (true): rigid distance forms keep structures built once
// (`RenderSettings::rt_static_forms`; renderer.md, "Static structures for rigid forms").
bool rt_static_forms_tunable() noexcept;
// `renderer.rt.static_build` (true): with static forms, build their structures once. False keeps
// the forms' fixed cut and builds it every frame — the comparison the tests and the measurement
// make.
bool rt_static_build_tunable() noexcept;
// `renderer.rt.caster_horizon` (false): `RenderSettings::rt_caster_horizon` from the command line.
bool rt_caster_horizon_tunable() noexcept;
// **The default budget follows the device** (renderer.md, "The budget follows the device"; roadmap
// R107, 2026-10-27). Until then it was 1,024 MiB on every card, chosen on the desert overlook,
// whose chain peaks at 134 MB; the forest proof of concept's expanded forest at 11520×2160 wants
// 243,604 structures a frame — 1.46 GiB at the RTX 5090's 6,426 bytes a cluster, 1.84 GiB at the
// capacity the policy grows to for it (25% headroom, rounded to its step) — and dropped instances
// on every frame at 1 GiB. The rule is a sixteenth
// of the device-local heaps (the heaps' size, which `gfx::MemoryBudget::device_local_bytes` knows
// with or without VK_EXT_memory_budget and which does not move with what other processes hold, so
// the same card gives the same capacity every run), never under `k_rt_budget_floor_mib` and never
// over `k_rt_budget_ceiling_mib`: 2,025 MiB on the 32 GB RTX 5090, 1,536 on 24 GB, 1,024 on 16 GB
// (the old default), 768 on 12 GB, 512 on 8 GB and on anything smaller. The budget is a ceiling,
// not an allocation: a frame that wants less allocates less (`RtCapacity`), so a bigger ceiling
// costs only the scenes that use it — but a scene past it holds it from the first frame
// (`first_rt_capacity`), which is why the share is a sixteenth and not more.
inline constexpr u32 k_rt_budget_device_share = 16;   // the budget is 1/16 of device-local memory
inline constexpr u32 k_rt_budget_floor_mib = 512;     // a 6 or 8 GB card
inline constexpr u32 k_rt_budget_ceiling_mib = 4096;  // a 64 GB card and up
// The rule alone: `device_local_bytes / k_rt_budget_device_share` in MiB, clamped to the floor and
// the ceiling. A device that reports no device-local memory gets the floor.
u32 rt_device_budget_mib(u64 device_local_bytes) noexcept;

// Where a scene's budget came from, which the summaries and the host's budget report say.
enum class RtBudgetSource : u8 { Setting, Tunable, Device };
const char* rt_budget_source_name(RtBudgetSource source) noexcept;
struct RtBudget {
  u32 mib = 0;
  RtBudgetSource source = RtBudgetSource::Device;
};
// `RenderSettings::rt_budget_mib` when it is not zero, else the `renderer.rt.budget_mib` tunable
// when that is not zero, else the device's rule. Arithmetic only, so the precedence is tested on
// the CPU.
RtBudget resolve_rt_budget(u32 setting_mib, u32 tunable_mib, u64 device_local_bytes) noexcept;
// The first capacity of a scene too large to hold whole within the budget, before any frame has
// said what it builds: 65,536 clusters is 400 MB on the RTX 5090, above every first frame the
// desert overlook's path draws at any resolution. A scene whose every pair in every view fits in
// less is allocated for all of them, exactly as before, and so never drops anything.
inline constexpr u32 k_initial_rt_clusters = 65536;
// `renderer.rt.initial_clusters` (`k_initial_rt_clusters`): the first capacity's floor.
u32 rt_initial_clusters_tunable() noexcept;
// **The resident cluster structure cache** (renderer.md, "The resident cluster structure cache";
// R101): `renderer.rt.cache` (true; off from 2026-10-27 until R106 made a cache that references
// nothing cost nothing) builds a rigid mesh's cluster structures once and references them;
// `renderer.rt.cache_mib` is the most its slots may take, which a scene needing less does not
// reach — 0, the default since R114, the device's (`rt_device_cache_mib`); `renderer.rt.
// reference_mib` (256) the ceiling of the frame's references to it, the cache's counterpart of the
// budget.
bool rt_cache_tunable() noexcept;
u32 rt_cache_mib_tunable() noexcept;
u32 rt_reference_mib_tunable() noexcept;
// `renderer.rt.cache_slots` (0, none): a ceiling in slots beside the MiB, slot 0 included, for a
// measurement or a test that wants a cache of a known size.
u32 rt_cache_slots_tunable() noexcept;
inline constexpr u32 k_default_rt_reference_mib = 256;
// **The cache's ceiling follows the device** (renderer.md, "The resident cluster structure cache";
// roadmap R114, 2026-11-01). It was 256 MiB on every card — 43,690 slots at the RTX 5090's 6,144
// bytes — and the forest proof of concept's v24 walk references 53,000 to 55,000 cached clusters a
// frame, so every frame of it overflowed. The rule is a sixty-fourth of the device-local heaps (as
// the chain's budget reads them, `rt_device_budget_mib`), never under `k_rt_cache_floor_mib` and
// never over `k_rt_cache_ceiling_mib`: 506 MiB on the 32 GB RTX 5090 (86,357 slots), 384 on 24 GB,
// 256 — the old ceiling — on 16 GB and below. It sits beside the chain's sixteenth, so the two take
// at most five sixty-fourths of the card, and a scene with more cached clusters than that holds —
// v24's 1.39 million, twice over for the caster forms — allocates the whole of it at create; the
// smallest power-of-two share that holds v24's wanted set with room on the card it runs on. Past
// it a frame builds what the cache cannot place itself, so the picture never depends on it.
inline constexpr u32 k_rt_cache_device_share = 64;   // 1/64 of device-local memory
inline constexpr u32 k_rt_cache_floor_mib = 256;     // the ceiling before R114: 16 GB and below
inline constexpr u32 k_rt_cache_ceiling_mib = 1024;  // a 64 GB card and up
u32 rt_device_cache_mib(u64 device_local_bytes) noexcept;
// The `renderer.rt.cache_mib` tunable when it is not zero, else the device's rule, and which.
RtBudget resolve_rt_cache_mib(u32 tunable_mib, u64 device_local_bytes) noexcept;

// **The first capacity, from what the scene knows** (renderer.md, "The first capacity"). Before any
// frame has said what it builds, the chain is sized for `estimate` — every pair a frame could
// build, which is the scene's pairs less the static forms' (they are built once, not per frame),
// in every view — clamped to the budget (`limit`) and to the union, and never under `floor` where
// the union is larger: so a scene whose every pair fits in the budget is allocated for all of
// them and can never drop anything, and one past it starts at the budget, where its first frame
// is drawn as complete as the budget allows instead of growing over its first frames. Until
// 2026-10-25 it was the union or 65,536 clusters, whichever was less, and a streamed forest's
// first frames dropped instances while the capacity grew towards what they wanted. Arithmetic
// only, so the rule is tested on the CPU.
u32 first_rt_capacity(u32 limit, u32 union_clusters, u64 estimate, u32 floor) noexcept;

// **The drop order** (renderer.md, "The ray tracing chain's memory"; `gfx::ClusterRecordParams::
// drop_order`): what a frame past its capacity spends it on first. The prefix's slots (rank 0, in
// slot order), then each block of a streamed tail at its ring's rank (ring + 1), the blocks of one
// rank by tile key, a block's live slots in slot order — so the farthest ring loses its casters
// first, then its instances, then the next ring in, and the same ones every run. `blocks` is
// sorted in place; `out` is `u32 count, u32 0`, then `{slot, rank}` pairs.
struct RtDropBlock {
  u64 key = 0;
  u32 rank = 0;
  u32 first_slot = 0;
  u32 slots = 0;
};
void rt_drop_order(u32 prefix_slots, std::span<RtDropBlock> blocks, Vector<u32>& out);

// Every time is wall time in seconds on one monotonic clock (the renderer's `time::monotonic_ns`),
// so a test drives the policy with made-up times.
class RtCapacity {
 public:
  // Starts at `capacity` (clamped to the limit) at time `now`, with an empty shrink window; the
  // capacity it starts at is held from `now`, like any other.
  void reset(const RtCapacityConfig& config, u32 capacity, f64 now) noexcept;
  // One completed frame wanted `wanted` structures, read at `now`. Returns the capacity the set
  // should have from now on, which is `capacity()` unless this frame crossed a threshold; the
  // caller resizes when the two differ and then calls `resized`.
  u32 observe(u32 wanted, f64 now) noexcept;
  // The capacity that holds `wanted` with the headroom, rounded up to the step and clamped to the
  // limit.
  u32 target(u32 wanted) const noexcept;
  // What a growth for `wanted` goes to: `target(wanted)` or `peak(now)`, whichever is larger.
  u32 grow_target(u32 wanted, f64 now) const noexcept;
  // The largest capacity held at any time in the last `peak_seconds` before `now` or in the last
  // `peak_frames` observed frames, whichever reaches further back, the current one included.
  u32 peak(f64 now) const noexcept;
  // The least a shrink at `now` may leave: `floor_pct` of `peak(now)`, rounded up to the step.
  u32 floor(f64 now) const noexcept;
  // What a caller that must not drop anything this frame (a frame somebody waits for) grows to:
  // `grow_target(wanted, now)`, with the shrink window restarted from `wanted`, so the growth is
  // not undone by a window that never saw the frame that asked for it.
  u32 grow_to(u32 wanted, f64 now) noexcept;
  void resized(u32 capacity, f64 now) noexcept;
  // A new limit (the scene's union grew with its stride) and the capacity the scene holds after it,
  // keeping the shrink window and the capacities held: a relayout is not a new scene.
  void relimit(u32 limit, u32 capacity, f64 now) noexcept;

  u32 capacity() const noexcept { return capacity_; }
  u32 limit() const noexcept { return config_.limit; }
  const RtCapacityConfig& config() const noexcept { return config_; }

  // The capacities held before the current one, newest last, each with the time it stopped being
  // held. Only those that can still be the peak are kept — each larger than every one after it —
  // so the bottom is the largest; a run of more than `k_held` falling capacities inside one
  // `peak_seconds` forgets the oldest, which is also the first to expire.
  static constexpr u32 k_held = 32;

 private:
  void hold(u32 previous, f64 until) noexcept;
  bool remembered(f64 until, u64 until_frame, f64 now) const noexcept;
  struct Held {
    u32 capacity = 0;
    f64 until = 0.0;
    u64 until_frame = 0;
  };
  RtCapacityConfig config_;
  u32 capacity_ = 0;
  u32 window_peak_ = 0;
  f64 window_start_ = 0.0;
  u64 window_start_frame_ = 0;
  u64 frames_ = 0;  // frames observed
  Held held_[k_held] = {};
  u32 held_first_ = 0;
  u32 held_count_ = 0;
};

}  // namespace engine::renderer
