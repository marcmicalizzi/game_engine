#pragma once

// A time-lapse of the dune field (docs/subsystems/renderer.md, "The dunes in time-lapse";
// docs/subsystems/terrain.md, "Re-evaluation"): game time running at `rate` game seconds per real
// second, and the terrain's grid re-evaluated at each step of it on the job pool — the closed form
// asked for another t, which is the whole of what moving the dunes costs (ADR-0043).
//
// **The step rule.** Game time accumulates every frame. When it has crossed a step boundary
// (`step_s`, a game day by default) since the last evaluation, and at least `min_frames` frames
// have passed since the last one was started, and none is in flight, an evaluation starts — **at
// the boundary it crossed**, not at the game time of the frame that noticed. So the heights a
// time-lapse produces are a function of the boundaries alone: two runs at different frame rates
// evaluate the same times and hand over the same bytes, only on different frames. A frame that
// crosses several boundaries evaluates the last of them; the ones between are skipped rather than
// queued, because a queue that grows faster than the pool empties it is a time-lapse falling
// further behind.
//
// **Never on the frame.** The evaluation is the grid's 64 x 64 blocks as jobs on the Performance
// pool (`evaluate_terrain_heights`); a frame starts them and a later frame finds them done. Nothing
// waits for them but `finish`.
//
// **What it hands over.** The heights, `size * size` of them in `build_terrain_mesh`'s order, to a
// `TerrainHeightSink`: the upload path. The renderer has none yet for the terrain — its mesh is a
// cluster LOD DAG over those heights, built once at load, and re-uploading moved heights into it
// is GPU work this change does not do — so engine-view's sink counts and times what it is given.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/jobs/job_system.h>
#include <systems/renderer/terrain.h>

#include <memory>
#include <string>

namespace engine::renderer {

struct TimeLapseConfig {
  f64 rate = 0.0;         // game seconds per real second; 0 is still
  f64 step_s = 86'400.0;  // game seconds between evaluations: `renderer.terrain.time_step_s`
  u32 min_frames = 30;    // frames between evaluations at least: `renderer.terrain.time_min_frames`
};

// The rate asked for, and the step and the frame spacing from their tunables.
TimeLapseConfig time_lapse_config_from_tunables(f64 rate) noexcept;

// The step rule, as a function of what it looks at: the last boundary evaluated (as an index of
// steps from the start), the game time now (seconds from the start), frames since the last
// evaluation started, and whether one is still in flight. Returns the boundary to evaluate, or -1.
i64 time_lapse_due(i64 evaluated_step, f64 game_s, f64 step_s, u32 frames_since, u32 min_frames,
                   bool in_flight) noexcept;

class TerrainHeightSink {
 public:
  virtual ~TerrainHeightSink() = default;
  // The grid at game time `time_s` (the description's own time plus the steps run).
  virtual void heights(f64 time_s, std::span<const f32> heights) = 0;
};

class TerrainTimeLapse {
 public:
  TerrainTimeLapse() = default;
  ~TerrainTimeLapse();
  TerrainTimeLapse(const TerrainTimeLapse&) = delete;
  TerrainTimeLapse& operator=(const TerrainTimeLapse&) = delete;

  // False, with a sentence, for a terrain that is not the dune generator's or a rate not positive.
  // `jobs` null evaluates on the calling thread, inside `tick`.
  bool start(const TerrainDesc& desc, const TimeLapseConfig& config, jobs::JobSystem* jobs,
             std::string* error = nullptr);
  // One frame of `real_dt_s`: hands a finished evaluation to the sink, then starts the next one if
  // the step rule says so. Returns true when it started one.
  bool tick(f64 real_dt_s, TerrainHeightSink& sink);
  // Waits for an evaluation in flight and hands it over.
  void finish(TerrainHeightSink& sink);

  bool active() const noexcept { return sampler_ != nullptr; }
  f64 game_time_s() const noexcept { return desc_.time_s + game_s_; }
  u32 started() const noexcept { return started_; }
  u32 delivered() const noexcept { return delivered_; }
  f64 last_eval_ms() const noexcept { return last_eval_ms_; }
  f64 total_eval_ms() const noexcept { return total_eval_ms_; }
  const TimeLapseConfig& config() const noexcept { return config_; }

 private:
  struct Block {
    TerrainTimeLapse* owner = nullptr;
    u32 begin = 0;
    u32 end = 0;
  };
  void deliver(TerrainHeightSink& sink);

  TerrainDesc desc_;
  TimeLapseConfig config_;
  jobs::JobSystem* jobs_ = nullptr;
  std::unique_ptr<TerrainSampler> sampler_;
  Vector<f32> heights_;
  Vector<Block> blocks_;
  Vector<jobs::Job> jobs_list_;
  jobs::Counter counter_;
  bool in_flight_ = false;
  f64 eval_time_s_ = 0.0;
  i64 eval_start_ns_ = 0;
  f64 game_s_ = 0.0;
  i64 evaluated_step_ = 0;
  u32 frames_since_ = 0;
  u32 started_ = 0;
  u32 delivered_ = 0;
  f64 last_eval_ms_ = 0.0;
  f64 total_eval_ms_ = 0.0;
};

}  // namespace engine::renderer
