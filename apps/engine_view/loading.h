#pragma once

// **The loading presentation** (apps.md, "The loading presentation"; roadmap W79): what a window
// shows while its scene loads, and the pieces that make it — none of which runs offscreen.
//
// - `LoadingOverlay`: engine-view's default, one line (the load's stage and its detail) and a bar
//   (its fraction), laid out by Dear ImGui into the overlay's triangle lists
//   (systems/renderer/overlay.h), like the HUD (game_hud.h) but needing no game.
// - `LoadingPresenter`: the frames before there is a renderer — the swapchain's image cleared and
//   an overlay drawn over it through an `OverlayPass` of its own, presented. Once the renderer
//   exists the same overlay goes into its frames (`FrameDesc::overlay`) instead.
// - `OverlayTextures`: where an overlay's textures go, so one overlay draws through either.
// - The plan (`load_progress_plan`, `save_progress_plan`): the stages a load expects, weighted by
//   the previous run's measured times, kept under the derived-data root.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/profiling/startup_trace.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/render_graph.h>
#include <domain/gfx/shader_library.h>
#include <systems/renderer/overlay.h>

#include <memory>
#include <span>
#include <string>

struct ImGuiContext;

namespace engine::gfx {
class Device;
class Swapchain;
}  // namespace engine::gfx

namespace engine::renderer {
class SceneRenderer;
}

namespace engine::view {

// Where an overlay's textures are made: the renderer's overlay pass, or the presenter's.
class OverlayTextures {
 public:
  virtual ~OverlayTextures() = default;
  // An RGBA8 texture; `OverlayPass::k_no_texture`, with `error`, when it could not be made.
  virtual u32 add_texture(u32 width, u32 height, const void* rgba, std::string* error) = 0;
  // No frame in flight may still read it (both implementations wait for the device).
  virtual void remove_texture(u32 texture) = 0;
};

// The renderer's overlay pass as an `OverlayTextures`.
class RendererTextures final : public OverlayTextures {
 public:
  explicit RendererTextures(renderer::SceneRenderer& renderer) noexcept : renderer_(renderer) {}
  u32 add_texture(u32 width, u32 height, const void* rgba, std::string* error) override;
  void remove_texture(u32 texture) override;

 private:
  renderer::SceneRenderer& renderer_;
};

// The default: a line and a bar, centred in the lower third of the picture.
class LoadingOverlay {
 public:
  LoadingOverlay() = default;
  ~LoadingOverlay();
  LoadingOverlay(const LoadingOverlay&) = delete;
  LoadingOverlay& operator=(const LoadingOverlay&) = delete;

  bool create();
  // Forgets its textures without removing them, for a target whose textures are already gone (the
  // presenter's, destroyed with it): the next `build` makes them again in whatever it is given.
  void forget_textures() noexcept;
  void destroy() noexcept;
  bool valid() const noexcept { return context_ != nullptr; }
  // The lists for `progress` over a `width` x `height` picture, their textures made in `textures`.
  // `out`'s spans point into this object and stay valid until the next build.
  bool build(const profiling::LoadProgress& progress, u32 width, u32 height, f32 display_scale,
             OverlayTextures& textures, renderer::OverlayDrawData& out, std::string& error);
  // The line the last build drew (the stage, its detail, the fraction in percent).
  const char* line() const noexcept { return line_; }
  u64 builds() const noexcept { return builds_; }

 private:
  ImGuiContext* context_ = nullptr;
  Vector<renderer::OverlayVertex> vertices_;
  Vector<u32> indices_;
  Vector<renderer::OverlayCommand> commands_;
  f32 font_px_ = 0.0f;
  char line_[224] = {};
  u64 builds_ = 0;
};

// Frames with no renderer: clear, an overlay, present.
class LoadingPresenter final : public OverlayTextures {
 public:
  LoadingPresenter() = default;
  ~LoadingPresenter() override;
  LoadingPresenter(const LoadingPresenter&) = delete;
  LoadingPresenter& operator=(const LoadingPresenter&) = delete;

  bool create(const gfx::Device& device, gfx::Swapchain& swapchain, std::string* error);
  // Waits for the device, then frees everything; the swapchain stays the caller's.
  void destroy() noexcept;
  bool valid() const noexcept { return device_ != nullptr; }
  // One frame: the swapchain's next image cleared and `overlay` (may be null) drawn over it. An
  // out-of-date or suboptimal chain is resized to the window's `width` x `height` and the frame
  // skipped. False only for a failure the session cannot continue past.
  bool present(const renderer::OverlayDrawData* overlay, u32 width, u32 height,
               std::string* error);
  u32 width() const noexcept;
  u32 height() const noexcept;
  u64 presented() const noexcept { return presented_; }

  u32 add_texture(u32 width, u32 height, const void* rgba, std::string* error) override;
  void remove_texture(u32 texture) override;

 private:
  const gfx::Device* device_ = nullptr;
  gfx::Swapchain* swapchain_ = nullptr;
  gfx::BindlessSet bindless_;
  gfx::ShaderLibrary shaders_;
  renderer::OverlayPass overlay_;
  gfx::FrameContext frames_;
  std::unique_ptr<gfx::RenderGraph> graph_;
  renderer::OverlayEncoding encoding_ = renderer::OverlayEncoding::Encoded;
  u64 presented_ = 0;
};

// The stages engine-view's load goes through, in order: the plan when no earlier run left one.
inline constexpr const char* k_load_stages[] = {
    "scene file",   "animation clips", "load scene",  "attach",  "ground prepare",
    "gpu scene",    "renderer",        "ground start", "world create", "session"};

// `<ddc>/startup/<key as 16 hex digits>.json`; empty when `ddc` is.
std::string progress_plan_path(const std::string& ddc, u64 key);
// The previous run's plan from `path` (its stages, weighted by their measured milliseconds), or
// `defaults` at equal weights when there is no file or it cannot be read (`why` says which); the
// ladder is `milestones` either way.
profiling::ProgressPlan load_progress_plan(const std::string& path,
                                           std::span<const char* const> defaults,
                                           std::span<const char* const> milestones,
                                           std::string* why);
// This run's measured plan (`profiling::progress_measured`), for the next run; best-effort.
bool save_progress_plan(const std::string& path, const profiling::ProgressPlan& measured,
                        std::string* error);

}  // namespace engine::view
