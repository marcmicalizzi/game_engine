#pragma once

// The Vulkan-facing surface of gfx: handles, VMA-backed resources, barriers, and one-shot
// submission. Included by gfx internals, by the render graph, and by the RHI's tests. Nothing
// above the RHI includes this header.

#include <core/base/types.h>
#include <domain/gfx/device.h>

#include <span>
#include <string>

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
};

const char* result_name(VkResult result) noexcept;

// ---- resources -------------------------------------------------------------------------------

struct BufferResource {
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  void* mapped = nullptr;  // persistent mapping when host visible
  u64 size = 0;
  VkDeviceAddress address = 0;  // when created with VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
};

struct ImageResource {
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  VkFormat format = VK_FORMAT_UNDEFINED;
  u32 width = 0;
  u32 height = 0;
};

// host_visible buffers are persistently mapped and host-coherent (upload and readback);
// otherwise device-local.
bool create_buffer(const Device& device, u64 size, VkBufferUsageFlags usage, bool host_visible,
                   BufferResource& out, std::string* error = nullptr);
void destroy_buffer(const Device& device, BufferResource& buffer) noexcept;
// Creates a device-local buffer with `usage` plus TRANSFER_DST and SHADER_DEVICE_ADDRESS, fills it
// from `data` through a staging buffer, and waits for the copy. For setup-time uploads.
bool upload_buffer(const Device& device, const void* data, u64 bytes, VkBufferUsageFlags usage,
                   BufferResource& out, std::string* error = nullptr);
// Creates a sampled 2D image, fills it from tightly packed `pixels` through a staging buffer, and
// leaves it in SHADER_READ_ONLY_OPTIMAL. For setup-time textures.
bool upload_image_2d(const Device& device, u32 width, u32 height, VkFormat format,
                     const void* pixels, u64 bytes, ImageResource& out,
                     std::string* error = nullptr);

bool create_image_2d(const Device& device, u32 width, u32 height, VkFormat format,
                     VkImageUsageFlags usage, ImageResource& out, std::string* error = nullptr);
void destroy_image(const Device& device, ImageResource& image) noexcept;

// ---- commands --------------------------------------------------------------------------------

using RecordFn = void (*)(VkCommandBuffer commands, void* context);

// Records with `record`, submits on the graphics queue, and waits for completion. For uploads,
// readbacks, tests, and tools; the render graph owns per-frame submission.
bool submit_immediate(const Device& device, RecordFn record, void* context,
                      std::string* error = nullptr);

template <class F>
bool submit_immediate(const Device& device, F&& record, std::string* error = nullptr) {
  return submit_immediate(
      device,
      [](VkCommandBuffer commands, void* context) { (*static_cast<F*>(context))(commands); },
      &record, error);
}

// ---- shaders and pipelines ----------------------------------------------------------------------

// SPIR-V bytes (4-byte aligned, as the embedded headers provide). VK_NULL_HANDLE on failure.
VkShaderModule create_shader_module(const Device& device, const unsigned char* spirv, usize bytes,
                                    std::string* error = nullptr);
void destroy_shader_module(const Device& device, VkShaderModule module) noexcept;

struct ComputePipeline {
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
};

// One compute entry point over the given descriptor set layouts and a push-constant block of
// `push_constant_bytes` (0 for none).
bool create_compute_pipeline(const Device& device, VkShaderModule module, const char* entry,
                             std::span<const VkDescriptorSetLayout> set_layouts,
                             u32 push_constant_bytes, ComputePipeline& out,
                             std::string* error = nullptr);
void destroy_compute_pipeline(const Device& device, ComputePipeline& pipeline) noexcept;

// A graphics pipeline for dynamic rendering: no vertex input (geometry is pulled through device
// addresses or generated), dynamic viewport and scissor, one color attachment, optional depth.
struct GraphicsPipelineDesc {
  VkShaderModule vertex = VK_NULL_HANDLE;
  const char* vertex_entry = "vs_main";
  VkShaderModule fragment = VK_NULL_HANDLE;
  const char* fragment_entry = "fs_main";
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkFormat color_format = VK_FORMAT_UNDEFINED;
  VkFormat depth_format = VK_FORMAT_UNDEFINED;
  VkCullModeFlags cull = VK_CULL_MODE_NONE;
  bool depth_test = false;
  bool depth_write = false;
  // Reversed-Z: greater-or-equal passes (core/math projections produce near = 1).
  VkCompareOp depth_compare = VK_COMPARE_OP_GREATER_OR_EQUAL;
};
bool create_graphics_pipeline(const Device& device, const GraphicsPipelineDesc& desc,
                              VkPipeline& out, std::string* error = nullptr);
void destroy_pipeline(const Device& device, VkPipeline pipeline) noexcept;

// A mesh-shader pipeline: optional task stage, mesh stage, fragment stage; no vertex input or
// input assembly state exists for these. Requires DeviceFeatures::mesh_shader.
struct MeshPipelineDesc {
  VkShaderModule task = VK_NULL_HANDLE;  // optional
  const char* task_entry = "task_main";
  VkShaderModule mesh = VK_NULL_HANDLE;
  const char* mesh_entry = "mesh_main";
  VkShaderModule fragment = VK_NULL_HANDLE;
  const char* fragment_entry = "fs_main";
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkFormat color_format = VK_FORMAT_UNDEFINED;
  VkFormat depth_format = VK_FORMAT_UNDEFINED;
  VkCullModeFlags cull = VK_CULL_MODE_NONE;
  bool depth_test = false;
  bool depth_write = false;
  VkCompareOp depth_compare = VK_COMPARE_OP_GREATER_OR_EQUAL;
};
bool create_mesh_pipeline(const Device& device, const MeshPipelineDesc& desc, VkPipeline& out,
                          std::string* error = nullptr);

// synchronization2 image layout transition on the whole color aspect.
void image_barrier(VkCommandBuffer commands, VkImage image, VkImageLayout old_layout,
                   VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                   VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                   VkAccessFlags2 dst_access) noexcept;

}  // namespace engine::gfx
