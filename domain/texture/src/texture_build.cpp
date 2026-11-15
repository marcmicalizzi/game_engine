// A texture build: options, the cache key, and the pipeline source bytes -> decode -> mip chain
// -> blocks -> TextureData (texture_build.h).
#include <core/hash/hash.h>
#include <core/time/time.h>
#include <domain/texture/texture_build.h>
#include <foundation/image/decode.h>
#include <foundation/io/vfs.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace engine::texture {

namespace {

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

constexpr u32 k_pack_srgb = 1u << 4;
constexpr u32 k_pack_normal = 1u << 5;
constexpr u32 k_pack_mips = 1u << 6;
constexpr u32 k_pack_valid = 1u << 7;
constexpr u32 k_pack_edge_x_shift = 8;
constexpr u32 k_pack_edge_y_shift = 10;
constexpr u32 k_pack_cutoff_shift = 16;
constexpr u32 k_pack_known = 0xfu | k_pack_srgb | k_pack_normal | k_pack_mips | k_pack_valid |
                             (3u << k_pack_edge_x_shift) | (3u << k_pack_edge_y_shift) |
                             (0xffu << k_pack_cutoff_shift);

// The largest side the builder takes. Level 1's working set is a float image of half the source
// and a horizontal pass of half its width at full height, about 12 bytes a source texel, and the
// engine's `Vector` counts in 32 bits: 16,384 on a side is 3 GiB of floats in the worst pass,
// which is past what that holds.
constexpr u32 k_max_side = 16384;

// A mask level's coverage after the encoder (build_texture): how far from level 0's it may land
// before the level is scaled again, and how many times it is.
constexpr f64 k_coverage_slack = 0.005;
constexpr u32 k_coverage_passes = 4;

f64 ms_since(i64 start_ns) noexcept {
  return static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e6;
}

// The channels a format keeps, as a mask over RGBA: what the error figure compares.
u32 stored_channels(TextureFormat format) noexcept {
  switch (format) {
    case TextureFormat::bc1: return 0x7u;
    case TextureFormat::bc4: return 0x1u;
    case TextureFormat::bc5: return 0x3u;
    case TextureFormat::rgba8:
    case TextureFormat::bc3:
    case TextureFormat::bc7: return 0xfu;
  }
  return 0xfu;
}

}  // namespace

const char* format_choice_name(FormatChoice choice) noexcept {
  switch (choice) {
    case FormatChoice::automatic: return "auto";
    case FormatChoice::rgba8: return "rgba8";
    case FormatChoice::bc1: return "bc1";
    case FormatChoice::bc3: return "bc3";
    case FormatChoice::bc4: return "bc4";
    case FormatChoice::bc5: return "bc5";
    case FormatChoice::bc7: return "bc7";
  }
  return "unknown";
}

bool parse_format_choice(std::string_view name, FormatChoice& out) noexcept {
  for (u32 value = 0; value <= static_cast<u32>(FormatChoice::bc7); ++value) {
    const FormatChoice choice = static_cast<FormatChoice>(value);
    if (name == format_choice_name(choice)) {
      out = choice;
      return true;
    }
  }
  return false;
}

const char* edge_mode_name(EdgeMode mode) noexcept {
  switch (mode) {
    case EdgeMode::clamp: return "clamp";
    case EdgeMode::repeat: return "repeat";
    case EdgeMode::mirror: return "mirror";
  }
  return "unknown";
}

bool parse_edge_mode(std::string_view name, EdgeMode& out) noexcept {
  for (u32 value = 0; value <= static_cast<u32>(EdgeMode::mirror); ++value) {
    const EdgeMode mode = static_cast<EdgeMode>(value);
    if (name == edge_mode_name(mode)) {
      out = mode;
      return true;
    }
  }
  return false;
}

u32 pack_texture_options(const TextureBuildOptions& options) noexcept {
  u32 word = static_cast<u32>(options.format) & 0xfu;
  if (options.color_space == ColorSpace::srgb) word |= k_pack_srgb;
  if (options.normal_map) word |= k_pack_normal;
  if (options.mips) word |= k_pack_mips;
  word |= static_cast<u32>(options.edge_x) << k_pack_edge_x_shift;
  word |= static_cast<u32>(options.edge_y) << k_pack_edge_y_shift;
  word |= static_cast<u32>(options.alpha_cutoff) << k_pack_cutoff_shift;
  return word | k_pack_valid;
}

