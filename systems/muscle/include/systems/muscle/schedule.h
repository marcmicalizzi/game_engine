#pragma once

// The line tier's fixed schedule and its located crossing (ADR-0055; the authoring track's r010
// SCHEDULE.md and LOCATOR.md, r009 `locate`, r008 `gap` and `gap_rate`).
//
// Tick k advances [(k-1)/60, k/60] through eight parents of H = 1/480 s. Parent j starts at its
// canonical time ((k-1) + j/8)/60 with the state carried from parent j-1 (no accumulated clock).
// While `startup_pending`, a parent is four pieces of h = H/4; otherwise one piece of h = H. A piece
// is one S probe from its incoming state, read by the locator:
//
//   - the gap g = L - projected(l) - Ls at the start, both internal stages and the end;
//   - the start's phase is the sign of its gap, or at an exact zero the sign of the outgoing gap
//     rate (cached per state; at most two such queries per parent; a zero or nonfinite rate is
//     EventTangentUnresolved, a third query EventBudgetExceeded before it is evaluated);
//   - an end of the start's phase with an internal stage of the other phase is EventUnresolved, a
//     zero internal stage EventTangentUnresolved; an end of the other phase, or an exact zero, is a
//     detected crossing;
//   - a detected crossing is located by 20 dyadic bisections of tau in (0, h] on the piece's
//     original-start one-step map, the high side kept, and must end with width <= h/2^20 and
//     |g_high| <= 1e-8 m, else EventLocationUnresolved; its remainder h - tau, if positive, is four
//     equal S steps, each inspected: any crossing or zero there is EventBudgetExceeded;
//   - one crossing per parent: a later piece of the same parent runs one full probe and is
//     inspected as a remainder is, so a second crossing is refused before any search.
//
// The ceilings follow: 28 probes in a startup parent (four pieces, 20 bisections, four tails), 25
// otherwise; 224 and 200 a tick. Any failure fails the tick; the caller commits nothing.
//
// The schedule is a template over a *prober* — the probe, the gap and its rate — so the tests can
// drive it with scalar fixtures whose crossings are known, and the kernel with the law
// (LineProber below), with no indirect call either way:
//   bool validate(const State& s, Failure& f);
//   bool probe(const State& start, double d, Probe& out, Failure& f);
//   bool gap(const State& s, double& g, Failure& f);
//   bool gap_rate(const State& s, double& r, Failure& f);

#include <systems/muscle/scheme.h>

#include <core/base/assert.h>
#include <core/base/types.h>

#include <cmath>

