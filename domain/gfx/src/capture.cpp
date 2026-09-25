#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/capture.h>
#include <domain/gfx/device.h>

#include <cstring>

namespace engine::gfx {

u32 capture_bytes_per_pixel(Format format) noexcept {
  switch (format) {
    case Format::R8G8B8A8Unorm:
    case Format::R8G8B8A8Srgb:
    case Format::B8G8R8A8Unorm:
    case Format::B8G8R8A8Srgb:
    case Format::R32Uint:
    case Format::R32Sfloat:
    case Format::D32Sfloat: return 4;
    case Format::R16G16B16A16Sfloat: return 8;
    case Format::R32G32B32A32Sfloat: return 16;
    default: return 0;
  }
}

bool capture_image(const Device& device, const ImageResource& image, ImageLayout image_layout,
                   Capture& out, std::string* error) {
  out = Capture{};
  const u32 bpp = capture_bytes_per_pixel(image.format);
  if (bpp == 0) {
    if (error != nullptr) *error = "capture_image: unsupported format";
    return false;
  }
  if (!image.image || image.width == 0 || image.height == 0) {
    if (error != nullptr) *error = "capture_image: no image";
    return false;
  }
  const u64 bytes = u64{image.width} * image.height * bpp;
  BufferResource staging;
  if (!create_buffer(device, bytes, BufferUsage::TransferDst, true, staging, error)) return false;

  const bool depth = image.format == Format::D32Sfloat;
  const VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  const VkImageLayout layout = vk::native(image_layout);
  const VkImage native_image = vk::native(image.image);
  const bool ok = submit_immediate(
      device,
      [&](CommandList list) {
        const VkCommandBuffer commands = vk::native(list);
        image_barrier(commands, native_image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                      VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {aspect, 0, 0, 1};
        region.imageExtent = {image.width, image.height, 1};
        vkCmdCopyImageToBuffer(commands, native_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               vk::native(staging.buffer), 1, &region);
        if (layout != VK_IMAGE_LAYOUT_UNDEFINED && layout != VK_IMAGE_LAYOUT_PREINITIALIZED) {
          image_barrier(commands, native_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout,
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
    case Format::R8G8B8A8Unorm:
    case Format::R8G8B8A8Srgb: break;
    case Format::B8G8R8A8Unorm:
    case Format::B8G8R8A8Srgb: bgra = true; break;
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
