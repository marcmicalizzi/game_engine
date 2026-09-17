#include <core/log/log.h>
#include <foundation/window/window.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace engine::window {

ENGINE_LOG_CATEGORY_DEFINE(log_window, "window");

namespace {

bool g_initialized = false;

Key key_from_scancode(SDL_Scancode code) noexcept {
  switch (code) {
    case SDL_SCANCODE_ESCAPE: return Key::Escape;
    case SDL_SCANCODE_SPACE: return Key::Space;
    case SDL_SCANCODE_RETURN: return Key::Enter;
    case SDL_SCANCODE_TAB: return Key::Tab;
    case SDL_SCANCODE_BACKSPACE: return Key::Backspace;
    case SDL_SCANCODE_LEFT: return Key::Left;
    case SDL_SCANCODE_RIGHT: return Key::Right;
    case SDL_SCANCODE_UP: return Key::Up;
    case SDL_SCANCODE_DOWN: return Key::Down;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT: return Key::Shift;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL: return Key::Control;
    case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_RALT: return Key::Alt;
    default: break;
  }
  if (code >= SDL_SCANCODE_A && code <= SDL_SCANCODE_Z) {
    return static_cast<Key>(static_cast<u16>(Key::A) + (code - SDL_SCANCODE_A));
  }
  if (code >= SDL_SCANCODE_1 && code <= SDL_SCANCODE_9) {
    return static_cast<Key>(static_cast<u16>(Key::Digit1) + (code - SDL_SCANCODE_1));
  }
  if (code == SDL_SCANCODE_0) return Key::Digit0;
  if (code >= SDL_SCANCODE_F1 && code <= SDL_SCANCODE_F12) {
    return static_cast<Key>(static_cast<u16>(Key::F1) + (code - SDL_SCANCODE_F1));
  }
  return Key::Unknown;
}

void set_error(std::string* error, const char* what) {
  if (error == nullptr) return;
  const char* reason = SDL_GetError();
  *error = std::string(what) + ": " + (reason != nullptr && reason[0] != '\0' ? reason : "unknown");
}

// --- gamepads -------------------------------------------------------------------------------
//
// SDL identifies a pad by an instance id that grows for the lifetime of the process; the engine
// wants a small stable slot instead, so this table maps one to the other. Slots are handed out
// lowest free first, which makes a single-pad session always slot 0 and a replay reproducible.
// The table is process-wide because SDL's gamepad events are: they carry no window.

bool g_gamepad_subsystem = false;

struct GamepadSlot {
  SDL_Gamepad* handle = nullptr;
  SDL_JoystickID instance = 0;
};

GamepadSlot g_gamepads[k_max_gamepads];

u32 find_gamepad_slot(SDL_JoystickID instance) noexcept {
  for (u32 i = 0; i < k_max_gamepads; ++i) {
    if (g_gamepads[i].handle != nullptr && g_gamepads[i].instance == instance) return i;
  }
  return k_max_gamepads;
}

// Opens a newly arrived pad into the lowest free slot. k_max_gamepads when it will not open or
// every slot is taken; the caller then drops the event.
u32 open_gamepad(SDL_JoystickID instance) {
  if (find_gamepad_slot(instance) != k_max_gamepads) return k_max_gamepads;  // already open
  u32 slot = k_max_gamepads;
  for (u32 i = 0; i < k_max_gamepads; ++i) {
    if (g_gamepads[i].handle == nullptr) {
      slot = i;
      break;
    }
  }
  if (slot == k_max_gamepads) {
    ENGINE_LOG_WARN(log_window, "gamepad ignored: every slot is taken",
                    log::field("slots", k_max_gamepads));
    return k_max_gamepads;
  }
  SDL_Gamepad* pad = SDL_OpenGamepad(instance);
  if (pad == nullptr) {
    ENGINE_LOG_WARN(log_window, "SDL_OpenGamepad failed", log::field("reason", SDL_GetError()));
    return k_max_gamepads;
  }
  g_gamepads[slot] = GamepadSlot{pad, instance};
  return slot;
}

void close_gamepad_slot(u32 slot) noexcept {
  if (slot >= k_max_gamepads || g_gamepads[slot].handle == nullptr) return;
  SDL_CloseGamepad(g_gamepads[slot].handle);
  g_gamepads[slot] = GamepadSlot{};
}

void close_all_gamepads() noexcept {
  for (u32 i = 0; i < k_max_gamepads; ++i)
    close_gamepad_slot(i);
}

GamepadButton button_from_sdl(u8 button) noexcept {
  switch (static_cast<int>(button)) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return GamepadButton::South;
    case SDL_GAMEPAD_BUTTON_EAST: return GamepadButton::East;
    case SDL_GAMEPAD_BUTTON_WEST: return GamepadButton::West;
    case SDL_GAMEPAD_BUTTON_NORTH: return GamepadButton::North;
    case SDL_GAMEPAD_BUTTON_BACK: return GamepadButton::Back;
    case SDL_GAMEPAD_BUTTON_GUIDE: return GamepadButton::Guide;
    case SDL_GAMEPAD_BUTTON_START: return GamepadButton::Start;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return GamepadButton::LeftStick;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return GamepadButton::RightStick;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return GamepadButton::LeftShoulder;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return GamepadButton::RightShoulder;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return GamepadButton::DpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return GamepadButton::DpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return GamepadButton::DpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return GamepadButton::DpadRight;
    // SDL routes a digital trigger click (the GameCube pad) to these two misc buttons.
    case SDL_GAMEPAD_BUTTON_MISC3: return GamepadButton::LeftTrigger;
    case SDL_GAMEPAD_BUTTON_MISC4: return GamepadButton::RightTrigger;
    default: return GamepadButton::Unknown;
  }
}

