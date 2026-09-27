// The dunes at high time-lapse rates, on the CPU (renderer.md, "The dunes in time-lapse", "A
// clock that never stops"; the experiment in docs/experiments/time-lapse-smoothness-2026-09-27.md).
// A model of the interactive time-lapse with the erg's three levels — the scene's grid at 1.5 m,
// the middle ring at a metre, the inner at 50 cm — on **one field worker** that evaluates them in
// turn, each field taking what it takes on the owner's machine (45 frames at 60 fps for the grid,
// 11 for the middle ring, 4 for the inner), driven through the same functions `TerrainMotion`
// runs: the cadence by displacement, `terrain_keep_up_time`, `TerrainClock` and
// `terrain_surface_frame`. The heights are a travelling profile per level, so the move of every
// sample the pool pass would draw is checked against the level's bound every frame.
//
// **The smoothness metric.** The sand drawn is a function of the surface time, so its motion is
// the surface time's advance per frame, as a share of the true one (`rate / 60`): the *speed
// ratio*. A time-lapse is smooth when that ratio changes slowly. Two numbers say whether it does:
//   - **stop-go**: of the frames the surface stood still while game time ran, the share followed
//     within a second (60 frames) by a frame above twice the true speed — the hold-then-burst the
//     owner saw;
//   - **window ratio**: over every one-second window, the largest speed ratio over the smallest
//     (a stand-still counted as 1% of the true speed, so a stop inside a moving second reads 100
//     or more), the worst window of the run.
// Both are taken after the first frame the surface moves: before it, no field but the first
// exists anywhere, and standing still is the only answer.
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/gfx/cluster_cull.h>
#include <systems/renderer/terrain_time.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr f64 k_dt = 1.0 / 60.0;
constexpr u32 k_warm_up = 180;  // frames the metric leaves out: the start from rest
constexpr u32 k_samples = 192;
// The erg's waves, its fastest band: about 330 m a year, 0.9 m a game day.
constexpr f64 k_speed = 0.9 / 86'400.0;

// A dune profile travelling downwind: 2 m tall, 12 m from crest to crest, a sharp brink.
Vector<f32> profile(f64 time_s, f64 spacing) {
  Vector<f32> out(k_samples);
  for (u32 i = 0; i < k_samples; ++i) {
    const f64 x = static_cast<f64>(i) * spacing - k_speed * time_s;
    const f64 u = x / 12.0 - std::floor(x / 12.0);
    out[i] = static_cast<f32>(u < 0.8 ? 2.0 * u / 0.8 : 2.0 * (1.0 - u) / 0.2);
  }
  return out;
}

struct LevelSpec {
  f64 spacing_m = 0.0;
  u32 eval_frames = 0;  // the field worker's time for one of its fields
  u32 slots = 4;        // the GPU scene's field slots: a, b, and the fields after b
};

// The erg's levels and their evaluation times on the owner's 16 cores (750 ms for the grid), with
// the GPU scene's slots: three for the grid (so one field after b), four for a ring (two).
constexpr LevelSpec k_erg[] = {{1.5, 45, 3}, {1.0, 11, 4}, {0.5, 4, 4}};

enum class Mode : u8 {
  before,  // the code before the clock: the surface chases game time, fields timed by eval alone
  after,   // the clock, and fields timed by the turnaround (evaluation plus the wait)
};

struct ModelRun {
  f64 stop_go = 0.0;         // share of stand-stills followed within a second by a burst
  f64 window_ratio = 0.0;    // worst one-second window's largest speed ratio over its smallest
  f64 stopped = 0.0;         // share of frames the surface stood still
  f64 max_speed = 0.0;       // the fastest frame, as a multiple of the true speed
  f64 min_speed = 0.0;       // the slowest after the start
  f64 max_move_share = 0.0;  // the most a level's sample moved in a frame over its bound
  f64 latency_s = 0.0;       // L at the end, game seconds
  f64 latency_frames = 0.0;  // and in frames at the rate
  f64 final_lag_s = 0.0;     // game time minus the surface at the end
  u32 fields = 0;            // fields evaluated
  u32 late = 0;              // of which the keep-up rule timed
  u32 late_steady = 0;       // of which asked after the warm-up
  bool exact_ends = true;    // every handover drew b's heights to the bit
  bool monotonic = true;
  bool shared = true;  // every level's surface time the same, every frame
};

