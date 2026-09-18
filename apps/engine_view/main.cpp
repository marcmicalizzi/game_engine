// engine-view: the window on `systems/renderer`. Everything that draws lives in that module now
// (docs/subsystems/renderer.md); this file is the command line over it — the flags, the window
// and its swapchain, the event loop, the capture, and the one JSON line of statistics that
// scripts and agents read. The renderer itself never sees a surface: engine-view acquires a
// swapchain image, hands it in as the frame's color target with the two semaphores, and
// presents it afterwards, which is the only difference between what this draws and what
// engine-host's `render.capture` draws offscreen.
//
// `--frames N --capture out.png` renders N frames and writes the last one as a PNG, then prints
// the statistics line on exit, so there is no need for a human at the window. `--mesh` takes a
// glTF file or a `.clusters` container; a glTF is looked up in the derived-data cache first and
// built into it on a miss. Shaders come from the build's manifest when it is found and
// recompile when their sources change.
//
// Exit codes: 0 ok; 1 runtime error; 2 usage; 3 unavailable (no display, no Vulkan device, no
// mesh shaders, or no presentation support), which tests treat as a skip.
#include <core/log/log.h>
#include <core/math/math.h>
#include <core/platform/process.h>
#include <core/time/time.h>
#include <domain/gfx/capture.h>
#include <domain/gfx/device.h>
#include <domain/gfx/swapchain.h>
#include <domain/gfx/vulkan.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>
#include <foundation/window/window.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

using namespace engine;

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_view, "view");

// clang-format off
constexpr const char* k_usage =
    "usage: engine-view [--width <px>] [--height <px>] [--frames <n>] [--capture <file.png>]\n"
    "                   [--no-vsync] [--adapter <index>] [--validation] [--grid <n>] [--log <spec>]\n"
    "                   [--shaders <manifest.json>] [--lod <px>] [--no-cull] [--no-occlusion] [--no-cone]\n"
    "                   [--raster direct|hw|vertex|sw|auto|rt] [--sw-px <px>] [--view <mode>] [--orbit <d>]\n"
    "                   [--mesh <file.gltf|file.glb|file.clusters>] [--scene <file.json>]\n"
    "                   [--grid-instances <n>] [--no-cache] [--ddc <dir>] [--no-lights]\n"
    "                   [--deform none|identity|wave|lattice] [--deform-amplitude <a>] [--rt-templates]\n"
    "                   [--shadows off|rt]\n"
    "\n"
    "  --frames <n>     render n frames, then exit (0: until the window closes)\n"
    "  --capture <png>  write the last frame as a PNG (implies --frames 60 when unset)\n"
    "  --grid <n>       heightfield resolution, n x n vertices (default 257)\n"
    "  --mesh <file>    render a mesh instead of the heightfield: a glTF 2.0 or GLB file (one\n"
    "                   cluster DAG per primitive; materials with their base-color,\n"
    "                   metallic-roughness, and normal textures from the file), or a .clusters\n"
    "                   container that already holds one\n"
    "  --scene <json>   {\"meshes\":[{\"path\":\"...\"}],\"instances\":[{\"mesh\":0,\"translation\":[x,y,z],\n"
    "                   \"rotation\":[x,y,z,w],\"scale\":[x,y,z]}]}; paths are relative to the file\n"
    "  --grid-instances <n>  place the loaded mesh n x n times with varied rotation and scale\n"
    "  --no-cache       always build a glTF from source; do not read or write ddc/clusters\n"
    "  --ddc <dir>      the derived-data root (default: <repo>/ddc, found beside AGENTS.md)\n"
    "  --lod <px>       screen-space error threshold in pixels for LOD selection (default 1)\n"
    "  --no-cull        draw every leaf cluster; no GPU culling or LOD selection\n"
    "  --no-occlusion   skip two-pass occlusion culling (hw mode only; on by default)\n"
    "  --no-cone        skip backface culling of clusters by their normal cones (on by default)\n"
    "  --no-lights      only the sun and the sky; no orbiting point lights (they are on by default)\n"
    "  --shadows <how>  off, or rt: every light in the resolve casts a ray-traced shadow against\n"
    "                   the structures the frame built from its own visible list. The default is\n"
    "                   rt where the device has cluster acceleration structures and ray queries.\n"
    "                   In a raster mode the frame runs the acceleration structure chain as well,\n"
    "                   which turns two-pass occlusion culling off (one visible list to build from)\n"
    "  --raster <mode>  direct: mesh shaders to color with a depth buffer; hw (default), vertex, sw,\n"
    "                   auto: the visibility buffer through mesh shaders, a vertex shader (the\n"
    "                   baseline tier, chosen automatically without mesh shaders), software, or\n"
    "                   both split by size; rt: ray queries against cluster acceleration structures\n"
    "                   built every frame from the cull output (NVIDIA RTX only)\n"
    "  --sw-px <px>     auto mode: clusters narrower than this go to the software rasterizer (32)\n"
    "  --view <mode>    id, tri, depth, shaded (default: materials with vertex normals, textures,\n"
    "                   and normal maps under a sun), normals, uv\n"
    "  --orbit <d>      orbit at a fixed distance instead of breathing between 8 and 36 units;\n"
    "                   distances scale with the scene radius (10 for the heightfield)\n"
    "  --deform <mode>  deform every instance through the per-frame deformed-vertex pool\n"
    "                   (experiment E25): identity writes the rest pose, wave displaces along the\n"
    "                   vertex normal, lattice runs a 3x3x3 cage over the mesh's bounds\n"
    "  --deform-amplitude <a>  displacement as a fraction of the mesh's bounds (default 0.02)\n"
    "  --rt-templates   --raster rt: build one cluster template per cluster at load and\n"
    "                   instantiate the cut's templates each frame instead of rebuilding the CLAS\n"
    "  --log <spec>     log levels, e.g. \"info,gfx=debug\" (stderr shows warnings and up)\n"
    "  --shaders <m>    shader manifest (default: <exe dir>/../shaders/manifest.json when present);\n"
    "                   shaders recompile and reload when their .slang sources change\n"
    "exit codes: 0 ok, 1 error, 2 usage, 3 unavailable (no display, device, mesh shaders)\n";
