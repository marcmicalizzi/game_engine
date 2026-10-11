#pragma once

// engine-editor's pointer maths (docs/subsystems/apps.md, "engine-editor"): a pointer's window
// coordinates as a pixel of the picture, the eye's ray through a pixel, and the translate gizmo's
// handles, hover and drag. Pure functions of the frame's own view-projection (`View::view_proj`),
// with no window, no device and no ImGui, so the tests hold every case of them without any of the
// three — and so the gizmo's hover is a few segments tested on the CPU each frame and never a read
// of the visibility buffer, which is `Viewport::pick`'s, a click's cost and not a frame's.
//
// **Every point is in the frame's space**, whose origin is the eye (ADR-0053): a world position
// enters as `relative(p, camera.position)`, the space `view_proj` maps from and the selection box
// is projected in. Only the drag keeps world positions (`WorldPos`, f64), because the camera may
// fly while the button is held and the axis the drag follows is a line in the world, not in the
// frame.
//
// Pixels are the picture's, y down, the convention the selection box and `Viewport::pick` use.

#include <core/base/types.h>
#include <core/math/math.h>
#include <core/math/world.h>

namespace engine::editor {

// ---- the pointer
// ---------------------------------------------------------------------------------

// What a pointer's window coordinates are multiplied by to be pixels of the picture. The window
// reports the pointer in its own coordinates, and on a display that scales (a high-DPI display at
// 150 %) those are not the swapchain's pixels: the ratio is the picture's size over the window's
// logical size, per axis. One where either size is unknown (zero), which is also the answer
// offscreen, where there is no window and the scripted coordinates are the picture's own pixels.
struct PointerScale {
  f32 x = 1.0f;
  f32 y = 1.0f;
};
PointerScale pointer_scale(u32 window_width, u32 window_height, u32 pixel_width,
                           u32 pixel_height) noexcept;

// ---- rays and projection
// -------------------------------------------------------------------------

struct Ray {
  Vec3 origin;     // on the near plane, in the frame's space
  Vec3 direction;  // unit length, away from the eye
};

// The eye's ray through point (`px`, `py`) of a `width` x `height` picture, through the inverse
// of `view_proj`: the point on the near plane (reversed-Z depth 1) and a point further along
// (depth 0.5), so it holds for an orthographic projection as for a perspective one. False when
// the picture is empty or the matrix does not invert.
bool pixel_ray(const Mat4& view_proj, f32 px, f32 py, u32 width, u32 height, Ray& out) noexcept;

// Where frame-space point `p` lands in the picture; false when it is at or behind the eye.
bool project_point(const Mat4& view_proj, Vec3 p, u32 width, u32 height, f32& px, f32& py) noexcept;

// The parameter `t` of the point of the line `origin + t * axis` (axis unit length) nearest to
// `ray`. False when the two are within a degree of parallel — an axis pointing at the eye, along
// which the pointer can say nothing — or when the nearest point of the ray is behind its origin.
bool closest_on_axis(const Ray& ray, Vec3 origin, Vec3 axis, f32& t) noexcept;

// ---- the translate gizmo
// -------------------------------------------------------------------------

inline constexpr u32 k_axis_count = 3;
inline constexpr u32 k_no_axis = 3;
// How long a handle is drawn, in window coordinates (multiplied by the pointer scale to be the
// picture's pixels, so a handle is the same size on every display), and how near the pointer must
// come to one to hover it. A fixed screen size rather than the selection's own, so a pebble and a
// building are moved by the same handles.
inline constexpr f32 k_handle_pixels = 80.0f;
inline constexpr f32 k_hover_pixels = 6.0f;
// The stretch of each handle nearest the centre where all three meet, as a fraction of its length,
// that hovers none of them: there the pointer is as near one axis as another.
inline constexpr f32 k_handle_dead_zone = 0.15f;

// World x, y or z.
Vec3 axis_direction(u32 axis) noexcept;
// "x", "y" or "z": what a rationale and the summary call it.
const char* axis_name(u32 axis) noexcept;

// The handles as drawn this frame: from the centre along each world axis for `length` metres,
// which is `pixels` long in the picture at the centre's depth.
struct GizmoHandles {
  bool visible = false;  // the centre is in front of the eye
  Vec3 centre;           // frame space
  f32 length = 0.0f;     // metres
  f32 cx = 0.0f;         // the centre in the picture
  f32 cy = 0.0f;
  f32 ex[k_axis_count] = {};  // each handle's far end in the picture
  f32 ey[k_axis_count] = {};
  bool shown[k_axis_count] = {};  // its far end is in front of the eye too
};
GizmoHandles gizmo_handles(const Mat4& view_proj, Vec3 centre, f32 fov_y, u32 width, u32 height,
                           f32 pixels) noexcept;

// The handle under (`px`, `py`): the nearest shown handle within `tolerance` pixels of its
// segment, past its dead zone; `k_no_axis` over none.
u32 hover_axis(const GizmoHandles& handles, f32 px, f32 py, f32 tolerance) noexcept;

// **A drag along one axis.** Begun where the pointer's ray met the axis line through the handles'
// centre, its `delta` is how far along the axis the ray meets the same line now: the same line in
// the world wherever the eye has gone since, so flying while dragging does not move the selection.
struct AxisDrag {
  u32 axis = k_no_axis;
  WorldPos anchor{};  // the handles' centre when the drag began
  f32 start = 0.0f;   // where along the axis the ray met it then, metres from `anchor`
  f32 delta = 0.0f;   // metres along the axis since: the last well-conditioned answer
};
// False (and `out` untouched) when the ray cannot meet the axis (`closest_on_axis`).
bool begin_axis_drag(u32 axis, WorldPos anchor, WorldPos eye, const Ray& ray,
                     AxisDrag& out) noexcept;
// Updates `delta` from a later ray; false, keeping the last delta, when the ray and the axis are
// near parallel.
bool update_axis_drag(AxisDrag& drag, WorldPos eye, const Ray& ray) noexcept;
// Where a translation that stood at `start` when the drag began stands now: moved by `delta`
// along the axis in f64, and on the other two axes not at all, to the bit.
WorldPos dragged_translation(WorldPos start, const AxisDrag& drag) noexcept;

}  // namespace engine::editor
