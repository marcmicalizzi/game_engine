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
    for (u32 w = 0; w < width && s < length; ++w, ++s)
      along[s] = 1;
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
// A level's coverage as the build holds it: the share of its bilinearly filtered surface at the
// cutoff.
f64 decoded_coverage(const TextureData& data, u32 level, u8 cutoff) {
  Vector<u8> rgba;
  const TextureFileLevel& l = data.levels[level];
  REQUIRE(decode_level(data.format, data.level_bytes(level), l.width, l.height, rgba, nullptr));
  return alpha_coverage_filtered(std::span<const u8>(rgba.data(), rgba.size()), l.width, l.height,
                                 cutoff);
}

// The same coverage found another way, as a sampler would: `count` points of a fixed sequence
// over the level, each a clamped bilinear tap in floats, tested at the cutoff. It shares no code
// with the build's integer measure, so the two agreeing is the measure checked and not restated.
f64 sampled_coverage(const TextureData& data, u32 level, u8 cutoff, u32 count) {
  Vector<u8> rgba;
  const TextureFileLevel& l = data.levels[level];
  REQUIRE(decode_level(data.format, data.level_bytes(level), l.width, l.height, rgba, nullptr));
  u32 state = 2463534242u;
  auto unit = [&state]() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<f64>(state >> 8) / 16777216.0;
  };
  auto alpha_at = [&](i64 x, i64 y) {
    const i64 cx = x < 0 ? 0 : (x >= static_cast<i64>(l.width) ? static_cast<i64>(l.width) - 1 : x);
    const i64 cy =
        y < 0 ? 0 : (y >= static_cast<i64>(l.height) ? static_cast<i64>(l.height) - 1 : y);
    return static_cast<f64>(rgba[static_cast<u32>((cy * l.width + cx) * 4 + 3)]);
  };
  u32 kept = 0;
  for (u32 i = 0; i < count; ++i) {
    const f64 u = unit() * static_cast<f64>(l.width) - 0.5;
    const f64 v = unit() * static_cast<f64>(l.height) - 0.5;
    const f64 fx = std::floor(u);
    const f64 fy = std::floor(v);
    const f64 tx = u - fx;
    const f64 ty = v - fy;
    const i64 x = static_cast<i64>(fx);
    const i64 y = static_cast<i64>(fy);
    const f64 a = (alpha_at(x, y) * (1.0 - tx) + alpha_at(x + 1, y) * tx) * (1.0 - ty) +
                  (alpha_at(x, y + 1) * (1.0 - tx) + alpha_at(x + 1, y + 1) * tx) * ty;
    if (a >= static_cast<f64>(cutoff)) ++kept;
  }
  return static_cast<f64>(kept) / static_cast<f64>(count);
}

}  // namespace

TEST_CASE("alpha mask: the filtered coverage is what a linear sampler draws, not a texel count") {
  // One texel exactly at the cutoff among texels below it: a nearest sampler draws the texel, a
  // linear one draws nothing, since every blend with a neighbour is under the cutoff.
  constexpr u32 k_side = 8;
  Vector<u8> pixels(static_cast<usize>(k_side) * k_side * 4, u8{0});
  pixels[(3 * k_side + 3) * 4 + 3] = 128;
  const std::span<const u8> one(pixels.data(), pixels.size());
  CHECK(alpha_coverage(one, k_side, k_side, 128) == doctest::Approx(1.0 / 64.0));
  CHECK(alpha_coverage_filtered(one, k_side, k_side, 128) == 0.0);
  // The same texel opaque: the surface passes where the blend keeps half, 12 of its 16 samples
  // (the four at 3/8 on both axes hold 25/64 of it).
  pixels[(3 * k_side + 3) * 4 + 3] = 255;
  CHECK(alpha_coverage_filtered(one, k_side, k_side, 128) == doctest::Approx(12.0 / (16.0 * 64.0)));
  // Solid and empty are 1 and 0, edges included; a repeating edge reads the far side.
  Vector<u8> solid(static_cast<usize>(k_side) * k_side * 4, u8{255});
  CHECK(alpha_coverage_filtered({solid.data(), solid.size()}, k_side, k_side, 128) == 1.0);
  Vector<u8> column(static_cast<usize>(k_side) * k_side * 4, u8{0});
  for (u32 y = 0; y < k_side; ++y)
    column[(y * k_side) * 4 + 3] = 200;  // the left edge's column
  const std::span<const u8> edge(column.data(), column.size());
  // Of the column's four samples across: 7/8 of 200 passes and 5/8 of it (125) does not. Clamped,
  // the two samples towards the edge read the column again and pass: three of thirty-two across.
  CHECK(alpha_coverage_filtered(edge, k_side, k_side, 128) == doctest::Approx(3.0 / 32.0));
  // Repeating, they blend with the empty far side, and the outer one fails: two.
  CHECK(alpha_coverage_filtered(edge, k_side, k_side, 128, EdgeMode::repeat, EdgeMode::clamp) ==
        doctest::Approx(2.0 / 32.0));
}

