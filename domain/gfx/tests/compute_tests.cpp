// The shader toolchain end to end: slangc at build time, SPIR-V embedded in a header, a shader
// module, a descriptor set, a compute pipeline, a dispatch, and a readback.
#include "raster_path.h"

#include <domain/gfx/device.h>
#include <domain/gfx/vulkan.h>

#include <doctest/doctest.h>

#include <cstring>
#include <shaders/fill.spv.h>
#include <string>

using namespace engine;

namespace {

struct FillParams {
  u32 count;
  u32 multiplier;
  u32 offset;
};

}  // namespace

TEST_CASE("gfx: a Slang compute shader fills a buffer") {
  static_assert(shaders::k_fill_spirv_size > 0 && shaders::k_fill_spirv_size % 4 == 0);
  static_assert(sizeof(shaders::k_fill_spirv) == shaders::k_fill_spirv_size);
  // SPIR-V magic number, little endian.
  CHECK(shaders::k_fill_spirv[0] == 0x03);
  CHECK(shaders::k_fill_spirv[1] == 0x02);
  CHECK(shaders::k_fill_spirv[2] == 0x23);
  CHECK(shaders::k_fill_spirv[3] == 0x07);

  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  const gfx::Handles& h = device.handles();

  VkShaderModule module =
      gfx::create_shader_module(device, shaders::k_fill_spirv, shaders::k_fill_spirv_size, &error);
  REQUIRE_MESSAGE(module != VK_NULL_HANDLE, error);

  // Descriptor set layout: one storage buffer at binding 0.
  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo layout_info{};
  layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layout_info.bindingCount = 1;
  layout_info.pBindings = &binding;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  REQUIRE(vkCreateDescriptorSetLayout(h.device, &layout_info, nullptr, &set_layout) == VK_SUCCESS);

  gfx::ComputePipeline pipeline;
  const VkDescriptorSetLayout layouts[] = {set_layout};
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, module, "fill", layouts, sizeof(FillParams),
                                               pipeline, &error),
                  error);

  constexpr u32 k_count = 1000;  // not a multiple of the 64-wide group: the tail must be masked
  const u64 bytes = u64{k_count} * sizeof(u32);
  gfx::BufferResource storage;
  gfx::BufferResource readback;
  REQUIRE(gfx::create_buffer(device, bytes + 64,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             /*host_visible=*/false, storage, &error));
  REQUIRE(gfx::create_buffer(device, bytes + 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             /*host_visible=*/true, readback, &error));
  std::memset(readback.mapped, 0xEE, static_cast<usize>(bytes + 64));

  VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  VkDescriptorPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  REQUIRE(vkCreateDescriptorPool(h.device, &pool_info, nullptr, &pool) == VK_SUCCESS);
  VkDescriptorSetAllocateInfo set_info{};
  set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  set_info.descriptorPool = pool;
  set_info.descriptorSetCount = 1;
  set_info.pSetLayouts = &set_layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  REQUIRE(vkAllocateDescriptorSets(h.device, &set_info, &set) == VK_SUCCESS);
  VkDescriptorBufferInfo buffer_info{storage.buffer, 0, bytes + 64};
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = set;
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.pBufferInfo = &buffer_info;
  vkUpdateDescriptorSets(h.device, 1, &write, 0, nullptr);

  const FillParams params{k_count, 3, 1};
  REQUIRE_MESSAGE(gfx::submit_immediate(
                      device,
                      [&](VkCommandBuffer commands) {
                        // Zero the whole buffer first so untouched tail words are visible.
                        vkCmdFillBuffer(commands, storage.buffer, 0, bytes + 64, 0);
                        VkMemoryBarrier2 to_compute{};
                        to_compute.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
                        to_compute.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
                        to_compute.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                        to_compute.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                        to_compute.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                        VkDependencyInfo dep{};
                        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                        dep.memoryBarrierCount = 1;
                        dep.pMemoryBarriers = &to_compute;
                        vkCmdPipelineBarrier2(commands, &dep);

                        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE,
                                          pipeline.pipeline);
                        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE,
                                                pipeline.layout, 0, 1, &set, 0, nullptr);
                        vkCmdPushConstants(commands, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT,
                                           0, sizeof(FillParams), &params);
                        vkCmdDispatch(commands, (k_count + 63) / 64, 1, 1);

                        VkMemoryBarrier2 to_copy{};
                        to_copy.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
                        to_copy.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                        to_copy.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                        to_copy.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                        to_copy.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                        dep.pMemoryBarriers = &to_copy;
                        vkCmdPipelineBarrier2(commands, &dep);
                        VkBufferCopy copy{0, 0, bytes + 64};
                        vkCmdCopyBuffer(commands, storage.buffer, readback.buffer, 1, &copy);
                      },
                      &error),
                  error);

  const auto* words = static_cast<const u32*>(readback.mapped);
  u32 wrong = 0;
  for (u32 i = 0; i < k_count; ++i) {
    if (words[i] != i * 3 + 1) ++wrong;
  }
  CHECK(wrong == 0);
  CHECK(words[0] == 1);
  CHECK(words[k_count - 1] == (k_count - 1) * 3 + 1);
  // The 64-word tail past `count` was zeroed by the fill and never written by the shader.
  u32 tail_touched = 0;
  for (u32 i = k_count; i < k_count + 16; ++i) {
    if (words[i] != 0) ++tail_touched;
  }
  CHECK(tail_touched == 0);

  vkDestroyDescriptorPool(h.device, pool, nullptr);
  vkDestroyDescriptorSetLayout(h.device, set_layout, nullptr);
  gfx::destroy_compute_pipeline(device, pipeline);
  gfx::destroy_shader_module(device, module);
  gfx::destroy_buffer(device, readback);
  gfx::destroy_buffer(device, storage);
  device.destroy();
}
