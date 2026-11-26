// The water_state reference's cases (docs/subsystems/water_state.md, "The cases"). Each case's
// bound is stated beside it with the reason for it; a closed form's formulas are the source's,
// cited at the helper that evaluates them.
#include <core/jobs/job_system.h>
#include <domain/water_state/water_state.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

using namespace engine;
using water_state::Config;
using water_state::Edge;
using water_state::Inputs;
using water_state::Solver;

namespace {

constexpr f64 k_g = 9.81;

// ---- the dam breaks' closed forms -------------------------------------------------------------
//
// Delestre et al., SWASHES (arXiv 1110.0288), section 4.1.1 (Stoker's solution, a wet bed) and
// section 4.1.2 (Ritter's, a dry one), with their parameters: 10 m, the dam at 5 m, h_l 5 mm,
// h_r 1 mm or 0, 6 s, no friction.

constexpr f64 k_dam_length = 10.0;
constexpr f64 k_dam_x0 = 5.0;
constexpr f64 k_dam_hl = 0.005;
constexpr f64 k_dam_time = 6.0;

// c_m = sqrt(g h_m) solves -8 g h_r c^2 (sqrt(g h_l) - c)^2 + (c^2 - g h_r)^2 (c^2 + g h_r) = 0
// between sqrt(g h_r) and sqrt(g h_l) (SWASHES 4.1.1). Bisection, to the last bit it can move.
f64 stoker_cm(f64 hl, f64 hr) {
  const f64 sl = std::sqrt(k_g * hl);
  auto f = [&](f64 c) {
    const f64 a = sl - c;
    const f64 b = c * c - k_g * hr;
    return -8.0 * k_g * hr * c * c * a * a + b * b * (c * c + k_g * hr);
  };
  f64 lo = std::sqrt(k_g * hr);
  f64 hi = sl;
  const f64 flo = f(lo);
  for (int i = 0; i < 200; ++i) {
    const f64 mid = 0.5 * (lo + hi);
    if ((f(mid) > 0.0) == (flo > 0.0))
      lo = mid;
    else
      hi = mid;
  }
  return 0.5 * (lo + hi);
}

// The depth of the dam break at (x, t): Stoker's for hr > 0, Ritter's for hr == 0 (SWASHES 4.1.1,
// 4.1.2).
f64 dam_depth(f64 x, f64 t, f64 hl, f64 hr) {
  const f64 sl = std::sqrt(k_g * hl);
  const f64 xa = k_dam_x0 - t * sl;
  if (x <= xa) return hl;
  const f64 fan =
      4.0 / (9.0 * k_g) * (sl - (x - k_dam_x0) / (2.0 * t)) * (sl - (x - k_dam_x0) / (2.0 * t));
  if (hr == 0.0) {
    const f64 xb = k_dam_x0 + 2.0 * t * sl;
    return x <= xb ? fan : 0.0;
  }
  const f64 cm = stoker_cm(hl, hr);
  const f64 xb = k_dam_x0 + t * (2.0 * sl - 3.0 * cm);
  const f64 xc = k_dam_x0 + t * 2.0 * cm * cm * (sl - cm) / (cm * cm - k_g * hr);
  if (x <= xb) return fan;
  if (x <= xc) return cm * cm / k_g;
  return hr;
}

struct DamResult {
  f64 l1_relative = 0.0;  // sum |h - exact| dx over sum exact dx
  f64 min_depth = 0.0;
  f64 volume_error = 0.0;  // relative
};

DamResult run_dam(u32 nx, f64 hr) {
  Config config;
  config.nx = nx;
  config.ny = 1;
  config.cell_m = k_dam_length / nx;
  config.gravity = k_g;
  Solver solver(config);
  std::vector<f64> bed(nx, 0.0);
  auto h = solver.depth();
  for (u32 x = 0; x < nx; ++x)
    h[x] = (x + 0.5) * config.cell_m <= k_dam_x0 ? k_dam_hl : hr;
  const f64 before = solver.surface_volume_m3();
  Inputs in;
  in.bed_m = bed;
  solver.advance(in, k_dam_time);
  DamResult r;
  f64 err = 0.0;
  f64 norm = 0.0;
  r.min_depth = h[0];
  for (u32 x = 0; x < nx; ++x) {
    const f64 exact = dam_depth((x + 0.5) * config.cell_m, k_dam_time, k_dam_hl, hr);
    err += std::fabs(h[x] - exact);
    norm += exact;
    r.min_depth = h[x] < r.min_depth ? h[x] : r.min_depth;
  }
  r.l1_relative = err / norm;
  r.volume_error = std::fabs(solver.surface_volume_m3() - before) / before;
  return r;
}

// ---- small helpers ----------------------------------------------------------------------------

// An integer hash, so a bed is the same input on every toolchain (no sin or cos making it).
u32 mix(u32 x, u32 y) {
  u32 h = x * 0x9e3779b1u ^ (y + 0x7f4a7c15u) * 0x85ebca77u;
  h ^= h >> 15;
  h *= 0x2c1b3c6du;
  h ^= h >> 12;
  return h;
}

f64 min_of(std::span<const f64> s) {
  f64 m = s[0];
  for (f64 v : s)
    m = v < m ? v : m;
  return m;
}

f64 max_abs_of(std::span<const f64> s) {
  f64 m = 0.0;
  for (f64 v : s)
    m = std::fabs(v) > m ? std::fabs(v) : m;
  return m;
}

}  // namespace

