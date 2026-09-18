#include <core/base/assert.h>
#include <core/platform/process.h>
#include <core/time/time.h>
#include <domain/gfx/capture.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/scene_renderer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <renderer_log.h>
#include <shaders/clas_records.spv.h>
#include <shaders/cluster_cull.spv.h>
#include <shaders/cluster_mesh.spv.h>
#include <shaders/cluster_sw_raster.spv.h>
#include <shaders/cluster_vertex.spv.h>
#include <shaders/deform.spv.h>
#include <shaders/hiz_build.spv.h>
#include <shaders/ray_visibility.spv.h>
#include <shaders/visibility_resolve.spv.h>
#include <shaders/visibility_resolve_rt.spv.h>

namespace engine::renderer {

namespace {

constexpr u32 k_view_lights = 2;  // the warm and cool point lights orbiting the scene

// Copies a device buffer into host memory through a staging buffer and a blocking submission.
// Only a capture does this; a frame never reads anything back.
bool read_buffer(const gfx::Device& device, const gfx::BufferResource& source, u64 bytes,
                 Vector<u8>& out, std::string* error) {
  gfx::BufferResource staging;
  if (!gfx::create_buffer(device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, staging, error))
    return false;
  const bool ok = gfx::submit_immediate(
      device,
      [&](VkCommandBuffer cb) {
        const VkBufferCopy copy{0, 0, bytes};
        vkCmdCopyBuffer(cb, source.buffer, staging.buffer, 1, &copy);
      },
      error);
  if (ok) {
    out.resize(static_cast<u32>(bytes));
    std::memcpy(out.data(), staging.mapped, bytes);
  }
  gfx::destroy_buffer(device, staging);
  return ok;
}

}  // namespace

Camera orbit_camera(const Vec3& center, f32 radius, f32 distance, u64 frame) noexcept {
  const f32 angle = static_cast<f32>(frame) * 0.006f;
  const f32 d =
      (distance > 0.0f ? distance : 22.0f + 14.0f * std::sin(static_cast<f32>(frame) * 0.004f)) *
      (radius / 10.0f);
  Camera camera;
  camera.position = center + Vec3{std::cos(angle) * d, 0.45f * d, std::sin(angle) * d};
  camera.target = center;
  camera.fov_y = radians(55.0f);
  camera.znear = 0.01f * radius;  // reversed-Z: 0.1 for the heightfield, 0.2 mm for a 2 cm mesh
  return camera;
}

Camera orbit_camera_at(const Vec3& center, f32 radius, f32 distance, f32 yaw, f32 pitch) noexcept {
  const f32 d = (distance > 0.0f ? distance : 22.0f) * (radius / 10.0f);
  // `pitch` is the elevation above the orbit circle of radius d, so the default (atan(0.45))
  // reproduces engine-view's fixed 0.45 height factor exactly and `--orbit 22` and
  // `orbit {distance: 22}` are the same camera. Clamped short of the pole, where the tangent and
  // the up vector both stop meaning anything.
  const f32 limit = radians(85.0f);
  const f32 clamped = pitch < -limit ? -limit : (pitch > limit ? limit : pitch);
  Camera camera;
  camera.position = center + Vec3{std::cos(yaw) * d, std::tan(clamped) * d, std::sin(yaw) * d};
  camera.target = center;
  camera.fov_y = radians(55.0f);
  camera.znear = 0.01f * radius;
  return camera;
}

void SceneRenderer::Pipelines::destroy(const gfx::Device& device) noexcept {
  if (direct != VK_NULL_HANDLE) gfx::destroy_pipeline(device, direct);
  if (hardware != VK_NULL_HANDLE) gfx::destroy_pipeline(device, hardware);
  if (vertex != VK_NULL_HANDLE) gfx::destroy_pipeline(device, vertex);
  if (resolve != VK_NULL_HANDLE) gfx::destroy_pipeline(device, resolve);
  gfx::destroy_compute_pipeline(device, software);
  gfx::destroy_compute_pipeline(device, cull);
  gfx::destroy_compute_pipeline(device, deform);
  gfx::destroy_compute_pipeline(device, hiz);
  gfx::destroy_compute_pipeline(device, records);
  gfx::destroy_compute_pipeline(device, record_ranges);
  gfx::destroy_compute_pipeline(device, record_emit);
  gfx::destroy_compute_pipeline(device, trace);
  direct = hardware = vertex = resolve = VK_NULL_HANDLE;
}

bool SceneRenderer::Targets::create(const gfx::Device& device, u32 w, u32 h, std::string* error) {
  destroy(device);
  width = w;
  height = h;
  hiz_mips = gfx::hiz_mip_count(w, h);
  const u32 hiz_elements = gfx::hiz_layout(w, h, hiz_offsets);
  hiz_levels.resize(hiz_mips * 2);
  hiz_dirty = true;
  // TRANSFER_SRC on the visibility buffer is what a capture's id and depth channels read back;
  // nothing in a frame ever copies from it.
  constexpr VkBufferUsageFlags k_buffer_usage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  return gfx::create_image_2d(device, w, h, VK_FORMAT_D32_SFLOAT,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, depth, error) &&
         gfx::create_buffer(device, u64{w} * h * sizeof(u64), k_buffer_usage, false, vis, error) &&
         gfx::create_buffer(device, u64{hiz_elements} * sizeof(f32), k_buffer_usage, false, hiz,
                            error);
}

void SceneRenderer::Targets::destroy(const gfx::Device& device) noexcept {
  if (depth.image != VK_NULL_HANDLE) gfx::destroy_image(device, depth);
  gfx::destroy_buffer(device, vis);
  gfx::destroy_buffer(device, hiz);
  depth = gfx::ImageResource{};
  vis = gfx::BufferResource{};
  hiz = gfx::BufferResource{};
}

SceneRenderer::~SceneRenderer() { destroy(); }

bool SceneRenderer::create(const gfx::Device& device, GpuScene& scene,
                           const ResolvedSettings& resolved, const Desc& desc, std::string* error) {
  destroy();
  device_ = &device;
  scene_ = &scene;
  resolved_ = resolved;
  desc_ = desc;
  width_ = desc.width;
  height_ = desc.height;

  if (!frames_.create(device, desc.frames_in_flight, error) ||
      !timer_.create(device, desc.frames_in_flight, 24, error)) {
    destroy();
    return false;
  }
  params_.resize(desc.frames_in_flight);
  resolves_.resize(desc.frames_in_flight);
  stat_blocks_.resize(desc.frames_in_flight);
  ray_params_.resize(desc.frames_in_flight);
  constexpr VkBufferUsageFlags k_address =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  bool ok = true;
  for (u32 slot = 0; slot < desc.frames_in_flight && ok; ++slot) {
    ok = gfx::create_buffer(device, sizeof(gfx::CullParams) * 2, k_address, true, params_[slot],
                            error) &&
         gfx::create_buffer(device,
                            sizeof(gfx::ResolveParams) + k_view_lights * sizeof(gfx::ResolveLight),
                            k_address, true, resolves_[slot], error) &&
         gfx::create_buffer(device, sizeof(u32) * 9, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                            stat_blocks_[slot], error);
    if (ok) std::memset(stat_blocks_[slot].mapped, 0, sizeof(u32) * 9);
    if (ok && resolved_.ray_path) {
      ok = gfx::create_buffer(device, sizeof(gfx::RayVisibilityParams), k_address, true,
                              ray_params_[slot], error);
    }
  }
  if (!ok) {
    destroy();
    return false;
  }

  // Shaders: the embedded copies always work; the build's manifest, when found, loads the same
  // shaders from their files and recompiles them when the .slang sources change while running.
  if (!shaders_.create(&device, error)) {
    destroy();
    return false;
  }
  shaders_.add_embedded("cluster_mesh", shaders::k_cluster_mesh_spirv,
                        shaders::k_cluster_mesh_spirv_size);
  shaders_.add_embedded("cluster_cull", shaders::k_cluster_cull_spirv,
                        shaders::k_cluster_cull_spirv_size);
  shaders_.add_embedded("cluster_sw_raster", shaders::k_cluster_sw_raster_spirv,
                        shaders::k_cluster_sw_raster_spirv_size);
  shaders_.add_embedded("hiz_build", shaders::k_hiz_build_spirv, shaders::k_hiz_build_spirv_size);
  shaders_.add_embedded("cluster_vertex", shaders::k_cluster_vertex_spirv,
                        shaders::k_cluster_vertex_spirv_size);
  shaders_.add_embedded("visibility_resolve", shaders::k_visibility_resolve_spirv,
                        shaders::k_visibility_resolve_spirv_size);
  // The same resolve with the shadow rays in; only a device with acceleration structures may
  // draw with it, because its `g_scenes[]` is binding 3 of the bindless set.
  shaders_.add_embedded("visibility_resolve_rt", shaders::k_visibility_resolve_rt_spirv,
                        shaders::k_visibility_resolve_rt_spirv_size);
  shaders_.add_embedded("clas_records", shaders::k_clas_records_spirv,
                        shaders::k_clas_records_spirv_size);
  shaders_.add_embedded("ray_visibility", shaders::k_ray_visibility_spirv,
                        shaders::k_ray_visibility_spirv_size);
  shaders_.add_embedded("deform", shaders::k_deform_spirv, shaders::k_deform_spirv_size);
  std::string manifest = desc.shader_manifest;
  if (manifest.empty()) {
    const std::string candidate = platform::executable_directory() + "/../shaders/manifest.json";
    if (io::exists(candidate)) manifest = candidate;
  }
  if (!manifest.empty()) {
    if (!shaders_.load_manifest(manifest, error)) {
      destroy();
      return false;
    }
    ENGINE_LOG_INFO(log_renderer, "shader manifest", log::field("path", manifest));
  }

  graph_ = new gfx::RenderGraph(device);
  if (!create_pipelines(error) || !targets_.create(device, width_, height_, error) ||
      !create_color_target(error)) {
    destroy();
    return false;
  }
  return true;
}

bool SceneRenderer::create_color_target(std::string* error) {
  if (!desc_.offscreen) return true;
  if (color_.image != VK_NULL_HANDLE) gfx::destroy_image(*device_, color_);
  color_ = gfx::ImageResource{};
  return gfx::create_image_2d(*device_, width_, height_, desc_.color_format,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              color_, error);
}

bool SceneRenderer::create_pipelines(std::string* error) {
  const gfx::Device& device = *device_;
  const gfx::Shader* mesh = shaders_.get("cluster_mesh", error);
  const gfx::Shader* cull = mesh != nullptr ? shaders_.get("cluster_cull", error) : nullptr;
  const gfx::Shader* sw = cull != nullptr ? shaders_.get("cluster_sw_raster", error) : nullptr;
  const gfx::Shader* hiz = sw != nullptr ? shaders_.get("hiz_build", error) : nullptr;
  const gfx::Shader* vertex = hiz != nullptr ? shaders_.get("cluster_vertex", error) : nullptr;
  const gfx::Shader* resolve =
      hiz != nullptr
          ? shaders_.get(resolved_.shadows ? "visibility_resolve_rt" : "visibility_resolve", error)
          : nullptr;
  if (resolve == nullptr || vertex == nullptr) return false;
  gfx::BindlessSet& bindless = scene_->bindless();
  if (resolved_.settings.deform) {
    const gfx::Shader* deform = shaders_.get("deform", error);
    if (deform == nullptr ||
        !gfx::create_compute_pipeline(device, deform->module, "deform_main", {},
                                      sizeof(gfx::DeformParams), pipelines_.deform, error)) {
      return false;
    }
  }
  if (resolved_.rt_chain) {
    const gfx::Shader* records = shaders_.get("clas_records", error);
    if (records == nullptr) return false;
    if (!gfx::create_compute_pipeline(device, records->module, "records_main", {},
                                      sizeof(gfx::ClusterRecordParams), pipelines_.records,
                                      error) ||
        !gfx::create_compute_pipeline(device, records->module, "ranges_main", {},
                                      sizeof(gfx::ClusterRecordParams), pipelines_.record_ranges,
                                      error) ||
        !gfx::create_compute_pipeline(device, records->module, "emit_main", {},
                                      sizeof(gfx::ClusterRecordParams), pipelines_.record_emit,
                                      error)) {
      return false;
    }
  }
  if (resolved_.ray_path) {
    const gfx::Shader* trace = shaders_.get("ray_visibility", error);
    if (trace == nullptr) return false;
    const VkDescriptorSetLayout set_layout = bindless.layout();
    if (!gfx::create_compute_pipeline(device, trace->module, "trace_main",
                                      std::span<const VkDescriptorSetLayout>(&set_layout, 1),
                                      sizeof(u64), pipelines_.trace, error)) {
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
  direct_desc.color_format = desc_.color_format;
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
  resolve_desc.color_format = desc_.color_format;
  return gfx::create_mesh_pipeline(device, direct_desc, pipelines_.direct, error) &&
         gfx::create_mesh_pipeline(device, hw_desc, pipelines_.hardware, error) &&
         gfx::create_graphics_pipeline(device, vertex_desc, pipelines_.vertex, error) &&
         gfx::create_compute_pipeline(device, sw->module, "sw_raster_main", {},
                                      sizeof(gfx::ClusterDrawParams), pipelines_.software, error) &&
         gfx::create_compute_pipeline(device, cull->module, "cull_main", {}, sizeof(u64),
                                      pipelines_.cull, error) &&
         gfx::create_compute_pipeline(device, hiz->module, "hiz_build_main", {},
                                      sizeof(gfx::HizParams), pipelines_.hiz, error) &&
         gfx::create_graphics_pipeline(device, resolve_desc, pipelines_.resolve, error);
}

void SceneRenderer::destroy() noexcept {
  if (device_ == nullptr) return;
  const gfx::Device& device = *device_;
  frames_.wait_idle();
  if (graph_ != nullptr) {
    graph_->reset();
    delete graph_;
    graph_ = nullptr;
  }
  targets_.destroy(device);
  pipelines_.destroy(device);
  shaders_.destroy();
  if (color_.image != VK_NULL_HANDLE) gfx::destroy_image(device, color_);
  color_ = gfx::ImageResource{};
  for (gfx::BufferResource& b : params_)
    gfx::destroy_buffer(device, b);
  for (gfx::BufferResource& b : resolves_)
    gfx::destroy_buffer(device, b);
  for (gfx::BufferResource& b : stat_blocks_)
    gfx::destroy_buffer(device, b);
  for (gfx::BufferResource& b : ray_params_)
    gfx::destroy_buffer(device, b);
  params_.clear();
  resolves_.clear();
  stat_blocks_.clear();
  ray_params_.clear();
  timer_.destroy();
  frames_.destroy();
  device_ = nullptr;
  scene_ = nullptr;
  width_ = height_ = 0;
  submitted_ = 0;
  collected_ = 0;
  flags_dirty_ = true;
  recording_ = false;
}

void SceneRenderer::reset_stats() noexcept { stats_ = Stats{}; }

bool SceneRenderer::resize(u32 width, u32 height, std::string* error) {
  if (width == 0 || height == 0 || (width == width_ && height == height_)) return true;
  frames_.wait_idle();
  width_ = width;
  height_ = height;
  flags_dirty_ = true;  // last frame's visible set no longer matches the Hi-Z
  return targets_.create(*device_, width_, height_, error) && create_color_target(error);
}

bool SceneRenderer::poll_shaders(Vector<std::string>& changed, std::string* error) {
  if (shaders_.poll_changes(changed, error) == 0) return true;
  frames_.wait_idle();
  pipelines_.destroy(*device_);
  return create_pipelines(error);
}

void SceneRenderer::collect_slot(u32 slot) {
  if (resolved_.settings.cull) {
    const auto* stats = static_cast<const u32*>(stat_blocks_[slot].mapped);
    stats_.visible_hw = stats[resolved_.vertex_path ? 1 : 0];
    stats_.visible_pass2 = stats[resolved_.vertex_path ? 4 : 3];
    stats_.visible_sw = stats[6];
    const u32 total = stats_.visible_pairs();
    stats_.visible_min = total < stats_.visible_min ? total : stats_.visible_min;
    stats_.visible_max = total > stats_.visible_max ? total : stats_.visible_max;
  }
  if (!timer_.results().empty()) {
    stats_.gpu_cull += timer_.ms("cull");
    stats_.gpu_hw += timer_.ms("hw");
    stats_.gpu_sw += timer_.ms("sw");
    stats_.gpu_hiz += timer_.ms("hiz");
    stats_.gpu_resolve += timer_.ms("resolve");
    stats_.gpu_rt += timer_.ms("records") + timer_.ms("ranges") + timer_.ms("emit") +
                     timer_.ms("clas") + timer_.ms("blas") + timer_.ms("tlas");
    stats_.gpu_clas += timer_.ms("clas");
    stats_.gpu_deform += timer_.ms("deform");
    stats_.gpu_trace += timer_.ms("trace");
    stats_.gpu_total += timer_.total_ms();
    ++stats_.timed_frames;
  }
}

void SceneRenderer::collect_visible() {
  if (!resolved_.settings.cull) return;
  const auto* stats = static_cast<const u32*>(stat_blocks_[frames_.slot()].mapped);
  stats_.visible_hw = stats[resolved_.vertex_path ? 1 : 0];
  stats_.visible_pass2 = stats[resolved_.vertex_path ? 4 : 3];
  stats_.visible_sw = stats[6];
  const u32 total = stats_.visible_pairs();
  stats_.visible_min = total < stats_.visible_min ? total : stats_.visible_min;
  stats_.visible_max = total > stats_.visible_max ? total : stats_.visible_max;
}

void SceneRenderer::begin_frame() {
  commands_ = frames_.begin_frame();
  timer_.begin_frame(commands_, frames_.slot());
  // The frame that last used this slot has completed: its statistics are readable.
  if (submitted_ >= frames_.frames_in_flight()) collect_slot(frames_.slot());
  recording_ = true;
}

u64 SceneRenderer::submit_frame(const FrameDesc& frame, std::string* error) {
  ENGINE_ASSERT(recording_, "SceneRenderer::submit_frame: begin_frame was not called");
  const i64 started = time::monotonic_ns();
  const gfx::ImageResource& color = frame.color.image != VK_NULL_HANDLE ? frame.color : color_;
  if (color.image == VK_NULL_HANDLE) {
    if (error != nullptr) *error = "the frame names no color target and the renderer owns none";
    frames_.end_frame();
    recording_ = false;
    return 0;
  }
  graph_->reset();
  const gfx::RgImage color_handle = graph_->import_image("color", color);
  if (!record_frame(frame, color_handle, error)) {
    frames_.end_frame();
    recording_ = false;
    return 0;
  }
  u64 value = 0;
  if (frame.wait != VK_NULL_HANDLE || frame.signal != VK_NULL_HANDLE) {
    gfx::FrameContext::PresentSync sync;
    sync.wait = frame.wait;
    sync.signal = frame.signal;
    value = frames_.end_frame(sync);
  } else {
    value = frames_.end_frame();
  }
  recording_ = false;
  ++submitted_;
  ++stats_.frames;
  targets_.hiz_dirty = false;
  flags_dirty_ = false;
  stats_.cpu_ns += static_cast<f64>(time::monotonic_ns() - started);
  return value;
}

void SceneRenderer::abort_frame() {
  if (!recording_) return;
  frames_.end_frame();
  recording_ = false;
}

bool SceneRenderer::render_offscreen(const FrameDesc& frame, std::string* error) {
  begin_frame();
  const u64 value = submit_frame(frame, error);
  if (value == 0) return false;
  if (!frames_.wait(value)) {
    if (error != nullptr) *error = "the GPU did not finish the frame";
    return false;
  }
  collect_visible();
  return true;
}

// One frame, from the cull pass to the resolve. Every parameter block here is referenced by a
// pass body and therefore has to outlive the execute() at the bottom, which is why declaring,
// compiling, and executing are one function rather than three: the render graph stores bodies
// in an arena and requires them to capture by reference.
bool SceneRenderer::record_frame(const FrameDesc& frame, gfx::RgImage color_handle,
                                 std::string* error) {
  GpuScene& scene = *scene_;
  const SceneData& data = scene.data();
  const RenderSettings& settings = resolved_.settings;
  const u32 slot = frames_.slot();
  const u32 pair_count = scene.pair_count();
  const u32 instance_count = scene.instance_count();
  const u32 cluster_count = scene.cluster_count();
  const u32 leaf_count = scene.leaf_count();
  const u32 triangles_per_cluster = scene.triangles_per_cluster();
  const u64 visible_run_bytes = scene.visible_run_bytes();
  const u32 extent_width = width_;
  const u32 extent_height = height_;
  const u64 rendered = frame.frame_index;

  const f32 aspect = static_cast<f32>(extent_width) / static_cast<f32>(extent_height);
  const Vec3 eye = frame.camera.position;
  const f32 fov_y = frame.camera.fov_y;
  const f32 znear = frame.camera.znear;
  const Mat4 view_proj = perspective_reversed_z(fov_y, aspect, znear) *
                         look_at(eye, frame.camera.target, Vec3{0.0f, 1.0f, 0.0f});
  const f32 proj_scale = 1.0f / std::tan(fov_y * 0.5f) * static_cast<f32>(extent_height) * 0.5f;
  const bool direct = resolved_.direct;
  const bool vertex_path = resolved_.vertex_path;
  const bool ray_path = resolved_.ray_path;
  const bool shadows = resolved_.shadows;
  const bool occlusion = resolved_.occlusion;
  const bool rt_chain = resolved_.rt_chain;
  const bool use_hw = settings.raster != RasterMode::Software && !ray_path;
  const bool use_sw = !direct && !ray_path && settings.raster != RasterMode::Hardware &&
                      settings.raster != RasterMode::Vertex && settings.cull;
  const u32 cur_flags = static_cast<u32>(rendered % 2);
  const u32 prev_flags = 1 - cur_flags;

  // The three runs of the frame's visible list; a draw's ids start at its run.
  const u64 run_address[k_visible_runs] = {scene.visible.address,
                                           scene.visible.address + visible_run_bytes,
                                           scene.visible.address + visible_run_bytes * 2};
  gfx::ClusterDrawParams draw{};
  draw.view_proj = view_proj;
  draw.clusters = scene.clusters.address;
  draw.mesh = scene.meshes.address;
  draw.instances = scene.instances.address;
  draw.triangles = scene.triangles.address;
  draw.triangles_per_cluster = triangles_per_cluster;
  draw.visible = settings.cull ? run_address[0] : 0;
  draw.visibility = targets_.vis.address;
  draw.width = extent_width;
  draw.height = extent_height;
  gfx::ClusterDrawParams draw_pass2 = draw;
  draw_pass2.visible = run_address[1];
  draw_pass2.visible_offset = pair_count;
  gfx::ClusterDrawParams draw_sw = draw;
  draw_sw.visible = run_address[2];
  draw_sw.visible_offset = pair_count * 2;

  gfx::CullParams cull{};
  gfx::set_frustum(cull, frustum_from_view_proj(view_proj));
  cull.view_proj = view_proj;
  cull.camera = Vec4{eye, znear};
  cull.lod = Vec4{proj_scale, settings.lod_px, 1.0f, 1.0f};
  const f32 raster_mode =
      direct || settings.raster == RasterMode::Hardware || vertex_path || ray_path
          ? gfx::k_raster_hardware
      : settings.raster == RasterMode::Software ? gfx::k_raster_software
                                                : gfx::k_raster_split;
  cull.raster = Vec4{settings.sw_px, raster_mode, 0.0f, 0.0f};
  cull.cluster_count = cluster_count;
  cull.count_index = vertex_path ? 1u : 0u;
  cull.cone_cull = settings.cone ? 1u : 0u;
  cull.clusters = scene.clusters.address;
  cull.lods = scene.lods.address;
  cull.visible = run_address[0];
  cull.draw_args = scene.draw_args[0].address;
  cull.sw_visible = run_address[2];
  cull.sw_args = scene.sw_args.address;
  cull.instances = scene.instances.address;
  cull.meshes = scene.meshes.address;
  cull.instance_count = instance_count;
  cull.pair_count = pair_count;
  if (occlusion) {
    cull.hiz = targets_.hiz.address;
    cull.prev_flags = scene.flags[prev_flags].address;
    cull.flags = scene.flags[cur_flags].address;
    cull.hiz_width = extent_width;
    cull.hiz_height = extent_height;
    cull.hiz_mips = targets_.hiz_mips;
    std::memcpy(cull.hiz_offsets, targets_.hiz_offsets, sizeof(cull.hiz_offsets));
    cull.pass = 1;
  }
  gfx::CullParams cull_pass2 = cull;
  cull_pass2.pass = 2;
  cull_pass2.visible = run_address[1];
  cull_pass2.draw_args = scene.draw_args[1].address;
  auto* blocks = static_cast<gfx::CullParams*>(params_[slot].mapped);
  blocks[0] = cull;
  blocks[1] = cull_pass2;
  const u64 block_address[2] = {params_[slot].address,
                                params_[slot].address + sizeof(gfx::CullParams)};

  gfx::ResolveParams resolve{};
  resolve.sky = Vec4{0.55f, 0.70f, 0.90f, 1.0f};
  resolve.sun = Vec4{normalize(Vec3{0.4f, 0.8f, 0.45f}), 1.0f};
  resolve.camera = Vec4{eye, 0.0f};
  resolve.view_proj = view_proj;
  resolve.visibility = targets_.vis.address;
  resolve.clusters = scene.clusters.address;
  resolve.mesh = scene.meshes.address;
  resolve.instances = scene.instances.address;
  resolve.visible = settings.cull ? scene.visible.address : 0;
  resolve.triangles = scene.triangles.address;
  resolve.materials = scene.materials.address;
  resolve.cluster_materials = scene.cluster_materials.address;
  resolve.attributes = scene.attributes.address;
  resolve.width = extent_width;
  resolve.height = extent_height;
  resolve.mode = frame.view_mode == ~u32{0} ? settings.view_mode : frame.view_mode;
  // Two point lights orbiting the scene out of phase, one warm and one cool, so the BSDF's
  // specular response sweeps across the surface while the camera turns and metal reads as metal.
  // Reach and intensity scale with the scene radius, intensity with its square because the
  // falloff is inverse square, so a 2 cm mesh and the heightfield look alike. They live behind
  // the params block in the same per-slot buffer.
  const Vec3 scene_center = data.center;
  const f32 scene_radius = data.radius;
  const f32 light_orbit = 1.35f * scene_radius;
  const f32 light_angle = static_cast<f32>(rendered) * 0.013f;
  gfx::ResolveLight lights[k_view_lights];
  lights[0].position_radius =
      Vec4{scene_center + Vec3{std::cos(light_angle) * light_orbit, 0.70f * scene_radius,
                               std::sin(light_angle) * light_orbit},
           4.0f * scene_radius};
  lights[0].color_intensity = Vec4{1.0f, 0.78f, 0.55f, light_orbit * light_orbit};
  lights[1].position_radius =
      Vec4{scene_center + Vec3{-std::cos(light_angle * 0.7f) * light_orbit, -0.35f * scene_radius,
                               -std::sin(light_angle * 0.7f) * light_orbit},
           4.0f * scene_radius};
  lights[1].color_intensity = Vec4{0.50f, 0.68f, 1.0f, 0.8f * light_orbit * light_orbit};
  resolve.lights = resolves_[slot].address + sizeof(resolve);
  resolve.light_count = settings.lights ? k_view_lights : 0;
  // Shadows: every light traces against this frame's top-level structure, which holds the same
  // visible list the rasterizer drew from, so a shadow can only come from geometry the picture
  // has. The bias is a thousandth of the scene radius — a couple of centimetres on the
  // heightfield, well over the half grid step by which a quantized position may differ from the
  // float one the structures were built from, and far under any feature that casts.
  resolve.scene = shadows ? scene.tlas_slot() : gfx::k_no_scene;
  resolve.shadow_flags = shadows ? gfx::k_shadow_sun | gfx::k_shadow_lights : 0u;
  resolve.shadow_bias = 1.0e-3f * scene_radius;
  auto* resolve_block = static_cast<u8*>(resolves_[slot].mapped);
  std::memcpy(resolve_block, &resolve, sizeof(resolve));
  std::memcpy(resolve_block + sizeof(resolve), lights, sizeof(lights));
  const u64 resolve_address = resolves_[slot].address;

  // One DeformParams per run of the visible list. The pass reads that run's count word out of
  // the cull's own arguments and dispatches one group per visible cluster from a copy of it, so
  // the pool pass costs the cut and nothing else.
  gfx::DeformParams deform_params[k_visible_runs];
  const f32 deform_time = static_cast<f32>(rendered) / 60.0f;
  for (u32 run = 0; run < k_visible_runs; ++run) {
    gfx::DeformParams& d = deform_params[run];
    d = gfx::DeformParams{};
    d.clusters = scene.clusters.address;
    d.instances = scene.instances.address;
    d.meshes = scene.meshes.address;
    d.attributes = scene.attributes.address;
    d.visible = run_address[run];
    d.visible_count =
        run == 2 ? scene.sw_args.address : scene.draw_args[run].address + u64{cull.count_index} * 4;
    d.pool = scene.deform_pool.address;
    d.deform = scene.deform_table.address;
    d.time = deform_time;
    d.amplitude = settings.deform_amplitude;
    d.max_entries = pair_count;
  }

  // The records pass turns this frame's visible list into CLAS build records and the builds
  // follow on the GPU. The ray path then traces the picture against them; a raster mode with
  // shadows on runs the same chain and the resolve traces the lights against them.
  gfx::ClusterRecordParams record_params{};
  u64 ray_address = 0;
  Vector<gfx::TlasInstance> tlas_instances;
  if (rt_chain) {
    record_params.clusters = scene.clusters.address;
    record_params.vertices = scene.vertices.address;
    record_params.indices8 = scene.indices8.address;
    record_params.instances = scene.instances.address;
    record_params.meshes = scene.meshes.address;
    record_params.instantiate = settings.rt_templates ? 1u : 0u;
    record_params.visible = run_address[0];
    // Where the cull pass counted this run's survivors: the mesh path's group count is the first
    // word of the indirect block, the vertex path's instance count the second.
    record_params.visible_count = scene.draw_args[0].address + (vertex_path ? sizeof(u32) : 0);
    record_params.slots = scene.slots.address;
    record_params.instance_counts = scene.instance_counts.address;
    record_params.instance_first = scene.instance_first.address;
    record_params.records = scene.records.address;
    record_params.record_count = scene.record_count.address;
    record_params.blas_records = scene.blas_records.address;
    record_params.clas_addresses = scene.clas_set.addresses.address;
    record_params.instance_count = instance_count;
    record_params.pair_count = pair_count;
    record_params.max_clusters = pair_count;
    // One top-level instance per scene instance: the world transform, the instance as the custom
    // index, and that instance's own cluster bottom-level structure.
    for (u32 i = 0; i < instance_count; ++i) {
      gfx::TlasInstance record;
      record.transform = data.instances[i].world;
      record.custom_index = i;
      record.blas = scene.cluster_blas[i].address;
      tlas_instances.push_back(record);
    }
    gfx::write_instances(
        std::span<const gfx::TlasInstance>(tlas_instances.data(), tlas_instances.size()),
        scene.rt_instances.mapped);
  }
  if (ray_path) {
    gfx::RayVisibilityParams ray{};
    ray.view_proj = view_proj;
    ray.inv_view_proj = inverse(view_proj);
    ray.camera = Vec4{eye, 0.0f};
    ray.output = targets_.vis.address;
    ray.instance_base = 0;  // a CLAS record's base geometry index is the visible entry
    ray.width = extent_width;
    ray.height = extent_height;
    ray.scene = scene.tlas_slot();
    std::memcpy(ray_params_[slot].mapped, &ray, sizeof(ray));
    ray_address = ray_params_[slot].address;
  }

  gfx::RenderGraph& graph = *graph_;
  gfx::GpuTimer& timer = timer_;
  Pipelines& pipelines = pipelines_;
  Targets& targets = targets_;
  gfx::BindlessSet& bindless = scene.bindless();
  const gfx::RgImage color = color_handle;
  const gfx::RgImage depth_target = graph.import_image("depth", targets.depth);
  const gfx::RgBuffer rg_args[2] = {graph.import_buffer("draw_args", scene.draw_args[0]),
                                    graph.import_buffer("draw_args2", scene.draw_args[1])};
  const gfx::RgBuffer rg_visible = graph.import_buffer("visible", scene.visible);
  const gfx::RgBuffer rg_flags[2] = {graph.import_buffer("flags0", scene.flags[0]),
                                     graph.import_buffer("flags1", scene.flags[1])};
  const gfx::RgBuffer rg_sw_args = graph.import_buffer("sw_args", scene.sw_args);
  const gfx::RgBuffer rg_vis = graph.import_buffer("visibility", targets.vis);
  const gfx::RgBuffer rg_hiz = graph.import_buffer("hiz", targets.hiz);
  const gfx::RgBuffer rg_stats = graph.import_buffer("stats", stat_blocks_[slot]);
  gfx::RgBuffer rg_pool{};
  gfx::RgBuffer rg_deform_args[k_visible_runs]{};
  if (settings.deform) {
    rg_pool = graph.import_buffer("deform pool", scene.deform_pool);
    for (u32 run = 0; run < k_visible_runs; ++run)
      rg_deform_args[run] = graph.import_buffer("deform args", scene.deform_args[run]);
  }
  struct RtBuffers {
    gfx::RgBuffer records, record_count, slots, instance_counts, instance_first, blas_records;
    gfx::RgBuffer clas_data, clas_addresses, clas_sizes, tlas, instances;
  } rt{};
  Vector<gfx::RgBuffer> rg_blas_data;
  if (rt_chain) {
    rt.records = graph.import_buffer("clas records", scene.records);
    rt.record_count = graph.import_buffer("clas record count", scene.record_count);
    rt.slots = graph.import_buffer("clas slots", scene.slots);
    rt.instance_counts = graph.import_buffer("clas instance counts", scene.instance_counts);
    rt.instance_first = graph.import_buffer("clas instance first", scene.instance_first);
    rt.blas_records = graph.import_buffer("cluster blas records", scene.blas_records);
    rt.clas_data = graph.import_buffer("clas", scene.clas_set.data);
    rt.clas_addresses = graph.import_buffer("clas addresses", scene.clas_set.addresses);
    rt.clas_sizes = graph.import_buffer("clas sizes", scene.clas_set.sizes);
    for (u32 i = 0; i < instance_count; ++i)
      rg_blas_data.push_back(graph.import_buffer("cluster blas", scene.cluster_blas[i].data));
    rt.tlas = graph.import_buffer("tlas", scene.tlas.buffer);
    rt.instances = graph.import_buffer("tlas instances", scene.rt_instances);
  }
  VkClearColorValue sky{};
  sky.float32[0] = 0.55f;
  sky.float32[1] = 0.70f;
  sky.float32[2] = 0.90f;
  sky.float32[3] = 1.0f;
  const bool fill_hiz = occlusion && targets.hiz_dirty;
  const bool fill_flags = occlusion && flags_dirty_;
  const bool cull_on = settings.cull;
  const bool deform_on = settings.deform;

  graph.add_pass(
      "reset", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        if (cull_on) {
          b.write(rg_args[0], gfx::Access::TransferWrite);
          b.write(rg_args[1], gfx::Access::TransferWrite);
          b.write(rg_sw_args, gfx::Access::TransferWrite);
        }
        if (!direct) b.write(rg_vis, gfx::Access::TransferWrite);
        if (occlusion) b.write(rg_flags[cur_flags], gfx::Access::TransferWrite);
        if (fill_flags) b.write(rg_flags[prev_flags], gfx::Access::TransferWrite);
        if (fill_hiz) b.write(rg_hiz, gfx::Access::TransferWrite);
        if (rt_chain) b.write(rt.instance_counts, gfx::Access::TransferWrite);
        if (deform_on) {
          for (const gfx::RgBuffer& args : rg_deform_args)
            b.write(args, gfx::Access::TransferWrite);
        }
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        if (cull_on) {
          for (u32 i = 0; i < 2; ++i) {
            if (vertex_path) {  // {vertexCount, instanceCount = 0, firstVertex, firstInstance}
              vkCmdFillBuffer(cb, scene.draw_args[i].buffer, 0, sizeof(u32),
                              triangles_per_cluster * 3);
              vkCmdFillBuffer(cb, scene.draw_args[i].buffer, sizeof(u32), sizeof(u32) * 3, 0);
            } else {  // {groups = 0, 1, 1}
              vkCmdFillBuffer(cb, scene.draw_args[i].buffer, 0, sizeof(u32), 0);
              vkCmdFillBuffer(cb, scene.draw_args[i].buffer, sizeof(u32), sizeof(u32) * 2, 1);
            }
          }
          vkCmdFillBuffer(cb, scene.sw_args.buffer, 0, sizeof(u32), 0);
          vkCmdFillBuffer(cb, scene.sw_args.buffer, sizeof(u32), sizeof(u32) * 2, 1);
        }
        if (!direct) vkCmdFillBuffer(cb, targets.vis.buffer, 0, VK_WHOLE_SIZE, 0);
        if (occlusion) vkCmdFillBuffer(cb, scene.flags[cur_flags].buffer, 0, VK_WHOLE_SIZE, 0);
        if (fill_flags) vkCmdFillBuffer(cb, scene.flags[prev_flags].buffer, 0, VK_WHOLE_SIZE, 0);
        if (fill_hiz) vkCmdFillBuffer(cb, targets.hiz.buffer, 0, VK_WHOLE_SIZE, 0);
        if (rt_chain) vkCmdFillBuffer(cb, scene.instance_counts.buffer, 0, VK_WHOLE_SIZE, 0);
        if (deform_on) {  // {groups = 0, 1, 1}; the copy below fills in the count
          for (const gfx::BufferResource& args : scene.deform_args) {
            vkCmdFillBuffer(cb, args.buffer, 0, sizeof(u32), 0);
            vkCmdFillBuffer(cb, args.buffer, sizeof(u32), sizeof(u32) * 2, 1);
          }
        }
      });
  auto add_cull = [&](u32 block, u32 list) {
    graph.add_pass(
        "cull", gfx::PassKind::Compute,
        [&, list](gfx::PassBuilder& b) {
          b.write(rg_args[list], gfx::Access::ComputeReadWrite);
          b.write(rg_visible, gfx::Access::ComputeWrite);
          if (use_sw) {
            b.write(rg_sw_args, gfx::Access::ComputeReadWrite);
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
          vkCmdPushConstants(cb, pipelines.cull.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(u64),
                             &block_address[block]);
          vkCmdDispatch(cb, gfx::cull_group_count(pair_count), 1, 1);
          timer.end(cb);
        });
  };
  // The deformed-vertex pool for one run of the visible list: copy that run's survivor count
  // into an indirect dispatch block, then one workgroup per surviving cluster. Two runs when
  // occlusion culling splits the cut, because pass 2's entries are not known until its cull has
  // run and the pool has to hold pass 1's positions before pass 1 draws.
  auto add_deform = [&](u32 run) {
    const u64 source_offset = run == 2 ? 0 : u64{cull.count_index} * sizeof(u32);
    const gfx::RgBuffer rg_source = run == 2 ? rg_sw_args : rg_args[run];
    graph.add_pass(
        "deform args", gfx::PassKind::Transfer,
        [&, rg_source, run](gfx::PassBuilder& b) {
          b.read(rg_source, gfx::Access::TransferRead);
          b.write(rg_deform_args[run], gfx::Access::TransferWrite);
        },
        [&, run, source_offset](VkCommandBuffer cb, gfx::RenderGraph&) {
          const VkBufferCopy copy{source_offset, 0, sizeof(u32)};
          const VkBuffer source = run == 2 ? scene.sw_args.buffer : scene.draw_args[run].buffer;
          vkCmdCopyBuffer(cb, source, scene.deform_args[run].buffer, 1, &copy);
        });
    const gfx::DeformParams* params = &deform_params[run];
    graph.add_pass(
        "deform", gfx::PassKind::Compute,
        [&, rg_source, run](gfx::PassBuilder& b) {
          b.read(rg_source, gfx::Access::ComputeRead);  // the run's count word
          b.read(rg_deform_args[run], gfx::Access::IndirectRead);
          b.read(rg_visible, gfx::Access::ComputeRead);
          b.write(rg_pool, gfx::Access::ComputeWrite);
        },
        [&, params, run](VkCommandBuffer cb, gfx::RenderGraph&) {
          timer.begin(cb, "deform");
          vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.deform.pipeline);
          vkCmdPushConstants(cb, pipelines.deform.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                             sizeof(*params), params);
          vkCmdDispatchIndirect(cb, scene.deform_args[run].buffer, 0);
          timer.end(cb);
        });
  };
  auto add_hw_draw = [&](u32 list, const gfx::ClusterDrawParams* params) {
    graph.add_pass(
        "hardware", gfx::PassKind::Raster,
        [&, list](gfx::PassBuilder& b) {
          b.render_area(extent_width, extent_height);
          b.write(rg_vis, gfx::Access::FragmentReadWrite);
          if (cull_on) {
            b.read(rg_args[list], gfx::Access::IndirectRead);
            b.read(rg_visible, vertex_path ? gfx::Access::VertexRead : gfx::Access::MeshRead);
          }
          if (deform_on)
            b.read(rg_pool, vertex_path ? gfx::Access::VertexRead : gfx::Access::MeshRead);
        },
        [&, list, params](VkCommandBuffer cb, gfx::RenderGraph&) {
          timer.begin(cb, "hw");
          vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            vertex_path ? pipelines.vertex : pipelines.hardware);
          bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
          vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                             sizeof(*params), params);
          if (vertex_path) {
            if (cull_on) {
              vkCmdDrawIndirect(cb, scene.draw_args[list].buffer, 0, 1, sizeof(u32) * 4);
            } else {
              vkCmdDraw(cb, triangles_per_cluster * 3, leaf_count, 0, 0);
            }
          } else if (cull_on) {
            vkCmdDrawMeshTasksIndirectEXT(cb, scene.draw_args[list].buffer, 0, 1, sizeof(u32) * 3);
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
      level->src =
          m == 0 ? targets.vis.address : targets.hiz.address + u64{targets.hiz_offsets[m - 1]} * 4;
      level->dst = targets.hiz.address + u64{targets.hiz_offsets[m]} * 4;
      level->src_width = m == 0 ? extent_width : gfx::hiz_mip_extent(extent_width, m - 1);
      level->src_height = m == 0 ? extent_height : gfx::hiz_mip_extent(extent_height, m - 1);
      level->dst_width = gfx::hiz_mip_extent(extent_width, m);
      level->dst_height = gfx::hiz_mip_extent(extent_height, m);
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

  if (cull_on) add_cull(0, 0);
  if (deform_on) add_deform(0);
  if (direct) {
    graph.add_pass(
        "direct", gfx::PassKind::Raster,
        [&](gfx::PassBuilder& b) {
          b.color_attachment(color, VK_ATTACHMENT_LOAD_OP_CLEAR, sky);
          b.depth_attachment(depth_target, VK_ATTACHMENT_LOAD_OP_CLEAR,
                             0.0f);  // reversed Z: far is 0
          if (cull_on) {
            b.read(rg_args[0], gfx::Access::IndirectRead);
            b.read(rg_visible, gfx::Access::MeshRead);
          }
          if (deform_on) b.read(rg_pool, gfx::Access::MeshRead);
        },
        [&](VkCommandBuffer cb, gfx::RenderGraph&) {
          timer.begin(cb, "hw");
          vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.direct);
          bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
          vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0, sizeof(draw),
                             &draw);
          if (cull_on) {
            vkCmdDrawMeshTasksIndirectEXT(cb, scene.draw_args[0].buffer, 0, 1, sizeof(u32) * 3);
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
      if (deform_on) add_deform(1);
      add_hw_draw(1, &draw_pass2);
      add_hiz(1);
    }
    if (use_sw) {
      if (deform_on) add_deform(2);
      graph.add_pass(
          "software", gfx::PassKind::Compute,
          [&](gfx::PassBuilder& b) {
            b.write(rg_vis, gfx::Access::ComputeReadWrite);
            b.read(rg_sw_args, gfx::Access::IndirectRead);
            b.read(rg_visible, gfx::Access::ComputeRead);
            if (deform_on) b.read(rg_pool, gfx::Access::ComputeRead);
          },
          [&](VkCommandBuffer cb, gfx::RenderGraph&) {
            timer.begin(cb, "sw");
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.software.pipeline);
            vkCmdPushConstants(cb, pipelines.software.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(draw_sw), &draw_sw);
            vkCmdDispatchIndirect(cb, scene.sw_args.buffer, 0);
            timer.end(cb);
          });
    }
    if (rt_chain) {
      // The frame's cut becomes the frame's ray tracing geometry with no CPU in between: bucket
      // the visible entries by instance, prefix-sum the per-instance counts, emit the dense CLAS
      // records, build every CLAS in one command, build one cluster bottom-level structure per
      // instance, and top-level over them. The picture is traced against it under the ray path
      // and the shadow rays are traced against it whenever shadows are on, so the chain is the
      // same passes in the same order for both.
      const gfx::ComputePipeline* record_passes[3] = {&pipelines.records, &pipelines.record_ranges,
                                                      &pipelines.record_emit};
      const char* record_names[3] = {"records", "ranges", "emit"};
      const u32 record_groups[3] = {
          (pair_count + gfx::k_cluster_records_workgroup - 1) / gfx::k_cluster_records_workgroup, 1,
          (pair_count + gfx::k_cluster_records_workgroup - 1) / gfx::k_cluster_records_workgroup};
      for (u32 p = 0; p < 3; ++p) {
        graph.add_pass(
            record_names[p], gfx::PassKind::Compute,
            [&, p](gfx::PassBuilder& b) {
              b.read(rg_args[0], gfx::Access::ComputeRead);
              b.read(rg_visible, gfx::Access::ComputeRead);
              b.write(rt.slots, gfx::Access::ComputeReadWrite);
              b.write(rt.instance_counts, gfx::Access::ComputeReadWrite);
              b.write(rt.instance_first, gfx::Access::ComputeReadWrite);
              if (p != 0) b.write(rt.records, gfx::Access::ComputeWrite);
              if (p == 1) {
                b.write(rt.record_count, gfx::Access::ComputeWrite);
                b.write(rt.blas_records, gfx::Access::ComputeWrite);
              }
            },
            [&, p, record_passes, record_names, record_groups](VkCommandBuffer cb,
                                                               gfx::RenderGraph&) {
              timer.begin(cb, record_names[p]);
              vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, record_passes[p]->pipeline);
              vkCmdPushConstants(cb, record_passes[p]->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                 sizeof(record_params), &record_params);
              vkCmdDispatch(cb, record_groups[p], 1, 1);
              timer.end(cb);
            });
      }
      graph.add_pass(
          "clas", gfx::PassKind::Compute,
          [&](gfx::PassBuilder& b) {
            b.read(rt.records, gfx::Access::AccelerationBuildRead);
            b.read(rt.record_count, gfx::Access::AccelerationBuildRead);
            if (deform_on) b.read(rg_pool, gfx::Access::AccelerationBuildRead);
            b.write(rt.clas_data, gfx::Access::AccelerationBuildWrite);
            b.write(rt.clas_addresses, gfx::Access::AccelerationBuildWrite);
            b.write(rt.clas_sizes, gfx::Access::AccelerationBuildWrite);
          },
          [&](VkCommandBuffer cb, gfx::RenderGraph&) {
            // The same command either way: a set created with `instantiate` runs the instantiate
            // op over the template records the emit pass wrote.
            timer.begin(cb, "clas");
            gfx::build_cluster_set(cb, scene.clas_set, scene.records.address,
                                   scene.record_count.address, scene.rt_scratch);
            timer.end(cb);
          });
      graph.add_pass(
          "blas", gfx::PassKind::Compute,
          [&](gfx::PassBuilder& b) {
            b.read(rt.blas_records, gfx::Access::AccelerationBuildRead);
            b.read(rt.clas_addresses, gfx::Access::AccelerationBuildRead);
            b.read(rt.clas_data, gfx::Access::AccelerationBuildRead);
            for (const gfx::RgBuffer& d : rg_blas_data)
              b.write(d, gfx::Access::AccelerationBuildWrite);
          },
          [&](VkCommandBuffer cb, gfx::RenderGraph&) {
            // One build per instance; they share the scratch, so each waits for the last.
            timer.begin(cb, "blas");
            for (u32 i = 0; i < instance_count; ++i) {
              if (i != 0) {
                gfx::acceleration_build_barrier(
                    cb, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                    VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                        VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
              }
              gfx::build_cluster_blas_indirect(
                  cb, scene.cluster_blas[i], scene.rt_scratch,
                  scene.blas_records.address + u64{i} * gfx::k_cluster_blas_record_bytes);
            }
            timer.end(cb);
          });
      graph.add_pass(
          "tlas", gfx::PassKind::Compute,
          [&](gfx::PassBuilder& b) {
            for (const gfx::RgBuffer& d : rg_blas_data)
              b.read(d, gfx::Access::AccelerationBuildRead);
            b.read(rt.instances, gfx::Access::AccelerationBuildRead);
            b.write(rt.tlas, gfx::Access::AccelerationBuildWrite);
          },
          [&](VkCommandBuffer cb, gfx::RenderGraph&) {
            timer.begin(cb, "tlas");
            gfx::build_tlas(cb, scene.tlas, scene.rt_instances.address, instance_count,
                            gfx::k_build_fast_trace, scene.rt_scratch);
            timer.end(cb);
          });
    }
    if (ray_path) {
      graph.add_pass(
          "trace", gfx::PassKind::Compute,
          [&](gfx::PassBuilder& b) {
            b.read(rt.tlas, gfx::Access::RayQueryRead);
            for (const gfx::RgBuffer& d : rg_blas_data)
              b.read(d, gfx::Access::RayQueryRead);
            b.read(rt.clas_data, gfx::Access::RayQueryRead);
            b.write(rg_vis, gfx::Access::ComputeWrite);
          },
          [&](VkCommandBuffer cb, gfx::RenderGraph&) {
            timer.begin(cb, "trace");
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.trace.pipeline);
            bindless.bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE);
            vkCmdPushConstants(cb, pipelines.trace.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(u64), &ray_address);
            vkCmdDispatch(cb, gfx::ray_visibility_group_count(extent_width),
                          gfx::ray_visibility_group_count(extent_height), 1);
            timer.end(cb);
          });
    }
    graph.add_pass(
        "resolve", gfx::PassKind::Raster,
        [&](gfx::PassBuilder& b) {
          b.color_attachment(color, VK_ATTACHMENT_LOAD_OP_CLEAR, sky);
          b.read(rg_vis, gfx::Access::FragmentRead);
          if (deform_on) b.read(rg_pool, gfx::Access::FragmentRead);
          if (shadows) {  // the shadow rays traverse them from the fragment stage
            b.read(rt.tlas, gfx::Access::FragmentRayQueryRead);
            for (const gfx::RgBuffer& d : rg_blas_data)
              b.read(d, gfx::Access::FragmentRayQueryRead);
            b.read(rt.clas_data, gfx::Access::FragmentRayQueryRead);
          }
        },
        [&](VkCommandBuffer cb, gfx::RenderGraph&) {
          timer.begin(cb, "resolve");
          vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.resolve);
          bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
          vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0, sizeof(u64),
                             &resolve_address);
          vkCmdDraw(cb, 3, 1, 0, 0);
          timer.end(cb);
        });
  }
  if (cull_on) {
    const gfx::BufferResource* stat_target = &stat_blocks_[slot];
    graph.add_pass(
        "stats", gfx::PassKind::Transfer,
        [&](gfx::PassBuilder& b) {
          b.read(rg_args[0], gfx::Access::TransferRead);
          b.read(rg_args[1], gfx::Access::TransferRead);
          b.read(rg_sw_args, gfx::Access::TransferRead);
          b.write(rg_stats, gfx::Access::TransferWrite);
        },
        [&, stat_target](VkCommandBuffer cb, gfx::RenderGraph&) {
          const gfx::BufferResource* arg_blocks[3] = {&scene.draw_args[0], &scene.draw_args[1],
                                                      &scene.sw_args};
          for (u32 i = 0; i < 3; ++i) {
            const VkBufferCopy copy{0, sizeof(u32) * 3 * i, sizeof(u32) * 3};
            vkCmdCopyBuffer(cb, arg_blocks[i]->buffer, stat_target->buffer, 1, &copy);
          }
        });
  }
  graph.set_final_layout(color, frame.final_layout);
  if (!graph.compile(error)) return false;
  graph.execute(commands_);
  return true;
}

