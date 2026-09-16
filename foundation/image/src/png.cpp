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
  for (const u8 b : data)
    out.push_back(b);
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

bool encode_png(u32 width, u32 height, u32 channels, std::span<const u8> pixels, Vector<u8>& out) {
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

  // zlib stream: header, stored blocks of at most 65535 bytes, adler32 of the raw data.
  const u32 block_count = (raw.size() + 65534) / 65535;
  Vector<u8> zlib;
  zlib.reserve(2 + raw.size() + block_count * 5 + 4);
  zlib.push_back(0x78);  // deflate, 32K window
  zlib.push_back(0x01);  // fastest, no dictionary, check bits make the header a multiple of 31
  u32 offset = 0;
  do {
    const u32 len = raw.size() - offset < 65535 ? raw.size() - offset : 65535;
    const bool final = offset + len == raw.size();
    zlib.push_back(final ? 1 : 0);  // BFINAL, BTYPE = 00 (stored)
    zlib.push_back(static_cast<u8>(len));
    zlib.push_back(static_cast<u8>(len >> 8));
    zlib.push_back(static_cast<u8>(~len));
    zlib.push_back(static_cast<u8>(~len >> 8));
    for (u32 i = 0; i < len; ++i)
      zlib.push_back(raw[offset + i]);
    offset += len;
  } while (offset < raw.size());
  put_u32_be(zlib, adler32(std::span<const u8>(raw.data(), raw.size())));

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
                     std::span<const u8> pixels) {
  Vector<u8> bytes;
  if (!encode_png(width, height, channels, pixels, bytes)) return io::Status::InvalidArgument;
  return io::write_file(
      native_path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

}  // namespace engine::image
