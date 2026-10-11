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
// All three change the editor's UI state and never a document, so they are registered read-only
// (protocol.md, "Roles, leases and proposals": a role that may not write still selects). Only the
// editor's dispatcher has them; engine-host answers "method not found". Every handler runs under
// the dispatch lock, which the editor's frame takes too, so `EditorState` is only ever touched by
// one thread at a time.

#include "editor_document.h"

#include <core/base/types.h>
#include <domain/protocol/rpc.h>
#include <systems/renderer/view_set.h>

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
};

// Registers editor.layout, editor.select and editor.state. The dispatcher's `Context::app` must be
// the `EditorState`.
void add_editor_methods(protocol::Dispatcher& dispatcher);

}  // namespace engine::editor
