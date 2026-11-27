// Moving water over a bed that changes from cell to cell, and lakes beside a dry bed, against their
// closed forms (roadmap W199; docs/subsystems/water_state.md, "Moving water over a bed that
// steps"). These are the cases a reconstruction that keeps moving steady states is to be judged by.
// They were built on the first-order hydrostatic reconstruction before any such change
// (2026-11-26): a case that scheme fails prints its numbers and asserts only what is true of it and
// must stay true (no number that is not finite, the lake's bounds where it holds them). Each bound
// a later scheme is to be held to is a named constant below, marked "not yet asserted", beside the
// variable that holds what was measured, so the commit that changes the scheme adds its CHECK
// without restructuring a case.
//
// The bumps are Berthon and Michel-Dansac's section 6.2.2 runs (arXiv 2501.03975), which are
// SWASHES section 3.1's (Delestre et al., arXiv 1110.0288): g 9.81, (0, 25), 75 cells, the start
// h + Z = H0 with q = q0 everywhere, q0 imposed at the left edge with h's gradient zero there, h =
// H0 imposed at the right edge where the outflow is subcritical and h's gradient zero otherwise.
// The edges' imposed values are the test's: an open (copying) edge, and the edge cell's discharge
// or depth written back after every step, so no edge kind is added to the module.
#include <core/math/deterministic.h>
#include <domain/water_state/water_state.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

using namespace engine;
using water_state::Config;
using water_state::Edge;
using water_state::Inputs;
using water_state::Solver;

