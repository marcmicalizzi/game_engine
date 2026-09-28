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
#include <iterator>
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

// How far the waves have travelled by a game time, metres: `k_speed` a second, and during a storm
// (`from`..`to`, game seconds) `factor` times that — the storm's flux over a mean hour's, times
// its transport gain (terrain.md, "A storm scales transport"). No storm is a straight line.
struct Transport {
  f64 from = 0.0;
  f64 to = 0.0;
  f64 factor = 1.0;
  f64 travel(f64 t) const noexcept {
    const f64 in = std::clamp(t, from, to) - from;
    return k_speed * (t + (factor - 1.0) * in);
  }
  // The cadence by displacement (`terrain_next_time`): the game time after `start` at which the
  // waves will have travelled `target`, clamped to [start + lo, start + hi]. The travel is
  // piecewise linear, so its inverse is exact; `gained` false reads the storm as a calm day's.
  f64 next_time(f64 start, f64 target, f64 lo, f64 hi, bool gained = true) const noexcept {
    const Transport calm{};
    const Transport& t = gained ? *this : calm;
    f64 a = start;
    f64 b = start + hi;
    if (t.travel(b) - t.travel(start) <= target) return b;
    for (u32 i = 0; i < 64; ++i) {
      const f64 mid = 0.5 * (a + b);
      if (t.travel(mid) - t.travel(start) <= target) {
        a = mid;
      } else {
        b = mid;
      }
    }
    return std::max(a, start + lo);
  }
};

