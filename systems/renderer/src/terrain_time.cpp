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

TerrainSurfaceResult terrain_surface_frame(std::span<TerrainBlend> blends, f64 target_s,
                                           std::span<const f64> budgets_m,
                                           std::span<TerrainNextField> next, std::span<f64> moved_m,
                                           std::span<u32> installed) noexcept {
  TerrainSurfaceResult result;
  const u32 n = std::min<u32>(static_cast<u32>(blends.size()), k_max_surface_levels);
  if (n == 0) return result;
  f64 budget[k_max_surface_levels];
  for (u32 k = 0; k < n; ++k) {
    budget[k] = std::max(budgets_m[k], 0.0);
    moved_m[k] = 0.0;
    installed[k] = 0;
  }
  f64 s = blends[0].surface_s;
  // A pass per b the surface reaches: each pass either reaches the game's time, stops on a level's
  // bound or on a late field, or stops on a level's b, which the next pass replaces.
  for (u32 guard = 0; guard < 4 * k_max_surface_levels; ++guard) {
    if (s < target_s) {
      for (u32 k = 0; k < n; ++k) {
        TerrainBlend& b = blends[k];
        const bool at_end = !b.has_b || s >= b.time_b;
        if (!at_end || !next[k].ready) continue;
        // b is the new pair's a, the same heights to the bit, and the surface stands on it.
        if (b.has_b) b.time_a = b.time_b;
        b.time_b = next[k].time_s;
        b.delta_m = next[k].delta_m;
        b.has_b = true;
        next[k].ready = false;
        ++installed[k];
      }
    }
    // As far as every level allows: no level's b passed (a level with one field stands at it),
    // and no level's vertices moved by more than what is left of its budget.
    f64 limit = target_s;
    u32 by = ~0u;
    bool bound = false;
    for (u32 k = 0; k < n; ++k) {
      const TerrainBlend& b = blends[k];
      const f64 reach = b.has_b ? b.time_b : s;
      if (reach < limit) {
        limit = reach;
        by = k;
        bound = false;
      }
      if (b.has_b && b.delta_m > 0.0 && b.time_b > b.time_a) {
        const f64 room = s + budget[k] / b.delta_m * (b.time_b - b.time_a);
        if (room < limit) {
          limit = room;
          by = k;
          bound = true;
        }
      }
    }
    if (!(limit > s)) {
      if (s < target_s && by != ~0u) {
        result.held = !bound;
        result.capped = bound;
        result.limiting = by;
      }
      break;
    }
    for (u32 k = 0; k < n; ++k) {
      const TerrainBlend& b = blends[k];
      if (!b.has_b || !(b.time_b > b.time_a)) continue;
      const f64 m = b.delta_m * (limit - s) / (b.time_b - b.time_a);
      moved_m[k] += m;
      budget[k] = std::max(budget[k] - m, 0.0);
    }
    s = limit;
    for (u32 k = 0; k < n; ++k)
      blends[k].surface_s = s;
    if (s >= target_s) break;
    if (bound) {
      result.capped = true;
      result.limiting = by;
      break;
    }
  }
  for (u32 k = 0; k < n; ++k)
    blends[k].surface_s = s;
  result.surface_s = s;
  return result;
}

// ---- the motion ------------------------------------------------------------------------------

TerrainMotion::~TerrainMotion() { finish(); }

namespace {

// Read when a time-lapse starts (docs/subsystems/renderer.md, "The rings in the scene").
tunables::Float ring_upload_mib{
    "renderer.terrain.ring_upload_mib", 32.0, 0.0, 4096.0,
    "Megabytes of a terrain ring's rebuilt chunks copied onto the device a frame, at most (one "
    "chunk a frame whatever it weighs)"};

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

bool same_window(const gfx::TerrainField& a, const gfx::TerrainField& b) noexcept {
  return a.i0 == b.i0 && a.j0 == b.j0 && a.nx == b.nx && a.nz == b.nz;
}

u64 samples_of(const gfx::TerrainField& w) noexcept { return u64{w.nx} * w.nz; }

f64 ms_since(i64 started) noexcept {
  return static_cast<f64>(time::monotonic_ns() - started) / 1.0e6;
}

}  // namespace