namespace {

constexpr f64 k_g = 9.81;  // the paper's and SWASHES's

#if defined(NDEBUG)
constexpr bool k_optimised = true;
#else
constexpr bool k_optimised = false;
#endif

// ---- a column with test-side edge values ------------------------------------------------------

struct ColumnSpec {
  u32 nx = 0;
  f64 cell_m = 0.0;
  std::vector<f64> bed;
  std::vector<f64> depth;     // the start
  std::vector<f64> momentum;  // the start
  f64 q_in = 0.0;             // the first cell's discharge, written back after every step
  f64 h_in = 0.0;             // > 0: the first cell's depth too (a supercritical inflow fixes both)
  f64 h_out = 0.0;            // > 0: the last cell's depth (a subcritical outflow's fixed depth)
  f64 run_s = 0.0;
};

struct ColumnRun {
  std::vector<f64> h, q;      // at the end
  std::vector<f64> h_before;  // one second before the end
  u64 steps = 0;
  f64 roundoff_m3 = 0.0;
  u32 non_finite = 0;
};

void hold_edges(Solver& solver, const ColumnSpec& c) {
  if (c.h_in > 0.0) solver.depth()[0] = c.h_in;
  solver.momentum_x()[0] = c.q_in;
  if (c.h_out > 0.0) solver.depth()[c.nx - 1] = c.h_out;
}

ColumnRun run_column(const ColumnSpec& c) {
  Config config;
  config.nx = c.nx;
  config.ny = 1;
  config.cell_m = c.cell_m;
  config.gravity = k_g;
  config.edges.west = Edge::open;
  config.edges.east = Edge::open;
  Solver solver(config);
  std::copy(c.depth.begin(), c.depth.end(), solver.depth().begin());
  std::copy(c.momentum.begin(), c.momentum.end(), solver.momentum_x().begin());
  hold_edges(solver, c);
  Inputs in;
  in.bed_m = c.bed;
  f64 t = 0.0;
  const auto run_to = [&](f64 end) {
    while (t < end) {
      const f64 left = end - t;
      const f64 dt = solver.step(in, left);
      if (!(dt > 0.0)) break;
      t = dt >= left ? end : t + dt;
      hold_edges(solver, c);
    }
  };
  run_to(c.run_s - 1.0);
  ColumnRun r;
  r.h_before.assign(solver.depth().begin(), solver.depth().end());
  run_to(c.run_s);
  r.h.assign(solver.depth().begin(), solver.depth().end());
  r.q.assign(solver.momentum_x().begin(), solver.momentum_x().end());
  for (u32 i = 0; i < c.nx; ++i)
    if (!std::isfinite(r.h[i]) || !std::isfinite(r.q[i])) ++r.non_finite;
  r.steps = solver.ledger().steps;
  r.roundoff_m3 = solver.ledger().roundoff_m3;
  return r;
}

// ---- the closed forms --------------------------------------------------------------------------
//
// A frictionless steady flow of discharge q keeps Bernoulli's head E = h + q^2 / (2 g h^2) + z, so
// at each bed height h solves h^3 + (z - E) h^2 + q^2 / (2 g) = 0 (SWASHES 3.1's cubics are this
// with E from the downstream depth, or from the critical depth on the crest; *derived*). With
// b = z - E < 0 the cubic's minimum over h > 0 is at h* = -2b/3, the critical depth: the
// subcritical root lies in [h*, -b], the supercritical one in (0, h*]. Bisection, to the last bit
// it can move.
f64 depth_for_head(f64 head, f64 z, f64 q, bool subcritical) {
  const f64 b = z - head;
  const f64 c = q * q / (2.0 * k_g);
  const auto f = [&](f64 h) { return (h + b) * h * h + c; };
  const f64 hs = -2.0 * b / 3.0;
  if (!(f(hs) < 0.0)) return hs;  // critical (or no root: the head does not reach the crest)
  f64 lo = subcritical ? hs : 0.0;
  f64 hi = subcritical ? -b : hs;
  const bool lo_positive = f(lo) > 0.0;
  for (int i = 0; i < 200; ++i) {
    const f64 mid = 0.5 * (lo + hi);
    if ((f(mid) > 0.0) == lo_positive)
      lo = mid;
    else
      hi = mid;
  }
  return 0.5 * (lo + hi);
}

f64 head_of(f64 h, f64 z, f64 q) { return h + q * q / (2.0 * k_g * h * h) + z; }

/// The paper's measure (its section 6.2.2): sqrt((1/dx) sum_i |v_{i+1} - v_i|^2) over neighbouring
/// cells, which is zero for a discrete steady state that keeps v exactly.
f64 neighbour_error(const std::vector<f64>& v, f64 dx) {
  f64 s = 0.0;
  for (usize i = 0; i + 1 < v.size(); ++i)
    s += (v[i + 1] - v[i]) * (v[i + 1] - v[i]);
  return std::sqrt(s / dx);
}

std::vector<f64> bernoulli_of(const ColumnRun& r, const std::vector<f64>& bed) {
  std::vector<f64> b(r.h.size());
  for (usize i = 0; i < r.h.size(); ++i)
    b[i] = r.q[i] * r.q[i] / (2.0 * r.h[i] * r.h[i]) + k_g * (r.h[i] + bed[i]);
  return b;
}

// ---- the bumps ---------------------------------------------------------------------------------

constexpr f64 k_bump_length = 25.0;
constexpr f64 k_bump_top = 0.2;  // z_M, at x = 10

f64 bump_bed(f64 x) { return x > 8.0 && x < 12.0 ? 0.05 * (x - 8.0) * (12.0 - x) : 0.0; }

enum class Bump { subcritical, transcritical, shock };

struct BumpCase {
  const char* name;
  f64 q0;
  f64 h0;  // H0: the start's level, and the right edge's depth where the outflow is subcritical
  bool fixed_outflow;
  f64 run_s;
};

BumpCase bump_case(Bump b) {
  switch (b) {
    case Bump::subcritical: return {"subcritical", 4.42, 2.0, true, 500.0};
    case Bump::transcritical: return {"transcritical", 1.53, 0.66, false, 125.0};
    case Bump::shock: break;
  }
  return {"transcritical with a shock", 0.18, 0.33, true, 1000.0};
}

/// The exact steady state: one head throughout (subcritical), the crest's critical head with the
/// root switching at x = 10 (transcritical), or the crest's head up to the shock and the outflow's
/// after it, the shock where the two supercritical and subcritical depths keep the momentum
/// function q^2 / h + g h^2 / 2 (Rankine and Hugoniot, SWASHES 3.1.4).
struct BumpExact {
  f64 q = 0.0;
  f64 head_up = 0.0;
  f64 head_down = 0.0;
  bool transcritical = false;
  f64 x_shock = 0.0;  // 0: none
  f64 h(f64 x) const {
    const f64 z = bump_bed(x);
    if (!transcritical) return depth_for_head(head_down, z, q, true);
    if (x < 10.0) return depth_for_head(head_up, z, q, true);
    if (x_shock > 0.0 && x >= x_shock) return depth_for_head(head_down, z, q, true);
    return depth_for_head(head_up, z, q, false);
  }
};

BumpExact bump_exact(Bump b) {
  const BumpCase c = bump_case(b);
  BumpExact e;
  e.q = c.q0;
  e.head_down = head_of(c.h0, 0.0, c.q0);
  if (b == Bump::subcritical) return e;
  e.transcritical = true;
  const f64 hc = std::cbrt(c.q0 * c.q0 / k_g);
  e.head_up = 1.5 * hc + k_bump_top;
  if (b == Bump::transcritical) return e;
  // The shock: from where the outflow's head first has a subcritical root (z = E_down - 1.5 h_c on
  // the bump's falling side) to the bump's foot, by bisection on M(h_super) - M(h_sub).
  const auto momentum_gap = [&](f64 x) {
    const f64 z = bump_bed(x);
    const f64 h1 = depth_for_head(e.head_up, z, e.q, false);
    const f64 h2 = depth_for_head(e.head_down, z, e.q, true);
    return e.q * e.q * (1.0 / h1 - 1.0 / h2) + 0.5 * k_g * (h1 * h1 - h2 * h2);
  };
  const f64 z_lo = e.head_down - 1.5 * hc;  // the bed height where the outflow is critical
  f64 lo = 10.0 + std::sqrt((k_bump_top - z_lo) / 0.05);
  f64 hi = 12.0;
  const bool lo_positive = momentum_gap(lo) > 0.0;
  for (int i = 0; i < 200; ++i) {
    const f64 mid = 0.5 * (lo + hi);
    if ((momentum_gap(mid) > 0.0) == lo_positive)
      lo = mid;
    else
      hi = mid;
  }
  e.x_shock = 0.5 * (lo + hi);
  return e;
}

struct BumpRun {
  f64 e_q = 0.0;         // the paper's neighbour-difference error of q
  f64 e_b = 0.0;         // and of Bernoulli's B = q^2 / (2 h^2) + g (h + z)
  f64 h_error = 0.0;     // the largest |h - h_exact|, more than two cells from a shock
  f64 kink_error = 0.0;  // the largest |h - h_exact| within two cells of x = 10
  f64 q_range = 0.0;     // max q - min q
  f64 settle = 0.0;      // the largest |h(T) - h(T - 1 s)|
  f64 shock_x = 0.0;     // the first cell past x = 10 whose depth passes the jump's middle
  u32 non_finite = 0;
  u64 steps = 0;
};

BumpRun run_bump(Bump b, u32 nx) {
  const BumpCase c = bump_case(b);
  const BumpExact exact = bump_exact(b);
  ColumnSpec s;
  s.nx = nx;
  s.cell_m = k_bump_length / nx;
  s.bed.resize(nx);
  s.depth.resize(nx);
  s.momentum.assign(nx, c.q0);
  for (u32 i = 0; i < nx; ++i) {
    s.bed[i] = bump_bed((i + 0.5) * s.cell_m);
    s.depth[i] = c.h0 - s.bed[i];
  }
  s.q_in = c.q0;
  s.h_out = c.fixed_outflow ? c.h0 : 0.0;
  s.run_s = c.run_s;
  const ColumnRun r = run_column(s);
  BumpRun out;
  out.non_finite = r.non_finite;
  out.steps = r.steps;
  out.e_q = neighbour_error(r.q, s.cell_m);
  out.e_b = neighbour_error(bernoulli_of(r, s.bed), s.cell_m);
  f64 q_lo = r.q[0];
  f64 q_hi = r.q[0];
  f64 jump_mid = 0.0;
  if (exact.x_shock > 0.0) {
    const f64 z = bump_bed(exact.x_shock);
    jump_mid = 0.5 * (depth_for_head(exact.head_up, z, exact.q, false) +
                      depth_for_head(exact.head_down, z, exact.q, true));
  }
  for (u32 i = 0; i < nx; ++i) {
    const f64 x = (i + 0.5) * s.cell_m;
    const f64 err = std::fabs(r.h[i] - exact.h(x));
    const bool near_shock = exact.x_shock > 0.0 && std::fabs(x - exact.x_shock) <= 2.0 * s.cell_m;
    if (!near_shock) out.h_error = std::max(out.h_error, err);
    if (std::fabs(x - 10.0) <= 2.0 * s.cell_m) out.kink_error = std::max(out.kink_error, err);
    out.settle = std::max(out.settle, std::fabs(r.h[i] - r.h_before[i]));
    q_lo = std::min(q_lo, r.q[i]);
    q_hi = std::max(q_hi, r.q[i]);
    if (jump_mid > 0.0 && out.shock_x == 0.0 && x > 10.0 && r.h[i] > jump_mid) out.shock_x = x;
  }
  out.q_range = q_hi - q_lo;
  MESSAGE("bump, " << std::string(c.name) << ", " << nx << " cells: e_q " << out.e_q << ", e_B "
                   << out.e_b << "; |h - exact| " << out.h_error
                   << " (within two cells of x = 10: " << out.kink_error << "); q from " << q_lo
                   << " to " << q_hi << "; settled to " << out.settle << " m over the last second; "
                   << out.steps << " steps; the shock (0: none) at " << out.shock_x << " against "
                   << exact.x_shock);
  return out;
}

// The bounds a reconstruction that keeps moving steady states is to be held to (the paper's own
// first-order errors are 1.06e-14 to 5.21e-14 in q and B on the two smooth bumps). Not yet
// asserted: the hydrostatic reconstruction misses them by twelve orders.
constexpr f64 k_bump_steady_bound = 1e-11;  // e_q and e_B, the two smooth bumps; not yet asserted
constexpr f64 k_bump_depth_bound = 1e-10;   // |h - exact|, the subcritical bump; not yet asserted
constexpr f64 k_shock_cells = 2.0;          // the shock within this many cells; not yet asserted

// The paper's grid, 75 cells, runs in every build (a second or two in a debug build); the finer
// grids, which only say how the error falls, take 8 to 10 s a case in a debug build and run where
// the build optimises.
std::vector<u32> bump_grids(std::initializer_list<u32> finer) {
  std::vector<u32> grids{75};
  if (k_optimised) grids.insert(grids.end(), finer.begin(), finer.end());
  return grids;
}

}  // namespace

