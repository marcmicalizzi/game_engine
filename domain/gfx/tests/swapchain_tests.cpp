// Presentation end to end: a real window, a surface, a swapchain, frames acquired and
// presented through the frame context and the render graph, a resize, and a capture of the
// presented image. Skips on machines without a display or a Vulkan driver.
#include <domain/gfx/capture.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/render_graph.h>
#include <domain/gfx/swapchain.h>
#include <domain/gfx/vulkan.h>
#include <foundation/window/window.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;

TEST_CASE("swapchain: acquire, render, present, resize, capture") {
  std::string error;
  if (!window::init(&error)) {
    MESSAGE("no display: " << error);
    return;
  }
  const auto extensions = window::Window::vulkan_instance_extensions();
  if (extensions.empty()) {
    MESSAGE("SDL has no Vulkan support here");
    window::shutdown();
    return;
  }
  window::WindowDesc window_desc;
  window_desc.title = "engine swapchain test";
  window_desc.width = 160;
  window_desc.height = 120;
  window::Window window;
  if (!window.create(window_desc, &error)) {
    MESSAGE("cannot create a window here: " << error);
    window::shutdown();
    return;
  }

  gfx::DeviceOptions options;
  options.instance_extensions = extensions.data();
  options.instance_extension_count = static_cast<u32>(extensions.size());
  gfx::Device device;
  if (!device.create(options, &error)) {
    MESSAGE("device unavailable: " << error);
    window.destroy();
    window::shutdown();
    return;
  }
  REQUIRE(device.features().presentation);

  VkSurfaceKHR surface = VK_NULL_HANDLE;
  REQUIRE_MESSAGE(window.create_vulkan_surface(device.handles().instance, surface, &error), error);

  gfx::SwapchainDesc desc;
  desc.surface = surface;
  desc.width = window.pixel_width();
  desc.height = window.pixel_height();
  desc.vsync = false;
  gfx::Swapchain swapchain;
  REQUIRE_MESSAGE(swapchain.create(device, desc, &error), error);
  CHECK(swapchain.image_count() >= 2);
  CHECK(swapchain.extent().width >= 160);
  CHECK(swapchain.extent().height >= 120);
  CHECK(swapchain.format() != VK_FORMAT_UNDEFINED);
  MESSAGE("swapchain: " << swapchain.image_count() << " images, format " << swapchain.format()
                        << ", present mode " << swapchain.present_mode() << ", transfer_src "
                        << swapchain.transfer_src());

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::RenderGraph graph(device);

  // Six frames: clear the acquired image to a frame-dependent color and present it.
  auto render_frame = [&](u32 frame, u32& image_index) {
    VkCommandBuffer commands = frames.begin_frame();
    const gfx::PresentStatus acquired = swapchain.acquire(frames.acquire_semaphore(), image_index);
    if (acquired != gfx::PresentStatus::Ok) {
      frames.end_frame();
      return acquired;
    }
    graph.reset();
    const gfx::RgImage color = graph.import_image("swapchain", swapchain.image(image_index));
    VkClearColorValue clear{};
    clear.float32[0] = static_cast<f32>(frame) / 8.0f;
    clear.float32[1] = 0.25f;
    clear.float32[2] = 1.0f - static_cast<f32>(frame) / 8.0f;
    clear.float32[3] = 1.0f;
    graph.add_pass(
        "clear", gfx::PassKind::Raster,
        [&](gfx::PassBuilder& b) { b.color_attachment(color, VK_ATTACHMENT_LOAD_OP_CLEAR, clear); },
        [](VkCommandBuffer, gfx::RenderGraph&) {});
    graph.set_final_layout(color, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    std::string compile_error;
    REQUIRE_MESSAGE(graph.compile(&compile_error), compile_error);
    graph.execute(commands);
    gfx::FrameContext::PresentSync sync;
    sync.wait = frames.acquire_semaphore();
    sync.signal = swapchain.render_finished(image_index);
    frames.end_frame(sync);
    return swapchain.present(image_index, swapchain.render_finished(image_index));
  };

  u32 presented = 0;
  u32 image_index = 0;
  for (u32 frame = 0; frame < 6; ++frame) {
    window::Event event;
    while (window.poll(event)) {
    }
    const gfx::PresentStatus status = render_frame(frame, image_index);
    if (status == gfx::PresentStatus::OutOfDate) {
      REQUIRE(swapchain.resize(window.pixel_width(), window.pixel_height(), &error));
      continue;
    }
    REQUIRE_MESSAGE(status == gfx::PresentStatus::Ok, swapchain.last_error());
    ++presented;
  }
  CHECK(presented >= 4);

  // Resize to a new chain and present once more.
  frames.wait_idle();
  REQUIRE_MESSAGE(swapchain.resize(200, 150, &error), error);
  const gfx::PresentStatus after_resize = render_frame(7, image_index);
  CHECK(after_resize != gfx::PresentStatus::Error);

  // Capture the last presented image when the surface allows reading it back.
  if (after_resize == gfx::PresentStatus::Ok && swapchain.transfer_src()) {
    frames.wait_idle();
    gfx::Capture capture;
    REQUIRE_MESSAGE(gfx::capture_image(device, swapchain.image(image_index),
                                       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, capture, &error),
                    error);
    CHECK(capture.width == swapchain.extent().width);
    Vector<u8> rgba;
    if (gfx::capture_to_rgba8(capture, rgba)) {
      // Frame 7 clears to (7/8, 0.25, 1/8): red-dominant.
      const u8* p = rgba.data() + (capture.width * capture.height / 2) * 4;
      CHECK(p[0] > 200);
      CHECK(p[2] < 60);
    }
  }

  frames.wait_idle();
  graph.reset();
  swapchain.destroy();
  frames.destroy();
  window::Window::destroy_vulkan_surface(device.handles().instance, surface);
  device.destroy();
  window.destroy();
  window::shutdown();
}
