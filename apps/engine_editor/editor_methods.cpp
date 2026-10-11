#include "editor_methods.h"

#include <core/math/math.h>

#include <cmath>
#include <limits>

namespace engine::editor {

namespace {

EditorState& state_of(protocol::Context& ctx) { return *static_cast<EditorState*>(ctx.app); }

bool finite(f64 v) noexcept { return std::isfinite(v); }

void fill_state(const EditorState& state, protocol::EditorStateResult& out) {
  out.session = state.document != nullptr && state.document->valid()
                    ? state.document->session().id()
                    : std::string();
  const Id128 selected = state.document != nullptr ? state.document->selection() : Id128{};
  out.selected = selected.is_null() ? std::optional<Id128>() : std::optional<Id128>(selected);
  out.camera = state.camera;
  out.layout = state.layout;
  out.layouts = state.layouts;
  out.width = state.width;
  out.height = state.height;
}

bool editor_layout(protocol::Context& ctx, const protocol::EditorLayoutParams& p,
                   protocol::EditorLayoutResult& r, protocol::RpcError& error) {
  // A page's layout is external input: validated, never asserted on.
  if (!finite(p.x) || !finite(p.y) || !finite(p.width) || !finite(p.height) ||
      !finite(p.device_pixel_ratio) || p.width < 0.0 || p.height < 0.0 ||
      p.device_pixel_ratio <= 0.0 || p.width > 1.0e6 || p.height > 1.0e6 ||
      std::fabs(p.x) > 1.0e6 || std::fabs(p.y) > 1.0e6 || p.device_pixel_ratio > 64.0) {
    error = protocol::make_error(protocol::codes::k_invalid_params,
                                 "editor.layout: a rectangle of finite CSS pixels and a positive "
                                 "device pixel ratio");
    return false;
  }
  EditorState& state = state_of(ctx);
  state.layout = p;
  ++state.layouts;
  const ViewportRect rect = layout_rect(p, state.pixels_per_unit);
  r.x = rect.x;
  r.y = rect.y;
  r.width = rect.width;
  r.height = rect.height;
  r.layouts = state.layouts;
  return true;
}

bool editor_select(protocol::Context& ctx, const protocol::EditorSelectParams& p,
                   protocol::EditorStateResult& r, protocol::RpcError& error) {
  EditorState& state = state_of(ctx);
  if (state.document == nullptr || !state.document->valid()) {
    error = protocol::make_error(protocol::codes::k_unavailable, "the editor has no document");
    return false;
  }
  const Id128 id = p.id.has_value() ? *p.id : Id128{};
  if (!id.is_null() && !state.document->document().exists(id)) {
    error =
        protocol::make_error(protocol::codes::k_not_found,
                             "editor.select: no live record by that id in the editor's session");
    return false;
  }
  state.document->select(id);
  fill_state(state, r);
  return true;
}

bool editor_state(protocol::Context& ctx, protocol::EditorStateResult& r, protocol::RpcError&) {
  fill_state(state_of(ctx), r);
  return true;
}

}  // namespace

ViewportRect layout_rect(const protocol::EditorLayoutParams& layout, f64 pixels_per_unit) noexcept {
  const f64 ratio = layout.device_pixel_ratio > 0.0 ? layout.device_pixel_ratio : 1.0;
  const f64 scale = ratio / (pixels_per_unit > 0.0 ? pixels_per_unit : 1.0);
  const f64 x0 = std::floor(layout.x * scale);
  const f64 y0 = std::floor(layout.y * scale);
  const f64 x1 = std::ceil((layout.x + layout.width) * scale);
  const f64 y1 = std::ceil((layout.y + layout.height) * scale);
  ViewportRect out;
  out.x = static_cast<i32>(x0);
  out.y = static_cast<i32>(y0);
  out.width = static_cast<u32>(std::max(1.0, x1 - x0));
  out.height = static_cast<u32>(std::max(1.0, y1 - y0));
  return out;
}

protocol::RenderFramedCamera framed_camera(const renderer::Camera& camera) noexcept {
  // engine-host's `framed_camera` (render_methods.cpp) by the same rules: `fov_deg` is the float
  // whose radians are the camera's own, and yaw and pitch are the fly convention's, forward =
  // (-cos p sin y, sin p, -cos p cos y), worked out in f64 from the two world positions.
  protocol::RenderFramedCamera out;
  out.position = camera.position;
  out.target = camera.target;
  out.znear = camera.znear;
  f32 deg = static_cast<f32>(static_cast<f64>(camera.fov_y) * (180.0 / 3.14159265358979323846));
  for (u32 step = 0; step < 16 && radians(deg) != camera.fov_y; ++step) {
    deg = std::nextafter(deg, radians(deg) < camera.fov_y ? std::numeric_limits<f32>::infinity()
                                                          : -std::numeric_limits<f32>::infinity());
  }
  out.fov_deg = deg;
  const DVec3 d = camera.target - camera.position;
  const f64 length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
  if (length > 0.0) {
    const f64 y = d.y / length;
    const f64 to_deg = 180.0 / 3.14159265358979323846;
    out.pitch_deg = static_cast<f32>(std::asin(y < -1.0 ? -1.0 : (y > 1.0 ? 1.0 : y)) * to_deg);
    out.yaw_deg = static_cast<f32>(std::atan2(-d.x, -d.z) * to_deg);
  }
  return out;
}

void add_editor_methods(protocol::Dispatcher& d) {
  d.add(protocol::read_only(
      protocol::method<protocol::EditorLayoutParams, protocol::EditorLayoutResult, &editor_layout>(
          "editor.layout",
          "engine-editor only: where the embedded shell's viewport cell is, in CSS pixels, and the "
          "page's device pixel ratio. The page sends it as a notification on load, on a split drag "
          "and on a resize; the editor moves its native viewport there. UI state, never the "
          "document.")));
  d.add(protocol::read_only(
      protocol::method<protocol::EditorSelectParams, protocol::EditorStateResult, &editor_select>(
          "editor.select",
          "engine-editor only: select a live record of the editor's session (null clears), so the "
          "native viewport outlines it and its gizmo attaches. UI state, never the document.")));
  d.add(protocol::read_only(protocol::method_no_params<protocol::EditorStateResult, &editor_state>(
      "editor.state",
      "engine-editor only: the selection, the viewport's camera (render.capture's shape) and the "
      "last layout, so a click in the native viewport reaches the shell's outliner.")));
}

}  // namespace engine::editor
