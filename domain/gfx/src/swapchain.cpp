#include <core/log/log.h>
#include <core/platform/thread.h>
#include <domain/gfx/backend/vulkan/swapchain.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/device.h>

#include <chrono>

namespace engine::gfx {

ENGINE_LOG_CATEGORY_DEFINE(log_swapchain, "gfx.swapchain");

namespace {

void set_error(std::string* error, const char* what, VkResult result) {
  if (error != nullptr) *error = std::string(what) + " failed: " + result_name(result);
}

// The clock time::monotonic_ns() reads (core/time/src/clock.cpp), read here directly because gfx
// does not depend on the time module for one call: display times are handed out on it so a
// caller can subtract them from its own frame timestamps.
i64 steady_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The driver's queue of presents whose display time has not been read yet, and how many one poll
// reads. **The queue is deep because reading it is not free**: on NVIDIA's Windows driver (610.88,
// its DXGI-layered presentation) vkGetPastPresentationTimingEXT blocks until the presentation
// thread's next vblank, about a refresh, whatever the flags ask for — measured at 10.4-11.2 ms a
// call at 82 Hz — so a caller that polled every frame would be paced by the poll. A deep queue lets
// a run be read once, at its end: 16,384 presents is 200 s at 82 Hz. When the queue is full,
// present() stops asking for times rather than fail (timing_dropped() counts those presents).
constexpr u32 k_timing_queue = 16384;
constexpr u32 k_timing_batch = 1024;

bool host_domain(VkTimeDomainKHR domain) noexcept {
  return domain == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR ||
         domain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR ||
         domain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR;
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
    if (f.format == vk::native(desc_.preferred_format) &&
        f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      chosen = f;
      break;
    }
  }
  format_ = vk::wrap(chosen.format);
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
  if (desc_.present_mode != VK_PRESENT_MODE_MAX_ENUM_KHR) {
    if (offers(desc_.present_mode)) {
      mode_ = desc_.present_mode;
    } else {
      ENGINE_LOG_WARN(log_swapchain, "the surface does not offer the present mode asked for; FIFO",
                      log::field("asked", static_cast<u32>(desc_.present_mode)));
    }
  } else if (!desc_.vsync) {
    if (offers(VK_PRESENT_MODE_MAILBOX_KHR)) {
      mode_ = VK_PRESENT_MODE_MAILBOX_KHR;
    } else if (offers(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
      mode_ = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
  }

  // Present ids, waits and display timing: asked for, enabled on the device, and offered by this
  // surface (VK_KHR_get_surface_capabilities2's query, which the device only enables them with).
  ids_ = false;
  wait_ = false;
  timing_ = false;
  if ((desc_.timing || desc_.present_wait) && h.present_id2) {
    VkPresentTimingSurfaceCapabilitiesEXT timing_caps{};
    timing_caps.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT;
    VkSurfaceCapabilitiesPresentWait2KHR wait_caps{};
    wait_caps.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_WAIT_2_KHR;
    wait_caps.pNext = h.present_timing ? &timing_caps : nullptr;
    VkSurfaceCapabilitiesPresentId2KHR id_caps{};
    id_caps.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR;
    id_caps.pNext = &wait_caps;
    VkSurfaceCapabilities2KHR caps2{};
    caps2.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
    caps2.pNext = &id_caps;
    VkPhysicalDeviceSurfaceInfo2KHR surface_info{};
    surface_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
    surface_info.surface = desc_.surface;
    if (vkGetPhysicalDeviceSurfaceCapabilities2KHR(h.physical, &surface_info, &caps2) ==
        VK_SUCCESS) {
      ids_ = id_caps.presentId2Supported == VK_TRUE;
      wait_ = desc_.present_wait && ids_ && h.present_wait2 &&
              wait_caps.presentWait2Supported == VK_TRUE;
      timing_ =
          desc_.timing && ids_ && h.present_timing &&
          timing_caps.presentTimingSupported == VK_TRUE &&
          (timing_caps.presentStageQueries & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT) != 0;
    }
  }

  // Extent: the surface dictates it when it reports a fixed size; otherwise the window's.
  if (caps.currentExtent.width != 0xFFFFFFFFu) {
    extent_ = vk::wrap(caps.currentExtent);
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
  if (ids_) info.flags |= VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;
  if (wait_) info.flags |= VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR;
  if (timing_) info.flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
  info.surface = desc_.surface;
  info.minImageCount = image_count;
  info.imageFormat = vk::native(format_);
  info.imageColorSpace = color_space_;
  info.imageExtent = vk::native(extent_);
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
  ++chains_;
  chain_first_id_ = present_id_ + 1;
  if (timing_) setup_timing();

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
    image.image = vk::wrap(raw[i]);
    image.format = format_;
    image.width = extent_.width;
    image.height = extent_.height;
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = raw[i];
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = vk::native(format_);
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView image_view = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateImageView(h.device, &view, nullptr, &image_view);
        r != VK_SUCCESS) {
      set_error(error, "vkCreateImageView(swapchain)", r);
      return false;
    }
    views_[i] = vk::wrap(image_view);
    VkSemaphoreCreateInfo semaphore{};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore finished = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateSemaphore(h.device, &semaphore, nullptr, &finished);
        r != VK_SUCCESS) {
      set_error(error, "vkCreateSemaphore(render finished)", r);
      return false;
    }
    render_finished_[i] = vk::wrap(finished);
  }
  ENGINE_LOG_DEBUG(log_swapchain, "swapchain created", log::field("width", extent_.width),
                   log::field("height", extent_.height), log::field("images", count),
                   log::field("format", static_cast<u32>(format_)),
                   log::field("present_mode", static_cast<u32>(mode_)),
                   log::field("transfer_src", transfer_src_), log::field("present_ids", ids_),
                   log::field("present_wait", wait_), log::field("present_timing", timing_),
                   log::field("time_domain", timing_domain_),
                   log::field("refresh_ns", refresh_ns_));
  return true;
}

// ---- present timing --------------------------------------------------------------------------
//
// The driver reports each present's first-pixel-out time in a time domain of the swapchain's
// choosing: the host's (QueryPerformanceCounter, CLOCK_MONOTONIC), or one local to the swapchain
// or the present stage. A host domain is preferred because it is the clock the frame loop reads;
// either way the ticks are mapped onto steady_ns() by a two-point calibration through
// vkGetCalibratedTimestampsKHR — a tick count read between two clock readings, once at creation,
// again 5 ms later, and again every half second after — since neither the tick rate of
// QueryPerformanceCounter nor a local domain's is anything this module is told.

void Swapchain::setup_timing() noexcept {
  const VkDevice device = device_->handles().device;
  // The deepest queue the driver takes, halving from k_timing_queue.
  timing_queue_ = 0;
  for (u32 size = k_timing_queue; size >= 64; size /= 2) {
    if (vkSetSwapchainPresentTimingQueueSizeEXT(device, swapchain_, size) == VK_SUCCESS) {
      timing_queue_ = size;
      break;
    }
  }
  timing_outstanding_ = 0;
  if (timing_queue_ == 0) {
    timing_ = false;
    return;
  }
  VkSwapchainTimingPropertiesEXT properties{};
  properties.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT;
  u64 counter = 0;
  if (vkGetSwapchainTimingPropertiesEXT(device, swapchain_, &properties, &counter) == VK_SUCCESS) {
    refresh_ns_ = properties.refreshDuration;
    timing_properties_counter_ = counter;
  }
  // The domains the chain can report in: prefer the host's, then the present stage's own.
  VkSwapchainTimeDomainPropertiesEXT domains{};
  domains.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT;
  if (vkGetSwapchainTimeDomainPropertiesEXT(device, swapchain_, &domains, nullptr) != VK_SUCCESS ||
      domains.timeDomainCount == 0) {
    timing_ = false;
    return;
  }
  Vector<VkTimeDomainKHR> kinds(domains.timeDomainCount);
  Vector<u64> ids(domains.timeDomainCount);
  domains.pTimeDomains = kinds.data();
  domains.pTimeDomainIds = ids.data();
  if (vkGetSwapchainTimeDomainPropertiesEXT(device, swapchain_, &domains, nullptr) != VK_SUCCESS) {
    timing_ = false;
    return;
  }
  u32 chosen = 0;
  for (u32 i = 0; i < domains.timeDomainCount; ++i) {
    if (host_domain(kinds[i])) {
      chosen = i;
      break;
    }
    if (kinds[i] == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT) chosen = i;
  }
  timing_domain_ = static_cast<u32>(kinds[chosen]);
  timing_domain_id_ = ids[chosen];
  if (!calibrate(0)) {
    timing_ = false;
    return;
  }
  platform::sleep_ms(5);
  if (!calibrate(1)) timing_ = false;
  past_.resize(k_timing_batch);
  past_stages_.resize(k_timing_batch);
  const f64 span_ticks = static_cast<f64>(calibration_ticks_[1] - calibration_ticks_[0]);
  ENGINE_LOG_INFO(
      log_swapchain, "present timing", log::field("time_domain", timing_domain_),
      log::field("domains", domains.timeDomainCount), log::field("refresh_ns", refresh_ns_),
      log::field("queue", timing_queue_),
      log::field("ns_per_tick",
                 span_ticks > 0.0
                     ? static_cast<f64>(calibration_ns_[1] - calibration_ns_[0]) / span_ticks
                     : 0.0));
}

bool Swapchain::calibrate(u32 slot) noexcept {
  VkSwapchainCalibratedTimestampInfoEXT local{};
  local.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT;
  local.swapchain = swapchain_;
  local.presentStage = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
  local.timeDomainId = timing_domain_id_;
  VkCalibratedTimestampInfoKHR info{};
  info.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
  info.timeDomain = static_cast<VkTimeDomainKHR>(timing_domain_);
  if (!host_domain(info.timeDomain)) info.pNext = &local;
  u64 ticks = 0;
  u64 deviation = 0;
  const i64 before = steady_ns();
  const VkResult r =
      vkGetCalibratedTimestampsKHR(device_->handles().device, 1, &info, &ticks, &deviation);
  const i64 after = steady_ns();
  if (r != VK_SUCCESS) return false;
  calibration_ticks_[slot] = ticks;
  calibration_ns_[slot] = before + (after - before) / 2;
  return true;
}

i64 Swapchain::to_monotonic_ns(u64 ticks) const noexcept {
  const f64 span_ticks = static_cast<f64>(calibration_ticks_[1] - calibration_ticks_[0]);
  const f64 span_ns = static_cast<f64>(calibration_ns_[1] - calibration_ns_[0]);
  const f64 scale = span_ticks > 0.0 ? span_ns / span_ticks : 1.0;
  const f64 from_last = static_cast<f64>(static_cast<i64>(ticks - calibration_ticks_[1])) * scale;
  return calibration_ns_[1] + static_cast<i64>(from_last);
}

u32 Swapchain::poll_timings(PresentTiming* out, u32 capacity) {
  if (!timing_ || swapchain_ == VK_NULL_HANDLE || capacity == 0) return 0;
  const VkDevice device = device_->handles().device;
  // Refine the calibration every half second; the tick rate does not move, so this only shrinks
  // the error the first 5 ms left.
  if (steady_ns() - calibration_ns_[1] > 500'000'000) (void)calibrate(1);
  const u32 n = capacity < past_.size() ? capacity : past_.size();
  for (u32 i = 0; i < n; ++i) {
    past_[i] = VkPastPresentationTimingEXT{};
    past_[i].sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT;
    past_[i].presentStageCount = 1;
    past_[i].pPresentStages = &past_stages_[i];
  }
  VkPastPresentationTimingInfoEXT info{};
  info.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT;
  info.swapchain = swapchain_;
  VkPastPresentationTimingPropertiesEXT properties{};
  properties.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT;
  properties.presentationTimingCount = n;
  properties.pPresentationTimings = past_.data();
  const VkResult r = vkGetPastPresentationTimingEXT(device, &info, &properties);
  if (r != VK_SUCCESS && r != VK_INCOMPLETE) return 0;
  if (properties.timingPropertiesCounter != timing_properties_counter_) {
    VkSwapchainTimingPropertiesEXT timing{};
    timing.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT;
    u64 counter = 0;
    if (vkGetSwapchainTimingPropertiesEXT(device, swapchain_, &timing, &counter) == VK_SUCCESS) {
      refresh_ns_ = timing.refreshDuration;
      timing_properties_counter_ = counter;
    }
  }
  u32 written = 0;
  const u32 returned =
      properties.presentationTimingCount < n ? properties.presentationTimingCount : n;
  timing_outstanding_ = timing_outstanding_ > returned ? timing_outstanding_ - returned : 0;
  for (u32 i = 0; i < returned; ++i) {
    const VkPastPresentationTimingEXT& t = past_[i];
    if (t.presentStageCount == 0 || t.pPresentStages[0].time == 0) continue;
    if (static_cast<u32>(t.timeDomain) != timing_domain_) continue;  // another domain: skip it
    out[written].id = t.presentId;
    out[written].shown_ns = to_monotonic_ns(t.pPresentStages[0].time);
    ++written;
  }
  return written;
}

bool Swapchain::wait_for_present(u64 id, u64 timeout_ns) {
  // An id from before the chain was recreated was never presented on this one, and a wait for it
  // would last the whole timeout.
  if (!wait_ || swapchain_ == VK_NULL_HANDLE || id < chain_first_id_ || id > present_id_)
    return false;
  VkPresentWait2InfoKHR info{};
  info.sType = VK_STRUCTURE_TYPE_PRESENT_WAIT_2_INFO_KHR;
  info.presentId = id;
  info.timeout = timeout_ns;
  return vkWaitForPresent2KHR(device_->handles().device, swapchain_, &info) == VK_SUCCESS;
}

void Swapchain::destroy_chain() noexcept {
  if (device_ == nullptr) return;
  const Handles& h = device_->handles();
  for (const ImageViewHandle view : views_) {
    if (view) vkDestroyImageView(h.device, vk::native(view), nullptr);
  }
  for (const SemaphoreHandle semaphore : render_finished_) {
    if (semaphore) vkDestroySemaphore(h.device, vk::native(semaphore), nullptr);
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

PresentStatus Swapchain::acquire(SemaphoreHandle signal, u32& image_index, u64 timeout_ns) {
  ENGINE_VERIFY(swapchain_ != VK_NULL_HANDLE, "Swapchain::acquire: not created");
  if (desc_.width == 0 || desc_.height == 0) return PresentStatus::OutOfDate;
  const VkResult r = vkAcquireNextImageKHR(device_->handles().device, swapchain_, timeout_ns,
                                           vk::native(signal), VK_NULL_HANDLE, &image_index);
  if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) return PresentStatus::Ok;
  if (r == VK_ERROR_OUT_OF_DATE_KHR) return PresentStatus::OutOfDate;
  last_error_ = result_name(r);
  return PresentStatus::Error;
}

PresentStatus Swapchain::present(u32 image_index, SemaphoreHandle wait) {
  ENGINE_VERIFY(swapchain_ != VK_NULL_HANDLE, "Swapchain::present: not created");
  const VkSemaphore wait_semaphore = vk::native(wait);
  VkPresentInfoKHR info{};
  info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  info.waitSemaphoreCount = wait ? 1 : 0;
  info.pWaitSemaphores = &wait_semaphore;
  info.swapchainCount = 1;
  info.pSwapchains = &swapchain_;
  info.pImageIndices = &image_index;
  // An id on every present when the chain carries them, and a request for the first-pixel-out
  // time when it has present timing, in the domain setup_timing() chose.
  const u64 id = present_id_ + 1;
  VkPresentId2KHR ids{};
  ids.sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR;
  ids.swapchainCount = 1;
  ids.pPresentIds = &id;
  VkPresentTimingInfoEXT timing{};
  timing.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT;
  timing.timeDomainId = timing_domain_id_;
  timing.presentStageQueries = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
  VkPresentTimingsInfoEXT timings{};
  timings.sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT;
  timings.swapchainCount = 1;
  timings.pTimingInfos = &timing;
  const bool timed = timing_ && timing_outstanding_ < timing_queue_;
  if (ids_) {
    info.pNext = &ids;
    if (timed) ids.pNext = &timings;
  }
  if (timing_) {
    if (timed) {
      ++timing_outstanding_;
    } else {
      ++timing_dropped_;
    }
  }
  VkResult r = vkQueuePresentKHR(device_->handles().graphics_queue, &info);
  if (r == VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT) {
    // The driver's count and ours disagree: present without asking rather than drop the frame.
    ids.pNext = nullptr;
    --timing_outstanding_;
    ++timing_dropped_;
    r = vkQueuePresentKHR(device_->handles().graphics_queue, &info);
  }
  present_id_ = id;
  if (r == VK_SUCCESS) return PresentStatus::Ok;
  if (r == VK_SUBOPTIMAL_KHR) ++suboptimal_;
  if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR) return PresentStatus::OutOfDate;
  last_error_ = result_name(r);
  return PresentStatus::Error;
}

}  // namespace engine::gfx
