#include "editor_input.h"

#include <core/json/json_value.h>
#include <core/time/time.h>
#include <domain/doc/document.h>

#include <span>
#include <utility>

namespace engine::editor {

void EditorInput::set_view(const EditorDocument& document, Viewport& viewport,
                           const renderer::Camera& camera, u32 width, u32 height, f32 scale) {
  camera_ = camera;
  width_ = width;
  height_ = height;
  scale_ = scale > 0.0f ? scale : 1.0f;
  handles_ = GizmoHandles{};
  instance_ = ~0u;
  if (!viewport.valid()) {
    hovered_ = k_no_axis;
    return;
  }
  const renderer::ViewSet& views = viewport.renderer().update_views(camera);
  if (views.size() == 0) {
    hovered_ = k_no_axis;
    return;
  }
  view_proj_ = views[0].view_proj;
  const u32 instance = viewport.scene().instance_of(document.selection());
  WorldPos corners[8];
  if (instance == ~0u || !viewport.instance_box(instance, corners)) {
    hovered_ = k_no_axis;
    return;
  }
  // The box's centre, in f64: the handles stand where the selection box's middle is drawn.
  DVec3 sum{0.0, 0.0, 0.0};
  for (const WorldPos& c : corners)
    sum = sum + (c - corners[0]);
  instance_ = instance;
  centre_ = corners[0] + sum / 8.0;
  handles_ = gizmo_handles(view_proj_, relative(centre_, camera.position), camera.fov_y, width,
                           height, k_handle_pixels * scale_);
  if (!handles_.visible) hovered_ = k_no_axis;
}

bool EditorInput::handle(const window::Event& e, bool over_panels, bool keys_free,
                         EditorDocument& document, Viewport& viewport, std::string& error) {
  using window::EventKind;
  using window::Key;
  switch (e.kind) {
    case EventKind::MouseMove:
      if (dragging_) return move_drag(e.x, e.y);
      hovered_ = over_panels ? k_no_axis : hover_axis(handles_, e.x, e.y, k_hover_pixels * scale_);
      return true;
    case EventKind::MouseButtonDown: {
      if (e.button != 1 || over_panels || dragging_) return true;
      // A press on a handle is the start of a drag, not a pick: grabbing the gizmo keeps the
      // selection it belongs to.
      const u32 axis = hover_axis(handles_, e.x, e.y, k_hover_pixels * scale_);
      if (axis != k_no_axis) {
        hovered_ = axis;
        return begin_drag(e.x, e.y, document, viewport, error);
      }
      return pick(e.x, e.y, document, viewport, error);
    }
    case EventKind::MouseButtonUp:
      if (e.button == 1 && dragging_) return end_drag(e.x, e.y, document, viewport, error);
      return true;
    case EventKind::KeyDown:
    case EventKind::KeyUp: {
      const bool down = e.kind == EventKind::KeyDown;
      if (e.key == Key::Control) control_ = down;
      if (e.key == Key::Shift) shift_ = down;
      if (!down || !keys_free) return true;
      if (e.key == Key::Escape && dragging_) return cancel_drag(viewport, error);
      if (control_ && e.key == Key::Z) {
        return shift_ ? redo(document, viewport, error) : undo(document, viewport, error);
      }
      if (control_ && e.key == Key::Y) return redo(document, viewport, error);
      if (e.key == Key::Delete && !e.repeat) return remove_selection(document, viewport, error);
      return true;
    }
    case EventKind::FocusLost:
      // The release may go to another window: a drag the editor cannot see end is cancelled.
      control_ = false;
      shift_ = false;
      return dragging_ ? cancel_drag(viewport, error) : true;
    default: return true;
  }
}

bool EditorInput::begin_drag(f32 px, f32 py, const EditorDocument& document, Viewport& viewport,
                             std::string& error) {
  (void)error;
  if (instance_ == ~0u || hovered_ == k_no_axis) return true;
  Ray ray;
  if (!pixel_ray(view_proj_, px, py, width_, height_, ray)) return true;
  AxisDrag drag;
  // An axis pointing at the eye cannot be dragged along: the press does nothing.
  if (!begin_axis_drag(hovered_, centre_, camera_.position, ray, drag)) return true;
  const renderer::SceneInstance* placed = viewport.placement(instance_);
  if (placed == nullptr) return true;
  drag_ = drag;
  drag_start_ = *placed;
  drag_instance_ = instance_;
  drag_record_ = document.selection();
  dragging_ = true;
  message_.clear();
  return true;
}

bool EditorInput::move_drag(f32 px, f32 py) {
  Ray ray;
  if (!pixel_ray(view_proj_, px, py, width_, height_, ray)) return true;
  if (update_axis_drag(drag_, camera_.position, ray)) preview_dirty_ = true;
  return true;
}

bool EditorInput::flush(Viewport& viewport, std::string& error) {
  if (!dragging_ || !preview_dirty_) return true;
  preview_dirty_ = false;
  renderer::SceneInstance placed = drag_start_;
  placed.origin = dragged_translation(drag_start_.origin, drag_);
  return viewport.preview(drag_instance_, placed, error);
}

bool EditorInput::end_drag(f32 px, f32 py, EditorDocument& document, Viewport& viewport,
                           std::string& error) {
  Ray ray;
  if (pixel_ray(view_proj_, px, py, width_, height_, ray))
    update_axis_drag(drag_, camera_.position, ray);
  dragging_ = false;
  preview_dirty_ = false;
  const AxisDrag drag = drag_;
  drag_ = AxisDrag{};
  // Nothing moved, or the record went while the button was held: nothing to write.
  if (drag.delta == 0.0f || !document.document().exists(drag_record_))
    return viewport.cancel_preview(error);
  const WorldPos to = dragged_translation(drag_start_.origin, drag);
  JsonValue value = JsonValue::array();
  value.push_back(JsonValue(to.x));
  value.push_back(JsonValue(to.y));
  value.push_back(JsonValue(to.z));
  const doc::Command set = doc::cmd_set(drag_record_, "translation", std::move(value));
  const std::string rationale =
      "move " + document.display_name(drag_record_) + " along " + axis_name(drag.axis);
  std::string refused;
  if (!document.apply(std::span<const doc::Command>(&set, 1), rationale, refused)) {
    message_ = refused;
    return viewport.cancel_preview(error);
  }
  // Committed: the next `sync` moves the instance to the record's translation, which is where the
  // drag left it, so the preview is simply forgotten.
  viewport.keep_preview();
  ++stats_.drags;
  stats_.last_axis = drag.axis;
  stats_.last_delta = static_cast<f64>(drag.delta);
  stats_.last_transaction = static_cast<i64>(document.document().journal().size()) - 1;
  stats_.last_rationale = rationale;
  return true;
}

bool EditorInput::cancel_drag(Viewport& viewport, std::string& error) {
  if (!dragging_) return true;
  dragging_ = false;
  preview_dirty_ = false;
  drag_ = AxisDrag{};
  return viewport.cancel_preview(error);
}

bool EditorInput::undo(EditorDocument& document, Viewport& viewport, std::string& error) {
  if (!cancel_drag(viewport, error)) return false;
  if (!document.can_undo()) return true;
  std::string refused;
  if (!document.undo(refused)) {
    message_ = refused;
    return true;
  }
  message_.clear();
  ++stats_.undos;
  return true;
}

bool EditorInput::redo(EditorDocument& document, Viewport& viewport, std::string& error) {
  if (!cancel_drag(viewport, error)) return false;
  if (!document.can_redo()) return true;
  std::string refused;
  if (!document.redo(refused)) {
    message_ = refused;
    return true;
  }
  message_.clear();
  ++stats_.redos;
  return true;
}

bool EditorInput::remove_selection(EditorDocument& document, Viewport& viewport,
                                   std::string& error) {
  if (!cancel_drag(viewport, error)) return false;
  const Id128 id = document.selection();
  if (id.is_null() || !document.document().exists(id)) return true;
  const doc::Command remove = doc::cmd_delete(id);
  std::string refused;
  if (!document.apply(std::span<const doc::Command>(&remove, 1),
                      "delete " + document.display_name(id), refused)) {
    message_ = refused;
    return true;
  }
  message_.clear();
  document.select(Id128{});
  hovered_ = k_no_axis;
  ++stats_.deletes;
  return true;
}

bool EditorInput::pick(f32 px, f32 py, EditorDocument& document, Viewport& viewport,
                       std::string& error) {
  if (!(px >= 0.0f) || !(py >= 0.0f)) {
    document.select(Id128{});
    return true;
  }
  // The whole visibility buffer is read back for one pixel: a click's cost, and the summary's
  // `pick_ms` says what it is, so nobody mistakes it for something a frame could afford.
  const i64 start = time::monotonic_ns();
  Id128 picked;
  if (!viewport.pick(static_cast<u32>(px), static_cast<u32>(py), picked, error)) return false;
  stats_.pick_ms = static_cast<f64>(time::monotonic_ns() - start) / 1.0e6;
  ++stats_.picks;
  document.select(picked);
  hovered_ = k_no_axis;
  return true;
}

}  // namespace engine::editor
