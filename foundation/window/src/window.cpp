#include <core/log/log.h>
#include <foundation/window/backend/vulkan/surface.h>
#include <foundation/window/window.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h, which WIN32_LEAN_AND_MEAN keeps from including it: open_file_dialog.
#include <commdlg.h>
// The session's lock state, for desktop_state().
#include <wtsapi32.h>
#endif

namespace engine::window {

ENGINE_LOG_CATEGORY_DEFINE(log_window, "window");

namespace {

bool g_initialized = false;

// **Child windows** (WindowDesc::parent). SDL3 has no child window in the platform's sense: its
// "parent" makes an owned top-level window (a popup, a tooltip, a modal) that is not clipped by the
// parent's client area and does not move inside it. So on Windows the child is a plain WS_CHILD
// window of our own class, made here, which SDL then adopts as an external window
// (SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER): its pointer events and its Vulkan surface are SDL's
// as a top-level window's are, and nothing above this file sees the difference.
//
// SDL's event queue is one per process, so a child's events are routed to its parent's `poll`:
// this table says whose child a window id is. Main-thread only, like everything here.
struct ChildEntry {
  u32 child = 0;
  u32 parent = 0;
};
ChildEntry g_children[16];

[[maybe_unused]] void add_child(u32 child, u32 parent) noexcept {
  for (ChildEntry& e : g_children) {
    if (e.child == 0) {
      e = ChildEntry{child, parent};
      return;
    }
  }
}

void remove_child(u32 child) noexcept {
  for (ChildEntry& e : g_children) {
    if (e.child == child) e = ChildEntry{};
  }
}

// Whether an event for `window_id` is the polling window's (`own`): its own, or a child's, which is
// marked as such.
// **Relative mode on an adopted child** (window.md, "Relative mode on an adopted child"). SDL's
// relative mode belongs to its keyboard focus — the mode is on only while that window carries the
// flag — and SDL gives its keyboard focus only to the platform's foreground window, which a
// WS_CHILD never is; worse, SDL drops the absolute motion of a window that carries the flag while
// the mode is off, so a child that asked for it got no motion at all (engine-editor's look,
// 2026-10-12). Asked through the top-level parent instead, the mode came on and SDL's raw-input
// thread then delivered nothing — no motion and no release — beside the web view's
// cross-process child window. So a child's relative mode is this module's, on the platform's own
// pointer messages: the cursor is hidden and confined to the child, each motion's delta is read
// from the child's absolute pointer and the cursor put back where the gesture began, and it is
// shown there again when the mode ends. That only when the gesture is a real pointer's — the
// cursor is over the child and the child's window is the platform's foreground window — and
// otherwise (a test posting to a window in the background) the child's motion is reported as it
// comes, which is the same delta. One pointer, so one such child at a time; main-thread only.
struct RelativeChild {
  u32 child = 0;      // 0: no child holds relative mode
  bool held = false;  // the cursor is hidden, confined and re-centred (a real gesture)
  f32 x = 0.0f;       // where the gesture began, in the child's coordinates
  f32 y = 0.0f;
  i32 desktop_x = 0;  // ... and on the desktop, where the cursor is put back
  i32 desktop_y = 0;
};
RelativeChild g_relative;

#if defined(_WIN32)
// **A child's pointer and keys are its own messages** (window.md, "What the child's messages say",
// roadmap A40). SDL's translation of the child's messages stays, with three things corrected, each
// a place where SDL consults the device or the desktop rather than the message it was handed:
//
// - SDL arms TrackMouseEvent on the first motion after each WM_MOUSELEAVE, and the platform posts
//   the leave at once when the desktop's cursor is not over the window; SDL then reads the buttons
//   from the device (WIN_CheckAsyncMouseRelease, GetAsyncKeyState) on the next motion and releases
//   any the device says are up. A press the platform delivered to the child (a test's, posted, or a
//   person's whose release message is still queued) is let go of there. While the child's own
//   messages hold the button and the child holds the capture — under which every release is
//   delivered to it as a message — that release is not reported (`forgotten`), and the release the
//   message brings later is, by this module if SDL, already up, makes none.
// - While SDL has let go of a button that way, the motion SDL makes up from the desktop's cursor on
//   WM_MOUSELEAVE is not the child's; and a motion's delta is from the child's previous pointer
//   message, not from SDL's last position, which that made-up motion moved.
// - SDL gives a key its keyboard focus's window id, and its keyboard focus only to the platform's
//   foreground window; a key the child's procedure handles while SDL has no keyboard focus (the
//   desktop locked, another desktop, the window behind another) came with no window id and was
//   dropped. It is the child's parent's, as a key SDL attributes to its focus is.
//
// The child's procedure is ours, in front of SDL's (which adopted the window), so that whatever SDL
// makes while handling one of the child's messages is known to be from that message; an event
// filter applies the three rules. Main thread only: the filter leaves events made on other threads
// alone.
struct ChildProc {
  HWND hwnd = nullptr;
  WNDPROC sdl = nullptr;  // SDL's procedure, which ours calls
  u32 id = 0;
  u32 parent = 0;
  u32 held = 0;       // SDL button masks the child's button messages hold
  u32 forgotten = 0;  // ... of which SDL let go by reading the device (above)
  f32 last_x = 0.0f;  // the child's last pointer message's position, for a motion's delta
  f32 last_y = 0.0f;
  bool has_last = false;
};
ChildProc g_child_procs[16];
// The id SDL gives the system's cursor's events (its internal SDL_GLOBAL_MOUSE_ID): the window
// messages' pointer, as against a raw device's, a touch's or a pen's.
constexpr SDL_MouseID k_sdl_system_mouse = 0;
// The child whose procedure is running, and the message it is handling; a message sent while
// another is handled saves and restores both.
ChildProc* g_handling = nullptr;
UINT g_handling_msg = 0;
LPARAM g_handling_lp = 0;
SDL_EventFilter g_prev_filter = nullptr;
void* g_prev_filter_data = nullptr;

ChildProc* child_proc_of(HWND hwnd) noexcept {
  for (ChildProc& c : g_child_procs) {
    if (c.hwnd != nullptr && c.hwnd == hwnd) return &c;
  }
  return nullptr;
}

ChildProc* child_proc_by_id(u32 id) noexcept {
  for (ChildProc& c : g_child_procs) {
    if (c.hwnd != nullptr && c.id == id) return &c;
  }
  return nullptr;
}

// The SDL button a button message is about, as SDL reads it (swapped buttons swap left and right).
u8 sdl_button_of(UINT msg, WPARAM wp) noexcept {
  const bool swap = GetSystemMetrics(SM_SWAPBUTTON) != 0;
  switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK: return swap ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK: return swap ? SDL_BUTTON_LEFT : SDL_BUTTON_RIGHT;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK: return SDL_BUTTON_MIDDLE;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
      return GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? SDL_BUTTON_X1 : SDL_BUTTON_X2;
    default: return 0;
  }
}

bool is_button_down(UINT msg) noexcept {
  return msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN ||
         msg == WM_XBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK ||
         msg == WM_MBUTTONDBLCLK || msg == WM_XBUTTONDBLCLK;
}

bool is_button_up(UINT msg) noexcept {
  return msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP || msg == WM_XBUTTONUP;
}

bool is_key_message(UINT msg) noexcept {
  return msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP;
}

LRESULT CALLBACK child_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  ChildProc* const c = child_proc_of(hwnd);
  if (c == nullptr || c->sdl == nullptr) return DefWindowProcW(hwnd, msg, wp, lp);
  const WNDPROC sdl = c->sdl;
  const u8 button = sdl_button_of(msg, wp);
  if (button != 0 && is_button_down(msg)) c->held |= SDL_BUTTON_MASK(button);
  ChildProc* const outer = g_handling;
  const UINT outer_msg = g_handling_msg;
  const LPARAM outer_lp = g_handling_lp;
  g_handling = c;
  g_handling_msg = msg;
  g_handling_lp = lp;
  const LRESULT r = CallWindowProcW(sdl, hwnd, msg, wp, lp);
  if (button != 0 && is_button_up(msg) && c->hwnd == hwnd) {
    const u32 mask = SDL_BUTTON_MASK(button);
    c->held &= ~mask;
    if ((c->forgotten & mask) != 0) {
      // SDL let go of this button already and made no release now; the message's is reported.
      c->forgotten &= ~mask;
      SDL_Event e;
      SDL_zero(e);
      e.type = SDL_EVENT_MOUSE_BUTTON_UP;
      e.button.windowID = c->id;
      e.button.which = k_sdl_system_mouse;
      e.button.button = button;
      e.button.down = false;
      e.button.clicks = 1;
      e.button.x = static_cast<f32>(static_cast<i16>(LOWORD(lp)));
      e.button.y = static_cast<f32>(static_cast<i16>(HIWORD(lp)));
      (void)SDL_PushEvent(&e);
    }
  }
  g_handling = outer;
  g_handling_msg = outer_msg;
  g_handling_lp = outer_lp;
  return r;
}

