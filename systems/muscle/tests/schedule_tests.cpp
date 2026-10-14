// The fixed schedule and the located crossing (schedule.h) on fixtures whose answers are known:
// the r010 declaration's preflight groups, expressed as provers the schedule drives directly.
#include <systems/muscle/schedule.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>

using namespace engine;
using namespace engine::muscle;

namespace {

constexpr double H = k_parent_duration;
constexpr double h4 = k_startup_piece;

// A probe that moves only time: the stages at t + c_i d, (a, l) carried, no work. Every probe's
// (start, duration) is logged so two runs can be compared.
struct Log {
  u32 probes = 0;
  double start[256] = {};
  double duration[256] = {};
};

inline void fill_probe(const State& s, double d, Probe& out, double a, double l) {
  for (u32 i = 0; i < 3; ++i)
    out.stage[i] = State{s.t + k_tableau_c[i] * d, a, l};
  for (auto& row : out.terms)
    for (double& w : row)
      w = 0.0;
  out.cache = Cache{};
}

// The gap is a function of time, g(t), with its rate g'(t).
template <class G, class R>
struct TimeProber {
  G g;
  R rate;
  Log log{};
  double a = 0.0;
  double l = 0.1;

  bool validate(const State&, Failure&) { return true; }
  bool probe(const State& s, double d, Probe& out, Failure&) {
    if (log.probes < 256) {
      log.start[log.probes] = s.t;
      log.duration[log.probes] = d;
    }
    ++log.probes;
    fill_probe(s, d, out, a, l);
    return true;
  }
  bool gap(const State& s, double& out, Failure&) {
    out = g(s.t);
    return true;
  }
  bool gap_rate(const State& s, double& out, Failure&) {
    out = rate(s.t);
    return true;
  }
};

template <class G, class R>
TimeProber<G, R> time_prober(G g, R r) {
  return TimeProber<G, R>{g, r};
}

struct Run {
  bool ok = false;
  Failure failure{};
  Counters n{};
  TickTrial trial{};
  u32 parent_probes[k_parents] = {};
};

template <class Prober>
Run run(Prober& pr, bool startup, u64 tick = 1) {
  Run r;
  u32 before = 0;
  r.ok = run_tick(pr, tick, startup, 0.0, 0.1, r.trial, r.n, r.failure,
                  [&](u32 j, const State&, const Cache&) {
                    r.parent_probes[j] = pr.log.probes - before;
                    before = pr.log.probes;
                  });
  return r;
}

// The gap is scripted per probe: a probe tags its stage states with its own index (in `a`) and
// the stage (in `l`), so the gap of any state the locator reads is a lookup. This is how a test
// places a second crossing at an exact remainder position or later piece, which no function of
// time could do without the first probe seeing it too.
struct ScriptProber {
  Log log{};
  // The gap of probe p's end and of its internal stages; the start of the tick has `initial`.
  double initial = -1.0;
  double (*end)(u32 p) = nullptr;
  double (*internal)(u32 p, u32 i) = nullptr;

  bool validate(const State&, Failure&) { return true; }
  bool probe(const State& s, double d, Probe& out, Failure&) {
    const u32 p = log.probes;
    if (p < 256) {
      log.start[p] = s.t;
      log.duration[p] = d;
    }
    ++log.probes;
    for (u32 i = 0; i < 3; ++i)
      out.stage[i] = State{s.t + k_tableau_c[i] * d, static_cast<double>(p), static_cast<double>(i)};
    for (auto& row : out.terms)
      for (double& w : row)
        w = 0.0;
    out.cache = Cache{};
    return true;
  }
  bool gap(const State& s, double& out, Failure&) {
    if (s.a < 0.0) {
      out = initial;
      return true;
    }
    const u32 p = static_cast<u32>(s.a);
    const u32 i = static_cast<u32>(s.l);
    out = i == 2 ? end(p) : internal(p, i);
    return true;
  }
  bool gap_rate(const State&, double& out, Failure&) {
    out = 1.0;
    return true;
  }
};

template <class Prober>
Run run_script(Prober& pr, bool startup) {
  Run r;
  u32 before = 0;
  r.ok = run_tick(pr, 1, startup, -1.0, 0.0, r.trial, r.n, r.failure,
                  [&](u32 j, const State&, const Cache&) {
                    r.parent_probes[j] = pr.log.probes - before;
                    before = pr.log.probes;
                  });
  return r;
}

}  // namespace

