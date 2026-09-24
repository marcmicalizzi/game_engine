// The mip chain: its dimensions and count at every kind of size, the sRGB tables against the
// transfer function and against each other, filtering in linear light (a black-and-white checker
// averages to sRGB 188, not to 128), constancy, renormalized normals, odd sides filtered rather
// than dropped, the kernel's own properties, and the same bytes for any thread count.
#include "fixtures.h"

#include <core/jobs/job_system.h>
#include <domain/texture/texture_build.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace engine;
using namespace engine::texture;

namespace {

// The distance between two floats in units of the last place, for finite values of one sign.
u32 ulps(f32 a, f32 b) {
  u32 ia = 0;
  u32 ib = 0;
  std::memcpy(&ia, &a, 4);
  std::memcpy(&ib, &b, 4);
  return ia > ib ? ia - ib : ib - ia;
}

f64 srgb_decode(f64 c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }

jobs::JobSystemConfig four_workers() {
  jobs::JobSystemConfig config;
  config.performance_workers = 4;
  config.efficiency_workers = 1;
  config.pin_threads = false;
  return config;
}

}  // namespace

TEST_CASE("mips: the chain's sides halve, round down and stop at one") {
  struct Case {
    u32 w, h, levels;
  };
  const Case cases[] = {{256, 256, 9}, {300, 200, 9}, {1, 1, 1},      {1, 7, 3},
                        {37, 21, 6},   {4, 1, 3},     {2048, 512, 12}};
  for (const Case& c : cases) {
    Vector<u8> pixels(c.w * c.h * 4, u8{77});
    Vector<MipLevel> chain;
    build_mip_chain(std::span<const u8>(pixels.data(), pixels.size()), c.w, c.h, MipMode::srgb,
                    true, chain);
    REQUIRE(chain.size() == c.levels);
    for (u32 i = 0; i < chain.size(); ++i) {
      CHECK(chain[i].width == texture_level_extent(c.w, i));
      CHECK(chain[i].height == texture_level_extent(c.h, i));
      CHECK(chain[i].rgba.size() == chain[i].width * chain[i].height * 4);
    }
    CHECK(chain.back().width == 1);
    CHECK(chain.back().height == 1);
    Vector<MipLevel> single;
    build_mip_chain(std::span<const u8>(pixels.data(), pixels.size()), c.w, c.h, MipMode::srgb,
                    false, single);
    CHECK(single.size() == 1);
  }
}

TEST_CASE("mips: the sRGB tables are the transfer function, and round-trip every code") {
  const std::span<const f32> decode = srgb_decode_table();
  const std::span<const f32> thresholds = srgb_threshold_table();
  REQUIRE(decode.size() == 256);
  REQUIRE(thresholds.size() == 255);
  u32 worst = 0;
  for (u32 n = 0; n < 256; ++n) {
    const f32 want = static_cast<f32>(srgb_decode(n / 255.0));
    worst = std::max(worst, ulps(decode[n], want));
    CHECK(srgb8_to_linear(static_cast<u8>(n)) == decode[n]);
  }
  for (u32 n = 0; n < 255; ++n) {
    const f32 want = static_cast<f32>(srgb_decode((n + 0.5) / 255.0));
    worst = std::max(worst, ulps(thresholds[n], want));
  }
  // The tables were taken in double precision and rounded once; the C library's `pow` in double
  // and a rounding to float may land an ulp away on some platform, and nothing further.
  CHECK(worst <= 1);
  // Every code lies strictly between the thresholds around it, so decoding and encoding again is
  // the identity — the property that lets level 0 be the source's own bytes.
  for (u32 n = 0; n < 256; ++n) {
    if (n > 0) CHECK(thresholds[n - 1] < decode[n]);
    if (n < 255) CHECK(decode[n] < thresholds[n]);
    CHECK(linear_to_srgb8(decode[n]) == n);
  }
  CHECK(linear_to_srgb8(-1.0f) == 0);
  CHECK(linear_to_srgb8(2.0f) == 255);
  CHECK(linear_to_srgb8(std::nanf("")) == 0);
  // Linear 0.5 is sRGB 0.7354 of full scale, code 187.52: the grey a black-and-white checker
  // averages to in light.
  CHECK(linear_to_srgb8(0.5f) == 188);
}

