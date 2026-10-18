#pragma once

// Windows and input over SDL3 (ADR-0012). The rest of the engine never sees SDL. Everything here
// is process-wide and main-thread only.
//
//     if (!window::init(&error)) { /* no display: run headless */ }
//     window::Window window;
//     window.create({.title = "view", .width = 1280, .height = 720}, &error);
//     window::Event event;
//     while (window.poll(event)) { if (event.kind == window::EventKind::Quit) running = false; }
//
// The Vulkan glue a window system owns — the instance extensions a surface needs, and making and
// destroying the surface — is not here: it is a backend header,
// foundation/window/backend/vulkan/surface.h, so that this one names no graphics API
// (docs/subsystems/window.md, "The Vulkan surface").

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <foundation/gpu_lock/device_hold.h>

#include <span>
#include <string>

namespace engine::window {

// Starts the video and event subsystems, then the gamepad and joystick subsystems when the
// platform has them (a machine with no joystick driver still gets a window; the failure is
// logged, not returned). False, with the reason, on a machine without a display server or video
// driver; nothing else in this module may be used then.
bool init(std::string* error = nullptr);
void shutdown() noexcept;
bool initialized() noexcept;

// SDL drops gamepad and joystick events while the process owns windows and none of them holds
// keyboard focus, which is right for a game and wrong for a tool the owner starts from a
// terminal and then touches a wheel. Off by default; `engine-input` turns it on. Call after
// init(); it has no effect on a process with no window.
void set_background_input(bool enabled) noexcept;

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
  Delete,  // after the F keys, so every value above keeps its number
};

// How many gamepads this module tracks at once. Slots are handed out lowest free first and
// released on disconnect, so `Event::gamepad` is always below this.
inline constexpr u32 k_max_gamepads = 8;

// How many raw joysticks this module tracks at once. SDL reports every device as a joystick and
// the ones it has a mapping for additionally as a gamepad; this module opens a joystick only
// when it is *not* a gamepad, so the two tables never hold the same device.
//
// **The two slot spaces are separate.** `Event::gamepad` indexes the gamepad table and
// `Event::joystick` the joystick table, both from 0, both lowest free first, both released on
// disconnect: gamepad 0 and joystick 0 are two different devices. Whoever converts these events
// into `input::RawEvent`s keeps them apart by source, not by the slot number
// (docs/subsystems/input.md).
inline constexpr u32 k_max_joysticks = 8;

// `InputDeviceInfo::slot` for a device that is attached but has no slot: its arrival event has
// not been polled yet, or every slot of its kind is taken.
inline constexpr u8 k_invalid_slot = 0xFF;

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

// Where a hat (a d-pad on a stick, the "POV hat") is pushed, as a bitmask of the four cardinal
// directions: a diagonal is two bits, and centered is none. The values are SDL's own and are
// part of the input log format (`input::k_hat_up` and friends mirror them): append, never
// reorder. A binding names one direction of one hat, `input::hat_code(hat, direction)`.
enum class HatDirection : u8 {
  Centered = 0,
  Up = 1,
  Right = 2,
  Down = 4,
  Left = 8,
  RightUp = Right | Up,      // 3
  RightDown = Right | Down,  // 6
  LeftUp = Left | Up,        // 9
  LeftDown = Left | Down,    // 12
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
  // Raw joysticks: everything SDL has no gamepad mapping for (wheels, pedal sets, shifters,
  // flight sticks, arcade panels). No layout is assumed, so axes, buttons, and hats are bare
  // indices; `joystick` holds this module's joystick slot id, a different space from `gamepad`.
  JoystickConnected,
  JoystickDisconnected,
  JoystickAxis,
  JoystickButtonDown,
  JoystickButtonUp,
  JoystickHat,
  // The display's scale under the window changed (it was dragged to a monitor of another scale,
  // or the scale was changed in the system's settings): `value` holds the new scale, 1.5 at
  // 150 % (`Window::display_scale`). Reported for a top-level window; its children follow it.
  ScaleChanged,
  // A file was dropped on the window from the platform's file manager: `text` is its path (UTF-8,
  // valid until the next `poll`), `x`/`y` where it was dropped. One event per file.
  DropFile,
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
  u8 joystick = 0;  // Joystick*: the joystick slot id, 0..k_max_joysticks-1
  u8 index = 0;     // JoystickAxis/JoystickButton*/JoystickHat: the axis, button, or hat index
  HatDirection hat = HatDirection::Centered;  // JoystickHat
  // The event is a child window's (`WindowDesc::parent`), reported through its parent's `poll`:
  // pointer coordinates are then the child's own, and Resized is the child's size.
  bool child = false;
  // GamepadAxis: -1..1 for sticks, 0..1 for triggers. JoystickAxis: -1..1. ScaleChanged: the
  // display's new scale.
  f32 value = 0.0f;
  // When the platform reported it, nanoseconds on SDL's clock (`SDL_GetTicksNS`), which is also the
  // clock of `Window::relative_mouse_since_ns`: what tells motion reported before relative mode
  // came on from motion after it. Zero for an event nothing stamped.
  u64 timestamp_ns = 0;
  // DropFile: the dropped file's path, owned by the window that polled it and valid until its next
  // `poll` (for `push_event`, owned by the caller until the event is polled). Null otherwise.
  const char* text = nullptr;
};

