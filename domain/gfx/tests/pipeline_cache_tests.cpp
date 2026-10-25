// The pipeline cache on disk (pipeline_cache.h; gfx.md, "The pipeline cache"; roadmap W75): the
// header check without a device, and with one, a cache that is written when a pipeline grew it,
// read back by the next device, ignored when its header is another device's, and a compute
// shader that writes the same bytes through a cached pipeline as through a fresh one.
#include "raster_path.h"

#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/device.h>
#include <domain/gfx/pipeline_cache.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstring>
#include <shaders/fill.spv.h>
#include <string>

using namespace engine;

namespace {

Vector<u8> header_for(const gfx::PipelineCacheIdentity& id) {
  Vector<u8> bytes(gfx::k_pipeline_cache_header_bytes + 16, u8{0});
  const u32 words[4] = {gfx::k_pipeline_cache_header_bytes, 1u, id.vendor_id, id.device_id};
  std::memcpy(bytes.data(), words, sizeof(words));
  std::memcpy(bytes.data() + 16, id.uuid, sizeof(id.uuid));
  return bytes;
}

gfx::PipelineCacheIdentity some_device() {
  gfx::PipelineCacheIdentity id;
  id.vendor_id = 0x10de;
  id.device_id = 0x2b85;
  id.driver_version = 0x9a4a8000;
  for (u32 i = 0; i < 16; ++i)
    id.uuid[i] = static_cast<u8>(i * 7 + 1);
  return id;
}

struct FillParams {
  u32 count;
  u32 multiplier;
  u32 offset;
};

// The fill shader through `device`'s pipeline, its output read back.
bool fill(const gfx::Device& device, Vector<u32>& out) {
  const gfx::Handles& h = device.handles();
  std::string error;
  gfx::ShaderModuleHandle module =
      gfx::create_shader_module(device, shaders::k_fill_spirv, shaders::k_fill_spirv_size, &error);
  REQUIRE_MESSAGE(module.valid(), error);
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
  const gfx::DescriptorSetLayoutHandle layouts[] = {gfx::vk::wrap(set_layout)};
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, module, "fill", layouts, sizeof(FillParams),
                                               pipeline, &error),
                  error);
  constexpr u32 k_count = 256;
  const u64 bytes = u64{k_count} * sizeof(u32);
  gfx::BufferResource storage;
  gfx::BufferResource readback;
  REQUIRE(gfx::create_buffer(device, bytes,
                             gfx::BufferUsage::Storage | gfx::BufferUsage::TransferSrc, false,
                             storage, &error));
  REQUIRE(gfx::create_buffer(device, bytes, gfx::BufferUsage::TransferDst, true, readback, &error));
  VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  VkDescriptorPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  VkDescriptorPool pool = {};
  REQUIRE(vkCreateDescriptorPool(h.device, &pool_info, nullptr, &pool) == VK_SUCCESS);
  VkDescriptorSetAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  alloc.descriptorPool = pool;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &set_layout;
  VkDescriptorSet set = {};
  REQUIRE(vkAllocateDescriptorSets(h.device, &alloc, &set) == VK_SUCCESS);
  VkDescriptorBufferInfo buffer_info{gfx::vk::native(storage.buffer), 0, bytes};
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = set;
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.pBufferInfo = &buffer_info;
  vkUpdateDescriptorSets(h.device, 1, &write, 0, nullptr);
  const FillParams params{k_count, 3u, 11u};
  REQUIRE_MESSAGE(gfx::submit_immediate(
                      device,
                      [&](gfx::CommandList commands) {
                        commands.bind_pipeline(gfx::BindPoint::Compute, pipeline.pipeline);
                        vkCmdBindDescriptorSets(
                            gfx::vk::native(commands), VK_PIPELINE_BIND_POINT_COMPUTE,
                            gfx::vk::native(pipeline.layout), 0, 1, &set, 0, nullptr);
                        commands.push_constants(pipeline.layout, gfx::ShaderStage::Compute, 0,
                                                sizeof(FillParams), &params);
                        commands.dispatch((k_count + 63) / 64, 1, 1);
                        VkMemoryBarrier2 to_copy{};
                        to_copy.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
                        to_copy.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                        to_copy.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                        to_copy.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                        to_copy.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                        VkDependencyInfo dep{};
                        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                        dep.memoryBarrierCount = 1;
                        dep.pMemoryBarriers = &to_copy;
                        vkCmdPipelineBarrier2(gfx::vk::native(commands), &dep);
                        gfx::BufferCopy copy{0, 0, bytes};
                        commands.copy_buffer(storage.buffer, readback.buffer, copy);
                      },
                      &error),
                  error);
  out.resize(k_count);
  std::memcpy(out.data(), readback.mapped, static_cast<usize>(bytes));
  vkDestroyDescriptorPool(h.device, pool, nullptr);
  gfx::destroy_buffer(device, storage);
  gfx::destroy_buffer(device, readback);
  gfx::destroy_compute_pipeline(device, pipeline);
  vkDestroyDescriptorSetLayout(h.device, set_layout, nullptr);
  gfx::destroy_shader_module(device, module);
  return true;
}

}  // namespace

