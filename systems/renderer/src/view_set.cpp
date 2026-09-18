#include <domain/gfx/visibility_resolve.h>
#include <systems/renderer/view_set.h>

#include <cmath>
#include <renderer_log.h>

namespace engine::renderer {

Camera orbit_camera(const Vec3& center, f32 radius, f32 distance, u64 frame) noexcept {
  const f32 angle = static_cast<f32>(frame) * 0.006f;
  const f32 d =
      (distance > 0.0f ? distance : 22.0f + 14.0f * std::sin(static_cast<f32>(frame) * 0.004f)) *
      (radius / 10.0f);
  Camera camera;
  camera.position = center + Vec3{std::cos(angle) * d, 0.45f * d, std::sin(angle) * d};
  camera.target = center;
  camera.fov_y = radians(55.0f);
  camera.znear = 0.01f * radius;  // reversed-Z: 0.1 for the heightfield, 0.2 mm for a 2 cm mesh
  return camera;
}

Camera orbit_camera_at(const Vec3& center, f32 radius, f32 distance, f32 yaw, f32 pitch) noexcept {
  const f32 d = (distance > 0.0f ? distance : 22.0f) * (radius / 10.0f);
  // `pitch` is the elevation above the orbit circle of radius d, so the default (atan(0.45))
  // reproduces engine-view's fixed 0.45 height factor exactly and `--orbit 22` and
  // `orbit {distance: 22}` are the same camera. Clamped short of the pole, where the tangent and
  // the up vector both stop meaning anything.
  const f32 limit = radians(85.0f);
  const f32 clamped = pitch < -limit ? -limit : (pitch > limit ? limit : pitch);
  Camera camera;
  camera.position = center + Vec3{std::cos(yaw) * d, std::tan(clamped) * d, std::sin(yaw) * d};
  camera.target = center;
  camera.fov_y = radians(55.0f);
  camera.znear = 0.01f * radius;
  return camera;
}

Mat4 off_center_reversed_z(f32 left, f32 right, f32 bottom, f32 top, f32 near) noexcept {
  Mat4 m;
  m.at(0, 0) = 2.0f * near / (right - left);
  m.at(0, 2) = (right + left) / (right - left);
  m.at(1, 1) = 2.0f * near / (top - bottom);
  m.at(1, 2) = (top + bottom) / (top - bottom);
  m.at(2, 2) = 0.0f;  // infinite far plane, as perspective_reversed_z's default
  m.at(2, 3) = near;
  m.at(3, 2) = -1.0f;
  m.at(3, 3) = 0.0f;
  return m;
}

u32 view_count_of(ViewLayout layout) noexcept { return layout == ViewLayout::Surround3 ? 3u : 1u; }

bool ViewSet::build(const ViewSetDesc& desc, u32 width, u32 height, std::string* error) {
  *this = ViewSet{};
  desc_ = desc;
  width_ = width;
  height_ = height;
  if (width == 0 || height == 0) {
    if (error != nullptr) *error = "a view set needs a target with a positive extent";
    return false;
  }
  const f32 half_fov_y = desc.fov_y * 0.5f;
  const f32 tan_half_y = std::tan(half_fov_y);

  switch (desc.layout) {
    case ViewLayout::Single: {
      count_ = 1;
      views_[0].rect = ViewRect{0, 0, width, height};
      views_[0].source_width = width;
      views_[0].source_height = height;
      views_[0].symmetric = true;
      views_[0].aspect = static_cast<f32>(width) / static_cast<f32>(height);
      break;
    }
    case ViewLayout::Surround3: {
      // Three monitors side by side, no gap in pixels: a driver-surround surface has none.
      const u32 monitor_w =
          desc.surround.monitor_width != 0 ? desc.surround.monitor_width : width / 3;
      const u32 monitor_h =
          desc.surround.monitor_height != 0 ? desc.surround.monitor_height : height;
      if (monitor_w == 0 || monitor_h == 0 || monitor_w * 3 > width || monitor_h > height) {
        if (error != nullptr) {
          *error =
              "surround3 needs three monitors that fit the target; pass a width divisible by "
              "three or set Surround3::monitor_width";
        }
        return false;
      }
      count_ = 3;
      // The eye distance that makes one monitor subtend the vertical field of view, in the same
      // pixel units the monitor geometry is in. Everything below is a similar triangle.
      const f32 eye_distance = (static_cast<f32>(monitor_h) * 0.5f) / tan_half_y;
      const f32 half_w = static_cast<f32>(monitor_w) * 0.5f;
      const f32 half_h = static_cast<f32>(monitor_h) * 0.5f;
      const f32 hinge = half_w + static_cast<f32>(desc.surround.bezel);
      const f32 yaw = desc.surround.side_yaw;
      const f32 cos_yaw = std::cos(yaw);
      const f32 sin_yaw = std::sin(yaw);
      // A side monitor is perpendicular to its own view axis by construction, so every point on it
      // is the same distance along that axis and the frustum is one rectangle at that distance.
      const f32 side_distance = eye_distance * cos_yaw + hinge * sin_yaw;
      const f32 side_x0 = hinge * cos_yaw - eye_distance * sin_yaw;
      for (u32 v = 0; v < 3; ++v) {
        View& view = views_[v];
        view.rect = ViewRect{v * monitor_w, 0, monitor_w, monitor_h};
        view.source_width = monitor_w;
        view.source_height = monitor_h;
        view.symmetric = false;
        if (v == 1) {
          // The centre monitor is never turned and never moved by the bezel, so its frustum is
          // centred whatever the arrangement — and a centred frustum is the symmetric projection.
          // Taking it rather than the off-axis one is not an optimization: it makes the centre
          // view of a flat surround **bit-for-bit** the single view of one monitor, which is the
          // identity the multi-view cull is tested against.
          view.yaw = 0.0f;
          view.symmetric = true;
          view.aspect = static_cast<f32>(monitor_w) / static_cast<f32>(monitor_h);
          view.left = -half_w / eye_distance;
          view.right = half_w / eye_distance;
          view.top = half_h / eye_distance;
          view.bottom = -view.top;
        } else {
          const f32 side = v == 0 ? -1.0f : 1.0f;
          view.yaw = side * yaw;
          const f32 inner = side_x0 / side_distance;
          const f32 outer = (side_x0 + static_cast<f32>(monitor_w)) / side_distance;
          view.left = side > 0.0f ? inner : -outer;
          view.right = side > 0.0f ? outer : -inner;
          view.top = half_h / side_distance;
          view.bottom = -view.top;
          view.quality.lod_scale = desc.peripheral_lod;
          // A coarser shading rate for the periphery is what §4.6 asks for next; nothing reads it.
          view.quality.shading_rate = desc.peripheral_lod > 1.0f ? 2u : 1u;
        }
      }
      break;
    }
    case ViewLayout::Panini: {
      count_ = 1;
      // The picture holds exactly the field of view the single rectilinear picture would have, so
      // the two are comparable; the source is the same frustum at a wider resolution.
      const f32 half_fov_x =
          std::atan(tan_half_y * static_cast<f32>(width) / static_cast<f32>(height));
      source_half_width_ = std::tan(half_fov_x);
      panini_half_width_ = gfx::panini_abscissa(desc.panini_d, half_fov_x);
      oversample_ = gfx::panini_oversample(desc.panini_d, half_fov_x);
      if (!(oversample_ >= 1.0f) || oversample_ > k_max_oversample) {
        if (error != nullptr) {
          *error =
              "a Panini view at this field of view and d would need a rectilinear source "
              "more than four times the target's width; lower d or narrow the view";
        }
        return false;
      }
      const u32 source_w = static_cast<u32>(static_cast<f32>(width) * oversample_ + 0.5f);  // round
      views_[0].rect = ViewRect{0, 0, width, height};
      views_[0].source_width = source_w > width ? source_w : width;
      views_[0].source_height = height;
      views_[0].symmetric = true;
      views_[0].aspect = static_cast<f32>(width) / static_cast<f32>(height);
      resample_ = true;
      break;
    }
  }

  source_width_ = 0;
  source_height_ = 0;
  source_pixels_ = 0;
  for (u32 v = 0; v < count_; ++v) {
    source_width_ = views_[v].source_width > source_width_ ? views_[v].source_width : source_width_;
    source_height_ =
        views_[v].source_height > source_height_ ? views_[v].source_height : source_height_;
    source_pixels_ += u64{views_[v].source_width} * views_[v].source_height;
  }
  ENGINE_LOG_INFO(log_renderer, "view set", log::field("layout", view_layout_name(desc.layout)),
                  log::field("views", count_), log::field("width", width),
                  log::field("height", height), log::field("source_width", source_width_),
                  log::field("source_pixels", source_pixels_),
                  log::field("oversample", static_cast<f64>(oversample_)),
                  log::field("side_yaw_deg", static_cast<f64>(degrees(desc.surround.side_yaw))),
                  log::field("peripheral_lod", static_cast<f64>(desc.peripheral_lod)));
  return true;
}

// The same inverse the resolve does, in double precision on the CPU. Kept beside the projection
// it inverts rather than beside its caller, because the shader's copy and this one have to agree
// and the test that says so compares them here.
bool panini_source_pixel(const ViewSet& set, const View& view, u32 x, u32 y, u32& source_x,
                         u32& source_y) noexcept {
  const f32 ndc_x =
      ((static_cast<f32>(x) + 0.5f) / static_cast<f32>(view.rect.width)) * 2.0f - 1.0f;
  const f32 ndc_y =
      1.0f - ((static_cast<f32>(y) + 0.5f) / static_cast<f32>(view.rect.height)) * 2.0f;
  const f32 d = set.panini_d();
  const f32 k = ndc_x * set.panini_half_width() / (d + 1.0f);
  const f32 root = std::sqrt(1.0f + k * k);
  const f32 ratio = k * d / root;
  const f32 theta = std::atan(k) + std::asin(ratio < -1.0f ? -1.0f : (ratio > 1.0f ? 1.0f : ratio));
  const f32 cos_theta = std::cos(theta);
  const f32 s = (d + 1.0f) / (d + cos_theta);
  const f32 src_ndc_x = std::tan(theta) / set.source_half_width();
  const f32 src_ndc_y = ndc_y / (s * cos_theta);
  const f32 px = (src_ndc_x * 0.5f + 0.5f) * static_cast<f32>(view.source_width);
  const f32 py = (0.5f - src_ndc_y * 0.5f) * static_cast<f32>(view.source_height);
  if (!(px >= 0.0f) || !(py >= 0.0f)) return false;
  source_x = static_cast<u32>(px);
  source_y = static_cast<u32>(py);
  return source_x < view.source_width && source_y < view.source_height;
}

void ViewSet::update(const Camera& camera) noexcept {
  // The head frame: the frame camera's own forward, right, and up. A view's yaw turns the forward
  // direction about the head's up, which is what a monitor standing beside another one does.
  const Vec3 forward = normalize(camera.target - camera.position);
  const Vec3 right = normalize(cross(forward, Vec3{0.0f, 1.0f, 0.0f}));
  const Vec3 up = cross(right, forward);
  for (u32 v = 0; v < count_; ++v) {
    View& view = views_[v];
    // A view that is not turned takes the frame camera's own look-at expression, so that a single
    // view produces the matrix it always has, to the bit.
    const Mat4 view_matrix =
        view.yaw == 0.0f
            ? look_at(camera.position, camera.target, Vec3{0.0f, 1.0f, 0.0f})
            : look_at(camera.position,
                      camera.position + forward * std::cos(view.yaw) + right * std::sin(view.yaw),
                      up);
    const Mat4 projection =
        view.symmetric ? perspective_reversed_z(camera.fov_y, view.aspect, camera.znear)
                       : off_center_reversed_z(view.left * camera.znear, view.right * camera.znear,
                                               view.bottom * camera.znear, view.top * camera.znear,
                                               camera.znear);
    view.view_proj = projection * view_matrix;
    view.proj_scale = projection.at(1, 1) * static_cast<f32>(view.source_height) * 0.5f;
  }
}

}  // namespace engine::renderer
