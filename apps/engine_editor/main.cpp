// engine-editor: the engine's window on a document (docs/subsystems/apps.md, "engine-editor";
// ADR-0054; roadmap A34, A37). One native window holding two children that tile and never overlap:
// the web shell (apps/engine_shell — outliner, properties, journal, command line) in the platform's
// web view, and the renderer's live viewport in a child window placed over the page's viewport
// cell, with engine-view's fly camera, picking from the visibility buffer, a translate gizmo whose
// every drag is one transaction, undo, redo and delete. Both are clients of **one host**: the
// editor runs the protocol's listener in process over the session it holds (protocol::Listener,
// editor_methods.h), the page speaks only the protocol over it, and the viewport follows the
// document after every transaction, whoever made it. Nothing is composited and nothing crosses per
// frame: the viewport presents at the display's rate, the page repaints on state, and the two
// share only state over the protocol and the cell's rectangle when it changes.
//
// The document is opened through `protocol::Session` and every edit goes through its checked write
// path (editor_document.h); the pointer and keys are editor_input.h's, their maths
// viewport_math.h's; everything drawn is the renderer's (editor_viewport.h). `--no-shell` is the
// viewport alone in the window, `--offscreen` the same frames with no window, and `--frames`,
// `--pick`, `--move`, `--drag` and `--capture` are how a test drives either mode and reads the
// answer from the one JSON line it prints at the end.

#include "../engine_view/fly_camera.h"
#include "../engine_view/window_input.h"
#include "editor_document.h"
#include "editor_input.h"
#include "editor_methods.h"
#include "editor_ui.h"
#include "editor_viewport.h"
#include "viewport_math.h"

#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/log/log.h>
#include <core/platform/cpu_baseline.h>
#include <core/platform/process.h>
#include <core/time/time.h>
#include <domain/gfx/backend/vulkan/swapchain.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/device.h>
#include <domain/protocol/listener.h>
#include <domain/protocol/rpc.h>
#include <foundation/image/png.h>
#include <foundation/input/input.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>
#include <foundation/net/socket.h>
#include <foundation/tunables/tunables.h>
#include <foundation/window/backend/vulkan/surface.h>
#include <foundation/window/window.h>
#include <systems/renderer/capture.h>

#if defined(ENGINE_EDITOR_WEBVIEW)
#include <foundation/webview/webview.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <engine_build_stamp.h>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

using namespace engine;

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_editor, "editor");

tunables::Int present_max_hz{"editor.present.max_hz", 120, 1, 1000,
                             "The most frames a second the editor's window draws: it shares the "
                             "desktop, so it never runs unthrottled"};

// How long an embedded window waits for the page's first `editor.layout` before it gives up: the
// web view's first start on a machine makes its profile, which takes seconds, not minutes.
constexpr i64 k_layout_timeout_ns = 60'000'000'000;
// How long `--test-page` waits for the page's checks (and a reconnect) once its frames are drawn.
constexpr i64 k_page_checks_timeout_ns = 120'000'000'000;

constexpr const char* k_usage =
    "engine-editor --doc <dir> [--create] [--actor <name>] [--width N] [--height N]\n"
    "              [--embedded | --no-shell] [--listen 127.0.0.1:<port>] [--shell <dir>]\n"
    "              [--offscreen] [--frames N] [--pick X,Y|center] [--move X,Y,Z]\n"
    "              [--drag X0,Y0 X1,Y1] [--look DX,DY] [--capture out.png] [--ddc <dir>]\n"
    "              [--webview-profile <dir>] [--resize-to W,H] [--test-keys] [--test-page]\n"
    "              [--test-drop-socket-at N] [--test-relayout-at N] [--trace-input <file>]\n"
    "engine-editor --version\n"
    "  --doc        the document directory (protocol::Session opens it)\n"
    "  --create     create the document when the directory holds none\n"
    "  --actor      who the edits are attributed to (role: director)\n"
    "  (window)     the web shell in the platform's web view beside the native viewport, both\n"
    "               clients of the protocol listener the editor runs on 127.0.0.1; where there is\n"
    "               no web view (no WebView2 runtime, a build without ENGINE_WITH_WEBVIEW) the\n"
    "               viewport fills the window and the printed \"shell\" URL opens in a browser\n"
    "  --embedded   the web shell beside the viewport, or exit 3 saying why there is none\n"
    "  --no-shell   the viewport alone in the window, and no listener unless --listen asks\n"
    "  --listen     the listener's address (default 127.0.0.1:0, a free port); loopback only.\n"
    "               Prints one line first: "
    "{\"listen\":\"ws://...\",\"token\":\"...\",\"shell\":...}\n"
    "  --shell      the shell's files (default <bin>/../shell)\n"
    "  --offscreen  no window: draw --frames frames into the renderer's own target\n"
    "  --frames N   stop after N frames of the viewport (0: until the window closes); with the\n"
    "               shell, counted from when the page last placed the viewport\n"
    "  --pick X,Y   after the second frame, select what is under the pointer at (X, Y), in the\n"
    "               viewport's window coordinates (the picture's pixels offscreen or where "
    "nothing\n"
    "               scales); `center` is the middle of the picture\n"
    "  --move X,Y,Z after picking, set the selection's translation in one transaction\n"
    "  --drag X0,Y0 X1,Y1  after picking, press at the first point, move to the second over\n"
    "               three frames and release, as a pointer would (frames 3 to 8): on a gizmo\n"
    "               handle that is one transaction; needs --pick and --frames 10 or more\n"
    "  --look DX,DY after picking, hold the right button over the viewport and move the pointer\n"
    "               by (DX, DY) over three frames, as the look gesture does (frames 3 to 7)\n"
    "  --trace-input <file>  one JSON line per pointer event the window gives the editor (with\n"
    "               what SDL and the platform said about relative mode, focus and the cursor) and\n"
    "               per frame (the camera's yaw and pitch), written when the run ends\n"
    "  --capture    offscreen: the last frame, overlay and all, as a PNG\n"
    "  --webview-profile  where the web view keeps its profile (default <doc>/.editor/webview,\n"
    "               in a directory that ignores itself: <doc>/.editor/.gitignore is \"*\")\n"
    "  Test flags, the embedded window's (frames counted since the page last moved the viewport,\n"
    "  as --pick's are): --resize-to W,H resizes the window at frame 4; --test-keys (with --move\n"
    "  and --frames 32 or more) clicks the viewport and posts Ctrl+Z to it through the platform's\n"
    "  queue, then gives the page the keyboard and posts Ctrl+Z again; --test-look-os DX,DY posts\n"
    "  the look gesture to the viewport through the platform's queue (frames 3 to 10); "
    "--test-page\n"
    "  loads the page\n"
    "  with its test hook (?test=1) and waits for its checks; --test-drop-socket-at N shuts every\n"
    "  connection, and --test-relayout-at N sends editor.relayout, from frame N once the page's\n"
    "  checks are in.\n"
    "  --version    the commit this binary was built from, as one JSON line\n"
    "In the viewport: left click picks; drag a gizmo handle to move the selection along it "
    "(Escape\n"
    "cancels); Ctrl+Z undo, Ctrl+Y or Ctrl+Shift+Z redo, Delete deletes the selection.\n"
    "Exit codes: 0 ok, 1 runtime error, 2 usage, 3 no display, no device or (--embedded) no web\n"
    "view.\n";

enum class Shell : u8 { Auto, Embedded, None };

struct Options {
  std::string doc;
  bool create = false;
  std::string actor = "editor";
  u32 width = 1600;  // the shell's three columns want about 950 CSS pixels, at 150 % about 1430
  u32 height = 900;
  bool offscreen = false;
  Shell shell = Shell::Auto;
  std::string listen;  // empty: the default for the mode
  u16 listen_port = 0;
  std::string shell_dir;
  u32 frames = 0;
  bool pick = false;
  bool pick_center = false;
  u32 pick_x = 0;
  u32 pick_y = 0;
  bool move = false;
  f64 move_to[3] = {0.0, 0.0, 0.0};
  bool drag = false;
  f64 drag_from[2] = {0.0, 0.0};
  f64 drag_to[2] = {0.0, 0.0};
  std::string capture;
  std::string ddc;
  bool look = false;
  f64 look_by[2] = {0.0, 0.0};
  std::string webview_profile;  // empty: <doc>/.editor/webview
  bool resize = false;
  f64 resize_to[2] = {0.0, 0.0};
  bool test_keys = false;
  bool test_page = false;
  u32 drop_at = 0;          // 0: never
  u32 relayout_at = 0;      // 0: never
  std::string trace_input;  // --trace-input: one JSON line per pointer event and per frame
  bool look_os = false;     // --test-look-os
  f64 look_os_by[2] = {0.0, 0.0};
};

