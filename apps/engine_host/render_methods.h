#pragma once

// engine-host's `render.*` methods (docs/subsystems/apps.md, protocol.md): the renderer over the
// protocol. `RenderHost` is what the methods' state lives in — the Vulkan device, created on the
// first call and shared by every later one, and the scenes a `render.load` left behind — and it
// reaches the handlers through `protocol::Context::app`, which is the registration point an app
// attaches its own methods through. The protocol layer knows nothing about any of it: it is two
// layers below `systems/renderer`.
//
// There is no window in this process. Every frame goes into an offscreen target the renderer
// owns, which is the whole point: the Phase 1 exit criterion is that *agents* can capture and
// benchmark, and an agent has no display.

#include <core/base/macros.h>
#include <core/containers/vector.h>
#include <domain/gfx/device.h>
#include <domain/protocol/rpc.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>

#include <memory>
#include <string>

namespace engine::host {

class RenderHost {
 public:
  RenderHost() noexcept = default;
  ~RenderHost();
  ENGINE_NON_COPYABLE(RenderHost);

  // One loaded scene. `data` is the expensive part — the import, the weld, the clustering, or
  // the container read — and it is kept across calls; `gpu` and `renderer` are sized by the
  // settings and the frame size and are rebuilt whenever either changes.
  struct Scene {
    std::string id;
    renderer::SceneData data;
    renderer::RenderSettings requested;
    renderer::ResolvedSettings resolved;
    std::unique_ptr<renderer::GpuScene> gpu;
    std::unique_ptr<renderer::SceneRenderer> view;
    renderer::RenderSettings built;  // what `gpu` and `view` were built with
    u32 width = 0;
    u32 height = 0;
  };

  // The device, created on first use. Null with `error` when there is none; the failure is
  // remembered, because retrying a driver that is not installed on every call is only slower.
  gfx::Device* device(u32 adapter, std::string& error);

  Scene* add_scene();
  Scene* find(std::string_view id) noexcept;
  usize count() const noexcept { return scenes_.size(); }

  // Rebuilds `scene.gpu` and `scene.view` when the settings or the frame size differ from what
  // they were built with, and leaves them alone when they do not. `unavailable` tells the two
  // kinds of failure apart: this machine cannot render these settings at all (protocol error
  // 1007, which a test skips on), against anything else, which is an internal error.
  bool ensure_renderer(Scene& scene, const renderer::RenderSettings& settings, u32 width,
                       u32 height, std::string& error, bool& unavailable);

 private:
  gfx::Device device_;
  Vector<std::unique_ptr<Scene>> scenes_;
  u32 next_id_ = 1;
  bool device_ready_ = false;
  bool device_failed_ = false;
  std::string device_error_;
};

// Registers render.load, render.capture, render.benchmark, and render.compare. The `RenderHost`
// they work on is the dispatcher's `Context::app`, which the caller sets and owns.
void add_render_methods(protocol::Dispatcher& dispatcher);

}  // namespace engine::host
