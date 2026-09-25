// The `.clip` container (clip_file.h). The layout and the checks are the texture container's
// (domain/texture/src/texture_file.cpp), itself the cluster container's, so a reader of one reads
// the others.
#include <core/hash/hash.h>
#include <domain/audio/clip_file.h>
#include <domain/audio/format.h>

#include <cstddef>
#include <cstring>
#include <utility>

namespace engine::audio {

namespace {

constexpr u64 k_header_bytes = sizeof(ClipFileHeader);
constexpr u64 k_record_bytes = sizeof(ClipFileSection);

constexpr u64 align_up(u64 value) noexcept {
  const u64 a = k_clip_file_alignment;
  return (value + a - 1) / a * a;
}

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

std::string section_label(u32 kind) {
  return std::string(clip_section_name(kind)) + " (kind " + std::to_string(kind) + ")";
}

struct Payload {
  ClipFileSection record;
  const void* data = nullptr;
};

void add_section(Vector<Payload>& out, ClipSection kind, u32 element_size, u64 element_count,
                 const void* data) {
  Payload payload;
  payload.record.kind = static_cast<u32>(kind);
  payload.record.element_size = element_size;
  payload.record.element_count = element_count;
  payload.data = data;
  out.push_back(payload);
}

// Serializes `data` into `out` and returns the content hash the header carries.
u64 encode(const ClipFileData& data, std::string& out) {
  ClipFileDesc desc;
  desc.sample_rate = k_sample_rate;
  desc.channels = data.channels;
  desc.frames = data.frames;
  desc.source_channels = data.source_channels;
  desc.source_rate = data.source_rate;

  Vector<Payload> payloads;
  payloads.reserve(3);
  add_section(payloads, ClipSection::Desc, static_cast<u32>(sizeof(ClipFileDesc)), 1u, &desc);
  add_section(payloads, ClipSection::Samples, static_cast<u32>(sizeof(f32)), data.samples.size(),
              data.samples.data());
  const u64 identity[2] = {data.source_hash, data.build_key};
  add_section(payloads, ClipSection::SourceHash, static_cast<u32>(sizeof(u64)), 2u, identity);

  u64 offset = k_header_bytes + k_record_bytes * payloads.size();
  for (Payload& payload : payloads) {
    offset = align_up(offset);
    payload.record.offset = offset;
    offset += u64{payload.record.element_size} * payload.record.element_count;
  }
  const u64 total = offset;

  out.assign(static_cast<usize>(total), '\0');
  char* base = out.data();
  ClipFileHeader header;
  header.section_count = payloads.size();
  header.total_bytes = total;
  std::memcpy(base, &header, sizeof(header));
  u64 record_at = k_header_bytes;
  for (const Payload& payload : payloads) {
    std::memcpy(base + record_at, &payload.record, sizeof(payload.record));
    record_at += k_record_bytes;
    const u64 span_bytes = u64{payload.record.element_size} * payload.record.element_count;
    if (span_bytes != 0)
      std::memcpy(base + payload.record.offset, payload.data, static_cast<usize>(span_bytes));
  }
  const u64 content_hash =
      hash_bytes(base + k_header_bytes, static_cast<usize>(total - k_header_bytes));
  std::memcpy(base + offsetof(ClipFileHeader, content_hash), &content_hash, sizeof(content_hash));
  return content_hash;
}

// The header and the section table against a file of `file_bytes` bytes: the magic, the version,
// and that the table and every section fit.
bool check_table(const ClipFileHeader& header, std::span<const ClipFileSection> sections,
                 u64 file_bytes, std::string* error) {
  if (std::memcmp(header.magic, "CLIP", 4) != 0)
    return fail(error, "not a clip file: the magic is not \"CLIP\"");
  if (header.version != k_clip_file_version) {
    return fail(error, "unsupported clip file version " + std::to_string(header.version) +
                           ": this build reads version " + std::to_string(k_clip_file_version));
  }
  if (header.total_bytes < k_header_bytes || file_bytes < header.total_bytes) {
    return fail(error, "clip file is truncated: " + std::to_string(file_bytes) +
                           " bytes, the header says " + std::to_string(header.total_bytes));
  }
  for (const ClipFileSection& section : sections) {
    if (section.element_count > 0xffffffffull) {
      return fail(error, "clip file section " + section_label(section.kind) + " holds " +
                             std::to_string(section.element_count) +
                             " elements, more than this build reads");
    }
    const u64 span_bytes = u64{section.element_size} * section.element_count;
    if (section.offset < k_header_bytes || section.offset > header.total_bytes ||
        span_bytes > header.total_bytes - section.offset) {
      return fail(error,
                  "clip file section " + section_label(section.kind) + " extends past the end");
    }
  }
  return true;
}

bool read_header(std::span<const u8> bytes, ClipFileHeader& header, std::string* error) {
  if (bytes.size() < k_header_bytes) {
    return fail(error, "clip file is truncated: " + std::to_string(bytes.size()) +
                           " bytes, the 32-byte header does not fit");
  }
  std::memcpy(&header, bytes.data(), sizeof(header));
  return true;
}

// The header, the section table, that no section runs past the end, and the content hash.
bool check_container(std::span<const u8> bytes, ClipFileHeader& header,
                     Vector<ClipFileSection>& sections, std::string* error) {
  if (!read_header(bytes, header, error)) return false;
  if (std::memcmp(header.magic, "CLIP", 4) != 0)
    return fail(error, "not a clip file: the magic is not \"CLIP\"");
  const u64 table_end = k_header_bytes + k_record_bytes * u64{header.section_count};
  if (table_end > bytes.size() ||
      (header.total_bytes >= k_header_bytes && table_end > header.total_bytes)) {
    return fail(error, "clip file is truncated: the table of " +
                           std::to_string(header.section_count) + " sections does not fit");
  }
  sections.resize(header.section_count);
  for (u32 i = 0; i < header.section_count; ++i)
    std::memcpy(&sections[i], bytes.data() + k_header_bytes + k_record_bytes * i, k_record_bytes);
  if (!check_table(header, std::span<const ClipFileSection>(sections.data(), sections.size()),
                   bytes.size(), error)) {
    return false;
  }
  const u64 content_hash = hash_bytes(bytes.data() + k_header_bytes,
                                      static_cast<usize>(header.total_bytes - k_header_bytes));
  if (content_hash != header.content_hash) {
    return fail(error, "clip file content hash mismatch: the header says " +
                           std::to_string(header.content_hash) + ", the contents give " +
                           std::to_string(content_hash));
  }
  return true;
}

const ClipFileSection* find_section(std::span<const ClipFileSection> sections, ClipSection kind) {
  for (const ClipFileSection& section : sections) {
    if (section.kind == static_cast<u32>(kind)) return &section;
  }
  return nullptr;
}

// The description, checked: the mix rate, one or two channels, a frame count a clip can hold.
bool check_desc(const ClipFileDesc& desc, std::string* error) {
  if (desc.sample_rate != k_sample_rate) {
    return fail(error, "clip file is at " + std::to_string(desc.sample_rate) +
                           " Hz; the mix format is " + std::to_string(k_sample_rate));
  }
  if (desc.channels != 1 && desc.channels != 2)
    return fail(error, "clip file has " + std::to_string(desc.channels) + " channels");
  if (desc.frames == 0 || desc.frames > 0xffff'0000ull)
    return fail(error, "clip file holds " + std::to_string(desc.frames) + " frames");
  return true;
}

// The sections a reader needs, found and checked against each other.
bool check_sections(std::span<const ClipFileSection> sections, const ClipFileSection*& desc,
                    const ClipFileSection*& samples, std::string* error) {
  desc = find_section(sections, ClipSection::Desc);
  if (desc == nullptr) return fail(error, "clip file has no " + section_label(1) + " section");
  if (desc->element_size != sizeof(ClipFileDesc) || desc->element_count != 1)
    return fail(error, "clip file desc section is not one 32-byte record");
  samples = find_section(sections, ClipSection::Samples);
  if (samples == nullptr) return fail(error, "clip file has no " + section_label(2) + " section");
  if (samples->element_size != sizeof(f32))
    return fail(error, "clip file samples section does not hold 4-byte samples");
  return true;
}

void read_identity(std::span<const u8> bytes, std::span<const ClipFileSection> sections,
                   u64& source_hash, u64& build_key) {
  source_hash = 0;
  build_key = 0;
  const ClipFileSection* section = find_section(sections, ClipSection::SourceHash);
  if (section == nullptr || section->element_size != sizeof(u64)) return;
  if (section->element_count >= 1)
    std::memcpy(&source_hash, bytes.data() + section->offset, sizeof(u64));
  if (section->element_count >= 2)
    std::memcpy(&build_key, bytes.data() + section->offset + sizeof(u64), sizeof(u64));
}

}  // namespace

const char* clip_section_name(u32 kind) noexcept {
  switch (static_cast<ClipSection>(kind)) {
    case ClipSection::Desc: return "desc";
    case ClipSection::Samples: return "samples";
    case ClipSection::SourceHash: return "source_hash";
  }
  return "unknown";
}

u64 clip_source_hash(std::span<const u8> encoded) noexcept {
  return hash_bytes(encoded.data(), encoded.size());
}

u64 clip_cache_key(u64 source_hash) noexcept {
  u64 key = hash_combine(source_hash, 0x636c6970'2e6d6978ull);  // "clip.mix": not a texture key
  key = hash_combine(key, k_clip_cache_version);
  key = hash_combine(key, k_sample_rate);
  return hash_combine(key, 2u);  // at most two channels: the fold is part of the format
}

std::string clip_cache_path(std::string_view ddc_root, u64 key) {
  constexpr char k_digits[] = "0123456789abcdef";
  char name[17];
  for (u32 i = 0; i < 16; ++i)
    name[i] = k_digits[(key >> ((15 - i) * 4)) & 0xfull];
  name[16] = '\0';
  return io::join_path(io::join_path(ddc_root, "clips"), std::string(name) + ".clip");
}

DecodeStatus build_clip(std::span<const u8> encoded, ClipFileData& out) {
  out = ClipFileData{};
  DecodedClip decoded;
  const DecodeStatus status = decode_clip(encoded, decoded);
  if (status != DecodeStatus::Ok) return status;
  out.channels = decoded.channels;
  out.frames = decoded.frames;
  out.source_channels = decoded.source_channels;
  out.source_rate = decoded.source_rate;
  out.samples = std::move(decoded.samples);
  out.source_hash = clip_source_hash(encoded);
  out.build_key = clip_cache_key(out.source_hash);
  return DecodeStatus::Ok;
}

bool validate_clip(const ClipFileData& data, std::string* error) {
  if (data.channels != 1 && data.channels != 2)
    return fail(error, "a clip has one or two channels, not " + std::to_string(data.channels));
  if (data.frames == 0) return fail(error, "a clip has at least one frame");
  if (u64{data.samples.size()} != u64{data.frames} * data.channels) {
    return fail(error, "a clip of " + std::to_string(data.frames) + " frames of " +
                           std::to_string(data.channels) + " channels holds " +
                           std::to_string(data.samples.size()) + " samples");
  }
  return true;
}

u64 clip_file_hash(const ClipFileData& data) {
  std::string bytes;
  return encode(data, bytes);
}

bool is_clip_file(std::span<const u8> bytes) noexcept {
  return bytes.size() >= 4 && std::memcmp(bytes.data(), "CLIP", 4) == 0;
}

bool write_clip_file(std::string_view path, const ClipFileData& data, std::string* error) {
  if (!validate_clip(data, error)) return false;
  std::string bytes;
  encode(data, bytes);
  const io::Status status = io::write_file_atomic(path, bytes);
  if (status != io::Status::Ok) {
    return fail(error,
                "cannot write clip file '" + std::string(path) + "': " + io::status_name(status));
  }
  return true;
}

bool read_clip_file(std::string_view path, ClipFileData& out, std::string* error) {
  out = ClipFileData{};
  std::string bytes;
  const io::Status status = io::read_file(path, bytes);
  if (status != io::Status::Ok) {
    return fail(error,
                "cannot read clip file '" + std::string(path) + "': " + io::status_name(status));
  }
  return read_clip_file_memory(
      std::span<const u8>(reinterpret_cast<const u8*>(bytes.data()), bytes.size()), out, error);
}

bool read_clip_file_table(std::span<const u8> bytes, ClipFileHeader& header,
                          Vector<ClipFileSection>& sections, std::string* error) {
  return check_container(bytes, header, sections, error);
}

bool read_clip_file_memory(std::span<const u8> bytes, ClipFileData& out, std::string* error) {
  out = ClipFileData{};
  ClipFileHeader header;
  Vector<ClipFileSection> sections;
  if (!check_container(bytes, header, sections, error)) return false;
  const std::span<const ClipFileSection> table(sections.data(), sections.size());
  // Unknown kinds are skipped: that is the forward-compatibility guarantee.
  const ClipFileSection* desc_section = nullptr;
  const ClipFileSection* samples = nullptr;
  if (!check_sections(table, desc_section, samples, error)) return false;
  ClipFileDesc desc;
  std::memcpy(&desc, bytes.data() + desc_section->offset, sizeof(desc));
  if (!check_desc(desc, error)) return false;

  ClipFileData result;
  result.channels = desc.channels;
  result.frames = static_cast<u32>(desc.frames);
  result.source_channels = desc.source_channels;
  result.source_rate = desc.source_rate;
  if (samples->element_count != desc.frames * desc.channels) {
    return fail(error, "clip file holds " + std::to_string(samples->element_count) +
                           " samples and its description says " +
                           std::to_string(desc.frames * desc.channels));
  }
  result.samples.resize_exact(static_cast<u32>(samples->element_count));
  std::memcpy(result.samples.data(), bytes.data() + samples->offset,
              static_cast<usize>(samples->element_count) * sizeof(f32));
  read_identity(bytes, table, result.source_hash, result.build_key);
  if (!validate_clip(result, error)) return false;
  out = std::move(result);
  return true;
}

bool read_clip_file_layout(const io::FileHandle& file, ClipFileLayout& out, std::string* error) {
  out = ClipFileLayout{};
  if (!file.valid()) return fail(error, "clip file is not open");
  // The front of the file: the header, then the table it declares, then the description and the
  // identity wherever the table puts them — three small ranged reads, none of them the frames.
  u8 head[sizeof(ClipFileHeader)];
  u64 got = 0;
  if (file.read_at(0, head, sizeof(head), got) != io::Status::Ok || got != sizeof(head)) {
    return fail(error, "clip file '" + file.path() + "' is truncated: the header does not fit");
  }
  ClipFileHeader header;
  std::memcpy(&header, head, sizeof(header));
  if (std::memcmp(header.magic, "CLIP", 4) != 0)
    return fail(error, "not a clip file: the magic is not \"CLIP\"");
  if (header.section_count == 0 || header.section_count > 1024)
    return fail(error, "clip file declares " + std::to_string(header.section_count) + " sections");
  Vector<ClipFileSection> sections;
  sections.resize(header.section_count);
  const u64 table_bytes = k_record_bytes * header.section_count;
  if (file.read_at(k_header_bytes, sections.data(), table_bytes, got) != io::Status::Ok ||
      got != table_bytes) {
    return fail(error, "clip file is truncated: the table does not fit");
  }
  const std::span<const ClipFileSection> table(sections.data(), sections.size());
  if (!check_table(header, table, file.size(), error)) return false;
  const ClipFileSection* desc_section = nullptr;
  const ClipFileSection* samples = nullptr;
  if (!check_sections(table, desc_section, samples, error)) return false;
  ClipFileDesc desc;
  if (file.read_at(desc_section->offset, &desc, sizeof(desc), got) != io::Status::Ok ||
      got != sizeof(desc)) {
    return fail(error, "clip file is truncated: the description does not fit");
  }
  if (!check_desc(desc, error)) return false;
  if (samples->element_count != desc.frames * desc.channels)
    return fail(error, "clip file's samples do not match its description");
  out.channels = desc.channels;
  out.frames = static_cast<u32>(desc.frames);
  out.source_channels = desc.source_channels;
  out.source_rate = desc.source_rate;
  out.samples_offset = samples->offset;
  if (const ClipFileSection* identity = find_section(table, ClipSection::SourceHash);
      identity != nullptr && identity->element_size == sizeof(u64) &&
      identity->element_count >= 2) {
    u64 words[2] = {0, 0};
    if (file.read_at(identity->offset, words, sizeof(words), got) == io::Status::Ok &&
        got == sizeof(words)) {
      out.source_hash = words[0];
      out.build_key = words[1];
    }
  }
  return true;
}

bool read_clip_file_identity(std::string_view path, u64& source_hash, u64& build_key,
                             std::string* error) {
  source_hash = 0;
  build_key = 0;
  std::string bytes;
  const io::Status status = io::read_file(path, bytes);
  if (status != io::Status::Ok) {
    return fail(error,
                "cannot read clip file '" + std::string(path) + "': " + io::status_name(status));
  }
  const std::span<const u8> view(reinterpret_cast<const u8*>(bytes.data()), bytes.size());
  ClipFileHeader header;
  Vector<ClipFileSection> sections;
  if (!check_container(view, header, sections, error)) return false;
  read_identity(view, std::span<const ClipFileSection>(sections.data(), sections.size()),
                source_hash, build_key);
  return true;
}

}  // namespace engine::audio