bool unpack_texture_options(u32 packed, TextureBuildOptions& out) noexcept {
  if ((packed & k_pack_valid) == 0 || (packed & ~k_pack_known) != 0) return false;
  const u32 format = packed & 0xfu;
  if (format > static_cast<u32>(FormatChoice::bc7)) return false;
  const u32 edge_x = (packed >> k_pack_edge_x_shift) & 3u;
  const u32 edge_y = (packed >> k_pack_edge_y_shift) & 3u;
  if (edge_x > static_cast<u32>(EdgeMode::mirror) || edge_y > static_cast<u32>(EdgeMode::mirror))
    return false;
  out.format = static_cast<FormatChoice>(format);
  out.color_space = (packed & k_pack_srgb) != 0 ? ColorSpace::srgb : ColorSpace::linear;
  out.normal_map = (packed & k_pack_normal) != 0;
  out.mips = (packed & k_pack_mips) != 0;
  out.edge_x = static_cast<EdgeMode>(edge_x);
  out.edge_y = static_cast<EdgeMode>(edge_y);
  out.alpha_cutoff = static_cast<u8>((packed >> k_pack_cutoff_shift) & 0xffu);
  return true;
}

bool resolve_texture_format(const TextureBuildOptions& options, u32 source_channels,
                            TextureFormat& out, std::string* error) noexcept {
  TextureFormat format = TextureFormat::bc7;
  switch (options.format) {
    case FormatChoice::automatic:
      if (options.normal_map) {
        format = TextureFormat::bc5;
      } else if (source_channels == 1 && options.color_space == ColorSpace::linear) {
        format = TextureFormat::bc4;
      } else {
        format = TextureFormat::bc7;
      }
      break;
    case FormatChoice::rgba8: format = TextureFormat::rgba8; break;
    case FormatChoice::bc1: format = TextureFormat::bc1; break;
    case FormatChoice::bc3: format = TextureFormat::bc3; break;
    case FormatChoice::bc4: format = TextureFormat::bc4; break;
    case FormatChoice::bc5: format = TextureFormat::bc5; break;
    case FormatChoice::bc7: format = TextureFormat::bc7; break;
  }
  const bool srgb = options.color_space == ColorSpace::srgb && !options.normal_map;
  if (srgb && (format == TextureFormat::bc4 || format == TextureFormat::bc5)) {
    if (error != nullptr) {
      *error = std::string(texture_format_name(format)) +
               " has no sRGB form in any graphics API; build it --linear, or choose bc7";
    }
    return false;
  }
  out = format;
  return true;
}

u64 texture_cache_key(u64 source_hash, const TextureBuildOptions& options) noexcept {
  u64 key = hash_combine(source_hash, 0x7465787475726521ull);  // "texture!": not a cluster key
  key = hash_combine(key, k_texture_cache_version);
  key = hash_combine(key, pack_texture_options(options));
  // A mask's chain has a version of its own, so changing it rebuilds the masks and nothing else.
  return options.alpha_cutoff != 0 ? hash_combine(key, k_alpha_mask_version) : key;
}

std::string texture_cache_path(std::string_view ddc_root, u64 key) {
  constexpr char k_digits[] = "0123456789abcdef";
  char name[17];
  for (u32 i = 0; i < 16; ++i)
    name[i] = k_digits[(key >> ((15 - i) * 4)) & 0xfull];
  name[16] = '\0';
  return io::join_path(io::join_path(ddc_root, "textures"), std::string(name) + ".tex");
}

f64 alpha_coverage(std::span<const u8> rgba, u32 width, u32 height, u8 cutoff) noexcept {
  const usize texels = static_cast<usize>(width) * height;
  if (texels == 0 || rgba.size() < texels * 4) return 0.0;
  usize kept = 0;
  for (usize i = 0; i < texels; ++i)
    if (rgba[i * 4 + 3] >= cutoff) ++kept;
  return static_cast<f64>(kept) / static_cast<f64>(texels);
}

