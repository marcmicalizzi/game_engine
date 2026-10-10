#pragma once

// engine-editor's viewport (docs/subsystems/apps.md, "engine-editor"): the document's placements as
// a renderer scene (`doc_scene`), drawn by `renderer::SceneRenderer` exactly as engine-view draws a
// scene, into a swapchain image or offscreen; a transaction's changes applied to it; a pixel turned
// back into the record under it. Every pass it needs is the renderer's — the overlay the UI draws
// through, `move_instances`, `read_last_frame` — so engine-host can grow the same viewport over the
// protocol without a line of this file (AGENTS.md, "Rendering code lives in systems/renderer").

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/doc/document.h>
#include <domain/gfx/device.h>
#include <domain/gfx/display.h>
#include <domain/gfx/rhi.h>
#include <systems/doc_scene/doc_scene.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/view_set.h>

#include <memory>
#include <string>

namespace engine::editor {

class Viewport {
 public:
  struct Options {
    u32 width = 1280;
    u32 height = 720;
    gfx::Format color_format = gfx::Format::R8G8B8A8Unorm;
    bool offscreen = true;  // the renderer owns its target; false draws into the caller's image
    renderer::RenderSettings settings;
    std::string ddc;  // derived-data root; empty finds the repository's
  };

  Viewport() = default;
  ~Viewport() { destroy(); }
  ENGINE_NON_COPYABLE(Viewport);

  // Reads the document's placements and loads them: meshes through the derived-data cache, the GPU
  // scene, a renderer. `base_dir` is the document's directory, which mesh paths are relative to.
  bool build(const gfx::Device& device, const doc::Document& document, const std::string& base_dir,
             const Options& options, std::string& error);
  void destroy() noexcept;
  bool valid() const noexcept { return renderer_ != nullptr; }

  // Brings the viewport up to the document after transactions: the placements whose transform
  // changed are moved in place (`SceneRenderer::move_instances`); a placement created, deleted or
  // given another mesh rebuilds the scene, and `rebuilt` says so (the renderer, and with it the
  // overlay's textures, is new). Never between `begin_frame` and `submit_frame`.
  bool sync(const doc::Document& document, bool& rebuilt, std::string& error);

  renderer::SceneRenderer& renderer() noexcept { return *renderer_; }
  const doc_scene::DocumentScene& scene() const noexcept { return scene_; }
  const renderer::SceneData& data() const noexcept { return data_; }

  // A camera that frames the whole scene (the renderer's orbit at the scene's bounds).
  renderer::Camera framing_camera() const noexcept;

  // The record under pixel (`x`, `y`) of the frame last drawn: the null id over nothing, over the
  // stand-in ground, or outside the picture. Reads the visibility buffer
  // (`SceneRenderer::read_last_frame`), so it waits for the device: a click's cost, not a frame's.
  bool pick(u32 x, u32 y, Id128& out, std::string& error);

  // The eight corners of instance `instance`'s box (its mesh's, through the mesh's fit and the
  // instance's placement) in world space; false past the instances.
  bool instance_box(u32 instance, WorldPos corners[8]) const noexcept;

 private:
  bool load(std::string& error);

  const gfx::Device* device_ = nullptr;
  Options options_;
  std::string base_dir_;
  doc_scene::DocumentScene scene_;
  renderer::SceneData data_;
  renderer::ResolvedSettings resolved_;
  std::unique_ptr<renderer::GpuScene> gpu_;
  std::unique_ptr<renderer::SceneRenderer> renderer_;
  Vector<Vec3> mesh_lo_;  // each mesh's box, in its own space before the fit
  Vector<Vec3> mesh_hi_;
};

}  // namespace engine::editor
