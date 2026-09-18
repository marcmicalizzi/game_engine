#include "render_methods.h"

#include <core/log/log.h>
#include <core/time/time.h>
#include <foundation/image/decode.h>
#include <foundation/image/metrics.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/settings.h>

#include <cmath>

namespace engine::host {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_render, "host.render");

RenderHost* host_of(protocol::Context& ctx) { return static_cast<RenderHost*>(ctx.app); }

protocol::RpcError unavailable(std::string message) {
  return protocol::make_error(protocol::codes::k_render_unavailable, std::move(message));
}

protocol::RpcError invalid(std::string message) {
  return protocol::make_error(protocol::codes::k_invalid_argument, std::move(message));
}

// A `file://` URI of a native path, which is what a client needs to open the file without
// guessing at the host's path syntax. Backslashes become forward slashes, a Windows drive path
// gets the third slash, and the characters a URI cannot carry raw are percent-encoded.
std::string file_uri(std::string_view path) {
  std::string out = path.starts_with("/") ? "file://" : "file:///";
  for (const char c : path) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (c == '\\') {
      out.push_back('/');
    } else if (u <= 0x20 || u >= 0x7f || c == '"' || c == '#' || c == '%' || c == '<' || c == '>' ||
               c == '?' || c == '{' || c == '}' || c == '|' || c == '^' || c == '`') {
      char escape[4] = {};
      std::snprintf(escape, sizeof(escape), "%%%02X", u);
      out += escape;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// The schema's strings into the renderer's settings, reporting the first one that is not a
// spelling either host knows. The parsers are the renderer's, so a flag and a protocol field
// can never drift apart.
bool read_settings(const protocol::RenderSettings& in, renderer::RenderSettings& out,
                   protocol::RpcError& error) {
  out = renderer::RenderSettings{};
  if (!renderer::parse_raster_mode(in.raster, out.raster)) {
    error = invalid("raster must be direct, hw, vertex, sw, auto, or rt; got '" + in.raster + "'");
    return false;
  }
  if (!renderer::parse_shadow_mode(in.shadows, out.shadows)) {
    error = invalid("shadows must be off, rt, or auto; got '" + in.shadows + "'");
    return false;
  }
  if (!renderer::parse_view_mode(in.view, out.view_mode)) {
    error = invalid("view must be id, tri, depth, shaded, normals, or uv; got '" + in.view + "'");
    return false;
  }
  if (!renderer::parse_deform_mode(in.deform, out.deform, out.deform_kind)) {
    error = invalid("deform must be none, identity, wave, or lattice; got '" + in.deform + "'");
    return false;
  }
  out.lod_px = in.lod_px;
  out.sw_px = in.sw_px;
  out.cull = in.cull;
  out.occlusion = in.occlusion;
  out.cone = in.cone;
  out.lights = in.lights;
  out.deform_amplitude = in.deform_amplitude;
  out.rt_templates = in.rt_templates;
  return true;
}

void fill_stats(const renderer::Stats& in, protocol::RenderStats& out) {
  out.frames = in.frames;
  out.visible_hw = in.visible_hw;
  out.visible_pass2 = in.visible_pass2;
  out.visible_sw = in.visible_sw;
  out.visible_pairs = in.visible_pairs();
  out.visible_min = in.visible_min == ~u32{0} ? 0u : in.visible_min;
  out.visible_max = in.visible_max;
  out.cpu_ms_per_frame = in.cpu_ms_per_frame();
  out.gpu_ms.cull = in.cull_ms();
  out.gpu_ms.hw = in.hw_ms();
  out.gpu_ms.sw = in.sw_ms();
  out.gpu_ms.hiz = in.hiz_ms();
  out.gpu_ms.resolve = in.resolve_ms();
  out.gpu_ms.rt = in.rt_ms();
  out.gpu_ms.clas = in.clas_ms();
  out.gpu_ms.deform = in.deform_ms();
  out.gpu_ms.trace = in.trace_ms();
  out.gpu_ms.total = in.total_ms();
  out.gpu_ms.frames = in.timed_frames;
}

renderer::Camera camera_of(const RenderHost::Scene& scene,
                           const std::optional<protocol::RenderCamera>& explicit_camera,
                           const std::optional<protocol::RenderOrbit>& orbit) {
  if (explicit_camera.has_value()) {
    renderer::Camera camera;
    camera.position = explicit_camera->position;
    camera.target = explicit_camera->target;
    camera.fov_y = radians(explicit_camera->fov_deg);
    camera.znear =
        explicit_camera->znear > 0.0f ? explicit_camera->znear : 0.01f * scene.data.radius;
    return camera;
  }
  const protocol::RenderOrbit o = orbit.has_value() ? *orbit : protocol::RenderOrbit{};
  return renderer::orbit_camera_at(scene.data.center, scene.data.radius, o.distance,
                                   radians(o.yaw_deg), radians(o.pitch_deg));
}

// ---- render.load -------------------------------------------------------------------------------

bool render_load(protocol::Context& ctx, const protocol::RenderLoadParams& params,
                 protocol::RenderSceneInfo& out, protocol::RpcError& error) {
  RenderHost* host = host_of(ctx);
  if (host == nullptr) {
    error = unavailable("this host has no renderer attached");
    return false;
  }
  if (!params.mesh.empty() && !params.scene.empty()) {
    error = invalid("name a mesh or a scene file, not both");
    return false;
  }
  if (params.grid_instances > 64) {
    error = invalid("grid_instances must be at most 64");
    return false;
  }
  if (params.grid < 2 || params.grid > 2048) {
    error = invalid("grid must be within 2..2048");
    return false;
  }
  renderer::RenderSettings settings;
  if (!read_settings(params.settings, settings, error)) return false;

  std::string device_error;
  gfx::Device* device = host->device(params.adapter, device_error);
  if (device == nullptr) {
    error = unavailable(std::move(device_error));
    return false;
  }

  renderer::SceneDesc desc;
  desc.heightfield_grid = params.grid;
  desc.grid_instances = params.grid_instances;
  desc.ddc = params.ddc;
  desc.cache = params.cache;
  std::string load_error;
  if (!params.scene.empty()) {
    if (!renderer::read_scene_file(params.scene, desc, load_error)) {
      error = protocol::make_error(protocol::codes::k_io_error, std::move(load_error));
      return false;
    }
  } else {
    desc.meshes.push_back(params.mesh);  // empty: the procedural heightfield
  }

  RenderHost::Scene* scene = host->add_scene();
  scene->requested = settings;
  if (!renderer::load_scene(desc, scene->data, load_error)) {
    error = protocol::make_error(protocol::codes::k_io_error, std::move(load_error));
    return false;
  }
  renderer::resolve_settings(settings, device->features(), &scene->data, scene->resolved);
  const renderer::RenderAvailability availability =
      renderer::check_availability(scene->resolved, device->features());
  if (availability != renderer::RenderAvailability::Ok) {
    error = unavailable(std::string(device->adapter().name) + " " +
                        renderer::availability_message(availability));
    return false;
  }

  out.scene = scene->id;
  out.adapter = device->adapter().name;
  out.meshes = scene->data.parts.size();
  out.instances = scene->data.instances.size();
  out.pairs = scene->data.pair_count;
  out.clusters = scene->data.cluster_count();
  out.leaf_clusters = scene->data.leaf_count();
  out.triangles = scene->data.lod.leaf_triangle_count;
  out.lod_levels = scene->data.lod.level_cluster_counts.size();
  out.mesh_primitives = scene->data.mesh_primitives;
  out.mesh_cache = scene->data.mesh_cache;
  out.build_ms = static_cast<f64>(scene->data.build_ns) / 1.0e6;
  out.center = scene->data.center;
  out.radius = scene->data.radius;
  out.raster = renderer::raster_name(scene->resolved.settings.raster);
  out.shadows = scene->resolved.shadows ? "rt" : "off";
  out.cull = scene->resolved.settings.cull;
  out.occlusion = scene->resolved.occlusion;
  out.deform = renderer::deform_name(scene->resolved.settings);
  out.rt_templates = scene->resolved.settings.rt_templates;
  ENGINE_LOG_INFO(log_render, "scene loaded", log::field("scene", scene->id),
                  log::field("clusters", out.clusters), log::field("instances", out.instances),
                  log::field("pairs", out.pairs), log::field("cache", out.mesh_cache));
  return true;
}

// ---- render.capture ----------------------------------------------------------------------------

bool render_capture(protocol::Context& ctx, const protocol::RenderCaptureParams& params,
                    protocol::RenderCaptureResult& out, protocol::RpcError& error) {
  RenderHost* host = host_of(ctx);
  RenderHost::Scene* scene = host != nullptr ? host->find(params.scene) : nullptr;
  if (scene == nullptr) {
    error = protocol::make_error(protocol::codes::k_not_found, "no scene " + params.scene);
    return false;
  }
  if (params.width == 0 || params.height == 0 || params.width > 16384 || params.height > 16384) {
    error = invalid("width and height must be within 1..16384");
    return false;
  }
  renderer::CaptureChannels channels;
  if (params.channels.empty()) {
    channels.color = true;
  } else {
    channels = renderer::CaptureChannels{false, false, false, false};
    for (const std::string& name : params.channels) {
      if (name == "color") {
        channels.color = true;
      } else if (name == "ids") {
        channels.ids = true;
      } else if (name == "depth") {
        channels.depth = true;
      } else if (name == "normals") {
        channels.normals = true;
      } else {
        error = invalid("channels take color, ids, depth, or normals; got '" + name + "'");
        return false;
      }
    }
  }
  std::string message;
  bool no_device = false;
  if (!host->ensure_renderer(*scene, scene->requested, params.width, params.height, message,
                             no_device)) {
    error = no_device ? unavailable(std::move(message))
                      : protocol::make_error(protocol::codes::k_internal_error, std::move(message));
    return false;
  }

  renderer::FrameDesc frame;
  frame.camera = camera_of(*scene, params.camera, params.orbit);
  frame.frame_index = params.frame;
  renderer::CapturedFrame shot;
  scene->view->reset_stats();
  if (!scene->view->capture(frame, channels, shot, &message)) {
    error = invalid(std::move(message));
    return false;
  }
  renderer::CaptureFiles files;
  if (!renderer::write_capture(params.out_dir, params.name, shot, files, message)) {
    error = protocol::make_error(protocol::codes::k_io_error, std::move(message));
    return false;
  }
  out.width = shot.width;
  out.height = shot.height;
  out.covered = shot.covered;
  out.depth_min = shot.depth_min;
  out.depth_max = shot.depth_max;
  if (!files.color.empty()) out.files.insert_or_assign("color", file_uri(files.color));
  if (!files.ids.empty()) {
    out.files.insert_or_assign("ids", file_uri(files.ids));
    out.files.insert_or_assign("ids_header", file_uri(files.ids_header));
    out.ids_header = renderer::id_buffer_header(shot, io::file_name(files.ids));
  }
  if (!files.depth.empty()) out.files.insert_or_assign("depth", file_uri(files.depth));
  if (!files.normals.empty()) out.files.insert_or_assign("normals", file_uri(files.normals));
  fill_stats(scene->view->stats(), out.stats);
  return true;
}

// ---- render.benchmark --------------------------------------------------------------------------

bool render_benchmark(protocol::Context& ctx, const protocol::RenderBenchmarkParams& params,
                      protocol::RenderBenchmarkResult& out, protocol::RpcError& error) {
  RenderHost* host = host_of(ctx);
  RenderHost::Scene* scene = host != nullptr ? host->find(params.scene) : nullptr;
  if (scene == nullptr) {
    error = protocol::make_error(protocol::codes::k_not_found, "no scene " + params.scene);
    return false;
  }
  if (params.width == 0 || params.height == 0 || params.width > 16384 || params.height > 16384) {
    error = invalid("width and height must be within 1..16384");
    return false;
  }
  if (params.frames == 0 || params.frames > 100000) {
    error = invalid("frames must be within 1..100000");
    return false;
  }
  renderer::RenderSettings settings = scene->requested;
  if (params.settings.has_value() && !read_settings(*params.settings, settings, error))
    return false;
  std::string message;
  bool no_device = false;
  if (!host->ensure_renderer(*scene, settings, params.width, params.height, message, no_device)) {
    error = no_device ? unavailable(std::move(message))
                      : protocol::make_error(protocol::codes::k_internal_error, std::move(message));
    return false;
  }

  // The same loop engine-view runs, minus the present: the frames stay in flight, because a
  // benchmark that waited for each one would measure the wait. The camera is held still so the
  // LOD cut is the same every frame and the numbers are comparable between runs; the frame
  // number still advances, so the lights orbit and a deformed scene deforms.
  const renderer::Camera camera = camera_of(*scene, params.camera, params.orbit);
  scene->view->reset_stats();
  const i64 started = time::monotonic_ns();
  u64 last = 0;
  for (u32 i = 0; i < params.frames; ++i) {
    renderer::FrameDesc frame;
    frame.camera = camera;
    frame.frame_index = i;
    scene->view->begin_frame();
    last = scene->view->submit_frame(frame, &message);
    if (last == 0) {
      error = protocol::make_error(protocol::codes::k_internal_error, std::move(message));
      return false;
    }
  }
  scene->view->wait(last);
  scene->view->collect_visible();
  out.seconds = static_cast<f64>(time::monotonic_ns() - started) / 1.0e9;
  out.width = scene->view->width();
  out.height = scene->view->height();
  out.raster = renderer::raster_name(scene->resolved.settings.raster);
  out.shadows = scene->resolved.shadows ? "rt" : "off";
  fill_stats(scene->view->stats(), out.stats);
  return true;
}

// ---- render.compare ----------------------------------------------------------------------------

bool render_compare(protocol::Context&, const protocol::RenderCompareParams& params,
                    protocol::RenderCompareResult& out, protocol::RpcError& error) {
  image::Image a;
  image::Image b;
  std::string message;
  if (image::read_image(params.a, a, 4, &message) != io::Status::Ok) {
    error =
        protocol::make_error(protocol::codes::k_io_error,
                             "cannot read " + params.a + (message.empty() ? "" : ": " + message));
    return false;
  }
  if (image::read_image(params.b, b, 4, &message) != io::Status::Ok) {
    error =
        protocol::make_error(protocol::codes::k_io_error,
                             "cannot read " + params.b + (message.empty() ? "" : ": " + message));
    return false;
  }
  if (a.width != b.width || a.height != b.height) {
    error = invalid("the images are " + std::to_string(a.width) + "x" + std::to_string(a.height) +
                    " and " + std::to_string(b.width) + "x" + std::to_string(b.height) +
                    "; a comparison needs the same size");
    return false;
  }
  image::MetricsOptions options;
  options.pixels_per_degree = params.ppd;
  image::FloatImage weights;
  if (params.weights == "center") {
    image::center_weight_map(a.width, a.height, image::CenterWeightOptions{}, weights);
    options.weights = &weights;
  } else if (params.weights != "none" && !params.weights.empty()) {
    error = invalid("weights take center or none; got '" + params.weights + "'");
    return false;
  }
  if (!(params.ppd > 0.0f)) {
    error = invalid("ppd must be a positive number of pixels per degree");
    return false;
  }
  image::ImageMetrics metrics;
  image::FloatImage error_map;
  const i64 started = time::monotonic_ns();
  if (!image::compare_images(a, b, options, metrics, &error_map, &message)) {
    error = protocol::make_error(protocol::codes::k_internal_error, std::move(message));
    return false;
  }
  out.ms = static_cast<f64>(time::monotonic_ns() - started) / 1.0e6;
  out.width = a.width;
  out.height = a.height;
  // Two identical images have infinite PSNR, and JSON has no infinity: the field is null and
  // `identical` says so, which a reader can tell apart from a very large number that only means
  // "almost identical".
  out.identical = !std::isfinite(metrics.psnr);
  if (!out.identical) out.psnr = static_cast<f64>(metrics.psnr);
  out.ssim = static_cast<f64>(metrics.ssim);
  out.flip_mean = static_cast<f64>(metrics.flip_mean);
  out.flip_p95 = static_cast<f64>(metrics.flip_percentile);
  out.flip_max = static_cast<f64>(metrics.flip_max);
  out.flip_weighted_mean = static_cast<f64>(metrics.flip_weighted_mean);
  if (!params.flip_out.empty()) {
    image::Image heat;
    if (!image::flip_heat_map(error_map, heat)) {
      error =
          protocol::make_error(protocol::codes::k_internal_error, "cannot build the FLIP heat map");
      return false;
    }
    const io::Status status =
        image::write_png(params.flip_out, heat.width, heat.height, heat.channels,
                         std::span<const u8>(heat.pixels.data(), heat.pixels.size()));
    if (status != io::Status::Ok) {
      error = protocol::make_error(protocol::codes::k_io_error, "cannot write " + params.flip_out +
                                                                    ": " + io::status_name(status));
      return false;
    }
    out.flip_image = file_uri(params.flip_out);
  }
  return true;
}

}  // namespace

RenderHost::~RenderHost() {
  // The renderers hold pipelines and the scenes hold buffers; both must go before the device.
  for (const std::unique_ptr<Scene>& scene : scenes_) {
    scene->view.reset();
    scene->gpu.reset();
  }
  scenes_.clear();
  if (device_ready_) device_.destroy();
}

gfx::Device* RenderHost::device(u32 adapter, std::string& error) {
  if (device_ready_) return &device_;
  if (device_failed_) {
    error = device_error_;
    return nullptr;
  }
  gfx::DeviceOptions options;
  options.adapter_index = adapter;  // no surface extensions: this process presents nothing
  if (!device_.create(options, &device_error_)) {
    device_failed_ = true;
    error = device_error_;
    ENGINE_LOG_WARN(log_render, "no Vulkan device", log::field("error", device_error_));
    return nullptr;
  }
  device_ready_ = true;
  ENGINE_LOG_INFO(log_render, "render device", log::field("adapter", device_.adapter().name),
                  log::field("mesh_shader", device_.features().mesh_shader),
                  log::field("cluster_as", device_.features().cluster_acceleration_structure));
  return &device_;
}

RenderHost::Scene* RenderHost::add_scene() {
  auto scene = std::make_unique<Scene>();
  scene->id = "scene" + std::to_string(next_id_++);
  scenes_.push_back(std::move(scene));
  return scenes_.back().get();
}

RenderHost::Scene* RenderHost::find(std::string_view id) noexcept {
  for (const std::unique_ptr<Scene>& scene : scenes_) {
    if (scene->id == id) return scene.get();
  }
  return nullptr;
}

bool RenderHost::ensure_renderer(Scene& scene, const renderer::RenderSettings& settings, u32 width,
                                 u32 height, std::string& error, bool& unavailable) {
  unavailable = false;
  gfx::Device* dev = device(0, error);
  if (dev == nullptr) {
    unavailable = true;
    return false;
  }
  if (scene.view != nullptr && scene.built == settings) {
    // Only the frame size changed: the screen-sized targets are the renderer's to recreate, and
    // the scene-sized buffers and the pipelines stay.
    return scene.view->resize(width, height, &error);
  }
  scene.view.reset();
  scene.gpu.reset();
  renderer::resolve_settings(settings, dev->features(), &scene.data, scene.resolved);
  const renderer::RenderAvailability availability =
      renderer::check_availability(scene.resolved, dev->features());
  if (availability != renderer::RenderAvailability::Ok) {
    error = std::string(dev->adapter().name) + " " + renderer::availability_message(availability);
    unavailable = true;  // a machine that cannot, not a caller that asked wrongly
    return false;
  }
  auto gpu = std::make_unique<renderer::GpuScene>();
  if (!gpu->create(*dev, scene.data, scene.resolved, &error)) return false;
  auto view = std::make_unique<renderer::SceneRenderer>();
  renderer::SceneRenderer::Desc desc;
  desc.width = width;
  desc.height = height;
  desc.offscreen = true;
  if (!view->create(*dev, *gpu, scene.resolved, desc, &error)) return false;
  scene.gpu = std::move(gpu);
  scene.view = std::move(view);
  scene.built = settings;
  scene.width = width;
  scene.height = height;
  return true;
}

void add_render_methods(protocol::Dispatcher& d) {
  d.add(protocol::method<protocol::RenderLoadParams, protocol::RenderSceneInfo, &render_load>(
      "render.load",
      "Load a mesh or a scene file into a GPU scene the session holds; returns its id and "
      "counts."));
  d.add(protocol::method<protocol::RenderCaptureParams, protocol::RenderCaptureResult,
                         &render_capture>(
      "render.capture",
      "Render one frame of a loaded scene offscreen and write the asked-for channels (color, "
      "ids, depth, normals) as files."));
  d.add(protocol::method<protocol::RenderBenchmarkParams, protocol::RenderBenchmarkResult,
                         &render_benchmark>(
      "render.benchmark",
      "Render a loaded scene for n frames with the camera held still; returns GPU milliseconds "
      "per pass and CPU milliseconds per frame."));
  d.add(protocol::method<protocol::RenderCompareParams, protocol::RenderCompareResult,
                         &render_compare>(
      "render.compare", "FLIP, PSNR, and SSIM between two images, with an optional heat map."));
}

}  // namespace engine::host