bool parse_u32(std::string_view s, u32& out) {
  if (s.empty()) return false;
  u64 v = 0;
  for (const char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<u64>(c - '0');
    if (v > 0xffffffffull) return false;
  }
  out = static_cast<u32>(v);
  return true;
}

// "a,b" or "a,b,c" of doubles.
bool parse_list(const std::string& s, f64* out, u32 count) {
  usize at = 0;
  for (u32 i = 0; i < count; ++i) {
    const usize comma = s.find(',', at);
    const std::string part =
        s.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
    if (part.empty()) return false;
    char* end = nullptr;
    out[i] = std::strtod(part.c_str(), &end);
    if (end == nullptr || *end != '\0') return false;
    if (i + 1 < count && comma == std::string::npos) return false;
    at = comma == std::string::npos ? s.size() : comma + 1;
  }
  return at >= s.size();
}

int usage(const char* why) {
  std::fprintf(stderr, "engine-editor: %s\n%s", why, k_usage);
  return 2;
}

int unavailable(const char* what, const std::string& why) {
  std::fprintf(stderr, "engine-editor: %s%s%s\n", what, why.empty() ? "" : ": ", why.c_str());
  return 3;
}

// engine-view's fly camera, fed only what the overlay does not take: WASD and E/Q to move, the
// pointer to look while the right button is held over the picture (apps/engine_view/fly_camera.h).
struct Camera {
  view::FlyState state;
  view::FlyParams params;
  view::FlyActions actions;
  input::ActionMap map;  // what `input` points at: it has to live as long as the state does
  input::InputState input;
  view::EdgeEvents edge;
  FixedStepClock clock{240, 60};
  SimTick fed{0};
  bool looking = false;

  bool start(const renderer::Camera& from, f32 radius, std::string& error) {
    state = view::fly_state_from_camera(from);
    params.speed = std::max(1.0f, radius * 0.5f);
    map = view::default_fly_map();
    if (!view::resolve_fly_actions(map, actions, &error)) return false;
    input.set_map(map);
    return true;
  }
  static void on_tick(void* user, SimTick, const input::InputState& state) {
    Camera& self = *static_cast<Camera*>(user);
    view::fly_tick(self.state, state, self.actions, self.params);
  }
  // Runs the ticks the clock owes: the events gathered since the last call carry the first of them
  // (`edge.add` with `next()`), and a frame that owed none keeps them for the next.
  SimTick next() const { return SimTick{fed.value + 1}; }
  void advance(i64 ns) {
    clock.advance(ns);
    while (clock.step()) {
    }
    if (clock.tick().value <= fed.value) return;
    input::feed_ticks(input, edge.events(), 0, next(), clock.tick(),
                      input::ReplayOptions{&Camera::on_tick, this});
    fed = clock.tick();
    edge.clear();
  }
  renderer::Camera view() const { return view::fly_view(state, 0.9599310886f, 0.05f); }
};

// --trace-input: one JSON line per pointer event the window's queue gives the editor, before
// anything filters it — its kind, where, how far, SDL's timestamp, whether SDL said it was the
// child's — with what SDL and the platform said at that moment about relative mode, the keyboard's
// owner, the mouse's owner and the cursor (`Window::PointerReport`), and one line per frame that
// had pointer events or was looking, with the camera's yaw and pitch after the frame's ticks. It is
// how the look gesture's failure on a real pointer was found (window.md, "Relative mode on an
// adopted child"), and the first thing to turn on when a gesture misbehaves on someone's machine.
struct InputTrace {
  // The lines are kept and written when the run ends (a gesture's trace is a few hundred lines),
  // so tracing costs a frame no file write.
  std::string path;
  std::string text;
  bool file = false;
  u32 events = 0;  // this frame's

  bool open(const std::string& to) {
    path = to;
    file = !to.empty();
    return !file || io::write_file(path, "") == io::Status::Ok;
  }
  ~InputTrace() {
    if (file) (void)io::write_file(path, text);
  }
  static const char* kind_name(window::EventKind k) noexcept {
    switch (k) {
      case window::EventKind::MouseMove: return "move";
      case window::EventKind::MouseButtonDown: return "down";
      case window::EventKind::MouseButtonUp: return "up";
      case window::EventKind::MouseWheel: return "wheel";
      case window::EventKind::FocusGained: return "focus_gained";
      case window::EventKind::FocusLost: return "focus_lost";
      default: return nullptr;
    }
  }
  void line(const JsonValue& v) {
    text += write_json(v);
    text += '\n';
  }
  void event(const window::Event& e, u64 frame, const window::Window& view) {
    const char* kind = kind_name(e.kind);
    if (!file || kind == nullptr) return;
    ++events;
    const window::Window::PointerReport r = view.pointer_report();
    JsonValue v = JsonValue::object();
    v["frame"] = JsonValue(static_cast<f64>(frame));
    v["kind"] = JsonValue(kind);
    v["button"] = JsonValue(static_cast<f64>(e.button));
    v["x"] = JsonValue(static_cast<f64>(e.x));
    v["y"] = JsonValue(static_cast<f64>(e.y));
    v["dx"] = JsonValue(static_cast<f64>(e.dx));
    v["dy"] = JsonValue(static_cast<f64>(e.dy));
    v["t_ns"] = JsonValue(static_cast<f64>(e.timestamp_ns));
    v["child"] = JsonValue(e.child);
    v["view_id"] = JsonValue(static_cast<f64>(view.id()));
    v["relative_asked"] = JsonValue(r.relative_asked);
    v["relative_on"] = JsonValue(r.relative_on);
    v["keyboard_focus"] = JsonValue(static_cast<f64>(r.keyboard_focus));
    v["mouse_focus"] = JsonValue(static_cast<f64>(r.mouse_focus));
    v["cursor"] = JsonValue::array();
    v["cursor"].push_back(JsonValue(static_cast<f64>(r.cursor_x)));
    v["cursor"].push_back(JsonValue(static_cast<f64>(r.cursor_y)));
    v["cursor_inside"] = JsonValue(r.cursor_inside);
    v["sdl_rect"] = JsonValue::array();
    for (const i32 n : {r.sdl_x, r.sdl_y, r.sdl_width, r.sdl_height})
      v["sdl_rect"].push_back(JsonValue(static_cast<f64>(n)));
    v["os_rect"] = JsonValue::array();
    for (const i32 n : {r.os_x, r.os_y, r.os_width, r.os_height})
      v["os_rect"].push_back(JsonValue(static_cast<f64>(n)));
    v["clipped"] = JsonValue(r.clipped);
    if (r.clipped) {
      v["clip"] = JsonValue::array();
      for (const i32 n : {r.clip_left, r.clip_top, r.clip_right, r.clip_bottom})
        v["clip"].push_back(JsonValue(static_cast<f64>(n)));
    }
    line(v);
  }
  void frame(u64 frame, f32 yaw, f32 pitch, bool looking) {
    if (!file || (events == 0 && !looking)) return;
    events = 0;
    JsonValue v = JsonValue::object();
    v["frame"] = JsonValue(static_cast<f64>(frame));
    v["kind"] = JsonValue("frame");
    v["looking"] = JsonValue(looking);
    v["yaw_deg"] = JsonValue(static_cast<f64>(yaw) * 57.29577951308232);
    v["pitch_deg"] = JsonValue(static_cast<f64>(pitch) * 57.29577951308232);
    line(v);
  }
};

