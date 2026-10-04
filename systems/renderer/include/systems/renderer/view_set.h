#pragma once

// Multi-view rendering (docs/plan/04-renderer.md §4.6, docs/subsystems/renderer.md). A `ViewSet`
// is N views over **one** scene: one `GpuScene`, one culling hierarchy, one top-level acceleration
// structure, one residency budget. Each view has its own camera orientation, its own projection —
// which may be off-axis, because three monitors around a viewer are three windows onto one world
// and not three cameras — its own rectangle of the color target, and its own quality tier.
//
//     ViewSet views;
//     views.build({.layout = ViewLayout::Surround3, .surround = {.side_yaw = radians(30.0f)},
//                  .peripheral_lod = 2.0f}, 11520, 2160, &error);
//     views.update(orbit_camera(centre, radius, 22.0f, frame));
//     for (u32 v = 0; v < views.size(); ++v) { views[v].view_proj; views[v].rect; }
//
// **The camera is the frame's; the layout is the target's.** `build()` fixes what is sized by the
// screen — the rectangles, the Panini source width, the frustum shapes as tangents at unit
// distance — and `update()` turns one camera into N view-projection matrices every frame. That
// split is what lets the renderer allocate the visibility buffer and the Hi-Z pyramids once.
//
// A **single** view is the default and is byte-for-byte what the renderer has always drawn: one
// rectangle covering the target, `perspective_reversed_z` from the camera's own field of view,
// and no resample. Every expression below reduces to the old one when `size() == 1`.

#include <core/base/types.h>
#include <core/math/math.h>
#include <domain/gfx/cluster_cull.h>
#include <systems/renderer/settings.h>

#include <string>