TEST_CASE("water_state: a lake at rest over a bed with dry islands stays exactly at rest") {
  // Every bed height and the surface are multiples of 1/64 under 2, so every sum and difference
  // the reconstruction takes is exact: the two sides of every face are the same state to the bit,
  // the flux is that state's exact flux, and its pressure cancels the correction to the bit. The
  // bound is therefore zero: not one bit of depth or momentum may move in 600 steps.
  Config config;
  config.nx = 64;
  config.ny = 48;
  config.cell_m = 0.5;
  config.carried = 2;
  Solver solver(config);
  std::vector<f64> bed(solver.cell_count());
  constexpr f64 eta = 1.0;
  u32 dry = 0;
  for (u32 y = 0; y < config.ny; ++y) {
    for (u32 x = 0; x < config.nx; ++x) {
      f64 z = static_cast<f64>((x * 7 + y * 13) % 17) / 64.0;
      const bool island = (x / 8 + y / 8) % 3 == 0 && (x % 8) > 1 && (y % 8) > 2;
      if (island) z = 1.0 + static_cast<f64>(mix(x, y) % 16) / 64.0;  // at or above the surface
      bed[solver.index(x, y)] = z;
      const f64 h = eta - z;
      solver.depth()[solver.index(x, y)] = h > 0.0 ? h : 0.0;
      if (!(h > 0.0)) ++dry;
    }
  }
  REQUIRE(dry > 100);
  std::vector<f64> before(solver.depth().begin(), solver.depth().end());
  Inputs in;
  in.bed_m = bed;
  const u64 steps = solver.advance(in, 20.0);
  CHECK(steps > 500);
  u32 moved = 0;
  for (u32 i = 0; i < solver.cell_count(); ++i) {
    if (solver.depth()[i] != before[i] || solver.momentum_x()[i] != 0.0 ||
        solver.momentum_y()[i] != 0.0)
      ++moved;
  }
  CHECK(moved == 0u);
  CHECK(solver.ledger().roundoff_m3 == 0.0);
}

TEST_CASE("water_state: a lake at rest over an inexact bed stays at rest to round-off") {
  // The same lake over a bed of heights no binary fraction holds: h + z then rounds, the two sides
  // of a face differ by an ulp of the surface (about 1.1e-16 m at 0.9 m), and that ulp is the only
  // force. Bound: the surface stays within 1e-13 m of where it started and no speed passes
  // 1e-11 m/s after 600 steps, a thousand times the ulp it starts from and far below anything a
  // case of moving water reads.
  Config config;
  config.nx = 64;
  config.ny = 48;
  config.cell_m = 0.5;
  Solver solver(config);
  std::vector<f64> bed(solver.cell_count());
  constexpr f64 eta = 0.9;
  for (u32 y = 0; y < config.ny; ++y) {
    for (u32 x = 0; x < config.nx; ++x) {
      f64 z = 0.731 * static_cast<f64>(mix(x, y) % 1001) / 1001.0;
      const bool island = (x / 8 + y / 8) % 3 == 0 && (x % 8) > 1 && (y % 8) > 2;
      if (island) z = 0.9 + 0.37 * static_cast<f64>(mix(y, x) % 101) / 101.0;
      bed[solver.index(x, y)] = z;
      const f64 h = eta - z;
      solver.depth()[solver.index(x, y)] = h > 0.0 ? h : 0.0;
    }
  }
  Inputs in;
  in.bed_m = bed;
  solver.advance(in, 20.0);
  f64 surface_drift = 0.0;
  f64 fastest = 0.0;
  for (u32 i = 0; i < solver.cell_count(); ++i) {
    const f64 h = solver.depth()[i];
    if (h > 0.0) {
      const f64 d = std::fabs(h + bed[i] - eta);
      surface_drift = d > surface_drift ? d : surface_drift;
    }
    fastest = solver.speed()[i] > fastest ? solver.speed()[i] : fastest;
  }
  MESSAGE("inexact lake: surface drift " << surface_drift << " m, fastest " << fastest << " m/s");
  CHECK(surface_drift <= 1e-13);
  CHECK(fastest <= 1e-11);
}

