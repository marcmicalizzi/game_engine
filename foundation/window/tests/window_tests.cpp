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
  if (!window.create(desc, &error)) {
    // Service sessions and some virtual desktops refuse windows or Vulkan-capable windows.
    MESSAGE("cannot create a window here: " << error);
    window::shutdown();
    return;
  }
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

  // Draining also runs the gamepad path: SDL reports every pad already plugged in as an
  // SDL_EVENT_GAMEPAD_ADDED during init, so a machine with one arrives here as a Connected
  // event with a slot below k_max_gamepads and an open handle behind it.
  window::Event event;
  u32 drained = 0;
  u32 pads_seen = 0;
  while (window.poll(event) && drained < 1000) {
    CHECK(event.kind != window::EventKind::None);
    if (event.kind == window::EventKind::GamepadConnected) {
      CHECK(event.gamepad < window::k_max_gamepads);
      CHECK(window::gamepad_connected(event.gamepad));
      MESSAGE("gamepad " << static_cast<u32>(event.gamepad) << ": "
                         << window::gamepad_name(event.gamepad));
      ++pads_seen;
    }
    if (event.kind == window::EventKind::GamepadButtonDown ||
        event.kind == window::EventKind::GamepadButtonUp) {
      CHECK(event.gamepad_button != window::GamepadButton::Unknown);
      CHECK(event.gamepad_button < window::GamepadButton::Count);
    }
    if (event.kind == window::EventKind::GamepadAxis) {
      CHECK(event.gamepad_axis < window::GamepadAxis::Count);
      CHECK(event.value >= -1.0f);
      CHECK(event.value <= 1.0f);
    }
    ++drained;
  }
  if (pads_seen == 0) MESSAGE("no gamepad is plugged in here");

  // Slots nothing has been plugged into report as empty rather than as a stale handle.
  for (u32 i = pads_seen; i < window::k_max_gamepads; ++i) {
    CHECK_FALSE(window::gamepad_connected(i));
    CHECK(std::strcmp(window::gamepad_name(i), "") == 0);
  }
  CHECK_FALSE(window::gamepad_connected(window::k_max_gamepads));

  window.set_title("renamed");
  window.destroy();
  CHECK_FALSE(window.valid());
  window::shutdown();
  CHECK_FALSE(window::initialized());
  // shutdown() closes every pad it opened.
  for (u32 i = 0; i < window::k_max_gamepads; ++i)
    CHECK_FALSE(window::gamepad_connected(i));
}

// The button and axis enums are the input log's vocabulary (docs/subsystems/input.md), so their
// names and numbering are pinned here: no display and no gamepad are needed to check them.
TEST_CASE("window: the gamepad enums are named and numbered for the input log") {
  CHECK(static_cast<u32>(window::GamepadButton::Unknown) == 0);
  CHECK(static_cast<u32>(window::GamepadButton::South) == 1);
  CHECK(static_cast<u32>(window::GamepadButton::DpadRight) == 15);
  CHECK(static_cast<u32>(window::GamepadButton::Count) == 18);
  CHECK(static_cast<u32>(window::GamepadAxis::LeftX) == 0);
  CHECK(static_cast<u32>(window::GamepadAxis::RightTrigger) == 5);
  CHECK(static_cast<u32>(window::GamepadAxis::Count) == 6);

  CHECK(std::strcmp(window::gamepad_button_name(window::GamepadButton::South), "South") == 0);
  CHECK(std::strcmp(window::gamepad_button_name(window::GamepadButton::LeftShoulder),
                    "LeftShoulder") == 0);
  CHECK(std::strcmp(window::gamepad_button_name(window::GamepadButton::Unknown), "Unknown") == 0);
  CHECK(std::strcmp(window::gamepad_axis_name(window::GamepadAxis::LeftY), "LeftY") == 0);
  CHECK(std::strcmp(window::gamepad_axis_name(window::GamepadAxis::RightTrigger), "RightTrigger") ==
        0);

  // Every enumerator has a name of its own: no two share one, and none falls through.
  for (u32 i = 1; i < static_cast<u32>(window::GamepadButton::Count); ++i) {
    const char* name = window::gamepad_button_name(static_cast<window::GamepadButton>(i));
    CHECK(std::strcmp(name, "Unknown") != 0);
    for (u32 j = 1; j < i; ++j) {
      CHECK(std::strcmp(name, window::gamepad_button_name(static_cast<window::GamepadButton>(j))) !=
            0);
    }
  }
  for (u32 i = 0; i < static_cast<u32>(window::GamepadAxis::Count); ++i) {
    CHECK(std::strcmp(window::gamepad_axis_name(static_cast<window::GamepadAxis>(i)), "Unknown") !=
          0);
  }
}