ModelRun run_model(f64 rate, Mode mode, u32 frames, std::span<const LevelSpec> specs,
                   u32 stall_at = 0, u32 stall = 0, usize depth_override = 0) {
  // Fields after b a level may hold: one before the clock; after it, what its slots leave.
  const auto depth_of = [&](u32 k) -> usize {
    if (depth_override != 0) return depth_override;
    if (mode == Mode::before) return 1u;
    return std::min<usize>(2u, specs[k].slots > 2 ? specs[k].slots - 2 : 1u);
  };
  const u32 n = static_cast<u32>(specs.size());
  const f64 t0 = 94'608'000.0;
  const f64 fraction = 0.25;
  const f64 min_step = 60.0;
  const f64 max_step = 2'592'000.0;
  const f64 lead = 1.5;
  struct Level {
    f64 spacing = 0.0;
    f64 budget = 0.0;
    gfx::TerrainField window{};
    Vector<f32> a, b, latest, previous;
    // Fields evaluated and not yet taken as b, oldest first: at most `depth`.
    Vector<Vector<f32>> queued;
    Vector<TerrainNextField> queued_next;
    f64 latest_time = 0.0;
    f64 want_real = 0.0;   // when it last came to want a field (-1: it has what it may hold)
    f64 turnaround = 0.0;  // ema of wanting to ready, real seconds
    f64 delta = 0.0;       // its last pair's largest difference
    bool pending = false;  // on the worker, or waiting its turn
    f64 max_move = 0.0;
  };
  Vector<Level> levels(n);
  TerrainBlend blends[k_max_surface_levels];
  TerrainNextField next[k_max_surface_levels];
  for (u32 k = 0; k < n; ++k) {
    Level& l = levels[k];
    l.spacing = specs[k].spacing_m;
    l.budget = fraction * l.spacing;
    l.window.nx = k_samples;
    l.window.nz = 1;
    l.a = profile(t0, l.spacing);
    l.b = l.a;
    l.latest = l.a;
    l.previous = l.a;
    l.latest_time = t0;
    l.turnaround = static_cast<f64>(specs[k].eval_frames) * k_dt;
    blends[k].time_a = blends[k].time_b = blends[k].surface_s = t0;
  }
  TerrainClock clock;
  TerrainClockConfig clock_config;
  clock_config.rate = rate;
  clock.reset(clock_config);
  // The worker: one field at a time.
  bool busy = false;
  u32 busy_level = 0;
  u32 done_at = 0;
  f64 busy_time = 0.0;
  ModelRun run;
  Vector<f64> speed;  // per frame, the surface's advance over the true one
  speed.reserve(frames);
  f64 real = 0.0;
  u32 first_move = ~0u;
  for (u32 f = 1; f <= frames; ++f) {
    real += k_dt;
    const f64 game = t0 + rate * real;
    // Take a finished field (TerrainMotion::take_finished).
    if (busy && f >= done_at) {
      Level& l = levels[busy_level];
      Vector<f32> field = profile(busy_time, l.spacing);
      TerrainNextField nf;
      nf.ready = true;
      nf.time_s = busy_time;
      nf.delta_m = terrain_field_delta(std::span<const f32>(l.latest.data(), k_samples), l.window,
                                       std::span<const f32>(field.data(), k_samples), l.window);
      const f64 took = real - l.want_real;
      clock.arrived(busy_level, took);
      l.turnaround = 0.5 * l.turnaround + 0.5 * took;
      l.latest = field;
      l.latest_time = busy_time;
      l.queued.push_back(std::move(field));
      l.queued_next.push_back(nf);
      if (!next[busy_level].ready) next[busy_level] = l.queued_next[0];
      l.want_real = l.queued.size() < depth_of(busy_level) ? real : -1.0;
      l.pending = false;
      busy = false;
    }
    // The surface (TerrainMotion::frame).
    const f64 before = blends[0].surface_s;
    f64 target = game;
    if (mode == Mode::after) {
      f64 horizon = game;
      for (u32 k = 0; k < n; ++k) {
        const TerrainBlend& bl = blends[k];
        const f64 frontier = next[k].ready ? next[k].time_s : (bl.has_b ? bl.time_b : bl.time_a);
        horizon = std::min(horizon, frontier);
      }
      target = clock.advance(k_dt, game, before, std::max(horizon, before));
    }
    bool had_b[k_max_surface_levels];
    f64 budgets[k_max_surface_levels];
    f64 moved[k_max_surface_levels] = {};
    u32 installed[k_max_surface_levels] = {};
    for (u32 k = 0; k < n; ++k) {
      had_b[k] = blends[k].has_b;
      budgets[k] = levels[k].budget;
    }
    terrain_surface_frame(std::span<TerrainBlend>(blends, n), target,
                          std::span<const f64>(budgets, n), std::span<TerrainNextField>(next, n),
                          std::span<f64>(moved, n), std::span<u32>(installed, n));
    if (mode == Mode::after) clock.settle(k_dt, before, blends[0].surface_s);
    for (u32 k = 1; k < n; ++k)
      run.shared = run.shared && blends[k].surface_s == blends[0].surface_s;
    run.monotonic = run.monotonic && blends[0].surface_s >= before;
    for (u32 k = 0; k < n; ++k) {
      Level& l = levels[k];
      if (installed[k] > 0) {
        if (had_b[k]) l.a = l.b;
        l.b = std::move(l.queued[0]);
        l.queued.erase(l.queued.begin());
        l.queued_next.erase(l.queued_next.begin());
        if (!l.queued_next.empty()) next[k] = l.queued_next[0];
        if (l.want_real < 0.0 && !l.pending) l.want_real = real;
        l.delta = blends[k].delta_m;
      }
      const f32 t = static_cast<f32>(blends[k].blend());
      f64 frame_move = 0.0;
      bool at_b = blends[k].has_b && t == 1.0f;
      for (u32 i = 0; i < k_samples; ++i) {
        const f32 h = blends[k].has_b ? l.a[i] * (1.0f - t) + l.b[i] * t : l.a[i];
        if (at_b && h != l.b[i]) run.exact_ends = false;
        frame_move =
            std::max(frame_move, std::abs(static_cast<f64>(h) - static_cast<f64>(l.previous[i])));
        l.previous[i] = h;
      }
      l.max_move = std::max(l.max_move, frame_move);
    }
    const f64 ds = blends[0].surface_s - before;
    if (first_move == ~0u && ds > 0.0) first_move = f;
    speed.push_back(ds / (rate * k_dt));
    // The worker's next task (TerrainMotion::schedule_next): the level whose newest field the
    // surface reaches first, when it wants one.
    const bool stalled = f >= stall_at && f < stall_at + stall;
    if (!busy && !stalled) {
      u32 best = ~0u;
      f64 best_from = 0.0;
      for (u32 k = 0; k < n; ++k) {
        if (levels[k].pending || levels[k].queued.size() >= depth_of(k)) continue;
        const f64 from = levels[k].latest_time;
        if (best == ~0u || from < best_from) {
          best = k;
          best_from = from;
        }
      }
      if (best != ~0u) {
        Level& l = levels[best];
        const f64 cadence =
            best_from + std::clamp(fraction * l.spacing / k_speed, min_step, max_step);
        TerrainKeepUp keep;
        keep.rate = rate;
        keep.lead = lead;
        keep.longest_step_s = max_step;
        if (mode == Mode::after) {
          keep.lead = lead * static_cast<f64>(3 - depth_of(best));
          keep.turnaround_s = std::max(clock.turnaround_s(best), l.turnaround);
          keep.frame_s = k_dt;
          keep.delta_m = l.delta;
          keep.budget_m = l.budget;
        } else {
          keep.turnaround_s = static_cast<f64>(specs[best].eval_frames) * k_dt;
        }
        bool late = false;
        busy_time = terrain_keep_up_time(best_from, cadence, keep, &late);
        run.late += late ? 1u : 0u;
        run.late_steady += late && f > k_warm_up ? 1u : 0u;
        ++run.fields;
        l.pending = true;
        busy = true;
        busy_level = best;
        done_at = f + specs[best].eval_frames;
      }
    }
    run.final_lag_s = game - blends[0].surface_s;
  }
  // The metric, from the first frame that moved.
  const u32 start = first_move == ~0u ? frames : std::max(first_move - 1, k_warm_up);
  u32 stops = 0;
  u32 stop_go = 0;
  u32 counted = 0;
  run.min_speed = 1.0e30;
  for (u32 i = start; i < speed.size(); ++i) {
    ++counted;
    run.max_speed = std::max(run.max_speed, speed[i]);
    run.min_speed = std::min(run.min_speed, speed[i]);
    if (speed[i] > 0.0) continue;
    ++stops;
    for (u32 j = i + 1; j < std::min<u32>(i + 61, static_cast<u32>(speed.size())); ++j) {
      if (speed[j] > 2.0) {
        ++stop_go;
        break;
      }
    }
  }
  if (counted == 0) run.min_speed = 0.0;
  run.stopped = counted > 0 ? static_cast<f64>(stops) / counted : 0.0;
  run.stop_go = counted > 0 ? static_cast<f64>(stop_go) / counted : 0.0;
  for (u32 i = start; i + 60 <= speed.size(); ++i) {
    f64 hi = 0.0;
    f64 lo = 1.0e30;
    for (u32 j = i; j < i + 60; ++j) {
      hi = std::max(hi, speed[j]);
      lo = std::min(lo, std::max(speed[j], 0.01));
    }
    run.window_ratio = std::max(run.window_ratio, hi / lo);
  }
  for (u32 k = 0; k < n; ++k)
    run.max_move_share = std::max(run.max_move_share, levels[k].max_move / levels[k].budget);
  run.latency_s = clock.latency_s();
  run.latency_frames = clock.latency_s() / (rate * k_dt);
  return run;
}

