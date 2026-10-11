#pragma once

// What engine-editor draws over its viewport, in Dear ImGui (ADR-0054: ImGui is for the tools that
// live inside the engine's window): the selection's box, the translate gizmo's handles, and the
// last edit the session refused. ImGui is fed the window's events and its draw lists are handed to
// the renderer's overlay pass (`renderer::OverlayDrawData`) — no ImGui backend is compiled, so
// Vulkan and SDL stay where the confinement rules keep them. It changes nothing itself: every edit
// is the document's (`EditorDocument::apply`, through `EditorInput`).
//
// **There are no panels here.** The outliner, the properties and the journal are the web shell's
// (apps/engine_shell), which the editor shows in an OS webview beside this viewport and which
// reaches the session through the protocol like any other client (roadmap A37; the three ImGui
// panels of A34 were a stopgap and are gone).

#include "editor_document.h"
#include "editor_input.h"
#include "editor_viewport.h"

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <foundation/window/window.h>
#include <systems/renderer/overlay.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/view_set.h>

#include <string>

struct ImGuiContext;

namespace engine::editor {

class EditorUi {
 public:
  EditorUi() = default;
  ~EditorUi() { destroy(); }
  ENGINE_NON_COPYABLE(EditorUi);

  bool create();
  void destroy() noexcept;

  // One window event into ImGui's queue.
  void feed(const window::Event& event);
  // Whether ImGui takes the pointer or the keyboard this frame: what the viewport does not get.
  // With nothing but the viewport's own drawing, ImGui takes neither; the editor still asks, so a
  // tool window added here later takes its events without a second path.
  bool wants_mouse() const noexcept;
  bool wants_keyboard() const noexcept;

  // Builds one frame of overlay over a `width` x `height` target: the selection's box and the
  // gizmo, `input`'s handles.
  void frame(f32 dt, u32 width, u32 height, const EditorDocument& document,
             const Viewport& viewport, const renderer::Camera& camera, const EditorInput& input);
  // ImGui's draw lists as the overlay's, after its textures are made, updated and retired through
  // the renderer (`SceneRenderer::overlay`). `out` points into this object until the next call.
  bool render(renderer::SceneRenderer& renderer, renderer::OverlayDrawData& out,
              std::string& error);
  // After the viewport was rebuilt: the renderer that held the textures is gone, so every texture
  // ImGui has is made again on the next `render`.
  void forget_textures() noexcept;

 private:
  ImGuiContext* context_ = nullptr;
  Vector<renderer::OverlayVertex> vertices_;
  Vector<u32> indices_;
  Vector<renderer::OverlayCommand> commands_;
};

}  // namespace engine::editor
