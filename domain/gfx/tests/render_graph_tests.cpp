// Render graph v0: declared accesses drive barriers and layouts; pass bodies only bind and
// dispatch. Two compute passes chained through a transient buffer, an image cleared and read
// back, and the validation of a read without a producer.
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/render_graph.h>
#include <domain/gfx/vulkan.h>

#include <doctest/doctest.h>

#include <cstring>
#include <shaders/fill.spv.h>
#include <shaders/scale.spv.h>
#include <string>

using namespace engine;

namespace {

// A descriptor set of N storage buffers at bindings 0..N-1, for compute test passes.
struct StorageSet {
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;

  bool create(const gfx::Device& device, std::span<const VkBuffer> buffers) {
    const gfx::Handles& h = device.handles();
    Vector<VkDescriptorSetLayoutBinding> bindings;
    for (u32 i = 0; i < buffers.size(); ++i) {
      VkDescriptorSetLayoutBinding b{};
      b.binding = i;
      b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      b.descriptorCount = 1;
      b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
      bindings.push_back(b);
    }
    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = bindings.size();
    layout_info.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(h.device, &layout_info, nullptr, &layout) != VK_SUCCESS)
      return false;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<u32>(buffers.size())};
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &size;
    if (vkCreateDescriptorPool(h.device, &pool_info, nullptr, &pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo set_info{};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_info.descriptorPool = pool;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &layout;
    if (vkAllocateDescriptorSets(h.device, &set_info, &set) != VK_SUCCESS) return false;
    Vector<VkDescriptorBufferInfo> infos;
    Vector<VkWriteDescriptorSet> writes;
    for (u32 i = 0; i < buffers.size(); ++i)
      infos.push_back(VkDescriptorBufferInfo{buffers[i], 0, VK_WHOLE_SIZE});
    for (u32 i = 0; i < buffers.size(); ++i) {
      VkWriteDescriptorSet w{};
      w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w.dstSet = set;
      w.dstBinding = i;
      w.descriptorCount = 1;
      w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      w.pBufferInfo = &infos[i];
      writes.push_back(w);
    }
    vkUpdateDescriptorSets(h.device, writes.size(), writes.data(), 0, nullptr);
    return true;
  }
  void destroy(const gfx::Device& device) {
    const gfx::Handles& h = device.handles();
    if (pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(h.device, pool, nullptr);
    if (layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(h.device, layout, nullptr);
    *this = StorageSet{};
  }
};

struct FillParams {
  u32 count;
  u32 multiplier;
  u32 offset;
};
struct ScaleParams {
  u32 count;
  u32 factor;
};

}  // namespace

TEST_CASE("render graph: compute passes chained through a transient buffer") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("device unavailable: " << error);
    return;
  }
  constexpr u32 k_count = 1000;
  const u64 bytes = u64{k_count} * sizeof(u32);

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::BufferResource output;
  REQUIRE(gfx::create_buffer(device, bytes,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             false, output, &error));
  gfx::BufferResource readback;
  REQUIRE(
      gfx::create_buffer(device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, readback, &error));

  VkShaderModule fill_module =
      gfx::create_shader_module(device, shaders::k_fill_spirv, shaders::k_fill_spirv_size, &error);
  VkShaderModule scale_module = gfx::create_shader_module(device, shaders::k_scale_spirv,
                                                          shaders::k_scale_spirv_size, &error);
  REQUIRE(fill_module != VK_NULL_HANDLE);
  REQUIRE(scale_module != VK_NULL_HANDLE);

  gfx::RenderGraph graph(device);
  const gfx::RgBuffer temp =
      graph.create_buffer("temp", {bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false});
  const gfx::RgBuffer out = graph.import_buffer("out", output);
  const gfx::RgBuffer host = graph.import_buffer("readback", readback);

  // Pass bodies bind their sets and pipelines; sets are created once the graph has allocated
  // the transient, which is why creation happens after compile below.
  StorageSet fill_set;
  StorageSet scale_set;
  gfx::ComputePipeline fill_pipeline;
  gfx::ComputePipeline scale_pipeline;

