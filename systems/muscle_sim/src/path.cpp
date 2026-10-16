#include <systems/muscle_sim/path.h>

#include <cmath>

namespace engine::muscle_sim {

double route_length(std::span<const Mat4> model, std::span<const RoutePoint> route) noexcept {
  double total = 0.0;
  double previous[3] = {0.0, 0.0, 0.0};
  for (usize p = 0; p < route.size(); ++p) {
    const Mat4& m = model[route[p].joint];
    const double o[3] = {static_cast<double>(route[p].offset.x),
                         static_cast<double>(route[p].offset.y),
                         static_cast<double>(route[p].offset.z)};
    double point[3] = {0.0, 0.0, 0.0};
    for (usize r = 0; r < 3; ++r) {
      point[r] = static_cast<double>(m.at(r, 0)) * o[0] + static_cast<double>(m.at(r, 1)) * o[1] +
                 static_cast<double>(m.at(r, 2)) * o[2] + static_cast<double>(m.at(r, 3));
    }
    if (p > 0) {
      const double dx = point[0] - previous[0];
      const double dy = point[1] - previous[1];
      const double dz = point[2] - previous[2];
      total += std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    previous[0] = point[0];
    previous[1] = point[1];
    previous[2] = point[2];
  }
  return total;
}

muscle::HermitePath next_path(PathHistory& history, double length) noexcept {
  const bool finite = std::isfinite(length);
  if (!history.valid) {
    history = PathHistory{length, 0.0, finite};
    return muscle::HermitePath{length, 0.0, length, 0.0};
  }
  const double rate = (length - history.length) / muscle::HermitePath::k_duration;
  const muscle::HermitePath path{history.length, history.rate, length, rate};
  history = PathHistory{length, rate, finite};
  return path;
}

}  // namespace engine::muscle_sim