bool TerrainMotion::start(GpuScene& scene, TerrainRingSet* rings, const TimeLapseConfig& config,
                          jobs::JobSystem* jobs, std::string* error) {
  finish();
  levels_.clear();
  scene_ = nullptr;
  rings_ = nullptr;
  const auto fail = [&](std::string sentence) {
    if (error != nullptr) *error = std::move(sentence);
    return false;
  };
  if (!(config.rate >= 0.0)) return fail("time-lapse: the rate must not be negative");
  if (scene.terrain_level_count() == 0) {
    return fail(
        "time-lapse: the scene's terrain does not move (it must name the dune generator, and the "
        "settings ask for a time rate above zero or the terrain rings, when the GPU scene is "
        "made)");
  }
  if (rings != nullptr && rings->valid() && scene.terrain_level_count() > 1) {
    if (rings->level_count() != scene.terrain_level_count())
      return fail("terrain rings: the GPU scene was made with other rings than these");
    rings_ = rings;
  } else if (scene.terrain_level_count() > 1) {
    return fail("terrain rings: the GPU scene has ring levels, and no ring set was handed over");
  }
  config_ = config;
  jobs_ = jobs;
  desc_ = std::make_unique<TerrainDesc>(scene.data().terrain);
  sampler_ = std::make_unique<TerrainSampler>(*desc_);
  start_s_ = desc_->time_s;
  game_s_ = 0.0;
  last_move_m_ = 0.0;
  recentre_ = Recentre::none;
  rings_stopped_ = false;
  frames_ = 0;
  asked_frame_ = 0;
  frozen_frame_ = 0;
  ring_stats_ = RingStats{};
  uploads_.clear();
  upload_next_ = 0;
  upload_frames_ = 0;
  upload_bytes_ = 0;
  pending_moved_ = 0;
  for (Pair& p : pending_)
    p = Pair{};
  upload_budget_bytes_ = static_cast<u64>(ring_upload_mib.get() * 1024.0 * 1024.0);
  shown_layout_ = rings_ != nullptr ? rings_->layout() : TerrainRingLayout{};
  pending_layout_ = shown_layout_;
  levels_.resize(scene.terrain_level_count());
  for (u32 k = 0; k < levels_.size(); ++k) {
    Level& level = levels_[k];
    level.lattice = scene.terrain_lattice(k);
    level.spacing_m = level.lattice.spacing;
    level.slots = scene.terrain_field_slots(k);
    if (k == 0) {
      level.window = gfx::TerrainField{};
      level.window.nx = level.lattice.size;
      level.window.nz = level.lattice.size;
    } else {
      level.window = rings_->field_window(k, shown_layout_);
    }
    if (samples_of(level.window) > scene.terrain_field_capacity(k))
      return fail("time-lapse: a terrain level's window is larger than its field slots");
    // The rest pose: the heights at the scene's own time, which on the scene's grid are the mesh's
    // to the bit (`evaluate_terrain_heights`) and on a ring its chunks' to the micrometre they
    // were built at. The first frame draws them; nothing jumps when the time-lapse starts.
    const i64 started = time::monotonic_ns();
    Vector<f32> heights;
    evaluate_window(*sampler_, jobs_, start_s_, level.lattice, level.window, heights);
    f64 padding = 0.0;
    if (k == 0) {
      level.rest = heights;
    } else {
      padding =
          rings_->padding(k, std::span<const f32>(heights.data(), heights.size()), level.window);
    }
    gfx::BufferResource staging;
    if (!scene.terrain_staging(heights.size(), staging, error)) return false;
    std::memcpy(staging.mapped, heights.data(), heights.size() * sizeof(f32));
    if (!scene.terrain_upload(k, 0, level.window, staging, error)) return false;
    level.a = Field{0, start_s_, padding, 0.0, level.window};
    level.b = Field{};
    level.next = Field{};
    level.has_b = false;
    level.next_state = Level::Next::none;
    level.blend = TerrainBlend{};
    level.blend.time_a = level.blend.time_b = level.blend.surface_s = start_s_;
    level.eval_ms_ema = ms_since(started);
    level.stats = LevelStats{};
    level.stats.spacing_m = level.spacing_m;
    level.stats.last_eval_ms = level.eval_ms_ema;
    level.stats.last_hash = hash_bytes(heights.data(), heights.size() * sizeof(f32));
    level.shown_slots.clear();
    if (k > 0) {
      for (const TerrainChunk& chunk : rings_->chunks(k))
        if (chunk.slot != ~0u) level.shown_slots.push_back(chunk.slot);
    }
    level.cache.clear();
    level.cache.push_back(Cached{start_s_, level.window, std::move(heights)});
  }
  scene_ = &scene;
  for (u32 k = 0; k < levels_.size(); ++k)
    show(k);
  stop_ = false;
  busy_ = false;
  task_ = Task{};
  worker_ = std::thread([this] { worker_main(); });
  if (rings_ != nullptr) {
    ring_stop_ = false;
    ring_busy_ = false;
    ring_task_ = RingTask{};
    ring_worker_ = std::thread([this] { ring_worker_main(); });
  }
  schedule_next();
  ENGINE_LOG_INFO(log_renderer, "terrain time-lapse", log::field("rate", config_.rate),
                  log::field("levels", levels_.size()), log::field("fraction", config_.fraction),
                  log::field("spacing_m", levels_[0].spacing_m),
                  log::field("rest_ms", levels_[0].stats.last_eval_ms),
                  log::field("rings", rings_ != nullptr), log::field("wait", config_.wait));
  return true;
}

void TerrainMotion::finish() {
  if (ring_worker_.joinable()) {
    {
      std::unique_lock<std::mutex> lock(ring_mutex_);
      ring_stop_ = true;
    }
    ring_wake_.notify_all();
    ring_worker_.join();
  }
  if (worker_.joinable()) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      stop_ = true;
    }
    wake_.notify_all();
    worker_.join();
  }
  if (scene_ == nullptr) return;
  // A field finished and not handed over yet goes to the scene, which frees its staging with its
  // own if no frame copies it; a re-centre's goes back unused.
  Task finished;
  bool have = false;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (busy_ && task_.done) {
      finished = std::move(task_);
      task_ = Task{};
      have = true;
    }
    busy_ = false;
  }
  if (have && finished.kind == Task::Kind::field) take_field(finished);
  if (have && finished.kind == Task::Kind::pairs) {
    for (Pair& p : finished.pairs) {
      scene_->terrain_retire(p.staging_a);
      scene_->terrain_retire(p.staging_b);
    }
  }
  for (Pair& p : pending_) {
    scene_->terrain_retire(p.staging_a);
    scene_->terrain_retire(p.staging_b);
    p = Pair{};
  }
  ring_busy_ = false;
}