TEST_CASE("water_state: a dam breaks on a wet bed as Stoker's solution, first order") {
  // SWASHES 4.1.1's case at 200, 400 and 800 cells. A first-order scheme converges at order 1
  // where the solution is smooth and at order 1/2 to 1 at its shock and its fan's corners, so the
  // L1 order measured between grids is held to at least 0.5; the error on the finest grid is held
  // to the error measured there and 15 % more, so a change that loses accuracy fails here and a
  // second- order form has a number to beat. Volume is held to 1e-12 of itself: every face's flux
  // leaves one cell and enters the other, each rounding once, over a few thousand steps (the
  // count's case says why that is the bound).
  const DamResult r200 = run_dam(200, 0.001);
  const DamResult r400 = run_dam(400, 0.001);
  const DamResult r800 = run_dam(800, 0.001);
  const f64 order_a = std::log2(r200.l1_relative / r400.l1_relative);
  const f64 order_b = std::log2(r400.l1_relative / r800.l1_relative);
  MESSAGE("Stoker L1: " << r200.l1_relative << ", " << r400.l1_relative << ", " << r800.l1_relative
                        << "; orders " << order_a << ", " << order_b);
  CHECK(order_a >= 0.5);
  CHECK(order_b >= 0.5);
  CHECK(r800.l1_relative <= 0.0045);  // measured 0.00394 on MSVC (2026-11-25)
  CHECK(r800.min_depth >= 0.0);
  CHECK(r800.volume_error <= 1e-12);
}

TEST_CASE("water_state: a dam breaks on a dry bed as Ritter's solution, first order") {
  // SWASHES 4.1.2's case. The front over a dry bed is where a first-order scheme is weakest (the
  // depth there goes to zero as the fan's square and the dry threshold cuts the tip), so the same
  // order bound holds and the finest grid's error is held to its own measured error and 15 % more.
  const DamResult r200 = run_dam(200, 0.0);
  const DamResult r400 = run_dam(400, 0.0);
  const DamResult r800 = run_dam(800, 0.0);
  const f64 order_a = std::log2(r200.l1_relative / r400.l1_relative);
  const f64 order_b = std::log2(r400.l1_relative / r800.l1_relative);
  MESSAGE("Ritter L1: " << r200.l1_relative << ", " << r400.l1_relative << ", " << r800.l1_relative
                        << "; orders " << order_a << ", " << order_b);
  CHECK(order_a >= 0.5);
  CHECK(order_b >= 0.5);
  CHECK(r800.l1_relative <= 0.0058);  // measured 0.00502 on MSVC (2026-11-25)
  CHECK(r800.min_depth >= 0.0);
  CHECK(r800.volume_error <= 1e-12);
}

namespace {

// ---- the rough slope: water released down it, with every input the step reads -----------------
//
// 48 x 40 cells of 0.5 m; the bed falls 2 % to the east, with bumps; a block of 0.3 m on the west
// quarter; a spring and a drain; Manning n 0.03; an open east edge; on the east half a sandy ground
// (Rawls et al. 1993 via the grounding page's table: K 236 mm/h, psi 49.5 mm; a deficit of 0.35
// chosen) whose store holds 10 mm. With `carried`, two quantities: a foam made by the dissipation
// and decaying in 4 s, and a sediment the water starts with at 0.5 a cubic metre, settling
// everywhere and lifted in one patch.

struct Slope {
  Config config;
  std::optional<Solver> solver;
  std::vector<f64> bed, manning, source, ground_k, ground_s, ground_cap, exchange;
  Inputs in;
  f64 initial_water = 0.0;
  std::array<f64, 2> initial_carried{};
};

constexpr f64 k_sand_k = 0.236 / 3600.0;  // m/s
constexpr f64 k_sand_s = 0.0495 * 0.35;   // psi times the deficit, m
constexpr f64 k_sand_capacity = 0.010;    // m

void build_slope(Slope& s, bool carried, u32 rows_per_job = 16) {
  s.config.nx = 48;
  s.config.ny = 40;
  s.config.cell_m = 0.5;
  s.config.edges.east = Edge::open;
  s.config.rows_per_job = rows_per_job;
  if (carried) {
    s.config.carried = 2;
    s.config.carried_config[0].per_joule = 1e-3;
    s.config.carried_config[0].decay_s = 4.0;
    s.config.carried_config[0].min_speed_m_per_s = 0.05;
  }
  s.solver.emplace(s.config);
  Solver& solver = *s.solver;
  const u32 cells = solver.cell_count();
  s.bed.assign(cells, 0.0);
  s.manning.assign(cells, 0.03);
  s.source.assign(cells, 0.0);
  s.ground_k.assign(cells, 0.0);
  s.ground_s.assign(cells, 0.0);
  s.ground_cap.assign(cells, 0.0);
  s.exchange.assign(cells, -2e-5);
  for (u32 y = 0; y < s.config.ny; ++y) {
    for (u32 x = 0; x < s.config.nx; ++x) {
      const u32 i = solver.index(x, y);
      const f64 bump = 0.08 * static_cast<f64>(mix(x / 3, y / 3) % 97) / 97.0;
      s.bed[i] = 0.6 - 0.02 * static_cast<f64>(x) * s.config.cell_m + bump;
      if (x < 12) solver.depth()[i] = 0.3;
      if (x >= 20 && x < 24 && y >= 6 && y < 10) s.source[i] = 2e-3;    // a spring
      if (x >= 30 && x < 34 && y >= 28 && y < 32) s.source[i] = -5e-3;  // a drain
      if (x >= 24) {
        s.ground_k[i] = k_sand_k;
        s.ground_s[i] = k_sand_s;
        s.ground_cap[i] = k_sand_capacity;
      }
      if (x >= 14 && x < 18 && y >= 14 && y < 26) s.exchange[i] = 1e-4;  // lifted here
      if (carried) solver.carried(1)[i] = 0.5 * solver.depth()[i];
    }
  }
  s.in.bed_m = s.bed;
  s.in.manning = s.manning;
  s.in.source_m_per_s = s.source;
  s.in.ground_conductivity_m_per_s = s.ground_k;
  s.in.ground_suction_m = s.ground_s;
  s.in.ground_capacity_m = s.ground_cap;
  if (carried) s.in.carried_exchange[1] = s.exchange;
  s.initial_water = solver.surface_volume_m3();
  if (carried) {
    s.initial_carried[0] = solver.carried_total(0);
    s.initial_carried[1] = solver.carried_total(1);
  }
}

}  // namespace

