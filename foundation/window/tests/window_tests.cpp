#include <core/containers/vector.h>
#include <core/time/time.h>
#include <foundation/gpu_lock/device_hold.h>
#include <foundation/gpu_lock/gpu_lock.h>
#include <foundation/window/backend/vulkan/surface.h>
#include <foundation/window/window.h>

#include <doctest/doctest.h>
#include <test_environment.h>
#include <test_temp_dir.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

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
  const auto extensions = window::vulkan::instance_extensions();
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
                         << std::string(window::gamepad_name(event.gamepad)));
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
                          << std::string(window::joystick_name(event.joystick)));
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
                      << static_cast<u32>(info.slot) << ": " << info.name << " (driver: \""
                      << info.raw_name << "\") [" << info.guid << "] vendor " << info.vendor
                      << " product " << info.product << ", " << static_cast<u32>(info.axes)
                      << " axes, " << static_cast<u32>(info.buttons) << " buttons, "
                      << static_cast<u32>(info.hats) << " hats");
    CHECK(info.guid.size() == 32);
    // A device always ends up with a name, even when the driver gave none: the table's, the
    // driver's, or its ids. The unedited driver name stays beside it.
    CHECK_FALSE(info.name.empty());
    CHECK(info.name ==
          window::resolve_device_name(info.vendor, info.product, info.raw_name.c_str()));
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

  // Force feedback. Nothing here can feel one, so what is checked is that a slot with nothing
  // in it says no to every call, and that a device that does have haptics can be opened, read,
  // and handed back. No force is ever set: opening already stops a wheel's own centring, and
  // this closes again immediately so the driver takes it back.
  window::HapticInfo haptics;
  CHECK_FALSE(window::haptics_open(static_cast<u8>(window::k_max_joysticks)));
  CHECK_FALSE(window::haptics_info(static_cast<u8>(window::k_max_joysticks), haptics));
  for (u32 i = sticks_seen; i < window::k_max_joysticks; ++i) {
    const u8 slot = static_cast<u8>(i);
    CHECK_FALSE(window::haptics_open(slot));
    CHECK_FALSE(window::haptics_info(slot, haptics));
    CHECK_FALSE(window::set_spring(slot, 0.5f, 0.0f));
    CHECK_FALSE(window::set_damper(slot, 0.5f));
    CHECK_FALSE(window::set_constant_force(slot, 0.5f));
    CHECK_FALSE(window::stop_forces(slot));
    window::haptics_close(slot);  // and closing one that was never opened does nothing
  }
  for (u32 i = 0; i < sticks_seen; ++i) {
    const u8 slot = static_cast<u8>(i);
    if (!window::haptics_open(slot)) {
      MESSAGE("joystick " << i << " has no force feedback: "
                          << std::string(window::joystick_name(slot)));
      continue;
    }
    REQUIRE(window::haptics_info(slot, haptics));
    MESSAGE("joystick " << i << " haptics: " << std::string(window::joystick_name(slot))
                        << " constant " << haptics.constant << " spring " << haptics.spring
                        << " damper " << haptics.damper << " friction " << haptics.friction
                        << " sine " << haptics.sine << ", " << haptics.axes << " axes");
    CHECK(window::stop_forces(slot));
    window::haptics_close(slot);
    CHECK_FALSE(window::haptics_info(slot, haptics));
  }

  // Relative mouse mode: what SDL says is in effect follows what was asked, and turning it off
  // again leaves the window as it was. The window is hidden and has no focus, so SDL records the
  // mode without taking the pointer from whoever is at this machine.
  CHECK_FALSE(window.relative_mouse());
  if (window.set_relative_mouse(true)) {
    CHECK(window.relative_mouse());
    CHECK(window.set_relative_mouse(false));
  } else {
    MESSAGE("this platform refused relative mouse mode for a hidden window");
  }
  CHECK_FALSE(window.relative_mouse());

  // A synthetic event comes back out of poll() as the platform's own would, through the same
  // switch: a key by its scancode (with the `Key` the scancode maps to), motion as `dx`/`dy`.
  // Anything else is refused, since a gamepad slot belongs to a real device.
  window::Event key;
  key.kind = window::EventKind::KeyDown;
  key.scancode = 26;  // SDL_SCANCODE_W
  window::Event motion;
  motion.kind = window::EventKind::MouseMove;
  motion.dx = 12.5f;
  motion.dy = -3.0f;
  window::Event pad;
  pad.kind = window::EventKind::GamepadButtonDown;
  CHECK_FALSE(window.push_event(pad));
  if (window.push_event(key) && window.push_event(motion)) {
    bool saw_key = false;
    bool saw_motion = false;
    for (u32 i = 0; i < 1000 && window.poll(event); ++i) {
      if (event.kind == window::EventKind::KeyDown && event.scancode == 26) {
        saw_key = true;
        CHECK(event.key == window::Key::W);
        CHECK_FALSE(event.repeat);
      }
      if (event.kind == window::EventKind::MouseMove && event.dx == 12.5f) {
        saw_motion = true;
        CHECK(event.dy == -3.0f);
      }
    }
    CHECK(saw_key);
    CHECK(saw_motion);
  } else {
    MESSAGE("SDL's event queue refused a synthetic event here");
  }

  window.set_title("renamed");
  window.destroy();
  CHECK_FALSE(window.valid());
  CHECK_FALSE(window.set_relative_mouse(true));
  CHECK_FALSE(window.relative_mouse());
  CHECK_FALSE(window.push_event(key));
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
TEST_CASE("window: a borderless window is created at the size asked") {
  // `WindowDesc::borderless` (window.md, "Borderless windows and presentation"): no decorations,
  // at the primary display's corner. Hidden, so it covers nobody's desktop; what is checked is
  // that the flag creates a window of the size asked, which is what makes one the display's size
  // cover the display.
  std::string error;
  if (!window::init(&error)) {
    MESSAGE("no display: " << error);
    return;
  }
  window::WindowDesc desc;
  desc.title = "engine borderless test";
  desc.width = 96;
  desc.height = 64;
  desc.hidden = true;
  desc.borderless = true;
  window::Window window;
  if (!window.create(desc, &error)) {
    MESSAGE("cannot create a window here: " << error);
    window::shutdown();
    return;
  }
  CHECK(window.valid());
  CHECK(window.pixel_width() >= 96);
  CHECK(window.pixel_height() >= 64);
  window.destroy();
  window::shutdown();
}

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