void TerrainMotion::worker_main() {
  for (;;) {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait(lock, [&] { return stop_ || (busy_ && !task_.done); });
    if (busy_ && !task_.done) {
      lock.unlock();
      if (task_.kind == Task::Kind::field) {
        run_field(task_);
      } else {
        run_pairs(task_);
      }
      lock.lock();
      task_.done = true;
      done_.notify_all();
      continue;
    }
    if (stop_) return;
  }
}

void TerrainMotion::ring_worker_main() {
  for (;;) {
    std::unique_lock<std::mutex> lock(ring_mutex_);
    ring_wake_.wait(lock, [&] { return ring_stop_ || (ring_busy_ && !ring_task_.done); });
    if (ring_busy_ && !ring_task_.done) {
      lock.unlock();
      run_rings(ring_task_);
      lock.lock();
      ring_task_.done = true;
      ring_done_.notify_all();
      continue;
    }
    if (ring_stop_) return;
  }
}

// ---- the field worker's side: a level's cache and rest are its alone ---------------------------

const f32* TerrainMotion::field_heights(Level& level, f64 time_s, const gfx::TerrainField& window,
                                        Vector<f32>& scratch) {
  for (const Cached& c : level.cache) {
    if (c.time_s == time_s && same_window(c.window, window)) return c.heights.data();
  }
  // Not kept (it should have been): made again, which costs a field and changes nothing.
  field_over(level, time_s, window, scratch);
  return scratch.data();
}

// A level's field at `time_s` over `window`, from a kept field at the same time over another
// window where the two overlap and evaluated where they do not: the evaluation is a function of
// the lattice point and the time (`evaluate_terrain_window`, which terrain_generator_tests.cpp
// holds to the point function), so the copy is the same bytes an evaluation would be — and a ring
// re-centred by a snap step has most of its new window in its old one.
void TerrainMotion::field_over(Level& level, f64 time_s, const gfx::TerrainField& window,
                               Vector<f32>& out) {
  out.resize(static_cast<usize>(window.nx) * window.nz);
  const Cached* from = nullptr;
  for (const Cached& c : level.cache) {
    if (c.time_s == time_s) from = &c;
  }
  const i32 wi1 = window.i0 + static_cast<i32>(window.nx);
  const i32 wj1 = window.j0 + static_cast<i32>(window.nz);
  i32 oi0 = window.i0;
  i32 oi1 = window.i0;
  i32 oj0 = window.j0;
  i32 oj1 = window.j0;
  if (from != nullptr) {
    oi0 = std::max(window.i0, from->window.i0);
    oj0 = std::max(window.j0, from->window.j0);
    oi1 = std::min(wi1, from->window.i0 + static_cast<i32>(from->window.nx));
    oj1 = std::min(wj1, from->window.j0 + static_cast<i32>(from->window.nz));
  }
  if (from == nullptr || oi0 >= oi1 || oj0 >= oj1) {
    evaluate_window(*sampler_, jobs_, time_s, level.lattice, window, out);
    return;
  }
  for (i32 j = oj0; j < oj1; ++j) {
    const f32* src = from->heights.data() +
                     static_cast<usize>(j - from->window.j0) * from->window.nx +
                     static_cast<usize>(oi0 - from->window.i0);
    f32* dst = out.data() + static_cast<usize>(j - window.j0) * window.nx +
               static_cast<usize>(oi0 - window.i0);
    std::memcpy(dst, src, static_cast<usize>(oi1 - oi0) * sizeof(f32));
  }
  Vector<f32> part;
  const auto evaluate_rect = [&](i32 i0, i32 j0, i32 i1, i32 j1) {
    if (i0 >= i1 || j0 >= j1) return;
    gfx::TerrainField rect{};
    rect.i0 = i0;
    rect.j0 = j0;
    rect.nx = static_cast<u32>(i1 - i0);
    rect.nz = static_cast<u32>(j1 - j0);
    evaluate_window(*sampler_, jobs_, time_s, level.lattice, rect, part);
    for (i32 j = j0; j < j1; ++j) {
      std::memcpy(out.data() + static_cast<usize>(j - window.j0) * window.nx +
                      static_cast<usize>(i0 - window.i0),
                  part.data() + static_cast<usize>(j - j0) * rect.nx, rect.nx * sizeof(f32));
    }
  };
  evaluate_rect(window.i0, window.j0, wi1, oj0);  // the rows before the overlap
  evaluate_rect(window.i0, oj1, wi1, wj1);        // and after it
  evaluate_rect(window.i0, oj0, oi0, oj1);        // the overlap's rows, to its left
  evaluate_rect(oi1, oj0, wi1, oj1);              // and to its right
}

void TerrainMotion::keep_field(Level& level, f64 time_s, const gfx::TerrainField& window,
                               Vector<f32>&& heights, f64 keep_from_s) {
  // What no later field is measured from and no re-centre carries over goes: fields older than
  // the level's a, and fields over a window the level no longer draws.
  Vector<Cached> kept;
  for (Cached& c : level.cache) {
    if (c.time_s >= keep_from_s && c.time_s != time_s && same_window(c.window, window))
      kept.push_back(std::move(c));
  }
  kept.push_back(Cached{time_s, window, std::move(heights)});
  while (kept.size() > 4)
    kept.erase(kept.begin());
  level.cache = std::move(kept);
}

