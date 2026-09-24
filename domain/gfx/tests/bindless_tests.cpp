// The bindless model end to end (ADR-0023): a texture and a sampler registered in the global set,
// sampled by index from a compute shader into a storage image registered in the same set, then
// read back; a buffer written through its device address; slot recycling on the timeline.
#include "raster_path.h"

#include <domain/gfx/bindless.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/render_graph.h>
#include <domain/gfx/vulkan.h>

#include <doctest/doctest.h>

#include <cstring>
#include <shaders/bda_fill.spv.h>
#include <shaders/bindless_copy.spv.h>
#include <string>

using namespace engine;

namespace {

struct CopyParams {
  u32 texture_index;
  u32 sampler_index;
  u32 output_index;
  u32 width;
  u32 height;
};

struct BdaParams {
  u64 address;
  u32 count;
  u32 stride;
};

}  // namespace

TEST_CASE("bindless: texture, sampler, and storage image by index") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::BindlessSet bindless;
  REQUIRE_MESSAGE(bindless.create(device, gfx::BindlessConfig{}, &error), error);
  CHECK(bindless.capacity().sampled_images >= 1024);
  CHECK(bindless.capacity().storage_images >= 64);
  CHECK(bindless.capacity().samplers >= 16);
  CHECK(bindless.capacity().push_constant_bytes >= sizeof(CopyParams));

  constexpr u32 k_size = 8;
  const u64 bytes = u64{k_size} * k_size * 4;

  // Source texture with a known pattern, uploaded through a staging buffer.
  gfx::ImageResource texture;
  REQUIRE(gfx::create_image_2d(device, k_size, k_size, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               texture, &error));
  gfx::BufferResource staging;
  REQUIRE(
      gfx::create_buffer(device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, staging, &error));
  auto* pattern = static_cast<u8*>(staging.mapped);
  for (u32 y = 0; y < k_size; ++y) {
    for (u32 x = 0; x < k_size; ++x) {
      u8* p = pattern + (y * k_size + x) * 4;
      p[0] = static_cast<u8>(x * 32);
      p[1] = static_cast<u8>(y * 32);
      p[2] = static_cast<u8>(255 - x * 32);
      p[3] = 255;
    }
  }
  gfx::ImageResource output;
  REQUIRE(gfx::create_image_2d(device, k_size, k_size, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, output,
                               &error));
  gfx::BufferResource readback;
  REQUIRE(
      gfx::create_buffer(device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, readback, &error));
  std::memset(readback.mapped, 0, static_cast<usize>(bytes));

  VkImageView texture_view = VK_NULL_HANDLE;
  VkImageView output_view = VK_NULL_HANDLE;
  VkSampler nearest = VK_NULL_HANDLE;
  VkSampler linear = VK_NULL_HANDLE;
  REQUIRE(gfx::create_image_view(device, texture, texture_view, &error));
  REQUIRE(gfx::create_image_view(device, output, output_view, &error));
  REQUIRE(gfx::create_sampler(device, VK_FILTER_NEAREST, nearest, &error));
  REQUIRE(gfx::create_sampler(device, VK_FILTER_LINEAR, linear, &error));

  // Occupy slot 0 of each array with something else so the test exercises non-zero indices.
  const u32 decoy_texture = bindless.add_sampled_image(output_view, VK_IMAGE_LAYOUT_GENERAL);
  const u32 decoy_sampler = bindless.add_sampler(linear);
  const u32 texture_slot =
      bindless.add_sampled_image(texture_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  const u32 sampler_slot = bindless.add_sampler(nearest);
  const u32 output_slot = bindless.add_storage_image(output_view);
  CHECK(decoy_texture == 0);
  CHECK(texture_slot == 1);
  CHECK(decoy_sampler == 0);
  CHECK(sampler_slot == 1);
  CHECK(output_slot == 0);
  CHECK(bindless.live_sampled_images() == 2);
  CHECK(bindless.live_samplers() == 2);
  CHECK(bindless.live_storage_images() == 1);

  VkShaderModule module = gfx::create_shader_module(device, shaders::k_bindless_copy_spirv,
                                                    shaders::k_bindless_copy_spirv_size, &error);
  REQUIRE(module != VK_NULL_HANDLE);
  // The bindless pipeline layout is shared: the compute pipeline only needs the module.
  VkComputePipelineCreateInfo pipeline_info{};
  pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline_info.stage.module = module;
  pipeline_info.stage.pName = "copy";
  pipeline_info.layout = bindless.pipeline_layout();
  VkPipeline pipeline = VK_NULL_HANDLE;
  REQUIRE(vkCreateComputePipelines(device.handles().device, VK_NULL_HANDLE, 1, &pipeline_info,
                                   nullptr, &pipeline) == VK_SUCCESS);

  gfx::RenderGraph graph(device);
  const gfx::RgBuffer rg_staging = graph.import_buffer(
      "staging", staging, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT);
  const gfx::RgImage rg_texture = graph.import_image("texture", texture);
  const gfx::RgImage rg_output = graph.import_image("output", output);
  const gfx::RgBuffer rg_readback = graph.import_buffer("readback", readback);
  graph.add_pass(
      "upload", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        b.read(rg_staging, gfx::Access::TransferRead);
        b.write(rg_texture, gfx::Access::TransferWrite);
      },
      [&](VkCommandBuffer commands, gfx::RenderGraph& g) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {k_size, k_size, 1};
        vkCmdCopyBufferToImage(commands, staging.buffer, g.image(rg_texture).image,
                               g.image_layout(rg_texture), 1, &region);
      });
  graph.add_pass(
      "copy through bindless", gfx::PassKind::Compute,
      [&](gfx::PassBuilder& b) {
        b.read(rg_texture, gfx::Access::SampledRead);
        b.write(rg_output, gfx::Access::ComputeWrite);
      },
      [&](VkCommandBuffer commands, gfx::RenderGraph&) {
        const CopyParams params{texture_slot, sampler_slot, output_slot, k_size, k_size};
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        bindless.bind(commands, VK_PIPELINE_BIND_POINT_COMPUTE);
        vkCmdPushConstants(commands, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                           sizeof(params), &params);
        vkCmdDispatch(commands, (k_size + 7) / 8, (k_size + 7) / 8, 1);
      });
  graph.add_pass(
      "readback", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        b.read(rg_output, gfx::Access::TransferRead);
        b.write(rg_readback, gfx::Access::TransferWrite);
      },
      [&](VkCommandBuffer commands, gfx::RenderGraph& g) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {k_size, k_size, 1};
        vkCmdCopyImageToBuffer(commands, g.image(rg_output).image, g.image_layout(rg_output),
                               readback.buffer, 1, &region);
      });
  REQUIRE_MESSAGE(graph.compile(&error), error);
  CHECK(graph.stats().layout_transitions ==
        4);  // texture: ->DST, ->READ_ONLY; output: ->GENERAL, ->SRC

  VkCommandBuffer commands = frames.begin_frame();
  graph.execute(commands);
  const u64 value = frames.end_frame();
  REQUIRE(frames.wait(value));

  const auto* pixels = static_cast<const u8*>(readback.mapped);
  u32 wrong = 0;
  for (u32 i = 0; i < k_size * k_size; ++i) {
    for (u32 c = 0; c < 4; ++c) {
      if (pixels[i * 4 + c] != pattern[i * 4 + c]) ++wrong;
    }
  }
  CHECK(wrong == 0);
  CHECK(pixels[(3 * k_size + 5) * 4] == 5 * 32);

  // Release slots for the frame that used them; they come back only when it completes.
  bindless.release_sampled_image(texture_slot, value);
  bindless.release_sampler(sampler_slot, value);
  bindless.release_storage_image(output_slot, value);
  CHECK(bindless.pending_releases() == 3);
  bindless.recycle(value - 1);
  CHECK(bindless.pending_releases() == 3);
  CHECK(bindless.live_sampled_images() == 2);
  bindless.recycle(frames.completed());
  CHECK(bindless.pending_releases() == 0);
  CHECK(bindless.live_sampled_images() == 1);
  CHECK(bindless.live_samplers() == 1);
  CHECK(bindless.live_storage_images() == 0);
  // Freed slots are reused before fresh ones.
  CHECK(bindless.add_sampler(nearest) == sampler_slot);
  CHECK(bindless.add_storage_image(output_view) == output_slot);

  graph.reset();
  vkDestroyPipeline(device.handles().device, pipeline, nullptr);
  gfx::destroy_shader_module(device, module);
  bindless.destroy();
  gfx::destroy_sampler(device, nearest);
  gfx::destroy_sampler(device, linear);
  gfx::destroy_image_view(device, texture_view);
  gfx::destroy_image_view(device, output_view);
  gfx::destroy_buffer(device, readback);
  gfx::destroy_buffer(device, staging);
  gfx::destroy_image(device, output);
  gfx::destroy_image(device, texture);
  frames.destroy();
  device.destroy();
}

