#include "editor_input.h"

#include <core/json/json_value.h>
#include <core/time/time.h>
#include <domain/doc/document.h>

#include <cmath>
#include <cstdio>
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
  rings_ = GizmoRings{};
  if (tool_ == Tool::Translate) {
    handles_ = gizmo_handles(view_proj_, relative(centre_, camera.position), camera.fov_y, width,
                             height, k_handle_pixels * scale_);
    if (!handles_.visible) hovered_ = k_no_axis;
    return;
  }
  // The rings stand where a turn turns about: the placement's origin, not the box's centre.
  const renderer::SceneInstance* placed = viewport.placement(instance);
  pivot_ = placed != nullptr ? placed->origin + DVec3{placed->transform.position} : centre_;
  rings_ = gizmo_rings(view_proj_, relative(pivot_, camera.position), camera.fov_y, width, height,
                       k_ring_pixels * scale_);
  if (!rings_.visible) hovered_ = k_no_axis;
}

const char* tool_name(Tool tool) noexcept { return tool == Tool::Rotate ? "rotate" : "translate"; }

bool tool_from_name(std::string_view name, Tool& out) noexcept {
  if (name == "translate") {
    out = Tool::Translate;
    return true;
  }
  if (name == "rotate") {
    out = Tool::Rotate;
    return true;
  }
  return false;
}

bool EditorInput::set_tool(Tool tool, Viewport& viewport, std::string& error) {
  if (tool == tool_) return true;
  if (!cancel_drag(viewport, error)) return false;
  tool_ = tool;
  hovered_ = k_no_axis;
  return true;
}

