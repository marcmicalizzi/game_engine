#pragma once

// sin, cos and atan2 of an `f32` from arithmetic alone: the same bits from every compiler, C
// library and CPU baseline the tree builds for.
//
// The C library's `std::sin`, `std::cos`, `std::acos` and `std::atan2` are not correctly rounded,
// and MSVC's UCRT, glibc and LLVM's libc disagree in the last bit for some arguments, so a
// computation whose result is hashed or replayed on another machine cannot call them. These two
// are `+ - * /` and `std::sqrt` (correctly rounded by IEEE 754) in `f64`, in the source's order —
// the tree compiles with floating-point contraction off (ADR-0035) — and a round to `f32` at the
// end, so they are a function of their argument's bits.
//
// They were engine-view's (`fly_sin_cos` and `fly_atan2`, apps/engine_view/fly_camera.h: the
// recorded sessions' camera, the walker's heading, the crouch's leg solve) until 2026-11-19, when
// they moved here unchanged so a capability could use them (S35 stage 4's inertialization:
// docs/experiments/animation-graphs.md, "Open questions for the owner", question 4);
// engine-view's two names forward to these, and `core/math/tests/portable_trig_tests.cpp` pins a
// table of their outputs.
//
// For f64 arguments over wide ranges, with a stated accuracy against the C library, see
// `core/math/deterministic.h` (fdlibm's ports). These are the smaller, older pair: Taylor and
// half-angle series, good to about 1e-13 before the round, meant for angles within a few turns of
// zero.

#include <core/base/types.h>

#include <cmath>

