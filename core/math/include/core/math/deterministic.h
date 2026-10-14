#pragma once

// Deterministic f64 transcendentals: the same bits from every compiler and C library.
//
// `std::exp`, `expm1`, `log`, `log1p`, `tanh`, `asinh`, `sin` and `cos` are the C library's, and
// MSVC's UCRT, glibc and LLVM's libc round them differently in the last place. A computation whose
// *committed state* must be the same bytes on MSVC, GCC and Clang (the muscle line tier, ADR-0055;
// ADR-0035 for why the tree compiles with contraction off) cannot call them. These eight are built
// from IEEE `+ - * /`, `std::sqrt` (correctly rounded by IEEE 754, a single instruction on every
// target), `std::ldexp` (exact scaling) and integer operations on the representation, nothing
// else, so with floating-point contraction off they are a function of their argument's bits.
//
// The algorithms are fdlibm's (Sun Microsystems, 1993; the FreeBSD msun descendants), the
// published reference implementations: Cody-Waite reduction by ln 2 or pi/2 and a fixed minimax
// polynomial or rational kernel. The coefficients are written as their bit patterns, so no
// decimal-to-binary conversion stands between the source and the constant.
//
// Accuracy, measured against the platform's `std::` on dense grids by
// `core/math/tests/deterministic_tests.cpp` (which prints the maxima and holds the bounds): on
// MSVC's UCRT the largest difference is 1 ulp for `exp`, `log`, `log1p`, `asinh`, `sin` and
// `cos`, 2 for `tanh` and 3 for `expm1` (a difference of two approximations, each off by up to
// an ulp or so). The ranges the tests cover are the ones the line tier uses and more: `exp` on
// [-700, 700], `expm1` on [-40, 100], `log` over 2^-60..2^60, `log1p` on [-0.999, 1e6], `tanh` on
// [-30, 30], `asinh` on [-1e6, 1e6], `sin` and `cos` on [-64, 64] and at the multiples of pi/2.
//
// `sin` and `cos` reduce by pi/2 in three Cody-Waite parts (fdlibm's "medium" reduction, good to
// 118 bits), which is exact enough for |x| <= 2^19 * pi/2 (about 823,550); outside that they
// return NaN rather than a wrong answer, because the full Payne-Hanek reduction is not here. The
// line tier's arguments stay under 20 (a corpus path's `omega * t` reaches 10 pi * 0.6).
//
// `std::fma` appears nowhere: a fused multiply-add is one rounding fewer than the C++ says, and
// would make these depend on whether the compiler fused.

#include <core/base/types.h>

#include <bit>
#include <cmath>
#include <limits>

