// The deterministic transcendentals (core/math/deterministic.h): accurate against the platform's
// `std::` to a stated number of ulps over dense grids, and the same bits on every toolchain.
//
// The second half is the point. Every grid's inputs are made with exact or correctly rounded IEEE
// arithmetic from integers, so they are the same bits everywhere, and the hash of every output is
// compared with a value committed here. The value was taken on MSVC; GCC and Clang (the Linux
// containers, `tools/linux-build.ps1`) must reproduce it. A failure on one toolchain and not
// another is a bug in the functions (a C library call, a contraction the tree's flags let through,
// a branch on something other than the argument's bits), not a value to replace.
#include <core/hash/hash.h>
#include <core/math/deterministic.h>

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace engine;

namespace {

// Distance in units in the last place between two finite doubles of any sign.
u64 ulps(double a, double b) {
  if (a == b) return 0;
  auto ordered = [](double x) {
    const i64 i = std::bit_cast<i64>(x);
    return i < 0 ? std::numeric_limits<i64>::min() - i : i;
  };
  const i64 ia = ordered(a);
  const i64 ib = ordered(b);
  return ia > ib ? static_cast<u64>(ia - ib) : static_cast<u64>(ib - ia);
}

struct Sweep {
  u64 max_ulps = 0;
  double worst_x = 0.0;
  u64 hash = k_hash_seed;
  u64 count = 0;

  template <class Det, class Ref>
  void at(double x, Det det, Ref ref) {
    const double y = det(x);
    const double expected = ref(x);
    hash = hash_combine(hash, std::bit_cast<u64>(y));
    ++count;
    if (!std::isfinite(expected) || !std::isfinite(y)) {
      CHECK(std::bit_cast<u64>(y) == std::bit_cast<u64>(expected));
      return;
    }
    const u64 d = ulps(y, expected);
    if (d > max_ulps) {
      max_ulps = d;
      worst_x = x;
    }
  }

  // n + 1 points from lo to hi inclusive: lo + i * ((hi - lo) / n), every operation one rounding.
  template <class Det, class Ref>
  void range(double lo, double hi, i64 n, Det det, Ref ref) {
    const double step = (hi - lo) / static_cast<double>(n);
    for (i64 i = 0; i <= n; ++i)
      at(lo + static_cast<double>(i) * step, det, ref);
  }

  void report(const char* name) const {
    std::printf("det::%-6s %8llu points, max %llu ulp at x = %.17g, hash 0x%016llx\n", name,
                static_cast<unsigned long long>(count), static_cast<unsigned long long>(max_ulps),
                worst_x, static_cast<unsigned long long>(hash));
  }
};

double ref_exp(double x) { return std::exp(x); }
double ref_expm1(double x) { return std::expm1(x); }
double ref_log(double x) { return std::log(x); }
double ref_log1p(double x) { return std::log1p(x); }
double ref_tanh(double x) { return std::tanh(x); }
double ref_asinh(double x) { return std::asinh(x); }
double ref_sin(double x) { return std::sin(x); }
double ref_cos(double x) { return std::cos(x); }

Sweep sweep_exp() {
  Sweep s;
  auto f = [](double x) { return det::exp(x); };
  s.range(-700.0, 700.0, 400000, f, ref_exp);
  s.range(-1.0, 1.0, 200000, f, ref_exp);
  for (i64 i = -4096; i <= 4096; ++i)
    s.at(static_cast<double>(i) * 0x1p-30, f, ref_exp);
  return s;
}

Sweep sweep_expm1() {
  Sweep s;
  auto f = [](double x) { return det::expm1(x); };
  s.range(-40.0, 100.0, 400000, f, ref_expm1);
  s.range(-1.0, 1.0, 200000, f, ref_expm1);
  for (i64 i = -4096; i <= 4096; ++i)
    s.at(static_cast<double>(i) * 0x1p-40, f, ref_expm1);
  return s;
}

Sweep sweep_log() {
  Sweep s;
  auto f = [](double x) { return det::log(x); };
  // Every binade from 2^-60 to 2^60, 4096 mantissas each: exact inputs.
  for (int e = -60; e <= 60; ++e)
    for (i64 m = 0; m < 4096; ++m)
      s.at(std::ldexp(1.0 + static_cast<double>(m) / 4096.0, e), f, ref_log);
  s.range(0.5, 2.0, 200000, f, ref_log);
  for (i64 i = -4096; i <= 4096; ++i)
    s.at(1.0 + static_cast<double>(i) * 0x1p-40, f, ref_log);
  return s;
}

Sweep sweep_log1p() {
  Sweep s;
  auto f = [](double x) { return det::log1p(x); };
  s.range(-0.999, 10.0, 400000, f, ref_log1p);
  s.range(10.0, 1.0e6, 100000, f, ref_log1p);
  for (i64 i = -4096; i <= 4096; ++i)
    s.at(static_cast<double>(i) * 0x1p-40, f, ref_log1p);
  return s;
}

Sweep sweep_tanh() {
  Sweep s;
  auto f = [](double x) { return det::tanh(x); };
  s.range(-30.0, 30.0, 400000, f, ref_tanh);
  for (i64 i = -4096; i <= 4096; ++i)
    s.at(static_cast<double>(i) * 0x1p-30, f, ref_tanh);
  return s;
}

Sweep sweep_asinh() {
  Sweep s;
  auto f = [](double x) { return det::asinh(x); };
  s.range(-10.0, 10.0, 400000, f, ref_asinh);
  s.range(-1.0e6, 1.0e6, 200000, f, ref_asinh);
  for (i64 i = -4096; i <= 4096; ++i)
    s.at(static_cast<double>(i) * 0x1p-30, f, ref_asinh);
  return s;
}

template <class Det, class Ref>
Sweep sweep_trig(Det f, Ref ref) {
  Sweep s;
  s.range(-64.0, 64.0, 800000, f, ref);
  // Next to the multiples of pi/2, where the reduction's cancellation is worst.
  const double half_pi = 1.5707963267948966;
  for (i64 k = -2000; k <= 2000; ++k) {
    const double c = static_cast<double>(k) * half_pi;
    s.at(c, f, ref);
    s.at(std::nextafter(c, 1.0e300), f, ref);
    s.at(std::nextafter(c, -1.0e300), f, ref);
  }
  s.range(-800000.0, 800000.0, 100000, f, ref);
  return s;
}

}  // namespace

