#include <core/hash/hash.h>
#include <core/time/time.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/terrain_time.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <renderer_log.h>

namespace engine::renderer {

namespace {

// Read when a time-lapse starts (docs/subsystems/renderer.md, "The dunes in time-lapse").
tunables::Float move_fraction{
    "renderer.terrain.move_fraction", 0.25, 0.01, 4.0,
    "How far, as a share of a terrain level's spacing, the dune field's fastest band may travel "
    "between two evaluated fields, and how far any vertex may move in one frame"};
tunables::Float min_step_s{"renderer.terrain.min_step_s", 60.0, 1.0, 1.0e7,
                           "Game seconds between two evaluated fields of the dune field, at least"};
tunables::Float max_step_s{"renderer.terrain.max_step_s", 2'592'000.0, 60.0, 3.2e7,
                           "Game seconds between two evaluated fields of the dune field, at most"};
tunables::Float lead{"renderer.terrain.lead", 1.5, 0.0, 100.0,
                     "How far ahead of what an evaluation of the dune field costs the next field "
                     "is timed, as a multiple of the last evaluation's wall time at the rate"};

// Blocks a job takes: a 4,097 grid's 4,225 blocks are a few hundred jobs, a 257 grid's 25 still
// spread over a pool.
constexpr u32 k_blocks_per_job = 16;

}  // namespace

TimeLapseConfig time_lapse_config_from_tunables(f64 rate) {
  TimeLapseConfig c;
  c.rate = rate;
  c.fraction = move_fraction.get();
  c.min_step_s = min_step_s.get();
  c.max_step_s = std::max(max_step_s.get(), c.min_step_s);
  c.lead = lead.get();
  return c;
}

f64 terrain_next_time(const TerrainSampler& sampler, f64 from_s, f64 spacing_m, f64 fraction,
                      f64 min_step, f64 max_step) noexcept {
  const f64 lo_step = std::max(min_step, 1.0);
  const f64 hi_step = std::max(max_step, lo_step);
  const f64 target = fraction * spacing_m;
  if (sampler.dunes_field() == nullptr || !(target > 0.0)) return from_s + hi_step;
  if (terrain_band_travel_m(sampler, from_s, from_s + lo_step) >= target) return from_s + lo_step;
  if (terrain_band_travel_m(sampler, from_s, from_s + hi_step) <= target) return from_s + hi_step;
  // The travel only grows with the step (it is a path length), so the largest step within the
  // bound is found by bisection; forty-eight halvings of at most a year are well under a
  // millisecond, and the answer is rounded down to one, which keeps it under the bound.
  f64 lo = lo_step;
  f64 hi = hi_step;
  for (u32 i = 0; i < 48 && hi - lo > 1.0e-3; ++i) {
    const f64 mid = 0.5 * (lo + hi);
    if (terrain_band_travel_m(sampler, from_s, from_s + mid) <= target) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return from_s + std::max(lo_step, std::floor(lo * 1000.0) / 1000.0);
}

f64 TerrainBlend::blend() const noexcept {
  if (!has_b || !(time_b > time_a)) return 0.0;
  return std::clamp((surface_s - time_a) / (time_b - time_a), 0.0, 1.0);
}

TerrainBlendStep terrain_blend_step(const TerrainBlend& state, f64 target_s,
                                    f64 budget_m) noexcept {
  TerrainBlendStep step;
  step.surface_s = state.surface_s;
  if (!state.has_b || !(state.time_b > state.time_a)) {
    // One field, or two at one time: nothing to blend towards. A pair of equal times is at its b.
    step.at_b = state.has_b;
    return step;
  }
  const f64 span = state.time_b - state.time_a;
  // Never backwards, never past b.
  f64 goal = std::clamp(target_s, state.surface_s, state.time_b);
  // Never more than the budget anywhere: a vertex moves by |b - a| times the change of the blend.
  if (state.delta_m > 0.0) {
    const f64 room = std::max(budget_m, 0.0) / state.delta_m * span;
    goal = std::min(goal, state.surface_s + room);
  }
  step.surface_s = goal;
  step.moved_m = state.delta_m * (goal - state.surface_s) / span;
  step.at_b = goal >= state.time_b;
  return step;
}

TerrainFrameResult terrain_blend_frame(TerrainBlend& blend, f64 target_s, f64 budget_m,
                                       TerrainNextField& next) noexcept {
  TerrainFrameResult result;
  f64 budget = budget_m;
  // Two passes at most: the old pair to its end, the new one from its start.
  for (u32 guard = 0; guard < 4; ++guard) {
    const bool at_end = !blend.has_b || blend.surface_s >= blend.time_b;
    if (at_end && next.ready && blend.surface_s < target_s) {
      // b is the new pair's a, the same heights to the bit, and the surface stands on it.
      if (blend.has_b) blend.time_a = blend.time_b;
      blend.time_b = next.time_s;
      blend.delta_m = next.delta_m;
      blend.has_b = true;
      blend.surface_s = blend.time_a;
      next.ready = false;
      ++result.installed;
      continue;
    }
    if (at_end) {
      result.held = blend.surface_s < target_s;
      break;
    }
    const TerrainBlendStep step = terrain_blend_step(blend, target_s, budget);
    result.moved_m += step.moved_m;
    budget -= step.moved_m;
    blend.surface_s = step.surface_s;
    if (!step.at_b) {
      result.capped = step.surface_s < std::min(target_s, blend.time_b);
      break;
    }
  }
  return result;
}

f64 terrain_field_delta(std::span<const f32> a, const gfx::TerrainField& wa, std::span<const f32> b,
                        const gfx::TerrainField& wb) noexcept {
  const i32 i0 = std::max(wa.i0, wb.i0);
  const i32 j0 = std::max(wa.j0, wb.j0);
  const i32 i1 = std::min(wa.i0 + static_cast<i32>(wa.nx), wb.i0 + static_cast<i32>(wb.nx));
  const i32 j1 = std::min(wa.j0 + static_cast<i32>(wa.nz), wb.j0 + static_cast<i32>(wb.nz));
  f64 most = 0.0;
  for (i32 j = j0; j < j1; ++j) {
    const usize ra = static_cast<usize>(j - wa.j0) * wa.nx;
    const usize rb = static_cast<usize>(j - wb.j0) * wb.nx;
    for (i32 i = i0; i < i1; ++i) {
      const f64 d = std::abs(static_cast<f64>(a[ra + static_cast<usize>(i - wa.i0)]) -
                             static_cast<f64>(b[rb + static_cast<usize>(i - wb.i0)]));
      most = d > most ? d : most;
    }
  }
  return most;
}

// ---- the motion ------------------------------------------------------------------------------

TerrainMotion::~TerrainMotion() { finish(); }

namespace {

// The heights of a window, a job per `k_blocks_per_job` blocks, or on the calling thread.
void evaluate_window(const TerrainSampler& sampler, jobs::JobSystem* jobs, f64 time_s,
                     const TerrainLattice& lattice, const gfx::TerrainField& window,
                     Vector<f32>& out) {
  out.resize(static_cast<usize>(window.nx) * window.nz);
  const u32 blocks = terrain_window_blocks(window.nx, window.nz);
  const std::span<f32> heights(out.data(), out.size());
  if (jobs == nullptr) {
    evaluate_terrain_window(sampler, time_s, lattice, window.i0, window.j0, window.nx, window.nz, 0,
                            blocks, heights);
    return;
  }
  jobs->parallel_for(jobs::Pool::Performance, (blocks + k_blocks_per_job - 1) / k_blocks_per_job, 1,
                     [&](u32 begin, u32 end) {
                       evaluate_terrain_window(sampler, time_s, lattice, window.i0, window.j0,
                                               window.nx, window.nz, begin * k_blocks_per_job,
                                               std::min(blocks, end * k_blocks_per_job), heights);
                     });
}

}  // namespace

bool TerrainMotion::start(GpuScene& scene, const TimeLapseConfig& config, jobs::JobSystem* jobs,
                          std::string* error) {
  finish();
  levels_.clear();
  scene_ = nullptr;
  if (!(config.rate > 0.0)) {
    if (error != nullptr) *error = "time-lapse: the rate must be positive";
    return false;
  }
  if (scene.terrain_level_count() == 0) {
    if (error != nullptr)
      *error =
          "time-lapse: the scene's terrain does not move (it must name the dune generator, and "
          "the settings' time rate be above zero when the GPU scene is made)";
    return false;
  }
  config_ = config;
  jobs_ = jobs;
  desc_ = std::make_unique<TerrainDesc>(scene.data().terrain);
  sampler_ = std::make_unique<TerrainSampler>(*desc_);
  start_s_ = desc_->time_s;
  game_s_ = 0.0;
  last_move_m_ = 0.0;
  levels_.resize(scene.terrain_level_count());
  for (u32 k = 0; k < levels_.size(); ++k) {
    Level& level = levels_[k];
    level.lattice = scene.terrain_lattice(k);
    level.spacing_m = level.lattice.spacing;
    level.window = gfx::TerrainField{};
    level.window.nx = level.lattice.size;
    level.window.nz = level.lattice.size;
    // The rest pose: the heights at the scene's own time, which on the scene's grid are the mesh's
    // to the bit (`evaluate_terrain_heights`). The first frame draws them; nothing jumps when the
    // time-lapse starts.
    const i64 started = time::monotonic_ns();
    evaluate_window(*sampler_, jobs_, start_s_, level.lattice, level.window, level.rest);
    level.latest = level.rest;
    gfx::BufferResource staging;
    if (!scene.terrain_staging(level.rest.size(), staging, error)) return false;
    std::memcpy(staging.mapped, level.rest.data(), level.rest.size() * sizeof(f32));
    if (!scene.terrain_upload(k, 0, level.window, staging, error)) return false;
    level.a = Field{0, start_s_, 0.0, 0.0};
    level.has_b = false;
    level.next_state = Level::Next::none;
    level.blend = TerrainBlend{};
    level.blend.time_a = level.blend.time_b = level.blend.surface_s = start_s_;
    level.eval_ms_ema = static_cast<f64>(time::monotonic_ns() - started) / 1.0e6;
    level.stats = LevelStats{};
    level.stats.spacing_m = level.spacing_m;
    level.stats.last_eval_ms = level.eval_ms_ema;
    level.stats.last_hash = hash_bytes(level.rest.data(), level.rest.size() * sizeof(f32));
  }
  scene_ = &scene;
  for (u32 k = 0; k < levels_.size(); ++k)
    show(k);
  stop_ = false;
  busy_ = false;
  task_ = Task{};
  worker_ = std::thread([this] { worker_main(); });
  if (!config_.wait) schedule(0);
  ENGINE_LOG_INFO(log_renderer, "terrain time-lapse", log::field("rate", config_.rate),
                  log::field("levels", levels_.size()), log::field("fraction", config_.fraction),
                  log::field("spacing_m", levels_[0].spacing_m),
                  log::field("rest_ms", levels_[0].stats.last_eval_ms),
                  log::field("wait", config_.wait));
  return true;
}

void TerrainMotion::finish() {
  if (worker_.joinable()) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      stop_ = true;
    }
    wake_.notify_all();
    worker_.join();
  }
  // A field finished and not handed over yet goes to the scene, which frees its staging with its
  // own if no frame copies it.
  if (scene_ != nullptr) take_finished();
}

