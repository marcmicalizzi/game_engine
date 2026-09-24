// The `.tex` container (texture_file.h). The layout and the checks are the cluster container's
// (domain/geometry/src/cluster_file.cpp), so a reader of one reads the other.
#include <core/hash/hash.h>
#include <domain/texture/texture_file.h>
#include <foundation/io/vfs.h>

#include <cstddef>
#include <cstring>
#include <utility>

namespace engine::texture {

namespace {

constexpr u64 k_header_bytes = sizeof(TextureFileHeader);
constexpr u64 k_record_bytes = sizeof(TextureFileSection);

constexpr u64 align_up(u64 value) noexcept {
  const u64 a = k_texture_file_alignment;
  return (value + a - 1) / a * a;
}

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

std::string section_label(u32 kind) {
  return std::string(texture_section_name(kind)) + " (kind " + std::to_string(kind) + ")";
}

struct Payload {
  TextureFileSection record;
  const void* data = nullptr;
};

void add_section(Vector<Payload>& out, TextureSection kind, u32 element_size, u64 element_count,
                 const void* data) {
  Payload payload;
  payload.record.kind = static_cast<u32>(kind);
  payload.record.element_size = element_size;
  payload.record.element_count = element_count;
  payload.data = data;
  out.push_back(payload);
}

// Serializes `data` into `out` and returns the content hash the header carries.
u64 encode(const TextureData& data, std::string& out) {
  TextureFileDesc desc;
  desc.format = static_cast<u32>(data.format);
  desc.color_space = static_cast<u32>(data.color_space);
  desc.width = data.width;
  desc.height = data.height;
  desc.level_count = data.levels.size();
  desc.flags = data.flags;
  desc.source_channels = data.source_channels;

  Vector<Payload> payloads;
  payloads.reserve(4);
  add_section(payloads, TextureSection::Desc, static_cast<u32>(sizeof(TextureFileDesc)), 1u, &desc);
  add_section(payloads, TextureSection::Levels, static_cast<u32>(sizeof(TextureFileLevel)),
              data.levels.size(), data.levels.data());
  add_section(payloads, TextureSection::Data, 1u, data.data.size(), data.data.data());
  const u64 identity[2] = {data.source_hash, data.build_key};
  add_section(payloads, TextureSection::SourceHash, static_cast<u32>(sizeof(u64)), 2u, identity);

  u64 offset = k_header_bytes + k_record_bytes * payloads.size();
  for (Payload& payload : payloads) {
    offset = align_up(offset);
    payload.record.offset = offset;
    offset += u64{payload.record.element_size} * payload.record.element_count;
  }
  const u64 total = offset;

  out.assign(static_cast<usize>(total), '\0');
  char* base = out.data();
  TextureFileHeader header;
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
  std::memcpy(base + offsetof(TextureFileHeader, content_hash), &content_hash,
              sizeof(content_hash));
  return content_hash;
}

// The header, the section table, that no section runs past the end, and the content hash.
bool check_container(std::span<const u8> bytes, TextureFileHeader& header,
                     Vector<TextureFileSection>& sections, std::string* error) {
  if (bytes.size() < k_header_bytes) {
    return fail(error, "texture file is truncated: " + std::to_string(bytes.size()) +
                           " bytes, the 32-byte header does not fit");
  }
  std::memcpy(&header, bytes.data(), sizeof(header));
  if (std::memcmp(header.magic, "TEXF", 4) != 0)
    return fail(error, "not a texture file: the magic is not \"TEXF\"");
  if (header.version != k_texture_file_version) {
    return fail(error, "unsupported texture file version " + std::to_string(header.version) +
                           ": this build reads version " + std::to_string(k_texture_file_version));
  }
  if (header.total_bytes < k_header_bytes || bytes.size() < header.total_bytes) {
    return fail(error, "texture file is truncated: " + std::to_string(bytes.size()) +
                           " bytes, the header says " + std::to_string(header.total_bytes));
  }
  const u64 table_end = k_header_bytes + k_record_bytes * u64{header.section_count};
  if (table_end > header.total_bytes) {
    return fail(error, "texture file is truncated: the table of " +
                           std::to_string(header.section_count) + " sections does not fit");
  }
  sections.resize(header.section_count);
  for (u32 i = 0; i < header.section_count; ++i)
    std::memcpy(&sections[i], bytes.data() + k_header_bytes + k_record_bytes * i, k_record_bytes);
  for (const TextureFileSection& section : sections) {
    if (section.element_count > 0xffffffffull) {
      return fail(error, "texture file section " + section_label(section.kind) + " holds " +
                             std::to_string(section.element_count) +
                             " elements, more than this build reads");
    }
    const u64 span_bytes = u64{section.element_size} * section.element_count;
    if (section.offset < k_header_bytes || section.offset > header.total_bytes ||
        span_bytes > header.total_bytes - section.offset) {
      return fail(error, "texture file section " + section_label(section.kind) +
                             " extends past the end of the file");
    }
  }
  const u64 content_hash = hash_bytes(bytes.data() + k_header_bytes,
                                      static_cast<usize>(header.total_bytes - k_header_bytes));
  if (content_hash != header.content_hash) {
    return fail(error, "texture file content hash mismatch: the header says " +
                           std::to_string(header.content_hash) + ", the contents give " +
                           std::to_string(content_hash));
  }
  return true;
}

bool read_identity(std::span<const u8> bytes, std::span<const TextureFileSection> sections,
                   u64& source_hash, u64& build_key, std::string* error) {
  source_hash = 0;
  build_key = 0;
  for (const TextureFileSection& section : sections) {
    if (section.kind != static_cast<u32>(TextureSection::SourceHash)) continue;
    if (section.element_count == 0) return true;
    if (section.element_size != sizeof(u64)) {
      return fail(error, "texture file section source_hash has " +
                             std::to_string(section.element_size) + "-byte elements, expected 8");
    }
    std::memcpy(&source_hash, bytes.data() + section.offset, sizeof(u64));
    if (section.element_count >= 2)
      std::memcpy(&build_key, bytes.data() + section.offset + sizeof(u64), sizeof(u64));
    return true;
  }
  return true;
}

}  // namespace

const char* texture_format_name(TextureFormat format) noexcept {
  switch (format) {
    case TextureFormat::rgba8: return "rgba8";
    case TextureFormat::bc1: return "bc1";
    case TextureFormat::bc3: return "bc3";
    case TextureFormat::bc4: return "bc4";
    case TextureFormat::bc5: return "bc5";
    case TextureFormat::bc7: return "bc7";
  }
  return "unknown";
}

const char* color_space_name(ColorSpace space) noexcept {
  return space == ColorSpace::srgb ? "srgb" : "linear";
}

bool parse_texture_format(std::string_view name, TextureFormat& out) noexcept {
  for (u32 value = 1; value <= static_cast<u32>(TextureFormat::bc7); ++value) {
    const TextureFormat format = static_cast<TextureFormat>(value);
    if (name == texture_format_name(format)) {
      out = format;
      return true;
    }
  }
  return false;
}

bool texture_format_known(u32 value) noexcept {
  return value >= static_cast<u32>(TextureFormat::rgba8) &&
         value <= static_cast<u32>(TextureFormat::bc7);
}

u32 texture_block_extent(TextureFormat format) noexcept {
  return format == TextureFormat::rgba8 ? 1u : 4u;
}

u32 texture_block_bytes(TextureFormat format) noexcept {
  switch (format) {
    case TextureFormat::rgba8: return 4;
    case TextureFormat::bc1:
    case TextureFormat::bc4: return 8;
    case TextureFormat::bc3:
    case TextureFormat::bc5:
    case TextureFormat::bc7: return 16;
  }
  return 0;
}

u64 texture_level_bytes(TextureFormat format, u32 width, u32 height) noexcept {
  const u32 e = texture_block_extent(format);
  const u64 blocks_x = (u64{width} + e - 1) / e;
  const u64 blocks_y = (u64{height} + e - 1) / e;
  return blocks_x * blocks_y * texture_block_bytes(format);
}

u32 texture_full_level_count(u32 width, u32 height) noexcept {
  u32 side = width > height ? width : height;
  u32 count = 1;
  while (side > 1) {
    side >>= 1;
    ++count;
  }
  return count;
}

const char* texture_section_name(u32 kind) noexcept {
  switch (static_cast<TextureSection>(kind)) {
    case TextureSection::Desc: return "desc";
    case TextureSection::Levels: return "levels";
    case TextureSection::Data: return "data";
    case TextureSection::SourceHash: return "source_hash";
  }
  return "unknown";
}

std::span<const u8> TextureData::level_bytes(u32 index) const noexcept {
  if (index >= levels.size()) return {};
  const TextureFileLevel& level = levels[index];
  if (level.offset > data.size() || level.bytes > data.size() - level.offset) return {};
  return std::span<const u8>(data.data() + level.offset, static_cast<usize>(level.bytes));
}

bool validate_texture(const TextureData& data, std::string* error) {
  if (!texture_format_known(static_cast<u32>(data.format)))
    return fail(error, "texture format " + std::to_string(static_cast<u32>(data.format)) +
                           " is not one this build knows");
  if (data.color_space != ColorSpace::linear && data.color_space != ColorSpace::srgb)
    return fail(error, "texture color space " + std::to_string(static_cast<u32>(data.color_space)) +
                           " is unknown");
  if ((data.format == TextureFormat::bc4 || data.format == TextureFormat::bc5) &&
      data.color_space == ColorSpace::srgb) {
    return fail(error, std::string("texture format ") + texture_format_name(data.format) +
                           " has no sRGB form");
  }
  if (data.width == 0 || data.height == 0) return fail(error, "texture has no texels");
  if (data.levels.empty()) return fail(error, "texture has no levels");
  if (data.levels.size() > texture_full_level_count(data.width, data.height)) {
    return fail(error, "texture has " + std::to_string(data.levels.size()) +
                           " levels, more than a " + std::to_string(data.width) + "x" +
                           std::to_string(data.height) + " chain holds");
  }
  for (u32 i = 0; i < data.levels.size(); ++i) {
    const TextureFileLevel& level = data.levels[i];
    const u32 w = texture_level_extent(data.width, i);
    const u32 h = texture_level_extent(data.height, i);
    const std::string at = "texture level " + std::to_string(i);
    if (level.width != w || level.height != h) {
      return fail(error, at + " is " + std::to_string(level.width) + "x" +
                             std::to_string(level.height) + ", a mip chain makes it " +
                             std::to_string(w) + "x" + std::to_string(h));
    }
    if (level.bytes != texture_level_bytes(data.format, w, h)) {
      return fail(error, at + " holds " + std::to_string(level.bytes) + " bytes, " +
                             texture_format_name(data.format) + " at that size is " +
                             std::to_string(texture_level_bytes(data.format, w, h)));
    }
    if (level.offset % k_texture_file_alignment != 0)
      return fail(error, at + " is not at a 16-byte aligned offset");
    if (level.offset > data.data.size() || level.bytes > data.data.size() - level.offset) {
      return fail(error,
                  at + " runs past the " + std::to_string(data.data.size()) + "-byte data payload");
    }
  }
  return true;
}

u64 texture_file_hash(const TextureData& data) {
  std::string bytes;
  return encode(data, bytes);
}

bool is_texture_file(std::span<const u8> bytes) noexcept {
  return bytes.size() >= 4 && std::memcmp(bytes.data(), "TEXF", 4) == 0;
}

bool write_texture_file(std::string_view path, const TextureData& data, std::string* error) {
  if (!validate_texture(data, error)) return false;
  std::string bytes;
  encode(data, bytes);
  const io::Status status = io::write_file_atomic(path, bytes);
  if (status != io::Status::Ok) {
    return fail(
        error, "cannot write texture file '" + std::string(path) + "': " + io::status_name(status));
  }
  return true;
}

bool read_texture_file(std::string_view path, TextureData& out, std::string* error) {
  out = TextureData{};
  std::string bytes;
  const io::Status status = io::read_file(path, bytes);
  if (status != io::Status::Ok) {
    return fail(error,
                "cannot read texture file '" + std::string(path) + "': " + io::status_name(status));
  }
  return read_texture_file_memory(
      std::span<const u8>(reinterpret_cast<const u8*>(bytes.data()), bytes.size()), out, error);
}

bool read_texture_file_table(std::span<const u8> bytes, TextureFileHeader& header,
                             Vector<TextureFileSection>& sections, std::string* error) {
  return check_container(bytes, header, sections, error);
}

bool read_texture_file_identity(std::string_view path, u64& source_hash, u64& build_key,
                                std::string* error) {
  source_hash = 0;
  build_key = 0;
  std::string bytes;
  const io::Status status = io::read_file(path, bytes);
  if (status != io::Status::Ok) {
    return fail(error,
                "cannot read texture file '" + std::string(path) + "': " + io::status_name(status));
  }
  const std::span<const u8> view(reinterpret_cast<const u8*>(bytes.data()), bytes.size());
  TextureFileHeader header;
  Vector<TextureFileSection> sections;
  if (!check_container(view, header, sections, error)) return false;
  return read_identity(view, std::span<const TextureFileSection>(sections.data(), sections.size()),
                       source_hash, build_key, error);
}

bool read_texture_file_memory(std::span<const u8> bytes, TextureData& out, std::string* error) {
  out = TextureData{};
  TextureFileHeader header;
  Vector<TextureFileSection> sections;
  if (!check_container(bytes, header, sections, error)) return false;

  // Unknown kinds are skipped: that is the forward-compatibility guarantee.
  const TextureFileSection* found[k_texture_section_kinds] = {};
  for (const TextureFileSection& section : sections) {
    if (section.kind == 0 || section.kind >= k_texture_section_kinds) continue;
    if (found[section.kind] == nullptr) found[section.kind] = &section;
  }
  auto require = [&](TextureSection kind, usize element_size,
                     const TextureFileSection*& section) -> bool {
    const u32 index = static_cast<u32>(kind);
    section = found[index];
    if (section == nullptr)
      return fail(error, "texture file has no " + section_label(index) + " section");
    if (section->element_size != element_size) {
      return fail(error, "texture file section " + section_label(index) + " has " +
                             std::to_string(section->element_size) + "-byte elements, expected " +
                             std::to_string(element_size));
    }
    return true;
  };

  TextureData result;
  const TextureFileSection* desc_section = nullptr;
  if (!require(TextureSection::Desc, sizeof(TextureFileDesc), desc_section)) return false;
  if (desc_section->element_count != 1) {
    return fail(error, "texture file desc section holds " +
                           std::to_string(desc_section->element_count) + " records, expected one");
  }
  TextureFileDesc desc;
  std::memcpy(&desc, bytes.data() + desc_section->offset, sizeof(desc));
  if (!texture_format_known(desc.format)) {
    return fail(error, "texture file format " + std::to_string(desc.format) +
                           " is not one this build knows");
  }
  result.format = static_cast<TextureFormat>(desc.format);
  result.color_space = static_cast<ColorSpace>(desc.color_space);
  result.width = desc.width;
  result.height = desc.height;
  result.flags = desc.flags;
  result.source_channels = desc.source_channels;

  const TextureFileSection* levels = nullptr;
  if (!require(TextureSection::Levels, sizeof(TextureFileLevel), levels)) return false;
  if (levels->element_count != desc.level_count) {
    return fail(error, "texture file lists " + std::to_string(levels->element_count) +
                           " levels and its description says " + std::to_string(desc.level_count));
  }
  result.levels.resize(static_cast<u32>(levels->element_count));
  if (levels->element_count != 0) {
    std::memcpy(result.levels.data(), bytes.data() + levels->offset,
                static_cast<usize>(levels->element_count) * sizeof(TextureFileLevel));
  }
  const TextureFileSection* payload = nullptr;
  if (!require(TextureSection::Data, 1, payload)) return false;
  result.data.resize(static_cast<u32>(payload->element_count));
  if (payload->element_count != 0) {
    std::memcpy(result.data.data(), bytes.data() + payload->offset,
                static_cast<usize>(payload->element_count));
  }
  if (!read_identity(bytes, std::span<const TextureFileSection>(sections.data(), sections.size()),
                     result.source_hash, result.build_key, error)) {
    return false;
  }
  if (!validate_texture(result, error)) return false;
  out = std::move(result);
  return true;
}

}  // namespace engine::texture
