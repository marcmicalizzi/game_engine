// engine-view: a window on the renderer as it stands. A procedural heightfield is split into
// clusters (domain/geometry), uploaded behind device addresses, and drawn one mesh-shader
// workgroup per cluster into the swapchain with a reversed-Z depth buffer while the camera
// orbits. `--frames N --capture out.png` renders N frames and writes the last one as a PNG, and
// the process prints one JSON line of statistics on exit, so scripts and agents can look at the
// picture and the numbers without a human at the window.
//
// Exit codes: 0 ok; 1 runtime error; 2 usage; 3 unavailable (no display, no Vulkan device, no
// mesh shaders, or no presentation support), which tests treat as a skip.
#include <core/log/log.h>
#include <core/math/math.h>
#include <core/time/time.h>
#include <domain/geometry/cluster.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/capture.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/render_graph.h>
#include <domain/gfx/swapchain.h>
#include <domain/gfx/vulkan.h>
#include <foundation/image/png.h>
#include <foundation/window/window.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <shaders/cluster_mesh.spv.h>
#include <string>
#include <string_view>

using namespace engine;

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_view, "view");

constexpr const char* k_usage =
    "usage: engine-view [--width <px>] [--height <px>] [--frames <n>] [--capture <file.png>]\n"
    "                   [--no-vsync] [--adapter <index>] [--validation] [--grid <n>] [--log "
    "<spec>]\n"
    "\n"
    "  --frames <n>     render n frames, then exit (0: until the window closes)\n"
    "  --capture <png>  write the last frame as a PNG (implies --frames 60 when unset)\n"
    "  --grid <n>       heightfield resolution, n x n vertices (default 129)\n"
    "  --log <spec>     log levels, e.g. \"info,gfx=debug\" (stderr shows warnings and up)\n"
    "exit codes: 0 ok, 1 error, 2 usage, 3 unavailable (no display, device, mesh shaders)\n";

constexpr int k_exit_error = 1;
constexpr int k_exit_usage = 2;
constexpr int k_exit_unavailable = 3;