TEST_CASE("mips: colour is filtered in linear light, data as stored") {
  constexpr u32 n = 64;
  Vector<u8> checker(n * n * 4);
  for (u32 y = 0; y < n; ++y) {
    for (u32 x = 0; x < n; ++x) {
      const u8 v = ((x + y) & 1) == 0 ? 0 : 255;
      u8* t = &checker[(y * n + x) * 4];
      t[0] = t[1] = t[2] = v;
      t[3] = v;  // alpha is always linear
    }
  }
  Vector<MipLevel> srgb;
  build_mip_chain(std::span<const u8>(checker.data(), checker.size()), n, n, MipMode::srgb, true,
                  srgb);
  Vector<MipLevel> linear;
  build_mip_chain(std::span<const u8>(checker.data(), checker.size()), n, n, MipMode::linear, true,
                  linear);
  // Level 0 is the source's bytes in both modes.
  CHECK(std::memcmp(srgb[0].rgba.data(), checker.data(), checker.size()) == 0);
  CHECK(std::memcmp(linear[0].rgba.data(), checker.data(), checker.size()) == 0);
  // Every level past it is the checker's mean: half the light, which sRGB stores as 188, and half
  // the value, which data stores as 128 — where a filter in gamma space would have stored 128 for
  // both. Away from the clamped edges every tap pairs a black texel with a white one of the same
  // weight, so the mean is exact but for the float rounding of the normalized weights, which can
  // put a value that is exactly half a code on either side of it: hence one code of slack.
  auto near = [](u8 got, i32 want) { return std::abs(static_cast<i32>(got) - want) <= 1; };
  for (u32 level = 1; level < 4; ++level) {
    const MipLevel& s = srgb[level];
    const MipLevel& l = linear[level];
    for (u32 y = 2; y + 2 < s.height; ++y) {
      for (u32 x = 2; x + 2 < s.width; ++x) {
        const u8* st = &s.rgba[(y * s.width + x) * 4];
        const u8* lt = &l.rgba[(y * l.width + x) * 4];
        CHECK(near(st[0], 188));
        CHECK(near(st[3], 128));
        CHECK(near(lt[0], 128));
        CHECK(near(lt[3], 128));
      }
    }
  }
}

TEST_CASE("mips: a constant image stays that constant at every level, in every mode") {
  for (const MipMode mode : {MipMode::srgb, MipMode::linear}) {
    for (const u32 value : {0u, 1u, 77u, 128u, 254u, 255u}) {
      Vector<u8> pixels(19 * 11 * 4, static_cast<u8>(value));
      Vector<MipLevel> chain;
      build_mip_chain(std::span<const u8>(pixels.data(), pixels.size()), 19, 11, mode, true, chain);
      for (const MipLevel& level : chain) {
        for (const u8 byte : level.rgba)
          CHECK(byte == value);
      }
    }
  }
  // A flat normal map is +z everywhere, and stays so.
  Vector<u8> flat(8 * 8 * 4);
  for (u32 i = 0; i < 64; ++i) {
    flat[i * 4 + 0] = 128;
    flat[i * 4 + 1] = 128;
    flat[i * 4 + 2] = 255;
    flat[i * 4 + 3] = 255;
  }
  Vector<MipLevel> chain;
  build_mip_chain(std::span<const u8>(flat.data(), flat.size()), 8, 8, MipMode::normal, true,
                  chain);
  for (const MipLevel& level : chain) {
    for (u32 i = 0; i < level.width * level.height; ++i) {
      CHECK(level.rgba[i * 4 + 0] == 128);
      CHECK(level.rgba[i * 4 + 1] == 128);
      CHECK(level.rgba[i * 4 + 2] == 255);
    }
  }
}

