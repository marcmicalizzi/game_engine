#include <foundation/image/png.h>

#include <cstring>

namespace engine::image {

namespace {

struct CrcTable {
  u32 entries[256];
  constexpr CrcTable() : entries{} {
    for (u32 n = 0; n < 256; ++n) {
      u32 c = n;
      for (int k = 0; k < 8; ++k)
        c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      entries[n] = c;
    }
  }
};
constexpr CrcTable k_crc_table{};

void put_u32_be(Vector<u8>& out, u32 v) {
  out.push_back(static_cast<u8>(v >> 24));
  out.push_back(static_cast<u8>(v >> 16));
  out.push_back(static_cast<u8>(v >> 8));
  out.push_back(static_cast<u8>(v));
}

// Appends a chunk: length, type, data, CRC over type + data.
void put_chunk(Vector<u8>& out, const char type[4], std::span<const u8> data) {
  put_u32_be(out, static_cast<u32>(data.size()));
  const u32 type_offset = out.size();
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>(type[i]));
  // One memcpy rather than Vector::append's element-by-element copy: an IDAT payload is the
  // whole image, and at `Compression::Stored` that copy is most of the encoder's time.
  const u32 at = out.size();
  if (!data.empty()) {
    out.resize(at + static_cast<u32>(data.size()));
    std::memcpy(out.data() + at, data.data(), data.size());
  }
  const u32 crc = crc32(std::span<const u8>(out.data() + type_offset, 4 + data.size()));
  put_u32_be(out, crc);
}

}  // namespace

u32 crc32(std::span<const u8> bytes, u32 seed) noexcept {
  u32 c = seed ^ 0xFFFFFFFFu;
  for (const u8 b : bytes)
    c = k_crc_table.entries[(c ^ b) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

u32 adler32(std::span<const u8> bytes) noexcept {
  u32 a = 1;
  u32 b = 0;
  usize i = 0;
  while (i < bytes.size()) {
    // 5552 is the largest block that cannot overflow 32 bits before the modulo.
    const usize end = i + 5552 < bytes.size() ? i + 5552 : bytes.size();
    for (; i < end; ++i) {
      a += bytes[i];
      b += a;
    }
    a %= 65521;
    b %= 65521;
  }
  return (b << 16) | a;
}

bool encode_png(u32 width, u32 height, u32 channels, std::span<const u8> pixels, Vector<u8>& out,
                Compression level) {
  out.clear();
  if (width == 0 || height == 0 || channels < 1 || channels > 4) return false;
  const u64 row_bytes = u64{width} * channels;
  if (row_bytes * height != pixels.size() || row_bytes + 1 > 0x7FFFFFFFu) return false;

  // Raw scanlines with filter byte 0 each: what the zlib stream carries.
  const u64 raw_size = (row_bytes + 1) * height;
  if (raw_size > 0xFFFFFFFFu) return false;
  Vector<u8> raw(static_cast<u32>(raw_size));
  for (u32 y = 0; y < height; ++y) {
    u8* dst = raw.data() + static_cast<usize>(y) * (row_bytes + 1);
    dst[0] = 0;
    std::memcpy(dst + 1, pixels.data() + static_cast<usize>(y) * row_bytes,
                static_cast<usize>(row_bytes));
  }

  // The IDAT payload: a zlib stream over those scanlines at the requested level.
  Vector<u8> zlib;
  zlib_compress(std::span<const u8>(raw.data(), raw.size()), level, zlib);

  static constexpr u8 k_signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  out.reserve(8 + 25 + 12 + zlib.size() + 12);
  for (const u8 b : k_signature)
    out.push_back(b);

  Vector<u8> ihdr;
  put_u32_be(ihdr, width);
  put_u32_be(ihdr, height);
  ihdr.push_back(8);  // bit depth
  static constexpr u8 k_color_type[5] = {0, 0, 4, 2, 6};
  ihdr.push_back(k_color_type[channels]);
  ihdr.push_back(0);  // compression
  ihdr.push_back(0);  // filter
  ihdr.push_back(0);  // interlace
  put_chunk(out, "IHDR", std::span<const u8>(ihdr.data(), ihdr.size()));
  put_chunk(out, "IDAT", std::span<const u8>(zlib.data(), zlib.size()));
  put_chunk(out, "IEND", {});
  return true;
}

io::Status write_png(std::string_view native_path, u32 width, u32 height, u32 channels,
                     std::span<const u8> pixels, Compression level) {
  Vector<u8> bytes;
  if (!encode_png(width, height, channels, pixels, bytes, level))
    return io::Status::InvalidArgument;
  return io::write_file(
      native_path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

}  // namespace engine::image