TEST_CASE(
    "water_state: water is counted to the last cell through wetting, drying, the ground and an "
    "open edge") {
  // surface + ground + out - initial - sources - roundoff is zero in exact arithmetic; in double
  // each of the run's few hundred thousand additions rounds by half an ulp of what it adds, so the
  // bound is 1e-12 of the water involved, and the clamp's round-off is held separately.
  Slope s;
  build_slope(s, false);
  Solver& solver = *s.solver;
  const u32 cells = solver.cell_count();
  std::vector<bool> was_wet(cells);
  for (u32 i = 0; i < cells; ++i)
    was_wet[i] = solver.depth()[i] > 0.0;
  u32 wetted = 0;
  f64 lowest = 0.0;
  for (int piece = 0; piece < 6; ++piece) {
    solver.advance(s.in, 10.0);
    lowest = std::min(lowest, min_of(solver.depth()));
  }
  for (u32 i = 0; i < cells; ++i)
    if (!was_wet[i] && solver.depth()[i] > 0.0) ++wetted;
  u32 drained = 0;
  for (u32 y = 28; y < 32; ++y)
    for (u32 x = 30; x < 34; ++x)
      drained += solver.depth()[solver.index(x, y)] == 0.0 ? 1u : 0u;
  u32 full = 0;
  f64 over = 0.0;
  for (u32 i = 0; i < cells; ++i) {
    if (s.ground_cap[i] > 0.0 && solver.ground()[i] >= k_sand_capacity * (1.0 - 1e-15)) ++full;
    over = std::max(over, solver.ground()[i] - s.ground_cap[i]);
  }
  const auto& ledger = solver.ledger();
  const f64 balance = solver.surface_volume_m3() + solver.ground_volume_m3() +
                      ledger.open_edge_out_m3 - s.initial_water - ledger.source_m3 -
                      ledger.roundoff_m3;
  MESSAGE("count: initial " << s.initial_water << " m3, source " << ledger.source_m3 << ", ground "
                            << ledger.ground_m3 << ", out " << ledger.open_edge_out_m3
                            << ", roundoff " << ledger.roundoff_m3 << ", balance " << balance
                            << ", wetted " << wetted << ", full stores " << full << ", steps "
                            << ledger.steps);
  CHECK(lowest >= 0.0);
  CHECK(wetted > 200u);
  CHECK(drained > 0u);
  CHECK(full > 0u);
  CHECK(over <= 1e-17);  // a store never holds more than its capacity, past a rounding
  CHECK(ledger.ground_m3 > 0.0);
  CHECK(std::fabs(ledger.ground_m3 - solver.ground_volume_m3()) <= 1e-12 * ledger.ground_m3);
  CHECK(ledger.open_edge_out_m3 > 0.0);
  CHECK(std::fabs(balance) <= 1e-12 * (s.initial_water + std::fabs(ledger.source_m3)));
  CHECK(ledger.roundoff_m3 <= 1e-15 * s.initial_water);
}

