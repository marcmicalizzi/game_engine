#include "editor_methods.h"

#include "editor_import.h"
#include "editor_input.h"

#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/math/math.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

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
  out.tool = state.tool;
}

bool editor_tool(protocol::Context& ctx, const protocol::EditorToolParams& p,
                 protocol::EditorStateResult& r, protocol::RpcError& error) {
  EditorState& state = state_of(ctx);
  Tool tool = Tool::Translate;
  if (!tool_from_name(p.tool, tool)) {
    error = protocol::make_error(protocol::codes::k_invalid_params,
                                 "editor.tool: \"translate\" or \"rotate\"");
    return false;
  }
  state.tool = tool_name(tool);
  fill_state(state, r);
  return true;
}

bool editor_import_mesh(protocol::Context& ctx, const protocol::EditorImportMeshParams& p,
                        protocol::EditorImportMeshResult& r, protocol::RpcError& error) {
  EditorState& state = state_of(ctx);
  if (state.document == nullptr || !state.document->valid()) {
    error = protocol::make_error(protocol::codes::k_unavailable, "the editor has no document");
    return false;
  }
  EditorDocument& document = *state.document;
  ImportedFiles files;
  std::string why;
  if (!import_mesh_files(p.path, document.directory(), files, why)) {
    error = protocol::make_error(protocol::codes::k_invalid_argument, "editor.import_mesh: " + why);
    return false;
  }
  const WorldPos at = p.at.has_value()
                          ? *p.at
                          : default_import_position(state.camera.position, state.camera.target);
  const std::string name = p.name.has_value() && !p.name->empty() ? *p.name : files.stem;
  JsonValue properties = JsonValue::object();
  properties.set("name", JsonValue(name));
  properties.set("mesh", JsonValue(files.mesh));
  JsonValue translation = JsonValue::array();
  translation.push_back(JsonValue(at.x));
  translation.push_back(JsonValue(at.y));
  translation.push_back(JsonValue(at.z));
  properties.set("translation", std::move(translation));
  const Id128 id = Id128::generate();
  const doc::Command create =
      doc::cmd_create(id, "engine.scene.MeshPlacement", Id128{}, std::move(properties));
  // The file's own name in the rationale, as the person chose it.
  const usize slash = p.path.find_last_of("/\\");
  const std::string file = slash == std::string::npos ? p.path : p.path.substr(slash + 1);
  if (!document.apply(std::span<const doc::Command>(&create, 1), "import " + file, why)) {
    remove_imported(document.directory(), files);
    error = protocol::make_error(protocol::codes::k_document_error, "editor.import_mesh: " + why);
    return false;
  }
  // Selected now, so the page's outliner shows it; the frame keeps the selection once the
  // viewport has rebuilt with it.
  document.select(id);
  state.imported = id;
  r.id = id;
  r.name = name;
  r.mesh = files.mesh;
  r.translation = at;
  for (const std::string& c : files.copied)
    r.copied.push_back(c);
  for (const std::string& c : files.reused)
    r.reused.push_back(c);
  return true;
}

bool editor_pick_file(protocol::Context& ctx, const protocol::EditorPickFileParams& p,
                      protocol::EditorPickFileResult& r, protocol::RpcError& error) {
  EditorState& state = state_of(ctx);
  r.path.reset();
  if (!state.dialogs || state.lock == nullptr || state.closing) return true;
  if (state.pick_pending) {
    error = protocol::make_error(protocol::codes::k_unavailable,
                                 "editor.pick_file: a dialog is already open");
    return false;
  }
  state.pick_filter.clear();
  for (const std::string& f : p.filter)
    state.pick_filter.push_back(f);
  state.pick_pending = true;
  state.pick_answered = false;
  // The dispatch lock is this thread's (the listener took it); waiting releases it, so the frame
  // goes on drawing and shows the dialog, and takes it back before the answer is read.
  std::unique_lock<std::mutex> held(*state.lock, std::adopt_lock);
  state.pick_done.wait(held, [&] { return state.pick_answered || state.closing; });
  (void)held.release();
  state.pick_pending = false;
  if (state.pick_answered && !state.pick_path.empty()) r.path = state.pick_path;
  return true;
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

bool editor_test_report(protocol::Context& ctx, const protocol::EditorTestReportParams& p,
                        protocol::EditorTestReportResult& r, protocol::RpcError& error) {
  EditorState& state = state_of(ctx);
  // A test page reports a handful of checks; anything past that is a page in a loop.
  if (state.reports.size() >= 256 || p.name.empty() || p.name.size() > 64 ||
      p.detail.size() > 4096) {
    error = protocol::make_error(protocol::codes::k_invalid_params,
                                 "editor.test_report: a name of 1 to 64 bytes, a detail of at most "
                                 "4096, and at most 256 reports a run");
    return false;
  }
  state.reports.push_back(EditorState::Report{p.name, p.ok, p.detail});
  r.reports = static_cast<u32>(state.reports.size());
  return true;
}

}  // namespace

ViewportRect layout_rect(const protocol::EditorLayoutParams& layout, f64 pixels_per_unit) noexcept {
  const f64 ratio = layout.device_pixel_ratio > 0.0 ? layout.device_pixel_ratio : 1.0;
  const f64 scale = ratio / (pixels_per_unit > 0.0 ? pixels_per_unit : 1.0);
  // A twentieth of a pixel of slack before rounding outward: the page lays out in 1/64ths of a CSS
  // pixel, so a cell whose edge is the window's (853.33 CSS pixels at 1.5, laid out as 853.34) must
  // not round past it into a pixel the window does not have.
  constexpr f64 k_slack = 0.05;
  const f64 x0 = std::floor(layout.x * scale + k_slack);
  const f64 y0 = std::floor(layout.y * scale + k_slack);
  const f64 x1 = std::ceil((layout.x + layout.width) * scale - k_slack);
  const f64 y1 = std::ceil((layout.y + layout.height) * scale - k_slack);
  ViewportRect out;
  out.x = static_cast<i32>(x0);
  out.y = static_cast<i32>(y0);
  out.width = static_cast<u32>(std::max(1.0, x1 - x0));
  out.height = static_cast<u32>(std::max(1.0, y1 - y0));
  return out;
}