// --- device names ---------------------------------------------------------------------------
//
// Drivers name a device badly or not at all. A Thrustmaster T300 RS arrives from the Windows
// driver as "44F B66E" — its USB ids in hex, because nothing wrote an OEM name into the
// registry — and a T500 RS gear shift reports "Thustmaster T500 RS Gear Shift", a typo in the
// hardware's own product string. So this module keeps a small table of the devices the project
// has been shown, keyed by USB vendor and product (or, where the ids were never captured, by
// the exact misspelled name the device reports), and that table wins over what the driver said.
// What the driver said stays available as `InputDeviceInfo::raw_name`.
//
// The table is a courtesy, not a layout: naming a device is not the same as knowing which axis
// is its brake. Adding one is a line here and a line in docs/subsystems/window.md.

// The table's name for a device, or nullptr when it has no entry. `raw_name` may be null.
const char* known_device_name(u16 vendor, u16 product, const char* raw_name) noexcept;

// What this module reports as a device's name: the table's entry when it has one, otherwise the
// driver's name, otherwise the ids in hex ("044F B66E"), otherwise "Unknown device".
std::string resolve_device_name(u16 vendor, u16 product, const char* raw_name);

// "South", "LeftTrigger", ... for logs and tools; "Unknown" outside the enum.
const char* gamepad_button_name(GamepadButton button) noexcept;
const char* gamepad_axis_name(GamepadAxis axis) noexcept;
// "Centered", "Up", "RightDown", ...; "Unknown" for a mask outside the nine positions.
const char* hat_direction_name(HatDirection hat) noexcept;
// The pad's resolved product name ("Xbox Series X Controller"), or "" for a slot with nothing
// in it. Valid until that slot disconnects.
const char* gamepad_name(u32 gamepad) noexcept;
bool gamepad_connected(u32 gamepad) noexcept;
// The same two, over the separate joystick slot space.
const char* joystick_name(u32 joystick) noexcept;
bool joystick_connected(u32 joystick) noexcept;

// What a device is and how much of it there is, without waiting for an event. `slot` indexes
// the gamepad table when `is_gamepad`, the joystick table otherwise, and is k_invalid_slot
// until that device's arrival event has been polled.
struct InputDeviceInfo {
  std::string name;      // resolved: the name table's entry where it has one (see above)
  std::string raw_name;  // what the driver called it, unedited; empty when it named nothing
  std::string guid;      // SDL's 32-character stable device id: bus, vendor, product, version
  u16 vendor = 0;
  u16 product = 0;
  u8 axes = 0;
  u8 buttons = 0;
  u8 hats = 0;
  bool is_gamepad = false;
  u8 slot = k_invalid_slot;
};

// Every gamepad and joystick attached right now, in SDL's enumeration order. Replaces `out`;
// returns how many were written. Empty before init() and on a machine with no joystick driver.
u32 input_devices(Vector<InputDeviceInfo>& out);