// A dune profile travelling downwind by `travel` metres: 2 m tall, 12 m from crest to crest, a
// sharp brink.
Vector<f32> profile_at(f64 travel, f64 spacing) {
  Vector<f32> out(k_samples);
  for (u32 i = 0; i < k_samples; ++i) {
    const f64 x = static_cast<f64>(i) * spacing - travel;
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
  // With the rate changed while it runs (`RateStep`s): per frame, the rate in force and the
  // surface's advance in game seconds, for the metrics of a change.
  Vector<f64> rate_of;
  Vector<f64> advance_of;
  u32 changes = 0;
  // The most the waves travelled between the two fields of any pair, over the level's spacing:
  // at most the cadence's fraction when the pair is a slide, more when it is a cross-fade.
  f64 max_pair_travel_share = 0.0;
};

// The rate from a frame on: frame 0 is the start, and a later one is a change made between the
// frame before and this one — where engine-view's `,` and `.` land, since a frame's time-lapse step
// runs before its input is read (`TerrainMotion::set_rate`).
struct RateStep {
  u32 frame = 0;
  f64 rate = 0.0;
};

ModelRun run_model(std::span<const RateStep> schedule, Mode mode, u32 frames,
                   std::span<const LevelSpec> specs, u32 stall_at = 0, u32 stall = 0,
                   usize depth_override = 0, const Transport* transport = nullptr,
                   f64 min_step_s = 60.0, bool cadence_gained = true) {
  const Transport calm{};
  const Transport& sand = transport != nullptr ? *transport : calm;
  f64 rate = schedule[0].rate;
  u32 next_step = 1;
  // Game time is the start plus `rate * real` since the last change plus what the changes before
  // it ran up, so a run with no change computes it exactly as `t0 + rate * real`.
  f64 game_before = 0.0;  // game seconds run up to the last change
  f64 real_at = 0.0;      // real seconds at the last change
  // Fields after b a level may hold: one before the clock; after it, what its slots leave.
  const auto depth_of = [&](u32 k) -> usize {
    if (depth_override != 0) return depth_override;
    if (mode == Mode::before) return 1u;
    return std::min<usize>(2u, specs[k].slots > 2 ? specs[k].slots - 2 : 1u);
  };
  const u32 n = static_cast<u32>(specs.size());
  const f64 t0 = 94'608'000.0;
  const f64 fraction = 0.25;
  const f64 min_step = min_step_s;
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
    l.a = profile_at(sand.travel(t0), l.spacing);
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
  run.rate_of.reserve(frames);
  run.advance_of.reserve(frames);
  f64 real = 0.0;
  u32 first_move = ~0u;
  for (u32 f = 1; f <= frames; ++f) {
    // A change of rate between the last frame and this one: what `TerrainMotion::set_rate` does
    // besides changing the rate game time runs at — the clock takes the rate, and a level with
    // nothing on its way comes to want its next field now.
    if (next_step < schedule.size() && schedule[next_step].frame <= f) {
      const f64 to = schedule[next_step].rate;
      ++next_step;
      if (to != rate) {
        game_before += rate * (real - real_at);
        real_at = real;
        rate = to;
        clock.set_rate(to);
        ++run.changes;
        for (Level& l : levels) {
          if (l.want_real >= 0.0 && !l.pending) l.want_real = real;
        }
      }
    }
    real += k_dt;
    const f64 game = t0 + (game_before + rate * (real - real_at));
    // Take a finished field (TerrainMotion::take_finished).
    if (busy && f >= done_at) {
      Level& l = levels[busy_level];
      Vector<f32> field = profile_at(sand.travel(busy_time), l.spacing);
      run.max_pair_travel_share =
          std::max(run.max_pair_travel_share,
                   (sand.travel(busy_time) - sand.travel(l.latest_time)) / l.spacing);
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
    speed.push_back(rate > 0.0 ? ds / (rate * k_dt) : 0.0);
    run.rate_of.push_back(rate);
    run.advance_of.push_back(ds);
    // The worker's next task (TerrainMotion::schedule_next): the level whose newest field the
    // surface reaches first, when it wants one.
    const bool stalled = f >= stall_at && f < stall_at + stall;
    // At a rate of zero nothing is asked for (`TerrainMotion::schedule`).
    if (!busy && !stalled && rate > 0.0) {
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
            sand.next_time(best_from, fraction * l.spacing, min_step, max_step, cadence_gained);
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
  run.latency_frames = rate > 0.0 ? clock.latency_s() / (rate * k_dt) : 0.0;
  return run;
}

ModelRun run_model(f64 rate, Mode mode, u32 frames, std::span<const LevelSpec> specs,
                   u32 stall_at = 0, u32 stall = 0, usize depth_override = 0) {
  const RateStep one[] = {{0, rate}};
  return run_model(std::span<const RateStep>(one), mode, frames, specs, stall_at, stall,
                   depth_override);
}

// **The smoothness metric across changes of rate.** The constant-rate metric reads the surface's
// advance over the true one; across a change "the true one" changes by up to ten times in a frame,
// which is the key being pressed and not the sand being rough. So a run whose rate changes is read
// in the sand's own units — game seconds of surface per real second, the speed — by:
//   - **stops and stop-go**: frames the surface stood still at a rate above zero (once it has moved
//     since the rate last left zero: the start from rest, as the constant-rate metric leaves it
//     out), and of those, the ones followed within a second by a frame above twice the rate;
//   - **jolt**: the largest change of speed from one frame to the next, over what the clock allows
//     at the larger of the rates in force over the last second (a whole rate per `response_s`);
//   - **settled**: the fastest frame a second or more after a change, over the rate — a slow-down
//     comes down at the old rate's deceleration from at most `catch_up` times it, which takes
//     `catch_up * response_s` — and the worst one-second window's ratio among the windows of one
//     rate three seconds or more after a change to it: a speed-up refills the fields at the new
//     rate over a turnaround or two first.
struct ChangeMetrics {
  u32 stops = 0;
  u32 stop_go = 0;
  f64 jolt = 0.0;
  f64 max_speed_settled = 0.0;
  f64 window_settled = 0.0;
  u32 windows = 0;  // settled windows measured
};

ChangeMetrics change_metrics(const ModelRun& r) {
  ChangeMetrics m;
  const u32 n = static_cast<u32>(r.rate_of.size());
  const f64 response = TerrainClockConfig{}.response_s;
  u32 changed = 0;      // the frame the rate last changed at
  bool moving = false;  // the surface has moved since the rate last left zero
  for (u32 i = 0; i < n; ++i) {
    const f64 rate = r.rate_of[i];
    if (i > 0 && rate != r.rate_of[i - 1]) {
      changed = i;
      if (r.rate_of[i - 1] == 0.0) moving = false;
    }
    const f64 v = r.advance_of[i] / k_dt;
    if (i > 0) {
      // The clock's own rule: the acceleration of the rate, or after a change of the speed the
      // sand had then — the larger of the rates and the speeds over the last second bounds both.
      f64 larger = 0.0;
      for (u32 j = i > 60 ? i - 60 : 0; j <= i; ++j)
        larger = std::max({larger, r.rate_of[j], r.advance_of[j] / k_dt});
      const f64 allowed = larger / response * k_dt;
      const f64 dv = std::abs(v - r.advance_of[i - 1] / k_dt);
      if (allowed > 0.0) m.jolt = std::max(m.jolt, dv / allowed);
    }
    if (!(rate > 0.0)) continue;
    if (v > 0.0) moving = true;
    if (!moving) continue;
    if (v == 0.0) {
      ++m.stops;
      for (u32 j = i + 1; j < std::min(i + 61, n); ++j) {
        if (r.advance_of[j] / k_dt > 2.0 * r.rate_of[j]) {
          ++m.stop_go;
          break;
        }
      }
    }
    if (i - changed >= 60) m.max_speed_settled = std::max(m.max_speed_settled, v / rate);
    // A settled window: a second of one rate, starting three seconds after the change to it.
    if (i - changed >= 180 && i + 60 <= n && r.rate_of[i + 59] == rate) {
      bool one_rate = true;
      f64 hi = 0.0;
      f64 lo = 1.0e30;
      for (u32 j = i; j < i + 60; ++j) {
        one_rate = one_rate && r.rate_of[j] == rate;
        const f64 s = r.advance_of[j] / k_dt / rate;
        hi = std::max(hi, s);
        lo = std::min(lo, std::max(s, 0.01));
      }
      if (one_rate) {
        m.window_settled = std::max(m.window_settled, hi / lo);
        ++m.windows;
      }
    }
  }
  return m;
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
    // And read the way a run that changes its rate is read: the clock's own allowance a frame.
    CHECK(change_metrics(r).jolt <= 1.0 + 1.0e-6);
    // Two runs are the same run.
    const ModelRun again = run_model(rate, Mode::after, 1'200, k_erg);
    CHECK(again.window_ratio == r.window_ratio);
    CHECK(again.final_lag_s == r.final_lag_s);
  }
}

TEST_CASE("renderer: the rate changed while the sand moves, up and down the ladder") {
  // engine-view's `,` and `.` (apps.md): the dune ladder one rung at a time from a standing start
  // to a week a second and back down to zero, and then a day a second again, six seconds a rung —
  // through `TerrainClock::set_rate` and the re-timing `TerrainMotion::set_rate` does, on the
  // erg's three levels and one worker. (Measured when written: no stop, a jolt 1.10 of the
  // allowance at worst — the brake's last discrete steps — the settled speed within 1.50 of the
  // rate, the settled windows 1.11, and the most any sample moved in a frame 0.27 of its bound.)
  constexpr f64 k_ladder[] = {0.0,      60.0,    600.0,   3'600.0, 8'640.0, 86'400.0, 604'800.0,
                              86'400.0, 8'640.0, 3'600.0, 600.0,   60.0,    0.0,      86'400.0};
  constexpr u32 k_rung = 360;
  Vector<RateStep> schedule;
  for (u32 i = 0; i < std::size(k_ladder); ++i)
    schedule.push_back(RateStep{i * k_rung + (i > 0 ? 1u : 0u), k_ladder[i]});
  const u32 frames = static_cast<u32>(std::size(k_ladder)) * k_rung;
  const std::span<const RateStep> steps(schedule.data(), schedule.size());
  const ModelRun r = run_model(steps, Mode::after, frames, k_erg);
  const ChangeMetrics m = change_metrics(r);
  char line[320];
  std::snprintf(line, sizeof line,
                "ladder: %u changes, move/bound %.3f, stops %u (stop-go %u), jolt %.2f of the "
                "clock's allowance, settled speed <= %.2f of the rate, settled window ratio %.2f "
                "over %u windows, fields %u (late %u)",
                r.changes, r.max_move_share, m.stops, m.stop_go, m.jolt, m.max_speed_settled,
                m.window_settled, m.windows, r.fields, r.late);
  MESSAGE(std::string(line));
  CHECK(r.changes == std::size(k_ladder) - 1);
  // The blend's invariants through every change: the per-frame bound, exact handovers, one surface
  // time for every level, and never backwards — a change never steps the sand.
  CHECK(r.max_move_share <= 1.0 + 1.0e-4);
  CHECK(r.exact_ends);
  CHECK(r.monotonic);
  CHECK(r.shared);
  // Smooth through every change (change_metrics): the sand never stands while the rate runs, its
  // speed changes by about what the clock allows a frame and no more, it is within the catch-up a
  // second after a change, and three seconds after one it keeps the constant rate's window ratio.
  CHECK(m.stops == 0);
  CHECK(m.stop_go == 0);
  CHECK(m.jolt <= 1.25);
  CHECK(m.max_speed_settled <= 1.5 + 1.0e-9);
  CHECK(m.windows > 1'000);
  CHECK(m.window_settled <= 1.25);
  // At zero the sand comes to rest and stays there, and game time with it.
  for (u32 i = 12 * k_rung + 60; i < 13 * k_rung; ++i)
    CHECK(r.advance_of[i] == 0.0);
  // Two runs are the same run.
  const ModelRun again = run_model(steps, Mode::after, frames, k_erg);
  CHECK(again.final_lag_s == r.final_lag_s);
  CHECK(again.advance_of == r.advance_of);
}

TEST_CASE("renderer: keys pressed faster than the sand settles") {
  // Three presses a second, in runs: down the ladder from a week a second to zero in two seconds,
  // up it again as fast, and a week, a tenth of a day, a week, an hour, a week. A change taken
  // before the last one has settled starts from the speed the sand had reached, so it neither
  // creeps down from a week a second at an hour's deceleration nor jumps up at a week's
  // acceleration.
  constexpr f64 k_presses[] = {0.0,     60.0,      86'400.0, 604'800.0, 86'400.0, 8'640.0,
                               3'600.0, 600.0,     60.0,     0.0,       60.0,     600.0,
                               3'600.0, 8'640.0,   86'400.0, 604'800.0, 8'640.0,  604'800.0,
                               3'600.0, 604'800.0, 604'800.0};
  // A standing start, six seconds at a week a second, a second at rest, and otherwise twenty
  // frames between presses; the last rate is held for five seconds.
  const auto hold = [](u32 i, f64 rate) -> u32 {
    if (i == 0) return 240;
    if (i == 3) return 360;
    if (rate == 0.0) return 60;
    return 20;
  };
  Vector<RateStep> schedule;
  u32 at = 0;
  u32 rest_end = 0;  // the frame the second at rest ends
  for (u32 i = 0; i < std::size(k_presses); ++i) {
    schedule.push_back(RateStep{at, k_presses[i]});
    at += hold(i, k_presses[i]);
    if (i > 0 && k_presses[i] == 0.0) rest_end = at;
  }
  const u32 frames = at + 300;
  const std::span<const RateStep> steps(schedule.data(), schedule.size());
  const ModelRun r = run_model(steps, Mode::after, frames, k_erg);
  const ChangeMetrics m = change_metrics(r);
  char line[256];
  std::snprintf(line, sizeof line,
                "presses: %u changes, move/bound %.3f, stops %u (stop-go %u), jolt %.2f, settled "
                "speed <= %.2f of the rate, settled window ratio %.2f over %u windows",
                r.changes, r.max_move_share, m.stops, m.stop_go, m.jolt, m.max_speed_settled,
                m.window_settled, m.windows);
  MESSAGE(std::string(line));
  // The bound, the handovers, one surface and never backwards, whatever the keys do.
  CHECK(r.max_move_share <= 1.0 + 1.0e-4);
  CHECK(r.exact_ends);
  CHECK(r.monotonic);
  CHECK(r.shared);
  // Never a stand followed by a sprint, and never faster than the catch-up once a second has
  // passed. What it does not keep is the ladder's "never stands": a week a second reached from an
  // hour a second in one press, twenty frames after the last, leaves the sand far behind game time
  // (it ramped at an hour's acceleration while game time ran at a week's), it catches up at up to
  // half again the rate, and one worker's fields — timed for the rate, twenty times past what it
  // keeps on the cadence — run out under a catch-up, so it brakes to a stand for about half a
  // second, as it does for a stalled worker. A jolt of up to twice the allowance is the brake's
  // last discrete steps into that stand.
  CHECK(m.stop_go == 0);
  CHECK(m.max_speed_settled <= 1.5 + 1.0e-9);
  CHECK(m.jolt <= 2.0);
  // A zero among the presses brings it to rest within its second, from whatever speed the presses
  // before it had left.
  REQUIRE(rest_end > 0);
  CHECK(r.advance_of[rest_end - 2] == 0.0);
}

TEST_CASE("renderer: the surface clock across a change of rate") {
  // A change is taken at the old rate's acceleration for a latency's worth of real time. Speeding
  // up: L is the new rate's at once, and for that while the speed grows by the old rate every
  // response_s at most — the fields on their way were timed at the old rate, and a sand that
  // outran them would brake to a stand before the first one timed at the new rate arrived — and by
  // the new rate's after it. Slowing down: the speed comes down at the old rate's deceleration, not
  // the new one's — a week a second to a minute a second in under a second, not in hours — and
  // never drops to the new ceiling in one frame. Once over, the clock is the constant clock. And a
  // rate of zero brings a moving surface to rest the same way.
  TerrainClock clock;
  TerrainClockConfig config;
  config.rate = 1'000.0;
  clock.reset(config);
  clock.arrived(0, 0.5);
  CHECK(clock.latency_s() == doctest::Approx(600.0));
  // Up to speed at 1,000, the surface at game time minus L.
  f64 surface = 0.0;
  f64 game = 600.0;
  for (u32 i = 0; i < 600; ++i) {
    game += 1'000.0 * k_dt;
    const f64 to = clock.advance(k_dt, game, surface, 1.0e12);
    clock.settle(k_dt, surface, to);
    surface = to;
  }
  CHECK(clock.speed() == doctest::Approx(1'000.0).epsilon(0.01));
  const f64 carried = clock.speed();
  clock.set_rate(100'000.0);
  CHECK(clock.rate() == 100'000.0);
  CHECK(clock.latency_s() == doctest::Approx(60'000.0));  // the same turnaround at the new rate
  f64 last = clock.speed();
  const u32 ramp = static_cast<u32>(config.margin * 0.5 / k_dt);  // L's real time: 36 frames
  for (u32 i = 0; i < 180; ++i) {
    game += 100'000.0 * k_dt;
    const f64 to = clock.advance(k_dt, game, surface, 1.0e12);
    clock.settle(k_dt, surface, to);
    const f64 reference = i < ramp ? carried : 100'000.0;
    CHECK(clock.speed() - last <= reference / config.response_s * k_dt * (1.0 + 1.0e-9));
    last = clock.speed();
    surface = to;
  }
  CHECK(clock.speed() > 10'000.0);
  // Down to 100: the deceleration is the old rate's, so the speed falls smoothly from about
  // 100,000 to at most 150 within catch_up * response_s (it may have been catching up at up to
  // catch_up times the old rate) and never in one frame.
  const f64 before = clock.speed();
  clock.set_rate(100.0);
  u32 frames_down = 0;
  for (u32 i = 0; i < 120; ++i) {
    game += 100.0 * k_dt;
    const f64 to = clock.advance(k_dt, game, surface, 1.0e12);
    clock.settle(k_dt, surface, to);
    CHECK(last - clock.speed() <= before / config.response_s * k_dt * (1.0 + 1.0e-9));
    if (clock.speed() > 150.0) ++frames_down;
    last = clock.speed();
    surface = to;
  }
  CHECK(before > 10'000.0);
  CHECK(frames_down > 1);
  CHECK(frames_down <= 45);
  CHECK(clock.speed() <= 150.0 + 1.0e-9);
  // To zero: to rest at the old rate's deceleration, and then it stays.
  clock.set_rate(0.0);
  CHECK(clock.latency_s() == 0.0);
  for (u32 i = 0; i < 60; ++i) {
    const f64 to = clock.advance(k_dt, game, surface, 1.0e12);
    clock.settle(k_dt, surface, to);
    CHECK(to >= surface);
    surface = to;
  }
  CHECK(clock.speed() == 0.0);
  CHECK(clock.advance(k_dt, game + 1.0e6, surface, 1.0e12) == surface);
  // A negative rate is still.
  clock.set_rate(-5.0);
  CHECK(clock.rate() == 0.0);

  // A second change before the first has settled starts from the speed reached: from 100,000 to
  // 100 and, five frames later, to zero, the sand is at rest within catch_up * response_s of the
  // first — not creeping down at 100's deceleration for minutes.
  TerrainClock quick;
  config.rate = 100'000.0;
  quick.reset(config);
  quick.arrived(0, 0.5);
  surface = 0.0;
  game = 60'000.0;
  for (u32 i = 0; i < 300; ++i) {
    game += 100'000.0 * k_dt;
    const f64 to = quick.advance(k_dt, game, surface, 1.0e12);
    quick.settle(k_dt, surface, to);
    surface = to;
  }
  CHECK(quick.speed() > 50'000.0);
  quick.set_rate(100.0);
  u32 to_rest = 0;
  for (u32 i = 0; i < 600 && (i < 5 || quick.speed() > 0.0); ++i, ++to_rest) {
    if (i == 5) quick.set_rate(0.0);
    game += quick.rate() * k_dt;
    const f64 to = quick.advance(k_dt, game, surface, 1.0e12);
    quick.settle(k_dt, surface, to);
    surface = to;
  }
  CHECK(to_rest <= 45);
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

TEST_CASE("renderer: a storm at the game's own rate moves the sand within its bound, as a slide") {
  // A storm's transport gain (terrain.md, "A storm scales transport") at one game second a real
  // second: the waves' travel is its hours' flux — sixteen mean hours' at the peak — times the gain
  // for forty seconds of a hundred, and nothing else about the clock changes. The cadence reads
  // the gained travel (`terrain_band_travel_m` is the record's integral, which carries the gain),
  // so a storm's fields come as often as its sand moves and each pair is a slide of at most a
  // quarter of a sample; timed by a calm day's travel, a gain of 50 would cross-fade pairs a
  // sample and more apart.
  const RateStep own[] = {{0, 1.0}};
  const f64 t0 = 94'608'000.0;
  for (const f64 gain : {1.0, 10.0, 50.0}) {
    CAPTURE(gain);
    const Transport storm{t0 + 30.0, t0 + 70.0, 16.0 * gain};
    const ModelRun r = run_model(own, Mode::after, 6'000, k_erg, 0, 0, 0, &storm, 1.0);
    char line[240];
    std::snprintf(line, sizeof line,
                  "a storm at gain %.0f at the game's rate: %u fields, largest pair %.3f of a "
                  "sample, largest move %.3f of the bound, window ratio %.2f, stop-go %.3f",
                  gain, r.fields, r.max_pair_travel_share, r.max_move_share, r.window_ratio,
                  r.stop_go);
    MESSAGE(std::string(line));
    CHECK(r.max_move_share <= 1.0 + 1.0e-4);
    CHECK(r.exact_ends);
    CHECK(r.shared);
    CHECK(r.stop_go == 0.0);
    CHECK(r.window_ratio <= 1.25);
    CHECK(r.max_pair_travel_share <= 0.25 + 1.0e-6);
  }
  // What the gain would cost a cadence that read a calm day's travel, and what the old floor of a
  // minute between fields costs a gain of 50 on the inner ring.
  const Transport storm{t0 + 30.0, t0 + 70.0, 16.0 * 50.0};
  const ModelRun blind = run_model(own, Mode::after, 6'000, k_erg, 0, 0, 0, &storm, 1.0, false);
  const ModelRun minute = run_model(own, Mode::after, 6'000, k_erg, 0, 0, 0, &storm, 60.0);
  char line[200];
  std::snprintf(line, sizeof line,
                "gain 50 timed by a calm day's travel: largest pair %.2f samples; with a minute "
                "between fields at least: %.2f",
                blind.max_pair_travel_share, minute.max_pair_travel_share);
  MESSAGE(std::string(line));
  CHECK(blind.max_pair_travel_share > 0.5);
  CHECK(minute.max_pair_travel_share > 0.25);
  CHECK(blind.max_move_share <= 1.0 + 1.0e-4);  // the bound still holds; the motion is a fade
}
