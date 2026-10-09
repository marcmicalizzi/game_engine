// The output encode's CPU side (display.h; display.slang is its GPU side; ADR-0052), with no
// device: the code steps a target has, the noise's range, distribution and spread, the dither's
// ends, and a 10-bit capture's reduction to 8 bits. The renderer's banding test holds the GPU's
// picture to these functions; this file holds the functions to what they promise.
#include "display_reference.h"

#include <domain/gfx/capture.h>
#include <domain/gfx/display.h>

#include <doctest/doctest.h>

#include <algorithm>
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

// ---- HDR output (E39): display.h against display_reference.h, in double -----------------------

TEST_CASE("display: PQ round-trips in double, and the float encode is the reference's") {
  // The reference against itself: encode then decode is the identity to 1e-6 relative across the
  // whole range (0.001 to 10,000 nits); black is c1^m2, 7e-7, far under a code; 1 is 10,000 nits.
  CHECK(gfx::reference::pq_encode(0.0) < 1e-6);
  CHECK(gfx::reference::pq_decode(gfx::reference::pq_encode(0.0)) < 1e-9);
  CHECK(gfx::reference::pq_encode(10000.0) == doctest::Approx(1.0).epsilon(1e-12));
  CHECK(gfx::reference::pq_decode(1.0) == doctest::Approx(10000.0).epsilon(1e-9));
  // Known points of the curve: 100 nits is 0.508, 1,000 nits 0.752 (ITU-R BT.2100's table).
  CHECK(gfx::reference::pq_encode(100.0) == doctest::Approx(0.5081).epsilon(2e-4));
  CHECK(gfx::reference::pq_encode(1000.0) == doctest::Approx(0.7518).epsilon(2e-4));
  f64 worst = 0.0;
  for (f64 nits = 0.001; nits <= 10000.0; nits *= 1.05) {
    const f64 back = gfx::reference::pq_decode(gfx::reference::pq_encode(nits));
    worst = std::max(worst, std::fabs(back - nits) / nits);
  }
  MESSAGE("PQ round trip in double, worst relative error " << worst);
  CHECK(worst < 1e-6);
  // The float encode (display.h, display.slang's arithmetic) within a sixteenth of a 10-bit code
  // of the double one everywhere, and its decode within a thousandth relative above a hundredth
  // of a nit: a float is enough for a 10-bit PQ signal.
  f64 encode_worst = 0.0;
  f64 decode_worst = 0.0;
  for (f64 nits = 0.01; nits <= 10000.0; nits *= 1.02) {
    const f64 e = gfx::reference::pq_encode(nits);
    encode_worst = std::max(
        encode_worst, std::fabs(static_cast<f64>(gfx::pq_encode(static_cast<f32>(nits))) - e));
    const f64 d = static_cast<f64>(gfx::pq_decode(static_cast<f32>(e)));
    decode_worst = std::max(decode_worst, std::fabs(d - nits) / nits);
  }
  MESSAGE("float PQ: encode within " << encode_worst * 1023.0 << " of a 10-bit code, decode within "
                                     << decode_worst << " relative");
  CHECK(encode_worst * 1023.0 < 1.0 / 16.0);
  CHECK(decode_worst < 1e-3);
}

TEST_CASE("display: the Rec. 709 to BT.2020 matrix is the primaries', and white stays white") {
  f64 m[3][3];
  gfx::reference::bt709_to_bt2020(m);
  for (u32 r = 0; r < 3; ++r) {
    f64 sum = 0.0;
    f64 float_sum = 0.0;
    for (u32 c = 0; c < 3; ++c) {
      CAPTURE(r);
      CAPTURE(c);
      MESSAGE("reference[" << r << "][" << c << "] = " << m[r][c]);
      CHECK(std::fabs(static_cast<f64>(gfx::k_bt709_to_bt2020[r][c]) - m[r][c]) < 1e-6);
      CHECK(gfx::k_bt709_to_bt2020[r][c] > 0.0f);
      sum += m[r][c];
      float_sum += static_cast<f64>(gfx::k_bt709_to_bt2020[r][c]);
    }
    // White (1, 1, 1) in Rec. 709 is white in BT.2020: every row sums to one.
    CHECK(std::fabs(sum - 1.0) < 1e-9);
    CHECK(std::fabs(float_sum - 1.0) < 1e-6);
  }
}

