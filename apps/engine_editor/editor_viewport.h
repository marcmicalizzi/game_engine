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
  // instance's placement — the preview's while there is one) in world space; false past the
  // instances.
  bool instance_box(u32 instance, WorldPos corners[8]) const noexcept;

  // **A preview**: instance `instance` drawn as `placed` places it while the document still
  // says otherwise — what the gizmo shows while its button is held, so that a drag writes nothing
  // until it is released (one gesture, one command). It is `SceneRenderer::move_instances`, the
  // call `sync` makes for a transaction, which waits for nothing: the next frame draws it there
  // (until 2026-10-22 it waited for the device, a pointer event's cost). One instance at a time;
  // the box follows it. Never between `begin_frame` and `submit_frame`.
  bool preview(u32 instance, const renderer::SceneInstance& placed, std::string& error);
  // Puts the previewed instance back where the document has it: a cancelled or empty drag.
  bool cancel_preview(std::string& error);
  // Forgets the preview without moving anything, once the transaction that commits it is applied:
  // the next `sync` moves the instance to the record's translation, which is where it stands.
  void keep_preview() noexcept { preview_instance_ = ~0u; }
  bool previewing() const noexcept { return preview_instance_ != ~0u; }
  // Where instance `instance` is drawn: the preview's placement while there is one, else the
  // document's; null past the instances.
  const renderer::SceneInstance* placement(u32 instance) const noexcept;

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
  u32 preview_instance_ = ~0u;
  renderer::SceneInstance preview_placement_;
};

}  // namespace engine::editor