namespace engine::renderer {

// A `ViewSet` holds at most this many views: three monitors, an eye each for a headset, or a
// hand-built layout. The bound is real rather than defensive — every per-view array in the
// renderer's statistics is this long, and the acceleration structures reserve `views` times the
// scene's pair count.
inline constexpr u32 k_max_views = 8;

struct Camera {
  Vec3 position{};
  Vec3 target{};
  f32 fov_y = 0.9599310886f;  // radians(55)
  f32 znear = 0.1f;           // reversed-Z: there is no far plane
};

// **The elevation every orbit holds above its circle unless told otherwise**, in degrees and in
// radians: the protocol's default `RenderOrbit.pitch_deg` (schemas/protocol.schema), which
// `request_tests.cpp` holds to this constant, and so the pitch of `orbit {distance: 22}` and of
// engine-view's `--orbit 22` alike. It is atan(0.45) to four places — engine-view's orbit rose
// 0.45 of its distance before the two hosts shared it.
//
// **Why one constant and one function, to the bit** (renderer.md, "One request, two hosts"):
// until 2026-10-04 engine-view placed its orbit at `0.45 * d` and the protocol's at
// `tan(radians(24.2277)) * d`, 32 float steps lower — 0.8 mm on the erg's orbit. Nothing in the
// picture showed it but the sky's exposure, which is metered from the eye's altitude and came out
// a few parts in ten million apart between the two hosts, and that moved a channel byte by one
// at a pixel or two. The camera was the cause, not the meter; both hosts now compute every orbit
// through `orbit_camera_at`'s arithmetic from the same pitch.
inline constexpr f32 k_orbit_pitch_deg = 24.2277f;
inline constexpr f32 k_orbit_pitch = radians(k_orbit_pitch_deg);

// How far an orbit at `pitch` (radians, within 85 degrees of level) rises per unit of its
// distance: tan(pitch), worked out from additions, multiplications and one division in double,
// and rounded once to float. Not `std::tan`, because a compiler is free to fold a call on a
// constant at build time — GCC does, correctly rounded — while the same call on a pitch read off
// the wire goes to the C library at run time, and the two need not round alike; with nothing but
// the four operations (and contraction off, ADR-0035) every compiler, every machine and both
// times give the same bits. Within a float step of `std::tan` (`request_tests.cpp`).
f32 orbit_rise(f32 pitch) noexcept;

// The camera engine-view has always orbited with, so a capture of a scene from engine-host and
// a capture of the same scene from engine-view are the same picture. `distance` of zero
// breathes between 8 and 36 units instead of holding still, and every distance scales with the
// scene's radius, so a 2 cm mesh and a 20 m one are framed alike. At frame 0 it is
// `orbit_camera_at(center, radius, distance, 0, k_orbit_pitch)` to the bit.
Camera orbit_camera(const Vec3& center, f32 radius, f32 distance, u64 frame) noexcept;
// The same orbit at explicit angles, which is what a caller that wants one picture asks for.
// `distance` of zero is the breathing orbit's rest distance (22 radius-tenths).
Camera orbit_camera_at(const Vec3& center, f32 radius, f32 distance, f32 yaw, f32 pitch) noexcept;

// A scripted fly-in: step `step` of `steps` on a path from `from` mesh radii to `to`, turning as
// `orbit_camera` turns so that the cut has to change for two reasons and not one.
//
// **The distance is interpolated geometrically, not linearly**, and that is the whole of the
// design. What a LOD cut and a page budget answer to is the *ratio* of distances: halving the
// distance doubles every projected error whether it is 50 radii to 25 or 2 to 1. Equal steps in
// log distance are therefore equal steps in refinement, and a fly-in sampled that way spends its
// steps where pages are actually arriving instead of spending nine tenths of them on the outer
// half of the path, where a mesh is a handful of clusters and nothing is requested at all
// ([geometry](../../../../docs/subsystems/geometry.md), "What it costs on the sample meshes",
// measured exactly that: the outer 80% of the FlightHelmet's fly-in draws out of one page).
//
// `steps` under 2, or a non-positive distance, gives the camera at `from`.
Camera fly_camera(const Vec3& center, f32 radius, f32 from, f32 to, u32 step, u32 steps) noexcept;

// Reversed-Z perspective through an arbitrary rectangle on the near plane, which is what an
// off-axis monitor needs and what `core/math`'s symmetric `perspective_reversed_z` cannot express.
// Same conventions: the camera looks down -z, the near plane maps to 1 and the far plane to 0, and
// there is no far plane. Equal to `perspective_reversed_z(fov_y, aspect, near)` to within floating
// point when the rectangle is centred.
Mat4 off_center_reversed_z(f32 left, f32 right, f32 bottom, f32 top, f32 near) noexcept;

// The three monitors of a surround arrangement, in pixels of the target's own grid.
//
// The rectangles always tile the target with no gap, because a driver-surround swapchain has no
// pixels behind a bezel. `bezel` is therefore **bezel correction**: it widens the angle between
// the monitors as if the hidden pixels existed, so a straight line crossing a bezel still looks
// straight. `side_yaw` is the physical angle the side monitors are turned inward; at zero the
// three monitors are coplanar and the three off-axis frusta are exactly the three thirds of one
// wide rectilinear frustum, which is the identity the tests and experiment E9 lean on.
struct Surround3 {
  u32 monitor_width = 0;   // 0: a third of the target's width
  u32 monitor_height = 0;  // 0: the target's height
  u32 bezel = 0;           // gap between two monitors, in the same pixel units
  f32 side_yaw = 0.0f;     // radians the side monitors are turned towards the viewer
};

// What a view is allowed to cost. A surround player treats the side monitors as peripheral vision
// (§4.6), so a peripheral view takes a coarser cut and, once there is a shading-rate path, a
// coarser shading rate.
struct ViewQuality {
  // Multiplies `RenderSettings::lod_px` in this view's cull pass: 2 lets a cluster be twice as
  // wrong in screen space before its parent group is drawn instead.
  f32 lod_scale = 1.0f;
  // 1 is full rate. **Stubbed**: the value is carried and reported, and nothing consumes it. A
  // real rate needs `VK_KHR_fragment_shading_rate` in the RHI and a per-view attachment or a
  // per-draw rate, neither of which `domain/gfx` has (docs/subsystems/renderer.md, §4.6).
  u32 shading_rate = 1;
};

// A rectangle of pixels. Signed nowhere: every screen-space structure in this engine is sized from
// the render configuration and there are no 16-bit screen coordinates (§4.6, coordinate hygiene).
struct ViewRect {
  u32 x = 0;
  u32 y = 0;
  u32 width = 0;
  u32 height = 0;
  u64 pixels() const noexcept { return u64{width} * height; }
};

struct View {
  // Where the view's picture lands in the color target.
  ViewRect rect;
  // What the rasterizers and the visibility buffer work at. The same as `rect`'s extent for every
  // view that draws rectilinearly; wider for a Panini view, whose rectilinear source is
  // oversampled so that the resample never magnifies (see `ViewSet::oversample`).
  u32 source_width = 0;
  u32 source_height = 0;
  // Radians about the camera's own up axis: how far this view's forward direction is turned from
  // the frame camera's. Zero for a single view and for a surround's centre monitor, and zero for
  // every view of a flat surround as well, where the turn is in the projection instead.
  f32 yaw = 0.0f;
  // A centred projection from the camera's own field of view at `aspect`, or the off-axis
  // rectangle below. `aspect` is the *picture's*, not the source's: a Panini view renders its
  // wider source through the same frustum the single rectilinear view would have used.
  bool symmetric = true;
  f32 aspect = 1.0f;
  // The off-axis rectangle, as tangents at unit distance; multiplied by the camera's near plane
  // when the projection is built, so the near plane stays the frame's.
  f32 left = -1.0f;
  f32 right = 1.0f;
  f32 bottom = -1.0f;
  f32 top = 1.0f;
  ViewQuality quality;

