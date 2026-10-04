// The output encode's CPU side (display.h; display.slang is its GPU side; ADR-0052), with no
// device: the code steps a target has, the noise's range, distribution and spread, the dither's
// ends, and a 10-bit capture's reduction to 8 bits. The renderer's banding test holds the GPU's
// picture to these functions; this file holds the functions to what they promise.
#include <domain/gfx/capture.h>
#include <domain/gfx/display.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

using namespace engine;

TEST_CASE("display: a target's code steps and bits") {
  CHECK(gfx::display_steps(gfx::Format::R8G8B8A8Unorm) == 255);
  CHECK(gfx::display_steps(gfx::Format::B8G8R8A8Unorm) == 255);
  CHECK(gfx::display_steps(gfx::Format::A2B10G10R10Unorm) == 1023);
  CHECK(gfx::display_steps(gfx::Format::A2R10G10B10Unorm) == 1023);
  // A float target holds no steps, and an `_Srgb` one is never dithered (display.h says why).
  CHECK(gfx::display_steps(gfx::Format::R32G32B32A32Sfloat) == 0);
  CHECK(gfx::display_steps(gfx::Format::R16G16B16A16Sfloat) == 0);
  CHECK(gfx::display_steps(gfx::Format::B8G8R8A8Srgb) == 0);
  CHECK(gfx::display_bits(gfx::Format::B8G8R8A8Srgb) == 8);
  CHECK(gfx::display_bits(gfx::Format::A2B10G10R10Unorm) == 10);
  CHECK(gfx::display_bits(gfx::Format::R32G32B32A32Sfloat) == 32);
  CHECK(std::strcmp(gfx::format_name(gfx::Format::A2B10G10R10Unorm), "A2B10G10R10Unorm") == 0);
  CHECK(std::strcmp(gfx::format_name(gfx::Format::D32Sfloat), "other") == 0);
}

TEST_CASE("display: the noise is triangular on [-1, 1), and spread out in every window") {
  // Over the first 512 x 512 pixels and over a block at the owner's surround's far corner (11520
  // wide): the range, the mean, the triangular distribution's quarters, and — what the choice over
  // white noise rested on — how little of it survives an 8 x 8 average, against the 1/8 of its
  // deviation white noise leaves.
  for (const u32 origin : {0u, 11008u}) {
    CAPTURE(origin);
    constexpr u32 k_side = 512;
    f64 sum = 0.0;
    f64 squares = 0.0;
    u64 inner = 0;  // |n| < 0.5: three quarters of a triangular distribution
    f32 lo = 1.0f;
    f32 hi = -1.0f;
    for (u32 y = 0; y < k_side; ++y) {
      for (u32 x = 0; x < k_side; ++x) {
        const f32 n = gfx::display_dither(origin + x, origin / 4 + y);
        lo = std::fmin(lo, n);
        hi = std::fmax(hi, n);
        sum += static_cast<f64>(n);
        squares += static_cast<f64>(n) * static_cast<f64>(n);
        if (std::fabs(n) < 0.5f) ++inner;
      }
    }
    const f64 count = static_cast<f64>(k_side) * k_side;
    const f64 mean = sum / count;
    const f64 deviation = std::sqrt(squares / count - mean * mean);
    CHECK(lo >= -1.0f);
    CHECK(hi < 1.0f);
    CHECK(std::fabs(mean) < 0.01);
    CHECK(deviation == doctest::Approx(1.0 / std::sqrt(6.0)).epsilon(0.02));  // triangular
    CHECK(static_cast<f64>(inner) / count == doctest::Approx(0.75).epsilon(0.02));
    f64 block_squares = 0.0;
    u32 blocks = 0;
    for (u32 by = 0; by < k_side; by += 8) {
      for (u32 bx = 0; bx < k_side; bx += 8) {
        f64 block = 0.0;
        for (u32 y = by; y < by + 8; ++y)
          for (u32 x = bx; x < bx + 8; ++x) {
            block += static_cast<f64>(gfx::display_dither(origin + x, origin / 4 + y));
          }
        block /= 64.0;
        block_squares += block * block;
        ++blocks;
      }
    }
    const f64 block_rms = std::sqrt(block_squares / blocks);
    MESSAGE("noise deviation " << deviation << ", 8x8 mean RMS " << block_rms << " (white noise "
                               << deviation / 8.0 << ")");
    CHECK(block_rms < 0.5 * deviation / 8.0);  // measured: 0.018 against white noise's 0.051
  }
}

