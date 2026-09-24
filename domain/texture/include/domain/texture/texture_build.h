#pragma once

// Building a `.tex` from a source image (docs/subsystems/texture.md, docs/plan/07-content-
// pipeline.md §7.3): the mip chain, the block compression, and the derived-data cache key that
// addresses the result. A pure function of the source's bytes and the options — **not of the
// compiler, the instruction set, the C library or the thread count** — because the output lives in
// a content-addressed cache that more than one machine reads (ADR-0035, and the same rule the
// cluster build keeps). Every floating-point step here is IEEE arithmetic and `sqrt`; the sRGB
// transfer function is two committed tables rather than `std::pow`; the resampling kernel is a
// polynomial; parallel work writes fixed slots and nothing is merged by completion order.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/texture/texture_file.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::jobs {
class JobSystem;
}

namespace engine::texture {

// Bumped whenever the builder changes in a way that makes an old cache entry wrong — the filter,
// an encoder setting, the level-0 rule for normal maps, the encoders' pin. It is not the file
// format version: a cache miss is cheap, a wrong texture is not.
// 1: the first texture build (2026-09-24).
inline constexpr u32 k_texture_cache_version = 1;

// What a build is asked for. `format` may be `auto`, which `resolve_texture_format` turns into a
// concrete format from the options and what the source stored.
enum class FormatChoice : u8 {
  automatic = 0,
  rgba8 = 1,
  bc1 = 2,
  bc3 = 3,
  bc4 = 4,
  bc5 = 5,
  bc7 = 6
};

struct TextureBuildOptions {
  FormatChoice format = FormatChoice::automatic;
  ColorSpace color_space = ColorSpace::srgb;  // what the source's bytes are
  bool normal_map = false;  // a tangent-space normal map: renormalized, filtered as vectors
  bool mips = true;         // the full chain to 1x1; false writes level 0 alone
};

const char* format_choice_name(FormatChoice choice) noexcept;
bool parse_format_choice(std::string_view name, FormatChoice& out) noexcept;

// The options as one non-zero word, for the cache key and for the record a `.clusters` container
// keeps per image (geometry::ClusterFileTexture::options): bits 0..3 the format choice, bit 4 sRGB,
// bit 5 normal map, bit 6 mips, bit 7 always set so that no valid packing is zero — zero is the
// record's "no texture wanted". `unpack` refuses a word with unknown bits or an unknown format.
u32 pack_texture_options(const TextureBuildOptions& options) noexcept;
bool unpack_texture_options(u32 packed, TextureBuildOptions& out) noexcept;

// `auto`: a normal map is BC5; a one-channel linear source is BC4; everything else is BC7 (sRGB or
// linear as asked). An explicit format is checked: BC4 and BC5 have no sRGB form, so asking for
// either with sRGB is refused rather than silently stored linear. `source_channels` is what the
// file stored (1 to 4).
bool resolve_texture_format(const TextureBuildOptions& options, u32 source_channels,
                            TextureFormat& out, std::string* error = nullptr) noexcept;

// The derived-data cache key: the source's content hash (`hash_bytes` over its encoded bytes)
// mixed with the packed options and `k_texture_cache_version`. Nothing about the machine.
u64 texture_cache_key(u64 source_hash, const TextureBuildOptions& options) noexcept;

// "<ddc_root>/textures/<key as 16 lower-case hex digits>.tex".
std::string texture_cache_path(std::string_view ddc_root, u64 key);

// What one build did and what it cost, for the tool's JSON line.
struct TextureBuildReport {
  f64 decode_ms = 0.0;
  f64 mip_ms = 0.0;
  f64 encode_ms = 0.0;
  // Level 0 decoded back and compared with the level the encoder was given, over the channels the
  // format stores: RGB for bc1, RGBA for bc3/bc7/rgba8, R for bc4, RG for bc5. Infinity when
  // identical.
  f64 psnr = 0.0;
  u32 max_error = 0;  // the largest per-channel difference, 0..255
};

// Builds from 8-bit pixels: `rgba` is `width * height * 4` bytes, rows top first. `source_channels`
// is what the file stored (it chooses `auto`'s format and is recorded). `pool` spreads the mip
// filter's rows and the encoder's block rows over the performance pool; null runs on the calling
// thread, and the bytes are the same either way. The identity words are left zero: see the next.
bool build_texture(std::span<const u8> rgba, u32 width, u32 height, u32 source_channels,
                   const TextureBuildOptions& options, TextureData& out, jobs::JobSystem* pool,
                   std::string* error = nullptr, TextureBuildReport* report = nullptr);

// Decodes an encoded image (PNG, JPEG, TGA, BMP through foundation/image) and builds it, recording
// `hash_bytes(encoded)` and `texture_cache_key` over it as the texture's identity — so that what
// this writes is exactly the entry `texture_cache_path` names for those bytes and options.
bool build_texture_from_encoded(std::span<const u8> encoded, const TextureBuildOptions& options,
                                TextureData& out, jobs::JobSystem* pool,
                                std::string* error = nullptr, TextureBuildReport* report = nullptr);

// ---- the pieces, public because the tests and the tool measure them -----------------------------

// sRGB <-> linear without the C library: a 256-entry decode table and the 255 thresholds between
// consecutive codes (the linear value whose sRGB encoding is exactly n + 0.5 over 255), both taken
// once in double precision and committed. `linear_to_srgb8` is a binary search over the thresholds,
// so it is exact rounding of the true transfer function up to the tables' own float rounding, and
// `linear_to_srgb8(srgb8_to_linear(n)) == n` for every n.
f32 srgb8_to_linear(u8 code) noexcept;
u8 linear_to_srgb8(f32 linear) noexcept;
std::span<const f32> srgb_decode_table() noexcept;     // 256 entries
std::span<const f32> srgb_threshold_table() noexcept;  // 255 entries

// How a level's texels are filtered: colour in linear light (the source is sRGB), data as stored
// (linear), or unit vectors (a tangent-space normal map: decoded to -1..1, filtered, renormalized).
enum class MipMode : u8 { srgb, linear, normal };

// The Mitchell-Netravali cubic (B = C = 1/3) the chain is filtered with, at `x` in units of the
// *destination* texel. Support [-2, 2]. A polynomial, so every compiler evaluates it identically.
f32 mip_kernel(f32 x) noexcept;

// One level of a chain in 8-bit RGBA.
struct MipLevel {
  u32 width = 0;
  u32 height = 0;
  Vector<u8> rgba;
};

// The chain from `rgba` down to 1x1 (or level 0 alone without `mips`). Level 0 is the source's
// bytes unchanged for `srgb` and `linear`; for `normal` it is the source renormalized (a vector the
// 8-bit quantization left short of unit is put back on the sphere, because a two-channel format
// reconstructs z from x and y and would otherwise reconstruct the wrong direction). Every further
// level is filtered from the previous level's floats, not from its 8-bit rounding, by a separable
// Mitchell-Netravali kernel with clamped edges — the edge rule the renderer's sampler uses — at the
// exact ratio of the two sizes, so an odd side is filtered rather than dropped.
void build_mip_chain(std::span<const u8> rgba, u32 width, u32 height, MipMode mode, bool mips,
                     Vector<MipLevel>& out, jobs::JobSystem* pool = nullptr);

// Encodes one level of RGBA8 into `format`'s blocks (`texture_level_bytes` of them). Normal maps
// go to BC5 as R and G; BC4 takes R. `perceptual` weights BC7's error in YCbCr, which is right for
// sRGB colour and wrong for data.
void encode_level(TextureFormat format, std::span<const u8> rgba, u32 width, u32 height,
                  bool perceptual, std::span<u8> out, jobs::JobSystem* pool = nullptr);

// Decodes one level back to RGBA8, as a GPU samples it at the texel centers: BC4 fills R and
// leaves G and B zero and A 255, BC5 fills R and G and leaves B zero, which is also what Vulkan
// returns for those formats. For tests and for the tool's error figure.
bool decode_level(TextureFormat format, std::span<const u8> blocks, u32 width, u32 height,
                  Vector<u8>& rgba, std::string* error = nullptr);

}  // namespace engine::texture
