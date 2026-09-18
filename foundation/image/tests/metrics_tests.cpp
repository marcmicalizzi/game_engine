// PSNR, SSIM, and FLIP: the degenerate cases that have exact answers, an additive-noise pair
// whose PSNR is known analytically, and the orderings a perceptual metric has to get right —
// a shifted edge beats a flat one-level change, and the same damage counts for less in the
// periphery than in the middle once the center weighting is applied.
#include <foundation/image/metrics.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

using namespace engine;

namespace {

image::Image make_rgba(u32 width, u32 height) {
  image::Image out;
  out.width = width;
  out.height = height;
  out.channels = 4;
  out.pixels.resize(width * height * 4);
  return out;
}

// A picture with structure in it: a diagonal gradient, a checker, and a bright disc, so the
// filters and the feature detectors all have something to find.
image::Image detailed(u32 width, u32 height) {
  image::Image out = make_rgba(width, height);
  for (u32 y = 0; y < height; ++y) {
    for (u32 x = 0; x < width; ++x) {
      const u32 i = (y * width + x) * 4;
      const u32 checker = ((x / 4 + y / 4) % 2 != 0) ? 60u : 0u;
      const f32 dx = static_cast<f32>(x) - static_cast<f32>(width) * 0.5f;
      const f32 dy = static_cast<f32>(y) - static_cast<f32>(height) * 0.5f;
      const bool disc = dx * dx + dy * dy < 100.0f;
      out.pixels[i + 0] = static_cast<u8>((x * 3 + checker + (disc ? 120u : 0u)) & 0xffu);
      out.pixels[i + 1] = static_cast<u8>((y * 5 + checker) & 0xffu);
      out.pixels[i + 2] = static_cast<u8>((x + y + checker) & 0xffu);
      out.pixels[i + 3] = u8{255};
    }
  }
  return out;
}

// A four-pixel checker of two well-separated grays, which is the same pattern wherever a block
// of it is taken from, as long as the block starts on a multiple of eight.
image::Image checker(u32 width, u32 height) {
  image::Image out = make_rgba(width, height);
  for (u32 y = 0; y < height; ++y) {
    for (u32 x = 0; x < width; ++x) {
      const u32 i = (y * width + x) * 4;
      const u8 value = static_cast<u8>(((x / 4 + y / 4) % 2 != 0) ? 220 : 40);
      out.pixels[i + 0] = value;
      out.pixels[i + 1] = value;
      out.pixels[i + 2] = value;
      out.pixels[i + 3] = u8{255};
    }
  }
  return out;
}

image::Image solid(u32 width, u32 height, u8 value) {
  image::Image out = make_rgba(width, height);
  for (u32 i = 0; i < width * height; ++i) {
    out.pixels[i * 4 + 0] = value;
    out.pixels[i * 4 + 1] = value;
    out.pixels[i * 4 + 2] = value;
    out.pixels[i * 4 + 3] = u8{255};
  }
  return out;
}

// Replaces a square with its own average, which is a blur strong enough to see and weak enough
// that the metric has to be perceptual to notice where it happened.
void flatten_block(image::Image& image, u32 x0, u32 y0, u32 size) {
  for (u32 c = 0; c < 3; ++c) {
    u32 sum = 0;
    for (u32 y = y0; y < y0 + size; ++y) {
      for (u32 x = x0; x < x0 + size; ++x)
        sum += image.pixels[(y * image.width + x) * 4 + c];
    }
    const u8 mean = static_cast<u8>(sum / (size * size));
    for (u32 y = y0; y < y0 + size; ++y) {
      for (u32 x = x0; x < x0 + size; ++x)
        image.pixels[(y * image.width + x) * 4 + c] = mean;
    }
  }
}

}  // namespace

TEST_CASE("metrics: identical images are perfect by every measure") {
  const image::Image a = detailed(64, 48);
  const image::Image b = a;

  CHECK(std::isinf(image::psnr_rgb(a, b)));
  CHECK(image::ssim_luma(a, b) == doctest::Approx(1.0).epsilon(1e-6));

  image::FloatImage error;
  std::string message;
  REQUIRE_MESSAGE(image::flip(a, b, error, image::FlipOptions{}, &message), message);
  CHECK(error.width == a.width);
  CHECK(error.height == a.height);
  CHECK(error.values.size() == a.width * a.height);
  CHECK(image::pool_mean(error) == doctest::Approx(0.0).epsilon(1e-9));
  CHECK(image::pool_max(error) == doctest::Approx(0.0).epsilon(1e-9));
  CHECK(image::pool_percentile(error) == doctest::Approx(0.0).epsilon(1e-9));
}

