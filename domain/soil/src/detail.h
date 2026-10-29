#pragma once
#include <domain/soil/soil.h>

#include <algorithm>
#include <cmath>
namespace engine::soil::detail {
inline f32 smooth(f32 t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}
inline f32 taper(const SoilFile& f, f32 x) {
  return smooth((1.0f - std::abs(x) / (f.width_m * 0.5f)) * 5.0f);
}
inline f32 recess(const SoilFile& f) { return f.kind == Kind::Undercut ? f.undercut_m : 0.0f; }
inline f32 back(const SoilFile& f) { return recess(f) + 0.5f; }
inline f32 radius(const SoilFile& f) {
  return std::max(f.width_m * 0.5f + 1.0f, f.length_m + 4.0f);
}
}  // namespace engine::soil::detail