void TerrainMotion::run_field(Task& task) {
  Level& level = levels_[task.level];
  const i64 started = time::monotonic_ns();
  Vector<f32> heights;
  evaluate_window(*sampler_, jobs_, task.time_s, level.lattice, task.window, heights);
  const usize n = heights.size();
  const std::span<const f32> field(heights.data(), n);
  Vector<f32> scratch;
  const f32* from = field_heights(level, task.from_s, task.window, scratch);
  task.delta_m =
      terrain_field_delta(std::span<const f32>(from, n), task.window, field, task.window);
  if (task.level == 0) {
    task.padding_m = terrain_field_delta(std::span<const f32>(level.rest.data(), level.rest.size()),
                                         task.window, field, task.window);
  } else {
    task.padding_m = rings_->padding(task.level, field, task.window);
  }
  std::memcpy(task.staging.mapped, heights.data(), n * sizeof(f32));
  task.hash = hash_bytes(heights.data(), n * sizeof(f32));
  keep_field(level, task.time_s, task.window, std::move(heights), task.keep_from_s);
  task.eval_ms = ms_since(started);
}

// A re-centre's pairs, once the rings hold them: each moved ring's pair over its new window (kept
// fields copied, the new strip evaluated), and every moved ring's paddings measured against the
// chunks it will be drawn over from the swap on — and the ones it is drawn over until then.
void TerrainMotion::run_pairs(Task& task) {
  const i64 started = time::monotonic_ns();
  for (u32 k = 1; k < levels_.size(); ++k) {
    Pair& p = task.pairs[k];
    if (!p.moved && !p.window_changed) continue;
    Level& level = levels_[k];
    const gfx::TerrainField& w = p.a.window;
    const usize count = samples_of(w);
    Vector<f32> ha;
    Vector<f32> hb;
    const f32* pa = nullptr;
    const f32* pb = nullptr;
    if (p.window_changed) {
      field_over(level, p.a.time_s, w, ha);
      pa = ha.data();
      if (p.has_b) {
        field_over(level, p.b.time_s, w, hb);
        pb = hb.data();
        p.b.delta_m = terrain_field_delta(std::span<const f32>(pa, count), w,
                                          std::span<const f32>(pb, count), w);
      }
    } else {
      pa = field_heights(level, p.a.time_s, w, ha);
      if (p.has_b) pb = field_heights(level, p.b.time_s, w, hb);
    }
    p.a.padding_m = rings_->padding(k, std::span<const f32>(pa, count), w);
    if (p.has_b) p.b.padding_m = rings_->padding(k, std::span<const f32>(pb, count), w);
    if (p.window_changed) {
      std::memcpy(p.staging_a.mapped, pa, count * sizeof(f32));
      if (p.has_b) std::memcpy(p.staging_b.mapped, pb, count * sizeof(f32));
      keep_field(level, p.a.time_s, w, std::move(ha), p.a.time_s);
      if (p.has_b) keep_field(level, p.b.time_s, w, std::move(hb), p.a.time_s);
    }
  }
  task.eval_ms = ms_since(started);
}

// ---- the ring worker's side: the ring set's chunk lists are its while a re-centre runs ---------

void TerrainMotion::run_rings(RingTask& task) {
  const i64 started = time::monotonic_ns();
  std::string error;
  u32 moved = 0;
  // The chunks' rest heights are the field at the surface's time when the camera asked, which a
  // drawn field differs from by what the sand moved since: the padding measures it.
  if (!rings_->update(task.camera_x, task.camera_z, task.time_s,
                      std::span<const TerrainRingSet::Heights>(), jobs_, moved, &error)) {
    task.ok = false;
    task.error = error;
  } else if (!(rings_->layout() == task.layout)) {
    task.ok = false;
    task.error = "terrain rings: a re-centre did not land where the frame asked it to";
  }
  task.moved = moved;
  task.built = rings_->last_built();
  task.kept = rings_->last_kept();
  task.ms = ms_since(started);
}

// ---- the frame's side ---------------------------------------------------------------------------

bool TerrainMotion::worker_busy() {
  std::unique_lock<std::mutex> lock(mutex_);
  return busy_;
}

void TerrainMotion::wait_worker() {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [&] { return !busy_ || task_.done; });
  }
  take_finished();
}

void TerrainMotion::post(Task&& task) {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    task_ = std::move(task);
    task_.done = false;
    busy_ = true;
  }
  wake_.notify_all();
}

u32 TerrainMotion::free_slot(const Level& level, u32 besides) const noexcept {
  for (u32 s = 0; s < level.slots; ++s) {
    if (s == besides || s == level.a.slot) continue;
    if (level.has_b && s == level.b.slot) continue;
    if (level.next_state != Level::Next::none && s == level.next.slot) continue;
    return s;
  }
  return ~0u;
}