TEST_CASE("display: the dither keeps black, white and every mean, and moves a value one step") {
  for (const u32 steps : {255u, 1023u}) {
    CAPTURE(steps);
    const f32 s = static_cast<f32>(steps);
    for (u32 y = 0; y < 16; ++y) {
      for (u32 x = 0; x < 16; ++x) {
        CHECK(gfx::display_dithered(0.0f, x, y, steps) == 0.0f);
        CHECK(gfx::display_dithered(1.0f, x, y, steps) == 1.0f);
        CHECK(gfx::display_dithered(-0.25f, x, y, steps) == 0.0f);  // clamped first
        CHECK(gfx::display_dithered(1.5f, x, y, steps) == 1.0f);
      }
    }
    // A value between two codes rounds to one of the codes within a step of it, and over a field of
    // pixels to its own value on average: what keeps a gradient's mean where it was.
    for (const f32 v : {0.5f / s, 3.3f / s, 0.4321f, 1.0f - 2.7f / s}) {
      CAPTURE(v);
      f64 sum = 0.0;
      u32 count = 0;
      u32 outside = 0;  // out of [0, 1], or more than one step from the value
      for (u32 y = 0; y < 256; ++y) {
        for (u32 x = 0; x < 256; ++x) {
          const f32 d = gfx::display_dithered(v, x, y, steps);
          if (d < 0.0f || d > 1.0f || std::fabs(d - v) * s > 1.0001f) ++outside;
          sum += static_cast<f64>(std::floor(d * s + 0.5f));
          ++count;
        }
      }
      CHECK(outside == 0);
      CHECK(sum / count / static_cast<f64>(s) ==
            doctest::Approx(static_cast<f64>(v)).epsilon(0.01));
    }
    // No dither at zero steps: the value as it came.
    CHECK(gfx::display_dithered(0.4321f, 3, 4, 0) == 0.4321f);
  }
}

namespace {

gfx::Capture ten_bit(gfx::Format format, std::initializer_list<u32> words) {
  gfx::Capture c;
  c.width = static_cast<u32>(words.size());
  c.height = 1;
  c.format = format;
  c.bytes_per_pixel = 4;
  c.bytes.resize(c.width * 4);
  u32 i = 0;
  for (const u32 w : words)
    std::memcpy(c.bytes.data() + 4 * i++, &w, 4);
  return c;
}

u32 word_rgb(u32 low, u32 mid, u32 high) { return low | (mid << 10) | (high << 20) | (3u << 30); }

}  // namespace

TEST_CASE("display: a 10-bit capture reduces to 8 bits, rounded or through the dither") {
  // A2B10G10R10 is red in the low bits; A2R10G10B10 blue.
  const gfx::Capture abgr =
      ten_bit(gfx::Format::A2B10G10R10Unorm, {word_rgb(1023, 0, 512), word_rgb(4, 2, 1)});
  Vector<u8> rgba;
  REQUIRE(gfx::capture_to_rgba8(abgr, rgba));
  CHECK(rgba[0] == 255);
  CHECK(rgba[1] == 0);
  CHECK(rgba[2] == 128);  // 512 * 255 / 1023 = 127.6
  CHECK(rgba[3] == 255);
  CHECK(rgba[4] == 1);  // 4 * 255 / 1023 = 1.0
  CHECK(rgba[5] == 0);
  CHECK(rgba[6] == 0);
  const gfx::Capture argb = ten_bit(gfx::Format::A2R10G10B10Unorm, {word_rgb(1023, 0, 512)});
  REQUIRE(gfx::capture_to_rgba8(argb, rgba));
  CHECK(rgba[0] == 128);
  CHECK(rgba[2] == 255);
  // Alpha through when asked: two bits to a byte.
  REQUIRE(gfx::capture_to_rgba8(ten_bit(gfx::Format::A2B10G10R10Unorm, {1u << 30}), rgba, false));
  CHECK(rgba[3] == 85);

  // A 10-bit ramp of 1024 pixels: rounded, it is 256 runs of four (bands, at 8 bits); dithered,
  // every byte is within one of the rounded one, black and white are kept, and the mean of every
  // run of sixteen pixels stays within a third of a code of the ramp's own.
  gfx::Capture ramp;
  ramp.width = 1024;
  ramp.height = 1;
  ramp.format = gfx::Format::A2B10G10R10Unorm;
  ramp.bytes_per_pixel = 4;
  ramp.bytes.resize(1024 * 4);
  for (u32 i = 0; i < 1024; ++i) {
    const u32 w = word_rgb(i, i, i);
    std::memcpy(ramp.bytes.data() + 4 * i, &w, 4);
  }
  Vector<u8> plain;
  Vector<u8> dithered;
  REQUIRE(gfx::capture_to_rgba8(ramp, plain, true, false));
  REQUIRE(gfx::capture_to_rgba8(ramp, dithered, true, true));
  CHECK(dithered[0] == 0);
  CHECK(dithered[4 * 1023] == 255);
  u32 differ = 0;
  u32 far = 0;
  for (u32 i = 0; i < 1024; ++i) {
    const int d = static_cast<int>(dithered[4 * i]) - static_cast<int>(plain[4 * i]);
    if (std::abs(d) > 1) ++far;
    if (d != 0) ++differ;
  }
  CHECK(far == 0);
  CHECK(differ > 100);
  for (u32 run = 0; run < 1024; run += 16) {
    f64 got = 0.0;
    f64 want = 0.0;
    for (u32 i = run; i < run + 16; ++i) {
      got += static_cast<f64>(dithered[4 * i]);
      want += static_cast<f64>(i) * 255.0 / 1023.0;
    }
    CHECK(std::fabs(got - want) / 16.0 < 0.34);
  }
}
