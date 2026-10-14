#pragma once

// The line tier's law: a Hill-type fiber (De Groote et al. 2016's curves, OpenSim's
// DeGrooteFregly2016Muscle constants) with activation dynamics, in series with a compliant
// exponential tendon at a fixed pennation height (docs/subsystems/muscle.md, ADR-0055).
//
// This is the authoring track's r005 reference law (`model.py`, `compliant.py`) carried over
// expression for expression: the same operations in the same order, the same domain tests in the
// same order, the same endpoint rules. Where the reference wrote `x**2` this writes `x * x` (the
// C library's pow is exact for a square). Every transcendental is `engine::det`'s, so the bytes
// are the same on MSVC, GCC and Clang (core/math/deterministic.h); none comes from `std::`.
//
// A function that can fail returns false and fills a `Failure`: the enumerated test and the value
// it read. Nothing throws, nothing clamps, nothing fabricates a value (D20).

#include <core/base/types.h>
#include <core/math/deterministic.h>

#include <cmath>
#include <schemas/muscle.h>

namespace engine::muscle {

// The test that failed and the value it read.
struct Failure {
  ValidationTest test = ValidationTest::None;
  double value = 0.0;
};

// How a root solve ended: converged, out of iterations, or failed a domain test in a residual.
enum class RootStatus : u8 { Converged, NotConverged, Failed };

inline bool fail(Failure& f, ValidationTest test, double value) noexcept {
  f.test = test;
  f.value = value;
  return false;
}

// The reference's `bounded`: finite and inside the closed interval.
inline bool bounded(double value, double low, double high) noexcept {
  return std::isfinite(value) && low <= value && value <= high;
}

// The active force-length curve: three Gaussians (b1, b2, b3, b4), pinned constants.
inline constexpr double k_gaussians[3][4] = {
    {0.8150671134243542, 1.055033428970575, 0.162384573599574, 0.063303448465465},
    {0.433004984392647, 0.716775413397760, -0.029947116970696, 0.200356847296188},
    {0.1, 1.0, 0.353553390593274, 0.0},
};
// The force-velocity curve: d1 * asinh(d2 * w + d3) + d4.
inline constexpr double k_velocity[4] = {-0.3211346127989808, -8.149, -0.374, 0.8825327733249912};

// One actuator's law, derived once from its parameters: the hot read-only row of a tick. Every
// derived value is what the reference recomputes, in the same operations, on each call; computing
// it once gives the same bits.
struct LawRow {
  // Fiber.
  double force_scale;       // F0, N
  double lopt;              // optimal fiber length, m
  double vmax_lopt;         // vmax in optimal lengths per second
  double tau_activation;    // s
  double tau_deactivation;  // s
  double smoothing;         // the activation switch's gain
  double passive_e;         // passive strain at unit force
  double passive_k;         // passive shape
  double passive_x0;        // passive zero normalized length
  double min_norm;          // normalized length domain
  double max_norm;
  double min_snap;             // 4 ulp(min_norm): the endpoint snap
  double max_snap;             // 4 ulp(max_norm)
  double passive_offset;       // exp(k * (x0 - 1) / e)
  double passive_denominator;  // exp(k) - offset
  // Series.
  double slack;         // tendon slack length Ls, m
  double tendon_e1;     // tendon strain at unit force
  double tendon_k;      // ln 6 / e1
  double tendon_scale;  // 0.2 * F0
  double damping;       // normalized damping beta
  double min_cos;       // minimum pennation cosine
  double height;        // lopt * sin(alpha0), m
  // Derived bounds.
  double vmax;           // vmax_lopt * lopt, m/s
  double lmin;           // lopt * min_norm, m
  double lmax;           // lopt * max_norm, m
  double cap;            // Ls * (1 + 2 e1): the trial extension cap, m
  double damping_scale;  // F0 * beta
  double energy_scale;   // F0 * lopt
};

// The law's row from its parameters. The parameters are the validator's to check (validate());
// this only derives.
inline LawRow make_law_row(const FiberParameters& p, const SeriesParameters& s) noexcept {
  LawRow r{};
  r.force_scale = p.force_scale;
  r.lopt = p.optimal_fiber_length;
  r.vmax_lopt = p.vmax_lopt_per_s;
  r.tau_activation = p.tau_activation;
  r.tau_deactivation = p.tau_deactivation;
  r.smoothing = p.activation_smoothing;
  r.passive_e = p.passive_strain_at_unit_force;
  r.passive_k = p.passive_shape;
  r.passive_x0 = p.passive_zero_normalized_length;
  r.min_norm = p.min_normalized_length;
  r.max_norm = p.max_normalized_length;
  // math.ulp of a positive normal double: the gap to the next one up.
  r.min_snap = 4.0 * (std::nextafter(r.min_norm, 1.0e300) - r.min_norm);
  r.max_snap = 4.0 * (std::nextafter(r.max_norm, 1.0e300) - r.max_norm);
  r.passive_offset = det::exp(r.passive_k * (r.passive_x0 - 1.0) / r.passive_e);
  r.passive_denominator = det::exp(r.passive_k) - r.passive_offset;
  r.slack = s.tendon_slack_length;
  r.tendon_e1 = s.tendon_strain_at_unit_force;
  r.tendon_k = det::log(6.0) / r.tendon_e1;
  r.tendon_scale = 0.2 * r.force_scale;
  r.damping = s.normalized_damping;
  r.min_cos = s.minimum_cosine;
  r.height = r.lopt * det::sin(s.pennation_at_optimal);
  r.vmax = r.vmax_lopt * r.lopt;
  r.lmin = r.lopt * r.min_norm;
  r.lmax = r.lopt * r.max_norm;
  r.cap = r.slack * (1.0 + 2.0 * r.tendon_e1);
  r.damping_scale = r.force_scale * r.damping;
  r.energy_scale = r.force_scale * r.lopt;
  return r;
}

// ---- the fiber (model.py)
// ------------------------------------------------------------------------

// da/dt for activation a under excitation u.
inline bool activation_rate(const LawRow& p, double a, double u, double& out, Failure& f) noexcept {
  if (!bounded(a, 0.0, 1.0)) return fail(f, ValidationTest::ActivationOutOfRange, a);
  if (!bounded(u, 0.0, 1.0)) return fail(f, ValidationTest::ExcitationOutOfRange, u);
  const double z = 0.5 + 1.5 * a;
  const double sw = 0.5 * det::tanh(p.smoothing * (u - a));
  out = (u - a) * ((sw + 0.5) / (p.tau_activation * z) + (0.5 - sw) * z / p.tau_deactivation);
  return true;
}

// One classical RK4 step of the activation over dt from a, the excitation held. `k1` is
// rate(a, u), which the three stages of a probe share (it is the same value every time).
inline bool rk4_activation(const LawRow& p, double a, double k1, double dt, double u, double& out,
                           Failure& f) noexcept {
  if (!std::isfinite(dt) || dt <= 0.0) return fail(f, ValidationTest::TimeStepInvalid, dt);
  double k2 = 0.0;
  double k3 = 0.0;
  double k4 = 0.0;
  if (!activation_rate(p, a + dt * k1 / 2.0, u, k2, f)) return false;
  if (!activation_rate(p, a + dt * k2 / 2.0, u, k3, f)) return false;
  if (!activation_rate(p, a + dt * k3, u, k4, f)) return false;
  out = a + dt * (k1 + 2.0 * k2 + 2.0 * k3 + k4) / 6.0;
  if (!bounded(out, 0.0, 1.0)) return fail(f, ValidationTest::IntegratedActivationOutOfRange, out);
  return true;
}

inline double active_length(double x) noexcept {
  double sum = 0.0;
  for (const auto& g : k_gaussians) {
    const double y = (x - g[1]) / (g[2] + g[3] * x);
    sum += g[0] * det::exp(-0.5 * (y * y));
  }
  return sum;
}

inline bool force_velocity(double w, double& out, Failure& f) noexcept {
  if (!bounded(w, -1.0, 1.0)) return fail(f, ValidationTest::NormalizedVelocityOutOfRange, w);
  const double result =
      k_velocity[0] * det::asinh(k_velocity[1] * w + k_velocity[2]) + k_velocity[3];
  // Only a floating-point endpoint residual is erased, never an input violation.
  out = (-1e-14 < result && result < 0.0) ? 0.0 : result;
  return true;
}

inline double passive_length(const LawRow& p, double x) noexcept {
  return (det::exp(p.passive_k * (x - 1.0) / p.passive_e) - p.passive_offset) /
         p.passive_denominator;
}

// The fiber length over the optimal one, snapped to an endpoint within 4 ulp of it, else bounded.
inline bool normalized_fiber_length(const LawRow& p, double l, double& x, Failure& f) noexcept {
  x = l / p.lopt;
  if (std::fabs(x - p.min_norm) <= p.min_snap) {
    x = p.min_norm;
    return true;
  }
  if (std::fabs(x - p.max_norm) <= p.max_snap) {
    x = p.max_norm;
    return true;
  }
  if (!bounded(x, p.min_norm, p.max_norm))
    return fail(f, ValidationTest::NormalizedLengthOutOfRange, x);
  return true;
}

// The passive fiber's stored energy at length l, J.
inline bool passive_energy(const LawRow& p, double l, double& out, Failure& f) noexcept {
  double x = 0.0;
  if (!normalized_fiber_length(p, l, x, f)) return false;
  double primitive = p.passive_e / p.passive_k *
                     (det::exp(p.passive_k * (x - 1.0) / p.passive_e) - p.passive_offset);
  primitive -= p.passive_offset * (x - p.passive_x0);
  out = p.energy_scale * primitive / p.passive_denominator;
  return true;
}

// ---- the series side (compliant.py)
// --------------------------------------------------------------

struct Geometry {
  double projected;  // the belly's length along the line of action, m
  double cosine;     // the pennation cosine
};

inline bool geometry(const LawRow& p, double l, Geometry& g, Failure& f) noexcept {
  double x = 0.0;
  if (!normalized_fiber_length(p, l, x, f)) return false;
  if (l <= p.height) return fail(f, ValidationTest::FiberAtPennationHeight, l);
  g.projected = std::sqrt(l * l - p.height * p.height);
  g.cosine = g.projected / l;
  if (g.cosine < p.min_cos) return fail(f, ValidationTest::PennationCosineBelowMinimum, g.cosine);
  return true;
}

struct Tendon {
  double force;   // N, zero in compression
  double energy;  // J
};

inline bool tendon(const LawRow& p, double length, Tendon& t, Failure& f) noexcept {
  if (!std::isfinite(length) || length <= 0.0)
    return fail(f, ValidationTest::TendonLengthInvalid, length);
  const double strain = length / p.slack - 1.0;
  if (strain <= 0.0) {
    t.force = 0.0;
    t.energy = 0.0;
    return true;
  }
  if (p.tendon_k * strain > 100.0)
    return fail(f, ValidationTest::TendonExtensionBeyondDomain, p.tendon_k * strain);
  const double em1 = det::expm1(p.tendon_k * strain);
  t.force = p.tendon_scale * em1;
  t.energy = p.tendon_scale * p.slack * (em1 / p.tendon_k - strain);
  return true;
}

// The parts of the series residual that do not depend on the fiber velocity: what a stage's or
// the equilibrium's residual evaluates once per fiber length.
struct LengthTerms {
  Geometry geometry;
  Tendon tendon;
  double activation_length;  // F0 * a * active_length(x)
  double passive_force;      // F0 * passive_length(x)
};

// components() up to the velocity: geometry, the tendon, then forces() as far as the velocity, in
// the reference's order of domain tests.
inline bool length_terms(const LawRow& p, double a, double l, double total, LengthTerms& out,
                         Failure& f) noexcept {
  if (!geometry(p, l, out.geometry, f)) return false;
  if (!tendon(p, total - out.geometry.projected, out.tendon, f)) return false;
  if (!bounded(a, 0.0, 1.0)) return fail(f, ValidationTest::ActivationOutOfRange, a);
  double x = 0.0;
  if (!normalized_fiber_length(p, l, x, f)) return false;
  out.activation_length = p.force_scale * a * active_length(x);
  out.passive_force = p.force_scale * passive_length(p, x);
  return true;
}

struct Forces {
  double active;
  double passive;
  double damping;
  double tendon;
  double residual;  // (fa + fp + fd) * cos - ft
};

// The velocity's half of components(): force-velocity, damping, the residual.
inline bool velocity_terms(const LawRow& p, const LengthTerms& t, double v, Forces& out,
                           Failure& f) noexcept {
  const double w = v / (p.vmax_lopt * p.lopt);
  double fv = 0.0;
  if (!force_velocity(w, fv, f)) return false;
  out.active = t.activation_length * fv;
  out.passive = t.passive_force;
  out.damping = p.damping_scale * v / p.vmax;
  out.tendon = t.tendon.force;
  out.residual = (out.active + out.passive + out.damping) * t.geometry.cosine - out.tendon;
  return true;
}

// The whole of components() at (a, l, v) against the path length `total`.
inline bool components(const LawRow& p, double a, double l, double v, double total, Forces& out,
                       Failure& f) noexcept {
  LengthTerms t{};
  if (!length_terms(p, a, l, total, t, f)) return false;
  return velocity_terms(p, t, v, out, f);
}

}  // namespace engine::muscle
