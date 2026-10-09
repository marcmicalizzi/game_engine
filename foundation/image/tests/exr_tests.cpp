// Half-float OpenEXR (exr.h; E39, roadmap R79): the conversion to and from half rounds as a GPU
// store does, and a known gradient written as an EXR reads back to the bit.

#include <foundation/image/exr.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <string>

using namespace engine;
using namespace engine::image;

namespace {

u32 bits_of(f32 v) {
  u32 b = 0;
  std::memcpy(&b, &v, sizeof(b));
  return b;
}

f32 float_of(u32 b) {
  f32 v = 0.0f;
  std::memcpy(&v, &b, sizeof(v));
  return v;
}

}  // namespace

TEST_CASE("exr: every half survives the trip through a float, and rounding is to nearest even") {
  // Every finite half and both infinities come back as themselves; a NaN stays a NaN.
  for (u32 h = 0; h < 0x10000u; ++h) {
    const u16 half = static_cast<u16>(h);
    const f32 f = f32_from_half(half);
    if (((h >> 10) & 0x1fu) == 0x1fu && (h & 0x3ffu) != 0u) {
      CHECK(std::isnan(f));
      CHECK(std::isnan(f32_from_half(half_from_f32(f))));
      continue;
    }
    CHECK_MESSAGE(half_from_f32(f) == half, "half " << h);
  }
  CHECK(f32_from_half(0x3c00u) == 1.0f);
  CHECK(f32_from_half(0x7bffu) == 65504.0f);
  CHECK(f32_from_half(0x0001u) == std::ldexp(1.0f, -24));
  // Halfway between two halves goes to the even one, a hair past it to the nearer.
  const f32 one_and_half_step = 1.0f + std::ldexp(1.0f, -11);  // between 0x3c00 and 0x3c01
  CHECK(half_from_f32(one_and_half_step) == 0x3c00u);
  CHECK(half_from_f32(float_of(bits_of(one_and_half_step) + 1u)) == 0x3c01u);
  const f32 three_halves = 1.0f + 3.0f * std::ldexp(1.0f, -11);  // between 0x3c01 and 0x3c02
  CHECK(half_from_f32(three_halves) == 0x3c02u);
  // Overflow: 65519 is still 65504, 65520 (the tie with 2^16) is infinity, as is anything above.
  CHECK(half_from_f32(65519.0f) == 0x7bffu);
  CHECK(half_from_f32(65520.0f) == 0x7c00u);
  CHECK(half_from_f32(1.0e9f) == 0x7c00u);
  CHECK(half_from_f32(-1.0e9f) == 0xfc00u);
  CHECK(half_from_f32(std::numeric_limits<f32>::infinity()) == 0x7c00u);
  // Subnormals: 2^-25 is the tie with zero (to zero), a hair above it the smallest subnormal, and
  // the largest subnormal rounds up into the smallest normal.
  CHECK(half_from_f32(std::ldexp(1.0f, -25)) == 0x0000u);
  CHECK(half_from_f32(float_of(bits_of(std::ldexp(1.0f, -25)) + 1u)) == 0x0001u);
  CHECK(half_from_f32(std::ldexp(1.0f, -14) - std::ldexp(1.0f, -26)) == 0x0400u);
  CHECK(half_from_f32(-0.0f) == 0x8000u);
}

TEST_CASE("exr: a gradient written as RGB and RGBA halves reads back to the bit") {
  engine::test::TempDir dir("engine_image_exr");
  // 61 x 37 (no dimension a multiple of ZIP's 16-line block), red a ramp over [0, 1] across,
  // green one down, blue the light past white an HDR picture holds (up to 64), alpha a checker:
  // smooth enough for ZIP to compress and odd enough to show a swapped channel or row.
  const u32 width = 61;
  const u32 height = 37;
  for (const u32 channels : {3u, 4u}) {
    CAPTURE(channels);
    Vector<u16> halves(width * height * channels);
    for (u32 y = 0; y < height; ++y) {
      for (u32 x = 0; x < width; ++x) {
        u16* p = halves.data() + (y * width + x) * channels;
        p[0] = half_from_f32(static_cast<f32>(x) / static_cast<f32>(width - 1));
        p[1] = half_from_f32(static_cast<f32>(y) / static_cast<f32>(height - 1));
        p[2] = half_from_f32(std::exp2(static_cast<f32>(x + y) / 16.0f) - 1.0f);
        if (channels == 4) p[3] = half_from_f32(((x / 4 + y / 4) & 1u) != 0u ? 1.0f : 0.25f);
      }
    }
    const std::string path = dir.path() + "/gradient-" + std::to_string(channels) + ".exr";
    ExrOptions options;
    options.white_luminance = 203.0f;
    REQUIRE(write_exr(path, width, height, channels,
                      std::span<const u16>(halves.data(), halves.size()),
                      options) == io::Status::Ok);
    ExrImage back;
    std::string error;
    REQUIRE_MESSAGE(read_exr(path, back, &error), error);
    CHECK(back.width == width);
    CHECK(back.height == height);
    CHECK(back.channels == channels);
    CHECK(back.white_luminance == 203.0f);
    REQUIRE(back.halves.size() == halves.size());
    CHECK(std::memcmp(back.halves.data(), halves.data(), halves.size() * sizeof(u16)) == 0);
    // ZIP did something: the file is smaller than the halves alone.
    std::string bytes;
    REQUIRE(io::read_file(path, bytes) == io::Status::Ok);
    CHECK(bytes.size() < halves.size() * sizeof(u16));
  }
}

TEST_CASE("exr: bad shapes are refused, not written") {
  Vector<u8> out;
  std::string error;
  const u16 pixel[3] = {0, 0, 0};
  CHECK_FALSE(encode_exr(1, 1, 2, std::span<const u16>(pixel, 2), out, {}, &error));
  CHECK(error.find("channels") != std::string::npos);
  CHECK_FALSE(encode_exr(2, 1, 3, std::span<const u16>(pixel, 3), out, {}, &error));
  CHECK(error.find("disagree") != std::string::npos);
  ExrImage image;
  const u8 junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  CHECK_FALSE(decode_exr(std::span<const u8>(junk, 8), image, &error));
  CHECK(error.find("not an OpenEXR") != std::string::npos);
}