// The switch is on the integer rather than on SDL_GamepadAxis: SDL delivers the axis as a byte,
// and a byte outside the enum's range is not a value of that enumeration type.
GamepadAxis axis_from_sdl(u8 axis, bool& is_trigger) noexcept {
  switch (static_cast<int>(axis)) {
    case SDL_GAMEPAD_AXIS_LEFTX: is_trigger = false; return GamepadAxis::LeftX;
    case SDL_GAMEPAD_AXIS_LEFTY: is_trigger = false; return GamepadAxis::LeftY;
    case SDL_GAMEPAD_AXIS_RIGHTX: is_trigger = false; return GamepadAxis::RightX;
    case SDL_GAMEPAD_AXIS_RIGHTY: is_trigger = false; return GamepadAxis::RightY;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: is_trigger = true; return GamepadAxis::LeftTrigger;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: is_trigger = true; return GamepadAxis::RightTrigger;
    default: is_trigger = false; return GamepadAxis::Count;
  }
}

// SDL's axes are Sint16. A stick maps to -1..1 (the negative end is one step longer, so the
// division is by 32767 and the result clamped); a trigger rests at 0 and maps to 0..1.
f32 normalize_axis(i16 raw, bool is_trigger) noexcept {
  const f32 v = static_cast<f32>(raw) / 32767.0f;
  if (is_trigger) return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
  return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
}

}  // namespace

bool init(std::string* error) {
  if (g_initialized) return true;
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
    set_error(error, "SDL_Init(video)");
    return false;
  }
  g_initialized = true;
  const char* driver = SDL_GetCurrentVideoDriver();
  ENGINE_LOG_DEBUG(log_window, "video initialized",
                   log::field("driver", driver != nullptr ? driver : "none"));
  // Gamepads are optional: a machine with no joystick driver (a container, a service session)
  // still gets a window, and poll() simply never reports a pad.
  g_gamepad_subsystem = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
  if (!g_gamepad_subsystem) {
    ENGINE_LOG_WARN(log_window, "no gamepad subsystem", log::field("reason", SDL_GetError()));
  }
  return true;
}

void shutdown() noexcept {
  if (!g_initialized) return;
  close_all_gamepads();
  g_gamepad_subsystem = false;
  SDL_Quit();
  g_initialized = false;
}

const char* gamepad_button_name(GamepadButton button) noexcept {
  switch (button) {
    case GamepadButton::South: return "South";
    case GamepadButton::East: return "East";
    case GamepadButton::West: return "West";
    case GamepadButton::North: return "North";
    case GamepadButton::Back: return "Back";
    case GamepadButton::Guide: return "Guide";
    case GamepadButton::Start: return "Start";
    case GamepadButton::LeftStick: return "LeftStick";
    case GamepadButton::RightStick: return "RightStick";
    case GamepadButton::LeftShoulder: return "LeftShoulder";
    case GamepadButton::RightShoulder: return "RightShoulder";
    case GamepadButton::DpadUp: return "DpadUp";
    case GamepadButton::DpadDown: return "DpadDown";
    case GamepadButton::DpadLeft: return "DpadLeft";
    case GamepadButton::DpadRight: return "DpadRight";
    case GamepadButton::LeftTrigger: return "LeftTrigger";
    case GamepadButton::RightTrigger: return "RightTrigger";
    case GamepadButton::Unknown:
    case GamepadButton::Count: break;
  }
  return "Unknown";
}

