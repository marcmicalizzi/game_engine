#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/capture.h>
#include <domain/gfx/device.h>
#include <domain/gfx/display.h>

#include <cstring>

namespace engine::gfx {

const char* format_name(Format format) noexcept {
  switch (format) {
    case Format::R8G8B8A8Unorm: return "R8G8B8A8Unorm";
    case Format::R8G8B8A8Srgb: return "R8G8B8A8Srgb";
    case Format::B8G8R8A8Unorm: return "B8G8R8A8Unorm";
    case Format::B8G8R8A8Srgb: return "B8G8R8A8Srgb";
    case Format::A2R10G10B10Unorm: return "A2R10G10B10Unorm";
    case Format::A2B10G10R10Unorm: return "A2B10G10R10Unorm";
    case Format::R16G16B16A16Sfloat: return "R16G16B16A16Sfloat";
    case Format::R32G32B32A32Sfloat: return "R32G32B32A32Sfloat";
    default: return "other";
  }
}

const char* color_space_name(ColorSpace space) noexcept {
  switch (space) {
    case ColorSpace::SrgbNonlinear: return "srgb_nonlinear";
    case ColorSpace::DisplayP3Nonlinear: return "display_p3_nonlinear";
    case ColorSpace::ExtendedSrgbLinear: return "extended_srgb_linear";
    case ColorSpace::DisplayP3Linear: return "display_p3_linear";
    case ColorSpace::DciP3Nonlinear: return "dci_p3_nonlinear";
    case ColorSpace::Bt709Linear: return "bt709_linear";
    case ColorSpace::Bt709Nonlinear: return "bt709_nonlinear";
    case ColorSpace::Bt2020Linear: return "bt2020_linear";
    case ColorSpace::Hdr10St2084: return "hdr10_st2084";
    case ColorSpace::DolbyVision: return "dolby_vision";
    case ColorSpace::Hdr10Hlg: return "hdr10_hlg";
    case ColorSpace::AdobeRgbLinear: return "adobe_rgb_linear";
    case ColorSpace::AdobeRgbNonlinear: return "adobe_rgb_nonlinear";
    case ColorSpace::PassThrough: return "pass_through";
    case ColorSpace::ExtendedSrgbNonlinear: return "extended_srgb_nonlinear";
    case ColorSpace::DisplayNativeAmd: return "display_native_amd";
    default: return "other";
  }
}

std::string describe_surface_formats(std::span<const SurfaceFormat> formats) {
  if (formats.empty()) return "nothing";
  std::string out;
  for (const SurfaceFormat& f : formats) {
    if (!out.empty()) out += ", ";
    const char* format = format_name(f.format);
    out += std::strcmp(format, "other") == 0 ? std::to_string(static_cast<u32>(f.format)) : format;
    out.push_back(' ');
    const char* space = color_space_name(f.color_space);
    out +=
        std::strcmp(space, "other") == 0 ? std::to_string(static_cast<u32>(f.color_space)) : space;
  }
  return out;
}

u32 capture_bytes_per_pixel(Format format) noexcept {
  switch (format) {
    case Format::R8G8B8A8Unorm:
    case Format::R8G8B8A8Srgb:
    case Format::B8G8R8A8Unorm:
    case Format::B8G8R8A8Srgb:
    case Format::A2R10G10B10Unorm:
    case Format::A2B10G10R10Unorm:
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

namespace {

// A 10-bit capture to 8 bits a channel: each code onto the nearest of 255 steps, or — with
// `dither` — through the output encode's own noise at 8 bits (display.h), which is how a picture
// quantized once at 10 bits with dither becomes an 8-bit PNG without the bands a plain rounding
// would put back. `red_low` is A2B10G10R10's order, red in the low ten bits.
void ten_bit_to_rgba8(const Capture& capture, bool red_low, bool opaque, bool dither, u8* dst) {
  const u8* src = capture.bytes.data();
  for (u32 y = 0; y < capture.height; ++y) {
    for (u32 x = 0; x < capture.width; ++x) {
      u32 word = 0;
      std::memcpy(&word, src, sizeof(word));
      const u32 low = word & 1023u;
      const u32 mid = (word >> 10) & 1023u;
      const u32 high = (word >> 20) & 1023u;
      const u32 codes[3] = {red_low ? low : high, mid, red_low ? high : low};
      for (u32 c = 0; c < 3; ++c) {
        f32 v = static_cast<f32>(codes[c]) / 1023.0f;
        if (dither) v = display_dithered(v, x, y, 255);
        const f32 clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        dst[c] = static_cast<u8>(clamped * 255.0f + 0.5f);
      }
      dst[3] = opaque ? 255 : static_cast<u8>((word >> 30) * 85u);
      src += 4;
      dst += 4;
    }
  }
}

}  // namespace

bool capture_to_rgba8(const Capture& capture, Vector<u8>& rgba, bool opaque, bool dither) {
  bool bgra = false;
  const u32 pixels = capture.width * capture.height;
  switch (capture.format) {
    case Format::R8G8B8A8Unorm:
    case Format::R8G8B8A8Srgb: break;
    case Format::B8G8R8A8Unorm:
    case Format::B8G8R8A8Srgb: bgra = true; break;
    case Format::A2R10G10B10Unorm:
    case Format::A2B10G10R10Unorm:
      if (capture.bytes.size() != pixels * 4) return false;
      rgba.resize(pixels * 4);
      ten_bit_to_rgba8(capture, capture.format == Format::A2B10G10R10Unorm, opaque, dither,
                       rgba.data());
      return true;
    default: return false;
  }
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
