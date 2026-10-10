#pragma once

// engine-editor's panels, in Dear ImGui (ADR-0054): the outliner, the property panel and the
// journal, docked round the viewport, and the selection's box drawn over it. ImGui is fed the
// window's events and its draw lists are handed to the renderer's overlay pass
// (`renderer::OverlayDrawData`) — no ImGui backend is compiled, so Vulkan and SDL stay where the
// confinement rules keep them. The panels draw from `EditorDocument`'s models and change nothing
// but the selection; every edit is the document's (`EditorDocument::apply`).

#include "editor_document.h"
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
  bool wants_mouse() const noexcept;
  bool wants_keyboard() const noexcept;

  // What a frame of panels asked for.
  struct Actions {
    bool select = false;  // the outliner chose `selected`
    Id128 selected;
  };
  // Builds one frame of panels over a `width` x `height` target.
  void frame(f32 dt, u32 width, u32 height, const EditorDocument& document,
             const Viewport& viewport, const renderer::Camera& camera, Actions& out);
  // ImGui's draw lists as the overlay's, after its textures are made, updated and retired through
  // the renderer (`SceneRenderer::overlay`). `out` points into this object until the next call.
  bool render(renderer::SceneRenderer& renderer, renderer::OverlayDrawData& out,
              std::string& error);
  // After the viewport was rebuilt: the renderer that held the textures is gone, so every texture
  // ImGui has is made again on the next `render`.
  void forget_textures() noexcept;

  // The panels' row counts in the last frame (the summary, the tests).
  u32 outliner_rows() const noexcept { return static_cast<u32>(outliner_.size()); }
  u32 property_rows() const noexcept { return static_cast<u32>(properties_.size()); }
  u32 journal_rows() const noexcept { return static_cast<u32>(journal_.size()); }

 private:
  void refresh(const EditorDocument& document);

  ImGuiContext* context_ = nullptr;
  bool laid_out_ = false;
  u64 revision_ = ~u64{0};
  Id128 shown_;
  u32 journal_size_ = ~0u;
  Vector<OutlinerRow> outliner_;
  Vector<PropertyRow> properties_;
  Vector<JournalRow> journal_;
  Vector<renderer::OverlayVertex> vertices_;
  Vector<u32> indices_;
  Vector<renderer::OverlayCommand> commands_;
};

}  // namespace engine::editor