TEST_CASE("water_state: a subcritical flow over a bump keeps its discharge and its head") {
  // SWASHES 3.1.1 / the paper's Table 6: q0 4.42, h 2 at the outflow, 500 s, at 75 and 150 cells.
  // The paper's hydrostatic first order (HLL) gives e_q 7.73e-2 and e_B 1.79e-1 at 75 cells; this
  // reference's central-upwind flux is not HLL, so agreement in order is the check on the case.
  for (u32 nx : bump_grids({150})) {
    const BumpRun r = run_bump(Bump::subcritical, nx);
    CHECK(r.non_finite == 0u);
    (void)k_bump_steady_bound;  // r.e_q, r.e_b <= this: not yet asserted
    (void)k_bump_depth_bound;   // r.h_error <= this: not yet asserted
  }
}

TEST_CASE("water_state: a transcritical flow over a bump keeps its discharge and its head") {
  // SWASHES 3.1.3 / the paper's Table 7: q0 1.53, a free outflow, 125 s, at 75, 150 and 300 cells;
  // critical at the crest (x = 10). The paper's hydrostatic first order: e_q 3.74e-2, e_B 1.45e-1
  // at 75 cells. The kink at x = 10 is reported at each grid (it is to fall with the cell).
  for (u32 nx : bump_grids({150, 300})) {
    const BumpRun r = run_bump(Bump::transcritical, nx);
    CHECK(r.non_finite == 0u);
    (void)k_bump_steady_bound;  // r.e_q, r.e_b <= this: not yet asserted
  }
}