void dilate_under_mask(Vector<u8>& rgba, u32 width, u32 height, u8 cutoff, u32 rings) {
  const usize texels = static_cast<usize>(width) * height;
  if (texels == 0 || rgba.size() < texels * 4) return;
  // 1: the texel's colour is the leaf's (it passes, or an earlier ring filled it); 0: not yet.
  Vector<u8> filled(static_cast<u32>(texels), 0);
  for (usize i = 0; i < texels; ++i)
    filled[static_cast<u32>(i)] = rgba.data()[i * 4 + 3] >= cutoff ? 1 : 0;
  Vector<u8> next;
  Vector<u8> colour;
  for (u32 ring = 0; ring < rings; ++ring) {
    next = filled;
    colour = rgba;
    const u8* src = rgba.data();
    u8* dst = colour.data();
    bool grew = false;
    for (u32 y = 0; y < height; ++y) {
      for (u32 x = 0; x < width; ++x) {
        const usize i = static_cast<usize>(y) * width + x;
        if (filled[static_cast<u32>(i)] != 0) continue;
        u32 sum[3] = {0, 0, 0};
        u32 count = 0;
        for (i32 dy = -1; dy <= 1; ++dy) {
          for (i32 dx = -1; dx <= 1; ++dx) {
            const i32 nx = static_cast<i32>(x) + dx;
            const i32 ny = static_cast<i32>(y) + dy;
            if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= static_cast<i32>(width) ||
                ny >= static_cast<i32>(height)) {
              continue;
            }
            const usize n = static_cast<usize>(ny) * width + static_cast<usize>(nx);
            if (filled[static_cast<u32>(n)] == 0) continue;
            for (u32 c = 0; c < 3; ++c)
              sum[c] += src[n * 4 + c];
            ++count;
          }
        }
        if (count == 0) continue;
        // Integer arithmetic, rounded half up: the same bytes from every compiler.
        for (u32 c = 0; c < 3; ++c)
          dst[i * 4 + c] = static_cast<u8>((sum[c] + count / 2) / count);
        next[static_cast<u32>(i)] = 1;
        grew = true;
      }
    }
    rgba = colour;
    filled = next;
    if (!grew) break;
  }
}

namespace {

// The texel a bilinear tap reads one step along an axis: the sampler's wrap, seen from the build.
// A clamped and a mirrored edge both read the edge texel again.
u32 neighbour_texel(u32 i, bool forward, u32 size, EdgeMode mode) noexcept {
  if (forward) return i + 1 < size ? i + 1 : (mode == EdgeMode::repeat ? 0u : size - 1);
  return i > 0 ? i - 1 : (mode == EdgeMode::repeat ? size - 1 : 0u);
}

// How many of the sixteen samples a texel of an alpha plane the filtered surface passes at
// (`alpha_coverage_filtered`), summed over the plane. A sample at 1/8, 3/8, 5/8 or 7/8 of a texel
// is 3/8 or 1/8 of a texel from its centre, so the tap there weighs the texel 5 or 7 eighths and
// its neighbour on that side the rest; the two axes' weights multiply, and the blend is an integer
// in sixty-fourths of a code.
u64 filtered_samples_kept(const u8* alpha, u32 width, u32 height, u8 cutoff, EdgeMode edge_x,
                          EdgeMode edge_y) noexcept {
  constexpr u32 k_own[4] = {5, 7, 7, 5};
  const u32 pass = static_cast<u32>(cutoff) * 64u;
  u64 kept = 0;
  for (u32 y = 0; y < height; ++y) {
    const u8* rows[3] = {
        alpha + static_cast<usize>(neighbour_texel(y, false, height, edge_y)) * width,
        alpha + static_cast<usize>(y) * width,
        alpha + static_cast<usize>(neighbour_texel(y, true, height, edge_y)) * width};
    for (u32 x = 0; x < width; ++x) {
      const u32 columns[3] = {neighbour_texel(x, false, width, edge_x), x,
                              neighbour_texel(x, true, width, edge_x)};
      u32 a[3][3];
      u32 least = 255;
      u32 most = 0;
      for (u32 j = 0; j < 3; ++j) {
        for (u32 i = 0; i < 3; ++i) {
          a[j][i] = rows[j][columns[i]];
          least = a[j][i] < least ? a[j][i] : least;
          most = a[j][i] > most ? a[j][i] : most;
        }
      }
      // Most texels of a mask are deep inside it or far outside: every blend of a neighbourhood
      // lies between its least and its most.
      if (least >= cutoff) {
        kept += 16;
        continue;
      }
      if (most < cutoff) continue;
      for (u32 sy = 0; sy < 4; ++sy) {
        const u32 wy = k_own[sy];
        const u32 jy = sy < 2 ? 0u : 2u;
        for (u32 sx = 0; sx < 4; ++sx) {
          const u32 wx = k_own[sx];
          const u32 ix = sx < 2 ? 0u : 2u;
          const u32 blend = a[1][1] * wx * wy + a[1][ix] * (8 - wx) * wy +
                            a[jy][1] * wx * (8 - wy) + a[jy][ix] * (8 - wx) * (8 - wy);
          if (blend >= pass) ++kept;
        }
      }
    }
  }
  return kept;
}

}  // namespace

