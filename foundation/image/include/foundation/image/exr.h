#pragma once

// **Half-float OpenEXR**, for pictures past 8 bits (E39, roadmap R79): an HDR picture's light, and
// the resolve's radiance before any curve, which a PNG cannot hold. Scanline images of one, three
// or four `HALF` channels, ZIP-compressed, written and read through tinyexr (BSD-3-Clause; fetched
// by cmake/EngineImage.cmake and compiled into a library of its own, so nothing outside this
// module's `exr.cpp` sees it). The pixels go in and come out as halves, so what is written is read
// back to the bit; `half_from_f32` and `f32_from_half` are the conversion, round to nearest even
// like a GPU's store.
//
// **Channel order.** The caller's pixels are interleaved R, G, B[, A] (or Y alone), rows top
// first, like `encode_png`'s. The file's channel list is sorted by name — A, B, G, R — because
// OpenEXR's own library lays a scanline out in sorted order whatever order the list was written
// in, so a file whose list is not sorted reads back with its channels swapped there.
//
// **What 1.0 is** is the caller's: the renderer writes linear Rec. 709 with 1.0 at the picture's
// white (renderer/capture.h), and `ExrOptions::white_luminance` puts the nits of that white in
// the file as OpenEXR's standard `whiteLuminance` attribute when it is known.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <foundation/io/vfs.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::image {

// IEEE 754 binary16 from binary32, rounded to nearest with ties to even; past the largest half
// (65504, and anything that rounds above it) is infinity, a NaN stays a NaN.
u16 half_from_f32(f32 value) noexcept;
// binary16 to binary32, exactly (every half is a float).
f32 f32_from_half(u16 half) noexcept;

struct ExrOptions {
  // The nits of RGB (1, 1, 1), written as `whiteLuminance` when above zero; 0 writes none.
  f32 white_luminance = 0.0f;
};

// A decoded EXR: interleaved halves in the order `encode_exr` takes them.
struct ExrImage {
  u32 width = 0;
  u32 height = 0;
  u32 channels = 0;  // 1 (Y), 3 (RGB) or 4 (RGBA)
  Vector<u16> halves;
  f32 white_luminance = 0.0f;  // 0 when the file has none
};

// `channels` 1, 3 or 4; `halves` holds width * height * channels values. False, with `error`
// saying why, when the sizes disagree or tinyexr refuses.
bool encode_exr(u32 width, u32 height, u32 channels, std::span<const u16> halves, Vector<u8>& out,
                const ExrOptions& options = {}, std::string* error = nullptr);

io::Status write_exr(std::string_view native_path, u32 width, u32 height, u32 channels,
                     std::span<const u16> halves, const ExrOptions& options = {});

// Reads a scanline EXR whose channels are Y, or R, G and B with or without A, any of them HALF or
// FLOAT (a FLOAT channel is rounded to half); anything else is refused with a reason.
bool decode_exr(std::span<const u8> bytes, ExrImage& out, std::string* error = nullptr);
bool read_exr(std::string_view native_path, ExrImage& out, std::string* error = nullptr);

}  // namespace engine::image