void TerrainMotion::worker_main() {
  for (;;) {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait(lock, [&] { return stop_ || (busy_ && !task_.done); });
    if (busy_ && !task_.done) {
      lock.unlock();
      run(task_);
      lock.lock();
      task_.done = true;
      done_.notify_all();
      continue;
    }
    if (stop_) return;
  }
}

// On the worker thread: the level's `latest` and `rest` are the worker's alone once the motion has
// started (the frame never reads them), and the task is the worker's until it says it is done.
void TerrainMotion::run(Task& task) {
  Level& level = levels_[task.level];
  const i64 started = time::monotonic_ns();
  evaluate_window(*sampler_, jobs_, task.time_s, level.lattice, level.window, task.heights);
  const std::span<const f32> heights(task.heights.data(), task.heights.size());
  task.delta_m = terrain_field_delta(std::span<const f32>(level.latest.data(), level.latest.size()),
                                     level.window, heights, level.window);
  task.padding_m = terrain_field_delta(std::span<const f32>(level.rest.data(), level.rest.size()),
                                       level.window, heights, level.window);
  std::memcpy(task.staging.mapped, task.heights.data(), task.heights.size() * sizeof(f32));
  task.hash = hash_bytes(task.heights.data(), task.heights.size() * sizeof(f32));
  // The field just made is the next one's predecessor; the buffer it replaces is the next task's.
  std::swap(level.latest, task.heights);
  task.eval_ms = static_cast<f64>(time::monotonic_ns() - started) / 1.0e6;
}

