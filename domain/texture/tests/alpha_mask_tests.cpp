// Alpha-tested masks (docs/subsystems/texture.md, "Alpha-tested masks"): the coverage a chain keeps
// at the material's cutoff, and the colour the build grows under the cut.

#include <domain/texture/material_textures.h>
#include <domain/texture/texture_build.h>

#include <doctest/doctest.h>

#include <cmath>
#include <string>

using namespace engine;
using namespace engine::texture;

namespace {

// A leaf: an ellipse with a stem, alpha 255 inside and 0 outside with a one-texel soft edge, green
// inside and black under the cut — what an editor that exports straight alpha leaves there.
void leaf_image(u32 side, Vector<u8>& out) {
  out.assign(static_cast<usize>(side) * side * 4, u8{0});
  const f32 c = static_cast<f32>(side) * 0.5f;
  for (u32 y = 0; y < side; ++y) {
    for (u32 x = 0; x < side; ++x) {
      const f32 u = (static_cast<f32>(x) + 0.5f - c) / (0.22f * static_cast<f32>(side));
      const f32 v = (static_cast<f32>(y) + 0.5f - c * 0.8f) / (0.38f * static_cast<f32>(side));
      const f32 d = std::sqrt(u * u + v * v);
      f32 a = (1.0f - d) * 0.22f * static_cast<f32>(side);  // texels inside the edge
      const f32 stem = std::fabs(static_cast<f32>(x) + 0.5f - c);
      if (y > static_cast<u32>(c * 1.1f) && stem < 1.5f) a = 1.0f;
      const f32 alpha = a <= 0.0f ? 0.0f : (a >= 1.0f ? 1.0f : a);
      u8* t = out.data() + (static_cast<usize>(y) * side + x) * 4;
      t[3] = static_cast<u8>(alpha * 255.0f + 0.5f);
      if (t[3] != 0) {
        t[0] = 40;
        t[1] = 150;
        t[2] = 30;
      }
    }
  }
}

// A conifer's sprig, the case the chain loses: needles one to three texels wide at an angle, at
// irregular gaps (a fixed LCG, so the bytes are the same everywhere), inside an elliptical
// envelope so their density varies across the image. Filtered as stored, a level whose texel spans
// more than a needle's width holds them below half alpha and they are cut away whole; the
// irregular spacing is what leaves a coarse level a spread of alphas a scale can act on (a
// perfectly periodic mask filters to one uniform alpha, which no scale can cut at a fraction).
void needle_image(u32 side, Vector<u8>& out) {
  out.assign(static_cast<usize>(side) * side * 4, u8{0});
  // The needles along s = x + y / 2: alternating runs of needle and gap, in whole texels.
  const u32 length = side * 2;
  Vector<u8> along(length, u8{0});
  u32 state = 12345u;
  auto next = [&state](u32 n) {
    state = state * 1664525u + 1013904223u;
    return (state >> 16) % n;
  };
  for (u32 s = 0; s < length;) {
    s += 2 + next(10);  // the gap
    const u32 width = 1 + next(3);
    for (u32 w = 0; w < width && s < length; ++w, ++s) along[s] = 1;
  }
  const f32 c = static_cast<f32>(side) * 0.5f;
  for (u32 y = 0; y < side; ++y) {
    for (u32 x = 0; x < side; ++x) {
      const u32 s = x + y / 2;
      const f32 u = (static_cast<f32>(x) + 0.5f - c) / (0.45f * static_cast<f32>(side));
      const f32 v = (static_cast<f32>(y) + 0.5f - c) / (0.48f * static_cast<f32>(side));
      const bool inside = u * u + v * v < 1.0f;
      u8* t = out.data() + (static_cast<usize>(y) * side + x) * 4;
      t[0] = 30;
      t[1] = 110;
      t[2] = 40;
      t[3] = inside && along[s] != 0 ? 255 : 0;
    }
  }
}
f64 decoded_coverage(const TextureData& data, u32 level, u8 cutoff) {
  Vector<u8> rgba;
  const TextureFileLevel& l = data.levels[level];
  REQUIRE(decode_level(data.format, data.level_bytes(level), l.width, l.height, rgba, nullptr));
  return alpha_coverage(std::span<const u8>(rgba.data(), rgba.size()), l.width, l.height, cutoff);
}

}  // namespace

