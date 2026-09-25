#pragma once

// The Vulkan backend's header set (docs/subsystems/gfx.md, "The RHI surface and the backend
// surface"): what a module that owns a device, a swapchain or a surface has to see, and nothing
// the rest of the engine needs. `domain/gfx`'s own sources and tests include it, and so does
// `apps/engine_view`, which presents; `systems/renderer` does not and must not — it records into
// an image the caller names through the engine's own vocabulary (rhi.h, commands.h), which is
// what lets a second backend run it unchanged. `tools/lint.ps1` enforces both halves: no Vulkan
// in a public header outside `backend/vulkan/` (`vulkan-in-public-header`), and no include of
// this directory, `<volk.h>` or `<vulkan/...>` outside the modules that own a device, a swapchain
// or a surface (`vulkan-backend-include`). The build enforces the second as well: the Vulkan,
// volk and VMA include paths reach only the targets that link `engine::gfx_vulkan`.
//
// What is here: the Vulkan handles behind a `Device` (`Handles`), the device-capability query,
// the one hand-written barrier the RHI's own code records, and the conversions between the
// engine's handles and enumerations and Vulkan's (`vk::native`, `vk::wrap`), which are casts
// that src/rhi_vulkan.cpp proves value for value.

#include <core/base/types.h>
#include <domain/gfx/commands.h>
#include <domain/gfx/device.h>
#include <domain/gfx/pipeline.h>
#include <domain/gfx/requirements.h>
#include <domain/gfx/resources.h>
#include <domain/gfx/rhi.h>

#include <cstdint>
#include <cstring>

// clang-format off: the VMA configuration macros must precede its header.
#include <volk.h>
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>
// clang-format on

