#pragma once

// The built clip, `.clip` (docs/subsystems/audio.md, "Built clips";
// docs/plan/07-content-pipeline.md §7.3): one clip decoded once, by the content build, into the mix
// format — f32, interleaved, mono or stereo, at 48 kHz — plus the identity of what it was built
// from. It is what `engine-content build` writes into the derived-data cache at
// `<ddc>/clips/<key>.clip`, and what the clip store loads as a copy, or streams by range, instead
// of decoding anything.
//
// It is the texture container's design (domain/texture/texture_file.h), itself the cluster
// container's, on purpose, so that one set of rules covers every derived artifact the engine
// writes:
//
//   header     32 bytes: magic "CLIP", version, flags, section_count, total_bytes, content_hash
//   sections   section_count records of { kind, element_size, element_count, offset }
//   payloads   at 16-byte aligned offsets from the start of the file, writer's order
//
// Little-endian, no pointers, every section an array of one fixed-size element that the reader
// and the writer move with memcpy. The kinds are fixed and **append-only**: a new kind takes the
// next free value, an old one is never renumbered or reused, and a reader skips a kind it does not
// know. Removing a required kind or changing an element's layout is a version bump.
// `content_hash` (core/hash `hash_bytes`) covers every byte after the header, section table
// included.
//
// **The samples are the section a stream reads.** They are one contiguous run of frames at a
// 16-byte aligned offset, so a voice streaming a built clip reads frame f at `samples_offset + f *
// channels * 4` by range, with no decoder at all (stream.h). A streamed file is checked by its
// header, its table and its identity, not by its content hash, which would read the frames the
// stream exists not to read.
//
// **The same bytes from every toolchain** (docs/subsystems/geometry.md, "The same bytes from every
// toolchain"; ADR-0035): a `.clip` is a function of its source's bytes and this build's decode,
// never of the compiler, the instruction set or the C library, which is why the cache key names
// none of them. Integer PCM converts to f32 exactly; a source at another rate goes through
// miniaudio's resampler, whose filter coefficients come from libm in double and are rounded to f32
// (docs/subsystems/audio.md, "Determinism", has the caveat and the pin that checks it); the golden
// hashes in tests/clip_file_tests.cpp are taken on MSVC and reproduced by GCC and Clang.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/audio/clip_store.h>
#include <foundation/io/vfs.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::audio {

inline constexpr u32 k_clip_file_version = 1;
inline constexpr u32 k_clip_file_alignment = 16;

// Bumped whenever the decode changes in a way that makes an old cache entry wrong — the mix rate,
// the resampler or its filter order, the channel fold, the container's layout — so an old entry is
// a miss and not a wrong clip. A cache miss is a decode; a wrong clip is a bug nobody sees.
inline constexpr u32 k_clip_cache_version = 1;

// Fixed, append-only section kinds. Unknown values are skipped on read.
enum class ClipSection : u32 {
  Desc = 1,     // exactly one ClipFileDesc
  Samples = 2,  // f32, frames x channels, interleaved: the clip at the mix rate
  // u64, two of them: [0] the content hash of the source's encoded bytes (`clip_source_hash`),
  // [1] the build key over it (`clip_cache_key`). Zero in a clip built from samples with no file
  // behind them, which an incremental build reads as "rebuild it".
  SourceHash = 3,
};
inline constexpr u32 k_clip_section_kinds = static_cast<u32>(ClipSection::SourceHash) + 1;
// The name of a kind this build knows, "unknown" otherwise.
const char* clip_section_name(u32 kind) noexcept;

// The header, memcpy'd in and out. 32 bytes, the same shape as the texture's.
struct ClipFileHeader {
  char magic[4] = {'C', 'L', 'I', 'P'};
  u32 version = k_clip_file_version;
  u32 flags = 0;
  u32 section_count = 0;
  u64 total_bytes = 0;
  u64 content_hash = 0;  // of every byte after the header
};

// One section record. 24 bytes.
struct ClipFileSection {
  u32 kind = 0;
  u32 element_size = 0;
  u64 element_count = 0;
  u64 offset = 0;  // from the start of the file, a multiple of k_clip_file_alignment
};