const char* gamepad_axis_name(GamepadAxis axis) noexcept {
  switch (axis) {
    case GamepadAxis::LeftX: return "LeftX";
    case GamepadAxis::LeftY: return "LeftY";
    case GamepadAxis::RightX: return "RightX";
    case GamepadAxis::RightY: return "RightY";
    case GamepadAxis::LeftTrigger: return "LeftTrigger";
    case GamepadAxis::RightTrigger: return "RightTrigger";
    case GamepadAxis::Count: break;
  }
  return "Unknown";
}

const char* gamepad_name(u32 gamepad) noexcept {
  if (gamepad >= k_max_gamepads || g_gamepads[gamepad].handle == nullptr) return "";
  const char* name = SDL_GetGamepadName(g_gamepads[gamepad].handle);
  return name != nullptr ? name : "";
}

bool gamepad_connected(u32 gamepad) noexcept {
  return gamepad < k_max_gamepads && g_gamepads[gamepad].handle != nullptr;
}

bool initialized() noexcept { return g_initialized; }

Window::~Window() { destroy(); }

bool Window::create(const WindowDesc& desc, std::string* error) {
  ENGINE_VERIFY(handle_ == nullptr, "Window::create: already created");
  if (!g_initialized) {
    if (error != nullptr) *error = "window::init() has not succeeded";
    return false;
  }
  SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
  if (desc.resizable) flags |= SDL_WINDOW_RESIZABLE;
  if (desc.vulkan) flags |= SDL_WINDOW_VULKAN;
  if (desc.hidden) flags |= SDL_WINDOW_HIDDEN;
  SDL_Window* window = SDL_CreateWindow(desc.title, static_cast<int>(desc.width),
                                        static_cast<int>(desc.height), flags);
  if (window == nullptr) {
    set_error(error, "SDL_CreateWindow");
    return false;
  }
  handle_ = window;
  id_ = SDL_GetWindowID(window);
  refresh_pixel_size();
  ENGINE_LOG_DEBUG(log_window, "window created", log::field("width", pixel_width_),
                   log::field("height", pixel_height_));
  return true;
}

void Window::destroy() noexcept {
  if (handle_ == nullptr) return;
  SDL_DestroyWindow(static_cast<SDL_Window*>(handle_));
  handle_ = nullptr;
  id_ = 0;
}

void Window::refresh_pixel_size() noexcept {
  int w = 0;
  int h = 0;
  if (handle_ != nullptr) SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(handle_), &w, &h);
  pixel_width_ = w > 0 ? static_cast<u32>(w) : 0;
  pixel_height_ = h > 0 ? static_cast<u32>(h) : 0;
}

