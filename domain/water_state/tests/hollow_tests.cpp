// A source fills a hollow to its lip and spills, and the lake stands at the level critical flow
// over the lip gives (roadmap W184; the geometry and the bounds are W176's specification, section 8
// of docs/experiments/water-state-grounding.md once that lands, and docs/subsystems/water_state.md,
// "The hollow"). `run_hollow` is the case's whole run and returns everything it measured, so a
// later case (W176's tie to the steady solve) adds its comparison beside these without running the
// water again or changing this file's cases.
#include <domain/water_state/water_state.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

using namespace engine;
using water_state::Config;
using water_state::Edge;
using water_state::Inputs;
using water_state::Solver;

namespace {

constexpr f64 k_g = 9.80665;
constexpr f64 k_n = 0.01;            // Manning's n everywhere, s m^-1/3
constexpr f64 k_floor = -0.5;        // the hollow's floor; the sill is at 0
constexpr f64 k_crest_width = 2.0;   // b
constexpr f64 k_crest_length = 0.5;  // L_c
constexpr f64 k_slope = 0.1;         // the outlet channel's
constexpr f64 k_lake_area = 64.0;    // m^2, the hollow's vertical-sided 8 x 8 m
constexpr f64 k_settle_s = 900.0;    // world time, more than the slowest 0.1 % settling (567 s)

// The case at W176's own cell is 2.4e9 cell-steps, some twenty minutes in a debug build and about a
// minute in an optimised one, so it runs where the build optimises (the release suites, the gate's
// Linux release container) and is skipped in a debug build; the case at twice the cell runs in
// both.
#if defined(NDEBUG)
constexpr bool k_optimised = true;
#else
constexpr bool k_optimised = false;
#endif

/// The crest's closed form for a discharge: critical depth y_c = (q^2 / g)^(1/3), q = Q / b; the
/// still water's head over the crest E_c = 1.5 y_c; and the most friction can add over the crest's
/// length at the critical section's slope, L_c n^2 V_c^2 / y_c^(4/3).
struct Crest {
  f64 yc = 0.0;
  f64 ec = 0.0;
  f64 friction_max = 0.0;
  f64 bound = 0.0;  // max(0.05 E_c, 1 mm)
  f64 tau_s = 0.0;  // the lake's linear time constant at the end, A E_c / (1.5 Q)
};

Crest crest_for(f64 q_total) {
  Crest c;
  const f64 q = q_total / k_crest_width;
  c.yc = std::cbrt(q * q / k_g);
  const f64 vc = q / c.yc;
  c.ec = 1.5 * c.yc;
  c.friction_max = k_crest_length * k_n * k_n * vc * vc / std::pow(c.yc, 4.0 / 3.0);
  c.bound = std::max(0.05 * c.ec, 0.001);
  c.tau_s = k_lake_area * c.ec / (1.5 * q_total);
  return c;
}

enum class Start { below, above };  // the plan's level (2 cm under the sill), or 2 E_c over it

struct HollowSpec {
  f64 discharge_m3_s = 0.1;
  f64 cell_m = 0.125;
  Start start = Start::below;
};

struct HollowRun {
  f64 level = 0.0;          // mean over x 2..6, y 2..6 over the last 10 s, m over the sill
  f64 level_range = 0.0;    // that mean's range over the last 10 s
  f64 level_change = 0.0;   // the 10 s mean now less the 10 s mean one tau earlier
  f64 outflow_m3_s = 0.0;   // through the free edge over the last 10 s
  f64 mass_balance = 0.0;   // source - (stored now - stored at start) - out, m^3
  f64 water_through = 0.0;  // the source's whole volume, the balance's scale
  f64 crest_depth = 0.0;    // mean over the crest's last column of cells, at the end
  f64 crest_energy = 0.0;   // the same column's mean specific energy h + |u|^2 / 2g
  u64 steps = 0;
};

HollowRun run_hollow(const HollowSpec& spec) {
  const Crest crest = crest_for(spec.discharge_m3_s);
  const f64 dx = spec.cell_m;
  Config config;
  config.nx = static_cast<u32>(13.5 / dx + 0.5);
  config.ny = static_cast<u32>(8.0 / dx + 0.5);
  config.cell_m = dx;
  config.gravity = k_g;
  config.edges.east = Edge::open;  // the free edge at x = 13.5
  Solver solver(config);
  const u32 cells = solver.cell_count();
  std::vector<f64> bed(cells), manning(cells, k_n), source(cells, 0.0);
  const f64 start_level = spec.start == Start::below ? -0.02 : 2.0 * crest.ec;
  const f64 inflow_area = 8.0;  // x 0..1, y 0..8
  for (u32 y = 0; y < config.ny; ++y) {
    for (u32 x = 0; x < config.nx; ++x) {
      const f64 cx = (x + 0.5) * dx;
      const f64 cy = (y + 0.5) * dx;
      const bool in_outlet = cy > 3.0 && cy < 5.0;
      f64 z = 10.0;  // wall: never wet
      if (cx < 8.0)
        z = k_floor;
      else if (cx < 8.5 && in_outlet)
        z = 0.0;
      else if (in_outlet)
        z = -k_slope * (cx - 8.5);
      const u32 i = solver.index(x, y);
      bed[i] = z;
      if (cx < 8.0) solver.depth()[i] = start_level - z;
      if (cx < 1.0) source[i] = spec.discharge_m3_s / inflow_area;
    }
  }
  Inputs in;
  in.bed_m = bed;
  in.manning = manning;
  in.source_m_per_s = source;
  const f64 stored0 = solver.surface_volume_m3();

  const auto lake_level = [&]() {
    f64 sum = 0.0;
    u32 count = 0;
    for (u32 y = 0; y < config.ny; ++y) {
      for (u32 x = 0; x < config.nx; ++x) {
        const f64 cx = (x + 0.5) * dx;
        const f64 cy = (y + 0.5) * dx;
        if (cx > 2.0 && cx < 6.0 && cy > 2.0 && cy < 6.0) {
          const u32 i = solver.index(x, y);
          sum += solver.depth()[i] + bed[i];
          ++count;
        }
      }
    }
    return sum / count;
  };

  // Up to the last tau + 20 s in one go, then one second at a time, the lake's level sampled.
  const u32 tail_s = static_cast<u32>(crest.tau_s) + 21;
  solver.advance(in, k_settle_s - tail_s);
  std::vector<f64> samples;
  f64 out_before_last10 = 0.0;
  for (u32 s = 0; s < tail_s; ++s) {
    if (s == tail_s - 10) out_before_last10 = solver.ledger().open_edge_out_m3;
    solver.advance(in, 1.0);
    samples.push_back(lake_level());
  }
  const auto mean_of = [&](usize end) {
    f64 m = 0.0;
    for (usize i = end - 10; i < end; ++i)
      m += samples[i];
    return m / 10.0;
  };
  HollowRun run;
  run.level = mean_of(samples.size());
  f64 lo = samples.back();
  f64 hi = samples.back();
  for (usize i = samples.size() - 10; i < samples.size(); ++i) {
    lo = std::min(lo, samples[i]);
    hi = std::max(hi, samples[i]);
  }
  run.level_range = hi - lo;
  run.level_change = run.level - mean_of(samples.size() - static_cast<usize>(crest.tau_s));
  run.outflow_m3_s = (solver.ledger().open_edge_out_m3 - out_before_last10) / 10.0;
  const auto& ledger = solver.ledger();
  run.water_through = ledger.source_m3;
  run.mass_balance = ledger.source_m3 - (solver.surface_volume_m3() - stored0) -
                     ledger.open_edge_out_m3 + ledger.roundoff_m3;
  u32 crest_cells = 0;
  for (u32 y = 0; y < config.ny; ++y) {
    for (u32 x = 0; x < config.nx; ++x) {
      const f64 cx = (x + 0.5) * dx;
      const f64 cy = (y + 0.5) * dx;
      if (cx > 8.5 - dx && cx < 8.5 && cy > 3.0 && cy < 5.0) {
        const u32 i = solver.index(x, y);
        const f64 h = solver.depth()[i];
        const f64 u = solver.momentum_x()[i] / h;
        const f64 v = solver.momentum_y()[i] / h;
        run.crest_depth += h;
        run.crest_energy += h + (u * u + v * v) / (2.0 * k_g);
        ++crest_cells;
      }
    }
  }
  run.crest_depth /= crest_cells;
  run.crest_energy /= crest_cells;
  run.steps = ledger.steps;
  return run;
}

// What the case asserts for one discharge at one cell size: W176's items 3 to 5 (settled, the
// outflow, the count, both starts), and critical flow on the lip: the crest's last column stands
// at the critical section's specific energy E_c within max(0.05 E_c, 1 mm), and the lake at least
// that high. **The lake's level over the sill is recorded, not held to E_c plus the crest's
// friction**: the first-order hydrostatic reconstruction loses head at the hollow's upstream step
// (0.5 m under 0.13 m of water) that the closed form does not (docs/subsystems/water_state.md, "The
// hollow"; the one-dimensional case below separates it from the entrance's contraction), so that
// part of W176's specification fails today and is reported, by how much, in the case's message.
void check_hollow(f64 discharge, const HollowRun& below, const HollowRun& above,
                  bool assert_level) {
  const Crest c = crest_for(discharge);
  const f64 plan_level = -0.02;
  MESSAGE("hollow Q " << discharge << ": E_c " << c.ec << " m (+ at most " << c.friction_max
                      << " m friction), bound " << c.bound << "; crest's last column h "
                      << below.crest_depth << " (y_c " << c.yc << "), energy " << below.crest_energy
                      << "; lake level from below " << below.level << " ("
                      << (below.level - c.ec) / c.ec * 100.0 << " % over E_c)"
                      << ", from above " << above.level << "; range " << below.level_range
                      << ", change over tau " << below.level_change << "; outflow "
                      << below.outflow_m3_s << "; mass " << below.mass_balance << " of "
                      << below.water_through << "; steps " << below.steps
                      << "; the plan's body level " << plan_level << " is "
                      << below.level - plan_level << " m under it");
  for (const HollowRun* run : {&below, &above}) {
    if (assert_level) {
      CHECK(std::fabs(run->crest_energy - c.ec) <= c.bound);
      CHECK(run->level >= c.ec - c.bound);
    }
    CHECK(std::fabs(run->level_change) <= 0.1 * c.bound);
    CHECK(run->level_range <= c.bound);
    CHECK(std::fabs(run->outflow_m3_s - discharge) <= 0.005 * discharge);
    CHECK(std::fabs(run->mass_balance) <= 1e-12 * run->water_through);
  }
  CHECK(std::fabs(below.level - above.level) <= c.bound);
}

}  // namespace

