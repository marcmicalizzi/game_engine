#pragma once

#include "kit.h"

// G103's content-build curves. No runtime state and no change to the shared tree kit.
namespace engine::flora::detail {

inline u32 fern_rachis_rows(u32 pinnae) { return pinnae * 2u / 3u < 2u ? 2u : pinnae * 2u / 3u; }
inline u32 fern_pinnate_triangles(u32 pinnae) {
  return 2u * fern_rachis_rows(pinnae) - 1u + 4u * pinnae;
}

// Integrate a tangent whose turn grows quadratically: the stipe rises before the blade arches.
// A supported tip continues along the local ground instead of penetrating it. Sixteen fixed
// intervals define structure; tessellation budgets only resample it.
inline Axis ground_plant_axis(D3 base, D3 heading, f64 start_angle, f64 turn, f64 length) {
  constexpr u32 rows = 16;
  Axis a;
  a.length = length;
  D3 point = base;
  const D3 side = normalized(cross(heading, k_up));
  const f64 step = length / rows;
  for (u32 i = 0; i <= rows; ++i) {
    const f64 t = static_cast<f64>(i) / rows;
    const f64 angle = start_angle + turn * t * t;
    D3 tangent = heading * det::sin(angle) + k_up * det::cos(angle);
    if (point.y <= base.y && tangent.y < 0.0) tangent = heading;
    a.point.push_back(point);
    a.tangent.push_back(tangent);
    a.normal.push_back(side);
    a.arc.push_back(step * i);
    if (i == rows) break;
    const f64 middle = (static_cast<f64>(i) + 0.5) / rows;
    const f64 middle_angle = start_angle + turn * middle * middle;
    D3 delta = heading * det::sin(middle_angle) + k_up * det::cos(middle_angle);
    if (point.y + delta.y * step < base.y) {
      delta.y = (base.y - point.y) / step;
      delta = heading * std::sqrt(1.0 - delta.y * delta.y) + k_up * delta.y;
    }
    point = point + delta * step;
  }
  return a;
}

}  // namespace engine::flora::detail