TEST_CASE("muscle schedule: the pinned durations and canonical parent times") {
  CHECK(H == 1.0 / 480.0);
  CHECK(h4 == H / 4.0);
  CHECK(parent_start(1, 0) == 0.0);
  CHECK(parent_start(1, 1) == H);  // (0 + 1/8)/60 rounds to the same double as 1/480
  CHECK(parent_start(7, 3) == (6.0 + 0.375) / 60.0);
}

TEST_CASE("muscle schedule: 32 probes on a startup tick, 8 after, whatever the probe returns") {
  // A constant positive gap: no crossing anywhere. Two payloads (the probe's activation and
  // length) must not change the schedule: it is fixed, not adaptive.
  auto one = time_prober([](double) { return 0.01; }, [](double) { return 0.0; });
  auto two = time_prober([](double) { return 0.01; }, [](double) { return 0.0; });
  one.a = 0.25;
  one.l = 0.09;
  two.a = 0.75;
  two.l = 0.12;
  const Run startup = run(one, true);
  const Run startup_two = run(two, true);
  REQUIRE(startup.ok);
  REQUIRE(startup_two.ok);
  CHECK(one.log.probes == 32);
  CHECK(two.log.probes == 32);
  for (u32 p = 0; p < 32; ++p) {
    CHECK(one.log.start[p] == two.log.start[p]);
    CHECK(one.log.duration[p] == h4);
  }
  // The pieces carry their own propagated time; a parent restarts at its canonical time.
  CHECK(one.log.start[1] == h4);
  CHECK(one.log.start[4] == parent_start(1, 1));
  auto later = time_prober([](double) { return 0.01; }, [](double) { return 0.0; });
  const Run normal = run(later, false, 2);
  REQUIRE(normal.ok);
  CHECK(later.log.probes == 8);
  for (u32 j = 0; j < 8; ++j) {
    CHECK(later.log.start[j] == parent_start(2, j));
    CHECK(later.log.duration[j] == H);
  }
  CHECK(normal.n.events == 0);
}

TEST_CASE("muscle locator: an exact endpoint zero is one crossing, and its boundary one query") {
  // g = t - H is exactly zero at parent 0's end, which is parent 1's canonical start.
  auto pr = time_prober([](double t) { return t - H; }, [](double) { return 1.0; });
  const Run r = run(pr, false);
  REQUIRE(r.ok);
  CHECK(r.n.events == 1);
  CHECK(r.n.zero_boundary_queries == 1);
  CHECK(r.n.bisection_probes == 0);  // an exact zero collapses the bracket
  CHECK(pr.log.probes == 8);
}

TEST_CASE("muscle locator: same-phase ends with an opposite internal stage are unresolved") {
  const double t1 = k_tableau_c[0] * H;  // stage 1's time in parent 0
  auto dip = time_prober([t1](double t) { return (t - t1) * (t - t1) - 1e-12; },
                         [](double) { return 1.0; });
  Run r = run(dip, false);
  CHECK_FALSE(r.ok);
  CHECK(r.failure.test == ValidationTest::EventUnresolved);
  auto touch = time_prober([t1](double t) { return (t - t1) * (t - t1); }, [](double) { return 1.0; });
  r = run(touch, false);
  CHECK_FALSE(r.ok);
  CHECK(r.failure.test == ValidationTest::EventTangentUnresolved);
}

TEST_CASE("muscle locator: g = t^2 starts at a zero with a zero rate, which is a tangent") {
  auto pr = time_prober([](double t) { return t * t; }, [](double t) { return 2.0 * t; });
  const Run r = run(pr, false);
  CHECK_FALSE(r.ok);
  CHECK(r.failure.test == ValidationTest::EventTangentUnresolved);
  CHECK(pr.log.probes == 1);
}