TEST_CASE(
    "display: the tone curve rises, holds the SDR picture below its knee, and is the SDR "
    "shoulder at a peak of paper white") {
  for (const f32 knee : {0.6f, 1.0f}) {
    for (const f32 ceiling : {1.0f, 2.5f, 5.0f, 10.0f}) {
      CAPTURE(knee);
      CAPTURE(ceiling);
      f32 last = -1.0f;
      for (u32 i = 0; i <= 4000; ++i) {
        const f32 x = static_cast<f32>(i) * 0.01f;  // 0 .. 40 times paper white
        const f32 y = gfx::display_tone(x, knee, ceiling);
        // Monotonic, never past the ceiling (once the knee is below it), and the double's.
        CHECK(y >= last);
        last = y;
        if (knee < ceiling) CHECK(y <= ceiling);
        CHECK(static_cast<f64>(y) ==
              doctest::Approx(gfx::reference::display_tone(static_cast<f64>(x),
                                                           static_cast<f64>(knee),
                                                           static_cast<f64>(ceiling)))
                  .epsilon(1e-5));
        // The SDR picture's curve is the same function with its ceiling at paper white.
        const f32 sdr = gfx::display_tone(x, knee, 1.0f);
        if (x <= knee) {
          // Below the knee — everything the SDR picture shows unrolled — the HDR picture is it.
          CHECK(y == x);
          CHECK(sdr == x);
        }
        // Above it the HDR picture never shows less than the SDR one, whose store clips at its
        // white: it has room to spare.
        CHECK(y >= std::min(sdr, 1.0f));
      }
    }
  }
  // The headroom: the peak over paper white, never below 1.
  CHECK(gfx::display_headroom(1000.0f, 200.0f) == 5.0f);
  CHECK(gfx::display_headroom(100.0f, 200.0f) == 1.0f);
  CHECK(gfx::display_headroom(1000.0f, 0.0f) == 1.0f);
}

TEST_CASE("display: the HDR encodes of one pixel, and an SDR white at paper white") {
  // A white below the knee lands at exactly paper white, in both encodings: HDR10's PQ code for
  // paper white on every channel (white stays white through the matrix), scRGB's paper white over
  // 80 nits.
  const f32 white[3] = {0.5f, 0.5f, 0.5f};
  f32 pq[3];
  gfx::display_encode_hdr(white, 0.6f, gfx::DisplayEncoding::Pq, 200.0f, 1000.0f, pq);
  for (const f32 v : pq)
    CHECK(static_cast<f64>(v) == doctest::Approx(gfx::reference::pq_encode(100.0)).epsilon(1e-5));
  f32 sc[3];
  gfx::display_encode_hdr(white, 0.6f, gfx::DisplayEncoding::ScRgb, 200.0f, 1000.0f, sc);
  for (const f32 v : sc)
    CHECK(static_cast<f64>(v) == doctest::Approx(100.0 / 80.0));
  // Far past the knee a channel approaches the peak and never passes it.
  const f32 sun[3] = {1000.0f, 1000.0f, 1000.0f};
  gfx::display_encode_hdr(sun, 0.6f, gfx::DisplayEncoding::Pq, 200.0f, 1000.0f, pq);
  for (const f32 v : pq) {
    CHECK(gfx::pq_decode(v) <= 1000.5f);
    CHECK(gfx::pq_decode(v) > 990.0f);
  }
  // The present formats' names and encodings.
  CHECK(std::strcmp(gfx::present_format_name(gfx::PresentFormat::Hdr10), "hdr10") == 0);
  CHECK(gfx::present_format_encoding(gfx::PresentFormat::ScRgb) == gfx::DisplayEncoding::ScRgb);
  CHECK(gfx::present_format_encoding(gfx::PresentFormat::Sdr10) == gfx::DisplayEncoding::Sdr);
  CHECK(gfx::display_encoding_format(gfx::DisplayEncoding::Pq) == gfx::Format::A2B10G10R10Unorm);
  CHECK(std::strcmp(gfx::color_space_name(gfx::ColorSpace::Hdr10St2084), "hdr10_st2084") == 0);
  const gfx::SurfaceFormat offers[2] = {
      {gfx::Format::B8G8R8A8Unorm, gfx::ColorSpace::SrgbNonlinear},
      {gfx::Format::A2B10G10R10Unorm, gfx::ColorSpace::Hdr10St2084}};
  CHECK(gfx::describe_surface_formats(offers) ==
        "B8G8R8A8Unorm srgb_nonlinear, A2B10G10R10Unorm hdr10_st2084");
}

