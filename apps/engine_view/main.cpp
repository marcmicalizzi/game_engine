// engine-view: a window on the renderer as it stands. A procedural heightfield is built into
// a cluster LOD DAG (domain/geometry), uploaded behind device addresses, culled and LOD-selected
// on the GPU every frame (cluster_cull.slang), and rasterized into a 64-bit visibility buffer by
// the hardware path (mesh shaders, fs_visibility), the software rasterizer (one compute
// workgroup per small cluster), or both split by projected cluster size (experiment E1); a
// fullscreen resolve turns the buffer into colors. `--raster direct` keeps the plain
// mesh-shader-to-color path with a depth buffer. The camera orbits and zooms so the LOD cut
// changes. `--frames N --capture out.png` renders N frames and writes the last one as a PNG, and
// the process prints one JSON line of statistics (including GPU milliseconds per pass from
// timestamps) on exit, so scripts and agents can look at the picture and the numbers without a
// human at the window. Shaders come from the build's manifest when it is found and recompile
// when their sources change.
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
#include <domain/gfx/gpu_timer.h>
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
#include <shaders/cluster_sw_raster.spv.h>
#include <shaders/visibility_resolve.spv.h>
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
    "                   [--raster direct|hw|sw|auto] [--sw-px <px>] [--view id|tri|depth] [--orbit <d>]\n"
    "\n"
    "  --frames <n>     render n frames, then exit (0: until the window closes)\n"
    "  --capture <png>  write the last frame as a PNG (implies --frames 60 when unset)\n"
    "  --grid <n>       heightfield resolution, n x n vertices (default 257)\n"
    "  --lod <px>       screen-space error threshold in pixels for LOD selection (default 1)\n"
    "  --no-cull        draw every leaf cluster; no GPU culling or LOD selection\n"
    "  --raster <mode>  direct: mesh shaders to color with a depth buffer; hw, sw, auto: the\n"
    "                   visibility buffer through hardware, software, or both split by size (default)\n"
    "  --sw-px <px>     auto mode: clusters narrower than this go to the software rasterizer (32)\n"
    "  --view <mode>    resolve as cluster colors, cluster colors with triangle shading (default),\n"
    "                   or depth\n"
    "  --orbit <d>      orbit at a fixed distance instead of breathing between 8 and 36 units\n"
    "  --log <spec>     log levels, e.g. \"info,gfx=debug\" (stderr shows warnings and up)\n"
    "  --shaders <m>    shader manifest (default: <exe dir>/../shaders/manifest.json when present);\n"
    "                   shaders recompile and reload when their .slang sources change\n"
    "exit codes: 0 ok, 1 error, 2 usage, 3 unavailable (no display, device, mesh shaders)\n";
// clang-format on

constexpr int k_exit_error = 1;
constexpr int k_exit_usage = 2;
constexpr int k_exit_unavailable = 3;
constexpr u32 k_frames_in_flight = 2;

enum class RasterMode : u8 { Direct, Hardware, Software, Auto };

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
  RasterMode raster = RasterMode::Auto;
  f32 sw_px = 32.0f;
  u32 view_mode = 1;
  f32 orbit = 0.0f;  // 0: breathe
};

const char* raster_name(RasterMode mode) {
  switch (mode) {
    case RasterMode::Direct: return "direct";
    case RasterMode::Hardware: return "hw";
    case RasterMode::Software: return "sw";
    case RasterMode::Auto: return "auto";
  }
  return "?";
}

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

// Screen-sized resources recreated on resize.
struct Targets {
  gfx::ImageResource depth;  // direct mode
  gfx::BufferResource vis;   // visibility buffer: u64 per pixel
  u32 width = 0;
  u32 height = 0;
  bool create(const gfx::Device& device, u32 w, u32 h, std::string* error) {
    destroy(device);
    width = w;
    height = h;
    return gfx::create_image_2d(device, w, h, VK_FORMAT_D32_SFLOAT,
                                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, depth, error) &&
           gfx::create_buffer(device, u64{w} * h * sizeof(u64),
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              false, vis, error);
  }
  void destroy(const gfx::Device& device) {
    if (depth.image != VK_NULL_HANDLE) gfx::destroy_image(device, depth);
    gfx::destroy_buffer(device, vis);
    depth = gfx::ImageResource{};
    vis = gfx::BufferResource{};
  }
};

