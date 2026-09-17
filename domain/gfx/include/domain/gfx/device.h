#pragma once

// The graphics device (docs/plan/04-renderer.md §4.2, ADR-0006): one Vulkan instance, one
// physical device chosen from enumerate_adapters(), one logical device with the feature chain
// the renderer relies on, its queues, and a memory allocator. This header is API-neutral:
// nothing above the RHI sees a Vulkan handle. gfx internals and the RHI's own tests reach the
// handles through domain/gfx/vulkan.h.
//
// Required features (creation fails without them): Vulkan 1.3 dynamic rendering,
// synchronization2, maintenance4; 1.2 buffer device address, descriptor indexing, timeline
// semaphores, scalar block layout, host query reset, draw indirect count. Optional features are
// enabled when the device offers them and reported in DeviceFeatures so systems can choose a
// path once rather than probing.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <domain/gfx/adapter.h>

#include <string>

namespace engine::gfx {

struct DeviceOptions {
  // Index into the sorted enumerate_adapters() list.
  u32 adapter_index = 0;
  // VK_LAYER_KHRONOS_validation when it is installed (Vulkan SDK); ignored otherwise with a
  // warning in the log.
  bool validation = false;
  // Route validation and driver messages through the log when VK_EXT_debug_utils exists.
  bool debug_messenger = true;
  // Instance extensions a window system needs for its surface
  // (window::Window::vulkan_instance_extensions()); creation fails when one is missing.
  const char* const* instance_extensions = nullptr;
  u32 instance_extension_count = 0;
};

// What the created device has enabled, beyond the required core features.
struct DeviceFeatures {
  bool mesh_shader = false;
  bool acceleration_structure = false;
  bool ray_tracing_pipeline = false;
  bool ray_query = false;
  bool cluster_acceleration_structure = false;
  bool descriptor_buffer = false;
  bool memory_decompression = false;
  bool shader_int64 = false;
  bool buffer_int64_atomics = false;  // 64-bit atomics on storage buffers (visibility buffer)
  bool sampler_anisotropy = false;
  bool presentation = false;  // VK_KHR_swapchain is enabled
  bool validation = false;    // the validation layer is active
};

struct Handles;  // Vulkan handles; see domain/gfx/vulkan.h

class Device {
 public:
  Device() noexcept = default;
  ~Device();
  ENGINE_NON_COPYABLE(Device);

  // Creates instance, logical device, queues, and allocator. False with `error` when there is
  // no loader, no driver, no suitable adapter, or a required feature is missing.
  bool create(const DeviceOptions& options, std::string* error = nullptr);
  void destroy() noexcept;
  bool valid() const noexcept { return impl_ != nullptr; }

  const AdapterInfo& adapter() const noexcept;
  const DeviceFeatures& features() const noexcept;
  // Family indices; compute and transfer may equal graphics on devices without dedicated
  // queues.
  u32 graphics_family() const noexcept;
  u32 compute_family() const noexcept;
  u32 transfer_family() const noexcept;

  void wait_idle() noexcept;

  // Vulkan handles for gfx internals and tests.
  const Handles& handles() const noexcept;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace engine::gfx