namespace engine::gfx {

struct Handles {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue graphics_queue = VK_NULL_HANDLE;
  VkQueue compute_queue = VK_NULL_HANDLE;   // may equal graphics_queue
  VkQueue transfer_queue = VK_NULL_HANDLE;  // may equal graphics_queue
  VmaAllocator allocator = nullptr;
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
  VkCommandPool immediate_pool = VK_NULL_HANDLE;  // graphics family, transient
  // Presentation extras the device enabled beside VK_KHR_swapchain, which only a presenting
  // module reads (the swapchain, backend/vulkan/swapchain.h), so they are here rather than in the
  // engine-neutral DeviceFeatures: present ids and waits (VK_KHR_present_id2,
  // VK_KHR_present_wait2) and display timing (VK_EXT_present_timing with the calibrated
  // timestamps it needs). Enabled only when the instance brought surface extensions.
  bool present_id2 = false;
  bool present_wait2 = false;
  bool present_timing = false;
};

const char* result_name(VkResult result) noexcept;

// Every property, feature and extension the requirements table reads, in one query
// (domain/gfx/requirements.h). This is the only place that touches Vulkan for them, so
// `enumerate_adapters` and `Device::create` check a device against exactly the same numbers.
void read_device_caps(VkPhysicalDevice physical, DeviceCaps& out);

// synchronization2 image layout transition on the whole color aspect.
void image_barrier(VkCommandBuffer commands, VkImage image, VkImageLayout old_layout,
                   VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                   VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                   VkAccessFlags2 dst_access) noexcept;

// ---- the engine's vocabulary in Vulkan's terms
// ----------------------------------------------------
//
// `native(x)` is the Vulkan object or value behind an engine one and `wrap(x)` the way back. A
// handle is the Vulkan handle's bits (every non-dispatchable handle is a pointer on the 64-bit
// targets the engine builds for, and so are the dispatchable ones), and an enumeration is the
// same number (rhi.h), so every one of these is a cast.
namespace vk {

static_assert(sizeof(void*) == 8, "handles carry a Vulkan handle's bits: 64-bit targets only");

#define ENGINE_GFX_VK_HANDLE(Engine, Native)                                     \
  inline Native native(Engine handle) noexcept {                                 \
    return reinterpret_cast<Native>(static_cast<std::uintptr_t>(handle.bits())); \
  }                                                                              \
  inline Engine wrap(Native handle) noexcept {                                   \
    return Engine{static_cast<u64>(reinterpret_cast<std::uintptr_t>(handle))};   \
  }

ENGINE_GFX_VK_HANDLE(BufferHandle, VkBuffer)
ENGINE_GFX_VK_HANDLE(ImageHandle, VkImage)
ENGINE_GFX_VK_HANDLE(ImageViewHandle, VkImageView)
ENGINE_GFX_VK_HANDLE(SamplerHandle, VkSampler)
ENGINE_GFX_VK_HANDLE(AccelerationStructureHandle, VkAccelerationStructureKHR)
ENGINE_GFX_VK_HANDLE(AllocationHandle, VmaAllocation)
ENGINE_GFX_VK_HANDLE(ShaderModuleHandle, VkShaderModule)
ENGINE_GFX_VK_HANDLE(PipelineHandle, VkPipeline)
ENGINE_GFX_VK_HANDLE(PipelineLayoutHandle, VkPipelineLayout)
ENGINE_GFX_VK_HANDLE(DescriptorSetLayoutHandle, VkDescriptorSetLayout)
ENGINE_GFX_VK_HANDLE(DescriptorSetHandle, VkDescriptorSet)
ENGINE_GFX_VK_HANDLE(DescriptorPoolHandle, VkDescriptorPool)
ENGINE_GFX_VK_HANDLE(CommandListHandle, VkCommandBuffer)
ENGINE_GFX_VK_HANDLE(CommandPoolHandle, VkCommandPool)
ENGINE_GFX_VK_HANDLE(SemaphoreHandle, VkSemaphore)
ENGINE_GFX_VK_HANDLE(QueryPoolHandle, VkQueryPool)

#undef ENGINE_GFX_VK_HANDLE

inline VkCommandBuffer native(CommandList commands) noexcept { return native(commands.handle()); }
inline CommandList command_list(VkCommandBuffer commands) noexcept {
  return CommandList{wrap(commands)};
}

// Enumerations: the engine's numbers are Vulkan's (src/rhi_vulkan.cpp checks every enumerator).
inline VkFormat native(Format format) noexcept { return static_cast<VkFormat>(format); }
inline Format wrap(VkFormat format) noexcept { return static_cast<Format>(format); }
inline VkImageLayout native(ImageLayout layout) noexcept {
  return static_cast<VkImageLayout>(layout);
}
inline ImageLayout wrap(VkImageLayout layout) noexcept { return static_cast<ImageLayout>(layout); }
inline VkPipelineBindPoint native(BindPoint point) noexcept {
  return static_cast<VkPipelineBindPoint>(point);
}
inline VkIndexType native(IndexType type) noexcept { return static_cast<VkIndexType>(type); }
inline VkAttachmentLoadOp native(LoadOp op) noexcept { return static_cast<VkAttachmentLoadOp>(op); }
inline VkCompareOp native(CompareOp op) noexcept { return static_cast<VkCompareOp>(op); }
inline VkFilter native(Filter filter) noexcept { return static_cast<VkFilter>(filter); }
inline VkSamplerMipmapMode native(SamplerMipmapMode mode) noexcept {
  return static_cast<VkSamplerMipmapMode>(mode);
}
inline VkSamplerAddressMode native(SamplerAddressMode mode) noexcept {
  return static_cast<VkSamplerAddressMode>(mode);
}
inline VkDescriptorType native(DescriptorType type) noexcept {
  return static_cast<VkDescriptorType>(type);
}
inline DescriptorType wrap(VkDescriptorType type) noexcept {
  return static_cast<DescriptorType>(type);
}
// Flags: Vulkan spells every 32-bit set VkFlags and every 64-bit one VkFlags64, so these convert
// one way only, and each has a name of its own where the way back is wanted.
inline VkBufferUsageFlags native(BufferUsage usage) noexcept {
  return static_cast<VkBufferUsageFlags>(usage);
}
inline VkImageUsageFlags native(ImageUsage usage) noexcept {
  return static_cast<VkImageUsageFlags>(usage);
}
inline VkShaderStageFlags native(ShaderStage stages) noexcept {
  return static_cast<VkShaderStageFlags>(stages);
}
inline ShaderStage shader_stage(VkShaderStageFlags stages) noexcept {
  return static_cast<ShaderStage>(stages);
}
inline VkCullModeFlags native(CullMode mode) noexcept { return static_cast<VkCullModeFlags>(mode); }
inline VkPipelineStageFlags2 native(PipelineStage stages) noexcept {
  return static_cast<VkPipelineStageFlags2>(stages);
}
inline PipelineStage pipeline_stage(VkPipelineStageFlags2 stages) noexcept {
  return static_cast<PipelineStage>(stages);
}
inline VkAccessFlags2 native(MemoryAccess access) noexcept {
  return static_cast<VkAccessFlags2>(access);
}
inline MemoryAccess memory_access(VkAccessFlags2 access) noexcept {
  return static_cast<MemoryAccess>(access);
}
inline VkBuildAccelerationStructureFlagsKHR native(AccelerationBuildFlags flags) noexcept {
  return static_cast<VkBuildAccelerationStructureFlagsKHR>(flags);
}
inline VkGeometryInstanceFlagsKHR native(GeometryInstanceFlags flags) noexcept {
  return static_cast<VkGeometryInstanceFlagsKHR>(flags);
}

// Small descriptions with Vulkan's layout (checked member for member in src/rhi_vulkan.cpp).
inline VkExtent2D native(Extent2D extent) noexcept { return {extent.width, extent.height}; }
inline Extent2D wrap(VkExtent2D extent) noexcept { return {extent.width, extent.height}; }
inline const VkBufferCopy* native(const BufferCopy* regions) noexcept {
  return reinterpret_cast<const VkBufferCopy*>(regions);
}
inline VkClearColorValue native(const ClearColor& color) noexcept {
  VkClearColorValue value;
  std::memcpy(&value, &color, sizeof(value));
  return value;
}

}  // namespace vk

}  // namespace engine::gfx
