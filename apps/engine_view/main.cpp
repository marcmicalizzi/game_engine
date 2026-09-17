// engine-view: a window on the renderer as it stands. A procedural heightfield is built into
// a cluster LOD DAG (domain/geometry), uploaded behind device addresses, culled and LOD-selected
// on the GPU every frame (cluster_cull.slang), and rasterized into a 64-bit visibility buffer by
// the hardware path (mesh shaders, fs_visibility), the software rasterizer (one compute
// workgroup per small cluster), or both split by projected cluster size (experiment E1); a
// fullscreen resolve turns the buffer into colors. In the hardware mode two-pass occlusion
// culling runs against a Hi-Z pyramid of the visibility buffer's depth. `--raster direct` keeps
// the plain mesh-shader-to-color path with a depth buffer. The camera orbits and zooms so the LOD
// cut changes. `--frames N --capture out.png` renders N frames and writes the last one as a PNG,
// and the process prints one JSON line of statistics (including GPU milliseconds per pass from
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
#include <domain/assets/gltf.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/gfx/acceleration.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/capture.h>
#include <domain/gfx/cluster_acceleration.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/gpu_timer.h>
#include <domain/gfx/ray_visibility.h>
#include <domain/gfx/render_graph.h>
#include <domain/gfx/shader_library.h>
#include <domain/gfx/swapchain.h>
#include <domain/gfx/visibility_resolve.h>
#include <domain/gfx/vulkan.h>
#include <foundation/image/decode.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>
#include <foundation/window/window.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <shaders/clas_records.spv.h>
#include <shaders/cluster_cull.spv.h>
#include <shaders/cluster_mesh.spv.h>
#include <shaders/cluster_sw_raster.spv.h>
#include <shaders/cluster_vertex.spv.h>
#include <shaders/hiz_build.spv.h>
#include <shaders/ray_visibility.spv.h>
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
    "                   [--shaders <manifest.json>] [--lod <px>] [--no-cull] [--no-occlusion] [--no-cone]\n"
    "                   [--raster direct|hw|vertex|sw|auto|rt] [--sw-px <px>] [--view <mode>] [--orbit <d>]\n"
    "                   [--mesh <file.gltf|file.glb>]\n"
    "\n"
    "  --frames <n>     render n frames, then exit (0: until the window closes)\n"
    "  --capture <png>  write the last frame as a PNG (implies --frames 60 when unset)\n"
    "  --grid <n>       heightfield resolution, n x n vertices (default 257)\n"
    "  --mesh <file>    render a glTF 2.0 file instead of the heightfield: one cluster DAG per\n"
    "                   primitive, materials and base-color textures from the file\n"
    "  --lod <px>       screen-space error threshold in pixels for LOD selection (default 1)\n"
    "  --no-cull        draw every leaf cluster; no GPU culling or LOD selection\n"
    "  --no-occlusion   skip two-pass occlusion culling (hw mode only; on by default)\n"
    "  --no-cone        skip backface culling of clusters by their normal cones (on by default)\n"
    "  --raster <mode>  direct: mesh shaders to color with a depth buffer; hw (default), vertex, sw,\n"
    "                   auto: the visibility buffer through mesh shaders, a vertex shader (the\n"
    "                   baseline tier, chosen automatically without mesh shaders), software, or\n"
    "                   both split by size; rt: ray queries against cluster acceleration structures\n"
    "                   built every frame from the cull output (NVIDIA RTX only)\n"
    "  --sw-px <px>     auto mode: clusters narrower than this go to the software rasterizer (32)\n"
    "  --view <mode>    id, tri, depth, shaded (default: materials with vertex normals and textures under\n"
    "                   a sun), normals, uv\n"
    "  --orbit <d>      orbit at a fixed distance instead of breathing between 8 and 36 units;\n"
    "                   distances scale with the scene radius (10 for the heightfield)\n"
    "  --log <spec>     log levels, e.g. \"info,gfx=debug\" (stderr shows warnings and up)\n"
    "  --shaders <m>    shader manifest (default: <exe dir>/../shaders/manifest.json when present);\n"
    "                   shaders recompile and reload when their .slang sources change\n"
    "exit codes: 0 ok, 1 error, 2 usage, 3 unavailable (no display, device, mesh shaders)\n";
// clang-format on

constexpr int k_exit_error = 1;
constexpr int k_exit_usage = 2;
constexpr int k_exit_unavailable = 3;
constexpr u32 k_frames_in_flight = 2;

enum class RasterMode : u8 { Direct, Hardware, Software, Auto, Vertex, RayTrace };

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
  std::string mesh;  // glTF file; empty renders the heightfield
  f32 lod_px = 1.0f;
  bool cull = true;
  bool occlusion = true;
  bool cone = true;
  RasterMode raster = RasterMode::Hardware;
  f32 sw_px = 32.0f;
  u32 view_mode = static_cast<u32>(gfx::ResolveMode::Shaded);
  f32 orbit = 0.0f;  // 0: breathe
};

