// Block compression against decoders written here from the format's definition (the D3D10 block
// compression specification, which Vulkan adopts): BC4 and BC5 are exact on constant blocks and on
// gradients whose values lie on a block's palette, BC1 is exact on a colour its 5:6:5 endpoints
// hold, BC7 decodes back within the bounds stated below, and the encoder writes the same blocks for
// any thread count. The reference decoders are deliberately the slow, readable kind — they are
// documentation of the formats as much as tests, like foundation/image's reference inflater.
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

// BC4: two 8-bit endpoints and sixteen 3-bit indices. With r0 > r1 the palette is r0, r1 and six
// values between them at sevenths; otherwise r0, r1, four values at fifths, 0 and 255. A GPU
// computes the interpolants in at least 8 bits of precision and rounds; for the values these tests
// use every interpolant is an integer, so any rounding gives the same byte.
void reference_bc4(const u8* block, u8 out[16]) {
  const u32 r0 = block[0];
  const u32 r1 = block[1];
  u32 palette[8];
  palette[0] = r0;
  palette[1] = r1;
  if (r0 > r1) {
    for (u32 i = 2; i < 8; ++i)
      palette[i] = ((8 - i) * r0 + (i - 1) * r1 + 3) / 7;
  } else {
    for (u32 i = 2; i < 6; ++i)
      palette[i] = ((6 - i) * r0 + (i - 1) * r1 + 2) / 5;
    palette[6] = 0;
    palette[7] = 255;
  }
  u64 bits = 0;
  for (u32 i = 0; i < 6; ++i)
    bits |= static_cast<u64>(block[2 + i]) << (8 * i);
  for (u32 t = 0; t < 16; ++t)
    out[t] = static_cast<u8>(palette[(bits >> (3 * t)) & 7u]);
}

// BC1's colour half: two 5:6:5 endpoints and sixteen 2-bit indices; four colours when c0 > c1
// (thirds), three and black otherwise (a half). Endpoints expand by bit replication.
void reference_bc1(const u8* block, u8 out[64]) {
  const u32 c0 = block[0] | (block[1] << 8);
  const u32 c1 = block[2] | (block[3] << 8);
  auto expand = [](u32 c, u32 rgb[3]) {
    const u32 r = (c >> 11) & 31;
    const u32 g = (c >> 5) & 63;
    const u32 b = c & 31;
    rgb[0] = (r << 3) | (r >> 2);
    rgb[1] = (g << 2) | (g >> 4);
    rgb[2] = (b << 3) | (b >> 2);
  };
  u32 p[4][3];
  expand(c0, p[0]);
  expand(c1, p[1]);
  for (u32 c = 0; c < 3; ++c) {
    if (c0 > c1) {
      p[2][c] = (2 * p[0][c] + p[1][c] + 1) / 3;
      p[3][c] = (p[0][c] + 2 * p[1][c] + 1) / 3;
    } else {
      p[2][c] = (p[0][c] + p[1][c]) / 2;
      p[3][c] = 0;
    }
  }
  const u32 bits =
      block[4] | (block[5] << 8) | (block[6] << 16) | (static_cast<u32>(block[7]) << 24);
  for (u32 t = 0; t < 16; ++t) {
    const u32 index = (bits >> (2 * t)) & 3u;
    out[t * 4 + 0] = static_cast<u8>(p[index][0]);
    out[t * 4 + 1] = static_cast<u8>(p[index][1]);
    out[t * 4 + 2] = static_cast<u8>(p[index][2]);
    out[t * 4 + 3] = 255;
  }
}

// One 4x4 block of RGBA through `encode_level`.
Vector<u8> encode_block(TextureFormat format, const u8 texels[64], bool perceptual = false) {
  Vector<u8> out(texture_block_bytes(format));
  encode_level(format, std::span<const u8>(texels, 64), 4, 4, perceptual,
               std::span<u8>(out.data(), out.size()));
  return out;
}

struct Error {
  f64 psnr = 0.0;
  u32 max = 0;
};

// Over the channels in `mask` (bit c for channel c).
Error compare(const Vector<u8>& a, const Vector<u8>& b, u32 mask) {
  f64 squared = 0.0;
  u64 samples = 0;
  Error e;
  for (u32 i = 0; i < a.size(); ++i) {
    if ((mask & (1u << (i % 4))) == 0) continue;
    const i32 d = static_cast<i32>(a[i]) - static_cast<i32>(b[i]);
    e.max = std::max(e.max, static_cast<u32>(d < 0 ? -d : d));
    squared += static_cast<f64>(d) * d;
    ++samples;
  }
  const f64 mse = squared / static_cast<f64>(samples);
  e.psnr = mse == 0.0 ? 1000.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
  return e;
}

}  // namespace