TEST_CASE("metrics: additive noise gives the analytic PSNR, and mismatched sizes give a NaN") {
  constexpr u32 k_width = 40;
  constexpr u32 k_height = 24;
  constexpr i32 k_delta = 4;
  image::Image a = make_rgba(k_width, k_height);
  image::Image b = make_rgba(k_width, k_height);
  for (u32 y = 0; y < k_height; ++y) {
    for (u32 x = 0; x < k_width; ++x) {
      const u32 i = (y * k_width + x) * 4;
      // Mid-range, so adding and subtracting the delta never clamps and the error is exact.
      const u8 base = static_cast<u8>(100 + (x * 7 + y * 13) % 50);
      const i32 sign = ((x + y) % 2 == 0) ? 1 : -1;
      for (u32 c = 0; c < 3; ++c) {
        a.pixels[i + c] = base;
        b.pixels[i + c] = static_cast<u8>(static_cast<i32>(base) + sign * k_delta);
      }
      a.pixels[i + 3] = u8{255};
      b.pixels[i + 3] = u8{255};
    }
  }
  // Every channel is off by exactly four, so the mean squared error is sixteen.
  const f64 expected = 10.0 * std::log10(255.0 * 255.0 / 16.0);
  CHECK(image::psnr_rgb(a, b) == doctest::Approx(expected).epsilon(1e-5));

  const image::Image other = make_rgba(k_width + 1, k_height);
  CHECK(std::isnan(image::psnr_rgb(a, other)));
  CHECK(std::isnan(image::ssim_luma(a, other)));
  image::FloatImage error;
  std::string message;
  CHECK_FALSE(image::flip(a, other, error, image::FlipOptions{}, &message));
  CHECK_FALSE(message.empty());
  CHECK(error.values.empty());
}

TEST_CASE("metrics: SSIM is symmetric, bounded, and drops when structure is destroyed") {
  const image::Image a = detailed(64, 64);
  image::Image b = a;
  flatten_block(b, 16, 16, 24);

  const f32 forward = image::ssim_luma(a, b);
  const f32 backward = image::ssim_luma(b, a);
  CHECK(forward == doctest::Approx(backward).epsilon(1e-6));
  CHECK(forward < 1.0f);
  CHECK(forward > -1.0f);
  CHECK(forward > image::ssim_luma(a, solid(64, 64, 128)));

  image::FloatImage map;
  const f32 pooled = image::ssim_luma(a, b, &map);
  CHECK(map.width == 64 - 10);
  CHECK(map.height == 64 - 10);
  CHECK(map.values.size() == map.width * map.height);
  CHECK(image::pool_mean(map) == doctest::Approx(pooled).epsilon(1e-5));

  // Smaller than one window: measured as a single window rather than refused.
  const image::Image tiny = detailed(6, 6);
  CHECK(image::ssim_luma(tiny, tiny) == doctest::Approx(1.0).epsilon(1e-6));
}

TEST_CASE("metrics: FLIP ranks a shifted edge above a one-level change of a flat field") {
  constexpr u32 k_size = 32;
  image::Image edge_a = make_rgba(k_size, k_size);
  image::Image edge_b = make_rgba(k_size, k_size);
  for (u32 y = 0; y < k_size; ++y) {
    for (u32 x = 0; x < k_size; ++x) {
      const u32 i = (y * k_size + x) * 4;
      const u8 left = static_cast<u8>(x < k_size / 2 ? 16 : 240);
      const u8 shifted = static_cast<u8>(x < k_size / 2 + 1 ? 16 : 240);
      for (u32 c = 0; c < 3; ++c) {
        edge_a.pixels[i + c] = left;
        edge_b.pixels[i + c] = shifted;
      }
      edge_a.pixels[i + 3] = u8{255};
      edge_b.pixels[i + 3] = u8{255};
    }
  }

  image::FloatImage edge_error;
  image::FloatImage flat_error;
  REQUIRE(image::flip(edge_a, edge_b, edge_error));
  REQUIRE(image::flip(solid(k_size, k_size, 128), solid(k_size, k_size, 129), flat_error));

  // Measured: the shifted edge pools to 0.096 with a worst pixel of 0.74, the one-level flat
  // change to 0.0299 everywhere -- which is what the paper's numbers say by hand for a pair of
  // grays 0.39 of L* apart, so the color pipeline and the redistribution are pinned here too.
  const f32 edge_mean = image::pool_mean(edge_error);
  const f32 flat_mean = image::pool_mean(flat_error);
  CHECK(edge_mean > 3.0f * flat_mean);
  CHECK(image::pool_max(edge_error) > 10.0f * image::pool_max(flat_error));
  CHECK(image::pool_max(edge_error) > 0.5f);
  // A one-level change over a whole flat field is close to invisible, and FLIP says so.
  CHECK(flat_mean == doctest::Approx(0.0299).epsilon(0.02));
  CHECK(image::pool_percentile(edge_error, 0.95f) >= image::pool_mean(edge_error));
}