f64 alpha_coverage_filtered(std::span<const u8> rgba, u32 width, u32 height, u8 cutoff,
                            EdgeMode edge_x, EdgeMode edge_y) noexcept {
  const usize texels = static_cast<usize>(width) * height;
  if (texels == 0 || rgba.size() < texels * 4) return 0.0;
  Vector<u8> alpha(static_cast<u32>(texels), u8{0});
  for (usize i = 0; i < texels; ++i)
    alpha[static_cast<u32>(i)] = rgba[i * 4 + 3];
  return static_cast<f64>(
             filtered_samples_kept(alpha.data(), width, height, cutoff, edge_x, edge_y)) /
         (16.0 * static_cast<f64>(texels));
}

void preserve_alpha_coverage(Vector<u8>& rgba, u32 width, u32 height, u8 cutoff, f64 coverage,
                             EdgeMode edge_x, EdgeMode edge_y) {
  const usize texels = static_cast<usize>(width) * height;
  if (texels == 0 || rgba.size() < texels * 4 || cutoff == 0) return;
  u8* px = rgba.data();
  Vector<u8> alpha(static_cast<u32>(texels), u8{0});
  for (usize i = 0; i < texels; ++i)
    alpha[static_cast<u32>(i)] = px[i * 4 + 3];
  Vector<u8> scaled(static_cast<u32>(texels), u8{0});
  // The level's alpha as the scale for `t` leaves it: `cutoff / t`, rounded down, clamped.
  auto scale_for = [&](u32 t, u8* table) {
    for (u32 v = 0; v < 256; ++v) {
      const u32 s = v * cutoff / t;
      table[v] = static_cast<u8>(s > 255u ? 255u : s);
    }
  };
  const f64 samples = 16.0 * static_cast<f64>(texels);
  auto share_at = [&](u32 t) {
    u8 table[256];
    scale_for(t, table);
    for (usize i = 0; i < texels; ++i)
      scaled[static_cast<u32>(i)] = table[alpha[static_cast<u32>(i)]];
    return static_cast<f64>(
               filtered_samples_kept(scaled.data(), width, height, cutoff, edge_x, edge_y)) /
           samples;
  };
  // The share falls as t rises (a smaller scale passes no sample a larger one refused). The
  // largest t that still reaches the coverage, then whichever of it and the next is closer: the
  // larger on a tie, which is the smaller change to the level.
  u32 best = 1;
  if (share_at(1) >= coverage) {
    u32 lo = 1;
    u32 hi = 255;
    while (lo < hi) {
      const u32 mid = (lo + hi + 1) / 2;
      if (share_at(mid) >= coverage) {
        lo = mid;
      } else {
        hi = mid - 1;
      }
    }
    best = lo;
    if (lo < 255 && std::fabs(share_at(lo + 1) - coverage) <= std::fabs(share_at(lo) - coverage)) {
      best = lo + 1;
    }
  }
  // A level the cutoff itself holds as well as any scale is left alone, byte for byte.
  if (best != cutoff &&
      std::fabs(share_at(cutoff) - coverage) <= std::fabs(share_at(best) - coverage)) {
    best = cutoff;
  }
  if (best == cutoff) return;
  u8 table[256];
  scale_for(best, table);
  for (usize i = 0; i < texels; ++i)
    px[i * 4 + 3] = table[px[i * 4 + 3]];
}

