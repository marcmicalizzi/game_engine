#pragma once

// The HDR output encode in double (gfx/display.h, display.slang; E39), the way brdf_reference.h
// and sky_reference.h pin the lighting model and the sky: SMPTE ST 2084 written from the standard's
// rationals, the Rec. 709 to BT.2020 matrix derived from the two sets of primaries and the D65
// white rather than copied, and the display tone curve. The CPU test holds display.h's float
// functions and constants to these; the renderer's HDR test decodes the GPU's codes through them.

#include <cmath>

namespace engine::gfx::reference {

// ST 2084: m1 = 2610/16384, m2 = 2523/4096 * 128, c1 = 3424/4096, c2 = 2413/4096 * 32,
// c3 = 2392/4096 * 32; luminance normalized to 10,000 nits.
inline constexpr double k_m1 = 2610.0 / 16384.0;
inline constexpr double k_m2 = 2523.0 / 4096.0 * 128.0;
inline constexpr double k_c1 = 3424.0 / 4096.0;
inline constexpr double k_c2 = 2413.0 / 4096.0 * 32.0;
inline constexpr double k_c3 = 2392.0 / 4096.0 * 32.0;

inline double pq_encode(double nits) {
  const double y = nits <= 0.0 ? 0.0 : (nits >= 10000.0 ? 1.0 : nits / 10000.0);
  const double p = std::pow(y, k_m1);
  return std::pow((k_c1 + k_c2 * p) / (1.0 + k_c3 * p), k_m2);
}

inline double pq_decode(double e) {
  const double c = e <= 0.0 ? 0.0 : (e >= 1.0 ? 1.0 : e);
  const double p = std::pow(c, 1.0 / k_m2);
  const double num = p - k_c1 > 0.0 ? p - k_c1 : 0.0;
  return 10000.0 * std::pow(num / (k_c2 - k_c3 * p), 1.0 / k_m1);
}

// RGB to XYZ for primaries (x, y) red, green, blue and white: the columns are the primaries' XYZ
// at Y = 1, scaled so that RGB (1, 1, 1) is the white at Y = 1.
inline void rgb_to_xyz(const double r[2], const double g[2], const double b[2], const double w[2],
                       double out[3][3]) {
  const double* p[3] = {r, g, b};
  double m[3][3];
  for (int c = 0; c < 3; ++c) {
    m[0][c] = p[c][0] / p[c][1];
    m[1][c] = 1.0;
    m[2][c] = (1.0 - p[c][0] - p[c][1]) / p[c][1];
  }
  const double white[3] = {w[0] / w[1], 1.0, (1.0 - w[0] - w[1]) / w[1]};
  // Solve m * s = white (Cramer's rule).
  const auto det = [](const double a[3][3]) {
    return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
           a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
           a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  };
  const double d = det(m);
  double s[3];
  for (int c = 0; c < 3; ++c) {
    double t[3][3];
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j)
        t[i][j] = j == c ? white[i] : m[i][j];
    s[c] = det(t) / d;
  }
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      out[i][j] = m[i][j] * s[j];
}

inline void invert3(const double a[3][3], double out[3][3]) {
  const double d = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                   a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                   a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  out[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / d;
  out[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / d;
  out[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / d;
  out[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / d;
  out[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / d;
  out[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / d;
  out[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / d;
  out[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / d;
  out[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / d;
}

// Linear Rec. 709 to linear BT.2020 (ITU-R BT.709 and BT.2020 primaries, D65 both): XYZ from 709,
// then into 2020.
inline void bt709_to_bt2020(double out[3][3]) {
  const double w[2] = {0.3127, 0.3290};
  const double r709[2] = {0.64, 0.33}, g709[2] = {0.30, 0.60}, b709[2] = {0.15, 0.06};
  const double r2020[2] = {0.708, 0.292}, g2020[2] = {0.170, 0.797}, b2020[2] = {0.131, 0.046};
  double a[3][3], b[3][3], b_inv[3][3];
  rgb_to_xyz(r709, g709, b709, w, a);
  rgb_to_xyz(r2020, g2020, b2020, w, b);
  invert3(b, b_inv);
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      double s = 0.0;
      for (int k = 0; k < 3; ++k)
        s += b_inv[i][k] * a[k][j];
      out[i][j] = s;
    }
}

// The display tone curve (display.h's `display_tone`).
inline double display_tone(double x, double knee, double ceiling) {
  if (x <= knee || knee >= ceiling) return x;
  return knee + (ceiling - knee) * (1.0 - std::exp(-(x - knee) / (ceiling - knee)));
}

}  // namespace engine::gfx::reference