// What an EXR capture decodes with (roadmap R79): the linear encoding is the radiance itself, as a
// half holds it; BT.2020 back to Rec. 709 undoes the encode's own matrix; and the ramp's three
// bands are what display.h says.
TEST_CASE("display: the linear encoding, BT.2020 back to Rec. 709, and the ramp") {
  const f32 exposed[3] = {0.25f, 3.5f, 1.0e9f};
  f32 out[3];
  gfx::display_encode_hdr(exposed, 0.6f, gfx::DisplayEncoding::Linear, 200.0f, 1000.0f, out);
  CHECK(out[0] == 0.25f);
  CHECK(out[1] == 3.5f);
  CHECK(out[2] == gfx::k_half_max);
  CHECK(gfx::present_format_encoding(gfx::PresentFormat::Linear) == gfx::DisplayEncoding::Linear);
  CHECK(gfx::display_encoding_format(gfx::DisplayEncoding::Linear) ==
        gfx::Format::R16G16B16A16Sfloat);
  CHECK(std::strcmp(gfx::display_encoding_name(gfx::DisplayEncoding::Linear), "linear") == 0);

  // The inverse matrix: a colour to BT.2020 and back is itself, white stays white, and a pure
  // BT.2020 green is outside Rec. 709 (a negative red and blue).
  const f32 colours[3][3] = {{1.0f, 1.0f, 1.0f}, {0.8f, 0.1f, 0.3f}, {0.02f, 0.5f, 0.9f}};
  for (const auto& c : colours) {
    f32 wide[3];
    for (u32 r = 0; r < 3; ++r) {
      wide[r] = gfx::k_bt709_to_bt2020[r][0] * c[0] + gfx::k_bt709_to_bt2020[r][1] * c[1] +
                gfx::k_bt709_to_bt2020[r][2] * c[2];
    }
    f32 back[3];
    gfx::bt2020_to_bt709(wide, back);
    for (u32 r = 0; r < 3; ++r)
      CHECK(std::fabs(back[r] - c[r]) <= 2.0e-6f);
  }
  const f32 green2020[3] = {0.0f, 1.0f, 0.0f};
  f32 green709[3];
  gfx::bt2020_to_bt709(green2020, green709);
  CHECK(green709[0] < 0.0f);
  CHECK(green709[1] > 1.0f);
  CHECK(green709[2] < 0.0f);

  // The ramp at 1024 x 400: the top half's signal is x / 1023, the third quarter's an eighth of
  // that, the last quarter four stops from white.
  CHECK(gfx::display_ramp(0, 0, 1024, 400) == 0.0f);
  CHECK(std::fabs(std::pow(gfx::display_ramp(511, 10, 1024, 400), 1.0f / 2.2f) - 511.0f / 1023.0f) <
        1.0e-6f);
  CHECK(gfx::display_ramp(1023, 199, 1024, 400) == 1.0f);
  CHECK(std::fabs(std::pow(gfx::display_ramp(1023, 200, 1024, 400), 1.0f / 2.2f) - 0.125f) <
        1.0e-6f);
  CHECK(gfx::display_ramp(0, 300, 1024, 400) == 1.0f);
  CHECK(gfx::display_ramp(1023, 399, 1024, 400) == 16.0f);
}