bool SDLCALL child_event_filter(void* userdata, SDL_Event* e) {
  (void)userdata;
  if (g_prev_filter != nullptr && !g_prev_filter(g_prev_filter_data, e)) return false;
  if (!SDL_IsMainThread()) return true;
  ChildProc* const handling = g_handling;
  switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
      if (e->key.windowID == 0 && handling != nullptr && is_key_message(g_handling_msg))
        e->key.windowID = handling->parent;
      return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      if (e->button.which != k_sdl_system_mouse) return true;  // touch and pen are SDL's
      ChildProc* const c = child_proc_by_id(e->button.windowID);
      if (c == nullptr || c != handling) return true;
      const u32 mask = SDL_BUTTON_MASK(e->button.button);
      if (e->type == SDL_EVENT_MOUSE_BUTTON_UP && g_handling_msg == WM_MOUSEMOVE &&
          (c->held & mask) != 0 && GetCapture() == c->hwnd) {
        c->forgotten |= mask;  // read from the device, not delivered: the message's comes later
        return false;
      }
      if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) c->forgotten &= ~mask;
      // Where the message says the button went, not SDL's last position (which a made-up motion
      // may have moved).
      if (sdl_button_of(g_handling_msg, 0) != 0) {  // a button message (any of its buttons)
        e->button.x = static_cast<f32>(static_cast<i16>(LOWORD(g_handling_lp)));
        e->button.y = static_cast<f32>(static_cast<i16>(HIWORD(g_handling_lp)));
      }
      c->last_x = e->button.x;
      c->last_y = e->button.y;
      c->has_last = true;
      return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
      if (e->motion.which != k_sdl_system_mouse) return true;
      ChildProc* const c = child_proc_by_id(e->motion.windowID);
      if (c == nullptr || c != handling) return true;  // a pushed event, or made outside
      if (g_handling_msg != WM_MOUSEMOVE && c->forgotten != 0) return false;  // the leave's
      if (c->has_last) {
        e->motion.xrel = e->motion.x - c->last_x;
        e->motion.yrel = e->motion.y - c->last_y;
      }
      c->last_x = e->motion.x;
      c->last_y = e->motion.y;
      c->has_last = true;
      return true;
    }
    default: return true;
  }
}

// The child's procedure in front of SDL's, and the filter, once SDL has adopted the window.
void adopt_child_proc(HWND hwnd, u32 id, u32 parent) noexcept {
  for (ChildProc& c : g_child_procs) {
    if (c.hwnd != nullptr) continue;
    c = ChildProc{};
    c.hwnd = hwnd;
    c.id = id;
    c.parent = parent;
    c.sdl = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&child_proc)));
    break;
  }
  SDL_EventFilter current = nullptr;
  void* data = nullptr;
  if (SDL_GetEventFilter(&current, &data) && current == &child_event_filter) return;
  g_prev_filter = current;
  g_prev_filter_data = data;
  SDL_SetEventFilter(&child_event_filter, nullptr);
}

// SDL's procedure back before SDL lets go of the window, which puts back what it found.
void release_child_proc(HWND hwnd) noexcept {
  ChildProc* const c = child_proc_of(hwnd);
  if (c == nullptr) return;
  if (c->sdl != nullptr)
    (void)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(c->sdl));
  if (g_handling == c) g_handling = nullptr;
  *c = ChildProc{};
}
#endif

// The last dropped file's path, which `Event::text` points at until the next poll: SDL's own copy
// lives only until then too, and one queue serves every window of the process.
std::string g_dropped;

bool route(u32 own, u32 window_id, Event& out) noexcept {
  if (window_id == own) return true;
  for (const ChildEntry& e : g_children) {
    if (e.child != 0 && e.child == window_id && e.parent == own) {
      out.child = true;
      return true;
    }
  }
  return false;
}

#if defined(_WIN32)
constexpr const wchar_t* k_child_class = L"engine_window_child";

bool register_child_class() noexcept {
  static bool registered = false;
  if (registered) return true;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
  // Black until the first frame is presented, so a child that has just moved or grown shows the
  // dark of the viewport's cell rather than a white flash.
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  wc.lpszClassName = k_child_class;
  registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  return registered;
}