TEST_CASE("deterministic: special values") {
  CHECK(det::exp(0.0) == 1.0);
  CHECK(ulps(det::exp(1.0), std::bit_cast<double>(0x4005bf0a8b145769ull)) <= 1);  // e, rounded
  CHECK(det::exp(1000.0) == std::numeric_limits<double>::infinity());
  CHECK(det::exp(-1000.0) == 0.0);
  CHECK(det::expm1(0.0) == 0.0);
  CHECK(det::expm1(-100.0) == -1.0);
  CHECK(det::log(1.0) == 0.0);
  CHECK(det::log(0.0) == -std::numeric_limits<double>::infinity());
  CHECK(std::isnan(det::log(-1.0)));
  CHECK(det::log1p(0.0) == 0.0);
  CHECK(det::log1p(-1.0) == -std::numeric_limits<double>::infinity());
  CHECK(std::isnan(det::log1p(-2.0)));
  CHECK(det::tanh(0.0) == 0.0);
  CHECK(det::tanh(50.0) == 1.0);
  CHECK(det::tanh(-std::numeric_limits<double>::infinity()) == -1.0);
  CHECK(det::asinh(0.0) == 0.0);
  CHECK(det::sin(0.0) == 0.0);
  CHECK(det::cos(0.0) == 1.0);
  CHECK(std::isnan(det::exp(std::numeric_limits<double>::quiet_NaN())));
  CHECK(std::isnan(det::sin(std::numeric_limits<double>::infinity())));
  // Beyond the medium reduction the answer is NaN, never a wrong number.
  CHECK(std::isnan(det::sin(1.0e7)));
  CHECK(std::isnan(det::cos(-1.0e7)));
  CHECK(!std::isnan(det::sin(800000.0)));
  // Odd functions are odd to the bit.
  for (double x : {0.3, 1.7, 5.25, 19.0}) {
    CHECK(det::tanh(-x) == -det::tanh(x));
    CHECK(det::asinh(-x) == -det::asinh(x));
    CHECK(det::sin(-x) == -det::sin(x));
    CHECK(det::cos(-x) == det::cos(x));
  }
}

// Each function against the platform's within a stated bound, and the hash of every output.
// The platform's own functions are within about half an ulp to an ulp or two of the true value,
// so a bound here is on the difference of two approximations. Measured against MSVC's UCRT:
// 1 ulp for exp, log, log1p, asinh, sin and cos, 2 for tanh, 3 for expm1 (at x = -0.69244, where
// the reduction is by one ln 2 and the result is near -1/2); each bound leaves one ulp for a C
// library that rounds the other way.
TEST_CASE("deterministic: accuracy against std:: and the pinned output hash") {
  struct Row {
    const char* name;
    Sweep sweep;
    u64 bound;
  };
  const Row rows[] = {
      {"exp", sweep_exp(), 2},
      {"expm1", sweep_expm1(), 4},
      {"log", sweep_log(), 2},
      {"log1p", sweep_log1p(), 2},
      {"tanh", sweep_tanh(), 3},
      {"asinh", sweep_asinh(), 2},
      {"sin", sweep_trig([](double x) { return det::sin(x); }, ref_sin), 2},
      {"cos", sweep_trig([](double x) { return det::cos(x); }, ref_cos), 2},
  };
  u64 all = k_hash_seed;
  for (const Row& row : rows) {
    row.sweep.report(row.name);
    INFO(row.name);
    CHECK(row.sweep.max_ulps <= row.bound);
    all = hash_combine(all, row.sweep.hash);
  }
  std::printf("det:: all outputs hash 0x%016llx\n", static_cast<unsigned long long>(all));
  // Taken on MSVC (x86-64-v3); GCC 13 and Clang 18 reproduce it. See the file's comment before
  // changing it.
  constexpr u64 k_pinned = 0x992a6eec6569d159ull;
  CHECK(all == k_pinned);
}
