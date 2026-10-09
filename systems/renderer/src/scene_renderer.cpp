#include <core/base/assert.h>
#include <core/containers/small_vector.h>
#include <core/platform/process.h>
#include <core/time/time.h>
#include <domain/gfx/capture.h>
#include <domain/gfx/display.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/lighting.h>
#include <systems/renderer/scene_renderer.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <renderer_log.h>
#include <shaders/clas_records.spv.h>
#include <shaders/cluster_cull.spv.h>
#include <shaders/cluster_mesh.spv.h>
#include <shaders/cluster_sw_raster.spv.h>
#include <shaders/cluster_vertex.spv.h>
#include <shaders/cluster_vertex_indexed.spv.h>
#include <shaders/deform.spv.h>
#include <shaders/deform_alloc.spv.h>
#include <shaders/hiz_build.spv.h>
#include <shaders/pair_expand.spv.h>
#include <shaders/ray_visibility.spv.h>
#include <shaders/tlas_references.spv.h>
#include <shaders/vertex_expand.spv.h>
#include <shaders/visibility_resolve.spv.h>
#include <shaders/visibility_resolve_rt.spv.h>

namespace engine::renderer {

namespace {

constexpr u32 k_view_lights = k_frame_lights;  // the warm and cool point lights (lighting.h)
// Per view: three indirect blocks of three u32, then the vertex path's indexed draw's cursor
// and fallback count for each of its two runs (gfx::VertexDrawHeader).
constexpr u32 k_stat_words = 13;
// The deformed-vertex pool's allocation record, copied in behind every view's block: it is one
// record for the frame, not one per view, because the pool's budget is the frame's.
constexpr u32 k_alloc_words = sizeof(gfx::DeformAlloc) / sizeof(u32);
// The ray tracing chain's record count block, behind that: built, wanted, and the instances that
// lost their drawn clusters and their casters to the capacity (gfx::ClusterRecordParams).
constexpr u32 k_rt_words = gfx::k_cluster_record_count_words;
// Per shadow cascade, behind the chain's block: its indirect block's three words, then the
// vertex path's indexed-draw cursor and fallback count for its region.
constexpr u32 k_shadow_stat_words = 5;
// Where the cascades' blocks start, in words.
constexpr u64 shadow_stat_base(u32 views) noexcept {
  return u64{k_stat_words} * views + k_alloc_words + k_rt_words;
}
// Where the sky's sums start, in bytes: behind every cascade's block, on a 16-byte boundary.
constexpr u64 sky_stat_offset(u32 views, u32 cascades) noexcept {
  return (sizeof(u32) * (shadow_stat_base(views) + u64{k_shadow_stat_words} * cascades) + 15) &
         ~u64{15};
}
// Where a slot's `gfx::SkyParams` is in its resolve buffer: behind the views' blocks, the lights,
// the shadow maps' block and the ground's detail.
constexpr u64 sky_params_offset(u32 views) noexcept {
  return sizeof(gfx::ResolveParams) * views + k_view_lights * sizeof(gfx::ResolveLight) +
         sizeof(gfx::ShadowMapParams) + sizeof(gfx::GroundDetailParams);
}
// The depth atlas's format: 32-bit float, because a cascade's depth range is the scene's extent
// along the light — kilometres on the desert overlook — and 16 bits of that is 8 cm.
constexpr gfx::Format k_shadow_format = gfx::Format::D32Sfloat;

// The sky is `renderer::k_sky` in `lighting.h`, and there are now **three** things that have to
// be the same number rather than two. The resolve pass's clear value and `ResolveParams::sky`,
// because `sky_is_clear` makes the shader discard an empty pixel instead of writing a colour the
// clear already put there — two spellings would make an uncovered pixel take whichever the clear
// said, silently. And the reference path tracer's background, which is quantized on the CPU from
// the same constant (04 §4.8): a third spelling would put a one-byte difference on every empty
// pixel of every comparison, which is a mistake this project has already made once and measured
// (docs/subsystems/renderer.md, "Reference renderer").

// The stand-in sky's flat colour in the target's encoding (E39). `k_sky` is an SDR picture's
// encoded value, cleared into the target as it is; an HDR target takes the luminance the SDR
// picture shows it at, put at paper white through the same curve and encode as a shaded pixel (a
// scene with no sky has no shoulder, so its knee is 1). Exactly `encoded` for SDR, so the clear,
// `ResolveParams::sky` and the reference's background stay one number there; for HDR only the clear
// takes it, since `ResolveParams::sky` is also the stand-in's ambient light.
Vec4 stand_in_sky(const Vec4& encoded, gfx::DisplayEncoding display,
                  const RenderSettings& settings) {
  if (display == gfx::DisplayEncoding::Sdr) return encoded;
  const DisplayLevels levels = display_levels(settings);
  const f32 linear[3] = {gfx::sdr_decode(encoded.x), gfx::sdr_decode(encoded.y),
                         gfx::sdr_decode(encoded.z)};
  f32 out[3];
  gfx::display_encode_hdr(linear, 1.0f, display, levels.paper_white_nits, levels.peak_nits, out);
  return Vec4{out[0], out[1], out[2], encoded.w};
}

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
void set_view_viewport(gfx::CommandList commands, u32 x, u32 y, u32 width, u32 height) {
  gfx::Viewport viewport{};
  viewport.x = static_cast<f32>(x);
  viewport.y = static_cast<f32>(y + height);
  viewport.width = static_cast<f32>(width);
  viewport.height = -static_cast<f32>(height);
  viewport.min_depth = 0.0f;
  viewport.max_depth = 1.0f;
  commands.set_viewport(viewport);
  const gfx::Rect2D scissor{{static_cast<i32>(x), static_cast<i32>(y)}, {width, height}};
  commands.set_scissor(scissor);
}

// Copies a device buffer into host memory through a staging buffer and a blocking submission.
// Only a capture does this; a frame never reads anything back.
bool read_buffer(const gfx::Device& device, const gfx::BufferResource& source, u64 bytes,
                 Vector<u8>& out, std::string* error) {
  gfx::BufferResource staging;
  if (!gfx::create_buffer(device, bytes, gfx::BufferUsage::TransferDst, true, staging, error))
    return false;
  const bool ok = gfx::submit_immediate(
      device,
      [&](gfx::CommandList cb) {
        const gfx::BufferCopy copy{0, 0, bytes};
        cb.copy_buffer(source.buffer, staging.buffer, copy);
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
  if (direct.valid()) gfx::destroy_pipeline(device, direct);
  if (hardware.valid()) gfx::destroy_pipeline(device, hardware);
  if (vertex.valid()) gfx::destroy_pipeline(device, vertex);
  if (vertex_fallback.valid()) gfx::destroy_pipeline(device, vertex_fallback);
  if (shadow.valid()) gfx::destroy_pipeline(device, shadow);
  if (shadow_fallback.valid()) gfx::destroy_pipeline(device, shadow_fallback);
  if (resolve.valid()) gfx::destroy_pipeline(device, resolve);
  gfx::destroy_compute_pipeline(device, software);
  gfx::destroy_compute_pipeline(device, cull);
  gfx::destroy_compute_pipeline(device, expand);
  gfx::destroy_compute_pipeline(device, deform);
  gfx::destroy_compute_pipeline(device, deform_cache);
  gfx::destroy_compute_pipeline(device, deform_alloc);
  gfx::destroy_compute_pipeline(device, hiz);
  gfx::destroy_compute_pipeline(device, records);
  gfx::destroy_compute_pipeline(device, record_ranges);
  gfx::destroy_compute_pipeline(device, record_emit);
  gfx::destroy_compute_pipeline(device, tlas_references);
  gfx::destroy_compute_pipeline(device, trace);
  gfx::destroy_compute_pipeline(device, pair_expand);
  direct = hardware = vertex = vertex_fallback = shadow = shadow_fallback = resolve = {};
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
  constexpr gfx::BufferUsage k_buffer_usage =
      gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress |
      gfx::BufferUsage::TransferDst | gfx::BufferUsage::TransferSrc;
  return gfx::create_image_2d(device, set.width(), set.height(), gfx::Format::D32Sfloat,
                              gfx::ImageUsage::DepthStencilAttachment, depth, error) &&
         gfx::create_buffer(device, vis_elements * sizeof(u64), k_buffer_usage, false, vis,
                            error) &&
         gfx::create_buffer(device, u64{hiz_elements} * sizeof(f32), k_buffer_usage, false, hiz,
                            error);
}

void SceneRenderer::Targets::destroy(const gfx::Device& device) noexcept {
  if (depth.image.valid()) gfx::destroy_image(device, depth);
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

  // An HDR encoding writes PQ codes or scRGB's linear light, which only its own target holds
  // (gfx/display.h): PQ a 10-bit packed format, scRGB half floats.
  if (desc.display != gfx::DisplayEncoding::Sdr) {
    const bool pq_ok = desc.display == gfx::DisplayEncoding::Pq &&
                       (desc.color_format == gfx::Format::A2B10G10R10Unorm ||
                        desc.color_format == gfx::Format::A2R10G10B10Unorm);
    const bool scrgb_ok = desc.display == gfx::DisplayEncoding::ScRgb &&
                          desc.color_format == gfx::Format::R16G16B16A16Sfloat;
    if (!pq_ok && !scrgb_ok) {
      if (error != nullptr) {
        *error = std::string("the ") + gfx::display_encoding_name(desc.display) +
                 " output encode needs " +
                 (desc.display == gfx::DisplayEncoding::Pq ? "a 10-bit packed" : "a half-float") +
                 " colour target, not " + gfx::format_name(desc.color_format);
      }
      destroy();
      return false;
    }
  }

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
  // The same for the shadow cascades: each has a run of the scene's visible list, so a renderer
  // that draws maps needs a scene built for them.
  if (resolved.csm && scene.shadow_cascades() != resolved.shadow_cascades) {
    if (error != nullptr) {
      *error =
          "the shadow cascades do not match the ones the scene was built with; pass the same "
          "RenderSettings to resolve_settings, the scene and the renderer";
    }
    destroy();
    return false;
  }

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
  // The terrain levels' table is per frame slot for the same reason (GpuScene::terrain_prepare).
  if (scene.terrain_level_count() > 0 && desc.frames_in_flight > k_joint_slots) {
    if (error != nullptr) {
      *error = "a scene whose terrain moves supports at most " + std::to_string(k_joint_slots) +
               " frames in flight; its level table has one region per frame slot";
    }
    destroy();
    return false;
  }

  // A streamed world's tables: one set per frame in flight, so a change is written into a set no
  // frame in flight reads and never waits for the device (GpuScene, "Instances that come and go").
  if (!scene.reserve_table_sets(desc.frames_in_flight, error)) {
    destroy();
    return false;
  }
  // Geometry residency, when the scene was built streamed. It owns the page manager, the staging
  // ring's bookkeeping and the per-slot feedback buffers; the frame owns the passes that clear the
  // feedback, copy the pages in, and copy the feedback out.
  if (!streamer_.create(device, scene, desc.frames_in_flight, desc.page_source, error)) {
    destroy();
    return false;
  }
  // The ray tracing chain's capacity policy starts from what the scene allocated and grows or
  // shrinks it from what the frames build (rt_capacity.h). The tunables are read here, once.
  if (resolved_.rt_chain) {
    RtCapacityConfig config;
    config.limit = scene.rt_capacity_limit();
    config.headroom_pct = rt_headroom_pct_tunable();
    config.shrink_frames = rt_shrink_frames_tunable();
    config.step = rt_step_tunable();
    rt_capacity_.reset(config, scene.rt_capacity());
    note_rt_bytes();
  }
  if (!frames_.create(device, desc.frames_in_flight, error) ||
      // Every view records its own cull, raster, Hi-Z and resolve zones, so the pool grows with
      // the layout; one view asks for exactly the 24 it always did.
      // 28 per view: the two cull passes, the two hardware draws, the two Hi-Z builds, the three
      // pool passes and the three allocations, the software raster, the six acceleration
      // structure zones, the trace and the resolve, with room to spare. The shadow cascades add
      // one cull zone and at most two per cascade plus two (index expansion, pool pass and its
      // allocation, the depth raster), twelve; a streamed world's table update two more; a sky's
      // tables, sky-view, air and sums four more.
      !timer_.create(device, desc.frames_in_flight, 47 + 16 * (views - 1), error)) {
    destroy();
    return false;
  }
  const u32 cascades = resolved.csm ? scene.shadow_cascades() : 0u;
  params_.resize(desc.frames_in_flight);
  resolves_.resize(desc.frames_in_flight);
  stat_blocks_.resize(desc.frames_in_flight);
  ray_params_.resize(desc.frames_in_flight);
  slot_frame_.assign(desc.frames_in_flight, 0);
  slot_submission_.assign(desc.frames_in_flight, 0);
  slot_pairs_.assign(desc.frames_in_flight, 0);
  slot_instances_.assign(desc.frames_in_flight, 0);
  slot_holes_.assign(desc.frames_in_flight, 0);
  slot_table_slots_.assign(desc.frames_in_flight, 0);
  slot_table_pairs_.assign(desc.frames_in_flight, 0);
  slot_terrain_bytes_.assign(desc.frames_in_flight, 0);
  slot_cpu_.assign(desc.frames_in_flight, FrameCpu{});
  constexpr gfx::BufferUsage k_address =
      gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress;
  // The statistics block ends with the sky's sums (`gfx::SkyFrame`), copied there after the
  // frame, for a scene with a sky.
  const u64 stat_bytes = sky_stat_offset(views, cascades) + sizeof(gfx::SkyFrame);
  bool ok = true;
  for (u32 slot = 0; slot < desc.frames_in_flight && ok; ++slot) {
    // Two cull blocks per view and one per shadow cascade behind them, one resolve block per view
    // with the frame's lights behind the last, the shadow maps' block behind those, the ground's
    // detail behind that and the sky's block last, and one statistics block per view.
    ok = gfx::create_buffer(device, sizeof(gfx::CullParams) * (2 * views + cascades), k_address,
                            true, params_[slot], error) &&
         gfx::create_buffer(device, sky_params_offset(views) + sizeof(gfx::SkyParams), k_address,
                            true, resolves_[slot], error) &&
         gfx::create_buffer(device, stat_bytes, gfx::BufferUsage::TransferDst, true,
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
  shaders_.add_embedded("cluster_vertex_indexed", shaders::k_cluster_vertex_indexed_spirv,
                        shaders::k_cluster_vertex_indexed_spirv_size);
  shaders_.add_embedded("vertex_expand", shaders::k_vertex_expand_spirv,
                        shaders::k_vertex_expand_spirv_size);
  shaders_.add_embedded("visibility_resolve", shaders::k_visibility_resolve_spirv,
                        shaders::k_visibility_resolve_spirv_size);
  // The same resolve with the shadow rays in; only a device with acceleration structures may
  // draw with it, because its `g_scenes[]` is binding 3 of the bindless set.
  shaders_.add_embedded("visibility_resolve_rt", shaders::k_visibility_resolve_rt_spirv,
                        shaders::k_visibility_resolve_rt_spirv_size);
  shaders_.add_embedded("clas_records", shaders::k_clas_records_spirv,
                        shaders::k_clas_records_spirv_size);
  shaders_.add_embedded("tlas_references", shaders::k_tlas_references_spirv,
                        shaders::k_tlas_references_spirv_size);
  shaders_.add_embedded("ray_visibility", shaders::k_ray_visibility_spirv,
                        shaders::k_ray_visibility_spirv_size);
  shaders_.add_embedded("deform", shaders::k_deform_spirv, shaders::k_deform_spirv_size);
  shaders_.add_embedded("deform_alloc", shaders::k_deform_alloc_spirv,
                        shaders::k_deform_alloc_spirv_size);
  shaders_.add_embedded("pair_expand", shaders::k_pair_expand_spirv,
                        shaders::k_pair_expand_spirv_size);
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
  // The scene's sky, when it names one (sky.h): its provider made through the registry, its tables'
  // buffers and pipelines, its stars. Nothing for a scene that names none.
  if (!sky_.create(device, scene.data(), shaders_, error)) {
    destroy();
    return false;
  }
  // The world's one clock (renderer.md, "One clock"): the sky stands at the time the ground's
  // surface does, which the GPU scene keeps for a terrain with a detail block and a moving
  // terrain's motion updates every frame. A terrain with neither starts it here, at the scene's own
  // time.
  if (sky_.active() && scene.data().terrain.enabled && !scene.ground_detail()) {
    scene.set_ground_time(scene.data().terrain.time_s);
  }

  graph_ = new gfx::RenderGraph(device);
  if (!create_pipelines(error) || !targets_.create(device, views_, error) ||
      !create_color_target(error) || !create_shadow_maps(error)) {
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

// The sun's depth atlas, its view, and the sampler the resolve gathers it through, registered in
// the scene's bindless set. Nearest filtering and clamping: the resolve reads texels, never a
// filtered value, because it compares every texel of its footprint against its own reference
// (visibility_resolve.slang, `cascade_visibility`).
bool SceneRenderer::create_shadow_maps(std::string* error) {
  if (!resolved_.csm) return true;
  const gfx::Device& device = *device_;
  const u32 cascades = scene_->shadow_cascades();
  const u32 map = resolved_.settings.shadow_map;
  gfx::BindlessSet& bindless = scene_->bindless();
  if (!gfx::create_image_2d(device, map * cascades, map, k_shadow_format,
                            gfx::ImageUsage::DepthStencilAttachment | gfx::ImageUsage::Sampled,
                            shadow_atlas_, error) ||
      !gfx::create_image_view(device, shadow_atlas_, shadow_view_, error) ||
      !gfx::create_sampler(device, gfx::Filter::Nearest, shadow_sampler_, error)) {
    return false;
  }
  shadow_texture_slot_ = bindless.add_sampled_image(shadow_view_, gfx::ImageLayout::ShaderReadOnly);
  shadow_sampler_slot_ = bindless.add_sampler(shadow_sampler_);
  if (shadow_texture_slot_ == gfx::BindlessSet::k_invalid_slot ||
      shadow_sampler_slot_ == gfx::BindlessSet::k_invalid_slot) {
    if (error != nullptr) *error = "no bindless slot for the shadow atlas";
    return false;
  }
  ENGINE_LOG_INFO(log_renderer, "cascaded shadow maps", log::field("cascades", cascades),
                  log::field("texels", map),
                  log::field("atlas_bytes", u64{map} * map * cascades * sizeof(f32)));
  return true;
}

// The GPU is idle (destroy waits), so the slots are released as of now.
void SceneRenderer::destroy_shadow_maps() noexcept {
  if (device_ == nullptr) return;
  if (scene_ != nullptr && scene_->valid()) {
    gfx::BindlessSet& bindless = scene_->bindless();
    if (shadow_texture_slot_ != gfx::BindlessSet::k_invalid_slot)
      bindless.release_sampled_image(shadow_texture_slot_, 0);
    if (shadow_sampler_slot_ != gfx::BindlessSet::k_invalid_slot)
      bindless.release_sampler(shadow_sampler_slot_, 0);
    bindless.recycle(0);
  }
  shadow_texture_slot_ = shadow_sampler_slot_ = gfx::BindlessSet::k_invalid_slot;
  gfx::destroy_sampler(*device_, shadow_sampler_);
  gfx::destroy_image_view(*device_, shadow_view_);
  if (shadow_atlas_.image.valid()) gfx::destroy_image(*device_, shadow_atlas_);
  shadow_sampler_ = {};
  shadow_view_ = {};
  shadow_atlas_ = gfx::ImageResource{};
  cascades_ = ShadowCascades{};
}

bool SceneRenderer::create_color_target(std::string* error) {
  if (!desc_.offscreen) return true;
  if (color_.image.valid()) gfx::destroy_image(*device_, color_);
  color_ = gfx::ImageResource{};
  return gfx::create_image_2d(*device_, width_, height_, desc_.color_format,
                              gfx::ImageUsage::ColorAttachment | gfx::ImageUsage::TransferSrc,
                              color_, error);
}

bool SceneRenderer::create_pipelines(std::string* error) {
  const gfx::Device& device = *device_;
  // **The mesh-shader pipelines are built only for a path that draws with them**, and
  // `cluster_mesh` is not even loaded otherwise. On a device without VK_EXT_mesh_shader
  // `resolve_settings` has already moved hw, direct and auto to the vertex path, and building a
  // mesh pipeline anyway fails `create_mesh_pipeline` — and with it the whole renderer, whatever
  // path was resolved. Every other pipeline below runs on any device `check_availability`
  // accepted, so it is built as before. The first GPU run on the Pascal server found this: 13 of
  // the renderer's cases and every `render.*` capture failed there, while the settings said
  // "vertex" (docs/ci/self-hosted-runners.md, "The first run on the Titan Xp").
  const bool direct_pipeline = resolved_.direct;
  const bool mesh_pipeline = !resolved_.direct && !resolved_.vertex_path && !resolved_.ray_path &&
                             resolved_.settings.raster != RasterMode::Software;
  const gfx::Shader* mesh = nullptr;
  if (direct_pipeline || mesh_pipeline) {
    mesh = shaders_.get("cluster_mesh", error);
    if (mesh == nullptr) return false;
  }
  const gfx::Shader* cull = shaders_.get("cluster_cull", error);
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
    const gfx::Shader* alloc = deform != nullptr ? shaders_.get("deform_alloc", error) : nullptr;
    if (alloc == nullptr ||
        !gfx::create_compute_pipeline(device, deform->module, "deform_main", {},
                                      sizeof(gfx::DeformParams), pipelines_.deform, error) ||
        (scene_->morphed() && !gfx::create_compute_pipeline(
                                  device, deform->module, "deform_cache_main", {},
                                  sizeof(gfx::DeformParams), pipelines_.deform_cache, error)) ||
        !gfx::create_compute_pipeline(device, alloc->module, "deform_alloc_main", {},
                                      sizeof(gfx::DeformAllocParams), pipelines_.deform_alloc,
                                      error)) {
      return false;
    }
  }
  if (resolved_.rt_chain) {
    const gfx::Shader* records = shaders_.get("clas_records", error);
    const gfx::Shader* references =
        records != nullptr ? shaders_.get("tlas_references", error) : nullptr;
    if (references == nullptr) return false;
    if (!gfx::create_compute_pipeline(device, references->module, "tlas_references_main", {},
                                      sizeof(gfx::TlasReferenceParams), pipelines_.tlas_references,
                                      error) ||
        !gfx::create_compute_pipeline(device, records->module, "records_main", {},
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
  // A streamed world's pair-table expansion, which only a scene whose instances come and go runs.
  if (scene_->dynamic()) {
    const gfx::Shader* pairs = shaders_.get("pair_expand", error);
    if (pairs == nullptr ||
        !gfx::create_compute_pipeline(device, pairs->module, "pair_expand_main", {},
                                      sizeof(PairExpandParams), pipelines_.pair_expand, error)) {
      return false;
    }
  }
  if (resolved_.ray_path) {
    const gfx::Shader* trace = shaders_.get("ray_visibility", error);
    if (trace == nullptr) return false;
    const gfx::DescriptorSetLayoutHandle set_layout = bindless.layout();
    if (!gfx::create_compute_pipeline(
            device, trace->module, "trace_main",
            std::span<const gfx::DescriptorSetLayoutHandle>(&set_layout, 1), sizeof(u64),
            pipelines_.trace, error)) {
      return false;
    }
  }
  gfx::GraphicsPipelineDesc vertex_desc;
  vertex_desc.vertex = vertex->module;
  vertex_desc.vertex_entry = "vs_cluster";
  vertex_desc.fragment = vertex->module;
  vertex_desc.fragment_entry = "fs_visibility";
  vertex_desc.layout = bindless.pipeline_layout();
  // The vertex path's culled draw is indexed where the device allows it (ResolvedSettings::
  // vertex_indexed): its own module, because SV_PrimitiveID in that fragment stage is the Geometry
  // capability, and a fallback that capacity-draws what the index budget had no room for with
  // cluster_vertex's own fragment stage. The cull's entry point writes the draw's records.
  if (resolved_.vertex_indexed) {
    const gfx::Shader* indexed = shaders_.get("cluster_vertex_indexed", error);
    const gfx::Shader* expand = indexed != nullptr ? shaders_.get("vertex_expand", error) : nullptr;
    if (expand == nullptr) return false;
    gfx::GraphicsPipelineDesc fallback_desc = vertex_desc;
    fallback_desc.vertex = indexed->module;
    fallback_desc.vertex_entry = "vs_cluster_fallback";
    vertex_desc.vertex = indexed->module;
    vertex_desc.vertex_entry = "vs_cluster_indexed";
    vertex_desc.fragment = indexed->module;
    vertex_desc.fragment_entry = "fs_visibility_indexed";
    if (!gfx::create_graphics_pipeline(device, fallback_desc, pipelines_.vertex_fallback, error) ||
        !gfx::create_compute_pipeline(device, expand->module, "expand_main", {},
                                      sizeof(gfx::VertexExpandParams), pipelines_.expand, error)) {
      return false;
    }
  }
  if (direct_pipeline) {
    gfx::MeshPipelineDesc direct_desc;
    direct_desc.mesh = mesh->module;
    direct_desc.fragment = mesh->module;
    direct_desc.fragment_entry = "fs_color";
    direct_desc.layout = bindless.pipeline_layout();
    direct_desc.color_format = desc_.color_format;
    direct_desc.depth_format = gfx::Format::D32Sfloat;
    direct_desc.depth_test = true;
    direct_desc.depth_write = true;
    if (!gfx::create_mesh_pipeline(device, direct_desc, pipelines_.direct, error)) return false;
  }
  if (mesh_pipeline) {
    gfx::MeshPipelineDesc hw_desc;
    hw_desc.mesh = mesh->module;
    hw_desc.fragment = mesh->module;
    hw_desc.fragment_entry = "fs_visibility";
    hw_desc.layout = bindless.pipeline_layout();
    if (!gfx::create_mesh_pipeline(device, hw_desc, pipelines_.hardware, error)) return false;
  }
  // **The shadow maps are drawn by the picture's own rasterizer**: the same mesh stage, or the
  // same vertex stage (indexed with its fallback, or the capacity draw), with no fragment stage
  // and the depth atlas as the attachment — so a cascade's triangles come off the same 16-bit grid
  // (or the same pool block) through the same arithmetic as the picture's, which is what lets the
  // resolve compare a receiver's own plane against them. Reversed depth, like everything else.
  if (resolved_.csm) {
    if (mesh_pipeline) {
      gfx::MeshPipelineDesc shadow_desc;
      shadow_desc.mesh = mesh->module;
      shadow_desc.fragment = {};
      shadow_desc.layout = bindless.pipeline_layout();
      shadow_desc.depth_format = k_shadow_format;
      shadow_desc.depth_test = true;
      shadow_desc.depth_write = true;
      if (!gfx::create_mesh_pipeline(device, shadow_desc, pipelines_.shadow, error)) return false;
    } else {
      gfx::GraphicsPipelineDesc shadow_desc = vertex_desc;
      shadow_desc.fragment = {};
      shadow_desc.depth_format = k_shadow_format;
      shadow_desc.depth_test = true;
      shadow_desc.depth_write = true;
      if (!gfx::create_graphics_pipeline(device, shadow_desc, pipelines_.shadow, error)) {
        return false;
      }
      if (resolved_.vertex_indexed) {
        shadow_desc.vertex_entry = "vs_cluster_fallback";  // same module as the indexed draw's
        if (!gfx::create_graphics_pipeline(device, shadow_desc, pipelines_.shadow_fallback,
                                           error)) {
          return false;
        }
      }
    }
  }
  gfx::GraphicsPipelineDesc resolve_desc;
  resolve_desc.vertex = resolve->module;
  resolve_desc.vertex_entry = "vs_fullscreen";
  resolve_desc.fragment = resolve->module;
  resolve_desc.fragment_entry = "fs_resolve";
  resolve_desc.layout = bindless.pipeline_layout();
  resolve_desc.color_format = desc_.color_format;
  return gfx::create_graphics_pipeline(device, vertex_desc, pipelines_.vertex, error) &&
         gfx::create_compute_pipeline(device, sw->module, "sw_raster_main", {},
                                      sizeof(gfx::ClusterDrawParams), pipelines_.software, error) &&
         gfx::create_compute_pipeline(device, cull->module,
                                      resolved_.vertex_indexed ? "cull_vertex_main" : "cull_main",
                                      {}, sizeof(u64), pipelines_.cull, error) &&
         gfx::create_compute_pipeline(device, hiz->module, "hiz_build_main", {},
                                      sizeof(gfx::HizParams), pipelines_.hiz, error) &&
         gfx::create_graphics_pipeline(device, resolve_desc, pipelines_.resolve, error);
}

void SceneRenderer::destroy() noexcept {
  if (device_ == nullptr) return;
  const gfx::Device& device = *device_;
  // A streamed world's changes since the last reset, once: what a run's changes cost the renderer
  // on the CPU and the GPU, which no per-frame record says whole.
  if (stats_.tiles.changes > 0 && scene_ != nullptr) {
    const TileStats& t = stats_.tiles;
    const f64 frames = t.table_frames > 0 ? static_cast<f64>(t.table_frames) : 1.0;
    ENGINE_LOG_INFO(log_renderer, "tile changes", log::field("changes", t.changes),
                    log::field("change_ms_mean", t.change_ns / 1.0e6 / static_cast<f64>(t.changes)),
                    log::field("change_ms_max", t.change_ns_max / 1.0e6),
                    log::field("kept", t.kept), log::field("rewritten", t.rewritten),
                    log::field("reused", t.reused), log::field("appended", t.appended),
                    log::field("freed", t.freed), log::field("compactions", t.compactions),
                    log::field("grows", t.grows), log::field("table_frames", t.table_frames),
                    log::field("table_slots", t.table_slots),
                    log::field("table_pairs", t.table_pairs),
                    log::field("tables_ms_mean", t.gpu_tables / frames),
                    log::field("pair_expand_ms_mean", t.gpu_pair_expand / frames),
                    log::field("table_sets", scene_->table_sets()),
                    log::field("hole_pairs", scene_->tile_layout().hole_pairs()),
                    log::field("free_pairs", scene_->tile_layout().free_pairs()),
                    log::field("pairs", scene_->pair_count()));
  }
  frames_.wait_idle();
  if (graph_ != nullptr) {
    graph_->reset();
    delete graph_;
    graph_ = nullptr;
  }
  targets_.destroy(device);
  pipelines_.destroy(device);
  sky_.destroy();
  frame_sky_ = FrameSky{};
  sky_params_address_ = 0;
  destroy_shadow_maps();
  shaders_.destroy();
  if (color_.image.valid()) gfx::destroy_image(device, color_);
  color_ = gfx::ImageResource{};
  for (gfx::BufferResource& b : params_)
    gfx::destroy_buffer(device, b);
  for (gfx::BufferResource& b : resolves_)
    gfx::destroy_buffer(device, b);
  for (gfx::BufferResource& b : stat_blocks_)
    gfx::destroy_buffer(device, b);
  for (gfx::BufferResource& b : ray_params_)
    gfx::destroy_buffer(device, b);
  streamer_.destroy();
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
  recorded_ = 0;
  collected_ = 0;
  flags_dirty_ = true;
  recording_ = false;
  rt_capacity_ = RtCapacity{};
  rt_overflow_warned_ = false;
  stats_.rt = RtStats{};
}

// The warm-up counter goes back with the numbers. A slot's per-frame statistics block survives a
// resize and a settings change, so without this the first frames of a new run would fold in the
// visible counts of the last one — which is how a benchmark of a 640x480 scene came back with
// the 320x240 capture's minimum.
void SceneRenderer::reset_stats() noexcept {
  stats_ = Stats{};
  streamer_.reset_stats();
  stats_.stream = streamer_.stats();
  note_rt_bytes();  // what the chain holds is state, not a count: it survives a reset
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

bool SceneRenderer::set_dynamic_instances(std::span<const SceneInstance> tail,
                                          std::span<const DynamicBlock> blocks, std::string* error,
                                          bool compact) {
  if (scene_ == nullptr) return false;
  if (recording_) {
    if (error != nullptr) *error = "instances change between frames, not inside one";
    return false;
  }
  // **No wait for the device.** The change goes into the CPU's table and is marked for every table
  // set; the next frame flips to a set no frame in flight reads and brings it up to date on the GPU
  // before anything reads it (record_frame). Until 2026-09-25 this waited for every frame in flight
  // and rewrote the one set of tables in place: E35's 3.2 ms a change with a kit of boxes and
  // 15–17 ms with the ashlar kit, a frame's latency every time.
  const i64 started = time::monotonic_ns();
  GpuScene::TileChange change;
  retired_.clear();
  if (!scene_->set_dynamic_instances(tail, blocks, change, &retired_, error, compact)) {
    for (const gfx::BufferResource& buffer : retired_)
      frames_.defer_destroy(buffer);
    retired_.clear();
    return false;
  }
  // What the change outgrew may still be read by a frame in flight: it goes when that frame's slot
  // comes round, which is the one wait there is, and nothing waits for it.
  for (const gfx::BufferResource& buffer : retired_)
    frames_.defer_destroy(buffer);
  retired_.clear();
  // Made again, the flags hold whatever the allocator left: the next frame fills both, which is
  // what a first frame does. Every block that did not move keeps its pairs and so its history, and
  // a pair whose block is new has a flag that only decides which occlusion pass tests it (the
  // picture is the same either way: pass 2 tests everything pass 1 did not draw).
  if (change.grew) flags_dirty_ = true;
  TileStats& tiles = stats_.tiles;
  ++tiles.changes;
  tiles.kept += change.layout.kept;
  tiles.rewritten += change.layout.rewritten;
  tiles.reused += change.layout.reused;
  tiles.appended += change.layout.appended;
  tiles.freed += change.layout.freed;
  tiles.compactions += change.layout.compacted ? 1u : 0u;
  tiles.grows += change.grew ? 1u : 0u;
  const f64 ns = static_cast<f64>(time::monotonic_ns() - started);
  tiles.change_ns += ns;
  tiles.change_ns_max = ns > tiles.change_ns_max ? ns : tiles.change_ns_max;
  return true;
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
  stats_.shadow_casters = 0;
  stats_.triangles_hw = 0;
  stats_.vertex_fallback = 0;
  for (u32 v = 0; v < view_count(); ++v) {
    const u32* block = stats + v * k_stat_words;
    ViewStats& view = stats_.views[v];
    view.visible_hw = block[resolved_.vertex_path ? 1 : 0];
    view.visible_pass2 = block[resolved_.vertex_path ? 4 : 3];
    // The software block counts the software rasterizer's survivors, or — in a frame that traces
    // shadows, which has no software pass — the shadow casters, which are not visible pairs.
    view.visible_sw = resolved_.casters ? 0u : block[6];
    view.shadow_casters = resolved_.casters ? block[6] : 0u;
    stats_.visible_hw += view.visible_hw;
    stats_.visible_pass2 += view.visible_pass2;
    stats_.visible_sw += view.visible_sw;
    stats_.shadow_casters += view.shadow_casters;
    if (resolved_.vertex_indexed) {
      view.triangles_hw = block[9] + block[11];
      view.vertex_fallback = block[10] + block[12];
    }
    stats_.triangles_hw += view.triangles_hw;
    stats_.vertex_fallback += view.vertex_fallback;
  }
  // Said once, because it is a budget question and not a per-frame event: those clusters drew the
  // same triangles through the capacity draw, whose raster pass was 1.8 times the indexed draw's on
  // the TITAN Xp's helmet grid.
  if (stats_.vertex_fallback > 0 && !vertex_fallback_warned_) {
    vertex_fallback_warned_ = true;
    ENGINE_LOG_WARN(log_renderer, "the vertex path's index budget overflowed",
                    log::field("clusters", stats_.vertex_fallback),
                    log::field("index_capacity", scene_->vertex_index_capacity()),
                    log::field("remedy", "raise renderer::k_vertex_index_budget"));
  }
  // The shadow cascades' cuts, behind the allocation record: what each cascade drew, and on the
  // indexed draw what its index budget sent to the fallback.
  stats_.shadow_pairs = 0;
  stats_.shadow_fallback = 0;
  if (resolved_.csm) {
    const u32* shadow = stats + shadow_stat_base(view_count());
    const u32 count_word = resolved_.vertex_path ? 1u : 0u;
    for (u32 c = 0; c < scene_->shadow_cascades(); ++c) {
      const u32* block = shadow + u64{c} * k_shadow_stat_words;
      stats_.shadow_pairs += block[count_word];
      if (resolved_.vertex_indexed) stats_.shadow_fallback += block[4];
    }
    if (stats_.shadow_fallback > 0 && !shadow_fallback_warned_) {
      shadow_fallback_warned_ = true;
      ENGINE_LOG_WARN(log_renderer, "a shadow cascade's index budget overflowed",
                      log::field("clusters", stats_.shadow_fallback),
                      log::field("index_capacity", scene_->vertex_index_capacity()),
                      log::field("remedy", "raise renderer::k_vertex_index_budget"));
    }
  }
  const u32 total = stats_.visible_pairs();
  stats_.visible_min = total < stats_.visible_min ? total : stats_.visible_min;
  stats_.visible_max = total > stats_.visible_max ? total : stats_.visible_max;
  if (resolved_.rt_chain) {
    const u32* rt = stats + u64{k_stat_words} * view_count() + k_alloc_words;
    RtStats& chain = stats_.rt;
    chain.built = rt[0];
    chain.wanted = rt[1];
    chain.peak_wanted = std::max(chain.peak_wanted, chain.wanted);
    if (rt[2] + rt[3] > 0) {
      ++chain.overflow_frames;
      chain.dropped_instances += rt[2];
      chain.dropped_caster_instances += rt[3];
      // Said once, because it is a capacity question: those instances cast no shadow this frame
      // (and under the ray path were not in the picture), the casters' lost only the shadows of
      // what faces away from the camera. Frames before a growth lands can do it; a run that keeps
      // doing it is past its budget.
      if (!rt_overflow_warned_) {
        rt_overflow_warned_ = true;
        ENGINE_LOG_WARN(log_renderer, "the ray tracing chain dropped instances' structures",
                        log::field("wanted", chain.wanted), log::field("built", chain.built),
                        log::field("capacity", scene_->rt_capacity()),
                        log::field("limit", scene_->rt_capacity_limit()),
                        log::field("instances", rt[2]), log::field("caster_instances", rt[3]),
                        log::field("remedy", "raise RenderSettings::rt_budget_mib"));
      }
    }
  }
  if (resolved_.deform_pass) {
    const u32* alloc = stats + u64{k_stat_words} * view_count();
    stats_.deform_vertices = alloc[0];
    stats_.deform_entries = alloc[1];
    stats_.deform_overflow_entries = alloc[2];
    stats_.deform_overflow_vertices = alloc[3];
    stats_.deform_peak_vertices = stats_.deform_vertices > stats_.deform_peak_vertices
                                      ? stats_.deform_vertices
                                      : stats_.deform_peak_vertices;
    // Said once, because it is a budget question and not a per-frame event: those pairs drew
    // their rest pose, which is visible on a character and invisible in a frame time.
    if (stats_.deform_overflow_entries > 0 && !deform_overflow_warned_) {
      deform_overflow_warned_ = true;
      ENGINE_LOG_WARN(log_renderer, "the deformed-vertex pool overflowed",
                      log::field("entries", stats_.deform_overflow_entries),
                      log::field("vertices", stats_.deform_overflow_vertices),
                      log::field("pool_vertices", scene_->deform_pool_vertices()),
                      log::field("remedy", "raise RenderSettings::deform_pool_mib"));
    }
  }
}

void SceneRenderer::collect_slot(u32 slot) {
  fold_visible(slot);
  // The page feedback of the frame that last used this slot, read now that the slot has come
  // around and the GPU is known to be done with it. This is the whole of §4.9's "the CPU reads it
  // back N frames later without stalling": N is `frames_in_flight`.
  const StreamStats before = stats_.stream;
  streamer_.consume(slot);
  stats_.stream = streamer_.stats();
  // The frame on its own, beside the sums: which one it was, its visible counts (just folded),
  // its zones, and what streaming did since the last fold.
  FrameStats& last = stats_.last;
  last = FrameStats{};
  last.frame = slot < slot_frame_.size() ? slot_frame_[slot] : 0;
  last.submission = slot < slot_submission_.size() ? slot_submission_[slot] : 0;
  last.pairs = slot < slot_pairs_.size() ? slot_pairs_[slot] : 0;
  last.instances = slot < slot_instances_.size() ? slot_instances_[slot] : 0;
  last.hole_pairs = slot < slot_holes_.size() ? slot_holes_[slot] : 0;
  last.table_slots = slot < slot_table_slots_.size() ? slot_table_slots_[slot] : 0;
  last.table_pairs = slot < slot_table_pairs_.size() ? slot_table_pairs_[slot] : 0;
  if (last.table_slots > 0) {
    ++stats_.tiles.table_frames;
    stats_.tiles.table_slots += last.table_slots;
    stats_.tiles.table_pairs += last.table_pairs;
  }
  last.visible_hw = stats_.visible_hw;
  last.visible_pass2 = stats_.visible_pass2;
  last.visible_sw = stats_.visible_sw;
  last.shadow_casters = stats_.shadow_casters;
  last.shadow_pairs = stats_.shadow_pairs;
  last.uploads = static_cast<u32>(stats_.stream.uploads - before.uploads);
  last.upload_bytes = stats_.stream.uploads_bytes - before.uploads_bytes;
  last.evictions = static_cast<u32>(stats_.stream.evictions - before.evictions);
  last.requests = static_cast<u32>(stats_.stream.requests - before.requests);
  last.pages_resident = stats_.stream.pages_resident;
  last.pending = stats_.stream.pending;
  last.loads_in_flight = stats_.stream.loads_in_flight;
  last.pool_pages = stats_.stream.pool_pages;
  last.rt_built = stats_.rt.built;
  last.rt_wanted = stats_.rt.wanted;
  last.rt_capacity = scene_->rt_capacity();
  ++stats_.folded;
  if (!timer_.results().empty()) {
    last.timed = true;
    for (u32 v = 0; v < view_count(); ++v) {
      last.gpu_cull += timer_.ms(k_zone_names[k_zone_cull][v]);
      last.gpu_hw += timer_.ms(k_zone_names[k_zone_hw][v]);
      last.gpu_sw += timer_.ms(k_zone_names[k_zone_sw][v]);
      last.gpu_hiz += timer_.ms(k_zone_names[k_zone_hiz][v]);
      last.gpu_resolve += timer_.ms(k_zone_names[k_zone_resolve][v]);
      last.gpu_deform += timer_.ms(k_zone_names[k_zone_deform][v]);
      last.gpu_trace += timer_.ms(k_zone_names[k_zone_trace][v]);
    }
    last.gpu_deform_alloc = timer_.ms("deform alloc");
    last.gpu_rt = timer_.ms("records") + timer_.ms("ranges") + timer_.ms("emit") +
                  timer_.ms("clas") + timer_.ms("blas") + timer_.ms("tlas");
    last.gpu_clas = timer_.ms("clas");
    last.gpu_blas = timer_.ms("blas");
    last.gpu_shadow_cull = timer_.ms("shadow cull");
    last.gpu_shadow = last.gpu_shadow_cull + timer_.ms("shadow");
    last.gpu_pair_expand = timer_.ms("pair expand");
    last.gpu_tables = timer_.ms("table copy") + last.gpu_pair_expand;
    last.gpu_terrain_upload = timer_.ms("terrain upload");
    last.gpu_sky = timer_.ms("sky");
    last.gpu_total = timer_.total_ms();
    stats_.tiles.gpu_tables += last.gpu_tables;
    stats_.tiles.gpu_pair_expand += last.gpu_pair_expand;
  }
  last.terrain_upload_bytes = slot < slot_terrain_bytes_.size() ? slot_terrain_bytes_[slot] : 0;
  last.cpu = slot < slot_cpu_.size() ? slot_cpu_[slot] : FrameCpu{};
  stats_.terrain_upload_bytes += last.terrain_upload_bytes;
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
    // The pool's allocator is one dispatch over every view of a run, so it is the frame's cost
    // and no view's — and it is kept out of `gpu_deform`, which is the pool pass and the number
    // E25's cost rule is about.
    stats_.gpu_deform_alloc += timer_.ms("deform alloc");
    // The acceleration structure chain is built once from the union of the views' cuts, so it is
    // the frame's cost and no view's.
    stats_.gpu_rt += timer_.ms("records") + timer_.ms("ranges") + timer_.ms("emit") +
                     timer_.ms("clas") + timer_.ms("blas") + timer_.ms("tlas");
    stats_.gpu_clas += timer_.ms("clas");
    stats_.gpu_blas += timer_.ms("blas");
    // The cascaded maps are drawn once for every view, so they are the frame's cost too.
    stats_.gpu_shadow_cull += timer_.ms("shadow cull");
    stats_.gpu_shadow += timer_.ms("shadow cull") + timer_.ms("shadow");
    stats_.gpu_terrain_upload += timer_.ms("terrain upload");
    // The sky's passes are drawn once for every view: the frame's cost.
    stats_.gpu_sky += timer_.ms("sky");
    stats_.gpu_total += timer_.total_ms();
    ++stats_.timed_frames;
  }
  fold_sky(slot);
}

// The sky's sums of the frame that last used this slot (gfx::SkyFrame): the exposure it was drawn
// at and what it was metered from.
void SceneRenderer::fold_sky(u32 slot) {
  if (!sky_.active() || resolved_.direct) return;
  const u32 cascades = resolved_.csm ? scene_->shadow_cascades() : 0u;
  gfx::SkyFrame sums;
  std::memcpy(
      &sums,
      static_cast<const u8*>(stat_blocks_[slot].mapped) + sky_stat_offset(view_count(), cascades),
      sizeof(sums));
  stats_.sky.exposure = sums.exposure.x;
  stats_.sky.ev100 = sums.exposure.y;
  stats_.sky.lux = sums.exposure.z;
  stats_.sky.sky_lux = sums.exposure.w;
}

void SceneRenderer::collect_visible() {
  fold_visible(frames_.slot());
  fold_sky(frames_.slot());
}

void SceneRenderer::begin_frame() {
  // The wait for the slot first, on its own clock: it is the GPU's time, and what follows it — the
  // slot's deferred frees, its timestamps, the fold, a step of the chain's capacity — is the CPU's
  // (`FrameCpu`).
  const i64 wait_started = time::monotonic_ns();
  if (const u64 value = frames_.next_slot_value(); value != 0) (void)frames_.wait(value);
  const i64 begin_started = time::monotonic_ns();
  frame_cpu_ = FrameCpu{};
  frame_cpu_.wait_ms = static_cast<f64>(begin_started - wait_started) / 1.0e6;
  commands_ = frames_.begin_frame();
  timer_.begin_frame(commands_, frames_.slot());
  // The frame that last used this slot has completed: its statistics are readable.
  if (submitted_ >= frames_.frames_in_flight()) {
    collect_slot(frames_.slot());
    // What it built is what the ray tracing chain is sized by (rt_capacity.h). A resize waits for
    // the device, so it happens here — before this frame records anything — and only when the
    // demand crossed a threshold, never as a matter of course.
    if (resolved_.rt_chain && !rt_hold_) {
      const u32 next = rt_capacity_.observe(stats_.rt.wanted);
      if (next != scene_->rt_capacity() && !apply_rt_capacity(next, nullptr)) {
        // The allocation failed: the device is out of memory. Say so and fall back to the smallest
        // step, which a frame can still record into, rather than drawing with nothing.
        ENGINE_LOG_ERROR(log_renderer, "the ray tracing chain could not be resized",
                         log::field("capacity", next));
        (void)apply_rt_capacity(rt_capacity_.config().step, nullptr);
      }
    }
  }
  frame_cpu_.begin_ms = static_cast<f64>(time::monotonic_ns() - begin_started) / 1.0e6;
  recording_ = true;
}

bool SceneRenderer::apply_rt_capacity(u32 capacity, std::string* error, bool beyond_budget) {
  GpuScene& scene = *scene_;
  const u32 before = scene.rt_capacity();
  if (capacity == before) return true;
  // Every frame in flight imports the chain's buffers by handle; none may still be reading them.
  const i64 started = time::monotonic_ns();
  frames_.wait_idle();
  if (!scene.resize_ray_tracing(capacity, error, beyond_budget)) return false;
  // A resize waits for the device and makes the chain's buffers again: a hitch in a window, named
  // when it is one (the slow frames' line, submit_frame).
  if (time::monotonic_ns() - started > k_slow_frame_ns) {
    ENGINE_LOG_WARN(log_renderer, "the ray tracing chain's resize was slow",
                    log::field("from", before), log::field("to", scene.rt_capacity()),
                    log::field("ms", static_cast<f64>(time::monotonic_ns() - started) / 1.0e6));
  }
  rt_capacity_.resized(scene.rt_capacity());
  RtStats& chain = stats_.rt;
  if (scene.rt_capacity() > before) {
    ++chain.grows;
  } else {
    ++chain.shrinks;
  }
  note_rt_bytes();
  ENGINE_LOG_INFO(log_renderer, "ray tracing chain resized", log::field("from", before),
                  log::field("to", scene.rt_capacity()), log::field("wanted", chain.wanted),
                  log::field("bytes", scene.rt_bytes()));
  return true;
}

void SceneRenderer::note_rt_bytes() noexcept {
  if (scene_ == nullptr || !resolved_.rt_chain) return;
  RtStats& chain = stats_.rt;
  chain.capacity = scene_->rt_capacity();
  chain.limit = scene_->rt_capacity_limit();
  chain.union_clusters = scene_->rt_union_clusters();
  chain.bytes = scene_->rt_bytes();
  chain.peak_capacity = std::max(chain.peak_capacity, chain.capacity);
  chain.peak_bytes = std::max(chain.peak_bytes, chain.bytes);
}

u64 SceneRenderer::submit_frame(const FrameDesc& frame, std::string* error) {
  ENGINE_ASSERT(recording_, "SceneRenderer::submit_frame: begin_frame was not called");
  const i64 started = time::monotonic_ns();
  const u32 slot = frames_.slot();
  const gfx::ImageResource& color = frame.color.image.valid() ? frame.color : color_;
  if (!color.image.valid()) {
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
  if (frame.wait.valid() || frame.signal.valid()) {
    gfx::FrameContext::PresentSync sync;
    sync.wait = frame.wait;
    sync.signal = frame.signal;
    value = frames_.end_frame(sync);
  } else {
    value = frames_.end_frame();
  }
  recording_ = false;
  if (slot < slot_frame_.size()) {
    slot_frame_[slot] = frame.frame_index;
    slot_submission_[slot] = submitted_;
    slot_pairs_[slot] = scene_->pair_count();
    slot_instances_[slot] = scene_->instance_count();
    slot_holes_[slot] = scene_->dynamic() ? scene_->tile_layout().hole_pairs() : 0u;
    slot_table_slots_[slot] = tables_.write ? tables_.slots : 0u;
    slot_table_pairs_[slot] = tables_.write ? static_cast<u32>(tables_.pairs) : 0u;
  }
  ++recorded_;
  ++submitted_;
  ++stats_.frames;
  targets_.hiz_dirty = false;
  flags_dirty_ = false;
  const i64 finished = time::monotonic_ns();
  stats_.cpu_ns += static_cast<f64>(finished - started);
  // **Where the frame's time went** (`FrameCpu`), kept with the slot and folded into
  // `Stats::last` with the rest of the frame's numbers when the slot comes around: the tables, the
  // terrain's copies and slot records, the passes' setup, the graph's compile and its recording —
  // with the pass whose recording took longest — and the submission.
  const auto ms = [](i64 a, i64 b) { return static_cast<f64>(b - a) / 1.0e6; };
  frame_cpu_.tables_ms = ms(started, phase_ns_[0]);
  frame_cpu_.terrain_ms = ms(phase_ns_[0], phase_ns_[1]);
  frame_cpu_.passes_ms = ms(phase_ns_[1], phase_ns_[2]);
  frame_cpu_.compile_ms = ms(phase_ns_[2], phase_ns_[3]);
  frame_cpu_.record_ms = ms(phase_ns_[3], phase_ns_[4]);
  frame_cpu_.submit_ms = ms(phase_ns_[4], finished);
  i64 slowest = 0;
  for (u32 p = 0; p < graph_->pass_count(); ++p) {
    if (graph_->pass_cpu_ns(p) <= slowest) continue;
    slowest = graph_->pass_cpu_ns(p);
    frame_cpu_.slowest_pass = graph_->pass_name(p);
  }
  frame_cpu_.slowest_pass_ms = static_cast<f64>(slowest) / 1.0e6;
  if (slot < slot_cpu_.size()) slot_cpu_[slot] = frame_cpu_;
  // **A frame whose recording took long says where** (renderer.md, "What a frame waits for"): the
  // same parts, in the log, for a host that keeps no frame records. The flight of 2026-09-30 had
  // 46–457 ms frames and no way to name the part; this line is how the next one is named.
  if (finished - started > k_slow_frame_ns) {
    ENGINE_LOG_WARN(log_renderer, "a frame's recording was slow",
                    log::field("frame", frame.frame_index),
                    log::field("total_ms", ms(started, finished)),
                    log::field("tables_ms", frame_cpu_.tables_ms),
                    log::field("terrain_ms", frame_cpu_.terrain_ms),
                    log::field("passes_ms", frame_cpu_.passes_ms),
                    log::field("compile_ms", frame_cpu_.compile_ms),
                    log::field("record_ms", frame_cpu_.record_ms),
                    log::field("slowest_pass", frame_cpu_.slowest_pass),
                    log::field("slowest_pass_ms", frame_cpu_.slowest_pass_ms),
                    log::field("submit_ms", frame_cpu_.submit_ms));
  }
  return value;
}

void SceneRenderer::abort_frame() {
  if (!recording_) return;
  frames_.end_frame();
  recording_ = false;
}

bool SceneRenderer::render_offscreen(const FrameDesc& frame, std::string* error) {
  // At most twice: once, and once more if the frame's ray tracing structures did not fit. While
  // the second is drawn the capacity policy holds still: that frame's own `begin_frame` folds an
  // older frame's demand, which must not undo the growth before the frame that asked for it is
  // drawn.
  bool ok = true;
  for (u32 attempt = 0; attempt < 2 && ok; ++attempt) {
    begin_frame();
    const u64 value = submit_frame(frame, error);
    if (value == 0) {
      ok = false;
      break;
    }
    if (!frames_.wait(value)) {
      if (error != nullptr) *error = "the GPU did not finish the frame";
      ok = false;
      break;
    }
    collect_visible();
    // **A blocking frame is drawn complete.** The capacity policy sizes the chain from frames that
    // have already finished, so the first frame of a large scene, a camera cut, or the reference
    // renderer's finest cut can want more than it holds. A frame drawn in flight lives with that
    // for the frames a growth takes to land; a frame somebody waits for — a capture, the
    // reference, a test — is the picture they asked for, so it grows the chain to fit (within the
    // budget) and is drawn again. Past the budget there is nothing to grow into, and the frame
    // keeps what the stated rule kept — except a frame that asked to be complete (the reference
    // renderer's), which may go past the budget up to the scene's whole union; the next ordinary
    // frame's policy brings the chain back under it.
    const u32 ceiling =
        frame.rt_complete ? scene_->rt_union_clusters() : scene_->rt_capacity_limit();
    if (attempt > 0 || !resolved_.rt_chain || stats_.rt.wanted <= stats_.rt.built ||
        scene_->rt_capacity() >= ceiling) {
      break;
    }
    u32 capacity = rt_capacity_.grow_to(stats_.rt.wanted);
    if (frame.rt_complete) capacity = std::max(capacity, std::min(stats_.rt.wanted, ceiling));
    ok = apply_rt_capacity(capacity, error, frame.rt_complete);
    rt_hold_ = true;
  }
  rt_hold_ = false;
  return ok;
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
  // A streamed world's tables, before anything reads their addresses: after a change the frame
  // flips `scene.instances` and `scene.pair_table` to the next table set, which no frame in flight
  // reads, and stages that set's changed slots in this slot's staging for the passes below to copy
  // and expand (GpuScene::prepare_tables). Nothing for a scene read whole or a frame with no
  // change.
  if (!scene.prepare_tables(slot, tables_, error)) return false;
  phase_ns_[0] = time::monotonic_ns();
  const bool write_tables = tables_.write;
  // A moving terrain's levels (docs/subsystems/renderer.md, "The dunes in time-lapse"): this frame
  // slot's table of what each level draws, which the cull pass and the pool pass read, and the
  // fields the host handed over since the last frame, which this frame copies onto the device
  // before either pass runs. Nothing for a scene whose terrain does not move.
  scene.terrain_prepare(slot, terrain_frame_);
  phase_ns_[1] = time::monotonic_ns();
  const u64 terrain_table = terrain_frame_.table;
  // The pairs there are, which is what the cull dispatches over and bounds-checks against, and the
  // run length every per-pair buffer is laid out by, which is what every offset into one uses. The
  // two are one number for a scene read whole; a scene whose instances come and go keeps a stride
  // above its count so a tile's instances can arrive without moving a run (GpuScene).
  const u32 pair_count = scene.pair_count();
  const u32 pair_stride = scene.pair_stride();
  const u32 instance_count = scene.instance_count();
  const u32 cluster_count = scene.cluster_count();
  const u32 leaf_count = scene.leaf_count();
  const u32 triangles_per_cluster = scene.triangles_per_cluster();
  const u32 views = views_.size();
  const u64 rendered = frame.frame_index;
  // Geometry residency for this frame: evict to the budget, admit what this frame's upload budget
  // allows, stage the payloads into this slot's region of the ring, and write this slot's residency
  // words. Nothing here touches the device — the copies are recorded by the "page upload" pass
  // below, and the words are in a host-visible region no frame in flight is reading.
  const u64 stream_params = streamer_.prepare(slot);
  const bool streaming = scene.streamed();

  // This frame's one camera through the layout: N view-projection matrices sharing one eye and
  // one near plane, differing in orientation and in the shape of the frustum.
  views_.update(frame.camera);

  // **The frame's origin is its eye** (ADR-0053; renderer.md, "The frame's origin"). Every pass is
  // given it as a `gfx::FrameEye` and works relative to it, and every view's eye is at the origin
  // of that space (`eye`), because the views of a set share the camera's. Nothing below this line
  // holds an absolute position in a float.
  const WorldEye frame_origin = frame_eye(frame.camera);
  const gfx::FrameEye gpu_eye = gfx::frame_eye(frame_origin);
  const Vec3 eye{};
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
  // The shadow casters live in the software rasterizer's run and are counted in its argument
  // block, which a frame that traces shadows never fills (gfx::k_caster_run).
  const bool casters = resolved_.casters;
  ENGINE_ASSERT(!casters || (!use_sw && !occlusion && rt_chain),
                "the caster run is the software run of a frame with neither a software pass nor "
                "occlusion culling");
  // The sun's cascaded shadow maps: a run of the visible list per cascade behind every view's
  // three, culled from the light, drawn depth-only into the atlas, and filtered by the resolve.
  const bool csm = resolved_.csm;
  const u32 cascade_runs = csm ? scene.shadow_cascades() : 0u;
  ENGINE_ASSERT(!csm || (!shadows && !use_sw && !ray_path && !direct),
                "cascaded shadow maps are drawn on the hw and vertex paths, never beside rays");
  // A negative override means "what the settings say", which is every caller but the reference.
  const f32 frame_lod_px = frame.lod_px >= 0.0f ? frame.lod_px : settings.lod_px;
  // The occlusion history ping-pongs on the renderer's own count of frames recorded, **not** on
  // the caller's frame number: that number drives the lights, and a caller may repeat it — a
  // benchmark's warm-up holds frame 0, `render.capture` asks for the same frame twice — and a
  // repeated parity made pass 1 read the flags buffer written two frames earlier, or never. The
  // picture was right either way (pass 2 catches what pass 1 missed), and the visible pairs and
  // the timings were not those of the frame before it (found by the flythrough's warm-up,
  // docs/subsystems/renderer.md "Scenes, camera paths and flythroughs").
  const u32 cur_flags = static_cast<u32>(recorded_ % 2);
  const u32 prev_flags = 1 - cur_flags;
  const u32 count_index = vertex_path ? 1u : 0u;
  // The vertex path's culled draw, indexed (cluster_vertex_indexed.slang): per view and hardware
  // run a header the reset starts from and the expansion's push block.
  const bool vertex_indexed = resolved_.vertex_indexed;
  const gfx::VertexDrawHeader vertex_reset =
      gfx::vertex_draw_reset(scene.vertex_index_capacity(), triangles_per_cluster);
  gfx::VertexExpandParams expand_params[k_max_views][2];
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
  // One allocation block per run, not per view: the allocator's dispatch is a single workgroup
  // that walks every view of the run, because the pool's budget is the frame's and the order the
  // blocks are handed out in has to be a function of the cut and not of the GPU's scheduling.
  gfx::DeformAllocParams alloc_params[k_visible_runs];
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
      if ((desc.stages & gfx::k_deform_stage_skin) != 0 &&
          instance < frame.instance_joints.size()) {
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

  // The **pose** morph weights, on the same contract as the joint matrices: one span in, one
  // write into this slot's region, and each deformed instance's record pointed at its own run of
  // it. The static half is the scene's and is left exactly as it was, which is the whole
  // difference between the two stages.
  //
  // A scene with no morph channels reaches none of this and its records keep the addresses the
  // scene uploaded, so a non-morphed frame is the instructions it always was.
  u32 cache_rebuilds = 0;
  if (scene.morphed()) {
    const u32 channels = scene.morph_channel_count();
    const std::span<const gfx::DeformDesc> statics = scene.deform_descs();
    f32* region = scene.morph_weight_slot(slot);
    for (u32 d = 0; d < statics.size(); ++d) {
      f32* pose = region + u64{d} * 2 * channels + channels;
      const u32 first = statics[d].first_channel;
      for (u32 c = 0; c < statics[d].channel_count; ++c) {
        const u32 at = first + c;
        pose[c] = at < frame.morph_weights.size() ? frame.morph_weights[at] : 0.0f;
      }
    }
    // A morphed scene always has a per-frame table, because the pose weights live in a per-slot
    // region and each record has to point at this slot's.
    gfx::DeformDesc* table = scene.deform_frame(slot);
    const u64 weights_base = scene.morph_weight_slot_address(slot);
    for (u32 d = 0; d < statics.size(); ++d) {
      if (!scene.skinned()) table[d] = statics[d];
      table[d].weights = weights_base + u64{d} * 2 * channels * sizeof(f32);
    }
    deform_table_address = scene.deform_frame_address(slot);
    if (scene.static_cache_dirty()) {
      cache_rebuilds = scene.static_cached_instances();
      scene.clear_static_cache_dirty();
    }
  }

  // The frame's lights, shared by every view and living behind the last view's params block in
  // one buffer. They come out of `frame_lighting` rather than being built here, because the
  // reference path tracer has to light the same scene with the same numbers at the same frame
  // index or a comparison between the two measures the lights (04 §4.8, lighting.h). The sun is
  // where the caller's day has put it (`FrameDesc::sun_time_s`; 0 is the sun it always had).
  //
  // **With a sky** the lights are the sky's (lighting.h, sky.h): its provider's sun and moon at the
  // world's one clock — the time the ground's surface stands at, plus the frame's own offset — and
  // the frame's `gfx::SkyParams` behind the ground's detail in the same buffer.
  const bool sky_on = sky_.active();
  frame_sky_ = FrameSky{};
  u64 sky_address = 0;
  FrameLighting lighting;
  if (sky_on) {
    sky_.evaluate(scene.ground_time_s() + frame.sun_time_s, frame_sky_);
    frame_lighting(data, frame_sky_, lighting, frame.camera.position);
    // The run's exposure (settings) under the frame's own (keys): a frame's fixed value wins, then
    // the run's, and the compensations add.
    ExposureRequest exposure = frame.exposure;
    exposure.compensation_ev += settings.exposure_ev;
    if (!exposure.fixed && settings.exposure_ev100.has_value()) {
      exposure.fixed = true;
      exposure.ev100 = *settings.exposure_ev100;
    }
    gfx::SkyParams block;
    sky_.fill(frame_sky_, views_, frame.camera, exposure, block);
    std::memcpy(resolve_bytes + sky_params_offset(views), &block, sizeof(block));
    sky_address = resolves_[slot].address + sky_params_offset(views);
    SkyStats& s = stats_.sky;
    s.active = true;
    s.time_s = frame_sky_.time_s;
    s.day_of_year = frame_sky_.state.day_of_year;
    s.hour = frame_sky_.state.hour;
    const Vec3 sun = normalize(frame_sky_.state.sun);
    const Vec3 moon = normalize(frame_sky_.state.moon);
    s.sun_elevation_deg = degrees(std::asin(std::clamp(sun.y, -1.0f, 1.0f)));
    s.sun_azimuth_deg = degrees(std::atan2(sun.z, sun.x));
    s.moon_elevation_deg = degrees(std::asin(std::clamp(moon.y, -1.0f, 1.0f)));
    s.moon_azimuth_deg = degrees(std::atan2(moon.z, moon.x));
    s.moon_lit = frame_sky_.state.moon_lit;
    s.moon_illuminance = frame_sky_.moon ? frame_sky_.state.moon_illuminance : 0.0f;
    s.moon_key = frame_sky_.moon_key;
  } else {
    frame_lighting(data, rendered, lighting_options(settings, frame.sun_time_s), lighting,
                   frame.camera.position);
  }
  sky_params_address_ = sky_address;
  std::memcpy(resolve_bytes + sizeof(gfx::ResolveParams) * views, lighting.lights,
              sizeof(lighting.lights));

  // ---- the sun's cascades (shadow_cascades.h) -------------------------------------------------
  //
  // Fit on the CPU from this frame's camera and layout, and written behind the lights for the
  // resolve. The cascades' cut is **the camera's cut over each cascade's box**: the cull pass runs
  // with the cascade's six planes and the camera's own LOD — its eye, its projection scale and
  // its threshold, the finest of the views' — so the LOD test, which does not look at the frustum,
  // makes exactly the decision it makes for the picture. A shadow is cast by the picture's LOD,
  // which is what makes a receiver's own triangle land in the map at the depth the resolve's
  // receiver plane predicts, and a caster outside the frustum by the LOD the camera would draw it
  // at from where it stands.
  const u64 shadow_maps_address =
      resolves_[slot].address + sizeof(gfx::ResolveParams) * views + sizeof(lighting.lights);
  // ---- the sand's detail (renderer.md, "The sand close up") -----------------------------------
  //
  // One block for every view, behind the shadow maps': the scene's numbers and the wind its ground
  // says blows at the time the surface stands at. Zero for a scene that draws none, which is every
  // scene without a terrain detail block: the resolve then reads exactly what it read before.
  u64 ground_detail_address = 0;
  if (scene.ground_detail()) {
    gfx::GroundDetailParams detail = scene.ground_detail_params();
    // Its frame at this frame's eye, which is every view's `camera` (gfx.md, "Far from the
    // origin"): the pattern's lattices are counted from a corner near it, in double.
    gfx::ground_detail_frame(detail, frame.camera.position, frame.camera.position);
    const u64 offset =
        sizeof(gfx::ResolveParams) * views + sizeof(lighting.lights) + sizeof(gfx::ShadowMapParams);
    std::memcpy(resolve_bytes + offset, &detail, sizeof(detail));
    ground_detail_address = resolves_[slot].address + offset;
  }
  u32 shadow_lod_view = 0;
  if (csm) {
    ShadowFit fit;
    fit.cascades = cascade_runs;
    fit.resolution = settings.shadow_map;
    fit.distance = settings.shadow_distance;
    // The maps follow the key light: the sun, or on a sky's night the moon, the array's light 0
    // (`gfx::ShadowMapParams::light` is its index plus one).
    const Vec3 key =
        frame_sky_.moon_key ? lighting.lights[0].position_radius.xyz() : lighting.sun.xyz();
    fit_shadow_cascades(views_, frame.camera, key, data.center, data.radius, fit, cascades_);
    gfx::ShadowMapParams maps;
    shadow_map_params(cascades_, shadow_texture_slot_, cascade_runs, shadow_sampler_slot_, maps);
    if (settings.shadow_normal_offset >= 0.0f) maps.normal_offset = settings.shadow_normal_offset;
    maps.light = frame_sky_.moon_key ? 1u : 0u;
    std::memcpy(resolve_bytes + sizeof(gfx::ResolveParams) * views + sizeof(lighting.lights), &maps,
                sizeof(maps));
    // The view whose cut is finest: the largest projection scale over threshold.
    f32 finest = -1.0f;
    for (u32 v = 0; v < views; ++v) {
      const f32 threshold = frame_lod_px * views_[v].quality.lod_scale;
      const f32 ratio = threshold > 0.0f ? views_[v].proj_scale / threshold : 3.0e38f;
      if (ratio > finest) {
        finest = ratio;
        shadow_lod_view = v;
      }
    }
  }

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
      // The frame's origin, read out of this view's first cull block (`CullParams::eye`).
      draw.eye = vf.block_address[0] + gfx::k_cull_params_eye_offset;
      // Run 0 with culling off draws every leaf in index order, which is what a null list means.
      draw.visible = run == 0 && !settings.cull ? 0 : vf.run_address[run];
      // The indexed draw and its fallback read the run's records, which carry the entry.
      if (vertex_indexed && run < 2) {
        draw.visible = scene.vertex_records.address + scene.vertex_records_offset(v, run);
      }
      draw.visible_offset = vf.run_base[run];
      draw.visibility = vf.vis_address;
      draw.extent = gfx::draw_extent(vf.width, vf.height);
    }

    // ---- the cull pass's two blocks ----------------------------------------------------------
    gfx::CullParams cull{};
    gfx::set_frustum(cull, frustum_from_view_proj(view.view_proj));
    cull.view_proj = view.view_proj;
    cull.camera = Vec4{eye, znear};
    cull.eye = gpu_eye;
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
    // The way back from a pixel's pair to the entry this frame drew it as (gfx.md, "The tie rule"):
    // this view's table, and where each run the pass appends to starts in the whole list.
    cull.pair_entries = scene.pair_entries_address(v);
    cull.visible_base = vf.run_base[0];
    cull.sw_visible_base = vf.run_base[2];
    cull.instances = scene.instances.address;
    cull.meshes = scene.meshes.address;
    cull.instance_count = instance_count;
    cull.pair_count = pair_count;
    cull.terrain = terrain_table;
    // Geometry streaming: the drawing rule, and the feedback it writes. Null for a scene uploaded
    // whole, and the pass then runs exactly the instructions it always ran.
    cull.streaming = stream_params;
    cull.page_count = scene.page_count();
    cull.max_requests = scene.max_requests();
    if (vertex_indexed) {
      cull.vertex_draw = scene.vertex_headers.address + scene.vertex_header_offset(v, 0);
      cull.vertex_records = scene.vertex_records.address + scene.vertex_records_offset(v, 0);
      for (u32 run = 0; run < 2; ++run) {
        gfx::VertexExpandParams& e = expand_params[v][run];
        e = gfx::VertexExpandParams{};
        e.header = scene.vertex_headers.address + scene.vertex_header_offset(v, run);
        e.records = scene.vertex_records.address + scene.vertex_records_offset(v, run);
        e.indices = scene.vertex_indices.address + scene.vertex_indices_offset(v, run);
        e.clusters = scene.clusters.address;
        e.triangles = scene.triangles.address;
      }
    }
    // What the cone test rejects, kept for the shadows: this view's caster run, counted in this
    // view's software argument block (docs/subsystems/renderer.md, "Shadows").
    if (casters) {
      cull.casters = vf.run_address[gfx::k_caster_run];
      cull.caster_count = scene.sw_args.address + vf.args_offset;
    }
    if (occlusion) {
      // This view's own pyramid, at its own mip offsets into the shared buffer, and its own slice
      // of the drawn-last-frame flags: a cluster may be occluded in one view and visible in
      // another, so the two-pass state cannot be shared.
      cull.hiz = targets_.hiz.address;
      cull.prev_flags = scene.flags[prev_flags].address + u64{v} * pair_stride * sizeof(u32);
      cull.flags = scene.flags[cur_flags].address + u64{v} * pair_stride * sizeof(u32);
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
    cull_pass2.visible_base = vf.run_base[1];
    cull_pass2.draw_args = scene.draw_args[1].address + vf.args_offset;
    if (vertex_indexed) {
      cull_pass2.vertex_draw = scene.vertex_headers.address + scene.vertex_header_offset(v, 1);
      cull_pass2.vertex_records = scene.vertex_records.address + scene.vertex_records_offset(v, 1);
    }

    // ---- the resolve's block ------------------------------------------------------------------
    gfx::ResolveParams resolve{};
    // `frame_lighting`'s sky, which is `k_sky` — one spelling, because the clear below writes it
    // too and the reference path tracer reads it as its background (04 §4.8, lighting.h).
    // It is also the stand-in's ambient sky (linear light, `ambient_radiance`), so an HDR target
    // keeps it as it is: there only the clear is the HDR-encoded colour (`stand_in_sky`), and the
    // empty pixel the shader discards keeps that.
    resolve.sky = lighting.sky;
    // Both raster paths clear the colour target to exactly this before they draw, so an empty
    // pixel is a fragment whose value is already in the target: the shader discards it instead.
    // With a sky no clear is the sky, and every uncovered pixel is written (sky_pixel).
    resolve.sky_is_clear = sky_on ? 0u : 1u;
    resolve.sky_params = sky_address;
    resolve.sky_view = v;
    // The output encode's dither (gfx/display.h, ADR-0052): one code step of the target the
    // pipelines draw into — 255 at 8 bits, 1023 at 10, none for a float target — so the picture is
    // quantized once, at the depth the target holds.
    resolve.dither_steps = settings.dither ? gfx::display_steps(desc_.color_format) : 0u;
    // The display the encode is for (E39): SDR, or an HDR encoding at the run's paper white and
    // peak (`display_levels`: the setting, the tunable, what the display reports, the default).
    if (desc_.display != gfx::DisplayEncoding::Sdr) {
      const DisplayLevels levels = display_levels(settings);
      resolve.display_encoding = static_cast<u32>(desc_.display);
      resolve.display_paper_white_nits = levels.paper_white_nits;
      resolve.display_peak_nits = levels.peak_nits;
    }
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
    // The deformed shading normals the chain wrote beside the positions. Null for a scene with no
    // morph channels, and the resolve then reads the rest attribute stream exactly as it did.
    resolve.normal_pool = scene.normal_pool_address();
    resolve.sun = lighting.sun;
    // The ground below the hemisphere's horizon: the scene's, shared with the reference like the
    // sun and the sky, so its escaped rays see the ground this term stands for.
    resolve.ground = lighting.ground;
    resolve.camera = Vec4{eye, 0.0f};
    resolve.eye = gpu_eye;
    resolve.view_proj = view.view_proj;
    resolve.visibility = vf.vis_address;
    resolve.clusters = scene.clusters.address;
    resolve.mesh = scene.meshes.address;
    resolve.instances = scene.instances.address;
    resolve.visible = settings.cull ? scene.visible.address : 0;
    // A pixel's id is the scene's pair: the scene's pair table decodes it, and this view's
    // pair-to-entry table says which entry the frame drew it as, which only a deformed instance's
    // pool block needs. Without culling the list is null and the pair is the entry.
    resolve.pairs = scene.pair_table.address;
    resolve.pair_entries = settings.cull ? scene.pair_entries_address(v) : 0;
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
    // has. The offset a ray leaves its surface by is `FrameLighting::shadow_bias` plus
    // `shadow_bias_steps` of the surface's own grid, shared with the reference for the same reason
    // the lights are.
    resolve.scene = shadows ? scene.tlas_slot() : gfx::k_no_scene;
    resolve.shadow_flags = shadows ? gfx::k_shadow_sun | gfx::k_shadow_lights : 0u;
    resolve.shadow_bias = lighting.shadow_bias;
    resolve.shadow_bias_steps = lighting.shadow_bias_steps;
    // Or the sun's cascaded maps: the same query, filtered out of the atlas, and the sun alone.
    if (csm) {
      resolve.shadow_flags = gfx::k_shadow_sun | gfx::k_shadow_cascades;
      // On a sky's night the maps are the moon's, a light of the array, which the query then asks.
      if (frame_sky_.moon_key) resolve.shadow_flags |= gfx::k_shadow_lights;
      resolve.shadow_maps = shadow_maps_address;
    }
    resolve.ground_detail = ground_detail_address;
    // The scene grid's UV frame, which a world tile's material lookup places the tile's corner in
    // (its UVs are from the corner; `terrain_uv_frame` keeps the numbers inside 32 bits).
    resolve.terrain_uv_x0_mm = static_cast<i32>(scene.terrain_uv().x0_mm);
    resolve.terrain_uv_z0_mm = static_cast<i32>(scene.terrain_uv().z0_mm);
    resolve.terrain_uv_per_mm =
        static_cast<f32>(1.0 / static_cast<f64>(scene.terrain_uv().size_mm));
    std::memcpy(resolve_bytes + sizeof(gfx::ResolveParams) * v, &resolve, sizeof(resolve));

    // ---- the deformed-vertex pool, one block per run ------------------------------------------
    // The pass reads that run's count word out of the cull's own arguments and dispatches one
    // group per visible cluster from a copy of it, so the pool pass costs the cut and nothing
    // else. Two views that both draw a cluster get a block each, written with the same value.
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
      d.slots = scene.deform_slots.address;
      d.morph = scene.morph_params_address();
      d.normal_pool = scene.normal_pool_address();
      d.time = deform_time;
      d.amplitude = settings.deform_amplitude;
      d.max_entries = pair_stride;
      d.visible_offset = vf.run_base[run];
      d.terrain = terrain_table;
    }

    if (ray_path) {
      gfx::RayVisibilityParams ray{};
      ray.view_proj = view.view_proj;
      ray.clip_to_ray = view.clip_to_ray;
      ray.camera = Vec4{eye, 0.0f};
      ray.output = vf.vis_address;
      ray.instance_base = 0;  // a CLAS record's base geometry index is the visible entry
      ray.width = vf.width;
      ray.height = vf.height;
      ray.scene = scene.tlas_slot();
      // The id the pass writes is the entry's pair (gfx.md, "The tie rule"), and its second look
      // at a tied distance leaves the shadow casters' run out.
      ray.visible = scene.visible.address;
      ray.instances = scene.instances.address;
      ray.meshes = scene.meshes.address;
      ray.caster_base = casters ? scene.visible_base(0, gfx::k_caster_run) : ~u32{0};
      vf.ray_address = ray_params_[slot].address + sizeof(gfx::RayVisibilityParams) * v;
      std::memcpy(static_cast<u8*>(ray_params_[slot].mapped) + sizeof(gfx::RayVisibilityParams) * v,
                  &ray, sizeof(ray));
    }
  }

  // ---- the shadow cascades' cull blocks, draws, index expansions and pool passes ---------------
  //
  // One cull block per cascade behind the views' pairs: the cascade's planes, the camera's LOD, no
  // normal cones (a caster's facing to the camera says nothing about the light), no occlusion
  // (there is no Hi-Z from the light) and no pair-to-entry table (the resolve decodes the picture's
  // pairs, never a cascade's). Everything goes to the hardware run, which the cascade's own draw
  // reads.
  const u32 cascades_drawn = csm ? cascades_.count : 0u;
  gfx::ClusterDrawParams shadow_draws[gfx::k_max_shadow_cascades];
  gfx::VertexExpandParams shadow_expand[gfx::k_max_shadow_cascades];
  gfx::DeformParams shadow_deform[gfx::k_max_shadow_cascades];
  gfx::DeformAllocParams shadow_alloc{};
  const u32 shadow_map = settings.shadow_map;
  for (u32 c = 0; c < cascades_drawn; ++c) {
    const gfx::ShadowCascade& cascade = cascades_.cascades[c];
    const View& lod_view = views_[shadow_lod_view];
    const u32 base = scene.cascade_base(c);
    const u64 run_address = scene.visible.address + u64{base} * 2 * sizeof(u32);
    const u64 args_address = scene.shadow_args.address + scene.shadow_args_offset(c);
    gfx::CullParams cull{};
    gfx::set_frustum(cull, frustum_from_view_proj(cascade.view_proj));
    cull.view_proj = cascade.view_proj;
    cull.camera = Vec4{eye, znear};
    cull.eye = gpu_eye;
    cull.lod = Vec4{lod_view.proj_scale,
                    frame_lod_px * lod_view.quality.lod_scale * settings.shadow_lod_scale, 1.0f,
                    settings.shadow_frustum ? 1.0f : 0.0f};
    cull.raster = Vec4{settings.sw_px, gfx::k_raster_hardware, 0.0f, 0.0f};
    cull.cluster_count = cluster_count;
    cull.count_index = count_index;
    cull.cone_cull = 0;
    cull.clusters = scene.clusters.address;
    cull.lods = scene.lods.address;
    cull.visible = run_address;
    cull.draw_args = args_address;
    cull.sw_visible = run_address;  // never appended to: the raster mode is hardware
    cull.sw_args = args_address;
    cull.visible_base = base;
    cull.sw_visible_base = base;
    cull.instances = scene.instances.address;
    cull.meshes = scene.meshes.address;
    cull.instance_count = instance_count;
    cull.pair_count = pair_count;
    cull.terrain = terrain_table;
    cull.streaming = stream_params;
    cull.page_count = scene.page_count();
    cull.max_requests = scene.max_requests();
    if (vertex_indexed) {
      cull.vertex_draw = scene.vertex_headers.address + scene.shadow_vertex_header_offset(c);
      cull.vertex_records = scene.vertex_records.address + scene.shadow_vertex_records_offset(c);
      gfx::VertexExpandParams& e = shadow_expand[c];
      e = gfx::VertexExpandParams{};
      e.header = cull.vertex_draw;
      e.records = cull.vertex_records;
      e.indices = scene.vertex_indices.address + scene.shadow_vertex_indices_offset(c);
      e.clusters = scene.clusters.address;
      e.triangles = scene.triangles.address;
    }
    cull_blocks[views * 2 + c] = cull;

    gfx::ClusterDrawParams& draw = shadow_draws[c];
    draw = gfx::ClusterDrawParams{};
    draw.view_proj = cascade.view_proj;
    draw.clusters = scene.clusters.address;
    draw.mesh = scene.meshes.address;
    draw.instances = scene.instances.address;
    draw.triangles = scene.triangles.address;
    draw.eye = params_[slot].address + sizeof(gfx::CullParams) * (views * 2 + c) +
               gfx::k_cull_params_eye_offset;
    draw.visible = vertex_indexed ? cull.vertex_records : run_address;
    draw.visible_offset = base;
    draw.visibility = 0;  // depth only: no fragment stage reads it
    draw.extent = gfx::draw_extent(shadow_map, shadow_map);

    if (resolved_.deform_pass) {
      gfx::DeformParams& d = shadow_deform[c];
      d = deform_params[0][0];
      d.visible = run_address;
      d.visible_count = args_address + u64{count_index} * sizeof(u32);
      d.visible_offset = base;
    }
  }
  if (resolved_.deform_pass && cascades_drawn > 0) {
    // The cascades' runs, in one dispatch after the camera's three have been allocated, carrying
    // the frame's cursor on: every cascade entry of a deformed instance gets its own block.
    shadow_alloc.clusters = scene.clusters.address;
    shadow_alloc.instances = scene.instances.address;
    shadow_alloc.visible = scene.visible.address;
    shadow_alloc.visible_counts = scene.shadow_args.address + u64{count_index} * sizeof(u32);
    shadow_alloc.slots = scene.deform_slots.address;
    shadow_alloc.alloc = scene.deform_alloc.address;
    shadow_alloc.pool_vertices = scene.deform_pool_vertices();
    shadow_alloc.pair_count = pair_stride;
    shadow_alloc.views = cascades_drawn;
    shadow_alloc.first_entry = scene.cascade_base(0);
    shadow_alloc.reset = 0;
  }

  // ---- the pool's allocator, one block per run ------------------------------------------------
  //
  // The budget is the *frame's*, so the runs share one cursor: run 0 resets it and the later runs
  // carry it forward across the barriers the render graph already puts between them. Each block
  // covers every view of its run in one dispatch of one workgroup, which is what lets the cursor
  // advance by what was actually placed rather than by what was asked for. While the cut fits the
  // budget the order the blocks come out in does not reach the picture — every entry reads its
  // own block — and a capture is byte-identical; past the budget it does, and it is the cull
  // pass's atomics, so it is not reproducible (`deform_alloc.slang` says so at length).
  if (resolved_.deform_pass) {
    for (u32 run = 0; run < k_visible_runs; ++run) {
      gfx::DeformAllocParams& a = alloc_params[run];
      a = gfx::DeformAllocParams{};
      a.clusters = scene.clusters.address;
      a.instances = scene.instances.address;
      a.visible = scene.visible.address;
      a.visible_counts = run == 2 ? scene.sw_args.address
                                  : scene.draw_args[run].address + u64{count_index} * sizeof(u32);
      a.slots = scene.deform_slots.address;
      a.alloc = scene.deform_alloc.address;
      a.pool_vertices = scene.deform_pool_vertices();
      a.pair_count = pair_stride;
      a.views = views;
      a.first_entry = scene.visible_base(0, run);
      a.reset = run == 0 ? 1u : 0u;
    }
  }

  // The records pass turns this frame's visible list into CLAS build records and the builds
  // follow on the GPU. The ray path then traces the picture against them; a raster mode with
  // shadows on runs the same chain and the resolve traces the lights against them. With more than
  // one view the geometry is the **union** of the views' cuts: the visible list is run-major, so
  // every view's first run is one contiguous range at the front and one dispatch covers them all.
  gfx::ClusterRecordParams record_params{};
  gfx::TlasReferenceParams tlas_references{};  // the BLAS addresses into the top-level records
  if (rt_chain) {
    tlas_references.addresses = scene.blas_set.addresses.address;
    tlas_references.records = scene.rt_instances.address;
    tlas_references.count = instance_count;
    // And each record's transform, from the instance's cell and this frame's eye (ADR-0053).
    tlas_references.instances = scene.instances.address;
    tlas_references.eye = gpu_eye;
    record_params.clusters = scene.clusters.address;
    record_params.vertices = scene.vertices.address;
    record_params.indices8 = scene.indices8.address;
    record_params.instances = scene.instances.address;
    record_params.meshes = scene.meshes.address;
    record_params.visible = scene.visible.address;
    // Where the cull pass counted each view's survivors: the mesh path's group count is the first
    // word of the view's indirect block, the vertex path's instance count the second, and the
    // views' blocks are `k_draw_args_bytes` apart.
    record_params.visible_count = scene.draw_args[0].address + u64{count_index} * sizeof(u32);
    record_params.slots = scene.slots.address;
    record_params.instance_counts = scene.instance_counts.address;
    // The shadow casters' count words, in the software argument blocks the cull pass counted them
    // in; the runs themselves are `gfx::k_caster_run` of every view, where the shader looks.
    record_params.caster_count = casters ? scene.sw_args.address : 0;
    record_params.records = scene.records.address;
    record_params.record_count = scene.record_count.address;
    record_params.blas_records = scene.blas_records.address;
    record_params.clas_addresses = scene.clas_set.addresses.address;
    record_params.instance_count = instance_count;
    record_params.pair_count = pair_stride;
    record_params.mode = gfx::cluster_records_mode(views, settings.rt_templates);
    // What the frame's structures hold room for: the records pass keeps no more, dropping whole
    // instances past it (gfx::ClusterRecordParams). The top-level instance records — one per scene
    // instance, the world transform and the instance as the custom index, shared by every view —
    // were written once with the scene; the frame only copies each instance's bottom-level address
    // into them after the one build that decides it.
    record_params.capacity = scene.rt_capacity();
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
  const gfx::RgBuffer rg_pair_entries = graph.import_buffer("pair entries", scene.pair_entries);
  gfx::RgBuffer rg_vertex_headers{};
  gfx::RgBuffer rg_vertex_records{};
  gfx::RgBuffer rg_vertex_indices{};
  if (vertex_indexed) {
    rg_vertex_headers = graph.import_buffer("vertex draw headers", scene.vertex_headers);
    rg_vertex_records = graph.import_buffer("vertex draw records", scene.vertex_records);
    rg_vertex_indices = graph.import_buffer("vertex draw indices", scene.vertex_indices);
  }
  const gfx::RgBuffer rg_flags[2] = {graph.import_buffer("flags0", scene.flags[0]),
                                     graph.import_buffer("flags1", scene.flags[1])};
  const gfx::RgBuffer rg_sw_args = graph.import_buffer("sw_args", scene.sw_args);
  // The cascades' argument blocks and the depth atlas. The atlas is cleared every frame, so its
  // old contents are discarded; what is carried in is that the previous frame's resolve sampled
  // it, which the first write of this frame has to wait for.
  gfx::RgBuffer rg_shadow_args{};
  gfx::RgImage rg_atlas{};
  if (csm) {
    rg_shadow_args = graph.import_buffer("shadow args", scene.shadow_args);
    rg_atlas = graph.import_image("shadow atlas", shadow_atlas_, gfx::ImageLayout::Undefined,
                                  gfx::PipelineStage::FragmentShader,
                                  gfx::MemoryAccess::ShaderSampledRead);
  }
  const gfx::RgBuffer rg_vis = graph.import_buffer("visibility", targets.vis);
  const gfx::RgBuffer rg_hiz = graph.import_buffer("hiz", targets.hiz);
  const gfx::RgBuffer rg_stats = graph.import_buffer("stats", stat_blocks_[slot]);
  // A streamed world's table set, on the frame that brings it up to date: the changed instance
  // slots copied out of this slot's staging, then their pairs expanded from them. Every pass that
  // reads a table says so (`read_tables`), so the graph orders them after the writes; on any other
  // frame nothing writes the tables and nothing is declared.
  gfx::RgBuffer rg_instances{};
  gfx::RgBuffer rg_pair_table{};
  gfx::RgBuffer rg_table_staging{};
  PairExpandParams pair_expand{};
  if (write_tables) {
    rg_instances = graph.import_buffer("instances", scene.instances);
    rg_pair_table = graph.import_buffer("pair table", scene.pair_table);
    rg_table_staging = graph.import_buffer("table staging", tables_.staging);
    pair_expand.instances = scene.instances.address;
    pair_expand.meshes = scene.meshes.address;
    pair_expand.list = tables_.list_address;
    pair_expand.pairs = scene.pair_table.address;
    pair_expand.count = tables_.expand;
  }
  auto read_tables = [&, write_tables](gfx::PassBuilder& b, gfx::Access access, bool pairs) {
    if (!write_tables) return;
    b.read(rg_instances, access);
    if (pairs) b.read(rg_pair_table, access);
  };
  gfx::RgBuffer rg_pool{};
  gfx::RgBuffer rg_slots{};
  gfx::RgBuffer rg_alloc{};
  gfx::RgBuffer rg_deform_args{};
  if (resolved_.deform_pass) {
    rg_pool = graph.import_buffer("deform pool", scene.deform_pool);
    rg_slots = graph.import_buffer("deform slots", scene.deform_slots);
    rg_alloc = graph.import_buffer("deform alloc", scene.deform_alloc);
    rg_deform_args = graph.import_buffer("deform args", scene.deform_args);
  }
  // The terrain levels' fields: every field buffer this frame's table names, which the pool pass
  // reads, and the ones the host handed over, which this frame copies in from their staging. A
  // field slot is written only once no table still to be recorded names it, but frames already
  // submitted may still read it, so the import says it was last read everywhere: the copy's barrier
  // then waits for them (docs/subsystems/renderer.md, "The dunes in time-lapse").
  gfx::RgBuffer rg_terrain_fields[2 * GpuScene::k_terrain_field_slots * 4]{};
  u32 terrain_field_count = 0;
  struct TerrainCopy {
    gfx::RgBuffer staging, field;
  };
  // Every copy handed over since the last frame, one a slot (GpuScene::terrain_prepare): a frame
  // that shows a slot copies it in first.
  const u32 terrain_copies = terrain_frame_.copies.size();
  SmallVector<TerrainCopy, 16> rg_terrain_copies(terrain_copies);
  for (u32 c = 0; c < terrain_copies; ++c) {
    const GpuScene::TerrainUpdate::Copy& copy = terrain_frame_.copies[c];
    rg_terrain_copies[c].staging = graph.import_buffer(
        "terrain staging", copy.staging, gfx::PipelineStage::Host, gfx::MemoryAccess::HostWrite);
    rg_terrain_copies[c].field =
        graph.import_buffer("terrain field", copy.field, gfx::PipelineStage::AllCommands,
                            gfx::MemoryAccess::MemoryRead);
  }
  for (const gfx::BufferResource& field : terrain_frame_.fields) {
    if (terrain_field_count >= std::size(rg_terrain_fields)) break;
    gfx::RgBuffer handle{};
    for (u32 c = 0; c < terrain_copies; ++c) {
      if (terrain_frame_.copies[c].field.buffer == field.buffer)
        handle = rg_terrain_copies[c].field;
    }
    if (!handle.valid()) {
      handle = graph.import_buffer("terrain field", field, gfx::PipelineStage::AllCommands,
                                   gfx::MemoryAccess::MemoryRead);
    }
    rg_terrain_fields[terrain_field_count++] = handle;
  }
  auto read_terrain = [&, terrain_field_count](gfx::PassBuilder& b) {
    for (u32 f = 0; f < terrain_field_count; ++f)
      b.read(rg_terrain_fields[f], gfx::Access::ComputeRead);
  };
  // The rings' chunk uploads and slot records (GpuScene::terrain_chunk_upload/show): copies into
  // the scene's geometry buffers, which every pass reads. On such a frame the buffers are imported
  // as last read everywhere — so the copies wait for the frames in flight that may still read a
  // slot being reused — and every pass that reads geometry declares it (`read_pool`), so the graph
  // orders it after the copies. Every other frame declares nothing, as a scene read whole never
  // has.
  const bool terrain_geometry = !terrain_frame_.geometry.empty();
  struct TerrainGeometry {
    gfx::RgBuffer clusters, lods, quantized, attributes, triangles, meshes, vertices, indices8;
    gfx::RgBuffer instances;  // a slot turned on stands at its chunk's corner
  } tg{};
  if (terrain_geometry) {
    const auto import = [&](const char* name, const gfx::BufferResource& buffer) {
      return buffer.buffer.valid()
                 ? graph.import_buffer(name, buffer, gfx::PipelineStage::AllCommands,
                                       gfx::MemoryAccess::MemoryRead)
                 : gfx::RgBuffer{};
    };
    tg.clusters = import("ring clusters", scene.clusters);
    tg.lods = import("ring lods", scene.lods);
    tg.quantized = import("ring quantized", scene.quantized);
    tg.attributes = import("ring attributes", scene.attributes);
    tg.triangles = import("ring triangles", scene.triangles);
    tg.meshes = import("ring meshes", scene.meshes);
    tg.instances = import("ring instances", scene.instances);
    if (rt_chain) {
      tg.vertices = import("ring vertices", scene.vertices);
      tg.indices8 = import("ring indices8", scene.indices8);
    }
  }
  auto read_ring_geometry = [&, terrain_geometry](gfx::PassBuilder& b, gfx::Access access) {
    if (!terrain_geometry) return;
    for (const gfx::RgBuffer handle :
         {tg.clusters, tg.lods, tg.quantized, tg.attributes, tg.triangles, tg.meshes, tg.vertices,
          tg.indices8, tg.instances}) {
      if (handle.valid()) b.read(handle, access);
    }
  };
  // The streamed scene's page pool and its feedback. The pool buffers have to be *declared*, not
  // only written: a page upload is a transfer into the same `clusters`, `quantized`, `attributes`
  // and `triangles` the cull pass, the rasterizers, the resolve and the CLAS builds read, and the
  // render graph is where a hazard between two passes is turned into a barrier (AGENTS.md).
  struct StreamBuffers {
    gfx::RgBuffer pool_clusters, pool_quantized, pool_attributes, pool_triangles;
    gfx::RgBuffer pool_vertices, pool_indices8, stage;
    gfx::RgBuffer used, requests, request_count, request_mask, feedback;
  } sb{};
  if (streaming) {
    sb.pool_clusters = graph.import_buffer("page clusters", scene.clusters);
    sb.pool_quantized = graph.import_buffer("page quantized", scene.quantized);
    sb.pool_attributes = graph.import_buffer("page attributes", scene.attributes);
    sb.pool_triangles = graph.import_buffer("page triangles", scene.triangles);
    sb.stage = graph.import_buffer("page stage", scene.page_stage);
    sb.used = graph.import_buffer("page used", scene.page_used);
    sb.requests = graph.import_buffer("page requests", scene.page_requests);
    sb.request_count = graph.import_buffer("page request count", scene.request_count);
    sb.request_mask = graph.import_buffer("page request mask", scene.request_mask);
    sb.feedback = graph.import_buffer("page feedback", streamer_.feedback(slot));
    if (rt_chain) {
      sb.pool_vertices = graph.import_buffer("page vertices", scene.vertices);
      sb.pool_indices8 = graph.import_buffer("page indices8", scene.indices8);
    }
  }
  // Every pass that reads geometry out of the pool says so with the access its own stage uses, so
  // the one transfer that filled it this frame is made visible to each of them.
  auto read_pool = [&, streaming, rt_chain](gfx::PassBuilder& b, gfx::Access access) {
    read_ring_geometry(b, access);
    if (!streaming) return;
    b.read(sb.pool_clusters, access);
    b.read(sb.pool_quantized, access);
    b.read(sb.pool_attributes, access);
    b.read(sb.pool_triangles, access);
    if (rt_chain && access == gfx::Access::AccelerationBuildRead) {
      b.read(sb.pool_vertices, access);
      b.read(sb.pool_indices8, access);
    }
  };
  struct RtBuffers {
    gfx::RgBuffer records, record_count, slots, instance_counts, blas_records;
    gfx::RgBuffer clas_data, clas_addresses, clas_sizes, blas_data, blas_addresses, tlas, instances;
  } rt{};
  if (rt_chain) {
    rt.records = graph.import_buffer("clas records", scene.records);
    rt.record_count = graph.import_buffer("clas record count", scene.record_count);
    rt.slots = graph.import_buffer("clas slots", scene.slots);
    rt.instance_counts = graph.import_buffer("clas instance counts", scene.instance_counts);
    rt.blas_records = graph.import_buffer("cluster blas records", scene.blas_records);
    rt.clas_data = graph.import_buffer("clas", scene.clas_set.data);
    rt.clas_addresses = graph.import_buffer("clas addresses", scene.clas_set.addresses);
    rt.clas_sizes = graph.import_buffer("clas sizes", scene.clas_set.sizes);
    rt.blas_data = graph.import_buffer("cluster blas", scene.blas_set.data);
    rt.blas_addresses = graph.import_buffer("cluster blas addresses", scene.blas_set.addresses);
    rt.tlas = graph.import_buffer("tlas", scene.tlas.buffer);
    rt.instances = graph.import_buffer("tlas instances", scene.rt_instances);
  }
  gfx::ClearColor sky{};
  const Vec4 clear_sky = stand_in_sky(k_sky, desc_.display, resolved_.settings);
  sky.float32[0] = clear_sky.x;
  sky.float32[1] = clear_sky.y;
  sky.float32[2] = clear_sky.z;
  sky.float32[3] = clear_sky.w;
  // The sky's tables and sums (sky.h). The star table is written once at create and read here only.
  struct SkyBuffers {
    gfx::RgBuffer transmittance, multiscatter, view, aerial, frame;
  } sb_sky{};
  if (sky_on) {
    sb_sky.transmittance = graph.import_buffer("sky transmittance", sky_.transmittance);
    sb_sky.multiscatter = graph.import_buffer("sky multiscatter", sky_.multiscatter);
    sb_sky.view = graph.import_buffer("sky view", sky_.sky_view);
    sb_sky.aerial = graph.import_buffer("sky aerial", sky_.aerial);
    sb_sky.frame = graph.import_buffer("sky frame", sky_.frame);
  }
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
        if (csm) b.write(rg_shadow_args, gfx::Access::TransferWrite);
        if (!direct) b.write(rg_vis, gfx::Access::TransferWrite);
        if (occlusion) b.write(rg_flags[cur_flags], gfx::Access::TransferWrite);
        if (fill_flags) b.write(rg_flags[prev_flags], gfx::Access::TransferWrite);
        if (fill_hiz) b.write(rg_hiz, gfx::Access::TransferWrite);
        if (rt_chain) b.write(rt.instance_counts, gfx::Access::TransferWrite);
        if (deform_on) b.write(rg_deform_args, gfx::Access::TransferWrite);
        if (vertex_indexed) b.write(rg_vertex_headers, gfx::Access::TransferWrite);
        if (streaming) {  // the page feedback is per frame, so it starts every frame empty
          b.write(sb.used, gfx::Access::TransferWrite);
          b.write(sb.request_mask, gfx::Access::TransferWrite);
          b.write(sb.request_count, gfx::Access::TransferWrite);
        }
      },
      [&](gfx::CommandList cb, gfx::RenderGraph&) {
        if (cull_on) {
          for (u32 v = 0; v < views; ++v) {
            const u64 at = view_frames[v].args_offset;
            for (u32 i = 0; i < 2; ++i) {
              if (vertex_path) {  // {vertexCount, instanceCount = 0, firstVertex, firstInstance}
                cb.fill_buffer(scene.draw_args[i].buffer, at, sizeof(u32),
                               triangles_per_cluster * 3);
                cb.fill_buffer(scene.draw_args[i].buffer, at + sizeof(u32), sizeof(u32) * 3, 0);
              } else {  // {groups = 0, 1, 1}
                cb.fill_buffer(scene.draw_args[i].buffer, at, sizeof(u32), 0);
                cb.fill_buffer(scene.draw_args[i].buffer, at + sizeof(u32), sizeof(u32) * 2, 1);
              }
            }
            cb.fill_buffer(scene.sw_args.buffer, at, sizeof(u32), 0);
            cb.fill_buffer(scene.sw_args.buffer, at + sizeof(u32), sizeof(u32) * 2, 1);
          }
        }
        // Each cascade's block, in the shape its draw reads: the vertex path's
        // {vertexCount, instanceCount = 0, ...} or the mesh path's {groups = 0, 1, 1}.
        for (u32 c = 0; c < cascade_runs; ++c) {
          const u64 at = scene.shadow_args_offset(c);
          if (vertex_path) {
            cb.fill_buffer(scene.shadow_args.buffer, at, sizeof(u32), triangles_per_cluster * 3);
            cb.fill_buffer(scene.shadow_args.buffer, at + sizeof(u32), sizeof(u32) * 3, 0);
          } else {
            cb.fill_buffer(scene.shadow_args.buffer, at, sizeof(u32), 0);
            cb.fill_buffer(scene.shadow_args.buffer, at + sizeof(u32), sizeof(u32) * 2, 1);
          }
        }
        if (!direct) cb.fill_buffer(targets.vis.buffer, 0, gfx::k_whole_size, 0);
        if (vertex_indexed) {
          for (u32 v = 0; v < views; ++v) {
            for (u32 run = 0; run < 2; ++run) {
              cb.update_buffer(scene.vertex_headers.buffer, scene.vertex_header_offset(v, run),
                               sizeof(vertex_reset), &vertex_reset);
            }
          }
          for (u32 c = 0; c < cascade_runs; ++c) {
            cb.update_buffer(scene.vertex_headers.buffer, scene.shadow_vertex_header_offset(c),
                             sizeof(vertex_reset), &vertex_reset);
          }
        }
        if (occlusion) cb.fill_buffer(scene.flags[cur_flags].buffer, 0, gfx::k_whole_size, 0);
        if (fill_flags) cb.fill_buffer(scene.flags[prev_flags].buffer, 0, gfx::k_whole_size, 0);
        if (fill_hiz) cb.fill_buffer(targets.hiz.buffer, 0, gfx::k_whole_size, 0);
        if (rt_chain) cb.fill_buffer(scene.instance_counts.buffer, 0, gfx::k_whole_size, 0);
        if (deform_on) {  // {groups = 0, 1, 1}; the copy below fills in the count
          for (u32 i = 0; i < views * k_visible_runs + cascade_runs; ++i) {
            const u64 at = u64{i} * gfx::k_draw_args_bytes;
            cb.fill_buffer(scene.deform_args.buffer, at, sizeof(u32), 0);
            cb.fill_buffer(scene.deform_args.buffer, at + sizeof(u32), sizeof(u32) * 2, 1);
          }
        }
        if (streaming) {
          cb.fill_buffer(scene.page_used.buffer, 0, gfx::k_whole_size, 0);
          cb.fill_buffer(scene.request_mask.buffer, 0, gfx::k_whole_size, 0);
          cb.fill_buffer(scene.request_count.buffer, 0, gfx::k_whole_size, 0);
        }
      });

  // The pages this frame admitted, copied out of the staging ring into their pool slots. It is the
  // first thing the frame does after the reset, so everything below reads a pool that already has
  // them — which is what makes a page arriving and a page being drawn the same frame rather than
  // the next one.
  if (streaming && streamer_.has_uploads()) {
    graph.add_pass(
        "page upload", gfx::PassKind::Transfer,
        [&](gfx::PassBuilder& b) {
          b.read(sb.stage, gfx::Access::TransferRead);
          b.write(sb.pool_clusters, gfx::Access::TransferWrite);
          b.write(sb.pool_quantized, gfx::Access::TransferWrite);
          b.write(sb.pool_attributes, gfx::Access::TransferWrite);
          b.write(sb.pool_triangles, gfx::Access::TransferWrite);
          if (rt_chain) {
            b.write(sb.pool_vertices, gfx::Access::TransferWrite);
            b.write(sb.pool_indices8, gfx::Access::TransferWrite);
          }
        },
        [&](gfx::CommandList cb, gfx::RenderGraph&) { streamer_.record_uploads(cb); });
  }

  // A moving terrain's fields the host evaluated since the last frame, copied out of their staging
  // into their field slots before the cull pass and the pool pass read them. The staging buffers go
  // to the frame context once this frame is recorded, and are freed when it is done.
  if (terrain_copies > 0 || terrain_geometry) {
    graph.add_pass(
        "terrain upload", gfx::PassKind::Transfer,
        [&, terrain_copies, terrain_geometry](gfx::PassBuilder& b) {
          for (u32 c = 0; c < terrain_copies; ++c) {
            b.read(rg_terrain_copies[c].staging, gfx::Access::TransferRead);
            b.write(rg_terrain_copies[c].field, gfx::Access::TransferWrite);
          }
          if (terrain_geometry) {
            for (const gfx::RgBuffer handle :
                 {tg.clusters, tg.lods, tg.quantized, tg.attributes, tg.triangles, tg.meshes,
                  tg.vertices, tg.indices8, tg.instances}) {
              if (handle.valid()) b.write(handle, gfx::Access::TransferWrite);
            }
          }
        },
        [&, terrain_copies](gfx::CommandList cb, gfx::RenderGraph&) {
          timer.begin(cb, "terrain upload");
          for (u32 c = 0; c < terrain_copies; ++c) {
            const GpuScene::TerrainUpdate::Copy& copy = terrain_frame_.copies[c];
            cb.copy_buffer(copy.staging.buffer, copy.field.buffer,
                           gfx::BufferCopy{copy.offset, copy.offset, copy.bytes});
          }
          for (const GpuScene::TerrainUpdate::GeometryCopy& copy : terrain_frame_.geometry)
            cb.copy_buffer(copy.src, copy.dst, copy.region);
          timer.end(cb);
        });
  }

  // A streamed world's change, into the table set this frame flipped to and nothing else: the
  // slots the change wrote (every change since this set was last current), then one workgroup per
  // live instance among them writing its pairs (pair_expand.slang). The frames in flight read the
  // other sets, so nothing here waits for them.
  if (write_tables) {
    graph.add_pass(
        "table copy", gfx::PassKind::Transfer,
        [&](gfx::PassBuilder& b) {
          b.read(rg_table_staging, gfx::Access::TransferRead);
          b.write(rg_instances, gfx::Access::TransferWrite);
        },
        [&](gfx::CommandList cb, gfx::RenderGraph&) {
          timer.begin(cb, "table copy");
          cb.copy_buffer(
              tables_.staging.buffer, scene.instances.buffer,
              std::span<const gfx::BufferCopy>(tables_.copies.data(), tables_.copies.size()));
          timer.end(cb);
        });
    if (tables_.expand > 0) {
      graph.add_pass(
          "pair expand", gfx::PassKind::Compute,
          [&](gfx::PassBuilder& b) {
            b.read(rg_table_staging, gfx::Access::ComputeRead);  // the list of instances
            b.read(rg_instances, gfx::Access::ComputeRead);
            b.write(rg_pair_table, gfx::Access::ComputeWrite);
          },
          [&](gfx::CommandList cb, gfx::RenderGraph&) {
            timer.begin(cb, "pair expand");
            cb.bind_pipeline(gfx::BindPoint::Compute, pipelines.pair_expand.pipeline);
            cb.push_constants(pipelines.pair_expand.layout, gfx::ShaderStage::Compute, 0,
                              sizeof(PairExpandParams), &pair_expand);
            cb.dispatch(tables_.expand, 1, 1);
            timer.end(cb);
          });
    }
  }

  auto add_cull = [&](u32 block, u32 list) {
    graph.add_pass(
        "cull", gfx::PassKind::Compute,
        [&, list](gfx::PassBuilder& b) {
          b.write(rg_args[list], gfx::Access::ComputeReadWrite);
          b.write(rg_visible, gfx::Access::ComputeWrite);
          b.write(rg_pair_entries, gfx::Access::ComputeWrite);
          if (vertex_indexed) {
            b.write(rg_vertex_headers, gfx::Access::ComputeReadWrite);
            b.write(rg_vertex_records, gfx::Access::ComputeWrite);
          }
          if (use_sw || casters) {  // the software run's counter, or the casters'
            b.write(rg_sw_args, gfx::Access::ComputeReadWrite);
          }
          if (occlusion) {
            b.read(rg_hiz, gfx::Access::ComputeRead);
            b.read(rg_flags[prev_flags], gfx::Access::ComputeRead);
            b.write(rg_flags[cur_flags], gfx::Access::ComputeReadWrite);
          }
          read_pool(b, gfx::Access::ComputeRead);
          read_tables(b, gfx::Access::ComputeRead, false);
          if (streaming) {  // the feedback: what the cut used, and what it could not refine into
            b.write(sb.used, gfx::Access::ComputeWrite);
            b.write(sb.requests, gfx::Access::ComputeWrite);
            b.write(sb.request_count, gfx::Access::ComputeReadWrite);
            b.write(sb.request_mask, gfx::Access::ComputeReadWrite);
          }
        },
        [&, block](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Compute, pipelines.cull.pipeline);
          for (u32 v = 0; v < views; ++v) {
            timer.begin(cb, k_zone_names[k_zone_cull][v]);
            cb.push_constants(pipelines.cull.layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                              &view_frames[v].block_address[block]);
            cb.dispatch(gfx::cull_group_count(pair_count), 1, 1);
            timer.end(cb);
          }
        });
  };
  // The deformed-vertex pool for one run of every view's visible list: copy each run's survivor
  // count into an indirect dispatch block, then one workgroup per surviving cluster. Two runs when
  // occlusion culling splits the cut, because pass 2's entries are not known until its cull has
  // run and the pool has to hold pass 1's positions before pass 1 draws.
  // The static shape stage, on the frames where it has anything to do. It runs over each cached
  // instance's **whole mesh** rather than over the cut, because a cache the next frame's cut can
  // start from has to cover every cluster the cut might name — which is also why it is a cost
  // measured in "a weights change" and not in "a frame".
  auto add_static_cache = [&]() {
    if (cache_rebuilds == 0) return;
    graph.add_pass(
        "deform cache", gfx::PassKind::Compute,
        [&](gfx::PassBuilder& b) { b.write(rg_pool, gfx::Access::ComputeWrite); },
        [&](gfx::CommandList cb, gfx::RenderGraph&) {
          timer.begin(cb, "deform cache");
          cb.bind_pipeline(gfx::BindPoint::Compute, pipelines.deform_cache.pipeline);
          const std::span<const gfx::DeformDesc> statics = scene.deform_descs();
          const std::span<const u32> owners = scene.deform_instances();
          for (u32 d = 0; d < statics.size(); ++d) {
            if (statics[d].cache == 0) continue;
            gfx::DeformParams p = deform_params[0][0];
            // `deform_cache_main` reads these two with a different meaning, and says so where it
            // uses them: the instance to rebuild, and how many clusters its mesh has.
            p.visible_offset = owners[d];
            p.max_entries = scene.deform_mesh_clusters(d);
            if (p.max_entries == 0) continue;
            cb.push_constants(pipelines.deform_cache.layout, gfx::ShaderStage::Compute, 0,
                              sizeof(gfx::DeformParams), &p);
            cb.dispatch(p.max_entries, 1, 1);
          }
          timer.end(cb);
        });
  };
  auto add_deform = [&](u32 run) {
    const u64 source_offset = run == 2 ? 0 : u64{count_index} * sizeof(u32);
    const gfx::RgBuffer rg_source = run == 2 ? rg_sw_args : rg_args[run];
    // Suballocate the pool for this run of every view before anything writes or reads it: one
    // workgroup, a prefix sum over the entries' vertex counts, and one `deform_slots` word per
    // entry saying where its block is (or that it did not fit).
    graph.add_pass(
        "deform alloc", gfx::PassKind::Compute,
        [&, rg_source](gfx::PassBuilder& b) {
          b.read(rg_source, gfx::Access::ComputeRead);
          b.read(rg_visible, gfx::Access::ComputeRead);
          b.write(rg_slots, gfx::Access::ComputeWrite);
          b.write(rg_alloc, gfx::Access::ComputeReadWrite);
          read_pool(b, gfx::Access::ComputeRead);
        },
        [&, run](gfx::CommandList cb, gfx::RenderGraph&) {
          // Its own zone rather than a view's: one dispatch covers every view of the run, so
          // charging it to view 0 would put the whole set's allocation on one monitor's row.
          timer.begin(cb, "deform alloc");
          cb.bind_pipeline(gfx::BindPoint::Compute, pipelines.deform_alloc.pipeline);
          cb.push_constants(pipelines.deform_alloc.layout, gfx::ShaderStage::Compute, 0,
                            sizeof(gfx::DeformAllocParams), &alloc_params[run]);
          cb.dispatch(1, 1, 1);
          timer.end(cb);
        });
    graph.add_pass(
        "deform args", gfx::PassKind::Transfer,
        [&, rg_source](gfx::PassBuilder& b) {
          b.read(rg_source, gfx::Access::TransferRead);
          b.write(rg_deform_args, gfx::Access::TransferWrite);
        },
        [&, run, source_offset](gfx::CommandList cb, gfx::RenderGraph&) {
          const gfx::BufferHandle source =
              run == 2 ? scene.sw_args.buffer : scene.draw_args[run].buffer;
          for (u32 v = 0; v < views; ++v) {
            const gfx::BufferCopy copy{view_frames[v].args_offset + source_offset,
                                       scene.deform_args_offset(v, run), sizeof(u32)};
            cb.copy_buffer(source, scene.deform_args.buffer, copy);
          }
        });
    graph.add_pass(
        "deform", gfx::PassKind::Compute,
        [&, rg_source](gfx::PassBuilder& b) {
          b.read(rg_source, gfx::Access::ComputeRead);  // the run's count word
          b.read(rg_deform_args, gfx::Access::IndirectRead);
          b.read(rg_visible, gfx::Access::ComputeRead);
          b.read(rg_slots, gfx::Access::ComputeRead);
          b.write(rg_pool, gfx::Access::ComputeWrite);
          read_terrain(b);
          read_pool(b, gfx::Access::ComputeRead);
        },
        [&, run](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Compute, pipelines.deform.pipeline);
          for (u32 v = 0; v < views; ++v) {
            timer.begin(cb, k_zone_names[k_zone_deform][v]);
            cb.push_constants(pipelines.deform.layout, gfx::ShaderStage::Compute, 0,
                              sizeof(gfx::DeformParams), &deform_params[v][run]);
            cb.dispatch_indirect(scene.deform_args.buffer, scene.deform_args_offset(v, run));
            timer.end(cb);
          }
        });
  };
  // The indexed draw's indices, between the cull pass that allocated them and the draw that reads
  // them: one workgroup per survivor, dispatched from the count the cull kept in the header
  // (vertex_expand.slang). Timed as the raster pass's, because it is what drawing indexed costs.
  auto add_expand = [&](u32 run) {
    graph.add_pass(
        "expand", gfx::PassKind::Compute,
        [&](gfx::PassBuilder& b) {
          // Its own dispatch, then the index count it finishes. The order is load-bearing: the
          // render graph leaves a buffer in the state of the pass's *last* use of it, and it has to
          // be the write, or the draw's read of the count gets no barrier and sees the reset's
          // zero.
          b.read(rg_vertex_headers, gfx::Access::IndirectRead);
          b.write(rg_vertex_headers, gfx::Access::ComputeReadWrite);
          b.read(rg_vertex_records, gfx::Access::ComputeRead);
          b.write(rg_vertex_indices, gfx::Access::ComputeWrite);
          read_pool(b, gfx::Access::ComputeRead);
        },
        [&, run](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Compute, pipelines.expand.pipeline);
          for (u32 v = 0; v < views; ++v) {
            timer.begin(cb, k_zone_names[k_zone_hw][v]);
            cb.push_constants(pipelines.expand.layout, gfx::ShaderStage::Compute, 0,
                              sizeof(gfx::VertexExpandParams), &expand_params[v][run]);
            cb.dispatch_indirect(scene.vertex_headers.buffer, scene.vertex_header_offset(v, run) +
                                                                  gfx::k_vertex_draw_expand_offset);
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
          if (vertex_indexed) {  // arguments, records and indices, each one way
            b.read(rg_vertex_headers, gfx::Access::IndirectRead);
            b.read(rg_vertex_records, gfx::Access::VertexRead);
            b.read(rg_vertex_indices, gfx::Access::IndexRead);
          } else if (cull_on) {
            b.read(rg_args[list], gfx::Access::IndirectRead);
            b.read(rg_visible, vertex_path ? gfx::Access::VertexRead : gfx::Access::MeshRead);
          }
          {
            const gfx::Access stage = vertex_path ? gfx::Access::VertexRead : gfx::Access::MeshRead;
            if (deform_on) {
              b.read(rg_pool, stage);
              b.read(rg_slots, stage);
            }
            read_pool(b, stage);
            read_tables(b, stage, false);
          }
        },
        [&, list, run](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Graphics,
                           vertex_path ? pipelines.vertex : pipelines.hardware);
          bindless.bind(cb, gfx::BindPoint::Graphics);
          for (u32 v = 0; v < views; ++v) {
            timer.begin(cb, k_zone_names[k_zone_hw][v]);
            set_view_viewport(cb, 0, 0, view_frames[v].width, view_frames[v].height);
            cb.push_constants(bindless.pipeline_layout(), gfx::ShaderStage::All, 0,
                              sizeof(gfx::ClusterDrawParams), &draws[v][run]);
            if (vertex_indexed) {
              // The run's triangles, indexed, then whatever its index budget had no room for
              // through the capacity draw — an empty draw unless something overflowed.
              const u64 header = scene.vertex_header_offset(v, run);
              cb.bind_index_buffer(scene.vertex_indices.buffer, scene.vertex_indices_offset(v, run),
                                   gfx::IndexType::Uint32);
              cb.draw_indexed_indirect(scene.vertex_headers.buffer, header, 1,
                                       sizeof(gfx::DrawIndexedIndirectArgs));
              cb.bind_pipeline(gfx::BindPoint::Graphics, pipelines.vertex_fallback);
              cb.draw_indirect(scene.vertex_headers.buffer,
                               header + gfx::k_vertex_draw_fallback_offset, 1,
                               sizeof(gfx::DrawIndirectArgs));
              if (v + 1 < views) {
                cb.bind_pipeline(gfx::BindPoint::Graphics, pipelines.vertex);
              }
            } else if (vertex_path) {
              if (cull_on) {
                cb.draw_indirect(scene.draw_args[list].buffer, view_frames[v].args_offset, 1,
                                 sizeof(u32) * 4);
              } else {
                cb.draw(triangles_per_cluster * 3, leaf_count, 0, 0);
              }
            } else if (cull_on) {
              cb.draw_mesh_tasks_indirect(scene.draw_args[list].buffer, view_frames[v].args_offset,
                                          1, sizeof(u32) * 3);
            } else {
              cb.draw_mesh_tasks(leaf_count, 1, 1);
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
            [&, level, first, last, v, src_w, src_h](gfx::CommandList cb, gfx::RenderGraph&) {
              if (first) timer.begin(cb, k_zone_names[k_zone_hiz][v]);
              cb.bind_pipeline(gfx::BindPoint::Compute, pipelines.hiz.pipeline);
              cb.push_constants(pipelines.hiz.layout, gfx::ShaderStage::Compute, 0, sizeof(*level),
                                level);
              cb.dispatch(gfx::hiz_group_count(src_w), gfx::hiz_group_count(src_h), 1);
              if (last) timer.end(cb);
            });
      }
    }
  };

  // **The sun's cascaded shadow maps** (docs/subsystems/renderer.md, "Shadows"): each cascade's
  // light-view cull into its own run, the pool and the index expansion for those runs where the
  // path has them, and one depth-only raster pass that draws every cascade into its tile of the
  // atlas through the picture's own rasterizer. They run after the picture's passes and before the
  // resolve, which reads the atlas. Each stage is one graph pass with a loop over the cascades, as
  // the views' stages are, because the cascades write disjoint slices of the same buffers.
  auto add_shadow_maps = [&]() {
    const u32 count = cascades_drawn;
    if (count == 0) return;
    graph.add_pass(
        "shadow cull", gfx::PassKind::Compute,
        [&](gfx::PassBuilder& b) {
          b.write(rg_shadow_args, gfx::Access::ComputeReadWrite);
          b.write(rg_visible, gfx::Access::ComputeWrite);
          read_tables(b, gfx::Access::ComputeRead, false);
          if (vertex_indexed) {
            b.write(rg_vertex_headers, gfx::Access::ComputeReadWrite);
            b.write(rg_vertex_records, gfx::Access::ComputeWrite);
          }
          read_pool(b, gfx::Access::ComputeRead);
          if (streaming) {  // a caster the camera cannot see still asks for its pages
            b.write(sb.used, gfx::Access::ComputeWrite);
            b.write(sb.requests, gfx::Access::ComputeWrite);
            b.write(sb.request_count, gfx::Access::ComputeReadWrite);
            b.write(sb.request_mask, gfx::Access::ComputeReadWrite);
          }
        },
        [&, count](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Compute, pipelines.cull.pipeline);
          timer.begin(cb, "shadow cull");
          for (u32 c = 0; c < count; ++c) {
            const u64 block = params_[slot].address + sizeof(gfx::CullParams) * (views * 2 + c);
            cb.push_constants(pipelines.cull.layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                              &block);
            cb.dispatch(gfx::cull_group_count(pair_count), 1, 1);
          }
          timer.end(cb);
        });
    if (deform_on) {
      // The cascades' entries of a deformed instance get pool blocks of their own, allocated after
      // the camera's and deformed by the same pass, so a moving character casts its pose.
      graph.add_pass(
          "shadow deform alloc", gfx::PassKind::Compute,
          [&](gfx::PassBuilder& b) {
            b.read(rg_shadow_args, gfx::Access::ComputeRead);
            b.read(rg_visible, gfx::Access::ComputeRead);
            b.write(rg_slots, gfx::Access::ComputeWrite);
            b.write(rg_alloc, gfx::Access::ComputeReadWrite);
            read_pool(b, gfx::Access::ComputeRead);
          },
          [&](gfx::CommandList cb, gfx::RenderGraph&) {
            timer.begin(cb, "shadow");
            cb.bind_pipeline(gfx::BindPoint::Compute, pipelines.deform_alloc.pipeline);
            cb.push_constants(pipelines.deform_alloc.layout, gfx::ShaderStage::Compute, 0,
                              sizeof(gfx::DeformAllocParams), &shadow_alloc);
            cb.dispatch(1, 1, 1);
            timer.end(cb);
          });
      graph.add_pass(
          "shadow deform args", gfx::PassKind::Transfer,
          [&](gfx::PassBuilder& b) {
            b.read(rg_shadow_args, gfx::Access::TransferRead);
            b.write(rg_deform_args, gfx::Access::TransferWrite);
          },
          [&, count](gfx::CommandList cb, gfx::RenderGraph&) {
            for (u32 c = 0; c < count; ++c) {
              const gfx::BufferCopy copy{
                  scene.shadow_args_offset(c) + u64{count_index} * sizeof(u32),
                  scene.shadow_deform_args_offset(c), sizeof(u32)};
              cb.copy_buffer(scene.shadow_args.buffer, scene.deform_args.buffer, copy);
            }
          });
      graph.add_pass(
          "shadow deform", gfx::PassKind::Compute,
          [&](gfx::PassBuilder& b) {
            b.read(rg_shadow_args, gfx::Access::ComputeRead);
            b.read(rg_deform_args, gfx::Access::IndirectRead);
            b.read(rg_visible, gfx::Access::ComputeRead);
            b.read(rg_slots, gfx::Access::ComputeRead);
            b.write(rg_pool, gfx::Access::ComputeWrite);
            read_terrain(b);
            read_pool(b, gfx::Access::ComputeRead);
          },
          [&, count](gfx::CommandList cb, gfx::RenderGraph&) {
            cb.bind_pipeline(gfx::BindPoint::Compute, pipelines.deform.pipeline);
            timer.begin(cb, "shadow");
            for (u32 c = 0; c < count; ++c) {
              cb.push_constants(pipelines.deform.layout, gfx::ShaderStage::Compute, 0,
                                sizeof(gfx::DeformParams), &shadow_deform[c]);
              cb.dispatch_indirect(scene.deform_args.buffer, scene.shadow_deform_args_offset(c));
            }
            timer.end(cb);
          });
    }
    if (vertex_indexed) {
      graph.add_pass(
          "shadow expand", gfx::PassKind::Compute,
          [&](gfx::PassBuilder& b) {
            // The dispatch's arguments, then the count it finishes: the write is declared last so
            // the draw's read of the count gets its barrier (see add_expand).
            b.read(rg_vertex_headers, gfx::Access::IndirectRead);
            b.write(rg_vertex_headers, gfx::Access::ComputeReadWrite);
            b.read(rg_vertex_records, gfx::Access::ComputeRead);
            b.write(rg_vertex_indices, gfx::Access::ComputeWrite);
            read_pool(b, gfx::Access::ComputeRead);
          },
          [&, count](gfx::CommandList cb, gfx::RenderGraph&) {
            cb.bind_pipeline(gfx::BindPoint::Compute, pipelines.expand.pipeline);
            timer.begin(cb, "shadow");
            for (u32 c = 0; c < count; ++c) {
              cb.push_constants(pipelines.expand.layout, gfx::ShaderStage::Compute, 0,
                                sizeof(gfx::VertexExpandParams), &shadow_expand[c]);
              cb.dispatch_indirect(
                  scene.vertex_headers.buffer,
                  scene.shadow_vertex_header_offset(c) + gfx::k_vertex_draw_expand_offset);
            }
            timer.end(cb);
          });
    }
    graph.add_pass(
        "shadow raster", gfx::PassKind::Raster,
        [&](gfx::PassBuilder& b) {
          // Reversed depth: the clear is the far end, 0, and the nearest caster to the light wins.
          b.depth_attachment(rg_atlas, gfx::LoadOp::Clear, 0.0f);
          if (vertex_indexed) {
            b.read(rg_vertex_headers, gfx::Access::IndirectRead);
            b.read(rg_vertex_records, gfx::Access::VertexRead);
            b.read(rg_vertex_indices, gfx::Access::IndexRead);
          } else {
            b.read(rg_shadow_args, gfx::Access::IndirectRead);
            b.read(rg_visible, vertex_path ? gfx::Access::VertexRead : gfx::Access::MeshRead);
          }
          const gfx::Access stage = vertex_path ? gfx::Access::VertexRead : gfx::Access::MeshRead;
          if (deform_on) {
            b.read(rg_pool, stage);
            b.read(rg_slots, stage);
          }
          read_pool(b, stage);
          read_tables(b, stage, false);
        },
        [&, count](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Graphics, pipelines.shadow);
          bindless.bind(cb, gfx::BindPoint::Graphics);
          timer.begin(cb, "shadow");
          for (u32 c = 0; c < count; ++c) {
            // Cascade c's tile of the atlas, flipped like every raster pass's viewport.
            set_view_viewport(cb, c * shadow_map, 0, shadow_map, shadow_map);
            cb.push_constants(bindless.pipeline_layout(), gfx::ShaderStage::All, 0,
                              sizeof(gfx::ClusterDrawParams), &shadow_draws[c]);
            if (vertex_indexed) {
              const u64 header = scene.shadow_vertex_header_offset(c);
              cb.bind_index_buffer(scene.vertex_indices.buffer,
                                   scene.shadow_vertex_indices_offset(c), gfx::IndexType::Uint32);
              cb.draw_indexed_indirect(scene.vertex_headers.buffer, header, 1,
                                       sizeof(gfx::DrawIndexedIndirectArgs));
              cb.bind_pipeline(gfx::BindPoint::Graphics, pipelines.shadow_fallback);
              cb.draw_indirect(scene.vertex_headers.buffer,
                               header + gfx::k_vertex_draw_fallback_offset, 1,
                               sizeof(gfx::DrawIndirectArgs));
              if (c + 1 < count) {
                cb.bind_pipeline(gfx::BindPoint::Graphics, pipelines.shadow);
              }
            } else if (vertex_path) {
              cb.draw_indirect(scene.shadow_args.buffer, scene.shadow_args_offset(c), 1,
                               sizeof(u32) * 4);
            } else {
              cb.draw_mesh_tasks_indirect(scene.shadow_args.buffer, scene.shadow_args_offset(c), 1,
                                          sizeof(u32) * 3);
            }
          }
          timer.end(cb);
        });
  };

  if (cull_on) add_cull(0, 0);
  if (deform_on) add_static_cache();
  if (deform_on) add_deform(0);
  if (direct) {
    // The direct path is single-view by construction: it draws mesh shaders straight to color.
    const gfx::ClusterDrawParams* params = &draws[0][0];
    graph.add_pass(
        "direct", gfx::PassKind::Raster,
        [&](gfx::PassBuilder& b) {
          b.color_attachment(color, gfx::LoadOp::Clear, sky);
          b.depth_attachment(depth_target, gfx::LoadOp::Clear,
                             0.0f);  // reversed Z: far is 0
          if (cull_on) {
            b.read(rg_args[0], gfx::Access::IndirectRead);
            b.read(rg_visible, gfx::Access::MeshRead);
          }
          if (deform_on) {
            b.read(rg_pool, gfx::Access::MeshRead);
            b.read(rg_slots, gfx::Access::MeshRead);
          }
          read_tables(b, gfx::Access::MeshRead, false);
        },
        [&, params](gfx::CommandList cb, gfx::RenderGraph&) {
          timer.begin(cb, k_zone_names[k_zone_hw][0]);
          cb.bind_pipeline(gfx::BindPoint::Graphics, pipelines.direct);
          bindless.bind(cb, gfx::BindPoint::Graphics);
          cb.push_constants(bindless.pipeline_layout(), gfx::ShaderStage::All, 0, sizeof(*params),
                            params);
          if (cull_on) {
            cb.draw_mesh_tasks_indirect(scene.draw_args[0].buffer, 0, 1, sizeof(u32) * 3);
          } else {
            cb.draw_mesh_tasks(leaf_count, 1, 1);
          }
          timer.end(cb);
        });
  } else {
    if (use_hw) {
      if (vertex_indexed) add_expand(0);
      add_hw_draw(0, 0);
    }
    if (occlusion) {
      add_hiz(0);
      add_cull(1, 1);
      if (deform_on) add_deform(1);
      if (vertex_indexed) add_expand(1);
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
            read_tables(b, gfx::Access::ComputeRead, false);
            if (deform_on) {
              b.read(rg_pool, gfx::Access::ComputeRead);
              b.read(rg_slots, gfx::Access::ComputeRead);
            }
            read_pool(b, gfx::Access::ComputeRead);
          },
          [&](gfx::CommandList cb, gfx::RenderGraph&) {
            cb.bind_pipeline(gfx::BindPoint::Compute, pipelines.software.pipeline);
            for (u32 v = 0; v < views; ++v) {
              timer.begin(cb, k_zone_names[k_zone_sw][v]);
              cb.push_constants(pipelines.software.layout, gfx::ShaderStage::Compute, 0,
                                sizeof(gfx::ClusterDrawParams), &draws[v][2]);
              cb.dispatch_indirect(scene.sw_args.buffer, view_frames[v].args_offset);
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
      // same passes in the same order for both. Its dispatches cover every view's first run, and
      // with shadow casters every view's caster run as well — the bucketing pass reads both, the
      // emit pass covers the pair slots, which the casters share with the drawn pairs.
      const gfx::ComputePipeline* record_passes[3] = {&pipelines.records, &pipelines.record_ranges,
                                                      &pipelines.record_emit};
      const char* record_names[3] = {"records", "ranges", "emit"};
      const u32 union_groups = (views * pair_stride + gfx::k_cluster_records_workgroup - 1) /
                               gfx::k_cluster_records_workgroup;
      const u32 bucket_groups =
          ((casters ? 2u : 1u) * views * pair_stride + gfx::k_cluster_records_workgroup - 1) /
          gfx::k_cluster_records_workgroup;
      const u32 record_groups[3] = {bucket_groups, 1, union_groups};
      for (u32 p = 0; p < 3; ++p) {
        graph.add_pass(
            record_names[p], gfx::PassKind::Compute,
            [&, p](gfx::PassBuilder& b) {
              b.read(rg_args[0], gfx::Access::ComputeRead);
              if (casters) b.read(rg_sw_args, gfx::Access::ComputeRead);  // the casters' counts
              b.read(rg_visible, gfx::Access::ComputeRead);
              b.write(rt.slots, gfx::Access::ComputeReadWrite);
              b.write(rt.instance_counts, gfx::Access::ComputeReadWrite);
              if (p != 0) b.write(rt.records, gfx::Access::ComputeWrite);
              if (p == 1) {
                b.write(rt.record_count, gfx::Access::ComputeWrite);
                b.write(rt.blas_records, gfx::Access::ComputeWrite);
              }
              read_pool(b, gfx::Access::ComputeRead);
            },
            [&, p, record_passes, record_names, record_groups](gfx::CommandList cb,
                                                               gfx::RenderGraph&) {
              timer.begin(cb, record_names[p]);
              cb.bind_pipeline(gfx::BindPoint::Compute, record_passes[p]->pipeline);
              cb.push_constants(record_passes[p]->layout, gfx::ShaderStage::Compute, 0,
                                sizeof(record_params), &record_params);
              cb.dispatch(record_groups[p], 1, 1);
              timer.end(cb);
            });
      }
      graph.add_pass(
          "clas", gfx::PassKind::Compute,
          [&](gfx::PassBuilder& b) {
            b.read(rt.records, gfx::Access::AccelerationBuildRead);
            b.read(rt.record_count, gfx::Access::AccelerationBuildRead);
            if (deform_on) {
              b.read(rg_pool, gfx::Access::AccelerationBuildRead);
              b.read(rg_slots, gfx::Access::AccelerationBuildRead);
            }
            b.write(rt.clas_data, gfx::Access::AccelerationBuildWrite);
            b.write(rt.clas_addresses, gfx::Access::AccelerationBuildWrite);
            b.write(rt.clas_sizes, gfx::Access::AccelerationBuildWrite);
            read_pool(b, gfx::Access::AccelerationBuildRead);
          },
          [&](gfx::CommandList cb, gfx::RenderGraph&) {
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
            b.write(rt.blas_data, gfx::Access::AccelerationBuildWrite);
            b.write(rt.blas_addresses, gfx::Access::AccelerationBuildWrite);
          },
          [&](gfx::CommandList cb, gfx::RenderGraph&) {
            // Every instance's structure in **one** build, packed into one buffer by the driver
            // (gfx::ClusterBlasSet). It used to be a build per instance, each waiting for the last
            // because they shared the scratch — 90 of them a frame on the desert overlook.
            timer.begin(cb, "blas");
            gfx::build_cluster_blas_set(cb, scene.blas_set, scene.blas_records.address, 0,
                                        scene.rt_scratch);
            timer.end(cb);
          });
      // Each instance's bottom-level address, which the build just decided, into the reference
      // field of its top-level record: one dispatch, a thread an instance (tlas_references.slang).
      // Until 2026-10-04 it was one copy of `instance_count` eight-byte regions, which the driver
      // encodes region by region on this thread — 47,849 of them on the endless desert, 3 ms of a
      // 3.5 ms frame and every spike past 8 ms
      // (docs/experiments/frame-thread-spikes-2026-10-04.md).
      graph.add_pass(
          "tlas instances", gfx::PassKind::Compute,
          [&](gfx::PassBuilder& b) {
            b.read(rt.blas_addresses, gfx::Access::ComputeRead);
            // The scene's instance records, which a frame that turns a terrain slot on writes.
            read_ring_geometry(b, gfx::Access::ComputeRead);
            b.write(rt.instances, gfx::Access::ComputeWrite);
          },
          [&](gfx::CommandList cb, gfx::RenderGraph&) {
            cb.bind_pipeline(gfx::BindPoint::Compute, pipelines.tlas_references.pipeline);
            cb.push_constants(pipelines.tlas_references.layout, gfx::ShaderStage::Compute, 0,
                              sizeof(gfx::TlasReferenceParams), &tlas_references);
            cb.dispatch((instance_count + gfx::k_tlas_references_workgroup - 1) /
                            gfx::k_tlas_references_workgroup,
                        1, 1);
          });
      graph.add_pass(
          "tlas", gfx::PassKind::Compute,
          [&](gfx::PassBuilder& b) {
            b.read(rt.blas_data, gfx::Access::AccelerationBuildRead);
            b.read(rt.instances, gfx::Access::AccelerationBuildRead);
            b.write(rt.tlas, gfx::Access::AccelerationBuildWrite);
          },
          [&](gfx::CommandList cb, gfx::RenderGraph&) {
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
            b.read(rt.blas_data, gfx::Access::RayQueryRead);
            b.read(rt.clas_data, gfx::Access::RayQueryRead);
            b.read(rg_visible, gfx::Access::ComputeRead);  // a hit's entry -> its pair, the id
            b.write(rg_vis, gfx::Access::ComputeWrite);
            read_pool(b, gfx::Access::ComputeRead);
          },
          [&](gfx::CommandList cb, gfx::RenderGraph&) {
            cb.bind_pipeline(gfx::BindPoint::Compute, pipelines.trace.pipeline);
            bindless.bind(cb, gfx::BindPoint::Compute);
            for (u32 v = 0; v < views; ++v) {
              timer.begin(cb, k_zone_names[k_zone_trace][v]);
              cb.push_constants(pipelines.trace.layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                                &view_frames[v].ray_address);
              cb.dispatch(gfx::ray_visibility_group_count(view_frames[v].width),
                          gfx::ray_visibility_group_count(view_frames[v].height), 1);
              timer.end(cb);
            }
          });
    }
    if (csm) add_shadow_maps();
    // The sky (sky.h, sky_luts.slang): the scene's two tables on its first frame, then every frame
    // the sky from the eye and the air to a surface, and the frame's sums from the first — all four
    // under one zone, "sky", which is the frame's and no view's. One small compute dispatch each.
    if (sky_on) {
      const u64* sky_push = &sky_address;
      auto add_sky = [&](const char* name, const gfx::ComputePipeline& pipeline, u32 gx, u32 gy,
                         gfx::RgBuffer write, gfx::RgBuffer read_a, gfx::RgBuffer read_b) {
        graph.add_pass(
            name, gfx::PassKind::Compute,
            [&, write, read_a, read_b](gfx::PassBuilder& b) {
              if (read_a.valid()) b.read(read_a, gfx::Access::ComputeRead);
              if (read_b.valid()) b.read(read_b, gfx::Access::ComputeRead);
              b.write(write, gfx::Access::ComputeWrite);
            },
            [&, gx, gy, sky_push, p = &pipeline](gfx::CommandList cb, gfx::RenderGraph&) {
              timer.begin(cb, "sky");
              cb.bind_pipeline(gfx::BindPoint::Compute, p->pipeline);
              cb.push_constants(p->layout, gfx::ShaderStage::Compute, 0, sizeof(u64), sky_push);
              cb.dispatch(gx, gy, 1);
              timer.end(cb);
            });
      };
      auto groups = [](u32 extent) { return (extent + 7) / 8; };
      if (!sky_.tables_built()) {
        add_sky("sky transmittance", sky_.transmittance_pipeline,
                groups(gfx::k_sky_transmittance_width), groups(gfx::k_sky_transmittance_height),
                sb_sky.transmittance, {}, {});
        add_sky("sky multiscatter", sky_.multiscatter_pipeline,
                groups(gfx::k_sky_multiscatter_size), groups(gfx::k_sky_multiscatter_size),
                sb_sky.multiscatter, sb_sky.transmittance, {});
        sky_.set_tables_built();
      }
      add_sky("sky view", sky_.view_pipeline, groups(gfx::k_sky_view_width),
              groups(gfx::k_sky_view_height), sb_sky.view, sb_sky.transmittance,
              sb_sky.multiscatter);
      add_sky("sky aerial", sky_.aerial_pipeline, groups(gfx::k_sky_aerial_width),
              groups(gfx::k_sky_aerial_height), sb_sky.aerial, sb_sky.transmittance,
              sb_sky.multiscatter);
      add_sky("sky frame", sky_.frame_pipeline, 1, 1, sb_sky.frame, sb_sky.view,
              sb_sky.transmittance);
    }
    // One resolve pass, one fullscreen draw per view through that view's rectangle of the target.
    // The clear covers the whole target once, so a pixel no view owns keeps the sky.
    graph.add_pass(
        "resolve", gfx::PassKind::Raster,
        [&](gfx::PassBuilder& b) {
          b.color_attachment(color, gfx::LoadOp::Clear, sky);
          b.read(rg_vis, gfx::Access::FragmentRead);
          if (sky_on) {
            b.read(sb_sky.transmittance, gfx::Access::FragmentRead);
            b.read(sb_sky.view, gfx::Access::FragmentRead);
            b.read(sb_sky.aerial, gfx::Access::FragmentRead);
            b.read(sb_sky.frame, gfx::Access::FragmentRead);
          }
          if (csm && cascades_drawn > 0) b.read(rg_atlas, gfx::Access::SampledRead);
          // A pixel's pair decodes through the scene's pair table, which no pass writes; the entry
          // it was drawn as is read only for a deformed instance's pool block.
          if (cull_on && deform_on) b.read(rg_pair_entries, gfx::Access::FragmentRead);
          // The per-tile coverage mask the last Hi-Z build left behind it, in the same buffer.
          if (occlusion) b.read(rg_hiz, gfx::Access::FragmentRead);
          if (deform_on) {
            b.read(rg_pool, gfx::Access::FragmentRead);
            b.read(rg_slots, gfx::Access::FragmentRead);
          }
          read_tables(b, gfx::Access::FragmentRead, true);
          read_pool(b, gfx::Access::FragmentRead);
          if (shadows) {  // the shadow rays traverse them from the fragment stage
            b.read(rt.tlas, gfx::Access::FragmentRayQueryRead);
            b.read(rt.blas_data, gfx::Access::FragmentRayQueryRead);
            b.read(rt.clas_data, gfx::Access::FragmentRayQueryRead);
          }
        },
        [&](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Graphics, pipelines.resolve);
          bindless.bind(cb, gfx::BindPoint::Graphics);
          for (u32 v = 0; v < views; ++v) {
            timer.begin(cb, k_zone_names[k_zone_resolve][v]);
            const View& view = views_[v];
            set_view_viewport(cb, view.rect.x, view.rect.y, view.rect.width, view.rect.height);
            cb.push_constants(bindless.pipeline_layout(), gfx::ShaderStage::All, 0, sizeof(u64),
                              &view_frames[v].resolve_address);
            cb.draw(3, 1, 0, 0);
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
          if (deform_on) b.read(rg_alloc, gfx::Access::TransferRead);
          if (vertex_indexed) b.read(rg_vertex_headers, gfx::Access::TransferRead);
          if (rt_chain) b.read(rt.record_count, gfx::Access::TransferRead);
          if (csm) b.read(rg_shadow_args, gfx::Access::TransferRead);
          b.write(rg_stats, gfx::Access::TransferWrite);
        },
        [&, stat_target](gfx::CommandList cb, gfx::RenderGraph&) {
          const gfx::BufferResource* arg_blocks[3] = {&scene.draw_args[0], &scene.draw_args[1],
                                                      &scene.sw_args};
          for (u32 v = 0; v < views; ++v) {
            for (u32 i = 0; i < 3; ++i) {
              const gfx::BufferCopy copy{view_frames[v].args_offset,
                                         sizeof(u32) * (u64{v} * k_stat_words + 3 * i),
                                         sizeof(u32) * 3};
              cb.copy_buffer(arg_blocks[i]->buffer, stat_target->buffer, copy);
            }
            // The indexed draw's cursor and fallback count, words 5 and 15 of each run's header.
            for (u32 run = 0; vertex_indexed && run < 2; ++run) {
              const u64 header = scene.vertex_header_offset(v, run);
              const u64 at = sizeof(u32) * (u64{v} * k_stat_words + 9 + 2 * run);
              const gfx::BufferCopy cursor{header + offsetof(gfx::VertexDrawHeader, cursor), at,
                                           sizeof(u32)};
              const gfx::BufferCopy overflow{header + offsetof(gfx::VertexDrawHeader, overflow),
                                             at + sizeof(u32), sizeof(u32)};
              cb.copy_buffer(scene.vertex_headers.buffer, stat_target->buffer, cursor);
              cb.copy_buffer(scene.vertex_headers.buffer, stat_target->buffer, overflow);
            }
          }
          if (deform_on) {
            const gfx::BufferCopy copy{0, sizeof(u32) * u64{k_stat_words} * views,
                                       sizeof(gfx::DeformAlloc)};
            cb.copy_buffer(scene.deform_alloc.buffer, stat_target->buffer, copy);
          }
          // What the ray tracing chain built and wanted, which is what its capacity is sized by.
          if (rt_chain) {
            const gfx::BufferCopy copy{0, sizeof(u32) * (u64{k_stat_words} * views + k_alloc_words),
                                       sizeof(u32) * k_rt_words};
            cb.copy_buffer(scene.record_count.buffer, stat_target->buffer, copy);
          }
          // Each shadow cascade's block and, on the indexed draw, its cursor and fallback count,
          // behind the chain's.
          for (u32 c = 0; c < cascade_runs; ++c) {
            const u64 at = sizeof(u32) * (shadow_stat_base(views) + u64{k_shadow_stat_words} * c);
            const gfx::BufferCopy args{scene.shadow_args_offset(c), at, sizeof(u32) * 3};
            cb.copy_buffer(scene.shadow_args.buffer, stat_target->buffer, args);
            if (vertex_indexed) {
              const u64 header = scene.shadow_vertex_header_offset(c);
              const gfx::BufferCopy cursor{header + offsetof(gfx::VertexDrawHeader, cursor),
                                           at + 3 * sizeof(u32), sizeof(u32)};
              const gfx::BufferCopy overflow{header + offsetof(gfx::VertexDrawHeader, overflow),
                                             at + 4 * sizeof(u32), sizeof(u32)};
              cb.copy_buffer(scene.vertex_headers.buffer, stat_target->buffer, cursor);
              cb.copy_buffer(scene.vertex_headers.buffer, stat_target->buffer, overflow);
            }
          }
        });
  }
  // The sky's sums into this slot's statistics block, behind everything else there: the exposure
  // the frame was drawn at and the light it was metered from, read one frame late (SkyStats).
  if (sky_on && !direct) {
    const gfx::BufferResource* stat_target = &stat_blocks_[slot];
    const u64 at = sky_stat_offset(views, cascade_runs);
    graph.add_pass(
        "sky stats", gfx::PassKind::Transfer,
        [&](gfx::PassBuilder& b) {
          b.read(sb_sky.frame, gfx::Access::TransferRead);
          b.write(rg_stats, gfx::Access::TransferWrite);
        },
        [&, stat_target, at](gfx::CommandList cb, gfx::RenderGraph&) {
          const gfx::BufferCopy copy{0, at, sizeof(gfx::SkyFrame)};
          cb.copy_buffer(sky_.frame.buffer, stat_target->buffer, copy);
        });
  }
  // The page feedback into this slot's host-visible buffer, read when the slot comes around again.
  // It is one copy of three ranges rather than a readback: the frame never waits, and the manager
  // acts on what the frame `frames_in_flight` ago asked for, which is what §4.9's "the CPU streams
  // them" costs in latency and what the drawing rule's fallback covers in the meantime.
  if (streaming) {
    const gfx::BufferResource* target = &streamer_.feedback(slot);
    graph.add_pass(
        "page feedback", gfx::PassKind::Transfer,
        [&](gfx::PassBuilder& b) {
          b.read(sb.request_count, gfx::Access::TransferRead);
          b.read(sb.requests, gfx::Access::TransferRead);
          b.read(sb.used, gfx::Access::TransferRead);
          b.write(sb.feedback, gfx::Access::TransferWrite);
        },
        [&, target](gfx::CommandList cb, gfx::RenderGraph&) {
          const u64 request_bytes = u64{scene.max_requests()} * sizeof(geometry::PageRequest);
          const gfx::BufferCopy count{0, 0, sizeof(u32)};
          cb.copy_buffer(scene.request_count.buffer, target->buffer, count);
          const gfx::BufferCopy requests{0, sizeof(u32), request_bytes};
          cb.copy_buffer(scene.page_requests.buffer, target->buffer, requests);
          const gfx::BufferCopy used{0, sizeof(u32) + request_bytes,
                                     u64{scene.page_count()} * sizeof(u32)};
          cb.copy_buffer(scene.page_used.buffer, target->buffer, used);
        });
  }
  graph.set_final_layout(color, frame.final_layout);
  phase_ns_[2] = time::monotonic_ns();
  const bool compiled = graph.compile(error);
  phase_ns_[3] = time::monotonic_ns();
  if (compiled) graph.execute(commands_);
  phase_ns_[4] = time::monotonic_ns();
  // A field's staging comes back to the scene once this frame is done (GpuScene::terrain_staging),
  // and the staging ring's share with it; what the frame frees is the buffers a staging the ring
  // had no room for made, recorded or not: nothing else will.
  u64 terrain_bytes = terrain_frame_.geometry_bytes;
  for (u32 c = 0; c < terrain_copies; ++c)
    terrain_bytes += terrain_frame_.copies[c].bytes;
  for (const gfx::BufferResource& staging : terrain_frame_.retire)
    frames_.defer_destroy(staging);
  terrain_frame_.retire.clear();
  if (slot < slot_terrain_bytes_.size()) slot_terrain_bytes_[slot] = terrain_bytes;
  return compiled;
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
  first.final_layout = gfx::ImageLayout::TransferSrc;
  first.wait = {};
  first.signal = {};
  if (!render_offscreen(first, error)) return false;
  if (channels.color) {
    gfx::Capture shot;
    if (!gfx::capture_image(*device_, color_, gfx::ImageLayout::TransferSrc, shot, error) ||
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
    if (!gfx::capture_image(*device_, color_, gfx::ImageLayout::TransferSrc, shot, error) ||
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

// The visibility buffer holds `depth << 32 | (pair << 8 | triangle)`, and the pair is the scene's
// (docs/subsystems/gfx.md, "The tie rule"), so it decodes into the {instance, cluster} it names
// with nothing but the scene: `GpuScene::pair_cluster`, the inverse of the cull pass's arithmetic.
// That is what a capture reports — the pair, not an index into a list that will not exist a frame
// later — and it needs no readback of the frame's list. The no-cull case, where the list is null
// and the rasterizers draw instance 0's clusters in index order, names cluster i as pair i.
//
// **A capture is in the target's pixels, whatever the layout.** Each view owns a rectangle of the
// target and a region of the visibility buffer, so the walk is per view: a pixel inside a view's
// rectangle reads that view's region, a pixel no view covers is empty, and a view whose source is
// wider than its rectangle — a Panini view — is sampled through the same map the resolve used, so
// an id in a capture names what is under that pixel of the picture.
bool SceneRenderer::read_visibility(CapturedFrame& out, const CaptureChannels& channels,
                                    std::string* error) {
  Vector<u8> vis_bytes;
  if (!read_buffer(*device_, targets_.vis, targets_.vis.size, vis_bytes, error)) return false;
  const bool has_list = resolved_.settings.cull && scene_->visible_run_bytes() > 0;
  const auto* values = reinterpret_cast<const u64*>(vis_bytes.data());
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
        const u32 pair = id >> 8;
        u32 instance = 0;
        u32 cluster = pair;
        if (has_list && !scene_->pair_cluster(pair, instance, cluster)) {
          instance = k_no_id;  // a pair past the scene's: the frame and the scene disagree
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
