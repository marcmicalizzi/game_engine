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
#include <domain/gfx/display.h>
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
  // A present mode asked for by name (engine-view's `--present`), overriding `vsync`: taken when
  // the surface offers it, FIFO (which every surface offers) when it does not — present_mode()
  // says which. VK_PRESENT_MODE_MAX_ENUM_KHR: none asked for.
  VkPresentModeKHR present_mode = VK_PRESENT_MODE_MAX_ENUM_KHR;
  // Request ImageUsage::TransferSrc so captures can read the presented image; granted when the
  // surface supports it (transfer_src() reports the outcome).
  bool transfer_src = true;
  // **The colour's depth** (display.h; ADR-0052). Every format is taken in the sRGB non-linear
  // colour space and as UNORM, never `_SRGB`: the picture arrives encoded by the renderer's own
  // output encode, which an `_SRGB` store would encode a second time. 10 takes A2B10G10R10, then
  // A2R10G10B10, where the surface offers one, and the 8-bit choice where it offers neither; 8
  // takes `preferred_format`, then B8G8R8A8 and R8G8B8A8. A surface that offers none of them gets
  // its first format, as before. format() and offers_ten_bit() say what happened.
  u32 color_bits = 8;
  Format preferred_format = Format::B8G8R8A8Unorm;
  // **The present format** (display.h, E39). `Auto` leaves the choice to `color_bits` above, as
  // every chain before it; `Sdr8` and `Sdr10` are `color_bits` 8 and 10. `Hdr10` takes
  // A2B10G10R10 (then A2R10G10B10) in HDR10_ST2084 and `ScRgb` R16G16B16A16Sfloat in
  // EXTENDED_SRGB_LINEAR, exactly: a surface that does not offer it fails create() with a sentence
  // naming what it does offer (`describe_surface_formats`), never a fallback to an SDR chain the
  // picture's PQ codes would be shown through. Those colour spaces are listed only on an instance
  // with VK_EXT_swapchain_colorspace, which a presenting device enables where the loader has it.
  PresentFormat present_format = PresentFormat::Auto;
  // Clamped to the surface's range; image_count() is what the driver created, which may be more.
  u32 min_image_count = 3;
  // Carry an id on every present and allow waiting for one to be shown (VK_KHR_present_id2 and
  // VK_KHR_present_wait2, when the device enabled them and the surface supports them;
  // present_wait() reports the outcome). What a pacer that samples on the display's clock needs.
  bool present_wait = false;
  // Ask for each present's display time (VK_EXT_present_timing, likewise; present_timing()
  // reports the outcome). Off by default: nothing but a measurement reads it, reading it is not
  // free (poll_timings), and on NVIDIA's Windows driver a chain created with it paces FIFO
  // differently from one without (docs/experiments/second-interactive-session-2026-09-25.md), so
  // it is asked for only by a run that measures.
  bool timing = false;
};

enum class PresentStatus : u8 {
  Ok,
  // The surface changed (resize, minimize): recreate with resize() and skip this frame. After
  // acquire() nothing was acquired; after present() the frame was still consumed.
  OutOfDate,
  Error,
};

// When a present reached the display (VK_EXT_present_timing): the id present() returned, and the
// moment its first pixel went out, on time::monotonic_ns()'s clock.
struct PresentTiming {
  u64 id = 0;
  i64 shown_ns = 0;
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
  // Queues `image_index` for presentation once `wait` is signaled. VK_SUBOPTIMAL_KHR reports
  // OutOfDate like VK_ERROR_OUT_OF_DATE_KHR (the frame was still consumed), and
  // suboptimal_presents() counts it.
  PresentStatus present(u32 image_index, SemaphoreHandle wait);
  // The id the last present() carried (1, 2, 3, ... over the swapchain's life, recreations
  // included); what poll_timings() reports against. 0 before the first.
  u64 last_present_id() const noexcept { return present_id_; }

  // ---- when frames reached the display (VK_EXT_present_timing) ----
  // True when the chain was created with `SwapchainDesc::timing` on a device and surface that
  // can say when a present's first pixel went out.
  bool present_timing() const noexcept { return timing_; }
  // The display's refresh period the driver reports, nanoseconds; 0 when it cannot say (no
  // present timing, or a variable-refresh display with no fixed period).
  u64 refresh_ns() const noexcept { return refresh_ns_; }
  // The presents whose display time has come back since the last call, at most `capacity` (the
  // rest wait for the next call). Returns how many were written. **Not a frame-loop call**: on
  // NVIDIA's Windows driver it blocks for about a refresh (swapchain.cpp says why), so the
  // driver's queue is deep enough for a run to be read once, at its end; past that depth present()
  // stops asking and timing_dropped() counts what went unmeasured.
  u32 poll_timings(PresentTiming* out, u32 capacity);
  u32 timing_dropped() const noexcept { return timing_dropped_; }
  // True when the chain carries present ids and can wait on them (`SwapchainDesc::present_wait`).
  bool present_wait() const noexcept { return wait_; }
  // Blocks until the present `id` has been shown (or a later one), or `timeout_ns` passes.
  // True when it was shown; false on timeout, on an id never presented, or without present_wait().
  bool wait_for_present(u64 id, u64 timeout_ns);