struct Summary {
  u64 frames = 0;
  u32 records = 0;
  u32 instances = 0;
  Id128 selected;
  bool picked = false;
  bool moved = false;
  u32 rebuilds = 0;
  u32 transactions = 0;  // the session's journal, whole
  u32 undo_position = 0;
  u32 width = 0;  // the picture's pixels: the swapchain's, or the offscreen target's
  u32 height = 0;
  bool embedded = false;          // the web shell was beside the viewport
  u16 listen = 0;                 // the listener's port, 0 when there was none
  u32 layouts = 0;                // editor.layout notifications received
  editor::ViewportRect viewport;  // where the last layout put the viewport, in window coordinates
  editor::EditorInput::Stats input;
  // Every placement a layout made (the first 16): the window's client size, the child's
  // rectangle, whether it had to be clamped, and whether the page was in its narrow layout.
  struct Placed {
    u32 client_width = 0;
    u32 client_height = 0;
    editor::ViewportRect rect;
    bool clamped = false;
    bool narrow = false;
    bool from_page = true;  // a layout the page sent; false: the editor's own re-placement of the
                            // last one (a resize, a change of scale) before the page's answer
  };
  Vector<Placed> placements;
  f32 display_scale = 1.0f;  // the display's scale under the window, last read
  u32 scale_changes = 0;     // ScaleChanged events
  u32 relayouts = 0;         // editor.relayout notifications sent
  u32 relayout_reached = 0;  // connections the last one reached
  u32 layouts_at_relayout = 0;
  u32 drops = 0;  // --test-drop-socket-at: connections shut
  i64 frames_at_drop = -1;
  i64 frames_at_reconnect = -1;
  // --look: the camera's yaw and pitch before and after the gesture, and whether SDL said the
  // child was in relative mode while the button was held.
  bool looked = false;
  f32 look_yaw[2] = {0.0f, 0.0f};
  f32 look_pitch[2] = {0.0f, 0.0f};
  bool look_relative = false;        // in effect (the parent holds SDL's keyboard focus)
  bool look_relative_asked = false;  // asked for, whether or not in effect
  // --test-look-os: the gesture through the platform's queue (window.md, "Relative mode on an
  // adopted child"): the yaw it turned and the yaw the posted motion should turn, whether relative
  // mode was in effect, whether SDL's flag ever sat on the child itself (what swallowed every
  // motion before 2026-10-12), and where the cursor was held while the button was down.
  bool looked_os = false;
  f32 look_os_yaw[2] = {0.0f, 0.0f};
  f64 look_os_expected_deg = 0.0;
  bool look_os_asked = false;
  bool look_os_in_effect = false;
  bool look_os_child_flag = false;
  bool look_os_clip_inside = false;
  bool look_os_cursor_inside = false;
  bool look_os_released_by_test = false;
  // --test-keys: whose the keyboard was at each step, and what each Ctrl+Z did.
  u32 sdl_keys = 0;  // KeyDown events SDL delivered to the editor, all run
  bool keys_tested = false;
  bool viewport_focus = false;  // after the posted click into the viewport
  bool posted_to_viewport = false;
  u32 undos_from_viewport = 0;  // the editor's undos after Ctrl+Z went to the viewport
  bool page_focus = false;      // after the web view was given the keyboard
  bool posted_to_page = false;  // the platform had a focused window in this thread to post to
  u32 sdl_keys_from_page = 0;   // KeyDown events SDL delivered after the page had the keyboard
  u32 undos_after_page = 0;
  u32 undo_position_before_page = 0;
  u32 undo_position_after_page = 0;
  Vector<editor::EditorState::Report> reports;  // --test-page: the page's own checks
};

void print_summary(const Summary& s, const Options& o) {
  JsonValue out = JsonValue::object();
  out["tool"] = JsonValue("engine-editor");
  out["mode"] = JsonValue(o.offscreen ? "offscreen" : "window");
  out["frames"] = JsonValue(s.frames);
  out["records"] = JsonValue(s.records);
  out["instances"] = JsonValue(s.instances);
  out["selected"] = s.selected.is_null() ? JsonValue() : JsonValue(editor::hex(s.selected));
  out["picked"] = JsonValue(s.picked);
  out["moved"] = JsonValue(s.moved);
  out["rebuilds"] = JsonValue(s.rebuilds);
  out["width"] = JsonValue(s.width);
  out["height"] = JsonValue(s.height);
  out["transactions"] = JsonValue(s.transactions);
  out["undo_position"] = JsonValue(s.undo_position);
  out["embedded"] = JsonValue(s.embedded);
  out["listen"] = s.listen == 0 ? JsonValue() : JsonValue(u64{s.listen});
  out["layouts"] = JsonValue(s.layouts);
  if (s.layouts > 0) {
    JsonValue rect = JsonValue::array();
    rect.push_back(JsonValue(static_cast<i64>(s.viewport.x)));
    rect.push_back(JsonValue(static_cast<i64>(s.viewport.y)));
    rect.push_back(JsonValue(s.viewport.width));
    rect.push_back(JsonValue(s.viewport.height));
    out["viewport"] = std::move(rect);
  } else {
    out["viewport"] = JsonValue();
  }
  // What one pick costs: it reads the whole visibility buffer back for one pixel, which is a
  // click's price and not a frame's (`Viewport::pick`). Null before any pick.
  out["pick_ms"] = s.input.pick_ms < 0.0 ? JsonValue() : JsonValue(s.input.pick_ms);
  out["drags"] = JsonValue(s.input.drags);
  if (s.input.last_transaction >= 0) {
    JsonValue drag = JsonValue::object();
    drag["axis"] = JsonValue(editor::axis_name(s.input.last_axis));
    drag["delta"] = JsonValue(s.input.last_delta);
    drag["transaction"] = JsonValue(s.input.last_transaction);
    drag["rationale"] = JsonValue(s.input.last_rationale);
    out["drag"] = std::move(drag);
  } else {
    out["drag"] = JsonValue();
  }
  JsonValue placements = JsonValue::array();
  for (const Summary::Placed& p : s.placements) {
    JsonValue one = JsonValue::object();
    JsonValue client = JsonValue::array();
    client.push_back(JsonValue(p.client_width));
    client.push_back(JsonValue(p.client_height));
    one["client"] = std::move(client);
    JsonValue rect = JsonValue::array();
    rect.push_back(JsonValue(static_cast<i64>(p.rect.x)));
    rect.push_back(JsonValue(static_cast<i64>(p.rect.y)));
    rect.push_back(JsonValue(p.rect.width));
    rect.push_back(JsonValue(p.rect.height));
    one["viewport"] = std::move(rect);
    one["clamped"] = JsonValue(p.clamped);
    one["narrow"] = JsonValue(p.narrow);
    one["from_page"] = JsonValue(p.from_page);
    placements.push_back(std::move(one));
  }
  out["placements"] = std::move(placements);
  out["display_scale"] = JsonValue(static_cast<f64>(s.display_scale));
  out["scale_changes"] = JsonValue(s.scale_changes);
  out["relayouts"] = JsonValue(s.relayouts);
  out["relayout_reached"] = JsonValue(s.relayout_reached);
  out["layouts_at_relayout"] = JsonValue(s.layouts_at_relayout);
  out["drops"] = JsonValue(s.drops);
  out["frames_at_drop"] = JsonValue(s.frames_at_drop);
  out["frames_at_reconnect"] = JsonValue(s.frames_at_reconnect);
  if (s.looked) {
    JsonValue look = JsonValue::object();
    look["yaw_deg"] =
        JsonValue(static_cast<f64>(s.look_yaw[1] - s.look_yaw[0]) * 57.29577951308232);
    look["pitch_deg"] =
        JsonValue(static_cast<f64>(s.look_pitch[1] - s.look_pitch[0]) * 57.29577951308232);
    look["relative"] = JsonValue(s.look_relative);
    look["relative_asked"] = JsonValue(s.look_relative_asked);
    out["look"] = std::move(look);
  } else {
    out["look"] = JsonValue();
  }
  if (s.looked_os) {
    JsonValue look = JsonValue::object();
    look["yaw_deg"] =
        JsonValue(static_cast<f64>(s.look_os_yaw[1] - s.look_os_yaw[0]) * 57.29577951308232);
    look["expected_yaw_deg"] = JsonValue(s.look_os_expected_deg);
    look["asked"] = JsonValue(s.look_os_asked);
    look["in_effect"] = JsonValue(s.look_os_in_effect);
    look["child_flag"] = JsonValue(s.look_os_child_flag);
    look["clip_inside"] = JsonValue(s.look_os_clip_inside);
    look["cursor_inside"] = JsonValue(s.look_os_cursor_inside);
    look["released_by_test"] = JsonValue(s.look_os_released_by_test);
    out["look_os"] = std::move(look);
  } else {
    out["look_os"] = JsonValue();
  }
  out["sdl_keys"] = JsonValue(s.sdl_keys);
  if (s.keys_tested) {
    JsonValue keys = JsonValue::object();
    keys["viewport_focus"] = JsonValue(s.viewport_focus);
    keys["posted_to_viewport"] = JsonValue(s.posted_to_viewport);
    keys["undos_from_viewport"] = JsonValue(s.undos_from_viewport);
    keys["page_focus"] = JsonValue(s.page_focus);
    keys["posted_to_page"] = JsonValue(s.posted_to_page);
    keys["sdl_keys_from_page"] = JsonValue(s.sdl_keys_from_page);
    keys["undos_after_page"] = JsonValue(s.undos_after_page);
    keys["undo_position_before_page"] = JsonValue(s.undo_position_before_page);
    keys["undo_position_after_page"] = JsonValue(s.undo_position_after_page);
    out["keys"] = std::move(keys);
  } else {
    out["keys"] = JsonValue();
  }
  JsonValue reports = JsonValue::array();
  for (const editor::EditorState::Report& r : s.reports) {
    JsonValue one = JsonValue::object();
    one["name"] = JsonValue(r.name);
    one["ok"] = JsonValue(r.ok);
    one["detail"] = JsonValue(r.detail);
    reports.push_back(std::move(one));
  }
  out["page"] = std::move(reports);
  out["undos"] = JsonValue(s.input.undos);
  out["redos"] = JsonValue(s.input.redos);
  out["deletes"] = JsonValue(s.input.deletes);
  JsonWriteOptions compact;
  compact.pretty = false;  // one line, the last the run prints: what a test reads
  std::printf("%s\n", write_json(out, compact).c_str());
}

