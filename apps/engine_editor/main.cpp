// engine-editor: the engine's window on a document (docs/subsystems/apps.md, "engine-editor";
// ADR-0054; roadmap A34). One window: the renderer's picture of a document's placements,
// engine-view's fly camera, picking from the visibility buffer, a translate gizmo whose every drag
// is one transaction, undo, redo and delete — the in-engine half of the editor, whatever its shell
// becomes — and three Dear ImGui panels (outliner, properties, journal) that are a stopgap for the
// shell (A36's web client over the protocol). A transaction that moves a placement moves it in the
// next frame, whoever made it.
//
// The document is opened through `protocol::Session` and every edit goes through its checked write
// path (editor_document.h); the pointer and keys are editor_input.h's, their maths
// viewport_math.h's; everything drawn is the renderer's (editor_viewport.h). `--offscreen` runs the
// same frames with no window, and `--frames`, `--pick`, `--move`, `--drag` and `--capture` are how
// a test drives either mode and reads the answer from the one JSON line it prints at the end.

#include "../engine_view/fly_camera.h"
#include "../engine_view/window_input.h"
#include "editor_document.h"
#include "editor_input.h"
#include "editor_ui.h"
#include "editor_viewport.h"
#include "viewport_math.h"

#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/platform/cpu_baseline.h>
#include <core/time/time.h>
#include <domain/gfx/backend/vulkan/swapchain.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/device.h>
#include <foundation/image/png.h>
#include <foundation/input/input.h>
#include <foundation/input/input_log.h>
#include <foundation/tunables/tunables.h>
#include <foundation/window/backend/vulkan/surface.h>
#include <foundation/window/window.h>
#include <systems/renderer/capture.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <engine_build_stamp.h>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

using namespace engine;