// Plays a rumble effect on a gamepad slot: `low` and `high` are the two motors, 0..1, for `ms`
// milliseconds. False when the slot is empty or the pad has no motors — which is most of them,
// so a caller treats false as "nothing happened", not as an error. Joysticks have no rumble
// here: a force-feedback wheel is a haptics device, not two motors.
bool rumble(u8 gamepad, f32 low, f32 high, u32 ms) noexcept;

// --- force feedback -------------------------------------------------------------------------
//
// A force-feedback wheel is not two motors: it is a motor on the steering axis that a game
// drives with *forces*, and the shapes of those forces are the device's own. This module
// exposes the four that every wheel driver implements — a constant force, a spring towards a
// centre, a damper against velocity, and (read-only, in `HapticInfo`) friction and a sine — over
// the raw joystick slot space, because a wheel has no gamepad mapping.
//
// **Opening a wheel turns its own centring off.** The driver applies an autocentre spring until
// something takes the device over; SDL's open is that takeover, so a wheel that felt sprung
// goes limp the moment a slot holds it. That is not a bug to fix here: a game that opens a
// wheel must set a spring or a damper itself (`set_spring(slot, 0.5f, 0.0f)` is a reasonable
// resting force) and keep it running for as long as it holds the device.
//
// Every call returns false where the device, the driver, or the platform does not support it,
// so false means "no force was applied", never "something went wrong".

// Which effects a slot's haptics device supports, and how many axes it drives. Valid after
// haptics_open; an unopened or empty slot answers false from haptics_info and nothing else.
struct HapticInfo {
  bool constant = false;  // a force of a fixed level along an axis
  bool spring = false;    // a force towards a centre, proportional to the distance from it
  bool damper = false;    // a force against the axis's velocity
  bool friction = false;  // a force against motion, independent of speed
  bool sine = false;      // a periodic force: rumble strips, engine vibration
  u32 axes = 0;           // how many axes the device can be pushed along
};

// Opens the haptics device behind a raw joystick slot. False when the slot is empty, the device
// has no haptics, or SDL will not open it. Opening twice is a no-op that answers true.
bool haptics_open(u8 joystick);
// Stops every running force and closes the device. Safe on a slot that was never opened;
// shutdown() and a disconnect do it too.
void haptics_close(u8 joystick) noexcept;
// What the open device supports. False (leaving `out` untouched) when the slot has no open
// haptics device.
bool haptics_info(u8 joystick, HapticInfo& out) noexcept;

// A force of a fixed level along the device's first axis, -1..1: negative pushes one way,
// positive the other, 0 stops pushing. The effect runs until stop_forces or a new level.
bool set_constant_force(u8 joystick, f32 level) noexcept;
// A spring pulling the axis towards `center` (-1..1, 0 being the middle of the travel) with
// `strength` 0..1. This is what replaces the driver's own centring.
bool set_spring(u8 joystick, f32 strength, f32 center) noexcept;
// A force against the axis's velocity, `strength` 0..1: weight rather than centring.
bool set_damper(u8 joystick, f32 strength) noexcept;
// Stops and destroys every effect this module started on the slot. The wheel goes limp.
bool stop_forces(u8 joystick) noexcept;

class Window;

