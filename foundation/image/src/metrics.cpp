// PSNR, SSIM, and LDR-FLIP (docs/subsystems/image.md, docs/plan/04-renderer.md §4.8).
//
// FLIP follows Andersson, Nilsson, Akenine-Möller, Oskarsson, Åström, Fairchild, "FLIP: A
// Difference Evaluator for Alternating Images", HPG 2020, and its constants are the paper's;
// they were checked against NVIDIA's reference implementation (NVlabs/flip, BSD-3-Clause,
// third_party/LICENSES.md). The code below is this project's own.
//
// Two deliberate departures from the reference, both documented in image.md: each Gaussian of
// the contrast-sensitivity filter gets its own three-sigma support instead of the widest one's,
// which is the same filter to a thousandth and about a third less work; and the two-Gaussian
// channel is evaluated as two separable convolutions blended by their own window mass, which is
// exactly the normalized two-dimensional filter and costs four one-dimensional passes instead
// of a 21x21 gather.
#include <foundation/image/metrics.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace engine::image {
namespace {

constexpr f32 k_pi = 3.14159265358979f;

// FLIP's constants (paper §3): the color-difference exponent and the two numbers that
// redistribute it, the width in degrees of the feature-detection Gaussian, and the feature
// exponent.
constexpr f32 k_gqc = 0.7f;
constexpr f32 k_gpc = 0.4f;
constexpr f32 k_gpt = 0.95f;
constexpr f32 k_gw = 0.082f;
constexpr f32 k_gqf = 0.5f;

// The spatial contrast sensitivity of the three opponent channels, each a sum of at most two
// Gaussians in the frequency domain: amplitude and scale per term, achromatic first, then
// red-green, then blue-yellow. An amplitude of zero means the channel has one term.
struct CsfTerm {
  f32 amplitude;
  f32 scale;
};
constexpr CsfTerm k_csf[3][2] = {
    {{1.0f, 0.0047f}, {0.0f, 1.0e-5f}},
    {{1.0f, 0.0053f}, {0.0f, 1.0e-5f}},
    {{34.1f, 0.04f}, {13.5f, 0.025f}},
};

// D65, the white both YCxCz and CIELab are taken relative to.
constexpr f32 k_white_x = 0.950428545f;
constexpr f32 k_white_y = 1.0f;
constexpr f32 k_white_z = 1.088900371f;

// Linear sRGB to XYZ divided by that white, so the division happens once here and never per
// pixel, and its inverse (XYZ-over-white back to linear sRGB).
constexpr f32 k_rgb_to_wxyz[9] = {
    0.41239080f / k_white_x, 0.35758434f / k_white_x, 0.18048079f / k_white_x,
    0.21263901f / k_white_y, 0.71516868f / k_white_y, 0.07219232f / k_white_y,
    0.01933082f / k_white_z, 0.11919478f / k_white_z, 0.95053215f / k_white_z};
constexpr f32 k_wxyz_to_rgb[9] = {
    3.24096994f * k_white_x,  -1.53738318f * k_white_y, -0.49861076f * k_white_z,
    -0.96924364f * k_white_x, 1.87596750f * k_white_y,  0.04155506f * k_white_z,
    0.05563008f * k_white_x,  -0.20397696f * k_white_y, 1.05697151f * k_white_z};

f32 srgb_to_linear(f32 c) noexcept {
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// 8-bit sRGB has 256 possible values per channel, so the transfer function is a table.
const f32* srgb_table() {
  static const std::array<f32, 256> table = [] {
    std::array<f32, 256> t{};
    for (u32 i = 0; i < 256; ++i)
      t[i] = srgb_to_linear(static_cast<f32>(i) / 255.0f);
    return t;
  }();
  return table.data();
}

f32 lab_f(f32 t) noexcept {
  constexpr f32 k_delta = 6.0f / 29.0f;
  constexpr f32 k_delta_cubed = k_delta * k_delta * k_delta;
  return t > k_delta_cubed ? std::cbrt(t) : t / (3.0f * k_delta * k_delta) + 4.0f / 29.0f;
}

// Linear sRGB in [0, 1] to CIELab with the Hunt adjustment, which scales the chroma axes by
// the lightness so that a difference in a dark region counts for less than the same difference
// in a bright one.
void linear_rgb_to_hunt_lab(f32 r, f32 g, f32 b, f32& out_l, f32& out_a, f32& out_b) noexcept {
  const f32 x = k_rgb_to_wxyz[0] * r + k_rgb_to_wxyz[1] * g + k_rgb_to_wxyz[2] * b;
  const f32 y = k_rgb_to_wxyz[3] * r + k_rgb_to_wxyz[4] * g + k_rgb_to_wxyz[5] * b;
  const f32 z = k_rgb_to_wxyz[6] * r + k_rgb_to_wxyz[7] * g + k_rgb_to_wxyz[8] * b;
  const f32 fx = lab_f(x);
  const f32 fy = lab_f(y);
  const f32 fz = lab_f(z);
  const f32 lightness = 116.0f * fy - 16.0f;
  const f32 hunt = 0.01f * lightness;
  out_l = lightness;
  out_a = hunt * 500.0f * (fx - fy);
  out_b = hunt * 200.0f * (fy - fz);
}

// A filtered YCxCz triple back to the Hunt-adjusted CIELab the color difference is taken in.
// The clamp is in linear sRGB, not in XYZ: the filter can push a value outside the gamut and
// what a display would show is the clamped color.
void ycc_to_hunt_lab(f32 yy, f32 cx, f32 cz, f32& out_l, f32& out_a, f32& out_b) noexcept {
  const f32 ty = (yy + 16.0f) / 116.0f;
  const f32 tx = ty + cx / 500.0f;
  const f32 tz = ty - cz / 200.0f;
  const f32 r =
      std::clamp(k_wxyz_to_rgb[0] * tx + k_wxyz_to_rgb[1] * ty + k_wxyz_to_rgb[2] * tz, 0.0f, 1.0f);
  const f32 g =
      std::clamp(k_wxyz_to_rgb[3] * tx + k_wxyz_to_rgb[4] * ty + k_wxyz_to_rgb[5] * tz, 0.0f, 1.0f);
  const f32 b =
      std::clamp(k_wxyz_to_rgb[6] * tx + k_wxyz_to_rgb[7] * ty + k_wxyz_to_rgb[8] * tz, 0.0f, 1.0f);
  linear_rgb_to_hunt_lab(r, g, b, out_l, out_a, out_b);
}

f32 hyab(f32 l1, f32 a1, f32 b1, f32 l2, f32 a2, f32 b2) noexcept {
  const f32 da = a1 - a2;
  const f32 db = b1 - b2;
  return std::abs(l1 - l2) + std::sqrt(da * da + db * db);
}

// The largest color difference the sRGB gamut can produce, which is what the redistribution
// normalizes against: pure green against pure blue.
f32 max_color_distance() {
  static const f32 value = [] {
    f32 gl = 0.0f;
    f32 ga = 0.0f;
    f32 gb = 0.0f;
    f32 bl = 0.0f;
    f32 ba = 0.0f;
    f32 bb = 0.0f;
    linear_rgb_to_hunt_lab(0.0f, 1.0f, 0.0f, gl, ga, gb);
    linear_rgb_to_hunt_lab(0.0f, 0.0f, 1.0f, bl, ba, bb);
    return std::pow(hyab(gl, ga, gb, bl, ba, bb), k_gqc);
  }();
  return value;
}

// ---- separable filtering -----------------------------------------------------------------

struct Kernel1d {
  Vector<f32> weights;
  u32 radius = 0;
  f32 window_mass = 0.0f;  // the sum before normalization, which weighs a channel's two terms
};

// One Gaussian of the contrast-sensitivity filter, sampled in degrees of visual angle. The
// frequency-domain term a * sqrt(pi/b) * exp(-pi^2 v^2 / b) is this Gaussian in the spatial
// domain, whose standard deviation is sqrt(b / 2pi^2) degrees; three of those is the support.
Kernel1d csf_kernel(f32 scale, f32 ppd) {
  Kernel1d k;
  const f32 sigma_px = std::sqrt(scale / (2.0f * k_pi * k_pi)) * ppd;
  k.radius = static_cast<u32>(std::ceil(3.0f * sigma_px));
  if (k.radius < 1) k.radius = 1;
  const u32 taps = 2 * k.radius + 1;
  k.weights.resize(taps);
  f32 sum = 0.0f;
  for (u32 i = 0; i < taps; ++i) {
    const f32 degrees = (static_cast<f32>(i) - static_cast<f32>(k.radius)) / ppd;
    const f32 w = std::exp(-k_pi * k_pi * degrees * degrees / scale);
    k.weights[i] = w;
    sum += w;
  }
  k.window_mass = sum;
  for (u32 i = 0; i < taps; ++i)
    k.weights[i] /= sum;
  return k;
}

// The feature detector's Gaussian and its first and second derivatives, at the paper's width
// of 0.082 degrees. The derivatives have their positive and negative halves normalized to +1
// and -1 separately, so a step of one unit produces a response of one unit.
void feature_kernels(f32 ppd, Kernel1d& smooth, Kernel1d& first, Kernel1d& second) {
  const f32 sigma = 0.5f * k_gw * ppd;
  u32 radius = static_cast<u32>(std::ceil(3.0f * sigma));
  if (radius < 1) radius = 1;
  const u32 taps = 2 * radius + 1;
  smooth.radius = radius;
  first.radius = radius;
  second.radius = radius;
  smooth.weights.resize(taps);
  first.weights.resize(taps);
  second.weights.resize(taps);
  f32 gauss_sum = 0.0f;
  f32 first_positive = 0.0f;
  f32 first_negative = 0.0f;
  f32 second_positive = 0.0f;
  f32 second_negative = 0.0f;
  for (u32 i = 0; i < taps; ++i) {
    const f32 x = static_cast<f32>(i) - static_cast<f32>(radius);
    const f32 g = std::exp(-x * x / (2.0f * sigma * sigma));
    const f32 dg = -x * g;
    const f32 ddg = (x * x / (sigma * sigma) - 1.0f) * g;
    smooth.weights[i] = g;
    first.weights[i] = dg;
    second.weights[i] = ddg;
    gauss_sum += g;
    if (dg > 0.0f) {
      first_positive += dg;
    } else {
      first_negative -= dg;
    }
    if (ddg > 0.0f) {
      second_positive += ddg;
    } else {
      second_negative -= ddg;
    }
  }
  for (u32 i = 0; i < taps; ++i) {
    smooth.weights[i] /= gauss_sum;
    const f32 dg = first.weights[i];
    if (dg != 0.0f) first.weights[i] = dg / (dg > 0.0f ? first_positive : first_negative);
    const f32 ddg = second.weights[i];
    if (ddg != 0.0f) second.weights[i] = ddg / (ddg > 0.0f ? second_positive : second_negative);
  }
  smooth.window_mass = gauss_sum;
  first.window_mass = first_positive;
  second.window_mass = second_positive;
}

enum class Combine : u8 {
  Set,               // dst = scale * value
  AddScaled,         // dst += scale * value
  SquareAccumulate,  // dst = dst*dst + value*value, for a gradient magnitude in one pass
};

// Horizontal pass, edges extended by clamping. `dst` never aliases `src` here.
//
// The middle of the row accumulates one tap at a time across x rather than summing the taps at
// one x: what the compiler then sees is a row of independent lanes instead of a float reduction
// it is not allowed to reorder, and the loop vectorizes. Written the other way round the whole
// metric runs about twice as slow.
void convolve_h(const f32* src, u32 w, u32 h, const Kernel1d& k, f32* dst) noexcept {
  const i32 radius = static_cast<i32>(k.radius);
  const i32 width = static_cast<i32>(w);
  const f32* weights = k.weights.data();
  const i32 taps = 2 * radius + 1;
  const i32 left = std::min(radius, width);
  const i32 right = std::max(left, width - radius);
  for (u32 y = 0; y < h; ++y) {
    const f32* row = src + static_cast<usize>(y) * w;
    f32* out = dst + static_cast<usize>(y) * w;
    for (i32 x = 0; x < left; ++x) {
      f32 sum = 0.0f;
      for (i32 t = 0; t < taps; ++t)
        sum += weights[t] * row[std::clamp(x + t - radius, 0, width - 1)];
      out[x] = sum;
    }
    for (i32 x = right; x < width; ++x) {
      f32 sum = 0.0f;
      for (i32 t = 0; t < taps; ++t)
        sum += weights[t] * row[std::clamp(x + t - radius, 0, width - 1)];
      out[x] = sum;
    }
    for (i32 x = left; x < right; ++x)
      out[x] = 0.0f;
    for (i32 t = 0; t < taps; ++t) {
      const f32 weight = weights[t];
      const f32* p = row + t - radius;
      for (i32 x = left; x < right; ++x)
        out[x] += weight * p[x];
    }
  }
}

// Vertical pass, row by row so the reads stay sequential; `scratch` is one row wide.
void convolve_v(const f32* src, u32 w, u32 h, const Kernel1d& k, f32 scale, Combine mode,
                f32* scratch, f32* dst) noexcept {
  const i32 radius = static_cast<i32>(k.radius);
  const i32 height = static_cast<i32>(h);
  const f32* weights = k.weights.data();
  const i32 taps = 2 * radius + 1;
  for (i32 y = 0; y < height; ++y) {
    for (u32 x = 0; x < w; ++x)
      scratch[x] = 0.0f;
    for (i32 t = 0; t < taps; ++t) {
      const f32 weight = weights[t];
      if (weight == 0.0f) continue;
      const i32 sy = std::clamp(y + t - radius, 0, height - 1);
      const f32* row = src + static_cast<usize>(sy) * w;
      for (u32 x = 0; x < w; ++x)
        scratch[x] += weight * row[x];
    }
    f32* out = dst + static_cast<usize>(y) * w;
    switch (mode) {
      case Combine::Set:
        for (u32 x = 0; x < w; ++x)
          out[x] = scale * scratch[x];
        break;
      case Combine::AddScaled:
        for (u32 x = 0; x < w; ++x)
          out[x] += scale * scratch[x];
        break;
      case Combine::SquareAccumulate:
        for (u32 x = 0; x < w; ++x) {
          const f32 v = scratch[x];
          out[x] = out[x] * out[x] + v * v;
        }
        break;
    }
  }
}

// ---- pixel access ------------------------------------------------------------------------

bool comparable(const Image& a, const Image& b) {
  return a.width != 0 && a.height != 0 && a.width == b.width && a.height == b.height &&
         a.channels >= 1 && b.channels >= 1 && a.pixels.size() >= a.width * a.height * a.channels &&
         b.pixels.size() >= b.width * b.height * b.channels;
}

// One pixel's stored (sRGB) channels as bytes; a one- or two-channel image is gray.
void load_rgb8(const Image& image, usize index, u32& r, u32& g, u32& b) noexcept {
  const u8* p = image.pixels.data() + index * image.channels;
  r = p[0];
  if (image.channels >= 3) {
    g = p[1];
    b = p[2];
  } else {
    g = r;
    b = r;
  }
}

f32 luma601(u32 r, u32 g, u32 b) noexcept {
  return 0.299f * static_cast<f32>(r) + 0.587f * static_cast<f32>(g) + 0.114f * static_cast<f32>(b);
}

// ---- the heat map ramp -------------------------------------------------------------------

// Nine stops of a perceptually uniform black-purple-red-yellow ramp (the shape FLIP is
// conventionally shown in), interpolated linearly. Nine is enough: the ramp is smooth and the
// eye is reading a picture, not a lookup table.
constexpr u8 k_ramp[9][3] = {{0, 0, 4},       {24, 15, 61},    {59, 15, 112},
                             {114, 31, 129},  {182, 54, 121},  {241, 96, 93},
                             {254, 159, 109}, {254, 207, 146}, {252, 253, 191}};

}  // namespace

// ---- viewing conditions ------------------------------------------------------------------

f32 pixels_per_degree(u32 width_px, u32 height_px, f32 diagonal_inches, f32 distance_meters) {
  if (width_px == 0 || height_px == 0 || !(diagonal_inches > 0.0f) || !(distance_meters > 0.0f))
    return 0.0f;
  const f32 w = static_cast<f32>(width_px);
  const f32 h = static_cast<f32>(height_px);
  const f32 diagonal_m = diagonal_inches * 0.0254f;
  const f32 screen_width_m = diagonal_m * w / std::sqrt(w * w + h * h);
  // Small-angle form, as FLIP uses: pixels per metre at the screen, times metres per radian at
  // the eye, times radians per degree.
  return distance_meters * (w / screen_width_m) * (k_pi / 180.0f);
}

// ---- PSNR --------------------------------------------------------------------------------

f32 psnr_rgb(const Image& a, const Image& b) {
  if (!comparable(a, b)) return std::numeric_limits<f32>::quiet_NaN();
  const usize count = static_cast<usize>(a.width) * a.height;
  f64 sum = 0.0;
  for (usize i = 0; i < count; ++i) {
    u32 ar = 0;
    u32 ag = 0;
    u32 ab = 0;
    u32 br = 0;
    u32 bg = 0;
    u32 bb = 0;
    load_rgb8(a, i, ar, ag, ab);
    load_rgb8(b, i, br, bg, bb);
    const i32 dr = static_cast<i32>(ar) - static_cast<i32>(br);
    const i32 dg = static_cast<i32>(ag) - static_cast<i32>(bg);
    const i32 db = static_cast<i32>(ab) - static_cast<i32>(bb);
    sum += static_cast<f64>(dr * dr + dg * dg + db * db);
  }
  const f64 mse = sum / static_cast<f64>(count * 3);
  if (mse <= 0.0) return std::numeric_limits<f32>::infinity();
  return static_cast<f32>(10.0 * std::log10(255.0 * 255.0 / mse));
}

// ---- SSIM --------------------------------------------------------------------------------

namespace {

// "Valid" convolution: the output holds only the positions whose whole window is inside the
// image, so the borders, where a window would have to invent data, are not measured.
void convolve_valid(const f32* src, u32 w, u32 h, const f32* weights, u32 radius, f32* row_temp,
                    f32* temp, f32* dst) noexcept {
  const u32 taps = 2 * radius + 1;
  const u32 out_w = w - 2 * radius;
  const u32 out_h = h - 2 * radius;
  for (u32 y = 0; y < h; ++y) {
    const f32* row = src + static_cast<usize>(y) * w;
    f32* out = temp + static_cast<usize>(y) * out_w;
    // One tap at a time across x, for the same reason `convolve_h` does.
    for (u32 x = 0; x < out_w; ++x)
      out[x] = 0.0f;
    for (u32 t = 0; t < taps; ++t) {
      const f32 weight = weights[t];
      const f32* p = row + t;
      for (u32 x = 0; x < out_w; ++x)
        out[x] += weight * p[x];
    }
  }
  for (u32 y = 0; y < out_h; ++y) {
    for (u32 x = 0; x < out_w; ++x)
      row_temp[x] = 0.0f;
    for (u32 t = 0; t < taps; ++t) {
      const f32 weight = weights[t];
      const f32* row = temp + static_cast<usize>(y + t) * out_w;
      for (u32 x = 0; x < out_w; ++x)
        row_temp[x] += weight * row[x];
    }
    f32* out = dst + static_cast<usize>(y) * out_w;
    for (u32 x = 0; x < out_w; ++x)
      out[x] = row_temp[x];
  }
}

}  // namespace

f32 ssim_luma(const Image& a, const Image& b, FloatImage* map) {
  if (map != nullptr) *map = FloatImage{};
  if (!comparable(a, b)) return std::numeric_limits<f32>::quiet_NaN();

  constexpr u32 k_radius = 5;  // an 11x11 window
  constexpr f32 k_sigma = 1.5f;
  constexpr f32 k_c1 = 0.01f * 255.0f * (0.01f * 255.0f);
  constexpr f32 k_c2 = 0.03f * 255.0f * (0.03f * 255.0f);

  const u32 w = a.width;
  const u32 h = a.height;
  const usize count = static_cast<usize>(w) * h;
  Vector<f32> la(static_cast<u32>(count));
  Vector<f32> lb(static_cast<u32>(count));
  for (usize i = 0; i < count; ++i) {
    u32 ar = 0;
    u32 ag = 0;
    u32 ab = 0;
    u32 br = 0;
    u32 bg = 0;
    u32 bb = 0;
    load_rgb8(a, i, ar, ag, ab);
    load_rgb8(b, i, br, bg, bb);
    la[static_cast<u32>(i)] = luma601(ar, ag, ab);
    lb[static_cast<u32>(i)] = luma601(br, bg, bb);
  }

  // An image too small for one window is measured as a single window over all of it, which is
  // the same formula with uniform weights.
  if (w < 2 * k_radius + 1 || h < 2 * k_radius + 1) {
    f32 ma = 0.0f;
    f32 mb = 0.0f;
    for (usize i = 0; i < count; ++i) {
      ma += la[static_cast<u32>(i)];
      mb += lb[static_cast<u32>(i)];
    }
    const f32 n = static_cast<f32>(count);
    ma /= n;
    mb /= n;
    f32 saa = 0.0f;
    f32 sbb = 0.0f;
    f32 sab = 0.0f;
    for (usize i = 0; i < count; ++i) {
      const f32 da = la[static_cast<u32>(i)] - ma;
      const f32 db = lb[static_cast<u32>(i)] - mb;
      saa += da * da;
      sbb += db * db;
      sab += da * db;
    }
    saa /= n;
    sbb /= n;
    sab /= n;
    const f32 value = ((2.0f * ma * mb + k_c1) * (2.0f * sab + k_c2)) /
                      ((ma * ma + mb * mb + k_c1) * (saa + sbb + k_c2));
    if (map != nullptr) {
      map->width = 1;
      map->height = 1;
      map->values.assign(1, value);
    }
    return value;
  }

  constexpr u32 k_taps = 2 * k_radius + 1;
  std::array<f32, k_taps> window{};
  {
    f32 sum = 0.0f;
    for (u32 i = 0; i < k_taps; ++i) {
      const f32 x = static_cast<f32>(i) - static_cast<f32>(k_radius);
      window[i] = std::exp(-x * x / (2.0f * k_sigma * k_sigma));
      sum += window[i];
    }
    for (u32 i = 0; i < k_taps; ++i)
      window[i] /= sum;
  }

  const u32 out_w = w - 2 * k_radius;
  const u32 out_h = h - 2 * k_radius;
  const usize out_count = static_cast<usize>(out_w) * out_h;
  Vector<f32> temp(static_cast<u32>(static_cast<usize>(out_w) * h));
  Vector<f32> row_temp(out_w);
  Vector<f32> product(static_cast<u32>(count));
  Vector<f32> mean_a(static_cast<u32>(out_count));
  Vector<f32> mean_b(static_cast<u32>(out_count));
  Vector<f32> var_a(static_cast<u32>(out_count));
  Vector<f32> var_b(static_cast<u32>(out_count));
  Vector<f32> covar(static_cast<u32>(out_count));

  convolve_valid(la.data(), w, h, window.data(), k_radius, row_temp.data(), temp.data(),
                 mean_a.data());
  convolve_valid(lb.data(), w, h, window.data(), k_radius, row_temp.data(), temp.data(),
                 mean_b.data());
  for (usize i = 0; i < count; ++i)
    product[static_cast<u32>(i)] = la[static_cast<u32>(i)] * la[static_cast<u32>(i)];
  convolve_valid(product.data(), w, h, window.data(), k_radius, row_temp.data(), temp.data(),
                 var_a.data());
  for (usize i = 0; i < count; ++i)
    product[static_cast<u32>(i)] = lb[static_cast<u32>(i)] * lb[static_cast<u32>(i)];
  convolve_valid(product.data(), w, h, window.data(), k_radius, row_temp.data(), temp.data(),
                 var_b.data());
  for (usize i = 0; i < count; ++i)
    product[static_cast<u32>(i)] = la[static_cast<u32>(i)] * lb[static_cast<u32>(i)];
  convolve_valid(product.data(), w, h, window.data(), k_radius, row_temp.data(), temp.data(),
                 covar.data());

  f64 total = 0.0;
  if (map != nullptr) {
    map->width = out_w;
    map->height = out_h;
    map->values.resize(static_cast<u32>(out_count));
  }
  for (usize i = 0; i < out_count; ++i) {
    const u32 j = static_cast<u32>(i);
    const f32 ma = mean_a[j];
    const f32 mb = mean_b[j];
    const f32 saa = var_a[j] - ma * ma;
    const f32 sbb = var_b[j] - mb * mb;
    const f32 sab = covar[j] - ma * mb;
    const f32 value = ((2.0f * ma * mb + k_c1) * (2.0f * sab + k_c2)) /
                      ((ma * ma + mb * mb + k_c1) * (saa + sbb + k_c2));
    if (map != nullptr) map->values[j] = value;
    total += static_cast<f64>(value);
  }
  return static_cast<f32>(total / static_cast<f64>(out_count));
}

// ---- FLIP --------------------------------------------------------------------------------

bool flip(const Image& reference, const Image& test, FloatImage& out, const FlipOptions& options,
          std::string* error) {
  out = FloatImage{};
  if (!comparable(reference, test)) {
    if (error != nullptr)
      *error = "flip: the two images must be the same non-empty size with at least one channel";
    return false;
  }
  const f32 ppd =
      options.pixels_per_degree > 0.0f ? options.pixels_per_degree : k_default_pixels_per_degree;
  const u32 w = reference.width;
  const u32 h = reference.height;
  const usize count = static_cast<usize>(w) * h;
  const u32 n = static_cast<u32>(count);

  // Both images in YCxCz: the opponent space the contrast-sensitivity filter is defined in.
  Vector<f32> ref_y(n);
  Vector<f32> ref_cx(n);
  Vector<f32> ref_cz(n);
  Vector<f32> test_y(n);
  Vector<f32> test_cx(n);
  Vector<f32> test_cz(n);
  {
    const f32* table = srgb_table();
    for (usize i = 0; i < count; ++i) {
      const u32 j = static_cast<u32>(i);
      u32 r8 = 0;
      u32 g8 = 0;
      u32 b8 = 0;
      load_rgb8(reference, i, r8, g8, b8);
      f32 r = table[r8];
      f32 g = table[g8];
      f32 b = table[b8];
      f32 x = k_rgb_to_wxyz[0] * r + k_rgb_to_wxyz[1] * g + k_rgb_to_wxyz[2] * b;
      f32 y = k_rgb_to_wxyz[3] * r + k_rgb_to_wxyz[4] * g + k_rgb_to_wxyz[5] * b;
      f32 z = k_rgb_to_wxyz[6] * r + k_rgb_to_wxyz[7] * g + k_rgb_to_wxyz[8] * b;
      ref_y[j] = 116.0f * y - 16.0f;
      ref_cx[j] = 500.0f * (x - y);
      ref_cz[j] = 200.0f * (y - z);
      load_rgb8(test, i, r8, g8, b8);
      r = table[r8];
      g = table[g8];
      b = table[b8];
      x = k_rgb_to_wxyz[0] * r + k_rgb_to_wxyz[1] * g + k_rgb_to_wxyz[2] * b;
      y = k_rgb_to_wxyz[3] * r + k_rgb_to_wxyz[4] * g + k_rgb_to_wxyz[5] * b;
      z = k_rgb_to_wxyz[6] * r + k_rgb_to_wxyz[7] * g + k_rgb_to_wxyz[8] * b;
      test_y[j] = 116.0f * y - 16.0f;
      test_cx[j] = 500.0f * (x - y);
      test_cz[j] = 200.0f * (y - z);
    }
  }

  Vector<f32> scratch(w);
  Vector<f32> temp(n);
  Vector<f32> mag_ref(n);
  Vector<f32> mag_test(n);
  Vector<f32> feature(n);

  // Feature difference: edges and points, from the achromatic channel before it is filtered.
  // The detectors are linear, so running them on the YCxCz luminance and scaling by 1/116 is
  // the same as running them on the relative luminance the paper names.
  {
    Kernel1d smooth;
    Kernel1d first;
    Kernel1d second;
    feature_kernels(ppd, smooth, first, second);
    constexpr f32 k_luma_scale = 1.0f / 116.0f;
    const Kernel1d* derivatives[2] = {&first, &second};
    for (u32 pass = 0; pass < 2; ++pass) {
      const Kernel1d& derivative = *derivatives[pass];
      for (u32 side = 0; side < 2; ++side) {
        const Vector<f32>& source = side == 0 ? ref_y : test_y;
        Vector<f32>& magnitude = side == 0 ? mag_ref : mag_test;
        convolve_h(source.data(), w, h, derivative, temp.data());
        convolve_v(temp.data(), w, h, smooth, 1.0f, Combine::Set, scratch.data(), magnitude.data());
        convolve_h(source.data(), w, h, smooth, temp.data());
        convolve_v(temp.data(), w, h, derivative, 1.0f, Combine::SquareAccumulate, scratch.data(),
                   magnitude.data());
        for (usize i = 0; i < count; ++i) {
          const u32 j = static_cast<u32>(i);
          magnitude[j] = std::sqrt(magnitude[j]) * k_luma_scale;
        }
      }
      for (usize i = 0; i < count; ++i) {
        const u32 j = static_cast<u32>(i);
        const f32 difference = std::abs(mag_ref[j] - mag_test[j]);
        feature[j] = pass == 0 ? difference : std::max(feature[j], difference);
      }
    }
    // The feature exponent is 1/2, so this is a square root rather than a call to pow.
    static_assert(k_gqf == 0.5f, "the feature exponent is no longer a square root");
    constexpr f32 k_inv_sqrt2 = 0.70710678f;
    for (usize i = 0; i < count; ++i) {
      const u32 j = static_cast<u32>(i);
      feature[j] = std::clamp(std::sqrt(k_inv_sqrt2 * feature[j]), 0.0f, 1.0f);
    }
  }
  mag_test = Vector<f32>{};  // the filter below needs one accumulator, not two

  // Contrast sensitivity: one separable convolution per Gaussian of each channel, blended by
  // the mass each one carries inside its own window, which is the normalized filter.
  {
    Vector<f32>* planes[6] = {&ref_y, &ref_cx, &ref_cz, &test_y, &test_cx, &test_cz};
    for (u32 index = 0; index < 6; ++index) {
      const u32 channel = index % 3;
      const CsfTerm& term0 = k_csf[channel][0];
      const CsfTerm& term1 = k_csf[channel][1];
      const Kernel1d kernel0 = csf_kernel(term0.scale, ppd);
      const f32 mass0 = term0.amplitude * std::sqrt(k_pi / term0.scale) * kernel0.window_mass *
                        kernel0.window_mass;
      f32 mass1 = 0.0f;
      Kernel1d kernel1;
      if (term1.amplitude > 0.0f) {
        kernel1 = csf_kernel(term1.scale, ppd);
        mass1 = term1.amplitude * std::sqrt(k_pi / term1.scale) * kernel1.window_mass *
                kernel1.window_mass;
      }
      const f32 total = mass0 + mass1;
      Vector<f32>& plane = *planes[index];
      convolve_h(plane.data(), w, h, kernel0, temp.data());
      convolve_v(temp.data(), w, h, kernel0, mass0 / total, Combine::Set, scratch.data(),
                 mag_ref.data());
      if (mass1 > 0.0f) {
        convolve_h(plane.data(), w, h, kernel1, temp.data());
        convolve_v(temp.data(), w, h, kernel1, mass1 / total, Combine::AddScaled, scratch.data(),
                   mag_ref.data());
      }
      for (usize i = 0; i < count; ++i)
        plane[static_cast<u32>(i)] = mag_ref[static_cast<u32>(i)];
    }
  }

  // Color difference, redistributed so that the threshold of noticing lands near 0.95 of the
  // range, then softened where the feature detectors found an edge or a point.
  const f32 cmax = max_color_distance();
  const f32 pccmax = k_gpc * cmax;
  out.width = w;
  out.height = h;
  out.values.resize(n);
  for (usize i = 0; i < count; ++i) {
    const u32 j = static_cast<u32>(i);
    f32 rl = 0.0f;
    f32 ra = 0.0f;
    f32 rb = 0.0f;
    f32 tl = 0.0f;
    f32 ta = 0.0f;
    f32 tb = 0.0f;
    ycc_to_hunt_lab(ref_y[j], ref_cx[j], ref_cz[j], rl, ra, rb);
    ycc_to_hunt_lab(test_y[j], test_cx[j], test_cz[j], tl, ta, tb);
    f32 difference = std::pow(hyab(rl, ra, rb, tl, ta, tb), k_gqc);
    difference = difference < pccmax
                     ? difference * (k_gpt / pccmax)
                     : k_gpt + ((difference - pccmax) / (cmax - pccmax)) * (1.0f - k_gpt);
    difference = std::clamp(difference, 0.0f, 1.0f);
    out.values[j] = difference <= 0.0f ? 0.0f : std::pow(difference, 1.0f - feature[j]);
  }
  return true;
}

// ---- pooling -----------------------------------------------------------------------------

f32 pool_mean(const FloatImage& error) {
  if (error.values.empty()) return 0.0f;
  f64 sum = 0.0;
  for (const f32 v : error.values)
    sum += static_cast<f64>(v);
  return static_cast<f32>(sum / static_cast<f64>(error.values.size()));
}

f32 pool_weighted_mean(const FloatImage& error, const FloatImage& weights) {
  if (error.values.empty() || weights.values.size() != error.values.size()) return 0.0f;
  f64 sum = 0.0;
  f64 total = 0.0;
  for (u32 i = 0; i < error.values.size(); ++i) {
    const f64 w = static_cast<f64>(weights.values[i]);
    sum += w * static_cast<f64>(error.values[i]);
    total += w;
  }
  return total > 0.0 ? static_cast<f32>(sum / total) : 0.0f;
}

f32 pool_percentile(const FloatImage& error, f32 fraction) {
  if (error.values.empty()) return 0.0f;
  Vector<f32> sorted(error.values);
  const f32 clamped = std::clamp(fraction, 0.0f, 1.0f);
  const u32 last = sorted.size() - 1;
  const u32 index = static_cast<u32>(clamped * static_cast<f32>(last) + 0.5f);
  const u32 at = index > last ? last : index;
  std::nth_element(sorted.begin(), sorted.begin() + at, sorted.end());
  return sorted[at];
}

f32 pool_max(const FloatImage& error) {
  f32 worst = 0.0f;
  for (const f32 v : error.values)
    worst = v > worst ? v : worst;
  return worst;
}

void center_weight_map(u32 width, u32 height, const CenterWeightOptions& options, FloatImage& out) {
  out = FloatImage{};
  if (width == 0 || height == 0) return;
  out.width = width;
  out.height = height;
  out.values.resize(static_cast<u32>(static_cast<usize>(width) * height));
  const f32 sigma = options.sigma > 0.0f ? options.sigma : 0.35f;
  const f32 floor = std::clamp(options.floor, 0.0f, 1.0f);
  const f32 inv_two_sigma_sq = 1.0f / (2.0f * sigma * sigma);
  for (u32 y = 0; y < height; ++y) {
    const f32 v = 2.0f * (static_cast<f32>(y) + 0.5f) / static_cast<f32>(height) - 1.0f;
    for (u32 x = 0; x < width; ++x) {
      const f32 u = 2.0f * (static_cast<f32>(x) + 0.5f) / static_cast<f32>(width) - 1.0f;
      const f32 falloff = std::exp(-(u * u + v * v) * inv_two_sigma_sq);
      out.values[y * width + x] = floor + (1.0f - floor) * falloff;
    }
  }
}

bool flip_heat_map(const FloatImage& error, Image& out) {
  out = Image{};
  if (error.values.empty() || error.width == 0 || error.height == 0) return false;
  const usize count = static_cast<usize>(error.width) * error.height;
  if (error.values.size() < count) return false;
  out.width = error.width;
  out.height = error.height;
  out.channels = 3;
  out.pixels.resize(static_cast<u32>(count * 3));
  constexpr f32 k_last_stop = 8.0f;  // nine stops, eight intervals
  for (usize i = 0; i < count; ++i) {
    const f32 t = std::clamp(error.values[static_cast<u32>(i)], 0.0f, 1.0f) * k_last_stop;
    const u32 stop = std::min(static_cast<u32>(t), 7u);
    const f32 fraction = t - static_cast<f32>(stop);
    for (u32 c = 0; c < 3; ++c) {
      const f32 lo = static_cast<f32>(k_ramp[stop][c]);
      const f32 hi = static_cast<f32>(k_ramp[stop + 1][c]);
      const f32 value = lo + (hi - lo) * fraction;
      out.pixels[static_cast<u32>(i * 3 + c)] =
          static_cast<u8>(std::clamp(value + 0.5f, 0.0f, 255.0f));
    }
  }
  return true;
}

// ---- everything at once ------------------------------------------------------------------

bool compare_images(const Image& a, const Image& b, const MetricsOptions& options,
                    ImageMetrics& out, FloatImage* flip_map, std::string* error) {
  out = ImageMetrics{};
  FloatImage local;
  FloatImage& map = flip_map != nullptr ? *flip_map : local;
  FlipOptions flip_options;
  flip_options.pixels_per_degree = options.pixels_per_degree;
  if (!flip(a, b, map, flip_options, error)) return false;
  out.psnr = psnr_rgb(a, b);
  out.ssim = ssim_luma(a, b);
  out.flip_mean = pool_mean(map);
  out.flip_weighted_mean =
      options.weights != nullptr ? pool_weighted_mean(map, *options.weights) : out.flip_mean;
  out.flip_percentile = pool_percentile(map, options.percentile);
  out.flip_max = pool_max(map);
  return true;
}

}  // namespace engine::image