// Asks the worker for `level`'s next field, timed by the cadence rule (and, without `wait`, far
// enough ahead that evaluating it keeps up). False when the worker is busy, the level holds its
// pair for a re-centre, or there is no slot.
bool TerrainMotion::schedule(u32 k) {
  Level& level = levels_[k];
  if (frozen(k) || level.next_state != Level::Next::none || !(config_.rate > 0.0)) return false;
  if (worker_busy()) return false;
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
  if (!scene_->terrain_staging(samples_of(level.window), staging, &error)) {
    ENGINE_LOG_ERROR(log_renderer, "a terrain field's staging could not be made",
                     log::field("error", error));
    return false;
  }
  level.next = Field{slot, at, 0.0, 0.0, level.window};
  level.next_state = Level::Next::evaluating;
  Task task;
  task.kind = Task::Kind::field;
  task.level = k;
  task.time_s = at;
  task.from_s = from;
  // The scene's grid measures its next field from b alone; a ring keeps its a too, which a
  // re-centre carries over to the new layout with its b.
  task.keep_from_s = k == 0 ? from : level.a.time_s;
  task.window = level.window;
  task.staging = staging;
  post(std::move(task));
  return true;
}

// The field worker's next task when it has none: a re-centre's pairs when the rings hold them,
// else the next field of the level whose b the surface reaches first.
void TerrainMotion::schedule_next() {
  if (worker_busy()) return;
  if (recentre_ == Recentre::pairs_asked) {
    schedule_pairs();
    return;
  }
  if (!(config_.rate > 0.0)) return;
  u32 best = ~0u;
  f64 best_from = 0.0;
  for (u32 k = 0; k < levels_.size(); ++k) {
    const Level& level = levels_[k];
    if (frozen(k) || level.next_state != Level::Next::none) continue;
    const f64 from = level.has_b ? level.b.time_s : level.a.time_s;
    if (best == ~0u || from < best_from) {
      best = k;
      best_from = from;
    }
  }
  if (best != ~0u) schedule(best);
}

void TerrainMotion::take_finished() {
  Task finished;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!busy_ || !task_.done) return;
    finished = std::move(task_);
    task_ = Task{};
    busy_ = false;
  }
  if (finished.kind == Task::Kind::field) {
    take_field(finished);
  } else {
    take_pairs(finished);
  }
}

// A field the worker finished goes to the scene, which the next frame copies into its slot.
void TerrainMotion::take_field(Task& finished) {
  Level& level = levels_[finished.level];
  if (frozen(finished.level) || level.next_state != Level::Next::evaluating) {
    // A ring's field the freeze made stale: the re-centre carries the ring's pair over, and the
    // field after it is asked for again once the rings are swapped.
    scene_->terrain_retire(finished.staging);
    level.next = Field{};
    level.next_state = Level::Next::none;
    return;
  }
  std::string error;
  if (!scene_->terrain_upload(finished.level, level.next.slot, finished.window, finished.staging,
                              &error)) {
    ENGINE_LOG_ERROR(log_renderer, "a terrain field could not be handed over",
                     log::field("error", error));
    scene_->terrain_retire(finished.staging);
    level.next = Field{};
    level.next_state = Level::Next::none;
    return;
  }
  level.next.delta_m = finished.delta_m;
  level.next.padding_m = finished.padding_m;
  level.next.window = finished.window;
  level.next_state = Level::Next::ready;
  level.eval_ms_ema = level.stats.evaluated == 0 ? finished.eval_ms
                                                 : 0.5 * level.eval_ms_ema + 0.5 * finished.eval_ms;
  ++level.stats.evaluated;
  level.stats.last_eval_ms = finished.eval_ms;
  level.stats.total_eval_ms += finished.eval_ms;
  level.stats.last_hash = finished.hash;
}

// ---- the rings' re-centres
// -----------------------------------------------------------------------

// The camera has left a ring's middle half: its chunks are rebuilt on the ring worker, from the
// field at the surface's time, while the sand goes on moving.
void TerrainMotion::ask_recentre(f32 camera_x, f32 camera_z) {
  const TerrainRingLayout next = rings_->next_layout(camera_x, camera_z, shown_layout_);
  if (next == shown_layout_) return;
  pending_layout_ = next;
  asked_frame_ = frames_;
  RingTask task;
  task.camera_x = camera_x;
  task.camera_z = camera_z;
  task.time_s = levels_[0].blend.surface_s;
  task.layout = next;
  {
    std::unique_lock<std::mutex> lock(ring_mutex_);
    ring_task_ = std::move(task);
    ring_task_.done = false;
    ring_busy_ = true;
  }
  ring_wake_.notify_all();
  recentre_ = Recentre::rebuilding;
}

// A finished rebuild: its new chunks queued for upload. `block` waits for it.
bool TerrainMotion::take_rings(bool block) {
  RingTask finished;
  {
    std::unique_lock<std::mutex> lock(ring_mutex_);
    if (!ring_busy_) return false;
    if (block) ring_done_.wait(lock, [&] { return ring_task_.done; });
    if (!ring_task_.done) return false;
    finished = std::move(ring_task_);
    ring_task_ = RingTask{};
    ring_busy_ = false;
  }
  ++ring_stats_.rebuilds;
  ring_stats_.chunks_built += finished.built;
  ring_stats_.chunks_kept += finished.kept;
  ring_stats_.last_rebuild_ms = finished.ms;
  ring_stats_.max_rebuild_ms = std::max(ring_stats_.max_rebuild_ms, finished.ms);
  if (!finished.ok) {
    abandon_recentre(finished.error);
    return true;
  }
  pending_moved_ = finished.moved;
  uploads_.clear();
  upload_next_ = 0;
  upload_frames_ = 0;
  upload_bytes_ = 0;
  for (u32 k = 1; k < levels_.size(); ++k) {
    if ((pending_moved_ & (1u << k)) == 0) continue;
    const Vector<TerrainChunk>& chunks = rings_->chunks(k);
    for (u32 c = 0; c < chunks.size(); ++c) {
      if (chunks[c].slot == ~0u && !chunks[c].lod.mesh.clusters.empty())
        uploads_.push_back(Upload{k, c});
    }
  }
  recentre_ = Recentre::uploading;
  return true;
}

