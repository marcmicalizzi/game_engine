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
constexpr u32 k_pack_known = 0xfu | k_pack_srgb | k_pack_normal | k_pack_mips | k_pack_valid |
                             (3u << k_pack_edge_x_shift) | (3u << k_pack_edge_y_shift);

// The largest side the builder takes. Level 1's working set is a float image of half the source
// and a horizontal pass of half its width at full height, about 12 bytes a source texel, and the
// engine's `Vector` counts in 32 bits: 16,384 on a side is 3 GiB of floats in the worst pass,
// which is past what that holds.
constexpr u32 k_max_side = 16384;

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
  return hash_combine(key, pack_texture_options(options));
}

std::string texture_cache_path(std::string_view ddc_root, u64 key) {
  constexpr char k_digits[] = "0123456789abcdef";
  char name[17];
  for (u32 i = 0; i < 16; ++i)
    name[i] = k_digits[(key >> ((15 - i) * 4)) & 0xfull];
  name[16] = '\0';
  return io::join_path(io::join_path(ddc_root, "textures"), std::string(name) + ".tex");
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

  const i64 mip_start = time::monotonic_ns();
  Vector<MipLevel> chain;
  build_mip_chain(rgba, width, height, mode, options.mips, chain, pool, options.edge_x,
                  options.edge_y);
  r.mip_ms = ms_since(mip_start);

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
  for (u32 i = 0; i < chain.size(); ++i) {
    const MipLevel& level = chain[i];
    const TextureFileLevel& record = out.levels[i];
    encode_level(format, std::span<const u8>(level.rgba.data(), level.rgba.size()), level.width,
                 level.height, perceptual,
                 std::span<u8>(out.data.data() + record.offset, static_cast<usize>(record.bytes)),
                 pool);
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
