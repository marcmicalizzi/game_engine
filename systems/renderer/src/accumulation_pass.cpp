#include <accumulation_pass.h>
#include <motion_pass.h>
#include <renderer_log.h>
#include <shaders/accumulate.spv.h>

namespace engine::renderer {

namespace {

constexpr u32 k_workgroup = 8;  // accumulate.slang's

}  // namespace

bool AccumulationPass::create(const gfx::Device& device, GpuScene& scene, const MotionPass& motion,
                              gfx::ShaderLibrary& shaders, u32 width, u32 height,
                              u32 frames_in_flight, const Options& options, std::string* error) {
  device_ = &device;
  scene_ = &scene;
  motion_ = &motion;
  options_ = options;
  width_ = width;
  height_ = height;
  shaders.add_embedded("accumulate", shaders::k_accumulate_spirv, shaders::k_accumulate_spirv_size);
  const gfx::Shader* shader = shaders.get("accumulate", error);
  if (shader == nullptr) return false;
  const gfx::DescriptorSetLayoutHandle set_layout = scene.bindless().layout();
  const std::span<const gfx::DescriptorSetLayoutHandle> sets(&set_layout, 1);
  if (!gfx::create_compute_pipeline(device, shader->module, "accumulate_main", sets, sizeof(u64),
                                    pipeline_, error)) {
    return false;
  }
  params_.resize(frames_in_flight);
  for (gfx::BufferResource& block : params_) {
    if (!gfx::create_buffer(device, sizeof(AccumulateParams) * k_max_views,
                            gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress, true,
                            block, error)) {
      return false;
    }
  }
  return create_images(error);
}

bool AccumulationPass::create_images(std::string* error) {
  gfx::BindlessSet& bindless = scene_->bindless();
  for (u32 i = 0; i < 2; ++i) {
    if (!gfx::create_image_2d(*device_, width_, height_, k_frame_pass_format,
                              gfx::ImageUsage::Storage | gfx::ImageUsage::Sampled, images_[i],
                              error) ||
        !gfx::create_image_view(*device_, images_[i], image_views_[i], error)) {
      return false;
    }
    storage_slots_[i] = bindless.add_storage_image(image_views_[i]);
    sampled_slots_[i] =
        bindless.add_sampled_image(image_views_[i], gfx::ImageLayout::ShaderReadOnly);
    if (storage_slots_[i] == gfx::BindlessSet::k_invalid_slot ||
        sampled_slots_[i] == gfx::BindlessSet::k_invalid_slot) {
      if (error != nullptr) *error = "no bindless slot for the accumulation's images";
      return false;
    }
  }
  written_ = false;
  count_ = 0;
  return true;
}

void AccumulationPass::destroy_images() noexcept {
  if (device_ == nullptr) return;
  const bool slots = scene_ != nullptr && scene_->valid();
  for (u32 i = 0; i < 2; ++i) {
    if (slots) {
      gfx::BindlessSet& bindless = scene_->bindless();
      if (storage_slots_[i] != gfx::BindlessSet::k_invalid_slot)
        bindless.release_storage_image(storage_slots_[i], 0);
      if (sampled_slots_[i] != gfx::BindlessSet::k_invalid_slot)
        bindless.release_sampled_image(sampled_slots_[i], 0);
    }
    storage_slots_[i] = sampled_slots_[i] = gfx::BindlessSet::k_invalid_slot;
    gfx::destroy_image_view(*device_, image_views_[i]);
    if (images_[i].image.valid()) gfx::destroy_image(*device_, images_[i]);
    image_views_[i] = {};
    images_[i] = gfx::ImageResource{};
  }
  if (slots) scene_->bindless().recycle(0);
  written_ = false;
}

bool AccumulationPass::resize(u32 width, u32 height, std::string* error) {
  destroy_images();
  width_ = width;
  height_ = height;
  return create_images(error);
}

void AccumulationPass::destroy() noexcept {
  if (device_ == nullptr) return;
  destroy_images();
  gfx::destroy_compute_pipeline(*device_, pipeline_);
  for (gfx::BufferResource& block : params_)
    gfx::destroy_buffer(*device_, block);
  *this = AccumulationPass{};
}

u64 AccumulationPass::device_bytes() const noexcept {
  u64 bytes = u64{width_} * height_ * 8 * 2;  // two half-float images of four channels
  for (const gfx::BufferResource& block : params_)
    bytes += block.size;
  return bytes;
}

bool AccumulationPass::record_thunk(const FramePassContext& context, void* user,
                                    std::string* error) {
  return static_cast<AccumulationPass*>(user)->record(context, error);
}

bool AccumulationPass::record(const FramePassContext& context, std::string* error) {
  if (!context.motion.valid()) {
    if (error != nullptr) *error = "the accumulation was given no motion buffer";
    return false;
  }
  // **Whether there is a history**: the frame before this one drew into the other image, and the
  // motion pass measured this frame from it. A reset (a cut, a resize), a frame this pass did not
  // draw between the two (a debug view) and the first frame of all have none, and start again.
  const u64 serial = motion_->serial();
  const bool history = written_ && motion_->had_history() && written_serial_ + 1 == serial;
  count_ = history ? count_ + 1 : 1;
  const f32 share = 1.0f / static_cast<f32>(count_);
  // The running mean while the history is shorter than the blend's own length, so the frames after
  // a reset weigh alike; then the exponential blend. `mean` mode is the running mean for ever.
  const f32 weight = options_.mean || share > options_.weight ? share : options_.weight;
  const u32 before = current_;
  current_ = 1 - current_;
  written_ = true;
  written_serial_ = serial;

  const u32 slot = context.slot;
  auto* blocks = static_cast<AccumulateParams*>(params_[slot].mapped);
  view_count_ = context.view_count;
  // Last frame's names are the scene's pairs, read through this frame's table: good unless the
  // scene's tables changed between the two (a streamed world's tail), when a pair may name another
  // instance and the colours alone decide.
  const bool names = scene_->pair_table.address != 0 && !motion_->tables_changed();
  for (u32 v = 0; v < view_count_; ++v) {
    const FramePassView& view = context.views[v];
    AccumulateParams p{};
    p.visibility = view.visibility;
    p.prev_visibility = motion_->prev_visibility_address(v);
    p.motion = motion_->motion_address(v);
    p.pairs = scene_->pair_table.address;
    p.pair_count = scene_->pair_count();
    p.width = view.width;
    p.height = view.height;
    p.rect_x = view.x;
    p.rect_y = view.y;
    p.color = context.color_storage;
    p.history = storage_slots_[before];
    p.output = storage_slots_[current_];
    p.weight = weight;
    p.clip_still = options_.clip_still;
    p.clip_moving = options_.clip_moving;
    p.prev_motion = motion_->prev_motion_address(v);
    p.motion_match = options_.motion_match;
    p.moved = motion_->moved_address(slot);
    p.moved_count = p.moved != 0 ? motion_->moved_count() : 0;
    p.knee = context.display_knee;
    p.ceiling = context.display_ceiling;
    p.filter = options_.filter;
    p.reactive_weight = options_.reactive_weight;
    if (scene_->has_water()) {
      p.materials = scene_->materials.address;
      p.cluster_materials = scene_->cluster_materials.address;
      p.instances = scene_->instances.address;
    }
    // A view that resamples a wider source (Panini) has no pixel-for-pixel motion or names: its
    // mean is still its mean, and otherwise it is drawn as it was.
    const bool resampled = view.panini;
    if (options_.mean) {
      p.flags = k_accumulate_mean | (history ? k_accumulate_history : 0u);
    } else if (history && !resampled) {
      p.flags = k_accumulate_history | (names ? k_accumulate_names : 0u);
    }
    blocks[v] = p;
    view_width_[v] = view.width;
    view_height_[v] = view.height;
  }

  gfx::RenderGraph& graph = *context.graph;
  // The image this frame writes holds nothing worth keeping; the other was last read by the encode
  // of the frame that wrote it.
  const gfx::RgImage out =
      graph.import_image("accumulated", images_[current_], gfx::ImageLayout::Undefined,
                         gfx::PipelineStage::ComputeShader | gfx::PipelineStage::FragmentShader,
                         gfx::MemoryAccess::ShaderSampledRead);
  gfx::RgImage past{};
  gfx::RgBuffer past_names{};
  gfx::RgBuffer past_motion{};
  if (history) {
    past = graph.import_image("accumulated before", images_[before],
                              gfx::ImageLayout::ShaderReadOnly, gfx::PipelineStage::FragmentShader,
                              gfx::MemoryAccess::ShaderSampledRead);
    if (!options_.mean) {
      past_names = graph.import_buffer("visibility before", motion_->prev_visibility());
      past_motion = graph.import_buffer("motion before", motion_->prev_motion());
    }
  }
  const gfx::RgImage color = context.color;
  const gfx::RgBuffer visibility = context.visibility;
  const gfx::RgBuffer motion = context.motion;
  const bool mean = options_.mean;
  graph.add_pass(
      "accumulate", gfx::PassKind::Compute,
      [&](gfx::PassBuilder& b) {
        b.read(color, gfx::Access::ComputeRead);
        if (!mean) {
          b.read(visibility, gfx::Access::ComputeRead);
          b.read(motion, gfx::Access::ComputeRead);
        }
        if (past.valid()) b.read(past, gfx::Access::ComputeRead);
        if (past_names.valid()) b.read(past_names, gfx::Access::ComputeRead);
        if (past_motion.valid()) b.read(past_motion, gfx::Access::ComputeRead);
        b.write(out, gfx::Access::ComputeWrite);
      },
      [this, slot, timer = context.timer, bindless = context.bindless](gfx::CommandList cb,
                                                                       gfx::RenderGraph&) {
        cb.bind_pipeline(gfx::BindPoint::Compute, pipeline_.pipeline);
        bindless->bind(cb, gfx::BindPoint::Compute);
        timer->begin(cb, "accumulate");
        for (u32 v = 0; v < view_count_; ++v) {
          const u64 block = params_[slot].address + u64{v} * sizeof(AccumulateParams);
          cb.push_constants(pipeline_.layout, gfx::ShaderStage::Compute, 0, sizeof(u64), &block);
          cb.dispatch((view_width_[v] + k_workgroup - 1) / k_workgroup,
                      (view_height_[v] + k_workgroup - 1) / k_workgroup, 1);
        }
        timer->end(cb);
      });
  // The frame's picture from here on is this image: the encode reads it.
  context.report->color = out;
  context.report->color_sampled = sampled_slots_[current_];
  return true;
}

}  // namespace engine::renderer
