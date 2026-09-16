#pragma once

// GPU image capture (docs/plan/06-tooling.md: the capture API agents and tests read frames
// through). A capture is an immediate submission that copies an image into host memory and
// leaves the image in the layout it was found in. The render graph tests and engine-view's
// --capture use the same path, so what a test checks is what a screenshot shows.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/gfx/vulkan.h>

#include <string>

namespace engine::gfx {

struct Capture {
  u32 width = 0;
  u32 height = 0;
  VkFormat format = VK_FORMAT_UNDEFINED;
  u32 bytes_per_pixel = 0;
  Vector<u8> bytes;  // rows tightly packed, top row first
};

// Bytes per pixel for the formats capture understands (8-bit RGBA/BGRA UNORM and SRGB, R32
// UINT/SFLOAT, D32 SFLOAT, R16G16B16A16 SFLOAT, R32G32B32A32 SFLOAT); 0 otherwise.
u32 capture_bytes_per_pixel(VkFormat format) noexcept;

// `layout` is the image's current layout; it is restored afterwards unless it is UNDEFINED, in
// which case the image is left in TRANSFER_SRC_OPTIMAL. Blocks until the copy completes.
bool capture_image(const Device& device, const ImageResource& image, VkImageLayout layout,
                   Capture& out, std::string* error = nullptr);

// Converts an 8-bit RGBA or BGRA capture to tightly packed RGBA8 (alpha forced opaque when
// `opaque`). False for other formats.
bool capture_to_rgba8(const Capture& capture, Vector<u8>& rgba, bool opaque = true);

}  // namespace engine::gfx
