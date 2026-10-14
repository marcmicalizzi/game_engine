#pragma once

// The implicit stage's root by a safeguarded Newton iteration inside the stage's own bracket, the
// alternative to Brent that D23 (5) allows "if bit-identical across MSVC, GCC and Clang with the
// corpus gates holding" (docs/experiments/muscle-line-tier-cost.md has what each costs).
//
// The residual is the same function Brent solves, evaluated with the same operations; Newton adds
// its slope along the stage's line l = base + diagonal * v, from the same transcendental values
// (the Gaussians' exponentials, the passive curve's, the tendon's expm1 + 1, the asinh's
// 1/sqrt(1 + z^2)), so a slope costs arithmetic and no further transcendental. The iteration keeps
// the bracket the stage's test established (r(lo) <= 0 <= r(hi)), shrinks it with every residual's
// sign, starts from the bracket's secant point, takes the Newton step when it lands strictly inside
// the bracket and bisects otherwise, and stops when the step is under Brent's tolerance
// (xtol + rtol |v|)/2 or the residual is exactly zero. The derivative ignores the two endpoint
// rules (the 4-ulp snap and the force-velocity's -1e-14 zero), which move the residual by rounding.

#include <core/base/types.h>
#include <core/math/deterministic.h>
#include <systems/muscle/law.h>

#include <cmath>

namespace engine::muscle {

// The residual at (a, base + diagonal * v, v) against `total`, and its derivative in v: the same
// domain tests in the same order as length_terms() and velocity_terms(), and the same residual.
inline bool residual_and_slope(const LawRow& p, double a, double base, double diagonal,
                               double total, double v, double& r, double& slope,
                               Failure& f) noexcept {
  const double l = base + diagonal * v;
  Geometry g{};
  if (!geometry(p, l, g, f)) return false;
  Tendon t{};
  const double lt = total - g.projected;
  if (!tendon(p, lt, t, f)) return false;
  if (!bounded(a, 0.0, 1.0)) return fail(f, ValidationTest::ActivationOutOfRange, a);
  double x = 0.0;
  if (!normalized_fiber_length(p, l, x, f)) return false;
  // active_length(x) and its derivative, the sum in the same order.
  double al = 0.0;
  double al_dx = 0.0;
  for (const auto& gs : k_gaussians) {
    const double den = gs[2] + gs[3] * x;
    const double y = (x - gs[1]) / den;
    const double e = gs[0] * det::exp(-0.5 * (y * y));
    al += e;
    al_dx += e * (-y) * ((gs[2] + gs[3] * gs[1]) / (den * den));
  }
  const double activation_length = p.force_scale * a * al;
  const double ep = det::exp(p.passive_k * (x - 1.0) / p.passive_e);
  const double passive = p.force_scale * ((ep - p.passive_offset) / p.passive_denominator);
  const double w = v / (p.vmax_lopt * p.lopt);
  if (!bounded(w, -1.0, 1.0)) return fail(f, ValidationTest::NormalizedVelocityOutOfRange, w);
  const double z = k_velocity[1] * w + k_velocity[2];
  const double fv_raw = k_velocity[0] * det::asinh(z) + k_velocity[3];
  const double fv = (-1e-14 < fv_raw && fv_raw < 0.0) ? 0.0 : fv_raw;
  const double active = activation_length * fv;
  const double damping = p.damping_scale * v / p.vmax;
  const double fiber = active + passive + damping;
  r = fiber * g.cosine - t.force;
  // The slope along l = base + diagonal * v.
  const double fv_dw = k_velocity[0] * k_velocity[1] / std::sqrt(1.0 + z * z);
  const double active_dl = p.force_scale * a * al_dx * fv / p.lopt;
  const double active_dv = activation_length * fv_dw / p.vmax;
  const double passive_dl =
      p.force_scale * ep * (p.passive_k / p.passive_e) / p.passive_denominator / p.lopt;
  const double damping_dv = p.damping_scale / p.vmax;
  const double cosine_dl = p.height * p.height / (g.projected * l * l);
  const double tendon_dl = lt / p.slack - 1.0 > 0.0  // a taut tendon; zero in compression
                               ? -(p.tendon_scale * p.tendon_k / p.slack) *
                                     (t.force / p.tendon_scale + 1.0) * (l / g.projected)
                               : 0.0;
  slope = ((active_dl + passive_dl) * diagonal + active_dv + damping_dv) * g.cosine +
          fiber * cosine_dl * diagonal - tendon_dl * diagonal;
  return true;
}

// f(x, r, slope, failure). The bracket must hold f(lo) <= 0 <= f(hi), as the stage's test made it.
template <class F>
RootStatus safeguarded_newton(F&& f, double lo, double hi, double f_lo, double f_hi, double xtol,
                              double rtol, u32 max_iterations, double& root, u32& iterations,
                              Failure& failure) noexcept {
  if (f_lo == 0.0) {
    root = lo;
    return RootStatus::Converged;
  }
  if (f_hi == 0.0) {
    root = hi;
    return RootStatus::Converged;
  }
  double x = lo - f_lo * (hi - lo) / (f_hi - f_lo);
  if (!(x > lo && x < hi)) x = 0.5 * (lo + hi);
  for (u32 i = 0; i < max_iterations; ++i) {
    ++iterations;
    double fx = 0.0;
    double dfx = 0.0;
    if (!f(x, fx, dfx, failure)) return RootStatus::Failed;
    if (fx == 0.0) {
      root = x;
      return RootStatus::Converged;
    }
    if (fx < 0.0) {
      lo = x;
    } else {
      hi = x;
    }
    double next = 0.5 * (lo + hi);
    if (dfx > 0.0 && std::isfinite(dfx)) {
      const double newton = x - fx / dfx;
      if (newton > lo && newton < hi) next = newton;
    }
    const double delta = (xtol + rtol * std::fabs(next)) / 2.0;
    if (std::fabs(next - x) < delta || hi - lo < delta) {
      root = next;
      return RootStatus::Converged;
    }
    x = next;
  }
  root = x;
  return RootStatus::NotConverged;
}

}  // namespace engine::muscle