TEST_CASE("metrics: FLIP is symmetric and blind to a change both images make") {
  const image::Image a = detailed(48, 40);
  image::Image b = a;
  flatten_block(b, 8, 8, 16);

  image::FloatImage forward;
  image::FloatImage backward;
  REQUIRE(image::flip(a, b, forward));
  REQUIRE(image::flip(b, a, backward));
  REQUIRE(forward.values.size() == backward.values.size());
  f32 worst = 0.0f;
  for (u32 i = 0; i < forward.values.size(); ++i)
    worst = std::max(worst, std::abs(forward.values[i] - backward.values[i]));
  CHECK(worst == doctest::Approx(0.0).epsilon(1e-6));

  // The same global change to both pictures — here the red and blue channels swapped — leaves
  // two identical images identical, whatever the color difference does to any one of them.
  image::Image swapped_a = a;
  image::Image swapped_b = a;
  for (u32 i = 0; i < a.width * a.height; ++i) {
    std::swap(swapped_a.pixels[i * 4 + 0], swapped_a.pixels[i * 4 + 2]);
    std::swap(swapped_b.pixels[i * 4 + 0], swapped_b.pixels[i * 4 + 2]);
  }
  image::FloatImage swapped_error;
  REQUIRE(image::flip(swapped_a, swapped_b, swapped_error));
  CHECK(image::pool_max(swapped_error) == doctest::Approx(0.0).epsilon(1e-9));
}

TEST_CASE("metrics: the center weighting makes a peripheral regression weigh less") {
  constexpr u32 k_size = 128;
  constexpr u32 k_block = 24;
  const image::Image base = checker(k_size, k_size);
  image::Image center = base;
  image::Image corner = base;
  // Both blocks start on a multiple of eight, so the damaged content is the same pattern.
  flatten_block(center, 52, 52, k_block);
  flatten_block(corner, 4, 4, k_block);

  image::FloatImage center_error;
  image::FloatImage corner_error;
  REQUIRE(image::flip(base, center, center_error));
  REQUIRE(image::flip(base, corner, corner_error));

  image::FloatImage weights;
  image::center_weight_map(k_size, k_size, image::CenterWeightOptions{}, weights);
  REQUIRE(weights.width == k_size);
  REQUIRE(weights.height == k_size);
  CHECK(weights.at(k_size / 2, k_size / 2) == doctest::Approx(1.0).epsilon(1e-3));
  CHECK(weights.at(0, 0) < 0.1f);
  CHECK(weights.at(0, 0) > 0.0f);

  const f32 center_weighted = image::pool_weighted_mean(center_error, weights);
  const f32 corner_weighted = image::pool_weighted_mean(corner_error, weights);
  const f32 center_mean = image::pool_mean(center_error);
  const f32 corner_mean = image::pool_mean(corner_error);
  // Measured: 0.0186 unweighted either way, 0.0717 against 0.0051 once the attention region
  // is applied -- fourteen times, which is the point of the rule.
  CHECK(center_weighted > 8.0f * corner_weighted);
  CHECK(center_mean > 0.0f);
  CHECK(corner_mean == doctest::Approx(center_mean).epsilon(0.05));
}