bool Window::poll(Event& out) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    out = Event{};
    switch (e.type) {
      case SDL_EVENT_QUIT: out.kind = EventKind::Quit; return true;
      // Gamepad events carry no window id, so they are reported to whichever window polls.
      case SDL_EVENT_GAMEPAD_ADDED: {
        const u32 slot = open_gamepad(e.gdevice.which);
        if (slot >= k_max_gamepads) continue;
        out.kind = EventKind::GamepadConnected;
        out.gamepad = static_cast<u8>(slot);
        ENGINE_LOG_INFO(log_window, "gamepad connected", log::field("slot", slot),
                        log::field("name", gamepad_name(slot)));
        return true;
      }
      case SDL_EVENT_GAMEPAD_REMOVED: {
        const u32 slot = find_gamepad_slot(e.gdevice.which);
        if (slot >= k_max_gamepads) continue;
        close_gamepad_slot(slot);
        out.kind = EventKind::GamepadDisconnected;
        out.gamepad = static_cast<u8>(slot);
        ENGINE_LOG_INFO(log_window, "gamepad disconnected", log::field("slot", slot));
        return true;
      }
      case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
      case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const u32 slot = find_gamepad_slot(e.gbutton.which);
        const GamepadButton mapped = button_from_sdl(e.gbutton.button);
        if (slot >= k_max_gamepads || mapped == GamepadButton::Unknown) continue;
        out.kind = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? EventKind::GamepadButtonDown
                                                           : EventKind::GamepadButtonUp;
        out.gamepad = static_cast<u8>(slot);
        out.gamepad_button = mapped;
        out.value = out.kind == EventKind::GamepadButtonDown ? 1.0f : 0.0f;
        return true;
      }
      case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const u32 slot = find_gamepad_slot(e.gaxis.which);
        bool is_trigger = false;
        const GamepadAxis mapped = axis_from_sdl(e.gaxis.axis, is_trigger);
        if (slot >= k_max_gamepads || mapped == GamepadAxis::Count) continue;
        out.kind = EventKind::GamepadAxis;
        out.gamepad = static_cast<u8>(slot);
        out.gamepad_axis = mapped;
        out.value = normalize_axis(e.gaxis.value, is_trigger);
        return true;
      }
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (e.window.windowID != id_) continue;
        out.kind = EventKind::CloseRequested;
        return true;
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        if (e.window.windowID != id_) continue;
        out.kind = EventKind::Resized;
        out.width = e.window.data1;
        out.height = e.window.data2;
        pixel_width_ = e.window.data1 > 0 ? static_cast<u32>(e.window.data1) : 0;
        pixel_height_ = e.window.data2 > 0 ? static_cast<u32>(e.window.data2) : 0;
        return true;
      case SDL_EVENT_WINDOW_FOCUS_GAINED:
        if (e.window.windowID != id_) continue;
        out.kind = EventKind::FocusGained;
        return true;
      case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (e.window.windowID != id_) continue;
        out.kind = EventKind::FocusLost;
        return true;
      case SDL_EVENT_KEY_DOWN:
      case SDL_EVENT_KEY_UP:
        if (e.key.windowID != id_) continue;
        out.kind = e.type == SDL_EVENT_KEY_DOWN ? EventKind::KeyDown : EventKind::KeyUp;
        out.key = key_from_scancode(e.key.scancode);
        out.scancode = static_cast<u16>(e.key.scancode);
        out.repeat = e.key.repeat;
        return true;
      case SDL_EVENT_MOUSE_MOTION:
        if (e.motion.windowID != id_) continue;
        out.kind = EventKind::MouseMove;
        out.x = e.motion.x;
        out.y = e.motion.y;
        out.dx = e.motion.xrel;
        out.dy = e.motion.yrel;
        return true;
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.windowID != id_) continue;
        out.kind = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? EventKind::MouseButtonDown
                                                         : EventKind::MouseButtonUp;
        out.button = e.button.button;
        out.x = e.button.x;
        out.y = e.button.y;
        return true;
      case SDL_EVENT_MOUSE_WHEEL:
        if (e.wheel.windowID != id_) continue;
        out.kind = EventKind::MouseWheel;
        out.x = e.wheel.mouse_x;
        out.y = e.wheel.mouse_y;
        out.dx = e.wheel.x;
        out.dy = e.wheel.y;
        return true;
      default: continue;
    }
  }
  return false;
}

void Window::set_title(const char* title) noexcept {
  if (handle_ != nullptr) SDL_SetWindowTitle(static_cast<SDL_Window*>(handle_), title);
}

void Window::show() noexcept {
  if (handle_ != nullptr) SDL_ShowWindow(static_cast<SDL_Window*>(handle_));
}

std::span<const char* const> Window::vulkan_instance_extensions() {
  Uint32 count = 0;
  const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
  if (names == nullptr) {
    ENGINE_LOG_WARN(log_window, "SDL has no Vulkan support here",
                    log::field("reason", SDL_GetError()));
    return {};
  }
  return std::span<const char* const>(names, count);
}

bool Window::create_vulkan_surface(VkInstance instance, VkSurfaceKHR& out,
                                   std::string* error) const {
  out = nullptr;
  if (handle_ == nullptr) {
    if (error != nullptr) *error = "Window::create_vulkan_surface: no window";
    return false;
  }
  if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(handle_), instance, nullptr, &out)) {
    set_error(error, "SDL_Vulkan_CreateSurface");
    out = nullptr;
    return false;
  }
  return true;
}

void Window::destroy_vulkan_surface(VkInstance instance, VkSurfaceKHR surface) noexcept {
  if (surface != nullptr) SDL_Vulkan_DestroySurface(instance, surface, nullptr);
}

}  // namespace engine::window
