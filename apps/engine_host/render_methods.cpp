#include "render_methods.h"

#include <core/log/log.h>
#include <core/time/time.h>
#include <foundation/bench/machine_state.h>
#include <foundation/image/decode.h>
#include <foundation/image/metrics.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/settings.h>

#include <cmath>
#include <cstring>

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
  if (!renderer::parse_view_layout(in.views, out.views)) {
    error = invalid("views must be single, surround3, or panini; got '" + in.views + "'");
    return false;
  }
  if (in.side_yaw_deg < 0.0f || in.side_yaw_deg > 80.0f) {
    error = invalid("side_yaw_deg must be within 0..80");
    return false;
  }
  if (in.peripheral_lod < 1.0f) {
    error = invalid("peripheral_lod must be at least 1");
    return false;
  }
  if (in.panini_d < 0.0f) {
    error = invalid("panini_d must not be negative");
    return false;
  }
  // The two streaming budgets are MiB and KiB on the wire and bytes in the renderer, which is the
  // same split engine-view's `--page-budget` and `--upload-budget` have: a budget is chosen at the
  // scale of a scene and an upload at the scale of a frame, while the renderer sizes buffers and
  // so counts bytes. The conversion lives here, in the one place the wire meets the module, rather
  // than in `RenderSettings`, so `settings.h` never has to say what unit a caller was thinking in.
  // The bound keeps the kibibyte product inside the u32 the renderer's field is.
  if (in.upload_budget_kib > 1024u * 1024u) {
    error = invalid("upload_budget_kib must be at most 1048576 (1 GiB)");
    return false;
  }
  out.stream = in.stream;
  out.page_budget_bytes = u64{in.page_budget_mib} * 1024u * 1024u;
  out.upload_budget_bytes = in.upload_budget_kib * 1024u;
  out.lod_px = in.lod_px;
  out.sw_px = in.sw_px;
  out.cull = in.cull;
  out.occlusion = in.occlusion;
  out.cone = in.cone;
  out.lights = in.lights;
  out.deform_amplitude = in.deform_amplitude;
  out.rt_templates = in.rt_templates;
  out.side_yaw = radians(in.side_yaw_deg);
  out.panini_d = in.panini_d;
  out.peripheral_lod = in.peripheral_lod;
  return true;
}

// The statistics of a run, including the per-view breakdown. The view set is passed beside them
// because the layout's name and the Panini oversampling factor live there rather than in `Stats`.
void fill_stats(const renderer::Stats& in, const renderer::ViewSet& views,
                protocol::RenderStats& out) {
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
  out.gpu_memory.budget_mib = in.gpu_memory.budget_mib;
  out.gpu_memory.used_mib = in.gpu_memory.used_mib;
  out.gpu_memory.device_local_total_mib = in.gpu_memory.device_local_total_mib;
  out.view_layout = renderer::view_layout_name(views.layout());
  out.oversample = views.oversample();
  // Geometry residency, in the shape and under the key names engine-view's `streaming` summary
  // object uses, so one reader serves both hosts. `from_file` becomes a word rather than a flag
  // because "host" and "file" are two different sources and not the presence or absence of one.
  const renderer::StreamStats& stream = in.stream;
  out.stream.pages_total = stream.pages_total;
  out.stream.pages_resident = stream.pages_resident;
  out.stream.pages_pinned = stream.pages_pinned;
  out.stream.page_slots = stream.page_slots;
  out.stream.pending = stream.pending;
  out.stream.requests = stream.requests;
  out.stream.uploads = stream.uploads;
  out.stream.uploads_bytes = stream.uploads_bytes;
  out.stream.evictions = stream.evictions;
  out.stream.stale = stream.stale;
  out.stream.overflows = stream.overflows;
  out.stream.frames_to_converge = stream.frames_to_converge;
  out.stream.resident_bytes = stream.resident_bytes;
  out.stream.page_bytes = stream.page_bytes;
  out.stream.budget_bytes = stream.budget_bytes;
  out.stream.source = stream.from_file ? "file" : "host";
  out.stream.file_reads = stream.file_reads;
  out.stream.file_bytes = stream.file_bytes;
  out.stream.host_bytes_freed = stream.host_bytes_freed;
  out.stream.load_waits = stream.load_waits;
  out.stream.loads_in_flight = stream.loads_in_flight;
  const f64 timed = in.timed();
  out.views.clear();
  for (u32 v = 0; v < in.view_count; ++v) {
    const renderer::ViewStats& s = in.views[v];
    protocol::RenderViewStats entry;
    entry.x = s.x;
    entry.y = s.y;
    entry.width = s.width;
    entry.height = s.height;
    entry.source_width = s.source_width;
    entry.source_height = s.source_height;
    entry.lod_scale = s.lod_scale;
    entry.shading_rate = s.shading_rate;
    entry.visible_hw = s.visible_hw;
    entry.visible_pass2 = s.visible_pass2;
    entry.visible_sw = s.visible_sw;
    entry.visible_pairs = s.visible_pairs();
    entry.cull_ms = s.gpu_cull / timed;
    entry.hw_ms = s.gpu_hw / timed;
    entry.sw_ms = s.gpu_sw / timed;
    entry.hiz_ms = s.gpu_hiz / timed;
    entry.resolve_ms = s.gpu_resolve / timed;
    entry.deform_ms = s.gpu_deform / timed;
    entry.trace_ms = s.gpu_trace / timed;
    entry.total_ms = s.gpu_sum() / timed;
    out.views.push_back(entry);
  }
}

