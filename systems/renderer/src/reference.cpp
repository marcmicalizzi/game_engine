#include <core/platform/process.h>
#include <core/time/time.h>
#include <domain/gfx/path_trace.h>
#include <domain/gfx/requirements.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/lighting.h>
#include <systems/renderer/reference.h>

#include <cstring>
#include <renderer_log.h>
#include <shaders/path_trace.spv.h>

namespace engine::renderer {

namespace {

constexpr gfx::BufferUsage k_address =
    gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress;
constexpr gfx::BufferUsage k_readable = k_address | gfx::BufferUsage::TransferSrc;

// Copies a device buffer into host memory through a staging buffer and a blocking submission.
// A reference render is not a frame path — it is minutes of dispatches followed by two
// readbacks — so the rule that keeps readbacks out of the frame does not reach here.
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

bool reference_available(const ResolvedSettings& resolved, const gfx::Device& device,
                         std::string* why) {
  // The reference traces the structures the *frame* builds, so it needs both the device that can
  // build them and a frame configured to. That is deliberate rather than a limitation worked
  // around: a reference built from its own geometry would be a reference for a different scene.
  const gfx::DeviceFeatures& features = device.features();
  if (!features.cluster_acceleration_structure || !features.ray_query) {
    if (why != nullptr) {
      const std::string sentence = gfx::ray_tracing_degradation(device.caps());
      *why = sentence.empty() ? std::string("has no cluster acceleration structures or ray queries")
                              : "cannot trace rays: " + sentence;
    }
    return false;
  }
  if (!resolved.rt_chain) {
    if (why != nullptr) {
      *why =
          "these settings build no acceleration structures; the reference traces the ones the "
          "frame builds, so ask for --shadows rt or --raster rt";
    }
    return false;
  }
  if (resolved.view_count != 1) {
    if (why != nullptr) {
      *why = "the reference renders one view; surround3 and panini are not supported yet";
    }
    return false;
  }
  return true;
}

ReferenceRenderer::~ReferenceRenderer() { destroy(); }

bool ReferenceRenderer::create(const gfx::Device& device, GpuScene& scene, SceneRenderer& renderer,
                               const Desc& desc, std::string* error) {
  destroy();
  device_ = &device;
  scene_ = &scene;
  renderer_ = &renderer;
  desc_ = desc;

  std::string why;
  if (!reference_available(renderer.settings(), device, &why)) {
    if (error != nullptr) *error = std::string(device.adapter().name) + " " + why;
    destroy();
    return false;
  }
  if (!shaders_.create(&device, error)) {
    destroy();
    return false;
  }
  shaders_.add_embedded("path_trace", shaders::k_path_trace_spirv,
                        shaders::k_path_trace_spirv_size);
  std::string manifest = desc.shader_manifest;
  if (manifest.empty()) {
    const std::string candidate = platform::executable_directory() + "/../shaders/manifest.json";
    if (io::exists(candidate)) manifest = candidate;
  }
  if (!manifest.empty() && !shaders_.load_manifest(manifest, error)) {
    destroy();
    return false;
  }
  if (!create_pipelines(error) || !create_targets(error)) {
    destroy();
    return false;
  }
  // The block, and the ground's detail behind it (renderer.md, "The sand close up").
  if (!gfx::create_buffer(device, sizeof(gfx::PathTraceParams) + sizeof(gfx::GroundDetailParams),
                          k_address, true, params_, error) ||
      !gfx::create_buffer(device, sizeof(gfx::ResolveLight) * k_frame_lights, k_address, true,
                          lights_, error) ||
      !timer_.create(device, 1, 4, error)) {
    destroy();
    return false;
  }
  ENGINE_LOG_INFO(log_renderer, "reference renderer ready", log::field("width", width_),
                  log::field("height", height_));
  return true;
}

bool ReferenceRenderer::create_pipelines(std::string* error) {
  const gfx::Shader* shader = shaders_.get("path_trace", error);
  if (shader == nullptr) return false;
  const gfx::DescriptorSetLayoutHandle set_layout = scene_->bindless().layout();
  const std::span<const gfx::DescriptorSetLayoutHandle> layouts(&set_layout, 1);
  return gfx::create_compute_pipeline(*device_, shader->module, "path_trace_main", layouts,
                                      sizeof(u64), trace_, error) &&
         gfx::create_compute_pipeline(*device_, shader->module, "tonemap_main", layouts,
                                      sizeof(u64), tonemap_, error);
}

bool ReferenceRenderer::create_targets(std::string* error) {
  width_ = renderer_->width();
  height_ = renderer_->height();
  gfx::destroy_buffer(*device_, accum_);
  gfx::destroy_buffer(*device_, output_);
  accum_ = gfx::BufferResource{};
  output_ = gfx::BufferResource{};
  gfx::destroy_buffer(*device_, coverage_);
  coverage_ = gfx::BufferResource{};
  const u64 pixels = u64{width_} * height_;
  return gfx::create_buffer(*device_, pixels * 4 * sizeof(f32), k_readable, false, accum_, error) &&
         gfx::create_buffer(*device_, pixels * sizeof(u32), k_readable, false, output_, error) &&
         gfx::create_buffer(*device_, pixels * sizeof(u32), k_address, false, coverage_, error);
}

bool ReferenceRenderer::resize(std::string* error) {
  if (device_ == nullptr) return false;
  if (renderer_->width() == width_ && renderer_->height() == height_) return true;
  renderer_->wait_idle();
  return create_targets(error);
}

void ReferenceRenderer::destroy() noexcept {
  if (device_ == nullptr) return;
  const gfx::Device& device = *device_;
  if (renderer_ != nullptr && renderer_->valid()) renderer_->wait_idle();
  timer_.destroy();
  gfx::destroy_compute_pipeline(device, trace_);
  gfx::destroy_compute_pipeline(device, tonemap_);
  gfx::destroy_buffer(device, accum_);
  gfx::destroy_buffer(device, output_);
  gfx::destroy_buffer(device, coverage_);
  gfx::destroy_buffer(device, params_);
  gfx::destroy_buffer(device, lights_);
  accum_ = gfx::BufferResource{};
  output_ = gfx::BufferResource{};
  coverage_ = gfx::BufferResource{};
  params_ = gfx::BufferResource{};
  lights_ = gfx::BufferResource{};
  shaders_.destroy();
  device_ = nullptr;
  scene_ = nullptr;
  renderer_ = nullptr;
  width_ = height_ = 0;
}

bool ReferenceRenderer::render(const Camera& camera, const ReferenceSettings& settings,
                               ReferenceFrame& out, std::string* error, ReferenceProgress progress,
                               void* user) {
  if (device_ == nullptr) {
    if (error != nullptr) *error = "the reference renderer was not created";
    return false;
  }
  if (settings.spp == 0) {
    if (error != nullptr) *error = "spp must be at least 1";
    return false;
  }
  if (!resize(error)) return false;
  const i64 started = time::monotonic_ns();
  out = ReferenceFrame{};
  out.width = width_;
  out.height = height_;

  // **One real-time frame first.** That is what runs the cull pass, fills the visible list, and
  // builds the cluster acceleration structures and the top-level structure over them — the
  // geometry every ray below traces. `lod_px` is the one thing the reference changes about it.
  FrameDesc frame;
  frame.camera = camera;
  frame.frame_index = settings.frame_index;
  frame.sun_time_s = settings.sun_time_s;
  frame.lod_px = settings.finest ? 0.0f : -1.0f;
  // Every structure the frame wants, budget or not: the converged picture traces nothing else, and
  // the finest cut is every leaf in view (docs/subsystems/renderer.md, "The ray tracing chain's
  // memory"). The chain returns under its budget on the next real-time frame.
  frame.rt_complete = true;
  // The pose, straight through. The frame writes the deformed-vertex pool from it and builds the
  // acceleration structures from the pool, so every ray below traces the character in the pose the
  // caller's tick produced without this file knowing what a clip is.
  frame.joints = settings.joints;
  frame.instance_joints = settings.instance_joints;
  if (!renderer_->render_offscreen(frame, error)) return false;
  out.visible_pairs = renderer_->stats().visible_pairs();

  const RenderSettings& resolved = renderer_->settings().settings;
  FrameLighting lighting;
  // With a sky, the frame's own: its lights, and its block and tables, which the frame just built
  // and nothing touches until the renderer draws again (sky.h).
  const bool sky = renderer_->sky().active() && !settings.uniform_sky;
  if (sky) {
    frame_lighting(scene_->data(), renderer_->frame_sky(), lighting);
  } else {
    frame_lighting(scene_->data(), settings.frame_index,
                   lighting_options(resolved, settings.sun_time_s), lighting);
  }
  std::memcpy(lights_.mapped, lighting.lights, sizeof(lighting.lights));

  const View& view = renderer_->views()[0];
  gfx::PathTraceParams params{};
  params.inv_view_proj = inverse(view.view_proj);
  params.camera = Vec4{camera.position, 0.0f};
  params.sky = lighting.sky;
  params.sun = lighting.sun;
  params.ground = lighting.ground;
  params.accum = accum_.address;
  params.output = output_.address;
  params.coverage = coverage_.address;
  params.background = gfx::pack_unorm_rgba8(lighting.sky);
  params.clusters = scene_->clusters.address;
  params.mesh = scene_->meshes.address;
  params.triangles = scene_->triangles.address;
  params.instances = scene_->instances.address;
  params.attributes = scene_->attributes.address;
  params.materials = scene_->materials.address;
  params.cluster_materials = scene_->cluster_materials.address;
  // The same rule the resolve follows: a null list means entry i is {0, i}, which is what a
  // frame drawn without the cull pass produces. The reference always runs with culling on
  // (the acceleration structure chain forces it), so this is the cull's own list.
  params.visible = resolved.cull ? scene_->visible.address : 0;
  params.lights = lights_.address;
  params.width = width_;
  params.height = height_;
  params.light_count = lighting.light_count;
  params.scene = scene_->tlas_slot();
  params.max_bounces = settings.max_bounces;
  params.seed = settings.seed;
  params.ray_bias = lighting.shadow_bias;
  params.ray_bias_steps = lighting.shadow_bias_steps;
  params.flags = (settings.uniform_sky ? gfx::k_pt_uniform_sky : 0u) |
                 (settings.pixel_center ? gfx::k_pt_pixel_center : 0u);
  params.sky_params = sky ? renderer_->sky_params_address() : 0;
  // A furnace is a closed environment and *nothing else*: a furnace with a sun in it does not
  // test energy conservation, it tests the sun. So the flag that makes the environment uniform
  // also puts the sun and the analytic lights out.
  if (settings.uniform_sky) {
    params.sun.w = 0.0f;
    params.light_count = 0;
  }
  // The sand's detail: the block the frame's resolve read, the same numbers and the same wind at
  // the same surface time, which the path tracer draws unfiltered (path_trace.slang says why).
  if (scene_->ground_detail()) {
    const gfx::GroundDetailParams detail = scene_->ground_detail_params();
    std::memcpy(static_cast<u8*>(params_.mapped) + sizeof(gfx::PathTraceParams), &detail,
                sizeof(detail));
    params.ground_detail = params_.address + sizeof(gfx::PathTraceParams);
  }

  const u32 batch = settings.batch == 0 ? settings.spp : settings.batch;
  const u32 groups_x = gfx::path_trace_group_count(width_);
  const u32 groups_y = gfx::path_trace_group_count(height_);
  const u64 address = params_.address;
  gfx::BindlessSet& bindless = scene_->bindless();

  // Batches rather than one dispatch, for three reasons: progress can be reported, a render can
  // be stopped and still produce a valid (noisier) picture because accumulation is progressive,
  // and no single submission runs long enough to meet the display driver's watchdog. The
  // accumulator is seeded by (pixel, sample index, seed), so the split changes nothing about the
  // image — ten batches of ten samples are the hundred-sample picture, exactly.
  u32 done = 0;
  bool stopped = false;
  while (done < settings.spp && !stopped) {
    const u32 count = batch < settings.spp - done ? batch : settings.spp - done;
    params.spp = count;
    params.sample_base = done;
    std::memcpy(params_.mapped, &params, sizeof(params));
    const bool ok = gfx::submit_immediate(
        *device_,
        [&](gfx::CommandList cb) {
          timer_.begin_frame(cb, 0);  // reads the previous batch's timestamps, resets the pool
          out.trace_ms += timer_.total_ms();
          cb.bind_pipeline(gfx::BindPoint::Compute, trace_.pipeline);
          bindless.bind(cb, gfx::BindPoint::Compute);
          cb.push_constants(trace_.layout, gfx::ShaderStage::Compute, 0, sizeof(u64), &address);
          timer_.begin(cb, "trace");
          cb.dispatch(groups_x, groups_y, 1);
          timer_.end(cb);
        },
        error);
    if (!ok) return false;
    done += count;
    out.samples = done;
    if (progress != nullptr && !progress(done, settings.spp, user)) stopped = true;
  }

  // The accumulator to bytes, through the display transform the resolve ends on.
  params.spp = 0;
  std::memcpy(params_.mapped, &params, sizeof(params));
  if (!gfx::submit_immediate(
          *device_,
          [&](gfx::CommandList cb) {
            timer_.begin_frame(cb, 0);  // the last trace batch's timestamps
            out.trace_ms += timer_.total_ms();
            cb.bind_pipeline(gfx::BindPoint::Compute, tonemap_.pipeline);
            bindless.bind(cb, gfx::BindPoint::Compute);
            cb.push_constants(tonemap_.layout, gfx::ShaderStage::Compute, 0, sizeof(u64), &address);
            cb.dispatch(groups_x, groups_y, 1);
          },
          error)) {
    return false;
  }

  const u64 pixels = u64{width_} * height_;
  Vector<u8> bytes;
  if (!read_buffer(*device_, accum_, pixels * 4 * sizeof(f32), bytes, error)) return false;
  out.hdr.resize(static_cast<u32>(pixels * 4));
  {
    const auto* values = reinterpret_cast<const f32*>(bytes.data());
    for (u64 p = 0; p < pixels; ++p) {
      const u32 base = static_cast<u32>(p * 4);
      const f32 samples = values[base + 3];
      const f32 scale = samples > 0.0f ? 1.0f / samples : 0.0f;
      out.hdr[base + 0] = values[base + 0] * scale;
      out.hdr[base + 1] = values[base + 1] * scale;
      out.hdr[base + 2] = values[base + 2] * scale;
      out.hdr[base + 3] = samples;
    }
  }
  if (!read_buffer(*device_, output_, pixels * sizeof(u32), bytes, error)) return false;
  out.color.resize(static_cast<u32>(pixels * 4));
  std::memcpy(out.color.data(), bytes.data(), pixels * 4);
  out.seconds = static_cast<f64>(time::monotonic_ns() - started) / 1.0e9;
  return true;
}

}  // namespace engine::renderer
