// **The visible list's runs are sized by the frame, not the scene** (W51-5; renderer.md, the
// section of that name): `SceneRenderer::step_runs`, the policy's step, in a file of its own beside
// the buffers' half (gpu_scene_runs.cpp).
//
// The policy is the ray tracing chain's arithmetic (`RtCapacity`, rt_capacity.h) with its own
// numbers: a frame's demand is its fullest run, a run that filled says how many appends it turned
// away, and the capacity is kept a headroom above the demand, grown the frame a demand eats into
// half of it and shrunk only after a window of frames and seconds. The first capacity is the stride
// — every run as long as the scene's pairs, which no frame can overfill — up to a first bound
// (W156, below), and it is a guess the first frames answer, so a world standing where it loaded
// gives the rest back within seconds.

#include <core/log/log.h>
#include <core/time/time.h>
#include <domain/gfx/frame.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/scene_renderer.h>

#include <algorithm>
#include <renderer_log.h>
#include <string>

namespace engine::renderer {

namespace {

// **Why these numbers.** A run that fills drops pairs from the picture for the frames until the
// growth lands (the read-back is two frames late and the frames in flight record against the old
// capacity), so the headroom is twice the chain's: a run holds half as much again as the fullest
// run, and grows the frame a run passes a quarter more than it was sized for. Not more: the
// arithmetic shrinks only when a whole window's peak with its headroom leaves more than half the
// runs unused, so a headroom of 100 % kept the low setting's runs at the stride standing in the
// forest staging (the fullest run, the farthest cascade's, was 28 % of it: W51-5's first flight).
// The step is 65,536 entries, half a megabyte of list a run.
tunables::Bool runs_by_frame{
    "renderer.runs.by_frame", true,
    "Size the visible list's runs by what the frames draw, a headroom above the fullest run, "
    "rather than by the scene's pairs (W51-5). A streamed world's runs only; false keeps every run "
    "as long as the pair stride"};
tunables::Int runs_headroom_pct{"renderer.runs.headroom_pct", 50, 25, 1000,
                                "Percent above the fullest run of recent frames that a run holds"};
// **The first bound** (W156; renderer.md, "The visible list's runs are sized by the frame"): a
// tail's arrival lays the runs out at the stride only up to this many entries, so a world's first
// frames do not hold a list for every pair the tail could ever name (the forest staging at 4K:
// 16,777,216 entries a run for frames whose fullest held 1,100,232; at 1080p with cascades
// 15,757,312 for 4,419,461). A frame that fills it grows the runs as any filled run does, and a
// frame somebody waits for is drawn again (`render_offscreen`). 0 lays them out at the stride.
// **Why 4,194,304** (32 MiB of list a run): at least twice the fullest camera run the staging's 4K
// frames fill, so the guess is given back to the fit after its first window (`RtCapacity`'s half
// rule); a larger bound that the fit passes half of is kept, as 8,388,608 would be at 1080p with
// cascades (a fit of 6,684,672), which would stand 137 MiB above the stride's fit. There the
// farthest cascade's first frame fills the bound (4,419,461) and grows to that same fit.
tunables::Int runs_first_entries{
    "renderer.runs.first_entries", 4'194'304, 0, 1 << 30,
    "The most entries a run of the visible list is laid out with when a streamed tail arrives, "
    "before the frames have said what they draw (W156); 0 follows the pair stride"};
constexpr u32 k_run_step = 65536;
constexpr f64 k_run_first_seconds = 2.0;  // the stride is given back after this and a window

}  // namespace

// A bound only where the frames grow the runs: the cull on and the policy on (`step_runs`).
u32 SceneRenderer::first_runs() const noexcept {
  if (!resolved_.settings.cull || !runs_by_frame.get()) return 0;
  return static_cast<u32>(runs_first_entries.get());
}

// **A frame drawn at the first bound is measured before it is handed back** (W156-2; renderer.md,
// "A tail's arrival lays the runs out at a first bound"). Every run shares one capacity and a full
// run turns pairs away in the atomic append's order, so a frame drawn in flight at a bound no frame
// has sized could reach a window with clusters or casters missing. While the runs are unmeasured
// the frame is drawn without its present's signal and waited for; a run that filled grows the runs
// as `render_offscreen` grows them (the policy's growth, then the stride) and the frame is drawn
// again, the acquired image waited on once; the runs then count as the frames' (`fit_runs`) and an
// empty submission behind the frame raises the present's semaphore. Once a scene load, on every
// host's path; its cost is the wait and the redraws.
u64 SceneRenderer::submit_frame(const FrameDesc& frame, std::string* error) {
  if (scene_ == nullptr || !runs_measure_) return submit_recorded(frame, error);
  ++stats_.run_measured;
  FrameDesc held = frame;
  held.signal = gfx::SemaphoreHandle{};
  u64 value = 0;
  for (u32 attempt = 0;; ++attempt) {
    value = submit_recorded(held, error);
    if (value == 0) return 0;
    if (!frames_.wait(value)) {
      if (error != nullptr) *error = "the GPU did not finish the frame";
      return 0;
    }
    collect_visible();
    held.wait = gfx::SemaphoreHandle{};  // the image was acquired for the first attempt
    const u32 dropped = stats_.run_drops + stats_.cascade_run_drops;
    if (dropped == 0 || attempt == 2 || scene_->run_capacity() >= scene_->pair_stride()) break;
    const f64 now = static_cast<f64>(time::monotonic_ns()) / 1.0e9;
    const u32 wanted = stats_.run_high + dropped;
    u32 runs = scene_->pair_stride();
    if (attempt == 0) runs = run_policy_set_ ? run_policy_.grow_to(wanted, now) : wanted;
    ++stats_.run_redraws;
    if (!resize_runs(std::max(runs, wanted), now)) break;  // refused: the frame stands, counted
    begin_frame();
  }
  runs_measure_ = false;
  if (scene_->runs_unmeasured()) (void)scene_->fit_runs(scene_->run_capacity(), nullptr, nullptr);
  if (frame.signal.valid()) {
    begin_frame();
    gfx::FrameContext::PresentSync sync;
    sync.signal = frame.signal;
    (void)frames_.end_frame(sync);
    recording_ = false;
  }
  return value;
}

void SceneRenderer::step_runs(i64 now_ns) {
  GpuScene& scene = *scene_;
  stats_.run_capacity = scene.run_capacity();
  // A scene read whole holds exactly its pairs a run, which is what its frames are bounded by; the
  // runs that outgrow the frames are a streamed world's, whose stride is its tail and half again.
  if (!resolved_.settings.cull || !scene.dynamic() || !runs_by_frame.get()) return;
  const f64 now = static_cast<f64>(now_ns) / 1.0e9;
  const u32 limit = scene.pair_stride();
  if (!run_policy_set_) {
    RtCapacityConfig config;
    config.limit = limit;
    config.step = k_run_step;
    config.headroom_pct = static_cast<u32>(runs_headroom_pct.get());
    config.shrink_seconds = rt_shrink_seconds_tunable();
    config.peak_seconds = rt_peak_seconds_tunable();
    config.floor_pct = rt_floor_pct_tunable();
    config.shrink_frames = rt_shrink_frames_tunable();
    config.peak_frames = rt_peak_frames_tunable();
    config.slope_frames = rt_slope_frames_tunable();
    config.first_seconds = k_run_first_seconds;
    run_policy_.reset(config, scene.run_capacity(), now);
    run_policy_.first_is_guess(true);
    run_policy_set_ = true;
  } else if (run_policy_.limit() != limit) {
    run_policy_.relimit(limit, scene.run_capacity(), now);  // the stride grew with the tail
  }
  // A run that filled stopped at its capacity, so the frame wanted at least what it drew and what
  // it turned away; such a frame grows at once, past the window.
  const u32 dropped = stats_.run_drops + stats_.cascade_run_drops;
  const u32 wanted = stats_.run_high + dropped;
  const u32 next =
      dropped != 0 ? run_policy_.grow_to(wanted, now) : run_policy_.observe(wanted, now);
  const u32 held = scene.run_capacity();
  if (next == held) return;
  (void)resize_runs(next, now);
}

// The runs laid out at `capacity`, swapped in beside the frames in flight: everything they size —
// the list, the vertex path's records, the pool's per-entry slots and the allocator's scratch — is
// made before anything old goes, and a refusal leaves the scene as it was (`GpuScene::fit_runs`),
// so the frame records at the runs it had and the next growth is asked again. Counted and said;
// a blocking frame that dropped calls it too (`render_offscreen`), the policy or none.
bool SceneRenderer::resize_runs(u32 capacity, f64 now) {
  GpuScene& scene = *scene_;
  const u32 held = scene.run_capacity();
  std::string error;
  Vector<gfx::BufferResource> retired;
  if (!scene.fit_runs(capacity, &retired, &error)) {
    ++stats_.run_refusals;
    ENGINE_LOG_WARN(log_renderer, "the visible list's runs could not be laid out again",
                    log::field("capacity", capacity), log::field("held", held),
                    log::field("refusals", stats_.run_refusals), log::field("error", error));
    if (run_policy_set_) run_policy_.resized(held, now);
    return false;
  }
  for (const gfx::BufferResource& buffer : retired)
    frames_.defer_destroy(buffer);
  const u32 dropped = stats_.run_drops + stats_.cascade_run_drops;
  const u32 limit = scene.pair_stride();
  if (run_policy_set_) run_policy_.resized(scene.run_capacity(), now);
  stats_.run_capacity = scene.run_capacity();
  ENGINE_LOG_INFO(log_renderer, "the visible list's runs were sized by the frame",
                  log::field("capacity", scene.run_capacity()), log::field("held", held),
                  log::field("fullest_run", stats_.run_high), log::field("dropped", dropped),
                  log::field("stride", limit),
                  log::field("list_mib", static_cast<f64>(scene.visible.size) / (1024.0 * 1024.0)));
  return true;
}

}  // namespace engine::renderer
