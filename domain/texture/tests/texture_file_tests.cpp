// The `.tex` container: a built texture written, read back and compared field by field; the same
// bytes read from memory; the identity alone; the forward-compatibility rule (a section of an
// unknown kind is skipped); and every way a file can be broken, each with a sentence of its own
// and an empty result. The layout is the cluster container's, so these are that file's tests in
// the shape of a texture.
#include "fixtures.h"

#include <core/hash/hash.h>
#include <domain/texture/texture_build.h>
#include <domain/texture/texture_file.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstring>
#include <string>

using namespace engine;
using namespace engine::texture;

namespace {

std::span<const u8> view(const std::string& bytes) {
  return std::span<const u8>(reinterpret_cast<const u8*>(bytes.data()), bytes.size());
}

void patch(std::string& file, usize at, const void* data, usize bytes) {
  std::memcpy(file.data() + at, data, bytes);
}

void rehash(std::string& file) {
  const u64 content = hash_bytes(file.data() + 32, file.size() - 32);
  patch(file, 24, &content, sizeof(content));
}

// A 37 x 21 BC7 texture with its full chain: odd sides, so the last blocks of every level are
// partial and the chain's sides round down, and a recorded identity.
TextureData make_fixture() {
  Vector<u8> pixels;
  test_fixtures::colour_image(37, 21, true, pixels);
  TextureBuildOptions options;
  options.format = FormatChoice::bc7;
  TextureData data;
  std::string error;
  const bool built = build_texture(std::span<const u8>(pixels.data(), pixels.size()), 37, 21, 4,
                                   options, data, nullptr, &error);
  REQUIRE_MESSAGE(built, error);
  data.source_hash = 0x1122334455667788ull;
  data.build_key = texture_cache_key(data.source_hash, options);
  return data;
}

void check_equal(const TextureData& a, const TextureData& b) {
  CHECK(a.format == b.format);
  CHECK(a.color_space == b.color_space);
  CHECK(a.width == b.width);
  CHECK(a.height == b.height);
  CHECK(a.flags == b.flags);
  CHECK(a.source_channels == b.source_channels);
  CHECK(a.source_hash == b.source_hash);
  CHECK(a.build_key == b.build_key);
  REQUIRE(a.levels.size() == b.levels.size());
  for (u32 i = 0; i < a.levels.size(); ++i) {
    CHECK(a.levels[i].width == b.levels[i].width);
    CHECK(a.levels[i].height == b.levels[i].height);
    CHECK(a.levels[i].offset == b.levels[i].offset);
    CHECK(a.levels[i].bytes == b.levels[i].bytes);
  }
  REQUIRE(a.data.size() == b.data.size());
  CHECK(std::memcmp(a.data.data(), b.data.data(), a.data.size()) == 0);
}

// A copy of `file` with a section of an unknown kind inserted, the way the cluster container's
// test does it: one more record in the table, every payload shifted by a multiple of 16, and the
// unknown payload appended at the end.
std::string with_unknown_section(const std::string& file) {
  TextureFileHeader header;
  std::memcpy(&header, file.data(), sizeof(header));
  const u64 table_end =
      sizeof(TextureFileHeader) + sizeof(TextureFileSection) * header.section_count;
  Vector<TextureFileSection> sections(header.section_count);
  u64 first_payload = header.total_bytes;
  for (u32 i = 0; i < header.section_count; ++i) {
    std::memcpy(&sections[i],
                file.data() + sizeof(TextureFileHeader) + sizeof(TextureFileSection) * i,
                sizeof(TextureFileSection));
    if (sections[i].offset < first_payload) first_payload = sections[i].offset;
  }
  const u64 shift = (table_end + sizeof(TextureFileSection) + 15) / 16 * 16 - first_payload;
  std::string out;
  out.append(file, 0, sizeof(TextureFileHeader));
  for (TextureFileSection section : sections) {
    section.offset += shift;
    out.append(reinterpret_cast<const char*>(&section), sizeof(section));
  }
  TextureFileSection unknown;
  unknown.kind = 4242;
  unknown.element_size = 4;
  unknown.element_count = 4;
  const usize unknown_record_at = out.size();
  out.append(reinterpret_cast<const char*>(&unknown), sizeof(unknown));
  out.append(static_cast<usize>(first_payload + shift) - out.size(), '\0');
  out.append(file, static_cast<usize>(first_payload),
             static_cast<usize>(header.total_bytes - first_payload));
  while (out.size() % 16 != 0)
    out.push_back('\0');
  unknown.offset = out.size();
  out.append(16, static_cast<char>(0xab));
  patch(out, unknown_record_at, &unknown, sizeof(unknown));
  header.section_count += 1;
  header.total_bytes = out.size();
  std::memcpy(out.data(), &header, sizeof(header));
  rehash(out);
  return out;
}

}  // namespace