TEST_CASE("pipeline cache: a header names one device and driver, and anything else is refused") {
  const gfx::PipelineCacheIdentity id = some_device();
  const Vector<u8> good = header_for(id);
  CHECK(gfx::pipeline_cache_header_matches(std::span<const u8>(good.data(), good.size()), id));
  // Shorter than a header, another vendor, another device, another UUID, another header version.
  CHECK_FALSE(gfx::pipeline_cache_header_matches(std::span<const u8>(good.data(), 31), id));
  for (const u32 at : {4u, 8u, 12u, 16u, 31u}) {
    Vector<u8> bad = good;
    bad[at] ^= 0x5au;
    CHECK_FALSE(
        gfx::pipeline_cache_header_matches(std::span<const u8>(bad.data(), bad.size()), id));
  }
  CHECK(gfx::pipeline_cache_file("d/pipeline-cache", id).ends_with("10de-2b85-9a4a8000.bin"));
  gfx::PipelineCacheIdentity other = id;
  other.driver_version += 1;
  CHECK(gfx::pipeline_cache_file("d", other) != gfx::pipeline_cache_file("d", id));
}

TEST_CASE("pipeline cache: written when a pipeline grew it, read back, and the same bytes drawn") {
  const test::TempDir tmp("gfx_pipeline_cache");
  REQUIRE(tmp.ok());
  gfx::DeviceOptions options;
  options.pipeline_cache_dir = tmp.file("pipeline-cache");

  Vector<u32> first;
  std::string path;
  {
    gfx::Device device;
    if (!gfx_test::open_device(device, options)) return;
    const gfx::PipelineCacheReport& report = device.pipeline_cache();
    REQUIRE(report.enabled);
    CHECK_FALSE(report.loaded);  // nothing there yet
    path = report.path;
    REQUIRE(fill(device, first));
    REQUIRE(device.save_pipeline_cache());
    CHECK(device.pipeline_cache().saved_bytes > gfx::k_pipeline_cache_header_bytes);
    CHECK(io::exists(path));
  }
  Vector<u32> second;
  {
    gfx::Device device;
    REQUIRE(gfx_test::open_device(device, options));
    CHECK(device.pipeline_cache().loaded);
    CHECK(device.pipeline_cache().loaded_bytes > gfx::k_pipeline_cache_header_bytes);
    CHECK(device.pipeline_cache().ignored.empty());
    REQUIRE(fill(device, second));
  }
  CHECK(first == second);  // a pipeline from the cache computes what a fresh one did

  // Another device's header (one byte of the UUID changed): ignored, said, and the device works.
  std::string bytes;
  REQUIRE(io::read_file(path, bytes) == io::Status::Ok);
  REQUIRE(bytes.size() > 20);
  bytes[20] = static_cast<char>(bytes[20] ^ 0x5a);
  REQUIRE(io::write_file(path, bytes) == io::Status::Ok);
  {
    gfx::Device device;
    REQUIRE(gfx_test::open_device(device, options));
    CHECK_FALSE(device.pipeline_cache().loaded);
    CHECK(device.pipeline_cache().ignored == "another device's or driver's header");
    Vector<u32> third;
    REQUIRE(fill(device, third));
    CHECK(third == first);
  }
  // Not a cache at all.
  REQUIRE(io::write_file(path, "not a pipeline cache") == io::Status::Ok);
  {
    gfx::Device device;
    REQUIRE(gfx_test::open_device(device, options));
    CHECK_FALSE(device.pipeline_cache().loaded);
    CHECK(device.pipeline_cache().ignored == "shorter than a header");
  }
  // No directory: no cache, every pipeline made without one, as before.
  {
    gfx::Device device;
    REQUIRE(gfx_test::open_device(device));
    CHECK_FALSE(device.pipeline_cache().enabled);
    Vector<u32> plain;
    REQUIRE(fill(device, plain));
    CHECK(plain == first);
  }
}