TEST_CASE("water_state: the ground takes ponded water at Green and Ampt's rate until it is full") {
  // A pond of 1 m over loam (Rawls et al. 1993 via the grounding page: K 13.2 mm/h, psi 88.9 mm; a
  // deficit of 0.3 chosen). Green and Ampt's cumulative F = K t + psi dtheta ln(1 + F / psi dtheta)
  // (the grounding page, section 3) is solved here by Newton; the step integrates the rate
  // implicitly, first order in a step of about 0.07 s, so after an hour the two agree to well under
  // a part in a thousand, which is the bound. A second column with a store of 20 mm stops at it.
  Config config;
  config.nx = 4;
  config.ny = 2;
  config.cell_m = 1.0;
  Solver solver(config);
  const u32 cells = solver.cell_count();
  constexpr f64 k = 0.0132 / 3600.0;
  constexpr f64 s = 0.0889 * 0.3;
  std::vector<f64> bed(cells, 0.0), gk(cells, k), gs(cells, s), cap(cells, 10.0);
  for (u32 x = 0; x < 4; ++x)
    cap[solver.index(x, 1)] = 0.020;
  for (u32 i = 0; i < cells; ++i)
    solver.depth()[i] = 1.0;
  Inputs in;
  in.bed_m = bed;
  in.ground_conductivity_m_per_s = gk;
  in.ground_suction_m = gs;
  in.ground_capacity_m = cap;
  constexpr f64 t = 3600.0;
  solver.advance(in, t);
  f64 f = k * t + s;
  for (int i = 0; i < 100; ++i) {
    const f64 r = f - k * t - s * std::log(1.0 + f / s);
    const f64 d = 1.0 - s / (s + f);
    f -= r / d;
  }
  const f64 taken = solver.ground()[solver.index(0, 0)];
  MESSAGE("Green and Ampt after an hour: " << taken << " m against " << f << " m; full store "
                                           << solver.ground()[solver.index(0, 1)]);
  CHECK(std::fabs(taken - f) <= 1e-3 * f);
  CHECK(std::fabs(solver.ground()[solver.index(0, 1)] - 0.020) <= 1e-17);
  // The water the full stores did not take stayed in the pond, which levels across the columns.
  CHECK(std::fabs(solver.surface_volume_m3() + solver.ground_volume_m3() - 8.0) <= 1e-12 * 8.0);
}

TEST_CASE("water_state: a raised bed keeps each cell's water and a rougher bed is read at once") {
  // A lake at rest over a bed of binary fractions with dry islands (0.5 m surface); after 2 s the
  // bed under a block of it is raised 0.125 m, and the friction over another patch doubles (a
  // rippled bed is rougher). The step keeps every depth, so the volume is unchanged to the bit at
  // the change, and the block's surface stands 0.125 m proud. The wave it makes runs out against
  // the friction and the scheme's own dissipation; after 400 s the lake is at rest again at the
  // level its volume gives over the new bed. Bounds: the volume to 1e-12 of itself (the count's
  // reason), the surface's range over wet cells and its gap to the level the volume gives under a
  // micrometre, no speed over a micrometre a second (measured on MSVC: 4e-14 m/s, the range and
  // the gap under 1e-6 of that).
  Config config;
  config.nx = 48;
  config.ny = 32;
  config.cell_m = 0.5;
  Solver solver(config);
  const u32 cells = solver.cell_count();
  std::vector<f64> bed(cells), manning(cells, 0.04);
  constexpr f64 eta = 0.5;
  for (u32 y = 0; y < config.ny; ++y) {
    for (u32 x = 0; x < config.nx; ++x) {
      f64 z = static_cast<f64>((x * 7 + y * 13) % 17) / 64.0;
      if ((x / 8 + y / 8) % 3 == 0 && (x % 8) > 1 && (y % 8) > 2) z = 0.75;
      const u32 i = solver.index(x, y);
      bed[i] = z;
      solver.depth()[i] = eta - z > 0.0 ? eta - z : 0.0;
    }
  }
  Inputs in;
  in.bed_m = bed;
  in.manning = manning;
  solver.advance(in, 2.0);
  const f64 volume = solver.surface_volume_m3();
  for (u32 y = 4; y < 14; ++y)
    for (u32 x = 26; x < 40; ++x)
      bed[solver.index(x, y)] += 0.125;
  for (u32 y = 18; y < 30; ++y)
    for (u32 x = 4; x < 20; ++x)
      manning[solver.index(x, y)] = 0.08;
  CHECK(solver.surface_volume_m3() == volume);  // nothing moved but the bed
  f64 peak = 0.0;
  for (int piece = 0; piece < 40; ++piece) {
    solver.advance(in, 10.0);
    if (piece == 0) peak = max_abs_of(solver.speed());
  }
  // The level the volume gives over the new bed, by bisection.
  f64 lo = 0.0;
  f64 hi = 2.0;
  for (int i = 0; i < 200; ++i) {
    const f64 mid = 0.5 * (lo + hi);
    f64 v = 0.0;
    for (u32 c = 0; c < cells; ++c)
      v += mid > bed[c] ? mid - bed[c] : 0.0;
    (v * solver.cell_area_m2() < volume ? lo : hi) = mid;
  }
  const f64 level = 0.5 * (lo + hi);
  f64 top = -1e9;
  f64 bottom = 1e9;
  for (u32 c = 0; c < cells; ++c) {
    if (solver.depth()[c] > 1e-3) {
      top = std::max(top, solver.depth()[c] + bed[c]);
      bottom = std::min(bottom, solver.depth()[c] + bed[c]);
    }
  }
  const f64 fastest = max_abs_of(solver.speed());
  MESSAGE("changed bed: volume change " << solver.surface_volume_m3() - volume << " m3, level "
                                        << level << ", surface " << bottom << " to " << top
                                        << ", fastest " << fastest << " m/s (peak " << peak << ")");
  CHECK(peak > 0.01);  // the raise made a wave
  CHECK(std::fabs(solver.surface_volume_m3() - volume) <= 1e-12 * volume);
  CHECK(top - bottom <= 1e-6);
  CHECK(std::fabs(0.5 * (top + bottom) - level) <= 1e-6);
  CHECK(fastest <= 1e-6);
}