struct Options {
  u32 width = 1280;
  u32 height = 720;
  u32 frames = 0;
  std::string capture;
  bool vsync = true;
  u32 adapter = 0;
  bool validation = false;
  u32 grid = 129;
  std::string log_spec;
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

// Mirrors MeshParams in domain/gfx/shaders/cluster_mesh.slang.
struct MeshParams {
  Mat4 view_proj;
  u64 clusters;
  u64 vertices;
  u64 triangles;
  u32 cluster_count;
  u32 pad = 0;
};
static_assert(sizeof(MeshParams) == 96);

// An (n x n) heightfield over [-extent, extent]^2 in XZ, dunes-and-ridges in Y.
void make_terrain(u32 n, f32 extent, Vector<Vec3>& positions, Vector<u32>& indices) {
  positions.reserve(n * n);
  for (u32 z = 0; z < n; ++z) {
    for (u32 x = 0; x < n; ++x) {
      const f32 fx = -extent + 2.0f * extent * static_cast<f32>(x) / (n - 1);
      const f32 fz = -extent + 2.0f * extent * static_cast<f32>(z) / (n - 1);
      const f32 h = 0.9f * std::sin(fx * 0.55f) * std::cos(fz * 0.4f) +
                    0.35f * std::sin(fx * 1.7f + fz * 1.1f) +
                    0.12f * std::cos(fx * 4.3f - fz * 3.7f);
      positions.push_back(Vec3{fx, h, fz});
    }
  }
  indices.reserve((n - 1) * (n - 1) * 6);
  for (u32 z = 0; z + 1 < n; ++z) {
    for (u32 x = 0; x + 1 < n; ++x) {
      const u32 a = z * n + x;
      indices.push_back(a);
      indices.push_back(a + n);
      indices.push_back(a + 1);
      indices.push_back(a + 1);
      indices.push_back(a + n);
      indices.push_back(a + n + 1);
    }
  }
}

struct Depth {
  gfx::ImageResource image;
  bool create(const gfx::Device& device, u32 width, u32 height, std::string* error) {
    destroy(device);
    return gfx::create_image_2d(device, width, height, VK_FORMAT_D32_SFLOAT,
                                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, image, error);
  }
  void destroy(const gfx::Device& device) {
    if (image.image != VK_NULL_HANDLE) gfx::destroy_image(device, image);
    image = gfx::ImageResource{};
  }
};

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
               a == "--grid") {
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
    } else if (a == "--capture") {
      if (!next_value(argc, argv, i, a, options.capture)) return k_exit_usage;
    } else if (a == "--log") {
      if (!next_value(argc, argv, i, a, options.log_spec)) return k_exit_usage;
    } else if (a == "--no-vsync") {
      options.vsync = false;
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
  if (!options.capture.empty() && options.frames == 0) options.frames = 60;

  log::StreamSink stderr_sink(stderr, log::StreamSink::Format::Text);
  stderr_sink.set_min_level(log::Level::Warn);
  log::add_sink(&stderr_sink);
  if (!options.log_spec.empty()) log::apply_level_spec(options.log_spec);

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
    return fail("window", error);
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
  if (!device.features().mesh_shader || !device.features().presentation) {
    const std::string why =
        std::string(device.adapter().name) +
        (device.features().mesh_shader ? " cannot present" : " has no mesh shaders");
    device.destroy();
    window.destroy();
    window::shutdown();
    return unavailable(why.c_str(), "");
  }

  int exit_code = 0;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  gfx::Swapchain swapchain;
  gfx::FrameContext frames;
  gfx::BindlessSet bindless;
  gfx::BufferResource cluster_buffer;
  gfx::BufferResource vertex_buffer;
  gfx::BufferResource triangle_buffer;
  VkShaderModule module = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  Depth depth;
  geometry::ClusterMesh mesh;
  u64 rendered = 0;
  i64 started_ns = 0;
  i64 finished_ns = 0;
  bool captured = false;

  // Everything below unwinds through this label so the destruction order stays in one place.
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
    if (!frames.create(device, 2, &error) ||
        !bindless.create(device, gfx::BindlessConfig{}, &error)) {
      exit_code = fail("frames", error);
      break;
    }

    Vector<Vec3> positions;
    Vector<u32> indices;
    make_terrain(options.grid, 10.0f, positions, indices);
    if (!geometry::build_clusters(positions, indices, geometry::ClusterBuildOptions{}, mesh,
                                  &error)) {
      exit_code = fail("clusters", error);
      break;
    }
    if (!gfx::upload_buffer(device, mesh.clusters.data(),
                            mesh.clusters.size() * sizeof(geometry::ClusterDesc),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, cluster_buffer, &error) ||
        !gfx::upload_buffer(device, mesh.vertices.data(), mesh.vertices.size() * sizeof(Vec3),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vertex_buffer, &error) ||
        !gfx::upload_buffer(device, mesh.triangles.data(), mesh.triangles.size() * sizeof(u32),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, triangle_buffer, &error)) {
      exit_code = fail("upload", error);
      break;
    }

    module = gfx::create_shader_module(device, shaders::k_cluster_mesh_spirv,
                                       shaders::k_cluster_mesh_spirv_size, &error);
    if (module == VK_NULL_HANDLE) {
      exit_code = fail("shader", error);
      break;
    }
    gfx::MeshPipelineDesc pipeline_desc;
    pipeline_desc.mesh = module;
    pipeline_desc.fragment = module;
    pipeline_desc.fragment_entry = "fs_color";
    pipeline_desc.layout = bindless.pipeline_layout();
    pipeline_desc.color_format = swapchain.format();
    pipeline_desc.depth_format = VK_FORMAT_D32_SFLOAT;
    pipeline_desc.depth_test = true;
    pipeline_desc.depth_write = true;
    if (!gfx::create_mesh_pipeline(device, pipeline_desc, pipeline, &error)) {
      exit_code = fail("pipeline", error);
      break;
    }
    if (!depth.create(device, swapchain.extent().width, swapchain.extent().height, &error)) {
      exit_code = fail("depth", error);
      break;
    }
    ENGINE_LOG_INFO(log_view, "ready", log::field("clusters", mesh.clusters.size()),
                    log::field("triangles", mesh.source_triangle_count),
                    log::field("width", swapchain.extent().width),
                    log::field("height", swapchain.extent().height));

    gfx::RenderGraph graph(device);
    bool running = true;
    bool resize_pending = false;
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
      if (resize_pending) {
        resize_pending = false;
        if (!swapchain.resize(window.pixel_width(), window.pixel_height(), &error)) {
          exit_code = fail("resize", error);
          break;
        }
        if (window.pixel_width() > 0 && window.pixel_height() > 0) {
          frames.wait_idle();
          if (!depth.create(device, swapchain.extent().width, swapchain.extent().height, &error)) {
            exit_code = fail("depth", error);
            break;
          }
        }
      }

      VkCommandBuffer commands = frames.begin_frame();
      u32 image_index = 0;
      const gfx::PresentStatus acquired =
          swapchain.acquire(frames.acquire_semaphore(), image_index);
      if (acquired != gfx::PresentStatus::Ok) {
        frames.end_frame();
        if (acquired == gfx::PresentStatus::Error) {
          exit_code = fail("acquire", swapchain.last_error());
          break;
        }
        resize_pending = true;  // minimized or out of date: try again next loop
        continue;
      }

      const VkExtent2D extent = swapchain.extent();
      const f32 aspect = static_cast<f32>(extent.width) / static_cast<f32>(extent.height);
      const f32 angle = static_cast<f32>(rendered) * 0.008f;
      const Vec3 eye{std::cos(angle) * 15.0f, 7.5f, std::sin(angle) * 15.0f};
      MeshParams params{};
      params.view_proj = perspective_reversed_z(radians(55.0f), aspect, 0.1f) *
                         look_at(eye, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
      params.clusters = cluster_buffer.address;
      params.vertices = vertex_buffer.address;
      params.triangles = triangle_buffer.address;
      params.cluster_count = mesh.clusters.size();

      graph.reset();
      const gfx::RgImage color = graph.import_image("swapchain", swapchain.image(image_index));
      const gfx::RgImage depth_target = graph.import_image("depth", depth.image);
      VkClearColorValue sky{};
      sky.float32[0] = 0.55f;
      sky.float32[1] = 0.70f;
      sky.float32[2] = 0.90f;
      sky.float32[3] = 1.0f;
      graph.add_pass(
          "terrain", gfx::PassKind::Raster,
          [&](gfx::PassBuilder& b) {
            b.color_attachment(color, VK_ATTACHMENT_LOAD_OP_CLEAR, sky);
            b.depth_attachment(depth_target, VK_ATTACHMENT_LOAD_OP_CLEAR,
                               0.0f);  // reversed Z: far is 0
          },
          [&](VkCommandBuffer cb, gfx::RenderGraph&) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
            vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                               sizeof(params), &params);
            vkCmdDrawMeshTasksEXT(cb, mesh.clusters.size(), 1, 1);
          });
      graph.set_final_layout(color, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
      if (!graph.compile(&error)) {
        frames.end_frame();
        exit_code = fail("graph", error);
        break;
      }
      graph.execute(commands);
      gfx::FrameContext::PresentSync sync;
      sync.wait = frames.acquire_semaphore();
      sync.signal = swapchain.render_finished(image_index);
      const u64 value = frames.end_frame(sync);
      ++rendered;

      const bool last = options.frames != 0 && rendered >= options.frames;
      if (last && !options.capture.empty()) {
        frames.wait(value);
        gfx::Capture capture;
        Vector<u8> rgba;
        if (!gfx::capture_image(device, swapchain.image(image_index),
                                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, capture, &error) ||
            !gfx::capture_to_rgba8(capture, rgba)) {
          exit_code = fail("capture", error.empty() ? "unsupported swapchain format" : error);
        } else if (const io::Status status =
                       image::write_png(options.capture, capture.width, capture.height, 4,
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
    frames.wait_idle();
    graph.reset();
  } while (false);

  frames.wait_idle();
  depth.destroy(device);
  if (pipeline != VK_NULL_HANDLE) gfx::destroy_pipeline(device, pipeline);
  if (module != VK_NULL_HANDLE) gfx::destroy_shader_module(device, module);
  gfx::destroy_buffer(device, triangle_buffer);
  gfx::destroy_buffer(device, vertex_buffer);
  gfx::destroy_buffer(device, cluster_buffer);
  bindless.destroy();
  frames.destroy();
  swapchain.destroy();
  window::Window::destroy_vulkan_surface(device.handles().instance, surface);
  device.destroy();
  window.destroy();
  window::shutdown();

  if (exit_code == 0) {
    const f64 seconds = static_cast<f64>(finished_ns - started_ns) / 1.0e9;
    const f64 avg_ms = rendered > 0 ? seconds * 1000.0 / static_cast<f64>(rendered) : 0.0;
    std::printf(
        "{\"frames\":%llu,\"seconds\":%.3f,\"avg_ms\":%.3f,\"width\":%u,\"height\":%u,"
        "\"clusters\":%u,\"triangles\":%u,\"captured\":%s}\n",
        static_cast<unsigned long long>(rendered), seconds, avg_ms, options.width, options.height,
        mesh.clusters.size(), mesh.source_triangle_count, captured ? "true" : "false");
  }
  log::remove_sink(&stderr_sink);
  return exit_code;
}