  // ---- filled by ViewSet::update() ------------------------------------------------------------
  Mat4 view_proj;
  // Clip space to the world direction of the eye's ray through a pixel: the inverse of the
  // projection times the view's rotation, with the eye at the origin (gfx::clip_to_ray). What the
  // sky, the ray path's primary rays and the reference's camera rays read a pixel's direction
  // from, because the inverse of `view_proj` carries the camera's world position and, kilometres
  // from the origin, a direction made from it is wrong by pixels (gfx.md, "The sky").
  Mat4 clip_to_ray;
  // The cull pass's LOD scale, cot(fov_y / 2) * source_height / 2 for a centred projection and
  // the same expression through the projection's own vertical scale for an off-axis one. In
  // *source* pixels, because that is where the error the threshold bounds is measured.
  f32 proj_scale = 0.0f;
};

struct ViewSetDesc {
  ViewLayout layout = ViewLayout::Single;
  Surround3 surround;
  // The Panini parameter d: 0 is the ordinary rectilinear projection, 1 is the classic Pannini,
  // and larger values compress the periphery harder. Only read for `ViewLayout::Panini`.
  f32 panini_d = 1.0f;
  // Multiplies the LOD threshold of every view outside the attention region. The attention region
  // is the centre monitor for a surround and the whole display otherwise (§4.6), so this is the
  // side monitors' `ViewQuality::lod_scale` and nothing else's.
  f32 peripheral_lod = 1.0f;
  // The vertical field of view the layout is sized from. It decides the surround's eye distance
  // and the Panini source's width, both of which size buffers, so it is fixed here rather than
  // taken from a frame's camera. `update()` still builds every projection from the camera it is
  // given; a camera with a different field of view is rendered correctly, just over- or
  // under-sampled at the edges of a Panini view.
  f32 fov_y = 0.9599310886f;  // radians(55)
};

// How many views a layout has, before anything is built. `resolve_settings` needs it to size the
// scene's per-frame working set, which is created before the renderer and therefore before the
// view set.
u32 view_count_of(ViewLayout layout) noexcept;

class ViewSet {
 public:
  // Lays the layout out over a `width` x `height` color target. False with `error` when it does
  // not fit — a surround whose target is not divisible by three, a Panini oversample that would
  // need more source pixels than `k_max_source_pixels`, or a zero extent.
  bool build(const ViewSetDesc& desc, u32 width, u32 height, std::string* error = nullptr);
  // Every view's projection and view-projection matrix, from this frame's camera. The eye and the
  // near plane are shared; only the orientation and the frustum differ.
  void update(const Camera& camera) noexcept;

  u32 size() const noexcept { return count_; }
  const View& operator[](u32 index) const noexcept { return views_[index]; }
  const ViewSetDesc& desc() const noexcept { return desc_; }
  ViewLayout layout() const noexcept { return desc_.layout; }

  u32 width() const noexcept { return width_; }    // the color target
  u32 height() const noexcept { return height_; }  // the color target
  // The largest source rectangle any view rasterizes into, which is the render area the raster
  // passes need. Equal to the target's extent unless a view is oversampled.
  u32 source_width() const noexcept { return source_width_; }
  u32 source_height() const noexcept { return source_height_; }
  // Pixels of visibility buffer the whole set needs: the views' source rectangles, back to back.
  u64 source_pixels() const noexcept { return source_pixels_; }

  // Whether the resolve resamples, and the three numbers it resamples with. `panini_half_width`
  // is the picture's half-width in image units and `source_half_width` the rectilinear source's;
  // their ratio is `oversample`, which is how much wider the source is in pixels.
  bool resample() const noexcept { return resample_; }
  f32 panini_d() const noexcept { return desc_.panini_d; }
  f32 panini_half_width() const noexcept { return panini_half_width_; }
  f32 source_half_width() const noexcept { return source_half_width_; }
  f32 oversample() const noexcept { return oversample_; }

 private:
  ViewSetDesc desc_;
  View views_[k_max_views];
  u32 count_ = 1;
  u32 width_ = 0;
  u32 height_ = 0;
  u32 source_width_ = 0;
  u32 source_height_ = 0;
  u64 source_pixels_ = 0;
  f32 panini_half_width_ = 0.0f;
  f32 source_half_width_ = 0.0f;
  f32 oversample_ = 1.0f;
  bool resample_ = false;
};

// Where output pixel (x, y) of a Panini view reads its rectilinear source, in that view's source
// pixels. The CPU mirror of `panini_source_ndc` in visibility_resolve.slang, for the capture path,
// which has to name what is under a pixel of the *picture*; false when the map lands outside the
// source, which is a pixel the resolve left as sky. `set.resample()` must be true.
bool panini_source_pixel(const ViewSet& set, const View& view, u32 x, u32 y, u32& source_x,
                         u32& source_y) noexcept;

// The largest source rectangle a Panini view may ask for, as a multiple of the target's width. A
// Panini source is `(d + cos(theta_max)) / ((d + 1) cos(theta_max))` times as wide as the picture,
// which grows without bound as the horizontal field of view approaches 180 degrees; this is where
// the renderer says so instead of trying to allocate it.
inline constexpr f32 k_max_oversample = 4.0f;

}  // namespace engine::renderer