TEST_CASE(
    "water_state: two carried quantities are counted and the step does not know which is which") {
  // On the rough slope, a foam made by the dissipation and decaying, and a sediment settling and
  // lifted. Each one's account closes to 1e-12 of what passed through it (the count's reason). Then
  // the same run with the two quantities' places and settings swapped: every byte of each quantity,
  // and of the water, is the same, so nothing in the step depends on which quantity is which.
  Slope a;
  build_slope(a, true);
  a.solver->advance(a.in, 30.0);
  const auto& la = a.solver->ledger();
  for (u32 k = 0; k < 2; ++k) {
    const auto& c = la.carried[k];
    const f64 held = a.solver->carried_total(k);
    const f64 balance = held + c.open_edge_out + c.decayed - a.initial_carried[k] - c.source -
                        c.from_dissipation - c.exchange;
    const f64 scale = a.initial_carried[k] + c.from_dissipation + std::fabs(c.exchange);
    MESSAGE("carried " << k << ": held " << held << ", made " << c.from_dissipation << ", exchange "
                       << c.exchange << ", decayed " << c.decayed << ", out " << c.open_edge_out
                       << ", balance " << balance);
    CHECK(std::fabs(balance) <= 1e-12 * scale);
  }
  CHECK(la.carried[0].from_dissipation > 0.0);
  CHECK(la.carried[0].decayed > 0.0);
  CHECK(la.carried[1].exchange < 0.0);
  CHECK(min_of(a.solver->carried(0)) >= 0.0);
  CHECK(min_of(a.solver->carried(1)) >= 0.0);

  Slope b;
  build_slope(b, true);
  // Swap: quantity 0 is the sediment, 1 the foam.
  b.solver.reset();
  std::swap(b.config.carried_config[0], b.config.carried_config[1]);
  b.solver.emplace(b.config);
  for (u32 i = 0; i < b.solver->cell_count(); ++i) {
    b.solver->depth()[i] = i % b.config.nx < 12 ? 0.3 : 0.0;
    b.solver->carried(0)[i] = 0.5 * b.solver->depth()[i];
  }
  b.in.carried_exchange[1] = {};
  b.in.carried_exchange[0] = b.exchange;
  b.solver->advance(b.in, 30.0);
  bool same_water = true;
  bool same_carried = true;
  for (u32 i = 0; i < b.solver->cell_count(); ++i) {
    same_water = same_water && a.solver->depth()[i] == b.solver->depth()[i] &&
                 a.solver->momentum_x()[i] == b.solver->momentum_x()[i];
    same_carried = same_carried && a.solver->carried(0)[i] == b.solver->carried(1)[i] &&
                   a.solver->carried(1)[i] == b.solver->carried(0)[i];
  }
  CHECK(same_water);
  CHECK(same_carried);
}

TEST_CASE("water_state: a carried concentration makes no new extremum") {
  // A concentration between 0.2 and 0.8 laid over a lake that a dam's break sets moving over a
  // bumpy bed. Each cell's new amount is its old less what leaves by the cell's own concentration
  // plus what enters by its neighbours', with the water's own update the same weights, so the new
  // concentration is a convex combination of old ones when the depth after the outflow alone is
  // non-negative, which the step's limit gives. Bound: inside [0.2, 0.8] to 1e-12 of it in every
  // cell holding more than a micrometre (a cell nearer dry divides round-off by its depth).
  Config config;
  config.nx = 60;
  config.ny = 30;
  config.cell_m = 0.25;
  config.carried = 1;
  Solver solver(config);
  const u32 cells = solver.cell_count();
  std::vector<f64> bed(cells);
  for (u32 y = 0; y < config.ny; ++y) {
    for (u32 x = 0; x < config.nx; ++x) {
      const u32 i = solver.index(x, y);
      bed[i] = 0.05 * static_cast<f64>(mix(x / 2, y / 2) % 13) / 13.0;
      solver.depth()[i] = (x < 20 ? 0.6 : 0.2) - bed[i];
      const f64 c = 0.2 + 0.6 * static_cast<f64>(mix(y, x) % 1000) / 999.0;
      solver.carried(0)[i] = c * solver.depth()[i];
    }
  }
  Inputs in;
  in.bed_m = bed;
  f64 lowest = 1.0;
  f64 highest = 0.0;
  for (int piece = 0; piece < 10; ++piece) {
    solver.advance(in, 2.0);
    for (u32 i = 0; i < cells; ++i) {
      const f64 h = solver.depth()[i];
      if (h <= 1e-6) continue;
      const f64 c = solver.carried(0)[i] / h;
      lowest = std::min(lowest, c);
      highest = std::max(highest, c);
    }
  }
  MESSAGE("concentration stays in " << lowest << " to " << highest);
  CHECK(lowest >= 0.2 * (1.0 - 1e-12));
  CHECK(highest <= 0.8 * (1.0 + 1e-12));
}