// One spelling of the machine state for both hosts (foundation/bench/machine_state.h): the
// protocol struct is the same fields as the bench harness's JSON, so a renderer measurement and
// a CPU benchmark carry the same caveat in the same shape.
protocol::MachineState machine_state_of(const bench::MachineState& in) {
  protocol::MachineState out;
  if (in.cpu_valid) {
    out.cpu_total_pct = in.cpu_total_pct;
    out.cpu_own_pct = in.cpu_own_pct;
    out.cpu_others_pct = in.cpu_others_pct;
  }
  if (in.gpu_valid) {
    out.gpu_util_pct = in.gpu_util_pct;
    out.gpu_memory_used_mib = in.gpu_memory_used_mib;
    out.gpu_memory_total_mib = in.gpu_memory_total_mib;
  }
  if (in.session_locked != bench::Tristate::Unknown) {
    out.session_locked = in.session_locked == bench::Tristate::Yes;
  }
  return out;
}

// The schema's reference block into the renderer's settings, with the bounds a caller can get
// wrong. `spp` is capped rather than unbounded because a reference render is minutes of GPU work
// and a typo should not take the host out for an hour.
bool read_reference(const protocol::RenderReference& in, u64 frame,
                    renderer::ReferenceSettings& out, protocol::RpcError& error) {
  out = renderer::ReferenceSettings{};
  if (in.spp == 0 || in.spp > 65536) {
    error = invalid("reference.spp must be within 1..65536");
    return false;
  }
  if (in.bounces > 64) {
    error = invalid("reference.bounces must be at most 64");
    return false;
  }
  if (in.batch == 0 || in.batch > in.spp) {
    error = invalid("reference.batch must be within 1..spp");
    return false;
  }
  out.spp = in.spp;
  out.max_bounces = in.bounces;
  out.finest = in.finest;
  out.seed = in.seed;
  out.batch = in.batch;
  out.frame_index = frame;
  return true;
}

void fill_metrics(const image::ImageMetrics& in, protocol::RenderMetrics& out) {
  out.identical = !std::isfinite(in.psnr);
  if (!out.identical) out.psnr = static_cast<f64>(in.psnr);
  out.ssim = static_cast<f64>(in.ssim);
  out.flip_mean = static_cast<f64>(in.flip_mean);
  out.flip_p95 = static_cast<f64>(in.flip_percentile);
  out.flip_max = static_cast<f64>(in.flip_max);
  out.flip_weighted_mean = static_cast<f64>(in.flip_weighted_mean);
}