Placement place_viewport(const protocol::EditorLayoutParams& layout, f64 pixels_per_unit,
                         u32 client_width, u32 client_height) noexcept {
  Placement out;
  out.rect = layout_rect(layout, pixels_per_unit);
  if (client_width == 0 || client_height == 0) return out;
  ViewportRect& r = out.rect;
  const ViewportRect asked = r;
  const i64 cw = static_cast<i64>(client_width);
  const i64 ch = static_cast<i64>(client_height);
  i64 x0 = std::clamp<i64>(r.x, 0, cw - 1);
  i64 y0 = std::clamp<i64>(r.y, 0, ch - 1);
  i64 x1 = std::clamp<i64>(static_cast<i64>(r.x) + static_cast<i64>(r.width), x0 + 1, cw);
  i64 y1 = std::clamp<i64>(static_cast<i64>(r.y) + static_cast<i64>(r.height), y0 + 1, ch);
  r.x = static_cast<i32>(x0);
  r.y = static_cast<i32>(y0);
  r.width = static_cast<u32>(x1 - x0);
  r.height = static_cast<u32>(y1 - y0);
  // A page's viewport is a whole number of CSS pixels (a 1280-pixel view at 1.5 lays out 854 of
  // them, not 853.33), so a cell at its edge can reach one CSS pixel past the window: that much is
  // rounding, cut without a word. More is a page laid out wider than its window.
  const f64 scale = (layout.device_pixel_ratio > 0.0 ? layout.device_pixel_ratio : 1.0) /
                    (pixels_per_unit > 0.0 ? pixels_per_unit : 1.0);
  const i64 rounding = static_cast<i64>(std::ceil(scale));
  const i64 over = std::max({i64{0} - asked.x, i64{0} - asked.y,
                             static_cast<i64>(asked.x) + static_cast<i64>(asked.width) - cw,
                             static_cast<i64>(asked.y) + static_cast<i64>(asked.height) - ch});
  out.clamped = over > rounding;
  return out;
}

protocol::EditorLayoutParams rescaled_layout(const protocol::EditorLayoutParams& layout, f64 from,
                                             f64 to) noexcept {
  protocol::EditorLayoutParams out = layout;
  if (from > 0.0 && to > 0.0 && std::isfinite(from) && std::isfinite(to))
    out.device_pixel_ratio = layout.device_pixel_ratio * (to / from);
  return out;
}

std::string relayout_notification(std::string_view reason) {
  JsonValue params = JsonValue::object();
  params.set("reason", JsonValue(std::string(reason)));
  JsonValue n = JsonValue::object();
  n.set("jsonrpc", JsonValue("2.0"));
  n.set("method", JsonValue("editor.relayout"));
  n.set("params", std::move(params));
  JsonWriteOptions compact;
  compact.pretty = false;
  return write_json(n, compact);
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
      "engine-editor only: the selection, the viewport's camera (render.capture's shape), the "
      "last layout and the gizmo's tool, so a click in the native viewport reaches the shell's "
      "outliner.")));
  d.add(protocol::read_only(
      protocol::method<protocol::EditorToolParams, protocol::EditorStateResult, &editor_tool>(
          "editor.tool",
          "engine-editor only: the viewport gizmo's tool, \"translate\" or \"rotate\" (the T and R "
          "keys in the viewport). UI state, never the document.")));
  d.add(protocol::read_only(protocol::method<protocol::EditorPickFileParams,
                                             protocol::EditorPickFileResult, &editor_pick_file>(
      "editor.pick_file",
      "engine-editor only: the platform's open-file dialog on the editor's window, answering "
      "the chosen path or null; null at once in a test or an editor with no window. Reads "
      "nothing and writes nothing.")));
  // A write: it changes the document, so roles govern it (protocol.md, "Roles, leases and
  // proposals").
  d.add(protocol::method<protocol::EditorImportMeshParams, protocol::EditorImportMeshResult,
                         &editor_import_mesh>(
      "editor.import_mesh",
      "engine-editor only: copy a .gltf (with its buffers and images), .glb or .clusters file into "
      "the document's assets/<stem>/ and create one engine.scene.MeshPlacement naming it, in one "
      "transaction, at `at` or on the ground under the viewport's centre; the viewport rebuilds "
      "and selects it. Refused, with nothing written, for a missing file, a format the loader "
      "refuses, or a copy that would overwrite other bytes."));
}

std::string tool_notification(std::string_view tool) {
  JsonValue params = JsonValue::object();
  params.set("tool", JsonValue(std::string(tool)));
  JsonValue n = JsonValue::object();
  n.set("jsonrpc", JsonValue("2.0"));
  n.set("method", JsonValue("editor.tool"));
  n.set("params", std::move(params));
  JsonWriteOptions compact;
  compact.pretty = false;
  return write_json(n, compact);
}

void add_editor_test_methods(protocol::Dispatcher& d) {
  d.add(protocol::read_only(protocol::method<protocol::EditorTestReportParams,
                                             protocol::EditorTestReportResult, &editor_test_report>(
      "editor.test_report",
      "engine-editor --test-page only: one of the embedded page's own checks (a popup kept out of "
      "the viewport cell, a reconnect), for the run's summary. UI state, never the document.")));
}

}  // namespace engine::editor
