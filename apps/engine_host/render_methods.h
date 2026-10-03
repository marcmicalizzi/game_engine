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
#include <systems/renderer/reference.h>
#include <systems/renderer/request.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>

#include <memory>
#include <span>
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
    // What it was loaded from, as `render.scenes` reports it: "mesh", "scene" or "procedural",
    // and the path or the procedural scene's name.
    std::string kind;
    std::string source;
    renderer::SceneData data;
    // What `render.load` was asked, in the renderer's one description of a request (request.h),
    // and the settings it came to against the loaded scene: a call with no `settings` of its own
    // draws with these.
    renderer::RenderRequest request;
    renderer::RenderSettings requested;
    renderer::ResolvedSettings resolved;
    std::unique_ptr<renderer::GpuScene> gpu;
    std::unique_ptr<renderer::SceneRenderer> view;
    // The terrain levels a call moves — the time-lapse, the rings, the world's tiles — built with
    // the GPU scene, which reserves their slots, round the call's first camera. A scene that has
    // them is rebuilt for every call that draws, so a call's picture is a function of the call and
    // not of how far the calls before it moved the sand.
    std::unique_ptr<renderer::MovingGround> ground;
    // The reference path tracer over the same scene and the same renderer, created on the first
    // call that asks for one and kept afterwards: it owns a shader library, two pipelines and two
    // screen-sized buffers, and an optimization loop calls it once per proposal.
    std::unique_ptr<renderer::ReferenceRenderer> reference;
    renderer::RenderSettings built;  // what `gpu` and `view` were built with; the frame size is
                                     // the renderer's own (`view->width()`), because a resize
                                     // touches only the screen-sized targets
  };

  // The device, created on first use. Null with `error` when there is none; the failure is
  // remembered, because retrying a driver that is not installed on every call is only slower.
  gfx::Device* device(u32 adapter, std::string& error);

  Scene* add_scene();
  Scene* find(std::string_view id) noexcept;
  // Takes a scene out of the host, which then no longer knows the id; null when it holds none by
  // that name. `render.unload` releases what it returns (`release`), and the scene's memory goes
  // with the pointer.
  std::unique_ptr<Scene> take(std::string_view id) noexcept;
  // Whether `id` is one this host handed out and has since unloaded. Ids are never reused, so an
  // id that parses as one below the next and is not held was unloaded — which is worth a sentence
  // of its own in the NotFound a later call gets, since the id did exist.
  bool unloaded(std::string_view id) const noexcept;
  // Releases a scene's GPU state through the renderer's own teardown, in the order the objects
  // hold each other: the reference path tracer holds the renderer and the GPU scene, the renderer
  // holds the GPU scene. Each destructor waits for the device to finish with what it owns. The
  // host arrays (`data`) stay until the `Scene` itself goes.
  static void release(Scene& scene) noexcept;
  usize count() const noexcept { return scenes_.size(); }
  // Every loaded scene, in load order: what `engine.budgets` reports a scene's budgets from.
  std::span<const std::unique_ptr<Scene>> scenes() const noexcept {
    return {scenes_.data(), scenes_.size()};
  }
  // The device a render.* call opened, or null when none has. Opens nothing: a question about the
  // device must not be the thing that creates one.
  const gfx::Device* open_device() const noexcept { return device_ready_ ? &device_ : nullptr; }

  // Rebuilds `scene.gpu` and `scene.view` when the settings or the frame size differ from what
  // they were built with, and leaves them alone when they do not — except for a scene whose ground
  // is drawn as terrain levels, which is rebuilt every time, its rings or tiles laid out round
  // `first` (the call's first camera) and its motion started from the scene's own time.
  // `unavailable` tells the two kinds of failure apart: this machine cannot render these settings
  // at all (protocol error 1007, which a test skips on), against anything else, which is an
  // internal error.
  bool ensure_renderer(Scene& scene, const renderer::RenderSettings& settings, u32 width,
                       u32 height, const renderer::Camera& first, std::string& error,
                       bool& unavailable);

  // The same for the reference renderer, which has to come after `ensure_renderer` because it is
  // built against the `SceneRenderer` that call left in place and reads its extent. `unavailable`
  // covers both kinds of "this machine cannot": no cluster acceleration structures, and settings
  // that build none.
  bool ensure_reference(Scene& scene, std::string& error, bool& unavailable);

 private:
  gfx::Device device_;
  Vector<std::unique_ptr<Scene>> scenes_;
  u32 next_id_ = 1;
  bool device_ready_ = false;
  bool device_failed_ = false;
  std::string device_error_;
};

// Registers render.load, render.capture, render.benchmark, render.compare, render.evaluate,
// render.unload and render.scenes. The `RenderHost` they work on is the `render` member of the
// `HostState` the dispatcher's `Context::app` points at (host_state.h), which the caller sets and
// owns.
void add_render_methods(protocol::Dispatcher& dispatcher);

}  // namespace engine::host