u32 TerrainMotion::free_slot(const Level& level) const noexcept {
  for (u32 s = 0; s < GpuScene::k_terrain_field_slots; ++s) {
    if (s != level.a.slot && (!level.has_b || s != level.b.slot)) return s;
  }
  return ~0u;
}

// Asks the worker for `level`'s next field, timed by the cadence rule (and, without `wait`, far
// enough ahead that evaluating it keeps up). False when the worker is busy or there is no slot.
bool TerrainMotion::schedule(u32 k) {
  Level& level = levels_[k];
  if (level.next_state != Level::Next::none) return false;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (busy_) return false;
  }
  const u32 slot = free_slot(level);
  if (slot == ~0u) return false;
  const f64 from = level.has_b ? level.b.time_s : level.a.time_s;
  f64 at = terrain_next_time(*sampler_, from, level.spacing_m, config_.fraction, config_.min_step_s,
                             config_.max_step_s);
  if (!config_.wait && config_.lead > 0.0) {
    // Far enough ahead that the field is ready before the surface reaches it: an evaluation costs
    // its wall time, which is `rate` game seconds a second.
    const f64 keep_up = from + config_.rate * level.eval_ms_ema / 1000.0 * config_.lead;
    if (keep_up > at) {
      at = std::min(keep_up, from + config_.max_step_s);
      ++level.stats.late;
    }
  }
  level.stats.max_step_s = std::max(level.stats.max_step_s, at - from);
  gfx::BufferResource staging;
  std::string error;
  if (!scene_->terrain_staging(u64{level.window.nx} * level.window.nz, staging, &error)) {
    ENGINE_LOG_ERROR(log_renderer, "a terrain field's staging could not be made",
                     log::field("error", error));
    return false;
  }
  level.next = Field{slot, at, 0.0, 0.0};
  level.next_state = Level::Next::evaluating;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    task_.level = k;
    task_.time_s = at;
    task_.staging = staging;
    task_.done = false;
    busy_ = true;
  }
  wake_.notify_all();
  return true;
}