namespace engine::muscle {

inline constexpr u32 k_parents = 8;
inline constexpr u32 k_startup_pieces = 4;
inline constexpr u32 k_bisections = 20;
inline constexpr u32 k_cells = 1u << k_bisections;
inline constexpr u32 k_remainder_steps = 4;
inline constexpr u32 k_zero_queries_per_parent = 2;
inline constexpr u32 k_startup_parent_probes = 28;
inline constexpr u32 k_parent_probes = 25;
inline constexpr double k_gap_limit = 1e-8;  // m
// H = 1/480 s and h = H/4, the pinned binary64 values (r010 METHOD.json).
inline constexpr double k_parent_duration = 0x1.1111111111111p-9;
inline constexpr double k_startup_piece = 0x1.1111111111111p-11;

// The canonical start of parent j of tick k, as the reference computes it: (k - 1 + j/8)/60.
inline double parent_start(u64 tick, u32 j) noexcept {
  return (static_cast<double>(tick - 1) + static_cast<double>(j) / 8.0) / 60.0;
}

inline int sign_of(double x) noexcept { return (x > 0.0) - (x < 0.0); }

// One parent's allowances: the crossing, the two zero-boundary queries and their cache.
struct ParentContext {
  bool event_used = false;
  u32 queries = 0;
  u32 probes = 0;
  u32 ceiling = k_parent_probes;
  State query_state[k_zero_queries_per_parent] = {};
  int query_side[k_zero_queries_per_parent] = {};
};

template <class Prober>
inline bool counted_probe(Prober& pr, ParentContext& ctx, const State& start, double d, Probe& out,
                          Failure& f) noexcept {
  ++ctx.probes;
  ENGINE_ASSERT(ctx.probes <= ctx.ceiling, "muscle: the schedule exceeded its structural probe ceiling");
  return pr.probe(start, d, out, f);
}

// The phase of a piece's start: its gap's sign, or at an exact zero the outgoing rate's.
template <class Prober>
inline bool start_phase(Prober& pr, ParentContext& ctx, const State& s, int& side, Counters& n,
                        Failure& f) noexcept {
  double g = 0.0;
  if (!pr.gap(s, g, f)) return false;
  if (g != 0.0) {
    side = sign_of(g);
    return true;
  }
  for (u32 i = 0; i < ctx.queries; ++i) {
    if (same_state(ctx.query_state[i], s)) {
      side = ctx.query_side[i];
      return true;
    }
  }
  if (ctx.queries >= k_zero_queries_per_parent)
    return fail(f, ValidationTest::EventBudgetExceeded, s.t);
  ++n.zero_boundary_queries;
  double rate = 0.0;
  if (!pr.gap_rate(s, rate, f)) return false;
  if (rate == 0.0 || !std::isfinite(rate)) return fail(f, ValidationTest::EventTangentUnresolved, rate);
  side = sign_of(rate);
  ctx.query_state[ctx.queries] = s;
  ctx.query_side[ctx.queries] = side;
  ++ctx.queries;
  return true;
}

// Reads a probe's gaps against its start's phase. `remainder` is true for a crossing's remainder
// and for a later piece of a parent whose crossing is spent: there, any crossing is the second.
template <class Prober>
inline bool inspect(Prober& pr, ParentContext& ctx, const State& start, const Probe& trial,
                    bool remainder, int& side, bool& detected, double& end_gap, Counters& n,
                    Failure& f) noexcept {
  if (!start_phase(pr, ctx, start, side, n, f)) return false;
  double internal[2] = {0.0, 0.0};
  if (!pr.gap(trial.stage[0], internal[0], f)) return false;
  if (!pr.gap(trial.stage[1], internal[1], f)) return false;
  if (!pr.gap(trial.stage[2], end_gap, f)) return false;
  if (end_gap != 0.0 && sign_of(end_gap) == side) {
    for (const double g : internal) {
      if (g != 0.0 && sign_of(g) != side)
        return fail(f, remainder ? ValidationTest::EventBudgetExceeded : ValidationTest::EventUnresolved, g);
      if (g == 0.0)
        return fail(f, remainder ? ValidationTest::EventBudgetExceeded : ValidationTest::EventTangentUnresolved, g);
    }
  }
  detected = end_gap == 0.0 || sign_of(end_gap) != side;
  if (remainder && detected) return fail(f, ValidationTest::EventBudgetExceeded, end_gap);
  return true;
}

// One scheduled piece of duration h from `s`, which it advances. `accept(probe)` books each
// retained segment (the probe or the located crossing's high side, then the remainder's steps).
template <class Prober, class Accept>
inline bool run_piece(Prober& pr, ParentContext& ctx, State& s, double h, Accept&& accept,
                      Counters& n, Failure& f) noexcept {
  Probe full{};
  if (!counted_probe(pr, ctx, s, h, full, f)) return false;
  int side = 0;
  bool detected = false;
  double end_gap = 0.0;
  if (!inspect(pr, ctx, s, full, ctx.event_used, side, detected, end_gap, n, f)) return false;
  if (!detected) {
    accept(full);
    s = full.stage[2];
    return true;
  }
  // The parent's crossing: bisect tau on the piece's original-start endpoint map. The low side is
  // never advanced; the high side's probe is kept.
  Probe high = full;
  u32 lo = 0;
  u32 hi = k_cells;
  if (end_gap == 0.0) {
    lo = hi;
  } else {
    for (u32 it = 0; it < k_bisections; ++it) {
      const u32 mid = (lo + hi) / 2;
      Probe candidate{};
      ++n.bisection_probes;
      if (!counted_probe(pr, ctx, s, h * static_cast<double>(mid) / static_cast<double>(k_cells),
                         candidate, f))
        return false;
      int candidate_side = 0;
      bool candidate_detected = false;
      double g = 0.0;
      if (!inspect(pr, ctx, s, candidate, false, candidate_side, candidate_detected, g, n, f))
        return false;
      if (g == 0.0) {
        lo = hi = mid;
        high = candidate;
        break;
      }
      if (sign_of(g) == side) {
        lo = mid;
      } else {
        hi = mid;
        high = candidate;
      }
    }
  }
  const double cells = static_cast<double>(k_cells);
  const double tau = h * static_cast<double>(hi) / cells;
  double g_high = 0.0;
  if (!pr.gap(high.stage[2], g_high, f)) return false;
  const double width = h * static_cast<double>(hi - lo) / cells;
  if (width > h / cells) return fail(f, ValidationTest::EventLocationUnresolved, width);
  if (std::fabs(g_high) > k_gap_limit) return fail(f, ValidationTest::EventLocationUnresolved, g_high);
  ++n.events;
  ctx.event_used = true;
  accept(high);
  s = high.stage[2];
  const double remaining = h - tau;
  if (remaining != 0.0) {
    const double dt = remaining / static_cast<double>(k_remainder_steps);
    const double event_time = high.stage[2].t;
    for (u32 k = 0; k < k_remainder_steps; ++k) {
      const State start{event_time + static_cast<double>(k) * dt, s.a, s.l};
      Probe tail{};
      if (!counted_probe(pr, ctx, start, dt, tail, f)) return false;
      int tail_side = 0;
      bool tail_detected = false;
      double tail_gap = 0.0;
      if (!inspect(pr, ctx, start, tail, true, tail_side, tail_detected, tail_gap, n, f)) return false;
      accept(tail);
      s = tail.stage[2];
    }
  }
  return true;
}

// What one parent leaves behind: its canonical end time, the state and the cache.
struct ParentSample {
  double t;
  double a;
  double l;
  Cache cache;
};

// One tick's trial: the schedule from the committed (a, l), the work it books in stage order, the
// last segment's cache, and the end state. Nothing is written anywhere the caller commits from
// unless this returns true.
struct TickTrial {
  double work[k_work_channels];
  Cache cache;
  State end;
};

template <class Prober, class OnParent>
inline bool run_tick(Prober& pr, u64 tick, bool startup, double a, double l, TickTrial& out,
                     Counters& n, Failure& f, OnParent&& on_parent) noexcept {
  for (double& w : out.work)
    w = 0.0;
  State s{parent_start(tick, 0), a, l};
  if (!pr.validate(s, f)) return false;
  auto accept = [&](const Probe& probe) noexcept {
    for (u32 i = 0; i < 3; ++i)
      for (u32 c = 0; c < k_work_channels; ++c)
        out.work[c] += probe.terms[i][c];
    out.cache = probe.cache;
  };
  const u32 pieces = startup ? k_startup_pieces : 1u;
  const double h = startup ? k_startup_piece : k_parent_duration;
  for (u32 j = 0; j < k_parents; ++j) {
    s.t = parent_start(tick, j);
    ParentContext ctx{};
    ctx.ceiling = startup ? k_startup_parent_probes : k_parent_probes;
    for (u32 k = 0; k < pieces; ++k) {
      if (!run_piece(pr, ctx, s, h, accept, n, f)) return false;
    }
    on_parent(j, s, out.cache);
  }
  out.end = s;
  return true;
}

// The law's prober: the S probe, the gap and its rate for one actuator on one path in one tick.
template <class Path>
struct LineProber {
  const LawRow& law;
  const Path& path;
  double tick_start;
  double excitation;
  Counters& n;