TEST_CASE("water_state: a transcritical flow over a bump with a shock") {
  // SWASHES 3.1.4 / the paper's Table 8: q0 0.18, h 0.33 at the outflow, 1000 s, at 75 and 150
  // cells; the jump on the bump's falling side by Rankine and Hugoniot. The paper's first order
  // gives e_q 5.72e-2 and e_B 6.92e-1 hydrostatic, 7.09e-2 and 8.17e-1 with its reconstruction: the
  // whole domain's error is not to improve, the smooth parts are.
  for (u32 nx : bump_grids({150})) {
    const BumpRun r = run_bump(Bump::shock, nx);
    CHECK(r.non_finite == 0u);
    (void)k_shock_cells;  // |r.shock_x - x_shock| <= this many cells: not yet asserted
  }
}
namespace {

// A supercritical inflow over a sill: q 0.5 m^2/s at 0.1 m (Fr 5.05) over a sill 0.05 m high and
// 2 m long, z = 0.05 (1 - (x - 5)^2) for |x - 5| < 1, on 10 m at 0.05 m cells, frictionless; the
// inflow fixes depth and discharge, the outflow is free. By hand: E 1.3742 m against the crest's
// E_c + z = 0.4415 + 0.05 m, so the water stays supercritical over the top and the exact state is
// the supercritical root of one head. It is the case where a reconstructed depth exceeds its
// cell's (supercritical water thickens as it climbs), which the step's positivity argument does
// not cover once the scheme keeps moving states.
constexpr f64 k_sill_q = 0.5;
constexpr f64 k_sill_h = 0.1;
constexpr u32 k_sill_cells = 200;
constexpr f64 k_sill_dx = 0.05;
constexpr f64 k_sill_steady_bound = 1e-11;  // e_q and e_B; not yet asserted

f64 sill_bed(f64 x) {
  const f64 d = x - 5.0;
  return d > -1.0 && d < 1.0 ? 0.05 * (1.0 - d * d) : 0.0;
}

}  // namespace

