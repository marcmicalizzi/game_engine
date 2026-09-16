#include <foundation/window/window.h>

#include <doctest/doctest.h>

#include <cstring>
#include <string>

using namespace engine;

TEST_CASE("window: create a hidden window, read its size, drain events") {
  std::string error;
  if (!window::init(&error)) {
    MESSAGE("no display: " << error);
    CHECK_FALSE(window::initialized());
    window::Window none;
    CHECK_FALSE(none.create(window::WindowDesc{}, &error));
    CHECK_FALSE(error.empty());
    return;
  }
  CHECK(window::initialized());

  window::WindowDesc desc;
  desc.title = "engine window test";
  desc.width = 96;
  desc.height = 64;
  desc.hidden = true;
  desc.resizable = false;
  window::Window window;
  REQUIRE_MESSAGE(window.create(desc, &error), error);
  CHECK(window.valid());
  CHECK(window.native() != nullptr);
  CHECK(window.pixel_width() >= 96);  // high-DPI scaling can only make it larger
  CHECK(window.pixel_height() >= 64);

  // The surface extensions always include VK_KHR_surface when SDL has a Vulkan loader.
  const auto extensions = window::Window::vulkan_instance_extensions();
  bool has_surface = false;
  for (const char* name : extensions) {
    if (std::strcmp(name, "VK_KHR_surface") == 0) has_surface = true;
  }
  if (extensions.empty()) {
    MESSAGE("SDL reports no Vulkan support on this machine");
  } else {
    CHECK(has_surface);
  }

  window::Event event;
  u32 drained = 0;
  while (window.poll(event) && drained < 1000) {
    CHECK(event.kind != window::EventKind::None);
    ++drained;
  }
  window.set_title("renamed");
  window.destroy();
  CHECK_FALSE(window.valid());
  window::shutdown();
  CHECK_FALSE(window::initialized());
}