TEST_CASE("water_state: a hollow fills to its lip and spills at critical depth over it" *
          doctest::skip(!k_optimised)) {
  // W176's geometry at its own cell, 0.125 m (the crest four cells long, so the critical section
  // lies on the flat crest): an 8 x 8 m hollow 0.5 m deep, a crest 2 m wide and 0.5 m long, a 5 m
  // channel at a slope of 0.1 to a free edge; Manning's n 0.01; Q of 0.1 and 0.02 m^3/s spread over
  // the hollow's first metre; from the plan's level and from 2 E_c over the sill; 900 s each, the
  // four runs on threads of their own (each its own solver: the bytes do not depend on it). Held:
  // critical flow on the lip (check_hollow), settled, passing its inflow, its water counted, both
  // starts, and the two discharges' crest energies in the ratio 5^(2/3) within the two bounds.
  HollowRun b1, a1, b2, a2;
  {
    std::thread t1([&] { b1 = run_hollow({0.1, 0.125, Start::below}); });
    std::thread t2([&] { a1 = run_hollow({0.1, 0.125, Start::above}); });
    std::thread t3([&] { b2 = run_hollow({0.02, 0.125, Start::below}); });
    std::thread t4([&] { a2 = run_hollow({0.02, 0.125, Start::above}); });
    t1.join();
    t2.join();
    t3.join();
    t4.join();
  }
  check_hollow(0.1, b1, a1, true);
  check_hollow(0.02, b2, a2, true);
  const Crest c1 = crest_for(0.1);
  const Crest c2 = crest_for(0.02);
  // The ratio is taken from the crest's energies, which stand where the law puts them; the lakes'
  // levels are each wrong by the step's loss and their ratio (2.73) would pass for the wrong
  // reason.
  const f64 ratio = b1.crest_energy / b2.crest_energy;
  const f64 law = std::cbrt(25.0);  // 5^(2/3)
  MESSAGE("hollow: crest energies' ratio " << ratio << " against " << law << "; the lakes' "
                                           << b1.level / b2.level);
  CHECK(std::fabs(ratio - law) <= law * (c1.bound / c1.ec + c2.bound / c2.ec));
}

