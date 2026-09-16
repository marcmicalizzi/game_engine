#pragma once

// PNG encoding without a compression library: the image data goes into a zlib stream of
// stored (uncompressed) deflate blocks, which every decoder accepts. Output is about the raw
// size plus 1/64; fine for captures, test fixtures, and agent-visible screenshots, not for
// shipping assets. Encoding a 4K RGBA frame is a memcpy plus two checksums.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <foundation/io/vfs.h>

#include <span>
#include <string_view>

namespace engine::image {

// `channels`: 1 gray, 2 gray+alpha, 3 RGB, 4 RGBA; 8 bits per channel, rows tightly packed,
// top row first. False when the dimensions and byte count disagree or `channels` is unsupported.
bool encode_png(u32 width, u32 height, u32 channels, std::span<const u8> pixels, Vector<u8>& out);

io::Status write_png(std::string_view native_path, u32 width, u32 height, u32 channels,
                     std::span<const u8> pixels);

// Checksums PNG relies on; exposed for tests and other container formats.
u32 crc32(std::span<const u8> bytes, u32 seed = 0) noexcept;
u32 adler32(std::span<const u8> bytes) noexcept;

}  // namespace engine::image
