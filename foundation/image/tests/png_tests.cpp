#include <foundation/image/png.h>

#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <string>

using namespace engine;
using namespace engine::image;

namespace {

u32 read_be(const Vector<u8>& bytes, u32 at) {
  return (u32{bytes[at]} << 24) | (u32{bytes[at + 1]} << 16) | (u32{bytes[at + 2]} << 8) |
         bytes[at + 3];
}

// Walks the chunks, verifying every CRC, and returns the concatenated IDAT payload.
bool walk_chunks(const Vector<u8>& png, u32& width, u32& height, u8& color_type, Vector<u8>& idat) {
  static constexpr u8 k_signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (png.size() < 8 || std::memcmp(png.data(), k_signature, 8) != 0) return false;
  u32 at = 8;
  bool saw_end = false;
  while (at + 12 <= png.size() && !saw_end) {
    const u32 length = read_be(png, at);
    const char* type = reinterpret_cast<const char*>(png.data() + at + 4);
    if (at + 12 + length > png.size()) return false;
    const u32 crc = read_be(png, at + 8 + length);
    if (crc != crc32(std::span<const u8>(png.data() + at + 4, 4 + length))) return false;
    if (std::memcmp(type, "IHDR", 4) == 0) {
      width = read_be(png, at + 8);
      height = read_be(png, at + 12);
      color_type = png[at + 8 + 9];
    } else if (std::memcmp(type, "IDAT", 4) == 0) {
      for (u32 i = 0; i < length; ++i)
        idat.push_back(png[at + 8 + i]);
    } else if (std::memcmp(type, "IEND", 4) == 0) {
      saw_end = true;
    }
    at += 12 + length;
  }
  return saw_end && at == png.size();
}

// Inflates a stream made only of stored blocks and checks its adler32.
bool inflate_stored(const Vector<u8>& zlib, Vector<u8>& raw) {
  if (zlib.size() < 6 || zlib[0] != 0x78 || ((u32{zlib[0]} << 8) | zlib[1]) % 31 != 0) return false;
  u32 at = 2;
  bool final = false;
  while (!final) {
    if (at + 5 > zlib.size()) return false;
    const u8 header = zlib[at];
    if ((header & 0x06) != 0) return false;  // BTYPE must be stored
    final = (header & 1) != 0;
    const u32 len = u32{zlib[at + 1]} | (u32{zlib[at + 2]} << 8);
    const u32 nlen = u32{zlib[at + 3]} | (u32{zlib[at + 4]} << 8);
    if ((len ^ nlen) != 0xFFFF) return false;
    at += 5;
    if (at + len > zlib.size()) return false;
    for (u32 i = 0; i < len; ++i)
      raw.push_back(zlib[at + i]);
    at += len;
  }
  if (at + 4 != zlib.size()) return false;
  return read_be(zlib, at) == adler32(std::span<const u8>(raw.data(), raw.size()));
}

}  // namespace

TEST_CASE("png: checksums match the reference values") {
  const char* text = "123456789";
  const auto bytes = std::span<const u8>(reinterpret_cast<const u8*>(text), 9);
  CHECK(crc32(bytes) == 0xCBF43926u);
  CHECK(adler32(bytes) == 0x091E01DEu);
  CHECK(crc32({}) == 0u);
  CHECK(adler32({}) == 1u);
  // Incremental CRC equals the one-shot CRC.
  CHECK(crc32(bytes.subspan(4), crc32(bytes.subspan(0, 4))) == 0xCBF43926u);
}

TEST_CASE("png: a small RGBA image round-trips through the container") {
  constexpr u32 k_w = 3;
  constexpr u32 k_h = 2;
  u8 pixels[k_w * k_h * 4];
  for (u32 i = 0; i < k_w * k_h; ++i) {
    pixels[i * 4 + 0] = static_cast<u8>(i * 40);
    pixels[i * 4 + 1] = static_cast<u8>(255 - i * 40);
    pixels[i * 4 + 2] = static_cast<u8>(i * 7);
    pixels[i * 4 + 3] = 255;
  }
  Vector<u8> png;
  REQUIRE(encode_png(k_w, k_h, 4, pixels, png));
  u32 width = 0;
  u32 height = 0;
  u8 color_type = 0;
  Vector<u8> idat;
  REQUIRE(walk_chunks(png, width, height, color_type, idat));
  CHECK(width == k_w);
  CHECK(height == k_h);
  CHECK(color_type == 6);
  Vector<u8> raw;
  REQUIRE(inflate_stored(idat, raw));
  REQUIRE(raw.size() == (k_w * 4 + 1) * k_h);
  for (u32 y = 0; y < k_h; ++y) {
    CHECK(raw[y * (k_w * 4 + 1)] == 0);  // filter byte
    CHECK(std::memcmp(raw.data() + y * (k_w * 4 + 1) + 1, pixels + y * k_w * 4, k_w * 4) == 0);
  }
  // Overhead is the fixed container plus one block header.
  CHECK(png.size() == 8 + 25 + 12 + (2 + 5 + raw.size() + 4) + 12);
}

TEST_CASE("png: large images split into several stored blocks and other channel counts work") {
  constexpr u32 k_w = 300;
  constexpr u32 k_h = 300;
  Vector<u8> gray(k_w * k_h);
  for (u32 i = 0; i < gray.size(); ++i)
    gray[i] = static_cast<u8>((i * 31) & 0xFF);
  Vector<u8> png;
  REQUIRE(encode_png(k_w, k_h, 1, std::span<const u8>(gray.data(), gray.size()), png));
  u32 width = 0;
  u32 height = 0;
  u8 color_type = 0;
  Vector<u8> idat;
  REQUIRE(walk_chunks(png, width, height, color_type, idat));
  CHECK(color_type == 0);
  Vector<u8> raw;
  REQUIRE(inflate_stored(idat, raw));
  CHECK(raw.size() == (k_w + 1) * k_h);  // 90300 bytes: two stored blocks
  for (u32 y = 0; y < k_h; y += 37) {
    CHECK(std::memcmp(raw.data() + y * (k_w + 1) + 1, gray.data() + y * k_w, k_w) == 0);
  }
  Vector<u8> rgb(6 * 3);
  REQUIRE(encode_png(6, 1, 3, std::span<const u8>(rgb.data(), rgb.size()), png));
  REQUIRE(walk_chunks(png, width, height, color_type, idat));
  CHECK(color_type == 2);
}

TEST_CASE("png: bad arguments are rejected and write_png produces a file") {
  Vector<u8> png;
  u8 four[4] = {1, 2, 3, 4};
  CHECK_FALSE(encode_png(0, 1, 4, four, png));
  CHECK_FALSE(encode_png(1, 1, 5, four, png));
  CHECK_FALSE(encode_png(2, 1, 4, four, png));  // 2 pixels need 8 bytes
  CHECK(png.empty());
  const auto dir = std::filesystem::temp_directory_path() / "engine_png_tests";
  std::filesystem::create_directories(dir);
  const std::string path = (dir / "one.png").string();
  CHECK(write_png(path, 1, 1, 4, four) == io::Status::Ok);
  CHECK(std::filesystem::file_size(path) == 8 + 25 + 12 + (2 + 5 + 5 + 4) + 12);
  CHECK(write_png(path, 3, 1, 4, four) == io::Status::InvalidArgument);
  std::filesystem::remove_all(dir);
}
