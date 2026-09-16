// The first raster pass: dynamic rendering through the render graph, a graphics pipeline with no
// vertex input, and a pixel-exact readback of what was drawn.
#include <domain/gfx/bindless.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/render_graph.h>
#include <domain/gfx/vulkan.h>

#include <doctest/doctest.h>

#include <cstring>
#include <shaders/triangle.spv.h>
#include <string>

using namespace engine;

namespace {

struct TriangleParams {
  float color[3];
  float scale;
};

}  // namespace

TEST_CASE("raster: a triangle through the render graph lands on the expected pixels") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("device unavailable: " << error);
    return;
  }
  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));

  constexpr u32 k_size = 64;
  const u64 bytes = u64{k_size} * k_size * 4;
  gfx::BufferResource readback;
  REQUIRE(
      gfx::create_buffer(device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, readback, &error));
  std::memset(readback.mapped, 0x7f, static_cast<usize>(bytes));

  VkShaderModule module = gfx::create_shader_module(device, shaders::k_triangle_spirv,
                                                    shaders::k_triangle_spirv_size, &error);
  REQUIRE_MESSAGE(module != VK_NULL_HANDLE, error);
  gfx::GraphicsPipelineDesc desc;
  desc.vertex = module;
  desc.fragment = module;
  desc.layout = bindless.pipeline_layout();
  desc.color_format = VK_FORMAT_R8G8B8A8_UNORM;
  VkPipeline pipeline = VK_NULL_HANDLE;
  REQUIRE_MESSAGE(gfx::create_graphics_pipeline(device, desc, pipeline, &error), error);

  gfx::RenderGraph graph(device);
  const gfx::RgImage color = graph.create_image(
      "color", {k_size, k_size, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
  const gfx::RgBuffer host = graph.import_buffer("readback", readback);
  VkClearColorValue clear{};
  clear.float32[2] = 1.0f;  // blue background
  clear.float32[3] = 1.0f;
  VkExtent2D seen_area{};
  graph.add_pass(
      "triangle", gfx::PassKind::Raster,
      [&](gfx::PassBuilder& b) { b.color_attachment(color, VK_ATTACHMENT_LOAD_OP_CLEAR, clear); },
      [&](VkCommandBuffer commands, gfx::RenderGraph& g) {
        seen_area = g.render_area();
        const TriangleParams params{{1.0f, 0.5f, 0.0f}, 1.0f};
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        bindless.bind(commands, VK_PIPELINE_BIND_POINT_GRAPHICS);
        vkCmdPushConstants(commands, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                           sizeof(params), &params);
        vkCmdDraw(commands, 3, 1, 0, 0);
      });
  graph.add_pass(
      "readback", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        b.read(color, gfx::Access::TransferRead);
        b.write(host, gfx::Access::TransferWrite);
      },
      [&](VkCommandBuffer commands, gfx::RenderGraph& g) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {k_size, k_size, 1};
        vkCmdCopyImageToBuffer(commands, g.image(color).image, g.image_layout(color),
                               readback.buffer, 1, &region);
      });
  REQUIRE_MESSAGE(graph.compile(&error), error);
  CHECK(graph.stats().raster_passes == 1);
  CHECK(graph.stats().transient_images == 1);
  CHECK(graph.image_view(color) != VK_NULL_HANDLE);
  // color: UNDEFINED -> COLOR_ATTACHMENT (triangle), -> TRANSFER_SRC (readback).
  CHECK(graph.stats().layout_transitions == 2);

  VkCommandBuffer commands = frames.begin_frame();
  graph.execute(commands);
  const u64 value = frames.end_frame();
  REQUIRE(frames.wait(value));
  CHECK(seen_area.width == k_size);
  CHECK(seen_area.height == k_size);

  // The triangle covers the bottom-left half in y-up clip space; with the graph's flipped viewport
  // that is the bottom-left half of the image (row 0 is the top). The rest keeps the clear.
  const auto* pixels = static_cast<const u8*>(readback.mapped);
  u32 covered = 0;
  u32 wrong = 0;
  for (u32 y = 0; y < k_size; ++y) {
    for (u32 x = 0; x < k_size; ++x) {
      const u8* p = pixels + (y * k_size + x) * 4;
      const u32 fy = k_size - 1 - y;            // y-up row
      const bool inside = x + fy + 1 < k_size;  // strictly inside, away from the diagonal edge
      const bool outside = x + fy > k_size;     // strictly outside
      if (inside) {
        ++covered;
        if (p[0] != 255 || (p[1] < 127 || p[1] > 128) || p[2] != 0 || p[3] != 255) ++wrong;
      } else if (outside) {
        if (p[0] != 0 || p[1] != 0 || p[2] != 255 || p[3] != 255) ++wrong;
      }
    }
  }
  CHECK(wrong == 0);
  CHECK(covered == (k_size - 1) * k_size / 2);
  CHECK(pixels[(k_size - 1) * k_size * 4] == 255);  // bottom-left corner: triangle
  CHECK(pixels[(k_size - 1) * 4 + 2] == 255);       // top-right corner: clear

  graph.reset();
  gfx::destroy_pipeline(device, pipeline);
  gfx::destroy_shader_module(device, module);
  bindless.destroy();
  gfx::destroy_buffer(device, readback);
  frames.destroy();
  device.destroy();
}