// A field the worker finished goes to the scene, which the next frame copies it into its slot.
void TerrainMotion::take_finished() {
  Task finished;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!busy_ || !task_.done) return;
    finished.level = task_.level;
    finished.time_s = task_.time_s;
    finished.staging = task_.staging;
    finished.delta_m = task_.delta_m;
    finished.padding_m = task_.padding_m;
    finished.eval_ms = task_.eval_ms;
    finished.hash = task_.hash;
    task_.staging = gfx::BufferResource{};
    busy_ = false;
  }
  Level& level = levels_[finished.level];
  std::string error;
  if (!scene_->terrain_upload(finished.level, level.next.slot, level.window, finished.staging,
                              &error)) {
    ENGINE_LOG_ERROR(log_renderer, "a terrain field could not be handed over",
                     log::field("error", error));
    level.next_state = Level::Next::none;
    return;
  }
  level.next.delta_m = finished.delta_m;
  level.next.padding_m = finished.padding_m;
  level.next_state = Level::Next::ready;
  level.eval_ms_ema = level.stats.evaluated == 0 ? finished.eval_ms
                                                 : 0.5 * level.eval_ms_ema + 0.5 * finished.eval_ms;
  ++level.stats.evaluated;
  level.stats.last_eval_ms = finished.eval_ms;
  level.stats.total_eval_ms += finished.eval_ms;
  level.stats.last_hash = finished.hash;
}