TEST_CASE("texture file: a built chain survives a round trip field by field") {
  const test::TempDir tmp("engine_texture_file");
  const TextureData data = make_fixture();
  CHECK(data.levels.size() == 6);  // 37x21, 18x10, 9x5, 4x2, 2x1, 1x1
  CHECK(data.levels[5].width == 1);
  CHECK(data.levels[5].height == 1);
  CHECK((data.flags & k_texture_has_alpha) != 0);
  const std::string path = tmp.file("fixture.tex");
  std::string error;
  REQUIRE_MESSAGE(write_texture_file(path, data, &error), error);

  TextureData read;
  REQUIRE_MESSAGE(read_texture_file(path, read, &error), error);
  check_equal(read, data);

  std::string bytes;
  REQUIRE(io::read_file(path, bytes) == io::Status::Ok);
  TextureData from_memory;
  REQUIRE_MESSAGE(read_texture_file_memory(view(bytes), from_memory, &error), error);
  check_equal(from_memory, data);
  CHECK(is_texture_file(view(bytes)));

  // The header's hash is the one computed in memory, every payload is aligned and inside the
  // file, and every level's range is aligned inside the data payload.
  TextureFileHeader header;
  Vector<TextureFileSection> sections;
  REQUIRE_MESSAGE(read_texture_file_table(view(bytes), header, sections, &error), error);
  CHECK(header.content_hash == texture_file_hash(data));
  CHECK(header.total_bytes == bytes.size());
  CHECK(sections.size() == 4);
  for (const TextureFileSection& section : sections) {
    CHECK(section.offset % k_texture_file_alignment == 0);
    CHECK(section.offset + u64{section.element_size} * section.element_count <= bytes.size());
  }
  for (const TextureFileLevel& level : data.levels)
    CHECK(level.offset % k_texture_file_alignment == 0);

  u64 source_hash = 0;
  u64 build_key = 0;
  REQUIRE_MESSAGE(read_texture_file_identity(path, source_hash, build_key, &error), error);
  CHECK(source_hash == data.source_hash);
  CHECK(build_key == data.build_key);
}

TEST_CASE("texture file: a section of an unknown kind is skipped") {
  const test::TempDir tmp("engine_texture_file");
  const TextureData data = make_fixture();
  const std::string path = tmp.file("forward.tex");
  std::string error;
  REQUIRE_MESSAGE(write_texture_file(path, data, &error), error);
  std::string file;
  REQUIRE(io::read_file(path, file) == io::Status::Ok);
  const std::string newer = with_unknown_section(file);
  CHECK(newer.size() > file.size());
  TextureData read;
  REQUIRE_MESSAGE(read_texture_file_memory(view(newer), read, &error), error);
  check_equal(read, data);
}

TEST_CASE("texture file: a texture with no identity reads it as zero, not as a failure") {
  const test::TempDir tmp("engine_texture_file");
  TextureData data = make_fixture();
  data.source_hash = 0;
  data.build_key = 0;
  const std::string path = tmp.file("anonymous.tex");
  std::string error;
  REQUIRE_MESSAGE(write_texture_file(path, data, &error), error);
  u64 source_hash = 1;
  u64 build_key = 1;
  REQUIRE_MESSAGE(read_texture_file_identity(path, source_hash, build_key, &error), error);
  CHECK(source_hash == 0);
  CHECK(build_key == 0);
}

TEST_CASE("texture file: the writer refuses a chain that does not validate") {
  const test::TempDir tmp("engine_texture_file");
  const std::string path = tmp.file("invalid.tex");
  std::string error;

  TextureData wrong_extent = make_fixture();
  wrong_extent.levels[1].width += 1;
  CHECK_FALSE(write_texture_file(path, wrong_extent, &error));
  CHECK_MESSAGE(error.find("mip chain makes it") != std::string::npos, error);

  TextureData wrong_bytes = make_fixture();
  wrong_bytes.levels[2].bytes -= 16;
  CHECK_FALSE(write_texture_file(path, wrong_bytes, &error));
  CHECK_MESSAGE(error.find("holds") != std::string::npos, error);

  TextureData srgb_bc5 = make_fixture();
  srgb_bc5.format = TextureFormat::bc5;
  srgb_bc5.color_space = ColorSpace::srgb;
  CHECK_FALSE(write_texture_file(path, srgb_bc5, &error));
  CHECK_MESSAGE(error.find("no sRGB form") != std::string::npos, error);

  TextureData too_long = make_fixture();
  too_long.levels.push_back(too_long.levels.back());
  CHECK_FALSE(write_texture_file(path, too_long, &error));
  CHECK_MESSAGE(error.find("more than") != std::string::npos, error);
  CHECK_FALSE(io::exists(path));
}

