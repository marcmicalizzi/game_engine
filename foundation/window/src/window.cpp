#include <core/log/log.h>
#include <foundation/window/window.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <utility>

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

// --- raw joysticks --------------------------------------------------------------------------
//
// SDL announces every device as a joystick, and the subset it has a mapping for a second time as
// a gamepad. A device with a mapping belongs to the table above, which reports it by its named
// buttons and axes; everything else — wheels, pedal boxes, shifters, flight sticks — lands here
// and is reported by bare index, because there is nothing to name it by. The slot space is this
// table's own: joystick 0 and gamepad 0 are two different devices.

bool g_joystick_subsystem = false;

struct JoystickSlot {
  SDL_Joystick* handle = nullptr;
  SDL_JoystickID instance = 0;
};

JoystickSlot g_joysticks[k_max_joysticks];

u32 find_joystick_slot(SDL_JoystickID instance) noexcept {
  for (u32 i = 0; i < k_max_joysticks; ++i) {
    if (g_joysticks[i].handle != nullptr && g_joysticks[i].instance == instance) return i;
  }
  return k_max_joysticks;
}

// Opens a newly arrived joystick into the lowest free slot. k_max_joysticks when SDL also has a
// gamepad mapping for it (the gamepad table owns it then), when it will not open, or when every
// slot is taken; the caller drops the event.
u32 open_joystick(SDL_JoystickID instance) {
  if (SDL_IsGamepad(instance)) return k_max_joysticks;
  if (find_joystick_slot(instance) != k_max_joysticks) return k_max_joysticks;  // already open
  u32 slot = k_max_joysticks;
  for (u32 i = 0; i < k_max_joysticks; ++i) {
    if (g_joysticks[i].handle == nullptr) {
      slot = i;
      break;
    }
  }
  if (slot == k_max_joysticks) {
    ENGINE_LOG_WARN(log_window, "joystick ignored: every slot is taken",
                    log::field("slots", k_max_joysticks));
    return k_max_joysticks;
  }
  SDL_Joystick* stick = SDL_OpenJoystick(instance);
  if (stick == nullptr) {
    ENGINE_LOG_WARN(log_window, "SDL_OpenJoystick failed", log::field("reason", SDL_GetError()));
    return k_max_joysticks;
  }
  g_joysticks[slot] = JoystickSlot{stick, instance};
  return slot;
}

void close_joystick_slot(u32 slot) noexcept {
  if (slot >= k_max_joysticks || g_joysticks[slot].handle == nullptr) return;
  SDL_CloseJoystick(g_joysticks[slot].handle);
  g_joysticks[slot] = JoystickSlot{};
}

void close_all_joysticks() noexcept {
  for (u32 i = 0; i < k_max_joysticks; ++i)
    close_joystick_slot(i);
}

// SDL's hat value is already the four-bit mask; anything above those bits would not be one of
// the nine positions, so it is masked off rather than reported as a new direction.
HatDirection hat_from_sdl(u8 value) noexcept { return static_cast<HatDirection>(value & 0x0Fu); }

u8 count_to_u8(int count) noexcept {
  if (count <= 0) return 0;
  return count > 255 ? u8{255} : static_cast<u8>(count);
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
  // SDL_INIT_GAMEPAD implies SDL_INIT_JOYSTICK, so this usually only takes a second reference;
  // it is asked for separately so that a machine whose gamepad mappings fail to load still
  // reports its wheels and sticks as raw joysticks.
  g_joystick_subsystem = SDL_InitSubSystem(SDL_INIT_JOYSTICK);
  if (!g_joystick_subsystem) {
    ENGINE_LOG_WARN(log_window, "no joystick subsystem", log::field("reason", SDL_GetError()));
  }
  return true;
}

void set_background_input(bool enabled) noexcept {
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, enabled ? "1" : "0");
}

