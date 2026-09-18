#include <core/base/assert.h>
#include <core/platform/process.h>
#include <core/time/time.h>
#include <domain/gfx/capture.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/lighting.h>
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

constexpr u32 k_view_lights = k_frame_lights;  // the warm and cool point lights (lighting.h)
constexpr u32 k_stat_words = 9;                // three indirect blocks of three u32, per view

// The sky is `renderer::k_sky` in `lighting.h`, and there are now **three** things that have to
// be the same number rather than two. The resolve pass's clear value and `ResolveParams::sky`,
// because `sky_is_clear` makes the shader discard an empty pixel instead of writing a colour the
// clear already put there — two spellings would make an uncovered pixel take whichever the clear
// said, silently. And the reference path tracer's background, which is quantized on the CPU from
// the same constant (04 §4.8): a third spelling would put a one-byte difference on every empty
// pixel of every comparison, which is a mistake this project has already made once and measured
// (docs/subsystems/renderer.md, "Reference renderer").

// The GPU timer keys zones by name and sums equal names, so a view's own milliseconds need a name
// of their own. View 0 keeps the bare name, so a single-view frame records exactly the zones it
// always has, and the per-pass totals are the sum over the views' names.
enum ZoneKind : u32 {
  k_zone_cull = 0,
  k_zone_hw,
  k_zone_sw,
  k_zone_hiz,
  k_zone_resolve,
  k_zone_deform,
  k_zone_trace,
  k_zone_kinds
};
constexpr const char* k_zone_names[k_zone_kinds][k_max_views] = {
    {"cull", "cull.1", "cull.2", "cull.3", "cull.4", "cull.5", "cull.6", "cull.7"},
    {"hw", "hw.1", "hw.2", "hw.3", "hw.4", "hw.5", "hw.6", "hw.7"},
    {"sw", "sw.1", "sw.2", "sw.3", "sw.4", "sw.5", "sw.6", "sw.7"},
    {"hiz", "hiz.1", "hiz.2", "hiz.3", "hiz.4", "hiz.5", "hiz.6", "hiz.7"},
    {"resolve", "resolve.1", "resolve.2", "resolve.3", "resolve.4", "resolve.5", "resolve.6",
     "resolve.7"},
    {"deform", "deform.1", "deform.2", "deform.3", "deform.4", "deform.5", "deform.6", "deform.7"},
    {"trace", "trace.1", "trace.2", "trace.3", "trace.4", "trace.5", "trace.6", "trace.7"},
};

