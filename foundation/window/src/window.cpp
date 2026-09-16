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
  return true;
}

void shutdown() noexcept {
  if (!g_initialized) return;
  SDL_Quit();
  g_initialized = false;
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