HWND hwnd_of(void* sdl_window) noexcept {
  if (sdl_window == nullptr) return nullptr;
  return static_cast<HWND>(
      SDL_GetPointerProperty(SDL_GetWindowProperties(static_cast<SDL_Window*>(sdl_window)),
                             SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}
#endif

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
    case SDL_SCANCODE_DELETE: return Key::Delete;
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

// --- device names ---------------------------------------------------------------------------
//
// Every id here was read out of SDL's own device database in the vendored source
// (src/joystick/SDL_joystick.c's wheel and flight-stick lists, src/joystick/usb_ids.h, and the
// GUIDs in src/joystick/SDL_gamepad_db.h, where "030000006d04000019c2..." is vendor 0x046D
// product 0xC219 little-endian). Two things are worth knowing when adding a row:
//
//   * 0x044F 0xB65D is SDL's *generic* "Thrustmaster Wheel FFB", not the T.16000M; the stick is
//     0x044F 0xB10A, listed among SDL's flight sticks.
//   * The T500 RS gear shift's ids were never captured — the probe that found it recorded
//     events, not ids — so its row matches the misspelled product string the device itself
//     reports instead. Replace it with a vendor/product row the day a `devices` line shows them.
//
// A row whose `raw_name` is null matches on vendor and product; a row that has one matches on
// vendor and that exact name.
struct DeviceNameEntry {
  u16 vendor;
  u16 product;
  const char* raw_name;
  const char* name;
};

constexpr DeviceNameEntry k_device_names[] = {
    // Thrustmaster. The T300 RS enumerates in one of three modes and names itself in none.
    {0x044F, 0xB66D, nullptr, "Thrustmaster T300 RS (PS4 mode)"},
    {0x044F, 0xB66E, nullptr, "Thrustmaster T300 RS"},
    {0x044F, 0xB66F, nullptr, "Thrustmaster T300 RS (advanced mode)"},
    {0x044F, 0xB65E, nullptr, "Thrustmaster T500 RS"},
    {0x044F, 0xB10A, nullptr, "Thrustmaster T.16000M"},
    {0x044F, 0, "Thustmaster T500 RS Gear Shift", "Thrustmaster T500 RS Gear Shift"},
    // Logitech. Each pad has two ids, one per mode of its little switch. SDL names the
    // DirectInput ones itself out of its gamepad database; in XInput mode it can only call
    // them "XInput Controller #1", which is what the F710 on this desk reported before these
    // two rows existed.
    {0x046D, 0xC219, nullptr, "Logitech F710 Gamepad"},  // DirectInput
    {0x046D, 0xC21F, nullptr, "Logitech F710 Gamepad"},  // XInput
    {0x046D, 0xC216, nullptr, "Logitech F310 Gamepad"},  // DirectInput
    {0x046D, 0xC21D, nullptr, "Logitech F310 Gamepad"},  // XInput
};

// --- gamepads -------------------------------------------------------------------------------
//
// SDL identifies a pad by an instance id that grows for the lifetime of the process; the engine
// wants a small stable slot instead, so this table maps one to the other. Slots are handed out
// lowest free first, which makes a single-pad session always slot 0 and a replay reproducible.
// The table is process-wide because SDL's gamepad events are: they carry no window.

bool g_gamepad_subsystem = false;

// A slot keeps the resolved name rather than asking SDL for it: the answer never changes while
// the device is open, and gamepad_name()/joystick_name() have to hand back a stable pointer.
// Longer than any name seen so far; anything past it is truncated rather than allocated for.
constexpr usize k_device_name_capacity = 96;

void store_name(char (&dst)[k_device_name_capacity], const std::string& name) noexcept {
  const usize n =
      name.size() < k_device_name_capacity - 1 ? name.size() : k_device_name_capacity - 1;
  for (usize i = 0; i < n; ++i)
    dst[i] = name[i];
  dst[n] = '\0';
}

// The name a device takes into its slot: the table's, the driver's, or its ids in hex.
std::string resolve_name_for_id(SDL_JoystickID instance, bool gamepad) {
  const char* raw =
      gamepad ? SDL_GetGamepadNameForID(instance) : SDL_GetJoystickNameForID(instance);
  return resolve_device_name(SDL_GetJoystickVendorForID(instance),
                             SDL_GetJoystickProductForID(instance), raw);
}

struct GamepadSlot {
  SDL_Gamepad* handle = nullptr;
  SDL_JoystickID instance = 0;
  char name[k_device_name_capacity] = {};
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
  g_gamepads[slot] = GamepadSlot{pad, instance, {}};
  store_name(g_gamepads[slot].name, resolve_name_for_id(instance, true));
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
  char name[k_device_name_capacity] = {};
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
  g_joysticks[slot] = JoystickSlot{stick, instance, {}};
  store_name(g_joysticks[slot].name, resolve_name_for_id(instance, false));
  return slot;
}

void close_joystick_slot(u32 slot) noexcept {
  if (slot >= k_max_joysticks || g_joysticks[slot].handle == nullptr) return;
  // A wheel's haptics device hangs off the joystick; it goes first so no force outlives the
  // handle it was started on.
  haptics_close(static_cast<u8>(slot));
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

// --- force feedback -------------------------------------------------------------------------
//
// One haptics device per raw joystick slot, opened on request rather than with the joystick:
// opening it is what takes the wheel away from the driver's own centring, which is rude to do
// to a device the caller only wanted to read. Each of the three forces this module drives is
// one SDL effect, created on first use, updated in place afterwards, and run with an infinite
// length, because a force is a state a game holds and not an event it fires.

bool g_haptic_subsystem = false;

struct HapticSlot {
  SDL_Haptic* handle = nullptr;
  SDL_HapticEffectID constant = -1;
  SDL_HapticEffectID spring = -1;
  SDL_HapticEffectID damper = -1;
};

HapticSlot g_haptic_slots[k_max_joysticks];

// -1..1, with a NaN answering 0: a force nobody can name is better as no force at all.
f32 clamp_unit(f32 v) noexcept {
  if (v >= -1.0f && v <= 1.0f) return v;  // a NaN fails both comparisons and falls through
  if (v > 1.0f) return 1.0f;
  if (v < -1.0f) return -1.0f;
  return 0.0f;
}

Sint16 to_level(f32 v) noexcept { return static_cast<Sint16>(clamp_unit(v) * 32767.0f); }

Uint16 to_saturation(f32 strength) noexcept {
  const f32 s = strength > 0.0f ? (strength > 1.0f ? 1.0f : strength) : 0.0f;
  return static_cast<Uint16>(s * 65535.0f);
}

void fill_condition(SDL_HapticEffect& effect, SDL_HapticEffectType type, f32 strength,
                    f32 center) noexcept {
  effect = SDL_HapticEffect{};
  effect.condition.type = type;
  effect.condition.direction.type = SDL_HAPTIC_CARTESIAN;
  effect.condition.direction.dir[0] = 1;
  effect.condition.length = SDL_HAPTIC_INFINITY;
  const Uint16 saturation = to_saturation(strength);
  const Sint16 coefficient = to_level(strength > 0.0f ? strength : 0.0f);
  const Sint16 middle = to_level(center);
  // Every axis gets the same force: a wheel has one, a stick two, and a caller that wants them
  // to differ wants a layout, which this module does not have.
  for (int i = 0; i < 3; ++i) {
    effect.condition.right_sat[i] = saturation;
    effect.condition.left_sat[i] = saturation;
    effect.condition.right_coeff[i] = coefficient;
    effect.condition.left_coeff[i] = coefficient;
    effect.condition.deadband[i] = 0;
    effect.condition.center[i] = middle;
  }
}

// Creates the effect on first use and runs it forever; updates it in place after that. A driver
// that refuses an update (some rebuild an effect rather than retune it) gets it recreated.
bool apply_effect(HapticSlot& slot, SDL_HapticEffectID& id, const SDL_HapticEffect& effect,
                  Uint32 feature) noexcept {
  if (slot.handle == nullptr) return false;
  if ((SDL_GetHapticFeatures(slot.handle) & feature) == 0) return false;
  if (id >= 0) {
    if (SDL_UpdateHapticEffect(slot.handle, id, &effect)) return true;
    SDL_DestroyHapticEffect(slot.handle, id);
    id = -1;
  }
  id = SDL_CreateHapticEffect(slot.handle, &effect);
  if (id < 0) {
    ENGINE_LOG_WARN(log_window, "SDL_CreateHapticEffect failed",
                    log::field("reason", SDL_GetError()));
    return false;
  }
  if (!SDL_RunHapticEffect(slot.handle, id, SDL_HAPTIC_INFINITY)) {
    ENGINE_LOG_WARN(log_window, "SDL_RunHapticEffect failed", log::field("reason", SDL_GetError()));
    return false;
  }
  return true;
}

void destroy_effect(HapticSlot& slot, SDL_HapticEffectID& id) noexcept {
  if (slot.handle == nullptr || id < 0) return;
  SDL_DestroyHapticEffect(slot.handle, id);
  id = -1;
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
  // Force feedback is a subsystem of its own and is just as optional: without it the joystick
  // events still arrive and haptics_open() answers false.
  g_haptic_subsystem = SDL_InitSubSystem(SDL_INIT_HAPTIC);
  if (!g_haptic_subsystem) {
    ENGINE_LOG_WARN(log_window, "no haptics subsystem", log::field("reason", SDL_GetError()));
  }
  return true;
}

void set_background_input(bool enabled) noexcept {
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, enabled ? "1" : "0");
}

void shutdown() noexcept {
  if (!g_initialized) return;
  close_all_gamepads();
  close_all_joysticks();  // which closes each slot's haptics device first
  g_gamepad_subsystem = false;
  g_joystick_subsystem = false;
  g_haptic_subsystem = false;
  SDL_Quit();
  g_initialized = false;
}

const char* known_device_name(u16 vendor, u16 product, const char* raw_name) noexcept {
  for (const DeviceNameEntry& entry : k_device_names) {
    if (entry.vendor != vendor) continue;
    if (entry.raw_name == nullptr) {
      if (entry.product == product) return entry.name;
    } else if (raw_name != nullptr && SDL_strcmp(entry.raw_name, raw_name) == 0) {
      return entry.name;
    }
  }
  return nullptr;
}

std::string resolve_device_name(u16 vendor, u16 product, const char* raw_name) {
  if (const char* known = known_device_name(vendor, product, raw_name); known != nullptr) {
    return std::string(known);
  }
  if (raw_name != nullptr && raw_name[0] != '\0') return std::string(raw_name);
  // Nothing named it. Its ids are still something to tell two devices apart by, and they are
  // what a new row in the table above would be keyed on.
  if (vendor != 0 || product != 0) {
    char ids[16] = {};
    SDL_snprintf(ids, sizeof(ids), "%04X %04X", vendor, product);
    return std::string(ids);
  }
  return std::string("Unknown device");
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
  return g_gamepads[gamepad].name;
}

bool gamepad_connected(u32 gamepad) noexcept {
  return gamepad < k_max_gamepads && g_gamepads[gamepad].handle != nullptr;
}

const char* joystick_name(u32 joystick) noexcept {
  if (joystick >= k_max_joysticks || g_joysticks[joystick].handle == nullptr) return "";
  return g_joysticks[joystick].name;
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
    if (name != nullptr) info.raw_name.assign(name);
    char guid[33] = {};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, static_cast<int>(sizeof(guid)));
    info.guid.assign(guid);
    info.vendor = SDL_GetJoystickVendorForID(id);
    info.product = SDL_GetJoystickProductForID(id);
    info.name = resolve_device_name(info.vendor, info.product, name);

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

bool haptics_open(u8 joystick) {
  if (!g_haptic_subsystem) return false;
  if (joystick >= k_max_joysticks || g_joysticks[joystick].handle == nullptr) return false;
  HapticSlot& slot = g_haptic_slots[joystick];
  if (slot.handle != nullptr) return true;
  if (!SDL_IsJoystickHaptic(g_joysticks[joystick].handle)) {
    ENGINE_LOG_INFO(log_window, "joystick has no haptics", log::field("slot", joystick),
                    log::field("name", joystick_name(joystick)));
    return false;
  }
  SDL_Haptic* haptic = SDL_OpenHapticFromJoystick(g_joysticks[joystick].handle);
  if (haptic == nullptr) {
    ENGINE_LOG_WARN(log_window, "SDL_OpenHapticFromJoystick failed", log::field("slot", joystick),
                    log::field("reason", SDL_GetError()));
    return false;
  }
  slot = HapticSlot{haptic, -1, -1, -1};
  // Worth saying out loud once per open: the driver's own centring stopped the moment this
  // succeeded, and only a spring or a damper from here will bring it back.
  ENGINE_LOG_INFO(log_window, "haptics opened; the device's own centring is now off",
                  log::field("slot", joystick), log::field("name", joystick_name(joystick)),
                  log::field("effects", SDL_GetMaxHapticEffects(haptic)));
  return true;
}

void haptics_close(u8 joystick) noexcept {
  if (joystick >= k_max_joysticks) return;
  HapticSlot& slot = g_haptic_slots[joystick];
  if (slot.handle == nullptr) return;
  SDL_StopHapticEffects(slot.handle);
  destroy_effect(slot, slot.constant);
  destroy_effect(slot, slot.spring);
  destroy_effect(slot, slot.damper);
  SDL_CloseHaptic(slot.handle);
  slot = HapticSlot{};
}

bool haptics_info(u8 joystick, HapticInfo& out) noexcept {
  if (joystick >= k_max_joysticks || g_haptic_slots[joystick].handle == nullptr) return false;
  SDL_Haptic* haptic = g_haptic_slots[joystick].handle;
  const Uint32 features = SDL_GetHapticFeatures(haptic);
  out.constant = (features & SDL_HAPTIC_CONSTANT) != 0;
  out.spring = (features & SDL_HAPTIC_SPRING) != 0;
  out.damper = (features & SDL_HAPTIC_DAMPER) != 0;
  out.friction = (features & SDL_HAPTIC_FRICTION) != 0;
  out.sine = (features & SDL_HAPTIC_SINE) != 0;
  const int axes = SDL_GetNumHapticAxes(haptic);
  out.axes = axes > 0 ? static_cast<u32>(axes) : 0;
  return true;
}

bool set_constant_force(u8 joystick, f32 level) noexcept {
  if (joystick >= k_max_joysticks) return false;
  HapticSlot& slot = g_haptic_slots[joystick];
  SDL_HapticEffect effect{};
  effect.constant.type = static_cast<SDL_HapticEffectType>(SDL_HAPTIC_CONSTANT);
  effect.constant.direction.type = SDL_HAPTIC_CARTESIAN;
  effect.constant.direction.dir[0] = 1;
  effect.constant.length = SDL_HAPTIC_INFINITY;
  effect.constant.level = to_level(level);
  return apply_effect(slot, slot.constant, effect, SDL_HAPTIC_CONSTANT);
}

bool set_spring(u8 joystick, f32 strength, f32 center) noexcept {
  if (joystick >= k_max_joysticks) return false;
  HapticSlot& slot = g_haptic_slots[joystick];
  SDL_HapticEffect effect{};
  fill_condition(effect, static_cast<SDL_HapticEffectType>(SDL_HAPTIC_SPRING), strength, center);
  return apply_effect(slot, slot.spring, effect, SDL_HAPTIC_SPRING);
}

bool set_damper(u8 joystick, f32 strength) noexcept {
  if (joystick >= k_max_joysticks) return false;
  HapticSlot& slot = g_haptic_slots[joystick];
  SDL_HapticEffect effect{};
  fill_condition(effect, static_cast<SDL_HapticEffectType>(SDL_HAPTIC_DAMPER), strength, 0.0f);
  return apply_effect(slot, slot.damper, effect, SDL_HAPTIC_DAMPER);
}

bool stop_forces(u8 joystick) noexcept {
  if (joystick >= k_max_joysticks) return false;
  HapticSlot& slot = g_haptic_slots[joystick];
  if (slot.handle == nullptr) return false;
  const bool stopped = SDL_StopHapticEffects(slot.handle);
  // The effects go with the forces: the next set_* creates and runs a fresh one, which is
  // simpler than remembering that an updated effect would still be stopped.
  destroy_effect(slot, slot.constant);
  destroy_effect(slot, slot.spring);
  destroy_effect(slot, slot.damper);
  return stopped;
}

bool initialized() noexcept { return g_initialized; }

Window::~Window() { destroy(); }

bool Window::create(const WindowDesc& desc, std::string* error) {
  ENGINE_VERIFY(handle_ == nullptr, "Window::create: already created");
  if (!g_initialized) {
    if (error != nullptr) *error = "window::init() has not succeeded";
    return false;
  }
  // The GPU lock before the window (WindowDesc::vulkan): whatever wait there is happens with
  // nothing on screen. Found on 2026-09-30, when two test engine-views sat "Not responding" on
  // the owner's desktop for a quarter of an hour behind a merge gate's hold, having opened their
  // windows and then waited in Device::create, which pumps no messages.
  // A hidden one takes it when it is shown (show()): it is not on screen until then, and a test
  // that only needs a window's events should not queue behind somebody else's GPU work.
  vulkan_ = desc.vulkan;
  if (desc.parent != nullptr) {
#if defined(_WIN32)
    HWND parent = desc.parent->valid() ? hwnd_of(desc.parent->native()) : nullptr;
    if (parent == nullptr) {
      if (error != nullptr) *error = "a child window needs a parent that has a window";
      return false;
    }
    if (!register_child_class()) {
      if (error != nullptr) *error = "RegisterClassExW for the child window class failed";
      return false;
    }
    if (desc.vulkan && !desc.hidden) (void)gpu_hold_.acquire();
    HWND hwnd = CreateWindowExW(
        0, k_child_class, L"",
        WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | (desc.hidden ? 0u : WS_VISIBLE), desc.x,
        desc.y, static_cast<int>(desc.width), static_cast<int>(desc.height), parent, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (hwnd == nullptr) {
      if (error != nullptr)
        *error = "CreateWindowExW (child window) failed: " + std::to_string(GetLastError());
      gpu_hold_.release();
      return false;
    }
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER, hwnd);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, desc.vulkan);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
    SDL_Window* window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (window == nullptr) {
      set_error(error, "SDL_CreateWindowWithProperties (child window)");
      DestroyWindow(hwnd);
      gpu_hold_.release();
      return false;
    }
    handle_ = window;
    id_ = SDL_GetWindowID(window);
    child_ = true;
    add_child(id_, desc.parent->id_);
    adopt_child_proc(hwnd, id_, desc.parent->id_);
    // Above the parent's other children (a webview made before it), so where they overlap the
    // pointer and the picture are this window's.
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    refresh_pixel_size();
    ENGINE_LOG_DEBUG(log_window, "child window created", log::field("width", pixel_width_),
                     log::field("height", pixel_height_));
    return true;
#else
    if (error != nullptr)
      *error = "child windows are built on Windows only so far (docs/subsystems/window.md)";
    return false;
#endif
  }
  if (desc.vulkan && !desc.hidden) (void)gpu_hold_.acquire();
  SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
  if (desc.resizable) flags |= SDL_WINDOW_RESIZABLE;
  if (desc.vulkan) flags |= SDL_WINDOW_VULKAN;
  if (desc.hidden) flags |= SDL_WINDOW_HIDDEN;
  if (desc.borderless) flags |= SDL_WINDOW_BORDERLESS;
  SDL_Window* window = SDL_CreateWindow(desc.title, static_cast<int>(desc.width),
                                        static_cast<int>(desc.height), flags);
  if (window == nullptr) {
    set_error(error, "SDL_CreateWindow");
    gpu_hold_.release();
    return false;
  }
  if (desc.borderless) {
    // The primary display's top-left corner, so a window of the display's size covers it.
    SDL_Rect bounds{};
    if (SDL_GetDisplayBounds(SDL_GetPrimaryDisplay(), &bounds)) {
      (void)SDL_SetWindowPosition(window, bounds.x, bounds.y);
    }
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
  if (child_ && g_relative.child == id_) (void)set_relative_mouse(false);
#if defined(_WIN32)
  // SDL lets go of a window it adopted without destroying it; the child is ours to destroy.
  HWND own = child_ ? hwnd_of(handle_) : nullptr;
  if (own != nullptr) release_child_proc(own);
#endif
  SDL_DestroyWindow(static_cast<SDL_Window*>(handle_));
#if defined(_WIN32)
  if (own != nullptr && IsWindow(own)) DestroyWindow(own);
#endif
  if (child_) remove_child(id_);
  handle_ = nullptr;
  id_ = 0;
  gpu_hold_.release();  // the last of the process's holds (a device's, a window's) lets it go
  vulkan_ = false;
  child_ = false;
}

void Window::refresh_pixel_size() noexcept {
  int w = 0;
  int h = 0;
#if defined(_WIN32)
  // A child's client rectangle is its pixels (SDL makes the process per-monitor DPI aware); SDL's
  // own idea of an adopted window's size lags a move it did not make.
  if (child_) {
    RECT r{};
    if (GetClientRect(hwnd_of(handle_), &r)) {
      w = r.right - r.left;
      h = r.bottom - r.top;
    }
    pixel_width_ = w > 0 ? static_cast<u32>(w) : 0;
    pixel_height_ = h > 0 ? static_cast<u32>(h) : 0;
    return;
  }
#endif
  if (handle_ != nullptr) SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(handle_), &w, &h);
  pixel_width_ = w > 0 ? static_cast<u32>(w) : 0;
  pixel_height_ = h > 0 ? static_cast<u32>(h) : 0;
}

void* Window::os_handle() const noexcept {
#if defined(_WIN32)
  return hwnd_of(handle_);
#else
  return nullptr;
#endif
}

bool Window::set_rect(i32 x, i32 y, u32 width, u32 height) noexcept {
  if (handle_ == nullptr || !child_) return false;
#if defined(_WIN32)
  if (!SetWindowPos(hwnd_of(handle_), HWND_TOP, x, y, static_cast<int>(width),
                    static_cast<int>(height), SWP_NOACTIVATE))
    return false;
  refresh_pixel_size();
  return true;
#else
  (void)x;
  (void)y;
  (void)width;
  (void)height;
  return false;
#endif
}

bool Window::set_visible(bool visible) noexcept {
  if (handle_ == nullptr) return false;
  if (visible && vulkan_ && !gpu_hold_.counted()) (void)gpu_hold_.acquire();
#if defined(_WIN32)
  if (child_) {
    ShowWindow(hwnd_of(handle_), visible ? SW_SHOWNA : SW_HIDE);
    return true;
  }
#endif
  return visible ? SDL_ShowWindow(static_cast<SDL_Window*>(handle_))
                 : SDL_HideWindow(static_cast<SDL_Window*>(handle_));
}

void Window::focus() noexcept {
#if defined(_WIN32)
  if (HWND hwnd = hwnd_of(handle_); hwnd != nullptr && GetFocus() != hwnd) SetFocus(hwnd);
#endif
}

bool Window::logical_size(u32& width, u32& height) const noexcept {
  if (handle_ == nullptr) return false;
#if defined(_WIN32)
  // A child's coordinates are its parent's client pixels, which is what SDL reports its pointer in
  // (an adopted window's own size in SDL can lag a move SDL did not make).
  if (child_) {
    width = pixel_width_;
    height = pixel_height_;
    return true;
  }
#endif
  int w = 0;
  int h = 0;
  if (!SDL_GetWindowSize(static_cast<SDL_Window*>(handle_), &w, &h)) return false;
  width = w > 0 ? static_cast<u32>(w) : 0;
  height = h > 0 ? static_cast<u32>(h) : 0;
  return true;
}

bool Window::desktop_center(i32& x, i32& y) const noexcept {
  if (handle_ == nullptr) return false;
  SDL_Window* window = static_cast<SDL_Window*>(handle_);
  int left = 0;
  int top = 0;
  int w = 0;
  int h = 0;
  if (!SDL_GetWindowPosition(window, &left, &top) || !SDL_GetWindowSize(window, &w, &h))
    return false;
  x = left + w / 2;
  y = top + h / 2;
  return true;
}

f32 Window::display_scale() const noexcept {
  if (handle_ == nullptr) return 1.0f;
  const f32 scale = SDL_GetWindowDisplayScale(static_cast<SDL_Window*>(handle_));
  return scale > 0.0f ? scale : 1.0f;
}

bool Window::set_size(u32 width, u32 height) noexcept {
  if (handle_ == nullptr || child_) return false;
  return SDL_SetWindowSize(static_cast<SDL_Window*>(handle_), static_cast<int>(width),
                           static_cast<int>(height));
}

bool Window::poll(Event& out) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    out = Event{};
    out.timestamp_ns = e.common.timestamp;
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
        if (!route(id_, e.window.windowID, out)) continue;
        out.kind = EventKind::CloseRequested;
        return true;
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        if (!route(id_, e.window.windowID, out)) continue;
        out.kind = EventKind::Resized;
        out.width = e.window.data1;
        out.height = e.window.data2;
        if (!out.child) {  // a child's size is its own Window's, refreshed by its owner
          pixel_width_ = e.window.data1 > 0 ? static_cast<u32>(e.window.data1) : 0;
          pixel_height_ = e.window.data2 > 0 ? static_cast<u32>(e.window.data2) : 0;
        }
        return true;
      case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        if (!route(id_, e.window.windowID, out)) continue;
        out.kind = EventKind::ScaleChanged;
        out.value = SDL_GetWindowDisplayScale(SDL_GetWindowFromID(e.window.windowID));
        return true;
      case SDL_EVENT_WINDOW_FOCUS_GAINED:
        if (!route(id_, e.window.windowID, out)) continue;
        out.kind = EventKind::FocusGained;
        return true;
      case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (!route(id_, e.window.windowID, out)) continue;
#if defined(_WIN32)
        // Switching away gives the cursor back, as SDL's own relative mode does.
        if (g_relative.held) {
          (void)ClipCursor(nullptr);
          (void)SDL_ShowCursor();
          g_relative.held = false;
        }
#endif
        out.kind = EventKind::FocusLost;
        return true;
      case SDL_EVENT_KEY_DOWN:
      case SDL_EVENT_KEY_UP:
        if (!route(id_, e.key.windowID, out)) continue;
        out.kind = e.type == SDL_EVENT_KEY_DOWN ? EventKind::KeyDown : EventKind::KeyUp;
        out.key = key_from_scancode(e.key.scancode);
        out.scancode = static_cast<u16>(e.key.scancode);
        out.repeat = e.key.repeat;
        return true;
      case SDL_EVENT_MOUSE_MOTION:
        if (!route(id_, e.motion.windowID, out)) continue;
        out.kind = EventKind::MouseMove;
        out.x = e.motion.x;
        out.y = e.motion.y;
        out.dx = e.motion.xrel;
        out.dy = e.motion.yrel;
#if defined(_WIN32)
        // A child holding the pointer (g_relative): the delta is from where the cursor was put
        // back, and it is put back again at once, so the platform's next motion message — made
        // from the cursor's position when it is read — runs from there too.
        if (g_relative.held && e.motion.windowID == g_relative.child) {
          out.dx = e.motion.x - g_relative.x;
          out.dy = e.motion.y - g_relative.y;
          out.x = g_relative.x;
          out.y = g_relative.y;
          if (out.dx == 0.0f && out.dy == 0.0f) continue;  // the cursor put back, or no motion
          SDL_WarpMouseInWindow(SDL_GetWindowFromID(g_relative.child), g_relative.x, g_relative.y);
        }
#endif
        return true;
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP:
        if (!route(id_, e.button.windowID, out)) continue;
        out.kind = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? EventKind::MouseButtonDown
                                                         : EventKind::MouseButtonUp;
        out.button = e.button.button;
        out.x = e.button.x;
        out.y = e.button.y;
        return true;
      case SDL_EVENT_MOUSE_WHEEL:
        if (!route(id_, e.wheel.windowID, out)) continue;
        out.kind = EventKind::MouseWheel;
        out.x = e.wheel.mouse_x;
        out.y = e.wheel.mouse_y;
        out.dx = e.wheel.x;
        out.dy = e.wheel.y;
        return true;
      // A file dropped from the platform's file manager (SDL's drop events; a drop's text and
      // the drag's progress are not reported). The path is copied: SDL's string lives only until
      // the next poll, and so does `text`.
      case SDL_EVENT_DROP_FILE:
        if (e.drop.data == nullptr || !route(id_, e.drop.windowID, out)) continue;
        g_dropped = e.drop.data;
        out.kind = EventKind::DropFile;
        out.x = e.drop.x;
        out.y = e.drop.y;
        out.text = g_dropped.c_str();
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
  if (handle_ == nullptr) return;
  if (vulkan_ && !gpu_hold_.counted()) (void)gpu_hold_.acquire();  // before it appears (create)
  SDL_ShowWindow(static_cast<SDL_Window*>(handle_));
}

