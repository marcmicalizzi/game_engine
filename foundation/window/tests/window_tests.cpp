#include <core/containers/vector.h>
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

  // Draining also runs the gamepad and joystick paths: SDL reports every device already plugged
  // in as an arrival during init, so a machine with one arrives here as a Connected event with
  // a slot below the table's size and an open handle behind it. The two slot spaces are
  // separate, so a gamepad 0 and a joystick 0 may both show up.
  window::Event event;
  u32 drained = 0;
  u32 pads_seen = 0;
  u32 sticks_seen = 0;
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
    if (event.kind == window::EventKind::JoystickConnected) {
      CHECK(event.joystick < window::k_max_joysticks);
      CHECK(window::joystick_connected(event.joystick));
      MESSAGE("joystick " << static_cast<u32>(event.joystick) << ": "
                          << window::joystick_name(event.joystick));
      ++sticks_seen;
    }
    if (event.kind == window::EventKind::JoystickAxis) {
      CHECK(window::joystick_connected(event.joystick));
      CHECK(event.value >= -1.0f);
      CHECK(event.value <= 1.0f);
    }
    if (event.kind == window::EventKind::JoystickHat) {
      CHECK(window::joystick_connected(event.joystick));
      CHECK(static_cast<u32>(event.hat) <= 12);
      CHECK(std::strcmp(window::hat_direction_name(event.hat), "Unknown") != 0);
    }
    ++drained;
  }
  if (pads_seen == 0) MESSAGE("no gamepad is plugged in here");
  if (sticks_seen == 0) MESSAGE("no raw joystick is plugged in here");

  // Slots nothing has been plugged into report as empty rather than as a stale handle.
  for (u32 i = pads_seen; i < window::k_max_gamepads; ++i) {
    CHECK_FALSE(window::gamepad_connected(i));
    CHECK(std::strcmp(window::gamepad_name(i), "") == 0);
  }
  CHECK_FALSE(window::gamepad_connected(window::k_max_gamepads));
  for (u32 i = sticks_seen; i < window::k_max_joysticks; ++i) {
    CHECK_FALSE(window::joystick_connected(i));
    CHECK(std::strcmp(window::joystick_name(i), "") == 0);
  }
  CHECK_FALSE(window::joystick_connected(window::k_max_joysticks));

  // The description API needs no events: whatever is attached is listed, whether or not its
  // arrival has been polled. Having drained above, every listed device also has its slot.
  Vector<window::InputDeviceInfo> devices;
  const u32 device_count = window::input_devices(devices);
  CHECK(device_count == devices.size());
  CHECK(device_count >= pads_seen + sticks_seen);
  u32 listed_pads = 0;
  u32 listed_sticks = 0;
  for (const window::InputDeviceInfo& info : devices) {
    MESSAGE("device " << std::string(info.is_gamepad ? "gamepad" : "joystick") << " slot "
                      << static_cast<u32>(info.slot) << ": " << info.name << " [" << info.guid
                      << "] vendor " << info.vendor << " product " << info.product << ", "
                      << static_cast<u32>(info.axes) << " axes, " << static_cast<u32>(info.buttons)
                      << " buttons, " << static_cast<u32>(info.hats) << " hats");
    CHECK(info.guid.size() == 32);
    if (info.slot == window::k_invalid_slot) {
      // Attached but unslotted: it refused to open, or every slot of its kind is taken.
      MESSAGE("device has no slot: " << info.name);
      continue;
    }
    if (info.is_gamepad) {
      CHECK(window::gamepad_connected(info.slot));
      ++listed_pads;
    } else {
      CHECK(window::joystick_connected(info.slot));
      ++listed_sticks;
    }
  }
  CHECK(listed_pads == pads_seen);
  CHECK(listed_sticks == sticks_seen);

  // Rumble is harmless where it is unsupported and false for an empty slot.
  CHECK_FALSE(window::rumble(static_cast<u8>(window::k_max_gamepads), 0.5f, 0.5f, 10));
  for (u32 i = 0; i < pads_seen; ++i)
    (void)window::rumble(static_cast<u8>(i), 0.0f, 0.0f, 1);

  window.set_title("renamed");
  window.destroy();
  CHECK_FALSE(window.valid());
  window::shutdown();
  CHECK_FALSE(window::initialized());
  // shutdown() closes every device it opened.
  for (u32 i = 0; i < window::k_max_gamepads; ++i)
    CHECK_FALSE(window::gamepad_connected(i));
  for (u32 i = 0; i < window::k_max_joysticks; ++i)
    CHECK_FALSE(window::joystick_connected(i));
  devices.clear();
  CHECK(window::input_devices(devices) == 0);
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

  // Hat directions are a bitmask: a diagonal is the two cardinals or-ed, and the nine positions
  // are the only ones with names. input::k_hat_* mirrors these numbers.
  CHECK(static_cast<u32>(window::HatDirection::Centered) == 0);
  CHECK(static_cast<u32>(window::HatDirection::Up) == 1);
  CHECK(static_cast<u32>(window::HatDirection::Right) == 2);
  CHECK(static_cast<u32>(window::HatDirection::Down) == 4);
  CHECK(static_cast<u32>(window::HatDirection::Left) == 8);
  CHECK(static_cast<u32>(window::HatDirection::RightUp) == 3);
  CHECK(static_cast<u32>(window::HatDirection::RightDown) == 6);
  CHECK(static_cast<u32>(window::HatDirection::LeftUp) == 9);
  CHECK(static_cast<u32>(window::HatDirection::LeftDown) == 12);
  CHECK(std::strcmp(window::hat_direction_name(window::HatDirection::LeftDown), "LeftDown") == 0);
  CHECK(std::strcmp(window::hat_direction_name(static_cast<window::HatDirection>(5)), "Unknown") ==
        0);
  const window::HatDirection hats[] = {
      window::HatDirection::Centered,  window::HatDirection::Up,
      window::HatDirection::Right,     window::HatDirection::Down,
      window::HatDirection::Left,      window::HatDirection::RightUp,
      window::HatDirection::RightDown, window::HatDirection::LeftUp,
      window::HatDirection::LeftDown};
  for (u32 i = 0; i < 9; ++i) {
    const char* name = window::hat_direction_name(hats[i]);
    CHECK(std::strcmp(name, "Unknown") != 0);
    for (u32 j = 0; j < i; ++j)
      CHECK(std::strcmp(name, window::hat_direction_name(hats[j])) != 0);
  }
}