// An RGBA8 buffer as an image the metrics take, without a round trip through a file.
image::Image image_of(u32 width, u32 height, const Vector<u8>& rgba) {
  image::Image out;
  out.width = width;
  out.height = height;
  out.channels = 4;
  out.pixels.resize(rgba.size());
  std::memcpy(out.pixels.data(), rgba.data(), rgba.size());
  return out;
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
  // A page has to be able to hold a cluster or two, and a scene made of millions of pages is a
  // typo rather than a request: the same reason `grid` and `grid_instances` are bounded above.
  if (params.page_bytes != 0 &&
      (params.page_bytes < 4096u || params.page_bytes > 64u * 1024u * 1024u)) {
    error = invalid("page_bytes must be 0 (the default target) or within 4096..67108864");
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

  // Which procedural scene an empty mesh path builds. An unknown name is refused rather than
  // silently drawing the terrain, because a corpus scene that asked for the atlas fixture and
  // quietly got the heightfield would pass its threshold and guard nothing.
  renderer::Procedural procedural = renderer::Procedural::heightfield;
  if (!params.procedural.empty() && params.procedural != "heightfield") {
    if (params.procedural == "shredded-atlas") {
      procedural = renderer::Procedural::shredded_atlas;
    } else {
      error = invalid("procedural must be \"heightfield\" or \"shredded-atlas\"");
      return false;
    }
  }

  renderer::SceneDesc desc;
  desc.procedural = procedural;
  desc.heightfield_grid = params.grid;
  desc.grid_instances = params.grid_instances;
  desc.ddc = params.ddc;
  desc.cache = params.cache;
  // Whether loading keeps a page table at all, and what the heightfield's pages are sized at.
  // `SceneDesc::stream` is the load's question and `RenderSettings::stream` the frame's; the
  // scene's *requested* settings answer both, because a `SceneData` outlives a settings change
  // here and a later call that asks to stream a scene loaded without a table gets streaming
  // turned off with a log line instead of a page table it cannot have.
  desc.stream = settings.stream;
  desc.page_bytes = params.page_bytes;
  std::string load_error;
  if (!params.scene.empty()) {
    if (!renderer::read_scene_file(params.scene, desc, load_error)) {
      error = protocol::make_error(protocol::codes::k_io_error, std::move(load_error));
      return false;
    }
  } else {
    desc.meshes.push_back(params.mesh);  // empty: the procedural heightfield
  }

  // Loaded into a local and resolved before the scene is registered, so a mesh that does not
  // load or a device that cannot draw it leaves no half-built scene behind holding an id.
  renderer::SceneData data;
  if (!renderer::load_scene(desc, data, load_error)) {
    error = protocol::make_error(protocol::codes::k_io_error, std::move(load_error));
    return false;
  }
  renderer::ResolvedSettings resolved;
  renderer::resolve_settings(settings, device->features(), &data, resolved);
  const renderer::RenderAvailability availability =
      renderer::check_availability(resolved, device->features());
  if (availability != renderer::RenderAvailability::Ok) {
    error = unavailable(std::string(device->adapter().name) + " " +
                        renderer::availability_message(availability));
    return false;
  }
  RenderHost::Scene* scene = host->add_scene();
  scene->requested = settings;
  scene->resolved = resolved;
  scene->data = std::move(data);

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
  const bool reference = params.integrator == "reference";
  if (!reference && params.integrator != "realtime") {
    error = invalid("integrator takes realtime or reference; got '" + params.integrator + "'");
    return false;
  }
  renderer::ReferenceSettings reference_settings;
  if (reference) {
    // A path tracer writes no visibility buffer, so there are no ids, no depth and no normals to
    // read back: asking for one is an error with a message rather than a buffer of nothing, the
    // same rule `--raster direct` follows.
    for (const std::string& name : params.channels) {
      if (name != "color") {
        error = invalid("a reference capture has only a color channel; got '" + name + "'");
        return false;
      }
    }
    if (!read_reference(params.reference, params.frame, reference_settings, error)) return false;
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
  if (reference) {
    if (!host->ensure_reference(*scene, message, no_device)) {
      error = no_device
                  ? unavailable(std::move(message))
                  : protocol::make_error(protocol::codes::k_internal_error, std::move(message));
      return false;
    }
    renderer::ReferenceFrame picture;
    if (!scene->reference->render(frame.camera, reference_settings, picture, &message)) {
      error = protocol::make_error(protocol::codes::k_internal_error, std::move(message));
      return false;
    }
    // Into the same `CapturedFrame` the real-time path fills, so `write_capture` writes the same
    // file at the same name and a caller comparing the two needs no second code path.
    shot.width = picture.width;
    shot.height = picture.height;
    shot.color = std::move(picture.color);
    out.integrator = "reference";
    out.samples = picture.samples;
    out.trace_ms = picture.trace_ms;
    out.seconds = picture.seconds;
  } else if (!scene->view->capture(frame, channels, shot, &message)) {
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
  fill_stats(scene->view->stats(), scene->view->views(), out.stats);
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
  // Around the run, never inside it: the sampler sleeps for its CPU window and spawns a process
  // for the GPU reading, and neither belongs between two timed frames.
  const bench::MachineState machine_start = bench::sample_machine_state(bench::k_sample_window_ms);

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
  scene->view->sample_gpu_memory();  // after the run: what the card looked like while it ran
  const bench::MachineState machine_end = bench::sample_machine_state(bench::k_sample_window_ms);
  out.width = scene->view->width();
  out.height = scene->view->height();
  out.raster = renderer::raster_name(scene->resolved.settings.raster);
  out.shadows = scene->resolved.shadows ? "rt" : "off";
  fill_stats(scene->view->stats(), scene->view->views(), out.stats);
  out.machine_state.start = machine_state_of(machine_start);
  out.machine_state.end = machine_state_of(machine_end);
  // stdout belongs to the protocol, so the caveat goes to stderr — the same line and the same
  // thresholds the bench harness prints.
  (void)bench::warn_if_busy(bench::worst_of(machine_start, machine_end), bench::QuietThresholds{},
                            stderr);
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

// ---- render.evaluate ---------------------------------------------------------------------------

bool render_evaluate(protocol::Context& ctx, const protocol::RenderEvaluateParams& params,
                     protocol::RenderEvaluateResult& out, protocol::RpcError& error) {
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
  if (!(params.ppd > 0.0f)) {
    error = invalid("ppd must be a positive number of pixels per degree");
    return false;
  }
  renderer::RenderSettings settings = scene->requested;
  if (params.settings.has_value() && !read_settings(*params.settings, settings, error))
    return false;
  renderer::ReferenceSettings reference_settings;
  if (!read_reference(params.reference, params.frame, reference_settings, error)) return false;

  std::string message;
  bool no_device = false;
  if (!host->ensure_renderer(*scene, settings, params.width, params.height, message, no_device) ||
      !host->ensure_reference(*scene, message, no_device)) {
    error = no_device ? unavailable(std::move(message))
                      : protocol::make_error(protocol::codes::k_internal_error, std::move(message));
    return false;
  }

  const bench::MachineState machine_start = bench::sample_machine_state(bench::k_sample_window_ms);
  renderer::FrameDesc frame;
  frame.camera = camera_of(*scene, params.camera, params.orbit);
  frame.frame_index = params.frame;

  // The real-time picture first. `reset_stats` before it, so the statistics returned are this
  // frame's and not the ones a previous call left in the slot.
  scene->view->reset_stats();
  renderer::CapturedFrame realtime;
  const i64 realtime_started = time::monotonic_ns();
  if (!scene->view->capture(frame, renderer::CaptureChannels{}, realtime, &message)) {
    error = invalid(std::move(message));
    return false;
  }
  out.realtime_seconds = static_cast<f64>(time::monotonic_ns() - realtime_started) / 1.0e9;

  // Then the reference of the **same** frame: one camera, one frame number, one set of lights.
  renderer::ReferenceFrame reference;
  if (!scene->reference->render(frame.camera, reference_settings, reference, &message)) {
    error = protocol::make_error(protocol::codes::k_internal_error, std::move(message));
    return false;
  }
  // And, when asked, the same reference at one bounce. The difference between the two
  // comparisons is the indirect light the real-time path does not have, which is what makes a
  // per-scene threshold explicable instead of arbitrary.
  renderer::ReferenceFrame direct;
  if (params.direct_only) {
    renderer::ReferenceSettings direct_settings = reference_settings;
    direct_settings.max_bounces = 1;
    if (!scene->reference->render(frame.camera, direct_settings, direct, &message)) {
      error = protocol::make_error(protocol::codes::k_internal_error, std::move(message));
      return false;
    }
  }
  const bench::MachineState machine_end = bench::sample_machine_state(bench::k_sample_window_ms);

  image::MetricsOptions options;
  options.pixels_per_degree = params.ppd;
  image::FloatImage weights;
  if (params.weights == "center") {
    image::center_weight_map(realtime.width, realtime.height, image::CenterWeightOptions{},
                             weights);
    options.weights = &weights;
  } else if (params.weights != "none" && !params.weights.empty()) {
    error = invalid("weights take center or none; got '" + params.weights + "'");
    return false;
  }
  const image::Image a = image_of(realtime.width, realtime.height, realtime.color);
  const image::Image b = image_of(reference.width, reference.height, reference.color);
  image::ImageMetrics metrics;
  image::FloatImage error_map;
  const i64 metrics_started = time::monotonic_ns();
  if (!image::compare_images(a, b, options, metrics, &error_map, &message)) {
    error = protocol::make_error(protocol::codes::k_internal_error, std::move(message));
    return false;
  }
  fill_metrics(metrics, out.full);
  image::FloatImage direct_map;
  if (params.direct_only) {
    const image::Image c = image_of(direct.width, direct.height, direct.color);
    image::ImageMetrics direct_metrics;
    if (!image::compare_images(a, c, options, direct_metrics, &direct_map, &message)) {
      error = protocol::make_error(protocol::codes::k_internal_error, std::move(message));
      return false;
    }
    protocol::RenderMetrics filled;
    fill_metrics(direct_metrics, filled);
    out.direct = filled;
  }
  out.metrics_ms = static_cast<f64>(time::monotonic_ns() - metrics_started) / 1.0e6;

  // The files. Everything an agent needs to look at the result by eye is on disk beside the
  // numbers, because a FLIP figure with no heat map says where nothing.
  if (!params.out_dir.empty()) {
    const io::Status status = io::make_directories(params.out_dir);
    if (status != io::Status::Ok) {
      error = protocol::make_error(protocol::codes::k_io_error, "cannot create " + params.out_dir +
                                                                    ": " + io::status_name(status));
      return false;
    }
  }
  auto write = [&](const char* channel, const std::string& suffix, u32 width, u32 height,
                   u32 channels, std::span<const u8> pixels) {
    const std::string path = params.out_dir.empty()
                                 ? params.name + suffix
                                 : io::join_path(params.out_dir, params.name + suffix);
    const io::Status status = image::write_png(path, width, height, channels, pixels);
    if (status != io::Status::Ok) {
      error = protocol::make_error(protocol::codes::k_io_error,
                                   "cannot write " + path + ": " + io::status_name(status));
      return false;
    }
    out.files.insert_or_assign(channel, file_uri(path));
    return true;
  };
  image::Image heat;
  if (!write("realtime", ".realtime.png", a.width, a.height, 4,
             std::span<const u8>(a.pixels.data(), a.pixels.size())) ||
      !write("reference", ".reference.png", b.width, b.height, 4,
             std::span<const u8>(b.pixels.data(), b.pixels.size())) ||
      !image::flip_heat_map(error_map, heat) ||
      !write("flip", ".flip.png", heat.width, heat.height, heat.channels,
             std::span<const u8>(heat.pixels.data(), heat.pixels.size()))) {
    if (error.code == 0) {
      error =
          protocol::make_error(protocol::codes::k_internal_error, "cannot build the FLIP heat map");
    }
    return false;
  }
  if (params.direct_only) {
    image::Image direct_heat;
    if (!write("reference_direct", ".reference_direct.png", direct.width, direct.height, 4,
               std::span<const u8>(direct.color.data(), direct.color.size())) ||
        !image::flip_heat_map(direct_map, direct_heat) ||
        !write("flip_direct", ".flip_direct.png", direct_heat.width, direct_heat.height,
               direct_heat.channels,
               std::span<const u8>(direct_heat.pixels.data(), direct_heat.pixels.size()))) {
      if (error.code == 0) {
        error = protocol::make_error(protocol::codes::k_internal_error,
                                     "cannot build the FLIP heat map");
      }
      return false;
    }
  }

  out.width = realtime.width;
  out.height = realtime.height;
  out.samples = reference.samples;
  out.reference_trace_ms = reference.trace_ms;
  out.reference_seconds = reference.seconds + direct.seconds;
  fill_stats(scene->view->stats(), scene->view->views(), out.stats);
  out.machine_state.start = machine_state_of(machine_start);
  out.machine_state.end = machine_state_of(machine_end);
  (void)bench::warn_if_busy(bench::worst_of(machine_start, machine_end), bench::QuietThresholds{},
                            stderr);
  return true;
}

}  // namespace

RenderHost::~RenderHost() {
  // The renderers hold pipelines and the scenes hold buffers; both must go before the device.
  for (const std::unique_ptr<Scene>& scene : scenes_) {
    scene->reference.reset();  // it holds the renderer and the scene, so it goes first
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
  // `SceneRenderer::Desc::page_source` is left null, so a streamed scene's pages are copied out of
  // the `SceneData` this host keeps rather than read back out of the meshes' containers. That is
  // deliberate and it is not a "not done yet": `renderer::attach_page_source` **releases** the
  // merged host streams as the other half of what it does, and those streams are exactly what this
  // function rebuilds a `GpuScene` from when a later call changes the settings. engine-view can
  // release them because it builds its scene once; a host whose `render.benchmark` may be handed
  // `{"stream":false}` for the scene it just loaded with `{"stream":true}` cannot, because the
  // rebuild would upload buffers that are no longer there. Attaching one here means either
  // refusing that rebuild or re-opening the source against each new `GpuScene`, and neither is
  // worth doing untested — every scene this host can build without a content fixture is the
  // procedural heightfield, which has no container behind it and would be refused a file source
  // anyway. docs/subsystems/protocol.md says so under "Not yet".
  renderer::SceneRenderer::Desc desc;
  desc.width = width;
  desc.height = height;
  desc.offscreen = true;
  // The layout the settings asked for, over the offscreen target. It must be the one
  // `resolve_settings` saw just above, because the scene's per-frame working set is sized by the
  // view count; both come from the same `RenderSettings`, which is what keeps them in step.
  desc.views.layout = scene.resolved.settings.views;
  desc.views.surround.side_yaw = scene.resolved.settings.side_yaw;
  desc.views.panini_d = scene.resolved.settings.panini_d;
  desc.views.peripheral_lod = scene.resolved.settings.peripheral_lod;
  if (!view->create(*dev, *gpu, scene.resolved, desc, &error)) return false;
  scene.gpu = std::move(gpu);
  scene.view = std::move(view);
  scene.reference.reset();  // it holds the renderer that was just replaced
  scene.built = settings;
  return true;
}

bool RenderHost::ensure_reference(Scene& scene, std::string& error, bool& unavailable) {
  unavailable = false;
  if (scene.view == nullptr) {
    error = "the renderer has not been built; call ensure_renderer first";
    return false;
  }
  if (scene.reference != nullptr) return scene.reference->resize(&error);
  gfx::Device* dev = device(0, error);
  if (dev == nullptr) {
    unavailable = true;
    return false;
  }
  std::string why;
  if (!renderer::reference_available(scene.resolved, dev->features(), &why)) {
    error = std::string(dev->adapter().name) + " " + why;
    unavailable = true;
    return false;
  }
  auto reference = std::make_unique<renderer::ReferenceRenderer>();
  if (!reference->create(*dev, *scene.gpu, *scene.view, {}, &error)) return false;
  scene.reference = std::move(reference);
  return true;
}

void add_render_methods(protocol::Dispatcher& d) {
  d.add(protocol::method<protocol::RenderLoadParams, protocol::RenderSceneInfo, &render_load>(
      "render.load",
      "Load a mesh or a scene file into a GPU scene the session holds; returns its id and "
      "counts. `settings.stream` also keeps the scene's streaming page table, which `page_bytes` "
      "sizes for the procedural heightfield."));
  d.add(protocol::method<protocol::RenderCaptureParams, protocol::RenderCaptureResult,
                         &render_capture>(
      "render.capture",
      "Render one frame of a loaded scene offscreen and write the asked-for channels (color, "
      "ids, depth, normals) as files."));
  d.add(protocol::method<protocol::RenderBenchmarkParams, protocol::RenderBenchmarkResult,
                         &render_benchmark>(
      "render.benchmark",
      "Render a loaded scene for n frames with the camera held still; returns GPU milliseconds "
      "per pass, CPU milliseconds per frame, and the run's geometry residency in stats.stream."));
  d.add(protocol::method<protocol::RenderCompareParams, protocol::RenderCompareResult,
                         &render_compare>(
      "render.compare", "FLIP, PSNR, and SSIM between two images, with an optional heat map."));
  d.add(protocol::method<protocol::RenderEvaluateParams, protocol::RenderEvaluateResult,
                         &render_evaluate>(
      "render.evaluate",
      "Render one frame of a loaded scene through the real-time path and through the reference "
      "path tracer, compare them, write both pictures and the FLIP heat map, and return the "
      "numbers and the times: plan 04 section 4.8's optimization loop in one call."));
}

}  // namespace engine::host