// The name table, which needs no display and no device. Every string below is one a driver
// really produced on this project's hardware; content/input-logs/README.md says which.
TEST_CASE("window: the name table fixes what drivers call a device") {
  // The T300 RS: the Windows driver names it nothing at all and it arrives as its USB ids in
  // hex. That is the case the table exists for.
  CHECK(window::resolve_device_name(0x044F, 0xB66E, "44F B66E") == "Thrustmaster T300 RS");
  CHECK(window::resolve_device_name(0x044F, 0xB66E, "") == "Thrustmaster T300 RS");
  CHECK(window::resolve_device_name(0x044F, 0xB66E, nullptr) == "Thrustmaster T300 RS");
  CHECK(window::resolve_device_name(0x044F, 0xB66D, "") == "Thrustmaster T300 RS (PS4 mode)");

  // The F710 with its switch on XInput, where SDL can only say "XInput Controller #1"; and on
  // DirectInput, where SDL's own database already has the right answer and the table agrees.
  CHECK(window::resolve_device_name(0x046D, 0xC21F, "XInput Controller #1") ==
        "Logitech F710 Gamepad");
  CHECK(window::resolve_device_name(0x046D, 0xC219, "Logitech F710 Gamepad") ==
        "Logitech F710 Gamepad");
  CHECK(window::resolve_device_name(0x046D, 0xC216, "") == "Logitech F310 Gamepad");
  CHECK(window::resolve_device_name(0x044F, 0xB10A, "") == "Thrustmaster T.16000M");

  // The gear shift reports its own name with a typo in it, and no ids were ever captured for
  // it, so its row matches that misspelling — under its vendor, and under nobody else's.
  CHECK(window::resolve_device_name(0x044F, 0xB65A, "Thustmaster T500 RS Gear Shift") ==
        "Thrustmaster T500 RS Gear Shift");
  CHECK(window::resolve_device_name(0x046D, 0xB65A, "Thustmaster T500 RS Gear Shift") ==
        "Thustmaster T500 RS Gear Shift");

  // A device the table has never heard of keeps whatever the driver called it...
  CHECK(window::known_device_name(0x1234, 0x5678, "Some Pad") == nullptr);
  CHECK(window::resolve_device_name(0x1234, 0x5678, "Some Pad") == "Some Pad");
  // ...and one nothing named at all still gets something to be told apart by.
  CHECK(window::resolve_device_name(0x1234, 0x5678, "") == "1234 5678");
  CHECK(window::resolve_device_name(0x1234, 0x5678, nullptr) == "1234 5678");
  CHECK(window::resolve_device_name(0, 0, nullptr) == "Unknown device");
  CHECK(window::resolve_device_name(0, 0, "Keyboard-ish thing") == "Keyboard-ish thing");
}