// The scripted gesture's events for frame `frame`, in window coordinates, as a pointer would send
// them: after the pick (frame 2) the pointer goes to the first point (frame 3, so the handle under
// it is hovered first, as a person's would be), presses there (4), moves to the second over three
// frames (5 to 7) and releases there (8).
void scripted_events(const Options& o, u64 frame, Vector<window::Event>& out) {
  out.clear();
  if (!o.drag || frame < 3 || frame > 8) return;
  window::Event e;
  e.button = 1;
  const f64 k = frame <= 4 ? 0.0 : (frame >= 7 ? 1.0 : static_cast<f64>(frame - 4) / 3.0);
  e.x = static_cast<f32>(o.drag_from[0] + (o.drag_to[0] - o.drag_from[0]) * k);
  e.y = static_cast<f32>(o.drag_from[1] + (o.drag_to[1] - o.drag_from[1]) * k);
  e.kind = frame == 4   ? window::EventKind::MouseButtonDown
           : frame == 8 ? window::EventKind::MouseButtonUp
                        : window::EventKind::MouseMove;
  out.push_back(e);
}

bool is_pointer(const window::Event& e) noexcept {
  return e.kind == window::EventKind::MouseMove || e.kind == window::EventKind::MouseButtonDown ||
         e.kind == window::EventKind::MouseButtonUp || e.kind == window::EventKind::MouseWheel;
}

// The window's pointer coordinates as the picture's pixels (viewport_math.h, `pointer_scale`).
void to_pixels(window::Event& e, editor::PointerScale scale) noexcept {
  if (!is_pointer(e)) return;
  e.x *= scale.x;
  e.y *= scale.y;
}

// What both modes do between frames: the scripted pick and move, a drag's preview, then the
// viewport brought up to the document. `width` x `height` is the picture.
bool between_frames(const Options& options, u64 frame, editor::PointerScale scale, u32 width,
                    u32 height, editor::EditorDocument& document, editor::Viewport& viewport,
                    editor::EditorUi& ui, editor::EditorInput& input, Summary& summary,
                    std::string& error) {
  // Once: a window's loop can pass a frame's number twice (a swapchain out of date), and --move
  // must not commit twice.
  if (options.pick && frame == 2 && !summary.picked) {
    const f32 px = options.pick_center ? static_cast<f32>(width) * 0.5f
                                       : static_cast<f32>(options.pick_x) * scale.x;
    const f32 py = options.pick_center ? static_cast<f32>(height) * 0.5f
                                       : static_cast<f32>(options.pick_y) * scale.y;
    if (!input.pick(px, py, document, viewport, error)) return false;
    const Id128 picked = document.selection();
    summary.picked = true;
    if (options.move && !picked.is_null()) {
      JsonValue to = JsonValue::array();
      for (const f64 v : options.move_to)
        to.push_back(JsonValue(v));
      const doc::Command set = doc::cmd_set(picked, "translation", std::move(to));
      if (!document.apply(std::span<const doc::Command>(&set, 1), "moved by --move", error))
        return false;
      summary.moved = true;
    }
  }
  if (!input.flush(viewport, error)) return false;
  bool rebuilt = false;
  if (!viewport.sync(document.document(), rebuilt, error)) return false;
  if (rebuilt) {
    ui.forget_textures();
    ++summary.rebuilds;
  }
  return true;
}

void finish(Summary& summary, editor::EditorDocument& document, const editor::Viewport& viewport,
            const editor::EditorInput& input) {
  summary.selected = document.selection();
  summary.transactions = static_cast<u32>(document.document().journal().size());
  summary.undo_position = document.session().undo_position();
  summary.input = input.stats();
  summary.records = 0;
  for (const Id128 id : viewport.scene().records)
    summary.records += id.is_null() ? 0u : 1u;
  summary.instances = static_cast<u32>(viewport.scene().desc.instances.size());
}

int run_offscreen(const Options& options, editor::EditorDocument& document) {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) return unavailable("no Vulkan device", error);
  int code = 0;
  {
    editor::Viewport viewport;
    editor::Viewport::Options vo;
    vo.width = options.width;
    vo.height = options.height;
    vo.ddc = options.ddc;
    if (!viewport.build(device, document.document(), document.directory(), vo, error)) {
      std::fprintf(stderr, "engine-editor: %s\n", error.c_str());
      return 1;
    }
    editor::EditorUi ui;
    ui.create();
    Camera camera;
    if (!camera.start(viewport.framing_camera(), viewport.data().radius, error)) {
      std::fprintf(stderr, "engine-editor: %s\n", error.c_str());
      return 1;
    }
    editor::EditorInput input;
    Summary summary;
    Vector<window::Event> scripted;
    // No window: the scripted coordinates are the picture's own pixels.
    const editor::PointerScale scale;
    const u32 frames = options.frames > 0 ? options.frames : 3u;
    for (u64 f = 0; f < frames && code == 0; ++f) {
      scripted_events(options, f, scripted);
      for (window::Event e : scripted) {
        to_pixels(e, scale);
        ui.feed(e);
        if (!input.handle(e, ui.wants_mouse() && !input.dragging(), !ui.wants_keyboard(), document,
                          viewport, error)) {
          code = 1;
          break;
        }
      }
      if (code != 0) break;
      if (!between_frames(options, f, scale, options.width, options.height, document, viewport, ui,
                          input, summary, error)) {
        code = 1;
        break;
      }
      const renderer::Camera view = camera.view();
      input.set_view(document, viewport, view, options.width, options.height, scale.y);
      ui.frame(1.0f / 60.0f, options.width, options.height, document, viewport, view, input);
      renderer::OverlayDrawData overlay;
      if (!ui.render(viewport.renderer(), overlay, error)) {
        code = 1;
        break;
      }
      renderer::FrameDesc frame;
      frame.camera = view;
      frame.frame_index = f;
      frame.overlay = &overlay;
      const bool last = f + 1 == frames;
      if (last && !options.capture.empty()) {
        renderer::CaptureChannels channels;
        renderer::CapturedFrame shot;
        if (!viewport.renderer().capture(frame, channels, shot, &error)) {
          code = 1;
          break;
        }
        if (image::write_png(options.capture, shot.width, shot.height, 4,
                             std::span<const u8>(shot.color.data(), shot.color.size())) !=
            io::Status::Ok) {
          error = "cannot write " + options.capture;
          code = 1;
          break;
        }
      } else if (!viewport.renderer().render_offscreen(frame, &error)) {
        code = 1;
        break;
      }
      ++summary.frames;
    }
    if (code == 0) {
      finish(summary, document, viewport, input);
      summary.width = options.width;
      summary.height = options.height;
      print_summary(summary, options);
    } else {
      std::fprintf(stderr, "engine-editor: %s\n", error.c_str());
    }
  }
  device.destroy();
  return code;
}

#if defined(ENGINE_EDITOR_WEBVIEW)
// The web view's profile for a person: beside the document, in `.editor/`, a directory that
// ignores itself (its own `.gitignore` is "*"), so a document kept in git never picks up a
// browser cache whatever the repository's own rules say. Inside a test's scratch directory when the
// document is, which is every test's.
std::string webview_profile(const Options& options, const editor::EditorDocument& document) {
  if (!options.webview_profile.empty()) return options.webview_profile;
  const std::string dir = document.directory() + "/.editor";
  if (io::make_directories(dir) == io::Status::Ok) {
    std::string existing;
    if (io::read_file(dir + "/.gitignore", existing) != io::Status::Ok)
      (void)io::write_file(dir + "/.gitignore",
                           "# engine-editor's own state: never committed\n*\n");
  }
  return dir + "/webview";
}
#endif

// The protocol in process (editor_methods.h): a dispatcher over the editor's own session, with the
// built-in methods every host has and the editor's three, and the listener over it.
struct Host {
  editor::EditorState state;
  protocol::Policy policy;
  protocol::Dispatcher dispatcher;
  std::mutex lock;
  protocol::Listener listener;