TEST_CASE("bindless: buffers are reached through device addresses") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  constexpr u32 k_count = 300;
  const u64 bytes = u64{k_count} * sizeof(u32);
  gfx::BufferResource target;
  REQUIRE(gfx::create_buffer(device, bytes,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             false, target, &error));
  REQUIRE(target.address != 0);
  gfx::BufferResource readback;
  REQUIRE(
      gfx::create_buffer(device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, readback, &error));

  VkShaderModule module = gfx::create_shader_module(device, shaders::k_bda_fill_spirv,
                                                    shaders::k_bda_fill_spirv_size, &error);
  REQUIRE(module != VK_NULL_HANDLE);
  VkComputePipelineCreateInfo pipeline_info{};
  pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline_info.stage.module = module;
  pipeline_info.stage.pName = "bda_fill";
  pipeline_info.layout = bindless.pipeline_layout();
  VkPipeline pipeline = VK_NULL_HANDLE;
  REQUIRE(vkCreateComputePipelines(device.handles().device, VK_NULL_HANDLE, 1, &pipeline_info,
                                   nullptr, &pipeline) == VK_SUCCESS);

  const BdaParams params{target.address, k_count, 7};
  REQUIRE(gfx::submit_immediate(
      device,
      [&](VkCommandBuffer commands) {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        bindless.bind(commands, VK_PIPELINE_BIND_POINT_COMPUTE);
        vkCmdPushConstants(commands, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                           sizeof(params), &params);
        vkCmdDispatch(commands, (k_count + 63) / 64, 1, 1);
        VkMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commands, &dependency);
        VkBufferCopy copy{0, 0, bytes};
        vkCmdCopyBuffer(commands, target.buffer, readback.buffer, 1, &copy);
      },
      &error));
  const auto* words = static_cast<const u32*>(readback.mapped);
  u32 wrong = 0;
  for (u32 i = 0; i < k_count; ++i) {
    if (words[i] != i * 7) ++wrong;
  }
  CHECK(wrong == 0);
  CHECK(words[k_count - 1] == (k_count - 1) * 7);

  vkDestroyPipeline(device.handles().device, pipeline, nullptr);
  gfx::destroy_shader_module(device, module);
  bindless.destroy();
  gfx::destroy_buffer(device, readback);
  gfx::destroy_buffer(device, target);
  device.destroy();
}