// A Vulkan window takes its process's hold on the machine-wide GPU lock before it appears
// (WindowDesc::vulkan; docs/subsystems/gpu_lock.md, "Windows"). Waiting with a window on screen is
// what put two test engine-views "Not responding" on the owner's desktop for a quarter of an hour
// on 2026-09-30, so this case fails if a window of this process is on screen at any moment while
// the lock it is waiting for is still somebody else's. The lock is a scratch one, left by a
// holder whose lease runs out three seconds in, which the wait then breaks.
TEST_CASE("window: a Vulkan window is not on screen while its process waits for the GPU lock") {
#if ENGINE_PLATFORM_WINDOWS
  std::string error;
  if (!window::init(&error)) {
    MESSAGE("no display: " << error);
    return;
  }
  const test::TempDir dir("window_gpu_lock");
  const std::string path = dir.file("gpu.lock");
  const test::ScopedEnv lock("ENGINE_GPU_LOCK", path);
  const test::ScopedEnv on(gpu_lock::k_env_on_device, "1");
  const i64 now = time::wall_unix_ms() / 1000;
  const test::ScopedEnv until(gpu_lock::k_env_wait_until, std::to_string(now + 60));
  const test::ScopedEnv holder(gpu_lock::k_env_holder, "");
  const test::ScopedEnv hold_log(gpu_lock::k_env_log, "");
  const gpu_lock::Identity self = gpu_lock::current_identity();
  const std::string theirs =
      "{\"owner\":\"astra-blender\",\"purpose\":\"a render\",\"pid\":4321,"
      "\"started\":\"" +
      gpu_lock::format_iso8601_utc(now - 60) + "\",\"expires\":\"" +
      gpu_lock::format_iso8601_utc(now + 3) + "\",\"host\":\"" + self.host + "\"}";
  {
    std::ofstream out(path, std::ios::binary);
    out << theirs;
  }
  auto file_text = [&] {
    std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
  };
  // This process's top-level windows that are on screen.
  auto visible_windows = [] {
    struct Count {
      DWORD pid;
      int visible;
    } count{::GetCurrentProcessId(), 0};
    ::EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
          auto* c = reinterpret_cast<Count*>(param);
          DWORD owner = 0;
          ::GetWindowThreadProcessId(hwnd, &owner);
          if (owner == c->pid && ::IsWindowVisible(hwnd)) ++c->visible;
          return TRUE;
        },
        reinterpret_cast<LPARAM>(&count));
    return count.visible;
  };
  REQUIRE(visible_windows() == 0);

  std::atomic<bool> done{false};
  std::atomic<int> looks_while_theirs{0};
  std::atomic<int> windows_while_theirs{0};
  std::thread watcher([&] {
    while (!done.load()) {
      if (file_text() == theirs) {
        ++looks_while_theirs;
        if (visible_windows() > 0) ++windows_while_theirs;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  });
  window::WindowDesc desc;
  desc.title = "engine gpu lock test";
  desc.width = 96;
  desc.height = 64;
  desc.resizable = false;
  desc.vulkan = true;
  window::Window window;
  const bool created = window.create(desc, &error);
  done.store(true);
  watcher.join();
  if (!created) {
    MESSAGE("cannot create a window here: " << error);
    window::shutdown();
    return;
  }
  CHECK(looks_while_theirs.load() > 10);  // it did wait, for about three seconds
  CHECK(windows_while_theirs.load() == 0);
  CHECK(visible_windows() >= 1);
  CHECK(gpu_lock::read(path, self, time::wall_unix_ms() / 1000).pid == self.pid);
  window.destroy();
  std::error_code ec;
  CHECK_FALSE(std::filesystem::exists(std::filesystem::path(path), ec));  // released with it
  window::shutdown();
#else
  MESSAGE("counts the process's windows through the Win32 API; checked on Windows");
#endif
}
