#include <domain/terrain/fixed.h>

#include <array>

namespace engine::terrain::fx {

namespace {

constexpr u32 k_table = 1024;  // entries per quarter turn, plus the end point

// sin(i * pi / (2 * 1024)) in Q15, by a Taylor series to x^15 in Q30 integer arithmetic: the
// compiler evaluates it, so its value is a property of the program, not of a C library.
constexpr std::array<i32, k_table + 1> make_sin_table() {
  std::array<i32, k_table + 1> table{};
  constexpr i64 k_pi_q30 = 3373259426;  // pi * 2^30, rounded
  for (u32 i = 0; i <= k_table; ++i) {
    const i64 x = (static_cast<i64>(i) * k_pi_q30) / (2 * k_table);
    i64 term = x;
    i64 sum = x;
    for (i64 k = 1; k <= 7; ++k) {
      term = (term * x) >> 30;
      term = (term * x) >> 30;
      term = -term / ((2 * k) * (2 * k + 1));
      sum += term;
    }
    i64 q15 = (sum + (i64{1} << 14)) >> 15;
    if (q15 > k_one_q15) q15 = k_one_q15;
    table[i] = static_cast<i32>(q15);
  }
  return table;
}

constexpr std::array<i32, k_table + 1> k_sin = make_sin_table();
static_assert(k_sin[0] == 0 && k_sin[k_table] == k_one_q15);
static_assert(k_sin[k_table / 2] == 23170);  // sin(45 degrees) = 0.70711 in Q15

}  // namespace

i32 sin_q15(u32 angle) noexcept {
  const u32 a = angle & 0xFFFFu;
  const u32 quadrant = a >> 14;
  u32 index = a & 0x3FFFu;
  if ((quadrant & 1u) != 0) index = 0x4000u - index;
  const u32 i = index >> 4;
  const u32 f = index & 15u;
  i32 v = k_sin[i];
  if (f != 0) v += ((k_sin[i + 1] - k_sin[i]) * static_cast<i32>(f)) / 16;
  return quadrant >= 2 ? -v : v;
}

u64 isqrt(u64 v) noexcept {
  // Bit by bit, from the highest even power of two not above v: exact, and the same on every
  // machine, which a float square root rounded to an integer would not promise near a square.
  u64 result = 0;
  u64 bit = u64{1} << 62;
  while (bit > v)
    bit >>= 2;
  while (bit != 0) {
    if (v >= result + bit) {
      v -= result + bit;
      result = (result >> 1) + bit;
    } else {
      result >>= 1;
    }
    bit >>= 2;
  }
  return result;
}

}  // namespace engine::terrain::fx