const char* raster_name(RasterMode mode) {
  switch (mode) {
    case RasterMode::Direct: return "direct";
    case RasterMode::Hardware: return "hw";
    case RasterMode::Software: return "sw";
    case RasterMode::Auto: return "auto";
    case RasterMode::Vertex: return "vertex";
    case RasterMode::RayTrace: return "rt";
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
  gfx::BufferResource hiz;   // Hi-Z pyramid of the farthest depth (f32 per texel, all mips)
  u32 width = 0;
  u32 height = 0;
  u32 hiz_mips = 0;
  u32 hiz_offsets[gfx::k_hiz_max_mips] = {};
  Vector<gfx::HizParams> hiz_levels;  // 2 x hiz_mips: stable storage for pass bodies
  bool hiz_dirty = true;              // filled with zero (far) before its first use
  bool create(const gfx::Device& device, u32 w, u32 h, std::string* error) {
    destroy(device);
    width = w;
    height = h;
    hiz_mips = gfx::hiz_mip_count(w, h);
    const u32 hiz_elements = gfx::hiz_layout(w, h, hiz_offsets);
    hiz_levels.resize(hiz_mips * 2);
    hiz_dirty = true;
    constexpr VkBufferUsageFlags k_buffer_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                  VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return gfx::create_image_2d(device, w, h, VK_FORMAT_D32_SFLOAT,
                                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, depth, error) &&
           gfx::create_buffer(device, u64{w} * h * sizeof(u64), k_buffer_usage, false, vis,
                              error) &&
           gfx::create_buffer(device, u64{hiz_elements} * sizeof(f32), k_buffer_usage, false, hiz,
                              error);
  }
  void destroy(const gfx::Device& device) {
    if (depth.image != VK_NULL_HANDLE) gfx::destroy_image(device, depth);
    gfx::destroy_buffer(device, vis);
    gfx::destroy_buffer(device, hiz);
    depth = gfx::ImageResource{};
    vis = gfx::BufferResource{};
    hiz = gfx::BufferResource{};
  }
};

struct Pipelines {
  VkPipeline direct = VK_NULL_HANDLE;    // mesh + fs_color into the swapchain with depth
  VkPipeline hardware = VK_NULL_HANDLE;  // mesh + fs_visibility, no attachments
  VkPipeline vertex = VK_NULL_HANDLE;    // vertex shader + fs_visibility: the baseline tier
  gfx::ComputePipeline software;         // cluster_sw_raster
  gfx::ComputePipeline cull;             // cluster_cull
  gfx::ComputePipeline hiz;              // hiz_build
  gfx::ComputePipeline records;          // clas_records (--raster rt)
  gfx::ComputePipeline trace;            // ray_visibility (--raster rt)
  VkPipeline resolve = VK_NULL_HANDLE;   // fullscreen visibility resolve
  void destroy(const gfx::Device& device) {
    if (direct != VK_NULL_HANDLE) gfx::destroy_pipeline(device, direct);
    if (hardware != VK_NULL_HANDLE) gfx::destroy_pipeline(device, hardware);
    if (vertex != VK_NULL_HANDLE) gfx::destroy_pipeline(device, vertex);
    if (resolve != VK_NULL_HANDLE) gfx::destroy_pipeline(device, resolve);
    gfx::destroy_compute_pipeline(device, software);
    gfx::destroy_compute_pipeline(device, cull);
    gfx::destroy_compute_pipeline(device, hiz);
    gfx::destroy_compute_pipeline(device, records);
    gfx::destroy_compute_pipeline(device, trace);
    direct = hardware = vertex = resolve = VK_NULL_HANDLE;
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
      } else if (value == "vertex") {
        options.raster = RasterMode::Vertex;
      } else if (value == "rt") {
        options.raster = RasterMode::RayTrace;
      } else {
        std::fprintf(stderr, "engine-view: --raster expects direct, hw, vertex, sw, auto, or rt\n");
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
      } else if (value == "shaded") {
        options.view_mode = 3;
      } else if (value == "normals") {
        options.view_mode = 4;
      } else if (value == "uv") {
        options.view_mode = 5;
      } else {
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
    } else if (a == "--no-vsync") {
      options.vsync = false;
    } else if (a == "--no-cull") {
      options.cull = false;
    } else if (a == "--no-occlusion") {
      options.occlusion = false;
    } else if (a == "--no-cone") {
      options.cone = false;
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
  if (!options.cull && options.raster == RasterMode::Auto) options.raster = RasterMode::Hardware;
  // Occlusion culling runs on the hardware visibility path only.
  bool occlusion = options.occlusion && options.cull && options.raster == RasterMode::Hardware;

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
  if (!device.features().mesh_shader &&
      (options.raster == RasterMode::Direct || options.raster == RasterMode::Hardware ||
       options.raster == RasterMode::Auto)) {
    options.raster = RasterMode::Vertex;  // the baseline tier
  }
  occlusion = options.occlusion && options.cull &&
              (options.raster == RasterMode::Hardware || options.raster == RasterMode::Vertex);
  const bool vertex_path = options.raster == RasterMode::Vertex;
  const bool ray_path = options.raster == RasterMode::RayTrace;
  if (ray_path && !options.cull) {
    options.cull = true;  // the ray tracing geometry is built from the cull output
    ENGINE_LOG_WARN(log_view, "--no-cull ignored with --raster rt");
  }
  const bool ray_ok = !ray_path || (device.features().cluster_acceleration_structure &&
                                    device.features().ray_query);
  if (!device.features().presentation || !visibility_ok || !ray_ok) {
    const std::string why =
        std::string(device.adapter().name) +
        (!device.features().presentation ? " cannot present"
         : !visibility_ok ? " has no 64-bit buffer atomics"
                          : " has no cluster acceleration structures or ray queries");
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
  gfx::BufferResource vertex_buffer;     // float positions; only --raster rt needs them
  gfx::BufferResource quantized_buffer;  // three u16 per vertex on the mesh-wide grid
  gfx::BufferResource mesh_buffer;       // one gfx::MeshDesc pointing at it
  gfx::BufferResource triangle_buffer;
  gfx::BufferResource lod_buffer;
  gfx::BufferResource attribute_buffer;  // VertexAttributes parallel to the vertices
  gfx::ImageResource texture;            // procedural albedo texture in the bindless set
  VkImageView texture_view = VK_NULL_HANDLE;
  VkSampler texture_sampler = VK_NULL_HANDLE;
  Vector<gfx::ImageResource> mesh_textures;  // --mesh: decoded base-color textures
  Vector<VkImageView> mesh_texture_views;
  u32 mesh_primitives = 0;
  Vec3 scene_center{};
  f32 scene_radius = 10.0f;             // the heightfield's half extent; a mesh's bounding radius
  gfx::BufferResource material_buffer;  // ResolveMaterial table
  gfx::BufferResource cluster_material_buffer;              // material index per cluster
  gfx::BufferResource resolve_buffers[k_frames_in_flight];  // host-visible ResolveParams per slot
  gfx::BufferResource visible_buffer[2];                    // hardware survivors of pass 1 / pass 2
  gfx::BufferResource args_buffer[2];     // {count, 1, 1} for the indirect mesh draws
  gfx::BufferResource sw_visible_buffer;  // software survivors
  gfx::BufferResource sw_args_buffer;     // {count, 1, 1} for the indirect dispatch
  gfx::BufferResource flags_buffer[2];    // drawn last frame / this frame, ping-pong
  gfx::BufferResource params_buffers[k_frames_in_flight];  // host-visible: two CullParams per slot
  gfx::BufferResource stats_buffers[k_frames_in_flight];   // host-visible copies of the arg blocks
  // --raster rt: the frame's cut as cluster acceleration structures.
  gfx::BufferResource indices8_buffer;      // 8-bit packed cluster indices for the CLAS builds
  gfx::BufferResource records_buffer;       // CLAS build records written from the cull output
  gfx::BufferResource record_count_buffer;  // u32: how many
  gfx::BufferResource rt_instances;         // the one top-level instance record
  gfx::BufferResource rt_scratch;
  gfx::BufferResource ray_params[k_frames_in_flight];  // host-visible RayVisibilityParams per slot
  gfx::ClusterSet clas_set;
  gfx::ClusterBlas cluster_blas;
  gfx::AccelerationStructure tlas;
  u32 tlas_slot = gfx::BindlessSet::k_invalid_slot;
  gfx::ShaderLibrary shader_library;
  Pipelines pipelines;
  Targets targets;
  geometry::ClusterLodMesh lod;
  u64 rendered = 0;
  i64 started_ns = 0;
  i64 finished_ns = 0;
  bool captured = false;
  bool flags_dirty = true;
  u32 visible_hw_last = 0;
  u32 visible_pass2_last = 0;
  u32 visible_sw_last = 0;
  u32 visible_min = ~u32{0};
  u32 visible_max = 0;
  i64 build_ns = 0;
  f64 gpu_cull_ms = 0.0;
  f64 gpu_hw_ms = 0.0;
  f64 gpu_sw_ms = 0.0;
  f64 gpu_hiz_ms = 0.0;
  f64 gpu_resolve_ms = 0.0;
  f64 gpu_total_ms = 0.0;
  f64 gpu_rt_ms = 0.0;     // records + CLAS + cluster BLAS + TLAS builds
  f64 gpu_trace_ms = 0.0;  // the ray query pass
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
        !timer.create(device, k_frames_in_flight, 16, &error)) {
      exit_code = fail("frames", error);
      break;
    }

    // Geometry: the terrain's LOD DAG, or one DAG per primitive of a glTF file merged into one
    // so every cluster has a single material.
    assets::MeshData mesh_data;
    Vector<u32> part_of_cluster;  // --mesh: the primitive each cluster came from
    Vector<i32> part_material;    // --mesh: the material of each primitive with triangles
    const i64 build_start = time::monotonic_ns();
    if (options.mesh.empty()) {
      Vector<Vec3> positions;
      Vector<u32> indices;
      make_terrain(options.grid, 10.0f, positions, indices);
      Vector<Vec2> uvs;
      uvs.reserve(positions.size());
      for (const Vec3& p : positions)
        uvs.push_back(Vec2{(p.x + 10.0f) / 20.0f, (p.z + 10.0f) / 20.0f});
      geometry::AttributeSource attribute_source;
      attribute_source.uvs = std::span<const Vec2>(uvs.data(), uvs.size());  // normals: computed
      if (!geometry::build_cluster_lod(positions, indices, geometry::ClusterLodOptions{}, lod,
                                       &error, attribute_source)) {
        exit_code = fail("clusters", error);
        break;
      }
    } else {
      if (!assets::load_gltf(options.mesh, mesh_data, &error)) {
        exit_code = fail("mesh", error);
        break;
      }
      // Exporters duplicate vertices freely; welding the identical ones gives the cluster
      // builder shared vertices to fill clusters with and the LOD builder edges to collapse.
      const u32 loaded_vertices = mesh_data.positions.size();
      const u32 welded_vertices = geometry::weld_vertices(
          mesh_data.positions, mesh_data.normals, mesh_data.uvs,
          std::span<u32>(mesh_data.indices.data(), mesh_data.indices.size()));
      ENGINE_LOG_INFO(log_view, "mesh loaded", log::field("path", options.mesh),
                      log::field("vertices", loaded_vertices),
                      log::field("welded", welded_vertices),
                      log::field("triangles", mesh_data.indices.size() / 3),
                      log::field("primitives", mesh_data.primitives.size()),
                      log::field("materials", mesh_data.materials.size()),
                      log::field("images", mesh_data.images.size()));
      const geometry::AttributeSource attribute_source = assets::attribute_source(mesh_data);
      Vector<geometry::ClusterLodMesh> parts;
      bool parts_ok = true;
      for (const assets::Primitive& primitive : mesh_data.primitives) {
        if (primitive.index_count < 3) continue;
        const std::span<const u32> range(mesh_data.indices.data() + primitive.first_index,
                                         primitive.index_count);
        geometry::ClusterLodMesh part;
        if (!geometry::build_cluster_lod(mesh_data.positions, range, geometry::ClusterLodOptions{},
                                         part, &error, attribute_source)) {
          parts_ok = false;
          break;
        }
        parts.push_back(std::move(part));
        part_material.push_back(primitive.material);
      }
      if (!parts_ok || !geometry::merge_cluster_lod(parts, lod, &part_of_cluster, &error)) {
        exit_code = fail("mesh clusters", error);
        break;
      }
      mesh_primitives = parts.size();
      // The camera orbits the mesh's bounds instead of the heightfield's.
      Vec3 lo{1e30f, 1e30f, 1e30f};
      Vec3 hi{-1e30f, -1e30f, -1e30f};
      for (u32 i = 0; i < lod.level_cluster_counts[0]; ++i) {
        const geometry::ClusterDesc& c = lod.mesh.clusters[i];
        lo = Vec3{std::min(lo.x, c.center.x - c.radius), std::min(lo.y, c.center.y - c.radius),
                  std::min(lo.z, c.center.z - c.radius)};
        hi = Vec3{std::max(hi.x, c.center.x + c.radius), std::max(hi.y, c.center.y + c.radius),
                  std::max(hi.z, c.center.z + c.radius)};
      }
      scene_center = (lo + hi) * 0.5f;
      scene_radius = 1e-6f;  // tighter than the box diagonal for elongated meshes
      for (u32 i = 0; i < lod.level_cluster_counts[0]; ++i) {
        const geometry::ClusterDesc& c = lod.mesh.clusters[i];
        scene_radius = std::max(scene_radius, length(c.center - scene_center) + c.radius);
      }
    }
    build_ns = time::monotonic_ns() - build_start;
    const u32 cluster_count = lod.mesh.clusters.size();
    const u32 leaf_count = lod.level_cluster_counts[0];
    const u32 triangles_per_cluster = geometry::ClusterLodOptions{}.max_triangles;
    constexpr VkBufferUsageFlags k_storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    constexpr VkBufferUsageFlags k_address = k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    constexpr VkBufferUsageFlags k_args = k_address | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    // Positions go to the GPU on the mesh-wide 16-bit grid: six bytes a vertex instead of twelve.
    const u64 float_position_bytes = u64{lod.mesh.vertices.size()} * sizeof(Vec3);
    const u64 quantized_position_bytes =
        u64{lod.mesh.quantized.size()} * sizeof(u16) + sizeof(gfx::MeshDesc);
    if (!gfx::upload_buffer(device, lod.mesh.clusters.data(),
                            cluster_count * sizeof(geometry::ClusterDesc), k_storage,
                            cluster_buffer, &error) ||
        !gfx::upload_buffer(device, lod.mesh.quantized.data(),
                            u64{lod.mesh.quantized.size()} * sizeof(u16), k_storage,
                            quantized_buffer, &error) ||
        !gfx::upload_buffer(device, lod.mesh.triangles.data(),
                            lod.mesh.triangles.size() * sizeof(u32), k_storage, triangle_buffer,
                            &error) ||
        !gfx::upload_buffer(device, lod.lod.data(),
                            cluster_count * sizeof(geometry::ClusterLodDesc), k_storage, lod_buffer,
                            &error)) {
      exit_code = fail("upload", error);
      break;
    }
    gfx::MeshDesc mesh_block{};
    mesh_block.quant = Vec4{lod.mesh.quant_origin, lod.mesh.quant_scale};
    mesh_block.quantized = quantized_buffer.address;
    if (!gfx::upload_buffer(device, &mesh_block, sizeof(mesh_block), k_storage, mesh_buffer,
                            &error)) {
      exit_code = fail("upload", error);
      break;
    }
    // The float positions stay only for --raster rt: the cluster structure builds read them.
    if (ray_path &&
        !gfx::upload_buffer(device, lod.mesh.vertices.data(), float_position_bytes,
                            k_storage | gfx::k_build_input_usage, vertex_buffer, &error)) {
      exit_code = fail("upload", error);
      break;
    }
    ENGINE_LOG_INFO(log_view, "positions quantized",
                    log::field("vertices", lod.mesh.vertices.size()),
                    log::field("float_bytes", float_position_bytes),
                    log::field("quantized_bytes", quantized_position_bytes),
                    log::field("grid_step", lod.mesh.quant_scale));
    if (!gfx::upload_buffer(device, lod.mesh.attributes.data(),
                            lod.mesh.attributes.size() * sizeof(geometry::VertexAttributes),
                            k_storage, attribute_buffer, &error)) {
      exit_code = fail("attributes", error);
      break;
    }
    if (!gfx::create_sampler(device, VK_FILTER_LINEAR, texture_sampler, &error)) {
      exit_code = fail("sampler", error);
      break;
    }
    const u32 sampler_slot = bindless.add_sampler(texture_sampler);
    Vector<gfx::ResolveMaterial> materials;
    Vector<u32> cluster_material(cluster_count);
    if (options.mesh.empty()) {
      // A procedural ripple texture, linear-sampled through the bindless set.
      constexpr u32 k_texture_size = 256;
      Vector<u8> texels(k_texture_size * k_texture_size * 4);
      for (u32 y = 0; y < k_texture_size; ++y) {
        for (u32 x = 0; x < k_texture_size; ++x) {
          const f32 fx = static_cast<f32>(x);
          const f32 fy = static_cast<f32>(y);
          const f32 ripple = 0.5f + 0.5f * std::sin(fx * 0.25f + 2.0f * std::sin(fy * 0.08f));
          const f32 grain =
              0.5f + 0.5f * std::sin(fx * 1.7f + fy * 2.3f) * std::sin(fy * 1.1f - fx * 0.7f);
          const u8 v = static_cast<u8>((0.62f + 0.3f * ripple + 0.08f * grain) * 255.0f);
          u8* t = &texels[(y * k_texture_size + x) * 4];
          t[0] = t[1] = t[2] = v;
          t[3] = 255;
        }
      }
      if (!gfx::upload_image_2d(device, k_texture_size, k_texture_size, VK_FORMAT_R8G8B8A8_UNORM,
                                texels.data(), texels.size(), texture, &error) ||
          !gfx::create_image_view(device, texture, texture_view, &error)) {
        exit_code = fail("texture", error);
        break;
      }
      const u32 texture_slot =
          bindless.add_sampled_image(texture_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      // Materials: a flat table indexed per cluster by the height band of the cluster's center.
      materials.resize(3);
      materials[0].albedo = Vec4{0.86f, 0.72f, 0.46f, 0.9f};  // sand
      materials[1].albedo = Vec4{0.42f, 0.40f, 0.38f, 0.7f};  // rock
      materials[2].albedo = Vec4{0.92f, 0.94f, 0.97f, 0.4f};  // snow
      for (u32 i = 0; i < 2; ++i) {  // sand and rock carry the ripple texture at different scales
        materials[i].albedo_texture = texture_slot;
        materials[i].sampler = sampler_slot;
        materials[i].uv_scale = i == 0 ? 24.0f : 9.0f;
      }
      for (u32 i = 0; i < cluster_count; ++i) {
        const f32 y = lod.mesh.clusters[i].center.y;
        cluster_material[i] = y < -0.15f ? 0u : y < 0.65f ? 1u : 2u;
      }
    } else {
      // Materials from the file, plus a default for primitives without one. Base-color images
      // are decoded on the CPU (embedded bytes or a file beside the glTF) and uploaded as sRGB
      // so sampling returns linear color; an image that fails to decode leaves its material
      // untextured with a warning.
      Vector<u32> image_slot(mesh_data.images.size(), gfx::k_no_texture);
      Vector<bool> image_tried(mesh_data.images.size(), false);
      const std::string mesh_dir(io::parent_path(options.mesh));
      auto texture_slot_of = [&](i32 image_index) -> u32 {
        if (image_index < 0 || static_cast<u32>(image_index) >= mesh_data.images.size())
          return gfx::k_no_texture;
        const u32 index = static_cast<u32>(image_index);
        if (image_tried[index]) return image_slot[index];
        image_tried[index] = true;
        const assets::ImageRef& ref = mesh_data.images[index];
        image::Image decoded;
        std::string image_error;
        bool ok = false;
        if (!ref.bytes.empty()) {
          ok = image::decode_image(std::span<const u8>(ref.bytes.data(), ref.bytes.size()), decoded,
                                   4, &image_error);
        } else if (!ref.uri.empty()) {
          const std::string path = mesh_dir.empty() ? ref.uri : io::join_path(mesh_dir, ref.uri);
          ok = image::read_image(path, decoded, 4, &image_error) == io::Status::Ok;
        } else {
          image_error = "image has neither bytes nor a uri";
        }
        gfx::ImageResource uploaded;
        VkImageView view = VK_NULL_HANDLE;
        if (ok && (!gfx::upload_image_2d(device, decoded.width, decoded.height,
                                         VK_FORMAT_R8G8B8A8_SRGB, decoded.pixels.data(),
                                         decoded.pixels.size(), uploaded, &image_error) ||
                   !gfx::create_image_view(device, uploaded, view, &image_error))) {
          if (uploaded.image != VK_NULL_HANDLE) gfx::destroy_image(device, uploaded);
          ok = false;
        }
        if (!ok) {
          ENGINE_LOG_WARN(log_view, "texture skipped", log::field("image", index),
                          log::field("name", ref.name), log::field("error", image_error));
          return gfx::k_no_texture;
        }
        mesh_textures.push_back(uploaded);
        mesh_texture_views.push_back(view);
        image_slot[index] =
            bindless.add_sampled_image(view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return image_slot[index];
      };
      for (const assets::Material& source : mesh_data.materials) {
        gfx::ResolveMaterial material;
        material.albedo =
            Vec4{source.base_color.x, source.base_color.y, source.base_color.z, source.roughness};
        material.emissive = Vec4{0.0f, 0.0f, 0.0f, source.metallic};
        material.albedo_texture = texture_slot_of(source.base_color_image);
        material.sampler = sampler_slot;
        material.uv_scale = 1.0f;
        materials.push_back(material);
      }
      const u32 default_material = materials.size();
      gfx::ResolveMaterial plain;
      plain.albedo = Vec4{0.8f, 0.8f, 0.8f, 0.6f};
      materials.push_back(plain);
      for (u32 i = 0; i < cluster_count; ++i) {
        const i32 material = part_material[part_of_cluster[i]];
        cluster_material[i] = material >= 0 && static_cast<u32>(material) < default_material
                                  ? static_cast<u32>(material)
                                  : default_material;
      }
    }
    if (!gfx::upload_buffer(device, materials.data(),
                            materials.size() * sizeof(gfx::ResolveMaterial), k_storage,
                            material_buffer, &error) ||
        !gfx::upload_buffer(device, cluster_material.data(), cluster_count * sizeof(u32), k_storage,
                            cluster_material_buffer, &error)) {
      exit_code = fail("materials", error);
      break;
    }
    bool buffers_ok = true;
    for (u32 i = 0; i < 2; ++i) {
      buffers_ok =
          buffers_ok &&
          gfx::create_buffer(device, u64{cluster_count} * sizeof(u32), k_address, false,
                             visible_buffer[i], &error) &&
          gfx::create_buffer(device, sizeof(u32) * 4, k_args, false, args_buffer[i], &error) &&
          gfx::create_buffer(device, u64{cluster_count} * sizeof(u32),
                             k_address | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, flags_buffer[i],
                             &error);
    }
    buffers_ok = buffers_ok &&
                 gfx::create_buffer(device, u64{cluster_count} * sizeof(u32), k_address, false,
                                    sw_visible_buffer, &error) &&
                 gfx::create_buffer(device, sizeof(u32) * 3, k_args, false, sw_args_buffer, &error);
    for (u32 slot = 0; slot < k_frames_in_flight && buffers_ok; ++slot) {
      buffers_ok = gfx::create_buffer(device, sizeof(gfx::CullParams) * 2, k_address, true,
                                      params_buffers[slot], &error) &&
                   gfx::create_buffer(device, sizeof(gfx::ResolveParams), k_address, true,
                                      resolve_buffers[slot], &error) &&
                   gfx::create_buffer(device, sizeof(u32) * 9, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      true, stats_buffers[slot], &error);
      if (buffers_ok) std::memset(stats_buffers[slot].mapped, 0, sizeof(u32) * 9);
    }
    if (!buffers_ok) {
      exit_code = fail("buffers", error);
      break;
    }
    if (ray_path) {
      // Every cluster may be in some frame's cut, so the structures are sized for all of them.
      Vector<u8> indices8;
      gfx::pack_cluster_indices(
          std::span<const u32>(lod.mesh.triangles.data(), lod.mesh.triangles.size()), indices8);
      gfx::ClusterSetLimits limits;
      limits.max_clusters = cluster_count;
      limits.max_triangles_per_cluster = triangles_per_cluster;
      limits.max_vertices_per_cluster = geometry::ClusterLodOptions{}.max_vertices;
      limits.max_geometry_index = cluster_count - 1;
      constexpr VkBufferUsageFlags k_record_usage =
          k_address | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
      bool rt_ok = gfx::upload_buffer(device, indices8.data(), indices8.size(),
                                      gfx::k_build_input_usage, indices8_buffer, &error) &&
                   gfx::create_buffer(device, gfx::k_cluster_build_record_bytes * cluster_count,
                                      k_record_usage, false, records_buffer, &error) &&
                   gfx::create_buffer(device, sizeof(u32), k_record_usage, false,
                                      record_count_buffer, &error) &&
                   gfx::create_cluster_set(device, limits, clas_set, &error) &&
                   gfx::create_cluster_blas(device, cluster_count, cluster_blas, &error) &&
                   gfx::create_tlas(device, 1, gfx::k_build_fast_trace, tlas, &error) &&
                   gfx::create_buffer(device, gfx::k_instance_record_bytes,
                                      gfx::k_build_input_usage, true, rt_instances, &error);
      if (rt_ok) {
        u64 scratch_bytes = clas_set.build_scratch_bytes;
        scratch_bytes = std::max(scratch_bytes, cluster_blas.build_scratch_bytes);
        scratch_bytes = std::max(scratch_bytes, tlas.build_scratch_bytes);
        rt_ok = gfx::create_scratch(device, scratch_bytes, rt_scratch, &error);
        for (u32 s = 0; s < k_frames_in_flight && rt_ok; ++s) {
          rt_ok = gfx::create_buffer(device, sizeof(gfx::RayVisibilityParams), k_address, true,
                                     ray_params[s], &error);
        }
        gfx::InstanceDesc instance;
        instance.blas = cluster_blas.address;
        gfx::write_instances(std::span<const gfx::InstanceDesc>(&instance, 1), rt_instances.mapped);
        tlas_slot = bindless.add_acceleration_structure(tlas.handle);
        if (tlas_slot == gfx::BindlessSet::k_invalid_slot) {
          rt_ok = false;
          error = "no bindless slot for the top-level structure";
        }
      }
      if (!rt_ok) {
        exit_code = fail("ray tracing", error);
        break;
      }
      ENGINE_LOG_INFO(log_view, "ray tracing ready", log::field("clusters", cluster_count),
                      log::field("clas_bytes", clas_set.data.size),
                      log::field("blas_bytes", cluster_blas.data.size),
                      log::field("scratch_bytes", rt_scratch.size));
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
    shader_library.add_embedded("hiz_build", shaders::k_hiz_build_spirv,
                                shaders::k_hiz_build_spirv_size);
    shader_library.add_embedded("cluster_vertex", shaders::k_cluster_vertex_spirv,
                                shaders::k_cluster_vertex_spirv_size);
    shader_library.add_embedded("visibility_resolve", shaders::k_visibility_resolve_spirv,
                                shaders::k_visibility_resolve_spirv_size);
    shader_library.add_embedded("clas_records", shaders::k_clas_records_spirv,
                                shaders::k_clas_records_spirv_size);
    shader_library.add_embedded("ray_visibility", shaders::k_ray_visibility_spirv,
                                shaders::k_ray_visibility_spirv_size);
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
      const gfx::Shader* hiz = sw != nullptr ? shader_library.get("hiz_build", err) : nullptr;
      const gfx::Shader* vertex =
          hiz != nullptr ? shader_library.get("cluster_vertex", err) : nullptr;
      const gfx::Shader* resolve =
          hiz != nullptr ? shader_library.get("visibility_resolve", err) : nullptr;
      if (resolve == nullptr || vertex == nullptr) return false;
      if (ray_path) {
        const gfx::Shader* records = shader_library.get("clas_records", err);
        const gfx::Shader* trace =
            records != nullptr ? shader_library.get("ray_visibility", err) : nullptr;
        if (trace == nullptr) return false;
        const VkDescriptorSetLayout set_layout = bindless.layout();
        if (!gfx::create_compute_pipeline(device, records->module, "records_main", {},
                                          sizeof(gfx::ClusterRecordParams), pipelines.records,
                                          err) ||
            !gfx::create_compute_pipeline(device, trace->module, "trace_main",
                                          std::span<const VkDescriptorSetLayout>(&set_layout, 1),
                                          sizeof(u64), pipelines.trace, err)) {
          return false;
        }
      }
      gfx::GraphicsPipelineDesc vertex_desc;
      vertex_desc.vertex = vertex->module;
      vertex_desc.vertex_entry = "vs_cluster";
      vertex_desc.fragment = vertex->module;
      vertex_desc.fragment_entry = "fs_visibility";
      vertex_desc.layout = bindless.pipeline_layout();
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
             gfx::create_graphics_pipeline(device, vertex_desc, pipelines.vertex, err) &&
             gfx::create_compute_pipeline(device, sw->module, "sw_raster_main", {},
                                          sizeof(gfx::ClusterDrawParams), pipelines.software,
                                          err) &&
             gfx::create_compute_pipeline(device, cull->module, "cull_main", {}, sizeof(u64),
                                          pipelines.cull, err) &&
             gfx::create_compute_pipeline(device, hiz->module, "hiz_build_main", {},
                                          sizeof(gfx::HizParams), pipelines.hiz, err) &&
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
    ENGINE_LOG_INFO(
        log_view, "ready", log::field("clusters", cluster_count),
        log::field("leaf_clusters", leaf_count), log::field("triangles", lod.leaf_triangle_count),
        log::field("lod_levels", lod.level_cluster_counts.size()),
        log::field("build_ms", static_cast<f64>(build_ns) / 1.0e6),
        log::field("raster", raster_name(options.raster)), log::field("occlusion", occlusion),
        log::field("cone", options.cone), log::field("mesh_primitives", mesh_primitives),
        log::field("materials", materials.size()), log::field("width", swapchain.extent().width),
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
          flags_dirty = true;  // last frame's visible set no longer matches the Hi-Z
        }
      }

      VkCommandBuffer commands = frames.begin_frame();
      const u32 slot = frames.slot();
      timer.begin_frame(commands, slot);
      if (rendered >= k_frames_in_flight) {
        // The frame that last used this slot has completed: its statistics are readable.
        if (options.cull) {
          const auto* stats = static_cast<const u32*>(stats_buffers[slot].mapped);
          visible_hw_last = stats[vertex_path ? 1 : 0];
          visible_pass2_last = stats[vertex_path ? 4 : 3];
          visible_sw_last = stats[6];
          const u32 total = visible_hw_last + visible_pass2_last + visible_sw_last;
          visible_min = total < visible_min ? total : visible_min;
          visible_max = total > visible_max ? total : visible_max;
        }
        if (!timer.results().empty()) {
          gpu_cull_ms += timer.ms("cull");
          gpu_hw_ms += timer.ms("hw");
          gpu_sw_ms += timer.ms("sw");
          gpu_hiz_ms += timer.ms("hiz");
          gpu_resolve_ms += timer.ms("resolve");
          gpu_rt_ms += timer.ms("records") + timer.ms("clas") + timer.ms("blas") + timer.ms("tlas");
          gpu_trace_ms += timer.ms("trace");
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
      const f32 distance =
          (options.orbit > 0.0f ? options.orbit
                                : 22.0f + 14.0f * std::sin(static_cast<f32>(rendered) * 0.004f)) *
          (scene_radius / 10.0f);
      const Vec3 eye = scene_center + Vec3{std::cos(angle) * distance, 0.45f * distance,
                                           std::sin(angle) * distance};
      const f32 fov_y = radians(55.0f);
      const f32 znear = 0.01f * scene_radius;  // 0.1 for the heightfield; a 2 cm mesh gets 0.2 mm
      const Mat4 view_proj = perspective_reversed_z(fov_y, aspect, znear) *
                             look_at(eye, scene_center, Vec3{0.0f, 1.0f, 0.0f});
      const f32 proj_scale = 1.0f / std::tan(fov_y * 0.5f) * static_cast<f32>(extent.height) * 0.5f;
      const bool direct = options.raster == RasterMode::Direct;
      const bool use_hw = options.raster != RasterMode::Software && !ray_path;
      const bool use_sw = !direct && !ray_path && options.raster != RasterMode::Hardware &&
                          options.raster != RasterMode::Vertex && options.cull;
      const u32 cur_flags = static_cast<u32>(rendered % 2);
      const u32 prev_flags = 1 - cur_flags;

      gfx::ClusterDrawParams draw{};
      draw.view_proj = view_proj;
      draw.clusters = cluster_buffer.address;
      draw.mesh = mesh_buffer.address;
      draw.triangles = triangle_buffer.address;
      draw.cluster_count = options.cull ? cluster_count : leaf_count;
      draw.triangles_per_cluster = triangles_per_cluster;
      draw.visible = options.cull ? visible_buffer[0].address : 0;
      draw.visibility = targets.vis.address;
      draw.width = extent.width;
      draw.height = extent.height;
      gfx::ClusterDrawParams draw_pass2 = draw;
      draw_pass2.visible = visible_buffer[1].address;
      gfx::ClusterDrawParams draw_sw = draw;
      draw_sw.visible = sw_visible_buffer.address;

      gfx::CullParams cull{};
      gfx::set_frustum(cull, frustum_from_view_proj(view_proj));
      cull.view_proj = view_proj;
      cull.camera = Vec4{eye, znear};
      cull.lod = Vec4{proj_scale, options.lod_px, 1.0f, 1.0f};
      const f32 raster_mode =
          direct || options.raster == RasterMode::Hardware || vertex_path || ray_path
              ? gfx::k_raster_hardware
          : options.raster == RasterMode::Software ? gfx::k_raster_software
                                                   : gfx::k_raster_split;
      cull.raster = Vec4{options.sw_px, raster_mode, 0.0f, 0.0f};
      cull.cluster_count = cluster_count;
      cull.count_index = vertex_path ? 1u : 0u;
      cull.cone_cull = options.cone ? 1u : 0u;
      cull.clusters = cluster_buffer.address;
      cull.lods = lod_buffer.address;
      cull.visible = visible_buffer[0].address;
      cull.draw_args = args_buffer[0].address;
      cull.sw_visible = sw_visible_buffer.address;
      cull.sw_args = sw_args_buffer.address;
      if (occlusion) {
        cull.hiz = targets.hiz.address;
        cull.prev_flags = flags_buffer[prev_flags].address;
        cull.flags = flags_buffer[cur_flags].address;
        cull.hiz_width = extent.width;
        cull.hiz_height = extent.height;
        cull.hiz_mips = targets.hiz_mips;
        std::memcpy(cull.hiz_offsets, targets.hiz_offsets, sizeof(cull.hiz_offsets));
        cull.pass = 1;
      }
      gfx::CullParams cull_pass2 = cull;
      cull_pass2.pass = 2;
      cull_pass2.visible = visible_buffer[1].address;
      cull_pass2.draw_args = args_buffer[1].address;
      auto* blocks = static_cast<gfx::CullParams*>(params_buffers[slot].mapped);
      blocks[0] = cull;
      blocks[1] = cull_pass2;
      const u64 block_address[2] = {params_buffers[slot].address,
                                    params_buffers[slot].address + sizeof(gfx::CullParams)};

      gfx::ResolveParams resolve{};
      resolve.sky = Vec4{0.55f, 0.70f, 0.90f, 1.0f};
      resolve.sun = Vec4{normalize(Vec3{0.4f, 0.8f, 0.45f}), 1.0f};
      resolve.camera = Vec4{eye, 0.0f};
      resolve.view_proj = view_proj;
      resolve.visibility = targets.vis.address;
      resolve.clusters = cluster_buffer.address;
      resolve.mesh = mesh_buffer.address;
      resolve.triangles = triangle_buffer.address;
      resolve.materials = material_buffer.address;
      resolve.cluster_materials = cluster_material_buffer.address;
      resolve.attributes = attribute_buffer.address;
      resolve.width = extent.width;
      resolve.height = extent.height;
      resolve.mode = options.view_mode;
      std::memcpy(resolve_buffers[slot].mapped, &resolve, sizeof(resolve));
      const u64 resolve_address = resolve_buffers[slot].address;

      // --raster rt: the records pass turns this frame's visible list into CLAS build records,
      // the builds follow on the GPU, and the trace pass replaces the rasterizer.
      gfx::ClusterRecordParams record_params{};
      u64 ray_address = 0;
      if (ray_path) {
        record_params.clusters = cluster_buffer.address;
        record_params.vertices = vertex_buffer.address;
        record_params.indices8 = indices8_buffer.address;
        record_params.visible = visible_buffer[0].address;
        record_params.visible_count = args_buffer[0].address;  // count_index 0: the first word
        record_params.records = records_buffer.address;
        record_params.record_count = record_count_buffer.address;
        record_params.blas_record = cluster_blas.record.address;
        record_params.clas_addresses = clas_set.addresses.address;
        record_params.max_clusters = cluster_count;
        gfx::RayVisibilityParams ray{};
        ray.view_proj = view_proj;
        ray.inv_view_proj = inverse(view_proj);
        ray.camera = Vec4{eye, 0.0f};
        ray.output = targets.vis.address;
        ray.cut = 0;  // geometry indices are cluster ids
        ray.width = extent.width;
        ray.height = extent.height;
        ray.scene = tlas_slot;
        std::memcpy(ray_params[slot].mapped, &ray, sizeof(ray));
        ray_address = ray_params[slot].address;
      }

      graph.reset();
      const gfx::RgImage color = graph.import_image("swapchain", swapchain.image(image_index));
      const gfx::RgImage depth_target = graph.import_image("depth", targets.depth);
      const gfx::RgBuffer rg_args[2] = {graph.import_buffer("draw_args", args_buffer[0]),
                                        graph.import_buffer("draw_args2", args_buffer[1])};
      const gfx::RgBuffer rg_visible[2] = {graph.import_buffer("visible", visible_buffer[0]),
                                           graph.import_buffer("visible2", visible_buffer[1])};
      const gfx::RgBuffer rg_flags[2] = {graph.import_buffer("flags0", flags_buffer[0]),
                                         graph.import_buffer("flags1", flags_buffer[1])};
      const gfx::RgBuffer rg_sw_args = graph.import_buffer("sw_args", sw_args_buffer);
      const gfx::RgBuffer rg_sw_visible = graph.import_buffer("sw_visible", sw_visible_buffer);
      const gfx::RgBuffer rg_vis = graph.import_buffer("visibility", targets.vis);
      const gfx::RgBuffer rg_hiz = graph.import_buffer("hiz", targets.hiz);
      const gfx::RgBuffer rg_stats = graph.import_buffer("stats", stats_buffers[slot]);
      struct RtBuffers {
        gfx::RgBuffer records, record_count, blas_record, clas_data, clas_addresses, clas_sizes;
        gfx::RgBuffer blas_data, tlas, instances;
      } rt{};
      if (ray_path) {
        rt.records = graph.import_buffer("clas records", records_buffer);
        rt.record_count = graph.import_buffer("clas record count", record_count_buffer);
        rt.blas_record = graph.import_buffer("cluster blas record", cluster_blas.record);
        rt.clas_data = graph.import_buffer("clas", clas_set.data);
        rt.clas_addresses = graph.import_buffer("clas addresses", clas_set.addresses);
        rt.clas_sizes = graph.import_buffer("clas sizes", clas_set.sizes);
        rt.blas_data = graph.import_buffer("cluster blas", cluster_blas.data);
        rt.tlas = graph.import_buffer("tlas", tlas.buffer);
        rt.instances = graph.import_buffer("tlas instances", rt_instances);
      }
      VkClearColorValue sky{};
      sky.float32[0] = 0.55f;
      sky.float32[1] = 0.70f;
      sky.float32[2] = 0.90f;
      sky.float32[3] = 1.0f;
      const bool fill_hiz = occlusion && targets.hiz_dirty;
      const bool fill_flags = occlusion && flags_dirty;

      graph.add_pass(
          "reset", gfx::PassKind::Transfer,
          [&](gfx::PassBuilder& b) {
            if (options.cull) {
              b.write(rg_args[0], gfx::Access::TransferWrite);
              b.write(rg_args[1], gfx::Access::TransferWrite);
              b.write(rg_sw_args, gfx::Access::TransferWrite);
            }
            if (!direct) b.write(rg_vis, gfx::Access::TransferWrite);
            if (occlusion) b.write(rg_flags[cur_flags], gfx::Access::TransferWrite);
            if (fill_flags) b.write(rg_flags[prev_flags], gfx::Access::TransferWrite);
            if (fill_hiz) b.write(rg_hiz, gfx::Access::TransferWrite);
          },
          [&](VkCommandBuffer cb, gfx::RenderGraph&) {
            if (options.cull) {
              for (u32 i = 0; i < 2; ++i) {
                if (vertex_path) {  // {vertexCount, instanceCount = 0, firstVertex, firstInstance}
                  vkCmdFillBuffer(cb, args_buffer[i].buffer, 0, sizeof(u32),
                                  triangles_per_cluster * 3);
                  vkCmdFillBuffer(cb, args_buffer[i].buffer, sizeof(u32), sizeof(u32) * 3, 0);
                } else {  // {groups = 0, 1, 1}
                  vkCmdFillBuffer(cb, args_buffer[i].buffer, 0, sizeof(u32), 0);
                  vkCmdFillBuffer(cb, args_buffer[i].buffer, sizeof(u32), sizeof(u32) * 2, 1);
                }
              }
              vkCmdFillBuffer(cb, sw_args_buffer.buffer, 0, sizeof(u32), 0);
              vkCmdFillBuffer(cb, sw_args_buffer.buffer, sizeof(u32), sizeof(u32) * 2, 1);
            }
            if (!direct) vkCmdFillBuffer(cb, targets.vis.buffer, 0, VK_WHOLE_SIZE, 0);
            if (occlusion) vkCmdFillBuffer(cb, flags_buffer[cur_flags].buffer, 0, VK_WHOLE_SIZE, 0);
            if (fill_flags)
              vkCmdFillBuffer(cb, flags_buffer[prev_flags].buffer, 0, VK_WHOLE_SIZE, 0);
            if (fill_hiz) vkCmdFillBuffer(cb, targets.hiz.buffer, 0, VK_WHOLE_SIZE, 0);
          });
      auto add_cull = [&](u32 block, u32 list) {
        graph.add_pass(
            "cull", gfx::PassKind::Compute,
            [&, list](gfx::PassBuilder& b) {
              b.write(rg_args[list], gfx::Access::ComputeReadWrite);
              b.write(rg_visible[list], gfx::Access::ComputeWrite);
              if (use_sw) {
                b.write(rg_sw_args, gfx::Access::ComputeReadWrite);
                b.write(rg_sw_visible, gfx::Access::ComputeWrite);
              }
              if (occlusion) {
                b.read(rg_hiz, gfx::Access::ComputeRead);
                b.read(rg_flags[prev_flags], gfx::Access::ComputeRead);
                b.write(rg_flags[cur_flags], gfx::Access::ComputeReadWrite);
              }
            },
            [&, block](VkCommandBuffer cb, gfx::RenderGraph&) {
              timer.begin(cb, "cull");
              vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.cull.pipeline);
              vkCmdPushConstants(cb, pipelines.cull.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                 sizeof(u64), &block_address[block]);
              vkCmdDispatch(cb, gfx::cull_group_count(cluster_count), 1, 1);
              timer.end(cb);
            });
      };
      auto add_hw_draw = [&](u32 list, const gfx::ClusterDrawParams* params) {
        graph.add_pass(
            "hardware", gfx::PassKind::Raster,
            [&, list](gfx::PassBuilder& b) {
              b.render_area(extent.width, extent.height);
              b.write(rg_vis, gfx::Access::FragmentReadWrite);
              if (options.cull) {
                b.read(rg_args[list], gfx::Access::IndirectRead);
                b.read(rg_visible[list],
                       vertex_path ? gfx::Access::VertexRead : gfx::Access::MeshRead);
              }
            },
            [&, list, params](VkCommandBuffer cb, gfx::RenderGraph&) {
              timer.begin(cb, "hw");
              vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                vertex_path ? pipelines.vertex : pipelines.hardware);
              bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
              vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                                 sizeof(*params), params);
              if (vertex_path) {
                if (options.cull) {
                  vkCmdDrawIndirect(cb, args_buffer[list].buffer, 0, 1, sizeof(u32) * 4);
                } else {
                  vkCmdDraw(cb, triangles_per_cluster * 3, leaf_count, 0, 0);
                }
              } else if (options.cull) {
                vkCmdDrawMeshTasksIndirectEXT(cb, args_buffer[list].buffer, 0, 1, sizeof(u32) * 3);
              } else {
                vkCmdDrawMeshTasksEXT(cb, leaf_count, 1, 1);
              }
              timer.end(cb);
            });
      };
      auto add_hiz = [&](u32 set) {
        for (u32 m = 0; m < targets.hiz_mips; ++m) {
          gfx::HizParams* level = &targets.hiz_levels[set * targets.hiz_mips + m];
          *level = gfx::HizParams{};
          level->from_visibility = m == 0 ? 1u : 0u;
          level->src = m == 0 ? targets.vis.address
                              : targets.hiz.address + u64{targets.hiz_offsets[m - 1]} * 4;
          level->dst = targets.hiz.address + u64{targets.hiz_offsets[m]} * 4;
          level->src_width = m == 0 ? extent.width : gfx::hiz_mip_extent(extent.width, m - 1);
          level->src_height = m == 0 ? extent.height : gfx::hiz_mip_extent(extent.height, m - 1);
          level->dst_width = gfx::hiz_mip_extent(extent.width, m);
          level->dst_height = gfx::hiz_mip_extent(extent.height, m);
          graph.add_pass(
              "hiz", gfx::PassKind::Compute,
              [&, m](gfx::PassBuilder& b) {
                if (m == 0) b.read(rg_vis, gfx::Access::ComputeRead);
                b.write(rg_hiz, gfx::Access::ComputeReadWrite);
              },
              [&, level, m](VkCommandBuffer cb, gfx::RenderGraph&) {
                if (m == 0) timer.begin(cb, "hiz");
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.hiz.pipeline);
                vkCmdPushConstants(cb, pipelines.hiz.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(*level), level);
                vkCmdDispatch(cb, gfx::hiz_group_count(level->dst_width),
                              gfx::hiz_group_count(level->dst_height), 1);
                if (m + 1 == targets.hiz_mips) timer.end(cb);
              });
        }
      };

      if (options.cull) add_cull(0, 0);
      if (direct) {
        graph.add_pass(
            "terrain", gfx::PassKind::Raster,
            [&](gfx::PassBuilder& b) {
              b.color_attachment(color, VK_ATTACHMENT_LOAD_OP_CLEAR, sky);
              b.depth_attachment(depth_target, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                 0.0f);  // reversed Z: far is 0
              if (options.cull) {
                b.read(rg_args[0], gfx::Access::IndirectRead);
                b.read(rg_visible[0], gfx::Access::MeshRead);
              }
            },
            [&](VkCommandBuffer cb, gfx::RenderGraph&) {
              timer.begin(cb, "hw");
              vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.direct);
              bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
              vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                                 sizeof(draw), &draw);
              if (options.cull) {
                vkCmdDrawMeshTasksIndirectEXT(cb, args_buffer[0].buffer, 0, 1, sizeof(u32) * 3);
              } else {
                vkCmdDrawMeshTasksEXT(cb, leaf_count, 1, 1);
              }
              timer.end(cb);
            });
      } else {
        if (use_hw) add_hw_draw(0, &draw);
        if (occlusion) {
          add_hiz(0);
          add_cull(1, 1);
          add_hw_draw(1, &draw_pass2);
          add_hiz(1);
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
        if (ray_path) {
          graph.add_pass(
              "records", gfx::PassKind::Compute,
              [&](gfx::PassBuilder& b) {
                b.read(rg_args[0], gfx::Access::ComputeRead);
                b.read(rg_visible[0], gfx::Access::ComputeRead);
                b.write(rt.records, gfx::Access::ComputeWrite);
                b.write(rt.record_count, gfx::Access::ComputeWrite);
                b.write(rt.blas_record, gfx::Access::ComputeWrite);
              },
              [&](VkCommandBuffer cb, gfx::RenderGraph&) {
                timer.begin(cb, "records");
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.records.pipeline);
                vkCmdPushConstants(cb, pipelines.records.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(record_params), &record_params);
                vkCmdDispatch(cb,
                              (cluster_count + gfx::k_cluster_records_workgroup - 1) /
                                  gfx::k_cluster_records_workgroup,
                              1, 1);
                timer.end(cb);
              });
          graph.add_pass(
              "clas", gfx::PassKind::Compute,
              [&](gfx::PassBuilder& b) {
                b.read(rt.records, gfx::Access::AccelerationBuildRead);
                b.read(rt.record_count, gfx::Access::AccelerationBuildRead);
                b.write(rt.clas_data, gfx::Access::AccelerationBuildWrite);
                b.write(rt.clas_addresses, gfx::Access::AccelerationBuildWrite);
                b.write(rt.clas_sizes, gfx::Access::AccelerationBuildWrite);
              },
              [&](VkCommandBuffer cb, gfx::RenderGraph&) {
                timer.begin(cb, "clas");
                gfx::build_cluster_set(cb, clas_set, records_buffer.address,
                                       record_count_buffer.address, rt_scratch);
                timer.end(cb);
              });
          graph.add_pass(
              "blas", gfx::PassKind::Compute,
              [&](gfx::PassBuilder& b) {
                b.read(rt.blas_record, gfx::Access::AccelerationBuildRead);
                b.read(rt.clas_addresses, gfx::Access::AccelerationBuildRead);
                b.read(rt.clas_data, gfx::Access::AccelerationBuildRead);
                b.write(rt.blas_data, gfx::Access::AccelerationBuildWrite);
              },
              [&](VkCommandBuffer cb, gfx::RenderGraph&) {
                timer.begin(cb, "blas");
                gfx::build_cluster_blas_indirect(cb, cluster_blas, rt_scratch);
                timer.end(cb);
              });
          graph.add_pass(
              "tlas", gfx::PassKind::Compute,
              [&](gfx::PassBuilder& b) {
                b.read(rt.blas_data, gfx::Access::AccelerationBuildRead);
                b.read(rt.instances, gfx::Access::AccelerationBuildRead);
                b.write(rt.tlas, gfx::Access::AccelerationBuildWrite);
              },
              [&](VkCommandBuffer cb, gfx::RenderGraph&) {
                timer.begin(cb, "tlas");
                gfx::build_tlas(cb, tlas, rt_instances.address, 1, gfx::k_build_fast_trace,
                                rt_scratch);
                timer.end(cb);
              });
          graph.add_pass(
              "trace", gfx::PassKind::Compute,
              [&](gfx::PassBuilder& b) {
                b.read(rt.tlas, gfx::Access::RayQueryRead);
                b.read(rt.blas_data, gfx::Access::RayQueryRead);
                b.read(rt.clas_data, gfx::Access::RayQueryRead);
                b.write(rg_vis, gfx::Access::ComputeWrite);
              },
              [&](VkCommandBuffer cb, gfx::RenderGraph&) {
                timer.begin(cb, "trace");
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.trace.pipeline);
                bindless.bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE);
                vkCmdPushConstants(cb, pipelines.trace.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(u64), &ray_address);
                vkCmdDispatch(cb, gfx::ray_visibility_group_count(extent.width),
                              gfx::ray_visibility_group_count(extent.height), 1);
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
                                 sizeof(u64), &resolve_address);
              vkCmdDraw(cb, 3, 1, 0, 0);
              timer.end(cb);
            });
      }
      if (options.cull) {
        graph.add_pass(
            "stats", gfx::PassKind::Transfer,
            [&](gfx::PassBuilder& b) {
              b.read(rg_args[0], gfx::Access::TransferRead);
              b.read(rg_args[1], gfx::Access::TransferRead);
              b.read(rg_sw_args, gfx::Access::TransferRead);
              b.write(rg_stats, gfx::Access::TransferWrite);
            },
            [&](VkCommandBuffer cb, gfx::RenderGraph&) {
              const gfx::BufferResource* sources[3] = {&args_buffer[0], &args_buffer[1],
                                                       &sw_args_buffer};
              for (u32 i = 0; i < 3; ++i) {
                const VkBufferCopy copy{0, sizeof(u32) * 3 * i, sizeof(u32) * 3};
                vkCmdCopyBuffer(cb, sources[i]->buffer, stats_buffers[slot].buffer, 1, &copy);
              }
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
      targets.hiz_dirty = false;
      flags_dirty = false;

      const bool last = options.frames != 0 && rendered >= options.frames;
      if (last && !options.capture.empty()) {
        frames.wait(value);
        if (options.cull) {
          const auto* stats = static_cast<const u32*>(stats_buffers[slot].mapped);
          visible_hw_last = stats[vertex_path ? 1 : 0];
          visible_pass2_last = stats[vertex_path ? 4 : 3];
          visible_sw_last = stats[6];
          const u32 total = visible_hw_last + visible_pass2_last + visible_sw_last;
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
    gfx::destroy_buffer(device, resolve_buffers[slot]);
    gfx::destroy_buffer(device, stats_buffers[slot]);
  }
  for (u32 i = 0; i < 2; ++i) {
    gfx::destroy_buffer(device, flags_buffer[i]);
    gfx::destroy_buffer(device, args_buffer[i]);
    gfx::destroy_buffer(device, visible_buffer[i]);
  }
  gfx::destroy_buffer(device, sw_args_buffer);
  gfx::destroy_buffer(device, sw_visible_buffer);
  if (ray_path) {
    gfx::destroy_acceleration_structure(device, tlas);
    gfx::destroy_cluster_blas(device, cluster_blas);
    gfx::destroy_cluster_set(device, clas_set);
    for (u32 s = 0; s < k_frames_in_flight; ++s)
      gfx::destroy_buffer(device, ray_params[s]);
    gfx::destroy_buffer(device, rt_scratch);
    gfx::destroy_buffer(device, rt_instances);
    gfx::destroy_buffer(device, record_count_buffer);
    gfx::destroy_buffer(device, records_buffer);
    gfx::destroy_buffer(device, indices8_buffer);
  }
  gfx::destroy_buffer(device, lod_buffer);
  gfx::destroy_buffer(device, attribute_buffer);
  gfx::destroy_sampler(device, texture_sampler);
  gfx::destroy_image_view(device, texture_view);
  if (texture.image != VK_NULL_HANDLE) gfx::destroy_image(device, texture);
  for (VkImageView view : mesh_texture_views)
    gfx::destroy_image_view(device, view);
  for (gfx::ImageResource& image : mesh_textures)
    gfx::destroy_image(device, image);
  gfx::destroy_buffer(device, cluster_material_buffer);
  gfx::destroy_buffer(device, material_buffer);
  gfx::destroy_buffer(device, triangle_buffer);
  gfx::destroy_buffer(device, vertex_buffer);
  gfx::destroy_buffer(device, mesh_buffer);
  gfx::destroy_buffer(device, quantized_buffer);
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
        "\"mesh_primitives\":%u,"
        "\"cull\":%s,\"occlusion\":%s,\"cone\":%s,\"lod_px\":%.2f,\"raster\":\"%s\",\"sw_px\":%.1f,"
        "\"visible_hw_last\":%u,\"visible_pass2_last\":%u,\"visible_sw_last\":%u,\"visible_min\":%"
        "u,"
        "\"visible_max\":%u,"
        "\"gpu_ms\":{\"cull\":%.4f,\"hw\":%.4f,\"sw\":%.4f,\"hiz\":%.4f,\"resolve\":%.4f,"
        "\"rt\":%.4f,\"trace\":%.4f,\"total\":%.4f,"
        "\"frames\":%llu},\"captured\":%s}\n",
        static_cast<unsigned long long>(rendered), seconds, avg_ms, extent_width, extent_height,
        lod.mesh.clusters.size(),
        lod.level_cluster_counts.empty() ? 0u : lod.level_cluster_counts[0],
        lod.leaf_triangle_count, lod.level_cluster_counts.size(),
        static_cast<f64>(build_ns) / 1.0e6, mesh_primitives, options.cull ? "true" : "false",
        occlusion ? "true" : "false", options.cone ? "true" : "false",
        static_cast<f64>(options.lod_px), raster_name(options.raster),
        static_cast<f64>(options.sw_px), visible_hw_last, visible_pass2_last, visible_sw_last,
        visible_min, visible_max, gpu_cull_ms / n, gpu_hw_ms / n, gpu_sw_ms / n, gpu_hiz_ms / n,
        gpu_resolve_ms / n, gpu_rt_ms / n, gpu_trace_ms / n, gpu_total_ms / n,
        static_cast<unsigned long long>(timed_frames), captured ? "true" : "false");
  }
  log::remove_sink(&stderr_sink);
  return exit_code;
}