struct WindowDesc {
  const char* title = "engine";
  u32 width = 1280;  // logical size; the pixel size may differ on high-DPI displays
  u32 height = 720;
  bool resizable = true;
  // Create able to carry a Vulkan surface (backend/vulkan/surface.h). Such a window is for a GPU
  // device, so when the test environment's GPU-lock switch is on its process takes its hold on
  // the machine-wide GPU lock *before* the window appears — in create(), or in show() for one
  // created hidden — and keeps it until the window goes (foundation/gpu_lock/device_hold.h): a
  // process that has to wait for the lock waits with nothing on screen, instead of showing a
  // window that answers nothing ("Not responding") for as long as somebody else has the GPU. A
  // hidden window that is never shown takes nothing. With the switch off — every session a
  // person starts — nothing is taken either way.
  bool vulkan = true;
  bool hidden = false;
  // No title bar and no border, placed at the top-left corner of the primary display: a window
  // the size of the display then covers it exactly, which is what lets the compositor hand it the
  // display (independent flip) instead of composing it (docs/subsystems/window.md, "Borderless
  // windows and presentation"). Not fullscreen: the display mode never changes and other windows
  // can still be brought over it.
  bool borderless = false;
  // **A child window** (docs/subsystems/window.md, "Child windows"): with a parent, the window is a
  // rectangle inside the parent's client area — at (`x`, `y`) in the parent's coordinates, `width`
  // by `height`, no border, moved with the parent and clipped by it, and movable inside it with
  // `set_rect`. It carries a Vulkan surface exactly as a top-level window does (`vulkan`), which is
  // what lets engine-editor keep its swapchain in a rectangle of a window whose other children are
  // an OS webview. Its events arrive through the **parent's** `poll`, marked `Event::child` (one
  // queue, so polling the child alone would drop the parent's). Keyboard focus moves into it with
  // `focus()`. Windows only for now: elsewhere `create` refuses it with the reason.
  const Window* parent = nullptr;
  i32 x = 0;
  i32 y = 0;
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
  // The window's size in its own coordinates, the ones `Event::x`/`y` are in. On a display that
  // scales it is not the framebuffer's (a 1280 x 720 window at 150 % is 1920 x 1080 pixels), so a
  // host that turns the pointer into a pixel of its picture multiplies by the two sizes' ratio
  // (engine-editor's picking and gizmo). Asked of the platform each call; false without a window.
  bool logical_size(u32& width, u32& height) const noexcept;
  // The window's centre on the desktop, in the desktop's coordinates: which display it is on, for
  // a host that asks the display what it can show (HDR output's peak and paper white, E39). False
  // without a window or where the platform does not say (Wayland).
  bool desktop_center(i32& x, i32& y) const noexcept;
  // The scale of the display the window is on, as the system's settings give it (1.5 at 150 %),
  // which `EventKind::ScaleChanged` reports when it changes; 1 without a window.
  f32 display_scale() const noexcept;
  // A top-level window's new size in its own coordinates (the platform answers with `Resized`).
  // False for a child (its parent places it: `set_rect`) or with no window.
  bool set_size(u32 width, u32 height) noexcept;
  // **Fullscreen without a mode change** (docs/subsystems/window.md, "Borderless windows and
  // presentation"): SDL's borderless desktop fullscreen — `SDL_SetWindowFullscreenMode` with no
  // mode, then `SDL_SetWindowFullscreen` — so the window takes the bounds of the display it is on
  // and the display mode never changes, for the reason `WindowDesc::borderless` gives: covering
  // the display is what earns the flip, and an exclusive mode would only add a mode switch and
  // keep other windows from coming over it. The platform answers with `Resized` (the display's
  // pixels on the way in; on the way back SDL restores the windowed size and place itself), and
  // `pixel_width()`/`pixel_height()` follow it as they do for `set_size`. On a hidden window SDL
  // keeps the request and applies it when the window is shown. False for a child or with no
  // window.
  bool set_fullscreen(bool on) noexcept;
  // The state last asked for, or that SDL last reported (its enter- and leave-fullscreen events,
  // read in `poll`); false with no window.
  bool fullscreen() const noexcept { return fullscreen_; }

  // Pulls the next pending event for this window (and process-wide Quit). False when none
  // is left; call until it returns false once per frame.
  bool poll(Event& out);

  void set_title(const char* title) noexcept;
  void show() noexcept;
  void* native() const noexcept { return handle_; }  // SDL_Window*
  // The platform's own handle for the window — an HWND on Windows — for a module that embeds a
  // platform view in it (foundation/webview). Null where there is none to give.
  void* os_handle() const noexcept;