// The queued chunks into free slots, `renderer.terrain.ring_upload_mib` a frame (at least one
// chunk), or all of them. False when one does not fit, which abandons the re-centre.
bool TerrainMotion::upload_chunks(bool all) {
  const u64 before = scene_->terrain_chunk_upload_bytes();
  u32 done = 0;
  while (upload_next_ < uploads_.size()) {
    if (!all && done > 0 && scene_->terrain_chunk_upload_bytes() - before >= upload_budget_bytes_)
      break;
    const Upload u = uploads_[upload_next_];
    TerrainChunk& chunk = rings_->chunks(u.level)[u.chunk];
    std::string error;
    if (!scene_->terrain_chunk_upload(u.level, chunk, &error)) {
      abandon_recentre(error);
      return false;
    }
    ++upload_next_;
    ++done;
  }
  const u64 bytes = scene_->terrain_chunk_upload_bytes() - before;
  if (done > 0) {
    ++upload_frames_;
    upload_bytes_ += bytes;
    ++ring_stats_.upload_frames;
    ring_stats_.chunks_uploaded += done;
    ring_stats_.upload_bytes += bytes;
  }
  return true;
}

// Every chunk is on the device: the rings hold their pairs from here to the swap. A next field a
// ring has ready is not taken — its copy lands in a slot nothing shows — and is asked for again
// after the swap.
void TerrainMotion::freeze_rings() {
  frozen_frame_ = frames_;
  recentre_ = Recentre::pairs_asked;
  for (u32 k = 1; k < levels_.size(); ++k) {
    Level& level = levels_[k];
    if (level.next_state == Level::Next::ready) {
      level.next = Field{};
      level.next_state = Level::Next::none;
    }
  }
}

// The field worker carries the held pairs over: into a ring's two free slots when its window
// moves, with new paddings either way.
bool TerrainMotion::schedule_pairs() {
  if (recentre_ != Recentre::pairs_asked || worker_busy()) return false;
  Task task;
  task.kind = Task::Kind::pairs;
  const auto give_back = [&] {
    for (Pair& p : task.pairs) {
      scene_->terrain_retire(p.staging_a);
      scene_->terrain_retire(p.staging_b);
    }
  };
  for (u32 k = 1; k < levels_.size(); ++k) {
    Level& level = levels_[k];
    Pair& p = task.pairs[k];
    p.moved = (pending_moved_ & (1u << k)) != 0;
    p.has_b = level.has_b;
    p.a = level.a;
    p.b = level.has_b ? level.b : Field{};
    const gfx::TerrainField w = rings_->field_window(k, pending_layout_);
    p.window_changed = !same_window(w, level.window);
    if (!p.window_changed) continue;
    const u32 sa = free_slot(level);
    const u32 sb = p.has_b ? free_slot(level, sa) : ~0u;
    std::string error;
    if (samples_of(w) > scene_->terrain_field_capacity(k)) {
      error = "a ring's new window is larger than its field slots";
    } else if (sa == ~0u || (p.has_b && sb == ~0u)) {
      error = "a ring has no free field slot for its pair over the new window";
    } else if (!scene_->terrain_staging(samples_of(w), p.staging_a, &error) ||
               (p.has_b && !scene_->terrain_staging(samples_of(w), p.staging_b, &error))) {
      if (error.empty()) error = "a ring's field staging could not be made";
    }
    if (!error.empty()) {
      give_back();
      abandon_recentre(error);
      return false;
    }
    p.a.slot = sa;
    p.a.window = w;
    p.a.delta_m = 0.0;
    if (p.has_b) {
      p.b.slot = sb;
      p.b.window = w;
    }
  }
  post(std::move(task));
  recentre_ = Recentre::pairing;
  return true;
}

// The carried pairs go to their slots; the swap waits for their copies to reach a frame.
void TerrainMotion::take_pairs(Task& finished) {
  ring_stats_.last_pairs_ms = finished.eval_ms;
  ring_stats_.max_pairs_ms = std::max(ring_stats_.max_pairs_ms, finished.eval_ms);
  if (recentre_ != Recentre::pairing) {
    for (Pair& p : finished.pairs) {
      scene_->terrain_retire(p.staging_a);
      scene_->terrain_retire(p.staging_b);
    }
    return;
  }
  for (u32 k = 0; k < k_max_terrain_levels; ++k)
    pending_[k] = std::move(finished.pairs[k]);
  for (u32 k = 1; k < levels_.size(); ++k) {
    Pair& p = pending_[k];
    if (!p.window_changed) continue;
    std::string error;
    bool ok = scene_->terrain_upload(k, p.a.slot, p.a.window, p.staging_a, &error);
    if (ok) p.staging_a = gfx::BufferResource{};
    if (ok && p.has_b) {
      ok = scene_->terrain_upload(k, p.b.slot, p.b.window, p.staging_b, &error);
      if (ok) p.staging_b = gfx::BufferResource{};
    }
    if (!ok) {
      abandon_recentre(error);
      return;
    }
  }
  recentre_ = Recentre::swapping;
}