// The viewport a pass draws one view through, with the renderer's y flip (clip space is y-up like
// core/math, so the height is negative and the origin moves to the bottom of the rectangle).
void set_view_viewport(VkCommandBuffer commands, u32 x, u32 y, u32 width, u32 height) {
  VkViewport viewport{};
  viewport.x = static_cast<f32>(x);
  viewport.y = static_cast<f32>(y + height);
  viewport.width = static_cast<f32>(width);
  viewport.height = -static_cast<f32>(height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(commands, 0, 1, &viewport);
  const VkRect2D scissor{{static_cast<i32>(x), static_cast<i32>(y)}, {width, height}};
  vkCmdSetScissor(commands, 0, 1, &scissor);
}

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

// The views' visibility regions and Hi-Z pyramids, packed back to back into one buffer each. A
// view's region is as big as the rectangle it rasterizes, so for a single view the two buffers are
// exactly the size and layout they have always been.
bool SceneRenderer::Targets::create(const gfx::Device& device, const ViewSet& set,
                                    std::string* error) {
  destroy(device);
  width = set.source_width();
  height = set.source_height();
  views.resize(set.size());
  u64 vis_elements = 0;
  u32 hiz_elements = 0;
  u32 level_base = 0;
  for (u32 v = 0; v < set.size(); ++v) {
    ViewTarget& target = views[v];
    target.width = set[v].source_width;
    target.height = set[v].source_height;
    target.vis_offset = vis_elements;
    target.hiz_mips = gfx::hiz_mip_count(target.width, target.height);
    target.hiz_dispatches = gfx::hiz_dispatch_count(target.hiz_mips);
    const u32 pyramid = gfx::hiz_layout(target.width, target.height, target.hiz_offsets);
    // The offsets are into the shared pyramid buffer, so the cull pass reads its own view's mips
    // without knowing there are others.
    for (u32 m = 0; m < gfx::k_hiz_max_mips; ++m)
      target.hiz_offsets[m] += hiz_elements;
    target.level_base = level_base;
    level_base += target.hiz_dispatches * 2;
    hiz_elements += pyramid;
    vis_elements += u64{target.width} * target.height;
  }
  // The coverage masks live behind the pyramids in the same buffer, because they *are* the Hi-Z
  // build's by-product: one word per 32 x 32 tile of a view's region, which is one word per
  // workgroup of the build's first dispatch. 97 KB for a 11520 x 2160 view against the 100 MB of
  // pyramid in front of it, and one allocation rather than two.
  for (u32 v = 0; v < set.size(); ++v) {
    ViewTarget& target = views[v];
    target.coverage_pitch = gfx::hiz_coverage_pitch(target.width);
    target.coverage_offset = hiz_elements;
    hiz_elements += target.coverage_pitch * gfx::hiz_coverage_pitch(target.height);
  }
  hiz_levels.resize(level_base);
  hiz_dirty = true;
  // TRANSFER_SRC on the visibility buffer is what a capture's id and depth channels read back;
  // nothing in a frame ever copies from it.
  constexpr VkBufferUsageFlags k_buffer_usage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  return gfx::create_image_2d(device, set.width(), set.height(), VK_FORMAT_D32_SFLOAT,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, depth, error) &&
         gfx::create_buffer(device, vis_elements * sizeof(u64), k_buffer_usage, false, vis,
                            error) &&
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
  views.clear();
  hiz_levels.clear();
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

  if (!views_.build(desc.views, width_, height_, error)) {
    destroy();
    return false;
  }
  // The scene's per-frame working set was sized by `resolve_settings`' view count, so a renderer
  // whose layout disagrees would index past the end of the visible list. Say so rather than draw.
  if (views_.size() != scene.view_count()) {
    if (error != nullptr) {
      *error =
          "the view layout does not match the one the scene was built with; pass the same "
          "RenderSettings to resolve_settings and the renderer";
    }
    destroy();
    return false;
  }
  const u32 views = views_.size();

  // The joint buffer holds `k_joint_slots` frames' worth of bone matrices, one region per frame
  // slot, which is what makes the host write safe with no staging copy and no barrier. A renderer
  // with more frames in flight than regions would write a region the GPU is still reading, so it
  // is refused here rather than discovered as a character that twitches one frame in ten.
  if (scene.skinned() && desc.frames_in_flight > k_joint_slots) {
    if (error != nullptr) {
      *error = "a skinned scene supports at most " + std::to_string(k_joint_slots) +
               " frames in flight; the joint buffer has one region per frame slot";
    }
    destroy();
    return false;
  }

  if (!frames_.create(device, desc.frames_in_flight, error) ||
      // Every view records its own cull, raster, Hi-Z and resolve zones, so the pool grows with
      // the layout; one view asks for exactly the 24 it always did.
      !timer_.create(device, desc.frames_in_flight, 24 + 16 * (views - 1), error)) {
    destroy();
    return false;
  }
  params_.resize(desc.frames_in_flight);
  resolves_.resize(desc.frames_in_flight);
  stat_blocks_.resize(desc.frames_in_flight);
  ray_params_.resize(desc.frames_in_flight);
  constexpr VkBufferUsageFlags k_address =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  const u64 stat_bytes = sizeof(u32) * k_stat_words * views;
  bool ok = true;
  for (u32 slot = 0; slot < desc.frames_in_flight && ok; ++slot) {
    // Two cull blocks per view, one resolve block per view with the frame's lights behind the
    // last, and one statistics block per view.
    ok = gfx::create_buffer(device, sizeof(gfx::CullParams) * 2 * views, k_address, true,
                            params_[slot], error) &&
         gfx::create_buffer(
             device, sizeof(gfx::ResolveParams) * views + k_view_lights * sizeof(gfx::ResolveLight),
             k_address, true, resolves_[slot], error) &&
         gfx::create_buffer(device, stat_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                            stat_blocks_[slot], error);
    if (ok) std::memset(stat_blocks_[slot].mapped, 0, stat_bytes);
    if (ok && resolved_.ray_path) {
      ok = gfx::create_buffer(device, sizeof(gfx::RayVisibilityParams) * views, k_address, true,
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
  if (!create_pipelines(error) || !targets_.create(device, views_, error) ||
      !create_color_target(error)) {
    destroy();
    return false;
  }
  fill_view_layout();
  sample_gpu_memory();
  return true;
}

// A driver query, so it is not in the frame path: the caller samples it around a run. It
// survives reset_stats() being called between runs only because every caller samples again.
void SceneRenderer::sample_gpu_memory() noexcept {
  if (device_ == nullptr) return;
  gfx::MemoryBudget budget;
  if (!device_->memory_budget(budget)) return;
  constexpr u64 k_mib = 1024 * 1024;
  stats_.gpu_memory.valid = budget.valid;
  stats_.gpu_memory.budget_mib = budget.budget_bytes / k_mib;
  stats_.gpu_memory.used_mib = budget.used_bytes / k_mib;
  stats_.gpu_memory.device_local_total_mib = budget.device_local_bytes / k_mib;
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
  if (resolved_.deform_pass) {
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

// The warm-up counter goes back with the numbers. A slot's per-frame statistics block survives a
// resize and a settings change, so without this the first frames of a new run would fold in the
// visible counts of the last one — which is how a benchmark of a 640x480 scene came back with
// the 320x240 capture's minimum.
void SceneRenderer::reset_stats() noexcept {
  stats_ = Stats{};
  fill_view_layout();
  submitted_ = 0;
  sample_gpu_memory();  // a reset must not leave a summary with no memory figure at all
}

bool SceneRenderer::resize(u32 width, u32 height, std::string* error) {
  if (width == 0 || height == 0 || (width == width_ && height == height_)) return true;
  frames_.wait_idle();
  width_ = width;
  height_ = height;
  flags_dirty_ = true;  // last frame's visible set no longer matches the Hi-Z
  // The layout is a function of the target, so it is laid out again; the *count* cannot change,
  // because the scene's working set is sized by it, and no layout changes its count with size.
  if (!views_.build(desc_.views, width_, height_, error)) return false;
  fill_view_layout();
  return targets_.create(*device_, views_, error) && create_color_target(error);
}

bool SceneRenderer::poll_shaders(Vector<std::string>& changed, std::string* error) {
  if (shaders_.poll_changes(changed, error) == 0) return true;
  frames_.wait_idle();
  pipelines_.destroy(*device_);
  return create_pipelines(error);
}

// The visible-pair counts of the frame that last used `slot`, per view and summed. The three
// indirect blocks of each view were copied into that view's nine words of the statistics block,
// in the order pass 1, pass 2, software.
// The rectangles and the tiers into the statistics, so a summary can say which monitor a row
// belongs to. They are the layout's and not a frame's, so they are copied when the layout is laid
// out — a capture is one or two frames and would otherwise report a run of zeros.
void SceneRenderer::fill_view_layout() noexcept {
  stats_.view_count = views_.size();
  for (u32 v = 0; v < views_.size(); ++v) {
    const View& source = views_[v];
    ViewStats& view = stats_.views[v];
    view.x = source.rect.x;
    view.y = source.rect.y;
    view.width = source.rect.width;
    view.height = source.rect.height;
    view.source_width = source.source_width;
    view.source_height = source.source_height;
    view.lod_scale = source.quality.lod_scale;
    view.shading_rate = source.quality.shading_rate;
  }
}

void SceneRenderer::fold_visible(u32 slot) {
  if (!resolved_.settings.cull) return;
  const auto* stats = static_cast<const u32*>(stat_blocks_[slot].mapped);
  stats_.visible_hw = 0;
  stats_.visible_pass2 = 0;
  stats_.visible_sw = 0;
  for (u32 v = 0; v < view_count(); ++v) {
    const u32* block = stats + v * k_stat_words;
    ViewStats& view = stats_.views[v];
    view.visible_hw = block[resolved_.vertex_path ? 1 : 0];
    view.visible_pass2 = block[resolved_.vertex_path ? 4 : 3];
    view.visible_sw = block[6];
    stats_.visible_hw += view.visible_hw;
    stats_.visible_pass2 += view.visible_pass2;
    stats_.visible_sw += view.visible_sw;
  }
  const u32 total = stats_.visible_pairs();
  stats_.visible_min = total < stats_.visible_min ? total : stats_.visible_min;
  stats_.visible_max = total > stats_.visible_max ? total : stats_.visible_max;
}

void SceneRenderer::collect_slot(u32 slot) {
  fold_visible(slot);
  if (!timer_.results().empty()) {
    for (u32 v = 0; v < view_count(); ++v) {
      ViewStats& view = stats_.views[v];
      view.gpu_cull += timer_.ms(k_zone_names[k_zone_cull][v]);
      view.gpu_hw += timer_.ms(k_zone_names[k_zone_hw][v]);
      view.gpu_sw += timer_.ms(k_zone_names[k_zone_sw][v]);
      view.gpu_hiz += timer_.ms(k_zone_names[k_zone_hiz][v]);
      view.gpu_resolve += timer_.ms(k_zone_names[k_zone_resolve][v]);
      view.gpu_deform += timer_.ms(k_zone_names[k_zone_deform][v]);
      view.gpu_trace += timer_.ms(k_zone_names[k_zone_trace][v]);
      stats_.gpu_cull += timer_.ms(k_zone_names[k_zone_cull][v]);
      stats_.gpu_hw += timer_.ms(k_zone_names[k_zone_hw][v]);
      stats_.gpu_sw += timer_.ms(k_zone_names[k_zone_sw][v]);
      stats_.gpu_hiz += timer_.ms(k_zone_names[k_zone_hiz][v]);
      stats_.gpu_resolve += timer_.ms(k_zone_names[k_zone_resolve][v]);
      stats_.gpu_deform += timer_.ms(k_zone_names[k_zone_deform][v]);
      stats_.gpu_trace += timer_.ms(k_zone_names[k_zone_trace][v]);
    }
    // The acceleration structure chain is built once from the union of the views' cuts, so it is
    // the frame's cost and no view's.
    stats_.gpu_rt += timer_.ms("records") + timer_.ms("ranges") + timer_.ms("emit") +
                     timer_.ms("clas") + timer_.ms("blas") + timer_.ms("tlas");
    stats_.gpu_clas += timer_.ms("clas");
    stats_.gpu_total += timer_.total_ms();
    ++stats_.timed_frames;
  }
}

void SceneRenderer::collect_visible() { fold_visible(frames_.slot()); }

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

// One frame, from the cull pass to the resolve, for every view of the set. Every parameter block
// here is referenced by a pass body and therefore has to outlive the execute() at the bottom,
// which is why declaring, compiling, and executing are one function rather than three: the render
// graph stores bodies in an arena and requires them to capture by reference.
//
// **Every stage is one graph pass with a loop over the views inside it**, not one pass per view.
// The views write disjoint slices of the same buffers, and the render graph tracks whole buffers,
// so a pass per view would make the graph insert a barrier between views that have no hazard at
// all. The exception is the Hi-Z build, whose mips must be barriered anyway and whose views are
// therefore built one after another so that a view's pyramid is one timer zone.
//
// **What the per-view timings mean.** A zone's timestamps are written at ALL_COMMANDS on both
// ends, so bracketing each view separately orders the views on the queue. The per-view numbers are
// therefore what each view costs on its own, and their sum is what the frame costs measured this
// way — an upper bound on what the views would cost if the driver were free to overlap them.
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
  const u32 views = views_.size();
  const u64 rendered = frame.frame_index;

  // This frame's one camera through the layout: N view-projection matrices sharing one eye and
  // one near plane, differing in orientation and in the shape of the frustum.
  views_.update(frame.camera);

  const Vec3 eye = frame.camera.position;
  const f32 znear = frame.camera.znear;
  const bool direct = resolved_.direct;
  const bool vertex_path = resolved_.vertex_path;
  const bool ray_path = resolved_.ray_path;
  const bool shadows = resolved_.shadows;
  const bool occlusion = resolved_.occlusion;
  const bool rt_chain = resolved_.rt_chain;
  const bool use_hw = settings.raster != RasterMode::Software && !ray_path;
  const bool use_sw = !direct && !ray_path && settings.raster != RasterMode::Hardware &&
                      settings.raster != RasterMode::Vertex && settings.cull;
  // A negative override means "what the settings say", which is every caller but the reference.
  const f32 frame_lod_px = frame.lod_px >= 0.0f ? frame.lod_px : settings.lod_px;
  const u32 cur_flags = static_cast<u32>(rendered % 2);
  const u32 prev_flags = 1 - cur_flags;
  const u32 count_index = vertex_path ? 1u : 0u;
  const f32 raster_mode =
      direct || settings.raster == RasterMode::Hardware || vertex_path || ray_path
          ? gfx::k_raster_hardware
      : settings.raster == RasterMode::Software ? gfx::k_raster_software
                                                : gfx::k_raster_split;

  // What one view needs that the frame cannot share: its region of the visibility buffer, its
  // three runs of the visible list, its indirect blocks, and the addresses of its parameter
  // blocks in the per-slot buffers.
  struct ViewFrame {
    u32 width = 0;
    u32 height = 0;
    u64 vis_address = 0;
    u64 run_address[k_visible_runs] = {};
    u32 run_base[k_visible_runs] = {};
    u64 args_offset = 0;
    u64 block_address[2] = {};
    u64 resolve_address = 0;
    u64 ray_address = 0;
  };
  ViewFrame view_frames[k_max_views];
  gfx::ClusterDrawParams draws[k_max_views][k_visible_runs];
  gfx::DeformParams deform_params[k_max_views][k_visible_runs];
  auto* cull_blocks = static_cast<gfx::CullParams*>(params_[slot].mapped);
  auto* resolve_bytes = static_cast<u8*>(resolves_[slot].mapped);
  const u64 lights_address = resolves_[slot].address + sizeof(gfx::ResolveParams) * views;
  const f32 deform_time = static_cast<f32>(rendered) / 60.0f;

  // ---- the tick's bone matrices (docs/subsystems/animation.md, "The renderer contract") -------
  //
  // One memcpy of the whole population's matrices into this slot's region, then one pass over the
  // deform table rewriting the two words that change. The slot was not reused until the GPU
  // finished the frame that last had it, so neither write races a read, and neither allocates.
  // Everything else about the pool pass — the indirect dispatch, the per-run blocks below — is
  // exactly what a procedural deformer already does.
  u64 deform_table_address = scene.deform_table.address;
  if (scene.skinned()) {
    const u32 span = static_cast<u32>(frame.joints.size());
    const u32 uploaded = span < scene.max_joints() ? span : scene.max_joints();
    if (span > scene.max_joints() && !joint_overflow_warned_) {
      joint_overflow_warned_ = true;
      ENGINE_LOG_WARN(log_renderer, "more bone matrices than the scene was sized for",
                      log::field("joints", span), log::field("max_joints", scene.max_joints()));
    }
    if (uploaded > 0) {
      std::memcpy(scene.joint_slot(slot), frame.joints.data(),
                  u64{uploaded} * sizeof(anim::JointMatrix));
    }
    gfx::DeformDesc* table = scene.deform_frame(slot);
    const std::span<const gfx::DeformDesc> statics = scene.deform_descs();
    const std::span<const u32> owners = scene.deform_instances();
    const u64 base = scene.joint_slot_address(slot);
    for (u32 d = 0; d < statics.size(); ++d) {
      gfx::DeformDesc desc = statics[d];
      const u32 instance = owners[d];
      if (desc.flags == gfx::k_deform_skin && instance < frame.instance_joints.size()) {
        const InstanceJoints& run = frame.instance_joints[instance];
        // A run that is not wholly inside what was uploaded leaves the instance at rest rather
        // than reading past the end: bad offsets are the caller's bug, and a bind-pose character
        // is a visible one where a wild address is a crash.
        if (run.count > 0 && run.first + run.count <= uploaded) {
          desc.joints = base + u64{run.first} * sizeof(anim::JointMatrix);
          desc.joint_count = run.count;
        }
      }
      table[d] = desc;
    }
    deform_table_address = scene.deform_frame_address(slot);
  }

  // The frame's lights, shared by every view and living behind the last view's params block in
  // one buffer. They come out of `frame_lighting` rather than being built here, because the
  // reference path tracer has to light the same scene with the same numbers at the same frame
  // index or a comparison between the two measures the lights (04 §4.8, lighting.h).
  FrameLighting lighting;
  frame_lighting(data, rendered, settings.lights, lighting);
  std::memcpy(resolve_bytes + sizeof(gfx::ResolveParams) * views, lighting.lights,
              sizeof(lighting.lights));

  for (u32 v = 0; v < views; ++v) {
    const View& view = views_[v];
    const ViewTarget& target = targets_.views[v];
    ViewFrame& vf = view_frames[v];
    vf.width = target.width;
    vf.height = target.height;
    vf.vis_address = targets_.vis.address + target.vis_offset * sizeof(u64);
    for (u32 run = 0; run < k_visible_runs; ++run) {
      vf.run_base[run] = scene.visible_base(v, run);
      vf.run_address[run] = scene.visible.address + u64{vf.run_base[run]} * 2 * sizeof(u32);
    }
    vf.args_offset = scene.args_offset(v);
    vf.block_address[0] = params_[slot].address + sizeof(gfx::CullParams) * 2 * v;
    vf.block_address[1] = vf.block_address[0] + sizeof(gfx::CullParams);
    vf.resolve_address = resolves_[slot].address + sizeof(gfx::ResolveParams) * v;

    // ---- the rasterizers' push blocks, one per run -----------------------------------------
    for (u32 run = 0; run < k_visible_runs; ++run) {
      gfx::ClusterDrawParams& draw = draws[v][run];
      draw = gfx::ClusterDrawParams{};
      draw.view_proj = view.view_proj;
      draw.clusters = scene.clusters.address;
      draw.mesh = scene.meshes.address;
      draw.instances = scene.instances.address;
      draw.triangles = scene.triangles.address;
      draw.triangles_per_cluster = triangles_per_cluster;
      // Run 0 with culling off draws every leaf in index order, which is what a null list means.
      draw.visible = run == 0 && !settings.cull ? 0 : vf.run_address[run];
      draw.visible_offset = vf.run_base[run];
      draw.visibility = vf.vis_address;
      draw.width = vf.width;
      draw.height = vf.height;
    }

    // ---- the cull pass's two blocks ----------------------------------------------------------
    gfx::CullParams cull{};
    gfx::set_frustum(cull, frustum_from_view_proj(view.view_proj));
    cull.view_proj = view.view_proj;
    cull.camera = Vec4{eye, znear};
    // The LOD threshold is this view's: a peripheral view lets a cluster be `lod_scale` times as
    // wrong in screen space before the parent group is drawn instead (04 §4.6, foveation). A
    // frame may override the settings' threshold — `FrameDesc::lod_px`, which the reference
    // renderer sets to 0 to make the frame's cut the finest clusters (04 §4.8).
    cull.lod = Vec4{view.proj_scale, frame_lod_px * view.quality.lod_scale, 1.0f, 1.0f};
    cull.raster = Vec4{settings.sw_px, raster_mode, 0.0f, 0.0f};
    cull.cluster_count = cluster_count;
    cull.count_index = count_index;
    cull.cone_cull = settings.cone ? 1u : 0u;
    cull.clusters = scene.clusters.address;
    cull.lods = scene.lods.address;
    cull.visible = vf.run_address[0];
    cull.draw_args = scene.draw_args[0].address + vf.args_offset;
    cull.sw_visible = vf.run_address[2];
    cull.sw_args = scene.sw_args.address + vf.args_offset;
    cull.instances = scene.instances.address;
    cull.meshes = scene.meshes.address;
    cull.instance_count = instance_count;
    cull.pair_count = pair_count;
    if (occlusion) {
      // This view's own pyramid, at its own mip offsets into the shared buffer, and its own slice
      // of the drawn-last-frame flags: a cluster may be occluded in one view and visible in
      // another, so the two-pass state cannot be shared.
      cull.hiz = targets_.hiz.address;
      cull.prev_flags = scene.flags[prev_flags].address + u64{v} * pair_count * sizeof(u32);
      cull.flags = scene.flags[cur_flags].address + u64{v} * pair_count * sizeof(u32);
      cull.hiz_width = vf.width;
      cull.hiz_height = vf.height;
      cull.hiz_mips = target.hiz_mips;
      std::memcpy(cull.hiz_offsets, target.hiz_offsets, sizeof(cull.hiz_offsets));
      cull.pass = 1;
    }
    cull_blocks[v * 2] = cull;
    gfx::CullParams& cull_pass2 = cull_blocks[v * 2 + 1];
    cull_pass2 = cull;
    cull_pass2.pass = 2;
    cull_pass2.visible = vf.run_address[1];
    cull_pass2.draw_args = scene.draw_args[1].address + vf.args_offset;

    // ---- the resolve's block ------------------------------------------------------------------
    gfx::ResolveParams resolve{};
    // `frame_lighting`'s sky, which is `k_sky` — one spelling, because the clear below writes it
    // too and the reference path tracer reads it as its background (04 §4.8, lighting.h).
    resolve.sky = lighting.sky;
    // Both raster paths clear the colour target to exactly this before they draw, so an empty
    // pixel is a fragment whose value is already in the target: the shader discards it instead.
    resolve.sky_is_clear = 1;
    // Skip the visibility read for a 32 x 32 tile with nothing in it. The mask is exact **only**
    // when the last write to the visibility buffer happened before the last Hi-Z build, and that
    // is exactly when two-pass occlusion culling is on: `resolve_settings` allows it only under
    // `--raster hw` and `--raster vertex`, which are the two modes with no software-raster pass
    // and no ray trace after the Hi-Z. Add a pass that writes the buffer after `add_hiz(1)` and
    // this must go off with it.
    if (occlusion) {
      resolve.coverage = targets_.hiz.address + u64{target.coverage_offset} * 4;
      resolve.coverage_pitch = target.coverage_pitch;
    }
    resolve.sun = lighting.sun;
    resolve.camera = Vec4{eye, 0.0f};
    resolve.view_proj = view.view_proj;
    resolve.visibility = vf.vis_address;
    resolve.clusters = scene.clusters.address;
    resolve.mesh = scene.meshes.address;
    resolve.instances = scene.instances.address;
    resolve.visible = settings.cull ? scene.visible.address : 0;
    resolve.triangles = scene.triangles.address;
    resolve.materials = scene.materials.address;
    resolve.cluster_materials = scene.cluster_materials.address;
    resolve.attributes = scene.attributes.address;
    resolve.width = vf.width;
    resolve.height = vf.height;
    resolve.mode = frame.view_mode == ~u32{0} ? settings.view_mode : frame.view_mode;
    resolve.lights = lights_address;
    resolve.light_count = lighting.light_count;
    // Where this view's picture goes in the target, and — for a Panini view — the map from an
    // output pixel back into the wider rectilinear source the rasterizers filled.
    resolve.view_x = view.rect.x;
    resolve.view_y = view.rect.y;
    resolve.out_width = view.rect.width;
    resolve.out_height = view.rect.height;
    if (views_.resample()) {
      resolve.panini_d = views_.panini_d();
      resolve.panini_x = views_.panini_half_width();
      resolve.source_x = views_.source_half_width();
    }
    // Shadows: every light traces against this frame's top-level structure, which holds the same
    // visible list the rasterizer drew from, so a shadow can only come from geometry the picture
    // has. The bias is `FrameLighting::shadow_bias`, shared with the reference for the same
    // reason the lights are.
    resolve.scene = shadows ? scene.tlas_slot() : gfx::k_no_scene;
    resolve.shadow_flags = shadows ? gfx::k_shadow_sun | gfx::k_shadow_lights : 0u;
    resolve.shadow_bias = lighting.shadow_bias;
    std::memcpy(resolve_bytes + sizeof(gfx::ResolveParams) * v, &resolve, sizeof(resolve));

    // ---- the deformed-vertex pool, one block per run ------------------------------------------
    // The pass reads that run's count word out of the cull's own arguments and dispatches one
    // group per visible cluster from a copy of it, so the pool pass costs the cut and nothing
    // else. Two views that both draw a cluster write its vertices twice, with the same value.
    for (u32 run = 0; run < k_visible_runs; ++run) {
      gfx::DeformParams& d = deform_params[v][run];
      d = gfx::DeformParams{};
      d.clusters = scene.clusters.address;
      d.instances = scene.instances.address;
      d.meshes = scene.meshes.address;
      d.attributes = scene.attributes.address;
      d.visible = vf.run_address[run];
      d.visible_count =
          run == 2 ? scene.sw_args.address + vf.args_offset
                   : scene.draw_args[run].address + vf.args_offset + u64{count_index} * sizeof(u32);
      d.pool = scene.deform_pool.address;
      // This frame's table for a skinned scene, the static one otherwise; the two differ only in
      // `joints`/`joint_count`, which nothing but this pass reads.
      d.deform = deform_table_address;
      d.time = deform_time;
      d.amplitude = settings.deform_amplitude;
      d.max_entries = pair_count;
    }

    if (ray_path) {
      gfx::RayVisibilityParams ray{};
      ray.view_proj = view.view_proj;
      ray.inv_view_proj = inverse(view.view_proj);
      ray.camera = Vec4{eye, 0.0f};
      ray.output = vf.vis_address;
      ray.instance_base = 0;  // a CLAS record's base geometry index is the visible entry
      ray.width = vf.width;
      ray.height = vf.height;
      ray.scene = scene.tlas_slot();
      vf.ray_address = ray_params_[slot].address + sizeof(gfx::RayVisibilityParams) * v;
      std::memcpy(static_cast<u8*>(ray_params_[slot].mapped) + sizeof(gfx::RayVisibilityParams) * v,
                  &ray, sizeof(ray));
    }
  }

  // The records pass turns this frame's visible list into CLAS build records and the builds
  // follow on the GPU. The ray path then traces the picture against them; a raster mode with
  // shadows on runs the same chain and the resolve traces the lights against them. With more than
  // one view the geometry is the **union** of the views' cuts: the visible list is run-major, so
  // every view's first run is one contiguous range at the front and one dispatch covers them all.
  gfx::ClusterRecordParams record_params{};
  Vector<gfx::TlasInstance> tlas_instances;
  if (rt_chain) {
    record_params.clusters = scene.clusters.address;
    record_params.vertices = scene.vertices.address;
    record_params.indices8 = scene.indices8.address;
    record_params.instances = scene.instances.address;
    record_params.meshes = scene.meshes.address;
    record_params.instantiate = settings.rt_templates ? 1u : 0u;
    record_params.visible = scene.visible.address;
    // Where the cull pass counted each view's survivors: the mesh path's group count is the first
    // word of the view's indirect block, the vertex path's instance count the second, and the
    // views' blocks are `k_draw_args_bytes` apart.
    record_params.visible_count = scene.draw_args[0].address + u64{count_index} * sizeof(u32);
    record_params.slots = scene.slots.address;
    record_params.instance_counts = scene.instance_counts.address;
    record_params.instance_first = scene.instance_first.address;
    record_params.records = scene.records.address;
    record_params.record_count = scene.record_count.address;
    record_params.blas_records = scene.blas_records.address;
    record_params.clas_addresses = scene.clas_set.addresses.address;
    record_params.instance_count = instance_count;
    record_params.pair_count = pair_count;
    record_params.views = views;
    // One top-level instance per scene instance: the world transform, the instance as the custom
    // index, and that instance's own cluster bottom-level structure. One structure for the whole
    // set: the views share it, which is what makes them one view set and not three renderers.
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
  gfx::RgBuffer rg_deform_args{};
  if (resolved_.deform_pass) {
    rg_pool = graph.import_buffer("deform pool", scene.deform_pool);
    rg_deform_args = graph.import_buffer("deform args", scene.deform_args);
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
  sky.float32[0] = k_sky.x;
  sky.float32[1] = k_sky.y;
  sky.float32[2] = k_sky.z;
  sky.float32[3] = k_sky.w;
  const bool fill_hiz = occlusion && targets.hiz_dirty;
  const bool fill_flags = occlusion && flags_dirty_;
  const bool cull_on = settings.cull;
  const bool deform_on = resolved_.deform_pass;
  const u32 raster_width = targets.width;
  const u32 raster_height = targets.height;

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
        if (deform_on) b.write(rg_deform_args, gfx::Access::TransferWrite);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        if (cull_on) {
          for (u32 v = 0; v < views; ++v) {
            const u64 at = view_frames[v].args_offset;
            for (u32 i = 0; i < 2; ++i) {
              if (vertex_path) {  // {vertexCount, instanceCount = 0, firstVertex, firstInstance}
                vkCmdFillBuffer(cb, scene.draw_args[i].buffer, at, sizeof(u32),
                                triangles_per_cluster * 3);
                vkCmdFillBuffer(cb, scene.draw_args[i].buffer, at + sizeof(u32), sizeof(u32) * 3,
                                0);
              } else {  // {groups = 0, 1, 1}
                vkCmdFillBuffer(cb, scene.draw_args[i].buffer, at, sizeof(u32), 0);
                vkCmdFillBuffer(cb, scene.draw_args[i].buffer, at + sizeof(u32), sizeof(u32) * 2,
                                1);
              }
            }
            vkCmdFillBuffer(cb, scene.sw_args.buffer, at, sizeof(u32), 0);
            vkCmdFillBuffer(cb, scene.sw_args.buffer, at + sizeof(u32), sizeof(u32) * 2, 1);
          }
        }
        if (!direct) vkCmdFillBuffer(cb, targets.vis.buffer, 0, VK_WHOLE_SIZE, 0);
        if (occlusion) vkCmdFillBuffer(cb, scene.flags[cur_flags].buffer, 0, VK_WHOLE_SIZE, 0);
        if (fill_flags) vkCmdFillBuffer(cb, scene.flags[prev_flags].buffer, 0, VK_WHOLE_SIZE, 0);
        if (fill_hiz) vkCmdFillBuffer(cb, targets.hiz.buffer, 0, VK_WHOLE_SIZE, 0);
        if (rt_chain) vkCmdFillBuffer(cb, scene.instance_counts.buffer, 0, VK_WHOLE_SIZE, 0);
        if (deform_on) {  // {groups = 0, 1, 1}; the copy below fills in the count
          for (u32 i = 0; i < views * k_visible_runs; ++i) {
            const u64 at = u64{i} * gfx::k_draw_args_bytes;
            vkCmdFillBuffer(cb, scene.deform_args.buffer, at, sizeof(u32), 0);
            vkCmdFillBuffer(cb, scene.deform_args.buffer, at + sizeof(u32), sizeof(u32) * 2, 1);
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
          vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.cull.pipeline);
          for (u32 v = 0; v < views; ++v) {
            timer.begin(cb, k_zone_names[k_zone_cull][v]);
            vkCmdPushConstants(cb, pipelines.cull.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(u64), &view_frames[v].block_address[block]);
            vkCmdDispatch(cb, gfx::cull_group_count(pair_count), 1, 1);
            timer.end(cb);
          }
        });
  };
  // The deformed-vertex pool for one run of every view's visible list: copy each run's survivor
  // count into an indirect dispatch block, then one workgroup per surviving cluster. Two runs when
  // occlusion culling splits the cut, because pass 2's entries are not known until its cull has
  // run and the pool has to hold pass 1's positions before pass 1 draws.
  auto add_deform = [&](u32 run) {
    const u64 source_offset = run == 2 ? 0 : u64{count_index} * sizeof(u32);
    const gfx::RgBuffer rg_source = run == 2 ? rg_sw_args : rg_args[run];
    graph.add_pass(
        "deform args", gfx::PassKind::Transfer,
        [&, rg_source](gfx::PassBuilder& b) {
          b.read(rg_source, gfx::Access::TransferRead);
          b.write(rg_deform_args, gfx::Access::TransferWrite);
        },
        [&, run, source_offset](VkCommandBuffer cb, gfx::RenderGraph&) {
          const VkBuffer source = run == 2 ? scene.sw_args.buffer : scene.draw_args[run].buffer;
          for (u32 v = 0; v < views; ++v) {
            const VkBufferCopy copy{view_frames[v].args_offset + source_offset,
                                    scene.deform_args_offset(v, run), sizeof(u32)};
            vkCmdCopyBuffer(cb, source, scene.deform_args.buffer, 1, &copy);
          }
        });
    graph.add_pass(
        "deform", gfx::PassKind::Compute,
        [&, rg_source](gfx::PassBuilder& b) {
          b.read(rg_source, gfx::Access::ComputeRead);  // the run's count word
          b.read(rg_deform_args, gfx::Access::IndirectRead);
          b.read(rg_visible, gfx::Access::ComputeRead);
          b.write(rg_pool, gfx::Access::ComputeWrite);
        },
        [&, run](VkCommandBuffer cb, gfx::RenderGraph&) {
          vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.deform.pipeline);
          for (u32 v = 0; v < views; ++v) {
            timer.begin(cb, k_zone_names[k_zone_deform][v]);
            vkCmdPushConstants(cb, pipelines.deform.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(gfx::DeformParams), &deform_params[v][run]);
            vkCmdDispatchIndirect(cb, scene.deform_args.buffer, scene.deform_args_offset(v, run));
            timer.end(cb);
          }
        });
  };
  // One raster pass, one draw per view, each through a viewport at the origin of its own source
  // rectangle: a view rasterizes into its own region of the visibility buffer, in view-local
  // pixels, which is why none of the three rasterizers needed a line changed for multi-view.
  auto add_hw_draw = [&](u32 list, u32 run) {
    graph.add_pass(
        "hardware", gfx::PassKind::Raster,
        [&, list](gfx::PassBuilder& b) {
          b.render_area(raster_width, raster_height);
          b.write(rg_vis, gfx::Access::FragmentReadWrite);
          if (cull_on) {
            b.read(rg_args[list], gfx::Access::IndirectRead);
            b.read(rg_visible, vertex_path ? gfx::Access::VertexRead : gfx::Access::MeshRead);
          }
          if (deform_on)
            b.read(rg_pool, vertex_path ? gfx::Access::VertexRead : gfx::Access::MeshRead);
        },
        [&, list, run](VkCommandBuffer cb, gfx::RenderGraph&) {
          vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            vertex_path ? pipelines.vertex : pipelines.hardware);
          bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
          for (u32 v = 0; v < views; ++v) {
            timer.begin(cb, k_zone_names[k_zone_hw][v]);
            set_view_viewport(cb, 0, 0, view_frames[v].width, view_frames[v].height);
            vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                               sizeof(gfx::ClusterDrawParams), &draws[v][run]);
            if (vertex_path) {
              if (cull_on) {
                vkCmdDrawIndirect(cb, scene.draw_args[list].buffer, view_frames[v].args_offset, 1,
                                  sizeof(u32) * 4);
              } else {
                vkCmdDraw(cb, triangles_per_cluster * 3, leaf_count, 0, 0);
              }
            } else if (cull_on) {
              vkCmdDrawMeshTasksIndirectEXT(cb, scene.draw_args[list].buffer,
                                            view_frames[v].args_offset, 1, sizeof(u32) * 3);
            } else {
              vkCmdDrawMeshTasksEXT(cb, leaf_count, 1, 1);
            }
            timer.end(cb);
          }
        });
  };
  // A view's whole pyramid, a dispatch at a time: one workgroup folds a 32 x 32 tile into six
  // mips through shared memory (hiz_build.slang), so a 15-mip pyramid is three dispatches rather
  // than fifteen, and only the boundaries between them need a barrier. The views are still built
  // one after another so that a view's Hi-Z is one timer zone.
  auto add_hiz = [&](u32 set) {
    for (u32 v = 0; v < views; ++v) {
      const ViewTarget& target = targets.views[v];
      for (u32 d = 0; d < target.hiz_dispatches; ++d) {
        gfx::HizParams* level =
            &targets.hiz_levels[target.level_base + set * target.hiz_dispatches + d];
        const u32 src_mip = gfx::hiz_dispatch_src_mip(d);
        const bool first = d == 0;
        const bool last = d + 1 == target.hiz_dispatches;
        *level = gfx::HizParams{};
        level->from_visibility = first ? 1u : 0u;
        level->src = first ? view_frames[v].vis_address
                           : targets.hiz.address + u64{target.hiz_offsets[src_mip]} * 4;
        level->pyramid = targets.hiz.address;
        level->width = target.width;
        level->height = target.height;
        level->src_mip = src_mip;
        level->src_offset = target.hiz_offsets[src_mip];
        level->levels = gfx::hiz_dispatch_levels(target.hiz_mips, d);
        // Only the dispatch that reads the visibility buffer can say what a tile holds, and its
        // workgroup is exactly a tile — and only the **last** build of the frame says it about
        // the buffer the resolve will read, so the build after pass 1 (`set` 0) writes no mask at
        // all rather than one nothing looks at.
        level->coverage =
            first && set == 1 ? targets.hiz.address + u64{target.coverage_offset} * 4 : 0;
        const u32 src_w = gfx::hiz_mip_extent(target.width, src_mip);
        const u32 src_h = gfx::hiz_mip_extent(target.height, src_mip);
        graph.add_pass(
            "hiz", gfx::PassKind::Compute,
            [&, first](gfx::PassBuilder& b) {
              if (first) b.read(rg_vis, gfx::Access::ComputeRead);
              b.write(rg_hiz, gfx::Access::ComputeReadWrite);
            },
            [&, level, first, last, v, src_w, src_h](VkCommandBuffer cb, gfx::RenderGraph&) {
              if (first) timer.begin(cb, k_zone_names[k_zone_hiz][v]);
              vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.hiz.pipeline);
              vkCmdPushConstants(cb, pipelines.hiz.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                 sizeof(*level), level);
              vkCmdDispatch(cb, gfx::hiz_group_count(src_w), gfx::hiz_group_count(src_h), 1);
              if (last) timer.end(cb);
            });
      }
    }
  };

  if (cull_on) add_cull(0, 0);
  if (deform_on) add_deform(0);
  if (direct) {
    // The direct path is single-view by construction: it draws mesh shaders straight to color.
    const gfx::ClusterDrawParams* params = &draws[0][0];
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
        [&, params](VkCommandBuffer cb, gfx::RenderGraph&) {
          timer.begin(cb, k_zone_names[k_zone_hw][0]);
          vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.direct);
          bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
          vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                             sizeof(*params), params);
          if (cull_on) {
            vkCmdDrawMeshTasksIndirectEXT(cb, scene.draw_args[0].buffer, 0, 1, sizeof(u32) * 3);
          } else {
            vkCmdDrawMeshTasksEXT(cb, leaf_count, 1, 1);
          }
          timer.end(cb);
        });
  } else {
    if (use_hw) add_hw_draw(0, 0);
    if (occlusion) {
      add_hiz(0);
      add_cull(1, 1);
      if (deform_on) add_deform(1);
      add_hw_draw(1, 1);
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
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.software.pipeline);
            for (u32 v = 0; v < views; ++v) {
              timer.begin(cb, k_zone_names[k_zone_sw][v]);
              vkCmdPushConstants(cb, pipelines.software.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                 sizeof(gfx::ClusterDrawParams), &draws[v][2]);
              vkCmdDispatchIndirect(cb, scene.sw_args.buffer, view_frames[v].args_offset);
              timer.end(cb);
            }
          });
    }
    if (rt_chain) {
      // The frame's cut becomes the frame's ray tracing geometry with no CPU in between: bucket
      // the visible entries by instance, prefix-sum the per-instance counts, emit the dense CLAS
      // records, build every CLAS in one command, build one cluster bottom-level structure per
      // instance, and top-level over them. The picture is traced against it under the ray path
      // and the shadow rays are traced against it whenever shadows are on, so the chain is the
      // same passes in the same order for both. Its dispatches cover every view's first run.
      const gfx::ComputePipeline* record_passes[3] = {&pipelines.records, &pipelines.record_ranges,
                                                      &pipelines.record_emit};
      const char* record_names[3] = {"records", "ranges", "emit"};
      const u32 union_groups = (views * pair_count + gfx::k_cluster_records_workgroup - 1) /
                               gfx::k_cluster_records_workgroup;
      const u32 record_groups[3] = {union_groups, 1, union_groups};
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
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines.trace.pipeline);
            bindless.bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE);
            for (u32 v = 0; v < views; ++v) {
              timer.begin(cb, k_zone_names[k_zone_trace][v]);
              vkCmdPushConstants(cb, pipelines.trace.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                 sizeof(u64), &view_frames[v].ray_address);
              vkCmdDispatch(cb, gfx::ray_visibility_group_count(view_frames[v].width),
                            gfx::ray_visibility_group_count(view_frames[v].height), 1);
              timer.end(cb);
            }
          });
    }
    // One resolve pass, one fullscreen draw per view through that view's rectangle of the target.
    // The clear covers the whole target once, so a pixel no view owns keeps the sky.
    graph.add_pass(
        "resolve", gfx::PassKind::Raster,
        [&](gfx::PassBuilder& b) {
          b.color_attachment(color, VK_ATTACHMENT_LOAD_OP_CLEAR, sky);
          b.read(rg_vis, gfx::Access::FragmentRead);
          // The per-tile coverage mask the last Hi-Z build left behind it, in the same buffer.
          if (occlusion) b.read(rg_hiz, gfx::Access::FragmentRead);
          if (deform_on) b.read(rg_pool, gfx::Access::FragmentRead);
          if (shadows) {  // the shadow rays traverse them from the fragment stage
            b.read(rt.tlas, gfx::Access::FragmentRayQueryRead);
            for (const gfx::RgBuffer& d : rg_blas_data)
              b.read(d, gfx::Access::FragmentRayQueryRead);
            b.read(rt.clas_data, gfx::Access::FragmentRayQueryRead);
          }
        },
        [&](VkCommandBuffer cb, gfx::RenderGraph&) {
          vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.resolve);
          bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
          for (u32 v = 0; v < views; ++v) {
            timer.begin(cb, k_zone_names[k_zone_resolve][v]);
            const View& view = views_[v];
            set_view_viewport(cb, view.rect.x, view.rect.y, view.rect.width, view.rect.height);
            vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0, sizeof(u64),
                               &view_frames[v].resolve_address);
            vkCmdDraw(cb, 3, 1, 0, 0);
            timer.end(cb);
          }
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
          for (u32 v = 0; v < views; ++v) {
            for (u32 i = 0; i < 3; ++i) {
              const VkBufferCopy copy{view_frames[v].args_offset,
                                      sizeof(u32) * (u64{v} * k_stat_words + 3 * i),
                                      sizeof(u32) * 3};
              vkCmdCopyBuffer(cb, arg_blocks[i]->buffer, stat_target->buffer, 1, &copy);
            }
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
//
// **A capture is in the target's pixels, whatever the layout.** Each view owns a rectangle of the
// target and a region of the visibility buffer, so the walk is per view: a pixel inside a view's
// rectangle reads that view's region, a pixel no view covers is empty, and a view whose source is
// wider than its rectangle — a Panini view — is sampled through the same map the resolve used, so
// an id in a capture names what is under that pixel of the picture. The ids of two views are
// looked up in the *same* list, because the runs share one array and one index space.
bool SceneRenderer::read_visibility(CapturedFrame& out, const CaptureChannels& channels,
                                    std::string* error) {
  Vector<u8> vis_bytes;
  if (!read_buffer(*device_, targets_.vis, targets_.vis.size, vis_bytes, error)) return false;
  Vector<u8> visible_bytes;
  const bool has_list = resolved_.settings.cull && scene_->visible_run_bytes() > 0;
  const u64 list_bytes = scene_->visible_run_bytes() * k_visible_runs * scene_->view_count();
  if (has_list && !read_buffer(*device_, scene_->visible, list_bytes, visible_bytes, error)) {
    return false;
  }
  const auto* values = reinterpret_cast<const u64*>(vis_bytes.data());
  const auto* entries = reinterpret_cast<const u32*>(visible_bytes.data());
  const u32 entry_count = has_list ? static_cast<u32>(list_bytes / 8) : 0;
  const u32 pixel_count = static_cast<u32>(u64{width_} * height_);
  if (channels.ids) out.ids.resize(pixel_count * k_id_words);
  if (channels.depth) out.depth.resize(pixel_count);
  if (channels.ids) {
    for (u32 p = 0; p < pixel_count; ++p) {
      out.ids[p * k_id_words + 0] = k_no_id;
      out.ids[p * k_id_words + 1] = k_no_id;
      out.ids[p * k_id_words + 2] = k_no_id;
    }
  }
  f32 lo = 0.0f;
  f32 hi = 0.0f;
  for (u32 v = 0; v < views_.size(); ++v) {
    const View& view = views_[v];
    const ViewTarget& target = targets_.views[v];
    for (u32 y = 0; y < view.rect.height; ++y) {
      for (u32 x = 0; x < view.rect.width; ++x) {
        const u32 p = (view.rect.y + y) * width_ + view.rect.x + x;
        u32 sx = x;
        u32 sy = y;
        if (views_.resample() && !panini_source_pixel(views_, view, x, y, sx, sy)) continue;
        const u64 value = values[target.vis_offset + u64{sy} * target.width + sx];
        if (value == 0) continue;
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
    }
  }
  out.depth_min = lo;
  out.depth_max = hi;
  return true;
}

}  // namespace engine::renderer
