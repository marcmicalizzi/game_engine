#pragma once

// The graphics device (docs/plan/04-renderer.md §4.2, ADR-0006): one Vulkan instance, one
// physical device chosen from enumerate_adapters(), one logical device with the feature chain
// the renderer relies on, its queues, and a memory allocator. This header is API-neutral:
// nothing above the RHI sees a Vulkan handle. gfx internals and the RHI's own tests reach the
// handles through domain/gfx/vulkan.h.
//
// What creation requires, and what it merely enables, is **one table** in
// domain/gfx/requirements.h: API version, core features, extensions, and the limits the renderer
// depends on, each with the reason it is there. `create()` walks that table and refuses a device
// whose Required rows fail, naming them; `enumerate_adapters()` walks the same table and puts
// the result in `AdapterInfo::requirements` and `AdapterInfo::verdict`, so a report from a
// machine nobody here owns says exactly what creation would have said. Optional rows are enabled
// when the device offers them and reported in `DeviceFeatures` so systems choose a path once
// rather than probing.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/gfx/adapter.h>
#include <domain/gfx/requirements.h>

#include <span>
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
  // Report this device as a weaker one (domain/gfx/requirements.h). The overridden caps are what
  // the requirements are checked against, what the bindless set is clamped to, *and* what the
  // enabled feature chain is built from — so a profile that removes mesh shaders creates a
  // device that really has none. This is how a machine with an RTX 5090 in it exercises the
  // refusal a 2014 Kepler laptop would get and the clamping a Surface Pro would get; nothing in
  // the engine sets it.
  DeviceOverrides overrides;
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
  bool texture_compression_bc = false;  // BC1-BC7 formats: the content build's built textures
  bool geometry_shader = false;  // SV_PrimitiveID in a vertex pipeline: the indexed vertex path
  bool full_draw_index_uint32 = false;  // index values past 2^24 - 1: the indexed vertex path
  bool presentation = false;            // VK_KHR_swapchain is enabled
  bool validation = false;              // the validation layer is active
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
  // no loader, no driver, no suitable adapter, or a Required row of the requirements table
  // fails — in which case `error` names the rows and `verdict()` holds all of them in full,
  // because "a required Vulkan 1.2/1.3 feature is missing" is not an answer anyone can act on.
  bool create(const DeviceOptions& options, std::string* error = nullptr);
  void destroy() noexcept;
  bool valid() const noexcept { return impl_ != nullptr; }

  const AdapterInfo& adapter() const noexcept;
  const DeviceFeatures& features() const noexcept;
  // What the requirements were checked against: the device's own properties, with
  // DeviceOptions::overrides applied. The bindless set clamps itself to these too, so an
  // override reaches every consumer rather than only the report.
  const DeviceCaps& caps() const noexcept;
  // Every row of the table for this device, in table order.
  std::span<const DeviceRequirement> requirements() const noexcept;
  // Tier, what blocks it, what is degraded, and what the bindless set was clamped to.
  const DeviceVerdict& verdict() const noexcept;
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
  // Outside Impl on purpose: a `create()` that refused the device destroys Impl on the way out,
  // and the answer to "why did it refuse" has to outlive that.
  DeviceCaps caps_;
  Vector<DeviceRequirement> requirements_;
  DeviceVerdict verdict_;
};

}  // namespace engine::gfx