bool Window::set_relative_mouse(bool enabled) noexcept {
  if (handle_ == nullptr) return false;
#if defined(_WIN32)
  if (child_) {
    // This module's, on the platform's pointer messages (g_relative, above); SDL's flag never
    // goes on the child.
    if (enabled == (g_relative.child == id_)) return true;  // asked on a change only
    if (enabled && g_relative.child != 0) return false;     // another child holds the pointer
    const HWND own = hwnd_of(handle_);
    if (!enabled) {
      if (g_relative.held) {
        (void)ClipCursor(nullptr);
        (void)SetCursorPos(g_relative.desktop_x, g_relative.desktop_y);
        (void)SDL_ShowCursor();
      }
      g_relative = RelativeChild{};
      relative_since_ns_ = 0;
      return true;
    }
    POINT cursor{};
    (void)GetCursorPos(&cursor);
    POINT local = cursor;
    (void)ScreenToClient(own, &local);
    RECT client{};
    (void)GetClientRect(own, &client);
    RECT on_desktop = client;
    (void)MapWindowPoints(own, nullptr, reinterpret_cast<POINT*>(&on_desktop), 2);
    g_relative = RelativeChild{};
    g_relative.child = id_;
    g_relative.x = static_cast<f32>(local.x);
    g_relative.y = static_cast<f32>(local.y);
    g_relative.desktop_x = cursor.x;
    g_relative.desktop_y = cursor.y;
    // A real gesture: the cursor is over the child, and the child's window is in front. A posted
    // press on a window in the background (a test) leaves the desktop's cursor alone.
    g_relative.held =
        PtInRect(&client, local) != 0 && GetForegroundWindow() == GetAncestor(own, GA_ROOT);
    if (g_relative.held) {
      (void)SDL_HideCursor();
      (void)ClipCursor(&on_desktop);
    }
    relative_since_ns_ = SDL_GetTicksNS();
    return true;
  }
#endif
  if (!SDL_SetWindowRelativeMouseMode(static_cast<SDL_Window*>(handle_), enabled)) {
    ENGINE_LOG_WARN(log_window, "relative mouse mode refused", log::field("enabled", enabled),
                    log::field("reason", SDL_GetError()));
    return false;
  }
  // Every event SDL stamps from here on was reported with the mode on (Event::timestamp_ns).
  relative_since_ns_ = enabled ? SDL_GetTicksNS() : 0;
  return true;
}