  // A child window's (`WindowDesc::parent`) place and size in its parent, in the parent's
  // coordinates; it is raised above its siblings, and its pixel size follows at once. False for a
  // top-level window or with no window.
  bool set_rect(i32 x, i32 y, u32 width, u32 height) noexcept;
  // Shows or hides a child window without taking the GPU lock again (show() is the top-level's).
  bool set_visible(bool visible) noexcept;
  bool is_child() const noexcept { return child_; }
  // Gives the window the keyboard: what a host does when the pointer presses in a child window,
  // since a child is not activated by a click the way a top-level window is.
  void focus() noexcept;

  // **Relative mouse mode**: the cursor is hidden and held inside the window, and `MouseMove`
  // keeps reporting motion in `dx`/`dy` when the pointer would have stopped at the screen's edge,
  // which is what a first-person camera needs (docs/subsystems/window.md, "Relative mouse"). Off
  // by default. SDL keeps the mode per window and suspends it while the window has no focus, so a
  // player who switches away gets the cursor back. False when there is no window or the platform
  // refused; `relative_mouse()` reads back what SDL says is in effect for this window.
  //
  // **A child window** (`WindowDesc::parent`) gets this module's own relative mode, because SDL
  // runs its mode only for the window holding its keyboard focus, which a child never is, and
  // swallows a flagged child's motion (docs/subsystems/window.md, "Relative mode on an adopted
  // child"): on the platform's pointer messages, the cursor hidden and confined to the child, each
  // motion's `dx`/`dy` read from the child's pointer and the cursor put back where the gesture
  // began (`x`/`y` stay there), and shown there again when the mode ends or the window loses focus.
  // That only for a real pointer — the cursor over the child, its window in front; a posted
  // gesture on a window in the background is reported as it comes, the same deltas, and the
  // desktop's cursor is left alone. One child at a time; asking again for the mode it already has
  // does nothing. For a child, `relative_mouse()` is whether the cursor is held, and
  // `relative_mouse_asked()` whether the mode was asked for.
  bool set_relative_mouse(bool enabled) noexcept;
  bool relative_mouse() const noexcept;
  bool relative_mouse_asked() const noexcept;
  // When relative mode last came on (`set_relative_mouse(true)` returning true), on the events'
  // clock (`Event::timestamp_ns`); 0 while it is off. Motion queued before it is the absolute
  // pointer's, whose `dx`/`dy` run from wherever the cursor last was and can be a screen's width in
  // one event.
  u64 relative_mouse_since_ns() const noexcept { return relative_since_ns_; }

  // Puts an event on this window's queue as if the platform had delivered it, so the next
  // `poll()` returns it through exactly the path a real one takes. Keys (`scancode`), mouse
  // buttons, motion (`dx`/`dy`), the wheel and a dropped file (`text`, kept alive by the caller
  // until it is polled) only: a gamepad or joystick event names a device
  // slot this module assigned to a real device, so there is nothing to fake it for. False for any
  // other kind, with no window, or when SDL's queue refuses it.
  //
  // It is for driving a window with nobody at the keyboard — an end-to-end test, a smoke run on a
  // shared desktop — without handing keystrokes to whatever window the OS has focused, which is
  // what synthesizing them at the OS level would do (docs/subsystems/apps.md, `--inject-input`).
  bool push_event(const Event& event) noexcept;
  // Posts an event to the **platform's** queue for this window (Windows: `PostMessageW` to its
  // HWND), so it takes the path a real one takes from there — the window procedure, the
  // platform's keyboard focus, SDL's translation — where `push_event` starts after all of it.
  // Keys (`key`: Control, Shift, Alt, Escape, Delete, letters, digits), mouse buttons 1 to 3 at
  // (`x`, `y`) and motion only. It is how a test proves a key reaches whoever has the platform's
  // focus and a click moves that focus (engine-editor's `--test-keys`), still without handing a
  // keystroke to whatever window the OS has in front. False elsewhere, for any other kind, or with
  // no window. `post_to_focus` posts to the window that has this thread's keyboard focus instead,
  // whichever it is — this window, a child, or another module's (a web view's).
  bool post_os_event(const Event& event) noexcept;
  static bool post_to_focus(const Event& event) noexcept;
  // Whether this window has the platform's keyboard focus in this thread (Windows: `GetFocus`).
  bool has_os_focus() const noexcept;