// **The swap**, in one frame: every moved ring's new chunks on and its replaced ones off, the
// rings' pairs over their new windows (the same times, the same blend: the same sand), the scene
// grid's hole where the middle ring now is.
void TerrainMotion::swap_rings() {
  // The arenas are fullest now, the new chunks in beside the old: what a ring's room is sized for.
  for (u32 k = 1; k < levels_.size(); ++k) {
    const GpuScene::ArenaFree a = scene_->terrain_arena_free(k);
    if (a.triangle_capacity == 0 || a.vertex_capacity == 0) continue;
    const f64 share =
        std::max(1.0 - static_cast<f64>(a.triangles) / static_cast<f64>(a.triangle_capacity),
                 1.0 - static_cast<f64>(a.vertices) / static_cast<f64>(a.vertex_capacity));
    ring_stats_.arena_peak_share = std::max(ring_stats_.arena_peak_share, share);
  }
  for (u32 k = 1; k < levels_.size(); ++k) {
    Level& level = levels_[k];
    Pair& p = pending_[k];
    if (p.moved) {
      Vector<u32> slots;
      for (const TerrainChunk& chunk : rings_->chunks(k))
        if (chunk.slot != ~0u) slots.push_back(chunk.slot);
      for (const u32 s : level.shown_slots) {
        if (std::find(slots.begin(), slots.end(), s) == slots.end())
          scene_->terrain_chunk_show(k, s, false);
      }
      for (const u32 s : slots) {
        if (std::find(level.shown_slots.begin(), level.shown_slots.end(), s) ==
            level.shown_slots.end())
          scene_->terrain_chunk_show(k, s, true);
      }
      level.shown_slots = std::move(slots);
    }
    if (p.window_changed) {
      level.a = p.a;
      if (p.has_b) level.b = p.b;
      level.window = p.a.window;
    } else if (p.moved) {
      level.a.padding_m = p.a.padding_m;
      if (p.has_b) level.b.padding_m = p.b.padding_m;
    }
    p = Pair{};
  }
  shown_layout_ = pending_layout_;
  recentre_ = Recentre::none;
  ++ring_stats_.swaps;
  ring_stats_.last_swap_frames = static_cast<u32>(frames_ - asked_frame_);
  ring_stats_.last_frozen_frames = static_cast<u32>(frames_ - frozen_frame_);
  ring_stats_.last_upload_frames = upload_frames_;
  ring_stats_.last_upload_bytes = upload_bytes_;
  uploads_.clear();
  upload_next_ = 0;
  ENGINE_LOG_INFO(
      log_renderer, "terrain rings swapped", log::field("frames", frames_ - asked_frame_),
      log::field("frozen_frames", frames_ - frozen_frame_),
      log::field("rebuild_ms", ring_stats_.last_rebuild_ms),
      log::field("pairs_ms", ring_stats_.last_pairs_ms), log::field("upload_bytes", upload_bytes_),
      log::field("upload_frames", upload_frames_));
}

// A re-centre that cannot be drawn: whatever it uploaded is freed, the rings stay where they are
// drawn, and no further re-centre is asked for — the ground near the camera stays fine where it
// was and coarse past it, which is a picture, where a half-swapped ring would be a hole.
void TerrainMotion::abandon_recentre(const std::string& why) {
  ENGINE_LOG_ERROR(log_renderer,
                   "a terrain ring re-centre was dropped; the rings stay where they are",
                   log::field("error", why));
  for (u32 k = 1; k < levels_.size(); ++k) {
    const GpuScene::ArenaFree a = scene_->terrain_arena_free(k);
    ENGINE_LOG_INFO(
        log_renderer, "terrain ring arenas", log::field("level", k),
        log::field("free_vertices", a.vertices), log::field("largest_vertices", a.largest_vertices),
        log::field("vertex_capacity", a.vertex_capacity), log::field("free_triangles", a.triangles),
        log::field("largest_triangles", a.largest_triangles),
        log::field("triangle_capacity", a.triangle_capacity),
        log::field("free_slots", scene_->terrain_free_slots(k)));
  }
  ++ring_stats_.failed;
  rings_stopped_ = true;
  for (u32 u = 0; u < upload_next_ && u < uploads_.size(); ++u) {
    TerrainChunk& chunk = rings_->chunks(uploads_[u].level)[uploads_[u].chunk];
    if (chunk.slot != ~0u) {
      scene_->terrain_chunk_show(uploads_[u].level, chunk.slot, false);
      chunk.slot = ~0u;
    }
  }
  for (Pair& p : pending_) {
    scene_->terrain_retire(p.staging_a);
    scene_->terrain_retire(p.staging_b);
    p = Pair{};
  }
  uploads_.clear();
  upload_next_ = 0;
  recentre_ = Recentre::none;
}