bool Window::relative_mouse() const noexcept {
  if (handle_ == nullptr) return false;
  // A child: in effect, not merely asked — the cursor is hidden, confined and re-centred.
  if (child_) return g_relative.child == id_ && g_relative.held;
  return SDL_GetWindowRelativeMouseMode(static_cast<SDL_Window*>(handle_));
}

bool Window::relative_mouse_asked() const noexcept {
  if (handle_ == nullptr) return false;
  return child_ ? g_relative.child == id_
                : SDL_GetWindowRelativeMouseMode(static_cast<SDL_Window*>(handle_));
}

bool Window::push_event(const Event& event) noexcept {
  if (handle_ == nullptr) return false;
  SDL_Event e;
  SDL_zero(e);
  switch (event.kind) {
    case EventKind::KeyDown:
    case EventKind::KeyUp:
      e.type = event.kind == EventKind::KeyDown ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
      e.key.windowID = id_;
      e.key.scancode = static_cast<SDL_Scancode>(event.scancode);
      e.key.down = event.kind == EventKind::KeyDown;
      e.key.repeat = event.repeat;
      break;
    case EventKind::MouseButtonDown:
    case EventKind::MouseButtonUp:
      e.type = event.kind == EventKind::MouseButtonDown ? SDL_EVENT_MOUSE_BUTTON_DOWN
                                                        : SDL_EVENT_MOUSE_BUTTON_UP;
      e.button.windowID = id_;
      e.button.button = event.button;
      e.button.down = event.kind == EventKind::MouseButtonDown;
      e.button.clicks = 1;
      e.button.x = event.x;
      e.button.y = event.y;
      break;
    case EventKind::MouseMove:
      e.type = SDL_EVENT_MOUSE_MOTION;
      e.motion.windowID = id_;
      e.motion.x = event.x;
      e.motion.y = event.y;
      e.motion.xrel = event.dx;
      e.motion.yrel = event.dy;
      break;
    case EventKind::MouseWheel:
      e.type = SDL_EVENT_MOUSE_WHEEL;
      e.wheel.windowID = id_;
      e.wheel.x = event.dx;
      e.wheel.y = event.dy;
      e.wheel.mouse_x = event.x;
      e.wheel.mouse_y = event.y;
      break;
    case EventKind::DropFile:
      // SDL keeps the pointer as given; the caller keeps the string until the event is polled.
      if (event.text == nullptr) return false;
      e.type = SDL_EVENT_DROP_FILE;
      e.drop.windowID = id_;
      e.drop.x = event.x;
      e.drop.y = event.y;
      e.drop.data = event.text;
      break;
    default: return false;
  }
  // SDL stamps a zero timestamp with its own clock as the event goes on the queue.
  return SDL_PushEvent(&e);
}