TEST_CASE("water_state: a hollow at twice the cell is recorded beside it") {
  // The same at 0.25 m, the crest two cells long: W176's specification asserts the level only at
  // L_c / 4 and records this; settling, the outflow and the count are still held.
  HollowRun b1, a1;
  {
    std::thread t1([&] { b1 = run_hollow({0.1, 0.25, Start::below}); });
    std::thread t2([&] { a1 = run_hollow({0.1, 0.25, Start::above}); });
    t1.join();
    t2.join();
  }
  check_hollow(0.1, b1, a1, false);
}

namespace {

struct Crest1d {
  f64 level = 0.0;   // the lake's mean over x 2..6, m over the sill
  f64 lake_h = 0.0;  // the lake's last cell before the wall: its depth and speed
  f64 lake_u = 0.0;
  f64 entry_h = 0.0;  // the crest's first cell, past the wall
  f64 entry_u = 0.0;
  f64 lip_h = 0.0;       // the crest's last cell
  f64 lip_energy = 0.0;  // its specific energy
};

// The hollow's section along x alone, one cell wide, at W176's cell and the unit discharge of Q =
// 0.1 over its 2 m crest (q = 0.05 m^2/s), so no flow contracts sideways into the crest; 300 s,
// over twenty of this lake's time constants.
constexpr f64 k_q_1d = 0.05;
constexpr f64 k_dx_1d = 0.125;

Crest1d run_crest_1d() {
  Config config;
  config.nx = static_cast<u32>(13.5 / k_dx_1d + 0.5);
  config.ny = 1;
  config.cell_m = k_dx_1d;
  config.gravity = k_g;
  config.edges.east = Edge::open;
  Solver solver(config);
  std::vector<f64> bed(config.nx), manning(config.nx, k_n), source(config.nx, 0.0);
  u32 wall = 0;
  u32 lip = 0;
  for (u32 x = 0; x < config.nx; ++x) {
    const f64 cx = (x + 0.5) * k_dx_1d;
    bed[x] = cx < 8.0 ? k_floor : (cx < 8.5 ? 0.0 : -k_slope * (cx - 8.5));
    if (cx < 8.0) solver.depth()[x] = -0.02 - bed[x];
    if (cx < 1.0) source[x] = k_q_1d / 1.0;  // q per metre of width over the first metre
    if (cx < 8.0) wall = x;
    if (cx > 8.0 && cx < 8.5) lip = x;
  }
  Inputs in;
  in.bed_m = bed;
  in.manning = manning;
  in.source_m_per_s = source;
  solver.advance(in, 300.0);
  Crest1d r;
  u32 count = 0;
  for (u32 x = 0; x < config.nx; ++x) {
    const f64 cx = (x + 0.5) * k_dx_1d;
    if (cx > 2.0 && cx < 6.0) {
      r.level += solver.depth()[x] + bed[x];
      ++count;
    }
  }
  r.level /= count;
  const auto at = [&](u32 x, f64& h, f64& u) {
    h = solver.depth()[x];
    u = solver.momentum_x()[x] / h;
  };
  at(wall, r.lake_h, r.lake_u);
  at(wall + 1, r.entry_h, r.entry_u);
  f64 lip_u = 0.0;
  at(lip, r.lip_h, lip_u);
  r.lip_energy = r.lip_h + lip_u * lip_u / (2.0 * k_g);
  return r;
}

}  // namespace