// One frame's step of a re-centre; with `complete` (offscreen, and a frame whose surface waits
// for a frozen ring), every step at once, waiting for the workers: the re-centre is then drawn
// from the frame that asked for it, and the pictures of a run are a function of its frames.
void TerrainMotion::advance_recentre(bool complete) {
  for (u32 guard = 0; guard < 64; ++guard) {
    switch (recentre_) {
      case Recentre::none: return;
      case Recentre::rebuilding:
        if (!take_rings(complete)) return;
        break;
      case Recentre::uploading:
        if (!upload_chunks(complete)) return;
        if (upload_next_ < uploads_.size()) return;
        freeze_rings();
        break;
      case Recentre::pairs_asked:
        if (schedule_pairs()) break;
        if (recentre_ != Recentre::pairs_asked || !complete) return;
        wait_worker();  // another field first; the pairs next time round
        break;
      case Recentre::pairing:
        if (!complete) {
          take_finished();
          if (recentre_ == Recentre::pairing) return;
          break;
        }
        wait_worker();
        break;
      case Recentre::swapping:
        // The pairs' copies are recorded by the next frame, before anything reads a field.
        swap_rings();
        return;
    }
  }
}

void TerrainMotion::frame(f64 real_dt_s, f32 camera_x, f32 camera_z) {
  if (!active()) return;
  ++frames_;
  game_s_ += real_dt_s * config_.rate;
  const f64 target = start_s_ + game_s_;
  take_finished();
  if (rings_ != nullptr && !rings_stopped_) {
    if (recentre_ == Recentre::none) ask_recentre(camera_x, camera_z);
    advance_recentre(config_.wait);
  }
  const u32 n = levels_.size();
  f64 budget[k_max_terrain_levels];
  f64 moved[k_max_terrain_levels] = {};
  bool waited[k_max_terrain_levels] = {};
  for (u32 k = 0; k < n; ++k)
    budget[k] = config_.fraction * levels_[k].spacing_m;
  TerrainSurfaceResult result;
  for (u32 guard = 0; guard < 256; ++guard) {
    TerrainBlend blends[k_max_terrain_levels];
    TerrainNextField next[k_max_terrain_levels];
    f64 step[k_max_terrain_levels];
    u32 installed[k_max_terrain_levels];
    for (u32 k = 0; k < n; ++k) {
      const Level& level = levels_[k];
      blends[k] = level.blend;
      next[k].ready = level.next_state == Level::Next::ready && !frozen(k);
      next[k].time_s = level.next.time_s;
      next[k].delta_m = level.next.delta_m;
    }
    result =
        terrain_surface_frame(std::span<TerrainBlend>(blends, n), target,
                              std::span<const f64>(budget, n), std::span<TerrainNextField>(next, n),
                              std::span<f64>(step, n), std::span<u32>(installed, n));
    for (u32 k = 0; k < n; ++k) {
      Level& level = levels_[k];
      level.blend = blends[k];
      moved[k] += step[k];
      budget[k] = std::max(budget[k] - step[k], 0.0);
      if (installed[k] > 0) {
        // The field slots follow the pair: b's slot is a's now, the next field's is b's.
        if (level.has_b) level.a = level.b;
        level.b = level.next;
        level.has_b = true;
        level.next = Field{};
        level.next_state = Level::Next::none;
        level.stats.max_delta_m = std::max(level.stats.max_delta_m, level.b.delta_m);
        ++level.stats.installed;
      }
    }
    if (!result.held || !config_.wait || result.limiting >= n) break;
    // Offscreen: a field the surface has caught up with is waited for, so the picture of a frame
    // is a function of the frame. The worker may be busy with another level's field; that one is
    // taken, and this one asked for.
    const u32 h = result.limiting;
    Level& level = levels_[h];
    if (frozen(h)) {
      advance_recentre(true);
      if (frozen(h)) break;
    } else if (level.next_state == Level::Next::none) {
      if (!schedule(h)) {
        if (!worker_busy()) break;
        wait_worker();
      }
    } else if (level.next_state == Level::Next::evaluating) {
      wait_worker();
    } else {
      break;
    }
    waited[h] = true;
  }
  last_move_m_ = 0.0;
  for (u32 k = 0; k < n; ++k) {
    Level& level = levels_[k];
    LevelStats& stats = level.stats;
    if (result.held && result.limiting == k) ++stats.held;
    if (result.capped && result.limiting == k) ++stats.capped;
    if (waited[k]) ++stats.waited;
    stats.max_move_m = std::max(stats.max_move_m, moved[k]);
    stats.max_lag_s = std::max(stats.max_lag_s, target - level.blend.surface_s);
    last_move_m_ = std::max(last_move_m_, moved[k]);
    show(k);
  }
  schedule_next();
}

void TerrainMotion::show(u32 k) {
  const Level& level = levels_[k];
  const f64 padding = std::max(level.a.padding_m, level.has_b ? level.b.padding_m : 0.0);
  // The scene's grid leaves out the square the middle ring draws: its clusters wholly inside are
  // culled and its vertices inside are moved onto the square's edge (deform.slang).
  const Vec4 hole = k == 0 && rings_ != nullptr ? rings_->square(1, shown_layout_) : Vec4{};
  scene_->terrain_show(k, level.a.slot, level.has_b ? level.b.slot : ~0u,
                       static_cast<f32>(level.blend.blend()), static_cast<f32>(padding) + 0.01f,
                       hole);
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
