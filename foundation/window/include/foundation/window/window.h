#pragma once

// Windows and input over SDL3 (ADR-0012), and the two pieces of Vulkan glue a window system
// owns: the instance extensions a surface needs and surface creation. The rest of the engine
// never sees SDL. Everything here is process-wide and main-thread only.
//
//     if (!window::init(&error)) { /* no display: run headless */ }
//     window::Window window;
//     window.create({.title = "view", .width = 1280, .height = 720}, &error);
//     gfx::DeviceOptions options;
//     options.instance_extensions = window::Window::vulkan_instance_extensions();
//     ...
//     window::Event event;
//     while (window.poll(event)) { if (event.kind == window::EventKind::Quit) running = false; }
//
// Vulkan handle types are forward-declared the way SDL does it, so this header pulls in
// neither SDL nor the Vulkan headers; on 64-bit targets the typedefs match vulkan_core.h.

#include <core/base/macros.h>
#include <core/base/types.h>

#include <span>
#include <string>

#if !defined(VULKAN_CORE_H_)
typedef struct VkInstance_T* VkInstance;
typedef struct VkSurfaceKHR_T* VkSurfaceKHR;
#endif

namespace engine::window {

// Starts the video and event subsystems, and the gamepad subsystem when the platform has one
// (a machine with no gamepad driver still gets a window; the failure is logged, not returned).
// False, with the reason, on a machine without a display server or video driver; nothing else
// in this module may be used then.
bool init(std::string* error = nullptr);
void shutdown() noexcept;
bool initialized() noexcept;

enum class Key : u16 {
  Unknown = 0,
  Escape,
  Space,
  Enter,
  Tab,
  Backspace,
  Left,
  Right,
  Up,
  Down,
  Shift,
  Control,
  Alt,
  A,
  B,
  C,
  D,
  E,
  F,
  G,
  H,
  I,
  J,
  K,
  L,
  M,
  N,
  O,
  P,
  Q,
  R,
  S,
  T,
  U,
  V,
  W,
  X,
  Y,
  Z,
  Digit0,
  Digit1,
  Digit2,
  Digit3,
  Digit4,
  Digit5,
  Digit6,
  Digit7,
  Digit8,
  Digit9,
  F1,
  F2,
  F3,
  F4,
  F5,
  F6,
  F7,
  F8,
  F9,
  F10,
  F11,
  F12,
};

// How many gamepads this module tracks at once. Slots are handed out lowest free first and
// released on disconnect, so `Event::gamepad` is always below this.
inline constexpr u32 k_max_gamepads = 8;

// The standard gamepad layout, in SDL's own order. Face buttons are named by position, not by
// label, so the same enumerator is the bottom button on every pad (Xbox A, PlayStation cross,
// Nintendo B); a game that wants the printed label asks the platform layer, not this enum.
//
// The numeric values are part of the input log format (docs/subsystems/input.md): append new
// enumerators, never reorder or renumber them.
enum class GamepadButton : u8 {
  Unknown = 0,
  South,  // bottom face button
  East,   // right face button
  West,   // left face button
  North,  // top face button
  Back,
  Guide,
  Start,
  LeftStick,  // stick pressed in
  RightStick,
  LeftShoulder,
  RightShoulder,
  DpadUp,
  DpadDown,
  DpadLeft,
  DpadRight,
  // Only pads whose triggers are digital report these (SDL maps the GameCube trigger click
  // here). Analog triggers arrive as GamepadAxis::LeftTrigger/RightTrigger instead, and a game
  // that wants a digital trigger from them binds the axis with a deadzone.
  LeftTrigger,
  RightTrigger,
  Count,
};

// Gamepad axes. Sticks are -1..1 with +x right and +y down (SDL's sign convention, so a stick
// pushed forward reads negative y); triggers are 0..1. Numeric values are part of the input
// log format: append, never reorder.
enum class GamepadAxis : u8 {
  LeftX = 0,
  LeftY,
  RightX,
  RightY,
  LeftTrigger,
  RightTrigger,
  Count,
};

enum class EventKind : u8 {
  None = 0,
  Quit,            // the application was asked to quit (last window closed, SIGINT, ...)
  CloseRequested,  // this window's close button
  Resized,         // pixel size changed; width/height hold the new size
  FocusGained,
  FocusLost,
  KeyDown,
  KeyUp,
  MouseMove,
  MouseButtonDown,
  MouseButtonUp,
  MouseWheel,
  // Gamepads are process-wide, not per-window: SDL reports no window for them, so whichever
  // window is polling receives them. `gamepad` holds this module's slot id in every one.
  GamepadConnected,
  GamepadDisconnected,
  GamepadButtonDown,
  GamepadButtonUp,
  GamepadAxis,
};

struct Event {
  EventKind kind = EventKind::None;
  Key key = Key::Unknown;  // KeyDown/KeyUp
  u16 scancode = 0;        // KeyDown/KeyUp: the raw SDL scancode for keys outside `Key`
  u8 button = 0;           // MouseButtonDown/Up: 1 left, 2 middle, 3 right, 4/5 extra
  bool repeat = false;     // KeyDown: held-key repeat
  i32 width = 0;           // Resized
  i32 height = 0;
  f32 x = 0.0f;  // mouse position in window pixels (MouseMove, MouseButton*, MouseWheel)
  f32 y = 0.0f;
  f32 dx = 0.0f;  // MouseMove: relative motion; MouseWheel: horizontal/vertical scroll
  f32 dy = 0.0f;
  u8 gamepad = 0;  // Gamepad*: the slot id this module assigned, 0..k_max_gamepads-1
  GamepadButton gamepad_button = GamepadButton::Unknown;  // GamepadButtonDown/Up
  GamepadAxis gamepad_axis = GamepadAxis::LeftX;          // GamepadAxis
  f32 value = 0.0f;  // GamepadAxis: -1..1 for sticks, 0..1 for triggers
};

// "South", "LeftTrigger", ... for logs and tools; "Unknown" outside the enum.
const char* gamepad_button_name(GamepadButton button) noexcept;
const char* gamepad_axis_name(GamepadAxis axis) noexcept;
// The pad's product name ("Xbox Series X Controller"), or "" for a slot with nothing in it.
// Valid until that slot disconnects.
const char* gamepad_name(u32 gamepad) noexcept;
bool gamepad_connected(u32 gamepad) noexcept;

struct WindowDesc {
  const char* title = "engine";
  u32 width = 1280;  // logical size; the pixel size may differ on high-DPI displays
  u32 height = 720;
  bool resizable = true;
  bool vulkan = true;  // create with Vulkan surface support
  bool hidden = false;
};

class Window {
 public:
  Window() noexcept = default;
  ~Window();
  ENGINE_NON_COPYABLE(Window);

