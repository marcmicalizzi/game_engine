#pragma once

// GPU image capture (docs/plan/06-tooling.md: the capture API agents and tests read frames
// through). A capture is an immediate submission that copies an image into host memory and
// leaves the image in the layout it was found in. The render graph tests and engine-view's
// --capture use the same path, so what a test checks is what a screenshot shows.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/gfx/device.h>
#include <domain/gfx/resources.h>
#include <domain/gfx/rhi.h>

#include <string>

namespace engine::gfx {

struct Capture {
  u32 width = 0;
  u32 height = 0;
  Format format = Format::Undefined;
  u32 bytes_per_pixel = 0;
  Vector<u8> bytes;  // rows tightly packed, top row first
};

// Bytes per pixel for the formats capture understands (8-bit RGBA/BGRA UNORM and SRGB, the two
// 10-bit packed UNORM orders, R32 UINT/SFLOAT, D32 SFLOAT, R16G16B16A16 SFLOAT, R32G32B32A32
// SFLOAT); 0 otherwise.
u32 capture_bytes_per_pixel(Format format) noexcept;

// `layout` is the image's current layout; it is restored afterwards unless it is Undefined, in
// which case the image is left in TransferSrc. Blocks until the copy completes.
bool capture_image(const Device& device, const ImageResource& image, ImageLayout layout,
                   Capture& out, std::string* error = nullptr);

// Converts an 8-bit RGBA or BGRA capture, or a 10-bit one (A2B10G10R10, A2R10G10B10), to tightly
// packed RGBA8 (alpha forced opaque when `opaque`). An 8-bit capture is copied; a 10-bit one is
// reduced to 8 bits — each code to the nearest of 255 steps, or with `dither` through the output
// encode's own noise at 8 bits (display.h), so the PNG of a dithered 10-bit picture carries no
// bands the 10-bit picture did not. False for other formats.
bool capture_to_rgba8(const Capture& capture, Vector<u8>& rgba, bool opaque = true,
                      bool dither = false);

}  // namespace engine::gfx