TEST_CASE("water_state: a bore's dissipation is the jump's head loss times the flux through it") {
  // Stoker's dam break (SWASHES 4.1.1) at 800 cells. Behind the bore the depth is h_m = c_m^2 / g;
  // ahead it is h_r at rest, and the bore moves at x_C'(t), so the water passing through it is h_r
  // x_C' per metre of width and the energy it loses is that times rho g (h_m - h_r)^3 / (4 h_m
  // h_r), Belanger's head loss (the grounding page, section 4). The step's loss between 4 s and 6 s
  // is held to that within 25 % (the rarefaction's numerical dissipation, which falls with the
  // cell, is in the step's and not in the closed form; measured 9 % over on MSVC), the dissipation
  // the step gives out at 6 s (its positive part) within 50 % (measured 27 % over), and four fifths
  // of it within 0.5 m of the bore (measured 93 %).
  constexpr u32 nx = 800;
  constexpr f64 hr = 0.001;
  Config config;
  config.nx = nx;
  config.ny = 1;
  config.cell_m = k_dam_length / nx;
  config.gravity = k_g;
  Solver solver(config);
  std::vector<f64> bed(nx, 0.0);
  for (u32 x = 0; x < nx; ++x)
    solver.depth()[x] = (x + 0.5) * config.cell_m <= k_dam_x0 ? k_dam_hl : hr;
  Inputs in;
  in.bed_m = bed;
  solver.advance(in, 4.0);
  const f64 lost4 = solver.ledger().scheme_loss_j;
  solver.advance(in, 2.0);
  const f64 rate = (solver.ledger().scheme_loss_j - lost4) / 2.0;
  const f64 sl = std::sqrt(k_g * k_dam_hl);
  const f64 cm = stoker_cm(k_dam_hl, hr);
  const f64 hm = cm * cm / k_g;
  const f64 bore_speed = 2.0 * cm * cm * (sl - cm) / (cm * cm - k_g * hr);
  const f64 xc = k_dam_x0 + 6.0 * bore_speed;
  const f64 expected = 1000.0 * k_g * hr * bore_speed * (hm - hr) * (hm - hr) * (hm - hr) /
                       (4.0 * hm * hr) * config.cell_m;
  f64 total = 0.0;
  f64 near = 0.0;
  for (u32 x = 0; x < nx; ++x) {
    const f64 p = solver.dissipation()[x] * solver.cell_area_m2();
    total += p;
    if (std::fabs((x + 0.5) * config.cell_m - xc) <= 0.5) near += p;
  }
  MESSAGE("bore: step's loss " << rate << " W, closed form " << expected << " W, output total "
                               << total << " W, near the bore " << near / total);
  CHECK(std::fabs(rate - expected) <= 0.25 * expected);
  CHECK(std::fabs(total - expected) <= 0.5 * expected);
  CHECK(near >= 0.8 * total);
}