  graph.add_pass(
      "fill", gfx::PassKind::Compute,
      [&](gfx::PassBuilder& b) { b.write(temp, gfx::Access::ComputeWrite); },
      [&](VkCommandBuffer commands, gfx::RenderGraph&) {
        const FillParams params{k_count, 3, 1};
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, fill_pipeline.pipeline);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, fill_pipeline.layout, 0,
                                1, &fill_set.set, 0, nullptr);
        vkCmdPushConstants(commands, fill_pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(params), &params);
        vkCmdDispatch(commands, (k_count + 63) / 64, 1, 1);
      });
  graph.add_pass(
      "scale", gfx::PassKind::Compute,
      [&](gfx::PassBuilder& b) {
        b.read(temp, gfx::Access::ComputeRead);
        b.write(out, gfx::Access::ComputeWrite);
      },
      [&](VkCommandBuffer commands, gfx::RenderGraph&) {
        const ScaleParams params{k_count, 2};
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, scale_pipeline.pipeline);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, scale_pipeline.layout, 0,
                                1, &scale_set.set, 0, nullptr);
        vkCmdPushConstants(commands, scale_pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(params), &params);
        vkCmdDispatch(commands, (k_count + 63) / 64, 1, 1);
      });
  graph.add_pass(
      "readback", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        b.read(out, gfx::Access::TransferRead);
        b.write(host, gfx::Access::TransferWrite);
      },
      [&](VkCommandBuffer commands, gfx::RenderGraph& g) {
        VkBufferCopy copy{0, 0, bytes};
        vkCmdCopyBuffer(commands, g.buffer(out).buffer, g.buffer(host).buffer, 1, &copy);
      });

  REQUIRE_MESSAGE(graph.compile(&error), error);
  const gfx::RenderGraph::Stats& s = graph.stats();
  CHECK(s.passes == 3);
  CHECK(s.buffers == 3);
  CHECK(s.transient_buffers == 1);
  // fill: temp has no history -> no barrier. scale: temp was written (1), out has no history
  // (0). readback: out was written (1), host has no history (0). Total 2.
  CHECK(s.buffer_barriers == 2);
  CHECK(s.image_barriers == 0);
  CHECK(graph.buffer(temp).buffer != VK_NULL_HANDLE);
  CHECK(graph.buffer(temp).size == bytes);

  const VkBuffer fill_buffers[] = {graph.buffer(temp).buffer};
  REQUIRE(fill_set.create(device, fill_buffers));
  const VkBuffer scale_buffers[] = {graph.buffer(temp).buffer, output.buffer};
  REQUIRE(scale_set.create(device, scale_buffers));
  const VkDescriptorSetLayout fill_layouts[] = {fill_set.layout};
  const VkDescriptorSetLayout scale_layouts[] = {scale_set.layout};
  REQUIRE(gfx::create_compute_pipeline(device, fill_module, "fill", fill_layouts,
                                       sizeof(FillParams), fill_pipeline, &error));
  REQUIRE(gfx::create_compute_pipeline(device, scale_module, "scale", scale_layouts,
                                       sizeof(ScaleParams), scale_pipeline, &error));

  std::memset(readback.mapped, 0, static_cast<usize>(bytes));
  VkCommandBuffer commands = frames.begin_frame();
  graph.execute(commands);
  const u64 value = frames.end_frame();
  REQUIRE(frames.wait(value));

  const auto* words = static_cast<const u32*>(readback.mapped);
  u32 wrong = 0;
  for (u32 i = 0; i < k_count; ++i) {
    if (words[i] != (i * 3 + 1) * 2) ++wrong;
  }
  CHECK(wrong == 0);
  CHECK(words[k_count - 1] == ((k_count - 1) * 3 + 1) * 2);

  // Re-running the same graph next frame is a reset and a rebuild; transients are released.
  graph.reset();
  CHECK_FALSE(graph.compiled());
  CHECK(graph.stats().passes == 0);

  fill_set.destroy(device);
  scale_set.destroy(device);
  gfx::destroy_compute_pipeline(device, fill_pipeline);
  gfx::destroy_compute_pipeline(device, scale_pipeline);
  gfx::destroy_shader_module(device, fill_module);
  gfx::destroy_shader_module(device, scale_module);
  frames.destroy();
  gfx::destroy_buffer(device, readback);
  gfx::destroy_buffer(device, output);
  device.destroy();
}