  // stepping.validate_state: a committed state must admit the continuous model's velocity.
  bool validate(const State& s, Failure& f) noexcept {
    if (!std::isfinite(s.t)) return fail(f, ValidationTest::TimeNotFinite, s.t);
    if (!bounded(s.a, 0.0, 1.0)) return fail(f, ValidationTest::ActivationOutOfRange, s.a);
    Geometry g{};
    if (!geometry(law, s.l, g, f)) return false;
    PathSample p{};
    if (!sample_path(path, s.t, tick_start, p, f)) return false;
    double v = 0.0;
    return equilibrium_velocity(law, s.a, s.l, p.length, v, n, f);
  }

  bool probe(const State& start, double d, Probe& out, Failure& f) noexcept {
    return probe_s(law, path, tick_start, excitation, start, d, out, n, f);
  }

  // arm_a.gap: L - projected(l) - Ls.
  bool gap(const State& s, double& out, Failure& f) noexcept {
    PathSample p{};
    if (!sample_path(path, s.t, tick_start, p, f)) return false;
    Geometry g{};
    if (!geometry(law, s.l, g, f)) return false;
    out = p.length - g.projected - law.slack;
    return true;
  }

  // arm_a.gap_rate: dL/dt - v_eq / cos, only ever asked at a zero boundary.
  bool gap_rate(const State& s, double& out, Failure& f) noexcept {
    PathSample p{};
    if (!sample_path(path, s.t, tick_start, p, f)) return false;
    double v = 0.0;
    if (!equilibrium_velocity(law, s.a, s.l, p.length, v, n, f)) return false;
    Geometry g{};
    if (!geometry(law, s.l, g, f)) return false;
    out = p.rate - v / g.cosine;
    return true;
  }
};

}  // namespace engine::muscle