namespace engine {

namespace portable_trig_detail {
// In f64, and named apart from core/math's f32 constants. (The values engine-view's file held.)
inline constexpr f64 k_pi64 = 3.14159265358979323846;
inline constexpr f64 k_two_pi64 = 6.28318530717958647692;
inline constexpr f64 k_half_pi64 = 1.57079632679489661923;
// Further than this from zero an angle is not one a turn reaches; it is a broken input.
inline constexpr f64 k_angle_limit = 1.0e6;
}  // namespace portable_trig_detail

// sin and cos of `angle` (radians): a Taylor polynomial in f64 on [-pi/2, pi/2] after reduction by
// whole turns and by pi - x, good to about 1e-13 before the round to f32. An angle that is not
// finite or is further than 1e6 from zero (not one a turn reaches: a broken input) is taken as 0,
// rather than a reduction loop that never ends. Not a general replacement for the C library —
// slower, and meant for angles within a few turns of zero.
inline void portable_sin_cos(f32 angle, f32& sin_out, f32& cos_out) noexcept {
  using portable_trig_detail::k_angle_limit;
  using portable_trig_detail::k_half_pi64;
  using portable_trig_detail::k_pi64;
  using portable_trig_detail::k_two_pi64;
  f64 x = static_cast<f64>(angle);
  if (!(x >= -k_angle_limit && x <= k_angle_limit)) x = 0.0;
  while (x > k_pi64)
    x -= k_two_pi64;
  while (x < -k_pi64)
    x += k_two_pi64;
  // Into [-pi/2, pi/2]: sin(pi - x) = sin x and cos(pi - x) = -cos x, and the same about -pi.
  f64 cos_sign = 1.0;
  if (x > k_half_pi64) {
    x = k_pi64 - x;
    cos_sign = -1.0;
  } else if (x < -k_half_pi64) {
    x = -k_pi64 - x;
    cos_sign = -1.0;
  }
  // Taylor series through x^17 and x^16: at |x| = pi/2 the first terms left out are 4e-14 and
  // 5e-13, a hundred thousand times under an f32's ulp at 1. Every coefficient is one division of
  // two exactly representable integers, which every compiler folds to the same correctly rounded
  // double, and Horner's order is the source's (no contraction, ADR-0035).
  constexpr f64 s3 = -1.0 / 6.0;
  constexpr f64 s5 = 1.0 / 120.0;
  constexpr f64 s7 = -1.0 / 5040.0;
  constexpr f64 s9 = 1.0 / 362880.0;
  constexpr f64 s11 = -1.0 / 39916800.0;
  constexpr f64 s13 = 1.0 / 6227020800.0;
  constexpr f64 s15 = -1.0 / 1307674368000.0;
  constexpr f64 s17 = 1.0 / 355687428096000.0;
  constexpr f64 c2 = -1.0 / 2.0;
  constexpr f64 c4 = 1.0 / 24.0;
  constexpr f64 c6 = -1.0 / 720.0;
  constexpr f64 c8 = 1.0 / 40320.0;
  constexpr f64 c10 = -1.0 / 3628800.0;
  constexpr f64 c12 = 1.0 / 479001600.0;
  constexpr f64 c14 = -1.0 / 87178291200.0;
  constexpr f64 c16 = 1.0 / 20922789888000.0;
  const f64 x2 = x * x;
  f64 s = s17;
  s = s * x2 + s15;
  s = s * x2 + s13;
  s = s * x2 + s11;
  s = s * x2 + s9;
  s = s * x2 + s7;
  s = s * x2 + s5;
  s = s * x2 + s3;
  s = s * x2 + 1.0;
  s = s * x;
  f64 c = c16;
  c = c * x2 + c14;
  c = c * x2 + c12;
  c = c * x2 + c10;
  c = c * x2 + c8;
  c = c * x2 + c6;
  c = c * x2 + c4;
  c = c * x2 + c2;
  c = c * x2 + 1.0;
  sin_out = static_cast<f32>(s);
  cos_out = static_cast<f32>(cos_sign * c);
}

// atan2(y, x) the same way: in [-pi, pi], from `+ - * / sqrt` in f64 — two half-angle reductions
// and a Taylor series good to about 1e-16 — so a heading or a joint angle is the same bits on
// every compiler. 0 for (0, 0) and for an argument that is not finite.
inline f32 portable_atan2(f32 y_in, f32 x_in) noexcept {
  using portable_trig_detail::k_half_pi64;
  using portable_trig_detail::k_pi64;
  const f64 y = static_cast<f64>(y_in);
  const f64 x = static_cast<f64>(x_in);
  if (!std::isfinite(y) || !std::isfinite(x) || (y == 0.0 && x == 0.0)) return 0.0f;
  const f64 ay = y < 0.0 ? -y : y;
  const f64 ax = x < 0.0 ? -x : x;
  // Into [0, 1] by atan(z) = pi/2 - atan(1/z), then twice through the half angle,
  // atan(z) = 2 atan(z / (1 + sqrt(1 + z^2))), to |z| <= tan(pi/16) = 0.199, where the series
  // through z^21 leaves out a term of 4e-18. `sqrt` is correctly rounded by IEEE, so every step is
  // the same bits on every compiler, and Horner's order is the source's (ADR-0035).
  const bool swapped = ay > ax;
  f64 z = swapped ? ax / ay : ay / ax;
  z = z / (1.0 + std::sqrt(1.0 + z * z));
  z = z / (1.0 + std::sqrt(1.0 + z * z));
  const f64 z2 = z * z;
  f64 a = -1.0 / 21.0;
  a = a * z2 + 1.0 / 19.0;
  a = a * z2 - 1.0 / 17.0;
  a = a * z2 + 1.0 / 15.0;
  a = a * z2 - 1.0 / 13.0;
  a = a * z2 + 1.0 / 11.0;
  a = a * z2 - 1.0 / 9.0;
  a = a * z2 + 1.0 / 7.0;
  a = a * z2 - 1.0 / 5.0;
  a = a * z2 + 1.0 / 3.0;
  a = a * z2 - 1.0;
  a = -(a * z);  // z - z^3/3 + z^5/5 - ...
  a = 4.0 * a;
  if (swapped) a = k_half_pi64 - a;
  if (x < 0.0) a = k_pi64 - a;
  if (y < 0.0) a = -a;
  return static_cast<f32>(a);
}

}  // namespace engine