TEST_CASE("render graph: image layouts follow declared accesses") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("device unavailable: " << error);
    return;
  }
  constexpr u32 k_size = 32;
  const u64 bytes = u64{k_size} * k_size * 4;
  gfx::BufferResource readback;
  REQUIRE(
      gfx::create_buffer(device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, readback, &error));
  gfx::ImageResource persistent;
  REQUIRE(gfx::create_image_2d(device, k_size, k_size, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               persistent, &error));

  gfx::RenderGraph graph(device);
  const gfx::RgImage scratch = graph.create_image(
      "scratch", {k_size, k_size, VK_FORMAT_R8G8B8A8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
  const gfx::RgImage imported =
      graph.import_image("persistent", persistent, VK_IMAGE_LAYOUT_UNDEFINED);
  const gfx::RgBuffer host = graph.import_buffer("readback", readback);
  VkImageLayout seen_in_clear = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageLayout seen_in_copy = VK_IMAGE_LAYOUT_UNDEFINED;

  graph.add_pass(
      "clear", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) { b.write(scratch, gfx::Access::TransferWrite); },
      [&](VkCommandBuffer commands, gfx::RenderGraph& g) {
        seen_in_clear = g.image_layout(scratch);
        VkClearColorValue color{};
        color.float32[0] = 1.0f;
        color.float32[3] = 1.0f;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(commands, g.image(scratch).image, g.image_layout(scratch), &color, 1,
                             &range);
      });
  graph.add_pass(
      "blit", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        b.read(scratch, gfx::Access::TransferRead);
        b.write(imported, gfx::Access::TransferWrite);
      },
      [&](VkCommandBuffer commands, gfx::RenderGraph& g) {
        seen_in_copy = g.image_layout(scratch);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {k_size, k_size, 1};
        vkCmdCopyImage(commands, g.image(scratch).image, g.image_layout(scratch),
                       g.image(imported).image, g.image_layout(imported), 1, &region);
      });
  graph.add_pass(
      "readback", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        b.read(imported, gfx::Access::TransferRead);
        b.write(host, gfx::Access::TransferWrite);
      },
      [&](VkCommandBuffer commands, gfx::RenderGraph& g) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {k_size, k_size, 1};
        vkCmdCopyImageToBuffer(commands, g.image(imported).image, g.image_layout(imported),
                               g.buffer(host).buffer, 1, &region);
      });
  REQUIRE_MESSAGE(graph.compile(&error), error);
  const auto& s = graph.stats();
  CHECK(s.transient_images == 1);
  // scratch: UNDEFINED->DST (clear), DST->SRC (blit); persistent: UNDEFINED->DST (blit),
  // DST->SRC (readback). Four image barriers, all layout transitions.
  CHECK(s.image_barriers == 4);
  CHECK(s.layout_transitions == 4);
  CHECK(s.buffer_barriers == 0);

  REQUIRE(gfx::submit_immediate(
      device, [&](VkCommandBuffer commands) { graph.execute(commands); }, &error));
  CHECK(seen_in_clear == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  CHECK(seen_in_copy == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  CHECK(graph.final_layout(imported) == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  const auto* pixels = static_cast<const u8*>(readback.mapped);
  u32 wrong = 0;
  for (u32 i = 0; i < k_size * k_size; ++i) {
    if (pixels[i * 4] != 255 || pixels[i * 4 + 1] != 0 || pixels[i * 4 + 3] != 255) ++wrong;
  }
  CHECK(wrong == 0);

  // The final layout feeds the next graph's import so the first barrier is exact.
  graph.reset();
  const gfx::RgImage again =
      graph.import_image("persistent", persistent, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
  graph.add_pass(
      "read again", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) { b.read(again, gfx::Access::TransferRead); },
      [&](VkCommandBuffer, gfx::RenderGraph&) {});
  REQUIRE(graph.compile(&error));
  // Same layout, read after read: a barrier is still emitted because an import's history is
  // treated as a write, but no layout transition.
  CHECK(graph.stats().layout_transitions == 0);
  graph.reset();

  gfx::destroy_image(device, persistent);
  gfx::destroy_buffer(device, readback);
  device.destroy();
}

TEST_CASE("render graph: a read without a producer is rejected at compile") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("device unavailable: " << error);
    return;
  }
  gfx::RenderGraph graph(device);
  const gfx::RgBuffer orphan =
      graph.create_buffer("orphan", {256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false});
  graph.add_pass(
      "consumer", gfx::PassKind::Compute,
      [&](gfx::PassBuilder& b) { b.read(orphan, gfx::Access::ComputeRead); },
      [&](VkCommandBuffer, gfx::RenderGraph&) {});
  CHECK_FALSE(graph.compile(&error));
  CHECK(error.find("consumer") != std::string::npos);
  CHECK(error.find("orphan") != std::string::npos);
  CHECK_FALSE(graph.compiled());
  graph.reset();
  device.destroy();
}