TEST_CASE("blocks: BC4 is exact on every constant block and on on-palette gradients") {
  u8 texels[64];
  u8 decoded[16];
  for (u32 v = 0; v < 256; ++v) {
    for (u32 t = 0; t < 16; ++t) {
      texels[t * 4 + 0] = static_cast<u8>(v);
      texels[t * 4 + 1] = texels[t * 4 + 2] = 0;
      texels[t * 4 + 3] = 255;
    }
    const Vector<u8> block = encode_block(TextureFormat::bc4, texels);
    reference_bc4(block.data(), decoded);
    for (u32 t = 0; t < 16; ++t)
      CHECK(decoded[t] == v);
  }
  // Eight values a + 30 k (k = 0..7) are the whole palette of endpoints a and a + 210 — every
  // seventh of the span an integer — laid out twice over the block. An encoder that finds the
  // endpoints reproduces them exactly, and anything else would be a lossy answer to a lossless
  // question.
  for (const u32 a : {0u, 20u, 45u}) {
    for (u32 t = 0; t < 16; ++t) {
      texels[t * 4 + 0] = static_cast<u8>(a + 30 * (t % 8));
      texels[t * 4 + 1] = texels[t * 4 + 2] = 0;
      texels[t * 4 + 3] = 255;
    }
    const Vector<u8> block = encode_block(TextureFormat::bc4, texels);
    reference_bc4(block.data(), decoded);
    for (u32 t = 0; t < 16; ++t)
      CHECK(decoded[t] == a + 30 * (t % 8));
  }
  // A horizontal ramp across the block's four columns, on the palette of its own ends (0 and 210,
  // sevenths of 30): 0, 60, 150, 210.
  const u8 ramp_values[4] = {0, 60, 150, 210};
  for (u32 t = 0; t < 16; ++t) {
    texels[t * 4 + 0] = ramp_values[t % 4];
    texels[t * 4 + 3] = 255;
  }
  const Vector<u8> ramp = encode_block(TextureFormat::bc4, texels);
  reference_bc4(ramp.data(), decoded);
  for (u32 t = 0; t < 16; ++t)
    CHECK(decoded[t] == ramp_values[t % 4]);
}

TEST_CASE("blocks: BC5 is two independent BC4 channels, exact on the same blocks") {
  u8 texels[64];
  for (u32 t = 0; t < 16; ++t) {
    texels[t * 4 + 0] = static_cast<u8>(20 + 30 * (t % 8));  // a gradient on R
    texels[t * 4 + 1] = 131;                                 // a constant on G
    texels[t * 4 + 2] = 7;                                   // ignored
    texels[t * 4 + 3] = 255;
  }
  const Vector<u8> block = encode_block(TextureFormat::bc5, texels);
  u8 r[16];
  u8 g[16];
  reference_bc4(block.data(), r);
  reference_bc4(block.data() + 8, g);
  for (u32 t = 0; t < 16; ++t) {
    CHECK(r[t] == 20 + 30 * (t % 8));
    CHECK(g[t] == 131);
  }
  // The library's decoder, which the tool reports with, agrees with the reference and leaves blue
  // at zero, as a GPU sampling a BC5 view does.
  Vector<u8> decoded;
  REQUIRE(decode_level(TextureFormat::bc5, std::span<const u8>(block.data(), block.size()), 4, 4,
                       decoded));
  for (u32 t = 0; t < 16; ++t) {
    CHECK(decoded[t * 4 + 0] == r[t]);
    CHECK(decoded[t * 4 + 1] == g[t]);
    CHECK(decoded[t * 4 + 2] == 0);
    CHECK(decoded[t * 4 + 3] == 255);
  }
}

TEST_CASE("blocks: BC1 is exact on a colour its endpoints hold, and BC3 carries alpha") {
  // 5:6:5 values with bit replication: r = 0b10110 -> 0b10110101, g = 0b100111 -> 0b10011110, b =
  // 0b01001 -> 0b01001010.
  const u8 colour[3] = {0xb5, 0x9e, 0x4a};
  u8 texels[64];
  for (u32 t = 0; t < 16; ++t) {
    texels[t * 4 + 0] = colour[0];
    texels[t * 4 + 1] = colour[1];
    texels[t * 4 + 2] = colour[2];
    texels[t * 4 + 3] = static_cast<u8>(20 + 30 * (t % 8));  // alpha on a BC4 palette
  }
  const Vector<u8> bc1 = encode_block(TextureFormat::bc1, texels);
  u8 decoded[64];
  reference_bc1(bc1.data(), decoded);
  for (u32 t = 0; t < 16; ++t) {
    CHECK(decoded[t * 4 + 0] == colour[0]);
    CHECK(decoded[t * 4 + 1] == colour[1]);
    CHECK(decoded[t * 4 + 2] == colour[2]);
  }
  const Vector<u8> bc3 = encode_block(TextureFormat::bc3, texels);
  u8 alpha[16];
  reference_bc4(bc3.data(), alpha);  // BC3's first half is a BC4 block of alpha
  reference_bc1(bc3.data() + 8, decoded);
  for (u32 t = 0; t < 16; ++t) {
    CHECK(alpha[t] == 20 + 30 * (t % 8));
    CHECK(decoded[t * 4 + 0] == colour[0]);
  }
}