// clang-format on

constexpr int k_exit_error = 1;
constexpr int k_exit_usage = 2;
constexpr int k_exit_unavailable = 3;
constexpr u32 k_frames_in_flight = 2;

struct Options {
  u32 width = 1280;
  u32 height = 720;
  u32 frames = 0;
  std::string capture;
  bool vsync = true;
  u32 adapter = 0;
  bool validation = false;
  u32 grid = 257;
  std::string log_spec;
  std::string shaders;
  std::string mesh;        // glTF or .clusters file; empty renders the heightfield
  std::string scene;       // a scene JSON file: meshes and instances of them
  std::string ddc;         // derived-data root; empty is found from the executable
  u32 grid_instances = 0;  // n: place the one mesh n x n times
  bool cache = true;
  f32 orbit = 0.0f;  // 0: breathe
  renderer::RenderSettings settings;
};

bool next_value(int argc, char** argv, int& i, std::string_view flag, std::string& out) {
  if (i + 1 >= argc) {
    std::fprintf(stderr, "engine-view: %.*s needs a value\n", static_cast<int>(flag.size()),
                 flag.data());
    return false;
  }
  out = argv[++i];
  return true;
}

bool parse_u32(const std::string& text, u32& out) {
  char* end = nullptr;
  const unsigned long v = std::strtoul(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0' || v > 0xFFFFFFFFul) return false;
  out = static_cast<u32>(v);
  return true;
}

bool parse_f32(const std::string& text, f32& out) {
  char* end = nullptr;
  const double v = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0' || !(v > 0.0) || v > 1.0e6) return false;
  out = static_cast<f32>(v);
  return true;
}

int fail(const char* what, const std::string& error) {
  std::fprintf(stderr, "engine-view: %s: %s\n", what, error.c_str());
  return k_exit_error;
}

int unavailable(const char* what, const std::string& error) {
  std::fprintf(stderr, "engine-view: unavailable: %s%s%s\n", what, error.empty() ? "" : ": ",
               error.c_str());
  return k_exit_unavailable;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    std::string value;
    if (a == "--help" || a == "-h") {
      std::fputs(k_usage, stdout);
      return 0;
    } else if (a == "--width" || a == "--height" || a == "--frames" || a == "--adapter" ||
               a == "--grid" || a == "--grid-instances") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      u32 n = 0;
      if (!parse_u32(value, n)) {
        std::fprintf(stderr, "engine-view: %.*s expects a number\n", static_cast<int>(a.size()),
                     a.data());
        return k_exit_usage;
      }
      if (a == "--width") options.width = n;
      if (a == "--height") options.height = n;
      if (a == "--frames") options.frames = n;
      if (a == "--adapter") options.adapter = n;
      if (a == "--grid") options.grid = n;
      if (a == "--grid-instances") options.grid_instances = n;
    } else if (a == "--lod" || a == "--sw-px" || a == "--orbit" || a == "--deform-amplitude") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      f32 px = 0.0f;
      if (!parse_f32(value, px)) {
        std::fprintf(stderr, "engine-view: %.*s expects a positive number\n",
                     static_cast<int>(a.size()), a.data());
        return k_exit_usage;
      }
      (a == "--lod"     ? options.settings.lod_px
       : a == "--sw-px" ? options.settings.sw_px
       : a == "--orbit" ? options.orbit
                        : options.settings.deform_amplitude) = px;
    } else if (a == "--deform") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      if (!renderer::parse_deform_mode(value, options.settings.deform,
                                       options.settings.deform_kind)) {
        std::fprintf(stderr, "engine-view: --deform expects none, identity, wave, or lattice\n");
        return k_exit_usage;
      }
    } else if (a == "--rt-templates") {
      options.settings.rt_templates = true;
    } else if (a == "--raster") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      if (!renderer::parse_raster_mode(value, options.settings.raster)) {
        std::fprintf(stderr, "engine-view: --raster expects direct, hw, vertex, sw, auto, or rt\n");
        return k_exit_usage;
      }
    } else if (a == "--shadows") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      // `auto` is the default, not a spelling the flag takes: it is what no flag means.
      if (value == "auto" || !renderer::parse_shadow_mode(value, options.settings.shadows)) {
        std::fprintf(stderr, "engine-view: --shadows expects off or rt\n");
        return k_exit_usage;
      }
    } else if (a == "--view") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      if (!renderer::parse_view_mode(value, options.settings.view_mode)) {
        std::fprintf(stderr,
                     "engine-view: --view expects id, tri, depth, shaded, normals, or uv\n");
        return k_exit_usage;
      }
    } else if (a == "--capture") {
      if (!next_value(argc, argv, i, a, options.capture)) return k_exit_usage;
    } else if (a == "--log") {
      if (!next_value(argc, argv, i, a, options.log_spec)) return k_exit_usage;
    } else if (a == "--shaders") {
      if (!next_value(argc, argv, i, a, options.shaders)) return k_exit_usage;
    } else if (a == "--mesh") {
      if (!next_value(argc, argv, i, a, options.mesh)) return k_exit_usage;
    } else if (a == "--scene") {
      if (!next_value(argc, argv, i, a, options.scene)) return k_exit_usage;
    } else if (a == "--ddc") {
      if (!next_value(argc, argv, i, a, options.ddc)) return k_exit_usage;
    } else if (a == "--no-cache") {
      options.cache = false;
    } else if (a == "--no-vsync") {
      options.vsync = false;
    } else if (a == "--no-cull") {
      options.settings.cull = false;
    } else if (a == "--no-occlusion") {
      options.settings.occlusion = false;
    } else if (a == "--no-cone") {
      options.settings.cone = false;
    } else if (a == "--no-lights") {
      options.settings.lights = false;
    } else if (a == "--validation") {
      options.validation = true;
    } else {
      std::fprintf(stderr, "engine-view: unknown argument %.*s\n%s", static_cast<int>(a.size()),
                   a.data(), k_usage);
      return k_exit_usage;
    }
  }
  if (options.width == 0 || options.height == 0 || options.grid < 2 || options.grid > 2048) {
    std::fprintf(stderr, "engine-view: size must be positive and --grid within 2..2048\n");
    return k_exit_usage;
  }
  if (options.grid_instances > 64) {
    std::fprintf(stderr, "engine-view: --grid-instances must be at most 64\n");
    return k_exit_usage;
  }
  if (!options.scene.empty() && (!options.mesh.empty() || options.grid_instances != 0)) {
    std::fprintf(stderr, "engine-view: --scene names its own meshes and instances\n");
    return k_exit_usage;
  }
  if (!options.capture.empty() && options.frames == 0) options.frames = 60;

  log::StreamSink stderr_sink(stderr, log::StreamSink::Format::Text);
  stderr_sink.set_min_level(log::Level::Warn);
  log::add_sink(&stderr_sink);
  if (!options.log_spec.empty()) {
    // With an explicit spec the category levels decide what reaches stderr.
    stderr_sink.set_min_level(log::Level::Trace);
    log::apply_level_spec("warn");
    log::apply_level_spec(options.log_spec);
  }

  std::string error;
  if (!window::init(&error)) return unavailable("no display", error);
  const auto extensions = window::Window::vulkan_instance_extensions();
  if (extensions.empty()) {
    window::shutdown();
    return unavailable("SDL has no Vulkan support here", "");
  }
  window::WindowDesc window_desc;
  window_desc.title = "engine-view";
  window_desc.width = options.width;
  window_desc.height = options.height;
  window::Window window;
  if (!window.create(window_desc, &error)) {
    window::shutdown();
    return unavailable("cannot create a window", error);
  }

  gfx::DeviceOptions device_options;
  device_options.adapter_index = options.adapter;
  device_options.validation = options.validation;
  device_options.instance_extensions = extensions.data();
  device_options.instance_extension_count = static_cast<u32>(extensions.size());
  gfx::Device device;
  if (!device.create(device_options, &error)) {
    window.destroy();
    window::shutdown();
    return unavailable("no Vulkan device", error);
  }
  if (!device.features().presentation) {
    const std::string why = std::string(device.adapter().name) + " cannot present";
    device.destroy();
    window.destroy();
    window::shutdown();
    return unavailable(why.c_str(), "");
  }

  int exit_code = 0;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  gfx::Swapchain swapchain;
  renderer::SceneData scene_data;
  renderer::ResolvedSettings resolved;
  renderer::GpuScene scene;
  renderer::SceneRenderer view_renderer;
  u64 rendered = 0;
  i64 started_ns = 0;
  i64 finished_ns = 0;
  bool captured = false;
  u32 extent_width = options.width;
  u32 extent_height = options.height;
  // Read out of the GPU scene while it is alive, because the summary prints after it is gone.
  u64 deform_pool_bytes = 0;
  u64 template_bytes = 0;

  // Everything below unwinds through this block so the destruction order stays in one place.
  do {
    if (!window.create_vulkan_surface(device.handles().instance, surface, &error)) {
      exit_code = fail("surface", error);
      break;
    }
    gfx::SwapchainDesc swapchain_desc;
    swapchain_desc.surface = surface;
    swapchain_desc.width = window.pixel_width();
    swapchain_desc.height = window.pixel_height();
    swapchain_desc.vsync = options.vsync;
    if (!swapchain.create(device, swapchain_desc, &error)) {
      exit_code = fail("swapchain", error);
      break;
    }
    if (!options.capture.empty() && !swapchain.transfer_src()) {
      exit_code = fail("capture", "the surface does not allow reading presented images back");
      break;
    }

    // The scene: the file's meshes and instances, a grid of one mesh, or the heightfield.
    renderer::SceneDesc desc;
    desc.heightfield_grid = options.grid;
    desc.grid_instances = options.grid_instances;
    desc.ddc = options.ddc;
    desc.cache = options.cache;
    if (!options.scene.empty()) {
      if (!renderer::read_scene_file(options.scene, desc, error)) {
        exit_code = fail("scene", error);
        break;
      }
    } else {
      desc.meshes.push_back(options.mesh);  // empty: the procedural heightfield
    }
    if (!renderer::load_scene(desc, scene_data, error)) {
      exit_code = fail("mesh", error);
      break;
    }
    renderer::resolve_settings(options.settings, device.features(), &scene_data, resolved);
    const renderer::RenderAvailability availability =
        renderer::check_availability(resolved, device.features());
    if (availability != renderer::RenderAvailability::Ok) {
      const std::string why =
          std::string(device.adapter().name) + " " + renderer::availability_message(availability);
      exit_code = unavailable(why.c_str(), "");
      break;
    }
    if (!scene.create(device, scene_data, resolved, &error)) {
      exit_code = fail("scene", error);
      break;
    }
    deform_pool_bytes = scene.deform_pool_bytes();
    template_bytes = scene.template_bytes();
    renderer::SceneRenderer::Desc renderer_desc;
    renderer_desc.width = swapchain.extent().width;
    renderer_desc.height = swapchain.extent().height;
    renderer_desc.color_format = swapchain.format();
    renderer_desc.frames_in_flight = k_frames_in_flight;
    renderer_desc.offscreen = false;  // the swapchain image is the target
    renderer_desc.shader_manifest = options.shaders;
    if (!view_renderer.create(device, scene, resolved, renderer_desc, &error)) {
      exit_code = fail("renderer", error);
      break;
    }
    extent_width = view_renderer.width();
    extent_height = view_renderer.height();
    ENGINE_LOG_INFO(log_view, "ready", log::field("clusters", scene_data.cluster_count()),
                    log::field("leaf_clusters", scene_data.leaf_count()),
                    log::field("triangles", scene_data.lod.leaf_triangle_count),
                    log::field("lod_levels", scene_data.lod.level_cluster_counts.size()),
                    log::field("build_ms", static_cast<f64>(scene_data.build_ns) / 1.0e6),
                    log::field("raster", renderer::raster_name(resolved.settings.raster)),
                    log::field("occlusion", resolved.occlusion),
                    log::field("shadows", resolved.shadows ? "rt" : "off"),
                    log::field("cone", resolved.settings.cone),
                    log::field("mesh_primitives", scene_data.mesh_primitives),
                    log::field("materials", scene.material_count()),
                    log::field("meshes", scene_data.parts.size()),
                    log::field("instances", scene.instance_count()),
                    log::field("pairs", scene.pair_count()),
                    log::field("deform", renderer::deform_name(resolved.settings)),
                    log::field("rt_templates", resolved.settings.rt_templates),
                    log::field("width", extent_width), log::field("height", extent_height));

    bool running = true;
    bool resize_pending = false;
    i64 last_shader_poll_ns = 0;
    started_ns = time::monotonic_ns();
    while (running) {
      window::Event event;
      while (window.poll(event)) {
        switch (event.kind) {
          case window::EventKind::Quit:
          case window::EventKind::CloseRequested: running = false; break;
          case window::EventKind::Resized: resize_pending = true; break;
          case window::EventKind::KeyDown:
            if (event.key == window::Key::Escape) running = false;
            break;
          default: break;
        }
      }
      if (!running) break;

      // Hot reload: recompile edited shaders four times a second and rebuild every pipeline.
      if (time::monotonic_ns() - last_shader_poll_ns > 250'000'000) {
        last_shader_poll_ns = time::monotonic_ns();
        Vector<std::string> changed;
        std::string reload_error;
        const bool ok = view_renderer.poll_shaders(changed, &reload_error);
        if (!reload_error.empty()) {
          std::fprintf(stderr, "engine-view: shader compile error:\n%s\n", reload_error.c_str());
        }
        if (!ok) {
          exit_code = fail("pipelines", reload_error);
          break;
        }
        for (const std::string& name : changed) {
          ENGINE_LOG_INFO(log_view, "pipelines rebuilt after shader reload",
                          log::field("shader", name));
        }
      }
      if (resize_pending) {
        resize_pending = false;
        if (!swapchain.resize(window.pixel_width(), window.pixel_height(), &error)) {
          exit_code = fail("resize", error);
          break;
        }
        if (window.pixel_width() > 0 && window.pixel_height() > 0 &&
            !view_renderer.resize(swapchain.extent().width, swapchain.extent().height, &error)) {
          exit_code = fail("targets", error);
          break;
        }
      }

      view_renderer.begin_frame();
      u32 image_index = 0;
      const gfx::PresentStatus acquired =
          swapchain.acquire(view_renderer.acquire_semaphore(), image_index);
      if (acquired != gfx::PresentStatus::Ok) {
        view_renderer.abort_frame();
        if (acquired == gfx::PresentStatus::Error) {
          exit_code = fail("acquire", swapchain.last_error());
          break;
        }
        resize_pending = true;  // minimized or out of date: try again next loop
        continue;
      }
      extent_width = view_renderer.width();
      extent_height = view_renderer.height();

      renderer::FrameDesc frame;
      frame.camera =
          renderer::orbit_camera(scene_data.center, scene_data.radius, options.orbit, rendered);
      frame.frame_index = rendered;
      frame.color = swapchain.image(image_index);
      frame.final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      frame.wait = view_renderer.acquire_semaphore();
      frame.signal = swapchain.render_finished(image_index);
      const u64 value = view_renderer.submit_frame(frame, &error);
      if (value == 0) {
        exit_code = fail("frame", error);
        break;
      }
      ++rendered;

      const bool last = options.frames != 0 && rendered >= options.frames;
      if (last && !options.capture.empty()) {
        view_renderer.wait(value);
        view_renderer.collect_visible();
        gfx::Capture shot;
        Vector<u8> rgba;
        if (!gfx::capture_image(device, swapchain.image(image_index),
                                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, shot, &error) ||
            !gfx::capture_to_rgba8(shot, rgba)) {
          exit_code = fail("capture", error.empty() ? "unsupported swapchain format" : error);
        } else if (const io::Status status =
                       image::write_png(options.capture, shot.width, shot.height, 4,
                                        std::span<const u8>(rgba.data(), rgba.size()));
                   status != io::Status::Ok) {
          exit_code = fail("capture", std::string("cannot write ") + options.capture + ": " +
                                          io::status_name(status));
        } else {
          captured = true;
        }
      }
      const gfx::PresentStatus presented =
          swapchain.present(image_index, swapchain.render_finished(image_index));
      if (presented == gfx::PresentStatus::Error) {
        exit_code = fail("present", swapchain.last_error());
        break;
      }
      if (presented == gfx::PresentStatus::OutOfDate) resize_pending = true;
      if (last || exit_code != 0) running = false;
    }
    finished_ns = time::monotonic_ns();
    if (view_renderer.valid()) view_renderer.wait_idle();
  } while (false);

  const renderer::Stats stats = view_renderer.stats();
  view_renderer.destroy();
  scene.destroy();
  swapchain.destroy();
  window::Window::destroy_vulkan_surface(device.handles().instance, surface);
  device.destroy();
  window.destroy();
  window::shutdown();

  if (exit_code == 0) {
    const f64 seconds = static_cast<f64>(finished_ns - started_ns) / 1.0e9;
    const f64 avg_ms = rendered > 0 ? seconds * 1000.0 / static_cast<f64>(rendered) : 0.0;
    const u32 visible_min = stats.visible_min == ~u32{0} ? 0u : stats.visible_min;
    std::printf(
        "{\"frames\":%llu,\"seconds\":%.3f,\"avg_ms\":%.3f,\"width\":%u,\"height\":%u,"
        "\"clusters\":%u,\"leaf_clusters\":%u,\"triangles\":%u,\"lod_levels\":%u,\"build_ms\":%.1f,"
        "\"mesh_primitives\":%u,\"mesh_cache\":\"%s\",\"meshes\":%u,\"instances\":%u,\"pairs\":%u,"
        "\"cull\":%s,\"occlusion\":%s,\"cone\":%s,\"lod_px\":%.2f,\"raster\":\"%s\","
        "\"shadows\":\"%s\",\"sw_px\":%.1f,"
        "\"visible_hw_last\":%u,\"visible_pass2_last\":%u,\"visible_sw_last\":%u,"
        "\"visible_pairs_last\":%u,\"visible_min\":%u,"
        "\"visible_max\":%u,"
        "\"deform\":\"%s\",\"deform_pool_bytes\":%llu,\"rt_templates\":%s,"
        "\"template_bytes\":%llu,"
        "\"gpu_ms\":{\"cull\":%.4f,\"hw\":%.4f,\"sw\":%.4f,\"hiz\":%.4f,\"resolve\":%.4f,"
        "\"rt\":%.4f,\"clas\":%.4f,\"deform\":%.4f,\"trace\":%.4f,\"total\":%.4f,"
        "\"frames\":%llu},\"captured\":%s}\n",
        static_cast<unsigned long long>(rendered), seconds, avg_ms, extent_width, extent_height,
        scene_data.cluster_count(), scene_data.leaf_count(), scene_data.lod.leaf_triangle_count,
        scene_data.lod.level_cluster_counts.size(), static_cast<f64>(scene_data.build_ns) / 1.0e6,
        scene_data.mesh_primitives, scene_data.mesh_cache, scene_data.parts.size(),
        scene_data.instances.size(), scene_data.pair_count,
        resolved.settings.cull ? "true" : "false", resolved.occlusion ? "true" : "false",
        resolved.settings.cone ? "true" : "false", static_cast<f64>(resolved.settings.lod_px),
        renderer::raster_name(resolved.settings.raster), resolved.shadows ? "rt" : "off",
        static_cast<f64>(resolved.settings.sw_px), stats.visible_hw, stats.visible_pass2,
        stats.visible_sw, stats.visible_pairs(), visible_min, stats.visible_max,
        renderer::deform_name(resolved.settings),
        static_cast<unsigned long long>(deform_pool_bytes),
        resolved.settings.rt_templates ? "true" : "false",
        static_cast<unsigned long long>(template_bytes), stats.cull_ms(), stats.hw_ms(),
        stats.sw_ms(), stats.hiz_ms(), stats.resolve_ms(), stats.rt_ms(), stats.clas_ms(),
        stats.deform_ms(), stats.trace_ms(), stats.total_ms(),
        static_cast<unsigned long long>(stats.timed_frames), captured ? "true" : "false");
  }
  log::remove_sink(&stderr_sink);
  return exit_code;
}
