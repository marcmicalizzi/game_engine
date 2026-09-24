#pragma once

// The built texture, `.tex` (docs/subsystems/texture.md, docs/plan/07-content-pipeline.md §7.2,
// ADR-0036): one image as the GPU samples it — a block-compressed format, a color space, and a
// full mip chain — plus the identity of what it was built from. It is what `engine-content`
// writes into the derived-data cache at `<ddc>/textures/<key>.tex` and what the renderer uploads
// without decoding anything.
//
// It is the cluster container's design (domain/geometry/cluster_file.h) applied to a texture, on
// purpose, so that one set of rules covers every derived artifact the engine writes:
//
//   header     32 bytes: magic "TEXF", version, flags, section_count, total_bytes, content_hash
//   sections   section_count records of { kind, element_size, element_count, offset }
//   payloads   at 16-byte aligned offsets from the start of the file, writer's order
//
// Little-endian, no pointers, every section an array of one fixed-size element that the reader
// and the writer move with memcpy. The kinds are fixed and **append-only**: a new kind takes the
// next free value, an old one is never renumbered or reused, and a reader skips a kind it does not
// know, so a file written by a newer build still loads as long as the sections this build requires
// are there. Removing a required kind or changing an element's layout is a version bump.
// `content_hash` (core/hash `hash_bytes`) covers every byte after the header, section table
// included, and `texture_file_hash` computes the same number from a `TextureData` in memory.
//
// **Why not KTX2**, which plan 07 §7.2 named: see ADR-0036. In short, KTX2 is an interchange
// format with a data-format descriptor, supercompression and key-value metadata this engine
// neither writes nor wants to parse on load, it has no place for the source identity an
// incremental build reads, and it would be the only derived artifact not under the append-only
// section rule. An exporter to KTX2 is a function of this file and can come when something needs
// one.

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::texture {

inline constexpr u32 k_texture_file_version = 1;
inline constexpr u32 k_texture_file_alignment = 16;  // every payload and every level's offset

// What a level's bytes are. The numbers are the file's and never change; a new format takes the
// next free value. `rgba8` is the uncompressed escape hatch (`--format rgba8`), four bytes a texel;
// every other format is 4x4 blocks, and a level whose sides are not multiples of four still takes
// whole blocks (the edge texels are repeated into the padding by the encoder).
enum class TextureFormat : u32 {
  rgba8 = 1,
  bc1 = 2,  // RGB, 8 bytes a block, no alpha (the build never writes punch-through)
  bc3 = 3,  // RGBA, 16 bytes a block: BC1 colour plus a BC4 alpha
  bc4 = 4,  // one channel (R), 8 bytes a block, UNORM only
  bc5 = 5,  // two channels (RG), 16 bytes a block, UNORM only: tangent-space normals
  bc7 = 6,  // RGBA, 16 bytes a block
};

// Whether the stored values are sRGB-encoded colour (sampled through an _SRGB view, so a shader
// reads linear) or linear data. BC4 and BC5 have no sRGB form in any API, so they are always
// linear.
enum class ColorSpace : u32 { linear = 0, srgb = 1 };

// TextureFileDesc::flags.
inline constexpr u32 k_texture_normal_map = 1u;  // xy of a unit tangent-space normal, z implied
inline constexpr u32 k_texture_has_alpha = 2u;   // the source had a texel with alpha below 255

const char* texture_format_name(TextureFormat format) noexcept;
const char* color_space_name(ColorSpace space) noexcept;
// Accepts the names `texture_format_name` prints. False, leaving `out` alone, for anything else.
bool parse_texture_format(std::string_view name, TextureFormat& out) noexcept;
bool texture_format_known(u32 value) noexcept;
// 4 for the block formats, 1 for rgba8.
u32 texture_block_extent(TextureFormat format) noexcept;
// Bytes per block (per texel for rgba8).
u32 texture_block_bytes(TextureFormat format) noexcept;
// Bytes one level of `width` x `height` texels takes, in whole blocks.
u64 texture_level_bytes(TextureFormat format, u32 width, u32 height) noexcept;
// The full chain from `width` x `height` down to 1 x 1: floor(log2(max side)) + 1.
u32 texture_full_level_count(u32 width, u32 height) noexcept;
// A side of level `level`: the level-0 side halved `level` times, rounded down, never below 1 —
// what Vulkan, D3D and every GPU compute for a mip level.
constexpr u32 texture_level_extent(u32 extent, u32 level) noexcept {
  const u32 e = level >= 32 ? 0u : extent >> level;
  return e == 0 ? 1u : e;
}

// Fixed, append-only section kinds. Unknown values are skipped on read.
enum class TextureSection : u32 {
  Desc = 1,    // exactly one TextureFileDesc
  Levels = 2,  // TextureFileLevel, one per mip level, level 0 (the largest) first
  Data = 3,    // u8: every level's blocks back to back, each at a 16-byte aligned offset
  // u64, two of them: [0] the content hash of the source image's encoded bytes (`hash_bytes`
  // over the PNG or JPEG exactly as it was read), [1] the build key over that hash and the build
  // options (`texture_cache_key`). Zero in a texture built from pixels with no file behind them,
  // which an incremental build reads as "rebuild it".
  SourceHash = 4,
};
inline constexpr u32 k_texture_section_kinds = static_cast<u32>(TextureSection::SourceHash) + 1;
// The name of a kind this build knows, "unknown" otherwise; for diagnostics and `engine-content
// info`.
const char* texture_section_name(u32 kind) noexcept;

// The header, memcpy'd in and out. 32 bytes; the same shape as the cluster container's.
struct TextureFileHeader {
  char magic[4] = {'T', 'E', 'X', 'F'};
  u32 version = k_texture_file_version;
  u32 flags = 0;
  u32 section_count = 0;
  u64 total_bytes = 0;
  u64 content_hash = 0;  // of every byte after the header
};

// One section record. 24 bytes.
struct TextureFileSection {
  u32 kind = 0;
  u32 element_size = 0;
  u64 element_count = 0;
  u64 offset = 0;  // from the start of the file, a multiple of k_texture_file_alignment
};

// What the texture is, as one single-element section. 32 bytes.
struct TextureFileDesc {
  u32 format = 0;       // TextureFormat
  u32 color_space = 0;  // ColorSpace
  u32 width = 0;        // level 0, in texels
  u32 height = 0;
  u32 level_count = 0;
  u32 flags = 0;            // k_texture_normal_map | k_texture_has_alpha
  u32 source_channels = 0;  // what the source file stored (1 to 4), for a reader's information
  u32 reserved = 0;
};

// One mip level. 24 bytes. `offset` is into the `Data` payload, not into the file.
struct TextureFileLevel {
  u32 width = 0;
  u32 height = 0;
  u64 offset = 0;
  u64 bytes = 0;
};

static_assert(sizeof(TextureFileHeader) == 32, "the texture file header is 32 bytes on the wire");
static_assert(sizeof(TextureFileSection) == 24, "a texture file section record is 24 bytes");
static_assert(sizeof(TextureFileDesc) == 32, "the texture file description is 32 bytes");
static_assert(sizeof(TextureFileLevel) == 24, "a texture file level record is 24 bytes");

// Everything one file holds.
struct TextureData {
  TextureFormat format = TextureFormat::rgba8;
  ColorSpace color_space = ColorSpace::linear;
  u32 width = 0;
  u32 height = 0;
  u32 flags = 0;
  u32 source_channels = 0;
  Vector<TextureFileLevel> levels;  // level 0 first; offsets into `data`
  Vector<u8> data;
  u64 source_hash = 0;
  u64 build_key = 0;

  // Level `index`'s bytes; empty for an index past the chain.
  std::span<const u8> level_bytes(u32 index) const noexcept;
};

// Checks what the reader relies on: a known format, a chain whose every level has the extent
// `texture_level_extent` gives it and exactly `texture_level_bytes` of data at an aligned offset
// inside `data`, BC4/BC5 not claiming sRGB. The writer refuses what this refuses.
bool validate_texture(const TextureData& data, std::string* error = nullptr);

// Writes the file through io::write_file_atomic, so a reader sees the old bytes or the new ones
// and never a mix. Parent directories are not created. Returns false and fills `error`.
bool write_texture_file(std::string_view path, const TextureData& data,
                        std::string* error = nullptr);

// Reads a file written by `write_texture_file`. `out` is replaced, and left empty on failure: an
// unreadable file, a truncated one, the wrong magic or version, a section that runs past the end,
// a content hash that does not match, a missing required section, or a chain that does not
// validate.
bool read_texture_file(std::string_view path, TextureData& out, std::string* error = nullptr);
bool read_texture_file_memory(std::span<const u8> bytes, TextureData& out,
                              std::string* error = nullptr);

// Just the `SourceHash` section, with the header, the section table and the content hash checked
// exactly as a full read checks them. Both words are zero for a file that recorded no identity.
bool read_texture_file_identity(std::string_view path, u64& source_hash, u64& build_key,
                                std::string* error = nullptr);

// The content hash the writer stores in the header, computed without writing anything.
u64 texture_file_hash(const TextureData& data);

// True when `bytes` starts with the texture magic — how `engine-content info` tells a `.tex` from
// a `.clusters` whatever the files are called.
bool is_texture_file(std::span<const u8> bytes) noexcept;

// The header and section table of a file that passes the structural checks and the content hash,
// read straight from the bytes, so a reader reports the sections a newer build wrote as well as
// the ones it understands.
bool read_texture_file_table(std::span<const u8> bytes, TextureFileHeader& header,
                             Vector<TextureFileSection>& sections, std::string* error = nullptr);

}  // namespace engine::texture
