// The capture path: draw the test triangle through the graph, capture the attachment, convert
// to RGBA8, encode a PNG, and check the pixels the same way the raster test does. Also covers
// the render graph's set_final_layout, since captures of presented frames rely on it.
#include "raster_path.h"

#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/capture.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/render_graph.h>
#include <foundation/image/png.h>

#include <doctest/doctest.h>

#include <shaders/triangle.spv.h>
#include <string>

using namespace engine;

TEST_CASE("capture: an attachment left in a requested layout captures pixel-exact") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));

  gfx::ShaderModuleHandle module = gfx::create_shader_module(
      device, shaders::k_triangle_spirv, shaders::k_triangle_spirv_size, &error);
  REQUIRE(module.valid());
  gfx::GraphicsPipelineDesc desc;
  desc.vertex = module;
  desc.fragment = module;
  desc.layout = bindless.pipeline_layout();
  desc.color_format = gfx::Format::B8G8R8A8Unorm;
  gfx::PipelineHandle pipeline = {};
  REQUIRE_MESSAGE(gfx::create_graphics_pipeline(device, desc, pipeline, &error), error);

  // A persistent target imported into the graph, like a swapchain image would be.
  constexpr u32 k_size = 32;
  gfx::ImageResource target;
  REQUIRE(gfx::create_image_2d(device, k_size, k_size, gfx::Format::B8G8R8A8Unorm,
                               gfx::ImageUsage::ColorAttachment | gfx::ImageUsage::TransferSrc,
                               target, &error));

  gfx::RenderGraph graph(device);
  const gfx::RgImage color = graph.import_image("target", target);
  gfx::ClearColor clear{};
  clear.float32[2] = 1.0f;  // blue in BGRA order: byte 0
  clear.float32[3] = 1.0f;
  struct Push {
    f32 color[3];
    f32 scale;
  } push{{1.0f, 0.5f, 0.0f}, 1.0f};
  graph.add_pass(
      "triangle", gfx::PassKind::Raster,
      [&](gfx::PassBuilder& b) { b.color_attachment(color, gfx::LoadOp::Clear, clear); },
      [&](gfx::CommandList commands, gfx::RenderGraph&) {
        commands.bind_pipeline(gfx::BindPoint::Graphics, pipeline);
        bindless.bind(commands, gfx::BindPoint::Graphics);
        commands.push_constants(bindless.pipeline_layout(), gfx::ShaderStage::All, 0, sizeof(push),
                                &push);
        commands.draw(3, 1, 0, 0);
      });
  graph.set_final_layout(color, gfx::ImageLayout::Present);
  REQUIRE_MESSAGE(graph.compile(&error), error);
  CHECK(graph.stats().layout_transitions == 2);  // undefined -> color attachment -> present

  gfx::CommandList commands = frames.begin_frame();
  graph.execute(commands);
  REQUIRE(frames.wait(frames.end_frame()));
  CHECK(graph.final_layout(color) == gfx::ImageLayout::Present);

  gfx::Capture capture;
  REQUIRE_MESSAGE(gfx::capture_image(device, target, gfx::ImageLayout::Present, capture, &error),
                  error);
  CHECK(capture.width == k_size);
  CHECK(capture.height == k_size);
  CHECK(capture.bytes_per_pixel == 4);
  CHECK(capture.bytes.size() == k_size * k_size * 4);
  Vector<u8> rgba;
  REQUIRE(gfx::capture_to_rgba8(capture, rgba));

  // Same coverage as the raster test: the bottom-left half below the diagonal is the pushed color.
  u32 covered = 0;
  u32 wrong = 0;
  for (u32 y = 0; y < k_size; ++y) {
    for (u32 x = 0; x < k_size; ++x) {
      const u8* p = rgba.data() + (y * k_size + x) * 4;
      const bool orange = p[0] == 255 && (p[1] == 127 || p[1] == 128) && p[2] == 0;
      const bool blue = p[0] == 0 && p[1] == 0 && p[2] == 255;
      if (orange) ++covered;
      if (!orange && !blue) ++wrong;
      CHECK(p[3] == 255);
    }
  }
  CHECK(wrong == 0);
  CHECK(covered == (k_size - 1) * k_size / 2);

  // The capture is a valid PNG. `Stored` is the size of the raw scanlines plus the container;
  // the default level compresses, and two flat triangles leave very little behind.
  Vector<u8> png;
  REQUIRE(image::encode_png(capture.width, capture.height, 4,
                            std::span<const u8>(rgba.data(), rgba.size()), png,
                            image::Compression::Stored));
  CHECK(png.size() == 8 + 25 + 12 + (2 + 5 + (k_size * 4 + 1) * k_size + 4) + 12);
  REQUIRE(image::encode_png(capture.width, capture.height, 4,
                            std::span<const u8>(rgba.data(), rgba.size()), png));
  CHECK(png.size() < (k_size * 4 + 1) * k_size / 4);

  // Depth captures come back as floats; an unsupported format is refused.
  gfx::ImageResource depth;
  REQUIRE(gfx::create_image_2d(
      device, 4, 4, gfx::Format::D32Sfloat,
      gfx::ImageUsage::DepthStencilAttachment | gfx::ImageUsage::TransferSrc, depth, &error));
  gfx::Capture depth_capture;
  CHECK(gfx::capture_image(device, depth, gfx::ImageLayout::Undefined, depth_capture, &error));
  CHECK(depth_capture.bytes.size() == 4 * 4 * 4);
  CHECK_FALSE(gfx::capture_to_rgba8(depth_capture, rgba));
  CHECK(gfx::capture_bytes_per_pixel(gfx::Format::Bc7Unorm) == 0);

  graph.reset();
  gfx::destroy_image(device, depth);
  gfx::destroy_image(device, target);
  gfx::destroy_pipeline(device, pipeline);
  gfx::destroy_shader_module(device, module);
  bindless.destroy();
  frames.destroy();
  device.destroy();
}