// What the clip is, as one single-element section. 32 bytes.
struct ClipFileDesc {
  u32 sample_rate = 0;  // the mix rate the samples are at: k_sample_rate, checked on read
  u32 channels = 0;     // 1 or 2
  u64 frames = 0;
  u32 source_channels = 0;  // what the source stored, for a reader's information
  u32 source_rate = 0;
  u32 flags = 0;  // none defined yet
  u32 reserved = 0;
};

static_assert(sizeof(ClipFileHeader) == 32, "the clip file header is 32 bytes on the wire");
static_assert(sizeof(ClipFileSection) == 24, "a clip file section record is 24 bytes");
static_assert(sizeof(ClipFileDesc) == 32, "the clip file description is 32 bytes");

// Everything one file holds.
struct ClipFileData {
  u32 channels = 0;
  u32 frames = 0;
  u32 source_channels = 0;
  u32 source_rate = 0;
  Vector<f32> samples;  // frames x channels, interleaved
  u64 source_hash = 0;
  u64 build_key = 0;
};

// What a stream needs of a file, read from its front by range: the clip's shape, where its frames
// start, and its identity.
struct ClipFileLayout {
  u32 channels = 0;
  u32 frames = 0;
  u32 source_channels = 0;
  u32 source_rate = 0;
  u64 samples_offset = 0;
  u64 source_hash = 0;
  u64 build_key = 0;
};

// The source's identity: `hash_bytes` over its encoded bytes exactly as read — the convention every
// derived node's key starts from (`texture::` and `assets::source_mesh_hash` take the same).
u64 clip_source_hash(std::span<const u8> encoded) noexcept;
// The derived-data cache key: the source hash mixed with `k_clip_cache_version` and the mix format
// (the rate and the two-channel fold). Nothing about the machine.
u64 clip_cache_key(u64 source_hash) noexcept;
// "<ddc_root>/clips/<key as 16 lower-case hex digits>.clip".
std::string clip_cache_path(std::string_view ddc_root, u64 key);

// Decodes `encoded` (WAV, FLAC or MP3; the store's decoders, `decode_clip`) into `out` and records
// its identity — `clip_source_hash(encoded)` and the key over it — so that what it builds is
// exactly the entry `clip_cache_path` names for those bytes.
DecodeStatus build_clip(std::span<const u8> encoded, ClipFileData& out);

// Checks what the reader relies on: one or two channels, at least one frame, and exactly frames x
// channels samples. The writer refuses what this refuses.
bool validate_clip(const ClipFileData& data, std::string* error = nullptr);

// Writes the file through io::write_file_atomic, so a reader sees the old bytes or the new ones and
// never a mix. Parent directories are not created. False, with `error`, on a failure.
bool write_clip_file(std::string_view path, const ClipFileData& data, std::string* error = nullptr);

// Reads a file written by `write_clip_file`. `out` is replaced, and left empty on failure: an
// unreadable file, a truncated one, the wrong magic or version, a section that runs past the end, a
// content hash that does not match, a missing required section, a rate that is not the mix rate,
// or samples that do not validate.
bool read_clip_file(std::string_view path, ClipFileData& out, std::string* error = nullptr);
bool read_clip_file_memory(std::span<const u8> bytes, ClipFileData& out,
                           std::string* error = nullptr);

// The front of a file, by range: the header, the table, the description and the identity, with
// every structural check `read_clip_file` makes except the content hash. What a stream opens.
bool read_clip_file_layout(const io::FileHandle& file, ClipFileLayout& out,
                           std::string* error = nullptr);

// Just the identity, with the header, the table and the content hash checked as a full read checks
// them. Both words are zero for a file that recorded none. What an incremental build skips on.
bool read_clip_file_identity(std::string_view path, u64& source_hash, u64& build_key,
                             std::string* error = nullptr);

// The content hash the writer stores in the header, computed without writing anything.
u64 clip_file_hash(const ClipFileData& data);

// True when `bytes` starts with the clip magic — how the store and `engine-content info` tell a
// `.clip` from an encoded source whatever the files are called.
bool is_clip_file(std::span<const u8> bytes) noexcept;

// The header and section table of a file that passes the structural checks and the content hash.
bool read_clip_file_table(std::span<const u8> bytes, ClipFileHeader& header,
                          Vector<ClipFileSection>& sections, std::string* error = nullptr);

}  // namespace engine::audio
