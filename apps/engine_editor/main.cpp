// engine-editor: the editor's first slice (docs/subsystems/apps.md, "engine-editor"; ADR-0054;
// roadmap A31). One window: the renderer's picture of a document's placements, engine-view's fly
// camera, and Dear ImGui panels docked round it — an outliner, a property panel generated from the
// selected record's schema descriptor, and the session's journal. A click in the picture selects
// the record under it, read from the visibility buffer. A transaction that moves a placement moves
// it in the next frame.
//
// The document is opened through `protocol::Session` and every edit goes through its checked write
// path (editor_document.h); everything drawn is the renderer's (editor_viewport.h). `--offscreen`
// runs the same frames with no window, and `--frames`, `--pick`, `--move` and `--capture` are how
// a test drives either mode and reads the answer from the one JSON line it prints at the end.

#include "../engine_view/fly_camera.h"
#include "../engine_view/window_input.h"
#include "editor_document.h"
#include "editor_ui.h"
#include "editor_viewport.h"

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
    "              [--offscreen] [--frames N] [--pick X,Y] [--move X,Y,Z] [--capture out.png]\n"
    "              [--ddc <dir>]\n"
    "  --doc        the document directory (protocol::Session opens it)\n"
    "  --create     create the document when the directory holds none\n"
    "  --actor      who the edits are attributed to (role: director)\n"
    "  --offscreen  no window: draw --frames frames into the renderer's own target\n"
    "  --frames N   stop after N frames (0: until the window closes)\n"
    "  --pick X,Y   after the second frame, select what is under pixel (X, Y)\n"
    "  --move X,Y,Z after picking, set the selection's translation in one transaction\n"
    "  --capture    offscreen: the last frame, panels and all, as a PNG\n"
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
  input::InputState input;
  view::EdgeEvents edge;
  FixedStepClock clock{240, 60};
  SimTick fed{0};
  bool looking = false;

  bool start(const renderer::Camera& from, f32 radius, std::string& error) {
    state = view::fly_state_from_camera(from);
    params.speed = std::max(1.0f, radius * 0.5f);
    const input::ActionMap map = view::default_fly_map();
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
  JsonValue panels = JsonValue::object();
  panels["outliner"] = JsonValue(s.outliner);
  panels["properties"] = JsonValue(s.properties);
  panels["journal"] = JsonValue(s.journal);
  out["panels"] = std::move(panels);
  JsonWriteOptions compact;
  compact.pretty = false;  // one line, the last the run prints: what a test reads
  std::printf("%s\n", write_json(out, compact).c_str());
}

// What both modes do between frames: the scripted pick and move, then the viewport brought up to
// the document.
bool between_frames(const Options& options, u64 frame, editor::EditorDocument& document,
                    editor::Viewport& viewport, editor::EditorUi& ui, Summary& summary,
                    std::string& error) {
  if (options.pick && frame == 2) {
    Id128 picked;
    if (!viewport.pick(options.pick_x, options.pick_y, picked, error)) return false;
    document.select(picked);
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
  bool rebuilt = false;
  if (!viewport.sync(document.document(), rebuilt, error)) return false;
  if (rebuilt) {
    ui.forget_textures();
    ++summary.rebuilds;
  }
  return true;
}

void finish(Summary& summary, const editor::EditorDocument& document,
            const editor::Viewport& viewport, const editor::EditorUi& ui) {
  summary.selected = document.selection();
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
    Summary summary;
    const u32 frames = options.frames > 0 ? options.frames : 3u;
    for (u64 f = 0; f < frames && code == 0; ++f) {
      if (!between_frames(options, f, document, viewport, ui, summary, error)) {
        code = 1;
        break;
      }
      const renderer::Camera view = camera.view();
      editor::EditorUi::Actions actions;
      ui.frame(1.0f / 60.0f, options.width, options.height, document, viewport, view, actions);
      if (actions.select) document.select(actions.selected);
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
      finish(summary, document, viewport, ui);
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
    Camera camera;
    Summary summary;
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
      window::Event event;
      while (window.poll(event)) {
        if (event.kind == window::EventKind::Quit ||
            event.kind == window::EventKind::CloseRequested) {
          running = false;
        }
        if (event.kind == window::EventKind::Resized) resize = true;
        ui.feed(event);
        const bool over_panels = ui.wants_mouse();
        if (event.kind == window::EventKind::MouseButtonDown && event.button == 3 && !over_panels)
          camera.looking = true;
        if (event.kind == window::EventKind::MouseButtonUp && event.button == 3)
          camera.looking = false;
        if (event.kind == window::EventKind::MouseButtonDown && event.button == 1 && !over_panels) {
          Id128 picked;
          if (viewport.pick(static_cast<u32>(event.x), static_cast<u32>(event.y), picked, error))
            document.select(picked);
        }
        const bool keys_free = !ui.wants_keyboard();
        const bool is_key =
            event.kind == window::EventKind::KeyDown || event.kind == window::EventKind::KeyUp;
        if ((is_key && keys_free) || (!is_key && !over_panels) ||
            event.kind == window::EventKind::MouseButtonUp) {
          camera.edge.add(event, camera.next(), camera.looking);
        }
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
      if (!between_frames(options, summary.frames, document, viewport, ui, summary, error)) {
        code = 1;
        break;
      }
      const u32 width = swapchain.extent().width;
      const u32 height = swapchain.extent().height;
      const renderer::Camera view = camera.view();
      editor::EditorUi::Actions actions;
      ui.frame(static_cast<f32>(dt_ns) / 1.0e9f, width, height, document, viewport, view, actions);
      if (actions.select) document.select(actions.selected);
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
      finish(summary, document, viewport, ui);
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

  editor::EditorDocument document;
  std::string error;
  if (!document.open(options.doc, options.create, options.actor, error)) {
    std::fprintf(stderr, "engine-editor: %s\n", error.c_str());
    return 1;
  }
  return options.offscreen ? run_offscreen(options, document) : run_window(options, document);
}