TEST_CASE("muscle locator: the gap bound refuses a fast crossing the width bound accepts") {
  // At 1 m/s the located high side is within h/2^20 * 1 m/s = 2e-9 m of the root: accepted.
  auto slow = time_prober([](double t) { return t - H / 3.0; }, [](double) { return 1.0; });
  Run r = run(slow, false);
  REQUIRE(r.ok);
  CHECK(r.n.events == 1);
  CHECK(r.n.bisection_probes == 20);
  CHECK(r.parent_probes[0] == 25);  // 1 + 20 bisections + 4 remainder steps
  CHECK(slow.log.probes == 25 + 7);
  // The remainder is four equal steps from the event's time.
  const double tau = H * 349526.0 / 1048576.0;  // ceil(2^20 / 3) cells
  const double dt = (H - tau) / 4.0;
  CHECK(slow.log.duration[21] == dt);
  CHECK(slow.log.start[22] == slow.log.start[21] + dt);
  // At 100 m/s the same bracket leaves 1.3e-7 m: past 1e-8, refused.
  auto fast = time_prober([](double t) { return 100.0 * (t - H / 3.0); }, [](double) { return 100.0; });
  r = run(fast, false);
  CHECK_FALSE(r.ok);
  CHECK(r.failure.test == ValidationTest::EventLocationUnresolved);
  CHECK(std::fabs(r.failure.value) > 1e-8);
}

TEST_CASE("muscle locator: a non-dyadic crossing in startup piece 2 costs exactly 28 probes") {
  auto pr = time_prober([](double t) { return t - (h4 + h4 / 3.0); }, [](double) { return 1.0; });
  const Run r = run(pr, true);
  REQUIRE(r.ok);
  CHECK(r.n.events == 1);
  CHECK(r.parent_probes[0] == k_startup_parent_probes);  // 1 + (1 + 20 + 4) + 1 + 1
  for (u32 j = 1; j < k_parents; ++j)
    CHECK(r.parent_probes[j] == 4);
  CHECK(pr.log.probes == 28 + 7 * 4);
}

namespace {

// The first crossing: probe 0 ends on the other side; every bisection candidate (probes 1..20)
// ends 1e-9 m past the switch, so the bracket closes to the first cell and the high side is
// within the gap bound. Later probes stay on the new side unless the test moves one back.
u32 g_second = 0;  // the probe that crosses back
double end_gap(u32 p) {
  if (p == 0) return 0.5;
  if (p <= 20) return 1e-9;
  return p == g_second ? -0.5 : 0.5;
}
double internal_gap(u32 p, u32) { return p == 0 ? -0.5 : (p <= 20 ? -0.5 : 0.5); }

}  // namespace

TEST_CASE("muscle locator: a second crossing in any remainder step is refused, never searched") {
  for (u32 k = 0; k < k_remainder_steps; ++k) {
    INFO("remainder step " << k);
    g_second = 21 + k;
    ScriptProber pr{};
    pr.end = end_gap;
    pr.internal = internal_gap;
    const Run r = run_script(pr, false);
    CHECK_FALSE(r.ok);
    CHECK(r.failure.test == ValidationTest::EventBudgetExceeded);
    CHECK(pr.log.probes == 22 + k);   // the full probe, 20 bisections, k + 1 remainder steps
    CHECK(r.n.bisection_probes == 20);  // no second search
    CHECK(r.n.events == 1);
  }
}

TEST_CASE("muscle locator: a second crossing in a later startup piece is refused, never searched") {
  // Piece 0 locates the parent's crossing (probes 0..24); pieces 1..3 are probes 25..27.
  for (u32 q = 1; q < k_startup_pieces; ++q) {
    INFO("piece " << q);
    g_second = 24 + q;
    ScriptProber pr{};
    pr.end = end_gap;
    pr.internal = internal_gap;
    const Run r = run_script(pr, true);
    CHECK_FALSE(r.ok);
    CHECK(r.failure.test == ValidationTest::EventBudgetExceeded);
    CHECK(pr.log.probes == 25 + q);
    CHECK(r.n.bisection_probes == 20);
  }
  // With no second crossing the startup parent costs its ceiling less the three bisections it
  // never needed: 25 + 3 = 28.
  g_second = 0;
  ScriptProber pr{};
  pr.end = end_gap;
  pr.internal = internal_gap;
  Run r{};
  u32 before = 0;
  r.ok = run_tick(pr, 1, true, -1.0, 0.0, r.trial, r.n, r.failure,
                  [&](u32 j, const State&, const Cache&) {
                    if (j == 0) r.parent_probes[0] = pr.log.probes - before;
                  });
  // Parent 1 starts again from probe 27's end, on the new side: no further crossing.
  REQUIRE(r.ok);
  CHECK(r.parent_probes[0] == 28);
}