TEST_CASE("blocks: BC7 decodes back within the stated bounds") {
  // The colour fixture has four regions, and BC7 is held to a bound on each, measured over RGBA
  // against the level it was given (the numbers these print are the ones the bounds were set
  // under, with a margin): the smooth gradient — what banding shows on — above 45 dB with no
  // channel more than 6 codes off; the hard-edged two-colour tiles above 48 dB; the corner whose
  // alpha varies texel by texel above 30 dB; and the band of white noise, sixteen unrelated colours
  // a block that no 128-bit block of any format can hold, above 14 dB (a block of one flat colour
  // would score 10.8) — stated so that the figure
  // cannot quietly get worse, not because it is good.
  constexpr u32 w = 128;
  constexpr u32 h = 128;
  Vector<u8> pixels;
  test_fixtures::colour_image(w, h, true, pixels);
  Vector<u8> blocks(static_cast<u32>(texture_level_bytes(TextureFormat::bc7, w, h)));
  encode_level(TextureFormat::bc7, std::span<const u8>(pixels.data(), pixels.size()), w, h, true,
               std::span<u8>(blocks.data(), blocks.size()));
  Vector<u8> decoded;
  REQUIRE(decode_level(TextureFormat::bc7, std::span<const u8>(blocks.data(), blocks.size()), w, h,
                       decoded));
  // Whole blocks only, so a region's figure is not a neighbour's error at its border.
  auto region = [&](u32 x0, u32 y0, u32 x1, u32 y1) {
    Vector<u8> a;
    Vector<u8> b;
    for (u32 y = y0; y < y1; ++y) {
      for (u32 x = x0; x < x1; ++x) {
        for (u32 c = 0; c < 4; ++c) {
          a.push_back(pixels[(y * w + x) * 4 + c]);
          b.push_back(decoded[(y * w + x) * 4 + c]);
        }
      }
    }
    return compare(a, b, 0xf);
  };
  const Error gradient = region(w / 2, h / 4 + 4, w, h / 2);
  const Error tiles = region(0, h / 4 + 4, w / 4 - 4, h / 2);
  const Error alpha = region(0, h / 2 + 4, w / 4 - 4, h);  // x < w/3, y > h/2: varying alpha
  const Error noise = region(0, 0, w, h / 4);
  MESSAGE("BC7: gradient " << gradient.psnr << " dB (worst " << gradient.max << "), tiles "
                           << tiles.psnr << " dB, alpha " << alpha.psnr << " dB, noise "
                           << noise.psnr << " dB");
  CHECK(gradient.psnr > 45.0);
  CHECK(gradient.max <= 6);
  CHECK(tiles.psnr > 48.0);
  CHECK(alpha.psnr > 30.0);
  CHECK(noise.psnr > 14.0);
}

TEST_CASE("blocks: the encoder writes the same blocks for any thread count") {
  jobs::JobSystemConfig config;
  config.performance_workers = 4;
  config.efficiency_workers = 1;
  config.pin_threads = false;
  jobs::JobSystem pool(config);
  Vector<u8> pixels;
  test_fixtures::colour_image(61, 45, true, pixels);
  for (const TextureFormat format : {TextureFormat::bc1, TextureFormat::bc3, TextureFormat::bc4,
                                     TextureFormat::bc5, TextureFormat::bc7}) {
    const u32 bytes = static_cast<u32>(texture_level_bytes(format, 61, 45));
    Vector<u8> serial(bytes);
    Vector<u8> parallel(bytes);
    encode_level(format, std::span<const u8>(pixels.data(), pixels.size()), 61, 45, true,
                 std::span<u8>(serial.data(), serial.size()));
    encode_level(format, std::span<const u8>(pixels.data(), pixels.size()), 61, 45, true,
                 std::span<u8>(parallel.data(), parallel.size()), &pool);
    CHECK(std::memcmp(serial.data(), parallel.data(), bytes) == 0);
  }
}