void report(const char* what, f64 rate, const ModelRun& r, bool clock = true) {
  char line[320];
  char latency[64] = "no clock";
  if (clock) {
    std::snprintf(latency, sizeof latency, "L %.0f s (%.1f frames)", r.latency_s, r.latency_frames);
  }
  std::snprintf(line, sizeof line,
                "%s rate %.0f: stop-go %.3f, window ratio %.1f, stopped %.3f, speed %.2f..%.2f, "
                "move/bound %.3f, %s, lag %.0f s, fields %u (late %u)",
                what, rate, r.stop_go, r.window_ratio, r.stopped, r.min_speed, r.max_speed,
                r.max_move_share, latency, r.final_lag_s, r.fields, r.late);
  MESSAGE(std::string(line));
}

constexpr f64 k_rates[] = {8'640.0, 86'400.0, 604'800.0};

}  // namespace

TEST_CASE("renderer: the time-lapse model, before the clock, stops and bursts at high rates") {
  // What the owner saw, reproduced: at a day and a week a second the one worker cannot keep three
  // levels' fields coming on the evaluation's own time, the surface catches up with a field that
  // is not there, stands, and then catches up at the per-frame bound. Every frame is still within
  // the bound and every handover exact: the motion is safe and it is not smooth.
  for (const f64 rate : k_rates) {
    CAPTURE(rate);
    const ModelRun r = run_model(rate, Mode::before, 900, k_erg);
    report("before", rate, r, false);
    CHECK(r.max_move_share <= 1.0 + 1.0e-4);
    CHECK(r.exact_ends);
    CHECK(r.monotonic);
    CHECK(r.shared);
    // At a day a second it stands and then sprints (stop-go); at a week a second the bound keeps
    // the sprint under twice the true speed, and it stands six frames in ten instead.
    if (rate >= 86'400.0) CHECK(r.window_ratio > 10.0);
  }
}

TEST_CASE("renderer: the surface clock keeps the sand's speed continuous at every rate") {
  for (const f64 rate : k_rates) {
    CAPTURE(rate);
    const ModelRun r = run_model(rate, Mode::after, 1'200, k_erg);
    report("after", rate, r);
    // Every invariant the blend had it keeps: the bound, exact ends, one surface time.
    CHECK(r.max_move_share <= 1.0 + 1.0e-4);
    CHECK(r.exact_ends);
    CHECK(r.monotonic);
    CHECK(r.shared);
    // Never a stop followed by a burst, never faster than the catch-up allows.
    CHECK(r.stop_go == 0.0);
    CHECK(r.stopped == 0.0);
    CHECK(r.max_speed <= 1.5 + 1.0e-9);
    CHECK(r.window_ratio <= 1.25);
    // Two runs are the same run.
    const ModelRun again = run_model(rate, Mode::after, 1'200, k_erg);
    CHECK(again.window_ratio == r.window_ratio);
    CHECK(again.final_lag_s == r.final_lag_s);
  }
}

TEST_CASE("renderer: a stalled worker slows the sand to a stop and sets it off again gently") {
  // No field for five seconds at a week a second: the surface brakes for the newest field every
  // level has, stands, and sets off without a burst when the worker comes back.
  const ModelRun r = run_model(604'800.0, Mode::after, 1'500, k_erg, 400, 300);
  report("stall", 604'800.0, r);
  CHECK(r.max_move_share <= 1.0 + 1.0e-4);
  CHECK(r.exact_ends);
  CHECK(r.stop_go == 0.0);
  CHECK(r.max_speed <= 1.5 + 1.0e-9);
}

TEST_CASE(
    "renderer: the surface clock's latency covers the worst turnaround and brakes for the "
    "horizon") {
  TerrainClock clock;
  TerrainClockConfig config;
  config.rate = 100.0;
  clock.reset(config);
  CHECK(clock.latency_s() == 0.0);
  // 1.2 times the worst recent turnaround at the rate.
  clock.arrived(0, 0.5);
  CHECK(clock.latency_s() == doctest::Approx(60.0));
  clock.arrived(0, 0.25);
  CHECK(clock.latency_s() == doctest::Approx(60.0));
  clock.arrived(1, 1.0);
  CHECK(clock.latency_s() == doctest::Approx(120.0));  // the largest of the levels'
  CHECK(clock.turnaround_s(0) == 0.5);
  // A level forgets a turnaround after eight more arrivals.
  for (u32 i = 0; i < k_clock_samples; ++i)
    clock.arrived(1, 0.1);
  CHECK(clock.latency_s() == doctest::Approx(60.0));
  // From rest the speed grows by the rate every half second at most.
  const f64 s1 = clock.advance(0.1, 1'000.0, 0.0, 1.0e9);
  CHECK(s1 == doctest::Approx(0.1 * 20.0));
  clock.settle(0.1, 0.0, s1);
  CHECK(clock.speed() == doctest::Approx(20.0));
  // Never past the horizon, and never faster than stopping at it allows.
  const f64 s2 = clock.advance(0.1, 1'000.0, s1, s1 + 0.5);
  CHECK(s2 <= s1 + 0.5);
  CHECK(s2 - s1 <= std::sqrt(2.0 * 200.0 * 0.5) * 0.1 + 1.0e-12);
  // And never backwards.
  CHECK(clock.advance(0.1, 0.0, 5.0, 5.0) == 5.0);
  // The keep-up rule: the turnaround at the rate times the lead, and the bound's span.
  TerrainKeepUp keep;
  keep.rate = 1'000.0;
  keep.turnaround_s = 0.5;
  keep.lead = 1.5;
  bool late = false;
  CHECK(terrain_keep_up_time(0.0, 100.0, keep, &late) == doctest::Approx(750.0));
  CHECK(late);
  CHECK(terrain_keep_up_time(0.0, 2'000.0, keep, &late) == 2'000.0);
  CHECK_FALSE(late);
  keep.frame_s = 0.1;
  keep.delta_m = 2.0;
  keep.budget_m = 0.1;  // 20 frames of game time, and a quarter more
  CHECK(terrain_keep_up_time(0.0, 100.0, keep) == doctest::Approx(2'500.0));
  keep.longest_step_s = 1'000.0;
  CHECK(terrain_keep_up_time(0.0, 100.0, keep) == doctest::Approx(1'000.0));
}

TEST_CASE("renderer: the highest rate one field worker keeps on the displacement cadence") {
  // Past it, the keep-up rule times some fields further apart than a quarter of a sample of the
  // waves' travel, and the waves cross-fade between fields rather than slide: still smooth (the
  // clock), no longer a slide. The rate is found on a ladder of a tenth more each step, for the
  // owner's evaluation times and for the grid's at a tenth less (what four blocks a job instead of
  // sixteen buys a 32-thread pool, experiments/time-lapse-smoothness-2026-09-27.md).
  const LevelSpec faster[] = {{1.5, 41, 3}, {1.0, 10, 4}, {0.5, 4, 4}};
  const auto sustained = [](std::span<const LevelSpec> specs) {
    f64 best = 0.0;
    for (f64 rate = 500.0; rate < 1.0e6; rate *= 1.1) {
      const ModelRun r = run_model(rate, Mode::after, 900, specs);
      if (r.late_steady > 0) break;
      best = rate;
    }
    return best;
  };
  const f64 now = sustained(k_erg);
  const f64 then = sustained(faster);
  char line[160];
  std::snprintf(line, sizeof line,
                "on the displacement cadence up to %.0f game s a second (45-frame grid), %.0f "
                "(41-frame grid)",
                now, then);
  MESSAGE(std::string(line));
  CHECK(now > 1'000.0);
  CHECK(then >= now);
  CHECK(now < 604'800.0);  // a week a second is not a slide on the CPU (the GPU note says why)
}