namespace {

tunables::Int present_max_hz{"editor.present.max_hz", 120, 1, 1000,
                             "The most frames a second the editor's window draws: it shares the "
                             "desktop, so it never runs unthrottled"};

constexpr const char* k_usage =
    "engine-editor --doc <dir> [--create] [--actor <name>] [--width N] [--height N]\n"
    "              [--offscreen] [--frames N] [--pick X,Y] [--move X,Y,Z] [--drag X0,Y0 X1,Y1]\n"
    "              [--capture out.png] [--ddc <dir>]\n"
    "engine-editor --version\n"
    "  --doc        the document directory (protocol::Session opens it)\n"
    "  --create     create the document when the directory holds none\n"
    "  --actor      who the edits are attributed to (role: director)\n"
    "  --offscreen  no window: draw --frames frames into the renderer's own target\n"
    "  --frames N   stop after N frames (0: until the window closes)\n"
    "  --pick X,Y   after the second frame, select what is under the pointer at (X, Y), in\n"
    "               window coordinates (the picture's pixels offscreen or where nothing scales)\n"
    "  --move X,Y,Z after picking, set the selection's translation in one transaction\n"
    "  --drag X0,Y0 X1,Y1  after picking, press at the first point, move to the second over\n"
    "               three frames and release, as a pointer would (frames 3 to 8): on a gizmo\n"
    "               handle that is one transaction; needs --pick and --frames 10 or more\n"
    "  --capture    offscreen: the last frame, panels and all, as a PNG\n"
    "  --version    the commit this binary was built from, as one JSON line\n"
    "In the window: left click picks; drag a gizmo handle to move the selection along it (Escape\n"
    "cancels); Ctrl+Z undo, Ctrl+Y or Ctrl+Shift+Z redo, Delete deletes the selection.\n"
    "Exit codes: 0 ok, 1 runtime error, 2 usage, 3 no display or no device.\n";

struct Options {
  std::string doc;
  bool create = false;
  std::string actor = "editor";
  u32 width = 1280;
  u32 height = 720;
  bool offscreen = false;
  u32 frames = 0;
  bool pick = false;
  u32 pick_x = 0;
  u32 pick_y = 0;
  bool move = false;
  f64 move_to[3] = {0.0, 0.0, 0.0};
  bool drag = false;
  f64 drag_from[2] = {0.0, 0.0};
  f64 drag_to[2] = {0.0, 0.0};
  std::string capture;
  std::string ddc;
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

// engine-view's fly camera, fed only what the panels do not take: WASD and E/Q to move, the pointer
// to look while the right button is held over the picture (apps/engine_view/fly_camera.h).
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

struct Summary {
  u64 frames = 0;
  u32 records = 0;
  u32 instances = 0;
  Id128 selected;
  bool picked = false;
  bool moved = false;
  u32 outliner = 0;
  u32 properties = 0;
  u32 journal = 0;
  u32 rebuilds = 0;
  u32 transactions = 0;  // the session's journal, whole
  u32 undo_position = 0;
  u32 width = 0;  // the picture's pixels: the swapchain's, or the offscreen target's
  u32 height = 0;
  editor::EditorInput::Stats input;
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
  out["undos"] = JsonValue(s.input.undos);
  out["redos"] = JsonValue(s.input.redos);
  out["deletes"] = JsonValue(s.input.deletes);
  JsonValue panels = JsonValue::object();
  panels["outliner"] = JsonValue(s.outliner);
  panels["properties"] = JsonValue(s.properties);
  panels["journal"] = JsonValue(s.journal);
  out["panels"] = std::move(panels);
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
// viewport brought up to the document.
bool between_frames(const Options& options, u64 frame, editor::PointerScale scale,
                    editor::EditorDocument& document, editor::Viewport& viewport,
                    editor::EditorUi& ui, editor::EditorInput& input, Summary& summary,
                    std::string& error) {
  // Once: a window's loop can pass a frame's number twice (a swapchain out of date), and --move
  // must not commit twice.
  if (options.pick && frame == 2 && !summary.picked) {
    if (!input.pick(static_cast<f32>(options.pick_x) * scale.x,
                    static_cast<f32>(options.pick_y) * scale.y, document, viewport, error))
      return false;
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

// What the toolbar asked for this frame, through the same calls as the keys.
bool toolbar(const editor::EditorUi::Actions& actions, editor::EditorDocument& document,
             editor::Viewport& viewport, editor::EditorInput& input, std::string& error) {
  if (actions.select) document.select(actions.selected);
  if (actions.undo && !input.undo(document, viewport, error)) return false;
  if (actions.redo && !input.redo(document, viewport, error)) return false;
  if (actions.remove && !input.remove_selection(document, viewport, error)) return false;
  return true;
}

void finish(Summary& summary, editor::EditorDocument& document, const editor::Viewport& viewport,
            const editor::EditorUi& ui, const editor::EditorInput& input) {
  summary.selected = document.selection();
  summary.transactions = static_cast<u32>(document.document().journal().size());
  summary.undo_position = document.session().undo_position();
  summary.input = input.stats();
  summary.records = 0;
  for (const Id128 id : viewport.scene().records)
    summary.records += id.is_null() ? 0u : 1u;
  summary.instances = static_cast<u32>(viewport.scene().desc.instances.size());
  summary.outliner = ui.outliner_rows();
  summary.properties = ui.property_rows();
  summary.journal = ui.journal_rows();
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
      if (!between_frames(options, f, scale, document, viewport, ui, input, summary, error)) {
        code = 1;
        break;
      }
      const renderer::Camera view = camera.view();
      input.set_view(document, viewport, view, options.width, options.height, scale.y);
      editor::EditorUi::Actions actions;
      ui.frame(1.0f / 60.0f, options.width, options.height, document, viewport, view, input,
               actions);
      if (!toolbar(actions, document, viewport, input, error)) {
        code = 1;
        break;
      }
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
      finish(summary, document, viewport, ui, input);
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

int run_window(const Options& options, editor::EditorDocument& document) {
  std::string error;
  if (!window::init(&error)) return unavailable("no display", error);
  const auto extensions = window::vulkan::instance_extensions();
  if (extensions.empty()) {
    window::shutdown();
    return unavailable("SDL has no Vulkan support here", "");
  }
  window::WindowDesc wd;
  wd.title = "engine-editor";
  wd.width = options.width;
  wd.height = options.height;
  window::Window window;
  if (!window.create(wd, &error)) {
    window::shutdown();
    return unavailable("cannot create a window", error);
  }
  gfx::DeviceOptions device_options;
  device_options.instance_extensions = extensions.data();
  device_options.instance_extension_count = static_cast<u32>(extensions.size());
  gfx::Device device;
  if (!device.create(device_options, &error) || !device.features().presentation) {
    if (device.valid()) device.destroy();
    window.destroy();
    window::shutdown();
    return unavailable("no Vulkan device that presents", error);
  }
  int code = 0;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  {
    gfx::Swapchain swapchain;
    editor::Viewport viewport;
    editor::EditorUi ui;
    editor::EditorInput input;
    Camera camera;
    Summary summary;
    Vector<window::Event> scripted;
    u64 scripted_frame = ~u64{0};  // the frame whose scripted events were sent
    if (!window::vulkan::create_surface(window, device.handles().instance, surface, &error)) {
      code = unavailable("cannot create a surface", error);
    } else {
      gfx::SwapchainDesc sd;
      sd.surface = surface;
      sd.width = window.pixel_width();
      sd.height = window.pixel_height();
      sd.vsync = true;
      sd.color_bits = 8;  // SDR: the overlay composites onto the encoded picture (ADR-0054)
      if (!swapchain.create(device, sd, &error)) code = unavailable("no swapchain", error);
    }
    if (code == 0) {
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
      window.show();
    }
    const i64 period_ns = 1'000'000'000 / std::max<i64>(1, present_max_hz.get());
    i64 last_ns = time::monotonic_ns();
    bool resize = false;
    bool running = code == 0;
    while (running) {
      // The ceiling: a window on the owner's desktop never draws faster than this.
      while (time::monotonic_ns() < last_ns + period_ns)
        std::this_thread::yield();
      const i64 now = time::monotonic_ns();
      const i64 dt_ns = now - last_ns;
      last_ns = now;
      // The pointer arrives in the window's coordinates and the picture is the swapchain's pixels;
      // under display scaling the two differ, and a click would land beside its target.
      u32 logical_width = 0;
      u32 logical_height = 0;
      if (!window.logical_size(logical_width, logical_height)) logical_width = logical_height = 0;
      const editor::PointerScale scale = editor::pointer_scale(
          logical_width, logical_height, swapchain.extent().width, swapchain.extent().height);
      // One path for the platform's events and the scripted gesture's: ImGui first, then the
      // editor's pointer and keys, then the camera, which gets what neither took.
      const auto on_event = [&](window::Event e) {
        to_pixels(e, scale);
        ui.feed(e);
        const bool over_panels = ui.wants_mouse() && !input.dragging();
        if (e.kind == window::EventKind::MouseButtonDown && e.button == 3 && !over_panels)
          camera.looking = true;
        if (e.kind == window::EventKind::MouseButtonUp && e.button == 3) camera.looking = false;
        const bool keys_free = !ui.wants_keyboard();
        if (!input.handle(e, over_panels, keys_free, document, viewport, error)) return false;
        const bool is_key =
            e.kind == window::EventKind::KeyDown || e.kind == window::EventKind::KeyUp;
        // Ctrl is the editor's modifier (Ctrl+Z, Ctrl+Y), so here it does not lower the camera as
        // it does in engine-view; Q does.
        const bool editor_key = is_key && e.key == window::Key::Control;
        if (!editor_key && ((is_key && keys_free) || (!is_key && !over_panels) ||
                            e.kind == window::EventKind::MouseButtonUp)) {
          camera.edge.add(e, camera.next(), camera.looking);
        }
        return true;
      };
      bool failed = false;
      window::Event event;
      while (!failed && window.poll(event)) {
        if (event.kind == window::EventKind::Quit ||
            event.kind == window::EventKind::CloseRequested) {
          running = false;
        }
        if (event.kind == window::EventKind::Resized) resize = true;
        failed = !on_event(event);
      }
      // Once a frame: an iteration that draws nothing (an out-of-date swapchain) keeps the count.
      scripted.clear();
      if (summary.frames != scripted_frame) {
        scripted_frame = summary.frames;
        scripted_events(options, summary.frames, scripted);
      }
      for (const window::Event& e : scripted)
        failed = failed || !on_event(e);
      if (failed) {
        code = 1;
        break;
      }
      if (!running) break;
      window.set_relative_mouse(camera.looking);
      camera.advance(dt_ns);
      if (resize) {
        resize = false;
        if (!swapchain.resize(window.pixel_width(), window.pixel_height(), &error)) {
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
      if (!between_frames(options, summary.frames, scale, document, viewport, ui, input, summary,
                          error)) {
        code = 1;
        break;
      }
      const u32 width = swapchain.extent().width;
      const u32 height = swapchain.extent().height;
      const renderer::Camera view = camera.view();
      input.set_view(document, viewport, view, width, height, scale.y);
      editor::EditorUi::Actions actions;
      ui.frame(static_cast<f32>(dt_ns) / 1.0e9f, width, height, document, viewport, view, input,
               actions);
      if (!toolbar(actions, document, viewport, input, error)) {
        code = 1;
        break;
      }
      renderer::OverlayDrawData overlay;
      if (!ui.render(viewport.renderer(), overlay, error)) {
        code = 1;
        break;
      }
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
      if (options.frames > 0 && summary.frames >= options.frames) running = false;
    }
    if (viewport.valid()) viewport.renderer().wait_idle();
    if (code == 0) {
      finish(summary, document, viewport, ui, input);
      summary.width = swapchain.extent().width;
      summary.height = swapchain.extent().height;
      print_summary(summary, options);
    } else if (!error.empty()) {
      std::fprintf(stderr, "engine-editor: %s\n", error.c_str());
    }
    ui.destroy();
    viewport.destroy();
    swapchain.destroy();
  }
  if (surface != VK_NULL_HANDLE)
    window::vulkan::destroy_surface(device.handles().instance, surface);
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
    } else if (a == "--frames") {
      if (!value(v) || !parse_u32(v, options.frames)) return usage("--frames needs a count");
    } else if (a == "--pick") {
      f64 xy[2];
      if (!value(v) || !parse_list(v, xy, 2) || xy[0] < 0.0 || xy[1] < 0.0)
        return usage("--pick needs X,Y");
      options.pick = true;
      options.pick_x = static_cast<u32>(xy[0]);
      options.pick_y = static_cast<u32>(xy[1]);
    } else if (a == "--move") {
      if (!value(v) || !parse_list(v, options.move_to, 3)) return usage("--move needs X,Y,Z");
      options.move = true;
    } else if (a == "--drag") {
      std::string to;
      if (!value(v) || !value(to) || !parse_list(v, options.drag_from, 2) ||
          !parse_list(to, options.drag_to, 2))
        return usage("--drag needs X0,Y0 X1,Y1");
      options.drag = true;
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

  editor::EditorDocument document;
  std::string error;
  if (!document.open(options.doc, options.create, options.actor, error)) {
    std::fprintf(stderr, "engine-editor: %s\n", error.c_str());
    return 1;
  }
  return options.offscreen ? run_offscreen(options, document) : run_window(options, document);
}
