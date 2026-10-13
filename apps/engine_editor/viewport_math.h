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

// ---- the rotate gizmo
// -------------------------------------------------------------------------
//
// Three rings about world x, y and z through the placement's origin — the point a change of the
// record's `rotation` turns it about (the mesh's fit goes through the rotation; renderer/scene.h)
// — `k_ring_pixels` in radius at the origin's depth, so a pebble and a building are turned by the
// same rings. Each ring is drawn and hovered as a polyline of its projected points, so hover is a
// few dozen segments tested on the CPU, as the handles are.

inline constexpr f32 k_ring_pixels = 70.0f;
inline constexpr u32 k_ring_segments = 64;
// Shift snaps a turn to this many degrees.
inline constexpr f64 k_rotate_snap_degrees = 15.0;

// The ring about `axis`: two unit vectors spanning its plane, `u` x `v` = the axis, so a point at
// parameter t is centre + radius (cos t u + sin t v) and growing t turns the right-handed way
// about the axis (counter-clockwise seen from its tip), which is the sign a turn's degrees have.
void ring_basis(u32 axis, Vec3& u, Vec3& v) noexcept;

struct GizmoRings {
  bool visible = false;  // the centre is in front of the eye
  Vec3 centre;           // frame space
  f32 radius = 0.0f;     // metres
  f32 cx = 0.0f;         // the centre in the picture
  f32 cy = 0.0f;
  // Each ring's points in the picture, and whether each is in front of the eye; a segment is
  // drawn and hovered only when both its ends are.
  f32 px[k_axis_count][k_ring_segments] = {};
  f32 py[k_axis_count][k_ring_segments] = {};
  bool front[k_axis_count][k_ring_segments] = {};
};
GizmoRings gizmo_rings(const Mat4& view_proj, Vec3 centre, f32 fov_y, u32 width, u32 height,
                       f32 pixels) noexcept;

// The ring under (`px`, `py`): within `tolerance` pixels of its projected polyline. **y is the
// primary ring**: where it and another are both within reach (where they cross, and wherever they
// overlap seen from above or below), y wins; otherwise the nearest. `k_no_axis` over none.
u32 hover_ring(const GizmoRings& rings, f32 px, f32 py, f32 tolerance) noexcept;
// Where the ring about `axis` passes at parameter `t` (radians, `ring_basis`'s), in the picture;
// false when that point is at or behind the eye. What a scripted turn aims the pointer at.
bool ring_point(const Mat4& view_proj, const GizmoRings& rings, u32 axis, f32 t, u32 width,
                u32 height, f32& px, f32& py) noexcept;

// **A turn about one axis.** The pointer's ray meets the ring's plane (through `pivot`, normal to
// the axis); the turn is the angle in that plane from where it met at the press to where it meets
// now, summed step by step so a gesture can go past half a turn. Computed in f64 in the world
// about the pivot, so the eye's place rounds nothing: two rays that are the same line in the world
// give the same angle to the bit wherever the eye stood. A plane seen within a degree of edge-on
// is refused, as `closest_on_axis` refuses an axis within a degree of the line of sight, and so is
// a ray through the pivot itself, where the angle means nothing.
struct RingDrag {
  u32 axis = k_no_axis;
  WorldPos pivot{};
  DVec3 last{0.0, 0.0, 0.0};  // the unit direction in the plane, pivot to the last good hit
  f64 radians = 0.0;          // turned since the press, right-handed about the axis
};
bool ray_on_ring_plane(WorldPos pivot, u32 axis, WorldPos eye, const Ray& ray,
                       DVec3& direction) noexcept;
bool begin_ring_drag(u32 axis, WorldPos pivot, WorldPos eye, const Ray& ray,
                     RingDrag& out) noexcept;
// Updates `radians` from a later ray; false, keeping the last answer, when the ray is refused.
bool update_ring_drag(RingDrag& drag, WorldPos eye, const Ray& ray) noexcept;
// The turn in degrees, snapped to `k_rotate_snap_degrees` steps when `snap`.
f64 ring_drag_degrees(const RingDrag& drag, bool snap) noexcept;

// The unit quaternion turning `degrees` about world `axis`, from the half angle's sine and cosine
// in f64, so a multiple of 90 degrees is as exact as a float can hold it.
Quat axis_quat(u32 axis, f64 degrees) noexcept;
// **What a record's `rotation` becomes** once its placement has turned `degrees` about world
// `axis`, with `yaw_deg` (applied after `rotation`, doc_scene.md) left as the author wrote it: the
// placement stands at Y R, and turned it is G Y R = Y (Y^-1 G Y R), so the new `rotation` is
// Y^-1 G Y R — G R when the axis is y, which Y commutes with. In f64, normalised, and in the
// canonical sign (w > 0, or w = 0 and the first non-zero of x, y, z positive; no negative zero),
// so a quarter turn and back, or four, gives back the bits it started from.
Quat turned_rotation(Quat rotation, f32 yaw_deg, u32 axis, f64 degrees) noexcept;

}  // namespace engine::editor
