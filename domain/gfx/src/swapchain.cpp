#include <core/log/log.h>
#include <domain/gfx/device.h>
#include <domain/gfx/swapchain.h>

namespace engine::gfx {

ENGINE_LOG_CATEGORY_DEFINE(log_swapchain, "gfx.swapchain");

namespace {

void set_error(std::string* error, const char* what, VkResult result) {
  if (error != nullptr) *error = std::string(what) + " failed: " + result_name(result);
}

}  // namespace

Swapchain::~Swapchain() { destroy(); }

bool Swapchain::create(const Device& device, const SwapchainDesc& desc, std::string* error) {
  ENGINE_VERIFY(swapchain_ == VK_NULL_HANDLE, "Swapchain::create: already created");
  if (!device.valid() || !device.features().presentation) {
    if (error != nullptr) *error = "Swapchain::create: the device has no VK_KHR_swapchain";
    return false;
  }
  if (desc.surface == VK_NULL_HANDLE) {
    if (error != nullptr) *error = "Swapchain::create: no surface";
    return false;
  }
  const Handles& h = device.handles();
  VkBool32 supported = VK_FALSE;
  const VkResult r = vkGetPhysicalDeviceSurfaceSupportKHR(h.physical, device.graphics_family(),
                                                          desc.surface, &supported);
  if (r != VK_SUCCESS || supported != VK_TRUE) {
    if (error != nullptr)
      *error = "Swapchain::create: the graphics queue cannot present to this surface";
    return false;
  }
  device_ = &device;
  desc_ = desc;
  if (!create_chain(error)) {
    device_ = nullptr;
    return false;
  }
  return true;
}

bool Swapchain::create_chain(std::string* error) {
  const Handles& h = device_->handles();
  VkSurfaceCapabilitiesKHR caps{};
  if (const VkResult r =
          vkGetPhysicalDeviceSurfaceCapabilitiesKHR(h.physical, desc_.surface, &caps);
      r != VK_SUCCESS) {
    set_error(error, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", r);
    return false;
  }

  // Format: the preferred one when offered, otherwise the first the surface lists.
  u32 format_count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(h.physical, desc_.surface, &format_count, nullptr);
  Vector<VkSurfaceFormatKHR> formats(format_count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(h.physical, desc_.surface, &format_count, formats.data());
  if (formats.empty()) {
    if (error != nullptr) *error = "Swapchain: the surface offers no formats";
    return false;
  }
  VkSurfaceFormatKHR chosen = formats[0];
  for (const VkSurfaceFormatKHR& f : formats) {
    if (f.format == desc_.preferred_format && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      chosen = f;
      break;
    }
  }
  format_ = chosen.format;
  color_space_ = chosen.colorSpace;

  // Present mode.
  u32 mode_count = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(h.physical, desc_.surface, &mode_count, nullptr);
  Vector<VkPresentModeKHR> modes(mode_count);
  vkGetPhysicalDeviceSurfacePresentModesKHR(h.physical, desc_.surface, &mode_count, modes.data());
  auto offers = [&](VkPresentModeKHR m) {
    for (const VkPresentModeKHR mode : modes) {
      if (mode == m) return true;
    }
    return false;
  };
  mode_ = VK_PRESENT_MODE_FIFO_KHR;
  if (!desc_.vsync) {
    if (offers(VK_PRESENT_MODE_MAILBOX_KHR)) {
      mode_ = VK_PRESENT_MODE_MAILBOX_KHR;
    } else if (offers(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
      mode_ = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
  }

  // Extent: the surface dictates it when it reports a fixed size; otherwise the window's.
  if (caps.currentExtent.width != 0xFFFFFFFFu) {
    extent_ = caps.currentExtent;
  } else {
    extent_.width = desc_.width < caps.minImageExtent.width   ? caps.minImageExtent.width
                    : desc_.width > caps.maxImageExtent.width ? caps.maxImageExtent.width
                                                              : desc_.width;
    extent_.height = desc_.height < caps.minImageExtent.height   ? caps.minImageExtent.height
                     : desc_.height > caps.maxImageExtent.height ? caps.maxImageExtent.height
                                                                 : desc_.height;
  }
  if (extent_.width == 0 || extent_.height == 0) {
    if (error != nullptr) *error = "Swapchain: zero-sized surface";
    return false;
  }

  u32 image_count =
      desc_.min_image_count > caps.minImageCount ? desc_.min_image_count : caps.minImageCount;
  if (caps.maxImageCount != 0 && image_count > caps.maxImageCount) image_count = caps.maxImageCount;

  VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  transfer_src_ = false;
  if (desc_.transfer_src && (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0) {
    usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    transfer_src_ = true;
  }
  if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0) {
    usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  }

  VkSwapchainCreateInfoKHR info{};
  info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  info.surface = desc_.surface;
  info.minImageCount = image_count;
  info.imageFormat = format_;
  info.imageColorSpace = color_space_;
  info.imageExtent = extent_;
  info.imageArrayLayers = 1;
  info.imageUsage = usage;
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0
                          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                          : caps.currentTransform;
  info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
  info.presentMode = mode_;
  info.clipped = VK_TRUE;
  info.oldSwapchain = swapchain_;
  VkSwapchainKHR chain = VK_NULL_HANDLE;
  if (const VkResult r = vkCreateSwapchainKHR(h.device, &info, nullptr, &chain); r != VK_SUCCESS) {
    set_error(error, "vkCreateSwapchainKHR", r);
    return false;
  }
  destroy_chain();  // the old chain (retired through oldSwapchain) and its views/semaphores
  swapchain_ = chain;

  u32 count = 0;
  vkGetSwapchainImagesKHR(h.device, swapchain_, &count, nullptr);
  Vector<VkImage> raw(count);
  vkGetSwapchainImagesKHR(h.device, swapchain_, &count, raw.data());
  images_.resize(count);
  views_.resize(count);
  render_finished_.resize(count);
  for (u32 i = 0; i < count; ++i) {
    ImageResource& image = images_[i];
    image = ImageResource{};
    image.image = raw[i];
    image.format = format_;
    image.width = extent_.width;
    image.height = extent_.height;
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = raw[i];
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format_;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (const VkResult r = vkCreateImageView(h.device, &view, nullptr, &views_[i]);
        r != VK_SUCCESS) {
      set_error(error, "vkCreateImageView(swapchain)", r);
      return false;
    }
    VkSemaphoreCreateInfo semaphore{};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (const VkResult r = vkCreateSemaphore(h.device, &semaphore, nullptr, &render_finished_[i]);
        r != VK_SUCCESS) {
      set_error(error, "vkCreateSemaphore(render finished)", r);
      return false;
    }
  }
  ENGINE_LOG_DEBUG(log_swapchain, "swapchain created", log::field("width", extent_.width),
                   log::field("height", extent_.height), log::field("images", count),
                   log::field("format", static_cast<u32>(format_)),
                   log::field("present_mode", static_cast<u32>(mode_)),
                   log::field("transfer_src", transfer_src_));
  return true;
}

void Swapchain::destroy_chain() noexcept {
  if (device_ == nullptr) return;
  const Handles& h = device_->handles();
  for (VkImageView view : views_) {
    if (view != VK_NULL_HANDLE) vkDestroyImageView(h.device, view, nullptr);
  }
  for (VkSemaphore semaphore : render_finished_) {
    if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(h.device, semaphore, nullptr);
  }
  views_.clear();
  render_finished_.clear();
  images_.clear();
  if (swapchain_ != VK_NULL_HANDLE) {
    vkDestroySwapchainKHR(h.device, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
  }
}

void Swapchain::destroy() noexcept {
  if (device_ == nullptr) return;
  vkDeviceWaitIdle(device_->handles().device);
  destroy_chain();
  device_ = nullptr;
}

bool Swapchain::resize(u32 width, u32 height, std::string* error) {
  ENGINE_VERIFY(device_ != nullptr, "Swapchain::resize: not created");
  desc_.width = width;
  desc_.height = height;
  if (width == 0 || height == 0) return true;  // minimized: keep the old chain
  vkDeviceWaitIdle(device_->handles().device);
  return create_chain(error);
}

PresentStatus Swapchain::acquire(VkSemaphore signal, u32& image_index, u64 timeout_ns) {
  ENGINE_VERIFY(swapchain_ != VK_NULL_HANDLE, "Swapchain::acquire: not created");
  if (desc_.width == 0 || desc_.height == 0) return PresentStatus::OutOfDate;
  const VkResult r = vkAcquireNextImageKHR(device_->handles().device, swapchain_, timeout_ns,
                                           signal, VK_NULL_HANDLE, &image_index);
  if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) return PresentStatus::Ok;
  if (r == VK_ERROR_OUT_OF_DATE_KHR) return PresentStatus::OutOfDate;
  last_error_ = result_name(r);
  return PresentStatus::Error;
}

PresentStatus Swapchain::present(u32 image_index, VkSemaphore wait) {
  ENGINE_VERIFY(swapchain_ != VK_NULL_HANDLE, "Swapchain::present: not created");
  VkPresentInfoKHR info{};
  info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  info.waitSemaphoreCount = wait != VK_NULL_HANDLE ? 1 : 0;
  info.pWaitSemaphores = &wait;
  info.swapchainCount = 1;
  info.pSwapchains = &swapchain_;
  info.pImageIndices = &image_index;
  const VkResult r = vkQueuePresentKHR(device_->handles().graphics_queue, &info);
  if (r == VK_SUCCESS) return PresentStatus::Ok;
  if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR) return PresentStatus::OutOfDate;
  last_error_ = result_name(r);
  return PresentStatus::Error;
}

}  // namespace engine::gfx