namespace {

#if defined(_WIN32)
// The virtual key for the keys `post_os_event` takes; 0 for any other.
UINT virtual_key(Key key) noexcept {
  switch (key) {
    case Key::Control: return VK_CONTROL;
    case Key::Shift: return VK_SHIFT;
    case Key::Alt: return VK_MENU;
    case Key::Escape: return VK_ESCAPE;
    case Key::Delete: return VK_DELETE;
    default: break;
  }
  const u16 k = static_cast<u16>(key);
  if (k >= static_cast<u16>(Key::A) && k <= static_cast<u16>(Key::Z))
    return 'A' + (k - static_cast<u16>(Key::A));
  if (k >= static_cast<u16>(Key::Digit1) && k <= static_cast<u16>(Key::Digit9))
    return '1' + (k - static_cast<u16>(Key::Digit1));
  if (key == Key::Digit0) return '0';
  return 0;
}

// One event as the window message the platform would have sent, to `hwnd`.
bool post_message(HWND hwnd, const Event& event) noexcept {
  if (hwnd == nullptr) return false;
  const LPARAM at = MAKELPARAM(static_cast<i16>(event.x), static_cast<i16>(event.y));
  switch (event.kind) {
    case EventKind::KeyDown:
    case EventKind::KeyUp: {
      const UINT vk = virtual_key(event.key);
      if (vk == 0) return false;
      // The scancode in bits 16 to 23 is what SDL reads the key from; bits 30 and 31 are the
      // previous state and the transition, as a real key's message carries them.
      const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
      const bool down = event.kind == EventKind::KeyDown;
      const LPARAM bits = static_cast<LPARAM>(1u | (scan << 16) | (down ? 0u : 0xC0000000u));
      return PostMessageW(hwnd, down ? WM_KEYDOWN : WM_KEYUP, vk, bits) != 0;
    }
    case EventKind::MouseButtonDown:
    case EventKind::MouseButtonUp: {
      const bool down = event.kind == EventKind::MouseButtonDown;
      UINT msg = 0;
      WPARAM held = 0;
      switch (event.button) {
        case 1:
          msg = down ? WM_LBUTTONDOWN : WM_LBUTTONUP;
          held = down ? MK_LBUTTON : 0;
          break;
        case 2:
          msg = down ? WM_MBUTTONDOWN : WM_MBUTTONUP;
          held = down ? MK_MBUTTON : 0;
          break;
        case 3:
          msg = down ? WM_RBUTTONDOWN : WM_RBUTTONUP;
          held = down ? MK_RBUTTON : 0;
          break;
        default: return false;
      }
      return PostMessageW(hwnd, msg, held, at) != 0;
    }
    case EventKind::MouseMove: return PostMessageW(hwnd, WM_MOUSEMOVE, 0, at) != 0;
    default: return false;
  }
}
#endif

}  // namespace

