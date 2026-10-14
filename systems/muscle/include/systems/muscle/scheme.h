#pragma once

// The line tier's scheme: Alexander's three-stage, stiffly accurate, L-stable SDIRK of order three
// for the mechanics, an independent classical RK4 for each stage's activation, and the scalar
// implicit stage root (ADR-0055; the authoring track's r009 ARM-S.md and `probe_s`, r005
// `stepping.stage`). The adopted scheme and its pinned binary64 tableau are D23 (2); nothing here
// is tuned.
//
// One probe advances (t, a, l) over d with the excitation held:
//   a_i = RK4(a, c_i d)                 three independent activation steps from the same a
//   base_i = l + d * sum_{j<i} a_ij v_j  in increasing j
//   v_i solves the series residual at (a_i, base_i + gamma d v_i, v_i, L(t + c_i d))
//   the end state is (t + d, a_3, l_3); stage 3's components are the force cache
//   the five work channels gain (d b_i) P_i in stage order, b_2 negative, nothing clipped.

#include <core/base/types.h>
#include <systems/muscle/law.h>
#include <systems/muscle/newton.h>

#include <bit>
#include <cmath>

namespace engine::muscle {

// The pinned Alexander SDIRK3 tableau (r009 TABLEAU.json, used verbatim). gamma is the root in
// (0.43, 0.44) of 6 g^3 - 18 g^2 + 9 g - 1 = 0; a21 = (1 - g)/2, b1 = (-6g^2 + 16g - 1)/4,
// b2 = (6g^2 - 20g + 5)/4, b3 = g; c = {g, (1 + g)/2, 1}. tests/scheme_tests.cpp pins these against
// the rational forms and checks the order-three identities.
inline constexpr double k_gamma = 0x1.be53cb1d33509p-2;
inline constexpr double k_a21 = 0x1.20d61a716657cp-2;
inline constexpr double k_b1 = 0x1.35600951895b4p+0;
inline constexpr double k_b2 = -0x1.49e9f831ac5eep-1;
inline constexpr double k_b3 = k_gamma;
inline constexpr double k_tableau_a[3][3] = {
    {k_gamma, 0.0, 0.0},
    {k_a21, k_gamma, 0.0},
    {k_b1, k_b2, k_gamma},
};
inline constexpr double k_tableau_b[3] = {k_b1, k_b2, k_b3};
inline constexpr double k_tableau_c[3] = {k_gamma, 0x1.6f94f2c74cd42p-1, 1.0};

// How the implicit stage's root is solved: scipy's Brent, as the reference does, or a safeguarded
// Newton inside the same bracket (newton.h, D23 (5)). The kernel uses k_stage_root, Newton: under
// the same corpus gates (every case's metrics equal Brent's to four digits) and the same
// cross-compiler bytes, it costs 20 to 23% less a tick (docs/experiments/muscle-line-tier-cost.md).
// The corpus test runs both and pins both; Brent stays as the reference's own iteration.
enum class StageRoot : u8 { Brent, Newton };
inline constexpr StageRoot k_stage_root = StageRoot::Newton;

// The five work channels, in the ledger's order.
enum WorkChannel : u32 {
  k_work_external = 0,             // tendon force * path rate
  k_work_active_output = 1,        // -active force * fiber velocity
  k_work_dissipation = 2,          // damping force * fiber velocity
  k_work_active_throughput = 3,    // |active output power|
  k_work_external_throughput = 4,  // |external power|
  k_work_channels = 5,
};

// A point of a trajectory: time, activation, fiber length.
struct State {
  double t;
  double a;
  double l;
};

inline bool same_state(const State& x, const State& y) noexcept {
  return std::bit_cast<u64>(x.t) == std::bit_cast<u64>(y.t) &&
         std::bit_cast<u64>(x.a) == std::bit_cast<u64>(y.a) &&
         std::bit_cast<u64>(x.l) == std::bit_cast<u64>(y.l);
}

// The end of a step's final stage: what a committed state caches.
struct Cache {
  double tendon_force;
  double active_force;
  double passive_force;
  double damping_force;
  double passive_energy;
  double tendon_energy;
};

// What a tick spends, for the bench's breakdown and the schedule tests.
struct Counters {
  u32 probes = 0;            // S probes (three stages each)
  u32 stages = 0;            // implicit stages solved
  u32 residuals = 0;         // series residual evaluations (velocity half), stages and roots
  u32 root_iterations = 0;   // Brent iterations, every root
  u32 rate_evaluations = 0;  // activation right-hand sides
  u32 events = 0;            // located crossings
  u32 bisection_probes = 0;  // probes spent locating them
  u32 zero_boundary_queries = 0;
  u32 equilibrium_solves = 0;  // validate_state's and the gap rate's roots
};

// ---- Brent's method, as scipy's brentq (scipy/optimize/Zeros/brentq.c)
// ---------------------------
//
// The reference solves every root with scipy's brentq; the same iteration, the same tolerances and
// the same failure rules here keep the semantics (and, given the same residuals, the iterates).
// `f(x, fx, failure)` evaluates the residual; the two end values are the caller's, already
// computed, since the reference's bracket test evaluates them before brentq evaluates them again.
// Returns false with RootNotConverged's test after `max_iterations`, or the residual's own failure.
template <class F>
RootStatus brent(F&& f, double xa, double xb, double fa, double fb, double xtol, double rtol,
                 u32 max_iterations, double& root, u32& iterations, Failure& failure) noexcept {
  double xpre = xa;
  double xcur = xb;
  double fpre = fa;
  double fcur = fb;
  double xblk = 0.0;
  double fblk = 0.0;
  double spre = 0.0;
  double scur = 0.0;
  if (fpre == 0.0) {
    root = xpre;
    return RootStatus::Converged;
  }
  if (fcur == 0.0) {
    root = xcur;
    return RootStatus::Converged;
  }
  for (u32 i = 0; i < max_iterations; ++i) {
    ++iterations;
    if (fpre != 0.0 && fcur != 0.0 && (std::signbit(fpre) != std::signbit(fcur))) {
      xblk = xpre;
      fblk = fpre;
      spre = scur = xcur - xpre;
    }
    if (std::fabs(fblk) < std::fabs(fcur)) {
      xpre = xcur;
      xcur = xblk;
      xblk = xpre;
      fpre = fcur;
      fcur = fblk;
      fblk = fpre;
    }
    const double delta = (xtol + rtol * std::fabs(xcur)) / 2.0;
    const double sbis = (xblk - xcur) / 2.0;
    if (fcur == 0.0 || std::fabs(sbis) < delta) {
      root = xcur;
      return RootStatus::Converged;
    }
    if (std::fabs(spre) > delta && std::fabs(fcur) < std::fabs(fpre)) {
      double stry = 0.0;
      if (xpre == xblk) {
        stry = -fcur * (xcur - xpre) / (fcur - fpre);  // interpolate
      } else {
        const double dpre = (fpre - fcur) / (xpre - xcur);  // extrapolate
        const double dblk = (fblk - fcur) / (xblk - xcur);
        stry = -fcur * (fblk * dblk - fpre * dpre) / (dblk * dpre * (fblk - fpre));
      }
      const double limit = std::fabs(spre) < 3.0 * std::fabs(sbis) - delta
                               ? std::fabs(spre)
                               : 3.0 * std::fabs(sbis) - delta;
      if (2.0 * std::fabs(stry) < limit) {
        spre = scur;  // a good short step
        scur = stry;
      } else {
        spre = sbis;  // bisect
        scur = sbis;
      }
    } else {
      spre = sbis;
      scur = sbis;
    }
    xpre = xcur;
    fpre = fcur;
    if (std::fabs(scur) > delta) {
      xcur += scur;
    } else {
      xcur += (sbis > 0.0 ? delta : -delta);
    }
    if (!f(xcur, fcur, failure)) return RootStatus::Failed;
  }
  root = xcur;
  return RootStatus::NotConverged;
}

// scipy wraps the residual so that a NaN stops the solver with an error; so does the engine.
inline bool residual_is_number(double r, double x, Failure& f) noexcept {
  if (r != r) return fail(f, ValidationTest::ResidualNotANumber, x);
  return true;
}

// The equilibrium fiber velocity at (a, l) against path length `total`: the root of the series
// residual over [-vmax, vmax] (compliant.equilibrium_velocity; maxiter 100, scipy's default).
inline bool equilibrium_velocity(const LawRow& p, double a, double l, double total, double& v,
                                 Counters& n, Failure& f) noexcept {
  ++n.equilibrium_solves;
  LengthTerms terms{};
  if (!length_terms(p, a, l, total, terms, f)) return false;
  auto residual = [&](double x, double& r, Failure& fl) noexcept {
    ++n.residuals;
    Forces forces{};
    if (!velocity_terms(p, terms, x, forces, fl)) return false;
    r = forces.residual;
    return residual_is_number(r, x, fl);
  };
  double left = 0.0;
  double right = 0.0;
  if (!residual(-p.vmax, left, f)) return false;
  if (!residual(p.vmax, right, f)) return false;
  if (left > 0.0 || right < 0.0)
    return fail(f, ValidationTest::EquilibriumNotBracketed, left > 0.0 ? left : right);
  switch (
      brent(residual, -p.vmax, p.vmax, left, right, 2e-14, 1e-14, 100, v, n.root_iterations, f)) {
    case RootStatus::Converged: return true;
    case RootStatus::NotConverged: return fail(f, ValidationTest::EquilibriumNotConverged, v);
    default: return false;
  }
}

// A path sample: positive finite length, finite rate (stepping.sample_path).
struct PathSample {
  double length;
  double rate;
};

template <class Path>
inline bool sample_path(const Path& path, double t, double tick_start, PathSample& s,
                        Failure& f) noexcept {
  path.sample(t, tick_start, s.length, s.rate);
  if (!std::isfinite(s.length) || s.length <= 0.0)
    return fail(f, ValidationTest::PathSampleInvalid, s.length);
  if (!std::isfinite(s.rate)) return fail(f, ValidationTest::PathSampleInvalid, s.rate);
  return true;
}

// One implicit stage (stepping.stage): solve for the fiber velocity v at which the series residual
// at length base + diagonal * v vanishes, inside the bracket the length and speed domains allow.
struct StageOut {
  double l;
  double v;
  Forces forces;
  double tendon_energy;
  double path_rate;
};

template <StageRoot R = k_stage_root, class Path>
inline bool solve_stage(const LawRow& p, const Path& path, double tick_start, double a, double base,
                        double t, double diagonal, StageOut& out, Counters& n,
                        Failure& f) noexcept {
  ++n.stages;
  if (!bounded(a, 0.0, 1.0)) return fail(f, ValidationTest::StageActivationOutOfRange, a);
  PathSample s{};
  if (!sample_path(path, t, tick_start, s, f)) return false;
  const double total = s.length;
  double low = p.lmin;
  const double above = total - 1e-8 > 0.0 ? total - 1e-8 : 0.0;
  const double reach = std::sqrt(above * above + p.height * p.height);
  const double high = reach < p.lmax ? reach : p.lmax;
  if (total > p.cap) {
    const double over = total - p.cap;
    const double floor = std::sqrt(over * over + p.height * p.height);
    if (floor > low) low = floor;
  }
  // Python's max(-vmax, x) and min(vmax, x): the first argument unless the second is beyond it.
  const double lo_v = (low - base) / diagonal;
  const double hi_v = (high - base) / diagonal;
  double left = lo_v > -p.vmax ? lo_v : -p.vmax;
  double right = hi_v < p.vmax ? hi_v : p.vmax;
  if (left >= right) return fail(f, ValidationTest::StageBracketEmpty, right - left);
  left = std::nextafter(left, right);
  right = std::nextafter(right, left);
  auto residual = [&](double v, double& r, Failure& fl) noexcept {
    ++n.residuals;
    LengthTerms terms{};
    if (!length_terms(p, a, base + diagonal * v, total, terms, fl)) return false;
    Forces forces{};
    if (!velocity_terms(p, terms, v, forces, fl)) return false;
    r = forces.residual;
    return residual_is_number(r, v, fl);
  };
  double r_left = 0.0;
  double r_right = 0.0;
  if (!residual(left, r_left, f)) return false;
  if (r_left > 0.0) return fail(f, ValidationTest::StageRootNotBracketed, r_left);
  if (!residual(right, r_right, f)) return false;
  if (r_right < 0.0) return fail(f, ValidationTest::StageRootNotBracketed, r_right);
  double v = 0.0;
  RootStatus status = RootStatus::Failed;
  if constexpr (R == StageRoot::Newton) {
    auto with_slope = [&](double x, double& r, double& slope, Failure& fl) noexcept {
      ++n.residuals;
      if (!residual_and_slope(p, a, base, diagonal, total, x, r, slope, fl)) return false;
      return residual_is_number(r, x, fl);
    };
    status = safeguarded_newton(with_slope, left, right, r_left, r_right, 2e-14, 1e-14, 64, v,
                                n.root_iterations, f);
  } else {
    status =
        brent(residual, left, right, r_left, r_right, 2e-14, 1e-14, 64, v, n.root_iterations, f);
  }
  switch (status) {
    case RootStatus::Converged: break;
    case RootStatus::NotConverged: return fail(f, ValidationTest::StageRootNotConverged, v);
    default: return false;
  }
  out.l = base + diagonal * v;
  out.v = v;
  out.path_rate = s.rate;
  // components() at the root, as the reference recomputes it for the stage's values.
  LengthTerms terms{};
  if (!length_terms(p, a, out.l, total, terms, f)) return false;
  out.tendon_energy = terms.tendon.energy;
  return velocity_terms(p, terms, v, out.forces, f);
}

// The shared stage loop (methods.stage_loop): stage i's base is l + d * sum_{j<i} a_ij v_j summed
// in increasing j, its time t + c_i d and its diagonal gamma d; `solve(i, base, t_i, diagonal,
// v_i)` solves it. The probe and the tests' scalar fixtures drive this same loop.
template <class Solve>
inline bool stage_loop(double l, double t, double d, double (&v)[3], Solve&& solve) noexcept {
  for (u32 i = 0; i < 3; ++i) {
    double prior = 0.0;
    for (u32 j = 0; j < i; ++j)
      prior += k_tableau_a[i][j] * v[j];
    if (!solve(i, l + d * prior, t + k_tableau_c[i] * d, k_gamma * d, v[i])) return false;
  }
  return true;
}

// One S probe and what the locator and the ledger read from it.
struct Probe {
  State stage[3];                    // the stage states; stage[2] is the end
  double terms[3][k_work_channels];  // (d b_i) P_i per stage, in stage order
  Cache cache;                       // stage 3's forces and energies
};

// One S probe of duration d from `start` with the excitation u held (methods.probe_s).
template <StageRoot R = k_stage_root, class Path>
inline bool probe_s(const LawRow& p, const Path& path, double tick_start, double u,
                    const State& start, double d, Probe& out, Counters& n, Failure& f) noexcept {
  if (!std::isfinite(d) || d <= 0.0) return fail(f, ValidationTest::TimeStepInvalid, d);
  if (!std::isfinite(start.t)) return fail(f, ValidationTest::TimeNotFinite, start.t);
  ++n.probes;
  // k1 = rate(a, u) is the first evaluation of every stage's RK4 and the same value each time.
  double k1 = 0.0;
  if (!activation_rate(p, start.a, u, k1, f)) return false;
  n.rate_evaluations += 1;
  double v[3] = {0.0, 0.0, 0.0};
  return stage_loop(start.l, start.t, d, v,
                    [&](u32 i, double base, double t_i, double diagonal, double& v_i) noexcept {
                      double a_i = 0.0;
                      if (!rk4_activation(p, start.a, k1, k_tableau_c[i] * d, u, a_i, f))
                        return false;
                      n.rate_evaluations += 3;
                      StageOut st{};
                      if (!solve_stage<R>(p, path, tick_start, a_i, base, t_i, diagonal, st, n, f))
                        return false;
                      v_i = st.v;
                      out.stage[i] = State{t_i, a_i, st.l};
                      const double external = st.forces.tendon * st.path_rate;
                      const double active_output = -st.forces.active * st.v;
                      const double dissipation = st.forces.damping * st.v;
                      const double weight = d * k_tableau_b[i];
                      out.terms[i][k_work_external] = weight * external;
                      out.terms[i][k_work_active_output] = weight * active_output;
                      out.terms[i][k_work_dissipation] = weight * dissipation;
                      out.terms[i][k_work_active_throughput] = weight * std::fabs(active_output);
                      out.terms[i][k_work_external_throughput] = weight * std::fabs(external);
                      if (i == 2) {
                        out.cache.tendon_force = st.forces.tendon;
                        out.cache.active_force = st.forces.active;
                        out.cache.passive_force = st.forces.passive;
                        out.cache.damping_force = st.forces.damping;
                        out.cache.tendon_energy = st.tendon_energy;
                        if (!passive_energy(p, st.l, out.cache.passive_energy, f)) return false;
                      }
                      return true;
                    });
}

}  // namespace engine::muscle