  bool create(const WindowDesc& desc, std::string* error = nullptr);
  void destroy() noexcept;
  bool valid() const noexcept { return handle_ != nullptr; }

  // Framebuffer size in pixels, refreshed on every Resized event.
  u32 pixel_width() const noexcept { return pixel_width_; }
  u32 pixel_height() const noexcept { return pixel_height_; }
  void refresh_pixel_size() noexcept;

  // Pulls the next pending event for this window (and process-wide Quit). False when none
  // is left; call until it returns false once per frame.
  bool poll(Event& out);

  void set_title(const char* title) noexcept;
  void show() noexcept;
  void* native() const noexcept { return handle_; }  // SDL_Window*

  // Instance extensions the platform surface needs; empty (with SDL's reason logged) when
  // Vulkan is unavailable to SDL. Valid after init().
  static std::span<const char* const> vulkan_instance_extensions();
  bool create_vulkan_surface(VkInstance instance, VkSurfaceKHR& out,
                             std::string* error = nullptr) const;
  static void destroy_vulkan_surface(VkInstance instance, VkSurfaceKHR surface) noexcept;

 private:
  void* handle_ = nullptr;
  u32 id_ = 0;
  u32 pixel_width_ = 0;
  u32 pixel_height_ = 0;
};

}  // namespace engine::window
