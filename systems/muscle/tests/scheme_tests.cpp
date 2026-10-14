// The adopted scheme (scheme.h): the pinned tableau against its rational forms, the order-three
// identities, and the shared stage loop on a scalar problem whose answer is known.
#include <systems/muscle/scheme.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>

using namespace engine;
using namespace engine::muscle;

TEST_CASE("muscle scheme: the tableau is the pinned binary64 Alexander SDIRK3") {
  // The hex values are r009's TABLEAU.json, used verbatim; each is the binary64 evaluation of the
  // rational form in ARM-S.md with g * g for gamma squared.
  const double g = k_gamma;
  CHECK(g == 0x1.be53cb1d33509p-2);
  CHECK(g == 0.43586652150845899941601945);  // the decimal literal it was rounded from
  CHECK(k_a21 == (1.0 - g) / 2.0);
  CHECK(k_b1 == (-6.0 * (g * g) + 16.0 * g - 1.0) / 4.0);
  CHECK(k_b2 == (6.0 * (g * g) - 20.0 * g + 5.0) / 4.0);
  CHECK(k_b3 == g);
  CHECK(k_tableau_c[1] == (1.0 + g) / 2.0);
  CHECK(k_tableau_c[2] == 1.0);
  CHECK(k_b2 < 0.0);  // the signed weight the work channels keep
  // gamma is the root in (0.43, 0.44) of 6 g^3 - 18 g^2 + 9 g - 1.
  CHECK(std::fabs(6.0 * g * g * g - 18.0 * g * g + 9.0 * g - 1.0) < 1e-15);
  // Stiffly accurate: the last row is the weights.
  for (u32 j = 0; j < 3; ++j)
    CHECK(k_tableau_a[2][j] == k_tableau_b[j]);
}

TEST_CASE("muscle scheme: the order conditions hold to rounding") {
  const double* b = k_tableau_b;
  const double* c = k_tableau_c;
  const auto& a = k_tableau_a;
  double row[3] = {0.0, 0.0, 0.0};
  for (u32 i = 0; i < 3; ++i)
    for (u32 j = 0; j < 3; ++j)
      row[i] += a[i][j];
  for (u32 i = 0; i < 3; ++i)
    CHECK(std::fabs(row[i] - c[i]) < 1e-15);  // c_i = sum_j a_ij
  const double o1 = b[0] + b[1] + b[2];
  const double o2 = b[0] * c[0] + b[1] * c[1] + b[2] * c[2];
  const double o3a = b[0] * c[0] * c[0] + b[1] * c[1] * c[1] + b[2] * c[2] * c[2];
  double o3b = 0.0;
  for (u32 i = 0; i < 3; ++i)
    for (u32 j = 0; j < 3; ++j)
      o3b += b[i] * a[i][j] * c[j];
  CHECK(std::fabs(o1 - 1.0) < 1e-15);
  CHECK(std::fabs(o2 - 0.5) < 1e-15);
  CHECK(std::fabs(o3a - 1.0 / 3.0) < 1e-15);
  CHECK(std::fabs(o3b - 1.0 / 6.0) < 1e-15);
}

namespace {

// y' = lambda y and w' = y through the shared stage loop: stage i solves Y_i = base_i +
// gamma d K_i with K_i = lambda Y_i, the stiffly accurate end is the last stage, and w is
// integrated by quadrature with the same signed weights the work channels use.
struct Scalar {
  double y;
  double w;
};

Scalar step(Scalar s, double lambda, double d) {
  double k[3] = {0.0, 0.0, 0.0};
  double y_stage[3] = {0.0, 0.0, 0.0};
  const bool ok =
      stage_loop(s.y, 0.0, d, k, [&](u32 i, double base, double, double diagonal, double& k_i) {
        k_i = lambda * base / (1.0 - diagonal * lambda);
        y_stage[i] = base + diagonal * k_i;
        return true;
      });
  REQUIRE(ok);
  double w = s.w;
  for (u32 i = 0; i < 3; ++i)
    w += d * k_tableau_b[i] * y_stage[i];
  return Scalar{y_stage[2], w};
}

double error_after(u32 n) {
  Scalar s{1.0, 0.0};
  const double d = 1.0 / static_cast<double>(n);
  for (u32 i = 0; i < n; ++i) {
    s = step(s, -1.0, d);
    CHECK(std::fabs(s.y + s.w - 1.0) <= 1e-13);  // y + w is conserved
  }
  return std::fabs(s.y - std::exp(-1.0));
}

}  // namespace

TEST_CASE("muscle scheme: third order on y' = -y, w' = y, with y + w conserved") {
  const double e8 = error_after(8);
  const double e16 = error_after(16);
  const double e32 = error_after(32);
  const double e64 = error_after(64);
  std::printf(
      "muscle scheme: |error| at n = 8, 16, 32, 64: %.3e %.3e %.3e %.3e; ratios %.3f %.3f %.3f\n",
      e8, e16, e32, e64, e8 / e16, e16 / e32, e32 / e64);
  for (const double ratio : {e8 / e16, e16 / e32, e32 / e64}) {
    CHECK(ratio >= 7.0);
    CHECK(ratio <= 10.0);
  }
}

TEST_CASE("muscle scheme: the stability function, and L-stability in the stiff limit") {
  const double g = k_gamma;
  for (const double z : {-1.0, -10.0, -100.0, -1.0e6}) {
    const double numerator = 1.0 + (1.0 - 3.0 * g) * z + (0.5 - 3.0 * g + 3.0 * g * g) * z * z;
    const double denominator = (1.0 - g * z) * (1.0 - g * z) * (1.0 - g * z);
    const double r = numerator / denominator;
    const Scalar s = step(Scalar{1.0, 0.0}, z, 1.0);
    INFO("z = " << z << ": step " << s.y << ", R(z) " << r);
    // The stages cancel as z grows (R(-1e6) is 3e-6 from terms of order one), so the agreement
    // is relative to 1e-9 rather than to rounding.
    CHECK(std::fabs(s.y - r) <= 1e-9 * std::fabs(r));
    CHECK(std::fabs(r) < 1.0);
  }
  const double stiff = step(Scalar{1.0, 0.0}, -1.0e6, 1.0).y;
  CHECK(std::fabs(stiff) < 1e-5);  // R(z) -> 0 as z -> -inf: O(1/z)
}
