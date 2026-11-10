#include "detail.h"

#include <domain/rocks/exposure.h>

#include <cmath>

namespace engine::rocks {
namespace {
using namespace detail;

bool bad(std::string& error, const char* field, const char* range) {
  error = std::string("engine.rocks.ExposureRule.") + field + " must be " + range;
  return false;
}
f64 tangent(f32 degrees) {
  const f64 a = wide(degrees) * (k_pi / 180.0);
  return det::sin(a) / det::cos(a);
}
}  // namespace

bool prepare_exposure(const ExposureRule& rule, ExposureThresholds& out, std::string& error) {
  out = {};
  if (!(rule.soil_slope_deg >= 5.0f && rule.soil_slope_deg <= 60.0f))
    return bad(error, "soil_slope_deg", "in [5, 60]");
  if (!(rule.rock_slope_deg > rule.soil_slope_deg && rule.rock_slope_deg <= 80.0f))
    return bad(error, "rock_slope_deg", "above soil_slope_deg and at most 80");
  if (!(rule.critical_curvature >= 0.002f && rule.critical_curvature <= 1.0f))
    return bad(error, "critical_curvature", "in [0.002, 1]");
  if (!(rule.scale_m >= 1.0f && rule.scale_m <= 32.0f)) return bad(error, "scale_m", "in [1, 32]");
  if (!(rule.min_resistance >= 0.0f && rule.min_resistance <= 1.0f))
    return bad(error, "min_resistance", "in [0, 1]");
  out.soil_slope = tangent(rule.soil_slope_deg);
  out.rock_slope = tangent(rule.rock_slope_deg);
  out.critical_curvature = wide(rule.critical_curvature);
  out.min_resistance = wide(rule.min_resistance);
  out.arm_mm = scene_gen::nearest_mm(wide(rule.scale_m));
  return true;
}

GroundShape ground_shape(i64 centre_mm, i64 west_mm, i64 east_mm, i64 north_mm, i64 south_mm,
                         i64 arm_mm) noexcept {
  GroundShape s;
  // Differences of integers first, so the shape is exact in the heights; one division each.
  const f64 arm = static_cast<f64>(arm_mm);
  s.slope_x = static_cast<f64>(east_mm - west_mm) / (2.0 * arm);
  s.slope_z = static_cast<f64>(south_mm - north_mm) / (2.0 * arm);
  s.slope = std::sqrt(s.slope_x * s.slope_x + s.slope_z * s.slope_z);
  // The five-point Laplacian in 1/m: millimetres over millimetres squared, times 1000.
  const i64 sum = west_mm + east_mm + north_mm + south_mm - 4 * centre_mm;
  s.convexity = -static_cast<f64>(sum) * 1000.0 / (arm * arm);
  return s;
}

f64 exposure_fraction(const ExposureThresholds& rule, const GroundShape& shape) noexcept {
  if (!(shape.slope >= rule.soil_slope)) return 0.0;
  const f64 steep = (shape.slope - rule.soil_slope) / (rule.rock_slope - rule.soil_slope);
  f64 curved = shape.convexity / rule.critical_curvature;
  curved = curved < -1.0 ? -1.0 : (curved > 1.0 ? 1.0 : curved);
  // A hollow's fill delays the exposure; the rock slope ends it (2 steep - 1 is 1 there).
  const f64 mantled = steep + curved;
  const f64 bare = 2.0 * steep - 1.0;
  return clamp01(mantled > bare ? mantled : bare);
}

f64 resistance_factor(const ExposureThresholds& rule, f64 resistance) noexcept {
  if (!(resistance >= rule.min_resistance)) return 0.0;
  const f64 span = 1.0 - rule.min_resistance;
  const f64 t = span > 0.0 ? clamp01((resistance - rule.min_resistance) / span) : 1.0;
  return 0.35 + 0.65 * t;
}

Exposure exposure_at(const ExposureThresholds& rule, const gfx::FormationParams& formation,
                     const scene_gen::Ground& ground, i64 x_mm, i64 z_mm) noexcept {
  const auto mm = [&](i64 x, i64 z) { return scene_gen::nearest_mm(ground.surface_mm(x, z)); };
  const i64 a = rule.arm_mm;
  const i64 centre = mm(x_mm, z_mm);
  Exposure e;
  e.shape = ground_shape(centre, mm(x_mm - a, z_mm), mm(x_mm + a, z_mm), mm(x_mm, z_mm - a),
                         mm(x_mm, z_mm + a), a);
  e.height_m = static_cast<f64>(centre) / 1000.0;
  const gfx::FormationFrame frame = gfx::formation_frame(formation);
  e.coordinate = gfx::formation_coordinate(frame, static_cast<f64>(x_mm) / 1000.0, e.height_m,
                                           static_cast<f64>(z_mm) / 1000.0)
                     .coordinate;
  const gfx::FormationBedSample bed = gfx::formation_bed(formation.beds, e.coordinate);
  e.bed = bed.index;
  e.resistance = bed.resistance;
  e.fraction = exposure_fraction(rule, e.shape);
  e.stand = e.fraction * resistance_factor(rule, wide(bed.resistance));
  e.exposed = e.stand > 0.0;
  return e;
}

}  // namespace engine::rocks