  explicit Host(editor::EditorDocument& document)
      : dispatcher(protocol::Context{&document.sessions(), nullptr, nullptr, &state, &policy}),
        listener(dispatcher, lock) {
    state.document = &document;
    // The shell's edits are the editor's: the same actor, the director, as the viewport's.
    policy.identity = document.caller().who;
    protocol::add_builtin_methods(dispatcher);
    editor::add_editor_methods(dispatcher);
    // A click in the viewport reaches the shell's outliner as `event.selection`.
    document.set_events(&listener.events());
  }
  ~Host() {
    listener.stop();
    if (state.document != nullptr) state.document->set_events(nullptr);
  }
  ENGINE_NON_COPYABLE(Host);
};

int run_window(const Options& options, editor::EditorDocument& document) {
  std::string error;
  if (!window::init(&error)) return unavailable("no display", error);
  const auto extensions = window::vulkan::instance_extensions();
  if (extensions.empty()) {
    window::shutdown();
    return unavailable("SDL has no Vulkan support here", "");
  }
  // The shell beside the viewport, where there is one to show. Asked before anything appears, so
  // `--embedded` on a machine without the runtime exits with nothing on screen.
  bool embedded = false;
  std::string no_webview;
  if (options.shell != Shell::None) {
#if defined(ENGINE_EDITOR_WEBVIEW)
    embedded = webview::runtime_available(&no_webview);
#else
    no_webview = "this build has no web view (ENGINE_WITH_WEBVIEW is off)";
#endif
    if (!embedded) {
      if (options.shell == Shell::Embedded) {
        window::shutdown();
        return unavailable("no web view for the embedded shell", no_webview);
      }
      std::fprintf(stderr,
                   "engine-editor: %s; the viewport fills the window and the shell is the "
                   "\"shell\" URL above, in a browser\n",
                   no_webview.c_str());
    }
  }

  Host host(document);
  if (options.test_page) editor::add_editor_test_methods(host.dispatcher);
  const bool listening = options.shell != Shell::None || !options.listen.empty();
  if (listening) {
    std::string shell_dir = options.shell_dir;
    if (shell_dir.empty()) shell_dir = platform::executable_directory() + "/../shell";
    if (!host.listener.start(options.listen_port, shell_dir, document.session().id(), error)) {
      window::shutdown();
      std::fprintf(stderr, "engine-editor: --listen: %s\n", error.c_str());
      return 1;
    }
    // The first line on stdout, before anything is served, so a client can be started from it.
    const std::string line = host.listener.announcement();
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
  }

  // The top-level window. Embedded, it carries no surface: its children do the drawing — the web
  // view, and the viewport's child window over the page's cell (window.md, "Child windows").
  window::WindowDesc wd;
  wd.title = "engine-editor";
  wd.width = options.width;
  wd.height = options.height;
  wd.vulkan = !embedded;
  // Embedded, it appears only once the device is made: the device takes the GPU lock, and a window
  // on screen while its process waits for the lock pumps nothing and reads "Not responding".
  wd.hidden = embedded;
  window::Window window;
  if (!window.create(wd, &error)) {
    host.listener.stop();
    window::shutdown();
    return unavailable("cannot create a window", error);
  }
  gfx::DeviceOptions device_options;
  device_options.instance_extensions = extensions.data();
  device_options.instance_extension_count = static_cast<u32>(extensions.size());
  gfx::Device device;
  if (!device.create(device_options, &error) || !device.features().presentation) {
    if (device.valid()) device.destroy();
    host.listener.stop();
    window.destroy();
    window::shutdown();
    return unavailable("no Vulkan device that presents", error);
  }
  int code = 0;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  {
#if defined(ENGINE_EDITOR_WEBVIEW)
    webview::Webview web;
#endif
    // The window that presents: the child over the page's cell, or the top-level window itself.
    window::Window child;
    window::Window* view_window = &window;
    // The viewport is placed (and frames are counted) once the page has said where its cell is.
    bool placed = !embedded;
    u32 applied_layouts = 0;
    if (embedded) {
#if defined(ENGINE_EDITOR_WEBVIEW)
      window.show();
      u32 logical_w = 0;
      u32 logical_h = 0;
      if (!window.logical_size(logical_w, logical_h)) logical_w = logical_h = 0;
      webview::WebviewOptions wo;
      wo.profile_dir = webview_profile(options, document);
      if (!web.create(window.os_handle(), wo, &error)) {
        code = options.shell == Shell::Embedded ? unavailable("no web view", error) : 1;
        if (code == 1) std::fprintf(stderr, "engine-editor: %s\n", error.c_str());
      } else {
        web.set_rect(0, 0, logical_w, logical_h);
        // `test=1` only with --test-page: the page's test hook exists only then (engine_shell.md).
        if (!web.navigate(host.listener.shell_url() + "&embedded=1" +
                          (options.test_page ? "&test=1" : ""))) {
          error = "the web view refused the shell's address";
          code = 1;
        }
      }
#endif
      if (code == 0) {
        // Hidden until the page places it, so it never covers the page while the page loads.
        window::WindowDesc cd;
        cd.parent = &window;
        cd.width = 64;
        cd.height = 64;
        cd.hidden = true;
        cd.vulkan = true;
        if (!child.create(cd, &error)) {
          code = unavailable("cannot create the viewport's child window", error);
        } else {
          view_window = &child;
        }
      }
    }
    gfx::Swapchain swapchain;
    editor::Viewport viewport;
    editor::EditorUi ui;
    editor::EditorInput input;
    Camera camera;
    Summary summary;
    summary.embedded = embedded;
    summary.listen = listening ? host.listener.port() : u16{0};
    InputTrace trace;
    if (!trace.open(options.trace_input)) {
      std::fprintf(stderr, "engine-editor: --trace-input: cannot write %s\n",
                   options.trace_input.c_str());
      code = 1;
    }
    Vector<window::Event> scripted;
    u64 scripted_frame = ~u64{0};  // the frame whose scripted events were sent
    // Frames drawn since the page last moved the viewport: what the scripted pick and gesture and
    // `--frames` count with the shell, so a pick reads a frame drawn at the size it is aimed at (a
    // layout that lands between two frames resizes the picture, and the frame before it is gone).
    // Without the shell nothing resets it, and it is the run's frame count.
    u64 settled = 0;
    u64 acted_frame = ~u64{0};  // the frame whose test actions ran
    bool replace = false;       // place the child again from the (possibly rescaled) layout
    f32 scale_now = window.display_scale();
    summary.display_scale = scale_now;
    bool resized = false;
    bool dropped = false;
    bool relayout_sent = false;
    u32 keys_before_page = 0;
    i64 checks_since_ns = -1;  // --test-page: when the frames were done and the wait began
    if (code == 0) {
      if (!window::vulkan::create_surface(*view_window, device.handles().instance, surface,
                                          &error)) {
        code = unavailable("cannot create a surface", error);
      } else {
        gfx::SwapchainDesc sd;
        sd.surface = surface;
        sd.width = view_window->pixel_width();
        sd.height = view_window->pixel_height();
        sd.vsync = true;
        sd.color_bits = 8;  // SDR: the overlay composites onto the encoded picture (ADR-0054)
        if (!swapchain.create(device, sd, &error)) code = unavailable("no swapchain", error);
      }
    }
    if (code == 0) {
      // The listener may already be serving the page: the document is read under its lock.
      std::lock_guard<std::mutex> lock(host.lock);
      editor::Viewport::Options vo;
      vo.width = swapchain.extent().width;
      vo.height = swapchain.extent().height;
      vo.color_format = swapchain.format();
      vo.offscreen = false;
      vo.ddc = options.ddc;
      if (!viewport.build(device, document.document(), document.directory(), vo, error) ||
          !camera.start(viewport.framing_camera(), viewport.data().radius, error)) {
        std::fprintf(stderr, "engine-editor: %s\n", error.c_str());
        code = 1;
      }
      ui.create();
      if (!embedded) window.show();
    }
    const i64 period_ns = 1'000'000'000 / std::max<i64>(1, present_max_hz.get());
    const i64 started_ns = time::monotonic_ns();
    i64 last_ns = started_ns;
    bool resize = false;
    bool relative_asked = false;  // what the viewport's window was last asked for
    bool running = code == 0;
    while (running) {
      // The ceiling: a window on the owner's desktop never draws faster than this.
      while (time::monotonic_ns() < last_ns + period_ns)
        std::this_thread::yield();
      const i64 now = time::monotonic_ns();
      const i64 dt_ns = now - last_ns;
      last_ns = now;
      // Everything that reads or writes the document, the selection or the editor's state happens
      // under the listener's dispatch lock; the frame's submission and present do not.
      std::unique_lock<std::mutex> lock(host.lock);
      // The pointer arrives in the window's coordinates and the picture is the swapchain's pixels;
      // under display scaling the two differ, and a click would land beside its target.
      u32 logical_width = 0;
      u32 logical_height = 0;
      if (!view_window->logical_size(logical_width, logical_height))
        logical_width = logical_height = 0;
      const editor::PointerScale scale = editor::pointer_scale(
          logical_width, logical_height, swapchain.extent().width, swapchain.extent().height);
      // One path for the platform's events and the scripted gesture's: ImGui first, then the
      // editor's pointer and keys, then the camera, which gets what neither took.
      const auto on_event = [&](window::Event e) {
        to_pixels(e, scale);
        ui.feed(e);
        const bool over_overlay = ui.wants_mouse() && !input.dragging();
        if (e.kind == window::EventKind::MouseButtonDown && e.button == 3 && !over_overlay)
          camera.looking = true;
        if (e.kind == window::EventKind::MouseButtonUp && e.button == 3) camera.looking = false;
        const bool keys_free = !ui.wants_keyboard();
        if (!input.handle(e, over_overlay, keys_free, document, viewport, error)) return false;
        const bool is_key =
            e.kind == window::EventKind::KeyDown || e.kind == window::EventKind::KeyUp;
        // Ctrl is the editor's modifier (Ctrl+Z, Ctrl+Y), so here it does not lower the camera as
        // it does in engine-view; Q does.
        const bool editor_key = is_key && e.key == window::Key::Control;
        if (!editor_key && ((is_key && keys_free) || (!is_key && !over_overlay) ||
                            e.kind == window::EventKind::MouseButtonUp)) {
          camera.edge.add(e, camera.next(), camera.looking);
        }
        return true;
      };
      bool failed = false;
      window::Event event;
      while (!failed && window.poll(event)) {
        trace.event(event, summary.frames, *view_window);
        if (event.kind == window::EventKind::Quit ||
            event.kind == window::EventKind::CloseRequested) {
          if (!event.child) running = false;
          continue;
        }
        if (event.kind == window::EventKind::KeyDown) ++summary.sdl_keys;
        if (event.kind == window::EventKind::ScaleChanged) {
          // The display's scale under the window changed (dragged to a monitor of another scale,
          // or the setting changed): the child goes where the last layout would put it at the new
          // ratio at once, and the page is asked to lay itself out again (`editor.relayout`) and
          // send what it really has; picking follows the child's new size on the next frame,
          // since the pointer's scale is read from the child each frame.
          ++summary.scale_changes;
          const f32 to = event.value > 0.0f ? event.value : window.display_scale();
          if (embedded && host.state.layouts > 0 && to != scale_now) {
            host.state.layout = editor::rescaled_layout(
                host.state.layout, static_cast<f64>(scale_now), static_cast<f64>(to));
            replace = true;
            summary.relayout_reached =
                host.listener.notify_all(editor::relayout_notification("display scale"));
            ++summary.relayouts;
          }
          scale_now = to;
          summary.display_scale = to;
          continue;
        }
        if (embedded && !event.child) {
          // The top-level window's own: its size is the web view's. Its keys are the viewport's —
          // a key the page has never reaches the platform's event queue, and one that reaches it
          // came to the viewport's child, which the platform reports as its top-level window's.
          if (event.kind == window::EventKind::Resized) {
#if defined(ENGINE_EDITOR_WEBVIEW)
            u32 w = 0;
            u32 h = 0;
            if (window.logical_size(w, h)) web.set_rect(0, 0, w, h);
#endif
            // The child is clamped into the new client area at once; the page's own layout for
            // the new size follows.
            replace = placed;
            continue;
          }
          if (is_pointer(event)) continue;
        }
        if (event.kind == window::EventKind::Resized) {
          resize = true;
          continue;
        }
        // A press in the viewport gives it the keyboard: a child window is not activated by a
        // click the way a top-level window is, and a click in the page takes the keyboard back.
        if (embedded && event.child && event.kind == window::EventKind::MouseButtonDown)
          child.focus();
        failed = !on_event(event);
      }
      // The page's layout, when it moved: the viewport's child window goes where the cell is, and
      // is shown the first time.
      if (embedded && (host.state.layouts != applied_layouts || replace)) {
        const bool from_page = host.state.layouts != applied_layouts;
        applied_layouts = host.state.layouts;
        replace = false;
        u32 lw = 0;
        u32 lh = 0;
        const u32 pw = window.pixel_width();
        if (!window.logical_size(lw, lh)) lw = lh = 0;
        host.state.pixels_per_unit =
            lw > 0 && pw > 0 ? static_cast<f64>(pw) / static_cast<f64>(lw) : 1.0;
        // Never past the client area: a page wider than its window would put the cell's far edge
        // under the frame (the narrow layout is the page's answer; this is the editor's).
        const editor::Placement placement =
            editor::place_viewport(host.state.layout, host.state.pixels_per_unit, lw, lh);
        const editor::ViewportRect rect = placement.rect;
        if (placement.clamped) {
          const editor::ViewportRect asked =
              editor::layout_rect(host.state.layout, host.state.pixels_per_unit);
          ENGINE_LOG_WARN(log_editor, "viewport clamped to the window's client area",
                          log::field("asked_x", asked.x), log::field("asked_y", asked.y),
                          log::field("asked_width", asked.width),
                          log::field("asked_height", asked.height), log::field("client_width", lw),
                          log::field("client_height", lh),
                          log::field("narrow", host.state.layout.narrow));
        }
        const bool moved = !placed || !(rect == summary.viewport);
        summary.viewport = rect;
        summary.layouts = host.state.layouts;
        if (moved && summary.placements.size() < 16) {
          summary.placements.push_back(Summary::Placed{lw, lh, rect, placement.clamped,
                                                       host.state.layout.narrow, from_page});
        }
        if (!moved) {
          // The same rectangle again (a reconnect, an answer to editor.relayout): nothing moves.
        } else if (!child.set_rect(rect.x, rect.y, rect.width, rect.height)) {
          error = "the viewport's child window could not be placed";
          failed = true;
        } else {
          if (!placed) {
            child.set_visible(true);
            placed = true;
          }
          resize = true;
          settled = 0;
          scripted_frame = ~u64{0};
          acted_frame = ~u64{0};
        }
      }
      if (!placed && time::monotonic_ns() - started_ns > k_layout_timeout_ns) {
        error = "the page sent no editor.layout within a minute of starting";
        failed = true;
      }
      // The test flags' actions, once each at their frame (counted as --pick's are).
      if (embedded && placed && settled != acted_frame) {
        acted_frame = settled;
        const u64 f = settled;
        u32 cw = 0;
        u32 ch = 0;
        if (!child.logical_size(cw, ch)) cw = ch = 0;
        window::Event e;
        e.x = static_cast<f32>(cw) * 0.5f;
        e.y = static_cast<f32>(ch) * 0.5f;
        if (options.resize && !resized && f == 4) {
          resized = true;
          (void)window.set_size(static_cast<u32>(options.resize_to[0]),
                                static_cast<u32>(options.resize_to[1]));
        }
        // --look: the right button held over the viewport and the pointer moved, through SDL's
        // queue as the child's own events, so relative mode is asked of the adopted child as a
        // person's gesture asks it.
        if (options.look && f >= 3 && f <= 9) {
          if (f == 3) {
            summary.look_yaw[0] = camera.state.yaw;
            summary.look_pitch[0] = camera.state.pitch;
          }
          e.button = 3;
          if (f == 3 || f == 7) {
            e.kind = f == 3 ? window::EventKind::MouseButtonDown : window::EventKind::MouseButtonUp;
            (void)child.push_event(e);
          } else if (f <= 6) {
            e.kind = window::EventKind::MouseMove;
            e.dx = static_cast<f32>(options.look_by[0] / 3.0);
            e.dy = static_cast<f32>(options.look_by[1] / 3.0);
            (void)child.push_event(e);
            if (f == 5) {
              summary.look_relative = child.relative_mouse();
              summary.look_relative_asked = child.relative_mouse_asked();
            }
          } else if (f == 9) {
            summary.look_yaw[1] = camera.state.yaw;
            summary.look_pitch[1] = camera.state.pitch;
            summary.looked = true;
          }
        }
        // --test-look-os: the same gesture through the **platform's** queue — the window
        // procedure, SDL's translation, relative mode asked of the adopted child — posted to the
        // child and never to the desktop's foreground window: a move to the centre, the right
        // button down, three moves of (DX, DY) / 3, the button up. A posted move is absolute
        // motion, which is what SDL reads while relative mode is not in effect (the editor's
        // window is not the platform's foreground window, as under a test runner); in effect, SDL
        // reads the device's raw motion, which nothing but a device (or SendInput, which moves the
        // desktop's real cursor) makes, so the run then checks where the cursor was held instead,
        // and ends the gesture itself (window.md, "Relative mode on an adopted child").
        if (options.look_os && f >= 3 && f <= 10) {
          e.button = 3;
          const f32 cx = e.x;
          const f32 cy = e.y;
          if (f == 3) {
            summary.look_os_yaw[0] = camera.state.yaw;
            summary.look_os_expected_deg =
                -options.look_os_by[0] * static_cast<f64>(camera.params.look) * 57.29577951308232;
            e.kind = window::EventKind::MouseMove;
            (void)child.post_os_event(e);
            e.kind = window::EventKind::MouseButtonDown;
            (void)child.post_os_event(e);
          } else if (f <= 6) {
            const f32 k = static_cast<f32>(f - 3) / 3.0f;
            e.kind = window::EventKind::MouseMove;
            e.x = cx + static_cast<f32>(options.look_os_by[0]) * k;
            e.y = cy + static_cast<f32>(options.look_os_by[1]) * k;
            (void)child.post_os_event(e);
            if (f == 5) {
              const window::Window::PointerReport r = child.pointer_report();
              summary.look_os_asked = r.relative_asked;
              summary.look_os_in_effect = child.relative_mouse();
              summary.look_os_child_flag = r.sdl_flag;
              summary.look_os_clip_inside =
                  r.clipped && r.clip_left >= r.os_x && r.clip_top >= r.os_y &&
                  r.clip_right <= r.os_x + r.os_width && r.clip_bottom <= r.os_y + r.os_height;
              summary.look_os_cursor_inside = r.cursor_inside;
            }
          } else if (f == 7) {
            e.kind = window::EventKind::MouseButtonUp;
            e.x = cx + static_cast<f32>(options.look_os_by[0]);
            e.y = cy + static_cast<f32>(options.look_os_by[1]);
            (void)child.post_os_event(e);
          } else if (f == 8 && camera.looking) {
            // In effect, SDL reads the buttons from the device too, and the posted release is not
            // one: the run lets go of the pointer itself rather than hold it to the end.
            camera.looking = false;
            summary.look_os_released_by_test = true;
          } else if (f == 10) {
            summary.look_os_yaw[1] = camera.state.yaw;
            summary.looked_os = true;
          }
        }
        // --test-keys: a click in the viewport through the platform's queue gives it the
        // keyboard, and Ctrl+Z posted to whoever has it undoes --move's transaction; then the page
        // is given the keyboard and Ctrl+Z goes to it, which SDL must never see.
        const auto ctrl_z = [](window::Event k, bool to_focus, window::Window& w) {
          bool ok = true;
          const window::Key keys[4] = {window::Key::Control, window::Key::Z, window::Key::Z,
                                       window::Key::Control};
          for (u32 i = 0; i < 4; ++i) {
            k.kind = i < 2 ? window::EventKind::KeyDown : window::EventKind::KeyUp;
            k.key = keys[i];
            ok = (to_focus ? window::Window::post_to_focus(k) : w.post_os_event(k)) && ok;
          }
          return ok;
        };
        if (options.test_keys) {
          if (f == 10) {
            e.button = 1;
            e.kind = window::EventKind::MouseButtonDown;
            (void)child.post_os_event(e);
            e.kind = window::EventKind::MouseButtonUp;
            (void)child.post_os_event(e);
          } else if (f == 14) {
            summary.viewport_focus = child.has_os_focus();
            summary.posted_to_viewport = ctrl_z(e, true, child);
          } else if (f == 18) {
            summary.undos_from_viewport = input.stats().undos;
            summary.undo_position_before_page = document.session().undo_position();
#if defined(ENGINE_EDITOR_WEBVIEW)
            (void)web.focus();
#endif
          } else if (f == 22) {
            summary.page_focus = !child.has_os_focus() && !window.has_os_focus();
            keys_before_page = summary.sdl_keys;
            summary.posted_to_page = ctrl_z(e, true, child);
          } else if (f == 30) {
            summary.sdl_keys_from_page = summary.sdl_keys - keys_before_page;
            summary.undos_after_page = input.stats().undos;
            summary.undo_position_after_page = document.session().undo_position();
            summary.keys_tested = true;
          }
        }
        // The relayout and the drop wait for the page's own checks, so a report is never sent
        // into a socket that is down, and the drop waits for the relayout's answer.
        bool page_done = false;
        for (const editor::EditorState::Report& report : host.state.reports)
          page_done = page_done || report.name == "done";
        if (options.relayout_at > 0 && !relayout_sent && f >= options.relayout_at && page_done) {
          relayout_sent = true;
          summary.layouts_at_relayout = host.state.layouts;
          summary.relayout_reached = host.listener.notify_all(
              editor::relayout_notification("asked by --test-relayout-at"));
          ++summary.relayouts;
        }
        const bool relayout_answered =
            options.relayout_at == 0 ||
            (relayout_sent && host.state.layouts > summary.layouts_at_relayout);
        if (options.drop_at > 0 && !dropped && f >= options.drop_at && page_done &&
            relayout_answered) {
          dropped = true;
          summary.drops = host.listener.drop_connections();
          summary.frames_at_drop = static_cast<i64>(summary.frames);
        }
      }
      if (summary.frames_at_reconnect < 0 && dropped) {
        for (const editor::EditorState::Report& r : host.state.reports) {
          if (r.name == "reconnect") summary.frames_at_reconnect = static_cast<i64>(summary.frames);
        }
      }
      // Once a frame: an iteration that draws nothing (an out-of-date swapchain) keeps the count.
      scripted.clear();
      if (placed && settled != scripted_frame) {
        scripted_frame = settled;
        scripted_events(options, settled, scripted);
      }
      for (const window::Event& e : scripted)
        failed = failed || !on_event(e);
      if (failed) {
        code = 1;
        break;
      }
      if (!running) break;
      if (!placed) continue;  // nothing to draw until the page says where
      // Asked on a change only: the window holds the mode until told otherwise.
      if (camera.looking != relative_asked) {
        relative_asked = camera.looking;
        (void)view_window->set_relative_mouse(camera.looking);
      }
      camera.advance(dt_ns);
      trace.frame(summary.frames, camera.state.yaw, camera.state.pitch, camera.looking);
      if (resize) {
        resize = false;
        if (!swapchain.resize(view_window->pixel_width(), view_window->pixel_height(), &error)) {
          code = 1;
          break;
        }
        if (swapchain.extent().width == 0 || swapchain.extent().height == 0) continue;
        if (!viewport.renderer().resize(swapchain.extent().width, swapchain.extent().height,
                                        &error)) {
          code = 1;
          break;
        }
      }
      const u32 width = swapchain.extent().width;
      const u32 height = swapchain.extent().height;
      if (!between_frames(options, settled, scale, width, height, document, viewport, ui, input,
                          summary, error)) {
        code = 1;
        break;
      }
      const renderer::Camera view = camera.view();
      host.state.camera = editor::framed_camera(view);
      host.state.width = width;
      host.state.height = height;
      input.set_view(document, viewport, view, width, height, scale.y);
      ui.frame(static_cast<f32>(dt_ns) / 1.0e9f, width, height, document, viewport, view, input);
      renderer::OverlayDrawData overlay;
      if (!ui.render(viewport.renderer(), overlay, error)) {
        code = 1;
        break;
      }
      lock.unlock();
      renderer::SceneRenderer& r = viewport.renderer();
      r.begin_frame();
      u32 image = 0;
      const gfx::PresentStatus acquired = swapchain.acquire(r.acquire_semaphore(), image);
      if (acquired != gfx::PresentStatus::Ok) {
        r.abort_frame();
        if (acquired == gfx::PresentStatus::Error) {
          error = "the swapchain image could not be acquired";
          code = 1;
          break;
        }
        resize = true;
        continue;
      }
      renderer::FrameDesc frame;
      frame.camera = view;
      frame.frame_index = summary.frames;
      frame.color = swapchain.image(image);
      frame.final_layout = gfx::ImageLayout::Present;
      frame.wait = r.acquire_semaphore();
      frame.signal = swapchain.render_finished(image);
      frame.overlay = &overlay;
      if (r.submit_frame(frame, &error) == 0) {
        code = 1;
        break;
      }
      const gfx::PresentStatus presented =
          swapchain.present(image, swapchain.render_finished(image));
      if (presented == gfx::PresentStatus::Error) {
        error = "present failed";
        code = 1;
        break;
      }
      if (presented == gfx::PresentStatus::OutOfDate) resize = true;
      ++summary.frames;
      ++settled;
      if (options.frames > 0 && settled >= options.frames) {
        bool done = !options.test_page;
        if (!done) {
          bool checks = false;
          for (const editor::EditorState::Report& report : host.state.reports)
            checks = checks || report.name == "done";
          done = checks && (options.drop_at == 0 || summary.frames_at_reconnect >= 0);
          const i64 t = time::monotonic_ns();
          if (checks_since_ns < 0) checks_since_ns = t;
          if (!done && t - checks_since_ns > k_page_checks_timeout_ns) {
            error = "--test-page: the page's checks did not finish within two minutes";
            code = 1;
            break;
          }
        }
        if (done) running = false;
      }
    }
    // Nothing the listener dispatches reaches into the window, the viewport or the device, but it
    // stops before they go so that the page's last call is answered by a whole editor.
    host.listener.stop();
    if (viewport.valid()) viewport.renderer().wait_idle();
    if (code == 0) {
      finish(summary, document, viewport, input);
      summary.width = swapchain.extent().width;
      summary.height = swapchain.extent().height;
      summary.layouts = host.state.layouts;
      summary.reports = host.state.reports;
      print_summary(summary, options);
    } else if (!error.empty()) {
      std::fprintf(stderr, "engine-editor: %s\n", error.c_str());
    }
    ui.destroy();
    viewport.destroy();
    swapchain.destroy();
    if (surface != VK_NULL_HANDLE)
      window::vulkan::destroy_surface(device.handles().instance, surface);
    surface = VK_NULL_HANDLE;
#if defined(ENGINE_EDITOR_WEBVIEW)
    web.destroy();
#endif
    child.destroy();
  }
  device.destroy();
  window.destroy();
  window::shutdown();
  return code;
}

}  // namespace

