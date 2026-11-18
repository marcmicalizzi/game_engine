#include <foundation/tunables/tunables.h>
#include <systems/renderer/request.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exposure_pass.h>
#include <shaders/exposure.spv.h>

namespace engine::renderer {

namespace {

// Read when a renderer is made (renderer.md, "Exposure: the eye's adaptation"; the defaults and the
// sources they were read from are on docs/experiments/eye-adaptation.md).
tunables::Bool adapt{
    "renderer.exposure.adapt", false,
    "Meter the frame and adapt its exposure to it as an eye does (a scene's "
    "Sky.exposure: Adapted turns it on for that scene); a fixed exposure value pins "
    "it"};
tunables::Float low{"renderer.exposure.low", 0.10, 0.0, 1.0,
                    "The share of the metered weight, darkest first, left out of the average"};
tunables::Float high{"renderer.exposure.high", 0.90, 0.0, 1.0,
                     "The share of the metered weight, darkest first, where the average stops: "
                     "the brightest rest (a doorway to noon, the sun) is left out"};
tunables::Float min_ev{"renderer.exposure.min_ev100", -6.0, -20.0, 30.0,
                       "The lowest exposure value adaptation reaches (how far a dark frame is "
                       "lifted)"};
tunables::Float max_ev{"renderer.exposure.max_ev100", 18.0, -20.0, 30.0,
                       "The highest exposure value adaptation reaches"};
tunables::Float dark_tau{"renderer.exposure.dark_tau_s", 1.5, 0.0, 600.0,
                         "Seconds: the time constant of dark adaptation (the picture brightening "
                         "after a step into the dark); 0 is at once"};
tunables::Float cone_tau{"renderer.exposure.cone_tau_s", 0.1, 0.0, 60.0,
                         "Seconds: light adaptation's time constant in daylight (the cones')"};
tunables::Float rod_tau{"renderer.exposure.rod_tau_s", 0.4, 0.0, 60.0,
                        "Seconds: light adaptation's time constant at night (the rods')"};

constexpr u32 k_histogram_group = 16;  // exposure.slang's
constexpr f64 k_pi = 3.14159265358979323846;

}  // namespace

ExposureOptions exposure_options_from_tunables() {
  ExposureOptions o;
  o.adapt = adapt.get();
  o.low = static_cast<f32>(low.get());
  o.high = static_cast<f32>(high.get());
  o.min_ev100 = static_cast<f32>(min_ev.get());
  o.max_ev100 = static_cast<f32>(max_ev.get());
  o.dark_tau_s = static_cast<f32>(dark_tau.get());
  o.cone_tau_s = static_cast<f32>(cone_tau.get());
  o.rod_tau_s = static_cast<f32>(rod_tau.get());
  return o;
}

f64 exposure_key(f64 luminance) noexcept {
  return 1.03 - 2.0 / (2.0 + std::log10(std::max(luminance, 0.0) + 1.0));
}

f64 exposure_shown(f64 key) noexcept { return key / (1.0 + key); }

f64 exposure_rod_share(f64 luminance) noexcept { return 0.04 / (0.04 + std::max(luminance, 0.0)); }

f64 exposure_target_ev100(f64 luminance, f64 compensation,
                          const ExposureOptions& options) noexcept {
  const f64 key = std::max(exposure_key(luminance), 1e-3);
  const f64 ev =
      std::log2(k_pi * std::max(luminance, 1e-30) / (2.5 * exposure_shown(key))) - compensation;
  return std::clamp(ev, static_cast<f64>(options.min_ev100), static_cast<f64>(options.max_ev100));
}

f64 exposure_tau_s(f64 current, f64 target, f64 luminance,
                   const ExposureOptions& options) noexcept {
  if (target < current) return static_cast<f64>(options.dark_tau_s);
  const f64 rods = exposure_rod_share(luminance);
  return rods * static_cast<f64>(options.rod_tau_s) +
         (1.0 - rods) * static_cast<f64>(options.cone_tau_s);
}

f64 exposure_step(f64 current, f64 target, f64 luminance, f64 dt_s,
                  const ExposureOptions& options) noexcept {
  const f64 tau = exposure_tau_s(current, target, luminance, options);
  const f64 k = tau > 0.0 ? 1.0 - std::exp(-dt_s / tau) : 1.0;
  return current + (target - current) * k;
}

u32 exposure_metered_view(const ExposureViewRect* views, u32 count, u32 target_width,
                          u32 target_height) noexcept {
  const u32 cx = target_width / 2;
  const u32 cy = target_height / 2;
  for (u32 v = 0; v < count; ++v) {
    const ExposureViewRect& r = views[v];
    if (cx >= r.x && cx < r.x + r.width && cy >= r.y && cy < r.y + r.height) return v;
  }
  return 0;
}

u32 exposure_pixel_weight(const ExposureViewRect& view, u32 px, u32 py) noexcept {
  const f32 dx = static_cast<f32>(px - view.x) + 0.5f - static_cast<f32>(view.width) * 0.5f;
  const f32 dy = static_cast<f32>(py - view.y) + 0.5f - static_cast<f32>(view.height) * 0.5f;
  const f32 radius = static_cast<f32>(view.width) * k_exposure_centre_diameter * 0.5f;
  return dx * dx + dy * dy <= radius * radius ? k_exposure_centre_weight : 1u;
}

bool ExposurePass::create(const gfx::Device& device, GpuScene& scene, gfx::ShaderLibrary& shaders,
                          u32 frames_in_flight, const ExposureOptions& options,
                          std::string* error) {
  device_ = &device;
  options_ = options;
  shaders.add_embedded("exposure", shaders::k_exposure_spirv, shaders::k_exposure_spirv_size);
  const gfx::Shader* shader = shaders.get("exposure", error);
  if (shader == nullptr) return false;
  const gfx::DescriptorSetLayoutHandle set_layout = scene.bindless().layout();
  const std::span<const gfx::DescriptorSetLayoutHandle> sets(&set_layout, 1);
  if (!gfx::create_compute_pipeline(device, shader->module, "exposure_histogram_main", sets,
                                    sizeof(u64), histogram_pipeline_, error) ||
      !gfx::create_compute_pipeline(device, shader->module, "exposure_adapt_main", sets,
                                    sizeof(u64), adapt_pipeline_, error)) {
    return false;
  }
  const gfx::BufferUsage device_usage = gfx::BufferUsage::Storage |
                                        gfx::BufferUsage::ShaderDeviceAddress |
                                        gfx::BufferUsage::TransferDst;
  if (!gfx::create_buffer(device, sizeof(u32) * k_exposure_bins, device_usage, false, histogram_,
                          error) ||
      !gfx::create_buffer(device, sizeof(ExposureState), device_usage, false, state_, error)) {
    return false;
  }
  params_.resize(frames_in_flight);
  reports_.resize(frames_in_flight);
  for (u32 i = 0; i < frames_in_flight; ++i) {
    if (!gfx::create_buffer(device, sizeof(ExposureParams),
                            gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress, true,
                            params_[i], error) ||
        !gfx::create_buffer(device, sizeof(ExposureState),
                            gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress, true,
                            reports_[i], error)) {
      return false;
    }
    std::memset(reports_[i].mapped, 0, sizeof(ExposureState));
  }
  return true;
}

void ExposurePass::destroy() noexcept {
  if (device_ == nullptr) return;
  gfx::destroy_compute_pipeline(*device_, histogram_pipeline_);
  gfx::destroy_compute_pipeline(*device_, adapt_pipeline_);
  gfx::destroy_buffer(*device_, histogram_);
  gfx::destroy_buffer(*device_, state_);
  for (gfx::BufferResource& block : params_)
    gfx::destroy_buffer(*device_, block);
  for (gfx::BufferResource& block : reports_)
    gfx::destroy_buffer(*device_, block);
  *this = ExposurePass{};
}

ExposureState ExposurePass::latest() const noexcept {
  ExposureState out;
  if (recorded_ && last_slot_ < reports_.size()) {
    std::memcpy(&out, reports_[last_slot_].mapped, sizeof(ExposureState));
  }
  return out;
}

u64 ExposurePass::device_bytes() const noexcept {
  return histogram_.size + state_.size +
         (sizeof(ExposureParams) + sizeof(ExposureState)) * static_cast<u64>(params_.size());
}

bool ExposurePass::record_thunk(const FramePassContext& context, void* user, std::string* error) {
  return static_cast<ExposurePass*>(user)->record(context, error);
}

bool ExposurePass::record(const FramePassContext& context, std::string* error) {
  const u32 slot = context.slot;
  if (slot >= params_.size()) {
    if (error != nullptr) *error = "the exposure pass has no block for this frame slot";
    return false;
  }
  // What this slot's last frame reached: done, since the slot came round (begin_frame waited).
  std::memcpy(&reported_, reports_[slot].mapped, sizeof(ExposureState));

  // **The step's time**: the host's, or the frame index's at the clock a path and a capture advance
  // by. The first frame, and one whose index went back, take their target at once; a frame at the
  // same index as the last (a window drawing faster than the index's 60 Hz) steps by nothing.
  const bool snap = !recorded_ || context.frame_index < last_frame_index_;
  f64 dt = context.dt_s;
  if (!(dt > 0.0) && !snap) {
    dt = static_cast<f64>(context.frame_index - last_frame_index_) / static_cast<f64>(k_frame_hz);
  }
  dt = std::clamp(dt, 0.0, 600.0);
  last_frame_index_ = context.frame_index;
  last_slot_ = slot;
  recorded_ = true;

  // **The metered view**: the one holding the target's centre (exposure.h).
  ExposureViewRect rects[k_max_views];
  const u32 view_count = std::min(context.view_count, k_max_views);
  for (u32 v = 0; v < view_count; ++v) {
    rects[v] = ExposureViewRect{context.views[v].x, context.views[v].y, context.views[v].width,
                                context.views[v].height};
  }
  const ExposureViewRect rect =
      view_count > 0
          ? rects[exposure_metered_view(rects, view_count, context.width, context.height)]
          : ExposureViewRect{0, 0, context.width, context.height};

  ExposureParams p;
  p.histogram = histogram_.address;
  p.state = state_.address;
  p.report = reports_[slot].address;
  p.sky = context.sky_params;
  p.rect_x = rect.x;
  p.rect_y = rect.y;
  p.rect_width = rect.width;
  p.rect_height = rect.height;
  p.color = context.color_storage;
  p.flags = snap ? k_exposure_snap : 0u;
  p.dt_s = static_cast<f32>(dt);
  p.low = options_.low;
  p.high = options_.high;
  p.min_ev100 = options_.min_ev100;
  p.max_ev100 = options_.max_ev100;
  p.dark_tau_s = options_.dark_tau_s;
  p.cone_tau_s = options_.cone_tau_s;
  p.rod_tau_s = options_.rod_tau_s;
  std::memcpy(params_[slot].mapped, &p, sizeof(p));
  groups_x_ = (rect.width / k_exposure_stride + k_histogram_group) / k_histogram_group;
  groups_y_ = (rect.height / k_exposure_stride + k_histogram_group) / k_histogram_group;

  gfx::RenderGraph& graph = *context.graph;
  // Both kept from frame to frame and touched only by this pass and the encode, on the one queue.
  const gfx::RgBuffer bins =
      graph.import_buffer("exposure bins", histogram_, gfx::PipelineStage::AllCommands,
                          gfx::MemoryAccess::MemoryRead | gfx::MemoryAccess::MemoryWrite);
  const gfx::RgBuffer state =
      graph.import_buffer("exposure state", state_, gfx::PipelineStage::AllCommands,
                          gfx::MemoryAccess::MemoryRead | gfx::MemoryAccess::MemoryWrite);
  if (!cleared_) {
    // Device memory starts as anything: the bins and the state are zeroed once, and after that the
    // adaptation clears the bins as it reads them.
    cleared_ = true;
    graph.add_pass(
        "exposure clear", gfx::PassKind::Transfer,
        [&](gfx::PassBuilder& b) {
          b.write(bins, gfx::Access::TransferWrite);
          b.write(state, gfx::Access::TransferWrite);
        },
        [this](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.fill_buffer(histogram_.buffer, 0, histogram_.size, 0u);
          cb.fill_buffer(state_.buffer, 0, state_.size, 0u);
        });
  }
  const gfx::RgImage color = context.color;
  const u64 block = params_[slot].address;
  graph.add_pass(
      "exposure histogram", gfx::PassKind::Compute,
      [&](gfx::PassBuilder& b) {
        b.read(color, gfx::Access::ComputeRead);
        if (context.sky_params != 0) b.read(context.sky.frame, gfx::Access::ComputeRead);
        b.write(bins, gfx::Access::ComputeReadWrite);
      },
      [this, block, timer = context.timer, bindless = context.bindless](gfx::CommandList cb,
                                                                        gfx::RenderGraph&) {
        cb.bind_pipeline(gfx::BindPoint::Compute, histogram_pipeline_.pipeline);
        bindless->bind(cb, gfx::BindPoint::Compute);
        timer->begin(cb, "exposure");
        cb.push_constants(histogram_pipeline_.layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                          &block);
        cb.dispatch(groups_x_, groups_y_, 1);
      });
  graph.add_pass(
      "exposure adapt", gfx::PassKind::Compute,
      [&](gfx::PassBuilder& b) {
        if (context.sky_params != 0) b.read(context.sky.frame, gfx::Access::ComputeRead);
        b.write(bins, gfx::Access::ComputeReadWrite);
        b.write(state, gfx::Access::ComputeReadWrite);
      },
      [this, block, timer = context.timer, bindless = context.bindless](gfx::CommandList cb,
                                                                        gfx::RenderGraph&) {
        cb.bind_pipeline(gfx::BindPoint::Compute, adapt_pipeline_.pipeline);
        bindless->bind(cb, gfx::BindPoint::Compute);
        cb.push_constants(adapt_pipeline_.layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                          &block);
        cb.dispatch(1, 1, 1);
        timer->end(cb);
      });
  // The encode multiplies the picture by the state's scale.
  context.report->exposure = state;
  context.report->exposure_address = state_.address + offsetof(ExposureState, scale);
  return true;
}

}  // namespace engine::renderer
