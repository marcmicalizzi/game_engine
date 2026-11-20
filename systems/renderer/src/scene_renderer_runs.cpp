// **The visible list's runs are sized by the frame, not the scene** (W51-5; renderer.md, the
// section of that name): `SceneRenderer::step_runs`, the policy's step, in a file of its own beside
// the buffers' half (gpu_scene_runs.cpp).
//
// The policy is the ray tracing chain's arithmetic (`RtCapacity`, rt_capacity.h) with its own
// numbers: a frame's demand is its fullest run, a run that filled says how many appends it turned
// away, and the capacity is kept a headroom above the demand, grown the frame a demand eats into
// half of it and shrunk only after a window of frames and seconds. The first capacity is the stride
// — every run as long as the scene's pairs, which no frame can overfill — and it is a guess the
// first frames answer, so a world standing where it loaded gives the rest back within seconds.

#include <core/log/log.h>
#include <core/time/time.h>
#include <domain/gfx/frame.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/scene_renderer.h>

#include <renderer_log.h>
#include <string>

namespace engine::renderer {

namespace {

// **Why these numbers.** A run that fills drops pairs from the picture for the frames until the
// growth lands (the read-back is two frames late and the frames in flight record against the old
// capacity), so the headroom is the chain's four times: the capacity is twice the fullest run, and
// it grows the frame a run passes one and a half times what it was sized for. The step is a
// 64 KiB-entry multiple, half a megabyte of list a run.
tunables::Bool runs_by_frame{
    "renderer.runs.by_frame", true,
    "Size the visible list's runs by what the frames draw, a headroom above the fullest run, "
    "rather than by the scene's pairs (W51-5). A streamed world's runs only; false keeps every run "
    "as long as the pair stride"};
tunables::Int runs_headroom_pct{"renderer.runs.headroom_pct", 100, 25, 1000,
                                "Percent above the fullest run of recent frames that a run holds"};
constexpr u32 k_run_step = 65536;
constexpr f64 k_run_first_seconds = 2.0;  // the stride is given back after this and a window

}  // namespace

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
  std::string error;
  Vector<gfx::BufferResource> retired;
  if (!scene.fit_runs(next, &retired, &error)) {
    ENGINE_LOG_WARN(log_renderer, "the visible list's runs could not be laid out again",
                    log::field("capacity", next), log::field("held", held),
                    log::field("error", error));
    run_policy_.resized(held, now);
    return;
  }
  for (const gfx::BufferResource& buffer : retired)
    frames_.defer_destroy(buffer);
  // The pool's per-entry slots and the allocator's scratch follow the runs: made again now, before
  // this frame records, swapped in beside the frames in flight (GpuScene::relayout).
  if (scene.relayout_pending()) {
    relayout_retired_.clear();
    const bool swapped = scene.relayout(&error, &relayout_retired_);
    for (const gfx::BufferResource& buffer : relayout_retired_.buffers)
      frames_.defer_destroy(buffer);
    for (u32 i = 0; i < relayout_retired_.chain.count; ++i)
      retire_rt_buffer(relayout_retired_.chain.buffers[i]);
    if (relayout_retired_.tlas.handle.valid()) {
      retired_tlas_.push_back(
          RetiredTlas{relayout_retired_.tlas, relayout_retired_.tlas_slot, frames_.frame_index()});
    }
    relayout_retired_.clear();
    if (!swapped || scene.relayout_pending()) {
      frames_.wait_idle();
      if (!scene.relayout(&error, nullptr)) {
        ENGINE_LOG_ERROR(log_renderer, "the visible list's runs' pool slots could not be made",
                         log::field("capacity", next), log::field("error", error));
      }
    }
  }
  run_policy_.resized(scene.run_capacity(), now);
  stats_.run_capacity = scene.run_capacity();
  ENGINE_LOG_INFO(log_renderer, "the visible list's runs were sized by the frame",
                  log::field("capacity", scene.run_capacity()), log::field("held", held),
                  log::field("fullest_run", stats_.run_high), log::field("dropped", dropped),
                  log::field("stride", limit),
                  log::field("list_mib", static_cast<f64>(scene.visible.size) / (1024.0 * 1024.0)));
}

}  // namespace engine::renderer
