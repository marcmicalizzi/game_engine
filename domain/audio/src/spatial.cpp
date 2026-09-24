#include <domain/audio/spatial.h>

#include <cmath>

namespace engine::audio {

namespace {

constexpr f32 k_min_distance_floor = 1.0e-3f;

bool finite(Vec3 v) noexcept {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Normalizes `v`, or returns `fallback` when it is too short (or not finite) to have a direction.
Vec3 normalized_or(Vec3 v, Vec3 fallback) noexcept {
  const f32 length_squared = dot(v, v);
  if (!(length_squared > 1.0e-12f) || !std::isfinite(length_squared)) return fallback;
  return v / std::sqrt(length_squared);
}

f32 clamp_unit(f32 v) noexcept { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

}  // namespace

ListenerBasis make_listener_basis(const Listener& listener) noexcept {
  ListenerBasis basis;
  basis.position = finite(listener.position) ? listener.position : Vec3{};
  basis.forward = normalized_or(listener.forward, Vec3{0.0f, 0.0f, -1.0f});
  // Right-handed, y up, -z ahead (core/math): right = forward x up. An up parallel to forward gives
  // no right vector, so it falls back to +y — and if forward is itself vertical, to the horizontal
  // axis that keeps the basis right-handed.
  Vec3 right = normalized_or(cross(basis.forward, listener.up), Vec3{});
  if (dot(right, right) == 0.0f)
    right = normalized_or(cross(basis.forward, Vec3{0.0f, 1.0f, 0.0f}), Vec3{});
  if (dot(right, right) == 0.0f)
    right = normalized_or(cross(basis.forward, Vec3{0.0f, 0.0f, -1.0f}), Vec3{1.0f, 0.0f, 0.0f});
  basis.right = right;
  basis.up = cross(basis.right, basis.forward);
  return basis;
}

f32 distance_gain(DistanceModel model, f32 distance, f32 min_distance, f32 max_distance) noexcept {
  if (model == DistanceModel::None) return 1.0f;
  // Not `near` and `far`: <windows.h> defines both as empty macros.
  const f32 inner = min_distance > k_min_distance_floor ? min_distance : k_min_distance_floor;
  const f32 outer = max_distance > inner ? max_distance : inner;
  if (!(distance > inner)) return 1.0f;  // also catches a NaN distance: full level, not silence
  if (distance >= outer) return 0.0f;
  const f32 taper = (outer - distance) / (outer - inner);
  return model == DistanceModel::Linear ? taper : (inner / distance) * taper;
}

f32 cone_gain(f32 cos_off_axis, f32 inner_cos, f32 outer_cos, f32 outer_gain) noexcept {
  if (cos_off_axis >= inner_cos) return 1.0f;
  if (cos_off_axis <= outer_cos) return outer_gain;
  // inner_cos > cos_off_axis > outer_cos here, so the denominator is positive.
  const f32 t = (inner_cos - cos_off_axis) / (inner_cos - outer_cos);
  return 1.0f + (outer_gain - 1.0f) * t;
}

SpatialParams spatialize(const SourceSpatial& source, const ListenerBasis& listener) noexcept {
  SpatialParams out;
  out.spread = source.spread < 0.0f ? 0.0f : (source.spread > 1.0f ? 1.0f : source.spread);
  if ((source.flags & k_source_2d) != 0) {
    out.spread = 0.0f;
    return out;
  }
  const Vec3 offset = source.position - listener.position;
  const f32 distance = std::sqrt(dot(offset, offset));
  f32 attenuation =
      distance_gain(source.distance_model, distance, source.min_distance, source.max_distance);
  if (distance > 1.0e-6f) {
    const Vec3 toward = offset / distance;  // listener to source
    out.direction =
        Vec3{dot(toward, listener.right), dot(toward, listener.up), -dot(toward, listener.forward)};
    if (source.directivity == Directivity::Cone) {
      // The source faces its -z; the listener is at -toward from it.
      const Vec3 facing = rotate(source.orientation, Vec3{0.0f, 0.0f, -1.0f});
      attenuation *= cone_gain(-dot(facing, toward), source.cone_inner_cos, source.cone_outer_cos,
                               source.cone_outer_gain);
    }
  }
  out.attenuation = attenuation;
  return out;
}

f32 sin_quarter(f32 t) noexcept {
  if (!(t > 0.0f)) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  // x = t * pi/2 in [0, pi/2); Taylor through x^11, Horner in x^2. The truncation error at pi/2 is
  // (pi/2)^13 / 13! = 5.7e-8, under half an f32 ulp at 1.
  const f32 x = t * 1.57079632679489661923f;
  const f32 x2 = x * x;
  f32 p = -2.50521083854417187751e-8f;      // -1/11!
  p = p * x2 + 2.75573192239858906526e-6f;  // 1/9!
  p = p * x2 - 1.98412698412698412698e-4f;  // -1/7!
  p = p * x2 + 8.33333333333333333333e-3f;  // 1/5!
  p = p * x2 - 1.66666666666666666667e-1f;  // -1/3!
  p = p * x2 + 1.0f;
  const f32 s = x * p;
  return s < 1.0f ? s : 1.0f;
}

f32 cos_degrees(f32 degrees) noexcept {
  if (!std::isfinite(degrees)) return 1.0f;
  // Fold into [0, 360), then use cos(a) = sin(90 - a) by quadrant.
  f32 a = std::fmod(std::fabs(degrees), 360.0f);
  if (a > 180.0f) a = 360.0f - a;  // cos is even about 180
  if (a <= 90.0f) return sin_quarter((90.0f - a) / 90.0f);
  return -sin_quarter((a - 90.0f) / 90.0f);
}

Cone make_cone(f32 inner_degrees, f32 outer_degrees) noexcept {
  const auto half = [](f32 width) {
    const f32 w =
        std::isfinite(width) ? (width < 0.0f ? 0.0f : (width > 360.0f ? 360.0f : width)) : 360.0f;
    return w * 0.5f;
  };
  const f32 inner = half(inner_degrees);
  f32 outer = half(outer_degrees);
  if (outer < inner) outer = inner;
  return Cone{cos_degrees(inner), cos_degrees(outer)};
}

PanGains pan_constant_power(f32 pan) noexcept {
  const f32 p = clamp_unit(pan);
  const f32 t = (p + 1.0f) * 0.5f;  // 0 hard left, 1 hard right
  return PanGains{sin_quarter(1.0f - t), sin_quarter(t)};
}

PanGains pan_balance(f32 pan) noexcept {
  const f32 p = clamp_unit(pan);
  return PanGains{p > 0.0f ? 1.0f - p : 1.0f, p < 0.0f ? 1.0f + p : 1.0f};
}

}  // namespace engine::audio
