// The water_state reference's cases (docs/subsystems/water_state.md, "The cases"). Each case's
// bound is stated beside it with the reason for it; a closed form's formulas are the source's,
// cited at the helper that evaluates them.
#include <domain/water_state/water_state.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
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

TEST_CASE(
    "water_state: water is counted to the last cell through wetting, drying and an open edge") {
  // A block of water released down a rough slope with friction, a spring and a drain, out through
  // an open east edge. surface + out - initial - sources - roundoff is zero in exact arithmetic;
  // in double each of the run's few hundred thousand additions rounds by half an ulp of what it
  // adds, so the bound is 1e-12 of the water involved, and the clamp's round-off is held
  // separately.
  Config config;
  config.nx = 48;
  config.ny = 40;
  config.cell_m = 0.5;
  config.edges.east = Edge::open;
  Solver solver(config);
  const u32 cells = solver.cell_count();
  std::vector<f64> bed(cells), manning(cells, 0.03), source(cells, 0.0);
  for (u32 y = 0; y < config.ny; ++y) {
    for (u32 x = 0; x < config.nx; ++x) {
      const u32 i = solver.index(x, y);
      const f64 bump = 0.08 * static_cast<f64>(mix(x / 3, y / 3) % 97) / 97.0;
      bed[i] = 0.6 - 0.02 * static_cast<f64>(x) * config.cell_m + bump;
      if (x < 12) solver.depth()[i] = 0.3;
      if (x >= 20 && x < 24 && y >= 6 && y < 10) source[i] = 2e-3;    // a spring
      if (x >= 30 && x < 34 && y >= 28 && y < 32) source[i] = -5e-3;  // a drain
    }
  }
  std::vector<bool> was_wet(cells);
  for (u32 i = 0; i < cells; ++i)
    was_wet[i] = solver.depth()[i] > 0.0;
  const f64 initial = solver.surface_volume_m3();
  Inputs in;
  in.bed_m = bed;
  in.manning = manning;
  in.source_m_per_s = source;
  u32 wetted = 0;
  f64 lowest = 0.0;
  for (int piece = 0; piece < 6; ++piece) {
    solver.advance(in, 10.0);
    lowest = std::min(lowest, min_of(solver.depth()));
  }
  for (u32 i = 0; i < cells; ++i)
    if (!was_wet[i] && solver.depth()[i] > 0.0) ++wetted;
  u32 drained = 0;
  for (u32 y = 28; y < 32; ++y)
    for (u32 x = 30; x < 34; ++x)
      drained += solver.depth()[solver.index(x, y)] == 0.0 ? 1u : 0u;
  const auto& ledger = solver.ledger();
  const f64 balance = solver.surface_volume_m3() + ledger.open_edge_out_m3 - initial -
                      ledger.source_m3 - ledger.roundoff_m3;
  MESSAGE("count: initial " << initial << " m3, source " << ledger.source_m3 << ", out "
                            << ledger.open_edge_out_m3 << ", roundoff " << ledger.roundoff_m3
                            << ", balance " << balance << ", wetted " << wetted << ", steps "
                            << ledger.steps);
  CHECK(lowest >= 0.0);
  CHECK(wetted > 200u);
  CHECK(drained > 0u);
  CHECK(ledger.open_edge_out_m3 > 0.0);
  CHECK(std::fabs(balance) <= 1e-12 * (initial + std::fabs(ledger.source_m3)));
  CHECK(ledger.roundoff_m3 <= 1e-15 * initial);
  CHECK(max_abs_of(solver.momentum_x()) < 10.0);  // nothing blew up
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