TEST_CASE("metrics: pooling, the viewing-density helper, and the heat map") {
  image::FloatImage error;
  error.width = 4;
  error.height = 1;
  error.values.assign(4, 0.0f);
  error.values[0] = 0.0f;
  error.values[1] = 0.25f;
  error.values[2] = 0.5f;
  error.values[3] = 1.0f;
  CHECK(image::pool_mean(error) == doctest::Approx(0.4375).epsilon(1e-6));
  CHECK(image::pool_max(error) == doctest::Approx(1.0).epsilon(1e-6));
  CHECK(image::pool_percentile(error, 1.0f) == doctest::Approx(1.0).epsilon(1e-6));
  CHECK(image::pool_percentile(error, 0.0f) == doctest::Approx(0.0).epsilon(1e-6));

  image::FloatImage weights;
  weights.width = 4;
  weights.height = 1;
  weights.values.assign(4, 0.0f);
  weights.values[3] = 1.0f;
  CHECK(image::pool_weighted_mean(error, weights) == doctest::Approx(1.0).epsilon(1e-6));
  image::FloatImage mismatched;
  mismatched.width = 2;
  mismatched.height = 1;
  mismatched.values.assign(2, 1.0f);
  CHECK(image::pool_weighted_mean(error, mismatched) == doctest::Approx(0.0).epsilon(1e-9));

  // Twice as far away is twice as many pixels per degree; nonsense is zero.
  const f32 near = image::pixels_per_degree(3840, 2160, 27.0f, 0.7f);
  const f32 far = image::pixels_per_degree(3840, 2160, 27.0f, 1.4f);
  CHECK(near > 40.0f);
  CHECK(near < 120.0f);
  CHECK(far == doctest::Approx(2.0f * near).epsilon(1e-4));
  CHECK(image::pixels_per_degree(0, 2160, 27.0f, 0.7f) == 0.0f);
  CHECK(image::pixels_per_degree(3840, 2160, 27.0f, 0.0f) == 0.0f);
  // A denser display sees more of the difference, so it filters less of it away.
  const image::Image a = detailed(64, 64);
  image::Image b = a;
  flatten_block(b, 20, 20, 16);
  image::FloatImage coarse;
  image::FloatImage fine;
  REQUIRE(image::flip(a, b, coarse, image::FlipOptions{20.0f}));
  REQUIRE(image::flip(a, b, fine, image::FlipOptions{120.0f}));
  CHECK(image::pool_mean(fine) > image::pool_mean(coarse));

  image::FloatImage ramp;
  ramp.width = 3;
  ramp.height = 1;
  ramp.values.assign(3, 0.0f);
  ramp.values[1] = 0.5f;
  ramp.values[2] = 1.0f;
  image::Image heat;
  REQUIRE(image::flip_heat_map(ramp, heat));
  CHECK(heat.width == 3);
  CHECK(heat.height == 1);
  CHECK(heat.channels == 3);
  CHECK(heat.pixels.size() == 9);
  CHECK(heat.pixels[0] < 8);    // no error is nearly black
  CHECK(heat.pixels[6] > 200);  // full error is pale
  CHECK(heat.pixels[8] > 150);
  CHECK(heat.pixels[3] > heat.pixels[0]);
  CHECK_FALSE(image::flip_heat_map(image::FloatImage{}, heat));
}

TEST_CASE("metrics: compare_images reports the whole set from one pass") {
  const image::Image a = detailed(80, 60);
  image::Image b = a;
  flatten_block(b, 30, 20, 20);

  image::FloatImage weights;
  image::center_weight_map(80, 60, image::CenterWeightOptions{}, weights);
  image::MetricsOptions options;
  options.weights = &weights;

  image::ImageMetrics metrics;
  image::FloatImage map;
  std::string message;
  REQUIRE_MESSAGE(image::compare_images(a, b, options, metrics, &map, &message), message);
  CHECK(map.width == 80);
  CHECK(map.height == 60);
  CHECK(metrics.psnr == doctest::Approx(image::psnr_rgb(a, b)).epsilon(1e-5));
  CHECK(metrics.ssim == doctest::Approx(image::ssim_luma(a, b)).epsilon(1e-5));
  CHECK(metrics.flip_mean == doctest::Approx(image::pool_mean(map)).epsilon(1e-6));
  CHECK(metrics.flip_max >= metrics.flip_percentile);
  CHECK(metrics.flip_percentile >= 0.0f);
  CHECK(metrics.flip_weighted_mean > 0.0f);

  image::MetricsOptions unweighted;
  image::ImageMetrics plain;
  REQUIRE(image::compare_images(a, b, unweighted, plain));
  CHECK(plain.flip_weighted_mean == doctest::Approx(plain.flip_mean).epsilon(1e-6));
}