TEST_CASE("water_state: a bowl's water sloshes as Thacker's planar surface") {
  // SWASHES 4.2.2: a paraboloid z = -h0 (1 - r^2 / a^2), a = 1 m, h0 = 0.1 m, eta = 0.5, a 4 m
  // square; the surface stays a plane, tilting round as the water's centre circles at the speed
  // eta omega, omega = sqrt(2 g h0) / a. After one period at cells of 0.08 and 0.04 m the depth's
  // relative L1 error and the water's mean velocity are held: the error falls between the two (a
  // first-order scheme's moving shoreline converges at about order 1/2 to 1; measured 0.84) and the
  // finest is held to its measured value and 15 %. A first-order scheme damps the slosh: after one
  // period at 4 cm the water's mean velocity is off the closed form's by 28 % of its speed
  // (measured on MSVC), held to that and 15 %; a second-order form has these numbers to beat.
  struct Bowl {
    f64 l1 = 0.0;
    f64 velocity_error = 0.0;
  };
  constexpr f64 a = 1.0;
  constexpr f64 h0 = 0.1;
  constexpr f64 eta = 0.5;
  constexpr f64 side = 4.0;
  const f64 omega = std::sqrt(2.0 * k_g * h0) / a;
  const f64 period = 2.0 * 3.14159265358979323846 / omega;
  const auto exact = [&](f64 x, f64 y, f64 t) {
    const f64 dx = x - side / 2.0;
    const f64 dy = y - side / 2.0;
    const f64 z = -h0 * (1.0 - (dx * dx + dy * dy) / (a * a));
    const f64 h = eta * h0 / (a * a) *
                      (2.0 * dx * std::cos(omega * t) + 2.0 * dy * std::sin(omega * t) - eta) -
                  z;
    return h > 0.0 ? h : 0.0;
  };
  const auto run = [&](u32 n) {
    Config config;
    config.nx = n;
    config.ny = n;
    config.cell_m = side / n;
    config.gravity = k_g;
    Solver solver(config);
    std::vector<f64> bed(solver.cell_count());
    for (u32 y = 0; y < n; ++y) {
      for (u32 x = 0; x < n; ++x) {
        const f64 px = (x + 0.5) * config.cell_m;
        const f64 py = (y + 0.5) * config.cell_m;
        const f64 dx = px - side / 2.0;
        const f64 dy = py - side / 2.0;
        const u32 i = solver.index(x, y);
        bed[i] = -h0 * (1.0 - (dx * dx + dy * dy) / (a * a));
        solver.depth()[i] = exact(px, py, 0.0);
        solver.momentum_y()[i] = solver.depth()[i] * eta * omega;
      }
    }
    Inputs in;
    in.bed_m = bed;
    solver.advance(in, period);
    f64 err = 0.0;
    f64 norm = 0.0;
    f64 mass = 0.0;
    f64 mu = 0.0;
    f64 mv = 0.0;
    for (u32 y = 0; y < n; ++y) {
      for (u32 x = 0; x < n; ++x) {
        const u32 i = solver.index(x, y);
        const f64 e = exact((x + 0.5) * config.cell_m, (y + 0.5) * config.cell_m, period);
        err += std::fabs(solver.depth()[i] - e);
        norm += e;
        mass += solver.depth()[i];
        mu += solver.momentum_x()[i];
        mv += solver.momentum_y()[i];
      }
    }
    const f64 u = mu / mass;
    const f64 v = mv / mass;
    // The closed form's velocity at one period: (0, eta omega).
    const f64 du = u;
    const f64 dv = v - eta * omega;
    return Bowl{err / norm, std::sqrt(du * du + dv * dv) / (eta * omega)};
  };
  const Bowl coarse = run(50);
  const Bowl fine = run(100);
  MESSAGE("bowl: L1 " << coarse.l1 << ", " << fine.l1 << "; mean velocity off by "
                      << coarse.velocity_error << ", " << fine.velocity_error);
  CHECK(fine.l1 < coarse.l1);
  CHECK(std::log2(coarse.l1 / fine.l1) >= 0.5);
  CHECK(fine.l1 <= 0.27);              // measured 0.2335
  CHECK(fine.velocity_error <= 0.32);  // measured 0.278
}

TEST_CASE("water_state: the same bytes twice and at any thread count") {
  // The rough slope with everything on, 20 s, once on the calling thread, again, and once on four
  // workers with three rows a job: the state's hash must be the same all three times.
  Slope one;
  build_slope(one, true);
  one.solver->advance(one.in, 20.0);
  Slope two;
  build_slope(two, true);
  two.solver->advance(two.in, 20.0);
  jobs::JobSystemConfig jc;
  jc.performance_workers = 4;
  jc.efficiency_workers = 1;
  jc.pin_threads = false;
  jobs::JobSystem pool(jc);
  Slope many;
  build_slope(many, true, 3);
  many.solver->advance(many.in, 20.0, &pool);
  CHECK(one.solver->state_hash() == two.solver->state_hash());
  CHECK(one.solver->state_hash() == many.solver->state_hash());
  CHECK(one.solver->ledger().carried[0].from_dissipation ==
        many.solver->ledger().carried[0].from_dissipation);
}

TEST_CASE("water_state: the slope's bytes are the same on every toolchain") {
  // The pin: the state's hash after the rough slope's 20 s with everything on, taken on MSVC.
  // GCC and Clang must reproduce it (only + - * / and sqrt decide, the powers are core/math's, the
  // tree compiles without contraction). A failure is a bug until shown otherwise; only a deliberate
  // change to the step replaces the number, with the reason in the commit.
  Slope s;
  build_slope(s, true);
  s.solver->advance(s.in, 20.0);
  constexpr u64 k_pinned = 17390608002821259899ull;  // MSVC 14.51, 2026-11-25
  const u64 hash = s.solver->state_hash();
  if (hash != k_pinned) MESSAGE("replacement: constexpr u64 k_pinned = " << hash << "ull;");
  CHECK(hash == k_pinned);
}

TEST_CASE("water_state: a step refuses inputs that do not fit the grid") {
  Config config;
  config.nx = 4;
  config.ny = 3;
  Solver solver(config);
  std::vector<f64> bed(11, 0.0);
  Inputs in;
  in.bed_m = bed;
  CHECK(solver.step(in, 1.0) == 0.0);
  CHECK(solver.advance(in, 1.0) == 0u);
  CHECK(solver.ledger().steps == 0u);
}

TEST_CASE("water_state declares a determinism stance") {
  CHECK(std::string_view{water_state::k_determinism} == "hashed");
}