int main(int argc, char** argv) {
  engine::platform::require_cpu_baseline();  // ADR-0031, first statement
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    const auto value = [&](std::string& out) {
      if (i + 1 >= argc) return false;
      out = argv[++i];
      return true;
    };
    std::string v;
    if (a == "--help" || a == "-h") {
      std::fputs(k_usage, stdout);
      return 0;
    } else if (a == "--version") {
      // The build stamp (cmake/EngineBuildStamp.cmake), as every executable prints it: what an
      // install check reads. No document, no window, no device.
      std::printf("{\"tool\":\"engine-editor\",\"commit\":\"%s\",\"dirty\":%s}\n",
                  build_stamp::commit(), build_stamp::dirty() ? "true" : "false");
      return 0;
    } else if (a == "--doc") {
      if (!value(options.doc)) return usage("--doc needs a directory");
    } else if (a == "--create") {
      options.create = true;
    } else if (a == "--actor") {
      if (!value(options.actor)) return usage("--actor needs a name");
    } else if (a == "--width" || a == "--height") {
      u32 n = 0;
      if (!value(v) || !parse_u32(v, n) || n < 64 || n > 16384)
        return usage("--width and --height take 64 to 16384");
      (a == "--width" ? options.width : options.height) = n;
    } else if (a == "--offscreen") {
      options.offscreen = true;
    } else if (a == "--embedded") {
      options.shell = Shell::Embedded;
    } else if (a == "--no-shell") {
      options.shell = Shell::None;
    } else if (a == "--listen") {
      if (!value(options.listen)) return usage("--listen needs 127.0.0.1:<port>");
      std::string why;
      // Loopback or nothing (AGENTS.md: a listener binds loopback unless the user opts in, and
      // there is no opt-in here): any other address is refused before anything starts.
      if (!net::parse_loopback_address(options.listen, options.listen_port, why)) {
        std::fprintf(stderr, "engine-editor: --listen %s: %s\n", options.listen.c_str(),
                     why.c_str());
        return 2;
      }
    } else if (a == "--shell") {
      if (!value(options.shell_dir)) return usage("--shell needs a directory");
    } else if (a == "--frames") {
      if (!value(v) || !parse_u32(v, options.frames)) return usage("--frames needs a count");
    } else if (a == "--pick") {
      if (!value(v)) return usage("--pick needs X,Y or center");
      if (v == "center") {
        options.pick_center = true;
      } else {
        f64 xy[2];
        if (!parse_list(v, xy, 2) || xy[0] < 0.0 || xy[1] < 0.0)
          return usage("--pick needs X,Y or center");
        options.pick_x = static_cast<u32>(xy[0]);
        options.pick_y = static_cast<u32>(xy[1]);
      }
      options.pick = true;
    } else if (a == "--move") {
      if (!value(v) || !parse_list(v, options.move_to, 3)) return usage("--move needs X,Y,Z");
      options.move = true;
    } else if (a == "--drag") {
      std::string to;
      if (!value(v) || !value(to) || !parse_list(v, options.drag_from, 2) ||
          !parse_list(to, options.drag_to, 2))
        return usage("--drag needs X0,Y0 X1,Y1");
      options.drag = true;
    } else if (a == "--look") {
      if (!value(v) || !parse_list(v, options.look_by, 2)) return usage("--look needs DX,DY");
      options.look = true;
    } else if (a == "--test-look-os") {
      if (!value(v) || !parse_list(v, options.look_os_by, 2) ||
          std::fabs(options.look_os_by[0]) > 300.0 || std::fabs(options.look_os_by[1]) > 300.0)
        return usage("--test-look-os needs DX,DY, each within 300");
      options.look_os = true;
    } else if (a == "--trace-input") {
      if (!value(options.trace_input)) return usage("--trace-input needs a file");
    } else if (a == "--webview-profile") {
      if (!value(options.webview_profile)) return usage("--webview-profile needs a directory");
    } else if (a == "--resize-to") {
      if (!value(v) || !parse_list(v, options.resize_to, 2) || options.resize_to[0] < 64.0 ||
          options.resize_to[1] < 64.0 || options.resize_to[0] > 16384.0 ||
          options.resize_to[1] > 16384.0)
        return usage("--resize-to needs W,H, each 64 to 16384");
      options.resize = true;
    } else if (a == "--test-keys") {
      options.test_keys = true;
    } else if (a == "--test-page") {
      options.test_page = true;
    } else if (a == "--test-drop-socket-at" || a == "--test-relayout-at") {
      u32 n = 0;
      if (!value(v) || !parse_u32(v, n) || n < 3) return usage("a frame number of 3 or more");
      (a == "--test-drop-socket-at" ? options.drop_at : options.relayout_at) = n;
    } else if (a == "--capture") {
      if (!value(options.capture)) return usage("--capture needs a path");
    } else if (a == "--ddc") {
      if (!value(options.ddc)) return usage("--ddc needs a directory");
    } else {
      return usage(("unknown argument " + std::string(a)).c_str());
    }
  }
  if (options.doc.empty()) return usage("--doc is required");
  if (options.move && !options.pick) return usage("--move moves what --pick selected");
  if (!options.capture.empty() && !options.offscreen)
    return usage("--capture is offscreen's (a window's frame is the swapchain's)");
  if (options.pick && options.frames > 0 && options.frames < 3)
    return usage("--pick answers after the second frame: --frames 3 or more");
  if (options.drag && !options.pick) return usage("--drag drags what --pick selected");
  if (options.drag && options.frames > 0 && options.frames < 10)
    return usage("--drag releases at the eighth frame: --frames 10 or more");
  if (options.offscreen && (options.shell == Shell::Embedded || !options.listen.empty()))
    return usage("--offscreen draws the viewport alone: no --embedded, no --listen");
  if (options.look && options.drag) return usage("--look and --drag use the same frames");
  if (options.look_os && (options.look || options.drag))
    return usage("--test-look-os, --look and --drag use the same frames");
  if (options.look_os && options.frames > 0 && options.frames < 12)
    return usage("--test-look-os reads the camera at the tenth frame: --frames 12 or more");
  if (options.look && options.frames > 0 && options.frames < 10)
    return usage("--look releases at the seventh frame: --frames 10 or more");
  if (options.test_keys && (!options.move || (options.frames > 0 && options.frames < 32)))
    return usage("--test-keys undoes what --move did: --move, and --frames 32 or more");
  const bool test_window = options.resize || options.test_keys || options.test_page ||
                           options.drop_at > 0 || options.relayout_at > 0;
  if (test_window && (options.offscreen || options.shell != Shell::Embedded))
    return usage("--resize-to, --test-keys and the --test-* flags are the embedded window's");
  if ((options.drop_at > 0 || options.relayout_at > 0) && !options.test_page)
    return usage("--test-drop-socket-at and --test-relayout-at need --test-page to see the page");

  editor::EditorDocument document;
  std::string error;
  if (!document.open(options.doc, options.create, options.actor, error)) {
    std::fprintf(stderr, "engine-editor: %s\n", error.c_str());
    return 1;
  }
  return options.offscreen ? run_offscreen(options, document) : run_window(options, document);
}