void TerrainMotion::frame(f64 real_dt_s) {
  if (!active()) return;
  game_s_ += real_dt_s * config_.rate;
  const f64 target = start_s_ + game_s_;
  take_finished();
  last_move_m_ = 0.0;
  for (u32 k = 0; k < levels_.size(); ++k) {
    Level& level = levels_[k];
    LevelStats& stats = level.stats;
    f64 budget = config_.fraction * level.spacing_m;
    f64 moved = 0.0;
    bool capped = false;
    bool held = false;
    bool waited = false;
    for (u32 guard = 0; guard < 64; ++guard) {
      TerrainNextField next;
      next.ready = level.next_state == Level::Next::ready;
      next.time_s = level.next.time_s;
      next.delta_m = level.next.delta_m;
      const TerrainFrameResult result = terrain_blend_frame(level.blend, target, budget, next);
      moved += result.moved_m;
      budget -= result.moved_m;
      if (result.installed > 0) {
        // The field slots follow the pair: b's slot is a's now, the next field's is b's.
        if (level.has_b) level.a = level.b;
        level.b = level.next;
        level.has_b = true;
        level.next = Field{};
        level.next_state = Level::Next::none;
        stats.max_delta_m = std::max(stats.max_delta_m, level.b.delta_m);
        ++stats.installed;
      }
      capped = result.capped;
      held = result.held;
      if (!result.held || !config_.wait) break;
      // Offscreen: a field the surface has caught up with is waited for, so the picture of a frame
      // is a function of the frame. The worker may be busy with another level's; that one is
      // taken, and this one asked for.
      if (level.next_state == Level::Next::none) schedule(k);
      {
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [&] { return !busy_ || task_.done; });
      }
      take_finished();
      waited = true;
      held = false;
    }
    if (held) ++stats.held;
    if (capped) ++stats.capped;
    if (waited) ++stats.waited;
    stats.max_move_m = std::max(stats.max_move_m, moved);
    stats.max_lag_s = std::max(stats.max_lag_s, target - level.blend.surface_s);
    last_move_m_ = std::max(last_move_m_, moved);
    show(k);
    if (!config_.wait && level.next_state == Level::Next::none) schedule(k);
  }
}

void TerrainMotion::show(u32 k) {
  const Level& level = levels_[k];
  const f64 padding = std::max(level.a.padding_m, level.has_b ? level.b.padding_m : 0.0);
  scene_->terrain_show(k, level.a.slot, level.has_b ? level.b.slot : ~0u,
                       static_cast<f32>(level.blend.blend()), static_cast<f32>(padding) + 0.01f,
                       Vec4{});
}

TerrainMotion::LevelStats TerrainMotion::level_stats(u32 k) const noexcept {
  if (k >= levels_.size()) return LevelStats{};
  const Level& level = levels_[k];
  LevelStats s = level.stats;
  s.surface_s = level.blend.surface_s;
  s.time_a = level.blend.time_a;
  s.time_b = level.has_b ? level.blend.time_b : level.blend.time_a;
  s.blend = level.blend.blend();
  s.padding_m = std::max(level.a.padding_m, level.has_b ? level.b.padding_m : 0.0);
  return s;
}

}  // namespace engine::renderer