bool SceneRenderer::capture(const FrameDesc& frame, const CaptureChannels& channels,
                            CapturedFrame& out, std::string* error) {
  if (channels.any_visibility() && resolved_.direct) {
    if (error != nullptr) {
      *error =
          "the direct raster path writes no visibility buffer, so it has no id or depth "
          "channel; use hw, vertex, sw, auto, or rt";
    }
    return false;
  }
  out = CapturedFrame{};
  out.width = width_;
  out.height = height_;
  FrameDesc first = frame;
  first.color = gfx::ImageResource{};  // the renderer's own target
  first.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  first.wait = VK_NULL_HANDLE;
  first.signal = VK_NULL_HANDLE;
  if (!render_offscreen(first, error)) return false;
  if (channels.color) {
    gfx::Capture shot;
    if (!gfx::capture_image(*device_, color_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot, error) ||
        !gfx::capture_to_rgba8(shot, out.color)) {
      if (error != nullptr && error->empty()) *error = "unsupported color format";
      return false;
    }
    out.width = shot.width;
    out.height = shot.height;
  }
  if (channels.any_visibility() && !read_visibility(out, channels, error)) return false;
  if (channels.normals) {
    // The normal is what the resolve reconstructs, and only the resolve knows it: a second
    // frame in the normals view mode is the honest way to read one back, and it costs exactly
    // one more frame.
    FrameDesc second = first;
    second.view_mode = 4;  // gfx::ResolveMode::Normals
    if (!render_offscreen(second, error)) return false;
    gfx::Capture shot;
    Vector<u8> rgba;
    if (!gfx::capture_image(*device_, color_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot, error) ||
        !gfx::capture_to_rgba8(shot, rgba)) {
      if (error != nullptr && error->empty()) *error = "unsupported color format";
      return false;
    }
    out.normals.resize(shot.width * shot.height * 3);
    for (u32 i = 0; i < shot.width * shot.height; ++i) {
      out.normals[i * 3 + 0] = rgba[i * 4 + 0];
      out.normals[i * 3 + 1] = rgba[i * 4 + 1];
      out.normals[i * 3 + 2] = rgba[i * 4 + 2];
    }
  }
  return true;
}

