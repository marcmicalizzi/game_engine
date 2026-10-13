#pragma once

// engine-editor's pointer and keys over the viewport (docs/subsystems/apps.md, "engine-editor"): a
// left click picks; the translate gizmo's handles hover and drag; Ctrl+Z undoes, Ctrl+Y (and
// Ctrl+Shift+Z) redoes, Delete deletes the selection, Escape cancels a drag. Events arrive with
// their coordinates already in picture pixels (the window's coordinates times `PointerScale`, so a
// click lands on its target under display scaling), after ImGui has seen them, with whether ImGui
// took the pointer or the keyboard.
//
// **One gesture, one command.** While the button is held a drag only previews: the instance moves
// in the viewport (`Viewport::preview`, which is `SceneRenderer::move_instances`, once a frame from
// `flush`) and the document is not written. Releasing applies **one** `doc.apply` holding one
// `cmd_set(id, "translation", ...)` through `EditorDocument::apply`, with a rationale naming the
// record and the axis; the viewport then follows the transaction through `Viewport::sync` exactly
// as it follows anyone's, so a drag and the same translation set over the protocol by somebody
// else end in the same state. Escape puts the preview back and writes nothing, and so does a
// release that moved nothing. Undo, redo and delete are one session step or one transaction each,
// and the journal shows every one.
//
// Nothing here draws: the panels draw the handles from `handles()`, `hovered()` and `active()`.

#include "editor_document.h"
#include "editor_viewport.h"
#include "viewport_math.h"

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/ids/id128.h>
#include <core/math/world.h>
#include <foundation/window/window.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/view_set.h>

#include <string>
#include <string_view>

namespace engine::editor {

// **The tool the gizmo is** (A39): T moves along the handles, R turns about the rings, while the
// viewport has the keyboard; the shell's title bar sets it through `editor.tool` too.
enum class Tool : u8 { Translate, Rotate };
const char* tool_name(Tool tool) noexcept;  // "translate", "rotate"
bool tool_from_name(std::string_view name, Tool& out) noexcept;

class EditorInput {
 public:
  EditorInput() = default;
  ENGINE_NON_COPYABLE(EditorInput);

  // The view the frame is drawn with, and the selection's handles in it: called once a frame,
  // after the viewport has followed the document and before the panels draw. The next frame's
  // events are hit-tested against these handles, which are the ones on the screen. `scale` is the
  // pointer scale's vertical ratio: handles are `k_handle_pixels` long in window coordinates.
  void set_view(const EditorDocument& document, Viewport& viewport, const renderer::Camera& camera,
                u32 width, u32 height, f32 scale);

  // One event, its coordinates in picture pixels. False only when the viewport fails (a pick or a
  // preview the device refused), with `error`; an edit the session refuses is not a failure of the
  // editor: it is kept in `message()` for the toolbar and the run goes on.
  bool handle(const window::Event& event, bool over_panels, bool keys_free,
              EditorDocument& document, Viewport& viewport, std::string& error);
  // Moves a drag's preview to where this frame's events left the pointer: once a frame, after the
  // events and before the frame is drawn, because a preview is a `move_instances` that waits for
  // the device and a mouse can send a thousand moves a second.
  bool flush(Viewport& viewport, std::string& error);

  // What the toolbar's buttons and the keys do; each cancels a drag in progress first.
  bool undo(EditorDocument& document, Viewport& viewport, std::string& error);
  bool redo(EditorDocument& document, Viewport& viewport, std::string& error);
  // Deletes the selected record through one `doc.apply` with `cmd_delete` and selects nothing.
  bool remove_selection(EditorDocument& document, Viewport& viewport, std::string& error);
  // Selects the record under picture pixel (`px`, `py`), timing the read (`stats().pick_ms`).
  bool pick(f32 px, f32 py, EditorDocument& document, Viewport& viewport, std::string& error);
  // Puts a drag's preview back and forgets the drag.
  bool cancel_drag(Viewport& viewport, std::string& error);

  // The gizmo's tool; a change cancels a drag in progress (its preview goes back).
  Tool tool() const noexcept { return tool_; }
  bool set_tool(Tool tool, Viewport& viewport, std::string& error);

  // The translate tool's handles, or the rotate tool's rings (the other is invisible).
  const GizmoHandles& handles() const noexcept { return handles_; }
  const GizmoRings& rings() const noexcept { return rings_; }
  const Mat4& view_proj() const noexcept { return view_proj_; }
  u32 hovered() const noexcept { return hovered_; }
  u32 active() const noexcept {
    return !dragging_ ? k_no_axis : (rotating_ ? ring_drag_.axis : drag_.axis);
  }
  bool dragging() const noexcept { return dragging_; }
  bool rotating() const noexcept { return dragging_ && rotating_; }
  // The last edit the session refused, or empty.
  const std::string& message() const noexcept { return message_; }

  // What the run's summary reports.
  struct Stats {
    u32 picks = 0;
    f64 pick_ms = -1.0;  // the last pick's read of the visibility buffer; negative before any
    u32 drags = 0;       // gestures that committed a transaction
    u32 last_axis = k_no_axis;
    f64 last_delta = 0.0;
    i64 last_transaction = -1;  // its index in the journal
    std::string last_rationale;
    u32 undos = 0;
    u32 redos = 0;
    u32 deletes = 0;
    u32 rotations = 0;  // turns that committed a transaction
    u32 last_rotate_axis = k_no_axis;
    f64 last_degrees = 0.0;
    i64 last_rotate_transaction = -1;
    std::string last_rotate_rationale;
  };
  const Stats& stats() const noexcept { return stats_; }

 private:
  bool begin_drag(f32 px, f32 py, const EditorDocument& document, Viewport& viewport,
                  std::string& error);
  bool begin_turn(f32 px, f32 py, const EditorDocument& document, Viewport& viewport);
  bool end_turn(f32 px, f32 py, EditorDocument& document, Viewport& viewport, std::string& error);
  bool move_drag(f32 px, f32 py);
  bool end_drag(f32 px, f32 py, EditorDocument& document, Viewport& viewport, std::string& error);

  // The frame's view, from `set_view`.
  renderer::Camera camera_;
  Mat4 view_proj_;
  u32 width_ = 0;
  u32 height_ = 0;
  f32 scale_ = 1.0f;
  u32 instance_ = ~0u;  // the selection's instance, ~0 when it is none
  WorldPos centre_{};   // its box's centre, where the handles stand
  GizmoHandles handles_;
  GizmoRings rings_;
  WorldPos pivot_{};  // the placement's origin, where the rings stand and a turn turns about
  u32 hovered_ = k_no_axis;
  Tool tool_ = Tool::Translate;

  bool dragging_ = false;
  bool rotating_ = false;  // the drag is a turn (RingDrag) rather than a move (AxisDrag)
  RingDrag ring_drag_;
  bool preview_snap_ = false;   // whether the preview was last drawn snapped
  bool preview_dirty_ = false;  // the drag moved since the preview was last drawn
  AxisDrag drag_;
  Id128 drag_record_;
  u32 drag_instance_ = ~0u;
  renderer::SceneInstance drag_start_;  // the document's placement when the drag began

  bool control_ = false;
  bool shift_ = false;
  std::string message_;
  Stats stats_;
};

}  // namespace engine::editor
