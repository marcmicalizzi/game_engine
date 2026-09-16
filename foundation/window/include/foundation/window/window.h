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

// Starts the video and event subsystems. False, with the reason, on a machine without a
// display server or video driver; nothing else in this module may be used then.
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
};

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
