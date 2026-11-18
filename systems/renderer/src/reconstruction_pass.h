#pragma once

// **A registered reconstruction provider as the renderer's frame pass** (reconstruction.h;
// renderer.md, "Sampling over time: a reconstruction provider"; roadmap R159). The renderer makes
// one when `renderer.reconstruction.provider` names a provider other than its own accumulation:
// it creates the provider's state, registers this at `FramePassStage::Reconstruction`, and each
// frame hands the provider a `ReconstructionFrame` built from the frame pass's context and the
// motion pass's inputs, and the encode the image the provider returns.

#include <systems/renderer/frame_pass.h>
#include <systems/renderer/reconstruction.h>

#include <string>

namespace engine::renderer {

class MotionPass;

class ReconstructionPass {
 public:
  bool create(const ReconstructionProviderDesc& desc, const gfx::Device& device,
              gfx::BindlessSet& bindless, const MotionPass& motion, u32 width, u32 height,
              u32 frames_in_flight, std::string* error);
  void destroy() noexcept;
  bool resize(u32 width, u32 height, std::string* error);
  static bool record_thunk(const FramePassContext& context, void* user, std::string* error);

  const char* name() const noexcept { return desc_ != nullptr ? desc_->name : nullptr; }
  u64 device_bytes() const noexcept;

 private:
  const ReconstructionProviderDesc* desc_ = nullptr;
  void* state_ = nullptr;
  const MotionPass* motion_ = nullptr;
  bool drawn_ = false;      // the provider drew the frame before the last one it was handed
  u64 drawn_serial_ = 0;    // the motion pass's count when it last drew
};

}  // namespace engine::renderer