TEST_CASE("water_state: a supercritical flow over a sill keeps its discharge and its head") {
  ColumnSpec s;
  s.nx = k_sill_cells;
  s.cell_m = k_sill_dx;
  s.bed.resize(s.nx);
  s.depth.assign(s.nx, k_sill_h);
  s.momentum.assign(s.nx, k_sill_q);
  for (u32 i = 0; i < s.nx; ++i)
    s.bed[i] = sill_bed((i + 0.5) * k_sill_dx);
  s.q_in = k_sill_q;
  s.h_in = k_sill_h;
  s.run_s = 10.0;
  const ColumnRun r = run_column(s);
  const f64 head = head_of(k_sill_h, 0.0, k_sill_q);
  const f64 ec = 1.5 * std::cbrt(k_sill_q * k_sill_q / k_g);
  REQUIRE(head > ec + 0.05);  // the hand check: supercritical over the top
  f64 h_error = 0.0;
  f64 settle = 0.0;
  for (u32 i = 0; i < s.nx; ++i) {
    const f64 exact = depth_for_head(head, s.bed[i], k_sill_q, false);
    h_error = std::max(h_error, std::fabs(r.h[i] - exact));
    settle = std::max(settle, std::fabs(r.h[i] - r.h_before[i]));
  }
  const f64 e_q = neighbour_error(r.q, k_sill_dx);
  const f64 e_b = neighbour_error(bernoulli_of(r, s.bed), k_sill_dx);
  MESSAGE("sill: e_q " << e_q << ", e_B " << e_b << "; |h - exact| " << h_error << "; settled to "
                       << settle << " m over the last second; round-off clamped " << r.roundoff_m3
                       << " m^3; " << r.steps << " steps");
  CHECK(r.non_finite == 0u);
  (void)k_sill_steady_bound;  // e_q, e_B <= this: not yet asserted
}

TEST_CASE("water_state: a slow lake never lifts water onto a ledge above it") {
  // A lake 0.3 m deep moving at 0.03 m/s towards a 0.5 m ledge that carries a 1 mm film at rest;
  // 10 m at 0.1 m cells, closed edges, frictionless, 20 s. The lake's head (0.30005 m) is far under
  // the ledge's top, so no water may climb onto it: the ledge's water can only fall, the film
  // draining back down, and its volume is held at or under its start to 1e-12 of it at every step.
  // A reconstruction that hands the lower side of a face the depth a moving steady state would
  // give must not apply where no steady pair exists, or this face hands the lake's side nearly its
  // whole depth at the ledge's top.
  Config config;
  config.nx = 100;
  config.ny = 1;
  config.cell_m = 0.1;
  config.gravity = k_g;
  Solver solver(config);
  std::vector<f64> bed(config.nx);
  for (u32 i = 0; i < config.nx; ++i) {
    const bool ledge = (i + 0.5) * config.cell_m > 5.0;
    bed[i] = ledge ? 0.5 : 0.0;
    solver.depth()[i] = ledge ? 0.001 : 0.3;
    solver.momentum_x()[i] = ledge ? 0.0 : 0.3 * 0.03;
  }
  const auto ledge_volume = [&]() {
    f64 v = 0.0;
    for (u32 i = 0; i < config.nx; ++i)
      if (bed[i] > 0.0) v += solver.depth()[i];
    return v * config.cell_m;
  };
  const f64 start = ledge_volume();
  Inputs in;
  in.bed_m = bed;
  f64 t = 0.0;
  f64 highest = start;
  u32 non_finite = 0;
  while (t < 20.0) {
    const f64 dt = solver.step(in, 20.0 - t);
    if (!(dt > 0.0)) break;
    t += dt;
    highest = std::max(highest, ledge_volume());
  }
  for (u32 i = 0; i < config.nx; ++i)
    if (!std::isfinite(solver.depth()[i]) || !std::isfinite(solver.momentum_x()[i])) ++non_finite;
  MESSAGE("ledge: its water " << start << " m^3 at the start, at most " << highest << ", "
                              << ledge_volume() << " at the end; " << solver.ledger().steps
                              << " steps");
  CHECK(non_finite == 0u);
  CHECK(highest <= start * (1.0 + 1e-12));
}