  u32 image_count() const noexcept { return images_.size(); }
  const ImageResource& image(u32 index) const noexcept { return images_[index]; }
  ImageViewHandle view(u32 index) const noexcept { return views_[index]; }
  SemaphoreHandle render_finished(u32 image_index) const noexcept {
    return render_finished_[image_index];
  }
  Format format() const noexcept { return format_; }
  VkColorSpaceKHR color_space() const noexcept { return color_space_; }
  // The surface offers a 10-bit UNORM format in the sRGB non-linear colour space: what `auto`
  // would have taken, whatever `color_bits` asked for.
  bool offers_ten_bit() const noexcept { return ten_bit_offered_; }
  // Every format and colour space the surface offered when the chain was last made (E39's first
  // measurement for this window; display_probe.h asks the same of every output).
  const Vector<SurfaceFormat>& offered() const noexcept { return offered_; }
  // The chain's colour space in the engine's vocabulary.
  ColorSpace surface_color_space() const noexcept { return vk::wrap(color_space_); }

  // ---- HDR10 static metadata (VK_EXT_hdr_metadata) ----
  // Hands the display the mastering primaries and luminances and the content's light levels, and
  // hands them again whenever the chain is recreated. False when the device has no
  // VK_EXT_hdr_metadata (the chain presents without metadata) or there is no chain.
  bool set_hdr_metadata(const HdrMetadata& metadata);
  bool hdr_metadata_supported() const noexcept {
    return device_ != nullptr && device_->handles().hdr_metadata;
  }
  Extent2D extent() const noexcept { return extent_; }
  VkPresentModeKHR present_mode() const noexcept { return mode_; }
  bool transfer_src() const noexcept { return transfer_src_; }
  VkSwapchainKHR handle() const noexcept { return swapchain_; }
  const char* last_error() const noexcept { return last_error_; }
  // Presents that came back VK_SUBOPTIMAL_KHR, and chains created (the first included), over
  // this object's life.
  u32 suboptimal_presents() const noexcept { return suboptimal_; }
  u32 chains_created() const noexcept { return chains_; }

 private:
  bool create_chain(std::string* error);
  void destroy_chain() noexcept;
  void setup_timing() noexcept;
  bool calibrate(u32 slot) noexcept;
  i64 to_monotonic_ns(u64 ticks) const noexcept;

  const Device* device_ = nullptr;
  SwapchainDesc desc_{};
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  Vector<ImageResource> images_;  // not owned by VMA: image + format + extent only
  Vector<ImageViewHandle> views_;
  Vector<SemaphoreHandle> render_finished_;
  Format format_ = Format::Undefined;
  VkColorSpaceKHR color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  bool ten_bit_offered_ = false;
  Vector<SurfaceFormat> offered_;
  HdrMetadata metadata_{};
  bool has_metadata_ = false;
  void apply_hdr_metadata() noexcept;
  Extent2D extent_{};
  VkPresentModeKHR mode_ = VK_PRESENT_MODE_FIFO_KHR;
  bool transfer_src_ = false;
  const char* last_error_ = "";
  u32 suboptimal_ = 0;
  u32 chains_ = 0;
  u64 present_id_ = 0;
  u64 chain_first_id_ = 1;  // the first id presented on the current chain
  // Present timing: whether this chain has it (and present ids and waits), the refresh the driver
  // reports, the time domain the reports come in, and the reports' scratch space.
  bool timing_ = false;
  bool ids_ = false;
  bool wait_ = false;
  u64 refresh_ns_ = 0;
  u64 timing_properties_counter_ = ~u64{0};
  u32 timing_queue_ = 0;        // the driver's queue depth, as set
  u32 timing_outstanding_ = 0;  // presents asked for a time and not read back yet
  u32 timing_dropped_ = 0;      // presents not asked, because the queue was full
  // Two-point calibration of the report domain against time::monotonic_ns(): the first sample
  // and the latest, a tick count and the clock reading taken around it.
  u64 calibration_ticks_[2] = {};
  i64 calibration_ns_[2] = {};
  u32 timing_domain_ = 0;  // the VkTimeDomainKHR the calibration was taken for
  u64 timing_domain_id_ = 0;
  Vector<VkPastPresentationTimingEXT> past_;
  Vector<VkPresentStageTimeEXT> past_stages_;
};

}  // namespace engine::gfx
