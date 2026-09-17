#pragma once

// PNG encoding without a compression library: the image data goes into a zlib stream produced
// by the engine's own deflate (`deflate.h`), so nothing has to be linked and every decoder
// reads the result. The `Compression` level chooses the trade: `Stored` is a memcpy plus two
// checksums and grows the data by 1/64, `Fast` and `Default` run LZ77 and Huffman coding.
// Scanlines always use filter type 0 (None), so what deflate sees is the pixels themselves.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <foundation/image/deflate.h>
#include <foundation/io/vfs.h>

#include <span>
#include <string_view>

namespace engine::image {

// `channels`: 1 gray, 2 gray+alpha, 3 RGB, 4 RGBA; 8 bits per channel, rows tightly packed,
// top row first. False when the dimensions and byte count disagree or `channels` is unsupported.
bool encode_png(u32 width, u32 height, u32 channels, std::span<const u8> pixels, Vector<u8>& out,
                Compression level = Compression::Default);

io::Status write_png(std::string_view native_path, u32 width, u32 height, u32 channels,
                     std::span<const u8> pixels, Compression level = Compression::Default);

// Checksums PNG relies on; exposed for tests and other container formats.
u32 crc32(std::span<const u8> bytes, u32 seed = 0) noexcept;
u32 adler32(std::span<const u8> bytes) noexcept;

}  // namespace engine::image
