#include <domain/gfx/capture.h>
#include <domain/gfx/device.h>

#include <cstring>

namespace engine::gfx {

u32 capture_bytes_per_pixel(VkFormat format) noexcept {
  switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT: return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 0;
  }
}

bool capture_image(const Device& device, const ImageResource& image, VkImageLayout layout,
                   Capture& out, std::string* error) {
  out = Capture{};
  const u32 bpp = capture_bytes_per_pixel(image.format);
  if (bpp == 0) {
    if (error != nullptr) *error = "capture_image: unsupported format";
    return false;
  }
  if (image.image == VK_NULL_HANDLE || image.width == 0 || image.height == 0) {
    if (error != nullptr) *error = "capture_image: no image";
    return false;
  }
  const u64 bytes = u64{image.width} * image.height * bpp;
  BufferResource staging;
  if (!create_buffer(device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, staging, error))
    return false;

  const bool depth = image.format == VK_FORMAT_D32_SFLOAT;
  const VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  const bool ok = submit_immediate(
      device,
      [&](VkCommandBuffer commands) {
        image_barrier(commands, image.image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                      VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {aspect, 0, 0, 1};
        region.imageExtent = {image.width, image.height, 1};
        vkCmdCopyImageToBuffer(commands, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               staging.buffer, 1, &region);
        if (layout != VK_IMAGE_LAYOUT_UNDEFINED && layout != VK_IMAGE_LAYOUT_PREINITIALIZED) {
          image_barrier(commands, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout,
                        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT);
        }
      },
      error);
  if (ok) {
    out.width = image.width;
    out.height = image.height;
    out.format = image.format;
    out.bytes_per_pixel = bpp;
    out.bytes.resize(static_cast<u32>(bytes));
    std::memcpy(out.bytes.data(), staging.mapped, static_cast<usize>(bytes));
  }
  destroy_buffer(device, staging);
  return ok;
}

bool capture_to_rgba8(const Capture& capture, Vector<u8>& rgba, bool opaque) {
  bool bgra = false;
  switch (capture.format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB: break;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB: bgra = true; break;
    default: return false;
  }
  const u32 pixels = capture.width * capture.height;
  if (capture.bytes.size() != pixels * 4) return false;
  rgba.resize(pixels * 4);
  const u8* src = capture.bytes.data();
  u8* dst = rgba.data();
  for (u32 i = 0; i < pixels; ++i) {
    dst[0] = bgra ? src[2] : src[0];
    dst[1] = src[1];
    dst[2] = bgra ? src[0] : src[2];
    dst[3] = opaque ? 255 : src[3];
    src += 4;
    dst += 4;
  }
  return true;
}

}  // namespace engine::gfx