namespace {

u32 mix(u32 x, u32 y) {  // water_state_tests.cpp's, so the bed is the same input on every toolchain
  u32 h = x * 0x9e3779b1u ^ (y + 0x7f4a7c15u) * 0x85ebca77u;
  h ^= h >> 15;
  h *= 0x2c1b3c6du;
  h ^= h >> 12;
  return h;
}

constexpr f64 k_hair = 1.0 / 72057594037927936.0;  // 2^-56 m^2/s, about 1.4e-17

}  // namespace

TEST_CASE("water_state: a lake at rest beside dry islands, perturbed by a hair, stays at rest") {
  // The dyadic lake with dry islands (water_state_tests.cpp's first case) with every wet cell that
  // has a dry neighbour given a discharge of +-2^-56 m^2/s towards no particular side, 20 s. Held
  // to the inexact lake's bounds (1e-13 m, 1e-11 m/s), nothing that is not finite, and no depth
  // clamped. A reconstruction that divides by the dry neighbour's depth is 0/0 on its first step.
  Config config;
  config.nx = 64;
  config.ny = 48;
  config.cell_m = 0.5;
  Solver solver(config);
  std::vector<f64> bed(solver.cell_count());
  constexpr f64 eta = 1.0;
  for (u32 y = 0; y < config.ny; ++y) {
    for (u32 x = 0; x < config.nx; ++x) {
      f64 z = static_cast<f64>((x * 7 + y * 13) % 17) / 64.0;
      const bool island = (x / 8 + y / 8) % 3 == 0 && (x % 8) > 1 && (y % 8) > 2;
      if (island) z = 1.0 + static_cast<f64>(mix(x, y) % 16) / 64.0;
      bed[solver.index(x, y)] = z;
      const f64 h = eta - z;
      solver.depth()[solver.index(x, y)] = h > 0.0 ? h : 0.0;
    }
  }
  u32 perturbed = 0;
  for (u32 y = 0; y < config.ny; ++y) {
    for (u32 x = 0; x < config.nx; ++x) {
      const auto dry = [&](u32 xx, u32 yy) { return solver.depth()[solver.index(xx, yy)] == 0.0; };
      if (dry(x, y)) continue;
      const bool beside = (x > 0 && dry(x - 1, y)) || (x + 1 < config.nx && dry(x + 1, y)) ||
                          (y > 0 && dry(x, y - 1)) || (y + 1 < config.ny && dry(x, y + 1));
      if (!beside) continue;
      const f64 sign = (x + y) % 2 == 0 ? 1.0 : -1.0;
      solver.momentum_x()[solver.index(x, y)] = sign * k_hair;
      solver.momentum_y()[solver.index(x, y)] = -sign * k_hair;
      ++perturbed;
    }
  }
  REQUIRE(perturbed > 100);
  Inputs in;
  in.bed_m = bed;
  solver.advance(in, 20.0);
  f64 surface_drift = 0.0;
  f64 fastest = 0.0;
  u32 non_finite = 0;
  for (u32 i = 0; i < solver.cell_count(); ++i) {
    const f64 h = solver.depth()[i];
    if (!std::isfinite(h) || !std::isfinite(solver.momentum_x()[i]) ||
        !std::isfinite(solver.momentum_y()[i]))
      ++non_finite;
    if (h > 0.0) surface_drift = std::max(surface_drift, std::fabs(h + bed[i] - eta));
    fastest = std::max(fastest, solver.speed()[i]);
  }
  MESSAGE("lake perturbed by a hair (" << perturbed << " cells): surface drift " << surface_drift
                                       << " m, fastest " << fastest << " m/s, round-off clamped "
                                       << solver.ledger().roundoff_m3 << " m^3");
  CHECK(non_finite == 0u);
  CHECK(surface_drift <= 1e-13);
  CHECK(fastest <= 1e-11);
  CHECK(solver.ledger().roundoff_m3 == 0.0);
}

