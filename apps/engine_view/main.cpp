// engine-view: a window on the renderer as it stands. A procedural heightfield is built into
// a cluster LOD DAG (domain/geometry), uploaded behind device addresses, culled and LOD-selected
// on the GPU every frame (cluster_cull.slang), and drawn one mesh-shader workgroup per surviving
// cluster through an indirect draw into the swapchain with a reversed-Z depth buffer while the
// camera orbits and zooms. `--frames N --capture out.png` renders N frames and writes the last
// one as a PNG, and the process prints one JSON line of statistics on exit, so scripts and
// agents can look at the picture and the numbers without a human at the window. Shaders come
// from the build's manifest when it is found and recompile when their sources change.
//
// Exit codes: 0 ok; 1 runtime error; 2 usage; 3 unavailable (no display, no Vulkan device, no
// mesh shaders, or no presentation support), which tests treat as a skip.
#include <core/log/log.h>
#include <core/math/math.h>
#include <core/platform/process.h>
#include <core/time/time.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/capture.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/render_graph.h>
#include <domain/gfx/shader_library.h>
#include <domain/gfx/swapchain.h>
#include <domain/gfx/vulkan.h>
#include <foundation/image/png.h>
#include <foundation/window/window.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <shaders/cluster_cull.spv.h>
#include <shaders/cluster_mesh.spv.h>
#include <string>
#include <string_view>

using namespace engine;

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_view, "view");

// clang-format off
constexpr const char* k_usage =
    "usage: engine-view [--width <px>] [--height <px>] [--frames <n>] [--capture <file.png>]\n"
    "                   [--no-vsync] [--adapter <index>] [--validation] [--grid <n>] [--log <spec>]\n"
    "                   [--shaders <manifest.json>] [--lod <px>] [--no-cull]\n"
    "\n"
    "  --frames <n>     render n frames, then exit (0: until the window closes)\n"
    "  --capture <png>  write the last frame as a PNG (implies --frames 60 when unset)\n"
    "  --grid <n>       heightfield resolution, n x n vertices (default 257)\n"
    "  --lod <px>       screen-space error threshold in pixels for LOD selection (default 1)\n"
    "  --no-cull        draw every leaf cluster; no GPU culling or LOD selection\n"
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
  f32 lod_px = 1.0f;
  bool cull = true;
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