// The visibility buffer holds `depth << 32 | (visible_index << 8 | triangle)` and the visible
// list turns a `visible_index` into the {instance, cluster} pair it names — the same decode the
// resolve does in `visible_entry`, including the no-cull case where the list is null and entry i
// is {0, i}. Both are read back here so that what a capture reports is the pair, not an index
// into a list that will not exist a frame later.
bool SceneRenderer::read_visibility(CapturedFrame& out, const CaptureChannels& channels,
                                    std::string* error) {
  const u64 pixels = u64{width_} * height_;
  Vector<u8> vis_bytes;
  if (!read_buffer(*device_, targets_.vis, pixels * sizeof(u64), vis_bytes, error)) return false;
  Vector<u8> visible_bytes;
  const bool has_list = resolved_.settings.cull && scene_->visible_run_bytes() > 0;
  if (has_list &&
      !read_buffer(*device_, scene_->visible, scene_->visible_run_bytes() * k_visible_runs,
                   visible_bytes, error)) {
    return false;
  }
  const auto* values = reinterpret_cast<const u64*>(vis_bytes.data());
  const auto* entries = reinterpret_cast<const u32*>(visible_bytes.data());
  const u32 entry_count =
      has_list ? static_cast<u32>(scene_->visible_run_bytes() * k_visible_runs / 8) : 0;
  const u32 pixel_count = static_cast<u32>(pixels);
  if (channels.ids) out.ids.resize(pixel_count * k_id_words);
  if (channels.depth) out.depth.resize(pixel_count);
  f32 lo = 0.0f;
  f32 hi = 0.0f;
  for (u32 p = 0; p < pixel_count; ++p) {
    const u64 value = values[p];
    if (value == 0) {
      if (channels.ids) {
        out.ids[p * k_id_words + 0] = k_no_id;
        out.ids[p * k_id_words + 1] = k_no_id;
        out.ids[p * k_id_words + 2] = k_no_id;
      }
      continue;
    }
    ++out.covered;
    const u32 id = static_cast<u32>(value & 0xffffffffu);
    if (channels.depth) {
      const u32 bits = static_cast<u32>(value >> 32);
      f32 depth = 0.0f;
      std::memcpy(&depth, &bits, sizeof(depth));
      out.depth[p] = depth;
      if (out.covered == 1) {
        lo = hi = depth;
      } else {
        lo = depth < lo ? depth : lo;
        hi = depth > hi ? depth : hi;
      }
    }
    if (!channels.ids) continue;
    const u32 index = id >> 8;
    u32 instance = 0;
    u32 cluster = index;
    if (has_list && index < entry_count) {
      instance = entries[index * 2 + 0];
      cluster = entries[index * 2 + 1];
    } else if (has_list) {
      instance = k_no_id;  // an id past the list: the frame and the readback disagree
      cluster = k_no_id;
    }
    out.ids[p * k_id_words + 0] = instance;
    out.ids[p * k_id_words + 1] = cluster;
    out.ids[p * k_id_words + 2] = id & 0xffu;
  }
  out.depth_min = lo;
  out.depth_max = hi;
  return true;
}

}  // namespace engine::renderer
