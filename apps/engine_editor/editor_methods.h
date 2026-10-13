#pragma once

// engine-editor's own protocol methods (docs/subsystems/protocol.md, "The editor's methods";
// roadmap A37). The editor runs the protocol's listener in process over a dispatcher round the
// session it already holds, so the web shell embedded beside its viewport is a client of the same
// host as every other client — and these three are what the page and the native viewport need to
// share beyond the document:
//
// - `editor.layout` (a notification): where the page's viewport cell is, in CSS pixels, and the
//   page's device pixel ratio. The window moves the viewport's child window there.
// - `editor.select {id}`: the outliner's choice, so the viewport outlines it and the gizmo
// attaches.
// - `editor.state`: the selection and the viewport's camera, which the page's poll reads, so a
// click
//   in the native viewport reaches the outliner and the properties.
//
// And the editor speaks first once: `editor.relayout` (relayout_notification, below), a
// notification to the page when the display's scale changed. `editor.test_report` is the test
// page's channel, registered only with --test-page.
//
// All of them change the editor's UI state and never a document, so they are registered read-only
// (protocol.md, "Roles, leases and proposals": a role that may not write still selects). Only the
// editor's dispatcher has them; engine-host answers "method not found". Every handler runs under
// the dispatch lock, which the editor's frame takes too, so `EditorState` is only ever touched by
// one thread at a time.

#include "editor_document.h"

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/protocol/rpc.h>
#include <systems/renderer/view_set.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>

namespace engine::editor {

// Where a layout puts the viewport, in the editor window's own coordinates.
struct ViewportRect {
  i32 x = 0;
  i32 y = 0;
  u32 width = 0;
  u32 height = 0;
  bool operator==(const ViewportRect&) const noexcept = default;
};

// The page's cell, in CSS pixels, as the window's coordinates: CSS pixels times the page's ratio
// are the window's pixels (the webview fills the client area from its corner), and the window's
// coordinates are its pixels over `pixels_per_unit` (the swapchain-over-logical ratio stage 2's
// picking uses: one where the platform's coordinates are pixels, as on Windows). Rounded outward to
// whole units so the cell is covered, and never smaller than one unit a side.
ViewportRect layout_rect(const protocol::EditorLayoutParams& layout, f64 pixels_per_unit) noexcept;

// Where the viewport's child window goes: the layout's rectangle, **clamped to the window's client
// area** (`client_width` x `client_height`, in the window's coordinates), because a child that
// reaches past it is clipped by the window and its picture's edge is lost under the frame — which
// is what a page wider than its window did before the narrow layout (engine_shell.md). `clamped`
// says the clamp changed something, which the editor logs: a page that lays itself out right never
// needs it — beyond a pixel or two of rounding at a fractional ratio, since the page's viewport is
// a whole number of CSS pixels, which is cut without being called a clamp. A zero client size
// clamps nothing (the window has not said yet).
struct Placement {
  ViewportRect rect;
  bool clamped = false;
};
Placement place_viewport(const protocol::EditorLayoutParams& layout, f64 pixels_per_unit,
                         u32 client_width, u32 client_height) noexcept;

// The last layout as the page will send it once it sees a change of the display's scale from
// `from` to `to`: the same CSS rectangle at the ratio scaled by `to / from` (the page's ratio is
// the display's scale times its zoom). What the editor places the child by in the frames between
// the platform's `ScaleChanged` and the page's answer to `editor.relayout`.
protocol::EditorLayoutParams rescaled_layout(const protocol::EditorLayoutParams& layout, f64 from,
                                             f64 to) noexcept;

// `editor.relayout`, the one notification the editor sends the page (over the listener, to every
// connection): "lay yourself out and send `editor.layout` again", on a change of the display's
// scale. The page answers with a layout whether or not its own rectangle moved.
std::string relayout_notification(std::string_view reason);

// The camera in `render.capture`'s shape (A32): position and target, the field of view in degrees
// whose radians are the camera's own float, and the fly convention's yaw and pitch.
protocol::RenderFramedCamera framed_camera(const renderer::Camera& camera) noexcept;

// What the methods read and write. The main loop sets `pixels_per_unit`, `camera`, `width` and
// `height` each frame and reads `layout`/`layouts` to place the viewport; the methods set the rest.
struct EditorState {
  EditorDocument* document = nullptr;
  protocol::EditorLayoutParams layout;
  u32 layouts = 0;
  f64 pixels_per_unit = 1.0;
  protocol::RenderFramedCamera camera;
  u32 width = 0;
  u32 height = 0;
  // The page's own checks, when the editor runs with `--test-page` (`editor.test_report`).
  struct Report {
    std::string name;
    bool ok = false;
    std::string detail;
  };
  Vector<Report> reports;
  // The viewport gizmo's tool, "translate" or "rotate" (`editor.tool`, the T and R keys): the
  // frame applies what a method set and sets what a key chose, and tells the page (`editor.tool`
  // notification) when it changed.
  std::string tool = "translate";
  // The record `editor.import_mesh` created last, which the frame selects once the viewport has
  // it (null: none pending).
  Id128 imported;
  // **`editor.pick_file`'s dialog runs on the editor's thread**, never the listener's: the method
  // asks (`pick_pending`) and waits on `pick_done` with the dispatch lock released; the frame sees
  // the request, shows the platform's dialog outside the lock, and answers. Only a window run with
  // no test flag sets `dialogs`; without it the method answers null at once, so a test never opens
  // a dialog. `closing` answers a pending request with null when the editor stops.
  bool dialogs = false;
  bool closing = false;
  std::mutex* lock = nullptr;  // the dispatch lock the methods run under
  bool pick_pending = false;
  bool pick_answered = false;
  Vector<std::string> pick_filter;
  std::string pick_path;  // empty: cancelled
  std::condition_variable pick_done;
};

// The notification the editor sends every connection when the gizmo's tool changed.
std::string tool_notification(std::string_view tool);

// Registers editor.layout, editor.select and editor.state. The dispatcher's `Context::app` must be
// the `EditorState`.
void add_editor_methods(protocol::Dispatcher& dispatcher);

// Registers `editor.test_report` (read-only), the embedded page's channel for its own checks when
// it was loaded with `?embedded=1&test=1`: only `--test-page` calls this, so a normal editor
// answers the method with "not found" as engine-host does.
void add_editor_test_methods(protocol::Dispatcher& dispatcher);

}  // namespace engine::editor