// Mirrors MeshParams in domain/gfx/shaders/cluster_mesh.slang.
struct MeshParams {
  Mat4 view_proj;
  u64 clusters;
  u64 vertices;
  u64 triangles;
  u32 cluster_count;
  u32 pad = 0;
  u64 visible = 0;  // cull output; 0 draws clusters in index order
};
static_assert(sizeof(MeshParams) == 104);

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
    } else if (a == "--lod") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      if (!parse_f32(value, options.lod_px)) {
        std::fprintf(stderr, "engine-view: --lod expects a positive number of pixels\n");
        return k_exit_usage;
      }
    } else if (a == "--capture") {
      if (!next_value(argc, argv, i, a, options.capture)) return k_exit_usage;
    } else if (a == "--log") {
      if (!next_value(argc, argv, i, a, options.log_spec)) return k_exit_usage;
    } else if (a == "--shaders") {
      if (!next_value(argc, argv, i, a, options.shaders)) return k_exit_usage;
    } else if (a == "--no-vsync") {
      options.vsync = false;
    } else if (a == "--no-cull") {
      options.cull = false;
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
  gfx::BufferResource lod_buffer;
  gfx::BufferResource visible_buffer;
  gfx::BufferResource args_buffer;
  gfx::BufferResource params_buffers[k_frames_in_flight];  // host-visible CullParams per slot
  gfx::BufferResource stats_buffers[k_frames_in_flight];   // host-visible copy of the draw args
  gfx::ShaderLibrary shader_library;
  const gfx::Shader* cluster_shader = nullptr;
  const gfx::Shader* cull_shader = nullptr;
  VkPipeline pipeline = VK_NULL_HANDLE;
  gfx::ComputePipeline cull_pipeline;
  Depth depth;
  geometry::ClusterLodMesh lod;
  u64 rendered = 0;
  i64 started_ns = 0;
  i64 finished_ns = 0;
  bool captured = false;
  u32 visible_last = 0;
  u32 visible_min = ~u32{0};
  u32 visible_max = 0;
  i64 build_ns = 0;

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
    if (!frames.create(device, k_frames_in_flight, &error) ||
        !bindless.create(device, gfx::BindlessConfig{}, &error)) {
      exit_code = fail("frames", error);
      break;
    }

    // Geometry: the terrain and its LOD DAG.
    Vector<Vec3> positions;
    Vector<u32> indices;
    make_terrain(options.grid, 10.0f, positions, indices);
    const i64 build_start = time::monotonic_ns();
    if (!geometry::build_cluster_lod(positions, indices, geometry::ClusterLodOptions{}, lod,
                                     &error)) {
      exit_code = fail("clusters", error);
      break;
    }
    build_ns = time::monotonic_ns() - build_start;
    const u32 cluster_count = lod.mesh.clusters.size();
    const u32 leaf_count = lod.level_cluster_counts[0];
    constexpr VkBufferUsageFlags k_storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (!gfx::upload_buffer(device, lod.mesh.clusters.data(),
                            cluster_count * sizeof(geometry::ClusterDesc), k_storage,
                            cluster_buffer, &error) ||
        !gfx::upload_buffer(device, lod.mesh.vertices.data(),
                            lod.mesh.vertices.size() * sizeof(Vec3), k_storage, vertex_buffer,
                            &error) ||
        !gfx::upload_buffer(device, lod.mesh.triangles.data(),
                            lod.mesh.triangles.size() * sizeof(u32), k_storage, triangle_buffer,
                            &error) ||
        !gfx::upload_buffer(device, lod.lod.data(),
                            cluster_count * sizeof(geometry::ClusterLodDesc), k_storage, lod_buffer,
                            &error)) {
      exit_code = fail("upload", error);
      break;
    }
    if (!gfx::create_buffer(device, u64{cluster_count} * sizeof(u32),
                            k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false,
                            visible_buffer, &error) ||
        !gfx::create_buffer(device, sizeof(u32) * 3,
                            k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            false, args_buffer, &error)) {
      exit_code = fail("cull buffers", error);
      break;
    }
    bool buffers_ok = true;
    for (u32 slot = 0; slot < k_frames_in_flight; ++slot) {
      buffers_ok = buffers_ok &&
                   gfx::create_buffer(device, sizeof(gfx::CullParams),
                                      k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true,
                                      params_buffers[slot], &error) &&
                   gfx::create_buffer(device, sizeof(u32) * 3, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      true, stats_buffers[slot], &error);
      if (buffers_ok) std::memset(stats_buffers[slot].mapped, 0, sizeof(u32) * 3);
    }
    if (!buffers_ok) {
      exit_code = fail("per-frame buffers", error);
      break;
    }

    // Shaders: the embedded copies always work; the build's manifest, when found, loads the same
    // shaders from their files and recompiles them when the .slang sources change while running.
    if (!shader_library.create(&device, &error)) {
      exit_code = fail("shaders", error);
      break;
    }
    shader_library.add_embedded("cluster_mesh", shaders::k_cluster_mesh_spirv,
                                shaders::k_cluster_mesh_spirv_size);
    shader_library.add_embedded("cluster_cull", shaders::k_cluster_cull_spirv,
                                shaders::k_cluster_cull_spirv_size);
    std::string manifest = options.shaders;
    if (manifest.empty()) {
      const std::string candidate = platform::executable_directory() + "/../shaders/manifest.json";
      if (io::exists(candidate)) manifest = candidate;
    }
    if (!manifest.empty() && !shader_library.load_manifest(manifest, &error)) {
      exit_code = fail("shaders", error);
      break;
    }
    cluster_shader = shader_library.get("cluster_mesh", &error);
    cull_shader = cluster_shader != nullptr ? shader_library.get("cluster_cull", &error) : nullptr;
    if (cluster_shader == nullptr || cull_shader == nullptr) {
      exit_code = fail("shader", error);
      break;
    }
    if (!manifest.empty()) {
      ENGINE_LOG_INFO(log_view, "shader manifest", log::field("path", manifest),
                      log::field("cluster_mesh_from_file", cluster_shader->from_file));
    }
    gfx::MeshPipelineDesc pipeline_desc;
    pipeline_desc.mesh = cluster_shader->module;
    pipeline_desc.fragment = cluster_shader->module;
    pipeline_desc.fragment_entry = "fs_color";
    pipeline_desc.layout = bindless.pipeline_layout();
    pipeline_desc.color_format = swapchain.format();
    pipeline_desc.depth_format = VK_FORMAT_D32_SFLOAT;
    pipeline_desc.depth_test = true;
    pipeline_desc.depth_write = true;
    if (!gfx::create_mesh_pipeline(device, pipeline_desc, pipeline, &error) ||
        !gfx::create_compute_pipeline(device, cull_shader->module, "cull_main", {}, sizeof(u64),
                                      cull_pipeline, &error)) {
      exit_code = fail("pipeline", error);
      break;
    }
    if (!depth.create(device, swapchain.extent().width, swapchain.extent().height, &error)) {
      exit_code = fail("depth", error);
      break;
    }
    ENGINE_LOG_INFO(log_view, "ready", log::field("clusters", cluster_count),
                    log::field("leaf_clusters", leaf_count),
                    log::field("triangles", lod.leaf_triangle_count),
                    log::field("lod_levels", lod.level_cluster_counts.size()),
                    log::field("build_ms", static_cast<f64>(build_ns) / 1.0e6),
                    log::field("width", swapchain.extent().width),
                    log::field("height", swapchain.extent().height));

    gfx::RenderGraph graph(device);
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

      // Hot reload: recompile edited shaders four times a second and rebuild the pipelines.
      if (time::monotonic_ns() - last_shader_poll_ns > 250'000'000) {
        last_shader_poll_ns = time::monotonic_ns();
        Vector<std::string> changed;
        std::string reload_error;
        shader_library.poll_changes(changed, &reload_error);
        if (!reload_error.empty()) {
          std::fprintf(stderr, "engine-view: shader compile error:\n%s\n", reload_error.c_str());
        }
        for (const std::string& name : changed) {
          if (name != "cluster_mesh" && name != "cluster_cull") continue;
          frames.wait_idle();
          if (name == "cluster_mesh") {
            gfx::destroy_pipeline(device, pipeline);
            pipeline = VK_NULL_HANDLE;
            cluster_shader = shader_library.get("cluster_mesh", &error);
            pipeline_desc.mesh = cluster_shader->module;
            pipeline_desc.fragment = cluster_shader->module;
            if (!gfx::create_mesh_pipeline(device, pipeline_desc, pipeline, &error)) {
              exit_code = fail("pipeline", error);
              running = false;
              break;
            }
          } else {
            gfx::destroy_compute_pipeline(device, cull_pipeline);
            cull_shader = shader_library.get("cluster_cull", &error);
            if (!gfx::create_compute_pipeline(device, cull_shader->module, "cull_main", {},
                                              sizeof(u64), cull_pipeline, &error)) {
              exit_code = fail("cull pipeline", error);
              running = false;
              break;
            }
          }
          ENGINE_LOG_INFO(log_view, "pipeline rebuilt after shader reload",
                          log::field("shader", name));
        }
        if (!running) break;
      }
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
      const u32 slot = frames.slot();
      // The frame that last used this slot has completed: its draw-args copy is readable.
      if (options.cull && rendered >= k_frames_in_flight) {
        visible_last = static_cast<const u32*>(stats_buffers[slot].mapped)[0];
        visible_min = visible_last < visible_min ? visible_last : visible_min;
        visible_max = visible_last > visible_max ? visible_last : visible_max;
      }
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

      // Camera: orbit and breathe between close and far so the LOD cut changes visibly.
      const VkExtent2D extent = swapchain.extent();
      const f32 aspect = static_cast<f32>(extent.width) / static_cast<f32>(extent.height);
      const f32 angle = static_cast<f32>(rendered) * 0.006f;
      const f32 distance = 22.0f + 14.0f * std::sin(static_cast<f32>(rendered) * 0.004f);
      const Vec3 eye{std::cos(angle) * distance, 0.45f * distance, std::sin(angle) * distance};
      const f32 fov_y = radians(55.0f);
      const f32 znear = 0.1f;
      const Mat4 view_proj = perspective_reversed_z(fov_y, aspect, znear) *
                             look_at(eye, Vec3{}, Vec3{0.0f, 1.0f, 0.0f});

      MeshParams params{};
      params.view_proj = view_proj;
      params.clusters = cluster_buffer.address;
      params.vertices = vertex_buffer.address;
      params.triangles = triangle_buffer.address;
      params.cluster_count = options.cull ? cluster_count : leaf_count;
      params.visible = options.cull ? visible_buffer.address : 0;

      gfx::CullParams cull{};
      gfx::set_frustum(cull, frustum_from_view_proj(view_proj));
      cull.camera = Vec4{eye, znear};
      cull.lod = Vec4{1.0f / std::tan(fov_y * 0.5f) * static_cast<f32>(extent.height) * 0.5f,
                      options.lod_px, 1.0f, 1.0f};
      cull.cluster_count = cluster_count;
      cull.clusters = cluster_buffer.address;
      cull.lods = lod_buffer.address;
      cull.visible = visible_buffer.address;
      cull.draw_args = args_buffer.address;
      std::memcpy(params_buffers[slot].mapped, &cull, sizeof(cull));
      const u64 cull_params_address = params_buffers[slot].address;

      graph.reset();
      const gfx::RgImage color = graph.import_image("swapchain", swapchain.image(image_index));
      const gfx::RgImage depth_target = graph.import_image("depth", depth.image);
      const gfx::RgBuffer rg_args = graph.import_buffer("draw_args", args_buffer);
      const gfx::RgBuffer rg_visible = graph.import_buffer("visible", visible_buffer);
      const gfx::RgBuffer rg_stats = graph.import_buffer("stats", stats_buffers[slot]);
      VkClearColorValue sky{};
      sky.float32[0] = 0.55f;
      sky.float32[1] = 0.70f;
      sky.float32[2] = 0.90f;
      sky.float32[3] = 1.0f;
      if (options.cull) {
        graph.add_pass(
            "reset", gfx::PassKind::Transfer,
            [&](gfx::PassBuilder& b) { b.write(rg_args, gfx::Access::TransferWrite); },
            [&](VkCommandBuffer cb, gfx::RenderGraph&) {
              vkCmdFillBuffer(cb, args_buffer.buffer, 0, sizeof(u32), 0);
              vkCmdFillBuffer(cb, args_buffer.buffer, sizeof(u32), sizeof(u32) * 2, 1);
            });
        graph.add_pass(
            "cull", gfx::PassKind::Compute,
            [&](gfx::PassBuilder& b) {
              b.write(rg_args, gfx::Access::ComputeReadWrite);
              b.write(rg_visible, gfx::Access::ComputeWrite);
            },
            [&](VkCommandBuffer cb, gfx::RenderGraph&) {
              vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cull_pipeline.pipeline);
              vkCmdPushConstants(cb, cull_pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                 sizeof(u64), &cull_params_address);
              vkCmdDispatch(cb, gfx::cull_group_count(cluster_count), 1, 1);
            });
      }
      graph.add_pass(
          "terrain", gfx::PassKind::Raster,
          [&](gfx::PassBuilder& b) {
            b.color_attachment(color, VK_ATTACHMENT_LOAD_OP_CLEAR, sky);
            b.depth_attachment(depth_target, VK_ATTACHMENT_LOAD_OP_CLEAR,
                               0.0f);  // reversed Z: far is 0
            if (options.cull) {
              b.read(rg_args, gfx::Access::IndirectRead);
              b.read(rg_visible, gfx::Access::MeshRead);
            }
          },
          [&](VkCommandBuffer cb, gfx::RenderGraph&) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
            vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                               sizeof(params), &params);
            if (options.cull) {
              vkCmdDrawMeshTasksIndirectEXT(cb, args_buffer.buffer, 0, 1, sizeof(u32) * 3);
            } else {
              vkCmdDrawMeshTasksEXT(cb, leaf_count, 1, 1);
            }
          });
      if (options.cull) {
        graph.add_pass(
            "stats", gfx::PassKind::Transfer,
            [&](gfx::PassBuilder& b) {
              b.read(rg_args, gfx::Access::TransferRead);
              b.write(rg_stats, gfx::Access::TransferWrite);
            },
            [&](VkCommandBuffer cb, gfx::RenderGraph&) {
              const VkBufferCopy copy{0, 0, sizeof(u32) * 3};
              vkCmdCopyBuffer(cb, args_buffer.buffer, stats_buffers[slot].buffer, 1, &copy);
            });
      }
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
        if (options.cull) {
          visible_last = static_cast<const u32*>(stats_buffers[slot].mapped)[0];
          visible_min = visible_last < visible_min ? visible_last : visible_min;
          visible_max = visible_last > visible_max ? visible_last : visible_max;
        }
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
  gfx::destroy_compute_pipeline(device, cull_pipeline);
  shader_library.destroy();
  for (u32 slot = 0; slot < k_frames_in_flight; ++slot) {
    gfx::destroy_buffer(device, params_buffers[slot]);
    gfx::destroy_buffer(device, stats_buffers[slot]);
  }
  gfx::destroy_buffer(device, args_buffer);
  gfx::destroy_buffer(device, visible_buffer);
  gfx::destroy_buffer(device, lod_buffer);
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
    if (visible_min == ~u32{0}) visible_min = 0;
    std::printf(
        "{\"frames\":%llu,\"seconds\":%.3f,\"avg_ms\":%.3f,\"width\":%u,\"height\":%u,"
        "\"clusters\":%u,\"leaf_clusters\":%u,\"triangles\":%u,\"lod_levels\":%u,\"build_ms\":%.1f,"
        "\"cull\":%s,\"lod_px\":%.2f,\"visible_last\":%u,\"visible_min\":%u,\"visible_max\":%u,"
        "\"captured\":%s}\n",
        static_cast<unsigned long long>(rendered), seconds, avg_ms, options.width, options.height,
        lod.mesh.clusters.size(),
        lod.level_cluster_counts.empty() ? 0u : lod.level_cluster_counts[0],
        lod.leaf_triangle_count, lod.level_cluster_counts.size(),
        static_cast<f64>(build_ns) / 1.0e6, options.cull ? "true" : "false",
        static_cast<f64>(options.lod_px), visible_last, visible_min, visible_max,
        captured ? "true" : "false");
  }
  log::remove_sink(&stderr_sink);
  return exit_code;
}