bool Window::post_os_event(const Event& event) noexcept {
#if defined(_WIN32)
  return post_message(hwnd_of(handle_), event);
#else
  (void)event;
  return false;
#endif
}

bool Window::post_to_focus(const Event& event) noexcept {
#if defined(_WIN32)
  return post_message(GetFocus(), event);
#else
  (void)event;
  return false;
#endif
}

bool Window::has_os_focus() const noexcept {
#if defined(_WIN32)
  const HWND own = hwnd_of(handle_);
  return own != nullptr && GetFocus() == own;
#else
  return false;
#endif
}

bool open_file_dialog(const Window& owner, std::span<const std::string> extensions,
                      std::string& path) {
  path.clear();
#if defined(_WIN32)
  // "Files (*.a;*.b)\0*.a;*.b\0All files\0*.*\0\0", in UTF-16.
  std::wstring patterns;
  for (const std::string& ext : extensions) {
    if (!patterns.empty()) patterns += L';';
    patterns += L"*.";
    for (const char c : ext)
      patterns += static_cast<wchar_t>(static_cast<unsigned char>(c));
  }
  std::wstring filter;
  if (!patterns.empty()) {
    filter += L"Files (" + patterns + L")";
    filter.push_back(L'\0');
    filter += patterns;
    filter.push_back(L'\0');
  }
  filter += L"All files";
  filter.push_back(L'\0');
  filter += L"*.*";
  filter.push_back(L'\0');
  filter.push_back(L'\0');
  wchar_t file[4096] = {};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = static_cast<HWND>(owner.os_handle());
  ofn.lpstrFilter = filter.c_str();
  ofn.lpstrFile = file;
  ofn.nMaxFile = static_cast<DWORD>(sizeof(file) / sizeof(file[0]));
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
  if (!GetOpenFileNameW(&ofn)) return false;
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, file, -1, nullptr, 0, nullptr, nullptr);
  if (bytes <= 1) return false;
  path.resize(static_cast<usize>(bytes - 1));
  (void)WideCharToMultiByte(CP_UTF8, 0, file, -1, path.data(), bytes, nullptr, nullptr);
  return true;
