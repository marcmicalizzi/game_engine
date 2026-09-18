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
  bool presentation = false;   // VK_KHR_swapchain is enabled
  bool validation = false;     // the validation layer is active
  bool memory_budget = false;  // VK_EXT_memory_budget: Device::memory_budget() reports real
                               // numbers rather than heap sizes alone
};

// How much of the device-local memory this process may have, and how much of it it is using
// (VK_EXT_memory_budget). The development machine shares its GPU with other work — a diffusion
// job can hold most of a 32 GB card — and a frame time measured while another process owns the
// memory is a measurement of the contention, not of the renderer. Reported so a summary can say
// so instead of leaving the reader to guess.
//
// `budget_bytes` is what the *Vulkan* driver will let this process allocate given what it knows
// about, so `device_local_bytes - budget_bytes` is the share it believes the rest of the machine
// holds and `used_bytes` is this process's own. Both are estimates, and the first one is a
// **lower bound**: measured on an RTX 5090 with 17.1 GB of the card held by a CUDA workload, the
// Vulkan budget still reported 31,614 of 32,404 MiB free. Trust it when it is large and read
// nvidia-smi's figure — the machine_state block beside this one — for "is somebody else on this
// GPU". See docs/subsystems/gfx.md.
struct MemoryBudget {
  bool valid = false;          // false without VK_EXT_memory_budget: only device_local_bytes
  u64 budget_bytes = 0;        // what this process may use of the device-local heaps
  u64 used_bytes = 0;          // what this process has allocated of them
  u64 device_local_bytes = 0;  // the heaps' own size, known either way
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

  // Device-local memory: the heaps' size always, and this process's budget and usage when the
  // device has VK_EXT_memory_budget. False with `out` untouched when there is no device. It is
  // a driver query, not a frame path — sample it around a measurement, not inside one.
  bool memory_budget(MemoryBudget& out) const noexcept;

  // Vulkan handles for gfx internals and tests.
  const Handles& handles() const noexcept;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace engine::gfx