bool build_texture(std::span<const u8> rgba, u32 width, u32 height, u32 source_channels,
                   const TextureBuildOptions& options, TextureData& out, jobs::JobSystem* pool,
                   std::string* error, TextureBuildReport* report) {
  out = TextureData{};
  if (width == 0 || height == 0) return fail(error, "the source image has no texels");
  if (width > k_max_side || height > k_max_side) {
    return fail(error, "the source image is " + std::to_string(width) + "x" +
                           std::to_string(height) + "; the texture build takes sides up to " +
                           std::to_string(k_max_side));
  }
  if (rgba.size() < static_cast<usize>(width) * height * 4)
    return fail(error, "the source pixels are shorter than width x height x 4");
  TextureFormat format = TextureFormat::bc7;
  if (!resolve_texture_format(options, source_channels, format, error)) return false;

  const bool srgb = options.color_space == ColorSpace::srgb && !options.normal_map;
  const MipMode mode =
      options.normal_map ? MipMode::normal : (srgb ? MipMode::srgb : MipMode::linear);
  TextureBuildReport local;
  TextureBuildReport& r = report != nullptr ? *report : local;

  out.format = format;
  out.color_space = srgb ? ColorSpace::srgb : ColorSpace::linear;
  out.width = width;
  out.height = height;
  out.source_channels = source_channels;
  if (options.normal_map) out.flags |= k_texture_normal_map;
  for (usize i = 3; i < static_cast<usize>(width) * height * 4; i += 4) {
    if (rgba[i] != 255) {
      out.flags |= k_texture_has_alpha;
      break;
    }
  }

  // **An alpha-tested mask** (texture.md, "Alpha-tested masks"): the colour under the cut grown
  // outwards first, so the chain filters the leaf's colour into its fringe, and every level's alpha
  // then held to level 0's coverage at the cutoff. Only a build given a cutoff, of an image that
  // has alpha, of a format that keeps it; everything else is the bytes it always was.
  const bool masked = options.alpha_cutoff != 0 && (out.flags & k_texture_has_alpha) != 0 &&
                      !options.normal_map && (stored_channels(format) & 0x8u) != 0;
  Vector<u8> dilated;
  std::span<const u8> source = rgba;
  if (masked) {
    dilated.assign(rgba.data(), rgba.data() + static_cast<usize>(width) * height * 4);
    dilate_under_mask(dilated, width, height, options.alpha_cutoff, k_alpha_dilation_texels);
    source = std::span<const u8>(dilated.data(), dilated.size());
  }

  const i64 mip_start = time::monotonic_ns();
  Vector<MipLevel> chain;
  build_mip_chain(source, width, height, mode, options.mips, chain, pool, options.edge_x,
                  options.edge_y);
  // What a linear sampler draws of level 0 (the ray loops test it there, and a rasterizer does
  // wherever the mask is magnified): the coverage every level below is held to.
  const f64 coverage = masked ? alpha_coverage_filtered(source, width, height, options.alpha_cutoff,
                                                        options.edge_x, options.edge_y)
                              : 0.0;
  if (masked) out.flags |= k_texture_alpha_coverage;
  r.mip_ms = ms_since(mip_start);

  // The level table first, so the payload is sized once and each level encodes into its own
  // aligned range.
  u64 offset = 0;
  out.levels.reserve(chain.size());
  for (const MipLevel& level : chain) {
    offset = (offset + k_texture_file_alignment - 1) / k_texture_file_alignment *
             k_texture_file_alignment;
    TextureFileLevel record;
    record.width = level.width;
    record.height = level.height;
    record.offset = offset;
    record.bytes = texture_level_bytes(format, level.width, level.height);
    out.levels.push_back(record);
    offset += record.bytes;
  }
  if (offset > std::numeric_limits<u32>::max())
    return fail(error, "the texture's blocks would not fit one payload");
  out.data.resize(static_cast<u32>(offset));

  // Perceptual (YCbCr) error weighting is right for colour a person looks at and wrong for data a
  // shader multiplies by: a roughness map's green channel is not "more visible" than its blue.
  const bool perceptual = srgb;
  const i64 encode_start = time::monotonic_ns();
  Vector<u8> unscaled;
  Vector<u8> check;
  Vector<u8> best_blocks;
  Vector<u8> best_rgba;
  for (u32 i = 0; i < chain.size(); ++i) {
    MipLevel& level = chain[i];
    const TextureFileLevel& record = out.levels[i];
    const std::span<u8> blocks(out.data.data() + record.offset, static_cast<usize>(record.bytes));
    if (!masked) {
      encode_level(format, std::span<const u8>(level.rgba.data(), level.rgba.size()), level.width,
                   level.height, perceptual, blocks, pool);
      continue;
    }
    // A mask's level: its alpha scaled to the coverage, encoded, and measured **as the GPU will
    // read it**, decoded. The encoder moves an alpha by a code or two, and a filtered level holds
    // many texels within a code of the cutoff (every needle a texel wide sits at half alpha one
    // level down), so a scale that is exact before the encoder can be 3% off after it. The miss
    // is fed back into the target and the level is scaled again from its unscaled alpha, at most
    // `k_coverage_passes` times: deterministic, because the encoder is, and only a mask pays it.
    unscaled = level.rgba;
    if (level.width == 1 && level.height == 1) {
      // **One texel holds all or nothing**, and a card two pixels across is drawn from the blend
      // of this level with the one above. So it is stored where that blend is decided by the
      // level above, which does hold the share: at the cutoff when the coverage rounds to all,
      // one code under it when it rounds to nothing (a level far under the cutoff drags every
      // blend past its half under with it, and a far crown's cards vanished there). The encoder
      // may move a code, so the side is read back and the alpha stepped away until it holds.
      const bool all = coverage >= 0.5;
      for (u32 step = 0; step < 8; ++step) {
        const i32 wanted =
            all ? static_cast<i32>(options.alpha_cutoff) + static_cast<i32>(step)
                : static_cast<i32>(options.alpha_cutoff) - 1 - static_cast<i32>(step);
        level.rgba = unscaled;
        level.rgba[3] = static_cast<u8>(wanted < 0 ? 0 : (wanted > 255 ? 255 : wanted));
        encode_level(format, std::span<const u8>(level.rgba.data(), level.rgba.size()), level.width,
                     level.height, perceptual, blocks, pool);
        if (!decode_level(format, std::span<const u8>(blocks.data(), blocks.size()), level.width,
                          level.height, check, nullptr)) {
          break;
        }
        if ((check[3] >= options.alpha_cutoff) == all) break;
      }
      continue;
    }
    // The pass that landed closest is the one kept: a small level can step over the coverage and
    // back, and the last pass is then no better than the first.
    f64 target = coverage;
    f64 best_miss = std::numeric_limits<f64>::infinity();
    bool last_is_best = true;
    for (u32 pass = 0; pass < k_coverage_passes; ++pass) {
      level.rgba = unscaled;
      preserve_alpha_coverage(level.rgba, level.width, level.height, options.alpha_cutoff, target,
                              options.edge_x, options.edge_y);
      encode_level(format, std::span<const u8>(level.rgba.data(), level.rgba.size()), level.width,
                   level.height, perceptual, blocks, pool);
      if (!decode_level(format, std::span<const u8>(blocks.data(), blocks.size()), level.width,
                        level.height, check, nullptr)) {
        break;
      }
      const f64 measured = alpha_coverage_filtered(std::span<const u8>(check.data(), check.size()),
                                                   level.width, level.height, options.alpha_cutoff,
                                                   options.edge_x, options.edge_y);
      const f64 miss = coverage - measured;
      last_is_best = std::fabs(miss) < best_miss;
      if (last_is_best) {
        best_miss = std::fabs(miss);
        best_blocks.assign(blocks.data(), blocks.data() + blocks.size());
        best_rgba = level.rgba;
      }
      if (std::fabs(miss) <= k_coverage_slack) break;
      target = target + miss;
      target = target < 0.0 ? 0.0 : (target > 1.0 ? 1.0 : target);
    }
    if (!last_is_best && best_blocks.size() == blocks.size()) {
      std::memcpy(blocks.data(), best_blocks.data(), blocks.size());
      level.rgba = best_rgba;
    }
  }
  if (options.normal_map &&
      options.mips) {  // an unmipped map resolves every texel: nothing to keep
    Vector<MipLevel> variance;
    build_normal_variance_chain(source, width, height, options.mips, variance, pool, options.edge_x,
                                options.edge_y);
    u64 variance_bytes = 0;
    for (const MipLevel& level : variance)
      variance_bytes = (variance_bytes + 15u) / 16u * 16u +
                       texture_level_bytes(TextureFormat::bc4, level.width, level.height);
    if (variance_bytes > std::numeric_limits<u32>::max())
      return fail(error, "normal variance would not fit one payload");
    out.normal_variance.resize(static_cast<u32>(variance_bytes), 0);
    u64 at = 0;
    for (const MipLevel& level : variance) {
      at = (at + 15u) / 16u * 16u;
      const u64 bytes = texture_level_bytes(TextureFormat::bc4, level.width, level.height);
      encode_level(TextureFormat::bc4, {level.rgba.data(), level.rgba.size()}, level.width,
                   level.height, false,
                   {out.normal_variance.data() + at, static_cast<usize>(bytes)}, pool);
      at += bytes;
    }
  }
  r.encode_ms = ms_since(encode_start);

  // How far level 0 moved: decoded back and compared with what the encoder was given.
  Vector<u8> decoded;
  if (decode_level(format, out.level_bytes(0), width, height, decoded, nullptr)) {
    const u32 mask = stored_channels(format);
    const u8* a = chain[0].rgba.data();
    const u8* b = decoded.data();
    f64 squared = 0.0;
    u64 samples = 0;
    u32 worst = 0;
    for (usize i = 0; i < static_cast<usize>(width) * height; ++i) {
      for (u32 c = 0; c < 4; ++c) {
        if ((mask & (1u << c)) == 0) continue;
        const i32 d = static_cast<i32>(a[i * 4 + c]) - static_cast<i32>(b[i * 4 + c]);
        const u32 ad = static_cast<u32>(d < 0 ? -d : d);
        worst = ad > worst ? ad : worst;
        squared += static_cast<f64>(d) * static_cast<f64>(d);
        ++samples;
      }
    }
    r.max_error = worst;
    const f64 mse = samples != 0 ? squared / static_cast<f64>(samples) : 0.0;
    r.psnr =
        mse == 0.0 ? std::numeric_limits<f64>::infinity() : 10.0 * std::log10(255.0 * 255.0 / mse);
  }
  return true;
}

bool build_texture_from_encoded(std::span<const u8> encoded, const TextureBuildOptions& options,
                                TextureData& out, jobs::JobSystem* pool, std::string* error,
                                TextureBuildReport* report) {
  out = TextureData{};
  TextureBuildReport local;
  TextureBuildReport& r = report != nullptr ? *report : local;
  const i64 decode_start = time::monotonic_ns();
  image::ImageInfo info;
  std::string message;
  if (!image::probe_image(encoded, info, &message)) return fail(error, message);
  image::Image decoded;
  if (!image::decode_image(encoded, decoded, 4, &message)) return fail(error, message);
  r.decode_ms = ms_since(decode_start);
  if (!build_texture(std::span<const u8>(decoded.pixels.data(), decoded.pixels.size()),
                     decoded.width, decoded.height, info.channels, options, out, pool, error, &r)) {
    return false;
  }
  out.source_hash = hash_bytes(encoded.data(), encoded.size());
  out.build_key = texture_cache_key(out.source_hash, options);
  return true;
}

}  // namespace engine::texture