namespace {

// The paper's lake at rest (its section 6.2.1) on (0, 1) at 50 cells: the bed
// omega(x) = exp(1 - 1 / (1 - (4 (x - 1/2))^2)) for |x - 1/2| < 1/4 and 0 elsewhere, whose top is
// 1, under a surface at 0.5, so the bump stands dry above the lake; run to t = 1. The paper
// imposes the exact state at both edges; a closed edge mirrors that state at rest. Its printed
// errors at first order: h 1.85e-17 and q 1.24e-16 hydrostatic, 2.75e-17 and 5.17e-17 with its
// reconstruction (Table 4). `det::exp`, so the bed is the same bytes on every toolchain.
struct EmergedRun {
  f64 h_error = 0.0;  // the L2 error of h against the start, sqrt(dx sum |h - h0|^2)
  f64 q_error = 0.0;  // and of q against zero
  f64 fastest = 0.0;
  u32 non_finite = 0;
  f64 roundoff_m3 = 0.0;
};

EmergedRun run_emerged(bool hair) {
  Config config;
  config.nx = 50;
  config.ny = 1;
  config.cell_m = 1.0 / 50.0;
  config.gravity = k_g;
  Solver solver(config);
  std::vector<f64> bed(config.nx), start(config.nx);
  for (u32 i = 0; i < config.nx; ++i) {
    const f64 x = (i + 0.5) * config.cell_m;
    const f64 s = 4.0 * (x - 0.5);
    bed[i] = std::fabs(x - 0.5) < 0.25 ? det::exp(1.0 - 1.0 / (1.0 - s * s)) : 0.0;
    start[i] = std::max(0.0, 0.5 - bed[i]);
    solver.depth()[i] = start[i];
  }
  if (hair) {
    for (u32 i = 0; i < config.nx; ++i) {
      const bool wet = start[i] > 0.0;
      const bool beside =
          (i > 0 && start[i - 1] == 0.0) || (i + 1 < config.nx && start[i + 1] == 0.0);
      if (wet && beside) solver.momentum_x()[i] = (i % 2 == 0 ? 1.0 : -1.0) * k_hair;
    }
  }
  Inputs in;
  in.bed_m = bed;
  solver.advance(in, 1.0);
  EmergedRun r;
  for (u32 i = 0; i < config.nx; ++i) {
    const f64 h = solver.depth()[i];
    const f64 q = solver.momentum_x()[i];
    if (!std::isfinite(h) || !std::isfinite(q)) ++r.non_finite;
    r.h_error += (h - start[i]) * (h - start[i]);
    r.q_error += q * q;
    r.fastest = std::max(r.fastest, solver.speed()[i]);
  }
  r.h_error = std::sqrt(r.h_error * config.cell_m);
  r.q_error = std::sqrt(r.q_error * config.cell_m);
  r.roundoff_m3 = solver.ledger().roundoff_m3;
  return r;
}

}  // namespace

TEST_CASE("water_state: a lake at rest beside an emerged bump stays at rest") {
  // At rest, held to the paper's order (1e-15 in h and q); perturbed by a hair beside the shore,
  // held to the inexact lake's bounds.
  const EmergedRun rest = run_emerged(false);
  const EmergedRun hair = run_emerged(true);
  MESSAGE("emerged lake at rest: L2 h " << rest.h_error << ", q " << rest.q_error << ", fastest "
                                        << rest.fastest << "; with the hair: h " << hair.h_error
                                        << ", q " << hair.q_error << ", fastest " << hair.fastest);
  for (const EmergedRun* r : {&rest, &hair}) {
    CHECK(r->non_finite == 0u);
    CHECK(r->roundoff_m3 == 0.0);
  }
  CHECK(rest.h_error <= 1e-15);
  CHECK(rest.q_error <= 1e-15);
  CHECK(hair.h_error <= 1e-13);
  CHECK(hair.fastest <= 1e-11);
}