namespace engine::det {

namespace detail {

constexpr double bits(u64 b) noexcept { return std::bit_cast<double>(b); }
constexpr u64 to_bits(double x) noexcept { return std::bit_cast<u64>(x); }
constexpr u32 high_word(double x) noexcept { return static_cast<u32>(to_bits(x) >> 32); }
constexpr double abs(double x) noexcept { return bits(to_bits(x) & 0x7fffffffffffffffull); }
constexpr double with_sign_of(double magnitude, u32 sign_word) noexcept {
  return (sign_word >> 31) != 0 ? -magnitude : magnitude;
}

inline constexpr double k_inf = std::numeric_limits<double>::infinity();
inline constexpr double k_nan = std::numeric_limits<double>::quiet_NaN();

// ln 2 in two parts: the high part has 32 trailing zero bits, so k * hi is exact for |k| < 2^20.
inline constexpr double k_ln2_hi = bits(0x3fe62e42fee00000ull);
inline constexpr double k_ln2_lo = bits(0x3dea39ef35793c76ull);
inline constexpr double k_ln2 = bits(0x3fe62e42fefa39efull);
inline constexpr double k_inv_ln2 = bits(0x3ff71547652b82feull);

// e_exp.c: exp(r) = 1 + r + r*c/(2-c), c from this polynomial in r^2 on [-0.5 ln2, 0.5 ln2].
inline constexpr double k_exp_p1 = bits(0x3fc555555555553eull);
inline constexpr double k_exp_p2 = bits(0xbf66c16c16bebd93ull);
inline constexpr double k_exp_p3 = bits(0x3f11566aaf25de2cull);
inline constexpr double k_exp_p4 = bits(0xbebbbd41c5d26bf1ull);
inline constexpr double k_exp_p5 = bits(0x3e66376972bea4d0ull);

// s_expm1.c: the rational kernel's polynomial in r^2 / 2.
inline constexpr double k_expm1_q1 = bits(0xbfa11111111110f4ull);
inline constexpr double k_expm1_q2 = bits(0x3f5a01a019fe5585ull);
inline constexpr double k_expm1_q3 = bits(0xbf14ce199eaadbb7ull);
inline constexpr double k_expm1_q4 = bits(0x3ed0cfca86e65239ull);
inline constexpr double k_expm1_q5 = bits(0xbe8afdb76e09c32dull);

// e_log.c: log(1+f) = 2s + s*R(s^2), s = f/(2+f).
inline constexpr double k_lg1 = bits(0x3fe5555555555593ull);
inline constexpr double k_lg2 = bits(0x3fd999999997fa04ull);
inline constexpr double k_lg3 = bits(0x3fd2492494229359ull);
inline constexpr double k_lg4 = bits(0x3fcc71c51d8e78afull);
inline constexpr double k_lg5 = bits(0x3fc7466496cb03deull);
inline constexpr double k_lg6 = bits(0x3fc39a09d078c69full);
inline constexpr double k_lg7 = bits(0x3fc2f112df3e5244ull);

// k_sin.c and k_cos.c: the kernels on [-pi/4, pi/4].
inline constexpr double k_s1 = bits(0xbfc5555555555549ull);
inline constexpr double k_s2 = bits(0x3f8111111110f8a6ull);
inline constexpr double k_s3 = bits(0xbf2a01a019c161d5ull);
inline constexpr double k_s4 = bits(0x3ec71de357b1fe7dull);
inline constexpr double k_s5 = bits(0xbe5ae5e68a2b9cebull);
inline constexpr double k_s6 = bits(0x3de5d93a5acfd57cull);
inline constexpr double k_c1 = bits(0x3fa555555555554cull);
inline constexpr double k_c2 = bits(0xbf56c16c16c15177ull);
inline constexpr double k_c3 = bits(0x3efa01a019cb1590ull);
inline constexpr double k_c4 = bits(0xbe927e4f809c52adull);
inline constexpr double k_c5 = bits(0x3e21ee9ebdb4b1c4ull);
inline constexpr double k_c6 = bits(0xbda8fae9be8838d4ull);

// e_rem_pio2.c: 2/pi, and pi/2 in three parts of 33 bits plus each remainder's tail.
inline constexpr double k_inv_pio2 = bits(0x3fe45f306dc9c883ull);
inline constexpr double k_pio2_1 = bits(0x3ff921fb54400000ull);
inline constexpr double k_pio2_1t = bits(0x3dd0b4611a626331ull);
inline constexpr double k_pio2_2 = bits(0x3dd0b4611a600000ull);
inline constexpr double k_pio2_2t = bits(0x3ba3198a2e037073ull);
inline constexpr double k_pio2_3 = bits(0x3ba3198a2e000000ull);
inline constexpr double k_pio2_3t = bits(0x397b839a252049c1ull);

// 2^k for a k whose result is normal, built from its exponent field.
constexpr double pow2(int k) noexcept { return bits(static_cast<u64>(1023 + k) << 52); }

inline double sin_kernel(double x, double y, bool has_tail) noexcept {
  const double z = x * x;
  const double v = z * x;
  const double r = k_s2 + z * (k_s3 + z * (k_s4 + z * (k_s5 + z * k_s6)));
  if (!has_tail) return x + v * (k_s1 + z * r);
  return x - ((z * (0.5 * y - v * r) - y) - v * k_s1);
}

inline double cos_kernel(double x, double y) noexcept {
  const double z = x * x;
  const double w = z * z;
  const double r = z * (k_c1 + z * (k_c2 + z * k_c3)) + w * w * (k_c4 + z * (k_c5 + z * k_c6));
  const double hz = 0.5 * z;
  const double one_minus = 1.0 - hz;
  return one_minus + (((1.0 - one_minus) - hz) + (z * r - x * y));
}

// x = n * pi/2 + (y0 + y1), |y0 + y1| <= pi/4; valid for |x| <= 2^19 * pi/2.
inline int rem_pio2(double x, double& y0, double& y1) noexcept {
  const u32 hx = high_word(x);
  const u32 ix = hx & 0x7fffffffu;
  const double t = abs(x);
  const int n = static_cast<int>(t * k_inv_pio2 + 0.5);
  const double fn = static_cast<double>(n);
  double r = t - fn * k_pio2_1;
  double w = fn * k_pio2_1t;  // first round: good to 85 bits
  const int j = static_cast<int>(ix >> 20);
  y0 = r - w;
  int i = j - static_cast<int>((high_word(y0) >> 20) & 0x7ffu);
  if (i > 16) {  // second round: good to 118 bits
    double rt = r;
    w = fn * k_pio2_2;
    r = rt - w;
    w = fn * k_pio2_2t - ((rt - r) - w);
    y0 = r - w;
    i = j - static_cast<int>((high_word(y0) >> 20) & 0x7ffu);
    if (i > 49) {  // third round: 151 bits, for the rare argument very close to a multiple
      rt = r;
      w = fn * k_pio2_3;
      r = rt - w;
      w = fn * k_pio2_3t - ((rt - r) - w);
      y0 = r - w;
    }
  }
  y1 = (r - y0) - w;
  if ((hx >> 31) != 0) {
    y0 = -y0;
    y1 = -y1;
    return -n;
  }
  return n;
}

}  // namespace detail

// e^x. Correctly scaled through the normal range; overflow gives +inf, underflow 0.
inline double exp(double x) noexcept {
  using namespace detail;
  if (x != x) return x;
  if (x > 709.782712893383973096) return k_inf;
  if (x < -745.13321910194110842) return 0.0;
  const u32 hx = high_word(x) & 0x7fffffffu;
  const bool negative = x < 0.0;
  double hi = 0.0;
  double lo = 0.0;
  int k = 0;
  if (hx > 0x3fd62e42u) {    // |x| > 0.5 ln2
    if (hx < 0x3ff0a2b2u) {  // |x| < 1.5 ln2
      hi = negative ? x + k_ln2_hi : x - k_ln2_hi;
      lo = negative ? -k_ln2_lo : k_ln2_lo;
      k = negative ? -1 : 1;
    } else {
      k = static_cast<int>(k_inv_ln2 * x + (negative ? -0.5 : 0.5));
      const double t = static_cast<double>(k);
      hi = x - t * k_ln2_hi;
      lo = t * k_ln2_lo;
    }
    x = hi - lo;
  } else if (hx < 0x3e300000u) {  // |x| < 2^-28
    return 1.0 + x;
  }
  const double t = x * x;
  const double c =
      x - t * (k_exp_p1 + t * (k_exp_p2 + t * (k_exp_p3 + t * (k_exp_p4 + t * k_exp_p5))));
  if (k == 0) return 1.0 - ((x * c) / (c - 2.0) - x);
  const double y = 1.0 - ((lo - (x * c) / (2.0 - c)) - hi);
  return std::ldexp(y, k);
}

// e^x - 1, accurate near zero where `exp(x) - 1` cancels.
inline double expm1(double x) noexcept {
  using namespace detail;
  const u32 sign = high_word(x);
  const u32 hx = sign & 0x7fffffffu;
  const bool negative = (sign >> 31) != 0;
  if (hx >= 0x4043687au) {    // |x| >= 56 ln2
    if (hx >= 0x40862e42u) {  // |x| >= 709.78
      if (hx >= 0x7ff00000u) {
        if (x != x) return x + x;
        return negative ? -1.0 : x;
      }
      if (x > 709.782712893383973096) return k_inf;
    }
    if (negative) return -1.0;
  }
  double hi = 0.0;
  double lo = 0.0;
  double c = 0.0;
  int k = 0;
  if (hx > 0x3fd62e42u) {    // |x| > 0.5 ln2
    if (hx < 0x3ff0a2b2u) {  // |x| < 1.5 ln2
      hi = negative ? x + k_ln2_hi : x - k_ln2_hi;
      lo = negative ? -k_ln2_lo : k_ln2_lo;
      k = negative ? -1 : 1;
    } else {
      k = static_cast<int>(k_inv_ln2 * x + (negative ? -0.5 : 0.5));
      const double t = static_cast<double>(k);
      hi = x - t * k_ln2_hi;
      lo = t * k_ln2_lo;
    }
    x = hi - lo;
    c = (hi - x) - lo;
  } else if (hx < 0x3c900000u) {  // |x| < 2^-54
    return x;
  }
  const double hfx = 0.5 * x;
  const double hxs = x * hfx;
  const double r1 =
      1.0 + hxs * (k_expm1_q1 +
                   hxs * (k_expm1_q2 + hxs * (k_expm1_q3 + hxs * (k_expm1_q4 + hxs * k_expm1_q5))));
  double t = 3.0 - r1 * hfx;
  double e = hxs * ((r1 - t) / (6.0 - x * t));
  if (k == 0) return x - (x * e - hxs);
  e = x * (e - c) - c;
  e -= hxs;
  if (k == -1) return 0.5 * (x - e) - 0.5;
  if (k == 1) {
    if (x < -0.25) return -2.0 * (e - (x + 0.5));
    return 1.0 + 2.0 * (x - e);
  }
  if (k <= -2 || k > 56) {
    const double y = 1.0 - (e - x);
    return std::ldexp(y, k) - 1.0;
  }
  if (k < 20) {
    t = 1.0 - pow2(-k);  // exact: 1 - 2^-k
    return std::ldexp(t - (e - x), k);
  }
  t = pow2(-k);
  return std::ldexp((x - (e + t)) + 1.0, k);
}

// Natural logarithm. log(0) = -inf, a negative argument NaN.
inline double log(double x) noexcept {
  using namespace detail;
  u64 b = to_bits(x);
  i32 hx = static_cast<i32>(b >> 32);
  const u32 lx = static_cast<u32>(b);
  int k = 0;
  if (hx < 0x00100000) {  // x < 2^-1022
    if (((static_cast<u32>(hx) & 0x7fffffffu) | lx) == 0) return -k_inf;
    if (hx < 0) return k_nan;
    k -= 54;
    x *= bits(0x4350000000000000ull);  // 2^54: scale a subnormal up
    b = to_bits(x);
    hx = static_cast<i32>(b >> 32);
  }
  if (hx >= 0x7ff00000) return x + x;
  k += (hx >> 20) - 1023;
  hx &= 0x000fffff;
  const i32 i = (hx + 0x95f64) & 0x100000;
  // Normalize x or x/2 into [sqrt(2)/2, sqrt(2)).
  x = bits((static_cast<u64>(static_cast<u32>(hx | (i ^ 0x3ff00000))) << 32) | (b & 0xffffffffull));
  k += i >> 20;
  const double f = x - 1.0;
  if ((0x000fffff & (2 + hx)) < 3) {  // -2^-20 <= f < 2^-20
    if (f == 0.0) {
      if (k == 0) return 0.0;
      const double dk = static_cast<double>(k);
      return dk * k_ln2_hi + dk * k_ln2_lo;
    }
    const double r = f * f * (0.5 - 0.33333333333333333 * f);
    if (k == 0) return f - r;
    const double dk = static_cast<double>(k);
    return dk * k_ln2_hi - ((r - dk * k_ln2_lo) - f);
  }
  const double s = f / (2.0 + f);
  const double dk = static_cast<double>(k);
  const double z = s * s;
  i32 ii = hx - 0x6147a;
  const double w = z * z;
  const i32 jj = 0x6b851 - hx;
  const double t1 = w * (k_lg2 + w * (k_lg4 + w * k_lg6));
  const double t2 = z * (k_lg1 + w * (k_lg3 + w * (k_lg5 + w * k_lg7)));
  ii |= jj;
  const double r = t2 + t1;
  if (ii > 0) {
    const double hfsq = 0.5 * f * f;
    if (k == 0) return f - (hfsq - s * (hfsq + r));
    return dk * k_ln2_hi - ((hfsq - (s * (hfsq + r) + dk * k_ln2_lo)) - f);
  }
  if (k == 0) return f - s * (f - r);
  return dk * k_ln2_hi - ((s * (f - r) - dk * k_ln2_lo) - f);
}

// log(1 + x), accurate near zero: u = 1 + x rounds, and ((u - 1) - x) / u is what the rounding
// added, taken back off (Goldberg's correction). One rounding more than `log`.
inline double log1p(double x) noexcept {
  using namespace detail;
  if (!(x > -1.0)) return x == -1.0 ? -k_inf : k_nan;
  const double u = 1.0 + x;
  if (u == 1.0) return x;
  if (u == k_inf) return u;
  return log(u) - ((u - 1.0) - x) / u;
}

// Hyperbolic tangent, from expm1 so that no e^x - e^-x cancels.
inline double tanh(double x) noexcept {
  using namespace detail;
  const u32 jx = high_word(x);
  const u32 ix = jx & 0x7fffffffu;
  if (ix >= 0x7ff00000u) {
    if (x != x) return x + x;
    return with_sign_of(1.0, jx);
  }
  double z = 1.0;
  if (ix < 0x40360000u) {            // |x| < 22
    if (ix < 0x3c800000u) return x;  // |x| < 2^-55
    if (ix >= 0x3ff00000u) {         // |x| >= 1
      const double t = expm1(2.0 * abs(x));
      z = 1.0 - 2.0 / (t + 2.0);
    } else {
      const double t = expm1(-2.0 * abs(x));
      z = -t / (t + 2.0);
    }
  }
  return with_sign_of(z, jx);
}

// Inverse hyperbolic sine.
inline double asinh(double x) noexcept {
  using namespace detail;
  const u32 hx = high_word(x);
  const u32 ix = hx & 0x7fffffffu;
  if (ix >= 0x7ff00000u) return x + x;
  if (ix < 0x3e300000u) return x;  // |x| < 2^-28
  const double ax = abs(x);
  double w = 0.0;
  if (ix > 0x41b00000u) {  // |x| > 2^28
    w = log(ax) + k_ln2;
  } else if (ix > 0x40000000u) {  // 2 < |x| <= 2^28
    w = log(2.0 * ax + 1.0 / (std::sqrt(x * x + 1.0) + ax));
  } else {
    const double t = x * x;
    w = log1p(ax + t / (1.0 + std::sqrt(1.0 + t)));
  }
  return with_sign_of(w, hx);
}

// Sine, for |x| <= 2^19 * pi/2; NaN outside that (see the header comment).
inline double sin(double x) noexcept {
  using namespace detail;
  const u32 ix = high_word(x) & 0x7fffffffu;
  if (ix <= 0x3fe921fbu) {           // |x| ~<= pi/4
    if (ix < 0x3e400000u) return x;  // |x| < 2^-27
    return sin_kernel(x, 0.0, false);
  }
  if (ix >= 0x7ff00000u) return x - x;  // inf or NaN
  if (ix > 0x413921fbu) return k_nan;   // beyond the medium reduction
  double y0 = 0.0;
  double y1 = 0.0;
  const int n = rem_pio2(x, y0, y1);
  switch (n & 3) {
    case 0: return sin_kernel(y0, y1, true);
    case 1: return cos_kernel(y0, y1);
    case 2: return -sin_kernel(y0, y1, true);
    default: return -cos_kernel(y0, y1);
  }
}

// Cosine, for |x| <= 2^19 * pi/2; NaN outside that (see the header comment).
inline double cos(double x) noexcept {
  using namespace detail;
  const u32 ix = high_word(x) & 0x7fffffffu;
  if (ix <= 0x3fe921fbu) {             // |x| ~<= pi/4
    if (ix < 0x3e400000u) return 1.0;  // |x| < 2^-27
    return cos_kernel(x, 0.0);
  }
  if (ix >= 0x7ff00000u) return x - x;
  if (ix > 0x413921fbu) return k_nan;
  double y0 = 0.0;
  double y1 = 0.0;
  const int n = rem_pio2(x, y0, y1);
  switch (n & 3) {
    case 0: return cos_kernel(y0, y1);
    case 1: return -sin_kernel(y0, y1, true);
    case 2: return -cos_kernel(y0, y1);
    default: return sin_kernel(y0, y1, true);
  }
}

}  // namespace engine::det