  // SDL's id for the window, the one its events name; 0 with no window.
  u32 id() const noexcept { return id_; }

  // **What the platform and SDL say about the pointer and this window, now**: for a trace of a
  // gesture on a real pointer (engine-editor's `--trace-input`), where what went wrong is a
  // disagreement between the two — whose the keyboard is, where the window is, where the cursor
  // went (docs/subsystems/window.md, "Relative mode on an adopted child"). Read on demand, never
  // in a frame that does not trace.
  struct PointerReport {
    bool relative_asked = false;  // this window asked for relative mode (`relative_mouse_asked`)
    bool sdl_flag = false;        // SDL's own per-window flag on this window (never on a child)
    bool relative_on = false;     // SDL's relative mode is in effect, for whichever window
    u32 keyboard_focus = 0;  // SDL's keyboard focus (the window relative motion goes to), 0 none
    u32 mouse_focus = 0;     // SDL's mouse focus, 0 none
    i32 cursor_x = 0;        // the platform's cursor, in this window's client coordinates
    i32 cursor_y = 0;
    bool cursor_inside = false;  // ... inside the client area
    i32 sdl_x = 0;               // SDL's idea of the window's place (desktop) and size
    i32 sdl_y = 0;
    i32 sdl_width = 0;
    i32 sdl_height = 0;
    i32 os_x = 0;  // the platform's: the client area's corner on the desktop, and its size
    i32 os_y = 0;
    i32 os_width = 0;
    i32 os_height = 0;
    bool clipped = false;  // the cursor is confined (ClipCursor) to less than the desktop
    i32 clip_left = 0;     // ... to this rectangle, desktop coordinates
    i32 clip_top = 0;
    i32 clip_right = 0;
    i32 clip_bottom = 0;
  };
  PointerReport pointer_report() const noexcept;

 private:
  void* handle_ = nullptr;
  u32 id_ = 0;
  u32 pixel_width_ = 0;
  u32 pixel_height_ = 0;
  gpu_lock::DeviceHold gpu_hold_;  // a Vulkan window's share of the GPU lock (WindowDesc::vulkan)
  bool vulkan_ = false;            // created with WindowDesc::vulkan: show() takes the hold
  bool child_ = false;             // created with WindowDesc::parent
  bool fullscreen_ = false;        // set_fullscreen's state, followed by SDL's events
  u64 relative_since_ns_ = 0;      // set_relative_mouse(true)'s moment, SDL_GetTicksNS
};

// **What the desktop is doing, now** (roadmap A40): the desktop this thread's windows are on,
// whose window the platform has in front, and whether the session is locked — what the platform's
// focus and pointer rules depend on, so that a report of a gesture or a key that went wrong says
// what the desktop was doing at the time (engine-editor's `--test-look-os` and `--test-keys`;
// docs/subsystems/window.md, "What the child's messages say"). Windows only; elsewhere every field
// is empty or unknown. Cheap enough for a test's frame, not for every frame.
struct DesktopState {
  std::string desktop;     // this thread's: "WinSta0\Default", or a test's private one
  std::string foreground;  // the executable whose window is in front; "" when no window is (always
                           // so off the input desktop) or its process cannot be read
  bool foreground_own = false;  // ... and it is this process
  i32 locked = -1;              // the session is locked: 1, it is not: 0, unknown: -1
};
DesktopState desktop_state();

// **The platform's open-file dialog**, modal on `owner` (a top-level window): the files with one of
// `extensions` (without dots; empty offers every file), the chosen path in `path` (UTF-8). False
// when it was cancelled, or where the platform has none here (every platform but Windows so far).
// It is a dialog a person asked for in a desktop tool, never an error dialog (AGENTS.md: no
// engine binary opens a dialog when it fails), and a host never shows it in a test: engine-editor
// calls it only for `editor.pick_file` in a window run with no test flag (docs/subsystems/apps.md).
bool open_file_dialog(const Window& owner, std::span<const std::string> extensions,
                      std::string& path);

}  // namespace engine::window