struct Pipelines {
  VkPipeline direct = VK_NULL_HANDLE;    // mesh + fs_color into the swapchain with depth
  VkPipeline hardware = VK_NULL_HANDLE;  // mesh + fs_visibility, no attachments
  gfx::ComputePipeline software;         // cluster_sw_raster
  gfx::ComputePipeline cull;             // cluster_cull
  VkPipeline resolve = VK_NULL_HANDLE;   // fullscreen visibility resolve
  void destroy(const gfx::Device& device) {
    if (direct != VK_NULL_HANDLE) gfx::destroy_pipeline(device, direct);
    if (hardware != VK_NULL_HANDLE) gfx::destroy_pipeline(device, hardware);
    if (resolve != VK_NULL_HANDLE) gfx::destroy_pipeline(device, resolve);
    gfx::destroy_compute_pipeline(device, software);
    gfx::destroy_compute_pipeline(device, cull);
    direct = hardware = resolve = VK_NULL_HANDLE;
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
    } else if (a == "--lod" || a == "--sw-px" || a == "--orbit") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      f32 px = 0.0f;
      if (!parse_f32(value, px)) {
        std::fprintf(stderr, "engine-view: %.*s expects a positive number\n",
                     static_cast<int>(a.size()), a.data());
        return k_exit_usage;
      }
      (a == "--lod" ? options.lod_px : a == "--sw-px" ? options.sw_px : options.orbit) = px;
    } else if (a == "--raster") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      if (value == "direct") {
        options.raster = RasterMode::Direct;
      } else if (value == "hw") {
        options.raster = RasterMode::Hardware;
      } else if (value == "sw") {
        options.raster = RasterMode::Software;
      } else if (value == "auto") {
        options.raster = RasterMode::Auto;
      } else {
        std::fprintf(stderr, "engine-view: --raster expects direct, hw, sw, or auto\n");
        return k_exit_usage;
      }
    } else if (a == "--view") {
      if (!next_value(argc, argv, i, a, value)) return k_exit_usage;
      if (value == "id") {
        options.view_mode = 0;
      } else if (value == "tri") {
        options.view_mode = 1;
      } else if (value == "depth") {
        options.view_mode = 2;
      } else {
        std::fprintf(stderr, "engine-view: --view expects id, tri, or depth\n");
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
  if (!options.cull && options.raster != RasterMode::Direct) {
    // Without the cull pass there are no visible lists; every leaf goes through one path.
    if (options.raster == RasterMode::Auto) options.raster = RasterMode::Hardware;
  }

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
  const bool visibility_ok = device.features().buffer_int64_atomics;
  if (!device.features().mesh_shader || !device.features().presentation ||
      (!visibility_ok && options.raster != RasterMode::Direct)) {
    const std::string why = std::string(device.adapter().name) +
                            (!device.features().mesh_shader    ? " has no mesh shaders"
                             : !device.features().presentation ? " cannot present"
                                                               : " has no 64-bit buffer atomics");
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
  gfx::GpuTimer timer;
  gfx::BufferResource cluster_buffer;
  gfx::BufferResource vertex_buffer;
  gfx::BufferResource triangle_buffer;
  gfx::BufferResource lod_buffer;
  gfx::BufferResource visible_buffer;     // hardware survivors
  gfx::BufferResource sw_visible_buffer;  // software survivors
  gfx::BufferResource args_buffer;        // {count, 1, 1} for the indirect mesh draw
  gfx::BufferResource sw_args_buffer;     // {count, 1, 1} for the indirect dispatch
  gfx::BufferResource params_buffers[k_frames_in_flight];  // host-visible CullParams per slot
  gfx::BufferResource stats_buffers[k_frames_in_flight];   // host-visible copies of both arg blocks
  gfx::ShaderLibrary shader_library;
  Pipelines pipelines;
  Targets targets;
  geometry::ClusterLodMesh lod;
  u64 rendered = 0;
  i64 started_ns = 0;
  i64 finished_ns = 0;
  bool captured = false;
  u32 visible_hw_last = 0;
  u32 visible_sw_last = 0;
  u32 visible_min = ~u32{0};
  u32 visible_max = 0;
  i64 build_ns = 0;
  f64 gpu_cull_ms = 0.0;
  f64 gpu_hw_ms = 0.0;
  f64 gpu_sw_ms = 0.0;
  f64 gpu_resolve_ms = 0.0;
  f64 gpu_total_ms = 0.0;
  u64 timed_frames = 0;
  u32 extent_width = options.width;
  u32 extent_height = options.height;

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
        !bindless.create(device, gfx::BindlessConfig{}, &error) ||
        !timer.create(device, k_frames_in_flight, 8, &error)) {
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
    constexpr VkBufferUsageFlags k_args = k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                          VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
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
        !gfx::create_buffer(device, u64{cluster_count} * sizeof(u32),
                            k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false,
                            sw_visible_buffer, &error) ||
        !gfx::create_buffer(device, sizeof(u32) * 3, k_args, false, args_buffer, &error) ||
        !gfx::create_buffer(device, sizeof(u32) * 3, k_args, false, sw_args_buffer, &error)) {
      exit_code = fail("cull buffers", error);
      break;
    }
    bool buffers_ok = true;
    for (u32 slot = 0; slot < k_frames_in_flight; ++slot) {
      buffers_ok = buffers_ok &&
                   gfx::create_buffer(device, sizeof(gfx::CullParams),
                                      k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true,
                                      params_buffers[slot], &error) &&
                   gfx::create_buffer(device, sizeof(u32) * 6, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      true, stats_buffers[slot], &error);
      if (buffers_ok) std::memset(stats_buffers[slot].mapped, 0, sizeof(u32) * 6);
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
    shader_library.add_embedded("cluster_sw_raster", shaders::k_cluster_sw_raster_spirv,
                                shaders::k_cluster_sw_raster_spirv_size);
    shader_library.add_embedded("visibility_resolve", shaders::k_visibility_resolve_spirv,
                                shaders::k_visibility_resolve_spirv_size);
    std::string manifest = options.shaders;
    if (manifest.empty()) {
      const std::string candidate = platform::executable_directory() + "/../shaders/manifest.json";
      if (io::exists(candidate)) manifest = candidate;
    }
    if (!manifest.empty() && !shader_library.load_manifest(manifest, &error)) {
      exit_code = fail("shaders", error);
      break;
    }
    if (!manifest.empty())
      ENGINE_LOG_INFO(log_view, "shader manifest", log::field("path", manifest));

    auto create_pipelines = [&](std::string* err) {
      const gfx::Shader* mesh = shader_library.get("cluster_mesh", err);
      const gfx::Shader* cull = mesh != nullptr ? shader_library.get("cluster_cull", err) : nullptr;
      const gfx::Shader* sw =
          cull != nullptr ? shader_library.get("cluster_sw_raster", err) : nullptr;
      const gfx::Shader* resolve =
          sw != nullptr ? shader_library.get("visibility_resolve", err) : nullptr;
      if (resolve == nullptr) return false;
      gfx::MeshPipelineDesc direct_desc;
      direct_desc.mesh = mesh->module;
      direct_desc.fragment = mesh->module;
      direct_desc.fragment_entry = "fs_color";
      direct_desc.layout = bindless.pipeline_layout();
      direct_desc.color_format = swapchain.format();
      direct_desc.depth_format = VK_FORMAT_D32_SFLOAT;
      direct_desc.depth_test = true;
      direct_desc.depth_write = true;
      gfx::MeshPipelineDesc hw_desc;
      hw_desc.mesh = mesh->module;
      hw_desc.fragment = mesh->module;
      hw_desc.fragment_entry = "fs_visibility";
      hw_desc.layout = bindless.pipeline_layout();
      gfx::GraphicsPipelineDesc resolve_desc;
      resolve_desc.vertex = resolve->module;
      resolve_desc.vertex_entry = "vs_fullscreen";
      resolve_desc.fragment = resolve->module;
      resolve_desc.fragment_entry = "fs_resolve";
      resolve_desc.layout = bindless.pipeline_layout();
      resolve_desc.color_format = swapchain.format();
      return gfx::create_mesh_pipeline(device, direct_desc, pipelines.direct, err) &&
             gfx::create_mesh_pipeline(device, hw_desc, pipelines.hardware, err) &&
             gfx::create_compute_pipeline(device, sw->module, "sw_raster_main", {},
                                          sizeof(gfx::ClusterDrawParams), pipelines.software,
                                          err) &&
             gfx::create_compute_pipeline(device, cull->module, "cull_main", {}, sizeof(u64),
                                          pipelines.cull, err) &&
             gfx::create_graphics_pipeline(device, resolve_desc, pipelines.resolve, err);
    };
    if (!create_pipelines(&error)) {
      exit_code = fail("pipelines", error);
      break;
    }
    if (!targets.create(device, swapchain.extent().width, swapchain.extent().height, &error)) {
      exit_code = fail("targets", error);
      break;
    }
    ENGINE_LOG_INFO(log_view, "ready", log::field("clusters", cluster_count),
                    log::field("leaf_clusters", leaf_count),
                    log::field("triangles", lod.leaf_triangle_count),
                    log::field("lod_levels", lod.level_cluster_counts.size()),
                    log::field("build_ms", static_cast<f64>(build_ns) / 1.0e6),
                    log::field("raster", raster_name(options.raster)),
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

      // Hot reload: recompile edited shaders four times a second and rebuild every pipeline.
      if (time::monotonic_ns() - last_shader_poll_ns > 250'000'000) {
        last_shader_poll_ns = time::monotonic_ns();
        Vector<std::string> changed;
        std::string reload_error;
        shader_library.poll_changes(changed, &reload_error);
        if (!reload_error.empty()) {
          std::fprintf(stderr, "engine-view: shader compile error:\n%s\n", reload_error.c_str());
        }
        if (!changed.empty()) {
          frames.wait_idle();
          pipelines.destroy(device);
          if (!create_pipelines(&error)) {
            exit_code = fail("pipelines", error);
            break;
          }
          for (const std::string& name : changed) {
            ENGINE_LOG_INFO(log_view, "pipelines rebuilt after shader reload",
                            log::field("shader", name));
          }
        }
      }
      if (resize_pending) {
        resize_pending = false;
        if (!swapchain.resize(window.pixel_width(), window.pixel_height(), &error)) {
          exit_code = fail("resize", error);
          break;
        }
        if (window.pixel_width() > 0 && window.pixel_height() > 0) {
          frames.wait_idle();
          if (!targets.create(device, swapchain.extent().width, swapchain.extent().height,
                              &error)) {
            exit_code = fail("targets", error);
            break;
          }
        }
      }

      VkCommandBuffer commands = frames.begin_frame();
      const u32 slot = frames.slot();
      timer.begin_frame(commands, slot);
      if (rendered >= k_frames_in_flight) {
        // The frame that last used this slot has completed: its statistics are readable.
        if (options.cull) {
          const auto* stats = static_cast<const u32*>(stats_buffers[slot].mapped);
          visible_hw_last = stats[0];
          visible_sw_last = stats[3];
          const u32 total = visible_hw_last + visible_sw_last;
          visible_min = total < visible_min ? total : visible_min;
          visible_max = total > visible_max ? total : visible_max;
        }
        if (!timer.results().empty()) {
          gpu_cull_ms += timer.ms("cull");
          gpu_hw_ms += timer.ms("hw");
          gpu_sw_ms += timer.ms("sw");
          gpu_resolve_ms += timer.ms("resolve");
          gpu_total_ms += timer.total_ms();
          ++timed_frames;
        }
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
      extent_width = extent.width;
      extent_height = extent.height;
      const f32 aspect = static_cast<f32>(extent.width) / static_cast<f32>(extent.height);
      const f32 angle = static_cast<f32>(rendered) * 0.006f;
      const f32 distance = options.orbit > 0.0f
                               ? options.orbit
                               : 22.0f + 14.0f * std::sin(static_cast<f32>(rendered) * 0.004f);
      const Vec3 eye{std::cos(angle) * distance, 0.45f * distance, std::sin(angle) * distance};
      const f32 fov_y = radians(55.0f);
      const f32 znear = 0.1f;
      const Mat4 view_proj = perspective_reversed_z(fov_y, aspect, znear) *
                             look_at(eye, Vec3{}, Vec3{0.0f, 1.0f, 0.0f});
      const f32 proj_scale = 1.0f / std::tan(fov_y * 0.5f) * static_cast<f32>(extent.height) * 0.5f;
      const bool direct = options.raster == RasterMode::Direct;
      const bool use_hw = options.raster != RasterMode::Software;
      const bool use_sw = !direct && options.raster != RasterMode::Hardware && options.cull;

      gfx::ClusterDrawParams draw{};
      draw.view_proj = view_proj;
      draw.clusters = cluster_buffer.address;
      draw.vertices = vertex_buffer.address;
      draw.triangles = triangle_buffer.address;
      draw.cluster_count = options.cull ? cluster_count : leaf_count;
      draw.visible = options.cull ? visible_buffer.address : 0;
      draw.visibility = targets.vis.address;
      draw.width = extent.width;
      draw.height = extent.height;
      gfx::ClusterDrawParams draw_sw = draw;
      draw_sw.visible = sw_visible_buffer.address;

      gfx::CullParams cull{};
      gfx::set_frustum(cull, frustum_from_view_proj(view_proj));
      cull.camera = Vec4{eye, znear};
      cull.lod = Vec4{proj_scale, options.lod_px, 1.0f, 1.0f};
      const f32 raster_mode = direct || options.raster == RasterMode::Hardware
                                  ? gfx::k_raster_hardware
                              : options.raster == RasterMode::Software ? gfx::k_raster_software
                                                                       : gfx::k_raster_split;
      cull.raster = Vec4{options.sw_px, raster_mode, 0.0f, 0.0f};
      cull.cluster_count = cluster_count;
      cull.clusters = cluster_buffer.address;
      cull.lods = lod_buffer.address;
      cull.visible = visible_buffer.address;
      cull.draw_args = args_buffer.address;
      cull.sw_visible = sw_visible_buffer.address;
      cull.sw_args = sw_args_buffer.address;
      std::memcpy(params_buffers[slot].mapped, &cull, sizeof(cull));
      const u64 cull_params_address = params_buffers[slot].address;

      gfx::ResolveParams resolve{};
      resolve.sky = Vec4{0.55f, 0.70f, 0.90f, 1.0f};
      resolve.visibility = targets.vis.address;
      resolve.width = extent.width;
      resolve.height = extent.height;
      resolve.mode = options.view_mode;

      graph.reset();
      const gfx::RgImage color = graph.import_image("swapchain", swapchain.image(image_index));
      const gfx::RgImage depth_target = graph.import_image("depth", targets.depth);
      const gfx::RgBuffer rg_args = graph.import_buffer("draw_args", args_buffer);
      const gfx::RgBuffer rg_sw_args = graph.import_buffer("sw_args", sw_args_buffer);
      const gfx::RgBuffer rg_visible = graph.import_buffer("visible", visible_buffer);
      const gfx::RgBuffer rg_sw_visible = graph.import_buffer("sw_visible", sw_visible_buffer);
      const gfx::RgBuffer rg_vis = graph.import_buffer("visibility", targets.vis);
      const gfx::RgBuffer rg_stats = graph.import_buffer("stats", stats_buffers[slot]);
      VkClearColorValue sky{};
      sky.float32[0] = 0.55f;
      sky.float32[1] = 0.70f;
      sky.float32[2] = 0.90f;
      sky.float32[3] = 1.0f;

      graph.add_pass(
          "reset", gfx::PassKind::Transfer,
          [&](gfx::PassBuilder& b) {
            if (options.cull) {
              b.write(rg_args, gfx::Access::TransferWrite);
              b.write(rg_sw_args, gfx::Access::TransferWrite);
            }
            if (!direct) b.write(rg_vis, gfx::Access::TransferWrite);
          },
          [&](VkCommandBuffer cb, gfx::RenderGraph&) {
            if (options.cull) {
              vkCmdFillBuffer(cb, args_buffer.buffer, 0, sizeof(u32), 0);
              vkCmdFillBuffer(cb, args_buffer.buffer, sizeof(u32), sizeof(u32) * 2, 1);
              vkCmdFillBuffer(cb, sw_args_buffer.buffer, 0, sizeof(u32), 0);
              vkCmdFillBuffer(cb, sw_args_buffer.buffer, sizeof(u32), sizeof(u32) * 2, 1);
            }
            if (!direct) vkCmdFillBuffer(cb, targets.vis.buffer, 0, VK_WHOLE_SIZE, 0);
          });
      if (options.cull) {
        graph.add_pass(
            "cull", gfx::PassKind::Compute,
            [&](gfx::PassBuilder& b) {
              b.write(rg_args, gfx::Access::ComputeReadWrite);
              b.write(rg_sw_args, gfx::Access::ComputeReadWrite);
              b.write(rg_visible, gfx::Access::ComputeWrite);
              b.write(rg_sw_visible, gfx::Access::ComputeWrite);
            },
            [&](VkCommandBuffer cb, gfx::RenderGraph&) {
              timer.begin(cb, "cull");
              vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.cull.pipeline);
              vkCmdPushConstants(cb, pipelines.cull.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                 sizeof(u64), &cull_params_address);
              vkCmdDispatch(cb, gfx::cull_group_count(cluster_count), 1, 1);
              timer.end(cb);
            });
      }
      if (direct) {
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
              timer.begin(cb, "hw");
              vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.direct);
              bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
              vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                                 sizeof(draw), &draw);
              if (options.cull) {
                vkCmdDrawMeshTasksIndirectEXT(cb, args_buffer.buffer, 0, 1, sizeof(u32) * 3);
              } else {
                vkCmdDrawMeshTasksEXT(cb, leaf_count, 1, 1);
              }
              timer.end(cb);
            });
      } else {
        if (use_hw) {
          graph.add_pass(
              "hardware", gfx::PassKind::Raster,
              [&](gfx::PassBuilder& b) {
                b.render_area(extent.width, extent.height);
                b.write(rg_vis, gfx::Access::FragmentReadWrite);
                if (options.cull) {
                  b.read(rg_args, gfx::Access::IndirectRead);
                  b.read(rg_visible, gfx::Access::MeshRead);
                }
              },
              [&](VkCommandBuffer cb, gfx::RenderGraph&) {
                timer.begin(cb, "hw");
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.hardware);
                bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
                vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                                   sizeof(draw), &draw);
                if (options.cull) {
                  vkCmdDrawMeshTasksIndirectEXT(cb, args_buffer.buffer, 0, 1, sizeof(u32) * 3);
                } else {
                  vkCmdDrawMeshTasksEXT(cb, leaf_count, 1, 1);
                }
                timer.end(cb);
              });
        }
        if (use_sw) {
          graph.add_pass(
              "software", gfx::PassKind::Compute,
              [&](gfx::PassBuilder& b) {
                b.write(rg_vis, gfx::Access::ComputeReadWrite);
                b.read(rg_sw_args, gfx::Access::IndirectRead);
                b.read(rg_sw_visible, gfx::Access::ComputeRead);
              },
              [&](VkCommandBuffer cb, gfx::RenderGraph&) {
                timer.begin(cb, "sw");
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.software.pipeline);
                vkCmdPushConstants(cb, pipelines.software.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(draw_sw), &draw_sw);
                vkCmdDispatchIndirect(cb, sw_args_buffer.buffer, 0);
                timer.end(cb);
              });
        }
        graph.add_pass(
            "resolve", gfx::PassKind::Raster,
            [&](gfx::PassBuilder& b) {
              b.color_attachment(color, VK_ATTACHMENT_LOAD_OP_CLEAR, sky);
              b.read(rg_vis, gfx::Access::FragmentRead);
            },
            [&](VkCommandBuffer cb, gfx::RenderGraph&) {
              timer.begin(cb, "resolve");
              vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.resolve);
              bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
              vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                                 sizeof(resolve), &resolve);
              vkCmdDraw(cb, 3, 1, 0, 0);
              timer.end(cb);
            });
      }
      if (options.cull) {
        graph.add_pass(
            "stats", gfx::PassKind::Transfer,
            [&](gfx::PassBuilder& b) {
              b.read(rg_args, gfx::Access::TransferRead);
              b.read(rg_sw_args, gfx::Access::TransferRead);
              b.write(rg_stats, gfx::Access::TransferWrite);
            },
            [&](VkCommandBuffer cb, gfx::RenderGraph&) {
              const VkBufferCopy hw_copy{0, 0, sizeof(u32) * 3};
              vkCmdCopyBuffer(cb, args_buffer.buffer, stats_buffers[slot].buffer, 1, &hw_copy);
              const VkBufferCopy sw_copy{0, sizeof(u32) * 3, sizeof(u32) * 3};
              vkCmdCopyBuffer(cb, sw_args_buffer.buffer, stats_buffers[slot].buffer, 1, &sw_copy);
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
          const auto* stats = static_cast<const u32*>(stats_buffers[slot].mapped);
          visible_hw_last = stats[0];
          visible_sw_last = stats[3];
          const u32 total = visible_hw_last + visible_sw_last;
          visible_min = total < visible_min ? total : visible_min;
          visible_max = total > visible_max ? total : visible_max;
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
  targets.destroy(device);
  pipelines.destroy(device);
  shader_library.destroy();
  for (u32 slot = 0; slot < k_frames_in_flight; ++slot) {
    gfx::destroy_buffer(device, params_buffers[slot]);
    gfx::destroy_buffer(device, stats_buffers[slot]);
  }
  gfx::destroy_buffer(device, sw_args_buffer);
  gfx::destroy_buffer(device, args_buffer);
  gfx::destroy_buffer(device, sw_visible_buffer);
  gfx::destroy_buffer(device, visible_buffer);
  gfx::destroy_buffer(device, lod_buffer);
  gfx::destroy_buffer(device, triangle_buffer);
  gfx::destroy_buffer(device, vertex_buffer);
  gfx::destroy_buffer(device, cluster_buffer);
  timer.destroy();
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
    const f64 n = timed_frames > 0 ? static_cast<f64>(timed_frames) : 1.0;
    std::printf(
        "{\"frames\":%llu,\"seconds\":%.3f,\"avg_ms\":%.3f,\"width\":%u,\"height\":%u,"
        "\"clusters\":%u,\"leaf_clusters\":%u,\"triangles\":%u,\"lod_levels\":%u,\"build_ms\":%.1f,"
        "\"cull\":%s,\"lod_px\":%.2f,\"raster\":\"%s\",\"sw_px\":%.1f,"
        "\"visible_hw_last\":%u,\"visible_sw_last\":%u,\"visible_min\":%u,\"visible_max\":%u,"
        "\"gpu_ms\":{\"cull\":%.4f,\"hw\":%.4f,\"sw\":%.4f,\"resolve\":%.4f,\"total\":%.4f,"
        "\"frames\":%llu},"
        "\"captured\":%s}\n",
        static_cast<unsigned long long>(rendered), seconds, avg_ms, extent_width, extent_height,
        lod.mesh.clusters.size(),
        lod.level_cluster_counts.empty() ? 0u : lod.level_cluster_counts[0],
        lod.leaf_triangle_count, lod.level_cluster_counts.size(),
        static_cast<f64>(build_ns) / 1.0e6, options.cull ? "true" : "false",
        static_cast<f64>(options.lod_px), raster_name(options.raster),
        static_cast<f64>(options.sw_px), visible_hw_last, visible_sw_last, visible_min, visible_max,
        gpu_cull_ms / n, gpu_hw_ms / n, gpu_sw_ms / n, gpu_resolve_ms / n, gpu_total_ms / n,
        static_cast<unsigned long long>(timed_frames), captured ? "true" : "false");
  }
  log::remove_sink(&stderr_sink);
  return exit_code;
}