bool EditorInput::handle(const window::Event& e, bool over_panels, bool keys_free,
                         EditorDocument& document, Viewport& viewport, std::string& error) {
  using window::EventKind;
  using window::Key;
  switch (e.kind) {
    case EventKind::MouseMove: {
      if (dragging_) return move_drag(e.x, e.y);
      const f32 reach = k_hover_pixels * scale_;
      hovered_ = over_panels                ? k_no_axis
                 : tool_ == Tool::Translate ? hover_axis(handles_, e.x, e.y, reach)
                                            : hover_ring(rings_, e.x, e.y, reach);
      return true;
    }
    case EventKind::MouseButtonDown: {
      if (e.button != 1 || over_panels || dragging_) return true;
      // A press on a handle or a ring is the start of a drag, not a pick: grabbing the gizmo
      // keeps the selection it belongs to.
      const f32 reach = k_hover_pixels * scale_;
      const u32 axis = tool_ == Tool::Translate ? hover_axis(handles_, e.x, e.y, reach)
                                                : hover_ring(rings_, e.x, e.y, reach);
      if (axis != k_no_axis) {
        hovered_ = axis;
        return tool_ == Tool::Translate ? begin_drag(e.x, e.y, document, viewport, error)
                                        : begin_turn(e.x, e.y, document, viewport);
      }
      return pick(e.x, e.y, document, viewport, error);
    }
    case EventKind::MouseButtonUp:
      if (e.button == 1 && dragging_) {
        return rotating_ ? end_turn(e.x, e.y, document, viewport, error)
                         : end_drag(e.x, e.y, document, viewport, error);
      }
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
      // The gizmo's tool; W/A/S/D/E/Q are the camera's.
      if (!control_ && e.key == Key::T) return set_tool(Tool::Translate, viewport, error);
      if (!control_ && e.key == Key::R) return set_tool(Tool::Rotate, viewport, error);
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
  const bool moved = rotating_ ? update_ring_drag(ring_drag_, camera_.position, ray)
                               : update_axis_drag(drag_, camera_.position, ray);
  if (moved) preview_dirty_ = true;
  return true;
}

bool EditorInput::flush(Viewport& viewport, std::string& error) {
  // Shift snaps a turn, so a change of Shift alone moves the preview too.
  if (dragging_ && rotating_ && shift_ != preview_snap_) preview_dirty_ = true;
  if (!dragging_ || !preview_dirty_) return true;
  preview_dirty_ = false;
  renderer::SceneInstance placed = drag_start_;
  if (rotating_) {
    // The placement's rotation turned about the world axis; the commit computes the record's own
    // `rotation` from the document (`turned_rotation`), which lands on the same orientation.
    preview_snap_ = shift_;
    const Quat turn = axis_quat(ring_drag_.axis, ring_drag_degrees(ring_drag_, shift_));
    placed.transform.rotation = normalize(turn * drag_start_.transform.rotation);
  } else {
    placed.origin = dragged_translation(drag_start_.origin, drag_);
  }
  return viewport.preview(drag_instance_, placed, error);
}

bool EditorInput::begin_turn(f32 px, f32 py, const EditorDocument& document, Viewport& viewport) {
  if (instance_ == ~0u || hovered_ == k_no_axis) return true;
  Ray ray;
  if (!pixel_ray(view_proj_, px, py, width_, height_, ray)) return true;
  RingDrag drag;
  // A ring seen edge-on cannot be turned by the pointer: the press does nothing.
  if (!begin_ring_drag(hovered_, pivot_, camera_.position, ray, drag)) return true;
  const renderer::SceneInstance* placed = viewport.placement(instance_);
  if (placed == nullptr) return true;
  ring_drag_ = drag;
  drag_start_ = *placed;
  drag_instance_ = instance_;
  drag_record_ = document.selection();
  dragging_ = true;
  rotating_ = true;
  preview_snap_ = shift_;
  message_.clear();
  return true;
}

namespace {

// A turn's degrees as a rationale says them: whole when they are, else to a tenth.
std::string degrees_text(f64 degrees) {
  char text[32];
  const f64 whole = std::round(degrees);
  if (std::fabs(degrees - whole) < 0.05)
    std::snprintf(text, sizeof(text), "%.0f", whole + 0.0);
  else
    std::snprintf(text, sizeof(text), "%.1f", degrees);
  return std::string(text) + "\xC2\xB0";
}

// A record's `rotation` and `yaw_deg` as the document holds them (doc_scene.md): identity and
// zero where absent.
void record_rotation(const doc::Document& d, Id128 id, Quat& rotation, f32& yaw_deg) {
  rotation = Quat::identity();
  yaw_deg = 0.0f;
  const JsonValue* q = d.property(id, "rotation");
  if (q != nullptr && q->is_array() && q->size() == 4) {
    f64 c[4] = {0.0, 0.0, 0.0, 1.0};
    bool ok = true;
    for (u32 i = 0; i < 4; ++i)
      ok = (*q)[i].get_f64(c[i]) && ok;
    if (ok) {
      rotation = Quat{static_cast<f32>(c[0]), static_cast<f32>(c[1]), static_cast<f32>(c[2]),
                      static_cast<f32>(c[3])};
    }
  }
  const JsonValue* y = d.property(id, "yaw_deg");
  f64 yaw = 0.0;
  if (y != nullptr && y->get_f64(yaw)) yaw_deg = static_cast<f32>(yaw);
}

}  // namespace

bool EditorInput::end_turn(f32 px, f32 py, EditorDocument& document, Viewport& viewport,
                           std::string& error) {
  Ray ray;
  if (pixel_ray(view_proj_, px, py, width_, height_, ray))
    update_ring_drag(ring_drag_, camera_.position, ray);
  dragging_ = false;
  rotating_ = false;
  preview_dirty_ = false;
  const RingDrag drag = ring_drag_;
  ring_drag_ = RingDrag{};
  const f64 degrees = ring_drag_degrees(drag, shift_);
  // Nothing turned (or a snap to zero), or the record went while the button was held.
  if (degrees == 0.0 || !document.document().exists(drag_record_))
    return viewport.cancel_preview(error);
  Quat rotation;
  f32 yaw_deg = 0.0f;
  record_rotation(document.document(), drag_record_, rotation, yaw_deg);
  const Quat to = turned_rotation(rotation, yaw_deg, drag.axis, degrees);
  JsonValue value = JsonValue::array();
  value.push_back(JsonValue(static_cast<f64>(to.x)));
  value.push_back(JsonValue(static_cast<f64>(to.y)));
  value.push_back(JsonValue(static_cast<f64>(to.z)));
  value.push_back(JsonValue(static_cast<f64>(to.w)));
  const doc::Command set = doc::cmd_set(drag_record_, "rotation", std::move(value));
  const std::string rationale = "rotate " + document.display_name(drag_record_) + " about " +
                                axis_name(drag.axis) + " by " + degrees_text(degrees);
  std::string refused;
  if (!document.apply(std::span<const doc::Command>(&set, 1), rationale, refused)) {
    message_ = refused;
    return viewport.cancel_preview(error);
  }
  viewport.keep_preview();
  ++stats_.rotations;
  stats_.last_rotate_axis = drag.axis;
  stats_.last_degrees = degrees;
  stats_.last_rotate_transaction = static_cast<i64>(document.document().journal().size()) - 1;
  stats_.last_rotate_rationale = rationale;
  return true;
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
  rotating_ = false;
  preview_dirty_ = false;
  drag_ = AxisDrag{};
  ring_drag_ = RingDrag{};
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