TEST_CASE("alpha mask: the chain keeps level 0's coverage at the cutoff, within 2% a level") {
  constexpr u32 k_side = 256;
  constexpr u8 k_cutoff = 128;
  Vector<u8> pixels;
  needle_image(k_side, pixels);
  const std::span<const u8> source(pixels.data(), pixels.size());
  const f64 level0 = alpha_coverage(source, k_side, k_side, k_cutoff);
  REQUIRE(level0 > 0.1);
  REQUIRE(level0 < 0.6);

  TextureBuildOptions options;
  options.format = FormatChoice::bc7;
  options.color_space = ColorSpace::srgb;
  TextureData plain;
  REQUIRE(build_texture(source, k_side, k_side, 4, options, plain, nullptr));
  options.alpha_cutoff = k_cutoff;
  TextureData masked;
  REQUIRE(build_texture(source, k_side, k_side, 4, options, masked, nullptr));
  CHECK((masked.flags & k_texture_alpha_coverage) != 0);
  CHECK((plain.flags & k_texture_alpha_coverage) == 0);
  REQUIRE(masked.levels.size() == plain.levels.size());

  // Every level a coverage share can be told on: one texel is 2% of a 7x7 level, so the levels
  // below 8x8 are held to two texels' worth instead — a tie of equal alphas, then the encoder's
  // rounding — and they are a tree a few pixels across.
  f64 worst_plain = 0.0;
  for (u32 i = 0; i < masked.levels.size(); ++i) {
    const TextureFileLevel& l = masked.levels[i];
    const f64 texels = static_cast<f64>(l.width) * l.height;
    const f64 tolerance = texels >= 64.0 ? 0.02 : 2.0 / texels + 1e-9;
    const f64 kept = decoded_coverage(masked, i, k_cutoff);
    const f64 lost = decoded_coverage(plain, i, k_cutoff);
    CAPTURE(i);
    CAPTURE(kept);
    CAPTURE(lost);
    CHECK(std::fabs(kept - level0) <= tolerance);
    if (texels >= 64.0) worst_plain = std::fmax(worst_plain, std::fabs(lost - level0));
  }
  // The test can tell: the same chain filtered as stored drifts further than the tolerance.
  MESSAGE("level-0 coverage " << level0 << "; the unscaled chain's worst drift " << worst_plain);
  CHECK(worst_plain > 0.02);
}

TEST_CASE("alpha mask: the colour under the cut is the leaf's, a few texels out") {
  constexpr u32 k_side = 64;
  Vector<u8> pixels;
  leaf_image(k_side, pixels);
  Vector<u8> dilated = pixels;
  dilate_under_mask(dilated, k_side, k_side, 128, 4);
  u32 grown = 0;
  for (usize i = 0; i < static_cast<usize>(k_side) * k_side; ++i) {
    const u8* before = pixels.data() + i * 4;
    const u8* after = dilated.data() + i * 4;
    CHECK(after[3] == before[3]);  // alpha is never touched
    if (before[3] >= 128) {
      CHECK(after[0] == before[0]);  // nor a texel that passes
      continue;
    }
    if (after[1] != 0) {
      ++grown;
      CHECK(after[1] == 150);  // every neighbour it averaged is the leaf's one green
    }
  }
  CHECK(grown > 0);
}

TEST_CASE("alpha mask: the cutoff packs into the options word, and none packs as before") {
  TextureBuildOptions options;
  const u32 before = pack_texture_options(options);
  options.alpha_cutoff = 128;
  const u32 packed = pack_texture_options(options);
  CHECK(packed != before);
  CHECK((packed & ~0x00ff0000u) == before);
  TextureBuildOptions back;
  REQUIRE(unpack_texture_options(packed, back));
  CHECK(back.alpha_cutoff == 128);
  CHECK(texture_cache_key(7, options) != texture_cache_key(7, TextureBuildOptions{}));
  CHECK(alpha_cutoff_code(0.5f) == 128);
  CHECK(alpha_cutoff_code(0.0f) == 1);
  CHECK(alpha_cutoff_code(1.0f) == 255);
}
