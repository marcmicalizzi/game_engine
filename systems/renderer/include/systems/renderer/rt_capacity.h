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
  u32 limit = 0;          // the most the capacity may be, in clusters: the budget
  u32 step = 4096;        // capacities are multiples of this (and at least one step)
  u32 headroom_pct = 25;  // kept above the demand
  u32 shrink_frames = 240;
};

// The `renderer.rt.*` tunables, read once: the budget in MiB, the headroom, the shrink window and
// the step. `RenderSettings::rt_budget_mib` overrides the budget when it is not zero, and the
// budget tunable overrides the device's rule (below) when it is not zero; 0, its default, follows
// the device.
u32 rt_budget_mib_tunable() noexcept;
u32 rt_headroom_pct_tunable() noexcept;
u32 rt_shrink_frames_tunable() noexcept;
u32 rt_step_tunable() noexcept;
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
// `renderer.rt.cache_mib` (256) is the most its slots may take, which a scene needing less does not
// reach; `renderer.rt.reference_mib` (256) the ceiling of the frame's references to it, the cache's
// counterpart of the budget.
bool rt_cache_tunable() noexcept;
u32 rt_cache_mib_tunable() noexcept;
u32 rt_reference_mib_tunable() noexcept;
// `renderer.rt.cache_slots` (0, none): a ceiling in slots beside the MiB, slot 0 included, for a
// measurement or a test that wants a cache of a known size.
u32 rt_cache_slots_tunable() noexcept;
inline constexpr u32 k_default_rt_cache_mib = 256;
inline constexpr u32 k_default_rt_reference_mib = 256;

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