#else
  (void)owner;
  (void)extensions;
  return false;
#endif
}

Window::PointerReport Window::pointer_report() const noexcept {
  PointerReport r;
  if (handle_ == nullptr) return r;
  SDL_Window* window = static_cast<SDL_Window*>(handle_);
  r.relative_asked = relative_mouse_asked();
  r.sdl_flag = SDL_GetWindowRelativeMouseMode(window);
  SDL_Window* keyboard = SDL_GetKeyboardFocus();
  SDL_Window* mouse = SDL_GetMouseFocus();
  r.keyboard_focus = keyboard != nullptr ? SDL_GetWindowID(keyboard) : 0u;
  r.mouse_focus = mouse != nullptr ? SDL_GetWindowID(mouse) : 0u;
  // SDL's mode is on exactly when its keyboard focus carries the flag
  // (SDL_UpdateRelativeMouseMode).
  r.relative_on = keyboard != nullptr && SDL_GetWindowRelativeMouseMode(keyboard);
  (void)SDL_GetWindowPosition(window, &r.sdl_x, &r.sdl_y);
  (void)SDL_GetWindowSize(window, &r.sdl_width, &r.sdl_height);
#if defined(_WIN32)
  const HWND hwnd = hwnd_of(handle_);
  RECT client{};
  POINT corner{0, 0};
  if (hwnd != nullptr && GetClientRect(hwnd, &client) && ClientToScreen(hwnd, &corner)) {
    r.os_x = corner.x;
    r.os_y = corner.y;
    r.os_width = client.right - client.left;
    r.os_height = client.bottom - client.top;
  }
  POINT cursor{};
  if (GetCursorPos(&cursor)) {
    r.cursor_x = cursor.x - r.os_x;
    r.cursor_y = cursor.y - r.os_y;
    r.cursor_inside =
        r.cursor_x >= 0 && r.cursor_y >= 0 && r.cursor_x < r.os_width && r.cursor_y < r.os_height;
  }
  RECT clip{};
  if (GetClipCursor(&clip)) {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.clipped = clip.left > vx || clip.top > vy ||
                clip.right < vx + GetSystemMetrics(SM_CXVIRTUALSCREEN) ||
                clip.bottom < vy + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    r.clip_left = clip.left;
    r.clip_top = clip.top;
    r.clip_right = clip.right;
    r.clip_bottom = clip.bottom;
  }
#endif
  return r;
}

DesktopState desktop_state() {
  DesktopState s;
#if defined(_WIN32)
  const auto narrow = [](const wchar_t* w) {
    std::string out;
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return out;
    out.resize(static_cast<usize>(n - 1));
    (void)WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
  };
  wchar_t name[256] = {};
  DWORD needed = 0;
  if (GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, name, sizeof(name), &needed))
    s.desktop = narrow(name) + "\\";
  if (GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, name,
                                sizeof(name), &needed))
    s.desktop += narrow(name);
  // GetForegroundWindow is NULL for a thread whose desktop is not the input desktop.
  const HWND front = GetForegroundWindow();
  DWORD pid = 0;
  if (front != nullptr) (void)GetWindowThreadProcessId(front, &pid);
  s.foreground_own = pid != 0 && pid == GetCurrentProcessId();
  if (pid != 0) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process != nullptr) {
      wchar_t path[MAX_PATH] = {};
      DWORD size = MAX_PATH;
      if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        const wchar_t* base = path;
        for (const wchar_t* p = path; *p != L'\0'; ++p) {
          if (*p == L'\\' || *p == L'/') base = p + 1;
        }
        s.foreground = narrow(base);
      }
      CloseHandle(process);
    }
  }
  WTSINFOEXW* info = nullptr;
  DWORD bytes = 0;
  if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSSessionInfoEx,
                                  reinterpret_cast<LPWSTR*>(&info), &bytes) &&
      info != nullptr) {
    if (info->Level == 1) {
      const LONG flags = info->Data.WTSInfoExLevel1.SessionFlags;
      s.locked = flags == WTS_SESSIONSTATE_LOCK ? 1 : (flags == WTS_SESSIONSTATE_UNLOCK ? 0 : -1);
    }
    WTSFreeMemory(info);
  }
#endif
  return s;
}

}  // namespace engine::window

// ---- the Vulkan surface (backend/vulkan/surface.h) ----------------------------------------------

namespace engine::window::vulkan {

std::span<const char* const> instance_extensions() {
  Uint32 count = 0;
  const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
  if (names == nullptr) {
    ENGINE_LOG_WARN(log_window, "SDL has no Vulkan support here",
                    log::field("reason", SDL_GetError()));
    return {};
  }
  return std::span<const char* const>(names, count);
}

bool create_surface(const Window& window, VkInstance instance, VkSurfaceKHR& out,
                    std::string* error) {
  out = nullptr;
  if (window.native() == nullptr) {
    if (error != nullptr) *error = "window::vulkan::create_surface: no window";
    return false;
  }
  if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(window.native()), instance, nullptr,
                                &out)) {
    set_error(error, "SDL_Vulkan_CreateSurface");
    out = nullptr;
    return false;
  }
  return true;
}

void destroy_surface(VkInstance instance, VkSurfaceKHR surface) noexcept {
  if (surface != nullptr) SDL_Vulkan_DestroySurface(instance, surface, nullptr);
}

}  // namespace engine::window::vulkan