TEST_CASE("texture file: broken files fail with distinct messages and an empty result") {
  const test::TempDir tmp("engine_texture_file");
  const TextureData data = make_fixture();
  const std::string path = tmp.file("broken.tex");
  std::string error;
  REQUIRE_MESSAGE(write_texture_file(path, data, &error), error);
  std::string good;
  REQUIRE(io::read_file(path, good) == io::Status::Ok);

  Vector<std::string> messages;
  auto refuses = [&](const std::string& file, const char* what) {
    TextureData read;
    std::string message;
    CHECK_FALSE(read_texture_file_memory(view(file), read, &message));
    CHECK_MESSAGE(message.find(what) != std::string::npos, message);
    CHECK(read.levels.empty());
    CHECK(read.data.empty());
    CHECK(read.width == 0);
    messages.push_back(message);
  };

  refuses(good.substr(0, good.size() / 2), "truncated");
  refuses(std::string(), "truncated");

  std::string bad_magic = good;
  bad_magic[0] = 'X';
  refuses(bad_magic, "magic");

  std::string bad_version = good;
  const u32 version = k_texture_file_version + 1;
  patch(bad_version, 4, &version, sizeof(version));
  refuses(bad_version, "version");

  std::string past_end = good;
  TextureFileHeader header;
  std::memcpy(&header, good.data(), sizeof(header));
  TextureFileSection section;
  std::memcpy(&section, good.data() + sizeof(header), sizeof(section));
  section.offset = header.total_bytes - 16;
  section.element_count = 64;
  section.element_size = 1;
  patch(past_end, sizeof(header), &section, sizeof(section));
  refuses(past_end, "past the end");

  std::string corrupt = good;
  corrupt[corrupt.size() - 1] = static_cast<char>(corrupt[corrupt.size() - 1] ^ 0x5a);
  refuses(corrupt, "hash mismatch");

  // A required section renamed to an unknown kind is missing, not silently empty.
  std::string no_levels = good;
  usize levels_at = 0;
  for (u32 i = 0; i < header.section_count; ++i) {
    const usize at = sizeof(header) + sizeof(TextureFileSection) * i;
    TextureFileSection s;
    std::memcpy(&s, good.data() + at, sizeof(s));
    if (s.kind == static_cast<u32>(TextureSection::Levels)) levels_at = at;
  }
  REQUIRE(levels_at != 0);
  TextureFileSection levels;
  std::memcpy(&levels, good.data() + levels_at, sizeof(levels));
  levels.kind = 7777;
  patch(no_levels, levels_at, &levels, sizeof(levels));
  rehash(no_levels);
  refuses(no_levels, "has no levels");

  // A format from a build that does not exist yet is refused by name, not decoded as garbage.
  std::string future_format = good;
  usize desc_at = 0;
  for (u32 i = 0; i < header.section_count; ++i) {
    const usize at = sizeof(header) + sizeof(TextureFileSection) * i;
    TextureFileSection s;
    std::memcpy(&s, good.data() + at, sizeof(s));
    if (s.kind == static_cast<u32>(TextureSection::Desc)) desc_at = static_cast<usize>(s.offset);
  }
  REQUIRE(desc_at != 0);
  const u32 format = 99;
  patch(future_format, desc_at, &format, sizeof(format));
  rehash(future_format);
  refuses(future_format, "is not one this build knows");

  for (u32 i = 0; i < messages.size(); ++i) {
    for (u32 j = i + 1; j < messages.size(); ++j) {
      if (i == 0 && j == 1) continue;  // both truncations, the same sentence with other numbers
      CHECK_MESSAGE(messages[i] != messages[j], messages[i]);
    }
  }
}

TEST_CASE("texture file: sizes, names and the chain arithmetic") {
  CHECK(texture_level_bytes(TextureFormat::bc7, 4, 4) == 16);
  CHECK(texture_level_bytes(TextureFormat::bc7, 5, 4) == 32);
  CHECK(texture_level_bytes(TextureFormat::bc1, 1, 1) == 8);
  CHECK(texture_level_bytes(TextureFormat::bc4, 2048, 2048) == 2048u * 2048u / 2u);
  CHECK(texture_level_bytes(TextureFormat::bc5, 2048, 2048) == 2048u * 2048u);
  CHECK(texture_level_bytes(TextureFormat::rgba8, 3, 5) == 60);
  CHECK(texture_full_level_count(1, 1) == 1);
  CHECK(texture_full_level_count(2048, 2048) == 12);
  CHECK(texture_full_level_count(300, 200) == 9);
  CHECK(texture_full_level_count(1, 7) == 3);
  CHECK(texture_level_extent(300, 3) == 37);
  CHECK(texture_level_extent(200, 8) == 1);
  CHECK(texture_level_extent(5, 40) == 1);
  for (u32 v = 1; v <= 6; ++v) {
    const TextureFormat f = static_cast<TextureFormat>(v);
    TextureFormat parsed = TextureFormat::rgba8;
    CHECK(parse_texture_format(texture_format_name(f), parsed));
    CHECK(parsed == f);
  }
  TextureFormat unchanged = TextureFormat::bc7;
  CHECK_FALSE(parse_texture_format("bc6h", unchanged));
  CHECK(unchanged == TextureFormat::bc7);
  CHECK(std::string(texture_section_name(3)) == "data");
  CHECK(std::string(texture_section_name(99)) == "unknown");
}
