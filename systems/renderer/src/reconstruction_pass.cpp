#include <motion_pass.h>
#include <reconstruction_pass.h>
#include <renderer_log.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene_renderer.h>

namespace engine::renderer {

// `SceneRenderer`'s half (scene_renderer.h, `create_reconstruction`), here so the hub does not grow
// by it: the provider `renderer.reconstruction.provider` names, made after the inputs it reads.
bool SceneRenderer::create_reconstruction(std::string* error) {
  const ReconstructionProviderDesc* desc =
      find_reconstruction_provider(resolved_.reconstruction_provider);
  if (desc == nullptr) {
    if (error != nullptr) {
      *error = std::string("no reconstruction provider is registered as ") +
               resolved_.reconstruction_provider;
    }
    return false;
  }
  motion_->set_phases(resolved_.reconstruction_phases);
  reconstruction_ = new ReconstructionPass;
  if (!motion_->keep_motion(*device_, views_, error) ||
      !reconstruction_->create(*desc, *device_, scene_->bindless(), *motion_, width_, height_,
                               desc_.frames_in_flight, error) ||
      !add_frame_pass(FramePassStage::Reconstruction, &ReconstructionPass::record_thunk,
                      reconstruction_, desc->name, error)) {
    return false;
  }
  ENGINE_LOG_INFO(log_renderer, "sampling over time's provider", log::field("name", desc->name),
                  log::field("device_bytes", reconstruction_->device_bytes()),
                  log::field("inputs_bytes", motion_->device_bytes()));
  return true;
}

bool ReconstructionPass::create(const ReconstructionProviderDesc& desc, const gfx::Device& device,
                                gfx::BindlessSet& bindless, const MotionPass& motion, u32 width,
                                u32 height, u32 frames_in_flight, std::string* error) {
  desc_ = &desc;
  motion_ = &motion;
  ReconstructionCreate made;
  made.device = &device;
  made.bindless = &bindless;
  made.width = width;
  made.height = height;
  made.frames_in_flight = frames_in_flight;
  state_ = desc.create(made, error);
  if (state_ == nullptr) {
    if (error != nullptr && error->empty())
      *error = std::string("the reconstruction provider ") + desc.name + " refused the load";
    return false;
  }
  return true;
}

void ReconstructionPass::destroy() noexcept {
  if (desc_ != nullptr && state_ != nullptr) desc_->destroy(state_);
  *this = ReconstructionPass{};
}

bool ReconstructionPass::resize(u32 width, u32 height, std::string* error) {
  drawn_ = false;
  return desc_->resize == nullptr || desc_->resize(state_, width, height, error);
}

u64 ReconstructionPass::device_bytes() const noexcept {
  return desc_ != nullptr && desc_->device_bytes != nullptr ? desc_->device_bytes(state_) : 0;
}

bool ReconstructionPass::record_thunk(const FramePassContext& context, void* user,
                                      std::string* error) {
  auto& self = *static_cast<ReconstructionPass*>(user);
  const MotionPass& motion = *self.motion_;
  // The history is the frame before this one only if the provider drew it and the motion pass
  // measured this frame from it (the accumulation's own rule, accumulation_pass.cpp).
  const u64 serial = motion.serial();
  ReconstructionFrame in;
  in.frame = &context;
  in.history = self.drawn_ && motion.had_history() && self.drawn_serial_ + 1 == serial;
  self.drawn_ = true;
  self.drawn_serial_ = serial;
  for (u32 v = 0; v < context.view_count && v < k_max_views; ++v) {
    in.motion[v] = motion.motion_address(v);
    in.prev_visibility_address[v] = motion.prev_visibility_address(v);
    in.prev_motion_address[v] = motion.prev_motion().buffer.valid() ? motion.prev_motion_address(v)
                                                                    : 0;
  }
  if (in.history) {
    in.prev_visibility = context.graph->import_buffer("visibility before", motion.prev_visibility());
    if (motion.prev_motion().buffer.valid())
      in.prev_motion = context.graph->import_buffer("motion before", motion.prev_motion());
  }
  in.phase = motion.drawn_phase();
  in.phases = motion.phases();
  sample_offset(in.phase, in.offset_x, in.offset_y);
  in.output_width = context.width;
  in.output_height = context.height;
  ReconstructionOutput out;
  if (!self.desc_->record(self.state_, in, out, error)) return false;
  if (out.color.valid()) {
    context.report->color = out.color;
    context.report->color_sampled = out.color_sampled;
  }
  return true;
}

}  // namespace engine::renderer