TEST_CASE("alpha mask: the chain keeps level 0's coverage at the cutoff, within 2% a level") {
  constexpr u32 k_side = 256;
  constexpr u8 k_cutoff = 128;
  Vector<u8> pixels;
  needle_image(k_side, pixels);
  const std::span<const u8> source(pixels.data(), pixels.size());
  const f64 level0 = alpha_coverage_filtered(source, k_side, k_side, k_cutoff);
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

  // **What a sampler draws of each level** (G203): random bilinear taps of the decoded level, in
  // floats, pass the cutoff in level 0's share. Before the build measured the filtered surface it
  // held the share of *texels* at the cutoff, and a texel scaled just to the cutoff among lower
  // neighbours draws nothing under a linear filter, which is the crown a rasterizer drew thinner
  // than a ray; the next case measures how much that rule lost on these needles.
  constexpr u32 k_taps = 200000;  // a standard error of 0.1% at this coverage
  const f64 sampled0 = sampled_coverage(masked, 0, k_cutoff, k_taps);
  CHECK(std::fabs(sampled0 - level0) <= 0.01);
  for (u32 i = 1; i < masked.levels.size(); ++i) {
    const TextureFileLevel& l = masked.levels[i];
    if (static_cast<f64>(l.width) * l.height < 64.0) break;
    const f64 sampled = sampled_coverage(masked, i, k_cutoff, k_taps);
    CAPTURE(i);
    CAPTURE(sampled);
    CHECK(std::fabs(sampled - sampled0) <= 0.025);
  }
}

TEST_CASE("alpha mask: holding a level's texel count loses the coverage a linear sampler draws") {
  // The rule the build had until G203, restated here so the test above can be told from it: scale
  // a level's alpha so the share of texels at the cutoff is level 0's. Measured as a sampler
  // reads the level, the needles' chain then draws well under level 0's coverage.
  constexpr u32 k_side = 256;
  constexpr u8 k_cutoff = 128;
  Vector<u8> pixels;
  needle_image(k_side, pixels);
  const std::span<const u8> source(pixels.data(), pixels.size());
  const f64 level0 = alpha_coverage_filtered(source, k_side, k_side, k_cutoff);
  Vector<MipLevel> chain;
  build_mip_chain(source, k_side, k_side, MipMode::srgb, true, chain);
  f64 worst_count_held = 0.0;
  f64 worst_filtered_held = 0.0;
  std::string held;
  for (u32 i = 1; i < chain.size(); ++i) {
    MipLevel& level = chain[i];
    const usize texels = static_cast<usize>(level.width) * level.height;
    if (texels < 64) break;
    Vector<u8> counted = level.rgba;
    {
      u64 histogram[256] = {};
      for (usize t = 0; t < texels; ++t)
        ++histogram[counted[static_cast<u32>(t * 4 + 3)]];
      const f64 wanted = alpha_coverage(source, k_side, k_side, k_cutoff) * static_cast<f64>(texels);
      u64 at_least = 0;
      u32 best = k_cutoff;
      f64 best_miss = 1e300;
      for (i32 t = 255; t >= 1; --t) {
        at_least += histogram[t];
        const f64 miss = std::fabs(static_cast<f64>(at_least) - wanted);
        if (miss < best_miss) {
          best_miss = miss;
          best = static_cast<u32>(t);
        }
      }
      for (usize t = 0; t < texels; ++t) {
        const u32 scaled = static_cast<u32>(counted[static_cast<u32>(t * 4 + 3)]) * k_cutoff / best;
        counted[static_cast<u32>(t * 4 + 3)] = static_cast<u8>(scaled > 255u ? 255u : scaled);
      }
    }
    const f64 count_held = alpha_coverage_filtered({counted.data(), counted.size()}, level.width,
                                                   level.height, k_cutoff);
    preserve_alpha_coverage(level.rgba, level.width, level.height, k_cutoff, level0);
    const f64 filtered_held = alpha_coverage_filtered({level.rgba.data(), level.rgba.size()},
                                                      level.width, level.height, k_cutoff);
    worst_count_held = std::fmax(worst_count_held, std::fabs(count_held - level0));
    worst_filtered_held = std::fmax(worst_filtered_held, std::fabs(filtered_held - level0));
    held += " " + std::to_string(level.width) + ":" + std::to_string(count_held / level0) + "/" +
            std::to_string(filtered_held / level0);
  }
  MESSAGE("level-0 filtered coverage " << level0
                                       << "; per level, of level 0's: texel count held / filtered held:"
                                       << held);
  // Both in points of the surface, the currency of the build's own slack (half a point after the
  // encoder). The old rule: of level 0's 13.8 points the 128, 64, 16 and 8 texel levels drew 60,
  // 49, 32 and 49 percent (2026-11-14), so its worst level is more than four points short, twice
  // what the chain test above allows. The build's: within a point before the encoder, on levels
  // where one step of the 8-bit scale moves many of a few hundred samples at once.
  CHECK(worst_count_held > 0.04);
  CHECK(worst_filtered_held < 0.01);
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
