#pragma once

// Image decoding on the CPU: PNG, JPEG, TGA, and BMP in, 8-bit-per-channel pixels out.
// stb_image does the parsing (compiled into src/decode.cpp); this header is the engine-shaped
// surface around it: spans in, engine `Vector` out, `bool` plus an error string or an
// `io::Status`, no exceptions and no ownership of stb's buffers escaping the module.
//
// Decoding is always to 8 bits per channel: a 16-bit PNG is reduced by keeping the high byte,
// so a sample of 0xABCD decodes to 0xAB. Rows come out top first, tightly packed, with no row
// padding. `desired_channels` of 0 keeps the file's channel count; 1 to 4 converts (gray to
// RGB replicates, a missing alpha becomes 255).
//
// Not here: HDR and float formats, KTX2 and the block-compressed formats, and mipmap
// generation; GPU upload is `gfx`'s business, not this module's.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <foundation/io/vfs.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::image {

struct ImageInfo {
  u32 width = 0;
  u32 height = 0;
  u32 channels = 0;  // as stored in the file, whatever a decode is later asked for
};

struct Image {
  u32 width = 0;
  u32 height = 0;
  u32 channels = 0;
  Vector<u8> pixels;  // 8 bits per channel, rows top first, tightly packed
};

// Reads the header only: no pixels are decoded and nothing is allocated. False leaves `out`
// empty and fills `error` (when given) with a non-empty message.
bool probe_image(std::span<const u8> bytes, ImageInfo& out, std::string* error = nullptr);

// `desired_channels`: 0 keeps the file's count, 1 to 4 converts. False leaves `out` empty.
bool decode_image(std::span<const u8> bytes, Image& out, u32 desired_channels = 4,
                  std::string* error = nullptr);

// Reads the file through `io::read_file` and decodes it. The read's status is returned as is;
// bytes that are not a supported image are `io::Status::IoError`, and a `desired_channels`
// above 4 is `io::Status::InvalidArgument` (the file is not touched).
io::Status read_image(std::string_view native_path, Image& out, u32 desired_channels = 4,
                      std::string* error = nullptr);

}  // namespace engine::image