TEST_CASE("water_state: a hollow's crest in one dimension") {
  // It separates what the two-dimensional case's level owes to the wall from what it owes to the
  // entrance's contraction, and pins the wall's loss against its derivation (docs/subsystems/
  // water_state.md, "The hollow"): the reconstruction hands the face the lake's speed on a depth
  // dz shallower, so its left state carries q H / h_L of the discharge, and in a steady state the
  // central-upwind mass flux's numerical viscosity makes up the rest, which takes a depth
  // difference across the face of H - h_1 = q dz / (h_L |a-|), a- = u_L - sqrt(g H). Held within 25
  // %: the derivation drops the terms in a-/a+ and the crest's own friction.
  const Crest1d r = run_crest_1d();
  const Crest c = crest_for(k_q_1d * k_crest_width);
  const f64 dz = -k_floor;
  const f64 head = r.level;  // the face's left depth is the lake's level over the sill
  const f64 a_minus = r.lake_u - std::sqrt(k_g * head);
  const f64 predicted = k_q_1d * dz / (r.lake_h * std::fabs(a_minus));
  const f64 measured = head - r.entry_h;
  MESSAGE("crest in 1-D: level " << r.level << " m over the sill against E_c " << c.ec
                                 << "; the lip's cell h " << r.lip_h << " (y_c " << c.yc
                                 << "), its specific energy " << r.lip_energy
                                 << "; across the wall H - h_1 " << measured << " m against q dz / "
                                 << "(h_L |a-|) " << predicted << " m");
  // Critical flow on the lip holds here too; the lake's excess is the wall's.
  CHECK(std::fabs(r.lip_energy - c.ec) <= c.bound);
  CHECK(std::fabs(r.lip_h - c.yc) <= c.bound);
  CHECK(std::fabs(measured - predicted) <= 0.25 * predicted);
}

TEST_CASE(
    "water_state: a hollow's lake stands at E_c plus the crest's friction over a vertical "
    "wall" *
    doctest::should_fail()) {
  // W176's bound on the lake's level, |level - (E_c + friction)| <= max(0.05 E_c, 1 mm), held on
  // the one-dimensional run. **It is expected to fail**: the reference stands 35 % of E_c too high
  // over the wall (the case above says why). If this case ever passes, doctest reports the suite
  // failed: that is the day a scheme holds moving water over a bed step, and this decoration, the
  // page's "What does not" and the two-dimensional case's assertions are to be revisited with it.
  const Crest1d r = run_crest_1d();
  const Crest c = crest_for(k_q_1d * k_crest_width);
  CHECK(std::fabs(r.level - (c.ec + c.friction_max)) <= c.bound);
}