void shutdown() noexcept {
  if (!g_initialized) return;
  close_all_gamepads();
  close_all_joysticks();
  g_gamepad_subsystem = false;
  g_joystick_subsystem = false;
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

const char* hat_direction_name(HatDirection hat) noexcept {
  switch (hat) {
    case HatDirection::Centered: return "Centered";
    case HatDirection::Up: return "Up";
    case HatDirection::Right: return "Right";
    case HatDirection::Down: return "Down";
    case HatDirection::Left: return "Left";
    case HatDirection::RightUp: return "RightUp";
    case HatDirection::RightDown: return "RightDown";
    case HatDirection::LeftUp: return "LeftUp";
    case HatDirection::LeftDown: return "LeftDown";
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

const char* joystick_name(u32 joystick) noexcept {
  if (joystick >= k_max_joysticks || g_joysticks[joystick].handle == nullptr) return "";
  const char* name = SDL_GetJoystickName(g_joysticks[joystick].handle);
  return name != nullptr ? name : "";
}

bool joystick_connected(u32 joystick) noexcept {
  return joystick < k_max_joysticks && g_joysticks[joystick].handle != nullptr;
}

u32 input_devices(Vector<InputDeviceInfo>& out) {
  out.clear();
  if (!g_initialized) return 0;
  int count = 0;
  SDL_JoystickID* ids = SDL_GetJoysticks(&count);
  if (ids == nullptr) return 0;
  for (int i = 0; i < count; ++i) {
    const SDL_JoystickID id = ids[i];
    InputDeviceInfo info;
    info.is_gamepad = SDL_IsGamepad(id);
    const char* name = info.is_gamepad ? SDL_GetGamepadNameForID(id) : SDL_GetJoystickNameForID(id);
    if (name != nullptr) info.name.assign(name);
    char guid[33] = {};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, static_cast<int>(sizeof(guid)));
    info.guid.assign(guid);
    info.vendor = SDL_GetJoystickVendorForID(id);
    info.product = SDL_GetJoystickProductForID(id);

    // The axis, button, and hat counts need an open device. SDL reference-counts opening, so
    // this hands back the handle the gamepad table or the joystick table already holds and the
    // matching close only drops this function's reference; a device nothing has opened is
    // opened and closed here without disturbing the event stream.
    if (SDL_Joystick* stick = SDL_OpenJoystick(id); stick != nullptr) {
      info.axes = count_to_u8(SDL_GetNumJoystickAxes(stick));
      info.buttons = count_to_u8(SDL_GetNumJoystickButtons(stick));
      info.hats = count_to_u8(SDL_GetNumJoystickHats(stick));
      SDL_CloseJoystick(stick);
    }
    const u32 slot = info.is_gamepad ? find_gamepad_slot(id) : find_joystick_slot(id);
    const u32 limit = info.is_gamepad ? k_max_gamepads : k_max_joysticks;
    info.slot = slot < limit ? static_cast<u8>(slot) : k_invalid_slot;
    out.push_back(std::move(info));
  }
  SDL_free(ids);
  return out.size();
}

bool rumble(u8 gamepad, f32 low, f32 high, u32 ms) noexcept {
  if (gamepad >= k_max_gamepads || g_gamepads[gamepad].handle == nullptr) return false;
  auto motor = [](f32 v) -> Uint16 {
    if (!(v > 0.0f)) return 0;  // also catches NaN
    if (v > 1.0f) v = 1.0f;
    return static_cast<Uint16>(v * 65535.0f + 0.5f);
  };
  return SDL_RumbleGamepad(g_gamepads[gamepad].handle, motor(low), motor(high), ms);
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
      // Raw joysticks. SDL sends these for every open device, including the ones the gamepad
      // table owns, so an instance that is not in the joystick table is skipped here and
      // reported by the gamepad cases above instead.
      case SDL_EVENT_JOYSTICK_ADDED: {
        const u32 slot = open_joystick(e.jdevice.which);
        if (slot >= k_max_joysticks) continue;
        out.kind = EventKind::JoystickConnected;
        out.joystick = static_cast<u8>(slot);
        ENGINE_LOG_INFO(log_window, "joystick connected", log::field("slot", slot),
                        log::field("name", joystick_name(slot)));
        return true;
      }
      case SDL_EVENT_JOYSTICK_REMOVED: {
        const u32 slot = find_joystick_slot(e.jdevice.which);
        if (slot >= k_max_joysticks) continue;
        close_joystick_slot(slot);
        out.kind = EventKind::JoystickDisconnected;
        out.joystick = static_cast<u8>(slot);
        ENGINE_LOG_INFO(log_window, "joystick disconnected", log::field("slot", slot));
        return true;
      }
      case SDL_EVENT_JOYSTICK_AXIS_MOTION: {
        const u32 slot = find_joystick_slot(e.jaxis.which);
        if (slot >= k_max_joysticks) continue;
        out.kind = EventKind::JoystickAxis;
        out.joystick = static_cast<u8>(slot);
        out.index = e.jaxis.axis;
        // No layout, so no trigger convention: every axis is -1..1 as the device reports it.
        // A pedal that rests at one end reads -1 at rest, which is the device's truth and the
        // binding's problem (input::Binding::scale).
        out.value = normalize_axis(e.jaxis.value, false);
        return true;
      }
      case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
      case SDL_EVENT_JOYSTICK_BUTTON_UP: {
        const u32 slot = find_joystick_slot(e.jbutton.which);
        if (slot >= k_max_joysticks) continue;
        out.kind = e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN ? EventKind::JoystickButtonDown
                                                            : EventKind::JoystickButtonUp;
        out.joystick = static_cast<u8>(slot);
        out.index = e.jbutton.button;
        out.value = out.kind == EventKind::JoystickButtonDown ? 1.0f : 0.0f;
        return true;
      }
      case SDL_EVENT_JOYSTICK_HAT_MOTION: {
        const u32 slot = find_joystick_slot(e.jhat.which);
        if (slot >= k_max_joysticks) continue;
        out.kind = EventKind::JoystickHat;
        out.joystick = static_cast<u8>(slot);
        out.index = e.jhat.hat;
        out.hat = hat_from_sdl(e.jhat.value);
        out.value = static_cast<f32>(static_cast<u8>(out.hat));
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