TEST_CASE("mips: a normal map is put back on the unit sphere at every level") {
  constexpr u32 n = 64;
  Vector<u8> pixels;
  test_fixtures::normal_image(n, n, pixels);
  // A vector the file left short: (128, 128, 200) decodes to about (0.004, 0.004, 0.57).
  pixels[0] = 128;
  pixels[1] = 128;
  pixels[2] = 200;
  Vector<MipLevel> chain;
  build_mip_chain(std::span<const u8>(pixels.data(), pixels.size()), n, n, MipMode::normal, true,
                  chain);
  CHECK(chain[0].rgba[0] == 128);
  CHECK(chain[0].rgba[1] == 128);
  CHECK(chain[0].rgba[2] == 255);
  f64 worst = 0.0;
  for (const MipLevel& level : chain) {
    for (u32 i = 0; i < level.width * level.height; ++i) {
      const f64 x = level.rgba[i * 4 + 0] / 255.0 * 2.0 - 1.0;
      const f64 y = level.rgba[i * 4 + 1] / 255.0 * 2.0 - 1.0;
      const f64 z = level.rgba[i * 4 + 2] / 255.0 * 2.0 - 1.0;
      worst = std::max(worst, std::abs(std::sqrt(x * x + y * y + z * z) - 1.0));
    }
  }
  // Unit up to the 8-bit quantization of each component: three half-steps of 2/255 at most.
  CHECK(worst < 0.0118);
}

TEST_CASE("mips: an odd side is filtered, not dropped") {
  // Three texels, black-white-black, halve to one: the white centre contributes to it rather
  // than being skipped by a sampler that stepped by two.
  Vector<u8> row(3 * 4, u8{0});
  row[4 + 0] = row[4 + 1] = row[4 + 2] = row[4 + 3] = 255;
  Vector<MipLevel> chain;
  build_mip_chain(std::span<const u8>(row.data(), row.size()), 3, 1, MipMode::linear, true, chain);
  REQUIRE(chain.size() == 2);
  CHECK(chain[1].width == 1);
  CHECK(chain[1].rgba[0] > 60);
  CHECK(chain[1].rgba[0] < 200);
}

TEST_CASE("mips: the kernel is Mitchell-Netravali and a partition of unity") {
  CHECK(static_cast<f64>(mip_kernel(0.0f)) == doctest::Approx(16.0 / 18.0).epsilon(1e-6));
  CHECK(static_cast<f64>(mip_kernel(1.0f)) == doctest::Approx(1.0 / 18.0).epsilon(1e-6));
  CHECK(mip_kernel(-1.0f) == mip_kernel(1.0f));
  CHECK(mip_kernel(2.0f) == 0.0f);
  CHECK(mip_kernel(-2.5f) == 0.0f);
  CHECK(mip_kernel(1.5f) < 0.0f);  // the small negative lobe
  for (const f32 phase : {0.0f, 0.125f, 0.25f, 0.5f, 0.75f}) {
    f64 sum = 0.0;
    for (i32 k = -3; k <= 3; ++k)
      sum += static_cast<f64>(mip_kernel(phase + static_cast<f32>(k)));
    CHECK(sum == doctest::Approx(1.0).epsilon(1e-6));
  }
}

TEST_CASE("mips: the chain is the same bytes for any thread count") {
  Vector<u8> pixels;
  test_fixtures::colour_image(203, 117, true, pixels);
  jobs::JobSystem pool(four_workers());
  for (const MipMode mode : {MipMode::srgb, MipMode::linear, MipMode::normal}) {
    Vector<MipLevel> serial;
    Vector<MipLevel> parallel;
    build_mip_chain(std::span<const u8>(pixels.data(), pixels.size()), 203, 117, mode, true,
                    serial);
    build_mip_chain(std::span<const u8>(pixels.data(), pixels.size()), 203, 117, mode, true,
                    parallel, &pool);
    REQUIRE(serial.size() == parallel.size());
    for (u32 i = 0; i < serial.size(); ++i) {
      REQUIRE(serial[i].rgba.size() == parallel[i].rgba.size());
      CHECK(std::memcmp(serial[i].rgba.data(), parallel[i].rgba.data(), serial[i].rgba.size()) ==
            0);
    }
  }
}
