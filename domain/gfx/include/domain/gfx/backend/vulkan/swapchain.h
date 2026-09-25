#pragma once

// Presentation (docs/plan/04-renderer.md §4.8): a swapchain over a window surface. The window
// system creates the surface (foundation/window/backend/vulkan/surface.h); this class owns the
// swapchain, its image views, and one render-finished semaphore per image. Acquire semaphores
// belong to the frame context's slots (FrameContext::acquire_semaphore()), which is the pairing
// that keeps every binary semaphore unsignaled before reuse.
//
// **This is a backend header** (docs/subsystems/gfx.md, "The RHI surface and the backend
// surface"): a swapchain is made from a Vulkan surface, and presenting is the one thing a
// presenting app does that has no engine-neutral form yet, so the class lives in the backend set
// and only a module that presents includes it (apps/engine_view). What it hands the rest of the
// engine is engine-typed — its images are `ImageResource`s, its semaphores `SemaphoreHandle`s,
// its format a `Format` — so the renderer draws into a swapchain image without knowing it is one.
// A second backend brings a swapchain of its own beside this one.
//
//     Swapchain swapchain;
//     swapchain.create(device, {.surface = surface, .width = w, .height = h});
//     for (;;) {
//       CommandList commands = frames.begin_frame();
//       u32 image = 0;
//       if (swapchain.acquire(frames.acquire_semaphore(), image) != PresentStatus::Ok) { resize...
//       }
//       ...record, ending with the image in ImageLayout::Present...
//       frames.end_frame({.wait = frames.acquire_semaphore(),
//                         .signal = swapchain.render_finished(image)});
//       swapchain.present(image, swapchain.render_finished(image));
//     }

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/device.h>
#include <domain/gfx/resources.h>
#include <domain/gfx/rhi.h>

#include <string>

namespace engine::gfx {

struct SwapchainDesc {
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  u32 width = 0;  // the window's current pixel size
  u32 height = 0;
  // FIFO when true. Otherwise MAILBOX when the surface offers it, then IMMEDIATE, then FIFO.
  bool vsync = true;
  // Request ImageUsage::TransferSrc so captures can read the presented image; granted when the
  // surface supports it (transfer_src() reports the outcome).
  bool transfer_src = true;
  Format preferred_format = Format::B8G8R8A8Unorm;
  u32 min_image_count = 3;
};

enum class PresentStatus : u8 {
  Ok,
  // The surface changed (resize, minimize): recreate with resize() and skip this frame. After
  // acquire() nothing was acquired; after present() the frame was still consumed.
  OutOfDate,
  Error,
};

class Swapchain {
 public:
  Swapchain() noexcept = default;
  ~Swapchain();
  ENGINE_NON_COPYABLE(Swapchain);

  // Fails when the surface cannot present from the graphics queue or the device was created
  // without VK_KHR_swapchain (DeviceFeatures::presentation).
  bool create(const Device& device, const SwapchainDesc& desc, std::string* error = nullptr);
  void destroy() noexcept;
  bool valid() const noexcept { return swapchain_ != VK_NULL_HANDLE; }
  // Recreates for a new pixel size (a zero size, as when minimized, keeps the old chain and
  // reports true; acquire() keeps returning OutOfDate until a real size arrives). Waits for the
  // device to go idle first.
  bool resize(u32 width, u32 height, std::string* error = nullptr);

  // Acquires the next image, signaling `signal` when it may be written.
  PresentStatus acquire(SemaphoreHandle signal, u32& image_index, u64 timeout_ns = ~u64{0});
  // Queues `image_index` for presentation once `wait` is signaled.
  PresentStatus present(u32 image_index, SemaphoreHandle wait);

  u32 image_count() const noexcept { return images_.size(); }
  const ImageResource& image(u32 index) const noexcept { return images_[index]; }
  ImageViewHandle view(u32 index) const noexcept { return views_[index]; }
  SemaphoreHandle render_finished(u32 image_index) const noexcept {
    return render_finished_[image_index];
  }
  Format format() const noexcept { return format_; }
  Extent2D extent() const noexcept { return extent_; }
  VkPresentModeKHR present_mode() const noexcept { return mode_; }
  bool transfer_src() const noexcept { return transfer_src_; }
  VkSwapchainKHR handle() const noexcept { return swapchain_; }
  const char* last_error() const noexcept { return last_error_; }

 private:
  bool create_chain(std::string* error);
  void destroy_chain() noexcept;

  const Device* device_ = nullptr;
  SwapchainDesc desc_{};
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  Vector<ImageResource> images_;  // not owned by VMA: image + format + extent only
  Vector<ImageViewHandle> views_;
  Vector<SemaphoreHandle> render_finished_;
  Format format_ = Format::Undefined;
  VkColorSpaceKHR color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  Extent2D extent_{};
  VkPresentModeKHR mode_ = VK_PRESENT_MODE_FIFO_KHR;
  bool transfer_src_ = false;
  const char* last_error_ = "";
};

}  // namespace engine::gfx
